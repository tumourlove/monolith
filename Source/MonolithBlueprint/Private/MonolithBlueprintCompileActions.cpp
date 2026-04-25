#include "MonolithBlueprintCompileActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "K2Node_Variable.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "MonolithBlueprintEditCradle.h"
#include "ScopedTransaction.h"

// ============================================================
//  Registration
// ============================================================

void FMonolithBlueprintCompileActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("compile_blueprint"),
		TEXT("Compile a Blueprint asset and return errors, warnings, and compile status."),
		FMonolithActionHandler::CreateStatic(&HandleCompileBlueprint),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("validate_blueprint"),
		TEXT("Validate a Blueprint without compiling — returns unused variables, disconnected nodes, and compiler messages already stored on nodes."),
		FMonolithActionHandler::CreateStatic(&HandleValidateBlueprint),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("create_blueprint"),
		TEXT("Create a new Blueprint asset at the given save path with the specified parent class and blueprint type."),
		FMonolithActionHandler::CreateStatic(&HandleCreateBlueprint),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"),      TEXT("string"),  TEXT("Asset save path, e.g. /Game/Test/BP_MyActor"))
			.Required(TEXT("parent_class"),   TEXT("string"),  TEXT("Parent class name, e.g. Actor, Pawn, Character"))
			.Optional(TEXT("blueprint_type"), TEXT("string"),  TEXT("Blueprint type: Normal, Const, MacroLibrary, Interface, FunctionLibrary (default: Normal)"), TEXT("Normal"))
			.Optional(TEXT("skip_save"),      TEXT("boolean"), TEXT("Skip the synchronous package save — Blueprint exists in-memory and can be saved later (default: false)"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("duplicate_blueprint"),
		TEXT("Duplicate an existing Blueprint asset to a new path."),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateBlueprint),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Source Blueprint asset path"))
			.Required(TEXT("new_path"),   TEXT("string"), TEXT("Destination asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_dependencies"),
		TEXT("Get asset dependencies for a Blueprint using the Asset Registry. Reports what the Blueprint depends on and/or what references it."),
		FMonolithActionHandler::CreateStatic(&HandleGetDependencies),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Optional(TEXT("direction"),  TEXT("string"), TEXT("depends_on, referenced_by, or both (default: both)"), TEXT("both"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("save_asset"),
		TEXT("Save a loaded asset to disk. Works on any asset type, not just Blueprints."),
		FMonolithActionHandler::CreateStatic(&HandleSaveAsset),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path to save"))
			.Build());
}

// ============================================================
//  compile_blueprint
// ============================================================

FMonolithActionResult FMonolithBlueprintCompileActions::HandleCompileBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FCompilerResultsLog Results;
	FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

	// Build a map of compiler messages on nodes (node_id -> {graph_name, error_msg, severity})
	// This is the proven fallback approach using bHasCompilerMessage
	TMap<FString, TPair<FString, FString>> NodeErrorMap; // ErrorMsg -> {NodeId, GraphName}
	{
		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || !Node->bHasCompilerMessage) continue;
				if (!Node->ErrorMsg.IsEmpty())
				{
					NodeErrorMap.Add(Node->ErrorMsg, TPair<FString, FString>(Node->GetName(), Graph->GetName()));
				}
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> ErrorArr, WarnArr;
	for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
	{
		TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
		FString MsgText = Msg->ToText().ToString();
		MsgObj->SetStringField(TEXT("message"), MsgText);

		// Try to match this message to a node with compiler errors
		for (const auto& Pair : NodeErrorMap)
		{
			if (MsgText.Contains(Pair.Key) || Pair.Key.Contains(MsgText))
			{
				MsgObj->SetStringField(TEXT("node_id"), Pair.Value.Key);
				MsgObj->SetStringField(TEXT("graph_name"), Pair.Value.Value);
				break;
			}
		}

		if (Msg->GetSeverity() == EMessageSeverity::Error)
		{
			ErrorArr.Add(MakeShared<FJsonValueObject>(MsgObj));
		}
		else if (Msg->GetSeverity() == EMessageSeverity::Warning)
		{
			WarnArr.Add(MakeShared<FJsonValueObject>(MsgObj));
		}
	}

	FString StatusStr;
	switch (BP->Status)
	{
	case BS_Unknown:             StatusStr = TEXT("Unknown"); break;
	case BS_Dirty:               StatusStr = TEXT("Dirty"); break;
	case BS_Error:               StatusStr = TEXT("Error"); break;
	case BS_UpToDate:            StatusStr = TEXT("UpToDate"); break;
	case BS_UpToDateWithWarnings: StatusStr = TEXT("UpToDateWithWarnings"); break;
	case BS_BeingCreated:        StatusStr = TEXT("BeingCreated"); break;
	default:                     StatusStr = TEXT("Unknown"); break;
	}

	bool bSuccess = (BP->Status == BS_UpToDate || BP->Status == BS_UpToDateWithWarnings);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetBoolField(TEXT("success"), bSuccess);
	Root->SetStringField(TEXT("status"), StatusStr);
	Root->SetArrayField(TEXT("errors"), ErrorArr);
	Root->SetArrayField(TEXT("warnings"), WarnArr);
	Root->SetNumberField(TEXT("error_count"), ErrorArr.Num());
	Root->SetNumberField(TEXT("warning_count"), WarnArr.Num());
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  validate_blueprint
// ============================================================

FMonolithActionResult FMonolithBlueprintCompileActions::HandleValidateBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	// --- Unused variables ---
	TArray<TSharedPtr<FJsonValue>> UnusedVars;
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		bool bReferenced = false;
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			TArray<UK2Node_Variable*> VarNodes;
			Graph->GetNodesOfClass<UK2Node_Variable>(VarNodes);
			for (UK2Node_Variable* VarNode : VarNodes)
			{
				if (VarNode->GetVarName() == Var.VarName)
				{
					bReferenced = true;
					break;
				}
			}
			if (bReferenced) break;
		}
		if (!bReferenced)
		{
			UnusedVars.Add(MakeShared<FJsonValueString>(Var.VarName.ToString()));
		}
	}

	// --- Disconnected nodes ---
	TArray<TSharedPtr<FJsonValue>> DisconnectedNodes;
	int32 TotalNodes = 0;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		TotalNodes += Graph->Nodes.Num();
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_Event>()) continue;
			if (Cast<UEdGraphNode_Comment>(Node)) continue;

			bool bHasExecInput = false;
			bool bExecInputConnected = false;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->Direction == EGPD_Input)
				{
					bHasExecInput = true;
					if (Pin->LinkedTo.Num() > 0)
					{
						bExecInputConnected = true;
					}
				}
			}
			if (bHasExecInput && !bExecInputConnected)
			{
				TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
				NodeObj->SetStringField(TEXT("node_id"), Node->GetName());
				NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
				UEdGraph* DNodeGraph = Node->GetGraph();
				NodeObj->SetStringField(TEXT("graph"), DNodeGraph ? DNodeGraph->GetName() : TEXT("(orphaned)"));
				DisconnectedNodes.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
		}
	}

	// --- Node errors ---
	TArray<TSharedPtr<FJsonValue>> NodeErrors;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (Node->bHasCompilerMessage && Node->ErrorType <= EMessageSeverity::Warning)
			{
				TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
				ErrObj->SetStringField(TEXT("node_id"), Node->GetName());
				ErrObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
				ErrObj->SetStringField(TEXT("message"), Node->ErrorMsg);
				ErrObj->SetStringField(TEXT("severity"), Node->ErrorType == EMessageSeverity::Error ? TEXT("error") : TEXT("warning"));
				UEdGraph* ErrNodeGraph = Node->GetGraph();
				ErrObj->SetStringField(TEXT("graph"), ErrNodeGraph ? ErrNodeGraph->GetName() : TEXT("(orphaned)"));
				NodeErrors.Add(MakeShared<FJsonValueObject>(ErrObj));
			}
		}
	}

	// --- Unimplemented interface functions ---
	TArray<TSharedPtr<FJsonValue>> UnimplementedFuncs;

	// Collect all overridden event names across all graphs
	TSet<FName> OverriddenFuncNames;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		TArray<UK2Node_Event*> EventNodes;
		Graph->GetNodesOfClass<UK2Node_Event>(EventNodes);
		for (UK2Node_Event* EN : EventNodes)
		{
			if (EN && EN->bOverrideFunction)
			{
				OverriddenFuncNames.Add(EN->EventReference.GetMemberName());
			}
		}
	}

	for (const FBPInterfaceDescription& InterfaceDesc : BP->ImplementedInterfaces)
	{
		UClass* InterfaceClass = InterfaceDesc.Interface;
		if (!InterfaceClass) continue;

		// Collect names of graphs implemented for this interface
		TSet<FName> ImplementedGraphNames;
		for (UEdGraph* Graph : InterfaceDesc.Graphs)
		{
			if (Graph) ImplementedGraphNames.Add(Graph->GetFName());
		}

		for (TFieldIterator<UFunction> FuncIt(InterfaceClass, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
		{
			UFunction* Func = *FuncIt;
			if (!Func || !Func->HasAnyFunctionFlags(FUNC_BlueprintEvent)) continue;

			FName FuncName = Func->GetFName();
			if (!ImplementedGraphNames.Contains(FuncName) && !OverriddenFuncNames.Contains(FuncName))
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("interface"), InterfaceClass->GetName());
				Entry->SetStringField(TEXT("function"), FuncName.ToString());
				UnimplementedFuncs.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
	}

	// --- Duplicate custom events ---
	TArray<TSharedPtr<FJsonValue>> DuplicateEvents;
	TMap<FName, TArray<FString>> EventNameToGraphs;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		TArray<UK2Node_CustomEvent*> CustomEventNodes;
		Graph->GetNodesOfClass<UK2Node_CustomEvent>(CustomEventNodes);
		for (UK2Node_CustomEvent* CE : CustomEventNodes)
		{
			if (!CE || CE->CustomFunctionName.IsNone()) continue;
			EventNameToGraphs.FindOrAdd(CE->CustomFunctionName).Add(Graph->GetName());
		}
	}
	for (auto& Pair : EventNameToGraphs)
	{
		if (Pair.Value.Num() > 1)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("event_name"), Pair.Key.ToString());
			Entry->SetNumberField(TEXT("count"), Pair.Value.Num());
			TArray<TSharedPtr<FJsonValue>> GraphArr;
			for (const FString& GN : Pair.Value)
			{
				GraphArr.Add(MakeShared<FJsonValueString>(GN));
			}
			Entry->SetArrayField(TEXT("graphs"), GraphArr);
			DuplicateEvents.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetArrayField(TEXT("unused_variables"), UnusedVars);
	Root->SetArrayField(TEXT("disconnected_nodes"), DisconnectedNodes);
	Root->SetArrayField(TEXT("node_errors"), NodeErrors);
	Root->SetArrayField(TEXT("unimplemented_interface_functions"), UnimplementedFuncs);
	Root->SetArrayField(TEXT("duplicate_custom_events"), DuplicateEvents);
	Root->SetNumberField(TEXT("total_graphs"), AllGraphs.Num());
	Root->SetNumberField(TEXT("total_nodes"), TotalNodes);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  create_blueprint
// ============================================================

FMonolithActionResult FMonolithBlueprintCompileActions::HandleCreateBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: save_path"));
	}

	FString ParentClassName = Params->GetStringField(TEXT("parent_class"));
	if (ParentClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: parent_class"));
	}

	FString BlueprintTypeStr = Params->GetStringField(TEXT("blueprint_type"));
	if (BlueprintTypeStr.IsEmpty())
	{
		BlueprintTypeStr = TEXT("Normal");
	}

	// Extract asset name from the save path
	int32 LastSlash;
	if (!SavePath.FindLastChar(TEXT('/'), LastSlash))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid save_path — must contain at least one '/': %s"), *SavePath));
	}
	FString AssetName = SavePath.Mid(LastSlash + 1);
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("save_path must not end with '/': %s"), *SavePath));
	}

	// Resolve parent class — try exact, then with 'A' prefix
	UClass* ParentClass = FindFirstObject<UClass>(*ParentClassName, EFindFirstObjectOptions::NativeFirst);
	if (!ParentClass)
	{
		ParentClass = FindFirstObject<UClass>(*(TEXT("A") + ParentClassName), EFindFirstObjectOptions::NativeFirst);
	}
	if (!ParentClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Parent class not found: %s"), *ParentClassName));
	}

	// Parse blueprint type
	EBlueprintType BPType = BPTYPE_Normal;
	if (BlueprintTypeStr == TEXT("Const"))               BPType = BPTYPE_Const;
	else if (BlueprintTypeStr == TEXT("MacroLibrary"))   BPType = BPTYPE_MacroLibrary;
	else if (BlueprintTypeStr == TEXT("Interface"))      BPType = BPTYPE_Interface;
	else if (BlueprintTypeStr == TEXT("FunctionLibrary")) BPType = BPTYPE_FunctionLibrary;

	// Guard: check if a Blueprint already exists at this path.
	// Asset Registry check covers on-disk assets not yet loaded (cold path).
	// FindObject covers in-memory assets from the current session.
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData ExistingAsset = AR.GetAssetByObjectPath(FSoftObjectPath(SavePath + TEXT(".") + AssetName));
	if (ExistingAsset.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Blueprint already exists at '%s'. Use duplicate_blueprint or delete it first."), *SavePath));
	}
	UBlueprint* ExistingBP = FindObject<UBlueprint>(nullptr, *(SavePath + TEXT(".") + AssetName));
	if (!ExistingBP)
	{
		UPackage* ExistingPkg = FindPackage(nullptr, *SavePath);
		if (ExistingPkg)
		{
			ExistingBP = FindObject<UBlueprint>(ExistingPkg, *AssetName);
		}
	}
	if (ExistingBP)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Blueprint already exists at '%s'. Use duplicate_blueprint or delete it first."), *SavePath));
	}

	// Create the package — use the full save path as package name.
	// CreatePackage returns an existing in-memory package if one exists at
	// this path (e.g. leftover from a prior crashed attempt).
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at path: %s"), *SavePath));
	}

	// Ensure the package is fully loaded — if a prior crash left a .uasset
	// on disk, CreatePackage returns a partially-loaded stub.  SavePackage
	// asserts on partial packages.
	Package->FullyLoad();

	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*AssetName),
		BPType,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());

	if (!NewBP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create Blueprint at: %s"), *SavePath));
	}

	// Read skip_save param (default false)
	bool bSkipSave = false;
	if (Params->HasField(TEXT("skip_save")))
	{
		bSkipSave = Params->GetBoolField(TEXT("skip_save"));
	}

	// Compile before save — CreateBlueprint leaves the GeneratedClass in a
	// partially initialized state.  Serialization walks every sub-object
	// through IsValidChecked() which asserts if any inner UObject is null.
	FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::SkipGarbageCollection);

	// Fire edit cradle on CDO properties after compile (#29).
	// Uses FireFullCradle so root property is included in notification chain.
	UObject* CDO = NewBP->GeneratedClass ? NewBP->GeneratedClass->GetDefaultObject() : nullptr;
	if (CDO)
	{
		CDO->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(NSLOCTEXT("MonolithBlueprintCompileActions",
			"CreateBlueprint", "Monolith Create Blueprint"));
		CDO->Modify();
		for (TFieldIterator<FProperty> It(CDO->GetClass()); It; ++It)
		{
			FProperty* Prop = *It;
			if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
				continue;
			MonolithEditCradle::FireFullCradle(CDO, Prop);
		}
	}

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewBP);

	bool bSaved = false;
	if (!bSkipSave)
	{
		// SaveLoadedAsset is the safest path for newly-created assets —
		// it works on the loaded UObject directly and handles the package
		// state correctly.
		bSaved = UEditorAssetLibrary::SaveLoadedAsset(NewBP, false);
	}

	FString GeneratedClassName;
	if (NewBP->GeneratedClass)
	{
		GeneratedClassName = NewBP->GeneratedClass->GetName();
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), SavePath);
	Root->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	Root->SetStringField(TEXT("blueprint_type"), BlueprintTypeStr);
	Root->SetStringField(TEXT("generated_class"), GeneratedClassName);
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  duplicate_blueprint
// ============================================================

FMonolithActionResult FMonolithBlueprintCompileActions::HandleDuplicateBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString NewPath = Params->GetStringField(TEXT("new_path"));
	if (NewPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: new_path"));
	}

	UObject* Duplicated = UEditorAssetLibrary::DuplicateAsset(AssetPath, NewPath);
	bool bSuccess = (Duplicated != nullptr);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("original_path"), AssetPath);
	Root->SetStringField(TEXT("new_asset_path"), NewPath);
	Root->SetBoolField(TEXT("success"), bSuccess);
	if (!bSuccess)
	{
		Root->SetStringField(TEXT("error"), FString::Printf(TEXT("DuplicateAsset failed — check that the destination path is valid and doesn't already exist: %s"), *NewPath));
	}
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  get_dependencies
// ============================================================

FMonolithActionResult FMonolithBlueprintCompileActions::HandleGetDependencies(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString Direction = Params->GetStringField(TEXT("direction"));
	if (Direction.IsEmpty())
	{
		Direction = TEXT("both");
	}

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	FName PackageName = FName(*BP->GetPackage()->GetName());

	TArray<TSharedPtr<FJsonValue>> DependsOnArr;
	if (Direction == TEXT("depends_on") || Direction == TEXT("both"))
	{
		TArray<FAssetIdentifier> Dependencies;
		AR.GetDependencies(FAssetIdentifier(PackageName), Dependencies);
		for (const FAssetIdentifier& Dep : Dependencies)
		{
			DependsOnArr.Add(MakeShared<FJsonValueString>(Dep.ToString()));
		}
	}

	TArray<TSharedPtr<FJsonValue>> ReferencedByArr;
	if (Direction == TEXT("referenced_by") || Direction == TEXT("both"))
	{
		TArray<FAssetIdentifier> Referencers;
		AR.GetReferencers(FAssetIdentifier(PackageName), Referencers);
		for (const FAssetIdentifier& Ref : Referencers)
		{
			ReferencedByArr.Add(MakeShared<FJsonValueString>(Ref.ToString()));
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("direction"), Direction);
	Root->SetArrayField(TEXT("depends_on"), DependsOnArr);
	Root->SetArrayField(TEXT("referenced_by"), ReferencedByArr);
	Root->SetNumberField(TEXT("depends_on_count"), DependsOnArr.Num());
	Root->SetNumberField(TEXT("referenced_by_count"), ReferencedByArr.Num());
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  save_asset
// ============================================================

FMonolithActionResult FMonolithBlueprintCompileActions::HandleSaveAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	bool bWasDirty = Asset->GetOutermost()->IsDirty();
	bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(Asset, false);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("was_dirty"), bWasDirty);

	if (!bSaved)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to save asset: %s"), *AssetPath));
	}
	return FMonolithActionResult::Success(Root);
}
