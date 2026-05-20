// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompHUDWidget.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompHealthComponent.h"
#include "ProjectCompGameState.h"
#include "ProjectCompPlayerState.h"
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
	RefreshDisplay();
}

void UProjectCompHUDWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	RefreshDisplay();
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
