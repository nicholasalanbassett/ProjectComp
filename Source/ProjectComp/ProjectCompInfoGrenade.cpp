// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompInfoGrenade.h"
#include "ProjectComp.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompGameMode.h"
#include "ProjectCompGameState.h"
#include "ProjectCompPlayerState.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

// Subclass-specific debug viz (scan radius dome, count text, per-target rays).
// Separate from the throwable base's `pc.ThrowableGrenade.Debug` projectile-
// path CVar so detonation effects can be toggled independently.
static TAutoConsoleVariable<int32> CVarInfoGrenadeDebugDraw(
	TEXT("pc.InfoGrenade.Debug"),
	1,
	TEXT("Draw info grenade detonation viz (scan dome, count text, per-target rays). 0=off, 1=on"),
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

#if !UE_BUILD_SHIPPING
	const bool bDrawDebug = CVarInfoGrenadeDebugDraw.GetValueOnGameThread() != 0;
	if (bDrawDebug)
	{
		// Cyan wireframe sphere = scan area. Yellow marker at exact center.
		DrawDebugSphere(World, ScanCenter, ScanRadius, 32, FColor::Cyan,
			false, DebugDrawDuration, 0, 2.0f);
		DrawDebugSphere(World, ScanCenter, 18.0f, 10, FColor::Yellow,
			false, DebugDrawDuration, 0, 2.0f);
	}
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
			const float DistSq = (Char->GetActorLocation() - ScanCenter).SizeSquared();
			const bool bInRange = DistSq <= RadiusSq;

			if (bIsEnemy && bInRange)
			{
				++EnemyCount;
			}

#if !UE_BUILD_SHIPPING
			if (bDrawDebug)
			{
				// green = enemy counted; gray = enemy out of range;
				// blue = teammate / spectator (observation only).
				FColor RayColor = FColor::Blue;
				if (bIsEnemy)
				{
					RayColor = bInRange ? FColor::Green : FColor(140, 140, 140);
				}
				const FVector TargetLoc = Char->GetActorLocation();
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

#if !UE_BUILD_SHIPPING
	if (bDrawDebug)
	{
		const FString CountText = FString::Printf(TEXT("Enemies: %d"), EnemyCount);
		DrawDebugString(World, ScanCenter + FVector(0.0f, 0.0f, 200.0f),
			CountText, /*TestBaseActor*/ nullptr, FColor::Yellow,
			DebugDrawDuration, /*bDrawShadow*/ true, /*FontScale*/ 2.0f);
	}
#endif

	// Broadcast to all clients — placer's team sees the count, enemies in the
	// zone get the "you were scanned" tell.
	if (AProjectCompGameState* GS = World->GetGameState<AProjectCompGameState>())
	{
		GS->Multicast_OnInfoScan(EnemyCount, PlacerTeam, ScanCenter, ScanRadius);
	}

	// One-shot — clean up the grenade actor now that the scan has run.
	Server_Detonate();
}
