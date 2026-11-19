#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * material.measure_graph_nodes (issue #161): real node sizes and pin
 * positions for a material or material-function graph, read from the
 * material editor's graph widgets at zoom 1:1.
 */
class FMonolithMaterialMeasureActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult HandleMeasureGraphNodes(const TSharedPtr<FJsonObject>& Params);
};
