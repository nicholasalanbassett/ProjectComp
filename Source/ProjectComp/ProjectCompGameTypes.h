// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProjectCompGameTypes.generated.h"

/** Team identity. Index into GameState team arrays. */
UENUM(BlueprintType)
enum class ETeamID : uint8
{
	Attackers = 0,
	Defenders = 1,
	Spectator = 255
};

/** Round state machine phases. Server-driven, replicated to all clients. */
UENUM(BlueprintType)
enum class ERoundPhase : uint8
{
	/** Match has not begun. Initial state. */
	None,
	/** Barrier phase: draft voting + loadout selection. No gunplay. */
	PreRound,
	/** Live gunplay phase. Death sends players to ghost spectator. */
	RoundActive,
	/** Win condition resolved; killfeed/MVP window. Cleanup before next PreRound. */
	PostRound,
	/** Final round complete; match summary shown. Terminal. */
	MatchEnded
};

/**
 * Respawn ability identity. Nine unique combinations of the three design pillars:
 * (A)ggro / (C)ontrol / (M)idrange. First letter = primary pillar, second = secondary.
 */
UENUM(BlueprintType)
enum class EAbilityType : uint8
{
	None = 0,
	AA, AC, AM,
	CC, CA, CM,
	MM, MA, MC
};

/**
 * Per-team state, replicated as a TArray on the GameState (one entry per team).
 * All fields are server-authoritative; clients receive read-only copies.
 */
USTRUCT(BlueprintType)
struct FTeamLoadout
{
	GENERATED_BODY()

	/** First locked-in ability for this round. None until PreRound draft completes. */
	UPROPERTY(BlueprintReadOnly, Category = "Team")
	EAbilityType AbilitySlot1 = EAbilityType::None;

	/** Second locked-in ability for this round. */
	UPROPERTY(BlueprintReadOnly, Category = "Team")
	EAbilityType AbilitySlot2 = EAbilityType::None;

	/**
	 * Shared team ability cooldown. Range [0, 1].
	 * 1.0 = ready to cast; an ability consumes 1.0 -> 0.0 and recharges over time.
	 * Both AbilitySlot1 and AbilitySlot2 draw from this single pool.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Team")
	float SharedCooldownPool = 1.0f;

	/** Rounds won this match. */
	UPROPERTY(BlueprintReadOnly, Category = "Team")
	int32 Score = 0;
};
