using UnrealBuildTool;

public class MonolithGameplayTags : ModuleRules
{
	public MonolithGameplayTags(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// No optional-dependency probe here, unlike the MonolithFlow bridge: everything this
		// bridge uses ships with the engine, so it cannot compile out.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MonolithCore",
			"AssetRegistry",
			"GameplayTags",
			// Tag writes go through the same module the Project Settings tag editor uses.
			"GameplayTagsEditor",
			// update saves a tag list itself and checks the ini out first, as that module does.
			"SourceControl",
			"Json",
			// find_unused leaves Monolith and the plugins built on it out of the C++ check.
			"Projects"
		});
	}
}
