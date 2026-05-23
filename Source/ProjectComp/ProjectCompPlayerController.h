// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
class UProjectCompMinimapWidget;

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

	/** Owning-client entry point: toggle the Loom cast minimap. Only effective
	 *  while looming — alive callers are silently ignored. Bound to the
	 *  MinimapAction input but BP-callable for menus / cheats. */
	UFUNCTION(BlueprintCallable, Category = "Loom")
	void ToggleMinimap();

protected:

	UPROPERTY(EditAnywhere, Category="Input")
	TArray<UInputMappingContext*> DefaultMappingContexts;

	/** Press to toggle the Loom cast minimap. Looming players only. */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* MinimapAction;

	/** Subclass of UProjectCompMinimapWidget to instantiate for this PC. Set
	 *  to BP_MinimapWidget on the PlayerController BP defaults so designers
	 *  can per-level swap textures + world bounds without touching C++. */
	UPROPERTY(EditDefaultsOnly, Category="Loom")
	TSubclassOf<UProjectCompMinimapWidget> MinimapWidgetClass;

	virtual void SetupInputComponent() override;
	virtual void OnPossess(APawn* InPawn) override;

	/** Show/hide the minimap. Stricter version of ToggleMinimap that doesn't
	 *  toggle — used by lifecycle events (death/respawn) to force a state. */
	void SetMinimapOpen(bool bOpen);

	/** Lazily create the widget instance on first open. Returns null on failure. */
	UProjectCompMinimapWidget* EnsureMinimapWidget();

	// ---- Server RPCs (declared with full _Validate + _Implementation) ------

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_SubmitAbilityVote(EAbilityType First, EAbilityType Second);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_RequestAbilityCast(EAbilityType Type, FVector TargetLocation);

private:

	/** Lazily-created instance of the minimap widget. Owned by this PC, hidden
	 *  by default, added/removed from viewport via SetMinimapOpen. */
	UPROPERTY()
	TObjectPtr<UProjectCompMinimapWidget> MinimapInstance;

	/** True while MinimapInstance is on the viewport. */
	bool bMinimapOpen = false;
};
