// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompTitanPillar.generated.h"

class UStaticMeshComponent;

/**
 * CC (Control/Control) Loom ability: Titan Pillar.
 *
 * A massive solid cylinder that blocks movement, bullets (ECC_Visibility — the
 * channel hitscan + thrown-grenade impact traces use), and the projectile
 * channel. On spawn it runs a one-shot radial crush: every enemy of the caster
 * penetrating the pillar footprint is shoved out to the cylinder surface; anyone
 * with a wall behind them (no room to be pushed clear) takes instant lethal
 * damage through the standard HealthComponent chain. Then it stands as cover for
 * PillarLifetime seconds and despawns.
 *
 * Standalone AActor (not AProjectCompLoomZoneBase) — it's a physical blocker with
 * a one-shot effect, so the base's QueryOnly sphere + per-tick scan don't fit.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompTitanPillar : public AActor
{
	GENERATED_BODY()

public:
	AProjectCompTitanPillar();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Cylinder radius (cm). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Titan Pillar", meta = (ClampMin = "20.0"))
	float PillarRadius = 120.0f;

	/** Cylinder height (cm). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Titan Pillar", meta = (ClampMin = "50.0"))
	float PillarHeight = 400.0f;

	/** Players within this radius (cm) of the center are considered for the crush
	 *  sweep. Only those still penetrating the cylinder footprint get pushed/crushed. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Titan Pillar", meta = (ClampMin = "20.0"))
	float CrushRadius = 320.0f;

	/** Damage applied to an enemy who can't be pushed clear (pinned to a wall).
	 *  Large enough to be lethal through any armour. Routed through TakeDamage so
	 *  friendly-fire scaling still protects teammates (they're skipped anyway). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Titan Pillar", meta = (ClampMin = "1.0"))
	float LethalDamage = 100000.0f;

	/** Seconds the pillar stands before despawning. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Titan Pillar", meta = (ClampMin = "0.1"))
	float PillarLifetime = 6.0f;

	/** If true, the crush affects all teams; if false (default), only enemies of
	 *  the caster — teammates aren't shoved or crushed, the pillar is just cover. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Titan Pillar")
	bool bAffectAllTeams = false;

	/** Z lift (cm) added to the cast target so the cylinder base sits on the floor.
	 *  Default = PillarHeight/2 (centered pivot). Read off the CDO by the GameMode. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Titan Pillar", meta = (ClampMin = "0.0"))
	float SpawnZLift = 200.0f;

	/** Server: set the caster's team and run the one-shot crush sweep. */
	void Server_Initialize(ETeamID InCasterTeam);

protected:
	virtual void BeginPlay() override;

	/** Server: shove enemies out of the pillar footprint; lethal-damage anyone
	 *  pinned against a wall. */
	void Server_RunCrushSweep();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UStaticMeshComponent* PillarMesh;

	UPROPERTY(Replicated)
	ETeamID CasterTeam = ETeamID::Spectator;

	float DespawnTime = 0.0f;
};
