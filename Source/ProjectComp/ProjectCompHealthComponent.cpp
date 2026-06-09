// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompHealthComponent.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "ProjectCompGameMode.h"
#include "ProjectCompPlayerState.h"
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

	// Owner-only: opponents must not be able to memory-scan enemy HP through walls.
	// The killfeed/death events are sufficient public signal. Teammate HP visibility
	// (UI for low-HP teammates) is a separate channel and not implemented yet.
	DOREPLIFETIME_CONDITION(UProjectCompHealthComponent, CurrentHealth, COND_OwnerOnly);
}

void UProjectCompHealthComponent::BeginPlay()
{
	Super::BeginPlay();

	// Server initializes; clients receive via replication.
	if (GetOwnerRole() == ROLE_Authority)
	{
		CurrentHealth = static_cast<uint8>(FMath::Clamp(MaxHealth, 0.0f, 255.0f));
	}
}

// =====================================================================
// Server mutators
// =====================================================================

float UProjectCompHealthComponent::Server_TakeDamage(float Amount, AController* DamageInstigator)
{
	if (GetOwnerRole() != ROLE_Authority || Amount <= 0.0f || CurrentHealth == 0)
	{
		return 0.0f;
	}

	// Math in float so fractional damage doesn't silently round to zero — rounded
	// to nearest int at write-back so the wire format stays uint8.
	const float OldHealthFloat = static_cast<float>(CurrentHealth);
	const float NewHealthFloat = FMath::Max(OldHealthFloat - Amount, 0.0f);
	CurrentHealth = static_cast<uint8>(FMath::RoundToInt(NewHealthFloat));
	const float Applied = OldHealthFloat - static_cast<float>(CurrentHealth);

	OnHealthChanged.Broadcast(static_cast<float>(CurrentHealth), MaxHealth);

	// Credit the damage to the (enemy) instigator's match stats + remember them for
	// assist awarding on death. Damage goes to the PENDING (round) counter so it's
	// hidden until round end. Self-damage and friendly fire don't count.
	if (Applied > 0.0f && DamageInstigator)
	{
		APawn* OwningPawn = Cast<APawn>(GetOwner());
		AController* VictimController = OwningPawn ? OwningPawn->GetController() : nullptr;
		if (DamageInstigator != VictimController)
		{
			AProjectCompPlayerState* InstPS = DamageInstigator->GetPlayerState<AProjectCompPlayerState>();
			const AProjectCompPlayerState* VictimPS = VictimController ? VictimController->GetPlayerState<AProjectCompPlayerState>() : nullptr;
			const bool bEnemy = InstPS && VictimPS
				&& InstPS->GetTeamID() != ETeamID::Spectator
				&& InstPS->GetTeamID() != VictimPS->GetTeamID();
			if (bEnemy)
			{
				InstPS->Server_AddPendingDamage(Applied);
				RecentDamagers.AddUnique(DamageInstigator);
			}
		}
	}

	if (CurrentHealth == 0)
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
	CurrentHealth = static_cast<uint8>(FMath::Clamp(MaxHealth, 0.0f, 255.0f));
	OnHealthChanged.Broadcast(static_cast<float>(CurrentHealth), MaxHealth);
	// New life — clear the assist-damager list so old damagers don't carry over.
	RecentDamagers.Reset();
}

void UProjectCompHealthComponent::Server_Heal(float Amount)
{
	if (GetOwnerRole() != ROLE_Authority || Amount <= 0.0f || CurrentHealth == 0)
	{
		return;
	}
	const float NewHealthFloat = FMath::Min(static_cast<float>(CurrentHealth) + Amount, MaxHealth);
	CurrentHealth = static_cast<uint8>(FMath::RoundToInt(NewHealthFloat));
	OnHealthChanged.Broadcast(static_cast<float>(CurrentHealth), MaxHealth);
}

// =====================================================================
// RepNotify
// =====================================================================

void UProjectCompHealthComponent::OnRep_CurrentHealth(uint8 OldHealth)
{
	OnHealthChanged.Broadcast(static_cast<float>(CurrentHealth), MaxHealth);
	if (OldHealth > 0 && CurrentHealth == 0)
	{
		// Client-local visual death cue; authoritative death handling lives on the server.
		OnDeath.Broadcast(nullptr);
	}
}
