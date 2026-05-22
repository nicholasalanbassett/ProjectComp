// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompPlayerState.h"
#include "Engine/World.h"
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
	// Draft votes are an information leak across teams during PreRound — owner-only.
	// The locked-in TeamLoadout on GameState is the public picture once the draft tallies.
	DOREPLIFETIME_CONDITION(AProjectCompPlayerState, VotedAbility1, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AProjectCompPlayerState, VotedAbility2, COND_OwnerOnly);
	DOREPLIFETIME(AProjectCompPlayerState, Kills);
	DOREPLIFETIME(AProjectCompPlayerState, Deaths);
	// Lives remaining could give opponents an info edge ("they're on last life — push them") —
	// keep owner-only. Public-scoreboard versions can be added later via a separate replicated summary.
	DOREPLIFETIME_CONDITION(AProjectCompPlayerState, LivesRemaining, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AProjectCompPlayerState, RespawnEndTime, COND_OwnerOnly);
	// Flash effect is owner-only — only the flashed player needs to see their own
	// overlay, and replicating to everyone would leak "who's flashed where" to
	// opponents (info advantage). Owner-only keeps the wire compact + private.
	DOREPLIFETIME_CONDITION(AProjectCompPlayerState, FlashEndTime, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AProjectCompPlayerState, FlashFadeStartTime, COND_OwnerOnly);
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
	if (Kills < 255)
	{
		++Kills;
	}
}

void AProjectCompPlayerState::Server_AddDeath()
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	if (Deaths < 255)
	{
		++Deaths;
	}
}

void AProjectCompPlayerState::Server_DecrementLife()
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	if (LivesRemaining > 0)
	{
		--LivesRemaining;
	}
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

void AProjectCompPlayerState::Server_ApplyFlash(float FullWhiteDuration, float FadeDuration)
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	const UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.0f;
	FlashFadeStartTime = Now + FMath::Max(0.0f, FullWhiteDuration);
	FlashEndTime       = FlashFadeStartTime + FMath::Max(0.0f, FadeDuration);
}
