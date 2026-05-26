// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProjectCompGrenadeBase.h"
#include "ProjectCompVulnerableGrenade.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class UPrimitiveComponent;
class AProjectCompCharacter;

/**
 * Vulnerable grenade (Aggro Control). Map-placed circle util that applies a
 * 2x incoming-damage multiplier to enemies who are physically overlapping
 * its AOE sphere. Friendlies are unaffected.
 *
 * Breakable: the device mesh (root) is 1 HP — any bullet hit destroys the
 * grenade and cleans up the multiplier on still-overlapping characters.
 * Routing: weapon hits → AActor::TakeDamage (virtual dispatch) → our override.
 *
 * Lifecycle:
 *  - Spawn → AOE active immediately (no fuse)
 *  - Tracks enemy enter/exit via OnComponentBegin/EndOverlap → toggles each
 *    character's VulnerableZoneOverlapCount on AProjectCompCharacter.
 *  - On TakeDamage OR ZoneLifetime expiry → Server_Detonate cleans up.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompVulnerableGrenade : public AProjectCompGrenadeBase
{
	GENERATED_BODY()

public:
	AProjectCompVulnerableGrenade();

	virtual float TakeDamage(float DamageAmount,
	                         const struct FDamageEvent& DamageEvent,
	                         class AController* EventInstigator,
	                         AActor* DamageCauser) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
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

	/** Visible device — bullet-blocking on ECC_Visibility so weapon traces hit
	 *  it. 1-HP semantics implemented in the TakeDamage override. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UStaticMeshComponent* DeviceMesh;

	/** AOE sphere — overlap-only, no blocking, large radius. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	USphereComponent* ZoneSphere;

	/** AOE radius (cm). */
	UPROPERTY(EditDefaultsOnly, Category = "Vulnerable", meta = (ClampMin = "100.0"))
	float ZoneRadius = 600.0f;

	/** Server-side life cap. Zone auto-detonates after this many seconds even
	 *  if never shot. Designed to prevent permanent zones — tune in BP. */
	UPROPERTY(EditDefaultsOnly, Category = "Vulnerable", meta = (ClampMin = "1.0"))
	float ZoneLifetime = 30.0f;

private:
	/** Server-only: pawns currently inside the zone whose overlap counter we
	 *  incremented. Used at detonate-time to decrement (cleanup), so a
	 *  multiplier doesn't linger on a target that's still standing in a
	 *  zone whose physics body just got destroyed. */
	TSet<TWeakObjectPtr<AProjectCompCharacter>> TrackedOverlaps;

	/** Server-only lifetime timer. */
	FTimerHandle LifetimeTimer;
};
