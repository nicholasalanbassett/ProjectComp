// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "ProjectCompPlayerController.generated.h"

class UInputMappingContext;

/**
 *  Player controller for the tac-FPS character.
 *  Owns input mapping contexts and configures the camera manager class.
 */
UCLASS(abstract, config="Game")
class PROJECTCOMP_API AProjectCompPlayerController : public APlayerController
{
	GENERATED_BODY()

public:

	AProjectCompPlayerController();

protected:

	/** Input mapping contexts pushed onto the EnhancedInput subsystem on possession. */
	UPROPERTY(EditAnywhere, Category="Input")
	TArray<UInputMappingContext*> DefaultMappingContexts;

	virtual void SetupInputComponent() override;
};
