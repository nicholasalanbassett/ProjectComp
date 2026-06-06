// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProjectCompLoomZoneBase.h"
#include "ProjectCompAcousticZone.generated.h"

/**
 * AA (Aggro/Aggro) Loom ability: Acoustic Deafening.
 *
 * A passive marker zone. The gunfire-mute is decided client-side: when a client
 * receives a weapon's cosmetic Multicast_OnFired, it checks whether the LOCAL
 * listener's pawn is standing inside an ENEMY Acoustic Deafening zone via
 * AProjectCompCharacter::IsInsideEnemyAcousticZone() and, if so, skips the shot
 * SFX. Net effect: enemies inside the zone don't hear your pushing teammates'
 * gunfire, so the audio cue doesn't draw their rotations.
 *
 * (Audio is client-local, so the zone holds no per-listener state — it's just a
 * replicated, queryable volume. Gunfire SFX itself is authored on the weapon BP
 * via AProjectCompHitscanWeapon::OnFiredCosmetic, which honors the mute flag.)
 */
UCLASS()
class PROJECTCOMP_API AProjectCompAcousticZone : public AProjectCompLoomZoneBase
{
	GENERATED_BODY()

public:
	AProjectCompAcousticZone();
};
