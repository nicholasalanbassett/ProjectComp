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
 * Grenade type identity. Four utility grenades on the Aggro/Control/Midrange matrix.
 * Players equip up to two distinct types per round (bought during PreRound).
 *
 *  - Flash:      Aggro Midrange. Hand-thrown straight-line projectile; phases one wall.
 *  - Info:       Midrange Control. Map-placed; snapshots enemy count after delay.
 *  - Vulnerable: Aggro Control. Map-placed; 2x damage zone, breakable at 1 HP.
 *  - Sound:      Midrange Control. Map-placed; dampens enemy audio, toggleable by placer.
 *
 * Indexed numerically by (uint8) cast for TArray lookups (e.g. cooldown end times).
 * GrenadeType_MAX is the array-size sentinel; do not use as a valid type.
 */
UENUM(BlueprintType)
enum class EGrenadeType : uint8
{
	None = 0,
	Flash,
	Info,
	Vulnerable,
	Sound,
	GrenadeType_MAX UMETA(Hidden)
};

/** Payload for a single kill event; pushed into the GameState queue on every client. */
USTRUCT()
struct FKillFeedData
{
	GENERATED_BODY()

	FString KillerName;
	FString VictimName;
	ETeamID KillerTeam = ETeamID::Spectator;
	ETeamID VictimTeam = ETeamID::Spectator;
};

/** Hit-zone classification used by hitscan weapons to scale damage per body region. */
UENUM(BlueprintType)
enum class EHitZone : uint8
{
	Body = 0,    // Default; chest/upper torso
	Head,        // Maximum multiplier
	Stomach,     // Mid-tier
	Limb         // Reduced multiplier
};

/**
 * Spatial / functional role of an AProjectCompBarrier instance. Set per-instance
 * in the level editor (typically via a BP subclass with the Kind baked into
 * defaults). Drives both visibility and collision in RefreshAppearance, and
 * tells the GameMode which phases the barrier should be "engaged" in.
 *
 *  - AttackerSpawnLock: At attacker spawn-room boundary. PreRound = visible,
 *    blocks every channel (locks attackers in for strat phase). RoundActive
 *    and PostRound = invisible, blocks only ECC_DeadPawn so looming attackers
 *    can't leave spawn (safe zone for grenade lineups, etc.) while live
 *    players from either team cross freely.
 *
 *  - DefenderSpawnLock: At defender spawn-room boundary. PreRound = INACTIVE
 *    (invisible, no collision) so live defenders walk out to pre-position
 *    however they want. RoundActive and PostRound = VISIBLE, blocks only
 *    ECC_DeadPawn so looming defenders are held in spawn (same safe-zone
 *    purpose as the attacker side). Visible because defenders want to see
 *    where their safe zone ends.
 *
 *  - DefenderPreRoundLock: A PreRound-only midfield wall. PreRound = visible,
 *    blocks every channel (defenders can pre-position up to but not past this
 *    line). RoundActive and PostRound = INACTIVE (invisible, no collision)
 *    so gunplay flows normally.
 *
 * Halftime: barriers are ROLE-PERMANENT — OwnerTeam does NOT flip at halftime.
 * The team that currently has TeamID matching a barrier's OwnerTeam sees the
 * translucent material; the other side sees opaque. No swap call needed.
 */
UENUM(BlueprintType)
enum class EBarrierKind : uint8
{
	AttackerSpawnLock = 0,
	DefenderSpawnLock = 1,
	DefenderPreRoundLock = 2
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
