#pragma once

#include "CoreMinimal.h"

#if WITH_METASOUND

class FMonolithToolRegistry;
struct FMonolithActionResult;

class FMonolithAudioMetaSoundActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// ---- Core CRUD (12) ----
	static FMonolithActionResult CreateMetaSoundSource(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateMetaSoundPatch(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddMetaSoundNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RemoveMetaSoundNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ConnectMetaSoundNodes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DisconnectMetaSoundNodes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddMetaSoundInput(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddMetaSoundOutput(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetMetaSoundInputDefault(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddMetaSoundInterface(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetMetaSoundGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListMetaSoundConnections(const TSharedPtr<FJsonObject>& Params);

	// ---- Query & Discovery (5) ----
	static FMonolithActionResult ListAvailableMetaSoundNodes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetMetaSoundNodeInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult FindMetaSoundNodeInputs(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult FindMetaSoundNodeOutputs(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetMetaSoundInputNames(const TSharedPtr<FJsonObject>& Params);

	// ---- Build & Templates (8) ----
	static FMonolithActionResult BuildMetaSoundFromSpec(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateMetaSoundPreset(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateOneShotSfx(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateLoopingAmbientMetaSound(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateSynthesizedTone(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateInteractiveMetaSound(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddMetaSoundVariable(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetMetaSoundNodeLocation(const TSharedPtr<FJsonObject>& Params);

	// ---- Helpers ----

	/** Split "/Game/Foo/Bar" into PackagePath="/Game/Foo" and AssetName="Bar" */
	static bool SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName);

	/** Create a FMetasoundFrontendLiteral from a JSON value + data type string */
	static bool CreateLiteralFromJson(
		const TSharedPtr<FJsonValue>& JsonVal, const FString& DataType,
		struct FMetasoundFrontendLiteral& OutLiteral, FString& OutError);

	/** Parse a node class array ["Namespace","Name","Variant"] into FMetasoundFrontendClassName */
	static bool ParseNodeClassName(
		const TArray<TSharedPtr<FJsonValue>>& ClassArray,
		struct FMetasoundFrontendClassName& OutClassName, FString& OutError);

	/** Get or create a builder for an existing MetaSound asset by path */
	static class UMetaSoundBuilderBase* GetBuilderForAsset(const FString& AssetPath, FString& OutError);

	/** Resolve a node_id_or_handle string to a FMetaSoundNodeHandle within a builder.
	 *  Phase F #3: when AssetPath is non-empty, also consults the user-label alias registry
	 *  populated by add_metasound_node so callers can refer to nodes by their assigned label. */
	static bool ResolveNodeHandle(
		class UMetaSoundBuilderBase* Builder, const FString& NodeIdOrHandle,
		struct FMetaSoundNodeHandle& OutHandle, FString& OutError,
		const FString& AssetPath = FString());

	/** Phase F #3: register a user-supplied label -> engine GUID mapping for later resolution. */
	static void RegisterNodeIdAlias(const FString& AssetPath, const FString& UserLabel, const FGuid& NodeGuid);

	/** Phase F #3: look up a user label registered by add_metasound_node. Returns invalid FGuid on miss. */
	static FGuid LookupNodeIdAlias(const FString& AssetPath, const FString& UserLabel);
};

#endif // WITH_METASOUND
