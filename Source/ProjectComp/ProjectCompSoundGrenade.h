// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProjectCompGrenadeBase.h"
#include "ProjectCompSoundGrenade.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class AProjectCompPlayerState;

/**
 * Sound grenade (Midrange Control). Map-placed circle util that dampens enemy-
 * created audio inside its radius while active. Toggleable by the ORIGINAL
 * placer; repositionable ONCE after the placer's first death + respawn.
 *
 * Audio mixing itself isn't implemented in this pass — the grenade exposes
 * IsActive() + the AOE sphere so a future audio system (e.g. per-listener
 * sound class volume mixer keyed on opposing-team grenade presence) can
 * query state. For now this class manages the lifecycle and the toggle/
 * reposition state machine.
 *
 * Lifecycle:
 *  - Spawn → bIsActive=true (toggle is opt-out, not opt-in)
 *  - Original placer toggles via PlayerController.Server_ToggleSoundGrenade
 *  - Original placer repositions via Server_RepositionSoundGrenade
 *      (max RepositionsAllowed times — design says 1)
 *  - Placer becomes permanent-ghost → Destroy()
 *  - Round ends (not RoundActive) → Destroy() (StartPreRound clears all)
 */
UCLASS()
class PROJECTCOMP_API AProjectCompSoundGrenade : public AProjectCompGrenadeBase
{
	GENERATED_BODY()

public:
	AProjectCompSoundGrenade();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void Server_InitializeExtra(bool bAltFire, const FVector& AimDirection) override;

	UFUNCTION(BlueprintPure, Category = "Sound")
	bool IsActive() const { return bIsActive; }

	UFUNCTION(BlueprintPure, Category = "Sound")
	float GetZoneRadius() const { return ZoneRadius; }

	/** True if the calling PlayerState is the original placer. Used by the
	 *  PlayerController toggle/reposition RPCs for authorization. */
	bool IsPlacedBy(const AProjectCompPlayerState* PS) const;

	/** Server: toggle the active state on/off. Caller's identity is enforced
	 *  by the PlayerController-side check (IsPlacedBy), not re-checked here. */
	void Server_ToggleActive();

	/** Server: move the grenade to a new origin. Returns true if accepted —
	 *  rejected if already at the reposition cap. Caller authorization (placer
	 *  only, alive-or-respawning) is enforced at the PlayerController layer. */
	bool Server_Reposition(const FVector& NewOrigin);

	/** Number of times this grenade has been repositioned since placement.
	 *  Capped at RepositionsAllowed below. */
	UFUNCTION(BlueprintPure, Category = "Sound")
	int32 GetRepositionCount() const { return RepositionCount; }

protected:
	virtual void BeginPlay() override;
	virtual void HandleDetonationGameplay() override;

	/** Cosmetic device mesh. Set in BP — C++ leaves empty. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UStaticMeshComponent* DeviceMesh;

	/** AOE volume — non-blocking, query-only. The future audio system uses
	 *  this for IsPointInDampeningZone lookups. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	USphereComponent* ZoneSphere;

	UPROPERTY(EditDefaultsOnly, Category = "Sound", meta = (ClampMin = "100.0"))
	float ZoneRadius = 800.0f;

	/** Max reposition uses. Design memory: 1 (after placer's first death). */
	UPROPERTY(EditDefaultsOnly, Category = "Sound", meta = (ClampMin = "0"))
	int32 RepositionsAllowed = 1;

	/** Replicated active flag — clients can render the volume differently when
	 *  toggled off (e.g. dim the visual). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Sound")
	bool bIsActive = true;

private:
	/** Tracked at Initialize-time. Used by PlayerController-side authorization
	 *  for toggle/reposition + by Tick to detect placer permanent-death.
	 *  TObjectPtr (not weak) — UPROPERTY-tracked, so GC nullifies on PS destroy. */
	UPROPERTY(Replicated)
	TObjectPtr<AProjectCompPlayerState> PlacerPlayerState;

	/** Server-only counter — increments on each successful Server_Reposition. */
	int32 RepositionCount = 0;

	/** Server-only: throttled tick for placer-life monitoring (every 0.5s). */
	float PlacerCheckAccumulator = 0.0f;
};
