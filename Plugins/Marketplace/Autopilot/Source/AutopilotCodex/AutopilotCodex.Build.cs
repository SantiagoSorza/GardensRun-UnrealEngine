using UnrealBuildTool;

// The AutopilotCodex commandlet lives in its own editor module loaded at the Default phase: commandlets are looked up
// before PostEngineInit modules (AutopilotPlaytestEditor) load.
public class AutopilotCodex : ModuleRules
{
	public AutopilotCodex(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"Projects",
			"AssetRegistry",
			"BlueprintGraph",
			"UMG",
			"UMGEditor",
			"EnhancedInput",
			"InputCore",
		});
	}
}
