// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "Stats/Stats.h"

/** Main log category used across the project */
DECLARE_LOG_CATEGORY_EXTERN(LogProjectComp, Log, All);

/** Object channel reserved for looming (dead) pawns. Configured in DefaultEngine.ini. */
#define ECC_DeadPawn ECC_GameTraceChannel2

/** Trace channel that ONLY smoke volumes block (DefaultResponse=Ignore in
 *  DefaultEngine.ini). Vision/scan checks (Flash LOS, Info scan) trace against
 *  this to be occluded by smoke WITHOUT affecting bullets, which trace on
 *  ECC_Visibility — so smoke blocks sight but not gunfire. Configured in
 *  DefaultEngine.ini as ECC_GameTraceChannel3 ("VisionBlock"). */
#define ECC_VisionBlock ECC_GameTraceChannel3

/**
 * Stats group + cycle counters for the gameplay hot paths flagged by the
 * Principal-Engineer audit. SCOPE_CYCLE_COUNTER compiles to nothing in
 * shipping (STATS=0), so dropping these into FireOneShot / the lag-comp
 * snapshot loop is free in production but gives `stat ProjectComp` a precise
 * read in dev/test builds — the only place a sub-500us budget can be verified.
 */
DECLARE_STATS_GROUP(TEXT("ProjectComp"), STATGROUP_ProjectComp, STATCAT_Advanced);
DECLARE_CYCLE_STAT_EXTERN(TEXT("FireOneShot"),          STAT_ProjectComp_FireOneShot,          STATGROUP_ProjectComp, PROJECTCOMP_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("LagCompSnapshot"),      STAT_ProjectComp_LagCompSnapshot,      STATGROUP_ProjectComp, PROJECTCOMP_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("FlashDomeTickApply"),   STAT_ProjectComp_FlashDomeTickApply,   STATGROUP_ProjectComp, PROJECTCOMP_API);