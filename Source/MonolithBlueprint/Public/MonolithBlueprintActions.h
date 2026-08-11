#pragma once

#include "CoreMinimal.h"
#include "Engine/Blueprint.h"
#include "MonolithToolRegistry.h"

class FMonolithBlueprintActions
{
public:
	static void RegisterActions();

	static FMonolithActionResult HandleListGraphs(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetGraphData(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetGraphSummary(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetVariables(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetExecutionFlow(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSearchNodes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetComponents(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetComponentDetails(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetFunctions(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetEventDispatchers(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetParentClass(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetInterfaces(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetConstructionScript(const TSharedPtr<FJsonObject>& Params);

	// Wave 3 — Discovery & Resolution
	static FMonolithActionResult HandleSearchFunctions(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetNodeDetails(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetInterfaceFunctions(const TSharedPtr<FJsonObject>& Params);

	// Wave 6 — Inspection & Editing
	static FMonolithActionResult HandleGetFunctionSignature(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetEventDispatcherDetails(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetBlueprintInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleFindVariableReferences(const TSharedPtr<FJsonObject>& Params);

private:
	static UBlueprint* LoadBlueprint(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath);
};
