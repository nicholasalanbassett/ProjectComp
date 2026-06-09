// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompVulnerableGrenade.h"
#include "ProjectComp.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompPlayerState.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

// Toggle for the neon coral zone visualization. DEV-ONLY, OFF by default — the
// detonation/zone wireframe is too disruptive during playtests. Flip to 1 in the
// console to debug the zone. Independent from the throwable base's projectile CVar.
static TAutoConsoleVariable<int32> CVarVulnerableGrenadeDebugDraw(
	TEXT("pc.VulnerableGrenade.Debug"),
	0,
	TEXT("Draw vulnerable grenade zone viz (neon coral sphere while active). DEV-ONLY, off by default — too disruptive in playtests. 0=off, 1=on"),
	ECVF_Default);

// Design palette: neon coral. Saturated pinkish-orange, distinct from
// FLASH cyan and INFO cyan. RGB ~(255, 90, 60) reads clearly on default
// engine grey lighting.
static constexpr FColor NeonCoral_Bright = FColor(255, 96, 64, 255);
static constexpr FColor NeonCoral_Dim    = FColor(180, 64, 40, 255);

AProjectCompVulnerableGrenade::AProjectCompVulnerableGrenade()
{
	// 1s impact-to-activate window per design — gives enemies a brief warning
	// before the damage zone arms. BP can override per-instance.
	FuseAfterImpact = 1.0f;

	// AOE sphere — overlap-only. Disabled during flight; OnEnterActivePhase
	// enables event generation when the zone activates. Attaches to the
	// inherited ProjectileMesh root.
	ZoneSphere = CreateDefaultSubobject<USphereComponent>(TEXT("ZoneSphere"));
	ZoneSphere->SetupAttachment(GetRootComponent());
	ZoneSphere->InitSphereRadius(ZoneRadius);
	ZoneSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ZoneSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	ZoneSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	ZoneSphere->SetGenerateOverlapEvents(false);

	SetReplicateMovement(true);  // movement matters during flight; harmless once stationary
}

void AProjectCompVulnerableGrenade::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AProjectCompVulnerableGrenade, bIsZoneActive);
}

void AProjectCompVulnerableGrenade::BeginPlay()
{
	Super::BeginPlay();
	// Enable tick on both server and client so the per-tick zone viz can run
	// everywhere. The base's Tick early-returns on the client for the
	// projectile-motion path, so the cost on non-server is just our override.
	SetActorTickEnabled(true);
}

void AProjectCompVulnerableGrenade::Tick(float DeltaSeconds)
{
	// Base does projectile motion (server only, projectile phase only).
	Super::Tick(DeltaSeconds);

#if !UE_BUILD_SHIPPING
	if (!bIsZoneActive || CVarVulnerableGrenadeDebugDraw.GetValueOnGameThread() == 0)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const FVector Center = GetActorLocation();
	// Neon coral wireframe sphere — per-tick draw (one-frame lifetime), so the
	// zone is continuously visible as long as bIsZoneActive is true. 24-segment
	// resolution reads cleanly without being a perf hog.
	DrawDebugSphere(World, Center, ZoneRadius, 24, NeonCoral_Bright,
		/*bPersistent*/ false, /*LifeTime*/ -1.0f, 0, /*Thickness*/ 3.0f);
	// Center marker — solid small sphere so the device origin is obvious.
	DrawDebugSphere(World, Center, 22.0f, 10, NeonCoral_Dim,
		false, -1.0f, 0, 2.0f);
#endif
}

void AProjectCompVulnerableGrenade::OnEnterActivePhase()
{
	if (!HasAuthority())
	{
		return;
	}

	if (ZoneSphere)
	{
		// Apply BP-tuned radius now (constructor uses C++ default; BP override
		// has landed by the time we hit Active).
		ZoneSphere->SetSphereRadius(ZoneRadius, /*bUpdateOverlaps*/ false);
		ZoneSphere->SetGenerateOverlapEvents(true);

		ZoneSphere->OnComponentBeginOverlap.AddDynamic(this, &AProjectCompVulnerableGrenade::OnZoneBeginOverlap);
		ZoneSphere->OnComponentEndOverlap.AddDynamic(this, &AProjectCompVulnerableGrenade::OnZoneEndOverlap);

		// Catch anyone already standing in the zone at activation time —
		// without this, a defender placed under a stationary attacker wouldn't
		// tag them until they moved.
		TArray<AActor*> AlreadyOverlapping;
		ZoneSphere->GetOverlappingActors(AlreadyOverlapping, AProjectCompCharacter::StaticClass());
		for (AActor* Other : AlreadyOverlapping)
		{
			OnZoneBeginOverlap(ZoneSphere, Other, nullptr, INDEX_NONE, false, FHitResult());
		}
	}

	// Become bullet-blocking now that we're an active device — the inherited
	// ProjectileMesh was NoCollision during flight (so the projectile passes
	// through everything cleanly). Enable Query collision + block Visibility
	// so hitscan can register us as a target. The 1-HP semantics live in
	// TakeDamage (which only breaks while Phase == Active).
	if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(GetRootComponent()))
	{
		Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Mesh->SetCollisionObjectType(ECC_WorldDynamic);
		Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
		Mesh->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		// Players walk through the device — design intent is the zone, not the prop.
		Mesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	}

	// Flip the replicated active flag so every client's Tick draws the neon
	// coral zone viz. Without this, the zone would be invisible to non-server
	// peers since DrawDebugXXX doesn't replicate.
	bIsZoneActive = true;

	// Re-enable Tick on the server (the throwable base disabled it in
	// HandleImpact before the fuse started). Without this re-enable, the
	// listen-server host's local viewer wouldn't see the per-tick zone
	// viz — only the PIE client windows would. Clients keep tick on from
	// BeginPlay (SetActorTickEnabled isn't replicated), so they're fine.
	SetActorTickEnabled(true);

	// Auto-detonate at lifetime expiry. Can be cut short by being shot out
	// (TakeDamage → Server_Detonate).
	if (ZoneLifetime > 0.0f)
	{
		GetWorldTimerManager().SetTimer(LifetimeTimer, [this]()
		{
			UE_LOG(LogProjectComp, Log, TEXT("[VulnerableGrenade] %s lifetime expired."), *GetName());
			Server_Detonate();
		}, ZoneLifetime, false);
	}

	UE_LOG(LogProjectComp, Log,
		TEXT("[VulnerableGrenade] %s zone active at %s — radius %.0fcm, lifetime %.1fs."),
		*GetName(), *GetActorLocation().ToCompactString(), ZoneRadius, ZoneLifetime);
}

void AProjectCompVulnerableGrenade::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Defensive: if the actor tears down outside the normal Server_Detonate path
	// (level transition, etc.), still decrement counters so characters don't
	// keep a stale 2x multiplier.
	if (HasAuthority())
	{
		for (const TWeakObjectPtr<AProjectCompCharacter>& WeakChar : TrackedOverlaps)
		{
			if (AProjectCompCharacter* Char = WeakChar.Get())
			{
				Char->OnVulnerableZoneOverlapChanged(/*bEntered*/ false);
			}
		}
		TrackedOverlaps.Reset();
	}

	Super::EndPlay(EndPlayReason);
}

void AProjectCompVulnerableGrenade::OnZoneBeginOverlap(UPrimitiveComponent* OverlappedComponent,
                                                       AActor* OtherActor,
                                                       UPrimitiveComponent* OtherComp,
                                                       int32 OtherBodyIndex,
                                                       bool bFromSweep,
                                                       const FHitResult& SweepResult)
{
	if (!HasAuthority() || GetPhase() != EThrowablePhase::Active)
	{
		return;
	}
	AProjectCompCharacter* Char = Cast<AProjectCompCharacter>(OtherActor);
	if (!Char)
	{
		return;
	}

	// Enemies only — friendlies are unaffected per design.
	const APlayerController* PC = Cast<APlayerController>(Char->GetController());
	const AProjectCompPlayerState* PS = PC ? PC->GetPlayerState<AProjectCompPlayerState>() : nullptr;
	if (!PS)
	{
		return;
	}
	const ETeamID OtherTeam = PS->GetTeamID();
	if (OtherTeam == ETeamID::Spectator || OtherTeam == GetOwnerTeam())
	{
		return;
	}

	const TWeakObjectPtr<AProjectCompCharacter> Weak(Char);
	if (!TrackedOverlaps.Contains(Weak))
	{
		TrackedOverlaps.Add(Weak);
		Char->OnVulnerableZoneOverlapChanged(/*bEntered*/ true);
		UE_LOG(LogProjectComp, Verbose,
			TEXT("[VulnerableGrenade] %s tagged enemy '%s' as vulnerable."),
			*GetName(), *Char->GetName());
	}
}

void AProjectCompVulnerableGrenade::OnZoneEndOverlap(UPrimitiveComponent* OverlappedComponent,
                                                     AActor* OtherActor,
                                                     UPrimitiveComponent* OtherComp,
                                                     int32 OtherBodyIndex)
{
	if (!HasAuthority())
	{
		return;
	}
	AProjectCompCharacter* Char = Cast<AProjectCompCharacter>(OtherActor);
	if (!Char)
	{
		return;
	}

	const TWeakObjectPtr<AProjectCompCharacter> Weak(Char);
	if (TrackedOverlaps.Remove(Weak) > 0)
	{
		Char->OnVulnerableZoneOverlapChanged(/*bEntered*/ false);
		UE_LOG(LogProjectComp, Verbose,
			TEXT("[VulnerableGrenade] %s untagged '%s'."),
			*GetName(), *Char->GetName());
	}
}

float AProjectCompVulnerableGrenade::TakeDamage(float DamageAmount,
                                                 const FDamageEvent& DamageEvent,
                                                 AController* EventInstigator,
                                                 AActor* DamageCauser)
{
	// 1-HP semantics, but ONLY in Active phase — during Projectile/Fused the
	// mesh is NoCollision so this shouldn't fire, but guard anyway in case
	// some future damage source bypasses the collision channel.
	if (HasAuthority() && GetPhase() == EThrowablePhase::Active
	    && DamageAmount > 0.0f && !IsDetonated())
	{
		UE_LOG(LogProjectComp, Log,
			TEXT("[VulnerableGrenade] %s shot out by %s (dmg=%.1f)."),
			*GetName(), *GetNameSafe(EventInstigator), DamageAmount);
		Server_Detonate();
	}
	return Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
}

void AProjectCompVulnerableGrenade::HandleDetonationGameplay()
{
	// Clear the replicated active flag so clients stop drawing the zone viz.
	bIsZoneActive = false;

	// Cleanup pass — clear the multiplier on everyone we tagged. EndPlay also
	// does this defensively for unusual teardowns; in normal flow this is
	// where the counter decrements happen.
	for (const TWeakObjectPtr<AProjectCompCharacter>& WeakChar : TrackedOverlaps)
	{
		if (AProjectCompCharacter* Char = WeakChar.Get())
		{
			Char->OnVulnerableZoneOverlapChanged(/*bEntered*/ false);
		}
	}
	TrackedOverlaps.Reset();
}
