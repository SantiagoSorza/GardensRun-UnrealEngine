using UnrealBuildTool;

public class AutopilotPlaytest : ModuleRules
{
	public AutopilotPlaytest(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"DeveloperSettings",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"WebSockets",
			"AssetRegistry",
			"EnhancedInput",
			"InputCore",
			"ApplicationCore",
			"Slate",
			"SlateCore",
			"UMG",
			"Projects",
			"EngineSettings",
			"PhysicsCore",
			"RHI",
			"RenderCore",
			"ImageWrapper",
		});
	}
}
