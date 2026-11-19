#include "MonolithBlueprintEventActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithBlueprintNodeGeometry.h"
#include "MonolithParamSchema.h"
#include "MonolithPinTypeGrammar.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Engine/MemberReference.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_BaseAsyncTask.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "K2Node_GenericCreateObject.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"

// ============================================================================
//  Helpers. Named namespace with a file-unique name so the release's forced
//  full-unity pass cannot collide these with helpers in sibling .cpp files.
// ============================================================================

namespace MonolithBpEventActionsHelpers
{
	/** Read an object-valued param. Accepts a real JSON object or a JSON string
	 *  (some MCP transports serialize nested objects to strings). */
	static bool BpEvent_GetObjectParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, TSharedPtr<FJsonObject>& Out)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Params->TryGetObjectField(Field, Obj) && Obj && Obj->IsValid())
		{
			Out = *Obj;
			return true;
		}
		FString Str;
		if (Params->TryGetStringField(Field, Str) && !Str.IsEmpty())
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Str);
			TSharedPtr<FJsonObject> Parsed;
			if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
			{
				Out = Parsed;
				return true;
			}
		}
		return false;
	}

	/** Parse an optional [x, y] "position" array. */
	static bool BpEvent_ParsePosition(const TSharedPtr<FJsonObject>& Params, int32& OutX, int32& OutY)
	{
		const TArray<TSharedPtr<FJsonValue>>* PosArray = nullptr;
		if (Params->TryGetArrayField(TEXT("position"), PosArray) && PosArray && PosArray->Num() >= 2)
		{
			OutX = (int32)(*PosArray)[0]->AsNumber();
			OutY = (int32)(*PosArray)[1]->AsNumber();
			return true;
		}
		return false;
	}

	/** Find a node by id, reporting cross-graph id collisions when no graph is given. */
	static UEdGraphNode* BpEvent_FindNode(UBlueprint* BP, const FString& GraphName, const FString& NodeId, FString& OutError)
	{
		if (NodeId.IsEmpty())
		{
			OutError = TEXT("Missing node id");
			return nullptr;
		}
		TArray<FString> MatchGraphs;
		UEdGraphNode* Node = MonolithBlueprintInternal::FindNodeById(BP, GraphName, NodeId, &MatchGraphs);
		if (!Node)
		{
			OutError = FString::Printf(TEXT("Node '%s' not found%s%s. Use get_graph_data to list node ids."),
				*NodeId, GraphName.IsEmpty() ? TEXT("") : TEXT(" in graph "), *GraphName);
			return nullptr;
		}
		if (GraphName.IsEmpty() && MatchGraphs.Num() > 1)
		{
			OutError = FString::Printf(TEXT("Node id '%s' exists in %d graphs (%s); pass graph_name to disambiguate"),
				*NodeId, MatchGraphs.Num(), *FString::Join(MatchGraphs, TEXT(", ")));
			return nullptr;
		}
		return Node;
	}

	/** Resolve a {node, pin, graph?} reference object to a pin. */
	static UEdGraphPin* BpEvent_ResolvePinRef(UBlueprint* BP, const TSharedPtr<FJsonObject>& Ref, const FString& DefaultGraph,
		EEdGraphPinDirection Direction, const TCHAR* Label, FString& OutError)
	{
		FString NodeId, PinName, GraphName;
		if (!Ref->TryGetStringField(TEXT("node"), NodeId)) Ref->TryGetStringField(TEXT("node_id"), NodeId);
		if (!Ref->TryGetStringField(TEXT("pin"), PinName)) Ref->TryGetStringField(TEXT("pin_name"), PinName);
		if (!Ref->TryGetStringField(TEXT("graph"), GraphName)) GraphName = DefaultGraph;

		if (NodeId.IsEmpty() || PinName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s must be an object {node, pin, graph?}"), Label);
			return nullptr;
		}

		FString FindError;
		UEdGraphNode* Node = BpEvent_FindNode(BP, GraphName, NodeId, FindError);
		if (!Node)
		{
			OutError = FString::Printf(TEXT("%s: %s"), Label, *FindError);
			return nullptr;
		}

		FString Available;
		UEdGraphPin* Pin = MonolithBlueprintInternal::FindPinOnNode(Node, PinName, Direction, &Available);
		if (!Pin)
		{
			OutError = FString::Printf(TEXT("%s: pin '%s' not found on node '%s'. Available: %s"),
				Label, *PinName, *NodeId, *Available);
			return nullptr;
		}
		return Pin;
	}

	/** Resolve a class from a bare name (A/U prefix tolerant), a /Script path, or a Blueprint asset path. */
	static UClass* BpEvent_ResolveClass(const FString& Value)
	{
		if (Value.IsEmpty()) return nullptr;

		if (Value.Contains(TEXT("/")))
		{
			UObject* Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, *Value);
			if (UClass* AsClass = Cast<UClass>(Loaded))
			{
				return AsClass;
			}
			if (UBlueprint* AsBP = Cast<UBlueprint>(Loaded))
			{
				return AsBP->GeneratedClass;
			}
			int32 LastSlash = INDEX_NONE;
			if (!Value.EndsWith(TEXT("_C")) && Value.FindLastChar(TEXT('/'), LastSlash))
			{
				const FString RetryPath = FString::Printf(TEXT("%s.%s_C"), *Value, *Value.Mid(LastSlash + 1));
				return Cast<UClass>(StaticLoadObject(UObject::StaticClass(), nullptr, *RetryPath));
			}
			return nullptr;
		}

		UClass* Found = FindFirstObject<UClass>(*Value, EFindFirstObjectOptions::NativeFirst);
		if (!Found && Value.Len() > 1 && (Value.StartsWith(TEXT("A")) || Value.StartsWith(TEXT("U"))))
		{
			Found = FindFirstObject<UClass>(*Value.Mid(1), EFindFirstObjectOptions::NativeFirst);
		}
		if (!Found && !Value.StartsWith(TEXT("U")))
		{
			Found = FindFirstObject<UClass>(*FString::Printf(TEXT("U%s"), *Value), EFindFirstObjectOptions::NativeFirst);
		}
		if (!Found && !Value.StartsWith(TEXT("A")))
		{
			Found = FindFirstObject<UClass>(*FString::Printf(TEXT("A%s"), *Value), EFindFirstObjectOptions::NativeFirst);
		}
		return Found;
	}

	/** Resolve a K2Node class from "K2Node_Foo", "Foo", "UK2Node_Foo" or a /Script path. */
	static UClass* BpEvent_ResolveNodeClass(const FString& Value)
	{
		if (Value.IsEmpty()) return nullptr;
		if (Value.Contains(TEXT("/")))
		{
			return FindObject<UClass>(nullptr, *Value);
		}
		FString Name = Value;
		if (Name.StartsWith(TEXT("UK2Node_")))
		{
			Name = Name.Mid(1);
		}
		if (!Name.StartsWith(TEXT("K2Node_")))
		{
			Name = TEXT("K2Node_") + Name;
		}
		return FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst);
	}

	/** Collect the custom event by node id or by event name. */
	static UK2Node_CustomEvent* BpEvent_FindCustomEvent(UBlueprint* BP, const FString& GraphName, const FString& NodeId,
		const FString& EventName, FString& OutError)
	{
		if (!NodeId.IsEmpty())
		{
			UEdGraphNode* Node = BpEvent_FindNode(BP, GraphName, NodeId, OutError);
			if (!Node) return nullptr;
			UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node);
			if (!Event)
			{
				OutError = FString::Printf(TEXT("Node '%s' is a %s, not a custom event"), *NodeId, *Node->GetClass()->GetName());
			}
			return Event;
		}
		if (EventName.IsEmpty())
		{
			OutError = TEXT("Pass node_id or event_name to identify the custom event");
			return nullptr;
		}
		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);
		for (UEdGraph* G : AllGraphs)
		{
			if (!G) continue;
			for (UEdGraphNode* N : G->Nodes)
			{
				UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(N);
				if (Event && Event->CustomFunctionName.ToString().Equals(EventName, ESearchCase::IgnoreCase))
				{
					return Event;
				}
			}
		}
		OutError = FString::Printf(TEXT("No custom event named '%s' in this Blueprint"), *EventName);
		return nullptr;
	}

	/** Reconstruct call sites of a self-context function/event so they pick up a changed signature. */
	static int32 BpEvent_RefreshCallSites(UBlueprint* BP, FName FunctionName)
	{
		int32 Refreshed = 0;
		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);
		for (UEdGraph* G : AllGraphs)
		{
			if (!G) continue;
			for (UEdGraphNode* N : G->Nodes)
			{
				UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(N);
				if (!Call || Call->FunctionReference.GetMemberName() != FunctionName) continue;
				const UClass* Parent = Call->FunctionReference.GetMemberParentClass();
				const bool bSelf = Call->FunctionReference.IsSelfContext()
					|| (Parent && (Parent == BP->GeneratedClass || Parent == BP->SkeletonGeneratedClass));
				if (!bSelf) continue;
				Call->ReconstructNode();
				++Refreshed;
			}
		}
		return Refreshed;
	}

	/** Place a node: explicit position wins, otherwise a free slot in the graph. */
	static void BpEvent_PlaceNode(UEdGraph* Graph, UEdGraphNode* Node, bool bExplicit, int32 X, int32 Y)
	{
		if (bExplicit)
		{
			Node->NodePosX = X;
			Node->NodePosY = Y;
		}
		else
		{
			MonolithBlueprintNodeGeometry::PlaceNodeInFreeSlot(Graph, Node);
		}
	}

	static TSharedPtr<FJsonObject> BpEvent_MakeResult(UEdGraphNode* Node, const FString& AssetPath, UEdGraph* Graph, bool bExplicitPos)
	{
		TSharedPtr<FJsonObject> Root = MonolithBlueprintInternal::SerializeNode(Node);
		Root->SetStringField(TEXT("asset_path"), AssetPath);
		Root->SetStringField(TEXT("graph"), Graph->GetName());
		Root->SetStringField(TEXT("position_source"), bExplicitPos ? TEXT("explicit") : TEXT("auto"));
		return Root;
	}

	/** Why a Construct Object from Class node with a literal class pin will not
	 *  compile for ObjectClass; empty when it will. Mirrors the engine's
	 *  file-local FK2Node_GenericCreateObject_Utils::CanSpawnObjectOfClass
	 *  (K2Node_GenericCreateObject.cpp) with bAllowAbstract=false, which is what
	 *  UK2Node_GenericCreateObject::EarlyValidation applies to an unlinked class pin. */
	static FString BpEvent_GenericCreateObjectRejectReason(const UClass* ObjectClass)
	{
		if (!ObjectClass)
		{
			return TEXT("no class was resolved");
		}
		if (ObjectClass->HasAnyClassFlags(CLASS_Abstract))
		{
			return TEXT("the class is abstract; pick a concrete subclass");
		}
		if (ObjectClass->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			return TEXT("the class is deprecated or has been replaced by a newer version");
		}

		static const FName BlueprintTypeName(TEXT("BlueprintType"));
		static const FName NotBlueprintTypeName(TEXT("NotBlueprintType"));
		static const FName DontUseGenericSpawnObjectName(TEXT("DontUseGenericSpawnObject"));

		auto IsClassAllowed = [](const UClass* InClass)
		{
			return InClass != AActor::StaticClass() && InClass != UActorComponent::StaticClass();
		};
		auto ActorReason = [](const UClass* InClass)
		{
			return FString::Printf(TEXT("the class derives from %s; use Spawn Actor from Class or Add Component by Class instead"),
				*InClass->GetName());
		};

		bool bBlueprintType = false;
		const UClass* Current = ObjectClass;
		while (Current)
		{
			if (!IsClassAllowed(Current))
			{
				return ActorReason(Current);
			}
			if (Current->GetBoolMetaData(NotBlueprintTypeName))
			{
				return FString::Printf(TEXT("'%s' is marked NotBlueprintType"), *Current->GetName());
			}
			if (Current->GetBoolMetaData(BlueprintTypeName))
			{
				if (Current->GetBoolMetaData(DontUseGenericSpawnObjectName))
				{
					return FString::Printf(
						TEXT("'%s' is marked DontUseGenericSpawnObject (the type has its own creation node or function, e.g. Create Widget or Create Drag Drop Operation)"),
						*Current->GetName());
				}
				bBlueprintType = true;
				break;
			}
			Current = Current->GetSuperClass();
		}
		if (!bBlueprintType)
		{
			return TEXT("neither the class nor any parent is marked BlueprintType");
		}
		for (; Current; Current = Current->GetSuperClass())
		{
			if (!IsClassAllowed(Current))
			{
				return ActorReason(Current);
			}
		}
		return FString();
	}
}

using namespace MonolithBpEventActionsHelpers;

// ============================================================================
//  Registration
// ============================================================================

void FMonolithBlueprintEventActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_custom_event_input"),
		TEXT("Add an input parameter to a Custom Event node (a user-defined output pin on the event). The type comes from a type token "
			"(same grammar as add_variable / set_function_params: 'float', 'struct:Vector', 'array:object:Actor', 'map:name:int', ...) "
			"or is copied from an existing pin via like_pin {node, pin, graph?} -- handy for arrays, maps and structs. "
			"Refuses events whose signature is driven by a connected delegate. Reconstructs self call sites of the event."),
		FMonolithActionHandler::CreateStatic(&HandleAddCustomEventInput),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("pin_name"), TEXT("string"), TEXT("Name of the new input"), {TEXT("name"), TEXT("param_name")})
			.Optional(TEXT("node_id"), TEXT("string"), TEXT("Custom event node id (from get_graph_data). Either node_id or event_name is required."))
			.Optional(TEXT("event_name"), TEXT("string"), TEXT("Custom event name, used when node_id is omitted"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph containing node_id (searches all graphs if omitted)"))
			.Optional(TEXT("type"), TEXT("string"), TEXT("Type token, e.g. 'int', 'struct:Transform', 'array:struct:Vector', 'object:Actor'. Either type or like_pin is required."))
			.Optional(TEXT("like_pin"), TEXT("object"), TEXT("Copy the type of an existing pin: {node, pin, graph?}. Reference/const flags are dropped unless is_reference is set."))
			.Optional(TEXT("is_reference"), TEXT("bool"), TEXT("Pass the new input by reference (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_event_for_delegate"),
		TEXT("Create a Custom Event whose inputs match a delegate's signature and wire its delegate output to the delegate pin -- "
			"the 'Create matching event' editor action. Works on Bind Event / Unbind Event / Assign nodes (K2Node_AddDelegate, "
			"K2Node_AssignDelegate, K2Node_RemoveDelegate) and on any delegate-typed input pin (e.g. SetTimerByEvent's Event pin, via pin_name)."),
		FMonolithActionHandler::CreateStatic(&HandleAddEventForDelegate),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Node that owns the delegate input pin (Bind Event node, or any node with a delegate input)"))
			.Optional(TEXT("pin_name"), TEXT("string"), TEXT("Delegate input pin name. Defaults to the 'Delegate' pin on Bind/Unbind/Assign nodes; required for other nodes."))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph containing node_id (searches all graphs if omitted). Must be an event graph."))
			.Optional(TEXT("event_name"), TEXT("string"), TEXT("Name for the new custom event (default: '<Delegate>_Event', made unique)"))
			.Optional(TEXT("position"), TEXT("array"), TEXT("[x, y] for the new event (default: to the left of and below the delegate node)"))
			.Optional(TEXT("replace_existing"), TEXT("bool"), TEXT("If the delegate pin is already connected, break that link first (default false: error)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_create_delegate_node"),
		TEXT("Add a Create Event node (K2Node_CreateDelegate) bound to a named function or event in one call. The node's delegate output is "
			"linked to delegate_pin FIRST (the link is what supplies the signature), then the function is set, so HandleAnyChange cannot clear "
			"the selection. The binding is read back and reported. Optionally links the node's Object input to an object output (target_object)."),
		FMonolithActionHandler::CreateStatic(&HandleAddCreateDelegateNode),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("function_name"), TEXT("string"), TEXT("Function or custom event to bind (resolved on the scope class: self, or target_object's class)"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph to add the node to (default: EventGraph)"))
			.Optional(TEXT("delegate_pin"), TEXT("object"), TEXT("Delegate input to link the Create Event output to: {node, pin, graph?} (e.g. a Bind Event node's 'Delegate' pin). Strongly recommended: without it the binding cannot be validated and the editor clears it on the next pin change."))
			.Optional(TEXT("target_object"), TEXT("object"), TEXT("Object output pin that supplies the function's owner: {node, pin, graph?}. Omit to bind on self."))
			.Optional(TEXT("position"), TEXT("array"), TEXT("[x, y] node position (default: free slot)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_async_action_node"),
		TEXT("Add an async action node from a static factory function (the node type the palette makes for UBlueprintAsyncActionBase subclasses "
			"such as UAsyncAction_*). BlueprintInternalUseOnly factories are accepted. Uses UK2Node_AsyncAction::InitializeProxyFromFunction; "
			"other UK2Node_BaseAsyncTask subclasses can be chosen with node_class. Event graphs and macros only."),
		FMonolithActionHandler::CreateStatic(&HandleAddAsyncActionNode),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("factory_class"), TEXT("string"), TEXT("Class owning the static factory function (bare name, /Script path, or Blueprint path)"), {TEXT("target_class"), TEXT("class")})
			.Required(TEXT("function_name"), TEXT("string"), TEXT("Static factory function name (returns the proxy object)"))
			.Optional(TEXT("node_class"), TEXT("string"), TEXT("UK2Node_BaseAsyncTask subclass to spawn (default K2Node_AsyncAction). Use for classes marked HasDedicatedAsyncNode."))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph to add the node to (default: EventGraph)"))
			.Optional(TEXT("position"), TEXT("array"), TEXT("[x, y] node position (default: free slot)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_construct_object_node"),
		TEXT("Add (or retarget) a Construct Object from Class / Create Widget node with its class set and the exposed-on-spawn pins rebuilt "
			"(PinDefaultValueChanged -> OnClassPinChanged). kind: 'construct_object' (K2Node_GenericCreateObject), 'create_widget' "
			"(UMGEditor K2Node_CreateWidget), or pass node_class for any other UK2Node_ConstructObjectFromClass subclass. "
			"Pass node_id to change the class on an existing node instead of creating one."),
		FMonolithActionHandler::CreateStatic(&HandleAddConstructObjectNode),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("class"), TEXT("string"), TEXT("Class to construct: bare name, /Script path, or Blueprint path (e.g. /Game/UI/WBP_Hud)"), {TEXT("object_class"), TEXT("widget_class")})
			.Optional(TEXT("kind"), TEXT("string"), TEXT("'construct_object' (default) or 'create_widget'. Ignored when node_class or node_id is given."), TEXT("construct_object"))
			.Optional(TEXT("node_class"), TEXT("string"), TEXT("Explicit UK2Node_ConstructObjectFromClass subclass, e.g. 'K2Node_CreateWidget'"))
			.Optional(TEXT("node_id"), TEXT("string"), TEXT("Existing ConstructObjectFromClass-family node to retarget instead of creating a new node"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph to add the node to / containing node_id (default: EventGraph)"))
			.Optional(TEXT("position"), TEXT("array"), TEXT("[x, y] node position (default: free slot; ignored with node_id)"))
			.Build());
}

// ============================================================================
//  add_custom_event_input
// ============================================================================

FMonolithActionResult FMonolithBlueprintEventActions::HandleAddCustomEventInput(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	const FString PinName = Params->GetStringField(TEXT("pin_name"));
	if (PinName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: pin_name"));
	}

	FString FindError;
	UK2Node_CustomEvent* Event = BpEvent_FindCustomEvent(BP,
		Params->GetStringField(TEXT("graph_name")),
		Params->GetStringField(TEXT("node_id")),
		Params->GetStringField(TEXT("event_name")), FindError);
	if (!Event)
	{
		return FMonolithActionResult::Error(FindError);
	}

	// Resolve the pin type: explicit token, or copied from an existing pin.
	FEdGraphPinType PinType;
	FString TypeSource;
	const FString TypeStr = Params->GetStringField(TEXT("type"));
	TSharedPtr<FJsonObject> LikeRef;
	const bool bHasLike = BpEvent_GetObjectParam(Params, TEXT("like_pin"), LikeRef);
	if (!TypeStr.IsEmpty() && bHasLike)
	{
		return FMonolithActionResult::Error(TEXT("Pass either 'type' or 'like_pin', not both"));
	}
	if (!TypeStr.IsEmpty())
	{
		FString TypeError;
		if (!MonolithPinTypeGrammar::TryParsePinType(TypeStr, PinType, TypeError))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid type '%s': %s"), *TypeStr, *TypeError));
		}
		TypeSource = TypeStr;
	}
	else if (bHasLike)
	{
		FString RefError;
		UEdGraphPin* Source = BpEvent_ResolvePinRef(BP, LikeRef, FString(), EGPD_MAX, TEXT("like_pin"), RefError);
		if (!Source)
		{
			return FMonolithActionResult::Error(RefError);
		}
		PinType = Source->PinType;
		// A pin's reference/const flags describe how that particular node passes it
		// (array inputs on call nodes are const refs, for instance); they are not
		// part of the value type the caller wants to copy.
		PinType.bIsReference = false;
		PinType.bIsConst = false;
		TypeSource = FString::Printf(TEXT("like_pin %s.%s"), *Source->GetOwningNode()->GetName(), *Source->PinName.ToString());
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Pass 'type' (type token) or 'like_pin' {node, pin}"));
	}

	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
	{
		const bool bIsExec = PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
		if (bHasLike && !bIsExec)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Cannot add a wildcard input to a custom event; the like_pin source (%s) has no concrete type yet. Connect or resolve it first, or pass 'type'."),
				*TypeSource));
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Cannot add %s input to a custom event: custom event inputs carry data only (the event already has its own exec output). Pass a data type token, e.g. 'float' or 'object:Actor'."),
			bIsExec ? TEXT("an exec") : TEXT("a wildcard")));
	}

	bool bIsReference = false;
	if (Params->TryGetBoolField(TEXT("is_reference"), bIsReference))
	{
		PinType.bIsReference = bIsReference;
	}

	for (const TSharedPtr<FUserPinInfo>& Existing : Event->UserDefinedPins)
	{
		if (Existing.IsValid() && Existing->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Custom event '%s' already has an input named '%s'"), *Event->CustomFunctionName.ToString(), *PinName));
		}
	}

	FText CanCreateError;
	if (!Event->CanCreateUserDefinedPin(PinType, EGPD_Output, CanCreateError))
	{
		FString Reason = CanCreateError.ToString();
		if (Reason.IsEmpty() && !Event->IsEditable())
		{
			Reason = TEXT("its signature is driven by a connected delegate (break the delegate link, or change the delegate)");
		}
		return FMonolithActionResult::Error(FString::Printf(TEXT("Custom event '%s' rejected the input: %s"),
			*Event->CustomFunctionName.ToString(), Reason.IsEmpty() ? TEXT("not editable") : *Reason));
	}

	Event->Modify();
	UEdGraphPin* NewPin = Event->CreateUserDefinedPin(FName(*PinName), PinType, EGPD_Output, /*bUseUniqueName=*/false);
	if (!NewPin)
	{
		return FMonolithActionResult::Error(TEXT("CreateUserDefinedPin returned null"));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	const int32 CallSites = BpEvent_RefreshCallSites(BP, Event->CustomFunctionName);

	TSharedPtr<FJsonObject> Root = MonolithBlueprintInternal::SerializeNode(Event);
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("graph"), Event->GetGraph() ? Event->GetGraph()->GetName() : FString());
	Root->SetStringField(TEXT("added_pin"), NewPin->PinName.ToString());
	Root->SetStringField(TEXT("added_pin_type"),
		MonolithBlueprintInternal::ContainerPrefix(NewPin->PinType) + MonolithBlueprintInternal::PinTypeToString(NewPin->PinType));
	Root->SetStringField(TEXT("type_source"), TypeSource);
	Root->SetNumberField(TEXT("call_sites_refreshed"), CallSites);
	return FMonolithActionResult::Success(Root);
}

// ============================================================================
//  add_event_for_delegate
// ============================================================================

FMonolithActionResult FMonolithBlueprintEventActions::HandleAddEventForDelegate(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString FindError;
	UEdGraphNode* SourceNode = BpEvent_FindNode(BP, Params->GetStringField(TEXT("graph_name")),
		Params->GetStringField(TEXT("node_id")), FindError);
	if (!SourceNode)
	{
		return FMonolithActionResult::Error(FindError);
	}

	UEdGraph* Graph = SourceNode->GetGraph();
	const UEdGraphSchema* GraphSchema = Graph ? Graph->GetSchema() : nullptr;
	if (!GraphSchema || GraphSchema->GetGraphType(Graph) != GT_Ubergraph)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Node '%s' is in graph '%s', which is not an event graph; custom events can only live in event graphs"),
			*SourceNode->GetName(), Graph ? *Graph->GetName() : TEXT("?")));
	}

	UEdGraphPin* DelegatePin = nullptr;
	const UFunction* Signature = nullptr;
	FString DelegateLabel;
	const FString PinName = Params->GetStringField(TEXT("pin_name"));

	UK2Node_BaseMCDelegate* MCNode = Cast<UK2Node_BaseMCDelegate>(SourceNode);
	if (MCNode && PinName.IsEmpty())
	{
		DelegatePin = MCNode->GetDelegatePin();
		Signature = MCNode->GetDelegateSignature();
		DelegateLabel = MCNode->GetPropertyName().ToString();
		if (!DelegatePin)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Node '%s' (%s) has no delegate input pin (Call/Clear delegate nodes cannot bind an event)"),
				*SourceNode->GetName(), *SourceNode->GetClass()->GetName()));
		}
	}
	else
	{
		if (PinName.IsEmpty())
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Node '%s' is a %s; pass pin_name naming its delegate input. Delegate inputs: %s"),
				*SourceNode->GetName(), *SourceNode->GetClass()->GetName(),
				*MonolithBlueprintInternal::GetAvailablePinNames(SourceNode, EGPD_Input)));
		}
		FString Available;
		DelegatePin = MonolithBlueprintInternal::FindPinOnNode(SourceNode, PinName, EGPD_Input, &Available);
		if (!DelegatePin)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Input pin '%s' not found on '%s'. Available: %s"),
				*PinName, *SourceNode->GetName(), *Available));
		}
		if (DelegatePin->PinType.PinCategory != UEdGraphSchema_K2::PC_Delegate)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Pin '%s' is %s, not a delegate pin"),
				*PinName, *DelegatePin->PinType.PinCategory.ToString()));
		}
		Signature = FMemberReference::ResolveSimpleMemberReference<UFunction>(
			DelegatePin->PinType.PinSubCategoryMemberReference, BP->SkeletonGeneratedClass);
		DelegateLabel = DelegatePin->PinName.ToString();
	}

	if (!Signature)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Could not resolve the delegate signature for '%s'. If the delegate is a dispatcher on this Blueprint, compile it first."),
			*DelegateLabel));
	}

	const bool bReplace = Params->HasField(TEXT("replace_existing")) && Params->GetBoolField(TEXT("replace_existing"));
	if (DelegatePin->LinkedTo.Num() > 0 && !bReplace)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Delegate pin '%s' on '%s' is already connected; pass replace_existing=true to replace the link"),
			*DelegatePin->PinName.ToString(), *SourceNode->GetName()));
	}

	FString EventName = Params->GetStringField(TEXT("event_name"));
	if (EventName.IsEmpty())
	{
		EventName = FBlueprintEditorUtils::FindUniqueKismetName(BP, FString::Printf(TEXT("%s_Event"), *DelegateLabel)).ToString();
	}
	else if (MonolithBlueprintInternal::HasCustomEventNamed(BP, FName(*EventName)))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("A custom event named '%s' already exists in this Blueprint"), *EventName));
	}

	int32 PosX = SourceNode->NodePosX - 400;
	int32 PosY = SourceNode->NodePosY + 200;
	const bool bExplicitPos = BpEvent_ParsePosition(Params, PosX, PosY);
	if (!bExplicitPos)
	{
		// The default slot is relative to the delegate node, so a replacement event
		// (replace_existing) or a second event for the same node would land exactly
		// on the previous one. Step down until the slot is free.
		for (int32 Attempt = 0; Attempt < 32; ++Attempt)
		{
			const bool bOccupied = Graph->Nodes.ContainsByPredicate([PosX, PosY](const UEdGraphNode* Other)
			{
				return Other && Other->NodePosX == PosX && Other->NodePosY == PosY;
			});
			if (!bOccupied)
			{
				break;
			}
			PosY += 200;
		}
	}

	// Same construction the editor's "Create matching event" uses: user pins for
	// every non-out (or by-ref) parameter of the signature.
	UK2Node_CustomEvent* Event = UK2Node_CustomEvent::CreateFromFunction(
		FVector2D((double)PosX, (double)PosY), Graph, EventName, Signature, /*bSelectNewNode=*/false);
	if (!Event)
	{
		return FMonolithActionResult::Error(TEXT("UK2Node_CustomEvent::CreateFromFunction returned null"));
	}
	if (bExplicitPos)
	{
		Event->NodePosX = PosX;
		Event->NodePosY = PosY;
	}

	if (bReplace)
	{
		DelegatePin->BreakAllPinLinks(/*bNotifyNodes=*/true);
	}

	UEdGraphPin* EventDelegateOut = Event->FindPin(UK2Node_Event::DelegateOutputName);
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	const bool bConnected = EventDelegateOut && K2Schema->TryCreateConnection(EventDelegateOut, DelegatePin);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = BpEvent_MakeResult(Event, AssetPath, Graph, bExplicitPos);
	Root->SetStringField(TEXT("event_name"), Event->CustomFunctionName.ToString());
	Root->SetStringField(TEXT("signature"), Signature->GetName());
	Root->SetBoolField(TEXT("connected"), bConnected);
	Root->SetStringField(TEXT("delegate_node"), SourceNode->GetName());
	Root->SetStringField(TEXT("delegate_pin"), DelegatePin->PinName.ToString());
	if (!bConnected)
	{
		Root->SetStringField(TEXT("warning"),
			TEXT("Event created but the schema refused the delegate link; inspect with get_node_details and connect manually"));
	}
	return FMonolithActionResult::Success(Root);
}

// ============================================================================
//  add_create_delegate_node
// ============================================================================

FMonolithActionResult FMonolithBlueprintEventActions::HandleAddCreateDelegateNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	const FString FunctionNameStr = Params->GetStringField(TEXT("function_name"));
	if (FunctionNameStr.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: function_name"));
	}
	const FName FunctionName(*FunctionNameStr);

	const FString GraphName = Params->GetStringField(TEXT("graph_name"));
	UEdGraph* Graph = MonolithBlueprintInternal::FindGraphByName(BP, GraphName);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Graph not found: %s"), GraphName.IsEmpty() ? TEXT("EventGraph") : *GraphName));
	}

	// Resolve the optional pin references before creating anything.
	UEdGraphPin* TargetDelegatePin = nullptr;
	TSharedPtr<FJsonObject> DelegateRef;
	if (BpEvent_GetObjectParam(Params, TEXT("delegate_pin"), DelegateRef))
	{
		FString RefError;
		TargetDelegatePin = BpEvent_ResolvePinRef(BP, DelegateRef, Graph->GetName(), EGPD_Input, TEXT("delegate_pin"), RefError);
		if (!TargetDelegatePin)
		{
			return FMonolithActionResult::Error(RefError);
		}
		if (TargetDelegatePin->PinType.PinCategory != UEdGraphSchema_K2::PC_Delegate)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("delegate_pin '%s' is %s, not a delegate input"),
				*TargetDelegatePin->PinName.ToString(), *TargetDelegatePin->PinType.PinCategory.ToString()));
		}
		if (TargetDelegatePin->GetOwningNode()->GetGraph() != Graph)
		{
			return FMonolithActionResult::Error(TEXT("delegate_pin is in a different graph than graph_name"));
		}
	}

	UEdGraphPin* TargetObjectPin = nullptr;
	TSharedPtr<FJsonObject> ObjectRef;
	if (BpEvent_GetObjectParam(Params, TEXT("target_object"), ObjectRef))
	{
		FString RefError;
		TargetObjectPin = BpEvent_ResolvePinRef(BP, ObjectRef, Graph->GetName(), EGPD_Output, TEXT("target_object"), RefError);
		if (!TargetObjectPin)
		{
			return FMonolithActionResult::Error(RefError);
		}
		if (TargetObjectPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Object)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("target_object '%s' is %s, not an object output"),
				*TargetObjectPin->PinName.ToString(), *TargetObjectPin->PinType.PinCategory.ToString()));
		}
	}

	int32 PosX = 0, PosY = 0;
	const bool bExplicitPos = BpEvent_ParsePosition(Params, PosX, PosY);

	UK2Node_CreateDelegate* Node = NewObject<UK2Node_CreateDelegate>(Graph);
	Node->SetFlags(RF_Transactional);
	Graph->Modify();
	Graph->AddNode(Node, /*bUserAction=*/true, /*bSelectNewNode=*/false);
	Node->CreateNewGuid();
	Node->PostPlacedNewNode();
	Node->AllocateDefaultPins();
	BpEvent_PlaceNode(Graph, Node, bExplicitPos, PosX, PosY);

	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	auto FailAndRemove = [BP, Node](const FString& Msg)
	{
		FBlueprintEditorUtils::RemoveNode(BP, Node, /*bDontRecompile=*/true);
		return FMonolithActionResult::Error(Msg);
	};

	// 1) Scope: link the Object input first so GetScopeClass() sees it.
	if (TargetObjectPin)
	{
		UEdGraphPin* ObjIn = Node->GetObjectInPin();
		if (!ObjIn || !K2Schema->TryCreateConnection(TargetObjectPin, ObjIn))
		{
			return FailAndRemove(FString::Printf(TEXT("Could not link target_object '%s' to the Create Event Object pin"),
				*TargetObjectPin->PinName.ToString()));
		}
	}

	// 2) Signature: link the delegate output BEFORE setting the function. While the
	//    output is unlinked, HandleAnyChange (run by every pin-connection change)
	//    clears SelectedFunctionName because no signature can be determined.
	bool bDelegateLinked = false;
	if (TargetDelegatePin)
	{
		UEdGraphPin* DelegateOut = Node->GetDelegateOutPin();
		if (!DelegateOut || !K2Schema->TryCreateConnection(DelegateOut, TargetDelegatePin))
		{
			return FailAndRemove(FString::Printf(TEXT("Could not link the Create Event output to delegate_pin '%s'"),
				*TargetDelegatePin->PinName.ToString()));
		}
		bDelegateLinked = true;
	}

	// 3) Function: resolve on the scope class; regenerate the skeleton once for
	//    self scope so a just-added event/function is visible.
	UClass* ScopeClass = Node->GetScopeClass();
	UFunction* Found = ScopeClass ? ScopeClass->FindFunctionByName(FunctionName) : nullptr;
	if (!Found && !TargetObjectPin)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		ScopeClass = Node->GetScopeClass();
		Found = ScopeClass ? ScopeClass->FindFunctionByName(FunctionName) : nullptr;
	}
	if (!ScopeClass)
	{
		return FailAndRemove(TEXT("Could not determine the scope class for the Create Event node (is target_object typed?)"));
	}
	if (!Found)
	{
		return FailAndRemove(FString::Printf(TEXT("Function or event '%s' not found on scope class '%s'"),
			*FunctionNameStr, *ScopeClass->GetName()));
	}
	if (!UEdGraphSchema_K2::FunctionCanBeUsedInDelegate(Found))
	{
		return FailAndRemove(FString::Printf(TEXT("'%s' cannot be bound to a delegate (pure, latent, or deprecated functions are not bindable)"),
			*FunctionNameStr));
	}

	const UFunction* Signature = bDelegateLinked ? Node->GetDelegateSignature() : nullptr;
	if (Signature && !Signature->IsSignatureCompatibleWith(Found))
	{
		return FailAndRemove(FString::Printf(TEXT("'%s' does not match the delegate signature '%s'"),
			*FunctionNameStr, *Signature->GetName()));
	}

	Node->SetFunction(Found->GetFName());
	if (bDelegateLinked)
	{
		Node->HandleAnyChange(/*bForceModify=*/true);
	}

	// Read back: the stored selection is the ground truth.
	const FName ReadBack = Node->GetFunctionName();
	if (bDelegateLinked && ReadBack != Found->GetFName())
	{
		return FailAndRemove(FString::Printf(TEXT("Create Event did not keep the binding (read back '%s', expected '%s')"),
			*ReadBack.ToString(), *Found->GetName()));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Root = BpEvent_MakeResult(Node, AssetPath, Graph, bExplicitPos);
	Root->SetStringField(TEXT("selected_function"), ReadBack.ToString());
	Root->SetStringField(TEXT("scope_class"), ScopeClass->GetName());
	Root->SetBoolField(TEXT("delegate_linked"), bDelegateLinked);
	if (Signature)
	{
		Root->SetStringField(TEXT("signature"), Signature->GetName());
	}
	if (!bDelegateLinked)
	{
		Root->SetStringField(TEXT("warning"),
			TEXT("No delegate_pin given: the binding is unvalidated, and connecting the output later re-runs HandleAnyChange. Prefer passing delegate_pin."));
	}
	return FMonolithActionResult::Success(Root);
}

// ============================================================================
//  add_async_action_node
// ============================================================================

FMonolithActionResult FMonolithBlueprintEventActions::HandleAddAsyncActionNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	const FString GraphName = Params->GetStringField(TEXT("graph_name"));
	UEdGraph* Graph = MonolithBlueprintInternal::FindGraphByName(BP, GraphName);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Graph not found: %s"), GraphName.IsEmpty() ? TEXT("EventGraph") : *GraphName));
	}

	const FString FactoryClassStr = Params->GetStringField(TEXT("factory_class"));
	UClass* FactoryClass = BpEvent_ResolveClass(FactoryClassStr);
	if (!FactoryClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("factory_class '%s' not found"), *FactoryClassStr));
	}

	auto ListFactories = [FactoryClass]()
	{
		TArray<FString> Names;
		for (TFieldIterator<UFunction> It(FactoryClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			if (It->HasAnyFunctionFlags(FUNC_Static) && CastField<FObjectProperty>(It->GetReturnProperty()))
			{
				Names.Add(It->GetName());
			}
		}
		return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : FString(TEXT("(none)"));
	};

	const FString FunctionNameStr = Params->GetStringField(TEXT("function_name"));
	UFunction* Factory = FactoryClass->FindFunctionByName(FName(*FunctionNameStr));
	if (!Factory)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Function '%s' not found on '%s'. Static factories: %s"),
			*FunctionNameStr, *FactoryClass->GetName(), *ListFactories()));
	}
	const FObjectProperty* ReturnProp = CastField<FObjectProperty>(Factory->GetReturnProperty());
	if (!Factory->HasAnyFunctionFlags(FUNC_Static) || !ReturnProp || !ReturnProp->PropertyClass)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("'%s' is not a static factory returning a proxy object. Static factories on '%s': %s"),
			*FunctionNameStr, *FactoryClass->GetName(), *ListFactories()));
	}

	// Node class: explicit, else the generic async-action node.
	const FString NodeClassStr = Params->GetStringField(TEXT("node_class"));
	UClass* NodeClass = UK2Node_AsyncAction::StaticClass();
	if (!NodeClassStr.IsEmpty())
	{
		NodeClass = BpEvent_ResolveNodeClass(NodeClassStr);
		if (!NodeClass || !NodeClass->IsChildOf(UK2Node_BaseAsyncTask::StaticClass()) || NodeClass->HasAnyClassFlags(CLASS_Abstract))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("node_class '%s' is not a concrete UK2Node_BaseAsyncTask subclass"), *NodeClassStr));
		}
	}
	else if (!ReturnProp->PropertyClass->IsChildOf(UBlueprintAsyncActionBase::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("'%s' returns '%s', which is not a UBlueprintAsyncActionBase. Pass node_class for its dedicated async node (e.g. K2Node_LatentGameplayTaskCall for gameplay tasks)."),
			*FunctionNameStr, *ReturnProp->PropertyClass->GetName()));
	}

	const UK2Node* NodeCDO = NodeClass->GetDefaultObject<UK2Node>();
	if (NodeCDO && !NodeCDO->IsCompatibleWithGraph(Graph))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("%s cannot be placed in graph '%s' (async nodes need an event graph or macro)"),
			*NodeClass->GetName(), *Graph->GetName()));
	}

	TArray<FString> Warnings;
	if (NodeClass == UK2Node_AsyncAction::StaticClass() && FactoryClass->HasMetaData(TEXT("HasDedicatedAsyncNode")))
	{
		Warnings.Add(FString::Printf(TEXT("'%s' is marked HasDedicatedAsyncNode; the editor uses a dedicated node class for it. Pass node_class to match."),
			*FactoryClass->GetName()));
	}

	int32 PosX = 0, PosY = 0;
	const bool bExplicitPos = BpEvent_ParsePosition(Params, PosX, PosY);

	UK2Node_BaseAsyncTask* Node = NewObject<UK2Node_BaseAsyncTask>(Graph, NodeClass);
	Node->SetFlags(RF_Transactional);
	if (UK2Node_AsyncAction* AsyncAction = Cast<UK2Node_AsyncAction>(Node))
	{
		AsyncAction->InitializeProxyFromFunction(Factory);
	}
	else
	{
		// Other async node classes have no public initializer; the three proxy
		// fields are UPROPERTYs on UK2Node_BaseAsyncTask, so set them by reflection.
		UClass* BaseClass = UK2Node_BaseAsyncTask::StaticClass();
		FNameProperty* FuncNameProp = FindFProperty<FNameProperty>(BaseClass, TEXT("ProxyFactoryFunctionName"));
		FObjectPropertyBase* FactoryClassProp = FindFProperty<FObjectPropertyBase>(BaseClass, TEXT("ProxyFactoryClass"));
		FObjectPropertyBase* ProxyClassProp = FindFProperty<FObjectPropertyBase>(BaseClass, TEXT("ProxyClass"));
		if (!FuncNameProp || !FactoryClassProp || !ProxyClassProp)
		{
			return FMonolithActionResult::Error(TEXT("UK2Node_BaseAsyncTask proxy properties not found by reflection on this engine version"));
		}
		FuncNameProp->SetPropertyValue_InContainer(Node, Factory->GetFName());
		FactoryClassProp->SetObjectPropertyValue_InContainer(Node, Factory->GetOuterUClass());
		ProxyClassProp->SetObjectPropertyValue_InContainer(Node, ReturnProp->PropertyClass);
	}

	Graph->Modify();
	Graph->AddNode(Node, /*bUserAction=*/true, /*bSelectNewNode=*/false);
	Node->CreateNewGuid();
	Node->PostPlacedNewNode();
	Node->AllocateDefaultPins();
	BpEvent_PlaceNode(Graph, Node, bExplicitPos, PosX, PosY);

	if (Node->GetFactoryFunction() != Factory)
	{
		FBlueprintEditorUtils::RemoveNode(BP, Node, /*bDontRecompile=*/true);
		return FMonolithActionResult::Error(FString::Printf(TEXT("%s did not resolve factory '%s' after initialization"),
			*NodeClass->GetName(), *FunctionNameStr));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Root = BpEvent_MakeResult(Node, AssetPath, Graph, bExplicitPos);
	Root->SetStringField(TEXT("node_class"), NodeClass->GetName());
	Root->SetStringField(TEXT("factory_class"), Factory->GetOuterUClass()->GetName());
	Root->SetStringField(TEXT("factory_function"), Factory->GetName());
	Root->SetStringField(TEXT("proxy_class"), ReturnProp->PropertyClass->GetName());
	Root->SetBoolField(TEXT("internal_use_only"), Factory->HasMetaData(TEXT("BlueprintInternalUseOnly")));
	if (Warnings.Num() > 0)
	{
		Root->SetStringField(TEXT("warning"), FString::Join(Warnings, TEXT(" ")));
	}
	return FMonolithActionResult::Success(Root);
}

// ============================================================================
//  add_construct_object_node
// ============================================================================

FMonolithActionResult FMonolithBlueprintEventActions::HandleAddConstructObjectNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	const FString ClassStr = Params->GetStringField(TEXT("class"));
	if (ClassStr.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: class"));
	}

	const FString GraphName = Params->GetStringField(TEXT("graph_name"));
	const FString NodeId = Params->GetStringField(TEXT("node_id"));

	UK2Node_ConstructObjectFromClass* Node = nullptr;
	UEdGraph* Graph = nullptr;
	bool bCreated = false;
	int32 PosX = 0, PosY = 0;
	bool bExplicitPos = false;

	if (!NodeId.IsEmpty())
	{
		FString FindError;
		UEdGraphNode* Existing = BpEvent_FindNode(BP, GraphName, NodeId, FindError);
		if (!Existing)
		{
			return FMonolithActionResult::Error(FindError);
		}
		Node = Cast<UK2Node_ConstructObjectFromClass>(Existing);
		if (!Node)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Node '%s' is a %s, not a ConstructObjectFromClass-family node (Construct Object / Create Widget / Add Component by Class)"),
				*NodeId, *Existing->GetClass()->GetName()));
		}
		Graph = Node->GetGraph();
		Node->Modify();
	}
	else
	{
		Graph = MonolithBlueprintInternal::FindGraphByName(BP, GraphName);
		if (!Graph)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Graph not found: %s"), GraphName.IsEmpty() ? TEXT("EventGraph") : *GraphName));
		}

		UClass* NodeClass = nullptr;
		const FString NodeClassStr = Params->GetStringField(TEXT("node_class"));
		if (!NodeClassStr.IsEmpty())
		{
			NodeClass = BpEvent_ResolveNodeClass(NodeClassStr);
		}
		else
		{
			FString Kind = Params->GetStringField(TEXT("kind"));
			Kind = Kind.IsEmpty() ? FString(TEXT("construct_object")) : Kind.ToLower();
			if (Kind == TEXT("construct_object") || Kind == TEXT("construct") || Kind == TEXT("create_object"))
			{
				NodeClass = UK2Node_GenericCreateObject::StaticClass();
			}
			else if (Kind == TEXT("create_widget") || Kind == TEXT("widget"))
			{
				// K2Node_CreateWidget lives in UMGEditor (private header). Resolve it by
				// path so this module needs no UMGEditor link dependency.
				NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.K2Node_CreateWidget"));
				if (!NodeClass)
				{
					return FMonolithActionResult::Error(TEXT("K2Node_CreateWidget not found: the UMGEditor module is not loaded"));
				}
			}
			else
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Unknown kind '%s'. Use 'construct_object' or 'create_widget', or pass node_class."), *Kind));
			}
		}

		if (!NodeClass || !NodeClass->IsChildOf(UK2Node_ConstructObjectFromClass::StaticClass()) || NodeClass->HasAnyClassFlags(CLASS_Abstract))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("node_class '%s' is not a concrete UK2Node_ConstructObjectFromClass subclass"),
				NodeClass ? *NodeClass->GetName() : *Params->GetStringField(TEXT("node_class"))));
		}

		const UK2Node* NodeCDO = NodeClass->GetDefaultObject<UK2Node>();
		if (NodeCDO && !NodeCDO->IsCompatibleWithGraph(Graph))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("%s cannot be placed in graph '%s' (construction scripts and some graph types are not allowed)"),
				*NodeClass->GetName(), *Graph->GetName()));
		}

		bExplicitPos = BpEvent_ParsePosition(Params, PosX, PosY);

		Node = NewObject<UK2Node_ConstructObjectFromClass>(Graph, NodeClass);
		Node->SetFlags(RF_Transactional);
		Graph->Modify();
		Graph->AddNode(Node, /*bUserAction=*/true, /*bSelectNewNode=*/false);
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		BpEvent_PlaceNode(Graph, Node, bExplicitPos, PosX, PosY);
		bCreated = true;
	}

	UEdGraphPin* ClassPin = Node->GetClassPin();
	if (!ClassPin)
	{
		if (bCreated) FBlueprintEditorUtils::RemoveNode(BP, Node, /*bDontRecompile=*/true);
		return FMonolithActionResult::Error(TEXT("Node has no Class pin"));
	}
	if (ClassPin->LinkedTo.Num() > 0)
	{
		return FMonolithActionResult::Error(TEXT("The Class pin is connected; a connected class pin ignores its default. Disconnect it first."));
	}

	// Resolves bare names / paths / BP assets and enforces the pin's base class
	// (UUserWidget for Create Widget, UObject for Construct Object).
	FString ResolveError;
	UObject* Resolved = MonolithBlueprintInternal::ResolveDefaultObjectForPin(ClassPin, ClassStr, ResolveError);
	if (!Resolved)
	{
		if (bCreated) FBlueprintEditorUtils::RemoveNode(BP, Node, /*bDontRecompile=*/true);
		return FMonolithActionResult::Error(ResolveError);
	}

	// Apply the node's own compile-time class rule up front so a class the node
	// will refuse to construct is rejected here instead of at compile.
	{
		const UClass* ResolvedClass = Cast<UClass>(Resolved);
		FString RejectReason;
		FString NodeLabel;
		if (Node->IsA<UK2Node_GenericCreateObject>())
		{
			RejectReason = BpEvent_GenericCreateObjectRejectReason(ResolvedClass);
			NodeLabel = TEXT("Construct Object from Class");
		}
		else
		{
			const UClass* CreateWidgetNodeClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.K2Node_CreateWidget"));
			if (CreateWidgetNodeClass && Node->IsA(CreateWidgetNodeClass))
			{
				NodeLabel = TEXT("Create Widget");
			}
			else
			{
				NodeLabel = Node->GetClass()->GetName();
			}
			// UUserWidget::CreateInstanceInternal refuses these at runtime; other
			// ConstructObjectFromClass nodes cannot instance them either.
			if (ResolvedClass && ResolvedClass->HasAnyClassFlags(CLASS_Abstract))
			{
				RejectReason = TEXT("the class is abstract; pick a concrete subclass");
			}
			else if (ResolvedClass && ResolvedClass->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				RejectReason = TEXT("the class is deprecated or has been replaced by a newer version");
			}
		}
		if (!RejectReason.IsEmpty())
		{
			if (bCreated) FBlueprintEditorUtils::RemoveNode(BP, Node, /*bDontRecompile=*/true);
			return FMonolithActionResult::Error(FString::Printf(TEXT("%s cannot construct '%s': %s"),
				*NodeLabel, *GetPathNameSafe(Resolved), *RejectReason));
		}
	}

	ClassPin->DefaultObject = Resolved;
	ClassPin->DefaultValue.Reset();
	// Same notification the pin widget sends: rebuilds the exposed-on-spawn pins.
	Node->PinDefaultValueChanged(ClassPin);

	UClass* ReadBack = Node->GetClassToSpawn();
	if (ReadBack != Resolved)
	{
		if (bCreated) FBlueprintEditorUtils::RemoveNode(BP, Node, /*bDontRecompile=*/true);
		return FMonolithActionResult::Error(FString::Printf(TEXT("Class did not stick on the node (read back '%s')"),
			ReadBack ? *ReadBack->GetName() : TEXT("None")));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	// OnClassPinChanged keeps the previous class's spawn pins as orphaned pins when
	// they cannot be rewired. Drop the unlinked ones; keep linked ones (removing
	// them would silently cut wires) and report them separately.
	TArray<TSharedPtr<FJsonValue>> RemovedOrphans;
	TArray<TSharedPtr<FJsonValue>> LinkedOrphans;
	{
		const TArray<UEdGraphPin*> PinsSnapshot = Node->Pins;
		for (UEdGraphPin* Pin : PinsSnapshot)
		{
			if (!Pin || !Pin->bOrphanedPin || Pin->ParentPin || !Node->IsSpawnVarPin(Pin))
			{
				continue;
			}
			bool bLinked = Pin->LinkedTo.Num() > 0;
			for (const UEdGraphPin* SubPin : Pin->SubPins)
			{
				bLinked |= SubPin && SubPin->LinkedTo.Num() > 0;
			}
			const FString PinName = Pin->PinName.ToString();
			if (bLinked)
			{
				LinkedOrphans.Add(MakeShared<FJsonValueString>(PinName));
			}
			else if (Node->RemovePin(Pin))
			{
				RemovedOrphans.Add(MakeShared<FJsonValueString>(PinName));
			}
		}
		if (RemovedOrphans.Num() > 0)
		{
			Graph->NotifyGraphChanged();
		}
	}

	TArray<TSharedPtr<FJsonValue>> SpawnPins;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && !Pin->bOrphanedPin && !Pin->ParentPin && Node->IsSpawnVarPin(Pin))
		{
			SpawnPins.Add(MakeShared<FJsonValueString>(Pin->PinName.ToString()));
		}
	}

	TSharedPtr<FJsonObject> Root = BpEvent_MakeResult(Node, AssetPath, Graph, bExplicitPos || !bCreated);
	Root->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
	Root->SetStringField(TEXT("class_to_spawn"), ReadBack->GetPathName());
	Root->SetBoolField(TEXT("created"), bCreated);
	Root->SetArrayField(TEXT("spawn_pins"), SpawnPins);
	if (RemovedOrphans.Num() > 0)
	{
		Root->SetArrayField(TEXT("removed_orphan_pins"), RemovedOrphans);
	}
	if (LinkedOrphans.Num() > 0)
	{
		Root->SetArrayField(TEXT("orphaned_spawn_pins"), LinkedOrphans);
		Root->SetStringField(TEXT("warning"), FString::Printf(
			TEXT("%d pin(s) from the previous class are still linked and were kept as orphaned pins (%s); the class no longer exposes them, so the Blueprint will not compile until those links are removed"),
			LinkedOrphans.Num(), *FString::JoinBy(LinkedOrphans, TEXT(", "), [](const TSharedPtr<FJsonValue>& V) { return V->AsString(); })));
	}
	return FMonolithActionResult::Success(Root);
}
