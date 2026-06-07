// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompUIStatics.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompGameState.h"
#include "ProjectCompPlayerState.h"
#include "ProjectCompHitscanWeapon.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

namespace
{
	APlayerController* LocalPC(const UObject* WorldContextObject)
	{
		// Player index 0 = the local player on this machine (listen host or client).
		return UGameplayStatics::GetPlayerController(WorldContextObject, 0);
	}
}

AProjectCompPlayerState* UProjectCompUIStatics::GetLocalPlayerState(const UObject* WorldContextObject)
{
	const APlayerController* PC = LocalPC(WorldContextObject);
	return PC ? PC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
}

AProjectCompCharacter* UProjectCompUIStatics::GetLocalCharacter(const UObject* WorldContextObject)
{
	const APlayerController* PC = LocalPC(WorldContextObject);
	return PC ? Cast<AProjectCompCharacter>(PC->GetPawn()) : nullptr;
}

UProjectCompHealthComponent* UProjectCompUIStatics::GetLocalHealth(const UObject* WorldContextObject)
{
	const AProjectCompCharacter* Char = GetLocalCharacter(WorldContextObject);
	return Char ? Char->GetHealthComponent() : nullptr;
}

AProjectCompHitscanWeapon* UProjectCompUIStatics::GetLocalWeapon(const UObject* WorldContextObject)
{
	const AProjectCompCharacter* Char = GetLocalCharacter(WorldContextObject);
	return Char ? Char->GetCurrentWeapon() : nullptr;
}

AProjectCompGameState* UProjectCompUIStatics::GetCompGameState(const UObject* WorldContextObject)
{
	return Cast<AProjectCompGameState>(UGameplayStatics::GetGameState(WorldContextObject));
}
