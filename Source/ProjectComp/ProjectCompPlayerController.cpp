// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompPlayerController.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "InputMappingContext.h"
#include "ProjectCompCameraManager.h"
#include "ProjectCompGameMode.h"
#include "Engine/World.h"

AProjectCompPlayerController::AProjectCompPlayerController()
{
	PlayerCameraManagerClass = AProjectCompCameraManager::StaticClass();
}

void AProjectCompPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (!IsLocalPlayerController())
	{
		return;
	}

	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		for (UInputMappingContext* Context : DefaultMappingContexts)
		{
			Subsystem->AddMappingContext(Context, 0);
		}
	}
}

// =====================================================================
// BP-callable entry points
// =====================================================================

void AProjectCompPlayerController::SubmitAbilityVote(EAbilityType First, EAbilityType Second)
{
	Server_SubmitAbilityVote(First, Second);
}

void AProjectCompPlayerController::RequestAbilityCast(EAbilityType Type, FVector TargetLocation)
{
	Server_RequestAbilityCast(Type, TargetLocation);
}

// =====================================================================
// Server RPCs — _Validate runs first (drop the connection on cheating),
// then _Implementation forwards to the GameMode for authoritative logic.
// =====================================================================

bool AProjectCompPlayerController::Server_SubmitAbilityVote_Validate(EAbilityType First, EAbilityType Second)
{
	// Cheap sanity: enum range + distinct + non-None. The GameMode re-checks
	// phase and team membership, which can change between client send and
	// server receive.
	const uint8 FirstByte = static_cast<uint8>(First);
	const uint8 SecondByte = static_cast<uint8>(Second);
	const uint8 MaxValid = static_cast<uint8>(EAbilityType::MC);
	return FirstByte != 0 && FirstByte <= MaxValid
		&& SecondByte != 0 && SecondByte <= MaxValid
		&& First != Second;
}

void AProjectCompPlayerController::Server_SubmitAbilityVote_Implementation(EAbilityType First, EAbilityType Second)
{
	if (AProjectCompGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
	{
		GM->Server_SubmitAbilityVote(this, First, Second);
	}
}

bool AProjectCompPlayerController::Server_RequestAbilityCast_Validate(EAbilityType Type, FVector TargetLocation)
{
	// Cheap sanity: enum range + reasonable target location (rejects NaN/Inf
	// and absurd magnitudes that indicate a tampered client).
	const uint8 TypeByte = static_cast<uint8>(Type);
	const uint8 MaxValid = static_cast<uint8>(EAbilityType::MC);
	if (TypeByte == 0 || TypeByte > MaxValid)
	{
		return false;
	}
	if (TargetLocation.ContainsNaN())
	{
		return false;
	}
	// 1,000,000 cm = 10 km, well beyond any realistic playspace.
	if (TargetLocation.SizeSquared() > FMath::Square(1000000.0f))
	{
		return false;
	}
	return true;
}

void AProjectCompPlayerController::Server_RequestAbilityCast_Implementation(EAbilityType Type, FVector TargetLocation)
{
	if (AProjectCompGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
	{
		GM->Server_RequestAbilityCast(this, Type, TargetLocation);
	}
}
