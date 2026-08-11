#pragma once

#include "MonolithIndexer.h"
#include "Dom/JsonObject.h"

/**
 * Indexes UDataAsset subclasses by serializing all UPROPERTY defaults to JSON.
 * Covers AbilitySets, PawnData, InputConfigs, and any custom DataAssets whose
 * configuration lives in native properties rather than Blueprint graphs.
 */
class FDataAssetIndexer : public IMonolithIndexer
{
public:
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return {
			TEXT("PrimaryDataAsset"),
			TEXT("DataAsset")
		};
	}

	virtual bool IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual FString GetName() const override { return TEXT("DataAssetIndexer"); }

private:
	static TSharedPtr<FJsonObject> SerializeObjectProperties(UObject* Object);
};
