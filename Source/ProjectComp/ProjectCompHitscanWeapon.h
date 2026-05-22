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

	/** Minimum bullet spread half-angle (degrees) when standing still. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float BaseSpreadDegrees = 0.15f;

	/** Extra spread (degrees) added when moving at full run speed. Scales linearly with speed. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float MovingSpreadAdd = 3.5f;

	/** Ground speed (cm/s) below which the character is considered stationary for spread purposes. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float MoveSpeedThreshold = 50.0f;

	/** Spread multiplier when crouched (< 1.0 = tighter cone). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.01", ClampMax = "2.0"))
	float CrouchSpreadMult = 0.75f;

	/** Extra spread (degrees) added unconditionally when airborne. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float AirSpreadAdd = 4.0f;

	/** Spread (degrees) added to CurrentSpread per shot fired. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float SpreadPerShot = 0.5f;

	/** Maximum CurrentSpread allowed (caps per-shot accumulation). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float MaxSpread = 6.0f;

	/** Degrees per second that CurrentSpread decays while the trigger is released. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float SpreadRecoveryRate = 5.0f;

	/**
	 * Runtime spread accumulation (degrees). Replicated owner-only so the HUD
	 * can display a dynamic crosshair without an extra server query.
	 */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Weapon|Spread")
	float CurrentSpread = 0.0f;

	// ---- Recoil -----------------------------------------------------------

	/**
	 * Per-shot camera kick pattern (degrees). X = pitch up, Y = yaw right.
	 * Pattern clamps on the last entry once exhausted; resets to index 0 after
	 * RecoilPatternResetDelay seconds of not firing.
	 * Default is a Phantom-like moderate pattern; override per weapon in BP.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Recoil")
	TArray<FVector2D> RecoilPattern;

	/** Degrees per second at which the client camera auto-recovers after the last shot. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float RecoilRecoveryRate = 12.0f;

	/** Seconds after the last shot before camera recovery begins. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float RecoilRecoveryDelay = 0.1f;

	/** Seconds of idle before RecoilPatternIndex resets to 0 (allows a tight first shot again). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float RecoilPatternResetDelay = 0.4f;

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

	/** Server: fire a single shot. Trace + damage + multicast. */
	void FireOneShot();

	/** Compute the total spread half-angle (degrees) for one shot given owner's movement state. */
	float CalcTotalSpread(const AProjectCompCharacter* Char) const;

	/** Cosmetic-only: muzzle flash, tracer, impact decal. Runs on all clients. */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_OnFired(FVector StartLoc, FVector EndLoc, bool bHit);

	/** Convenience accessor for the owning character. */
	AProjectCompCharacter* GetOwningCharacter() const;
};
