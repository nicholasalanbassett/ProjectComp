// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ProjectCompLoomZoneBase.h"
#include "ProjectCompMinimapEmp.generated.h"

/**
 * MC (Midrange/Control) Loom ability: Minimap EMP.
 *
 * Marks an axis-aligned map rectangle centered on the cast point. While active,
 * enemy Info-grenade scans return ZERO targets for anyone inside the rect — i.e.
 * it hides the casting team's players in that quadrant from the opposing team's
 * info utility. The InfoGrenade scan (server-side) iterates active EMPs and skips
 * counting an enemy whose own team cast an EMP covering their position.
 *
 * Uses a rectangle rather than the base sphere because the design targets a "map
 * quadrant." The rect is queried server-side only (in the scan), so its extent
 * doesn't replicate — only CasterTeam (from the base) and the actor transform.
 */
UCLASS()
class PROJECTCOMP_API AProjectCompMinimapEmp : public AProjectCompLoomZoneBase
{
	GENERATED_BODY()

public:
	AProjectCompMinimapEmp();

	/** Half-extent (cm) of the AABB rect on the XY plane, centered on the actor. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Minimap EMP", meta = (ClampMin = "100.0"))
	FVector2D RectHalfExtent = FVector2D(1500.0f, 1500.0f);

	/** True if WorldPoint's XY falls inside the EMP rectangle. */
	UFUNCTION(BlueprintPure, Category = "Minimap EMP")
	bool IsPointInRect(const FVector& WorldPoint) const;
};
