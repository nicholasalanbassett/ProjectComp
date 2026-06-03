// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

// Dedicated-server build target. Produces a headless ProjectCompServer binary
// (no rendering RHI / audio) for hosting matches. Build with:
//   Build.bat ProjectCompServer Win64 Development -Project="...\ProjectComp.uproject"
public class ProjectCompServerTarget : TargetRules
{
	public ProjectCompServerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Server;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
		ExtraModuleNames.Add("ProjectComp");
	}
}
