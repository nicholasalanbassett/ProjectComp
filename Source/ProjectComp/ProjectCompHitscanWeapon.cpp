// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompHitscanWeapon.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompPlayerState.h"
#include "ProjectComp.h"
#include "Camera/CameraComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Net/UnrealNetwork.h"

AProjectCompHitscanWeapon::AProjectCompHitscanWeapon()
{
	// Ticks only on the server (clients have nothing to compute; cosmetic
	// effects come via multicast).
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	bReplicates = true;

	// No mesh / visual on the MVP weapon. Added later when art exists.

	// Default spray pattern (degrees): X = pitch up, Y = yaw deviation.
	// Phantom-like: steady vertical climb with light left/right alternation.
	// Override per weapon in BP defaults.
	RecoilPattern = {
		{ 0.50f,  0.00f },
		{ 0.80f,  0.05f },
		{ 0.90f, -0.10f },
		{ 0.85f,  0.15f },
		{ 0.80f, -0.20f },
		{ 0.75f,  0.20f },
		{ 0.70f, -0.15f },
		{ 0.65f,  0.10f },
		{ 0.60f, -0.05f },
		{ 0.55f,  0.05f },
	};
}

void AProjectCompHitscanWeapon::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// Owner-only: only the firer needs their own spread value for HUD display.
	DOREPLIFETIME_CONDITION(AProjectCompHitscanWeapon, CurrentSpread, COND_OwnerOnly);
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

		// Spread decays back to zero while idle.
		CurrentSpread = FMath::Max(0.0f, CurrentSpread - SpreadRecoveryRate * DeltaTime);

		// After prolonged idle the spray pattern resets so the next burst starts
		// from index 0 (tight, predictable first shot).
		ShotIdleTime += DeltaTime;
		if (ShotIdleTime >= RecoilPatternResetDelay)
		{
			RecoilPatternIndex = 0;
		}
		return;
	}

	ShotIdleTime = 0.0f;
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
	AProjectCompCharacter* OwnerChar = GetOwningCharacter();
	if (!OwnerChar)
	{
		return;
	}

	AController* OwnerController = OwnerChar->GetController();
	if (!OwnerController)
	{
		return;
	}

	// Looming players can hold the trigger but nothing happens — they're dead.
	if (const AProjectCompPlayerState* PS = OwnerController->GetPlayerState<AProjectCompPlayerState>())
	{
		if (!PS->IsAlive())
		{
			return;
		}
	}

	// Aim origin: the camera's world position (matches what the player sees).
	const UCameraComponent* Camera = OwnerChar->GetFirstPersonCameraComponent();
	const FVector TraceStart = Camera ? Camera->GetComponentLocation() : OwnerChar->GetActorLocation();

	// Aim direction: control rotation forward (replicates to server every move).
	const FRotator AimRot = OwnerController->GetControlRotation();
	const FVector TraceDir = AimRot.Vector();

	// Apply spread: randomly deviate the bullet within a cone whose half-angle is
	// determined by movement state, accumulated fire spread, and stance.
	const float TotalSpread = CalcTotalSpread(OwnerChar);
	const FVector ShotDir = TotalSpread > 0.0f
		? FMath::VRandCone(TraceDir, FMath::DegreesToRadians(TotalSpread))
		: TraceDir;
	const FVector TraceEnd = TraceStart + ShotDir * MaxRange;

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

		FPointDamageEvent DamageEvent(Damage, Hit, -ShotDir, /*DamageTypeClass*/ nullptr);
		Hit.GetActor()->TakeDamage(Damage, DamageEvent, OwnerController, this);
	}

	// Accumulate spread for the next shot (decays in Tick when not firing).
	CurrentSpread = FMath::Min(CurrentSpread + SpreadPerShot, MaxSpread);

	// Recoil: kick the owning client's camera along the deterministic spray pattern.
	if (RecoilPattern.Num() > 0)
	{
		const int32 Idx = FMath::Min(RecoilPatternIndex, RecoilPattern.Num() - 1);
		const FVector2D& Kick = RecoilPattern[Idx];
		RecoilPatternIndex = FMath::Min(RecoilPatternIndex + 1, RecoilPattern.Num() - 1);

		OwnerChar->Client_ApplyRecoilKick(Kick.X, Kick.Y, RecoilRecoveryRate, RecoilRecoveryDelay);
	}

	// Cosmetic broadcast to all clients (including the firer for now — no
	// client-side prediction in this MVP).
	const FVector EndLoc = bDidHit ? Hit.ImpactPoint : TraceEnd;
	Multicast_OnFired(TraceStart, EndLoc, bDidHit);
}

float AProjectCompHitscanWeapon::CalcTotalSpread(const AProjectCompCharacter* Char) const
{
	if (!Char)
	{
		return BaseSpreadDegrees;
	}

	float Spread = BaseSpreadDegrees + CurrentSpread;

	const UCharacterMovementComponent* CharMov = Char->GetCharacterMovement();
	if (CharMov && CharMov->IsFalling())
	{
		Spread += AirSpreadAdd;
	}
	else if (Char->GetVelocity().SizeSquared2D() > FMath::Square(MoveSpeedThreshold))
	{
		const float MaxSpeed = CharMov ? CharMov->MaxWalkSpeed : 560.0f;
		const float SpeedFrac = FMath::Clamp(Char->GetVelocity().Size2D() / MaxSpeed, 0.0f, 1.0f);
		Spread += MovingSpreadAdd * SpeedFrac;
	}

	if (Char->bIsCrouched)
	{
		Spread *= CrouchSpreadMult;
	}

	return FMath::Min(Spread, MaxSpread);
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
