// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompCameraSpike.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompGameMode.h"
#include "ProjectCompGameState.h"
#include "ProjectCompPlayerState.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

AProjectCompCameraSpike::AProjectCompCameraSpike()
{
	PingRevealRadius = 700.0f;
	// Long lifetime — the spike persists through the caster's looming window. It
	// self-destructs early on respawn / round-end (see Server_TickZone).
	ZoneLifetime = 120.0f;
	WindupDuration = 0.0f;
	SpawnZLift   = 100.0f;
	ScanHz       = 2.0f;
	DebugColor   = FColor(120, 255, 120);  // green
}

void AProjectCompCameraSpike::BeginPlay()
{
	// Keep the base sphere/debug radius in sync with the design knob.
	ZoneRadius = PingRevealRadius;
	Super::BeginPlay();
}

void AProjectCompCameraSpike::Server_TickZone(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	if (!World || CasterTeam == ETeamID::Spectator)
	{
		return;
	}

	AProjectCompGameState* GS = World->GetGameState<AProjectCompGameState>();
	if (!GS || GS->GetCurrentPhase() != ERoundPhase::RoundActive)
	{
		// Round window is over — clean up (reveals are also cleared at round start).
		Destroy();
		return;
	}

	// The reveal is for the casting (looming) player only. Owner is the casting PC
	// (set in the GameMode dispatch SpawnParams).
	const APlayerController* CasterPC = Cast<APlayerController>(GetOwner());
	AProjectCompPlayerState* CasterPS = CasterPC ? CasterPC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (!CasterPS)
	{
		Destroy();
		return;
	}
	if (CasterPS->IsAlive())
	{
		// Caster respawned — looming window over, spike retires.
		Destroy();
		return;
	}

	// Throttle the scan to ScanHz.
	const float ScanInterval = (ScanHz > 0.0f) ? (1.0f / ScanHz) : 0.5f;
	ScanAccumulator += DeltaSeconds;
	if (ScanAccumulator < ScanInterval)
	{
		return;
	}
	ScanAccumulator = 0.0f;

	AProjectCompGameMode* GM = CachedAuthGameMode.Get();
	if (!GM)
	{
		return;
	}

	const float Now = GS->GetServerWorldTimeSeconds();
	// Expiry slightly longer than the scan period so a blip doesn't flicker out
	// between refreshes; the next scan re-stamps it while the enemy stays in range.
	const float Expiry = Now + FMath::Max(1.0f, ScanInterval * 2.0f);
	const FVector Center = GetActorLocation();
	const float RadiusSq = PingRevealRadius * PingRevealRadius;

	for (const TWeakObjectPtr<AProjectCompCharacter>& WeakChar : GM->GetLiveCharacters())
	{
		AProjectCompCharacter* Char = WeakChar.Get();
		if (!Char)
		{
			continue;
		}
		const APlayerController* PC = Cast<APlayerController>(Char->GetController());
		AProjectCompPlayerState* PS = PC ? PC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
		if (!PS || !PS->IsAlive())
		{
			continue;
		}
		const ETeamID Team = PS->GetTeamID();
		if (Team == ETeamID::Spectator || Team == CasterTeam)
		{
			continue;
		}
		if ((Char->GetActorLocation() - Center).SizeSquared() > RadiusSq)
		{
			continue;
		}

		FEnemyRevealEntry Entry;
		Entry.Target           = PS;
		Entry.Location         = Char->GetActorLocation();
		Entry.RevealedToTeam   = ETeamID::Spectator;  // not a team reveal
		Entry.RevealedToPlayer = CasterPS;            // caster only
		Entry.ExpireServerTime = Expiry;
		Entry.bUseLiveLocation = true;                // track live on the minimap
		Entry.bShowLoadout     = false;
		GS->Server_AddEnemyReveal(Entry);
	}
}
