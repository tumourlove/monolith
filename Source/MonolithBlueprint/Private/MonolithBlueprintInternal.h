#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"
#include "MonolithAssetUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Variable.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_BaseMCDelegate.h"
#include "EdGraphNode_Comment.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "UObject/Package.h"

namespace MonolithBlueprintInternal
{
	/**
	 * Try to resolve a Level Blueprint from a level asset path.
	 * Level Blueprints are ULevelScriptBlueprint sub-objects of ULevel,
	 * not top-level assets, so standard LoadAssetByPath<UBlueprint> won't find them.
	 */
	inline UBlueprint* TryLoadLevelBlueprint(const FString& AssetPath)
	{
		// Support "$current" sentinel — return the level BP of the currently-open level
		if (AssetPath == TEXT("$current"))
		{
			if (!GEditor) return nullptr;
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World || !World->PersistentLevel) return nullptr;
			// bDontCreate=false → create the level script BP if it doesn't exist yet
			return World->PersistentLevel->GetLevelScriptBlueprint(false);
		}

		// Try loading the level package
		UPackage* LevelPackage = LoadPackage(nullptr, *AssetPath, LOAD_NoWarn);
		if (!LevelPackage) return nullptr;

		// Find the UWorld in the package, then get its PersistentLevel
		UWorld* World = nullptr;
		ForEachObjectWithPackage(LevelPackage, [&World](UObject* Obj)
		{
			if (UWorld* W = Cast<UWorld>(Obj))
			{
				World = W;
				return false; // stop iteration
			}
			return true; // continue
		});

		if (!World || !World->PersistentLevel) return nullptr;

		// bDontCreate=true → don't create, just return nullptr if no level BP exists
		return World->PersistentLevel->GetLevelScriptBlueprint(true);
	}

	inline UBlueprint* LoadBlueprintFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath)
	{
		OutAssetPath = Params->GetStringField(TEXT("asset_path"));
		if (OutAssetPath.IsEmpty()) return nullptr;

		// Try standard Blueprint asset load first
		UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(OutAssetPath);
		if (BP) return BP;

		// Fallback: try as a Level Blueprint
		// Heuristic: "$current" sentinel, path contains "/Maps/", or standard load failed
		// (cheap to try — if it's not a level package, LoadPackage just returns null)
		return TryLoadLevelBlueprint(OutAssetPath);
	}

	inline void AddGraphArray(
		TArray<TSharedPtr<FJsonValue>>& OutArr,
		const TArray<TObjectPtr<UEdGraph>>& Graphs,
		const FString& Type)
	{
		for (const auto& Graph : Graphs)
		{
			if (!Graph) continue;
			TSharedPtr<FJsonObject> GObj = MakeShared<FJsonObject>();
			GObj->SetStringField(TEXT("name"), Graph->GetName());
			GObj->SetStringField(TEXT("type"), Type);
			GObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
			OutArr.Add(MakeShared<FJsonValueObject>(GObj));
		}
	}

	inline UEdGraph* FindGraphByName(UBlueprint* BP, const FString& GraphName)
	{
		if (GraphName.IsEmpty() && BP->UbergraphPages.Num() > 0)
		{
			return BP->UbergraphPages[0];
		}

		auto SearchArray = [&](const TArray<TObjectPtr<UEdGraph>>& Arr) -> UEdGraph*
		{
			for (const auto& G : Arr)
			{
				if (G && G->GetName() == GraphName) return G;
			}
			return nullptr;
		};

		if (UEdGraph* G = SearchArray(BP->UbergraphPages)) return G;
		if (UEdGraph* G = SearchArray(BP->FunctionGraphs)) return G;
		if (UEdGraph* G = SearchArray(BP->MacroGraphs)) return G;
		if (UEdGraph* G = SearchArray(BP->DelegateSignatureGraphs)) return G;
		return nullptr;
	}

	inline FString PinTypeToString(const FEdGraphPinType& PinType)
	{
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			return TEXT("exec");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
			return TEXT("bool");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
			return TEXT("int");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int64)
			return TEXT("int64");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
			return PinType.PinSubCategory == TEXT("double") ? TEXT("double") : TEXT("float");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_String)
			return TEXT("string");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Name)
			return TEXT("name");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
			return TEXT("text");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
			return TEXT("byte");

		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
			PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
			PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
			PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass ||
			PinType.PinCategory == UEdGraphSchema_K2::PC_Interface)
		{
			FString TypeName = PinType.PinCategory.ToString();
			if (PinType.PinSubCategoryObject.IsValid())
			{
				TypeName += TEXT(":") + PinType.PinSubCategoryObject->GetName();
			}
			return TypeName;
		}
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			if (PinType.PinSubCategoryObject.IsValid())
			{
				return TEXT("struct:") + PinType.PinSubCategoryObject->GetName();
			}
			return TEXT("struct");
		}
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Enum)
		{
			if (PinType.PinSubCategoryObject.IsValid())
			{
				return TEXT("enum:") + PinType.PinSubCategoryObject->GetName();
			}
			return TEXT("enum");
		}
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
			return TEXT("wildcard");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate)
			return TEXT("delegate");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
			return TEXT("multicast_delegate");

		return PinType.PinCategory.ToString();
	}

	inline FString ContainerPrefix(const FEdGraphPinType& PinType)
	{
		switch (PinType.ContainerType)
		{
		case EPinContainerType::Array: return TEXT("array:");
		case EPinContainerType::Set: return TEXT("set:");
		case EPinContainerType::Map: return TEXT("map:");
		default: return TEXT("");
		}
	}

	inline TSharedPtr<FJsonObject> SerializePin(const UEdGraphPin* Pin)
	{
		TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("id"), Pin->PinId.ToString());
		PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinObj->SetStringField(TEXT("direction"),
			Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
		PinObj->SetStringField(TEXT("type"),
			ContainerPrefix(Pin->PinType) + PinTypeToString(Pin->PinType));

		if (!Pin->DefaultValue.IsEmpty())
		{
			PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		}
		if (Pin->DefaultObject)
		{
			PinObj->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
		}

		TArray<TSharedPtr<FJsonValue>> ConnArr;
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (!Linked || !Linked->GetOwningNode()) continue;
			FString ConnId = FString::Printf(TEXT("%s.%s"),
				*Linked->GetOwningNode()->GetName(),
				*Linked->PinName.ToString());
			ConnArr.Add(MakeShared<FJsonValueString>(ConnId));
		}
		PinObj->SetArrayField(TEXT("connected_to"), ConnArr);
		return PinObj;
	}

	inline TSharedPtr<FJsonObject> SerializeNode(UEdGraphNode* Node)
	{
		TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
		NObj->SetStringField(TEXT("id"), Node->GetName());
		NObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		NObj->SetStringField(TEXT("title"),
			Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

		TArray<TSharedPtr<FJsonValue>> PosArr;
		PosArr.Add(MakeShared<FJsonValueNumber>(Node->NodePosX));
		PosArr.Add(MakeShared<FJsonValueNumber>(Node->NodePosY));
		NObj->SetArrayField(TEXT("pos"), PosArr);

		if (!Node->NodeComment.IsEmpty())
		{
			NObj->SetStringField(TEXT("comment"), Node->NodeComment);
		}

		if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
		{
			NObj->SetStringField(TEXT("function"),
				CallNode->FunctionReference.GetMemberName().ToString());
			if (UClass* OwnerClass = CallNode->FunctionReference.GetMemberParentClass())
			{
				NObj->SetStringField(TEXT("function_class"), OwnerClass->GetName());
			}
		}
		else if (UK2Node_ComponentBoundEvent* BoundNode = Cast<UK2Node_ComponentBoundEvent>(Node))
		{
			NObj->SetStringField(TEXT("component_name"),
				BoundNode->ComponentPropertyName.ToString());
			NObj->SetStringField(TEXT("delegate_property_name"),
				BoundNode->DelegatePropertyName.ToString());
			NObj->SetStringField(TEXT("event_name"),
				BoundNode->EventReference.GetMemberName().ToString());
			if (BoundNode->CustomFunctionName != NAME_None)
			{
				NObj->SetStringField(TEXT("custom_name"),
					BoundNode->CustomFunctionName.ToString());
			}
			if (BoundNode->DelegateOwnerClass)
			{
				NObj->SetStringField(TEXT("delegate_owner_class"),
					BoundNode->DelegateOwnerClass->GetName());
			}
		}
		else if (UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(Node))
		{
			NObj->SetStringField(TEXT("delegate_property_name"),
				DelegateNode->DelegateReference.GetMemberName().ToString());
			if (UClass* DelegateOwner = DelegateNode->DelegateReference.GetMemberParentClass())
			{
				NObj->SetStringField(TEXT("delegate_owner_class"), DelegateOwner->GetName());
			}
			NObj->SetBoolField(TEXT("self_context"),
				DelegateNode->DelegateReference.IsSelfContext());
		}
		else if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			NObj->SetStringField(TEXT("event_name"),
				EventNode->EventReference.GetMemberName().ToString());
			if (EventNode->CustomFunctionName != NAME_None)
			{
				NObj->SetStringField(TEXT("custom_name"),
					EventNode->CustomFunctionName.ToString());
			}
		}
		else if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
		{
			if (MacroNode->GetMacroGraph())
			{
				NObj->SetStringField(TEXT("macro_name"),
					MacroNode->GetMacroGraph()->GetName());
			}
		}

		TArray<TSharedPtr<FJsonValue>> PinsArr;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->bHidden) continue;
			PinsArr.Add(MakeShared<FJsonValueObject>(SerializePin(Pin)));
		}
		NObj->SetArrayField(TEXT("pins"), PinsArr);

		return NObj;
	}

	inline TSharedPtr<FJsonObject> TraceExecFlow(
		UEdGraphNode* Node,
		TSet<UEdGraphNode*>& Visited,
		int32 MaxDepth = 100)
	{
		if (!Node || Visited.Contains(Node) || MaxDepth <= 0)
		{
			return nullptr;
		}
		Visited.Add(Node);

		TSharedPtr<FJsonObject> FlowObj = MakeShared<FJsonObject>();
		FlowObj->SetStringField(TEXT("node"),
			Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		FlowObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());

		TArray<UEdGraphPin*> ExecOutputs;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output &&
				Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
				!Pin->bHidden)
			{
				ExecOutputs.Add(Pin);
			}
		}

		if (ExecOutputs.Num() == 1 && ExecOutputs[0]->LinkedTo.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ThenArr;
			for (UEdGraphPin* Linked : ExecOutputs[0]->LinkedTo)
			{
				if (!Linked || !Linked->GetOwningNode()) continue;
				TSharedPtr<FJsonObject> Next = TraceExecFlow(
					Linked->GetOwningNode(), Visited, MaxDepth - 1);
				if (Next)
				{
					ThenArr.Add(MakeShared<FJsonValueObject>(Next));
				}
			}
			if (ThenArr.Num() > 0)
			{
				FlowObj->SetArrayField(TEXT("then"), ThenArr);
			}
		}
		else if (ExecOutputs.Num() > 1)
		{
			TSharedPtr<FJsonObject> BranchesObj = MakeShared<FJsonObject>();
			for (UEdGraphPin* ExecPin : ExecOutputs)
			{
				TArray<TSharedPtr<FJsonValue>> BranchArr;
				for (UEdGraphPin* Linked : ExecPin->LinkedTo)
				{
					if (!Linked || !Linked->GetOwningNode()) continue;
					TSet<UEdGraphNode*> BranchVisited = Visited;
					TSharedPtr<FJsonObject> Next = TraceExecFlow(
						Linked->GetOwningNode(), BranchVisited, MaxDepth - 1);
					if (Next)
					{
						BranchArr.Add(MakeShared<FJsonValueObject>(Next));
					}
				}
				BranchesObj->SetArrayField(ExecPin->PinName.ToString(), BranchArr);
			}
			FlowObj->SetObjectField(TEXT("branches"), BranchesObj);
		}

		return FlowObj;
	}

	inline UEdGraphNode* FindEntryNode(UEdGraph* Graph, const FString& EntryPoint)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				FString EventName = EventNode->EventReference.GetMemberName().ToString();
				if (EventName == EntryPoint || EventNode->GetName() == EntryPoint)
					return Node;
				if (EventNode->CustomFunctionName != NAME_None &&
					EventNode->CustomFunctionName.ToString() == EntryPoint)
					return Node;
				FString DisplayTitle = EventNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				if (DisplayTitle.Contains(EntryPoint))
					return Node;
			}
			if (UK2Node_FunctionEntry* FuncEntry = Cast<UK2Node_FunctionEntry>(Node))
			{
				if (Graph->GetName() == EntryPoint)
					return Node;
			}
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (Cast<UK2Node_Event>(Node) || Cast<UK2Node_FunctionEntry>(Node))
				continue;
			if (Cast<UEdGraphNode_Comment>(Node))
				continue;
			FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			if (Title.Contains(EntryPoint))
				return Node;
		}
		return nullptr;
	}

	// Find a node by its GetName() across all graphs or a specific graph
	inline UEdGraphNode* FindNodeById(UBlueprint* BP, const FString& GraphName, const FString& NodeId)
	{
		auto SearchGraph = [&](UEdGraph* Graph) -> UEdGraphNode*
		{
			if (!Graph) return nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->GetName() == NodeId) return Node;
			}
			return nullptr;
		};

		if (!GraphName.IsEmpty())
		{
			UEdGraph* Graph = FindGraphByName(BP, GraphName);
			return Graph ? SearchGraph(Graph) : nullptr;
		}

		auto SearchGraphs = [&](const TArray<TObjectPtr<UEdGraph>>& Graphs) -> UEdGraphNode*
		{
			for (const auto& G : Graphs)
			{
				if (UEdGraphNode* N = SearchGraph(G)) return N;
			}
			return nullptr;
		};

		if (UEdGraphNode* N = SearchGraphs(BP->UbergraphPages)) return N;
		if (UEdGraphNode* N = SearchGraphs(BP->FunctionGraphs)) return N;
		if (UEdGraphNode* N = SearchGraphs(BP->MacroGraphs)) return N;
		return nullptr;
	}

	// Build a comma-separated list of non-hidden pin names on a node (for error messages)
	inline FString GetAvailablePinNames(UEdGraphNode* Node, EEdGraphPinDirection Direction = EGPD_MAX)
	{
		if (!Node) return TEXT("(none)");
		TArray<FString> Names;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->bHidden) continue;
			if (Direction != EGPD_MAX && Pin->Direction != Direction) continue;
			Names.Add(Pin->PinName.ToString());
		}
		return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : TEXT("(none)");
	}

	// Find a pin on a node by name and optional direction.
	// Tries exact match first, then case-insensitive fallback.
	// If OutAvailablePins is provided, it is populated with available pin names on failure.
	inline UEdGraphPin* FindPinOnNode(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction = EGPD_MAX, FString* OutAvailablePins = nullptr)
	{
		if (!Node) return nullptr;

		// Pass 1: exact match
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString() == PinName)
			{
				if (Direction == EGPD_MAX || Pin->Direction == Direction)
					return Pin;
			}
		}

		// Pass 2: case-insensitive fallback
		FString PinNameLower = PinName.ToLower();
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString().ToLower() == PinNameLower)
			{
				if (Direction == EGPD_MAX || Pin->Direction == Direction)
					return Pin;
			}
		}

		// No match — populate available pins for caller error messages
		if (OutAvailablePins)
		{
			*OutAvailablePins = GetAvailablePinNames(Node, Direction);
		}
		return nullptr;
	}

	/**
	 * Resolve any FObjectProperty by name on a Blueprint's generated class.
	 * Covers SCS / native ActorComponent subobjects (Actor BPs), UMG named
	 * widgets (Widget BPs — UButton, UTextBlock, etc.), and plain object-typed
	 * Blueprint variables — all compile into FObjectProperty entries on the
	 * generated class. Callers needing component/delegate validation must
	 * combine this with FindMulticastDelegateProperty (the effective filter).
	 * Returns null if no FObjectProperty with that name is found, or its
	 * PropertyClass is null.
	 */
	inline FObjectProperty* FindComponentProperty(UBlueprint* BP, FName ComponentName)
	{
		if (!BP || !BP->GeneratedClass) return nullptr;
		FObjectProperty* Prop = FindFProperty<FObjectProperty>(BP->GeneratedClass, ComponentName);
		if (!Prop || !Prop->PropertyClass) return nullptr;
		return Prop;
	}

	/**
	 * Find a BlueprintAssignable multicast delegate property on a class (or any superclass).
	 * Returns null if not found or not BlueprintAssignable.
	 */
	inline FMulticastDelegateProperty* FindMulticastDelegateProperty(UClass* OwnerClass, FName DelegateName)
	{
		if (!OwnerClass) return nullptr;
		FMulticastDelegateProperty* Prop = FindFProperty<FMulticastDelegateProperty>(OwnerClass, DelegateName);
		if (!Prop) return nullptr;
		if (!Prop->HasAnyPropertyFlags(CPF_BlueprintAssignable)) return nullptr;
		return Prop;
	}

	/**
	 * Resolves the delegate owner class + multicast property for a delegate-node
	 * add_node / resolve_node call (AddDelegate, RemoveDelegate, ClearDelegate,
	 * CallDelegate). Mirrors the prefix-normalization the editor's right-click
	 * menu performs — accepts bare and A/U-prefixed class names. SelfContextClass
	 * is used when 'target_class' is empty (e.g. self-context: BP->GeneratedClass);
	 * pass nullptr if no self-context fallback should be attempted.
	 *
	 * NodeTypeLabel goes into error messages (e.g. "AddDelegate", "RemoveDelegate")
	 * so the caller sees which node type's required parameter was missing.
	 *
	 * On success, returns a Success result with OutOwnerClass / OutDelegateProp /
	 * OutbSelfContext populated. On failure, returns an Error result; outputs are
	 * only meaningful when bSuccess is true — do not read them on the error path.
	 */
	inline FMonolithActionResult ResolveDelegateOwnerAndProperty(
		const TSharedPtr<FJsonObject>& Params,
		UClass* SelfContextClass,
		const TCHAR* NodeTypeLabel,
		UClass*& OutOwnerClass,
		FMulticastDelegateProperty*& OutDelegateProp,
		bool& OutbSelfContext)
	{
		FString DelegateNameStr = Params->GetStringField(TEXT("delegate_property_name"));
		if (DelegateNameStr.IsEmpty())
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("%s requires 'delegate_property_name'"), NodeTypeLabel));
		}

		FString TargetClassName = Params->GetStringField(TEXT("target_class"));
		OutbSelfContext = TargetClassName.IsEmpty();

		if (OutbSelfContext)
		{
			if (!SelfContextClass)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("%s requires either target_class or asset_path (for self-context)"),
					NodeTypeLabel));
			}
			OutOwnerClass = SelfContextClass;
		}
		else
		{
			OutOwnerClass = FindFirstObject<UClass>(*TargetClassName, EFindFirstObjectOptions::NativeFirst);
			if (!OutOwnerClass && !TargetClassName.StartsWith(TEXT("A")))
				OutOwnerClass = FindFirstObject<UClass>(*FString::Printf(TEXT("A%s"), *TargetClassName), EFindFirstObjectOptions::NativeFirst);
			if (!OutOwnerClass && !TargetClassName.StartsWith(TEXT("U")))
				OutOwnerClass = FindFirstObject<UClass>(*FString::Printf(TEXT("U%s"), *TargetClassName), EFindFirstObjectOptions::NativeFirst);
			// Strip a leading A/U prefix and retry bare — handles callers passing the C++ class name
			// (e.g. "AMyActor", "UMyComponent") when UE's object registry uses the bare form.
			if (!OutOwnerClass && TargetClassName.Len() > 1 &&
				(TargetClassName.StartsWith(TEXT("A")) || TargetClassName.StartsWith(TEXT("U"))))
				OutOwnerClass = FindFirstObject<UClass>(*TargetClassName.Mid(1), EFindFirstObjectOptions::NativeFirst);
			if (!OutOwnerClass)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("%s target_class '%s' not found"), NodeTypeLabel, *TargetClassName));
			}
		}

		OutDelegateProp = FindMulticastDelegateProperty(OutOwnerClass, FName(*DelegateNameStr));
		if (!OutDelegateProp)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("BlueprintAssignable multicast delegate '%s' not found on class '%s'"),
				*DelegateNameStr, *OutOwnerClass->GetName()));
		}

		// Success-side payload is unused — callers only check bSuccess and read the out params.
		return FMonolithActionResult::Success(MakeShared<FJsonObject>());
	}

	/** Returns true if a UK2Node_CustomEvent with the given name already exists in any graph of the Blueprint */
	bool HasCustomEventNamed(UBlueprint* BP, FName EventName);

	// Parse MCP-friendly type string to FEdGraphPinType
	inline FEdGraphPinType ParsePinTypeFromString(const FString& TypeStr)
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean; // default fallback

		FString BaseType = TypeStr;
		EPinContainerType ContainerType = EPinContainerType::None;

		// Check for container prefix
		if (TypeStr.StartsWith(TEXT("array:")))
		{
			ContainerType = EPinContainerType::Array;
			BaseType = TypeStr.Mid(6);
		}
		else if (TypeStr.StartsWith(TEXT("set:")))
		{
			ContainerType = EPinContainerType::Set;
			BaseType = TypeStr.Mid(4);
		}
		else if (TypeStr.StartsWith(TEXT("map:")))
		{
			ContainerType = EPinContainerType::Map;
			// map:KeyType:ValueType — split on second colon
			int32 SecondColon;
			if (BaseType.Mid(4).FindChar(TEXT(':'), SecondColon))
			{
				BaseType = TypeStr.Mid(4, SecondColon);
				FString ValueType = TypeStr.Mid(4 + SecondColon + 1);
				PinType.PinValueType = FEdGraphTerminalType();
				// Parse value type recursively for the terminal type
				FEdGraphPinType ValPinType = ParsePinTypeFromString(ValueType);
				PinType.PinValueType.TerminalCategory = ValPinType.PinCategory;
				PinType.PinValueType.TerminalSubCategory = ValPinType.PinSubCategory;
				PinType.PinValueType.TerminalSubCategoryObject = ValPinType.PinSubCategoryObject;
			}
			else
			{
				BaseType = TypeStr.Mid(4);
			}
		}

		PinType.ContainerType = ContainerType;

		if (BaseType == TEXT("bool"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		}
		else if (BaseType == TEXT("int"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		}
		else if (BaseType == TEXT("int64"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
		}
		else if (BaseType == TEXT("float"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			PinType.PinSubCategory = TEXT("float");
		}
		else if (BaseType == TEXT("double"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			PinType.PinSubCategory = TEXT("double");
		}
		else if (BaseType == TEXT("string"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_String;
		}
		else if (BaseType == TEXT("name"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
		}
		else if (BaseType == TEXT("text"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
		}
		else if (BaseType == TEXT("byte"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		}
		else if (BaseType.StartsWith(TEXT("object:")))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
			FString ClassName = BaseType.Mid(7);
			UClass* FoundClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
			if (FoundClass) PinType.PinSubCategoryObject = FoundClass;
		}
		else if (BaseType.StartsWith(TEXT("class:")))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Class;
			FString ClassName = BaseType.Mid(6);
			UClass* FoundClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
			if (FoundClass) PinType.PinSubCategoryObject = FoundClass;
		}
		else if (BaseType.StartsWith(TEXT("struct:")))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			FString StructName = BaseType.Mid(7);
			UScriptStruct* FoundStruct = FindFirstObject<UScriptStruct>(*StructName, EFindFirstObjectOptions::NativeFirst);
			if (FoundStruct) PinType.PinSubCategoryObject = FoundStruct;
		}
		else if (BaseType.StartsWith(TEXT("enum:")))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Enum;
			FString EnumName = BaseType.Mid(5);
			UEnum* FoundEnum = FindFirstObject<UEnum>(*EnumName, EFindFirstObjectOptions::NativeFirst);
			if (FoundEnum) PinType.PinSubCategoryObject = FoundEnum;
		}
		else if (BaseType.StartsWith(TEXT("softobject:")))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
			FString ClassName = BaseType.Mid(11);
			UClass* FoundClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
			if (FoundClass) PinType.PinSubCategoryObject = FoundClass;
		}
		else if (BaseType.StartsWith(TEXT("softclass:")))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
			FString ClassName = BaseType.Mid(10);
			UClass* FoundClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
			if (FoundClass) PinType.PinSubCategoryObject = FoundClass;
		}
		else if (BaseType == TEXT("exec"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
		}
		else if (BaseType == TEXT("wildcard"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
		}

		return PinType;
	}
}
