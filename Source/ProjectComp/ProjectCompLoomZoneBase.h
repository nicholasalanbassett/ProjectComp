// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompLoomZoneBase.generated.h"

class UStaticMeshComponent;
class USphereComponent;
class APawn;
class AProjectCompGameMode;

/**
 * Shared base for spawned, replicated, ticking Loom-ability zone actors.
 *
 * Extracted from the original AProjectCompFlashDome scaffolding so every
 * zone-style Loom ability reuses the same proven shape:
 *   - a cosmetic mesh root (NoCollision) + a QueryOnly sphere volume,
 *   - replicated CasterTeam (targets are the OPPOSING team),
 *   - a windup fuse before the effect activates, a lifetime then Destroy(),
 *   - pre-allocated hot-path scratch (IgnoreActorsScratch — Reset() per tick,
 *     never Empty()) and an AlreadyAffected latch set,
 *   - a cached authority GameMode for per-tick LiveCharacters registry reads.
 *
 * Subclasses override Server_OnActivate() (one-shot at windup end) and/or
 * Server_TickZone(DeltaSeconds) (every tick after activation). The cosmetic
 * mesh/material is populated on the BP subclass; C++ runs asset-free with a
 * debug sphere in non-shipping builds.
 *
 * NOT used by AProjectCompTitanPillar (CC) — that's a physical blocker with a
 * one-shot crush, so a QueryOnly sphere + per-tick scan would be dead weight.
 */
UCLASS(Abstract)
class PROJECTCOMP_API AProjectCompLoomZoneBase : public AActor
{
	GENERATED_BODY()

public:
	AProjectCompLoomZoneBase();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ---- Tunables (public: the GameMode dispatch reads SpawnZLift off the CDO) ----

	/** Sphere radius (cm) of the effect zone. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Loom Zone", meta = (ClampMin = "50.0"))
	float ZoneRadius = 800.0f;

	/** Seconds the zone exists before despawning. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Loom Zone", meta = (ClampMin = "0.1"))
	float ZoneLifetime = 8.0f;

	/** Seconds between spawn and the effect becoming active (fuse). Counts inside
	 *  ZoneLifetime, so total existence is unchanged. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Loom Zone", meta = (ClampMin = "0.0"))
	float WindupDuration = 0.0f;

	/** Z offset (cm) added to the cast target when spawning, lifting the zone
	 *  center off the floor. Read off the CDO by the GameMode before SpawnActor. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Loom Zone", meta = (ClampMin = "0.0"))
	float SpawnZLift = 100.0f;

	/** Debug-sphere color in non-shipping builds. Per-subclass so overlapping
	 *  zones are visually distinguishable in PIE. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Loom Zone")
	FColor DebugColor = FColor(120, 200, 255);

	// ---- Server interface -------------------------------------------------

	/** Server: configure the zone with the caster's team. Targets are the OPPOSING team. */
	void Server_Initialize(ETeamID InCasterTeam);

	UFUNCTION(BlueprintPure, Category = "Loom Zone")
	ETeamID GetCasterTeam() const { return CasterTeam; }

	UFUNCTION(BlueprintPure, Category = "Loom Zone")
	float GetZoneRadius() const { return ZoneRadius; }

	/** True if WorldPoint is inside the zone sphere. */
	UFUNCTION(BlueprintPure, Category = "Loom Zone")
	bool IsPointInZone(const FVector& WorldPoint) const;

protected:
	virtual void BeginPlay() override;

	/** One-shot hook fired when the windup elapses. Default no-op. */
	virtual void Server_OnActivate() {}

	/** Per-tick hook fired every tick after activation, until despawn. Default no-op. */
	virtual void Server_TickZone(float DeltaSeconds) {}

	/** Rebuild IgnoreActorsScratch from the LiveCharacters registry + self.
	 *  Reset()s (not Empty()s) — the allocation is retained across ticks. */
	void BuildLiveCharacterIgnoreList();

	/** True iff a trace From→To is unobstructed. Walls block (ECC_Visibility);
	 *  optionally smoke also blocks (ECC_VisionBlock). IgnoreActorsScratch is
	 *  applied so pawn bodies don't shield. Call BuildLiveCharacterIgnoreList first. */
	bool HasClearLineOfSight(const FVector& From, const FVector& To, bool bAlsoBlockedBySmoke) const;

	/** Cosmetic mesh root — populate on the BP subclass. NoCollision. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UStaticMeshComponent* ZoneMesh;

	/** Query-only zone volume (overlaps ECC_Pawn). For BP/AI queries; gameplay
	 *  uses IsPointInZone / radius math directly. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	USphereComponent* ZoneSphere;

	/** Caster's team — replicated so clients can condition local FX on whose zone this is. */
	UPROPERTY(Replicated)
	ETeamID CasterTeam = ETeamID::Spectator;

	/** Server-time the zone despawns. Set in BeginPlay from ZoneLifetime. */
	float DespawnTime = 0.0f;

	/** Server-time the windup ends and the effect activates. Set in BeginPlay. */
	float WindupEndTime = 0.0f;

	/** True once Server_OnActivate has fired (latches the one-shot). */
	bool bActivated = false;

	/** Cached authority GameMode for per-tick registry reads. Null on clients. */
	TWeakObjectPtr<AProjectCompGameMode> CachedAuthGameMode;

	/** Hot-path LOS-ignore scratch — Reserve()d in BeginPlay, Reset() per use. */
	TArray<AActor*> IgnoreActorsScratch;

	/** Server-only latch — pawns already affected by this zone instance. */
	TSet<TWeakObjectPtr<APawn>> AlreadyAffected;
};
