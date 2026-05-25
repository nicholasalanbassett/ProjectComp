// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectComp.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_PRIMARY_GAME_MODULE( FDefaultGameModuleImpl, ProjectComp, "ProjectComp" );

DEFINE_LOG_CATEGORY(LogProjectComp)

// Cycle stat definitions paired with the DECLARE_CYCLE_STAT_EXTERN in ProjectComp.h.
// Shipping builds (STATS=0) compile these out alongside their SCOPE_CYCLE_COUNTER sites.
DEFINE_STAT(STAT_ProjectComp_FireOneShot);
DEFINE_STAT(STAT_ProjectComp_LagCompSnapshot);
DEFINE_STAT(STAT_ProjectComp_FlashDomeTickApply);