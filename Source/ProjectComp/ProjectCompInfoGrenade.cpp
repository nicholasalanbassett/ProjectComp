// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompInfoGrenade.h"
#include "ProjectComp.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompGameMode.h"
#include "ProjectCompGameState.h"
#include "ProjectCompPlayerState.h"
#include "ProjectCompMinimapEmp.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

// Subclass-specific debug viz (scan radius dome, count text, per-target rays).
// Separate from the throwable base's `pc.ThrowableGrenade.Debug` projectile-
// path CVar so detonation effects can be toggled independently.
static TAutoConsoleVariable<int32> CVarInfoGrenadeDebugDraw(
	TEXT("pc.InfoGrenade.Debug"),
	0,
	TEXT("Draw info grenade detonation viz (scan dome, count text, per-target rays). DEV-ONLY, off by default — too disruptive in playtests. 0=off, 1=on"),
	ECVF_Default);

AProjectCompInfoGrenade::AProjectCompInfoGrenade()
{
	// 1s impact-to-scan window per design: gives enemies a brief tell to
	// vacate. BP defaults can override per-instance for tuning.
	FuseAfterImpact = 1.0f;
}

void AProjectCompInfoGrenade::OnEnterActivePhase()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		Server_Detonate();
		return;
	}

	const FVector ScanCenter = GetActorLocation();
	const ETeamID PlacerTeam = GetOwnerTeam();

	// Center viz (dome + center marker + count text) replicates to every
	// machine via multicast. Count is finalized below so we delay the
	// multicast until after the scan loop.
#if !UE_BUILD_SHIPPING
	const bool bDrawDebug = CVarInfoGrenadeDebugDraw.GetValueOnGameThread() != 0;
#else
	constexpr bool bDrawDebug = false;
#endif

	if (PlacerTeam == ETeamID::Spectator)
	{
		Server_Detonate();
		return;
	}

	int32 EnemyCount = 0;
	const float RadiusSq = ScanRadius * ScanRadius;

	// LiveCharacters registry walk — cache-friendly, no TActorIterator alloc.
	if (AProjectCompGameMode* GM = World->GetAuthGameMode<AProjectCompGameMode>())
	{
		for (const TWeakObjectPtr<AProjectCompCharacter>& WeakChar : GM->GetLiveCharacters())
		{
			AProjectCompCharacter* Char = WeakChar.Get();
			if (!Char) { continue; }

			const APlayerController* PC = Cast<APlayerController>(Char->GetController());
			AProjectCompPlayerState* PS = PC ? PC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
			if (!PS || !PS->IsAlive()) { continue; }

			const ETeamID Team = PS->GetTeamID();
			const bool bIsEnemy = (Team != ETeamID::Spectator && Team != PlacerTeam);
			const FVector TargetLoc = Char->GetActorLocation();
			const float DistSq = (TargetLoc - ScanCenter).SizeSquared();
			const bool bInRange = DistSq <= RadiusSq;

			// Smoke occlusion: an active smoke volume between the scan origin and
			// the enemy hides them from the scan. Traces only ECC_VisionBlock, so
			// walls don't block the scan (unchanged behavior) — only smoke does.
			bool bSmokeOccluded = false;
			if (bIsEnemy && bInRange)
			{
				FCollisionQueryParams SmokeParams(SCENE_QUERY_STAT(InfoScanSmoke), /*bTraceComplex*/ false, this);
				FHitResult SmokeHit;
				bSmokeOccluded = World->LineTraceSingleByChannel(
					SmokeHit, ScanCenter, TargetLoc, ECC_VisionBlock, SmokeParams);
			}

			// MC (Minimap EMP): if the scanned enemy's OWN team has an active EMP
			// rect over their position, they don't register on this enemy's scan.
			bool bEmpHidden = false;
			if (bIsEnemy && bInRange && !bSmokeOccluded)
			{
				for (TActorIterator<AProjectCompMinimapEmp> EmpIt(World); EmpIt; ++EmpIt)
				{
					const AProjectCompMinimapEmp* Emp = *EmpIt;
					if (Emp && Emp->GetCasterTeam() == Team && Emp->IsPointInRect(TargetLoc))
					{
						bEmpHidden = true;
						break;
					}
				}
			}

			if (bIsEnemy && bInRange && !bSmokeOccluded && !bEmpHidden)
			{
				++EnemyCount;
			}

#if !UE_BUILD_SHIPPING
			if (bDrawDebug)
			{
				// green = enemy counted; magenta = enemy hidden by smoke;
				// gray = enemy out of range; blue = teammate / spectator.
				FColor RayColor = FColor::Blue;
				if (bIsEnemy)
				{
					RayColor = !bInRange ? FColor(140, 140, 140)
						: (bSmokeOccluded ? FColor(255, 60, 200) : FColor::Green);
				}
				DrawDebugLine(World, ScanCenter, TargetLoc, RayColor,
					false, DebugDrawDuration, 0, 2.0f);
				DrawDebugSphere(World, TargetLoc, 14.0f, 8, RayColor,
					false, DebugDrawDuration, 0, 1.5f);
			}
#endif
		}
	}

	UE_LOG(LogProjectComp, Log,
		TEXT("[InfoGrenade] %s scan complete: %d enemy/ies inside r=%.0fcm at %s."),
		*GetName(), EnemyCount, ScanRadius, *ScanCenter.ToCompactString());

	// Now that the scan has run and EnemyCount is finalized, multicast the
	// dome + count text to all machines so non-host peers see them too.
	Multicast_DrawScanViz(ScanCenter, ScanRadius, EnemyCount);

	// Broadcast to all clients — placer's team sees the count, enemies in the
	// zone get the "you were scanned" tell.
	if (AProjectCompGameState* GS = World->GetGameState<AProjectCompGameState>())
	{
		GS->Multicast_OnInfoScan(EnemyCount, PlacerTeam, ScanCenter, ScanRadius);
	}

	// One-shot — clean up the grenade actor now that the scan has run.
	Server_Detonate();
}

void AProjectCompInfoGrenade::Multicast_DrawScanViz_Implementation(FVector Center, float Radius, int32 EnemyCount)
{
#if !UE_BUILD_SHIPPING
	if (CVarInfoGrenadeDebugDraw.GetValueOnGameThread() == 0)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// Cyan wireframe scan sphere + yellow center marker.
	DrawDebugSphere(World, Center, Radius, 32, FColor::Cyan,
		false, DebugDrawDuration, 0, 2.0f);
	DrawDebugSphere(World, Center, 18.0f, 10, FColor::Yellow,
		false, DebugDrawDuration, 0, 2.0f);
	// World-space count text floating ~2m above the center.
	const FString CountText = FString::Printf(TEXT("Enemies: %d"), EnemyCount);
	DrawDebugString(World, Center + FVector(0.0f, 0.0f, 200.0f),
		CountText, /*TestBaseActor*/ nullptr, FColor::Yellow,
		DebugDrawDuration, /*bDrawShadow*/ true, /*FontScale*/ 2.0f);
#endif
}
