// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompFlashDome.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompPlayerState.h"
#include "ProjectComp.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "EngineUtils.h"
#include "UObject/ConstructorHelpers.h"
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

	// Center-pop marker. Attached to DomeMesh so it moves with the dome root,
	// but absolute-scaled so BP_FlashDome scaling up DomeMesh (e.g. 16x to
	// match DomeRadius=800) doesn't blow up this marker too.
	CenterBallMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CenterBallMesh"));
	CenterBallMesh->SetupAttachment(DomeMesh);
	CenterBallMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CenterBallMesh->SetUsingAbsoluteScale(true);
	CenterBallMesh->SetRelativeScale3D(FVector(0.3f, 0.3f, 0.3f));

	// Default to engine sphere so the marker is visible out of the box. BP can
	// override the mesh + material to recolor without touching C++.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere"));
	if (SphereFinder.Succeeded())
	{
		CenterBallMesh->SetStaticMesh(SphereFinder.Object);
	}
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
		const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		WindupEndTime = Now + WindupDuration;
		DespawnTime   = Now + DomeLifetime;
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

	// Windup gate — dome exists but flash is inert until the fuse expires.
	if (Now < WindupEndTime)
	{
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

		// Facing check: dot the target's view direction against the direction to
		// the dome center. If they're looking roughly at it, full flash; otherwise
		// they catch a brief "half-blind" peripheral flash. Both still latch this
		// pawn so the dome can't repeatedly re-flash them within its lifetime.
		// TODO(loom-tuning): promote thresholds + half-flash durations to UPROPERTYs
		// in a future restart-pass so designers can tune per-dome.
		constexpr float kFacingDotThreshold     = 0.5f;   // ~60deg cone, generous "looking at" read
		constexpr float kHalfFlashFullWhite     = 0.05f;
		constexpr float kHalfFlashFade          = 0.05f;  // total 0.1s peripheral blink

		const FVector ViewDir = PC->GetControlRotation().Vector();
		const FVector ToCenter = (DomeCenter - CamLoc).GetSafeNormal();
		const float FacingDot = FVector::DotProduct(ViewDir, ToCenter);
		const bool bFacingCenter = FacingDot >= kFacingDotThreshold;

		const float FullWhite = bFacingCenter ? FlashFullWhiteDuration : kHalfFlashFullWhite;
		const float Fade      = bFacingCenter ? FlashFadeDuration      : kHalfFlashFade;

		PS->Server_ApplyFlash(FullWhite, Fade);
		AlreadyFlashed.Add(Pawn);

		UE_LOG(LogProjectComp, Log,
			TEXT("[Loom] FlashDome %s-flashed pawn=%s (team=%d) for %.2fs."),
			bFacingCenter ? TEXT("FULL") : TEXT("HALF"),
			*GetNameSafe(Pawn), static_cast<int32>(PawnTeam),
			FullWhite + Fade);
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
