// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompMinimapWidget.generated.h"

class UImage;
class UTexture2D;
class UCanvasPanel;
class UBorder;
class UTextBlock;
class AProjectCompPlayerController;

/**
 * Loom cast UI — tactical-map widget shown only to looming (dead) players.
 *
 * Design:
 *   - Pressing M while dead toggles this widget on/off (PC handles the input).
 *   - Player moves the OS cursor on the map; clicks place the currently-selected
 *     ability slot at the corresponding world location.
 *   - Click → world projection uses MapWorldMin / MapWorldMax (set per-level in
 *     a BP subclass) to convert widget-local pixels to world XY. Z is fixed
 *     to MapTargetZ (default 0 / floor) — the GameMode's per-ability Z-lift
 *     still applies on top so the dome ends up at head height regardless.
 *
 * Phase A scope (current): full-screen translucent overlay, placeholder
 * background, click anywhere → fires the team's slot-1 ability at the
 * projected world location.
 * Later phases: real map texture, live teammate arrows, slot selection.
 */
UCLASS(Abstract, BlueprintType)
class PROJECTCOMP_API UProjectCompMinimapWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	// ---- Per-level tunables (set in BP_MinimapWidget defaults) ----------

	/** World rectangle that the map area represents. (MinX, MinY) corresponds
	 *  to the top-left corner of the click area; (MaxX, MaxY) to bottom-right.
	 *  Phase A defaults cover roughly Substrate's playable area; tune per map. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Bounds")
	FVector2D MapWorldMin = FVector2D(-3000.0f, -3000.0f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Bounds")
	FVector2D MapWorldMax = FVector2D(3000.0f, 3000.0f);

	/** Z height applied to every click-derived TargetLocation. Per-ability
	 *  lift in the GameMode (e.g. CA's +150cm) stacks on top of this — leave
	 *  at 0 unless your level's floor isn't at world Z=0. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Bounds")
	float MapTargetZ = 0.0f;

	/** Static top-down render of the level. Assign per-level on BP_MinimapWidget
	 *  subclasses. If null, MapBackgroundColor is used instead (placeholder fill). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Visuals")
	UTexture2D* MapTexture;

	/** Fallback fill when MapTexture is unset. Replaced by the texture on assignment. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Visuals")
	FLinearColor MapBackgroundColor = FLinearColor(0.05f, 0.05f, 0.07f, 0.85f);

	/** Tint applied to the full-screen dim behind the map. Lets players
	 *  still see the world a little through the overlay. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Visuals")
	FLinearColor ScreenDimColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.55f);

	/** Color of the teammate-position markers. CS/Val convention is blue. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Teammates")
	FLinearColor TeammateMarkerColor = FLinearColor(0.30f, 0.62f, 0.95f, 1.0f);

	/** Pixel size of each teammate marker (width, height). Long axis = forward
	 *  direction so the rectangle visually reads as an arrow once rotated by yaw. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Teammates")
	FVector2D TeammateMarkerSize = FVector2D(14.0f, 22.0f);

protected:

	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

	/** Build the widget tree programmatically. */
	void BuildLayout();

	/** Update marker positions/rotations for alive teammates each tick. */
	void UpdateTeammateMarkers();

	/** Refresh the bottom hint text to reflect the current SelectedSlot. */
	void UpdateHintText();

	/** Convert a widget-local pixel into a world XY using MapWorldMin/Max. */
	FVector2D MinimapLocalToWorldXY(const FVector2D& LocalPixel, const FVector2D& WidgetSize) const;

	/** Inverse — world XY → widget-local pixel inside the map content area. */
	FVector2D WorldXYToMinimapLocal(const FVector& WorldLocation, const FVector2D& AreaSize) const;

	/** Outer Border for the click area + visual frame. */
	UPROPERTY()
	TObjectPtr<UBorder> MapClickArea;

	/** Canvas inside the Border. Hosts MapImage + dynamic teammate markers. */
	UPROPERTY()
	TObjectPtr<UCanvasPanel> MapContent;

	/** Map background image (texture if set, fallback color otherwise). */
	UPROPERTY()
	TObjectPtr<UImage> MapImage;

	/** Bottom-center hint text describing the current state ("Open for Loom cast", etc.) */
	UPROPERTY()
	TObjectPtr<UTextBlock> HintText;

	/** Pooled teammate-marker widgets. Grown lazily up to the active teammate
	 *  count; excess entries are collapsed instead of destroyed so we don't
	 *  thrash the widget tree on each tick. */
	UPROPERTY()
	TArray<TObjectPtr<UImage>> TeammateMarkers;

	/** Active ability slot the next click will place (1 or 2). Persists across
	 *  re-opens; not replicated — purely local UI state. */
	int32 SelectedSlot = 1;
};
