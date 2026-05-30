// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompHUDWidget.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompFulfillButton.h"
#include "ProjectCompGameMode.h"
#include "ProjectCompHealthComponent.h"
#include "ProjectCompHitscanWeapon.h"
#include "ProjectCompGameState.h"
#include "ProjectCompPlayerController.h"
#include "ProjectCompPlayerState.h"
#include "ProjectCompBombSite.h"
#include "EngineUtils.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Fonts/SlateFontInfo.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "Styling/SlateColor.h"
#include "Engine/World.h"
#include "Engine/Font.h"

namespace
{
	/** Build a Roboto Slate font (engine default) at the given size. */
	FSlateFontInfo MakeFont(float Size)
	{
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Regular", static_cast<int32>(Size));
		// Subtle drop shadow for legibility on any background.
		Font.OutlineSettings.OutlineSize = 0;
		return Font;
	}

	/** Apply a standard shadow offset + color to a text block. */
	void ApplyTextShadow(UTextBlock* Text)
	{
		if (!Text) return;
		Text->SetShadowOffset(FVector2D(1.0f, 1.0f));
		Text->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.55f));
	}
}

// =====================================================================
// Lifecycle
// =====================================================================

void UProjectCompHUDWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
	CacheBombSites();
	RefreshDisplay();
}

void UProjectCompHUDWidget::CacheBombSites()
{
	CachedBombSites.Reset();
	if (!GetWorld())
	{
		return;
	}
	// Bomb sites are level-placed and never spawn at runtime — one walk on init
	// is enough. If level streaming ever spawns sites mid-match, this needs
	// converting to a delegate or rebuild on phase change.
	for (TActorIterator<AProjectCompBombSite> It(GetWorld()); It; ++It)
	{
		CachedBombSites.Add(*It);
	}
}

void UProjectCompHUDWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	RefreshDisplay();
	UpdateKillFeed(InDeltaTime);
	RefreshFlashOverlay();
}

// =====================================================================
// Layout construction
// =====================================================================

void UProjectCompHUDWidget::BuildLayout()
{
	if (!WidgetTree)
	{
		return;
	}

	// ---- Root Canvas Panel -------------------------------------------------
	UCanvasPanel* RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
	WidgetTree->RootWidget = RootCanvas;

	// ---- Top-center: scores + phase/timer ---------------------------------
	UHorizontalBox* TopRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("TopRow"));
	UCanvasPanelSlot* TopRowSlot = RootCanvas->AddChildToCanvas(TopRow);
	TopRowSlot->SetAnchors(FAnchors(0.5f, 0.0f));
	TopRowSlot->SetAlignment(FVector2D(0.5f, 0.0f));
	TopRowSlot->SetPosition(FVector2D(0.0f, TopBarTopMargin));
	TopRowSlot->SetAutoSize(true);

	// Attacker score (left)
	AttackerScoreText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("AttackerScoreText"));
	AttackerScoreText->SetFont(MakeFont(64));
	AttackerScoreText->SetText(FText::FromString(TEXT("0")));
	AttackerScoreText->SetColorAndOpacity(AttackersColor);
	ApplyTextShadow(AttackerScoreText);
	UHorizontalBoxSlot* AttackerSlot = TopRow->AddChildToHorizontalBox(AttackerScoreText);
	AttackerSlot->SetPadding(FMargin(24.0f, 0.0f));
	AttackerSlot->SetVerticalAlignment(VAlign_Center);

	// Center: vertical box with phase label + timer
	UVerticalBox* CenterColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("CenterColumn"));
	UHorizontalBoxSlot* CenterSlot = TopRow->AddChildToHorizontalBox(CenterColumn);
	CenterSlot->SetPadding(FMargin(40.0f, 0.0f));
	CenterSlot->SetVerticalAlignment(VAlign_Center);

	PhaseLabelText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("PhaseLabelText"));
	PhaseLabelText->SetFont(MakeFont(24));
	PhaseLabelText->SetText(FText::FromString(TEXT("—")));
	PhaseLabelText->SetColorAndOpacity(LabelTextColor);
	PhaseLabelText->SetJustification(ETextJustify::Center);
	ApplyTextShadow(PhaseLabelText);
	UVerticalBoxSlot* PhaseLabelSlot = CenterColumn->AddChildToVerticalBox(PhaseLabelText);
	PhaseLabelSlot->SetHorizontalAlignment(HAlign_Center);

	PhaseTimerText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("PhaseTimerText"));
	PhaseTimerText->SetFont(MakeFont(52));
	PhaseTimerText->SetText(FText::FromString(TEXT("0:00")));
	PhaseTimerText->SetColorAndOpacity(PrimaryTextColor);
	PhaseTimerText->SetJustification(ETextJustify::Center);
	ApplyTextShadow(PhaseTimerText);
	UVerticalBoxSlot* TimerSlot = CenterColumn->AddChildToVerticalBox(PhaseTimerText);
	TimerSlot->SetHorizontalAlignment(HAlign_Center);

	// Defender score (right)
	DefenderScoreText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("DefenderScoreText"));
	DefenderScoreText->SetFont(MakeFont(64));
	DefenderScoreText->SetText(FText::FromString(TEXT("0")));
	DefenderScoreText->SetColorAndOpacity(DefendersColor);
	ApplyTextShadow(DefenderScoreText);
	UHorizontalBoxSlot* DefenderSlot = TopRow->AddChildToHorizontalBox(DefenderScoreText);
	DefenderSlot->SetPadding(FMargin(24.0f, 0.0f));
	DefenderSlot->SetVerticalAlignment(VAlign_Center);

	// ---- Bottom-left: HP value + HP label ---------------------------------
	UVerticalBox* HealthBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("HealthBox"));
	UCanvasPanelSlot* HealthBoxSlot = RootCanvas->AddChildToCanvas(HealthBox);
	HealthBoxSlot->SetAnchors(FAnchors(0.0f, 1.0f));
	HealthBoxSlot->SetAlignment(FVector2D(0.0f, 1.0f));
	HealthBoxSlot->SetPosition(FVector2D(CornerPadding, -CornerPadding));
	HealthBoxSlot->SetAutoSize(true);

	HealthValueText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("HealthValueText"));
	HealthValueText->SetFont(MakeFont(56));
	HealthValueText->SetText(FText::FromString(TEXT("100")));
	HealthValueText->SetColorAndOpacity(PrimaryTextColor);
	ApplyTextShadow(HealthValueText);
	HealthBox->AddChildToVerticalBox(HealthValueText);

	HealthLabelText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("HealthLabelText"));
	HealthLabelText->SetFont(MakeFont(14));
	HealthLabelText->SetText(FText::FromString(TEXT("HP")));
	HealthLabelText->SetColorAndOpacity(LabelTextColor);
	ApplyTextShadow(HealthLabelText);
	HealthBox->AddChildToVerticalBox(HealthLabelText);

	// ---- Bottom-right: ABILITY label + indicator --------------------------
	UHorizontalBox* AbilityBox = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("AbilityBox"));
	UCanvasPanelSlot* AbilityBoxSlot = RootCanvas->AddChildToCanvas(AbilityBox);
	AbilityBoxSlot->SetAnchors(FAnchors(1.0f, 1.0f));
	AbilityBoxSlot->SetAlignment(FVector2D(1.0f, 1.0f));
	AbilityBoxSlot->SetPosition(FVector2D(-CornerPadding, -CornerPadding));
	AbilityBoxSlot->SetAutoSize(true);

	AbilityLabelText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("AbilityLabelText"));
	AbilityLabelText->SetFont(MakeFont(16));
	AbilityLabelText->SetText(FText::FromString(TEXT("ABILITY")));
	AbilityLabelText->SetColorAndOpacity(LabelTextColor);
	ApplyTextShadow(AbilityLabelText);
	UHorizontalBoxSlot* AbilityLabelSlot = AbilityBox->AddChildToHorizontalBox(AbilityLabelText);
	AbilityLabelSlot->SetPadding(FMargin(0.0f, 0.0f, 12.0f, 0.0f));
	AbilityLabelSlot->SetVerticalAlignment(VAlign_Center);

	AbilityIndicatorImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("AbilityIndicatorImage"));
	AbilityIndicatorImage->SetColorAndOpacity(AbilityCooldownColor);
	AbilityIndicatorImage->SetDesiredSizeOverride(FVector2D(20.0f, 20.0f));
	UHorizontalBoxSlot* AbilityIndicatorSlot = AbilityBox->AddChildToHorizontalBox(AbilityIndicatorImage);
	AbilityIndicatorSlot->SetVerticalAlignment(VAlign_Center);

	// ---- Bottom-right (stacked above Ability): AMMO value + label ---------
	// Anchored bottom-right with extra Y offset so the column sits cleanly
	// above the Ability row regardless of viewport size.
	AmmoBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("AmmoBox"));
	UCanvasPanelSlot* AmmoBoxSlot = RootCanvas->AddChildToCanvas(AmmoBox);
	AmmoBoxSlot->SetAnchors(FAnchors(1.0f, 1.0f));
	AmmoBoxSlot->SetAlignment(FVector2D(1.0f, 1.0f));
	AmmoBoxSlot->SetPosition(FVector2D(-CornerPadding, -CornerPadding - 80.0f));
	AmmoBoxSlot->SetAutoSize(true);

	AmmoValueText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("AmmoValueText"));
	AmmoValueText->SetFont(MakeFont(26));
	AmmoValueText->SetText(FText::FromString(TEXT("30 / 120")));
	AmmoValueText->SetColorAndOpacity(PrimaryTextColor);
	AmmoValueText->SetJustification(ETextJustify::Right);
	ApplyTextShadow(AmmoValueText);
	AmmoBox->AddChildToVerticalBox(AmmoValueText);

	AmmoLabelText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("AmmoLabelText"));
	AmmoLabelText->SetFont(MakeFont(14));
	AmmoLabelText->SetText(FText::FromString(TEXT("AMMO")));
	AmmoLabelText->SetColorAndOpacity(LabelTextColor);
	AmmoLabelText->SetJustification(ETextJustify::Right);
	ApplyTextShadow(AmmoLabelText);
	AmmoBox->AddChildToVerticalBox(AmmoLabelText);

	// ---- Center crosshair (4 ticks + optional center dot) -----------------
	// Each tick is positioned relative to the viewport center via anchor
	// (0.5, 0.5). Sized in pixels — visual fidelity depends on DPI scale
	// (set Project Settings -> UI -> DPI Scaling to Constant 1.0 for
	// pixel-accurate competitive aim feel).
	auto BuildCrosshairTick = [&](const TCHAR* Name, FVector2D Position, FVector2D Size)
	{
		UImage* Tick = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), Name);
		Tick->SetColorAndOpacity(CrosshairColor);
		Tick->SetDesiredSizeOverride(Size);
		UCanvasPanelSlot* Slot = RootCanvas->AddChildToCanvas(Tick);
		Slot->SetAnchors(FAnchors(0.5f, 0.5f));
		Slot->SetAlignment(FVector2D(0.5f, 0.5f));
		Slot->SetPosition(Position);
		Slot->SetSize(Size);
		Slot->SetAutoSize(false);
	};

	const float DistFromCenter = CrosshairGap + (CrosshairLineLength * 0.5f);
	const FVector2D VerticalTickSize(CrosshairLineThickness, CrosshairLineLength);
	const FVector2D HorizontalTickSize(CrosshairLineLength, CrosshairLineThickness);

	BuildCrosshairTick(TEXT("CrosshairTop"),    FVector2D(0.0f, -DistFromCenter), VerticalTickSize);
	BuildCrosshairTick(TEXT("CrosshairBottom"), FVector2D(0.0f,  DistFromCenter), VerticalTickSize);
	BuildCrosshairTick(TEXT("CrosshairLeft"),   FVector2D(-DistFromCenter, 0.0f), HorizontalTickSize);
	BuildCrosshairTick(TEXT("CrosshairRight"),  FVector2D( DistFromCenter, 0.0f), HorizontalTickSize);

	if (bShowCenterDot)
	{
		UImage* Dot = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("CrosshairCenterDot"));
		Dot->SetColorAndOpacity(CrosshairColor);
		const FVector2D DotSize(CenterDotSize, CenterDotSize);
		Dot->SetDesiredSizeOverride(DotSize);
		UCanvasPanelSlot* DotSlot = RootCanvas->AddChildToCanvas(Dot);
		DotSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		DotSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		DotSlot->SetPosition(FVector2D(0.0f, 0.0f));
		DotSlot->SetSize(DotSize);
		DotSlot->SetAutoSize(false);
	}

	// ---- Death status overlay (center-ish; hidden while alive) ------------
	// Positioned slightly above screen center so it doesn't crash into the
	// crosshair. Title shows state ("RESPAWN IN N" or "ELIMINATED"); subtitle
	// adds context ("SPECTATING TEAM" for permanent ghosts).
	DeathStatusBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("DeathStatusBox"));
	UCanvasPanelSlot* DeathSlot = RootCanvas->AddChildToCanvas(DeathStatusBox);
	DeathSlot->SetAnchors(FAnchors(0.5f, 0.5f));
	DeathSlot->SetAlignment(FVector2D(0.5f, 0.5f));
	DeathSlot->SetPosition(FVector2D(0.0f, -120.0f));
	DeathSlot->SetAutoSize(true);
	DeathStatusBox->SetVisibility(ESlateVisibility::Collapsed);

	DeathStatusTitle = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("DeathStatusTitle"));
	DeathStatusTitle->SetFont(MakeFont(44));
	DeathStatusTitle->SetText(FText::FromString(TEXT("DEAD")));
	DeathStatusTitle->SetColorAndOpacity(RespawnTextColor);
	DeathStatusTitle->SetJustification(ETextJustify::Center);
	ApplyTextShadow(DeathStatusTitle);
	UVerticalBoxSlot* TitleSlot = DeathStatusBox->AddChildToVerticalBox(DeathStatusTitle);
	TitleSlot->SetHorizontalAlignment(HAlign_Center);

	DeathStatusSubtitle = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("DeathStatusSubtitle"));
	DeathStatusSubtitle->SetFont(MakeFont(20));
	DeathStatusSubtitle->SetText(FText::FromString(TEXT("")));
	DeathStatusSubtitle->SetColorAndOpacity(LabelTextColor);
	DeathStatusSubtitle->SetJustification(ETextJustify::Center);
	ApplyTextShadow(DeathStatusSubtitle);
	UVerticalBoxSlot* SubtitleSlot = DeathStatusBox->AddChildToVerticalBox(DeathStatusSubtitle);
	SubtitleSlot->SetHorizontalAlignment(HAlign_Center);
	SubtitleSlot->SetPadding(FMargin(0.0f, 6.0f, 0.0f, 0.0f));

	// ---- Bomb status: plant/defuse progress + planted banner + countdown ----
	// Positioned just above the crosshair, below the top-center scores.
	BombStatusBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("BombStatusBox"));
	UCanvasPanelSlot* BombSlot = RootCanvas->AddChildToCanvas(BombStatusBox);
	BombSlot->SetAnchors(FAnchors(0.5f, 0.5f));
	BombSlot->SetAlignment(FVector2D(0.5f, 0.5f));
	BombSlot->SetPosition(FVector2D(0.0f, -200.0f));
	BombSlot->SetAutoSize(true);
	BombStatusBox->SetVisibility(ESlateVisibility::Collapsed);

	BombStatusTitle = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("BombStatusTitle"));
	BombStatusTitle->SetFont(MakeFont(28));
	BombStatusTitle->SetText(FText::FromString(TEXT("")));
	BombStatusTitle->SetColorAndOpacity(EliminatedTextColor);
	BombStatusTitle->SetJustification(ETextJustify::Center);
	ApplyTextShadow(BombStatusTitle);
	UVerticalBoxSlot* BombTitleSlot = BombStatusBox->AddChildToVerticalBox(BombStatusTitle);
	BombTitleSlot->SetHorizontalAlignment(HAlign_Center);

	BombStatusSubtitle = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("BombStatusSubtitle"));
	BombStatusSubtitle->SetFont(MakeFont(16));
	BombStatusSubtitle->SetText(FText::FromString(TEXT("")));
	BombStatusSubtitle->SetColorAndOpacity(LabelTextColor);
	BombStatusSubtitle->SetJustification(ETextJustify::Center);
	ApplyTextShadow(BombStatusSubtitle);
	UVerticalBoxSlot* BombSubSlot = BombStatusBox->AddChildToVerticalBox(BombStatusSubtitle);
	BombSubSlot->SetHorizontalAlignment(HAlign_Center);
	BombSubSlot->SetPadding(FMargin(0.0f, 4.0f, 0.0f, 0.0f));

	// ---- Kill feed (top-right) ---------------------------------------------
	// Rows are added dynamically at runtime; this box is just the container.
	KillFeedBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("KillFeedBox"));
	UCanvasPanelSlot* KillFeedSlot = RootCanvas->AddChildToCanvas(KillFeedBox);
	KillFeedSlot->SetAnchors(FAnchors(1.0f, 0.0f));
	KillFeedSlot->SetAlignment(FVector2D(1.0f, 0.0f));
	KillFeedSlot->SetPosition(FVector2D(-CornerPadding, TopBarTopMargin + 80.0f));
	KillFeedSlot->SetAutoSize(true);

	// ---- Personal kill / death notification (bottom-center) ----------------
	KillNotifText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("KillNotifText"));
	KillNotifText->SetFont(MakeFont(20));
	KillNotifText->SetJustification(ETextJustify::Center);
	KillNotifText->SetColorAndOpacity(KillNotifColor);
	ApplyTextShadow(KillNotifText);
	UCanvasPanelSlot* NotifSlot = RootCanvas->AddChildToCanvas(KillNotifText);
	NotifSlot->SetAnchors(FAnchors(0.5f, 1.0f));
	NotifSlot->SetAlignment(FVector2D(0.5f, 1.0f));
	NotifSlot->SetPosition(FVector2D(0.0f, -(CornerPadding + 90.0f)));
	NotifSlot->SetAutoSize(true);
	KillNotifText->SetVisibility(ESlateVisibility::Hidden);

	// ---- Bottom-center: equipped-item indicator ----------------------------
	// Three vertical columns side-by-side. Each column has a small cooldown
	// text on top (only visible while that grenade is on CD) and a larger
	// label below ("[key] NAME"). Equipped slot glows in its brand color.
	{
		UHorizontalBox* EquipRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("EquipRow"));
		UCanvasPanelSlot* EquipRowSlot = RootCanvas->AddChildToCanvas(EquipRow);
		EquipRowSlot->SetAnchors(FAnchors(0.5f, 1.0f));
		EquipRowSlot->SetAlignment(FVector2D(0.5f, 1.0f));
		EquipRowSlot->SetPosition(FVector2D(0.0f, -CornerPadding));
		EquipRowSlot->SetAutoSize(true);

		auto AddEquipColumn = [&](const TCHAR* CDName, const TCHAR* LabelName,
		                          TObjectPtr<UTextBlock>& OutCDText,
		                          TObjectPtr<UTextBlock>& OutLabelText)
		{
			UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
			UHorizontalBoxSlot* HSlot = EquipRow->AddChildToHorizontalBox(Col);
			HSlot->SetPadding(FMargin(28.0f, 0.0f));
			HSlot->SetVerticalAlignment(VAlign_Bottom);

			OutCDText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), CDName);
			OutCDText->SetFont(MakeFont(16));
			OutCDText->SetText(FText::GetEmpty());
			OutCDText->SetColorAndOpacity(PrimaryTextColor);
			OutCDText->SetJustification(ETextJustify::Center);
			ApplyTextShadow(OutCDText);
			OutCDText->SetVisibility(ESlateVisibility::Hidden);
			UVerticalBoxSlot* CDSlot = Col->AddChildToVerticalBox(OutCDText);
			CDSlot->SetHorizontalAlignment(HAlign_Center);

			OutLabelText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), LabelName);
			OutLabelText->SetFont(MakeFont(22));
			OutLabelText->SetText(FText::FromString(TEXT("---")));
			OutLabelText->SetColorAndOpacity(LabelTextColor);
			OutLabelText->SetJustification(ETextJustify::Center);
			ApplyTextShadow(OutLabelText);
			UVerticalBoxSlot* LabelSlot = Col->AddChildToVerticalBox(OutLabelText);
			LabelSlot->SetHorizontalAlignment(HAlign_Center);
		};

		AddEquipColumn(TEXT("CooldownWeaponText"), TEXT("EquipWeaponText"),
			CooldownWeaponText, EquipWeaponText);
		AddEquipColumn(TEXT("CooldownSlot1Text"),  TEXT("EquipSlot1Text"),
			CooldownSlot1Text, EquipSlot1Text);
		AddEquipColumn(TEXT("CooldownSlot2Text"),  TEXT("EquipSlot2Text"),
			CooldownSlot2Text, EquipSlot2Text);
	}

	// ---- PreRound buy panel (centered overlay) ----------------------------
	// Built before the FlashOverlay so the overlay still sits above it in Z.
	BuildBuyPanel(RootCanvas);

	// ---- Flash overlay (full-screen white) ---------------------------------
	// Added LAST so it sits on top of every other HUD element in Z-order.
	// HitTestInvisible so clicks/input pass through. Anchored to all four edges
	// with zero offsets fills the viewport at any resolution.
	FlashOverlay = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("FlashOverlay"));
	FlashOverlay->SetColorAndOpacity(FLinearColor(1.0f, 1.0f, 1.0f, 0.0f));
	UCanvasPanelSlot* FlashSlot = RootCanvas->AddChildToCanvas(FlashOverlay);
	FlashSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
	FlashSlot->SetOffsets(FMargin(0.0f, 0.0f, 0.0f, 0.0f));
	FlashOverlay->SetVisibility(ESlateVisibility::Collapsed);
}

// =====================================================================
// Kill feed
// =====================================================================

void UProjectCompHUDWidget::UpdateKillFeed(float DeltaTime)
{
	// Consume any new events queued by Multicast_OnPlayerKilled.
	if (AProjectCompGameState* GS = GetTypedGameState())
	{
		for (const FKillFeedData& Event : GS->PendingKillFeedEvents)
		{
			AddKillFeedEntry(Event.KillerName, Event.VictimName, Event.KillerTeam, Event.VictimTeam);
		}
		GS->PendingKillFeedEvents.Empty();
	}

	// Tick timers, fade rows, remove expired ones (iterate backward for safe removal).
	static constexpr float FeedDuration  = 5.0f;
	static constexpr float FeedFadeStart = 1.5f;

	for (int32 i = KillFeedTimers.Num() - 1; i >= 0; --i)
	{
		KillFeedTimers[i] -= DeltaTime;
		if (KillFeedTimers[i] <= 0.0f)
		{
			if (KillFeedRows.IsValidIndex(i) && KillFeedRows[i])
			{
				KillFeedRows[i]->RemoveFromParent();
			}
			KillFeedRows.RemoveAt(i);
			KillFeedTimers.RemoveAt(i);
		}
		else if (KillFeedRows.IsValidIndex(i) && KillFeedRows[i])
		{
			const float Alpha = KillFeedTimers[i] < FeedFadeStart
				? KillFeedTimers[i] / FeedFadeStart
				: 1.0f;
			KillFeedRows[i]->SetRenderOpacity(Alpha);
		}
	}

	// Fade the personal notification.
	if (KillNotifTimeRemaining > 0.0f && KillNotifText)
	{
		KillNotifTimeRemaining -= DeltaTime;
		if (KillNotifTimeRemaining <= 0.0f)
		{
			KillNotifText->SetVisibility(ESlateVisibility::Hidden);
		}
		else
		{
			static constexpr float NotifFadeStart = 0.8f;
			const float Alpha = KillNotifTimeRemaining < NotifFadeStart
				? KillNotifTimeRemaining / NotifFadeStart
				: 1.0f;
			KillNotifText->SetRenderOpacity(Alpha);
		}
	}
}

void UProjectCompHUDWidget::AddKillFeedEntry(const FString& KillerName, const FString& VictimName, ETeamID KillerTeam, ETeamID VictimTeam)
{
	if (!KillFeedBox)
	{
		return;
	}

	// Is the local player the killer or the victim?
	const AProjectCompPlayerState* LocalPS = GetLocalPlayerState();
	const FString LocalName = LocalPS ? LocalPS->GetPlayerName() : TEXT("");
	const bool bLocalKilled = !LocalName.IsEmpty() && KillerName == LocalName;
	const bool bLocalDied   = !LocalName.IsEmpty() && VictimName == LocalName;

	// Cap at 6 rows — evict the oldest (top of the list).
	static constexpr int32 MaxEntries = 6;
	while (KillFeedTimers.Num() >= MaxEntries)
	{
		if (KillFeedRows.Num() > 0 && KillFeedRows[0])
		{
			KillFeedRows[0]->RemoveFromParent();
			KillFeedRows.RemoveAt(0);
		}
		if (KillFeedTimers.Num() > 0)
		{
			KillFeedTimers.RemoveAt(0);
		}
	}

	// Build the row: [KillerName]  >  [VictimName]
	UHorizontalBox* Row = NewObject<UHorizontalBox>(this);

	auto AppendText = [&](const FString& Str, FLinearColor Color)
	{
		UTextBlock* Txt = NewObject<UTextBlock>(this);
		Txt->SetText(FText::FromString(Str));
		Txt->SetFont(MakeFont(16));
		Txt->SetColorAndOpacity(Color);
		ApplyTextShadow(Txt);
		UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(Txt);
		S->SetVerticalAlignment(VAlign_Center);
	};

	AppendText(KillerName,  GetTeamColor(KillerTeam));
	AppendText(TEXT("  >  "), LabelTextColor);
	AppendText(VictimName,  GetTeamColor(VictimTeam));

	// Rows that involve the local player start fully opaque; others at 80%
	// so self-relevant entries pop slightly from the rest of the feed.
	Row->SetRenderOpacity((bLocalKilled || bLocalDied) ? 1.0f : 0.80f);

	UVerticalBoxSlot* RowSlot = KillFeedBox->AddChildToVerticalBox(Row);
	RowSlot->SetPadding(FMargin(0.0f, 2.0f));

	KillFeedRows.Add(Row);
	KillFeedTimers.Add(5.0f);

	// Personal notification — only for the local player.
	if (KillNotifText)
	{
		if (bLocalKilled)
		{
			KillNotifText->SetText(FText::FromString(FString::Printf(TEXT("YOU KILLED  %s"), *VictimName)));
			KillNotifText->SetColorAndOpacity(KillNotifColor);
			KillNotifText->SetVisibility(ESlateVisibility::HitTestInvisible);
			KillNotifText->SetRenderOpacity(1.0f);
			KillNotifTimeRemaining = 2.5f;
		}
		else if (bLocalDied)
		{
			KillNotifText->SetText(FText::FromString(FString::Printf(TEXT("KILLED BY  %s"), *KillerName)));
			KillNotifText->SetColorAndOpacity(EliminatedTextColor);
			KillNotifText->SetVisibility(ESlateVisibility::HitTestInvisible);
			KillNotifText->SetRenderOpacity(1.0f);
			KillNotifTimeRemaining = 2.5f;
		}
	}
}

// =====================================================================
// Flash overlay
// =====================================================================

float UProjectCompHUDWidget::RefreshFlashOverlay()
{
	if (!FlashOverlay)
	{
		return 0.0f;
	}

	const AProjectCompPlayerState* PS = GetLocalPlayerState();
	const UWorld* World = GetWorld();
	if (!PS || !World)
	{
		FlashOverlay->SetVisibility(ESlateVisibility::Collapsed);
		return 0.0f;
	}

	// Compare against the GameState's replicated server clock so this stays in
	// sync with the timestamps stamped by Server_ApplyFlash. Using local-world
	// time on a remote client would drift visibly (flash too short or too long).
	const AGameStateBase* GS = World->GetGameState();
	const float Now      = GS ? GS->GetServerWorldTimeSeconds() : World->GetTimeSeconds();
	const float EndTime  = PS->GetFlashEndTime();
	const float FadeBeg  = PS->GetFlashFadeStartTime();

	// Not flashed (or flash already ended) — collapse so SDecoratable doesn't
	// pay for an invisible widget every frame.
	if (EndTime <= Now)
	{
		FlashOverlay->SetVisibility(ESlateVisibility::Collapsed);
		return 0.0f;
	}

	// Full-white phase (Now < FadeBeg): alpha = 1. Fade phase (Now >= FadeBeg):
	// linear fade from 1 → 0 across the remaining fade window.
	float Alpha = 1.0f;
	if (Now >= FadeBeg)
	{
		const float FadeSpan = EndTime - FadeBeg;
		Alpha = FadeSpan > KINDA_SMALL_NUMBER
		      ? FMath::Clamp((EndTime - Now) / FadeSpan, 0.0f, 1.0f)
		      : 0.0f;
	}

	FlashOverlay->SetColorAndOpacity(FLinearColor(1.0f, 1.0f, 1.0f, Alpha));
	FlashOverlay->SetVisibility(ESlateVisibility::HitTestInvisible);
	return Alpha;
}

// =====================================================================
// Per-frame refresh
// =====================================================================

void UProjectCompHUDWidget::RefreshDisplay()
{
	const AProjectCompGameState* GS = GetTypedGameState();
	const AProjectCompPlayerState* PS = GetLocalPlayerState();
	const UProjectCompHealthComponent* HC = GetLocalHealth();

	// Round info
	if (GS)
	{
		if (PhaseLabelText)
		{
			PhaseLabelText->SetText(FormatPhaseText(GS->GetCurrentPhase()));
		}
		if (PhaseTimerText)
		{
			PhaseTimerText->SetText(FormatTimeText(GS->GetPhaseTimeRemaining()));
		}
		if (AttackerScoreText)
		{
			AttackerScoreText->SetText(FText::AsNumber(GS->GetTeamLoadout(ETeamID::Attackers).Score));
		}
		if (DefenderScoreText)
		{
			DefenderScoreText->SetText(FText::AsNumber(GS->GetTeamLoadout(ETeamID::Defenders).Score));
		}
	}

	// Health
	if (HC)
	{
		const float CurrentHP = HC->GetCurrentHealth();
		if (HealthValueText)
		{
			HealthValueText->SetText(FText::AsNumber(FMath::CeilToInt(CurrentHP)));
			HealthValueText->SetColorAndOpacity(GetHealthColorForFraction(HC->GetHealthFraction()));
		}
	}

	// Ammo readout — read from the local pawn's equipped weapon.
	// Hidden when no weapon (spectator / between possessions).
	if (AmmoBox)
	{
		const AProjectCompCharacter* LocalChar = GetLocalCharacter();
		const AProjectCompHitscanWeapon* Weapon = LocalChar ? LocalChar->GetCurrentWeapon() : nullptr;
		if (!Weapon)
		{
			AmmoBox->SetVisibility(ESlateVisibility::Collapsed);
		}
		else
		{
			AmmoBox->SetVisibility(ESlateVisibility::HitTestInvisible);
			if (AmmoValueText)
			{
				if (Weapon->IsReloading())
				{
					// Show a countdown using the replicated server-stamped end time
					// against server-world-time so remote clients don't drift.
					const UWorld* W = GetWorld();
					const AGameStateBase* GSBase = W ? W->GetGameState() : nullptr;
					const float ServerTime = GSBase ? GSBase->GetServerWorldTimeSeconds()
					                                : (W ? W->GetTimeSeconds() : 0.0f);
					const float Remaining = FMath::Max(0.0f, Weapon->GetReloadEndTime() - ServerTime);
					AmmoValueText->SetText(FText::FromString(FString::Printf(TEXT("RELOADING %.1fs"), Remaining)));
					AmmoValueText->SetColorAndOpacity(LabelTextColor);
				}
				else
				{
					// Format: "<currentMag> / <reserveBullets>" — total ammo pool
					// in the reserve, not clip count. Player sees the full economy.
					AmmoValueText->SetText(FText::FromString(FString::Printf(TEXT("%d / %d"),
						Weapon->GetCurrentMagAmmo(), Weapon->GetReserveAmmo())));
					AmmoValueText->SetColorAndOpacity(PrimaryTextColor);
				}
			}
		}
	}

	// Ability indicator: tinted by team color when ready, gray when cooling.
	if (PS && GS && AbilityIndicatorImage)
	{
		const ETeamID Team = PS->GetTeamID();
		if (Team == ETeamID::Spectator)
		{
			AbilityIndicatorImage->SetColorAndOpacity(AbilityCooldownColor);
		}
		else
		{
			const bool bReady = GS->IsTeamAbilityReady(Team);
			AbilityIndicatorImage->SetColorAndOpacity(bReady ? AbilityReadyColor : AbilityCooldownColor);
		}
	}

	// Death status overlay: shown only while dead.
	if (DeathStatusBox && PS)
	{
		if (PS->IsAlive())
		{
			DeathStatusBox->SetVisibility(ESlateVisibility::Collapsed);
		}
		else if (PS->IsPermanentlyDead())
		{
			DeathStatusBox->SetVisibility(ESlateVisibility::HitTestInvisible);
			if (DeathStatusTitle)
			{
				DeathStatusTitle->SetText(NSLOCTEXT("HUD", "Eliminated", "ELIMINATED"));
				DeathStatusTitle->SetColorAndOpacity(EliminatedTextColor);
			}
			if (DeathStatusSubtitle)
			{
				DeathStatusSubtitle->SetText(NSLOCTEXT("HUD", "Spectating", "SPECTATING — LOOMING"));
			}
		}
		else
		{
			// Temporary ghost: respawn countdown.
			DeathStatusBox->SetVisibility(ESlateVisibility::HitTestInvisible);

			// Compare against the replicated server clock — the timestamp was
			// stamped on the server using server world time, so a remote client
			// reading local-world time would drift. Reuses the outer GS for
			// the server clock; falls back to local world time only if GS is null.
			const UWorld* World = GetWorld();
			const float ServerTime = GS ? GS->GetServerWorldTimeSeconds() : (World ? World->GetTimeSeconds() : 0.0f);
			const float Remaining = FMath::Max(0.0f, PS->GetRespawnEndTime() - ServerTime);
			const int32 RemainingInt = FMath::CeilToInt(Remaining);

			if (DeathStatusTitle)
			{
				DeathStatusTitle->SetText(FText::FromString(FString::Printf(TEXT("RESPAWN IN %d"), RemainingInt)));
				DeathStatusTitle->SetColorAndOpacity(RespawnTextColor);
			}
			if (DeathStatusSubtitle)
			{
				DeathStatusSubtitle->SetText(NSLOCTEXT("HUD", "LoomActive", "LOOM ACTIVE"));
			}
		}
	}

	RefreshBombStatus();
	RefreshEquipIndicator();
	RefreshBuyPanel();
}

// =====================================================================
// Equipped-item indicator (bottom-center)
// =====================================================================

FLinearColor UProjectCompHUDWidget::GetGrenadeBrandColor(EGrenadeType Type)
{
	// Design memory project_grenade_system: FLASH=YELLOW, INFO=CYAN,
	// VULN=NEON CORAL, SOUND=GREY. The SOUND grey is intentionally bright
	// (~85% lightness) so the equipped-glow reads as highlighted rather than
	// blending with the "not equipped" dim grey at ~55% lightness.
	switch (Type)
	{
	case EGrenadeType::Flash:      return FLinearColor(1.00f, 0.85f, 0.00f);
	case EGrenadeType::Info:       return FLinearColor(0.00f, 0.85f, 1.00f);
	case EGrenadeType::Vulnerable: return FLinearColor(1.00f, 0.376f, 0.251f);
	case EGrenadeType::Smoke:      return FLinearColor(0.85f, 0.85f, 0.90f);
	default:                       return FLinearColor::White;
	}
}

void UProjectCompHUDWidget::RefreshEquipIndicator()
{
	if (!EquipWeaponText || !EquipSlot1Text || !EquipSlot2Text)
	{
		return;
	}

	const AProjectCompCharacter* Char = GetLocalCharacter();
	const AProjectCompPlayerState* PS = GetLocalPlayerState();
	if (!Char || !PS)
	{
		EquipWeaponText->SetVisibility(ESlateVisibility::Hidden);
		EquipSlot1Text->SetVisibility(ESlateVisibility::Hidden);
		EquipSlot2Text->SetVisibility(ESlateVisibility::Hidden);
		if (CooldownSlot1Text) CooldownSlot1Text->SetVisibility(ESlateVisibility::Hidden);
		if (CooldownSlot2Text) CooldownSlot2Text->SetVisibility(ESlateVisibility::Hidden);
		return;
	}
	EquipWeaponText->SetVisibility(ESlateVisibility::HitTestInvisible);
	EquipSlot1Text->SetVisibility(ESlateVisibility::HitTestInvisible);
	EquipSlot2Text->SetVisibility(ESlateVisibility::HitTestInvisible);
	if (CooldownWeaponText) CooldownWeaponText->SetVisibility(ESlateVisibility::Hidden); // weapon never has a CD

	const int32 EquippedSlot = Char->GetEquippedGrenadeSlot();
	const FLinearColor TeamColor = GetTeamColor(PS->GetTeamID());
	const TArray<EGrenadeType>& Kit = PS->GetEquippedGrenadeTypes();

	// Pseudo-glow: TextBlock shadow at offset (0,0) tinted with the slot's
	// glow color reads as a halo around the glyphs. The text itself is also
	// glow-tinted so the equipped item pops. Non-equipped items revert to a
	// dim grey with a normal small drop shadow.
	auto StyleEquipText = [](UTextBlock* Text, bool bEquipped, const FLinearColor& GlowColor)
	{
		if (bEquipped)
		{
			Text->SetColorAndOpacity(FSlateColor(GlowColor));
			Text->SetShadowOffset(FVector2D(0.0f, 0.0f));
			Text->SetShadowColorAndOpacity(FLinearColor(GlowColor.R, GlowColor.G, GlowColor.B, 0.9f));
		}
		else
		{
			Text->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.55f, 0.85f)));
			Text->SetShadowOffset(FVector2D(1.0f, 1.0f));
			Text->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.55f));
		}
	};

	auto GrenadeShortName = [](EGrenadeType Type) -> const TCHAR*
	{
		switch (Type)
		{
		case EGrenadeType::Flash:      return TEXT("FLASH");
		case EGrenadeType::Info:       return TEXT("INFO");
		case EGrenadeType::Vulnerable: return TEXT("VULN");
		case EGrenadeType::Smoke:      return TEXT("SMOKE");
		default:                       return TEXT("---");
		}
	};

	// ---- Weapon column ---------------------------------------------------
	EquipWeaponText->SetText(FText::FromString(TEXT("[1] WEAPON")));
	StyleEquipText(EquipWeaponText, EquippedSlot == -1, TeamColor);

	// ---- Grenade slots: brand-color glow + cooldown countdown ------------
	const AProjectCompGameState* GS = GetTypedGameState();
	const float ServerNow = GS ? GS->GetServerWorldTimeSeconds() : 0.0f;

	auto RefreshGrenadeColumn = [&](UTextBlock* LabelText, UTextBlock* CDText,
	                                int32 KitIndex, const TCHAR* KeyLabel)
	{
		const bool bHasGrenade = Kit.IsValidIndex(KitIndex) && Kit[KitIndex] != EGrenadeType::None;
		const EGrenadeType Type = bHasGrenade ? Kit[KitIndex] : EGrenadeType::None;

		const FString Label = bHasGrenade
			? FString::Printf(TEXT("[%s] %s"), KeyLabel, GrenadeShortName(Type))
			: FString::Printf(TEXT("[%s] ---"), KeyLabel);
		LabelText->SetText(FText::FromString(Label));

		const FLinearColor GlowColor = bHasGrenade
			? GetGrenadeBrandColor(Type)
			: FLinearColor::White;
		StyleEquipText(LabelText, EquippedSlot == KitIndex, GlowColor);

		// Cooldown text above the label — hidden when no grenade or ready.
		if (CDText)
		{
			if (!bHasGrenade)
			{
				CDText->SetVisibility(ESlateVisibility::Hidden);
				return;
			}
			const float EndTime = PS->GetGrenadeCooldownEndTime(Type);
			const float Remaining = FMath::Max(0.0f, EndTime - ServerNow);
			if (Remaining > 0.05f)
			{
				CDText->SetText(FText::FromString(FString::Printf(TEXT("%.1fs"), Remaining)));
				// Tint the cooldown text in the grenade's brand color too —
				// reinforces which grenade is on CD at a glance.
				CDText->SetColorAndOpacity(FSlateColor(GlowColor));
				CDText->SetVisibility(ESlateVisibility::HitTestInvisible);
			}
			else
			{
				CDText->SetVisibility(ESlateVisibility::Hidden);
			}
		}
	};

	RefreshGrenadeColumn(EquipSlot1Text, CooldownSlot1Text, /*KitIndex*/ 0, TEXT("E"));
	RefreshGrenadeColumn(EquipSlot2Text, CooldownSlot2Text, /*KitIndex*/ 1, TEXT("Q"));
}

// =====================================================================
// Bomb status (plant/defuse progress, planted banner, detonation countdown)
// =====================================================================

void UProjectCompHUDWidget::RefreshBombStatus()
{
	if (!BombStatusBox)
	{
		return;
	}

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;

	// Check for a freshly-defused site and start the 3s banner timer.
	bool bAnyDefused = false;
	for (const TWeakObjectPtr<AProjectCompBombSite>& WeakSite : CachedBombSites)
	{
		AProjectCompBombSite* Site = WeakSite.Get();
		if (Site && Site->bIsDefused)
		{
			bAnyDefused = true;
			if (DefuseMessageEndTime == 0.0f)
			{
				DefuseMessageEndTime = Now + 3.0f;
			}
			break;
		}
	}
	if (!bAnyDefused)
	{
		DefuseMessageEndTime = 0.0f;
	}

	if (DefuseMessageEndTime > 0.0f && Now < DefuseMessageEndTime)
	{
		BombStatusBox->SetVisibility(ESlateVisibility::HitTestInvisible);
		if (BombStatusTitle)
		{
			BombStatusTitle->SetText(NSLOCTEXT("HUD", "BombDefused", "BOMB DEFUSED"));
			BombStatusTitle->SetColorAndOpacity(DefendersColor);
		}
		if (BombStatusSubtitle)
		{
			BombStatusSubtitle->SetText(FText::GetEmpty());
		}
		return;
	}

	// Find the most-interesting bomb site to display. Priority: planted >
	// active plant > active defuse > nothing. We only show one at a time.
	AProjectCompBombSite* DisplaySite = nullptr;
	bool bShowDefuse = false;
	for (const TWeakObjectPtr<AProjectCompBombSite>& WeakSite : CachedBombSites)
	{
		AProjectCompBombSite* Site = WeakSite.Get();
		if (!Site || Site->IsTerminal())
		{
			continue;
		}
		if (Site->bIsPlanted)
		{
			DisplaySite = Site;
			bShowDefuse = Site->DefuseProgress > 0.01f;
			break;  // planted always wins
		}
		if (Site->PlantProgress > 0.01f && !DisplaySite)
		{
			DisplaySite = Site;
		}
	}

	if (!DisplaySite)
	{
		BombStatusBox->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}

	BombStatusBox->SetVisibility(ESlateVisibility::HitTestInvisible);

	if (DisplaySite->bIsPlanted)
	{
		// Use the replicated server clock — DetonationEndTime is stamped on the
		// server, so reading local-world time on a remote client would drift.
		const UWorld* World = GetWorld();
		const AGameStateBase* GS = World ? World->GetGameState() : nullptr;
		const float ServerTime = GS ? GS->GetServerWorldTimeSeconds() : (World ? World->GetTimeSeconds() : 0.0f);
		const float Remaining = FMath::Max(0.0f, DisplaySite->DetonationEndTime - ServerTime);
		const int32 RemainingInt = FMath::CeilToInt(Remaining);

		if (BombStatusTitle)
		{
			BombStatusTitle->SetText(FText::FromString(FString::Printf(TEXT("BOMB PLANTED AT %s"), *DisplaySite->Label.ToString())));
			BombStatusTitle->SetColorAndOpacity(EliminatedTextColor);
		}
		if (BombStatusSubtitle)
		{
			if (bShowDefuse)
			{
				const int32 Percent = FMath::FloorToInt(DisplaySite->DefuseProgress * 100.0f);
				BombStatusSubtitle->SetText(FText::FromString(FString::Printf(TEXT("DEFUSING %d%%  —  DETONATES IN %ds"), Percent, RemainingInt)));
			}
			else
			{
				BombStatusSubtitle->SetText(FText::FromString(FString::Printf(TEXT("DETONATES IN %ds"), RemainingInt)));
			}
		}
	}
	else
	{
		// Plant in progress.
		const int32 Percent = FMath::FloorToInt(DisplaySite->PlantProgress * 100.0f);
		if (BombStatusTitle)
		{
			BombStatusTitle->SetText(FText::FromString(FString::Printf(TEXT("PLANTING AT %s"), *DisplaySite->Label.ToString())));
			BombStatusTitle->SetColorAndOpacity(AttackersColor);
		}
		if (BombStatusSubtitle)
		{
			BombStatusSubtitle->SetText(FText::FromString(FString::Printf(TEXT("%d%%"), Percent)));
		}
	}
}

// =====================================================================
// Formatting helpers
// =====================================================================

FLinearColor UProjectCompHUDWidget::GetHealthColorForFraction(float Fraction) const
{
	if (Fraction < 0.25f) return HealthCriticalColor;
	if (Fraction < 0.50f) return HealthWarningColor;
	return PrimaryTextColor;
}

FLinearColor UProjectCompHUDWidget::GetTeamColor(ETeamID Team) const
{
	switch (Team)
	{
		case ETeamID::Attackers: return AttackersColor;
		case ETeamID::Defenders: return DefendersColor;
		default:                 return LabelTextColor;
	}
}

FText UProjectCompHUDWidget::FormatPhaseText(ERoundPhase Phase) const
{
	switch (Phase)
	{
		case ERoundPhase::LoomSelection: return NSLOCTEXT("HUD", "LoomSelection", "LOOM SELECTION");
		case ERoundPhase::PreRound:      return NSLOCTEXT("HUD", "PreRound",      "PRE-ROUND");
		case ERoundPhase::RoundActive:   return NSLOCTEXT("HUD", "RoundActive",   "LIVE");
		case ERoundPhase::PostRound:     return NSLOCTEXT("HUD", "PostRound",     "POST-ROUND");
		case ERoundPhase::MatchEnded:    return NSLOCTEXT("HUD", "MatchEnded",    "MATCH OVER");
		default:                         return FText::FromString(TEXT("—"));
	}
}

FText UProjectCompHUDWidget::FormatTimeText(float Seconds) const
{
	const int32 TotalSeconds = FMath::CeilToInt(FMath::Max(Seconds, 0.0f));
	const int32 Minutes = TotalSeconds / 60;
	const int32 Secs = TotalSeconds % 60;
	return FText::FromString(FString::Printf(TEXT("%d:%02d"), Minutes, Secs));
}

// =====================================================================
// Null-safe state accessors
// =====================================================================

AProjectCompCharacter* UProjectCompHUDWidget::GetLocalCharacter() const
{
	const APlayerController* PC = GetOwningPlayer();
	return PC ? Cast<AProjectCompCharacter>(PC->GetPawn()) : nullptr;
}

UProjectCompHealthComponent* UProjectCompHUDWidget::GetLocalHealth() const
{
	const AProjectCompCharacter* Char = GetLocalCharacter();
	return Char ? Char->GetHealthComponent() : nullptr;
}

AProjectCompPlayerState* UProjectCompHUDWidget::GetLocalPlayerState() const
{
	const APlayerController* PC = GetOwningPlayer();
	return PC ? PC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
}

AProjectCompGameState* UProjectCompHUDWidget::GetTypedGameState() const
{
	return GetWorld() ? GetWorld()->GetGameState<AProjectCompGameState>() : nullptr;
}

// =====================================================================
// PreRound buy panel — widget tree construction
// =====================================================================

void UProjectCompHUDWidget::BuildBuyPanel(UCanvasPanel* RootCanvas)
{
	if (!RootCanvas || !WidgetTree)
	{
		return;
	}

	// Root VBox anchored to screen center, autosize. Hidden until needed.
	BuyPanelRoot = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("BuyPanelRoot"));
	UCanvasPanelSlot* RootSlot = RootCanvas->AddChildToCanvas(BuyPanelRoot);
	RootSlot->SetAnchors(FAnchors(0.5f, 0.5f));
	RootSlot->SetAlignment(FVector2D(0.5f, 0.5f));
	RootSlot->SetPosition(FVector2D(0.0f, 0.0f));
	RootSlot->SetAutoSize(true);
	BuyPanelRoot->SetVisibility(ESlateVisibility::Collapsed);

	// ---- Header row: credits | phase label | timer -----------------------
	UHorizontalBox* HeaderRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("BuyHeaderRow"));
	UVerticalBoxSlot* HeaderSlot = BuyPanelRoot->AddChildToVerticalBox(HeaderRow);
	HeaderSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 12.0f));
	HeaderSlot->SetHorizontalAlignment(HAlign_Center);

	BuyHeaderCredits = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("BuyHeaderCredits"));
	BuyHeaderCredits->SetFont(MakeFont(24));
	BuyHeaderCredits->SetColorAndOpacity(PrimaryTextColor);
	BuyHeaderCredits->SetText(FText::FromString(TEXT("$0")));
	ApplyTextShadow(BuyHeaderCredits);
	UHorizontalBoxSlot* CreditsSlot = HeaderRow->AddChildToHorizontalBox(BuyHeaderCredits);
	CreditsSlot->SetPadding(FMargin(20.0f, 0.0f));
	CreditsSlot->SetVerticalAlignment(VAlign_Center);

	BuyHeaderPhase = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("BuyHeaderPhase"));
	BuyHeaderPhase->SetFont(MakeFont(24));
	BuyHeaderPhase->SetColorAndOpacity(LabelTextColor);
	BuyHeaderPhase->SetText(FText::FromString(TEXT("—")));
	ApplyTextShadow(BuyHeaderPhase);
	UHorizontalBoxSlot* PhaseSlot = HeaderRow->AddChildToHorizontalBox(BuyHeaderPhase);
	PhaseSlot->SetPadding(FMargin(20.0f, 0.0f));
	PhaseSlot->SetVerticalAlignment(VAlign_Center);

	BuyHeaderTimer = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("BuyHeaderTimer"));
	BuyHeaderTimer->SetFont(MakeFont(24));
	BuyHeaderTimer->SetColorAndOpacity(PrimaryTextColor);
	BuyHeaderTimer->SetText(FText::FromString(TEXT("0:00")));
	ApplyTextShadow(BuyHeaderTimer);
	UHorizontalBoxSlot* TimerSlot = HeaderRow->AddChildToHorizontalBox(BuyHeaderTimer);
	TimerSlot->SetPadding(FMargin(20.0f, 0.0f));
	TimerSlot->SetVerticalAlignment(VAlign_Center);

	// ---- 3-column row: Weapons | Grenades | Loom -------------------------
	UHorizontalBox* ColumnsRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("BuyColumnsRow"));
	UVerticalBoxSlot* ColumnsSlot = BuyPanelRoot->AddChildToVerticalBox(ColumnsRow);
	ColumnsSlot->SetHorizontalAlignment(HAlign_Center);

	// Helper to build a column with title.
	auto MakeColumn = [&](const TCHAR* Name, const FString& Title) -> UVerticalBox*
	{
		UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), Name);
		UHorizontalBoxSlot* ColSlot = ColumnsRow->AddChildToHorizontalBox(Col);
		ColSlot->SetPadding(FMargin(24.0f, 0.0f));
		ColSlot->SetVerticalAlignment(VAlign_Top);

		UTextBlock* TitleText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(*(FString(Name) + TEXT("Title"))));
		TitleText->SetFont(MakeFont(20));
		TitleText->SetColorAndOpacity(LabelTextColor);
		TitleText->SetText(FText::FromString(Title));
		TitleText->SetJustification(ETextJustify::Center);
		ApplyTextShadow(TitleText);
		UVerticalBoxSlot* TitleSlot = Col->AddChildToVerticalBox(TitleText);
		TitleSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 10.0f));
		TitleSlot->SetHorizontalAlignment(HAlign_Center);
		return Col;
	};

	BuyWeaponsColumn       = MakeColumn(TEXT("BuyWeaponsColumn"),       TEXT("WEAPONS"));
	BuyGrenadesColumn      = MakeColumn(TEXT("BuyGrenadesColumn"),      TEXT("GRENADES"));
	BuyLoomColumn          = MakeColumn(TEXT("BuyLoomColumn"),          TEXT("LOOM"));
	TeammateLoadoutsColumn = MakeColumn(TEXT("TeammateLoadoutsColumn"), TEXT("TEAMMATES"));

	// Pre-allocate up to 5 teammate rows. Each row is a vertical box holding
	// a loadout label and a horizontal box for dynamically-spawned fulfill
	// buttons (rebuilt per tick from each teammate's request lists).
	TeammateRowContainers.Reserve(5);
	TeammateLoadoutLabels.Reserve(5);
	TeammateRequestsBoxes.Reserve(5);
	for (int32 i = 0; i < 5; ++i)
	{
		UVerticalBox* Container = WidgetTree->ConstructWidget<UVerticalBox>(
			UVerticalBox::StaticClass(),
			FName(*FString::Printf(TEXT("TeammateRow%d"), i)));
		UVerticalBoxSlot* ContainerSlot = TeammateLoadoutsColumn->AddChildToVerticalBox(Container);
		ContainerSlot->SetPadding(FMargin(0.0f, 4.0f));
		ContainerSlot->SetHorizontalAlignment(HAlign_Left);
		Container->SetVisibility(ESlateVisibility::Collapsed);

		UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(
			UTextBlock::StaticClass(),
			FName(*FString::Printf(TEXT("TeammateLabel%d"), i)));
		Label->SetFont(MakeFont(14));
		Label->SetColorAndOpacity(PrimaryTextColor);
		Label->SetText(FText::FromString(TEXT("")));
		ApplyTextShadow(Label);
		Container->AddChildToVerticalBox(Label);

		UHorizontalBox* RequestsRow = WidgetTree->ConstructWidget<UHorizontalBox>(
			UHorizontalBox::StaticClass(),
			FName(*FString::Printf(TEXT("TeammateRequests%d"), i)));
		Container->AddChildToVerticalBox(RequestsRow);

		TeammateRowContainers.Add(Container);
		TeammateLoadoutLabels.Add(Label);
		TeammateRequestsBoxes.Add(RequestsRow);
	}

	// Helper to add a buy button to a column. Returns the constructed button
	// so the caller can bind the OnClicked delegate.
	auto AddBuyButton = [&](UVerticalBox* Col, const TCHAR* WidgetName, const FString& Label) -> UButton*
	{
		UButton* Btn = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), WidgetName);
		UTextBlock* T = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),
			FName(*(FString(WidgetName) + TEXT("_Txt"))));
		T->SetText(FText::FromString(Label));
		T->SetFont(MakeFont(14));
		T->SetColorAndOpacity(PrimaryTextColor);
		T->SetJustification(ETextJustify::Center);
		Btn->AddChild(T);
		UVerticalBoxSlot* BtnSlot = Col->AddChildToVerticalBox(Btn);
		BtnSlot->SetPadding(FMargin(0.0f, 3.0f));
		BtnSlot->SetHorizontalAlignment(HAlign_Fill);
		return Btn;
	};

	// Helper for subsection titles within a column (e.g. "Primary", "Secondary").
	auto AddSubTitle = [&](UVerticalBox* Col, const FString& Text)
	{
		UTextBlock* Sub = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Sub->SetFont(MakeFont(14));
		Sub->SetColorAndOpacity(LabelTextColor);
		Sub->SetText(FText::FromString(Text));
		ApplyTextShadow(Sub);
		UVerticalBoxSlot* S = Col->AddChildToVerticalBox(Sub);
		S->SetPadding(FMargin(0.0f, 8.0f, 0.0f, 2.0f));
		S->SetHorizontalAlignment(HAlign_Left);
	};

	// ---- Weapons column: Primary | Secondary | Armour -------------------
	AddSubTitle(BuyWeaponsColumn, TEXT("Primary"));
	BtnBuySMG   = AddBuyButton(BuyWeaponsColumn, TEXT("BtnBuySMG"),   TEXT("SMG     $1200"));
	BtnBuyRifle = AddBuyButton(BuyWeaponsColumn, TEXT("BtnBuyRifle"), TEXT("RIFLE   $2700"));

	AddSubTitle(BuyWeaponsColumn, TEXT("Secondary"));
	BtnBuyPistol = AddBuyButton(BuyWeaponsColumn, TEXT("BtnBuyPistol"), TEXT("PISTOL  $250"));

	AddSubTitle(BuyWeaponsColumn, TEXT("Armour"));
	BtnBuyArmourLight = AddBuyButton(BuyWeaponsColumn, TEXT("BtnBuyArmourLight"), TEXT("LIGHT   $650"));
	BtnBuyArmourHeavy = AddBuyButton(BuyWeaponsColumn, TEXT("BtnBuyArmourHeavy"), TEXT("HEAVY   $1000"));

	BtnBuyPistol->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickBuyPistol);
	BtnBuySMG->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickBuySMG);
	BtnBuyRifle->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickBuyRifle);
	BtnBuyArmourLight->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickBuyArmourLight);
	BtnBuyArmourHeavy->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickBuyArmourHeavy);

	// ---- Grenades column: 4 grenade types -------------------------------
	BtnBuyFlash = AddBuyButton(BuyGrenadesColumn, TEXT("BtnBuyFlash"), TEXT("FLASH   $300"));
	BtnBuyInfo  = AddBuyButton(BuyGrenadesColumn, TEXT("BtnBuyInfo"),  TEXT("INFO    $300"));
	BtnBuyVuln  = AddBuyButton(BuyGrenadesColumn, TEXT("BtnBuyVuln"),  TEXT("VULN    $300"));
	BtnBuySmoke = AddBuyButton(BuyGrenadesColumn, TEXT("BtnBuySmoke"), TEXT("SMOKE   $300"));

	BtnBuyFlash->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickBuyFlash);
	BtnBuyInfo->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickBuyInfo);
	BtnBuyVuln->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickBuyVuln);
	BtnBuySmoke->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickBuySmoke);

	// ---- Loom column: 9-cell ability matrix laid out as a single horizontal
	// row of buttons (instead of a tall vertical list — looks cleaner with
	// short 2-letter labels). Row sits beneath the column title inside the
	// BuyLoomColumn VBox.
	UHorizontalBox* LoomButtonsRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("LoomButtonsRow"));
	UVerticalBoxSlot* LoomRowSlot = BuyLoomColumn->AddChildToVerticalBox(LoomButtonsRow);
	LoomRowSlot->SetHorizontalAlignment(HAlign_Center);
	LoomRowSlot->SetPadding(FMargin(0.0f, 4.0f));

	auto AddLoomButton = [&](const TCHAR* WidgetName, const FString& Label) -> UButton*
	{
		UButton* Btn = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), WidgetName);
		UTextBlock* T = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),
			FName(*(FString(WidgetName) + TEXT("_Txt"))));
		T->SetText(FText::FromString(Label));
		T->SetFont(MakeFont(14));
		T->SetColorAndOpacity(PrimaryTextColor);
		T->SetJustification(ETextJustify::Center);
		Btn->AddChild(T);
		UHorizontalBoxSlot* BtnSlot = LoomButtonsRow->AddChildToHorizontalBox(Btn);
		BtnSlot->SetPadding(FMargin(3.0f, 0.0f));
		BtnSlot->SetVerticalAlignment(VAlign_Fill);
		return Btn;
	};

	BtnClaimAA = AddLoomButton(TEXT("BtnClaimAA"), TEXT("AA"));
	BtnClaimAC = AddLoomButton(TEXT("BtnClaimAC"), TEXT("AC"));
	BtnClaimAM = AddLoomButton(TEXT("BtnClaimAM"), TEXT("AM"));
	BtnClaimCC = AddLoomButton(TEXT("BtnClaimCC"), TEXT("CC"));
	BtnClaimCA = AddLoomButton(TEXT("BtnClaimCA"), TEXT("CA"));
	BtnClaimCM = AddLoomButton(TEXT("BtnClaimCM"), TEXT("CM"));
	BtnClaimMM = AddLoomButton(TEXT("BtnClaimMM"), TEXT("MM"));
	BtnClaimMA = AddLoomButton(TEXT("BtnClaimMA"), TEXT("MA"));
	BtnClaimMC = AddLoomButton(TEXT("BtnClaimMC"), TEXT("MC"));

	BtnClaimAA->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickClaimAA);
	BtnClaimAC->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickClaimAC);
	BtnClaimAM->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickClaimAM);
	BtnClaimCC->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickClaimCC);
	BtnClaimCA->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickClaimCA);
	BtnClaimCM->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickClaimCM);
	BtnClaimMM->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickClaimMM);
	BtnClaimMA->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickClaimMA);
	BtnClaimMC->OnClicked.AddDynamic(this, &UProjectCompHUDWidget::OnClickClaimMC);
}

// ---- Helpers shared by RefreshBuyPanel ------------------------------

namespace
{
	/** Tint applied to a Loom button whose ability has already been claimed by
	 *  ANY teammate (including the local player). Brighter than the default
	 *  button tint, so the claimed buttons clearly pop. */
	const FLinearColor LoomClaimedTint   = FLinearColor(2.4f, 2.4f, 2.4f, 1.0f);
	const FLinearColor LoomUnclaimedTint = FLinearColor(0.55f, 0.55f, 0.55f, 1.0f);

	/** Walk the local player's team, calling Visitor(PS) for each member
	 *  (including self). Spectators skipped. */
	template <typename FnT>
	void ForEachTeammate(const UWorld* World, ETeamID Team, FnT&& Visitor)
	{
		if (!World || Team == ETeamID::Spectator) { return; }
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			const APlayerController* PC = It->Get();
			if (!PC) { continue; }
			const AProjectCompPlayerState* TPS = PC->GetPlayerState<AProjectCompPlayerState>();
			if (!TPS || TPS->GetTeamID() != Team) { continue; }
			Visitor(TPS);
		}
	}

	/** Update a button's child text in place — assumes the button's first
	 *  child is the UTextBlock label. */
	void SetButtonLabel(UButton* Btn, const FString& Label)
	{
		if (!Btn || Btn->GetChildrenCount() == 0) { return; }
		if (UTextBlock* T = Cast<UTextBlock>(Btn->GetChildAt(0)))
		{
			T->SetText(FText::FromString(Label));
		}
	}

	/** Build a string of N bullet characters. */
	FString BuildDots(int32 N)
	{
		if (N <= 0) { return FString(); }
		FString Out;
		Out.Reserve(N * 2);
		for (int32 i = 0; i < N; ++i)
		{
			if (i > 0) { Out.AppendChar(TEXT(' ')); }
			Out.AppendChar(TEXT('\x2022'));  // U+2022 BULLET •
		}
		return Out;
	}

	/** Append " <dots>" to the base label if Count > 0. */
	FString WithDots(const FString& BaseLabel, int32 Count)
	{
		if (Count <= 0) { return BaseLabel; }
		return FString::Printf(TEXT("%s   %s"), *BaseLabel, *BuildDots(Count));
	}

	/** Short weapon abbreviation for the teammates column. */
	const TCHAR* WeaponAbbrev(EWeaponClass W)
	{
		switch (W)
		{
		case EWeaponClass::Pistol: return TEXT("PIS");
		case EWeaponClass::SMG:    return TEXT("SMG");
		case EWeaponClass::Rifle:  return TEXT("RIF");
		default:                   return TEXT("—");
		}
	}

	const TCHAR* ArmourAbbrev(EArmourLevel A)
	{
		switch (A)
		{
		case EArmourLevel::Light: return TEXT("LA");
		case EArmourLevel::Heavy: return TEXT("HA");
		default:                  return TEXT("—");
		}
	}

	TCHAR GrenadeLetter(EGrenadeType G)
	{
		switch (G)
		{
		case EGrenadeType::Flash:      return TEXT('F');
		case EGrenadeType::Info:       return TEXT('I');
		case EGrenadeType::Vulnerable: return TEXT('V');
		case EGrenadeType::Smoke:      return TEXT('S');
		default:                       return TEXT('?');
		}
	}
}

void UProjectCompHUDWidget::RefreshBuyPanel()
{
	if (!BuyPanelRoot)
	{
		return;
	}

	RefreshBuyPanelVisibility();

	const AProjectCompPlayerState* LocalPS = GetLocalPlayerState();
	const AProjectCompGameState*   GS      = GetTypedGameState();
	UWorld* World = GetWorld();
	if (!LocalPS || !GS || !World)
	{
		return;
	}

	if (BuyHeaderCredits)
	{
		BuyHeaderCredits->SetText(FText::FromString(FString::Printf(TEXT("$%d"), LocalPS->GetCurrentCredits())));
	}
	if (BuyHeaderTimer)
	{
		BuyHeaderTimer->SetText(FormatTimeText(GS->GetPhaseTimeRemaining()));
	}
	if (BuyHeaderPhase)
	{
		BuyHeaderPhase->SetText(FormatPhaseText(GS->GetCurrentPhase()));
	}

	const ETeamID LocalTeam = LocalPS->GetTeamID();
	if (LocalTeam == ETeamID::Spectator)
	{
		return;
	}

	// ---- Tally team-wide ownership in a single walk ---------------------
	// One walk through teammates collects every count we need: per-loom-ability
	// claim flag, per-weapon-class count, per-armour-level count, per-grenade-
	// type count. Avoids re-iterating once per button.

	bool LoomClaimed[static_cast<int32>(EAbilityType::MC) + 1] = {};
	int32 PistolCount = 0, SMGCount = 0, RifleCount = 0;
	int32 LightArmCount = 0, HeavyArmCount = 0;
	int32 FlashCount = 0, InfoCount = 0, VulnCount = 0, SmokeCount = 0;

	struct FTeammateSnapshot
	{
		const AProjectCompPlayerState* PS;
	};
	TArray<FTeammateSnapshot, TInlineAllocator<5>> Teammates;

	ForEachTeammate(World, LocalTeam, [&](const AProjectCompPlayerState* TPS)
	{
		Teammates.Add({TPS});

		const EAbilityType Loom = TPS->GetChosenLoomAbility();
		if (Loom != EAbilityType::None)
		{
			LoomClaimed[static_cast<int32>(Loom)] = true;
		}

		switch (TPS->GetEquippedPrimary())
		{
		case EWeaponClass::SMG:   ++SMGCount;   break;
		case EWeaponClass::Rifle: ++RifleCount; break;
		default: break;
		}
		if (TPS->GetEquippedSecondary() == EWeaponClass::Pistol) { ++PistolCount; }
		switch (TPS->GetArmourLevel())
		{
		case EArmourLevel::Light: ++LightArmCount; break;
		case EArmourLevel::Heavy: ++HeavyArmCount; break;
		default: break;
		}
		for (EGrenadeType G : TPS->GetEquippedGrenadeTypes())
		{
			switch (G)
			{
			case EGrenadeType::Flash:      ++FlashCount; break;
			case EGrenadeType::Info:       ++InfoCount;  break;
			case EGrenadeType::Vulnerable: ++VulnCount;  break;
			case EGrenadeType::Smoke:      ++SmokeCount; break;
			default: break;
			}
		}
	});

	// ---- Loom highlights -------------------------------------------------
	auto ApplyLoomTint = [](UButton* Btn, bool bClaimed)
	{
		if (Btn)
		{
			Btn->SetColorAndOpacity(bClaimed ? LoomClaimedTint : LoomUnclaimedTint);
		}
	};
	ApplyLoomTint(BtnClaimAA, LoomClaimed[static_cast<int32>(EAbilityType::AA)]);
	ApplyLoomTint(BtnClaimAC, LoomClaimed[static_cast<int32>(EAbilityType::AC)]);
	ApplyLoomTint(BtnClaimAM, LoomClaimed[static_cast<int32>(EAbilityType::AM)]);
	ApplyLoomTint(BtnClaimCC, LoomClaimed[static_cast<int32>(EAbilityType::CC)]);
	ApplyLoomTint(BtnClaimCA, LoomClaimed[static_cast<int32>(EAbilityType::CA)]);
	ApplyLoomTint(BtnClaimCM, LoomClaimed[static_cast<int32>(EAbilityType::CM)]);
	ApplyLoomTint(BtnClaimMM, LoomClaimed[static_cast<int32>(EAbilityType::MM)]);
	ApplyLoomTint(BtnClaimMA, LoomClaimed[static_cast<int32>(EAbilityType::MA)]);
	ApplyLoomTint(BtnClaimMC, LoomClaimed[static_cast<int32>(EAbilityType::MC)]);

	// ---- Buy-button labels with teammate ownership dots ------------------
	SetButtonLabel(BtnBuyPistol,       WithDots(TEXT("PISTOL  $250"),    PistolCount));
	SetButtonLabel(BtnBuySMG,          WithDots(TEXT("SMG     $1200"),   SMGCount));
	SetButtonLabel(BtnBuyRifle,        WithDots(TEXT("RIFLE   $2700"),   RifleCount));
	SetButtonLabel(BtnBuyArmourLight,  WithDots(TEXT("LIGHT   $650"),    LightArmCount));
	SetButtonLabel(BtnBuyArmourHeavy,  WithDots(TEXT("HEAVY   $1000"),   HeavyArmCount));
	SetButtonLabel(BtnBuyFlash,        WithDots(TEXT("FLASH   $300"),    FlashCount));
	SetButtonLabel(BtnBuyInfo,         WithDots(TEXT("INFO    $300"),    InfoCount));
	SetButtonLabel(BtnBuyVuln,         WithDots(TEXT("VULN    $300"),    VulnCount));
	SetButtonLabel(BtnBuySmoke,        WithDots(TEXT("SMOKE   $300"),    SmokeCount));

	// ---- Teammate loadout rows ------------------------------------------
	// Helpers — build a fulfill button for one requested item and add it to
	// the supplied requests row.
	auto AddFulfillButton = [&](UHorizontalBox* Row, const AProjectCompPlayerState* RequesterPS,
	                            EFulfillItemKind Kind, uint8 ItemId, const FString& Label)
	{
		if (!Row || !RequesterPS) { return; }
		APlayerController* RequesterPC = Cast<APlayerController>(RequesterPS->GetOwner());
		if (!RequesterPC) { return; }

		UProjectCompFulfillButton* Btn = WidgetTree->ConstructWidget<UProjectCompFulfillButton>(
			UProjectCompFulfillButton::StaticClass());
		Btn->RequesterPC = RequesterPC;
		Btn->Kind        = Kind;
		Btn->ItemId      = ItemId;

		UTextBlock* T = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		T->SetText(FText::FromString(Label));
		T->SetFont(MakeFont(12));
		T->SetColorAndOpacity(PrimaryTextColor);
		T->SetJustification(ETextJustify::Center);
		Btn->AddChild(T);

		Btn->OnFulfillClicked.AddDynamic(this, &UProjectCompHUDWidget::OnFulfillRequestClicked);

		UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(Btn);
		S->SetPadding(FMargin(2.0f, 0.0f));
		S->SetVerticalAlignment(VAlign_Center);
	};

	for (int32 i = 0; i < TeammateRowContainers.Num(); ++i)
	{
		UVerticalBox* Container  = TeammateRowContainers[i];
		UTextBlock*   Label      = TeammateLoadoutLabels.IsValidIndex(i) ? TeammateLoadoutLabels[i].Get()   : nullptr;
		UHorizontalBox* RowReqs  = TeammateRequestsBoxes.IsValidIndex(i) ? TeammateRequestsBoxes[i].Get()   : nullptr;
		if (!Container || !Label || !RowReqs) { continue; }

		if (!Teammates.IsValidIndex(i))
		{
			Container->SetVisibility(ESlateVisibility::Collapsed);
			continue;
		}
		const AProjectCompPlayerState* TPS = Teammates[i].PS;
		if (!TPS)
		{
			Container->SetVisibility(ESlateVisibility::Collapsed);
			continue;
		}

		// Loadout label.
		FString GrenadeStr;
		for (EGrenadeType G : TPS->GetEquippedGrenadeTypes())
		{
			if (!GrenadeStr.IsEmpty()) { GrenadeStr.AppendChar(TEXT(' ')); }
			GrenadeStr.AppendChar(GrenadeLetter(G));
		}
		if (GrenadeStr.IsEmpty()) { GrenadeStr = TEXT("—"); }
		Label->SetText(FText::FromString(FString::Printf(
			TEXT("%s: %s | %s | %s | %s"),
			*TPS->GetPlayerName(),
			WeaponAbbrev(TPS->GetEquippedPrimary()),
			WeaponAbbrev(TPS->GetEquippedSecondary()),
			ArmourAbbrev(TPS->GetArmourLevel()),
			*GrenadeStr)));

		// Rebuild request buttons. Per-tick clear + rebuild — small N so OK.
		// The old widgets become unreferenced and will be GC'd.
		RowReqs->ClearChildren();
		for (EWeaponClass W : TPS->GetRequestedWeapons())
		{
			AddFulfillButton(RowReqs, TPS, EFulfillItemKind::Weapon, static_cast<uint8>(W),
				FString::Printf(TEXT("→ %s"), WeaponAbbrev(W)));
		}
		for (EArmourLevel A : TPS->GetRequestedArmour())
		{
			AddFulfillButton(RowReqs, TPS, EFulfillItemKind::Armour, static_cast<uint8>(A),
				FString::Printf(TEXT("→ %s"), ArmourAbbrev(A)));
		}
		for (EGrenadeType G : TPS->GetRequestedGrenades())
		{
			AddFulfillButton(RowReqs, TPS, EFulfillItemKind::Grenade, static_cast<uint8>(G),
				FString::Printf(TEXT("→ %c"), GrenadeLetter(G)));
		}

		Container->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	}
}

void UProjectCompHUDWidget::RefreshBuyPanelVisibility()
{
	if (!BuyPanelRoot)
	{
		return;
	}

	const AProjectCompGameState* GS = GetTypedGameState();
	if (!GS)
	{
		BuyPanelRoot->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}

	const ERoundPhase Phase   = GS->GetCurrentPhase();
	const bool bWindowOpen    = GS->IsSelectionWindowOpen();
	const bool bIsLoom        = (Phase == ERoundPhase::LoomSelection);
	const bool bIsPreRoundBuy = (Phase == ERoundPhase::PreRound && bWindowOpen);

	// Auto-close the buy menu on every fresh entry into PreRound so each round
	// starts unopened (the player has to press B to open it).
	if (Phase == ERoundPhase::PreRound && LastVisibilityPhase != ERoundPhase::PreRound)
	{
		bBuyMenuOpen = false;
	}
	LastVisibilityPhase = Phase;

	// LoomSelection is mandatory (mouse-locked) → ignore the toggle. PreRound
	// buy is opt-in via B → only show while bBuyMenuOpen.
	const bool bShowBuyContent = bIsPreRoundBuy && bBuyMenuOpen;

	if (!bIsLoom && !bShowBuyContent)
	{
		BuyPanelRoot->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	BuyPanelRoot->SetVisibility(ESlateVisibility::SelfHitTestInvisible);

	// LoomSelection: only the Loom column. PreRound buy: Weapons + Grenades + Teammates.
	if (BuyWeaponsColumn)
	{
		BuyWeaponsColumn->SetVisibility(bShowBuyContent ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (BuyGrenadesColumn)
	{
		BuyGrenadesColumn->SetVisibility(bShowBuyContent ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (BuyLoomColumn)
	{
		BuyLoomColumn->SetVisibility(bIsLoom ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (TeammateLoadoutsColumn)
	{
		TeammateLoadoutsColumn->SetVisibility(bShowBuyContent ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
}

// =====================================================================
// Buy / Claim click handlers — each forwards to the PlayerController
// =====================================================================

namespace
{
	AProjectCompPlayerController* GetTypedPC(UUserWidget* W)
	{
		return W ? Cast<AProjectCompPlayerController>(W->GetOwningPlayer()) : nullptr;
	}
}

void UProjectCompHUDWidget::OnClickBuyPistol()       { if (auto* PC = GetTypedPC(this)) PC->BuyWeapon(EWeaponClass::Pistol); }
void UProjectCompHUDWidget::OnClickBuySMG()          { if (auto* PC = GetTypedPC(this)) PC->BuyWeapon(EWeaponClass::SMG); }
void UProjectCompHUDWidget::OnClickBuyRifle()        { if (auto* PC = GetTypedPC(this)) PC->BuyWeapon(EWeaponClass::Rifle); }
void UProjectCompHUDWidget::OnClickBuyArmourLight()  { if (auto* PC = GetTypedPC(this)) PC->BuyArmour(EArmourLevel::Light); }
void UProjectCompHUDWidget::OnClickBuyArmourHeavy()  { if (auto* PC = GetTypedPC(this)) PC->BuyArmour(EArmourLevel::Heavy); }
void UProjectCompHUDWidget::OnClickBuyFlash()        { if (auto* PC = GetTypedPC(this)) PC->BuyGrenade(EGrenadeType::Flash); }
void UProjectCompHUDWidget::OnClickBuyInfo()         { if (auto* PC = GetTypedPC(this)) PC->BuyGrenade(EGrenadeType::Info); }
void UProjectCompHUDWidget::OnClickBuyVuln()         { if (auto* PC = GetTypedPC(this)) PC->BuyGrenade(EGrenadeType::Vulnerable); }
void UProjectCompHUDWidget::OnClickBuySmoke()        { if (auto* PC = GetTypedPC(this)) PC->BuyGrenade(EGrenadeType::Smoke); }

void UProjectCompHUDWidget::OnClickClaimAA() { if (auto* PC = GetTypedPC(this)) PC->ClaimLoomAbility(EAbilityType::AA); }
void UProjectCompHUDWidget::OnClickClaimAC() { if (auto* PC = GetTypedPC(this)) PC->ClaimLoomAbility(EAbilityType::AC); }
void UProjectCompHUDWidget::OnClickClaimAM() { if (auto* PC = GetTypedPC(this)) PC->ClaimLoomAbility(EAbilityType::AM); }
void UProjectCompHUDWidget::OnClickClaimCC() { if (auto* PC = GetTypedPC(this)) PC->ClaimLoomAbility(EAbilityType::CC); }
void UProjectCompHUDWidget::OnClickClaimCA() { if (auto* PC = GetTypedPC(this)) PC->ClaimLoomAbility(EAbilityType::CA); }
void UProjectCompHUDWidget::OnClickClaimCM() { if (auto* PC = GetTypedPC(this)) PC->ClaimLoomAbility(EAbilityType::CM); }
void UProjectCompHUDWidget::OnClickClaimMM() { if (auto* PC = GetTypedPC(this)) PC->ClaimLoomAbility(EAbilityType::MM); }
void UProjectCompHUDWidget::OnClickClaimMA() { if (auto* PC = GetTypedPC(this)) PC->ClaimLoomAbility(EAbilityType::MA); }
void UProjectCompHUDWidget::OnClickClaimMC() { if (auto* PC = GetTypedPC(this)) PC->ClaimLoomAbility(EAbilityType::MC); }

// =====================================================================
// Right-click on buy buttons → Request that the team buy this for me.
// SButton consumes left-clicks; we listen at the preview phase so the
// right-click reaches us even though the button child is hit-test target.
// =====================================================================

FReply UProjectCompHUDWidget::NativeOnPreviewMouseButtonDown(const FGeometry& InGeometry,
                                                             const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		if (UButton* Btn = FindBuyButtonUnderCursor(InMouseEvent.GetScreenSpacePosition()))
		{
			DispatchRightClickRequest(Btn);
			return FReply::Handled();
		}
	}
	return Super::NativeOnPreviewMouseButtonDown(InGeometry, InMouseEvent);
}

UButton* UProjectCompHUDWidget::FindBuyButtonUnderCursor(const FVector2D& ScreenPos) const
{
	// Only buy/armour/grenade buttons participate — Loom claim is left-click only.
	UButton* const Candidates[] = {
		BtnBuyPistol, BtnBuySMG, BtnBuyRifle,
		BtnBuyArmourLight, BtnBuyArmourHeavy,
		BtnBuyFlash, BtnBuyInfo, BtnBuyVuln, BtnBuySmoke,
	};
	for (UButton* B : Candidates)
	{
		if (!B || !B->IsVisible()) { continue; }
		const FGeometry& G = B->GetCachedGeometry();
		if (G.GetLocalSize().IsNearlyZero()) { continue; }
		if (G.IsUnderLocation(ScreenPos))
		{
			return B;
		}
	}
	return nullptr;
}

void UProjectCompHUDWidget::DispatchRightClickRequest(UButton* Btn)
{
	AProjectCompPlayerController* PC = GetTypedPC(this);
	if (!PC || !Btn) { return; }

	if      (Btn == BtnBuyPistol)        { PC->RequestWeapon(EWeaponClass::Pistol); }
	else if (Btn == BtnBuySMG)           { PC->RequestWeapon(EWeaponClass::SMG); }
	else if (Btn == BtnBuyRifle)         { PC->RequestWeapon(EWeaponClass::Rifle); }
	else if (Btn == BtnBuyArmourLight)   { PC->RequestArmour(EArmourLevel::Light); }
	else if (Btn == BtnBuyArmourHeavy)   { PC->RequestArmour(EArmourLevel::Heavy); }
	else if (Btn == BtnBuyFlash)         { PC->RequestGrenade(EGrenadeType::Flash); }
	else if (Btn == BtnBuyInfo)          { PC->RequestGrenade(EGrenadeType::Info); }
	else if (Btn == BtnBuyVuln)          { PC->RequestGrenade(EGrenadeType::Vulnerable); }
	else if (Btn == BtnBuySmoke)         { PC->RequestGrenade(EGrenadeType::Smoke); }
}

void UProjectCompHUDWidget::OnFulfillRequestClicked(UProjectCompFulfillButton* Button)
{
	if (!Button || !Button->RequesterPC.IsValid()) { return; }
	AProjectCompPlayerController* PC = GetTypedPC(this);
	if (!PC) { return; }

	APlayerController* RequesterPC = Button->RequesterPC.Get();
	switch (Button->Kind)
	{
	case EFulfillItemKind::Weapon:
		PC->FulfillWeaponRequest(RequesterPC, static_cast<EWeaponClass>(Button->ItemId));
		break;
	case EFulfillItemKind::Armour:
		PC->FulfillArmourRequest(RequesterPC, static_cast<EArmourLevel>(Button->ItemId));
		break;
	case EFulfillItemKind::Grenade:
		PC->FulfillGrenadeRequest(RequesterPC, static_cast<EGrenadeType>(Button->ItemId));
		break;
	default:
		break;
	}
}
