// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompGameMode.h"
#include "ProjectCompGameState.h"
#include "ProjectCompPlayerState.h"
#include "ProjectCompPlayerController.h"
#include "ProjectCompPlayerStart.h"
#include "ProjectCompBarrier.h"
#include "ProjectCompBombSite.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompFlashDome.h"
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

	// Build the bomb-site cache once; level-placed sites don't change at runtime.
	CachedBombSites.Reset();
	for (TActorIterator<AProjectCompBombSite> It(GetWorld()); It; ++It)
	{
		CachedBombSites.Add(*It);
	}

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

	// Lag-compensation: push a transform snapshot onto every character's ring buffer
	// once per server tick. The ring depth (~500ms) covers worst-case competitive
	// RTT plus interp delay. Cheap — one struct copy per character per tick.
	if (UWorld* World = GetWorld())
	{
		const float Now = World->GetTimeSeconds();
		for (TActorIterator<AProjectCompCharacter> It(World); It; ++It)
		{
			if (AProjectCompCharacter* Char = *It)
			{
				Char->Server_RecordTransformSnapshot(Now);
			}
		}
	}

	// Countdown active phase.
	const float Remaining = GS->GetPhaseTimeRemaining() - DeltaSeconds;
	GS->Server_SetPhaseTimeRemaining(FMath::Max(Remaining, 0.0f));

	// Recharge ability pools only while gunplay is live.
	if (GS->GetCurrentPhase() == ERoundPhase::RoundActive)
	{
		// Bomb-resolution check runs every tick — if a bomb hit detonated/defused
		// since last frame, route the score and end the round immediately.
		if (AProjectCompBombSite* TerminalSite = GetTerminalBombSite())
		{
			HandleBombResolution(TerminalSite);
			return;  // round just ended — skip the rest of the tick
		}

		TickCooldownPools(DeltaSeconds);
		EnforceLoomingPlayerBounds();
	}

	// Auto-transition when timer hits zero.
	if (Remaining <= 0.0f)
	{
		switch (GS->GetCurrentPhase())
		{
			case ERoundPhase::PreRound:    StartRoundActive(); break;
			case ERoundPhase::RoundActive:
				if (IsAnyBombPlanted())
				{
					// Bomb planted but not yet resolved — round continues past
					// the regular timer until detonate/defuse fires.
					break;
				}
				// Round timeout with no plant = defenders win (1 pt).
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
		UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Cast rejected: round not active (phase=%d)."),
			static_cast<int32>(GS->GetCurrentPhase()));
		return false;
	}

	AProjectCompPlayerState* PS = Caster ? Caster->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (!PS)
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Cast rejected: no PlayerState."));
		return false;
	}

	if (PS->IsAlive())
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Cast rejected: caster is alive (Loom requires looming/dead)."));
		return false;
	}

	const ETeamID Team = PS->GetTeamID();
	if (Team == ETeamID::Spectator)
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Cast rejected: caster on Spectator team."));
		return false;
	}

	const FTeamLoadout Loadout = GS->GetTeamLoadout(Team);
	if (Type != Loadout.AbilitySlot1 && Type != Loadout.AbilitySlot2)
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Cast rejected: ability %d not drafted by team %d (slots: %d, %d)."),
			static_cast<int32>(Type), static_cast<int32>(Team),
			static_cast<int32>(Loadout.AbilitySlot1), static_cast<int32>(Loadout.AbilitySlot2));
		return false;
	}

	if (Loadout.SharedCooldownPool < 1.0f)
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Cast rejected: pool not full (%.2f)."), Loadout.SharedCooldownPool);
		return false;
	}

	// All checks passed: deduct pool, dispatch per-ability gameplay effect, then
	// fire the cosmetic multicast so clients can hook VFX/audio off it.
	GS->Server_SetTeamCooldownPool(Team, 0.0f);

	// Inline switch is intentional — we only have one implemented ability so
	// far. Refactor to an IAbilityHandler dispatch table when the second one
	// lands; abstracting now would over-fit to a single example.
	UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Cast ACCEPTED: type=%d team=%d target=%s. Dispatching..."),
		static_cast<int32>(Type), static_cast<int32>(Team), *TargetLocation.ToCompactString());

	switch (Type)
	{
	case EAbilityType::CA:
	{
		const TSubclassOf<AProjectCompFlashDome> SpawnClass =
			FlashDomeClass ? FlashDomeClass : TSubclassOf<AProjectCompFlashDome>(AProjectCompFlashDome::StaticClass());

		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = Caster;
		SpawnParams.Instigator = Caster->GetPawn();
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		// Lift the dome above the trace impact point. Floor-level dome centers
		// fail LOS because the trace from camera (~Z=160) down to dome (Z=0)
		// grazes or hits the floor mesh before reaching the endpoint. 150cm
		// lands the center near standing eye-level (capsule half-height 96 +
		// camera Z 64 = 160), so a standing enemy facing the dome's general
		// area has the center naturally inside their crosshair.
		// TODO(loom-tuning): promote to a UPROPERTY on the dome class in a
		// future restart-pass so designers can tune per-ability.
		constexpr float kDomeSpawnZLift = 150.0f;
		const FVector DomeSpawn = TargetLocation + FVector(0.0f, 0.0f, kDomeSpawnZLift);

		AProjectCompFlashDome* Dome = GetWorld()->SpawnActor<AProjectCompFlashDome>(
				SpawnClass, DomeSpawn, FRotator::ZeroRotator, SpawnParams);
		if (Dome)
		{
			Dome->Server_Initialize(Team);
			UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] FlashDome spawned at %s (target=%s + lift) for team %d."),
				*DomeSpawn.ToCompactString(), *TargetLocation.ToCompactString(), static_cast<int32>(Team));
		}
		else
		{
			UE_LOG(LogProjectComp, Warning, TEXT("[Loom] FlashDome SpawnActor returned null."));
		}
		break;
	}
	default:
		UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Cast type=%d has no gameplay handler yet (cosmetic multicast only)."),
			static_cast<int32>(Type));
		break;
	}

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

	// Reset bomb sites: clear plant/defuse/detonate state from last round.
	Server_ResetAllBombSites();

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

	// Server-side SetControlRotation doesn't propagate to the owning client's
	// autonomous proxy — the client owns its ControlRotation locally and only
	// sends it up in saved moves. For the listen-server host this doesn't
	// matter (the server IS the local player), but a remote client (P2+) would
	// keep its stale yaw and spawn facing the wrong way. ClientSetRotation is
	// a reliable Client RPC that explicitly pushes the new rotation down so
	// the next saved move comes back with the corrected value.
	PC->ClientSetRotation(YawOnly, /*bResetCamera*/ true);
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

bool AProjectCompGameMode::IsAnyBombPlanted() const
{
	for (const TWeakObjectPtr<AProjectCompBombSite>& WeakSite : CachedBombSites)
	{
		const AProjectCompBombSite* Site = WeakSite.Get();
		if (Site && Site->bIsPlanted && !Site->IsTerminal())
		{
			return true;
		}
	}
	return false;
}

AProjectCompBombSite* AProjectCompGameMode::GetTerminalBombSite() const
{
	for (const TWeakObjectPtr<AProjectCompBombSite>& WeakSite : CachedBombSites)
	{
		AProjectCompBombSite* Site = WeakSite.Get();
		if (Site && Site->IsTerminal())
		{
			return Site;
		}
	}
	return nullptr;
}

void AProjectCompGameMode::HandleBombResolution(AProjectCompBombSite* Site)
{
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS || !Site)
	{
		return;
	}

	if (Site->bIsDetonated)
	{
		GS->Server_AddTeamScore(ETeamID::Attackers, Site->PointValue);
		UE_LOG(LogProjectComp, Log, TEXT("Round %d: Bomb DETONATED at %s. Attackers +%d (now %d)."),
			GS->GetCurrentRound(), *Site->Label.ToString(), Site->PointValue,
			GS->GetTeamLoadout(ETeamID::Attackers).Score);
	}
	else if (Site->bIsDefused)
	{
		GS->Server_AddTeamScore(ETeamID::Defenders, Site->DefusePointValue);
		UE_LOG(LogProjectComp, Log, TEXT("Round %d: Bomb DEFUSED at %s. Defenders +%d (now %d)."),
			GS->GetCurrentRound(), *Site->Label.ToString(), Site->DefusePointValue,
			GS->GetTeamLoadout(ETeamID::Defenders).Score);
	}

	StartPostRound();
}

void AProjectCompGameMode::Server_ResetAllBombSites()
{
	for (const TWeakObjectPtr<AProjectCompBombSite>& WeakSite : CachedBombSites)
	{
		if (AProjectCompBombSite* Site = WeakSite.Get())
		{
			Site->Server_ResetForNewRound();
		}
	}
}

void AProjectCompGameMode::EnforceLoomingPlayerBounds()
{
	// Snap any dead player who has wandered out of their team's spawn room
	// back to their assigned PlayerStart. Map-specific Y-axis bounds — simple
	// for the prototype; replace with a level-placed volume system later.
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC)
		{
			continue;
		}

		const AProjectCompPlayerState* PS = PC->GetPlayerState<AProjectCompPlayerState>();
		if (!PS || PS->IsAlive())
		{
			continue;  // Alive players aren't bound to the spawn room.
		}

		APawn* Pawn = PC->GetPawn();
		if (!Pawn)
		{
			continue;
		}

		const float Y = Pawn->GetActorLocation().Y;
		bool bOutOfBounds = false;
		if (PS->GetTeamID() == ETeamID::Attackers && Y > AttackerSpawnRoomMaxY)
		{
			bOutOfBounds = true;
		}
		else if (PS->GetTeamID() == ETeamID::Defenders && Y < DefenderSpawnRoomMinY)
		{
			bOutOfBounds = true;
		}

		if (bOutOfBounds)
		{
			RelocatePlayerToTeamStart(PC);
		}
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

	const bool bAllAttackersDead = AttackerTotal > 0 && AttackerPermaGhost == AttackerTotal;
	const bool bAllDefendersDead = DefenderTotal > 0 && DefenderPermaGhost == DefenderTotal;

	if (IsAnyBombPlanted())
	{
		// Bomb planted: attackers win immediately if all defenders are eliminated
		// (nobody left to defuse — detonation is inevitable). Defenders killing all
		// attackers does NOT end the round — they must still defuse or lose to the timer.
		if (bAllDefendersDead)
		{
			// Score as if the bomb detonated — find the planted site for PointValue.
			for (const TWeakObjectPtr<AProjectCompBombSite>& WeakSite : CachedBombSites)
			{
				AProjectCompBombSite* Site = WeakSite.Get();
				if (Site && Site->bIsPlanted && !Site->IsTerminal())
				{
					GS->Server_AddTeamScore(ETeamID::Attackers, Site->PointValue);
					UE_LOG(LogProjectComp, Log,
						TEXT("Round %d: Defenders eliminated with bomb planted at %s. Attackers +%d (now %d)."),
						GS->GetCurrentRound(), *Site->Label.ToString(),
						Site->PointValue, GS->GetTeamLoadout(ETeamID::Attackers).Score);
					StartPostRound();
					return;
				}
			}
		}
		// All attackers dead but bomb still ticking — defenders must defuse; no round-end.
		return;
	}

	// No bomb planted: standard elimination — full team permanently dead = opponent wins 1 pt.
	if (bAllAttackersDead)
	{
		GS->Server_AddTeamScore(ETeamID::Defenders, 1);
		UE_LOG(LogProjectComp, Log, TEXT("Round %d: Attackers eliminated. Defenders +1 (now %d)."),
			GS->GetCurrentRound(),
			GS->GetTeamLoadout(ETeamID::Defenders).Score);
		StartPostRound();
	}
	else if (bAllDefendersDead)
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
	// the team voted), fall back to CA/AA so the Loom system has something to
	// test against until the draft UI lands.
	// TODO(loom-ui): change these fallbacks back to EAbilityType::None once the
	// PreRound draft widget is in place — the production behavior should be
	// "no votes = no abilities," not "no votes = free CA/AA."
	FTeamLoadout Loadout = GS->GetTeamLoadout(Team);
	Loadout.AbilitySlot1 = Sorted.Num() > 0 ? Sorted[0].Key : EAbilityType::CA;
	Loadout.AbilitySlot2 = Sorted.Num() > 1 ? Sorted[1].Key : EAbilityType::AA;
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

	// Looming-mobile: the dead player keeps input + movement so they can
	// walk around their spawn room during their loom window. We change the
	// pawn's collision so it doesn't block live players and bullets pass
	// through it (no double-tap kills on a corpse). Weapon firing is gated
	// separately in AProjectCompHitscanWeapon::FireOneShot. Setting the
	// object type to DeadPawn lets the (otherwise-passable-to-live-players)
	// spawn-room barrier block looming pawns so they can't leave the room.
	if (APawn* DeadPawn = Victim->GetPawn())
	{
		if (UCapsuleComponent* Capsule = DeadPawn->FindComponentByClass<UCapsuleComponent>())
		{
			Capsule->SetCollisionObjectType(ECC_DeadPawn);
			Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
			Capsule->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
		}
		if (USkeletalMeshComponent* BodyMesh = DeadPawn->FindComponentByClass<USkeletalMeshComponent>())
		{
			// Mesh becomes a visual-only prop — rendering still happens (so teammates
			// see the looming body), but it doesn't physically block live players
			// walking through, and bullets pass without hit-registering.
			BodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
	}

	// Teleport to the player's assigned spawn so they loom from their own room.
	RelocatePlayerToTeamStart(Victim);

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

	// Broadcast to all clients for kill feed display.
	if (AProjectCompGameState* GS = GetTypedGameState())
	{
		const AProjectCompPlayerState* KillerPS2 = Killer ? Killer->GetPlayerState<AProjectCompPlayerState>() : nullptr;
		const FString KillerName = KillerPS2 ? KillerPS2->GetPlayerName() : TEXT("??");
		const FString VictimName = PS ? PS->GetPlayerName() : TEXT("Unknown");
		const ETeamID KillerTeam = KillerPS2 ? KillerPS2->GetTeamID() : ETeamID::Spectator;
		const ETeamID VictimTeam = PS ? PS->GetTeamID() : ETeamID::Spectator;
		GS->Multicast_OnPlayerKilled(KillerName, VictimName, KillerTeam, VictimTeam);
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
