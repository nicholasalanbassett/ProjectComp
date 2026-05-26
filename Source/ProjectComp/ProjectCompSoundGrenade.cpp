// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompSoundGrenade.h"
#include "ProjectComp.h"
#include "ProjectCompPlayerState.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"

AProjectCompSoundGrenade::AProjectCompSoundGrenade()
{
	// Tick at engine rate but throttled internally — placer-life check only
	// runs every PlacerCheckIntervalSeconds to keep cost negligible.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	DeviceMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DeviceMesh"));
	SetRootComponent(DeviceMesh);
	DeviceMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	ZoneSphere = CreateDefaultSubobject<USphereComponent>(TEXT("ZoneSphere"));
	ZoneSphere->SetupAttachment(DeviceMesh);
	ZoneSphere->InitSphereRadius(ZoneRadius);
	// Query-only — a future audio system can call GetOverlappingActors on this.
	ZoneSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ZoneSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	ZoneSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	ZoneSphere->SetGenerateOverlapEvents(false);  // future audio system pulls on demand

	bReplicates = true;
	SetReplicateMovement(false);
}

void AProjectCompSoundGrenade::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AProjectCompSoundGrenade, bIsActive);
	DOREPLIFETIME(AProjectCompSoundGrenade, PlacerPlayerState);
}

void AProjectCompSoundGrenade::BeginPlay()
{
	Super::BeginPlay();
	if (ZoneSphere)
	{
		ZoneSphere->SetSphereRadius(ZoneRadius, /*bUpdateOverlaps*/ false);
	}
}

void AProjectCompSoundGrenade::Server_InitializeExtra(bool /*bAltFire*/, const FVector& /*AimDirection*/)
{
	if (!HasAuthority())
	{
		return;
	}
	// Stamp the placer reference. The component sets Owner=PC at spawn-time, so
	// we resolve the PlayerState off the owner chain here.
	if (AProjectCompPlayerState* PS = GetOwnerPlayerState())
	{
		PlacerPlayerState = PS;
		UE_LOG(LogProjectComp, Log,
			TEXT("[SoundGrenade] %s placed by '%s' at %s — radius %.0fcm."),
			*GetName(), *PS->GetPlayerName(), *GetActorLocation().ToCompactString(), ZoneRadius);
	}
	else
	{
		UE_LOG(LogProjectComp, Warning,
			TEXT("[SoundGrenade] %s could not resolve placer PlayerState — toggle/reposition will be unowned."),
			*GetName());
	}
}

void AProjectCompSoundGrenade::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority())
	{
		return;
	}

	// Throttled placer-life check. 0.5s cadence is plenty — destruction on
	// permanent-death doesn't need to be instantaneous, just eventual.
	constexpr float PlacerCheckIntervalSeconds = 0.5f;
	PlacerCheckAccumulator += DeltaSeconds;
	if (PlacerCheckAccumulator < PlacerCheckIntervalSeconds)
	{
		return;
	}
	PlacerCheckAccumulator = 0.0f;

	const AProjectCompPlayerState* PS = PlacerPlayerState.Get();
	if (!PS || PS->IsPermanentlyDead())
	{
		UE_LOG(LogProjectComp, Log,
			TEXT("[SoundGrenade] %s placer gone or permanent-dead — destroying."),
			*GetName());
		Destroy();
	}
}

bool AProjectCompSoundGrenade::IsPlacedBy(const AProjectCompPlayerState* PS) const
{
	return PS && PlacerPlayerState.Get() == PS;
}

void AProjectCompSoundGrenade::Server_ToggleActive()
{
	if (!HasAuthority())
	{
		return;
	}
	bIsActive = !bIsActive;
	UE_LOG(LogProjectComp, Log, TEXT("[SoundGrenade] %s toggled active=%d."),
		*GetName(), bIsActive ? 1 : 0);
}

bool AProjectCompSoundGrenade::Server_Reposition(const FVector& NewOrigin)
{
	if (!HasAuthority())
	{
		return false;
	}
	if (RepositionCount >= RepositionsAllowed)
	{
		UE_LOG(LogProjectComp, Verbose,
			TEXT("[SoundGrenade] %s reposition rejected — already used %d/%d."),
			*GetName(), RepositionCount, RepositionsAllowed);
		return false;
	}
	SetActorLocation(NewOrigin, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
	++RepositionCount;
	UE_LOG(LogProjectComp, Log,
		TEXT("[SoundGrenade] %s repositioned to %s (use %d/%d)."),
		*GetName(), *NewOrigin.ToCompactString(), RepositionCount, RepositionsAllowed);
	return true;
}

void AProjectCompSoundGrenade::HandleDetonationGameplay()
{
	// Sound grenade has no "explosion" — Destroy() in normal flow is via
	// placer-permadeath in Tick. If something else triggers Server_Detonate
	// (e.g. round end), there's no gameplay effect to clean up.
}
