// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompMinimapEmp.h"

AProjectCompMinimapEmp::AProjectCompMinimapEmp()
{
	// ZoneRadius drives only the debug-sphere viz here; the real query is the rect.
	// Sized roughly to the rect's diagonal so the debug viz reads sensibly.
	ZoneRadius   = 2000.0f;
	ZoneLifetime = 15.0f;
	WindupDuration = 0.0f;
	SpawnZLift   = 100.0f;
	DebugColor   = FColor(60, 220, 200);  // teal
}

bool AProjectCompMinimapEmp::IsPointInRect(const FVector& WorldPoint) const
{
	const FVector Center = GetActorLocation();
	return FMath::Abs(WorldPoint.X - Center.X) <= RectHalfExtent.X
		&& FMath::Abs(WorldPoint.Y - Center.Y) <= RectHalfExtent.Y;
}
