// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerStart.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompPlayerStart.generated.h"

/**
 * PlayerStart tagged with a team. The GameMode's ChoosePlayerStart override
 * filters by TeamID so attackers/defenders spawn on the correct side of the
 * map. Spectator-tagged starts are used as fallbacks for unassigned players.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompPlayerStart : public APlayerStart
{
	GENERATED_BODY()

public:
	AProjectCompPlayerStart(const FObjectInitializer& ObjectInitializer);

	ETeamID GetTeamID() const { return TeamID; }

protected:

	/** Which team spawns at this location. Set per-instance in the level editor. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Team")
	ETeamID TeamID = ETeamID::Spectator;
};
