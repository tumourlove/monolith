using System.IO;
using UnrealBuildTool;

public class MonolithNiagara : ModuleRules
{
	public MonolithNiagara(ReadOnlyTargetRules Target) : base(Target)
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
			"Niagara",
			"NiagaraCore",
			"NiagaraEditor",
			"NiagaraShader",
			"Json",
			"JsonUtilities",
			"AssetTools",
			"Slate",
			"SlateCore"
		});

		PrivateIncludePaths.AddRange(new string[]
		{
			Path.Combine(EngineDirectory, "Plugins/FX/Niagara/Source/NiagaraEditor/Private")
		});
	}
}
