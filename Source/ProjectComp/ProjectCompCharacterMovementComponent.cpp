// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompCharacterMovementComponent.h"
#include "GameFramework/Character.h"
#include "ProjectCompGameState.h"

bool UProjectCompCharacterMovementComponent::IsLoomSelectionPhase() const
{
    if (!CachedGameState.IsValid())
    {
        if (const UWorld* World = GetWorld())
        {
            CachedGameState = World->GetGameState<AProjectCompGameState>();
        }
    }
    const AProjectCompGameState* GS = CachedGameState.Get();
    return GS && GS->GetCurrentPhase() == ERoundPhase::LoomSelection;
}

UProjectCompCharacterMovementComponent::UProjectCompCharacterMovementComponent()
{
    bWantsToWalk = 0;

    // AirControl is NOT a fixed value — CalcVelocity drives it each falling frame,
    // scaling WASD air authority linearly with launch speed (Min/MaxAirControl).
    // This initial value only matters before the first falling frame.
    AirControl = MaxAirControl;
}

// Clamp the engine's MaxSpeed down to TacticalWalkSpeed while the walk
// modifier is held — but only while standing on the ground. Crouch speed
// is owned by MaxWalkSpeedCrouched on the base class and naturally caps below.
float UProjectCompCharacterMovementComponent::GetMaxSpeed() const
{
    // Frozen in place during the LoomSelection draft — no walking.
    if (IsLoomSelectionPhase())
    {
        return 0.0f;
    }

    const float BaseMax = Super::GetMaxSpeed();

    if (bWantsToWalk && MovementMode == MOVE_Walking && !IsCrouching())
    {
        return FMath::Min(BaseMax, TacticalWalkSpeed);
    }

    return BaseMax;
}

// Crouch-jump impulse is a scaled version of the standing jump. The base
// implementation does `Velocity.Z = max(Velocity.Z, JumpZVelocity)`; we
// temporarily lower JumpZVelocity so the same prediction path runs on owning
// client, server, and replay with no extra state to sync.
bool UProjectCompCharacterMovementComponent::DoJump(bool bReplayingMoves, float DeltaTime)
{
    // No jumping during the LoomSelection draft — players are frozen in place.
    if (IsLoomSelectionPhase())
    {
        return false;
    }

    if (CharacterOwner && CharacterOwner->bIsCrouched && CrouchJumpZScale < 1.0f)
    {
        const float SavedZ = JumpZVelocity;
        JumpZVelocity = SavedZ * CrouchJumpZScale;
        const bool bJumped = Super::DoJump(bReplayingMoves, DeltaTime);
        JumpZVelocity = SavedZ;
        return bJumped;
    }
    return Super::DoJump(bReplayingMoves, DeltaTime);
}

// No crouching during the LoomSelection draft — players must be completely still.
// Gating here (rather than the input handler) keeps it prediction-safe: server and
// owning client both deny the crouch from the same replicated phase.
bool UProjectCompCharacterMovementComponent::CanCrouchInCurrentState() const
{
    if (IsLoomSelectionPhase())
    {
        return false;
    }
    return Super::CanCrouchInCurrentState();
}

// Server replay: rebuild bWantsToWalk from the compressed flag the client sent.
void UProjectCompCharacterMovementComponent::UpdateFromCompressedFlags(uint8 Flags)
{
    Super::UpdateFromCompressedFlags(Flags);

    bWantsToWalk = (Flags & FSavedMove_Character::FLAG_Custom_0) != 0;
}

// Two custom behaviors layer on top of the engine's velocity integration:
//
//   (1) Mouse airstrafe — BEFORE Super. Rotates the existing horizontal
//       velocity by a small fraction of the player's mouse-yaw delta this
//       frame. Magnitude unchanged; just a redirect.
//
//   (2) Air speed cap — AFTER Super. Clamps horizontal speed to the speed
//       the character launched with (with a small floor). Means AirControl
//       acts as a redirect rate, not a way to accelerate beyond your jump
//       envelope. This is the Valorant model.
void UProjectCompCharacterMovementComponent::CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration)
{
    if (CharacterOwner)
    {
        const float CurrentYaw = CharacterOwner->GetControlRotation().Yaw;

        // Snapshot the START-of-move yaw so FSavedMove can record the pre-state.
        // SetMoveFor runs AFTER this function and would otherwise see the
        // already-updated value, causing zero-delta replays.
        LastMoveStartPreviousControlYaw = PreviousControlYaw;

        if (IsFalling() && !Acceleration.IsNearlyZero() && AirSteerStrength > 0.0f)
        {
            const float DeltaYaw = FMath::FindDeltaAngleDegrees(PreviousControlYaw, CurrentYaw) * AirSteerStrength;
            if (!FMath::IsNearlyZero(DeltaYaw))
            {
                const FRotator YawRot(0.0f, DeltaYaw, 0.0f);
                Velocity = YawRot.RotateVector(Velocity);
            }
        }

        PreviousControlYaw = CurrentYaw;
    }

    // WASD air-control authority = launch-speed ceiling × inertia decay. Set
    // before Super, which consumes AirControl for the falling lateral accel.
    // Both inputs (JumpedGroundSpeed, Velocity.Z) are replay-restored, so this is
    // deterministic for network prediction with no extra saved state.
    if (IsFalling() && AirControlFullMovementSpeed > 0.0f)
    {
        // (1) Launch-speed ceiling: standstill jump → MinAirControl (little
        //     steering); full-run jump → MaxAirControl (lots).
        const float SpeedAlpha = FMath::Clamp(JumpedGroundSpeed / AirControlFullMovementSpeed, 0.0f, 1.0f);
        const float SpeedScaled = FMath::Lerp(MinAirControl, MaxAirControl, SpeedAlpha);

        // (2) Inertia decay: full authority right after launch (still rising
        //     hard), fading to ApexAirControlFraction once the upward impulse is
        //     spent (apex + descent). RiseFrac = remaining fraction of the launch
        //     up-velocity; it falls ~linearly with time during the ascent, so
        //     control "decays as time passes" but stays physically grounded and
        //     prediction-safe (reads Velocity.Z, not a synced timer).
        const float RiseFrac = (JumpZVelocity > KINDA_SMALL_NUMBER)
            ? FMath::Clamp(Velocity.Z / JumpZVelocity, 0.0f, 1.0f) : 0.0f;
        const float InertiaDecay = FMath::Lerp(ApexAirControlFraction, 1.0f, RiseFrac);

        AirControl = SpeedScaled * InertiaDecay;
    }

    Super::CalcVelocity(DeltaTime, Friction, bFluid, BrakingDeceleration);

    if (IsFalling())
    {
        const float Cap = FMath::Max(JumpedGroundSpeed, StandstillAirSpeedCap);
        FVector HorizVel(Velocity.X, Velocity.Y, 0.0f);
        const float HorizSpeed = HorizVel.Size();
        if (HorizSpeed > Cap && HorizSpeed > KINDA_SMALL_NUMBER)
        {
            HorizVel *= (Cap / HorizSpeed);
            Velocity.X = HorizVel.X;
            Velocity.Y = HorizVel.Y;
        }
    }
}

// Sample the launch speed on the ground->falling transition. This becomes the
// air-speed cap until we land. Cleared back to 0 when we re-enter walking.
//
// Server-side we clamp the captured speed to GetMaxSpeed() (+5% headroom for
// slope/ramp edges) so a tampered client can't report an inflated launch speed
// and use it as a higher in-air cap. Honest clients always satisfy the clamp
// because their CalcVelocity respects GetMaxSpeed itself.
void UProjectCompCharacterMovementComponent::OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode)
{
    Super::OnMovementModeChanged(PreviousMovementMode, PreviousCustomMode);

    if (MovementMode == MOVE_Falling && PreviousMovementMode != MOVE_Falling)
    {
        const FVector HorizVel(Velocity.X, Velocity.Y, 0.0f);
        const float MeasuredSpeed = HorizVel.Size();

        if (CharacterOwner && CharacterOwner->HasAuthority())
        {
            const float ServerCap = GetMaxSpeed() * 1.05f;
            JumpedGroundSpeed = FMath::Min(MeasuredSpeed, ServerCap);
        }
        else
        {
            JumpedGroundSpeed = MeasuredSpeed;
        }
    }
    else if (MovementMode == MOVE_Walking)
    {
        JumpedGroundSpeed = 0.0f;
    }
}

FNetworkPredictionData_Client* UProjectCompCharacterMovementComponent::GetPredictionData_Client() const
{
    if (ClientPredictionData == nullptr)
    {
        UProjectCompCharacterMovementComponent* MutableThis = const_cast<UProjectCompCharacterMovementComponent*>(this);
        MutableThis->ClientPredictionData = new FNetworkPredictionData_Client_ProjectComp(*this);
    }
    return ClientPredictionData;
}

// =====================================================================
// FSavedMove_ProjectComp — stores per-tick input snapshot for the network
// prediction system. We extend FSavedMove_Character with bSavedWantsToWalk
// so the server can replay client moves with the same walk-modifier state.
// =====================================================================

FSavedMove_ProjectComp::FSavedMove_ProjectComp()
    : bSavedWantsToWalk(0)
{
}

void FSavedMove_ProjectComp::Clear()
{
    Super::Clear();
    bSavedWantsToWalk = 0;
    SavedPreviousControlYaw = 0.0f;
    SavedJumpedGroundSpeed = 0.0f;
}

// Packs bWantsToWalk into FLAG_Custom_0 for transport.
uint8 FSavedMove_ProjectComp::GetCompressedFlags() const
{
    uint8 Flags = Super::GetCompressedFlags();
    if (bSavedWantsToWalk)
    {
        Flags |= FLAG_Custom_0;
    }
    return Flags;
}

// Two adjacent moves can only merge into one packet if all custom replay state
// matches; otherwise the velocity outcome would differ on replay.
bool FSavedMove_ProjectComp::CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* InCharacter, float MaxDelta) const
{
    const FSavedMove_ProjectComp* Other = static_cast<FSavedMove_ProjectComp*>(NewMove.Get());
    if (bSavedWantsToWalk != Other->bSavedWantsToWalk)
    {
        return false;
    }
    if (!FMath::IsNearlyEqual(SavedJumpedGroundSpeed, Other->SavedJumpedGroundSpeed, 1.0f))
    {
        return false;
    }
    if (!FMath::IsNearlyEqual(SavedPreviousControlYaw, Other->SavedPreviousControlYaw, 0.5f))
    {
        return false;
    }
    return Super::CanCombineWith(NewMove, InCharacter, MaxDelta);
}

// Snapshot CMC -> SavedMove (called as the client records the move, AFTER the
// move has been simulated). We save the PRE-state for fields the engine has
// already mutated by this point (PreviousControlYaw -> LastMoveStartPreviousControlYaw)
// and the current value for fields stable across the move (JumpedGroundSpeed,
// bWantsToWalk).
void FSavedMove_ProjectComp::SetMoveFor(ACharacter* C, float InDeltaTime, FVector const& NewAccel, FNetworkPredictionData_Client_Character& ClientData)
{
    Super::SetMoveFor(C, InDeltaTime, NewAccel, ClientData);

    if (const UProjectCompCharacterMovementComponent* CharMov = Cast<UProjectCompCharacterMovementComponent>(C->GetCharacterMovement()))
    {
        bSavedWantsToWalk = CharMov->bWantsToWalk;
        SavedJumpedGroundSpeed = CharMov->JumpedGroundSpeed;
        SavedPreviousControlYaw = CharMov->LastMoveStartPreviousControlYaw;
    }
}

// Restore SavedMove -> CMC (called BEFORE the move runs during a server
// correction replay). Sets PreviousControlYaw back to its pre-move value
// so CalcVelocity computes the same yaw delta it computed originally.
void FSavedMove_ProjectComp::PrepMoveFor(ACharacter* C)
{
    Super::PrepMoveFor(C);

    if (UProjectCompCharacterMovementComponent* CharMov = Cast<UProjectCompCharacterMovementComponent>(C->GetCharacterMovement()))
    {
        CharMov->bWantsToWalk = bSavedWantsToWalk;
        CharMov->JumpedGroundSpeed = SavedJumpedGroundSpeed;
        CharMov->PreviousControlYaw = SavedPreviousControlYaw;
    }
}

// =====================================================================
// FNetworkPredictionData_Client_ProjectComp — factory that produces our
// custom SavedMove type instead of the base FSavedMove_Character.
// =====================================================================

FNetworkPredictionData_Client_ProjectComp::FNetworkPredictionData_Client_ProjectComp(const UCharacterMovementComponent& ClientMovement)
    : Super(ClientMovement)
{
}

FSavedMovePtr FNetworkPredictionData_Client_ProjectComp::AllocateNewMove()
{
    return MakeShared<FSavedMove_ProjectComp>();
}
