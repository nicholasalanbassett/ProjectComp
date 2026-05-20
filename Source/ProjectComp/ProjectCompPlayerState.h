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

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Player")
	int32 Kills = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Player")
	int32 Deaths = 0;
};
