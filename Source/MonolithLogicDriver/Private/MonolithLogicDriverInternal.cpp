#include "MonolithLogicDriverInternal.h"

#if WITH_LOGICDRIVER

#include "AssetRegistry/AssetData.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "AssetRegistry/AssetRegistryModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithLDInternal, Log, All);

namespace MonolithLD
{

// ── Class resolution (cached via static locals) ──────────────────

static UClass* FindLDClass(const TCHAR* ClassName)
{
	UClass* Found = FindFirstObject<UClass>(ClassName, EFindFirstObjectOptions::NativeFirst);
	if (!Found)
	{
		UE_LOG(LogMonolithLDInternal, Warning, TEXT("Logic Driver class '%s' not found"), ClassName);
	}
	return Found;
}

UClass* GetSMBlueprintClass()           { static UClass* C = FindLDClass(TEXT("SMBlueprint")); return C; }
UClass* GetSMNodeBlueprintClass()       { static UClass* C = FindLDClass(TEXT("SMNodeBlueprint")); return C; }
UClass* GetSMGraphClass()               { static UClass* C = FindLDClass(TEXT("SMGraph")); return C; }
UClass* GetSMGraphNodeBaseClass()       { static UClass* C = FindLDClass(TEXT("SMGraphNode_Base")); return C; }
UClass* GetSMGraphNodeStateClass()      { static UClass* C = FindLDClass(TEXT("SMGraphNode_StateNode")); return C; }
UClass* GetSMGraphNodeTransitionClass() { static UClass* C = FindLDClass(TEXT("SMGraphNode_TransitionEdge")); return C; }
UClass* GetSMGraphNodeConduitClass()    { static UClass* C = FindLDClass(TEXT("SMGraphNode_ConduitNode")); return C; }
UClass* GetSMGraphNodeSMClass()         { static UClass* C = FindLDClass(TEXT("SMGraphNode_StateMachineStateNode")); return C; }
UClass* GetSMGraphNodeAnyStateClass()   { static UClass* C = FindLDClass(TEXT("SMGraphNode_AnyStateNode")); return C; }
UClass* GetSMBlueprintFactoryClass()    { static UClass* C = FindLDClass(TEXT("SMBlueprintFactory")); return C; }
UClass* GetSMNodeBlueprintFactoryClass(){ static UClass* C = FindLDClass(TEXT("SMNodeBlueprintFactory")); return C; }
UClass* GetSMInstanceClass()            { static UClass* C = FindLDClass(TEXT("SMInstance")); return C; }
UClass* GetSMComponentClass()           { static UClass* C = FindLDClass(TEXT("SMStateMachineComponent")); return C; }
UClass* GetSMEntryNodeClass()           { static UClass* C = FindLDClass(TEXT("SMGraphNode_StateMachineEntryNode")); return C; }

// ── Reflection utilities ─────────────────────────────────────────

UObject* GetObjectProperty(UObject* Obj, FName PropName)
{
	if (!Obj) return nullptr;
	FObjectProperty* Prop = CastField<FObjectProperty>(Obj->GetClass()->FindPropertyByName(PropName));
	if (!Prop) return nullptr;
	return Prop->GetObjectPropertyValue(Prop->ContainerPtrToValuePtr<void>(Obj));
}

FString GetStringProperty(UObject* Obj, FName PropName)
{
	if (!Obj) return FString();
	FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropName);
	if (!Prop) return FString();
	FString Value;
	Prop->ExportTextItem_Direct(Value, Prop->ContainerPtrToValuePtr<void>(Obj), nullptr, Obj, PPF_None);
	return Value;
}

bool GetBoolProperty(UObject* Obj, FName PropName, bool Default)
{
	if (!Obj) return Default;
	FBoolProperty* Prop = CastField<FBoolProperty>(Obj->GetClass()->FindPropertyByName(PropName));
	if (!Prop) return Default;
	return Prop->GetPropertyValue(Prop->ContainerPtrToValuePtr<void>(Obj));
}

int32 GetIntProperty(UObject* Obj, FName PropName, int32 Default)
{
	if (!Obj) return Default;
	FIntProperty* Prop = CastField<FIntProperty>(Obj->GetClass()->FindPropertyByName(PropName));
	if (!Prop) return Default;
	return Prop->GetPropertyValue(Prop->ContainerPtrToValuePtr<void>(Obj));
}

float GetFloatProperty(UObject* Obj, FName PropName, float Default)
{
	if (!Obj) return Default;
	FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropName);
	if (!Prop) return Default;
	if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		return FloatProp->GetPropertyValue(FloatProp->ContainerPtrToValuePtr<void>(Obj));
	if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		return static_cast<float>(DoubleProp->GetPropertyValue(DoubleProp->ContainerPtrToValuePtr<void>(Obj)));
	return Default;
}

// ── Helpers ──────────────────────────────────────────────────────

UBlueprint* LoadSMBlueprint(const FString& AssetPath, FString& OutError)
{
	UClass* SMBPClass = GetSMBlueprintClass();
	if (!SMBPClass)
	{
		OutError = TEXT("Logic Driver not loaded — SMBlueprint class not found");
		return nullptr;
	}

	UObject* LoadedObj = StaticLoadObject(UBlueprint::StaticClass(), nullptr, *AssetPath);
	UBlueprint* BP = Cast<UBlueprint>(LoadedObj);
	if (!BP)
	{
		OutError = FString::Printf(TEXT("Failed to load Blueprint at '%s'"), *AssetPath);
		return nullptr;
	}
	if (!BP->GetClass()->IsChildOf(SMBPClass))
	{
		OutError = FString::Printf(TEXT("'%s' is not an SM Blueprint (class: %s)"), *AssetPath, *BP->GetClass()->GetName());
		return nullptr;
	}
	return BP;
}

// Recursive helper to find SMGraph in nested sub-graphs
static UEdGraph* FindSMGraphRecursive(UEdGraph* Graph, UClass* SMGraphClass)
{
	if (!Graph || !SMGraphClass) return nullptr;

	// Check this graph itself
	if (Graph->GetClass()->IsChildOf(SMGraphClass))
	{
		return Graph;
	}

	// Check sub-graphs owned by this graph
	for (UEdGraph* SubGraph : Graph->SubGraphs)
	{
		if (UEdGraph* Found = FindSMGraphRecursive(SubGraph, SMGraphClass))
		{
			return Found;
		}
	}

	// Check sub-graphs owned by nodes in this graph
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		// Nodes can have bound graphs (e.g., SMGraphK2Node_StateMachineSelectNode owns the SMGraph)
		for (UEdGraph* SubGraph : Node->GetSubGraphs())
		{
			if (UEdGraph* Found = FindSMGraphRecursive(SubGraph, SMGraphClass))
			{
				return Found;
			}
		}
	}

	return nullptr;
}

UEdGraph* GetRootGraph(UBlueprint* Blueprint)
{
	if (!Blueprint) return nullptr;
	UClass* SMGraphClass = GetSMGraphClass();
	if (!SMGraphClass) return nullptr;

	// Search all UbergraphPages recursively — Logic Driver nests the SMGraph
	// inside a K2 graph node (SMGraphK2Node_StateMachineSelectNode)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (UEdGraph* Found = FindSMGraphRecursive(Graph, SMGraphClass))
		{
			return Found;
		}
	}
	return nullptr;
}

UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& GuidString)
{
	if (!Graph) return nullptr;
	FGuid TargetGuid;
	if (!FGuid::Parse(GuidString, TargetGuid)) return nullptr;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == TargetGuid) return Node;
	}
	return nullptr;
}

bool CompileSMBlueprint(UBlueprint* Blueprint, FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Null blueprint");
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (Blueprint->Status == BS_Error)
	{
		OutError = TEXT("Compilation failed with errors");
		return false;
	}
	return true;
}

bool SetNodeName(UEdGraphNode* Node, const FString& Name)
{
	if (!Node || Name.IsEmpty()) return false;

	// Primary: use UEdGraphNode::OnRenameNode (engine virtual — LD overrides to set name properly)
	Node->Modify();
	Node->OnRenameNode(Name);

	// Also set via template reflection as backup
	UObject* Template = GetObjectProperty(Node, TEXT("NodeInstanceTemplate"));
	if (Template)
	{
		FStructProperty* DescProp = CastField<FStructProperty>(Template->GetClass()->FindPropertyByName(TEXT("NodeDescription")));
		if (DescProp)
		{
			void* DescPtr = DescProp->ContainerPtrToValuePtr<void>(Template);
			FNameProperty* NameProp = CastField<FNameProperty>(DescProp->Struct->FindPropertyByName(TEXT("Name")));
			if (NameProp)
			{
				Template->Modify();
				NameProp->SetPropertyValue(NameProp->ContainerPtrToValuePtr<void>(DescPtr), FName(*Name));
			}
		}
	}

	return true;
}

FString GetNodeName(UEdGraphNode* Node)
{
	if (!Node) return FString();

	UObject* Template = GetObjectProperty(Node, TEXT("NodeInstanceTemplate"));
	if (Template)
	{
		FStructProperty* DescProp = CastField<FStructProperty>(Template->GetClass()->FindPropertyByName(TEXT("NodeDescription")));
		if (DescProp)
		{
			void* DescPtr = DescProp->ContainerPtrToValuePtr<void>(Template);
			FNameProperty* NameProp = CastField<FNameProperty>(DescProp->Struct->FindPropertyByName(TEXT("Name")));
			if (NameProp)
			{
				FName FoundName = NameProp->GetPropertyValue(NameProp->ContainerPtrToValuePtr<void>(DescPtr));
				if (FoundName != NAME_None)
				{
					return FoundName.ToString();
				}
			}
		}
	}

	// Fallback to node title
	return Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
}

FString GetNodeType(UEdGraphNode* Node)
{
	if (!Node) return TEXT("unknown");
	UClass* NodeClass = Node->GetClass();

	// Check in specificity order (most specific first)
	if (UClass* TransClass = GetSMGraphNodeTransitionClass())
		if (NodeClass->IsChildOf(TransClass)) return TEXT("transition");
	if (UClass* ConduitClass = GetSMGraphNodeConduitClass())
		if (NodeClass->IsChildOf(ConduitClass)) return TEXT("conduit");
	if (UClass* AnyStateClass = GetSMGraphNodeAnyStateClass())
		if (NodeClass->IsChildOf(AnyStateClass)) return TEXT("any_state");
	if (UClass* SMNodeClass = GetSMGraphNodeSMClass())
		if (NodeClass->IsChildOf(SMNodeClass)) return TEXT("state_machine");
	if (UClass* StateClass = GetSMGraphNodeStateClass())
		if (NodeClass->IsChildOf(StateClass)) return TEXT("state");
	if (UClass* EntryClass = GetSMEntryNodeClass())
		if (NodeClass->IsChildOf(EntryClass)) return TEXT("entry");
	if (UClass* BaseClass = GetSMGraphNodeBaseClass())
		if (NodeClass->IsChildOf(BaseClass)) return TEXT("sm_node");  // Generic SM node

	// Fallback — check class name for hints
	FString ClassName = NodeClass->GetName();
	if (ClassName.Contains(TEXT("Entry"))) return TEXT("entry");
	return TEXT("unknown");
}

TSharedPtr<FJsonObject> NodeToJson(UEdGraphNode* Node, bool bDetailed)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	if (!Node) return Json;

	Json->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
	Json->SetStringField(TEXT("name"), GetNodeName(Node));
	Json->SetNumberField(TEXT("position_x"), Node->NodePosX);
	Json->SetNumberField(TEXT("position_y"), Node->NodePosY);

	FString NodeType = GetNodeType(Node);
	Json->SetStringField(TEXT("node_type"), NodeType);

	if (NodeType == TEXT("transition"))
	{
		// Source and target from pin connections
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			if (Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0])
			{
				Json->SetStringField(TEXT("source_guid"), Pin->LinkedTo[0]->GetOwningNode()->NodeGuid.ToString());
			}
			if (Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0])
			{
				Json->SetStringField(TEXT("target_guid"), Pin->LinkedTo[0]->GetOwningNode()->NodeGuid.ToString());
			}
		}
		// Priority via reflection
		Json->SetNumberField(TEXT("priority"), GetIntProperty(Node, TEXT("PriorityOrder"), 0));
	}
	else if (NodeType == TEXT("state"))
	{
		// Check if initial state (connected from entry node)
		bool bIsInitial = false;
		UClass* EntryClass = GetSMEntryNodeClass();
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input)
			{
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (LinkedPin && LinkedPin->GetOwningNode())
					{
						UEdGraphNode* SourceNode = LinkedPin->GetOwningNode();
						if (EntryClass && SourceNode->GetClass()->IsChildOf(EntryClass))
						{
							bIsInitial = true;
							break;
						}
					}
				}
				if (bIsInitial) break;
			}
		}
		Json->SetBoolField(TEXT("is_initial"), bIsInitial);
		Json->SetBoolField(TEXT("is_end_state"), GetBoolProperty(Node, TEXT("bIsEndState")));
	}

	if (bDetailed)
	{
		Json->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());

		// Connections summary
		TArray<TSharedPtr<FJsonValue>> InputConns, OutputConns;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;
				FString ConnGuid = LinkedPin->GetOwningNode()->NodeGuid.ToString();
				if (Pin->Direction == EGPD_Input)
					InputConns.Add(MakeShared<FJsonValueString>(ConnGuid));
				else
					OutputConns.Add(MakeShared<FJsonValueString>(ConnGuid));
			}
		}
		if (InputConns.Num() > 0) Json->SetArrayField(TEXT("input_connections"), InputConns);
		if (OutputConns.Num() > 0) Json->SetArrayField(TEXT("output_connections"), OutputConns);
	}

	return Json;
}

TSharedPtr<FJsonObject> SMStructureToJson(UBlueprint* Blueprint, int32 MaxDepth)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	if (!Blueprint) return Json;

	Json->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Json->SetStringField(TEXT("asset_name"), Blueprint->GetName());

	UEdGraph* RootGraph = GetRootGraph(Blueprint);
	if (!RootGraph)
	{
		Json->SetStringField(TEXT("error"), TEXT("No root SM graph found"));
		return Json;
	}

	TArray<TSharedPtr<FJsonValue>> States, Transitions, Conduits, NestedSMs;
	FString EntryNodeGuid;
	int32 StateCount = 0, TransitionCount = 0;

	for (UEdGraphNode* RawNode : RootGraph->Nodes)
	{
		if (!RawNode) continue;
		FString NodeType = GetNodeType(RawNode);
		TSharedPtr<FJsonObject> NodeJson = NodeToJson(RawNode, true);

		if (NodeType == TEXT("transition"))
		{
			Transitions.Add(MakeShared<FJsonValueObject>(NodeJson));
			TransitionCount++;
		}
		else if (NodeType == TEXT("conduit"))
		{
			Conduits.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
		else if (NodeType == TEXT("state_machine"))
		{
			NestedSMs.Add(MakeShared<FJsonValueObject>(NodeJson));
			StateCount++;
		}
		else if (NodeType == TEXT("state") || NodeType == TEXT("any_state"))
		{
			States.Add(MakeShared<FJsonValueObject>(NodeJson));
			if (NodeType == TEXT("state")) StateCount++;
		}
		else if (NodeType == TEXT("entry"))
		{
			if (EntryNodeGuid.IsEmpty())
				EntryNodeGuid = RawNode->NodeGuid.ToString();
			States.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
	}

	Json->SetArrayField(TEXT("states"), States);
	Json->SetArrayField(TEXT("transitions"), Transitions);
	Json->SetArrayField(TEXT("conduits"), Conduits);
	Json->SetArrayField(TEXT("nested_state_machines"), NestedSMs);
	Json->SetStringField(TEXT("entry_node_guid"), EntryNodeGuid);
	Json->SetNumberField(TEXT("state_count"), StateCount);
	Json->SetNumberField(TEXT("transition_count"), TransitionCount);

	return Json;
}

bool EnsureAssetPathFree(UPackage* Package, const FString& PackagePath, const FString& AssetName, FString& OutError)
{
	FString FullPath = PackagePath + TEXT(".") + AssetName;

	// Tier 1: AssetRegistry
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData Existing = AR.GetAssetByObjectPath(FSoftObjectPath(FullPath));
	if (Existing.IsValid())
	{
		OutError = FString::Printf(TEXT("Asset already exists at '%s'"), *FullPath);
		return false;
	}

	// Tier 2: Global in-memory search
	UObject* InMemory = FindObject<UObject>(nullptr, *FullPath);
	if (InMemory)
	{
		OutError = FString::Printf(TEXT("Asset '%s' exists in memory"), *FullPath);
		return false;
	}

	// Tier 3: Package-scoped search (catches objects loaded by FullyLoad)
	if (Package)
	{
		UObject* InPackage = FindObject<UObject>(Package, *AssetName);
		if (InPackage)
		{
			OutError = FString::Printf(TEXT("Object '%s' already exists in package '%s' (stale from prior attempt — use a different path or restart editor)"), *AssetName, *PackagePath);
			return false;
		}
	}

	return true;
}

} // namespace MonolithLD

#endif // WITH_LOGICDRIVER
