#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FProjectSearchAction
{
public:
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FString GetName() { return TEXT("search"); }
	static FString GetDescription() { return TEXT("Full-text search across indexed project assets (name, class, description, path, module) and graph nodes (name, class, type), optionally narrowed with asset_class"); }
	static TSharedPtr<FJsonObject> GetSchema();
};
