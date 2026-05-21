// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompHitscanWeapon.h"
#include "ProjectCompCharacter.h"
#include "ProjectComp.h"
#include "Camera/CameraComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"

AProjectCompHitscanWeapon::AProjectCompHitscanWeapon()
{
	// Ticks only on the server (clients have nothing to compute; cosmetic
	// effects come via multicast).
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	bReplicates = true;

	// No mesh / visual on the MVP weapon. Added later when art exists.
}

void AProjectCompHitscanWeapon::BeginPlay()
{
	Super::BeginPlay();

	// Start "ready" so the first shot fires immediately on trigger pull.
	TimeSinceLastShot = FireInterval;
}

void AProjectCompHitscanWeapon::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Authoritative fire processing — server only. Clients tick this actor
	// but skip the work; their visual updates come from Multicast_OnFired.
	if (GetLocalRole() != ROLE_Authority)
	{
		return;
	}

	if (!bTriggerHeld)
	{
		// Cap accumulation while trigger is released so we don't bank infinite
		// shots; one frame of "ready" is enough for an instant first shot.
		TimeSinceLastShot = FMath::Min(TimeSinceLastShot + DeltaTime, FireInterval);
		return;
	}

	TimeSinceLastShot += DeltaTime;

	// Tickrate-independent fire loop, bounded by MaxShotsPerTick to defeat
	// burst-fire exploits at huge DeltaTimes.
	int32 ShotsThisTick = 0;
	while (TimeSinceLastShot >= FireInterval && ShotsThisTick < MaxShotsPerTick)
	{
		FireOneShot();
		TimeSinceLastShot -= FireInterval;
		++ShotsThisTick;
	}
}

void AProjectCompHitscanWeapon::Server_SetTriggerHeld(bool bHeld)
{
	if (!ensure(HasAuthority()))
	{
		return;
	}
	bTriggerHeld = bHeld;
}

void AProjectCompHitscanWeapon::FireOneShot()
{
	const AProjectCompCharacter* OwnerChar = GetOwningCharacter();
	if (!OwnerChar)
	{
		return;
	}

	AController* OwnerController = OwnerChar->GetController();
	if (!OwnerController)
	{
		return;
	}

	// Aim origin: the camera's world position (matches what the player sees).
	const UCameraComponent* Camera = OwnerChar->GetFirstPersonCameraComponent();
	const FVector TraceStart = Camera ? Camera->GetComponentLocation() : OwnerChar->GetActorLocation();

	// Aim direction: control rotation forward (replicates to server every move).
	const FRotator AimRot = OwnerController->GetControlRotation();
	const FVector TraceDir = AimRot.Vector();
	const FVector TraceEnd = TraceStart + TraceDir * MaxRange;

	// Trace.
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WeaponTrace), /*bTraceComplex*/ true, OwnerChar);
	Params.bReturnPhysicalMaterial = false;
	const bool bDidHit = GetWorld()->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, Params);

	// Damage application.
	if (bDidHit && Hit.GetActor())
	{
		const bool bIsHeadshot = (Hit.BoneName == HeadBoneName);
		const float Damage = bIsHeadshot ? BodyDamage * HeadDamageMultiplier : BodyDamage;

		FPointDamageEvent DamageEvent(Damage, Hit, -TraceDir, /*DamageTypeClass*/ nullptr);
		Hit.GetActor()->TakeDamage(Damage, DamageEvent, OwnerController, this);
	}

	// Cosmetic broadcast to all clients (including the firer for now — no
	// client-side prediction in this MVP).
	const FVector EndLoc = bDidHit ? Hit.ImpactPoint : TraceEnd;
	Multicast_OnFired(TraceStart, EndLoc, bDidHit);
}

void AProjectCompHitscanWeapon::Multicast_OnFired_Implementation(FVector StartLoc, FVector EndLoc, bool bHit)
{
	// Cosmetic hook only — never mutate authoritative state here.
	// For MVP: a debug line. Replace with muzzle flash particle, tracer
	// niagara system, and impact decal once art is in.
	if (GetWorld())
	{
		const FColor LineColor = bHit ? FColor::Red : FColor::Yellow;
		DrawDebugLine(GetWorld(), StartLoc, EndLoc, LineColor, /*persistent*/ false, /*lifetime*/ 0.15f, /*depth*/ 0, /*thickness*/ 1.5f);
	}
}

AProjectCompCharacter* AProjectCompHitscanWeapon::GetOwningCharacter() const
{
	return Cast<AProjectCompCharacter>(GetOwner());
}
