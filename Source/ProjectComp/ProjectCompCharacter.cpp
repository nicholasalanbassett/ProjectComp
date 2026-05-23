// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompCharacter.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "InputActionValue.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/DamageEvents.h"
#include "Net/UnrealNetwork.h"
#include "ProjectComp.h"
#include "ProjectCompCharacterMovementComponent.h"
#include "ProjectCompHealthComponent.h"
#include "ProjectCompBombSite.h"
#include "ProjectCompHitscanWeapon.h"
#include "ProjectCompPlayerController.h"
#include "ProjectCompPlayerState.h"
#include "ProjectCompGameState.h"
#include "ProjectCompGameMode.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"

AProjectCompCharacter::AProjectCompCharacter(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer.SetDefaultSubobjectClass<UProjectCompCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
    // Tick is event-driven (enabled only during a crouch camera transition).
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;

    // Align character replication to the server tickrate (set in DefaultEngine.ini
    // → NetServerMaxTickRate). At 64Hz server, replicate at 64Hz, allow throttle
    // down to 30Hz if the character is idle/distant.
    SetNetUpdateFrequency(64.0f);
    SetMinNetUpdateFrequency(30.0f);

    GetCapsuleComponent()->InitCapsuleSize(55.f, 96.0f);

    // Capsule handles character/world collision but IGNORES weapon traces, so
    // bullets pass through to the skeletal mesh (which has per-bone shapes for
    // headshot detection). Standard tac-FPS rig — CS/Valorant work the same way.
    GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
    GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);

    // ---- Camera rig (tac-FPS) ---------------------------------------------
    // Camera is attached to the capsule (world-aligned axes), NOT the head
    // bone. This kills animation-driven view sway (bad for aim) and makes
    // crouch math trivial. FP arms are then parented to the camera so they
    // follow the view.
    FirstPersonCameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("First Person Camera"));
    FirstPersonCameraComponent->SetupAttachment(GetCapsuleComponent());
    FirstPersonCameraComponent->SetRelativeLocation(FVector(0.0f, 0.0f, StandingCameraZ));
    FirstPersonCameraComponent->bUsePawnControlRotation = true;
    FirstPersonCameraComponent->bEnableFirstPersonFieldOfView = true;
    FirstPersonCameraComponent->bEnableFirstPersonScale = true;
    FirstPersonCameraComponent->FirstPersonFieldOfView = 70.0f;
    FirstPersonCameraComponent->FirstPersonScale = 0.6f;

    FirstPersonMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("First Person Mesh"));
    FirstPersonMesh->SetupAttachment(FirstPersonCameraComponent);
    FirstPersonMesh->SetOnlyOwnerSee(true);
    FirstPersonMesh->FirstPersonPrimitiveType = EFirstPersonPrimitiveType::FirstPerson;
    FirstPersonMesh->SetCollisionProfileName(FName("NoCollision"));

    // Health container — replicated; bridges damage events into GameMode death flow.
    HealthComponent = CreateDefaultSubobject<UProjectCompHealthComponent>(TEXT("Health Component"));

    // Third-person mesh: body other players see; hidden from the local owner.
    GetMesh()->SetOwnerNoSee(true);
    GetMesh()->FirstPersonPrimitiveType = EFirstPersonPrimitiveType::WorldSpaceRepresentation;
    // The mesh is what enables headshot detection — explicitly block hitscan
    // traces (the capsule above is the fallback if the BP has no skeletal mesh).
    GetMesh()->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);

    GetCapsuleComponent()->SetCapsuleSize(34.0f, 96.0f);

    // ---- Tactical movement tuning -----------------------------------------
    // Numbers are tuned by feel, not strict 1:1 with Valorant.
    // Reference: Valorant run = 540, walk = 270, crouch ≈ 240 (cm/s).

    // Snappy ground accel: 6000 cm/s² = ~0.1s from standstill to top speed.
    // High accel is the hallmark of tac-FPS movement (enables instant counter-strafe).
    GetCharacterMovement()->MaxAcceleration = 6000.0f;

    // Dead-stop braking: kills UE's default slide on key release.
    GetCharacterMovement()->GroundFriction = 10.0f;
    GetCharacterMovement()->BrakingDecelerationWalking = 10000.0f;
    GetCharacterMovement()->BrakingFrictionFactor = 1.0f;

    // Run speed (cm/s). Slightly faster than Valorant's 540.
    GetCharacterMovement()->MaxWalkSpeed = 560.0f;

    // Air control governs how strongly input redirects velocity mid-air. The
    // resulting horizontal speed is then capped to jump-time speed by
    // UProjectCompCharacterMovementComponent::CalcVelocity, so this only
    // affects the *rate* of redirect, not max lateral velocity gained in air.
    GetCharacterMovement()->AirControl = 0.35f;
    GetCharacterMovement()->AirControlBoostMultiplier = 1.0f;   // Disables low-speed steering boost.
    GetCharacterMovement()->BrakingDecelerationFalling = 0.0f;  // Zero air drag — jump momentum is preserved.

    // Heavy gravity for a committed, snappy jump arc.
    // With JumpZ=600 and effective gravity 2.0 × 980 = 1960 cm/s²:
    //   apex height ≈ 92 cm, total air time ≈ 0.61 s.
    GetCharacterMovement()->GravityScale = 2.0f;
    GetCharacterMovement()->JumpZVelocity = 600.0f;

    // Engine-driven crouch: bIsCrouched is replicated, ceiling-aware on uncrouch.
    GetCharacterMovement()->NavAgentProps.bCanCrouch = true;
    GetCharacterMovement()->SetCrouchedHalfHeight(48.0f);       // 96 -> 48 = capsule drops 48 cm.
    GetCharacterMovement()->MaxWalkSpeedCrouched = 250.0f;

    MovementState = EMovementState::Walking;
    CrouchEyeHeight = 40.0f;
    DefaultEyeHeight = 64.0f;
}

// Binds Enhanced Input actions to the Do* handlers below.
void AProjectCompCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
    {
        EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &AProjectCompCharacter::DoJumpStart);
        EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &AProjectCompCharacter::DoJumpEnd);

        EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AProjectCompCharacter::MoveInput);

        EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AProjectCompCharacter::LookInput);
        EnhancedInputComponent->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &AProjectCompCharacter::LookInput);

        // Hold Shift: tactical walk (slower, quieter movement).
        EnhancedInputComponent->BindAction(WalkAction, ETriggerEvent::Started, this, &AProjectCompCharacter::DoWalkStart);
        EnhancedInputComponent->BindAction(WalkAction, ETriggerEvent::Completed, this, &AProjectCompCharacter::DoWalkEnd);

        // Hold Ctrl: crouch.
        if (CrouchAction)
        {
            EnhancedInputComponent->BindAction(CrouchAction, ETriggerEvent::Started, this, &AProjectCompCharacter::DoCrouchStart);
            EnhancedInputComponent->BindAction(CrouchAction, ETriggerEvent::Completed, this, &AProjectCompCharacter::DoCrouchEnd);
        }

        // Hold Mouse1: fire equipped weapon.
        if (FireAction)
        {
            EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Started, this, &AProjectCompCharacter::DoFireStart);
            EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Completed, this, &AProjectCompCharacter::DoFireEnd);
        }

        // Hold E (default): plant / defuse the bomb while inside a site.
        if (UseAction)
        {
            EnhancedInputComponent->BindAction(UseAction, ETriggerEvent::Started, this, &AProjectCompCharacter::DoUseStart);
            EnhancedInputComponent->BindAction(UseAction, ETriggerEvent::Completed, this, &AProjectCompCharacter::DoUseEnd);
        }

        // Slot 1 (Loom) is cast via the minimap UI, not a character input —
        // see UProjectCompMinimapWidget. Slot 2 (mid-round) is still pending
        // a design direction; the Q binding stays as scaffolding until then.
        if (AbilitySlot2Action)
        {
            EnhancedInputComponent->BindAction(AbilitySlot2Action, ETriggerEvent::Started, this, &AProjectCompCharacter::DoCastAbilitySlot2);
        }

        // Re-emit held input on possession (fixes the "held F/Mouse1 across
        // respawn never re-engages" bug). Deferred one tick so Enhanced Input
        // has processed the first frame and the equipped weapon exists.
        if (UWorld* World = GetWorld())
        {
            World->GetTimerManager().SetTimerForNextTick(this, &AProjectCompCharacter::ReconcileHeldInputAfterPossession);
        }
    }
    else
    {
        UE_LOG(LogProjectComp, Error, TEXT("'%s' Failed to find an Enhanced Input Component! This template is built to use the Enhanced Input system. If you intend to use the legacy system, then you will need to update this C++ file."), *GetNameSafe(this));
    }
}

void AProjectCompCharacter::MoveInput(const FInputActionValue& Value)
{
    const FVector2D MovementVector = Value.Get<FVector2D>();
    DoMove(MovementVector.X, MovementVector.Y);
    UpdateAnimationState();
}

void AProjectCompCharacter::LookInput(const FInputActionValue& Value)
{
    const FVector2D LookAxisVector = Value.Get<FVector2D>();
    DoAim(LookAxisVector.X, LookAxisVector.Y);
}

// Yaw/Pitch in degrees-per-frame consumed by the controller.
void AProjectCompCharacter::DoAim(float Yaw, float Pitch)
{
    if (!GetController<APlayerController>())
    {
        return;
    }
    AddControllerYawInput(Yaw);
    AddControllerPitchInput(Pitch);
}

// Right/Forward scalars in [-1, 1]; applied along the actor's local axes.
void AProjectCompCharacter::DoMove(float Right, float Forward)
{
    if (!GetController())
    {
        return;
    }
    AddMovementInput(GetActorRightVector(), Right);
    AddMovementInput(GetActorForwardVector(), Forward);
}

void AProjectCompCharacter::DoJumpStart()
{
    // Request uncrouch alongside the jump so the capsule expands on the next
    // movement tick. CanJumpInternal_Implementation lets the jump through even
    // while bIsCrouched is still true (the engine default would refuse it).
    if (bIsCrouched)
    {
        UnCrouch();
    }

    MovementState = EMovementState::Airborne;
    UpdateAnimationState();

    Jump();
}

// Allow jump-from-crouch. Both Super::CanJumpInternal_Implementation and
// JumpIsAllowedInternal refuse while bIsCrouched is set, so we replicate the
// same gates here minus the crouch check. DoJumpStart has already requested
// the uncrouch — the capsule resizes on the next movement tick.
bool AProjectCompCharacter::CanJumpInternal_Implementation() const
{
    UCharacterMovementComponent* CharMov = GetCharacterMovement();
    if (!CharMov || !CharMov->IsJumpAllowed())
    {
        return false;
    }
    if (!CharMov->IsMovingOnGround() && !CharMov->IsFalling())
    {
        return false;
    }
    // Mirror ACharacter::CanJumpInternal hold-jump logic.
    const bool bJumpKeyHeld = (bPressedJump && JumpKeyHoldTime < GetJumpMaxHoldTime());
    return !bWasJumping && (JumpCurrentCount == 0 || (bJumpKeyHeld && JumpCurrentCount < JumpMaxCount));
}

void AProjectCompCharacter::DoJumpEnd()
{
    StopJumping();
}

// Engine override that fires on landing.
void AProjectCompCharacter::Landed(const FHitResult& Hit)
{
    Super::Landed(Hit);

    MovementState = EMovementState::Walking;
    UpdateAnimationState();
}

// Drop dead (looming) characters from opponents' relevancy entirely so a cheat
// client cannot read their position by toggling SetVisibility. Teammates and the
// looming player themselves still see the actor. Spectators and the owner are
// always relevant. Alive characters and non-character viewers go through the
// default relevancy path.
bool AProjectCompCharacter::IsNetRelevantFor(const AActor* RealViewer, const AActor* ViewTarget, const FVector& SrcLocation) const
{
    const AProjectCompPlayerState* MyPS = GetPlayerState<AProjectCompPlayerState>();
    if (MyPS && !MyPS->IsAlive())
    {
        const APlayerController* ViewerPC = Cast<APlayerController>(RealViewer);
        const AProjectCompPlayerState* ViewerPS = ViewerPC ? ViewerPC->GetPlayerState<AProjectCompPlayerState>() : nullptr;

        if (ViewerPC && ViewerPC->GetPawn() == this)
        {
            // Owner always relevant (we still need to drive the dead pawn for them).
            return Super::IsNetRelevantFor(RealViewer, ViewTarget, SrcLocation);
        }

        if (ViewerPS
            && ViewerPS->GetTeamID() != ETeamID::Spectator
            && MyPS->GetTeamID() != ETeamID::Spectator
            && ViewerPS->GetTeamID() != MyPS->GetTeamID())
        {
            return false;
        }
    }
    return Super::IsNetRelevantFor(RealViewer, ViewTarget, SrcLocation);
}

// Standard UE damage entry point. Forward into the HealthComponent on the
// server; clients drop. Compatible with any engine damage source (UDamageType,
// UGameplayStatics::ApplyDamage, projectile hit events, etc.).
//
// Friendly-fire policy lives here: damage between teammates is scaled by the
// GameMode's FriendlyFireScalar (default 0.0 = friendly fire off). Self-damage
// (Instigator == GetController()) bypasses the scalar so falls/explosives
// hurt the source player fully.
float AProjectCompCharacter::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
    float Adjusted = DamageAmount;

    if (HasAuthority() && EventInstigator && EventInstigator != GetController())
    {
        const AProjectCompPlayerState* InstPS = EventInstigator->GetPlayerState<AProjectCompPlayerState>();
        const AProjectCompPlayerState* SelfPS = GetPlayerState<AProjectCompPlayerState>();
        if (InstPS && SelfPS
            && InstPS->GetTeamID() != ETeamID::Spectator
            && InstPS->GetTeamID() == SelfPS->GetTeamID())
        {
            const AProjectCompGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr;
            const float Scalar = GM ? GM->GetFriendlyFireScalar() : 0.0f;
            Adjusted *= Scalar;
        }
    }

    if (Adjusted <= 0.0f)
    {
        // Skip Super so engine doesn't run damage-type processing on a zeroed event.
        return 0.0f;
    }

    const float Actual = Super::TakeDamage(Adjusted, DamageEvent, EventInstigator, DamageCauser);
    if (HasAuthority() && HealthComponent && Actual > 0.0f)
    {
        return HealthComponent->Server_TakeDamage(Actual, EventInstigator);
    }
    return Actual;
}

// Console command for damage testing: type "SelfDamage 100" in the PIE
// console. Runs through the standard damage chain so the GameMode death flow
// fires exactly as it would from a real weapon. No-op in shipping so it
// can't be invoked from retail consoles.
void AProjectCompCharacter::SelfDamage(float Amount)
{
#if UE_BUILD_SHIPPING
    (void)Amount;
#else
    if (Amount <= 0.0f)
    {
        return;
    }
    FDamageEvent DamageEvent;
    TakeDamage(Amount, DamageEvent, GetController(), this);
#endif
}

// PIE cheat for testing the Loom dispatch without a vote UI: writes both teams'
// AbilitySlot1/2 directly and refills pools so a cast is immediately ready.
// Runs on the listen-server host only (PIE Player 1). Remote PIE clients
// silently no-op since their character isn't authoritative.
void AProjectCompCharacter::ForceDraft(const FString& Slot1, const FString& Slot2)
{
#if UE_BUILD_SHIPPING
    (void)Slot1; (void)Slot2;
#else
    if (!HasAuthority())
    {
        UE_LOG(LogProjectComp, Log,
            TEXT("[Loom] ForceDraft: no authority on this client. Run from the listen-server host (PIE Player 1)."));
        return;
    }

    auto Parse = [](const FString& Code) -> EAbilityType
    {
        const FString U = Code.ToUpper();
        if (U == TEXT("AA")) return EAbilityType::AA;
        if (U == TEXT("AC")) return EAbilityType::AC;
        if (U == TEXT("AM")) return EAbilityType::AM;
        if (U == TEXT("CC")) return EAbilityType::CC;
        if (U == TEXT("CA")) return EAbilityType::CA;
        if (U == TEXT("CM")) return EAbilityType::CM;
        if (U == TEXT("MM")) return EAbilityType::MM;
        if (U == TEXT("MA")) return EAbilityType::MA;
        if (U == TEXT("MC")) return EAbilityType::MC;
        return EAbilityType::None;
    };

    const EAbilityType A1 = Parse(Slot1);
    const EAbilityType A2 = Parse(Slot2);
    if (A1 == EAbilityType::None || A2 == EAbilityType::None)
    {
        UE_LOG(LogProjectComp, Log,
            TEXT("[Loom] ForceDraft: invalid codes '%s' '%s'. Use AA AC AM CC CA CM MM MA MC."),
            *Slot1, *Slot2);
        return;
    }

    AProjectCompGameState* GS = GetWorld() ? GetWorld()->GetGameState<AProjectCompGameState>() : nullptr;
    if (!GS)
    {
        return;
    }

    for (uint8 i = 0; i < 2; ++i)
    {
        const ETeamID Team = static_cast<ETeamID>(i);
        FTeamLoadout Loadout = GS->GetTeamLoadout(Team);
        Loadout.AbilitySlot1 = A1;
        Loadout.AbilitySlot2 = A2;
        Loadout.SharedCooldownPool = 1.0f;
        GS->Server_SetTeamLoadout(Team, Loadout);
    }

    UE_LOG(LogProjectComp, Log,
        TEXT("[Loom] ForceDraft applied: both teams slot1=%s slot2=%s, pools full."),
        *Slot1, *Slot2);
#endif
}

// Setting bWantsToWalk on the CMC routes through saved moves; both server
// and owning client predict the same MaxWalkSpeed. ensure()s in non-shipping
// builds — silent failure of a movement input would be a hard-to-trace
// competitive bug, so we want a loud assert if the BP somehow uses the wrong
// movement component class.
void AProjectCompCharacter::DoWalkStart()
{
    UProjectCompCharacterMovementComponent* CharMov = Cast<UProjectCompCharacterMovementComponent>(GetCharacterMovement());
    if (ensureMsgf(CharMov, TEXT("AProjectCompCharacter requires UProjectCompCharacterMovementComponent. Check BP_FirstPersonCharacter's Movement Component class.")))
    {
        CharMov->bWantsToWalk = true;
    }
    UpdateAnimationState();
}

void AProjectCompCharacter::DoWalkEnd()
{
    UProjectCompCharacterMovementComponent* CharMov = Cast<UProjectCompCharacterMovementComponent>(GetCharacterMovement());
    if (ensure(CharMov))
    {
        CharMov->bWantsToWalk = false;
    }
    UpdateAnimationState();
}

// Drive bWantsToCrouch directly (instead of Crouch()/UnCrouch()) so a crouch
// requested mid-air persists and engages the moment we land. The CMC honors
// the flag during UpdateCharacterStateBeforeMovement and runs its own ground
// check on entry and ceiling check on exit.
//
// While the interact lock owns the crouch state (plant/defuse), don't touch
// bWantsToCrouch — that would let the player stand up mid-plant. Record the
// intent in bWasCrouchingBeforeInteract so the lock-release path applies the
// player's last input as the post-interaction crouch state.
void AProjectCompCharacter::DoCrouchStart()
{
    if (bInteractLockActive)
    {
        bWasCrouchingBeforeInteract = true;
        return;
    }
    if (UCharacterMovementComponent* CharMov = GetCharacterMovement())
    {
        CharMov->bWantsToCrouch = true;
    }
}

void AProjectCompCharacter::DoCrouchEnd()
{
    if (bInteractLockActive)
    {
        bWasCrouchingBeforeInteract = false;
        return;
    }
    if (UCharacterMovementComponent* CharMov = GetCharacterMovement())
    {
        CharMov->bWantsToCrouch = false;
    }
}

// Fire input handlers route through Server_SetTriggerHeld so the authoritative
// weapon (which lives server-side) starts/stops its fire loop.
void AProjectCompCharacter::DoFireStart()
{
    bIsFiring = true;
    // Reset accumulated recoil so each new spray starts clean.
    AccumulatedRecoilPitch = 0.0f;
    AccumulatedRecoilYaw   = 0.0f;
    // Reset the local pattern mirror so the next multicast applies pattern[0].
    if (CurrentWeapon)
    {
        CurrentWeapon->ResetLocalRecoilPattern();
    }
    Server_SetTriggerHeld(true);
}

void AProjectCompCharacter::DoFireEnd()
{
    bIsFiring = false;
    Server_SetTriggerHeld(false);
}

// =====================================================================
// Lag compensation — server-only transform history + rewind
// =====================================================================

void AProjectCompCharacter::Server_RecordTransformSnapshot(float ServerTime)
{
    if (!HasAuthority() || bIsRewound)
    {
        return;
    }

    FCharacterTransformSnapshot& Slot = HistoryBuf[HistoryHead];
    Slot.ServerTime = ServerTime;
    Slot.Location = GetActorLocation();
    Slot.Rotation = GetActorRotation();
    if (const UCapsuleComponent* Capsule = GetCapsuleComponent())
    {
        Slot.CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
    }

    HistoryHead = (HistoryHead + 1) % kHistorySize;
    HistoryCount = FMath::Min(HistoryCount + 1, kHistorySize);
}

bool AProjectCompCharacter::Server_GetHistoricalSnapshot(float TargetServerTime, FCharacterTransformSnapshot& Out) const
{
    if (HistoryCount == 0)
    {
        return false;
    }

    // Walk newest → oldest and find the bracketing pair around TargetServerTime.
    // Indices: most recent sample is at (HistoryHead - 1 + N) % N.
    auto IndexAtOffset = [this](int32 Offset) -> int32
    {
        return ((HistoryHead - 1 - Offset) % kHistorySize + kHistorySize) % kHistorySize;
    };

    const FCharacterTransformSnapshot& Newest = HistoryBuf[IndexAtOffset(0)];
    if (TargetServerTime >= Newest.ServerTime)
    {
        Out = Newest;
        return true;
    }

    for (int32 Off = 1; Off < HistoryCount; ++Off)
    {
        const FCharacterTransformSnapshot& Older = HistoryBuf[IndexAtOffset(Off)];
        const FCharacterTransformSnapshot& Newer = HistoryBuf[IndexAtOffset(Off - 1)];

        if (TargetServerTime >= Older.ServerTime && TargetServerTime <= Newer.ServerTime)
        {
            const float Span = Newer.ServerTime - Older.ServerTime;
            const float Alpha = Span > KINDA_SMALL_NUMBER
                              ? (TargetServerTime - Older.ServerTime) / Span
                              : 0.0f;
            Out.ServerTime = TargetServerTime;
            Out.Location = FMath::Lerp(Older.Location, Newer.Location, Alpha);
            Out.Rotation = FMath::Lerp(Older.Rotation, Newer.Rotation, Alpha);
            Out.CapsuleHalfHeight = FMath::Lerp(Older.CapsuleHalfHeight, Newer.CapsuleHalfHeight, Alpha);
            return true;
        }
    }

    // Requested time is older than the oldest sample we have — use the oldest.
    Out = HistoryBuf[IndexAtOffset(HistoryCount - 1)];
    return true;
}

void AProjectCompCharacter::Server_RewindTo(float TargetServerTime)
{
    if (!HasAuthority() || bIsRewound)
    {
        return;
    }

    FCharacterTransformSnapshot Snap;
    if (!Server_GetHistoricalSnapshot(TargetServerTime, Snap))
    {
        return;
    }

    PreRewindLocation = GetActorLocation();
    PreRewindRotation = GetActorRotation();
    bIsRewound = true;

    // Position-only move; the mesh is attached to the capsule so it follows.
    // ETeleportType::TeleportPhysics avoids sweep collisions during the rewind.
    SetActorLocationAndRotation(Snap.Location, Snap.Rotation,
        /*bSweep*/ false, /*OutSweepHitResult*/ nullptr, ETeleportType::TeleportPhysics);
}

void AProjectCompCharacter::Server_RestoreFromRewind()
{
    if (!bIsRewound)
    {
        return;
    }
    SetActorLocationAndRotation(PreRewindLocation, PreRewindRotation,
        /*bSweep*/ false, /*OutSweepHitResult*/ nullptr, ETeleportType::TeleportPhysics);
    bIsRewound = false;
}

FRotator AProjectCompCharacter::Server_GetValidatedAimRotation()
{
    AController* C = GetController();
    if (!C || !HasAuthority())
    {
        return C ? C->GetControlRotation() : FRotator::ZeroRotator;
    }

    FRotator Raw = C->GetControlRotation();
    // Normalize pitch into [-180, 180] so the clamp below is meaningful.
    Raw.Pitch = FRotator::NormalizeAxis(Raw.Pitch);
    Raw.Yaw   = FRotator::NormalizeAxis(Raw.Yaw);

    // Absolute pitch limit always applies (looking past straight-up is impossible).
    Raw.Pitch = FMath::Clamp(Raw.Pitch, -MaxAbsolutePitch, MaxAbsolutePitch);

    const UWorld* World = GetWorld();
    const float Now = World ? World->GetTimeSeconds() : 0.0f;

    if (LastAimValidationTime < 0.0f)
    {
        // First call this life — seed without clamping (otherwise spawn rotation
        // would always look like an instantaneous "delta" from 0).
        LastAimValidationTime = Now;
        LastValidatedYaw   = Raw.Yaw;
        LastValidatedPitch = Raw.Pitch;
        C->SetControlRotation(Raw);
        return Raw;
    }

    const float DeltaSeconds = FMath::Max(Now - LastAimValidationTime, 0.0f);
    // 0.001s floor: a single-tick repeat call would otherwise allow a near-zero
    // budget and reject legitimate moves on the next frame.
    const float MaxYawStep   = MaxYawRateDegPerSec   * FMath::Max(DeltaSeconds, 0.001f);
    const float MaxPitchStep = MaxPitchRateDegPerSec * FMath::Max(DeltaSeconds, 0.001f);

    const float YawDelta   = FRotator::NormalizeAxis(Raw.Yaw   - LastValidatedYaw);
    const float PitchDelta = FRotator::NormalizeAxis(Raw.Pitch - LastValidatedPitch);

    FRotator Clamped = Raw;
    if (FMath::Abs(YawDelta) > MaxYawStep)
    {
        Clamped.Yaw = FRotator::NormalizeAxis(LastValidatedYaw + FMath::Sign(YawDelta) * MaxYawStep);
    }
    if (FMath::Abs(PitchDelta) > MaxPitchStep)
    {
        Clamped.Pitch = FMath::Clamp(LastValidatedPitch + FMath::Sign(PitchDelta) * MaxPitchStep,
                                     -MaxAbsolutePitch, MaxAbsolutePitch);
    }

    // If we had to clamp, write the validated rotation back to the controller so
    // subsequent reads (replication, other authoritative consumers) see the
    // clamped value rather than the client's raw claim.
    if (!FMath::IsNearlyEqual(Clamped.Yaw,   Raw.Yaw,   0.001f) ||
        !FMath::IsNearlyEqual(Clamped.Pitch, Raw.Pitch, 0.001f))
    {
        C->SetControlRotation(Clamped);
    }

    LastAimValidationTime = Now;
    LastValidatedYaw   = Clamped.Yaw;
    LastValidatedPitch = Clamped.Pitch;
    return Clamped;
}

void AProjectCompCharacter::ApplyRecoilKickLocal(float PitchKick, float YawKick, float RecoveryRate, float RecoveryDelay)
{
	APlayerController* PC = GetController<APlayerController>();
	if (!PC || !IsLocallyControlled())
	{
		return;
	}

	// UE control pitch: positive = look up. PitchKick > 0 kicks camera up.
	FRotator Rot = PC->GetControlRotation();
	Rot.Pitch += PitchKick;
	Rot.Yaw   += YawKick;
	PC->SetControlRotation(Rot);

	// Track the outstanding kick for intra-spray smooth recovery.
	AccumulatedRecoilPitch  += PitchKick;
	AccumulatedRecoilYaw    += YawKick;
	RecoilRecoveryRate       = RecoveryRate;
	TimeUntilRecoilRecovery  = RecoveryDelay;

	SetActorTickEnabled(true);
}

bool AProjectCompCharacter::Server_SetTriggerHeld_Validate(bool /*bHeld*/)
{
    // bool is self-validating; per-context gates live in _Implementation.
    return true;
}

void AProjectCompCharacter::Server_SetTriggerHeld_Implementation(bool bHeld)
{
    if (!CurrentWeapon)
    {
        return;
    }

    // Aliveness gate — a looming or permanent-ghost player must not be able to
    // arm the server-side fire loop. Weapon::FireOneShot also checks this, but
    // rejecting at the RPC boundary stops the loop entirely (cheaper).
    if (const AProjectCompPlayerState* PS = GetPlayerState<AProjectCompPlayerState>())
    {
        if (!PS->IsAlive())
        {
            return;
        }
    }

    // No phase gate here — the trigger-held state should always reflect the
    // player's actual input. The weapon's Tick combines this with a phase gate
    // to decide whether to actually fire. Keeping the state accurate means a
    // held trigger across a PreRound -> RoundActive transition auto-fires the
    // moment gunplay opens, and a release during PostRound is recorded
    // correctly (fixes the "bullets keep spraying after round-end kill" bug).
    CurrentWeapon->Server_SetTriggerHeld(bHeld);
}

void AProjectCompCharacter::DoUseStart()
{
    bUseHeld = true;
    UpdateInteractLock();
    Server_SetUseHeld(true);
}

void AProjectCompCharacter::DoUseEnd()
{
    bUseHeld = false;
    UpdateInteractLock();
    Server_SetUseHeld(false);
}

void AProjectCompCharacter::DoCastAbilitySlot2()
{
    UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Slot 2 input pressed."));
    RequestAbilityCastForSlot(2);
}

void AProjectCompCharacter::RequestAbilityCastForSlot(int32 SlotIndex)
{
    AProjectCompPlayerController* PC = GetController<AProjectCompPlayerController>();
    if (!PC)
    {
        UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Slot %d: no PlayerController (not locally controlled?)."), SlotIndex);
        return;
    }

    // Resolve the slot → the team's locked-in ability from GameState. If the
    // team hasn't drafted this slot (still PreRound, or zero voters), short-
    // circuit before sending an RPC the server would just reject.
    const AProjectCompPlayerState* PS = GetPlayerState<AProjectCompPlayerState>();
    const UWorld* World = GetWorld();
    const AProjectCompGameState* GS = World ? World->GetGameState<AProjectCompGameState>() : nullptr;
    if (!PS || !GS)
    {
        UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Slot %d: missing PlayerState (%d) or GameState (%d)."),
            SlotIndex, PS != nullptr, GS != nullptr);
        return;
    }
    const FTeamLoadout Loadout = GS->GetTeamLoadout(PS->GetTeamID());
    const EAbilityType Ability = (SlotIndex == 1) ? Loadout.AbilitySlot1 : Loadout.AbilitySlot2;
    if (Ability == EAbilityType::None)
    {
        UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Slot %d: team %d has no drafted ability for this slot (vote in PreRound first)."),
            SlotIndex, static_cast<int32>(PS->GetTeamID()));
        return;
    }

    // Aim-trace from the camera to derive a world TargetLocation. ECC_Visibility
    // blocks on world geometry and body meshes (per BeginPlay's collision setup),
    // so a cast aimed at a teammate lands on their body; one aimed at empty sky
    // falls back to the end of the ray. Trace ignores self so the looming pawn's
    // own collisions don't capture the hit.
    FVector CameraLoc;
    FRotator CameraRot;
    PC->GetPlayerViewPoint(CameraLoc, CameraRot);
    const FVector TraceEnd = CameraLoc + CameraRot.Vector() * AbilityAimDistance;
    FHitResult Hit;
    FCollisionQueryParams Params(NAME_None, /*bTraceComplex*/ false, this);
    FVector TargetLocation = TraceEnd;
    if (World && World->LineTraceSingleByChannel(Hit, CameraLoc, TraceEnd, ECC_Visibility, Params))
    {
        TargetLocation = Hit.ImpactPoint;
    }

    UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Slot %d: forwarding cast type=%d to server, target=%s."),
        SlotIndex, static_cast<int32>(Ability), *TargetLocation.ToCompactString());
    PC->RequestAbilityCast(Ability, TargetLocation);
}

// Held-input reconciliation on possession.
//
// Enhanced Input is edge-triggered: "Started" fires when an action transitions
// from inactive to active, "Completed" on the reverse. If a player is holding
// Mouse1/F when their pawn is destroyed, the held state lives on the *pawn*
// (bIsFiring + weapon's bTriggerHeld, or bUseHeld on this character). The new
// pawn starts those booleans at false, and Enhanced Input does NOT auto-emit
// Started for keys that were already down before this pawn was possessed.
//
// Result without this fix: the player keeps Mouse1 held through death, sees
// the new pawn spawn, and nothing fires until they release+re-press. Worst
// case is a held F that fails to start planting after a mid-plant death.
//
// Fix: one tick after SetupPlayerInputComponent runs (so the input subsystem
// has populated action values for the new pawn), query each held-style action
// and synthesise the Started call.
void AProjectCompCharacter::ReconcileHeldInputAfterPossession()
{
    const APlayerController* PC = GetController<APlayerController>();
    if (!PC)
    {
        return;
    }
    const ULocalPlayer* LP = PC->GetLocalPlayer();
    if (!LP)
    {
        return;
    }
    const UEnhancedInputLocalPlayerSubsystem* Subsystem =
        LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
    const UEnhancedPlayerInput* PlayerInput = Subsystem ? Subsystem->GetPlayerInput() : nullptr;
    if (!PlayerInput)
    {
        return;
    }

    if (FireAction && PlayerInput->GetActionValue(FireAction).Get<bool>())
    {
        UE_LOG(LogProjectComp, Verbose, TEXT("[Input] Reconcile: Fire held across possession — synthesising Started."));
        DoFireStart();
    }
    if (UseAction && PlayerInput->GetActionValue(UseAction).Get<bool>())
    {
        UE_LOG(LogProjectComp, Verbose, TEXT("[Input] Reconcile: Use held across possession — synthesising Started."));
        DoUseStart();
    }
}

bool AProjectCompCharacter::Server_SetUseHeld_Validate(bool /*bHeld*/)
{
    return true;
}

void AProjectCompCharacter::Server_SetUseHeld_Implementation(bool bHeld)
{
    // Dead players don't interact (no plant/defuse from looming).
    if (const AProjectCompPlayerState* PS = GetPlayerState<AProjectCompPlayerState>())
    {
        if (!PS->IsAlive())
        {
            bUseHeld = false;
            UpdateInteractLock();
            return;
        }
    }

    bUseHeld = bHeld;
    UpdateInteractLock();
}

void AProjectCompCharacter::OnBombSiteOverlapChanged(bool bEntered)
{
    if (bEntered)
    {
        ++BombSiteOverlapCount;
    }
    else
    {
        // Clamp at 0 in case of begin/end imbalance during teleport/destroy races.
        BombSiteOverlapCount = FMath::Max(0, BombSiteOverlapCount - 1);
    }
    UpdateInteractLock();
}

void AProjectCompCharacter::NotifyBombSiteOverlapChanged()
{
    // Legacy entry point — recount from the overlap query so external callers
    // who don't know the enter/exit sense still produce a correct counter.
    TArray<AActor*> Overlapping;
    GetOverlappingActors(Overlapping, AProjectCompBombSite::StaticClass());
    BombSiteOverlapCount = Overlapping.Num();
    UpdateInteractLock();
}

// Single source of truth for the plant/defuse movement lock.
//
// Why this is a re-evaluating helper rather than edge-triggered code at the
// input callbacks:
//   1. Pressing F outside a site then walking in must engage the lock — needs
//      to react to overlap changes, not just key edges.
//   2. On a listen-server host, DoUseStart and Server_SetUseHeld_Implementation
//      both run for the same key press. If both independently captured
//      bWasCrouchingBeforeInteract from bWantsToCrouch, the second read would
//      see the value the first just wrote (true) and the player would never
//      uncrouch. Idempotent transitions on a single helper avoid that.
void AProjectCompCharacter::UpdateInteractLock()
{
    UCharacterMovementComponent* CharMov = GetCharacterMovement();
    if (!CharMov)
    {
        return;
    }

    const bool bShouldLock = bUseHeld && IsOverlappingBombSite();

    if (bShouldLock && !bInteractLockActive)
    {
        bWasCrouchingBeforeInteract = CharMov->bWantsToCrouch;
        CharMov->MaxWalkSpeed = 0.0f;
        CharMov->MaxWalkSpeedCrouched = 0.0f;
        CharMov->bWantsToCrouch = true;
        bInteractLockActive = true;
    }
    else if (!bShouldLock && bInteractLockActive)
    {
        CharMov->MaxWalkSpeed = DefaultMaxWalkSpeed;
        CharMov->MaxWalkSpeedCrouched = DefaultMaxWalkSpeedCrouched;
        if (!bWasCrouchingBeforeInteract)
        {
            CharMov->bWantsToCrouch = false;
        }
        bInteractLockActive = false;
    }
}


// Spawn the default weapon for newly-possessed characters on the server.
// Called from PossessedBy (also fires on respawn since RestartPlayer creates
// a new character that gets a fresh possession).
void AProjectCompCharacter::Server_EquipDefaultWeapon()
{
    if (!HasAuthority() || !DefaultWeaponClass)
    {
        return;
    }

    if (CurrentWeapon)
    {
        // Already equipped (e.g. PossessedBy fired twice). Don't double-spawn.
        return;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = this;
    SpawnParams.Instigator = this;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    CurrentWeapon = GetWorld()->SpawnActor<AProjectCompHitscanWeapon>(
        DefaultWeaponClass,
        GetActorLocation(),
        GetActorRotation(),
        SpawnParams);

    if (CurrentWeapon)
    {
        CurrentWeapon->AttachToActor(this, FAttachmentTransformRules::SnapToTargetIncludingScale);
    }
}

void AProjectCompCharacter::PossessedBy(AController* NewController)
{
    Super::PossessedBy(NewController);
    Server_EquipDefaultWeapon();
}

void AProjectCompCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AProjectCompCharacter, CurrentWeapon);
}

// Force collision responses for hitscan even if the BP has its own collision
// preset saved. Runs after CDO + BP defaults are applied, so this wins.
void AProjectCompCharacter::BeginPlay()
{
    Super::BeginPlay();

    if (UCharacterMovementComponent* CharMov = GetCharacterMovement())
    {
        DefaultMaxWalkSpeed = CharMov->MaxWalkSpeed;
        DefaultMaxWalkSpeedCrouched = CharMov->MaxWalkSpeedCrouched;
    }

    if (UCapsuleComponent* Capsule = GetCapsuleComponent())
    {
        Capsule->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
        Capsule->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
    }
    if (USkeletalMeshComponent* BodyMesh = GetMesh())
    {
        // Force collision on. Default character meshes often ship with
        // CollisionEnabled=NoCollision in templates; bullets can't hit a mesh
        // with collision turned off, regardless of response channels.
        BodyMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        BodyMesh->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
    }

    // Refresh mesh visibility based on local viewer's team relationship to this
    // character. 0.1s is responsive enough for death/respawn/halftime swap; this
    // runs on every client (including the listen server's host).
    GetWorldTimerManager().SetTimer(VisibilityRefreshTimer, this,
        &AProjectCompCharacter::UpdateMeshVisibilityForLocalViewer,
        0.1f, /*bLoop*/ true);
}

void AProjectCompCharacter::UpdateMeshVisibilityForLocalViewer()
{
    USkeletalMeshComponent* BodyMesh = GetMesh();
    if (!BodyMesh)
    {
        return;
    }

    const AProjectCompPlayerState* MyPS = GetPlayerState<AProjectCompPlayerState>();
    if (!MyPS)
    {
        return;
    }

    // Alive: always visible to everyone.
    if (MyPS->IsAlive())
    {
        if (!BodyMesh->IsVisible())
        {
            BodyMesh->SetVisibility(true);
        }
        return;
    }

    // Dead (looming): hide body from opponents only. Self + teammates still see
    // the looming body so they know where their teammate is and how they're moving.
    const UWorld* World = GetWorld();
    const APlayerController* LocalPC = World ? World->GetFirstPlayerController() : nullptr;
    const AProjectCompPlayerState* LocalPS = LocalPC ? LocalPC->GetPlayerState<AProjectCompPlayerState>() : nullptr;

    bool bHideFromLocalViewer = false;
    if (LocalPS && LocalPS != MyPS
        && MyPS->GetTeamID() != ETeamID::Spectator
        && LocalPS->GetTeamID() != MyPS->GetTeamID())
    {
        bHideFromLocalViewer = true;
    }

    BodyMesh->SetVisibility(!bHideFromLocalViewer);
}

// OnStartCrouch/OnEndCrouch: engine fires these after the capsule has snapped
// to its new height. We use them to start the smooth-camera transition (the
// capsule snaps instantly for accurate hit-reg; only the *view* eases).
void AProjectCompCharacter::OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
    Super::OnStartCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);

    TargetCrouchAlpha = 1.0f;
    CachedCrouchHalfHeightDiff = HalfHeightAdjust;
    SetActorTickEnabled(true);

    UpdateAnimationState();
}

void AProjectCompCharacter::OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
    Super::OnEndCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);

    TargetCrouchAlpha = 0.0f;
    CachedCrouchHalfHeightDiff = HalfHeightAdjust;
    SetActorTickEnabled(true);

    UpdateAnimationState();
}

// Drives two active transitions: crouch camera ease and recoil recovery.
// Tick is only enabled when at least one is in progress; both disable tick
// when they settle so idle characters pay zero per-frame cost.
//
// Crouch: the engine snaps the capsule instantly for accurate hit-reg; we
// undo that snap on the camera and re-apply it gradually so the view eases.
void AProjectCompCharacter::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (!FirstPersonCameraComponent)
    {
        return;
    }

    bool bTickStillNeeded = false;

    // ---- Crouch camera transition -----------------------------------------
    CrouchVisualAlpha = FMath::FInterpTo(CrouchVisualAlpha, TargetCrouchAlpha, DeltaTime, CrouchInterpSpeed);
    const bool bCrouchAtRest = FMath::IsNearlyEqual(CrouchVisualAlpha, TargetCrouchAlpha, 0.001f);
    if (bCrouchAtRest)
    {
        CrouchVisualAlpha = TargetCrouchAlpha;
    }
    else
    {
        bTickStillNeeded = true;
    }

    const float EngineOffset = bIsCrouched ? CachedCrouchHalfHeightDiff : 0.0f;
    const float VisualOffset = CachedCrouchHalfHeightDiff * CrouchVisualAlpha;

    FVector Loc = FirstPersonCameraComponent->GetRelativeLocation();
    Loc.Z = StandingCameraZ + EngineOffset - VisualOffset;
    FirstPersonCameraComponent->SetRelativeLocation(Loc);

    // ---- Recoil recovery (owning client only) -----------------------------
    // Smoothly ease the camera back toward pre-spray neutral. Runs both DURING
    // a held spray (intra-spray recovery) and AFTER trigger release until the
    // accumulator reaches zero. Valorant-style: tap-fire kicks up fast, then
    // the camera drifts back down on its own at RecoilRecoveryRate.
    const bool bHasRecoil = !FMath::IsNearlyZero(AccumulatedRecoilPitch, 0.01f)
                          || !FMath::IsNearlyZero(AccumulatedRecoilYaw, 0.01f);
    if (IsLocallyControlled() && bHasRecoil)
    {
        TimeUntilRecoilRecovery = FMath::Max(0.0f, TimeUntilRecoilRecovery - DeltaTime);
        if (TimeUntilRecoilRecovery <= 0.0f)
        {
            if (APlayerController* PC = GetController<APlayerController>())
            {
                const float RecoverThisFrame = RecoilRecoveryRate * DeltaTime;
                FRotator Rot = PC->GetControlRotation();

                const float PitchRecover = FMath::Min(RecoverThisFrame, FMath::Abs(AccumulatedRecoilPitch));
                Rot.Pitch -= FMath::Sign(AccumulatedRecoilPitch) * PitchRecover;
                AccumulatedRecoilPitch -= FMath::Sign(AccumulatedRecoilPitch) * PitchRecover;

                const float YawRecover = FMath::Min(RecoverThisFrame, FMath::Abs(AccumulatedRecoilYaw));
                Rot.Yaw -= FMath::Sign(AccumulatedRecoilYaw) * YawRecover;
                AccumulatedRecoilYaw -= FMath::Sign(AccumulatedRecoilYaw) * YawRecover;

                PC->SetControlRotation(Rot);
            }
        }
        bTickStillNeeded = true;
    }

    if (!bTickStillNeeded)
    {
        SetActorTickEnabled(false);
    }
}

// Stub: animation BPs read MovementState directly via GetMovementState().
// Kept as a single call site so future state-sync code lands in one place.
void AProjectCompCharacter::UpdateAnimationState()
{
}