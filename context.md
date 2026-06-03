# PROJECT COMP - BLUEPRINT [MAX REPO CONTEXT]

## 1. CORE ARCHITECTURE
- **Genre:** 5v5 Tactical FPS (High Competitive Integrity / Anti-Cheat Focused)
- **Engine & Version:** UE 5.7 (`EngineAssociation: "5.7"`, module `ProjectComp` Runtime, deps: Engine/AIModule/UMG; plugins: ModelingToolsEditorMode (Editor), StateTree, GameplayStateTree)
- **Target Mechanics:** Low-poly/stylized graphics, high mechanical depth, strict server authority, high-polling rate processing (Wooting-optimized), precise local prediction.
- **Game Format:** 5v5 Attackers vs Defenders, bomb-plant objective + "Loom" dead-player support mechanic (NOT "Ghost Commander"). 2 lives/player/round (`MaxLives=2`). First to `RoundsToWinMatch=13` wins. Halftime team swap at round `HalftimeRoundNumber=8`.
- **Round Loop:** `LoomSelection` (mouse-locked Loom-ability draft — players FULLY frozen: no walk/jump/crouch) → `PreRound` (buy + positioning, barrier) → `RoundActive` (100s gunplay) → `PostRound` (5s killfeed) → repeat. Terminal: `MatchEnded`. Dev cheat `PauseTimer` freezes the phase countdown for testing (gated from shipping).
- **Ability System:** 9 abilities (AA/AC/AM/CC/CA/CM/MM/MA/MC) on 3 pillars (Aggro/Control/Midrange). Per-team shared cooldown pool [0..1], recharge rate `0.1/s`. Dead players draft + cast. Some Loom casts spawn world zones (CA → `AProjectCompFlashDome`, MC → `AProjectCompSoundDome`).
- **Loom usage (one cast per round, per player):** every player gets exactly ONE Loom ability cast per round (reset each round via `Server_ResetLoomUsage`). Because a player has 2 lives, they CHOOSE when to spend that single cast — during their looming window after their **1st death**, OR during their looming window after their **2nd (final) death** — but only one of the two, never both. Spending it after the 1st death means none is available after the 2nd, and vice-versa.
- **Utility / Grenade System:** 4 throwables — FLASH / INFO(scan) / VULNERABLE / SMOKE — 2 distinct per player, bought with credits (`CurrentCredits`), per-type cooldowns (NOT charges). Fully decoupled from the Loom. Standardized throw via `AProjectCompThrowableGrenadeBase`: **LMB** = CS2-style overhand (upward launch-pitch release + gravity arc + bounce-and-settle), **RMB** = standardized short underhand self-toss. Throw inherits the thrower's movement velocity (CS2-style: forward/back + jump only, strafe excluded; peak-velocity forgiveness window). Optional **global drop** = hold the slot key → minimap overlay → click to place at reduced range.

## 2. REPO TOPOGRAPHY (Key Classes)
- [Source/ProjectComp/ProjectComp.h](Source/ProjectComp/ProjectComp.h): `LogProjectComp` category; `#define ECC_DeadPawn ECC_GameTraceChannel2` (looming-pawn collision channel); `#define ECC_VisionBlock ECC_GameTraceChannel3` (smoke-only sight blocker — Flash LOS + Info scan trace it; bullets stay on `ECC_Visibility`). Both configured in DefaultEngine.ini.
- [ProjectCompGameTypes.h](Source/ProjectComp/ProjectCompGameTypes.h): `ETeamID` (Attackers=0/Defenders=1/Spectator=255), `ERoundPhase` (incl. `LoomSelection`), `EAbilityType`, `EHitZone` (Body/Head/Stomach/Limb), `EGrenadeType` (Flash/Info/Vulnerable/Smoke), `EThrowableFireMode`, `EThrowablePhase` (Projectile/Fused/Active), `FTeamLoadout`, `FKillFeedData`.
- [ProjectCompGameMode.h/.cpp](Source/ProjectComp/ProjectCompGameMode.h): Server-only match authority. Owns round FSM + phase-timer countdown (`Tick`), vote tally, respawn timers (`RespawnTimers TMap`), team assignment (`JoinSequence` alternation, `AssignedStarts TMap`), bomb-site cache, looming-player spawn-room bound enforcement (`EnforceLoomingPlayerBounds` w/ `AttackerSpawnRoomMaxY=-2400`, `DefenderSpawnRoomMinY=2400`), grenade purchase flow (`Server_BuyGrenade`). Dev cheat `bCheatFreezeTimer` / `Server_CheatToggleFreezeTimer` halts the countdown.
- [ProjectCompGameState.h/.cpp](Source/ProjectComp/ProjectCompGameState.h): Pure state replicator, zero logic. `PhaseTimeRemainingDs` (uint16 deciseconds wire), `PhaseTimeRemainingServerFloat` (server-only precise). `Teams TArray<FTeamLoadout>` size 2. `PendingKillFeedEvents` consumed by HUD.
- [ProjectCompPlayerState.h/.cpp](Source/ProjectComp/ProjectCompPlayerState.h): `TeamID`, `bIsAlive`, `VotedAbility1/2`, `Kills/Deaths` (uint8 wire), `LivesRemaining/MaxLives` (uint8), `RespawnEndTime` (server time). Economy/utility: `CurrentCredits` (uint16), `EquippedGrenadeTypes` (TArray<EGrenadeType>, max 2 distinct), `GrenadeCooldownEndTimes` (TArray<float> indexed by type).
- [ProjectCompPlayerController.h/.cpp](Source/ProjectComp/ProjectCompPlayerController.h): Input mapping contexts; phase-driven input mode (`ApplyInputMode`: GameOnly / GameAndUILocked for LoomSelection+settings / GameAndUIHoverFree for the buy menu — buy menu centers the cursor on open). Loom RPCs (`Server_SubmitAbilityVote`, `Server_RequestAbilityCast`); grenade/economy RPCs (`Server_UseGrenade`, `Server_GlobalDropGrenade`, `Server_BuyGrenade/Weapon/Armour`); global-drop minimap (`OpenGrenadePlacement`/`CloseGrenadePlacement`, spawns `MinimapWidgetClass`); dev-cheat execs (`PauseTimer`→`Server_CheatToggleFreezeTimer`, `EndRound`, `GiveCredits`, `UseNade`, `BuyNade` — all `#if !UE_BUILD_SHIPPING`).
- [ProjectCompCharacter.h/.cpp](Source/ProjectComp/ProjectCompCharacter.h): First-person tac-FPS pawn. Owns `FirstPersonMesh`, `FirstPersonCameraComponent`, `HealthComponent`, `GrenadeComponent`, replicated `CurrentWeapon`. Hosts anti-cheat aim-rate clamp (`MaxYawRateDegPerSec=1440`, `MaxPitchRateDegPerSec=720`, `MaxAbsolutePitch=89`), lag-comp ring buffer (`HistoryBuf[32]` = 500ms@64Hz; snapshot also records `Velocity` → `Server_GetWindowedThrowVelocity` for the grenade forgiveness window), recoil accumulators, bomb-site overlap counter, interact lock. Keybinds: **F** = interact/plant/defuse, **E/Q** = equip grenade slot 0/1 (LMB = throw, RMB = self-toss — both arm on press, throw on release; hold E/Q past `GrenadeGlobalHoldThreshold` = open global-drop minimap).
- [ProjectCompCharacterMovementComponent.h/.cpp](Source/ProjectComp/ProjectCompCharacterMovementComponent.h): Prediction-safe tactical walk (`FLAG_Custom_0`/`bWantsToWalk`, `TacticalWalkSpeed=320`), mouse airstrafe (`AirSteerStrength=1.0`, rotates horiz velocity by yaw delta), Valorant-style in-air horiz speed cap (`JumpedGroundSpeed` snapshot, `StandstillAirSpeedCap=250`), crouch-jump z scale `0.7`, `MaxWalkSpeedCrouched=200` (run `MaxWalkSpeed=560`). **WASD air control** = launch-speed-scaled (`MinAirControl=0.1`→`MaxAirControl=1.0` by `JumpedGroundSpeed/AirControlFullMovementSpeed`) × **inertia decay** (`ApexAirControlFraction=0.7` once the upward jump impulse is spent — driven by `Velocity.Z`, so no extra synced state). **LoomSelection freeze**: `GetMaxSpeed`→0, `DoJump`→false, `CanCrouchInCurrentState`→false while phase==LoomSelection (reads cached GameState phase). Custom `FSavedMove_ProjectComp` + `FNetworkPredictionData_Client_ProjectComp`.
- [ProjectCompHitscanWeapon.h/.cpp](Source/ProjectComp/ProjectCompHitscanWeapon.h): Server-authoritative hitscan. Tickrate-independent catch-up fire (`MaxShotsPerTick=3` cap), `FireInterval=0.1` (600 RPM), `BodyDamage=30`, head x4 / stomach x1.1 / limb x0.75, `MaxRange=10000cm`. `BoneZoneMap TMap<FName,EHitZone>`. Movement/crouch/air spread, deterministic `RecoilPattern TArray<FVector2D>` (X=pitch up, Y=yaw right), server+client mirror indices. Lag comp: `MaxRewindSeconds=0.25`, `ClientInterpolationDelay=0.05`. Cosmetic broadcast `Multicast_OnFired` uses `FVector_NetQuantize` + `FVector_NetQuantizeNormal` + uint16 distance; hit endpoint NEVER sent (anti-ESP).
- [ProjectCompHealthComponent.h/.cpp](Source/ProjectComp/ProjectCompHealthComponent.h): Replicated HP container. `CurrentHealth uint8` (ReplicatedUsing=`OnRep_CurrentHealth`), `MaxHealth=100`. `Server_TakeDamage`/`Server_ResetHealth`/`Server_Heal`. Delegates `OnHealthChanged`, `OnDeath`.
- [ProjectCompBombSite.h/.cpp](Source/ProjectComp/ProjectCompBombSite.h): Server-authoritative plant/defuse/detonate FSM. Replicated: `bIsPlanted` (`OnRep_IsPlanted`), `bIsDetonated`, `bIsDefused`, `DetonationEndTime`, `PlantProgress`, `DefuseProgress`. Tunables: `PlantDuration=4`, `DefuseDuration=6`, `DetonationDuration=35`, `Label="A"`, `PointValue=1`, `DefusePointValue=1`. Trigger (UBoxComponent) overlap counter.
- [ProjectCompBarrier.h/.cpp](Source/ProjectComp/ProjectCompBarrier.h): One-way visibility spawn barrier. `OwnerTeam` (ReplicatedUsing=`OnRep_OwnerTeam`), `bIsActive` (ReplicatedUsing=`OnRep_IsActive`). Owning team sees translucent material; opposing sees opaque. `Server_SwapOwnerTeam` at halftime.
- [ProjectCompPlayerStart.h/.cpp](Source/ProjectComp/ProjectCompPlayerStart.h): Team-tagged `APlayerStart`. `GetTeamID()` used by `ChoosePlayerStart_Implementation`.
- [ProjectCompHUDWidget.h/.cpp](Source/ProjectComp/ProjectCompHUDWidget.h): C++-only UMG. Valorant-style top-center scoreboard, HP corner, ability indicator, killfeed (`KillFeedRows` + parallel `KillFeedTimers`), bomb status box. Pulls per `NativeTick`. `CachedBombSites` one-shot at init (no per-frame `TActorIterator`).
- [ProjectCompHUD.h/.cpp](Source/ProjectComp/ProjectCompHUD.h): Thin AHUD owner; constructs/destructs `HUDWidget`.
- [ProjectCompCameraManager.h/.cpp](Source/ProjectComp/ProjectCompCameraManager.h): Basic pitch clamp camera manager.

### Grenade / Utility classes
- [ProjectCompGrenadeBase.h/.cpp](Source/ProjectComp/ProjectCompGrenadeBase.h): Abstract base for any grenade. `Server_Initialize(team,type)`, `Server_InitializeExtra(bAltFire, AimDir)` (throwables launch here), `Server_Detonate`.
- [ProjectCompThrowableGrenadeBase.h/.cpp](Source/ProjectComp/ProjectCompThrowableGrenadeBase.h): Standardized thrown projectile + 3-phase lifecycle (Projectile→Fused→Active). One throw shared by all four (only the activation effect differs). Tunables (single source of truth; BP-overridable): `PrimaryProjectileSpeed=2100`/`PrimaryGravity=980`/`PrimaryLaunchPitchBias=10°`; `AltProjectileSpeed=630`/`AltGravity=980`/`AltLaunchPitchBias=25°`; bounce `Restitution=0.18`/`TangentialFriction=0.45`/`RestSpeed=200`/`MaxBounces=12`; `FuseAfterImpact` (Flash 0 / Smoke 0.5 / Info+Vuln 1.0); movement inheritance `MovementInheritScalar=1.0`/`AltMovementInheritScalar=0.6`/`MovementInheritWindow=0.20`. `Server_DeployInPlace` = global-drop (skips flight). Server runs per-tick traces (unbreakable in flight); char-hit early-detonates.
- Grenade subclasses: [ProjectCompFlashGrenade](Source/ProjectComp/ProjectCompFlashGrenade.h) (instant pop, LOS+facing scaled flash, friend+foe), [ProjectCompInfoGrenade](Source/ProjectComp/ProjectCompInfoGrenade.h) (enemy-count scan, smoke-occluded), [ProjectCompVulnerableGrenade](Source/ProjectComp/ProjectCompVulnerableGrenade.h) (2× damage zone, breakable), [ProjectCompSmokeGrenade](Source/ProjectComp/ProjectCompSmokeGrenade.h) (vision-block sphere on `ECC_VisionBlock`, bullets pass through).
- Loom domes: [ProjectCompFlashDome](Source/ProjectComp/ProjectCompFlashDome.h) (CA ability), [ProjectCompSoundDome](Source/ProjectComp/ProjectCompSoundDome.h) (MC ability — audio-dampen zone; mixer still a stub).
- [ProjectCompGrenadeComponent.h/.cpp](Source/ProjectComp/ProjectCompGrenadeComponent.h): Character-attached spawn + cooldown arbiter. `GrenadeClasses` TMap<EGrenadeType,TSubclassOf> (set on BP_PCCharacter), per-type cooldowns, `Server_UseGrenade` / `Server_GlobalDropGrenade` (validate phase/alive/equipped/cooldown, spawn, stamp cooldown).
- [ProjectCompMinimapWidget.h/.cpp](Source/ProjectComp/ProjectCompMinimapWidget.h): C++ UMG tactical map. Dual-mode (`EMinimapMode` LoomCast / GrenadePlacement). Per-level `MapTexture` + `MapWorldMin`/`MapWorldMax` drive a linear pixel→world projection (requires an ORTHO top-down). Loom cast = focusable + click-to-cast; grenade placement = non-focusable, click-to-place / release-to-close, and clicks return a focus-preserving reply so the game viewport never loses focus (avoids flushing held movement keys).
- [ProjectCompMinimapCaptureActor.h/.cpp](Source/ProjectComp/ProjectCompMinimapCaptureActor.h): **Editor dev helper** (CallInEditor `CaptureToTarget`/`LogBounds`). Ortho `SceneCaptureComponent2D` (camera = child offset above a scene root → capturing never moves the actor) renders a top-down into a render target and logs the exact `MapWorldMin`/`Max`. Not for shipping — remove from levels before packaging.

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
- `CurrentCredits` (uint16), `EquippedGrenadeTypes` (TArray<EGrenadeType>), `GrenadeCooldownEndTimes` (TArray<float>, server-time end-stamps)

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
  - `AProjectCompPlayerController::Server_UseGrenade(EGrenadeType, bool bAltFire, FVector Origin, FVector Direction)`
  - `AProjectCompPlayerController::Server_GlobalDropGrenade(EGrenadeType, FVector Target)`
  - `AProjectCompPlayerController::Server_BuyGrenade/Server_BuyWeapon/Server_BuyArmour`
  - `AProjectCompCharacter::Server_SetTriggerHeld(bool)`
  - `AProjectCompCharacter::Server_SetUseHeld(bool)`
  - Dev cheats (`#if !UE_BUILD_SHIPPING`): `Server_CheatEndRound`, `Server_CheatAddCredits`, `Server_CheatToggleFreezeTimer`
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
- **SOUND is no longer a grenade** — it was re-homed as the MC Loom ability (`AProjectCompSoundDome`); **SMOKE** is the 4th grenade in its place. Grenade roster: FLASH / INFO / VULNERABLE / SMOKE.

### Known Deferred Bugs / WIP (do not silently "fix" without confirming)
- P2 yaw issue on respawn.
- Defender barrier missing (barriers belong in play area near/inside bomb sites, NOT at spawn-room boundary).
- Mid-round respawn occasionally team-incorrect.
- No end-of-round repositioning.
- **Global-drop minimap map texture is WIP** — `T_TopDown` is currently transparent (capture needs Final Color + Lighting show-flag off for an opaque unlit bake); orientation vs the widget's projection not yet calibrated.
- **Asset name typos** committed: `Content/Grenades/BP+SmokeGrenade.uasset` (should be `BP_`), `Content/Loom/UI/RT_Subsrate_TopDown.uasset` ("Subsrate"). Rename in-editor (leaves redirectors) when convenient.
- No Host/Join UI or OnlineSubsystem — multiplayer hosting is listen-server via launch args (`Map?listen`) / direct-IP connect only.
