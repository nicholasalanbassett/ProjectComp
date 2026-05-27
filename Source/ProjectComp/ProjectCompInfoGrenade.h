// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProjectCompThrowableGrenadeBase.h"
#include "ProjectCompInfoGrenade.generated.h"

/**
 * Info grenade (Midrange Control). Thrown like FLASH:
 *  - LMB = wall-phase projectile
 *  - RMB = single-bounce projectile
 *
 * On impact, sits for FuseAfterImpact seconds (default 1s — gives enemies a
 * brief audio/visual tell to move), then snapshots the count of enemy players
 * within ScanRadius of the impact point and broadcasts the result via
 * GameState::Multicast_OnInfoScan.
 *
 * One-shot detonate. Not toggleable, not repositionable.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompInfoGrenade : public AProjectCompThrowableGrenadeBase
{
	GENERATED_BODY()

public:
	AProjectCompInfoGrenade();

protected:
	virtual void OnEnterActivePhase() override;

	/** Radius (cm) inside which enemy players are counted at scan time. */
	UPROPERTY(EditDefaultsOnly, Category = "Info", meta = (ClampMin = "100.0"))
	float ScanRadius = 1000.0f;

	/** Seconds the debug shapes persist post-scan. Ignored in shipping. */
	UPROPERTY(EditDefaultsOnly, Category = "Info|Debug", meta = (ClampMin = "0.5"))
	float DebugDrawDuration = 4.0f;
};
