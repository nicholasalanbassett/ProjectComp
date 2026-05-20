// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompPlayerState.h"
#include "Net/UnrealNetwork.h"

AProjectCompPlayerState::AProjectCompPlayerState()
{
	// PlayerState replicates by default; nothing to enable here.
}

void AProjectCompPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AProjectCompPlayerState, TeamID);
	DOREPLIFETIME(AProjectCompPlayerState, bIsAlive);
	DOREPLIFETIME(AProjectCompPlayerState, VotedAbility1);
	DOREPLIFETIME(AProjectCompPlayerState, VotedAbility2);
	DOREPLIFETIME(AProjectCompPlayerState, Kills);
	DOREPLIFETIME(AProjectCompPlayerState, Deaths);
	DOREPLIFETIME(AProjectCompPlayerState, LivesRemaining);
	DOREPLIFETIME(AProjectCompPlayerState, RespawnEndTime);
}

void AProjectCompPlayerState::Server_SetTeamID(ETeamID NewTeam)
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	TeamID = NewTeam;
}

void AProjectCompPlayerState::Server_SetIsAlive(bool bNewAlive)
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	bIsAlive = bNewAlive;
}

void AProjectCompPlayerState::Server_SetVotes(EAbilityType First, EAbilityType Second)
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	VotedAbility1 = First;
	VotedAbility2 = Second;
}

void AProjectCompPlayerState::Server_ClearVotes()
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	VotedAbility1 = EAbilityType::None;
	VotedAbility2 = EAbilityType::None;
}

void AProjectCompPlayerState::Server_AddKill()
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	++Kills;
}

void AProjectCompPlayerState::Server_AddDeath()
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	++Deaths;
}

void AProjectCompPlayerState::Server_DecrementLife()
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	LivesRemaining = FMath::Max(0, LivesRemaining - 1);
}

void AProjectCompPlayerState::Server_ResetLives()
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	LivesRemaining = MaxLives;
}

void AProjectCompPlayerState::Server_SetRespawnEndTime(float NewTime)
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	RespawnEndTime = NewTime;
}
