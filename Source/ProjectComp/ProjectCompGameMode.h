// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompGameMode.generated.h"

class AProjectCompGameState;
class AProjectCompPlayerState;
class AProjectCompCharacter;
class AProjectCompBomb;
class AProjectCompBombSpawn;
class AController;
class APlayerController;
class APlayerState;

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

	/**
	 * Resolves a Loom-ability draft claim. Validates: PreRound + selection window
	 * open, caller has valid team, ability != None, AND no teammate already
	 * holds this ability (first-claim-wins arbitration per design memory
	 * "grenade-system-and-loom-pivot"). On success, writes the claim onto the
	 * claimer's PlayerState.
	 */
	bool Server_ClaimLoomAbility(APlayerController* Claimer, EAbilityType Ability);

	/**
	 * Resolves a grenade purchase. Validates: PreRound + selection window open,
	 * caller has valid team + alive, type != None, not already equipped, room
	 * for one more (max 2 distinct types), enough credits. On success, deducts
	 * credits and equips the grenade for the round.
	 */
	bool Server_BuyGrenade(APlayerController* Buyer, EGrenadeType Type);

	/**
	 * Resolves a weapon purchase. Validates: PreRound + selection window open,
	 * caller has valid team, Weapon != None, slot/class match (Pistol → Secondary,
	 * Rifle/SMG → Primary), enough credits. On success, deducts credits and
	 * sets the slot on PlayerState.
	 */
	bool Server_BuyWeapon(APlayerController* Buyer, EWeaponClass Weapon);

	/**
	 * Resolves an armour purchase. Validates: PreRound + selection window open,
	 * caller has valid team, Level != None, enough credits. On success deducts
	 * credits and sets ArmourLevel on PlayerState.
	 */
	bool Server_BuyArmour(APlayerController* Buyer, EArmourLevel Level);

	/**
	 * Right-click in the buy panel: toggle the item in the caller's request
	 * list. Adds if missing, removes if already there. PreRound buy window
	 * gated only. Replicated requests are visible to all teammates.
	 */
	bool Server_RequestWeapon (APlayerController* Requester, EWeaponClass Weapon);
	bool Server_RequestArmour (APlayerController* Requester, EArmourLevel Level);
	bool Server_RequestGrenade(APlayerController* Requester, EGrenadeType Type);

	/**
	 * Teammate fulfillment: caller pays the item's cost and the item is set on
	 * the Requester's loadout. The request is then cleared. Validates: same
	 * team, requester actually had the item requested, caller has credits,
	 * phase is PreRound buy window, slot/level constraints. Returns false on
	 * any rejection (silently — no broadcast). For weapons, the fulfilled
	 * weapon overwrites whatever was in the Requester's relevant slot.
	 */
	bool Server_FulfillWeaponRequest (APlayerController* Fulfiller, APlayerController* Requester, EWeaponClass Weapon);
	bool Server_FulfillArmourRequest (APlayerController* Fulfiller, APlayerController* Requester, EArmourLevel Level);
	bool Server_FulfillGrenadeRequest(APlayerController* Fulfiller, APlayerController* Requester, EGrenadeType Type);

	/** CHEAT: force-end the current RoundActive by awarding +1 score and round-end
	 *  credits to WinningTeam, then transitioning to PostRound. No-op outside
	 *  RoundActive. Dev/test only — strip or gate before production. */
	void Server_CheatEndRound(ETeamID WinningTeam);

	/** CHEAT/testing: toggle freezing the phase-timer countdown so the current
	 *  round never advances while you test. Returns the new frozen state. Server-
	 *  only; the flag stays false in shipping because nothing toggles it there. */
	bool Server_CheatToggleFreezeTimer();

	/** CHEAT/testing: end the current phase and start the next immediately (forces
	 *  the phase clock to expire; from the lobby, starts the match). */
	void Server_CheatAdvancePhase();

	/** When true, Tick() skips the phase-timer decrement so the round clock holds.
	 *  Toggled by the PauseTimer cheat exec. Server-only, never replicated. */
	bool bCheatFreezeTimer = false;

	/** Pre-match lobby: the lobby leader starts the match (begins round 1) out of
	 *  the None phase. No-op if already started, not in the lobby, or the requester
	 *  isn't the current lobby leader. */
	void Server_StartMatch(APlayerController* Requester);

	/** End-of-match: lobby leader returns everyone to the team-select lobby (None
	 *  phase), resetting match scores, stats, lives, credits, loadouts, the bomb,
	 *  and dropped loot. Teams are retained. Only valid from the MatchEnded phase. */
	void Server_ReturnToLobby(APlayerController* Requester);

	/** True when running as a dedicated server (auto-manage the match flow: start,
	 *  return-to-lobby, empty reset). Listen/PIE play stays leader-driven. */
	bool IsDedicatedServerMode() const { return GetNetMode() == NM_DedicatedServer; }

	/** Pre-match lobby: transfer lobby leadership to Target. Only the CURRENT
	 *  leader may call this, and only during the None phase. */
	void Server_SetLobbyLeader(APlayerController* Requester, APlayerState* Target);

	/** Pre-match lobby: change a player's team (free swap, any size). Only honored
	 *  during the None phase; relocates the player to the new team's spawn. */
	void Server_RequestTeamChange(APlayerController* PC, ETeamID NewTeam);

	/** True once the host has started the match (left the lobby). Server-only. */
	bool bMatchStarted = false;

	/** Public so the HealthComponent (or other damage source) can drive the death cascade. */
	void HandlePlayerDeath(APlayerController* Victim, AController* Killer);

	/**
	 * Scan both teams; if one is entirely permanent-ghost (all dead AND no lives
	 * remaining), end the round with an elimination point for the other team.
	 * Called after every death and respawn. Becomes a no-op while any bomb is
	 * planted — bomb resolution takes priority over elimination scoring.
	 */
	void CheckRoundEndCondition();

	/** Returns true if the bomb is currently planted (and not yet terminal). */
	bool IsAnyBombPlanted() const;

	/** True if Actor is inside any bomb-site plant area. Used by the bomb's plant FSM. */
	bool IsActorInBombSite(const AActor* Actor) const;

	/** Server-side cache of all level-placed bomb sites (plant areas). Built in
	 *  StartPlay; bomb sites do not spawn at runtime. */
	UPROPERTY()
	TArray<TWeakObjectPtr<class AProjectCompBombSite>> CachedBombSites;

	/** The single bomb actor (spawned in StartPlay, class = BombClass). */
	UPROPERTY()
	TObjectPtr<class AProjectCompBomb> BombActor;

	/** Class to spawn for the bomb. Defaults to AProjectCompBomb; override in BP. */
	UPROPERTY(EditDefaultsOnly, Category = "Bomb")
	TSubclassOf<class AProjectCompBomb> BombClass;

	/** Level-placed bomb-spawn marker (first found). The bomb resets here each round. */
	UPROPERTY()
	TWeakObjectPtr<class AProjectCompBombSpawn> CachedBombSpawn;

	/**
	 * Server-side event-driven registry of every AProjectCompCharacter currently
	 * in the world. Maintained by AProjectCompCharacter::BeginPlay (register) and
	 * EndPlay (unregister) on the authority. Replaces per-frame TActorIterator
	 * scans in (a) the lag-comp snapshot loop in this class's Tick, (b) the
	 * hitscan weapon's rewind loop in FireOneShot, and (c) the flash dome's
	 * LOS-ignore list rebuild — each previously O(WorldActors), now O(LivePawns).
	 * Weak ptrs guard against torn-down pawns whose dereference would race a
	 * pending unregister. Reserve()d in StartPlay to cap one-time allocation.
	 */
	UPROPERTY()
	TArray<TWeakObjectPtr<AProjectCompCharacter>> LiveCharacters;

	/** Server only: O(1) amortized registry insert. Idempotent — duplicate adds no-op
	 *  so a defensive double-register (e.g. EndPlay/BeginPlay race) cannot bloat the list. */
	void Server_RegisterLiveCharacter(AProjectCompCharacter* Char);

	/** Server only: O(LivePawns) registry remove (RemoveSingleSwap — order doesn't matter).
	 *  Silent no-op if Char was never registered. */
	void Server_UnregisterLiveCharacter(AProjectCompCharacter* Char);

	/** Read-only registry view for hot-path consumers (weapon, flash, snapshot loop).
	 *  Callers must skip stale TWeakObjectPtr entries; the registry purges on EndPlay
	 *  but a holder iterating during teardown could still observe a null Resolve(). */
	const TArray<TWeakObjectPtr<AProjectCompCharacter>>& GetLiveCharacters() const { return LiveCharacters; }

	/** Server: route score for the terminal bomb (detonated/defused) and end the round. */
	void HandleBombResolution();

	/** Server: reset all bomb sites to NotPlanted at the start of a new round. */
	void Server_ResetAllBombSites();

	/** Server: destroy all weapons lying on the ground (no owner) — clears dropped
	 *  loot between rounds so it doesn't accumulate. Called during round prep. */
	void Server_CleanupGroundWeapons();

protected:

	// ---- Phase transition handlers (server only) --------------------------

	/** Server: 10s mouse-locked Loom claim phase. Inserted before rounds where
	 *  the rotation rule fires; see ShouldRunLoomSelectionForRound. Clears
	 *  ChosenLoomAbility on every player at start so picks happen fresh. */
	void StartLoomSelection();

	void StartPreRound();
	void StartRoundActive();
	void StartPostRound();
	/** End the match. ForcedWinner overrides the score-based winner (used for forfeits
	 *  when a team empties); leave as Spectator to derive the winner from the score. */
	void EndMatch(ETeamID ForcedWinner = ETeamID::Spectator);

	/** Server: rotation rule — true if the round needs a LoomSelection before
	 *  its PreRound. Fires on rounds 1, 4, 7, 10, 13, 16, 19, 22 (every 3
	 *  starting from 1). Halftime guarantee: also true for HalftimeRoundNumber+1
	 *  to ensure post-swap players get a fresh pick window, even if the rotation
	 *  math wouldn't naturally place a selection there. */
	bool ShouldRunLoomSelectionForRound(int32 Round) const;

	/** Tally votes for both teams and write the top-2 picks into the GameState team loadouts. */
	void TallyAndLockTeamDraft(ETeamID Team);

	/** Recharge both teams' shared cooldown pools by DeltaSeconds * CooldownRechargeRate, clamped to [0,1]. */
	void TickCooldownPools(float DeltaSeconds);

	/** Sets phase timer and pushes the value to GameState. */
	void SetPhaseTimer(float Duration);

	// ---- Player lifecycle hooks ------------------------------------------

	/** Move a freshly-spawned player into the world and back to alive state. */
	void HandlePlayerRespawn(APlayerController* Player);

	/** Pick the joining player's team: fill the smaller side, capped at MaxTeamSize,
	 *  else Spectator. `Joining` is excluded from the count (it may already be in
	 *  PlayerArray with a default team when this runs). */
	ETeamID PickTeamForNewPlayer(const APlayerState* Joining);

	/** Flip every player's TeamID (Attackers ↔ Defenders). Triggered at halftime. */
	void Server_SwapAllPlayerTeams();

	/** Routes the right `bIsActive` to every barrier in the world based on the
	 *  new phase and each barrier's Kind. Called at every phase transition.
	 *  See EBarrierKind for the per-Kind activation table. */
	void Server_SetAllBarriersForPhase(ERoundPhase NewPhase);

	/**
	 * Server: snap any looming player whose pawn has wandered out of their
	 * team's spawn room back to their assigned PlayerStart. Called every tick.
	 * Uses AttackerSpawnRoomMaxY / DefenderSpawnRoomMinY as bounds.
	 */
	void EnforceLoomingPlayerBounds();

	/**
	 * Server: recompute every live player's line-of-sight "seen" enemies and fold the
	 * result into their PlayerState SpottedEnemies list (owner-only → anti-ESP).
	 * In-LOS enemies become live arrow markers; an enemy that just left LOS converts to
	 * a fading red "?" at its last-seen spot (SpotLostFadeTime); fully-faded records drop.
	 * Throttled by SpotUpdateInterval; called from Tick during RoundActive.
	 */
	void UpdateLineOfSightSpotting(float DeltaSeconds);

	/** Filter team-tagged PlayerStarts so attackers/defenders spawn on the right side of the map. */
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;

	// ---- Tuning knobs (editable in BP defaults) ---------------------------

	/** Duration of the LoomSelection phase in seconds. Mouse-locked, no movement,
	 *  Loom claim only. Inserted at the start of the match and every
	 *  LoomSelectionRoundInterval rounds thereafter. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning", meta = (ClampMin = "0.0"))
	float LoomSelectionDuration = 10.0f;

	/** How often the LoomSelection draft runs: at the start of the match (round 1)
	 *  and every N rounds after. Default 6 → rounds 1, 7, 13, 19. The drafted
	 *  ability persists across the rounds between selections. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning", meta = (ClampMin = "1"))
	int32 LoomSelectionRoundInterval = 6;

	/** Duration of the PreRound phase in seconds (buy window + positioning). */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning", meta = (ClampMin = "0.0"))
	float PreRoundDuration = 30.0f;

	/** First N seconds of PreRound during which the buy UI is active. After
	 *  this window closes, players can move/position in spawn for the
	 *  remaining (PreRoundDuration - SelectionWindowDuration) seconds. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning", meta = (ClampMin = "0.0"))
	float SelectionWindowDuration = 10.0f;

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

	/** Multiplier applied to damage from teammates. 0.0 disables friendly fire entirely,
	 *  1.0 means full damage. Tac-FPS default is off; tournaments may enable a fractional
	 *  value to discourage teamflashing without enabling griefing. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FriendlyFireScalar = 0.0f;

	// ---- Line-of-sight "seen" spotting (drives the minimap red markers) ----

	/** How often (seconds) the server recomputes who each player can see. ~0.1 = 10 Hz;
	 *  cost is ≤ (live players)² traces per pass — trivial at 5v5. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Spotting", meta = (ClampMin = "0.02"))
	float SpotUpdateInterval = 0.1f;

	/** Max distance (cm) an enemy can be auto-spotted at. Beyond this they don't appear. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Spotting", meta = (ClampMin = "0.0"))
	float SpotMaxRange = 9000.0f;

	/** Half-angle (degrees) of the view cone an enemy must be inside to count as "seen".
	 *  ~60 ≈ a 120° cone — a touch wider than the screen FOV so peripheral enemies still spot. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Spotting", meta = (ClampMin = "1.0", ClampMax = "180.0"))
	float SpotConeHalfAngleDeg = 60.0f;

	/** Grace added to each in-LOS record's expiry beyond the update interval, so a single
	 *  missed pass doesn't flicker an actively-seen enemy to the "?" state. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Spotting", meta = (ClampMin = "0.0"))
	float SpotRefreshGrace = 0.35f;

	/** Seconds the red "?" lingers and fades at the last-seen spot after LOS is lost. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Spotting", meta = (ClampMin = "0.0"))
	float SpotLostFadeTime = 3.0f;

	// ---- Loom ability actor classes (BP subclasses hook visuals; the C++ base
	// owns gameplay). One per in-rotation ability. CA + AC remain standby/TBD
	// (no class); the old CA FlashDome is deleted and the MC SoundDome is benched
	// (MC is now Minimap EMP). Each spawned via the shared SpawnLoomAbility<>() —
	// every ability actor exposes a public SpawnZLift + Server_Initialize(ETeamID). ----

	/** AA — Acoustic Deafening: gunfire-mute zone. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Abilities")
	TSubclassOf<class AProjectCompAcousticZone> AcousticZoneClass;

	/** AM — Utility Reveal: enemy utility-loadout scan. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Abilities")
	TSubclassOf<class AProjectCompRevealZone> RevealZoneClass;

	/** CC — Titan Pillar: solid blocker + crush sweep. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Abilities")
	TSubclassOf<class AProjectCompTitanPillar> TitanPillarClass;

	/** CM — Interact Jammer: blocks enemy F-interact in radius. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Abilities")
	TSubclassOf<class AProjectCompInteractJammer> InteractJammerClass;

	/** MA — Tunnel Vision: peripheral-blackout debuff. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Abilities")
	TSubclassOf<class AProjectCompTunnelVisionZone> TunnelVisionZoneClass;

	/** MC — Minimap EMP: zeros enemy Info scans in a map quadrant. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Abilities")
	TSubclassOf<class AProjectCompMinimapEmp> MinimapEmpClass;

	/** MM — Camera Spike: caster-only continuous minimap reveal. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Abilities")
	TSubclassOf<class AProjectCompCameraSpike> CameraSpikeClass;

	// ---- Economy tuning (CS-style; all playtest-tunable in BP defaults) ----

	/** Credits granted on PostLogin (i.e. when a player joins). */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Economy", meta = (ClampMin = "0"))
	int32 StartingCredits = 1000;

	/** Credits granted to every member of the winning team at round end. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Economy", meta = (ClampMin = "0"))
	int32 RoundWinCreditBonus = 600;

	/** Credits granted to every member of the losing team at round end. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Economy", meta = (ClampMin = "0"))
	int32 RoundLossCreditBonus = 400;

	/** Credits granted to a player when they earn a kill. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Economy", meta = (ClampMin = "0"))
	int32 KillCreditBonus = 200;

	/** Per-grenade purchase prices. All four start at the same baseline; tune in
	 *  BP defaults once playtest data exists. Lookup via GetGrenadeCost(EGrenadeType). */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Economy", meta = (ClampMin = "0"))
	int32 FlashGrenadeCost = 300;

	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Economy", meta = (ClampMin = "0"))
	int32 InfoGrenadeCost = 300;

	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Economy", meta = (ClampMin = "0"))
	int32 VulnerableGrenadeCost = 300;

	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Economy", meta = (ClampMin = "0"))
	int32 SmokeGrenadeCost = 300;

	// ---- Weapon + armour pricing (CS-style ballpark; tune in playtest) ---

	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Economy", meta = (ClampMin = "0"))
	int32 PistolCost = 250;

	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Economy", meta = (ClampMin = "0"))
	int32 SMGCost = 1200;

	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Economy", meta = (ClampMin = "0"))
	int32 RifleCost = 2700;

	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Economy", meta = (ClampMin = "0"))
	int32 ArmourLightCost = 650;

	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Economy", meta = (ClampMin = "0"))
	int32 ArmourHeavyCost = 1000;

	/** Returns the configured cost for the given grenade type. Returns 0 for None. */
	int32 GetGrenadeCost(EGrenadeType Type) const;

	/** Returns the configured cost for the given weapon class. Returns 0 for None. */
	int32 GetWeaponCost(EWeaponClass Weapon) const;

	/** Returns the configured cost for the given armour level. Returns 0 for None. */
	int32 GetArmourCost(EArmourLevel Level) const;

	/** Awards round-end credits to all players based on the winning team.
	 *  Spectators receive nothing. Called from each win-determination path
	 *  immediately BEFORE StartPostRound() so credits replicate during PostRound
	 *  and are present by the time the next PreRound's buy window opens. */
	void AwardRoundEndCredits(ETeamID WinningTeam);

public:
	float GetFriendlyFireScalar() const { return FriendlyFireScalar; }

protected:

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
	int32 HalftimeRoundNumber = 12;

	// ---- Dedicated-server lifecycle (auto-managed only when NetMode == DedicatedServer;
	// listen/PIE play keeps the leader-driven Start / Return-to-Lobby buttons) -------

	/** Min connected players before a dedicated server auto-starts a match from the lobby. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Server", meta = (ClampMin = "1"))
	int32 MinPlayersToAutoStart = 2;

	/** Max players per team. Join-time assignment keeps each team at or below this. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Server", meta = (ClampMin = "1"))
	int32 MaxTeamSize = 5;

	/** Seconds the results screen stays up before a dedicated server auto-returns to the lobby. */
	UPROPERTY(EditDefaultsOnly, Category = "Match Tuning|Server", meta = (ClampMin = "0.0"))
	float PostMatchDuration = 15.0f;

private:

	/** Convenience getter for the typed GameState. */
	AProjectCompGameState* GetTypedGameState() const;

	/** Server: begin the round loop (round 1). Shared by the leader-driven
	 *  Server_StartMatch and the dedicated-server auto-start. No gating. */
	void BeginMatch();

	/** Server: reset all match state back to the team-select lobby (None phase).
	 *  Shared by Server_ReturnToLobby and the dedicated auto-return / empty reset. */
	void ResetToLobby();

	/** Dedicated only: auto-start once MinPlayersToAutoStart are present in the lobby. */
	void TickServerLobby();

	/** Count connected players currently assigned to Team (optionally ignoring one
	 *  PlayerState, e.g. a player mid-Logout still in PlayerArray). */
	int32 CountPlayersOnTeam(ETeamID Team, const APlayerState* Ignore) const;

	/** Dedicated only: if a team has emptied mid-match, forfeit to the other team. */
	void CheckForfeit(const APlayerState* Ignore);

	/** Server: per-round prep that needs to happen exactly once at the start
	 *  of each round, regardless of whether that round opens with
	 *  LoomSelection or jumps straight to PreRound. Idempotent — guarded by
	 *  LastPreparedRound so a second call within the same round no-ops. */
	void PrepareForNewRound();

	/** Server-only counter: the CurrentRound value that PrepareForNewRound has
	 *  already run for. Starts at 0 so round 1's first call always prepares. */
	int32 LastPreparedRound = 0;

	/** Server-only: accumulates DeltaSeconds to throttle LOS spotting to SpotUpdateInterval. */
	float SpotAccumulator = 0.0f;

	/** Returns true if the given pair is a valid draft submission (distinct, non-None). */
	static bool IsValidVotePair(EAbilityType First, EAbilityType Second);

	/** Per-player respawn timer handles, keyed by PlayerController. */
	UPROPERTY()
	TMap<TObjectPtr<APlayerController>, FTimerHandle> RespawnTimers;

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
