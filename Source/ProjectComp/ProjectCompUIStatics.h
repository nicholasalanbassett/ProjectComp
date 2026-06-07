// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ProjectCompUIStatics.generated.h"

class AProjectCompPlayerState;
class AProjectCompGameState;
class AProjectCompCharacter;
class AProjectCompHitscanWeapon;
class UProjectCompHealthComponent;

/**
 * Blueprint helper library for UI (WBP HUD / buy menu). Wraps the local-player
 * traversal so a designer's WBP can fetch typed game state in a single node
 * instead of casting GetOwningPlayer->PlayerState/Pawn/etc. by hand in every
 * binding. All read-only; the actual data getters live on the returned objects
 * (already BlueprintPure) and all writes go through the PlayerController RPCs.
 */
UCLASS()
class PROJECTCOMP_API UProjectCompUIStatics : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Local player's typed PlayerState. Null until possessed / on a non-client. */
	UFUNCTION(BlueprintPure, Category = "ProjectComp|UI", meta = (WorldContext = "WorldContextObject"))
	static AProjectCompPlayerState* GetLocalPlayerState(const UObject* WorldContextObject);

	/** Local player's typed character pawn. Null while looming/unpossessed. */
	UFUNCTION(BlueprintPure, Category = "ProjectComp|UI", meta = (WorldContext = "WorldContextObject"))
	static AProjectCompCharacter* GetLocalCharacter(const UObject* WorldContextObject);

	/** Local player's health component (HP source for the HUD). */
	UFUNCTION(BlueprintPure, Category = "ProjectComp|UI", meta = (WorldContext = "WorldContextObject"))
	static UProjectCompHealthComponent* GetLocalHealth(const UObject* WorldContextObject);

	/** Local player's equipped weapon (ammo / reload readout source). */
	UFUNCTION(BlueprintPure, Category = "ProjectComp|UI", meta = (WorldContext = "WorldContextObject"))
	static AProjectCompHitscanWeapon* GetLocalWeapon(const UObject* WorldContextObject);

	/** The match GameState (phase, timer, scores, enemy reveals). */
	UFUNCTION(BlueprintPure, Category = "ProjectComp|UI", meta = (WorldContext = "WorldContextObject"))
	static AProjectCompGameState* GetCompGameState(const UObject* WorldContextObject);
};
