using UnrealBuildTool;

public class MonolithEditor : ModuleRules
{
	public MonolithEditor(ReadOnlyTargetRules Target) : base(Target)
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
			"MonolithIndex",
			"SQLiteCore",
			"MonolithSource",
			"UnrealEd",
			"EditorSubsystem",
			"PropertyEditor",
			"Slate",
			"SlateCore",
			"Json",
			"JsonUtilities",
			"MessageLog",
			// Capture actions
			"RenderCore",
			"RHI",
			"ImageWrapper",
			"Niagara",
			"AssetTools",
			"EditorScriptingUtilities",
			"AdvancedPreviewScene",
			"ImageCore",
			"Projects"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}
	}
}
