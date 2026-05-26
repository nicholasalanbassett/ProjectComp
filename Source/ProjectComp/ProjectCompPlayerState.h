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

	/** Server-time the player's current flash effect fully ends. 0 = not flashed.
	 *  HUD reads this each tick to drive the white overlay alpha. */
	UFUNCTION(BlueprintPure, Category = "Player")
	float GetFlashEndTime() const { return FlashEndTime; }

	/** Server-time the fade phase begins (FlashEndTime - FadeDuration). Up to this
	 *  point the overlay is full white; from here it linearly fades to zero. */
	UFUNCTION(BlueprintPure, Category = "Player")
	float GetFlashFadeStartTime() const { return FlashFadeStartTime; }

	// ---- Loom + Economy accessors ---------------------------------------

	UFUNCTION(BlueprintPure, Category = "Player|Economy")
	int32 GetCurrentCredits() const { return CurrentCredits; }

	/** True if this player has spent their single Loom charge for the round. */
	UFUNCTION(BlueprintPure, Category = "Player|Loom")
	bool HasUsedLoomThisRound() const { return bHasUsedLoomThisRound; }

	/** The Loom ability claimed during this round's PreRound draft. None = not yet claimed. */
	UFUNCTION(BlueprintPure, Category = "Player|Loom")
	EAbilityType GetChosenLoomAbility() const { return ChosenLoomAbility; }

	// ---- Grenade accessors ----------------------------------------------

	/** Up to two distinct grenade types equipped this round. */
	const TArray<EGrenadeType>& GetEquippedGrenadeTypes() const { return EquippedGrenadeTypes; }

	UFUNCTION(BlueprintPure, Category = "Player|Grenade")
	bool HasGrenadeEquipped(EGrenadeType Type) const;

	/** Server-time at which the given grenade's cooldown expires. 0 = ready (or not equipped). */
	UFUNCTION(BlueprintPure, Category = "Player|Grenade")
	float GetGrenadeCooldownEndTime(EGrenadeType Type) const;

	/** True if the given grenade is equipped AND its cooldown end-time is in the future. */
	UFUNCTION(BlueprintPure, Category = "Player|Grenade")
	bool IsGrenadeOnCooldown(EGrenadeType Type) const;

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

	/** Server: apply a flash effect with the given full-white + fade durations.
	 *  Stamps FlashFadeStartTime + FlashEndTime in absolute server-time and replicates. */
	void Server_ApplyFlash(float FullWhiteDuration, float FadeDuration);

	// ---- Server: Economy -------------------------------------------------

	/** Server: set wallet to an absolute amount (clamped to uint16 range). */
	void Server_SetCredits(int32 NewAmount);

	/** Server: add credits (positive Delta). Clamped to uint16 ceiling. */
	void Server_AddCredits(int32 Delta);

	/** Server: try to deduct Cost. Returns true if the player had enough and was charged. */
	bool Server_SpendCredits(int32 Cost);

	// ---- Server: Loom ---------------------------------------------------

	/** Server: claim a Loom ability for this round. No same-team duplicate check here —
	 *  the GameMode performs first-claim arbitration before calling this. */
	void Server_SetChosenLoomAbility(EAbilityType Ability);

	/** Server: clear the Loom claim (called at PreRound start). */
	void Server_ClearChosenLoomAbility();

	/** Server: record that the player has spent their single Loom charge for the round. */
	void Server_MarkLoomUsed();

	/** Server: clear the per-round Loom usage flag (called at PreRound start). */
	void Server_ResetLoomUsage();

	// ---- Server: Grenades -----------------------------------------------

	/** Server: equip a grenade type if not already equipped and slots available (max 2).
	 *  Returns true on success. Does NOT charge credits — GameMode handles purchasing. */
	bool Server_EquipGrenade(EGrenadeType Type);

	/** Server: remove all equipped grenade types and reset all cooldown end-times.
	 *  Called at PreRound start to clear last round's loadout. */
	void Server_ClearEquippedGrenades();

	/** Server: set the absolute server-time at which the given grenade's cooldown expires. */
	void Server_SetGrenadeCooldownEndTime(EGrenadeType Type, float NewEndTime);

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

	/** Server-time the current flash effect fully ends. 0 = not flashed. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Player")
	float FlashEndTime = 0.0f;

	/** Server-time the fade phase begins. Equal to FlashEndTime - FadeDuration. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Player")
	float FlashFadeStartTime = 0.0f;

	// ---- Economy --------------------------------------------------------

	/** Credits this player has available to spend in the PreRound buy phase.
	 *  Owner-only on the wire — opponent wallets aren't useful info to leak.
	 *  uint16 is not BP-readable directly; BP consumes via GetCurrentCredits(). */
	UPROPERTY(Replicated)
	uint16 CurrentCredits = 0;

	// ---- Loom -----------------------------------------------------------

	/** Loom ability this player claimed during the current round's draft.
	 *  Replicated to everyone: teammates need it for duplicate-prevention UI;
	 *  enemies learn it anyway the moment it's cast. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Player|Loom")
	EAbilityType ChosenLoomAbility = EAbilityType::None;

	/** True once this player has spent their single Loom charge for the current
	 *  round. Reset by Server_ResetLoomUsage at PreRound start. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Player|Loom")
	bool bHasUsedLoomThisRound = false;

	// ---- Grenades -------------------------------------------------------

	/** Up to two distinct grenade types this player owns for the round.
	 *  Owner-only: enemy loadouts are revealed by use, not replicated up-front. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Player|Grenade")
	TArray<EGrenadeType> EquippedGrenadeTypes;

	/** Per-type cooldown end times in absolute server-time. Fixed-size, indexed by
	 *  (uint8)EGrenadeType — slot 0 (None) is unused. Owner-only on the wire.
	 *  0.0 = ready (or grenade not equipped). */
	UPROPERTY(Replicated)
	TArray<float> GrenadeCooldownEndTimes;
};
