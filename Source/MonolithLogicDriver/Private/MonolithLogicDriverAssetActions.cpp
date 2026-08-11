#include "MonolithLogicDriverAssetActions.h"
#include "AssetRegistry/AssetData.h"
#include "MonolithParamSchema.h"

#if WITH_LOGICDRIVER

#include "MonolithLogicDriverInternal.h"
#include "Engine/Blueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Factories/Factory.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EditorAssetLibrary.h"
#include "UObject/GarbageCollection.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithLDAsset, Log, All);

// ============================================================
//  Anonymous helpers
// ============================================================
namespace
{
	FString ExtractAssetName(const FString& Path)
	{
		FString AssetName = FPackageName::GetLongPackageAssetName(Path);
		if (AssetName.IsEmpty())
		{
			int32 LastSlash;
			if (Path.FindLastChar(TEXT('/'), LastSlash))
			{
				AssetName = Path.Mid(LastSlash + 1);
			}
			else
			{
				AssetName = Path;
			}
		}
		return AssetName;
	}

	bool SaveSMAsset(UObject* Asset)
	{
		if (!Asset) return false;
		UPackage* Package = Asset->GetPackage();
		FString PackageFilename = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Package, Asset, *PackageFilename, SaveArgs);
	}
}

// ============================================================
//  RegisterActions
// ============================================================
void FMonolithLogicDriverAssetActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("logicdriver"), TEXT("create_state_machine"),
		TEXT("Create a new Logic Driver State Machine Blueprint via USMBlueprintFactory"),
		FMonolithActionHandler::CreateStatic(&HandleCreateStateMachine),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path for the new SM Blueprint (e.g. /Game/AI/SM_EnemyBehavior)"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Override asset name (extracted from save_path if omitted)"))
			.Optional(TEXT("parent_class"), TEXT("string"), TEXT("Parent USMInstance class name (default: SMInstance)"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("get_state_machine"),
		TEXT("Get full JSON dump of a state machine's structure: states, transitions, conduits, nested SMs"),
		FMonolithActionHandler::CreateStatic(&HandleGetStateMachine),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("SM Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("list_state_machines"),
		TEXT("List all Logic Driver State Machine Blueprints in the project via AssetRegistry"),
		FMonolithActionHandler::CreateStatic(&HandleListStateMachines),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("path_filter"), TEXT("Filter by path prefix (e.g. /Game/AI)"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("delete_state_machine"),
		TEXT("Delete a Logic Driver State Machine Blueprint asset"),
		FMonolithActionHandler::CreateStatic(&HandleDeleteStateMachine),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("SM Blueprint asset path to delete"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("duplicate_state_machine"),
		TEXT("Deep copy a Logic Driver State Machine Blueprint to a new path"),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateStateMachine),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("source_path"), TEXT("Source SM Blueprint asset path"))
			.RequiredAssetPath(TEXT("dest_path"), TEXT("Destination asset path for the copy"))
			.Build());

	// ── Node Blueprint CRUD (Phase 2) ──

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("create_node_blueprint"),
		TEXT("Create a new Logic Driver Node Blueprint (custom state, transition, conduit, or state machine node class)"),
		FMonolithActionHandler::CreateStatic(&HandleCreateNodeBlueprint),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path for the new Node Blueprint (e.g. /Game/AI/Nodes/NBP_CustomState)"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Asset name"))
			.Required(TEXT("node_type"), TEXT("string"), TEXT("Node type: state, transition, conduit, state_machine"))
			.Optional(TEXT("parent_class"), TEXT("string"), TEXT("Parent node instance class name (default: auto from node_type)"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("get_node_blueprint"),
		TEXT("Get info about a Logic Driver Node Blueprint: class hierarchy, node type, properties"),
		FMonolithActionHandler::CreateStatic(&HandleGetNodeBlueprint),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Node Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("list_node_blueprints"),
		TEXT("List all Logic Driver Node Blueprints in the project"),
		FMonolithActionHandler::CreateStatic(&HandleListNodeBlueprints),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("path_filter"), TEXT("Filter by path prefix (e.g. /Game/AI/Nodes)"))
			.Optional(TEXT("node_type"), TEXT("string"), TEXT("Filter by node type: state, transition, conduit, state_machine"))
			.Build());

	UE_LOG(LogMonolithLDAsset, Log, TEXT("MonolithLogicDriver Asset: registered 8 actions"));
}

// ============================================================
//  1. create_state_machine
// ============================================================
FMonolithActionResult FMonolithLogicDriverAssetActions::HandleCreateStateMachine(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'save_path'"));
	}

	// Determine asset name
	FString AssetName;
	if (Params->HasField(TEXT("name")) && !Params->GetStringField(TEXT("name")).IsEmpty())
	{
		AssetName = Params->GetStringField(TEXT("name"));
	}
	else
	{
		AssetName = ExtractAssetName(SavePath);
	}
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Could not determine asset name from save_path"));
	}

	// Find factory via FindFirstObject (hybrid approach — marketplace plugin may be precompiled)
	UClass* FactoryClass = MonolithLD::GetSMBlueprintFactoryClass();
	if (!FactoryClass)
	{
		return FMonolithActionResult::Error(TEXT("SMBlueprintFactory class not found. Is Logic Driver Pro loaded?"));
	}

	// Create factory instance
	UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
	if (!Factory)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create SMBlueprintFactory instance"));
	}

	// If parent_class is specified, set it on the factory via reflection
	FString ParentClassName = Params->GetStringField(TEXT("parent_class"));
	if (!ParentClassName.IsEmpty())
	{
		UClass* ParentClass = FindFirstObject<UClass>(*ParentClassName, EFindFirstObjectOptions::NativeFirst);
		if (!ParentClass)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Parent class '%s' not found"), *ParentClassName));
		}
		// SMBlueprintFactory stores the parent in a "ParentClass" property
		FObjectProperty* ParentProp = CastField<FObjectProperty>(FactoryClass->FindPropertyByName(TEXT("ParentClass")));
		if (ParentProp)
		{
			ParentProp->SetObjectPropertyValue(ParentProp->ContainerPtrToValuePtr<void>(Factory), ParentClass);
		}
	}

	// Create package
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to create package at: %s"), *SavePath));
	}
	Package->FullyLoad();

	// Guard AFTER FullyLoad (FullyLoad can re-populate package with stale content from disk)
	FString ExistError;
	if (!MonolithLD::EnsureAssetPathFree(Package, SavePath, AssetName, ExistError))
	{
		return FMonolithActionResult::Error(ExistError);
	}

	// Create the SM Blueprint via factory
	UClass* SupportedClass = Factory->GetSupportedClass();
	if (!SupportedClass)
	{
		// Fallback — try SMBlueprint class via reflection
		SupportedClass = MonolithLD::GetSMBlueprintClass();
	}

	UObject* NewAsset = Factory->FactoryCreateNew(
		SupportedClass, Package, FName(*AssetName),
		RF_Public | RF_Standalone, nullptr, GWarn);

	if (!NewAsset)
	{
		return FMonolithActionResult::Error(TEXT("FactoryCreateNew returned null"));
	}

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewAsset);

	// Compile the new blueprint
	UBlueprint* SMBlueprint = Cast<UBlueprint>(NewAsset);
	if (SMBlueprint)
	{
		FString CompileError;
		MonolithLD::CompileSMBlueprint(SMBlueprint, CompileError);
		// Non-fatal if compile has warnings on fresh BP
	}

	// Save to disk
	bool bSaved = SaveSMAsset(NewAsset);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetStringField(TEXT("asset_name"), AssetName);
	Result->SetStringField(TEXT("class"), NewAsset->GetClass()->GetName());
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Created SM Blueprint '%s'"), *AssetName));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  2. get_state_machine
// ============================================================
FMonolithActionResult FMonolithLogicDriverAssetActions::HandleGetStateMachine(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));
	}

	FString LoadError;
	UBlueprint* SMBlueprint = MonolithLD::LoadSMBlueprint(AssetPath, LoadError);
	if (!SMBlueprint)
	{
		return FMonolithActionResult::Error(LoadError);
	}

	TSharedPtr<FJsonObject> Structure = MonolithLD::SMStructureToJson(SMBlueprint);
	if (!Structure.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Failed to serialize state machine structure"));
	}

	return FMonolithActionResult::Success(Structure);
}

// ============================================================
//  3. list_state_machines
// ============================================================
FMonolithActionResult FMonolithLogicDriverAssetActions::HandleListStateMachines(const TSharedPtr<FJsonObject>& Params)
{
	FString PathFilter;
	if (Params->HasField(TEXT("path_filter")))
	{
		PathFilter = Params->GetStringField(TEXT("path_filter"));
	}

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	UClass* SMBPClass = MonolithLD::GetSMBlueprintClass();
	if (!SMBPClass)
	{
		return FMonolithActionResult::Error(TEXT("Logic Driver not loaded — SMBlueprint class not found"));
	}
	Filter.ClassPaths.Add(SMBPClass->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;

	if (!PathFilter.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PathFilter));
		Filter.bRecursivePaths = true;
	}

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> AssetArray;
	for (const FAssetData& AssetData : Assets)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		Entry->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		Entry->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		AssetArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("state_machines"), AssetArray);
	Result->SetNumberField(TEXT("count"), Assets.Num());

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  4. delete_state_machine
// ============================================================
FMonolithActionResult FMonolithLogicDriverAssetActions::HandleDeleteStateMachine(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));
	}

	// Verify asset exists and is an SM Blueprint
	FString LoadError;
	UBlueprint* SMBlueprint = MonolithLD::LoadSMBlueprint(AssetPath, LoadError);
	if (!SMBlueprint)
	{
		return FMonolithActionResult::Error(LoadError);
	}

	// Use EditorAssetLibrary for clean deletion (handles references, etc.)
	bool bDeleted = UEditorAssetLibrary::DeleteAsset(AssetPath);

	if (bDeleted)
	{
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("deleted"), bDeleted);
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	if (bDeleted)
	{
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Deleted SM Blueprint at '%s'"), *AssetPath));
	}
	else
	{
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Failed to delete SM Blueprint at '%s' — may have active references"), *AssetPath));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  5. duplicate_state_machine
// ============================================================
FMonolithActionResult FMonolithLogicDriverAssetActions::HandleDuplicateStateMachine(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath = Params->GetStringField(TEXT("source_path"));
	if (SourcePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'source_path'"));
	}

	FString DestPath = Params->GetStringField(TEXT("dest_path"));
	if (DestPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'dest_path'"));
	}

	// Verify source exists and is an SM Blueprint
	FString LoadError;
	UBlueprint* SourceBP = MonolithLD::LoadSMBlueprint(SourcePath, LoadError);
	if (!SourceBP)
	{
		return FMonolithActionResult::Error(LoadError);
	}

	// Check dest is free (no package context needed for duplicate — EditorAssetLibrary handles it)
	FString DestName = ExtractAssetName(DestPath);
	FString DupExistError;
	if (!MonolithLD::EnsureAssetPathFree(nullptr, DestPath, DestName, DupExistError))
	{
		return FMonolithActionResult::Error(DupExistError);
	}

	// Duplicate via EditorAssetLibrary
	UObject* NewAsset = UEditorAssetLibrary::DuplicateAsset(SourcePath, DestPath);
	if (!NewAsset)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to duplicate '%s' to '%s'"), *SourcePath, *DestPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source_path"), SourcePath);
	Result->SetStringField(TEXT("dest_path"), DestPath);
	Result->SetStringField(TEXT("asset_name"), DestName);
	Result->SetStringField(TEXT("class"), NewAsset->GetClass()->GetName());
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Duplicated SM Blueprint to '%s'"), *DestPath));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  6. create_node_blueprint
// ============================================================
FMonolithActionResult FMonolithLogicDriverAssetActions::HandleCreateNodeBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	FString AssetName = Params->GetStringField(TEXT("name"));
	FString NodeType = Params->GetStringField(TEXT("node_type")).ToLower();

	if (SavePath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'save_path'"));
	if (AssetName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'name'"));
	if (NodeType.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'node_type'"));

	if (NodeType != TEXT("state") && NodeType != TEXT("transition")
		&& NodeType != TEXT("conduit") && NodeType != TEXT("state_machine"))
	{
		return FMonolithActionResult::Error(TEXT("node_type must be: state, transition, conduit, state_machine"));
	}

	// Get factory
	UClass* FactoryClass = MonolithLD::GetSMNodeBlueprintFactoryClass();
	if (!FactoryClass)
	{
		return FMonolithActionResult::Error(TEXT("SMNodeBlueprintFactory class not found. Is Logic Driver Pro loaded?"));
	}

	UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
	if (!Factory)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create SMNodeBlueprintFactory instance"));
	}

	// Set ParentClass on the factory based on node_type or explicit parent_class
	FString ParentClassName = Params->GetStringField(TEXT("parent_class"));
	if (ParentClassName.IsEmpty())
	{
		// Map node_type to default parent class names
		if (NodeType == TEXT("state"))              ParentClassName = TEXT("SMStateInstance");
		else if (NodeType == TEXT("transition"))    ParentClassName = TEXT("SMTransitionInstance");
		else if (NodeType == TEXT("conduit"))       ParentClassName = TEXT("SMConduitInstance");
		else if (NodeType == TEXT("state_machine")) ParentClassName = TEXT("SMStateMachineInstance");
	}

	if (!ParentClassName.IsEmpty())
	{
		UClass* ParentClass = FindFirstObject<UClass>(*ParentClassName, EFindFirstObjectOptions::NativeFirst);
		if (!ParentClass)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Parent class '%s' not found"), *ParentClassName));
		}
		FObjectProperty* ParentProp = CastField<FObjectProperty>(FactoryClass->FindPropertyByName(TEXT("ParentClass")));
		if (ParentProp)
		{
			ParentProp->SetObjectPropertyValue(ParentProp->ContainerPtrToValuePtr<void>(Factory), ParentClass);
		}
	}

	// Create package
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at: %s"), *SavePath));
	}
	Package->FullyLoad();

	// Guard AFTER FullyLoad (FullyLoad can re-populate package with stale content from disk)
	FString ExistError;
	if (!MonolithLD::EnsureAssetPathFree(Package, SavePath, AssetName, ExistError))
	{
		return FMonolithActionResult::Error(ExistError);
	}

	// Create via factory
	UClass* SupportedClass = Factory->GetSupportedClass();
	if (!SupportedClass)
	{
		SupportedClass = MonolithLD::GetSMNodeBlueprintClass();
	}

	UObject* NewAsset = Factory->FactoryCreateNew(
		SupportedClass, Package, FName(*AssetName),
		RF_Public | RF_Standalone, nullptr, GWarn);

	if (!NewAsset)
	{
		return FMonolithActionResult::Error(TEXT("FactoryCreateNew returned null for Node Blueprint"));
	}

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewAsset);

	// Compile
	UBlueprint* NodeBP = Cast<UBlueprint>(NewAsset);
	if (NodeBP)
	{
		FString CompileError;
		MonolithLD::CompileSMBlueprint(NodeBP, CompileError);
	}

	bool bSaved = SaveSMAsset(NewAsset);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetStringField(TEXT("asset_name"), AssetName);
	Result->SetStringField(TEXT("node_type"), NodeType);
	Result->SetStringField(TEXT("class"), NewAsset->GetClass()->GetName());
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Created Node Blueprint '%s' (type: %s)"), *AssetName, *NodeType));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  7. get_node_blueprint
// ============================================================
FMonolithActionResult FMonolithLogicDriverAssetActions::HandleGetNodeBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));
	}

	UClass* NodeBPClass = MonolithLD::GetSMNodeBlueprintClass();
	if (!NodeBPClass)
	{
		return FMonolithActionResult::Error(TEXT("Logic Driver not loaded — SMNodeBlueprint class not found"));
	}

	UObject* LoadedObj = StaticLoadObject(UBlueprint::StaticClass(), nullptr, *AssetPath);
	UBlueprint* BP = Cast<UBlueprint>(LoadedObj);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load Blueprint at '%s'"), *AssetPath));
	}
	if (!BP->GetClass()->IsChildOf(NodeBPClass))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("'%s' is not a Node Blueprint (class: %s)"), *AssetPath, *BP->GetClass()->GetName()));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("asset_name"), BP->GetName());
	Result->SetStringField(TEXT("blueprint_class"), BP->GetClass()->GetName());

	// Parent class info
	if (BP->ParentClass)
	{
		Result->SetStringField(TEXT("parent_class"), BP->ParentClass->GetName());
		Result->SetStringField(TEXT("parent_class_path"), BP->ParentClass->GetPathName());

		// Determine node type from parent class name
		FString ParentName = BP->ParentClass->GetName();
		FString NodeType = TEXT("unknown");
		if (ParentName.Contains(TEXT("Transition")))      NodeType = TEXT("transition");
		else if (ParentName.Contains(TEXT("Conduit")))     NodeType = TEXT("conduit");
		else if (ParentName.Contains(TEXT("StateMachine"))) NodeType = TEXT("state_machine");
		else if (ParentName.Contains(TEXT("State")))        NodeType = TEXT("state");
		Result->SetStringField(TEXT("node_type"), NodeType);
	}

	// Generated class
	if (BP->GeneratedClass)
	{
		Result->SetStringField(TEXT("generated_class"), BP->GeneratedClass->GetName());
	}

	// Blueprint status
	Result->SetStringField(TEXT("status"),
		BP->Status == BS_Error ? TEXT("error") :
		BP->Status == BS_UpToDate ? TEXT("up_to_date") :
		BP->Status == BS_Dirty ? TEXT("dirty") : TEXT("unknown"));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  8. list_node_blueprints
// ============================================================
FMonolithActionResult FMonolithLogicDriverAssetActions::HandleListNodeBlueprints(const TSharedPtr<FJsonObject>& Params)
{
	UClass* NodeBPClass = MonolithLD::GetSMNodeBlueprintClass();
	if (!NodeBPClass)
	{
		return FMonolithActionResult::Error(TEXT("Logic Driver not loaded — SMNodeBlueprint class not found"));
	}

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(NodeBPClass->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;

	FString PathFilter;
	if (Params->HasField(TEXT("path_filter")))
	{
		PathFilter = Params->GetStringField(TEXT("path_filter"));
		if (!PathFilter.IsEmpty())
		{
			Filter.PackagePaths.Add(FName(*PathFilter));
		}
	}

	FString NodeTypeFilter;
	if (Params->HasField(TEXT("node_type")))
	{
		NodeTypeFilter = Params->GetStringField(TEXT("node_type")).ToLower();
	}

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> ResultArray;
	for (const FAssetData& AssetData : Assets)
	{
		// Extract parent class tag to determine node type
		FString ParentClassTag;
		AssetData.GetTagValue(FName(TEXT("ParentClass")), ParentClassTag);

		FString NodeType = TEXT("unknown");
		if (ParentClassTag.Contains(TEXT("Transition")))       NodeType = TEXT("transition");
		else if (ParentClassTag.Contains(TEXT("Conduit")))     NodeType = TEXT("conduit");
		else if (ParentClassTag.Contains(TEXT("StateMachine"))) NodeType = TEXT("state_machine");
		else if (ParentClassTag.Contains(TEXT("State")))        NodeType = TEXT("state");

		// Apply node_type filter if specified
		if (!NodeTypeFilter.IsEmpty() && NodeType != NodeTypeFilter)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		Entry->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		Entry->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		Entry->SetStringField(TEXT("node_type"), NodeType);
		ResultArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("node_blueprints"), ResultArray);
	Result->SetNumberField(TEXT("count"), ResultArray.Num());

	return FMonolithActionResult::Success(Result);
}

#else

void FMonolithLogicDriverAssetActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// Logic Driver not available
}

#endif // WITH_LOGICDRIVER
