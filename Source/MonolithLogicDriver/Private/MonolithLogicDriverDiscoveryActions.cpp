#include "MonolithLogicDriverDiscoveryActions.h"
#include "AssetRegistry/AssetData.h"
#include "MonolithParamSchema.h"
#include "MonolithLogicDriverInternal.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_LOGICDRIVER

DEFINE_LOG_CATEGORY_STATIC(LogMonolithLDDiscovery, Log, All);

void FMonolithLogicDriverDiscoveryActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("logicdriver"), TEXT("get_sm_overview"),
		TEXT("Project scan: count SM Blueprints, Node Blueprints, and component usage across the project"),
		FMonolithActionHandler::CreateStatic(&HandleGetSMOverview),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("path_filter"), TEXT("Only include assets under this path prefix (e.g. /Game/StateMachines)"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("validate_state_machine"),
		TEXT("Validate a state machine for common issues: missing initial state, orphaned states, unreachable nodes"),
		FMonolithActionHandler::CreateStatic(&HandleValidateStateMachine),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("SM Blueprint asset path to validate"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("find_sm_references"),
		TEXT("Find all Blueprints in the project that reference a given SM Blueprint (via dependencies)"),
		FMonolithActionHandler::CreateStatic(&HandleFindSMReferences),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("SM Blueprint asset path to find references for"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("visualize_sm_as_text"),
		TEXT("Generate a text diagram of a state machine in ASCII, Mermaid, or DOT format"),
		FMonolithActionHandler::CreateStatic(&HandleVisualizeSMAsText),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("SM Blueprint asset path"))
			.Required(TEXT("format"), TEXT("string"), TEXT("Output format: ascii, mermaid, dot"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("explain_state_machine"),
		TEXT("Generate a structured explanation of a state machine: purpose, states, flow paths, key decisions, complexity rating"),
		FMonolithActionHandler::CreateStatic(&HandleExplainStateMachine),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("SM Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("find_node_class_usages"),
		TEXT("Search all SM Blueprints in the project for nodes that use a specific Node Blueprint class"),
		FMonolithActionHandler::CreateStatic(&HandleFindNodeClassUsages),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("node_bp_path"), TEXT("Node Blueprint asset path to search for"))
			.Build());

	UE_LOG(LogMonolithLDDiscovery, Log, TEXT("MonolithLogicDriver Discovery: registered 6 actions"));
}

FMonolithActionResult FMonolithLogicDriverDiscoveryActions::HandleGetSMOverview(const TSharedPtr<FJsonObject>& Params)
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FString PathFilter;
	if (Params.IsValid() && Params->HasField(TEXT("path_filter")))
	{
		PathFilter = Params->GetStringField(TEXT("path_filter"));
	}

	// ── 1. SM Blueprints (USMBlueprint) ──
	TArray<TSharedPtr<FJsonValue>> SMBlueprintArray;
	{
		UClass* SMBPClass = MonolithLD::GetSMBlueprintClass();
		if (!SMBPClass)
		{
			return FMonolithActionResult::Error(TEXT("Logic Driver not loaded — SMBlueprint class not found"));
		}
		FARFilter Filter;
		Filter.ClassPaths.Add(SMBPClass->GetClassPathName());
		Filter.bRecursiveClasses = true;
		if (!PathFilter.IsEmpty())
		{
			Filter.PackagePaths.Add(FName(*PathFilter));
			Filter.bRecursivePaths = true;
		}

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssets(Filter, Assets);

		for (const FAssetData& Asset : Assets)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
			Entry->SetStringField(TEXT("path"), Asset.GetObjectPathString());
			SMBlueprintArray.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	// ── 2. Node Blueprints (USMNodeBlueprint) ──
	TArray<TSharedPtr<FJsonValue>> NodeBlueprintArray;
	{
		UClass* NodeBPClass = FindFirstObject<UClass>(TEXT("SMNodeBlueprint"), EFindFirstObjectOptions::NativeFirst);
		if (NodeBPClass)
		{
			FARFilter Filter;
			Filter.ClassPaths.Add(NodeBPClass->GetClassPathName());
			Filter.bRecursiveClasses = true;
			if (!PathFilter.IsEmpty())
			{
				Filter.PackagePaths.Add(FName(*PathFilter));
				Filter.bRecursivePaths = true;
			}

			TArray<FAssetData> Assets;
			AssetRegistry.GetAssets(Filter, Assets);

			for (const FAssetData& Asset : Assets)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
				Entry->SetStringField(TEXT("path"), Asset.GetObjectPathString());

				// Try to extract the node type from the asset's parent class tag
				FString NodeType;
				Asset.GetTagValue(FName(TEXT("ParentClass")), NodeType);
				if (!NodeType.IsEmpty())
				{
					// Extract just the class name from the full path
					int32 LastDot;
					if (NodeType.FindLastChar(TEXT('.'), LastDot))
					{
						NodeType = NodeType.Mid(LastDot + 1);
					}
				}
				Entry->SetStringField(TEXT("node_type"), NodeType);

				NodeBlueprintArray.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
	}

	// ── 3. Component usage — deferred to indexer (expensive scan) ──
	// Return -1 to indicate "not yet scanned"

	// ── Build result ──
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("sm_blueprint_count"), SMBlueprintArray.Num());
	Result->SetArrayField(TEXT("sm_blueprints"), SMBlueprintArray);
	Result->SetNumberField(TEXT("node_blueprint_count"), NodeBlueprintArray.Num());
	Result->SetArrayField(TEXT("node_blueprints"), NodeBlueprintArray);
	Result->SetNumberField(TEXT("sm_component_count"), -1); // Not yet scanned — requires indexer
	Result->SetArrayField(TEXT("actors_with_sm_component"), TArray<TSharedPtr<FJsonValue>>());
	if (!PathFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("path_filter"), PathFilter);
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLogicDriverDiscoveryActions::HandleValidateStateMachine(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));

	FString LoadError;
	UBlueprint* SMBlueprint = MonolithLD::LoadSMBlueprint(AssetPath, LoadError);
	if (!SMBlueprint) return FMonolithActionResult::Error(LoadError);

	UEdGraph* RootGraph = MonolithLD::GetRootGraph(SMBlueprint);
	if (!RootGraph) return FMonolithActionResult::Error(TEXT("No root SM graph found"));

	TArray<TSharedPtr<FJsonValue>> Issues;

	// Collect states and their connectivity info
	struct FStateInfo
	{
		UEdGraphNode* Node;
		FString Name;
		FString Guid;
		bool bIsInitial = false;
		bool bHasInboundTransition = false;
		bool bHasOutboundTransition = false;
		bool bIsEndState = false;
	};
	TArray<FStateInfo> States;
	int32 TransitionCount = 0;
	bool bHasInitialState = false;

	UClass* BaseClass = MonolithLD::GetSMGraphNodeBaseClass();

	for (UEdGraphNode* RawNode : RootGraph->Nodes)
	{
		if (!RawNode) continue;
		FString NodeType = MonolithLD::GetNodeType(RawNode);

		if (NodeType == TEXT("state") || NodeType == TEXT("state_machine"))
		{
			FStateInfo Info;
			Info.Node = RawNode;
			Info.Name = RawNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			Info.Guid = RawNode->NodeGuid.ToString();
			Info.bIsEndState = MonolithLD::GetBoolProperty(RawNode, TEXT("bIsEndState"));

			// Check if initial (connected from entry node)
			for (UEdGraphPin* Pin : RawNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input)
				{
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->GetOwningNode())
						{
							UEdGraphNode* SourceNode = LinkedPin->GetOwningNode();
							FString SourceType = MonolithLD::GetNodeType(SourceNode);
							if (SourceType == TEXT("entry"))
							{
								Info.bIsInitial = true;
							}
							else if (SourceType == TEXT("transition"))
							{
								Info.bHasInboundTransition = true;
							}
						}
					}
				}
				else if (Pin && Pin->Direction == EGPD_Output)
				{
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->GetOwningNode())
						{
							FString TargetType = MonolithLD::GetNodeType(LinkedPin->GetOwningNode());
							if (TargetType == TEXT("transition"))
							{
								Info.bHasOutboundTransition = true;
							}
						}
					}
				}
			}

			if (Info.bIsInitial) bHasInitialState = true;
			States.Add(Info);
		}
		else if (NodeType == TEXT("transition"))
		{
			TransitionCount++;
		}
	}

	// Validation checks
	auto AddIssue = [&Issues](const FString& Severity, const FString& Message, const FString& NodeGuid = FString())
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), Severity);
		Issue->SetStringField(TEXT("message"), Message);
		if (!NodeGuid.IsEmpty()) Issue->SetStringField(TEXT("node_guid"), NodeGuid);
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	};

	// 1. No initial state
	if (!bHasInitialState && States.Num() > 0)
	{
		AddIssue(TEXT("error"), TEXT("No initial state — entry node is not connected to any state"));
	}

	// 2. Orphaned states (no inbound transitions and not initial)
	for (const FStateInfo& State : States)
	{
		if (!State.bIsInitial && !State.bHasInboundTransition)
		{
			AddIssue(TEXT("warning"),
				FString::Printf(TEXT("Orphaned state '%s' — no inbound transitions"), *State.Name),
				State.Guid);
		}
	}

	// 3. Dead-end states (no outbound transitions and not end state)
	for (const FStateInfo& State : States)
	{
		if (!State.bHasOutboundTransition && !State.bIsEndState)
		{
			AddIssue(TEXT("info"),
				FString::Printf(TEXT("Dead-end state '%s' — no outbound transitions and not marked as end state"), *State.Name),
				State.Guid);
		}
	}

	// 4. No transitions at all
	if (TransitionCount == 0 && States.Num() > 1)
	{
		AddIssue(TEXT("warning"), TEXT("No transitions found between states"));
	}

	// 5. No end states
	bool bHasEndState = false;
	for (const FStateInfo& State : States)
	{
		if (State.bIsEndState) { bHasEndState = true; break; }
	}
	if (!bHasEndState && States.Num() > 0)
	{
		AddIssue(TEXT("info"), TEXT("No end states defined — state machine will run indefinitely unless stopped externally"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetBoolField(TEXT("is_valid"), Issues.Num() == 0);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetNumberField(TEXT("state_count"), States.Num());
	Result->SetNumberField(TEXT("transition_count"), TransitionCount);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLogicDriverDiscoveryActions::HandleFindSMReferences(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Get the package name from the asset path
	FString PackageName = AssetPath;
	// Strip .AssetName suffix if present
	int32 DotIdx;
	if (PackageName.FindLastChar(TEXT('.'), DotIdx))
	{
		PackageName = PackageName.Left(DotIdx);
	}

	// Find referencers (assets that depend on this SM)
	TArray<FAssetIdentifier> Referencers;
	AR.GetReferencers(FAssetIdentifier(FName(*PackageName)), Referencers);

	TArray<TSharedPtr<FJsonValue>> RefArray;
	for (const FAssetIdentifier& Ref : Referencers)
	{
		FString RefPath = Ref.PackageName.ToString();
		if (RefPath == PackageName) continue; // Skip self

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("package"), RefPath);

		// Try to get asset data for more info
		TArray<FAssetData> AssetsInPackage;
		AR.GetAssetsByPackageName(Ref.PackageName, AssetsInPackage, true);
		if (AssetsInPackage.Num() > 0)
		{
			Entry->SetStringField(TEXT("asset_name"), AssetsInPackage[0].AssetName.ToString());
			Entry->SetStringField(TEXT("asset_class"), AssetsInPackage[0].AssetClassPath.GetAssetName().ToString());
		}

		RefArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("reference_count"), RefArray.Num());
	Result->SetArrayField(TEXT("references"), RefArray);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLogicDriverDiscoveryActions::HandleFindNodeClassUsages(const TSharedPtr<FJsonObject>& Params)
{
	FString NodeBPPath = Params->GetStringField(TEXT("node_bp_path"));
	if (NodeBPPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'node_bp_path'"));

	// Load the node blueprint to get its generated class
	FString LoadError;
	UBlueprint* NodeBP = Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *NodeBPPath));
	if (!NodeBP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load Node Blueprint at '%s'"), *NodeBPPath));
	}
	UClass* TargetClass = NodeBP->GeneratedClass;
	FString TargetClassName = TargetClass ? TargetClass->GetName() : NodeBP->GetName();

	// Scan all SM Blueprints in the project
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	UClass* SMBPClass = MonolithLD::GetSMBlueprintClass();
	if (!SMBPClass) return FMonolithActionResult::Error(TEXT("SMBlueprint class not found"));

	FARFilter Filter;
	Filter.ClassPaths.Add(SMBPClass->GetClassPathName());
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> SMAssets;
	AR.GetAssets(Filter, SMAssets);

	TArray<TSharedPtr<FJsonValue>> Usages;
	int32 SMsScanned = 0;

	for (const FAssetData& SMAsset : SMAssets)
	{
		UBlueprint* SMBP = Cast<UBlueprint>(SMAsset.GetAsset());
		if (!SMBP) continue;

		SMsScanned++;
		UEdGraph* RootGraph = MonolithLD::GetRootGraph(SMBP);
		if (!RootGraph) continue;

		for (UEdGraphNode* Node : RootGraph->Nodes)
		{
			if (!Node) continue;

			// Check if this node references the target class
			// Node Blueprints in LD are used as custom state/transition classes
			// Check NodeClass, RuntimeNodeClass, or StateClass properties
			bool bFound = false;

			static const FName ClassPropNames[] = {
				TEXT("NodeClass"),
				TEXT("RuntimeNodeClass"),
				TEXT("StateClass"),
				TEXT("TransitionClass"),
				TEXT("NodeInstanceClass"),
			};

			for (const FName& PropName : ClassPropNames)
			{
				FProperty* Prop = Node->GetClass()->FindPropertyByName(PropName);
				if (!Prop) continue;

				FString ClassValue;
				Prop->ExportTextItem_Direct(ClassValue, Prop->ContainerPtrToValuePtr<void>(Node), nullptr, nullptr, PPF_None);

				if (ClassValue.Contains(TargetClassName))
				{
					bFound = true;
					break;
				}
			}

			// Also check the node's own class hierarchy
			if (!bFound && TargetClass)
			{
				if (Node->GetClass()->IsChildOf(TargetClass))
				{
					bFound = true;
				}
			}

			if (bFound)
			{
				TSharedPtr<FJsonObject> Usage = MakeShared<FJsonObject>();
				Usage->SetStringField(TEXT("sm_path"), SMAsset.GetObjectPathString());
				Usage->SetStringField(TEXT("sm_name"), SMAsset.AssetName.ToString());
				Usage->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
				Usage->SetStringField(TEXT("node_type"), MonolithLD::GetNodeType(Node));
				Usage->SetStringField(TEXT("node_name"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
				Usages.Add(MakeShared<FJsonValueObject>(Usage));
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_bp_path"), NodeBPPath);
	Result->SetStringField(TEXT("target_class"), TargetClassName);
	Result->SetNumberField(TEXT("sm_blueprints_scanned"), SMsScanned);
	Result->SetNumberField(TEXT("usage_count"), Usages.Num());
	Result->SetArrayField(TEXT("usages"), Usages);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLogicDriverDiscoveryActions::HandleVisualizeSMAsText(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString Format = Params->GetStringField(TEXT("format")).ToLower();
	if (AssetPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));
	if (Format.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'format'"));
	if (Format != TEXT("ascii") && Format != TEXT("mermaid") && Format != TEXT("dot"))
	{
		return FMonolithActionResult::Error(TEXT("format must be: ascii, mermaid, dot"));
	}

	FString LoadError;
	UBlueprint* SMBlueprint = MonolithLD::LoadSMBlueprint(AssetPath, LoadError);
	if (!SMBlueprint) return FMonolithActionResult::Error(LoadError);

	UEdGraph* RootGraph = MonolithLD::GetRootGraph(SMBlueprint);
	if (!RootGraph) return FMonolithActionResult::Error(TEXT("No root SM graph found"));

	// Collect node info
	struct FNodeInfo
	{
		FString Guid;
		FString Name;
		FString SafeName; // sanitized for diagram identifiers
		FString Type;
		bool bIsInitial = false;
		bool bIsEnd = false;
	};
	TMap<FString, FNodeInfo> NodeMap; // guid -> info

	struct FTransInfo
	{
		FString SourceGuid;
		FString TargetGuid;
		FString Name;
	};
	TArray<FTransInfo> Transitions;

	UClass* BaseClass = MonolithLD::GetSMGraphNodeBaseClass();

	for (UEdGraphNode* RawNode : RootGraph->Nodes)
	{
		if (!RawNode) continue;
		FString NodeType = MonolithLD::GetNodeType(RawNode);

		if (NodeType == TEXT("state") || NodeType == TEXT("conduit")
			|| NodeType == TEXT("state_machine") || NodeType == TEXT("any_state"))
		{
			FNodeInfo Info;
			Info.Guid = RawNode->NodeGuid.ToString();
			Info.Name = RawNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			if (Info.Name.IsEmpty()) Info.Name = NodeType;
			Info.SafeName = Info.Name.Replace(TEXT(" "), TEXT("_")).Replace(TEXT("-"), TEXT("_"));
			Info.Type = NodeType;
			Info.bIsEnd = MonolithLD::GetBoolProperty(RawNode, TEXT("bIsEndState"));

			// Check initial
			for (UEdGraphPin* Pin : RawNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input)
				{
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->GetOwningNode())
						{
							FString SourceType = MonolithLD::GetNodeType(LinkedPin->GetOwningNode());
							if (SourceType == TEXT("entry"))
							{
								Info.bIsInitial = true;
							}
						}
					}
				}
			}
			NodeMap.Add(Info.Guid, Info);
		}
		else if (NodeType == TEXT("transition"))
		{
			FTransInfo Trans;
			Trans.Name = RawNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			for (UEdGraphPin* Pin : RawNode->Pins)
			{
				if (!Pin) continue;
				if (Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0])
				{
					Trans.SourceGuid = Pin->LinkedTo[0]->GetOwningNode()->NodeGuid.ToString();
				}
				if (Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0])
				{
					Trans.TargetGuid = Pin->LinkedTo[0]->GetOwningNode()->NodeGuid.ToString();
				}
			}
			if (!Trans.SourceGuid.IsEmpty() && !Trans.TargetGuid.IsEmpty())
			{
				Transitions.Add(Trans);
			}
		}
	}

	FString Diagram;

	if (Format == TEXT("mermaid"))
	{
		Diagram += TEXT("stateDiagram-v2\n");
		for (const auto& Pair : NodeMap)
		{
			const FNodeInfo& N = Pair.Value;
			if (N.bIsInitial)
			{
				Diagram += FString::Printf(TEXT("    [*] --> %s\n"), *N.SafeName);
			}
			if (N.bIsEnd)
			{
				Diagram += FString::Printf(TEXT("    %s --> [*]\n"), *N.SafeName);
			}
			if (N.Type == TEXT("conduit"))
			{
				Diagram += FString::Printf(TEXT("    %s : <<conduit>>\n"), *N.SafeName);
			}
			else if (N.Type == TEXT("state_machine"))
			{
				Diagram += FString::Printf(TEXT("    %s : <<nested SM>>\n"), *N.SafeName);
			}
			else if (N.Type == TEXT("any_state"))
			{
				Diagram += FString::Printf(TEXT("    %s : <<any state>>\n"), *N.SafeName);
			}
		}
		for (const FTransInfo& T : Transitions)
		{
			const FNodeInfo* Src = NodeMap.Find(T.SourceGuid);
			const FNodeInfo* Tgt = NodeMap.Find(T.TargetGuid);
			if (Src && Tgt)
			{
				if (T.Name.IsEmpty())
					Diagram += FString::Printf(TEXT("    %s --> %s\n"), *Src->SafeName, *Tgt->SafeName);
				else
					Diagram += FString::Printf(TEXT("    %s --> %s : %s\n"), *Src->SafeName, *Tgt->SafeName, *T.Name);
			}
		}
	}
	else if (Format == TEXT("dot"))
	{
		Diagram += FString::Printf(TEXT("digraph \"%s\" {\n"), *SMBlueprint->GetName());
		Diagram += TEXT("    rankdir=LR;\n");
		Diagram += TEXT("    node [shape=box, style=rounded];\n");
		for (const auto& Pair : NodeMap)
		{
			const FNodeInfo& N = Pair.Value;
			FString Shape = TEXT("box");
			if (N.Type == TEXT("conduit")) Shape = TEXT("diamond");
			else if (N.Type == TEXT("state_machine")) Shape = TEXT("doubleoctagon");
			else if (N.Type == TEXT("any_state")) Shape = TEXT("ellipse");

			FString Extra;
			if (N.bIsInitial) Extra = TEXT(", penwidth=3");
			if (N.bIsEnd) Extra += TEXT(", peripheries=2");

			Diagram += FString::Printf(TEXT("    \"%s\" [label=\"%s\", shape=%s%s];\n"),
				*N.Guid, *N.Name, *Shape, *Extra);
		}
		for (const FTransInfo& T : Transitions)
		{
			if (T.Name.IsEmpty())
				Diagram += FString::Printf(TEXT("    \"%s\" -> \"%s\";\n"), *T.SourceGuid, *T.TargetGuid);
			else
				Diagram += FString::Printf(TEXT("    \"%s\" -> \"%s\" [label=\"%s\"];\n"), *T.SourceGuid, *T.TargetGuid, *T.Name);
		}
		Diagram += TEXT("}\n");
	}
	else // ascii
	{
		Diagram += FString::Printf(TEXT("=== %s ===\n\n"), *SMBlueprint->GetName());

		// List states
		Diagram += TEXT("States:\n");
		for (const auto& Pair : NodeMap)
		{
			const FNodeInfo& N = Pair.Value;
			FString Flags;
			if (N.bIsInitial) Flags += TEXT(" [INITIAL]");
			if (N.bIsEnd) Flags += TEXT(" [END]");
			if (N.Type == TEXT("conduit")) Flags += TEXT(" [CONDUIT]");
			else if (N.Type == TEXT("state_machine")) Flags += TEXT(" [NESTED SM]");
			else if (N.Type == TEXT("any_state")) Flags += TEXT(" [ANY STATE]");
			Diagram += FString::Printf(TEXT("  [%s]%s\n"), *N.Name, *Flags);
		}

		Diagram += TEXT("\nTransitions:\n");
		for (const FTransInfo& T : Transitions)
		{
			const FNodeInfo* Src = NodeMap.Find(T.SourceGuid);
			const FNodeInfo* Tgt = NodeMap.Find(T.TargetGuid);
			if (Src && Tgt)
			{
				Diagram += FString::Printf(TEXT("  [%s] --> [%s]"), *Src->Name, *Tgt->Name);
				if (!T.Name.IsEmpty()) Diagram += FString::Printf(TEXT("  (%s)"), *T.Name);
				Diagram += TEXT("\n");
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("format"), Format);
	Result->SetStringField(TEXT("diagram"), Diagram);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLogicDriverDiscoveryActions::HandleExplainStateMachine(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));

	FString LoadError;
	UBlueprint* SMBlueprint = MonolithLD::LoadSMBlueprint(AssetPath, LoadError);
	if (!SMBlueprint) return FMonolithActionResult::Error(LoadError);

	UEdGraph* RootGraph = MonolithLD::GetRootGraph(SMBlueprint);
	if (!RootGraph) return FMonolithActionResult::Error(TEXT("No root SM graph found"));

	// Collect state info
	struct FStateData
	{
		FString Name;
		FString Guid;
		bool bIsInitial = false;
		bool bIsEnd = false;
		TArray<FString> OutgoingTargets; // names of states reachable from this one
		int32 ConnectionCount = 0;
	};

	TMap<FString, FStateData> StatesByGuid;  // guid -> data
	TMap<FString, FString> GuidToName;
	int32 TransitionCount = 0;

	// First pass: collect states
	for (UEdGraphNode* Node : RootGraph->Nodes)
	{
		if (!Node) continue;
		FString NodeType = MonolithLD::GetNodeType(Node);

		if (NodeType == TEXT("state") || NodeType == TEXT("conduit")
			|| NodeType == TEXT("state_machine") || NodeType == TEXT("any_state"))
		{
			FStateData Data;
			Data.Name = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			if (Data.Name.IsEmpty()) Data.Name = NodeType;
			Data.Guid = Node->NodeGuid.ToString();
			Data.bIsEnd = MonolithLD::GetBoolProperty(Node, TEXT("bIsEndState"));

			// Check initial (connected from entry)
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input)
				{
					for (UEdGraphPin* LP : Pin->LinkedTo)
					{
						if (LP && LP->GetOwningNode() && MonolithLD::GetNodeType(LP->GetOwningNode()) == TEXT("entry"))
						{
							Data.bIsInitial = true;
						}
					}
				}
			}

			GuidToName.Add(Data.Guid, Data.Name);
			StatesByGuid.Add(Data.Guid, Data);
		}
	}

	// Second pass: collect transitions and wire up outgoing targets
	for (UEdGraphNode* Node : RootGraph->Nodes)
	{
		if (!Node) continue;
		FString NodeType = MonolithLD::GetNodeType(Node);
		if (NodeType != TEXT("transition")) continue;

		TransitionCount++;

		FString SourceGuid, TargetGuid;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			if (Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0])
			{
				SourceGuid = Pin->LinkedTo[0]->GetOwningNode()->NodeGuid.ToString();
			}
			if (Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0])
			{
				TargetGuid = Pin->LinkedTo[0]->GetOwningNode()->NodeGuid.ToString();
			}
		}

		FStateData* SrcData = StatesByGuid.Find(SourceGuid);
		FString* TgtName = GuidToName.Find(TargetGuid);
		if (SrcData && TgtName)
		{
			SrcData->OutgoingTargets.Add(*TgtName);
			SrcData->ConnectionCount++;
		}

		// Count inbound on target too
		FStateData* TgtData = StatesByGuid.Find(TargetGuid);
		if (TgtData)
		{
			TgtData->ConnectionCount++;
		}
	}

	int32 StateCount = StatesByGuid.Num();

	// Build states array
	TArray<TSharedPtr<FJsonValue>> StatesArr;
	FString InitialGuid;
	for (const auto& Pair : StatesByGuid)
	{
		const FStateData& S = Pair.Value;
		TSharedPtr<FJsonObject> StateJson = MakeShared<FJsonObject>();
		StateJson->SetStringField(TEXT("name"), S.Name);

		FString Type = TEXT("normal");
		if (S.bIsInitial) { Type = TEXT("initial"); InitialGuid = S.Guid; }
		else if (S.bIsEnd) Type = TEXT("end");
		StateJson->SetStringField(TEXT("type"), Type);
		StateJson->SetNumberField(TEXT("connections"), S.ConnectionCount);

		StatesArr.Add(MakeShared<FJsonValueObject>(StateJson));
	}

	// Trace flow paths from initial state via BFS (max 50 paths to avoid explosion)
	TArray<TSharedPtr<FJsonValue>> FlowPaths;
	if (!InitialGuid.IsEmpty())
	{
		// BFS to build the primary path and branching paths
		TArray<TArray<FString>> Paths;
		TArray<TArray<FString>> Queue;
		TSet<FString> Visited;

		TArray<FString> StartPath;
		FString* StartName = GuidToName.Find(InitialGuid);
		if (StartName) StartPath.Add(*StartName);
		Queue.Add(StartPath);

		while (Queue.Num() > 0 && Paths.Num() < 20)
		{
			TArray<FString> CurrentPath = Queue[0];
			Queue.RemoveAt(0);

			FString LastName = CurrentPath.Last();

			// Find the state data for this name
			const FStateData* LastState = nullptr;
			for (const auto& Pair : StatesByGuid)
			{
				if (Pair.Value.Name == LastName)
				{
					LastState = &Pair.Value;
					break;
				}
			}

			if (!LastState || LastState->OutgoingTargets.Num() == 0)
			{
				// Terminal path
				if (CurrentPath.Num() > 1)
				{
					Paths.Add(CurrentPath);
				}
				continue;
			}

			for (const FString& Target : LastState->OutgoingTargets)
			{
				FString PathKey = LastName + TEXT("->") + Target;
				if (Visited.Contains(PathKey))
				{
					// Add cycle indicator and terminate
					TArray<FString> CyclePath = CurrentPath;
					CyclePath.Add(Target + TEXT(" (cycle)"));
					Paths.Add(CyclePath);
					continue;
				}
				Visited.Add(PathKey);

				TArray<FString> NewPath = CurrentPath;
				NewPath.Add(Target);
				Queue.Add(NewPath);
			}
		}

		for (const TArray<FString>& Path : Paths)
		{
			FString PathStr = FString::Join(Path, TEXT(" -> "));
			FlowPaths.Add(MakeShared<FJsonValueString>(PathStr));
		}
	}

	// Identify key decisions (states with >1 outgoing transition)
	TArray<TSharedPtr<FJsonValue>> KeyDecisions;
	for (const auto& Pair : StatesByGuid)
	{
		const FStateData& S = Pair.Value;
		if (S.OutgoingTargets.Num() > 1)
		{
			FString TargetList = FString::Join(S.OutgoingTargets, TEXT(" or "));
			FString Decision = FString::Printf(TEXT("At %s, can go to %s"), *S.Name, *TargetList);
			KeyDecisions.Add(MakeShared<FJsonValueString>(Decision));
		}
	}

	// Complexity rating
	FString Complexity;
	if (StateCount <= 3 && TransitionCount <= 3)
	{
		Complexity = TEXT("simple");
	}
	else if (StateCount <= 8 && TransitionCount <= 12)
	{
		Complexity = TEXT("moderate");
	}
	else
	{
		Complexity = TEXT("complex");
	}

	// Purpose string
	FString Purpose = FString::Printf(TEXT("State machine with %d states managing %s"),
		StateCount,
		StateCount <= 3 ? TEXT("a basic flow") :
		StateCount <= 8 ? TEXT("a multi-phase process") : TEXT("a complex behavioral system"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), SMBlueprint->GetName());
	Result->SetStringField(TEXT("purpose"), Purpose);
	Result->SetArrayField(TEXT("states"), StatesArr);
	Result->SetArrayField(TEXT("flow_paths"), FlowPaths);
	Result->SetArrayField(TEXT("key_decisions"), KeyDecisions);
	Result->SetStringField(TEXT("complexity"), Complexity);
	Result->SetNumberField(TEXT("state_count"), StateCount);
	Result->SetNumberField(TEXT("transition_count"), TransitionCount);

	return FMonolithActionResult::Success(Result);
}

#else

void FMonolithLogicDriverDiscoveryActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// Logic Driver not available
}

#endif // WITH_LOGICDRIVER
