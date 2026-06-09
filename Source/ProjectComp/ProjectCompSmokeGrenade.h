// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProjectCompThrowableGrenadeBase.h"
#include "ProjectCompSmokeGrenade.generated.h"

class USphereComponent;
class UStaticMeshComponent;

/**
 * Smoke grenade (vision-denial util). Thrown with the standardized base
 * trajectory (LMB gravity arc / RMB straight, both detonate on first contact),
 * then blooms a spherical smoke volume at the landing point for SmokeDuration.
 *
 * The bloom is a REAL line-of-sight block: SmokeVolume blocks the dedicated
 * ECC_VisionBlock trace channel (and ONLY that channel). Because bullets trace
 * on ECC_Visibility, gunfire passes straight through — smoke denies sight, not
 * shots. Sight-based systems that should respect smoke:
 *   - Flash grenade LOS (an enemy behind smoke is not flashed).
 *   - Info grenade scan (an enemy behind smoke is not counted).
 * Both add a VisionBlock trace against this volume; see those classes.
 *
 * Lifecycle (inherited three-phase model):
 *   Projectile → (FuseAfterImpact bloom delay) → Active: enable SmokeVolume +
 *   set bIsSmokeActive, schedule Server_Detonate at SmokeDuration. Detonate
 *   clears the volume and the actor cleans up.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompSmokeGrenade : public AProjectCompThrowableGrenadeBase
{
	GENERATED_BODY()

public:
	AProjectCompSmokeGrenade();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void Tick(float DeltaSeconds) override;

	UFUNCTION(BlueprintPure, Category = "Smoke")
	bool IsSmokeActive() const { return bIsSmokeActive; }

	UFUNCTION(BlueprintPure, Category = "Smoke")
	float GetSmokeRadius() const { return SmokeRadius; }

protected:
	virtual void BeginPlay() override;
	virtual void OnEnterActivePhase() override;
	virtual void HandleDetonationGameplay() override;

	/** SMOKE tracer/drop-line color: white. */
	virtual FColor GetTracerColor() const override { return FColor::White; }

	/** Spherical vision-block volume. NoCollision during flight; OnEnterActivePhase
	 *  enables it as a QueryOnly blocker on ECC_VisionBlock only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	USphereComponent* SmokeVolume;

	/** Cosmetic smoke dome — a sphere shown while the smoke is active. NoCollision
	 *  (the real vision block is SmokeVolume). Assign a TWO-SIDED material on the BP
	 *  so it's opaque from both outside AND inside — the default single-sided
	 *  material is see-through from within. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UStaticMeshComponent* SmokeDomeMesh;

	/** Radius (cm) of the smoke cloud / vision block. */
	UPROPERTY(EditDefaultsOnly, Category = "Smoke", meta = (ClampMin = "100.0"))
	float SmokeRadius = 380.0f;

	/** Seconds the smoke persists (vision blocked) before it dissipates. */
	UPROPERTY(EditDefaultsOnly, Category = "Smoke", meta = (ClampMin = "0.5"))
	float SmokeDuration = 15.0f;

	/** Replicated active flag — RepNotify shows/hides the dome on clients. */
	UPROPERTY(ReplicatedUsing = OnRep_SmokeActive, BlueprintReadOnly, Category = "Smoke")
	bool bIsSmokeActive = false;

	/** RepNotify: show/hide the smoke dome on clients when bIsSmokeActive changes. */
	UFUNCTION()
	void OnRep_SmokeActive();

	/** Sync the dome's scale (to SmokeRadius) + visibility (to bIsSmokeActive).
	 *  Server calls it on activate/detonate; clients via OnRep_SmokeActive. */
	void UpdateSmokeDomeVisual();

private:
	/** Server handle for the dissipation (Active → Detonate) timer. */
	FTimerHandle DissipateTimer;
};
