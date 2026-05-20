// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ProjectCompHealthComponent.generated.h"

class AController;
class AProjectCompHealthComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FHealthChangedSignature, float, NewHealth, float, MaxHealth);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeathSignature, AController*, Killer);

/**
 * Replicated health container.
 *
 * Server is sole writer. Damage flows in via Server_TakeDamage (called by the
 * owning Character's TakeDamage override). On HP <= 0, calls into the GameMode
 * to drive the authoritative death/respawn flow.
 *
 * Health, max health, and the changed/death events are all replicated; UI can
 * bind directly to the events to drive HP bars and damage indicators.
 */
UCLASS(ClassGroup = (ProjectComp), meta = (BlueprintSpawnableComponent))
class PROJECTCOMP_API UProjectCompHealthComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UProjectCompHealthComponent();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;

	// ---- Accessors --------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "Health")
	float GetCurrentHealth() const { return CurrentHealth; }

	UFUNCTION(BlueprintPure, Category = "Health")
	float GetMaxHealth() const { return MaxHealth; }

	UFUNCTION(BlueprintPure, Category = "Health")
	float GetHealthFraction() const { return MaxHealth > 0.0f ? CurrentHealth / MaxHealth : 0.0f; }

	UFUNCTION(BlueprintPure, Category = "Health")
	bool IsDead() const { return CurrentHealth <= 0.0f; }

	// ---- Server-only mutators --------------------------------------------

	/**
	 * Apply damage. Server-authoritative. If HP drops to 0, drives the death
	 * flow via the GameMode. Returns the actual damage applied (clamped).
	 */
	float Server_TakeDamage(float Amount, AController* DamageInstigator);

	/** Restore HP to MaxHealth. Called by the GameMode after respawn. */
	void Server_ResetHealth();

	/** Heal by Amount, clamped to MaxHealth. */
	void Server_Heal(float Amount);

	// ---- Events (bindable from BP / C++) ---------------------------------

	UPROPERTY(BlueprintAssignable, Category = "Health")
	FHealthChangedSignature OnHealthChanged;

	/** Fires on the server when the owner's HP first reaches 0 this life. */
	UPROPERTY(BlueprintAssignable, Category = "Health")
	FDeathSignature OnDeath;

protected:

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Health", meta = (ClampMin = "1.0"))
	float MaxHealth = 100.0f;

	UPROPERTY(ReplicatedUsing = OnRep_CurrentHealth, BlueprintReadOnly, Category = "Health")
	float CurrentHealth = 100.0f;

	UFUNCTION()
	void OnRep_CurrentHealth(float OldHealth);
};
