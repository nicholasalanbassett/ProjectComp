// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompCornerMinimapWidget.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompGameState.h"
#include "ProjectCompPlayerState.h"
#include "ProjectCompPlayerController.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Styling/CoreStyle.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

void UProjectCompCornerMinimapWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
	// Non-interactive HUD element — never steal input/focus.
	SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UProjectCompCornerMinimapWidget::BuildLayout()
{
	if (!WidgetTree)
	{
		return;
	}

	UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("CornerRoot"));
	WidgetTree->RootWidget = Root;

	// Fixed-size frame pinned to the top-left corner.
	RootFrame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("CornerFrame"));
	RootFrame->SetBrushColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.35f));
	{
		UCanvasPanelSlot* FrameSlot = Root->AddChildToCanvas(RootFrame);
		FrameSlot->SetAnchors(FAnchors(0.0f, 0.0f));
		FrameSlot->SetAlignment(FVector2D(0.0f, 0.0f));
		FrameSlot->SetPosition(FVector2D(ScreenMargin, ScreenMargin));
		FrameSlot->SetSize(FVector2D(MapSizePixels, MapSizePixels));
		FrameSlot->SetAutoSize(false);
	}

	MapContent = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("CornerMapContent"));
	RootFrame->SetContent(MapContent);

	MapImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("CornerMapImage"));
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
		UCanvasPanelSlot* ImgSlot = MapContent->AddChildToCanvas(MapImage);
		ImgSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
		ImgSlot->SetOffsets(FMargin(0.0f));
	}
}

void UProjectCompCornerMinimapWidget::ApplyMapConfig(UTexture2D* InTexture, const FVector2D& InWorldMin, const FVector2D& InWorldMax)
{
	MapTexture = InTexture;
	MapWorldMin = InWorldMin;
	MapWorldMax = InWorldMax;

	// Refresh the already-built map image to the shared texture (or the fallback fill).
	if (MapImage)
	{
		if (MapTexture)
		{
			MapImage->SetBrushFromTexture(MapTexture);
			MapImage->SetColorAndOpacity(FLinearColor::White);
		}
		else
		{
			MapImage->SetColorAndOpacity(MapBackgroundColor);
		}
	}
}

void UProjectCompCornerMinimapWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// IMPORTANT: never collapse the widget ITSELF here — Slate doesn't tick collapsed
	// widgets, so a self-collapse (e.g. while in the lobby) would stop NativeTick and
	// the map could never turn itself back on. Keep the root ticking and toggle only
	// the inner map frame instead.
	const bool bShow = ShouldBeVisible();
	if (RootFrame)
	{
		RootFrame->SetVisibility(bShow ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (bShow)
	{
		UpdateMarkers();
	}
}

bool UProjectCompCornerMinimapWidget::ShouldBeVisible() const
{
	// Hidden while the full-screen Loom/grenade map is up (don't double up).
	if (const AProjectCompPlayerController* PC = Cast<AProjectCompPlayerController>(GetOwningPlayer()))
	{
		if (PC->IsFullscreenMapOpen())
		{
			return false;
		}
	}

	const UWorld* World = GetWorld();
	const AProjectCompGameState* GS = World ? World->GetGameState<AProjectCompGameState>() : nullptr;
	if (!GS)
	{
		return false;
	}
	const ERoundPhase Phase = GS->GetCurrentPhase();
	if (Phase == ERoundPhase::None || Phase == ERoundPhase::MatchEnded)
	{
		return false;  // lobby / results — no minimap
	}

	const APlayerController* OwningPC = GetOwningPlayer();
	const AProjectCompPlayerState* PS = OwningPC ? OwningPC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	return PS && PS->GetTeamID() != ETeamID::Spectator;
}

void UProjectCompCornerMinimapWidget::UpdateMarkers()
{
	if (!MapContent || !WidgetTree)
	{
		return;
	}
	const FVector2D AreaSize = MapContent->GetCachedGeometry().GetLocalSize();
	if (AreaSize.X <= KINDA_SMALL_NUMBER || AreaSize.Y <= KINDA_SMALL_NUMBER)
	{
		return;  // geometry not laid out yet (first frame)
	}

	UWorld* World = GetWorld();
	const APlayerController* OwningPC = GetOwningPlayer();
	const AProjectCompPlayerState* LocalPS = OwningPC ? OwningPC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (!World || !LocalPS)
	{
		return;
	}
	const ETeamID LocalTeam = LocalPS->GetTeamID();

	// ---- Teammates (incl. self): iterate replicated character pawns (client-safe;
	// GetPlayerControllerIterator only sees the local PC on clients). ----
	int32 TeammateCount = 0;
	for (TActorIterator<AProjectCompCharacter> It(World); It; ++It)
	{
		AProjectCompCharacter* Char = *It;
		if (!Char)
		{
			continue;
		}
		const AProjectCompPlayerState* PS = Char->GetPlayerState<AProjectCompPlayerState>();
		if (!PS || !PS->IsAlive() || PS->GetTeamID() != LocalTeam)
		{
			continue;
		}
		UImage* Marker = GetPooledMarker(TeammateMarkers, TeammateCount, TeammateMarkerColor, TeammateMarkerSize);
		if (!Marker)
		{
			continue;
		}
		if (UCanvasPanelSlot* MarkerSlot = Cast<UCanvasPanelSlot>(Marker->Slot))
		{
			MarkerSlot->SetPosition(WorldToLocal(Char->GetActorLocation(), AreaSize));
		}
		// +90° to match the 180°-rotated projection (same as the full-screen map).
		FWidgetTransform Tr;
		Tr.Angle = Char->GetActorRotation().Yaw + 90.0f;
		Marker->SetRenderTransform(Tr);
		Marker->SetVisibility(ESlateVisibility::HitTestInvisible);
		++TeammateCount;
	}
	for (int32 i = TeammateCount; i < TeammateMarkers.Num(); ++i)
	{
		if (TeammateMarkers[i]) { TeammateMarkers[i]->SetVisibility(ESlateVisibility::Collapsed); }
	}

	// ---- Enemies: only those currently revealed ("spotted") to this viewer. The
	// reveal records expire, so enemies show briefly. ----
	int32 EnemyCount = 0;
	const AProjectCompGameState* GS = World->GetGameState<AProjectCompGameState>();
	if (GS)
	{
		const float Now = GS->GetServerWorldTimeSeconds();
		for (const FEnemyRevealEntry& Entry : GS->GetEnemyReveals())
		{
			if (!Entry.Target || Entry.ExpireServerTime <= Now)
			{
				continue;
			}
			const bool bForMe = (Entry.RevealedToPlayer == LocalPS)
				|| (Entry.RevealedToPlayer == nullptr && Entry.RevealedToTeam == LocalTeam);
			if (!bForMe)
			{
				continue;
			}
			FVector WorldLoc = Entry.Location;
			if (Entry.bUseLiveLocation)
			{
				if (const APawn* Pawn = Entry.Target->GetPawn())
				{
					WorldLoc = Pawn->GetActorLocation();
				}
			}
			UImage* Marker = GetPooledMarker(EnemyMarkers, EnemyCount, EnemyMarkerColor, EnemyMarkerSize);
			if (!Marker)
			{
				continue;
			}
			if (UCanvasPanelSlot* MarkerSlot = Cast<UCanvasPanelSlot>(Marker->Slot))
			{
				MarkerSlot->SetPosition(WorldToLocal(WorldLoc, AreaSize));
			}
			Marker->SetVisibility(ESlateVisibility::HitTestInvisible);
			++EnemyCount;
		}
	}
	for (int32 i = EnemyCount; i < EnemyMarkers.Num(); ++i)
	{
		if (EnemyMarkers[i]) { EnemyMarkers[i]->SetVisibility(ESlateVisibility::Collapsed); }
	}

	// ---- Line-of-sight "seen" enemies (owner-only SpottedEnemies, filled server-side).
	// In LOS  → solid red arrow showing where the enemy is looking.
	// LOS lost → red "?" at the last-seen spot, fading out over LostFadeWindow. ----
	int32 SpottedCount = 0;
	int32 LostCount = 0;
	{
		const AProjectCompGameState* SpotGS = World->GetGameState<AProjectCompGameState>();
		const float SpotNow = SpotGS ? SpotGS->GetServerWorldTimeSeconds() : 0.0f;
		for (const FSpottedEnemy& Spot : LocalPS->GetSpottedEnemies())
		{
			if (Spot.ExpireServerTime <= SpotNow)
			{
				continue;  // expired / already faded out
			}
			const FVector2D Pos = WorldToLocal(Spot.Location, AreaSize);
			if (!Spot.bLastKnown)
			{
				// Currently seen → red arrow pointing the way they're facing.
				UImage* Arrow = GetPooledMarker(SpottedMarkers, SpottedCount, SpottedMarkerColor, SpottedMarkerSize);
				if (!Arrow)
				{
					continue;
				}
				if (UCanvasPanelSlot* MarkerSlot = Cast<UCanvasPanelSlot>(Arrow->Slot))
				{
					MarkerSlot->SetPosition(Pos);
				}
				FWidgetTransform Tr;
				Tr.Angle = Spot.Yaw + 90.0f;  // +90° to match the 180°-rotated projection
				Arrow->SetRenderTransform(Tr);
				Arrow->SetRenderOpacity(1.0f);
				Arrow->SetVisibility(ESlateVisibility::HitTestInvisible);
				++SpottedCount;
			}
			else
			{
				// LOS lost → fading red "?" anchored at the last place we saw them.
				UTextBlock* Q = GetPooledText(LostMarkers, LostCount);
				if (!Q)
				{
					continue;
				}
				if (UCanvasPanelSlot* TextSlot = Cast<UCanvasPanelSlot>(Q->Slot))
				{
					TextSlot->SetPosition(Pos);
				}
				const float Alpha = (LostFadeWindow > KINDA_SMALL_NUMBER)
					? FMath::Clamp((Spot.ExpireServerTime - SpotNow) / LostFadeWindow, 0.0f, 1.0f)
					: 1.0f;
				Q->SetRenderOpacity(Alpha);
				Q->SetVisibility(ESlateVisibility::HitTestInvisible);
				++LostCount;
			}
		}
	}
	for (int32 i = SpottedCount; i < SpottedMarkers.Num(); ++i)
	{
		if (SpottedMarkers[i]) { SpottedMarkers[i]->SetVisibility(ESlateVisibility::Collapsed); }
	}
	for (int32 i = LostCount; i < LostMarkers.Num(); ++i)
	{
		if (LostMarkers[i]) { LostMarkers[i]->SetVisibility(ESlateVisibility::Collapsed); }
	}
}

FVector2D UProjectCompCornerMinimapWidget::WorldToLocal(const FVector& World, const FVector2D& AreaSize) const
{
	// 180°-rotated projection (matches UProjectCompMinimapWidget::WorldXYToMinimapLocal).
	const float SpanX = MapWorldMax.X - MapWorldMin.X;
	const float SpanY = MapWorldMax.Y - MapWorldMin.Y;
	if (FMath::Abs(SpanX) <= KINDA_SMALL_NUMBER || FMath::Abs(SpanY) <= KINDA_SMALL_NUMBER)
	{
		return FVector2D::ZeroVector;
	}
	const float Nx = FMath::Clamp((MapWorldMax.X - World.X) / SpanX, 0.0f, 1.0f);
	const float Ny = FMath::Clamp((MapWorldMax.Y - World.Y) / SpanY, 0.0f, 1.0f);
	return FVector2D(Nx * AreaSize.X, Ny * AreaSize.Y);
}

UImage* UProjectCompCornerMinimapWidget::GetPooledMarker(TArray<TObjectPtr<UImage>>& Pool, int32 Index, const FLinearColor& Color, const FVector2D& Size)
{
	if (Index < Pool.Num())
	{
		return Pool[Index];
	}
	UImage* Marker = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
	Marker->SetColorAndOpacity(Color);
	Marker->SetRenderTransformPivot(FVector2D(0.5f, 0.5f));
	if (UCanvasPanelSlot* NewSlot = MapContent->AddChildToCanvas(Marker))
	{
		NewSlot->SetAnchors(FAnchors(0.0f, 0.0f));
		NewSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		NewSlot->SetSize(Size);
		NewSlot->SetAutoSize(false);
	}
	Pool.Add(Marker);
	return Marker;
}

UTextBlock* UProjectCompCornerMinimapWidget::GetPooledText(TArray<TObjectPtr<UTextBlock>>& Pool, int32 Index)
{
	if (Index < Pool.Num())
	{
		return Pool[Index];
	}
	UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Text->SetText(FText::FromString(TEXT("?")));
	Text->SetJustification(ETextJustify::Center);
	Text->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 14));
	Text->SetColorAndOpacity(FSlateColor(SpottedMarkerColor));
	if (UCanvasPanelSlot* NewSlot = MapContent->AddChildToCanvas(Text))
	{
		NewSlot->SetAnchors(FAnchors(0.0f, 0.0f));
		NewSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		NewSlot->SetAutoSize(true);
	}
	Pool.Add(Text);
	return Text;
}
