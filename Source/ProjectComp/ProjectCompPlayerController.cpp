// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompPlayerController.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "ProjectComp.h"
#include "ProjectCompCameraManager.h"
#include "ProjectCompGameMode.h"
#include "ProjectCompGameState.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompGrenadeComponent.h"
#include "ProjectCompMinimapWidget.h"
#include "ProjectCompPlayerState.h"
#include "ProjectCompSoundGrenade.h"
#include "Camera/CameraComponent.h"
#include "EngineUtils.h"
#include "Blueprint/UserWidget.h"
#include "Engine/World.h"
#include "GameFramework/PlayerInput.h"

AProjectCompPlayerController::AProjectCompPlayerController()
{
	PlayerCameraManagerClass = AProjectCompCameraManager::StaticClass();
}

void AProjectCompPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (!IsLocalPlayerController())
	{
		return;
	}

	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		for (UInputMappingContext* Context : DefaultMappingContexts)
		{
			Subsystem->AddMappingContext(Context, 0);
		}
	}

	// Bind the minimap toggle. Looming-only gate lives inside ToggleMinimap
	// so a living player pressing M is a silent no-op (logged at Verbose).
	if (UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(InputComponent))
	{
		if (MinimapAction)
		{
			EIC->BindAction(MinimapAction, ETriggerEvent::Started, this, &AProjectCompPlayerController::ToggleMinimap);
		}
	}
}

void AProjectCompPlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	// If the player respawned alive while the minimap was open (e.g. from
	// looming → respawn), force-close it. Loom UI is for the dead only.
	if (const AProjectCompPlayerState* PS = GetPlayerState<AProjectCompPlayerState>())
	{
		if (PS->IsAlive() && bMinimapOpen)
		{
			SetMinimapOpen(false);
		}
	}
}

void AProjectCompPlayerController::ToggleMinimap()
{
	// Looming-only — alive players silently ignored.
	const AProjectCompPlayerState* PS = GetPlayerState<AProjectCompPlayerState>();
	if (!PS)
	{
		return;
	}
	if (PS->IsAlive() && !bMinimapOpen)
	{
		// Pressing M while alive: no-op. Verbose so we don't spam Output Log.
		UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Minimap toggle ignored: caller is alive."));
		return;
	}

	SetMinimapOpen(!bMinimapOpen);
}

void AProjectCompPlayerController::SetMinimapOpen(bool bOpen)
{
	UProjectCompMinimapWidget* Widget = EnsureMinimapWidget();
	if (!Widget)
	{
		return;
	}

	if (bOpen && !bMinimapOpen)
	{
		// AddToViewport with a high Z so it sits above the regular HUD widget.
		Widget->AddToViewport(/*ZOrder*/ 100);
		bShowMouseCursor = true;
		// GameAndUI keeps gameplay input alive (movement, look) while letting
		// the widget capture mouse clicks. Looming players still need to
		// move around the spawn room while the map is open. SetWidgetToFocus
		// is critical for slot-selection keys (1/2) to reach NativeOnKeyDown.
		FInputModeGameAndUI InputMode;
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		InputMode.SetHideCursorDuringCapture(false);
		InputMode.SetWidgetToFocus(Widget->TakeWidget());
		SetInputMode(InputMode);
		bMinimapOpen = true;
		UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Minimap opened."));
	}
	else if (!bOpen && bMinimapOpen)
	{
		Widget->RemoveFromParent();
		bShowMouseCursor = false;
		SetInputMode(FInputModeGameOnly());
		bMinimapOpen = false;
		UE_LOG(LogProjectComp, Verbose, TEXT("[Loom] Minimap closed."));
	}
}

UProjectCompMinimapWidget* AProjectCompPlayerController::EnsureMinimapWidget()
{
	if (MinimapInstance)
	{
		return MinimapInstance;
	}
	if (!MinimapWidgetClass)
	{
		UE_LOG(LogProjectComp, Warning,
			TEXT("[Loom] MinimapWidgetClass unset on PlayerController — assign BP_MinimapWidget in defaults."));
		return nullptr;
	}
	MinimapInstance = CreateWidget<UProjectCompMinimapWidget>(this, MinimapWidgetClass);
	return MinimapInstance;
}

// =====================================================================
// BP-callable entry points
// =====================================================================

void AProjectCompPlayerController::SubmitAbilityVote(EAbilityType First, EAbilityType Second)
{
	Server_SubmitAbilityVote(First, Second);
}

void AProjectCompPlayerController::RequestAbilityCast(EAbilityType Type, FVector TargetLocation)
{
	Server_RequestAbilityCast(Type, TargetLocation);
}

void AProjectCompPlayerController::ClaimLoomAbility(EAbilityType Ability)
{
	Server_ClaimLoomAbility(Ability);
}

void AProjectCompPlayerController::BuyGrenade(EGrenadeType Type)
{
	Server_BuyGrenade(Type);
}

void AProjectCompPlayerController::UseGrenadeInSlot(int32 SlotIndex, bool bAltFire)
{
	const AProjectCompPlayerState* PS = GetPlayerState<AProjectCompPlayerState>();
	if (!PS)
	{
		return;
	}
	const TArray<EGrenadeType>& Kit = PS->GetEquippedGrenadeTypes();
	if (!Kit.IsValidIndex(SlotIndex))
	{
		// Slot empty — player didn't buy a 2nd grenade. Silent no-op so the
		// keybind doesn't spam errors when the kit isn't full.
		return;
	}
	const EGrenadeType Type = Kit[SlotIndex];
	if (Type == EGrenadeType::None)
	{
		return;
	}

	APawn* MyPawn = GetPawn();
	if (!MyPawn)
	{
		return;
	}
	// Mirror UseNade exec — resolve origin/direction from the first-person camera.
	FVector Origin = MyPawn->GetActorLocation();
	FVector Direction = MyPawn->GetActorForwardVector();
	if (const AProjectCompCharacter* PCChar = Cast<AProjectCompCharacter>(MyPawn))
	{
		if (const UCameraComponent* Cam = PCChar->GetFirstPersonCameraComponent())
		{
			Origin = Cam->GetComponentLocation();
			Direction = Cam->GetForwardVector();
		}
	}
	Server_UseGrenade(Type, bAltFire, Origin, Direction);
}

// =====================================================================
// Server RPCs — _Validate runs first (drop the connection on cheating),
// then _Implementation forwards to the GameMode for authoritative logic.
// =====================================================================

bool AProjectCompPlayerController::Server_SubmitAbilityVote_Validate(EAbilityType First, EAbilityType Second)
{
	// Cheap sanity: enum range + distinct + non-None. The GameMode re-checks
	// phase and team membership, which can change between client send and
	// server receive.
	const uint8 FirstByte = static_cast<uint8>(First);
	const uint8 SecondByte = static_cast<uint8>(Second);
	const uint8 MaxValid = static_cast<uint8>(EAbilityType::MC);
	return FirstByte != 0 && FirstByte <= MaxValid
		&& SecondByte != 0 && SecondByte <= MaxValid
		&& First != Second;
}

void AProjectCompPlayerController::Server_SubmitAbilityVote_Implementation(EAbilityType First, EAbilityType Second)
{
	if (AProjectCompGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
	{
		GM->Server_SubmitAbilityVote(this, First, Second);
	}
}

bool AProjectCompPlayerController::Server_RequestAbilityCast_Validate(EAbilityType Type, FVector TargetLocation)
{
	// Cheap sanity: enum range + reasonable target location (rejects NaN/Inf
	// and absurd magnitudes that indicate a tampered client).
	const uint8 TypeByte = static_cast<uint8>(Type);
	const uint8 MaxValid = static_cast<uint8>(EAbilityType::MC);
	if (TypeByte == 0 || TypeByte > MaxValid)
	{
		return false;
	}
	if (TargetLocation.ContainsNaN())
	{
		return false;
	}
	// 1,000,000 cm = 10 km, well beyond any realistic playspace.
	if (TargetLocation.SizeSquared() > FMath::Square(1000000.0f))
	{
		return false;
	}
	return true;
}

void AProjectCompPlayerController::Server_RequestAbilityCast_Implementation(EAbilityType Type, FVector TargetLocation)
{
	if (AProjectCompGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
	{
		GM->Server_RequestAbilityCast(this, Type, TargetLocation);
	}
}

bool AProjectCompPlayerController::Server_ClaimLoomAbility_Validate(EAbilityType Ability)
{
	const uint8 Byte = static_cast<uint8>(Ability);
	const uint8 MaxValid = static_cast<uint8>(EAbilityType::MC);
	return Byte != 0 && Byte <= MaxValid;
}

void AProjectCompPlayerController::Server_ClaimLoomAbility_Implementation(EAbilityType Ability)
{
	if (AProjectCompGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
	{
		GM->Server_ClaimLoomAbility(this, Ability);
	}
}

bool AProjectCompPlayerController::Server_BuyGrenade_Validate(EGrenadeType Type)
{
	const uint8 Byte = static_cast<uint8>(Type);
	const uint8 MaxValid = static_cast<uint8>(EGrenadeType::Sound);
	return Byte != 0 && Byte <= MaxValid;
}

void AProjectCompPlayerController::Server_BuyGrenade_Implementation(EGrenadeType Type)
{
	if (AProjectCompGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
	{
		GM->Server_BuyGrenade(this, Type);
	}
}

bool AProjectCompPlayerController::Server_CheatAddCredits_Validate(int32 Amount)
{
	// Cap the cheat to a sane band so a typo can't overflow uint16 storage.
	return Amount >= 0 && Amount <= 65535;
}

void AProjectCompPlayerController::Server_CheatAddCredits_Implementation(int32 Amount)
{
	if (AProjectCompPlayerState* PS = GetPlayerState<AProjectCompPlayerState>())
	{
		PS->Server_AddCredits(Amount);
		UE_LOG(LogProjectComp, Log,
			TEXT("[Cheat] %s credits topped up by %d (now %d)."),
			*PS->GetPlayerName(), Amount, PS->GetCurrentCredits());
	}
}

bool AProjectCompPlayerController::Server_CheatEndRound_Validate(ETeamID WinningTeam)
{
	return WinningTeam == ETeamID::Attackers || WinningTeam == ETeamID::Defenders;
}

void AProjectCompPlayerController::Server_CheatEndRound_Implementation(ETeamID WinningTeam)
{
	if (AProjectCompGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
	{
		GM->Server_CheatEndRound(WinningTeam);
	}
}

bool AProjectCompPlayerController::Server_UseGrenade_Validate(EGrenadeType Type, bool bAltFire, FVector Origin, FVector Direction)
{
	const uint8 Byte = static_cast<uint8>(Type);
	const uint8 MaxValid = static_cast<uint8>(EGrenadeType::Sound);
	if (Byte == 0 || Byte > MaxValid)
	{
		return false;
	}
	if (Origin.ContainsNaN() || Direction.ContainsNaN())
	{
		return false;
	}
	// 1,000,000 cm = 10 km — well beyond any realistic playspace, indicates tamper.
	if (Origin.SizeSquared() > FMath::Square(1000000.0f))
	{
		return false;
	}
	return true;
}

void AProjectCompPlayerController::Server_UseGrenade_Implementation(EGrenadeType Type, bool bAltFire, FVector Origin, FVector Direction)
{
	AProjectCompCharacter* Char = Cast<AProjectCompCharacter>(GetPawn());
	if (!Char)
	{
		return;
	}
	UProjectCompGrenadeComponent* GrenadeComp = Char->GetGrenadeComponent();
	if (!GrenadeComp)
	{
		return;
	}
	GrenadeComp->Server_UseGrenade(Type, bAltFire, Origin, Direction);
}

// =====================================================================
// SOUND grenade toggle + reposition
// =====================================================================

void AProjectCompPlayerController::Server_ToggleSoundGrenade_Implementation()
{
	const AProjectCompPlayerState* PS = GetPlayerState<AProjectCompPlayerState>();
	if (!PS)
	{
		return;
	}
	// Find the caller's SOUND grenade. Walks live actors — bounded (10 players
	// × max 1 SOUND grenade each), so trivial cost. No registry needed.
	for (TActorIterator<AProjectCompSoundGrenade> It(GetWorld()); It; ++It)
	{
		AProjectCompSoundGrenade* SG = *It;
		if (SG && SG->IsPlacedBy(PS))
		{
			SG->Server_ToggleActive();
			return;
		}
	}
	UE_LOG(LogProjectComp, Verbose, TEXT("[SoundToggle] No SOUND grenade owned by '%s'."),
		*PS->GetPlayerName());
}

bool AProjectCompPlayerController::Server_RepositionSoundGrenade_Validate(FVector NewOrigin)
{
	if (NewOrigin.ContainsNaN())
	{
		return false;
	}
	if (NewOrigin.SizeSquared() > FMath::Square(1000000.0f))
	{
		return false;
	}
	return true;
}

void AProjectCompPlayerController::Server_RepositionSoundGrenade_Implementation(FVector NewOrigin)
{
	AProjectCompPlayerState* PS = GetPlayerState<AProjectCompPlayerState>();
	if (!PS || PS->IsPermanentlyDead())
	{
		return;
	}
	for (TActorIterator<AProjectCompSoundGrenade> It(GetWorld()); It; ++It)
	{
		AProjectCompSoundGrenade* SG = *It;
		if (SG && SG->IsPlacedBy(PS))
		{
			SG->Server_Reposition(NewOrigin);
			return;
		}
	}
	UE_LOG(LogProjectComp, Verbose, TEXT("[SoundReposition] No SOUND grenade owned by '%s'."),
		*PS->GetPlayerName());
}

// ---- Exec wrappers for the SOUND RPCs --------------------------------

void AProjectCompPlayerController::ToggleSound()
{
	UE_LOG(LogProjectComp, Log, TEXT("[Exec] ToggleSound → Server_ToggleSoundGrenade."));
	Server_ToggleSoundGrenade();
}

void AProjectCompPlayerController::ReposSound()
{
	APawn* MyPawn = GetPawn();
	if (!MyPawn)
	{
		UE_LOG(LogProjectComp, Warning, TEXT("[Exec] ReposSound: no pawn possessed."));
		return;
	}
	// Target = where the camera is looking. Mirrors UseNade's origin pick.
	FVector Origin = MyPawn->GetActorLocation();
	if (const AProjectCompCharacter* PCChar = Cast<AProjectCompCharacter>(MyPawn))
	{
		if (const UCameraComponent* Cam = PCChar->GetFirstPersonCameraComponent())
		{
			Origin = Cam->GetComponentLocation();
		}
	}
	UE_LOG(LogProjectComp, Log, TEXT("[Exec] ReposSound → Server_RepositionSoundGrenade @ %s."),
		*Origin.ToCompactString());
	Server_RepositionSoundGrenade(Origin);
}

// =====================================================================
// Console exec hooks (dev/test only — PIE-friendly buy/claim drivers)
// =====================================================================

void AProjectCompPlayerController::BuyNade(int32 TypeIndex)
{
	const int32 MaxValid = static_cast<int32>(EGrenadeType::Sound);
	if (TypeIndex <= 0 || TypeIndex > MaxValid)
	{
		UE_LOG(LogProjectComp, Warning,
			TEXT("[Exec] BuyNade: invalid index %d. Use 1=Flash 2=Info 3=Vulnerable 4=Sound."),
			TypeIndex);
		return;
	}
	const EGrenadeType Type = static_cast<EGrenadeType>(TypeIndex);
	UE_LOG(LogProjectComp, Log, TEXT("[Exec] BuyNade %d → Server_BuyGrenade."), TypeIndex);
	Server_BuyGrenade(Type);
}

void AProjectCompPlayerController::ClaimLoom(int32 AbilityIndex)
{
	const int32 MaxValid = static_cast<int32>(EAbilityType::MC);
	if (AbilityIndex <= 0 || AbilityIndex > MaxValid)
	{
		UE_LOG(LogProjectComp, Warning,
			TEXT("[Exec] ClaimLoom: invalid index %d. Use 1=AA 2=AC 3=AM 4=CC 5=CA 6=CM 7=MM 8=MA 9=MC."),
			AbilityIndex);
		return;
	}
	const EAbilityType Ability = static_cast<EAbilityType>(AbilityIndex);
	UE_LOG(LogProjectComp, Log, TEXT("[Exec] ClaimLoom %d → Server_ClaimLoomAbility."), AbilityIndex);
	Server_ClaimLoomAbility(Ability);
}

void AProjectCompPlayerController::GiveCredits(int32 Amount)
{
	if (Amount < 0)
	{
		UE_LOG(LogProjectComp, Warning, TEXT("[Exec] GiveCredits: negative amount %d rejected."), Amount);
		return;
	}
	UE_LOG(LogProjectComp, Log, TEXT("[Exec] GiveCredits %d → Server_CheatAddCredits."), Amount);
	Server_CheatAddCredits(Amount);
}

void AProjectCompPlayerController::UseNade(int32 TypeIndex, int32 AltFire)
{
	const int32 MaxValid = static_cast<int32>(EGrenadeType::Sound);
	if (TypeIndex <= 0 || TypeIndex > MaxValid)
	{
		UE_LOG(LogProjectComp, Warning,
			TEXT("[Exec] UseNade: invalid index %d. Use 1=Flash 2=Info 3=Vulnerable 4=Sound."),
			TypeIndex);
		return;
	}

	APawn* MyPawn = GetPawn();
	if (!MyPawn)
	{
		UE_LOG(LogProjectComp, Warning, TEXT("[Exec] UseNade: no pawn possessed."));
		return;
	}

	// Origin = pawn location; Direction = camera forward if the camera component
	// resolves, otherwise pawn forward. The server re-validates inside the
	// component, so a tampered direction can't bypass anything authoritative.
	FVector Origin = MyPawn->GetActorLocation();
	FVector Direction = MyPawn->GetActorForwardVector();
	if (const AProjectCompCharacter* PCChar = Cast<AProjectCompCharacter>(MyPawn))
	{
		if (const UCameraComponent* Cam = PCChar->GetFirstPersonCameraComponent())
		{
			Origin = Cam->GetComponentLocation();
			Direction = Cam->GetForwardVector();
		}
	}

	const EGrenadeType Type = static_cast<EGrenadeType>(TypeIndex);
	const bool bAltFire = AltFire != 0;
	UE_LOG(LogProjectComp, Log, TEXT("[Exec] UseNade %d alt=%d → Server_UseGrenade."), TypeIndex, AltFire);
	Server_UseGrenade(Type, bAltFire, Origin, Direction);
}

void AProjectCompPlayerController::EndRound(int32 TeamIndex)
{
	if (TeamIndex != 0 && TeamIndex != 1)
	{
		UE_LOG(LogProjectComp, Warning,
			TEXT("[Exec] EndRound: invalid team %d. Use 0=Attackers 1=Defenders."),
			TeamIndex);
		return;
	}
	const ETeamID Team = static_cast<ETeamID>(TeamIndex);
	UE_LOG(LogProjectComp, Log, TEXT("[Exec] EndRound %d → Server_CheatEndRound."), TeamIndex);
	Server_CheatEndRound(Team);
}

void AProjectCompPlayerController::Perf(int32 Level)
{
	Level = FMath::Clamp(Level, 0, 4);

	// ResolutionQuality is a percentage (0-100), the rest are 0-4 buckets.
	// Map each preset level to the right percentage so the FPS gains scale
	// hardest at the lowest setting (which is what the iGPU friends need).
	const int32 ResolutionPct = (Level == 0) ? 50
	                           : (Level == 1) ? 75
	                           : (Level == 2) ? 90
	                           : 100;

	// Map preset → bucket index. Levels 0/1 both clamp to bucket 0 (off/low)
	// because UE only exposes 4 buckets; this lets Level 0 still differ from
	// Level 1 via the resolution percentage.
	const int32 Bucket = (Level == 0) ? 0
	                    : (Level == 1) ? 1
	                    : (Level == 2) ? 2
	                    : (Level == 3) ? 3
	                    : 4;

	auto Run = [this](const FString& Cmd)
	{
		ConsoleCommand(Cmd, /*bWriteToLog*/ true);
	};

	Run(FString::Printf(TEXT("sg.ResolutionQuality %d"), ResolutionPct));
	Run(FString::Printf(TEXT("sg.ViewDistanceQuality %d"), Bucket));
	Run(FString::Printf(TEXT("sg.AntiAliasingQuality %d"), Bucket));
	Run(FString::Printf(TEXT("sg.ShadowQuality %d"),       Bucket));
	Run(FString::Printf(TEXT("sg.PostProcessQuality %d"),  Bucket));
	Run(FString::Printf(TEXT("sg.TextureQuality %d"),      Bucket));
	Run(FString::Printf(TEXT("sg.EffectsQuality %d"),      Bucket));
	Run(FString::Printf(TEXT("sg.FoliageQuality %d"),      Bucket));

	UE_LOG(LogProjectComp, Log,
		TEXT("[Perf] Applied preset Level=%d (ResolutionScale=%d%%, bucket=%d)."),
		Level, ResolutionPct, Bucket);
}

void AProjectCompPlayerController::DumpEcon()
{
	const AProjectCompGameState* GS = GetWorld() ? GetWorld()->GetGameState<AProjectCompGameState>() : nullptr;
	const AProjectCompPlayerState* PS = GetPlayerState<AProjectCompPlayerState>();

	if (!GS || !PS)
	{
		UE_LOG(LogProjectComp, Warning, TEXT("[Exec] DumpEcon: GameState or PlayerState missing."));
		return;
	}

	FString GrenadeKit;
	for (const EGrenadeType T : PS->GetEquippedGrenadeTypes())
	{
		if (!GrenadeKit.IsEmpty()) { GrenadeKit += TEXT(","); }
		GrenadeKit += FString::FromInt(static_cast<int32>(T));
	}
	if (GrenadeKit.IsEmpty()) { GrenadeKit = TEXT("(empty)"); }

	UE_LOG(LogProjectComp, Log,
		TEXT("[Econ] phase=%d windowOpen=%d phaseTime=%.1fs | player='%s' team=%d credits=%d loomClaim=%d loomUsed=%d kit=%s"),
		static_cast<int32>(GS->GetCurrentPhase()),
		GS->IsSelectionWindowOpen() ? 1 : 0,
		GS->GetPhaseTimeRemaining(),
		*PS->GetPlayerName(),
		static_cast<int32>(PS->GetTeamID()),
		PS->GetCurrentCredits(),
		static_cast<int32>(PS->GetChosenLoomAbility()),
		PS->HasUsedLoomThisRound() ? 1 : 0,
		*GrenadeKit);
}
