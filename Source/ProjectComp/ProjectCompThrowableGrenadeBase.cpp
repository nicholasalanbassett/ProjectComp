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
	// Tick is enabled from spawn on EVERY machine. The server does projectile
	// motion (gated by HasAuthority + Phase below); clients use the same Tick
	// to draw a local tracer based on the replicated actor position. Without
	// always-on client tick, in-flight grenades only show tracers on the host.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

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
	// Horizontal facing axis (taken pre-pitch) used to filter velocity inheritance
	// below: only movement along this axis (forward/back) plus vertical (jump)
	// should affect the throw — sideways strafing must not.
	FVector ForwardH(Dir.X, Dir.Y, 0.0f);
	if (!ForwardH.Normalize())
	{
		ForwardH = FVector::ZeroVector;
	}
	// CS2-style upward release: pitch the launch direction UP from the player's
	// aim so the throw arcs (even when aiming level) then falls under gravity.
	// Both throws use it — the primary's smaller bias gives the long overhand
	// arc; the alt's larger bias + lower speed gives the short self-toss.
	const float PitchBias = bIsAltFire ? AltLaunchPitchBias : PrimaryLaunchPitchBias;
	if (PitchBias > 0.0f)
	{
		FRotator LaunchRot = Dir.Rotation();
		LaunchRot.Pitch = FMath::Clamp(LaunchRot.Pitch + PitchBias, -89.0f, 89.0f);
		Dir = LaunchRot.Vector();
	}
	const float Speed = bIsAltFire ? AltProjectileSpeed : PrimaryProjectileSpeed;
	Velocity = Dir * Speed;

	// CS2-style velocity inheritance: add the thrower's movement velocity so a
	// run-throw carries farther, a jump-throw gains height, and a back-pedal throw
	// falls short. Scaled by MovementInheritScalar (1.0 = full carry). We use the
	// PEAK velocity over a short window (MovementInheritWindow) instead of the
	// exact release-frame velocity, so jump/strafe/lineup throws are forgiving on
	// timing — see AProjectCompCharacter::Server_GetWindowedThrowVelocity.
	const float InheritScalar = bIsAltFire ? AltMovementInheritScalar : MovementInheritScalar;
	if (InheritScalar > 0.0f)
	{
		if (const APawn* ThrowerPawn = GetInstigator())
		{
			FVector ThrowerVel = ThrowerPawn->GetVelocity();
			if (const AProjectCompCharacter* ThrowerChar = Cast<AProjectCompCharacter>(ThrowerPawn))
			{
				ThrowerVel = ThrowerChar->Server_GetWindowedThrowVelocity(MovementInheritWindow);
			}
			// Keep ONLY forward/back (movement along the horizontal aim) + vertical
			// (jump) velocity. Discard the sideways/strafe component so strafing
			// doesn't skew the trajectory or add boost.
			FVector FilteredVel(0.0f, 0.0f, ThrowerVel.Z);
			FilteredVel += ForwardH * FVector::DotProduct(ThrowerVel, ForwardH);
			Velocity += FilteredVel * InheritScalar;
		}
	}

	// Face the actual travel direction (post-inheritance) so the mesh points along
	// the real trajectory rather than just the aim.
	if (!Velocity.IsNearlyZero())
	{
		SetActorRotation(Velocity.GetSafeNormal().Rotation());
	}

	Phase = EThrowablePhase::Projectile;
	SetActorTickEnabled(true);

	// Mode label for log readability. Switch by active mode rather than just
	// bIsAltFire — designers may have repurposed primary/alt in a subclass BP.
	const TCHAR* ModeLabel = TEXT("Unknown");
	switch (GetActiveFireMode())
	{
	case EThrowableFireMode::PhaseOneWall: ModeLabel = TEXT("PhaseOneWall"); break;
	case EThrowableFireMode::BounceOnce:   ModeLabel = TEXT("BounceOnce");   break;
	case EThrowableFireMode::DirectImpact: ModeLabel = TEXT("DirectImpact"); break;
	case EThrowableFireMode::StrongThrow:  ModeLabel = TEXT("StrongThrow");  break;
	}

	UE_LOG(LogProjectComp, Log,
		TEXT("[Throwable] %s launched: slot=%s mode=%s speed=%.0fcm/s gravity=%.0f lifetime=%.1fs fuseAfterImpact=%.1fs"),
		*GetName(),
		bIsAltFire ? TEXT("RMB") : TEXT("LMB"),
		ModeLabel, Speed, GetActiveGravity(), MaxProjectileLifetime, FuseAfterImpact);
}

void AProjectCompThrowableGrenadeBase::Server_DeployInPlace(const FVector& Location)
{
	if (!HasAuthority() || Phase != EThrowablePhase::Projectile)
	{
		return;
	}
	// No flight — treat the drop point as an immediate landing. HandleImpact
	// stamps location, stops motion, and runs the fuse → Active transition, so
	// the utility deploys exactly as a thrown one would on landing.
	Velocity = FVector::ZeroVector;
	UE_LOG(LogProjectComp, Log, TEXT("[Throwable] %s global-dropped (deploy-in-place) at %s."),
		*GetName(), *Location.ToCompactString());
	// "From the sky" indicator: a vertical type-colored line down to the landing
	// point, shown on every machine (global drop has no flight arc to telegraph it).
	Multicast_DrawGlobalDropLine(Location);
	HandleImpact(Location);
}

void AProjectCompThrowableGrenadeBase::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Local tracer — runs on EVERY machine (server + each client) so non-host
	// peers see the in-flight trail. Uses the replicated actor position; no
	// extra RPC traffic per tick. Must run BEFORE the HasAuthority gate.
#if !UE_BUILD_SHIPPING
	DrawTracerSegment_Local();
#endif

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

	// Apply gravity per the active mode. Both LMB and the RMB soft lob use
	// non-zero gravity for their arc (the lob just launches slower + pitched up).
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
	// (Tracer drawing now lives in DrawTracerSegment_Local, called above the
	// HasAuthority gate so it runs on every machine.)

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

			// Multicast the phase event so every machine draws the entry/exit
			// markers + connecting line locally. Server's own draw runs via
			// the multicast implementation too (the multicast hits the server
			// in addition to clients).
			Multicast_DrawPhaseEvent(WallEntry, BackHit.ImpactPoint);
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

			Multicast_DrawBounceEvent(Hit.ImpactPoint, Normal);
			return;
		}
		HandleImpact(Hit.ImpactPoint);
		return;
	}

	case EThrowableFireMode::StrongThrow:
	{
		// CS-style bounce-and-settle. Reflect velocity about the surface normal,
		// scaling the normal component by Restitution (bounciness) and shaving
		// the tangential component by TangentialFriction (scuff). Settle when the
		// throw is slow enough or has bounced too many times.
		const FVector N = Hit.ImpactNormal;
		const float VdotN = FVector::DotProduct(Velocity, N);
		const FVector VNormal = VdotN * N;          // into-surface component
		const FVector VTangent = Velocity - VNormal; // along-surface component
		// Outgoing: keep (scuffed) tangent, flip + scale the normal part.
		Velocity = VTangent * (1.0f - TangentialFriction) - VNormal * Restitution;

		++BounceCount;

		// Step off the surface so the next trace doesn't immediately re-hit it.
		const FVector StepOff = Hit.ImpactPoint + N * 1.0f;
		SetActorLocation(StepOff, false, nullptr, ETeleportType::TeleportPhysics);

		Multicast_DrawBounceEvent(Hit.ImpactPoint, N);

		// Settle test — too slow to matter, or bounced its budget out.
		if (Velocity.Size() <= RestSpeed || BounceCount >= MaxBounces)
		{
			UE_LOG(LogProjectComp, Verbose,
				TEXT("[Throwable] %s settled after %d bounce(s) at %s (speed=%.0f)."),
				*GetName(), BounceCount, *StepOff.ToCompactString(), Velocity.Size());
			HandleImpact(StepOff);
		}
		return;
	}

	case EThrowableFireMode::DirectImpact:
	default:
		// No phase, no bounce — first contact is the impact. Arc-vs-flat is
		// decided by the active gravity, not the mode.
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
	// NOTE: tick stays enabled on clients (they don't run server motion anyway,
	// and the local tracer Tick path returns silently with no movement).
	// On the server we keep Tick on too — subclasses (e.g. VULN) may need
	// Tick during Fused/Active phases; they call SetActorTickEnabled(true)
	// themselves in OnEnterActivePhase to make this explicit.
	SetActorTickEnabled(false);

	UE_LOG(LogProjectComp, Log,
		TEXT("[Throwable] %s impact at %s (fuse=%.1fs)."),
		*GetName(), *ImpactLocation.ToCompactString(), FuseAfterImpact);

	// Tell every machine to draw a small marker at the impact point so clients
	// know exactly where the projectile landed (otherwise they just see the
	// in-flight tracer stop, with no clear "this is the impact spot" signal).
	Multicast_DrawImpactMarker(ImpactLocation);

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

// =====================================================================
// Local + multicast debug viz
// =====================================================================

void AProjectCompThrowableGrenadeBase::DrawTracerSegment_Local()
{
#if !UE_BUILD_SHIPPING
	if (CVarThrowableGrenadeDebugDraw.GetValueOnGameThread() == 0)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const FVector CurLoc = GetActorLocation();
	if (bHasLastDrawLocation)
	{
		// Skip near-zero-delta segments — happens while the actor is stationary
		// (pre-launch, post-impact). Threshold lower than projectile speed * dt.
		const float SegmentLengthSq = (CurLoc - LastDrawLocation).SizeSquared();
		if (SegmentLengthSq > 1.0f)
		{
			DrawDebugLine(World, LastDrawLocation, CurLoc, GetTracerColor(),
				/*bPersistent*/ false, /*Life*/ 1.5f, 0, /*Thickness*/ 2.0f);
		}
	}
	LastDrawLocation = CurLoc;
	bHasLastDrawLocation = true;
#endif
}

void AProjectCompThrowableGrenadeBase::Multicast_DrawGlobalDropLine_Implementation(FVector Landing)
{
#if !UE_BUILD_SHIPPING
	if (CVarThrowableGrenadeDebugDraw.GetValueOnGameThread() == 0)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// ~40m straight up so it reads clearly as "dropped from the sky." Type-colored,
	// ~3s lifetime — a momentary incoming indicator at the cast point.
	constexpr float SkyHeight = 4000.0f;
	const FVector Top = Landing + FVector(0.0f, 0.0f, SkyHeight);
	DrawDebugLine(World, Top, Landing, GetTracerColor(),
		/*bPersistent*/ false, /*LifeTime*/ 3.0f, 0, /*Thickness*/ 3.0f);
#endif
}

void AProjectCompThrowableGrenadeBase::Multicast_DrawPhaseEvent_Implementation(FVector WallEntry, FVector ExitPoint)
{
#if !UE_BUILD_SHIPPING
	if (CVarThrowableGrenadeDebugDraw.GetValueOnGameThread() == 0)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// Orange entry sphere, green exit sphere, grey line through the wall.
	DrawDebugSphere(World, WallEntry, 12.0f, 8, FColor(255, 160, 60),
		false, 3.0f, 0, 1.5f);
	DrawDebugSphere(World, ExitPoint, 12.0f, 8, FColor(60, 220, 100),
		false, 3.0f, 0, 1.5f);
	DrawDebugLine(World, WallEntry, ExitPoint, FColor(200, 200, 200),
		false, 3.0f, 0, 1.0f);
#endif
}

void AProjectCompThrowableGrenadeBase::Multicast_DrawBounceEvent_Implementation(FVector ImpactAt, FVector ImpactNormal)
{
#if !UE_BUILD_SHIPPING
	if (CVarThrowableGrenadeDebugDraw.GetValueOnGameThread() == 0)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	DrawDebugSphere(World, ImpactAt, 12.0f, 8, FColor(60, 240, 240),
		false, 3.0f, 0, 1.5f);
	DrawDebugDirectionalArrow(World, ImpactAt, ImpactAt + ImpactNormal * 80.0f,
		20.0f, FColor::White, false, 3.0f, 0, 1.5f);
#endif
}

void AProjectCompThrowableGrenadeBase::Multicast_DrawImpactMarker_Implementation(FVector ImpactAt)
{
#if !UE_BUILD_SHIPPING
	if (CVarThrowableGrenadeDebugDraw.GetValueOnGameThread() == 0)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// Small magenta sphere at the impact point — clearly distinct from the
	// phase (orange/green) and bounce (cyan) markers.
	DrawDebugSphere(World, ImpactAt, 10.0f, 10, FColor(255, 60, 200),
		false, 3.0f, 0, 2.0f);
#endif
}
