#include "MonolithAIDiscoveryActions.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "EnvironmentQuery/EnvQuery.h"

#if WITH_STATETREE
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#endif

#if WITH_SMARTOBJECTS
#include "SmartObjectDefinition.h"
#endif

#include "AIController.h"
#include "Perception/AIPerceptionComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_CallFunction.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/UObjectIterator.h"

#include "MonolithIndexSubsystem.h"
#include "MonolithIndexDatabase.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"
#include "Editor.h"

// ============================================================
//  Registration
// ============================================================

void FMonolithAIDiscoveryActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 203. get_ai_overview
	Registry.RegisterAction(TEXT("ai"), TEXT("get_ai_overview"),
		TEXT("Scan project: count BTs, BBs, STs, EQS queries, Smart Objects, AI Controllers"),
		FMonolithActionHandler::CreateStatic(&HandleGetAIOverview),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Only scan assets under this path prefix"))
			.Build());

	// 204. list_ai_node_types
	Registry.RegisterAction(TEXT("ai"), TEXT("list_ai_node_types"),
		TEXT("Unified type discovery for AI systems — enumerate available node classes"),
		FMonolithActionHandler::CreateStatic(&HandleListAINodeTypes),
		FParamSchemaBuilder()
			.Required(TEXT("system"), TEXT("string"), TEXT("System to query: bt, st, or eqs"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Category filter (system-specific, e.g. composite/task/decorator/service for BT)"))
			.Build());

	// 217. search_ai_assets
	Registry.RegisterAction(TEXT("ai"), TEXT("search_ai_assets"),
		TEXT("Full-text search across AI asset names"),
		FMonolithActionHandler::CreateStatic(&HandleSearchAIAssets),
		FParamSchemaBuilder()
			.Required(TEXT("query"), TEXT("string"), TEXT("Search string to match against asset names"))
			.Optional(TEXT("asset_type"), TEXT("string"), TEXT("Filter by type: bt, bb, st, eqs, so, controller"))
			.Build());

	// 205. validate_ai_data_flow
	Registry.RegisterAction(TEXT("ai"), TEXT("validate_ai_data_flow"),
		TEXT("Trace full data flow for an AI Controller: BB keys vs BT refs, EQS->BB, Perception config"),
		FMonolithActionHandler::CreateStatic(&HandleValidateAIDataFlow),
		FParamSchemaBuilder()
			.Required(TEXT("controller_path"), TEXT("string"), TEXT("AI Controller Blueprint asset path"))
			.Build());

	// 214. find_eqs_references
	Registry.RegisterAction(TEXT("ai"), TEXT("find_eqs_references"),
		TEXT("Find which BTs, STs, and Blueprints reference a given EQS query"),
		FMonolithActionHandler::CreateStatic(&HandleFindEQSReferences),
		FParamSchemaBuilder()
			.Required(TEXT("eqs_path"), TEXT("string"), TEXT("EQS query asset path"))
			.Build());

	// 215. find_so_references
	Registry.RegisterAction(TEXT("ai"), TEXT("find_so_references"),
		TEXT("Find which BTs, STs, and levels reference a given Smart Object definition"),
		FMonolithActionHandler::CreateStatic(&HandleFindSOReferences),
		FParamSchemaBuilder()
			.Required(TEXT("so_path"), TEXT("string"), TEXT("Smart Object definition asset path"))
			.Build());

	// 207. lint_behavior_tree
	Registry.RegisterAction(TEXT("ai"), TEXT("lint_behavior_tree"),
		TEXT("Style lint a Behavior Tree: unreachable branches, redundant decorators, single-child composites, unnamed nodes"),
		FMonolithActionHandler::CreateStatic(&HandleLintBehaviorTree),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Build());

	// 208. lint_state_tree
	Registry.RegisterAction(TEXT("ai"), TEXT("lint_state_tree"),
		TEXT("Style lint a State Tree: no-task states, self-transitions without delay, dead-end states"),
		FMonolithActionHandler::CreateStatic(&HandleLintStateTree),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("State Tree asset path"))
			.Build());

	// 209. detect_ai_circular_references
	Registry.RegisterAction(TEXT("ai"), TEXT("detect_ai_circular_references"),
		TEXT("Check BT RunBehavior chains, ST linked assets, BB parent chains for circular references"),
		FMonolithActionHandler::CreateStatic(&HandleDetectAICircularReferences),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Only scan assets under this path prefix"))
			.Build());

	// 210. export_ai_manifest
	Registry.RegisterAction(TEXT("ai"), TEXT("export_ai_manifest"),
		TEXT("Full project AI manifest with cross-references — JSON or Markdown table"),
		FMonolithActionHandler::CreateStatic(&HandleExportAIManifest),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Only include assets under this path prefix"))
			.Optional(TEXT("format"), TEXT("string"), TEXT("Output format: json (default) or markdown"))
			.Build());

	// 211. get_ai_behavior_summary
	Registry.RegisterAction(TEXT("ai"), TEXT("get_ai_behavior_summary"),
		TEXT("Structured JSON summary of a BT or ST: flow paths, decision points, key BB dependencies"),
		FMonolithActionHandler::CreateStatic(&HandleGetAIBehaviorSummary),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree or State Tree asset path"))
			.Build());
}

// ============================================================
//  Helpers
// ============================================================

namespace
{
	int32 CountAssetsByClass(IAssetRegistry& AR, const FTopLevelAssetPath& ClassPath, const FString& PathFilter)
	{
		TArray<FAssetData> Assets;
		AR.GetAssetsByClass(ClassPath, Assets);

		if (PathFilter.IsEmpty())
		{
			return Assets.Num();
		}

		int32 Count = 0;
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.GetObjectPathString().StartsWith(PathFilter))
			{
				Count++;
			}
		}
		return Count;
	}

	int32 CountAIControllerBlueprints(IAssetRegistry& AR, const FString& PathFilter)
	{
		TArray<FAssetData> Assets;
		AR.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Assets);

		int32 Count = 0;
		for (const FAssetData& Asset : Assets)
		{
			FAssetTagValueRef ParentClassTag = Asset.TagsAndValues.FindTag(FName(TEXT("ParentClass")));
			if (!ParentClassTag.IsSet()) continue;

			FString ParentClassPath = ParentClassTag.GetValue();
			if (!ParentClassPath.Contains(TEXT("AIController"))) continue;

			if (!PathFilter.IsEmpty() && !Asset.GetObjectPathString().StartsWith(PathFilter))
			{
				continue;
			}

			Count++;
		}
		return Count;
	}

	/** Collect BT node classes of a given base type for discovery */
	void CollectBTNodeClasses(UClass* BaseClass, const FString& Category, TArray<TSharedPtr<FJsonValue>>& OutArr)
	{
		TArray<UClass*> DerivedClasses;
		GetDerivedClasses(BaseClass, DerivedClasses, /*bRecursive=*/true);

		for (UClass* Cls : DerivedClasses)
		{
			if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}
			if (Cls->IsChildOf(UEdGraphNode::StaticClass()))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("class_name"), Cls->GetName());
			Entry->SetStringField(TEXT("display_name"), Cls->GetDisplayNameText().ToString());
			Entry->SetStringField(TEXT("category"), Category);

			if (UBTNode* CDO = Cast<UBTNode>(Cls->GetDefaultObject()))
			{
				FString NodeName = CDO->GetNodeName();
				if (!NodeName.IsEmpty())
				{
					Entry->SetStringField(TEXT("node_name"), NodeName);
				}
			}

			OutArr.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	/** Search assets of a given class for matching names */
	void SearchAssetsOfClass(IAssetRegistry& AR, const FTopLevelAssetPath& ClassPath, const FString& Query, const FString& TypeLabel, TArray<TSharedPtr<FJsonValue>>& OutArr)
	{
		TArray<FAssetData> Assets;
		AR.GetAssetsByClass(ClassPath, Assets);

		FString QueryLower = Query.ToLower();

		for (const FAssetData& Asset : Assets)
		{
			FString Name = Asset.AssetName.ToString();
			if (Name.ToLower().Contains(QueryLower))
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("asset_path"), Asset.PackageName.ToString());
				Item->SetStringField(TEXT("name"), Name);
				Item->SetStringField(TEXT("type"), TypeLabel);
				OutArr.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
	}
}

// ============================================================
//  203. get_ai_overview
// ============================================================

FMonolithActionResult FMonolithAIDiscoveryActions::HandleGetAIOverview(const TSharedPtr<FJsonObject>& Params)
{
	FString PathFilter = Params->GetStringField(TEXT("path_filter"));

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Behavior Trees
	Result->SetNumberField(TEXT("behavior_trees"),
		CountAssetsByClass(AR, UBehaviorTree::StaticClass()->GetClassPathName(), PathFilter));

	// Blackboards
	Result->SetNumberField(TEXT("blackboards"),
		CountAssetsByClass(AR, UBlackboardData::StaticClass()->GetClassPathName(), PathFilter));

	// EQS Queries
	Result->SetNumberField(TEXT("eqs_queries"),
		CountAssetsByClass(AR, UEnvQuery::StaticClass()->GetClassPathName(), PathFilter));

	// State Trees
#if WITH_STATETREE
	Result->SetNumberField(TEXT("state_trees"),
		CountAssetsByClass(AR, UStateTree::StaticClass()->GetClassPathName(), PathFilter));
#else
	Result->SetNumberField(TEXT("state_trees"), 0);
#endif

	// Smart Objects
#if WITH_SMARTOBJECTS
	Result->SetNumberField(TEXT("smart_object_definitions"),
		CountAssetsByClass(AR, USmartObjectDefinition::StaticClass()->GetClassPathName(), PathFilter));
#else
	Result->SetNumberField(TEXT("smart_object_definitions"), 0);
#endif

	// AI Controllers (Blueprint-based)
	Result->SetNumberField(TEXT("ai_controllers"),
		CountAIControllerBlueprints(AR, PathFilter));

	if (!PathFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("path_filter"), PathFilter);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  204. list_ai_node_types
// ============================================================

FMonolithActionResult FMonolithAIDiscoveryActions::HandleListAINodeTypes(const TSharedPtr<FJsonObject>& Params)
{
	FString System;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("system"), System, ErrResult))
	{
		return ErrResult;
	}

	System = System.ToLower();
	FString CategoryFilter = Params->GetStringField(TEXT("category")).ToLower();

	TArray<TSharedPtr<FJsonValue>> NodeTypes;

	if (System == TEXT("bt"))
	{
		if (CategoryFilter.IsEmpty() || CategoryFilter == TEXT("composite"))
		{
			CollectBTNodeClasses(UBTCompositeNode::StaticClass(), TEXT("composite"), NodeTypes);
		}
		if (CategoryFilter.IsEmpty() || CategoryFilter == TEXT("task"))
		{
			CollectBTNodeClasses(UBTTaskNode::StaticClass(), TEXT("task"), NodeTypes);
		}
		if (CategoryFilter.IsEmpty() || CategoryFilter == TEXT("decorator"))
		{
			CollectBTNodeClasses(UBTDecorator::StaticClass(), TEXT("decorator"), NodeTypes);
		}
		if (CategoryFilter.IsEmpty() || CategoryFilter == TEXT("service"))
		{
			CollectBTNodeClasses(UBTService::StaticClass(), TEXT("service"), NodeTypes);
		}
	}
	else if (System == TEXT("st"))
	{
		// State Tree node discovery — stub for now
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("node_types"), NodeTypes);
		Result->SetNumberField(TEXT("count"), 0);
		Result->SetStringField(TEXT("note"), TEXT("State Tree node type enumeration not yet implemented"));
		return FMonolithActionResult::Success(Result);
	}
	else if (System == TEXT("eqs"))
	{
		// EQS node discovery — stub for now
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("node_types"), NodeTypes);
		Result->SetNumberField(TEXT("count"), 0);
		Result->SetStringField(TEXT("note"), TEXT("EQS node type enumeration not yet implemented"));
		return FMonolithActionResult::Success(Result);
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown system '%s'. Valid values: bt, st, eqs"), *System));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System);
	Result->SetArrayField(TEXT("node_types"), NodeTypes);
	Result->SetNumberField(TEXT("count"), NodeTypes.Num());
	if (!CategoryFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("category_filter"), CategoryFilter);
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  217. search_ai_assets
// ============================================================

FMonolithActionResult FMonolithAIDiscoveryActions::HandleSearchAIAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("query"), Query, ErrResult))
	{
		return ErrResult;
	}

	FString AssetTypeFilter = Params->GetStringField(TEXT("asset_type")).ToLower();

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<TSharedPtr<FJsonValue>> Matches;

	// Behavior Trees
	if (AssetTypeFilter.IsEmpty() || AssetTypeFilter == TEXT("bt"))
	{
		SearchAssetsOfClass(AR, UBehaviorTree::StaticClass()->GetClassPathName(), Query, TEXT("BehaviorTree"), Matches);
	}

	// Blackboards
	if (AssetTypeFilter.IsEmpty() || AssetTypeFilter == TEXT("bb"))
	{
		SearchAssetsOfClass(AR, UBlackboardData::StaticClass()->GetClassPathName(), Query, TEXT("Blackboard"), Matches);
	}

	// EQS Queries
	if (AssetTypeFilter.IsEmpty() || AssetTypeFilter == TEXT("eqs"))
	{
		SearchAssetsOfClass(AR, UEnvQuery::StaticClass()->GetClassPathName(), Query, TEXT("EQS"), Matches);
	}

	// State Trees
#if WITH_STATETREE
	if (AssetTypeFilter.IsEmpty() || AssetTypeFilter == TEXT("st"))
	{
		SearchAssetsOfClass(AR, UStateTree::StaticClass()->GetClassPathName(), Query, TEXT("StateTree"), Matches);
	}
#endif

	// Smart Objects
#if WITH_SMARTOBJECTS
	if (AssetTypeFilter.IsEmpty() || AssetTypeFilter == TEXT("so"))
	{
		SearchAssetsOfClass(AR, USmartObjectDefinition::StaticClass()->GetClassPathName(), Query, TEXT("SmartObject"), Matches);
	}
#endif

	// AI Controllers (search Blueprints with AIController parent)
	if (AssetTypeFilter.IsEmpty() || AssetTypeFilter == TEXT("controller"))
	{
		FString QueryLower = Query.ToLower();
		TArray<FAssetData> BPAssets;
		AR.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), BPAssets);

		for (const FAssetData& Asset : BPAssets)
		{
			FAssetTagValueRef ParentClassTag = Asset.TagsAndValues.FindTag(FName(TEXT("ParentClass")));
			if (!ParentClassTag.IsSet()) continue;

			FString ParentClassPath = ParentClassTag.GetValue();
			if (!ParentClassPath.Contains(TEXT("AIController"))) continue;

			FString Name = Asset.AssetName.ToString();
			if (Name.ToLower().Contains(QueryLower))
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("asset_path"), Asset.PackageName.ToString());
				Item->SetStringField(TEXT("name"), Name);
				Item->SetStringField(TEXT("type"), TEXT("AIController"));
				Matches.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("query"), Query);
	Result->SetArrayField(TEXT("results"), Matches);
	Result->SetNumberField(TEXT("count"), Matches.Num());
	if (!AssetTypeFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("asset_type_filter"), AssetTypeFilter);
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  205. validate_ai_data_flow
// ============================================================

namespace
{
	/** Query ai_cross_refs for assets referencing a given target path. */
	void QueryCrossRefsForTarget(FSQLiteDatabase* RawDB, const FString& TargetPath, TArray<TSharedPtr<FJsonValue>>& OutRefs, bool& bOutUsedIndex)
	{
		if (!RawDB) return;

		FString EscapedPath = TargetPath.Replace(TEXT("'"), TEXT("''"));
		FString SQL = FString::Printf(
			TEXT("SELECT a.path, a.type, r.ref_type FROM ai_cross_refs r "
				 "JOIN ai_assets a ON a.id = r.source_id "
				 "JOIN ai_assets t ON t.id = r.target_id "
				 "WHERE t.path = '%s'"),
			*EscapedPath);

		FSQLitePreparedStatement Stmt = RawDB->PrepareStatement(*SQL);
		if (Stmt.IsValid())
		{
			bOutUsedIndex = true;
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString AssetPath, AssetType, RefType;
				Stmt.GetColumnValueByIndex(0, AssetPath);
				Stmt.GetColumnValueByIndex(1, AssetType);
				Stmt.GetColumnValueByIndex(2, RefType);

				TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
				Ref->SetStringField(TEXT("asset_path"), AssetPath);
				Ref->SetStringField(TEXT("asset_type"), AssetType);
				Ref->SetStringField(TEXT("ref_type"), RefType);
				Ref->SetStringField(TEXT("source"), TEXT("index"));
				OutRefs.Add(MakeShared<FJsonValueObject>(Ref));
			}
		}
	}

	/** Supplement references from the AssetRegistry referencers API. */
	void SupplementWithAssetRegistryRefs(const FString& AssetPath, TArray<TSharedPtr<FJsonValue>>& InOutRefs)
	{
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

		TArray<FName> Referencers;
		AR.GetReferencers(FName(*AssetPath), Referencers);

		TSet<FString> AlreadyFound;
		for (const auto& Ref : InOutRefs)
		{
			TSharedPtr<FJsonObject> RefObj = Ref->AsObject();
			if (RefObj.IsValid())
			{
				AlreadyFound.Add(RefObj->GetStringField(TEXT("asset_path")));
			}
		}

		for (const FName& RefName : Referencers)
		{
			FString RefPath = RefName.ToString();
			if (RefPath.IsEmpty() || AlreadyFound.Contains(RefPath)) continue;

			TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
			RefObj->SetStringField(TEXT("asset_path"), RefPath);

			TArray<FAssetData> AssetDataArr;
			AR.GetAssetsByPackageName(RefName, AssetDataArr);
			if (AssetDataArr.Num() > 0)
			{
				RefObj->SetStringField(TEXT("asset_type"), AssetDataArr[0].AssetClassPath.GetAssetName().ToString());
			}
			RefObj->SetStringField(TEXT("source"), TEXT("asset_registry"));
			InOutRefs.Add(MakeShared<FJsonValueObject>(RefObj));
		}
	}

	/** Recursively collect BB key references from BT nodes. */
	void CollectBTKeyRefs(const UBTCompositeNode* Node, TSet<FName>& OutKeyRefs, TArray<FString>& OutEQSRefs)
	{
		if (!Node) return;

		auto ScanObjectForKeyRefs = [&OutKeyRefs, &OutEQSRefs](const UObject* Obj)
		{
			if (!Obj) return;
			for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
			{
				if (FStructProperty* StructProp = CastField<FStructProperty>(*It))
				{
					if (StructProp->Struct->GetFName() == FName(TEXT("BlackboardKeySelector")))
					{
						const void* PropAddr = (*It)->ContainerPtrToValuePtr<void>(Obj);
						const FBlackboardKeySelector* Selector = reinterpret_cast<const FBlackboardKeySelector*>(PropAddr);
						if (!Selector->SelectedKeyName.IsNone())
						{
							OutKeyRefs.Add(Selector->SelectedKeyName);
						}
					}
				}
				if (FObjectProperty* ObjProp = CastField<FObjectProperty>(*It))
				{
					if (ObjProp->PropertyClass && ObjProp->PropertyClass->IsChildOf(UEnvQuery::StaticClass()))
					{
						const void* PropAddr = (*It)->ContainerPtrToValuePtr<void>(Obj);
						UObject* RefObj = ObjProp->GetPropertyValue(PropAddr);
						if (RefObj)
						{
							OutEQSRefs.Add(RefObj->GetPathName());
						}
					}
				}
			}
		};

		// Scan services on composite
		for (const UBTService* Svc : Node->Services) { ScanObjectForKeyRefs(Svc); }

		for (int32 i = 0; i < Node->Children.Num(); ++i)
		{
			const FBTCompositeChild& Child = Node->Children[i];
			ScanObjectForKeyRefs(Child.ChildTask);
			for (const UBTDecorator* Dec : Child.Decorators) { ScanObjectForKeyRefs(Dec); }

			if (Child.ChildComposite)
			{
				CollectBTKeyRefs(Child.ChildComposite, OutKeyRefs, OutEQSRefs);
			}
		}
	}
}

FMonolithActionResult FMonolithAIDiscoveryActions::HandleValidateAIDataFlow(const TSharedPtr<FJsonObject>& Params)
{
	FString ControllerPath = Params->GetStringField(TEXT("controller_path"));
	if (ControllerPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: controller_path"));
	}

	UBlueprint* BP = Cast<UBlueprint>(MonolithAI::ResolveAsset(UBlueprint::StaticClass(), ControllerPath));
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found at '%s'"), *ControllerPath));
	}

	UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
	AAIController* AIC = BPGC ? Cast<AAIController>(BPGC->GetDefaultObject()) : nullptr;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("controller_path"), ControllerPath);

	TArray<FString> Issues;
	TArray<FString> DataFlowItems;

	// 1. Find BehaviorTree — three-tier detector.
	//    Vanilla AAIController has NO UBehaviorTree* UPROPERTY on the CDO; BTs are
	//    started via RunBehaviorTree() at runtime. The original validator only ran tier 1
	//    and produced a false-negative on every stock controller (#52).
	//    Tier 1: standard UPROPERTY check (custom subclasses that own a BT* member).
	//    Tier 2: scan event-graph K2Node_CallFunction nodes for RunBehaviorTree, read the
	//            BT pin's default-value literal.
	//    Tier 3: soft-info fallback — emit bt_assigned=false (informational, not an issue).
	UBehaviorTree* BT = nullptr;
	FString BTSource = TEXT("none");
	if (AIC)
	{
		// --- Tier 1: UPROPERTY scan on the BPGC ---
		for (TFieldIterator<FObjectProperty> It(BPGC); It; ++It)
		{
			if (It->PropertyClass && It->PropertyClass->IsChildOf(UBehaviorTree::StaticClass()))
			{
				void* PropAddr = It->ContainerPtrToValuePtr<void>(AIC);
				BT = Cast<UBehaviorTree>(It->GetPropertyValue(PropAddr));
				if (BT)
				{
					BTSource = TEXT("uproperty");
					DataFlowItems.Add(FString::Printf(TEXT("Controller -> BT (UPROPERTY): %s"), *BT->GetPathName()));
					break;
				}
			}
		}

		// --- Tier 2: Event-graph scan for RunBehaviorTree call nodes ---
		if (!BT)
		{
			static const FName RunBehaviorTreeName(TEXT("RunBehaviorTree"));

			auto ScanGraphForRunBT = [&BT](UEdGraph* Graph) -> UEdGraph*
			{
				if (!Graph)
				{
					return nullptr;
				}
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
					if (!CallNode)
					{
						continue;
					}
					if (CallNode->FunctionReference.GetMemberName() != RunBehaviorTreeName)
					{
						continue;
					}
					// Read the BT input pin literal. RunBehaviorTree(BehaviorTreeAsset) — the
					// BT pin is the only UBehaviorTree* input on the call node.
					for (UEdGraphPin* Pin : CallNode->Pins)
					{
						if (!Pin || Pin->Direction != EGPD_Input)
						{
							continue;
						}
						// Skip the implicit 'self' input pin (target object) — never carries a BT literal.
						if (Pin->PinName == TEXT("self"))
						{
							continue;
						}
						UObject* Obj = Pin->DefaultObject;
						if (UBehaviorTree* PinBT = Cast<UBehaviorTree>(Obj))
						{
							BT = PinBT;
							return Graph;
						}
					}
					// Pin literal not set (likely a variable wired into the BT pin); we still
					// know SOMETHING calls RunBehaviorTree even if we can't resolve the asset.
					return Graph;
				}
				return nullptr;
			};

			UEdGraph* FoundGraph = nullptr;
			for (UEdGraph* UberGraph : BP->UbergraphPages)
			{
				FoundGraph = ScanGraphForRunBT(UberGraph);
				if (FoundGraph)
				{
					break;
				}
			}
			if (!FoundGraph)
			{
				for (UEdGraph* FuncGraph : BP->FunctionGraphs)
				{
					FoundGraph = ScanGraphForRunBT(FuncGraph);
					if (FoundGraph)
					{
						break;
					}
				}
			}

			if (FoundGraph)
			{
				BTSource = TEXT("event_graph");
				if (BT)
				{
					DataFlowItems.Add(FString::Printf(
						TEXT("Controller -> BT (RunBehaviorTree node in '%s'): %s"),
						*FoundGraph->GetName(), *BT->GetPathName()));
				}
				else
				{
					DataFlowItems.Add(FString::Printf(
						TEXT("Controller -> BT (RunBehaviorTree call in '%s'): asset wired via variable, not a literal"),
						*FoundGraph->GetName()));
				}
			}
		}

		// --- Tier 3 reporting: surface assignment status as soft info, not an error. ---
		Result->SetBoolField(TEXT("bt_assigned"), BT != nullptr || BTSource == TEXT("event_graph"));
		Result->SetStringField(TEXT("bt_source"), BTSource);

		// Check perception
		bool bHasPerception = (AIC->PerceptionComponent != nullptr);
		if (!bHasPerception)
		{
			for (TFieldIterator<FObjectProperty> It(BPGC); It; ++It)
			{
				if (It->PropertyClass && It->PropertyClass->IsChildOf(UAIPerceptionComponent::StaticClass()))
				{
					bHasPerception = true;
					break;
				}
			}
		}
		Result->SetBoolField(TEXT("has_perception"), bHasPerception);
	}

	// 2. Analyze BT
	if (BT)
	{
		Result->SetStringField(TEXT("behavior_tree"), BT->GetPathName());

		UBlackboardData* BB = BT->BlackboardAsset;
		if (BB)
		{
			Result->SetStringField(TEXT("blackboard"), BB->GetPathName());
			DataFlowItems.Add(FString::Printf(TEXT("BT -> BB: %s"), *BB->GetPathName()));

			// Collect BB defined keys (including parent BBs)
			TSet<FName> DefinedKeys;
			for (const UBlackboardData* CurrentBB = BB; CurrentBB; CurrentBB = CurrentBB->Parent)
			{
				for (const FBlackboardEntry& Entry : CurrentBB->Keys)
				{
					DefinedKeys.Add(Entry.EntryName);
				}
			}

			// Collect BT referenced keys and EQS refs
			TSet<FName> ReferencedKeys;
			TArray<FString> EQSRefs;
			if (BT->RootNode)
			{
				CollectBTKeyRefs(BT->RootNode, ReferencedKeys, EQSRefs);
			}

			// Missing keys
			for (const FName& RefKey : ReferencedKeys)
			{
				if (!DefinedKeys.Contains(RefKey))
				{
					Issues.Add(FString::Printf(TEXT("BT references BB key '%s' not defined in blackboard"), *RefKey.ToString()));
				}
			}

			// Unused keys
			TArray<TSharedPtr<FJsonValue>> UnusedArr;
			for (const FName& DefKey : DefinedKeys)
			{
				if (!ReferencedKeys.Contains(DefKey))
				{
					UnusedArr.Add(MakeShared<FJsonValueString>(DefKey.ToString()));
				}
			}
			if (UnusedArr.Num() > 0)
			{
				Result->SetArrayField(TEXT("unused_bb_keys"), UnusedArr);
			}

			// EQS refs
			if (EQSRefs.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> EQSArr;
				for (const FString& E : EQSRefs)
				{
					EQSArr.Add(MakeShared<FJsonValueString>(E));
					DataFlowItems.Add(FString::Printf(TEXT("BT -> EQS: %s"), *E));
				}
				Result->SetArrayField(TEXT("eqs_references"), EQSArr);
			}

			Result->SetNumberField(TEXT("bb_keys_defined"), DefinedKeys.Num());
			Result->SetNumberField(TEXT("bb_keys_referenced"), ReferencedKeys.Num());
		}
		else
		{
			Issues.Add(TEXT("BehaviorTree has no Blackboard asset assigned"));
		}
	}
	else
	{
		// Tier 3 fallback: no BT discoverable on the CDO and no RunBehaviorTree call in any
		// graph. Per #52 design, this is soft info — the controller may assign a BT at
		// runtime via external code. bt_assigned was already set above.
		DataFlowItems.Add(TEXT("No BehaviorTree assignment detected (UPROPERTY or RunBehaviorTree node) — assigned at runtime by external code"));
	}

	TArray<TSharedPtr<FJsonValue>> IssueArr;
	for (const FString& I : Issues) IssueArr.Add(MakeShared<FJsonValueString>(I));
	Result->SetArrayField(TEXT("issues"), IssueArr);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());

	TArray<TSharedPtr<FJsonValue>> FlowArr;
	for (const FString& F : DataFlowItems) FlowArr.Add(MakeShared<FJsonValueString>(F));
	Result->SetArrayField(TEXT("data_flow"), FlowArr);

	Result->SetStringField(TEXT("message"), Issues.Num() == 0
		? TEXT("Data flow validation passed")
		: FString::Printf(TEXT("Found %d issue(s)"), Issues.Num()));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  214. find_eqs_references
// ============================================================

FMonolithActionResult FMonolithAIDiscoveryActions::HandleFindEQSReferences(const TSharedPtr<FJsonObject>& Params)
{
	FString EQSPath = Params->GetStringField(TEXT("eqs_path"));
	if (EQSPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: eqs_path"));
	}

	TArray<TSharedPtr<FJsonValue>> References;
	bool bUsedIndex = false;

	// Strategy 1: AI index database
	if (GEditor)
	{
		UMonolithIndexSubsystem* IndexSub = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
		if (IndexSub)
		{
			FMonolithIndexDatabase* DB = IndexSub->GetDatabase();
			if (DB)
			{
				QueryCrossRefsForTarget(DB->GetRawDatabase(), EQSPath, References, bUsedIndex);
			}
		}
	}

	// Strategy 2: AssetRegistry referencers
	SupplementWithAssetRegistryRefs(EQSPath, References);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("eqs_path"), EQSPath);
	Result->SetArrayField(TEXT("references"), References);
	Result->SetNumberField(TEXT("count"), References.Num());
	Result->SetBoolField(TEXT("used_deep_index"), bUsedIndex);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  215. find_so_references
// ============================================================

FMonolithActionResult FMonolithAIDiscoveryActions::HandleFindSOReferences(const TSharedPtr<FJsonObject>& Params)
{
	FString SOPath = Params->GetStringField(TEXT("so_path"));
	if (SOPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: so_path"));
	}

	TArray<TSharedPtr<FJsonValue>> References;
	bool bUsedIndex = false;

	// Strategy 1: AI index database
	if (GEditor)
	{
		UMonolithIndexSubsystem* IndexSub = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
		if (IndexSub)
		{
			FMonolithIndexDatabase* DB = IndexSub->GetDatabase();
			if (DB)
			{
				QueryCrossRefsForTarget(DB->GetRawDatabase(), SOPath, References, bUsedIndex);
			}
		}
	}

	// Strategy 2: AssetRegistry referencers
	SupplementWithAssetRegistryRefs(SOPath, References);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("so_path"), SOPath);
	Result->SetArrayField(TEXT("references"), References);
	Result->SetNumberField(TEXT("count"), References.Num());
	Result->SetBoolField(TEXT("used_deep_index"), bUsedIndex);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  207. lint_behavior_tree
// ============================================================

namespace
{
	/** Recursively lint a BT composite node. */
	void LintBTNode(const UBTCompositeNode* Node, int32 Depth, TArray<TSharedPtr<FJsonValue>>& OutIssues, int32& OutScore)
	{
		if (!Node) return;

		FString NodeId = FString::Printf(TEXT("%s [depth %d]"), *Node->GetNodeName(), Depth);

		// Single-child composite is suspicious
		if (Node->Children.Num() == 1)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("warning"));
			Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("Composite '%s' has only 1 child — consider simplifying"), *NodeId));
			Issue->SetStringField(TEXT("node_id"), NodeId);
			OutIssues.Add(MakeShared<FJsonValueObject>(Issue));
			OutScore -= 5;
		}

		// Empty composite (zero children)
		if (Node->Children.Num() == 0)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("error"));
			Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("Composite '%s' has no children — dead branch"), *NodeId));
			Issue->SetStringField(TEXT("node_id"), NodeId);
			OutIssues.Add(MakeShared<FJsonValueObject>(Issue));
			OutScore -= 15;
		}

		for (int32 i = 0; i < Node->Children.Num(); ++i)
		{
			const FBTCompositeChild& Child = Node->Children[i];

			// Check for unnamed task nodes
			if (Child.ChildTask)
			{
				FString TaskName = Child.ChildTask->GetNodeName();
				if (TaskName.IsEmpty() || TaskName == TEXT("None"))
				{
					TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("severity"), TEXT("info"));
					Issue->SetStringField(TEXT("message"), FString::Printf(
						TEXT("Task at index %d under '%s' has no custom name"), i, *NodeId));
					Issue->SetStringField(TEXT("node_id"), FString::Printf(TEXT("%s/child[%d]"), *NodeId, i));
					OutIssues.Add(MakeShared<FJsonValueObject>(Issue));
					OutScore -= 2;
				}
			}

			// Check for redundant decorators (multiple of same type on same child)
			TMap<FName, int32> DecoratorCounts;
			for (const UBTDecorator* Dec : Child.Decorators)
			{
				if (Dec)
				{
					FName DecClass = Dec->GetClass()->GetFName();
					int32& Count = DecoratorCounts.FindOrAdd(DecClass);
					Count++;
					if (Count > 1)
					{
						TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
						Issue->SetStringField(TEXT("severity"), TEXT("warning"));
						Issue->SetStringField(TEXT("message"), FString::Printf(
							TEXT("Duplicate decorator '%s' on child %d of '%s'"), *DecClass.ToString(), i, *NodeId));
						Issue->SetStringField(TEXT("node_id"), FString::Printf(TEXT("%s/child[%d]"), *NodeId, i));
						OutIssues.Add(MakeShared<FJsonValueObject>(Issue));
						OutScore -= 5;
					}
				}
			}

			// Recurse into child composites
			if (Child.ChildComposite)
			{
				LintBTNode(Child.ChildComposite, Depth + 1, OutIssues, OutScore);
			}
		}
	}
}

FMonolithActionResult FMonolithAIDiscoveryActions::HandleLintBehaviorTree(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = MonolithAI::LoadBehaviorTreeFromParams(Params, AssetPath, Error);
	if (!BT) return FMonolithActionResult::Error(Error);

	TArray<TSharedPtr<FJsonValue>> Issues;
	int32 Score = 100;

	// Check blackboard assignment
	if (!BT->BlackboardAsset)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), TEXT("warning"));
		Issue->SetStringField(TEXT("message"), TEXT("BehaviorTree has no Blackboard asset assigned"));
		Issue->SetStringField(TEXT("node_id"), TEXT("root"));
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
		Score -= 10;
	}

	// Lint the tree structure
	if (BT->RootNode)
	{
		LintBTNode(BT->RootNode, 0, Issues, Score);
	}
	else
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), TEXT("error"));
		Issue->SetStringField(TEXT("message"), TEXT("BehaviorTree has no root node"));
		Issue->SetStringField(TEXT("node_id"), TEXT("root"));
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
		Score -= 30;
	}

	Score = FMath::Max(0, Score);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());
	Result->SetNumberField(TEXT("score"), Score);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  208. lint_state_tree
// ============================================================

#if WITH_STATETREE
namespace
{
	void LintSTState(const UStateTreeState* State, TArray<TSharedPtr<FJsonValue>>& OutIssues, int32& OutScore)
	{
		if (!State) return;

		FString StateId = FString::Printf(TEXT("%s [%s]"), *State->Name.ToString(), *State->ID.ToString());

		// No-task state (leaf state with no tasks)
		if (State->Children.Num() == 0 && State->Tasks.Num() == 0 &&
			State->Type == EStateTreeStateType::State)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("warning"));
			Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("State '%s' has no tasks and no children — does nothing"), *State->Name.ToString()));
			Issue->SetStringField(TEXT("node_id"), StateId);
			OutIssues.Add(MakeShared<FJsonValueObject>(Issue));
			OutScore -= 5;
		}

		// Dead-end state: leaf with no transitions
		if (State->Children.Num() == 0 && State->Transitions.Num() == 0 &&
			State->Type == EStateTreeStateType::State)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("warning"));
			Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("State '%s' has no transitions — dead-end state"), *State->Name.ToString()));
			Issue->SetStringField(TEXT("node_id"), StateId);
			OutIssues.Add(MakeShared<FJsonValueObject>(Issue));
			OutScore -= 10;
		}

		// Self-transition without delay
		for (int32 i = 0; i < State->Transitions.Num(); ++i)
		{
			const FStateTreeTransition& Trans = State->Transitions[i];
#if WITH_EDITORONLY_DATA
			if (Trans.State.ID == State->ID && !Trans.bDelayTransition)
			{
				TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("severity"), TEXT("error"));
				Issue->SetStringField(TEXT("message"), FString::Printf(
					TEXT("Transition %d on '%s' targets itself with no delay — infinite loop risk"),
					i, *State->Name.ToString()));
				Issue->SetStringField(TEXT("node_id"), StateId);
				OutIssues.Add(MakeShared<FJsonValueObject>(Issue));
				OutScore -= 15;
			}
#endif
		}

		// Disabled state
		if (!State->bEnabled)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("info"));
			Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("State '%s' is disabled"), *State->Name.ToString()));
			Issue->SetStringField(TEXT("node_id"), StateId);
			OutIssues.Add(MakeShared<FJsonValueObject>(Issue));
		}

		// Recurse
		for (const UStateTreeState* Child : State->Children)
		{
			LintSTState(Child, OutIssues, OutScore);
		}
	}
}
#endif // WITH_STATETREE

FMonolithActionResult FMonolithAIDiscoveryActions::HandleLintStateTree(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_STATETREE
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));
	}
	AssetPath = FMonolithAssetUtils::ResolveAssetPath(AssetPath);

	UStateTree* ST = FMonolithAssetUtils::LoadAssetByPath<UStateTree>(AssetPath);
	if (!ST)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("StateTree not found at '%s'"), *AssetPath));
	}

#if WITH_EDITORONLY_DATA
	UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(ST->EditorData);
	if (!EditorData)
	{
		return FMonolithActionResult::Error(TEXT("StateTree has no editor data"));
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	int32 Score = 100;

	if (EditorData->SubTrees.Num() == 0)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), TEXT("error"));
		Issue->SetStringField(TEXT("message"), TEXT("StateTree has no subtrees/states"));
		Issue->SetStringField(TEXT("node_id"), TEXT("root"));
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
		Score -= 30;
	}

	for (const UStateTreeState* SubTree : EditorData->SubTrees)
	{
		LintSTState(SubTree, Issues, Score);
	}

	Score = FMath::Max(0, Score);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());
	Result->SetNumberField(TEXT("score"), Score);
	return FMonolithActionResult::Success(Result);
#else
	return FMonolithActionResult::Error(TEXT("Editor data not available"));
#endif

#else
	return FMonolithActionResult::Error(TEXT("StateTree module not available (WITH_STATETREE=0)"));
#endif
}

// ============================================================
//  209. detect_ai_circular_references
// ============================================================

FMonolithActionResult FMonolithAIDiscoveryActions::HandleDetectAICircularReferences(const TSharedPtr<FJsonObject>& Params)
{
	FString PathFilter = Params->GetStringField(TEXT("path_filter"));

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<TSharedPtr<FJsonValue>> Cycles;

	// --- Check BB parent chains for cycles ---
	{
		TArray<FAssetData> BBAssets;
		AR.GetAssetsByClass(UBlackboardData::StaticClass()->GetClassPathName(), BBAssets);

		for (const FAssetData& Asset : BBAssets)
		{
			if (!PathFilter.IsEmpty() && !Asset.GetObjectPathString().StartsWith(PathFilter)) continue;

			UBlackboardData* BB = Cast<UBlackboardData>(Asset.GetAsset());
			if (!BB) continue;

			TSet<UBlackboardData*> Visited;
			UBlackboardData* Current = BB;
			bool bCycleFound = false;
			while (Current)
			{
				if (Visited.Contains(Current))
				{
					bCycleFound = true;
					break;
				}
				Visited.Add(Current);
				Current = Current->Parent;
			}

			if (bCycleFound)
			{
				TSharedPtr<FJsonObject> CycleObj = MakeShared<FJsonObject>();
				CycleObj->SetStringField(TEXT("type"), TEXT("blackboard_parent_chain"));
				CycleObj->SetStringField(TEXT("asset_path"), Asset.PackageName.ToString());
				CycleObj->SetStringField(TEXT("message"), FString::Printf(
					TEXT("Blackboard '%s' has a circular parent chain"), *Asset.AssetName.ToString()));
				Cycles.Add(MakeShared<FJsonValueObject>(CycleObj));
			}
		}
	}

	// --- Check BT RunBehavior / RunBehaviorDynamic chains ---
	{
		TArray<FAssetData> BTAssets;
		AR.GetAssetsByClass(UBehaviorTree::StaticClass()->GetClassPathName(), BTAssets);

		// Build a map of BT -> referenced BTs (via asset registry references)
		TMap<FString, TArray<FString>> BTGraph;
		for (const FAssetData& Asset : BTAssets)
		{
			if (!PathFilter.IsEmpty() && !Asset.GetObjectPathString().StartsWith(PathFilter)) continue;

			FString BTPath = Asset.PackageName.ToString();
			TArray<FName> Dependencies;
			AR.GetDependencies(Asset.PackageName, Dependencies);

			TArray<FString>& Refs = BTGraph.FindOrAdd(BTPath);
			for (const FName& Dep : Dependencies)
			{
				FString DepPath = Dep.ToString();
				// Check if this dependency is also a BT
				for (const FAssetData& OtherBT : BTAssets)
				{
					if (OtherBT.PackageName.ToString() == DepPath)
					{
						Refs.Add(DepPath);
						break;
					}
				}
			}
		}

		// DFS cycle detection on BT graph
		TSet<FString> GlobalVisited;
		for (const auto& Pair : BTGraph)
		{
			TSet<FString> RecStack;
			TArray<FString> Stack;
			Stack.Add(Pair.Key);

			while (Stack.Num() > 0)
			{
				FString Current = Stack.Pop();
				if (RecStack.Contains(Current))
				{
					TSharedPtr<FJsonObject> CycleObj = MakeShared<FJsonObject>();
					CycleObj->SetStringField(TEXT("type"), TEXT("behavior_tree_chain"));
					CycleObj->SetStringField(TEXT("asset_path"), Pair.Key);
					CycleObj->SetStringField(TEXT("message"), FString::Printf(
						TEXT("BT '%s' has circular RunBehavior reference through '%s'"),
						*FPackageName::GetShortName(Pair.Key), *FPackageName::GetShortName(Current)));
					Cycles.Add(MakeShared<FJsonValueObject>(CycleObj));
					break;
				}
				RecStack.Add(Current);

				if (const TArray<FString>* Deps = BTGraph.Find(Current))
				{
					for (const FString& Dep : *Deps)
					{
						if (!GlobalVisited.Contains(Dep))
						{
							Stack.Add(Dep);
						}
					}
				}
			}
			GlobalVisited.Append(RecStack);
		}
	}

#if WITH_STATETREE
	// --- Check ST linked asset chains ---
	{
		TArray<FAssetData> STAssets;
		AR.GetAssetsByClass(UStateTree::StaticClass()->GetClassPathName(), STAssets);

		TMap<FString, TArray<FString>> STGraph;
		for (const FAssetData& Asset : STAssets)
		{
			if (!PathFilter.IsEmpty() && !Asset.GetObjectPathString().StartsWith(PathFilter)) continue;

			FString STPath = Asset.PackageName.ToString();
			TArray<FName> Dependencies;
			AR.GetDependencies(Asset.PackageName, Dependencies);

			TArray<FString>& Refs = STGraph.FindOrAdd(STPath);
			for (const FName& Dep : Dependencies)
			{
				FString DepPath = Dep.ToString();
				for (const FAssetData& OtherST : STAssets)
				{
					if (OtherST.PackageName.ToString() == DepPath)
					{
						Refs.Add(DepPath);
						break;
					}
				}
			}
		}

		TSet<FString> GlobalVisited;
		for (const auto& Pair : STGraph)
		{
			TSet<FString> RecStack;
			TArray<FString> Stack;
			Stack.Add(Pair.Key);

			while (Stack.Num() > 0)
			{
				FString Current = Stack.Pop();
				if (RecStack.Contains(Current))
				{
					TSharedPtr<FJsonObject> CycleObj = MakeShared<FJsonObject>();
					CycleObj->SetStringField(TEXT("type"), TEXT("state_tree_linked"));
					CycleObj->SetStringField(TEXT("asset_path"), Pair.Key);
					CycleObj->SetStringField(TEXT("message"), FString::Printf(
						TEXT("ST '%s' has circular linked-asset reference through '%s'"),
						*FPackageName::GetShortName(Pair.Key), *FPackageName::GetShortName(Current)));
					Cycles.Add(MakeShared<FJsonValueObject>(CycleObj));
					break;
				}
				RecStack.Add(Current);

				if (const TArray<FString>* Deps = STGraph.Find(Current))
				{
					for (const FString& Dep : *Deps)
					{
						if (!GlobalVisited.Contains(Dep))
						{
							Stack.Add(Dep);
						}
					}
				}
			}
			GlobalVisited.Append(RecStack);
		}
	}
#endif

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("cycles"), Cycles);
	Result->SetNumberField(TEXT("cycle_count"), Cycles.Num());
	Result->SetBoolField(TEXT("clean"), Cycles.Num() == 0);
	if (!PathFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("path_filter"), PathFilter);
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  210. export_ai_manifest
// ============================================================

FMonolithActionResult FMonolithAIDiscoveryActions::HandleExportAIManifest(const TSharedPtr<FJsonObject>& Params)
{
	FString PathFilter = Params->GetStringField(TEXT("path_filter"));
	FString Format = Params->GetStringField(TEXT("format")).ToLower();
	if (Format.IsEmpty()) Format = TEXT("json");

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Asset type descriptors
	struct FAssetTypeInfo
	{
		FString Key;
		FString Label;
		FTopLevelAssetPath ClassPath;
	};

	TArray<FAssetTypeInfo> Types;
	Types.Add({TEXT("behavior_trees"), TEXT("Behavior Trees"), UBehaviorTree::StaticClass()->GetClassPathName()});
	Types.Add({TEXT("blackboards"), TEXT("Blackboards"), UBlackboardData::StaticClass()->GetClassPathName()});
	Types.Add({TEXT("eqs_queries"), TEXT("EQS Queries"), UEnvQuery::StaticClass()->GetClassPathName()});
#if WITH_STATETREE
	Types.Add({TEXT("state_trees"), TEXT("State Trees"), UStateTree::StaticClass()->GetClassPathName()});
#endif
#if WITH_SMARTOBJECTS
	Types.Add({TEXT("smart_objects"), TEXT("Smart Objects"), USmartObjectDefinition::StaticClass()->GetClassPathName()});
#endif

	TSharedPtr<FJsonObject> ManifestJson = MakeShared<FJsonObject>();
	FString MarkdownOutput;

	if (Format == TEXT("markdown"))
	{
		MarkdownOutput += TEXT("# AI Asset Manifest\n\n");
		if (!PathFilter.IsEmpty())
		{
			MarkdownOutput += FString::Printf(TEXT("**Path filter:** `%s`\n\n"), *PathFilter);
		}
	}

	int32 TotalAssets = 0;

	for (const FAssetTypeInfo& TypeInfo : Types)
	{
		TArray<FAssetData> Assets;
		AR.GetAssetsByClass(TypeInfo.ClassPath, Assets);

		TArray<TSharedPtr<FJsonValue>> AssetArr;
		for (const FAssetData& Asset : Assets)
		{
			if (!PathFilter.IsEmpty() && !Asset.GetObjectPathString().StartsWith(PathFilter)) continue;

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("path"), Asset.PackageName.ToString());
			Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());

			// Count referencers
			TArray<FName> Referencers;
			AR.GetReferencers(Asset.PackageName, Referencers);
			Entry->SetNumberField(TEXT("referencer_count"), Referencers.Num());

			AssetArr.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TotalAssets += AssetArr.Num();

		if (Format == TEXT("json"))
		{
			TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
			Section->SetNumberField(TEXT("count"), AssetArr.Num());
			Section->SetArrayField(TEXT("assets"), AssetArr);
			ManifestJson->SetObjectField(TypeInfo.Key, Section);
		}
		else
		{
			MarkdownOutput += FString::Printf(TEXT("## %s (%d)\n\n"), *TypeInfo.Label, AssetArr.Num());
			if (AssetArr.Num() > 0)
			{
				MarkdownOutput += TEXT("| Name | Path | Refs |\n|------|------|------|\n");
				for (const auto& Item : AssetArr)
				{
					TSharedPtr<FJsonObject> ItemObj = Item->AsObject();
					MarkdownOutput += FString::Printf(TEXT("| %s | `%s` | %d |\n"),
						*ItemObj->GetStringField(TEXT("name")),
						*ItemObj->GetStringField(TEXT("path")),
						static_cast<int32>(ItemObj->GetNumberField(TEXT("referencer_count"))));
				}
			}
			MarkdownOutput += TEXT("\n");
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("total_assets"), TotalAssets);

	if (Format == TEXT("json"))
	{
		Result->SetObjectField(TEXT("manifest"), ManifestJson);
	}
	else
	{
		Result->SetStringField(TEXT("manifest_markdown"), MarkdownOutput);
	}

	Result->SetStringField(TEXT("format"), Format);
	if (!PathFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("path_filter"), PathFilter);
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  211. get_ai_behavior_summary
// ============================================================

namespace
{
	/** Recursively summarize a BT composite node into a structured summary. */
	TSharedPtr<FJsonObject> SummarizeBTNode(const UBTCompositeNode* Node, TSet<FName>& OutBBKeys, int32& OutDecisionPoints)
	{
		if (!Node) return nullptr;

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("node_type"), Node->GetClass()->GetName());
		Obj->SetStringField(TEXT("node_name"), Node->GetNodeName());
		Obj->SetNumberField(TEXT("child_count"), Node->Children.Num());

		if (Node->Children.Num() > 1)
		{
			OutDecisionPoints++;
		}

		TArray<TSharedPtr<FJsonValue>> ChildSummaries;
		for (int32 i = 0; i < Node->Children.Num(); ++i)
		{
			const FBTCompositeChild& Child = Node->Children[i];

			TSharedPtr<FJsonObject> ChildObj = MakeShared<FJsonObject>();
			ChildObj->SetNumberField(TEXT("index"), i);

			// Decorators (conditions)
			TArray<TSharedPtr<FJsonValue>> DecArr;
			for (const UBTDecorator* Dec : Child.Decorators)
			{
				if (Dec)
				{
					DecArr.Add(MakeShared<FJsonValueString>(Dec->GetNodeName()));
					// Scan for BB key refs
					for (TFieldIterator<FStructProperty> It(Dec->GetClass()); It; ++It)
					{
						if (It->Struct->GetFName() == FName(TEXT("BlackboardKeySelector")))
						{
							const FBlackboardKeySelector* Sel = reinterpret_cast<const FBlackboardKeySelector*>(
								It->ContainerPtrToValuePtr<void>(Dec));
							if (!Sel->SelectedKeyName.IsNone()) OutBBKeys.Add(Sel->SelectedKeyName);
						}
					}
				}
			}
			if (DecArr.Num() > 0)
			{
				ChildObj->SetArrayField(TEXT("decorators"), DecArr);
			}

			if (Child.ChildTask)
			{
				ChildObj->SetStringField(TEXT("task"), Child.ChildTask->GetNodeName());
				ChildObj->SetStringField(TEXT("task_class"), Child.ChildTask->GetClass()->GetName());
			}
			else if (Child.ChildComposite)
			{
				ChildObj->SetObjectField(TEXT("composite"), SummarizeBTNode(Child.ChildComposite, OutBBKeys, OutDecisionPoints));
			}

			ChildSummaries.Add(MakeShared<FJsonValueObject>(ChildObj));
		}

		Obj->SetArrayField(TEXT("children"), ChildSummaries);
		return Obj;
	}
}

FMonolithActionResult FMonolithAIDiscoveryActions::HandleGetAIBehaviorSummary(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));
	}
	AssetPath = FMonolithAssetUtils::ResolveAssetPath(AssetPath);

	// Try as BehaviorTree first
	UBehaviorTree* BT = FMonolithAssetUtils::LoadAssetByPath<UBehaviorTree>(AssetPath);
	if (BT)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetStringField(TEXT("asset_type"), TEXT("BehaviorTree"));

		if (BT->BlackboardAsset)
		{
			Result->SetStringField(TEXT("blackboard"), BT->BlackboardAsset->GetPathName());
		}

		TSet<FName> BBKeys;
		int32 DecisionPoints = 0;

		if (BT->RootNode)
		{
			Result->SetObjectField(TEXT("tree_summary"), SummarizeBTNode(BT->RootNode, BBKeys, DecisionPoints));
		}

		TArray<TSharedPtr<FJsonValue>> KeyArr;
		for (const FName& Key : BBKeys)
		{
			KeyArr.Add(MakeShared<FJsonValueString>(Key.ToString()));
		}
		Result->SetArrayField(TEXT("bb_key_dependencies"), KeyArr);
		Result->SetNumberField(TEXT("decision_points"), DecisionPoints);

		return FMonolithActionResult::Success(Result);
	}

#if WITH_STATETREE
	// Try as StateTree
	UStateTree* ST = FMonolithAssetUtils::LoadAssetByPath<UStateTree>(AssetPath);
	if (ST)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetStringField(TEXT("asset_type"), TEXT("StateTree"));
		Result->SetBoolField(TEXT("is_ready_to_run"), ST->IsReadyToRun());

		if (ST->GetSchema())
		{
			Result->SetStringField(TEXT("schema"), ST->GetSchema()->GetClass()->GetName());
		}

#if WITH_EDITORONLY_DATA
		UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(ST->EditorData);
		if (EditorData)
		{
			// Count states, transitions, tasks recursively
			int32 StateCount = 0, TransitionCount = 0, TaskCount = 0;
			TArray<FString> FlowPaths;

			TFunction<void(const UStateTreeState*, const FString&)> WalkStates =
				[&](const UStateTreeState* State, const FString& Path)
			{
				if (!State) return;
				StateCount++;
				FString CurrentPath = Path.IsEmpty() ? State->Name.ToString() : (Path + TEXT(" -> ") + State->Name.ToString());

				TaskCount += State->Tasks.Num();
				TransitionCount += State->Transitions.Num();

				if (State->Children.Num() == 0)
				{
					FlowPaths.Add(CurrentPath);
				}
				for (const UStateTreeState* Child : State->Children)
				{
					WalkStates(Child, CurrentPath);
				}
			};

			for (const UStateTreeState* SubTree : EditorData->SubTrees)
			{
				WalkStates(SubTree, TEXT(""));
			}

			Result->SetNumberField(TEXT("state_count"), StateCount);
			Result->SetNumberField(TEXT("transition_count"), TransitionCount);
			Result->SetNumberField(TEXT("task_count"), TaskCount);

			TArray<TSharedPtr<FJsonValue>> PathArr;
			for (const FString& P : FlowPaths)
			{
				PathArr.Add(MakeShared<FJsonValueString>(P));
			}
			Result->SetArrayField(TEXT("flow_paths"), PathArr);
			Result->SetNumberField(TEXT("decision_points"), StateCount > 0 ? StateCount - FlowPaths.Num() : 0);
		}
#endif

		return FMonolithActionResult::Success(Result);
	}
#endif

	return FMonolithActionResult::Error(FString::Printf(TEXT("Asset at '%s' is not a BehaviorTree or StateTree"), *AssetPath));
}
