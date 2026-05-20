// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompCharacter.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "InputActionValue.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "ProjectComp.h"
#include "ProjectCompCharacterMovementComponent.h"

AProjectCompCharacter::AProjectCompCharacter(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer.SetDefaultSubobjectClass<UProjectCompCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
    // Tick is event-driven (enabled only during a crouch camera transition).
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;

    GetCapsuleComponent()->InitCapsuleSize(55.f, 96.0f);

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

    // Third-person mesh: body other players see; hidden from the local owner.
    GetMesh()->SetOwnerNoSee(true);
    GetMesh()->FirstPersonPrimitiveType = EFirstPersonPrimitiveType::WorldSpaceRepresentation;

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

    // Run speed (cm/s). A bit faster than Valorant's 540.
    GetCharacterMovement()->MaxWalkSpeed = 600.0f;

    // Air control governs how strongly input redirects velocity mid-air. The
    // resulting horizontal speed is then capped to jump-time speed by
    // UProjectCompCharacterMovementComponent::CalcVelocity, so this only
    // affects the *rate* of redirect, not max lateral velocity gained in air.
    GetCharacterMovement()->AirControl = 0.35f;
    GetCharacterMovement()->AirControlBoostMultiplier = 1.0f;   // Disables low-speed steering boost.
    GetCharacterMovement()->BrakingDecelerationFalling = 0.0f;  // Zero air drag — jump momentum is preserved.

    // Heavy gravity for a committed, snappy jump arc.
    // With JumpZ=660 and effective gravity 2.0 × 980 = 1960 cm/s²:
    //   apex height ≈ 111 cm, total air time ≈ 0.67 s.
    GetCharacterMovement()->GravityScale = 2.0f;
    GetCharacterMovement()->JumpZVelocity = 660.0f;

    // Engine-driven crouch: bIsCrouched is replicated, ceiling-aware on uncrouch.
    GetCharacterMovement()->NavAgentProps.bCanCrouch = true;
    GetCharacterMovement()->SetCrouchedHalfHeight(48.0f);       // 96 -> 48 = capsule drops 48 cm.
    GetCharacterMovement()->MaxWalkSpeedCrouched = 200.0f;      // Note: BP currently overrides to 300.

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
    // Tac-FPS convention: no jumping while crouched.
    if (bIsCrouched)
    {
        return;
    }

    MovementState = EMovementState::Airborne;
    UpdateAnimationState();

    Jump();
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

// Crouch()/UnCrouch() are the engine's prediction-safe path. CanCrouch() gates
// the ground check on entry; ceiling check on exit happens automatically.
void AProjectCompCharacter::DoCrouchStart()
{
    Crouch();
}

void AProjectCompCharacter::DoCrouchEnd()
{
    UnCrouch();
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

// Eases the camera between standing and crouched eye height. Active only
// during the transition (~0.2s) — disables itself when CrouchVisualAlpha
// reaches the target.
//
// The "EngineOffset - VisualOffset" trick: the engine snaps the actor up/down
// by CachedCrouchHalfHeightDiff the instant crouch toggles. We undo that snap
// on the camera, then re-apply it gradually as CrouchVisualAlpha catches up.
// When the two are equal the offsets cancel and the camera sits at its
// natural StandingCameraZ relative to whatever capsule center the engine has.
void AProjectCompCharacter::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (!FirstPersonCameraComponent)
    {
        return;
    }

    CrouchVisualAlpha = FMath::FInterpTo(CrouchVisualAlpha, TargetCrouchAlpha, DeltaTime, CrouchInterpSpeed);
    const bool bAtRest = FMath::IsNearlyEqual(CrouchVisualAlpha, TargetCrouchAlpha, 0.001f);
    if (bAtRest)
    {
        CrouchVisualAlpha = TargetCrouchAlpha;
    }

    const float EngineOffset = bIsCrouched ? CachedCrouchHalfHeightDiff : 0.0f;
    const float VisualOffset = CachedCrouchHalfHeightDiff * CrouchVisualAlpha;

    FVector Loc = FirstPersonCameraComponent->GetRelativeLocation();
    Loc.Z = StandingCameraZ + EngineOffset - VisualOffset;
    FirstPersonCameraComponent->SetRelativeLocation(Loc);

    if (bAtRest)
    {
        SetActorTickEnabled(false);
    }
}

// Stub: animation BPs read MovementState directly via GetMovementState().
// Kept as a single call site so future state-sync code lands in one place.
void AProjectCompCharacter::UpdateAnimationState()
{
}