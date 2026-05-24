// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompBombSite.generated.h"

class UBoxComponent;
class UStaticMeshComponent;
class APawn;
class AProjectCompCharacter;
class AProjectCompPlayerState;

/**
 * Bomb site — server-authoritative plant/defuse/detonate state machine.
 *
 * Lifecycle within a round:
 *   NotPlanted → (alive attacker holds Use inside trigger for PlantDuration) → Planted
 *   Planted → (detonation timer ticks down) → Detonated
 *           OR (alive defender holds Use inside trigger for DefuseDuration) → Defused
 *
 * Detonated / Defused are terminal — bomb site stays in that state until the
 * round ends and the GameMode resets it. Plant/defuse progress is tracked
 * via a per-attempt timer; releasing Use or leaving the trigger cancels and
 * resets the progress.
 *
 * Designed for multi-site maps. Each instance has a Label ("A" / "B") and
 * a PointValue (1pt easier site, 2pt harder site — defaults to 1).
 */
UCLASS()
class PROJECTCOMP_API AProjectCompBombSite : public AActor
{
	GENERATED_BODY()

public:
	AProjectCompBombSite();

	virtual void Tick(float DeltaTime) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ---- Designer-tunable properties ----------------------------------

	/** Visible site label (e.g. "A" or "B"). Used by HUD ("BOMB PLANTED AT A"). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Site")
	FName Label = TEXT("A");

	/** Round-score awarded to attackers on successful detonation. 1 = easy plant (close to attackers), 2 = hard plant (far from attackers). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Site", meta = (ClampMin = "1"))
	int32 PointValue = 1;

	/**
	 * Round-score awarded to defenders on successful defuse. Inversely correlated
	 * to PointValue: a site that's easy for attackers to plant is far from defender
	 * spawn (hard defuse, 2pt); a site that's hard for attackers to plant is close
	 * to defender spawn (easy defuse, 1pt). Set per-instance in the editor.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Site", meta = (ClampMin = "1"))
	int32 DefusePointValue = 1;

	/** Seconds an attacker must hold Use inside the trigger to complete a plant.
	 *  EditAnywhere so a level designer can tune per-instance (A site easier than
	 *  B, etc.) without needing a BP subclass. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tuning", meta = (ClampMin = "0.1"))
	float PlantDuration = 4.0f;

	/** Seconds a defender must hold Use inside the trigger to complete a defuse. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tuning", meta = (ClampMin = "0.1"))
	float DefuseDuration = 6.0f;

	/** Seconds between a successful plant and detonation. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tuning", meta = (ClampMin = "0.1"))
	float DetonationDuration = 35.0f;

	// ---- Replicated state (read-only on clients) ----------------------

	/** True once a plant has completed. Replicated for HUD + GameMode round-end check. */
	UPROPERTY(ReplicatedUsing = OnRep_IsPlanted, BlueprintReadOnly, Category = "State")
	bool bIsPlanted = false;

	/** Terminal: bomb detonated. Replicated for HUD + round-end. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "State")
	bool bIsDetonated = false;

	/** Terminal: bomb defused. Replicated for HUD + round-end. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "State")
	bool bIsDefused = false;

	/** Server-time the bomb will detonate. Valid only when bIsPlanted && !bIsDetonated && !bIsDefused. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "State")
	float DetonationEndTime = 0.0f;

	/** [0..1] plant progress. Replicated for HUD progress bar. 0 when nobody is planting. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "State")
	float PlantProgress = 0.0f;

	/** [0..1] defuse progress. Replicated for HUD progress bar. 0 when nobody is defusing. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "State")
	float DefuseProgress = 0.0f;

	// ---- Server interface ---------------------------------------------

	/** Server: reset the bomb site to NotPlanted at round start. */
	void Server_ResetForNewRound();

	/** Convenience for the GameMode's round-end check. */
	bool IsTerminal() const { return bIsDetonated || bIsDefused; }

protected:
	virtual void BeginPlay() override;

	/** Trigger volume — must overlap with an alive player's capsule to count as "inside". */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UBoxComponent* Trigger;

	/** Optional decorative marker mesh — purely cosmetic, can be left null. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UStaticMeshComponent* MarkerMesh;

	UFUNCTION()
	void OnTriggerBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	UFUNCTION()
	void OnTriggerEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex);

	UFUNCTION()
	void OnRep_IsPlanted();

private:
	/** Server-only: pawns currently inside the trigger. Filtered to alive + valid team in tick. */
	UPROPERTY()
	TArray<TObjectPtr<APawn>> PawnsInside;

	/** Server: advance plant progress while a valid attacker is holding Use inside. */
	void TickPlanting(float DeltaTime);

	/** Server: advance defuse progress while a valid defender is holding Use inside. */
	void TickDefusing(float DeltaTime);

	/** Server: complete plant — flip state, schedule detonation. */
	void CompletePlant();

	/** Server: complete defuse — terminal state, GameMode will pick this up. */
	void CompleteDefuse();

	/** Server: detonate — terminal state, GameMode will pick this up. */
	void Detonate();

	/** Returns the first alive pawn in PawnsInside on TeamFilter that is holding Use. Null if none. */
	APawn* FindActiveInteractor(ETeamID TeamFilter) const;
};
