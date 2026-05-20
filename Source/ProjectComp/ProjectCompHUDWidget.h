// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompHUDWidget.generated.h"

class AProjectCompCharacter;
class AProjectCompGameState;
class AProjectCompPlayerState;
class UCanvasPanel;
class UHorizontalBox;
class UImage;
class UProjectCompHealthComponent;
class UTextBlock;
class UVerticalBox;

/**
 * Main HUD widget — built entirely in C++. No WBP required.
 *
 * Layout (Valorant-style centered top + balanced corners):
 *   - Top-center:  [Attacker Score]  [Phase Label / Timer]  [Defender Score]
 *   - Bottom-left:  HP number + "HP" label
 *   - Bottom-right: "ABILITY" label + ready indicator
 *
 * Visual style: cold-white text on transparent, team colors on scores,
 * health shifts yellow at <50% and red at <25%, ability indicator cyan
 * when ready and dim gray when on cooldown.
 *
 * Updates are pulled from the GameState / PlayerState / HealthComponent
 * each NativeTick (~once per frame). Cheap — only text content updates.
 */
UCLASS(BlueprintType)
class PROJECTCOMP_API UProjectCompHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	// ---- Visual tuning (editable in BP defaults if you subclass) ----------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Colors")
	FLinearColor AttackersColor = FLinearColor(0.92f, 0.30f, 0.28f, 1.0f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Colors")
	FLinearColor DefendersColor = FLinearColor(0.30f, 0.60f, 0.92f, 1.0f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Colors")
	FLinearColor PrimaryTextColor = FLinearColor(0.95f, 0.95f, 0.95f, 1.0f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Colors")
	FLinearColor LabelTextColor = FLinearColor(0.65f, 0.65f, 0.65f, 1.0f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Colors")
	FLinearColor HealthWarningColor = FLinearColor(1.0f, 0.82f, 0.10f, 1.0f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Colors")
	FLinearColor HealthCriticalColor = FLinearColor(1.0f, 0.25f, 0.20f, 1.0f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Colors")
	FLinearColor AbilityReadyColor = FLinearColor(0.45f, 0.92f, 1.0f, 1.0f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Colors")
	FLinearColor AbilityCooldownColor = FLinearColor(0.40f, 0.40f, 0.40f, 0.7f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Layout")
	float TopBarTopMargin = 30.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Layout")
	float CornerPadding = 40.0f;

protected:

	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/** Build the widget tree programmatically. Called once on widget init. */
	void BuildLayout();

	/** Push current game state values into the text/image widgets. */
	void RefreshDisplay();

	// ---- Widget references (held for fast per-frame updates) -------------

	UPROPERTY() TObjectPtr<UTextBlock> AttackerScoreText;
	UPROPERTY() TObjectPtr<UTextBlock> DefenderScoreText;
	UPROPERTY() TObjectPtr<UTextBlock> PhaseLabelText;
	UPROPERTY() TObjectPtr<UTextBlock> PhaseTimerText;
	UPROPERTY() TObjectPtr<UTextBlock> HealthValueText;
	UPROPERTY() TObjectPtr<UTextBlock> HealthLabelText;
	UPROPERTY() TObjectPtr<UTextBlock> AbilityLabelText;
	UPROPERTY() TObjectPtr<UImage> AbilityIndicatorImage;

	// ---- State accessors (null-safe traversal) ---------------------------

	AProjectCompCharacter* GetLocalCharacter() const;
	UProjectCompHealthComponent* GetLocalHealth() const;
	AProjectCompPlayerState* GetLocalPlayerState() const;
	AProjectCompGameState* GetTypedGameState() const;

	FLinearColor GetHealthColorForFraction(float Fraction) const;
	FLinearColor GetTeamColor(ETeamID Team) const;
	FText FormatPhaseText(ERoundPhase Phase) const;
	FText FormatTimeText(float Seconds) const;
};
