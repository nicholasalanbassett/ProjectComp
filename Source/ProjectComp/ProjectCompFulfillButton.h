// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/Button.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompFulfillButton.generated.h"

class APlayerController;

/**
 * Identifies what a UProjectCompFulfillButton fulfills. The HUD's click
 * handler reads Kind + Id to know which Server_FulfillX RPC to call.
 */
UENUM()
enum class EFulfillItemKind : uint8
{
	None = 0,
	Weapon,
	Armour,
	Grenade,
};

/**
 * Specialized button used in the HUD's teammates column. Each instance carries:
 *   - A weak ptr to the requester PlayerController
 *   - An item kind + id describing what they're requesting
 *
 * On a left-click, the button broadcasts itself (the typed pointer with payload
 * intact) to whichever handler is bound to OnFulfillClicked. The HUD widget
 * reads Kind/Id and forwards to the matching Server_FulfillX RPC on the local
 * player's controller.
 *
 * This works around the fact that UButton's native OnClicked is parameterless —
 * we can't capture per-button state in a UFUNCTION handler without a custom
 * payload-carrying subclass.
 */
UCLASS()
class PROJECTCOMP_API UProjectCompFulfillButton : public UButton
{
	GENERATED_BODY()

public:
	UProjectCompFulfillButton();

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnFulfillClickedDelegate, UProjectCompFulfillButton*, Button);

	/** Bind once on construction; fires every left-click. The HUD reads
	 *  RequesterPC + Kind + Id off the broadcasting button. */
	UPROPERTY(BlueprintAssignable, Category = "Fulfill")
	FOnFulfillClickedDelegate OnFulfillClicked;

	/** The PlayerController that requested this item. */
	UPROPERTY()
	TWeakObjectPtr<APlayerController> RequesterPC;

	UPROPERTY()
	EFulfillItemKind Kind = EFulfillItemKind::None;

	/** Cast from EWeaponClass / EArmourLevel / EGrenadeType depending on Kind. */
	UPROPERTY()
	uint8 ItemId = 0;

private:
	UFUNCTION()
	void HandleNativeClicked();
};
