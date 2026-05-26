// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProjectCompGrenadeBase.h"
#include "ProjectCompFlashGrenade.generated.h"

class UStaticMeshComponent;

/**
 * Flash grenade (Aggro Midrange). Two modes selected at use-time via bAltFire:
 *
 *  - Primary (bAltFire=false, "throw"): Straight-line projectile (no gravity)
 *    that phases through ONE wall, then detonates on the next surface it
 *    contacts. Unbreakable in flight (no collision response to weapon traces).
 *    On detonation: flashes enemies within FlashRadius who have LOS + facing
 *    the detonation point. Same full/half-flash split as the CA FlashDome
 *    ability — half flash if the target's view is off-axis from the popup.
 *
 *  - Alt (bAltFire=true, "self flash"): Detonates immediately at spawn
 *    location, applying flash ONLY to the instigator (and only to them — no
 *    radius scan, no facing check). Anti-peek self-sufficiency tool per design.
 *
 * Both modes share one cooldown slot on PlayerState (EGrenadeType::Flash).
 */
UCLASS()
class PROJECTCOMP_API AProjectCompFlashGrenade : public AProjectCompGrenadeBase
{
	GENERATED_BODY()

public:
	AProjectCompFlashGrenade();

	virtual void Tick(float DeltaSeconds) override;
	virtual void Server_InitializeExtra(bool bAltFire, const FVector& AimDirection) override;

protected:
	virtual void BeginPlay() override;

	virtual void HandleDetonationGameplay() override;

	/** Cosmetic mesh slot for the in-flight projectile. Set the mesh + material
	 *  on the BP subclass — C++ leaves it empty so the POC compiles asset-free. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UStaticMeshComponent* ProjectileMesh;

	// ---- Projectile tunables ---------------------------------------------

	/** Constant speed (cm/s). No gravity per design — straight line travel. */
	UPROPERTY(EditDefaultsOnly, Category = "Flash|Projectile", meta = (ClampMin = "100.0"))
	float ProjectileSpeed = 3500.0f;

	/** Max wall thickness (cm) the projectile can phase through. Walls thicker
	 *  than this detonate the grenade on first contact rather than phasing. */
	UPROPERTY(EditDefaultsOnly, Category = "Flash|Projectile", meta = (ClampMin = "1.0"))
	float MaxWallThickness = 200.0f;

	/** Safety cap: if the projectile never hits anything (e.g. flies into the
	 *  skybox), force-detonate at this many seconds post-spawn. */
	UPROPERTY(EditDefaultsOnly, Category = "Flash|Projectile", meta = (ClampMin = "0.5"))
	float MaxProjectileLifetime = 6.0f;

	// ---- Flash-effect tunables (mirror FlashDome's tuning model) ---------

	/** Radius (cm) within which enemies are eligible for flash on detonation. */
	UPROPERTY(EditDefaultsOnly, Category = "Flash|Effect", meta = (ClampMin = "100.0"))
	float FlashRadius = 1200.0f;

	/** Full-white duration for a facing target. */
	UPROPERTY(EditDefaultsOnly, Category = "Flash|Effect", meta = (ClampMin = "0.0"))
	float FlashFullWhiteDuration = 1.5f;

	/** Fade-out duration for a facing target. */
	UPROPERTY(EditDefaultsOnly, Category = "Flash|Effect", meta = (ClampMin = "0.0"))
	float FlashFadeDuration = 2.0f;

	/** dot(view, toCenter) threshold — at-or-above = full flash; below = half. */
	UPROPERTY(EditDefaultsOnly, Category = "Flash|Effect", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float FacingDotThreshold = 0.5f;

	UPROPERTY(EditDefaultsOnly, Category = "Flash|Effect", meta = (ClampMin = "0.0"))
	float HalfFlashFullWhiteDuration = 0.05f;

	UPROPERTY(EditDefaultsOnly, Category = "Flash|Effect", meta = (ClampMin = "0.0"))
	float HalfFlashFadeDuration = 0.05f;

	/** Self-flash mode: full-white duration applied to the instigator. Tuned
	 *  separately from the enemy-facing flash because the player KNOWS they
	 *  pressed it — shorter blind is forgiving but still impactful. */
	UPROPERTY(EditDefaultsOnly, Category = "Flash|Self", meta = (ClampMin = "0.0"))
	float SelfFlashFullWhiteDuration = 0.8f;

	UPROPERTY(EditDefaultsOnly, Category = "Flash|Self", meta = (ClampMin = "0.0"))
	float SelfFlashFadeDuration = 1.2f;

private:
	/** Server-only: per-tick projectile state. Server traces from PrevLocation
	 *  → ProposedLocation each tick, with phase-through-one-wall behavior. */
	FVector Velocity = FVector::ZeroVector;

	/** Server-only: true once the projectile has phased one wall. Subsequent
	 *  contacts detonate. */
	bool bHasPhasedWall = false;

	/** Server-only: spawn time used by the MaxProjectileLifetime safety cap. */
	float SpawnServerTime = 0.0f;

	/** Server-only: true if this instance is a self-flash (set in
	 *  Server_InitializeExtra). Drives the gameplay handler's target scope. */
	bool bIsSelfFlashMode = false;

	/** Server-side LOS-ignore scratch (reused across the detonation scan).
	 *  Mirrors the FlashDome pattern — Reserve()d once, Reset() between uses. */
	TArray<AActor*> IgnoreActorsScratch;

	/** Returns true iff a Visibility trace from PawnCameraLoc to BlastCenter is
	 *  unobstructed. Pawn body meshes block ECC_Visibility — pass them in
	 *  IgnoreActors so a teammate doesn't shield an enemy from the flash. */
	bool HasClearLineOfSight(const FVector& PawnCameraLoc,
	                         const FVector& BlastCenter,
	                         const TArray<AActor*>& IgnoreActors) const;
};
