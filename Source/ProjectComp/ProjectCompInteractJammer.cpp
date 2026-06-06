// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompInteractJammer.h"

AProjectCompInteractJammer::AProjectCompInteractJammer()
{
	ZoneRadius   = 600.0f;
	ZoneLifetime = 10.0f;
	WindupDuration = 0.0f;
	SpawnZLift   = 100.0f;
	DebugColor   = FColor(220, 60, 220);  // magenta
}
