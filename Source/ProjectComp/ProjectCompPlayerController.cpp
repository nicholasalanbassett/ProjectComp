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
#include "ProjectCompMinimapWidget.h"
#include "ProjectCompPlayerState.h"
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
