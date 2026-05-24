// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompHUDWidget.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompHealthComponent.h"
#include "ProjectCompHitscanWeapon.h"
#include "ProjectCompGameState.h"
#include "ProjectCompPlayerState.h"
#include "ProjectCompBombSite.h"
#include "EngineUtils.h"
#include "Blueprint/WidgetTree.h"
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
		case ERoundPhase::PreRound:    return NSLOCTEXT("HUD", "PreRound",    "PRE-ROUND");
		case ERoundPhase::RoundActive: return NSLOCTEXT("HUD", "RoundActive", "LIVE");
		case ERoundPhase::PostRound:   return NSLOCTEXT("HUD", "PostRound",   "POST-ROUND");
		case ERoundPhase::MatchEnded:  return NSLOCTEXT("HUD", "MatchEnded",  "MATCH OVER");
		default:                       return FText::FromString(TEXT("—"));
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
