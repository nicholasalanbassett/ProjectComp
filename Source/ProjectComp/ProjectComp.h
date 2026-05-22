// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"

/** Main log category used across the project */
DECLARE_LOG_CATEGORY_EXTERN(LogProjectComp, Log, All);

/** Object channel reserved for looming (dead) pawns. Configured in DefaultEngine.ini. */
#define ECC_DeadPawn ECC_GameTraceChannel2