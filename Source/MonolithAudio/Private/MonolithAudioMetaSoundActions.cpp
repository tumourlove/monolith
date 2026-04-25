#include "MonolithAudioMetaSoundActions.h"

#if WITH_METASOUND

#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

// MetaSound Engine
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundSource.h"
#include "Metasound.h"

// MetaSound Frontend
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendNodeClassRegistry.h"

// IMetaSoundDocumentInterface — used by TScriptInterface in Builder/Editor subsystem APIs
// MetasoundSource.h typically pulls this in, but include explicitly for Cast<> usage
#include "MetasoundDocumentInterface.h"
// Fallback if above doesn't exist: interface comes from MetasoundBuilderBase.h or MetasoundSource.h transitively

// MetaSound Editor
#include "MetasoundEditorSubsystem.h"

// Engine
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Sound/SoundWave.h"
#include "UObject/SavePackage.h"
#include "Editor.h"

// JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ============================================================================
// Phase F #3 — node_id alias registry
// ----------------------------------------------------------------------------
// add_metasound_node accepts an optional decorative `node_id` (user label).
// Without this registry the param was echoed back to the caller but the engine
// never associated it with the new node, so subsequent calls (remove_metasound_node,
// connect_metasound_nodes, etc.) had to use the raw GUID. The alias registry
// maps (AssetPath, UserLabel) -> Engine NodeID GUID so callers can keep using
// their labels.
// ============================================================================

namespace
{
	// Per-asset alias map. Static to the module — lives until editor shutdown.
	// Outer key = MetaSound asset path; inner key = user label; value = engine NodeID.
	TMap<FString, TMap<FString, FGuid>>& GetMetaSoundNodeIdAliasMap()
	{
		static TMap<FString, TMap<FString, FGuid>> Map;
		return Map;
	}
}

void FMonolithAudioMetaSoundActions::RegisterNodeIdAlias(const FString& AssetPath, const FString& UserLabel, const FGuid& NodeGuid)
{
	if (AssetPath.IsEmpty() || UserLabel.IsEmpty() || !NodeGuid.IsValid())
	{
		return;
	}
	GetMetaSoundNodeIdAliasMap().FindOrAdd(AssetPath).Add(UserLabel, NodeGuid);
}

FGuid FMonolithAudioMetaSoundActions::LookupNodeIdAlias(const FString& AssetPath, const FString& UserLabel)
{
	if (AssetPath.IsEmpty() || UserLabel.IsEmpty())
	{
		return FGuid();
	}
	if (TMap<FString, FGuid>* PerAsset = GetMetaSoundNodeIdAliasMap().Find(AssetPath))
	{
		if (FGuid* Found = PerAsset->Find(UserLabel))
		{
			return *Found;
		}
	}
	return FGuid();
}

// ============================================================================
// Helpers
// ============================================================================

bool FMonolithAudioMetaSoundActions::SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName)
{
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash <= 0)
	{
		return false;
	}
	OutPackagePath = AssetPath.Left(LastSlash);
	OutAssetName = AssetPath.Mid(LastSlash + 1);
	return !OutAssetName.IsEmpty();
}

bool FMonolithAudioMetaSoundActions::CreateLiteralFromJson(
	const TSharedPtr<FJsonValue>& JsonVal, const FString& DataType,
	FMetasoundFrontendLiteral& OutLiteral, FString& OutError)
{
	UMetaSoundBuilderSubsystem& Sub = UMetaSoundBuilderSubsystem::GetChecked();
	FName DummyDataType;

	if (DataType.Equals(TEXT("Float"), ESearchCase::IgnoreCase) ||
		DataType.Equals(TEXT("Time"), ESearchCase::IgnoreCase))
	{
		double Val = 0.0;
		if (JsonVal.IsValid()) JsonVal->TryGetNumber(Val);
		OutLiteral = Sub.CreateFloatMetaSoundLiteral(static_cast<float>(Val), DummyDataType);
		return true;
	}
	if (DataType.Equals(TEXT("Int32"), ESearchCase::IgnoreCase))
	{
		double Val = 0.0;
		if (JsonVal.IsValid()) JsonVal->TryGetNumber(Val);
		OutLiteral = Sub.CreateIntMetaSoundLiteral(static_cast<int32>(Val), DummyDataType);
		return true;
	}
	if (DataType.Equals(TEXT("Bool"), ESearchCase::IgnoreCase) ||
		DataType.Equals(TEXT("Trigger"), ESearchCase::IgnoreCase))
	{
		bool Val = false;
		if (JsonVal.IsValid()) JsonVal->TryGetBool(Val);
		OutLiteral = Sub.CreateBoolMetaSoundLiteral(Val, DummyDataType);
		return true;
	}
	if (DataType.Equals(TEXT("String"), ESearchCase::IgnoreCase))
	{
		FString Val;
		if (JsonVal.IsValid()) JsonVal->TryGetString(Val);
		OutLiteral = Sub.CreateStringMetaSoundLiteral(Val, DummyDataType);
		return true;
	}
	if (DataType.Equals(TEXT("WaveAsset"), ESearchCase::IgnoreCase))
	{
		FString WavePath;
		if (JsonVal.IsValid()) JsonVal->TryGetString(WavePath);
		UObject* Wave = nullptr;
		if (!WavePath.IsEmpty())
		{
			Wave = StaticLoadObject(USoundWave::StaticClass(), nullptr, *WavePath);
			if (!Wave)
			{
				OutError = FString::Printf(TEXT("Could not load SoundWave at '%s'"), *WavePath);
				return false;
			}
		}
		// CreateObjectMetaSoundLiteral has NO DataType out param
		OutLiteral = Sub.CreateObjectMetaSoundLiteral(Wave);
		return true;
	}

	OutError = FString::Printf(TEXT("Unsupported data type '%s'. Valid: Float, Int32, Bool, String, Trigger, Time, WaveAsset"), *DataType);
	return false;
}

bool FMonolithAudioMetaSoundActions::ParseNodeClassName(
	const TArray<TSharedPtr<FJsonValue>>& ClassArray,
	FMetasoundFrontendClassName& OutClassName, FString& OutError)
{
	if (ClassArray.Num() < 2)
	{
		OutError = TEXT("node_class must be an array of at least [Namespace, Name] (optionally [Namespace, Name, Variant])");
		return false;
	}
	FString Namespace, Name, Variant;
	ClassArray[0]->TryGetString(Namespace);
	ClassArray[1]->TryGetString(Name);
	if (ClassArray.Num() >= 3)
	{
		ClassArray[2]->TryGetString(Variant);
	}
	OutClassName = FMetasoundFrontendClassName(FName(*Namespace), FName(*Name), FName(*Variant));
	return true;
}

UMetaSoundBuilderBase* FMonolithAudioMetaSoundActions::GetBuilderForAsset(const FString& AssetPath, FString& OutError)
{
	// Load the MetaSound asset
	IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
	UObject* Loaded = AssetData.IsValid() ? AssetData.GetAsset() : StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);

	if (!Loaded)
	{
		OutError = FString::Printf(TEXT("MetaSound asset not found at '%s'"), *AssetPath);
		return nullptr;
	}

	// Cast to IMetaSoundDocumentInterface via TScriptInterface
	TScriptInterface<IMetaSoundDocumentInterface> DocInterface;
	if (Loaded->GetClass()->ImplementsInterface(UMetaSoundDocumentInterface::StaticClass()))
	{
		DocInterface.SetObject(Loaded);
		DocInterface.SetInterface(Cast<IMetaSoundDocumentInterface>(Loaded));
	}

	if (!DocInterface.GetInterface())
	{
		OutError = FString::Printf(TEXT("Asset at '%s' does not implement IMetaSoundDocumentInterface"), *AssetPath);
		return nullptr;
	}

	UMetaSoundEditorSubsystem* EditorSub = GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>();
	if (!EditorSub)
	{
		OutError = TEXT("UMetaSoundEditorSubsystem not available");
		return nullptr;
	}

	EMetaSoundBuilderResult BuildResult;
	UMetaSoundBuilderBase* Builder = EditorSub->FindOrBeginBuilding(DocInterface, BuildResult);
	if (!Builder || BuildResult != EMetaSoundBuilderResult::Succeeded)
	{
		OutError = FString::Printf(TEXT("Failed to create builder for MetaSound at '%s'. Re-open the asset and try again."), *AssetPath);
		return nullptr;
	}

	return Builder;
}

bool FMonolithAudioMetaSoundActions::ResolveNodeHandle(
	UMetaSoundBuilderBase* Builder, const FString& NodeIdOrHandle,
	FMetaSoundNodeHandle& OutHandle, FString& OutError,
	const FString& AssetPath)
{
	// Try parsing as GUID first (format: 32 hex chars or standard GUID format)
	FGuid ParsedGuid;
	if (FGuid::Parse(NodeIdOrHandle, ParsedGuid))
	{
		OutHandle.NodeID = ParsedGuid;
		if (Builder->ContainsNode(OutHandle))
		{
			return true;
		}
	}

	// Not a valid GUID or node not found by GUID — try as graph input/output name
	EMetaSoundBuilderResult Result;

	// Try graph input
	FMetaSoundNodeHandle InputNode = Builder->FindGraphInputNode(FName(*NodeIdOrHandle), Result);
	if (Result == EMetaSoundBuilderResult::Succeeded && InputNode.IsSet())
	{
		OutHandle = InputNode;
		return true;
	}

	// Try graph output
	FMetaSoundNodeHandle OutputNode = Builder->FindGraphOutputNode(FName(*NodeIdOrHandle), Result);
	if (Result == EMetaSoundBuilderResult::Succeeded && OutputNode.IsSet())
	{
		OutHandle = OutputNode;
		return true;
	}

	// Phase F #3: try the user-label alias registry populated by add_metasound_node.
	if (!AssetPath.IsEmpty())
	{
		const FGuid AliasGuid = LookupNodeIdAlias(AssetPath, NodeIdOrHandle);
		if (AliasGuid.IsValid())
		{
			OutHandle.NodeID = AliasGuid;
			if (Builder->ContainsNode(OutHandle))
			{
				return true;
			}
			OutError = FString::Printf(
				TEXT("Node label '%s' was registered for '%s' but the underlying node no longer exists in the builder"),
				*NodeIdOrHandle, *AssetPath);
			return false;
		}
	}

	OutError = FString::Printf(TEXT("Could not resolve node '%s' — not a valid GUID, graph I/O name, or registered node_id alias"), *NodeIdOrHandle);
	return false;
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithAudioMetaSoundActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// ---- Core CRUD (12) ----

	Registry.RegisterAction(TEXT("audio"), TEXT("create_metasound_source"),
		TEXT("Create a new MetaSound Source asset with standard interfaces"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::CreateMetaSoundSource),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path (e.g. /Game/Audio/MS_Gunshot)"))
			.Optional(TEXT("format"), TEXT("string"), TEXT("Output format: Mono or Stereo"), TEXT("Mono"))
			.Optional(TEXT("one_shot"), TEXT("boolean"), TEXT("Whether this is a one-shot sound (adds OnFinished)"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("create_metasound_patch"),
		TEXT("Create a new MetaSound Patch asset (processing graph, no audio output)"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::CreateMetaSoundPatch),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path (e.g. /Game/Audio/MSP_Filter)"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("add_metasound_node"),
		TEXT("Add a node to an existing MetaSound by class name"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::AddMetaSoundNode),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Required(TEXT("node_class"), TEXT("array"), TEXT("Node class as [Namespace, Name, Variant], e.g. [\"UE\", \"Sine\", \"Audio\"]"))
			.Optional(TEXT("node_id"), TEXT("string"), TEXT("User-assigned label. When set, registers an alias so subsequent calls (remove/connect/find_inputs/etc.) can pass this label instead of the GUID."))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("remove_metasound_node"),
		TEXT("Remove a node from a MetaSound graph"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::RemoveMetaSoundNode),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Required(TEXT("node_id_or_handle"), TEXT("string"), TEXT("Node GUID or user label"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("connect_metasound_nodes"),
		TEXT("Connect two nodes in a MetaSound graph by name"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::ConnectMetaSoundNodes),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Required(TEXT("from_node"), TEXT("string"), TEXT("Source node GUID or graph input/output name"))
			.Required(TEXT("from_output"), TEXT("string"), TEXT("Output pin name on source node"))
			.Required(TEXT("to_node"), TEXT("string"), TEXT("Destination node GUID or graph input/output name"))
			.Required(TEXT("to_input"), TEXT("string"), TEXT("Input pin name on destination node"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("disconnect_metasound_nodes"),
		TEXT("Disconnect two nodes in a MetaSound graph"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::DisconnectMetaSoundNodes),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Required(TEXT("from_node"), TEXT("string"), TEXT("Source node GUID"))
			.Required(TEXT("from_output"), TEXT("string"), TEXT("Output pin name"))
			.Required(TEXT("to_node"), TEXT("string"), TEXT("Destination node GUID"))
			.Required(TEXT("to_input"), TEXT("string"), TEXT("Input pin name"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("add_metasound_input"),
		TEXT("Add a graph-level input to a MetaSound"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::AddMetaSoundInput),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Input name"))
			.Required(TEXT("data_type"), TEXT("string"), TEXT("Data type: Float, Int32, Bool, String, Trigger, Time, Audio, WaveAsset"))
			.Optional(TEXT("default_value"), TEXT("any"), TEXT("Default value for the input"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("add_metasound_output"),
		TEXT("Add a graph-level output to a MetaSound"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::AddMetaSoundOutput),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Output name"))
			.Required(TEXT("data_type"), TEXT("string"), TEXT("Data type: Float, Int32, Bool, String, Trigger, Time, Audio, WaveAsset"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("set_metasound_input_default"),
		TEXT("Set the default value of a graph input on a MetaSound"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::SetMetaSoundInputDefault),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Required(TEXT("input_name"), TEXT("string"), TEXT("Name of the graph input"))
			.Required(TEXT("value"), TEXT("any"), TEXT("New default value"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("add_metasound_interface"),
		TEXT("Add a standard interface to a MetaSound (e.g. UE.Source, UE.OutputFormat.Mono)"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::AddMetaSoundInterface),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Required(TEXT("interface_name"), TEXT("string"), TEXT("Interface name (e.g. UE.Source, UE.Source.OneShot, UE.OutputFormat.Mono, UE.OutputFormat.Stereo)"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("get_metasound_graph"),
		TEXT("Get the full graph of a MetaSound as JSON (nodes, edges, inputs, outputs, interfaces)"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::GetMetaSoundGraph),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("list_metasound_connections"),
		TEXT("List all connections in a MetaSound graph"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::ListMetaSoundConnections),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Build());

	// ---- Query & Discovery (5) ----

	Registry.RegisterAction(TEXT("audio"), TEXT("list_available_metasound_nodes"),
		TEXT("List all registered MetaSound node classes with their inputs/outputs"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::ListAvailableMetaSoundNodes),
		FParamSchemaBuilder()
			.Optional(TEXT("filter"), TEXT("string"), TEXT("Substring filter on node name"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Filter by namespace/category"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum results (default: 200)"), TEXT("200"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("get_metasound_node_info"),
		TEXT("Get detailed info about a MetaSound node class (inputs, outputs, types)"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::GetMetaSoundNodeInfo),
		FParamSchemaBuilder()
			.Required(TEXT("node_class"), TEXT("array"), TEXT("Node class as [Namespace, Name, Variant]"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("find_metasound_node_inputs"),
		TEXT("Get all input pins of a node in a MetaSound graph"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::FindMetaSoundNodeInputs),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Required(TEXT("node_id_or_handle"), TEXT("string"), TEXT("Node GUID or graph I/O name"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("find_metasound_node_outputs"),
		TEXT("Get all output pins of a node in a MetaSound graph"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::FindMetaSoundNodeOutputs),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Required(TEXT("node_id_or_handle"), TEXT("string"), TEXT("Node GUID or graph I/O name"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("get_metasound_input_names"),
		TEXT("Get all graph-level input names and types from a MetaSound"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::GetMetaSoundInputNames),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Build());

	// ---- Build & Templates (8) ----

	Registry.RegisterAction(TEXT("audio"), TEXT("build_metasound_from_spec"),
		TEXT("Build a complete MetaSound from a JSON spec (interfaces, inputs, nodes, connections) in one call"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::BuildMetaSoundFromSpec),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path for the new MetaSound"))
			.Required(TEXT("spec"), TEXT("object"), TEXT("Full MetaSound specification: {type, format, one_shot, interfaces, inputs, outputs, nodes, connections, graph_input_connections, interface_connections}"))
			.Optional(TEXT("strict_mode"), TEXT("boolean"), TEXT("If true, abort with error (no save) when any node/connection/input is skipped"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("create_metasound_preset"),
		TEXT("Create a MetaSound preset referencing an existing MetaSound"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::CreateMetaSoundPreset),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path for the new preset"))
			.Required(TEXT("reference_metasound"), TEXT("string"), TEXT("Asset path of the MetaSound to reference"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("create_oneshot_sfx"),
		TEXT("Create a simple one-shot MetaSound: WavePlayer -> Audio Output"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::CreateOneShotSfx),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path for the new MetaSound"))
			.Required(TEXT("sound_wave"), TEXT("string"), TEXT("Path to the SoundWave asset"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("create_looping_ambient_metasound"),
		TEXT("Create a looping ambient MetaSound with LFO modulation and random pitch"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::CreateLoopingAmbientMetaSound),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path for the new MetaSound"))
			.Required(TEXT("sound_wave"), TEXT("string"), TEXT("Path to the SoundWave asset"))
			.Optional(TEXT("lfo_frequency"), TEXT("number"), TEXT("LFO frequency in Hz (default: 0.25)"), TEXT("0.25"))
			.Optional(TEXT("pitch_range"), TEXT("array"), TEXT("Pitch range [min, max] (default: [0.95, 1.05])"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("create_synthesized_tone"),
		TEXT("Create a synthesized tone MetaSound: Oscillator -> Filter -> ADSR -> Output"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::CreateSynthesizedTone),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path for the new MetaSound"))
			.Optional(TEXT("oscillator_type"), TEXT("string"), TEXT("Oscillator type: Sine, Saw, or Square (default: Sine)"), TEXT("Sine"))
			.Optional(TEXT("frequency"), TEXT("number"), TEXT("Base frequency in Hz (default: 440.0)"), TEXT("440.0"))
			.Optional(TEXT("adsr"), TEXT("object"), TEXT("ADSR envelope: {attack, decay, sustain, release} in seconds/level"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("create_interactive_metasound"),
		TEXT("Create a parameter-driven interactive MetaSound with crossfade between sound waves"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::CreateInteractiveMetaSound),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path for the new MetaSound"))
			.Required(TEXT("sound_waves"), TEXT("array"), TEXT("Array of SoundWave asset paths"))
			.Optional(TEXT("parameter_name"), TEXT("string"), TEXT("Name of the blend parameter (default: BlendAmount)"), TEXT("BlendAmount"))
			.Optional(TEXT("blend_type"), TEXT("string"), TEXT("Blend type: Crossfade or Switch (default: Crossfade)"), TEXT("Crossfade"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("add_metasound_variable"),
		TEXT("Add a graph variable to a MetaSound with optional Get/Set nodes"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::AddMetaSoundVariable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Variable name"))
			.Required(TEXT("data_type"), TEXT("string"), TEXT("Data type: Float, Int32, Bool, String"))
			.Optional(TEXT("default_value"), TEXT("any"), TEXT("Default value"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("set_metasound_node_location"),
		TEXT("Set the visual position of a node in the MetaSound editor graph"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioMetaSoundActions::SetMetaSoundNodeLocation),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Required(TEXT("node_id_or_handle"), TEXT("string"), TEXT("Node GUID or graph I/O name"))
			.Required(TEXT("x"), TEXT("number"), TEXT("X position"))
			.Required(TEXT("y"), TEXT("number"), TEXT("Y position"))
			.Build());
}

// ============================================================================
// Core CRUD (12 actions)
// ============================================================================

FMonolithActionResult FMonolithAudioMetaSoundActions::CreateMetaSoundSource(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString PackagePath, AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		return FMonolithActionResult::Error(TEXT("Invalid asset_path — must contain at least one '/'"));
	}

	const FString FormatStr = Params->GetStringField(TEXT("format"));
	EMetaSoundOutputAudioFormat Format = EMetaSoundOutputAudioFormat::Mono;
	if (FormatStr.Equals(TEXT("Stereo"), ESearchCase::IgnoreCase))
	{
		Format = EMetaSoundOutputAudioFormat::Stereo;
	}

	bool bOneShot = true;
	if (Params->HasField(TEXT("one_shot")))
	{
		bOneShot = Params->GetBoolField(TEXT("one_shot"));
	}

	UMetaSoundBuilderSubsystem& Sub = UMetaSoundBuilderSubsystem::GetChecked();

	FMetaSoundBuilderNodeOutputHandle OnPlayOutput;
	FMetaSoundBuilderNodeInputHandle OnFinishedInput;
	TArray<FMetaSoundBuilderNodeInputHandle> AudioOutInputs;
	EMetaSoundBuilderResult BuildResult;

	FName BuilderName = FName(*FString::Printf(TEXT("Monolith_%s"), *AssetName));

	UMetaSoundSourceBuilder* Builder = Sub.CreateSourceBuilder(
		BuilderName, OnPlayOutput, OnFinishedInput, AudioOutInputs,
		BuildResult, Format, bOneShot);

	if (!Builder || BuildResult != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create MetaSound source builder"));
	}

	// Auto-layout nodes before persisting
	Builder->InitNodeLocations();

	// Build to asset
	UMetaSoundEditorSubsystem* EditorSub = GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>();
	if (!EditorSub)
	{
		return FMonolithActionResult::Error(TEXT("UMetaSoundEditorSubsystem not available"));
	}

	EMetaSoundBuilderResult SaveResult;
	TScriptInterface<IMetaSoundDocumentInterface> NewAsset = EditorSub->BuildToAsset(
		Builder, TEXT("Monolith"), AssetName, PackagePath, SaveResult, nullptr);

	if (SaveResult != EMetaSoundBuilderResult::Succeeded || !NewAsset.GetObject())
	{
		return FMonolithActionResult::Error(TEXT("BuildToAsset failed for MetaSound source"));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), NewAsset.GetObject()->GetPathName());
	Result->SetStringField(TEXT("format"), FormatStr.IsEmpty() ? TEXT("Mono") : FormatStr);
	Result->SetBoolField(TEXT("one_shot"), bOneShot);
	Result->SetNumberField(TEXT("audio_outputs"), AudioOutInputs.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::CreateMetaSoundPatch(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString PackagePath, AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		return FMonolithActionResult::Error(TEXT("Invalid asset_path"));
	}

	UMetaSoundBuilderSubsystem& Sub = UMetaSoundBuilderSubsystem::GetChecked();
	EMetaSoundBuilderResult BuildResult;

	FName BuilderName = FName(*FString::Printf(TEXT("Monolith_%s"), *AssetName));
	UMetaSoundPatchBuilder* Builder = Sub.CreatePatchBuilder(BuilderName, BuildResult);

	if (!Builder || BuildResult != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create MetaSound patch builder"));
	}

	Builder->InitNodeLocations();

	UMetaSoundEditorSubsystem* EditorSub = GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>();
	if (!EditorSub)
	{
		return FMonolithActionResult::Error(TEXT("UMetaSoundEditorSubsystem not available"));
	}

	EMetaSoundBuilderResult SaveResult;
	TScriptInterface<IMetaSoundDocumentInterface> NewAsset = EditorSub->BuildToAsset(
		Builder, TEXT("Monolith"), AssetName, PackagePath, SaveResult, nullptr);

	if (SaveResult != EMetaSoundBuilderResult::Succeeded || !NewAsset.GetObject())
	{
		return FMonolithActionResult::Error(TEXT("BuildToAsset failed for MetaSound patch"));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), NewAsset.GetObject()->GetPathName());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::AddMetaSoundNode(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	const TArray<TSharedPtr<FJsonValue>>* ClassArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("node_class"), ClassArray) || !ClassArray)
	{
		return FMonolithActionResult::Error(TEXT("node_class is required (array of [Namespace, Name, Variant])"));
	}

	FMetasoundFrontendClassName ClassName;
	FString ParseError;
	if (!ParseNodeClassName(*ClassArray, ClassName, ParseError))
	{
		return FMonolithActionResult::Error(ParseError);
	}

	FString Error;
	UMetaSoundBuilderBase* Builder = GetBuilderForAsset(AssetPath, Error);
	if (!Builder)
	{
		return FMonolithActionResult::Error(Error);
	}

	EMetaSoundBuilderResult Result;
	FMetaSoundNodeHandle NodeHandle = Builder->AddNodeByClassName(ClassName, Result, 1);

	if (Result != EMetaSoundBuilderResult::Succeeded || !NodeHandle.IsSet())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to add node [%s, %s, %s]"),
			*ClassName.Namespace.ToString(), *ClassName.Name.ToString(), *ClassName.Variant.ToString()));
	}

	// Get inputs and outputs for the new node
	EMetaSoundBuilderResult PinResult;
	TArray<FMetaSoundBuilderNodeInputHandle> Inputs = Builder->FindNodeInputs(NodeHandle, PinResult);
	TArray<FMetaSoundBuilderNodeOutputHandle> Outputs = Builder->FindNodeOutputs(NodeHandle, PinResult);

	TArray<TSharedPtr<FJsonValue>> InputsJson;
	for (const auto& Input : Inputs)
	{
		auto PinJson = MakeShared<FJsonObject>();
		PinJson->SetStringField(TEXT("vertex_id"), Input.VertexID.ToString());
		InputsJson.Add(MakeShared<FJsonValueObject>(PinJson));
	}

	TArray<TSharedPtr<FJsonValue>> OutputsJson;
	for (const auto& Output : Outputs)
	{
		auto PinJson = MakeShared<FJsonObject>();
		PinJson->SetStringField(TEXT("vertex_id"), Output.VertexID.ToString());
		OutputsJson.Add(MakeShared<FJsonValueObject>(PinJson));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("node_handle"), NodeHandle.NodeID.ToString());
	ResultJson->SetArrayField(TEXT("inputs"), InputsJson);
	ResultJson->SetArrayField(TEXT("outputs"), OutputsJson);

	if (Params->HasField(TEXT("node_id")))
	{
		const FString UserLabel = Params->GetStringField(TEXT("node_id"));
		ResultJson->SetStringField(TEXT("node_id"), UserLabel);

		// Phase F #3: register the user label so subsequent calls (remove/connect/find_inputs/etc.)
		// can pass `node_id_or_handle = <label>` and have it resolve to this NodeID.
		RegisterNodeIdAlias(AssetPath, UserLabel, NodeHandle.NodeID);
		ResultJson->SetBoolField(TEXT("node_id_alias_registered"), true);
	}

	// Mark dirty
	if (UObject* Obj = Builder->GetOuter())
	{
		if (UPackage* Pkg = Obj->GetOutermost())
		{
			Pkg->MarkPackageDirty();
		}
	}

	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::RemoveMetaSoundNode(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString NodeIdStr = Params->GetStringField(TEXT("node_id_or_handle"));
	if (AssetPath.IsEmpty() || NodeIdStr.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and node_id_or_handle are required"));
	}

	FString Error;
	UMetaSoundBuilderBase* Builder = GetBuilderForAsset(AssetPath, Error);
	if (!Builder)
	{
		return FMonolithActionResult::Error(Error);
	}

	FMetaSoundNodeHandle NodeHandle;
	if (!ResolveNodeHandle(Builder, NodeIdStr, NodeHandle, Error, AssetPath))
	{
		return FMonolithActionResult::Error(Error);
	}

	EMetaSoundBuilderResult Result;
	Builder->RemoveNode(NodeHandle, Result, /*bRemoveUnusedDependencies=*/ true);

	if (Result != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to remove node '%s'"), *NodeIdStr));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetBoolField(TEXT("success"), true);
	ResultJson->SetStringField(TEXT("removed_node"), NodeIdStr);
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::ConnectMetaSoundNodes(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString FromNode = Params->GetStringField(TEXT("from_node"));
	const FString FromOutput = Params->GetStringField(TEXT("from_output"));
	const FString ToNode = Params->GetStringField(TEXT("to_node"));
	const FString ToInput = Params->GetStringField(TEXT("to_input"));

	if (AssetPath.IsEmpty() || FromNode.IsEmpty() || FromOutput.IsEmpty() || ToNode.IsEmpty() || ToInput.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path, from_node, from_output, to_node, and to_input are all required"));
	}

	FString Error;
	UMetaSoundBuilderBase* Builder = GetBuilderForAsset(AssetPath, Error);
	if (!Builder)
	{
		return FMonolithActionResult::Error(Error);
	}

	FMetaSoundNodeHandle SourceHandle, DestHandle;
	if (!ResolveNodeHandle(Builder, FromNode, SourceHandle, Error, AssetPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("from_node: %s"), *Error));
	}
	if (!ResolveNodeHandle(Builder, ToNode, DestHandle, Error, AssetPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("to_node: %s"), *Error));
	}

	// Use name-based ConnectNodes for ergonomics
	EMetaSoundBuilderResult Result;
	Builder->ConnectNodes(SourceHandle, FName(*FromOutput), DestHandle, FName(*ToInput), Result);

	if (Result != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to connect %s.%s -> %s.%s"), *FromNode, *FromOutput, *ToNode, *ToInput));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::DisconnectMetaSoundNodes(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString FromNode = Params->GetStringField(TEXT("from_node"));
	const FString FromOutput = Params->GetStringField(TEXT("from_output"));
	const FString ToNode = Params->GetStringField(TEXT("to_node"));
	const FString ToInput = Params->GetStringField(TEXT("to_input"));

	if (AssetPath.IsEmpty() || FromNode.IsEmpty() || FromOutput.IsEmpty() || ToNode.IsEmpty() || ToInput.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path, from_node, from_output, to_node, and to_input are all required"));
	}

	FString Error;
	UMetaSoundBuilderBase* Builder = GetBuilderForAsset(AssetPath, Error);
	if (!Builder)
	{
		return FMonolithActionResult::Error(Error);
	}

	FMetaSoundNodeHandle SourceHandle, DestHandle;
	if (!ResolveNodeHandle(Builder, FromNode, SourceHandle, Error, AssetPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("from_node: %s"), *Error));
	}
	if (!ResolveNodeHandle(Builder, ToNode, DestHandle, Error, AssetPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("to_node: %s"), *Error));
	}

	// Resolve handles for disconnect (needs handle-based overload)
	EMetaSoundBuilderResult Result;
	FMetaSoundBuilderNodeOutputHandle OutputHandle = Builder->FindNodeOutputByName(SourceHandle, FName(*FromOutput), Result);
	if (Result != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Output pin '%s' not found on node '%s'"), *FromOutput, *FromNode));
	}

	FMetaSoundBuilderNodeInputHandle InputHandle = Builder->FindNodeInputByName(DestHandle, FName(*ToInput), Result);
	if (Result != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Input pin '%s' not found on node '%s'"), *ToInput, *ToNode));
	}

	Builder->DisconnectNodes(OutputHandle, InputHandle, Result);

	if (Result != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to disconnect %s.%s -> %s.%s"), *FromNode, *FromOutput, *ToNode, *ToInput));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::AddMetaSoundInput(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString Name = Params->GetStringField(TEXT("name"));
	const FString DataType = Params->GetStringField(TEXT("data_type"));

	if (AssetPath.IsEmpty() || Name.IsEmpty() || DataType.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path, name, and data_type are required"));
	}

	FString Error;
	UMetaSoundBuilderBase* Builder = GetBuilderForAsset(AssetPath, Error);
	if (!Builder)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Create default literal
	FMetasoundFrontendLiteral DefaultLiteral;
	TSharedPtr<FJsonValue> DefaultJsonVal;
	if (Params->HasField(TEXT("default_value")))
	{
		DefaultJsonVal = Params->TryGetField(TEXT("default_value"));
	}

	if (!CreateLiteralFromJson(DefaultJsonVal, DataType, DefaultLiteral, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	EMetaSoundBuilderResult Result;
	// AddGraphInputNode returns output handle (counterintuitive!)
	FMetaSoundBuilderNodeOutputHandle OutputHandle = Builder->AddGraphInputNode(
		FName(*Name), FName(*DataType), DefaultLiteral, Result, /*bIsConstructorInput=*/ false);

	if (Result != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to add graph input '%s' of type '%s'"), *Name, *DataType));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("name"), Name);
	ResultJson->SetStringField(TEXT("data_type"), DataType);
	ResultJson->SetStringField(TEXT("output_handle"), OutputHandle.VertexID.ToString());
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::AddMetaSoundOutput(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString Name = Params->GetStringField(TEXT("name"));
	const FString DataType = Params->GetStringField(TEXT("data_type"));

	if (AssetPath.IsEmpty() || Name.IsEmpty() || DataType.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path, name, and data_type are required"));
	}

	FString Error;
	UMetaSoundBuilderBase* Builder = GetBuilderForAsset(AssetPath, Error);
	if (!Builder)
	{
		return FMonolithActionResult::Error(Error);
	}

	FMetasoundFrontendLiteral DefaultLiteral;
	// For outputs, just create an empty literal of the right type
	if (!CreateLiteralFromJson(nullptr, DataType, DefaultLiteral, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	EMetaSoundBuilderResult Result;
	// AddGraphOutputNode returns input handle (counterintuitive!)
	FMetaSoundBuilderNodeInputHandle InputHandle = Builder->AddGraphOutputNode(
		FName(*Name), FName(*DataType), DefaultLiteral, Result, /*bIsConstructorOutput=*/ false);

	if (Result != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to add graph output '%s' of type '%s'"), *Name, *DataType));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("name"), Name);
	ResultJson->SetStringField(TEXT("data_type"), DataType);
	ResultJson->SetStringField(TEXT("input_handle"), InputHandle.VertexID.ToString());
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::SetMetaSoundInputDefault(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString InputName = Params->GetStringField(TEXT("input_name"));

	if (AssetPath.IsEmpty() || InputName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and input_name are required"));
	}

	FString Error;
	UMetaSoundBuilderBase* Builder = GetBuilderForAsset(AssetPath, Error);
	if (!Builder)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Find the graph input to determine its data type
	EMetaSoundBuilderResult Result;
	FName OutDataType;
	FMetaSoundBuilderNodeOutputHandle NodeOutputHandle;
	FMetaSoundNodeHandle InputNode = Builder->FindGraphInputNode(
		FName(*InputName), OutDataType, NodeOutputHandle, Result);

	if (Result != EMetaSoundBuilderResult::Succeeded || !InputNode.IsSet())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Graph input '%s' not found"), *InputName));
	}

	// Create literal from the value field using the discovered data type
	FMetasoundFrontendLiteral Literal;
	TSharedPtr<FJsonValue> ValueField = Params->TryGetField(TEXT("value"));
	if (!ValueField.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("value is required"));
	}

	if (!CreateLiteralFromJson(ValueField, OutDataType.ToString(), Literal, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	// Find the input handle on the input node, then set its default
	TArray<FMetaSoundBuilderNodeInputHandle> InputHandles = Builder->FindNodeInputs(InputNode, Result);
	if (InputHandles.Num() > 0)
	{
		Builder->SetNodeInputDefault(InputHandles[0], Literal, Result);
		if (Result != EMetaSoundBuilderResult::Succeeded)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to set default for input '%s'"), *InputName));
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetBoolField(TEXT("success"), true);
	ResultJson->SetStringField(TEXT("input_name"), InputName);
	ResultJson->SetStringField(TEXT("data_type"), OutDataType.ToString());
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::AddMetaSoundInterface(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString InterfaceName = Params->GetStringField(TEXT("interface_name"));

	if (AssetPath.IsEmpty() || InterfaceName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and interface_name are required"));
	}

	FString Error;
	UMetaSoundBuilderBase* Builder = GetBuilderForAsset(AssetPath, Error);
	if (!Builder)
	{
		return FMonolithActionResult::Error(Error);
	}

	EMetaSoundBuilderResult Result;
	Builder->AddInterface(FName(*InterfaceName), Result);

	if (Result != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to add interface '%s'"), *InterfaceName));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetBoolField(TEXT("success"), true);
	ResultJson->SetStringField(TEXT("interface"), InterfaceName);
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::GetMetaSoundGraph(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	// Load asset and access document via IMetaSoundDocumentInterface
	IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
	UObject* Loaded = AssetData.IsValid() ? AssetData.GetAsset() : StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (!Loaded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("MetaSound asset not found at '%s'"), *AssetPath));
	}

	IMetaSoundDocumentInterface* DocInterface = Cast<IMetaSoundDocumentInterface>(Loaded);
	if (!DocInterface)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset at '%s' does not implement IMetaSoundDocumentInterface"), *AssetPath));
	}

	// Try to get the builder's document first — it contains in-memory mutations
	// that haven't been persisted back to the asset document yet.
	const FMetasoundFrontendDocument* DocPtr = &DocInterface->GetConstDocument();
	FString BuilderError;
	UMetaSoundBuilderBase* Builder = GetBuilderForAsset(AssetPath, BuilderError);
	if (Builder)
	{
		// UMetaSoundBuilderBase does NOT implement IMetaSoundDocumentInterface.
		// Access its document via GetConstBuilder().GetConstDocumentChecked().
		DocPtr = &Builder->GetConstBuilder().GetConstDocumentChecked();
	}
	const FMetasoundFrontendDocument& Doc = *DocPtr;

	// UE 5.5+: Nodes live in PagedGraphs, not the deprecated RootGraph.Graph field.
	// Use GetConstDefaultGraph() to access the default page's graph.
	const FMetasoundFrontendGraph& DefaultGraph = Doc.RootGraph.GetConstDefaultGraph();

	// Nodes
	TArray<TSharedPtr<FJsonValue>> NodesJson;
	for (const FMetasoundFrontendNode& Node : DefaultGraph.Nodes)
	{
		auto NodeJson = MakeShared<FJsonObject>();
		NodeJson->SetStringField(TEXT("node_id"), Node.GetID().ToString());
		NodeJson->SetStringField(TEXT("class_id"), Node.ClassID.ToString());
		NodeJson->SetStringField(TEXT("name"), Node.Name.ToString());

		// Node inputs
		TArray<TSharedPtr<FJsonValue>> NodeInputsJson;
		for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Inputs)
		{
			auto VertexJson = MakeShared<FJsonObject>();
			VertexJson->SetStringField(TEXT("name"), Vertex.Name.ToString());
			VertexJson->SetStringField(TEXT("type"), Vertex.TypeName.ToString());
			VertexJson->SetStringField(TEXT("vertex_id"), Vertex.VertexID.ToString());
			NodeInputsJson.Add(MakeShared<FJsonValueObject>(VertexJson));
		}
		NodeJson->SetArrayField(TEXT("inputs"), NodeInputsJson);

		// Node outputs
		TArray<TSharedPtr<FJsonValue>> NodeOutputsJson;
		for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Outputs)
		{
			auto VertexJson = MakeShared<FJsonObject>();
			VertexJson->SetStringField(TEXT("name"), Vertex.Name.ToString());
			VertexJson->SetStringField(TEXT("type"), Vertex.TypeName.ToString());
			VertexJson->SetStringField(TEXT("vertex_id"), Vertex.VertexID.ToString());
			NodeOutputsJson.Add(MakeShared<FJsonValueObject>(VertexJson));
		}
		NodeJson->SetArrayField(TEXT("outputs"), NodeOutputsJson);

		NodesJson.Add(MakeShared<FJsonValueObject>(NodeJson));
	}

	// Edges
	TArray<TSharedPtr<FJsonValue>> EdgesJson;
	for (const FMetasoundFrontendEdge& Edge : DefaultGraph.Edges)
	{
		auto EdgeJson = MakeShared<FJsonObject>();
		EdgeJson->SetStringField(TEXT("from_node"), Edge.FromNodeID.ToString());
		EdgeJson->SetStringField(TEXT("from_vertex"), Edge.FromVertexID.ToString());
		EdgeJson->SetStringField(TEXT("to_node"), Edge.ToNodeID.ToString());
		EdgeJson->SetStringField(TEXT("to_vertex"), Edge.ToVertexID.ToString());
		EdgesJson.Add(MakeShared<FJsonValueObject>(EdgeJson));
	}

	// Graph inputs
	TArray<TSharedPtr<FJsonValue>> InputsJson;
	for (const FMetasoundFrontendClassInput& Input : Doc.RootGraph.Interface.Inputs)
	{
		auto InputJson = MakeShared<FJsonObject>();
		InputJson->SetStringField(TEXT("name"), Input.Name.ToString());
		InputJson->SetStringField(TEXT("type"), Input.TypeName.ToString());
		InputJson->SetStringField(TEXT("vertex_id"), Input.VertexID.ToString());
		InputsJson.Add(MakeShared<FJsonValueObject>(InputJson));
	}

	// Graph outputs
	TArray<TSharedPtr<FJsonValue>> OutputsJson;
	for (const FMetasoundFrontendClassOutput& Output : Doc.RootGraph.Interface.Outputs)
	{
		auto OutputJson = MakeShared<FJsonObject>();
		OutputJson->SetStringField(TEXT("name"), Output.Name.ToString());
		OutputJson->SetStringField(TEXT("type"), Output.TypeName.ToString());
		OutputJson->SetStringField(TEXT("vertex_id"), Output.VertexID.ToString());
		OutputsJson.Add(MakeShared<FJsonValueObject>(OutputJson));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetArrayField(TEXT("nodes"), NodesJson);
	ResultJson->SetArrayField(TEXT("edges"), EdgesJson);
	ResultJson->SetArrayField(TEXT("inputs"), InputsJson);
	ResultJson->SetArrayField(TEXT("outputs"), OutputsJson);
	ResultJson->SetNumberField(TEXT("node_count"), DefaultGraph.Nodes.Num());
	ResultJson->SetNumberField(TEXT("edge_count"), DefaultGraph.Edges.Num());

	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::ListMetaSoundConnections(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
	UObject* Loaded = AssetData.IsValid() ? AssetData.GetAsset() : StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (!Loaded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("MetaSound asset not found at '%s'"), *AssetPath));
	}

	IMetaSoundDocumentInterface* DocInterface = Cast<IMetaSoundDocumentInterface>(Loaded);
	if (!DocInterface)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset at '%s' does not implement IMetaSoundDocumentInterface"), *AssetPath));
	}

	// Prefer builder's document if available (contains in-memory mutations)
	const FMetasoundFrontendDocument* DocPtr = &DocInterface->GetConstDocument();
	FString BuilderError;
	UMetaSoundBuilderBase* Builder = GetBuilderForAsset(AssetPath, BuilderError);
	if (Builder)
	{
		DocPtr = &Builder->GetConstBuilder().GetConstDocumentChecked();
	}
	const FMetasoundFrontendDocument& Doc = *DocPtr;

	// UE 5.5+: Edges live in PagedGraphs, not the deprecated RootGraph.Graph field.
	const FMetasoundFrontendGraph& DefaultGraph = Doc.RootGraph.GetConstDefaultGraph();

	TArray<TSharedPtr<FJsonValue>> ConnectionsJson;
	for (const FMetasoundFrontendEdge& Edge : DefaultGraph.Edges)
	{
		auto EdgeJson = MakeShared<FJsonObject>();
		EdgeJson->SetStringField(TEXT("from_node"), Edge.FromNodeID.ToString());
		EdgeJson->SetStringField(TEXT("from_vertex"), Edge.FromVertexID.ToString());
		EdgeJson->SetStringField(TEXT("to_node"), Edge.ToNodeID.ToString());
		EdgeJson->SetStringField(TEXT("to_vertex"), Edge.ToVertexID.ToString());
		ConnectionsJson.Add(MakeShared<FJsonValueObject>(EdgeJson));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetArrayField(TEXT("connections"), ConnectionsJson);
	ResultJson->SetNumberField(TEXT("count"), ConnectionsJson.Num());
	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Query & Discovery (5 actions)
// ============================================================================

FMonolithActionResult FMonolithAudioMetaSoundActions::ListAvailableMetaSoundNodes(const TSharedPtr<FJsonObject>& Params)
{
	const FString Filter = Params->GetStringField(TEXT("filter"));
	const FString Category = Params->GetStringField(TEXT("category"));
	int32 Limit = 200;
	if (Params->HasField(TEXT("limit")))
	{
		Limit = static_cast<int32>(Params->GetNumberField(TEXT("limit")));
	}

	Metasound::Frontend::INodeClassRegistry& Registry = Metasound::Frontend::INodeClassRegistry::GetChecked();

	TArray<TSharedPtr<FJsonValue>> NodesJson;
	int32 Count = 0;

	Registry.IterateRegistry(
		[&](const FMetasoundFrontendClass& InClass)
		{
			if (Count >= Limit) return;

			const FMetasoundFrontendClassName& ClassName = InClass.Metadata.GetClassName();
			const FString NameStr = ClassName.Name.ToString();
			const FString NamespaceStr = ClassName.Namespace.ToString();

			// Apply filters
			if (!Filter.IsEmpty() && !NameStr.Contains(Filter, ESearchCase::IgnoreCase) &&
				!NamespaceStr.Contains(Filter, ESearchCase::IgnoreCase))
			{
				return;
			}
			if (!Category.IsEmpty() && !NamespaceStr.Contains(Category, ESearchCase::IgnoreCase))
			{
				return;
			}

			auto NodeJson = MakeShared<FJsonObject>();
			NodeJson->SetStringField(TEXT("namespace"), NamespaceStr);
			NodeJson->SetStringField(TEXT("name"), NameStr);
			NodeJson->SetStringField(TEXT("variant"), ClassName.Variant.ToString());

			// Inputs
			TArray<TSharedPtr<FJsonValue>> InputsJson;
			for (const FMetasoundFrontendClassVertex& Vertex : InClass.Interface.Inputs)
			{
				auto VJson = MakeShared<FJsonObject>();
				VJson->SetStringField(TEXT("name"), Vertex.Name.ToString());
				VJson->SetStringField(TEXT("type"), Vertex.TypeName.ToString());
				InputsJson.Add(MakeShared<FJsonValueObject>(VJson));
			}
			NodeJson->SetArrayField(TEXT("inputs"), InputsJson);

			// Outputs
			TArray<TSharedPtr<FJsonValue>> OutputsJson;
			for (const FMetasoundFrontendClassVertex& Vertex : InClass.Interface.Outputs)
			{
				auto VJson = MakeShared<FJsonObject>();
				VJson->SetStringField(TEXT("name"), Vertex.Name.ToString());
				VJson->SetStringField(TEXT("type"), Vertex.TypeName.ToString());
				OutputsJson.Add(MakeShared<FJsonValueObject>(VJson));
			}
			NodeJson->SetArrayField(TEXT("outputs"), OutputsJson);

			NodesJson.Add(MakeShared<FJsonValueObject>(NodeJson));
			Count++;
		},
		EMetasoundFrontendClassType::Invalid // Invalid = iterate ALL types
	);

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetArrayField(TEXT("nodes"), NodesJson);
	ResultJson->SetNumberField(TEXT("count"), NodesJson.Num());
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::GetMetaSoundNodeInfo(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* ClassArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("node_class"), ClassArray) || !ClassArray)
	{
		return FMonolithActionResult::Error(TEXT("node_class is required (array of [Namespace, Name, Variant])"));
	}

	FMetasoundFrontendClassName ClassName;
	FString ParseError;
	if (!ParseNodeClassName(*ClassArray, ClassName, ParseError))
	{
		return FMonolithActionResult::Error(ParseError);
	}

	Metasound::Frontend::INodeClassRegistry& Registry = Metasound::Frontend::INodeClassRegistry::GetChecked();

	TSharedPtr<FJsonObject> ResultJson;
	bool bFound = false;

	Registry.IterateRegistry(
		[&](const FMetasoundFrontendClass& InClass)
		{
			if (bFound) return;

			const FMetasoundFrontendClassName& CN = InClass.Metadata.GetClassName();
			if (CN.Namespace == ClassName.Namespace && CN.Name == ClassName.Name &&
				(ClassName.Variant.IsNone() || CN.Variant == ClassName.Variant))
			{
				bFound = true;
				ResultJson = MakeShared<FJsonObject>();
				ResultJson->SetStringField(TEXT("namespace"), CN.Namespace.ToString());
				ResultJson->SetStringField(TEXT("name"), CN.Name.ToString());
				ResultJson->SetStringField(TEXT("variant"), CN.Variant.ToString());

				TArray<TSharedPtr<FJsonValue>> InputsJson;
				for (const FMetasoundFrontendClassVertex& V : InClass.Interface.Inputs)
				{
					auto VJson = MakeShared<FJsonObject>();
					VJson->SetStringField(TEXT("name"), V.Name.ToString());
					VJson->SetStringField(TEXT("type"), V.TypeName.ToString());
					InputsJson.Add(MakeShared<FJsonValueObject>(VJson));
				}
				ResultJson->SetArrayField(TEXT("inputs"), InputsJson);

				TArray<TSharedPtr<FJsonValue>> OutputsJson;
				for (const FMetasoundFrontendClassVertex& V : InClass.Interface.Outputs)
				{
					auto VJson = MakeShared<FJsonObject>();
					VJson->SetStringField(TEXT("name"), V.Name.ToString());
					VJson->SetStringField(TEXT("type"), V.TypeName.ToString());
					OutputsJson.Add(MakeShared<FJsonValueObject>(VJson));
				}
				ResultJson->SetArrayField(TEXT("outputs"), OutputsJson);
			}
		},
		EMetasoundFrontendClassType::Invalid
	);

	if (!bFound)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Node class [%s, %s, %s] not found in registry"),
			*ClassName.Namespace.ToString(), *ClassName.Name.ToString(), *ClassName.Variant.ToString()));
	}

	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::FindMetaSoundNodeInputs(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString NodeIdStr = Params->GetStringField(TEXT("node_id_or_handle"));

	if (AssetPath.IsEmpty() || NodeIdStr.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and node_id_or_handle are required"));
	}

	FString Error;
	UMetaSoundBuilderBase* Builder = GetBuilderForAsset(AssetPath, Error);
	if (!Builder)
	{
		return FMonolithActionResult::Error(Error);
	}

	FMetaSoundNodeHandle NodeHandle;
	if (!ResolveNodeHandle(Builder, NodeIdStr, NodeHandle, Error, AssetPath))
	{
		return FMonolithActionResult::Error(Error);
	}

	EMetaSoundBuilderResult Result;
	TArray<FMetaSoundBuilderNodeInputHandle> Inputs = Builder->FindNodeInputs(NodeHandle, Result);

	if (Result != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to find inputs for node '%s'"), *NodeIdStr));
	}

	// Look up the node in the document to get names and types
	// Access builder's document directly (contains in-memory mutations from add/remove/connect)
	const FMetasoundFrontendDocument& Doc = Builder->GetConstBuilder().GetConstDocumentChecked();
	// UE 5.5+: Nodes live in PagedGraphs, not the deprecated RootGraph.Graph field.
	const FMetasoundFrontendGraph& DefaultGraph = Doc.RootGraph.GetConstDefaultGraph();
	TMap<FGuid, const FMetasoundFrontendVertex*> VertexMap;
	for (const FMetasoundFrontendNode& Node : DefaultGraph.Nodes)
	{
		if (Node.GetID() == NodeHandle.NodeID)
		{
			for (const FMetasoundFrontendVertex& V : Node.Interface.Inputs)
			{
				VertexMap.Add(V.VertexID, &V);
			}
			break;
		}
	}

	TArray<TSharedPtr<FJsonValue>> InputsJson;
	for (const FMetaSoundBuilderNodeInputHandle& Input : Inputs)
	{
		auto InputJson = MakeShared<FJsonObject>();
		InputJson->SetStringField(TEXT("vertex_id"), Input.VertexID.ToString());

		if (const FMetasoundFrontendVertex** Found = VertexMap.Find(Input.VertexID))
		{
			InputJson->SetStringField(TEXT("name"), (*Found)->Name.ToString());
			InputJson->SetStringField(TEXT("type"), (*Found)->TypeName.ToString());
		}

		InputsJson.Add(MakeShared<FJsonValueObject>(InputJson));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("node"), NodeIdStr);
	ResultJson->SetArrayField(TEXT("inputs"), InputsJson);
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::FindMetaSoundNodeOutputs(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString NodeIdStr = Params->GetStringField(TEXT("node_id_or_handle"));

	if (AssetPath.IsEmpty() || NodeIdStr.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and node_id_or_handle are required"));
	}

	FString Error;
	UMetaSoundBuilderBase* Builder = GetBuilderForAsset(AssetPath, Error);
	if (!Builder)
	{
		return FMonolithActionResult::Error(Error);
	}

	FMetaSoundNodeHandle NodeHandle;
	if (!ResolveNodeHandle(Builder, NodeIdStr, NodeHandle, Error, AssetPath))
	{
		return FMonolithActionResult::Error(Error);
	}

	EMetaSoundBuilderResult Result;
	TArray<FMetaSoundBuilderNodeOutputHandle> Outputs = Builder->FindNodeOutputs(NodeHandle, Result);

	if (Result != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to find outputs for node '%s'"), *NodeIdStr));
	}

	// Access builder's document directly (contains in-memory mutations from add/remove/connect)
	const FMetasoundFrontendDocument& Doc = Builder->GetConstBuilder().GetConstDocumentChecked();
	// UE 5.5+: Nodes live in PagedGraphs, not the deprecated RootGraph.Graph field.
	const FMetasoundFrontendGraph& DefaultGraph = Doc.RootGraph.GetConstDefaultGraph();
	TMap<FGuid, const FMetasoundFrontendVertex*> VertexMap;
	for (const FMetasoundFrontendNode& Node : DefaultGraph.Nodes)
	{
		if (Node.GetID() == NodeHandle.NodeID)
		{
			for (const FMetasoundFrontendVertex& V : Node.Interface.Outputs)
			{
				VertexMap.Add(V.VertexID, &V);
			}
			break;
		}
	}

	TArray<TSharedPtr<FJsonValue>> OutputsJson;
	for (const FMetaSoundBuilderNodeOutputHandle& Output : Outputs)
	{
		auto OutputJson = MakeShared<FJsonObject>();
		OutputJson->SetStringField(TEXT("vertex_id"), Output.VertexID.ToString());

		if (const FMetasoundFrontendVertex** Found = VertexMap.Find(Output.VertexID))
		{
			OutputJson->SetStringField(TEXT("name"), (*Found)->Name.ToString());
			OutputJson->SetStringField(TEXT("type"), (*Found)->TypeName.ToString());
		}

		OutputsJson.Add(MakeShared<FJsonValueObject>(OutputJson));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("node"), NodeIdStr);
	ResultJson->SetArrayField(TEXT("outputs"), OutputsJson);
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::GetMetaSoundInputNames(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
	UObject* Loaded = AssetData.IsValid() ? AssetData.GetAsset() : StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (!Loaded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("MetaSound asset not found at '%s'"), *AssetPath));
	}

	IMetaSoundDocumentInterface* DocInterface = Cast<IMetaSoundDocumentInterface>(Loaded);
	if (!DocInterface)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset at '%s' does not implement IMetaSoundDocumentInterface"), *AssetPath));
	}

	// Prefer builder's document if available (contains in-memory mutations)
	const FMetasoundFrontendDocument* DocPtr = &DocInterface->GetConstDocument();
	FString BuilderError;
	UMetaSoundBuilderBase* Builder = GetBuilderForAsset(AssetPath, BuilderError);
	if (Builder)
	{
		DocPtr = &Builder->GetConstBuilder().GetConstDocumentChecked();
	}
	const FMetasoundFrontendDocument& Doc = *DocPtr;

	TArray<TSharedPtr<FJsonValue>> InputsJson;
	for (const FMetasoundFrontendClassInput& Input : Doc.RootGraph.Interface.Inputs)
	{
		auto InputJson = MakeShared<FJsonObject>();
		InputJson->SetStringField(TEXT("name"), Input.Name.ToString());
		InputJson->SetStringField(TEXT("type"), Input.TypeName.ToString());
		InputJson->SetStringField(TEXT("vertex_id"), Input.VertexID.ToString());
		InputsJson.Add(MakeShared<FJsonValueObject>(InputJson));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetArrayField(TEXT("inputs"), InputsJson);
	ResultJson->SetNumberField(TEXT("count"), InputsJson.Num());
	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Build & Templates (8 actions)
// ============================================================================

FMonolithActionResult FMonolithAudioMetaSoundActions::BuildMetaSoundFromSpec(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("spec"), SpecPtr) || !SpecPtr || !SpecPtr->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("spec is required (JSON object)"));
	}
	const TSharedPtr<FJsonObject>& Spec = *SpecPtr;

	FString PackagePath, AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		return FMonolithActionResult::Error(TEXT("Invalid asset_path"));
	}

	bool bStrictMode = false;
	Params->TryGetBoolField(TEXT("strict_mode"), bStrictMode);

	// =====================================================================
	// PRE-VALIDATION PASS — collect connection/declaration mismatches before
	// any builder mutation. In strict mode this aborts before save.
	// =====================================================================
	TArray<FString> PreValidationErrors;
	TSet<FString> DeclaredNodeIds;

	const TArray<TSharedPtr<FJsonValue>>* PreNodesArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("nodes"), PreNodesArray) && PreNodesArray)
	{
		for (const auto& NodeVal : *PreNodesArray)
		{
			const TSharedPtr<FJsonObject>& NodeObj = NodeVal->AsObject();
			if (!NodeObj.IsValid()) continue;
			const FString NodeId = NodeObj->GetStringField(TEXT("id"));
			if (NodeId.IsEmpty())
			{
				PreValidationErrors.Add(TEXT("Node missing 'id' field"));
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* ClassArr = nullptr;
			if (!NodeObj->TryGetArrayField(TEXT("class"), ClassArr) || !ClassArr)
			{
				PreValidationErrors.Add(FString::Printf(TEXT("Node '%s' missing 'class' array"), *NodeId));
				continue;
			}
			DeclaredNodeIds.Add(NodeId);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* PreConnsArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("connections"), PreConnsArray) && PreConnsArray)
	{
		for (const auto& ConnVal : *PreConnsArray)
		{
			const TSharedPtr<FJsonObject>& ConnObj = ConnVal->AsObject();
			if (!ConnObj.IsValid()) continue;
			const FString FromId = ConnObj->GetStringField(TEXT("from"));
			const FString ToId = ConnObj->GetStringField(TEXT("to"));
			if (!DeclaredNodeIds.Contains(FromId) || !DeclaredNodeIds.Contains(ToId))
			{
				PreValidationErrors.Add(FString::Printf(TEXT("Connection '%s' -> '%s' references undeclared node"), *FromId, *ToId));
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* PreGraphInConns = nullptr;
	if (Spec->TryGetArrayField(TEXT("graph_input_connections"), PreGraphInConns) && PreGraphInConns)
	{
		for (const auto& ConnVal : *PreGraphInConns)
		{
			const TSharedPtr<FJsonObject>& ConnObj = ConnVal->AsObject();
			if (!ConnObj.IsValid()) continue;
			const FString ToNodeId = ConnObj->GetStringField(TEXT("to_node"));
			if (!DeclaredNodeIds.Contains(ToNodeId))
			{
				PreValidationErrors.Add(FString::Printf(TEXT("graph_input_connection target node '%s' not declared"), *ToNodeId));
			}
		}
	}

	const TSharedPtr<FJsonObject>* PreInterfaceConns = nullptr;
	if (Spec->TryGetObjectField(TEXT("interface_connections"), PreInterfaceConns) && PreInterfaceConns && PreInterfaceConns->IsValid())
	{
		for (const auto& Pair : (*PreInterfaceConns)->Values)
		{
			const TSharedPtr<FJsonObject>& ConnObj = Pair.Value->AsObject();
			if (!ConnObj.IsValid()) continue;
			if (ConnObj->HasField(TEXT("to_node")))
			{
				const FString ToNodeId = ConnObj->GetStringField(TEXT("to_node"));
				if (!DeclaredNodeIds.Contains(ToNodeId))
				{
					PreValidationErrors.Add(FString::Printf(TEXT("interface_connection '%s' target node '%s' not declared"), *Pair.Key, *ToNodeId));
				}
			}
			if (ConnObj->HasField(TEXT("from_node")))
			{
				const FString FromNodeId = ConnObj->GetStringField(TEXT("from_node"));
				if (!DeclaredNodeIds.Contains(FromNodeId))
				{
					PreValidationErrors.Add(FString::Printf(TEXT("interface_connection '%s' source node '%s' not declared"), *Pair.Key, *FromNodeId));
				}
			}
		}
	}

	if (bStrictMode && PreValidationErrors.Num() > 0)
	{
		FString Msg = TEXT("strict_mode: spec pre-validation failed (asset NOT saved):");
		for (const FString& E : PreValidationErrors)
		{
			Msg += TEXT("\n  - ") + E;
		}
		return FMonolithActionResult::Error(Msg);
	}

	// Tracking arrays — populated during build, returned in skipped_* fields.
	TArray<TSharedPtr<FJsonValue>> SkippedNodes;
	TArray<TSharedPtr<FJsonValue>> SkippedInputs;
	TArray<TSharedPtr<FJsonValue>> SkippedOutputs;
	TArray<TSharedPtr<FJsonValue>> SkippedConnections;
	TArray<TSharedPtr<FJsonValue>> SkippedInterfaces;
	TArray<TSharedPtr<FJsonValue>> SkippedDefaults;

	auto MakeSkipObj = [](const FString& Identifier, const FString& Reason) -> TSharedPtr<FJsonValue>
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Identifier);
		Obj->SetStringField(TEXT("reason"), Reason);
		return MakeShared<FJsonValueObject>(Obj);
	};

	// 1. Create builder
	UMetaSoundBuilderSubsystem& Sub = UMetaSoundBuilderSubsystem::GetChecked();
	EMetaSoundBuilderResult BuildResult;
	FName BuilderName = FName(*FString::Printf(TEXT("Monolith_%s"), *AssetName));

	const FString TypeStr = Spec->GetStringField(TEXT("type"));
	bool bIsSource = !TypeStr.Equals(TEXT("Patch"), ESearchCase::IgnoreCase);

	UMetaSoundBuilderBase* Builder = nullptr;

	if (bIsSource)
	{
		const FString FormatStr = Spec->GetStringField(TEXT("format"));
		EMetaSoundOutputAudioFormat Format = EMetaSoundOutputAudioFormat::Mono;
		if (FormatStr.Equals(TEXT("Stereo"), ESearchCase::IgnoreCase))
		{
			Format = EMetaSoundOutputAudioFormat::Stereo;
		}

		bool bOneShot = true;
		if (Spec->HasField(TEXT("one_shot")))
		{
			bOneShot = Spec->GetBoolField(TEXT("one_shot"));
		}

		FMetaSoundBuilderNodeOutputHandle OnPlayOutput;
		FMetaSoundBuilderNodeInputHandle OnFinishedInput;
		TArray<FMetaSoundBuilderNodeInputHandle> AudioOutInputs;

		Builder = Sub.CreateSourceBuilder(
			BuilderName, OnPlayOutput, OnFinishedInput, AudioOutInputs,
			BuildResult, Format, bOneShot);
	}
	else
	{
		Builder = Sub.CreatePatchBuilder(BuilderName, BuildResult);
	}

	if (!Builder || BuildResult != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create MetaSound builder"));
	}

	// 2. Add interfaces
	const TArray<TSharedPtr<FJsonValue>>* InterfacesArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("interfaces"), InterfacesArray) && InterfacesArray)
	{
		for (const auto& InterfaceVal : *InterfacesArray)
		{
			FString InterfaceName;
			if (!InterfaceVal->TryGetString(InterfaceName) || InterfaceName.IsEmpty())
			{
				SkippedInterfaces.Add(MakeSkipObj(TEXT("<empty>"), TEXT("Interface name missing or not a string")));
				continue;
			}
			EMetaSoundBuilderResult InterfaceResult;
			Builder->AddInterface(FName(*InterfaceName), InterfaceResult);
			// Note: failures here often mean interface already present (e.g. from CreateSourceBuilder)
			// — not necessarily fatal. We do not log these as skipped to avoid noise.
		}
	}

	// 3. Add graph inputs
	const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("inputs"), InputsArray) && InputsArray)
	{
		for (const auto& InputVal : *InputsArray)
		{
			const TSharedPtr<FJsonObject>& InputObj = InputVal->AsObject();
			if (!InputObj.IsValid())
			{
				SkippedInputs.Add(MakeSkipObj(TEXT("<invalid>"), TEXT("Input entry is not a JSON object")));
				continue;
			}

			const FString InputName = InputObj->GetStringField(TEXT("name"));
			const FString InputType = InputObj->GetStringField(TEXT("type"));
			if (InputName.IsEmpty() || InputType.IsEmpty())
			{
				SkippedInputs.Add(MakeSkipObj(InputName.IsEmpty() ? TEXT("<unnamed>") : InputName, TEXT("Input missing 'name' or 'type'")));
				continue;
			}

			FMetasoundFrontendLiteral DefaultLiteral;
			FString LitError;
			TSharedPtr<FJsonValue> DefaultVal = InputObj->TryGetField(TEXT("default"));
			CreateLiteralFromJson(DefaultVal, InputType, DefaultLiteral, LitError);

			EMetaSoundBuilderResult InputResult;
			Builder->AddGraphInputNode(FName(*InputName), FName(*InputType), DefaultLiteral, InputResult);
			if (InputResult != EMetaSoundBuilderResult::Succeeded)
			{
				SkippedInputs.Add(MakeSkipObj(InputName, FString::Printf(TEXT("AddGraphInputNode failed (type='%s')"), *InputType)));
			}
		}
	}

	// 4. Add graph outputs (non-interface)
	const TArray<TSharedPtr<FJsonValue>>* OutputsArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("outputs"), OutputsArray) && OutputsArray)
	{
		for (const auto& OutputVal : *OutputsArray)
		{
			const TSharedPtr<FJsonObject>& OutputObj = OutputVal->AsObject();
			if (!OutputObj.IsValid())
			{
				SkippedOutputs.Add(MakeSkipObj(TEXT("<invalid>"), TEXT("Output entry is not a JSON object")));
				continue;
			}

			const FString OutputName = OutputObj->GetStringField(TEXT("name"));
			const FString OutputType = OutputObj->GetStringField(TEXT("type"));
			if (OutputName.IsEmpty() || OutputType.IsEmpty())
			{
				SkippedOutputs.Add(MakeSkipObj(OutputName.IsEmpty() ? TEXT("<unnamed>") : OutputName, TEXT("Output missing 'name' or 'type'")));
				continue;
			}

			FMetasoundFrontendLiteral DefaultLiteral;
			FString LitError;
			CreateLiteralFromJson(nullptr, OutputType, DefaultLiteral, LitError);

			EMetaSoundBuilderResult OutputResult;
			Builder->AddGraphOutputNode(FName(*OutputName), FName(*OutputType), DefaultLiteral, OutputResult);
			if (OutputResult != EMetaSoundBuilderResult::Succeeded)
			{
				SkippedOutputs.Add(MakeSkipObj(OutputName, FString::Printf(TEXT("AddGraphOutputNode failed (type='%s')"), *OutputType)));
			}
		}
	}

	// 5. Add all nodes — map string IDs to FMetaSoundNodeHandle
	TMap<FString, FMetaSoundNodeHandle> NodeMap;
	int32 NodeCount = 0;

	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("nodes"), NodesArray) && NodesArray)
	{
		for (const auto& NodeVal : *NodesArray)
		{
			const TSharedPtr<FJsonObject>& NodeObj = NodeVal->AsObject();
			if (!NodeObj.IsValid())
			{
				SkippedNodes.Add(MakeSkipObj(TEXT("<invalid>"), TEXT("Node entry is not a JSON object")));
				continue;
			}

			const FString NodeId = NodeObj->GetStringField(TEXT("id"));
			const TArray<TSharedPtr<FJsonValue>>* ClassArr = nullptr;
			if (!NodeObj->TryGetArrayField(TEXT("class"), ClassArr) || !ClassArr)
			{
				SkippedNodes.Add(MakeSkipObj(NodeId.IsEmpty() ? TEXT("<unnamed>") : NodeId, TEXT("Node missing 'class' array")));
				continue;
			}

			FMetasoundFrontendClassName ClassName;
			FString ParseError;
			if (!ParseNodeClassName(*ClassArr, ClassName, ParseError))
			{
				UE_LOG(LogMonolith, Warning, TEXT("build_metasound_from_spec: Skipping node '%s': %s"), *NodeId, *ParseError);
				SkippedNodes.Add(MakeSkipObj(NodeId, FString::Printf(TEXT("Class parse error: %s"), *ParseError)));
				continue;
			}

			EMetaSoundBuilderResult NodeResult;
			FMetaSoundNodeHandle Handle = Builder->AddNodeByClassName(ClassName, NodeResult, 1);

			if (NodeResult != EMetaSoundBuilderResult::Succeeded || !Handle.IsSet())
			{
				UE_LOG(LogMonolith, Warning, TEXT("build_metasound_from_spec: Failed to add node '%s' [%s, %s, %s]"),
					*NodeId, *ClassName.Namespace.ToString(), *ClassName.Name.ToString(), *ClassName.Variant.ToString());
				SkippedNodes.Add(MakeSkipObj(NodeId, FString::Printf(TEXT("AddNodeByClassName failed [%s.%s.%s]"),
					*ClassName.Namespace.ToString(), *ClassName.Name.ToString(), *ClassName.Variant.ToString())));
				continue;
			}

			NodeMap.Add(NodeId, Handle);
			NodeCount++;

			// 5b. Set node input defaults
			const TSharedPtr<FJsonObject>* DefaultsPtr = nullptr;
			if (NodeObj->TryGetObjectField(TEXT("defaults"), DefaultsPtr) && DefaultsPtr && DefaultsPtr->IsValid())
			{
				for (const auto& DefaultPair : (*DefaultsPtr)->Values)
				{
					EMetaSoundBuilderResult FindResult;
					FMetaSoundBuilderNodeInputHandle InputHandle = Builder->FindNodeInputByName(
						Handle, FName(*DefaultPair.Key), FindResult);

					if (FindResult != EMetaSoundBuilderResult::Succeeded)
					{
						SkippedDefaults.Add(MakeSkipObj(
							FString::Printf(TEXT("%s.%s"), *NodeId, *DefaultPair.Key),
							TEXT("Pin not found on node — default ignored")));
						continue;
					}

					// Try to determine data type from the node's document interface
					// For simplicity, attempt all scalar types based on JSON value type
					FMetasoundFrontendLiteral Literal;
					FString DummyError;
					const TSharedPtr<FJsonValue>& Val = DefaultPair.Value;

					EJson JsonType = Val->Type;
					if (JsonType == EJson::Boolean)
					{
						bool BoolVal = false;
						Val->TryGetBool(BoolVal);
						FName DT;
						Literal = Sub.CreateBoolMetaSoundLiteral(BoolVal, DT);
					}
					else if (JsonType == EJson::Number)
					{
						double NumVal = 0.0;
						Val->TryGetNumber(NumVal);
						FName DT;
						Literal = Sub.CreateFloatMetaSoundLiteral(static_cast<float>(NumVal), DT);
					}
					else if (JsonType == EJson::String)
					{
						FString StrVal;
						Val->TryGetString(StrVal);

						// If it looks like an asset path, try loading as WaveAsset
						if (StrVal.StartsWith(TEXT("/Game/")) || StrVal.StartsWith(TEXT("/Engine/")))
						{
							UObject* Obj = StaticLoadObject(UObject::StaticClass(), nullptr, *StrVal);
							if (Obj)
							{
								Literal = Sub.CreateObjectMetaSoundLiteral(Obj);
							}
							else
							{
								FName DT;
								Literal = Sub.CreateStringMetaSoundLiteral(StrVal, DT);
							}
						}
						else
						{
							FName DT;
							Literal = Sub.CreateStringMetaSoundLiteral(StrVal, DT);
						}
					}
					else
					{
						continue;
					}

					EMetaSoundBuilderResult SetResult;
					Builder->SetNodeInputDefault(InputHandle, Literal, SetResult);
				}
			}
		}
	}

	// 6. Wire node-to-node connections
	int32 ConnectionCount = 0;
	const TArray<TSharedPtr<FJsonValue>>* ConnectionsArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("connections"), ConnectionsArray) && ConnectionsArray)
	{
		for (const auto& ConnVal : *ConnectionsArray)
		{
			const TSharedPtr<FJsonObject>& ConnObj = ConnVal->AsObject();
			if (!ConnObj.IsValid())
			{
				SkippedConnections.Add(MakeSkipObj(TEXT("<invalid>"), TEXT("Connection entry is not a JSON object")));
				continue;
			}

			const FString FromId = ConnObj->GetStringField(TEXT("from"));
			const FString OutputName = ConnObj->GetStringField(TEXT("output"));
			const FString ToId = ConnObj->GetStringField(TEXT("to"));
			const FString InputName = ConnObj->GetStringField(TEXT("input"));

			const FString ConnIdent = FString::Printf(TEXT("%s.%s -> %s.%s"), *FromId, *OutputName, *ToId, *InputName);

			const FMetaSoundNodeHandle* FromHandle = NodeMap.Find(FromId);
			const FMetaSoundNodeHandle* ToHandle = NodeMap.Find(ToId);

			if (!FromHandle || !ToHandle)
			{
				UE_LOG(LogMonolith, Warning, TEXT("build_metasound_from_spec: Connection from '%s' to '%s' — node not found"), *FromId, *ToId);
				SkippedConnections.Add(MakeSkipObj(ConnIdent, FString::Printf(TEXT("Endpoint node not found (from='%s' present=%s, to='%s' present=%s)"),
					*FromId, FromHandle ? TEXT("yes") : TEXT("no"),
					*ToId, ToHandle ? TEXT("yes") : TEXT("no"))));
				continue;
			}

			EMetaSoundBuilderResult ConnResult;
			Builder->ConnectNodes(*FromHandle, FName(*OutputName), *ToHandle, FName(*InputName), ConnResult);

			if (ConnResult == EMetaSoundBuilderResult::Succeeded)
			{
				ConnectionCount++;
			}
			else
			{
				UE_LOG(LogMonolith, Warning, TEXT("build_metasound_from_spec: Failed to connect %s.%s -> %s.%s"),
					*FromId, *OutputName, *ToId, *InputName);
				SkippedConnections.Add(MakeSkipObj(ConnIdent, TEXT("ConnectNodes failed (pin name/type mismatch?)")));
			}
		}
	}

	// 7. Wire graph inputs to node inputs
	const TArray<TSharedPtr<FJsonValue>>* GraphInputConns = nullptr;
	if (Spec->TryGetArrayField(TEXT("graph_input_connections"), GraphInputConns) && GraphInputConns)
	{
		for (const auto& ConnVal : *GraphInputConns)
		{
			const TSharedPtr<FJsonObject>& ConnObj = ConnVal->AsObject();
			if (!ConnObj.IsValid())
			{
				SkippedConnections.Add(MakeSkipObj(TEXT("<invalid graph_input>"), TEXT("graph_input_connection entry is not a JSON object")));
				continue;
			}

			const FString GraphInputName = ConnObj->GetStringField(TEXT("input"));
			const FString ToNodeId = ConnObj->GetStringField(TEXT("to_node"));
			const FString ToPinName = ConnObj->GetStringField(TEXT("to_pin"));

			const FString ConnIdent = FString::Printf(TEXT("graph_input '%s' -> %s.%s"), *GraphInputName, *ToNodeId, *ToPinName);

			const FMetaSoundNodeHandle* ToHandle = NodeMap.Find(ToNodeId);
			if (!ToHandle)
			{
				UE_LOG(LogMonolith, Warning, TEXT("build_metasound_from_spec: graph_input_connection to '%s' — node not found"), *ToNodeId);
				SkippedConnections.Add(MakeSkipObj(ConnIdent, FString::Printf(TEXT("Target node '%s' not found"), *ToNodeId)));
				continue;
			}

			EMetaSoundBuilderResult ConnResult;
			Builder->ConnectGraphInputToNode(FName(*GraphInputName), *ToHandle, FName(*ToPinName), ConnResult);
			if (ConnResult == EMetaSoundBuilderResult::Succeeded)
			{
				ConnectionCount++;
			}
			else
			{
				SkippedConnections.Add(MakeSkipObj(ConnIdent, TEXT("ConnectGraphInputToNode failed")));
			}
		}
	}

	// 8. Wire interface connections (OnPlay, Audio outputs, etc.)
	const TSharedPtr<FJsonObject>* InterfaceConns = nullptr;
	if (Spec->TryGetObjectField(TEXT("interface_connections"), InterfaceConns) && InterfaceConns && InterfaceConns->IsValid())
	{
		for (const auto& Pair : (*InterfaceConns)->Values)
		{
			const FString& InterfacePinName = Pair.Key;
			const TSharedPtr<FJsonObject>& ConnObj = Pair.Value->AsObject();
			if (!ConnObj.IsValid())
			{
				SkippedInterfaces.Add(MakeSkipObj(InterfacePinName, TEXT("Connection value is not a JSON object")));
				continue;
			}

			// Input connections: interface provides a trigger/value → connect to a node input
			if (ConnObj->HasField(TEXT("to_node")))
			{
				const FString ToNodeId = ConnObj->GetStringField(TEXT("to_node"));
				const FString ToPinName = ConnObj->GetStringField(TEXT("to_pin"));

				const FMetaSoundNodeHandle* ToHandle = NodeMap.Find(ToNodeId);
				if (!ToHandle)
				{
					UE_LOG(LogMonolith, Warning, TEXT("build_metasound_from_spec: interface_connection '%s' -> '%s' — node not found"), *InterfacePinName, *ToNodeId);
					SkippedInterfaces.Add(MakeSkipObj(
						FString::Printf(TEXT("%s -> %s.%s"), *InterfacePinName, *ToNodeId, *ToPinName),
						FString::Printf(TEXT("Target node '%s' not found"), *ToNodeId)));
					continue;
				}

				// Interface inputs are graph inputs — use ConnectGraphInputToNode
				// Strip any suffix like ":0" for array indexing
				FString CleanName = InterfacePinName;
				int32 ColonIdx;
				if (CleanName.FindChar(':', ColonIdx))
				{
					CleanName = CleanName.Left(ColonIdx);
				}

				EMetaSoundBuilderResult ConnResult;
				Builder->ConnectGraphInputToNode(FName(*CleanName), *ToHandle, FName(*ToPinName), ConnResult);
				if (ConnResult == EMetaSoundBuilderResult::Succeeded)
				{
					ConnectionCount++;
				}
				else
				{
					SkippedInterfaces.Add(MakeSkipObj(
						FString::Printf(TEXT("%s -> %s.%s"), *InterfacePinName, *ToNodeId, *ToPinName),
						TEXT("ConnectGraphInputToNode failed for interface input")));
				}
			}

			// Output connections: node output → connect to interface output (graph output)
			if (ConnObj->HasField(TEXT("from_node")))
			{
				const FString FromNodeId = ConnObj->GetStringField(TEXT("from_node"));
				const FString FromPinName = ConnObj->GetStringField(TEXT("from_pin"));

				const FMetaSoundNodeHandle* FromHandle = NodeMap.Find(FromNodeId);
				if (!FromHandle)
				{
					UE_LOG(LogMonolith, Warning, TEXT("build_metasound_from_spec: interface_connection from '%s' — node not found"), *FromNodeId);
					SkippedInterfaces.Add(MakeSkipObj(
						FString::Printf(TEXT("%s.%s -> %s"), *FromNodeId, *FromPinName, *InterfacePinName),
						FString::Printf(TEXT("Source node '%s' not found"), *FromNodeId)));
					continue;
				}

				FString CleanName = InterfacePinName;
				int32 ColonIdx;
				if (CleanName.FindChar(':', ColonIdx))
				{
					CleanName = CleanName.Left(ColonIdx);
				}

				EMetaSoundBuilderResult ConnResult;
				Builder->ConnectNodeToGraphOutput(*FromHandle, FName(*FromPinName), FName(*CleanName), ConnResult);
				if (ConnResult == EMetaSoundBuilderResult::Succeeded)
				{
					ConnectionCount++;
				}
				else
				{
					SkippedInterfaces.Add(MakeSkipObj(
						FString::Printf(TEXT("%s.%s -> %s"), *FromNodeId, *FromPinName, *InterfacePinName),
						TEXT("ConnectNodeToGraphOutput failed for interface output")));
				}
			}
		}
	}

	// 8b. strict_mode bail-out — if anything was skipped, refuse to save.
	const int32 TotalSkipped = SkippedNodes.Num() + SkippedInputs.Num() + SkippedOutputs.Num()
		+ SkippedConnections.Num() + SkippedInterfaces.Num() + SkippedDefaults.Num();
	if (bStrictMode && TotalSkipped > 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("strict_mode: %d items skipped during build (nodes=%d, inputs=%d, outputs=%d, connections=%d, interfaces=%d, defaults=%d) — asset NOT saved. Re-run with strict_mode=false to inspect skipped_* arrays."),
			TotalSkipped, SkippedNodes.Num(), SkippedInputs.Num(), SkippedOutputs.Num(),
			SkippedConnections.Num(), SkippedInterfaces.Num(), SkippedDefaults.Num()));
	}

	// 9. Auto-layout
	Builder->InitNodeLocations();

	// 10. Build to asset
	UMetaSoundEditorSubsystem* EditorSub = GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>();
	if (!EditorSub)
	{
		return FMonolithActionResult::Error(TEXT("UMetaSoundEditorSubsystem not available"));
	}

	EMetaSoundBuilderResult SaveResult;
	TScriptInterface<IMetaSoundDocumentInterface> NewAsset = EditorSub->BuildToAsset(
		Builder, TEXT("Monolith"), AssetName, PackagePath, SaveResult, nullptr);

	if (SaveResult != EMetaSoundBuilderResult::Succeeded || !NewAsset.GetObject())
	{
		return FMonolithActionResult::Error(TEXT("BuildToAsset failed"));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), NewAsset.GetObject()->GetPathName());
	ResultJson->SetNumberField(TEXT("node_count"), NodeCount);
	ResultJson->SetNumberField(TEXT("connection_count"), ConnectionCount);
	ResultJson->SetArrayField(TEXT("skipped_nodes"), SkippedNodes);
	ResultJson->SetArrayField(TEXT("skipped_inputs"), SkippedInputs);
	ResultJson->SetArrayField(TEXT("skipped_outputs"), SkippedOutputs);
	ResultJson->SetArrayField(TEXT("skipped_connections"), SkippedConnections);
	ResultJson->SetArrayField(TEXT("skipped_interfaces"), SkippedInterfaces);
	ResultJson->SetArrayField(TEXT("skipped_defaults"), SkippedDefaults);
	if (TotalSkipped > 0)
	{
		ResultJson->SetStringField(TEXT("warning"), FString::Printf(
			TEXT("%d spec items were skipped during build — see skipped_* arrays. Use strict_mode=true to abort instead."), TotalSkipped));
	}
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::CreateMetaSoundPreset(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString ReferencePath = Params->GetStringField(TEXT("reference_metasound"));

	if (AssetPath.IsEmpty() || ReferencePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and reference_metasound are required"));
	}

	FString PackagePath, AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		return FMonolithActionResult::Error(TEXT("Invalid asset_path"));
	}

	// Load the reference MetaSound
	UObject* RefObj = StaticLoadObject(UObject::StaticClass(), nullptr, *ReferencePath);
	if (!RefObj)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Reference MetaSound not found at '%s'"), *ReferencePath));
	}

	TScriptInterface<IMetaSoundDocumentInterface> RefInterface;
	if (RefObj->GetClass()->ImplementsInterface(UMetaSoundDocumentInterface::StaticClass()))
	{
		RefInterface.SetObject(RefObj);
		RefInterface.SetInterface(Cast<IMetaSoundDocumentInterface>(RefObj));
	}

	if (!RefInterface.GetInterface())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset at '%s' does not implement IMetaSoundDocumentInterface"), *ReferencePath));
	}

	UMetaSoundBuilderSubsystem& Sub = UMetaSoundBuilderSubsystem::GetChecked();
	EMetaSoundBuilderResult BuildResult;
	FName BuilderName = FName(*FString::Printf(TEXT("Monolith_Preset_%s"), *AssetName));

	// Use the generic CreatePresetBuilder which returns a reference
	UMetaSoundBuilderBase& Builder = Sub.CreatePresetBuilder(BuilderName, RefInterface, BuildResult);

	if (BuildResult != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create preset builder"));
	}

	Builder.InitNodeLocations();

	UMetaSoundEditorSubsystem* EditorSub = GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>();
	if (!EditorSub)
	{
		return FMonolithActionResult::Error(TEXT("UMetaSoundEditorSubsystem not available"));
	}

	EMetaSoundBuilderResult SaveResult;
	TScriptInterface<IMetaSoundDocumentInterface> NewAsset = EditorSub->BuildToAsset(
		&Builder, TEXT("Monolith"), AssetName, PackagePath, SaveResult, nullptr);

	if (SaveResult != EMetaSoundBuilderResult::Succeeded || !NewAsset.GetObject())
	{
		return FMonolithActionResult::Error(TEXT("BuildToAsset failed for preset"));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), NewAsset.GetObject()->GetPathName());
	ResultJson->SetStringField(TEXT("reference"), ReferencePath);
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::CreateOneShotSfx(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString WavePath = Params->GetStringField(TEXT("sound_wave"));

	if (AssetPath.IsEmpty() || WavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and sound_wave are required"));
	}

	USoundWave* Wave = Cast<USoundWave>(StaticLoadObject(USoundWave::StaticClass(), nullptr, *WavePath));
	if (!Wave)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("SoundWave not found at '%s'"), *WavePath));
	}

	FString PackagePath, AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		return FMonolithActionResult::Error(TEXT("Invalid asset_path"));
	}

	UMetaSoundBuilderSubsystem& Sub = UMetaSoundBuilderSubsystem::GetChecked();
	EMetaSoundBuilderResult BuildResult;
	FName BuilderName = FName(*FString::Printf(TEXT("Monolith_%s"), *AssetName));

	FMetaSoundBuilderNodeOutputHandle OnPlayOutput;
	FMetaSoundBuilderNodeInputHandle OnFinishedInput;
	TArray<FMetaSoundBuilderNodeInputHandle> AudioOutInputs;

	UMetaSoundSourceBuilder* Builder = Sub.CreateSourceBuilder(
		BuilderName, OnPlayOutput, OnFinishedInput, AudioOutInputs,
		BuildResult, EMetaSoundOutputAudioFormat::Mono, /*bIsOneShot=*/ true);

	if (!Builder || BuildResult != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create source builder"));
	}

	// Add Wave Player node
	EMetaSoundBuilderResult NodeResult;
	FMetaSoundNodeHandle WavePlayer = Builder->AddNodeByClassName(
		FMetasoundFrontendClassName(FName("UE"), FName("Wave Player"), FName("Mono")),
		NodeResult, 1);

	if (NodeResult != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(TEXT("Failed to add Wave Player node"));
	}

	// Set Wave Asset input
	FMetaSoundBuilderNodeInputHandle WaveInput = Builder->FindNodeInputByName(
		WavePlayer, FName("Wave Asset"), NodeResult);
	if (NodeResult == EMetaSoundBuilderResult::Succeeded)
	{
		FMetasoundFrontendLiteral WaveLiteral = Sub.CreateObjectMetaSoundLiteral(Wave);
		Builder->SetNodeInputDefault(WaveInput, WaveLiteral, NodeResult);
	}

	// Connect OnPlay -> Wave Player Play (handle-based — OnPlayOutput is an output handle)
	FMetaSoundBuilderNodeInputHandle PlayInput = Builder->FindNodeInputByName(WavePlayer, FName("Play"), NodeResult);
	if (NodeResult == EMetaSoundBuilderResult::Succeeded)
	{
		Builder->ConnectNodes(OnPlayOutput, PlayInput, NodeResult);
	}

	// Connect Wave Player Audio -> Audio Output
	FMetaSoundBuilderNodeOutputHandle AudioOutput = Builder->FindNodeOutputByName(WavePlayer, FName("Audio"), NodeResult);
	if (NodeResult == EMetaSoundBuilderResult::Succeeded && AudioOutInputs.Num() > 0)
	{
		Builder->ConnectNodes(AudioOutput, AudioOutInputs[0], NodeResult);
	}

	// Connect Wave Player OnFinished -> OnFinished
	FMetaSoundBuilderNodeOutputHandle PlayerFinished = Builder->FindNodeOutputByName(WavePlayer, FName("On Finished"), NodeResult);
	if (NodeResult == EMetaSoundBuilderResult::Succeeded)
	{
		Builder->ConnectNodes(PlayerFinished, OnFinishedInput, NodeResult);
	}

	Builder->InitNodeLocations();

	UMetaSoundEditorSubsystem* EditorSub = GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>();
	if (!EditorSub)
	{
		return FMonolithActionResult::Error(TEXT("UMetaSoundEditorSubsystem not available"));
	}

	EMetaSoundBuilderResult SaveResult;
	TScriptInterface<IMetaSoundDocumentInterface> NewAsset = EditorSub->BuildToAsset(
		Builder, TEXT("Monolith"), AssetName, PackagePath, SaveResult, nullptr);

	if (SaveResult != EMetaSoundBuilderResult::Succeeded || !NewAsset.GetObject())
	{
		return FMonolithActionResult::Error(TEXT("BuildToAsset failed"));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), NewAsset.GetObject()->GetPathName());
	ResultJson->SetStringField(TEXT("sound_wave"), WavePath);
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::CreateLoopingAmbientMetaSound(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString WavePath = Params->GetStringField(TEXT("sound_wave"));

	if (AssetPath.IsEmpty() || WavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and sound_wave are required"));
	}

	USoundWave* Wave = Cast<USoundWave>(StaticLoadObject(USoundWave::StaticClass(), nullptr, *WavePath));
	if (!Wave)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("SoundWave not found at '%s'"), *WavePath));
	}

	float LfoFrequency = 0.25f;
	if (Params->HasField(TEXT("lfo_frequency")))
	{
		LfoFrequency = static_cast<float>(Params->GetNumberField(TEXT("lfo_frequency")));
	}

	float PitchMin = 0.95f, PitchMax = 1.05f;
	const TArray<TSharedPtr<FJsonValue>>* PitchRange = nullptr;
	if (Params->TryGetArrayField(TEXT("pitch_range"), PitchRange) && PitchRange && PitchRange->Num() >= 2)
	{
		double TmpMin = PitchMin, TmpMax = PitchMax;
		(*PitchRange)[0]->TryGetNumber(TmpMin);
		(*PitchRange)[1]->TryGetNumber(TmpMax);
		PitchMin = static_cast<float>(TmpMin);
		PitchMax = static_cast<float>(TmpMax);
	}

	FString PackagePath, AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		return FMonolithActionResult::Error(TEXT("Invalid asset_path"));
	}

	UMetaSoundBuilderSubsystem& Sub = UMetaSoundBuilderSubsystem::GetChecked();
	EMetaSoundBuilderResult BuildResult;
	FName BuilderName = FName(*FString::Printf(TEXT("Monolith_%s"), *AssetName));

	FMetaSoundBuilderNodeOutputHandle OnPlayOutput;
	FMetaSoundBuilderNodeInputHandle OnFinishedInput;
	TArray<FMetaSoundBuilderNodeInputHandle> AudioOutInputs;

	// Not one-shot — looping
	UMetaSoundSourceBuilder* Builder = Sub.CreateSourceBuilder(
		BuilderName, OnPlayOutput, OnFinishedInput, AudioOutInputs,
		BuildResult, EMetaSoundOutputAudioFormat::Mono, /*bIsOneShot=*/ false);

	if (!Builder || BuildResult != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create source builder"));
	}

	EMetaSoundBuilderResult NR;

	// Wave Player (looping)
	FMetaSoundNodeHandle WavePlayer = Builder->AddNodeByClassName(
		FMetasoundFrontendClassName(FName("UE"), FName("Wave Player"), FName("Mono")), NR, 1);

	// Set wave asset
	FMetaSoundBuilderNodeInputHandle WaveInput = Builder->FindNodeInputByName(WavePlayer, FName("Wave Asset"), NR);
	if (NR == EMetaSoundBuilderResult::Succeeded)
	{
		FMetasoundFrontendLiteral WaveLiteral = Sub.CreateObjectMetaSoundLiteral(Wave);
		Builder->SetNodeInputDefault(WaveInput, WaveLiteral, NR);
	}

	// Set looping
	FMetaSoundBuilderNodeInputHandle LoopInput = Builder->FindNodeInputByName(WavePlayer, FName("Loop"), NR);
	if (NR == EMetaSoundBuilderResult::Succeeded)
	{
		FName DT;
		FMetasoundFrontendLiteral LoopLiteral = Sub.CreateBoolMetaSoundLiteral(true, DT);
		Builder->SetNodeInputDefault(LoopInput, LoopLiteral, NR);
	}

	// LFO for volume modulation
	FMetaSoundNodeHandle LFO = Builder->AddNodeByClassName(
		FMetasoundFrontendClassName(FName("UE"), FName("LFO"), FName("")), NR, 1);

	// Set LFO frequency
	FMetaSoundBuilderNodeInputHandle LfoFreqInput = Builder->FindNodeInputByName(LFO, FName("Frequency"), NR);
	if (NR == EMetaSoundBuilderResult::Succeeded)
	{
		FName DT;
		FMetasoundFrontendLiteral FreqLiteral = Sub.CreateFloatMetaSoundLiteral(LfoFrequency, DT);
		Builder->SetNodeInputDefault(LfoFreqInput, FreqLiteral, NR);
	}

	// Random node for pitch variation
	FMetaSoundNodeHandle RandomFloat = Builder->AddNodeByClassName(
		FMetasoundFrontendClassName(FName("UE"), FName("Random (Float)"), FName("")), NR, 1);

	// Set random min/max
	FMetaSoundBuilderNodeInputHandle RandMin = Builder->FindNodeInputByName(RandomFloat, FName("Min"), NR);
	if (NR == EMetaSoundBuilderResult::Succeeded)
	{
		FName DT;
		Builder->SetNodeInputDefault(RandMin, Sub.CreateFloatMetaSoundLiteral(PitchMin, DT), NR);
	}
	FMetaSoundBuilderNodeInputHandle RandMax = Builder->FindNodeInputByName(RandomFloat, FName("Max"), NR);
	if (NR == EMetaSoundBuilderResult::Succeeded)
	{
		FName DT;
		Builder->SetNodeInputDefault(RandMax, Sub.CreateFloatMetaSoundLiteral(PitchMax, DT), NR);
	}

	// Set pitch on wave player
	FMetaSoundBuilderNodeOutputHandle RandOut = Builder->FindNodeOutputByName(RandomFloat, FName("Value"), NR);
	FMetaSoundBuilderNodeInputHandle PitchInput = Builder->FindNodeInputByName(WavePlayer, FName("Pitch Shift"), NR);
	if (NR == EMetaSoundBuilderResult::Succeeded && RandOut.VertexID.IsValid())
	{
		Builder->ConnectNodes(RandOut, PitchInput, NR);
	}

	// Connect OnPlay -> WavePlayer Play
	FMetaSoundBuilderNodeInputHandle PlayInput = Builder->FindNodeInputByName(WavePlayer, FName("Play"), NR);
	if (NR == EMetaSoundBuilderResult::Succeeded)
	{
		Builder->ConnectNodes(OnPlayOutput, PlayInput, NR);
	}

	// Connect WavePlayer Audio -> Audio Out
	FMetaSoundBuilderNodeOutputHandle AudioOut = Builder->FindNodeOutputByName(WavePlayer, FName("Audio"), NR);
	if (NR == EMetaSoundBuilderResult::Succeeded && AudioOutInputs.Num() > 0)
	{
		Builder->ConnectNodes(AudioOut, AudioOutInputs[0], NR);
	}

	// Connect OnPlay -> Random trigger
	FMetaSoundBuilderNodeInputHandle RandTrigger = Builder->FindNodeInputByName(RandomFloat, FName("Next"), NR);
	if (NR == EMetaSoundBuilderResult::Succeeded)
	{
		Builder->ConnectNodes(OnPlayOutput, RandTrigger, NR);
	}

	Builder->InitNodeLocations();

	UMetaSoundEditorSubsystem* EditorSub = GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>();
	if (!EditorSub)
	{
		return FMonolithActionResult::Error(TEXT("UMetaSoundEditorSubsystem not available"));
	}

	EMetaSoundBuilderResult SaveResult;
	TScriptInterface<IMetaSoundDocumentInterface> NewAsset = EditorSub->BuildToAsset(
		Builder, TEXT("Monolith"), AssetName, PackagePath, SaveResult, nullptr);

	if (SaveResult != EMetaSoundBuilderResult::Succeeded || !NewAsset.GetObject())
	{
		return FMonolithActionResult::Error(TEXT("BuildToAsset failed"));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), NewAsset.GetObject()->GetPathName());
	ResultJson->SetStringField(TEXT("sound_wave"), WavePath);
	ResultJson->SetNumberField(TEXT("lfo_frequency"), LfoFrequency);
	ResultJson->SetNumberField(TEXT("pitch_min"), PitchMin);
	ResultJson->SetNumberField(TEXT("pitch_max"), PitchMax);
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::CreateSynthesizedTone(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	const FString OscType = Params->HasField(TEXT("oscillator_type"))
		? Params->GetStringField(TEXT("oscillator_type")) : TEXT("Sine");

	float Frequency = 440.0f;
	if (Params->HasField(TEXT("frequency")))
	{
		Frequency = static_cast<float>(Params->GetNumberField(TEXT("frequency")));
	}

	// ADSR defaults
	float Attack = 0.01f, Decay = 0.1f, Sustain = 0.7f, Release = 0.3f;
	const TSharedPtr<FJsonObject>* AdsrPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("adsr"), AdsrPtr) && AdsrPtr && AdsrPtr->IsValid())
	{
		if ((*AdsrPtr)->HasField(TEXT("attack"))) Attack = static_cast<float>((*AdsrPtr)->GetNumberField(TEXT("attack")));
		if ((*AdsrPtr)->HasField(TEXT("decay"))) Decay = static_cast<float>((*AdsrPtr)->GetNumberField(TEXT("decay")));
		if ((*AdsrPtr)->HasField(TEXT("sustain"))) Sustain = static_cast<float>((*AdsrPtr)->GetNumberField(TEXT("sustain")));
		if ((*AdsrPtr)->HasField(TEXT("release"))) Release = static_cast<float>((*AdsrPtr)->GetNumberField(TEXT("release")));
	}

	FString PackagePath, AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		return FMonolithActionResult::Error(TEXT("Invalid asset_path"));
	}

	UMetaSoundBuilderSubsystem& Sub = UMetaSoundBuilderSubsystem::GetChecked();
	EMetaSoundBuilderResult BuildResult;
	FName BuilderName = FName(*FString::Printf(TEXT("Monolith_%s"), *AssetName));

	FMetaSoundBuilderNodeOutputHandle OnPlayOutput;
	FMetaSoundBuilderNodeInputHandle OnFinishedInput;
	TArray<FMetaSoundBuilderNodeInputHandle> AudioOutInputs;

	UMetaSoundSourceBuilder* Builder = Sub.CreateSourceBuilder(
		BuilderName, OnPlayOutput, OnFinishedInput, AudioOutInputs,
		BuildResult, EMetaSoundOutputAudioFormat::Mono, /*bIsOneShot=*/ true);

	if (!Builder || BuildResult != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create source builder"));
	}

	EMetaSoundBuilderResult NR;

	// Determine oscillator class variant
	FName OscVariant = FName("Audio");
	FName OscName = FName("Sine");
	if (OscType.Equals(TEXT("Saw"), ESearchCase::IgnoreCase))
	{
		OscName = FName("Saw");
	}
	else if (OscType.Equals(TEXT("Square"), ESearchCase::IgnoreCase))
	{
		OscName = FName("Square");
	}

	// Add oscillator
	FMetaSoundNodeHandle Osc = Builder->AddNodeByClassName(
		FMetasoundFrontendClassName(FName("UE"), OscName, OscVariant), NR, 1);

	// Set frequency
	FMetaSoundBuilderNodeInputHandle FreqInput = Builder->FindNodeInputByName(Osc, FName("Frequency"), NR);
	if (NR == EMetaSoundBuilderResult::Succeeded)
	{
		FName DT;
		Builder->SetNodeInputDefault(FreqInput, Sub.CreateFloatMetaSoundLiteral(Frequency, DT), NR);
	}

	// Add ADSR envelope
	FMetaSoundNodeHandle ADSR = Builder->AddNodeByClassName(
		FMetasoundFrontendClassName(FName("UE"), FName("AD Envelope"), FName("Audio")), NR, 1);

	// Set ADSR params
	FMetaSoundBuilderNodeInputHandle AttackInput = Builder->FindNodeInputByName(ADSR, FName("Attack Time"), NR);
	if (NR == EMetaSoundBuilderResult::Succeeded)
	{
		FName DT;
		Builder->SetNodeInputDefault(AttackInput, Sub.CreateFloatMetaSoundLiteral(Attack, DT), NR);
	}
	FMetaSoundBuilderNodeInputHandle DecayInput = Builder->FindNodeInputByName(ADSR, FName("Decay Time"), NR);
	if (NR == EMetaSoundBuilderResult::Succeeded)
	{
		FName DT;
		Builder->SetNodeInputDefault(DecayInput, Sub.CreateFloatMetaSoundLiteral(Decay, DT), NR);
	}

	// Add Multiply (Audio) for envelope * oscillator
	FMetaSoundNodeHandle Multiply = Builder->AddNodeByClassName(
		FMetasoundFrontendClassName(FName("UE"), FName("Multiply"), FName("Audio")), NR, 1);

	// Wire: Osc Audio -> Multiply A
	FMetaSoundBuilderNodeOutputHandle OscAudio = Builder->FindNodeOutputByName(Osc, FName("Audio"), NR);
	if (NR == EMetaSoundBuilderResult::Succeeded)
	{
		Builder->ConnectNodes(Osc, FName("Audio"), Multiply, FName("A"), NR);
	}

	// Wire: ADSR Envelope -> Multiply B
	Builder->ConnectNodes(ADSR, FName("Out Envelope"), Multiply, FName("B"), NR);

	// Wire: Multiply Audio -> Audio Out
	FMetaSoundBuilderNodeOutputHandle MultOut = Builder->FindNodeOutputByName(Multiply, FName("Out"), NR);
	if (NR == EMetaSoundBuilderResult::Succeeded && AudioOutInputs.Num() > 0)
	{
		Builder->ConnectNodes(MultOut, AudioOutInputs[0], NR);
	}

	// Wire: OnPlay -> ADSR Trigger
	FMetaSoundBuilderNodeInputHandle TriggerInput = Builder->FindNodeInputByName(ADSR, FName("Trigger"), NR);
	if (NR == EMetaSoundBuilderResult::Succeeded)
	{
		Builder->ConnectNodes(OnPlayOutput, TriggerInput, NR);
	}

	// Add a graph input for frequency control
	FName DT;
	FMetasoundFrontendLiteral FreqDefault = Sub.CreateFloatMetaSoundLiteral(Frequency, DT);
	Builder->AddGraphInputNode(FName("Frequency"), FName("Float"), FreqDefault, NR);
	if (NR == EMetaSoundBuilderResult::Succeeded)
	{
		Builder->ConnectGraphInputToNode(FName("Frequency"), Osc, FName("Frequency"), NR);
	}

	Builder->InitNodeLocations();

	UMetaSoundEditorSubsystem* EditorSub = GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>();
	if (!EditorSub)
	{
		return FMonolithActionResult::Error(TEXT("UMetaSoundEditorSubsystem not available"));
	}

	EMetaSoundBuilderResult SaveResult;
	TScriptInterface<IMetaSoundDocumentInterface> NewAsset = EditorSub->BuildToAsset(
		Builder, TEXT("Monolith"), AssetName, PackagePath, SaveResult, nullptr);

	if (SaveResult != EMetaSoundBuilderResult::Succeeded || !NewAsset.GetObject())
	{
		return FMonolithActionResult::Error(TEXT("BuildToAsset failed"));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), NewAsset.GetObject()->GetPathName());
	ResultJson->SetStringField(TEXT("oscillator_type"), OscType);
	ResultJson->SetNumberField(TEXT("frequency"), Frequency);
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::CreateInteractiveMetaSound(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	const TArray<TSharedPtr<FJsonValue>>* WavesArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("sound_waves"), WavesArray) || !WavesArray || WavesArray->Num() < 2)
	{
		return FMonolithActionResult::Error(TEXT("sound_waves is required and must contain at least 2 entries"));
	}

	FString ParamName = Params->HasField(TEXT("parameter_name"))
		? Params->GetStringField(TEXT("parameter_name")) : TEXT("BlendAmount");

	const FString BlendType = Params->HasField(TEXT("blend_type"))
		? Params->GetStringField(TEXT("blend_type")) : TEXT("Crossfade");

	// Load all waves
	TArray<USoundWave*> Waves;
	for (const auto& WaveVal : *WavesArray)
	{
		FString WavePath;
		WaveVal->TryGetString(WavePath);
		USoundWave* Wave = Cast<USoundWave>(StaticLoadObject(USoundWave::StaticClass(), nullptr, *WavePath));
		if (!Wave)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("SoundWave not found at '%s'"), *WavePath));
		}
		Waves.Add(Wave);
	}

	FString PackagePath, AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		return FMonolithActionResult::Error(TEXT("Invalid asset_path"));
	}

	UMetaSoundBuilderSubsystem& Sub = UMetaSoundBuilderSubsystem::GetChecked();
	EMetaSoundBuilderResult BuildResult;
	FName BuilderName = FName(*FString::Printf(TEXT("Monolith_%s"), *AssetName));

	FMetaSoundBuilderNodeOutputHandle OnPlayOutput;
	FMetaSoundBuilderNodeInputHandle OnFinishedInput;
	TArray<FMetaSoundBuilderNodeInputHandle> AudioOutInputs;

	UMetaSoundSourceBuilder* Builder = Sub.CreateSourceBuilder(
		BuilderName, OnPlayOutput, OnFinishedInput, AudioOutInputs,
		BuildResult, EMetaSoundOutputAudioFormat::Mono, /*bIsOneShot=*/ false);

	if (!Builder || BuildResult != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create source builder"));
	}

	EMetaSoundBuilderResult NR;

	// Add blend parameter as graph input
	FName DT;
	FMetasoundFrontendLiteral BlendDefault = Sub.CreateFloatMetaSoundLiteral(0.0f, DT);
	Builder->AddGraphInputNode(FName(*ParamName), FName("Float"), BlendDefault, NR);

	// Create wave players for each wave
	TArray<FMetaSoundNodeHandle> WavePlayers;
	for (int32 i = 0; i < Waves.Num(); i++)
	{
		FMetaSoundNodeHandle Player = Builder->AddNodeByClassName(
			FMetasoundFrontendClassName(FName("UE"), FName("Wave Player"), FName("Mono")), NR, 1);

		if (NR != EMetaSoundBuilderResult::Succeeded) continue;

		// Set wave asset
		FMetaSoundBuilderNodeInputHandle WaveInput = Builder->FindNodeInputByName(Player, FName("Wave Asset"), NR);
		if (NR == EMetaSoundBuilderResult::Succeeded)
		{
			Builder->SetNodeInputDefault(WaveInput, Sub.CreateObjectMetaSoundLiteral(Waves[i]), NR);
		}

		// Set looping
		FMetaSoundBuilderNodeInputHandle LoopInput = Builder->FindNodeInputByName(Player, FName("Loop"), NR);
		if (NR == EMetaSoundBuilderResult::Succeeded)
		{
			Builder->SetNodeInputDefault(LoopInput, Sub.CreateBoolMetaSoundLiteral(true, DT), NR);
		}

		// Connect OnPlay -> Play
		FMetaSoundBuilderNodeInputHandle PlayInput = Builder->FindNodeInputByName(Player, FName("Play"), NR);
		if (NR == EMetaSoundBuilderResult::Succeeded)
		{
			Builder->ConnectNodes(OnPlayOutput, PlayInput, NR);
		}

		WavePlayers.Add(Player);
	}

	// For 2 waves: simple crossfade using Stereo Mixer or Multiply + invert
	if (Waves.Num() == 2 && WavePlayers.Num() == 2)
	{
		// Wave A * (1 - blend), Wave B * blend, then add
		// Multiply A
		FMetaSoundNodeHandle MultA = Builder->AddNodeByClassName(
			FMetasoundFrontendClassName(FName("UE"), FName("Multiply"), FName("Audio Float")), NR, 1);
		// Multiply B
		FMetaSoundNodeHandle MultB = Builder->AddNodeByClassName(
			FMetasoundFrontendClassName(FName("UE"), FName("Multiply"), FName("Audio Float")), NR, 1);
		// Subtract node: 1 - blend
		FMetaSoundNodeHandle Sub1 = Builder->AddNodeByClassName(
			FMetasoundFrontendClassName(FName("UE"), FName("Subtract"), FName("Float")), NR, 1);
		// Mixer (add)
		FMetaSoundNodeHandle Add = Builder->AddNodeByClassName(
			FMetasoundFrontendClassName(FName("UE"), FName("Add"), FName("Audio")), NR, 1);

		// Set 1.0 constant for subtraction
		FMetaSoundBuilderNodeInputHandle SubAInput = Builder->FindNodeInputByName(Sub1, FName("A"), NR);
		if (NR == EMetaSoundBuilderResult::Succeeded)
		{
			Builder->SetNodeInputDefault(SubAInput, Sub.CreateFloatMetaSoundLiteral(1.0f, DT), NR);
		}

		// Connect blend param -> Sub1.B
		Builder->ConnectGraphInputToNode(FName(*ParamName), Sub1, FName("B"), NR);

		// Connect Sub1.Out -> MultA.Float
		Builder->ConnectNodes(Sub1, FName("Out"), MultA, FName("Float"), NR);

		// Connect blend param -> MultB.Float
		Builder->ConnectGraphInputToNode(FName(*ParamName), MultB, FName("Float"), NR);

		// Connect WavePlayer[0].Audio -> MultA.Audio
		Builder->ConnectNodes(WavePlayers[0], FName("Audio"), MultA, FName("Audio"), NR);

		// Connect WavePlayer[1].Audio -> MultB.Audio
		Builder->ConnectNodes(WavePlayers[1], FName("Audio"), MultB, FName("Audio"), NR);

		// Connect MultA.Out -> Add.A
		Builder->ConnectNodes(MultA, FName("Out"), Add, FName("A"), NR);

		// Connect MultB.Out -> Add.B
		Builder->ConnectNodes(MultB, FName("Out"), Add, FName("B"), NR);

		// Connect Add.Out -> Audio Output
		FMetaSoundBuilderNodeOutputHandle AddOut = Builder->FindNodeOutputByName(Add, FName("Out"), NR);
		if (NR == EMetaSoundBuilderResult::Succeeded && AudioOutInputs.Num() > 0)
		{
			Builder->ConnectNodes(AddOut, AudioOutInputs[0], NR);
		}
	}
	else if (AudioOutInputs.Num() > 0 && WavePlayers.Num() > 0)
	{
		// For 3+ waves, just connect the first one for now
		// A full N-way crossfade would need a more complex graph
		FMetaSoundBuilderNodeOutputHandle FirstAudio = Builder->FindNodeOutputByName(WavePlayers[0], FName("Audio"), NR);
		if (NR == EMetaSoundBuilderResult::Succeeded)
		{
			Builder->ConnectNodes(FirstAudio, AudioOutInputs[0], NR);
		}
	}

	Builder->InitNodeLocations();

	UMetaSoundEditorSubsystem* EditorSub = GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>();
	if (!EditorSub)
	{
		return FMonolithActionResult::Error(TEXT("UMetaSoundEditorSubsystem not available"));
	}

	EMetaSoundBuilderResult SaveResult;
	TScriptInterface<IMetaSoundDocumentInterface> NewAsset = EditorSub->BuildToAsset(
		Builder, TEXT("Monolith"), AssetName, PackagePath, SaveResult, nullptr);

	if (SaveResult != EMetaSoundBuilderResult::Succeeded || !NewAsset.GetObject())
	{
		return FMonolithActionResult::Error(TEXT("BuildToAsset failed"));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), NewAsset.GetObject()->GetPathName());
	ResultJson->SetStringField(TEXT("parameter_name"), ParamName);
	ResultJson->SetStringField(TEXT("blend_type"), BlendType);
	ResultJson->SetNumberField(TEXT("wave_count"), Waves.Num());
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::AddMetaSoundVariable(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString VarName = Params->GetStringField(TEXT("name"));
	const FString DataType = Params->GetStringField(TEXT("data_type"));

	if (AssetPath.IsEmpty() || VarName.IsEmpty() || DataType.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path, name, and data_type are required"));
	}

	FString Error;
	UMetaSoundBuilderBase* Builder = GetBuilderForAsset(AssetPath, Error);
	if (!Builder)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Create default literal
	FMetasoundFrontendLiteral DefaultLiteral;
	TSharedPtr<FJsonValue> DefaultVal = Params->TryGetField(TEXT("default_value"));
	if (!CreateLiteralFromJson(DefaultVal, DataType, DefaultLiteral, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	EMetaSoundBuilderResult Result;
	Builder->AddGraphVariable(FName(*VarName), FName(*DataType), DefaultLiteral, Result);

	if (Result != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to add variable '%s' of type '%s'"), *VarName, *DataType));
	}

	// Also add Get and Set nodes for convenience
	EMetaSoundBuilderResult GetResult, SetResult;
	FMetaSoundNodeHandle GetNode = Builder->AddGraphVariableGetNode(FName(*VarName), GetResult);
	FMetaSoundNodeHandle SetNode = Builder->AddGraphVariableSetNode(FName(*VarName), SetResult);

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("variable_name"), VarName);
	ResultJson->SetStringField(TEXT("data_type"), DataType);
	if (GetResult == EMetaSoundBuilderResult::Succeeded)
	{
		ResultJson->SetStringField(TEXT("get_node"), GetNode.NodeID.ToString());
	}
	if (SetResult == EMetaSoundBuilderResult::Succeeded)
	{
		ResultJson->SetStringField(TEXT("set_node"), SetNode.NodeID.ToString());
	}
	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithAudioMetaSoundActions::SetMetaSoundNodeLocation(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString NodeIdStr = Params->GetStringField(TEXT("node_id_or_handle"));

	if (AssetPath.IsEmpty() || NodeIdStr.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and node_id_or_handle are required"));
	}

	if (!Params->HasField(TEXT("x")) || !Params->HasField(TEXT("y")))
	{
		return FMonolithActionResult::Error(TEXT("x and y are required"));
	}

	const double X = Params->GetNumberField(TEXT("x"));
	const double Y = Params->GetNumberField(TEXT("y"));

	FString Error;
	UMetaSoundBuilderBase* Builder = GetBuilderForAsset(AssetPath, Error);
	if (!Builder)
	{
		return FMonolithActionResult::Error(Error);
	}

	FMetaSoundNodeHandle NodeHandle;
	if (!ResolveNodeHandle(Builder, NodeIdStr, NodeHandle, Error, AssetPath))
	{
		return FMonolithActionResult::Error(Error);
	}

	UMetaSoundEditorSubsystem* EditorSub = GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>();
	if (!EditorSub)
	{
		return FMonolithActionResult::Error(TEXT("UMetaSoundEditorSubsystem not available"));
	}

	EMetaSoundBuilderResult Result;
	EditorSub->SetNodeLocation(Builder, NodeHandle, FVector2D(X, Y), Result);

	if (Result != EMetaSoundBuilderResult::Succeeded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to set location for node '%s'"), *NodeIdStr));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetBoolField(TEXT("success"), true);
	ResultJson->SetStringField(TEXT("node"), NodeIdStr);
	ResultJson->SetNumberField(TEXT("x"), X);
	ResultJson->SetNumberField(TEXT("y"), Y);
	return FMonolithActionResult::Success(ResultJson);
}

#endif // WITH_METASOUND
