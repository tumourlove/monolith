#pragma once
#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithBlueprintComponentActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult HandleAddComponent(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveComponent(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRenameComponent(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleReparentComponent(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetComponentProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDuplicateComponent(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSwapComponentClass(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDumpInstanceComponentValues(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRestoreInstanceComponentValues(const TSharedPtr<FJsonObject>& Params);
};
