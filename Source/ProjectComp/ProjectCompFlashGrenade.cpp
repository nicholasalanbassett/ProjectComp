// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompFlashGrenade.h"
#include "ProjectComp.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompGameMode.h"
#include "ProjectCompPlayerState.h"
#include "Camera/CameraComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

// Toggle for the FLASH-specific detonation viz (radius sphere, per-target rays,
// self-flash marker). Separate from the throwable base's projectile-path
// CVar (`pc.ThrowableGrenade.Debug`) so you can toggle detonation effects
// independently from the flight tracer.
static TAutoConsoleVariable<int32> CVarFlashGrenadeDebugDraw(
	TEXT("pc.FlashGrenade.Debug"),
	1,
	TEXT("Draw flash grenade detonation viz (radius sphere, per-target rays). 0=off, 1=on"),
	ECVF_Default);

AProjectCompFlashGrenade::AProjectCompFlashGrenade()
{
	// FLASH wants instant detonation on impact — no fuse window.
	// Trajectory is intentionally NOT overridden: FLASH uses the standardized
	// base throw (LMB gravity arc / RMB straight, both detonate on first
	// contact) so it flies identically to INFO / VULN / SMOKE.
	FuseAfterImpact = 0.0f;
}

void AProjectCompFlashGrenade::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority())
	{
		// Pre-allocate the LOS-ignore scratch buffer (used at detonation).
		// 16 covers a 5v5 plus the dome / grenade actor + headroom.
		IgnoreActorsScratch.Reserve(16);
	}
}

void AProjectCompFlashGrenade::OnEnterActivePhase()
{
	// FLASH has no persistence — the moment the projectile reaches the
	// detonation point, the scan + flash apply runs and the actor cleans up.
	Server_Detonate();
}

void AProjectCompFlashGrenade::HandleDetonationGameplay()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Radius scan with LOS + facing. Applies to ALL players in range —
	// teammates and enemies alike (per design: "throw it well or eat your
	// own flash"). The only filter is spectators, who aren't valid targets.
	const FVector BlastCenter = GetActorLocation();

	// Center viz (radius dome + center marker) goes to ALL machines via
	// multicast — clients on a non-host PC also see the dome this way. Per-
	// target rays below stay server-only (a tuning detail, expensive to
	// replicate for low payoff).
	Multicast_DrawDetonationViz(BlastCenter, FlashRadius);

#if !UE_BUILD_SHIPPING
	const bool bDrawDebug = CVarFlashGrenadeDebugDraw.GetValueOnGameThread() != 0;
#else
	constexpr bool bDrawDebug = false;
#endif

	IgnoreActorsScratch.Reset();
	IgnoreActorsScratch.Add(this);
	if (AProjectCompGameMode* GM = World->GetAuthGameMode<AProjectCompGameMode>())
	{
		for (const TWeakObjectPtr<AProjectCompCharacter>& WeakChar : GM->GetLiveCharacters())
		{
			if (AProjectCompCharacter* Char = WeakChar.Get())
			{
				IgnoreActorsScratch.Add(Char);
			}
		}
	}
	const TArray<AActor*>& IgnoreActors = IgnoreActorsScratch;

	const float RadiusSq = FlashRadius * FlashRadius;

	enum class EFlashOutcome : uint8 { SkipNoPawn, SkipTeam, SkipDead, OutOfRange, LOSBlocked, FullFlash, HalfFlash };

	int32 FullCount = 0, HalfCount = 0, RangeFailCount = 0, LOSFailCount = 0, SkipCount = 0;

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC) { continue; }

		AProjectCompPlayerState* PS = PC->GetPlayerState<AProjectCompPlayerState>();
		AProjectCompCharacter* Char = PS ? Cast<AProjectCompCharacter>(PC->GetPawn()) : nullptr;
		if (!Char || !Char->GetFirstPersonCameraComponent())
		{
			++SkipCount;
			continue;
		}

		const FVector CamLoc = Char->GetFirstPersonCameraComponent()->GetComponentLocation();
		EFlashOutcome Outcome = EFlashOutcome::SkipTeam;
		float FacingDot = 0.0f;
		float DistanceCm = 0.0f;

		if (!PS->IsAlive())
		{
			Outcome = EFlashOutcome::SkipDead;
		}
		else if (PS->GetTeamID() == ETeamID::Spectator)
		{
			// Spectators are skipped — they have no pawn to flash. NOTE: this
			// is the ONLY filter; teammates DO get flashed per design.
			Outcome = EFlashOutcome::SkipTeam;
		}
		else
		{
			DistanceCm = (BlastCenter - CamLoc).Size();
			if (DistanceCm * DistanceCm > RadiusSq)
			{
				Outcome = EFlashOutcome::OutOfRange;
			}
			else if (!HasClearLineOfSight(CamLoc, BlastCenter, IgnoreActors))
			{
				Outcome = EFlashOutcome::LOSBlocked;
			}
			else
			{
				const FVector ViewDir = PC->GetControlRotation().Vector();
				const FVector ToCenter = (BlastCenter - CamLoc).GetSafeNormal();
				FacingDot = FVector::DotProduct(ViewDir, ToCenter);
				Outcome = (FacingDot >= FacingDotThreshold)
					? EFlashOutcome::FullFlash
					: EFlashOutcome::HalfFlash;
			}
		}

		// Distance scaling: durations lerp from 1.0x at the detonation point
		// down to MinDistanceScalar at the radius edge.
		const float DistanceRatio = FlashRadius > 0.0f
			? FMath::Clamp(DistanceCm / FlashRadius, 0.0f, 1.0f)
			: 1.0f;
		const float DistanceScale = FMath::Lerp(1.0f, MinDistanceScalar, DistanceRatio);

		if (Outcome == EFlashOutcome::FullFlash)
		{
			const float FullWhite = FlashFullWhiteDuration * DistanceScale;
			const float Fade      = FlashFadeDuration      * DistanceScale;
			PS->Server_ApplyFlash(FullWhite, Fade);
			++FullCount;
			UE_LOG(LogProjectComp, Verbose,
				TEXT("[FlashGrenade] FULL pawn=%s dist=%.0fcm dot=%.2f scale=%.2f total=%.2fs"),
				*GetNameSafe(Char), DistanceCm, FacingDot, DistanceScale, FullWhite + Fade);
		}
		else if (Outcome == EFlashOutcome::HalfFlash)
		{
			const float FullWhite = HalfFlashFullWhiteDuration * DistanceScale;
			const float Fade      = HalfFlashFadeDuration      * DistanceScale;
			PS->Server_ApplyFlash(FullWhite, Fade);
			++HalfCount;
			UE_LOG(LogProjectComp, Verbose,
				TEXT("[FlashGrenade] HALF pawn=%s dist=%.0fcm dot=%.2f scale=%.2f total=%.2fs"),
				*GetNameSafe(Char), DistanceCm, FacingDot, DistanceScale, FullWhite + Fade);
		}
		else if (Outcome == EFlashOutcome::OutOfRange) { ++RangeFailCount; }
		else if (Outcome == EFlashOutcome::LOSBlocked) { ++LOSFailCount; }
		else { ++SkipCount; }

#if !UE_BUILD_SHIPPING
		if (bDrawDebug)
		{
			FColor RayColor = FColor::Blue;
			switch (Outcome)
			{
				case EFlashOutcome::FullFlash:  RayColor = FColor::Green;  break;
				case EFlashOutcome::HalfFlash:  RayColor = FColor(255, 220, 60); break;
				case EFlashOutcome::OutOfRange: RayColor = FColor(140, 140, 140); break;
				case EFlashOutcome::LOSBlocked: RayColor = FColor::Red;    break;
				default:                        RayColor = FColor::Blue;   break;
			}
			DrawDebugLine(World, BlastCenter, CamLoc, RayColor,
				false, DebugDrawDuration, 0, 2.0f);
			DrawDebugSphere(World, CamLoc, 14.0f, 8, RayColor,
				false, DebugDrawDuration, 0, 1.5f);
		}
#endif
	}

	UE_LOG(LogProjectComp, Log,
		TEXT("[FlashGrenade] scan @%s r=%.0f: full=%d half=%d outOfRange=%d LOSBlocked=%d skipped=%d"),
		*BlastCenter.ToCompactString(), FlashRadius,
		FullCount, HalfCount, RangeFailCount, LOSFailCount, SkipCount);
}

void AProjectCompFlashGrenade::Multicast_DrawDetonationViz_Implementation(FVector Center, float Radius)
{
#if !UE_BUILD_SHIPPING
	if (CVarFlashGrenadeDebugDraw.GetValueOnGameThread() == 0)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	DrawDebugSphere(World, Center, Radius, 32, FColor::Cyan,
		false, DebugDrawDuration, 0, 1.5f);
	DrawDebugSphere(World, Center, 18.0f, 10, FColor::Yellow,
		false, DebugDrawDuration, 0, 2.0f);
#endif
}

bool AProjectCompFlashGrenade::HasClearLineOfSight(const FVector& PawnCameraLoc,
                                                    const FVector& BlastCenter,
                                                    const TArray<AActor*>& IgnoreActors) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(FlashGrenadeLOS), /*bTraceComplex*/ false);
	Params.AddIgnoredActors(IgnoreActors);

	FHitResult Hit;
	// Geometry occlusion — walls block the flash.
	const bool bWallBlocked = World->LineTraceSingleByChannel(
		Hit, PawnCameraLoc, BlastCenter, ECC_Visibility, Params);
	if (bWallBlocked)
	{
		return false;
	}
	// Smoke occlusion — an active smoke volume between the player and the blast
	// also blocks the flash. Separate channel so this doesn't double-count walls
	// (smoke volumes ignore ECC_Visibility) and bullets remain unaffected.
	const bool bSmokeBlocked = World->LineTraceSingleByChannel(
		Hit, PawnCameraLoc, BlastCenter, ECC_VisionBlock, Params);
	return !bSmokeBlocked;
}
