// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompGameState.h"
#include "Net/UnrealNetwork.h"
#include "ProjectComp.h"

AProjectCompGameState::AProjectCompGameState()
{
	// State only; never tick on the GameState (logic lives in GameMode).
	PrimaryActorTick.bCanEverTick = false;

	// Two teams: Attackers (0), Defenders (1). Initialized with default loadouts.
	Teams.SetNum(2);
}

void AProjectCompGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AProjectCompGameState, CurrentPhase);
	DOREPLIFETIME(AProjectCompGameState, CurrentRound);
	DOREPLIFETIME(AProjectCompGameState, PhaseTimeRemainingDs);
	DOREPLIFETIME(AProjectCompGameState, Teams);
	DOREPLIFETIME(AProjectCompGameState, bSelectionWindowOpen);
	DOREPLIFETIME(AProjectCompGameState, LobbyLeader);
	DOREPLIFETIME(AProjectCompGameState, Bomb);
	DOREPLIFETIME(AProjectCompGameState, MatchWinner);
	DOREPLIFETIME(AProjectCompGameState, EnemyReveals);
}

// =====================================================================
// Public accessors
// =====================================================================

FTeamLoadout AProjectCompGameState::GetTeamLoadout(ETeamID Team) const
{
	if (const FTeamLoadout* Loadout = GetTeamLoadoutConst(Team))
	{
		return *Loadout;
	}
	return FTeamLoadout{};
}

bool AProjectCompGameState::IsTeamAbilityReady(ETeamID Team) const
{
	const FTeamLoadout* Loadout = GetTeamLoadoutConst(Team);
	return Loadout && Loadout->SharedCooldownPool >= 1.0f;
}

// =====================================================================
// Server mutators — only callable by the GameMode (HasAuthority guarded)
// =====================================================================

void AProjectCompGameState::Server_SetPhase(ERoundPhase NewPhase)
{
	if (!ensure(HasAuthority())) return;
	CurrentPhase = NewPhase;
	OnRep_CurrentPhase();  // fire on server too so any local UI updates uniformly
}

void AProjectCompGameState::Server_SetRound(int32 NewRound)
{
	if (!ensure(HasAuthority())) return;
	CurrentRound = NewRound;
}

void AProjectCompGameState::Server_SetPhaseTimeRemaining(float NewTime)
{
	if (!ensure(HasAuthority())) return;
	// Server keeps the precise float so countdown math doesn't snap to deciseconds
	// each tick; clients only get the uint16 wire value.
	const float Clamped = FMath::Clamp(NewTime, 0.0f, 6553.5f);
	PhaseTimeRemainingServerFloat = Clamped;
	PhaseTimeRemainingDs = static_cast<uint16>(FMath::RoundToInt(Clamped * 10.0f));
}

void AProjectCompGameState::Server_SetTeamLoadout(ETeamID Team, const FTeamLoadout& NewLoadout)
{
	if (!ensure(HasAuthority())) return;
	if (FTeamLoadout* Loadout = GetTeamLoadoutMutable(Team))
	{
		*Loadout = NewLoadout;
	}
}

void AProjectCompGameState::Server_SetTeamCooldownPool(ETeamID Team, float NewPool)
{
	if (!ensure(HasAuthority())) return;
	if (FTeamLoadout* Loadout = GetTeamLoadoutMutable(Team))
	{
		Loadout->SharedCooldownPool = FMath::Clamp(NewPool, 0.0f, 1.0f);
	}
}

void AProjectCompGameState::Server_AddTeamScore(ETeamID Team, int32 Delta)
{
	if (!ensure(HasAuthority())) return;
	if (FTeamLoadout* Loadout = GetTeamLoadoutMutable(Team))
	{
		Loadout->Score += Delta;
	}
}

void AProjectCompGameState::Server_SetSelectionWindowOpen(bool bOpen)
{
	if (!ensure(HasAuthority())) return;
	bSelectionWindowOpen = bOpen;
}

void AProjectCompGameState::Server_SetMatchWinner(ETeamID Team)
{
	if (!ensure(HasAuthority())) return;
	MatchWinner = Team;
}

void AProjectCompGameState::Server_SetLobbyLeader(APlayerState* NewLeader)
{
	if (!ensure(HasAuthority())) return;
	LobbyLeader = NewLeader;
}

void AProjectCompGameState::Server_SetBomb(AProjectCompBomb* InBomb)
{
	if (!ensure(HasAuthority())) return;
	Bomb = InBomb;
}

void AProjectCompGameState::Server_AddEnemyReveal(const FEnemyRevealEntry& Entry)
{
	if (!ensure(HasAuthority())) return;
	if (!Entry.Target)
	{
		return;
	}
	// Upsert: a continuously-refreshing producer (Camera Spike) re-reveals the
	// same enemy every scan, so match an existing record for this (Target, viewer)
	// pair and refresh it in place rather than appending duplicates.
	for (FEnemyRevealEntry& Existing : EnemyReveals)
	{
		if (Existing.Target == Entry.Target
			&& Existing.RevealedToTeam == Entry.RevealedToTeam
			&& Existing.RevealedToPlayer == Entry.RevealedToPlayer)
		{
			Existing = Entry;
			return;
		}
	}
	EnemyReveals.Add(Entry);
}

void AProjectCompGameState::Server_PurgeExpiredReveals()
{
	if (!HasAuthority() || EnemyReveals.Num() == 0)
	{
		return;
	}
	const float Now = GetServerWorldTimeSeconds();
	EnemyReveals.RemoveAll([Now](const FEnemyRevealEntry& E)
	{
		return !E.Target || E.ExpireServerTime <= Now;
	});
}

void AProjectCompGameState::Server_ClearEnemyRevealsForRound()
{
	if (!ensure(HasAuthority())) return;
	EnemyReveals.Reset();
}

// =====================================================================
// Multicast — runs on server + all clients to play VFX/SFX uniformly
// =====================================================================

void AProjectCompGameState::Multicast_OnPlayerKilled_Implementation(const FString& KillerName, const FString& VictimName, ETeamID KillerTeam, ETeamID VictimTeam)
{
	FKillFeedData& Event = PendingKillFeedEvents.AddDefaulted_GetRef();
	Event.KillerName = KillerName;
	Event.VictimName = VictimName;
	Event.KillerTeam = KillerTeam;
	Event.VictimTeam = VictimTeam;
}

void AProjectCompGameState::Multicast_OnAbilityCast_Implementation(EAbilityType Type, FVector TargetLocation, ETeamID Team)
{
	// Hook for UI/VFX/audio. Effects are local-only; never alter authoritative state here.
	UE_LOG(LogProjectComp, Log, TEXT("Ability cast: Type=%d Team=%d Loc=%s"),
		static_cast<int32>(Type), static_cast<int32>(Team), *TargetLocation.ToCompactString());
}

void AProjectCompGameState::Multicast_OnInfoScan_Implementation(int32 EnemyCount, ETeamID PlacerTeam, FVector ScanLocation, float ScanRadius)
{
	// Hook for the HUD widget. Two consumer paths:
	//  - Local player on PlacerTeam: render the count briefly.
	//  - Local player whose pawn is within ScanRadius of ScanLocation AND on
	//    the opposing team: render the "scanned" tell.
	// Both are HUD responsibilities — this multicast just delivers the data
	// so we can wire them once the widget exists.
	UE_LOG(LogProjectComp, Log,
		TEXT("[InfoScan] EnemyCount=%d PlacerTeam=%d Loc=%s Radius=%.0f"),
		EnemyCount, static_cast<int32>(PlacerTeam), *ScanLocation.ToCompactString(), ScanRadius);
}

// =====================================================================
// RepNotify handlers
// =====================================================================

void AProjectCompGameState::OnRep_CurrentPhase()
{
	// Hook for client-side phase-change reactions (UI panels, music cues, etc.).
}

// =====================================================================
// Internal helpers
// =====================================================================

FTeamLoadout* AProjectCompGameState::GetTeamLoadoutMutable(ETeamID Team)
{
	const int32 Index = static_cast<int32>(Team);
	if (Index < 0 || Index >= Teams.Num())
	{
		return nullptr;
	}
	return &Teams[Index];
}

const FTeamLoadout* AProjectCompGameState::GetTeamLoadoutConst(ETeamID Team) const
{
	const int32 Index = static_cast<int32>(Team);
	if (Index < 0 || Index >= Teams.Num())
	{
		return nullptr;
	}
	return &Teams[Index];
}
