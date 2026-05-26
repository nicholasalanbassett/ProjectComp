// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompFlashGrenade.h"
#include "ProjectComp.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompGameMode.h"
#include "ProjectCompPlayerState.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

AProjectCompFlashGrenade::AProjectCompFlashGrenade()
{
	// Tick disabled by default; Server_InitializeExtra turns it on in projectile mode.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	ProjectileMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ProjectileMesh"));
	SetRootComponent(ProjectileMesh);
	// Per design: FLASH is unbreakable mid-flight. NoCollision means weapon
	// traces (ECC_Visibility) and player capsules pass straight through without
	// imparting force or registering hits. Server runs its OWN line trace each
	// tick for wall/player contact — that's where detonation triggers are read.
	ProjectileMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AProjectCompFlashGrenade::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority())
	{
		SpawnServerTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		// Mirror FlashDome's pre-reserved scratch buffer. 16 covers 5v5 + headroom.
		IgnoreActorsScratch.Reserve(16);
	}
}

void AProjectCompFlashGrenade::Server_InitializeExtra(bool bAltFire, const FVector& AimDirection)
{
	if (!HasAuthority())
	{
		return;
	}

	if (bAltFire)
	{
		// Self-flash mode: no projectile flight, no Tick — detonate immediately
		// and let HandleDetonationGameplay scope the effect to the instigator.
		bIsSelfFlashMode = true;
		UE_LOG(LogProjectComp, Log, TEXT("[FlashGrenade] %s entering self-flash mode."), *GetName());
		Server_Detonate();
		return;
	}

	// Projectile mode: stamp velocity from aim direction + enable Tick.
	bIsSelfFlashMode = false;
	FVector Dir = AimDirection;
	if (!Dir.Normalize())
	{
		Dir = GetActorForwardVector();
	}
	Velocity = Dir * ProjectileSpeed;
	SetActorRotation(Dir.Rotation());
	SetActorTickEnabled(true);

	UE_LOG(LogProjectComp, Log,
		TEXT("[FlashGrenade] %s entering projectile mode at %.0f cm/s."),
		*GetName(), ProjectileSpeed);
}

void AProjectCompFlashGrenade::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!HasAuthority() || IsDetonated())
	{
		return;
	}

	// Safety cap — never let a stray projectile linger forever.
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	if (Now - SpawnServerTime > MaxProjectileLifetime)
	{
		UE_LOG(LogProjectComp, Log, TEXT("[FlashGrenade] %s exceeded lifetime — force-detonating."), *GetName());
		Server_Detonate();
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FVector PrevLoc = GetActorLocation();
	const FVector NewLoc = PrevLoc + Velocity * DeltaSeconds;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(FlashProjectile), /*bTraceComplex*/ false, this);
	if (AActor* MyOwner = GetOwner())
	{
		Params.AddIgnoredActor(MyOwner);
		// Also ignore the instigator pawn so we don't immediately detonate on
		// our own body while the projectile is leaving the muzzle.
		if (APawn* InstigatorPawn = Cast<APawn>(MyOwner) ? Cast<APawn>(MyOwner) : nullptr)
		{
			Params.AddIgnoredActor(InstigatorPawn);
		}
	}
	if (APawn* InstigPawn = GetInstigator())
	{
		Params.AddIgnoredActor(InstigPawn);
	}

	FHitResult Hit;
	const bool bHit = World->LineTraceSingleByChannel(Hit, PrevLoc, NewLoc, ECC_Visibility, Params);

#if !UE_BUILD_SHIPPING
	// Tracer line for PIE testing. Stripped from shipping.
	DrawDebugLine(World, PrevLoc, NewLoc,
		bHasPhasedWall ? FColor(255, 200, 80) : FColor(255, 255, 80),
		/*bPersistent*/ false, /*Life*/ 1.5f, 0, /*Thickness*/ 2.0f);
#endif

	if (!bHit)
	{
		SetActorLocation(NewLoc, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
		return;
	}

	// Hit a character? Detonate immediately regardless of phase status — players
	// are not walls to phase through.
	const AActor* HitActor = Hit.GetActor();
	const bool bHitCharacter = HitActor && HitActor->IsA<AProjectCompCharacter>();
	if (bHitCharacter)
	{
		SetActorLocation(Hit.ImpactPoint, false, nullptr, ETeleportType::TeleportPhysics);
		UE_LOG(LogProjectComp, Log, TEXT("[FlashGrenade] %s hit character %s — detonating."),
			*GetName(), *GetNameSafe(HitActor));
		Server_Detonate();
		return;
	}

	// Hit world geometry. If we haven't phased yet, phase through; otherwise
	// detonate on this surface.
	if (!bHasPhasedWall)
	{
		const FVector WallEntry = Hit.ImpactPoint;
		const FVector Dir = Velocity.GetSafeNormal();
		const FVector FarSide = WallEntry + Dir * MaxWallThickness;

		// Backward trace from FarSide toward WallEntry. The first hit is the
		// BACK FACE of the wall we just entered — that's where we exit.
		FHitResult BackHit;
		FCollisionQueryParams BackParams(SCENE_QUERY_STAT(FlashProjectileBackTrace), false, this);
		BackParams.AddIgnoredActor(this);
		const bool bFoundBack = World->LineTraceSingleByChannel(
			BackHit, FarSide, WallEntry, ECC_Visibility, BackParams);

		if (!bFoundBack)
		{
			// Wall is thicker than MaxWallThickness, OR there's no back face
			// within range (rare — e.g. against an unbounded brush). Detonate
			// at entry rather than disappearing into the wall.
			SetActorLocation(WallEntry, false, nullptr, ETeleportType::TeleportPhysics);
			UE_LOG(LogProjectComp, Log,
				TEXT("[FlashGrenade] %s couldn't phase wall (thickness > %.0fcm) — detonating at entry."),
				*GetName(), MaxWallThickness);
			Server_Detonate();
			return;
		}

		// Teleport just past the back face so the next tick's trace starts
		// outside the wall. A 1cm offset is enough to avoid re-hitting the
		// same back surface from inside.
		const FVector ExitPoint = BackHit.ImpactPoint + Dir * 1.0f;
		SetActorLocation(ExitPoint, false, nullptr, ETeleportType::TeleportPhysics);
		bHasPhasedWall = true;

		UE_LOG(LogProjectComp, Verbose,
			TEXT("[FlashGrenade] %s phased wall: entry=%s exit=%s thickness=%.1fcm."),
			*GetName(),
			*WallEntry.ToCompactString(),
			*ExitPoint.ToCompactString(),
			(BackHit.ImpactPoint - WallEntry).Size());
		return;
	}

	// Already phased once — this is the second surface contact. Detonate.
	SetActorLocation(Hit.ImpactPoint, false, nullptr, ETeleportType::TeleportPhysics);
	UE_LOG(LogProjectComp, Log,
		TEXT("[FlashGrenade] %s post-phase contact at %s — detonating."),
		*GetName(), *Hit.ImpactPoint.ToCompactString());
	Server_Detonate();
}

void AProjectCompFlashGrenade::HandleDetonationGameplay()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// ---- Self-flash mode: instigator only --------------------------------
	if (bIsSelfFlashMode)
	{
		if (AProjectCompPlayerState* InstigatorPS = GetOwnerPlayerState())
		{
			InstigatorPS->Server_ApplyFlash(SelfFlashFullWhiteDuration, SelfFlashFadeDuration);
			UE_LOG(LogProjectComp, Log, TEXT("[FlashGrenade] self-flash applied to '%s'."),
				*InstigatorPS->GetPlayerName());
		}
		return;
	}

	// ---- Projectile mode: radius scan w/ LOS + facing --------------------
	const FVector BlastCenter = GetActorLocation();
	const ETeamID ThrowerTeam = GetOwnerTeam();
	if (ThrowerTeam == ETeamID::Spectator)
	{
		return;
	}

	// Build the LOS-ignore list against the live-character registry so a
	// teammate's body doesn't shield an enemy from the flash trace.
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

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC)
		{
			continue;
		}

		AProjectCompPlayerState* PS = PC->GetPlayerState<AProjectCompPlayerState>();
		if (!PS || !PS->IsAlive())
		{
			continue;
		}

		// Enemies only. Same-team and spectators skipped.
		const ETeamID PawnTeam = PS->GetTeamID();
		if (PawnTeam == ETeamID::Spectator || PawnTeam == ThrowerTeam)
		{
			continue;
		}

		AProjectCompCharacter* Char = Cast<AProjectCompCharacter>(PC->GetPawn());
		if (!Char || !Char->GetFirstPersonCameraComponent())
		{
			continue;
		}

		const FVector CamLoc = Char->GetFirstPersonCameraComponent()->GetComponentLocation();
		if ((BlastCenter - CamLoc).SizeSquared() > RadiusSq)
		{
			continue;
		}

		if (!HasClearLineOfSight(CamLoc, BlastCenter, IgnoreActors))
		{
			continue;
		}

		const FVector ViewDir = PC->GetControlRotation().Vector();
		const FVector ToCenter = (BlastCenter - CamLoc).GetSafeNormal();
		const float FacingDot = FVector::DotProduct(ViewDir, ToCenter);
		const bool bFacingCenter = FacingDot >= FacingDotThreshold;

		const float FullWhite = bFacingCenter ? FlashFullWhiteDuration : HalfFlashFullWhiteDuration;
		const float Fade      = bFacingCenter ? FlashFadeDuration      : HalfFlashFadeDuration;

		PS->Server_ApplyFlash(FullWhite, Fade);

		UE_LOG(LogProjectComp, Verbose,
			TEXT("[FlashGrenade] %s-flashed pawn=%s (team=%d) for %.2fs."),
			bFacingCenter ? TEXT("FULL") : TEXT("HALF"),
			*GetNameSafe(Char), static_cast<int32>(PawnTeam),
			FullWhite + Fade);
	}
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
	const bool bBlocked = World->LineTraceSingleByChannel(
		Hit, PawnCameraLoc, BlastCenter, ECC_Visibility, Params);
	return !bBlocked;
}
