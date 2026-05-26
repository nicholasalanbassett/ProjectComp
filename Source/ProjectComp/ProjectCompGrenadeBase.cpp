// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompGrenadeBase.h"
#include "ProjectComp.h"
#include "ProjectCompPlayerState.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

AProjectCompGrenadeBase::AProjectCompGrenadeBase()
{
	bReplicates = true;
	SetReplicateMovement(true);
	// Tick disabled by default — subclasses re-enable if they need per-frame
	// behavior (projectile motion, persistent-zone scans). Keeps the base cheap.
	PrimaryActorTick.bCanEverTick = false;
}

void AProjectCompGrenadeBase::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AProjectCompGrenadeBase, GrenadeType);
	DOREPLIFETIME(AProjectCompGrenadeBase, OwnerTeam);
	DOREPLIFETIME(AProjectCompGrenadeBase, bDetonated);
}

AProjectCompPlayerState* AProjectCompGrenadeBase::GetOwnerPlayerState() const
{
	if (const APlayerController* PC = Cast<APlayerController>(GetOwner()))
	{
		return PC->GetPlayerState<AProjectCompPlayerState>();
	}
	return nullptr;
}

void AProjectCompGrenadeBase::Server_Initialize(ETeamID InTeam, EGrenadeType InType)
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	OwnerTeam = InTeam;
	GrenadeType = InType;

	UE_LOG(LogProjectComp, Log, TEXT("[Grenade] %s initialized: type=%d team=%d."),
		*GetName(), static_cast<int32>(InType), static_cast<int32>(InTeam));
}

void AProjectCompGrenadeBase::Server_Detonate()
{
	if (!HasAuthority() || bDetonated)
	{
		return;
	}

	bDetonated = true;

	// Server runs gameplay effect locally (authoritative state) and the cosmetic
	// hook here too — clients run the cosmetic hook via OnRep_Detonated. Splitting
	// the calls keeps server gameplay deterministic regardless of client visibility.
	HandleDetonationGameplay();
	HandleDetonationCosmetic();

	UE_LOG(LogProjectComp, Log, TEXT("[Grenade] %s detonated."), *GetName());

	if (CleanupDelay > 0.0f)
	{
		GetWorldTimerManager().SetTimer(CleanupTimer, [this]()
		{
			Destroy();
		}, CleanupDelay, false);
	}
	else
	{
		Destroy();
	}
}

void AProjectCompGrenadeBase::OnRep_Detonated()
{
	if (bDetonated)
	{
		HandleDetonationCosmetic();
	}
}
