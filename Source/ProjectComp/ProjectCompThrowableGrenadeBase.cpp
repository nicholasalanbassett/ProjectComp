// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompThrowableGrenadeBase.h"
#include "ProjectComp.h"
#include "ProjectCompCharacter.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "TimerManager.h"

// Toggleable projectile-path debug viz. Subclasses keep their own CVars for
// detonation effects (radius spheres, scan rays, count text). This one only
// controls the in-flight tracer line + phase/bounce markers.
static TAutoConsoleVariable<int32> CVarThrowableGrenadeDebugDraw(
	TEXT("pc.ThrowableGrenade.Debug"),
	1,
	TEXT("Draw throwable grenade projectile debug viz (tracer line, phase/bounce markers). 0=off, 1=on"),
	ECVF_Default);

AProjectCompThrowableGrenadeBase::AProjectCompThrowableGrenadeBase()
{
	// Tick is enabled by Server_InitializeExtra once the velocity is stamped.
	// Default-off so a misconfigured spawn (e.g. forgot to call InitializeExtra)
	// doesn't run an idle Tick on a stationary actor.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	ProjectileMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ProjectileMesh"));
	SetRootComponent(ProjectileMesh);
	// Throwable grenades are unbreakable in flight per design — collision is
	// fly-through (NoCollision). Server runs its own per-tick trace for impact
	// detection. Subclasses (e.g. VULN's zone) re-enable collision when they
	// transition to a stationary state.
	ProjectileMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AProjectCompThrowableGrenadeBase::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority())
	{
		SpawnServerTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	}
}

void AProjectCompThrowableGrenadeBase::Server_InitializeExtra(bool bAltFire, const FVector& AimDirection)
{
	if (!HasAuthority())
	{
		return;
	}

	bIsAltFire = bAltFire;

	FVector Dir = AimDirection;
	if (!Dir.Normalize())
	{
		Dir = GetActorForwardVector();
	}
	const float Speed = bIsAltFire ? AltProjectileSpeed : PrimaryProjectileSpeed;
	Velocity = Dir * Speed;
	SetActorRotation(Dir.Rotation());

	Phase = EThrowablePhase::Projectile;
	SetActorTickEnabled(true);

	// Mode label for log readability. Switch by active mode rather than just
	// bIsAltFire — designers may have repurposed primary/alt in a subclass BP.
	const TCHAR* ModeLabel = TEXT("Unknown");
	switch (GetActiveFireMode())
	{
	case EThrowableFireMode::PhaseOneWall: ModeLabel = TEXT("PhaseOneWall"); break;
	case EThrowableFireMode::BounceOnce:   ModeLabel = TEXT("BounceOnce");   break;
	case EThrowableFireMode::GravityLob:   ModeLabel = TEXT("GravityLob");   break;
	}

	UE_LOG(LogProjectComp, Log,
		TEXT("[Throwable] %s launched: slot=%s mode=%s speed=%.0fcm/s gravity=%.0f lifetime=%.1fs fuseAfterImpact=%.1fs"),
		*GetName(),
		bIsAltFire ? TEXT("RMB") : TEXT("LMB"),
		ModeLabel, Speed, GetActiveGravity(), MaxProjectileLifetime, FuseAfterImpact);
}

void AProjectCompThrowableGrenadeBase::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!HasAuthority() || IsDetonated() || Phase != EThrowablePhase::Projectile)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Lifetime safety cap — never let a stray projectile linger forever.
	const float Now = World->GetTimeSeconds();
	if (Now - SpawnServerTime > MaxProjectileLifetime)
	{
		UE_LOG(LogProjectComp, Log,
			TEXT("[Throwable] %s exceeded lifetime — force-impacting at current loc."),
			*GetName());
		HandleImpact(GetActorLocation());
		return;
	}

	// Apply gravity per the active mode. GravityLob uses non-zero; PhaseOneWall
	// and BounceOnce default to 0 (designers can override per-grenade in BP).
	// Acceleration on -Z; Velocity is updated BEFORE the trace so this frame's
	// step already reflects the new vector — keeps integration tight at the
	// scale UE tickrates run at.
	const float Gravity = GetActiveGravity();
	if (Gravity > 0.0f)
	{
		Velocity.Z -= Gravity * DeltaSeconds;
	}

	const FVector PrevLoc = GetActorLocation();
	const FVector NewLoc  = PrevLoc + Velocity * DeltaSeconds;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ThrowableProjectile), /*bTraceComplex*/ false, this);
	if (AActor* MyOwner = GetOwner())
	{
		Params.AddIgnoredActor(MyOwner);
	}
	if (APawn* InstigPawn = GetInstigator())
	{
		Params.AddIgnoredActor(InstigPawn);
	}

	FHitResult Hit;
	const bool bHit = World->LineTraceSingleByChannel(Hit, PrevLoc, NewLoc, ECC_Visibility, Params);

	const EThrowableFireMode Mode = GetActiveFireMode();

#if !UE_BUILD_SHIPPING
	if (CVarThrowableGrenadeDebugDraw.GetValueOnGameThread() != 0)
	{
		// Tracer color encodes mode + state:
		//   PhaseOneWall pre-phase = bright yellow ; post-phase = orange
		//   BounceOnce   pre-bounce = bright cyan  ; post-bounce = light blue
		//   GravityLob              = magenta (no phase/bounce state)
		FColor TracerColor;
		switch (Mode)
		{
		case EThrowableFireMode::PhaseOneWall:
			TracerColor = bHasPhasedWall ? FColor(255, 200, 80) : FColor(255, 255, 80);
			break;
		case EThrowableFireMode::BounceOnce:
			TracerColor = bHasBounced ? FColor(120, 200, 255) : FColor(60, 240, 240);
			break;
		case EThrowableFireMode::GravityLob:
		default:
			TracerColor = FColor(255, 100, 220);
			break;
		}
		DrawDebugLine(World, PrevLoc, NewLoc, TracerColor,
			/*bPersistent*/ false, /*Life*/ 1.5f, 0, /*Thickness*/ 2.0f);
	}
#endif

	if (!bHit)
	{
		SetActorLocation(NewLoc, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
		// Rotate the actor to face its current velocity vector. Matters for
		// gravity arcs (the mesh's pitch tracks the falling trajectory).
		if (Gravity > 0.0f && !Velocity.IsNearlyZero())
		{
			SetActorRotation(Velocity.GetSafeNormal().Rotation());
		}
		return;
	}

	// Hit a character? Impact immediately regardless of mode — players are
	// not surfaces to phase or bounce off of.
	const AActor* HitActor = Hit.GetActor();
	const bool bHitCharacter = HitActor && HitActor->IsA<AProjectCompCharacter>();
	if (bHitCharacter)
	{
		UE_LOG(LogProjectComp, Log,
			TEXT("[Throwable] %s hit character %s — impacting."),
			*GetName(), *GetNameSafe(HitActor));
		HandleImpact(Hit.ImpactPoint);
		return;
	}

	// World geometry hit — dispatch on active fire mode.
	switch (Mode)
	{
	case EThrowableFireMode::PhaseOneWall:
	{
		if (!bHasPhasedWall)
		{
			const FVector WallEntry = Hit.ImpactPoint;
			const FVector Dir = Velocity.GetSafeNormal();
			const FVector FarSide = WallEntry + Dir * MaxWallThickness;

			FHitResult BackHit;
			FCollisionQueryParams BackParams(SCENE_QUERY_STAT(ThrowableProjectileBackTrace), false, this);
			BackParams.AddIgnoredActor(this);
			const bool bFoundBack = World->LineTraceSingleByChannel(
				BackHit, FarSide, WallEntry, ECC_Visibility, BackParams);

			if (!bFoundBack)
			{
				UE_LOG(LogProjectComp, Log,
					TEXT("[Throwable] %s couldn't phase wall (thickness > %.0fcm) — impacting at entry."),
					*GetName(), MaxWallThickness);
				HandleImpact(WallEntry);
				return;
			}

			const FVector ExitPoint = BackHit.ImpactPoint + Dir * 1.0f;
			SetActorLocation(ExitPoint, false, nullptr, ETeleportType::TeleportPhysics);
			bHasPhasedWall = true;

			UE_LOG(LogProjectComp, Verbose,
				TEXT("[Throwable] %s phased wall: entry=%s exit=%s thickness=%.1fcm"),
				*GetName(), *WallEntry.ToCompactString(), *ExitPoint.ToCompactString(),
				(BackHit.ImpactPoint - WallEntry).Size());

#if !UE_BUILD_SHIPPING
			if (CVarThrowableGrenadeDebugDraw.GetValueOnGameThread() != 0)
			{
				DrawDebugSphere(World, WallEntry, 12.0f, 8, FColor(255, 160, 60),
					false, 3.0f, 0, 1.5f);
				DrawDebugSphere(World, BackHit.ImpactPoint, 12.0f, 8, FColor(60, 220, 100),
					false, 3.0f, 0, 1.5f);
				DrawDebugLine(World, WallEntry, BackHit.ImpactPoint, FColor(200, 200, 200),
					false, 3.0f, 0, 1.0f);
			}
#endif
			return;
		}
		// Post-phase contact — impact.
		HandleImpact(Hit.ImpactPoint);
		return;
	}

	case EThrowableFireMode::BounceOnce:
	{
		if (!bHasBounced)
		{
			const FVector Incoming = Velocity.GetSafeNormal();
			const FVector Normal = Hit.ImpactNormal;
			// Standard reflection: R = D - 2*dot(D,N)*N. Preserves magnitude.
			const FVector Reflected = Incoming - 2.0f * FVector::DotProduct(Incoming, Normal) * Normal;
			// Use the current speed magnitude (not the configured PrimaryProjectileSpeed)
			// so a gravity-affected projectile's accumulated speed at bounce-time
			// is preserved. For non-gravity bounce modes (INFO/VULN) this is the
			// configured speed anyway, since the magnitude was constant pre-bounce.
			const float CurrentSpeed = Velocity.Size();
			Velocity = Reflected * CurrentSpeed;

			const FVector StepOff = Hit.ImpactPoint + Reflected * 1.0f;
			SetActorLocation(StepOff, false, nullptr, ETeleportType::TeleportPhysics);
			SetActorRotation(Reflected.Rotation());
			bHasBounced = true;

			UE_LOG(LogProjectComp, Verbose,
				TEXT("[Throwable] %s bounced: at=%s incoming=%s normal=%s reflected=%s"),
				*GetName(), *Hit.ImpactPoint.ToCompactString(),
				*Incoming.ToCompactString(), *Normal.ToCompactString(),
				*Reflected.ToCompactString());

#if !UE_BUILD_SHIPPING
			if (CVarThrowableGrenadeDebugDraw.GetValueOnGameThread() != 0)
			{
				DrawDebugSphere(World, Hit.ImpactPoint, 12.0f, 8, FColor(60, 240, 240),
					false, 3.0f, 0, 1.5f);
				DrawDebugDirectionalArrow(World, Hit.ImpactPoint,
					Hit.ImpactPoint + Normal * 80.0f,
					20.0f, FColor::White, false, 3.0f, 0, 1.5f);
			}
#endif
			return;
		}
		HandleImpact(Hit.ImpactPoint);
		return;
	}

	case EThrowableFireMode::GravityLob:
	default:
		// No phase, no bounce — first contact is the impact.
		HandleImpact(Hit.ImpactPoint);
		return;
	}
}

void AProjectCompThrowableGrenadeBase::HandleImpact(const FVector& ImpactLocation)
{
	if (!HasAuthority() || Phase != EThrowablePhase::Projectile)
	{
		return;
	}

	SetActorLocation(ImpactLocation, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
	Velocity = FVector::ZeroVector;
	SetActorTickEnabled(false);

	UE_LOG(LogProjectComp, Log,
		TEXT("[Throwable] %s impact at %s (fuse=%.1fs)."),
		*GetName(), *ImpactLocation.ToCompactString(), FuseAfterImpact);

	if (FuseAfterImpact > 0.0f)
	{
		TransitionToFuse();
	}
	else
	{
		TransitionToActive();
	}
}

void AProjectCompThrowableGrenadeBase::TransitionToFuse()
{
	Phase = EThrowablePhase::Fused;
	GetWorldTimerManager().SetTimer(FuseTimer, [this]()
	{
		if (HasAuthority() && Phase == EThrowablePhase::Fused)
		{
			TransitionToActive();
		}
	}, FuseAfterImpact, /*bLoop*/ false);
}

void AProjectCompThrowableGrenadeBase::TransitionToActive()
{
	Phase = EThrowablePhase::Active;
	UE_LOG(LogProjectComp, Verbose, TEXT("[Throwable] %s entering Active phase."), *GetName());
	OnEnterActivePhase();
}
