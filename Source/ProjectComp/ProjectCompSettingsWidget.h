// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ProjectCompSettingsWidget.generated.h"

class UButton;
class UTextBlock;
class UVerticalBox;
class UHorizontalBox;
class UCanvasPanel;

/**
 * Press-P performance settings overlay. Four tiers (Low/Medium/High/Epic) shown
 * as a horizontal row of buttons; clicking one calls into the PlayerController's
 * Perf exec and persists via UGameUserSettings so the choice survives launches.
 *
 * The currently-applied tier is highlighted (bright cyan label + accent border)
 * every tick by reading PC->GetCurrentPerfLevel(). No BP subclass required —
 * the widget builds its own tree in C++.
 */
UCLASS(BlueprintType)
class PROJECTCOMP_API UProjectCompSettingsWidget : public UUserWidget
{
	GENERATED_BODY()

protected:

	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& InGeometry, float InDeltaTime) override;

	/** Build the widget tree programmatically. */
	void BuildLayout();

	/** Push current scalability level into the button highlights. Cheap; runs each tick. */
	void RefreshHighlight();

	/** Single entry point for tier application — used by all four button callbacks. */
	void ApplyTier(int32 Level);

	// ---- Widget tree references ------------------------------------------

	UPROPERTY() TObjectPtr<UButton> BtnLow;
	UPROPERTY() TObjectPtr<UButton> BtnMedium;
	UPROPERTY() TObjectPtr<UButton> BtnHigh;
	UPROPERTY() TObjectPtr<UButton> BtnEpic;

	UPROPERTY() TObjectPtr<UTextBlock> LblLow;
	UPROPERTY() TObjectPtr<UTextBlock> LblMedium;
	UPROPERTY() TObjectPtr<UTextBlock> LblHigh;
	UPROPERTY() TObjectPtr<UTextBlock> LblEpic;

	UPROPERTY() TObjectPtr<UTextBlock> TitleText;
	UPROPERTY() TObjectPtr<UTextBlock> HintText;

	// ---- Click handlers --------------------------------------------------

	UFUNCTION() void OnClickLow();
	UFUNCTION() void OnClickMedium();
	UFUNCTION() void OnClickHigh();
	UFUNCTION() void OnClickEpic();

	/** Last level applied to the highlight so we only repaint on change. */
	int32 LastHighlightedLevel = -1;
};
