#include "MonolithComboGraphActions.h"
#include "MonolithComboGraphModule.h"
#include "MonolithParamSchema.h"
// NO ComboGraph includes — all property access via reflection

#if WITH_COMBOGRAPH

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Factories/Factory.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_LatentAbilityCall.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "GameplayTagContainer.h"
#include "Abilities/GameplayAbility.h"

// ============================================================
//  Anonymous helpers
// ============================================================

namespace
{
	// ── Class resolution ─────────────────────────────────────────

	static UClass* FindComboGraphClass()
	{
		return FindFirstObject<UClass>(TEXT("ComboGraph"), EFindFirstObjectOptions::NativeFirst);
	}

	static UClass* FindComboNodeBaseClass()
	{
		return FindFirstObject<UClass>(TEXT("ComboGraphNodeAnimBase"), EFindFirstObjectOptions::NativeFirst);
	}

	static UClass* FindComboNodeMontageClass()
	{
		return FindFirstObject<UClass>(TEXT("ComboGraphNodeMontage"), EFindFirstObjectOptions::NativeFirst);
	}

	static UClass* FindComboNodeSequenceClass()
	{
		return FindFirstObject<UClass>(TEXT("ComboGraphNodeSequence"), EFindFirstObjectOptions::NativeFirst);
	}

	static UClass* FindComboEdgeClass()
	{
		return FindFirstObject<UClass>(TEXT("ComboGraphEdge"), EFindFirstObjectOptions::NativeFirst);
	}

	static UClass* FindComboFactoryClass()
	{
		return FindFirstObject<UClass>(TEXT("ComboGraphFactory"), EFindFirstObjectOptions::NativeFirst);
	}

	static UClass* FindComboAbilityTaskClass()
	{
		return FindFirstObject<UClass>(TEXT("ComboGraphAbilityTask_StartGraph"), EFindFirstObjectOptions::NativeFirst);
	}

	// ── Reflection utilities ─────────────────────────────────────

	template<typename T>
	T* GetPropertyPtr(UObject* Obj, FName PropName)
	{
		if (!Obj) return nullptr;
		FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropName);
		if (!Prop) return nullptr;
		return Prop->ContainerPtrToValuePtr<T>(Obj);
	}

	UObject* GetObjectProperty(UObject* Obj, FName PropName)
	{
		if (!Obj) return nullptr;
		FObjectProperty* Prop = CastField<FObjectProperty>(Obj->GetClass()->FindPropertyByName(PropName));
		if (!Prop) return nullptr;
		return Prop->GetObjectPropertyValue(Prop->ContainerPtrToValuePtr<void>(Obj));
	}

	void SetObjectProperty(UObject* Obj, FName PropName, UObject* Value)
	{
		if (!Obj) return;
		FObjectProperty* Prop = CastField<FObjectProperty>(Obj->GetClass()->FindPropertyByName(PropName));
		if (Prop)
		{
			Prop->SetObjectPropertyValue(Prop->ContainerPtrToValuePtr<void>(Obj), Value);
		}
	}

	float GetFloatProperty(UObject* Obj, FName PropName, float Default = 1.0f)
	{
		if (!Obj) return Default;
		FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropName);
		if (!Prop) return Default;
		if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{
			return FloatProp->GetPropertyValue(FloatProp->ContainerPtrToValuePtr<void>(Obj));
		}
		if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{
			return static_cast<float>(DoubleProp->GetPropertyValue(DoubleProp->ContainerPtrToValuePtr<void>(Obj)));
		}
		return Default;
	}

	void SetFloatProperty(UObject* Obj, FName PropName, float Value)
	{
		if (!Obj) return;
		FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropName);
		if (!Prop) return;
		if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{
			FloatProp->SetPropertyValue(FloatProp->ContainerPtrToValuePtr<void>(Obj), Value);
		}
		else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{
			DoubleProp->SetPropertyValue(DoubleProp->ContainerPtrToValuePtr<void>(Obj), static_cast<double>(Value));
		}
	}

	FString GetStringProperty(UObject* Obj, FName PropName)
	{
		if (!Obj) return FString();
		FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropName);
		if (!Prop) return FString();
		FString Value;
		Prop->ExportText_Direct(Value, Prop->ContainerPtrToValuePtr<void>(Obj), nullptr, Obj, PPF_None);
		return Value;
	}

	// Get the byte/enum value as int
	int32 GetEnumProperty(UObject* Obj, FName PropName, int32 Default = 0)
	{
		if (!Obj) return Default;
		FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropName);
		if (!Prop) return Default;
		if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			return ByteProp->GetPropertyValue(ByteProp->ContainerPtrToValuePtr<void>(Obj));
		}
		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
			if (UnderlyingProp)
			{
				return static_cast<int32>(UnderlyingProp->GetSignedIntPropertyValue(EnumProp->ContainerPtrToValuePtr<void>(Obj)));
			}
		}
		return Default;
	}

	void SetEnumPropertyFromString(UObject* Obj, FName PropName, const FString& ValueStr)
	{
		if (!Obj) return;
		FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropName);
		if (!Prop) return;

		// Try to import the text value
		Prop->ImportText_Direct(*ValueStr, Prop->ContainerPtrToValuePtr<void>(Obj), Obj, PPF_None);
	}

	// ── AllNodes array helper ────────────────────────────────────

	TArray<UObject*> GetAllNodes(UObject* Graph)
	{
		TArray<UObject*> Nodes;
		if (!Graph) return Nodes;

		FArrayProperty* AllNodesProp = CastField<FArrayProperty>(Graph->GetClass()->FindPropertyByName(TEXT("AllNodes")));
		if (!AllNodesProp) return Nodes;

		FScriptArrayHelper ArrayHelper(AllNodesProp, AllNodesProp->ContainerPtrToValuePtr<void>(Graph));
		for (int32 i = 0; i < ArrayHelper.Num(); i++)
		{
			void* ElemPtr = ArrayHelper.GetRawPtr(i);
			FObjectProperty* InnerProp = CastField<FObjectProperty>(AllNodesProp->Inner);
			if (InnerProp)
			{
				UObject* Node = InnerProp->GetObjectPropertyValue(ElemPtr);
				Nodes.Add(Node);
			}
		}
		return Nodes;
	}

	UObject* GetNodeAtIndex(UObject* Graph, int32 Index)
	{
		TArray<UObject*> Nodes = GetAllNodes(Graph);
		if (Nodes.IsValidIndex(Index))
		{
			return Nodes[Index];
		}
		return nullptr;
	}

	int32 GetNodeIndex(UObject* Graph, UObject* Node)
	{
		TArray<UObject*> Nodes = GetAllNodes(Graph);
		return Nodes.IndexOfByKey(Node);
	}

	// Get edges array from a node
	TArray<UObject*> GetEdges(UObject* Node)
	{
		TArray<UObject*> Edges;
		if (!Node) return Edges;

		FArrayProperty* EdgesProp = CastField<FArrayProperty>(Node->GetClass()->FindPropertyByName(TEXT("Edges")));
		if (!EdgesProp) return Edges;

		FScriptArrayHelper ArrayHelper(EdgesProp, EdgesProp->ContainerPtrToValuePtr<void>(Node));
		FObjectProperty* InnerProp = CastField<FObjectProperty>(EdgesProp->Inner);
		if (!InnerProp) return Edges;

		for (int32 i = 0; i < ArrayHelper.Num(); i++)
		{
			UObject* Edge = InnerProp->GetObjectPropertyValue(ArrayHelper.GetRawPtr(i));
			if (Edge) Edges.Add(Edge);
		}
		return Edges;
	}

	// Get children nodes array
	TArray<UObject*> GetChildrenNodes(UObject* Node)
	{
		TArray<UObject*> Children;
		if (!Node) return Children;

		FArrayProperty* ChildProp = CastField<FArrayProperty>(Node->GetClass()->FindPropertyByName(TEXT("ChildrenNodes")));
		if (!ChildProp) return Children;

		FScriptArrayHelper ArrayHelper(ChildProp, ChildProp->ContainerPtrToValuePtr<void>(Node));
		FObjectProperty* InnerProp = CastField<FObjectProperty>(ChildProp->Inner);
		if (!InnerProp) return Children;

		for (int32 i = 0; i < ArrayHelper.Num(); i++)
		{
			UObject* Child = InnerProp->GetObjectPropertyValue(ArrayHelper.GetRawPtr(i));
			if (Child) Children.Add(Child);
		}
		return Children;
	}

	// ── Node info serialization ──────────────────────────────────

	TSharedPtr<FJsonObject> NodeToJson(UObject* Node, int32 Index)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Node) return Obj;

		Obj->SetNumberField(TEXT("index"), Index);
		Obj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		Obj->SetStringField(TEXT("name"), Node->GetName());

		// Animation asset (Montage or AnimSequence via soft ptr — export as string)
		FString MontageStr = GetStringProperty(Node, TEXT("Montage"));
		if (!MontageStr.IsEmpty())
		{
			Obj->SetStringField(TEXT("montage"), MontageStr);
		}

		FString AnimSequenceStr = GetStringProperty(Node, TEXT("AnimationSequence"));
		if (!AnimSequenceStr.IsEmpty())
		{
			Obj->SetStringField(TEXT("animation_sequence"), AnimSequenceStr);
		}

		// Play rate
		float PlayRate = GetFloatProperty(Node, TEXT("MontagePlayRate"), 1.0f);
		Obj->SetNumberField(TEXT("play_rate"), PlayRate);

		// StartSection
		FString StartSection = GetStringProperty(Node, TEXT("StartSection"));
		if (!StartSection.IsEmpty() && StartSection != TEXT("None"))
		{
			Obj->SetStringField(TEXT("start_section"), StartSection);
		}

		// Edges
		TArray<UObject*> NodeEdges = GetEdges(Node);
		TArray<TSharedPtr<FJsonValue>> EdgesArr;
		for (UObject* Edge : NodeEdges)
		{
			if (!Edge) continue;
			TSharedPtr<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
			EdgeObj->SetStringField(TEXT("class"), Edge->GetClass()->GetName());

			UObject* StartNode = Cast<UObject>(GetObjectProperty(Edge, TEXT("StartNode")));
			UObject* EndNode = Cast<UObject>(GetObjectProperty(Edge, TEXT("EndNode")));
			if (StartNode) EdgeObj->SetStringField(TEXT("start_node"), StartNode->GetName());
			if (EndNode) EdgeObj->SetStringField(TEXT("end_node"), EndNode->GetName());

			// Transition properties
			FString TransInput = GetStringProperty(Edge, TEXT("TransitionInput"));
			if (!TransInput.IsEmpty() && TransInput != TEXT("None"))
			{
				EdgeObj->SetStringField(TEXT("transition_input"), TransInput);
			}

			FString TriggerEvent = GetStringProperty(Edge, TEXT("TriggerEvent"));
			if (!TriggerEvent.IsEmpty())
			{
				EdgeObj->SetStringField(TEXT("trigger_event"), TriggerEvent);
			}

			FString TransBehavior = GetStringProperty(Edge, TEXT("TransitionBehavior"));
			if (!TransBehavior.IsEmpty())
			{
				EdgeObj->SetStringField(TEXT("transition_behavior"), TransBehavior);
			}

			EdgesArr.Add(MakeShared<FJsonValueObject>(EdgeObj));
		}
		if (EdgesArr.Num() > 0)
		{
			Obj->SetArrayField(TEXT("edges"), EdgesArr);
		}

		// Children count
		TArray<UObject*> Children = GetChildrenNodes(Node);
		Obj->SetNumberField(TEXT("children_count"), Children.Num());

		return Obj;
	}

	// ── Asset path helpers ───────────────────────────────────────

	FString ExtractAssetName(const FString& Path)
	{
		FString AssetName = FPackageName::GetLongPackageAssetName(Path);
		if (AssetName.IsEmpty())
		{
			int32 LastSlash;
			if (Path.FindLastChar(TEXT('/'), LastSlash))
			{
				AssetName = Path.Mid(LastSlash + 1);
			}
			else
			{
				AssetName = Path;
			}
		}
		return AssetName;
	}

	bool EnsureAssetPathFree(const FString& PackagePath, const FString& AssetName, FString& OutError)
	{
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FString FullPath = PackagePath + TEXT(".") + AssetName;
		FAssetData Existing = AR.GetAssetByObjectPath(FSoftObjectPath(FullPath));
		if (Existing.IsValid())
		{
			OutError = FString::Printf(TEXT("Asset already exists at '%s' (found via AssetRegistry)"), *FullPath);
			return false;
		}
		// Memory check
		UObject* InMemory = FindObject<UObject>(nullptr, *FullPath);
		if (InMemory)
		{
			OutError = FString::Printf(TEXT("Asset already exists in memory at '%s'"), *FullPath);
			return false;
		}
		return true;
	}

	UObject* LoadComboGraph(const FString& AssetPath, FString& OutError)
	{
		UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
		if (!Asset)
		{
			OutError = FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath);
			return nullptr;
		}

		UClass* ComboGraphClass = FindComboGraphClass();
		if (!ComboGraphClass)
		{
			OutError = TEXT("ComboGraph class not found. Is the ComboGraph plugin loaded?");
			return nullptr;
		}

		if (!Asset->IsA(ComboGraphClass))
		{
			OutError = FString::Printf(TEXT("Asset '%s' is not a ComboGraph (is %s)"), *AssetPath, *Asset->GetClass()->GetName());
			return nullptr;
		}

		return Asset;
	}

	void MarkGraphDirty(UObject* Graph)
	{
		if (Graph)
		{
			Graph->Modify();
			Graph->GetPackage()->MarkPackageDirty();
		}
	}

	bool SaveGraphAsset(UObject* Graph)
	{
		if (!Graph) return false;
		UPackage* Package = Graph->GetPackage();
		FString PackageFilename = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Package, Graph, *PackageFilename, SaveArgs);
	}

	// Add a node to AllNodes array via reflection
	bool AddNodeToAllNodes(UObject* Graph, UObject* Node)
	{
		if (!Graph || !Node) return false;

		FArrayProperty* AllNodesProp = CastField<FArrayProperty>(Graph->GetClass()->FindPropertyByName(TEXT("AllNodes")));
		if (!AllNodesProp) return false;

		FScriptArrayHelper ArrayHelper(AllNodesProp, AllNodesProp->ContainerPtrToValuePtr<void>(Graph));
		int32 NewIndex = ArrayHelper.AddValue();
		FObjectProperty* InnerProp = CastField<FObjectProperty>(AllNodesProp->Inner);
		if (InnerProp)
		{
			InnerProp->SetObjectPropertyValue(ArrayHelper.GetRawPtr(NewIndex), Node);
		}
		return true;
	}

	// Set ChildrenNodes / ParentNodes arrays
	void AddToObjectArray(UObject* Owner, FName ArrayPropName, UObject* ItemToAdd)
	{
		if (!Owner || !ItemToAdd) return;

		FArrayProperty* ArrProp = CastField<FArrayProperty>(Owner->GetClass()->FindPropertyByName(ArrayPropName));
		if (!ArrProp) return;

		FScriptArrayHelper ArrayHelper(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(Owner));
		int32 NewIndex = ArrayHelper.AddValue();
		FObjectProperty* InnerProp = CastField<FObjectProperty>(ArrProp->Inner);
		if (InnerProp)
		{
			InnerProp->SetObjectPropertyValue(ArrayHelper.GetRawPtr(NewIndex), ItemToAdd);
		}
	}

	// ── TMap reflection helper for EffectsContainerMap / CuesContainerMap ──

	FString SerializeMapProperty(UObject* Node, FName PropName)
	{
		if (!Node) return TEXT("{}");
		FProperty* Prop = Node->GetClass()->FindPropertyByName(PropName);
		if (!Prop) return TEXT("{}");

		FString Result;
		Prop->ExportText_Direct(Result, Prop->ContainerPtrToValuePtr<void>(Node), nullptr, Node, PPF_None);
		return Result;
	}
}

// ============================================================
//  Registration
// ============================================================

void FMonolithComboGraphActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// ── Read/Inspect ──

	Registry.RegisterAction(TEXT("combograph"), TEXT("list_combo_graphs"),
		TEXT("Find all ComboGraph assets in the project with optional path filter"),
		FMonolithActionHandler::CreateStatic(&HandleListComboGraphs),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Only include assets under this path prefix (e.g. /Game/Combat)"))
			.Build());

	Registry.RegisterAction(TEXT("combograph"), TEXT("get_combo_graph_info"),
		TEXT("Read full structure of a ComboGraph: nodes, edges, transitions, animation refs"),
		FMonolithActionHandler::CreateStatic(&HandleGetComboGraphInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ComboGraph asset path (e.g. /Game/Combat/CG_MeleeCombo)"))
			.Build());

	Registry.RegisterAction(TEXT("combograph"), TEXT("get_combo_node_effects"),
		TEXT("Read GAS effect and cue containers on a specific combo graph node"),
		FMonolithActionHandler::CreateStatic(&HandleGetComboNodeEffects),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ComboGraph asset path"))
			.Required(TEXT("node_index"), TEXT("number"), TEXT("Index of the node in AllNodes array"))
			.Build());

	Registry.RegisterAction(TEXT("combograph"), TEXT("validate_combo_graph"),
		TEXT("Validate a combo graph: check for orphan nodes, missing animations, broken edges"),
		FMonolithActionHandler::CreateStatic(&HandleValidateComboGraph),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ComboGraph asset path"))
			.Build());

	// ── Create/Modify ──

	Registry.RegisterAction(TEXT("combograph"), TEXT("create_combo_graph"),
		TEXT("Create a new ComboGraph asset via UComboGraphFactory"),
		FMonolithActionHandler::CreateStatic(&HandleCreateComboGraph),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path for the new ComboGraph (e.g. /Game/Combat/CG_MeleeCombo)"))
			.Build());

	Registry.RegisterAction(TEXT("combograph"), TEXT("add_combo_node"),
		TEXT("Add a montage or sequence animation node to a combo graph"),
		FMonolithActionHandler::CreateStatic(&HandleAddComboNode),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ComboGraph asset path"))
			.Required(TEXT("animation_asset"), TEXT("string"), TEXT("Path to AnimMontage or AnimSequence asset"))
			.Optional(TEXT("node_type"), TEXT("string"), TEXT("Node type: 'montage' (default) or 'sequence'"), TEXT("montage"))
			.Optional(TEXT("parent_node_index"), TEXT("number"), TEXT("Index of the parent node to connect from (-1 or omit for none)"))
			.Optional(TEXT("play_rate"), TEXT("number"), TEXT("Montage play rate (default 1.0)"), TEXT("1.0"))
			.Build());

	Registry.RegisterAction(TEXT("combograph"), TEXT("add_combo_edge"),
		TEXT("Connect two combo graph nodes with a transition edge"),
		FMonolithActionHandler::CreateStatic(&HandleAddComboEdge),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ComboGraph asset path"))
			.Required(TEXT("from_node_index"), TEXT("number"), TEXT("Index of the source node"))
			.Required(TEXT("to_node_index"), TEXT("number"), TEXT("Index of the target node"))
			.Optional(TEXT("input_action"), TEXT("string"), TEXT("InputAction asset path for transition trigger"))
			.Optional(TEXT("trigger_event"), TEXT("string"), TEXT("ETriggerEvent: Started, Triggered, Canceled"))
			.Optional(TEXT("transition_behavior"), TEXT("string"), TEXT("Transition timing: Immediately, OnAnimNotifyClass, OnAnimNotifyName, OnComboWindowEnd"))
			.Build());

	Registry.RegisterAction(TEXT("combograph"), TEXT("set_combo_node_effects"),
		TEXT("Configure GAS gameplay effect containers on a combo node via EffectsContainerMap"),
		FMonolithActionHandler::CreateStatic(&HandleSetComboNodeEffects),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ComboGraph asset path"))
			.Required(TEXT("node_index"), TEXT("number"), TEXT("Index of the node in AllNodes"))
			.Required(TEXT("effects"), TEXT("object"), TEXT("Map of gameplay tag -> effect container config: {\"Tag.Name\": {\"effect_classes\": [\"/Game/GE\"], \"use_set_by_caller\": true, \"set_by_caller_tag\": \"Tag\", \"set_by_caller_magnitude\": 25.0}}"))
			.Build());

	Registry.RegisterAction(TEXT("combograph"), TEXT("set_combo_node_cues"),
		TEXT("Configure gameplay cue containers on a combo node via CuesContainerMap"),
		FMonolithActionHandler::CreateStatic(&HandleSetComboNodeCues),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ComboGraph asset path"))
			.Required(TEXT("node_index"), TEXT("number"), TEXT("Index of the node in AllNodes"))
			.Required(TEXT("cues"), TEXT("object"), TEXT("Map of gameplay tag -> cue container: {\"GameplayCue.Hit\": {\"definitions\": [{\"gameplay_cue_tags\": [\"Tag\"], \"source_type\": \"Niagara\", \"source_asset\": \"/Game/VFX/NS\"}]}}"))
			.Build());

	// ── Scaffolding ──

	Registry.RegisterAction(TEXT("combograph"), TEXT("create_combo_ability"),
		TEXT("Create a GameplayAbility Blueprint pre-wired with a ComboGraphAbilityTask_StartGraph node"),
		FMonolithActionHandler::CreateStatic(&HandleCreateComboAbility),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path for the new ability BP"))
			.Optional(TEXT("combo_graph"), TEXT("string"), TEXT("ComboGraph asset path to wire as default"))
			.Optional(TEXT("initial_input"), TEXT("string"), TEXT("InputAction path for initial combo trigger"))
			.Optional(TEXT("parent_class"), TEXT("string"), TEXT("Parent ability class (default: GameplayAbility)"), TEXT("GameplayAbility"))
			.Build());

	Registry.RegisterAction(TEXT("combograph"), TEXT("link_ability_to_combo_graph"),
		TEXT("Wire or update a StartComboGraph ability task node on an existing ability Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleLinkAbilityToComboGraph),
		FParamSchemaBuilder()
			.Required(TEXT("ability_path"), TEXT("string"), TEXT("GameplayAbility Blueprint asset path"))
			.Required(TEXT("combo_graph"), TEXT("string"), TEXT("ComboGraph asset path to link"))
			.Build());

	Registry.RegisterAction(TEXT("combograph"), TEXT("scaffold_combo_from_montages"),
		TEXT("Auto-build a complete combo graph from an ordered list of animation montages"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldComboFromMontages),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path for the new ComboGraph"))
			.Required(TEXT("montages"), TEXT("array"), TEXT("Ordered array of AnimMontage asset paths"))
			.Optional(TEXT("input_action"), TEXT("string"), TEXT("InputAction to use for all transitions"))
			.Optional(TEXT("transition_behavior"), TEXT("string"), TEXT("Transition behavior for all edges (default: Immediately)"), TEXT("Immediately"))
			.Build());

	// ── Layout ──

	Registry.RegisterAction(TEXT("combograph"), TEXT("layout_combo_graph"),
		TEXT("Auto-layout nodes in a combo graph. Arranges nodes left-to-right following the combo chain, with branches spreading vertically."),
		FMonolithActionHandler::CreateStatic(&HandleLayoutComboGraph),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ComboGraph asset path"))
			.Optional(TEXT("horizontal_spacing"), TEXT("integer"), TEXT("Horizontal spacing between nodes (default 300)"), TEXT("300"))
			.Optional(TEXT("vertical_spacing"), TEXT("integer"), TEXT("Vertical spacing between branch nodes (default 200)"), TEXT("200"))
			.Build());

	UE_LOG(LogMonolithComboGraph, Log, TEXT("Registered 13 combograph actions"));
}

// ============================================================
//  1. list_combo_graphs
// ============================================================

FMonolithActionResult FMonolithComboGraphActions::HandleListComboGraphs(const TSharedPtr<FJsonObject>& Params)
{
	UClass* ComboGraphClass = FindComboGraphClass();
	if (!ComboGraphClass)
	{
		return FMonolithActionResult::Error(TEXT("ComboGraph class not found. Is the ComboGraph plugin loaded?"));
	}

	FString PathFilter = Params->GetStringField(TEXT("path_filter"));

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(ComboGraphClass->GetClassPathName());
	Filter.bRecursiveClasses = true;
	if (!PathFilter.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PathFilter));
		Filter.bRecursivePaths = true;
	}

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> ResultArray;
	for (const FAssetData& AssetData : Assets)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		Entry->SetStringField(TEXT("name"), AssetData.AssetName.ToString());

		// Load to get node count (lightweight — ComboGraphs are small assets)
		UObject* Graph = AssetData.GetAsset();
		if (Graph)
		{
			TArray<UObject*> Nodes = GetAllNodes(Graph);
			Entry->SetNumberField(TEXT("node_count"), Nodes.Num());
		}
		else
		{
			Entry->SetNumberField(TEXT("node_count"), -1);
		}

		ResultArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("combo_graphs"), ResultArray);
	Result->SetNumberField(TEXT("count"), ResultArray.Num());

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  2. get_combo_graph_info
// ============================================================

FMonolithActionResult FMonolithComboGraphActions::HandleGetComboGraphInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));
	}

	FString Error;
	UObject* Graph = LoadComboGraph(AssetPath, Error);
	if (!Graph)
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("class"), Graph->GetClass()->GetName());

	// AllNodes
	TArray<UObject*> Nodes = GetAllNodes(Graph);
	TArray<TSharedPtr<FJsonValue>> NodesArr;
	for (int32 i = 0; i < Nodes.Num(); i++)
	{
		if (Nodes[i])
		{
			NodesArr.Add(MakeShared<FJsonValueObject>(NodeToJson(Nodes[i], i)));
		}
	}
	Result->SetArrayField(TEXT("nodes"), NodesArr);
	Result->SetNumberField(TEXT("node_count"), Nodes.Num());

	// EntryNode / FirstNode
	UObject* EntryNode = Cast<UObject>(GetObjectProperty(Graph, TEXT("EntryNode")));
	if (EntryNode)
	{
		int32 EntryIdx = GetNodeIndex(Graph, EntryNode);
		Result->SetNumberField(TEXT("entry_node_index"), EntryIdx);
	}

	UObject* FirstNode = Cast<UObject>(GetObjectProperty(Graph, TEXT("FirstNode")));
	if (FirstNode)
	{
		int32 FirstIdx = GetNodeIndex(Graph, FirstNode);
		Result->SetNumberField(TEXT("first_node_index"), FirstIdx);
	}

	// Default properties
	FString DefaultInputAction = GetStringProperty(Graph, TEXT("DefaultInputAction"));
	if (!DefaultInputAction.IsEmpty())
	{
		Result->SetStringField(TEXT("default_input_action"), DefaultInputAction);
	}

	FString DefaultNodeMontageType = GetStringProperty(Graph, TEXT("DefaultNodeMontageType"));
	if (!DefaultNodeMontageType.IsEmpty())
	{
		Result->SetStringField(TEXT("default_node_montage_type"), DefaultNodeMontageType);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  3. get_combo_node_effects
// ============================================================

FMonolithActionResult FMonolithComboGraphActions::HandleGetComboNodeEffects(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));
	}

	int32 NodeIndex = static_cast<int32>(Params->GetNumberField(TEXT("node_index")));

	FString Error;
	UObject* Graph = LoadComboGraph(AssetPath, Error);
	if (!Graph)
	{
		return FMonolithActionResult::Error(Error);
	}

	UObject* Node = GetNodeAtIndex(Graph, NodeIndex);
	if (!Node)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Node index %d not found (graph has %d nodes)"),
				NodeIndex, GetAllNodes(Graph).Num()));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("node_index"), NodeIndex);
	Result->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
	Result->SetStringField(TEXT("node_name"), Node->GetName());

	// EffectsContainerMap — serialize via reflection
	FString EffectsMapStr = SerializeMapProperty(Node, TEXT("EffectsContainerMap"));
	Result->SetStringField(TEXT("effects_container_map_raw"), EffectsMapStr);

	// CuesContainerMap — serialize via reflection
	FString CuesMapStr = SerializeMapProperty(Node, TEXT("CuesContainerMap"));
	Result->SetStringField(TEXT("cues_container_map_raw"), CuesMapStr);

	// DamagesContainerMap
	FString DamagesMapStr = SerializeMapProperty(Node, TEXT("DamagesContainerMap"));
	if (!DamagesMapStr.IsEmpty() && DamagesMapStr != TEXT("()"))
	{
		Result->SetStringField(TEXT("damages_container_map_raw"), DamagesMapStr);
	}

	// Cost GE
	FString CostGE = GetStringProperty(Node, TEXT("CostGameplayEffect"));
	if (!CostGE.IsEmpty() && CostGE != TEXT("None"))
	{
		Result->SetStringField(TEXT("cost_gameplay_effect"), CostGE);
	}

	// Play rate
	float PlayRate = GetFloatProperty(Node, TEXT("MontagePlayRate"), 1.0f);
	Result->SetNumberField(TEXT("play_rate"), PlayRate);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  4. validate_combo_graph
// ============================================================

FMonolithActionResult FMonolithComboGraphActions::HandleValidateComboGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));
	}

	FString Error;
	UObject* Graph = LoadComboGraph(AssetPath, Error);
	if (!Graph)
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> Errors;

	TArray<UObject*> Nodes = GetAllNodes(Graph);

	// Check: empty graph
	if (Nodes.Num() == 0)
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("Graph has no nodes")));
	}

	// Check entry node
	UObject* EntryNode = Cast<UObject>(GetObjectProperty(Graph, TEXT("EntryNode")));
	if (!EntryNode && Nodes.Num() > 0)
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("No EntryNode set on graph")));
	}

	// Check first node
	UObject* FirstNode = Cast<UObject>(GetObjectProperty(Graph, TEXT("FirstNode")));
	if (!FirstNode && Nodes.Num() > 0)
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("No FirstNode set on graph")));
	}

	UClass* NodeMontageClass = FindComboNodeMontageClass();
	UClass* NodeSequenceClass = FindComboNodeSequenceClass();

	TSet<UObject*> ReachableNodes;
	// BFS from entry/first node to find reachable nodes
	if (FirstNode)
	{
		TArray<UObject*> Queue;
		Queue.Add(FirstNode);
		ReachableNodes.Add(FirstNode);
		while (Queue.Num() > 0)
		{
			UObject* Current = Queue.Pop(EAllowShrinking::No);
			TArray<UObject*> Children = GetChildrenNodes(Current);
			for (UObject* Child : Children)
			{
				if (Child && !ReachableNodes.Contains(Child))
				{
					ReachableNodes.Add(Child);
					Queue.Add(Child);
				}
			}
		}
	}

	for (int32 i = 0; i < Nodes.Num(); i++)
	{
		UObject* Node = Nodes[i];
		if (!Node)
		{
			Errors.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Null node at index %d"), i)));
			continue;
		}

		// Orphan check
		if (FirstNode && !ReachableNodes.Contains(Node) && Node != EntryNode)
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Node %d '%s' is unreachable (orphan)"), i, *Node->GetName())));
		}

		// Missing animation reference
		if (NodeMontageClass && Node->IsA(NodeMontageClass))
		{
			FString MontageRef = GetStringProperty(Node, TEXT("Montage"));
			if (MontageRef.IsEmpty() || MontageRef == TEXT("None") || MontageRef == TEXT("()"))
			{
				Errors.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Montage node %d '%s' has no animation assigned"), i, *Node->GetName())));
			}
		}
		else if (NodeSequenceClass && Node->IsA(NodeSequenceClass))
		{
			FString SeqRef = GetStringProperty(Node, TEXT("AnimationSequence"));
			if (SeqRef.IsEmpty() || SeqRef == TEXT("None") || SeqRef == TEXT("()"))
			{
				Errors.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Sequence node %d '%s' has no animation assigned"), i, *Node->GetName())));
			}
		}

		// Broken edges (edge referencing null start/end)
		TArray<UObject*> NodeEdges = GetEdges(Node);
		for (int32 e = 0; e < NodeEdges.Num(); e++)
		{
			UObject* Edge = NodeEdges[e];
			if (!Edge)
			{
				Errors.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Null edge at index %d on node %d '%s'"), e, i, *Node->GetName())));
				continue;
			}
			UObject* StartN = Cast<UObject>(GetObjectProperty(Edge, TEXT("StartNode")));
			UObject* EndN = Cast<UObject>(GetObjectProperty(Edge, TEXT("EndNode")));
			if (!StartN)
			{
				Errors.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Edge %d on node %d has null StartNode"), e, i)));
			}
			if (!EndN)
			{
				Errors.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Edge %d on node %d has null EndNode"), e, i)));
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("node_count"), Nodes.Num());
	Result->SetBoolField(TEXT("valid"), Errors.Num() == 0);
	Result->SetArrayField(TEXT("errors"), Errors);
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetNumberField(TEXT("error_count"), Errors.Num());
	Result->SetNumberField(TEXT("warning_count"), Warnings.Num());

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  5. create_combo_graph
// ============================================================

FMonolithActionResult FMonolithComboGraphActions::HandleCreateComboGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'save_path'"));
	}

	FString AssetName = ExtractAssetName(SavePath);
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Could not extract asset name from save_path"));
	}

	// Check path is free
	FString ExistError;
	if (!EnsureAssetPathFree(SavePath, AssetName, ExistError))
	{
		return FMonolithActionResult::Error(ExistError);
	}

	// Find factory
	UClass* FactoryClass = FindComboFactoryClass();
	if (!FactoryClass)
	{
		return FMonolithActionResult::Error(TEXT("ComboGraphFactory class not found. Is ComboGraphEditor loaded?"));
	}

	// Create factory instance
	UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
	if (!Factory)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create ComboGraphFactory instance"));
	}

	// Create package
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to create package at: %s"), *SavePath));
	}
	Package->FullyLoad();

	// Create the ComboGraph via factory
	UObject* NewGraph = Factory->FactoryCreateNew(
		FindComboGraphClass(), Package, FName(*AssetName),
		RF_Public | RF_Standalone, nullptr, GWarn);

	if (!NewGraph)
	{
		return FMonolithActionResult::Error(TEXT("FactoryCreateNew returned null"));
	}

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewGraph);

	// Save
	bool bSaved = SaveGraphAsset(NewGraph);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetStringField(TEXT("class"), NewGraph->GetClass()->GetName());
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Created ComboGraph '%s'"), *AssetName));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  6. add_combo_node
// ============================================================

FMonolithActionResult FMonolithComboGraphActions::HandleAddComboNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));
	}

	FString AnimAssetPath = Params->GetStringField(TEXT("animation_asset"));
	if (AnimAssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'animation_asset'"));
	}

	FString NodeType = Params->GetStringField(TEXT("node_type"));
	if (NodeType.IsEmpty()) NodeType = TEXT("montage");

	float PlayRate = 1.0f;
	if (Params->HasField(TEXT("play_rate")))
	{
		PlayRate = static_cast<float>(Params->GetNumberField(TEXT("play_rate")));
	}

	int32 ParentNodeIndex = -1;
	if (Params->HasField(TEXT("parent_node_index")))
	{
		ParentNodeIndex = static_cast<int32>(Params->GetNumberField(TEXT("parent_node_index")));
	}

	// Load graph
	FString Error;
	UObject* Graph = LoadComboGraph(AssetPath, Error);
	if (!Graph)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Resolve node class
	UClass* NodeClass = nullptr;
	if (NodeType.Equals(TEXT("sequence"), ESearchCase::IgnoreCase))
	{
		NodeClass = FindComboNodeSequenceClass();
		if (!NodeClass)
		{
			return FMonolithActionResult::Error(TEXT("ComboGraphNodeSequence class not found"));
		}
	}
	else
	{
		NodeClass = FindComboNodeMontageClass();
		if (!NodeClass)
		{
			return FMonolithActionResult::Error(TEXT("ComboGraphNodeMontage class not found"));
		}
	}

	// Load animation asset
	UObject* AnimAsset = StaticLoadObject(UObject::StaticClass(), nullptr, *AnimAssetPath);
	if (!AnimAsset)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load animation asset: %s"), *AnimAssetPath));
	}

	// Create node
	UObject* NewNode = NewObject<UObject>(Graph, NodeClass);
	if (!NewNode)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create combo node"));
	}

	// Set animation via reflection — Montage is TSoftObjectPtr, use ImportText
	if (NodeType.Equals(TEXT("montage"), ESearchCase::IgnoreCase))
	{
		FProperty* MontageProp = NewNode->GetClass()->FindPropertyByName(TEXT("Montage"));
		if (MontageProp)
		{
			FString SoftPath = AnimAssetPath;
			MontageProp->ImportText_Direct(*SoftPath, MontageProp->ContainerPtrToValuePtr<void>(NewNode), NewNode, PPF_None);
		}
	}
	else
	{
		// Sequence node — AnimationSequence property
		FProperty* SeqProp = NewNode->GetClass()->FindPropertyByName(TEXT("AnimationSequence"));
		if (SeqProp)
		{
			FString SoftPath = AnimAssetPath;
			SeqProp->ImportText_Direct(*SoftPath, SeqProp->ContainerPtrToValuePtr<void>(NewNode), NewNode, PPF_None);
		}
	}

	// Set play rate
	SetFloatProperty(NewNode, TEXT("MontagePlayRate"), PlayRate);

	// Add to AllNodes
	if (!AddNodeToAllNodes(Graph, NewNode))
	{
		return FMonolithActionResult::Error(TEXT("Failed to add node to AllNodes array"));
	}

	int32 NewNodeIndex = GetAllNodes(Graph).Num() - 1;

	// If this is the first node, set as FirstNode
	if (NewNodeIndex == 0 || !GetObjectProperty(Graph, TEXT("FirstNode")))
	{
		SetObjectProperty(Graph, TEXT("FirstNode"), NewNode);
	}

	// Connect to parent node if specified
	if (ParentNodeIndex >= 0)
	{
		UObject* ParentNode = GetNodeAtIndex(Graph, ParentNodeIndex);
		if (ParentNode)
		{
			// Add to parent's ChildrenNodes
			AddToObjectArray(ParentNode, TEXT("ChildrenNodes"), NewNode);
			// Add parent to new node's ParentNodes
			AddToObjectArray(NewNode, TEXT("ParentNodes"), ParentNode);

			// Create an edge
			UClass* EdgeClass = FindComboEdgeClass();
			if (EdgeClass)
			{
				UObject* Edge = NewObject<UObject>(Graph, EdgeClass);
				if (Edge)
				{
					SetObjectProperty(Edge, TEXT("StartNode"), ParentNode);
					SetObjectProperty(Edge, TEXT("EndNode"), NewNode);
					AddToObjectArray(ParentNode, TEXT("Edges"), Edge);
				}
			}
		}
		else
		{
			UE_LOG(LogMonolithComboGraph, Warning,
				TEXT("Parent node index %d not found, node added without connection"), ParentNodeIndex);
		}
	}

	MarkGraphDirty(Graph);
	bool bSaved = SaveGraphAsset(Graph);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("node_index"), NewNodeIndex);
	Result->SetStringField(TEXT("node_class"), NewNode->GetClass()->GetName());
	Result->SetStringField(TEXT("node_type"), NodeType);
	Result->SetStringField(TEXT("animation_asset"), AnimAssetPath);
	Result->SetNumberField(TEXT("play_rate"), PlayRate);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Added %s node at index %d"), *NodeType, NewNodeIndex));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  7. add_combo_edge
// ============================================================

FMonolithActionResult FMonolithComboGraphActions::HandleAddComboEdge(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));
	}

	int32 FromIndex = static_cast<int32>(Params->GetNumberField(TEXT("from_node_index")));
	int32 ToIndex = static_cast<int32>(Params->GetNumberField(TEXT("to_node_index")));

	FString InputAction = Params->GetStringField(TEXT("input_action"));
	FString TriggerEvent = Params->GetStringField(TEXT("trigger_event"));
	FString TransitionBehavior = Params->GetStringField(TEXT("transition_behavior"));

	// Load graph
	FString Error;
	UObject* Graph = LoadComboGraph(AssetPath, Error);
	if (!Graph)
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<UObject*> Nodes = GetAllNodes(Graph);

	UObject* FromNode = Nodes.IsValidIndex(FromIndex) ? Nodes[FromIndex] : nullptr;
	UObject* ToNode = Nodes.IsValidIndex(ToIndex) ? Nodes[ToIndex] : nullptr;

	if (!FromNode)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Source node index %d not found (graph has %d nodes)"), FromIndex, Nodes.Num()));
	}
	if (!ToNode)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Target node index %d not found (graph has %d nodes)"), ToIndex, Nodes.Num()));
	}

	// Create edge
	UClass* EdgeClass = FindComboEdgeClass();
	if (!EdgeClass)
	{
		return FMonolithActionResult::Error(TEXT("ComboGraphEdge class not found"));
	}

	UObject* Edge = NewObject<UObject>(Graph, EdgeClass);
	if (!Edge)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create edge"));
	}

	// Set start/end nodes
	SetObjectProperty(Edge, TEXT("StartNode"), FromNode);
	SetObjectProperty(Edge, TEXT("EndNode"), ToNode);

	// Set transition input (InputAction soft ref)
	if (!InputAction.IsEmpty())
	{
		FProperty* TransInputProp = Edge->GetClass()->FindPropertyByName(TEXT("TransitionInput"));
		if (TransInputProp)
		{
			TransInputProp->ImportText_Direct(*InputAction, TransInputProp->ContainerPtrToValuePtr<void>(Edge), Edge, PPF_None);
		}
	}

	// Set trigger event enum
	if (!TriggerEvent.IsEmpty())
	{
		SetEnumPropertyFromString(Edge, TEXT("TriggerEvent"), TriggerEvent);
	}

	// Set transition behavior enum
	if (!TransitionBehavior.IsEmpty())
	{
		SetEnumPropertyFromString(Edge, TEXT("TransitionBehavior"), TransitionBehavior);
	}

	// Wire up graph relationships
	AddToObjectArray(FromNode, TEXT("Edges"), Edge);
	AddToObjectArray(FromNode, TEXT("ChildrenNodes"), ToNode);
	AddToObjectArray(ToNode, TEXT("ParentNodes"), FromNode);

	MarkGraphDirty(Graph);
	bool bSaved = SaveGraphAsset(Graph);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("from_node_index"), FromIndex);
	Result->SetNumberField(TEXT("to_node_index"), ToIndex);
	if (!InputAction.IsEmpty()) Result->SetStringField(TEXT("input_action"), InputAction);
	if (!TriggerEvent.IsEmpty()) Result->SetStringField(TEXT("trigger_event"), TriggerEvent);
	if (!TransitionBehavior.IsEmpty()) Result->SetStringField(TEXT("transition_behavior"), TransitionBehavior);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Connected node %d -> %d"), FromIndex, ToIndex));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  8. set_combo_node_effects
// ============================================================

FMonolithActionResult FMonolithComboGraphActions::HandleSetComboNodeEffects(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));
	}

	int32 NodeIndex = static_cast<int32>(Params->GetNumberField(TEXT("node_index")));

	const TSharedPtr<FJsonObject>* EffectsObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("effects"), EffectsObj) || !EffectsObj || !(*EffectsObj).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'effects' (must be a JSON object)"));
	}

	// Load graph
	FString Error;
	UObject* Graph = LoadComboGraph(AssetPath, Error);
	if (!Graph)
	{
		return FMonolithActionResult::Error(Error);
	}

	UObject* Node = GetNodeAtIndex(Graph, NodeIndex);
	if (!Node)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Node index %d not found"), NodeIndex));
	}

	// Access EffectsContainerMap via reflection
	// EffectsContainerMap is TMap<FGameplayTag, FComboGraphGameplayEffectContainer>
	FMapProperty* MapProp = CastField<FMapProperty>(Node->GetClass()->FindPropertyByName(TEXT("EffectsContainerMap")));
	if (!MapProp)
	{
		return FMonolithActionResult::Error(TEXT("EffectsContainerMap property not found on node. Is this a ComboGraphNodeAnimBase?"));
	}

	// Build the text representation for ImportText
	// Format: ((TagName="Damage.Type.Ballistic",TagGuid=...),(TargetGameplayEffectClasses=("/Game/GE"),UseSetByCaller=True,...))
	// This is complex — use property-level ImportText with UE text format
	// Instead of trying to build the exact text format, we'll set the map by iterating keys
	// and building container structs via the inner property's ImportText

	// Clear the existing map first
	FScriptMapHelper MapHelper(MapProp, MapProp->ContainerPtrToValuePtr<void>(Node));
	MapHelper.EmptyValues();

	int32 EntriesSet = 0;

	for (const auto& Pair : (*EffectsObj)->Values)
	{
		FString TagName = Pair.Key;
		const TSharedPtr<FJsonObject>* ContainerObj = nullptr;

		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
		{
			UE_LOG(LogMonolithComboGraph, Warning,
				TEXT("Skipping effects entry '%s': value is not an object"), *TagName);
			continue;
		}
		ContainerObj = &(Pair.Value->AsObject());

		// Build the text representation for this map entry
		// Key: GameplayTag text
		// Value: container struct text
		FString KeyText = FString::Printf(TEXT("(TagName=\"%s\")"), *TagName);

		// Build container text
		TArray<FString> ContainerParts;
		const TSharedPtr<FJsonObject>& ContainerData = *ContainerObj;

		// effect_classes -> TargetGameplayEffectClasses
		const TArray<TSharedPtr<FJsonValue>>* EffectClasses = nullptr;
		if (ContainerData->TryGetArrayField(TEXT("effect_classes"), EffectClasses) && EffectClasses)
		{
			TArray<FString> ClassPaths;
			for (const auto& Val : *EffectClasses)
			{
				if (Val.IsValid()) ClassPaths.Add(Val->AsString());
			}
			if (ClassPaths.Num() > 0)
			{
				FString ClassesStr = TEXT("(");
				for (int32 c = 0; c < ClassPaths.Num(); c++)
				{
					if (c > 0) ClassesStr += TEXT(",");
					ClassesStr += ClassPaths[c];
				}
				ClassesStr += TEXT(")");
				ContainerParts.Add(FString::Printf(TEXT("TargetGameplayEffectClasses=%s"), *ClassesStr));
			}
		}

		// use_set_by_caller -> UseSetByCaller
		bool bUseSetByCaller = false;
		if (ContainerData->TryGetBoolField(TEXT("use_set_by_caller"), bUseSetByCaller))
		{
			ContainerParts.Add(FString::Printf(TEXT("UseSetByCaller=%s"),
				bUseSetByCaller ? TEXT("True") : TEXT("False")));
		}

		// set_by_caller_tag -> SetByCallerTag
		FString SetByCallerTag;
		if (ContainerData->TryGetStringField(TEXT("set_by_caller_tag"), SetByCallerTag) && !SetByCallerTag.IsEmpty())
		{
			ContainerParts.Add(FString::Printf(TEXT("SetByCallerTag=(TagName=\"%s\")"), *SetByCallerTag));
		}

		// set_by_caller_magnitude -> SetByCallerMagnitude
		double Magnitude = 0.0;
		if (ContainerData->TryGetNumberField(TEXT("set_by_caller_magnitude"), Magnitude))
		{
			ContainerParts.Add(FString::Printf(TEXT("SetByCallerMagnitude=%f"), Magnitude));
		}

		FString ValueText = TEXT("(") + FString::Join(ContainerParts, TEXT(",")) + TEXT(")");

		// Add entry to map via AddDefaultValue_Invalid_NeedsRehash + set key/value
		int32 NewIndex = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
		MapProp->KeyProp->ImportText_Direct(*KeyText, MapHelper.GetKeyPtr(NewIndex), Node, PPF_None);
		MapProp->ValueProp->ImportText_Direct(*ValueText, MapHelper.GetValuePtr(NewIndex), Node, PPF_None);
		EntriesSet++;
	}

	MapHelper.Rehash();

	MarkGraphDirty(Graph);
	bool bSaved = SaveGraphAsset(Graph);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("node_index"), NodeIndex);
	Result->SetNumberField(TEXT("entries_set"), EntriesSet);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Set %d effect container entries on node %d"), EntriesSet, NodeIndex));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  9. set_combo_node_cues
// ============================================================

FMonolithActionResult FMonolithComboGraphActions::HandleSetComboNodeCues(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));
	}

	int32 NodeIndex = static_cast<int32>(Params->GetNumberField(TEXT("node_index")));

	const TSharedPtr<FJsonObject>* CuesObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("cues"), CuesObj) || !CuesObj || !(*CuesObj).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'cues' (must be a JSON object)"));
	}

	// Load graph
	FString Error;
	UObject* Graph = LoadComboGraph(AssetPath, Error);
	if (!Graph)
	{
		return FMonolithActionResult::Error(Error);
	}

	UObject* Node = GetNodeAtIndex(Graph, NodeIndex);
	if (!Node)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Node index %d not found"), NodeIndex));
	}

	// Access CuesContainerMap via reflection
	FMapProperty* MapProp = CastField<FMapProperty>(Node->GetClass()->FindPropertyByName(TEXT("CuesContainerMap")));
	if (!MapProp)
	{
		return FMonolithActionResult::Error(TEXT("CuesContainerMap property not found on node"));
	}

	FScriptMapHelper MapHelper(MapProp, MapProp->ContainerPtrToValuePtr<void>(Node));
	MapHelper.EmptyValues();

	int32 EntriesSet = 0;

	for (const auto& Pair : (*CuesObj)->Values)
	{
		FString TagName = Pair.Key;
		const TSharedPtr<FJsonObject>* ContainerObj = nullptr;

		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
		{
			UE_LOG(LogMonolithComboGraph, Warning,
				TEXT("Skipping cues entry '%s': value is not an object"), *TagName);
			continue;
		}
		ContainerObj = &(Pair.Value->AsObject());
		const TSharedPtr<FJsonObject>& ContainerData = *ContainerObj;

		FString KeyText = FString::Printf(TEXT("(TagName=\"%s\")"), *TagName);

		// Build container text from definitions array
		const TArray<TSharedPtr<FJsonValue>>* Definitions = nullptr;
		TArray<FString> DefTexts;

		if (ContainerData->TryGetArrayField(TEXT("definitions"), Definitions) && Definitions)
		{
			for (const auto& DefVal : *Definitions)
			{
				if (!DefVal.IsValid() || DefVal->Type != EJson::Object) continue;
				const TSharedPtr<FJsonObject>& Def = DefVal->AsObject();

				TArray<FString> DefParts;

				// gameplay_cue_tags -> GameplayCueTags
				const TArray<TSharedPtr<FJsonValue>>* CueTags = nullptr;
				if (Def->TryGetArrayField(TEXT("gameplay_cue_tags"), CueTags) && CueTags)
				{
					TArray<FString> TagTexts;
					for (const auto& TV : *CueTags)
					{
						if (TV.IsValid())
						{
							TagTexts.Add(FString::Printf(TEXT("(TagName=\"%s\")"), *TV->AsString()));
						}
					}
					if (TagTexts.Num() > 0)
					{
						DefParts.Add(FString::Printf(TEXT("GameplayCueTags=(GameplayTags=(%s))"),
							*FString::Join(TagTexts, TEXT(","))));
					}
				}

				// source_type -> SourceType (enum)
				FString SourceType;
				if (Def->TryGetStringField(TEXT("source_type"), SourceType) && !SourceType.IsEmpty())
				{
					DefParts.Add(FString::Printf(TEXT("SourceType=%s"), *SourceType));
				}

				// source_asset -> SourceAsset (soft object ref)
				FString SourceAsset;
				if (Def->TryGetStringField(TEXT("source_asset"), SourceAsset) && !SourceAsset.IsEmpty())
				{
					DefParts.Add(FString::Printf(TEXT("SourceAsset=%s"), *SourceAsset));
				}

				if (DefParts.Num() > 0)
				{
					DefTexts.Add(TEXT("(") + FString::Join(DefParts, TEXT(",")) + TEXT(")"));
				}
			}
		}

		FString ValueText;
		if (DefTexts.Num() > 0)
		{
			ValueText = FString::Printf(TEXT("(Definitions=(%s))"), *FString::Join(DefTexts, TEXT(",")));
		}
		else
		{
			ValueText = TEXT("()");
		}

		int32 NewIndex = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
		MapProp->KeyProp->ImportText_Direct(*KeyText, MapHelper.GetKeyPtr(NewIndex), Node, PPF_None);
		MapProp->ValueProp->ImportText_Direct(*ValueText, MapHelper.GetValuePtr(NewIndex), Node, PPF_None);
		EntriesSet++;
	}

	MapHelper.Rehash();

	MarkGraphDirty(Graph);
	bool bSaved = SaveGraphAsset(Graph);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("node_index"), NodeIndex);
	Result->SetNumberField(TEXT("entries_set"), EntriesSet);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Set %d cue container entries on node %d"), EntriesSet, NodeIndex));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  10. create_combo_ability
// ============================================================

FMonolithActionResult FMonolithComboGraphActions::HandleCreateComboAbility(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'save_path'"));
	}

	FString ComboGraphPath = Params->GetStringField(TEXT("combo_graph"));
	FString InitialInput = Params->GetStringField(TEXT("initial_input"));
	FString ParentClassName = Params->GetStringField(TEXT("parent_class"));
	if (ParentClassName.IsEmpty()) ParentClassName = TEXT("GameplayAbility");

	FString AssetName = ExtractAssetName(SavePath);
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Could not extract asset name from save_path"));
	}

	// Check path is free
	FString ExistError;
	if (!EnsureAssetPathFree(SavePath, AssetName, ExistError))
	{
		return FMonolithActionResult::Error(ExistError);
	}

	// Resolve parent class
	FString FullClassName = ParentClassName;
	if (!FullClassName.StartsWith(TEXT("U")))
	{
		FullClassName = TEXT("U") + FullClassName;
	}
	UClass* ParentClass = FindFirstObject<UClass>(*FullClassName, EFindFirstObjectOptions::NativeFirst);
	if (!ParentClass)
	{
		// Try without U prefix
		ParentClass = FindFirstObject<UClass>(*ParentClassName, EFindFirstObjectOptions::NativeFirst);
	}
	if (!ParentClass)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Parent class not found: %s"), *ParentClassName));
	}
	if (!ParentClass->IsChildOf(UGameplayAbility::StaticClass()))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Parent class '%s' is not a GameplayAbility subclass"), *ParentClassName));
	}

	// Create package
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to create package at: %s"), *SavePath));
	}
	Package->FullyLoad();

	// Create Blueprint
	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass, Package, FName(*AssetName),
		BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());

	if (!NewBP)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create ability Blueprint"));
	}

	// Set instancing policy to InstancedPerActor (required for ability tasks)
	if (NewBP->GeneratedClass)
	{
		UGameplayAbility* CDO = Cast<UGameplayAbility>(NewBP->GeneratedClass->GetDefaultObject());
		if (CDO)
		{
			// Use reflection to set InstancingPolicy
			FProperty* InstProp = CDO->GetClass()->FindPropertyByName(TEXT("InstancingPolicy"));
			if (InstProp)
			{
				InstProp->ImportText_Direct(TEXT("InstancedPerActor"), InstProp->ContainerPtrToValuePtr<void>(CDO), CDO, PPF_None);
			}
		}
	}

	// Add StartComboGraph ability task node
	bool bTaskWired = false;
	UClass* TaskClass = FindComboAbilityTaskClass();

	if (TaskClass)
	{
		// Find the event graph
		UEdGraph* EventGraph = nullptr;
		for (UEdGraph* Graph : NewBP->UbergraphPages)
		{
			if (Graph && Graph->GetFName() == UEdGraphSchema_K2::GN_EventGraph)
			{
				EventGraph = Graph;
				break;
			}
		}
		if (!EventGraph && NewBP->UbergraphPages.Num() > 0)
		{
			EventGraph = NewBP->UbergraphPages[0];
		}

		if (EventGraph)
		{
			// Find the factory function (StartGraph or StartComboGraph)
			UFunction* FactoryFunc = nullptr;
			for (TFieldIterator<UFunction> It(TaskClass); It; ++It)
			{
				if (It->HasAnyFunctionFlags(FUNC_Static) && It->GetName().Contains(TEXT("Start")))
				{
					FactoryFunc = *It;
					break;
				}
			}

			if (FactoryFunc)
			{
				UK2Node_LatentAbilityCall* TaskNode = NewObject<UK2Node_LatentAbilityCall>(EventGraph);

				// Set ProxyFactoryFunctionName, ProxyFactoryClass, ProxyClass via reflection
				{
					FProperty* FFNProp = TaskNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryFunctionName"));
					if (FFNProp) { *FFNProp->ContainerPtrToValuePtr<FName>(TaskNode) = FactoryFunc->GetFName(); }
					FProperty* FFCProp = TaskNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryClass"));
					if (FFCProp) { *FFCProp->ContainerPtrToValuePtr<UClass*>(TaskNode) = TaskClass; }
					FProperty* PCProp = TaskNode->GetClass()->FindPropertyByName(TEXT("ProxyClass"));
					if (PCProp) { *PCProp->ContainerPtrToValuePtr<UClass*>(TaskNode) = TaskClass; }
				}

				TaskNode->NodePosX = 400;
				TaskNode->NodePosY = 0;
				EventGraph->AddNode(TaskNode, /*bUserAction=*/true, /*bSelectNewNode=*/false);
				TaskNode->AllocateDefaultPins();

				// If combo_graph path provided, set the pin default
				if (!ComboGraphPath.IsEmpty())
				{
					UEdGraphPin* GraphPin = TaskNode->FindPin(TEXT("ComboGraph"));
					if (!GraphPin) GraphPin = TaskNode->FindPin(TEXT("ComboGraphAsset"));
					if (!GraphPin) GraphPin = TaskNode->FindPin(TEXT("InComboGraph"));
					if (GraphPin)
					{
						GraphPin->DefaultValue = ComboGraphPath;
					}
				}

				// If initial input provided, set it
				if (!InitialInput.IsEmpty())
				{
					UEdGraphPin* InputPin = TaskNode->FindPin(TEXT("InitialInput"));
					if (!InputPin) InputPin = TaskNode->FindPin(TEXT("InputAction"));
					if (InputPin)
					{
						InputPin->DefaultValue = InitialInput;
					}
				}

				bTaskWired = true;
			}
			else
			{
				UE_LOG(LogMonolithComboGraph, Warning,
					TEXT("Could not find StartGraph factory function on %s"), *TaskClass->GetName());
			}
		}
	}
	else
	{
		UE_LOG(LogMonolithComboGraph, Warning,
			TEXT("ComboGraphAbilityTask_StartGraph class not found — ability created without task node"));
	}

	// Compile
	FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::SkipGarbageCollection);

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewBP);

	// Save
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	bool bSaved = UPackage::SavePackage(Package, NewBP, *PackageFilename, SaveArgs);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	Result->SetBoolField(TEXT("combo_task_wired"), bTaskWired);
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (!ComboGraphPath.IsEmpty()) Result->SetStringField(TEXT("combo_graph"), ComboGraphPath);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Created combo ability '%s'%s"),
			*AssetName,
			bTaskWired ? TEXT(" with StartComboGraph task") : TEXT(" (task node not wired)")));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  11. link_ability_to_combo_graph
// ============================================================

FMonolithActionResult FMonolithComboGraphActions::HandleLinkAbilityToComboGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AbilityPath = Params->GetStringField(TEXT("ability_path"));
	if (AbilityPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'ability_path'"));
	}

	FString ComboGraphPath = Params->GetStringField(TEXT("combo_graph"));
	if (ComboGraphPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'combo_graph'"));
	}

	// Load blueprint
	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AbilityPath);
	UBlueprint* BP = Cast<UBlueprint>(Asset);
	if (!BP)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load Blueprint at '%s'"), *AbilityPath));
	}

	if (!BP->GeneratedClass || !BP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("'%s' is not a GameplayAbility Blueprint"), *AbilityPath));
	}

	// Find existing StartComboGraph task node in event graphs
	UClass* TaskClass = FindComboAbilityTaskClass();
	if (!TaskClass)
	{
		return FMonolithActionResult::Error(TEXT("ComboGraphAbilityTask_StartGraph class not found"));
	}

	UK2Node_LatentAbilityCall* ExistingTaskNode = nullptr;
	UEdGraph* TargetGraph = nullptr;

	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_LatentAbilityCall* LatentNode = Cast<UK2Node_LatentAbilityCall>(Node);
			if (!LatentNode) continue;

			// Check if this task node uses our ComboGraph task class
			FProperty* FFCProp = LatentNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryClass"));
			if (FFCProp)
			{
				UClass** ClassPtr = FFCProp->ContainerPtrToValuePtr<UClass*>(LatentNode);
				if (ClassPtr && *ClassPtr == TaskClass)
				{
					ExistingTaskNode = LatentNode;
					TargetGraph = Graph;
					break;
				}
			}
		}
		if (ExistingTaskNode) break;
	}

	bool bCreatedNew = false;

	if (ExistingTaskNode)
	{
		// Update existing node's combo graph pin
		UEdGraphPin* GraphPin = ExistingTaskNode->FindPin(TEXT("ComboGraph"));
		if (!GraphPin) GraphPin = ExistingTaskNode->FindPin(TEXT("ComboGraphAsset"));
		if (!GraphPin) GraphPin = ExistingTaskNode->FindPin(TEXT("InComboGraph"));
		if (GraphPin)
		{
			GraphPin->DefaultValue = ComboGraphPath;
		}
		else
		{
			return FMonolithActionResult::Error(TEXT("Found StartComboGraph task node but could not find ComboGraph pin"));
		}
	}
	else
	{
		// Create new task node
		TargetGraph = nullptr;
		for (UEdGraph* Graph : BP->UbergraphPages)
		{
			if (Graph && Graph->GetFName() == UEdGraphSchema_K2::GN_EventGraph)
			{
				TargetGraph = Graph;
				break;
			}
		}
		if (!TargetGraph && BP->UbergraphPages.Num() > 0)
		{
			TargetGraph = BP->UbergraphPages[0];
		}

		if (!TargetGraph)
		{
			return FMonolithActionResult::Error(TEXT("No event graph found in ability Blueprint"));
		}

		// Find factory function
		UFunction* FactoryFunc = nullptr;
		for (TFieldIterator<UFunction> It(TaskClass); It; ++It)
		{
			if (It->HasAnyFunctionFlags(FUNC_Static) && It->GetName().Contains(TEXT("Start")))
			{
				FactoryFunc = *It;
				break;
			}
		}

		if (!FactoryFunc)
		{
			return FMonolithActionResult::Error(TEXT("Could not find StartGraph factory function on task class"));
		}

		UK2Node_LatentAbilityCall* TaskNode = NewObject<UK2Node_LatentAbilityCall>(TargetGraph);
		{
			FProperty* FFNProp = TaskNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryFunctionName"));
			if (FFNProp) { *FFNProp->ContainerPtrToValuePtr<FName>(TaskNode) = FactoryFunc->GetFName(); }
			FProperty* FFCProp = TaskNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryClass"));
			if (FFCProp) { *FFCProp->ContainerPtrToValuePtr<UClass*>(TaskNode) = TaskClass; }
			FProperty* PCProp = TaskNode->GetClass()->FindPropertyByName(TEXT("ProxyClass"));
			if (PCProp) { *PCProp->ContainerPtrToValuePtr<UClass*>(TaskNode) = TaskClass; }
		}
		TaskNode->NodePosX = 400;
		TaskNode->NodePosY = 0;
		TargetGraph->AddNode(TaskNode, /*bUserAction=*/true, /*bSelectNewNode=*/false);
		TaskNode->AllocateDefaultPins();

		UEdGraphPin* GraphPin = TaskNode->FindPin(TEXT("ComboGraph"));
		if (!GraphPin) GraphPin = TaskNode->FindPin(TEXT("ComboGraphAsset"));
		if (!GraphPin) GraphPin = TaskNode->FindPin(TEXT("InComboGraph"));
		if (GraphPin)
		{
			GraphPin->DefaultValue = ComboGraphPath;
		}

		bCreatedNew = true;
	}

	// Mark modified and compile
	BP->Modify();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

	BP->GetPackage()->MarkPackageDirty();

	// Save
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		BP->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	bool bSaved = UPackage::SavePackage(BP->GetPackage(), BP, *PackageFilename, SaveArgs);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("ability_path"), AbilityPath);
	Result->SetStringField(TEXT("combo_graph"), ComboGraphPath);
	Result->SetBoolField(TEXT("created_new_node"), bCreatedNew);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("%s StartComboGraph task with graph '%s'"),
			bCreatedNew ? TEXT("Created") : TEXT("Updated"),
			*ComboGraphPath));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  12. scaffold_combo_from_montages
// ============================================================

FMonolithActionResult FMonolithComboGraphActions::HandleScaffoldComboFromMontages(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'save_path'"));
	}

	const TArray<TSharedPtr<FJsonValue>>* MontagesArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("montages"), MontagesArr) || !MontagesArr || MontagesArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param 'montages' (array of asset paths)"));
	}

	FString InputAction = Params->GetStringField(TEXT("input_action"));
	FString TransitionBehavior = Params->GetStringField(TEXT("transition_behavior"));
	if (TransitionBehavior.IsEmpty()) TransitionBehavior = TEXT("Immediately");

	// Parse montage paths
	TArray<FString> MontagePaths;
	for (const auto& Val : *MontagesArr)
	{
		if (Val.IsValid())
		{
			FString Path = Val->AsString();
			if (!Path.IsEmpty())
			{
				MontagePaths.Add(Path);
			}
		}
	}

	if (MontagePaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No valid montage paths in 'montages' array"));
	}

	// Step 1: Create the combo graph
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("save_path"), SavePath);
	FMonolithActionResult CreateResult = HandleCreateComboGraph(CreateParams);
	if (!CreateResult.bSuccess)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to create combo graph: %s"), *CreateResult.ErrorMessage));
	}

	// Step 2: Add nodes for each montage
	TArray<int32> NodeIndices;
	for (int32 i = 0; i < MontagePaths.Num(); i++)
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("asset_path"), SavePath);
		NodeParams->SetStringField(TEXT("animation_asset"), MontagePaths[i]);
		NodeParams->SetStringField(TEXT("node_type"), TEXT("montage"));

		FMonolithActionResult NodeResult = HandleAddComboNode(NodeParams);
		if (!NodeResult.bSuccess)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to add node for montage '%s': %s"),
					*MontagePaths[i], *NodeResult.ErrorMessage));
		}

		int32 NodeIndex = static_cast<int32>(NodeResult.Result->GetNumberField(TEXT("node_index")));
		NodeIndices.Add(NodeIndex);
	}

	// Step 3: Chain nodes with edges
	int32 EdgesCreated = 0;
	for (int32 i = 0; i < NodeIndices.Num() - 1; i++)
	{
		TSharedPtr<FJsonObject> EdgeParams = MakeShared<FJsonObject>();
		EdgeParams->SetStringField(TEXT("asset_path"), SavePath);
		EdgeParams->SetNumberField(TEXT("from_node_index"), NodeIndices[i]);
		EdgeParams->SetNumberField(TEXT("to_node_index"), NodeIndices[i + 1]);

		if (!InputAction.IsEmpty())
		{
			EdgeParams->SetStringField(TEXT("input_action"), InputAction);
		}
		if (!TransitionBehavior.IsEmpty())
		{
			EdgeParams->SetStringField(TEXT("transition_behavior"), TransitionBehavior);
		}

		FMonolithActionResult EdgeResult = HandleAddComboEdge(EdgeParams);
		if (!EdgeResult.bSuccess)
		{
			UE_LOG(LogMonolithComboGraph, Warning,
				TEXT("Failed to create edge %d->%d: %s"),
				NodeIndices[i], NodeIndices[i + 1], *EdgeResult.ErrorMessage);
		}
		else
		{
			EdgesCreated++;
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetNumberField(TEXT("nodes_created"), MontagePaths.Num());
	Result->SetNumberField(TEXT("edges_created"), EdgesCreated);

	TArray<TSharedPtr<FJsonValue>> MontageList;
	for (const FString& Path : MontagePaths)
	{
		MontageList.Add(MakeShared<FJsonValueString>(Path));
	}
	Result->SetArrayField(TEXT("montages"), MontageList);

	if (!InputAction.IsEmpty()) Result->SetStringField(TEXT("input_action"), InputAction);
	Result->SetStringField(TEXT("transition_behavior"), TransitionBehavior);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Scaffolded combo graph with %d nodes and %d edges from %d montages"),
			MontagePaths.Num(), EdgesCreated, MontagePaths.Num()));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  13. layout_combo_graph
// ============================================================

FMonolithActionResult FMonolithComboGraphActions::HandleLayoutComboGraph(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	const int32 HSpacing = Params->HasField(TEXT("horizontal_spacing"))
		? static_cast<int32>(Params->GetNumberField(TEXT("horizontal_spacing")))
		: 300;
	const int32 VSpacing = Params->HasField(TEXT("vertical_spacing"))
		? static_cast<int32>(Params->GetNumberField(TEXT("vertical_spacing")))
		: 200;

	// Load the ComboGraph asset
	UObject* Graph = LoadObject<UObject>(nullptr, *AssetPath);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));
	}

	UClass* ComboGraphClass = FindComboGraphClass();
	if (!ComboGraphClass || !Graph->IsA(ComboGraphClass))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset is not a ComboGraph: %s"), *AssetPath));
	}

	// Get the EditorGraph (UEdGraph) via reflection
	FObjectProperty* EdGraphProp = CastField<FObjectProperty>(Graph->GetClass()->FindPropertyByName(TEXT("EditorGraph")));
	if (!EdGraphProp)
	{
		return FMonolithActionResult::Error(TEXT("Could not find EditorGraph property on ComboGraph"));
	}

	UEdGraph* EdGraph = Cast<UEdGraph>(EdGraphProp->GetObjectPropertyValue(EdGraphProp->ContainerPtrToValuePtr<void>(Graph)));
	if (!EdGraph)
	{
		return FMonolithActionResult::Error(TEXT("EditorGraph is null"));
	}

	if (EdGraph->Nodes.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("EditorGraph has no nodes"));
	}

	// Find the entry node
	UEdGraphNode* EntryNode = nullptr;
	for (UEdGraphNode* Node : EdGraph->Nodes)
	{
		if (Node && Node->GetClass()->GetName().Contains(TEXT("Entry")))
		{
			EntryNode = Node;
			break;
		}
	}
	if (!EntryNode && EdGraph->Nodes.Num() > 0)
	{
		EntryNode = EdGraph->Nodes[0];
	}

	// BFS from entry to assign depths
	TMap<UEdGraphNode*, int32> DepthMap;
	TQueue<UEdGraphNode*> Queue;
	if (EntryNode)
	{
		Queue.Enqueue(EntryNode);
		DepthMap.Add(EntryNode, 0);
	}

	int32 MaxDepth = 0;

	while (!Queue.IsEmpty())
	{
		UEdGraphNode* Current;
		Queue.Dequeue(Current);
		int32 CurrentDepth = DepthMap[Current];
		MaxDepth = FMath::Max(MaxDepth, CurrentDepth);

		for (UEdGraphPin* Pin : Current->Pins)
		{
			if (Pin->Direction == EGPD_Output)
			{
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					UEdGraphNode* NextNode = LinkedPin->GetOwningNode();
					if (NextNode && !DepthMap.Contains(NextNode))
					{
						DepthMap.Add(NextNode, CurrentDepth + 1);
						Queue.Enqueue(NextNode);
					}
				}
			}
		}
	}

	// Assign positions by depth layer
	TMap<int32, int32> DepthCounters;
	int32 MaxY = 0;
	int32 LayoutNodeCount = 0;

	for (auto& Pair : DepthMap)
	{
		int32 Depth = Pair.Value;
		int32 Index = DepthCounters.FindOrAdd(Depth);
		DepthCounters[Depth] = Index + 1;

		Pair.Key->NodePosX = Depth * HSpacing;
		Pair.Key->NodePosY = Index * VSpacing;

		MaxY = FMath::Max(MaxY, Index * VSpacing);
		LayoutNodeCount++;
	}

	// Handle unconnected nodes (not reached by BFS) — place below the main layout
	int32 UnconnectedY = MaxY + VSpacing * 2;
	int32 UnconnectedX = 0;
	int32 UnconnectedCount = 0;

	for (UEdGraphNode* Node : EdGraph->Nodes)
	{
		if (Node && !DepthMap.Contains(Node))
		{
			// Check if this is a comment node — place comments at the end
			Node->NodePosX = UnconnectedX;
			Node->NodePosY = UnconnectedY;
			UnconnectedX += HSpacing;
			UnconnectedCount++;
			LayoutNodeCount++;
		}
	}

	// Mark dirty
	Graph->MarkPackageDirty();
	EdGraph->NotifyGraphChanged();

	// Build result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("nodes_laid_out"), LayoutNodeCount);
	Result->SetNumberField(TEXT("connected_nodes"), DepthMap.Num());
	Result->SetNumberField(TEXT("unconnected_nodes"), UnconnectedCount);
	Result->SetNumberField(TEXT("max_depth"), MaxDepth);
	Result->SetNumberField(TEXT("horizontal_spacing"), HSpacing);
	Result->SetNumberField(TEXT("vertical_spacing"), VSpacing);
	Result->SetNumberField(TEXT("layout_width"), MaxDepth * HSpacing);
	Result->SetNumberField(TEXT("layout_height"), MaxY);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Laid out %d nodes across %d layers (%d unconnected placed below)"),
			LayoutNodeCount, MaxDepth + 1, UnconnectedCount));

	return FMonolithActionResult::Success(Result);
}

#else // !WITH_COMBOGRAPH

void FMonolithComboGraphActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	UE_LOG(LogMonolithComboGraph, Log, TEXT("ComboGraph not installed — no combograph actions registered"));
}

#endif // WITH_COMBOGRAPH
