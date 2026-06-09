// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ProjectCompCornerMinimapWidget.generated.h"

class UImage;
class UTextBlock;
class UTexture2D;
class UCanvasPanel;
class UBorder;

/**
 * Always-on top-left HUD minimap. Reuses the same per-level map texture + world
 * bounds as the full-screen Loom/grenade map (UProjectCompMinimapWidget) and the
 * same world→pixel projection, but renders small, non-interactive, and persistent.
 *
 * Shows alive teammates (live, rotated by yaw) and revealed enemies (the "spotted"
 * reveal records from AM Utility Reveal / MM Camera Spike — they expire, so enemies
 * show only briefly). Auto-hides while the full-screen map is open or outside live
 * gameplay (lobby / match-end).
 *
 * Config (MapTexture + MapWorldMin/Max) is set on a BP subclass — copy the values
 * from BP_MinimapWidget so the corner map and full-screen map agree. Works without
 * a texture (falls back to MapBackgroundColor) for quick testing.
 */
UCLASS(BlueprintType)
class PROJECTCOMP_API UProjectCompCornerMinimapWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** World rectangle the map represents — MUST match BP_MinimapWidget's bounds. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Bounds")
	FVector2D MapWorldMin = FVector2D(-3000.0f, -3000.0f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Bounds")
	FVector2D MapWorldMax = FVector2D(3000.0f, 3000.0f);

	/** Top-down map render. If null, MapBackgroundColor fills the area instead. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Visuals")
	TObjectPtr<UTexture2D> MapTexture;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Visuals")
	FLinearColor MapBackgroundColor = FLinearColor(0.05f, 0.05f, 0.07f, 0.85f);

	/** Square pixel size of the corner map. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Layout", meta = (ClampMin = "64.0"))
	float MapSizePixels = 340.0f;

	/** Padding (px) from the top-left screen corner. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Layout", meta = (ClampMin = "0.0"))
	float ScreenMargin = 24.0f;

	/** Adopt the texture + world bounds from the full-screen Loom/grenade map so both
	 *  show the SAME level art and projections line up. Called by the PlayerController
	 *  right after creation, copying from the BP_MinimapWidget CDO (single source of
	 *  truth). Refreshes the live map image. */
	void ApplyMapConfig(UTexture2D* InTexture, const FVector2D& InWorldMin, const FVector2D& InWorldMax);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Teammates")
	FLinearColor TeammateMarkerColor = FLinearColor(0.30f, 0.62f, 0.95f, 1.0f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Teammates")
	FVector2D TeammateMarkerSize = FVector2D(10.0f, 16.0f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Enemies")
	FLinearColor EnemyMarkerColor = FLinearColor(0.95f, 0.30f, 0.25f, 1.0f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Enemies")
	FVector2D EnemyMarkerSize = FVector2D(12.0f, 12.0f);

	/** Colour of a line-of-sight "seen" enemy (the red arrow + the fading "?"). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Enemies")
	FLinearColor SpottedMarkerColor = FLinearColor(0.95f, 0.18f, 0.18f, 1.0f);

	/** Size of the in-LOS enemy arrow. Elongated so the facing direction reads. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Enemies")
	FVector2D SpottedMarkerSize = FVector2D(10.0f, 16.0f);

	/** Seconds the last-seen red "?" takes to fade after LOS is lost — MUST match the
	 *  GameMode's SpotLostFadeTime so the fade lines up with when the record expires. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap|Enemies", meta = (ClampMin = "0.1"))
	float LostFadeWindow = 3.0f;

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	void BuildLayout();
	void UpdateMarkers();

	/** True while the corner map should render — live gameplay + the full-screen
	 *  Loom/grenade map isn't open. */
	bool ShouldBeVisible() const;

	/** Same 180°-rotated world→pixel projection as the full-screen map, so markers
	 *  line up between the two. */
	FVector2D WorldToLocal(const FVector& World, const FVector2D& AreaSize) const;

	/** Lazily grow a marker pool and return the entry at Index (color/size applied
	 *  on first creation). */
	UImage* GetPooledMarker(TArray<TObjectPtr<UImage>>& Pool, int32 Index, const FLinearColor& Color, const FVector2D& Size);

	/** Lazily grow a "?" text pool (for lost-LOS enemies) and return the entry at Index. */
	UTextBlock* GetPooledText(TArray<TObjectPtr<UTextBlock>>& Pool, int32 Index);

	UPROPERTY() TObjectPtr<UBorder> RootFrame;
	UPROPERTY() TObjectPtr<UCanvasPanel> MapContent;
	UPROPERTY() TObjectPtr<UImage> MapImage;
	UPROPERTY() TArray<TObjectPtr<UImage>> TeammateMarkers;
	UPROPERTY() TArray<TObjectPtr<UImage>> EnemyMarkers;
	/** In-LOS enemies: red arrows pointing where they look. */
	UPROPERTY() TArray<TObjectPtr<UImage>> SpottedMarkers;
	/** Lost-LOS enemies: fading red "?" at the last-seen spot. */
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> LostMarkers;
};
