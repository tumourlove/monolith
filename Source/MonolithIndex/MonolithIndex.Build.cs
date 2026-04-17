using UnrealBuildTool;

public class MonolithIndex : ModuleRules
{
	public MonolithIndex(ReadOnlyTargetRules Target) : base(Target)
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
			"AssetRegistry",
			"Json",
			"JsonUtilities",
			"SQLiteCore",
			"Slate",
			"SlateCore",
			"BlueprintGraph",
			"KismetCompiler",
			"EditorSubsystem",
			"AnimationCore",
			"Niagara",
			"GameplayTags",
			"GameplayAbilities",
			"EnhancedInput",
			"Projects",
			"MetasoundEngine",
			"MetasoundFrontend"
		});
	}
}
