// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompBarrier.generated.h"

class UStaticMeshComponent;

/**
 * Round-start spawn barrier with one-way visibility.
 *
 * During PreRound the barrier is solid (collision on) so neither team can leave
 * their spawn room. The owning team sees a transparent material — they get a
 * preview of the play area; the opposing team sees an opaque wall — they cannot
 * see into the enemy spawn room.
 *
 * During RoundActive the barrier is hidden and collision is off so the round
 * can proceed normally.
 *
 * Barriers are ROLE-PERMANENT — OwnerTeam does NOT flip at halftime. The
 * team currently with TeamID matching OwnerTeam sees the translucent material.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompBarrier : public AActor
{
	GENERATED_BODY()

public:
	AProjectCompBarrier();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void Tick(float DeltaSeconds) override;

	/** Server: raise (true) or drop (false) the barrier. Replicates to clients.
	 *  The interpretation of "active" varies by Kind — see EBarrierKind. The
	 *  GameMode computes the correct value per Kind on each phase transition. */
	void Server_SetActive(bool bNewActive);

	/** Which team this barrier belongs to — see-through for them. Set per-instance in editor. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_OwnerTeam, Category = "Team")
	ETeamID OwnerTeam = ETeamID::Spectator;

	/** Spatial role of this barrier — see EBarrierKind for the collision contracts.
	 *  Set per-instance in the level editor: SpawnRoomLock for attacker-side spawn
	 *  walls; SitePerimeter for defender-side pre-position rings around bomb sites. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Behavior")
	EBarrierKind Kind = EBarrierKind::AttackerSpawnLock;

	/** Material shown to non-owning team (and to spectators). Solid wall. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Materials")
	UMaterialInterface* OpaqueMaterial = nullptr;

	/** Material shown to the owning team. Translucent so they can see through. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Materials")
	UMaterialInterface* TransparentMaterial = nullptr;

protected:
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UStaticMeshComponent* Mesh;

	/** Replicated barrier state. true = wall up (collision + visible), false = wall down. */
	UPROPERTY(ReplicatedUsing = OnRep_IsActive)
	bool bIsActive = true;

	UFUNCTION()
	void OnRep_OwnerTeam();

	UFUNCTION()
	void OnRep_IsActive();

	/** Recompute material + visibility + collision for the local viewer. */
	void RefreshAppearance();

	/** Picks Transparent vs Opaque material based on whether the local viewer's
	 *  TeamID matches this barrier's OwnerTeam. Pulled out so each Kind branch
	 *  in RefreshAppearance can share the picker. */
	void ApplyOwnerTeamMaterial();

	/** Which team is the local player on? Returns Spectator if no local PC / PS. */
	ETeamID GetLocalViewerTeam() const;
};
