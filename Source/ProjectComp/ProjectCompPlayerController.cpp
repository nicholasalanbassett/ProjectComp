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
#include "ProjectCompHUD.h"
#include "ProjectCompHUDWidget.h"
#include "ProjectCompMinimapWidget.h"
#include "ProjectCompPlayerState.h"
#include "ProjectCompSettingsWidget.h"
#include "Camera/CameraComponent.h"
#include "EngineUtils.h"
#include "Blueprint/UserWidget.h"
#include "Engine/Engine.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Components/LightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/PlayerInput.h"
#include "HAL/IConsoleManager.h"
#include "Particles/ParticleSystemComponent.h"
#include "RHI.h"
#include "RenderTimer.h"

// UE5.7 doesn't expose these in any public header — they're defined in
// UnrealEngine.cpp as ENGINE_API globals. PerfCheck reads them for the
// smoothed FPS/frame-time display.
extern ENGINE_API float GAverageFPS;
extern ENGINE_API float GAverageMS;

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

	// Raw-key binding for B = toggle PreRound buy menu. Using BindKey rather
	// than an InputAction asset so the keymap doesn't need editor changes;
	// ToggleBuyMenu itself is a cheap state flip and self-gates on phase.
	if (InputComponent)
	{
		InputComponent->BindKey(EKeys::B, IE_Pressed, this, &AProjectCompPlayerController::ToggleBuyMenu);
		// Raw-key binding for P = toggle perf settings overlay. Always available.
		InputComponent->BindKey(EKeys::P, IE_Pressed, this, &AProjectCompPlayerController::ToggleSettings);
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

void AProjectCompPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);
	UpdateInputModeForPhase();
}

void AProjectCompPlayerController::UpdateInputModeForPhase()
{
	if (!IsLocalPlayerController())
	{
		return;
	}
	// Loom minimap input mode takes priority — the player explicitly opted into
	// it and we don't want a phase transition to steal cursor capture.
	if (bMinimapOpen)
	{
		return;
	}

	UWorld* World = GetWorld();
	const AProjectCompGameState* GS = World ? World->GetGameState<AProjectCompGameState>() : nullptr;
	if (!GS)
	{
		return;
	}

	EAppliedInputMode Desired = EAppliedInputMode::GameOnly;
	const ERoundPhase Phase = GS->GetCurrentPhase();

	// Settings overlay takes priority — when it's open the cursor must be
	// usable to click tier buttons regardless of phase.
	if (bSettingsOpen)
	{
		Desired = EAppliedInputMode::GameAndUILocked;
	}
	else if (Phase == ERoundPhase::LoomSelection)
	{
		// Mouse-locked claim phase. Cursor is captured to the viewport so
		// players can't accidentally click outside the buy panel.
		Desired = EAppliedInputMode::GameAndUILocked;
	}
	else if (Phase == ERoundPhase::PreRound && GS->IsSelectionWindowOpen())
	{
		// PreRound default = full game input so the player can look around and
		// reposition freely. Only flip to hover-free when the buy menu is
		// actually open (B). Closed → GameOnly stays the default.
		bool bBuyOpen = false;
		if (const AProjectCompHUD* H = Cast<AProjectCompHUD>(GetHUD()))
		{
			if (const UProjectCompHUDWidget* W = H->GetHUDWidget())
			{
				bBuyOpen = W->IsBuyMenuOpen();
			}
		}
		Desired = bBuyOpen ? EAppliedInputMode::GameAndUIHoverFree : EAppliedInputMode::GameOnly;
	}

	if (Desired == LastAppliedInputMode)
	{
		return;
	}
	LastAppliedInputMode = Desired;

	switch (Desired)
	{
	case EAppliedInputMode::GameOnly:
		bShowMouseCursor = false;
		SetInputMode(FInputModeGameOnly());
		break;
	case EAppliedInputMode::GameAndUILocked:
	{
		bShowMouseCursor = true;
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::LockAlways);
		Mode.SetHideCursorDuringCapture(false);
		SetInputMode(Mode);
		break;
	}
	case EAppliedInputMode::GameAndUIHoverFree:
	{
		bShowMouseCursor = true;
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		SetInputMode(Mode);
		// Center the cursor when the buy menu opens (same as the grenade map) so
		// it always starts mid-screen rather than wherever it was left. This case
		// only runs on the transition INTO hover-free (LastAppliedInputMode guard
		// above), i.e. once per buy-menu open.
		{
			int32 ViewX = 0, ViewY = 0;
			GetViewportSize(ViewX, ViewY);
			if (ViewX > 0 && ViewY > 0)
			{
				SetMouseLocation(ViewX / 2, ViewY / 2);
			}
		}
		break;
	}
	default:
		break;
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

void AProjectCompPlayerController::ToggleBuyMenu()
{
	// Route to the HUD widget; widget gates its own visibility on phase, so
	// pressing B outside PreRound just no-ops visually (state is reset on
	// PreRound entry anyway).
	if (AProjectCompHUD* H = Cast<AProjectCompHUD>(GetHUD()))
	{
		if (UProjectCompHUDWidget* W = H->GetHUDWidget())
		{
			W->ToggleBuyMenu();
		}
	}
}

UProjectCompSettingsWidget* AProjectCompPlayerController::EnsureSettingsWidget()
{
	if (SettingsInstance) { return SettingsInstance; }

	// Fall back to the C++ class if no BP subclass was assigned in defaults.
	UClass* Class = SettingsWidgetClass
		? SettingsWidgetClass.Get()
		: UProjectCompSettingsWidget::StaticClass();

	SettingsInstance = CreateWidget<UProjectCompSettingsWidget>(this, Class);
	return SettingsInstance;
}

void AProjectCompPlayerController::ToggleSettings()
{
	if (!IsLocalController()) { return; }

	UProjectCompSettingsWidget* W = EnsureSettingsWidget();
	if (!W) { return; }

	bSettingsOpen = !bSettingsOpen;
	if (bSettingsOpen)
	{
		if (!W->IsInViewport()) { W->AddToViewport(50); }
	}
	else
	{
		W->RemoveFromParent();
	}

	// Force the input-mode tracker to re-apply on the next tick. Without this
	// the cached LastAppliedInputMode would suppress the GameOnly -> locked
	// transition when settings opens during phases that share input modes.
	LastAppliedInputMode = EAppliedInputMode::Unknown;
}

void AProjectCompPlayerController::SetMinimapOpen(bool bOpen, bool bFocusWidget)
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
		// Only grab keyboard focus when asked (Loom map needs 1/2 keys). For the
		// grenade global-drop map we skip focus so the game keeps keyboard input —
		// otherwise the focus change flushes held movement keys and the player
		// stops dead while the map is open. Mouse clicks still reach the widget
		// in GameAndUI without focus, so map placement is unaffected.
		if (bFocusWidget)
		{
			InputMode.SetWidgetToFocus(Widget->TakeWidget());
		}
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

void AProjectCompPlayerController::BuyWeapon(EWeaponClass Weapon)
{
	Server_BuyWeapon(Weapon);
}

void AProjectCompPlayerController::BuyArmour(EArmourLevel Level)
{
	Server_BuyArmour(Level);
}

void AProjectCompPlayerController::RequestWeapon (EWeaponClass Weapon) { Server_RequestWeapon(Weapon); }
void AProjectCompPlayerController::RequestArmour (EArmourLevel Level)  { Server_RequestArmour(Level); }
void AProjectCompPlayerController::RequestGrenade(EGrenadeType Type)   { Server_RequestGrenade(Type); }

void AProjectCompPlayerController::FulfillWeaponRequest (APlayerController* R, EWeaponClass W) { if (R) Server_FulfillWeaponRequest(R, W); }
void AProjectCompPlayerController::FulfillArmourRequest (APlayerController* R, EArmourLevel L) { if (R) Server_FulfillArmourRequest(R, L); }
void AProjectCompPlayerController::FulfillGrenadeRequest(APlayerController* R, EGrenadeType T) { if (R) Server_FulfillGrenadeRequest(R, T); }

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

void AProjectCompPlayerController::RequestGlobalDrop(int32 SlotIndex, FVector Target)
{
	const AProjectCompPlayerState* PS = GetPlayerState<AProjectCompPlayerState>();
	if (!PS)
	{
		return;
	}
	const TArray<EGrenadeType>& Kit = PS->GetEquippedGrenadeTypes();
	if (!Kit.IsValidIndex(SlotIndex))
	{
		return;
	}
	const EGrenadeType Type = Kit[SlotIndex];
	if (Type == EGrenadeType::None)
	{
		return;
	}
	Server_GlobalDropGrenade(Type, Target);
}

void AProjectCompPlayerController::OpenGrenadePlacement(int32 SlotIndex, const TArray<FKey>& CloseKeys)
{
	const AProjectCompPlayerState* PS = GetPlayerState<AProjectCompPlayerState>();
	if (!PS || PS->IsPermanentlyDead())
	{
		return;
	}
	// Must actually own a grenade in this slot.
	if (!PS->GetEquippedGrenadeTypes().IsValidIndex(SlotIndex))
	{
		return;
	}
	UProjectCompMinimapWidget* Widget = EnsureMinimapWidget();
	if (!Widget)
	{
		return;
	}
	Widget->SetGrenadePlacementMode(SlotIndex, CloseKeys);
	bGrenadePlacementMode = true;
	// No keyboard focus — keep the player moving while the map is open. The
	// slot-key release that closes the map comes through the gameplay input path
	// (HandleGrenadeSlotReleased), which now works because we don't flush keys.
	SetMinimapOpen(true, /*bFocusWidget*/ false);

	// Snap the cursor to screen center on open so the player always starts
	// aiming from the middle of the map (and the map's center = roughly their
	// own position), instead of wherever the cursor happened to be left.
	int32 ViewX = 0, ViewY = 0;
	GetViewportSize(ViewX, ViewY);
	if (ViewX > 0 && ViewY > 0)
	{
		SetMouseLocation(ViewX / 2, ViewY / 2);
	}

	UE_LOG(LogProjectComp, Verbose, TEXT("[Global drop] Placement minimap opened for slot %d."), SlotIndex);
}

void AProjectCompPlayerController::CloseGrenadePlacement()
{
	if (!bGrenadePlacementMode)
	{
		return;
	}
	bGrenadePlacementMode = false;
	SetMinimapOpen(false);
	if (MinimapInstance)
	{
		MinimapInstance->SetLoomCastMode();  // restore default for the Loom path
	}
	UE_LOG(LogProjectComp, Verbose, TEXT("[Global drop] Placement minimap closed."));
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
	const uint8 MaxValid = static_cast<uint8>(EGrenadeType::Smoke);
	return Byte != 0 && Byte <= MaxValid;
}

void AProjectCompPlayerController::Server_BuyGrenade_Implementation(EGrenadeType Type)
{
	if (AProjectCompGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
	{
		GM->Server_BuyGrenade(this, Type);
	}
}

bool AProjectCompPlayerController::Server_BuyWeapon_Validate(EWeaponClass Weapon)
{
	const uint8 Byte = static_cast<uint8>(Weapon);
	const uint8 MaxValid = static_cast<uint8>(EWeaponClass::Rifle);
	return Byte != 0 && Byte <= MaxValid;
}

void AProjectCompPlayerController::Server_BuyWeapon_Implementation(EWeaponClass Weapon)
{
	if (AProjectCompGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
	{
		GM->Server_BuyWeapon(this, Weapon);
	}
}

bool AProjectCompPlayerController::Server_BuyArmour_Validate(EArmourLevel Level)
{
	const uint8 Byte = static_cast<uint8>(Level);
	const uint8 MaxValid = static_cast<uint8>(EArmourLevel::Heavy);
	return Byte != 0 && Byte <= MaxValid;
}

void AProjectCompPlayerController::Server_BuyArmour_Implementation(EArmourLevel Level)
{
	if (AProjectCompGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
	{
		GM->Server_BuyArmour(this, Level);
	}
}

// ---- Request / Fulfill server RPCs ----------------------------------

bool AProjectCompPlayerController::Server_RequestWeapon_Validate(EWeaponClass Weapon)
{
	const uint8 B = static_cast<uint8>(Weapon);
	return B != 0 && B <= static_cast<uint8>(EWeaponClass::Rifle);
}
void AProjectCompPlayerController::Server_RequestWeapon_Implementation(EWeaponClass Weapon)
{
	if (auto* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
		GM->Server_RequestWeapon(this, Weapon);
}

bool AProjectCompPlayerController::Server_RequestArmour_Validate(EArmourLevel Level)
{
	const uint8 B = static_cast<uint8>(Level);
	return B != 0 && B <= static_cast<uint8>(EArmourLevel::Heavy);
}
void AProjectCompPlayerController::Server_RequestArmour_Implementation(EArmourLevel Level)
{
	if (auto* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
		GM->Server_RequestArmour(this, Level);
}

bool AProjectCompPlayerController::Server_RequestGrenade_Validate(EGrenadeType Type)
{
	const uint8 B = static_cast<uint8>(Type);
	return B != 0 && B <= static_cast<uint8>(EGrenadeType::Smoke);
}
void AProjectCompPlayerController::Server_RequestGrenade_Implementation(EGrenadeType Type)
{
	if (auto* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
		GM->Server_RequestGrenade(this, Type);
}

bool AProjectCompPlayerController::Server_FulfillWeaponRequest_Validate(APlayerController* Requester, EWeaponClass Weapon)
{
	const uint8 B = static_cast<uint8>(Weapon);
	return Requester != nullptr && B != 0 && B <= static_cast<uint8>(EWeaponClass::Rifle);
}
void AProjectCompPlayerController::Server_FulfillWeaponRequest_Implementation(APlayerController* Requester, EWeaponClass Weapon)
{
	if (auto* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
		GM->Server_FulfillWeaponRequest(this, Requester, Weapon);
}

bool AProjectCompPlayerController::Server_FulfillArmourRequest_Validate(APlayerController* Requester, EArmourLevel Level)
{
	const uint8 B = static_cast<uint8>(Level);
	return Requester != nullptr && B != 0 && B <= static_cast<uint8>(EArmourLevel::Heavy);
}
void AProjectCompPlayerController::Server_FulfillArmourRequest_Implementation(APlayerController* Requester, EArmourLevel Level)
{
	if (auto* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
		GM->Server_FulfillArmourRequest(this, Requester, Level);
}

bool AProjectCompPlayerController::Server_FulfillGrenadeRequest_Validate(APlayerController* Requester, EGrenadeType Type)
{
	const uint8 B = static_cast<uint8>(Type);
	return Requester != nullptr && B != 0 && B <= static_cast<uint8>(EGrenadeType::Smoke);
}
void AProjectCompPlayerController::Server_FulfillGrenadeRequest_Implementation(APlayerController* Requester, EGrenadeType Type)
{
	if (auto* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
		GM->Server_FulfillGrenadeRequest(this, Requester, Type);
}

bool AProjectCompPlayerController::Server_CheatAddCredits_Validate(int32 Amount)
{
#if !UE_BUILD_SHIPPING
	// Cap the cheat to a sane band so a typo can't overflow uint16 storage.
	return Amount >= 0 && Amount <= 65535;
#else
	(void)Amount;
	return false;  // Validation fails in shipping — disconnects any caller.
#endif
}

void AProjectCompPlayerController::Server_CheatAddCredits_Implementation(int32 Amount)
{
#if !UE_BUILD_SHIPPING
	if (AProjectCompPlayerState* PS = GetPlayerState<AProjectCompPlayerState>())
	{
		PS->Server_AddCredits(Amount);
		UE_LOG(LogProjectComp, Log,
			TEXT("[Cheat] %s credits topped up by %d (now %d)."),
			*PS->GetPlayerName(), Amount, PS->GetCurrentCredits());
	}
#else
	(void)Amount;
#endif
}

bool AProjectCompPlayerController::Server_CheatEndRound_Validate(ETeamID WinningTeam)
{
#if !UE_BUILD_SHIPPING
	return WinningTeam == ETeamID::Attackers || WinningTeam == ETeamID::Defenders;
#else
	(void)WinningTeam;
	return false;
#endif
}

void AProjectCompPlayerController::Server_CheatEndRound_Implementation(ETeamID WinningTeam)
{
#if !UE_BUILD_SHIPPING
	if (AProjectCompGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
	{
		GM->Server_CheatEndRound(WinningTeam);
	}
#else
	(void)WinningTeam;
#endif
}

bool AProjectCompPlayerController::Server_CheatToggleFreezeTimer_Validate()
{
#if !UE_BUILD_SHIPPING
	return true;
#else
	return false;
#endif
}

void AProjectCompPlayerController::Server_CheatToggleFreezeTimer_Implementation()
{
#if !UE_BUILD_SHIPPING
	if (AProjectCompGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr)
	{
		GM->Server_CheatToggleFreezeTimer();
	}
#endif
}

bool AProjectCompPlayerController::Server_UseGrenade_Validate(EGrenadeType Type, bool bAltFire, FVector Origin, FVector Direction)
{
	const uint8 Byte = static_cast<uint8>(Type);
	const uint8 MaxValid = static_cast<uint8>(EGrenadeType::Smoke);
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

bool AProjectCompPlayerController::Server_GlobalDropGrenade_Validate(EGrenadeType Type, FVector Target)
{
	const uint8 Byte = static_cast<uint8>(Type);
	if (Byte == 0 || Byte > static_cast<uint8>(EGrenadeType::Smoke))
	{
		return false;
	}
	if (Target.ContainsNaN() || Target.SizeSquared() > FMath::Square(1000000.0f))
	{
		return false;
	}
	return true;
}

void AProjectCompPlayerController::Server_GlobalDropGrenade_Implementation(EGrenadeType Type, FVector Target)
{
	AProjectCompCharacter* Char = Cast<AProjectCompCharacter>(GetPawn());
	if (!Char)
	{
		return;
	}
	if (UProjectCompGrenadeComponent* GrenadeComp = Char->GetGrenadeComponent())
	{
		GrenadeComp->Server_GlobalDropGrenade(Type, Target);
	}
}

// =====================================================================
// Console exec hooks (dev/test only — PIE-friendly buy/claim drivers)
// =====================================================================

void AProjectCompPlayerController::BuyNade(int32 TypeIndex)
{
	const int32 MaxValid = static_cast<int32>(EGrenadeType::Smoke);
	if (TypeIndex <= 0 || TypeIndex > MaxValid)
	{
		UE_LOG(LogProjectComp, Warning,
			TEXT("[Exec] BuyNade: invalid index %d. Use 1=Flash 2=Info 3=Vulnerable 4=Smoke."),
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
#if !UE_BUILD_SHIPPING
	if (Amount < 0)
	{
		UE_LOG(LogProjectComp, Warning, TEXT("[Exec] GiveCredits: negative amount %d rejected."), Amount);
		return;
	}
	UE_LOG(LogProjectComp, Log, TEXT("[Exec] GiveCredits %d → Server_CheatAddCredits."), Amount);
	Server_CheatAddCredits(Amount);
#else
	(void)Amount;
#endif
}

void AProjectCompPlayerController::UseNade(int32 TypeIndex, int32 AltFire)
{
	const int32 MaxValid = static_cast<int32>(EGrenadeType::Smoke);
	if (TypeIndex <= 0 || TypeIndex > MaxValid)
	{
		UE_LOG(LogProjectComp, Warning,
			TEXT("[Exec] UseNade: invalid index %d. Use 1=Flash 2=Info 3=Vulnerable 4=Smoke."),
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

void AProjectCompPlayerController::BuyW(int32 ClassIndex)
{
	const int32 MaxValid = static_cast<int32>(EWeaponClass::Rifle);
	if (ClassIndex <= 0 || ClassIndex > MaxValid)
	{
		UE_LOG(LogProjectComp, Warning,
			TEXT("[Exec] BuyW: invalid index %d. Use 1=Pistol 2=SMG 3=Rifle."),
			ClassIndex);
		return;
	}
	const EWeaponClass Weapon = static_cast<EWeaponClass>(ClassIndex);
	UE_LOG(LogProjectComp, Log, TEXT("[Exec] BuyW %d → Server_BuyWeapon."), ClassIndex);
	Server_BuyWeapon(Weapon);
}

void AProjectCompPlayerController::BuyA(int32 LevelIndex)
{
	const int32 MaxValid = static_cast<int32>(EArmourLevel::Heavy);
	if (LevelIndex <= 0 || LevelIndex > MaxValid)
	{
		UE_LOG(LogProjectComp, Warning,
			TEXT("[Exec] BuyA: invalid index %d. Use 1=Light 2=Heavy."),
			LevelIndex);
		return;
	}
	const EArmourLevel Level = static_cast<EArmourLevel>(LevelIndex);
	UE_LOG(LogProjectComp, Log, TEXT("[Exec] BuyA %d → Server_BuyArmour."), LevelIndex);
	Server_BuyArmour(Level);
}

void AProjectCompPlayerController::EndRound(int32 TeamIndex)
{
#if !UE_BUILD_SHIPPING
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
#else
	(void)TeamIndex;
#endif
}

void AProjectCompPlayerController::PauseTimer()
{
#if !UE_BUILD_SHIPPING
	UE_LOG(LogProjectComp, Log, TEXT("[Exec] PauseTimer → Server_CheatToggleFreezeTimer."));
	Server_CheatToggleFreezeTimer();
#endif
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

	CurrentPerfLevel = Level;

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

void AProjectCompPlayerController::PerfCheck()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogProjectComp, Warning, TEXT("[PerfCheck] No world."));
		return;
	}

	// ---- Frame timing -----------------------------------------------------
	// GAverageFPS is smoothed across recent frames; the cycle counters give a
	// per-thread snapshot of the last completed frame. Derive ms from FPS so
	// we don't depend on GAverageMS (renamed/removed in some 5.x branches).
	const float FPS       = GAverageFPS;
	const float FrameMs   = (FPS > 0.0f) ? (1000.0f / FPS) : 0.0f;
	const float GameMs    = FPlatformTime::ToMilliseconds(GGameThreadTime);
	const float RenderMs  = FPlatformTime::ToMilliseconds(GRenderThreadTime);
	const float GPUMs     = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());

	// ---- Draw calls + RHI -------------------------------------------------
	// GNumDrawCallsRHI accumulates across all viewports for the frame; index 0
	// is the main viewport.
	const int32 DrawCalls = GNumDrawCallsRHI[0];
	const int32 PrimsDrawn = GNumPrimitivesDrawnRHI[0];

	// ---- Scene composition -----------------------------------------------
	int32 NumActors = 0;
	int32 NumStaticMeshes = 0;
	int32 NumSkelMeshes = 0;
	int32 NumLights = 0;
	int32 NumPSCs = 0;
	int32 NaniteEnabledMeshes = 0;
	int64 TotalStaticTris = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		++NumActors;
	}
	for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
	{
		if (!It->IsValidLowLevelFast() || It->GetWorld() != World) { continue; }
		if (!It->IsVisible() || It->IsTemplate()) { continue; }
		++NumStaticMeshes;
		if (UStaticMesh* SM = It->GetStaticMesh())
		{
#if WITH_EDITORONLY_DATA
			// IsNaniteEnabled() lives behind WITH_EDITORONLY_DATA in 5.7; in
			// packaged builds the Nanite count simply stays 0.
			if (SM->IsNaniteEnabled()) { ++NaniteEnabledMeshes; }
#endif
			if (SM->GetRenderData() && SM->GetRenderData()->LODResources.Num() > 0)
			{
				TotalStaticTris += SM->GetRenderData()->LODResources[0].GetNumTriangles();
			}
		}
	}
	for (TObjectIterator<USkeletalMeshComponent> It; It; ++It)
	{
		if (!It->IsValidLowLevelFast() || It->GetWorld() != World) { continue; }
		if (!It->IsVisible() || It->IsTemplate()) { continue; }
		++NumSkelMeshes;
	}
	for (TObjectIterator<ULightComponent> It; It; ++It)
	{
		if (!It->IsValidLowLevelFast() || It->GetWorld() != World) { continue; }
		if (It->IsTemplate()) { continue; }
		++NumLights;
	}
	for (TObjectIterator<UParticleSystemComponent> It; It; ++It)
	{
		if (!It->IsValidLowLevelFast() || It->GetWorld() != World) { continue; }
		if (!It->IsActive() || It->IsTemplate()) { continue; }
		++NumPSCs;
	}

	// ---- Key rendering CVars (snapshot — confirms current quality level) -
	auto ReadCVarInt = [](const TCHAR* Name) -> int32
	{
		IConsoleVariable* CV = IConsoleManager::Get().FindConsoleVariable(Name);
		return CV ? CV->GetInt() : -1;
	};
	auto ReadCVarFloat = [](const TCHAR* Name) -> float
	{
		IConsoleVariable* CV = IConsoleManager::Get().FindConsoleVariable(Name);
		return CV ? CV->GetFloat() : -1.0f;
	};
	const float ScreenPct = ReadCVarFloat(TEXT("r.ScreenPercentage"));
	const int32 SgShadow  = ReadCVarInt(TEXT("sg.ShadowQuality"));
	const int32 SgPP      = ReadCVarInt(TEXT("sg.PostProcessQuality"));
	const int32 SgView    = ReadCVarInt(TEXT("sg.ViewDistanceQuality"));
	const int32 SgEffects = ReadCVarInt(TEXT("sg.EffectsQuality"));
	const int32 SgGI      = ReadCVarInt(TEXT("sg.GlobalIlluminationQuality"));
	const int32 SgRefl    = ReadCVarInt(TEXT("sg.ReflectionQuality"));

	// ---- Format + emit ----------------------------------------------------
	TArray<FString> Lines;
	Lines.Add(TEXT("===== PerfCheck ====="));
	Lines.Add(FString::Printf(TEXT("Frame:  %.2fms (%.1f FPS)   Game:%.2f  Render:%.2f  GPU:%.2f"),
		FrameMs, FPS, GameMs, RenderMs, GPUMs));
	Lines.Add(FString::Printf(TEXT("RHI:    %d draws   %d prims drawn"),
		DrawCalls, PrimsDrawn));
	Lines.Add(FString::Printf(TEXT("Scene:  %d actors  StaticMC:%d (Nanite:%d, ~%lldk static tris)  SkelMC:%d  Lights:%d  ActivePSC:%d"),
		NumActors, NumStaticMeshes, NaniteEnabledMeshes, TotalStaticTris / 1000, NumSkelMeshes, NumLights, NumPSCs));
	Lines.Add(FString::Printf(TEXT("CVars:  r.ScreenPercentage=%.0f  sg(shadow=%d pp=%d view=%d fx=%d gi=%d refl=%d)"),
		ScreenPct, SgShadow, SgPP, SgView, SgEffects, SgGI, SgRefl));

	// Heuristic verdicts so you don't have to remember the rules of thumb.
	{
		FString Verdict;
		if      (FPS >= 90.f)              { Verdict += TEXT("FPS:OK ");      }
		else if (FPS >= 60.f)              { Verdict += TEXT("FPS:fine ");    }
		else if (FPS >= 30.f)              { Verdict += TEXT("FPS:LOW ");     }
		else                               { Verdict += TEXT("FPS:CRIT ");    }

		if      (DrawCalls < 1500)         { Verdict += TEXT("Draws:OK ");    }
		else if (DrawCalls < 3000)         { Verdict += TEXT("Draws:high ");  }
		else                               { Verdict += TEXT("Draws:CRIT ");  }

		// CPU-bound vs GPU-bound hint.
		const float CpuMs = FMath::Max(GameMs, RenderMs);
		if      (CpuMs > GPUMs * 1.25f)    { Verdict += TEXT("Bound:CPU ");   }
		else if (GPUMs > CpuMs * 1.25f)    { Verdict += TEXT("Bound:GPU ");   }
		else                               { Verdict += TEXT("Bound:bal ");   }
		Lines.Add(FString::Printf(TEXT("Verdict: %s"), *Verdict));
	}

	for (const FString& Line : Lines)
	{
		UE_LOG(LogProjectComp, Display, TEXT("%s"), *Line);
	}
	if (GEngine)
	{
		// Stagger keys so the lines stack in order rather than overwrite.
		int32 Key = 9000;
		for (const FString& Line : Lines)
		{
			GEngine->AddOnScreenDebugMessage(Key++, 12.0f, FColor::Yellow, Line);
		}
	}
}
