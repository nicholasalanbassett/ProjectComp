// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompPlayerController.generated.h"

class UInputMappingContext;

/**
 *  Player controller for the tac-FPS character.
 *
 *  Owns:
 *    - Input mapping contexts (added to the Enhanced Input subsystem on login)
 *    - Custom camera manager class assignment
 *    - Client-to-server RPC entry points for the Loom system
 *      (ability draft votes + cast requests). RPCs validate cheaply on the
 *      client side then forward to the GameMode for full authoritative checks.
 */
UCLASS(abstract, config="Game")
class PROJECTCOMP_API AProjectCompPlayerController : public APlayerController
{
	GENERATED_BODY()

public:

	AProjectCompPlayerController();

	// ---- BP-callable helpers that issue the RPCs ---------------------------

	/** Owning-client entry point: submit your two votes for this PreRound's draft. */
	UFUNCTION(BlueprintCallable, Category = "Ability")
	void SubmitAbilityVote(EAbilityType First, EAbilityType Second);

	/** Owning-client entry point: request a Loom ability cast (player must be looming). */
	UFUNCTION(BlueprintCallable, Category = "Ability")
	void RequestAbilityCast(EAbilityType Type, FVector TargetLocation);

protected:

	UPROPERTY(EditAnywhere, Category="Input")
	TArray<UInputMappingContext*> DefaultMappingContexts;

	virtual void SetupInputComponent() override;

	// ---- Server RPCs (declared with full _Validate + _Implementation) ------

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_SubmitAbilityVote(EAbilityType First, EAbilityType Second);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_RequestAbilityCast(EAbilityType Type, FVector TargetLocation);
};
