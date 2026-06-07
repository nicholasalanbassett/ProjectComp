// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "ProjectCompHUD.generated.h"

class UProjectCompHUDWidget;
class UUserWidget;

/**
 * HUD owner. Constructs and tears down the main HUD widget for the local
 * player. No drawing logic of its own — everything visual is UMG.
 *
 * Two HUD paths: if DesignerHUDWidgetClass is set, that plain WBP is used (the
 * designer-authored UMG redesign); otherwise the C++ UProjectCompHUDWidget is
 * created. Lets the WBP migration happen incrementally without losing the
 * working C++ HUD as a fallback.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompHUD : public AHUD
{
	GENERATED_BODY()

public:
	AProjectCompHUD();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Accessor for the active HUD widget. PlayerController uses this to route
	 *  input (e.g. B = toggle buy menu) into widget state. May be null before
	 *  BeginPlay or after EndPlay. */
	UProjectCompHUDWidget* GetHUDWidget() const { return HUDWidget; }

protected:

	/** Fallback C++-built HUD widget class (or a BP subclass of it). Used only
	 *  when DesignerHUDWidgetClass is unset. */
	UPROPERTY(EditDefaultsOnly, Category = "HUD")
	TSubclassOf<UProjectCompHUDWidget> HUDWidgetClass;

	/** Designer-authored WBP HUD (a plain UUserWidget). When set, this is used
	 *  instead of the C++ HUD — the path for the full UMG redesign. */
	UPROPERTY(EditDefaultsOnly, Category = "HUD")
	TSubclassOf<UUserWidget> DesignerHUDWidgetClass;

	/** Active C++ widget instance (null when the designer WBP is in use). */
	UPROPERTY()
	TObjectPtr<UProjectCompHUDWidget> HUDWidget;

	/** Active designer WBP instance (null when the C++ HUD is in use). */
	UPROPERTY()
	TObjectPtr<UUserWidget> DesignerHUDWidget;
};
