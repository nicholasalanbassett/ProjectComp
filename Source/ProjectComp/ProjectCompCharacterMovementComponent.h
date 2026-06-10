// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "ProjectCompCharacterMovementComponent.generated.h"

/**
 * Custom CharacterMovementComponent.
 *
 * Adds three things on top of the base CMC, all designed for tac-FPS feel:
 *   1. Prediction-safe tactical-walk modifier (hold shift slows max speed).
 *   2. Subtle mouse airstrafe (rotates horizontal velocity by yaw delta in air).
 *   3. Valorant-style air speed cap (horizontal speed in air can't exceed the
 *      speed you launched with — air control redirects, never gains speed).
 *
 * Crouch is already prediction-safe in the base class (FLAG_WantsToCrouch).
 *
 * Tuning reference (every tweakable movement knob + where to edit it live):
 * Docs/MovementTuning.md
 */
UCLASS()
class UProjectCompCharacterMovementComponent : public UCharacterMovementComponent
{
    GENERATED_BODY()

public:
    UProjectCompCharacterMovementComponent();

    /** True while the player is holding the tactical-walk modifier. Set on the owning client; replicated to the server via FLAG_Custom_0 in compressed flags. */
    uint8 bWantsToWalk : 1;

    /** Standing speed when the walk modifier is held (cm/s). */
    UPROPERTY(EditDefaultsOnly, Category = "Character Movement: Walking")
    float TacticalWalkSpeed = 320.0f;

    /**
     * Per-frame fraction of mouse-yaw rotation applied to horizontal velocity
     * while airborne and holding movement. Subtle by design — Valorant is not
     * CS bhop. 0 = no airstrafe, 0.2 = light curve, 1.0 = CS-style.
     */
    UPROPERTY(EditDefaultsOnly, Category = "Character Movement: Jumping / Falling", meta = (ClampMin = "0.0", UIMin = "0.0"))
    float AirSteerStrength = 1.0f;

    /**
     * Floor for the in-air horizontal speed cap (cm/s). Even on a standstill
     * jump, the player can still reach this speed laterally via WASD air control
     * — this is what lets a stationary jumper drift sideways to jump-peek a
     * corner with just WASD (no mouse airstrafe needed). Running jumps are
     * unaffected (their cap = launch speed). Set to 0 for fully committed
     * standstill jumps (no lateral movement).
     */
    UPROPERTY(EditDefaultsOnly, Category = "Character Movement: Jumping / Falling", meta = (ClampMin = "0.0", UIMin = "0.0"))
    float StandstillAirSpeedCap = 250.0f;

    /**
     * WASD air-control authority scales LINEARLY with how fast you were moving
     * when you jumped (JumpedGroundSpeed). A standstill jump gets MinAirControl
     * (very little WASD steering); a jump at/above AirControlFullMovementSpeed
     * gets MaxAirControl (lots). So fast movers can redirect hard mid-air while a
     * standstill jump stays mostly committed. Driven each falling frame in
     * CalcVelocity (overrides the base AirControl).
     */
    UPROPERTY(EditDefaultsOnly, Category = "Character Movement: Jumping / Falling", meta = (ClampMin = "0.0", UIMin = "0.0", ClampMax = "1.0", UIMax = "1.0"))
    float MinAirControl = 0.1f;

    UPROPERTY(EditDefaultsOnly, Category = "Character Movement: Jumping / Falling", meta = (ClampMin = "0.0", UIMin = "0.0", ClampMax = "1.0", UIMax = "1.0"))
    float MaxAirControl = 1.0f;

    /** Launch ground speed (cm/s) at which air control reaches MaxAirControl;
     *  below this it lerps down toward MinAirControl. Roughly run speed. */
    UPROPERTY(EditDefaultsOnly, Category = "Character Movement: Jumping / Falling", meta = (ClampMin = "1.0", UIMin = "1.0"))
    float AirControlFullMovementSpeed = 500.0f;

    /**
     * Inertia decay: air control is at FULL (the speed-scaled value above) right
     * after launch while you're still rising hard, then fades to this fraction of
     * it once the upward jump impulse is spent (apex + descent). Models inertia —
     * you can adjust your launch, but commit as the jump carries on. Driven by
     * remaining vertical velocity (replay-safe, no synced timer). 1.0 = no decay
     * (control constant through the jump); lower = more committed mid-air.
     */
    UPROPERTY(EditDefaultsOnly, Category = "Character Movement: Jumping / Falling", meta = (ClampMin = "0.0", UIMin = "0.0", ClampMax = "1.0", UIMax = "1.0"))
    float ApexAirControlFraction = 0.7f;

    /**
     * Descent-only air-control decay. Once past the apex and falling, WASD air
     * control fades a SECOND time — from ApexAirControlFraction (at the apex)
     * down to this fraction by the time downward speed reaches JumpZVelocity (a
     * symmetric same-level landing); clamped here for longer drops off ledges.
     * Lets the fall feel more committed than the rise WITHOUT touching the rise.
     * Normally <= ApexAirControlFraction (set equal for the old flat-descent
     * behavior; higher would give MORE control while falling). Driven by
     * Velocity.Z, so it's replay-safe with no extra synced state.
     */
    UPROPERTY(EditDefaultsOnly, Category = "Character Movement: Jumping / Falling", meta = (ClampMin = "0.0", UIMin = "0.0", ClampMax = "1.0", UIMax = "1.0"))
    float FallAirControlFraction = 0.5f;

    /**
     * Scale applied to JumpZVelocity when jumping from a crouched state.
     * 0.7 ≈ a noticeably shorter hop while still clearing low cover.
     */
    UPROPERTY(EditDefaultsOnly, Category = "Character Movement: Jumping / Falling", meta = (ClampMin = "0.0", UIMin = "0.0", ClampMax = "1.0", UIMax = "1.0"))
    float CrouchJumpZScale = 0.7f;

    /**
     * Landing recovery — for a short window after touching down from the air, WASD
     * ground ACCELERATION is throttled so you can't instantly snap back to full
     * run/strafe speed on landing; it "spools up" over the window instead. Only
     * MaxAcceleration is cut — MaxWalkSpeed is untouched — so incoming jump momentum
     * is preserved and bunny-hopping still chains; a fresh standstill landing has to
     * rebuild speed. Re-armed on every landing.
     *
     * Duration of the throttle window (seconds). 0 disables the mechanic.
     */
    UPROPERTY(EditDefaultsOnly, Category = "Character Movement: Jumping / Falling", meta = (ClampMin = "0.0", UIMin = "0.0"))
    float LandingRecoveryDuration = 0.25f;

    /**
     * MaxAcceleration multiplier at the instant of landing; ramps linearly back to
     * 1.0 over LandingRecoveryDuration. 1.0 = no penalty; lower = a longer, heavier
     * "spool up" before you can accelerate / change direction at full responsiveness.
     */
    UPROPERTY(EditDefaultsOnly, Category = "Character Movement: Jumping / Falling", meta = (ClampMin = "0.0", UIMin = "0.0", ClampMax = "1.0", UIMax = "1.0"))
    float LandingAccelScale = 0.4f;

    virtual float GetMaxSpeed() const override;
    virtual float GetMaxAcceleration() const override;
    virtual void UpdateCharacterStateBeforeMovement(float DeltaSeconds) override;
    virtual bool DoJump(bool bReplayingMoves, float DeltaTime) override;
    virtual bool CanCrouchInCurrentState() const override;
    virtual void UpdateFromCompressedFlags(uint8 Flags) override;
    virtual class FNetworkPredictionData_Client* GetPredictionData_Client() const override;
    virtual void CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration) override;
    virtual void OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode) override;

    /** Control yaw on the previous CalcVelocity call. Diffed each frame to drive mouse airstrafe. */
    float PreviousControlYaw = 0.0f;

    /** Horizontal ground speed sampled the instant the character entered MOVE_Falling. Used as the in-air horizontal speed cap. */
    float JumpedGroundSpeed = 0.0f;

    /** Counts down from LandingRecoveryDuration after a landing; while > 0, ground
     *  acceleration is throttled (see GetMaxAcceleration). Ticked once per move in
     *  UpdateCharacterStateBeforeMovement and saved/restored for prediction replay. */
    float LandingRecoveryRemaining = 0.0f;

private:
    /**
     * Snapshot of PreviousControlYaw as it was at the START of the current move.
     * Captured at the top of CalcVelocity before we overwrite PreviousControlYaw,
     * so FSavedMove can record the pre-state needed for deterministic replay.
     */
    float LastMoveStartPreviousControlYaw = 0.0f;

    /** Snapshot of LandingRecoveryRemaining at the START of the current move, taken
     *  before it is decremented, so FSavedMove can record the pre-state needed for
     *  deterministic replay (mirrors LastMoveStartPreviousControlYaw). */
    float LastMoveStartLandingRecovery = 0.0f;

    /** Cached typed GameState, lazily resolved, so the per-move phase check in
     *  GetMaxSpeed / DoJump doesn't Cast<> every call. */
    mutable TWeakObjectPtr<class AProjectCompGameState> CachedGameState;

    /** True while players should be fully frozen in place (no walk/jump/crouch):
     *  the pre-match team-select lobby (None phase) and the LoomSelection draft —
     *  both mouse-locked menu states. Driven by the replicated phase, so server +
     *  owning client agree and movement prediction stays consistent. */
    bool IsFrozenPhase() const;

    friend class FSavedMove_ProjectComp;
};

class FSavedMove_ProjectComp : public FSavedMove_Character
{
public:
    using Super = FSavedMove_Character;

    uint8 bSavedWantsToWalk : 1;

    /** Pre-state of CMC->PreviousControlYaw at the start of this move (drives mouse-airstrafe replay). */
    float SavedPreviousControlYaw = 0.0f;

    /** CMC->JumpedGroundSpeed at the time this move was recorded (drives the air-speed cap on replay). */
    float SavedJumpedGroundSpeed = 0.0f;

    /** Pre-state of CMC->LandingRecoveryRemaining at the start of this move (drives the landing accel throttle on replay). */
    float SavedLandingRecoveryRemaining = 0.0f;

    FSavedMove_ProjectComp();

    virtual void Clear() override;
    virtual uint8 GetCompressedFlags() const override;
    virtual bool CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* InCharacter, float MaxDelta) const override;
    virtual void SetMoveFor(ACharacter* C, float InDeltaTime, FVector const& NewAccel, class FNetworkPredictionData_Client_Character& ClientData) override;
    virtual void PrepMoveFor(ACharacter* C) override;
};

class FNetworkPredictionData_Client_ProjectComp : public FNetworkPredictionData_Client_Character
{
public:
    using Super = FNetworkPredictionData_Client_Character;

    FNetworkPredictionData_Client_ProjectComp(const UCharacterMovementComponent& ClientMovement);

    virtual FSavedMovePtr AllocateNewMove() override;
};
