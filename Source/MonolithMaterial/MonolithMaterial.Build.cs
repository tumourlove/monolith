using UnrealBuildTool;

public class MonolithMaterial : ModuleRules
{
	public MonolithMaterial(ReadOnlyTargetRules Target) : base(Target)
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
			"MaterialEditor",
			"GraphEditor",
			"EditorScriptingUtilities",
			"AssetTools",
			"RenderCore",
			"RHI",
			"Slate",
			"SlateCore",
			"Json",
			"JsonUtilities"
		});
	}
}
