// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompMinimapCaptureActor.h"
#include "ProjectComp.h"
#include "Components/SceneComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"

AProjectCompMinimapCaptureActor::AProjectCompMinimapCaptureActor()
{
	PrimaryActorTick.bCanEverTick = false;

	// Plain scene root the designer positions. Keeping the capture camera as a
	// CHILD (offset up by CameraHeight) means re-capturing never moves the actor —
	// the earlier version made the camera the root, so SetRelativeLocation teleported
	// the whole actor to world origin on every capture.
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	CaptureComponent = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("CaptureComponent"));
	CaptureComponent->SetupAttachment(SceneRoot);

	// Orthographic top-down: no perspective distortion, so the widget's linear
	// pixel->world projection stays accurate.
	CaptureComponent->ProjectionType = ECameraProjectionMode::Orthographic;
	CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	CaptureComponent->bCaptureEveryFrame = false;
	CaptureComponent->bCaptureOnMovement = false;

	ConfigureCapture();
}

void AProjectCompMinimapCaptureActor::ConfigureCapture()
{
	if (!CaptureComponent)
	{
		return;
	}
	// Child offsets are RELATIVE to the root, so this lifts the camera above the
	// actor and points it straight down WITHOUT moving the actor itself.
	CaptureComponent->SetRelativeLocation(FVector(0.0f, 0.0f, CameraHeight));
	CaptureComponent->SetRelativeRotation(FRotator(-90.0f, 0.0f, 0.0f));
	CaptureComponent->ProjectionType = ECameraProjectionMode::Orthographic;
	CaptureComponent->OrthoWidth = CaptureWorldSize;
}

void AProjectCompMinimapCaptureActor::CaptureToTarget()
{
	if (!CaptureComponent)
	{
		return;
	}
	if (!RenderTarget)
	{
		UE_LOG(LogProjectComp, Warning,
			TEXT("[MinimapCapture] No RenderTarget assigned. Create a SQUARE Render Target asset (e.g. 1024x1024) and assign it to RenderTarget."));
		return;
	}

	ConfigureCapture();
	CaptureComponent->TextureTarget = RenderTarget;
	CaptureComponent->CaptureScene();

	LogBounds();

	UE_LOG(LogProjectComp, Log,
		TEXT("[MinimapCapture] Captured into render target '%s'. Next: right-click it in the Content Browser -> Create Static Texture, then assign that texture to the minimap widget's MapTexture."),
		*RenderTarget->GetName());
}

void AProjectCompMinimapCaptureActor::LogBounds() const
{
	const FVector Loc = GetActorLocation();
	const float Half = CaptureWorldSize * 0.5f;
	UE_LOG(LogProjectComp, Log,
		TEXT("[MinimapCapture] Paste into BP_Minimap defaults:  MapWorldMin = (X=%.1f, Y=%.1f)   MapWorldMax = (X=%.1f, Y=%.1f)   [center=(%.1f, %.1f), size=%.1f cm]"),
		Loc.X - Half, Loc.Y - Half, Loc.X + Half, Loc.Y + Half, Loc.X, Loc.Y, CaptureWorldSize);
}
