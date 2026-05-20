// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "ProjectCompHUD.generated.h"

class UProjectCompHUDWidget;

/**
 * HUD owner. Constructs and tears down the main HUD widget for the local
 * player. No drawing logic of its own — everything visual is UMG.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompHUD : public AHUD
{
	GENERATED_BODY()

public:
	AProjectCompHUD();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

protected:

	/** WBP_HUD subclass to spawn. Assigned in BP defaults. */
	UPROPERTY(EditDefaultsOnly, Category = "HUD")
	TSubclassOf<UProjectCompHUDWidget> HUDWidgetClass;

	/** Active widget instance. Lives in viewport while HUD is alive. */
	UPROPERTY()
	TObjectPtr<UProjectCompHUDWidget> HUDWidget;
};
