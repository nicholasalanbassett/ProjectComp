// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProjectCompBombSpawn.generated.h"

class UStaticMeshComponent;

/**
 * Level-placed marker for where the bomb spawns (on the ground, pickup-able) at
 * the start of each round. Place one in the level near the attacker spawn. The
 * GameMode caches it (TActorIterator) and resets the bomb to its location each
 * round. Has a small editor-only mesh for visibility; hidden in game.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompBombSpawn : public AActor
{
	GENERATED_BODY()

public:
	AProjectCompBombSpawn();

protected:
	/** Editor-visibility marker only (hidden in game). */
	UPROPERTY(VisibleAnywhere, Category = "BombSpawn")
	TObjectPtr<UStaticMeshComponent> MarkerMesh;
};
