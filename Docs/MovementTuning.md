# Movement Tuning Reference

A complete map of every tweakable movement parameter — what it does, the current value, and **where to change it live (no C++ recompile)**.

The custom movement component (`UProjectCompCharacterMovementComponent`) is installed on the
character in C++ via `SetDefaultSubobjectClass`
([ProjectCompCharacter.cpp:33](../Source/ProjectComp/ProjectCompCharacter.cpp#L33)), so **every value
below is already editable in the character Blueprint** — the C++ values are just defaults; a Blueprint
override wins and persists across recompiles.

## How to tune live (no recompile)

1. Open the gameplay pawn: **`Content/FirstPerson/Blueprints/BP_FirstPersonCharacter`**
   (spawned by `BP_FirstPersonGameMode` as `DefaultPawnClass`; some older notes call it "BP_PCCharacter" — same asset).
2. Select the component / defaults named in the **"Where to edit"** column:
   - **CMC comp** = select the **CharacterMovement** component, then find the listed category in Details.
   - **Character defaults** = click the **Class Defaults** button in the BP toolbar, then find the listed category.
   - **Camera comp** = select the **First Person Camera** component.
3. Change the value and **restart PIE** — it applies with no compile. Overrides persist (save the BP).

> The `↑/↓` notes describe the effect of raising/lowering each value.

---

## A. Ground speed

| Knob | Value | Where to edit | Effect |
|---|---|---|---|
| `MaxWalkSpeed` | 560 | CMC comp → *Walking* | Run speed. ↑ faster base movement. |
| `TacticalWalkSpeed` | 320 | CMC comp → *Character Movement: Walking* | Speed while **Shift** held (silent walk). Only applies on ground, standing. |
| `MaxWalkSpeedCrouched` | 150 | CMC comp → *Walking* | Crouch-move speed. |
| `MaxAcceleration` | 6000 | CMC comp → *Walking* | How fast you reach top speed (~0.1s now). ↑ snappier starts/counter-strafe; ↓ floatier ramp-up. |

## B. Stopping / counter-strafe (braking)

| Knob | Value | Where to edit | Effect |
|---|---|---|---|
| `BrakingDecelerationWalking` | 10000 | CMC comp → *Walking* | Dead-stop force on key release. ↑ instant stop; ↓ more slide/peek-out. **Primary counter-strafe knob.** |
| `GroundFriction` | 10 | CMC comp → *Walking* | Grip while changing direction. ↑ snappier; ↓ icier. |
| `BrakingFrictionFactor` | 1.0 | CMC comp → *Walking* | Scales the friction used for braking. **Caveat:** `bUseSeparateBrakingFriction` is **false** by default, so braking actually uses `GroundFriction` (not `BrakingFriction`). Flip that bool first if you want an independent braking grip. |

## C. Jump & gravity (vertical arc)

| Knob | Value | Where to edit | Effect |
|---|---|---|---|
| `JumpZVelocity` | 600 | CMC comp → *Jumping / Falling* | Jump impulse = height. ↑ higher hop. |
| `GravityScale` | 2.0 | CMC comp → *Jumping / Falling* | Fall rate. ↑ snappier/heavier arc, less hang time; ↓ floatier. (Now: apex ≈ 92 cm, air time ≈ 0.61 s.) |
| `CrouchJumpZScale` | 0.7 | CMC comp → *Jumping / Falling* | Crouch-jump height as a fraction of a standing jump. |
| `BrakingDecelerationFalling` | 0.0 | CMC comp → *Jumping / Falling* | Air drag. 0 = momentum preserved; ↑ bleeds horizontal speed mid-air. |
| `JumpMaxHoldTime` | 0 *(engine default, unset)* | CMC comp → *Jumping / Falling* | Raise for variable jump height (hold space = go higher). Off today. |
| `JumpMaxCount` | 1 *(engine default, unset)* | CMC comp → *Jumping / Falling* | Raise for multi-jump (double jump). Off today. |

## D. Air control & airstrafe (horizontal, in-air)

**System note:** in-air horizontal speed is hard-capped to `max(launch speed, StandstillAirSpeedCap)`
in `CalcVelocity` — this is the anti-bhop / Valorant model. Air control and airstrafe **redirect**
velocity; they cannot push past that cap.

| Knob | Value | Where to edit | Effect |
|---|---|---|---|
| `AirSteerStrength` | 1.0 | CMC comp → *Jumping / Falling* | Fraction of mouse-yaw applied to velocity mid-air. 0 = no mouse airstrafe; 1.0 = full CS-style curve. |
| `StandstillAirSpeedCap` | 250 | CMC comp → *Jumping / Falling* | Lateral drift floor on a standstill jump (lets you WASD-drift to jump-peek). 0 = fully committed standstill jumps. |
| `MinAirControl` | 0.1 | CMC comp → *Jumping / Falling* | WASD steering authority for a **standstill** jump. |
| `MaxAirControl` | 0.6 | CMC comp → *Jumping / Falling* | WASD steering authority for a **full-run** jump. Lower = more committed air jumps (less mid-air WASD redirect). |
| `AirControlFullMovementSpeed` | 500 | CMC comp → *Jumping / Falling* | Launch speed at which air control reaches `MaxAirControl`; below it lerps toward `MinAirControl`. |
| `ApexAirControlFraction` | 0.7 | CMC comp → *Jumping / Falling* | Air control **at the apex** (once the upward impulse is spent). The rise fades from full (1.0) down to this; the fall continues from this down to `FallAirControlFraction`. |
| `FallAirControlFraction` | 0.5 | CMC comp → *Jumping / Falling* | WASD control once **falling**: fades from `ApexAirControlFraction` at the apex down to this by the time drop speed reaches `JumpZVelocity` (a same-level landing), clamped lower for longer drops off ledges. ↓ = more committed fall, rise untouched. Set equal to `ApexAirControlFraction` for the old flat-descent behavior. |
| `CrouchJumpAirControlScale` | 0.5 | CMC comp → *Jumping / Falling* | Multiplier on WASD air control **while crouched in the air** (crouch-jump). 0.5 = half a standing jump's air authority (more committed hop); 1.0 = no difference. Does not touch mouse airstrafe (`AirSteerStrength`). |
| `AirControl` *(engine)* | 0.35 | CMC comp → *Jumping / Falling* | **Mostly inert** — `CalcVelocity` overwrites this every falling frame from the Min/Max system above. Tune the custom knobs, not this. |
| `AirControlBoostMultiplier` | 1.0 | CMC comp → *Jumping / Falling* | Engine low-speed steering boost; 1.0 disables it. Leave at 1.0 unless you want the engine boost back. |

## D2. Landing recovery (post-jump WASD throttle)

For a short window after landing from the air, **`MaxAcceleration`** (not max speed) is throttled and ramps back to full over the window — so you can't instantly snap to full run/strafe speed on touchdown; you "spool up". Because only acceleration is cut, **incoming jump momentum is preserved → bunny-hopping still chains**; only fresh ground acceleration is penalized. Re-armed on every landing (`MOVE_Falling → MOVE_Walking`).

| Knob | Value | Where to edit | Effect |
|---|---|---|---|
| `LandingRecoveryDuration` | 0 (off) | CMC comp → *Jumping / Falling* | Length of the throttle window (s). ↑ longer spool-up after landing. **Ships at 0 (disabled) for smooth momentum bhop** — raise (~0.25) to re-enable. |
| `LandingAccelScale` | 0.4 | CMC comp → *Jumping / Falling* | `MaxAcceleration` multiplier at the instant of landing, ramping to 1.0 over the window. ↓ = heavier/longer penalty (slower to rebuild speed); 1.0 = no penalty. |

> Triggers on **any** air→ground landing, including stepping off a small ledge. Defaults are short/slight so curb-steps are barely noticeable; gating it to real jumps only is a small follow-up code change.

## D3. Jump fatigue (Valorant-style anti-bhop)

Consecutive jumps get **progressively lower AND slower** until you recover on the ground. `JumpFatigue` is a 0..1 accumulator: each jump adds `JumpFatiguePerJump` (capped at 1), and while **grounded** it bleeds off over `JumpFatigueRecoverySeconds`. Two penalties read it: **jump height** scales from full (fatigue 0) down to `JumpFatigueMinScale` (fatigue 1), and **ground speed** scales toward `JumpFatigueSpeedScale` for fatigue accumulated **beyond a single jump** (so a lone jump keeps full speed, but sustained mini-jump/bhop spam bleeds your run speed — no full-speed "bouncing head" site entries). Recovery is ground-only, so bunny-hops (almost no ground contact) keep stacking → each hop is lower *and* slower. Together with the landing-recovery accel throttle (D2), this is the full Valorant fatigue model (height + speed).

| Knob | Value | Where to edit | Effect |
|---|---|---|---|
| `JumpFatiguePerJump` | 0 (off) | CMC comp → *Jumping / Falling* | Fatigue added per jump (0..1). The penalty uses fatigue from **prior** jumps, so the 1st jump is full height/speed. **Ships at 0 (disabled) for smooth momentum bhop** — raise (~0.45) to re-enable both height & speed fatigue. Also acts as the "one jump's worth" threshold below which there is no speed penalty. |
| `JumpFatigueMinScale` | 0.5 | CMC comp → *Jumping / Falling* | Jump-**height** multiplier at full fatigue. 1.0 = no penalty; 0.5 ≈ half-height hop when spammed. |
| `JumpFatigueSpeedScale` | 0.8 | CMC comp → *Jumping / Falling* | Ground-**speed** multiplier at full fatigue, applied only to fatigue **beyond one jump**. 0.8 ≈ Valorant's gentle ~20% per-landing loss — sustained spam bleeds to ~80% (low+slightly-slow hops), but a quick bhop keeps near-full momentum. 1.0 = no speed penalty; lower = stricter. |
| `JumpFatigueRecoverySeconds` | 0.4 | CMC comp → *Jumping / Falling* | Grounded seconds to bleed off 1.0 of fatigue (both penalties). 0.4 = fast "resume full speed" recovery, and fast enough that the between-hop decay drops you back under the speed threshold so a quick 1–2-hop bhop maintains momentum. ↑ = harsher / less momentum-friendly. |

> **Ships DISABLED** (`JumpFatiguePerJump = 0`, `LandingRecoveryDuration = 0`): bhop is smooth and momentum-preserving — jumps keep full height and horizontal speed is maintained, but the air-speed cap still holds you at launch speed so there is **no CS-style speed gain**.
>
> To re-enable anti-bhop fatigue, raise `JumpFatiguePerJump` (~0.45): jumps then shrink, and (above one jump's worth of fatigue) speed bleeds toward `JumpFatigueSpeedScale`, recovering over `JumpFatigueRecoverySeconds`. **To shrink the bounce height without slowing momentum** (kill the "bouncing head" look while keeping a fluid bhop), raise `JumpFatiguePerJump` but set `JumpFatigueSpeedScale = 1.0`.

## E. Crouch capsule & crouch-camera feel

| Knob | Value | Where to edit | Effect |
|---|---|---|---|
| `CrouchedHalfHeight` | 48 | CMC comp → *Walking* (`Crouched Half Height`) | How far the capsule (and camera) drops when crouched. |
| `CrouchInterpSpeed` | 10 | Character defaults → *Camera* | Crouch camera ease speed (`FInterpTo`). ↑ snappier crouch view; ↓ smoother/slower. ≈10 ≈ 0.2s. |
| `StandingCameraZ` | 64 | Character defaults → *Camera* | Standing eye height (relative to capsule center). |
| Capsule size | r 34 × hh 96 | Capsule comp / ctor | Standing hitbox & height. *Note:* the ctor inits 55×96 then resets radius to 34 — **34** is the live value. |

> **Vestigial — ignore:** `CrouchEyeHeight` / `DefaultEyeHeight` are set in the ctor but are **not**
> `UPROPERTY`s and are never read; the crouch camera uses the `CrouchVisualAlpha` / `CrouchInterpSpeed`
> path instead. (Cleanup candidate, out of scope.)

## F. Camera / view

| Knob | Value | Where to edit | Effect |
|---|---|---|---|
| `FirstPersonFieldOfView` | 90 | Camera comp | FOV. Affects perceived speed. (70 caused motion sickness in playtests; 90 is the tac-FPS norm.) |
| `FirstPersonScale` | 0.6 | Camera comp | FP-arms render scale (cosmetic, not movement). |

## G. Turn-rate caps (bound the airstrafe ceiling; also anti-cheat)

| Knob | Value | Where to edit | Effect |
|---|---|---|---|
| `MaxYawRateDegPerSec` | 1440 | Character defaults → *AntiCheat* | Server-accepted max horizontal turn rate → caps how hard mouse-airstrafe can curve. ↓ to soften the flick/airstrafe ceiling. |
| `MaxPitchRateDegPerSec` | 720 | Character defaults → *AntiCheat* | Max vertical look rate. |
| `MaxAbsolutePitch` | 89 | Character defaults → *AntiCheat* | Up/down look clamp (degrees). |

## H. Netcode / prediction (feel under latency — change with care)

| Knob | Value | Where to edit | Effect |
|---|---|---|---|
| `NetUpdateFrequency` / `MinNetUpdateFrequency` | 64 / 30 | Character ctor ([:42-43](../Source/ProjectComp/ProjectCompCharacter.cpp#L42-L43)) | Replication rate of this pawn. |
| `NetServerMaxTickRate` | 64 | `Config/DefaultEngine.ini` | Server sim/tick rate. |
| `ClientNetSendMoveDeltaTime` (+ throttled/stationary) | 0.0166 / 0.0222 | `Config/DefaultEngine.ini` | Client→server move-send cadence. |
| `NetworkSmoothingMode` | engine default | CMC comp → *Character Movement (Networking)* | Visual smoothing of **remote** pawns (not local feel). |

## I. Hard-coded gating (NOT tuning dials — listed so they're not mistaken for bugs)

- **Frozen phases:** speed/jump/crouch are forced off during `None` + `LoomSelection`
  (`GetMaxSpeed`→0, `DoJump`→false, `CanCrouchInCurrentState`→false).
- **Interact lock:** `MaxWalkSpeed` / `MaxWalkSpeedCrouched` are forced to 0 during plant/defuse, then
  restored from the values cached at `BeginPlay`. Because the cache reads the **live** CMC values
  ([ProjectCompCharacter.cpp:1766-1767](../Source/ProjectComp/ProjectCompCharacter.cpp#L1766-L1767)),
  a Blueprint override of `MaxWalkSpeed` survives the restore — your edit sticks.

## J. Untouched engine defaults you may also want

All live in the **CharacterMovement** component panel:

- `MaxStepHeight` (45) — step-up height over small ledges.
- `WalkableFloorAngle` (~44.8°) — steepest slope you can walk before sliding.
- `PerchRadiusThreshold` / ledge-check settings — edge teeter behavior.
- `FallingLateralFriction` (0) — sideways drag while airborne (separate from air control).

---

## Quick recipes

- **Slidier stops (more peek-out):** lower `BrakingDecelerationWalking` (e.g. 10000 → 2000) and/or `GroundFriction`.
- **Floatier jump / more hang time:** lower `GravityScale` (2.0 → 1.5). Keep `JumpZVelocity` for height, gravity for hang time.
- **More committed jumps (less air steering):** lower `MaxAirControl`, `AirSteerStrength`, and `StandstillAirSpeedCap`.
- **Less WASD control specifically while falling (after the apex):** lower `FallAirControlFraction` (leaves the rise untouched).
- **Weightier landings (no instant re-acceleration, bhop still works):** lower `LandingAccelScale` and/or raise `LandingRecoveryDuration`. Set `LandingRecoveryDuration = 0` to remove it.
- **Faster overall pace:** raise `MaxWalkSpeed`; raise `MaxAcceleration` to keep starts snappy at the higher top speed.
- **Snappier crouch view:** raise `CrouchInterpSpeed` (10 → 18).
