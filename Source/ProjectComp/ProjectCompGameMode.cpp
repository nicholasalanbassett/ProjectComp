// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompGameMode.h"
#include "ProjectCompGameState.h"
#include "ProjectCompPlayerState.h"
#include "ProjectCompPlayerController.h"
#include "ProjectCompPlayerStart.h"
#include "ProjectCompBarrier.h"
#include "ProjectCompBombSite.h"
#include "ProjectCompBomb.h"
#include "ProjectCompBombSpawn.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompAcousticZone.h"
#include "ProjectCompRevealZone.h"
#include "ProjectCompTitanPillar.h"
#include "ProjectCompInteractJammer.h"
#include "ProjectCompTunnelVisionZone.h"
#include "ProjectCompMinimapEmp.h"
#include "ProjectCompCameraSpike.h"
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

	// Spawn the plain C++ bomb by default; a BP subclass can override it.
	BombClass = AProjectCompBomb::StaticClass();
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

	// Find the level-placed bomb-spawn marker (first one) and spawn the single bomb.
	CachedBombSpawn = nullptr;
	for (TActorIterator<AProjectCompBombSpawn> It(GetWorld()); It; ++It)
	{
		CachedBombSpawn = *It;
		break;
	}
	if (!BombActor && BombClass)
	{
		const FVector SpawnLoc = CachedBombSpawn.IsValid() ? CachedBombSpawn->GetActorLocation() : FVector::ZeroVector;
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		BombActor = GetWorld()->SpawnActor<AProjectCompBomb>(BombClass, SpawnLoc, FRotator::ZeroRotator, SpawnParams);
		if (!CachedBombSpawn.IsValid())
		{
			UE_LOG(LogProjectComp, Warning, TEXT("[Bomb] No AProjectCompBombSpawn placed in the level — bomb spawned at world origin. Place a BombSpawn marker."));
		}
		if (AProjectCompGameState* GS = GetTypedGameState())
		{
			GS->Server_SetBomb(BombActor);
		}
	}

	// Cap one-time allocation for the live-character registry. 5v5 fills 10
	// slots; 16 leaves headroom for late-join, AI, or 10v10 stress without
	// triggering a TArray growth-and-copy inside the Server_RegisterLiveCharacter
	// path. Characters register from their server-side BeginPlay.
	LiveCharacters.Reserve(16);

	// Sit in the pre-match team-select lobby (None phase) until the host starts.
	// Round 1 is staged here; Server_StartMatch (host) kicks off the round loop.
	if (AProjectCompGameState* GS = GetTypedGameState())
	{
		GS->Server_SetRound(1);
		GS->Server_SetPhase(ERoundPhase::None);
		GS->Server_SetPhaseTimeRemaining(0.0f);
	}
	bMatchStarted = false;
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

	// Countdown active phase. The PauseTimer cheat freezes the decrement (the
	// clock holds at its current value) so a round can be held open for testing.
	float Remaining = GS->GetPhaseTimeRemaining();
	if (!bCheatFreezeTimer)
	{
		Remaining = FMath::Max(Remaining - DeltaSeconds, 0.0f);
		GS->Server_SetPhaseTimeRemaining(Remaining);
	}

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
		if (BombActor && BombActor->IsTerminal())
		{
			HandleBombResolution();
			return;  // round just ended — skip the rest of the tick
		}

		TickCooldownPools(DeltaSeconds);
		EnforceLoomingPlayerBounds();
		// Drop expired AM/MM enemy-reveal records so the minimap/HUD stop showing them.
		GS->Server_PurgeExpiredReveals();
	}

	// Auto-transition when timer hits zero.
	if (Remaining <= 0.0f)
	{
		switch (GS->GetCurrentPhase())
		{
			case ERoundPhase::LoomSelection: StartPreRound(); break;
			case ERoundPhase::PreRound:      StartRoundActive(); break;
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
					const int32 NextRound = GS->GetCurrentRound() + 1;
					GS->Server_SetRound(NextRound);
					// Route to LoomSelection on rotation rounds, PreRound otherwise.
					if (ShouldRunLoomSelectionForRound(NextRound))
					{
						StartLoomSelection();
					}
					else
					{
						StartPreRound();
					}
				}
				break;
			}
			case ERoundPhase::None:
					// Lobby idles with timer 0, so this runs every tick. On a dedicated
					// server, auto-start once enough players are present.
					if (IsDedicatedServerMode())
					{
						TickServerLobby();
					}
					break;
				case ERoundPhase::MatchEnded:
					// Results screen held for PostMatchDuration; on a dedicated server,
					// auto-return to the lobby when it elapses so matches loop unattended.
					if (IsDedicatedServerMode())
					{
						ResetToLobby();
					}
					break;
				default: break;
		}
	}
}

void AProjectCompGameMode::PostLogin(APlayerController* NewPlayer)
{
	AProjectCompPlayerState* PS = NewPlayer ? NewPlayer->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (PS)
	{
		PS->Server_SetTeamID(PickTeamForNewPlayer(PS));
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

	// First player to join becomes the lobby leader (transferable later). Not
	// tied to the host — whoever connects first leads until they hand it off.
	if (AProjectCompGameState* GS = GetTypedGameState())
	{
		if (PS && !GS->GetLobbyLeader())
		{
			GS->Server_SetLobbyLeader(PS);
			UE_LOG(LogProjectComp, Log, TEXT("[Lobby] '%s' is the lobby leader (first joiner)."), *PS->GetPlayerName());
		}
	}
}

void AProjectCompGameMode::Server_StartMatch(APlayerController* Requester)
{
	if (!HasAuthority() || bMatchStarted || !Requester)
	{
		return;
	}
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS || GS->GetCurrentPhase() != ERoundPhase::None)
	{
		return;  // only valid out of the pre-match lobby
	}
	// Only the lobby leader may start the match (not the host).
	const APlayerState* ReqPS = Requester->GetPlayerState<APlayerState>();
	if (!ReqPS || GS->GetLobbyLeader() != ReqPS)
	{
		UE_LOG(LogProjectComp, Warning, TEXT("[Lobby] StartMatch rejected — requester is not the lobby leader."));
		return;
	}
	UE_LOG(LogProjectComp, Log, TEXT("[Lobby] Lobby leader started the match."));
	BeginMatch();
}

void AProjectCompGameMode::BeginMatch()
{
	if (!HasAuthority() || bMatchStarted)
	{
		return;
	}
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS || GS->GetCurrentPhase() != ERoundPhase::None)
	{
		return;
	}
	bMatchStarted = true;
	// Round 1 always opens with LoomSelection per the rotation rule.
	if (ShouldRunLoomSelectionForRound(1))
	{
		StartLoomSelection();
	}
	else
	{
		StartPreRound();
	}
}

void AProjectCompGameMode::Server_SetLobbyLeader(APlayerController* Requester, APlayerState* Target)
{
	if (!HasAuthority() || !Requester || !Target)
	{
		return;
	}
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS || GS->GetCurrentPhase() != ERoundPhase::None)
	{
		return;  // leadership only changes in the lobby
	}
	// Only the CURRENT leader can hand off leadership.
	const APlayerState* ReqPS = Requester->GetPlayerState<APlayerState>();
	if (!ReqPS || GS->GetLobbyLeader() != ReqPS)
	{
		UE_LOG(LogProjectComp, Warning, TEXT("[Lobby] SetLobbyLeader rejected — requester is not the current leader."));
		return;
	}
	// Target must be a real player currently in the match.
	if (!GS->PlayerArray.Contains(Target))
	{
		return;
	}
	GS->Server_SetLobbyLeader(Target);
	UE_LOG(LogProjectComp, Log, TEXT("[Lobby] Leadership transferred to '%s'."), *Target->GetPlayerName());
}

void AProjectCompGameMode::Server_RequestTeamChange(APlayerController* PC, ETeamID NewTeam)
{
	if (!HasAuthority() || !PC)
	{
		return;
	}
	// Free swap, any size — but only while still in the lobby.
	const AProjectCompGameState* GS = GetTypedGameState();
	if (!GS || GS->GetCurrentPhase() != ERoundPhase::None)
	{
		return;
	}
	if (NewTeam != ETeamID::Attackers && NewTeam != ETeamID::Defenders)
	{
		return;
	}
	AProjectCompPlayerState* PS = PC->GetPlayerState<AProjectCompPlayerState>();
	if (!PS || PS->GetTeamID() == NewTeam)
	{
		return;
	}
	PS->Server_SetTeamID(NewTeam);
	// Drop the cached spawn so the player relocates to the new team's side.
	AssignedStarts.Remove(PC);
	RelocatePlayerToTeamStart(PC);
	UE_LOG(LogProjectComp, Log, TEXT("[Lobby] '%s' swapped to team %d."),
		*PS->GetPlayerName(), static_cast<int32>(NewTeam));
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
	APlayerState* LeavingPS = nullptr;
	if (APlayerController* PC = Cast<APlayerController>(Exiting))
	{
		LeavingPS = PC->GetPlayerState<APlayerState>();
		if (FTimerHandle* Handle = RespawnTimers.Find(PC))
		{
			GetWorldTimerManager().ClearTimer(*Handle);
		}
		RespawnTimers.Remove(PC);
		AssignedStarts.Remove(PC);
	}
	Super::Logout(Exiting);

	// If the lobby leader left, hand leadership to the first remaining player.
	if (AProjectCompGameState* GS = GetTypedGameState())
	{
		const APlayerState* Leader = GS->GetLobbyLeader();
		if (!Leader || Leader == LeavingPS)
		{
			APlayerState* NewLeader = nullptr;
			for (APlayerState* PS : GS->PlayerArray)
			{
				if (PS && PS != LeavingPS)
				{
					NewLeader = PS;
					break;
				}
			}
			GS->Server_SetLobbyLeader(NewLeader);
			if (NewLeader)
			{
				UE_LOG(LogProjectComp, Log, TEXT("[Lobby] Leader left; '%s' is now the lobby leader."), *NewLeader->GetPlayerName());
			}
		}
	}

	// Dedicated server: if everyone's gone, reset to a clean lobby for the next
	// joiners; otherwise check whether a team just emptied mid-match (forfeit).
	// LeavingPS may still be in PlayerArray at this point, so exclude it.
	if (IsDedicatedServerMode())
	{
		const AProjectCompGameState* GS = GetTypedGameState();
		int32 Remaining = 0;
		if (GS)
		{
			for (APlayerState* PS : GS->PlayerArray)
			{
				if (PS && PS != LeavingPS)
				{
					++Remaining;
				}
			}
		}
		if (Remaining <= 0)
		{
			ResetToLobby();
		}
		else
		{
			CheckForfeit(LeavingPS);
		}
	}
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

namespace
{
	/**
	 * Shared Loom-ability actor spawn. Resolves the configured class (or the
	 * native default), lifts the spawn by the class CDO's SpawnZLift, sets
	 * owner/instigator, spawns, and calls Server_Initialize(Team). Returns the
	 * spawned actor (null on failure). Every Loom ability actor exposes a public
	 * float SpawnZLift and void Server_Initialize(ETeamID), so one template covers
	 * all of them (zone subclasses + the standalone Titan Pillar).
	 */
	template <typename TAbility>
	TAbility* SpawnLoomAbility(UWorld* World, TSubclassOf<TAbility> ConfiguredClass,
		APlayerController* Caster, ETeamID Team, const FVector& TargetLocation)
	{
		if (!World)
		{
			return nullptr;
		}
		const TSubclassOf<TAbility> SpawnClass = ConfiguredClass
			? ConfiguredClass
			: TSubclassOf<TAbility>(TAbility::StaticClass());

		UClass* SpawnUClass = SpawnClass.Get();
		const TAbility* CDO = SpawnUClass ? Cast<TAbility>(SpawnUClass->GetDefaultObject()) : nullptr;
		const float ZLift = CDO ? CDO->SpawnZLift : 100.0f;

		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = Caster;
		SpawnParams.Instigator = Caster ? Caster->GetPawn() : nullptr;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		TAbility* Actor = World->SpawnActor<TAbility>(
			SpawnClass, TargetLocation + FVector(0.0f, 0.0f, ZLift), FRotator::ZeroRotator, SpawnParams);
		if (Actor)
		{
			Actor->Server_Initialize(Team);
		}
		return Actor;
	}
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

	// One Loom cast per round per player. The charge persists across this player's
	// respawn within the round (reset only at round start via Server_ResetLoomUsage),
	// so a player who spent it during their 1st-death looming window has none left
	// for their 2nd-death window, and vice-versa — exactly one of the two.
	if (PS->HasUsedLoomThisRound())
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Cast rejected: player already used their one Loom charge this round."));
		return false;
	}

	// All checks passed: dispatch per-ability gameplay effect, then deduct the
	// pool + fire the cosmetic multicast IF the effect actually spawned. The
	// deduct used to happen before the switch, which let a misconfigured class
	// (e.g. unset FlashDomeClass with a null CDO, or a spawn-collision reject)
	// drain the pool without producing a dome.
	//
	// Inline switch dispatches per-ability via the shared SpawnLoomAbility<>()
	// helper. CA + AC are standby/TBD (fall through to default, not consumed);
	// every in-rotation slot maps to its spawned ability actor.
	UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Cast ACCEPTED: type=%d team=%d target=%s. Dispatching..."),
		static_cast<int32>(Type), static_cast<int32>(Team), *TargetLocation.ToCompactString());

	bool bSpawnSucceeded = false;

	switch (Type)
	{
	case EAbilityType::AA:  // Acoustic Deafening
		bSpawnSucceeded = SpawnLoomAbility<AProjectCompAcousticZone>(
			GetWorld(), AcousticZoneClass, Caster, Team, TargetLocation) != nullptr;
		break;
	case EAbilityType::AM:  // Utility Reveal
		bSpawnSucceeded = SpawnLoomAbility<AProjectCompRevealZone>(
			GetWorld(), RevealZoneClass, Caster, Team, TargetLocation) != nullptr;
		break;
	case EAbilityType::CC:  // Titan Pillar
		bSpawnSucceeded = SpawnLoomAbility<AProjectCompTitanPillar>(
			GetWorld(), TitanPillarClass, Caster, Team, TargetLocation) != nullptr;
		break;
	case EAbilityType::CM:  // Interact Jammer
		bSpawnSucceeded = SpawnLoomAbility<AProjectCompInteractJammer>(
			GetWorld(), InteractJammerClass, Caster, Team, TargetLocation) != nullptr;
		break;
	case EAbilityType::MA:  // Tunnel Vision
		bSpawnSucceeded = SpawnLoomAbility<AProjectCompTunnelVisionZone>(
			GetWorld(), TunnelVisionZoneClass, Caster, Team, TargetLocation) != nullptr;
		break;
	case EAbilityType::MC:  // Minimap EMP (re-homed from the benched SoundDome)
		bSpawnSucceeded = SpawnLoomAbility<AProjectCompMinimapEmp>(
			GetWorld(), MinimapEmpClass, Caster, Team, TargetLocation) != nullptr;
		break;
	case EAbilityType::MM:  // Camera Spike
		bSpawnSucceeded = SpawnLoomAbility<AProjectCompCameraSpike>(
			GetWorld(), CameraSpikeClass, Caster, Team, TargetLocation) != nullptr;
		break;
	default:
		// AC + CA are standby/TBD (no handler); None is invalid. Charge not consumed.
		UE_LOG(LogProjectComp, Warning, TEXT("[Loom] Cast type=%d has no gameplay handler (AC/CA are standby) — not consumed."),
			static_cast<int32>(Type));
		break;
	}

	if (!bSpawnSucceeded)
	{
		return false;
	}

	// Consume this player's single per-round Loom charge (the authoritative gate
	// now — the team shared-cooldown pool is no longer used to limit casts).
	PS->Server_MarkLoomUsed();
	GS->Multicast_OnAbilityCast(Type, TargetLocation, Team);
	return true;
}

// =====================================================================
// Phase transitions
// =====================================================================

bool AProjectCompGameMode::ShouldRunLoomSelectionForRound(int32 Round) const
{
	if (Round < 1)
	{
		return false;
	}
	// Halftime guarantee: first round of half 2 always opens with LoomSelection
	// so post-swap players get a fresh pick window even if the rotation math
	// wouldn't naturally place a selection there. For the default 12-round-
	// half config this coincides with the rotation rule anyway, but it's
	// explicit safety if HalftimeRoundNumber is later retuned.
	if (HalftimeRoundNumber > 0 && Round == HalftimeRoundNumber + 1)
	{
		return true;
	}
	// Runs at the start of the match (round 1) and every LoomSelectionRoundInterval
	// rounds thereafter (default 6 → rounds 1, 7, 13, 19...).
	return (Round - 1) % FMath::Max(1, LoomSelectionRoundInterval) == 0;
}

void AProjectCompGameMode::PrepareForNewRound()
{
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS)
	{
		return;
	}

	// Idempotency guard. Both StartLoomSelection and StartPreRound call this
	// helper, but only the FIRST phase of each round should actually prep —
	// we don't want a second call to destroy + respawn pawns mid-flow.
	const int32 ThisRound = GS->GetCurrentRound();
	if (LastPreparedRound == ThisRound)
	{
		return;
	}
	LastPreparedRound = ThisRound;

	// Reset per-team round state (deprecated AbilitySlot/Pool kept until the
	// Loom cast-path migration deletes them, but reset for back-compat).
	for (uint8 i = 0; i < 2; ++i)
	{
		FTeamLoadout Loadout = GS->GetTeamLoadout(static_cast<ETeamID>(i));
		Loadout.AbilitySlot1 = EAbilityType::None;
		Loadout.AbilitySlot2 = EAbilityType::None;
		Loadout.SharedCooldownPool = 1.0f;
		GS->Server_SetTeamLoadout(static_cast<ETeamID>(i), Loadout);
	}

	// Per-player resets. NOTE: ChosenLoomAbility is deliberately NOT cleared
	// here — it persists across the rounds between LoomSelection phases (the
	// LoomSelectionRoundInterval window), and only StartLoomSelection clears it.
	// The Loom-usage charge (1 cast per round) DOES reset every round regardless.
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
				PS->Server_ResetLoomUsage();
				PS->Server_ClearEquippedGrenades();
				// Weapons + armour reset each round — players re-buy in PreRound.
				// Future change (carry-weapons-on-survive CS pattern) goes here.
				PS->Server_ClearLoadout();
			}
		}
	}

	// Clear last round's litter: destroy any weapons dropped on the ground (death
	// drops + manual G-drops). Equipped weapons have an owner and are skipped.
	Server_CleanupGroundWeapons();

	// Fresh round = fresh spawn allocation. Destroy + respawn every player's
	// pawn so anyone in a frozen-corpse state from last round gets a fresh
	// fully-functional pawn back.
	AssignedStarts.Empty();
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (APlayerController* PC = It->Get())
		{
			RespawnPlayerAtAssignedStart(PC);
			if (AProjectCompCharacter* Char = Cast<AProjectCompCharacter>(PC->GetPawn()))
			{
				if (AProjectCompHitscanWeapon* Weapon = Char->GetCurrentWeapon())
				{
					Weapon->Server_RefillAllAmmo();
				}
			}
		}
	}

	// Reset bomb sites for the new round.
	Server_ResetAllBombSites();

	// Clear any lingering AM/MM enemy-reveal records from last round (GS validated above).
	GS->Server_ClearEnemyRevealsForRound();
}

void AProjectCompGameMode::StartLoomSelection()
{
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS)
	{
		return;
	}

	// LoomSelection is always the FIRST phase of any round it runs in, so
	// per-round prep (player respawn, bomb reset, etc.) happens here.
	PrepareForNewRound();

	// Wipe the per-player Loom claim so this LoomSelection's draft is fresh.
	// (PrepareForNewRound deliberately leaves ChosenLoomAbility alone so that
	// non-LoomSelection rounds preserve the drafted pick until the next selection.)
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (APlayerController* PC = It->Get())
		{
			if (AProjectCompPlayerState* PS = PC->GetPlayerState<AProjectCompPlayerState>())
			{
				PS->Server_ClearChosenLoomAbility();
			}
		}
	}

	GS->Server_SetPhase(ERoundPhase::LoomSelection);
	SetPhaseTimer(LoomSelectionDuration);
	// Selection window is meaningless during LoomSelection (the gate is the
	// phase itself); make sure it's closed so a stale flag from last round
	// doesn't accidentally re-open a buy panel.
	GS->Server_SetSelectionWindowOpen(false);
	// Barriers behave the same as PreRound — players locked in spawn.
	Server_SetAllBarriersForPhase(ERoundPhase::LoomSelection);

	UE_LOG(LogProjectComp, Log,
		TEXT("Round %d: LoomSelection started (%.1fs)."),
		GS->GetCurrentRound(), LoomSelectionDuration);
}

void AProjectCompGameMode::StartPreRound()
{
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS)
	{
		return;
	}

	// If LoomSelection didn't run for this round, this is the first phase
	// and we still need the per-round prep. PrepareForNewRound is idempotent
	// — if LoomSelection already ran it, this call no-ops.
	PrepareForNewRound();

	GS->Server_SetPhase(ERoundPhase::PreRound);
	SetPhaseTimer(PreRoundDuration);

	// Open the selection window for the first SelectionWindowDuration seconds
	// of this PreRound. Tick() flips it closed when elapsed crosses the threshold.
	GS->Server_SetSelectionWindowOpen(true);

	// Engage each barrier per its Kind's PreRound rule.
	Server_SetAllBarriersForPhase(ERoundPhase::PreRound);

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

void AProjectCompGameMode::EndMatch(ETeamID ForcedWinner)
{
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS)
	{
		return;
	}

	// Normal end fires when a team reaches RoundsToWinMatch, so the higher score is
	// the winner (a true tie is impossible under first-to-N). A forfeit (team
	// emptied) passes ForcedWinner to override the score.
	const int32 AttackerScore = GS->GetTeamLoadout(ETeamID::Attackers).Score;
	const int32 DefenderScore = GS->GetTeamLoadout(ETeamID::Defenders).Score;
	const ETeamID Winner = (ForcedWinner != ETeamID::Spectator)
		? ForcedWinner
		: ((AttackerScore >= DefenderScore) ? ETeamID::Attackers : ETeamID::Defenders);
	GS->Server_SetMatchWinner(Winner);

	GS->Server_SetPhase(ERoundPhase::MatchEnded);
	// Hold the results screen for PostMatchDuration. On a dedicated server the Tick
	// auto-returns to lobby when this expires; listen/PIE waits for the leader's button.
	GS->Server_SetPhaseTimeRemaining(PostMatchDuration);
	UE_LOG(LogProjectComp, Log, TEXT("Match ended after %d rounds — %s win (%d-%d)%s."),
		GS->GetCurrentRound(),
		Winner == ETeamID::Attackers ? TEXT("Attackers") : TEXT("Defenders"),
		AttackerScore, DefenderScore,
		ForcedWinner != ETeamID::Spectator ? TEXT(" [forfeit]") : TEXT(""));
}

// Leader-gated entry from the results-screen button. Validates the requester is
// the lobby leader and we're on the results screen, then runs the shared reset.
void AProjectCompGameMode::Server_ReturnToLobby(APlayerController* Requester)
{
	if (!HasAuthority() || !Requester)
	{
		return;
	}
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS || GS->GetCurrentPhase() != ERoundPhase::MatchEnded)
	{
		return;  // only valid from the results screen
	}
	const APlayerState* ReqPS = Requester->GetPlayerState<APlayerState>();
	if (!ReqPS || GS->GetLobbyLeader() != ReqPS)
	{
		UE_LOG(LogProjectComp, Warning, TEXT("[Lobby] ReturnToLobby rejected — requester is not the lobby leader."));
		return;
	}
	UE_LOG(LogProjectComp, Log, TEXT("[Lobby] Lobby leader returned the match to the lobby."));
	ResetToLobby();
}

// Shared reset back to the pre-match team-select lobby (None phase). Mirrors
// StartPlay's lobby staging + the per-round reset in PrepareForNewRound, but also
// zeroes match scores and stats. No gating — callers (leader button, dedicated
// auto-return, empty-server reset) enforce their own preconditions.
void AProjectCompGameMode::ResetToLobby()
{
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS)
	{
		return;
	}

	bMatchStarted = false;
	LastPreparedRound = 0;  // force a fresh round-1 prep when the next match starts

	// Zero both teams' scores + per-round ability state.
	for (uint8 i = 0; i < 2; ++i)
	{
		FTeamLoadout Loadout = GS->GetTeamLoadout(static_cast<ETeamID>(i));
		Loadout.Score = 0;
		Loadout.AbilitySlot1 = EAbilityType::None;
		Loadout.AbilitySlot2 = EAbilityType::None;
		Loadout.SharedCooldownPool = 1.0f;
		GS->Server_SetTeamLoadout(static_cast<ETeamID>(i), Loadout);
	}

	// Per-player reset (teams are retained so a rematch is one click in the lobby).
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (APlayerController* PC = It->Get())
		{
			if (AProjectCompPlayerState* PS = PC->GetPlayerState<AProjectCompPlayerState>())
			{
				PS->Server_SetIsAlive(true);
				PS->Server_ClearVotes();
				PS->Server_ResetLives();
				PS->Server_SetRespawnEndTime(0.0f);
				PS->Server_ResetLoomUsage();
				PS->Server_ClearEquippedGrenades();
				PS->Server_ClearLoadout();
				PS->Server_ClearChosenLoomAbility();
				PS->Server_SetCredits(StartingCredits);
				PS->Server_ResetMatchStats();
			}
		}
	}

	// Clear dropped loot, reset the bomb, and respawn everyone at their team spawn.
	Server_CleanupGroundWeapons();
	Server_ResetAllBombSites();
	AssignedStarts.Empty();
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (APlayerController* PC = It->Get())
		{
			RespawnPlayerAtAssignedStart(PC);
		}
	}

	// Back to the team-select lobby. Clients' UpdateInputModeForPhase re-opens the
	// lobby widget when they see the None phase.
	GS->Server_SetMatchWinner(ETeamID::Spectator);
	GS->Server_SetSelectionWindowOpen(false);
	GS->Server_SetRound(1);
	GS->Server_SetPhaseTimeRemaining(0.0f);
	GS->Server_SetPhase(ERoundPhase::None);

	UE_LOG(LogProjectComp, Log, TEXT("[Lobby] Match reset to the team-select lobby."));
}

void AProjectCompGameMode::TickServerLobby()
{
	if (bMatchStarted)
	{
		return;
	}
	const AProjectCompGameState* GS = GetTypedGameState();
	if (!GS || GS->GetCurrentPhase() != ERoundPhase::None)
	{
		return;
	}
	if (GS->PlayerArray.Num() >= MinPlayersToAutoStart)
	{
		UE_LOG(LogProjectComp, Log, TEXT("[Server] Auto-starting match (%d players present)."), GS->PlayerArray.Num());
		BeginMatch();
	}
}

int32 AProjectCompGameMode::CountPlayersOnTeam(ETeamID Team, const APlayerState* Ignore) const
{
	const AProjectCompGameState* GS = GetTypedGameState();
	if (!GS)
	{
		return 0;
	}
	int32 Count = 0;
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		if (!PSBase || PSBase == Ignore)
		{
			continue;
		}
		if (const AProjectCompPlayerState* PS = Cast<AProjectCompPlayerState>(PSBase))
		{
			if (PS->GetTeamID() == Team)
			{
				++Count;
			}
		}
	}
	return Count;
}

void AProjectCompGameMode::CheckForfeit(const APlayerState* Ignore)
{
	const AProjectCompGameState* GS = GetTypedGameState();
	if (!GS)
	{
		return;
	}
	const ERoundPhase Phase = GS->GetCurrentPhase();
	// Only forfeit while a match is actually being played (not in lobby / results).
	if (!bMatchStarted || Phase == ERoundPhase::None || Phase == ERoundPhase::MatchEnded)
	{
		return;
	}
	const int32 Att = CountPlayersOnTeam(ETeamID::Attackers, Ignore);
	const int32 Def = CountPlayersOnTeam(ETeamID::Defenders, Ignore);
	if (Att == 0 && Def > 0)
	{
		UE_LOG(LogProjectComp, Log, TEXT("[Server] Attackers empty mid-match — Defenders win by forfeit."));
		EndMatch(ETeamID::Defenders);
	}
	else if (Def == 0 && Att > 0)
	{
		UE_LOG(LogProjectComp, Log, TEXT("[Server] Defenders empty mid-match — Attackers win by forfeit."));
		EndMatch(ETeamID::Attackers);
	}
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
	//                          LoomSel    PreRound    RoundActive    PostRound
	//   AttackerSpawnLock      true       true        false          false
	//   DefenderSpawnLock      false      false       true           true
	//   DefenderPreRoundLock   true       true        false          false
	//
	// LoomSelection mirrors PreRound for barriers: players are locked into
	// spawn rooms during the mouse-locked Loom claim window, same as the
	// hover-free buy phase that follows.
	const bool bIsPrePlayPhase = (NewPhase == ERoundPhase::LoomSelection
	                           || NewPhase == ERoundPhase::PreRound);
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
				bShouldBeActive = bIsPrePlayPhase;
				break;
			case EBarrierKind::DefenderSpawnLock:
				bShouldBeActive = (NewPhase == ERoundPhase::RoundActive
				                || NewPhase == ERoundPhase::PostRound);
				break;
			case EBarrierKind::DefenderPreRoundLock:
				bShouldBeActive = bIsPrePlayPhase;
				break;
		}
		B->Server_SetActive(bShouldBeActive);
	}
}

bool AProjectCompGameMode::IsAnyBombPlanted() const
{
	return BombActor && BombActor->IsPlanted();
}

bool AProjectCompGameMode::IsActorInBombSite(const AActor* Actor) const
{
	if (!Actor)
	{
		return false;
	}
	for (const TWeakObjectPtr<AProjectCompBombSite>& WeakSite : CachedBombSites)
	{
		const AProjectCompBombSite* Site = WeakSite.Get();
		if (Site && Site->IsActorInside(Actor))
		{
			return true;
		}
	}
	return false;
}

void AProjectCompGameMode::HandleBombResolution()
{
	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS || !BombActor)
	{
		return;
	}

	// Plant detonation = 1 point, defuse = 1 point — uniform so the score reads as
	// a count of rounds won.
	constexpr int32 BombResolutionPoints = 1;

	ETeamID WinningTeam = ETeamID::Spectator;
	if (BombActor->IsDetonated())
	{
		GS->Server_AddTeamScore(ETeamID::Attackers, BombResolutionPoints);
		WinningTeam = ETeamID::Attackers;
		UE_LOG(LogProjectComp, Log, TEXT("Round %d: Bomb DETONATED. Attackers +%d (now %d)."),
			GS->GetCurrentRound(), BombResolutionPoints, GS->GetTeamLoadout(ETeamID::Attackers).Score);
	}
	else if (BombActor->IsDefused())
	{
		GS->Server_AddTeamScore(ETeamID::Defenders, BombResolutionPoints);
		WinningTeam = ETeamID::Defenders;
		UE_LOG(LogProjectComp, Log, TEXT("Round %d: Bomb DEFUSED. Defenders +%d (now %d)."),
			GS->GetCurrentRound(), BombResolutionPoints, GS->GetTeamLoadout(ETeamID::Defenders).Score);
	}

	AwardRoundEndCredits(WinningTeam);
	StartPostRound();
}

void AProjectCompGameMode::Server_ResetAllBombSites()
{
	// Reset the bomb back to its spawn marker (Available) for the new round.
	if (BombActor)
	{
		const FVector SpawnLoc = CachedBombSpawn.IsValid() ? CachedBombSpawn->GetActorLocation() : FVector::ZeroVector;
		BombActor->Server_ResetToLocation(SpawnLoc);
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
			// Score as if the bomb detonated. Hardcoded 1 point — matches the
			// uniform plant/defuse scoring rule (see HandleBombResolution).
			constexpr int32 BombResolutionPoints = 1;
			for (const TWeakObjectPtr<AProjectCompBombSite>& WeakSite : CachedBombSites)
			{
				AProjectCompBombSite* Site = WeakSite.Get();
				if (Site && Site->bIsPlanted && !Site->IsTerminal())
				{
					GS->Server_AddTeamScore(ETeamID::Attackers, BombResolutionPoints);
					UE_LOG(LogProjectComp, Log,
						TEXT("Round %d: Defenders eliminated with bomb planted at %s. Attackers +%d (now %d)."),
						GS->GetCurrentRound(), *Site->Label.ToString(),
						BombResolutionPoints, GS->GetTeamLoadout(ETeamID::Attackers).Score);
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
	// the team voted), fall back to AA/CM so the Loom system has something to
	// test against until the draft UI lands. (Must be IMPLEMENTED slots — CA is
	// now standby/TBD, so the old CA fallback would draft an unusable ability.)
	// TODO(loom-ui): change these fallbacks back to EAbilityType::None once the
	// PreRound draft widget is in place — the production behavior should be
	// "no votes = no abilities," not "no votes = free AA/CM."
	FTeamLoadout Loadout = GS->GetTeamLoadout(Team);
	Loadout.AbilitySlot1 = Sorted.Num() > 0 ? Sorted[0].Key : EAbilityType::AA;
	Loadout.AbilitySlot2 = Sorted.Num() > 1 ? Sorted[1].Key : EAbilityType::CM;
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

void AProjectCompGameMode::Server_CleanupGroundWeapons()
{
	if (!HasAuthority())
	{
		return;
	}
	// A dropped weapon has no owner (cleared on drop); an equipped weapon's owner
	// is its wielding character. Destroy the orphaned (ground) ones so loot from
	// the previous round doesn't pile up. Collect first, then destroy, so we don't
	// mutate the actor list mid-iteration.
	TArray<AProjectCompHitscanWeapon*> ToDestroy;
	for (TActorIterator<AProjectCompHitscanWeapon> It(GetWorld()); It; ++It)
	{
		if (AProjectCompHitscanWeapon* W = *It)
		{
			if (W->GetOwner() == nullptr)
			{
				ToDestroy.Add(W);
			}
		}
	}
	for (AProjectCompHitscanWeapon* W : ToDestroy)
	{
		W->Destroy();
	}
}

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

	// Drop the bomb where the carrier fell, so other attackers can pick it up.
	if (BombActor && BombActor->GetState() == EBombState::Carried)
	{
		if (APawn* VictimPawn = Victim->GetPawn())
		{
			if (BombActor->GetCarrier() == VictimPawn)
			{
				BombActor->Server_Drop(VictimPawn->GetActorLocation());
			}
		}
	}

	// Drop the held weapon ONLY on the FINAL death of the round (no lives left —
	// the "2nd death" with MaxLives=2). On a non-final death the player will
	// respawn, so the gun stays with them (and is replaced fresh on respawn)
	// rather than littering the map. Life was already decremented above, so
	// LivesRemaining==0 here means there's no respawn coming. Dropped before
	// RelocatePlayerToTeamStart so it lands at the death spot, not at spawn.
	if (PS && PS->GetLivesRemaining() <= 0)
	{
		if (AProjectCompCharacter* VictimChar = Cast<AProjectCompCharacter>(Victim->GetPawn()))
		{
			VictimChar->DropCurrentWeaponToGround();
		}
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

ETeamID AProjectCompGameMode::PickTeamForNewPlayer(const APlayerState* Joining)
{
	// Balance by filling the smaller team, capped at MaxTeamSize; if both are full
	// the joiner becomes a spectator (e.g. the 11th player in a 5v5, or a late join
	// into full teams). Counting currently-assigned teams (rather than a join
	// counter) keeps sides balanced across mid-match leaves and rejoins. The
	// joining player is excluded from the count — they may already sit in
	// PlayerArray with a default team when PostLogin runs. Players can still
	// free-swap in the lobby via the team-select UI.
	const int32 Att = CountPlayersOnTeam(ETeamID::Attackers, Joining);
	const int32 Def = CountPlayersOnTeam(ETeamID::Defenders, Joining);
	if (Att <= Def)
	{
		if (Att < MaxTeamSize) { return ETeamID::Attackers; }
		if (Def < MaxTeamSize) { return ETeamID::Defenders; }
	}
	else
	{
		if (Def < MaxTeamSize) { return ETeamID::Defenders; }
		if (Att < MaxTeamSize) { return ETeamID::Attackers; }
	}
	return ETeamID::Spectator;
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
	case EGrenadeType::Smoke:      return SmokeGrenadeCost;
	default:                       return 0;
	}
}

int32 AProjectCompGameMode::GetWeaponCost(EWeaponClass Weapon) const
{
	switch (Weapon)
	{
	case EWeaponClass::Pistol: return PistolCost;
	case EWeaponClass::SMG:    return SMGCost;
	case EWeaponClass::Rifle:  return RifleCost;
	default:                   return 0;
	}
}

int32 AProjectCompGameMode::GetArmourCost(EArmourLevel Level) const
{
	switch (Level)
	{
	case EArmourLevel::Light: return ArmourLightCost;
	case EArmourLevel::Heavy: return ArmourHeavyCost;
	default:                  return 0;
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

bool AProjectCompGameMode::Server_CheatToggleFreezeTimer()
{
	if (!HasAuthority())
	{
		return bCheatFreezeTimer;
	}
	bCheatFreezeTimer = !bCheatFreezeTimer;
	const AProjectCompGameState* GS = GetTypedGameState();
	UE_LOG(LogProjectComp, Log,
		TEXT("[Cheat] Phase timer %s (phase=%d, %.1fs on the clock)."),
		bCheatFreezeTimer ? TEXT("FROZEN") : TEXT("resumed"),
		GS ? static_cast<int32>(GS->GetCurrentPhase()) : -1,
		GS ? GS->GetPhaseTimeRemaining() : -1.0f);
	return bCheatFreezeTimer;
}

void AProjectCompGameMode::Server_CheatAdvancePhase()
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

	// Lobby: there's no timer to expire — kick off the match (round 1).
	if (GS->GetCurrentPhase() == ERoundPhase::None)
	{
		if (!bMatchStarted)
		{
			bMatchStarted = true;
			UE_LOG(LogProjectComp, Log, TEXT("[Cheat] NextPhase from lobby — starting match."));
			if (ShouldRunLoomSelectionForRound(1)) { StartLoomSelection(); }
			else { StartPreRound(); }
		}
		return;
	}

	// Otherwise force the phase clock to expire; the normal Tick transition then
	// advances to the next phase (and handles round-advance / halftime / match-end).
	UE_LOG(LogProjectComp, Log, TEXT("[Cheat] NextPhase — expiring phase %d."),
		static_cast<int32>(GS->GetCurrentPhase()));
	GS->Server_SetPhaseTimeRemaining(0.0f);
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

	// Phase gate: Loom claims only land during the dedicated LoomSelection
	// phase (10s mouse-locked window between PostRound and PreRound). The
	// per-team no-duplicates check happens below — this is the phase guard.
	if (GS->GetCurrentPhase() != ERoundPhase::LoomSelection)
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("[Loom claim] Rejected: phase=%d (must be LoomSelection)."),
			static_cast<int32>(GS->GetCurrentPhase()));
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

	const int32 Cost = GetGrenadeCost(Type);
	if (Cost <= 0)
	{
		return false;
	}

	// Refund toggle: clicking a grenade you ALREADY own refunds the credits
	// and removes it from your kit. This is the same RPC used by the buy
	// path — the HUD has no "sell" button; left-click on an owned item
	// just inverts the buy.
	if (BuyerPS->HasGrenadeEquipped(Type))
	{
		BuyerPS->Server_UnequipGrenade(Type);
		BuyerPS->Server_AddCredits(Cost);
		UE_LOG(LogProjectComp, Log,
			TEXT("[Grenade buy] '%s' REFUNDED grenade %d for %d credits (now %d)."),
			*BuyerPS->GetPlayerName(), static_cast<int32>(Type), Cost, BuyerPS->GetCurrentCredits());
		return true;
	}

	// Up-front gating so we can mutate atomically below without rollback paths.
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

	// Cost was computed above (used by the refund path); just check affordability.
	if (BuyerPS->GetCurrentCredits() < Cost)
	{
		return false;
	}

	// All gates passed: spend + equip. Each helper still runs its own asserts;
	// neither can fail here given the checks above.
	BuyerPS->Server_SpendCredits(Cost);
	BuyerPS->Server_EquipGrenade(Type);
	// If the buyer had this on their wishlist, drop the request — they own it
	// now, no need to keep asking teammates.
	BuyerPS->Server_RemoveGrenadeRequest(Type);

	UE_LOG(LogProjectComp, Log,
		TEXT("[Grenade buy] '%s' bought grenade %d for %d credits (remaining %d)."),
		*BuyerPS->GetPlayerName(), static_cast<int32>(Type), Cost, BuyerPS->GetCurrentCredits());
	return true;
}

bool AProjectCompGameMode::Server_BuyWeapon(APlayerController* Buyer, EWeaponClass Weapon)
{
	if (!HasAuthority()) { return false; }

	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS) { return false; }

	if (GS->GetCurrentPhase() != ERoundPhase::PreRound || !GS->IsSelectionWindowOpen())
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("[Weapon buy] Rejected: phase=%d window=%d."),
			static_cast<int32>(GS->GetCurrentPhase()), GS->IsSelectionWindowOpen() ? 1 : 0);
		return false;
	}

	if (Weapon == EWeaponClass::None) { return false; }

	AProjectCompPlayerState* BuyerPS = Buyer ? Buyer->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (!BuyerPS || BuyerPS->GetTeamID() == ETeamID::Spectator) { return false; }

	// Slot routing: Pistol → Secondary, SMG/Rifle → Primary. Reject any
	// mismatched future class to keep the buy contract simple.
	const bool bIsSecondary = (Weapon == EWeaponClass::Pistol);
	const bool bIsPrimary   = (Weapon == EWeaponClass::SMG || Weapon == EWeaponClass::Rifle);
	if (!bIsPrimary && !bIsSecondary)
	{
		return false;
	}

	const int32 Cost = GetWeaponCost(Weapon);
	if (Cost <= 0)
	{
		return false;
	}

	// Refund toggle: clicking the weapon you already own in this slot returns
	// the credits and clears the slot. Slot-mismatched clicks (e.g. click SMG
	// while owning Rifle) fall through to the standard buy path, which
	// overwrites the existing slot WITHOUT refunding the old weapon — buy
	// deliberately.
	const EWeaponClass CurrentInSlot = bIsPrimary
		? BuyerPS->GetEquippedPrimary()
		: BuyerPS->GetEquippedSecondary();
	if (CurrentInSlot == Weapon)
	{
		BuyerPS->Server_AddCredits(Cost);
		if (bIsPrimary)
		{
			BuyerPS->Server_SetEquippedPrimary(EWeaponClass::None);
		}
		else
		{
			BuyerPS->Server_SetEquippedSecondary(EWeaponClass::None);
		}
		UE_LOG(LogProjectComp, Log,
			TEXT("[Weapon buy] '%s' REFUNDED %s weapon %d for %d credits (now %d)."),
			*BuyerPS->GetPlayerName(),
			bIsPrimary ? TEXT("PRIMARY") : TEXT("SECONDARY"),
			static_cast<int32>(Weapon), Cost, BuyerPS->GetCurrentCredits());
		return true;
	}

	if (BuyerPS->GetCurrentCredits() < Cost)
	{
		return false;
	}

	// Spend + assign. Buying a slot overwrites whatever was in it — re-buying
	// the same class at the same cost is harmless (refund parity above).
	BuyerPS->Server_SpendCredits(Cost);
	if (bIsPrimary)
	{
		BuyerPS->Server_SetEquippedPrimary(Weapon);
	}
	else
	{
		BuyerPS->Server_SetEquippedSecondary(Weapon);
	}
	BuyerPS->Server_RemoveWeaponRequest(Weapon);

	UE_LOG(LogProjectComp, Log,
		TEXT("[Weapon buy] '%s' bought %s weapon %d for %d credits (remaining %d)."),
		*BuyerPS->GetPlayerName(),
		bIsPrimary ? TEXT("PRIMARY") : TEXT("SECONDARY"),
		static_cast<int32>(Weapon), Cost, BuyerPS->GetCurrentCredits());
	return true;
}

bool AProjectCompGameMode::Server_BuyArmour(APlayerController* Buyer, EArmourLevel Level)
{
	if (!HasAuthority()) { return false; }

	AProjectCompGameState* GS = GetTypedGameState();
	if (!GS) { return false; }

	if (GS->GetCurrentPhase() != ERoundPhase::PreRound || !GS->IsSelectionWindowOpen())
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("[Armour buy] Rejected: phase=%d window=%d."),
			static_cast<int32>(GS->GetCurrentPhase()), GS->IsSelectionWindowOpen() ? 1 : 0);
		return false;
	}

	if (Level == EArmourLevel::None) { return false; }

	AProjectCompPlayerState* BuyerPS = Buyer ? Buyer->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (!BuyerPS || BuyerPS->GetTeamID() == ETeamID::Spectator) { return false; }

	const int32 Cost = GetArmourCost(Level);
	if (Cost <= 0)
	{
		return false;
	}

	// Refund toggle: clicking the armour level you already own returns the
	// credits and clears the slot. Click-different-level falls through to
	// the buy path which still rejects downgrades.
	if (BuyerPS->GetArmourLevel() == Level)
	{
		BuyerPS->Server_AddCredits(Cost);
		BuyerPS->Server_SetArmourLevel(EArmourLevel::None);
		UE_LOG(LogProjectComp, Log,
			TEXT("[Armour buy] '%s' REFUNDED armour level %d for %d credits (now %d)."),
			*BuyerPS->GetPlayerName(), static_cast<int32>(Level), Cost, BuyerPS->GetCurrentCredits());
		return true;
	}

	// Downgrade attempts: reject if the player already has better armour.
	// Players can upgrade Light → Heavy by paying the Heavy cost (no Light refund).
	if (static_cast<uint8>(Level) < static_cast<uint8>(BuyerPS->GetArmourLevel()))
	{
		return false;
	}

	if (BuyerPS->GetCurrentCredits() < Cost)
	{
		return false;
	}

	BuyerPS->Server_SpendCredits(Cost);
	BuyerPS->Server_SetArmourLevel(Level);
	BuyerPS->Server_RemoveArmourRequest(Level);

	UE_LOG(LogProjectComp, Log,
		TEXT("[Armour buy] '%s' bought armour level %d for %d credits (remaining %d)."),
		*BuyerPS->GetPlayerName(), static_cast<int32>(Level), Cost, BuyerPS->GetCurrentCredits());
	return true;
}

// =====================================================================
// Request + Fulfill (right-click in buy panel + teammate fulfill button)
// =====================================================================

namespace
{
	/** Phase gate shared by every Request/Fulfill RPC. */
	bool IsBuyPhase(const AProjectCompGameState* GS)
	{
		return GS && GS->GetCurrentPhase() == ERoundPhase::PreRound && GS->IsSelectionWindowOpen();
	}

	bool ValidRequester(APlayerController* Requester, AProjectCompPlayerState*& OutPS)
	{
		OutPS = Requester ? Requester->GetPlayerState<AProjectCompPlayerState>() : nullptr;
		return OutPS && OutPS->GetTeamID() != ETeamID::Spectator;
	}
}

bool AProjectCompGameMode::Server_RequestWeapon(APlayerController* Requester, EWeaponClass Weapon)
{
	if (!HasAuthority()) { return false; }
	if (!IsBuyPhase(GetTypedGameState())) { return false; }
	if (Weapon == EWeaponClass::None) { return false; }
	AProjectCompPlayerState* PS = nullptr;
	if (!ValidRequester(Requester, PS)) { return false; }
	PS->Server_ToggleWeaponRequest(Weapon);
	UE_LOG(LogProjectComp, Verbose, TEXT("[Request] '%s' toggled weapon %d (now %d on list)."),
		*PS->GetPlayerName(), static_cast<int32>(Weapon), PS->GetRequestedWeapons().Num());
	return true;
}

bool AProjectCompGameMode::Server_RequestArmour(APlayerController* Requester, EArmourLevel Level)
{
	if (!HasAuthority()) { return false; }
	if (!IsBuyPhase(GetTypedGameState())) { return false; }
	if (Level == EArmourLevel::None) { return false; }
	AProjectCompPlayerState* PS = nullptr;
	if (!ValidRequester(Requester, PS)) { return false; }
	PS->Server_ToggleArmourRequest(Level);
	UE_LOG(LogProjectComp, Verbose, TEXT("[Request] '%s' toggled armour %d (now %d on list)."),
		*PS->GetPlayerName(), static_cast<int32>(Level), PS->GetRequestedArmour().Num());
	return true;
}

bool AProjectCompGameMode::Server_RequestGrenade(APlayerController* Requester, EGrenadeType Type)
{
	if (!HasAuthority()) { return false; }
	if (!IsBuyPhase(GetTypedGameState())) { return false; }
	if (Type == EGrenadeType::None) { return false; }
	AProjectCompPlayerState* PS = nullptr;
	if (!ValidRequester(Requester, PS)) { return false; }
	PS->Server_ToggleGrenadeRequest(Type);
	UE_LOG(LogProjectComp, Verbose, TEXT("[Request] '%s' toggled grenade %d (now %d on list)."),
		*PS->GetPlayerName(), static_cast<int32>(Type), PS->GetRequestedGrenades().Num());
	return true;
}

bool AProjectCompGameMode::Server_FulfillWeaponRequest(APlayerController* Fulfiller, APlayerController* Requester, EWeaponClass Weapon)
{
	if (!HasAuthority()) { return false; }
	if (!IsBuyPhase(GetTypedGameState())) { return false; }
	if (Weapon == EWeaponClass::None) { return false; }

	AProjectCompPlayerState* FulfillerPS = Fulfiller ? Fulfiller->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	AProjectCompPlayerState* RequesterPS = Requester ? Requester->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (!FulfillerPS || !RequesterPS) { return false; }
	if (FulfillerPS == RequesterPS) { return false; }
	if (FulfillerPS->GetTeamID() != RequesterPS->GetTeamID()) { return false; }
	if (!RequesterPS->IsRequestingWeapon(Weapon)) { return false; }

	const bool bIsSecondary = (Weapon == EWeaponClass::Pistol);
	const bool bIsPrimary   = (Weapon == EWeaponClass::SMG || Weapon == EWeaponClass::Rifle);
	if (!bIsPrimary && !bIsSecondary) { return false; }

	const int32 Cost = GetWeaponCost(Weapon);
	if (Cost <= 0 || FulfillerPS->GetCurrentCredits() < Cost) { return false; }

	FulfillerPS->Server_SpendCredits(Cost);
	if (bIsPrimary) { RequesterPS->Server_SetEquippedPrimary(Weapon); }
	else            { RequesterPS->Server_SetEquippedSecondary(Weapon); }
	RequesterPS->Server_RemoveWeaponRequest(Weapon);

	UE_LOG(LogProjectComp, Log,
		TEXT("[Fulfill] '%s' fulfilled weapon %d for '%s' — paid %d (remaining %d)."),
		*FulfillerPS->GetPlayerName(), static_cast<int32>(Weapon),
		*RequesterPS->GetPlayerName(), Cost, FulfillerPS->GetCurrentCredits());
	return true;
}

bool AProjectCompGameMode::Server_FulfillArmourRequest(APlayerController* Fulfiller, APlayerController* Requester, EArmourLevel Level)
{
	if (!HasAuthority()) { return false; }
	if (!IsBuyPhase(GetTypedGameState())) { return false; }
	if (Level == EArmourLevel::None) { return false; }

	AProjectCompPlayerState* FulfillerPS = Fulfiller ? Fulfiller->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	AProjectCompPlayerState* RequesterPS = Requester ? Requester->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (!FulfillerPS || !RequesterPS) { return false; }
	if (FulfillerPS == RequesterPS) { return false; }
	if (FulfillerPS->GetTeamID() != RequesterPS->GetTeamID()) { return false; }
	if (!RequesterPS->IsRequestingArmour(Level)) { return false; }

	// Downgrade guard: don't fulfill Light if Requester already has Heavy.
	if (static_cast<uint8>(Level) < static_cast<uint8>(RequesterPS->GetArmourLevel()))
	{
		return false;
	}

	const int32 Cost = GetArmourCost(Level);
	if (Cost <= 0 || FulfillerPS->GetCurrentCredits() < Cost) { return false; }

	FulfillerPS->Server_SpendCredits(Cost);
	RequesterPS->Server_SetArmourLevel(Level);
	RequesterPS->Server_RemoveArmourRequest(Level);

	UE_LOG(LogProjectComp, Log,
		TEXT("[Fulfill] '%s' fulfilled armour %d for '%s' — paid %d (remaining %d)."),
		*FulfillerPS->GetPlayerName(), static_cast<int32>(Level),
		*RequesterPS->GetPlayerName(), Cost, FulfillerPS->GetCurrentCredits());
	return true;
}

bool AProjectCompGameMode::Server_FulfillGrenadeRequest(APlayerController* Fulfiller, APlayerController* Requester, EGrenadeType Type)
{
	if (!HasAuthority()) { return false; }
	if (!IsBuyPhase(GetTypedGameState())) { return false; }
	if (Type == EGrenadeType::None) { return false; }

	AProjectCompPlayerState* FulfillerPS = Fulfiller ? Fulfiller->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	AProjectCompPlayerState* RequesterPS = Requester ? Requester->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (!FulfillerPS || !RequesterPS) { return false; }
	if (FulfillerPS == RequesterPS) { return false; }
	if (FulfillerPS->GetTeamID() != RequesterPS->GetTeamID()) { return false; }
	if (!RequesterPS->IsRequestingGrenade(Type)) { return false; }

	if (RequesterPS->HasGrenadeEquipped(Type)) { return false; }  // already has
	constexpr int32 MaxEquippedGrenadeTypes = 2;
	if (RequesterPS->GetEquippedGrenadeTypes().Num() >= MaxEquippedGrenadeTypes) { return false; }

	// Per-team holder cap: fulfilling counts the requester as a holder. Check
	// that the team isn't already maxed for this type.
	constexpr int32 MaxHoldersPerTypePerTeam = 2;
	int32 TeamHolders = 0;
	const ETeamID Team = RequesterPS->GetTeamID();
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* OtherPC = It->Get();
		if (!OtherPC || OtherPC == Requester) { continue; }
		const AProjectCompPlayerState* OtherPS = OtherPC->GetPlayerState<AProjectCompPlayerState>();
		if (!OtherPS || OtherPS->GetTeamID() != Team) { continue; }
		if (OtherPS->HasGrenadeEquipped(Type)) { ++TeamHolders; }
	}
	if (TeamHolders >= MaxHoldersPerTypePerTeam) { return false; }

	const int32 Cost = GetGrenadeCost(Type);
	if (Cost <= 0 || FulfillerPS->GetCurrentCredits() < Cost) { return false; }

	FulfillerPS->Server_SpendCredits(Cost);
	RequesterPS->Server_EquipGrenade(Type);
	RequesterPS->Server_RemoveGrenadeRequest(Type);

	UE_LOG(LogProjectComp, Log,
		TEXT("[Fulfill] '%s' fulfilled grenade %d for '%s' — paid %d (remaining %d)."),
		*FulfillerPS->GetPlayerName(), static_cast<int32>(Type),
		*RequesterPS->GetPlayerName(), Cost, FulfillerPS->GetCurrentCredits());
	return true;
}
