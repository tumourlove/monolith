#include "MonolithAIBehaviorTreeActions.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#include "Misc/Optional.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_NativeEnum.h"
#include "EnvironmentQuery/EnvQuery.h"

#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Service.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "ScopedTransaction.h"
#include "IMonolithGraphFormatter.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "BehaviorTree/Tasks/BTTask_BlueprintBase.h"
#include "BehaviorTree/Decorators/BTDecorator_BlueprintBase.h"
#include "BehaviorTree/Services/BTService_BlueprintBase.h"
#include "Interfaces/IPluginManager.h"  // Phase D2: GameplayBehaviorSmartObjects plugin probe

// Phase I2: BT-to-GAS direct ability activation. Header self-gates on
// WITH_GAMEPLAYABILITIES; the action handler also gates so the registry entry
// returns a clear runtime error when the engine plugin is disabled.
#if WITH_GAMEPLAYABILITIES
#include "BehaviorTreeTasks/BTTask_TryActivateAbility.h"
#include "Abilities/GameplayAbility.h"
#endif

// ============================================================
//  Helpers
// ============================================================

namespace
{
	/** Recursively serialize a BT graph node and its children to JSON */
	TSharedPtr<FJsonObject> SerializeBTGraphNode(const UBehaviorTreeGraphNode* GraphNode, int32 Depth)
	{
		if (!GraphNode) return nullptr;

		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();

		// Node identity
		NodeObj->SetStringField(TEXT("node_id"), GraphNode->NodeGuid.ToString());
		NodeObj->SetNumberField(TEXT("depth"), Depth);

		// Get the underlying BT node
		UBTNode* BTNode = Cast<UBTNode>(GraphNode->NodeInstance);
		if (BTNode)
		{
			NodeObj->SetStringField(TEXT("node_class"), BTNode->GetClass()->GetName());
			NodeObj->SetStringField(TEXT("display_name"), BTNode->GetNodeName());

			// Node category
			if (BTNode->IsA<UBTCompositeNode>())
			{
				NodeObj->SetStringField(TEXT("category"), TEXT("composite"));
			}
			else if (BTNode->IsA<UBTTaskNode>())
			{
				NodeObj->SetStringField(TEXT("category"), TEXT("task"));
			}
			else if (BTNode->IsA<UBTDecorator>())
			{
				NodeObj->SetStringField(TEXT("category"), TEXT("decorator"));
			}
			else if (BTNode->IsA<UBTService>())
			{
				NodeObj->SetStringField(TEXT("category"), TEXT("service"));
			}
		}
		else
		{
			// Root node or unknown
			NodeObj->SetStringField(TEXT("node_class"), GraphNode->GetClass()->GetName());
			NodeObj->SetStringField(TEXT("display_name"), GraphNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		}

		// Decorators
		if (GraphNode->Decorators.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> DecoratorArr;
			for (const UBehaviorTreeGraphNode* DecNode : GraphNode->Decorators)
			{
				if (!DecNode) continue;
				TSharedPtr<FJsonObject> DecObj = MakeShared<FJsonObject>();
				DecObj->SetStringField(TEXT("node_id"), DecNode->NodeGuid.ToString());
				if (UBTNode* DecBTNode = Cast<UBTNode>(DecNode->NodeInstance))
				{
					DecObj->SetStringField(TEXT("node_class"), DecBTNode->GetClass()->GetName());
					DecObj->SetStringField(TEXT("display_name"), DecBTNode->GetNodeName());
				}
				DecoratorArr.Add(MakeShared<FJsonValueObject>(DecObj));
			}
			NodeObj->SetArrayField(TEXT("decorators"), DecoratorArr);
		}

		// Services
		if (GraphNode->Services.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ServiceArr;
			for (const UBehaviorTreeGraphNode* SvcNode : GraphNode->Services)
			{
				if (!SvcNode) continue;
				TSharedPtr<FJsonObject> SvcObj = MakeShared<FJsonObject>();
				SvcObj->SetStringField(TEXT("node_id"), SvcNode->NodeGuid.ToString());
				if (UBTNode* SvcBTNode = Cast<UBTNode>(SvcNode->NodeInstance))
				{
					SvcObj->SetStringField(TEXT("node_class"), SvcBTNode->GetClass()->GetName());
					SvcObj->SetStringField(TEXT("display_name"), SvcBTNode->GetNodeName());
				}
				ServiceArr.Add(MakeShared<FJsonValueObject>(SvcObj));
			}
			NodeObj->SetArrayField(TEXT("services"), ServiceArr);
		}

		// Children — walk output pins to find connected child nodes
		TArray<TSharedPtr<FJsonValue>> ChildrenArr;
		for (UEdGraphPin* Pin : GraphNode->Pins)
		{
			if (Pin->Direction != EGPD_Output) continue;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;
				UBehaviorTreeGraphNode* ChildGraphNode = Cast<UBehaviorTreeGraphNode>(LinkedPin->GetOwningNode());
				if (ChildGraphNode)
				{
					TSharedPtr<FJsonObject> ChildJson = SerializeBTGraphNode(ChildGraphNode, Depth + 1);
					if (ChildJson.IsValid())
					{
						ChildrenArr.Add(MakeShared<FJsonValueObject>(ChildJson));
					}
				}
			}
		}
		if (ChildrenArr.Num() > 0)
		{
			NodeObj->SetArrayField(TEXT("children"), ChildrenArr);
		}

		return NodeObj;
	}

	/** Load a BT and its editor graph, returning both. Sets OutError on failure. */
	bool LoadBTAndGraph(const TSharedPtr<FJsonObject>& Params, UBehaviorTree*& OutBT, UBehaviorTreeGraph*& OutGraph, FString& OutAssetPath, FString& OutError)
	{
		OutBT = MonolithAI::LoadBehaviorTreeFromParams(Params, OutAssetPath, OutError);
		if (!OutBT)
		{
			return false;
		}

		OutGraph = Cast<UBehaviorTreeGraph>(OutBT->BTGraph);
		if (!OutGraph)
		{
			// Graph doesn't exist yet — create it (same as FBehaviorTreeEditor::RestoreBehaviorTree)
			OutBT->BTGraph = FBlueprintEditorUtils::CreateNewGraph(
				OutBT, TEXT("Behavior Tree"),
				UBehaviorTreeGraph::StaticClass(),
				UEdGraphSchema_BehaviorTree::StaticClass());
			OutGraph = Cast<UBehaviorTreeGraph>(OutBT->BTGraph);
			if (OutGraph)
			{
				const UEdGraphSchema* Schema = OutGraph->GetSchema();
				Schema->CreateDefaultNodesForGraph(*OutGraph);
				OutGraph->OnCreated();
				OutGraph->Initialize();
			}
		}
		if (!OutGraph)
		{
			OutError = TEXT("Failed to create BT editor graph");
			return false;
		}
		return true;
	}

	/** Find a BT graph node by GUID string across all nodes (including sub-nodes). */
	UBehaviorTreeGraphNode* FindGraphNodeByGuid(UBehaviorTreeGraph* Graph, const FString& GuidStr)
	{
		FGuid TargetGuid;
		if (!FGuid::Parse(GuidStr, TargetGuid))
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UBehaviorTreeGraphNode* BTNode = Cast<UBehaviorTreeGraphNode>(Node);
			if (!BTNode) continue;

			if (BTNode->NodeGuid == TargetGuid)
			{
				return BTNode;
			}

			// Check decorators
			for (UBehaviorTreeGraphNode* Dec : BTNode->Decorators)
			{
				if (Dec && Dec->NodeGuid == TargetGuid)
				{
					return Dec;
				}
			}

			// Check services
			for (UBehaviorTreeGraphNode* Svc : BTNode->Services)
			{
				if (Svc && Svc->NodeGuid == TargetGuid)
				{
					return Svc;
				}
			}
		}
		return nullptr;
	}

	/**
	 * Phase J F15 — distinguish "invalid GUID format" from "unknown GUID" at the call site.
	 *
	 * The legacy `FindGraphNodeByGuid` collapses both failure modes into `nullptr`, so 16
	 * sibling call sites in this file all returned the same "...not found" error regardless
	 * of whether the caller typed garbage or a valid-but-unmatched GUID. Hoisted here so
	 * every site emits one of two distinct, actionable messages:
	 *   - parse fail:     `<ParamName> '<GuidStr>' is not a valid GUID`
	 *   - lookup fail:    `No node with GUID '<GuidStr>' in BT '<BTName>'`
	 *
	 * Returns true on success (OutNode populated, OutError untouched).
	 * Returns false on failure (OutNode = nullptr, OutError populated with the right message).
	 *
	 * Existing post-lookup validation (cast to subclass, ValidateParentForChildTask, etc.)
	 * stays at each call site — this helper only replaces the parse + base-lookup steps.
	 */
	bool RequireBtNodeByGuid(
		UBehaviorTreeGraph* Graph,
		const FString& GuidStr,
		const FString& ParamName,
		const FString& BTName,
		UBehaviorTreeGraphNode*& OutNode,
		FString& OutError)
	{
		OutNode = nullptr;

		FGuid Probe;
		if (!FGuid::Parse(GuidStr, Probe))
		{
			OutError = FString::Printf(TEXT("%s '%s' is not a valid GUID"), *ParamName, *GuidStr);
			return false;
		}

		UBehaviorTreeGraphNode* Found = FindGraphNodeByGuid(Graph, GuidStr);
		if (!Found)
		{
			OutError = FString::Printf(
				TEXT("No node with GUID '%s' in BT '%s'"),
				*GuidStr,
				BTName.IsEmpty() ? TEXT("(unknown)") : *BTName);
			return false;
		}

		OutNode = Found;
		return true;
	}

	/** Find the root graph node in a BT graph. */
	UBehaviorTreeGraphNode_Root* FindRootNode(UBehaviorTreeGraph* Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UBehaviorTreeGraphNode_Root* Root = Cast<UBehaviorTreeGraphNode_Root>(Node))
			{
				return Root;
			}
		}
		return nullptr;
	}

	/** Get children of a graph node via pin connections, in order. */
	TArray<UBehaviorTreeGraphNode*> GetChildNodes(UBehaviorTreeGraphNode* Parent)
	{
		TArray<UBehaviorTreeGraphNode*> Children;
		for (UEdGraphPin* Pin : Parent->Pins)
		{
			if (Pin->Direction != EGPD_Output) continue;
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (UBehaviorTreeGraphNode* Child = Cast<UBehaviorTreeGraphNode>(Linked->GetOwningNode()))
				{
					Children.Add(Child);
				}
			}
		}
		return Children;
	}

	/**
	 * Phase F1: validate that a graph node is a legal parent for a *task* graph child.
	 *
	 * The crash this guards against: `UBehaviorTreeGraphNode_Root` is a UI-only sentinel
	 * with `NodeInstance == nullptr` — wiring a Task directly under it produces an
	 * invalid graph that crashes `UBehaviorTreeGraph::UpdateAsset()` at
	 * `BehaviorTreeGraph.cpp:517` (`RootNode->Children.Reset()`, deref of null
	 * `BTAsset->RootNode`). The engine's own schema (`UEdGraphSchema_BehaviorTree::
	 * CanCreateConnection`) rejects Root->Task at the connection step ("Only
	 * composite nodes are allowed"), but our `ConnectParentChild` helper bypasses
	 * the schema by calling raw `MakeLinkTo`. This validator restores the schema
	 * invariant at the API entry point.
	 *
	 * Returns `TOptional<FString>` — `Some(error)` rejects the call, `Unset` accepts.
	 * `ActionName` is interpolated into the error so callers don't all hand-roll
	 * the same prefix.
	 */
	TOptional<FString> ValidateParentForChildTask(const UBehaviorTreeGraphNode* ParentGraphNode, const TCHAR* ActionName)
	{
		if (!ParentGraphNode)
		{
			return FString::Printf(TEXT("%s: parent graph node is null"), ActionName);
		}

		// Root edge node has no NodeInstance by engine design (BehaviorTreeGraphNode_Root.cpp:54-57).
		if (ParentGraphNode->IsA<UBehaviorTreeGraphNode_Root>() && ParentGraphNode->NodeInstance == nullptr)
		{
			return FString::Printf(TEXT(
				"%s: Cannot add task as direct child of root: BT root has no composite. "
				"Add a composite node first via add_bt_node(class=BTComposite_Selector) "
				"then re-target this action with parent_id=<composite_guid>."),
				ActionName);
		}

		// Non-root parent must wrap a UBTCompositeNode at runtime — tasks may only attach to composites.
		const UBTNode* ParentBTNode = Cast<UBTNode>(ParentGraphNode->NodeInstance);
		if (!ParentBTNode)
		{
			// Root-with-null already errored above; any other graph node lacking a NodeInstance is malformed.
			return FString::Printf(TEXT(
				"%s: parent graph node '%s' has no BT node instance — graph is malformed. "
				"Add a composite first via add_bt_node(class=BTComposite_Selector)."),
				ActionName, *ParentGraphNode->NodeGuid.ToString());
		}
		if (!ParentBTNode->IsA<UBTCompositeNode>())
		{
			return FString::Printf(TEXT(
				"%s: parent node '%s' is a %s; tasks may only be attached to composites "
				"(Selector/Sequence/Parallel/SimpleParallel)."),
				ActionName,
				*ParentGraphNode->NodeGuid.ToString(),
				*ParentBTNode->GetClass()->GetName());
		}

		return TOptional<FString>(); // unset = valid
	}

	/**
	 * Connect parent output pin to child input pin at a given index.
	 *
	 * Phase F1 belt-and-suspenders: route through `UEdGraphSchema::CanCreateConnection`
	 * before calling `MakeLinkTo`. The entry-point validator (`ValidateParentForChildTask`)
	 * is the primary defense, but routing through the schema here also protects:
	 *  - future internal callers who skip the entry guard
	 *  - composite-of-composite / decorator wiring edge cases the schema enforces
	 *
	 * Schema reference: `UEdGraphSchema_BehaviorTree::CanCreateConnection`
	 * (`Engine/Source/Editor/BehaviorTreeEditor/Private/EdGraphSchema_BehaviorTree.cpp:238`).
	 */
	bool ConnectParentChild(UBehaviorTreeGraphNode* Parent, UBehaviorTreeGraphNode* Child, int32 Index = -1)
	{
		UEdGraphPin* ParentOut = Parent ? Parent->GetOutputPin() : nullptr;
		UEdGraphPin* ChildIn  = Child  ? Child->GetInputPin()  : nullptr;
		if (!ParentOut || !ChildIn) return false;

		const UEdGraph* OwnerGraph = Parent->GetGraph();
		if (const UEdGraphSchema* Schema = OwnerGraph ? OwnerGraph->GetSchema() : nullptr)
		{
			const FPinConnectionResponse Resp = Schema->CanCreateConnection(ParentOut, ChildIn);
			if (Resp.Response == CONNECT_RESPONSE_DISALLOW)
			{
				UE_LOG(LogMonolithAI, Warning,
					TEXT("ConnectParentChild rejected by schema: %s"), *Resp.Message.ToString());
				return false;
			}
		}

		ParentOut->MakeLinkTo(ChildIn);
		return true;
	}

	/** Disconnect a child node from its parent pin connections. */
	void DisconnectFromParent(UBehaviorTreeGraphNode* Child)
	{
		UEdGraphPin* ChildIn = Child->GetInputPin();
		if (ChildIn)
		{
			ChildIn->BreakAllPinLinks();
		}
	}

	/** Determine graph node class for a given BT node class. */
	UClass* GetGraphNodeClassForBTNode(UClass* BTNodeClass)
	{
		if (BTNodeClass->IsChildOf(UBTCompositeNode::StaticClass()))
		{
			return UBehaviorTreeGraphNode_Composite::StaticClass();
		}
		else if (BTNodeClass->IsChildOf(UBTTaskNode::StaticClass()))
		{
			return UBehaviorTreeGraphNode_Task::StaticClass();
		}
		else if (BTNodeClass->IsChildOf(UBTDecorator::StaticClass()))
		{
			return UBehaviorTreeGraphNode_Decorator::StaticClass();
		}
		else if (BTNodeClass->IsChildOf(UBTService::StaticClass()))
		{
			return UBehaviorTreeGraphNode_Service::StaticClass();
		}
		return nullptr;
	}

	/** Apply a JSON property value to a UPROPERTY on a UObject. Returns true on success. */
	bool SetPropertyValue(UObject* Obj, const FString& PropName, const TSharedPtr<FJsonValue>& Value, UBehaviorTree* BT, FString& OutError)
	{
		if (!Obj || !Value.IsValid())
		{
			OutError = TEXT("Null object or value");
			return false;
		}

		// Handle dot-notation: "BlackboardKey.SelectedKeyName" → treat as "BlackboardKey" with value
		FString ActualPropName = PropName;
		if (PropName.Contains(TEXT(".")))
		{
			FString Left, Right;
			PropName.Split(TEXT("."), &Left, &Right);
			// If the left part is a FBlackboardKeySelector, use the left part as prop name
			FProperty* ParentProp = Obj->GetClass()->FindPropertyByName(*Left);
			if (ParentProp)
			{
				if (FStructProperty* ParentStruct = CastField<FStructProperty>(ParentProp))
				{
					if (ParentStruct->Struct->GetFName() == FName(TEXT("BlackboardKeySelector")))
					{
						// Redirect: set the BB key selector with the value
						ActualPropName = Left;
					}
				}
			}
		}

		FProperty* Prop = Obj->GetClass()->FindPropertyByName(*ActualPropName);
		if (!Prop)
		{
			OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *PropName, *Obj->GetClass()->GetName());
			return false;
		}

		void* PropAddr = Prop->ContainerPtrToValuePtr<void>(Obj);

		// Special handling: FBlackboardKeySelector
		if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			if (StructProp->Struct->GetFName() == FName(TEXT("BlackboardKeySelector")))
			{
				FBlackboardKeySelector* KeySelector = reinterpret_cast<FBlackboardKeySelector*>(PropAddr);
				FString KeyName = Value->AsString();
				KeySelector->SelectedKeyName = *KeyName;

				// Resolve against the BT's blackboard
				if (BT && BT->BlackboardAsset)
				{
					KeySelector->ResolveSelectedKey(*BT->BlackboardAsset);
				}
				return true;
			}

			// Special handling: FAIDataProviderFloatValue / ValueOrBBKey_Float (UE 5.7)
			FName StructFName = StructProp->Struct->GetFName();
			if (StructFName == FName(TEXT("AIDataProviderFloatValue")) || StructFName == FName(TEXT("ValueOrBBKey_Float")))
			{
				// Phase D3: if caller passed a struct literal like "(DefaultValue=5.0)" as a string,
				// route through ImportText_Direct so we don't atod-zero the wrapped numeric.
				if (Value->Type == EJson::String)
				{
					FString StrVal = Value->AsString();
					if (StrVal.TrimStartAndEnd().StartsWith(TEXT("(")))
					{
						const TCHAR* Buffer = *StrVal;
						if (StructProp->ImportText_Direct(Buffer, PropAddr, Obj, PPF_None) != nullptr)
						{
							return true;
						}
						OutError = FString::Printf(TEXT("Failed to ImportText for FAIDataProviderFloatValue literal '%s' on '%s'"), *StrVal, *PropName);
						return false;
					}
				}

				// Set DefaultValue directly — the struct wraps a float with optional data binding
				FProperty* DefaultValProp = StructProp->Struct->FindPropertyByName(TEXT("DefaultValue"));
				if (DefaultValProp)
				{
					void* DefaultAddr = DefaultValProp->ContainerPtrToValuePtr<void>(PropAddr);
					if (FFloatProperty* FloatDefault = CastField<FFloatProperty>(DefaultValProp))
					{
						FloatDefault->SetPropertyValue(DefaultAddr, (float)Value->AsNumber());
						return true;
					}
				}
				OutError = FString::Printf(TEXT("Failed to set DefaultValue on FAIDataProviderFloatValue '%s'"), *PropName);
				return false;
			}
			if (StructFName == FName(TEXT("AIDataProviderBoolValue")) || StructFName == FName(TEXT("ValueOrBBKey_Bool")))
			{
				// Phase D3: handle struct-literal string form before AsBool().
				if (Value->Type == EJson::String)
				{
					FString StrVal = Value->AsString();
					if (StrVal.TrimStartAndEnd().StartsWith(TEXT("(")))
					{
						const TCHAR* Buffer = *StrVal;
						if (StructProp->ImportText_Direct(Buffer, PropAddr, Obj, PPF_None) != nullptr)
						{
							return true;
						}
						OutError = FString::Printf(TEXT("Failed to ImportText for FAIDataProviderBoolValue literal '%s' on '%s'"), *StrVal, *PropName);
						return false;
					}
				}

				FProperty* DefaultValProp = StructProp->Struct->FindPropertyByName(TEXT("DefaultValue"));
				if (DefaultValProp)
				{
					void* DefaultAddr = DefaultValProp->ContainerPtrToValuePtr<void>(PropAddr);
					if (FBoolProperty* BoolDefault = CastField<FBoolProperty>(DefaultValProp))
					{
						BoolDefault->SetPropertyValue(DefaultAddr, Value->AsBool());
						return true;
					}
				}
				OutError = FString::Printf(TEXT("Failed to set DefaultValue on FAIDataProviderBoolValue '%s'"), *PropName);
				return false;
			}
		}

		// Bool
		if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			BoolProp->SetPropertyValue(PropAddr, Value->AsBool());
			return true;
		}

		// Numeric types
		if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
		{
			IntProp->SetPropertyValue(PropAddr, (int32)Value->AsNumber());
			return true;
		}
		if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{
			FloatProp->SetPropertyValue(PropAddr, (float)Value->AsNumber());
			return true;
		}
		if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{
			DoubleProp->SetPropertyValue(PropAddr, Value->AsNumber());
			return true;
		}

		// String types
		if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			StrProp->SetPropertyValue(PropAddr, Value->AsString());
			return true;
		}
		if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			NameProp->SetPropertyValue(PropAddr, FName(*Value->AsString()));
			return true;
		}
		if (FTextProperty* TextProp = CastField<FTextProperty>(Prop))
		{
			TextProp->SetPropertyValue(PropAddr, FText::FromString(Value->AsString()));
			return true;
		}

		// Enum (byte)
		if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (ByteProp->Enum)
			{
				int64 EnumVal = ByteProp->Enum->GetValueByNameString(Value->AsString());
				if (EnumVal == INDEX_NONE)
				{
					ByteProp->SetPropertyValue(PropAddr, (uint8)Value->AsNumber());
				}
				else
				{
					ByteProp->SetPropertyValue(PropAddr, (uint8)EnumVal);
				}
			}
			else
			{
				ByteProp->SetPropertyValue(PropAddr, (uint8)Value->AsNumber());
			}
			return true;
		}
		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			UEnum* Enum = EnumProp->GetEnum();
			if (Enum)
			{
				int64 EnumVal = Enum->GetValueByNameString(Value->AsString());
				if (EnumVal != INDEX_NONE)
				{
					FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
					UnderlyingProp->SetIntPropertyValue(PropAddr, EnumVal);
					return true;
				}
			}
			OutError = FString::Printf(TEXT("Could not resolve enum value '%s' for property '%s'"), *Value->AsString(), *PropName);
			return false;
		}

		// Struct — check for AI data providers first, then try ImportText
		if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			// FAIDataProviderFloatValue / FAIDataProviderBoolValue may arrive here
			// if the first struct block didn't catch them (e.g. struct name mismatch)
			FName StructName = StructProp->Struct->GetFName();

			// Check for float/bool wrapper structs by name or inheritance
			// UE 5.7 uses ValueOrBBKey_Float/Bool; older versions use FAIDataProviderFloatValue/BoolValue
			FName LeafName = StructProp->Struct->GetFName();
			bool bIsFloatWrapper = (LeafName == FName(TEXT("AIDataProviderFloatValue"))
				|| LeafName == FName(TEXT("ValueOrBBKey_Float")));
			bool bIsBoolWrapper = (LeafName == FName(TEXT("AIDataProviderBoolValue"))
				|| LeafName == FName(TEXT("ValueOrBBKey_Bool")));

			// Also walk inheritance chain as fallback
			if (!bIsFloatWrapper && !bIsBoolWrapper)
			{
				for (const UScriptStruct* S = StructProp->Struct; S; S = Cast<UScriptStruct>(S->GetSuperStruct()))
				{
					FName SName = S->GetFName();
					if (SName == FName(TEXT("AIDataProviderFloatValue")) || SName == FName(TEXT("ValueOrBBKey_Float")))
					{ bIsFloatWrapper = true; break; }
					if (SName == FName(TEXT("AIDataProviderBoolValue")) || SName == FName(TEXT("ValueOrBBKey_Bool")))
					{ bIsBoolWrapper = true; break; }
				}
			}

			if (bIsFloatWrapper)
			{
				// Phase D3: handle struct-literal string form before AsNumber() atod-zeros it.
				if (Value->Type == EJson::String)
				{
					FString StrVal = Value->AsString();
					if (StrVal.TrimStartAndEnd().StartsWith(TEXT("(")))
					{
						const TCHAR* Buffer = *StrVal;
						if (StructProp->ImportText_Direct(Buffer, PropAddr, Obj, PPF_None) != nullptr)
						{
							return true;
						}
					}
				}

				FProperty* DVP = StructProp->Struct->FindPropertyByName(TEXT("DefaultValue"));
				if (DVP)
				{
					void* DVAddr = DVP->ContainerPtrToValuePtr<void>(PropAddr);
					if (FFloatProperty* FP = CastField<FFloatProperty>(DVP))
					{
						FP->SetPropertyValue(DVAddr, (float)Value->AsNumber());
						return true;
					}
				}
			}
			if (bIsBoolWrapper)
			{
				// Phase D3: handle struct-literal string form before AsBool().
				if (Value->Type == EJson::String)
				{
					FString StrVal = Value->AsString();
					if (StrVal.TrimStartAndEnd().StartsWith(TEXT("(")))
					{
						const TCHAR* Buffer = *StrVal;
						if (StructProp->ImportText_Direct(Buffer, PropAddr, Obj, PPF_None) != nullptr)
						{
							return true;
						}
					}
				}

				FProperty* DVP = StructProp->Struct->FindPropertyByName(TEXT("DefaultValue"));
				if (DVP)
				{
					void* DVAddr = DVP->ContainerPtrToValuePtr<void>(PropAddr);
					if (FBoolProperty* BP2 = CastField<FBoolProperty>(DVP))
					{
						BP2->SetPropertyValue(DVAddr, Value->AsBool());
						return true;
					}
				}
			}

			// Generic struct import
			// Phase D3: guard against EJson::Number — AsString() can't coerce a numeric value to a struct literal.
			if (Value->Type == EJson::Number)
			{
				OutError = FString::Printf(TEXT(
					"Cannot set struct property '%s' (struct: %s) from a JSON number — pass a struct literal "
					"string like \"(Field=Value)\" or, if this struct wraps a single numeric, use the wrapper-aware path."),
					*PropName, *StructName.ToString());
				return false;
			}
			FString StructStr = Value->AsString();
			const TCHAR* Buffer = *StructStr;
			if (StructProp->ImportText_Direct(Buffer, PropAddr, Obj, PPF_None) != nullptr)
			{
				return true;
			}
			OutError = FString::Printf(TEXT("Failed to import text for struct property '%s' (struct: %s)"), *PropName, *StructName.ToString());
			return false;
		}

		// Object reference
		if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			FString ObjPath = Value->AsString();
			if (ObjPath.IsEmpty())
			{
				ObjProp->SetPropertyValue(PropAddr, nullptr);
				return true;
			}
			UObject* RefObj = FMonolithAssetUtils::LoadAssetByPath(ObjProp->PropertyClass, ObjPath);
			if (!RefObj)
			{
				OutError = FString::Printf(TEXT("Object not found: %s"), *ObjPath);
				return false;
			}
			ObjProp->SetPropertyValue(PropAddr, RefObj);
			return true;
		}

		OutError = FString::Printf(TEXT("Unsupported property type for '%s': %s"), *PropName, *Prop->GetClass()->GetName());
		return false;
	}

	/** Apply a JSON object of property_name→value pairs to a UObject. */
	void ApplyProperties(UObject* Obj, const TSharedPtr<FJsonObject>& PropsObj, UBehaviorTree* BT, TArray<FString>& OutErrors)
	{
		if (!Obj || !PropsObj.IsValid()) return;

		for (const auto& Pair : PropsObj->Values)
		{
			FString Error;
			if (!SetPropertyValue(Obj, Pair.Key, Pair.Value, BT, Error))
			{
				OutErrors.Add(Error);
			}
		}
	}

	/** Serialize all UPROPERTYs of a UObject to JSON. */
	TSharedPtr<FJsonObject> SerializeObjectProperties(UObject* Obj)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Obj) return Result;

		for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
		{
			FProperty* Prop = *It;
			// Skip editor-only / transient metadata where not useful
			if (Prop->HasAnyPropertyFlags(CPF_Deprecated)) continue;

			void* PropAddr = Prop->ContainerPtrToValuePtr<void>(Obj);
			const FString PropName = Prop->GetName();

			// FBlackboardKeySelector special case
			if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				if (StructProp->Struct->GetFName() == FName(TEXT("BlackboardKeySelector")))
				{
					const FBlackboardKeySelector* KeySel = reinterpret_cast<const FBlackboardKeySelector*>(PropAddr);
					TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
					KeyObj->SetStringField(TEXT("selected_key_name"), KeySel->SelectedKeyName.ToString());
					KeyObj->SetStringField(TEXT("type"), TEXT("BlackboardKeySelector"));
					Result->SetObjectField(PropName, KeyObj);
					continue;
				}
			}

			// Bool
			if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
			{
				Result->SetBoolField(PropName, BoolProp->GetPropertyValue(PropAddr));
				continue;
			}
			// Int
			if (CastField<FIntProperty>(Prop))
			{
				Result->SetNumberField(PropName, *reinterpret_cast<int32*>(PropAddr));
				continue;
			}
			// Float
			if (CastField<FFloatProperty>(Prop))
			{
				Result->SetNumberField(PropName, *reinterpret_cast<float*>(PropAddr));
				continue;
			}
			if (CastField<FDoubleProperty>(Prop))
			{
				Result->SetNumberField(PropName, *reinterpret_cast<double*>(PropAddr));
				continue;
			}
			// String types
			if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
			{
				Result->SetStringField(PropName, StrProp->GetPropertyValue(PropAddr));
				continue;
			}
			if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
			{
				Result->SetStringField(PropName, NameProp->GetPropertyValue(PropAddr).ToString());
				continue;
			}
			if (FTextProperty* TextProp = CastField<FTextProperty>(Prop))
			{
				Result->SetStringField(PropName, TextProp->GetPropertyValue(PropAddr).ToString());
				continue;
			}
			// Byte/Enum
			if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
			{
				if (ByteProp->Enum)
				{
					Result->SetStringField(PropName, ByteProp->Enum->GetNameStringByValue(ByteProp->GetPropertyValue(PropAddr)));
				}
				else
				{
					Result->SetNumberField(PropName, ByteProp->GetPropertyValue(PropAddr));
				}
				continue;
			}
			if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
			{
				FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
				int64 Val = UnderlyingProp->GetSignedIntPropertyValue(PropAddr);
				UEnum* Enum = EnumProp->GetEnum();
				if (Enum)
				{
					Result->SetStringField(PropName, Enum->GetNameStringByValue(Val));
				}
				else
				{
					Result->SetNumberField(PropName, (double)Val);
				}
				continue;
			}
			// Object reference
			if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
			{
				UObject* RefObj = ObjProp->GetPropertyValue(PropAddr);
				Result->SetStringField(PropName, RefObj ? RefObj->GetPathName() : TEXT("None"));
				continue;
			}
			// Struct — export text
			if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				FString ExportedText;
				StructProp->ExportTextItem_Direct(ExportedText, PropAddr, nullptr, Obj, PPF_None);
				Result->SetStringField(PropName, ExportedText);
				continue;
			}
		}

		return Result;
	}

	/** Collect BT node classes of a given base type */
	void CollectNodeClasses(UClass* BaseClass, const FString& Category, TArray<TSharedPtr<FJsonValue>>& OutArr)
	{
		TArray<UClass*> DerivedClasses;
		GetDerivedClasses(BaseClass, DerivedClasses, /*bRecursive=*/true);

		for (UClass* Cls : DerivedClasses)
		{
			// Skip abstract, deprecated, or skeleton classes
			if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}

			// Skip editor-only graph node wrappers
			if (Cls->IsChildOf(UEdGraphNode::StaticClass()))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("class_name"), Cls->GetName());
			Entry->SetStringField(TEXT("display_name"), Cls->GetDisplayNameText().ToString());
			Entry->SetStringField(TEXT("category"), Category);

			// Get description from CDO if available
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

	/** Map a friendly type name (Selector, Sequence, etc.) or raw class name to a UClass*. */
	UClass* ResolveNodeClass(const FString& TypeName)
	{
		// Shorthand composite names
		if (TypeName == TEXT("Selector")) return UBTComposite_Selector::StaticClass();
		if (TypeName == TEXT("Sequence")) return UBTComposite_Sequence::StaticClass();
		if (TypeName == TEXT("SimpleParallel")) return UBTComposite_SimpleParallel::StaticClass();

		// Try direct lookup
		UClass* Cls = FindFirstObject<UClass>(*TypeName, EFindFirstObjectOptions::EnsureIfAmbiguous);
		if (!Cls)
		{
			Cls = FindFirstObject<UClass>(*(FString(TEXT("U")) + TypeName), EFindFirstObjectOptions::EnsureIfAmbiguous);
		}
		return Cls;
	}

	/** Create an inline blackboard from a spec's "blackboard" object. Returns BB asset or nullptr. */
	UBlackboardData* CreateInlineBB(const FString& BTSavePath, const TSharedPtr<FJsonObject>& BBSpec, FString& OutError)
	{
		// Derive a BB path from the BT path
		FString BBPath = BTSavePath;
		FString BTName = FPackageName::GetShortName(BBPath);
		// Replace BT_ prefix with BB_ if present, otherwise append _BB
		FString BBName = BTName;
		if (BBName.StartsWith(TEXT("BT_")))
		{
			BBName = TEXT("BB_") + BBName.Mid(3);
		}
		else
		{
			BBName = BBName + TEXT("_BB");
		}
		// Build package path by replacing the last segment
		int32 LastSlash;
		if (BBPath.FindLastChar(TEXT('/'), LastSlash))
		{
			BBPath = BBPath.Left(LastSlash + 1) + BBName;
		}
		else
		{
			BBPath = BBPath + TEXT("_BB");
		}

		FString PkgError;
		UPackage* BBPackage = MonolithAI::GetOrCreatePackage(BBPath, PkgError);
		if (!BBPackage)
		{
			OutError = FString::Printf(TEXT("Failed to create package for inline BB: %s"), *PkgError);
			return nullptr;
		}

		UBlackboardData* BB = NewObject<UBlackboardData>(BBPackage, *BBName, RF_Public | RF_Standalone);
		if (!BB)
		{
			OutError = TEXT("Failed to create UBlackboardData object");
			return nullptr;
		}

		// Add keys
		const TArray<TSharedPtr<FJsonValue>>* KeysArr;
		if (BBSpec->TryGetArrayField(TEXT("keys"), KeysArr))
		{
			for (const TSharedPtr<FJsonValue>& KeyVal : *KeysArr)
			{
				const TSharedPtr<FJsonObject>* KeyObj;
				if (!KeyVal->TryGetObject(KeyObj)) continue;

				FString KName = (*KeyObj)->GetStringField(TEXT("name"));
				FString KType = (*KeyObj)->GetStringField(TEXT("type"));
				if (KName.IsEmpty() || KType.IsEmpty()) continue;

				UBlackboardKeyType* KeyType = nullptr;

				if (KType == TEXT("Bool")) KeyType = NewObject<UBlackboardKeyType_Bool>(BB);
				else if (KType == TEXT("Int")) KeyType = NewObject<UBlackboardKeyType_Int>(BB);
				else if (KType == TEXT("Float")) KeyType = NewObject<UBlackboardKeyType_Float>(BB);
				else if (KType == TEXT("String")) KeyType = NewObject<UBlackboardKeyType_String>(BB);
				else if (KType == TEXT("Name")) KeyType = NewObject<UBlackboardKeyType_Name>(BB);
				else if (KType == TEXT("Vector")) KeyType = NewObject<UBlackboardKeyType_Vector>(BB);
				else if (KType == TEXT("Rotator")) KeyType = NewObject<UBlackboardKeyType_Rotator>(BB);
				else if (KType == TEXT("Object"))
				{
					UBlackboardKeyType_Object* ObjKey = NewObject<UBlackboardKeyType_Object>(BB);
					FString BaseClassName = (*KeyObj)->GetStringField(TEXT("base_class"));
					if (!BaseClassName.IsEmpty())
					{
						UClass* BaseClass = FindFirstObject<UClass>(*BaseClassName, EFindFirstObjectOptions::EnsureIfAmbiguous);
						if (!BaseClass)
						{
							BaseClass = FindFirstObject<UClass>(*(FString(TEXT("A")) + BaseClassName), EFindFirstObjectOptions::EnsureIfAmbiguous);
						}
						if (BaseClass)
						{
							ObjKey->BaseClass = BaseClass;
						}
					}
					KeyType = ObjKey;
				}
				else if (KType == TEXT("Class"))
				{
					KeyType = NewObject<UBlackboardKeyType_Class>(BB);
				}
				else if (KType == TEXT("Enum"))
				{
					FString EnumName = (*KeyObj)->GetStringField(TEXT("enum_type"));
					UEnum* FoundEnum = nullptr;
					if (!EnumName.IsEmpty())
					{
						FoundEnum = FindFirstObject<UEnum>(*EnumName, EFindFirstObjectOptions::EnsureIfAmbiguous);
					}
					if (FoundEnum)
					{
						UBlackboardKeyType_Enum* EnumKey = NewObject<UBlackboardKeyType_Enum>(BB);
						EnumKey->EnumType = FoundEnum;
						KeyType = EnumKey;
					}
					else
					{
						KeyType = NewObject<UBlackboardKeyType_Enum>(BB);
					}
				}

				if (KeyType)
				{
					FBlackboardEntry NewEntry;
					NewEntry.EntryName = *KName;
					NewEntry.KeyType = KeyType;
					BB->Keys.Add(NewEntry);
				}
			}
		}

		BB->UpdateKeyIDs();
		FAssetRegistryModule::AssetCreated(BB);
		BB->MarkPackageDirty();

		return BB;
	}

	struct FSpecBuildContext
	{
		UBehaviorTree* BT;
		UBehaviorTreeGraph* BTGraph;
		int32 NodeCount;
		TArray<FString> Warnings;
	};

	/**
	 * Recursively build a BT node from a spec JSON node.
	 * Returns the created graph node (composite or task), or nullptr on failure.
	 */
	UBehaviorTreeGraphNode* BuildNodeFromSpec(
		const TSharedPtr<FJsonObject>& NodeSpec,
		UBehaviorTreeGraphNode* ParentGraphNode,
		int32 ChildIndex,
		int32 Depth,
		FSpecBuildContext& Ctx)
	{
		if (!NodeSpec.IsValid()) return nullptr;

		FString TypeName = NodeSpec->GetStringField(TEXT("type"));
		if (TypeName.IsEmpty())
		{
			Ctx.Warnings.Add(TEXT("Node missing 'type' field, skipped"));
			return nullptr;
		}

		// Resolve the BT node class
		UClass* BTNodeClass = ResolveNodeClass(TypeName);
		if (!BTNodeClass)
		{
			Ctx.Warnings.Add(FString::Printf(TEXT("Unknown node type '%s', skipped"), *TypeName));
			return nullptr;
		}

		// Validate it's a composite or task
		bool bIsComposite = BTNodeClass->IsChildOf(UBTCompositeNode::StaticClass());
		bool bIsTask = BTNodeClass->IsChildOf(UBTTaskNode::StaticClass());
		if (!bIsComposite && !bIsTask)
		{
			Ctx.Warnings.Add(FString::Printf(TEXT("'%s' is not a composite or task class, skipped"), *TypeName));
			return nullptr;
		}

		// Determine graph node class
		UClass* GraphNodeClass = GetGraphNodeClassForBTNode(BTNodeClass);
		if (!GraphNodeClass) return nullptr;

		// Create the graph node
		UBehaviorTreeGraphNode* NewGraphNode = NewObject<UBehaviorTreeGraphNode>(Ctx.BTGraph, GraphNodeClass);
		NewGraphNode->SetFlags(RF_Transactional);
		Ctx.BTGraph->AddNode(NewGraphNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
		NewGraphNode->CreateNewGuid();
		NewGraphNode->AllocateDefaultPins();

		// Create the BT node instance
		UBTNode* NewBTNode = NewObject<UBTNode>(NewGraphNode, BTNodeClass);
		NewGraphNode->NodeInstance = NewBTNode;

		// Set custom name if provided
		FString NodeName = NodeSpec->GetStringField(TEXT("name"));
		if (!NodeName.IsEmpty())
		{
			NewBTNode->NodeName = NodeName;
		}

		// Position: simple depth*300 for Y, child_index*250 for X
		if (ParentGraphNode)
		{
			NewGraphNode->NodePosX = ParentGraphNode->NodePosX + (ChildIndex * 250);
			NewGraphNode->NodePosY = ParentGraphNode->NodePosY + 200;
		}
		else
		{
			NewGraphNode->NodePosX = Depth * 300;
			NewGraphNode->NodePosY = ChildIndex * 250;
		}

		// Wire to parent
		if (ParentGraphNode)
		{
			ConnectParentChild(ParentGraphNode, NewGraphNode);
		}

		// Apply properties
		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		if (NodeSpec->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
		{
			TArray<FString> PropErrors;
			ApplyProperties(NewBTNode, *PropsObj, Ctx.BT, PropErrors);
			for (const FString& Err : PropErrors)
			{
				Ctx.Warnings.Add(FString::Printf(TEXT("Property warning on '%s': %s"), *TypeName, *Err));
			}
		}

		// Note: Do NOT call InitializeInstance() here — it can crash if the graph
		// isn't fully built yet. UpdateAsset() at the end handles initialization.
		Ctx.NodeCount++;

		// Add decorators
		const TArray<TSharedPtr<FJsonValue>>* DecoratorsArr;
		if (NodeSpec->TryGetArrayField(TEXT("decorators"), DecoratorsArr))
		{
			for (const TSharedPtr<FJsonValue>& DecVal : *DecoratorsArr)
			{
				const TSharedPtr<FJsonObject>* DecObj;
				if (!DecVal->TryGetObject(DecObj)) continue;

				FString DecClassName = (*DecObj)->GetStringField(TEXT("class"));
				if (DecClassName.IsEmpty()) continue;

				UClass* DecClass = FindFirstObject<UClass>(*DecClassName, EFindFirstObjectOptions::EnsureIfAmbiguous);
				if (!DecClass)
				{
					DecClass = FindFirstObject<UClass>(*(FString(TEXT("U")) + DecClassName), EFindFirstObjectOptions::EnsureIfAmbiguous);
				}
				if (!DecClass || !DecClass->IsChildOf(UBTDecorator::StaticClass()))
				{
					Ctx.Warnings.Add(FString::Printf(TEXT("Decorator class '%s' not found or invalid"), *DecClassName));
					continue;
				}

				UBehaviorTreeGraphNode_Decorator* DecGraphNode = NewObject<UBehaviorTreeGraphNode_Decorator>(Ctx.BTGraph);
				UBTDecorator* DecInstance = NewObject<UBTDecorator>(DecGraphNode, DecClass);
				DecGraphNode->NodeInstance = DecInstance;
				NewGraphNode->AddSubNode(DecGraphNode, Ctx.BTGraph);

				const TSharedPtr<FJsonObject>* DecPropsObj = nullptr;
				if ((*DecObj)->TryGetObjectField(TEXT("properties"), DecPropsObj) && DecPropsObj && (*DecPropsObj).IsValid())
				{
					TArray<FString> DecPropErrors;
					ApplyProperties(DecInstance, *DecPropsObj, Ctx.BT, DecPropErrors);
					for (const FString& Err : DecPropErrors)
					{
						Ctx.Warnings.Add(FString::Printf(TEXT("Decorator '%s' property: %s"), *DecClassName, *Err));
					}
				}
			}
		}

		// Add services
		const TArray<TSharedPtr<FJsonValue>>* ServicesArr;
		if (NodeSpec->TryGetArrayField(TEXT("services"), ServicesArr))
		{
			for (const TSharedPtr<FJsonValue>& SvcVal : *ServicesArr)
			{
				const TSharedPtr<FJsonObject>* SvcObj;
				if (!SvcVal->TryGetObject(SvcObj)) continue;

				FString SvcClassName = (*SvcObj)->GetStringField(TEXT("class"));
				if (SvcClassName.IsEmpty()) continue;

				UClass* SvcClass = FindFirstObject<UClass>(*SvcClassName, EFindFirstObjectOptions::EnsureIfAmbiguous);
				if (!SvcClass)
				{
					SvcClass = FindFirstObject<UClass>(*(FString(TEXT("U")) + SvcClassName), EFindFirstObjectOptions::EnsureIfAmbiguous);
				}
				if (!SvcClass || !SvcClass->IsChildOf(UBTService::StaticClass()))
				{
					Ctx.Warnings.Add(FString::Printf(TEXT("Service class '%s' not found or invalid"), *SvcClassName));
					continue;
				}

				UBehaviorTreeGraphNode_Service* SvcGraphNode = NewObject<UBehaviorTreeGraphNode_Service>(Ctx.BTGraph);
				UBTService* SvcInstance = NewObject<UBTService>(SvcGraphNode, SvcClass);
				SvcGraphNode->NodeInstance = SvcInstance;
				NewGraphNode->AddSubNode(SvcGraphNode, Ctx.BTGraph);

				const TSharedPtr<FJsonObject>* SvcPropsObj = nullptr;
				if ((*SvcObj)->TryGetObjectField(TEXT("properties"), SvcPropsObj) && SvcPropsObj && (*SvcPropsObj).IsValid())
				{
					TArray<FString> SvcPropErrors;
					ApplyProperties(SvcInstance, *SvcPropsObj, Ctx.BT, SvcPropErrors);
					for (const FString& Err : SvcPropErrors)
					{
						Ctx.Warnings.Add(FString::Printf(TEXT("Service '%s' property: %s"), *SvcClassName, *Err));
					}
				}
			}
		}

		// Recurse into children (only valid for composites)
		const TArray<TSharedPtr<FJsonValue>>* ChildrenArr;
		if (NodeSpec->TryGetArrayField(TEXT("children"), ChildrenArr))
		{
			if (!bIsComposite)
			{
				Ctx.Warnings.Add(FString::Printf(TEXT("Task node '%s' has children — ignored"), *TypeName));
			}
			else
			{
				for (int32 i = 0; i < ChildrenArr->Num(); ++i)
				{
					const TSharedPtr<FJsonObject>* ChildObj;
					if (!(*ChildrenArr)[i]->TryGetObject(ChildObj)) continue;
					BuildNodeFromSpec(*ChildObj, NewGraphNode, i, Depth + 1, Ctx);
				}
			}
		}

		return NewGraphNode;
	}

	/** Recursively export a BT graph node to the spec JSON format. */
	TSharedPtr<FJsonObject> ExportNodeToSpec(const UBehaviorTreeGraphNode* GraphNode)
	{
		if (!GraphNode) return nullptr;

		UBTNode* BTNode = Cast<UBTNode>(GraphNode->NodeInstance);
		if (!BTNode) return nullptr;

		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();

		// Type — use shorthand for known composites
		FString ClassName = BTNode->GetClass()->GetName();
		if (ClassName == TEXT("BTComposite_Selector")) NodeObj->SetStringField(TEXT("type"), TEXT("Selector"));
		else if (ClassName == TEXT("BTComposite_Sequence")) NodeObj->SetStringField(TEXT("type"), TEXT("Sequence"));
		else if (ClassName == TEXT("BTComposite_SimpleParallel")) NodeObj->SetStringField(TEXT("type"), TEXT("SimpleParallel"));
		else NodeObj->SetStringField(TEXT("type"), ClassName);

		// Name
		FString NodeName = BTNode->GetNodeName();
		if (!NodeName.IsEmpty())
		{
			NodeObj->SetStringField(TEXT("name"), NodeName);
		}

		// Properties (only non-default ones would be ideal, but serialize all for completeness)
		TSharedPtr<FJsonObject> Props = SerializeObjectProperties(BTNode);
		if (Props.IsValid() && Props->Values.Num() > 0)
		{
			NodeObj->SetObjectField(TEXT("properties"), Props);
		}

		// Decorators
		if (GraphNode->Decorators.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> DecArr;
			for (const UBehaviorTreeGraphNode* DecNode : GraphNode->Decorators)
			{
				if (!DecNode) continue;
				UBTNode* DecBTNode = Cast<UBTNode>(DecNode->NodeInstance);
				if (!DecBTNode) continue;

				TSharedPtr<FJsonObject> DecObj = MakeShared<FJsonObject>();
				DecObj->SetStringField(TEXT("class"), DecBTNode->GetClass()->GetName());
				TSharedPtr<FJsonObject> DecProps = SerializeObjectProperties(DecBTNode);
				if (DecProps.IsValid() && DecProps->Values.Num() > 0)
				{
					DecObj->SetObjectField(TEXT("properties"), DecProps);
				}
				DecArr.Add(MakeShared<FJsonValueObject>(DecObj));
			}
			NodeObj->SetArrayField(TEXT("decorators"), DecArr);
		}

		// Services
		if (GraphNode->Services.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> SvcArr;
			for (const UBehaviorTreeGraphNode* SvcNode : GraphNode->Services)
			{
				if (!SvcNode) continue;
				UBTNode* SvcBTNode = Cast<UBTNode>(SvcNode->NodeInstance);
				if (!SvcBTNode) continue;

				TSharedPtr<FJsonObject> SvcObj = MakeShared<FJsonObject>();
				SvcObj->SetStringField(TEXT("class"), SvcBTNode->GetClass()->GetName());
				TSharedPtr<FJsonObject> SvcProps = SerializeObjectProperties(SvcBTNode);
				if (SvcProps.IsValid() && SvcProps->Values.Num() > 0)
				{
					SvcObj->SetObjectField(TEXT("properties"), SvcProps);
				}
				SvcArr.Add(MakeShared<FJsonValueObject>(SvcObj));
			}
			NodeObj->SetArrayField(TEXT("services"), SvcArr);
		}

		// Children — walk output pins
		TArray<UBehaviorTreeGraphNode*> Children = GetChildNodes(const_cast<UBehaviorTreeGraphNode*>(GraphNode));
		if (Children.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ChildArr;
			for (UBehaviorTreeGraphNode* Child : Children)
			{
				TSharedPtr<FJsonObject> ChildSpec = ExportNodeToSpec(Child);
				if (ChildSpec.IsValid())
				{
					ChildArr.Add(MakeShared<FJsonValueObject>(ChildSpec));
				}
			}
			NodeObj->SetArrayField(TEXT("children"), ChildArr);
		}

		return NodeObj;
	}

	/** Collect all BB key names referenced by FBlackboardKeySelector properties in a BT node. */
	void CollectBBKeyReferences(UBTNode* BTNode, TArray<FString>& OutKeys)
	{
		if (!BTNode) return;
		for (TFieldIterator<FStructProperty> It(BTNode->GetClass()); It; ++It)
		{
			if (It->Struct->GetFName() == FName(TEXT("BlackboardKeySelector")))
			{
				void* PropAddr = It->ContainerPtrToValuePtr<void>(BTNode);
				const FBlackboardKeySelector* KeySel = reinterpret_cast<const FBlackboardKeySelector*>(PropAddr);
				if (!KeySel->SelectedKeyName.IsNone())
				{
					OutKeys.AddUnique(KeySel->SelectedKeyName.ToString());
				}
			}
		}
	}

	/** Recursively validate a BT graph node. */
	void ValidateGraphNode(
		const UBehaviorTreeGraphNode* GraphNode,
		const TSet<FName>& ValidBBKeys,
		TArray<TSharedPtr<FJsonValue>>& OutIssues,
		TSet<const UBehaviorTreeGraphNode*>& Visited)
	{
		if (!GraphNode || Visited.Contains(GraphNode)) return;
		Visited.Add(GraphNode);

		UBTNode* BTNode = Cast<UBTNode>(GraphNode->NodeInstance);

		// Check empty composite
		if (BTNode && BTNode->IsA<UBTCompositeNode>())
		{
			TArray<UBehaviorTreeGraphNode*> Children = GetChildNodes(const_cast<UBehaviorTreeGraphNode*>(GraphNode));
			if (Children.Num() == 0)
			{
				TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("severity"), TEXT("warning"));
				Issue->SetStringField(TEXT("node_id"), GraphNode->NodeGuid.ToString());
				Issue->SetStringField(TEXT("node_class"), BTNode->GetClass()->GetName());
				Issue->SetStringField(TEXT("message"), TEXT("Empty composite node — has no children"));
				OutIssues.Add(MakeShared<FJsonValueObject>(Issue));
			}

			// Recurse children
			for (UBehaviorTreeGraphNode* Child : Children)
			{
				ValidateGraphNode(Child, ValidBBKeys, OutIssues, Visited);
			}
		}

		// Check BB key references on the node itself
		if (BTNode && ValidBBKeys.Num() > 0)
		{
			TArray<FString> ReferencedKeys;
			CollectBBKeyReferences(BTNode, ReferencedKeys);
			for (const FString& Key : ReferencedKeys)
			{
				if (!ValidBBKeys.Contains(FName(*Key)))
				{
					TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("severity"), TEXT("error"));
					Issue->SetStringField(TEXT("node_id"), GraphNode->NodeGuid.ToString());
					Issue->SetStringField(TEXT("node_class"), BTNode->GetClass()->GetName());
					Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("References non-existent BB key: '%s'"), *Key));
					OutIssues.Add(MakeShared<FJsonValueObject>(Issue));
				}
			}
		}

		// Check decorators
		for (const UBehaviorTreeGraphNode* DecNode : GraphNode->Decorators)
		{
			if (!DecNode) continue;
			UBTNode* DecBTNode = Cast<UBTNode>(DecNode->NodeInstance);
			if (DecBTNode && ValidBBKeys.Num() > 0)
			{
				TArray<FString> DecKeys;
				CollectBBKeyReferences(DecBTNode, DecKeys);
				for (const FString& Key : DecKeys)
				{
					if (!ValidBBKeys.Contains(FName(*Key)))
					{
						TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
						Issue->SetStringField(TEXT("severity"), TEXT("error"));
						Issue->SetStringField(TEXT("node_id"), DecNode->NodeGuid.ToString());
						Issue->SetStringField(TEXT("node_class"), DecBTNode->GetClass()->GetName());
						Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("Decorator references non-existent BB key: '%s'"), *Key));
						OutIssues.Add(MakeShared<FJsonValueObject>(Issue));
					}
				}
			}
		}

		// Check services
		for (const UBehaviorTreeGraphNode* SvcNode : GraphNode->Services)
		{
			if (!SvcNode) continue;
			UBTNode* SvcBTNode = Cast<UBTNode>(SvcNode->NodeInstance);
			if (SvcBTNode && ValidBBKeys.Num() > 0)
			{
				TArray<FString> SvcKeys;
				CollectBBKeyReferences(SvcBTNode, SvcKeys);
				for (const FString& Key : SvcKeys)
				{
					if (!ValidBBKeys.Contains(FName(*Key)))
					{
						TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
						Issue->SetStringField(TEXT("severity"), TEXT("error"));
						Issue->SetStringField(TEXT("node_id"), SvcNode->NodeGuid.ToString());
						Issue->SetStringField(TEXT("node_class"), SvcBTNode->GetClass()->GetName());
						Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("Service references non-existent BB key: '%s'"), *Key));
						OutIssues.Add(MakeShared<FJsonValueObject>(Issue));
					}
				}
			}
		}
	}
}

// ============================================================
//  Registration
// ============================================================

void FMonolithAIBehaviorTreeActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 13. create_behavior_tree
	Registry.RegisterAction(TEXT("ai"), TEXT("create_behavior_tree"),
		TEXT("Create a new Behavior Tree asset, optionally linking a Blackboard"),
		FMonolithActionHandler::CreateStatic(&HandleCreateBehaviorTree),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path (e.g. /Game/AI/BT_Enemy)"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Asset name (derived from save_path if omitted)"))
			.Optional(TEXT("blackboard_path"), TEXT("string"), TEXT("Blackboard asset path to link"))
			.Build());

	// 14. get_behavior_tree
	Registry.RegisterAction(TEXT("ai"), TEXT("get_behavior_tree"),
		TEXT("Full tree structure as JSON — nodes, decorators, services, hierarchy from root"),
		FMonolithActionHandler::CreateStatic(&HandleGetBehaviorTree),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Build());

	// 15. list_behavior_trees
	Registry.RegisterAction(TEXT("ai"), TEXT("list_behavior_trees"),
		TEXT("List all UBehaviorTree assets in the project"),
		FMonolithActionHandler::CreateStatic(&HandleListBehaviorTrees),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Only include assets under this path prefix"))
			.Build());

	// 16. delete_behavior_tree
	Registry.RegisterAction(TEXT("ai"), TEXT("delete_behavior_tree"),
		TEXT("Delete a Behavior Tree asset"),
		FMonolithActionHandler::CreateStatic(&HandleDeleteBehaviorTree),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path to delete"))
			.Build());

	// 17. duplicate_behavior_tree
	Registry.RegisterAction(TEXT("ai"), TEXT("duplicate_behavior_tree"),
		TEXT("Deep copy a Behavior Tree asset to a new path"),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateBehaviorTree),
		FParamSchemaBuilder()
			.Required(TEXT("source_path"), TEXT("string"), TEXT("Source BT asset path"))
			.Required(TEXT("dest_path"), TEXT("string"), TEXT("Destination asset path for the copy"))
			.Build());

	// 18. set_bt_blackboard
	Registry.RegisterAction(TEXT("ai"), TEXT("set_bt_blackboard"),
		TEXT("Set or change the Blackboard reference on a Behavior Tree"),
		FMonolithActionHandler::CreateStatic(&HandleSetBTBlackboard),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Required(TEXT("blackboard_path"), TEXT("string"), TEXT("Blackboard asset path to link (empty string to clear)"))
			.Build());

	// 29. list_bt_node_classes
	Registry.RegisterAction(TEXT("ai"), TEXT("list_bt_node_classes"),
		TEXT("List all available BT node classes (composites, tasks, decorators, services) with descriptions"),
		FMonolithActionHandler::CreateStatic(&HandleListBTNodeClasses),
		FParamSchemaBuilder()
			.Optional(TEXT("category"), TEXT("string"), TEXT("Filter: composite, task, decorator, or service"))
			.Build());

	// 19. add_bt_node
	Registry.RegisterAction(TEXT("ai"), TEXT("add_bt_node"),
		TEXT("Add a composite or task node to a Behavior Tree. parent_id=null adds under root."),
		FMonolithActionHandler::CreateStatic(&HandleAddBTNode),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Optional(TEXT("parent_id"), TEXT("string"), TEXT("GUID of parent composite node (null = root)"))
			.Required(TEXT("node_class"), TEXT("string"), TEXT("BT node class name (e.g. BTTask_Wait, BTComposite_Sequence)"))
			.Optional(TEXT("index"), TEXT("number"), TEXT("Child index under parent (-1 or omit = append)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("JSON object of property_name→value pairs to set"))
			.Build());

	// 20. remove_bt_node
	Registry.RegisterAction(TEXT("ai"), TEXT("remove_bt_node"),
		TEXT("Remove a node and its children from a Behavior Tree"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveBTNode),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("GUID of the node to remove"))
			.Build());

	// 21. move_bt_node
	Registry.RegisterAction(TEXT("ai"), TEXT("move_bt_node"),
		TEXT("Reparent a node under a different composite in the Behavior Tree"),
		FMonolithActionHandler::CreateStatic(&HandleMoveBTNode),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("GUID of the node to move"))
			.Required(TEXT("new_parent_id"), TEXT("string"), TEXT("GUID of the new parent composite node"))
			.Optional(TEXT("index"), TEXT("number"), TEXT("Child index under new parent (-1 or omit = append)"))
			.Build());

	// 22. add_bt_decorator
	Registry.RegisterAction(TEXT("ai"), TEXT("add_bt_decorator"),
		TEXT("Add a decorator as a sub-node on a target BT node"),
		FMonolithActionHandler::CreateStatic(&HandleAddBTDecorator),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("GUID of the target node"))
			.Required(TEXT("decorator_class"), TEXT("string"), TEXT("Decorator class name (e.g. BTDecorator_Blackboard)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("JSON object of property_name→value pairs to set"))
			.Build());

	// 23. remove_bt_decorator
	Registry.RegisterAction(TEXT("ai"), TEXT("remove_bt_decorator"),
		TEXT("Remove a decorator from a BT node by index"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveBTDecorator),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("GUID of the node that owns the decorator"))
			.Required(TEXT("decorator_index"), TEXT("number"), TEXT("Index of the decorator to remove"))
			.Build());

	// 24. add_bt_service
	Registry.RegisterAction(TEXT("ai"), TEXT("add_bt_service"),
		TEXT("Add a service as a sub-node on a composite or task node"),
		FMonolithActionHandler::CreateStatic(&HandleAddBTService),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("GUID of the target node"))
			.Required(TEXT("service_class"), TEXT("string"), TEXT("Service class name (e.g. BTService_DefaultFocus)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("JSON object of property_name→value pairs to set"))
			.Build());

	// 25. remove_bt_service
	Registry.RegisterAction(TEXT("ai"), TEXT("remove_bt_service"),
		TEXT("Remove a service from a BT node by index"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveBTService),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("GUID of the node that owns the service"))
			.Required(TEXT("service_index"), TEXT("number"), TEXT("Index of the service to remove"))
			.Build());

	// 26. set_bt_node_property
	Registry.RegisterAction(TEXT("ai"), TEXT("set_bt_node_property"),
		TEXT("Set a UPROPERTY on a BT node instance. Special handling for FBlackboardKeySelector."),
		FMonolithActionHandler::CreateStatic(&HandleSetBTNodeProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("GUID of the node"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("UPROPERTY name to set"))
			.Required(TEXT("value"), TEXT("any"), TEXT("Value to set (type depends on property)"))
			.Build());

	// 27. get_bt_node_properties
	Registry.RegisterAction(TEXT("ai"), TEXT("get_bt_node_properties"),
		TEXT("Read all UPROPERTYs from a BT node instance"),
		FMonolithActionHandler::CreateStatic(&HandleGetBTNodeProperties),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("GUID of the node"))
			.Build());

	// 28. reorder_bt_children
	Registry.RegisterAction(TEXT("ai"), TEXT("reorder_bt_children"),
		TEXT("Reorder child nodes under a composite by specifying new GUID order"),
		FMonolithActionHandler::CreateStatic(&HandleReorderBTChildren),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Required(TEXT("parent_id"), TEXT("string"), TEXT("GUID of the parent composite node"))
			.Required(TEXT("new_order"), TEXT("array"), TEXT("Array of child node GUIDs in desired order"))
			.Build());

	// 30. add_bt_run_eqs_task
	Registry.RegisterAction(TEXT("ai"), TEXT("add_bt_run_eqs_task"),
		TEXT("Convenience: add a fully configured RunEQSQuery task node"),
		FMonolithActionHandler::CreateStatic(&HandleAddBTRunEQSTask),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Optional(TEXT("parent_id"), TEXT("string"), TEXT("GUID of parent composite node (null = root)"))
			.Required(TEXT("eqs_path"), TEXT("string"), TEXT("EQS query asset path"))
			.Required(TEXT("bb_result_key"), TEXT("string"), TEXT("Blackboard key to store result"))
			.Optional(TEXT("run_mode"), TEXT("string"), TEXT("EEnvQueryRunMode (SingleBestItem, AllMatching, etc.)"))
			.Build());

	// 31. add_bt_smart_object_task
	Registry.RegisterAction(TEXT("ai"), TEXT("add_bt_smart_object_task"),
		TEXT("Convenience: add a FindAndUseSmartObject task node"),
		FMonolithActionHandler::CreateStatic(&HandleAddBTSmartObjectTask),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Optional(TEXT("parent_id"), TEXT("string"), TEXT("GUID of parent composite node (null = root)"))
			.Required(TEXT("activity_tags"), TEXT("string"), TEXT("Gameplay tag query for SO activities (e.g. Activity.Sit)"))
			.Optional(TEXT("search_radius"), TEXT("number"), TEXT("Search radius for smart objects (default 5000)"))
			.Build());

	// 31b. add_bt_use_ability_task — Phase I2 (BT-to-GAS direct activation)
	// Drops a UBTTask_TryActivateAbility node into the target BT, configured
	// to fire a Gameplay Ability when reached at tick. K2 aliases support the
	// shorter `ability` / `tags` synonyms agents tend to emit.
	Registry.RegisterAction(TEXT("ai"), TEXT("add_bt_use_ability_task"),
		TEXT("Convenience: add a fully configured TryActivateAbility task node that fires a GAS ability on tick"),
		FMonolithActionHandler::CreateStatic(&HandleAddBTUseAbilityTask),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Optional(TEXT("parent_id"), TEXT("string"), TEXT("GUID of parent composite node (null = root)"))
			.Optional(TEXT("ability_class"), TEXT("string"),
				TEXT("Asset path or class name of UGameplayAbility subclass (mutually exclusive with ability_tags)"),
				{ TEXT("ability") })
			.Optional(TEXT("ability_tags"), TEXT("string"),
				TEXT("Comma-separated gameplay tags — activate first granted ability matching ALL (mutually exclusive with ability_class)"),
				{ TEXT("tags") })
			.Optional(TEXT("wait_for_end"), TEXT("boolean"),
				TEXT("If true, hold InProgress until ability ends (default true)"), TEXT("true"))
			.Optional(TEXT("succeed_on_blocked"), TEXT("boolean"),
				TEXT("If true, return Succeeded when activation is blocked (default false)"), TEXT("false"))
			.Optional(TEXT("event_tag"), TEXT("string"),
				TEXT("Optional gameplay tag — send a gameplay event with this tag after successful activation"))
			.Optional(TEXT("node_name"), TEXT("string"), TEXT("Override the BT node display name"))
			.Build());

	// 32. build_behavior_tree_from_spec
	Registry.RegisterAction(TEXT("ai"), TEXT("build_behavior_tree_from_spec"),
		TEXT("Create a complete Behavior Tree from a declarative JSON spec — the crown jewel"),
		FMonolithActionHandler::CreateStatic(&HandleBuildBTFromSpec),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path (e.g. /Game/AI/BT_Enemy)"))
			.Required(TEXT("spec"), TEXT("object"), TEXT("Full tree spec with root, children, decorators, services, properties"))
			.Optional(TEXT("strict_mode"), TEXT("boolean"), TEXT("If true, abort with error (no save) when any node fails to resolve"), TEXT("false"))
			.Build());

	// 33. export_bt_spec
	Registry.RegisterAction(TEXT("ai"), TEXT("export_bt_spec"),
		TEXT("Export an existing Behavior Tree as a JSON spec (inverse of build_behavior_tree_from_spec)"),
		FMonolithActionHandler::CreateStatic(&HandleExportBTSpec),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Build());

	// 34. import_bt_spec
	Registry.RegisterAction(TEXT("ai"), TEXT("import_bt_spec"),
		TEXT("Recreate a Behavior Tree from an exported spec (overwrites existing tree structure)"),
		FMonolithActionHandler::CreateStatic(&HandleImportBTSpec),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path (must exist)"))
			.Required(TEXT("spec"), TEXT("object"), TEXT("Full tree spec in export format"))
			.Build());

	// 37. validate_behavior_tree
	Registry.RegisterAction(TEXT("ai"), TEXT("validate_behavior_tree"),
		TEXT("Validate a Behavior Tree: check BB key refs, unreachable branches, empty composites, missing properties"),
		FMonolithActionHandler::CreateStatic(&HandleValidateBehaviorTree),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Build());

	// 35. clone_bt_subtree
	Registry.RegisterAction(TEXT("ai"), TEXT("clone_bt_subtree"),
		TEXT("Copy a subtree from one Behavior Tree to another (deep clone with decorators, services, properties)"),
		FMonolithActionHandler::CreateStatic(&HandleCloneBTSubtree),
		FParamSchemaBuilder()
			.Required(TEXT("source_path"), TEXT("string"), TEXT("Source Behavior Tree asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("GUID of the root node of the subtree to copy"))
			.Required(TEXT("dest_path"), TEXT("string"), TEXT("Destination Behavior Tree asset path"))
			.Optional(TEXT("dest_parent_id"), TEXT("string"), TEXT("GUID of parent node in dest (null = root)"))
			.Build());

	// 36. auto_arrange_bt
	Registry.RegisterAction(TEXT("ai"), TEXT("auto_arrange_bt"),
		TEXT("Auto-layout a Behavior Tree graph. Uses Blueprint Assist formatter if available, else depth/breadth positioning."),
		FMonolithActionHandler::CreateStatic(&HandleAutoArrangeBT),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Optional(TEXT("formatter"), TEXT("string"), TEXT("Formatter: default, blueprint_assist, builtin (default tries BA then builtin)"))
			.Build());

	// 38. compare_behavior_trees
	Registry.RegisterAction(TEXT("ai"), TEXT("compare_behavior_trees"),
		TEXT("Structural diff between two Behavior Trees: nodes added/removed/moved, property changes"),
		FMonolithActionHandler::CreateStatic(&HandleCompareBehaviorTrees),
		FParamSchemaBuilder()
			.Required(TEXT("path_a"), TEXT("string"), TEXT("First Behavior Tree asset path"))
			.Required(TEXT("path_b"), TEXT("string"), TEXT("Second Behavior Tree asset path"))
			.Build());

	// 39. create_bt_task_blueprint
	Registry.RegisterAction(TEXT("ai"), TEXT("create_bt_task_blueprint"),
		TEXT("Create a BTTask Blueprint (parent defaults to BTTaskNode)"),
		FMonolithActionHandler::CreateStatic(&HandleCreateBTTaskBlueprint),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path (e.g. /Game/AI/Tasks/BTTask_MyTask)"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Blueprint name"))
			.Optional(TEXT("parent_class"), TEXT("string"), TEXT("Parent class name (default: BTTask_BlueprintBase)"))
			.Build());

	// 40. create_bt_decorator_blueprint
	Registry.RegisterAction(TEXT("ai"), TEXT("create_bt_decorator_blueprint"),
		TEXT("Create a BTDecorator Blueprint (parent defaults to BTDecorator_BlueprintBase)"),
		FMonolithActionHandler::CreateStatic(&HandleCreateBTDecoratorBlueprint),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Blueprint name"))
			.Optional(TEXT("parent_class"), TEXT("string"), TEXT("Parent class name (default: BTDecorator_BlueprintBase)"))
			.Build());

	// 41. create_bt_service_blueprint
	Registry.RegisterAction(TEXT("ai"), TEXT("create_bt_service_blueprint"),
		TEXT("Create a BTService Blueprint (parent defaults to BTService_BlueprintBase)"),
		FMonolithActionHandler::CreateStatic(&HandleCreateBTServiceBlueprint),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Blueprint name"))
			.Optional(TEXT("parent_class"), TEXT("string"), TEXT("Parent class name (default: BTService_BlueprintBase)"))
			.Build());

	// 42. generate_bt_diagram
	Registry.RegisterAction(TEXT("ai"), TEXT("generate_bt_diagram"),
		TEXT("Generate a text diagram of a Behavior Tree (ASCII tree or Mermaid graph)"),
		FMonolithActionHandler::CreateStatic(&HandleGenerateBTDiagram),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Optional(TEXT("format"), TEXT("string"), TEXT("Output format: ascii (default) or mermaid"))
			.Build());

	// F8 (J-phase) — flat graph topology with parent_id + children GUIDs.
	// Distinct from get_behavior_tree (recursive nested tree). Used by
	// J2 §TC2.18 to look up a single node's GUID from the BT.
	Registry.RegisterAction(TEXT("ai"), TEXT("get_bt_graph"),
		TEXT("Flat node array with parent_id + children GUIDs. Use when you need to look up a node by ID without walking the full tree (vs get_behavior_tree which returns a nested tree)."),
		FMonolithActionHandler::CreateStatic(&HandleGetBTGraph),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Build());
}

// ============================================================
//  13. create_behavior_tree
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleCreateBehaviorTree(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult))
	{
		return ErrResult;
	}

	FString AssetName = Params->GetStringField(TEXT("name"));
	if (AssetName.IsEmpty())
	{
		AssetName = FPackageName::GetShortName(SavePath);
	}

	FString PackagePath = SavePath;
	if (FPackageName::GetShortName(PackagePath) == AssetName)
	{
		// save_path includes the asset name — use as-is
	}

	// Check path is free
	FString PathError;
	if (!MonolithAI::EnsureAssetPathFree(PackagePath, AssetName, PathError))
	{
		return FMonolithActionResult::Error(PathError);
	}

	FString PkgError;
	UPackage* Package = MonolithAI::GetOrCreatePackage(PackagePath, PkgError);
	if (!Package)
	{
		return FMonolithActionResult::Error(PkgError);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Create Behavior Tree")));

	UBehaviorTree* BT = NewObject<UBehaviorTree>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!BT)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UBehaviorTree object"));
	}

	// Optionally link blackboard
	FString BBPath = Params->GetStringField(TEXT("blackboard_path"));
	if (!BBPath.IsEmpty())
	{
		UBlackboardData* BB = Cast<UBlackboardData>(FMonolithAssetUtils::LoadAssetByPath(UBlackboardData::StaticClass(), BBPath));
		if (!BB)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Blackboard not found: %s"), *BBPath));
		}
		BT->BlackboardAsset = BB;
	}

	FAssetRegistryModule::AssetCreated(BT);
	BT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(PackagePath, TEXT("Behavior Tree created"));
	Result->SetStringField(TEXT("name"), AssetName);
	if (BT->BlackboardAsset)
	{
		Result->SetStringField(TEXT("blackboard"), BT->BlackboardAsset->GetPathName());
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  14. get_behavior_tree
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleGetBehaviorTree(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = MonolithAI::LoadBehaviorTreeFromParams(Params, AssetPath, Error);
	if (!BT)
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("name"), BT->GetName());

	if (BT->BlackboardAsset)
	{
		Result->SetStringField(TEXT("blackboard"), BT->BlackboardAsset->GetPathName());
	}

#if WITH_EDITORONLY_DATA
	// Walk the editor graph for full structure
	UBehaviorTreeGraph* BTGraph = Cast<UBehaviorTreeGraph>(BT->BTGraph);
	if (BTGraph)
	{
		// Find the root graph node
		UBehaviorTreeGraphNode_Root* RootGraphNode = nullptr;
		for (UEdGraphNode* Node : BTGraph->Nodes)
		{
			RootGraphNode = Cast<UBehaviorTreeGraphNode_Root>(Node);
			if (RootGraphNode) break;
		}

		if (RootGraphNode)
		{
			TSharedPtr<FJsonObject> TreeJson = SerializeBTGraphNode(RootGraphNode, 0);
			if (TreeJson.IsValid())
			{
				Result->SetObjectField(TEXT("tree"), TreeJson);
			}
		}

		Result->SetNumberField(TEXT("node_count"), BTGraph->Nodes.Num());
	}
	else
	{
		Result->SetStringField(TEXT("note"), TEXT("No editor graph available — BT may not have been opened in editor yet"));
	}
#else
	// Fallback: serialize from runtime RootNode
	if (BT->RootNode)
	{
		TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
		RootObj->SetStringField(TEXT("node_class"), BT->RootNode->GetClass()->GetName());
		RootObj->SetStringField(TEXT("display_name"), BT->RootNode->GetNodeName());
		RootObj->SetNumberField(TEXT("child_count"), BT->RootNode->Children.Num());
		Result->SetObjectField(TEXT("root_node"), RootObj);
	}
#endif

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  15. list_behavior_trees
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleListBehaviorTrees(const TSharedPtr<FJsonObject>& Params)
{
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetData> Assets;
	AR.GetAssetsByClass(UBehaviorTree::StaticClass()->GetClassPathName(), Assets);

	FString PathFilter = Params->GetStringField(TEXT("path_filter"));

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FAssetData& Asset : Assets)
	{
		FString ObjPath = Asset.GetObjectPathString();
		if (!PathFilter.IsEmpty() && !ObjPath.StartsWith(PathFilter))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("asset_path"), Asset.PackageName.ToString());
		Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());

		// Check for blackboard tag
		FAssetTagValueRef BBTag = Asset.TagsAndValues.FindTag(FName(TEXT("BlackboardAsset")));
		if (BBTag.IsSet())
		{
			Item->SetStringField(TEXT("blackboard"), BBTag.GetValue());
		}

		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("behavior_trees"), Items);
	Result->SetNumberField(TEXT("count"), Items.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  16. delete_behavior_tree
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleDeleteBehaviorTree(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("asset_path"), AssetPath, ErrResult))
	{
		return ErrResult;
	}

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(UBehaviorTree::StaticClass(), AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Behavior Tree not found: %s"), *AssetPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Delete Behavior Tree")));

	TArray<UObject*> ObjectsToDelete;
	ObjectsToDelete.Add(Asset);

	int32 NumDeleted = ObjectTools::ForceDeleteObjects(ObjectsToDelete, /*bShowConfirmation=*/false);
	if (NumDeleted == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to delete: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("message"), TEXT("Behavior Tree deleted"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  17. duplicate_behavior_tree
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleDuplicateBehaviorTree(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath, DestPath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("source_path"), SourcePath, ErrResult))
	{
		return ErrResult;
	}
	if (!MonolithAI::RequireStringParam(Params, TEXT("dest_path"), DestPath, ErrResult))
	{
		return ErrResult;
	}

	UBehaviorTree* SourceBT = Cast<UBehaviorTree>(FMonolithAssetUtils::LoadAssetByPath(UBehaviorTree::StaticClass(), SourcePath));
	if (!SourceBT)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source Behavior Tree not found: %s"), *SourcePath));
	}

	FString DestAssetName = FPackageName::GetShortName(DestPath);
	FString PkgError;
	UPackage* DestPackage = MonolithAI::GetOrCreatePackage(DestPath, PkgError);
	if (!DestPackage)
	{
		return FMonolithActionResult::Error(PkgError);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Duplicate Behavior Tree")));

	UBehaviorTree* NewBT = Cast<UBehaviorTree>(
		StaticDuplicateObject(SourceBT, DestPackage, *DestAssetName));
	if (!NewBT)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to duplicate '%s' to '%s'"), *SourcePath, *DestPath));
	}

	NewBT->SetFlags(RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(NewBT);
	NewBT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source_path"), SourcePath);
	Result->SetStringField(TEXT("asset_path"), DestPath);
	Result->SetStringField(TEXT("name"), NewBT->GetName());
	if (NewBT->BlackboardAsset)
	{
		Result->SetStringField(TEXT("blackboard"), NewBT->BlackboardAsset->GetPathName());
	}
	Result->SetStringField(TEXT("message"), TEXT("Behavior Tree duplicated"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  18. set_bt_blackboard
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleSetBTBlackboard(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = MonolithAI::LoadBehaviorTreeFromParams(Params, AssetPath, Error);
	if (!BT)
	{
		return FMonolithActionResult::Error(Error);
	}

	FString BBPath = Params->GetStringField(TEXT("blackboard_path"));

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Set BT Blackboard")));

	if (BBPath.IsEmpty())
	{
		// Clear blackboard
		BT->BlackboardAsset = nullptr;
	}
	else
	{
		UBlackboardData* BB = Cast<UBlackboardData>(FMonolithAssetUtils::LoadAssetByPath(UBlackboardData::StaticClass(), BBPath));
		if (!BB)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Blackboard not found: %s"), *BBPath));
		}
		BT->BlackboardAsset = BB;
	}

	BT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(AssetPath, TEXT("Blackboard reference updated"));
	if (BT->BlackboardAsset)
	{
		Result->SetStringField(TEXT("blackboard"), BT->BlackboardAsset->GetPathName());
	}
	else
	{
		Result->SetStringField(TEXT("blackboard"), TEXT("(cleared)"));
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  29. list_bt_node_classes
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleListBTNodeClasses(const TSharedPtr<FJsonObject>& Params)
{
	FString CategoryFilter = Params->GetStringField(TEXT("category")).ToLower();

	TArray<TSharedPtr<FJsonValue>> NodeClasses;

	if (CategoryFilter.IsEmpty() || CategoryFilter == TEXT("composite"))
	{
		CollectNodeClasses(UBTCompositeNode::StaticClass(), TEXT("composite"), NodeClasses);
	}
	if (CategoryFilter.IsEmpty() || CategoryFilter == TEXT("task"))
	{
		CollectNodeClasses(UBTTaskNode::StaticClass(), TEXT("task"), NodeClasses);
	}
	if (CategoryFilter.IsEmpty() || CategoryFilter == TEXT("decorator"))
	{
		CollectNodeClasses(UBTDecorator::StaticClass(), TEXT("decorator"), NodeClasses);
	}
	if (CategoryFilter.IsEmpty() || CategoryFilter == TEXT("service"))
	{
		CollectNodeClasses(UBTService::StaticClass(), TEXT("service"), NodeClasses);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("node_classes"), NodeClasses);
	Result->SetNumberField(TEXT("count"), NodeClasses.Num());
	if (!CategoryFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("category_filter"), CategoryFilter);
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  19. add_bt_node
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleAddBTNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString NodeClassName;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("node_class"), NodeClassName, ErrResult))
	{
		return ErrResult;
	}

	// Find the BT node class
	UClass* BTNodeClass = FindFirstObject<UClass>(*NodeClassName, EFindFirstObjectOptions::EnsureIfAmbiguous);
	if (!BTNodeClass)
	{
		// Try with U prefix
		BTNodeClass = FindFirstObject<UClass>(*(FString(TEXT("U")) + NodeClassName), EFindFirstObjectOptions::EnsureIfAmbiguous);
	}
	if (!BTNodeClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("BT node class not found: %s"), *NodeClassName));
	}

	// Validate it's a BT node type
	if (!BTNodeClass->IsChildOf(UBTCompositeNode::StaticClass()) && !BTNodeClass->IsChildOf(UBTTaskNode::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("'%s' is not a composite or task node class"), *NodeClassName));
	}

	// Find the parent graph node
	FString ParentIdStr = Params->GetStringField(TEXT("parent_id"));
	UBehaviorTreeGraphNode* ParentGraphNode = nullptr;

	if (ParentIdStr.IsEmpty())
	{
		// Use root
		ParentGraphNode = FindRootNode(BTGraph);
		if (!ParentGraphNode)
		{
			return FMonolithActionResult::Error(TEXT("Root node not found in BT graph"));
		}
	}
	else
	{
		FString GuidErr;
		if (!RequireBtNodeByGuid(BTGraph, ParentIdStr, TEXT("parent_id"), BT->GetName(), ParentGraphNode, GuidErr))
		{
			return FMonolithActionResult::Error(MoveTemp(GuidErr));
		}
	}

	// Phase F1: when adding a Task, the parent MUST be a composite. The Root edge node
	// has no NodeInstance and tasks-under-root crash UpdateAsset. Composites are exempt
	// — Root accepts a single composite child by engine design.
	if (BTNodeClass->IsChildOf(UBTTaskNode::StaticClass()))
	{
		if (TOptional<FString> ParentErr = ValidateParentForChildTask(ParentGraphNode, TEXT("add_bt_node")))
		{
			return FMonolithActionResult::Error(MoveTemp(*ParentErr));
		}
	}

	// Determine graph node class
	UClass* GraphNodeClass = GetGraphNodeClassForBTNode(BTNodeClass);
	if (!GraphNodeClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No graph node class for: %s"), *NodeClassName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add BT Node")));
	BTGraph->Modify();

	// Create the graph node
	UBehaviorTreeGraphNode* NewGraphNode = NewObject<UBehaviorTreeGraphNode>(BTGraph, GraphNodeClass);
	NewGraphNode->SetFlags(RF_Transactional);
	BTGraph->AddNode(NewGraphNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	NewGraphNode->CreateNewGuid();
	NewGraphNode->AllocateDefaultPins();

	// Create the BT node instance
	UBTNode* NewBTNode = NewObject<UBTNode>(NewGraphNode, BTNodeClass);
	NewGraphNode->NodeInstance = NewBTNode;

	// Set a default position offset from parent so they don't stack
	NewGraphNode->NodePosX = ParentGraphNode->NodePosX + 200;
	NewGraphNode->NodePosY = ParentGraphNode->NodePosY + 150;

	// Wire parent → child via pins
	ConnectParentChild(ParentGraphNode, NewGraphNode);

	// Apply optional properties
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
	{
		TArray<FString> PropErrors;
		ApplyProperties(NewBTNode, *PropsObj, BT, PropErrors);
	}

	// Init and sync
	NewGraphNode->InitializeInstance();
	BTGraph->UpdateAsset();
	BT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("node_id"), NewGraphNode->NodeGuid.ToString());
	Result->SetStringField(TEXT("node_class"), BTNodeClass->GetName());
	Result->SetStringField(TEXT("parent_id"), ParentGraphNode->NodeGuid.ToString());
	Result->SetStringField(TEXT("message"), TEXT("Node added"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  20. remove_bt_node
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleRemoveBTNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString NodeId;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("node_id"), NodeId, ErrResult))
	{
		return ErrResult;
	}

	UBehaviorTreeGraphNode* TargetNode = nullptr;
	{
		FString GuidErr;
		if (!RequireBtNodeByGuid(BTGraph, NodeId, TEXT("node_id"), BT->GetName(), TargetNode, GuidErr))
		{
			return FMonolithActionResult::Error(MoveTemp(GuidErr));
		}
	}

	// Don't allow removing the root
	if (Cast<UBehaviorTreeGraphNode_Root>(TargetNode))
	{
		return FMonolithActionResult::Error(TEXT("Cannot remove the root node"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Remove BT Node")));
	BTGraph->Modify();

	// Break all pin connections
	for (UEdGraphPin* Pin : TargetNode->Pins)
	{
		Pin->BreakAllPinLinks();
	}

	// Remove all sub-nodes first
	TargetNode->RemoveAllSubNodes();

	// Destroy the node from the graph
	TargetNode->DestroyNode();

	BTGraph->UpdateAsset();
	BT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("removed_node_id"), NodeId);
	Result->SetStringField(TEXT("message"), TEXT("Node removed"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  21. move_bt_node
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleMoveBTNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString NodeId, NewParentId;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("node_id"), NodeId, ErrResult))
	{
		return ErrResult;
	}
	if (!MonolithAI::RequireStringParam(Params, TEXT("new_parent_id"), NewParentId, ErrResult))
	{
		return ErrResult;
	}

	UBehaviorTreeGraphNode* TargetNode = nullptr;
	{
		FString GuidErr;
		if (!RequireBtNodeByGuid(BTGraph, NodeId, TEXT("node_id"), BT->GetName(), TargetNode, GuidErr))
		{
			return FMonolithActionResult::Error(MoveTemp(GuidErr));
		}
	}

	UBehaviorTreeGraphNode* NewParent = nullptr;
	{
		FString GuidErr;
		if (!RequireBtNodeByGuid(BTGraph, NewParentId, TEXT("new_parent_id"), BT->GetName(), NewParent, GuidErr))
		{
			return FMonolithActionResult::Error(MoveTemp(GuidErr));
		}
	}

	// Phase F1: if we're reparenting a Task, the new parent must be a composite.
	// Same hazard as add_bt_use_ability_task — wiring a Task under Root crashes
	// UpdateAsset() (BehaviorTreeGraph.cpp:517). Composites can be moved under Root
	// (engine accepts a single composite child of Root).
	if (const UBTNode* TargetBTNode = Cast<UBTNode>(TargetNode->NodeInstance))
	{
		if (TargetBTNode->IsA<UBTTaskNode>())
		{
			if (TOptional<FString> ParentErr = ValidateParentForChildTask(NewParent, TEXT("move_bt_node")))
			{
				return FMonolithActionResult::Error(MoveTemp(*ParentErr));
			}
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Move BT Node")));
	BTGraph->Modify();

	// Disconnect from old parent
	DisconnectFromParent(TargetNode);

	// Connect to new parent
	ConnectParentChild(NewParent, TargetNode);

	// Position near new parent
	TargetNode->NodePosX = NewParent->NodePosX + 200;
	TargetNode->NodePosY = NewParent->NodePosY + 150;

	BTGraph->UpdateAsset();
	BT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("new_parent_id"), NewParentId);
	Result->SetStringField(TEXT("message"), TEXT("Node moved"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  22. add_bt_decorator
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleAddBTDecorator(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString NodeId, DecoratorClassName;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("node_id"), NodeId, ErrResult))
	{
		return ErrResult;
	}
	if (!MonolithAI::RequireStringParam(Params, TEXT("decorator_class"), DecoratorClassName, ErrResult))
	{
		return ErrResult;
	}

	UBehaviorTreeGraphNode* TargetNode = nullptr;
	{
		FString GuidErr;
		if (!RequireBtNodeByGuid(BTGraph, NodeId, TEXT("node_id"), BT->GetName(), TargetNode, GuidErr))
		{
			return FMonolithActionResult::Error(MoveTemp(GuidErr));
		}
	}

	// Find decorator class
	UClass* DecClass = FindFirstObject<UClass>(*DecoratorClassName, EFindFirstObjectOptions::EnsureIfAmbiguous);
	if (!DecClass)
	{
		DecClass = FindFirstObject<UClass>(*(FString(TEXT("U")) + DecoratorClassName), EFindFirstObjectOptions::EnsureIfAmbiguous);
	}
	if (!DecClass || !DecClass->IsChildOf(UBTDecorator::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Decorator class not found or invalid: %s"), *DecoratorClassName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add BT Decorator")));
	BTGraph->Modify();

	// Create decorator graph node
	UBehaviorTreeGraphNode_Decorator* DecGraphNode = NewObject<UBehaviorTreeGraphNode_Decorator>(BTGraph);

	// Create the decorator instance
	UBTDecorator* DecInstance = NewObject<UBTDecorator>(DecGraphNode, DecClass);
	DecGraphNode->NodeInstance = DecInstance;

	// AddSubNode handles: guid, pins, parent linkage, OnSubNodeAdded → Decorators[], UpdateAsset
	TargetNode->AddSubNode(DecGraphNode, BTGraph);

	// Apply optional properties
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
	{
		TArray<FString> PropErrors;
		ApplyProperties(DecInstance, *PropsObj, BT, PropErrors);
	}

	BT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("decorator_id"), DecGraphNode->NodeGuid.ToString());
	Result->SetStringField(TEXT("decorator_class"), DecClass->GetName());
	Result->SetNumberField(TEXT("decorator_count"), TargetNode->Decorators.Num());
	Result->SetStringField(TEXT("message"), TEXT("Decorator added"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  23. remove_bt_decorator
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleRemoveBTDecorator(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString NodeId;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("node_id"), NodeId, ErrResult))
	{
		return ErrResult;
	}

	int32 DecoratorIndex = (int32)Params->GetNumberField(TEXT("decorator_index"));

	UBehaviorTreeGraphNode* TargetNode = nullptr;
	{
		FString GuidErr;
		if (!RequireBtNodeByGuid(BTGraph, NodeId, TEXT("node_id"), BT->GetName(), TargetNode, GuidErr))
		{
			return FMonolithActionResult::Error(MoveTemp(GuidErr));
		}
	}

	if (DecoratorIndex < 0 || DecoratorIndex >= TargetNode->Decorators.Num())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Decorator index %d out of range [0, %d)"), DecoratorIndex, TargetNode->Decorators.Num()));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Remove BT Decorator")));
	BTGraph->Modify();

	UBehaviorTreeGraphNode* DecNode = TargetNode->Decorators[DecoratorIndex];
	TargetNode->RemoveSubNode(DecNode);

	BTGraph->UpdateAsset();
	BT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetNumberField(TEXT("removed_index"), DecoratorIndex);
	Result->SetNumberField(TEXT("decorator_count"), TargetNode->Decorators.Num());
	Result->SetStringField(TEXT("message"), TEXT("Decorator removed"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  24. add_bt_service
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleAddBTService(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString NodeId, ServiceClassName;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("node_id"), NodeId, ErrResult))
	{
		return ErrResult;
	}
	if (!MonolithAI::RequireStringParam(Params, TEXT("service_class"), ServiceClassName, ErrResult))
	{
		return ErrResult;
	}

	UBehaviorTreeGraphNode* TargetNode = nullptr;
	{
		FString GuidErr;
		if (!RequireBtNodeByGuid(BTGraph, NodeId, TEXT("node_id"), BT->GetName(), TargetNode, GuidErr))
		{
			return FMonolithActionResult::Error(MoveTemp(GuidErr));
		}
	}

	// Find service class
	UClass* SvcClass = FindFirstObject<UClass>(*ServiceClassName, EFindFirstObjectOptions::EnsureIfAmbiguous);
	if (!SvcClass)
	{
		SvcClass = FindFirstObject<UClass>(*(FString(TEXT("U")) + ServiceClassName), EFindFirstObjectOptions::EnsureIfAmbiguous);
	}
	if (!SvcClass || !SvcClass->IsChildOf(UBTService::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Service class not found or invalid: %s"), *ServiceClassName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add BT Service")));
	BTGraph->Modify();

	// Create service graph node
	UBehaviorTreeGraphNode_Service* SvcGraphNode = NewObject<UBehaviorTreeGraphNode_Service>(BTGraph);

	// Create the service instance
	UBTService* SvcInstance = NewObject<UBTService>(SvcGraphNode, SvcClass);
	SvcGraphNode->NodeInstance = SvcInstance;

	// AddSubNode handles everything (guid, pins, parent linkage, OnSubNodeAdded → Services[], UpdateAsset)
	TargetNode->AddSubNode(SvcGraphNode, BTGraph);

	// Apply optional properties
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
	{
		TArray<FString> PropErrors;
		ApplyProperties(SvcInstance, *PropsObj, BT, PropErrors);
	}

	BT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("service_id"), SvcGraphNode->NodeGuid.ToString());
	Result->SetStringField(TEXT("service_class"), SvcClass->GetName());
	Result->SetNumberField(TEXT("service_count"), TargetNode->Services.Num());
	Result->SetStringField(TEXT("message"), TEXT("Service added"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  25. remove_bt_service
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleRemoveBTService(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString NodeId;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("node_id"), NodeId, ErrResult))
	{
		return ErrResult;
	}

	int32 ServiceIndex = (int32)Params->GetNumberField(TEXT("service_index"));

	UBehaviorTreeGraphNode* TargetNode = nullptr;
	{
		FString GuidErr;
		if (!RequireBtNodeByGuid(BTGraph, NodeId, TEXT("node_id"), BT->GetName(), TargetNode, GuidErr))
		{
			return FMonolithActionResult::Error(MoveTemp(GuidErr));
		}
	}

	if (ServiceIndex < 0 || ServiceIndex >= TargetNode->Services.Num())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Service index %d out of range [0, %d)"), ServiceIndex, TargetNode->Services.Num()));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Remove BT Service")));
	BTGraph->Modify();

	UBehaviorTreeGraphNode* SvcNode = TargetNode->Services[ServiceIndex];
	TargetNode->RemoveSubNode(SvcNode);

	BTGraph->UpdateAsset();
	BT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetNumberField(TEXT("removed_index"), ServiceIndex);
	Result->SetNumberField(TEXT("service_count"), TargetNode->Services.Num());
	Result->SetStringField(TEXT("message"), TEXT("Service removed"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  26. set_bt_node_property
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleSetBTNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString NodeId, PropertyName;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("node_id"), NodeId, ErrResult))
	{
		return ErrResult;
	}
	if (!MonolithAI::RequireStringParam(Params, TEXT("property_name"), PropertyName, ErrResult))
	{
		return ErrResult;
	}

	UBehaviorTreeGraphNode* GraphNode = nullptr;
	{
		FString GuidErr;
		if (!RequireBtNodeByGuid(BTGraph, NodeId, TEXT("node_id"), BT->GetName(), GraphNode, GuidErr))
		{
			return FMonolithActionResult::Error(MoveTemp(GuidErr));
		}
	}

	UBTNode* BTNode = Cast<UBTNode>(GraphNode->NodeInstance);
	if (!BTNode)
	{
		return FMonolithActionResult::Error(TEXT("Node has no BT node instance"));
	}

	TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
	if (!Value.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: value"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Set BT Node Property")));
	BTNode->Modify();

	FString PropError;
	if (!SetPropertyValue(BTNode, PropertyName, Value, BT, PropError))
	{
		return FMonolithActionResult::Error(PropError);
	}

	BTGraph->UpdateAsset();
	BT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("property_name"), PropertyName);
	Result->SetStringField(TEXT("message"), TEXT("Property set"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  27. get_bt_node_properties
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleGetBTNodeProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString NodeId;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("node_id"), NodeId, ErrResult))
	{
		return ErrResult;
	}

	UBehaviorTreeGraphNode* GraphNode = nullptr;
	{
		FString GuidErr;
		if (!RequireBtNodeByGuid(BTGraph, NodeId, TEXT("node_id"), BT->GetName(), GraphNode, GuidErr))
		{
			return FMonolithActionResult::Error(MoveTemp(GuidErr));
		}
	}

	UBTNode* BTNode = Cast<UBTNode>(GraphNode->NodeInstance);
	if (!BTNode)
	{
		return FMonolithActionResult::Error(TEXT("Node has no BT node instance"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("node_class"), BTNode->GetClass()->GetName());
	Result->SetObjectField(TEXT("properties"), SerializeObjectProperties(BTNode));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  28. reorder_bt_children
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleReorderBTChildren(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString ParentId;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("parent_id"), ParentId, ErrResult))
	{
		return ErrResult;
	}

	UBehaviorTreeGraphNode* ParentNode = nullptr;
	{
		FString GuidErr;
		if (!RequireBtNodeByGuid(BTGraph, ParentId, TEXT("parent_id"), BT->GetName(), ParentNode, GuidErr))
		{
			return FMonolithActionResult::Error(MoveTemp(GuidErr));
		}
	}

	// Phase F1 note: reorder is a permutation of EXISTING children — it cannot introduce
	// a Task-under-Root pairing that wasn't already wired. The count-mismatch guard below
	// rejects empty `new_order` against a populated parent and vice-versa, so we cannot
	// wire a Task into an empty Root-only graph through this path. The hardened
	// ConnectParentChild also schema-rejects any malformed pairing it's asked to recreate.
	// No additional ValidateParentForChildTask guard needed here.

	// Parse new_order array
	const TArray<TSharedPtr<FJsonValue>>* NewOrderArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("new_order"), NewOrderArr) || !NewOrderArr)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: new_order (array of node GUIDs)"));
	}

	// Get current children
	TArray<UBehaviorTreeGraphNode*> CurrentChildren = GetChildNodes(ParentNode);

	// Build map of GUID → child node
	TMap<FString, UBehaviorTreeGraphNode*> ChildMap;
	for (UBehaviorTreeGraphNode* Child : CurrentChildren)
	{
		ChildMap.Add(Child->NodeGuid.ToString(), Child);
	}

	// Validate and build new order
	TArray<UBehaviorTreeGraphNode*> NewOrder;
	for (const TSharedPtr<FJsonValue>& Val : *NewOrderArr)
	{
		FString ChildGuid = Val->AsString();
		UBehaviorTreeGraphNode** Found = ChildMap.Find(ChildGuid);
		if (!Found)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Child GUID not found under parent: %s"), *ChildGuid));
		}
		NewOrder.Add(*Found);
	}

	if (NewOrder.Num() != CurrentChildren.Num())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("new_order has %d entries but parent has %d children — all children must be specified"),
			NewOrder.Num(), CurrentChildren.Num()));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Reorder BT Children")));
	BTGraph->Modify();

	// Disconnect all children from parent output pin
	UEdGraphPin* ParentOut = ParentNode->GetOutputPin();
	if (ParentOut)
	{
		ParentOut->BreakAllPinLinks();
	}

	// Reconnect in new order
	for (UBehaviorTreeGraphNode* Child : NewOrder)
	{
		ConnectParentChild(ParentNode, Child);
	}

	// Rebuild execution order to reflect new child ordering
	BTGraph->RebuildChildOrder(ParentNode);
	BTGraph->UpdateAsset();
	BT->MarkPackageDirty();

	// Build result with new order
	TArray<TSharedPtr<FJsonValue>> OrderResult;
	for (UBehaviorTreeGraphNode* Child : NewOrder)
	{
		TSharedPtr<FJsonObject> ChildObj = MakeShared<FJsonObject>();
		ChildObj->SetStringField(TEXT("node_id"), Child->NodeGuid.ToString());
		if (UBTNode* BTChild = Cast<UBTNode>(Child->NodeInstance))
		{
			ChildObj->SetStringField(TEXT("node_class"), BTChild->GetClass()->GetName());
		}
		OrderResult.Add(MakeShared<FJsonValueObject>(ChildObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("parent_id"), ParentId);
	Result->SetArrayField(TEXT("new_order"), OrderResult);
	Result->SetStringField(TEXT("message"), TEXT("Children reordered"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  30. add_bt_run_eqs_task
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleAddBTRunEQSTask(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString EQSPath, BBResultKey;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("eqs_path"), EQSPath, ErrResult))
	{
		return ErrResult;
	}
	if (!MonolithAI::RequireStringParam(Params, TEXT("bb_result_key"), BBResultKey, ErrResult))
	{
		return ErrResult;
	}

	// Verify the EQS asset exists
	UEnvQuery* EQSQuery = Cast<UEnvQuery>(FMonolithAssetUtils::LoadAssetByPath(UEnvQuery::StaticClass(), EQSPath));
	if (!EQSQuery)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("EQS query not found: %s"), *EQSPath));
	}

	// Find the RunEQSQuery task class
	UClass* RunEQSClass = FindFirstObject<UClass>(TEXT("BTTask_RunEQSQuery"), EFindFirstObjectOptions::EnsureIfAmbiguous);
	if (!RunEQSClass)
	{
		RunEQSClass = FindFirstObject<UClass>(TEXT("UBTTask_RunEQSQuery"), EFindFirstObjectOptions::EnsureIfAmbiguous);
	}
	if (!RunEQSClass)
	{
		return FMonolithActionResult::Error(TEXT("BTTask_RunEQSQuery class not found — ensure AIModule is loaded"));
	}

	// Find parent
	FString ParentIdStr = Params->GetStringField(TEXT("parent_id"));
	UBehaviorTreeGraphNode* ParentGraphNode = nullptr;
	if (ParentIdStr.IsEmpty())
	{
		ParentGraphNode = FindRootNode(BTGraph);
		if (!ParentGraphNode)
		{
			return FMonolithActionResult::Error(TEXT("Root node not found in BT graph"));
		}
	}
	else
	{
		FString GuidErr;
		if (!RequireBtNodeByGuid(BTGraph, ParentIdStr, TEXT("parent_id"), BT->GetName(), ParentGraphNode, GuidErr))
		{
			return FMonolithActionResult::Error(MoveTemp(GuidErr));
		}
	}

	// Phase F1: parent must be a composite (RunEQSQuery is always a Task).
	if (TOptional<FString> ParentErr = ValidateParentForChildTask(ParentGraphNode, TEXT("add_bt_run_eqs_task")))
	{
		return FMonolithActionResult::Error(MoveTemp(*ParentErr));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add RunEQSQuery Task")));
	BTGraph->Modify();

	// Create graph node
	UBehaviorTreeGraphNode_Task* TaskGraphNode = NewObject<UBehaviorTreeGraphNode_Task>(BTGraph);
	TaskGraphNode->SetFlags(RF_Transactional);
	BTGraph->AddNode(TaskGraphNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	TaskGraphNode->CreateNewGuid();
	TaskGraphNode->AllocateDefaultPins();

	// Create task instance
	UBTNode* TaskNode = NewObject<UBTNode>(TaskGraphNode, RunEQSClass);
	TaskGraphNode->NodeInstance = TaskNode;

	// Position
	TaskGraphNode->NodePosX = ParentGraphNode->NodePosX + 200;
	TaskGraphNode->NodePosY = ParentGraphNode->NodePosY + 150;

	// Wire
	ConnectParentChild(ParentGraphNode, TaskGraphNode);

	// Set properties: QueryTemplate, EQSQueryBlackboardKey, RunMode
	FString PropError;
	TSharedPtr<FJsonValue> EQSPathVal = MakeShared<FJsonValueString>(EQSPath);
	SetPropertyValue(TaskNode, TEXT("QueryTemplate"), EQSPathVal, BT, PropError);

	TSharedPtr<FJsonValue> BBKeyVal = MakeShared<FJsonValueString>(BBResultKey);
	SetPropertyValue(TaskNode, TEXT("EQSQueryBlackboardKey"), BBKeyVal, BT, PropError);
	// Also try BlackboardKey as some versions use that
	SetPropertyValue(TaskNode, TEXT("BlackboardKey"), BBKeyVal, BT, PropError);

	FString RunMode = Params->GetStringField(TEXT("run_mode"));
	if (!RunMode.IsEmpty())
	{
		TSharedPtr<FJsonValue> RunModeVal = MakeShared<FJsonValueString>(RunMode);
		SetPropertyValue(TaskNode, TEXT("RunMode"), RunModeVal, BT, PropError);
	}

	TaskGraphNode->InitializeInstance();
	BTGraph->UpdateAsset();
	BT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("node_id"), TaskGraphNode->NodeGuid.ToString());
	Result->SetStringField(TEXT("node_class"), RunEQSClass->GetName());
	Result->SetStringField(TEXT("eqs_path"), EQSPath);
	Result->SetStringField(TEXT("bb_result_key"), BBResultKey);
	Result->SetStringField(TEXT("message"), TEXT("RunEQSQuery task added and configured"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  31. add_bt_smart_object_task
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleAddBTSmartObjectTask(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString ActivityTags;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("activity_tags"), ActivityTags, ErrResult))
	{
		return ErrResult;
	}

	// Phase D2: pre-check that the GameplayBehaviorSmartObjects plugin is enabled
	// (the actual SO BT task class lives in this plugin, NOT in core SmartObjects)
	{
		TSharedPtr<IPlugin> GBSOPlugin = IPluginManager::Get().FindPlugin(TEXT("GameplayBehaviorSmartObjects"));
		if (!GBSOPlugin.IsValid() || !GBSOPlugin->IsEnabled())
		{
			return FMonolithActionResult::Error(TEXT(
				"Smart Object BT task requires the 'GameplayBehaviorSmartObjects' plugin "
				"(Edit > Plugins > AI > GameplayBehaviorSmartObjects). "
				"This is separate from the core 'SmartObjects' plugin. "
				"Enable it and restart the editor."));
		}
	}

	// Find smart object task class — try common names
	// Canonical: BTTask_FindAndUseGameplayBehaviorSmartObject (from GameplayBehaviorSmartObjects plugin)
	UClass* SOTaskClass = nullptr;
	static const TCHAR* SOClassNames[] = {
		TEXT("BTTask_FindAndUseGameplayBehaviorSmartObject"),
		TEXT("UBTTask_FindAndUseGameplayBehaviorSmartObject"),
		TEXT("BTTask_UseSmartObject"),
		TEXT("UBTTask_UseSmartObject"),
		TEXT("BTTask_FindAndUseSmartObject"),
		TEXT("UBTTask_FindAndUseSmartObject"),
	};

	for (const TCHAR* ClassName : SOClassNames)
	{
		SOTaskClass = FindFirstObject<UClass>(ClassName, EFindFirstObjectOptions::EnsureIfAmbiguous);
		if (SOTaskClass && SOTaskClass->IsChildOf(UBTTaskNode::StaticClass()))
		{
			break;
		}
		SOTaskClass = nullptr;
	}

	if (!SOTaskClass)
	{
		return FMonolithActionResult::Error(TEXT(
			"Smart Object BT task class not found. Expected 'BTTask_FindAndUseGameplayBehaviorSmartObject' "
			"from the 'GameplayBehaviorSmartObjects' plugin. The plugin is reported enabled but the class "
			"could not be resolved — this usually means the editor needs to be restarted after enabling the plugin."));
	}

	// Find parent
	FString ParentIdStr = Params->GetStringField(TEXT("parent_id"));
	UBehaviorTreeGraphNode* ParentGraphNode = nullptr;
	if (ParentIdStr.IsEmpty())
	{
		ParentGraphNode = FindRootNode(BTGraph);
		if (!ParentGraphNode)
		{
			return FMonolithActionResult::Error(TEXT("Root node not found in BT graph"));
		}
	}
	else
	{
		FString GuidErr;
		if (!RequireBtNodeByGuid(BTGraph, ParentIdStr, TEXT("parent_id"), BT->GetName(), ParentGraphNode, GuidErr))
		{
			return FMonolithActionResult::Error(MoveTemp(GuidErr));
		}
	}

	// Phase F1: parent must be a composite (SmartObject task is always a Task).
	if (TOptional<FString> ParentErr = ValidateParentForChildTask(ParentGraphNode, TEXT("add_bt_smart_object_task")))
	{
		return FMonolithActionResult::Error(MoveTemp(*ParentErr));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add Smart Object Task")));
	BTGraph->Modify();

	// Create graph node
	UBehaviorTreeGraphNode_Task* TaskGraphNode = NewObject<UBehaviorTreeGraphNode_Task>(BTGraph);
	TaskGraphNode->SetFlags(RF_Transactional);
	BTGraph->AddNode(TaskGraphNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	TaskGraphNode->CreateNewGuid();
	TaskGraphNode->AllocateDefaultPins();

	// Create task instance
	UBTNode* TaskNode = NewObject<UBTNode>(TaskGraphNode, SOTaskClass);
	TaskGraphNode->NodeInstance = TaskNode;

	// Position
	TaskGraphNode->NodePosX = ParentGraphNode->NodePosX + 200;
	TaskGraphNode->NodePosY = ParentGraphNode->NodePosY + 150;

	// Wire
	ConnectParentChild(ParentGraphNode, TaskGraphNode);

	// Apply activity tags
	FString PropError;
	TSharedPtr<FJsonValue> TagsVal = MakeShared<FJsonValueString>(ActivityTags);
	// Try ActivityRequirements, ActivityTag, and Tag — the property name varies by version
	if (!SetPropertyValue(TaskNode, TEXT("ActivityRequirements"), TagsVal, BT, PropError))
	{
		if (!SetPropertyValue(TaskNode, TEXT("ActivityTag"), TagsVal, BT, PropError))
		{
			SetPropertyValue(TaskNode, TEXT("Tag"), TagsVal, BT, PropError);
		}
	}

	// Apply search radius
	double SearchRadius = 5000.0;
	if (Params->HasField(TEXT("search_radius")))
	{
		SearchRadius = Params->GetNumberField(TEXT("search_radius"));
	}
	TSharedPtr<FJsonValue> RadiusVal = MakeShared<FJsonValueNumber>(SearchRadius);
	SetPropertyValue(TaskNode, TEXT("SearchRadius"), RadiusVal, BT, PropError);
	// Also try Radius as fallback
	SetPropertyValue(TaskNode, TEXT("Radius"), RadiusVal, BT, PropError);

	TaskGraphNode->InitializeInstance();
	BTGraph->UpdateAsset();
	BT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("node_id"), TaskGraphNode->NodeGuid.ToString());
	Result->SetStringField(TEXT("node_class"), SOTaskClass->GetName());
	Result->SetStringField(TEXT("activity_tags"), ActivityTags);
	Result->SetNumberField(TEXT("search_radius"), SearchRadius);
	Result->SetStringField(TEXT("message"), TEXT("Smart Object task added and configured"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  31b. add_bt_use_ability_task — Phase I2 (BT-to-GAS)
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleAddBTUseAbilityTask(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_GAMEPLAYABILITIES
	return FMonolithActionResult::Error(TEXT(
		"add_bt_use_ability_task requires the GameplayAbilities engine plugin. "
		"Enable Edit > Plugins > Gameplay > Gameplay Abilities and rebuild MonolithAI."));
#else
	// 1. Load BT + graph
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	// 2. Mutual-exclusion validation
	FString AbilityClassPath;
	const bool bHasAbilityClass = Params->TryGetStringField(TEXT("ability_class"), AbilityClassPath)
		&& !AbilityClassPath.IsEmpty();

	// ability_tags accepts either a comma-separated string OR a JSON array of strings
	TArray<FString> RawTagList;
	bool bHasAbilityTags = false;
	{
		const TArray<TSharedPtr<FJsonValue>>* TagsArrayPtr = nullptr;
		if (Params->TryGetArrayField(TEXT("ability_tags"), TagsArrayPtr) && TagsArrayPtr)
		{
			for (const TSharedPtr<FJsonValue>& Val : *TagsArrayPtr)
			{
				FString S;
				if (Val.IsValid() && Val->TryGetString(S) && !S.IsEmpty())
				{
					RawTagList.Add(S);
				}
			}
			bHasAbilityTags = RawTagList.Num() > 0;
		}
		else
		{
			FString TagsStr;
			if (Params->TryGetStringField(TEXT("ability_tags"), TagsStr) && !TagsStr.IsEmpty())
			{
				TagsStr.ParseIntoArray(RawTagList, TEXT(","), /*InCullEmpty=*/true);
				for (FString& T : RawTagList) { T.TrimStartAndEndInline(); }
				bHasAbilityTags = RawTagList.Num() > 0;
			}
		}
	}

	if (bHasAbilityClass && bHasAbilityTags)
	{
		return FMonolithActionResult::Error(TEXT(
			"add_bt_use_ability_task: ability_class and ability_tags are mutually exclusive — supply exactly one"));
	}
	if (!bHasAbilityClass && !bHasAbilityTags)
	{
		return FMonolithActionResult::Error(TEXT(
			"add_bt_use_ability_task: must supply either ability_class or ability_tags"));
	}

	// 3. Resolve ability class (asset path -> Blueprint -> GeneratedClass; or native class name)
	UClass* ResolvedAbilityClass = nullptr;
	if (bHasAbilityClass)
	{
		// Asset path form (e.g. /Game/AI/Abilities/GA_Roar) — load the Blueprint, take its GeneratedClass.
		if (AbilityClassPath.StartsWith(TEXT("/")))
		{
			UObject* Loaded = FMonolithAssetUtils::LoadAssetByPath(UObject::StaticClass(), AbilityClassPath);
			if (UBlueprint* BP = Cast<UBlueprint>(Loaded))
			{
				ResolvedAbilityClass = BP->GeneratedClass;
			}
			else if (UClass* DirectCls = Cast<UClass>(Loaded))
			{
				// Some pipelines expose the generated class directly
				ResolvedAbilityClass = DirectCls;
			}
		}

		// Class-name fallback: native or already-loaded Blueprint class
		if (!ResolvedAbilityClass)
		{
			ResolvedAbilityClass = FindFirstObject<UClass>(*AbilityClassPath, EFindFirstObjectOptions::EnsureIfAmbiguous);
			if (!ResolvedAbilityClass)
			{
				// Try with a 'U' prefix (native class convention)
				ResolvedAbilityClass = FindFirstObject<UClass>(
					*(FString(TEXT("U")) + AbilityClassPath),
					EFindFirstObjectOptions::EnsureIfAmbiguous);
			}
		}

		if (!ResolvedAbilityClass)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("add_bt_use_ability_task: ability_class '%s' could not be resolved (asset path or class name)"),
				*AbilityClassPath));
		}

		if (!ResolvedAbilityClass->IsChildOf(UGameplayAbility::StaticClass()))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("add_bt_use_ability_task: '%s' is not a UGameplayAbility subclass"),
				*ResolvedAbilityClass->GetName()));
		}
	}

	// 4. Parse tag container
	FGameplayTagContainer ResolvedTags;
	if (bHasAbilityTags)
	{
		for (const FString& TagStr : RawTagList)
		{
			const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(*TagStr, /*ErrorIfNotFound=*/false);
			if (Tag.IsValid())
			{
				ResolvedTags.AddTag(Tag);
			}
			else
			{
				UE_LOG(LogMonolithAI, Warning,
					TEXT("add_bt_use_ability_task: unknown gameplay tag '%s' (will not be added to query)"),
					*TagStr);
			}
		}
		if (ResolvedTags.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT(
				"add_bt_use_ability_task: ability_tags supplied but none resolved to valid GameplayTags"));
		}
	}

	// 5. Validate optional event_tag (non-fatal: warn only)
	FGameplayTag EventTag;
	{
		FString EventTagStr;
		if (Params->TryGetStringField(TEXT("event_tag"), EventTagStr) && !EventTagStr.IsEmpty())
		{
			EventTag = FGameplayTag::RequestGameplayTag(*EventTagStr, /*ErrorIfNotFound=*/false);
			if (!EventTag.IsValid())
			{
				UE_LOG(LogMonolithAI, Warning,
					TEXT("add_bt_use_ability_task: event_tag '%s' is not a registered GameplayTag — will be ignored at runtime"),
					*EventTagStr);
			}
		}
	}

	// 6. Find parent
	FString ParentIdStr;
	Params->TryGetStringField(TEXT("parent_id"), ParentIdStr);
	UBehaviorTreeGraphNode* ParentGraphNode = nullptr;
	if (ParentIdStr.IsEmpty())
	{
		ParentGraphNode = FindRootNode(BTGraph);
		if (!ParentGraphNode)
		{
			return FMonolithActionResult::Error(TEXT("add_bt_use_ability_task: Root node not found in BT graph"));
		}
	}
	else
	{
		FString GuidErr;
		if (!RequireBtNodeByGuid(BTGraph, ParentIdStr, TEXT("parent_id"), BT->GetName(), ParentGraphNode, GuidErr))
		{
			return FMonolithActionResult::Error(MoveTemp(GuidErr));
		}
	}

	// Phase F1 (the original crash site): parent must be a composite. Wiring a Task
	// directly under the Root edge node crashes UBehaviorTreeGraph::UpdateAsset() at
	// BehaviorTreeGraph.cpp:517 (deref of null BTAsset->RootNode).
	if (TOptional<FString> ParentErr = ValidateParentForChildTask(ParentGraphNode, TEXT("add_bt_use_ability_task")))
	{
		return FMonolithActionResult::Error(MoveTemp(*ParentErr));
	}

	// 7. Build the graph node + node instance
	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add TryActivateAbility Task")));
	BTGraph->Modify();

	UBehaviorTreeGraphNode_Task* TaskGraphNode = NewObject<UBehaviorTreeGraphNode_Task>(BTGraph);
	TaskGraphNode->SetFlags(RF_Transactional);
	BTGraph->AddNode(TaskGraphNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	TaskGraphNode->CreateNewGuid();
	TaskGraphNode->AllocateDefaultPins();

	UBTTask_TryActivateAbility* TaskNode = NewObject<UBTTask_TryActivateAbility>(
		TaskGraphNode, UBTTask_TryActivateAbility::StaticClass());
	TaskGraphNode->NodeInstance = TaskNode;

	// Position
	TaskGraphNode->NodePosX = ParentGraphNode->NodePosX + 200;
	TaskGraphNode->NodePosY = ParentGraphNode->NodePosY + 150;

	// Wire parent -> child
	ConnectParentChild(ParentGraphNode, TaskGraphNode);

	// 8. Apply configuration directly (we own the UCLASS so no reflection needed)
	if (ResolvedAbilityClass)
	{
		TaskNode->AbilityClass = ResolvedAbilityClass;
	}
	if (!ResolvedTags.IsEmpty())
	{
		TaskNode->AbilityTags = ResolvedTags;
	}

	bool bWaitForEnd = true;
	Params->TryGetBoolField(TEXT("wait_for_end"), bWaitForEnd);
	TaskNode->bWaitForEnd = bWaitForEnd;

	bool bSucceedOnBlocked = false;
	Params->TryGetBoolField(TEXT("succeed_on_blocked"), bSucceedOnBlocked);
	TaskNode->bSucceedOnBlocked = bSucceedOnBlocked;

	if (EventTag.IsValid())
	{
		TaskNode->EventTagOnActivate = EventTag;
	}

	// Optional node-name override
	FString NodeName;
	if (Params->TryGetStringField(TEXT("node_name"), NodeName) && !NodeName.IsEmpty())
	{
		TaskNode->NodeName = NodeName;
	}

	TaskGraphNode->InitializeInstance();
	BTGraph->UpdateAsset();
	BT->MarkPackageDirty();

	// 9. Return result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("node_id"), TaskGraphNode->NodeGuid.ToString());
	Result->SetStringField(TEXT("node_class"), UBTTask_TryActivateAbility::StaticClass()->GetName());
	if (ResolvedAbilityClass)
	{
		Result->SetStringField(TEXT("ability_class"), ResolvedAbilityClass->GetPathName());
	}
	if (!ResolvedTags.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> TagsArr;
		for (const FGameplayTag& T : ResolvedTags)
		{
			TagsArr.Add(MakeShared<FJsonValueString>(T.ToString()));
		}
		Result->SetArrayField(TEXT("ability_tags"), TagsArr);
	}
	Result->SetBoolField(TEXT("wait_for_end"), bWaitForEnd);
	Result->SetBoolField(TEXT("succeed_on_blocked"), bSucceedOnBlocked);
	if (EventTag.IsValid())
	{
		Result->SetStringField(TEXT("event_tag"), EventTag.ToString());
	}
	Result->SetStringField(TEXT("message"), TEXT("TryActivateAbility task added and configured"));
	return FMonolithActionResult::Success(Result);
#endif // WITH_GAMEPLAYABILITIES
}

// ============================================================
//  32. build_behavior_tree_from_spec
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleBuildBTFromSpec(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult))
	{
		return ErrResult;
	}

	const TSharedPtr<FJsonObject>* SpecObjPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("spec"), SpecObjPtr) || !SpecObjPtr || !(*SpecObjPtr).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: spec (JSON object)"));
	}
	const TSharedPtr<FJsonObject>& Spec = *SpecObjPtr;

	bool bStrictMode = false;
	Params->TryGetBoolField(TEXT("strict_mode"), bStrictMode);

	// Validate root exists
	const TSharedPtr<FJsonObject>* RootObjPtr = nullptr;
	if (!Spec->TryGetObjectField(TEXT("root"), RootObjPtr) || !RootObjPtr || !(*RootObjPtr).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Spec missing 'root' field"));
	}

	// =====================================================================
	// PRE-VALIDATION — resolve the root node's class BEFORE creating any
	// package. If the root cannot be added we must NOT save a zero-node BT.
	// =====================================================================
	{
		const FString RootType = (*RootObjPtr)->GetStringField(TEXT("type"));
		if (RootType.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Spec root missing 'type' field — refusing to create empty BT"));
		}
		UClass* RootBTClass = ResolveNodeClass(RootType);
		if (!RootBTClass)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Spec root type '%s' does not resolve to a UClass — refusing to create empty BT"), *RootType));
		}
		const bool bRootIsComposite = RootBTClass->IsChildOf(UBTCompositeNode::StaticClass());
		const bool bRootIsTask = RootBTClass->IsChildOf(UBTTaskNode::StaticClass());
		if (!bRootIsComposite && !bRootIsTask)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Spec root type '%s' is neither composite nor task — refusing to create empty BT"), *RootType));
		}

		// Phase F1: reject Task-as-spec-root. The BT runtime root must be a composite
		// (Selector/Sequence/Parallel/SimpleParallel) — a bare task at root produces an
		// invalid graph that crashes UpdateAsset() at BehaviorTreeGraph.cpp:517.
		// Per ADR: do NOT auto-wrap; auto-wrap masks user intent.
		if (bRootIsTask)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("build_behavior_tree_from_spec: BT root must be a Composite node "
					 "(Selector/Sequence/Parallel/SimpleParallel), got Task '%s'. "
					 "Wrap your task in a composite."),
				*RootType));
		}
	}

	// In strict mode, walk the entire spec tree and collect ALL unresolvable
	// types before any mutation; bail out if anything fails to resolve.
	if (bStrictMode)
	{
		TArray<FString> StrictErrors;
		TFunction<void(const TSharedPtr<FJsonObject>&, const FString&)> WalkSpec;
		WalkSpec = [&](const TSharedPtr<FJsonObject>& NodeSpec, const FString& Path)
		{
			if (!NodeSpec.IsValid()) return;
			const FString TypeName = NodeSpec->GetStringField(TEXT("type"));
			if (TypeName.IsEmpty())
			{
				StrictErrors.Add(FString::Printf(TEXT("%s: missing 'type' field"), *Path));
			}
			else
			{
				UClass* Cls = ResolveNodeClass(TypeName);
				if (!Cls)
				{
					StrictErrors.Add(FString::Printf(TEXT("%s: type '%s' does not resolve"), *Path, *TypeName));
				}
				else if (!Cls->IsChildOf(UBTCompositeNode::StaticClass()) && !Cls->IsChildOf(UBTTaskNode::StaticClass()))
				{
					StrictErrors.Add(FString::Printf(TEXT("%s: type '%s' is not a composite or task"), *Path, *TypeName));
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* ChildrenArr = nullptr;
			if (NodeSpec->TryGetArrayField(TEXT("children"), ChildrenArr) && ChildrenArr)
			{
				for (int32 i = 0; i < ChildrenArr->Num(); ++i)
				{
					const TSharedPtr<FJsonObject>* ChildObj = nullptr;
					if ((*ChildrenArr)[i]->TryGetObject(ChildObj) && ChildObj && (*ChildObj).IsValid())
					{
						WalkSpec(*ChildObj, FString::Printf(TEXT("%s.children[%d]"), *Path, i));
					}
				}
			}
		};
		WalkSpec(*RootObjPtr, TEXT("root"));
		if (StrictErrors.Num() > 0)
		{
			FString Msg = TEXT("strict_mode: spec validation failed (asset NOT saved):");
			for (const FString& E : StrictErrors)
			{
				Msg += TEXT("\n  - ") + E;
			}
			return FMonolithActionResult::Error(Msg);
		}
	}

	FString AssetName = FPackageName::GetShortName(SavePath);

	// Check path is free
	FString PathError;
	if (!MonolithAI::EnsureAssetPathFree(SavePath, AssetName, PathError))
	{
		return FMonolithActionResult::Error(PathError);
	}

	// Create package
	FString PkgError;
	UPackage* Package = MonolithAI::GetOrCreatePackage(SavePath, PkgError);
	if (!Package)
	{
		return FMonolithActionResult::Error(PkgError);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Build BT From Spec")));

	// Create the BT asset
	UBehaviorTree* BT = NewObject<UBehaviorTree>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!BT)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UBehaviorTree object"));
	}

	// Handle blackboard — either load from path or create inline
	FString BBPath = Spec->GetStringField(TEXT("blackboard_path"));
	if (!BBPath.IsEmpty())
	{
		UBlackboardData* BB = Cast<UBlackboardData>(FMonolithAssetUtils::LoadAssetByPath(UBlackboardData::StaticClass(), BBPath));
		if (!BB)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Blackboard not found: %s"), *BBPath));
		}
		BT->BlackboardAsset = BB;
	}
	else
	{
		const TSharedPtr<FJsonObject>* BBSpecPtr = nullptr;
		if (Spec->TryGetObjectField(TEXT("blackboard"), BBSpecPtr) && BBSpecPtr && (*BBSpecPtr).IsValid())
		{
			FString BBError;
			UBlackboardData* InlineBB = CreateInlineBB(SavePath, *BBSpecPtr, BBError);
			if (!InlineBB)
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create inline BB: %s"), *BBError));
			}
			BT->BlackboardAsset = InlineBB;
		}
	}

	FAssetRegistryModule::AssetCreated(BT);

	// Create editor graph — same pattern as FBehaviorTreeEditor::RestoreBehaviorTree
	UBehaviorTreeGraph* BTGraph = Cast<UBehaviorTreeGraph>(BT->BTGraph);
	if (!BTGraph)
	{
		BT->BTGraph = FBlueprintEditorUtils::CreateNewGraph(
			BT, TEXT("Behavior Tree"),
			UBehaviorTreeGraph::StaticClass(),
			UEdGraphSchema_BehaviorTree::StaticClass());
		BTGraph = Cast<UBehaviorTreeGraph>(BT->BTGraph);

		if (BTGraph)
		{
			const UEdGraphSchema* Schema = BTGraph->GetSchema();
			Schema->CreateDefaultNodesForGraph(*BTGraph);
			BTGraph->OnCreated();
			BTGraph->Initialize();
		}
	}
	if (!BTGraph)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create BT editor graph"));
	}

	// Find the root graph node
	UBehaviorTreeGraphNode_Root* RootGraphNode = FindRootNode(BTGraph);
	if (!RootGraphNode)
	{
		return FMonolithActionResult::Error(TEXT("Root graph node not found in freshly created BT"));
	}

	// Recursively build the tree from spec
	FSpecBuildContext Ctx;
	Ctx.BT = BT;
	Ctx.BTGraph = BTGraph;
	Ctx.NodeCount = 0;

	UBehaviorTreeGraphNode* FirstNode = BuildNodeFromSpec(*RootObjPtr, RootGraphNode, 0, 0, Ctx);

	// If the build produced zero usable nodes, refuse to save a husk asset.
	// Pre-validation should have caught this for root, but children-only failures
	// could still reach here in pathological cases.
	if (Ctx.NodeCount == 0 || !FirstNode)
	{
		FString Msg = TEXT("All spec nodes failed to resolve — refusing to save zero-node BT.");
		if (Ctx.Warnings.Num() > 0)
		{
			Msg += TEXT(" Warnings:");
			for (const FString& W : Ctx.Warnings)
			{
				Msg += TEXT("\n  - ") + W;
			}
		}
		return FMonolithActionResult::Error(Msg);
	}

	// Ensure all graph nodes have their graph reference set before UpdateAsset
	for (UEdGraphNode* Node : BTGraph->Nodes)
	{
		if (Node && Node->GetGraph() != BTGraph)
		{
			Node->Rename(nullptr, BTGraph);
		}
	}

	// Update the runtime tree from the editor graph
	BTGraph->UpdateAsset(0);
	BT->MarkPackageDirty();

	// Build result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetStringField(TEXT("name"), AssetName);
	Result->SetNumberField(TEXT("node_count"), Ctx.NodeCount);
	if (BT->BlackboardAsset)
	{
		Result->SetStringField(TEXT("blackboard"), BT->BlackboardAsset->GetPathName());
	}

	// Always emit skipped_nodes (may be empty); keep legacy 'warnings' too for back-compat.
	TArray<TSharedPtr<FJsonValue>> SkippedArr;
	if (Ctx.Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Ctx.Warnings)
		{
			WarnArr.Add(MakeShared<FJsonValueString>(W));
			SkippedArr.Add(MakeShared<FJsonValueString>(W));
		}
		Result->SetArrayField(TEXT("warnings"), WarnArr);
		Result->SetStringField(TEXT("warning"), FString::Printf(
			TEXT("%d spec items were skipped during build — see skipped_nodes. Use strict_mode=true to abort instead."), Ctx.Warnings.Num()));
	}
	Result->SetArrayField(TEXT("skipped_nodes"), SkippedArr);

	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Behavior Tree built from spec — %d nodes created"), Ctx.NodeCount));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  33. export_bt_spec
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleExportBTSpec(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	// Find root
	UBehaviorTreeGraphNode_Root* RootGraphNode = FindRootNode(BTGraph);
	if (!RootGraphNode)
	{
		return FMonolithActionResult::Error(TEXT("Root graph node not found"));
	}

	// The root's first child is the actual tree root (root graph node is just the entry point)
	TArray<UBehaviorTreeGraphNode*> RootChildren = GetChildNodes(RootGraphNode);
	if (RootChildren.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Behavior Tree is empty — no nodes under root"));
	}

	// Build the spec
	TSharedPtr<FJsonObject> SpecObj = MakeShared<FJsonObject>();

	// Blackboard reference
	if (BT->BlackboardAsset)
	{
		SpecObj->SetStringField(TEXT("blackboard_path"), BT->BlackboardAsset->GetPathName());
	}

	// Export the first child as root (typically a BT has one top-level composite)
	TSharedPtr<FJsonObject> RootSpec = ExportNodeToSpec(RootChildren[0]);
	if (RootSpec.IsValid())
	{
		SpecObj->SetObjectField(TEXT("root"), RootSpec);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetObjectField(TEXT("spec"), SpecObj);
	Result->SetStringField(TEXT("message"), TEXT("Behavior Tree exported as spec"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  34. import_bt_spec
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleImportBTSpec(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	const TSharedPtr<FJsonObject>* SpecObjPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("spec"), SpecObjPtr) || !SpecObjPtr || !(*SpecObjPtr).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: spec (JSON object)"));
	}
	const TSharedPtr<FJsonObject>& Spec = *SpecObjPtr;

	const TSharedPtr<FJsonObject>* RootObjPtr = nullptr;
	if (!Spec->TryGetObjectField(TEXT("root"), RootObjPtr) || !RootObjPtr || !(*RootObjPtr).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Spec missing 'root' field"));
	}

	// Update blackboard if specified
	FString BBPath = Spec->GetStringField(TEXT("blackboard_path"));
	if (!BBPath.IsEmpty())
	{
		UBlackboardData* BB = Cast<UBlackboardData>(FMonolithAssetUtils::LoadAssetByPath(UBlackboardData::StaticClass(), BBPath));
		if (BB)
		{
			BT->BlackboardAsset = BB;
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Import BT Spec")));
	BTGraph->Modify();

	// Remove all existing nodes except root
	UBehaviorTreeGraphNode_Root* RootGraphNode = FindRootNode(BTGraph);
	if (!RootGraphNode)
	{
		return FMonolithActionResult::Error(TEXT("Root graph node not found"));
	}

	// Collect and remove existing non-root nodes
	TArray<UEdGraphNode*> NodesToRemove;
	for (UEdGraphNode* Node : BTGraph->Nodes)
	{
		if (Node != RootGraphNode)
		{
			NodesToRemove.Add(Node);
		}
	}
	for (UEdGraphNode* Node : NodesToRemove)
	{
		// Break all pin connections
		for (UEdGraphPin* Pin : Node->Pins)
		{
			Pin->BreakAllPinLinks();
		}
		if (UBehaviorTreeGraphNode* BTGNode = Cast<UBehaviorTreeGraphNode>(Node))
		{
			BTGNode->RemoveAllSubNodes();
		}
		Node->DestroyNode();
	}

	// Break root's output connections
	UEdGraphPin* RootOut = RootGraphNode->GetOutputPin();
	if (RootOut)
	{
		RootOut->BreakAllPinLinks();
	}

	// Rebuild tree from spec
	FSpecBuildContext Ctx;
	Ctx.BT = BT;
	Ctx.BTGraph = BTGraph;
	Ctx.NodeCount = 0;

	BuildNodeFromSpec(*RootObjPtr, RootGraphNode, 0, 0, Ctx);

	BTGraph->UpdateAsset(0);
	BT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("node_count"), Ctx.NodeCount);
	if (Ctx.Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Ctx.Warnings)
		{
			WarnArr.Add(MakeShared<FJsonValueString>(W));
		}
		Result->SetArrayField(TEXT("warnings"), WarnArr);
	}
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("BT rebuilt from spec — %d nodes"), Ctx.NodeCount));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  37. validate_behavior_tree
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleValidateBehaviorTree(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Issues;

	// Collect valid BB keys
	TSet<FName> ValidBBKeys;
	if (BT->BlackboardAsset)
	{
		// Collect own keys
		for (const FBlackboardEntry& Entry : BT->BlackboardAsset->Keys)
		{
			ValidBBKeys.Add(Entry.EntryName);
		}
#if WITH_EDITORONLY_DATA
		BT->BlackboardAsset->UpdateParentKeys();
		for (const FBlackboardEntry& Entry : BT->BlackboardAsset->ParentKeys)
		{
			ValidBBKeys.Add(Entry.EntryName);
		}
#else
		// Walk parent chain
		const UBlackboardData* ParentBB = BT->BlackboardAsset->Parent;
		while (ParentBB)
		{
			for (const FBlackboardEntry& Entry : ParentBB->Keys)
			{
				ValidBBKeys.Add(Entry.EntryName);
			}
			ParentBB = ParentBB->Parent;
		}
#endif
	}
	else
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), TEXT("warning"));
		Issue->SetStringField(TEXT("message"), TEXT("No Blackboard assigned to this Behavior Tree"));
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	}

	// Find root
	UBehaviorTreeGraphNode_Root* RootGraphNode = FindRootNode(BTGraph);
	if (!RootGraphNode)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), TEXT("error"));
		Issue->SetStringField(TEXT("message"), TEXT("No root node found in BT graph"));
		Issues.Add(MakeShared<FJsonValueObject>(Issue));

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetArrayField(TEXT("issues"), Issues);
		Result->SetNumberField(TEXT("issue_count"), Issues.Num());
		Result->SetBoolField(TEXT("valid"), false);
		return FMonolithActionResult::Success(Result);
	}

	// Check root has at least one child
	TArray<UBehaviorTreeGraphNode*> RootChildren = GetChildNodes(RootGraphNode);
	if (RootChildren.Num() == 0)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), TEXT("error"));
		Issue->SetStringField(TEXT("message"), TEXT("Root has no children — empty Behavior Tree"));
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	}
	else
	{
		// Check that root's first child is a composite
		UBTNode* FirstChild = Cast<UBTNode>(RootChildren[0]->NodeInstance);
		if (FirstChild && !FirstChild->IsA<UBTCompositeNode>())
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("warning"));
			Issue->SetStringField(TEXT("node_id"), RootChildren[0]->NodeGuid.ToString());
			Issue->SetStringField(TEXT("message"), TEXT("Root's first child is not a composite — usually the top-level should be Selector or Sequence"));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}
	}

	// Walk the tree recursively and validate
	TSet<const UBehaviorTreeGraphNode*> Visited;
	for (UBehaviorTreeGraphNode* Child : RootChildren)
	{
		ValidateGraphNode(Child, ValidBBKeys, Issues, Visited);
	}

	// Check for orphan nodes (not reachable from root)
	for (UEdGraphNode* Node : BTGraph->Nodes)
	{
		UBehaviorTreeGraphNode* BTGNode = Cast<UBehaviorTreeGraphNode>(Node);
		if (!BTGNode) continue;
		if (Cast<UBehaviorTreeGraphNode_Root>(BTGNode)) continue;
		// Skip decorator/service sub-nodes (they're attached via AddSubNode, not pins)
		if (Cast<UBehaviorTreeGraphNode_Decorator>(BTGNode)) continue;
		if (Cast<UBehaviorTreeGraphNode_Service>(BTGNode)) continue;

		if (!Visited.Contains(BTGNode))
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("warning"));
			Issue->SetStringField(TEXT("node_id"), BTGNode->NodeGuid.ToString());
			if (UBTNode* OrphanBTNode = Cast<UBTNode>(BTGNode->NodeInstance))
			{
				Issue->SetStringField(TEXT("node_class"), OrphanBTNode->GetClass()->GetName());
			}
			Issue->SetStringField(TEXT("message"), TEXT("Orphan node — not reachable from root"));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}
	}

	bool bValid = true;
	for (const TSharedPtr<FJsonValue>& IssueVal : Issues)
	{
		const TSharedPtr<FJsonObject>* IssueObj;
		if (IssueVal->TryGetObject(IssueObj))
		{
			if ((*IssueObj)->GetStringField(TEXT("severity")) == TEXT("error"))
			{
				bValid = false;
				break;
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetBoolField(TEXT("valid"), bValid);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetNumberField(TEXT("bb_key_count"), ValidBBKeys.Num());
	Result->SetStringField(TEXT("message"), bValid
		? FString::Printf(TEXT("Validation passed with %d warnings"), Issues.Num())
		: FString::Printf(TEXT("Validation failed — %d issues found"), Issues.Num()));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  35. clone_bt_subtree
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleCloneBTSubtree(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath, DestPath, NodeIdStr;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("source_path"), SourcePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("node_id"), NodeIdStr, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("dest_path"), DestPath, ErrResult)) return ErrResult;

	FString DestParentIdStr = Params->GetStringField(TEXT("dest_parent_id"));

	// Load source BT
	FString SrcError;
	TSharedPtr<FJsonObject> SrcParams = MakeShared<FJsonObject>();
	SrcParams->SetStringField(TEXT("asset_path"), SourcePath);
	UBehaviorTree* SrcBT = nullptr;
	UBehaviorTreeGraph* SrcGraph = nullptr;
	FString SrcAssetPath;
	if (!LoadBTAndGraph(SrcParams, SrcBT, SrcGraph, SrcAssetPath, SrcError))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source BT: %s"), *SrcError));
	}

	// Find the source subtree root
	UBehaviorTreeGraphNode* SubtreeRoot = nullptr;
	{
		FString GuidErr;
		if (!RequireBtNodeByGuid(SrcGraph, NodeIdStr, TEXT("node_id"), SrcBT->GetName(), SubtreeRoot, GuidErr))
		{
			return FMonolithActionResult::Error(MoveTemp(GuidErr));
		}
	}

	// Export the subtree as spec
	TSharedPtr<FJsonObject> SubtreeSpec = ExportNodeToSpec(SubtreeRoot);
	if (!SubtreeSpec.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Failed to export subtree to spec"));
	}

	// Load dest BT
	TSharedPtr<FJsonObject> DestParams = MakeShared<FJsonObject>();
	DestParams->SetStringField(TEXT("asset_path"), DestPath);
	UBehaviorTree* DestBT = nullptr;
	UBehaviorTreeGraph* DestGraph = nullptr;
	FString DestAssetPath, DestError;
	if (!LoadBTAndGraph(DestParams, DestBT, DestGraph, DestAssetPath, DestError))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Dest BT: %s"), *DestError));
	}

	// Find dest parent (root if not specified)
	UBehaviorTreeGraphNode* DestParent = nullptr;
	if (!DestParentIdStr.IsEmpty())
	{
		FString GuidErr;
		if (!RequireBtNodeByGuid(DestGraph, DestParentIdStr, TEXT("dest_parent_id"), DestBT->GetName(), DestParent, GuidErr))
		{
			return FMonolithActionResult::Error(MoveTemp(GuidErr));
		}
	}
	else
	{
		DestParent = FindRootNode(DestGraph);
		if (!DestParent)
		{
			return FMonolithActionResult::Error(TEXT("Dest BT has no root node"));
		}
	}

	// Use import_bt_spec approach: wrap spec and dispatch build
	// Build a temp spec that imports into the dest tree under the parent
	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Clone BT Subtree")));

	// We need to build the subtree in the destination. Use the BT spec build infrastructure.
	// Simplest approach: export subtree as spec, then use add_bt_node + properties for each node
	// But the most reliable approach: dispatch build_behavior_tree_from_spec on a temp BT, then move nodes.
	// Actually, the cleanest approach: re-use export spec and dispatch node-by-node creation.

	// Recursive node creation via dispatch
	TArray<FString> CreatedNodeIds;
	TArray<FString> Warnings;

	struct FCloneHelper
	{
		static bool CloneNode(
			const TSharedPtr<FJsonObject>& NodeSpec,
			const FString& BTPath,
			const FString& ParentId,
			TArray<FString>& OutCreatedIds,
			TArray<FString>& OutWarnings,
			FMonolithAIBehaviorTreeActions* /*unused*/)
		{
			// Determine node class
			FString NodeType = NodeSpec->GetStringField(TEXT("type"));
			FString NodeClass;
			if (NodeType == TEXT("Selector")) NodeClass = TEXT("BTComposite_Selector");
			else if (NodeType == TEXT("Sequence")) NodeClass = TEXT("BTComposite_Sequence");
			else if (NodeType == TEXT("SimpleParallel")) NodeClass = TEXT("BTComposite_SimpleParallel");
			else NodeClass = NodeType; // Already a class name

			// Add the node
			TSharedPtr<FJsonObject> AddP = MakeShared<FJsonObject>();
			AddP->SetStringField(TEXT("asset_path"), BTPath);
			AddP->SetStringField(TEXT("node_class"), NodeClass);
			if (!ParentId.IsEmpty())
			{
				AddP->SetStringField(TEXT("parent_id"), ParentId);
			}

			// Properties
			const TSharedPtr<FJsonObject>* PropsObj = nullptr;
			if (NodeSpec->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
			{
				AddP->SetObjectField(TEXT("properties"), *PropsObj);
			}

			FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(TEXT("ai"), TEXT("add_bt_node"), AddP);
			if (!R.bSuccess)
			{
				OutWarnings.Add(FString::Printf(TEXT("Failed to add node %s: %s"), *NodeClass, *R.ErrorMessage));
				return false;
			}

			FString NewNodeId;
			if (R.Result.IsValid())
			{
				NewNodeId = R.Result->GetStringField(TEXT("node_id"));
			}
			OutCreatedIds.Add(NewNodeId);

			// Add decorators
			const TArray<TSharedPtr<FJsonValue>>* DecoratorsArr = nullptr;
			if (NodeSpec->TryGetArrayField(TEXT("decorators"), DecoratorsArr) && DecoratorsArr)
			{
				for (const TSharedPtr<FJsonValue>& DecVal : *DecoratorsArr)
				{
					const TSharedPtr<FJsonObject>* DecObj = nullptr;
					if (DecVal->TryGetObject(DecObj) && DecObj && (*DecObj).IsValid())
					{
						TSharedPtr<FJsonObject> DecP = MakeShared<FJsonObject>();
						DecP->SetStringField(TEXT("asset_path"), BTPath);
						DecP->SetStringField(TEXT("node_id"), NewNodeId);
						DecP->SetStringField(TEXT("decorator_class"), (*DecObj)->GetStringField(TEXT("class")));
						const TSharedPtr<FJsonObject>* DecProps = nullptr;
						if ((*DecObj)->TryGetObjectField(TEXT("properties"), DecProps) && DecProps && (*DecProps).IsValid())
						{
							DecP->SetObjectField(TEXT("properties"), *DecProps);
						}
						FMonolithToolRegistry::Get().ExecuteAction(TEXT("ai"), TEXT("add_bt_decorator"), DecP);
					}
				}
			}

			// Add services
			const TArray<TSharedPtr<FJsonValue>>* ServicesArr = nullptr;
			if (NodeSpec->TryGetArrayField(TEXT("services"), ServicesArr) && ServicesArr)
			{
				for (const TSharedPtr<FJsonValue>& SvcVal : *ServicesArr)
				{
					const TSharedPtr<FJsonObject>* SvcObj = nullptr;
					if (SvcVal->TryGetObject(SvcObj) && SvcObj && (*SvcObj).IsValid())
					{
						TSharedPtr<FJsonObject> SvcP = MakeShared<FJsonObject>();
						SvcP->SetStringField(TEXT("asset_path"), BTPath);
						SvcP->SetStringField(TEXT("node_id"), NewNodeId);
						SvcP->SetStringField(TEXT("service_class"), (*SvcObj)->GetStringField(TEXT("class")));
						const TSharedPtr<FJsonObject>* SvcProps = nullptr;
						if ((*SvcObj)->TryGetObjectField(TEXT("properties"), SvcProps) && SvcProps && (*SvcProps).IsValid())
						{
							SvcP->SetObjectField(TEXT("properties"), *SvcProps);
						}
						FMonolithToolRegistry::Get().ExecuteAction(TEXT("ai"), TEXT("add_bt_service"), SvcP);
					}
				}
			}

			// Recurse into children
			const TArray<TSharedPtr<FJsonValue>>* ChildrenArr = nullptr;
			if (NodeSpec->TryGetArrayField(TEXT("children"), ChildrenArr) && ChildrenArr)
			{
				for (const TSharedPtr<FJsonValue>& ChildVal : *ChildrenArr)
				{
					const TSharedPtr<FJsonObject>* ChildObj = nullptr;
					if (ChildVal->TryGetObject(ChildObj) && ChildObj && (*ChildObj).IsValid())
					{
						CloneNode(*ChildObj, BTPath, NewNodeId, OutCreatedIds, OutWarnings, nullptr);
					}
				}
			}

			return true;
		}
	};

	FString ParentIdForClone = DestParentIdStr;
	// If dest_parent_id was empty, we use the root's first composite child's ID, or root ID
	if (ParentIdForClone.IsEmpty())
	{
		UBehaviorTreeGraphNode_Root* RootNode = FindRootNode(DestGraph);
		if (RootNode)
		{
			ParentIdForClone = RootNode->NodeGuid.ToString();
		}
	}

	FCloneHelper::CloneNode(SubtreeSpec, DestPath, ParentIdForClone, CreatedNodeIds, Warnings, nullptr);

	DestBT->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Cloned subtree (%d nodes) from %s to %s"), CreatedNodeIds.Num(), *SourcePath, *DestPath));
	Result->SetStringField(TEXT("source_path"), SourcePath);
	Result->SetStringField(TEXT("dest_path"), DestPath);
	Result->SetNumberField(TEXT("nodes_cloned"), CreatedNodeIds.Num());

	TArray<TSharedPtr<FJsonValue>> IdArr;
	for (const FString& Id : CreatedNodeIds) IdArr.Add(MakeShared<FJsonValueString>(Id));
	Result->SetArrayField(TEXT("created_node_ids"), IdArr);

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings) WarnArr.Add(MakeShared<FJsonValueString>(W));
		Result->SetArrayField(TEXT("warnings"), WarnArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  36. auto_arrange_bt
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleAutoArrangeBT(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString FormatterPref = Params->GetStringField(TEXT("formatter"));
	if (FormatterPref.IsEmpty()) FormatterPref = TEXT("default");

	bool bUsedBA = false;
	int32 NodesFormatted = 0;
	FString FormatterError;

	// Try external formatter (Blueprint Assist) first unless explicitly asking for builtin
	if (FormatterPref != TEXT("builtin"))
	{
		if (IMonolithGraphFormatter::IsAvailable())
		{
			IMonolithGraphFormatter& Formatter = IMonolithGraphFormatter::Get();
			if (Formatter.SupportsGraph(BTGraph))
			{
				FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Auto Arrange BT (BA)")));
				if (Formatter.FormatGraph(BTGraph, NodesFormatted, FormatterError))
				{
					bUsedBA = true;
					BT->MarkPackageDirty();
				}
				else if (FormatterPref == TEXT("blueprint_assist"))
				{
					// Explicitly requested BA but it failed
					return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint Assist formatter failed: %s"), *FormatterError));
				}
			}
		}
		else if (FormatterPref == TEXT("blueprint_assist"))
		{
			return FMonolithActionResult::Error(TEXT("Blueprint Assist formatter not available — is the plugin loaded?"));
		}
	}

	// Fallback: position by depth/breadth
	if (!bUsedBA)
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Auto Arrange BT (builtin)")));

		UBehaviorTreeGraphNode_Root* RootNode = FindRootNode(BTGraph);
		if (!RootNode)
		{
			return FMonolithActionResult::Error(TEXT("No root node found in BT graph"));
		}

		// BFS layout: Y = depth * 200, X = breadth index * 300
		struct FLayoutHelper
		{
			int32 NodeCount = 0;

			void LayoutNode(UBehaviorTreeGraphNode* Node, int32 Depth, int32& BreadthIndex)
			{
				if (!Node) return;

				Node->NodePosX = BreadthIndex * 300;
				Node->NodePosY = Depth * 200;
				NodeCount++;

				TArray<UBehaviorTreeGraphNode*> Children = GetChildNodes(Node);
				if (Children.Num() == 0)
				{
					BreadthIndex++;
					return;
				}

				for (UBehaviorTreeGraphNode* Child : Children)
				{
					LayoutNode(Child, Depth + 1, BreadthIndex);
				}
			}
		};

		FLayoutHelper Layout;
		int32 BreadthIdx = 0;

		// Position root at origin
		RootNode->NodePosX = 0;
		RootNode->NodePosY = 0;

		TArray<UBehaviorTreeGraphNode*> RootChildren = GetChildNodes(RootNode);
		for (UBehaviorTreeGraphNode* Child : RootChildren)
		{
			Layout.LayoutNode(Child, 1, BreadthIdx);
		}

		// Center root above its children
		if (RootChildren.Num() > 0)
		{
			int32 MinX = RootChildren[0]->NodePosX;
			int32 MaxX = RootChildren.Last()->NodePosX;
			RootNode->NodePosX = (MinX + MaxX) / 2;
		}

		NodesFormatted = Layout.NodeCount + 1; // +1 for root
		BT->MarkPackageDirty();
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("formatter_used"), bUsedBA ? TEXT("blueprint_assist") : TEXT("builtin"));
	Result->SetNumberField(TEXT("nodes_formatted"), NodesFormatted);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Auto-arranged %d nodes using %s formatter"),
		NodesFormatted, bUsedBA ? TEXT("Blueprint Assist") : TEXT("builtin depth/breadth")));

	if (!FormatterError.IsEmpty())
	{
		Result->SetStringField(TEXT("formatter_warning"), FormatterError);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  38. compare_behavior_trees
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleCompareBehaviorTrees(const TSharedPtr<FJsonObject>& Params)
{
	FString PathA, PathB;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("path_a"), PathA, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("path_b"), PathB, ErrResult)) return ErrResult;

	// Load both BTs
	TSharedPtr<FJsonObject> ParamsA = MakeShared<FJsonObject>();
	ParamsA->SetStringField(TEXT("asset_path"), PathA);
	UBehaviorTree* BTA = nullptr;
	UBehaviorTreeGraph* GraphA = nullptr;
	FString AssetPathA, ErrorA;
	if (!LoadBTAndGraph(ParamsA, BTA, GraphA, AssetPathA, ErrorA))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("BT A: %s"), *ErrorA));
	}

	TSharedPtr<FJsonObject> ParamsB = MakeShared<FJsonObject>();
	ParamsB->SetStringField(TEXT("asset_path"), PathB);
	UBehaviorTree* BTB = nullptr;
	UBehaviorTreeGraph* GraphB = nullptr;
	FString AssetPathB, ErrorB;
	if (!LoadBTAndGraph(ParamsB, BTB, GraphB, AssetPathB, ErrorB))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("BT B: %s"), *ErrorB));
	}

	// Build node maps for both trees: class+name -> count, and collect all node info
	struct FNodeInfo
	{
		FString NodeClass;
		FString DisplayName;
		FString Category;
		FString NodeId;
		int32 Depth = 0;
		int32 DecoratorCount = 0;
		int32 ServiceCount = 0;
	};

	auto CollectNodes = [](UBehaviorTreeGraph* Graph) -> TArray<FNodeInfo>
	{
		TArray<FNodeInfo> Nodes;
		for (UEdGraphNode* EdNode : Graph->Nodes)
		{
			UBehaviorTreeGraphNode* BTGNode = Cast<UBehaviorTreeGraphNode>(EdNode);
			if (!BTGNode) continue;
			if (Cast<UBehaviorTreeGraphNode_Root>(BTGNode)) continue;
			if (Cast<UBehaviorTreeGraphNode_Decorator>(BTGNode)) continue;
			if (Cast<UBehaviorTreeGraphNode_Service>(BTGNode)) continue;

			UBTNode* BTNode = Cast<UBTNode>(BTGNode->NodeInstance);
			if (!BTNode) continue;

			FNodeInfo Info;
			Info.NodeClass = BTNode->GetClass()->GetName();
			Info.DisplayName = BTNode->GetNodeName();
			Info.NodeId = BTGNode->NodeGuid.ToString();
			Info.DecoratorCount = BTGNode->Decorators.Num();
			Info.ServiceCount = BTGNode->Services.Num();

			if (BTNode->IsA<UBTCompositeNode>()) Info.Category = TEXT("composite");
			else if (BTNode->IsA<UBTTaskNode>()) Info.Category = TEXT("task");

			Nodes.Add(Info);
		}
		return Nodes;
	};

	TArray<FNodeInfo> NodesA = CollectNodes(GraphA);
	TArray<FNodeInfo> NodesB = CollectNodes(GraphB);

	// Build class frequency maps
	TMap<FString, int32> ClassCountA, ClassCountB;
	for (const FNodeInfo& N : NodesA) ClassCountA.FindOrAdd(N.NodeClass)++;
	for (const FNodeInfo& N : NodesB) ClassCountB.FindOrAdd(N.NodeClass)++;

	// Find differences
	TArray<TSharedPtr<FJsonValue>> Differences;

	// Classes in A but not B (or reduced count)
	for (const auto& Pair : ClassCountA)
	{
		int32 CountInB = ClassCountB.Contains(Pair.Key) ? ClassCountB[Pair.Key] : 0;
		if (Pair.Value > CountInB)
		{
			TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
			Diff->SetStringField(TEXT("type"), TEXT("removed_in_b"));
			Diff->SetStringField(TEXT("node_class"), Pair.Key);
			Diff->SetNumberField(TEXT("count_a"), Pair.Value);
			Diff->SetNumberField(TEXT("count_b"), CountInB);
			Differences.Add(MakeShared<FJsonValueObject>(Diff));
		}
	}

	// Classes in B but not A (or increased count)
	for (const auto& Pair : ClassCountB)
	{
		int32 CountInA = ClassCountA.Contains(Pair.Key) ? ClassCountA[Pair.Key] : 0;
		if (Pair.Value > CountInA)
		{
			TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
			Diff->SetStringField(TEXT("type"), TEXT("added_in_b"));
			Diff->SetStringField(TEXT("node_class"), Pair.Key);
			Diff->SetNumberField(TEXT("count_a"), CountInA);
			Diff->SetNumberField(TEXT("count_b"), Pair.Value);
			Differences.Add(MakeShared<FJsonValueObject>(Diff));
		}
	}

	// Structural comparison via spec export
	UBehaviorTreeGraphNode_Root* RootA = FindRootNode(GraphA);
	UBehaviorTreeGraphNode_Root* RootB = FindRootNode(GraphB);

	TSharedPtr<FJsonObject> SpecA, SpecB;
	if (RootA)
	{
		TArray<UBehaviorTreeGraphNode*> ChildrenA = GetChildNodes(RootA);
		if (ChildrenA.Num() > 0) SpecA = ExportNodeToSpec(ChildrenA[0]);
	}
	if (RootB)
	{
		TArray<UBehaviorTreeGraphNode*> ChildrenB = GetChildNodes(RootB);
		if (ChildrenB.Num() > 0) SpecB = ExportNodeToSpec(ChildrenB[0]);
	}

	// Blackboard comparison
	FString BBA_Path = BTA->BlackboardAsset ? BTA->BlackboardAsset->GetPathName() : TEXT("(none)");
	FString BBB_Path = BTB->BlackboardAsset ? BTB->BlackboardAsset->GetPathName() : TEXT("(none)");
	if (BBA_Path != BBB_Path)
	{
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetStringField(TEXT("type"), TEXT("blackboard_different"));
		Diff->SetStringField(TEXT("a"), BBA_Path);
		Diff->SetStringField(TEXT("b"), BBB_Path);
		Differences.Add(MakeShared<FJsonValueObject>(Diff));
	}

	bool bIdentical = Differences.Num() == 0 && NodesA.Num() == NodesB.Num();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path_a"), PathA);
	Result->SetStringField(TEXT("path_b"), PathB);
	Result->SetBoolField(TEXT("identical"), bIdentical);
	Result->SetNumberField(TEXT("node_count_a"), NodesA.Num());
	Result->SetNumberField(TEXT("node_count_b"), NodesB.Num());
	Result->SetNumberField(TEXT("difference_count"), Differences.Num());
	Result->SetArrayField(TEXT("differences"), Differences);

	if (SpecA.IsValid()) Result->SetObjectField(TEXT("spec_a"), SpecA);
	if (SpecB.IsValid()) Result->SetObjectField(TEXT("spec_b"), SpecB);

	Result->SetStringField(TEXT("message"), bIdentical
		? TEXT("Behavior Trees are structurally identical")
		: FString::Printf(TEXT("Found %d differences between the Behavior Trees"), Differences.Num()));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  39. create_bt_task_blueprint
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleCreateBTTaskBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, ErrResult)) return ErrResult;

	FString ParentClassName = Params->GetStringField(TEXT("parent_class"));

	// Resolve parent class
	UClass* ParentClass = UBTTask_BlueprintBase::StaticClass();
	if (!ParentClassName.IsEmpty())
	{
		UClass* Found = FindObject<UClass>(nullptr, *ParentClassName);
		if (!Found)
		{
			Found = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/AIModule.%s"), *ParentClassName));
		}
		if (!Found)
		{
			// Search by short name
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->GetName() == ParentClassName && It->IsChildOf(UBTTaskNode::StaticClass()))
				{
					Found = *It;
					break;
				}
			}
		}
		if (Found && Found->IsChildOf(UBTTaskNode::StaticClass()))
		{
			ParentClass = Found;
		}
		else
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Parent class '%s' not found or not a BTTask"), *ParentClassName));
		}
	}

	FString PathError;
	if (!MonolithAI::EnsureAssetPathFree(SavePath, Name, PathError))
	{
		return FMonolithActionResult::Error(PathError);
	}

	FString PkgError;
	UPackage* Package = MonolithAI::GetOrCreatePackage(SavePath, PkgError);
	if (!Package)
	{
		return FMonolithActionResult::Error(PkgError);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Create BTTask Blueprint")));

	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass, Package, *Name, BPTYPE_Normal, FName(TEXT("MonolithAI")));

	if (!NewBP)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create BTTask Blueprint"));
	}

	FAssetRegistryModule::AssetCreated(NewBP);
	NewBP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(SavePath, TEXT("BTTask Blueprint created"));
	Result->SetStringField(TEXT("name"), Name);
	Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	Result->SetStringField(TEXT("category"), TEXT("task"));

	TArray<TSharedPtr<FJsonValue>> NextSteps;
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Override ReceiveExecuteAI / ReceiveAbortAI events in the Blueprint")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Use the task in a Behavior Tree via add_bt_node")));
	Result->SetArrayField(TEXT("next_steps"), NextSteps);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  40. create_bt_decorator_blueprint
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleCreateBTDecoratorBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, ErrResult)) return ErrResult;

	FString ParentClassName = Params->GetStringField(TEXT("parent_class"));

	UClass* ParentClass = UBTDecorator_BlueprintBase::StaticClass();
	if (!ParentClassName.IsEmpty())
	{
		UClass* Found = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == ParentClassName && It->IsChildOf(UBTDecorator::StaticClass()))
			{
				Found = *It;
				break;
			}
		}
		if (Found)
		{
			ParentClass = Found;
		}
		else
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Parent class '%s' not found or not a BTDecorator"), *ParentClassName));
		}
	}

	FString PathError;
	if (!MonolithAI::EnsureAssetPathFree(SavePath, Name, PathError))
	{
		return FMonolithActionResult::Error(PathError);
	}

	FString PkgError;
	UPackage* Package = MonolithAI::GetOrCreatePackage(SavePath, PkgError);
	if (!Package)
	{
		return FMonolithActionResult::Error(PkgError);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Create BTDecorator Blueprint")));

	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass, Package, *Name, BPTYPE_Normal, FName(TEXT("MonolithAI")));

	if (!NewBP)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create BTDecorator Blueprint"));
	}

	FAssetRegistryModule::AssetCreated(NewBP);
	NewBP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(SavePath, TEXT("BTDecorator Blueprint created"));
	Result->SetStringField(TEXT("name"), Name);
	Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	Result->SetStringField(TEXT("category"), TEXT("decorator"));

	TArray<TSharedPtr<FJsonValue>> NextSteps;
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Override ReceiveConditionCheck / ReceiveObserverActivated in the Blueprint")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Attach to BT nodes via add_bt_decorator")));
	Result->SetArrayField(TEXT("next_steps"), NextSteps);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  41. create_bt_service_blueprint
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleCreateBTServiceBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, ErrResult)) return ErrResult;

	FString ParentClassName = Params->GetStringField(TEXT("parent_class"));

	UClass* ParentClass = UBTService_BlueprintBase::StaticClass();
	if (!ParentClassName.IsEmpty())
	{
		UClass* Found = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == ParentClassName && It->IsChildOf(UBTService::StaticClass()))
			{
				Found = *It;
				break;
			}
		}
		if (Found)
		{
			ParentClass = Found;
		}
		else
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Parent class '%s' not found or not a BTService"), *ParentClassName));
		}
	}

	FString PathError;
	if (!MonolithAI::EnsureAssetPathFree(SavePath, Name, PathError))
	{
		return FMonolithActionResult::Error(PathError);
	}

	FString PkgError;
	UPackage* Package = MonolithAI::GetOrCreatePackage(SavePath, PkgError);
	if (!Package)
	{
		return FMonolithActionResult::Error(PkgError);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Create BTService Blueprint")));

	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass, Package, *Name, BPTYPE_Normal, FName(TEXT("MonolithAI")));

	if (!NewBP)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create BTService Blueprint"));
	}

	FAssetRegistryModule::AssetCreated(NewBP);
	NewBP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(SavePath, TEXT("BTService Blueprint created"));
	Result->SetStringField(TEXT("name"), Name);
	Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	Result->SetStringField(TEXT("category"), TEXT("service"));

	TArray<TSharedPtr<FJsonValue>> NextSteps;
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Override ReceiveTickAI / ReceiveActivationAI in the Blueprint")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Attach to BT nodes via add_bt_service")));
	Result->SetArrayField(TEXT("next_steps"), NextSteps);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  42. generate_bt_diagram
// ============================================================

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleGenerateBTDiagram(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = nullptr;
	UBehaviorTreeGraph* BTGraph = nullptr;
	if (!LoadBTAndGraph(Params, BT, BTGraph, AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString Format = Params->GetStringField(TEXT("format"));
	if (Format.IsEmpty()) Format = TEXT("ascii");

	UBehaviorTreeGraphNode_Root* RootGraphNode = FindRootNode(BTGraph);
	if (!RootGraphNode)
	{
		return FMonolithActionResult::Error(TEXT("No root node found in BT graph"));
	}

	TArray<UBehaviorTreeGraphNode*> RootChildren = GetChildNodes(RootGraphNode);
	if (RootChildren.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Behavior Tree is empty"));
	}

	int32 NodeCount = 0;
	FString Diagram;

	if (Format == TEXT("mermaid"))
	{
		// Mermaid graph TD format
		Diagram = TEXT("graph TD\n");
		int32 IdCounter = 0;

		struct FMermaidHelper
		{
			static void EmitNode(
				UBehaviorTreeGraphNode* GraphNode,
				int32 ParentMermaidId,
				int32& IdCounter,
				FString& OutDiagram,
				int32& OutNodeCount)
			{
				if (!GraphNode) return;

				int32 MyId = IdCounter++;
				OutNodeCount++;

				UBTNode* BTNode = Cast<UBTNode>(GraphNode->NodeInstance);
				FString Label;
				FString Shape;

				if (BTNode)
				{
					FString ClassName = BTNode->GetClass()->GetName();
					FString NodeName = BTNode->GetNodeName();

					if (BTNode->IsA<UBTCompositeNode>())
					{
						// Composites use square brackets
						if (ClassName.Contains(TEXT("Selector"))) Label = TEXT("Selector");
						else if (ClassName.Contains(TEXT("Sequence"))) Label = TEXT("Sequence");
						else if (ClassName.Contains(TEXT("Parallel"))) Label = TEXT("SimpleParallel");
						else Label = ClassName;
						if (!NodeName.IsEmpty()) Label += TEXT(": ") + NodeName;
						Shape = FString::Printf(TEXT("    N%d[\"%s\"]\n"), MyId, *Label);
					}
					else if (BTNode->IsA<UBTTaskNode>())
					{
						// Tasks use rounded brackets
						Label = ClassName;
						if (!NodeName.IsEmpty()) Label += TEXT(": ") + NodeName;
						Shape = FString::Printf(TEXT("    N%d(\"%s\")\n"), MyId, *Label);
					}
					else
					{
						Label = ClassName;
						Shape = FString::Printf(TEXT("    N%d[\"%s\"]\n"), MyId, *Label);
					}
				}
				else
				{
					Label = TEXT("Root");
					Shape = FString::Printf(TEXT("    N%d[\"%s\"]\n"), MyId, *Label);
				}

				OutDiagram += Shape;

				// Decorators as annotations
				for (const UBehaviorTreeGraphNode* Dec : GraphNode->Decorators)
				{
					if (!Dec) continue;
					UBTNode* DecNode = Cast<UBTNode>(Dec->NodeInstance);
					if (DecNode)
					{
						int32 DecId = IdCounter++;
						OutDiagram += FString::Printf(TEXT("    D%d{{\"%s\"}} -.-> N%d\n"),
							DecId, *DecNode->GetClass()->GetName(), MyId);
					}
				}

				// Connection from parent
				if (ParentMermaidId >= 0)
				{
					OutDiagram += FString::Printf(TEXT("    N%d --> N%d\n"), ParentMermaidId, MyId);
				}

				// Recurse children
				TArray<UBehaviorTreeGraphNode*> Children = GetChildNodes(GraphNode);
				for (UBehaviorTreeGraphNode* Child : Children)
				{
					EmitNode(Child, MyId, IdCounter, OutDiagram, OutNodeCount);
				}
			}
		};

		for (UBehaviorTreeGraphNode* Child : RootChildren)
		{
			FMermaidHelper::EmitNode(Child, -1, IdCounter, Diagram, NodeCount);
		}
	}
	else // ASCII
	{
		struct FAsciiHelper
		{
			static void EmitNode(
				UBehaviorTreeGraphNode* GraphNode,
				const FString& Prefix,
				bool bIsLast,
				FString& OutDiagram,
				int32& OutNodeCount)
			{
				if (!GraphNode) return;
				OutNodeCount++;

				UBTNode* BTNode = Cast<UBTNode>(GraphNode->NodeInstance);
				FString NodeLabel;

				if (BTNode)
				{
					FString ClassName = BTNode->GetClass()->GetName();
					FString NodeName = BTNode->GetNodeName();

					if (BTNode->IsA<UBTCompositeNode>())
					{
						if (ClassName.Contains(TEXT("Selector"))) NodeLabel = TEXT("[Selector]");
						else if (ClassName.Contains(TEXT("Sequence"))) NodeLabel = TEXT("[Sequence]");
						else if (ClassName.Contains(TEXT("Parallel"))) NodeLabel = TEXT("[SimpleParallel]");
						else NodeLabel = FString::Printf(TEXT("[%s]"), *ClassName);
					}
					else if (BTNode->IsA<UBTTaskNode>())
					{
						NodeLabel = FString::Printf(TEXT("[Task] %s"), *ClassName);
					}
					else
					{
						NodeLabel = ClassName;
					}

					if (!NodeName.IsEmpty())
					{
						NodeLabel += TEXT(" ") + NodeName;
					}
				}
				else
				{
					NodeLabel = TEXT("[Root]");
				}

				// Emit decorators above the node line
				FString ConnectorStr = bIsLast ? TEXT("\u2514\u2500\u2500 ") : TEXT("\u251C\u2500\u2500 ");
				FString ChildPrefix = Prefix + (bIsLast ? TEXT("    ") : TEXT("\u2502   "));

				for (const UBehaviorTreeGraphNode* Dec : GraphNode->Decorators)
				{
					if (!Dec) continue;
					UBTNode* DecNode = Cast<UBTNode>(Dec->NodeInstance);
					if (DecNode)
					{
						OutDiagram += Prefix + ConnectorStr + FString::Printf(TEXT("{Decorator} %s"), *DecNode->GetClass()->GetName()) + TEXT("\n");
						// After first decorator, use continuation connector
						ConnectorStr = bIsLast ? TEXT("\u2514\u2500\u2500 ") : TEXT("\u251C\u2500\u2500 ");
					}
				}

				// Emit the node itself
				OutDiagram += Prefix + (bIsLast ? TEXT("\u2514\u2500\u2500 ") : TEXT("\u251C\u2500\u2500 ")) + NodeLabel + TEXT("\n");

				// Services below
				for (const UBehaviorTreeGraphNode* Svc : GraphNode->Services)
				{
					if (!Svc) continue;
					UBTNode* SvcNode = Cast<UBTNode>(Svc->NodeInstance);
					if (SvcNode)
					{
						OutDiagram += ChildPrefix + FString::Printf(TEXT("  (Service) %s"), *SvcNode->GetClass()->GetName()) + TEXT("\n");
					}
				}

				// Children
				TArray<UBehaviorTreeGraphNode*> Children = GetChildNodes(GraphNode);
				for (int32 i = 0; i < Children.Num(); i++)
				{
					EmitNode(Children[i], ChildPrefix, i == Children.Num() - 1, OutDiagram, OutNodeCount);
				}
			}
		};

		for (int32 i = 0; i < RootChildren.Num(); i++)
		{
			FAsciiHelper::EmitNode(RootChildren[i], TEXT(""), i == RootChildren.Num() - 1, Diagram, NodeCount);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("format"), Format);
	Result->SetStringField(TEXT("diagram"), Diagram);
	Result->SetNumberField(TEXT("node_count"), NodeCount);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Generated %s diagram with %d nodes"), *Format.ToUpper(), NodeCount));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  F8 (J-phase): get_bt_graph — flat node array with parent_id
// ============================================================
//
// Walks BT->BTGraph->Nodes (UEdGraph node list) and emits a flat array of
// { node_id, node_class, node_name, parent_id, children[] } records, where
// node_id is the editor-assigned NodeGuid. Parent linkage is derived from each
// node's input pin LinkedTo set; children from output pins. The Root node has
// parent_id = null. The action returns root_id pointing at the Root graph
// node's GUID for convenience.
//
// Failure modes (no crashes — clean ok=false):
//   * BT not loadable / wrong type (handled by LoadBehaviorTreeFromParams).
//   * BT has no editor graph yet (returns ok=true with empty nodes array and
//     a "note" field — same fallback policy as get_behavior_tree).

FMonolithActionResult FMonolithAIBehaviorTreeActions::HandleGetBTGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBehaviorTree* BT = MonolithAI::LoadBehaviorTreeFromParams(Params, AssetPath, Error);
	if (!BT)
	{
		return FMonolithActionResult::Error(Error);
	}

#if WITH_EDITORONLY_DATA
	UBehaviorTreeGraph* BTGraph = Cast<UBehaviorTreeGraph>(BT->BTGraph);
	if (!BTGraph)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		const TArray<TSharedPtr<FJsonValue>> EmptyArr;
		Result->SetArrayField(TEXT("nodes"), EmptyArr);
		Result->SetStringField(TEXT("note"), TEXT("No editor graph available — BT may not have been opened in editor yet"));
		return FMonolithActionResult::Success(Result);
	}

	FString RootIdStr;
	TArray<TSharedPtr<FJsonValue>> NodesArr;

	for (UEdGraphNode* GraphNode : BTGraph->Nodes)
	{
		if (!GraphNode) continue;

		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		const FString NodeIdStr = GraphNode->NodeGuid.ToString();
		NodeObj->SetStringField(TEXT("node_id"), NodeIdStr);

		// node_class — prefer the underlying BT node class; fall back to the
		// graph-node class for the Root (which has nullptr NodeInstance by design).
		FString NodeClass;
		FString NodeName;
		if (UBehaviorTreeGraphNode* BTGraphNode = Cast<UBehaviorTreeGraphNode>(GraphNode))
		{
			if (UBTNode* BTNode = Cast<UBTNode>(BTGraphNode->NodeInstance))
			{
				NodeClass = BTNode->GetClass()->GetName();
				NodeName = BTNode->GetNodeName();
			}
		}
		if (NodeClass.IsEmpty())
		{
			NodeClass = GraphNode->GetClass()->GetName();
		}
		if (NodeName.IsEmpty())
		{
			NodeName = GraphNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		}
		NodeObj->SetStringField(TEXT("node_class"), NodeClass);
		NodeObj->SetStringField(TEXT("node_name"), NodeName);

		// Identify the Root by graph-node class — same idiom as
		// HandleGetBehaviorTree (it scans for UBehaviorTreeGraphNode_Root).
		const bool bIsRoot = GraphNode->IsA(UBehaviorTreeGraphNode_Root::StaticClass());
		if (bIsRoot && RootIdStr.IsEmpty())
		{
			RootIdStr = NodeIdStr;
		}

		// Parent: walk input-pin LinkedTo. Multi-parent shouldn't happen in BTs
		// (DAG of single-parent nodes), but we still pick the first to keep
		// the schema scalar.
		FString ParentIdStr;
		for (UEdGraphPin* Pin : GraphNode->Pins)
		{
			if (Pin->Direction != EGPD_Input) continue;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin && LinkedPin->GetOwningNode())
				{
					ParentIdStr = LinkedPin->GetOwningNode()->NodeGuid.ToString();
					break;
				}
			}
			if (!ParentIdStr.IsEmpty()) break;
		}
		if (ParentIdStr.IsEmpty() || bIsRoot)
		{
			NodeObj->SetField(TEXT("parent_id"), MakeShared<FJsonValueNull>());
		}
		else
		{
			NodeObj->SetStringField(TEXT("parent_id"), ParentIdStr);
		}

		// Children: walk output-pin LinkedTo.
		TArray<TSharedPtr<FJsonValue>> ChildrenArr;
		for (UEdGraphPin* Pin : GraphNode->Pins)
		{
			if (Pin->Direction != EGPD_Output) continue;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin && LinkedPin->GetOwningNode())
				{
					ChildrenArr.Add(MakeShared<FJsonValueString>(LinkedPin->GetOwningNode()->NodeGuid.ToString()));
				}
			}
		}
		NodeObj->SetArrayField(TEXT("children"), ChildrenArr);

		NodesArr.Add(MakeShared<FJsonValueObject>(NodeObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	if (!RootIdStr.IsEmpty())
	{
		Result->SetStringField(TEXT("root_id"), RootIdStr);
	}
	else
	{
		Result->SetField(TEXT("root_id"), MakeShared<FJsonValueNull>());
	}
	Result->SetArrayField(TEXT("nodes"), NodesArr);
	Result->SetNumberField(TEXT("node_count"), NodesArr.Num());
	return FMonolithActionResult::Success(Result);

#else
	// Runtime/non-editor builds — Monolith is editor-only, but keep the symbol
	// stable so callers see a clear message rather than a link error.
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), false);
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("error"), TEXT("get_bt_graph requires editor builds (WITH_EDITORONLY_DATA)"));
	return FMonolithActionResult::Success(Result);
#endif
}
