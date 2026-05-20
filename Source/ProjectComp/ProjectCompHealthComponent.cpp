// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompHealthComponent.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "ProjectCompGameMode.h"
#include "ProjectComp.h"
#include "Engine/World.h"

UProjectCompHealthComponent::UProjectCompHealthComponent()
{
	// No tick — health is event-driven.
	PrimaryComponentTick.bCanEverTick = false;

	SetIsReplicatedByDefault(true);
}

void UProjectCompHealthComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UProjectCompHealthComponent, CurrentHealth);
}

void UProjectCompHealthComponent::BeginPlay()
{
	Super::BeginPlay();

	// Server initializes; clients receive via replication.
	if (GetOwnerRole() == ROLE_Authority)
	{
		CurrentHealth = MaxHealth;
	}
}

// =====================================================================
// Server mutators
// =====================================================================

float UProjectCompHealthComponent::Server_TakeDamage(float Amount, AController* DamageInstigator)
{
	if (GetOwnerRole() != ROLE_Authority || Amount <= 0.0f || CurrentHealth <= 0.0f)
	{
		return 0.0f;
	}

	const float OldHealth = CurrentHealth;
	CurrentHealth = FMath::Max(CurrentHealth - Amount, 0.0f);
	const float Applied = OldHealth - CurrentHealth;

	OnHealthChanged.Broadcast(CurrentHealth, MaxHealth);

	if (CurrentHealth <= 0.0f)
	{
		OnDeath.Broadcast(DamageInstigator);

		// Drive the authoritative death flow through the GameMode.
		APawn* OwningPawn = Cast<APawn>(GetOwner());
		APlayerController* VictimPC = OwningPawn ? Cast<APlayerController>(OwningPawn->GetController()) : nullptr;
		if (VictimPC)
		{
			if (AProjectCompGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
			{
				GM->HandlePlayerDeath(VictimPC, DamageInstigator);
			}
		}
	}

	return Applied;
}

void UProjectCompHealthComponent::Server_ResetHealth()
{
	if (GetOwnerRole() != ROLE_Authority)
	{
		return;
	}
	CurrentHealth = MaxHealth;
	OnHealthChanged.Broadcast(CurrentHealth, MaxHealth);
}

void UProjectCompHealthComponent::Server_Heal(float Amount)
{
	if (GetOwnerRole() != ROLE_Authority || Amount <= 0.0f || CurrentHealth <= 0.0f)
	{
		return;
	}
	CurrentHealth = FMath::Min(CurrentHealth + Amount, MaxHealth);
	OnHealthChanged.Broadcast(CurrentHealth, MaxHealth);
}

// =====================================================================
// RepNotify
// =====================================================================

void UProjectCompHealthComponent::OnRep_CurrentHealth(float OldHealth)
{
	OnHealthChanged.Broadcast(CurrentHealth, MaxHealth);
	if (OldHealth > 0.0f && CurrentHealth <= 0.0f)
	{
		// Client-local visual death cue; authoritative death handling lives on the server.
		OnDeath.Broadcast(nullptr);
	}
}
