// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompGameState.generated.h"

class AProjectCompBomb;

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

	/** Pre-match lobby leader — the only player allowed to start the match. The
	 *  first joiner by default; transferable by the current leader. Null until set. */
	UFUNCTION(BlueprintPure, Category = "Lobby")
	APlayerState* GetLobbyLeader() const { return LobbyLeader; }

	/** Team that won the match — set when EndMatch fires; Spectator until then.
	 *  Drives the end-of-match results screen. */
	UFUNCTION(BlueprintPure, Category = "Round State")
	ETeamID GetMatchWinner() const { return MatchWinner; }

	/** The single physical bomb actor (spawned by the GameMode). Replicated so all
	 *  clients + the character can read carry/plant state for HUD + interact lock. */
	UFUNCTION(BlueprintPure, Category = "Bomb")
	AProjectCompBomb* GetBomb() const { return Bomb; }

	// ---- Enemy reveals (AM Utility Reveal + MM Camera Spike) --------------

	/** Read-only view of the active enemy-reveal records. Consumers (minimap / HUD)
	 *  filter by RevealedToTeam / RevealedToPlayer for the local viewer. */
	const TArray<FEnemyRevealEntry>& GetEnemyReveals() const { return EnemyReveals; }

	// ---- Server-only mutators (called by GameMode) ------------------------

	void Server_SetLobbyLeader(APlayerState* NewLeader);
	void Server_SetBomb(AProjectCompBomb* InBomb);

	void Server_SetPhase(ERoundPhase NewPhase);
	void Server_SetRound(int32 NewRound);
	void Server_SetPhaseTimeRemaining(float NewTime);
	void Server_SetTeamLoadout(ETeamID Team, const FTeamLoadout& NewLoadout);
	void Server_SetTeamCooldownPool(ETeamID Team, float NewPool);
	void Server_AddTeamScore(ETeamID Team, int32 Delta = 1);
	void Server_SetSelectionWindowOpen(bool bOpen);
	void Server_SetMatchWinner(ETeamID Team);

	// ---- Enemy-reveal mutators (called by the Loom ability actors) --------

	/** Server: add or refresh an enemy-reveal record. Upserts on (Target,
	 *  RevealedToTeam, RevealedToPlayer) so a continuously-refreshing producer
	 *  (MM Camera Spike) doesn't pile up duplicate entries for the same enemy. */
	void Server_AddEnemyReveal(const FEnemyRevealEntry& Entry);

	/** Server: drop every reveal whose ExpireServerTime has passed. Called each tick. */
	void Server_PurgeExpiredReveals();

	/** Server: clear all reveals (called at round start). */
	void Server_ClearEnemyRevealsForRound();

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

	/** Pre-match lobby leader (replicated so all clients gate the Start button +
	 *  the "make leader" right-click menu). Set by the GameMode. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Lobby")
	TObjectPtr<APlayerState> LobbyLeader = nullptr;

	/** The match's single bomb actor (spawned by the GameMode). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Bomb")
	TObjectPtr<AProjectCompBomb> Bomb = nullptr;

	/** Winning team of the just-ended match (Spectator until EndMatch). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Round State")
	ETeamID MatchWinner = ETeamID::Spectator;

	/** Active enemy-reveal records (AM + MM). Replicated to all; consumers filter
	 *  by local team / player. Server purges expired entries each tick. */
	UPROPERTY(Replicated)
	TArray<FEnemyRevealEntry> EnemyReveals;

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
