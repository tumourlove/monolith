#include "MonolithLogicDriverSpecActions.h"
#include "MonolithParamSchema.h"

#if WITH_LOGICDRIVER

#include "MonolithLogicDriverInternal.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Factories/Factory.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "MonolithAssetUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithLDSpec, Log, All);

void FMonolithLogicDriverSpecActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("logicdriver"), TEXT("export_sm_json"),
		TEXT("Export a state machine's full structure as JSON. Optionally write to a file on disk"),
		FMonolithActionHandler::CreateStatic(&HandleExportSMJson),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("SM Blueprint asset path"))
			.OptionalDiskPath(TEXT("output_path"), TEXT("File path to write JSON to (e.g. C:/Temp/sm_export.json). If omitted, returns JSON in response"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("build_sm_from_spec"),
		TEXT("Create a complete state machine from a JSON spec in one call. The crown jewel — states, transitions, conduits, nested SMs, initial/end markers, all wired and compiled"),
		FMonolithActionHandler::CreateStatic(&HandleBuildSMFromSpec),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path for the new SM (e.g. /Game/StateMachines/SM_MyMachine)"))
			.Required(TEXT("spec"), TEXT("object"), TEXT("Spec object: {states: [{name, is_initial?, is_end?}], transitions: [{from, to}], conduits?: [{name}], nested_sms?: [{name, sm_path?}]}"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("export_sm_spec"),
		TEXT("Export a state machine as a spec JSON (same format as build_sm_from_spec input). Inverse of build_sm_from_spec"),
		FMonolithActionHandler::CreateStatic(&HandleExportSMSpec),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("SM Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("import_sm_json"),
		TEXT("Import a state machine from a JSON spec — either a file path or inline JSON string. Parses and delegates to build_sm_from_spec logic"),
		FMonolithActionHandler::CreateStatic(&HandleImportSMJson),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path to create the SM at"))
			.Required(TEXT("json_path_or_data"), TEXT("string"), TEXT("File path (containing / or \\) to read JSON from, or inline JSON string"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("compare_state_machines"),
		TEXT("Compare two state machines structurally: diff states, transitions, and topology by name"),
		FMonolithActionHandler::CreateStatic(&HandleCompareStateMachines),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("path_a"), TEXT("Asset path for the first SM Blueprint"))
			.RequiredAssetPath(TEXT("path_b"), TEXT("Asset path for the second SM Blueprint"))
			.Build());

	UE_LOG(LogMonolithLDSpec, Log, TEXT("MonolithLogicDriver Spec: registered 5 actions"));
}

FMonolithActionResult FMonolithLogicDriverSpecActions::HandleExportSMJson(const TSharedPtr<FJsonObject>& Params)
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

	// Serialize to pretty JSON string
	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(Structure.ToSharedRef(), Writer);

	FString OutputPath;
	if (Params->HasField(TEXT("output_path")))
	{
		OutputPath = Params->GetStringField(TEXT("output_path"));
	}

	if (!OutputPath.IsEmpty())
	{
		// Write to file
		if (!FFileHelper::SaveStringToFile(JsonString, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to write JSON to '%s'"), *OutputPath));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetStringField(TEXT("output_path"), OutputPath);
		Result->SetNumberField(TEXT("json_size_bytes"), JsonString.Len());
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Exported SM structure to '%s'"), *OutputPath));
		return FMonolithActionResult::Success(Result);
	}
	else
	{
		// Return JSON inline
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetObjectField(TEXT("structure"), Structure);
		return FMonolithActionResult::Success(Result);
	}
}

FMonolithActionResult FMonolithLogicDriverSpecActions::HandleImportSMJson(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'save_path'"));

	FString JsonPathOrData = Params->GetStringField(TEXT("json_path_or_data"));
	if (JsonPathOrData.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'json_path_or_data'"));

	FString JsonString;

	// Determine if it's a file path or inline JSON
	if (JsonPathOrData.Contains(TEXT("/")) || JsonPathOrData.Contains(TEXT("\\")))
	{
		// Treat as file path
		if (!FFileHelper::LoadFileToString(JsonString, *JsonPathOrData))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to read file: %s"), *JsonPathOrData));
		}
	}
	else
	{
		JsonString = JsonPathOrData;
	}

	// Parse JSON
	TSharedPtr<FJsonObject> ParsedSpec;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, ParsedSpec) || !ParsedSpec.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Failed to parse JSON spec"));
	}

	// Build a new params object and delegate to build_sm_from_spec
	TSharedPtr<FJsonObject> BuildParams = MakeShared<FJsonObject>();
	BuildParams->SetStringField(TEXT("save_path"), SavePath);
	BuildParams->SetObjectField(TEXT("spec"), ParsedSpec);

	return HandleBuildSMFromSpec(BuildParams);
}

FMonolithActionResult FMonolithLogicDriverSpecActions::HandleBuildSMFromSpec(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'save_path'"));

	const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("spec"), SpecPtr) || !SpecPtr || !(*SpecPtr).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'spec' (object)"));
	}
	TSharedPtr<FJsonObject> Spec = *SpecPtr;

	// ── 1. Create SM Blueprint via factory ──
	UClass* FactoryClass = MonolithLD::GetSMBlueprintFactoryClass();
	if (!FactoryClass)
	{
		return FMonolithActionResult::Error(TEXT("SMBlueprintFactory not found. Is Logic Driver Pro loaded?"));
	}

	UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
	if (!Factory) return FMonolithActionResult::Error(TEXT("Failed to create SMBlueprintFactory instance"));

	// Derive asset name from save path
	FString AssetName = FPackageName::GetShortName(SavePath);

	UPackage* Package = CreatePackage(*SavePath);
	if (!Package) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at: %s"), *SavePath));
	Package->FullyLoad();

	// Guard AFTER FullyLoad (FullyLoad can re-populate package with stale content from disk)
	FString ExistError;
	if (!MonolithLD::EnsureAssetPathFree(Package, SavePath, AssetName, ExistError))
	{
		return FMonolithActionResult::Error(ExistError);
	}

	UClass* SupportedClass = Factory->GetSupportedClass();
	if (!SupportedClass) SupportedClass = MonolithLD::GetSMBlueprintClass();

	UObject* NewAsset = Factory->FactoryCreateNew(
		SupportedClass, Package, FName(*AssetName),
		RF_Public | RF_Standalone, nullptr, GWarn);
	if (!NewAsset) return FMonolithActionResult::Error(TEXT("FactoryCreateNew returned null"));

	UBlueprint* SMBlueprint = Cast<UBlueprint>(NewAsset);
	if (!SMBlueprint) return FMonolithActionResult::Error(TEXT("Created asset is not a Blueprint"));

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewAsset);

	UEdGraph* RootGraph = MonolithLD::GetRootGraph(SMBlueprint);
	if (!RootGraph) return FMonolithActionResult::Error(TEXT("No root SM graph found in new Blueprint"));

	UClass* StateClass = MonolithLD::GetSMGraphNodeStateClass();
	UClass* ConduitClass = MonolithLD::GetSMGraphNodeConduitClass();
	UClass* SMNodeClass = MonolithLD::GetSMGraphNodeSMClass();
	if (!StateClass) return FMonolithActionResult::Error(TEXT("SMGraphNode_StateNode class not found"));

	// Find entry node
	UEdGraphNode* EntryNode = nullptr;
	for (UEdGraphNode* Node : RootGraph->Nodes)
	{
		if (Node && MonolithLD::GetNodeType(Node) == TEXT("entry"))
		{
			EntryNode = Node;
			break;
		}
	}

	// Lambda: create a node (naming happens after compile when NodeInstanceTemplate exists)
	auto CreateNode = [&](UClass* NodeClass, int32 PosX, int32 PosY) -> UEdGraphNode*
	{
		if (!NodeClass) return nullptr;
		UEdGraphNode* NewNode = NewObject<UEdGraphNode>(RootGraph, NodeClass);
		NewNode->CreateNewGuid();
		NewNode->NodePosX = PosX;
		NewNode->NodePosY = PosY;
		RootGraph->AddNode(NewNode, false, false);
		NewNode->PostPlacedNewNode();
		NewNode->AllocateDefaultPins();
		return NewNode;
	};

	// Lambda: connect two nodes via schema
	auto ConnectNodes = [](UEdGraphNode* Source, UEdGraphNode* Target) -> bool
	{
		if (!Source || !Target) return false;
		UEdGraphPin* OutputPin = nullptr;
		UEdGraphPin* InputPin = nullptr;
		for (UEdGraphPin* Pin : Source->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output) { OutputPin = Pin; break; }
		}
		for (UEdGraphPin* Pin : Target->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input) { InputPin = Pin; break; }
		}
		if (OutputPin && InputPin)
		{
			const UEdGraphSchema* Schema = Source->GetGraph()->GetSchema();
			if (Schema) return Schema->TryCreateConnection(OutputPin, InputPin);
		}
		return false;
	};

	// ── 2. Create states ──
	TMap<FString, UEdGraphNode*> NameToNode;
	TSharedPtr<FJsonObject> NodeGuids = MakeShared<FJsonObject>();
	int32 PosX = 300;

	if (Spec->HasField(TEXT("states")))
	{
		const TArray<TSharedPtr<FJsonValue>>& States = Spec->GetArrayField(TEXT("states"));
		for (const TSharedPtr<FJsonValue>& StateVal : States)
		{
			const TSharedPtr<FJsonObject>& StateObj = StateVal->AsObject();
			if (!StateObj.IsValid()) continue;

			FString Name = StateObj->GetStringField(TEXT("name"));
			if (Name.IsEmpty()) continue;

			UEdGraphNode* StateNode = CreateNode(StateClass, PosX, 0);
			PosX += 300;
			if (!StateNode) continue;

			// Initial state: connect from entry
			if (StateObj->HasField(TEXT("is_initial")) && StateObj->GetBoolField(TEXT("is_initial")))
			{
				if (EntryNode) ConnectNodes(EntryNode, StateNode);
			}

			// End state
			if (StateObj->HasField(TEXT("is_end")) && StateObj->GetBoolField(TEXT("is_end")))
			{
				FBoolProperty* EndProp = CastField<FBoolProperty>(StateNode->GetClass()->FindPropertyByName(TEXT("bIsEndState")));
				if (EndProp) EndProp->SetPropertyValue(EndProp->ContainerPtrToValuePtr<void>(StateNode), true);
			}

			NameToNode.Add(Name, StateNode);
			NodeGuids->SetStringField(Name, StateNode->NodeGuid.ToString());
		}
	}

	// ── 3. Create conduits ──
	if (Spec->HasField(TEXT("conduits")) && ConduitClass)
	{
		const TArray<TSharedPtr<FJsonValue>>& Conduits = Spec->GetArrayField(TEXT("conduits"));
		for (const TSharedPtr<FJsonValue>& CV : Conduits)
		{
			const TSharedPtr<FJsonObject>& CO = CV->AsObject();
			if (!CO.IsValid()) continue;
			FString Name = CO->GetStringField(TEXT("name"));
			if (Name.IsEmpty()) continue;

			UEdGraphNode* ConduitNode = CreateNode(ConduitClass, PosX, 150);
			PosX += 300;
			if (!ConduitNode) continue;

			NameToNode.Add(Name, ConduitNode);
			NodeGuids->SetStringField(Name, ConduitNode->NodeGuid.ToString());
		}
	}

	// ── 4. Create nested SMs ──
	if (Spec->HasField(TEXT("nested_sms")) && SMNodeClass)
	{
		const TArray<TSharedPtr<FJsonValue>>& NestedSMs = Spec->GetArrayField(TEXT("nested_sms"));
		for (const TSharedPtr<FJsonValue>& NV : NestedSMs)
		{
			const TSharedPtr<FJsonObject>& NO = NV->AsObject();
			if (!NO.IsValid()) continue;
			FString Name = NO->GetStringField(TEXT("name"));
			if (Name.IsEmpty()) continue;

			UEdGraphNode* SMNode = CreateNode(SMNodeClass, PosX, -150);
			PosX += 300;
			if (!SMNode) continue;

			// If sm_path given, try to set the referenced SM class
			if (NO->HasField(TEXT("sm_path")))
			{
				FString SMPath = NO->GetStringField(TEXT("sm_path"));
				FString LoadError;
				UBlueprint* RefBP = MonolithLD::LoadSMBlueprint(SMPath, LoadError);
				if (RefBP && RefBP->GeneratedClass)
				{
					FClassProperty* ClassProp = CastField<FClassProperty>(
						SMNode->GetClass()->FindPropertyByName(TEXT("StateMachineClass")));
					if (ClassProp)
					{
						ClassProp->SetObjectPropertyValue(
							ClassProp->ContainerPtrToValuePtr<void>(SMNode), RefBP->GeneratedClass);
					}
				}
			}

			NameToNode.Add(Name, SMNode);
			NodeGuids->SetStringField(Name, SMNode->NodeGuid.ToString());
		}
	}

	// ── 5. Wire transitions ──
	int32 TransitionsCreated = 0;
	if (Spec->HasField(TEXT("transitions")))
	{
		const TArray<TSharedPtr<FJsonValue>>& Transitions = Spec->GetArrayField(TEXT("transitions"));
		for (const TSharedPtr<FJsonValue>& TV : Transitions)
		{
			const TSharedPtr<FJsonObject>& TO = TV->AsObject();
			if (!TO.IsValid()) continue;

			FString From = TO->GetStringField(TEXT("from"));
			FString To = TO->GetStringField(TEXT("to"));

			UEdGraphNode** FromNode = NameToNode.Find(From);
			UEdGraphNode** ToNode = NameToNode.Find(To);

			if (FromNode && ToNode && ConnectNodes(*FromNode, *ToNode))
			{
				TransitionsCreated++;
			}
		}
	}

	// ── 6. Compile, set names, recompile, save ──
	FString CompileError;
	bool bCompiled = MonolithLD::CompileSMBlueprint(SMBlueprint, CompileError);

	// Set names AFTER compile (NodeInstanceTemplate created during compilation)
	// Do NOT recompile — recompilation reconstructs templates and loses name changes
	for (auto& Pair : NameToNode)
	{
		MonolithLD::SetNodeName(Pair.Value, Pair.Key);
	}

	bool bSaved = FMonolithAssetUtils::SaveAssetPackage(Package);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetStringField(TEXT("asset_name"), AssetName);
	Result->SetNumberField(TEXT("states_created"), NameToNode.Num());
	Result->SetNumberField(TEXT("transitions_created"), TransitionsCreated);
	Result->SetBoolField(TEXT("compiled"), bCompiled);
	if (!bCompiled) Result->SetStringField(TEXT("compile_error"), CompileError);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetObjectField(TEXT("node_guids"), NodeGuids);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLogicDriverSpecActions::HandleExportSMSpec(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));

	FString LoadError;
	UBlueprint* SMBlueprint = MonolithLD::LoadSMBlueprint(AssetPath, LoadError);
	if (!SMBlueprint) return FMonolithActionResult::Error(LoadError);

	UEdGraph* RootGraph = MonolithLD::GetRootGraph(SMBlueprint);
	if (!RootGraph) return FMonolithActionResult::Error(TEXT("No root SM graph found"));

	// Collect states, conduits, nested SMs, transitions
	TArray<TSharedPtr<FJsonValue>> StatesArr;
	TArray<TSharedPtr<FJsonValue>> ConduitsArr;
	TArray<TSharedPtr<FJsonValue>> NestedSMsArr;
	TArray<TSharedPtr<FJsonValue>> TransitionsArr;

	// Map GUID -> name for transition lookup
	TMap<FString, FString> GuidToName;

	for (UEdGraphNode* RawNode : RootGraph->Nodes)
	{
		if (!RawNode) continue;
		FString NodeType = MonolithLD::GetNodeType(RawNode);
		FString NodeName = RawNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		FString Guid = RawNode->NodeGuid.ToString();

		if (NodeType == TEXT("state"))
		{
			TSharedPtr<FJsonObject> StateObj = MakeShared<FJsonObject>();
			StateObj->SetStringField(TEXT("name"), NodeName);

			// Check initial: is it connected from entry?
			bool bIsInitial = false;
			for (UEdGraphPin* Pin : RawNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input)
				{
					for (UEdGraphPin* LP : Pin->LinkedTo)
					{
						if (LP && LP->GetOwningNode() && MonolithLD::GetNodeType(LP->GetOwningNode()) == TEXT("entry"))
						{
							bIsInitial = true;
						}
					}
				}
			}
			if (bIsInitial) StateObj->SetBoolField(TEXT("is_initial"), true);

			bool bIsEnd = MonolithLD::GetBoolProperty(RawNode, TEXT("bIsEndState"));
			if (bIsEnd) StateObj->SetBoolField(TEXT("is_end"), true);

			StatesArr.Add(MakeShared<FJsonValueObject>(StateObj));
			GuidToName.Add(Guid, NodeName);
		}
		else if (NodeType == TEXT("conduit"))
		{
			TSharedPtr<FJsonObject> CO = MakeShared<FJsonObject>();
			CO->SetStringField(TEXT("name"), NodeName);
			ConduitsArr.Add(MakeShared<FJsonValueObject>(CO));
			GuidToName.Add(Guid, NodeName);
		}
		else if (NodeType == TEXT("state_machine"))
		{
			TSharedPtr<FJsonObject> SMObj = MakeShared<FJsonObject>();
			SMObj->SetStringField(TEXT("name"), NodeName);
			// Try to get StateMachineClass path
			FString SMClassPath;
			FProperty* SMClassProp = RawNode->GetClass()->FindPropertyByName(TEXT("StateMachineClass"));
			if (SMClassProp)
			{
				SMClassProp->ExportTextItem_Direct(SMClassPath, SMClassProp->ContainerPtrToValuePtr<void>(RawNode), nullptr, nullptr, PPF_None);
				if (!SMClassPath.IsEmpty() && SMClassPath != TEXT("None"))
				{
					SMObj->SetStringField(TEXT("sm_path"), SMClassPath);
				}
			}
			NestedSMsArr.Add(MakeShared<FJsonValueObject>(SMObj));
			GuidToName.Add(Guid, NodeName);
		}
		else if (NodeType == TEXT("any_state"))
		{
			// Treat as a state with special flag
			TSharedPtr<FJsonObject> StateObj = MakeShared<FJsonObject>();
			StateObj->SetStringField(TEXT("name"), NodeName);
			StateObj->SetBoolField(TEXT("is_any_state"), true);
			StatesArr.Add(MakeShared<FJsonValueObject>(StateObj));
			GuidToName.Add(Guid, NodeName);
		}
	}

	// Collect transitions
	for (UEdGraphNode* RawNode : RootGraph->Nodes)
	{
		if (!RawNode) continue;
		FString NodeType = MonolithLD::GetNodeType(RawNode);
		if (NodeType != TEXT("transition")) continue;

		FString SourceGuid, TargetGuid;
		for (UEdGraphPin* Pin : RawNode->Pins)
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

		if (!SourceGuid.IsEmpty() && !TargetGuid.IsEmpty())
		{
			FString* FromName = GuidToName.Find(SourceGuid);
			FString* ToName = GuidToName.Find(TargetGuid);
			if (FromName && ToName)
			{
				TSharedPtr<FJsonObject> Trans = MakeShared<FJsonObject>();
				Trans->SetStringField(TEXT("from"), *FromName);
				Trans->SetStringField(TEXT("to"), *ToName);
				TransitionsArr.Add(MakeShared<FJsonValueObject>(Trans));
			}
		}
	}

	TSharedPtr<FJsonObject> SpecObj = MakeShared<FJsonObject>();
	SpecObj->SetArrayField(TEXT("states"), StatesArr);
	SpecObj->SetArrayField(TEXT("transitions"), TransitionsArr);
	SpecObj->SetArrayField(TEXT("conduits"), ConduitsArr);
	SpecObj->SetArrayField(TEXT("nested_sms"), NestedSMsArr);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetObjectField(TEXT("spec"), SpecObj);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLogicDriverSpecActions::HandleCompareStateMachines(const TSharedPtr<FJsonObject>& Params)
{
	FString PathA = Params->GetStringField(TEXT("path_a"));
	FString PathB = Params->GetStringField(TEXT("path_b"));
	if (PathA.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'path_a'"));
	if (PathB.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'path_b'"));

	// Load both SMs
	FString LoadErrorA, LoadErrorB;
	UBlueprint* BPA = MonolithLD::LoadSMBlueprint(PathA, LoadErrorA);
	if (!BPA) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load A: %s"), *LoadErrorA));

	UBlueprint* BPB = MonolithLD::LoadSMBlueprint(PathB, LoadErrorB);
	if (!BPB) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load B: %s"), *LoadErrorB));

	UEdGraph* GraphA = MonolithLD::GetRootGraph(BPA);
	UEdGraph* GraphB = MonolithLD::GetRootGraph(BPB);
	if (!GraphA) return FMonolithActionResult::Error(TEXT("No root graph in SM A"));
	if (!GraphB) return FMonolithActionResult::Error(TEXT("No root graph in SM B"));

	// Collect state names and transitions for each graph
	struct FGraphInfo
	{
		TSet<FString> StateNames;
		TArray<TPair<FString, FString>> Transitions; // from_name -> to_name
		TMap<FString, FString> GuidToName;
	};

	auto CollectInfo = [](UEdGraph* Graph) -> FGraphInfo
	{
		FGraphInfo Info;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			FString NodeType = MonolithLD::GetNodeType(Node);
			if (NodeType == TEXT("state") || NodeType == TEXT("conduit")
				|| NodeType == TEXT("state_machine") || NodeType == TEXT("any_state"))
			{
				FString Name = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				Info.StateNames.Add(Name);
				Info.GuidToName.Add(Node->NodeGuid.ToString(), Name);
			}
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			FString NodeType = MonolithLD::GetNodeType(Node);
			if (NodeType != TEXT("transition")) continue;

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

			FString* FromName = Info.GuidToName.Find(SourceGuid);
			FString* ToName = Info.GuidToName.Find(TargetGuid);
			if (FromName && ToName)
			{
				Info.Transitions.Add(TPair<FString, FString>(*FromName, *ToName));
			}
		}

		return Info;
	};

	FGraphInfo InfoA = CollectInfo(GraphA);
	FGraphInfo InfoB = CollectInfo(GraphB);

	// Diff states
	TArray<TSharedPtr<FJsonValue>> StatesOnlyInA, StatesOnlyInB;
	for (const FString& S : InfoA.StateNames)
	{
		if (!InfoB.StateNames.Contains(S))
		{
			StatesOnlyInA.Add(MakeShared<FJsonValueString>(S));
		}
	}
	for (const FString& S : InfoB.StateNames)
	{
		if (!InfoA.StateNames.Contains(S))
		{
			StatesOnlyInB.Add(MakeShared<FJsonValueString>(S));
		}
	}

	// Diff transitions (compare as "from->to" strings)
	TSet<FString> TransSetA, TransSetB;
	for (const auto& T : InfoA.Transitions)
	{
		TransSetA.Add(FString::Printf(TEXT("%s -> %s"), *T.Key, *T.Value));
	}
	for (const auto& T : InfoB.Transitions)
	{
		TransSetB.Add(FString::Printf(TEXT("%s -> %s"), *T.Key, *T.Value));
	}

	TArray<TSharedPtr<FJsonValue>> TransOnlyInA, TransOnlyInB;
	for (const FString& T : TransSetA)
	{
		if (!TransSetB.Contains(T))
		{
			TransOnlyInA.Add(MakeShared<FJsonValueString>(T));
		}
	}
	for (const FString& T : TransSetB)
	{
		if (!TransSetA.Contains(T))
		{
			TransOnlyInB.Add(MakeShared<FJsonValueString>(T));
		}
	}

	bool bIdentical = StatesOnlyInA.Num() == 0 && StatesOnlyInB.Num() == 0
		&& TransOnlyInA.Num() == 0 && TransOnlyInB.Num() == 0;

	// Summary
	FString Summary;
	if (bIdentical)
	{
		Summary = TEXT("State machines are structurally identical");
	}
	else
	{
		TArray<FString> Parts;
		if (StatesOnlyInA.Num() > 0)
			Parts.Add(FString::Printf(TEXT("A has %d extra state(s)"), StatesOnlyInA.Num()));
		if (StatesOnlyInB.Num() > 0)
			Parts.Add(FString::Printf(TEXT("B has %d extra state(s)"), StatesOnlyInB.Num()));
		if (TransOnlyInA.Num() > 0)
			Parts.Add(FString::Printf(TEXT("A has %d extra transition(s)"), TransOnlyInA.Num()));
		if (TransOnlyInB.Num() > 0)
			Parts.Add(FString::Printf(TEXT("B has %d extra transition(s)"), TransOnlyInB.Num()));
		Summary = FString::Join(Parts, TEXT(", "));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path_a"), PathA);
	Result->SetStringField(TEXT("path_b"), PathB);
	Result->SetArrayField(TEXT("states_only_in_a"), StatesOnlyInA);
	Result->SetArrayField(TEXT("states_only_in_b"), StatesOnlyInB);
	Result->SetArrayField(TEXT("transitions_only_in_a"), TransOnlyInA);
	Result->SetArrayField(TEXT("transitions_only_in_b"), TransOnlyInB);
	Result->SetNumberField(TEXT("state_count_a"), InfoA.StateNames.Num());
	Result->SetNumberField(TEXT("state_count_b"), InfoB.StateNames.Num());
	Result->SetNumberField(TEXT("transition_count_a"), InfoA.Transitions.Num());
	Result->SetNumberField(TEXT("transition_count_b"), InfoB.Transitions.Num());
	Result->SetBoolField(TEXT("identical"), bIdentical);
	Result->SetStringField(TEXT("summary"), Summary);

	return FMonolithActionResult::Success(Result);
}

#else

void FMonolithLogicDriverSpecActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// Logic Driver not available
}

#endif // WITH_LOGICDRIVER
