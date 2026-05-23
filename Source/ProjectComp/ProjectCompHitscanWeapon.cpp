// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompHitscanWeapon.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompPlayerState.h"
#include "ProjectComp.h"
#include "Camera/CameraComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
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
	// Multicasts (fire events) follow the owning character's relevancy. Dead
	// opponents — already filtered by AProjectCompCharacter::IsNetRelevantFor —
	// will not receive fire packets, which closes the wallhack channel.
	bNetUseOwnerRelevancy = true;

	// No mesh / visual on the MVP weapon. Added later when art exists.

	// Bone → hit-zone mapping for the standard UE mannequin rig.
	// Override BoneZoneMap in BP defaults for any custom skeleton.
	BoneZoneMap.Add(TEXT("head"),         EHitZone::Head);
	BoneZoneMap.Add(TEXT("neck_01"),      EHitZone::Head);
	BoneZoneMap.Add(TEXT("spine_03"),     EHitZone::Body);
	BoneZoneMap.Add(TEXT("spine_02"),     EHitZone::Body);
	BoneZoneMap.Add(TEXT("spine_01"),     EHitZone::Stomach);
	BoneZoneMap.Add(TEXT("pelvis"),       EHitZone::Stomach);
	BoneZoneMap.Add(TEXT("thigh_l"),      EHitZone::Limb);
	BoneZoneMap.Add(TEXT("thigh_r"),      EHitZone::Limb);
	BoneZoneMap.Add(TEXT("calf_l"),       EHitZone::Limb);
	BoneZoneMap.Add(TEXT("calf_r"),       EHitZone::Limb);
	BoneZoneMap.Add(TEXT("foot_l"),       EHitZone::Limb);
	BoneZoneMap.Add(TEXT("foot_r"),       EHitZone::Limb);
	BoneZoneMap.Add(TEXT("upperarm_l"),   EHitZone::Limb);
	BoneZoneMap.Add(TEXT("upperarm_r"),   EHitZone::Limb);
	BoneZoneMap.Add(TEXT("lowerarm_l"),   EHitZone::Limb);
	BoneZoneMap.Add(TEXT("lowerarm_r"),   EHitZone::Limb);
	BoneZoneMap.Add(TEXT("hand_l"),       EHitZone::Limb);
	BoneZoneMap.Add(TEXT("hand_r"),       EHitZone::Limb);

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

	// No phase gate here — the player decides when to fire. The original
	// "bullets spray after round-end kill" bug was caused by the character
	// RPC dropping release events, leaving bTriggerHeld stuck true. With
	// releases now flowing through unconditionally (see Character::
	// Server_SetTriggerHeld_Implementation), bTriggerHeld accurately reflects
	// player intent across all phases — including intentional PostRound fire
	// for economy damage / save shots.
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

	// Aim origin: camera world position; aim direction: SERVER-VALIDATED
	// control rotation (rate-clamped + pitch-limited). Using the raw client
	// rotation here would mean an aimbot could snap to a perfect headshot
	// between RPCs; the validator denies that delta budget.
	const UCameraComponent* Camera = OwnerChar->GetFirstPersonCameraComponent();
	const FVector TraceStart = Camera ? Camera->GetComponentLocation() : OwnerChar->GetActorLocation();
	const FRotator AimRot    = OwnerChar->Server_GetValidatedAimRotation();
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

	// ---- Lag compensation: rewind every other character to where they were
	// when the firer's client took the shot. RewindTime = Now - half-RTT - InterpDelay.
	// All rewound characters are restored at scope exit so the world state is
	// unchanged outside this function.
	UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.0f;
	float HalfRttSeconds = 0.0f;
	if (const APlayerController* PC = Cast<APlayerController>(OwnerController))
	{
		if (const APlayerState* PS = PC->PlayerState)
		{
			// ExactPing is in milliseconds, full RTT.
			HalfRttSeconds = (PS->ExactPing * 0.001f) * 0.5f;
		}
	}
	const float RewindSeconds = FMath::Clamp(HalfRttSeconds + ClientInterpolationDelay, 0.0f, MaxRewindSeconds);
	const float RewindTime = Now - RewindSeconds;

	TArray<AProjectCompCharacter*, TInlineAllocator<16>> Rewound;
	if (RewindSeconds > KINDA_SMALL_NUMBER && World)
	{
		for (TActorIterator<AProjectCompCharacter> It(World); It; ++It)
		{
			AProjectCompCharacter* Target = *It;
			if (!Target || Target == OwnerChar)
			{
				continue;
			}
			Target->Server_RewindTo(RewindTime);
			Rewound.Add(Target);
		}
	}

	// Trace.
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WeaponTrace), /*bTraceComplex*/ true, OwnerChar);
	Params.bReturnPhysicalMaterial = false;
	const bool bDidHit = World ? World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, Params) : false;

	// Restore rewound characters BEFORE damage so OnDeath / death-flow sees current state.
	for (AProjectCompCharacter* Target : Rewound)
	{
		if (Target)
		{
			Target->Server_RestoreFromRewind();
		}
	}

	// Damage — per-zone tier from the bone-name map (Body / Head / Stomach / Limb).
	if (bDidHit && Hit.GetActor())
	{
		const EHitZone Zone = ClassifyHitZone(Hit.BoneName);
		const float Damage = BodyDamage * GetZoneMultiplier(Zone);
		FPointDamageEvent DamageEvent(Damage, Hit, -BulletDir, nullptr);
		Hit.GetActor()->TakeDamage(Damage, DamageEvent, OwnerController, this);
	}

	// Accumulate so the next bullet's trace starts from where this kick landed.
	ServerRecoilPitch += Entry.X;
	ServerRecoilYaw   += Entry.Y;

	// Per-shot Client RPC removed — the Multicast_OnFired below already arrives
	// on the firer's client, and the owning-client side advances its local
	// RecoilPattern mirror there. Saves one Reliable RPC per shot.

	// Advance pattern index, clamp at last entry.
	RecoilPatternIndex = FMath::Min(RecoilPatternIndex + 1, RecoilPattern.Num() - 1);

	// Cosmetic broadcast to all (still-relevant) clients. We send origin +
	// quantized direction + a short distance, never the literal hit endpoint —
	// receivers reconstruct visuals locally so the multicast does not leak
	// enemy positions to opponents who happen to receive a stray packet.
	const FVector EndLoc = bDidHit ? Hit.ImpactPoint : TraceEnd;
	const float HitDistance = FMath::Min(FVector::Distance(TraceStart, EndLoc), static_cast<float>(MaxRange));
	const uint16 DistanceCm = static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(HitDistance), 0, 65535));
	Multicast_OnFired(TraceStart, BulletDir, DistanceCm, bDidHit);
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

void AProjectCompHitscanWeapon::Multicast_OnFired_Implementation(FVector_NetQuantize StartLoc, FVector_NetQuantizeNormal Direction, uint16 DistanceCm, bool bHit)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Cosmetic hook — endpoint reconstructed from StartLoc + Direction * Distance
	// so opponents can't extract enemy positions from server packets.
	const FVector Start = static_cast<FVector>(StartLoc);
	const FVector Dir = static_cast<FVector>(Direction).GetSafeNormal();
	const FVector End = Start + Dir * static_cast<float>(DistanceCm);
	const FVector MuzzleLoc = Start + Dir * 100.0f;
	const FColor LineColor = bHit ? FColor::Red : FColor::Yellow;
	DrawDebugLine(World, MuzzleLoc, End, LineColor, /*persistent*/ false, /*lifetime*/ 0.15f, /*depth*/ 0, /*thickness*/ 1.5f);

	// Local recoil prediction for the owning client. The pattern is
	// deterministic and identical to the server's RecoilPatternIndex math, so
	// no Client RPC is needed — this just drives the same kick on the local
	// view at the same wall-clock as the muzzle flash.
	AProjectCompCharacter* OwnerChar = GetOwningCharacter();
	if (OwnerChar && OwnerChar->IsLocallyControlled())
	{
		const float Now = World->GetTimeSeconds();
		if (LocalLastShotClientTime > 0.0f && (Now - LocalLastShotClientTime) >= RecoilPatternResetDelay)
		{
			LocalRecoilPatternIndex = 0;
		}
		LocalLastShotClientTime = Now;

		if (RecoilPattern.Num() > 0)
		{
			const int32 Idx = FMath::Min(LocalRecoilPatternIndex, RecoilPattern.Num() - 1);
			const FVector2D& Entry = RecoilPattern[Idx];
			OwnerChar->ApplyRecoilKickLocal(Entry.X, Entry.Y, RecoilRecoveryRate, RecoilRecoveryDelay);
			LocalRecoilPatternIndex = FMath::Min(LocalRecoilPatternIndex + 1, RecoilPattern.Num() - 1);
		}
	}
}

AProjectCompCharacter* AProjectCompHitscanWeapon::GetOwningCharacter() const
{
	return Cast<AProjectCompCharacter>(GetOwner());
}

EHitZone AProjectCompHitscanWeapon::ClassifyHitZone(FName BoneName) const
{
	if (const EHitZone* Zone = BoneZoneMap.Find(BoneName))
	{
		return *Zone;
	}
	return EHitZone::Body;
}

float AProjectCompHitscanWeapon::GetZoneMultiplier(EHitZone Zone) const
{
	switch (Zone)
	{
		case EHitZone::Head:    return HeadDamageMultiplier;
		case EHitZone::Stomach: return StomachDamageMultiplier;
		case EHitZone::Limb:    return LimbDamageMultiplier;
		case EHitZone::Body:
		default:                return 1.0f;
	}
}
