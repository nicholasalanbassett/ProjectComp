// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompHUDWidget.generated.h"

class AProjectCompCharacter;
class AProjectCompGameState;
class AProjectCompPlayerState;
class AProjectCompBombSite;
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

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Colors")
	FLinearColor RespawnTextColor = FLinearColor(0.95f, 0.95f, 0.95f, 1.0f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Colors")
	FLinearColor EliminatedTextColor = FLinearColor(0.95f, 0.30f, 0.28f, 1.0f);

	/** Color for "YOU KILLED [Name]" personal notification. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Colors")
	FLinearColor KillNotifColor = FLinearColor(1.0f, 0.85f, 0.20f, 1.0f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Layout")
	float TopBarTopMargin = 30.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Layout")
	float CornerPadding = 40.0f;

	// ---- Crosshair tuning -------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Crosshair")
	FLinearColor CrosshairColor = FLinearColor(0.98f, 0.98f, 0.98f, 1.0f);

	/** Length of each of the four ticks (pixels). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Crosshair", meta = (ClampMin = "0.0"))
	float CrosshairLineLength = 8.0f;

	/** Thickness of each tick (pixels). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Crosshair", meta = (ClampMin = "0.5"))
	float CrosshairLineThickness = 2.0f;

	/** Pixel gap between the screen-center and the inner edge of each tick. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Crosshair", meta = (ClampMin = "0.0"))
	float CrosshairGap = 4.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Crosshair")
	bool bShowCenterDot = true;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD|Crosshair", meta = (ClampMin = "0.5"))
	float CenterDotSize = 2.0f;

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

	/** Bottom-right ammo readout: "30 / 120" (current mag / total reserve bullets).
	 *  Replaced by AmmoReloadingText while a reload is in progress. */
	UPROPERTY() TObjectPtr<UTextBlock> AmmoValueText;
	/** "AMMO" label below the value, matches the HP label visual treatment. */
	UPROPERTY() TObjectPtr<UTextBlock> AmmoLabelText;
	/** Container for the ammo column so reload-state visibility can toggle cleanly. */
	UPROPERTY() TObjectPtr<UVerticalBox> AmmoBox;
	UPROPERTY() TObjectPtr<UVerticalBox> DeathStatusBox;
	UPROPERTY() TObjectPtr<UTextBlock> DeathStatusTitle;
	UPROPERTY() TObjectPtr<UTextBlock> DeathStatusSubtitle;

	/** Center-screen bomb status: plant/defuse progress + planted-banner + detonation countdown. */
	UPROPERTY() TObjectPtr<UVerticalBox> BombStatusBox;
	UPROPERTY() TObjectPtr<UTextBlock> BombStatusTitle;
	UPROPERTY() TObjectPtr<UTextBlock> BombStatusSubtitle;

	/** Full-screen white image rendered when the local player is flashed. Alpha
	 *  is driven each tick from PlayerState->GetFlashEndTime() so the fade looks
	 *  smooth even though only end-time + fade-start-time replicate. */
	UPROPERTY() TObjectPtr<UImage> FlashOverlay;

	/** Update FlashOverlay alpha + visibility from local PlayerState's flash times.
	 *  Returns the alpha that was applied (mainly useful for testing). */
	float RefreshFlashOverlay();

	// ---- Kill feed (top-right) + personal notification (bottom-center) ------

	/** Vertical container holding kill feed rows. */
	UPROPERTY() TObjectPtr<UVerticalBox> KillFeedBox;

	/** Bottom-center "YOU KILLED / KILLED BY" flash. */
	UPROPERTY() TObjectPtr<UTextBlock> KillNotifText;

	/** Live kill-feed rows (parallel to KillFeedTimers). UPROPERTY keeps GC safe. */
	UPROPERTY() TArray<TObjectPtr<UHorizontalBox>> KillFeedRows;

	/** Remaining display time (seconds) for each row, mirroring KillFeedRows. */
	TArray<float> KillFeedTimers;

	/** Remaining display time for the personal kill/death notification. */
	float KillNotifTimeRemaining = 0.0f;

	/** Consume pending events from GameState and advance fade timers. */
	void UpdateKillFeed(float DeltaTime);

	/** Build and insert one row into the kill feed. */
	void AddKillFeedEntry(const FString& KillerName, const FString& VictimName, ETeamID KillerTeam, ETeamID VictimTeam);

	// ---- State accessors (null-safe traversal) ---------------------------

	AProjectCompCharacter* GetLocalCharacter() const;
	UProjectCompHealthComponent* GetLocalHealth() const;
	AProjectCompPlayerState* GetLocalPlayerState() const;
	AProjectCompGameState* GetTypedGameState() const;

	FLinearColor GetHealthColorForFraction(float Fraction) const;
	FLinearColor GetTeamColor(ETeamID Team) const;
	FText FormatPhaseText(ERoundPhase Phase) const;
	FText FormatTimeText(float Seconds) const;

	/** Update BombStatusBox text/visibility from any active bomb site in the world. */
	void RefreshBombStatus();

	/** Server time at which the "BOMB DEFUSED" banner should stop showing. 0 = inactive. */
	float DefuseMessageEndTime = 0.0f;

	/** Bomb sites discovered at HUD init. Bomb sites are level-placed and never spawned
	 *  at runtime, so a one-shot scan in NativeOnInitialized avoids per-frame TActorIterator
	 *  walks of the whole actor table on every NativeTick. */
	UPROPERTY()
	TArray<TWeakObjectPtr<AProjectCompBombSite>> CachedBombSites;

	void CacheBombSites();
};
