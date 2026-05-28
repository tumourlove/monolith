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
			"Projects",
			"ProceduralMeshComponent",
			// editor.run_python + editor.load_level (HOFF 7)
			"PythonScriptPlugin",
			"LevelEditor",
			// Preview & inspection surface expansion (2026-05-26 plan, Phase 1):
			// UMG = FWidgetRenderer + UWidget + UUserWidget for asset_type=widget.
			// UMGEditor = UWidgetBlueprint typed loading.
			// MaterialEditor = UMaterialEditingLibrary (Phase 2 inspect_material_pbr — declared up front so the dep delta lands in one cohesive Build.cs edit).
			"UMG",
			"UMGEditor",
			"MaterialEditor"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}
	}
}
