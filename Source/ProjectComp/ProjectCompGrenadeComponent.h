// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompGrenadeComponent.generated.h"

class AProjectCompCharacter;
class AProjectCompGrenadeBase;

/**
 * Character-attached component that handles the server-side spawn-and-cooldown
 * flow for grenade use. The owning character's PlayerState holds the kit
 * (EquippedGrenadeTypes) and per-type cooldown end-times; this component just
 * arbitrates "is this use legal right now?" and, if so, spawns the right
 * AProjectCompGrenadeBase subclass and stamps the cooldown.
 *
 * Use flow:
 *   Input/Exec → AProjectCompPlayerController::Server_UseGrenade (RPC)
 *     → finds the Pawn's GrenadeComponent
 *     → GrenadeComponent::Server_UseGrenade(...)
 *     → validates, spawns, calls Server_Initialize, stamps cooldown
 *
 * Per-type cooldowns live here as EditDefaultsOnly knobs so designers can tune
 * without re-cooking. Per-grenade BP defaults (radius, damage, etc.) live on the
 * grenade subclass CDO.
 */
UCLASS(ClassGroup = (ProjectComp), meta = (BlueprintSpawnableComponent))
class PROJECTCOMP_API UProjectCompGrenadeComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UProjectCompGrenadeComponent();

	/**
	 * Server-only: spawn a grenade of the given type at Origin facing Direction.
	 * Validates phase (RoundActive), not-permanently-dead, type-equipped, and
	 * cooldown. On success: spawns + initializes the actor, stamps the
	 * PlayerState cooldown end-time, returns the new grenade. Returns null on
	 * any rejection (logged at Verbose).
	 *
	 * bAltFire: false = LMB overhand throw; true = the standardized underhand
	 *   soft lob (shared by all throwables, handled in the throwable base).
	 */
	AProjectCompGrenadeBase* Server_UseGrenade(EGrenadeType Type, bool bAltFire, const FVector& Origin, const FVector& Direction);

	/**
	 * Server-only: GLOBAL DROP. Same validation as Server_UseGrenade, but instead
	 * of throwing, the grenade is deployed directly at TargetXY — its Z is resolved
	 * to the floor beneath that column (downward trace) and the actor skips flight
	 * (Server_DeployInPlace). This is the hold-key minimap placement path. Returns
	 * null on any rejection.
	 */
	AProjectCompGrenadeBase* Server_GlobalDropGrenade(EGrenadeType Type, const FVector& TargetXY);

	/** Returns the configured cooldown seconds for the given type. None = 0. */
	float GetCooldownForType(EGrenadeType Type) const;

protected:
	virtual void BeginPlay() override;

	/** Spawnable class per grenade type. Populate via the BP defaults on the
	 *  character (BP_PCCharacter → GrenadeComponent → GrenadeClasses) once
	 *  the four subclasses exist. Until then, missing entries reject the use. */
	UPROPERTY(EditDefaultsOnly, Category = "Grenade")
	TMap<EGrenadeType, TSubclassOf<AProjectCompGrenadeBase>> GrenadeClasses;

	// ---- Per-type cooldowns (seconds) ------------------------------------

	UPROPERTY(EditDefaultsOnly, Category = "Grenade|Cooldowns", meta = (ClampMin = "0.0"))
	float FlashCooldown = 15.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Grenade|Cooldowns", meta = (ClampMin = "0.0"))
	float InfoCooldown = 15.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Grenade|Cooldowns", meta = (ClampMin = "0.0"))
	float VulnerableCooldown = 15.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Grenade|Cooldowns", meta = (ClampMin = "0.0"))
	float SmokeCooldown = 15.0f;

private:
	/** Cached pointer to owning character. Resolved in BeginPlay. */
	UPROPERTY()
	TObjectPtr<AProjectCompCharacter> OwnerCharacter;
};
