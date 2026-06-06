// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompTunnelVisionZone.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompGameMode.h"
#include "ProjectCompPlayerState.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

AProjectCompTunnelVisionZone::AProjectCompTunnelVisionZone()
{
	ZoneRadius   = 800.0f;
	ZoneLifetime = 5.0f;
	WindupDuration = 0.0f;
	SpawnZLift   = 100.0f;
	DebugColor   = FColor(20, 20, 20);  // near-black, matches the effect
}

void AProjectCompTunnelVisionZone::Server_TickZone(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	if (!World || CasterTeam == ETeamID::Spectator)
	{
		return;
	}

	const FVector Center = GetActorLocation();
	const float RadiusSq = ZoneRadius * ZoneRadius;

	AProjectCompGameMode* GM = CachedAuthGameMode.Get();
	if (!GM)
	{
		return;
	}

	for (const TWeakObjectPtr<AProjectCompCharacter>& WeakChar : GM->GetLiveCharacters())
	{
		AProjectCompCharacter* Char = WeakChar.Get();
		if (!Char)
		{
			continue;
		}

		// Enemies only, alive, not already affected by this zone instance.
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
		if (AlreadyAffected.Contains(Char))
		{
			continue;
		}
		if ((Char->GetActorLocation() - Center).SizeSquared() > RadiusSq)
		{
			continue;
		}

		PS->Server_ApplyTunnelVision(TunnelVisionDuration);
		AlreadyAffected.Add(Char);
	}
}
