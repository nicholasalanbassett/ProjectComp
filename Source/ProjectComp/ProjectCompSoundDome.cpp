// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompSoundDome.h"
#include "ProjectComp.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Net/UnrealNetwork.h"

// Toggle for the dampen-zone viz (muted blue wireframe sphere). Independent
// per-ability CVar, matching the other Loom/grenade debug toggles.
static TAutoConsoleVariable<int32> CVarSoundDomeDebugDraw(
	TEXT("pc.SoundDome.Debug"),
	1,
	TEXT("Draw sound dome dampen-zone viz (muted blue sphere). 0=off, 1=on"),
	ECVF_Default);

AProjectCompSoundDome::AProjectCompSoundDome()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	bReplicates = true;
	SetReplicateMovement(false);  // stationary once placed

	DomeMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DomeMesh"));
	DomeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetRootComponent(DomeMesh);

	ZoneSphere = CreateDefaultSubobject<USphereComponent>(TEXT("ZoneSphere"));
	ZoneSphere->SetupAttachment(DomeMesh);
	ZoneSphere->InitSphereRadius(DomeRadius);
	// Query-only — a future audio mixer pulls overlaps on demand. Overlap pawns
	// only; never blocks movement, sight, or bullets.
	ZoneSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ZoneSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	ZoneSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	ZoneSphere->SetGenerateOverlapEvents(false);
}

void AProjectCompSoundDome::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AProjectCompSoundDome, CasterTeam);
}

void AProjectCompSoundDome::BeginPlay()
{
	Super::BeginPlay();
	if (ZoneSphere)
	{
		ZoneSphere->SetSphereRadius(DomeRadius, /*bUpdateOverlaps*/ false);
	}
	if (HasAuthority())
	{
		const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		DespawnTime = Now + DomeLifetime;
	}
}

void AProjectCompSoundDome::Server_Initialize(ETeamID InCasterTeam)
{
	if (!HasAuthority())
	{
		return;
	}
	CasterTeam = InCasterTeam;
	UE_LOG(LogProjectComp, Log,
		TEXT("[SoundDome] Placed at %s by team %d — radius %.0fcm, lifetime %.1fs."),
		*GetActorLocation().ToCompactString(), static_cast<int32>(InCasterTeam),
		DomeRadius, DomeLifetime);
}

void AProjectCompSoundDome::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// POC visual: muted-blue debug sphere on all peers so testers can see the
	// dampen volume in PIE without an art asset. Compiled out in shipping.
#if !UE_BUILD_SHIPPING
	if (CVarSoundDomeDebugDraw.GetValueOnGameThread() != 0)
	{
		DrawDebugSphere(GetWorld(), GetActorLocation(), DomeRadius, 24,
			FColor(80, 140, 220), /*bPersistent*/ false, /*LifeTime*/ -1.0f, 0, /*Thickness*/ 3.0f);
	}
#endif

	if (!HasAuthority())
	{
		return;
	}

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	if (Now >= DespawnTime)
	{
		UE_LOG(LogProjectComp, Log, TEXT("[SoundDome] %s lifetime expired — despawning."), *GetName());
		Destroy();
	}
}

bool AProjectCompSoundDome::IsPointInZone(const FVector& WorldPoint) const
{
	return (WorldPoint - GetActorLocation()).SizeSquared() <= (DomeRadius * DomeRadius);
}
