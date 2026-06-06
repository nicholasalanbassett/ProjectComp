// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProjectCompLoomZoneBase.h"
#include "ProjectCompRevealZone.generated.h"

/**
 * AM (Aggro/Midrange) Loom ability: Utility Reveal.
 *
 * On activation, runs a single server-side scan of enemies inside the cast zone
 * and publishes a team-wide reveal for each — their position plus their utility
 * loadout (EquippedGrenadeTypes) — into the GameState's EnemyReveals array for
 * RevealDuration seconds. The casting team's HUD lists the revealed kits and the
 * minimap shows blips. One-shot: the actor self-expires almost immediately; the
 * reveal lives on the GameState.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompRevealZone : public AProjectCompLoomZoneBase
{
	GENERATED_BODY()

public:
	AProjectCompRevealZone();

	/** Seconds the revealed enemies stay highlighted on the team's HUD/minimap. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Utility Reveal", meta = (ClampMin = "0.1"))
	float RevealDuration = 4.0f;

protected:
	virtual void Server_OnActivate() override;
};
