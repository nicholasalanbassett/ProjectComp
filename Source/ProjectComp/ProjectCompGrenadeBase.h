// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompGrenadeBase.generated.h"

class AProjectCompPlayerState;
class APlayerController;

/**
 * Base class for all server-spawned grenade actors (Flash / Info / Vulnerable / Smoke).
 *
 * Identity (type + owner team) is stamped via Server_Initialize immediately after
 * SpawnActor by the GrenadeComponent. Subclasses divergently implement throw vs
 * place vs persistent-zone behavior — this base only owns:
 *
 *   - Replicated identity tags (GrenadeType, OwnerTeam)
 *   - Detonation latch (bDetonated, RepNotify-driven cosmetic hook)
 *   - Server-side self-destroy timer
 *   - Two override hooks: HandleDetonationGameplay (server) + HandleDetonationCosmetic (every machine)
 *
 * Visibility: per design memory "grenade-system-and-loom-pivot", placed grenades
 * are visible to enemies by default — no relevancy filter here. A future stealth
 * variant can override IsNetRelevantFor.
 */
UCLASS(abstract)
class PROJECTCOMP_API AProjectCompGrenadeBase : public AActor
{
	GENERATED_BODY()

public:
	AProjectCompGrenadeBase();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ---- Identity accessors ----------------------------------------------

	UFUNCTION(BlueprintPure, Category = "Grenade")
	EGrenadeType GetGrenadeType() const { return GrenadeType; }

	UFUNCTION(BlueprintPure, Category = "Grenade")
	ETeamID GetOwnerTeam() const { return OwnerTeam; }

	UFUNCTION(BlueprintPure, Category = "Grenade")
	bool IsDetonated() const { return bDetonated; }

	/** Walks Owner → PlayerState. Returns null off-server, on detached actors,
	 *  or after the owning PC has been GC'd. */
	AProjectCompPlayerState* GetOwnerPlayerState() const;

	// ---- Server interface -------------------------------------------------

	/** Server: stamp identity tags. Called by the GrenadeComponent immediately
	 *  after SpawnActor. Idempotent — repeat calls just overwrite, which is
	 *  fine because there's only one legitimate caller. */
	void Server_Initialize(ETeamID InTeam, EGrenadeType InType);

	/** Server: subclass-specific configuration hook called by the GrenadeComponent
	 *  right after Server_Initialize. Subclasses interpret bAltFire/AimDirection
	 *  per their own semantics (e.g. FLASH: bAltFire=true → self-flash; INFO/
	 *  VULNERABLE/SOUND ignore bAltFire). Base no-op. */
	virtual void Server_InitializeExtra(bool bAltFire, const FVector& AimDirection) {}

	/** Server: latch detonation, run gameplay + cosmetic effect, schedule
	 *  Destroy() after CleanupDelay. Idempotent (subsequent calls are no-ops). */
	void Server_Detonate();

protected:
	/** Subclass override: apply the grenade's actual game-state effect on the
	 *  authoritative server (damage, scans, zone spawning, etc.). Base no-op. */
	virtual void HandleDetonationGameplay() {}

	/** Subclass override: spawn VFX/SFX. Runs on every machine — never mutate
	 *  authoritative state here. Base no-op. */
	virtual void HandleDetonationCosmetic() {}

	/** Server-side delay between Server_Detonate() and Destroy(). Lets trailing
	 *  particle/audio effects play to completion before the actor vanishes. */
	UPROPERTY(EditDefaultsOnly, Category = "Grenade", meta = (ClampMin = "0.0"))
	float CleanupDelay = 1.0f;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Grenade")
	EGrenadeType GrenadeType = EGrenadeType::None;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Grenade")
	ETeamID OwnerTeam = ETeamID::Spectator;

	UPROPERTY(ReplicatedUsing = OnRep_Detonated, BlueprintReadOnly, Category = "Grenade")
	bool bDetonated = false;

	UFUNCTION()
	void OnRep_Detonated();

private:
	/** Server-side handle for the post-detonate Destroy() timer. */
	FTimerHandle CleanupTimer;
};
