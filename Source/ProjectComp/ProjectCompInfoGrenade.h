// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProjectCompGrenadeBase.h"
#include "ProjectCompInfoGrenade.generated.h"

class USphereComponent;
class UStaticMeshComponent;

/**
 * Info grenade (Midrange Control). Map-placed util that, after FuseDuration
 * seconds, snapshots the count of enemy players inside ScanRadius and
 * broadcasts the result via GameState::Multicast_OnInfoScan.
 *
 * Design contract:
 *  - Count only, no positions reported.
 *  - Enemies inside the scan are aware they were scanned (the broadcast
 *    location + radius lets each enemy client decide locally whether to
 *    show the "you were scanned" tell).
 *  - One-shot detonate; not toggleable / not repositionable.
 *
 * Lifecycle:
 *  - SpawnActor (by GrenadeComponent)
 *  - Server_Initialize stamps team + type
 *  - Server_InitializeExtra (default no-op) — bAltFire ignored for INFO
 *  - BeginPlay schedules the fuse timer
 *  - Fuse fires → HandleDetonationGameplay runs the scan + multicast
 *  - Base class queues Destroy() after CleanupDelay
 */
UCLASS()
class PROJECTCOMP_API AProjectCompInfoGrenade : public AProjectCompGrenadeBase
{
	GENERATED_BODY()

public:
	AProjectCompInfoGrenade();

protected:
	virtual void BeginPlay() override;
	virtual void HandleDetonationGameplay() override;

	/** Cosmetic mesh slot — set in BP. C++ leaves it empty. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UStaticMeshComponent* DeviceMesh;

	/** Visual indicator of the scan volume — pure cosmetic, non-blocking. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	USphereComponent* ScanVolume;

	/** Seconds between place and scan. User design said "1/2/3s delay (TBD)" —
	 *  defaulting to 2s as the middle option; tune in BP defaults. */
	UPROPERTY(EditDefaultsOnly, Category = "Info", meta = (ClampMin = "0.1"))
	float FuseDuration = 2.0f;

	/** Radius (cm) inside which enemy players are counted. */
	UPROPERTY(EditDefaultsOnly, Category = "Info", meta = (ClampMin = "100.0"))
	float ScanRadius = 1000.0f;

private:
	/** Server-only fuse timer handle. */
	FTimerHandle FuseTimer;
};
