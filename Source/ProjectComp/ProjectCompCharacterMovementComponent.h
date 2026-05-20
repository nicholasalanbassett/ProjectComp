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
    float AirSteerStrength = 0.2f;

    /**
     * Floor for the in-air horizontal speed cap (cm/s). Even on a standstill
     * jump, the player can still reach this speed laterally — useful for
     * letting a stationary jumper nudge sideways to clear a ledge.
     * Set to 0 for fully committed standstill jumps (no lateral movement).
     */
    UPROPERTY(EditDefaultsOnly, Category = "Character Movement: Jumping / Falling", meta = (ClampMin = "0.0", UIMin = "0.0"))
    float StandstillAirSpeedCap = 50.0f;

    /**
     * Scale applied to JumpZVelocity when jumping from a crouched state.
     * 0.7 ≈ a noticeably shorter hop while still clearing low cover.
     */
    UPROPERTY(EditDefaultsOnly, Category = "Character Movement: Jumping / Falling", meta = (ClampMin = "0.0", UIMin = "0.0", ClampMax = "1.0", UIMax = "1.0"))
    float CrouchJumpZScale = 0.7f;

    virtual float GetMaxSpeed() const override;
    virtual bool DoJump(bool bReplayingMoves) override;
    virtual void UpdateFromCompressedFlags(uint8 Flags) override;
    virtual class FNetworkPredictionData_Client* GetPredictionData_Client() const override;
    virtual void CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration) override;
    virtual void OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode) override;

    /** Control yaw on the previous CalcVelocity call. Diffed each frame to drive mouse airstrafe. */
    float PreviousControlYaw = 0.0f;

    /** Horizontal ground speed sampled the instant the character entered MOVE_Falling. Used as the in-air horizontal speed cap. */
    float JumpedGroundSpeed = 0.0f;

private:
    /**
     * Snapshot of PreviousControlYaw as it was at the START of the current move.
     * Captured at the top of CalcVelocity before we overwrite PreviousControlYaw,
     * so FSavedMove can record the pre-state needed for deterministic replay.
     */
    float LastMoveStartPreviousControlYaw = 0.0f;

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
