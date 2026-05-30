// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompSoundDome.generated.h"

class UStaticMeshComponent;
class USphereComponent;

/**
 * MC (Midrange/Control) Loom ability: a server-spawned audio-dampening dome.
 * This is the re-homed "Sound grenade" — the dead-player Loom cast now places
 * the dampen field instead of a thrown grenade.
 *
 * Behavior: while alive, the dome marks a spherical zone in which enemy-created
 * audio should be dampened. The actual per-listener audio mixing is future work
 * (it was a stub on the old Sound grenade too); this actor owns the zone volume,
 * lifetime, and replicated state a mixer will query via GetOverlappingActors on
 * ZoneSphere / IsPointInZone.
 *
 * Lifecycle: spawned in AProjectCompGameMode::Server_RequestAbilityCast at the
 * cast target. Ticks until DomeLifetime elapses, then Destroy()s. Mirrors
 * AProjectCompFlashDome's spawn/despawn shape.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompSoundDome : public AActor
{
	GENERATED_BODY()

public:
	AProjectCompSoundDome();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Sphere radius (cm) of the dampening zone. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Sound Dome", meta = (ClampMin = "100.0"))
	float DomeRadius = 800.0f;

	/** Seconds the dome persists before despawning. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Sound Dome", meta = (ClampMin = "0.1"))
	float DomeLifetime = 10.0f;

	/** Z offset (cm) added to the cast target when spawning, so a floor-level
	 *  target lifts the zone center off the ground. Read off the CDO by the
	 *  GameMode before SpawnActor (same pattern as FlashDome). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Sound Dome", meta = (ClampMin = "0.0"))
	float SpawnZLift = 100.0f;

	/** Server: configure the dome with the casting team. */
	void Server_Initialize(ETeamID InCasterTeam);

	UFUNCTION(BlueprintPure, Category = "Sound Dome")
	float GetZoneRadius() const { return DomeRadius; }

	/** True if WorldPoint is inside the dampening sphere. For a future audio
	 *  mixer to gate enemy audio per-listener. */
	UFUNCTION(BlueprintPure, Category = "Sound Dome")
	bool IsPointInZone(const FVector& WorldPoint) const;

protected:
	virtual void BeginPlay() override;

	/** Cosmetic dome mesh — populate in BP. C++ leaves it empty so the POC runs
	 *  asset-free. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UStaticMeshComponent* DomeMesh;

	/** Query-only zone volume — a future audio system pulls overlaps from this. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	USphereComponent* ZoneSphere;

	/** Casting team — replicated so clients can later condition local FX. */
	UPROPERTY(Replicated)
	ETeamID CasterTeam = ETeamID::Spectator;

	/** Server-time the dome despawns. Set in BeginPlay from DomeLifetime. */
	float DespawnTime = 0.0f;
};
