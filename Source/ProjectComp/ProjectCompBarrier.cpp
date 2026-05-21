// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompBarrier.h"
#include "ProjectCompPlayerState.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"

AProjectCompBarrier::AProjectCompBarrier()
{
	// Tick only while active (PreRound) — cheap recheck so the material
	// re-picks if the local player's TeamID changes (halftime swap).
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	bReplicates = true;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	SetRootComponent(Mesh);
}

void AProjectCompBarrier::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AProjectCompBarrier, OwnerTeam);
	DOREPLIFETIME(AProjectCompBarrier, bIsActive);
}

void AProjectCompBarrier::BeginPlay()
{
	Super::BeginPlay();
	SetActorTickEnabled(bIsActive);
	RefreshAppearance();
}

void AProjectCompBarrier::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	RefreshAppearance();
}

void AProjectCompBarrier::Server_SetActive(bool bNewActive)
{
	if (!HasAuthority()) return;
	if (bIsActive == bNewActive) return;
	bIsActive = bNewActive;
	OnRep_IsActive();
}

void AProjectCompBarrier::Server_SwapOwnerTeam()
{
	if (!HasAuthority()) return;
	if (OwnerTeam == ETeamID::Attackers)      OwnerTeam = ETeamID::Defenders;
	else if (OwnerTeam == ETeamID::Defenders) OwnerTeam = ETeamID::Attackers;
	OnRep_OwnerTeam();
}

void AProjectCompBarrier::OnRep_OwnerTeam()
{
	RefreshAppearance();
}

void AProjectCompBarrier::OnRep_IsActive()
{
	SetActorTickEnabled(bIsActive);
	RefreshAppearance();
}

void AProjectCompBarrier::RefreshAppearance()
{
	if (!Mesh)
	{
		return;
	}

	if (!bIsActive)
	{
		Mesh->SetVisibility(false);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}

	Mesh->SetVisibility(true);
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

	const ETeamID Viewer = GetLocalViewerTeam();
	UMaterialInterface* Picked = (Viewer == OwnerTeam) ? TransparentMaterial : OpaqueMaterial;
	if (Picked)
	{
		Mesh->SetMaterial(0, Picked);
	}
}

ETeamID AProjectCompBarrier::GetLocalViewerTeam() const
{
	if (UWorld* World = GetWorld())
	{
		if (APlayerController* LocalPC = World->GetFirstPlayerController())
		{
			if (const AProjectCompPlayerState* PS = LocalPC->GetPlayerState<AProjectCompPlayerState>())
			{
				return PS->GetTeamID();
			}
		}
	}
	return ETeamID::Spectator;
}
