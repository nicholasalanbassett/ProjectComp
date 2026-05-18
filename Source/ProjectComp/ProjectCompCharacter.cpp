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
    GetCharacterMovement()->MaxAcceleration = 8000.0f;

    // 2. Dead-Stop Braking (Completely eliminates the default Unreal slide)
    GetCharacterMovement()->GroundFriction = 10.0f;
    GetCharacterMovement()->BrakingDecelerationWalking = 10000.0f;
    GetCharacterMovement()->BrakingFrictionFactor = 1.0f;

    // 3. Exact Tactical Pacing (Valorant run speed is 5.4 m/s -> 540 cm/s)
    GetCharacterMovement()->MaxWalkSpeed = 540.0f;

	// 4. Responsive Air Strafing (Source/CS-style steering power)
    GetCharacterMovement()->AirControl = 0.4f;                  // High lateral steering force
    GetCharacterMovement()->AirControlBoostMultiplier = 2.0f;   // Extra snappiness at low speeds
    GetCharacterMovement()->AirControlBoostVelocityThreshold = 250.0f; 
    GetCharacterMovement()->BrakingDecelerationFalling = 0.0f;  // Removes air drag so you keep full sprint momentum
	
	// 5. Hardcore Tactical Gravity (Crushes the air apex hang time)
	GetCharacterMovement()->GravityScale = 2.0f;
	GetCharacterMovement()->JumpZVelocity = 720.0f;
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
    // pass Jump to the character
    Jump();
}

void AProjectCompCharacter::DoJumpEnd()
{
    // pass StopJumping to the character
    StopJumping();
}