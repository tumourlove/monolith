#include "MonolithGASScaffoldActions.h"
#include "MonolithParamSchema.h"
#include "MonolithGASInternal.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/DataTable.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "AbilitySystemInterface.h"
#include "Abilities/GameplayAbility.h"
#include "AttributeSet.h"
#include "GameplayEffect.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsEditorModule.h"
#include "GameplayAbilitySpec.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "HAL/PlatformFileManager.h"
#include "UObject/SavePackage.h"
#include "Interfaces/IPluginManager.h"


// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

void FMonolithGASScaffoldActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("gas"), TEXT("bootstrap_gas_foundation"),
		TEXT("Day-one GAS setup: generates AbilitySystemGlobals subclass, configures DefaultGame.ini, creates folder structure, and ensures Build.cs has required modules"),
		FMonolithActionHandler::CreateStatic(&HandleBootstrapGASFoundation),
		FParamSchemaBuilder()
			.Required(TEXT("project_name"), TEXT("string"), TEXT("Project name (e.g. 'Leviathan') — used for class naming"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("validate_gas_setup"),
		TEXT("Full GAS audit — checks modules, globals config, ASCs, AttributeSets, abilities, effects, and interface implementation. Returns readiness score 0-10."),
		FMonolithActionHandler::CreateStatic(&HandleValidateGASSetup),
		FParamSchemaBuilder()
			.Build());

	// Phase 2: Advanced Scaffolding
	Registry.RegisterAction(TEXT("gas"), TEXT("scaffold_gas_project"),
		TEXT("Full project scaffolding from preset — creates tag hierarchy, folder structure, and orchestrates bootstrap. Ties together bootstrap_gas_foundation + scaffold_tag_hierarchy."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldGASProject),
		FParamSchemaBuilder()
			.Required(TEXT("preset"), TEXT("string"), TEXT("Preset name: 'survival_horror'"))
			.Optional(TEXT("actor_paths"), TEXT("array"), TEXT("Array of actor Blueprint paths to add ASCs to"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("scaffold_damage_pipeline"),
		TEXT("Generate ExecCalc + damage GEs + meta attribute flow for a complete damage pipeline. Creates GE Blueprints for each damage type with SetByCaller magnitudes."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldDamagePipeline),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Base path for generated assets (e.g. '/Game/GAS/Effects/Damage')"))
			.Required(TEXT("damage_types"), TEXT("array"), TEXT("Array of damage type names (e.g. ['Ballistic', 'Fire', 'Explosive'])"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("scaffold_status_effect"),
		TEXT("Generate a DOT/buff/debuff GameplayEffect with stacking, periodic damage, and optional cue tag"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldStatusEffect),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path for the new GE (e.g. '/Game/GAS/Effects/Status/GE_Status_Burning')"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Status effect name (e.g. 'Burning')"))
			.Required(TEXT("config"), TEXT("object"), TEXT("Configuration object: { duration?, period?, stacking_type?, stack_limit?, damage_per_tick?, attribute?, status_tag?, cue_tag?, removes_tags?[] }"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("scaffold_weapon_ability"),
		TEXT("Generate a weapon ability Blueprint with fire mode handling (single, burst, auto) and reload support"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldWeaponAbility),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path for the ability Blueprint"))
			.Required(TEXT("weapon_type"), TEXT("string"), TEXT("Weapon type: 'pistol', 'rifle', 'shotgun', 'smg', 'melee'"))
			.Optional(TEXT("fire_mode"), TEXT("string"), TEXT("Fire mode: 'single', 'burst', 'auto' (default: single)"), TEXT("single"))
			.Build());

	// Phase F8 (J-phase) — author-time ability grant for test scaffolds.
	// Locates the pawn BP's ASC SCS node, finds a TArray<TSubclassOf<UGameplayAbility>>
	// UPROPERTY on the ASC subclass (project-agnostic — any subclass that follows
	// the conventional StartupAbilities pattern works), appends the ability class,
	// marks the BP dirty + compiles. Stock UAbilitySystemComponent has NO such
	// property, so the action errors with a clear hint when no array is found.
	Registry.RegisterAction(TEXT("gas"), TEXT("grant_ability_to_pawn"),
		TEXT("Author-time: append a GameplayAbility class to a pawn BP's ASC startup-abilities array. Requires the ASC subclass to expose a TArray<TSubclassOf<UGameplayAbility>> UPROPERTY whose name contains 'Ability' (StartupAbilities, DefaultAbilities, etc.)."),
		FMonolithActionHandler::CreateStatic(&HandleGrantAbilityToPawn),
		FParamSchemaBuilder()
			.Required(TEXT("pawn_bp_path"), TEXT("string"), TEXT("Pawn Blueprint asset path (e.g. /Game/Tests/BP_TestPawn) — must contain a UAbilitySystemComponent"))
			.Required(TEXT("ability_class_path"), TEXT("string"), TEXT("GameplayAbility class path — Blueprint asset path or class name"))
			.Optional(TEXT("level"), TEXT("integer"), TEXT("Ability level (default 1, stored alongside class — only used if ASC array is FGameplayAbilitySpec-shaped, otherwise ignored)"), TEXT("1"))
			.Optional(TEXT("input_id"), TEXT("integer"), TEXT("Input ID (default -1, only used if ASC array is FGameplayAbilitySpec-shaped, otherwise ignored)"), TEXT("-1"))
			.Build());
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/** Check if a module name exists in the project Build.cs */
static bool ModuleExistsInBuildCS(const FString& BuildCSPath, const FString& ModuleName)
{
	FString Contents;
	if (!FFileHelper::LoadFileToString(Contents, *BuildCSPath))
	{
		return false;
	}
	// Check for the module in dependency arrays
	return Contents.Contains(FString::Printf(TEXT("\"%s\""), *ModuleName));
}

/** Add a module to the PublicDependencyModuleNames in Build.cs if not present */
static bool AddModuleToBuildCS(const FString& BuildCSPath, const FString& ModuleName, FString& OutError)
{
	FString Contents;
	if (!FFileHelper::LoadFileToString(Contents, *BuildCSPath))
	{
		OutError = FString::Printf(TEXT("Cannot read Build.cs at: %s"), *BuildCSPath);
		return false;
	}

	if (Contents.Contains(FString::Printf(TEXT("\"%s\""), *ModuleName)))
	{
		return true; // Already present
	}

	// Find the PublicDependencyModuleNames.AddRange line and append the module
	// Look for the closing of the first AddRange array
	FString SearchPattern = TEXT("PublicDependencyModuleNames.AddRange");
	int32 Idx = Contents.Find(SearchPattern);
	if (Idx == INDEX_NONE)
	{
		OutError = TEXT("Could not find PublicDependencyModuleNames.AddRange in Build.cs");
		return false;
	}

	// Find the closing }); of this statement
	int32 BraceStart = Contents.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Idx);
	if (BraceStart == INDEX_NONE)
	{
		OutError = TEXT("Malformed PublicDependencyModuleNames.AddRange in Build.cs");
		return false;
	}

	// Find the matching closing brace
	int32 BraceEnd = Contents.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, BraceStart + 1);
	if (BraceEnd == INDEX_NONE)
	{
		OutError = TEXT("Cannot find closing brace in PublicDependencyModuleNames");
		return false;
	}

	// Insert before the closing brace
	FString Insertion = FString::Printf(TEXT(", \"%s\""), *ModuleName);
	Contents.InsertAt(BraceEnd, Insertion);

	if (!FFileHelper::SaveStringToFile(Contents, *BuildCSPath))
	{
		OutError = FString::Printf(TEXT("Failed to write Build.cs: %s"), *BuildCSPath);
		return false;
	}

	return true;
}

/** Ensure a directory exists, create if not */
static bool EnsureDirectory(const FString& Path)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*Path))
	{
		return PlatformFile.CreateDirectoryTree(*Path);
	}
	return true;
}

/** Count Blueprint assets of a specific parent class */
static int32 CountBlueprintsOfClass(IAssetRegistry& AssetRegistry, UClass* ParentClass)
{
	TArray<FAssetData> Assets;
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	AssetRegistry.GetAssets(Filter, Assets);

	int32 Count = 0;
	for (const FAssetData& Asset : Assets)
	{
		// Check NativeParentClass or ParentClass tag
		FString ParentClassName;
		if (Asset.GetTagValue(FName("NativeParentClass"), ParentClassName) ||
			Asset.GetTagValue(FName("ParentClass"), ParentClassName))
		{
			if (ParentClassName.Contains(ParentClass->GetName()))
			{
				Count++;
			}
		}
	}
	return Count;
}

// ─────────────────────────────────────────────────────────────────────────────
// bootstrap_gas_foundation
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASScaffoldActions::HandleBootstrapGASFoundation(const TSharedPtr<FJsonObject>& Params)
{
	FString ProjectName;
	FMonolithActionResult ErrorResult;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("project_name"), ProjectName, ErrorResult))
	{
		return ErrorResult;
	}

	TArray<TSharedPtr<FJsonValue>> Checklist;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	bool bAllSucceeded = true;

	auto AddChecklist = [&](const FString& Item, bool bSuccess, const FString& Detail = TEXT(""))
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("item"), Item);
		Entry->SetBoolField(TEXT("success"), bSuccess);
		if (!Detail.IsEmpty())
		{
			Entry->SetStringField(TEXT("detail"), Detail);
		}
		Checklist.Add(MakeShared<FJsonValueObject>(Entry));
		if (!bSuccess) bAllSucceeded = false;
	};

	auto AddWarning = [&](const FString& Msg)
	{
		Warnings.Add(MakeShared<FJsonValueString>(Msg));
	};

	// ── 1. Generate AbilitySystemGlobals subclass ──
	FString ClassName = FString::Printf(TEXT("U%sAbilitySystemGlobals"), *ProjectName);
	FString HeaderFileName = FString::Printf(TEXT("%sAbilitySystemGlobals.h"), *ProjectName);
	FString SourceFileName = FString::Printf(TEXT("%sAbilitySystemGlobals.cpp"), *ProjectName);

	FString SourceDir = FPaths::ProjectDir() / TEXT("Source") / ProjectName;
	FString HeaderPath = SourceDir / HeaderFileName;
	FString SourcePath = SourceDir / SourceFileName;

	// Check if the file already exists
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	bool bHeaderExists = PlatformFile.FileExists(*HeaderPath);
	bool bSourceExists = PlatformFile.FileExists(*SourcePath);

	if (bHeaderExists && bSourceExists)
	{
		AddChecklist(TEXT("AbilitySystemGlobals subclass"),
			true, FString::Printf(TEXT("Already exists: %s"), *HeaderFileName));
	}
	else
	{
		// Generate header
		// ComboGraph is a dependency — inherit from UComboGraphAbilitySystemGlobals
		FString HeaderContent = FString::Printf(TEXT(
			"// Auto-generated by Monolith GAS Bootstrap\n"
			"#pragma once\n"
			"\n"
			"#include \"CoreMinimal.h\"\n"
			"#include \"Abilities/ComboGraphAbilitySystemGlobals.h\"\n"
			"#include \"%s.generated.h\"\n"
			"\n"
			"/**\n"
			" * Project-specific AbilitySystemGlobals.\n"
			" * Inherits from UComboGraphAbilitySystemGlobals to preserve ComboGraph functionality.\n"
			" * Add project-wide GAS configuration here.\n"
			" */\n"
			"UCLASS()\n"
			"class %s_API %s : public UComboGraphAbilitySystemGlobals\n"
			"{\n"
			"\tGENERATED_BODY()\n"
			"\n"
			"public:\n"
			"\t%s();\n"
			"};\n"),
			*FPaths::GetBaseFilename(HeaderPath),
			*ProjectName.ToUpper(),
			*ClassName,
			*ClassName);

		// Generate source
		FString SourceContent = FString::Printf(TEXT(
			"// Auto-generated by Monolith GAS Bootstrap\n"
			"#include \"%s\"\n"
			"\n"
			"%s::%s()\n"
			"{\n"
			"}\n"),
			*HeaderFileName,
			*ClassName, *ClassName);

		bool bHeaderOk = FFileHelper::SaveStringToFile(HeaderContent, *HeaderPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		bool bSourceOk = FFileHelper::SaveStringToFile(SourceContent, *SourcePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

		if (bHeaderOk && bSourceOk)
		{
			AddChecklist(TEXT("AbilitySystemGlobals subclass"),
				true, FString::Printf(TEXT("Created %s and %s"), *HeaderFileName, *SourceFileName));
		}
		else
		{
			AddChecklist(TEXT("AbilitySystemGlobals subclass"),
				false, TEXT("Failed to write C++ files"));
		}
	}

	// ── 2. Update DefaultGame.ini ──
	FString INIPath = FPaths::ProjectConfigDir() / TEXT("DefaultGame.ini");
	FString CurrentGlobalsClass;

	GConfig->GetString(
		TEXT("/Script/GameplayAbilities.AbilitySystemGlobals"),
		TEXT("AbilitySystemGlobalsClassName"),
		CurrentGlobalsClass,
		GGameIni);

	FString DesiredClassName = FString::Printf(TEXT("/Script/%s.%s"), *ProjectName, *ClassName);

	if (CurrentGlobalsClass == DesiredClassName)
	{
		AddChecklist(TEXT("DefaultGame.ini AbilitySystemGlobals"),
			true, TEXT("Already configured correctly"));
	}
	else
	{
		if (!CurrentGlobalsClass.IsEmpty() && CurrentGlobalsClass != DesiredClassName)
		{
			AddWarning(FString::Printf(
				TEXT("Overwriting existing AbilitySystemGlobalsClassName: '%s' -> '%s'"),
				*CurrentGlobalsClass, *DesiredClassName));
		}

		GConfig->SetString(
			TEXT("/Script/GameplayAbilities.AbilitySystemGlobals"),
			TEXT("AbilitySystemGlobalsClassName"),
			*DesiredClassName,
			GGameIni);
		GConfig->Flush(false, GGameIni);

		AddChecklist(TEXT("DefaultGame.ini AbilitySystemGlobals"),
			true, FString::Printf(TEXT("Set to %s"), *DesiredClassName));
	}

	// ── 3. Create folder structure ──
	FString ContentDir = FPaths::ProjectContentDir();
	TArray<FString> GASFolders = {
		TEXT("GAS/Abilities"),
		TEXT("GAS/Effects"),
		TEXT("GAS/AttributeSets"),
		TEXT("GAS/Cues"),
		TEXT("GAS/Tags")
	};

	int32 FoldersCreated = 0;
	int32 FoldersExisted = 0;
	for (const FString& Folder : GASFolders)
	{
		FString FullPath = ContentDir / Folder;
		if (PlatformFile.DirectoryExists(*FullPath))
		{
			FoldersExisted++;
		}
		else
		{
			if (EnsureDirectory(FullPath))
			{
				FoldersCreated++;
			}
			else
			{
				AddChecklist(FString::Printf(TEXT("Create Content/%s"), *Folder),
					false, TEXT("Failed to create directory"));
			}
		}
	}
	AddChecklist(TEXT("GAS content folder structure"),
		true, FString::Printf(TEXT("%d created, %d already existed"), FoldersCreated, FoldersExisted));

	// ── 4. Add modules to Build.cs ──
	FString BuildCSPath = FPaths::ProjectDir() / TEXT("Source") / ProjectName / (ProjectName + TEXT(".Build.cs"));
	if (!PlatformFile.FileExists(*BuildCSPath))
	{
		AddChecklist(TEXT("Build.cs module dependencies"),
			false, FString::Printf(TEXT("Build.cs not found at: %s"), *BuildCSPath));
	}
	else
	{
		TArray<FString> RequiredModules = {
			TEXT("GameplayAbilities"),
			TEXT("GameplayTags"),
			TEXT("GameplayTasks")
		};

		TArray<FString> AddedModules;
		TArray<FString> ExistingModules;
		FString ModuleError;

		for (const FString& Module : RequiredModules)
		{
			if (ModuleExistsInBuildCS(BuildCSPath, Module))
			{
				ExistingModules.Add(Module);
			}
			else
			{
				if (AddModuleToBuildCS(BuildCSPath, Module, ModuleError))
				{
					AddedModules.Add(Module);
				}
				else
				{
					AddChecklist(
						FString::Printf(TEXT("Add %s to Build.cs"), *Module),
						false, ModuleError);
				}
			}
		}

		FString Detail;
		if (AddedModules.Num() > 0)
		{
			Detail = FString::Printf(TEXT("Added: %s"), *FString::Join(AddedModules, TEXT(", ")));
		}
		if (ExistingModules.Num() > 0)
		{
			if (!Detail.IsEmpty()) Detail += TEXT(". ");
			Detail += FString::Printf(TEXT("Already present: %s"), *FString::Join(ExistingModules, TEXT(", ")));
		}
		AddChecklist(TEXT("Build.cs GAS module dependencies"), true, Detail);
	}

	// ── Build result ──
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("project_name"), ProjectName);
	Result->SetBoolField(TEXT("all_succeeded"), bAllSucceeded);
	Result->SetArrayField(TEXT("checklist"), Checklist);
	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}

	// Next steps guidance
	TArray<TSharedPtr<FJsonValue>> NextSteps;
	NextSteps.Add(MakeShared<FJsonValueString>(
		TEXT("Rebuild the project to compile the new AbilitySystemGlobals subclass")));
	NextSteps.Add(MakeShared<FJsonValueString>(
		TEXT("Run validate_gas_setup to verify the configuration")));
	NextSteps.Add(MakeShared<FJsonValueString>(
		TEXT("Create AttributeSets (Health, Combat, etc.) via create_attribute_set")));
	NextSteps.Add(MakeShared<FJsonValueString>(
		TEXT("Add AbilitySystemComponent to your player/AI actors via add_asc_to_actor")));
	NextSteps.Add(MakeShared<FJsonValueString>(
		TEXT("Implement IAbilitySystemInterface on your character class")));
	Result->SetArrayField(TEXT("next_steps"), NextSteps);

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// validate_gas_setup
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASScaffoldActions::HandleValidateGASSetup(const TSharedPtr<FJsonObject>& Params)
{
	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	TArray<TSharedPtr<FJsonValue>> Checks;
	int32 Score = 0;
	int32 MaxScore = 10;

	auto AddCheck = [&](const FString& Category, bool bPassed, const FString& Detail, int32 Points = 1)
	{
		TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
		Check->SetStringField(TEXT("check"), Category);
		Check->SetBoolField(TEXT("passed"), bPassed);
		Check->SetStringField(TEXT("detail"), Detail);
		Check->SetNumberField(TEXT("points"), bPassed ? Points : 0);
		Check->SetNumberField(TEXT("max_points"), Points);
		Checks.Add(MakeShared<FJsonValueObject>(Check));
		if (bPassed) Score += Points;
	};

	// ── 1. GameplayAbilities module in Build.cs (2 pts) ──
	{
		// Find the project Build.cs
		FString ProjectName = FApp::GetProjectName();
		FString BuildCSPath = FPaths::ProjectDir() / TEXT("Source") / ProjectName / (ProjectName + TEXT(".Build.cs"));

		if (PlatformFile.FileExists(*BuildCSPath))
		{
			bool bHasGA = ModuleExistsInBuildCS(BuildCSPath, TEXT("GameplayAbilities"));
			bool bHasGT = ModuleExistsInBuildCS(BuildCSPath, TEXT("GameplayTags"));
			bool bHasGTasks = ModuleExistsInBuildCS(BuildCSPath, TEXT("GameplayTasks"));

			if (bHasGA && bHasGT && bHasGTasks)
			{
				AddCheck(TEXT("Build.cs modules"), true,
					TEXT("GameplayAbilities, GameplayTags, and GameplayTasks all present"), 2);
			}
			else
			{
				TArray<FString> Missing;
				if (!bHasGA) Missing.Add(TEXT("GameplayAbilities"));
				if (!bHasGT) Missing.Add(TEXT("GameplayTags"));
				if (!bHasGTasks) Missing.Add(TEXT("GameplayTasks"));
				AddCheck(TEXT("Build.cs modules"), false,
					FString::Printf(TEXT("Missing: %s"), *FString::Join(Missing, TEXT(", "))), 2);
			}
		}
		else
		{
			AddCheck(TEXT("Build.cs modules"), false,
				FString::Printf(TEXT("Build.cs not found at %s"), *BuildCSPath), 2);
		}
	}

	// ── 2. AbilitySystemGlobals configured (1 pt) ──
	{
		FString GlobalsClass;
		GConfig->GetString(
			TEXT("/Script/GameplayAbilities.AbilitySystemGlobals"),
			TEXT("AbilitySystemGlobalsClassName"),
			GlobalsClass,
			GGameIni);

		if (!GlobalsClass.IsEmpty())
		{
			AddCheck(TEXT("AbilitySystemGlobals config"), true,
				FString::Printf(TEXT("Set to: %s"), *GlobalsClass), 1);
		}
		else
		{
			AddCheck(TEXT("AbilitySystemGlobals config"), false,
				TEXT("AbilitySystemGlobalsClassName not set in DefaultGame.ini"), 1);
		}
	}

	// ── 3. ASCs in project (2 pts) ──
	{
		// Search for Blueprints containing AbilitySystemComponent
		TArray<FAssetData> AllBPs;
		FARFilter Filter;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		AssetRegistry.GetAssets(Filter, AllBPs);

		int32 ASCCount = 0;
		TArray<FString> ActorsWithASC;
		for (const FAssetData& Asset : AllBPs)
		{
			// Check if any Blueprint has an ASC by loading and inspecting
			// For performance, check asset tags first
			FString NativeComponents;
			if (Asset.GetTagValue(FName("BlueprintDescription"), NativeComponents))
			{
				if (NativeComponents.Contains(TEXT("AbilitySystem")))
				{
					ASCCount++;
					ActorsWithASC.Add(Asset.AssetName.ToString());
				}
			}
		}

		// Also check C++ classes
		int32 NativeASCCount = 0;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (Class->IsChildOf(AActor::StaticClass()) && !Class->HasAnyClassFlags(CLASS_Abstract))
			{
				AActor* CDO = Cast<AActor>(Class->GetDefaultObject());
				if (CDO)
				{
					UAbilitySystemComponent* ASC = CDO->FindComponentByClass<UAbilitySystemComponent>();
					if (ASC)
					{
						NativeASCCount++;
						ActorsWithASC.Add(Class->GetName());
					}
				}
			}
		}

		int32 TotalASC = ASCCount + NativeASCCount;
		if (TotalASC > 0)
		{
			AddCheck(TEXT("AbilitySystemComponents"), true,
				FString::Printf(TEXT("Found %d actor(s) with ASC: %s"),
					TotalASC, *FString::Join(ActorsWithASC, TEXT(", "))), 2);
		}
		else
		{
			AddCheck(TEXT("AbilitySystemComponents"), false,
				TEXT("No actors found with AbilitySystemComponent"), 2);
		}
	}

	// ── 4. AttributeSets (1 pt) ──
	{
		int32 Count = 0;
		TArray<FString> FoundSets;

		// Check native C++ AttributeSets
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (Class->IsChildOf(UAttributeSet::StaticClass()) &&
				Class != UAttributeSet::StaticClass() &&
				!Class->GetName().StartsWith(TEXT("SKEL_")) &&
				!Class->HasAnyClassFlags(CLASS_Abstract))
			{
				// Filter to project classes (not engine/plugin)
				FString ModuleName = Class->GetOuterUPackage()->GetName();
				if (!ModuleName.StartsWith(TEXT("/Script/GameplayAbilities")) &&
					!ModuleName.StartsWith(TEXT("/Script/Engine")))
				{
					Count++;
					FoundSets.Add(Class->GetName());
				}
			}
		}

		// Check Blueprint AttributeSets
		int32 BPCount = CountBlueprintsOfClass(AssetRegistry, UAttributeSet::StaticClass());
		Count += BPCount;

		if (Count > 0)
		{
			AddCheck(TEXT("AttributeSets"), true,
				FString::Printf(TEXT("Found %d AttributeSet(s): %s"),
					Count, FoundSets.Num() > 0 ? *FString::Join(FoundSets, TEXT(", ")) : TEXT("(Blueprint)")), 1);
		}
		else
		{
			AddCheck(TEXT("AttributeSets"), false,
				TEXT("No AttributeSets found in project"), 1);
		}
	}

	// ── 5. GameplayEffects (1 pt) ──
	{
		TArray<FAssetData> Effects;
		FARFilter Filter;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		AssetRegistry.GetAssets(Filter, Effects);

		int32 GECount = 0;
		for (const FAssetData& Asset : Effects)
		{
			FString ParentClass;
			if (Asset.GetTagValue(FName("NativeParentClass"), ParentClass) ||
				Asset.GetTagValue(FName("ParentClass"), ParentClass))
			{
				if (ParentClass.Contains(TEXT("GameplayEffect")))
				{
					GECount++;
				}
			}
		}

		if (GECount > 0)
		{
			AddCheck(TEXT("GameplayEffects"), true,
				FString::Printf(TEXT("Found %d GameplayEffect Blueprint(s)"), GECount), 1);
		}
		else
		{
			AddCheck(TEXT("GameplayEffects"), false,
				TEXT("No GameplayEffect Blueprints found"), 1);
		}
	}

	// ── 6. GameplayAbilities (1 pt) ──
	{
		TArray<FAssetData> Abilities;
		FARFilter Filter;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		AssetRegistry.GetAssets(Filter, Abilities);

		int32 GACount = 0;
		for (const FAssetData& Asset : Abilities)
		{
			FString ParentClass;
			if (Asset.GetTagValue(FName("NativeParentClass"), ParentClass) ||
				Asset.GetTagValue(FName("ParentClass"), ParentClass))
			{
				if (ParentClass.Contains(TEXT("GameplayAbility")))
				{
					GACount++;
				}
			}
		}

		if (GACount > 0)
		{
			AddCheck(TEXT("GameplayAbilities"), true,
				FString::Printf(TEXT("Found %d GameplayAbility Blueprint(s)"), GACount), 1);
		}
		else
		{
			AddCheck(TEXT("GameplayAbilities"), false,
				TEXT("No GameplayAbility Blueprints found"), 1);
		}
	}

	// ── 7. IAbilitySystemInterface implementation (2 pts) ──
	{
		int32 InterfaceCount = 0;
		TArray<FString> Implementors;

		// Check native classes for IAbilitySystemInterface
		UClass* InterfaceClass = UAbilitySystemComponent::StaticClass();
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (Class->IsChildOf(AActor::StaticClass()) &&
				!Class->HasAnyClassFlags(CLASS_Abstract) &&
				Class->ImplementsInterface(UAbilitySystemInterface::StaticClass()))
			{
				// Filter to project classes
				FString ModuleName = Class->GetOuterUPackage()->GetName();
				if (!ModuleName.StartsWith(TEXT("/Script/GameplayAbilities")) &&
					!ModuleName.StartsWith(TEXT("/Script/Engine")) &&
					!ModuleName.StartsWith(TEXT("/Script/ComboGraph")))
				{
					InterfaceCount++;
					Implementors.Add(Class->GetName());
				}
			}
		}

		if (InterfaceCount > 0)
		{
			AddCheck(TEXT("IAbilitySystemInterface"), true,
				FString::Printf(TEXT("Found %d class(es) implementing interface: %s"),
					InterfaceCount, *FString::Join(Implementors, TEXT(", "))), 2);
		}
		else
		{
			AddCheck(TEXT("IAbilitySystemInterface"), false,
				TEXT("No project classes implement IAbilitySystemInterface. "
					 "Your Character/PlayerState needs this for GAS to work."), 2);
		}
	}

	// ── Build final result ──
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("readiness_score"), Score);
	Result->SetNumberField(TEXT("max_score"), MaxScore);
	Result->SetStringField(TEXT("readiness_grade"),
		Score >= 9 ? TEXT("Production Ready") :
		Score >= 7 ? TEXT("Functional") :
		Score >= 4 ? TEXT("Partial Setup") :
		Score >= 1 ? TEXT("Minimal") :
		TEXT("Not Started"));
	Result->SetArrayField(TEXT("checks"), Checks);

	// Recommendations based on what's missing
	TArray<TSharedPtr<FJsonValue>> Recommendations;
	for (const auto& CheckVal : Checks)
	{
		const TSharedPtr<FJsonObject>& Check = CheckVal->AsObject();
		if (!Check->GetBoolField(TEXT("passed")))
		{
			FString CheckName = Check->GetStringField(TEXT("check"));
			FString Rec;
			if (CheckName == TEXT("Build.cs modules"))
				Rec = TEXT("Run bootstrap_gas_foundation to add GameplayAbilities/Tags/Tasks modules");
			else if (CheckName == TEXT("AbilitySystemGlobals config"))
				Rec = TEXT("Run bootstrap_gas_foundation to configure AbilitySystemGlobals");
			else if (CheckName == TEXT("AbilitySystemComponents"))
				Rec = TEXT("Use add_asc_to_actor to add AbilitySystemComponent to your character");
			else if (CheckName == TEXT("AttributeSets"))
				Rec = TEXT("Create AttributeSets (e.g. Health, Combat) via create_attribute_set");
			else if (CheckName == TEXT("GameplayEffects"))
				Rec = TEXT("Create GameplayEffect Blueprints for damage, healing, buffs in Content/GAS/Effects");
			else if (CheckName == TEXT("GameplayAbilities"))
				Rec = TEXT("Create GameplayAbility Blueprints in Content/GAS/Abilities");
			else if (CheckName == TEXT("IAbilitySystemInterface"))
				Rec = TEXT("Implement IAbilitySystemInterface on your Character or PlayerState C++ class");

			if (!Rec.IsEmpty())
			{
				Recommendations.Add(MakeShared<FJsonValueString>(Rec));
			}
		}
	}
	if (Recommendations.Num() > 0)
	{
		Result->SetArrayField(TEXT("recommendations"), Recommendations);
	}

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// scaffold_gas_project
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASScaffoldActions::HandleScaffoldGASProject(const TSharedPtr<FJsonObject>& Params)
{
	FString Preset;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("preset"), Preset, Err)) return Err;

	if (!Preset.Equals(TEXT("survival_horror"), ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown preset: '%s'. Available: survival_horror"), *Preset));
	}

	TArray<TSharedPtr<FJsonValue>> Steps;
	bool bAllSucceeded = true;

	auto AddStep = [&](const FString& StepName, bool bSuccess, const FString& Detail)
	{
		TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetStringField(TEXT("step"), StepName);
		Step->SetBoolField(TEXT("success"), bSuccess);
		Step->SetStringField(TEXT("detail"), Detail);
		Steps.Add(MakeShared<FJsonValueObject>(Step));
		if (!bSuccess) bAllSucceeded = false;
	};

	// Step 1: Bootstrap foundation
	FString ProjectName = FApp::GetProjectName();
	{
		TSharedPtr<FJsonObject> BootstrapParams = MakeShared<FJsonObject>();
		BootstrapParams->SetStringField(TEXT("project_name"), ProjectName);
		FMonolithActionResult BootstrapResult = HandleBootstrapGASFoundation(BootstrapParams);
		AddStep(TEXT("bootstrap_gas_foundation"),
			BootstrapResult.bSuccess,
			BootstrapResult.bSuccess ? TEXT("Foundation configured") : TEXT("Bootstrap failed — check logs"));
	}

	// Step 2: Scaffold core tag hierarchy (root categories only — use scaffold_tag_hierarchy for the full set)
	{
		TArray<FString> CoreTags = {
			TEXT("Ability.Combat"), TEXT("Ability.Movement"), TEXT("Ability.Interaction"),
			TEXT("Ability.Horror"), TEXT("Ability.Survival"),
			TEXT("State.Movement"), TEXT("State.Combat"), TEXT("State.Status"),
			TEXT("State.Horror"), TEXT("State.Condition"), TEXT("State.Accessibility"),
			TEXT("Damage.Type"), TEXT("Damage.Zone"),
			TEXT("Status"), TEXT("Cooldown.Ability"),
			TEXT("GameplayCue.Combat"), TEXT("GameplayCue.Status"), TEXT("GameplayCue.Horror"), TEXT("GameplayCue.Player"),
			TEXT("SetByCaller.Damage"), TEXT("SetByCaller.Duration"), TEXT("SetByCaller.Horror"),
			TEXT("Event.Combat"), TEXT("Event.Interaction"), TEXT("Event.Horror"),
			TEXT("Director.Intensity"), TEXT("Director.Phase"), TEXT("Director.Spawn"), TEXT("Director.Music")
		};

		TSet<FString> AllTagSet;
		for (const FString& Tag : CoreTags)
		{
			TArray<FString> Parts;
			Tag.ParseIntoArray(Parts, TEXT("."));
			FString Accumulator;
			for (int32 i = 0; i < Parts.Num(); ++i)
			{
				if (i > 0) Accumulator += TEXT(".");
				Accumulator += Parts[i];
				AllTagSet.Add(Accumulator);
			}
		}
		TArray<FString> AllTagsToAdd = AllTagSet.Array();
		AllTagsToAdd.Sort();

		int32 AddedCount = 0;
		IGameplayTagsEditorModule& TagsEditor = IGameplayTagsEditorModule::Get();
		UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
		FGuid SuspendToken = FGuid::NewGuid();
		TagManager.SuspendEditorRefreshGameplayTagTree(SuspendToken);

		for (const FString& TagStr : AllTagsToAdd)
		{
			if (TagManager.IsDictionaryTag(FName(*TagStr)))
			{
				continue;
			}
			bool bOk = TagsEditor.AddNewGameplayTagToINI(TagStr, TEXT(""), NAME_None, false, true);
			if (bOk)
			{
				AddedCount++;
			}
		}

		TagManager.ResumeEditorRefreshGameplayTagTree(SuspendToken);

		AddStep(TEXT("scaffold_tag_hierarchy"),
			true,
			FString::Printf(TEXT("Added %d root-level tag categories (%d total with hierarchy). Run scaffold_tag_hierarchy for the full ~250 tag set."),
				CoreTags.Num(), AllTagsToAdd.Num()));
	}

	// Step 3: Create extended content folders
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		FString ContentDir = FPaths::ProjectContentDir();
		TArray<FString> ExtraFolders = {
			TEXT("GAS/Abilities/Combat"), TEXT("GAS/Abilities/Movement"),
			TEXT("GAS/Abilities/Interaction"), TEXT("GAS/Abilities/Horror"),
			TEXT("GAS/Abilities/Survival"),
			TEXT("GAS/Effects/Damage"), TEXT("GAS/Effects/Status"),
			TEXT("GAS/Effects/Buff"), TEXT("GAS/Effects/Cooldown"),
			TEXT("GAS/Effects/Init"),
			TEXT("GAS/Cues/Combat"), TEXT("GAS/Cues/Status"), TEXT("GAS/Cues/Horror"),
			TEXT("GAS/DataTables")
		};
		int32 Created = 0;
		for (const FString& Folder : ExtraFolders)
		{
			FString FullPath = ContentDir / Folder;
			if (!PlatformFile.DirectoryExists(*FullPath))
			{
				EnsureDirectory(FullPath);
				Created++;
			}
		}
		AddStep(TEXT("create_content_folders"),
			true,
			FString::Printf(TEXT("Ensured %d content folders exist (%d newly created)"),
				ExtraFolders.Num(), Created));
	}

	// Step 4: Report actor ASC status
	TArray<FString> ActorPaths = MonolithGAS::ParseStringArray(Params, TEXT("actor_paths"));
	TArray<TSharedPtr<FJsonValue>> ActorStatus;
	if (ActorPaths.Num() > 0)
	{
		for (const FString& ActorPath : ActorPaths)
		{
			TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
			Status->SetStringField(TEXT("actor_path"), ActorPath);

			FString LoadError;
			UObject* Obj = MonolithGAS::LoadAssetFromPath(ActorPath, LoadError);
			UBlueprint* BP = Cast<UBlueprint>(Obj);
			if (BP)
			{
				bool bHasASC = false;
				if (BP->SimpleConstructionScript)
				{
					for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
					{
						if (Node && Node->ComponentClass &&
							Node->ComponentClass->IsChildOf(UAbilitySystemComponent::StaticClass()))
						{
							bHasASC = true;
							break;
						}
					}
				}
				Status->SetBoolField(TEXT("has_asc"), bHasASC);
				Status->SetStringField(TEXT("action_needed"),
					bHasASC ? TEXT("none") : TEXT("add_asc_to_actor"));
			}
			else
			{
				Status->SetStringField(TEXT("error"),
					FString::Printf(TEXT("Failed to load: %s"), *LoadError));
			}
			ActorStatus.Add(MakeShared<FJsonValueObject>(Status));
		}
		AddStep(TEXT("check_actor_ascs"),
			true,
			FString::Printf(TEXT("Checked %d actor(s) for ASC presence"), ActorPaths.Num()));
	}

	// Build result
	TSharedPtr<FJsonObject> ProjectResult = MakeShared<FJsonObject>();
	ProjectResult->SetStringField(TEXT("preset"), Preset);
	ProjectResult->SetStringField(TEXT("project_name"), ProjectName);
	ProjectResult->SetBoolField(TEXT("all_succeeded"), bAllSucceeded);
	ProjectResult->SetArrayField(TEXT("steps"), Steps);

	if (ActorStatus.Num() > 0)
	{
		ProjectResult->SetArrayField(TEXT("actor_status"), ActorStatus);
	}

	TArray<TSharedPtr<FJsonValue>> NextSteps;
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Run scaffold_tag_hierarchy preset:'survival_horror' for the full ~250 tag hierarchy")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Create AttributeSets via create_attribute_set (VitalSet, StaminaSet, HorrorSet)")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Add ASC to actors via add_asc_to_actor")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Run scaffold_damage_pipeline for the damage system")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Implement IAbilitySystemInterface in C++ on Character/PlayerState")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Build the project to compile generated C++ files")));
	ProjectResult->SetArrayField(TEXT("next_steps"), NextSteps);

	return FMonolithActionResult::Success(ProjectResult);
}

// ─────────────────────────────────────────────────────────────────────────────
// scaffold_damage_pipeline
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASScaffoldActions::HandleScaffoldDamagePipeline(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("save_path"), SavePath, Err)) return Err;

	TArray<FString> DamageTypes = MonolithGAS::ParseStringArray(Params, TEXT("damage_types"));
	if (DamageTypes.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: damage_types"));
	}

	TArray<TSharedPtr<FJsonValue>> CreatedAssets;
	TArray<TSharedPtr<FJsonValue>> AssetErrors;
	int32 SuccessCount = 0;

	// Check for the SetByCaller.Damage.Base tag
	FString SBCTagStr = TEXT("SetByCaller.Damage.Base");
	FGameplayTag SBCTag = FGameplayTag::RequestGameplayTag(FName(*SBCTagStr), false);

	for (const FString& DmgType : DamageTypes)
	{
		FString GEName = FString::Printf(TEXT("GE_Damage_%s"), *DmgType);
		FString GEPath = SavePath / GEName;

		// Check for existing asset (AssetRegistry + in-memory multi-tier check)
		FString ExistError;
		if (!MonolithGAS::EnsureAssetPathFree(GEPath, GEName, ExistError))
		{
			TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
			Info->SetStringField(TEXT("asset_path"), GEPath);
			Info->SetStringField(TEXT("status"), TEXT("already_exists"));
			CreatedAssets.Add(MakeShared<FJsonValueObject>(Info));
			continue;
		}

		UPackage* Package = CreatePackage(*GEPath);
		if (!Package)
		{
			AssetErrors.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Failed to create package for %s"), *GEPath)));
			continue;
		}
		Package->FullyLoad();

		UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
			UGameplayEffect::StaticClass(),
			Package,
			FName(*GEName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());

		if (!NewBP)
		{
			AssetErrors.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Failed to create Blueprint for %s"), *GEPath)));
			continue;
		}

		FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::SkipGarbageCollection);

		UGameplayEffect* GE_CDO = MonolithGAS::GetBlueprintCDO<UGameplayEffect>(NewBP);
		if (GE_CDO)
		{
			// Instant damage GE
			GE_CDO->DurationPolicy = EGameplayEffectDurationType::Instant;

			// Modifier with SetByCaller or fallback scalable float
			FGameplayModifierInfo DamageMod;
			DamageMod.ModifierOp = EGameplayModOp::Additive;

			if (SBCTag.IsValid())
			{
				FSetByCallerFloat SBC;
				SBC.DataTag = SBCTag;
				DamageMod.ModifierMagnitude = FGameplayEffectModifierMagnitude(SBC);
			}
			else
			{
				DamageMod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(-1.0f));
			}
			// Attribute intentionally not set — user must point at their meta attribute
			GE_CDO->Modifiers.Add(DamageMod);

			// Tag the GE with its damage type
			FString DamageTagStr = FString::Printf(TEXT("Damage.Type.%s"), *DmgType);
			FGameplayTag DamageTypeTag = FGameplayTag::RequestGameplayTag(FName(*DamageTagStr), false);
			if (DamageTypeTag.IsValid())
			{
				FProperty* AssetTagProp = UGameplayEffect::StaticClass()->FindPropertyByName(TEXT("InheritableGameplayEffectTags"));
				if (AssetTagProp)
				{
					FInheritedTagContainer* AssetTags = AssetTagProp->ContainerPtrToValuePtr<FInheritedTagContainer>(GE_CDO);
					if (AssetTags)
					{
						AssetTags->Added.AddTag(DamageTypeTag);
					}
				}
			}
		}

		FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::SkipGarbageCollection);
		Package->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(NewBP);

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		FString PackageFilename = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		bool bSaved = UPackage::SavePackage(Package, NewBP, *PackageFilename, SaveArgs);

		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		Info->SetStringField(TEXT("asset_path"), GEPath);
		Info->SetStringField(TEXT("damage_type"), DmgType);
		Info->SetStringField(TEXT("status"), bSaved ? TEXT("created") : TEXT("created_unsaved"));
		Info->SetStringField(TEXT("duration_policy"), TEXT("instant"));
		Info->SetStringField(TEXT("magnitude_type"), SBCTag.IsValid() ? TEXT("set_by_caller") : TEXT("scalable_float"));
		CreatedAssets.Add(MakeShared<FJsonValueObject>(Info));
		SuccessCount++;
	}

	TSharedPtr<FJsonObject> PipelineResult = MakeShared<FJsonObject>();
	PipelineResult->SetStringField(TEXT("save_path"), SavePath);
	PipelineResult->SetNumberField(TEXT("created_count"), SuccessCount);
	PipelineResult->SetNumberField(TEXT("damage_type_count"), DamageTypes.Num());
	PipelineResult->SetArrayField(TEXT("assets"), CreatedAssets);
	if (AssetErrors.Num() > 0)
	{
		PipelineResult->SetArrayField(TEXT("errors"), AssetErrors);
	}

	TArray<TSharedPtr<FJsonValue>> NextSteps;
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Create a VitalSet AttributeSet with IncomingDamage meta attribute")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Create UExecCalc_Damage C++ class to read IncomingDamage + apply resistances")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Use add_execution to link the ExecCalc to these GEs (or a parent GE)")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Use set_modifier to point modifiers at your IncomingDamage attribute")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Add GameplayCue tags via link_cue_to_effect for hit feedback")));
	PipelineResult->SetArrayField(TEXT("next_steps"), NextSteps);

	TSharedPtr<FJsonObject> Pipeline = MakeShared<FJsonObject>();
	Pipeline->SetStringField(TEXT("flow"),
		TEXT("Weapon fires -> Apply GE_Damage_[Type] with SetByCaller(Base) -> "
			 "ExecCalc reads IncomingDamage meta attr + target resistances -> "
			 "Outputs to Health/Shield -> PostGEExecute clamps -> GameplayCue fires"));
	Pipeline->SetStringField(TEXT("meta_attribute"), TEXT("IncomingDamage (on VitalSet, NOT replicated)"));
	Pipeline->SetStringField(TEXT("exec_calc"), TEXT("UExecCalc_Damage (needs C++ implementation)"));
	PipelineResult->SetObjectField(TEXT("pipeline_architecture"), Pipeline);

	return FMonolithActionResult::Success(PipelineResult);
}

// ─────────────────────────────────────────────────────────────────────────────
// scaffold_status_effect
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASScaffoldActions::HandleScaffoldStatusEffect(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("save_path"), SavePath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("name"), Name, Err)) return Err;

	const TSharedPtr<FJsonObject>* ConfigPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("config"), ConfigPtr) || !ConfigPtr || !(*ConfigPtr).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: config (object)"));
	}
	const TSharedPtr<FJsonObject>& Config = *ConfigPtr;

	// Parse config with sane defaults for DOT effects
	float Duration = Config->HasField(TEXT("duration")) ? Config->GetNumberField(TEXT("duration")) : 5.0f;
	float Period = Config->HasField(TEXT("period")) ? Config->GetNumberField(TEXT("period")) : 1.0f;
	FString StackingType = Config->HasField(TEXT("stacking_type")) ? Config->GetStringField(TEXT("stacking_type")) : TEXT("aggregate_by_target");
	int32 StackLimit = Config->HasField(TEXT("stack_limit")) ? static_cast<int32>(Config->GetNumberField(TEXT("stack_limit"))) : 5;
	float DamagePerTick = Config->HasField(TEXT("damage_per_tick")) ? Config->GetNumberField(TEXT("damage_per_tick")) : 0.0f;
	FString Attribute = Config->GetStringField(TEXT("attribute"));
	FString StatusTag = Config->GetStringField(TEXT("status_tag"));
	FString CueTag = Config->GetStringField(TEXT("cue_tag"));
	TArray<FString> RemovesTags = MonolithGAS::ParseStringArray(Config, TEXT("removes_tags"));

	// Extract asset name
	int32 LastSlash;
	if (!SavePath.FindLastChar(TEXT('/'), LastSlash))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid save_path: %s"), *SavePath));
	}
	FString AssetName = SavePath.Mid(LastSlash + 1);

	// Check for existing asset (AssetRegistry + in-memory multi-tier check)
	FString ExistError;
	if (!MonolithGAS::EnsureAssetPathFree(SavePath, AssetName, ExistError))
	{
		return FMonolithActionResult::Error(ExistError);
	}

	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *SavePath));
	}
	Package->FullyLoad();

	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		UGameplayEffect::StaticClass(),
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());

	if (!NewBP)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to create GE Blueprint at: %s"), *SavePath));
	}

	FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::SkipGarbageCollection);

	UGameplayEffect* GE_CDO = MonolithGAS::GetBlueprintCDO<UGameplayEffect>(NewBP);
	if (!GE_CDO)
	{
		return FMonolithActionResult::Error(TEXT("Failed to get CDO after compile"));
	}

	// Duration policy: HasDuration for status effects
	GE_CDO->DurationPolicy = EGameplayEffectDurationType::HasDuration;
	GE_CDO->DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Duration));
	GE_CDO->Period = FScalableFloat(Period);

	// Stacking configuration
	{
		EGameplayEffectStackingType StackType = EGameplayEffectStackingType::AggregateByTarget;
		if (StackingType.Equals(TEXT("aggregate_by_source"), ESearchCase::IgnoreCase))
		{
			StackType = EGameplayEffectStackingType::AggregateBySource;
		}
		else if (StackingType.Equals(TEXT("none"), ESearchCase::IgnoreCase))
		{
			StackType = EGameplayEffectStackingType::None;
		}

		FProperty* StackProp = UGameplayEffect::StaticClass()->FindPropertyByName(TEXT("StackingType"));
		if (StackProp)
		{
			StackProp->CopyCompleteValue(StackProp->ContainerPtrToValuePtr<void>(GE_CDO), &StackType);
		}
		GE_CDO->StackLimitCount = StackLimit;
		GE_CDO->StackDurationRefreshPolicy = EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication;
		GE_CDO->StackPeriodResetPolicy = EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication;
		GE_CDO->StackExpirationPolicy = EGameplayEffectStackingExpirationPolicy::RemoveSingleStackAndRefreshDuration;
	}

	// Add damage modifier if configured
	if (DamagePerTick != 0.0f)
	{
		FGameplayModifierInfo DamageMod;
		DamageMod.ModifierOp = EGameplayModOp::Additive;
		DamageMod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(-FMath::Abs(DamagePerTick)));

		if (!Attribute.IsEmpty())
		{
			FString ClassName, PropName;
			if (Attribute.Split(TEXT("."), &ClassName, &PropName))
			{
				UClass* AttrClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
				if (!AttrClass)
				{
					AttrClass = FindFirstObject<UClass>(*(TEXT("U") + ClassName), EFindFirstObjectOptions::NativeFirst);
				}
				if (AttrClass)
				{
					FProperty* Prop = FindFProperty<FProperty>(AttrClass, FName(*PropName));
					if (Prop)
					{
						DamageMod.Attribute = FGameplayAttribute(Prop);
					}
				}
			}
		}
		GE_CDO->Modifiers.Add(DamageMod);
	}

	// Add status identity tag to GE asset tags
	if (!StatusTag.IsEmpty())
	{
		FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*StatusTag), false);
		if (Tag.IsValid())
		{
			FProperty* AssetTagProp = UGameplayEffect::StaticClass()->FindPropertyByName(TEXT("InheritableGameplayEffectTags"));
			if (AssetTagProp)
			{
				FInheritedTagContainer* AssetTags = AssetTagProp->ContainerPtrToValuePtr<FInheritedTagContainer>(GE_CDO);
				if (AssetTags)
				{
					AssetTags->Added.AddTag(Tag);
				}
			}
		}
	}

	// Grant state tag to target while active
	if (!StatusTag.IsEmpty())
	{
		FString StateTag = StatusTag;
		StateTag.ReplaceInline(TEXT("Status."), TEXT("State.Status."));
		FGameplayTag TargetTag = FGameplayTag::RequestGameplayTag(FName(*StateTag), false);
		if (TargetTag.IsValid())
		{
			FProperty* GrantedTagProp = UGameplayEffect::StaticClass()->FindPropertyByName(TEXT("InheritableOwnedTagsContainer"));
			if (GrantedTagProp)
			{
				FInheritedTagContainer* GrantedTags = GrantedTagProp->ContainerPtrToValuePtr<FInheritedTagContainer>(GE_CDO);
				if (GrantedTags)
				{
					GrantedTags->Added.AddTag(TargetTag);
				}
			}
		}
	}

	// Set GameplayCue tag for visual feedback
	if (!CueTag.IsEmpty())
	{
		FGameplayTag CueGPTag = FGameplayTag::RequestGameplayTag(FName(*CueTag), false);
		if (CueGPTag.IsValid())
		{
			FProperty* CueProp = UGameplayEffect::StaticClass()->FindPropertyByName(TEXT("GameplayCues"));
			if (CueProp)
			{
				FInheritedTagContainer* CueTags = CueProp->ContainerPtrToValuePtr<FInheritedTagContainer>(GE_CDO);
				if (CueTags)
				{
					CueTags->Added.AddTag(CueGPTag);
				}
			}
		}
	}

	// Compile, save
	FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::SkipGarbageCollection);
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewBP);

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	bool bSaved = UPackage::SavePackage(Package, NewBP, *PackageFilename, SaveArgs);

	TSharedPtr<FJsonObject> StatusResult = MakeShared<FJsonObject>();
	StatusResult->SetStringField(TEXT("asset_path"), SavePath);
	StatusResult->SetStringField(TEXT("name"), Name);
	StatusResult->SetBoolField(TEXT("saved"), bSaved);

	TSharedPtr<FJsonObject> EffectConfig = MakeShared<FJsonObject>();
	EffectConfig->SetStringField(TEXT("duration_policy"), TEXT("has_duration"));
	EffectConfig->SetNumberField(TEXT("duration"), Duration);
	EffectConfig->SetNumberField(TEXT("period"), Period);
	EffectConfig->SetStringField(TEXT("stacking_type"), StackingType);
	EffectConfig->SetNumberField(TEXT("stack_limit"), StackLimit);
	if (DamagePerTick != 0.0f) EffectConfig->SetNumberField(TEXT("damage_per_tick"), DamagePerTick);
	if (!StatusTag.IsEmpty()) EffectConfig->SetStringField(TEXT("status_tag"), StatusTag);
	if (!CueTag.IsEmpty()) EffectConfig->SetStringField(TEXT("cue_tag"), CueTag);
	if (!Attribute.IsEmpty()) EffectConfig->SetStringField(TEXT("attribute"), Attribute);
	StatusResult->SetObjectField(TEXT("config"), EffectConfig);

	StatusResult->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Created status effect '%s' at '%s' — %gs duration, %gs period, x%d stacks"),
			*Name, *SavePath, Duration, Period, StackLimit));

	return FMonolithActionResult::Success(StatusResult);
}

// ─────────────────────────────────────────────────────────────────────────────
// scaffold_weapon_ability
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASScaffoldActions::HandleScaffoldWeaponAbility(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, WeaponType;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("save_path"), SavePath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("weapon_type"), WeaponType, Err)) return Err;

	FString FireMode = Params->GetStringField(TEXT("fire_mode"));
	if (FireMode.IsEmpty()) FireMode = TEXT("single");

	// Validate inputs
	TArray<FString> ValidWeaponTypes = { TEXT("pistol"), TEXT("rifle"), TEXT("shotgun"), TEXT("smg"), TEXT("melee") };
	FString WeaponLower = WeaponType.ToLower();
	if (!ValidWeaponTypes.Contains(WeaponLower))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid weapon_type: '%s'. Valid: pistol, rifle, shotgun, smg, melee"), *WeaponType));
	}

	TArray<FString> ValidFireModes = { TEXT("single"), TEXT("burst"), TEXT("auto") };
	FString FireModeLower = FireMode.ToLower();
	if (!ValidFireModes.Contains(FireModeLower))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid fire_mode: '%s'. Valid: single, burst, auto"), *FireMode));
	}

	int32 LastSlash;
	if (!SavePath.FindLastChar(TEXT('/'), LastSlash))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid save_path: %s"), *SavePath));
	}
	FString AssetName = SavePath.Mid(LastSlash + 1);

	// Check for existing asset (AssetRegistry + in-memory multi-tier check)
	FString ExistError;
	if (!MonolithGAS::EnsureAssetPathFree(SavePath, AssetName, ExistError))
	{
		return FMonolithActionResult::Error(ExistError);
	}

	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *SavePath));
	}
	Package->FullyLoad();

	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		UGameplayAbility::StaticClass(),
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());

	if (!NewBP)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to create ability Blueprint at: %s"), *SavePath));
	}

	FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::SkipGarbageCollection);

	UGameplayAbility* AbilityCDO = MonolithGAS::GetBlueprintCDO<UGameplayAbility>(NewBP);
	if (!AbilityCDO)
	{
		return FMonolithActionResult::Error(TEXT("Failed to get Ability CDO after compile"));
	}

	bool bIsMelee = WeaponLower == TEXT("melee");

	// Configure AbilityTags
	{
		FString AbilityTagStr = bIsMelee ?
			TEXT("Ability.Combat.Melee") : TEXT("Ability.Combat.Ranged.Fire");
		FGameplayTag AbilityTag = FGameplayTag::RequestGameplayTag(FName(*AbilityTagStr), false);
		if (AbilityTag.IsValid())
		{
			FProperty* Prop = MonolithGAS::FindAbilityAssetTagsProperty(AbilityCDO->GetClass());
			if (Prop)
			{
				FGameplayTagContainer* Tags = Prop->ContainerPtrToValuePtr<FGameplayTagContainer>(AbilityCDO);
				if (Tags) Tags->AddTag(AbilityTag);
			}
		}
	}

	// ActivationBlockedTags — can't fire while dead/stunned/staggered
	{
		FProperty* BlockProp = AbilityCDO->GetClass()->FindPropertyByName(TEXT("ActivationBlockedTags"));
		if (BlockProp)
		{
			FGameplayTagContainer* BlockedTags = BlockProp->ContainerPtrToValuePtr<FGameplayTagContainer>(AbilityCDO);
			if (BlockedTags)
			{
				TArray<FString> BlockTagStrs = {
					TEXT("State.Combat.Dead"), TEXT("State.Combat.Stunned"), TEXT("State.Combat.Staggered")
				};
				if (!bIsMelee) BlockTagStrs.Add(TEXT("State.Combat.Reloading"));

				for (const FString& TagStr : BlockTagStrs)
				{
					FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagStr), false);
					if (Tag.IsValid()) BlockedTags->AddTag(Tag);
				}
			}
		}
	}

	// Instancing policy: InstancedPerActor
	{
		FProperty* InstancingProp = AbilityCDO->GetClass()->FindPropertyByName(TEXT("InstancingPolicy"));
		if (InstancingProp)
		{
			EGameplayAbilityInstancingPolicy::Type Policy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
			InstancingProp->CopyCompleteValue(InstancingProp->ContainerPtrToValuePtr<void>(AbilityCDO), &Policy);
		}
	}

	// Recompile and save
	FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::SkipGarbageCollection);
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewBP);

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	bool bSaved = UPackage::SavePackage(Package, NewBP, *PackageFilename, SaveArgs);

	TSharedPtr<FJsonObject> WeaponResult = MakeShared<FJsonObject>();
	WeaponResult->SetStringField(TEXT("asset_path"), SavePath);
	WeaponResult->SetStringField(TEXT("weapon_type"), WeaponType);
	WeaponResult->SetStringField(TEXT("fire_mode"), FireMode);
	WeaponResult->SetBoolField(TEXT("is_melee"), bIsMelee);
	WeaponResult->SetBoolField(TEXT("saved"), bSaved);
	WeaponResult->SetStringField(TEXT("instancing_policy"), TEXT("InstancedPerActor"));

	// Describe the expected BP graph flow
	TSharedPtr<FJsonObject> GraphFlow = MakeShared<FJsonObject>();
	if (bIsMelee)
	{
		GraphFlow->SetStringField(TEXT("pattern"), TEXT("montage"));
		GraphFlow->SetStringField(TEXT("flow"),
			TEXT("ActivateAbility -> CommitAbility -> PlayMontageAndWait -> "
				 "OnNotifyBegin: ApplyGE_Damage -> OnCompleted: EndAbility"));
	}
	else if (FireModeLower == TEXT("single"))
	{
		GraphFlow->SetStringField(TEXT("pattern"), TEXT("instant"));
		GraphFlow->SetStringField(TEXT("flow"),
			TEXT("ActivateAbility -> CommitAbility -> Fire trace/projectile -> "
				 "ApplyGE_Damage to hit target -> EndAbility"));
	}
	else if (FireModeLower == TEXT("burst"))
	{
		GraphFlow->SetStringField(TEXT("pattern"), TEXT("burst_loop"));
		GraphFlow->SetStringField(TEXT("flow"),
			TEXT("ActivateAbility -> CommitAbility -> Loop(3 rounds): "
				 "Fire + WaitDelay(burst_interval) -> EndAbility"));
	}
	else // auto
	{
		GraphFlow->SetStringField(TEXT("pattern"), TEXT("channeled"));
		GraphFlow->SetStringField(TEXT("flow"),
			TEXT("ActivateAbility -> CommitAbility -> RepeatAction(fire_rate): "
				 "Fire each tick -> WaitInputRelease -> EndAbility"));
	}
	WeaponResult->SetObjectField(TEXT("graph_flow"), GraphFlow);

	TArray<TSharedPtr<FJsonValue>> NextSteps;
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Open the ability Blueprint and implement the graph flow")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Create a GE_Damage_[Type] via scaffold_damage_pipeline")));
	if (!bIsMelee)
	{
		NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Create a reload ability (GA_Reload) and bind to input")));
		NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Wire to MassProjectile system via OnProjectileHitDelegate")));
	}
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Bind to input via bind_ability_to_input")));
	WeaponResult->SetArrayField(TEXT("next_steps"), NextSteps);

	WeaponResult->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Created %s weapon ability '%s' with %s fire mode"),
			*WeaponType, *AssetName, *FireMode));

	return FMonolithActionResult::Success(WeaponResult);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase F8 (J-phase): grant_ability_to_pawn
//
//  Author-time mutation of a pawn BP. Steps:
//   1. Load the BP, find its UAbilitySystemComponent SCS node.
//   2. Get the ASC component template (per-instance defaults — what runs on PIE).
//   3. Reflect over the ASC class for any TArray<TSubclassOf<UGameplayAbility>>
//      UPROPERTY whose name contains "Ability" (matches the conventional
//      StartupAbilities / DefaultAbilities pattern across most projects). Stock
//      UAbilitySystemComponent has NO such property — the action returns a
//      clear error if no array is found.
//   4. Append the resolved ability class (skip if duplicate) and mark BP
//      structurally modified, then compile.
//
//  We deliberately stop short of writing to UPROPERTY shapes other than
//  TArray<TSubclassOf<UGameplayAbility>> — projects that store starts as
//  TArray<FGameplayAbilitySpec> need a per-project handler, and v1 documents
//  this as a known limitation.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	/** Mirror of the resolver in MonolithGASASCActions.cpp. Asset-path-first, falls back to native class lookup + asset-registry scan for bare BP names. */
	TSubclassOf<UGameplayAbility> ResolveAbilityClassForGrant(const FString& ClassId, FString& OutError)
	{
		// Try asset path form first (e.g. /Game/.../GA_Test).
		if (ClassId.StartsWith(TEXT("/")))
		{
			FString BPPath = ClassId;
			if (!BPPath.EndsWith(TEXT("_C")))
			{
				FString BaseName = FPaths::GetBaseFilename(ClassId);
				BPPath = ClassId + TEXT(".") + BaseName + TEXT("_C");
			}
			if (UClass* LoadedClass = LoadClass<UGameplayAbility>(nullptr, *BPPath))
			{
				return LoadedClass;
			}
		}

		// Native class lookup (handles "MyAbility" and "UMyAbility").
		UClass* FoundClass = FindFirstObject<UClass>(*ClassId, EFindFirstObjectOptions::NativeFirst);
		if (!FoundClass)
		{
			FoundClass = FindFirstObject<UClass>(*(TEXT("U") + ClassId), EFindFirstObjectOptions::NativeFirst);
		}
		if (!FoundClass && !ClassId.EndsWith(TEXT("_C")))
		{
			FoundClass = FindFirstObject<UClass>(*(ClassId + TEXT("_C")), EFindFirstObjectOptions::NativeFirst);
		}
		if (FoundClass && FoundClass->IsChildOf(UGameplayAbility::StaticClass()))
		{
			return FoundClass;
		}

		// Asset-registry fallback for bare BP name with no path.
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FString SearchName = ClassId;
		if (SearchName.EndsWith(TEXT("_C"))) SearchName = SearchName.LeftChop(2);
		TArray<FAssetData> Assets;
		AR.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Assets);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetName.ToString() == SearchName)
			{
				const FString BPPath = Asset.GetObjectPathString() + TEXT("_C");
				if (UClass* Loaded = LoadClass<UGameplayAbility>(nullptr, *BPPath))
				{
					return Loaded;
				}
			}
		}

		OutError = FString::Printf(TEXT("GameplayAbility class not found: '%s'"), *ClassId);
		return nullptr;
	}

	/**
	 * Find an SCS node on `BP` whose component class derives from UAbilitySystemComponent.
	 * Returns nullptr if BP has no SCS or no ASC component.
	 */
	USCS_Node* FindASCSCSNode(UBlueprint* BP)
	{
		if (!BP || !BP->SimpleConstructionScript) return nullptr;
		for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->ComponentClass &&
				Node->ComponentClass->IsChildOf(UAbilitySystemComponent::StaticClass()))
			{
				return Node;
			}
		}
		return nullptr;
	}
}

FMonolithActionResult FMonolithGASScaffoldActions::HandleGrantAbilityToPawn(const TSharedPtr<FJsonObject>& Params)
{
	FString PawnPath, AbilityClassId;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("pawn_bp_path"), PawnPath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("ability_class_path"), AbilityClassId, Err)) return Err;

	double LevelD = 1.0;
	double InputIDD = -1.0;
	Params->TryGetNumberField(TEXT("level"), LevelD);
	Params->TryGetNumberField(TEXT("input_id"), InputIDD);
	const int32 Level = static_cast<int32>(LevelD);
	const int32 InputID = static_cast<int32>(InputIDD);

	// Load BP via the centralized 4-tier resolver. We skip
	// MonolithGAS::LoadBlueprintFromParams because it expects the legacy
	// "asset_path" key — this F8 action uses "pawn_bp_path" to match the spec.
	FString LoadError;
	UObject* Obj = MonolithGAS::LoadAssetFromPath(PawnPath, LoadError);
	UBlueprint* BP = Cast<UBlueprint>(Obj);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Pawn Blueprint not found at '%s' (%s)"), *PawnPath, *LoadError));
	}

	if (!BP->GeneratedClass || !BP->GeneratedClass->IsChildOf(AActor::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset at '%s' is not an Actor Blueprint"), *PawnPath));
	}

	// Resolve ability class.
	FString ResolveError;
	TSubclassOf<UGameplayAbility> AbilityClass = ResolveAbilityClassForGrant(AbilityClassId, ResolveError);
	if (!AbilityClass)
	{
		return FMonolithActionResult::Error(ResolveError);
	}

	// Find ASC SCS node.
	USCS_Node* ASCNode = FindASCSCSNode(BP);
	UAbilitySystemComponent* ASCTemplate = nullptr;
	UClass* ASCClass = nullptr;

	if (ASCNode)
	{
		ASCTemplate = Cast<UAbilitySystemComponent>(ASCNode->GetActualComponentTemplate(
			Cast<UBlueprintGeneratedClass>(BP->GeneratedClass)));
		ASCClass = ASCNode->ComponentClass;
	}
	else
	{
		// Fall back to a native ASC on the parent CDO (e.g. parent C++ Pawn class
		// with ASC declared as a UPROPERTY).
		if (AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject()))
		{
			ASCTemplate = CDO->FindComponentByClass<UAbilitySystemComponent>();
			if (ASCTemplate) ASCClass = ASCTemplate->GetClass();
		}
	}

	if (!ASCTemplate || !ASCClass)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Pawn '%s' has no UAbilitySystemComponent. Add one via gas::add_asc_to_actor first."), *PawnPath));
	}

	// Reflect for a TArray<TSubclassOf<UGameplayAbility>> UPROPERTY whose name contains "Ability".
	FArrayProperty* TargetArrayProp = nullptr;
	for (TFieldIterator<FProperty> PropIt(ASCClass); PropIt; ++PropIt)
	{
		FArrayProperty* ArrayProp = CastField<FArrayProperty>(*PropIt);
		if (!ArrayProp) continue;

		const FString PropName = ArrayProp->GetName();
		if (!PropName.Contains(TEXT("Ability"), ESearchCase::IgnoreCase)) continue;

		// Inner must be a class-property (TSubclassOf<T>). The MetaClass narrows
		// what concrete classes the array slot accepts. We accept the array iff
		// our resolved AbilityClass would be storable in it — i.e. AbilityClass
		// is a child of MetaClass (covers MetaClass=UGameplayAbility AND
		// MetaClass=UObject AND project-specific GA subclass bases).
		FClassProperty* InnerClassProp = CastField<FClassProperty>(ArrayProp->Inner);
		if (!InnerClassProp || !InnerClassProp->MetaClass) continue;
		if (!AbilityClass->IsChildOf(InnerClassProp->MetaClass)) continue;

		TargetArrayProp = ArrayProp;
		break;
	}

	if (!TargetArrayProp)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("ASC class '%s' has no TArray<TSubclassOf<UGameplayAbility>> UPROPERTY whose name contains 'Ability' (e.g. StartupAbilities, DefaultAbilities). Stock UAbilitySystemComponent does NOT ship one — subclass it and add a TArray<TSubclassOf<UGameplayAbility>> UPROPERTY to use this action."),
			*ASCClass->GetName()));
	}

	// Append the ability class (skip if duplicate).
	bool bAlreadyGranted = false;
	{
		FScriptArrayHelper Helper(TargetArrayProp, TargetArrayProp->ContainerPtrToValuePtr<void>(ASCTemplate));
		for (int32 i = 0; i < Helper.Num(); ++i)
		{
			if (FClassProperty* InnerClassProp = CastField<FClassProperty>(TargetArrayProp->Inner))
			{
				UObject* Existing = InnerClassProp->GetObjectPropertyValue(Helper.GetRawPtr(i));
				if (Existing == AbilityClass)
				{
					bAlreadyGranted = true;
					break;
				}
			}
		}

		if (!bAlreadyGranted)
		{
			const int32 NewIdx = Helper.AddValue();
			if (FClassProperty* InnerClassProp = CastField<FClassProperty>(TargetArrayProp->Inner))
			{
				InnerClassProp->SetObjectPropertyValue(Helper.GetRawPtr(NewIdx), AbilityClass);
			}
		}
	}

	// Mark + compile.
	if (!bAlreadyGranted)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("pawn_bp_path"), PawnPath);
	Result->SetStringField(TEXT("ability_class_path"), AbilityClassId);
	Result->SetNumberField(TEXT("level"), Level);
	Result->SetNumberField(TEXT("input_id"), InputID);
	Result->SetStringField(TEXT("asc_class"), ASCClass->GetName());
	Result->SetStringField(TEXT("startup_array_property"), TargetArrayProp->GetName());
	Result->SetBoolField(TEXT("already_granted"), bAlreadyGranted);
	Result->SetStringField(TEXT("message"),
		bAlreadyGranted
			? FString::Printf(TEXT("Ability '%s' was already in '%s.%s' on '%s' — no change."),
				*AbilityClass->GetName(), *ASCClass->GetName(), *TargetArrayProp->GetName(), *PawnPath)
			: FString::Printf(TEXT("Appended '%s' to '%s.%s' on '%s' and recompiled BP."),
				*AbilityClass->GetName(), *ASCClass->GetName(), *TargetArrayProp->GetName(), *PawnPath));
	return FMonolithActionResult::Success(Result);
}
