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

	/** Owning-client entry point: claim a Loom ability during PreRound selection window.
	 *  First-come-first-served per team (no duplicates). Server arbitrates. */
	UFUNCTION(BlueprintCallable, Category = "Loom")
	void ClaimLoomAbility(EAbilityType Ability);

	/** Owning-client entry point: purchase a grenade during PreRound selection window. */
	UFUNCTION(BlueprintCallable, Category = "Grenade")
	void BuyGrenade(EGrenadeType Type);

	// ---- Dev/test console hooks (PIE listen-server convenience) -----------
	// Use the UE console (~) and call e.g. `BuyNade 1` to buy a Flash.
	// Indices for BuyNade: 1=Flash 2=Info 3=Vulnerable 4=Sound.
	// Indices for ClaimLoom: 1=AA 2=AC 3=AM 4=CC 5=CA 6=CM 7=MM 8=MA 9=MC.

	UFUNCTION(Exec)
	void BuyNade(int32 TypeIndex);

	UFUNCTION(Exec)
	void ClaimLoom(int32 AbilityIndex);

	/** CHEAT: server-only-effective credit top-up for testing the buy phase. */
	UFUNCTION(Exec)
	void GiveCredits(int32 Amount);

	/** Logs a one-line snapshot of phase, selection window, and the calling
	 *  player's economy/Loom/grenade state to the Output Log. */
	UFUNCTION(Exec)
	void DumpEcon();

	/** CHEAT: force-end the current RoundActive with the given team as winner.
	 *  TeamIndex: 0=Attackers 1=Defenders. */
	UFUNCTION(Exec)
	void EndRound(int32 TeamIndex);

	/** Throw / place the equipped grenade of the given type from the caller's
	 *  pawn (origin = pawn location, direction = camera forward). Indices:
	 *  1=Flash 2=Info 3=Vulnerable 4=Sound. RoundActive only.
	 *  AltFire (0/1, default 0): FLASH right-click self-flash, others ignore. */
	UFUNCTION(Exec)
	void UseNade(int32 TypeIndex, int32 AltFire = 0);

	/** Toggle the caller's active SOUND grenade on/off. Only works while
	 *  the caller is the original placer and the grenade still exists. */
	UFUNCTION(Exec)
	void ToggleSound();

	/** Reposition the caller's active SOUND grenade to where their camera
	 *  is currently looking (or pawn location if no camera). Valid once per
	 *  round; rejected if used previously or if not the original placer. */
	UFUNCTION(Exec)
	void ReposSound();

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

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_ClaimLoomAbility(EAbilityType Ability);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_BuyGrenade(EGrenadeType Type);

	/** CHEAT path: tops up the caller's credits on the server. Unconditional —
	 *  this is dev-only and should be stripped or gated for production. */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_CheatAddCredits(int32 Amount);

	/** CHEAT path: force-end the current RoundActive with the given winner. */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_CheatEndRound(ETeamID WinningTeam);

	/** Server RPC: forward a grenade-use request to the pawn's GrenadeComponent.
	 *  Origin and Direction are computed client-side from the pawn pose, then
	 *  re-validated server-side inside the component.
	 *  bAltFire: subclass-specific secondary mode (FLASH right-click = self-flash). */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_UseGrenade(EGrenadeType Type, bool bAltFire, FVector Origin, FVector Direction);

	/** Server RPC: toggle the caller's placed SOUND grenade on/off. Server
	 *  finds the caller's grenade via IsPlacedBy() ownership check. */
	UFUNCTION(Server, Reliable)
	void Server_ToggleSoundGrenade();

	/** Server RPC: reposition the caller's placed SOUND grenade to NewOrigin.
	 *  Validates: caller is the placer, grenade still has reposition uses
	 *  remaining, caller is not permanently dead. */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_RepositionSoundGrenade(FVector NewOrigin);

private:

	/** Lazily-created instance of the minimap widget. Owned by this PC, hidden
	 *  by default, added/removed from viewport via SetMinimapOpen. */
	UPROPERTY()
	TObjectPtr<UProjectCompMinimapWidget> MinimapInstance;

	/** True while MinimapInstance is on the viewport. */
	bool bMinimapOpen = false;
};
