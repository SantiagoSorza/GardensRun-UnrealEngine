// Copyright 2026 Nwiro. All Rights Reserved.

using UnrealBuildTool;

public class NwiroRuntime : ModuleRules
{
	public NwiroRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Disable -Wshadow and -Wreorder-ctor warnings.
		// -Wshadow is triggered by Epic's own PCGPreconfiguration.h (engine bug in UE 5.2+)
		// -Wreorder-ctor is overly strict for our use case.
		bUseUnity = false;
		ShadowVariableWarningLevel = WarningLevel.Off;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"PCG",
			}
		);
	}
}
