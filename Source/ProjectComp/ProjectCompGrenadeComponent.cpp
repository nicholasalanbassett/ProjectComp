// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompGrenadeComponent.h"
#include "ProjectComp.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompGameState.h"
#include "ProjectCompGrenadeBase.h"
#include "ProjectCompPlayerState.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

UProjectCompGrenadeComponent::UProjectCompGrenadeComponent()
{
	// No per-tick work — this component only acts on explicit Server_UseGrenade calls.
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);  // state lives on PlayerState, not on the component
}

void UProjectCompGrenadeComponent::BeginPlay()
{
	Super::BeginPlay();
	OwnerCharacter = Cast<AProjectCompCharacter>(GetOwner());
	if (!OwnerCharacter)
	{
		UE_LOG(LogProjectComp, Warning,
			TEXT("[GrenadeComp] Attached to non-ProjectCompCharacter actor '%s' — Server_UseGrenade will be a no-op."),
			*GetNameSafe(GetOwner()));
	}
}

float UProjectCompGrenadeComponent::GetCooldownForType(EGrenadeType Type) const
{
	switch (Type)
	{
	case EGrenadeType::Flash:      return FlashCooldown;
	case EGrenadeType::Info:       return InfoCooldown;
	case EGrenadeType::Vulnerable: return VulnerableCooldown;
	case EGrenadeType::Sound:      return SoundCooldown;
	default:                       return 0.0f;
	}
}

AProjectCompGrenadeBase* UProjectCompGrenadeComponent::Server_UseGrenade(EGrenadeType Type, bool bAltFire, const FVector& Origin, const FVector& Direction)
{
	UWorld* World = GetWorld();
	if (!World || !GetOwner() || !GetOwner()->HasAuthority())
	{
		return nullptr;
	}

	if (Type == EGrenadeType::None)
	{
		return nullptr;
	}

	if (!OwnerCharacter)
	{
		return nullptr;
	}

	// Phase gate: RoundActive only. PreRound is for buying + Loom draft, not throws.
	const AProjectCompGameState* GS = World->GetGameState<AProjectCompGameState>();
	if (!GS || GS->GetCurrentPhase() != ERoundPhase::RoundActive)
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("[Grenade use] Rejected: phase=%d (must be RoundActive)."),
			GS ? static_cast<int32>(GS->GetCurrentPhase()) : -1);
		return nullptr;
	}

	// Resolve PlayerState through the character's controller. Non-const because
	// SpawnActor's Owner field expects a mutable AActor*.
	APlayerController* PC = Cast<APlayerController>(OwnerCharacter->GetController());
	AProjectCompPlayerState* PS = PC ? PC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (!PS)
	{
		return nullptr;
	}

	// Permanent-ghost lockout per design memory: usable while alive OR respawning,
	// but NOT during permanent-ghost spectate (both lives exhausted).
	if (PS->IsPermanentlyDead())
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("[Grenade use] Rejected: caller is permanently dead."));
		return nullptr;
	}

	// Kit gate: must own this type for the current round.
	if (!PS->HasGrenadeEquipped(Type))
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("[Grenade use] Rejected: type %d not in kit."),
			static_cast<int32>(Type));
		return nullptr;
	}

	// Cooldown gate: type-specific end-time stamped on PlayerState.
	if (PS->IsGrenadeOnCooldown(Type))
	{
		UE_LOG(LogProjectComp, Verbose, TEXT("[Grenade use] Rejected: type %d on cooldown until %.2f."),
			static_cast<int32>(Type), PS->GetGrenadeCooldownEndTime(Type));
		return nullptr;
	}

	// Spawn-class lookup. Empty map (or missing entry) cleanly rejects — useful
	// while subclasses are being authored: missing class = "can't throw yet."
	const TSubclassOf<AProjectCompGrenadeBase>* FoundClass = GrenadeClasses.Find(Type);
	if (!FoundClass || !*FoundClass)
	{
		UE_LOG(LogProjectComp, Warning,
			TEXT("[Grenade use] Rejected: no spawn class configured for type %d. Set GrenadeClasses on the character's GrenadeComponent."),
			static_cast<int32>(Type));
		return nullptr;
	}

	// Build the spawn transform from the caller's aim. Direction is normalized
	// defensively — exec/callers may pass an unnormalized vector.
	FVector AimDir = Direction;
	if (!AimDir.Normalize())
	{
		AimDir = OwnerCharacter->GetActorForwardVector();
	}
	const FTransform SpawnXform(AimDir.Rotation(), Origin);

	FActorSpawnParameters SpawnParams;
	// Setting Owner = PC routes any future RPCs and gives correct relevancy
	// filtering. Instigator = the pawn so damage attribution chains through.
	SpawnParams.Owner = PC;
	SpawnParams.Instigator = OwnerCharacter;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AProjectCompGrenadeBase* Grenade = World->SpawnActor<AProjectCompGrenadeBase>(
		*FoundClass, SpawnXform, SpawnParams);
	if (!Grenade)
	{
		UE_LOG(LogProjectComp, Warning, TEXT("[Grenade use] SpawnActor returned null for type %d."),
			static_cast<int32>(Type));
		return nullptr;
	}

	Grenade->Server_Initialize(PS->GetTeamID(), Type);
	Grenade->Server_InitializeExtra(bAltFire, AimDir);

	// Stamp cooldown end-time on the PlayerState so subsequent use attempts of
	// this type are gated by IsGrenadeOnCooldown. Uses GameState's replicated
	// server clock so client UI compares against the same epoch.
	const float Now = GS->GetServerWorldTimeSeconds();
	const float CD = GetCooldownForType(Type);
	PS->Server_SetGrenadeCooldownEndTime(Type, Now + CD);

	UE_LOG(LogProjectComp, Log,
		TEXT("[Grenade use] '%s' used type %d (alt=%d) at %s (CD %.1fs)."),
		*PS->GetPlayerName(), static_cast<int32>(Type), bAltFire ? 1 : 0, *Origin.ToCompactString(), CD);
	return Grenade;
}
