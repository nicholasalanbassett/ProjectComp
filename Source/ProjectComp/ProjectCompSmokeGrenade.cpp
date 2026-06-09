// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompSmokeGrenade.h"
#include "ProjectComp.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

// Dev-only wireframe overlay for the smoke radius. The real visual is now the
// SmokeDomeMesh (an opaque sphere), so this defaults OFF — flip to 1 to debug
// the exact radius. Independent from the throwable base's projectile-path CVar.
static TAutoConsoleVariable<int32> CVarSmokeGrenadeDebugDraw(
	TEXT("pc.SmokeGrenade.Debug"),
	0,
	TEXT("Draw smoke grenade radius wireframe (dev overlay; the dome mesh is the real visual). 0=off, 1=on"),
	ECVF_Default);

// Smoke palette: desaturated grey, clearly distinct from FLASH cyan, INFO cyan,
// and VULN neon-coral.
static constexpr FColor SmokeGrey_Bright = FColor(200, 200, 205, 255);
static constexpr FColor SmokeGrey_Dim    = FColor(120, 120, 128, 255);

AProjectCompSmokeGrenade::AProjectCompSmokeGrenade()
{
	// Short bloom delay after landing — gives a brief "it's about to smoke" tell
	// and lets the projectile settle before the volume snaps on.
	FuseAfterImpact = 0.5f;

	// Vision-block volume — overlap/trace-only, disabled during flight. The
	// projectile mesh root stays NoCollision (inherited) so the grenade flies
	// through everything; this volume arms only in OnEnterActivePhase.
	SmokeVolume = CreateDefaultSubobject<USphereComponent>(TEXT("SmokeVolume"));
	SmokeVolume->SetupAttachment(GetRootComponent());
	SmokeVolume->InitSphereRadius(SmokeRadius);
	SmokeVolume->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// Block ONLY the VisionBlock channel — never ECC_Visibility (bullets) or
	// ECC_Pawn (players walk through). This is what makes smoke deny sight
	// without stopping gunfire.
	SmokeVolume->SetCollisionObjectType(ECC_WorldDynamic);
	SmokeVolume->SetCollisionResponseToAllChannels(ECR_Ignore);
	SmokeVolume->SetCollisionResponseToChannel(ECC_VisionBlock, ECR_Block);
	SmokeVolume->SetGenerateOverlapEvents(false);

	// Cosmetic dome — an opaque sphere shown while active. Engine sphere is 50cm
	// radius at scale 1 and single-sided, so it's solid from outside but "open"
	// (see-through) from inside. NoCollision; SmokeVolume does the real vision
	// block. Hidden during flight; UpdateSmokeDomeVisual() shows it on Active.
	// BP can swap the material for a proper translucent smoke look.
	SmokeDomeMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SmokeDomeMesh"));
	SmokeDomeMesh->SetupAttachment(GetRootComponent());
	SmokeDomeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SmokeDomeMesh->SetVisibility(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere"));
	if (SphereFinder.Succeeded())
	{
		SmokeDomeMesh->SetStaticMesh(SphereFinder.Object);
	}
	SmokeDomeMesh->SetRelativeScale3D(FVector(SmokeRadius / 50.0f));

	SetReplicateMovement(true);  // matters during flight; harmless once stationary
}

void AProjectCompSmokeGrenade::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AProjectCompSmokeGrenade, bIsSmokeActive);
}

void AProjectCompSmokeGrenade::BeginPlay()
{
	Super::BeginPlay();
	// Tick on every machine so the per-tick cloud viz runs everywhere (the base
	// Tick early-returns on clients for the projectile-motion path).
	SetActorTickEnabled(true);
}

void AProjectCompSmokeGrenade::Tick(float DeltaSeconds)
{
	// Base does projectile motion (server only, projectile phase only).
	Super::Tick(DeltaSeconds);

#if !UE_BUILD_SHIPPING
	if (!bIsSmokeActive || CVarSmokeGrenadeDebugDraw.GetValueOnGameThread() == 0)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const FVector Center = GetActorLocation();
	// Grey wireframe sphere — per-tick draw (one-frame lifetime) so the cloud is
	// continuously visible while bIsSmokeActive. Stand-in for real VFX wired in BP.
	DrawDebugSphere(World, Center, SmokeRadius, 24, SmokeGrey_Bright,
		/*bPersistent*/ false, /*LifeTime*/ -1.0f, 0, /*Thickness*/ 3.0f);
	DrawDebugSphere(World, Center, 22.0f, 10, SmokeGrey_Dim,
		false, -1.0f, 0, 2.0f);
#endif
}

void AProjectCompSmokeGrenade::OnEnterActivePhase()
{
	if (!HasAuthority())
	{
		return;
	}

	if (SmokeVolume)
	{
		// Apply BP-tuned radius now (constructor used the C++ default; a BP
		// override has landed by the time we reach Active).
		SmokeVolume->SetSphereRadius(SmokeRadius, /*bUpdateOverlaps*/ false);
		// Arm the vision block. QueryOnly is enough for line traces.
		SmokeVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	}

	// Flip the replicated flag — RepNotify shows the dome on clients. Set the
	// dome scale from the (possibly BP-overridden) radius before showing it.
	bIsSmokeActive = true;
	UpdateSmokeDomeVisual();  // authority: OnRep won't fire on the server

	// Re-enable Tick on the server (the throwable base disabled it in
	// HandleImpact before the fuse). Clients keep tick on from BeginPlay.
	SetActorTickEnabled(true);

	// Dissipate after SmokeDuration — detonation clears the volume + cleans up.
	GetWorldTimerManager().SetTimer(DissipateTimer, [this]()
	{
		UE_LOG(LogProjectComp, Log, TEXT("[SmokeGrenade] %s dissipated."), *GetName());
		Server_Detonate();
	}, SmokeDuration, /*bLoop*/ false);

	UE_LOG(LogProjectComp, Log,
		TEXT("[SmokeGrenade] %s bloomed at %s — radius %.0fcm, duration %.1fs."),
		*GetName(), *GetActorLocation().ToCompactString(), SmokeRadius, SmokeDuration);
}

void AProjectCompSmokeGrenade::HandleDetonationGameplay()
{
	// Clear the vision block + flag so sight returns and the dome hides. The base
	// schedules Destroy() after CleanupDelay.
	bIsSmokeActive = false;
	UpdateSmokeDomeVisual();  // authority: OnRep won't fire on the server
	if (SmokeVolume)
	{
		SmokeVolume->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

void AProjectCompSmokeGrenade::OnRep_SmokeActive()
{
	// Clients: show/hide the dome to match the replicated active flag.
	UpdateSmokeDomeVisual();
}

void AProjectCompSmokeGrenade::UpdateSmokeDomeVisual()
{
	if (!SmokeDomeMesh)
	{
		return;
	}
	// Engine sphere is 50cm radius at scale 1 — match the (BP-overridable) radius.
	SmokeDomeMesh->SetRelativeScale3D(FVector(SmokeRadius / 50.0f));
	SmokeDomeMesh->SetVisibility(bIsSmokeActive);
}
