// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProjectCompLoomZoneBase.h"
#include "ProjectCompTunnelVisionZone.generated.h"

/**
 * MA (Midrange/Aggro) Loom ability: Tunnel Vision.
 *
 * While alive, applies a timed peripheral-blackout debuff to enemies who enter
 * the zone (latched once each via the base AlreadyAffected set). The debuff is a
 * server-stamped end-time on the target's PlayerState (TunnelVisionEndTime,
 * owner-only); the affected client's HUD reads it and renders a hard black
 * vignette that leaves the central crosshair sightline clear but denies wide
 * swings / flank reads. Central aim intact, peripheral awareness gone.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompTunnelVisionZone : public AProjectCompLoomZoneBase
{
	GENERATED_BODY()

public:
	AProjectCompTunnelVisionZone();

	/** Seconds of peripheral blackout applied to each enemy on entry. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Tunnel Vision", meta = (ClampMin = "0.1"))
	float TunnelVisionDuration = 3.0f;

protected:
	virtual void Server_TickZone(float DeltaSeconds) override;
};
