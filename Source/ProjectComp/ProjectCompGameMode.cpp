// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompGameMode.h"
#include "ProjectCompGameState.h"
#include "ProjectCompPlayerState.h"
#include "ProjectCompPlayerController.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompHealthComponent.h"
#include "ProjectComp.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"
#include "Engine/World.h"

AProjectCompGameMode::AProjectCompGameMode()
{
	// GameMode owns the phase timer countdown + cooldown recharge.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	// Use our subclasses by default. The BP can override.
	GameStateClass = AProjectCompGameState::StaticClass();
	PlayerStateClass = AProjectCompPlayerState::StaticClass();
	PlayerControllerClass = AProjectCompPlayerController::StaticClass();
}

void AProjectCompGameMode::StartPlay()
{
	Super::StartPlay();

	// Begin the match at round 1 with a PreRound phase.
	if (AProjectCompGameState* GS = GetTypedGameState())
	{
		GS->Server_SetRound(1);
	}
	StartPreRound();
}

void AProjectCompGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS)
	{
		return;
	}

	// Countdown active phase.
	const float Remaining = GS->GetPhaseTimeRemaining() - DeltaSeconds;
	GS->Server_SetPhaseTimeRemaining(FMath::Max(Remaining, 0.0f));

	// Recharge ability pools only while gunplay is live.
	if (GS->GetCurrentPhase() == ERoundPhase::RoundActive)
	{
		TickCooldownPools(DeltaSeconds);
	}

	// Auto-transition when timer hits zero.
	if (Remaining <= 0.0f)
	{
		switch (GS->GetCurrentPhase())
		{
			case ERoundPhase::PreRound:    StartRoundActive(); break;
			case ERoundPhase::RoundActive: StartPostRound();   break;
			case ERoundPhase::PostRound:
				if (GS->GetCurrentRound() >= RoundsPerMatch) { EndMatch(); }
				else { GS->Server_SetRound(GS->GetCurrentRound() + 1); StartPreRound(); }
				break;
			default: break;
		}
	}
}

void AProjectCompGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);

	if (AProjectCompPlayerState* PS = NewPlayer ? NewPlayer->GetPlayerState<AProjectCompPlayerState>() : nullptr)
	{
		PS->Server_SetTeamID(PickTeamForNewPlayer());
		PS->Server_SetIsAlive(true);
		PS->Server_ClearVotes();
	}
}

void AProjectCompGameMode::Logout(AController* Exiting)
{
	if (APlayerController* PC = Cast<APlayerController>(Exiting))
	{
		if (FTimerHandle* Handle = RespawnTimers.Find(PC))
		{
			GetWorldTimerManager().ClearTimer(*Handle);
		}
		RespawnTimers.Remove(PC);
	}
	Super::Logout(Exiting);
}

// =====================================================================
// RPC entry points
// =====================================================================

bool AProjectCompGameMode::Server_SubmitAbilityVote(APlayerController* Voter, EAbilityType First, EAbilityType Second)
{
	if (!HasAuthority())
	{
		return false;
	}

	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS)
	{
		return false;
	}

	// Phase gate: votes only matter during PreRound.
	if (GS->GetCurrentPhase() != ERoundPhase::PreRound)
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("Vote rejected: not in PreRound."));
		return false;
	}

	if (!IsValidVotePair(First, Second))
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("Vote rejected: invalid ability pair."));
		return false;
	}

	AProjectCompPlayerState* PS = Voter ? Voter->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (!PS || PS->GetTeamID() == ETeamID::Spectator)
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("Vote rejected: no valid team."));
		return false;
	}

	PS->Server_SetVotes(First, Second);
	return true;
}

bool AProjectCompGameMode::Server_RequestAbilityCast(APlayerController* Caster, EAbilityType Type, FVector TargetLocation)
{
	if (!HasAuthority())
	{
		return false;
	}

	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS)
	{
		return false;
	}

	if (GS->GetCurrentPhase() != ERoundPhase::RoundActive)
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("Cast rejected: round not active."));
		return false;
	}

	AProjectCompPlayerState* PS = Caster ? Caster->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (!PS)
	{
		return false;
	}

	if (PS->IsAlive())
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("Cast rejected: caster is alive (Ghost Commander mechanic requires death)."));
		return false;
	}

	const ETeamID Team = PS->GetTeamID();
	if (Team == ETeamID::Spectator)
	{
		return false;
	}

	const FTeamLoadout Loadout = GS->GetTeamLoadout(Team);
	if (Type != Loadout.AbilitySlot1 && Type != Loadout.AbilitySlot2)
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("Cast rejected: ability not drafted by team."));
		return false;
	}

	if (Loadout.SharedCooldownPool < 1.0f)
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("Cast rejected: pool not full (%.2f)."), Loadout.SharedCooldownPool);
		return false;
	}

	// All checks passed: deduct pool, fire effect multicast.
	GS->Server_SetTeamCooldownPool(Team, 0.0f);
	GS->Multicast_OnAbilityCast(Type, TargetLocation, Team);
	return true;
}

// =====================================================================
// Phase transitions
// =====================================================================

void AProjectCompGameMode::StartPreRound()
{
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS)
	{
		return;
	}

	GS->Server_SetPhase(ERoundPhase::PreRound);
	SetPhaseTimer(PreRoundDuration);

	// Reset per-team round state: clear locked picks, refill pool, clear votes.
	for (uint8 i = 0; i < 2; ++i)
	{
		FTeamLoadout Loadout = GS->GetTeamLoadout(static_cast<ETeamID>(i));
		Loadout.AbilitySlot1 = EAbilityType::None;
		Loadout.AbilitySlot2 = EAbilityType::None;
		Loadout.SharedCooldownPool = 1.0f;
		GS->Server_SetTeamLoadout(static_cast<ETeamID>(i), Loadout);
	}

	// Clear all player votes; reset alive state for the new round.
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (APlayerController* PC = It->Get())
		{
			if (AProjectCompPlayerState* PS = PC->GetPlayerState<AProjectCompPlayerState>())
			{
				PS->Server_ClearVotes();
				PS->Server_SetIsAlive(true);
			}
		}
	}

	UE_LOG(LogProjectComp, Log, TEXT("Round %d: PreRound started (%.1fs)."), GS->GetCurrentRound(), PreRoundDuration);
}

void AProjectCompGameMode::StartRoundActive()
{
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS)
	{
		return;
	}

	// Tally votes -> lock team loadouts.
	TallyAndLockTeamDraft(ETeamID::Attackers);
	TallyAndLockTeamDraft(ETeamID::Defenders);

	GS->Server_SetPhase(ERoundPhase::RoundActive);
	SetPhaseTimer(RoundActiveDuration);

	UE_LOG(LogProjectComp, Log, TEXT("Round %d: RoundActive started (%.1fs)."), GS->GetCurrentRound(), RoundActiveDuration);
}

void AProjectCompGameMode::StartPostRound()
{
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS)
	{
		return;
	}

	GS->Server_SetPhase(ERoundPhase::PostRound);
	SetPhaseTimer(PostRoundDuration);

	// Clear pending respawn timers — round is over.
	for (auto& Pair : RespawnTimers)
	{
		GetWorldTimerManager().ClearTimer(Pair.Value);
	}
	RespawnTimers.Empty();

	UE_LOG(LogProjectComp, Log, TEXT("Round %d: PostRound started (%.1fs)."), GS->GetCurrentRound(), PostRoundDuration);
}

void AProjectCompGameMode::EndMatch()
{
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS)
	{
		return;
	}

	GS->Server_SetPhase(ERoundPhase::MatchEnded);
	GS->Server_SetPhaseTimeRemaining(0.0f);
	UE_LOG(LogProjectComp, Log, TEXT("Match ended after %d rounds."), GS->GetCurrentRound());
}

// =====================================================================
// Draft tallying
// =====================================================================

void AProjectCompGameMode::TallyAndLockTeamDraft(ETeamID Team)
{
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS || Team == ETeamID::Spectator)
	{
		return;
	}

	// Count votes for each ability among players on this team. Each player
	// contributes +1 to their first pick and +1 to their second pick.
	TMap<EAbilityType, int32> Tally;
	int32 PlayerCount = 0;

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		AProjectCompPlayerState* PS = PC ? PC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
		if (!PS || PS->GetTeamID() != Team)
		{
			continue;
		}
		++PlayerCount;

		const EAbilityType V1 = PS->GetVotedAbility1();
		const EAbilityType V2 = PS->GetVotedAbility2();
		if (V1 != EAbilityType::None) { Tally.FindOrAdd(V1)++; }
		if (V2 != EAbilityType::None) { Tally.FindOrAdd(V2)++; }
	}

	// Sort by vote count descending. Deterministic tie-break: lower enum value
	// wins (alphabetical: AA < AC < AM < ...) so server replays produce the
	// same result given the same inputs.
	TArray<TPair<EAbilityType, int32>> Sorted;
	for (const TPair<EAbilityType, int32>& Pair : Tally)
	{
		Sorted.Add(Pair);
	}
	Sorted.Sort([](const TPair<EAbilityType, int32>& A, const TPair<EAbilityType, int32>& B)
	{
		if (A.Value != B.Value) return A.Value > B.Value;
		return static_cast<uint8>(A.Key) < static_cast<uint8>(B.Key);
	});

	// Pick the top two distinct entries. If voting was empty (e.g. nobody on
	// the team), AbilitySlot1/2 remain None and the team has no abilities
	// this round.
	FTeamLoadout Loadout = GS->GetTeamLoadout(Team);
	Loadout.AbilitySlot1 = Sorted.Num() > 0 ? Sorted[0].Key : EAbilityType::None;
	Loadout.AbilitySlot2 = Sorted.Num() > 1 ? Sorted[1].Key : EAbilityType::None;
	GS->Server_SetTeamLoadout(Team, Loadout);

	UE_LOG(LogProjectComp, Log, TEXT("Team %d draft locked: %d, %d (from %d voters)."),
		static_cast<int32>(Team),
		static_cast<int32>(Loadout.AbilitySlot1),
		static_cast<int32>(Loadout.AbilitySlot2),
		PlayerCount);
}

// =====================================================================
// Cooldown recharge + phase timing
// =====================================================================

void AProjectCompGameMode::TickCooldownPools(float DeltaSeconds)
{
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS || CooldownRechargeRate <= 0.0f)
	{
		return;
	}

	const float Delta = CooldownRechargeRate * DeltaSeconds;
	for (uint8 i = 0; i < 2; ++i)
	{
		const ETeamID Team = static_cast<ETeamID>(i);
		const FTeamLoadout Loadout = GS->GetTeamLoadout(Team);
		if (Loadout.SharedCooldownPool < 1.0f)
		{
			GS->Server_SetTeamCooldownPool(Team, Loadout.SharedCooldownPool + Delta);
		}
	}
}

void AProjectCompGameMode::SetPhaseTimer(float Duration)
{
	if (AProjectCompGameState* GS = GetTypedGameState())
	{
		GS->Server_SetPhaseTimeRemaining(Duration);
	}
}

// =====================================================================
// Player lifecycle
// =====================================================================

void AProjectCompGameMode::HandlePlayerDeath(APlayerController* Victim, AController* Killer)
{
	if (!HasAuthority() || !Victim)
	{
		return;
	}

	AProjectCompPlayerState* PS = Victim->GetPlayerState<AProjectCompPlayerState>();
	if (PS)
	{
		PS->Server_SetIsAlive(false);
		PS->Server_AddDeath();
	}

	if (AProjectCompPlayerState* KillerPS = Killer ? Killer->GetPlayerState<AProjectCompPlayerState>() : nullptr)
	{
		KillerPS->Server_AddKill();
	}

	// Freeze the dead pawn until respawn destroys it: no input, no collision,
	// no movement. The pawn remains visible so the killer can see the corpse.
	if (APawn* DeadPawn = Victim->GetPawn())
	{
		DeadPawn->DisableInput(Victim);
		if (UCapsuleComponent* Capsule = DeadPawn->FindComponentByClass<UCapsuleComponent>())
		{
			Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		if (UCharacterMovementComponent* CMC = DeadPawn->FindComponentByClass<UCharacterMovementComponent>())
		{
			CMC->DisableMovement();
		}
	}

	// Schedule respawn. Only fires if we're still in RoundActive when the timer elapses.
	if (RespawnDelay > 0.0f)
	{
		FTimerHandle& Handle = RespawnTimers.FindOrAdd(Victim);
		GetWorldTimerManager().SetTimer(Handle, [this, Victim]()
		{
			HandlePlayerRespawn(Victim);
		}, RespawnDelay, false);
	}
}

void AProjectCompGameMode::HandlePlayerRespawn(APlayerController* Player)
{
	if (!HasAuthority() || !Player)
	{
		return;
	}

	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS || GS->GetCurrentPhase() != ERoundPhase::RoundActive)
	{
		// Don't respawn outside of an active round; PostRound/PreRound handle their own reset.
		return;
	}

	// Destroy the frozen-corpse pawn before spawning a fresh one at a PlayerStart.
	if (APawn* OldPawn = Player->GetPawn())
	{
		OldPawn->Destroy();
	}

	if (AProjectCompPlayerState* PS = Player->GetPlayerState<AProjectCompPlayerState>())
	{
		PS->Server_SetIsAlive(true);
	}

	// Re-use the engine's RestartPlayer flow (SpawnDefaultPawnFor + Possess).
	RestartPlayer(Player);

	// Reset health on the freshly-spawned pawn.
	if (AProjectCompCharacter* NewChar = Cast<AProjectCompCharacter>(Player->GetPawn()))
	{
		if (UProjectCompHealthComponent* HC = NewChar->GetHealthComponent())
		{
			HC->Server_ResetHealth();
		}
	}

	RespawnTimers.Remove(Player);
}

ETeamID AProjectCompGameMode::PickTeamForNewPlayer() const
{
	int32 AttackerCount = 0;
	int32 DefenderCount = 0;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PC = It->Get();
		const AProjectCompPlayerState* PS = PC ? PC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
		if (!PS) continue;
		if (PS->GetTeamID() == ETeamID::Attackers) ++AttackerCount;
		else if (PS->GetTeamID() == ETeamID::Defenders) ++DefenderCount;
	}
	return AttackerCount <= DefenderCount ? ETeamID::Attackers : ETeamID::Defenders;
}

// =====================================================================
// Helpers
// =====================================================================

AProjectCompGameState* AProjectCompGameMode::GetTypedGameState() const
{
	return GetGameState<AProjectCompGameState>();
}

bool AProjectCompGameMode::IsValidVotePair(EAbilityType First, EAbilityType Second)
{
	return First != EAbilityType::None
		&& Second != EAbilityType::None
		&& First != Second;
}
