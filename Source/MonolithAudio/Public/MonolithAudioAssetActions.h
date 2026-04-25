#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithAudioAssetActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// --- Sound Attenuation ---
	static FMonolithActionResult CreateSoundAttenuation(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetAttenuationSettings(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetAttenuationSettings(const TSharedPtr<FJsonObject>& Params);

	// --- Sound Class ---
	static FMonolithActionResult CreateSoundClass(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetSoundClassProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetSoundClassProperties(const TSharedPtr<FJsonObject>& Params);

	// --- Sound Mix ---
	static FMonolithActionResult CreateSoundMix(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetSoundMixSettings(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetSoundMixSettings(const TSharedPtr<FJsonObject>& Params);

	// --- Sound Concurrency ---
	static FMonolithActionResult CreateSoundConcurrency(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetConcurrencySettings(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetConcurrencySettings(const TSharedPtr<FJsonObject>& Params);

	// --- Sound Submix ---
	static FMonolithActionResult CreateSoundSubmix(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetSubmixProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetSubmixProperties(const TSharedPtr<FJsonObject>& Params);

	// --- Test fixtures (F18) ---
	/** Procedurally synthesizes a 16-bit mono sine-tone USoundWave. Zero asset dependencies. */
	static FMonolithActionResult CreateTestWave(const TSharedPtr<FJsonObject>& Params);

	// --- Helpers ---

	/** Split "/Game/Foo/Bar" into PackagePath="/Game/Foo" and AssetName="Bar" */
	static bool SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName);

	/** Load an audio asset by path, using AssetRegistry first, then StaticLoadObject fallback */
	template<typename T>
	static T* LoadAudioAsset(const FString& AssetPath, FString& OutError);

	/** Serialize all UPROPERTY fields of a UStruct instance to a JSON object via reflection */
	static TSharedPtr<FJsonObject> StructToJson(const UStruct* StructDef, const void* StructData);

	/** Apply JSON fields onto a UStruct instance via reflection (partial update) */
	static bool JsonToStruct(const TSharedPtr<FJsonObject>& Json, const UStruct* StructDef, void* StructData, FString& OutError);

	/** Create asset via factory, register, save, return package */
	template<typename TFactory, typename TAsset>
	static TAsset* CreateAudioAsset(const FString& AssetPath, FString& OutError);
};
