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
 * Per-mode flight behavior. Primary (LMB) and Alt (RMB) each pick one.
 *
 *  - PhaseOneWall: straight-line, phases through ONE wall, detonates on the
 *    next surface contact after exiting. The "FLASH LMB" model.
 *  - BounceOnce:   straight-line, reflects ONCE off the first surface, then
 *    detonates on the next contact. The "INFO/VULN RMB" model.
 *  - GravityLob:   gravity-affected arc, detonates on first impact. No phase,
 *    no bounce. The "FLASH RMB underarm toss" model.
 */
UENUM(BlueprintType)
enum class EThrowableFireMode : uint8
{
	PhaseOneWall = 0,
	BounceOnce,
	GravityLob,
};

/**
 * Base class for any grenade that's thrown as a straight-line projectile with
 * two attack modes:
 *
 *  - Primary (bAltFire=false, "LMB"): phases through ONE wall, detonates on
 *    the next surface contact after exiting. Same as the original FLASH
 *    primary throw.
 *
 *  - Alt (bAltFire=true, "RMB"): reflects ONCE off the first surface struck
 *    (incident-angle reflection: R = D - 2*dot(D,N)*N), detonates on the next
 *    surface contact after that.
 *
 * Both modes share the projectile flight + lifetime cap + character-hit early-
 * detonate. Subclasses override OnEnterActivePhase to apply their gameplay
 * effect (flash radius scan, enemy-count scan, vulnerable-zone activation).
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

	/** LMB flight behavior. Defaults to PhaseOneWall (the FLASH/INFO/VULN
	 *  shared primary). Subclasses can override if a future design needs it. */
	UPROPERTY(EditDefaultsOnly, Category = "Throwable|Primary")
	EThrowableFireMode PrimaryFireMode = EThrowableFireMode::PhaseOneWall;

	UPROPERTY(EditDefaultsOnly, Category = "Throwable|Primary", meta = (ClampMin = "100.0"))
	float PrimaryProjectileSpeed = 3500.0f;

	/** Per-second downward acceleration in cm/s^2. 0 = no gravity (default
	 *  straight-line throws). ~980 = Earth-like (FLASH RMB underarm lob). */
	UPROPERTY(EditDefaultsOnly, Category = "Throwable|Primary", meta = (ClampMin = "0.0"))
	float PrimaryGravity = 0.0f;

	/** RMB flight behavior. Defaults to BounceOnce (the INFO/VULN model).
	 *  FLASH overrides to GravityLob in its constructor for the underarm toss. */
	UPROPERTY(EditDefaultsOnly, Category = "Throwable|Alt")
	EThrowableFireMode AltFireMode = EThrowableFireMode::BounceOnce;

	UPROPERTY(EditDefaultsOnly, Category = "Throwable|Alt", meta = (ClampMin = "100.0"))
	float AltProjectileSpeed = 3500.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Throwable|Alt", meta = (ClampMin = "0.0"))
	float AltGravity = 0.0f;

	/** Max wall thickness (cm) the projectile can phase through in PhaseOneWall
	 *  mode. Walls thicker than this detonate on first contact rather than
	 *  phasing. Ignored in BounceOnce / GravityLob modes. */
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
};
