// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompCharacter.h"
#include "Animation/AnimInstance.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "InputActionValue.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "ProjectComp.h"

AProjectCompCharacter::AProjectCompCharacter()
{
    // Set size for collision capsule
    GetCapsuleComponent()->InitCapsuleSize(55.f, 96.0f);
    
    // Create the first person mesh that will be viewed only by this character's owner
    FirstPersonMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("First Person Mesh"));

    FirstPersonMesh->SetupAttachment(GetMesh());
    FirstPersonMesh->SetOnlyOwnerSee(true);
    FirstPersonMesh->FirstPersonPrimitiveType = EFirstPersonPrimitiveType::FirstPerson;
    FirstPersonMesh->SetCollisionProfileName(FName("NoCollision"));

    // Create the Camera Component  
    FirstPersonCameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("First Person Camera"));
    FirstPersonCameraComponent->SetupAttachment(FirstPersonMesh, FName("head"));
    FirstPersonCameraComponent->SetRelativeLocationAndRotation(FVector(-2.8f, 5.89f, 0.0f), FRotator(0.0f, 90.0f, -90.0f));
    FirstPersonCameraComponent->bUsePawnControlRotation = true;
    FirstPersonCameraComponent->bEnableFirstPersonFieldOfView = true;
    FirstPersonCameraComponent->bEnableFirstPersonScale = true;
    FirstPersonCameraComponent->FirstPersonFieldOfView = 70.0f;
    FirstPersonCameraComponent->FirstPersonScale = 0.6f;

    // configure the character comps
    GetMesh()->SetOwnerNoSee(true);
    GetMesh()->FirstPersonPrimitiveType = EFirstPersonPrimitiveType::WorldSpaceRepresentation;

    GetCapsuleComponent()->SetCapsuleSize(34.0f, 96.0f);

    // =====================================================================
    // TACTICAL MOVEMENT TUNING (VALORANT STYLE)
    // =====================================================================
    
    // 1. Instant Acceleration (Hits top speed the millisecond input is pressed)
    GetCharacterMovement()->MaxAcceleration = 10000.0f;

    // 2. Dead-Stop Braking (Completely eliminates the default Unreal slide)
    GetCharacterMovement()->GroundFriction = 10.0f;
    GetCharacterMovement()->BrakingDecelerationWalking = 10000.0f;
    GetCharacterMovement()->BrakingFrictionFactor = 1.0f;

    // 3. Exact Tactical Pacing (Valorant run speed is 5.4 m/s -> 540 cm/s)
    GetCharacterMovement()->MaxWalkSpeed = 600.0f;

    // 4. Tactical Air Strafing (Valorant Arc)
    GetCharacterMovement()->AirControl = 0.40f;                 // Enough to curve, not enough to hyper-steer
    GetCharacterMovement()->AirControlBoostMultiplier = 1.0f;   // Kills the unnatural low-speed steering boost
    GetCharacterMovement()->BrakingDecelerationFalling = 0.0f;  // Keeps forward momentum alive
    
    // 5. Hardcore Tactical Gravity (Valorant heavy drop)
    GetCharacterMovement()->GravityScale = 3.5f;                // Snappy, heavy downward pull
    GetCharacterMovement()->JumpZVelocity = 660.0f;             // Sharp upward burst to clear 62cm tactical crates
}

void AProjectCompCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{   
    // Set up action bindings
    if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
    {
        // Jumping
        EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &AProjectCompCharacter::DoJumpStart);
        EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &AProjectCompCharacter::DoJumpEnd);

        // Moving
        EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AProjectCompCharacter::MoveInput);

        // Looking/Aiming
        EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AProjectCompCharacter::LookInput);
        EnhancedInputComponent->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &AProjectCompCharacter::LookInput);
    
        // Tactical Walk (Hold Shift)
        EnhancedInputComponent->BindAction(WalkAction, ETriggerEvent::Started, this, &AProjectCompCharacter::DoWalkStart);
        EnhancedInputComponent->BindAction(WalkAction, ETriggerEvent::Completed, this, &AProjectCompCharacter::DoWalkEnd);
    }
    else
    {
        UE_LOG(LogProjectComp, Error, TEXT("'%s' Failed to find an Enhanced Input Component! This template is built to use the Enhanced Input system. If you intend to use the legacy system, then you will need to update this C++ file."), *GetNameSafe(this));
    }
}

void AProjectCompCharacter::MoveInput(const FInputActionValue& Value)
{
    // get the Vector2D move axis
    FVector2D MovementVector = Value.Get<FVector2D>();

    // pass the axis values to the move input
    DoMove(MovementVector.X, MovementVector.Y);
}

void AProjectCompCharacter::LookInput(const FInputActionValue& Value)
{
    // get the Vector2D look axis
    FVector2D LookAxisVector = Value.Get<FVector2D>();

    // pass the axis values to the aim input
    DoAim(LookAxisVector.X, LookAxisVector.Y);
}

void AProjectCompCharacter::DoAim(float Yaw, float Pitch)
{
    if (GetController())
    {
        // pass the rotation inputs
        AddControllerYawInput(Yaw);
        AddControllerPitchInput(Pitch);
    }
}

void AProjectCompCharacter::DoMove(float Right, float Forward)
{
    if (GetController())
    {
        // pass the move inputs
        AddMovementInput(GetActorRightVector(), Right);
        AddMovementInput(GetActorForwardVector(), Forward);
    }
}

void AProjectCompCharacter::DoJumpStart()
{
    if (GetCharacterMovement())
    {
        // 1. Get the player's current horizontal speed
        FVector HorizontalVelocity = GetCharacterMovement()->Velocity;
        HorizontalVelocity.Z = 0.0f;
        float GroundSpeedAtJump = HorizontalVelocity.Size();

        // 2. Map the speed to a precise control scale
        // Input: Speed from 0 (Dead stop) to 600 (Sprint)
        FVector2D InputSpeedRange(0.0f, 600.0f);
        
        // Output: Air control from 0.015 (Micro-nudge) to 0.40 (Full curve)
        FVector2D OutputControlRange(0.015f, 0.40f);
        
        GetCharacterMovement()->AirControl = FMath::GetMappedRangeValueClamped(InputSpeedRange, OutputControlRange, GroundSpeedAtJump);

        // 3. Ensure momentum carries smoothly
        GetCharacterMovement()->BrakingDecelerationFalling = 0.0f;
    }

    Jump();
}

void AProjectCompCharacter::DoJumpEnd()
{
    // ONLY stop the jump logic when spacebar is released. 
    // Do not reset air control here!
    StopJumping();
}

void AProjectCompCharacter::Landed(const FHitResult& Hit)
{
    // 1. Call the original Unreal Engine landing math first
    Super::Landed(Hit);

    // 2. Reset baseline air control the exact millisecond boots touch the floor
    if (GetCharacterMovement())
    {
        GetCharacterMovement()->AirControl = 1.0f;
    }
}

void AProjectCompCharacter::DoWalkStart()
{
    // pass WalkStart to the character
    if (GetCharacterMovement())
    {
        GetCharacterMovement()->MaxWalkSpeed = 350.0f; // Valorant walk speed is 2.7 m/s -> 270 cm/s
    }
}

void AProjectCompCharacter::DoWalkEnd()
{
    // pass WalkEnd to the character
    if (GetCharacterMovement())
    {
        GetCharacterMovement()->MaxWalkSpeed = 600.0f; // Reset to default run speed
    }
}