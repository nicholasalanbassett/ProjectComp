// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompHUD.h"
#include "ProjectCompHUDWidget.h"
#include "Blueprint/UserWidget.h"
#include "GameFramework/PlayerController.h"

AProjectCompHUD::AProjectCompHUD()
{
	// No per-frame drawing in C++; the widget handles its own updates.
	PrimaryActorTick.bCanEverTick = false;
}

void AProjectCompHUD::BeginPlay()
{
	Super::BeginPlay();

	APlayerController* PC = GetOwningPlayerController();
	if (!PC || !PC->IsLocalController())
	{
		return;
	}

	// Prefer the designer-authored WBP HUD if one is assigned; otherwise fall
	// back to the C++-built HUD. This lets the UMG redesign land incrementally
	// without removing the working C++ HUD.
	if (DesignerHUDWidgetClass)
	{
		DesignerHUDWidget = CreateWidget<UUserWidget>(PC, DesignerHUDWidgetClass);
		if (DesignerHUDWidget)
		{
			DesignerHUDWidget->AddToViewport();
		}
		return;
	}

	if (!HUDWidgetClass)
	{
		return;
	}
	HUDWidget = CreateWidget<UProjectCompHUDWidget>(PC, HUDWidgetClass);
	if (HUDWidget)
	{
		HUDWidget->AddToViewport();
	}
}

void AProjectCompHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (HUDWidget)
	{
		HUDWidget->RemoveFromParent();
		HUDWidget = nullptr;
	}
	if (DesignerHUDWidget)
	{
		DesignerHUDWidget->RemoveFromParent();
		DesignerHUDWidget = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}
