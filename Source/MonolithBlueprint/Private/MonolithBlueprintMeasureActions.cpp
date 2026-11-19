#include "MonolithBlueprintMeasureActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithBlueprintNodeGeometry.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Misc/App.h"
#include "Editor.h"

#include "Framework/Application/SlateApplication.h"
#include "Layout/ArrangedChildren.h"
#include "Layout/Geometry.h"
#include "Widgets/SWidget.h"

#include "GraphEditor.h"
#include "SGraphPanel.h"
#include "SGraphNode.h"
#include "SGraphPin.h"
#include "BlueprintEditorModule.h"
#include "Kismet2/KismetEditorUtilities.h"

// ============================================================================
//  Measurement core
// ============================================================================

namespace
{
	/** Deepest widget nesting walked below a node widget looking for pin rows. */
	constexpr int32 BlueprintMeasureMaxWalkDepth = 64;

	float BlueprintMeasureRound(float Value)
	{
		return FMath::RoundToFloat(Value * 10.f) / 10.f;
	}

	/**
	 * Walk the arranged widget tree below Widget. Every pin widget found has its
	 * row centre recorded in Widget-root space. FindChildGeometry is protected on
	 * SWidget, so this rebuilds the arrangement through the public ArrangeChildren.
	 */
	void BlueprintMeasureWalkPins(
		const SWidget& Widget,
		const FGeometry& Geometry,
		const TMap<const SWidget*, const UEdGraphPin*>& PinWidgets,
		TArray<MonolithBlueprintGraphMeasure::FMeasuredPin>& OutPins,
		int32 Depth)
	{
		if (Depth > BlueprintMeasureMaxWalkDepth || OutPins.Num() >= PinWidgets.Num())
		{
			return;
		}

		FArrangedChildren Arranged(EVisibility::Visible);
		Widget.ArrangeChildren(Geometry, Arranged, /*bUpdateAttributes=*/true);

		for (int32 Index = 0; Index < Arranged.Num(); ++Index)
		{
			const FArrangedWidget& Child = Arranged[Index];
			const SWidget& ChildWidget = Child.Widget.Get();

			if (const UEdGraphPin* const* Pin = PinWidgets.Find(&ChildWidget))
			{
				MonolithBlueprintGraphMeasure::FMeasuredPin& Out = OutPins.AddDefaulted_GetRef();
				Out.Pin = *Pin;
				// The node root geometry is unscaled, so absolute == node-local.
				const FVector2f Centre = Child.Geometry.GetAbsolutePositionAtCoordinates(FVector2f(0.5f, 0.5f));
				const FVector2f Size = Child.Geometry.GetLocalSize();
				Out.Centre = Centre;
				Out.Size = Size;
				continue;
			}

			BlueprintMeasureWalkPins(ChildWidget, Child.Geometry, PinWidgets, OutPins, Depth + 1);
		}
	}

	void BlueprintMeasureOneNode(SGraphNode& NodeWidget, MonolithBlueprintGraphMeasure::FMeasuredNode& OutNode)
	{
		NodeWidget.SlatePrepass(1.0f);
		const FVector2f Desired = NodeWidget.GetDesiredSize();
		OutNode.Size = Desired;

		TArray<TSharedRef<SWidget>> PinWidgetRefs;
		NodeWidget.GetPins(PinWidgetRefs);

		TMap<const SWidget*, const UEdGraphPin*> PinWidgets;
		PinWidgets.Reserve(PinWidgetRefs.Num());
		for (const TSharedRef<SWidget>& PinWidget : PinWidgetRefs)
		{
			const TSharedRef<SGraphPin> GraphPin = StaticCastSharedRef<SGraphPin>(PinWidget);
			if (const UEdGraphPin* Pin = GraphPin->GetPinObj())
			{
				PinWidgets.Add(&PinWidget.Get(), Pin);
			}
		}

		if (PinWidgets.Num() == 0 || Desired.X <= 0.f || Desired.Y <= 0.f)
		{
			return;
		}

		const FGeometry Root = FGeometry::MakeRoot(Desired, FSlateLayoutTransform());
		BlueprintMeasureWalkPins(NodeWidget, Root, PinWidgets, OutNode.Pins, 0);
	}
}

namespace MonolithBlueprintGraphMeasure
{
	bool CanMeasure(FString& OutError)
	{
		if (IsRunningCommandlet())
		{
			OutError = TEXT("Node measurement needs the editor UI; it is not available in a commandlet.");
			return false;
		}
		if (!GEditor)
		{
			OutError = TEXT("Node measurement needs the editor (GEditor is null).");
			return false;
		}
		if (!FApp::CanEverRender())
		{
			OutError = TEXT("Node measurement needs a rendering editor; it is not available with -nullrhi. Relaunch without -nullrhi (-unattended is fine).");
			return false;
		}
		if (!FSlateApplication::IsInitialized())
		{
			OutError = TEXT("Node measurement needs Slate, and the Slate application is not initialized.");
			return false;
		}
		return true;
	}

	TSharedPtr<SGraphEditor> OpenBlueprintGraph(UEdGraph* Graph, FString& OutError)
	{
		if (!Graph)
		{
			OutError = TEXT("No graph to open.");
			return nullptr;
		}

		// Opens (or finds) the owning Blueprint's editor. Walks the graph's outer
		// chain, so function, macro and nested graphs resolve too.
		TSharedPtr<IBlueprintEditor> BlueprintEditor = FKismetEditorUtilities::GetIBlueprintEditorForObject(Graph, /*bOpenEditor=*/true);
		if (!BlueprintEditor.IsValid())
		{
			OutError = FString::Printf(TEXT("Could not open a Blueprint editor for graph '%s'."), *Graph->GetName());
			return nullptr;
		}

		TSharedPtr<SGraphEditor> GraphEditor = BlueprintEditor->OpenGraphAndBringToFront(Graph, /*bSetFocus=*/false);
		if (!GraphEditor.IsValid())
		{
			GraphEditor = SGraphEditor::FindGraphEditorForGraph(Graph);
		}
		if (!GraphEditor.IsValid())
		{
			OutError = FString::Printf(TEXT("The Blueprint editor did not produce a graph editor for graph '%s'."), *Graph->GetName());
			return nullptr;
		}
		return GraphEditor;
	}

	bool MeasureGraphEditor(
		const TSharedPtr<SGraphEditor>& GraphEditor,
		const TSet<const UEdGraphNode*>* NodeFilter,
		bool bRestoreView,
		FMeasuredGraph& OutMeasured,
		TArray<const UEdGraphNode*>& OutMissing,
		FString& OutError)
	{
		if (!GraphEditor.IsValid())
		{
			OutError = TEXT("No graph editor to measure.");
			return false;
		}

		UEdGraph* Graph = GraphEditor->GetCurrentGraph();
		SGraphPanel* Panel = GraphEditor->GetGraphPanel();
		if (!Graph || !Panel)
		{
			OutError = TEXT("The graph editor has no graph panel yet.");
			return false;
		}

		FVector2f OldLocation = FVector2f::ZeroVector;
		float OldZoom = 1.f;
		GraphEditor->GetViewLocation(OldLocation, OldZoom);

		// Zoom 1:1 sets the panel LOD synchronously (RestoreViewSettings ->
		// PostChangedZoom), so the node widgets lay out at full detail.
		GraphEditor->SetViewLocation(OldLocation, 1.0f);

		// Creates widgets for any node that does not have one yet.
		Panel->Update();

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (NodeFilter && !NodeFilter->Contains(Node)) continue;

			TSharedPtr<SGraphNode> NodeWidget = Panel->GetNodeWidgetFromGuid(Node->NodeGuid);
			if (!NodeWidget.IsValid())
			{
				OutMissing.Add(Node);
				continue;
			}

			FMeasuredNode& Measured = OutMeasured.Add(static_cast<const UEdGraphNode*>(Node));
			BlueprintMeasureOneNode(*NodeWidget, Measured);
		}

		if (bRestoreView && OldZoom > 0.f)
		{
			GraphEditor->SetViewLocation(OldLocation, OldZoom);
		}
		return true;
	}

	bool MeasureBlueprintGraph(UEdGraph* Graph, bool bRestoreView, FMeasuredGraph& OutMeasured, FString& OutError)
	{
		if (!CanMeasure(OutError))
		{
			return false;
		}
		TSharedPtr<SGraphEditor> GraphEditor = OpenBlueprintGraph(Graph, OutError);
		if (!GraphEditor.IsValid())
		{
			return false;
		}
		TArray<const UEdGraphNode*> Missing;
		return MeasureGraphEditor(GraphEditor, nullptr, bRestoreView, OutMeasured, Missing, OutError);
	}
}

// ============================================================================
//  Registration
// ============================================================================

void FMonolithBlueprintMeasureActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("measure_graph_nodes"),
		TEXT("Measure the real on-screen size of Blueprint graph nodes and the position of each pin. "
			"Opens the graph in the Blueprint editor, lays the nodes out at zoom 1:1 and returns per-node w/h plus each "
			"visible pin's row centre (x, y) relative to the node's top-left corner, in graph units. "
			"Also returns the estimate auto_layout uses (est_w/est_h) for comparison. Needs the editor UI: fails cleanly "
			"under -nullrhi or in a commandlet. Read-only apart from opening the editor tab."),
		FMonolithActionHandler::CreateStatic(&HandleMeasureGraphNodes),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (default: the first event graph)"))
			.Optional(TEXT("node_ids"), TEXT("array"), TEXT("Node IDs (as returned by get_graph_data) to measure. Default: every node in the graph"))
			.Optional(TEXT("include_pins"), TEXT("boolean"), TEXT("Include per-pin centre offsets"), TEXT("true"))
			.Optional(TEXT("restore_view"), TEXT("boolean"), TEXT("Restore the graph editor's previous view offset and zoom after measuring"), TEXT("true"))
			.Build());
}

// ============================================================================
//  measure_graph_nodes
// ============================================================================

FMonolithActionResult FMonolithBlueprintMeasureActions::HandleMeasureGraphNodes(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);
	UEdGraph* Graph = MonolithBlueprintInternal::FindGraphByName(BP, GraphName);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Graph '%s' not found in Blueprint '%s'. Use blueprint.list_graphs to see the graph names."),
			*GraphName, *AssetPath));
	}

	bool bIncludePins = true;
	Params->TryGetBoolField(TEXT("include_pins"), bIncludePins);
	bool bRestoreView = true;
	Params->TryGetBoolField(TEXT("restore_view"), bRestoreView);

	// Optional node filter, resolved up front so unknown IDs are reported.
	TSet<const UEdGraphNode*> Filter;
	TArray<FString> UnknownIds;
	const TArray<TSharedPtr<FJsonValue>>* NodeIdsArr = nullptr;
	const bool bHasFilter = Params->TryGetArrayField(TEXT("node_ids"), NodeIdsArr) && NodeIdsArr && NodeIdsArr->Num() > 0;
	if (bHasFilter)
	{
		for (const TSharedPtr<FJsonValue>& Value : *NodeIdsArr)
		{
			const FString Id = Value.IsValid() ? Value->AsString() : FString();
			if (Id.IsEmpty()) continue;

			const UEdGraphNode* Found = nullptr;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->GetName() == Id) { Found = Node; break; }
			}
			if (Found) { Filter.Add(Found); }
			else { UnknownIds.Add(Id); }
		}
		if (Filter.Num() == 0)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("None of the node_ids were found in graph '%s': %s"),
				*Graph->GetName(), *FString::Join(UnknownIds, TEXT(", "))));
		}
	}

	FString Error;
	if (!MonolithBlueprintGraphMeasure::CanMeasure(Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<SGraphEditor> GraphEditor = MonolithBlueprintGraphMeasure::OpenBlueprintGraph(Graph, Error);
	if (!GraphEditor.IsValid())
	{
		return FMonolithActionResult::Error(Error);
	}

	MonolithBlueprintGraphMeasure::FMeasuredGraph Measured;
	TArray<const UEdGraphNode*> Missing;
	if (!MonolithBlueprintGraphMeasure::MeasureGraphEditor(GraphEditor, bHasFilter ? &Filter : nullptr, bRestoreView, Measured, Missing, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> NodesArr;
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		const MonolithBlueprintGraphMeasure::FMeasuredNode* M = Node ? Measured.Find(Node) : nullptr;
		if (!M) continue;

		TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
		NObj->SetStringField(TEXT("id"), Node->GetName());
		NObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());

		TArray<TSharedPtr<FJsonValue>> PosArr;
		PosArr.Add(MakeShared<FJsonValueNumber>(Node->NodePosX));
		PosArr.Add(MakeShared<FJsonValueNumber>(Node->NodePosY));
		NObj->SetArrayField(TEXT("pos"), PosArr);

		NObj->SetNumberField(TEXT("w"), BlueprintMeasureRound(M->Size.X));
		NObj->SetNumberField(TEXT("h"), BlueprintMeasureRound(M->Size.Y));

		float EstW = 0.f, EstH = 0.f;
		MonolithBlueprintNodeGeometry::EstimateNodeSize(Node, EstW, EstH);
		NObj->SetNumberField(TEXT("est_w"), EstW);
		NObj->SetNumberField(TEXT("est_h"), EstH);

		if (bIncludePins)
		{
			TArray<TSharedPtr<FJsonValue>> PinsArr;
			for (const MonolithBlueprintGraphMeasure::FMeasuredPin& P : M->Pins)
			{
				if (!P.Pin) continue;
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"), P.Pin->PinName.ToString());
				PObj->SetStringField(TEXT("id"), P.Pin->PinId.ToString());
				PObj->SetStringField(TEXT("direction"), P.Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
				PObj->SetStringField(TEXT("category"), P.Pin->PinType.PinCategory.ToString());
				PObj->SetNumberField(TEXT("x"), BlueprintMeasureRound(P.Centre.X));
				PObj->SetNumberField(TEXT("y"), BlueprintMeasureRound(P.Centre.Y));
				PObj->SetNumberField(TEXT("w"), BlueprintMeasureRound(P.Size.X));
				PObj->SetNumberField(TEXT("h"), BlueprintMeasureRound(P.Size.Y));
				PinsArr.Add(MakeShared<FJsonValueObject>(PObj));
			}
			NObj->SetArrayField(TEXT("pins"), PinsArr);
		}

		NodesArr.Add(MakeShared<FJsonValueObject>(NObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("graph_name"), Graph->GetName());
	Result->SetNumberField(TEXT("zoom"), 1.0);
	Result->SetStringField(TEXT("units"), TEXT("graph units at zoom 1:1; pin x/y are row centres relative to the node's top-left"));
	Result->SetNumberField(TEXT("node_count"), NodesArr.Num());
	Result->SetArrayField(TEXT("nodes"), NodesArr);

	if (Missing.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> MissingArr;
		for (const UEdGraphNode* Node : Missing)
		{
			MissingArr.Add(MakeShared<FJsonValueString>(Node->GetName()));
		}
		Result->SetArrayField(TEXT("unmeasured"), MissingArr);
	}
	if (UnknownIds.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> UnknownArr;
		for (const FString& Id : UnknownIds)
		{
			UnknownArr.Add(MakeShared<FJsonValueString>(Id));
		}
		Result->SetArrayField(TEXT("unknown_node_ids"), UnknownArr);
	}

	return FMonolithActionResult::Success(Result);
}
