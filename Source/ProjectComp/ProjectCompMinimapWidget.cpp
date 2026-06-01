// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompMinimapWidget.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompGameState.h"
#include "ProjectCompPlayerController.h"
#include "ProjectCompPlayerState.h"
#include "ProjectComp.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Fonts/SlateFontInfo.h"
#include "GameFramework/PlayerController.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Widgets/SViewport.h"

namespace
{
	// A "Handled" reply that explicitly KEEPS keyboard focus on the game viewport.
	// Clicking the placement map must not move focus to the widget (or clear it):
	// the resulting viewport focus-loss makes the engine flush the player's held
	// movement keys, so they stop dead until re-pressing. Forcing focus to stay on
	// the viewport prevents that flush.
	FReply HandledKeepGameFocus()
	{
		FReply Reply = FReply::Handled();
		if (GEngine && GEngine->GameViewport)
		{
			const TSharedPtr<SViewport> Viewport = GEngine->GameViewport->GetGameViewportWidget();
			if (Viewport.IsValid())
			{
				Reply.SetUserFocus(Viewport.ToSharedRef(), EFocusCause::SetDirectly);
			}
		}
		return Reply;
	}
}

void UProjectCompMinimapWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
	UpdateHintText();
	// Widget needs to receive 1/2 key events for slot selection. SetIsFocusable
	// alone isn't enough — PC also calls SetKeyboardFocus after AddToViewport.
	SetIsFocusable(true);
}

void UProjectCompMinimapWidget::BuildLayout()
{
	if (!WidgetTree)
	{
		return;
	}

	// Root: full-screen canvas. Hosts the dim background, the click area, and
	// the bottom hint text.
	UCanvasPanel* RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("MinimapRootCanvas"));
	WidgetTree->RootWidget = RootCanvas;

	// Full-screen translucent dim so the gameplay world is still vaguely
	// visible behind the map (helps the looming player keep spatial context).
	{
		UBorder* Dim = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ScreenDim"));
		Dim->SetBrushColor(ScreenDimColor);
		UCanvasPanelSlot* DimSlot = RootCanvas->AddChildToCanvas(Dim);
		DimSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
		DimSlot->SetOffsets(FMargin(0.0f));
	}

	// Center clickable map area — Border holds a single CanvasPanel child
	// (MapContent) which in turn hosts the background image + teammate markers.
	// Border = visual frame (transparent here, BP can recolor); CanvasPanel
	// inside it = multi-child layout.
	MapClickArea = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("MapClickArea"));
	MapClickArea->SetBrushColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
	{
		// Point-anchor at center + center-pivot alignment. Actual size is set
		// per-tick in NativeTick to a square based on the smaller viewport
		// axis, so the 1:1 texture never gets non-uniformly stretched no
		// matter what aspect ratio the PIE / standalone window has.
		UCanvasPanelSlot* AreaSlot = RootCanvas->AddChildToCanvas(MapClickArea);
		AreaSlot->SetAnchors(FAnchors(0.5f, 0.5f, 0.5f, 0.5f));
		AreaSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		AreaSlot->SetPosition(FVector2D(0.0f, 0.0f));
		AreaSlot->SetAutoSize(false);
		// Initial placeholder size — overwritten on the first tick.
		AreaSlot->SetSize(FVector2D(100.0f, 100.0f));
	}

	MapContent = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("MapContent"));
	MapClickArea->SetContent(MapContent);

	// Map background. Texture if assigned, else solid color fill so the area
	// is visible while you're still wiring the asset on the BP side.
	MapImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("MapImage"));
	if (MapTexture)
	{
		MapImage->SetBrushFromTexture(MapTexture);
		MapImage->SetColorAndOpacity(FLinearColor::White);
	}
	else
	{
		MapImage->SetColorAndOpacity(MapBackgroundColor);
	}
	{
		UCanvasPanelSlot* MapImageSlot = MapContent->AddChildToCanvas(MapImage);
		MapImageSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
		MapImageSlot->SetOffsets(FMargin(0.0f));
		// Background should sit under any markers / cursor — UMG renders in
		// child-add order, so adding MapImage first puts it below.
	}

	// Bottom-center status hint.
	HintText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("HintText"));
	HintText->SetJustification(ETextJustify::Center);
	{
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Regular", 16);
		HintText->SetFont(Font);
		HintText->SetColorAndOpacity(FLinearColor(0.9f, 0.9f, 0.9f, 1.0f));
		UCanvasPanelSlot* HintSlot = RootCanvas->AddChildToCanvas(HintText);
		HintSlot->SetAnchors(FAnchors(0.5f, 1.0f));
		HintSlot->SetAlignment(FVector2D(0.5f, 1.0f));
		HintSlot->SetPosition(FVector2D(0.0f, -40.0f));
		HintSlot->SetAutoSize(true);
	}
}

void UProjectCompMinimapWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// Keep MapClickArea a centered square sized to ~80% of the smaller
	// viewport axis. Without this, the click area inherits the viewport's
	// aspect ratio and stretches the 1:1 minimap texture non-uniformly —
	// clicks land off-target on non-square windows (in-editor PIE viewports
	// are often weird aspects).
	if (MapClickArea)
	{
		if (UCanvasPanelSlot* AreaSlot = Cast<UCanvasPanelSlot>(MapClickArea->Slot))
		{
			const FVector2D ViewportSize = MyGeometry.GetLocalSize();
			const float SquareSide = FMath::Min(ViewportSize.X, ViewportSize.Y) * 0.8f;
			AreaSlot->SetSize(FVector2D(SquareSide, SquareSide));
		}
	}

	UpdateTeammateMarkers();
}

FReply UProjectCompMinimapWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	const FKey Button = InMouseEvent.GetEffectingButton();

	// ---- Grenade global-drop placement: left-click drops at that spot, right-
	// click cancels. The overlay stays open until one of those happens.
	if (Mode == EMinimapMode::GrenadePlacement)
	{
		AProjectCompPlayerController* PC = GetOwningPlayer<AProjectCompPlayerController>();
		if (Button == EKeys::RightMouseButton)
		{
			if (PC)
			{
				PC->CloseGrenadePlacement();
			}
			return HandledKeepGameFocus();
		}
		if (Button == EKeys::LeftMouseButton && PC && PendingGrenadeSlot >= 0)
		{
			FVector World;
			if (ProjectScreenPosToWorld(InMouseEvent.GetScreenSpacePosition(), World))
			{
				UE_LOG(LogProjectComp, Verbose, TEXT("[Global drop] Click-placing slot %d at world %s."),
					PendingGrenadeSlot, *World.ToCompactString());
				PC->RequestGlobalDrop(PendingGrenadeSlot, World);
				PC->CloseGrenadePlacement();
			}
			// Off-map click: ignore and leave the overlay open to try again.
		}
		return HandledKeepGameFocus();
	}

	if (Button != EKeys::LeftMouseButton)
	{
		return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
	}

	if (!MapContent)
	{
		return FReply::Unhandled();
	}

	// Translate the absolute mouse position into MapContent-local pixels so
	// the projection uses the map area, not the full-screen widget.
	const FGeometry& AreaGeom = MapContent->GetCachedGeometry();
	const FVector2D LocalPixel = AreaGeom.AbsoluteToLocal(InMouseEvent.GetScreenSpacePosition());
	const FVector2D AreaSize = AreaGeom.GetLocalSize();
	if (LocalPixel.X < 0.0f || LocalPixel.Y < 0.0f
		|| LocalPixel.X > AreaSize.X || LocalPixel.Y > AreaSize.Y
		|| AreaSize.X <= KINDA_SMALL_NUMBER || AreaSize.Y <= KINDA_SMALL_NUMBER)
	{
		return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
	}

	const FVector2D WorldXY = MinimapLocalToWorldXY(LocalPixel, AreaSize);
	const FVector TargetLocation(WorldXY.X, WorldXY.Y, MapTargetZ);

	// Resolve the active slot to the team's drafted ability via GameState.
	AProjectCompPlayerController* PC = GetOwningPlayer<AProjectCompPlayerController>();
	const AProjectCompPlayerState* PS = PC ? PC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	const UWorld* World = GetWorld();
	const AProjectCompGameState* GS = World ? World->GetGameState<AProjectCompGameState>() : nullptr;
	if (!PC || !PS || !GS)
	{
		return FReply::Handled();
	}

	// Minimap is the Loom cast UI — always places the team's Slot 1 (Loom)
	// ability. Slot 2 is a mid-round ability handled by a different path
	// (character aim-cast while alive), not by this widget.
	const FTeamLoadout Loadout = GS->GetTeamLoadout(PS->GetTeamID());
	const EAbilityType LoomAbility = Loadout.AbilitySlot1;
	if (LoomAbility == EAbilityType::None)
	{
		UE_LOG(LogProjectComp, Verbose,
			TEXT("[Loom] Minimap click @ %s → team has no Loom ability drafted (slot 1 = None)."),
			*TargetLocation.ToCompactString());
		return FReply::Handled();
	}

	UE_LOG(LogProjectComp, Verbose,
		TEXT("[Loom] Minimap click @ widget(%.0f, %.0f)/(%.0f, %.0f) → world %s → Loom ability %d."),
		LocalPixel.X, LocalPixel.Y, AreaSize.X, AreaSize.Y,
		*TargetLocation.ToCompactString(), static_cast<int32>(LoomAbility));

	PC->RequestAbilityCast(LoomAbility, TargetLocation);
	return FReply::Handled();
}

void UProjectCompMinimapWidget::UpdateTeammateMarkers()
{
	if (!MapContent)
	{
		return;
	}

	const APlayerController* OwningPC = GetOwningPlayer();
	const AProjectCompPlayerState* LocalPS = OwningPC ? OwningPC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (!LocalPS)
	{
		return;
	}
	const ETeamID LocalTeam = LocalPS->GetTeamID();
	if (LocalTeam == ETeamID::Spectator)
	{
		return;
	}

	const FGeometry& AreaGeom = MapContent->GetCachedGeometry();
	const FVector2D AreaSize = AreaGeom.GetLocalSize();
	if (AreaSize.X <= KINDA_SMALL_NUMBER || AreaSize.Y <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Walk teammates, allocate/reuse markers, place them. Tracks how many we
	// actually use this frame so unused pool entries can be hidden below.
	int32 ActiveCount = 0;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC)
		{
			continue;
		}

		APawn* Pawn = PC->GetPawn();
		if (!Pawn)
		{
			continue;
		}

		const AProjectCompPlayerState* PS = PC->GetPlayerState<AProjectCompPlayerState>();
		if (!PS || !PS->IsAlive())
		{
			continue;
		}
		if (PS->GetTeamID() != LocalTeam)
		{
			continue;  // teammates only — enemy info is gated by separate detection (later phase)
		}

		// Lazily extend the pool. Markers are children of MapContent so they
		// inherit its local coordinate space (which we project into).
		if (ActiveCount >= TeammateMarkers.Num())
		{
			UImage* NewMarker = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
			NewMarker->SetColorAndOpacity(TeammateMarkerColor);
			// Pivot at center so rotation spins around the marker's middle.
			NewMarker->SetRenderTransformPivot(FVector2D(0.5f, 0.5f));
			if (UCanvasPanelSlot* MarkerSlot = MapContent->AddChildToCanvas(NewMarker))
			{
				MarkerSlot->SetAnchors(FAnchors(0.0f, 0.0f));
				MarkerSlot->SetAlignment(FVector2D(0.5f, 0.5f));
				MarkerSlot->SetSize(TeammateMarkerSize);
				MarkerSlot->SetAutoSize(false);
			}
			TeammateMarkers.Add(NewMarker);
		}

		UImage* Marker = TeammateMarkers[ActiveCount];
		if (!Marker)
		{
			continue;
		}

		const FVector PawnLoc = Pawn->GetActorLocation();
		const FVector2D PixelPos = WorldXYToMinimapLocal(PawnLoc, AreaSize);
		if (UCanvasPanelSlot* MarkerSlot = Cast<UCanvasPanelSlot>(Marker->Slot))
		{
			MarkerSlot->SetPosition(PixelPos);
		}

		// Rotate marker by pawn yaw. With the 180°-rotated projection, world +X
		// now points "left" on the widget. So a pawn at yaw=0 needs its marker
		// rotated +90° (default-down rotated 90° CW = points left). Net offset
		// vs raw yaw: +90°.
		const float YawDeg = PawnLoc.IsNearlyZero() ? 0.0f : Pawn->GetActorRotation().Yaw;
		FWidgetTransform Tr;
		Tr.Angle = YawDeg + 90.0f;
		Marker->SetRenderTransform(Tr);
		Marker->SetVisibility(ESlateVisibility::HitTestInvisible);

		++ActiveCount;
	}

	// Hide unused pool entries (e.g. a teammate died mid-round).
	for (int32 i = ActiveCount; i < TeammateMarkers.Num(); ++i)
	{
		if (TeammateMarkers[i])
		{
			TeammateMarkers[i]->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
}

void UProjectCompMinimapWidget::UpdateHintText()
{
	if (!HintText)
	{
		return;
	}
	const FString Hint = (Mode == EMinimapMode::GrenadePlacement)
		? TEXT("GLOBAL DROP — left-click the map to place • release the util key to close.")
		: TEXT("LOOM CAST — click on the map to place your Loom ability. M to close.");
	HintText->SetText(FText::FromString(Hint));
}

FVector2D UProjectCompMinimapWidget::MinimapLocalToWorldXY(const FVector2D& LocalPixel, const FVector2D& WidgetSize) const
{
	// 180°-rotated projection — widget top-left maps to (MapWorldMax.X, MapWorldMax.Y),
	// widget bottom-right to (MapWorldMin.X, MapWorldMin.Y). Mathematically
	// identical to swapping MapWorldMin <-> MapWorldMax; baked in here so the
	// bounds in BP can stay in natural (min, max) ordering.
	const float Nx = FMath::Clamp(LocalPixel.X / WidgetSize.X, 0.0f, 1.0f);
	const float Ny = FMath::Clamp(LocalPixel.Y / WidgetSize.Y, 0.0f, 1.0f);
	return FVector2D(
		FMath::Lerp(MapWorldMin.X, MapWorldMax.X, 1.0f - Nx),
		FMath::Lerp(MapWorldMin.Y, MapWorldMax.Y, 1.0f - Ny));
}

FVector2D UProjectCompMinimapWidget::WorldXYToMinimapLocal(const FVector& WorldLocation, const FVector2D& AreaSize) const
{
	// Inverse of MinimapLocalToWorldXY — also 180°-rotated.
	const float SpanX = MapWorldMax.X - MapWorldMin.X;
	const float SpanY = MapWorldMax.Y - MapWorldMin.Y;
	if (FMath::Abs(SpanX) <= KINDA_SMALL_NUMBER || FMath::Abs(SpanY) <= KINDA_SMALL_NUMBER)
	{
		return FVector2D::ZeroVector;
	}
	const float Nx = FMath::Clamp((MapWorldMax.X - WorldLocation.X) / SpanX, 0.0f, 1.0f);
	const float Ny = FMath::Clamp((MapWorldMax.Y - WorldLocation.Y) / SpanY, 0.0f, 1.0f);
	return FVector2D(Nx * AreaSize.X, Ny * AreaSize.Y);
}

// =====================================================================
// Grenade global-drop placement mode
// =====================================================================

void UProjectCompMinimapWidget::SetGrenadePlacementMode(int32 SlotIndex, const TArray<FKey>& CloseKeys)
{
	Mode = EMinimapMode::GrenadePlacement;
	PendingGrenadeSlot = SlotIndex;
	PlacementCloseKeys = CloseKeys;
	// Placement is mouse-only and must NOT steal keyboard focus: a click on a
	// focusable widget grabs focus from the game viewport, which flushes the
	// player's held movement keys (they stop dead until re-pressing). The map
	// closes via the gameplay-side slot-key release, not widget keys, so it
	// doesn't need focus here.
	SetIsFocusable(false);
	UpdateHintText();
}

void UProjectCompMinimapWidget::SetLoomCastMode()
{
	Mode = EMinimapMode::LoomCast;
	PendingGrenadeSlot = -1;
	PlacementCloseKeys.Reset();
	// Loom cast needs keyboard focus for the 1/2 slot-selection keys.
	SetIsFocusable(true);
	UpdateHintText();
}

FReply UProjectCompMinimapWidget::NativeOnKeyUp(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	// Hold-to-keep-open: releasing the slot key that opened the placement map
	// closes it. We match the raw Slate key-up here (the Enhanced Input release
	// is swallowed by the input-mode switch that opened the map, so the
	// character-side handler can't see it — but Slate still routes key-up to the
	// focused widget).
	if (Mode == EMinimapMode::GrenadePlacement && PlacementCloseKeys.Contains(InKeyEvent.GetKey()))
	{
		if (AProjectCompPlayerController* PC = GetOwningPlayer<AProjectCompPlayerController>())
		{
			PC->CloseGrenadePlacement();
		}
		return FReply::Handled();
	}
	return Super::NativeOnKeyUp(InGeometry, InKeyEvent);
}

bool UProjectCompMinimapWidget::ProjectScreenPosToWorld(const FVector2D& AbsScreenPos, FVector& OutWorld) const
{
	if (!MapContent)
	{
		return false;
	}
	const FGeometry& AreaGeom = MapContent->GetCachedGeometry();
	const FVector2D LocalPixel = AreaGeom.AbsoluteToLocal(AbsScreenPos);
	const FVector2D AreaSize = AreaGeom.GetLocalSize();
	if (LocalPixel.X < 0.0f || LocalPixel.Y < 0.0f
		|| LocalPixel.X > AreaSize.X || LocalPixel.Y > AreaSize.Y
		|| AreaSize.X <= KINDA_SMALL_NUMBER || AreaSize.Y <= KINDA_SMALL_NUMBER)
	{
		return false;
	}
	const FVector2D WorldXY = MinimapLocalToWorldXY(LocalPixel, AreaSize);
	OutWorld = FVector(WorldXY.X, WorldXY.Y, MapTargetZ);
	return true;
}
