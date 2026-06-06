// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProjectCompLoomZoneBase.h"
#include "ProjectCompCameraSpike.generated.h"

/**
 * MM (Midrange/Midrange) Loom ability: Camera Spike.
 *
 * A static ping that keeps enemies inside its radius continuously revealed on the
 * LOOMING CASTER's minimap (single-viewer reveal — RevealedToPlayer = the caster
 * PlayerState, live location). Re-scans on a throttle and refreshes short-lived
 * reveal records so blips track live while an enemy stays in radius. Self-expires
 * when the caster respawns (no longer looming) or the round leaves RoundActive.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompCameraSpike : public AProjectCompLoomZoneBase
{
	GENERATED_BODY()

public:
	AProjectCompCameraSpike();

	/** Reveal radius (cm) around the spike. Mirrors ZoneRadius; kept as its own
	 *  knob for clarity in BP defaults. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Camera Spike", meta = (ClampMin = "100.0"))
	float PingRevealRadius = 700.0f;

	/** Re-scan frequency (Hz). 2 = twice a second — smooth enough for a minimap blip. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Camera Spike", meta = (ClampMin = "0.5"))
	float ScanHz = 2.0f;

protected:
	virtual void BeginPlay() override;
	virtual void Server_TickZone(float DeltaSeconds) override;

private:
	/** Time accumulated toward the next throttled scan. */
	float ScanAccumulator = 0.0f;
};
