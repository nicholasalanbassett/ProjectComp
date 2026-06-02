// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompBombSpawn.h"
#include "Components/StaticMeshComponent.h"

AProjectCompBombSpawn::AProjectCompBombSpawn()
{
	PrimaryActorTick.bCanEverTick = false;

	MarkerMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MarkerMesh"));
	SetRootComponent(MarkerMesh);
	MarkerMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MarkerMesh->SetHiddenInGame(true);  // editor placement aid only
}
