// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompTitanPillar.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompGameMode.h"
#include "ProjectCompPlayerState.h"
#include "ProjectComp.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

AProjectCompTitanPillar::AProjectCompTitanPillar()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	bReplicates = true;
	SetReplicateMovement(false);  // stationary once placed

	PillarMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PillarMesh"));
	SetRootComponent(PillarMesh);

	// Solid blocker: blocks pawns (movement), ECC_Visibility (bullets + thrown-
	// grenade impact traces), the projectile channel, and everything else. Static
	// world geometry from the engine's point of view.
	PillarMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	PillarMesh->SetCollisionObjectType(ECC_WorldStatic);
	PillarMesh->SetCollisionResponseToAllChannels(ECR_Block);
	PillarMesh->SetMobility(EComponentMobility::Movable);  // spawned at runtime

	// Default to the engine cylinder so the pillar is solid + visible out of the
	// box (BP can swap the mesh/material). Engine cylinder is 100cm tall, 50cm
	// radius, pivot at center — scaled to PillarRadius/Height in BeginPlay.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		PillarMesh->SetStaticMesh(CylinderFinder.Object);
	}
}

void AProjectCompTitanPillar::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AProjectCompTitanPillar, CasterTeam);
}

void AProjectCompTitanPillar::BeginPlay()
{
	Super::BeginPlay();

	// Engine cylinder native size: 100cm tall, 50cm radius.
	if (PillarMesh)
	{
		PillarMesh->SetWorldScale3D(FVector(PillarRadius / 50.0f, PillarRadius / 50.0f, PillarHeight / 100.0f));
	}

	if (HasAuthority())
	{
		const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		DespawnTime = Now + PillarLifetime;
	}
}

void AProjectCompTitanPillar::Server_Initialize(ETeamID InCasterTeam)
{
	if (!HasAuthority())
	{
		return;
	}
	CasterTeam = InCasterTeam;
	UE_LOG(LogProjectComp, Log, TEXT("[Loom] Titan Pillar placed at %s by team %d."),
		*GetActorLocation().ToCompactString(), static_cast<int32>(InCasterTeam));
	Server_RunCrushSweep();
}

void AProjectCompTitanPillar::Server_RunCrushSweep()
{
	UWorld* World = GetWorld();
	if (!World || !HasAuthority())
	{
		return;
	}
	AProjectCompGameMode* GM = World->GetAuthGameMode<AProjectCompGameMode>();
	if (!GM)
	{
		return;
	}

	AController* CasterController = Cast<AController>(GetOwner());
	const FVector Center = GetActorLocation();
	const float CrushRadiusSq = CrushRadius * CrushRadius;

	// Wall traces ignore the pillar itself + every live pawn (only world geometry
	// should count as "a wall behind them").
	TArray<AActor*> Ignore;
	Ignore.Reserve(16);
	Ignore.Add(this);
	for (const TWeakObjectPtr<AProjectCompCharacter>& WeakChar : GM->GetLiveCharacters())
	{
		if (AProjectCompCharacter* C = WeakChar.Get())
		{
			Ignore.Add(C);
		}
	}

	for (const TWeakObjectPtr<AProjectCompCharacter>& WeakChar : GM->GetLiveCharacters())
	{
		AProjectCompCharacter* Char = WeakChar.Get();
		if (!Char)
		{
			continue;
		}
		AProjectCompPlayerState* PS = Char->GetPlayerState<AProjectCompPlayerState>();
		if (!PS || !PS->IsAlive())
		{
			continue;
		}
		const ETeamID Team = PS->GetTeamID();
		if (Team == ETeamID::Spectator)
		{
			continue;
		}
		if (!bAffectAllTeams && Team == CasterTeam)
		{
			continue;  // teammates aren't shoved/crushed — the pillar is just cover for them
		}

		const FVector CharLoc = Char->GetActorLocation();
		FVector Flat = CharLoc - Center;
		Flat.Z = 0.0f;
		if (Flat.SizeSquared() > CrushRadiusSq)
		{
			continue;  // outside the affected radius
		}

		const float CapsuleR = Char->GetCapsuleComponent()
			? Char->GetCapsuleComponent()->GetScaledCapsuleRadius()
			: 34.0f;
		const float ClearDist = PillarRadius + CapsuleR + 10.0f;  // surface + capsule + skin
		const float CurrentDist = Flat.Size();
		if (CurrentDist >= ClearDist)
		{
			continue;  // already clear of the cylinder — no push needed
		}

		const FVector OutDir = (CurrentDist > KINDA_SMALL_NUMBER)
			? (Flat / CurrentDist)
			: FVector(1.0f, 0.0f, 0.0f);  // dead-center: pick an arbitrary outward dir
		FVector SafeTarget = Center + OutDir * ClearDist;
		SafeTarget.Z = CharLoc.Z;

		// Is there a wall between them and where they'd need to stand?
		FCollisionQueryParams Params(SCENE_QUERY_STAT(TitanPillarCrush), /*bTraceComplex*/ false);
		Params.AddIgnoredActors(Ignore);
		FHitResult WallHit;
		const bool bWallBehind = World->LineTraceSingleByChannel(WallHit, CharLoc, SafeTarget, ECC_Visibility, Params);

		if (bWallBehind)
		{
			// Pinned between the pillar and a wall — instant lethal. FF scaling in
			// Character::TakeDamage protects teammates (and they're already skipped).
			// Call through the public AActor::TakeDamage (the Character override is
			// protected); virtual dispatch still routes to the Character version.
			FDamageEvent DamageEvent;
			static_cast<AActor*>(Char)->TakeDamage(LethalDamage, DamageEvent, CasterController, this);
			UE_LOG(LogProjectComp, Log, TEXT("[Loom] Titan Pillar CRUSHED %s against geometry."),
				*GetNameSafe(Char));
		}
		else
		{
			// Clear path — shove them out to the cylinder surface.
			Char->SetActorLocation(SafeTarget, /*bSweep*/ false);
		}
	}
}

void AProjectCompTitanPillar::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!HasAuthority())
	{
		return;
	}
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	if (Now >= DespawnTime)
	{
		Destroy();
	}
}
