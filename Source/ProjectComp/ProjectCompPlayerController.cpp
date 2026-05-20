// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompPlayerController.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "InputMappingContext.h"
#include "ProjectCompCameraManager.h"

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
