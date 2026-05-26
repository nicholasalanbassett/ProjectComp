// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompVulnerableGrenade.h"
#include "ProjectComp.h"
#include "ProjectCompCharacter.h"
#include "ProjectCompPlayerState.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"

AProjectCompVulnerableGrenade::AProjectCompVulnerableGrenade()
{
	PrimaryActorTick.bCanEverTick = false;

	// Device mesh — bullet-stopping. Bullets hit this and route to TakeDamage
	// (we override below). 1-HP semantics live in the override.
	DeviceMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DeviceMesh"));
	SetRootComponent(DeviceMesh);
	DeviceMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	DeviceMesh->SetCollisionObjectType(ECC_WorldDynamic);
	DeviceMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	// Block weapon traces so hitscan can register us as a target.
	DeviceMesh->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	// Players walk through the device — design intent is the zone, not the prop.
	DeviceMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);

	// AOE sphere — overlap-only.
	ZoneSphere = CreateDefaultSubobject<USphereComponent>(TEXT("ZoneSphere"));
	ZoneSphere->SetupAttachment(DeviceMesh);
	ZoneSphere->InitSphereRadius(ZoneRadius);
	ZoneSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ZoneSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	ZoneSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	ZoneSphere->SetGenerateOverlapEvents(true);

	bReplicates = true;
	SetReplicateMovement(false);  // stationary once placed
}

void AProjectCompVulnerableGrenade::BeginPlay()
{
	Super::BeginPlay();

	if (ZoneSphere)
	{
		// Apply BP-tuned ZoneRadius to the sphere (constructor may have used
		// the C++ default before the BP CDO overrode it).
		ZoneSphere->SetSphereRadius(ZoneRadius, /*bUpdateOverlaps*/ false);
	}

	if (HasAuthority() && ZoneSphere)
	{
		// Overlap binding is server-only — the multiplier is server-authoritative
		// state and clients learn it via the eventual damage outcome.
		ZoneSphere->OnComponentBeginOverlap.AddDynamic(this, &AProjectCompVulnerableGrenade::OnZoneBeginOverlap);
		ZoneSphere->OnComponentEndOverlap.AddDynamic(this, &AProjectCompVulnerableGrenade::OnZoneEndOverlap);

		// Sanity: immediately apply the multiplier to anyone standing in the
		// zone at spawn time. Without this, a defender placing the zone on a
		// stationary attacker wouldn't tag them until they moved.
		TArray<AActor*> AlreadyOverlapping;
		ZoneSphere->GetOverlappingActors(AlreadyOverlapping, AProjectCompCharacter::StaticClass());
		for (AActor* Other : AlreadyOverlapping)
		{
			OnZoneBeginOverlap(ZoneSphere, Other, nullptr, INDEX_NONE, false, FHitResult());
		}

		// Schedule the natural-expiry detonate so zones don't outlive their welcome.
		if (ZoneLifetime > 0.0f)
		{
			GetWorldTimerManager().SetTimer(LifetimeTimer, [this]()
			{
				UE_LOG(LogProjectComp, Log, TEXT("[VulnerableGrenade] %s lifetime expired."), *GetName());
				Server_Detonate();
			}, ZoneLifetime, false);
		}

		UE_LOG(LogProjectComp, Log,
			TEXT("[VulnerableGrenade] %s active at %s — radius %.0fcm, lifetime %.1fs."),
			*GetName(), *GetActorLocation().ToCompactString(), ZoneRadius, ZoneLifetime);
	}
}

void AProjectCompVulnerableGrenade::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Defensive: if the actor is being torn down without going through
	// Server_Detonate (e.g. level transition), still decrement the tracked
	// overlaps so characters don't keep a stale multiplier.
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
	if (!HasAuthority())
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

	// Idempotent: only tag once per char per zone instance. Prevents double-
	// increment if UE fires duplicate overlaps (rare but defensive).
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
	// Any damage from any source breaks the device. 1-HP semantics — design
	// memory specifies "1 bullet" so we don't fuss about HP arithmetic.
	if (HasAuthority() && DamageAmount > 0.0f && !IsDetonated())
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
	// Cleanup pass: clear the multiplier on everyone we tagged. EndPlay also
	// does this defensively for unusual teardowns; in normal flow, this is
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
