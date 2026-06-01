// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProjectCompMinimapCaptureActor.generated.h"

class USceneComponent;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

/**
 * Editor helper for authoring the global-drop minimap texture + world bounds.
 *
 * Workflow:
 *   1. Drop one in the level, positioned (in X/Y) over the CENTER of the area you
 *      want the map to cover. Z can sit at ground level — the camera is lifted by
 *      CameraHeight automatically (as a child offset, so capturing never moves the
 *      actor).
 *   2. Set CaptureWorldSize to the square span (cm) the map should cover.
 *   3. Create a SQUARE Render Target (e.g. 1024x1024) and assign it to RenderTarget.
 *   4. Click "Capture To Target" in the Details panel. It renders an orthographic
 *      top-down into the render target and logs the exact MapWorldMin / MapWorldMax.
 *   5. Right-click the render target -> Create Static Texture; assign that to the
 *      minimap widget's MapTexture and paste the logged bounds into MapWorldMin/Max.
 *
 * Orthographic (not perspective) is required: the widget projects clicks to world
 * with a linear lerp between MapWorldMin/Max, which only stays accurate with a
 * distortion-free top-down.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompMinimapCaptureActor : public AActor
{
	GENERATED_BODY()

public:
	AProjectCompMinimapCaptureActor();

	/** Square world span (cm) the capture covers, centered on this actor's X/Y.
	 *  This is the camera's OrthoWidth and defines MapWorldMin/Max. */
	UPROPERTY(EditAnywhere, Category = "Minimap Capture", meta = (ClampMin = "100.0"))
	float CaptureWorldSize = 6000.0f;

	/** Height (cm) above this actor to place the ortho camera. Must clear the
	 *  tallest geometry you want visible on the map. */
	UPROPERTY(EditAnywhere, Category = "Minimap Capture", meta = (ClampMin = "100.0"))
	float CameraHeight = 20000.0f;

	/** Rotates ONLY the captured image (degrees) for orientation — use this to line
	 *  the map up with the widget instead of rotating the actor (which would just
	 *  move the camera around). Location and rotation stay independent. Try
	 *  0 / 90 / 180 / 270 if your first test click lands rotated/mirrored. */
	UPROPERTY(EditAnywhere, Category = "Minimap Capture", meta = (ClampMin = "-360.0", ClampMax = "360.0"))
	float MapYaw = 0.0f;

	/** Square render target to capture into (e.g. 1024x1024). Bake it to a static
	 *  texture afterward (right-click -> Create Static Texture). */
	UPROPERTY(EditAnywhere, Category = "Minimap Capture")
	TObjectPtr<UTextureRenderTarget2D> RenderTarget;

	/** Render the orthographic top-down into RenderTarget now, and log the bounds. */
	UFUNCTION(CallInEditor, Category = "Minimap Capture")
	void CaptureToTarget();

	/** Log the MapWorldMin/Max for the current position + CaptureWorldSize without
	 *  capturing (handy after nudging the actor). */
	UFUNCTION(CallInEditor, Category = "Minimap Capture")
	void LogBounds() const;

protected:
	/** Scene root the user positions. The capture camera is a child lifted above
	 *  it — so re-capturing never moves the actor. */
	UPROPERTY(VisibleAnywhere, Category = "Minimap Capture")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Minimap Capture")
	TObjectPtr<USceneCaptureComponent2D> CaptureComponent;

	/** Push CameraHeight / OrthoWidth / look-down rotation onto the child capture
	 *  component (without touching the actor's own transform). */
	void ConfigureCapture();
};
