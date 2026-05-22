// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompGameMode.generated.h"

class AProjectCompGameState;
class AProjectCompPlayerState;
class AController;
class APlayerController;

/**
 * Server-only authority for the match.
 *
 * Owns the round state machine, vote tallying, ability cast validation,
 * respawn timing, and team score updates. Never replicated; clients have no
 * GameMode instance.
 *
 * Ticking is enabled because we run two server-side loops:
 *   1. Phase timer countdown (PhaseTimeRemaining decrement).
 *   2. Per-team cooldown pool recharge.
 * Both are cheap (single-digit float ops).
 */
UCLASS(abstract)
class AProjectCompGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AProjectCompGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;

	// ---- RPC entry points (called by the PlayerController) ----------------

	/**
	 * Records a player's two ability votes for the current PreRound.
	 * Validates: caller exists, has a valid team, both abilities distinct,
	 * neither is None, current phase is PreRound. Returns true if accepted.
	 */
	bool Server_SubmitAbilityVote(APlayerController* Voter, EAbilityType First, EAbilityType Second);

	/**
	 * Resolves a Loom ability cast request.
	 * Validates: caller is looming (dead), round is active, team's pool is full,
	 * the requested ability is one of the two drafted for the team. On success,
	 * deducts the pool and multicasts the VFX hook on the GameState.
	 */
	bool Server_RequestAbilityCast(APlayerController* Caster, EAbilityType Type, FVector TargetLocation);

	/** Public so the HealthComponent (or other damage source) can drive the death cascade. */
	void HandlePlayerDeath(APlayerController* Victim, AController* Killer);

	/**
	 * Scan both teams; if one is entirely permanent-ghost (all dead AND no lives
	 * remaining), end the round with an elimination point for the other team.
	 * Called after every death and respawn. Becomes a no-op while any bomb is
	 * planted — bomb resolution takes priority over elimination scoring.
	 */
	void CheckRoundEndCondition();

	/** Returns true if any bomb site in the world is currently planted (and not yet terminal). */
	bool IsAnyBombPlanted() const;

	/** Returns the first bomb site that has reached a terminal state (detonated/defused) this round, or null. */
	class AProjectCompBombSite* GetTerminalBombSite() const;

	/** Server: route score for a terminal bomb site and end the round. */
	void HandleBombResolution(class AProjectCompBombSite* Site);

	/** Server: reset all bomb sites to NotPlanted at the start of a new round. */
	void Server_ResetAllBombSites();

protected:

	// ---- Phase transition handlers (server only) --------------------------

	void StartPreRound();
	void StartRoundActive();
	void StartPostRound();
	void EndMatch();

	/** Tally votes for both teams and write the top-2 picks into the GameState team loadouts. */
	void TallyAndLockTeamDraft(ETeamID Team);

	/** Recharge both teams' shared cooldown pools by DeltaSeconds * CooldownRechargeRate, clamped to [0,1]. */
	void TickCooldownPools(float DeltaSeconds);

	/** Sets phase timer and pushes the value to GameState. */
	void SetPhaseTimer(float Duration);

	// ---- Player lifecycle hooks ------------------------------------------

	/** Move a freshly-spawned player into the world and back to alive state. */
	void HandlePlayerRespawn(APlayerController* Player);

	/** Deterministic alternation by join order — see implementation comment. Non-const because GetNumPlayers() is non-const. */
	ETeamID PickTeamForNewPlayer();

	/** Flip every player's TeamID (Attackers ↔ Defenders). Triggered at halftime. */
	void Server_SwapAllPlayerTeams();

	/** Raise (true) / drop (false) every barrier in the world. Called at phase transitions. */
	void Server_SetAllBarriersActive(bool bActive);

	/**
	 * Server: snap any looming player whose pawn has wandered out of their
	 * team's spawn room back to their assigned PlayerStart. Called every tick.
	 * Uses AttackerSpawnRoomMaxY / DefenderSpawnRoomMinY as bounds.
	 */
	void EnforceLoomingPlayerBounds();

	/** Filter team-tagged PlayerStarts so attackers/defenders spawn on the right side of the map. */
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;

	// ---- Tuning knobs (editable in BP defaults) ---------------------------

	/** Duration of the PreRound phase in seconds (draft + barrier). */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning", meta = (ClampMin = "0.0"))
	float PreRoundDuration = 15.0f;

	/** Duration of an active round in seconds. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning", meta = (ClampMin = "0.0"))
	float RoundActiveDuration = 100.0f;

	/** Duration of the PostRound phase in seconds (killfeed/MVP cleanup). */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning", meta = (ClampMin = "0.0"))
	float PostRoundDuration = 5.0f;

	/**
	 * Seconds a player spends in temporary-ghost mode before respawning at
	 * the team spawn. Designed as the player's "low-cortisol support window" —
	 * time for ghost-casting, map reading, and comms. Only applies to deaths
	 * with lives remaining; on the final death, the player is permanent ghost
	 * for the rest of the round (no respawn).
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning", meta = (ClampMin = "0.0"))
	float RespawnDelay = 14.0f;

	/**
	 * Fraction of the cooldown pool restored per second. 0.1 = 10s to recharge
	 * from empty to full. Applied uniformly to both teams; only ticks during
	 * RoundActive.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning", meta = (ClampMin = "0.0"))
	float CooldownRechargeRate = 0.1f;

	/** Score threshold that ends the match (first team to reach this many round wins). */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning", meta = (ClampMin = "1"))
	int32 RoundsToWinMatch = 13;

	/**
	 * Y-axis ceiling for the attacker spawn room — a looming attacker whose
	 * pawn drifts above this Y is snapped back to their assigned PlayerStart.
	 * Map-specific; tune per level in BP defaults. Substrate: -2400.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Spawn Rooms")
	float AttackerSpawnRoomMaxY = -2400.0f;

	/** Y-axis floor for the defender spawn room (Substrate: 2400). */
	UPROPERTY(EditDefaultsOnly, Category = "Spawn Rooms")
	float DefenderSpawnRoomMinY = 2400.0f;

	/**
	 * After this round ends, every player's TeamID flips (Attackers ↔ Defenders).
	 * Scores stay with the team, not the player. Set to 0 to disable.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning", meta = (ClampMin = "0"))
	int32 HalftimeRoundNumber = 8;

private:

	/** Convenience getter for the typed GameState. */
	AProjectCompGameState* GetTypedGameState() const;

	/** Returns true if the given pair is a valid draft submission (distinct, non-None). */
	static bool IsValidVotePair(EAbilityType First, EAbilityType Second);

	/** Per-player respawn timer handles, keyed by PlayerController. */
	UPROPERTY()
	TMap<TObjectPtr<APlayerController>, FTimerHandle> RespawnTimers;

	/**
	 * Monotonic counter incremented once per PostLogin. Used by PickTeamForNewPlayer
	 * to alternate team assignment deterministically (odd → Attackers, even → Defenders)
	 * without relying on engine-counted state that can race in PIE listen-server.
	 */
	int32 JoinSequence = 0;

	/**
	 * Each player's sticky PlayerStart assignment for the current round.
	 * Mid-round respawns reuse the same spot — avoids "two teammates respawn
	 * to the same PlayerStart at the same instant" bugs. Cleared and
	 * re-populated each PreRound so a new round gives fresh allocations.
	 */
	UPROPERTY()
	TMap<TObjectPtr<APlayerController>, TObjectPtr<AActor>> AssignedStarts;

	/**
	 * Server: teleport the player's pawn to its assigned team-correct PlayerStart
	 * (or lazy-assigns one via ChoosePlayerStart if missing). Position-only
	 * move plus yaw-only rotation on both pawn and controller — avoids the
	 * sideways-camera bug we hit when passing a full FRotator to TeleportTo.
	 */
	void RelocatePlayerToTeamStart(APlayerController* PC);

	/**
	 * Server: destroy the existing pawn (frozen-corpse or otherwise), spawn a
	 * fresh one via the engine's RestartPlayer flow, relocate to the team-
	 * assigned PlayerStart, and reset health. Used by both mid-round respawn
	 * and end-of-round reset so every "you're back in the game" path produces
	 * a fully-functional pawn.
	 */
	void RespawnPlayerAtAssignedStart(APlayerController* PC);
};
