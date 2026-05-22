// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompPlayerState.generated.h"

/**
 * Per-player replicated state.
 * All fields are server-authoritative. Voted abilities are set via RPC from
 * the owning client during PreRound; the GameMode validates and tallies.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompPlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	AProjectCompPlayerState();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ---- Accessors --------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "Player")
	ETeamID GetTeamID() const { return TeamID; }

	UFUNCTION(BlueprintPure, Category = "Player")
	bool IsAlive() const { return bIsAlive; }

	UFUNCTION(BlueprintPure, Category = "Player")
	EAbilityType GetVotedAbility1() const { return VotedAbility1; }

	UFUNCTION(BlueprintPure, Category = "Player")
	EAbilityType GetVotedAbility2() const { return VotedAbility2; }

	UFUNCTION(BlueprintPure, Category = "Player")
	int32 GetKills() const { return Kills; }

	UFUNCTION(BlueprintPure, Category = "Player")
	int32 GetDeaths() const { return Deaths; }

	UFUNCTION(BlueprintPure, Category = "Player")
	int32 GetLivesRemaining() const { return LivesRemaining; }

	UFUNCTION(BlueprintPure, Category = "Player")
	int32 GetMaxLives() const { return MaxLives; }

	/** True when the player is dead AND has exhausted their lives for this round. */
	UFUNCTION(BlueprintPure, Category = "Player")
	bool IsPermanentlyDead() const { return !bIsAlive && LivesRemaining <= 0; }

	/** Server-time (seconds) at which this player will respawn. 0 = no respawn pending. */
	UFUNCTION(BlueprintPure, Category = "Player")
	float GetRespawnEndTime() const { return RespawnEndTime; }

	// ---- Server-only mutators (called by GameMode) ------------------------

	/** Server: assign this player to a team. */
	void Server_SetTeamID(ETeamID NewTeam);

	/** Server: set alive/dead state. */
	void Server_SetIsAlive(bool bNewAlive);

	/** Server: record the player's draft votes for this round. */
	void Server_SetVotes(EAbilityType First, EAbilityType Second);

	/** Server: clear votes (called at the start of each PreRound). */
	void Server_ClearVotes();

	/** Server: increment kill count. */
	void Server_AddKill();

	/** Server: increment death count. */
	void Server_AddDeath();

	/** Server: subtract 1 life. Floors at 0. */
	void Server_DecrementLife();

	/** Server: restore LivesRemaining to MaxLives. Called between rounds. */
	void Server_ResetLives();

	/** Server: set the absolute server-time at which this player will respawn. 0 to clear. */
	void Server_SetRespawnEndTime(float NewTime);

protected:

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Player")
	ETeamID TeamID = ETeamID::Spectator;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Player")
	bool bIsAlive = true;

	/** Player's first ability vote for this round's draft. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Player")
	EAbilityType VotedAbility1 = EAbilityType::None;

	/** Player's second ability vote for this round's draft. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Player")
	EAbilityType VotedAbility2 = EAbilityType::None;

	/** Replicated as uint8 — match-realistic kill counts never exceed 255. Saves
	 *  24 bits per update vs. int32. Public getters keep int32 for BP compat. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Player")
	uint8 Kills = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Player")
	uint8 Deaths = 0;

	/** Lives remaining in the current round. Reset to MaxLives at PreRound start. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Player")
	uint8 LivesRemaining = 2;

	/** Max lives per round. Edit in BP defaults to tune the death model (2 = current design). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Player", meta = (ClampMin = "1", ClampMax = "255"))
	uint8 MaxLives = 2;

	/** Server-time (seconds) at which this player will respawn. 0 = none pending. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Player")
	float RespawnEndTime = 0.0f;
};
