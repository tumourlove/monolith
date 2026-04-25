#include "MonolithAudioQueryActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "Sound/SoundWave.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundMix.h"
#include "Sound/AudioSettings.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// LogMonolith declared in MonolithJsonUtils.h (MonolithCore)

// ============================================================================
// Audio class type resolution
// ============================================================================

UClass* FMonolithAudioQueryActions::ResolveAudioClass(const FString& TypeName)
{
	if (TypeName.Equals(TEXT("SoundWave"), ESearchCase::IgnoreCase))
		return USoundWave::StaticClass();
	if (TypeName.Equals(TEXT("SoundCue"), ESearchCase::IgnoreCase))
		return USoundCue::StaticClass();
	if (TypeName.Equals(TEXT("MetaSoundSource"), ESearchCase::IgnoreCase) ||
		TypeName.Equals(TEXT("MetaSound"), ESearchCase::IgnoreCase))
	{
		// MetaSoundSource is a runtime class — resolve dynamically to avoid hard dep
		UClass* MetaSoundClass = FindObject<UClass>(nullptr, TEXT("/Script/MetasoundEngine.MetaSoundSource"));
		return MetaSoundClass; // May be null if MetaSound not available
	}
	if (TypeName.Equals(TEXT("SoundClass"), ESearchCase::IgnoreCase))
		return USoundClass::StaticClass();
	if (TypeName.Equals(TEXT("SoundAttenuation"), ESearchCase::IgnoreCase))
		return USoundAttenuation::StaticClass();
	if (TypeName.Equals(TEXT("SoundSubmix"), ESearchCase::IgnoreCase))
		return USoundSubmix::StaticClass();
	if (TypeName.Equals(TEXT("SoundConcurrency"), ESearchCase::IgnoreCase))
		return USoundConcurrency::StaticClass();
	if (TypeName.Equals(TEXT("SoundMix"), ESearchCase::IgnoreCase))
		return USoundMix::StaticClass();
	return nullptr;
}

FString FMonolithAudioQueryActions::CompressionTypeToString(uint8 Type)
{
	switch (Type)
	{
	case 0: return TEXT("BinkAudio");
	case 1: return TEXT("ADPCM");
	case 2: return TEXT("PCM");
	case 3: return TEXT("Opus");
	case 4: return TEXT("RADAudio");
	case 5: return TEXT("PlatformSpecific");
	case 6: return TEXT("ProjectDefined");
	default: return TEXT("Unknown");
	}
}

namespace
{
	// Phase F #6: read SoundWave's compression-type enum value robustly.
	// UENUM-tagged "enum class : uint8" can show up as either an FEnumProperty
	// (when UENUM(BlueprintType)) or an FByteProperty (plain UENUM). Try both.
	// Property name in UE 5.7 USoundWave is "SoundAssetCompressionType".
	uint8 ReadSoundAssetCompressionType(USoundWave* Wave)
	{
		if (!Wave) return 255; // sentinel -> "Unknown"

		FProperty* Prop = USoundWave::StaticClass()->FindPropertyByName(TEXT("SoundAssetCompressionType"));
		if (!Prop)
		{
			return 255;
		}

		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			uint8 Val = 0;
			if (FNumericProperty* Underlying = EnumProp->GetUnderlyingProperty())
			{
				const void* AddrInContainer = EnumProp->ContainerPtrToValuePtr<void>(Wave);
				const int64 SignedVal = Underlying->GetSignedIntPropertyValue(AddrInContainer);
				Val = static_cast<uint8>(SignedVal);
			}
			return Val;
		}
		if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			return ByteProp->GetPropertyValue_InContainer(Wave);
		}
		return 255;
	}
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithAudioQueryActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("audio"), TEXT("list_audio_assets"),
		TEXT("List audio assets by type with optional path filter"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioQueryActions::ListAudioAssets),
		FParamSchemaBuilder()
			.Required(TEXT("type"), TEXT("string"), TEXT("Asset type: SoundWave, SoundCue, MetaSoundSource, SoundClass, SoundAttenuation, SoundSubmix, SoundConcurrency, SoundMix, or All"))
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Only return assets under this content path (e.g. /Game/Audio)"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum number of results (default: 100)"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("search_audio_assets"),
		TEXT("Search audio assets by name substring with optional type filter"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioQueryActions::SearchAudioAssets),
		FParamSchemaBuilder()
			.Required(TEXT("query"), TEXT("string"), TEXT("Search string to match against asset names"))
			.Optional(TEXT("type"), TEXT("string"), TEXT("Filter by audio type (same values as list_audio_assets)"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum number of results (default: 50)"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("get_sound_wave_info"),
		TEXT("Get detailed properties of a USoundWave asset"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioQueryActions::GetSoundWaveInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("SoundWave asset path"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("get_sound_class_hierarchy"),
		TEXT("Get the SoundClass tree structure starting from a root class or all roots"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioQueryActions::GetSoundClassHierarchy),
		FParamSchemaBuilder()
			.Optional(TEXT("root_class"), TEXT("string"), TEXT("Asset path of root SoundClass. If omitted, returns all root classes"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("get_submix_hierarchy"),
		TEXT("Get the SoundSubmix tree structure starting from a root submix or all roots"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioQueryActions::GetSubmixHierarchy),
		FParamSchemaBuilder()
			.Optional(TEXT("root_submix"), TEXT("string"), TEXT("Asset path of root SoundSubmix. If omitted, returns all root submixes"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("find_audio_references"),
		TEXT("Find assets that reference or are depended on by a given audio asset"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioQueryActions::FindAudioReferences),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Audio asset path"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("find_unused_audio"),
		TEXT("Find audio assets with zero referencers (potentially unused)"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioQueryActions::FindUnusedAudio),
		FParamSchemaBuilder()
			.Optional(TEXT("type"), TEXT("string"), TEXT("Filter by audio type (default: All)"), TEXT("All"))
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Only scan assets under this content path"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum results to prevent timeout (default: 100)"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("find_sounds_without_class"),
		TEXT("Find SoundBase-derived assets that have no SoundClass assigned"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioQueryActions::FindSoundsWithoutClass),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Only scan assets under this content path"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Max results to return (default: 100)"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("find_unattenuated_sounds"),
		TEXT("Find SoundBase-derived assets with no attenuation configured"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioQueryActions::FindUnattenuatedSounds),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Only scan assets under this content path"))
			.Build());

	Registry.RegisterAction(TEXT("audio"), TEXT("get_audio_stats"),
		TEXT("Get aggregate audio statistics: counts by type, estimated size, compression breakdown"),
		FMonolithActionHandler::CreateStatic(&FMonolithAudioQueryActions::GetAudioStats),
		MakeShared<FJsonObject>());
}

// ============================================================================
// Action: list_audio_assets
// ============================================================================

FMonolithActionResult FMonolithAudioQueryActions::ListAudioAssets(const TSharedPtr<FJsonObject>& Params)
{
	const FString TypeStr = Params->GetStringField(TEXT("type"));
	const FString PathFilter = Params->HasField(TEXT("path_filter")) ? Params->GetStringField(TEXT("path_filter")) : TEXT("");
	const int32 Limit = Params->HasField(TEXT("limit")) ? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 100;

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Determine which classes to query
	TArray<UClass*> ClassesToQuery;
	if (TypeStr.Equals(TEXT("All"), ESearchCase::IgnoreCase))
	{
		ClassesToQuery.Add(USoundWave::StaticClass());
		ClassesToQuery.Add(USoundCue::StaticClass());
		ClassesToQuery.Add(USoundClass::StaticClass());
		ClassesToQuery.Add(USoundAttenuation::StaticClass());
		ClassesToQuery.Add(USoundSubmix::StaticClass());
		ClassesToQuery.Add(USoundConcurrency::StaticClass());
		ClassesToQuery.Add(USoundMix::StaticClass());
		// MetaSoundSource via dynamic lookup
		if (UClass* MetaSoundClass = FindObject<UClass>(nullptr, TEXT("/Script/MetasoundEngine.MetaSoundSource")))
		{
			ClassesToQuery.Add(MetaSoundClass);
		}
	}
	else
	{
		UClass* ResolvedClass = ResolveAudioClass(TypeStr);
		if (!ResolvedClass)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Unknown audio type '%s'. Valid types: SoundWave, SoundCue, MetaSoundSource, SoundClass, SoundAttenuation, SoundSubmix, SoundConcurrency, SoundMix, All"),
				*TypeStr));
		}
		ClassesToQuery.Add(ResolvedClass);
	}

	TArray<TSharedPtr<FJsonValue>> AssetsArray;
	int32 Count = 0;

	for (UClass* AssetClass : ClassesToQuery)
	{
		if (Count >= Limit) break;

		TArray<FAssetData> AssetDataList;
		AR.GetAssetsByClass(AssetClass->GetClassPathName(), AssetDataList, true);

		for (const FAssetData& AssetData : AssetDataList)
		{
			if (Count >= Limit) break;

			const FString AssetPath = AssetData.GetObjectPathString();

			// Apply path filter
			if (!PathFilter.IsEmpty() && !AssetPath.StartsWith(PathFilter))
			{
				continue;
			}

			auto AssetJson = MakeShared<FJsonObject>();
			AssetJson->SetStringField(TEXT("path"), AssetPath);
			AssetJson->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
			AssetJson->SetStringField(TEXT("type"), AssetData.AssetClassPath.GetAssetName().ToString());
			AssetsArray.Add(MakeShared<FJsonValueObject>(AssetJson));
			Count++;
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetNumberField(TEXT("count"), AssetsArray.Num());
	ResultJson->SetArrayField(TEXT("assets"), AssetsArray);
	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: search_audio_assets
// ============================================================================

FMonolithActionResult FMonolithAudioQueryActions::SearchAudioAssets(const TSharedPtr<FJsonObject>& Params)
{
	const FString Query = Params->GetStringField(TEXT("query"));
	const FString TypeStr = Params->HasField(TEXT("type")) ? Params->GetStringField(TEXT("type")) : TEXT("All");
	const int32 Limit = Params->HasField(TEXT("limit")) ? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 50;

	if (Query.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'query' parameter cannot be empty"));
	}

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Determine which classes to search
	TArray<UClass*> ClassesToSearch;
	if (TypeStr.Equals(TEXT("All"), ESearchCase::IgnoreCase))
	{
		ClassesToSearch.Add(USoundWave::StaticClass());
		ClassesToSearch.Add(USoundCue::StaticClass());
		ClassesToSearch.Add(USoundClass::StaticClass());
		ClassesToSearch.Add(USoundAttenuation::StaticClass());
		ClassesToSearch.Add(USoundSubmix::StaticClass());
		ClassesToSearch.Add(USoundConcurrency::StaticClass());
		ClassesToSearch.Add(USoundMix::StaticClass());
		if (UClass* MetaSoundClass = FindObject<UClass>(nullptr, TEXT("/Script/MetasoundEngine.MetaSoundSource")))
		{
			ClassesToSearch.Add(MetaSoundClass);
		}
	}
	else
	{
		UClass* ResolvedClass = ResolveAudioClass(TypeStr);
		if (!ResolvedClass)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Unknown audio type '%s'"), *TypeStr));
		}
		ClassesToSearch.Add(ResolvedClass);
	}

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 Count = 0;

	for (UClass* AssetClass : ClassesToSearch)
	{
		if (Count >= Limit) break;

		TArray<FAssetData> AssetDataList;
		AR.GetAssetsByClass(AssetClass->GetClassPathName(), AssetDataList, true);

		for (const FAssetData& AssetData : AssetDataList)
		{
			if (Count >= Limit) break;

			// Match against both asset name AND full path so folder-based matches work
			// e.g. query "footstep" matches /Game/Audio/Footsteps/SC_Step_Dirt_01
			const FString Name = AssetData.AssetName.ToString();
			const FString FullPath = AssetData.GetObjectPathString();
			if (Name.Contains(Query, ESearchCase::IgnoreCase) || FullPath.Contains(Query, ESearchCase::IgnoreCase))
			{
				auto AssetJson = MakeShared<FJsonObject>();
				AssetJson->SetStringField(TEXT("path"), FullPath);
				AssetJson->SetStringField(TEXT("name"), Name);
				AssetJson->SetStringField(TEXT("type"), AssetData.AssetClassPath.GetAssetName().ToString());
				ResultsArray.Add(MakeShared<FJsonValueObject>(AssetJson));
				Count++;
			}
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("query"), Query);
	ResultJson->SetNumberField(TEXT("count"), ResultsArray.Num());
	ResultJson->SetArrayField(TEXT("results"), ResultsArray);
	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_sound_wave_info
// ============================================================================

FMonolithActionResult FMonolithAudioQueryActions::GetSoundWaveInfo(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	USoundWave* SoundWave = Cast<USoundWave>(StaticLoadObject(USoundWave::StaticClass(), nullptr, *AssetPath));
	if (!SoundWave)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to load SoundWave at '%s'"), *AssetPath));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("name"), SoundWave->GetName());

	// Duration (inherited from USoundBase)
	ResultJson->SetNumberField(TEXT("duration"), SoundWave->Duration);

	// Channel / sample info
	ResultJson->SetNumberField(TEXT("num_channels"), SoundWave->NumChannels);
	ResultJson->SetNumberField(TEXT("sample_rate"), SoundWave->GetSampleRateForCurrentPlatform());

	// Compression
	ResultJson->SetNumberField(TEXT("compression_quality"), SoundWave->GetCompressionQuality());
	{
		// Phase F #6: use the byte/enum-tolerant reader so 'Unknown' only fires on real failure.
		const uint8 TypeVal = ReadSoundAssetCompressionType(SoundWave);
		ResultJson->SetStringField(TEXT("compression_type"), CompressionTypeToString(TypeVal));
	}

	// Looping
	ResultJson->SetBoolField(TEXT("looping"), SoundWave->bLooping);

	// Sound class (from USoundBase)
	if (SoundWave->SoundClassObject)
	{
		ResultJson->SetStringField(TEXT("sound_class"), SoundWave->SoundClassObject->GetPathName());
	}
	else
	{
		ResultJson->SetField(TEXT("sound_class"), MakeShared<FJsonValueNull>());
	}

	// Attenuation (from USoundBase)
	if (SoundWave->AttenuationSettings)
	{
		ResultJson->SetStringField(TEXT("attenuation_settings"), SoundWave->AttenuationSettings->GetPathName());
	}
	else
	{
		ResultJson->SetField(TEXT("attenuation_settings"), MakeShared<FJsonValueNull>());
	}

	// Submix (from USoundBase — TObjectPtr<USoundSubmixBase>)
	if (SoundWave->SoundSubmixObject)
	{
		ResultJson->SetStringField(TEXT("sound_submix"), SoundWave->SoundSubmixObject->GetPathName());
	}
	else
	{
		ResultJson->SetField(TEXT("sound_submix"), MakeShared<FJsonValueNull>());
	}

	// Volume and Pitch — these are on USoundWave directly (not base)
	// USoundWave has Volume and Pitch as UPROPERTY, read via reflection for safety
	if (const FFloatProperty* VolProp = FindFProperty<FFloatProperty>(USoundWave::StaticClass(), TEXT("Volume")))
	{
		ResultJson->SetNumberField(TEXT("volume"), VolProp->GetPropertyValue_InContainer(SoundWave));
	}
	if (const FFloatProperty* PitchProp = FindFProperty<FFloatProperty>(USoundWave::StaticClass(), TEXT("Pitch")))
	{
		ResultJson->SetNumberField(TEXT("pitch"), PitchProp->GetPropertyValue_InContainer(SoundWave));
	}

	// Resource size estimate
	const int32 ResourceSize = SoundWave->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal);
	ResultJson->SetNumberField(TEXT("resource_size_bytes"), ResourceSize);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_sound_class_hierarchy
// ============================================================================

TSharedPtr<FJsonObject> FMonolithAudioQueryActions::BuildSoundClassTree(USoundClass* SoundClass, TSet<USoundClass*>& Visited)
{
	if (!SoundClass || Visited.Contains(SoundClass))
	{
		return nullptr;
	}
	Visited.Add(SoundClass);

	auto NodeJson = MakeShared<FJsonObject>();
	NodeJson->SetStringField(TEXT("name"), SoundClass->GetName());
	NodeJson->SetStringField(TEXT("path"), SoundClass->GetPathName());

	// Properties
	auto PropsJson = MakeShared<FJsonObject>();
	const FSoundClassProperties& Props = SoundClass->Properties;
	PropsJson->SetNumberField(TEXT("volume"), Props.Volume);
	PropsJson->SetNumberField(TEXT("pitch"), Props.Pitch);
	PropsJson->SetNumberField(TEXT("low_pass_filter_frequency"), Props.LowPassFilterFrequency);
	PropsJson->SetNumberField(TEXT("attenuation_distance_scale"), Props.AttenuationDistanceScale);
	PropsJson->SetNumberField(TEXT("lfe_bleed"), Props.LFEBleed);
	PropsJson->SetBoolField(TEXT("always_play"), Props.bAlwaysPlay != 0);
	PropsJson->SetBoolField(TEXT("is_ui_sound"), Props.bIsUISound != 0);
	PropsJson->SetBoolField(TEXT("is_music"), Props.bIsMusic != 0);
	PropsJson->SetBoolField(TEXT("reverb"), Props.bReverb != 0);
	NodeJson->SetObjectField(TEXT("properties"), PropsJson);

	// Children
	TArray<TSharedPtr<FJsonValue>> ChildrenArray;
	for (USoundClass* ChildClass : SoundClass->ChildClasses)
	{
		if (TSharedPtr<FJsonObject> ChildJson = BuildSoundClassTree(ChildClass, Visited))
		{
			ChildrenArray.Add(MakeShared<FJsonValueObject>(ChildJson));
		}
	}
	NodeJson->SetArrayField(TEXT("children"), ChildrenArray);

	return NodeJson;
}

FMonolithActionResult FMonolithAudioQueryActions::GetSoundClassHierarchy(const TSharedPtr<FJsonObject>& Params)
{
	const FString RootClassPath = Params->HasField(TEXT("root_class")) ? Params->GetStringField(TEXT("root_class")) : TEXT("");

	TSet<USoundClass*> Visited;

	if (!RootClassPath.IsEmpty())
	{
		// Load specific root
		USoundClass* RootClass = Cast<USoundClass>(StaticLoadObject(USoundClass::StaticClass(), nullptr, *RootClassPath));
		if (!RootClass)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to load SoundClass at '%s'"), *RootClassPath));
		}

		TSharedPtr<FJsonObject> TreeJson = BuildSoundClassTree(RootClass, Visited);
		auto ResultJson = MakeShared<FJsonObject>();
		ResultJson->SetObjectField(TEXT("tree"), TreeJson);
		return FMonolithActionResult::Success(ResultJson);
	}

	// No root specified — find all SoundClass assets and build trees from roots
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TArray<FAssetData> AllClassAssets;
	AR.GetAssetsByClass(USoundClass::StaticClass()->GetClassPathName(), AllClassAssets, true);

	// Load all classes, then find roots (those not referenced as children by others)
	TArray<USoundClass*> AllClasses;
	TSet<USoundClass*> NonRoots;
	for (const FAssetData& AssetData : AllClassAssets)
	{
		USoundClass* SC = Cast<USoundClass>(AssetData.GetAsset());
		if (SC)
		{
			AllClasses.Add(SC);
			for (USoundClass* Child : SC->ChildClasses)
			{
				if (Child) NonRoots.Add(Child);
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> TreesArray;
	for (USoundClass* SC : AllClasses)
	{
		if (!NonRoots.Contains(SC))
		{
			if (TSharedPtr<FJsonObject> TreeJson = BuildSoundClassTree(SC, Visited))
			{
				TreesArray.Add(MakeShared<FJsonValueObject>(TreeJson));
			}
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetNumberField(TEXT("root_count"), TreesArray.Num());
	ResultJson->SetArrayField(TEXT("trees"), TreesArray);
	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_submix_hierarchy
// ============================================================================

TSharedPtr<FJsonObject> FMonolithAudioQueryActions::BuildSubmixTree(USoundSubmixBase* SubmixBase, TSet<USoundSubmixBase*>& Visited)
{
	if (!SubmixBase || Visited.Contains(SubmixBase))
	{
		return nullptr;
	}
	Visited.Add(SubmixBase);

	auto NodeJson = MakeShared<FJsonObject>();
	NodeJson->SetStringField(TEXT("name"), SubmixBase->GetName());
	NodeJson->SetStringField(TEXT("path"), SubmixBase->GetPathName());

	// Check if it's the concrete USoundSubmix type for extra properties
	USoundSubmix* Submix = Cast<USoundSubmix>(SubmixBase);
	if (Submix)
	{
		// Effect chain
		TArray<TSharedPtr<FJsonValue>> EffectsArray;
		for (const TObjectPtr<USoundEffectSubmixPreset>& Effect : Submix->SubmixEffectChain)
		{
			if (Effect)
			{
				auto EffectJson = MakeShared<FJsonObject>();
				EffectJson->SetStringField(TEXT("name"), Effect->GetName());
				EffectJson->SetStringField(TEXT("class"), Effect->GetClass()->GetName());
				EffectsArray.Add(MakeShared<FJsonValueObject>(EffectJson));
			}
		}
		NodeJson->SetArrayField(TEXT("effect_chain"), EffectsArray);
	}

	// Children — USoundSubmixBase has ChildSubmixes
	TArray<TSharedPtr<FJsonValue>> ChildrenArray;
	for (USoundSubmixBase* ChildSubmix : SubmixBase->ChildSubmixes)
	{
		if (TSharedPtr<FJsonObject> ChildJson = BuildSubmixTree(ChildSubmix, Visited))
		{
			ChildrenArray.Add(MakeShared<FJsonValueObject>(ChildJson));
		}
	}
	NodeJson->SetArrayField(TEXT("children"), ChildrenArray);

	return NodeJson;
}

FMonolithActionResult FMonolithAudioQueryActions::GetSubmixHierarchy(const TSharedPtr<FJsonObject>& Params)
{
	const FString RootSubmixPath = Params->HasField(TEXT("root_submix")) ? Params->GetStringField(TEXT("root_submix")) : TEXT("");

	TSet<USoundSubmixBase*> Visited;

	if (!RootSubmixPath.IsEmpty())
	{
		USoundSubmixBase* RootSubmix = Cast<USoundSubmixBase>(StaticLoadObject(USoundSubmixBase::StaticClass(), nullptr, *RootSubmixPath));
		if (!RootSubmix)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to load SoundSubmix at '%s'"), *RootSubmixPath));
		}

		TSharedPtr<FJsonObject> TreeJson = BuildSubmixTree(RootSubmix, Visited);
		auto ResultJson = MakeShared<FJsonObject>();
		ResultJson->SetObjectField(TEXT("tree"), TreeJson);
		return FMonolithActionResult::Success(ResultJson);
	}

	// No root specified — find all submixes and build trees from roots (those with no parent)
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TArray<FAssetData> AllSubmixAssets;
	AR.GetAssetsByClass(USoundSubmixBase::StaticClass()->GetClassPathName(), AllSubmixAssets, true);

	TArray<USoundSubmixBase*> AllSubmixes;
	TSet<USoundSubmixBase*> NonRoots;
	for (const FAssetData& AssetData : AllSubmixAssets)
	{
		USoundSubmixBase* SM = Cast<USoundSubmixBase>(AssetData.GetAsset());
		if (SM)
		{
			AllSubmixes.Add(SM);
			for (USoundSubmixBase* Child : SM->ChildSubmixes)
			{
				if (Child) NonRoots.Add(Child);
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> TreesArray;
	for (USoundSubmixBase* SM : AllSubmixes)
	{
		if (!NonRoots.Contains(SM))
		{
			if (TSharedPtr<FJsonObject> TreeJson = BuildSubmixTree(SM, Visited))
			{
				TreesArray.Add(MakeShared<FJsonValueObject>(TreeJson));
			}
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetNumberField(TEXT("root_count"), TreesArray.Num());
	ResultJson->SetArrayField(TEXT("trees"), TreesArray);
	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: find_audio_references
// ============================================================================

FMonolithActionResult FMonolithAudioQueryActions::FindAudioReferences(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Extract package name from asset path (e.g., "/Game/Audio/MySoundWave" -> "/Game/Audio/MySoundWave")
	// For asset paths, the package name is the path without the object name after the dot
	FString PackageName = AssetPath;
	int32 DotIndex;
	if (PackageName.FindChar(TEXT('.'), DotIndex))
	{
		PackageName.LeftInline(DotIndex);
	}

	const FName PackageFName(*PackageName);

	// Get referencers (who references this asset)
	TArray<FAssetIdentifier> Referencers;
	AR.GetReferencers(FAssetIdentifier(PackageFName), Referencers);

	TArray<TSharedPtr<FJsonValue>> ReferencersArray;
	for (const FAssetIdentifier& Ref : Referencers)
	{
		if (!Ref.PackageName.IsNone())
		{
			ReferencersArray.Add(MakeShared<FJsonValueString>(Ref.PackageName.ToString()));
		}
	}

	// Get dependencies (what this asset depends on)
	TArray<FAssetIdentifier> Dependencies;
	AR.GetDependencies(FAssetIdentifier(PackageFName), Dependencies);

	TArray<TSharedPtr<FJsonValue>> DependenciesArray;
	for (const FAssetIdentifier& Dep : Dependencies)
	{
		if (!Dep.PackageName.IsNone())
		{
			DependenciesArray.Add(MakeShared<FJsonValueString>(Dep.PackageName.ToString()));
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetNumberField(TEXT("referencer_count"), ReferencersArray.Num());
	ResultJson->SetArrayField(TEXT("referencers"), ReferencersArray);
	ResultJson->SetNumberField(TEXT("dependency_count"), DependenciesArray.Num());
	ResultJson->SetArrayField(TEXT("dependencies"), DependenciesArray);
	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: find_unused_audio
// ============================================================================

FMonolithActionResult FMonolithAudioQueryActions::FindUnusedAudio(const TSharedPtr<FJsonObject>& Params)
{
	const FString TypeStr = Params->HasField(TEXT("type")) ? Params->GetStringField(TEXT("type")) : TEXT("All");
	const FString PathFilter = Params->HasField(TEXT("path_filter")) ? Params->GetStringField(TEXT("path_filter")) : TEXT("");
	const int32 Limit = Params->HasField(TEXT("limit")) ? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 100;

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Determine which classes to scan
	TArray<UClass*> ClassesToScan;
	if (TypeStr.Equals(TEXT("All"), ESearchCase::IgnoreCase))
	{
		ClassesToScan.Add(USoundWave::StaticClass());
		ClassesToScan.Add(USoundCue::StaticClass());
		if (UClass* MetaSoundClass = FindObject<UClass>(nullptr, TEXT("/Script/MetasoundEngine.MetaSoundSource")))
		{
			ClassesToScan.Add(MetaSoundClass);
		}
	}
	else
	{
		UClass* ResolvedClass = ResolveAudioClass(TypeStr);
		if (!ResolvedClass)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown audio type '%s'"), *TypeStr));
		}
		ClassesToScan.Add(ResolvedClass);
	}

	TArray<TSharedPtr<FJsonValue>> UnusedArray;
	int32 ScannedCount = 0;

	for (UClass* AssetClass : ClassesToScan)
	{
		if (UnusedArray.Num() >= Limit) break;

		TArray<FAssetData> AssetDataList;
		AR.GetAssetsByClass(AssetClass->GetClassPathName(), AssetDataList, true);

		for (const FAssetData& AssetData : AssetDataList)
		{
			if (UnusedArray.Num() >= Limit) break;

			const FString AssetPathStr = AssetData.GetObjectPathString();
			if (!PathFilter.IsEmpty() && !AssetPathStr.StartsWith(PathFilter))
			{
				continue;
			}

			ScannedCount++;

			// Check referencers
			FString PkgName = AssetData.PackageName.ToString();
			TArray<FAssetIdentifier> Referencers;
			AR.GetReferencers(FAssetIdentifier(FName(*PkgName)), Referencers);

			if (Referencers.Num() == 0)
			{
				auto AssetJson = MakeShared<FJsonObject>();
				AssetJson->SetStringField(TEXT("path"), AssetPathStr);
				AssetJson->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
				AssetJson->SetStringField(TEXT("type"), AssetData.AssetClassPath.GetAssetName().ToString());
				UnusedArray.Add(MakeShared<FJsonValueObject>(AssetJson));
			}
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetNumberField(TEXT("scanned"), ScannedCount);
	ResultJson->SetNumberField(TEXT("unused_count"), UnusedArray.Num());
	ResultJson->SetBoolField(TEXT("limit_reached"), UnusedArray.Num() >= Limit);
	ResultJson->SetArrayField(TEXT("unused_assets"), UnusedArray);
	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: find_sounds_without_class
// ============================================================================

FMonolithActionResult FMonolithAudioQueryActions::FindSoundsWithoutClass(const TSharedPtr<FJsonObject>& Params)
{
	const FString PathFilter = Params->HasField(TEXT("path_filter")) ? Params->GetStringField(TEXT("path_filter")) : TEXT("");
	const int32 Limit = Params->HasField(TEXT("limit")) ? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 100;

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Scan SoundWave and SoundCue (the most common USoundBase derivatives)
	TArray<UClass*> ClassesToScan = { USoundWave::StaticClass(), USoundCue::StaticClass() };
	if (UClass* MetaSoundClass = FindObject<UClass>(nullptr, TEXT("/Script/MetasoundEngine.MetaSoundSource")))
	{
		ClassesToScan.Add(MetaSoundClass);
	}

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 ScannedCount = 0;
	bool bLimitReached = false;

	for (UClass* AssetClass : ClassesToScan)
	{
		if (bLimitReached) break;

		TArray<FAssetData> AssetDataList;
		AR.GetAssetsByClass(AssetClass->GetClassPathName(), AssetDataList, true);

		for (const FAssetData& AssetData : AssetDataList)
		{
			if (ResultsArray.Num() >= Limit)
			{
				bLimitReached = true;
				break;
			}

			const FString AssetPathStr = AssetData.GetObjectPathString();
			if (!PathFilter.IsEmpty() && !AssetPathStr.StartsWith(PathFilter))
			{
				continue;
			}

			++ScannedCount;

			// Load asset to check SoundClassObject
			USoundBase* SoundBase = Cast<USoundBase>(AssetData.GetAsset());
			if (SoundBase && !SoundBase->SoundClassObject)
			{
				auto AssetJson = MakeShared<FJsonObject>();
				AssetJson->SetStringField(TEXT("path"), AssetPathStr);
				AssetJson->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
				AssetJson->SetStringField(TEXT("type"), AssetData.AssetClassPath.GetAssetName().ToString());
				ResultsArray.Add(MakeShared<FJsonValueObject>(AssetJson));
			}
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetNumberField(TEXT("scanned"), ScannedCount);
	ResultJson->SetNumberField(TEXT("count"), ResultsArray.Num());
	ResultJson->SetBoolField(TEXT("limit_reached"), bLimitReached);
	ResultJson->SetArrayField(TEXT("sounds_without_class"), ResultsArray);
	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: find_unattenuated_sounds
// ============================================================================

FMonolithActionResult FMonolithAudioQueryActions::FindUnattenuatedSounds(const TSharedPtr<FJsonObject>& Params)
{
	const FString PathFilter = Params->HasField(TEXT("path_filter")) ? Params->GetStringField(TEXT("path_filter")) : TEXT("");

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<UClass*> ClassesToScan = { USoundWave::StaticClass(), USoundCue::StaticClass() };
	if (UClass* MetaSoundClass = FindObject<UClass>(nullptr, TEXT("/Script/MetasoundEngine.MetaSoundSource")))
	{
		ClassesToScan.Add(MetaSoundClass);
	}

	TArray<TSharedPtr<FJsonValue>> ResultsArray;

	for (UClass* AssetClass : ClassesToScan)
	{
		TArray<FAssetData> AssetDataList;
		AR.GetAssetsByClass(AssetClass->GetClassPathName(), AssetDataList, true);

		for (const FAssetData& AssetData : AssetDataList)
		{
			const FString AssetPathStr = AssetData.GetObjectPathString();
			if (!PathFilter.IsEmpty() && !AssetPathStr.StartsWith(PathFilter))
			{
				continue;
			}

			USoundBase* SoundBase = Cast<USoundBase>(AssetData.GetAsset());
			if (!SoundBase) continue;

			// Check: no attenuation asset assigned AND no override attenuation
			if (!SoundBase->AttenuationSettings)
			{
				// Check bOverrideAttenuation — only applicable to SoundCue
				bool bHasOverride = false;
				USoundCue* SoundCue = Cast<USoundCue>(SoundBase);
				if (SoundCue && SoundCue->bOverrideAttenuation)
				{
					bHasOverride = true;
				}

				if (!bHasOverride)
				{
					auto AssetJson = MakeShared<FJsonObject>();
					AssetJson->SetStringField(TEXT("path"), AssetPathStr);
					AssetJson->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
					AssetJson->SetStringField(TEXT("type"), AssetData.AssetClassPath.GetAssetName().ToString());
					ResultsArray.Add(MakeShared<FJsonValueObject>(AssetJson));
				}
			}
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetNumberField(TEXT("count"), ResultsArray.Num());
	ResultJson->SetArrayField(TEXT("unattenuated_sounds"), ResultsArray);
	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_audio_stats
// ============================================================================

FMonolithActionResult FMonolithAudioQueryActions::GetAudioStats(const TSharedPtr<FJsonObject>& Params)
{
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Type counts
	auto CountsJson = MakeShared<FJsonObject>();
	int32 TotalCount = 0;

	auto CountClass = [&](const FString& TypeName, UClass* Class)
	{
		TArray<FAssetData> Assets;
		AR.GetAssetsByClass(Class->GetClassPathName(), Assets, true);
		CountsJson->SetNumberField(TypeName, Assets.Num());
		TotalCount += Assets.Num();
		return Assets;
	};

	TArray<FAssetData> SoundWaveAssets = CountClass(TEXT("SoundWave"), USoundWave::StaticClass());
	CountClass(TEXT("SoundCue"), USoundCue::StaticClass());
	CountClass(TEXT("SoundClass"), USoundClass::StaticClass());
	CountClass(TEXT("SoundAttenuation"), USoundAttenuation::StaticClass());
	CountClass(TEXT("SoundSubmix"), USoundSubmix::StaticClass());
	CountClass(TEXT("SoundConcurrency"), USoundConcurrency::StaticClass());
	CountClass(TEXT("SoundMix"), USoundMix::StaticClass());

	// MetaSoundSource (dynamic)
	if (UClass* MetaSoundClass = FindObject<UClass>(nullptr, TEXT("/Script/MetasoundEngine.MetaSoundSource")))
	{
		CountClass(TEXT("MetaSoundSource"), MetaSoundClass);
	}

	// Compression breakdown and size estimate (SoundWave only)
	TMap<FString, int32> CompressionCounts;
	int64 TotalEstimatedSizeBytes = 0;

	for (const FAssetData& AssetData : SoundWaveAssets)
	{
		USoundWave* SW = Cast<USoundWave>(AssetData.GetAsset());
		if (!SW) continue;

		// Phase F #6: byte/enum-tolerant compression read. Previously always reported "Unknown"
		// because the property is FByteProperty in UE 5.7 (UENUM, not UENUM(BlueprintType)).
		const uint8 CompTypeVal = ReadSoundAssetCompressionType(SW);
		const FString CompType = CompressionTypeToString(CompTypeVal);
		CompressionCounts.FindOrAdd(CompType)++;

		TotalEstimatedSizeBytes += SW->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal);
	}

	auto CompressionJson = MakeShared<FJsonObject>();
	for (const auto& Pair : CompressionCounts)
	{
		CompressionJson->SetNumberField(Pair.Key, Pair.Value);
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetNumberField(TEXT("total_audio_assets"), TotalCount);
	ResultJson->SetObjectField(TEXT("counts_by_type"), CountsJson);
	ResultJson->SetObjectField(TEXT("compression_breakdown"), CompressionJson);
	ResultJson->SetNumberField(TEXT("total_estimated_size_bytes"), static_cast<double>(TotalEstimatedSizeBytes));
	ResultJson->SetNumberField(TEXT("total_estimated_size_mb"),
		static_cast<double>(TotalEstimatedSizeBytes) / (1024.0 * 1024.0));

	return FMonolithActionResult::Success(ResultJson);
}
