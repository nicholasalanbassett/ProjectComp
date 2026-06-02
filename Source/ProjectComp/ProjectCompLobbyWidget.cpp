// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompLobbyWidget.h"
#include "ProjectComp.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompGameState.h"
#include "ProjectCompPlayerController.h"
#include "ProjectCompPlayerState.h"
#include "InputCoreTypes.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/World.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/CoreStyle.h"
#include "Components/SlateWrapperTypes.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"

namespace
{
	FSlateFontInfo LobbyFont(int32 Size)
	{
		FSlateFontInfo F = FCoreStyle::GetDefaultFontStyle("Bold", Size);
		return F;
	}

	UTextBlock* MakeText(UWidgetTree* Tree, const FString& Text, int32 Size, const FLinearColor& Color)
	{
		UTextBlock* T = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		T->SetText(FText::FromString(Text));
		T->SetFont(LobbyFont(Size));
		T->SetColorAndOpacity(FSlateColor(Color));
		return T;
	}
}

void UProjectCompLobbyWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	// Visible (not SelfHitTestInvisible) so right-clicks on the background bubble
	// up to NativeOnMouseButtonDown for the "make lobby leader" menu.
	SetVisibility(ESlateVisibility::Visible);
	BuildLayout();
	RefreshPingCache();
	RebuildRoster();
}

void UProjectCompLobbyWidget::BuildLayout()
{
	if (!WidgetTree)
	{
		return;
	}

	RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("LobbyRoot"));
	WidgetTree->RootWidget = RootCanvas;

	// Full-screen dim — this is a "static" menu, so it fully covers the world.
	UBorder* Dim = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("LobbyDim"));
	Dim->SetBrushColor(ScreenDimColor);
	{
		UCanvasPanelSlot* S = RootCanvas->AddChildToCanvas(Dim);
		S->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
		S->SetOffsets(FMargin(0.0f));
	}

	// Centered content column.
	UVerticalBox* Center = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("LobbyCenter"));
	{
		UCanvasPanelSlot* S = RootCanvas->AddChildToCanvas(Center);
		S->SetAnchors(FAnchors(0.5f, 0.5f));
		S->SetAlignment(FVector2D(0.5f, 0.5f));
		S->SetAutoSize(true);
	}

	// Title.
	{
		UTextBlock* Title = MakeText(WidgetTree, TEXT("TEAM SELECT"), 48, FLinearColor::White);
		Title->SetJustification(ETextJustify::Center);
		UVerticalBoxSlot* VS = Center->AddChildToVerticalBox(Title);
		VS->SetHorizontalAlignment(HAlign_Center);
		VS->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 24.0f));
	}

	// Two roster columns side by side.
	UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("LobbyColumns"));
	Center->AddChildToVerticalBox(Columns);
	{
		// Attackers column.
		UVerticalBox* AttCol = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("AttCol"));
		AttackerHeader = MakeText(WidgetTree, TEXT("ATTACKERS (0)"), 26, AttackersColor);
		AttCol->AddChildToVerticalBox(AttackerHeader);
		AttackerList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("AttList"));
		AttCol->AddChildToVerticalBox(AttackerList);
		UHorizontalBoxSlot* HS = Columns->AddChildToHorizontalBox(AttCol);
		HS->SetPadding(FMargin(70.0f, 8.0f));
		HS->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

		// Defenders column.
		UVerticalBox* DefCol = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("DefCol"));
		DefenderHeader = MakeText(WidgetTree, TEXT("DEFENDERS (0)"), 26, DefendersColor);
		DefCol->AddChildToVerticalBox(DefenderHeader);
		DefenderList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("DefList"));
		DefCol->AddChildToVerticalBox(DefenderList);
		UHorizontalBoxSlot* HS2 = Columns->AddChildToHorizontalBox(DefCol);
		HS2->SetPadding(FMargin(70.0f, 8.0f));
		HS2->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	// Join buttons.
	UHorizontalBox* JoinRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("JoinRow"));
	{
		UVerticalBoxSlot* VS = Center->AddChildToVerticalBox(JoinRow);
		VS->SetHorizontalAlignment(HAlign_Center);
		VS->SetPadding(FMargin(0.0f, 24.0f, 0.0f, 0.0f));
	}
	{
		JoinAttackersButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("JoinAtt"));
		JoinAttackersButton->AddChild(MakeText(WidgetTree, TEXT("JOIN ATTACKERS"), 20, FLinearColor::Black));
		JoinAttackersButton->OnClicked.AddDynamic(this, &UProjectCompLobbyWidget::OnJoinAttackersClicked);
		UHorizontalBoxSlot* HS = JoinRow->AddChildToHorizontalBox(JoinAttackersButton);
		HS->SetPadding(FMargin(12.0f, 0.0f));

		JoinDefendersButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("JoinDef"));
		JoinDefendersButton->AddChild(MakeText(WidgetTree, TEXT("JOIN DEFENDERS"), 20, FLinearColor::Black));
		JoinDefendersButton->OnClicked.AddDynamic(this, &UProjectCompLobbyWidget::OnJoinDefendersClicked);
		UHorizontalBoxSlot* HS2 = JoinRow->AddChildToHorizontalBox(JoinDefendersButton);
		HS2->SetPadding(FMargin(12.0f, 0.0f));
	}

	// Host-only Start Match button.
	{
		StartMatchButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("StartMatch"));
		StartMatchLabel = MakeText(WidgetTree, TEXT("START MATCH"), 24, FLinearColor::Black);
		StartMatchButton->AddChild(StartMatchLabel);
		StartMatchButton->OnClicked.AddDynamic(this, &UProjectCompLobbyWidget::OnStartMatchClicked);
		UVerticalBoxSlot* VS = Center->AddChildToVerticalBox(StartMatchButton);
		VS->SetHorizontalAlignment(HAlign_Center);
		VS->SetPadding(FMargin(0.0f, 28.0f, 0.0f, 0.0f));
	}

	// Hint.
	{
		UTextBlock* Hint = MakeText(WidgetTree, TEXT("Pick a side. The lobby leader can right-click a name to pass leadership, and starts the match."), 16, FLinearColor(0.8f, 0.8f, 0.8f, 1.0f));
		Hint->SetJustification(ETextJustify::Center);
		UVerticalBoxSlot* VS = Center->AddChildToVerticalBox(Hint);
		VS->SetHorizontalAlignment(HAlign_Center);
		VS->SetPadding(FMargin(0.0f, 16.0f, 0.0f, 0.0f));
	}

	// Right-click "Make Lobby Leader" box — collapsed until opened at the cursor.
	ContextMenu = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("LobbyContext"));
	ContextMenu->SetBrushColor(FLinearColor(0.10f, 0.10f, 0.14f, 0.98f));
	ContextMenu->SetPadding(FMargin(4.0f));
	ContextMenu->SetVisibility(ESlateVisibility::Collapsed);
	{
		MakeLeaderButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("MakeLeaderBtn"));
		MakeLeaderButton->AddChild(MakeText(WidgetTree, TEXT("Make Lobby Leader"), 16, FLinearColor::Black));
		MakeLeaderButton->OnClicked.AddDynamic(this, &UProjectCompLobbyWidget::OnMakeLeaderClicked);
		ContextMenu->AddChild(MakeLeaderButton);

		UCanvasPanelSlot* CS = RootCanvas->AddChildToCanvas(ContextMenu);
		CS->SetAutoSize(true);
		CS->SetAlignment(FVector2D(0.0f, 0.0f));
	}
}

void UProjectCompLobbyWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	PingTimer += InDeltaTime;
	if (PingTimer >= PingRefreshInterval)
	{
		PingTimer = 0.0f;
		RefreshPingCache();
	}

	RosterTimer += InDeltaTime;
	if (RosterTimer >= RosterRefreshInterval)
	{
		RosterTimer = 0.0f;
		RebuildRoster();
	}
}

void UProjectCompLobbyWidget::RefreshPingCache()
{
	const UWorld* World = GetWorld();
	const AGameStateBase* GS = World ? World->GetGameState() : nullptr;
	if (!GS)
	{
		return;
	}
	PingCache.Reset();
	for (APlayerState* PS : GS->PlayerArray)
	{
		if (PS)
		{
			PingCache.Add(PS, FMath::RoundToInt(PS->GetPingInMilliseconds()));
		}
	}
}

void UProjectCompLobbyWidget::RebuildRoster()
{
	if (!AttackerList || !DefenderList)
	{
		return;
	}
	AttackerList->ClearChildren();
	DefenderList->ClearChildren();
	RosterRows.Reset();

	const UWorld* World = GetWorld();
	const AProjectCompGameState* GS = World ? World->GetGameState<AProjectCompGameState>() : nullptr;
	const APlayerState* LocalPS = GetOwningPlayer() ? GetOwningPlayer()->PlayerState : nullptr;
	if (!GS)
	{
		return;
	}
	const APlayerState* Leader = GS->GetLobbyLeader();

	int32 AttCount = 0;
	int32 DefCount = 0;
	for (APlayerState* Base : GS->PlayerArray)
	{
		const AProjectCompPlayerState* PS = Cast<AProjectCompPlayerState>(Base);
		if (!PS)
		{
			continue;
		}
		const ETeamID Team = PS->GetTeamID();
		if (Team != ETeamID::Attackers && Team != ETeamID::Defenders)
		{
			continue;  // spectators not listed
		}
		const int32* Cached = PingCache.Find(Base);
		const int32 PingMs = Cached ? *Cached : FMath::RoundToInt(Base->GetPingInMilliseconds());
		const bool bIsLocal = (Base == LocalPS);
		const bool bIsLeader = (Base == Leader);

		if (Team == ETeamID::Attackers)
		{
			AddRosterRow(AttackerList, Base, PS->GetPlayerName(), PingMs, bIsLocal, bIsLeader, AttackersColor);
			++AttCount;
		}
		else
		{
			AddRosterRow(DefenderList, Base, PS->GetPlayerName(), PingMs, bIsLocal, bIsLeader, DefendersColor);
			++DefCount;
		}
	}

	if (AttackerHeader)
	{
		AttackerHeader->SetText(FText::FromString(FString::Printf(TEXT("ATTACKERS (%d)"), AttCount)));
	}
	if (DefenderHeader)
	{
		DefenderHeader->SetText(FText::FromString(FString::Printf(TEXT("DEFENDERS (%d)"), DefCount)));
	}

	// Only the lobby leader sees / can use Start.
	if (StartMatchButton)
	{
		StartMatchButton->SetVisibility(IsLocalLeader() ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
}

void UProjectCompLobbyWidget::AddRosterRow(UVerticalBox* List, APlayerState* PS, const FString& Name, int32 PingMs, bool bIsLocal, bool bIsLeader, const FLinearColor& TeamColor)
{
	if (!List || !WidgetTree)
	{
		return;
	}
	const TCHAR* Prefix = bIsLeader ? TEXT("[Leader] ") : (bIsLocal ? TEXT("> ") : TEXT(""));
	const FString RowText = FString::Printf(TEXT("%s%s    %dms"), Prefix, *Name, PingMs);
	const FLinearColor Color = bIsLocal ? FLinearColor::White : FLinearColor(TeamColor.R, TeamColor.G, TeamColor.B, 0.85f);
	UTextBlock* Row = MakeText(WidgetTree, RowText, 18, Color);
	UVerticalBoxSlot* VS = List->AddChildToVerticalBox(Row);
	VS->SetPadding(FMargin(0.0f, 3.0f));
	RosterRows.Add({ Row, PS });  // track for right-click hit testing
}

bool UProjectCompLobbyWidget::IsLocalLeader() const
{
	const UWorld* World = GetWorld();
	const AProjectCompGameState* GS = World ? World->GetGameState<AProjectCompGameState>() : nullptr;
	const APlayerState* LocalPS = GetOwningPlayer() ? GetOwningPlayer()->PlayerState : nullptr;
	return GS && LocalPS && GS->GetLobbyLeader() == LocalPS;
}

FReply UProjectCompLobbyWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	const FVector2D AbsPos = InMouseEvent.GetScreenSpacePosition();
	const FKey Button = InMouseEvent.GetEffectingButton();

	if (Button == EKeys::LeftMouseButton)
	{
		// Click off an open context menu closes it (clicks on it are handled by the button).
		if (ContextMenu && ContextMenu->GetVisibility() != ESlateVisibility::Collapsed
			&& !ContextMenu->GetCachedGeometry().IsUnderLocation(AbsPos))
		{
			ContextMenu->SetVisibility(ESlateVisibility::Collapsed);
		}
		return FReply::Unhandled();  // let Join/Start buttons still receive the click
	}

	// Right-click a roster row → offer "Make Lobby Leader" (leader only, not yourself).
	if (Button == EKeys::RightMouseButton && IsLocalLeader())
	{
		const APlayerState* LocalPS = GetOwningPlayer() ? GetOwningPlayer()->PlayerState : nullptr;
		for (const FRosterRow& Row : RosterRows)
		{
			const UTextBlock* RowW = Row.Widget.Get();
			APlayerState* PS = Row.PS.Get();
			if (!RowW || !PS || PS == LocalPS)
			{
				continue;
			}
			if (RowW->GetCachedGeometry().IsUnderLocation(AbsPos))
			{
				ContextTargetPS = PS;
				OpenContextMenuAt(AbsPos);
				return FReply::Handled();
			}
		}
		// Right-click on empty space dismisses any open menu.
		if (ContextMenu)
		{
			ContextMenu->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
	return FReply::Unhandled();
}

void UProjectCompLobbyWidget::OpenContextMenuAt(const FVector2D& AbsScreenPos)
{
	if (!ContextMenu || !RootCanvas)
	{
		return;
	}
	const FVector2D Local = RootCanvas->GetCachedGeometry().AbsoluteToLocal(AbsScreenPos);
	if (UCanvasPanelSlot* S = Cast<UCanvasPanelSlot>(ContextMenu->Slot))
	{
		S->SetPosition(Local);
	}
	ContextMenu->SetVisibility(ESlateVisibility::Visible);
}

void UProjectCompLobbyWidget::OnMakeLeaderClicked()
{
	if (APlayerState* Target = ContextTargetPS.Get())
	{
		if (AProjectCompPlayerController* PC = GetOwningPlayer<AProjectCompPlayerController>())
		{
			PC->RequestSetLobbyLeader(Target);
		}
	}
	ContextTargetPS = nullptr;
	if (ContextMenu)
	{
		ContextMenu->SetVisibility(ESlateVisibility::Collapsed);
	}
}

void UProjectCompLobbyWidget::OnJoinAttackersClicked()
{
	if (AProjectCompPlayerController* PC = GetOwningPlayer<AProjectCompPlayerController>())
	{
		PC->RequestTeamChange(ETeamID::Attackers);
	}
}

void UProjectCompLobbyWidget::OnJoinDefendersClicked()
{
	if (AProjectCompPlayerController* PC = GetOwningPlayer<AProjectCompPlayerController>())
	{
		PC->RequestTeamChange(ETeamID::Defenders);
	}
}

void UProjectCompLobbyWidget::OnStartMatchClicked()
{
	if (AProjectCompPlayerController* PC = GetOwningPlayer<AProjectCompPlayerController>())
	{
		PC->RequestStartMatch();
	}
}
