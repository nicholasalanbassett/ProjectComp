// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompGameState.generated.h"

/**
 * Match-wide replicated state.
 *
 * Strictly a state-replicator: contains zero game logic. All mutations come
 * from AProjectCompGameMode on the authoritative server. Clients receive
 * read-only updates via UE's replication system.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	AProjectCompGameState();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ---- Replicated read-only accessors -----------------------------------

	UFUNCTION(BlueprintPure, Category = "Round State")
	ERoundPhase GetCurrentPhase() const { return CurrentPhase; }

	UFUNCTION(BlueprintPure, Category = "Round State")
	int32 GetCurrentRound() const { return CurrentRound; }

	UFUNCTION(BlueprintPure, Category = "Round State")
	float GetPhaseTimeRemaining() const
	{
		// Server holds the authoritative float so per-tick decrements smaller than
		// the 0.1s quantum still accumulate. Clients only have the uint16 wire value.
		return HasAuthority() ? PhaseTimeRemainingServerFloat
		                      : static_cast<float>(PhaseTimeRemainingDs) * 0.1f;
	}

	UFUNCTION(BlueprintPure, Category = "Round State")
	FTeamLoadout GetTeamLoadout(ETeamID Team) const;

	/** Returns true if the given team's shared ability cooldown pool is full (>= 1.0). */
	UFUNCTION(BlueprintPure, Category = "Round State")
	bool IsTeamAbilityReady(ETeamID Team) const;

	/** True during the first SelectionWindowDuration seconds of PreRound — the buy/draft
	 *  UI is active and the server accepts Server_BuyGrenade / Server_ClaimLoomAbility.
	 *  Always false in other phases. */
	UFUNCTION(BlueprintPure, Category = "Round State")
	bool IsSelectionWindowOpen() const { return bSelectionWindowOpen; }

	// ---- Server-only mutators (called by GameMode) ------------------------

	void Server_SetPhase(ERoundPhase NewPhase);
	void Server_SetRound(int32 NewRound);
	void Server_SetPhaseTimeRemaining(float NewTime);
	void Server_SetTeamLoadout(ETeamID Team, const FTeamLoadout& NewLoadout);
	void Server_SetTeamCooldownPool(ETeamID Team, float NewPool);
	void Server_AddTeamScore(ETeamID Team, int32 Delta = 1);
	void Server_SetSelectionWindowOpen(bool bOpen);

	// ---- Multicast hooks --------------------------------------------------

	/**
	 * Fires on every client (and the server) when a Loom ability resolves
	 * successfully. Hook visual/audio FX here. Server validates; clients are
	 * notified-only.
	 */
	UFUNCTION(NetMulticast, Reliable)
	void Multicast_OnAbilityCast(EAbilityType Type, FVector TargetLocation, ETeamID Team);

	/**
	 * Fires on every client when a player is killed. Implementation pushes
	 * into PendingKillFeedEvents so the HUD widget can consume it next tick
	 * without needing a delegate binding.
	 */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_OnPlayerKilled(const FString& KillerName, const FString& VictimName, ETeamID KillerTeam, ETeamID VictimTeam);

	/**
	 * Fires on every client when an Info grenade completes its scan. Clients on
	 * the placer's team render the EnemyCount; clients whose pawn was within
	 * ScanRadius of ScanLocation at receive-time show a "you were scanned" tell.
	 * Reliable so the count + warning don't get dropped under jittery net.
	 */
	UFUNCTION(NetMulticast, Reliable)
	void Multicast_OnInfoScan(int32 EnemyCount, ETeamID PlacerTeam, FVector ScanLocation, float ScanRadius);

	/**
	 * Client-side queue consumed by UProjectCompHUDWidget::UpdateKillFeed each
	 * tick. Not replicated — populated by Multicast_OnPlayerKilled.
	 */
	TArray<FKillFeedData> PendingKillFeedEvents;

protected:

	UPROPERTY(ReplicatedUsing = OnRep_CurrentPhase, BlueprintReadOnly, Category = "Round State")
	ERoundPhase CurrentPhase = ERoundPhase::None;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Round State")
	int32 CurrentRound = 0;

	/** Server-driven countdown in deciseconds (0.1s units). Replicated as uint16
	 *  — 16 bits vs. 32 for a float, with plenty of range (up to ~109 minutes).
	 *  Mutators take float seconds; the BlueprintPure getter converts back to float.
	 *  Not BlueprintReadOnly because uint16 isn't a BP-exposable type — use
	 *  GetPhaseTimeRemaining() instead. */
	UPROPERTY(Replicated)
	uint16 PhaseTimeRemainingDs = 0;

	/** Server-only authoritative countdown in seconds. Holds the precise float so
	 *  per-tick subtractions smaller than the 0.1s quantum still accumulate. Not
	 *  replicated — clients read the quantized uint16 above. */
	float PhaseTimeRemainingServerFloat = 0.0f;

	/** Two-element array (size 2): index 0 = Attackers, index 1 = Defenders. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Round State")
	TArray<FTeamLoadout> Teams;

	/** Flips true at PreRound start, false SelectionWindowDuration seconds later.
	 *  Authoritative gate for all buy/claim RPCs — clients render UI based on this. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Round State")
	bool bSelectionWindowOpen = false;

	// ---- RepNotify handlers ----------------------------------------------

	/** Client-side hook for phase changes (e.g. play sound, swap UI). */
	UFUNCTION()
	void OnRep_CurrentPhase();

	/**
	 * Bounds-checked team accessor used internally.
	 * Returns nullptr if Team is Spectator or out of range.
	 */
	FTeamLoadout* GetTeamLoadoutMutable(ETeamID Team);
	const FTeamLoadout* GetTeamLoadoutConst(ETeamID Team) const;
};
