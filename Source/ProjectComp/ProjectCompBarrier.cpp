// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompBarrier.h"
#include "ProjectComp.h"
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

	// Per-Kind behavior. bIsActive is driven by the GameMode per phase — it's
	// true during the phases this barrier is "engaged" in, false otherwise.
	// The exact engagement semantics differ per Kind (see EBarrierKind doc).
	switch (Kind)
	{
		case EBarrierKind::AttackerSpawnLock:
			// Always visible while the round is in progress — mirrors DefenderSpawnLock.
			// PreRound = full block (strat phase lock); RoundActive + PostRound =
			// DeadPawn-only block (looming attackers stay in spawn for the safe
			// downtime; live players from either team cross freely).
			Mesh->SetVisibility(true, /*bPropagateToChildren=*/true);
			Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			if (bIsActive)
			{
				Mesh->SetCollisionResponseToAllChannels(ECR_Block);
			}
			else
			{
				Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
				Mesh->SetCollisionResponseToChannel(ECC_DeadPawn, ECR_Block);
			}
			ApplyOwnerTeamMaterial();
			return;

		case EBarrierKind::DefenderSpawnLock:
			if (bIsActive)
			{
				// RoundActive + PostRound: VISIBLE (defenders want to see where
				// their loom-safe zone ends) + blocks only ECC_DeadPawn. Live
				// players cross freely; looming defenders are held in spawn.
				Mesh->SetVisibility(true, /*bPropagateToChildren=*/true);
				Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
				Mesh->SetCollisionResponseToChannel(ECC_DeadPawn, ECR_Block);
				ApplyOwnerTeamMaterial();
			}
			else
			{
				// PreRound: fully off. Defenders walk out unobstructed so they
				// can pre-position freely up to the DefenderPreRoundLock line.
				Mesh->SetVisibility(false, /*bPropagateToChildren=*/true);
				Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			}
			return;

		case EBarrierKind::DefenderPreRoundLock:
			if (bIsActive)
			{
				// PreRound only: visible + blocks every channel. Caps how far
				// south defenders can pre-position during the strat phase.
				Mesh->SetVisibility(true, /*bPropagateToChildren=*/true);
				Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				Mesh->SetCollisionResponseToAllChannels(ECR_Block);
				ApplyOwnerTeamMaterial();
			}
			else
			{
				// RoundActive + PostRound: fully off. Gunplay flows past where
				// the wall stood. No DeadPawn block here — looming pawns are
				// already held back by their team's SpawnLock barrier.
				Mesh->SetVisibility(false, /*bPropagateToChildren=*/true);
				Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			}
			return;
	}
}

void AProjectCompBarrier::ApplyOwnerTeamMaterial()
{
	if (!Mesh) { return; }
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
