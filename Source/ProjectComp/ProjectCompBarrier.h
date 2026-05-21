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
 * OwnerTeam can be swapped at halftime via Server_SwapOwnerTeam so the
 * see-through rule continues to work for whichever team currently spawns here.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompBarrier : public AActor
{
	GENERATED_BODY()

public:
	AProjectCompBarrier();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void Tick(float DeltaSeconds) override;

	/** Server: raise (true) or drop (false) the barrier. Replicates to clients. */
	void Server_SetActive(bool bNewActive);

	/** Server: flip OwnerTeam (Attackers ↔ Defenders). Called by the GameMode at halftime. */
	void Server_SwapOwnerTeam();

	/** Which team this barrier belongs to — see-through for them. Set per-instance in editor. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_OwnerTeam, Category = "Team")
	ETeamID OwnerTeam = ETeamID::Spectator;

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

	/** Which team is the local player on? Returns Spectator if no local PC / PS. */
	ETeamID GetLocalViewerTeam() const;
};
