// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompInfoGrenade.h"
#include "ProjectComp.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompGameMode.h"
#include "ProjectCompGameState.h"
#include "ProjectCompPlayerState.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"

AProjectCompInfoGrenade::AProjectCompInfoGrenade()
{
	PrimaryActorTick.bCanEverTick = false;

	DeviceMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DeviceMesh"));
	SetRootComponent(DeviceMesh);
	DeviceMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// Pure-visual scan volume — non-blocking. The actual count uses a manual
	// per-pawn distance check (cheaper + more predictable than overlap events).
	ScanVolume = CreateDefaultSubobject<USphereComponent>(TEXT("ScanVolume"));
	ScanVolume->SetupAttachment(DeviceMesh);
	ScanVolume->InitSphereRadius(ScanRadius);
	ScanVolume->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AProjectCompInfoGrenade::BeginPlay()
{
	Super::BeginPlay();

	if (ScanVolume)
	{
		// Mirror BP-tuned ScanRadius onto the visual volume.
		ScanVolume->SetSphereRadius(ScanRadius, /*bUpdateOverlaps*/ false);
	}

	if (HasAuthority())
	{
		GetWorldTimerManager().SetTimer(FuseTimer, [this]()
		{
			Server_Detonate();
		}, FuseDuration, /*bLoop*/ false);

		UE_LOG(LogProjectComp, Log,
			TEXT("[InfoGrenade] %s placed at %s — fuse %.1fs, scan radius %.0fcm."),
			*GetName(), *GetActorLocation().ToCompactString(), FuseDuration, ScanRadius);
	}
}

void AProjectCompInfoGrenade::HandleDetonationGameplay()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FVector ScanCenter = GetActorLocation();
	const ETeamID PlacerTeam = GetOwnerTeam();
	if (PlacerTeam == ETeamID::Spectator)
	{
		return;
	}

	int32 EnemyCount = 0;
	const float RadiusSq = ScanRadius * ScanRadius;

	// Iterate the LiveCharacters registry — cache-friendly, no TActorIterator
	// allocation. Mirror the FlashDome / FlashGrenade scan pattern.
	if (AProjectCompGameMode* GM = World->GetAuthGameMode<AProjectCompGameMode>())
	{
		for (const TWeakObjectPtr<AProjectCompCharacter>& WeakChar : GM->GetLiveCharacters())
		{
			AProjectCompCharacter* Char = WeakChar.Get();
			if (!Char)
			{
				continue;
			}

			const APlayerController* PC = Cast<APlayerController>(Char->GetController());
			AProjectCompPlayerState* PS = PC ? PC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
			if (!PS || !PS->IsAlive())
			{
				continue;
			}

			// Enemies only.
			const ETeamID Team = PS->GetTeamID();
			if (Team == ETeamID::Spectator || Team == PlacerTeam)
			{
				continue;
			}

			if ((Char->GetActorLocation() - ScanCenter).SizeSquared() <= RadiusSq)
			{
				++EnemyCount;
			}
		}
	}

	UE_LOG(LogProjectComp, Log,
		TEXT("[InfoGrenade] %s scan complete: %d enemy/ies inside r=%.0fcm at %s."),
		*GetName(), EnemyCount, ScanRadius, *ScanCenter.ToCompactString());

	// Broadcast to all clients. Each client decides locally how to render —
	// placer's team sees the count, enemies inside see a "you were scanned" tell.
	if (AProjectCompGameState* GS = World->GetGameState<AProjectCompGameState>())
	{
		GS->Multicast_OnInfoScan(EnemyCount, PlacerTeam, ScanCenter, ScanRadius);
	}
}
