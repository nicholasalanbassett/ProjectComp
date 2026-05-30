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

/** Round state machine phases. Server-driven, replicated to all clients.
 *
 * Match shape: 12 rounds per half × 2 = 24 rounds max. Halftime swap after
 * round 12. First-to-13 wins (CS-style).
 *
 * Phase flow per round:
 *   [LoomSelection (10s)]  ←-- inserted before rounds 1, 4, 7, 10, 13,
 *                                16, 19, 22 (rotation: (round-1)%3 == 0
 *                                AND/OR halftime first-round guarantee).
 *                                Mouse-locked, no movement, Loom claim only.
 *   PreRound (30s)         ←-- 10s hover-free buy window for weapons +
 *                                grenades + armour, then 20s positioning
 *                                while the barrier holds. Mouse hover-free.
 *   RoundActive (100s)     ←-- gunplay
 *   PostRound (5s)         ←-- killfeed/MVP cleanup
 *
 * ChosenLoomAbility persists for the 3-round window between LoomSelections;
 * bHasUsedLoomThisRound still resets every PreRound (1 cast per round). */
UENUM(BlueprintType)
enum class ERoundPhase : uint8
{
	/** Match has not begun. Initial state. */
	None,
	/** 10s mouse-locked Loom draft. Fires before rounds where the rotation
	 *  rule (round-1)%3 == 0 hits (rounds 1, 4, 7, 10, 13, 16, 19, 22).
	 *  Player picks ONE EAbilityType; no two teammates share a pick.
	 *  Inserted between PostRound (prev round) and PreRound (this round). */
	LoomSelection,
	/** 30s barrier phase: 10s hover-free buy window for weapons/grenades/
	 *  armour, then 20s of positioning while the barrier holds. */
	PreRound,
	/** Live gunplay phase. Death sends players to ghost spectator. */
	RoundActive,
	/** Win condition resolved; killfeed/MVP window. Cleanup before the next
	 *  LoomSelection (or PreRound, depending on rotation). */
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
 * Weapon class identity for the PreRound buy phase. Primary slot accepts Rifle
 * + SMG; Secondary slot accepts Pistol. The server-side buy RPC validates the
 * slot/class pairing.
 *
 * This is a stub list for the buy-phase UI — actual weapon BP classes are
 * wired separately via a TMap<EWeaponClass, TSubclassOf<AProjectCompHitscanWeapon>>
 * on the GameMode (to be added when integrating with character spawn).
 */
UENUM(BlueprintType)
enum class EWeaponClass : uint8
{
	None = 0,
	Pistol,
	SMG,
	Rifle,
	WeaponClass_MAX UMETA(Hidden)
};

/**
 * Armour purchase tiers. Persists for one round (resets in PrepareForNewRound).
 */
UENUM(BlueprintType)
enum class EArmourLevel : uint8
{
	None = 0,
	Light,
	Heavy,
	ArmourLevel_MAX UMETA(Hidden)
};

/**
 * Grenade type identity. Four utility grenades on the Aggro/Control/Midrange matrix.
 * Players equip up to two distinct types per round (bought during PreRound).
 *
 *  - Flash:      Aggro Midrange. Thrown projectile; flashes players in radius.
 *  - Info:       Midrange Control. Thrown; snapshots enemy count after delay.
 *  - Vulnerable: Aggro Control. Thrown; 2x damage zone, breakable at 1 HP.
 *  - Smoke:      Control. Thrown; blooms a vision-blocking cloud for a duration.
 *
 * All four share the standardized throw (LMB gravity arc / RMB straight line).
 * (Audio-dampening, formerly the "Sound" grenade, is now the MC Loom ability.)
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
	Smoke,
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
