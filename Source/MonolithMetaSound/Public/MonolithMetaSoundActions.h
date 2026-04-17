#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class UMetaSoundSource;
class UMetaSoundPatch;
struct FMetasoundFrontendDocument;
struct FMetasoundFrontendGraph;
struct FMetasoundFrontendNode;
struct FMetasoundFrontendEdge;
struct FMetasoundFrontendVertex;

class FMonolithMetaSoundActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// --- Graph Reading ---
	static FMonolithActionResult ListGraphs(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetGraphData(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetGraphSummary(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetNodeDetails(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetConnections(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetVariables(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetUserParameters(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SearchNodes(const TSharedPtr<FJsonObject>& Params);

	// --- Asset Info ---
	static FMonolithActionResult GetMetaSoundInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetDependencies(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListMetaSounds(const TSharedPtr<FJsonObject>& Params);

	// --- Validation ---
	static FMonolithActionResult ValidateMetaSound(const TSharedPtr<FJsonObject>& Params);
};
