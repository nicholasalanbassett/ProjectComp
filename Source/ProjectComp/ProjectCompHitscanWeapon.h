// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProjectCompHitscanWeapon.generated.h"

class AProjectCompCharacter;

/**
 * Hitscan weapon. Server-authoritative.
 *
 * The character calls SetTriggerHeld(true/false) via an RPC; the weapon's
 * server tick runs a tickrate-independent catch-up fire loop while the
 * trigger is held. Each fire:
 *   1. Traces from the owner's aim origin along the owner's aim direction.
 *   2. Applies damage via UE's standard TakeDamage chain.
 *   3. Multicasts a cosmetic event so all clients render muzzle flash / tracer.
 *
 * Tunables (EditDefaultsOnly) cover RPM, damage, range, head multiplier.
 * No prediction, no client-side fire effects yet — those land with lag comp
 * in a later pass.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompHitscanWeapon : public AActor
{
	GENERATED_BODY()

public:
	AProjectCompHitscanWeapon();

	virtual void Tick(float DeltaTime) override;

	// ---- Tunables ---------------------------------------------------------

	/** Seconds between shots. 0.1 = 600 RPM. Lower = faster fire. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon", meta = (ClampMin = "0.01"))
	float FireInterval = 0.1f;

	/** Base damage applied to non-head body bones. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon", meta = (ClampMin = "0.0"))
	float BodyDamage = 30.0f;

	/** Multiplier applied when the trace hits HeadBoneName. 4.0 = one-shot head at 100 HP with 30 body damage. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon", meta = (ClampMin = "1.0"))
	float HeadDamageMultiplier = 4.0f;

	/** Maximum trace range in cm. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon", meta = (ClampMin = "0.0"))
	float MaxRange = 5000.0f;

	/** Skeletal bone name treated as a head hit for damage multiplier. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon")
	FName HeadBoneName = TEXT("head");

	/**
	 * Maximum number of shots processed in a single tick. Caps burst-fire
	 * exploits at very high DeltaTimes (frame hitches or tampered clients).
	 * Gemini's "while loop" pattern is unbounded — we bound it.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon", meta = (ClampMin = "1"))
	int32 MaxShotsPerTick = 3;

	// ---- Server interface ------------------------------------------------

	/** Server: toggle the trigger state. Called by the owning character's RPC. */
	void Server_SetTriggerHeld(bool bHeld);

	/** Server: reset fire cooldown so the weapon is "ready" instantly on equip. */
	void ResetFireCooldown() { TimeSinceLastShot = FireInterval; }

protected:

	virtual void BeginPlay() override;

	/** True while the owner is holding the fire input. Server-side only. */
	bool bTriggerHeld = false;

	/** Accumulator for the tickrate-independent fire loop (seconds since last shot). */
	float TimeSinceLastShot = 0.0f;

	/** Server: fire a single shot. Trace + damage + multicast. */
	void FireOneShot();

	/** Cosmetic-only: muzzle flash, tracer, impact decal. Runs on all clients. */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_OnFired(FVector StartLoc, FVector EndLoc, bool bHit);

	/** Convenience accessor for the owning character. */
	AProjectCompCharacter* GetOwningCharacter() const;
};
