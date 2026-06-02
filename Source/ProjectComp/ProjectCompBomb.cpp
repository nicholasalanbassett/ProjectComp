// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompBomb.h"
#include "ProjectComp.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompPlayerState.h"
#include "ProjectCompGameMode.h"
#include "ProjectCompGameState.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"

AProjectCompBomb::AProjectCompBomb()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	SetReplicateMovement(true);

	// Pickup sphere is the root (unscaled), so its radius stays true while the
	// cube mesh under it is scaled down.
	PickupSphere = CreateDefaultSubobject<USphereComponent>(TEXT("PickupSphere"));
	SetRootComponent(PickupSphere);
	PickupSphere->InitSphereRadius(90.0f);
	PickupSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	PickupSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	PickupSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	// Visual mesh + material are assigned on the BP subclass (BP_Bomb — set it to a
	// black cube). Empty in C++. Cosmetic only; the pickup sphere does the gameplay.
	BombMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BombMesh"));
	BombMesh->SetupAttachment(PickupSphere);
	BombMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AProjectCompBomb::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AProjectCompBomb, State);
	DOREPLIFETIME(AProjectCompBomb, Carrier);
	DOREPLIFETIME(AProjectCompBomb, DetonationEndTime);
	DOREPLIFETIME(AProjectCompBomb, PlantProgress);
	DOREPLIFETIME(AProjectCompBomb, DefuseProgress);
}

void AProjectCompBomb::BeginPlay()
{
	Super::BeginPlay();

	// Two pickup paths, both supported: walk-over (this overlap) AND F-interact
	// (look-at, close range — see AProjectCompCharacter::Server_TryInteract).
	if (HasAuthority() && PickupSphere)
	{
		PickupSphere->OnComponentBeginOverlap.AddDynamic(this, &AProjectCompBomb::OnPickupOverlap);
	}
	RefreshVisual();
}

void AProjectCompBomb::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority())
	{
		return;
	}
	switch (State)
	{
	case EBombState::Carried: Server_TickPlant(DeltaSeconds); break;
	case EBombState::Planted: Server_TickDefuse(DeltaSeconds); break;
	default: break;
	}
}

void AProjectCompBomb::Server_TickPlant(float DeltaSeconds)
{
	AProjectCompCharacter* C = Carrier;
	const AProjectCompPlayerState* PS = C ? C->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	AProjectCompGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AProjectCompGameMode>() : nullptr;
	const AProjectCompGameState* GS = GetWorld() ? GetWorld()->GetGameState<AProjectCompGameState>() : nullptr;

	const bool bRoundActive = GS && GS->GetCurrentPhase() == ERoundPhase::RoundActive;
	const bool bCanPlant = bRoundActive && C && PS && PS->IsAlive() && C->IsUseHeld()
		&& GM && GM->IsActorInBombSite(C);

	if (bCanPlant)
	{
		PlantProgress = FMath::Clamp(PlantProgress + DeltaSeconds / FMath::Max(PlantDuration, 0.01f), 0.0f, 1.0f);
		if (PlantProgress >= 1.0f)
		{
			C->Server_NotifyInteractionComplete();
			Server_CompletePlant();
		}
	}
	else if (PlantProgress > 0.0f)
	{
		PlantProgress = 0.0f;
	}
}

void AProjectCompBomb::Server_CompletePlant()
{
	const FVector PlantLoc = Carrier ? Carrier->GetActorLocation() : GetActorLocation();
	Carrier = nullptr;
	PlantProgress = 0.0f;
	DefuseProgress = 0.0f;
	const UWorld* W = GetWorld();
	DetonationEndTime = (W ? W->GetTimeSeconds() : 0.0f) + DetonationDuration;
	DetachAndPlace(PlantLoc);
	Server_SetState(EBombState::Planted);
	UE_LOG(LogProjectComp, Log, TEXT("[Bomb] Planted at %s — detonates in %.0fs."),
		*PlantLoc.ToCompactString(), DetonationDuration);
}

void AProjectCompBomb::Server_TickDefuse(float DeltaSeconds)
{
	const UWorld* W = GetWorld();

	// Auto-detonate when the timer elapses.
	if (W && W->GetTimeSeconds() >= DetonationEndTime)
	{
		DefuseProgress = 0.0f;
		Server_SetState(EBombState::Detonated);
		UE_LOG(LogProjectComp, Log, TEXT("[Bomb] Detonated."));
		return;
	}

	// Find an alive defender within DefuseRadius holding interact.
	AProjectCompCharacter* Defuser = nullptr;
	const FVector BombLoc = GetActorLocation();
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		AProjectCompCharacter* Ch = PC ? Cast<AProjectCompCharacter>(PC->GetPawn()) : nullptr;
		const AProjectCompPlayerState* PS = Ch ? Ch->GetPlayerState<AProjectCompPlayerState>() : nullptr;
		if (!Ch || !PS || !PS->IsAlive() || PS->GetTeamID() != ETeamID::Defenders || !Ch->IsUseHeld())
		{
			continue;
		}
		if (FVector::DistSquared(Ch->GetActorLocation(), BombLoc) <= FMath::Square(DefuseRadius))
		{
			Defuser = Ch;
			break;
		}
	}

	if (Defuser)
	{
		DefuseProgress = FMath::Clamp(DefuseProgress + DeltaSeconds / FMath::Max(DefuseDuration, 0.01f), 0.0f, 1.0f);
		if (DefuseProgress >= 1.0f)
		{
			Defuser->Server_NotifyInteractionComplete();
			DefuseProgress = 0.0f;
			Server_SetState(EBombState::Defused);
			UE_LOG(LogProjectComp, Log, TEXT("[Bomb] Defused."));
		}
	}
	else if (DefuseProgress > 0.0f)
	{
		DefuseProgress = 0.0f;
	}
}

void AProjectCompBomb::OnPickupOverlap(UPrimitiveComponent* /*OverlappedComp*/, AActor* OtherActor,
	UPrimitiveComponent* /*OtherComp*/, int32 /*OtherBodyIndex*/, bool /*bFromSweep*/, const FHitResult& /*Sweep*/)
{
	if (!HasAuthority() || State != EBombState::Available)
	{
		return;
	}
	AProjectCompCharacter* Ch = Cast<AProjectCompCharacter>(OtherActor);
	const AProjectCompPlayerState* PS = Ch ? Ch->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (Ch && PS && PS->IsAlive() && PS->GetTeamID() == ETeamID::Attackers)
	{
		Server_Pickup(Ch);
	}
}

void AProjectCompBomb::Server_Pickup(AProjectCompCharacter* NewCarrier)
{
	if (!NewCarrier)
	{
		return;
	}
	Carrier = NewCarrier;
	PlantProgress = 0.0f;
	AttachToCarrier();
	Server_SetState(EBombState::Carried);
	UE_LOG(LogProjectComp, Log, TEXT("[Bomb] Picked up by '%s'."), *GetNameSafe(NewCarrier));
}

void AProjectCompBomb::Server_Drop(const FVector& DropLocation)
{
	if (State != EBombState::Carried)
	{
		return;
	}
	Carrier = nullptr;
	PlantProgress = 0.0f;
	DetachAndPlace(DropLocation);
	Server_SetState(EBombState::Available);
	UE_LOG(LogProjectComp, Log, TEXT("[Bomb] Dropped at %s."), *DropLocation.ToCompactString());
}

void AProjectCompBomb::Server_ResetToLocation(const FVector& WorldLocation)
{
	Carrier = nullptr;
	PlantProgress = 0.0f;
	DefuseProgress = 0.0f;
	DetonationEndTime = 0.0f;
	DetachAndPlace(WorldLocation);
	Server_SetState(EBombState::Available);
}

void AProjectCompBomb::Server_SetState(EBombState NewState)
{
	State = NewState;
	OnRep_State();  // mirror client-side visual logic on the server (listen-server host)
}

void AProjectCompBomb::AttachToCarrier()
{
	if (!Carrier)
	{
		return;
	}
	AttachToComponent(Carrier->GetRootComponent(), FAttachmentTransformRules::SnapToTargetIncludingScale);
	SetActorRelativeLocation(FVector(0.0f, 0.0f, 60.0f));  // float a bit above the carrier
}

void AProjectCompBomb::DetachAndPlace(const FVector& WorldLocation)
{
	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	SetActorLocation(WorldLocation + FVector(0.0f, 0.0f, 20.0f));  // sit just off the ground
}

void AProjectCompBomb::OnRep_State()
{
	RefreshVisual();
}

void AProjectCompBomb::OnRep_Carrier()
{
	RefreshVisual();
}

void AProjectCompBomb::RefreshVisual()
{
	// Hide once terminal; visible otherwise.
	const bool bHide = (State == EBombState::Defused || State == EBombState::Detonated);
	if (BombMesh)
	{
		BombMesh->SetVisibility(!bHide);
	}
	// Pickup only while Available (server-only overlap gate).
	if (HasAuthority() && PickupSphere)
	{
		PickupSphere->SetCollisionEnabled(State == EBombState::Available
			? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
	}
}
