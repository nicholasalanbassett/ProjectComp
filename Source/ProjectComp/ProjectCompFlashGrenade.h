// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProjectCompThrowableGrenadeBase.h"
#include "ProjectCompFlashGrenade.generated.h"

/**
 * Flash grenade (Aggro Midrange). Two modes selected at use-time via bAltFire:
 *
 *  - Primary (bAltFire=false, "LMB"): PhaseOneWall mode (inherited from base).
 *    Straight-line projectile that phases one wall and detonates on the next
 *    surface contact. Standard FLASH primary throw.
 *
 *  - Alt (bAltFire=true, "RMB"): GravityLob mode. Underarm toss — slower
 *    initial speed plus Earth-like gravity makes the projectile arc forward
 *    and land within a short distance. Detonates on first impact. No phase,
 *    no bounce. Self-flash is emergent (the lob lands near you, you're in
 *    the radius, you get flashed).
 *
 * Flash effect applies to ALL players (teammates and enemies) within radius
 * with LOS — no team filter. CS-style realism: a poorly-thrown flash can
 * blind your own team.
 *
 * Distance scaling: full/half-flash durations multiply by a linear lerp from
 * 1.0x at the detonation point down to MinDistanceScalar at FlashRadius edge.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompFlashGrenade : public AProjectCompThrowableGrenadeBase
{
	GENERATED_BODY()

public:
	AProjectCompFlashGrenade();

protected:
	virtual void BeginPlay() override;
	virtual void OnEnterActivePhase() override;
	virtual void HandleDetonationGameplay() override;

	// ---- Flash-effect tunables (mirror FlashDome's tuning model) ---------

	/** Radius (cm) within which enemies are eligible for flash on detonation. */
	UPROPERTY(EditDefaultsOnly, Category = "Flash|Effect", meta = (ClampMin = "100.0"))
	float FlashRadius = 3500.0f;

	/** Duration multiplier at the edge of FlashRadius. Linear lerp from 1.0 at
	 *  the detonation point down to this value at the radius edge. Set to 1.0
	 *  to disable distance scaling entirely. */
	UPROPERTY(EditDefaultsOnly, Category = "Flash|Effect", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinDistanceScalar = 0.15f;

	/** Full-white duration for a facing target at the detonation point. Scaled
	 *  toward MinDistanceScalar as distance approaches FlashRadius. */
	UPROPERTY(EditDefaultsOnly, Category = "Flash|Effect", meta = (ClampMin = "0.0"))
	float FlashFullWhiteDuration = 1.5f;

	/** Fade-out duration for a facing target at the detonation point. Scaled
	 *  toward MinDistanceScalar as distance approaches FlashRadius. */
	UPROPERTY(EditDefaultsOnly, Category = "Flash|Effect", meta = (ClampMin = "0.0"))
	float FlashFadeDuration = 2.0f;

	/** dot(view, toCenter) threshold — at-or-above = full flash; below = half. */
	UPROPERTY(EditDefaultsOnly, Category = "Flash|Effect", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float FacingDotThreshold = 0.5f;

	UPROPERTY(EditDefaultsOnly, Category = "Flash|Effect", meta = (ClampMin = "0.0"))
	float HalfFlashFullWhiteDuration = 0.05f;

	UPROPERTY(EditDefaultsOnly, Category = "Flash|Effect", meta = (ClampMin = "0.0"))
	float HalfFlashFadeDuration = 0.05f;

	/** Seconds the debug shapes persist in the world. Ignored in shipping. */
	UPROPERTY(EditDefaultsOnly, Category = "Flash|Debug", meta = (ClampMin = "0.5"))
	float DebugDrawDuration = 3.0f;

private:
	/** Server-side LOS-ignore scratch (reused across the detonation scan). */
	TArray<AActor*> IgnoreActorsScratch;

	bool HasClearLineOfSight(const FVector& PawnCameraLoc,
	                         const FVector& BlastCenter,
	                         const TArray<AActor*>& IgnoreActors) const;
};
