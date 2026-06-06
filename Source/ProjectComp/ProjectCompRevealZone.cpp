// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompRevealZone.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompGameMode.h"
#include "ProjectCompGameState.h"
#include "ProjectCompPlayerState.h"
#include "ProjectComp.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

AProjectCompRevealZone::AProjectCompRevealZone()
{
	ZoneRadius   = 900.0f;
	// Short lifetime: the actor's only job is the one-shot scan in Server_OnActivate.
	// The reveal itself persists for RevealDuration on the GameState.
	ZoneLifetime = 0.5f;
	WindupDuration = 0.0f;
	SpawnZLift   = 100.0f;
	DebugColor   = FColor(255, 210, 60);  // gold
}

void AProjectCompRevealZone::Server_OnActivate()
{
	UWorld* World = GetWorld();
	if (!World || CasterTeam == ETeamID::Spectator)
	{
		return;
	}
	AProjectCompGameState* GS = World->GetGameState<AProjectCompGameState>();
	AProjectCompGameMode* GM = CachedAuthGameMode.Get();
	if (!GS || !GM)
	{
		return;
	}

	const float Now = GS->GetServerWorldTimeSeconds();
	const FVector Center = GetActorLocation();
	const float RadiusSq = ZoneRadius * ZoneRadius;
	int32 RevealCount = 0;

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
			continue;  // enemies of the caster only
		}
		const FVector Loc = Char->GetActorLocation();
		if ((Loc - Center).SizeSquared() > RadiusSq)
		{
			continue;
		}

		// Area scan: no LOS requirement (it reads the whole zone, walls included).
		FEnemyRevealEntry Entry;
		Entry.Target           = PS;
		Entry.Location         = Loc;            // snapshot — AM is a momentary reveal
		Entry.RevealedToTeam   = CasterTeam;     // whole team sees it
		Entry.RevealedToPlayer = nullptr;
		Entry.ExpireServerTime = Now + RevealDuration;
		Entry.bUseLiveLocation = false;
		Entry.bShowLoadout     = true;
		Entry.RevealedLoadout  = PS->GetEquippedGrenadeTypes();  // copied server-side (enemy kit is owner-only)
		GS->Server_AddEnemyReveal(Entry);
		++RevealCount;
	}

	UE_LOG(LogProjectComp, Log,
		TEXT("[Loom] Utility Reveal: revealed %d enemy/ies to team %d for %.1fs."),
		RevealCount, static_cast<int32>(CasterTeam), RevealDuration);
}
