using UnrealBuildTool;

public class MonolithBlueprint : ModuleRules
{
	public MonolithBlueprint(ReadOnlyTargetRules Target) : base(Target)
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
			"BlueprintGraph",
			"BlueprintEditorLibrary",
			"SubobjectDataInterface",
			"Kismet",
			"KismetCompiler",
			// Slate + GraphEditor: measure_graph_nodes / auto_layout(measured)
			// build node widgets to read real sizes and pin positions (#161).
			"Slate",
			"SlateCore",
			"GraphEditor",
			"EditorScriptingUtilities",
			"EnhancedInput",
			"Json",
			"JsonUtilities"
			// (Historical: StructUtils was added here by PR #40 but is deprecated
			// since UE 5.5 — FInstancedStruct relocated into CoreUObject and resolves
			// transparently via the existing CoreUObject Public dep above.)
		});
	}
}
