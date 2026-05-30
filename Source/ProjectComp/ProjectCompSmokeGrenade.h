// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProjectCompThrowableGrenadeBase.h"
#include "ProjectCompSmokeGrenade.generated.h"

class USphereComponent;

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

	/** Spherical vision-block volume. NoCollision during flight; OnEnterActivePhase
	 *  enables it as a QueryOnly blocker on ECC_VisionBlock only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	USphereComponent* SmokeVolume;

	/** Radius (cm) of the smoke cloud / vision block. */
	UPROPERTY(EditDefaultsOnly, Category = "Smoke", meta = (ClampMin = "100.0"))
	float SmokeRadius = 450.0f;

	/** Seconds the smoke persists (vision blocked) before it dissipates. */
	UPROPERTY(EditDefaultsOnly, Category = "Smoke", meta = (ClampMin = "0.5"))
	float SmokeDuration = 15.0f;

	/** Replicated active flag — clients draw the cloud viz / play VFX while set. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Smoke")
	bool bIsSmokeActive = false;

private:
	/** Server handle for the dissipation (Active → Detonate) timer. */
	FTimerHandle DissipateTimer;
};
