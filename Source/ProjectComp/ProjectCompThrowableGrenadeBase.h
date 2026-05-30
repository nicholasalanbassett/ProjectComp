// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProjectCompGrenadeBase.h"
#include "ProjectCompThrowableGrenadeBase.generated.h"

class UStaticMeshComponent;

/**
 * Three-phase lifecycle for a throwable grenade.
 *  - Projectile: actor is moving via Tick, doing per-tick traces. Collision
 *    is fly-through (server detects impacts manually so client-side fly-through
 *    is purely cosmetic).
 *  - Fused: actor is stationary at the impact point, waiting FuseAfterImpact
 *    seconds before the gameplay effect fires. Tick is disabled.
 *  - Active: subclass's OnEnterActivePhase has run. What "active" means is
 *    subclass-specific (FLASH detonates immediately; VULN sits as a zone).
 */
UENUM(BlueprintType)
enum class EThrowablePhase : uint8
{
	Projectile = 0,
	Fused,
	Active,
};

/**
 * Per-mode flight behavior. The standardized throw (all four grenades share it)
 * is StrongThrow: a hard CS-style throw that arcs under gravity, bounces off
 * geometry losing energy each hit, settles, then activates (impact = "came to
 * rest"). Tune Restitution toward 0 for "lands and pops on first contact", or
 * up for a bouncier throw.
 *
 *  - StrongThrow:  gravity arc + multi-bounce-and-settle. The standardized
 *    default for every utility's tap-throw.
 *  - DirectImpact: detonates on the FIRST surface contact. No bounce. Retained
 *    for BP overrides (equivalent to StrongThrow with Restitution = 0).
 *  - PhaseOneWall: straight-line, phases through ONE wall, detonates on the
 *    next surface contact after exiting. Retained for BP overrides.
 *  - BounceOnce:   straight-line, reflects ONCE off the first surface, then
 *    detonates on the next contact. Retained for BP overrides.
 *
 * NOTE: the Alt (bAltFire) path no longer selects a different flight — both
 * tap-throws use StrongThrow. bAltFire is reserved for the future "global
 * drop" mechanic (hold the utility key → minimap overlay → place at a 2D point
 * resolved to the nearest floor), which is NOT a projectile throw at all.
 */
UENUM(BlueprintType)
enum class EThrowableFireMode : uint8
{
	PhaseOneWall = 0,
	BounceOnce,
	DirectImpact,
	StrongThrow,
};

/**
 * Base class for any grenade that's thrown as a projectile. ALL grenades share
 * one standardized throw (Flash / Info / Vulnerable / Smoke fly identically —
 * only their activation effect differs):
 *
 *  - Tap-throw (both LMB and the bAltFire path, for now): a strong CS-style
 *    throw — arcs under gravity, bounces off geometry with energy loss, settles,
 *    then "impacts" (activates) once it comes to rest. Tune via the StrongThrow
 *    knobs (Restitution / TangentialFriction / RestSpeed) below.
 *
 * The throw shares projectile flight + lifetime cap + character-hit early-
 * detonate. Subclasses override OnEnterActivePhase to apply their gameplay
 * effect (flash radius scan, enemy-count scan, vulnerable-zone activation,
 * smoke-volume bloom). They should NOT override the trajectory tunables — the
 * standardized base defaults below are the single source of truth.
 *
 * FUTURE: bAltFire will drive the "global drop" mechanic (hold the utility key
 * → transparent minimap overlay → place at a 2D point resolved to the nearest
 * floor, reduced effect). That path bypasses projectile flight entirely; until
 * it's built, bAltFire throws identically to the primary.
 *
 * FuseAfterImpact > 0 inserts a delay between impact and OnEnterActivePhase
 * — INFO/VULN use this for the design-mandated 1s warning window. FLASH leaves
 * it at 0 so the flash pops instantly on contact.
 *
 * Tunables are UPROPERTYs on this base class. Subclass BPs override them in
 * their defaults panel — overrides take precedence, so per-grenade tuning
 * (different speeds, fuses, lifetimes) works without touching C++.
 */
UCLASS(abstract)
class PROJECTCOMP_API AProjectCompThrowableGrenadeBase : public AProjectCompGrenadeBase
{
	GENERATED_BODY()

public:
	AProjectCompThrowableGrenadeBase();

	virtual void Tick(float DeltaSeconds) override;
	virtual void Server_InitializeExtra(bool bAltFire, const FVector& AimDirection) override;

	/** Server: global-drop entry. Skips projectile flight entirely — deploys the
	 *  grenade immediately at Location (as if it had landed there). Used by the
	 *  hold-key minimap placement path. Call INSTEAD of Server_InitializeExtra. */
	void Server_DeployInPlace(const FVector& Location);

	UFUNCTION(BlueprintPure, Category = "Throwable")
	EThrowablePhase GetPhase() const { return Phase; }

	UFUNCTION(BlueprintPure, Category = "Throwable")
	bool IsAltFire() const { return bIsAltFire; }

protected:
	virtual void BeginPlay() override;

	/** Subclass hook: called once when the grenade transitions into the Active
	 *  phase (either immediately at impact if FuseAfterImpact <= 0, or after
	 *  the fuse timer expires). Subclasses apply their gameplay effect here.
	 *  Examples:
	 *    - FLASH: scan + apply flash to enemies, then Server_Detonate.
	 *    - INFO:  scan + multicast count, then Server_Detonate.
	 *    - VULN:  enable zone overlap events; schedule Server_Detonate at
	 *             ZoneLifetime.
	 *  Base impl is no-op. */
	virtual void OnEnterActivePhase() {}

	// ---- Per-mode tunables (LMB / RMB each have their own) ---------------

	/** Primary (tap) flight behavior. Standardized: StrongThrow (CS-style arc +
	 *  bounce-and-settle). */
	UPROPERTY(EditDefaultsOnly, Category = "Throwable|Primary")
	EThrowableFireMode PrimaryFireMode = EThrowableFireMode::StrongThrow;

	/** Launch speed for the strong throw (cm/s). High = a hard overhand throw. */
	UPROPERTY(EditDefaultsOnly, Category = "Throwable|Primary", meta = (ClampMin = "100.0"))
	float PrimaryProjectileSpeed = 2200.0f;

	/** Per-second downward acceleration in cm/s^2. Earth-like (~980) gives the
	 *  CS-style throwing arc. */
	UPROPERTY(EditDefaultsOnly, Category = "Throwable|Primary", meta = (ClampMin = "0.0"))
	float PrimaryGravity = 980.0f;

	/** Alt flight behavior. Currently identical to primary (StrongThrow) — the
	 *  bAltFire path is reserved for the future global-drop and doesn't change
	 *  flight yet. */
	UPROPERTY(EditDefaultsOnly, Category = "Throwable|Alt")
	EThrowableFireMode AltFireMode = EThrowableFireMode::StrongThrow;

	UPROPERTY(EditDefaultsOnly, Category = "Throwable|Alt", meta = (ClampMin = "100.0"))
	float AltProjectileSpeed = 2200.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Throwable|Alt", meta = (ClampMin = "0.0"))
	float AltGravity = 980.0f;

	// ---- StrongThrow bounce-and-settle tunables --------------------------

	/** Bounciness on the surface-normal component of a bounce [0..1]. 0 = no
	 *  bounce (lands and pops on first contact); ~0.45 = CS-ish; higher = bouncy. */
	UPROPERTY(EditDefaultsOnly, Category = "Throwable|StrongThrow", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Restitution = 0.45f;

	/** Tangential energy lost per bounce [0..1] (surface friction/scuff). Higher
	 *  = the grenade "grips" and stops sliding sooner. */
	UPROPERTY(EditDefaultsOnly, Category = "Throwable|StrongThrow", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float TangentialFriction = 0.30f;

	/** Speed (cm/s) below which, on a bounce, the throw is considered settled and
	 *  impacts at the current point. Stops the projectile from micro-bouncing. */
	UPROPERTY(EditDefaultsOnly, Category = "Throwable|StrongThrow", meta = (ClampMin = "1.0"))
	float RestSpeed = 130.0f;

	/** Hard cap on bounces before a forced settle, regardless of speed — a safety
	 *  net against pathological geometry. */
	UPROPERTY(EditDefaultsOnly, Category = "Throwable|StrongThrow", meta = (ClampMin = "0"))
	int32 MaxBounces = 12;

	/** Max wall thickness (cm) the projectile can phase through in PhaseOneWall
	 *  mode. Walls thicker than this detonate on first contact rather than
	 *  phasing. Ignored in BounceOnce / DirectImpact modes. */
	UPROPERTY(EditDefaultsOnly, Category = "Throwable|Projectile", meta = (ClampMin = "1.0"))
	float MaxWallThickness = 200.0f;

	/** Safety cap: if the projectile never hits anything (flies into the sky),
	 *  force-impact at this many seconds post-throw. */
	UPROPERTY(EditDefaultsOnly, Category = "Throwable|Projectile", meta = (ClampMin = "0.5"))
	float MaxProjectileLifetime = 6.0f;

	/** Seconds between impact and OnEnterActivePhase. 0 = instant (FLASH).
	 *  >0 = delay during which the grenade sits at the impact point — design
	 *  intent: gives enemies a brief audio/visual tell to vacate. */
	UPROPERTY(EditDefaultsOnly, Category = "Throwable|Projectile", meta = (ClampMin = "0.0"))
	float FuseAfterImpact = 0.0f;

	/** Cosmetic mesh slot for the in-flight projectile. Set the mesh + material
	 *  on the BP subclass; C++ leaves it empty so the POC compiles asset-free. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UStaticMeshComponent* ProjectileMesh;

	/** Server-only: current phase. Drives Tick branch + impact gating. */
	EThrowablePhase Phase = EThrowablePhase::Projectile;

	/** Server-only: was this throw an alt-fire (RMB bounce)? Set in
	 *  Server_InitializeExtra and never mutated again. */
	bool bIsAltFire = false;

	/** Server-only: in PhaseOneWall mode, true once the projectile has phased
	 *  one wall and is on the far side. Subsequent contacts impact. */
	bool bHasPhasedWall = false;

	/** Server-only: in BounceOnce mode, true once the projectile has reflected
	 *  off one surface. Subsequent contacts impact rather than bounce. */
	bool bHasBounced = false;

	/** Server-only: bounce tally for StrongThrow, capped by MaxBounces. */
	int32 BounceCount = 0;

	/** Helper: pick the active fire mode based on bIsAltFire. */
	EThrowableFireMode GetActiveFireMode() const
	{
		return bIsAltFire ? AltFireMode : PrimaryFireMode;
	}

	/** Helper: pick the active gravity (cm/s^2 downward) based on bIsAltFire. */
	float GetActiveGravity() const
	{
		return bIsAltFire ? AltGravity : PrimaryGravity;
	}

	/** Server-only: spawn time, used by MaxProjectileLifetime safety cap. */
	float SpawnServerTime = 0.0f;

	/** Server-only: current motion vector (direction * speed). */
	FVector Velocity = FVector::ZeroVector;

	/** Server-only: handle for the fuse timer (impact → Active transition). */
	FTimerHandle FuseTimer;

	/** Server: called when the projectile lands at a final impact point.
	 *  Stamps the actor's location, then transitions to Fused (or directly to
	 *  Active if FuseAfterImpact <= 0). */
	void HandleImpact(const FVector& ImpactLocation);

	/** Server: starts the FuseAfterImpact timer. Set Phase=Fused, disable Tick. */
	void TransitionToFuse();

	/** Server: invokes the subclass's OnEnterActivePhase. Set Phase=Active. */
	void TransitionToActive();

	// ---- Multicast debug-draw events (every-machine viz) ----------------
	// Fired by the server at discrete moments (phase, bounce); each client
	// + the listen-server host runs the DrawDebug calls locally so all
	// peers see the same visualization.

	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_DrawPhaseEvent(FVector WallEntry, FVector ExitPoint);

	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_DrawBounceEvent(FVector ImpactAt, FVector ImpactNormal);

	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_DrawImpactMarker(FVector ImpactAt);

	/** Local-machine helper invoked from Tick on every peer. Draws a short
	 *  tracer line from the actor's last-seen position to its current
	 *  position — works on clients because the actor's location is
	 *  replicated via standard movement replication. */
	void DrawTracerSegment_Local();

	/** Per-machine state for the local tracer. NOT replicated. */
	FVector LastDrawLocation = FVector::ZeroVector;
	bool bHasLastDrawLocation = false;
};
