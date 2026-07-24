#include "MonolithControlRigWriteActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "ControlRigBlueprintLegacy.h"
#else
// UE 5.6 hasn't split the legacy Control Rig Blueprint out of ControlRigBlueprint.h yet.
#include "ControlRigBlueprint.h"
#endif
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMClient.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
// UE 5.8 relocated IRigVMAssetInterface (now an alias of IRigVMEditorAssetInterface).
#include "RigVMEditorAsset.h"
#elif ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 7
#include "RigVMAsset.h"
#endif
// UE 5.6 has neither header — UControlRigBlueprint (via URigVMBlueprint) already
// implements GetRigVMClient() directly, no IRigVMAssetInterface indirection needed.
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

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
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	FRigVMClient* Client = static_cast<IRigVMAssetInterface*>(CRB)->GetRigVMClient();
#else
	FRigVMClient* Client = CRB->GetRigVMClient();
#endif
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
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	FRigVMClient* Client = static_cast<IRigVMAssetInterface*>(CRB)->GetRigVMClient();
#else
	FRigVMClient* Client = CRB->GetRigVMClient();
#endif
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
