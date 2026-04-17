#pragma once

#include "MonolithIndexer.h"

class FMetaSoundIndexer : public IMonolithIndexer
{
public:
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("__MetaSound__") };
	}

	virtual bool IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual FString GetName() const override { return TEXT("MetaSoundIndexer"); }
	virtual bool IsSentinel() const override { return true; }

private:
	void IndexMetaSound(UObject* MetaSoundAsset, FMonolithIndexDatabase& DB, int64 AssetId);
};
