// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProjectCompThrowableGrenadeBase.h"
#include "ProjectCompVulnerableGrenade.generated.h"

class USphereComponent;
class UPrimitiveComponent;
class AProjectCompCharacter;

/**
 * Vulnerable grenade (Aggro Control). Thrown like FLASH:
 *  - LMB = wall-phase projectile
 *  - RMB = single-bounce projectile
 *
 * On impact, sits for FuseAfterImpact seconds, then activates a zone at the
 * impact point: enemies overlapping the AOE take 2x incoming damage. The zone
 * persists until ZoneLifetime expires or it's destroyed by a single bullet
 * (1-HP breakable). Friendlies are unaffected.
 *
 * Three lifecycle phases (inherited from ThrowableGrenadeBase):
 *  - Projectile: flying. Mesh has NoCollision (unbreakable in flight).
 *  - Fused: stationary at impact. Same NoCollision.
 *  - Active: zone enabled — mesh becomes bullet-blocking, overlap events drive
 *    the per-enemy multiplier counter on AProjectCompCharacter.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompVulnerableGrenade : public AProjectCompThrowableGrenadeBase
{
	GENERATED_BODY()

public:
	AProjectCompVulnerableGrenade();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void Tick(float DeltaSeconds) override;

	virtual float TakeDamage(float DamageAmount,
	                         const struct FDamageEvent& DamageEvent,
	                         class AController* EventInstigator,
	                         AActor* DamageCauser) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnEnterActivePhase() override;
	virtual void HandleDetonationGameplay() override;

	UFUNCTION()
	void OnZoneBeginOverlap(UPrimitiveComponent* OverlappedComponent,
	                        AActor* OtherActor,
	                        UPrimitiveComponent* OtherComp,
	                        int32 OtherBodyIndex,
	                        bool bFromSweep,
	                        const FHitResult& SweepResult);

	UFUNCTION()
	void OnZoneEndOverlap(UPrimitiveComponent* OverlappedComponent,
	                      AActor* OtherActor,
	                      UPrimitiveComponent* OtherComp,
	                      int32 OtherBodyIndex);

	/** AOE sphere — overlap-only, no blocking. Enabled in OnEnterActivePhase. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	USphereComponent* ZoneSphere;

	/** AOE radius (cm). */
	UPROPERTY(EditDefaultsOnly, Category = "Vulnerable", meta = (ClampMin = "100.0"))
	float ZoneRadius = 600.0f;

	/** Seconds the zone stays active after impact-fuse completes. Auto-detonates
	 *  at expiry; can also be cut short by being shot out (1-HP). */
	UPROPERTY(EditDefaultsOnly, Category = "Vulnerable", meta = (ClampMin = "1.0"))
	float ZoneLifetime = 30.0f;

	/** Replicated active flag — clients tick this to draw the neon coral zone
	 *  viz. Set true when the zone activates (post-fuse), false during the
	 *  detonate cleanup. Per design, the zone is visible to all players
	 *  (placer's team + enemies + spectators). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Vulnerable")
	bool bIsZoneActive = false;

private:
	/** Server-only: pawns currently inside the active zone whose overlap
	 *  counter we incremented. Used at detonate-time for cleanup. */
	TSet<TWeakObjectPtr<AProjectCompCharacter>> TrackedOverlaps;

	/** Server-only lifetime timer (started when entering Active phase). */
	FTimerHandle LifetimeTimer;
};
