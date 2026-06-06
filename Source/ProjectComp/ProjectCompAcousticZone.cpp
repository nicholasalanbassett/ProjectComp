// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompAcousticZone.h"

AProjectCompAcousticZone::AProjectCompAcousticZone()
{
	ZoneRadius   = 700.0f;
	ZoneLifetime = 8.0f;
	WindupDuration = 0.0f;
	SpawnZLift   = 100.0f;
	DebugColor   = FColor(255, 140, 40);  // orange
}
