#include "MonolithLogicDriverIndexer.h"
#include "AssetRegistry/AssetData.h"

#if WITH_LOGICDRIVER

#include "MonolithLogicDriverInternal.h"
#include "AssetRegistry/AssetRegistryModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithLDIndexer, Log, All);

void FStateMachineIndexer::Register()
{
	// TODO Phase 2: Register with MonolithIndex system (requires understanding the indexer registration API)
	// For now, just log that we're alive so module startup is visible.
	UE_LOG(LogMonolithLDIndexer, Log, TEXT("FStateMachineIndexer: registered (Phase 1 skeleton — not yet hooked into MonolithIndex)"));
}

void FStateMachineIndexer::Unregister()
{
	UE_LOG(LogMonolithLDIndexer, Log, TEXT("FStateMachineIndexer: unregistered"));
}

void FStateMachineIndexer::IndexStateMachine(const FAssetData& AssetData)
{
	// TODO Phase 2: Extract SM structure (states, transitions, node classes, variables)
	// and push into MonolithIndex for cross-reference queries.
	UE_LOG(LogMonolithLDIndexer, Verbose, TEXT("FStateMachineIndexer::IndexStateMachine: '%s' (stub — no-op)"), *AssetData.AssetName.ToString());
}

void FStateMachineIndexer::ReindexAll()
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Find all SM Blueprints
	UClass* SMBPClass = MonolithLD::GetSMBlueprintClass();
	if (!SMBPClass)
	{
		UE_LOG(LogMonolithLDIndexer, Warning, TEXT("FStateMachineIndexer::ReindexAll: SMBlueprint class not found — Logic Driver not loaded?"));
		return;
	}
	FARFilter Filter;
	Filter.ClassPaths.Add(SMBPClass->GetClassPathName());
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> SMAssets;
	AssetRegistry.GetAssets(Filter, SMAssets);

	UE_LOG(LogMonolithLDIndexer, Log, TEXT("FStateMachineIndexer::ReindexAll: found %d SM Blueprints"), SMAssets.Num());

	for (const FAssetData& Asset : SMAssets)
	{
		IndexStateMachine(Asset);
	}

	// Also find Node Blueprints
	UClass* NodeBPClass = FindFirstObject<UClass>(TEXT("SMNodeBlueprint"), EFindFirstObjectOptions::NativeFirst);
	if (NodeBPClass)
	{
		FARFilter NodeFilter;
		NodeFilter.ClassPaths.Add(NodeBPClass->GetClassPathName());
		NodeFilter.bRecursiveClasses = true;

		TArray<FAssetData> NodeAssets;
		AssetRegistry.GetAssets(NodeFilter, NodeAssets);

		UE_LOG(LogMonolithLDIndexer, Log, TEXT("FStateMachineIndexer::ReindexAll: found %d Node Blueprints"), NodeAssets.Num());

		// TODO Phase 2: Index node blueprints as well
	}

	UE_LOG(LogMonolithLDIndexer, Log, TEXT("FStateMachineIndexer::ReindexAll: complete (Phase 1 — index stubs only)"));
}

#endif // WITH_LOGICDRIVER
