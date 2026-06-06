// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProjectCompLoomZoneBase.h"
#include "ProjectCompInteractJammer.generated.h"

/**
 * CM (Control/Midrange) Loom ability: Interact Jammer.
 *
 * A passive marker zone. While an enemy of the caster stands inside it, the
 * server refuses to validate their F-interact input (plant / defuse / bomb
 * pickup / ground-weapon pickup). The check is a server-side spatial query —
 * AProjectCompCharacter::IsInsideEnemyInteractJammer() — consulted in the three
 * authoritative interact routes (CanInteractWithBombNow, Server_TryInteract, and
 * the bomb site's FindActiveInteractor). No replicated per-player state needed:
 * interactions are key-press-rare, so a per-call iterate is cheap.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompInteractJammer : public AProjectCompLoomZoneBase
{
	GENERATED_BODY()

public:
	AProjectCompInteractJammer();
};
