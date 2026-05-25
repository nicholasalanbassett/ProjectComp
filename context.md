# PROJECT COMP - BLUEPRINT [MAX REPO CONTEXT]

## 1. CORE ARCHITECTURE
- **Genre:** 5v5 Tactical FPS (High Competitive Integrity / Anti-Cheat Focused)
- **Engine & Version:** UE 5.7 (`EngineAssociation: "5.7"`, module `ProjectComp` Runtime, deps: Engine/AIModule/UMG; plugins: ModelingToolsEditorMode (Editor), StateTree, GameplayStateTree)
- **Target Mechanics:** Low-poly/stylized graphics, high mechanical depth, strict server authority, high-polling rate processing (Wooting-optimized), precise local prediction.
- **Game Format:** 5v5 Attackers vs Defenders, bomb-plant objective + "Loom" dead-player support mechanic (NOT "Ghost Commander"). 2 lives/player/round (`MaxLives=2`). First to `RoundsToWinMatch=13` wins. Halftime team swap at round `HalftimeRoundNumber=8`.
- **Round Loop:** `PreRound` (15s draft+barrier) → `RoundActive` (100s gunplay) → `PostRound` (5s killfeed) → repeat. Terminal: `MatchEnded`.
- **Ability System:** 9 abilities (AA/AC/AM/CC/CA/CM/MM/MA/MC) on 3 pillars (Aggro/Control/Midrange). Per-team shared cooldown pool [0..1], recharge rate `0.1/s`. Dead players draft + cast.

## 2. REPO TOPOGRAPHY (Key Classes)
- [Source/ProjectComp/ProjectComp.h](Source/ProjectComp/ProjectComp.h): `LogProjectComp` category; `#define ECC_DeadPawn ECC_GameTraceChannel2` (looming-pawn collision channel, configured in DefaultEngine.ini).
- [ProjectCompGameTypes.h](Source/ProjectComp/ProjectCompGameTypes.h): `ETeamID` (Attackers=0/Defenders=1/Spectator=255), `ERoundPhase`, `EAbilityType`, `EHitZone` (Body/Head/Stomach/Limb), `FTeamLoadout`, `FKillFeedData`.
- [ProjectCompGameMode.h/.cpp](Source/ProjectComp/ProjectCompGameMode.h): Server-only match authority. Owns round FSM, vote tally, respawn timers (`RespawnTimers TMap`), team assignment (`JoinSequence` alternation, `AssignedStarts TMap`), bomb-site cache, looming-player spawn-room bound enforcement (`EnforceLoomingPlayerBounds` w/ `AttackerSpawnRoomMaxY=-2400`, `DefenderSpawnRoomMinY=2400`).
- [ProjectCompGameState.h/.cpp](Source/ProjectComp/ProjectCompGameState.h): Pure state replicator, zero logic. `PhaseTimeRemainingDs` (uint16 deciseconds wire), `PhaseTimeRemainingServerFloat` (server-only precise). `Teams TArray<FTeamLoadout>` size 2. `PendingKillFeedEvents` consumed by HUD.
- [ProjectCompPlayerState.h/.cpp](Source/ProjectComp/ProjectCompPlayerState.h): `TeamID`, `bIsAlive`, `VotedAbility1/2`, `Kills/Deaths` (uint8 wire), `LivesRemaining/MaxLives` (uint8), `RespawnEndTime` (server time).
- [ProjectCompPlayerController.h/.cpp](Source/ProjectComp/ProjectCompPlayerController.h): Input mapping contexts, Loom RPC entry points (`Server_SubmitAbilityVote`, `Server_RequestAbilityCast` — both `Server, Reliable, WithValidation`).
- [ProjectCompCharacter.h/.cpp](Source/ProjectComp/ProjectCompCharacter.h): First-person tac-FPS pawn. Owns `FirstPersonMesh`, `FirstPersonCameraComponent`, `HealthComponent`, replicated `CurrentWeapon`. Hosts anti-cheat aim-rate clamp (`MaxYawRateDegPerSec=1440`, `MaxPitchRateDegPerSec=720`, `MaxAbsolutePitch=89`), lag-comp ring buffer (`HistoryBuf[32]` = 500ms@64Hz), recoil accumulators, bomb-site overlap counter, interact lock.
- [ProjectCompCharacterMovementComponent.h/.cpp](Source/ProjectComp/ProjectCompCharacterMovementComponent.h): Prediction-safe tactical walk (`FLAG_Custom_0`/`bWantsToWalk`, `TacticalWalkSpeed=320`), subtle mouse airstrafe (`AirSteerStrength=0.2`), Valorant-style in-air horiz speed cap (`JumpedGroundSpeed` snapshot, `StandstillAirSpeedCap=50`), crouch-jump z scale `0.7`. Custom `FSavedMove_ProjectComp` + `FNetworkPredictionData_Client_ProjectComp`.
- [ProjectCompHitscanWeapon.h/.cpp](Source/ProjectComp/ProjectCompHitscanWeapon.h): Server-authoritative hitscan. Tickrate-independent catch-up fire (`MaxShotsPerTick=3` cap), `FireInterval=0.1` (600 RPM), `BodyDamage=30`, head x4 / stomach x1.1 / limb x0.75, `MaxRange=10000cm`. `BoneZoneMap TMap<FName,EHitZone>`. Movement/crouch/air spread, deterministic `RecoilPattern TArray<FVector2D>` (X=pitch up, Y=yaw right), server+client mirror indices. Lag comp: `MaxRewindSeconds=0.25`, `ClientInterpolationDelay=0.05`. Cosmetic broadcast `Multicast_OnFired` uses `FVector_NetQuantize` + `FVector_NetQuantizeNormal` + uint16 distance; hit endpoint NEVER sent (anti-ESP).
- [ProjectCompHealthComponent.h/.cpp](Source/ProjectComp/ProjectCompHealthComponent.h): Replicated HP container. `CurrentHealth uint8` (ReplicatedUsing=`OnRep_CurrentHealth`), `MaxHealth=100`. `Server_TakeDamage`/`Server_ResetHealth`/`Server_Heal`. Delegates `OnHealthChanged`, `OnDeath`.
- [ProjectCompBombSite.h/.cpp](Source/ProjectComp/ProjectCompBombSite.h): Server-authoritative plant/defuse/detonate FSM. Replicated: `bIsPlanted` (`OnRep_IsPlanted`), `bIsDetonated`, `bIsDefused`, `DetonationEndTime`, `PlantProgress`, `DefuseProgress`. Tunables: `PlantDuration=4`, `DefuseDuration=6`, `DetonationDuration=35`, `Label="A"`, `PointValue=1`, `DefusePointValue=1`. Trigger (UBoxComponent) overlap counter.
- [ProjectCompBarrier.h/.cpp](Source/ProjectComp/ProjectCompBarrier.h): One-way visibility spawn barrier. `OwnerTeam` (ReplicatedUsing=`OnRep_OwnerTeam`), `bIsActive` (ReplicatedUsing=`OnRep_IsActive`). Owning team sees translucent material; opposing sees opaque. `Server_SwapOwnerTeam` at halftime.
- [ProjectCompPlayerStart.h/.cpp](Source/ProjectComp/ProjectCompPlayerStart.h): Team-tagged `APlayerStart`. `GetTeamID()` used by `ChoosePlayerStart_Implementation`.
- [ProjectCompHUDWidget.h/.cpp](Source/ProjectComp/ProjectCompHUDWidget.h): C++-only UMG. Valorant-style top-center scoreboard, HP corner, ability indicator, killfeed (`KillFeedRows` + parallel `KillFeedTimers`), bomb status box. Pulls per `NativeTick`. `CachedBombSites` one-shot at init (no per-frame `TActorIterator`).
- [ProjectCompHUD.h/.cpp](Source/ProjectComp/ProjectCompHUD.h): Thin AHUD owner; constructs/destructs `HUDWidget`.
- [ProjectCompCameraManager.h/.cpp](Source/ProjectComp/ProjectCompCameraManager.h): Basic pitch clamp camera manager.

## 3. NETWORKING & REPLICATION INVENTORY

### Replicated GameState (`AProjectCompGameState`)
- `CurrentPhase` (ERoundPhase, RepNotify `OnRep_CurrentPhase`)
- `CurrentRound` (int32)
- `PhaseTimeRemainingDs` (uint16 deciseconds — wire-compact; server holds float separately)
- `Teams` (TArray<FTeamLoadout>, size 2) — each entry: `AbilitySlot1`, `AbilitySlot2`, `SharedCooldownPool` (float [0..1]), `Score` (int32)

### Replicated PlayerState (`AProjectCompPlayerState`)
- `TeamID`, `bIsAlive`, `VotedAbility1`, `VotedAbility2`
- `Kills`, `Deaths`, `LivesRemaining` (all uint8 — bandwidth optimization)
- `RespawnEndTime` (float, server-time)

### Replicated Character / Components
- `AProjectCompCharacter::CurrentWeapon` (TObjectPtr<AProjectCompHitscanWeapon>)
- `UProjectCompHealthComponent::CurrentHealth` (uint8, RepNotify `OnRep_CurrentHealth(uint8 OldHealth)`)

### Replicated Bomb Site (`AProjectCompBombSite`)
- `bIsPlanted` (RepNotify `OnRep_IsPlanted`), `bIsDetonated`, `bIsDefused`, `DetonationEndTime`, `PlantProgress`, `DefuseProgress`

### Replicated Barrier (`AProjectCompBarrier`)
- `OwnerTeam` (RepNotify), `bIsActive` (RepNotify)

### RPC Naming Convention (strict)
- **Client → Server:** `UFUNCTION(Server, Reliable, WithValidation)` — prefix `Server_`. Required for every authority-touching call. Pairs: `Server_X_Implementation` + `Server_X_Validate`.
  - `AProjectCompPlayerController::Server_SubmitAbilityVote(EAbilityType, EAbilityType)`
  - `AProjectCompPlayerController::Server_RequestAbilityCast(EAbilityType, FVector)`
  - `AProjectCompCharacter::Server_SetTriggerHeld(bool)`
  - `AProjectCompCharacter::Server_SetUseHeld(bool)`
- **Server → All (Multicast):**
  - `AProjectCompGameState::Multicast_OnAbilityCast(EAbilityType, FVector, ETeamID)` — Reliable
  - `AProjectCompGameState::Multicast_OnPlayerKilled(FString, FString, ETeamID, ETeamID)` — Unreliable
  - `AProjectCompHitscanWeapon::Multicast_OnFired(FVector_NetQuantize, FVector_NetQuantizeNormal, uint16, bool)` — Unreliable; uses `bNetUseOwnerRelevancy` so it only reaches viewers who consider the firer relevant; hit endpoint NEVER on wire.
- **Server-internal helpers (not RPCs):** prefix `Server_` (e.g. `Server_SetTeamID`, `Server_AddKill`, `Server_RecordTransformSnapshot`, `Server_GetValidatedAimRotation`, `Server_RewindTo`, `Server_RestoreFromRewind`). Caller is responsible for `HasAuthority()`.

## 4. DESIGN CONSTRAINTS & CODING STANDARDS

### Boilerplate Rule (UHT)
- Server RPCs MUST be declared `UFUNCTION(Server, Reliable, WithValidation)` and define BOTH `Server_X_Implementation(...)` and `Server_X_Validate(...)` — never just one. UE drops the connection on `_Validate` failure (treat as a free anti-cheat layer; return `false` on impossible inputs).
- Multicast: `UFUNCTION(NetMulticast, Reliable|Unreliable)`. Choose Unreliable for cosmetic-only effects (muzzle flash, killfeed) — kill-feed missing one frame is fine; bandwidth matters more.
- RepNotify: `UPROPERTY(ReplicatedUsing = OnRep_X)` + `UFUNCTION() void OnRep_X();` (or `OnRep_X(OldValue)`).
- Every `UPROPERTY(Replicated...)` must be added in `GetLifetimeReplicatedProps`.
- Custom CMC requires `FSavedMove_Character` subclass + `FNetworkPredictionData_Client_Character` subclass + override `GetPredictionData_Client` / `UpdateFromCompressedFlags` / `GetCompressedFlags`. Predicted input flags use `FLAG_Custom_0..3`.

### Performance Constraints
- **No hot-path allocations.** Examples already in the codebase to mirror:
  - `BombSiteOverlapCount` (counter) replaces `GetOverlappingActors` per call.
  - `CachedBombSites` (one-shot init scan) replaces `TActorIterator` per `NativeTick`.
  - `HistoryBuf[32]` ring buffer (static array) for lag-comp snapshots — no dynamic alloc.
  - `RespawnTimers TMap` keyed by PC (no per-event new-up).
- Replicate the smallest type that fits: uint8 for HP/kills/lives/deaths, uint16 for time-deciseconds, `FVector_NetQuantize`/`FVector_NetQuantizeNormal` for vectors/dirs. NEVER replicate a raw `float` if a quantized form works.
- Cast minimally: cache typed pointers (e.g. `AProjectCompGameState* GetTypedGameState() const`). No `Cast<>` in tick loops.
- HUD `NativeTick` pulls only — no allocation per frame, only text content updates.
- Weapon fire loop is tickrate-independent BUT bounded (`MaxShotsPerTick=3`) — never unbounded `while`.

### Security Rule (Server Authority — Absolute)
- **Zero client-side declaration of damage, speed, position, fire rate, or hit results.** All damage flows through `UProjectCompHealthComponent::Server_TakeDamage` on the server.
- All aim-driven traces use `Server_GetValidatedAimRotation()` — clamps yaw/pitch rate, absolute pitch, and writes back to controller. Never call `GetControlRotation()` directly on the server for hit detection.
- All Server RPCs include `_Validate` checks (alive, in `RoundActive`, weapon present, valid team, etc.) — silent reject on failure.
- Lag-comp rewind window clamped (`MaxRewindSeconds=0.25`) — a tampered client cannot rewind targets arbitrarily far.
- Cosmetic multicasts strip hitpoint locations (anti-ESP) — clients must reconstruct via their own trace from `(StartLoc, Direction, DistanceCm)`.
- `IsNetRelevantFor` override + `bNetUseOwnerRelevancy` on weapon limits packet exposure of distant pawns.
- Spawn-room bounds enforced server-side every tick for looming players (`EnforceLoomingPlayerBounds`).
- Friendly fire gated by `FriendlyFireScalar` (default `0.0`) — never read on client.

### Terminology
- The dead-player support mechanic is **"The Loom" / "looming"** — never "Ghost Commander."
- "Looming player" = dead player still active (drafting, casting, spectating teammates).
- "Permanent ghost" = dead AND `LivesRemaining<=0` — no respawn this round.

### Known Deferred Bugs (do not silently "fix" without confirming)
- P2 yaw issue on respawn.
- Defender barrier missing (barriers belong in play area near/inside bomb sites, NOT at spawn-room boundary).
- Mid-round respawn occasionally team-incorrect.
- No end-of-round repositioning.
