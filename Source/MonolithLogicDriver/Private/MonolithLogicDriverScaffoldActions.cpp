#include "MonolithLogicDriverScaffoldActions.h"
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
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Factories/Factory.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MonolithAssetUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithLDScaffold, Log, All);

namespace
{
	bool SaveScaffoldAsset(UObject* Asset)
	{
		if (!Asset) return false;
		UPackage* Package = Asset->GetPackage();
		return FMonolithAssetUtils::SaveAssetPackage(Package);
	}

	struct FScaffoldStateDesc
	{
		FString Name;
		int32 PosX;
		int32 PosY;
		bool bIsEndState = false;
	};

	struct FScaffoldTransitionDesc
	{
		int32 SourceIndex;
		int32 TargetIndex;
	};

	UEdGraphNode* CreateNodeOfClass(UEdGraph* Graph, UClass* NodeClass, int32 PosX, int32 PosY)
	{
		if (!Graph || !NodeClass) return nullptr;
		UEdGraphNode* NewNode = NewObject<UEdGraphNode>(Graph, NodeClass);
		NewNode->CreateNewGuid();
		NewNode->NodePosX = PosX;
		NewNode->NodePosY = PosY;
		Graph->AddNode(NewNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		NewNode->PostPlacedNewNode();
		NewNode->AllocateDefaultPins();
		return NewNode;
	}

	bool ConnectNodes(UEdGraphNode* Source, UEdGraphNode* Target)
	{
		if (!Source || !Target) return false;

		UEdGraphPin* OutputPin = nullptr;
		UEdGraphPin* InputPin = nullptr;

		for (UEdGraphPin* Pin : Source->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output)
			{
				OutputPin = Pin;
				break;
			}
		}
		for (UEdGraphPin* Pin : Target->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input)
			{
				InputPin = Pin;
				break;
			}
		}

		if (OutputPin && InputPin)
		{
			const UEdGraphSchema* Schema = Source->GetGraph()->GetSchema();
			if (Schema)
			{
				return Schema->TryCreateConnection(OutputPin, InputPin);
			}
		}
		return false;
	}
}

void FMonolithLogicDriverScaffoldActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("logicdriver"), TEXT("scaffold_hello_world_sm"),
		TEXT("Create a ready-to-use SM Blueprint with 3 states (Idle->Active->Complete) and transitions — a quick-start template"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldHelloWorldSM),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path for the new SM (e.g. /Game/StateMachines/SM_HelloWorld)"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Asset name (default: SM_HelloWorld)"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("scaffold_weapon_sm"),
		TEXT("Create an FPS weapon state machine: Idle->Drawing->Ready->Firing->Cooldown->Reloading with transitions"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldWeaponSM),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path for the new SM (e.g. /Game/StateMachines/SM_Weapon)"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Asset name (default: SM_Weapon)"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("scaffold_horror_encounter_sm"),
		TEXT("Create a horror encounter state machine: Dormant->Lurking->Stalking->Chasing->Attacking->Retreating->Despawned"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldHorrorEncounterSM),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path for the new SM (e.g. /Game/StateMachines/SM_HorrorEncounter)"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Asset name (default: SM_HorrorEncounter)"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("scaffold_game_flow_sm"),
		TEXT("Create a game flow state machine: MainMenu->Loading->Gameplay->Pause->Results->Credits with loops"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldGameFlowSM),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path for the new SM (e.g. /Game/StateMachines/SM_GameFlow)"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Asset name (default: SM_GameFlow)"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("scaffold_dialogue_sm"),
		TEXT("Create a dialogue state machine with speaker/text states wired in sequence, with optional branching choices"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldDialogueSM),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path for the new SM"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Asset name"))
			.Optional(TEXT("dialogue_nodes"), TEXT("array"), TEXT("Array of {speaker, text, choices?:[string]} — each becomes a state"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("scaffold_quest_sm"),
		TEXT("Create a quest state machine: Inactive -> Active -> [objectives] -> Complete/Failed"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldQuestSM),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path for the new SM"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Asset name"))
			.Optional(TEXT("objectives"), TEXT("array"), TEXT("Array of objective name strings (each becomes a state)"))
			.Build());

	Registry.RegisterAction(TEXT("logicdriver"), TEXT("scaffold_interactable_sm"),
		TEXT("Create an interactable state machine with custom states (default: locked/unlocked/open/closed)"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldInteractableSM),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path for the new SM"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Asset name"))
			.Optional(TEXT("states"), TEXT("array"), TEXT("Array of state name strings (default: ['locked','unlocked','open','closed'])"))
			.Build());

	UE_LOG(LogMonolithLDScaffold, Log, TEXT("MonolithLogicDriver Scaffold: registered 7 actions"));
}

FMonolithActionResult FMonolithLogicDriverScaffoldActions::HandleScaffoldHelloWorldSM(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'save_path'"));

	FString AssetName = TEXT("SM_HelloWorld");
	if (Params->HasField(TEXT("name")) && !Params->GetStringField(TEXT("name")).IsEmpty())
	{
		AssetName = Params->GetStringField(TEXT("name"));
	}

	// ── 1. Create SM Blueprint via factory ──
	UClass* FactoryClass = MonolithLD::GetSMBlueprintFactoryClass();
	if (!FactoryClass)
	{
		return FMonolithActionResult::Error(TEXT("SMBlueprintFactory not found. Is Logic Driver Pro loaded?"));
	}

	UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
	if (!Factory)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create SMBlueprintFactory instance"));
	}

	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at: %s"), *SavePath));
	}
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

	if (!NewAsset)
	{
		return FMonolithActionResult::Error(TEXT("FactoryCreateNew returned null"));
	}

	UBlueprint* SMBlueprint = Cast<UBlueprint>(NewAsset);
	if (!SMBlueprint)
	{
		return FMonolithActionResult::Error(TEXT("Created asset is not a Blueprint"));
	}

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewAsset);

	// ── 2. Get root graph and add states ──
	UEdGraph* RootGraph = MonolithLD::GetRootGraph(SMBlueprint);
	if (!RootGraph)
	{
		return FMonolithActionResult::Error(TEXT("No root SM graph found in new Blueprint"));
	}

	UClass* StateClass = MonolithLD::GetSMGraphNodeStateClass();
	if (!StateClass)
	{
		return FMonolithActionResult::Error(TEXT("SMGraphNode_StateNode class not found"));
	}

	// Find the entry node (already exists in a new SM)
	UEdGraphNode* EntryNode = nullptr;
	for (UEdGraphNode* Node : RootGraph->Nodes)
	{
		if (Node && MonolithLD::GetNodeType(Node) == TEXT("entry"))
		{
			EntryNode = Node;
			break;
		}
	}

	// Create 3 states
	UEdGraphNode* IdleNode = CreateNodeOfClass(RootGraph, StateClass, 300, 0);
	UEdGraphNode* ActiveNode = CreateNodeOfClass(RootGraph, StateClass, 600, 0);
	UEdGraphNode* CompleteNode = CreateNodeOfClass(RootGraph, StateClass, 900, 0);

	if (!IdleNode || !ActiveNode || !CompleteNode)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create one or more state nodes"));
	}

	// Set Complete as end state
	FBoolProperty* EndStateProp = CastField<FBoolProperty>(CompleteNode->GetClass()->FindPropertyByName(TEXT("bIsEndState")));
	if (EndStateProp)
	{
		EndStateProp->SetPropertyValue(EndStateProp->ContainerPtrToValuePtr<void>(CompleteNode), true);
	}

	// ── 3. Wire transitions ──
	// Entry -> Idle (connect entry to first state = initial state)
	if (EntryNode)
	{
		ConnectNodes(EntryNode, IdleNode);
	}

	// For SM graphs, transitions are typically created automatically when connecting states
	// Connect Idle -> Active and Active -> Complete
	ConnectNodes(IdleNode, ActiveNode);
	ConnectNodes(ActiveNode, CompleteNode);

	// ── 4. Compile ──
	FString CompileError;
	bool bCompiled = MonolithLD::CompileSMBlueprint(SMBlueprint, CompileError);

	// Set state names AFTER compile (NodeInstanceTemplate created during compilation)
	MonolithLD::SetNodeName(IdleNode, TEXT("Idle"));
	MonolithLD::SetNodeName(ActiveNode, TEXT("Active"));
	MonolithLD::SetNodeName(CompleteNode, TEXT("Complete"));

	// Save (do NOT recompile — recompilation reconstructs templates and loses name changes)
	bool bSaved = SaveScaffoldAsset(NewAsset);

	// ── 5. Build result ──
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetStringField(TEXT("asset_name"), AssetName);
	Result->SetBoolField(TEXT("compiled"), bCompiled);
	if (!bCompiled)
	{
		Result->SetStringField(TEXT("compile_error"), CompileError);
	}
	Result->SetBoolField(TEXT("saved"), bSaved);

	// Node GUIDs for reference
	TSharedPtr<FJsonObject> Nodes = MakeShared<FJsonObject>();
	if (IdleNode) Nodes->SetStringField(TEXT("idle"), IdleNode->NodeGuid.ToString());
	if (ActiveNode) Nodes->SetStringField(TEXT("active"), ActiveNode->NodeGuid.ToString());
	if (CompleteNode) Nodes->SetStringField(TEXT("complete"), CompleteNode->NodeGuid.ToString());
	Result->SetObjectField(TEXT("nodes"), Nodes);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Scaffolded SM '%s' with states: Idle -> Active -> Complete"), *AssetName));

	return FMonolithActionResult::Success(Result);
}

// ── Shared scaffold helper ──

static FMonolithActionResult ScaffoldGeneric(
	const TSharedPtr<FJsonObject>& Params,
	const FString& DefaultName,
	const TArray<FScaffoldStateDesc>& StateDescs,
	const TArray<FScaffoldTransitionDesc>& TransDescs,
	int32 InitialStateIndex)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'save_path'"));

	FString AssetName = DefaultName;
	if (Params->HasField(TEXT("name")) && !Params->GetStringField(TEXT("name")).IsEmpty())
	{
		AssetName = Params->GetStringField(TEXT("name"));
	}

	// 1. Create SM Blueprint via factory
	UClass* FactoryClass = MonolithLD::GetSMBlueprintFactoryClass();
	if (!FactoryClass) return FMonolithActionResult::Error(TEXT("SMBlueprintFactory not found. Is Logic Driver Pro loaded?"));

	UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
	if (!Factory) return FMonolithActionResult::Error(TEXT("Failed to create SMBlueprintFactory instance"));

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

	// 2. Get root graph
	UEdGraph* RootGraph = MonolithLD::GetRootGraph(SMBlueprint);
	if (!RootGraph) return FMonolithActionResult::Error(TEXT("No root SM graph found in new Blueprint"));

	UClass* StateClass = MonolithLD::GetSMGraphNodeStateClass();
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

	// 3. Create state nodes
	TArray<UEdGraphNode*> StateNodes;
	TSharedPtr<FJsonObject> NodeGuids = MakeShared<FJsonObject>();

	for (const FScaffoldStateDesc& Desc : StateDescs)
	{
		UEdGraphNode* Node = CreateNodeOfClass(RootGraph, StateClass, Desc.PosX, Desc.PosY);
		if (!Node) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create state: %s"), *Desc.Name));

		if (Desc.bIsEndState)
		{
			FBoolProperty* EndStateProp = CastField<FBoolProperty>(Node->GetClass()->FindPropertyByName(TEXT("bIsEndState")));
			if (EndStateProp)
			{
				EndStateProp->SetPropertyValue(EndStateProp->ContainerPtrToValuePtr<void>(Node), true);
			}
		}

		StateNodes.Add(Node);
		NodeGuids->SetStringField(Desc.Name.ToLower().Replace(TEXT(" "), TEXT("_")), Node->NodeGuid.ToString());
	}

	// 4. Connect entry to initial state
	if (EntryNode && StateNodes.IsValidIndex(InitialStateIndex))
	{
		ConnectNodes(EntryNode, StateNodes[InitialStateIndex]);
	}

	// 5. Wire transitions
	for (const FScaffoldTransitionDesc& Trans : TransDescs)
	{
		if (StateNodes.IsValidIndex(Trans.SourceIndex) && StateNodes.IsValidIndex(Trans.TargetIndex))
		{
			ConnectNodes(StateNodes[Trans.SourceIndex], StateNodes[Trans.TargetIndex]);
		}
	}

	// 6. Compile
	FString CompileError;
	bool bCompiled = MonolithLD::CompileSMBlueprint(SMBlueprint, CompileError);

	// Set state names AFTER compile (NodeInstanceTemplate created during compilation)
	for (int32 i = 0; i < StateDescs.Num() && i < StateNodes.Num(); ++i)
	{
		MonolithLD::SetNodeName(StateNodes[i], StateDescs[i].Name);
	}

	// Save (do NOT recompile — recompilation reconstructs templates and loses name changes)
	bool bSaved = SaveScaffoldAsset(NewAsset);

	// 7. Build result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetStringField(TEXT("asset_name"), AssetName);
	Result->SetBoolField(TEXT("compiled"), bCompiled);
	if (!bCompiled) Result->SetStringField(TEXT("compile_error"), CompileError);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetObjectField(TEXT("nodes"), NodeGuids);
	Result->SetNumberField(TEXT("state_count"), StateDescs.Num());
	Result->SetNumberField(TEXT("transition_count"), TransDescs.Num());

	// Build state list string
	FString StateList;
	for (int32 i = 0; i < StateDescs.Num(); ++i)
	{
		if (i > 0) StateList += TEXT(", ");
		StateList += StateDescs[i].Name;
	}
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Scaffolded SM '%s' with %d states: %s"), *AssetName, StateDescs.Num(), *StateList));

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLogicDriverScaffoldActions::HandleScaffoldDialogueSM(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'save_path'"));

	FString AssetName;
	if (Params->HasField(TEXT("name")) && !Params->GetStringField(TEXT("name")).IsEmpty())
	{
		AssetName = Params->GetStringField(TEXT("name"));
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'name'"));
	}

	// Build states and transitions from dialogue_nodes
	TArray<FScaffoldStateDesc> States;
	TArray<FScaffoldTransitionDesc> Transitions;

	if (Params->HasField(TEXT("dialogue_nodes")))
	{
		const TArray<TSharedPtr<FJsonValue>>& DialogueNodes = Params->GetArrayField(TEXT("dialogue_nodes"));
		int32 PosX = 300;

		for (int32 i = 0; i < DialogueNodes.Num(); ++i)
		{
			const TSharedPtr<FJsonObject>& DN = DialogueNodes[i]->AsObject();
			if (!DN.IsValid()) continue;

			FString Speaker = DN->HasField(TEXT("speaker")) ? DN->GetStringField(TEXT("speaker")) : TEXT("Speaker");
			FString Text = DN->HasField(TEXT("text")) ? DN->GetStringField(TEXT("text")) : TEXT("");

			FString StateName = FString::Printf(TEXT("%s_%d"), *Speaker, i);
			if (!Text.IsEmpty())
			{
				// Truncate text for node name if too long
				FString ShortText = Text.Left(30);
				StateName = FString::Printf(TEXT("%s: %s"), *Speaker, *ShortText);
			}

			bool bIsEnd = (i == DialogueNodes.Num() - 1); // Last node is end state (unless it has choices)

			// Check for choices
			if (DN->HasField(TEXT("choices")))
			{
				const TArray<TSharedPtr<FJsonValue>>& Choices = DN->GetArrayField(TEXT("choices"));
				if (Choices.Num() > 0) bIsEnd = false;

				// Add the dialogue state
				int32 DialogueIdx = States.Num();
				States.Add({ StateName, PosX, 0, false });
				PosX += 300;

				// Add choice states branching from this dialogue node
				int32 ChoiceY = -150;
				for (int32 c = 0; c < Choices.Num(); ++c)
				{
					FString ChoiceName = Choices[c]->AsString();
					int32 ChoiceIdx = States.Num();
					States.Add({ ChoiceName, PosX, ChoiceY, false });
					Transitions.Add({ DialogueIdx, ChoiceIdx });
					ChoiceY += 150;
				}
				PosX += 300;
			}
			else
			{
				int32 StateIdx = States.Num();
				States.Add({ StateName, PosX, 0, bIsEnd });
				PosX += 300;

				// Wire linear: prev -> current
				if (StateIdx > 0)
				{
					Transitions.Add({ StateIdx - 1, StateIdx });
				}
			}
		}
	}
	else
	{
		// Default dialogue scaffold
		States = {
			{ TEXT("Greeting"),    300,  0 },
			{ TEXT("Question"),    600,  0 },
			{ TEXT("Response_A"),  900, -100 },
			{ TEXT("Response_B"),  900,  100 },
			{ TEXT("Farewell"),   1200,  0, true }
		};
		Transitions = {
			{ 0, 1 },  // Greeting -> Question
			{ 1, 2 },  // Question -> Response_A
			{ 1, 3 },  // Question -> Response_B
			{ 2, 4 },  // Response_A -> Farewell
			{ 3, 4 },  // Response_B -> Farewell
		};
	}

	return ScaffoldGeneric(Params, AssetName, States, Transitions, /*InitialStateIndex=*/0);
}

FMonolithActionResult FMonolithLogicDriverScaffoldActions::HandleScaffoldQuestSM(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'save_path'"));

	FString AssetName;
	if (Params->HasField(TEXT("name")) && !Params->GetStringField(TEXT("name")).IsEmpty())
	{
		AssetName = Params->GetStringField(TEXT("name"));
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'name'"));
	}

	// Collect objectives
	TArray<FString> Objectives;
	if (Params->HasField(TEXT("objectives")))
	{
		const TArray<TSharedPtr<FJsonValue>>& ObjArr = Params->GetArrayField(TEXT("objectives"));
		for (const TSharedPtr<FJsonValue>& V : ObjArr)
		{
			FString ObjName = V->AsString();
			if (!ObjName.IsEmpty()) Objectives.Add(ObjName);
		}
	}
	if (Objectives.Num() == 0)
	{
		Objectives = { TEXT("Objective_1"), TEXT("Objective_2"), TEXT("Objective_3") };
	}

	// States: Inactive(0) -> Active(1) -> [Obj_0..N](2..N+1) -> Complete(N+2) / Failed(N+3)
	TArray<FScaffoldStateDesc> States;
	TArray<FScaffoldTransitionDesc> Transitions;

	int32 PosX = 0;
	States.Add({ TEXT("Inactive"), PosX, 0 }); PosX += 300;       // 0
	States.Add({ TEXT("Active"), PosX, 0 }); PosX += 300;         // 1
	Transitions.Add({ 0, 1 }); // Inactive -> Active

	int32 FirstObjIdx = States.Num();
	for (int32 i = 0; i < Objectives.Num(); ++i)
	{
		States.Add({ Objectives[i], PosX, 0 }); PosX += 300;
		if (i == 0)
		{
			Transitions.Add({ 1, FirstObjIdx }); // Active -> first objective
		}
		else
		{
			Transitions.Add({ FirstObjIdx + i - 1, FirstObjIdx + i }); // Chain objectives
		}
	}

	int32 CompleteIdx = States.Num();
	States.Add({ TEXT("Complete"), PosX, -100, true }); // Complete
	int32 FailedIdx = States.Num();
	States.Add({ TEXT("Failed"), PosX, 100, true }); // Failed

	// Last objective -> Complete
	Transitions.Add({ FirstObjIdx + Objectives.Num() - 1, CompleteIdx });
	// Active -> Failed (can fail at any point)
	Transitions.Add({ 1, FailedIdx });

	return ScaffoldGeneric(Params, AssetName, States, Transitions, /*InitialStateIndex=*/0);
}

FMonolithActionResult FMonolithLogicDriverScaffoldActions::HandleScaffoldInteractableSM(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required param 'save_path'"));

	FString AssetName;
	if (Params->HasField(TEXT("name")) && !Params->GetStringField(TEXT("name")).IsEmpty())
	{
		AssetName = Params->GetStringField(TEXT("name"));
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'name'"));
	}

	// Collect state names
	TArray<FString> StateNames;
	if (Params->HasField(TEXT("states")))
	{
		const TArray<TSharedPtr<FJsonValue>>& StatesArr = Params->GetArrayField(TEXT("states"));
		for (const TSharedPtr<FJsonValue>& V : StatesArr)
		{
			FString SN = V->AsString();
			if (!SN.IsEmpty()) StateNames.Add(SN);
		}
	}
	if (StateNames.Num() == 0)
	{
		StateNames = { TEXT("Locked"), TEXT("Unlocked"), TEXT("Open"), TEXT("Closed") };
	}

	// Build states at evenly spaced positions
	TArray<FScaffoldStateDesc> States;
	TArray<FScaffoldTransitionDesc> Transitions;

	int32 PosX = 300;
	for (int32 i = 0; i < StateNames.Num(); ++i)
	{
		States.Add({ StateNames[i], PosX, 0 });
		PosX += 300;

		// Wire basic transitions between consecutive states
		if (i > 0)
		{
			Transitions.Add({ i - 1, i }); // Forward
			Transitions.Add({ i, i - 1 }); // Backward (interactables can toggle)
		}
	}

	return ScaffoldGeneric(Params, AssetName, States, Transitions, /*InitialStateIndex=*/0);
}

FMonolithActionResult FMonolithLogicDriverScaffoldActions::HandleScaffoldWeaponSM(const TSharedPtr<FJsonObject>& Params)
{
	// Idle(0) -> Drawing(1) -> Ready(2) -> Firing(3) -> Cooldown(4) -> Reloading(5)
	TArray<FScaffoldStateDesc> States = {
		{ TEXT("Idle"),      0,   0 },
		{ TEXT("Drawing"),   300, 0 },
		{ TEXT("Ready"),     600, 0 },
		{ TEXT("Firing"),    900, 0 },
		{ TEXT("Cooldown"),  900, 200 },
		{ TEXT("Reloading"), 600, 200 }
	};

	TArray<FScaffoldTransitionDesc> Transitions = {
		{ 0, 1 },  // Idle -> Drawing
		{ 1, 2 },  // Drawing -> Ready
		{ 2, 3 },  // Ready -> Firing
		{ 3, 4 },  // Firing -> Cooldown
		{ 4, 2 },  // Cooldown -> Ready
		{ 4, 5 },  // Cooldown -> Reloading
		{ 5, 0 },  // Reloading -> Idle
	};

	return ScaffoldGeneric(Params, TEXT("SM_Weapon"), States, Transitions, /*InitialStateIndex=*/0);
}

FMonolithActionResult FMonolithLogicDriverScaffoldActions::HandleScaffoldHorrorEncounterSM(const TSharedPtr<FJsonObject>& Params)
{
	// Dormant(0) -> Lurking(1) -> Stalking(2) -> Chasing(3) -> Attacking(4) -> Retreating(5) -> Despawned(6)
	TArray<FScaffoldStateDesc> States = {
		{ TEXT("Dormant"),    0,   0 },
		{ TEXT("Lurking"),    300, 0 },
		{ TEXT("Stalking"),   600, 0 },
		{ TEXT("Chasing"),    900, 0 },
		{ TEXT("Attacking"),  1200, 0 },
		{ TEXT("Retreating"), 1200, 200 },
		{ TEXT("Despawned"),  1500, 200, /*bIsEndState=*/true }
	};

	TArray<FScaffoldTransitionDesc> Transitions = {
		{ 0, 1 },  // Dormant -> Lurking
		{ 1, 2 },  // Lurking -> Stalking
		{ 2, 3 },  // Stalking -> Chasing
		{ 3, 4 },  // Chasing -> Attacking
		{ 4, 5 },  // Attacking -> Retreating
		{ 5, 6 },  // Retreating -> Despawned
		{ 3, 5 },  // Chasing -> Retreating (escape)
		{ 2, 0 },  // Stalking -> Dormant (lose interest)
	};

	return ScaffoldGeneric(Params, TEXT("SM_HorrorEncounter"), States, Transitions, /*InitialStateIndex=*/0);
}

FMonolithActionResult FMonolithLogicDriverScaffoldActions::HandleScaffoldGameFlowSM(const TSharedPtr<FJsonObject>& Params)
{
	// MainMenu(0) -> Loading(1) -> Gameplay(2) -> Pause(3) -> Results(4) -> Credits(5)
	TArray<FScaffoldStateDesc> States = {
		{ TEXT("MainMenu"), 0,   0 },
		{ TEXT("Loading"),  300, 0 },
		{ TEXT("Gameplay"), 600, 0 },
		{ TEXT("Pause"),    600, 200 },
		{ TEXT("Results"),  900, 0 },
		{ TEXT("Credits"),  1200, 0 }
	};

	TArray<FScaffoldTransitionDesc> Transitions = {
		{ 0, 1 },  // MainMenu -> Loading
		{ 1, 2 },  // Loading -> Gameplay
		{ 2, 3 },  // Gameplay -> Pause
		{ 3, 2 },  // Pause -> Gameplay (resume)
		{ 2, 4 },  // Gameplay -> Results
		{ 4, 5 },  // Results -> Credits
		{ 5, 0 },  // Credits -> MainMenu (loop)
	};

	return ScaffoldGeneric(Params, TEXT("SM_GameFlow"), States, Transitions, /*InitialStateIndex=*/0);
}

#else

void FMonolithLogicDriverScaffoldActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// Logic Driver not available
}

#endif // WITH_LOGICDRIVER
