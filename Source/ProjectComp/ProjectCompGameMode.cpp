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
#include "ProjectCompHitscanWeapon.h"
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

	// Cap one-time allocation for the live-character registry. 5v5 fills 10
	// slots; 16 leaves headroom for late-join, AI, or 10v10 stress without
	// triggering a TArray growth-and-copy inside the Server_RegisterLiveCharacter
	// path. Characters register from their server-side BeginPlay.
	LiveCharacters.Reserve(16);

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
	//
	// Iterating the event-driven LiveCharacters registry instead of TActorIterator
	// turns an O(WorldActors) scan into an O(LivePawns) walk over a hot, sequential
	// TArray — cache-friendly and free of any allocator activity.
	if (UWorld* World = GetWorld())
	{
		SCOPE_CYCLE_COUNTER(STAT_ProjectComp_LagCompSnapshot);
		const float Now = World->GetTimeSeconds();
		for (const TWeakObjectPtr<AProjectCompCharacter>& WeakChar : LiveCharacters)
		{
			if (AProjectCompCharacter* Char = WeakChar.Get())
			{
				Char->Server_RecordTransformSnapshot(Now);
			}
		}
	}

	// Countdown active phase.
	const float Remaining = GS->GetPhaseTimeRemaining() - DeltaSeconds;
	GS->Server_SetPhaseTimeRemaining(FMath::Max(Remaining, 0.0f));

	// Close the selection window once SelectionWindowDuration has elapsed within
	// PreRound. Cheap test — single bool flip — runs at most once per round.
	if (GS->GetCurrentPhase() == ERoundPhase::PreRound && GS->IsSelectionWindowOpen())
	{
		const float Elapsed = PreRoundDuration - Remaining;
		if (Elapsed >= SelectionWindowDuration)
		{
			GS->Server_SetSelectionWindowOpen(false);
			UE_LOG(LogProjectComp, Log, TEXT("PreRound: selection window closed at %.1fs (PreRound %.1fs total)."),
				SelectionWindowDuration, PreRoundDuration);
		}
	}

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
				AwardRoundEndCredits(ETeamID::Defenders);
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
		// Bootstrap the new player's wallet so their first PreRound buy window
		// has something to spend. Mid-match joiners get the same baseline —
		// graduated catch-up bonuses can be layered on later if needed.
		PS->Server_SetCredits(StartingCredits);
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

void AProjectCompGameMode::Server_RegisterLiveCharacter(AProjectCompCharacter* Char)
{
	// HasAuthority() check is the caller's responsibility — registry only exists
	// on the server. AddUnique guards against the EndPlay/BeginPlay edge case
	// where a respawn pipeline could double-fire registration; the cost is
	// O(N) but N is capped at the player count, so it's still nanoseconds.
	if (Char)
	{
		LiveCharacters.AddUnique(TWeakObjectPtr<AProjectCompCharacter>(Char));
	}
}

void AProjectCompGameMode::Server_UnregisterLiveCharacter(AProjectCompCharacter* Char)
{
	// RemoveSingleSwap is O(N) but doesn't preserve order (which we don't need)
	// and avoids the O(N) shift that RemoveSingle would do. Skipping the equals
	// operator path by passing a TWeakObjectPtr constructed in place.
	if (Char)
	{
		LiveCharacters.RemoveSingleSwap(TWeakObjectPtr<AProjectCompCharacter>(Char), EAllowShrinking::No);
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

	// All checks passed: dispatch per-ability gameplay effect, then deduct the
	// pool + fire the cosmetic multicast IF the effect actually spawned. The
	// deduct used to happen before the switch, which let a misconfigured class
	// (e.g. unset FlashDomeClass with a null CDO, or a spawn-collision reject)
	// drain the pool without producing a dome.
	//
	// Inline switch is intentional — we only have one implemented ability so
	// far. Refactor to an IAbilityHandler dispatch table when the second one
	// lands; abstracting now would over-fit to a single example.
	UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Cast ACCEPTED: type=%d team=%d target=%s. Dispatching..."),
		static_cast<int32>(Type), static_cast<int32>(Team), *TargetLocation.ToCompactString());

	bool bSpawnSucceeded = false;

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
		// grazes or hits the floor mesh before reaching the endpoint. Read the
		// per-class lift off the CDO so each BP variant can tune independently.
		const AProjectCompFlashDome* DomeCDO = SpawnClass
			? SpawnClass->GetDefaultObject<AProjectCompFlashDome>()
			: nullptr;
		const float DomeZLift = DomeCDO ? DomeCDO->SpawnZLift : 150.0f;
		const FVector DomeSpawn = TargetLocation + FVector(0.0f, 0.0f, DomeZLift);

		AProjectCompFlashDome* Dome = GetWorld()->SpawnActor<AProjectCompFlashDome>(
				SpawnClass, DomeSpawn, FRotator::ZeroRotator, SpawnParams);
		if (Dome)
		{
			Dome->Server_Initialize(Team);
			bSpawnSucceeded = true;
			UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] FlashDome spawned at %s (target=%s + lift) for team %d."),
				*DomeSpawn.ToCompactString(), *TargetLocation.ToCompactString(), static_cast<int32>(Team));
		}
		else
		{
			UE_LOG(LogProjectComp, Warning, TEXT("[Loom] FlashDome SpawnActor returned null — pool not deducted."));
		}
		break;
	}
	default:
		UE_LOG(LogProjectComp, Warning, TEXT("[Loom] Cast type=%d has no gameplay handler yet — pool not deducted."),
			static_cast<int32>(Type));
		break;
	}

	if (!bSpawnSucceeded)
	{
		return false;
	}

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
	// Also reset the new per-player Loom + grenade state (per grenade-system memory):
	// Loom claim cleared so this round's draft can run fresh, Loom usage flag
	// reset so each player gets their 1-per-round charge back, grenade loadout
	// cleared so credits-spent-last-round don't carry items into this one.
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
				PS->Server_ClearChosenLoomAbility();
				PS->Server_ResetLoomUsage();
				PS->Server_ClearEquippedGrenades();
			}
		}
	}

	// Open the selection window for the first SelectionWindowDuration seconds of
	// this PreRound. Tick() will flip it closed when elapsed crosses the threshold.
	GS->Server_SetSelectionWindowOpen(true);

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

			// Refill ammo to full on the freshly-spawned weapon. The respawn
			// already creates a new weapon (which BeginPlay-inits to full),
			// so this is belt-and-braces — keeps the intent visible and
			// survives any future change that swaps "destroy+respawn" for
			// "teleport existing pawn".
			if (AProjectCompCharacter* Char = Cast<AProjectCompCharacter>(PC->GetPawn()))
			{
				if (AProjectCompHitscanWeapon* Weapon = Char->GetCurrentWeapon())
				{
					Weapon->Server_RefillAllAmmo();
				}
			}
		}
	}

	// Engage each barrier per its Kind's PreRound rule.
	Server_SetAllBarriersForPhase(ERoundPhase::PreRound);

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

	// Defensive: selection window is gated by elapsed time in Tick, but a low
	// PreRoundDuration or future timing change could leave it open. Force-close.
	GS->Server_SetSelectionWindowOpen(false);

	GS->Server_SetPhase(ERoundPhase::RoundActive);
	SetPhaseTimer(RoundActiveDuration);

	// Re-route barriers for the RoundActive phase — AttackerSpawnLock drops to
	// DeadPawn-only mode, DefenderSpawnLock engages (DeadPawn-only + visible),
	// DefenderPreRoundLock goes fully off.
	Server_SetAllBarriersForPhase(ERoundPhase::RoundActive);

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

	// PostRound barrier state matches RoundActive for AttackerSpawnLock and
	// DefenderSpawnLock (looming-safe zones still active); DefenderPreRoundLock
	// stays off. Explicit call so the routing is obvious here.
	Server_SetAllBarriersForPhase(ERoundPhase::PostRound);

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

	// Barriers are role-permanent under the new EBarrierKind scheme — OwnerTeam
	// stays put; the player who is now TeamID=Defenders this half sees the
	// translucent material on every DefenderSpawnLock / DefenderPreRoundLock
	// automatically because the OwnerTeam comparison in RefreshAppearance
	// matches their new TeamID. Same for attackers. No swap call needed.

	// Swap team loadouts so per-team state (Score in particular) follows the
	// players to their new side. Without this, the "Attackers" Score remains
	// owned by the role label after the swap — meaning the team that earned
	// those points is now Defenders, and any subsequent attacker-side wins
	// land in the opponent's totals. AbilitySlot1/2 and SharedCooldownPool
	// get reset in StartPreRound right after, so only Score truly needs to
	// move, but we swap the whole struct so future per-team fields are
	// handled the same way without needing to revisit this function.
	if (AProjectCompGameState* GS = GetTypedGameState())
	{
		const FTeamLoadout AttackerLoadout = GS->GetTeamLoadout(ETeamID::Attackers);
		const FTeamLoadout DefenderLoadout = GS->GetTeamLoadout(ETeamID::Defenders);
		GS->Server_SetTeamLoadout(ETeamID::Attackers, DefenderLoadout);
		GS->Server_SetTeamLoadout(ETeamID::Defenders, AttackerLoadout);
		UE_LOG(LogProjectComp, Log, TEXT("Half-time swap: scores follow players — Attackers now %d, Defenders now %d."),
			GS->GetTeamLoadout(ETeamID::Attackers).Score,
			GS->GetTeamLoadout(ETeamID::Defenders).Score);
	}

	UE_LOG(LogProjectComp, Log, TEXT("Half-time swap: %d players changed sides."), SwappedCount);
}

void AProjectCompGameMode::Server_SetAllBarriersForPhase(ERoundPhase NewPhase)
{
	// Per-Kind activation table — see EBarrierKind doc for the rationale:
	//
	//                          PreRound    RoundActive    PostRound
	//   AttackerSpawnLock      true        false          false
	//   DefenderSpawnLock      false       true           true
	//   DefenderPreRoundLock   true        false          false
	//
	// "Active" means different things per Kind (block-all vs DeadPawn-only vs
	// fully off) — the bool is just an engagement flag the barrier's
	// RefreshAppearance reads alongside Kind to pick the right collision +
	// visibility setup.
	for (TActorIterator<AProjectCompBarrier> It(GetWorld()); It; ++It)
	{
		AProjectCompBarrier* B = *It;
		if (!B)
		{
			continue;
		}
		bool bShouldBeActive = false;
		switch (B->Kind)
		{
			case EBarrierKind::AttackerSpawnLock:
				bShouldBeActive = (NewPhase == ERoundPhase::PreRound);
				break;
			case EBarrierKind::DefenderSpawnLock:
				bShouldBeActive = (NewPhase == ERoundPhase::RoundActive
				                || NewPhase == ERoundPhase::PostRound);
				break;
			case EBarrierKind::DefenderPreRoundLock:
				bShouldBeActive = (NewPhase == ERoundPhase::PreRound);
				break;
		}
		B->Server_SetActive(bShouldBeActive);
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

	ETeamID WinningTeam = ETeamID::Spectator;
	if (Site->bIsDetonated)
	{
		GS->Server_AddTeamScore(ETeamID::Attackers, Site->PointValue);
		WinningTeam = ETeamID::Attackers;
		UE_LOG(LogProjectComp, Log, TEXT("Round %d: Bomb DETONATED at %s. Attackers +%d (now %d)."),
			GS->GetCurrentRound(), *Site->Label.ToString(), Site->PointValue,
			GS->GetTeamLoadout(ETeamID::Attackers).Score);
	}
	else if (Site->bIsDefused)
	{
		GS->Server_AddTeamScore(ETeamID::Defenders, Site->DefusePointValue);
		WinningTeam = ETeamID::Defenders;
		UE_LOG(LogProjectComp, Log, TEXT("Round %d: Bomb DEFUSED at %s. Defenders +%d (now %d)."),
			GS->GetCurrentRound(), *Site->Label.ToString(), Site->DefusePointValue,
			GS->GetTeamLoadout(ETeamID::Defenders).Score);
	}

	AwardRoundEndCredits(WinningTeam);
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
					AwardRoundEndCredits(ETeamID::Attackers);
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
		AwardRoundEndCredits(ETeamID::Defenders);
		StartPostRound();
	}
	else if (bAllDefendersDead)
	{
		GS->Server_AddTeamScore(ETeamID::Attackers, 1);
		UE_LOG(LogProjectComp, Log, TEXT("Round %d: Defenders eliminated. Attackers +1 (now %d)."),
			GS->GetCurrentRound(),
			GS->GetTeamLoadout(ETeamID::Attackers).Score);
		AwardRoundEndCredits(ETeamID::Attackers);
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
		KillerPS->Server_AddCredits(KillCreditBonus);
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

	AProjectCompPlayerState* PS = Player->GetPlayerState<AProjectCompPlayerState>();
	if (PS)
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

// =====================================================================
// Economy + grenade purchase + Loom claim arbitration
// =====================================================================

int32 AProjectCompGameMode::GetGrenadeCost(EGrenadeType Type) const
{
	switch (Type)
	{
	case EGrenadeType::Flash:      return FlashGrenadeCost;
	case EGrenadeType::Info:       return InfoGrenadeCost;
	case EGrenadeType::Vulnerable: return VulnerableGrenadeCost;
	case EGrenadeType::Sound:      return SoundGrenadeCost;
	default:                       return 0;
	}
}

void AProjectCompGameMode::Server_CheatEndRound(ETeamID WinningTeam)
{
	if (!HasAuthority())
	{
		return;
	}

	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS)
	{
		return;
	}

	if (GS->GetCurrentPhase() != ERoundPhase::RoundActive)
	{
		UE_LOG(LogProjectComp, Warning,
			TEXT("[Cheat] EndRound rejected: phase=%d (must be RoundActive)."),
			static_cast<int32>(GS->GetCurrentPhase()));
		return;
	}

	if (WinningTeam != ETeamID::Attackers && WinningTeam != ETeamID::Defenders)
	{
		UE_LOG(LogProjectComp, Warning, TEXT("[Cheat] EndRound rejected: invalid team %d."),
			static_cast<int32>(WinningTeam));
		return;
	}

	GS->Server_AddTeamScore(WinningTeam, 1);
	UE_LOG(LogProjectComp, Log,
		TEXT("[Cheat] Round %d force-ended. Team %d +1 (now %d)."),
		GS->GetCurrentRound(), static_cast<int32>(WinningTeam),
		GS->GetTeamLoadout(WinningTeam).Score);
	AwardRoundEndCredits(WinningTeam);
	StartPostRound();
}

void AProjectCompGameMode::AwardRoundEndCredits(ETeamID WinningTeam)
{
	if (!HasAuthority() || WinningTeam == ETeamID::Spectator)
	{
		return;
	}

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		AProjectCompPlayerState* PS = PC ? PC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
		if (!PS)
		{
			continue;
		}
		const ETeamID Team = PS->GetTeamID();
		if (Team == ETeamID::Spectator)
		{
			continue;
		}
		PS->Server_AddCredits(Team == WinningTeam ? RoundWinCreditBonus : RoundLossCreditBonus);
	}

	UE_LOG(LogProjectComp, Log,
		TEXT("Round %d: credits awarded — winners +%d, losers +%d (winning team=%d)."),
		GetTypedGameState() ? GetTypedGameState()->GetCurrentRound() : -1,
		RoundWinCreditBonus, RoundLossCreditBonus, static_cast<int32>(WinningTeam));
}

bool AProjectCompGameMode::Server_ClaimLoomAbility(APlayerController* Claimer, EAbilityType Ability)
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

	// Phase + selection-window gate. Claims only land while the buy UI is open.
	if (GS->GetCurrentPhase() != ERoundPhase::PreRound || !GS->IsSelectionWindowOpen())
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("[Loom claim] Rejected: phase=%d window=%d."),
			static_cast<int32>(GS->GetCurrentPhase()), GS->IsSelectionWindowOpen() ? 1 : 0);
		return false;
	}

	if (Ability == EAbilityType::None)
	{
		return false;
	}

	AProjectCompPlayerState* ClaimerPS = Claimer ? Claimer->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (!ClaimerPS)
	{
		return false;
	}

	const ETeamID Team = ClaimerPS->GetTeamID();
	if (Team == ETeamID::Spectator)
	{
		return false;
	}

	// First-claim-wins arbitration: reject if any teammate already holds this
	// ability for the round. The claimer themselves is excluded so re-claiming
	// the same slot is a no-op success rather than a hard reject.
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* OtherPC = It->Get();
		if (OtherPC == Claimer)
		{
			continue;
		}
		AProjectCompPlayerState* OtherPS = OtherPC ? OtherPC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
		if (OtherPS && OtherPS->GetTeamID() == Team && OtherPS->GetChosenLoomAbility() == Ability)
		{
			UE_LOG(LogProjectComp, Verbose,
				TEXT("[Loom claim] Rejected: ability %d already held by teammate."),
				static_cast<int32>(Ability));
			return false;
		}
	}

	ClaimerPS->Server_SetChosenLoomAbility(Ability);
	UE_LOG(LogProjectComp, Log, TEXT("[Loom claim] Team %d player '%s' claimed ability %d."),
		static_cast<int32>(Team), *ClaimerPS->GetPlayerName(), static_cast<int32>(Ability));
	return true;
}

bool AProjectCompGameMode::Server_BuyGrenade(APlayerController* Buyer, EGrenadeType Type)
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

	if (GS->GetCurrentPhase() != ERoundPhase::PreRound || !GS->IsSelectionWindowOpen())
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("[Grenade buy] Rejected: phase=%d window=%d."),
			static_cast<int32>(GS->GetCurrentPhase()), GS->IsSelectionWindowOpen() ? 1 : 0);
		return false;
	}

	if (Type == EGrenadeType::None)
	{
		return false;
	}

	AProjectCompPlayerState* BuyerPS = Buyer ? Buyer->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (!BuyerPS || BuyerPS->GetTeamID() == ETeamID::Spectator)
	{
		return false;
	}

	// Up-front gating so we can mutate atomically below without rollback paths.
	if (BuyerPS->HasGrenadeEquipped(Type))
	{
		return false;  // no same-type stacking
	}

	constexpr int32 MaxEquippedGrenadeTypes = 2;
	if (BuyerPS->GetEquippedGrenadeTypes().Num() >= MaxEquippedGrenadeTypes)
	{
		return false;  // kit full
	}

	// Per-team holder cap: at most MaxHoldersPerTypePerTeam players on the same
	// team can equip the same grenade type per round. Forces team-composition
	// diversity during the selection-window draft race — late-claimers may be
	// rejected on popular types and have to fall back to alternatives. Math:
	// 5 players × 2 picks = 10 desired, but cap × 4 types = 8 max → expected
	// 2-pick team shortage per round (deliberate; see grenade-system memory).
	constexpr int32 MaxHoldersPerTypePerTeam = 2;
	const ETeamID BuyerTeam = BuyerPS->GetTeamID();
	int32 TeamHolders = 0;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* OtherPC = It->Get();
		if (!OtherPC || OtherPC == Buyer)
		{
			continue;
		}
		const AProjectCompPlayerState* OtherPS = OtherPC->GetPlayerState<AProjectCompPlayerState>();
		if (!OtherPS || OtherPS->GetTeamID() != BuyerTeam)
		{
			continue;
		}
		if (OtherPS->HasGrenadeEquipped(Type))
		{
			++TeamHolders;
			if (TeamHolders >= MaxHoldersPerTypePerTeam)
			{
				UE_LOG(LogProjectComp, Verbose,
					TEXT("[Grenade buy] Rejected: team %d already at cap (%d) for type %d."),
					static_cast<int32>(BuyerTeam), MaxHoldersPerTypePerTeam, static_cast<int32>(Type));
				return false;
			}
		}
	}

	const int32 Cost = GetGrenadeCost(Type);
	if (Cost <= 0)
	{
		return false;
	}

	if (BuyerPS->GetCurrentCredits() < Cost)
	{
		return false;
	}

	// All gates passed: spend + equip. Each helper still runs its own asserts;
	// neither can fail here given the checks above.
	BuyerPS->Server_SpendCredits(Cost);
	BuyerPS->Server_EquipGrenade(Type);

	UE_LOG(LogProjectComp, Log,
		TEXT("[Grenade buy] '%s' bought grenade %d for %d credits (remaining %d)."),
		*BuyerPS->GetPlayerName(), static_cast<int32>(Type), Cost, BuyerPS->GetCurrentCredits());
	return true;
}
