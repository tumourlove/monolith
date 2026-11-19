#include "MonolithControlRigWriteActions.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "ControlRigBlueprintLegacy.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMClient.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
// UE 5.8 relocated IRigVMAssetInterface (now an alias of IRigVMEditorAssetInterface).
#include "RigVMEditorAsset.h"
#else
#include "RigVMAsset.h"
#endif
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "UObject/SoftObjectPath.h"

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithControlRigWriteActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// --- get_control_rig_graph ---
	Registry.RegisterAction(TEXT("animation"), TEXT("get_control_rig_graph"),
		TEXT("Read the full RigVM node graph from a Control Rig Blueprint: nodes, pins, connections"),
		FMonolithActionHandler::CreateStatic(&HandleGetControlRigGraph),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("ControlRigBlueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (default: root graph)"))
			.Build());

	// --- add_control_rig_node ---
	Registry.RegisterAction(TEXT("animation"), TEXT("add_control_rig_node"),
		TEXT("Add a rig unit node to a Control Rig graph from a struct path (e.g. RigUnit_SetTransform)"),
		FMonolithActionHandler::CreateStatic(&HandleAddControlRigNode),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("ControlRigBlueprint asset path"))
			.Required(TEXT("struct_path"), TEXT("string"), TEXT("Script struct path, e.g. /Script/ControlRig.RigUnit_SetTransform"))
			.Optional(TEXT("position_x"), TEXT("number"), TEXT("Node X position (default 0)"))
			.Optional(TEXT("position_y"), TEXT("number"), TEXT("Node Y position (default 0)"))
			.Optional(TEXT("node_name"), TEXT("string"), TEXT("Desired node name (auto-uniquified)"))
			.Optional(TEXT("method_name"), TEXT("string"), TEXT("Execute method name (default: Execute)"))
			.Optional(TEXT("pin_defaults"), TEXT("object"), TEXT("Pin default values as {pin_name: value} pairs"))
			.Build());

	// --- connect_control_rig_pins ---
	Registry.RegisterAction(TEXT("animation"), TEXT("connect_control_rig_pins"),
		TEXT("Connect two pins in a Control Rig graph using dot-notation paths (e.g. NodeName.PinName)"),
		FMonolithActionHandler::CreateStatic(&HandleConnectControlRigPins),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("ControlRigBlueprint asset path"))
			.Required(TEXT("source_pin"), TEXT("string"), TEXT("Output pin path, dot-notation: NodeName.PinName"))
			.Required(TEXT("target_pin"), TEXT("string"), TEXT("Input pin path, dot-notation: NodeName.PinName"))
			.Build());

	// --- remove_control_rig_node ---
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_control_rig_node"),
		TEXT("Remove a node (and its links) from a Control Rig graph by node name"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveControlRigNode),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("ControlRigBlueprint asset path"), { TEXT("asset") })
			.Required(TEXT("node_name"), TEXT("string"), TEXT("Node name as reported by get_control_rig_graph"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (default: root graph)"))
			.Build());

	// --- break_control_rig_link ---
	Registry.RegisterAction(TEXT("animation"), TEXT("break_control_rig_link"),
		TEXT("Break a link between two pins (source_pin + target_pin), or every link on one pin (pin). Pin paths use dot-notation: NodeName.PinName"),
		FMonolithActionHandler::CreateStatic(&HandleBreakControlRigLink),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("ControlRigBlueprint asset path"), { TEXT("asset") })
			.Optional(TEXT("source_pin"), TEXT("string"), TEXT("Output pin path of the link (with target_pin)"))
			.Optional(TEXT("target_pin"), TEXT("string"), TEXT("Input pin path of the link (with source_pin)"))
			.Optional(TEXT("pin"), TEXT("string"), TEXT("Break all links on this pin instead of one specific link"), { TEXT("pin_path") })
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (default: root graph)"))
			.Build());

	// --- set_control_rig_pin_default ---
	Registry.RegisterAction(TEXT("animation"), TEXT("set_control_rig_pin_default"),
		TEXT("Set the default value of an existing pin on a Control Rig graph node. Value uses UE text import format, e.g. 1.5, true, (X=1,Y=2,Z=3), (Type=Bone,Name=\"root\")"),
		FMonolithActionHandler::CreateStatic(&HandleSetControlRigPinDefault),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("ControlRigBlueprint asset path"), { TEXT("asset") })
			.Required(TEXT("pin_path"), TEXT("string"), TEXT("Pin path, dot-notation: NodeName.PinName (sub-pins: NodeName.PinName.X)"), { TEXT("pin") })
			.Required(TEXT("value"), TEXT("string"), TEXT("New default value (numbers and booleans are accepted and converted)"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (default: root graph)"))
			.Optional(TEXT("resize_arrays"), TEXT("boolean"), TEXT("Allow array pins to be resized to match the value"), TEXT("true"))
			.Build());

	// --- add_control_rig_variable ---
	Registry.RegisterAction(TEXT("animation"), TEXT("add_control_rig_variable"),
		TEXT("Add a member variable to a Control Rig Blueprint (compiles the rig). Type: bool, float, double, int32, FString, FName, FText, vector, vector2d, rotator, quat, transform, linearcolor, rigelementkey, a struct/enum object path, or TArray<...> of those"),
		FMonolithActionHandler::CreateStatic(&HandleAddControlRigVariable),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("ControlRigBlueprint asset path"), { TEXT("asset") })
			.Required(TEXT("name"), TEXT("string"), TEXT("Variable name (uniquified by the engine if taken)"), { TEXT("variable_name") })
			.Required(TEXT("type"), TEXT("string"), TEXT("Variable type, see action description"), { TEXT("cpp_type") })
			.Optional(TEXT("default"), TEXT("string"), TEXT("Default value in UE text import format"), { TEXT("default_value") })
			.Optional(TEXT("is_public"), TEXT("boolean"), TEXT("Expose the variable on instances (editable per rig instance)"), TEXT("false"))
			.Optional(TEXT("is_read_only"), TEXT("boolean"), TEXT("Mark the variable read-only (BlueprintReadOnly)"), TEXT("false"))
			.Build());
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static UControlRigBlueprint* LoadCRBlueprint(const FString& AssetPath, FString& OutError)
{
	UControlRigBlueprint* CRB = FMonolithAssetUtils::LoadAssetByPath<UControlRigBlueprint>(AssetPath);
	if (!CRB)
	{
		OutError = FString::Printf(TEXT("ControlRigBlueprint not found: %s"), *AssetPath);
	}
	return CRB;
}

static URigVMGraph* GetGraphFromBlueprint(UControlRigBlueprint* CRB, const FString& GraphName, FString& OutError)
{
	if (GraphName.IsEmpty())
	{
		// Get root/default model
		URigVMGraph* Graph = CRB->GetDefaultModel();
		if (!Graph)
		{
			OutError = TEXT("Control Rig has no default graph");
		}
		return Graph;
	}

	// Search by name across all models
	FRigVMClient* Client = static_cast<IRigVMAssetInterface*>(CRB)->GetRigVMClient();
	if (!Client)
	{
		OutError = TEXT("Failed to get RigVMClient");
		return nullptr;
	}

	TArray<URigVMGraph*> AllGraphs = Client->GetAllModels(/*bIncludeFunctionLibrary=*/true, /*bRecursive=*/true);
	for (URigVMGraph* G : AllGraphs)
	{
		if (G && G->GetName() == GraphName)
		{
			return G;
		}
	}

	OutError = FString::Printf(TEXT("Graph not found: %s"), *GraphName);
	return nullptr;
}

static URigVMController* GetControllerForGraph(UControlRigBlueprint* CRB, URigVMGraph* Graph, FString& OutError)
{
	FRigVMClient* Client = static_cast<IRigVMAssetInterface*>(CRB)->GetRigVMClient();
	if (!Client)
	{
		OutError = TEXT("Failed to get RigVMClient");
		return nullptr;
	}

	URigVMController* Controller = Client->GetOrCreateController(Graph);
	if (!Controller)
	{
		OutError = TEXT("Failed to get or create RigVM controller");
	}
	return Controller;
}

static FString PinDirectionToString(ERigVMPinDirection Dir)
{
	switch (Dir)
	{
	case ERigVMPinDirection::Input:   return TEXT("Input");
	case ERigVMPinDirection::Output:  return TEXT("Output");
	case ERigVMPinDirection::IO:      return TEXT("IO");
	case ERigVMPinDirection::Visible: return TEXT("Visible");
	case ERigVMPinDirection::Hidden:  return TEXT("Hidden");
	default:                          return TEXT("Unknown");
	}
}

static TSharedPtr<FJsonObject> SerializePin(URigVMPin* Pin)
{
	TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
	PinObj->SetStringField(TEXT("name"), Pin->GetName());
	PinObj->SetStringField(TEXT("direction"), PinDirectionToString(Pin->GetDirection()));
	PinObj->SetStringField(TEXT("type"), Pin->GetCPPType());
	PinObj->SetStringField(TEXT("default_value"), Pin->GetDefaultValue());
	PinObj->SetStringField(TEXT("pin_path"), Pin->GetPinPath());

	// Connected pins
	TArray<TSharedPtr<FJsonValue>> ConnArr;
	for (URigVMLink* Link : Pin->GetLinks())
	{
		URigVMPin* OtherPin = (Link->GetSourcePin() == Pin) ? Link->GetTargetPin() : Link->GetSourcePin();
		if (OtherPin)
		{
			ConnArr.Add(MakeShared<FJsonValueString>(OtherPin->GetPinPath()));
		}
	}
	if (ConnArr.Num() > 0)
	{
		PinObj->SetArrayField(TEXT("connected_to"), ConnArr);
	}

	// Sub-pins (for struct types)
	const TArray<URigVMPin*>& SubPins = Pin->GetSubPins();
	if (SubPins.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> SubArr;
		for (URigVMPin* SubPin : SubPins)
		{
			SubArr.Add(MakeShared<FJsonValueObject>(SerializePin(SubPin)));
		}
		PinObj->SetArrayField(TEXT("sub_pins"), SubArr);
	}

	return PinObj;
}

// ---------------------------------------------------------------------------
// get_control_rig_graph
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithControlRigWriteActions::HandleGetControlRigGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	FString Error;
	UControlRigBlueprint* CRB = LoadCRBlueprint(AssetPath, Error);
	if (!CRB) return FMonolithActionResult::Error(Error);

	URigVMGraph* Graph = GetGraphFromBlueprint(CRB, GraphName, Error);
	if (!Graph) return FMonolithActionResult::Error(Error);

	// Serialize nodes
	TArray<TSharedPtr<FJsonValue>> NodesArr;
	for (URigVMNode* Node : Graph->GetNodes())
	{
		if (!Node) continue;

		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("name"), Node->GetName());
		NodeObj->SetStringField(TEXT("node_path"), Node->GetNodePath());
		NodeObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());

		// Position
		FVector2D Pos = Node->GetPosition();
		TArray<TSharedPtr<FJsonValue>> PosArr;
		PosArr.Add(MakeShared<FJsonValueNumber>(Pos.X));
		PosArr.Add(MakeShared<FJsonValueNumber>(Pos.Y));
		NodeObj->SetArrayField(TEXT("position"), PosArr);

		// Struct path for unit nodes
		if (URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(Node))
		{
			UScriptStruct* SS = UnitNode->GetScriptStruct();
			if (SS)
			{
				NodeObj->SetStringField(TEXT("struct_path"), SS->GetPathName());
				NodeObj->SetStringField(TEXT("struct_name"), SS->GetName());
			}
		}

		// Pins (top-level only — sub-pins are nested inside)
		TArray<TSharedPtr<FJsonValue>> PinsArr;
		for (URigVMPin* Pin : Node->GetPins())
		{
			if (!Pin) continue;
			PinsArr.Add(MakeShared<FJsonValueObject>(SerializePin(Pin)));
		}
		NodeObj->SetArrayField(TEXT("pins"), PinsArr);

		NodesArr.Add(MakeShared<FJsonValueObject>(NodeObj));
	}

	// Serialize links
	TArray<TSharedPtr<FJsonValue>> LinksArr;
	for (URigVMLink* Link : Graph->GetLinks())
	{
		if (!Link) continue;

		TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
		LinkObj->SetStringField(TEXT("source"), Link->GetSourcePinPath());
		LinkObj->SetStringField(TEXT("target"), Link->GetTargetPinPath());
		LinksArr.Add(MakeShared<FJsonValueObject>(LinkObj));
	}

	// List available sub-graphs
	TArray<TSharedPtr<FJsonValue>> SubGraphArr;
	TArray<URigVMGraph*> ContainedGraphs = Graph->GetContainedGraphs();
	for (URigVMGraph* SubG : ContainedGraphs)
	{
		if (SubG)
		{
			SubGraphArr.Add(MakeShared<FJsonValueString>(SubG->GetName()));
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("graph_name"), Graph->GetName());
	Root->SetNumberField(TEXT("node_count"), NodesArr.Num());
	Root->SetNumberField(TEXT("link_count"), LinksArr.Num());
	Root->SetArrayField(TEXT("nodes"), NodesArr);
	Root->SetArrayField(TEXT("links"), LinksArr);
	if (SubGraphArr.Num() > 0)
	{
		Root->SetArrayField(TEXT("sub_graphs"), SubGraphArr);
	}

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// add_control_rig_node
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithControlRigWriteActions::HandleAddControlRigNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString StructPath = Params->GetStringField(TEXT("struct_path"));

	if (StructPath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("struct_path is required"));

	double PosX = 0, PosY = 0;
	Params->TryGetNumberField(TEXT("position_x"), PosX);
	Params->TryGetNumberField(TEXT("position_y"), PosY);

	FString NodeName;
	Params->TryGetStringField(TEXT("node_name"), NodeName);

	FString MethodName = TEXT("Execute");
	Params->TryGetStringField(TEXT("method_name"), MethodName);

	FString Error;
	UControlRigBlueprint* CRB = LoadCRBlueprint(AssetPath, Error);
	if (!CRB) return FMonolithActionResult::Error(Error);

	URigVMGraph* Graph = CRB->GetDefaultModel();
	if (!Graph) return FMonolithActionResult::Error(TEXT("Control Rig has no default graph"));

	URigVMController* Controller = GetControllerForGraph(CRB, Graph, Error);
	if (!Controller) return FMonolithActionResult::Error(Error);

	// Begin transaction
	GEditor->BeginTransaction(FText::FromString(TEXT("Add Control Rig Node")));
	static_cast<UBlueprint*>(CRB)->Modify();

	// Add the unit node
	URigVMUnitNode* NewNode = Controller->AddUnitNodeFromStructPath(
		StructPath,
		FName(*MethodName),
		FVector2D(PosX, PosY),
		NodeName,
		/*bSetupUndoRedo=*/true,
		/*bPrintPythonCommand=*/false);

	if (!NewNode)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to add unit node from struct path: %s"), *StructPath));
	}

	// Apply pin defaults if provided
	const TSharedPtr<FJsonObject>* PinDefaultsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("pin_defaults"), PinDefaultsObj) && PinDefaultsObj && (*PinDefaultsObj)->Values.Num() > 0)
	{
		for (const auto& KV : (*PinDefaultsObj)->Values)
		{
			FString PinPath = FString::Printf(TEXT("%s.%s"), *NewNode->GetName(), *KV.Key);
			FString Value;

			// Handle different JSON value types
			if (KV.Value->Type == EJson::String)
			{
				Value = KV.Value->AsString();
			}
			else if (KV.Value->Type == EJson::Number)
			{
				Value = FString::SanitizeFloat(KV.Value->AsNumber());
			}
			else if (KV.Value->Type == EJson::Boolean)
			{
				Value = KV.Value->AsBool() ? TEXT("true") : TEXT("false");
			}
			else
			{
				// For objects/arrays, serialize to string
				FString JsonStr;
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
				FJsonSerializer::Serialize(KV.Value, MonolithKeyToString(KV.Key), Writer);
				Value = JsonStr;
			}

			bool bSetOk = Controller->SetPinDefaultValue(PinPath, Value, /*bResizeArrays=*/true, /*bSetupUndoRedo=*/true);
			if (!bSetOk)
			{
				UE_LOG(LogTemp, Warning, TEXT("Monolith: Failed to set pin default %s = %s"), *PinPath, *Value);
			}
		}
	}

	// Reinit the VM
	CRB->RequestRigVMInit();
	CRB->MarkPackageDirty();

	GEditor->EndTransaction();

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("node_name"), NewNode->GetName());
	Root->SetStringField(TEXT("node_path"), NewNode->GetNodePath());

	UScriptStruct* SS = NewNode->GetScriptStruct();
	if (SS)
	{
		Root->SetStringField(TEXT("struct_path"), SS->GetPathName());
		Root->SetStringField(TEXT("struct_name"), SS->GetName());
	}

	FVector2D Pos = NewNode->GetPosition();
	Root->SetNumberField(TEXT("position_x"), Pos.X);
	Root->SetNumberField(TEXT("position_y"), Pos.Y);

	// Return pin names for reference
	TArray<TSharedPtr<FJsonValue>> PinNames;
	for (URigVMPin* Pin : NewNode->GetPins())
	{
		if (!Pin) continue;
		TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("name"), Pin->GetName());
		PinObj->SetStringField(TEXT("direction"), PinDirectionToString(Pin->GetDirection()));
		PinObj->SetStringField(TEXT("type"), Pin->GetCPPType());
		PinNames.Add(MakeShared<FJsonValueObject>(PinObj));
	}
	Root->SetArrayField(TEXT("pins"), PinNames);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// connect_control_rig_pins
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithControlRigWriteActions::HandleConnectControlRigPins(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SourcePin = Params->GetStringField(TEXT("source_pin"));
	FString TargetPin = Params->GetStringField(TEXT("target_pin"));

	if (SourcePin.IsEmpty() || TargetPin.IsEmpty())
		return FMonolithActionResult::Error(TEXT("source_pin and target_pin are required"));

	FString Error;
	UControlRigBlueprint* CRB = LoadCRBlueprint(AssetPath, Error);
	if (!CRB) return FMonolithActionResult::Error(Error);

	URigVMGraph* Graph = CRB->GetDefaultModel();
	if (!Graph) return FMonolithActionResult::Error(TEXT("Control Rig has no default graph"));

	URigVMController* Controller = GetControllerForGraph(CRB, Graph, Error);
	if (!Controller) return FMonolithActionResult::Error(Error);

	// Begin transaction
	GEditor->BeginTransaction(FText::FromString(TEXT("Connect Control Rig Pins")));
	static_cast<UBlueprint*>(CRB)->Modify();

	bool bSuccess = Controller->AddLink(
		SourcePin,
		TargetPin,
		/*bSetupUndoRedo=*/true,
		/*bPrintPythonCommand=*/false);

	if (!bSuccess)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to connect pins: %s -> %s (check pin paths and type compatibility)"),
			*SourcePin, *TargetPin));
	}

	// Reinit the VM
	CRB->RequestRigVMInit();
	CRB->MarkPackageDirty();

	GEditor->EndTransaction();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("source_pin"), SourcePin);
	Root->SetStringField(TEXT("target_pin"), TargetPin);
	Root->SetBoolField(TEXT("connected"), true);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Graph-edit helpers (remove / break / set default / add variable)
// ---------------------------------------------------------------------------

namespace
{
	/** Load the blueprint, resolve the graph (optional graph_name) and its controller. */
	bool CRWrite_ResolveGraphAndController(const TSharedPtr<FJsonObject>& Params, UControlRigBlueprint*& OutCRB,
		URigVMGraph*& OutGraph, URigVMController*& OutController, FString& OutError)
	{
		const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
		FString GraphName;
		Params->TryGetStringField(TEXT("graph_name"), GraphName);

		OutCRB = LoadCRBlueprint(AssetPath, OutError);
		if (!OutCRB) return false;
		OutGraph = GetGraphFromBlueprint(OutCRB, GraphName, OutError);
		if (!OutGraph) return false;
		OutController = GetControllerForGraph(OutCRB, OutGraph, OutError);
		return OutController != nullptr;
	}

	/** Stringify a JSON scalar for SetPinDefaultValue / variable defaults. */
	FString CRWrite_JsonScalarToString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid()) return FString();
		switch (Value->Type)
		{
		case EJson::String:  return Value->AsString();
		case EJson::Number:  return FString::SanitizeFloat(Value->AsNumber());
		case EJson::Boolean: return Value->AsBool() ? TEXT("True") : TEXT("False");
		default:             return FString();
		}
	}

	/**
	 * Map a user-facing type string onto the CPP type path accepted by
	 * IRigVMAssetInterface::AddMemberVariable. The engine conversion
	 * (RigVMTypeUtils::ExternalVariableFromCPPTypePath) hits check(false) on a type it
	 * cannot resolve, so every type is resolved here first and unknown types are rejected.
	 */
	bool CRWrite_ResolveVariableType(const FString& InType, FString& OutCPPTypePath, FString& OutError)
	{
		FString Base = InType.TrimStartAndEnd();
		bool bArray = false;
		if (Base.StartsWith(TEXT("TArray<")) && Base.EndsWith(TEXT(">")))
		{
			Base = Base.Mid(7, Base.Len() - 8).TrimStartAndEnd();
			bArray = true;
		}
		else if (Base.EndsWith(TEXT("[]")))
		{
			Base = Base.LeftChop(2).TrimStartAndEnd();
			bArray = true;
		}
		if (Base.StartsWith(TEXT("TArray<")) || Base.StartsWith(TEXT("TMap<")) || Base.StartsWith(TEXT("TSet<")))
		{
			OutError = TEXT("Nested containers, maps and sets are not supported as Control Rig variables.");
			return false;
		}

		static const TMap<FString, FString> Aliases = {
			{ TEXT("bool"), TEXT("bool") }, { TEXT("boolean"), TEXT("bool") },
			{ TEXT("float"), TEXT("float") },
			{ TEXT("double"), TEXT("double") },
			{ TEXT("int"), TEXT("int32") }, { TEXT("int32"), TEXT("int32") }, { TEXT("integer"), TEXT("int32") },
			{ TEXT("string"), TEXT("FString") }, { TEXT("fstring"), TEXT("FString") },
			{ TEXT("name"), TEXT("FName") }, { TEXT("fname"), TEXT("FName") },
			{ TEXT("text"), TEXT("FText") }, { TEXT("ftext"), TEXT("FText") },
			{ TEXT("vector"), TEXT("/Script/CoreUObject.Vector") }, { TEXT("fvector"), TEXT("/Script/CoreUObject.Vector") },
			{ TEXT("vector2d"), TEXT("/Script/CoreUObject.Vector2D") }, { TEXT("fvector2d"), TEXT("/Script/CoreUObject.Vector2D") },
			{ TEXT("rotator"), TEXT("/Script/CoreUObject.Rotator") }, { TEXT("frotator"), TEXT("/Script/CoreUObject.Rotator") },
			{ TEXT("quat"), TEXT("/Script/CoreUObject.Quat") }, { TEXT("fquat"), TEXT("/Script/CoreUObject.Quat") },
			{ TEXT("transform"), TEXT("/Script/CoreUObject.Transform") }, { TEXT("ftransform"), TEXT("/Script/CoreUObject.Transform") },
			{ TEXT("linearcolor"), TEXT("/Script/CoreUObject.LinearColor") }, { TEXT("flinearcolor"), TEXT("/Script/CoreUObject.LinearColor") },
			{ TEXT("rigelementkey"), TEXT("/Script/ControlRig.RigElementKey") }, { TEXT("frigelementkey"), TEXT("/Script/ControlRig.RigElementKey") },
		};

		FString Resolved;
		if (const FString* Found = Aliases.Find(Base.ToLower()))
		{
			Resolved = *Found;
		}
		else if (Base.StartsWith(TEXT("/")))
		{
			Resolved = Base;
		}
		else
		{
			OutError = FString::Printf(TEXT("Unsupported variable type '%s'. Use bool, float, double, int32, FString, FName, FText, vector, vector2d, rotator, quat, transform, linearcolor, rigelementkey, or a struct/enum object path such as /Script/CoreUObject.Vector."), *InType);
			return false;
		}

		if (Resolved.StartsWith(TEXT("/")))
		{
			UObject* TypeObject = FSoftObjectPath(Resolved).TryLoad();
			if (!TypeObject || !(TypeObject->IsA<UScriptStruct>() || TypeObject->IsA<UEnum>()))
			{
				OutError = FString::Printf(TEXT("Type path '%s' does not resolve to a struct or enum."), *Resolved);
				return false;
			}
			Resolved = TypeObject->GetPathName();
		}

		OutCPPTypePath = bArray ? FString::Printf(TEXT("TArray<%s>"), *Resolved) : Resolved;
		return true;
	}
}

// ---------------------------------------------------------------------------
// remove_control_rig_node
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithControlRigWriteActions::HandleRemoveControlRigNode(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString NodeName = Params->GetStringField(TEXT("node_name"));
	if (NodeName.IsEmpty())
		return FMonolithActionResult::Error(TEXT("node_name is required"));

	FString Error;
	UControlRigBlueprint* CRB = nullptr;
	URigVMGraph* Graph = nullptr;
	URigVMController* Controller = nullptr;
	if (!CRWrite_ResolveGraphAndController(Params, CRB, Graph, Controller, Error))
		return FMonolithActionResult::Error(Error);

	URigVMNode* Node = Graph->FindNodeByName(FName(*NodeName));
	if (!Node)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Node '%s' not found in graph '%s'. List nodes with get_control_rig_graph."),
			*NodeName, *Graph->GetName()));
	}

	int32 LinkCount = 0;
	for (URigVMLink* Link : Graph->GetLinks())
	{
		if (Link && (Link->GetSourceNode() == Node || Link->GetTargetNode() == Node)) ++LinkCount;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("Remove Control Rig Node")));
	static_cast<UBlueprint*>(CRB)->Modify();

	if (!Controller->RemoveNodeByName(FName(*NodeName), /*bSetupUndoRedo=*/true, /*bPrintPythonCommand=*/false))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("RemoveNodeByName failed for '%s' (node may be locked or non-removable)."), *NodeName));
	}

	CRB->RequestRigVMInit();
	CRB->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("graph_name"), Graph->GetName());
	Root->SetStringField(TEXT("removed_node"), NodeName);
	Root->SetNumberField(TEXT("removed_link_count"), LinkCount);
	Root->SetNumberField(TEXT("node_count"), Graph->GetNodes().Num());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// break_control_rig_link
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithControlRigWriteActions::HandleBreakControlRigLink(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SourcePin, TargetPin, SinglePin;
	Params->TryGetStringField(TEXT("source_pin"), SourcePin);
	Params->TryGetStringField(TEXT("target_pin"), TargetPin);
	Params->TryGetStringField(TEXT("pin"), SinglePin);

	const bool bPairMode = !SourcePin.IsEmpty() && !TargetPin.IsEmpty();
	if (!bPairMode && SinglePin.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Provide source_pin + target_pin to break one link, or pin to break every link on that pin."));

	FString Error;
	UControlRigBlueprint* CRB = nullptr;
	URigVMGraph* Graph = nullptr;
	URigVMController* Controller = nullptr;
	if (!CRWrite_ResolveGraphAndController(Params, CRB, Graph, Controller, Error))
		return FMonolithActionResult::Error(Error);

	TArray<TSharedPtr<FJsonValue>> Broken;

	if (bPairMode)
	{
		URigVMPin* Src = Graph->FindPin(SourcePin);
		URigVMPin* Dst = Graph->FindPin(TargetPin);
		if (!Src || !Dst)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Pin not found: %s"), !Src ? *SourcePin : *TargetPin));
		if (!Src->IsLinkedTo(Dst))
			return FMonolithActionResult::Error(FString::Printf(TEXT("No link exists between %s and %s."), *SourcePin, *TargetPin));

		const FScopedTransaction Transaction(FText::FromString(TEXT("Break Control Rig Link")));
		static_cast<UBlueprint*>(CRB)->Modify();
		if (!Controller->BreakLink(SourcePin, TargetPin, /*bSetupUndoRedo=*/true, /*bPrintPythonCommand=*/false))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("BreakLink failed: %s -> %s (source must be the output side)."), *SourcePin, *TargetPin));
		}
		TSharedPtr<FJsonObject> L = MakeShared<FJsonObject>();
		L->SetStringField(TEXT("source"), SourcePin);
		L->SetStringField(TEXT("target"), TargetPin);
		Broken.Add(MakeShared<FJsonValueObject>(L));
	}
	else
	{
		URigVMPin* Pin = Graph->FindPin(SinglePin);
		if (!Pin)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Pin not found: %s"), *SinglePin));

		const TArray<URigVMPin*> Sources = Pin->GetLinkedSourcePins();
		const TArray<URigVMPin*> Targets = Pin->GetLinkedTargetPins();
		if (Sources.Num() == 0 && Targets.Num() == 0)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Pin %s has no links."), *SinglePin));

		// Record paths before the links (and possibly the pin objects' link state) change.
		for (URigVMPin* S : Sources)
		{
			TSharedPtr<FJsonObject> L = MakeShared<FJsonObject>();
			L->SetStringField(TEXT("source"), S ? S->GetPinPath() : FString());
			L->SetStringField(TEXT("target"), SinglePin);
			Broken.Add(MakeShared<FJsonValueObject>(L));
		}
		for (URigVMPin* T : Targets)
		{
			TSharedPtr<FJsonObject> L = MakeShared<FJsonObject>();
			L->SetStringField(TEXT("source"), SinglePin);
			L->SetStringField(TEXT("target"), T ? T->GetPinPath() : FString());
			Broken.Add(MakeShared<FJsonValueObject>(L));
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("Break Control Rig Pin Links")));
		static_cast<UBlueprint*>(CRB)->Modify();

		// Links arriving at the pin (pin acts as input), then links leaving it (pin acts as output).
		if (Sources.Num() > 0)
		{
			Controller->BreakAllLinks(SinglePin, /*bAsInput=*/true, /*bSetupUndoRedo=*/true, /*bPrintPythonCommand=*/false);
		}
		if (Targets.Num() > 0)
		{
			Controller->BreakAllLinks(SinglePin, /*bAsInput=*/false, /*bSetupUndoRedo=*/true, /*bPrintPythonCommand=*/false);
		}
	}

	CRB->RequestRigVMInit();
	CRB->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("graph_name"), Graph->GetName());
	Root->SetArrayField(TEXT("broken_links"), Broken);
	Root->SetNumberField(TEXT("link_count"), Graph->GetLinks().Num());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// set_control_rig_pin_default
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithControlRigWriteActions::HandleSetControlRigPinDefault(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString PinPath = Params->GetStringField(TEXT("pin_path"));
	if (PinPath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("pin_path is required"));
	if (!Params->HasField(TEXT("value")))
		return FMonolithActionResult::Error(TEXT("value is required (string, number or boolean)"));
	const FString Value = CRWrite_JsonScalarToString(Params->TryGetField(TEXT("value")));

	bool bResizeArrays = true;
	Params->TryGetBoolField(TEXT("resize_arrays"), bResizeArrays);

	FString Error;
	UControlRigBlueprint* CRB = nullptr;
	URigVMGraph* Graph = nullptr;
	URigVMController* Controller = nullptr;
	if (!CRWrite_ResolveGraphAndController(Params, CRB, Graph, Controller, Error))
		return FMonolithActionResult::Error(Error);

	URigVMPin* Pin = Graph->FindPin(PinPath);
	if (!Pin)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Pin not found: %s. Pin paths are NodeName.PinName (see get_control_rig_graph pin_path)."), *PinPath));

	const FString OldValue = Pin->GetDefaultValue();
	const FString PinType = Pin->GetCPPType();

	const FScopedTransaction Transaction(FText::FromString(TEXT("Set Control Rig Pin Default")));
	static_cast<UBlueprint*>(CRB)->Modify();

	if (!Controller->SetPinDefaultValue(PinPath, Value, bResizeArrays, /*bSetupUndoRedo=*/true, /*bMergeUndoAction=*/false, /*bPrintPythonCommand=*/false))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("SetPinDefaultValue rejected '%s' for pin %s (type %s). Check the text-import format for that type."),
			*Value, *PinPath, *PinType));
	}

	CRB->RequestRigVMInit();
	CRB->MarkPackageDirty();

	// Re-resolve: resizing an array pin can rebuild its sub-pins.
	URigVMPin* After = Graph->FindPin(PinPath);
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("pin_path"), PinPath);
	Root->SetStringField(TEXT("type"), PinType);
	Root->SetStringField(TEXT("old_value"), OldValue);
	Root->SetStringField(TEXT("new_value"), After ? After->GetDefaultValue() : Value);
	Root->SetBoolField(TEXT("pin_is_linked"), After ? After->GetLinkedSourcePins().Num() > 0 : false);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// add_control_rig_variable
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithControlRigWriteActions::HandleAddControlRigVariable(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString VarName = Params->GetStringField(TEXT("name")).TrimStartAndEnd();
	const FString TypeStr = Params->GetStringField(TEXT("type"));
	if (VarName.IsEmpty() || TypeStr.IsEmpty())
		return FMonolithActionResult::Error(TEXT("name and type are required"));

	FString DefaultValue;
	if (Params->HasField(TEXT("default")))
	{
		DefaultValue = CRWrite_JsonScalarToString(Params->TryGetField(TEXT("default")));
	}

	bool bIsPublic = false;
	bool bIsReadOnly = false;
	Params->TryGetBoolField(TEXT("is_public"), bIsPublic);
	Params->TryGetBoolField(TEXT("is_read_only"), bIsReadOnly);

	FString CPPTypePath;
	FString Error;
	if (!CRWrite_ResolveVariableType(TypeStr, CPPTypePath, Error))
		return FMonolithActionResult::Error(Error);

	UControlRigBlueprint* CRB = LoadCRBlueprint(AssetPath, Error);
	if (!CRB) return FMonolithActionResult::Error(Error);

	const FScopedTransaction Transaction(FText::FromString(TEXT("Add Control Rig Variable")));
	static_cast<UBlueprint*>(CRB)->Modify();

	// Same virtual signature on IRigVMAssetInterface (5.7) and IRigVMEditorAssetInterface (5.8, aliased).
	const FName Added = static_cast<IRigVMAssetInterface*>(CRB)->AddMemberVariable(
		FName(*VarName), CPPTypePath, bIsPublic, bIsReadOnly, DefaultValue);
	if (Added.IsNone())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("AddMemberVariable failed for '%s' of type %s."), *VarName, *CPPTypePath));
	}

	CRB->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("name"), Added.ToString());
	Root->SetBoolField(TEXT("renamed"), Added.ToString() != VarName);
	Root->SetStringField(TEXT("cpp_type"), CPPTypePath);
	Root->SetStringField(TEXT("default"), DefaultValue);
	Root->SetBoolField(TEXT("is_public"), bIsPublic);
	Root->SetBoolField(TEXT("is_read_only"), bIsReadOnly);
	return FMonolithActionResult::Success(Root);
}
