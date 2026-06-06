// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompHitscanWeapon.generated.h"

class AProjectCompCharacter;
class AProjectCompGameMode;
class USceneComponent;
class UStaticMeshComponent;
class USphereComponent;
class UPrimitiveComponent;

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

	/** Base damage applied to a Body-zone hit. Head/stomach/limb scale this via zone multipliers. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon", meta = (ClampMin = "0.0"))
	float BodyDamage = 30.0f;

	/** Multiplier applied for Head-zone hits. 4.0 = one-shot head at 100 HP with 30 body damage. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon", meta = (ClampMin = "1.0"))
	float HeadDamageMultiplier = 4.0f;

	/** Multiplier applied for Stomach-zone hits. CS/Valorant-style mid-tier (1.0-1.25). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon", meta = (ClampMin = "0.0"))
	float StomachDamageMultiplier = 1.1f;

	/** Multiplier applied for Limb-zone hits (arms / legs). Tac-FPS norm is 0.75-0.85. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon", meta = (ClampMin = "0.0"))
	float LimbDamageMultiplier = 0.75f;

	/** Maximum trace range in cm. Map diagonal is ~6800 cm; 10000 gives full coverage plus buffer. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon", meta = (ClampMin = "0.0"))
	float MaxRange = 10000.0f;

	/** Skeletal-bone → hit-zone map. Defaults populated in the constructor for the standard UE
	 *  mannequin rig; override in BP to fit a custom skeleton. Unmapped bones fall back to Body. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon")
	TMap<FName, EHitZone> BoneZoneMap;

	/**
	 * Maximum number of shots processed in a single tick. Caps burst-fire
	 * exploits at very high DeltaTimes (frame hitches or tampered clients).
	 * Gemini's "while loop" pattern is unbounded — we bound it.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon", meta = (ClampMin = "1"))
	int32 MaxShotsPerTick = 3;

	// ---- Spread -----------------------------------------------------------

	/** Extra spread (degrees) added when STANDING and running (above MoveSpeedThreshold). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float MovingSpreadAdd = 4.0f;

	/** Intermediate spread (degrees) added when STANDING and walking — speed is
	 *  above WalkSpeedThreshold but below MoveSpeedThreshold. Lets tac-walk
	 *  (Shift) incur a small cone without being punished like a full sprint. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float WalkingSpreadAdd = 1.5f;

	/** Ground speed (cm/s) above which a STANDING character enters the "walking"
	 *  spread tier. Below this = no cone (still or creeping). Above this AND
	 *  below MoveSpeedThreshold = WalkingSpreadAdd. Above MoveSpeedThreshold =
	 *  MovingSpreadAdd (full running penalty). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float WalkSpeedThreshold = 100.0f;

	/** Ground speed (cm/s) above which a STANDING character incurs the full
	 *  movement spread penalty. Below = perfectly accurate. Set above your
	 *  tactical-walk speed to allow shift-walk + still to stay accurate.
	 *  Default 350 sits between TacticalWalkSpeed (320) and MaxWalkSpeed (560)
	 *  so tac-walk → WalkingSpreadAdd and run → MovingSpreadAdd. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float MoveSpeedThreshold = 350.0f;

	/** Ground speed (cm/s) above which a CROUCHED character incurs the crouch-
	 *  movement spread penalty. Kept independent of MoveSpeedThreshold so you
	 *  can have "tac-walk accurate, crouch-walk slight cone, run full cone"
	 *  simultaneously. Set below your crouch-walk speed to make any crouch
	 *  motion incur a slight cone; set above to make crouch-walk also accurate. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float CrouchMoveSpeedThreshold = 100.0f;

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

	/** Max additional spread (degrees) layered on top of the movement cone after
	 *  SpreadRampShotsToMax consecutive shots. First shot of a burst is unaffected
	 *  (ramp = 0). Held spray ramps linearly to this value and clamps. Set to 0
	 *  to disable the burst-accuracy ramp entirely. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float SpreadRampMaxAdd = 2.0f;

	/** How many consecutive shots it takes for the spread ramp to reach max.
	 *  Lower = ramps up faster (less forgiving for held spray). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "1"))
	int32 SpreadRampShotsToMax = 8;

	/** Idle seconds before the spread ramp resets to bullet 1 (first-shot accuracy).
	 *  Keep close to RecoilPatternResetDelay so both spray-decay systems agree. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float SpreadRampResetDelay = 0.15f;

	/** Curve exponent applied to the ramp progress (Frac = BurstShotsFired / Max).
	 *  ramp = SpreadRampMaxAdd × pow(Frac, this).
	 *  1.0 = linear (every shot adds the same amount).
	 *  < 1.0 = ease-out (fast start, slow end — late shots add less).
	 *  > 1.0 = ease-in  (slow start, fast end — late shots add more).
	 *  0.5 = strong ease-out (sqrt); 0.75 = mild ease-out (default). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.1", ClampMax = "3.0"))
	float SpreadRampCurve = 0.75f;


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
	 * Bumped from 2.0 → 15.0 — old value left the camera drifting for many seconds
	 * after a single burst, which was a major nausea contributor in playtest.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float RecoilRecoveryRate = 15.0f;

	/** Seconds after each shot before intra-spray recovery begins (usually 0). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float RecoilRecoveryDelay = 0.0f;

	/** Seconds of idle before RecoilPatternIndex resets to 0. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float RecoilPatternResetDelay = 0.15f;

	/** Global multiplier on the vertical (pitch) component of every pattern[N]
	 *  entry. Drop to flatten climb; raise to make the gun harder to control. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0", ClampMax = "3.0"))
	float VerticalRecoilMult = 1.0f;

	/** Global multiplier on the horizontal (yaw) component of every pattern[N]
	 *  entry. Drop to flatten the left/right sweep; raise to amplify it. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0", ClampMax = "3.0"))
	float HorizontalRecoilMult = 1.0f;

	/** Extra pitch-only multiplier applied ONLY when the firer is crouched AND
	 *  below CrouchMoveSpeedThreshold (i.e. crouched + still). Slight reduction
	 *  to vertical climb; horizontal sweep is untouched. Set to 1.0 to disable. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float CrouchStillVerticalRecoilMult = 0.92f;

	// ---- Magazine + reload -----------------------------------------------

	/** Bullets per magazine. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Magazine", meta = (ClampMin = "1"))
	int32 MagazineSize = 30;

	/** Reserve magazines available beyond the one currently chambered.
	 *  Total mag count = MaxReserveMags + 1. Default 4 → 5 total mags. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Magazine", meta = (ClampMin = "0"))
	int32 MaxReserveMags = 4;

	/** Seconds to complete a reload from press to mag-refilled. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Magazine", meta = (ClampMin = "0.0"))
	float ReloadDuration = 2.2f;

	// ---- Lag compensation -------------------------------------------------

	/** Maximum rewind window the server will honor (seconds). Caps absurd RTT
	 *  values that a tampered client could otherwise use to rewind targets too
	 *  far. 250ms covers 200ms RTT + 50ms interp comfortably. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|LagComp", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float MaxRewindSeconds = 0.25f;

	/** Client interpolation delay (seconds) — clients render opponents this far
	 *  in the past. Subtracted from now alongside half-RTT to land on what the
	 *  firer's screen actually showed when they clicked. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|LagComp", meta = (ClampMin = "0.0", ClampMax = "0.2"))
	float ClientInterpolationDelay = 0.05f;

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

	/** Server: begin a reload if eligible (alive, not already reloading,
	 *  mag not already full, reserve > 0). Idempotent — silently ignored if
	 *  any precondition fails. Completion fires in Tick when server time
	 *  reaches ReloadEndTime. */
	void Server_StartReload();

	/** Server: refill the current mag + reserve to max and cancel any in-progress
	 *  reload. Called by the GameMode at PreRound start. */
	void Server_RefillAllAmmo();

	/** Server: abort any in-progress reload without refilling the mag. Called when
	 *  the player engages a plant/defuse interaction so the lock blocks the full
	 *  reload, not just the start of one. Mag stays at whatever was chambered. */
	void Server_CancelReload();

	/** Server: enable/disable the walk-over auto-pickup volume. Enabled when the
	 *  weapon is dropped on the ground, disabled when it's equipped. */
	void SetAvailableForPickup(bool bAvailable);

	// ---- Owner-replicated ammo accessors (HUD reads these) ---------------

	int32 GetCurrentMagAmmo() const { return CurrentMagAmmo; }
	int32 GetReserveAmmo()    const { return ReserveAmmo; }
	int32 GetMagazineSize()   const { return MagazineSize; }
	bool  IsReloading()       const { return bIsReloading; }
	float GetReloadEndTime()  const { return ReloadEndTime; }
	float GetReloadDuration() const { return ReloadDuration; }

	/** Owning client: zero out the local recoil pattern mirror at the start of
	 *  a new spray. Called by the character's DoFireStart handler. */
	void ResetLocalRecoilPattern()
	{
		LocalRecoilPatternIndex = 0;
		LocalLastShotClientTime = 0.0f;
	}

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:

	virtual void BeginPlay() override;

	/** Server: a live player walked over this dropped weapon — auto-equip it if
	 *  they have no weapon (auto-pickup only fills an empty slot; F can swap). */
	UFUNCTION()
	void OnPickupOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex, bool bFromSweep, const FHitResult& Sweep);

	/** True while the owner is holding the fire input. Server-side only. */
	bool bTriggerHeld = false;

	/** Accumulator for the tickrate-independent fire loop (seconds since last shot). */
	float TimeSinceLastShot = 0.0f;

	/** Seconds since any shot was fired; drives spread recovery and pattern reset. */
	float ShotIdleTime = 0.0f;

	/** Current index into RecoilPattern; advances each shot, resets after idle.
	 *  Server-side only. */
	int32 RecoilPatternIndex = 0;

	/** Consecutive shots fired in the current burst — drives the spread ramp.
	 *  Resets after SpreadRampResetDelay seconds of idle. Server-side only. */
	int32 BurstShotsFired = 0;

	// ---- Replicated to owner only — drives the HUD ammo readout ----------

	/** Bullets currently chambered. Decrements each shot. Refilled by reload
	 *  or PreRound. COND_OwnerOnly — opponents don't need to see your ammo. */
	UPROPERTY(Replicated)
	int32 CurrentMagAmmo = 30;

	/** Loose-bullet reserve pool. Reload pulls (MagazineSize - currentMag) from
	 *  this pool — i.e. only what's needed to top off — and partial mags returned
	 *  on reload land back here. Seeded from MaxReserveMags × MagazineSize on
	 *  BeginPlay and on PreRound refill. */
	UPROPERTY(Replicated)
	int32 ReserveAmmo = 120;

	/** True while a reload is in progress. Replicated so the HUD can show
	 *  "RELOADING" without an extra RPC. */
	UPROPERTY(Replicated)
	bool bIsReloading = false;

	/** Server-world-time at which the in-progress reload will complete.
	 *  Replicated so the HUD can show a smooth countdown / progress bar. */
	UPROPERTY(Replicated)
	float ReloadEndTime = 0.0f;

	/** Local mirror of the recoil pattern index for the owning client — advances
	 *  inside Multicast_OnFired_Implementation so the camera kick happens at the
	 *  same time as muzzle flash / tracer. Replaces the per-shot Client RPC.
	 *  Reset on trigger press and after RecoilPatternResetDelay of idle. */
	int32 LocalRecoilPatternIndex = 0;
	float LocalLastShotClientTime = 0.0f;

	/** Server-side running total of recoil applied this burst (degrees).
	 *  Used to offset the bullet trace so it matches what the client camera shows. */
	float ServerRecoilPitch = 0.0f;
	float ServerRecoilYaw   = 0.0f;

	/** Cached authority GameMode resolved once in BeginPlay so the per-shot fire
	 *  path can reach the LiveCharacters registry without a Cast<>() or a
	 *  GetAuthGameMode() call inside the hot loop. Weak ptr to survive late
	 *  teardown without dereferencing freed memory. Null on client builds. */
	TWeakObjectPtr<AProjectCompGameMode> CachedAuthGameMode;

	// ---- Visual (BP-authored) -------------------------------------------
	// Plain scene root: attaches to the owner's FP camera while equipped and is
	// left on the ground when dropped. WeaponMesh hangs off this so the BP can
	// freely set the mesh's relative location/rotation — a native ROOT component's
	// transform can't be edited in the BP, which is why the mesh isn't the root.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon|Visual")
	TObjectPtr<USceneComponent> WeaponRoot;
	// Mesh + material are assigned on the BP subclass (e.g. BP_Rifle). No collision.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon|Visual")
	TObjectPtr<UStaticMeshComponent> WeaponMesh;

	/** Walk-over auto-pickup volume. Enabled only while the weapon is on the ground
	 *  (dropped); disabled while equipped. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon|Pickup")
	TObjectPtr<USphereComponent> PickupSphere;

	/** Server: fire a single shot. Trace + damage + multicast. */
	void FireOneShot();

	/** Look up a bone's hit zone via BoneZoneMap; returns EHitZone::Body when unmapped. */
	EHitZone ClassifyHitZone(FName BoneName) const;

	/** Convert a hit zone to the configured damage multiplier. */
	float GetZoneMultiplier(EHitZone Zone) const;

	/** Returns movement/air/crouch spread (degrees) to add on top of the deterministic pattern. 0 when standing still. */
	float CalcMovementSpread(const AProjectCompCharacter* Char) const;

	/** Spread (degrees) contributed by the burst ramp. Reads BurstShotsFired and
	 *  ramps linearly from 0 at the first shot to SpreadRampMaxAdd once the
	 *  counter passes SpreadRampShotsToMax. Returns 0 if the ramp is disabled. */
	float CalcSpreadRampAdd() const;

	/** Cosmetic-only: muzzle flash, tracer, impact decal. Runs on all clients.
	 *
	 *  Bandwidth/anti-ESP design:
	 *   - StartLoc is FVector_NetQuantize (whole-cm precision, ~24 bits per axis vs. 96 for float).
	 *   - Direction is FVector_NetQuantizeNormal (16 bits per axis, perfect for a unit vector).
	 *   - DistanceCm is uint16; max range is 10000 cm so it fits with headroom.
	 *  Hit endpoint is NEVER sent — clients reconstruct it via a local trace from
	 *  StartLoc along Direction. A wallhacked client therefore cannot read enemy
	 *  positions from server packets; they have to do their own (much louder) trace.
	 *
	 *  bNetUseOwnerRelevancy is set on the weapon so this multicast only reaches
	 *  receivers who consider the firing character relevant.
	 */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_OnFired(FVector_NetQuantize StartLoc, FVector_NetQuantizeNormal Direction, uint16 DistanceCm, bool bHit);

	/** Cosmetic fire hook for the weapon BP — author muzzle flash + the shot SFX
	 *  here. Called on EVERY client from Multicast_OnFired. When
	 *  bMutedForLocalListener is true the LOCAL player is standing inside an enemy
	 *  AA (Acoustic Deafening) zone, so the BP must SKIP the shot sound — the
	 *  gunfire is acoustically deafened for this listener. C++ base is a no-op
	 *  (verbose log only): gunfire audio is authored on the BP today. */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Weapon|Cosmetic")
	void OnFiredCosmetic(FVector StartLoc, FVector Direction, float DistanceCm, bool bHit, bool bMutedForLocalListener);

	/** True if the LOCAL listener (this machine's player) is inside an enemy
	 *  Acoustic Deafening zone — i.e. gunfire SFX should be muted for them. */
	bool IsGunfireMutedForLocalListener() const;

	/** Convenience accessor for the owning character. */
	AProjectCompCharacter* GetOwningCharacter() const;
};
