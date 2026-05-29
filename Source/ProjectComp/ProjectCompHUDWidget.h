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
class UButton;
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

	/** Bottom-center equipped-item indicator row. Three vertical boxes side
	 *  by side, each containing a cooldown text on top and a label text
	 *  below: cooldown text shows seconds remaining only when the grenade
	 *  is on CD (hidden otherwise); label is "[key] NAME". The currently-
	 *  equipped slot glows in its grenade-specific color (weapon = team color).
	 *  Equipped state from AProjectCompCharacter::GetEquippedGrenadeSlot. */
	UPROPERTY() TObjectPtr<UTextBlock> EquipWeaponText;
	UPROPERTY() TObjectPtr<UTextBlock> EquipSlot1Text;
	UPROPERTY() TObjectPtr<UTextBlock> EquipSlot2Text;

	/** Cooldown countdown texts. Weapon's slot has no CD so its text stays
	 *  permanently hidden — kept around so the three columns align vertically
	 *  even when only the grenades show counts. */
	UPROPERTY() TObjectPtr<UTextBlock> CooldownWeaponText;
	UPROPERTY() TObjectPtr<UTextBlock> CooldownSlot1Text;
	UPROPERTY() TObjectPtr<UTextBlock> CooldownSlot2Text;

	/** Pull the equipped state + kit into the indicator widgets each tick. */
	void RefreshEquipIndicator();

	/** Per-grenade brand color used for the equipped-state glow. Matches the
	 *  design palette: FLASH=YELLOW, INFO=CYAN, VULN=NEON CORAL, SOUND=GREY. */
	static FLinearColor GetGrenadeBrandColor(EGrenadeType Type);

	// =====================================================================
	// PreRound buy panel (centered overlay during LoomSelection + PreRound
	// buy window). 3 columns: Weapons | Grenades | Loom abilities. Header
	// shows live credits, phase timer, and phase label.
	// =====================================================================

	/** Root container for the whole buy panel. Visibility is driven by
	 *  RefreshBuyPanelVisibility based on the current ERoundPhase. */
	UPROPERTY() TObjectPtr<UVerticalBox> BuyPanelRoot;

	/** Header texts. */
	UPROPERTY() TObjectPtr<UTextBlock> BuyHeaderCredits;
	UPROPERTY() TObjectPtr<UTextBlock> BuyHeaderTimer;
	UPROPERTY() TObjectPtr<UTextBlock> BuyHeaderPhase;

	/** Column containers — visibility per phase: Weapons + Grenades visible
	 *  during PreRound buy window; Loom visible during LoomSelection. */
	UPROPERTY() TObjectPtr<UVerticalBox> BuyWeaponsColumn;
	UPROPERTY() TObjectPtr<UVerticalBox> BuyGrenadesColumn;
	UPROPERTY() TObjectPtr<UVerticalBox> BuyLoomColumn;

	/** Weapon buttons (primary, secondary, armour). */
	UPROPERTY() TObjectPtr<UButton> BtnBuyPistol;
	UPROPERTY() TObjectPtr<UButton> BtnBuySMG;
	UPROPERTY() TObjectPtr<UButton> BtnBuyRifle;
	UPROPERTY() TObjectPtr<UButton> BtnBuyArmourLight;
	UPROPERTY() TObjectPtr<UButton> BtnBuyArmourHeavy;

	/** Grenade buttons. */
	UPROPERTY() TObjectPtr<UButton> BtnBuyFlash;
	UPROPERTY() TObjectPtr<UButton> BtnBuyInfo;
	UPROPERTY() TObjectPtr<UButton> BtnBuyVuln;
	UPROPERTY() TObjectPtr<UButton> BtnBuySound;

	/** Loom ability buttons (9-cell matrix). */
	UPROPERTY() TObjectPtr<UButton> BtnClaimAA;
	UPROPERTY() TObjectPtr<UButton> BtnClaimAC;
	UPROPERTY() TObjectPtr<UButton> BtnClaimAM;
	UPROPERTY() TObjectPtr<UButton> BtnClaimCC;
	UPROPERTY() TObjectPtr<UButton> BtnClaimCA;
	UPROPERTY() TObjectPtr<UButton> BtnClaimCM;
	UPROPERTY() TObjectPtr<UButton> BtnClaimMM;
	UPROPERTY() TObjectPtr<UButton> BtnClaimMA;
	UPROPERTY() TObjectPtr<UButton> BtnClaimMC;

	/** Teammate loadouts column — shown next to the buy table during the
	 *  PreRound buy window. Each row is a VBox containing a loadout label
	 *  text plus a horizontal row of fulfill buttons (rebuilt per tick from
	 *  the teammate's RequestedWeapons/Armour/Grenades arrays). */
	UPROPERTY() TObjectPtr<UVerticalBox> TeammateLoadoutsColumn;
	UPROPERTY() TArray<TObjectPtr<UVerticalBox>>   TeammateRowContainers;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>>     TeammateLoadoutLabels;
	UPROPERTY() TArray<TObjectPtr<UHorizontalBox>> TeammateRequestsBoxes;

	/** Build the buy panel widget tree (called once in BuildLayout). */
	void BuildBuyPanel(UCanvasPanel* RootCanvas);

	/** Per-tick: update credits / timer / phase label + button enable states. */
	void RefreshBuyPanel();

	/** Set per-column + root visibility based on current phase. */
	void RefreshBuyPanelVisibility();

public:

	/** Toggle the buy menu's open state. During PreRound the panel only renders
	 *  while open; LoomSelection ignores this flag (selection is mandatory). */
	void ToggleBuyMenu() { SetBuyMenuOpen(!bBuyMenuOpen); }

	/** Set the buy menu's open state explicitly. Phase entry into PreRound
	 *  also force-resets this to false so each new round starts unopened. */
	void SetBuyMenuOpen(bool bOpen) { bBuyMenuOpen = bOpen; }

	bool IsBuyMenuOpen() const { return bBuyMenuOpen; }

protected:

	/** Tracks toggle state for the PreRound buy panel. LoomSelection bypasses this. */
	bool bBuyMenuOpen = false;

	/** Last phase observed by RefreshBuyPanelVisibility. Used to detect entry
	 *  into PreRound so the menu auto-resets to closed each round. */
	ERoundPhase LastVisibilityPhase = ERoundPhase::PostRound;

	// ---- Button click handlers (one per buy/claim target) ---------------

	UFUNCTION() void OnClickBuyPistol();
	UFUNCTION() void OnClickBuySMG();
	UFUNCTION() void OnClickBuyRifle();
	UFUNCTION() void OnClickBuyArmourLight();
	UFUNCTION() void OnClickBuyArmourHeavy();
	UFUNCTION() void OnClickBuyFlash();
	UFUNCTION() void OnClickBuyInfo();
	UFUNCTION() void OnClickBuyVuln();
	UFUNCTION() void OnClickBuySound();
	UFUNCTION() void OnClickClaimAA();
	UFUNCTION() void OnClickClaimAC();
	UFUNCTION() void OnClickClaimAM();
	UFUNCTION() void OnClickClaimCC();
	UFUNCTION() void OnClickClaimCA();
	UFUNCTION() void OnClickClaimCM();
	UFUNCTION() void OnClickClaimMM();
	UFUNCTION() void OnClickClaimMA();
	UFUNCTION() void OnClickClaimMC();

	// ---- Right-click on buy buttons (request item) ----------------------
	// Right-click is captured by NativeOnPreviewMouseButtonDown on the HUD
	// widget itself — UButton's SButton consumes left-clicks but not right-
	// clicks, so preview-level interception is the cleanest path.

	virtual FReply NativeOnPreviewMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

	/** Identify which (if any) buy button is under the cursor at right-click. */
	class UButton* FindBuyButtonUnderCursor(const FVector2D& ScreenPos) const;

	/** Map a right-clicked buy button to its Request* RPC. */
	void DispatchRightClickRequest(class UButton* Btn);

	/** Bound to each spawned fulfill button's OnFulfillClicked — reads the
	 *  button's payload (RequesterPC + Kind + ItemId) and forwards to the
	 *  matching Server_FulfillX RPC on the local PC. */
	UFUNCTION() void OnFulfillRequestClicked(class UProjectCompFulfillButton* Button);

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
