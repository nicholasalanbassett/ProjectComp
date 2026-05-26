// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompPlayerState.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"

AProjectCompPlayerState::AProjectCompPlayerState()
{
	// PlayerState replicates by default; nothing to enable here.

	// Pre-size the cooldown array so per-type lookups by enum index are always
	// in-bounds. Slot 0 (EGrenadeType::None) is unused but kept for index parity.
	GrenadeCooldownEndTimes.SetNumZeroed(static_cast<int32>(EGrenadeType::GrenadeType_MAX));
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

	// Wallet is owner-only — no tactical value in leaking enemy credit totals,
	// and tac-FPS economy convention keeps opponents guessing about buy state.
	DOREPLIFETIME_CONDITION(AProjectCompPlayerState, CurrentCredits, COND_OwnerOnly);

	// Loom claim + usage flag are public: teammates need the claim for the
	// no-duplicates draft race; enemies learn the ability when it's cast anyway.
	DOREPLIFETIME(AProjectCompPlayerState, ChosenLoomAbility);
	DOREPLIFETIME(AProjectCompPlayerState, bHasUsedLoomThisRound);

	// Grenade loadout + cooldowns are owner-only — opponent kits are revealed by
	// use, not by upfront wire data. Same info-economy logic as VotedAbility{1,2}.
	DOREPLIFETIME_CONDITION(AProjectCompPlayerState, EquippedGrenadeTypes, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AProjectCompPlayerState, GrenadeCooldownEndTimes, COND_OwnerOnly);
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
	// Use the GameState's replicated server clock (auto-synced to clients) so
	// the HUD on a remote client compares apples-to-apples when computing the
	// flash overlay alpha. Falls back to local world time if GameState isn't
	// up yet (shouldn't happen during gameplay).
	const UWorld* World = GetWorld();
	const AGameStateBase* GS = World ? World->GetGameState() : nullptr;
	const float Now = GS ? GS->GetServerWorldTimeSeconds()
	                     : (World ? World->GetTimeSeconds() : 0.0f);
	FlashFadeStartTime = Now + FMath::Max(0.0f, FullWhiteDuration);
	FlashEndTime       = FlashFadeStartTime + FMath::Max(0.0f, FadeDuration);
}

// =============================================================================
// Economy
// =============================================================================

void AProjectCompPlayerState::Server_SetCredits(int32 NewAmount)
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	CurrentCredits = static_cast<uint16>(FMath::Clamp(NewAmount, 0, static_cast<int32>(MAX_uint16)));
}

void AProjectCompPlayerState::Server_AddCredits(int32 Delta)
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	const int32 Updated = static_cast<int32>(CurrentCredits) + Delta;
	CurrentCredits = static_cast<uint16>(FMath::Clamp(Updated, 0, static_cast<int32>(MAX_uint16)));
}

bool AProjectCompPlayerState::Server_SpendCredits(int32 Cost)
{
	if (!ensure(HasAuthority()))
	{
		return false;
	}
	if (Cost <= 0 || static_cast<int32>(CurrentCredits) < Cost)
	{
		return false;
	}
	CurrentCredits -= static_cast<uint16>(Cost);
	return true;
}

// =============================================================================
// Loom
// =============================================================================

void AProjectCompPlayerState::Server_SetChosenLoomAbility(EAbilityType Ability)
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	ChosenLoomAbility = Ability;
}

void AProjectCompPlayerState::Server_ClearChosenLoomAbility()
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	ChosenLoomAbility = EAbilityType::None;
}

void AProjectCompPlayerState::Server_MarkLoomUsed()
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	bHasUsedLoomThisRound = true;
}

void AProjectCompPlayerState::Server_ResetLoomUsage()
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	bHasUsedLoomThisRound = false;
}

// =============================================================================
// Grenades
// =============================================================================

bool AProjectCompPlayerState::HasGrenadeEquipped(EGrenadeType Type) const
{
	if (Type == EGrenadeType::None)
	{
		return false;
	}
	return EquippedGrenadeTypes.Contains(Type);
}

float AProjectCompPlayerState::GetGrenadeCooldownEndTime(EGrenadeType Type) const
{
	const int32 Index = static_cast<int32>(Type);
	if (!GrenadeCooldownEndTimes.IsValidIndex(Index))
	{
		return 0.0f;
	}
	return GrenadeCooldownEndTimes[Index];
}

bool AProjectCompPlayerState::IsGrenadeOnCooldown(EGrenadeType Type) const
{
	if (!HasGrenadeEquipped(Type))
	{
		return false;
	}
	const float EndTime = GetGrenadeCooldownEndTime(Type);
	if (EndTime <= 0.0f)
	{
		return false;
	}
	const UWorld* World = GetWorld();
	const AGameStateBase* GS = World ? World->GetGameState() : nullptr;
	const float Now = GS ? GS->GetServerWorldTimeSeconds()
	                     : (World ? World->GetTimeSeconds() : 0.0f);
	return Now < EndTime;
}

bool AProjectCompPlayerState::Server_EquipGrenade(EGrenadeType Type)
{
	if (!ensure(HasAuthority()))
	{
		return false;
	}
	if (Type == EGrenadeType::None)
	{
		return false;
	}
	// 2-type cap with no same-type stacking (per design memory: grenade-system-and-loom-pivot).
	if (EquippedGrenadeTypes.Contains(Type))
	{
		return false;
	}
	constexpr int32 MaxEquippedGrenadeTypes = 2;
	if (EquippedGrenadeTypes.Num() >= MaxEquippedGrenadeTypes)
	{
		return false;
	}
	EquippedGrenadeTypes.Add(Type);
	return true;
}

void AProjectCompPlayerState::Server_ClearEquippedGrenades()
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	EquippedGrenadeTypes.Reset();
	// Wipe cooldown end-times so a re-buy of the same type next round starts ready.
	for (float& EndTime : GrenadeCooldownEndTimes)
	{
		EndTime = 0.0f;
	}
}

void AProjectCompPlayerState::Server_SetGrenadeCooldownEndTime(EGrenadeType Type, float NewEndTime)
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	const int32 Index = static_cast<int32>(Type);
	if (!GrenadeCooldownEndTimes.IsValidIndex(Index))
	{
		return;
	}
	GrenadeCooldownEndTimes[Index] = NewEndTime;
}
