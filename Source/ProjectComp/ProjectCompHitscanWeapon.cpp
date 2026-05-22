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

namespace
{
	/**
	 * Returns a random unit vector within an elliptical cone around Dir.
	 * VertHalfAngleRad  = half-angle (radians) in the local Up direction.
	 * HorizHalfAngleRad = half-angle (radians) in the local Right direction.
	 * Produces a uniform distribution over the elliptical solid angle.
	 */
	FVector VRandEllipticalCone(const FVector& Dir, float VertHalfAngleRad, float HorizHalfAngleRad)
	{
		FVector Up, Right;
		Dir.FindBestAxisVectors(Up, Right);

		// Polar coords with sqrt(R) for uniform area distribution inside the ellipse.
		const float Theta = FMath::FRandRange(0.0f, 2.0f * PI);
		const float R     = FMath::Sqrt(FMath::FRand());

		const float VertDev  = FMath::Tan(VertHalfAngleRad)  * FMath::Sin(Theta) * R;
		const float HorizDev = FMath::Tan(HorizHalfAngleRad) * FMath::Cos(Theta) * R;

		return (Dir + Up * VertDev + Right * HorizDev).GetSafeNormal();
	}
}

AProjectCompHitscanWeapon::AProjectCompHitscanWeapon()
{
	// Ticks only on the server (clients have nothing to compute; cosmetic
	// effects come via multicast).
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	bReplicates = true;

	// No mesh / visual on the MVP weapon. Added later when art exists.

	// 30-bullet spray pattern. X = pitch up (degrees), Y = yaw right (degrees).
	// Each entry defines exactly where that bullet goes AND how the camera kicks.
	// No random spread is added on top — what you see here is what fires.
	// Movement/air/crouch spread adds a random cone on top of this base direction.
	// Per-shot camera kick (degrees). X = pitch up this shot, Y = yaw right this shot.
	// CS2 AK-47 style: strong vertical climb early, yaw kicks left then right, settles late.
	RecoilPattern = {
		{ 0.20f,  0.00f },  //  1 — straight up
		{ 0.30f,  0.00f },  //  2
		{ 0.40f,  0.00f },  //  3
		// 4-14: light zigzag — peak excursion ~±0.43°.
		{ 0.55f, -0.15f },  //  4 — drift left
		{ 0.50f, -0.28f },  //  5 (peak left ~-0.43)
		{ 0.45f,  0.00f },  //  6 — sweep toward right
		{ 0.40f,  0.28f },  //  7
		{ 0.38f,  0.15f },  //  8 — back left
		{ 0.35f, -0.15f },  //  9
		{ 0.33f, -0.28f },  // 10 (peak left ~-0.43)
		{ 0.30f,  0.00f },  // 11 — sweep toward right
		{ 0.28f,  0.28f },  // 12
		{ 0.25f,  0.15f },  // 13 — back left
		{ 0.23f, -0.15f },  // 14
		// 15-30: two full sweep cycles. Each cycle = right→center→left→center
		// (8 shots = 4 shots per half). Per-shot kick of ±0.43° gives ~±0.85°
		// peak excursion in each direction. Vertical kick is held near-flat —
		// the gun's already climbed its full height by bullet 14.
		// Cycle 1 — right half (peak around bullet 16)
		{ 0.05f,  0.43f },  // 15 → pos +0.43
		{ 0.05f,  0.43f },  // 16 → pos +0.85 (peak right)
		{ 0.05f, -0.43f },  // 17 → pos +0.43
		{ 0.05f, -0.43f },  // 18 → pos  0    (center)
		// Cycle 1 — left half (peak around bullet 20)
		{ 0.05f, -0.43f },  // 19 → pos -0.43
		{ 0.05f, -0.43f },  // 20 → pos -0.85 (peak left)
		{ 0.05f,  0.43f },  // 21 → pos -0.43
		{ 0.05f,  0.43f },  // 22 → pos  0    (center, end cycle 1)
		// Cycle 2 — right half (peak around bullet 24)
		{ 0.05f,  0.43f },  // 23 → pos +0.43
		{ 0.05f,  0.43f },  // 24 → pos +0.85 (peak right)
		{ 0.05f, -0.43f },  // 25 → pos +0.43
		{ 0.05f, -0.43f },  // 26 → pos  0
		// Cycle 2 — left half (peak around bullet 28)
		{ 0.05f, -0.43f },  // 27 → pos -0.43
		{ 0.05f, -0.43f },  // 28 → pos -0.85 (peak left)
		{ 0.05f,  0.43f },  // 29 → pos -0.43
		{ 0.05f,  0.43f },  // 30 → pos  0    (center, end cycle 2)
	};
}

void AProjectCompHitscanWeapon::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
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
		// Cap accumulation so we don't bank infinite shots.
		TimeSinceLastShot = FMath::Min(TimeSinceLastShot + DeltaTime, FireInterval);

		// After prolonged idle the spray pattern resets so the next burst starts
		// from index 0 (tight, predictable first shot).
		ShotIdleTime += DeltaTime;
		if (ShotIdleTime >= RecoilPatternResetDelay)
		{
			RecoilPatternIndex = 0;
			ServerRecoilPitch  = 0.0f;
			ServerRecoilYaw    = 0.0f;
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

	// Aim origin: camera world position; aim direction: control rotation.
	const UCameraComponent* Camera = OwnerChar->GetFirstPersonCameraComponent();
	const FVector TraceStart = Camera ? Camera->GetComponentLocation() : OwnerChar->GetActorLocation();
	const FRotator AimRot    = OwnerController->GetControlRotation();
	const FVector  TraceDir  = AimRot.Vector();

	// Apply accumulated server-side recoil so bullet direction matches where the
	// client camera has actually moved to after previous kicks.
	FRotator BulletRot = AimRot;
	BulletRot.Pitch += ServerRecoilPitch;
	BulletRot.Yaw   += ServerRecoilYaw;
	FVector BulletDir = BulletRot.Vector();

	// Add movement/air/crouch random spread.
	const float MovementSpread = CalcMovementSpread(OwnerChar);
	if (MovementSpread > 0.0f)
	{
		const float VertRad  = FMath::DegreesToRadians(MovementSpread);
		const float HorizRad = VertRad * HorizontalSpreadMult;
		BulletDir = VRandEllipticalCone(BulletDir, VertRad, HorizRad);
	}

	// Clamp pattern index to the last entry so spraying past 30 bullets stays at the final position.
	const int32 Idx = FMath::Min(RecoilPatternIndex, RecoilPattern.Num() - 1);
	const FVector2D& Entry = RecoilPattern[Idx];

	const FVector TraceEnd = TraceStart + BulletDir * MaxRange;

	// Trace.
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WeaponTrace), /*bTraceComplex*/ true, OwnerChar);
	Params.bReturnPhysicalMaterial = false;
	const bool bDidHit = GetWorld()->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, Params);

	// Damage.
	if (bDidHit && Hit.GetActor())
	{
		const bool bIsHeadshot = (Hit.BoneName == HeadBoneName);
		const float Damage = bIsHeadshot ? BodyDamage * HeadDamageMultiplier : BodyDamage;
		FPointDamageEvent DamageEvent(Damage, Hit, -BulletDir, nullptr);
		Hit.GetActor()->TakeDamage(Damage, DamageEvent, OwnerController, this);
	}

	// Accumulate so the next bullet's trace starts from where this kick landed.
	ServerRecoilPitch += Entry.X;
	ServerRecoilYaw   += Entry.Y;

	OwnerChar->Client_ApplyRecoilKick(Entry.X, Entry.Y, RecoilRecoveryRate, RecoilRecoveryDelay);

	// Advance pattern index, clamp at last entry.
	RecoilPatternIndex = FMath::Min(RecoilPatternIndex + 1, RecoilPattern.Num() - 1);

	// Cosmetic broadcast to all clients (including the firer for now — no
	// client-side prediction in this MVP).
	const FVector EndLoc = bDidHit ? Hit.ImpactPoint : TraceEnd;
	Multicast_OnFired(TraceStart, EndLoc, bDidHit);
}

float AProjectCompHitscanWeapon::CalcMovementSpread(const AProjectCompCharacter* Char) const
{
	if (!Char)
	{
		return 0.0f;
	}

	float Spread = 0.0f;

	const UCharacterMovementComponent* CharMov = Char->GetCharacterMovement();
	if (CharMov && CharMov->IsFalling())
	{
		Spread += AirSpreadAdd;
	}
	else
	{
		const float Speed    = Char->GetVelocity().Size2D();
		const float MaxSpeed = CharMov ? CharMov->MaxWalkSpeed : 560.0f;
		// Ramp from 0 at MoveSpeedThreshold to full MoveAdd at MaxSpeed.
		// Below threshold: zero. Just above: eases in gradually instead of snapping.
		const float SpeedFrac = FMath::Clamp((Speed - MoveSpeedThreshold) / FMath::Max(MaxSpeed - MoveSpeedThreshold, 1.0f), 0.0f, 1.0f);
		const float MoveAdd   = Char->bIsCrouched ? CrouchMovingSpreadAdd : MovingSpreadAdd;
		Spread += MoveAdd * SpeedFrac * TransitionSpreadMultiplier;
	}

	if (Char->bIsCrouched && Spread > 0.0f)
	{
		Spread *= CrouchSpreadMult;
	}

	return Spread;
}

void AProjectCompHitscanWeapon::Multicast_OnFired_Implementation(FVector StartLoc, FVector EndLoc, bool bHit)
{
	// Cosmetic hook only — never mutate authoritative state here.
	// For MVP: a debug line. Replace with muzzle flash particle, tracer
	// niagara system, and impact decal once art is in.
	if (GetWorld())
	{
		// Offset the tracer start ~100 cm along the shot direction so the debug
		// capsule's end cap renders in front of the camera rather than filling
		// the screen when firing straight ahead.
		const FVector ShotDir = (EndLoc - StartLoc).GetSafeNormal();
		const FVector MuzzleLoc = StartLoc + ShotDir * 100.0f;
		const FColor LineColor = bHit ? FColor::Red : FColor::Yellow;
		DrawDebugLine(GetWorld(), MuzzleLoc, EndLoc, LineColor, /*persistent*/ false, /*lifetime*/ 0.15f, /*depth*/ 0, /*thickness*/ 1.5f);
	}
}

AProjectCompCharacter* AProjectCompHitscanWeapon::GetOwningCharacter() const
{
	return Cast<AProjectCompCharacter>(GetOwner());
}
