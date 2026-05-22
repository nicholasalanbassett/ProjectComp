// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompBombSite.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompPlayerState.h"
#include "ProjectComp.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

AProjectCompBombSite::AProjectCompBombSite()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	bReplicates = true;

	Trigger = CreateDefaultSubobject<UBoxComponent>(TEXT("Trigger"));
	Trigger->SetBoxExtent(FVector(200.f, 200.f, 100.f));  // 4m × 4m × 2m default
	Trigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Trigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	Trigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	// Don't block dead pawns either — the trigger overlaps live + dead pawns,
	// but plant/defuse logic filters to alive only.
	Trigger->SetCollisionResponseToChannel(ECC_GameTraceChannel2, ECR_Overlap);
	SetRootComponent(Trigger);

	MarkerMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MarkerMesh"));
	MarkerMesh->SetupAttachment(Trigger);
	MarkerMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AProjectCompBombSite::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AProjectCompBombSite, bIsPlanted);
	DOREPLIFETIME(AProjectCompBombSite, bIsDetonated);
	DOREPLIFETIME(AProjectCompBombSite, bIsDefused);
	DOREPLIFETIME(AProjectCompBombSite, DetonationEndTime);
	DOREPLIFETIME(AProjectCompBombSite, PlantProgress);
	DOREPLIFETIME(AProjectCompBombSite, DefuseProgress);
}

void AProjectCompBombSite::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		Trigger->OnComponentBeginOverlap.AddDynamic(this, &AProjectCompBombSite::OnTriggerBeginOverlap);
		Trigger->OnComponentEndOverlap.AddDynamic(this, &AProjectCompBombSite::OnTriggerEndOverlap);
	}
}

void AProjectCompBombSite::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!HasAuthority())
	{
		return;
	}

	// Terminal — nothing to do until the round resets us.
	if (IsTerminal())
	{
		return;
	}

	if (!bIsPlanted)
	{
		TickPlanting(DeltaTime);
	}
	else
	{
		TickDefusing(DeltaTime);

		// Detonation countdown.
		const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		if (Now >= DetonationEndTime)
		{
			Detonate();
		}
	}
}

void AProjectCompBombSite::OnTriggerBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	if (APawn* Pawn = Cast<APawn>(OtherActor))
	{
		PawnsInside.AddUnique(Pawn);
	}
}

void AProjectCompBombSite::OnTriggerEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
	if (APawn* Pawn = Cast<APawn>(OtherActor))
	{
		PawnsInside.Remove(Pawn);
	}
}

void AProjectCompBombSite::OnRep_IsPlanted()
{
	// Hook for client-side audio/VFX cues. Empty for MVP — HUD reads bIsPlanted
	// directly via NativeTick polling.
}

void AProjectCompBombSite::TickPlanting(float DeltaTime)
{
	APawn* ActivePlanter = FindActiveInteractor(ETeamID::Attackers);

	if (ActivePlanter)
	{
		// Convert held-progress to [0..1] over PlantDuration seconds.
		const float Delta = DeltaTime / FMath::Max(PlantDuration, 0.01f);
		PlantProgress = FMath::Clamp(PlantProgress + Delta, 0.0f, 1.0f);

		if (PlantProgress >= 1.0f)
		{
			CompletePlant();
		}
	}
	else if (PlantProgress > 0.0f)
	{
		// No active planter — reset immediately. Players must hold the full duration without releasing.
		PlantProgress = 0.0f;
	}
}

void AProjectCompBombSite::TickDefusing(float DeltaTime)
{
	APawn* ActiveDefuser = FindActiveInteractor(ETeamID::Defenders);

	if (ActiveDefuser)
	{
		const float Delta = DeltaTime / FMath::Max(DefuseDuration, 0.01f);
		DefuseProgress = FMath::Clamp(DefuseProgress + Delta, 0.0f, 1.0f);

		if (DefuseProgress >= 1.0f)
		{
			CompleteDefuse();
		}
	}
	else if (DefuseProgress > 0.0f)
	{
		// No active defuser — reset immediately. Players must hold the full duration without releasing.
		DefuseProgress = 0.0f;
	}
}

void AProjectCompBombSite::CompletePlant()
{
	bIsPlanted = true;
	PlantProgress = 0.0f;
	DetonationEndTime = (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f) + DetonationDuration;
	OnRep_IsPlanted();  // Run client-side hook on server too.
	UE_LOG(LogProjectComp, Log, TEXT("Bomb site %s: PLANTED. Detonation in %.1fs."), *Label.ToString(), DetonationDuration);
}

void AProjectCompBombSite::CompleteDefuse()
{
	bIsDefused = true;
	DefuseProgress = 0.0f;
	UE_LOG(LogProjectComp, Log, TEXT("Bomb site %s: DEFUSED."), *Label.ToString());
}

void AProjectCompBombSite::Detonate()
{
	bIsDetonated = true;
	UE_LOG(LogProjectComp, Log, TEXT("Bomb site %s: DETONATED."), *Label.ToString());
}

APawn* AProjectCompBombSite::FindActiveInteractor(ETeamID TeamFilter) const
{
	for (const TObjectPtr<APawn>& PawnPtr : PawnsInside)
	{
		APawn* Pawn = PawnPtr.Get();
		if (!Pawn)
		{
			continue;
		}

		const AProjectCompPlayerState* PS = Pawn->GetPlayerState<AProjectCompPlayerState>();
		if (!PS || !PS->IsAlive() || PS->GetTeamID() != TeamFilter)
		{
			continue;
		}

		const AProjectCompCharacter* Char = Cast<AProjectCompCharacter>(Pawn);
		if (Char && Char->IsUseHeld())
		{
			return Pawn;
		}
	}
	return nullptr;
}

void AProjectCompBombSite::Server_ResetForNewRound()
{
	if (!HasAuthority())
	{
		return;
	}
	bIsPlanted = false;
	bIsDetonated = false;
	bIsDefused = false;
	DetonationEndTime = 0.0f;
	PlantProgress = 0.0f;
	DefuseProgress = 0.0f;
}
