// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProjectCompBomb.generated.h"

class UStaticMeshComponent;
class USphereComponent;
class AProjectCompCharacter;

/** Physical bomb lifecycle. */
UENUM(BlueprintType)
enum class EBombState : uint8
{
	/** On the ground (spawn point or a drop) — an attacker who touches it carries it. */
	Available,
	/** Attached to a carrying attacker. Only the carrier can plant. */
	Carried,
	/** Planted at a world location; counting down to detonation; defusable in radius. */
	Planted,
	/** Terminal: detonated (attackers win). */
	Detonated,
	/** Terminal: defused (defenders win). */
	Defused
};

/**
 * The single physical bomb. CS-style flow:
 *   - Round start: GameMode resets it to a level-placed BombSpawn (Available).
 *   - An alive attacker walking over it auto-picks it up (Carried, attached).
 *   - The CARRIER holding the interact key inside a bomb-site area plants it
 *     (PlantDuration) at their location (Planted).
 *   - A defender holding interact within DefuseRadius of the planted bomb defuses
 *     it (DefuseDuration).
 *   - A carrier who dies drops it on the ground (Available) where they fell.
 *
 * Server-authoritative: the plant/defuse FSM runs in Tick on the server; clients
 * get replicated State/Carrier/progress for attach + HUD.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompBomb : public AActor
{
	GENERATED_BODY()

public:
	AProjectCompBomb();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ---- Server API (GameMode / death cascade) --------------------------

	/** Reset to Available at WorldLocation (round start). */
	void Server_ResetToLocation(const FVector& WorldLocation);

	/** Carrier died (or otherwise must drop) — drop on the ground at DropLocation. */
	void Server_Drop(const FVector& DropLocation);

	// ---- Read-only state (GameMode / character / HUD) -------------------

	UFUNCTION(BlueprintPure, Category = "Bomb") EBombState GetState() const { return State; }
	UFUNCTION(BlueprintPure, Category = "Bomb") bool IsAvailable() const { return State == EBombState::Available; }
	UFUNCTION(BlueprintPure, Category = "Bomb") bool IsPlanted() const { return State == EBombState::Planted; }
	UFUNCTION(BlueprintPure, Category = "Bomb") bool IsTerminal() const { return State == EBombState::Detonated || State == EBombState::Defused; }
	UFUNCTION(BlueprintPure, Category = "Bomb") bool IsDetonated() const { return State == EBombState::Detonated; }
	UFUNCTION(BlueprintPure, Category = "Bomb") bool IsDefused() const { return State == EBombState::Defused; }
	UFUNCTION(BlueprintPure, Category = "Bomb") AProjectCompCharacter* GetCarrier() const { return Carrier; }
	UFUNCTION(BlueprintPure, Category = "Bomb") float GetDefuseRadius() const { return DefuseRadius; }
	UFUNCTION(BlueprintPure, Category = "Bomb") float GetDetonationEndTime() const { return DetonationEndTime; }
	UFUNCTION(BlueprintPure, Category = "Bomb") float GetPlantProgress() const { return PlantProgress; }
	UFUNCTION(BlueprintPure, Category = "Bomb") float GetDefuseProgress() const { return DefuseProgress; }

	/** Server: hand the bomb to a carrier. Called by the character's F-interact pickup. */
	void Server_Pickup(AProjectCompCharacter* NewCarrier);

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void OnPickupOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex, bool bFromSweep, const FHitResult& Sweep);

	// ---- Components ------------------------------------------------------

	/** Black cube — the bomb's visual + root. */
	UPROPERTY(VisibleAnywhere, Category = "Bomb")
	TObjectPtr<UStaticMeshComponent> BombMesh;

	/** Auto-pickup overlap sphere (attackers walking over the dropped bomb). */
	UPROPERTY(VisibleAnywhere, Category = "Bomb")
	TObjectPtr<USphereComponent> PickupSphere;

	// ---- Tunables -------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, Category = "Bomb", meta = (ClampMin = "0.1"))
	float PlantDuration = 4.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Bomb", meta = (ClampMin = "0.1"))
	float DefuseDuration = 6.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Bomb", meta = (ClampMin = "1.0"))
	float DetonationDuration = 35.0f;

	/** A defender must be within this distance (cm) of the planted bomb to defuse. */
	UPROPERTY(EditDefaultsOnly, Category = "Bomb", meta = (ClampMin = "50.0"))
	float DefuseRadius = 350.0f;

	// ---- Replicated state ----------------------------------------------

	UPROPERTY(ReplicatedUsing = OnRep_State, BlueprintReadOnly, Category = "Bomb")
	EBombState State = EBombState::Available;

	/** The carrying character while State==Carried (drives client-side attach). */
	UPROPERTY(ReplicatedUsing = OnRep_Carrier, BlueprintReadOnly, Category = "Bomb")
	TObjectPtr<AProjectCompCharacter> Carrier = nullptr;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Bomb")
	float DetonationEndTime = 0.0f;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Bomb")
	float PlantProgress = 0.0f;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Bomb")
	float DefuseProgress = 0.0f;

	UFUNCTION() void OnRep_State();
	UFUNCTION() void OnRep_Carrier();

private:
	// Server-only FSM helpers.
	void Server_TickPlant(float DeltaSeconds);
	void Server_TickDefuse(float DeltaSeconds);
	void Server_CompletePlant();
	void Server_SetState(EBombState NewState);
	void AttachToCarrier();
	void DetachAndPlace(const FVector& WorldLocation);
	void RefreshVisual();
};
