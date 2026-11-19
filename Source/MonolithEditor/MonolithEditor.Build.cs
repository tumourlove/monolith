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
			"InputCore",
			"ApplicationCore",
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
			// Phase 8 (OG-E2/E5): actor_setup move_to -> AAIController::MoveToLocation
			// (AIMODULE_API). Stock-engine module, always present + auto-enabled, so this
			// is release-build safe — no conditional gating / WITH_* probe required.
			"AIModule",
			// Gap 4: pie_inject_input_action -> UEnhancedInputLocalPlayerSubsystem::InjectInputForAction.
			// EnhancedInput is a stock, always-enabled engine plugin module (same rationale as
			// AIModule above), so this is release-build safe — no conditional gating / WITH_* probe.
			"EnhancedInput",
			// Preview & inspection surface expansion (2026-05-26 plan, Phase 1):
			// UMG = FWidgetRenderer + UWidget + UUserWidget for asset_type=widget.
			// UMGEditor = UWidgetBlueprint typed loading.
			// MaterialEditor = UMaterialEditingLibrary (Phase 2 inspect_material_pbr — declared up front so the dep delta lands in one cohesive Build.cs edit).
			"UMG",
			"UMGEditor",
			"MaterialEditor",
			// save_packages: check out read-only (e.g. Perforce) targets before saving (issue #147).
			"SourceControl",
			// PIE pre-flight test (MonolithPiePreflightTests.cpp) builds a transient Blueprint
			// graph from UK2Node_CustomEvent + UK2Node_CallFunction. Stock editor module.
			"BlueprintGraph"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}
	}
}
