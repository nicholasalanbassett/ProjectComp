// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompCharacterMovementComponent.h"
#include "GameFramework/Character.h"
#include "ProjectCompGameState.h"

bool UProjectCompCharacterMovementComponent::IsFrozenPhase() const
{
    if (!CachedGameState.IsValid())
    {
        if (const UWorld* World = GetWorld())
        {
            CachedGameState = World->GetGameState<AProjectCompGameState>();
        }
    }
    const AProjectCompGameState* GS = CachedGameState.Get();
    if (!GS)
    {
        return false;
    }
    // Frozen during the pre-match lobby (None) and the LoomSelection draft.
    const ERoundPhase Phase = GS->GetCurrentPhase();
    return Phase == ERoundPhase::None || Phase == ERoundPhase::LoomSelection;
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
    if (IsFrozenPhase())
    {
        return 0.0f;
    }

    float MaxSpeed = Super::GetMaxSpeed();

    if (bWantsToWalk && MovementMode == MOVE_Walking && !IsCrouching())
    {
        MaxSpeed = FMath::Min(MaxSpeed, TacticalWalkSpeed);
    }

    // Jump-fatigue SPEED penalty (ground only): sustained mini-jumping bleeds max
    // ground speed toward JumpFatigueSpeedScale, so a player can't bhop-spam into a
    // site at full run speed (the "bouncing head" entry). Only fatigue ABOVE a single
    // jump's worth (JumpFatiguePerJump) counts, so a lone jump keeps full speed; the
    // penalty grows as fatigue stacks and recovers as it bleeds off on the ground. The
    // reduced ground speed carries into the air via the launch-speed cap (JumpedGroundSpeed
    // is sampled at takeoff). Reads the predicted JumpFatigue, so it's replay-safe.
    if (MovementMode == MOVE_Walking && JumpFatigueSpeedScale < 1.0f && JumpFatigue > JumpFatiguePerJump)
    {
        const float Denom = FMath::Max(1.0f - JumpFatiguePerJump, KINDA_SMALL_NUMBER);
        const float SpeedFatigue = FMath::Clamp((JumpFatigue - JumpFatiguePerJump) / Denom, 0.0f, 1.0f);
        MaxSpeed *= FMath::Lerp(1.0f, JumpFatigueSpeedScale, SpeedFatigue);
    }

    return MaxSpeed;
}

// Landing recovery: throttle ground acceleration for a short window after touching
// down, ramping from LandingAccelScale back to full over LandingRecoveryDuration.
// Only acceleration is cut (NOT max speed), so jump momentum carries through a
// bunny-hop while a fresh standstill landing has to spool back up to full speed.
// Reads the predicted, save/restored LandingRecoveryRemaining → replay-deterministic.
float UProjectCompCharacterMovementComponent::GetMaxAcceleration() const
{
    const float BaseAccel = Super::GetMaxAcceleration();

    if (MovementMode == MOVE_Walking && LandingRecoveryRemaining > 0.0f && LandingRecoveryDuration > 0.0f)
    {
        const float ElapsedFrac = 1.0f - FMath::Clamp(LandingRecoveryRemaining / LandingRecoveryDuration, 0.0f, 1.0f);
        const float Scale = FMath::Lerp(LandingAccelScale, 1.0f, ElapsedFrac);
        return BaseAccel * Scale;
    }

    return BaseAccel;
}

// Tick the landing-recovery countdown once per move, BEFORE physics — so it's
// independent of CalcVelocity sub-stepping (which can run multiple times per move
// under heavy frame drops). Snapshot the pre-decrement value first so FSavedMove
// records the start-of-move state for a deterministic prediction replay.
void UProjectCompCharacterMovementComponent::UpdateCharacterStateBeforeMovement(float DeltaSeconds)
{
    LastMoveStartLandingRecovery = LandingRecoveryRemaining;
    if (LandingRecoveryRemaining > 0.0f)
    {
        LandingRecoveryRemaining = FMath::Max(0.0f, LandingRecoveryRemaining - DeltaSeconds);
    }

    // Jump fatigue bleeds off ONLY while on the ground — bunny-hops spend almost no
    // time grounded, so they keep stacking fatigue (lower and lower jumps). Snapshot
    // before the decay so the move's pre-state is recorded for prediction replay.
    LastMoveStartJumpFatigue = JumpFatigue;
    if (JumpFatigue > 0.0f && IsMovingOnGround() && JumpFatigueRecoverySeconds > KINDA_SMALL_NUMBER)
    {
        JumpFatigue = FMath::Max(0.0f, JumpFatigue - DeltaSeconds / JumpFatigueRecoverySeconds);
    }

    Super::UpdateCharacterStateBeforeMovement(DeltaSeconds);
}

// Crouch-jump impulse is a scaled version of the standing jump. The base
// implementation does `Velocity.Z = max(Velocity.Z, JumpZVelocity)`; we
// temporarily lower JumpZVelocity so the same prediction path runs on owning
// client, server, and replay with no extra state to sync.
bool UProjectCompCharacterMovementComponent::DoJump(bool bReplayingMoves, float DeltaTime)
{
    // No jumping during the LoomSelection draft — players are frozen in place.
    if (IsFrozenPhase())
    {
        return false;
    }

    // Combined jump-height scale: crouch-jump shrink × consecutive-jump fatigue.
    // Fatigue uses the level accumulated from PRIOR jumps, so the first jump of a
    // sequence is full height and rapid re-jumps (before recovering on the ground)
    // get progressively lower — Valorant-style anti-bhop. Both inputs are predicted
    // / replay-restored (bIsCrouched, JumpFatigue), so this stays deterministic.
    float Scale = 1.0f;
    if (CharacterOwner && CharacterOwner->bIsCrouched)
    {
        Scale *= CrouchJumpZScale;
    }
    Scale *= FMath::Lerp(1.0f, JumpFatigueMinScale, FMath::Clamp(JumpFatigue, 0.0f, 1.0f));

    bool bJumped;
    if (Scale < 1.0f)
    {
        const float SavedZ = JumpZVelocity;
        JumpZVelocity = SavedZ * Scale;
        bJumped = Super::DoJump(bReplayingMoves, DeltaTime);
        JumpZVelocity = SavedZ;
    }
    else
    {
        bJumped = Super::DoJump(bReplayingMoves, DeltaTime);
    }

    // Bank this jump's fatigue only if it actually fired. Bleeds off on the ground
    // (UpdateCharacterStateBeforeMovement).
    if (bJumped)
    {
        JumpFatigue = FMath::Min(JumpFatigue + JumpFatiguePerJump, 1.0f);
    }
    return bJumped;
}

// No crouching during the LoomSelection draft — players must be completely still.
// Gating here (rather than the input handler) keeps it prediction-safe: server and
// owning client both deny the crouch from the same replicated phase.
bool UProjectCompCharacterMovementComponent::CanCrouchInCurrentState() const
{
    if (IsFrozenPhase())
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

        // (2) Inertia decay, in two halves around the apex (both keyed off the
        //     remaining/incoming vertical speed vs JumpZVelocity, so the curve is
        //     symmetric and prediction-safe — reads Velocity.Z, not a synced timer):
        //       Rising  (Velocity.Z >= 0): full authority right after launch,
        //         fading DOWN to ApexAirControlFraction as the upward impulse is spent.
        //       Falling (Velocity.Z <  0): continues fading from ApexAirControlFraction
        //         down to FallAirControlFraction as downward speed builds, so the
        //         fall is more committed than the rise — without touching the rise.
        //     Both branches meet at ApexAirControlFraction when Velocity.Z == 0.
        float InertiaDecay;
        if (Velocity.Z >= 0.0f)
        {
            // RiseFrac = remaining fraction of the launch up-velocity (1 at launch
            // -> 0 at apex). Falls ~linearly with time during the ascent.
            const float RiseFrac = (JumpZVelocity > KINDA_SMALL_NUMBER)
                ? FMath::Clamp(Velocity.Z / JumpZVelocity, 0.0f, 1.0f) : 0.0f;
            InertiaDecay = FMath::Lerp(ApexAirControlFraction, 1.0f, RiseFrac);
        }
        else
        {
            // FallFrac = how far into the committed fall we are (0 at apex -> 1 once
            // downward speed reaches JumpZVelocity, i.e. a same-level landing).
            const float FallFrac = (JumpZVelocity > KINDA_SMALL_NUMBER)
                ? FMath::Clamp(-Velocity.Z / JumpZVelocity, 0.0f, 1.0f) : 0.0f;
            InertiaDecay = FMath::Lerp(ApexAirControlFraction, FallAirControlFraction, FallFrac);
        }

        AirControl = SpeedScaled * InertiaDecay;

        // Crouch-jump is a more committed hop: scale down (CrouchJumpAirControlScale)
        // the WASD air authority while crouched in the air. The player stays crouched
        // through a crouch-jump, so bIsCrouched marks it; it's predicted/replicated,
        // keeping this replay-safe. Mouse airstrafe (AirSteerStrength) is unaffected.
        if (CharacterOwner && CharacterOwner->bIsCrouched && CrouchJumpAirControlScale < 1.0f)
        {
            AirControl *= CrouchJumpAirControlScale;
        }
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

        // Landing from the air: arm the brief ground-accel throttle so the player
        // can't instantly resume full-speed running/strafing. Only MaxAcceleration
        // is affected (see GetMaxAcceleration), so bunny-hop momentum is preserved.
        // OnMovementModeChanged replays deterministically, so server + client agree.
        if (PreviousMovementMode == MOVE_Falling && LandingRecoveryDuration > 0.0f)
        {
            LandingRecoveryRemaining = LandingRecoveryDuration;
        }
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
    SavedLandingRecoveryRemaining = 0.0f;
    SavedJumpFatigue = 0.0f;
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
    // Don't merge across a change in the landing-recovery countdown — the accel
    // throttle differs, so a combined move would replay a different velocity.
    if (!FMath::IsNearlyEqual(SavedLandingRecoveryRemaining, Other->SavedLandingRecoveryRemaining, 0.01f))
    {
        return false;
    }
    // Likewise don't merge across a change in jump fatigue — a combined move could
    // replay a different jump height.
    if (!FMath::IsNearlyEqual(SavedJumpFatigue, Other->SavedJumpFatigue, 0.01f))
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
        SavedLandingRecoveryRemaining = CharMov->LastMoveStartLandingRecovery;
        SavedJumpFatigue = CharMov->LastMoveStartJumpFatigue;
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
        CharMov->LandingRecoveryRemaining = SavedLandingRecoveryRemaining;
        CharMov->JumpFatigue = SavedJumpFatigue;
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
