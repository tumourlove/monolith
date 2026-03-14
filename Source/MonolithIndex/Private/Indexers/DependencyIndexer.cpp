#include "Indexers/DependencyIndexer.h"
#include "MonolithSettings.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

bool FDependencyIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	// This indexer ignores individual asset params -- processes ALL assets at once
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetData> AllAssets;
	FARFilter Filter;
	for (const FName& ContentPath : UMonolithSettings::GetIndexedContentPaths())
	{
		Filter.PackagePaths.Add(ContentPath);
	}
	Filter.bRecursivePaths = true;
	Registry.GetAssets(Filter, AllAssets);

	int32 DepsInserted = 0;

	for (const FAssetData& Source : AllAssets)
	{
		int64 SourceId = DB.GetAssetId(Source.PackageName.ToString());
		if (SourceId < 0) continue;

		// Get hard dependencies
		TArray<FAssetIdentifier> HardDeps;
		Registry.GetDependencies(Source.PackageName, HardDeps,
			UE::AssetRegistry::EDependencyCategory::Package,
			UE::AssetRegistry::EDependencyQuery::Hard);

		for (const FAssetIdentifier& Dep : HardDeps)
		{
			FString DepPath = Dep.PackageName.ToString();
			if (!UMonolithSettings::IsIndexedContentPath(DepPath)) continue;

			int64 TargetId = DB.GetAssetId(DepPath);
			if (TargetId < 0) continue;

			FIndexedDependency IndexedDep;
			IndexedDep.SourceAssetId = SourceId;
			IndexedDep.TargetAssetId = TargetId;
			IndexedDep.DependencyType = TEXT("Hard");
			DB.InsertDependency(IndexedDep);
			DepsInserted++;
		}

		// Get soft dependencies
		TArray<FAssetIdentifier> SoftDeps;
		Registry.GetDependencies(Source.PackageName, SoftDeps,
			UE::AssetRegistry::EDependencyCategory::Package,
			UE::AssetRegistry::EDependencyQuery::Soft);

		for (const FAssetIdentifier& Dep : SoftDeps)
		{
			FString DepPath = Dep.PackageName.ToString();
			if (!UMonolithSettings::IsIndexedContentPath(DepPath)) continue;

			int64 TargetId = DB.GetAssetId(DepPath);
			if (TargetId < 0) continue;

			FIndexedDependency IndexedDep;
			IndexedDep.SourceAssetId = SourceId;
			IndexedDep.TargetAssetId = TargetId;
			IndexedDep.DependencyType = TEXT("Soft");
			DB.InsertDependency(IndexedDep);
			DepsInserted++;
		}
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("DependencyIndexer: inserted %d dependency edges"), DepsInserted);
	return true;
}
