#include "MonolithAudioSoundCueActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h" // LogMonolith

// Sound Cue core
#include "Sound/SoundCue.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundNode.h"

// All 22 sound node types
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundNodeRandom.h"
#include "Sound/SoundNodeMixer.h"
#include "Sound/SoundNodeDelay.h"
#include "Sound/SoundNodeAttenuation.h"
#include "Sound/SoundNodeModulator.h"
#include "Sound/SoundNodeConcatenator.h"
#include "Sound/SoundNodeLooping.h"
#include "Sound/SoundNodeBranch.h"
#include "Sound/SoundNodeSwitch.h"
#include "Sound/SoundNodeDistanceCrossFade.h"
#include "Sound/SoundNodeParamCrossFade.h"
#include "Sound/SoundNodeEnveloper.h"
#include "Sound/SoundNodeOscillator.h"
#include "Sound/SoundNodeDoppler.h"
#include "Sound/SoundNodeSoundClass.h"
#include "Sound/SoundNodeMature.h"
#include "Sound/SoundNodeQualityLevel.h"
#include "Sound/SoundNodeGroupControl.h"
#include "Sound/SoundNodeWaveParam.h"
#include "Sound/SoundNodeDialoguePlayer.h"
#include "Sound/SoundNodeModulatorContinuous.h"

// Factory
#include "Factories/SoundCueFactoryNew.h"

// Sound Cue editor graph (AudioEditor) - the back pointers the engine graph helpers CastCheck
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "SoundCueGraph/SoundCueGraphNode.h"
#include "SoundCueGraph/SoundCueGraphNode_Root.h"

// Editor utilities
#include "Editor.h" // GEditor
#include "ObjectTools.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"

// Asset registry & utilities
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

// JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ============================================================================
// Node Type Registry
// ============================================================================

const TMap<FString, UClass*>& FMonolithAudioSoundCueActions::GetNodeTypeRegistry()
{
	static TMap<FString, UClass*> Registry;
	if (Registry.Num() == 0)
	{
		Registry.Add(TEXT("WavePlayer"),           USoundNodeWavePlayer::StaticClass());
		Registry.Add(TEXT("Random"),               USoundNodeRandom::StaticClass());
		Registry.Add(TEXT("Mixer"),                USoundNodeMixer::StaticClass());
		Registry.Add(TEXT("Delay"),                USoundNodeDelay::StaticClass());
		Registry.Add(TEXT("Attenuation"),          USoundNodeAttenuation::StaticClass());
		Registry.Add(TEXT("Modulator"),            USoundNodeModulator::StaticClass());
		Registry.Add(TEXT("Concatenator"),         USoundNodeConcatenator::StaticClass());
		Registry.Add(TEXT("Looping"),              USoundNodeLooping::StaticClass());
		Registry.Add(TEXT("Branch"),               USoundNodeBranch::StaticClass());
		Registry.Add(TEXT("Switch"),               USoundNodeSwitch::StaticClass());
		Registry.Add(TEXT("DistanceCrossFade"),    USoundNodeDistanceCrossFade::StaticClass());
		Registry.Add(TEXT("ParamCrossFade"),        USoundNodeParamCrossFade::StaticClass());
		Registry.Add(TEXT("Enveloper"),            USoundNodeEnveloper::StaticClass());
		Registry.Add(TEXT("Oscillator"),           USoundNodeOscillator::StaticClass());
		Registry.Add(TEXT("Doppler"),              USoundNodeDoppler::StaticClass());
		Registry.Add(TEXT("SoundClass"),           USoundNodeSoundClass::StaticClass());
		Registry.Add(TEXT("Mature"),               USoundNodeMature::StaticClass());
		Registry.Add(TEXT("QualityLevel"),         USoundNodeQualityLevel::StaticClass());
		Registry.Add(TEXT("GroupControl"),          USoundNodeGroupControl::StaticClass());
		Registry.Add(TEXT("WaveParam"),            USoundNodeWaveParam::StaticClass());
		Registry.Add(TEXT("DialoguePlayer"),       USoundNodeDialoguePlayer::StaticClass());
		Registry.Add(TEXT("ModulatorContinuous"),  USoundNodeModulatorContinuous::StaticClass());
	}
	return Registry;
}

UClass* FMonolithAudioSoundCueActions::ResolveNodeType(const FString& TypeName)
{
	const TMap<FString, UClass*>& Reg = GetNodeTypeRegistry();
	const UClass* const* Found = Reg.Find(TypeName);
	return Found ? const_cast<UClass*>(*Found) : nullptr;
}

// ============================================================================
// Helpers
// ============================================================================

USoundCue* FMonolithAudioSoundCueActions::LoadSoundCue(const FString& AssetPath, FString& OutError)
{
	// Normalize short paths: /Game/Foo/Bar -> /Game/Foo/Bar.Bar
	FString NormalizedPath = AssetPath;
	if (!NormalizedPath.Contains(TEXT(".")))
	{
		int32 LastSlash;
		if (NormalizedPath.FindLastChar('/', LastSlash) && LastSlash >= 0)
		{
			FString AssetName = NormalizedPath.Mid(LastSlash + 1);
			if (!AssetName.IsEmpty())
			{
				NormalizedPath = NormalizedPath + TEXT(".") + AssetName;
			}
		}
	}

	IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(NormalizedPath));
	if (AssetData.IsValid())
	{
		UObject* Loaded = AssetData.GetAsset();
		USoundCue* Cue = Cast<USoundCue>(Loaded);
		if (!Cue)
		{
			OutError = FString::Printf(TEXT("Asset at '%s' is not a USoundCue (found %s)"),
				*AssetPath, Loaded ? *Loaded->GetClass()->GetName() : TEXT("null"));
		}
		return Cue;
	}

	UObject* Loaded = StaticLoadObject(USoundCue::StaticClass(), nullptr, *AssetPath);
	if (!Loaded)
	{
		OutError = FString::Printf(TEXT("Sound Cue not found at '%s'"), *AssetPath);
		return nullptr;
	}
	USoundCue* Cue = Cast<USoundCue>(Loaded);
	if (!Cue)
	{
		OutError = FString::Printf(TEXT("Asset at '%s' is not a USoundCue"), *AssetPath);
	}
	return Cue;
}

FString FMonolithAudioSoundCueActions::MakeNodeId(USoundCue* Cue, USoundNode* Node)
{
	if (!Cue || !Node) return TEXT("");

#if WITH_EDITORONLY_DATA
	int32 Idx = Cue->AllNodes.IndexOfByKey(Node);
	if (Idx != INDEX_NONE)
	{
		return FString::Printf(TEXT("%s_%d"), *Node->GetClass()->GetName(), Idx);
	}
#endif

	return FString::Printf(TEXT("%s_%p"), *Node->GetClass()->GetName(), Node);
}

USoundNode* FMonolithAudioSoundCueActions::FindNodeById(USoundCue* Cue, const FString& NodeId)
{
	if (!Cue || NodeId.IsEmpty()) return nullptr;

#if WITH_EDITORONLY_DATA
	// Try to parse "ClassName_Index" format
	int32 UnderscoreIdx;
	if (NodeId.FindLastChar('_', UnderscoreIdx) && UnderscoreIdx > 0)
	{
		FString IndexStr = NodeId.Mid(UnderscoreIdx + 1);
		if (IndexStr.IsNumeric())
		{
			int32 Idx = FCString::Atoi(*IndexStr);
			if (Cue->AllNodes.IsValidIndex(Idx))
			{
				return Cue->AllNodes[Idx];
			}
		}
	}

	// Fallback: search by name match
	for (USoundNode* Node : Cue->AllNodes)
	{
		if (Node && MakeNodeId(Cue, Node) == NodeId)
		{
			return Node;
		}
	}
#endif

	return nullptr;
}

TSharedPtr<FJsonObject> FMonolithAudioSoundCueActions::SerializeNode(USoundCue* Cue, USoundNode* Node)
{
	auto Json = MakeShared<FJsonObject>();
	if (!Node) return Json;

	Json->SetStringField(TEXT("node_id"), MakeNodeId(Cue, Node));
	Json->SetStringField(TEXT("type"), Node->GetClass()->GetName());

	// Serialize known properties via reflection
	auto PropsJson = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> PropIt(Node->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
		{
			continue;
		}

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Node);
		const FString PropName = Prop->GetName();

		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			PropsJson->SetBoolField(PropName, BoolProp->GetPropertyValue(ValuePtr));
		}
		else if (const FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{
			PropsJson->SetNumberField(PropName, FloatProp->GetPropertyValue(ValuePtr));
		}
		else if (const FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{
			PropsJson->SetNumberField(PropName, DoubleProp->GetPropertyValue(ValuePtr));
		}
		else if (const FIntProperty* IntProp = CastField<FIntProperty>(Prop))
		{
			PropsJson->SetNumberField(PropName, static_cast<double>(IntProp->GetPropertyValue(ValuePtr)));
		}
		else if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			PropsJson->SetStringField(PropName, StrProp->GetPropertyValue(ValuePtr));
		}
		else if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			PropsJson->SetStringField(PropName, NameProp->GetPropertyValue(ValuePtr).ToString());
		}
		else if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			UObject* Obj = ObjProp->GetObjectPropertyValue(ValuePtr);
			PropsJson->SetStringField(PropName, Obj ? Obj->GetPathName() : TEXT("None"));
		}
		else if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			const UEnum* Enum = EnumProp->GetEnum();
			FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
			int64 Val = UnderlyingProp->GetSignedIntPropertyValue(ValuePtr);
			PropsJson->SetStringField(PropName, Enum ? Enum->GetNameStringByValue(Val) : FString::FromInt(Val));
		}
		else if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (ByteProp->Enum)
			{
				uint8 Val = ByteProp->GetPropertyValue(ValuePtr);
				PropsJson->SetStringField(PropName, ByteProp->Enum->GetNameStringByValue(Val));
			}
			else
			{
				PropsJson->SetNumberField(PropName, static_cast<double>(ByteProp->GetPropertyValue(ValuePtr)));
			}
		}
		else if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
		{
			TArray<TSharedPtr<FJsonValue>> JsonArray;
			FScriptArrayHelper ArrayHelper(ArrayProp, ValuePtr);
			for (int32 i = 0; i < ArrayHelper.Num(); ++i)
			{
				const void* ElemPtr = ArrayHelper.GetRawPtr(i);
				FProperty* InnerProp = ArrayProp->Inner;

				if (const FFloatProperty* InnerFloat = CastField<FFloatProperty>(InnerProp))
				{
					JsonArray.Add(MakeShared<FJsonValueNumber>(InnerFloat->GetPropertyValue(ElemPtr)));
				}
				else if (const FIntProperty* InnerInt = CastField<FIntProperty>(InnerProp))
				{
					JsonArray.Add(MakeShared<FJsonValueNumber>(static_cast<double>(InnerInt->GetPropertyValue(ElemPtr))));
				}
				else
				{
					FString ExportStr;
					InnerProp->ExportTextItem_Direct(ExportStr, ElemPtr, nullptr, nullptr, PPF_None);
					JsonArray.Add(MakeShared<FJsonValueString>(ExportStr));
				}
			}
			PropsJson->SetArrayField(PropName, JsonArray);
		}
	}

	// Special case: WavePlayer -> show SoundWave path
	if (USoundNodeWavePlayer* WavePlayer = Cast<USoundNodeWavePlayer>(Node))
	{
		USoundWave* Wave = WavePlayer->GetSoundWave();
		PropsJson->SetStringField(TEXT("SoundWave"), Wave ? Wave->GetPathName() : TEXT("None"));
	}

	Json->SetObjectField(TEXT("properties"), PropsJson);

	// Child connections
	TArray<TSharedPtr<FJsonValue>> ChildArray;
	for (int32 i = 0; i < Node->ChildNodes.Num(); ++i)
	{
		USoundNode* Child = Node->ChildNodes[i];
		if (Child)
		{
			auto ChildJson = MakeShared<FJsonObject>();
			ChildJson->SetNumberField(TEXT("child_index"), i);
			ChildJson->SetStringField(TEXT("node_id"), MakeNodeId(Cue, Child));
			ChildArray.Add(MakeShared<FJsonValueObject>(ChildJson));
		}
	}
	Json->SetArrayField(TEXT("children"), ChildArray);

	return Json;
}

bool FMonolithAudioSoundCueActions::SetNodeProperty(USoundNode* Node, const FString& PropName, const TSharedPtr<FJsonValue>& Value, FString& OutError)
{
	if (!Node || !Value.IsValid())
	{
		OutError = TEXT("Null node or value");
		return false;
	}

	// Special case: SoundWave on WavePlayer
	if (PropName == TEXT("SoundWave"))
	{
		USoundNodeWavePlayer* WavePlayer = Cast<USoundNodeWavePlayer>(Node);
		if (!WavePlayer)
		{
			OutError = TEXT("SoundWave property is only valid on WavePlayer nodes");
			return false;
		}
		FString WavePath;
		if (!Value->TryGetString(WavePath))
		{
			OutError = TEXT("SoundWave value must be a string path");
			return false;
		}
		USoundWave* Wave = Cast<USoundWave>(StaticLoadObject(USoundWave::StaticClass(), nullptr, *WavePath));
		if (!Wave)
		{
			OutError = FString::Printf(TEXT("Could not load SoundWave at '%s'"), *WavePath);
			return false;
		}
		WavePlayer->SetSoundWave(Wave);
		return true;
	}

	// Generic reflection-based property set
	FProperty* Prop = Node->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop)
	{
		OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *PropName, *Node->GetClass()->GetName());
		return false;
	}

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Node);

	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
	{
		bool bVal;
		if (Value->TryGetBool(bVal))
		{
			BoolProp->SetPropertyValue(ValuePtr, bVal);
			return true;
		}
	}
	else if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
	{
		double DVal;
		if (Value->TryGetNumber(DVal))
		{
			FloatProp->SetPropertyValue(ValuePtr, static_cast<float>(DVal));
			return true;
		}
	}
	else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
	{
		double DVal;
		if (Value->TryGetNumber(DVal))
		{
			DoubleProp->SetPropertyValue(ValuePtr, DVal);
			return true;
		}
	}
	else if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
	{
		double DVal;
		if (Value->TryGetNumber(DVal))
		{
			IntProp->SetPropertyValue(ValuePtr, static_cast<int32>(DVal));
			return true;
		}
	}
	else if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
	{
		FString SVal;
		if (Value->TryGetString(SVal))
		{
			StrProp->SetPropertyValue(ValuePtr, SVal);
			return true;
		}
	}
	else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
	{
		FString SVal;
		if (Value->TryGetString(SVal))
		{
			NameProp->SetPropertyValue(ValuePtr, FName(*SVal));
			return true;
		}
	}
	else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		FString SVal;
		if (Value->TryGetString(SVal))
		{
			const UEnum* Enum = EnumProp->GetEnum();
			int64 EnumVal = Enum ? Enum->GetValueByNameString(SVal) : INDEX_NONE;
			if (EnumVal != INDEX_NONE)
			{
				FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
				UnderlyingProp->SetIntPropertyValue(ValuePtr, EnumVal);
				return true;
			}
			OutError = FString::Printf(TEXT("Unknown enum value '%s' for '%s'"), *SVal, *PropName);
			return false;
		}
	}
	else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
	{
		if (ByteProp->Enum)
		{
			FString SVal;
			if (Value->TryGetString(SVal))
			{
				int64 EnumVal = ByteProp->Enum->GetValueByNameString(SVal);
				if (EnumVal != INDEX_NONE)
				{
					ByteProp->SetPropertyValue(ValuePtr, static_cast<uint8>(EnumVal));
					return true;
				}
				OutError = FString::Printf(TEXT("Unknown enum value '%s' for '%s'"), *SVal, *PropName);
				return false;
			}
		}
		else
		{
			double DVal;
			if (Value->TryGetNumber(DVal))
			{
				ByteProp->SetPropertyValue(ValuePtr, static_cast<uint8>(DVal));
				return true;
			}
		}
	}
	else if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
	{
		FString SVal;
		if (Value->TryGetString(SVal))
		{
			if (SVal == TEXT("None") || SVal.IsEmpty())
			{
				ObjProp->SetObjectPropertyValue(ValuePtr, nullptr);
				return true;
			}
			UObject* Loaded = StaticLoadObject(ObjProp->PropertyClass, nullptr, *SVal);
			if (Loaded)
			{
				ObjProp->SetObjectPropertyValue(ValuePtr, Loaded);
				return true;
			}
			OutError = FString::Printf(TEXT("Could not load object '%s' for '%s'"), *SVal, *PropName);
			return false;
		}
	}
	else if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		const TArray<TSharedPtr<FJsonValue>>* JsonArray;
		if (Value->TryGetArray(JsonArray))
		{
			FScriptArrayHelper ArrayHelper(ArrayProp, ValuePtr);
			ArrayHelper.Resize(JsonArray->Num());
			FProperty* InnerProp = ArrayProp->Inner;

			for (int32 i = 0; i < JsonArray->Num(); ++i)
			{
				void* ElemPtr = ArrayHelper.GetRawPtr(i);
				const TSharedPtr<FJsonValue>& ElemVal = (*JsonArray)[i];

				if (FFloatProperty* InnerFloat = CastField<FFloatProperty>(InnerProp))
				{
					double DVal;
					if (ElemVal->TryGetNumber(DVal))
					{
						InnerFloat->SetPropertyValue(ElemPtr, static_cast<float>(DVal));
					}
				}
				else if (FIntProperty* InnerInt = CastField<FIntProperty>(InnerProp))
				{
					double DVal;
					if (ElemVal->TryGetNumber(DVal))
					{
						InnerInt->SetPropertyValue(ElemPtr, static_cast<int32>(DVal));
					}
				}
				else if (FBoolProperty* InnerBool = CastField<FBoolProperty>(InnerProp))
				{
					bool bVal;
					if (ElemVal->TryGetBool(bVal))
					{
						InnerBool->SetPropertyValue(ElemPtr, bVal);
					}
				}
				else
				{
					FString SVal;
					if (ElemVal->TryGetString(SVal))
					{
						InnerProp->ImportText_Direct(*SVal, ElemPtr, nullptr, PPF_None);
					}
				}
			}
			return true;
		}
	}

	OutError = FString::Printf(TEXT("Could not set property '%s' — type mismatch or unsupported property type"), *PropName);
	return false;
}

// Restores the editor-only Sound Cue graph that the AudioEditor graph helpers assume is present.
//
// A cue that is loaded without its asset editor can carry a null SoundCueGraph - the engine only
// ensure()s on that state (Engine/Private/SoundCue.cpp, USoundCue::PostLoad) and skips its own
// graph work - and sound nodes built while the graph is missing keep a null GraphNode back
// pointer. Every AudioEditor entry point we drive CastChecks that back pointer:
// FSoundCueAudioEditor::LinkGraphNodesFromSoundNodes and ::CreateInputPin (reached from
// USoundNode::InsertChildNode) both live in Editor/AudioEditor/Private/SoundCueGraph.cpp and use
// CastChecked, which is a fatal assert rather than a soft failure.
//
// Rebuilding here is cheap and idempotent: CreateGraph() is a no-op when a graph already exists,
// and SetupSoundNode() only runs for nodes that have no graph node yet (it check()s that itself).
// Repaired nodes land at the graph origin; the cue editor can lay them out again.
bool FMonolithAudioSoundCueActions::EnsureSoundCueGraph(USoundCue* Cue)
{
	if (!Cue || !USoundCue::GetSoundCueAudioEditor().IsValid())
	{
		return false;
	}

	if (!Cue->GetGraph())
	{
		Cue->CreateGraph();
		if (!Cue->GetGraph())
		{
			return false;
		}
		UE_LOG(LogMonolith, Warning, TEXT("Sound Cue '%s' had no editor graph; rebuilt it before editing."), *Cue->GetPathName());
	}

#if WITH_EDITORONLY_DATA
	// LinkGraphNodesFromSoundNodes walks AllNodes, FirstNode and every ChildNodes entry, and
	// CastChecks the graph node of each - collect all three sets, not just AllNodes.
	TSet<USoundNode*> NodesToRepair;
	for (USoundNode* Node : Cue->AllNodes)
	{
		if (Node)
		{
			NodesToRepair.Add(Node);
			for (USoundNode* Child : Node->ChildNodes)
			{
				if (Child)
				{
					NodesToRepair.Add(Child);
				}
			}
		}
	}
	if (Cue->FirstNode)
	{
		NodesToRepair.Add(Cue->FirstNode.Get());
	}

	for (USoundNode* Node : NodesToRepair)
	{
		if (!Node->GetGraphNode())
		{
			Cue->SetupSoundNode(Node, /*bSelectNewNode=*/false);
		}
	}
#endif

	return true;
}

// Mirrors every fatal precondition of FSoundCueAudioEditor::LinkGraphNodesFromSoundNodes
// (Editor/AudioEditor/Private/SoundCueGraph.cpp): a graph, exactly one root node with a pin, a
// USoundCueGraphNode for every sound node it walks, and one input pin per child slot.
bool FMonolithAudioSoundCueActions::CanLinkGraphNodes(USoundCue* Cue)
{
	if (!Cue) return false;

	UEdGraph* Graph = Cue->GetGraph();
	if (!Graph) return false;

#if WITH_EDITORONLY_DATA
	if (Cue->FirstNode)
	{
		TArray<USoundCueGraphNode_Root*> RootNodes;
		Graph->GetNodesOfClass<USoundCueGraphNode_Root>(RootNodes);
		if (RootNodes.Num() != 1 || RootNodes[0]->Pins.Num() == 0)
		{
			return false;
		}

		USoundCueGraphNode* FirstGraphNode = Cast<USoundCueGraphNode>(Cue->FirstNode->GetGraphNode());
		if (!FirstGraphNode || !FirstGraphNode->GetOutputPin())
		{
			return false;
		}
	}

	for (USoundNode* Node : Cue->AllNodes)
	{
		if (!Node) continue;

		USoundCueGraphNode* GraphNode = Cast<USoundCueGraphNode>(Node->GetGraphNode());
		if (!GraphNode) return false;

		TArray<UEdGraphPin*> InputPins;
		GraphNode->GetInputPins(InputPins);
		if (InputPins.Num() != Node->ChildNodes.Num()) return false;

		for (USoundNode* Child : Node->ChildNodes)
		{
			if (!Child) continue;
			USoundCueGraphNode* ChildGraphNode = Cast<USoundCueGraphNode>(Child->GetGraphNode());
			if (!ChildGraphNode || !ChildGraphNode->GetOutputPin()) return false;
		}
	}
#endif

	return true;
}

void FMonolithAudioSoundCueActions::FinalizeCue(USoundCue* Cue, bool* bOutGraphLinkSkipped)
{
	if (bOutGraphLinkSkipped)
	{
		*bOutGraphLinkSkipped = false;
	}
	if (!Cue) return;

	// Repair first, then verify. If the graph still cannot be linked safely, skip the link
	// instead of asserting inside the engine: the runtime chain (FirstNode -> ChildNodes) is
	// already correct, so the cue plays back as authored.
	EnsureSoundCueGraph(Cue);
	if (CanLinkGraphNodes(Cue))
	{
		Cue->LinkGraphNodesFromSoundNodes();
	}
	else
	{
		if (bOutGraphLinkSkipped)
		{
			*bOutGraphLinkSkipped = true;
		}
		UE_LOG(LogMonolith, Warning,
			TEXT("Sound Cue '%s': skipped the editor graph link (no usable Sound Cue graph). The sound node chain was still updated."),
			*Cue->GetPathName());
	}

	Cue->CacheAggregateValues();
	Cue->GetPackage()->MarkPackageDirty();
}

USoundCue* FMonolithAudioSoundCueActions::CreateEmptySoundCue(const FString& AssetPath, FString& OutError)
{
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash <= 0)
	{
		OutError = TEXT("Invalid asset path — must contain at least one '/' (e.g. /Game/Audio/SC_MyCue)");
		return nullptr;
	}
	FString PackagePath = AssetPath.Left(LastSlash);
	FString AssetName = AssetPath.Mid(LastSlash + 1);
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("Asset name is empty");
		return nullptr;
	}

	// Check if already exists
	UObject* Existing = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (Existing)
	{
		OutError = FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath);
		return nullptr;
	}

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg)
	{
		OutError = FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath);
		return nullptr;
	}

	USoundCueFactoryNew* Factory = NewObject<USoundCueFactoryNew>();
	UObject* NewObj = Factory->FactoryCreateNew(
		USoundCue::StaticClass(), Pkg, FName(*AssetName),
		RF_Public | RF_Standalone, nullptr, GWarn);

	USoundCue* Cue = Cast<USoundCue>(NewObj);
	if (!Cue)
	{
		OutError = TEXT("SoundCueFactoryNew failed to create asset");
		return nullptr;
	}

	FAssetRegistryModule::AssetCreated(Cue);
	Pkg->MarkPackageDirty();

	// Save to disk
	FString PackageFilename = FPackageName::LongPackageNameToFilename(AssetPath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Pkg, Cue, *PackageFilename, SaveArgs);

	return Cue;
}

TArray<USoundNode*> FMonolithAudioSoundCueActions::CreateWavePlayerNodes(USoundCue* Cue, const TArray<FString>& WavePaths, FString& OutError)
{
	TArray<USoundNode*> Nodes;
	for (const FString& WavePath : WavePaths)
	{
		USoundWave* Wave = Cast<USoundWave>(StaticLoadObject(USoundWave::StaticClass(), nullptr, *WavePath));
		if (!Wave)
		{
			OutError = FString::Printf(TEXT("Could not load SoundWave at '%s'"), *WavePath);
			return TArray<USoundNode*>();
		}

		USoundNodeWavePlayer* WavePlayer = Cue->ConstructSoundNode<USoundNodeWavePlayer>();
		if (!WavePlayer)
		{
			OutError = TEXT("Failed to construct WavePlayer node");
			return TArray<USoundNode*>();
		}
		WavePlayer->SetSoundWave(Wave);
		Nodes.Add(WavePlayer);
	}
	return Nodes;
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithAudioSoundCueActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// --- CRUD ---

	Registry.RegisterAction(TEXT("audio"), TEXT("create_sound_cue"),
		TEXT("Create a new USoundCue asset. Optionally provide sound_waves[] to auto-create WavePlayers (+ Random node if multiple)."),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::CreateSoundCue),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path (e.g. /Game/Audio/SC_MyCue)"))
			.Optional(TEXT("sound_waves"), TEXT("array"), TEXT("Array of SoundWave asset paths to auto-populate"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("get_sound_cue_graph"),
		TEXT("Get the full Sound Cue graph as JSON: nodes[], connections[], first_node, properties"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::GetSoundCueGraph),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundCue"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("add_sound_cue_node"),
		TEXT("Add a sound node to a Sound Cue. Returns the auto-assigned node_id."),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::AddSoundCueNode),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundCue"))
			.Required(TEXT("node_type"), TEXT("string"), TEXT("Node type name (e.g. WavePlayer, Random, Mixer, Delay, Modulator, etc.)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("Property values to set on the new node"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("remove_sound_cue_node"),
		TEXT("Remove a sound node from a Sound Cue by node_id"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::RemoveSoundCueNode),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundCue"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Node ID to remove"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("connect_sound_cue_nodes"),
		TEXT("Connect two sound nodes: from_node becomes a child of to_node at child_index"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::ConnectSoundCueNodes),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundCue"))
			.Required(TEXT("from_node_id"), TEXT("string"), TEXT("Source node (child) ID"))
			.Required(TEXT("to_node_id"), TEXT("string"), TEXT("Destination node (parent) ID"))
			.Optional(TEXT("child_index"), TEXT("number"), TEXT("Child slot index on the parent (auto-appends if omitted)"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("set_sound_cue_first_node"),
		TEXT("Set the FirstNode (root) of a Sound Cue"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::SetSoundCueFirstNode),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundCue"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Node ID to set as FirstNode"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("set_sound_cue_node_property"),
		TEXT("Set a property on a sound node via reflection. Special case: SoundWave on WavePlayer uses SetSoundWave()."),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::SetSoundCueNodeProperty),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundCue"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Node ID"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Property name"))
			.Required(TEXT("value"), TEXT("any"), TEXT("Property value"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("list_sound_cue_node_types"),
		TEXT("List all 22 supported Sound Cue node types with max_children and description"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::ListSoundCueNodeTypes),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("find_sound_waves_in_cue"),
		TEXT("Find all SoundWave references in a Sound Cue (WavePlayer nodes)"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::FindSoundWavesInCue),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundCue"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("validate_sound_cue"),
		TEXT("Validate a Sound Cue: check null FirstNode, disconnected nodes, missing SoundWaves"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::ValidateSoundCue),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundCue"))
			.Build());

	// --- Build & Templates ---

	Registry.RegisterAction(TEXT("audio"), TEXT("build_sound_cue_from_spec"),
		TEXT("Build a complete Sound Cue from a JSON spec: nodes, connections, first_node, properties. The crown jewel."),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::BuildSoundCueFromSpec),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new Sound Cue"))
			.Required(TEXT("spec"), TEXT("object"), TEXT("JSON spec: {nodes: [{id, type, properties?}], connections: [{from, to, child_index?}], first_node, properties?}"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("create_random_sound_cue"),
		TEXT("Create a Sound Cue with N WavePlayers feeding a Random node, with optional weights"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::CreateRandomSoundCue),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new Sound Cue"))
			.Required(TEXT("sound_waves"), TEXT("array"), TEXT("Array of SoundWave asset paths"))
			.Optional(TEXT("weights"), TEXT("array"), TEXT("Array of float weights (same length as sound_waves)"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("create_layered_sound_cue"),
		TEXT("Create a Sound Cue with N WavePlayers feeding a Mixer node, with optional per-input volumes"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::CreateLayeredSoundCue),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new Sound Cue"))
			.Required(TEXT("sound_waves"), TEXT("array"), TEXT("Array of SoundWave asset paths"))
			.Optional(TEXT("volumes"), TEXT("array"), TEXT("Array of float volumes per input (same length as sound_waves)"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("create_looping_ambient_cue"),
		TEXT("Create a looping ambient Sound Cue: Looping -> Delay -> Random -> WavePlayers"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::CreateLoopingAmbientCue),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new Sound Cue"))
			.Required(TEXT("sound_waves"), TEXT("array"), TEXT("Array of SoundWave asset paths"))
			.Optional(TEXT("delay_min"), TEXT("number"), TEXT("Minimum delay between loops (default 0.1)"))
			.Optional(TEXT("delay_max"), TEXT("number"), TEXT("Maximum delay between loops (default 1.0)"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("create_distance_crossfade_cue"),
		TEXT("Create a Sound Cue with a DistanceCrossFade node and N distance bands"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::CreateDistanceCrossfadeCue),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new Sound Cue"))
			.Required(TEXT("bands"), TEXT("array"), TEXT("Array of {sound_wave, fade_in_distance_start, fade_in_distance_end, fade_out_distance_start, fade_out_distance_end, volume}"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("create_switch_sound_cue"),
		TEXT("Create a Sound Cue with a Switch node and N WavePlayer variants"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::CreateSwitchSoundCue),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new Sound Cue"))
			.Required(TEXT("parameter_name"), TEXT("string"), TEXT("Switch parameter name (FName)"))
			.Required(TEXT("sound_waves"), TEXT("array"), TEXT("Array of SoundWave asset paths — one per switch value"))
			.Build());

	// --- Utility ---

	Registry.RegisterAction(TEXT("audio"), TEXT("duplicate_sound_cue"),
		TEXT("Duplicate a Sound Cue to a new path"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::DuplicateSoundCue),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("source_path"), TEXT("Source Sound Cue asset path"))
			.RequiredAssetPath(TEXT("dest_path"), TEXT("Destination asset path"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("delete_audio_asset"),
		TEXT("Delete any audio asset (SoundCue, SoundWave, SoundAttenuation, etc.)"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::DeleteAudioAsset),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path to delete"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("preview_sound"),
		TEXT("Preview a sound asset in the editor (plays via GEditor)"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::PreviewSound),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of any USoundBase"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("stop_preview"),
		TEXT("Stop any currently playing preview sound"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::StopPreview),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("get_sound_cue_duration"),
		TEXT("Get the cached duration of a Sound Cue"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioSoundCueActions::GetSoundCueDuration),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the USoundCue"))
			.Build());
}

// ============================================================================
// CRUD Actions
// ============================================================================

FMonolithActionResult FMonolithAudioSoundCueActions::CreateSoundCue(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	USoundCue* Cue = CreateEmptySoundCue(AssetPath, Error);
	if (!Cue)
	{
		return FMonolithActionResult::Error(Error);
	}

	bool bGraphLinkSkipped = false;

	// Optional: auto-create WavePlayers from sound_waves array
	const TArray<TSharedPtr<FJsonValue>>* WavesArray = nullptr;
	if (Params->TryGetArrayField(TEXT("sound_waves"), WavesArray) && WavesArray && WavesArray->Num() > 0)
	{
		TArray<FString> WavePaths;
		for (const auto& Val : *WavesArray)
		{
			FString Path;
			if (Val->TryGetString(Path))
			{
				WavePaths.Add(Path);
			}
		}

		TArray<USoundNode*> WaveNodes = CreateWavePlayerNodes(Cue, WavePaths, Error);
		if (WaveNodes.Num() == 0 && !Error.IsEmpty())
		{
			return FMonolithActionResult::Error(Error);
		}

		if (WaveNodes.Num() == 1)
		{
			// Single wave: set as FirstNode directly
			Cue->FirstNode = WaveNodes[0];
		}
		else if (WaveNodes.Num() > 1)
		{
			// Multiple waves: create Random node as root
			USoundNodeRandom* RandomNode = Cue->ConstructSoundNode<USoundNodeRandom>();
			if (RandomNode)
			{
				// Use InsertChildNode to create graph-pin-synced child slots, then assign
				for (int32 i = 0; i < WaveNodes.Num(); ++i)
				{
					RandomNode->InsertChildNode(i);
					RandomNode->ChildNodes[i] = WaveNodes[i];
				}
				Cue->FirstNode = RandomNode;
			}
		}

		FinalizeCue(Cue, &bGraphLinkSkipped);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Cue->GetPathName());

#if WITH_EDITORONLY_DATA
	Result->SetNumberField(TEXT("node_count"), Cue->AllNodes.Num());
#endif

	if (bGraphLinkSkipped)
	{
		Result->SetBoolField(TEXT("graph_link_skipped"), true);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::GetSoundCueGraph(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	USoundCue* Cue = LoadSoundCue(AssetPath, Error);
	if (!Cue)
	{
		return FMonolithActionResult::Error(Error);
	}

	auto Result = MakeShared<FJsonObject>();

	// Nodes
	TArray<TSharedPtr<FJsonValue>> NodesArray;
#if WITH_EDITORONLY_DATA
	for (USoundNode* Node : Cue->AllNodes)
	{
		if (Node)
		{
			NodesArray.Add(MakeShared<FJsonValueObject>(SerializeNode(Cue, Node)));
		}
	}
#endif
	Result->SetArrayField(TEXT("nodes"), NodesArray);

	// Connections (from -> to with child_index)
	TArray<TSharedPtr<FJsonValue>> ConnsArray;
#if WITH_EDITORONLY_DATA
	for (USoundNode* Node : Cue->AllNodes)
	{
		if (!Node) continue;
		for (int32 i = 0; i < Node->ChildNodes.Num(); ++i)
		{
			USoundNode* Child = Node->ChildNodes[i];
			if (Child)
			{
				auto Conn = MakeShared<FJsonObject>();
				Conn->SetStringField(TEXT("from"), MakeNodeId(Cue, Child));
				Conn->SetStringField(TEXT("to"), MakeNodeId(Cue, Node));
				Conn->SetNumberField(TEXT("child_index"), i);
				ConnsArray.Add(MakeShared<FJsonValueObject>(Conn));
			}
		}
	}
#endif
	Result->SetArrayField(TEXT("connections"), ConnsArray);

	// FirstNode
	Result->SetStringField(TEXT("first_node"),
		Cue->FirstNode ? MakeNodeId(Cue, Cue->FirstNode) : TEXT("None"));

	// Cue-level properties
	auto PropsJson = MakeShared<FJsonObject>();
	PropsJson->SetNumberField(TEXT("VolumeMultiplier"), Cue->VolumeMultiplier);
	PropsJson->SetNumberField(TEXT("PitchMultiplier"), Cue->PitchMultiplier);
	PropsJson->SetBoolField(TEXT("bOverrideAttenuation"), Cue->bOverrideAttenuation);
	PropsJson->SetBoolField(TEXT("bPrimeOnLoad"), Cue->bPrimeOnLoad);
	PropsJson->SetBoolField(TEXT("bExcludeFromRandomNodeBranchCulling"), Cue->bExcludeFromRandomNodeBranchCulling);
	Result->SetObjectField(TEXT("properties"), PropsJson);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::AddSoundCueNode(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString NodeType = Params->GetStringField(TEXT("node_type"));
	if (AssetPath.IsEmpty() || NodeType.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and node_type are required"));
	}

	UClass* NodeClass = ResolveNodeType(NodeType);
	if (!NodeClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown node type '%s'. Use list_sound_cue_node_types for valid types."), *NodeType));
	}

	FString Error;
	USoundCue* Cue = LoadSoundCue(AssetPath, Error);
	if (!Cue)
	{
		return FMonolithActionResult::Error(Error);
	}

	Cue->Modify();

	// ConstructSoundNode -> USoundCue::SetupSoundNode dereferences the cue's editor graph, so the
	// graph has to exist before we build anything (issue #125).
	EnsureSoundCueGraph(Cue);

	USoundNode* NewNode = Cue->ConstructSoundNode<USoundNode>(NodeClass);
	if (!NewNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to construct node of type '%s'"), *NodeType));
	}

	// Apply optional properties
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && PropsObj->IsValid())
	{
		for (const auto& Pair : (*PropsObj)->Values)
		{
			FString PropError;
			const FString PairKeyStr = MonolithKeyToString(Pair.Key);
			if (!SetNodeProperty(NewNode, PairKeyStr, Pair.Value, PropError))
			{
				UE_LOG(LogMonolith, Warning, TEXT("add_sound_cue_node: property '%s' error: %s"), *PairKeyStr, *PropError);
			}
		}
	}

	bool bGraphLinkSkipped = false;
	FinalizeCue(Cue, &bGraphLinkSkipped);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), MakeNodeId(Cue, NewNode));
	Result->SetStringField(TEXT("type"), NodeType);
	Result->SetObjectField(TEXT("properties"), SerializeNode(Cue, NewNode)->GetObjectField(TEXT("properties")));
	if (bGraphLinkSkipped)
	{
		Result->SetBoolField(TEXT("graph_link_skipped"), true);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::RemoveSoundCueNode(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString NodeId = Params->GetStringField(TEXT("node_id"));
	if (AssetPath.IsEmpty() || NodeId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and node_id are required"));
	}

	FString Error;
	USoundCue* Cue = LoadSoundCue(AssetPath, Error);
	if (!Cue)
	{
		return FMonolithActionResult::Error(Error);
	}

	USoundNode* NodeToRemove = FindNodeById(Cue, NodeId);
	if (!NodeToRemove)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Node '%s' not found in cue"), *NodeId));
	}

	Cue->Modify();

	// Clear FirstNode if it's the one being removed
	if (Cue->FirstNode == NodeToRemove)
	{
		Cue->FirstNode = nullptr;
	}

	// Null out any ChildNode references to this node
#if WITH_EDITORONLY_DATA
	for (USoundNode* Node : Cue->AllNodes)
	{
		if (!Node) continue;
		for (int32 i = 0; i < Node->ChildNodes.Num(); ++i)
		{
			if (Node->ChildNodes[i] == NodeToRemove)
			{
				Node->ChildNodes[i] = nullptr;
			}
		}
	}

	// Remove from AllNodes
	Cue->AllNodes.Remove(NodeToRemove);
#endif

	bool bGraphLinkSkipped = false;
	FinalizeCue(Cue, &bGraphLinkSkipped);

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	if (bGraphLinkSkipped)
	{
		Result->SetBoolField(TEXT("graph_link_skipped"), true);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::ConnectSoundCueNodes(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString FromNodeId = Params->GetStringField(TEXT("from_node_id"));
	const FString ToNodeId = Params->GetStringField(TEXT("to_node_id"));
	if (AssetPath.IsEmpty() || FromNodeId.IsEmpty() || ToNodeId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path, from_node_id, and to_node_id are required"));
	}

	FString Error;
	USoundCue* Cue = LoadSoundCue(AssetPath, Error);
	if (!Cue)
	{
		return FMonolithActionResult::Error(Error);
	}

	USoundNode* FromNode = FindNodeById(Cue, FromNodeId);
	USoundNode* ToNode = FindNodeById(Cue, ToNodeId);
	if (!FromNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("From node '%s' not found"), *FromNodeId));
	}
	if (!ToNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("To node '%s' not found"), *ToNodeId));
	}

	Cue->Modify();

	// InsertChildNode below routes through FSoundCueAudioEditor::CreateInputPin, which CastChecks
	// the target node's graph node - make sure the graph and its back pointers exist first.
	EnsureSoundCueGraph(Cue);

	// Determine child index
	int32 ChildIndex = -1;
	double IdxVal;
	if (Params->TryGetNumberField(TEXT("child_index"), IdxVal))
	{
		ChildIndex = static_cast<int32>(IdxVal);
	}

	if (ChildIndex < 0)
	{
		// Auto-append: use InsertChildNode to grow the array
		ChildIndex = ToNode->ChildNodes.Num();
		ToNode->InsertChildNode(ChildIndex);
	}
	else
	{
		// Ensure enough slots exist
		while (ToNode->ChildNodes.Num() <= ChildIndex)
		{
			ToNode->InsertChildNode(ToNode->ChildNodes.Num());
		}
	}

	// Check max children
	int32 MaxChildren = ToNode->GetMaxChildNodes();
	if (MaxChildren > 0 && ChildIndex >= MaxChildren)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Child index %d exceeds max children (%d) for %s"),
			ChildIndex, MaxChildren, *ToNode->GetClass()->GetName()));
	}

	ToNode->ChildNodes[ChildIndex] = FromNode;

	bool bGraphLinkSkipped = false;
	FinalizeCue(Cue, &bGraphLinkSkipped);

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("child_index"), ChildIndex);
	if (bGraphLinkSkipped)
	{
		Result->SetBoolField(TEXT("graph_link_skipped"), true);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::SetSoundCueFirstNode(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString NodeId = Params->GetStringField(TEXT("node_id"));
	if (AssetPath.IsEmpty() || NodeId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and node_id are required"));
	}

	FString Error;
	USoundCue* Cue = LoadSoundCue(AssetPath, Error);
	if (!Cue)
	{
		return FMonolithActionResult::Error(Error);
	}

	USoundNode* Node = FindNodeById(Cue, NodeId);
	if (!Node)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Node '%s' not found"), *NodeId));
	}

	Cue->Modify();
	Cue->FirstNode = Node;

	bool bGraphLinkSkipped = false;
	FinalizeCue(Cue, &bGraphLinkSkipped);

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("first_node"), MakeNodeId(Cue, Node));
	if (bGraphLinkSkipped)
	{
		Result->SetBoolField(TEXT("graph_link_skipped"), true);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::SetSoundCueNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString NodeId = Params->GetStringField(TEXT("node_id"));
	const FString PropName = Params->GetStringField(TEXT("property_name"));
	if (AssetPath.IsEmpty() || NodeId.IsEmpty() || PropName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path, node_id, and property_name are required"));
	}

	if (!Params->HasField(TEXT("value")))
	{
		return FMonolithActionResult::Error(TEXT("value is required"));
	}

	FString Error;
	USoundCue* Cue = LoadSoundCue(AssetPath, Error);
	if (!Cue)
	{
		return FMonolithActionResult::Error(Error);
	}

	USoundNode* Node = FindNodeById(Cue, NodeId);
	if (!Node)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Node '%s' not found"), *NodeId));
	}

	Cue->Modify();

	TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
	FString PropError;
	if (!SetNodeProperty(Node, PropName, Value, PropError))
	{
		return FMonolithActionResult::Error(PropError);
	}

	bool bGraphLinkSkipped = false;
	FinalizeCue(Cue, &bGraphLinkSkipped);

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("property_name"), PropName);
	if (bGraphLinkSkipped)
	{
		Result->SetBoolField(TEXT("graph_link_skipped"), true);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::ListSoundCueNodeTypes(const TSharedPtr<FJsonObject>& Params)
{
	struct FNodeTypeInfo
	{
		const TCHAR* Name;
		int32 MaxChildren;
		const TCHAR* Description;
	};

	static const FNodeTypeInfo NodeTypes[] = {
		{ TEXT("WavePlayer"),          0,  TEXT("Plays a SoundWave asset (leaf node)") },
		{ TEXT("Random"),             32,  TEXT("Randomly selects one child node to play, with optional weights") },
		{ TEXT("Mixer"),              32,  TEXT("Plays all child nodes simultaneously, mixed together") },
		{ TEXT("Delay"),               1,  TEXT("Adds a configurable delay before playing child") },
		{ TEXT("Attenuation"),         1,  TEXT("Overrides attenuation settings for child") },
		{ TEXT("Modulator"),           1,  TEXT("Randomizes volume and pitch within min/max ranges") },
		{ TEXT("Concatenator"),       32,  TEXT("Plays child nodes in sequence") },
		{ TEXT("Looping"),             1,  TEXT("Loops the child node (deprecated — prefer bLooping on WavePlayer)") },
		{ TEXT("Branch"),              2,  TEXT("Selects between two children based on a bool parameter") },
		{ TEXT("Switch"),             32,  TEXT("Selects child based on an integer parameter value") },
		{ TEXT("DistanceCrossFade"),  32,  TEXT("Crossfades between children based on listener distance") },
		{ TEXT("ParamCrossFade"),     32,  TEXT("Crossfades between children based on a float parameter") },
		{ TEXT("Enveloper"),           1,  TEXT("Applies volume/pitch envelope curves over time") },
		{ TEXT("Oscillator"),          1,  TEXT("Modulates volume/pitch with sine/square/triangle/saw waveforms") },
		{ TEXT("Doppler"),             1,  TEXT("Applies Doppler pitch shift based on relative velocity") },
		{ TEXT("SoundClass"),          1,  TEXT("Overrides the SoundClass for the child subtree") },
		{ TEXT("Mature"),              2,  TEXT("Selects between mature/clean versions based on setting") },
		{ TEXT("QualityLevel"),       -1,  TEXT("Selects child based on audio quality level setting") },
		{ TEXT("GroupControl"),       32,  TEXT("Limits concurrent playback of sounds in this group") },
		{ TEXT("WaveParam"),           1,  TEXT("Plays a SoundWave selected by parameter name at runtime") },
		{ TEXT("DialoguePlayer"),      0,  TEXT("Plays a DialogueWave asset (leaf node)") },
		{ TEXT("ModulatorContinuous"), 1,  TEXT("Continuously modulates volume/pitch via parameters") },
	};

	TArray<TSharedPtr<FJsonValue>> TypesArray;
	for (const auto& Info : NodeTypes)
	{
		auto TypeJson = MakeShared<FJsonObject>();
		TypeJson->SetStringField(TEXT("type_name"), Info.Name);
		TypeJson->SetNumberField(TEXT("max_children"), Info.MaxChildren);
		TypeJson->SetStringField(TEXT("description"), Info.Description);
		TypesArray.Add(MakeShared<FJsonValueObject>(TypeJson));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("node_types"), TypesArray);
	Result->SetNumberField(TEXT("count"), UE_ARRAY_COUNT(NodeTypes));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::FindSoundWavesInCue(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	USoundCue* Cue = LoadSoundCue(AssetPath, Error);
	if (!Cue)
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> WavesArray;

#if WITH_EDITORONLY_DATA
	for (USoundNode* Node : Cue->AllNodes)
	{
		USoundNodeWavePlayer* WavePlayer = Cast<USoundNodeWavePlayer>(Node);
		if (!WavePlayer) continue;

		USoundWave* Wave = WavePlayer->GetSoundWave();
		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("node_id"), MakeNodeId(Cue, WavePlayer));
		Entry->SetStringField(TEXT("sound_wave_path"), Wave ? Wave->GetPathName() : TEXT("None"));
		WavesArray.Add(MakeShared<FJsonValueObject>(Entry));
	}
#endif

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("waves"), WavesArray);
	Result->SetNumberField(TEXT("count"), WavesArray.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::ValidateSoundCue(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	USoundCue* Cue = LoadSoundCue(AssetPath, Error);
	if (!Cue)
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	bool bValid = true;

	// Check null FirstNode
	if (!Cue->FirstNode)
	{
		auto Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), TEXT("error"));
		Issue->SetStringField(TEXT("message"), TEXT("FirstNode is null — Sound Cue will not play"));
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
		bValid = false;
	}

#if WITH_EDITORONLY_DATA
	// Build set of reachable nodes from FirstNode
	TSet<USoundNode*> Reachable;
	TArray<USoundNode*> Stack;
	if (Cue->FirstNode)
	{
		Stack.Add(Cue->FirstNode);
	}
	while (Stack.Num() > 0)
	{
		USoundNode* Current = Stack.Pop();
		if (!Current || Reachable.Contains(Current)) continue;
		Reachable.Add(Current);
		for (USoundNode* Child : Current->ChildNodes)
		{
			if (Child)
			{
				Stack.Add(Child);
			}
		}
	}

	// Check for disconnected nodes
	for (USoundNode* Node : Cue->AllNodes)
	{
		if (!Node) continue;
		if (!Reachable.Contains(Node))
		{
			auto Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("warning"));
			Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("Node '%s' is disconnected (not reachable from FirstNode)"), *MakeNodeId(Cue, Node)));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}
	}

	// Check WavePlayers with missing SoundWaves
	for (USoundNode* Node : Cue->AllNodes)
	{
		USoundNodeWavePlayer* WavePlayer = Cast<USoundNodeWavePlayer>(Node);
		if (!WavePlayer) continue;

		if (!WavePlayer->GetSoundWave())
		{
			auto Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("error"));
			Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("WavePlayer '%s' has no SoundWave assigned"), *MakeNodeId(Cue, WavePlayer)));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
			bValid = false;
		}
	}

	// Check null child slots on non-leaf nodes
	for (USoundNode* Node : Cue->AllNodes)
	{
		if (!Node) continue;
		for (int32 i = 0; i < Node->ChildNodes.Num(); ++i)
		{
			if (!Node->ChildNodes[i])
			{
				auto Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("severity"), TEXT("warning"));
				Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("Node '%s' has null child at index %d"), *MakeNodeId(Cue, Node), i));
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
			}
		}
	}
#endif

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("valid"), bValid);
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Build & Template Actions
// ============================================================================

FMonolithActionResult FMonolithAudioSoundCueActions::BuildSoundCueFromSpec(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("spec"), SpecPtr) || !SpecPtr || !(*SpecPtr).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("spec object is required"));
	}
	const TSharedPtr<FJsonObject>& Spec = *SpecPtr;

	// Create the cue
	FString Error;
	USoundCue* Cue = CreateEmptySoundCue(AssetPath, Error);
	if (!Cue)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Step 1: Create all nodes, map string IDs -> USoundNode*
	TMap<FString, USoundNode*> NodeMap;
	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("nodes"), NodesArray) && NodesArray)
	{
		for (const auto& NodeVal : *NodesArray)
		{
			const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
			if (!NodeVal->TryGetObject(NodeObjPtr) || !NodeObjPtr) continue;
			const TSharedPtr<FJsonObject>& NodeObj = *NodeObjPtr;

			FString Id = NodeObj->GetStringField(TEXT("id"));
			FString TypeName = NodeObj->GetStringField(TEXT("type"));
			if (Id.IsEmpty() || TypeName.IsEmpty())
			{
				return FMonolithActionResult::Error(TEXT("Each node in spec must have 'id' and 'type'"));
			}

			UClass* NodeClass = ResolveNodeType(TypeName);
			if (!NodeClass)
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown node type '%s' for node '%s'"), *TypeName, *Id));
			}

			USoundNode* NewNode = Cue->ConstructSoundNode<USoundNode>(NodeClass);
			if (!NewNode)
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to construct node '%s' of type '%s'"), *Id, *TypeName));
			}

			NodeMap.Add(Id, NewNode);
		}
	}

	// Step 2: Set all node properties
	if (NodesArray)
	{
		for (const auto& NodeVal : *NodesArray)
		{
			const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
			if (!NodeVal->TryGetObject(NodeObjPtr) || !NodeObjPtr) continue;
			const TSharedPtr<FJsonObject>& NodeObj = *NodeObjPtr;

			FString Id = NodeObj->GetStringField(TEXT("id"));
			USoundNode** FoundNode = NodeMap.Find(Id);
			if (!FoundNode) continue;

			const TSharedPtr<FJsonObject>* PropsObj = nullptr;
			if (NodeObj->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && PropsObj->IsValid())
			{
				for (const auto& Pair : (*PropsObj)->Values)
				{
					FString PropError;
					const FString PairKeyStr = MonolithKeyToString(Pair.Key);
					if (!SetNodeProperty(*FoundNode, PairKeyStr, Pair.Value, PropError))
					{
						UE_LOG(LogMonolith, Warning, TEXT("build_sound_cue_from_spec: node '%s' property '%s' error: %s"),
							*Id, *PairKeyStr, *PropError);
					}
				}
			}
		}
	}

	// Step 3: Wire connections
	int32 ConnectionCount = 0;
	const TArray<TSharedPtr<FJsonValue>>* ConnsArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("connections"), ConnsArray) && ConnsArray)
	{
		for (const auto& ConnVal : *ConnsArray)
		{
			const TSharedPtr<FJsonObject>* ConnObjPtr = nullptr;
			if (!ConnVal->TryGetObject(ConnObjPtr) || !ConnObjPtr) continue;
			const TSharedPtr<FJsonObject>& ConnObj = *ConnObjPtr;

			FString FromId = ConnObj->GetStringField(TEXT("from"));
			FString ToId = ConnObj->GetStringField(TEXT("to"));

			USoundNode** FromPtr = NodeMap.Find(FromId);
			USoundNode** ToPtr = NodeMap.Find(ToId);
			if (!FromPtr || !*FromPtr)
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("Connection 'from' node '%s' not found"), *FromId));
			}
			if (!ToPtr || !*ToPtr)
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("Connection 'to' node '%s' not found"), *ToId));
			}

			int32 ChildIndex = 0;
			double IdxVal;
			if (ConnObj->TryGetNumberField(TEXT("child_index"), IdxVal))
			{
				ChildIndex = static_cast<int32>(IdxVal);
			}
			else
			{
				// Auto-append
				ChildIndex = (*ToPtr)->ChildNodes.Num();
			}

			// Ensure enough child slots
			while ((*ToPtr)->ChildNodes.Num() <= ChildIndex)
			{
				(*ToPtr)->InsertChildNode((*ToPtr)->ChildNodes.Num());
			}

			(*ToPtr)->ChildNodes[ChildIndex] = *FromPtr;
			ConnectionCount++;
		}
	}

	// Step 4: Set FirstNode
	FString FirstNodeId = Spec->GetStringField(TEXT("first_node"));
	if (!FirstNodeId.IsEmpty())
	{
		USoundNode** FirstPtr = NodeMap.Find(FirstNodeId);
		if (!FirstPtr || !*FirstPtr)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("first_node '%s' not found in node map"), *FirstNodeId));
		}
		Cue->FirstNode = *FirstPtr;
	}

	// Step 5: Apply cue-level properties
	const TSharedPtr<FJsonObject>* CuePropsObj = nullptr;
	if (Spec->TryGetObjectField(TEXT("properties"), CuePropsObj) && CuePropsObj && CuePropsObj->IsValid())
	{
		double Val;
		if ((*CuePropsObj)->TryGetNumberField(TEXT("VolumeMultiplier"), Val))
		{
			Cue->VolumeMultiplier = static_cast<float>(Val);
		}
		if ((*CuePropsObj)->TryGetNumberField(TEXT("PitchMultiplier"), Val))
		{
			Cue->PitchMultiplier = static_cast<float>(Val);
		}
		bool bVal;
		if ((*CuePropsObj)->TryGetBoolField(TEXT("bOverrideAttenuation"), bVal))
		{
			Cue->bOverrideAttenuation = bVal;
		}
		if ((*CuePropsObj)->TryGetBoolField(TEXT("bPrimeOnLoad"), bVal))
		{
			Cue->bPrimeOnLoad = bVal;
		}
		if ((*CuePropsObj)->TryGetBoolField(TEXT("bExcludeFromRandomNodeBranchCulling"), bVal))
		{
			Cue->bExcludeFromRandomNodeBranchCulling = bVal;
		}
	}

	// Step 6: Finalize
	bool bGraphLinkSkipped = false;
	FinalizeCue(Cue, &bGraphLinkSkipped);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Cue->GetPathName());
	Result->SetNumberField(TEXT("node_count"), NodeMap.Num());
	Result->SetNumberField(TEXT("connection_count"), ConnectionCount);
	if (bGraphLinkSkipped)
	{
		Result->SetBoolField(TEXT("graph_link_skipped"), true);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::CreateRandomSoundCue(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	const TArray<TSharedPtr<FJsonValue>>* WavesArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("sound_waves"), WavesArray) || !WavesArray || WavesArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("sound_waves array is required and must not be empty"));
	}

	TArray<FString> WavePaths;
	for (const auto& Val : *WavesArray)
	{
		FString Path;
		if (Val->TryGetString(Path)) WavePaths.Add(Path);
	}

	FString Error;
	USoundCue* Cue = CreateEmptySoundCue(AssetPath, Error);
	if (!Cue) return FMonolithActionResult::Error(Error);

	TArray<USoundNode*> WaveNodes = CreateWavePlayerNodes(Cue, WavePaths, Error);
	if (WaveNodes.Num() == 0)
	{
		return FMonolithActionResult::Error(Error.IsEmpty() ? TEXT("Failed to create WavePlayer nodes") : Error);
	}

	// Create Random node
	USoundNodeRandom* RandomNode = Cue->ConstructSoundNode<USoundNodeRandom>();
	if (!RandomNode)
	{
		return FMonolithActionResult::Error(TEXT("Failed to construct Random node"));
	}

	// Use InsertChildNode to create graph-pin-synced child slots, then assign
	for (int32 i = 0; i < WaveNodes.Num(); ++i)
	{
		RandomNode->InsertChildNode(i);
		RandomNode->ChildNodes[i] = WaveNodes[i];
	}

	// Apply optional weights
	const TArray<TSharedPtr<FJsonValue>>* WeightsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("weights"), WeightsArray) && WeightsArray && WeightsArray->Num() > 0)
	{
		// USoundNodeRandom has a Weights TArray<float> property
		FProperty* WeightsProp = USoundNodeRandom::StaticClass()->FindPropertyByName(TEXT("Weights"));
		if (WeightsProp)
		{
			void* WeightsPtr = WeightsProp->ContainerPtrToValuePtr<void>(RandomNode);
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(WeightsProp);
			if (ArrayProp)
			{
				FScriptArrayHelper ArrayHelper(ArrayProp, WeightsPtr);
				ArrayHelper.Resize(WeightsArray->Num());
				for (int32 i = 0; i < WeightsArray->Num(); ++i)
				{
					double DVal;
					if ((*WeightsArray)[i]->TryGetNumber(DVal))
					{
						FFloatProperty* InnerFloat = CastField<FFloatProperty>(ArrayProp->Inner);
						if (InnerFloat)
						{
							InnerFloat->SetPropertyValue(ArrayHelper.GetRawPtr(i), static_cast<float>(DVal));
						}
					}
				}
			}
		}
	}

	Cue->FirstNode = RandomNode;

	bool bGraphLinkSkipped = false;
	FinalizeCue(Cue, &bGraphLinkSkipped);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Cue->GetPathName());
	Result->SetNumberField(TEXT("wave_count"), WaveNodes.Num());
	if (bGraphLinkSkipped)
	{
		Result->SetBoolField(TEXT("graph_link_skipped"), true);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::CreateLayeredSoundCue(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	const TArray<TSharedPtr<FJsonValue>>* WavesArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("sound_waves"), WavesArray) || !WavesArray || WavesArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("sound_waves array is required and must not be empty"));
	}

	TArray<FString> WavePaths;
	for (const auto& Val : *WavesArray)
	{
		FString Path;
		if (Val->TryGetString(Path)) WavePaths.Add(Path);
	}

	FString Error;
	USoundCue* Cue = CreateEmptySoundCue(AssetPath, Error);
	if (!Cue) return FMonolithActionResult::Error(Error);

	TArray<USoundNode*> WaveNodes = CreateWavePlayerNodes(Cue, WavePaths, Error);
	if (WaveNodes.Num() == 0)
	{
		return FMonolithActionResult::Error(Error.IsEmpty() ? TEXT("Failed to create WavePlayer nodes") : Error);
	}

	// Create Mixer node
	USoundNodeMixer* MixerNode = Cue->ConstructSoundNode<USoundNodeMixer>();
	if (!MixerNode)
	{
		return FMonolithActionResult::Error(TEXT("Failed to construct Mixer node"));
	}

	// Use InsertChildNode to create graph-pin-synced child slots, then assign
	for (int32 i = 0; i < WaveNodes.Num(); ++i)
	{
		MixerNode->InsertChildNode(i);
		MixerNode->ChildNodes[i] = WaveNodes[i];
	}

	// Apply optional per-input volumes via the InputVolume array
	const TArray<TSharedPtr<FJsonValue>>* VolumesArray = nullptr;
	if (Params->TryGetArrayField(TEXT("volumes"), VolumesArray) && VolumesArray && VolumesArray->Num() > 0)
	{
		FProperty* VolProp = USoundNodeMixer::StaticClass()->FindPropertyByName(TEXT("InputVolume"));
		if (VolProp)
		{
			void* VolPtr = VolProp->ContainerPtrToValuePtr<void>(MixerNode);
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(VolProp);
			if (ArrayProp)
			{
				FScriptArrayHelper ArrayHelper(ArrayProp, VolPtr);
				ArrayHelper.Resize(VolumesArray->Num());
				for (int32 i = 0; i < VolumesArray->Num(); ++i)
				{
					double DVal;
					if ((*VolumesArray)[i]->TryGetNumber(DVal))
					{
						FFloatProperty* InnerFloat = CastField<FFloatProperty>(ArrayProp->Inner);
						if (InnerFloat)
						{
							InnerFloat->SetPropertyValue(ArrayHelper.GetRawPtr(i), static_cast<float>(DVal));
						}
					}
				}
			}
		}
	}

	Cue->FirstNode = MixerNode;

	bool bGraphLinkSkipped = false;
	FinalizeCue(Cue, &bGraphLinkSkipped);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Cue->GetPathName());
	Result->SetNumberField(TEXT("wave_count"), WaveNodes.Num());
	if (bGraphLinkSkipped)
	{
		Result->SetBoolField(TEXT("graph_link_skipped"), true);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::CreateLoopingAmbientCue(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	const TArray<TSharedPtr<FJsonValue>>* WavesArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("sound_waves"), WavesArray) || !WavesArray || WavesArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("sound_waves array is required and must not be empty"));
	}

	double DelayMin = 0.1, DelayMax = 1.0;
	Params->TryGetNumberField(TEXT("delay_min"), DelayMin);
	Params->TryGetNumberField(TEXT("delay_max"), DelayMax);

	TArray<FString> WavePaths;
	for (const auto& Val : *WavesArray)
	{
		FString Path;
		if (Val->TryGetString(Path)) WavePaths.Add(Path);
	}

	FString Error;
	USoundCue* Cue = CreateEmptySoundCue(AssetPath, Error);
	if (!Cue) return FMonolithActionResult::Error(Error);

	TArray<USoundNode*> WaveNodes = CreateWavePlayerNodes(Cue, WavePaths, Error);
	if (WaveNodes.Num() == 0)
	{
		return FMonolithActionResult::Error(Error.IsEmpty() ? TEXT("Failed to create WavePlayer nodes") : Error);
	}

	// Build chain: Looping -> Delay -> Random -> WavePlayers
	//   (or Looping -> Delay -> WavePlayer if single wave)

	// Create Delay node
	USoundNodeDelay* DelayNode = Cue->ConstructSoundNode<USoundNodeDelay>();
	if (!DelayNode)
	{
		return FMonolithActionResult::Error(TEXT("Failed to construct Delay node"));
	}

	// Set delay min/max via reflection
	FProperty* DelayMinProp = USoundNodeDelay::StaticClass()->FindPropertyByName(TEXT("DelayMin"));
	FProperty* DelayMaxProp = USoundNodeDelay::StaticClass()->FindPropertyByName(TEXT("DelayMax"));
	if (DelayMinProp)
	{
		FFloatProperty* FloatProp = CastField<FFloatProperty>(DelayMinProp);
		if (FloatProp) FloatProp->SetPropertyValue(FloatProp->ContainerPtrToValuePtr<void>(DelayNode), static_cast<float>(DelayMin));
	}
	if (DelayMaxProp)
	{
		FFloatProperty* FloatProp = CastField<FFloatProperty>(DelayMaxProp);
		if (FloatProp) FloatProp->SetPropertyValue(FloatProp->ContainerPtrToValuePtr<void>(DelayNode), static_cast<float>(DelayMax));
	}

	USoundNode* ChildOfDelay = nullptr;
	if (WaveNodes.Num() == 1)
	{
		ChildOfDelay = WaveNodes[0];
	}
	else
	{
		// Create Random node
		USoundNodeRandom* RandomNode = Cue->ConstructSoundNode<USoundNodeRandom>();
		if (!RandomNode)
		{
			return FMonolithActionResult::Error(TEXT("Failed to construct Random node"));
		}
		// Use InsertChildNode to create graph-pin-synced child slots, then assign
		for (int32 i = 0; i < WaveNodes.Num(); ++i)
		{
			RandomNode->InsertChildNode(i);
			RandomNode->ChildNodes[i] = WaveNodes[i];
		}
		ChildOfDelay = RandomNode;
	}

	// Wire Delay -> child
	DelayNode->InsertChildNode(0);
	DelayNode->ChildNodes[0] = ChildOfDelay;

	// Create Looping node
	USoundNodeLooping* LoopingNode = Cue->ConstructSoundNode<USoundNodeLooping>();
	if (!LoopingNode)
	{
		return FMonolithActionResult::Error(TEXT("Failed to construct Looping node"));
	}

	// Wire Looping -> Delay
	LoopingNode->InsertChildNode(0);
	LoopingNode->ChildNodes[0] = DelayNode;

	Cue->FirstNode = LoopingNode;

	bool bGraphLinkSkipped = false;
	FinalizeCue(Cue, &bGraphLinkSkipped);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Cue->GetPathName());
	Result->SetNumberField(TEXT("wave_count"), WaveNodes.Num());
	Result->SetNumberField(TEXT("delay_min"), DelayMin);
	Result->SetNumberField(TEXT("delay_max"), DelayMax);
	if (bGraphLinkSkipped)
	{
		Result->SetBoolField(TEXT("graph_link_skipped"), true);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::CreateDistanceCrossfadeCue(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	const TArray<TSharedPtr<FJsonValue>>* BandsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("bands"), BandsArray) || !BandsArray || BandsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("bands array is required and must not be empty"));
	}

	FString Error;
	USoundCue* Cue = CreateEmptySoundCue(AssetPath, Error);
	if (!Cue) return FMonolithActionResult::Error(Error);

	// Create DistanceCrossFade node
	USoundNodeDistanceCrossFade* CrossFadeNode = Cue->ConstructSoundNode<USoundNodeDistanceCrossFade>();
	if (!CrossFadeNode)
	{
		return FMonolithActionResult::Error(TEXT("Failed to construct DistanceCrossFade node"));
	}

	// Parse bands and create WavePlayer children
	TArray<USoundNode*> WavePlayerChildren;
	for (const auto& BandVal : *BandsArray)
	{
		const TSharedPtr<FJsonObject>* BandObjPtr = nullptr;
		if (!BandVal->TryGetObject(BandObjPtr) || !BandObjPtr) continue;
		const TSharedPtr<FJsonObject>& BandObj = *BandObjPtr;

		FString WavePath = BandObj->GetStringField(TEXT("sound_wave"));
		if (WavePath.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Each band must have a 'sound_wave' path"));
		}

		USoundWave* Wave = Cast<USoundWave>(StaticLoadObject(USoundWave::StaticClass(), nullptr, *WavePath));
		if (!Wave)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Could not load SoundWave at '%s'"), *WavePath));
		}

		USoundNodeWavePlayer* WavePlayer = Cue->ConstructSoundNode<USoundNodeWavePlayer>();
		if (!WavePlayer)
		{
			return FMonolithActionResult::Error(TEXT("Failed to construct WavePlayer"));
		}
		WavePlayer->SetSoundWave(Wave);
		WavePlayerChildren.Add(WavePlayer);
	}

	// Use InsertChildNode to create graph-pin-synced child slots, then assign
	for (int32 i = 0; i < WavePlayerChildren.Num(); ++i)
	{
		CrossFadeNode->InsertChildNode(i);
		CrossFadeNode->ChildNodes[i] = WavePlayerChildren[i];
	}

	// Set CrossFadeInput array (FDistanceDatum entries) via reflection
	FProperty* CrossFadeInputProp = USoundNodeDistanceCrossFade::StaticClass()->FindPropertyByName(TEXT("CrossFadeInput"));
	if (CrossFadeInputProp)
	{
		FArrayProperty* ArrayProp = CastField<FArrayProperty>(CrossFadeInputProp);
		if (ArrayProp)
		{
			void* ArrayPtr = ArrayProp->ContainerPtrToValuePtr<void>(CrossFadeNode);
			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayPtr);
			ArrayHelper.Resize(BandsArray->Num());

			FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProp->Inner);
			if (InnerStruct)
			{
				for (int32 i = 0; i < BandsArray->Num(); ++i)
				{
					const TSharedPtr<FJsonObject>* BandObjPtr = nullptr;
					if (!(*BandsArray)[i]->TryGetObject(BandObjPtr) || !BandObjPtr) continue;
					const TSharedPtr<FJsonObject>& BandObj = *BandObjPtr;

					void* ElemPtr = ArrayHelper.GetRawPtr(i);

					// Set fields via reflection on FDistanceDatum struct
					auto SetFloatField = [&](const TCHAR* FieldName, const TCHAR* JsonField)
					{
						FProperty* FieldProp = InnerStruct->Struct->FindPropertyByName(FName(FieldName));
						if (FieldProp)
						{
							double DVal = 0;
							if (BandObj->TryGetNumberField(JsonField, DVal))
							{
								FFloatProperty* FP = CastField<FFloatProperty>(FieldProp);
								if (FP) FP->SetPropertyValue(FieldProp->ContainerPtrToValuePtr<void>(ElemPtr), static_cast<float>(DVal));
							}
						}
					};

					SetFloatField(TEXT("FadeInDistanceStart"), TEXT("fade_in_distance_start"));
					SetFloatField(TEXT("FadeInDistanceEnd"), TEXT("fade_in_distance_end"));
					SetFloatField(TEXT("FadeOutDistanceStart"), TEXT("fade_out_distance_start"));
					SetFloatField(TEXT("FadeOutDistanceEnd"), TEXT("fade_out_distance_end"));
					SetFloatField(TEXT("Volume"), TEXT("volume"));
				}
			}
		}
	}

	Cue->FirstNode = CrossFadeNode;

	bool bGraphLinkSkipped = false;
	FinalizeCue(Cue, &bGraphLinkSkipped);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Cue->GetPathName());
	Result->SetNumberField(TEXT("band_count"), BandsArray->Num());
	if (bGraphLinkSkipped)
	{
		Result->SetBoolField(TEXT("graph_link_skipped"), true);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::CreateSwitchSoundCue(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString ParameterName = Params->GetStringField(TEXT("parameter_name"));
	if (AssetPath.IsEmpty() || ParameterName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and parameter_name are required"));
	}

	const TArray<TSharedPtr<FJsonValue>>* WavesArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("sound_waves"), WavesArray) || !WavesArray || WavesArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("sound_waves array is required and must not be empty"));
	}

	TArray<FString> WavePaths;
	for (const auto& Val : *WavesArray)
	{
		FString Path;
		if (Val->TryGetString(Path)) WavePaths.Add(Path);
	}

	FString Error;
	USoundCue* Cue = CreateEmptySoundCue(AssetPath, Error);
	if (!Cue) return FMonolithActionResult::Error(Error);

	TArray<USoundNode*> WaveNodes = CreateWavePlayerNodes(Cue, WavePaths, Error);
	if (WaveNodes.Num() == 0)
	{
		return FMonolithActionResult::Error(Error.IsEmpty() ? TEXT("Failed to create WavePlayer nodes") : Error);
	}

	// Create Switch node
	USoundNodeSwitch* SwitchNode = Cue->ConstructSoundNode<USoundNodeSwitch>();
	if (!SwitchNode)
	{
		return FMonolithActionResult::Error(TEXT("Failed to construct Switch node"));
	}

	// Set the switch parameter name via reflection
	FProperty* IntParamProp = USoundNodeSwitch::StaticClass()->FindPropertyByName(TEXT("IntParameterName"));
	if (IntParamProp)
	{
		FNameProperty* NameProp = CastField<FNameProperty>(IntParamProp);
		if (NameProp)
		{
			NameProp->SetPropertyValue(NameProp->ContainerPtrToValuePtr<void>(SwitchNode), FName(*ParameterName));
		}
	}

	// Use InsertChildNode to create graph-pin-synced child slots, then assign
	for (int32 i = 0; i < WaveNodes.Num(); ++i)
	{
		SwitchNode->InsertChildNode(i);
		SwitchNode->ChildNodes[i] = WaveNodes[i];
	}

	Cue->FirstNode = SwitchNode;

	bool bGraphLinkSkipped = false;
	FinalizeCue(Cue, &bGraphLinkSkipped);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Cue->GetPathName());
	Result->SetStringField(TEXT("parameter_name"), ParameterName);
	Result->SetNumberField(TEXT("variant_count"), WaveNodes.Num());
	if (bGraphLinkSkipped)
	{
		Result->SetBoolField(TEXT("graph_link_skipped"), true);
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Utility Actions
// ============================================================================

FMonolithActionResult FMonolithAudioSoundCueActions::DuplicateSoundCue(const TSharedPtr<FJsonObject>& Params)
{
	const FString SourcePath = Params->GetStringField(TEXT("source_path"));
	const FString DestPath = Params->GetStringField(TEXT("dest_path"));
	if (SourcePath.IsEmpty() || DestPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("source_path and dest_path are required"));
	}

	IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
	UObject* Source = StaticLoadObject(UObject::StaticClass(), nullptr, *SourcePath);
	if (!Source) { return FMonolithActionResult::Error(FString::Printf(TEXT("Source asset not found: '%s'"), *SourcePath)); }
	FString DestPackagePath, DestAssetName;
	DestPath.Split(TEXT("/"), &DestPackagePath, &DestAssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	UObject* Duplicated = AssetTools.DuplicateAsset(DestAssetName, DestPackagePath, Source);
	if (!Duplicated)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("DuplicateAsset failed — check that source exists and destination doesn't already exist: '%s' -> '%s'"),
			*SourcePath, *DestPath));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("dest_path"), Duplicated->GetPathName());
	Result->SetStringField(TEXT("class"), Duplicated->GetClass()->GetName());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::DeleteAudioAsset(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found at '%s'"), *AssetPath));
	}

	bool bDeleted = ObjectTools::DeleteSingleObject(Asset, /*bPerformReferenceCheck=*/false);
	if (!bDeleted)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to delete asset at '%s'"), *AssetPath));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("deleted_path"), AssetPath);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::PreviewSound(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	USoundBase* Sound = Cast<USoundBase>(StaticLoadObject(USoundBase::StaticClass(), nullptr, *AssetPath));
	if (!Sound)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Could not load USoundBase at '%s'"), *AssetPath));
	}

	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor is not available"));
	}

	GEditor->PlayPreviewSound(Sound);

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("playing"), true);
	Result->SetNumberField(TEXT("duration"), Sound->GetDuration());
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::StopPreview(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor is not available"));
	}

	GEditor->ResetPreviewAudioComponent();

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAudioSoundCueActions::GetSoundCueDuration(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	USoundCue* Cue = LoadSoundCue(AssetPath, Error);
	if (!Cue)
	{
		return FMonolithActionResult::Error(Error);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("duration"), Cue->GetDuration());
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	return FMonolithActionResult::Success(Result);
}
