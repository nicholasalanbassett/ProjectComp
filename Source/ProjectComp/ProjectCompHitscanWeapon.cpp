// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompHitscanWeapon.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompGameMode.h"
#include "ProjectCompGameState.h"
#include "ProjectCompPlayerState.h"
#include "ProjectComp.h"
#include "Camera/CameraComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
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

	// Plain scene root attaches to the FP camera while equipped. The visual mesh
	// hangs off it so the BP subclass (e.g. BP_Rifle) can freely set the mesh's
	// relative rotation/location (the native root's transform isn't BP-editable).
	WeaponRoot = CreateDefaultSubobject<USceneComponent>(TEXT("WeaponRoot"));
	SetRootComponent(WeaponRoot);

	// Visual mesh + material are assigned on the BP subclass — empty in C++.
	WeaponMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WeaponMesh"));
	WeaponMesh->SetupAttachment(WeaponRoot);
	WeaponMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	WeaponMesh->SetCastShadow(false);

	// Walk-over auto-pickup volume. Starts disabled (weapons spawn equipped);
	// enabled when the weapon is dropped on the ground. Overlaps live pawns only.
	PickupSphere = CreateDefaultSubobject<USphereComponent>(TEXT("PickupSphere"));
	PickupSphere->SetupAttachment(WeaponRoot);
	PickupSphere->InitSphereRadius(80.0f);
	PickupSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PickupSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	PickupSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

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

	// Ammo state only matters to the owning client — opponents reading your
	// mag count would leak intel. COND_OwnerOnly keeps the broadcast scoped.
	DOREPLIFETIME_CONDITION(AProjectCompHitscanWeapon, CurrentMagAmmo, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AProjectCompHitscanWeapon, ReserveAmmo,    COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AProjectCompHitscanWeapon, bIsReloading,   COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AProjectCompHitscanWeapon, ReloadEndTime,  COND_OwnerOnly);
}

void AProjectCompHitscanWeapon::BeginPlay()
{
	Super::BeginPlay();

	// Start "ready" so the first shot fires immediately on trigger pull.
	TimeSinceLastShot = FireInterval;

	// Seed ammo from the BP/CDO tunables so a freshly-spawned weapon starts
	// with a full mag + full reserves. PreRound refill calls Server_RefillAllAmmo
	// for subsequent rounds.
	if (HasAuthority())
	{
		CurrentMagAmmo = MagazineSize;
		ReserveAmmo    = MaxReserveMags * MagazineSize;
		bIsReloading   = false;
		ReloadEndTime  = 0.0f;

		// Cache the authority GameMode once so the per-shot fire path can read
		// the LiveCharacters registry without a Cast<>() or GetAuthGameMode()
		// lookup. Weak ptr so a torn-down GameMode (level transition / PIE
		// teardown) cannot dangle.
		if (UWorld* World = GetWorld())
		{
			CachedAuthGameMode = World->GetAuthGameMode<AProjectCompGameMode>();
		}

		if (PickupSphere)
		{
			PickupSphere->OnComponentBeginOverlap.AddDynamic(this, &AProjectCompHitscanWeapon::OnPickupOverlap);
		}
	}
}

void AProjectCompHitscanWeapon::SetAvailableForPickup(bool bAvailable)
{
	if (!HasAuthority() || !PickupSphere)
	{
		return;
	}
	PickupSphere->SetCollisionEnabled(bAvailable
		? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
}

void AProjectCompHitscanWeapon::OnPickupOverlap(UPrimitiveComponent* /*OverlappedComp*/, AActor* OtherActor,
	UPrimitiveComponent* /*OtherComp*/, int32 /*OtherBodyIndex*/, bool /*bFromSweep*/, const FHitResult& /*Sweep*/)
{
	// Only a dropped (unowned) weapon is loot. Auto-pickup fills an EMPTY slot
	// only — if the walker already has a weapon, they keep it (F can still swap).
	if (!HasAuthority() || GetOwner() != nullptr)
	{
		return;
	}
	AProjectCompCharacter* Char = Cast<AProjectCompCharacter>(OtherActor);
	if (Char && Char->GetCurrentWeapon() == nullptr)
	{
		Char->EquipGroundWeapon(this, /*bAllowSwap*/ false);
	}
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

	// Reload completion runs independently of trigger state — releasing M1
	// during a reload shouldn't cancel it.
	if (bIsReloading)
	{
		const UWorld* World = GetWorld();
		const AGameStateBase* GS = World ? World->GetGameState() : nullptr;
		const float ServerTime = GS ? GS->GetServerWorldTimeSeconds()
		                            : (World ? World->GetTimeSeconds() : 0.0f);
		if (ServerTime >= ReloadEndTime)
		{
			// Partial-mag preserving reload: pool the leftover chambered rounds
			// with the reserve, then refill the mag from the pool. Reloading
			// with 10 left and 120 reserve → 30 chambered, 100 reserve (no loss).
			// Reloading with 5 left and 10 reserve → 15 chambered, 0 reserve
			// (partial top-off — better than CS but matches Valorant/Apex).
			const int32 Pool = CurrentMagAmmo + ReserveAmmo;
			CurrentMagAmmo = FMath::Min(MagazineSize, Pool);
			ReserveAmmo    = FMath::Max(0, Pool - MagazineSize);
			bIsReloading   = false;
			ReloadEndTime  = 0.0f;
		}
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
		// Spread ramp uses its own decay timer so the two systems can be tuned
		// independently (e.g. faster pattern reset, slower accuracy reset).
		if (ShotIdleTime >= SpreadRampResetDelay)
		{
			BurstShotsFired = 0;
		}
		return;
	}

	// Trigger is held — but the pre-match lobby (None) and the LoomSelection draft
	// hard-block firing (players are frozen + mouse-locked into a menu). Bail
	// before any shot. This lookup only runs while the trigger is actually held,
	// so non-firing weapons pay nothing. (PreRound/PostRound fire stays intentional.)
	if (const UWorld* W = GetWorld())
	{
		if (const AProjectCompGameState* PCGS = W->GetGameState<AProjectCompGameState>())
		{
			const ERoundPhase FirePhase = PCGS->GetCurrentPhase();
			if (FirePhase == ERoundPhase::LoomSelection || FirePhase == ERoundPhase::None)
			{
				return;
			}
		}
	}

	ShotIdleTime = 0.0f;
	TimeSinceLastShot += DeltaTime;

	// Holding the trigger while we can't fire (empty mag or mid-reload) must
	// NOT accumulate TimeSinceLastShot freely — otherwise a player who holds
	// M1 through a 2.2s reload would get MaxShotsPerTick instant catch-up
	// shots the moment ammo is restored. Cap to one shot's worth instead.
	const bool bCanFire = !bIsReloading && CurrentMagAmmo > 0;
	if (!bCanFire)
	{
		TimeSinceLastShot = FMath::Min(TimeSinceLastShot, FireInterval);
		return;
	}

	// Tickrate-independent fire loop, bounded by MaxShotsPerTick to defeat
	// burst-fire exploits at huge DeltaTimes.
	int32 ShotsThisTick = 0;
	while (TimeSinceLastShot >= FireInterval && ShotsThisTick < MaxShotsPerTick)
	{
		FireOneShot();
		TimeSinceLastShot -= FireInterval;
		++ShotsThisTick;
		// Mag emptied mid-burst — stop the loop. Auto-reload kicks in below.
		if (CurrentMagAmmo <= 0)
		{
			break;
		}
	}

	// Auto-reload the moment the mag hits 0 (the last shot fired this tick).
	// Server_StartReload self-gates on reserve > 0, !bIsReloading, alive, and
	// !interact-locked — silently no-ops in those cases, so this stays one line.
	if (CurrentMagAmmo <= 0)
	{
		Server_StartReload();
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

void AProjectCompHitscanWeapon::Server_StartReload()
{
	if (!HasAuthority())
	{
		return;
	}

	// Reject if any precondition fails. All silent — the input came from a
	// client RPC and we don't want to chatter back on every spurious press.
	if (bIsReloading || CurrentMagAmmo >= MagazineSize || ReserveAmmo <= 0)
	{
		return;
	}

	// Dead players can't reload (looming). Mirrors the gate in FireOneShot.
	// Same goes for plant/defuse — reload is blocked during the interaction.
	if (const AProjectCompCharacter* OwnerChar = GetOwningCharacter())
	{
		if (OwnerChar->IsInteractLocked())
		{
			return;
		}
		if (const AController* OwnerController = OwnerChar->GetController())
		{
			if (const AProjectCompPlayerState* PS = OwnerController->GetPlayerState<AProjectCompPlayerState>())
			{
				if (!PS->IsAlive())
				{
					return;
				}
			}
		}
	}

	const UWorld* World = GetWorld();
	const AGameStateBase* GS = World ? World->GetGameState() : nullptr;
	const float ServerTime = GS ? GS->GetServerWorldTimeSeconds()
	                            : (World ? World->GetTimeSeconds() : 0.0f);

	bIsReloading  = true;
	ReloadEndTime = ServerTime + ReloadDuration;
}

void AProjectCompHitscanWeapon::Server_RefillAllAmmo()
{
	if (!HasAuthority())
	{
		return;
	}
	CurrentMagAmmo = MagazineSize;
	ReserveAmmo    = MaxReserveMags * MagazineSize;
	bIsReloading   = false;
	ReloadEndTime  = 0.0f;
}

void AProjectCompHitscanWeapon::Server_CancelReload()
{
	if (!HasAuthority() || !bIsReloading)
	{
		return;
	}
	// Mag stays at whatever the player had chambered — they lose the reload
	// time spent, must press R again (or auto-reload via the lock-release
	// hook in Character::UpdateInteractLock) once they can act.
	bIsReloading  = false;
	ReloadEndTime = 0.0f;
}

void AProjectCompHitscanWeapon::FireOneShot()
{
	// Fire-path cycle counter. Compiled out in shipping; in dev/test the
	// command `stat ProjectComp` prints microseconds-per-shot so the
	// sub-500us gameplay-loop budget can be verified under stress.
	SCOPE_CYCLE_COUNTER(STAT_ProjectComp_FireOneShot);

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

	// Block fire while planting/defusing the bomb. The interact lock is set
	// server-side in UpdateInteractLock when the player is holding Use AND
	// overlapping a bomb site, so checking it here gates only the actual
	// plant/defuse window — bumping into the trigger volume without holding
	// Use still allows shooting.
	if (OwnerChar->IsInteractLocked())
	{
		return;
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

	// Random spread cone = movement/air/crouch base + burst ramp. First shot of
	// a burst gets ramp = 0 (tap accuracy preserved); held spray climbs to
	// SpreadRampMaxAdd. Two systems are independent so movement-only and ramp-
	// only contributions stack.
	const float MovementSpread = CalcMovementSpread(OwnerChar);
	const float RampSpread     = CalcSpreadRampAdd();
	const float TotalSpread    = MovementSpread + RampSpread;
	if (TotalSpread > 0.0f)
	{
		const float VertRad  = FMath::DegreesToRadians(TotalSpread);
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

	// 32 slots inlines without heap traffic up through full 10v10 stress
	// (19 other live pawns). Beyond that capacity the allocator kicks in;
	// the registry caps growth to whatever the GameMode admits, so a
	// runaway is not possible.
	TArray<AProjectCompCharacter*, TInlineAllocator<32>> Rewound;
	if (RewindSeconds > KINDA_SMALL_NUMBER)
	{
		// Iterate the GameMode's event-driven LiveCharacters registry instead of
		// TActorIterator. Pre-cached in BeginPlay so this hot-path read is a
		// single weak-ptr resolve, not a global actor scan.
		if (AProjectCompGameMode* GM = CachedAuthGameMode.Get())
		{
			for (const TWeakObjectPtr<AProjectCompCharacter>& WeakTarget : GM->GetLiveCharacters())
			{
				AProjectCompCharacter* Target = WeakTarget.Get();
				if (!Target || Target == OwnerChar)
				{
					continue;
				}
				Target->Server_RewindTo(RewindTime);
				Rewound.Add(Target);
			}
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
	// Pitch and yaw scale independently: VerticalRecoilMult + HorizontalRecoilMult
	// are global knobs; CrouchStillVerticalRecoilMult is a slight pitch-only
	// reduction that fires ONLY when crouched AND below CrouchMoveSpeedThreshold.
	const bool bCrouchedStill = OwnerChar && OwnerChar->bIsCrouched
		&& OwnerChar->GetVelocity().Size2D() < CrouchMoveSpeedThreshold;
	const float PitchScale = VerticalRecoilMult * (bCrouchedStill ? CrouchStillVerticalRecoilMult : 1.0f);
	const float YawScale   = HorizontalRecoilMult;
	ServerRecoilPitch += Entry.X * PitchScale;
	ServerRecoilYaw   += Entry.Y * YawScale;

	// Per-shot Client RPC removed — the Multicast_OnFired below already arrives
	// on the firer's client, and the owning-client side advances its local
	// RecoilPattern mirror there. Saves one Reliable RPC per shot.

	// Advance pattern index, clamp at last entry.
	RecoilPatternIndex = FMath::Min(RecoilPatternIndex + 1, RecoilPattern.Num() - 1);

	// Advance burst counter for the spread ramp. Clamps internally via the
	// helper, but cap here too so the int doesn't grow unbounded over a long spray.
	BurstShotsFired = FMath::Min(BurstShotsFired + 1, SpreadRampShotsToMax);

	// Decrement mag after the shot fully resolves (trace + damage + multicast)
	// so the ammo readout updates in lockstep with bullet effects. Clamped at
	// 0 — the fire loop in Tick gates re-entry so we shouldn't underflow, but
	// guarding is cheap.
	CurrentMagAmmo = FMath::Max(0, CurrentMagAmmo - 1);

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
		// Standing has THREE tiers (still / walking / running) so tac-walk can
		// incur a small cone without being punished like a full sprint.
		// Crouched stays at two tiers (still / moving) — crouch only has one
		// movement speed in practice (shift-crouch caps at crouch-walk anyway).
		const float Speed = Char->GetVelocity().Size2D();
		if (Char->bIsCrouched)
		{
			if (Speed > CrouchMoveSpeedThreshold)
			{
				Spread += CrouchMovingSpreadAdd * TransitionSpreadMultiplier;
			}
		}
		else
		{
			if (Speed > MoveSpeedThreshold)
			{
				Spread += MovingSpreadAdd * TransitionSpreadMultiplier;
			}
			else if (Speed > WalkSpeedThreshold)
			{
				Spread += WalkingSpreadAdd * TransitionSpreadMultiplier;
			}
		}
	}

	// Crouch tightening applies on the ground only. In mid-air, crouching gives
	// no spread bonus — bunny-hop-crouch can't be used to peek with better accuracy.
	const bool bFalling = CharMov && CharMov->IsFalling();
	if (Char->bIsCrouched && !bFalling && Spread > 0.0f)
	{
		Spread *= CrouchSpreadMult;
	}

	return Spread;
}

float AProjectCompHitscanWeapon::CalcSpreadRampAdd() const
{
	if (SpreadRampShotsToMax <= 0 || SpreadRampMaxAdd <= 0.0f)
	{
		return 0.0f;
	}
	const float Frac = FMath::Clamp(
		static_cast<float>(BurstShotsFired) / static_cast<float>(SpreadRampShotsToMax),
		0.0f, 1.0f);
	// SpreadRampCurve < 1 = ease-out (flatter at end), 1 = linear, > 1 = ease-in.
	const float Curved = FMath::Pow(Frac, SpreadRampCurve);
	return SpreadRampMaxAdd * Curved;
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
			// Scale the visual camera kick to match the server-side bullet-dir scaling
			// or crouched players would see a big camera kick while their bullets still
			// group tightly — feels desynced.
			const bool bCrouchedStill = OwnerChar->bIsCrouched
				&& OwnerChar->GetVelocity().Size2D() < CrouchMoveSpeedThreshold;
			const float PitchScale = VerticalRecoilMult * (bCrouchedStill ? CrouchStillVerticalRecoilMult : 1.0f);
			const float YawScale   = HorizontalRecoilMult;
			OwnerChar->ApplyRecoilKickLocal(Entry.X * PitchScale, Entry.Y * YawScale, RecoilRecoveryRate, RecoilRecoveryDelay);
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
