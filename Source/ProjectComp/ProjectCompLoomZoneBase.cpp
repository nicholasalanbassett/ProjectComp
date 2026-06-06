// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompLoomZoneBase.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompGameMode.h"
#include "ProjectComp.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "Net/UnrealNetwork.h"

// Shared debug toggle for every Loom zone's volume viz. Independent per-system
// CVar, matching the grenade/sound-dome debug toggles.
static TAutoConsoleVariable<int32> CVarLoomZoneDebugDraw(
	TEXT("pc.LoomZone.Debug"),
	1,
	TEXT("Draw Loom-ability zone volumes (per-ability colored sphere). 0=off, 1=on"),
	ECVF_Default);

AProjectCompLoomZoneBase::AProjectCompLoomZoneBase()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	bReplicates = true;
	SetReplicateMovement(false);  // stationary once placed

	ZoneMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ZoneMesh"));
	ZoneMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetRootComponent(ZoneMesh);

	ZoneSphere = CreateDefaultSubobject<USphereComponent>(TEXT("ZoneSphere"));
	ZoneSphere->SetupAttachment(ZoneMesh);
	ZoneSphere->InitSphereRadius(ZoneRadius);
	// Query-only — never blocks movement, sight, or bullets. Overlaps pawns only.
	ZoneSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ZoneSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	ZoneSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	ZoneSphere->SetGenerateOverlapEvents(false);
}

void AProjectCompLoomZoneBase::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AProjectCompLoomZoneBase, CasterTeam);
}

void AProjectCompLoomZoneBase::BeginPlay()
{
	Super::BeginPlay();

	if (ZoneSphere)
	{
		ZoneSphere->SetSphereRadius(ZoneRadius, /*bUpdateOverlaps*/ false);
	}

	if (HasAuthority())
	{
		const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		WindupEndTime = Now + WindupDuration;
		DespawnTime   = Now + ZoneLifetime;

		if (UWorld* World = GetWorld())
		{
			CachedAuthGameMode = World->GetAuthGameMode<AProjectCompGameMode>();
		}

		// Reserve the per-tick scratch once (16 = 5v5 + caster + 10v10 headroom).
		IgnoreActorsScratch.Reserve(16);
		AlreadyAffected.Reserve(16);
	}
}

void AProjectCompLoomZoneBase::Server_Initialize(ETeamID InCasterTeam)
{
	if (!HasAuthority())
	{
		return;
	}
	CasterTeam = InCasterTeam;
}

void AProjectCompLoomZoneBase::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// POC volume viz on all peers (compiled out in shipping). Replace with
	// authored FX on the BP subclass.
#if !UE_BUILD_SHIPPING
	if (CVarLoomZoneDebugDraw.GetValueOnGameThread() != 0)
	{
		DrawDebugSphere(GetWorld(), GetActorLocation(), ZoneRadius, 24,
			DebugColor, /*bPersistent*/ false, /*LifeTime*/ -1.0f, 0, /*Thickness*/ 3.0f);
	}
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

	// Windup gate — zone exists but is inert until the fuse expires.
	if (Now < WindupEndTime)
	{
		return;
	}

	if (!bActivated)
	{
		bActivated = true;
		Server_OnActivate();
		// Server_OnActivate may Destroy() this actor (e.g. a one-shot scan).
		if (!IsValid(this))
		{
			return;
		}
	}

	Server_TickZone(DeltaSeconds);
}

bool AProjectCompLoomZoneBase::IsPointInZone(const FVector& WorldPoint) const
{
	return (WorldPoint - GetActorLocation()).SizeSquared() <= (ZoneRadius * ZoneRadius);
}

void AProjectCompLoomZoneBase::BuildLiveCharacterIgnoreList()
{
	IgnoreActorsScratch.Reset();
	IgnoreActorsScratch.Add(const_cast<AProjectCompLoomZoneBase*>(this));
	if (AProjectCompGameMode* GM = CachedAuthGameMode.Get())
	{
		for (const TWeakObjectPtr<AProjectCompCharacter>& WeakChar : GM->GetLiveCharacters())
		{
			if (AProjectCompCharacter* Char = WeakChar.Get())
			{
				IgnoreActorsScratch.Add(Char);
			}
		}
	}
}

bool AProjectCompLoomZoneBase::HasClearLineOfSight(const FVector& From, const FVector& To, bool bAlsoBlockedBySmoke) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FCollisionQueryParams Params(NAME_None, /*bTraceComplex*/ false);
	Params.AddIgnoredActors(IgnoreActorsScratch);

	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Params))
	{
		return false;  // wall in the way
	}
	if (bAlsoBlockedBySmoke
		&& World->LineTraceSingleByChannel(Hit, From, To, ECC_VisionBlock, Params))
	{
		return false;  // smoke in the way
	}
	return true;
}
