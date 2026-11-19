#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class SGraphEditor;

// ============================================================
//  Real node sizes and pin positions (issue #161)
//
//  Graph-space node sizes are only known once Slate has built the node
//  widgets, so measuring needs the graph open in its asset editor. The
//  measurement opens the graph, forces the panel to zoom 1:1 (the zoomed-out
//  LOD hides pin default-value boxes and makes nodes measure too small),
//  refreshes the panel, prepasses each node widget, and walks the arranged
//  widget tree to find each pin row. The previous view is restored afterwards.
//
//  Used by blueprint.measure_graph_nodes and by auto_layout(measured=true).
//  The namespace is NAMED, never anonymous (full-unity safety).
// ============================================================

namespace MonolithBlueprintGraphMeasure
{
	struct FMeasuredPin
	{
		const UEdGraphPin* Pin = nullptr;
		/** Pin row centre, relative to the node's top-left corner, graph units at zoom 1:1. */
		FVector2f Centre = FVector2f::ZeroVector;
		/** Pin row size. */
		FVector2f Size = FVector2f::ZeroVector;
	};

	struct FMeasuredNode
	{
		FVector2f Size = FVector2f::ZeroVector;
		TArray<FMeasuredPin> Pins;

		const FMeasuredPin* FindPin(const UEdGraphPin* InPin) const
		{
			return Pins.FindByPredicate([InPin](const FMeasuredPin& P) { return P.Pin == InPin; });
		}
	};

	using FMeasuredGraph = TMap<const UEdGraphNode*, FMeasuredNode>;

	/** False (with a reason) when there is no usable Slate application to build node widgets with. */
	bool CanMeasure(FString& OutError);

	/** Open a Blueprint-owned graph in the Blueprint editor and return its graph editor widget. */
	TSharedPtr<SGraphEditor> OpenBlueprintGraph(UEdGraph* Graph, FString& OutError);

	/**
	 * Measure the nodes of the graph shown by GraphEditor.
	 * @param NodeFilter   when non-null, only these nodes are measured
	 * @param bRestoreView restore the editor's previous view offset and zoom afterwards
	 * @param OutMissing   nodes that had no widget (not measured)
	 */
	bool MeasureGraphEditor(
		const TSharedPtr<SGraphEditor>& GraphEditor,
		const TSet<const UEdGraphNode*>* NodeFilter,
		bool bRestoreView,
		FMeasuredGraph& OutMeasured,
		TArray<const UEdGraphNode*>& OutMissing,
		FString& OutError);

	/** Convenience: CanMeasure + OpenBlueprintGraph + MeasureGraphEditor over every node. */
	bool MeasureBlueprintGraph(UEdGraph* Graph, bool bRestoreView, FMeasuredGraph& OutMeasured, FString& OutError);
}

class FMonolithBlueprintMeasureActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult HandleMeasureGraphNodes(const TSharedPtr<FJsonObject>& Params);
};
