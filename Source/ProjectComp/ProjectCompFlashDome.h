// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompFlashDome.generated.h"

class UStaticMeshComponent;
class APawn;

/**
 * CA (Control/Aggro) ability: server-spawned dome that flashes enemy players
 * inside its radius who have unobstructed line of sight to the dome's center.
 *
 * Latching: once a player has been flashed by THIS dome instance, the dome
 * does not re-apply to them — their flash effect runs to completion driven
 * by their PlayerState's FlashEndTime, regardless of whether the dome is
 * still alive.
 *
 * Lifecycle: server-spawned in AProjectCompGameMode::Server_RequestAbilityCast.
 * Ticks until DomeLifetime elapses, then Destroy()s. Cosmetic mesh (set in BP
 * defaults) replicates so clients see where the dome was placed; per-player
 * flash state lives on AProjectCompPlayerState (owner-only replicated).
 */
UCLASS()
class PROJECTCOMP_API AProjectCompFlashDome : public AActor
{
	GENERATED_BODY()

public:
	AProjectCompFlashDome();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ---- Tunables ---------------------------------------------------------

	/** Sphere radius (cm) inside which an enemy with LOS to the center will be flashed. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Flash Dome", meta = (ClampMin = "100.0"))
	float DomeRadius = 800.0f;

	/** Seconds the dome exists in the world before despawning. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Flash Dome", meta = (ClampMin = "0.1"))
	float DomeLifetime = 4.0f;

	/** Seconds between dome spawn and the flash mechanism becoming active. The
	 *  dome exists during this window but does not apply flash — analogous to
	 *  a flashbang's fuse. Counts inside DomeLifetime, so total dome existence
	 *  is unchanged; bump DomeLifetime if you want a longer active phase. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Flash Dome", meta = (ClampMin = "0.0"))
	float WindupDuration = 0.5f;

	/** Seconds of full-white screen applied to each flashed player. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Flash Dome", meta = (ClampMin = "0.0"))
	float FlashFullWhiteDuration = 1.0f;

	/** Seconds of linear fade from full-white to clear after the full-white phase. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Flash Dome", meta = (ClampMin = "0.0"))
	float FlashFadeDuration = 1.5f;

	/** Cosine threshold for the facing check: dot(view, toCenter) >= this →
	 *  FULL flash; below → brief HALF flash. 0.5 ≈ 60° cone, generous read of
	 *  "looking at it". Raise toward 0.7 for stricter facing; lower toward 0.0
	 *  for "anything not behind you = full flash". */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Flash Dome", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float FacingDotThreshold = 0.5f;

	/** Full-white duration applied to a HALF-flashed (off-axis) target. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Flash Dome", meta = (ClampMin = "0.0"))
	float HalfFlashFullWhiteDuration = 0.05f;

	/** Fade duration applied to a HALF-flashed (off-axis) target. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Flash Dome", meta = (ClampMin = "0.0"))
	float HalfFlashFadeDuration = 0.05f;

	/** Z offset (cm) added to TargetLocation when the dome spawns. Lifts the
	 *  center above floor level so LOS traces from eye-height don't graze the
	 *  floor mesh. ~150 lands near standing eye level (capsule half-height 96
	 *  + camera Z 64 ≈ 160). The GameMode reads this off the dome class's CDO
	 *  before SpawnActor, so each BP variant can have its own value. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Flash Dome", meta = (ClampMin = "0.0"))
	float SpawnZLift = 150.0f;

	// ---- Server interface --------------------------------------------------

	/** Server: configure the dome with the caster's team. Targets are the OPPOSING team. */
	void Server_Initialize(ETeamID InCasterTeam);

protected:
	virtual void BeginPlay() override;

	/** Cosmetic dome mesh — populate in BP defaults. C++ leaves it empty so the
	 *  POC can run without an asset; level designers swap in the real visual. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UStaticMeshComponent* DomeMesh;

	/** Cosmetic center marker — small sphere visualizing where the flash will pop.
	 *  Override mesh / material / scale on BP_FlashDome to recolor or restyle per
	 *  ability variant. C++ default is the engine sphere at 0.3 world-scale (~15cm
	 *  radius) with the engine's default checker material. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UStaticMeshComponent* CenterBallMesh;

	/** Caster's team — used to filter targets to the opposing team only. Replicated
	 *  so clients can later condition local FX on whose dome this is. */
	UPROPERTY(Replicated)
	ETeamID CasterTeam = ETeamID::Spectator;

	/** Server-time the dome despawns. Set in BeginPlay from DomeLifetime. */
	float DespawnTime = 0.0f;

	/** Server-time the windup ends and flash application begins. Set in BeginPlay. */
	float WindupEndTime = 0.0f;

	/** Server-only latch — pawns already flashed by this dome instance. Weak ptrs
	 *  so a destroyed pawn (round end / level transition) doesn't keep entries alive. */
	TSet<TWeakObjectPtr<APawn>> AlreadyFlashed;

	/** Server: scan opposing-team living pawns inside the dome with clear LOS to
	 *  the center and apply flash to each (latched — only once per dome). */
	void Server_TickFlashApplication();

	/** True iff a Visibility trace from PawnCameraLoc to DomeCenter is unobstructed.
	 *  Walls block; character pawns (including the source) are added to the ignore
	 *  list so a teammate's body standing between you and the dome doesn't shield. */
	bool HasClearLineOfSight(const FVector& PawnCameraLoc,
	                         const FVector& DomeCenter,
	                         const TArray<AActor*>& IgnoreActors) const;
};
