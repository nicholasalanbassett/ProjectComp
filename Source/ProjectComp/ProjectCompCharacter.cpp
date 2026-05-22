// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompCharacter.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "InputActionValue.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/DamageEvents.h"
#include "Net/UnrealNetwork.h"
#include "ProjectComp.h"
#include "ProjectCompCharacterMovementComponent.h"
#include "ProjectCompHealthComponent.h"
#include "ProjectCompHitscanWeapon.h"
#include "ProjectCompPlayerState.h"
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
    NetUpdateFrequency = 64.0f;
    MinNetUpdateFrequency = 30.0f;

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

// Standard UE damage entry point. Forward into the HealthComponent on the
// server; clients drop. Compatible with any engine damage source (UDamageType,
// UGameplayStatics::ApplyDamage, projectile hit events, etc.).
float AProjectCompCharacter::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
    const float Actual = Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
    if (HasAuthority() && HealthComponent && Actual > 0.0f)
    {
        return HealthComponent->Server_TakeDamage(Actual, EventInstigator);
    }
    return Actual;
}

// Console command for damage testing: type "SelfDamage 100" in the PIE
// console. Runs through the standard damage chain so the GameMode death flow
// fires exactly as it would from a real weapon.
void AProjectCompCharacter::SelfDamage(float Amount)
{
    if (Amount <= 0.0f)
    {
        return;
    }
    FDamageEvent DamageEvent;
    TakeDamage(Amount, DamageEvent, GetController(), this);
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
void AProjectCompCharacter::DoCrouchStart()
{
    if (UCharacterMovementComponent* CharMov = GetCharacterMovement())
    {
        CharMov->bWantsToCrouch = true;
    }
}

void AProjectCompCharacter::DoCrouchEnd()
{
    if (UCharacterMovementComponent* CharMov = GetCharacterMovement())
    {
        CharMov->bWantsToCrouch = false;
    }
}

// Fire input handlers route through Server_SetTriggerHeld so the authoritative
// weapon (which lives server-side) starts/stops its fire loop.
void AProjectCompCharacter::DoFireStart()
{
    Server_SetTriggerHeld(true);
}

void AProjectCompCharacter::DoFireEnd()
{
    Server_SetTriggerHeld(false);
}

void AProjectCompCharacter::Server_SetTriggerHeld_Implementation(bool bHeld)
{
    if (CurrentWeapon)
    {
        CurrentWeapon->Server_SetTriggerHeld(bHeld);
    }
}

void AProjectCompCharacter::DoUseStart()
{
    if (UCharacterMovementComponent* CharMov = GetCharacterMovement())
    {
        // Remember whether the player chose to crouch independently so DoUseEnd
        // knows whether to uncrouch on release.
        bWasCrouchingBeforeInteract = CharMov->bWantsToCrouch;

        CharMov->MaxWalkSpeed = 0.0f;
        CharMov->MaxWalkSpeedCrouched = 0.0f;
        CharMov->bWantsToCrouch = true;
    }
    Server_SetUseHeld(true);
}

void AProjectCompCharacter::DoUseEnd()
{
    if (UCharacterMovementComponent* CharMov = GetCharacterMovement())
    {
        CharMov->MaxWalkSpeed = DefaultMaxWalkSpeed;
        CharMov->MaxWalkSpeedCrouched = DefaultMaxWalkSpeedCrouched;
        // Only uncrouch if the crouch was forced by the interaction, not by the
        // player holding Ctrl before or during the interaction.
        if (!bWasCrouchingBeforeInteract)
        {
            CharMov->bWantsToCrouch = false;
        }
    }
    Server_SetUseHeld(false);
}

void AProjectCompCharacter::Server_SetUseHeld_Implementation(bool bHeld)
{
    bUseHeld = bHeld;
    if (UCharacterMovementComponent* CharMov = GetCharacterMovement())
    {
        CharMov->MaxWalkSpeed = bHeld ? 0.0f : DefaultMaxWalkSpeed;
        CharMov->MaxWalkSpeedCrouched = bHeld ? 0.0f : DefaultMaxWalkSpeedCrouched;
        if (bHeld)
        {
            CharMov->bWantsToCrouch = true;
        }
        else if (!bWasCrouchingBeforeInteract)
        {
            CharMov->bWantsToCrouch = false;
        }
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