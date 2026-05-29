// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompSettingsWidget.h"

#include "ProjectComp.h"
#include "ProjectCompPlayerController.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Fonts/SlateFontInfo.h"
#include "GameFramework/GameUserSettings.h"
#include "Styling/CoreStyle.h"

namespace
{
	const FLinearColor PanelBg     = FLinearColor(0.04f, 0.04f, 0.06f, 0.92f);
	const FLinearColor ActiveTint  = FLinearColor(0.45f, 0.92f, 1.00f, 1.0f);
	const FLinearColor InactiveText= FLinearColor(0.85f, 0.85f, 0.85f, 1.0f);
	const FLinearColor SubText     = FLinearColor(0.60f, 0.60f, 0.60f, 1.0f);

	FSlateFontInfo MakeFont(int32 Size)
	{
		return FCoreStyle::GetDefaultFontStyle("Regular", Size);
	}

	const TCHAR* TierName(int32 Level)
	{
		switch (Level)
		{
			case 1:  return TEXT("LOW");
			case 2:  return TEXT("MEDIUM");
			case 3:  return TEXT("HIGH");
			case 4:  return TEXT("EPIC");
			default: return TEXT("?");
		}
	}

	const TCHAR* TierDescription(int32 Level)
	{
		switch (Level)
		{
			case 1:  return TEXT("Max FPS\nlow detail");
			case 2:  return TEXT("Balanced\nrecommended");
			case 3:  return TEXT("Nice visuals\nneeds GPU");
			case 4:  return TEXT("Full quality\npowerful PC");
			default: return TEXT("");
		}
	}
}

void UProjectCompSettingsWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
}

void UProjectCompSettingsWidget::NativeTick(const FGeometry& InGeometry, float InDeltaTime)
{
	Super::NativeTick(InGeometry, InDeltaTime);
	RefreshHighlight();
}

void UProjectCompSettingsWidget::BuildLayout()
{
	if (!WidgetTree) { return; }

	// Root canvas anchored fullscreen — needed so we can centre the panel
	// regardless of viewport size.
	UCanvasPanel* RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("SettingsRoot"));
	WidgetTree->RootWidget = RootCanvas;

	// Centered panel container.
	UVerticalBox* Panel = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("SettingsPanel"));
	UCanvasPanelSlot* PanelSlot = RootCanvas->AddChildToCanvas(Panel);
	PanelSlot->SetAnchors(FAnchors(0.5f, 0.5f));
	PanelSlot->SetAlignment(FVector2D(0.5f, 0.5f));
	PanelSlot->SetAutoSize(true);

	// Title.
	TitleText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Title"));
	TitleText->SetText(FText::FromString(TEXT("PERFORMANCE")));
	TitleText->SetFont(MakeFont(24));
	TitleText->SetColorAndOpacity(InactiveText);
	TitleText->SetJustification(ETextJustify::Center);
	{
		UVerticalBoxSlot* S = Panel->AddChildToVerticalBox(TitleText);
		S->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 14.0f));
		S->SetHorizontalAlignment(HAlign_Center);
	}

	// Horizontal row of 4 tier buttons.
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("TierRow"));
	{
		UVerticalBoxSlot* S = Panel->AddChildToVerticalBox(Row);
		S->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 12.0f));
		S->SetHorizontalAlignment(HAlign_Center);
	}

	for (int32 i = 0; i < 4; ++i)
	{
		const int32 Level = i + 1;

		UButton* B = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(),
			FName(*FString::Printf(TEXT("BtnTier%d"), Level)));

		// Vertical stack inside each button: TIER NAME (large) + description (small).
		UVerticalBox* InnerBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(),
			FName(*FString::Printf(TEXT("InnerTier%d"), Level)));

		UTextBlock* Name = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),
			FName(*FString::Printf(TEXT("LblTier%d"), Level)));
		Name->SetText(FText::FromString(TierName(Level)));
		Name->SetFont(MakeFont(18));
		Name->SetColorAndOpacity(InactiveText);
		Name->SetJustification(ETextJustify::Center);

		// Store into the typed members — switch instead of pointer-into-TObjectPtr
		// arrays (TObjectPtr* doesn't decay to T**).
		switch (Level)
		{
			case 1: BtnLow    = B; LblLow    = Name; break;
			case 2: BtnMedium = B; LblMedium = Name; break;
			case 3: BtnHigh   = B; LblHigh   = Name; break;
			case 4: BtnEpic   = B; LblEpic   = Name; break;
			default: break;
		}

		UTextBlock* Desc = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),
			FName(*FString::Printf(TEXT("DescTier%d"), Level)));
		Desc->SetText(FText::FromString(TierDescription(Level)));
		Desc->SetFont(MakeFont(11));
		Desc->SetColorAndOpacity(SubText);
		Desc->SetJustification(ETextJustify::Center);

		UVerticalBoxSlot* NameSlot = InnerBox->AddChildToVerticalBox(Name);
		NameSlot->SetHorizontalAlignment(HAlign_Center);
		NameSlot->SetPadding(FMargin(0.0f, 6.0f, 0.0f, 4.0f));

		UVerticalBoxSlot* DescSlot = InnerBox->AddChildToVerticalBox(Desc);
		DescSlot->SetHorizontalAlignment(HAlign_Center);
		DescSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 8.0f));

		B->AddChild(InnerBox);

		UHorizontalBoxSlot* BS = Row->AddChildToHorizontalBox(B);
		BS->SetPadding(FMargin(6.0f, 0.0f));
		BS->SetVerticalAlignment(VAlign_Center);
		// Buttons auto-size to inner content; the inner labels (TIER + Desc)
		// give roughly uniform widths because the descriptions are similar length.
	}

	BtnLow->OnClicked.AddDynamic(this,    &UProjectCompSettingsWidget::OnClickLow);
	BtnMedium->OnClicked.AddDynamic(this, &UProjectCompSettingsWidget::OnClickMedium);
	BtnHigh->OnClicked.AddDynamic(this,   &UProjectCompSettingsWidget::OnClickHigh);
	BtnEpic->OnClicked.AddDynamic(this,   &UProjectCompSettingsWidget::OnClickEpic);

	// Hint text under the row.
	HintText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Hint"));
	HintText->SetText(FText::FromString(TEXT("Press P to close")));
	HintText->SetFont(MakeFont(12));
	HintText->SetColorAndOpacity(SubText);
	HintText->SetJustification(ETextJustify::Center);
	{
		UVerticalBoxSlot* S = Panel->AddChildToVerticalBox(HintText);
		S->SetPadding(FMargin(0.0f, 6.0f, 0.0f, 0.0f));
		S->SetHorizontalAlignment(HAlign_Center);
	}
}

void UProjectCompSettingsWidget::RefreshHighlight()
{
	const APlayerController* PC = GetOwningPlayer();
	const AProjectCompPlayerController* TypedPC = Cast<AProjectCompPlayerController>(PC);
	const int32 Active = TypedPC ? TypedPC->GetCurrentPerfLevel() : -1;
	if (Active == LastHighlightedLevel) { return; }
	LastHighlightedLevel = Active;

	UTextBlock* Labels[4] = { LblLow, LblMedium, LblHigh, LblEpic };
	for (int32 i = 0; i < 4; ++i)
	{
		if (!Labels[i]) { continue; }
		const int32 Level = i + 1;
		Labels[i]->SetColorAndOpacity(Active == Level ? ActiveTint : InactiveText);
	}
}

void UProjectCompSettingsWidget::ApplyTier(int32 Level)
{
	AProjectCompPlayerController* PC = Cast<AProjectCompPlayerController>(GetOwningPlayer());
	if (!PC) { return; }

	// Runtime application via the existing Perf exec — applies sg.* CVars.
	PC->Perf(Level);

	// Persistence: also push to UGameUserSettings so the choice survives a
	// restart. The engine re-applies SavedScalability on next launch.
	if (UGameUserSettings* GUS = UGameUserSettings::GetGameUserSettings())
	{
		const int32 Bucket = (Level <= 1) ? 1 : Level; // Low button = bucket 1.
		GUS->SetOverallScalabilityLevel(Bucket);
		GUS->ApplySettings(/*bCheckForCommandLineOverrides*/ false);
		GUS->SaveSettings();
	}

	// Force the highlight to repaint next tick.
	LastHighlightedLevel = -1;
}

void UProjectCompSettingsWidget::OnClickLow()    { ApplyTier(1); }
void UProjectCompSettingsWidget::OnClickMedium() { ApplyTier(2); }
void UProjectCompSettingsWidget::OnClickHigh()   { ApplyTier(3); }
void UProjectCompSettingsWidget::OnClickEpic()   { ApplyTier(4); }
