using UnrealBuildTool;

public class MonolithMetaSound : ModuleRules
{
	public MonolithMetaSound(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MonolithCore",
			"UnrealEd",
			"MetasoundEngine",
			"MetasoundFrontend",
			"MetasoundGraphCore",
			"MetasoundEditor",
			"Json",
			"JsonUtilities",
			"AssetTools",
			"AssetRegistry",
			"Slate",
			"SlateCore"
		});
	}
}
