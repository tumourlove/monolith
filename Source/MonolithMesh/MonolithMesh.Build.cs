using UnrealBuildTool;
using System.IO;

public class MonolithMesh : ModuleRules
{
	public MonolithMesh(ReadOnlyTargetRules Target) : base(Target)
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
			"UnrealEd",
			"EditorSubsystem",
			"MeshDescription",
			"StaticMeshDescription",
			"MeshConversion",
			"PhysicsCore",
			"NavigationSystem",
			"RenderCore",
			"RHI",
			"EditorScriptingUtilities",
			"Json",
			"JsonUtilities",
			"Slate",
			"SlateCore",
			"AssetRegistry",
			"AssetTools",
			"MeshReductionInterface",
			"MeshMergeUtilities",
			"LevelInstanceEditor",
			"ImageCore"
		});

		// Optional: GeometryScripting (Tier 5 mesh operations only)
		//
		// Release builds: set MONOLITH_RELEASE_BUILD=1 to force this dep off so
		// the released DLL doesn't carry a hard import on UnrealEditor-GeometryScriptingCore.dll
		// (users who don't have the GeometryScripting plugin enabled in their .uproject
		// would otherwise hit GetLastError=126 at module load — see #26 / #30).
		// Source-tree users with GeometryScripting enabled still get full functionality.
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		bool bHasGeometryScripting = false;
		if (!bReleaseBuild)
		{
			string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
			string GeometryScriptingPath = Path.Combine(EngineDir,
				"Plugins", "Runtime", "GeometryScripting");
			bHasGeometryScripting = Directory.Exists(GeometryScriptingPath);
		}

		if (bHasGeometryScripting)
		{
			PrivateDependencyModuleNames.Add("GeometryScriptingCore");
			PrivateDependencyModuleNames.Add("GeometryFramework");
			PrivateDependencyModuleNames.Add("GeometryCore");
			PublicDefinitions.Add("WITH_GEOMETRYSCRIPT=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_GEOMETRYSCRIPT=0");
		}
	}
}
