// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompFlashDome.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompPlayerState.h"
#include "ProjectComp.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"

AProjectCompFlashDome::AProjectCompFlashDome()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	bReplicates = true;
	// Dome is stationary once placed; no movement replication overhead.
	SetReplicateMovement(false);

	DomeMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DomeMesh"));
	DomeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetRootComponent(DomeMesh);
}

void AProjectCompFlashDome::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AProjectCompFlashDome, CasterTeam);
}

void AProjectCompFlashDome::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority())
	{
		DespawnTime = (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f) + DomeLifetime;
	}
}

void AProjectCompFlashDome::Server_Initialize(ETeamID InCasterTeam)
{
	if (!HasAuthority())
	{
		return;
	}
	CasterTeam = InCasterTeam;
}

void AProjectCompFlashDome::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// POC visual: debug sphere on all peers so testers can see the dome volume
	// in PIE without an art asset. Compiled out in shipping. Replace with a
	// proper mesh + material once we have the dome FX authored.
#if !UE_BUILD_SHIPPING
	DrawDebugSphere(GetWorld(), GetActorLocation(), DomeRadius, 24,
		FColor(255, 220, 100), /*bPersistent*/ false, /*LifeTime*/ -1.0f, 0, /*Thickness*/ 3.0f);
#endif

	if (!HasAuthority())
	{
		return;
	}

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	if (Now >= DespawnTime)
	{
		Destroy();
		return;
	}

	Server_TickFlashApplication();
}

void AProjectCompFlashDome::Server_TickFlashApplication()
{
	UWorld* World = GetWorld();
	if (!World || CasterTeam == ETeamID::Spectator)
	{
		return;
	}

	// Build the LOS-ignore list once per tick. Pawn body meshes block ECC_Visibility
	// per BeginPlay's collision setup; without ignoring them, a teammate standing
	// between an enemy and the dome would shield that enemy — unintended for a flash.
	TArray<AActor*> IgnoreActors;
	IgnoreActors.Reserve(16);
	IgnoreActors.Add(this);
	for (TActorIterator<AProjectCompCharacter> CharIt(World); CharIt; ++CharIt)
	{
		IgnoreActors.Add(*CharIt);
	}

	const FVector DomeCenter = GetActorLocation();
	const float RadiusSq = DomeRadius * DomeRadius;

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC)
		{
			continue;
		}

		APawn* Pawn = PC->GetPawn();
		if (!Pawn)
		{
			continue;
		}

		// Enemies only (caller framing) — skip spectators and the caster's team.
		AProjectCompPlayerState* PS = PC->GetPlayerState<AProjectCompPlayerState>();
		if (!PS || !PS->IsAlive())
		{
			continue;
		}
		const ETeamID PawnTeam = PS->GetTeamID();
		if (PawnTeam == ETeamID::Spectator || PawnTeam == CasterTeam)
		{
			continue;
		}

		// Latch — one flash per dome per player.
		if (AlreadyFlashed.Contains(Pawn))
		{
			continue;
		}

		AProjectCompCharacter* Char = Cast<AProjectCompCharacter>(Pawn);
		if (!Char || !Char->GetFirstPersonCameraComponent())
		{
			continue;
		}

		const FVector CamLoc = Char->GetFirstPersonCameraComponent()->GetComponentLocation();
		if ((DomeCenter - CamLoc).SizeSquared() > RadiusSq)
		{
			continue;
		}

		if (!HasClearLineOfSight(CamLoc, DomeCenter, IgnoreActors))
		{
			continue;
		}

		PS->Server_ApplyFlash(FlashFullWhiteDuration, FlashFadeDuration);
		AlreadyFlashed.Add(Pawn);
	}
}

bool AProjectCompFlashDome::HasClearLineOfSight(const FVector& PawnCameraLoc,
                                                 const FVector& DomeCenter,
                                                 const TArray<AActor*>& IgnoreActors) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FCollisionQueryParams Params(NAME_None, /*bTraceComplex*/ false);
	Params.AddIgnoredActors(IgnoreActors);

	FHitResult Hit;
	// Return true iff trace finds NO blocking geometry — clear sightline.
	return !World->LineTraceSingleByChannel(Hit, PawnCameraLoc, DomeCenter, ECC_Visibility, Params);
}
