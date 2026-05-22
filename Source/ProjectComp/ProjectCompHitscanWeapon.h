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

	/** Maximum trace range in cm. Map diagonal is ~6800 cm; 10000 gives full coverage plus buffer. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon", meta = (ClampMin = "0.0"))
	float MaxRange = 10000.0f;

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

	// ---- Spread -----------------------------------------------------------

	/** Extra spread (degrees) added when moving at full run speed. Scales linearly with speed. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float MovingSpreadAdd = 4.0f;

	/** Ground speed (cm/s) below which the character is considered stationary for spread purposes. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float MoveSpeedThreshold = 100.0f;

	/**
	 * Horizontal spread as a fraction of vertical spread (0–1).
	 * 1.0 = circular cone. 0.4 = horizontal spread is 40% of vertical.
	 * Tune this to reduce side-to-side jitter while keeping vertical recoil spread.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float HorizontalSpreadMult = 0.4f;

	/** Spread multiplier when crouched (< 1.0 = tighter cone). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.01", ClampMax = "2.0"))
	float CrouchSpreadMult = 0.75f;

	/** Extra spread added when crouch-walking (replaces MovingSpreadAdd while crouched). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float CrouchMovingSpreadAdd = 1.0f;

	/** Extra spread (degrees) added unconditionally when airborne. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float AirSpreadAdd = 4.0f;


	// ---- Recoil -----------------------------------------------------------

	/**
	 * Per-shot spray pattern (degrees). X = pitch up (bullet goes high), Y = yaw right.
	 * Entry N defines exactly where bullet N lands relative to the crosshair.
	 * Pattern clamps on the last entry once the mag is exhausted.
	 * Resets to index 0 after RecoilPatternResetDelay seconds of not firing.
	 * The camera kicks by the same values so the player feels the recoil.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Recoil")
	TArray<FVector2D> RecoilPattern;

	/**
	 * Degrees per second at which the camera smoothly eases back during a spray.
	 * Recovery only runs while the trigger is held; camera stays put on release.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float RecoilRecoveryRate = 2.0f;

	/** Seconds after each shot before intra-spray recovery begins (usually 0). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float RecoilRecoveryDelay = 0.0f;

	/** Seconds of idle before RecoilPatternIndex resets to 0. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float RecoilPatternResetDelay = 0.15f;

	// ---- Skill modifier ----------------------------------------------------

	/**
	 * Multiplier on movement/air spread. 1.0 = full penalty. 0.0 = no penalty.
	 * Reduced by the "spray control" skill upgrade at runtime.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread")
	float TransitionSpreadMultiplier = 1.0f;

	// ---- Server interface ------------------------------------------------

	/** Server: toggle the trigger state. Called by the owning character's RPC. */
	void Server_SetTriggerHeld(bool bHeld);

	/** Server: reset fire cooldown so the weapon is "ready" instantly on equip. */
	void ResetFireCooldown() { TimeSinceLastShot = FireInterval; }

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:

	virtual void BeginPlay() override;

	/** True while the owner is holding the fire input. Server-side only. */
	bool bTriggerHeld = false;

	/** Accumulator for the tickrate-independent fire loop (seconds since last shot). */
	float TimeSinceLastShot = 0.0f;

	/** Seconds since any shot was fired; drives spread recovery and pattern reset. */
	float ShotIdleTime = 0.0f;

	/** Current index into RecoilPattern; advances each shot, resets after idle. */
	int32 RecoilPatternIndex = 0;

	/** Server-side running total of recoil applied this burst (degrees).
	 *  Used to offset the bullet trace so it matches what the client camera shows. */
	float ServerRecoilPitch = 0.0f;
	float ServerRecoilYaw   = 0.0f;

	/** Server: fire a single shot. Trace + damage + multicast. */
	void FireOneShot();

	/** Returns movement/air/crouch spread (degrees) to add on top of the deterministic pattern. 0 when standing still. */
	float CalcMovementSpread(const AProjectCompCharacter* Char) const;

	/** Cosmetic-only: muzzle flash, tracer, impact decal. Runs on all clients. */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_OnFired(FVector StartLoc, FVector EndLoc, bool bHit);

	/** Convenience accessor for the owning character. */
	AProjectCompCharacter* GetOwningCharacter() const;
};
