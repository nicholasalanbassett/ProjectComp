// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompGameMode.h"
#include "ProjectCompGameState.h"
#include "ProjectCompPlayerState.h"
#include "ProjectCompPlayerController.h"
#include "ProjectCompPlayerStart.h"
#include "ProjectCompBarrier.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompHealthComponent.h"
#include "ProjectCompHUD.h"
#include "ProjectComp.h"
#include "Components/CapsuleComponent.h"
#include "EngineUtils.h"
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
	HUDClass = AProjectCompHUD::StaticClass();
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
			case ERoundPhase::RoundActive:
				// Round timeout with no plant resolution = defenders win the round (1 pt).
				// When the bomb mechanic lands, this branch will check bomb state first.
				GS->Server_AddTeamScore(ETeamID::Defenders, 1);
				UE_LOG(LogProjectComp, Log, TEXT("Round %d: Timer expired with no plant. Defenders +1 (now %d)."),
					GS->GetCurrentRound(),
					GS->GetTeamLoadout(ETeamID::Defenders).Score);
				StartPostRound();
				break;
			case ERoundPhase::PostRound:
			{
				// First-to-RoundsToWinMatch ends the match. Otherwise advance,
				// performing the half-time team swap on the round boundary if
				// the round we just finished matches HalftimeRoundNumber.
				const int32 AttackerScore = GS->GetTeamLoadout(ETeamID::Attackers).Score;
				const int32 DefenderScore = GS->GetTeamLoadout(ETeamID::Defenders).Score;
				if (AttackerScore >= RoundsToWinMatch || DefenderScore >= RoundsToWinMatch)
				{
					EndMatch();
				}
				else
				{
					if (HalftimeRoundNumber > 0 && GS->GetCurrentRound() == HalftimeRoundNumber)
					{
						Server_SwapAllPlayerTeams();
					}
					GS->Server_SetRound(GS->GetCurrentRound() + 1);
					StartPreRound();
				}
				break;
			}
			default: break;
		}
	}
}

void AProjectCompGameMode::PostLogin(APlayerController* NewPlayer)
{
	AProjectCompPlayerState* PS = NewPlayer ? NewPlayer->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (PS)
	{
		PS->Server_SetTeamID(PickTeamForNewPlayer());
		PS->Server_SetIsAlive(true);
		PS->Server_ClearVotes();
	}

	Super::PostLogin(NewPlayer);

	// Engine's Login() runs FindPlayerStart with TeamID=Spectator (random spawn).
	// Now that the team is assigned, relocate to a team-correct start. The
	// helper caches the assignment so subsequent mid-round respawns reuse it.
	if (PS && PS->GetTeamID() != ETeamID::Spectator)
	{
		RelocatePlayerToTeamStart(NewPlayer);
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
		AssignedStarts.Remove(PC);
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
		UE_LOG(LogProjectComp, Verbose, TEXT("Cast rejected: caster is alive (Loom mechanic requires death)."));
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

	// Clear all player votes; reset alive state + lives for the new round.
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (APlayerController* PC = It->Get())
		{
			if (AProjectCompPlayerState* PS = PC->GetPlayerState<AProjectCompPlayerState>())
			{
				PS->Server_ClearVotes();
				PS->Server_SetIsAlive(true);
				PS->Server_ResetLives();
				PS->Server_SetRespawnEndTime(0.0f);
			}
		}
	}

	// Fresh round = fresh spawn allocation. Clear sticky assignments so the
	// next ChoosePlayerStart for each player picks a new (teammate-avoiding)
	// random start; mid-round respawns then reuse that pick. Destroy +
	// respawn every player's pawn so anyone who was in a frozen-corpse state
	// from last round (died, didn't respawn before round ended) gets a fresh
	// fully-functional pawn — otherwise input/collision/movement stay disabled.
	AssignedStarts.Empty();
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (APlayerController* PC = It->Get())
		{
			RespawnPlayerAtAssignedStart(PC);
		}
	}

	// Raise spawn-room barriers — players are locked in until the round goes active.
	Server_SetAllBarriersActive(true);

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

	// Drop barriers so players can leave the spawn rooms.
	Server_SetAllBarriersActive(false);

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

AActor* AProjectCompGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	APlayerController* PC = Cast<APlayerController>(Player);
	const AProjectCompPlayerState* PS = Player ? Player->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	const ETeamID WantedTeam = PS ? PS->GetTeamID() : ETeamID::Spectator;

	// If this player already has a sticky assignment for the round, use it.
	// Keeps mid-round respawns landing at the same spot per player.
	if (PC)
	{
		if (TObjectPtr<AActor>* Existing = AssignedStarts.Find(PC))
		{
			if (Existing->Get())
			{
				return Existing->Get();
			}
		}
	}

	// Collect team-matching candidates.
	TArray<AProjectCompPlayerStart*> Candidates;
	for (TActorIterator<AProjectCompPlayerStart> It(GetWorld()); It; ++It)
	{
		if (It->GetTeamID() == WantedTeam)
		{
			Candidates.Add(*It);
		}
	}

	if (Candidates.Num() == 0)
	{
		// Fallback: no team-tagged start matched (common during Login() before the
		// team is assigned — PostLogin then relocates the pawn). Use any PlayerStart.
		// Don't cache this — we want the real team-correct start when team is set.
		return Super::ChoosePlayerStart_Implementation(Player);
	}

	// Prefer starts not already assigned to a teammate. Avoids two players
	// spawning at the same PlayerStart simultaneously on round start.
	TSet<AActor*> AlreadyAssigned;
	for (const auto& Pair : AssignedStarts)
	{
		if (Pair.Key != PC && Pair.Value)
		{
			AlreadyAssigned.Add(Pair.Value.Get());
		}
	}

	TArray<AProjectCompPlayerStart*> FreeCandidates;
	for (AProjectCompPlayerStart* C : Candidates)
	{
		if (!AlreadyAssigned.Contains(C))
		{
			FreeCandidates.Add(C);
		}
	}

	AProjectCompPlayerStart* Chosen = nullptr;
	if (FreeCandidates.Num() > 0)
	{
		Chosen = FreeCandidates[FMath::RandRange(0, FreeCandidates.Num() - 1)];
	}
	else
	{
		// All team-starts are claimed (more teammates than starts). Pick any.
		Chosen = Candidates[FMath::RandRange(0, Candidates.Num() - 1)];
	}

	// Cache the assignment so mid-round respawns land here too.
	if (PC && Chosen && WantedTeam != ETeamID::Spectator)
	{
		AssignedStarts.Add(PC, Chosen);
	}
	return Chosen;
}

void AProjectCompGameMode::RelocatePlayerToTeamStart(APlayerController* PC)
{
	if (!PC)
	{
		return;
	}
	AActor* TeamStart = ChoosePlayerStart(PC);
	if (!TeamStart)
	{
		return;
	}
	APawn* Pawn = PC->GetPawn();
	if (!Pawn)
	{
		return;
	}

	Pawn->SetActorLocation(TeamStart->GetActorLocation(),
		/*bSweep*/ false, /*OutSweepHitResult*/ nullptr,
		ETeleportType::TeleportPhysics);

	// Yaw-only — passing a full FRotator confuses the camera setup (sideways
	// view). Apply to both pawn and controller so the body and view agree
	// even if SetControlRotation gets clobbered by a post-possession step.
	const FRotator YawOnly(0.0f, TeamStart->GetActorRotation().Yaw, 0.0f);
	Pawn->SetActorRotation(YawOnly);
	PC->SetControlRotation(YawOnly);
}

void AProjectCompGameMode::RespawnPlayerAtAssignedStart(APlayerController* PC)
{
	if (!HasAuthority() || !PC)
	{
		return;
	}

	// Destroy existing pawn (frozen-corpse or otherwise) so we start fresh.
	// Without this the pawn keeps its disabled input/collision/movement from
	// HandlePlayerDeath, and the player can't move next round.
	if (APawn* OldPawn = PC->GetPawn())
	{
		OldPawn->Destroy();
	}

	// Engine's RestartPlayer creates a fresh pawn and possesses it.
	RestartPlayer(PC);

	// Force to team-correct assigned start (RestartPlayer's spawn location
	// is unreliable in UE 5.7 — caches StartSpot from Login).
	RelocatePlayerToTeamStart(PC);

	// Fresh health on the new pawn.
	if (AProjectCompCharacter* NewChar = Cast<AProjectCompCharacter>(PC->GetPawn()))
	{
		if (UProjectCompHealthComponent* HC = NewChar->GetHealthComponent())
		{
			HC->Server_ResetHealth();
		}
	}
}

void AProjectCompGameMode::Server_SwapAllPlayerTeams()
{
	int32 SwappedCount = 0;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		AProjectCompPlayerState* PS = PC ? PC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
		if (!PS)
		{
			continue;
		}

		const ETeamID Current = PS->GetTeamID();
		if (Current == ETeamID::Attackers)
		{
			PS->Server_SetTeamID(ETeamID::Defenders);
			++SwappedCount;
		}
		else if (Current == ETeamID::Defenders)
		{
			PS->Server_SetTeamID(ETeamID::Attackers);
			++SwappedCount;
		}
	}

	// Flip barrier ownership too so the see-through rule keeps working for
	// whichever team now spawns in each room.
	for (TActorIterator<AProjectCompBarrier> It(GetWorld()); It; ++It)
	{
		It->Server_SwapOwnerTeam();
	}

	UE_LOG(LogProjectComp, Log, TEXT("Half-time swap: %d players changed sides."), SwappedCount);
}

void AProjectCompGameMode::Server_SetAllBarriersActive(bool bActive)
{
	for (TActorIterator<AProjectCompBarrier> It(GetWorld()); It; ++It)
	{
		It->Server_SetActive(bActive);
	}
}

void AProjectCompGameMode::CheckRoundEndCondition()
{
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS || GS->GetCurrentPhase() != ERoundPhase::RoundActive)
	{
		return;
	}

	int32 AttackerTotal = 0, AttackerPermaGhost = 0;
	int32 DefenderTotal = 0, DefenderPermaGhost = 0;

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PC = It->Get();
		const AProjectCompPlayerState* PS = PC ? PC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
		if (!PS) continue;

		const ETeamID Team = PS->GetTeamID();
		if (Team == ETeamID::Attackers)
		{
			++AttackerTotal;
			if (PS->IsPermanentlyDead()) ++AttackerPermaGhost;
		}
		else if (Team == ETeamID::Defenders)
		{
			++DefenderTotal;
			if (PS->IsPermanentlyDead()) ++DefenderPermaGhost;
		}
	}

	// Elimination (no bomb mechanic yet, so any full-team-out is a 1-point elim).
	// When the bomb system lands, plant state takes precedence over elimination scoring.
	if (AttackerTotal > 0 && AttackerPermaGhost == AttackerTotal)
	{
		GS->Server_AddTeamScore(ETeamID::Defenders, 1);
		UE_LOG(LogProjectComp, Log, TEXT("Round %d: Attackers eliminated. Defenders +1 (now %d)."),
			GS->GetCurrentRound(),
			GS->GetTeamLoadout(ETeamID::Defenders).Score);
		StartPostRound();
	}
	else if (DefenderTotal > 0 && DefenderPermaGhost == DefenderTotal)
	{
		GS->Server_AddTeamScore(ETeamID::Attackers, 1);
		UE_LOG(LogProjectComp, Log, TEXT("Round %d: Defenders eliminated. Attackers +1 (now %d)."),
			GS->GetCurrentRound(),
			GS->GetTeamLoadout(ETeamID::Attackers).Score);
		StartPostRound();
	}
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
		PS->Server_DecrementLife();
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

	// Schedule respawn only if lives remain. On the final death, player stays
	// in permanent ghost mode for the rest of the round.
	const bool bHasLivesLeft = PS && PS->GetLivesRemaining() > 0;
	if (bHasLivesLeft && RespawnDelay > 0.0f)
	{
		// Stamp the absolute server-time the respawn will happen at, so clients
		// can render an accurate countdown without timer drift.
		const float ServerTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		PS->Server_SetRespawnEndTime(ServerTime + RespawnDelay);

		FTimerHandle& Handle = RespawnTimers.FindOrAdd(Victim);
		GetWorldTimerManager().SetTimer(Handle, [this, Victim]()
		{
			HandlePlayerRespawn(Victim);
		}, RespawnDelay, false);
	}
	else if (PS)
	{
		PS->Server_SetRespawnEndTime(0.0f);
	}

	// Check if this death triggers a round-end (full team permanent-ghost).
	CheckRoundEndCondition();
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

	if (AProjectCompPlayerState* PS = Player->GetPlayerState<AProjectCompPlayerState>())
	{
		PS->Server_SetIsAlive(true);
		PS->Server_SetRespawnEndTime(0.0f);
	}

	// Destroy + respawn + relocate + reset health, all in one helper.
	RespawnPlayerAtAssignedStart(Player);

	RespawnTimers.Remove(Player);
}

ETeamID AProjectCompGameMode::PickTeamForNewPlayer()
{
	// Deterministic alternation via our own counter: 1st join → Attackers,
	// 2nd → Defenders, 3rd → Attackers, etc. We use a private counter rather
	// than GetNumPlayers() or NumPlayers because in PIE listen-server mode the
	// host's PlayerController is sometimes not yet in the world iterator when
	// the client's PostLogin fires, which made the engine-based count race.
	// When real team-select UI lands this becomes irrelevant — players choose.
	++JoinSequence;
	return (JoinSequence % 2 == 1) ? ETeamID::Attackers : ETeamID::Defenders;
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
