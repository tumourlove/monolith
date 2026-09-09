#include "MonolithBlueprintLayoutActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Knot.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "Modules/ModuleManager.h"
#include "IMonolithGraphFormatter.h"
#include "MonolithBlueprintNodeGeometry.h"

// ============================================================================
//  Registration
// ============================================================================

void FMonolithBlueprintLayoutActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("auto_layout"),
		TEXT("Auto-layout nodes in a Blueprint graph using a modified Sugiyama algorithm. "
			"Produces clean left-to-right flow: events on left, exec chain flows right, data feeders positioned left of consumers. "
			"Supports full, new-only, and selected-node modes."),
		FMonolithActionHandler::CreateStatic(&HandleAutoLayout),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (default: EventGraph)"))
			.Optional(TEXT("horizontal_spacing"), TEXT("integer"), TEXT("Horizontal spacing between layers in pixels"), TEXT("350"))
			.Optional(TEXT("vertical_spacing"), TEXT("integer"), TEXT("Vertical spacing between nodes in pixels"), TEXT("80"))
			.Optional(TEXT("layout_mode"), TEXT("string"), TEXT("Layout mode: 'all' (default, lays out every node), 'new_only' (moves only nodes at 0,0 or piled on top of another node, leaving hand-placed nodes alone — note that add_node's cascading default placement lands nodes in clear space, so those are NOT candidates; use 'all' to fold new nodes into the flow), 'selected' (only node_ids)"), TEXT("all"))
			.Optional(TEXT("node_ids"), TEXT("array"), TEXT("Array of node IDs to layout (for 'selected' mode)"))
			.Optional(TEXT("formatter"), TEXT("string"), TEXT("Formatter: 'auto' (default, prefers Blueprint Assist if available), 'blueprint_assist' (BA only, fails if unavailable), or 'monolith' (built-in Sugiyama)"), TEXT("auto"))
			.Build());
}

// ============================================================================
//  Internal types and helpers
// ============================================================================

namespace
{
	// --- Node classification ---
	enum class ELayoutNodeKind : uint8
	{
		Entry,      // Event / FunctionEntry — no incoming exec, starts a chain
		ExecChain,  // Has exec pins on both sides (or only input exec)
		DataOnly,   // No exec pins at all (pure math, getters, etc.)
		Comment,    // EdGraphNode_Comment — skip layout, resize after
		Reroute,    // UK2Node_Knot — skip entirely
	};

	// --- Edge classification ---
	struct FLayoutEdge
	{
		int32 FromNode;
		int32 ToNode;
		int32 Weight;   // 5 for exec, 1 for data
		bool bReversed; // True if this was a back-edge (cycle breaking)
	};

	// --- Per-node layout data ---
	struct FLayoutNode
	{
		UEdGraphNode* GraphNode = nullptr;
		int32 Index = -1;
		ELayoutNodeKind Kind = ELayoutNodeKind::DataOnly;
		int32 Layer = -1;           // Assigned during layering
		int32 OrderInLayer = -1;    // Position within layer (crossing minimization)
		float Barycenter = 0.f;
		bool bPinned = false;       // If true, don't move this node

		// Estimated dimensions
		float EstWidth = 300.f;
		float EstHeight = 74.f;

		// Original position (saved before layout for comment containment checks)
		int32 OrigX = 0;
		int32 OrigY = 0;

		// Final position
		int32 FinalX = 0;
		int32 FinalY = 0;

		// Adjacency (indices into the edge array)
		TArray<int32> OutEdges;
		TArray<int32> InEdges;
	};

	// --- Helpers ---

	bool IsExecPin(const UEdGraphPin* Pin)
	{
		return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
	}

	bool HasExecPins(UEdGraphNode* Node)
	{
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && !Pin->bHidden && IsExecPin(Pin))
			{
				return true;
			}
		}
		return false;
	}

	bool HasIncomingExec(UEdGraphNode* Node)
	{
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && !Pin->bHidden && IsExecPin(Pin) && Pin->Direction == EGPD_Input)
			{
				// Check if anything is actually connected
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (Linked && Linked->GetOwningNode()) return true;
				}
			}
		}
		return false;
	}

	bool HasOutgoingExec(UEdGraphNode* Node)
	{
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && !Pin->bHidden && IsExecPin(Pin) && Pin->Direction == EGPD_Output)
			{
				return true;
			}
		}
		return false;
	}

	ELayoutNodeKind ClassifyNode(UEdGraphNode* Node)
	{
		if (Cast<UEdGraphNode_Comment>(Node))
		{
			return ELayoutNodeKind::Comment;
		}
		if (Cast<UK2Node_Knot>(Node))
		{
			return ELayoutNodeKind::Reroute;
		}

		bool bHasExec = HasExecPins(Node);
		if (!bHasExec)
		{
			return ELayoutNodeKind::DataOnly;
		}

		// Entry nodes: have outgoing exec but no incoming exec connections
		// Events, FunctionEntry, CustomEvent all qualify
		if (Cast<UK2Node_Event>(Node) || Cast<UK2Node_CustomEvent>(Node) || Cast<UK2Node_FunctionEntry>(Node))
		{
			return ELayoutNodeKind::Entry;
		}

		// Also classify as entry if it has exec outputs but no connected incoming exec
		if (HasOutgoingExec(Node) && !HasIncomingExec(Node))
		{
			return ELayoutNodeKind::Entry;
		}

		return ELayoutNodeKind::ExecChain;
	}

	// The estimate itself lives in MonolithBlueprintNodeGeometry so that
	// add_node's default placement and this file's overlap test cannot disagree
	// about what a node occupies (issue #132).
	void EstimateNodeSize(FLayoutNode& LN)
	{
		MonolithBlueprintNodeGeometry::EstimateNodeSize(LN.GraphNode, LN.EstWidth, LN.EstHeight);
	}

	/** The node's footprint at the position it held when layout started. */
	MonolithBlueprintNodeGeometry::FNodeRect OriginalRect(const FLayoutNode& LN)
	{
		MonolithBlueprintNodeGeometry::FNodeRect Rect;
		Rect.X = (float)LN.OrigX;
		Rect.Y = (float)LN.OrigY;
		Rect.Width = LN.EstWidth;
		Rect.Height = LN.EstHeight;
		return Rect;
	}

	// Count crossings between two adjacent layers
	int32 CountCrossings(
		const TArray<int32>& LayerA,
		const TArray<int32>& LayerB,
		const TArray<FLayoutNode>& Nodes,
		const TArray<FLayoutEdge>& Edges)
	{
		// Build edge list between the two layers as (posA, posB) pairs
		TArray<TPair<int32, int32>> EdgePositions;

		// Map node index -> position in its layer
		TMap<int32, int32> PosInLayer;
		for (int32 i = 0; i < LayerA.Num(); i++) PosInLayer.Add(LayerA[i], i);
		for (int32 i = 0; i < LayerB.Num(); i++) PosInLayer.Add(LayerB[i], i);

		for (int32 AIdx : LayerA)
		{
			const FLayoutNode& AN = Nodes[AIdx];
			for (int32 EI : AN.OutEdges)
			{
				const FLayoutEdge& E = Edges[EI];
				int32 TargetIdx = E.bReversed ? E.FromNode : E.ToNode;
				if (PosInLayer.Contains(TargetIdx))
				{
					// Check target is in LayerB
					bool bInLayerB = false;
					for (int32 BI : LayerB)
					{
						if (BI == TargetIdx) { bInLayerB = true; break; }
					}
					if (bInLayerB)
					{
						EdgePositions.Add(TPair<int32, int32>(PosInLayer[AIdx], PosInLayer[TargetIdx]));
					}
				}
			}
		}

		// Count inversions (crossings)
		int32 Crossings = 0;
		for (int32 i = 0; i < EdgePositions.Num(); i++)
		{
			for (int32 j = i + 1; j < EdgePositions.Num(); j++)
			{
				if ((EdgePositions[i].Key < EdgePositions[j].Key && EdgePositions[i].Value > EdgePositions[j].Value) ||
					(EdgePositions[i].Key > EdgePositions[j].Key && EdgePositions[i].Value < EdgePositions[j].Value))
				{
					Crossings++;
				}
			}
		}
		return Crossings;
	}

} // anonymous namespace

// ============================================================================
//  Main auto_layout handler
// ============================================================================

FMonolithActionResult FMonolithBlueprintLayoutActions::HandleAutoLayout(const TSharedPtr<FJsonObject>& Params)
{
	// --- Parse params ---
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString GraphName = Params->GetStringField(TEXT("graph_name"));
	if (GraphName.IsEmpty()) GraphName = TEXT("EventGraph");

	int32 HSpacing = Params->HasField(TEXT("horizontal_spacing"))
		? static_cast<int32>(Params->GetNumberField(TEXT("horizontal_spacing"))) : 350;
	int32 VSpacing = Params->HasField(TEXT("vertical_spacing"))
		? static_cast<int32>(Params->GetNumberField(TEXT("vertical_spacing"))) : 80;

	FString LayoutMode = Params->GetStringField(TEXT("layout_mode"));
	if (LayoutMode.IsEmpty()) LayoutMode = TEXT("all");

	FString Formatter = Params->GetStringField(TEXT("formatter"));
	if (Formatter.IsEmpty()) Formatter = TEXT("auto");

	// Validate formatter
	if (Formatter != TEXT("auto") && Formatter != TEXT("blueprint_assist") && Formatter != TEXT("monolith"))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown formatter '%s'. Valid: 'auto', 'blueprint_assist', 'monolith'"), *Formatter));
	}

	// Validate layout_mode
	if (LayoutMode != TEXT("all") && LayoutMode != TEXT("new_only") && LayoutMode != TEXT("selected"))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid layout_mode '%s'. Must be 'all', 'new_only', or 'selected'."), *LayoutMode));
	}

	// Parse node_ids for "selected" mode
	TSet<FString> SelectedNodeIds;
	if (LayoutMode == TEXT("selected"))
	{
		const TArray<TSharedPtr<FJsonValue>>* NodeIdsArr = nullptr;
		if (Params->TryGetArrayField(TEXT("node_ids"), NodeIdsArr) && NodeIdsArr)
		{
			for (const auto& V : *NodeIdsArr)
			{
				FString Id = V->AsString();
				if (!Id.IsEmpty()) SelectedNodeIds.Add(Id);
			}
		}
		if (SelectedNodeIds.Num() == 0)
		{
			return FMonolithActionResult::Error(TEXT("layout_mode='selected' requires a non-empty 'node_ids' array."));
		}
	}

	// --- Find the graph ---
	UEdGraph* Graph = MonolithBlueprintInternal::FindGraphByName(BP, GraphName);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Graph '%s' not found in Blueprint '%s'."), *GraphName, *AssetPath));
	}

	// --- Formatter dispatch ---
	if (Formatter == TEXT("auto") || Formatter == TEXT("blueprint_assist"))
	{
		bool bExplicitBA = (Formatter == TEXT("blueprint_assist"));
		bool bBAAvailable = IMonolithGraphFormatter::IsAvailable()
			&& IMonolithGraphFormatter::Get().SupportsGraph(Graph);

		if (bBAAvailable)
		{
			int32 NodesFormatted = 0;
			FString ErrorMessage;
			if (IMonolithGraphFormatter::Get().FormatGraph(Graph, NodesFormatted, ErrorMessage))
			{
				auto Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("formatter_used"), TEXT("blueprint_assist"));
				Result->SetNumberField(TEXT("nodes_formatted"), NodesFormatted);

				FMonolithFormatterInfo Info = IMonolithGraphFormatter::Get().GetFormatterInfo(Graph);
				Result->SetStringField(TEXT("formatter_type"), Info.FormatterType);
				Result->SetStringField(TEXT("graph_class"), Info.GraphClassName);

				// Warn if BA-incompatible params were set
				if (LayoutMode != TEXT("all") || SelectedNodeIds.Num() > 0)
				{
					Result->SetStringField(TEXT("warning"),
						TEXT("layout_mode and node_ids are ignored by Blueprint Assist formatter"));
				}

				return FMonolithActionResult::Success(Result);
			}

			if (bExplicitBA)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Blueprint Assist formatter failed: %s"), *ErrorMessage));
			}

			// "auto" mode: BA failed, fall through to built-in with warning
			UE_LOG(LogMonolith, Warning,
				TEXT("BA formatter failed (%s), falling back to built-in"), *ErrorMessage);
		}
		else if (bExplicitBA)
		{
			return FMonolithActionResult::Error(
				TEXT("Blueprint Assist formatter is not available. "
					"Install Blueprint Assist (paid marketplace plugin) and restart the editor."));
		}
		// else: "auto" mode, BA not available -- fall through silently to built-in
	}
	// --- Built-in Sugiyama formatter continues below ---

	// ========================================================================
	//  PHASE 0: PREPROCESSING — Build node list, classify, build adjacency
	// ========================================================================

	TArray<FLayoutNode> LayoutNodes;
	TArray<FLayoutEdge> LayoutEdges;
	TMap<UEdGraphNode*, int32> NodeToIndex;
	TArray<UEdGraphNode_Comment*> CommentNodes;

	// Build node array (skip comments and reroutes from the layout graph)
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;

		ELayoutNodeKind Kind = ClassifyNode(Node);

		if (Kind == ELayoutNodeKind::Comment)
		{
			CommentNodes.Add(Cast<UEdGraphNode_Comment>(Node));
			continue;
		}

		if (Kind == ELayoutNodeKind::Reroute)
		{
			continue;
		}

		int32 Idx = LayoutNodes.Num();
		FLayoutNode LN;
		LN.GraphNode = Node;
		LN.Index = Idx;
		LN.OrigX = Node->NodePosX;
		LN.OrigY = Node->NodePosY;
		LN.Kind = Kind;
		EstimateNodeSize(LN);

		// Determine pinned status based on layout mode.
		// "new_only" needs the whole node set before it can decide (see the
		// overlap pass below), so it only seeds the at-origin half here.
		FString NodeName = Node->GetName();
		if (LayoutMode == TEXT("new_only"))
		{
			bool bAtOrigin = (Node->NodePosX == 0 && Node->NodePosY == 0);
			LN.bPinned = !bAtOrigin;
		}
		else if (LayoutMode == TEXT("selected"))
		{
			LN.bPinned = !SelectedNodeIds.Contains(NodeName);
		}
		// "all" mode: nothing is pinned

		NodeToIndex.Add(Node, Idx);
		LayoutNodes.Add(MoveTemp(LN));
	}

	if (LayoutNodes.Num() == 0)
	{
		auto R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("asset_path"), AssetPath);
		R->SetStringField(TEXT("graph_name"), GraphName);
		R->SetNumberField(TEXT("nodes_repositioned"), 0);
		R->SetStringField(TEXT("note"), TEXT("Graph has no layoutable nodes."));
		R->SetStringField(TEXT("formatter_used"), TEXT("monolith"));
		return FMonolithActionResult::Success(R);
	}

	// ------------------------------------------------------------------------
	//  new_only: unpin the piles as well as the origin (issue #132)
	//
	//  "At (0,0)" alone means "unpinned only while nobody has ever given this
	//  node a position", which made new_only depend on add_node defaulting every
	//  unplaced node onto the origin. That default is what produced the
	//  unreadable piles in the first place, so the two had to move together: once
	//  add_node cascades, a purely at-origin test pins every new node and
	//  new_only reports success while moving nothing.
	//
	//  A node is treated as new if it is at the origin OR piled on another node.
	//  A pile is what needs relaying wherever it sits, and a node the user placed
	//  by hand in clear space is left exactly where they put it. Both members of
	//  an overlapping pair are freed -- there is no way to tell which of the two
	//  is the newcomer, and moving only one of them can leave the other stranded.
	// ------------------------------------------------------------------------
	if (LayoutMode == TEXT("new_only"))
	{
		TArray<MonolithBlueprintNodeGeometry::FNodeRect> Rects;
		Rects.Reserve(LayoutNodes.Num());
		for (const FLayoutNode& LN : LayoutNodes)
		{
			Rects.Add(OriginalRect(LN));
		}

		for (int32 A = 0; A < LayoutNodes.Num(); ++A)
		{
			for (int32 B = A + 1; B < LayoutNodes.Num(); ++B)
			{
				// Nothing left to learn about a pair that is already free.
				if (!LayoutNodes[A].bPinned && !LayoutNodes[B].bPinned) continue;

				if (MonolithBlueprintNodeGeometry::RectsFormPile(Rects[A], Rects[B]))
				{
					LayoutNodes[A].bPinned = false;
					LayoutNodes[B].bPinned = false;
				}
			}
		}
	}

	// Build edges from pin connections
	for (int32 NI = 0; NI < LayoutNodes.Num(); NI++)
	{
		UEdGraphNode* Node = LayoutNodes[NI].GraphNode;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->bHidden || Pin->Direction != EGPD_Output) continue;

			bool bExec = IsExecPin(Pin);
			int32 Weight = bExec ? 5 : 1;

			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin) continue;
				UEdGraphNode* TargetNode = LinkedPin->GetOwningNode();
				if (!TargetNode) continue;

				int32* TargetIdx = NodeToIndex.Find(TargetNode);
				if (!TargetIdx) continue; // Target is a comment or reroute

				// Avoid duplicate edges
				bool bDuplicate = false;
				for (int32 EI : LayoutNodes[NI].OutEdges)
				{
					if (LayoutEdges[EI].ToNode == *TargetIdx)
					{
						// Upgrade weight if this is an exec edge on an existing data edge
						if (Weight > LayoutEdges[EI].Weight) LayoutEdges[EI].Weight = Weight;
						bDuplicate = true;
						break;
					}
				}
				if (bDuplicate) continue;

				int32 EdgeIdx = LayoutEdges.Num();
				FLayoutEdge E;
				E.FromNode = NI;
				E.ToNode = *TargetIdx;
				E.Weight = Weight;
				E.bReversed = false;

				LayoutNodes[NI].OutEdges.Add(EdgeIdx);
				LayoutNodes[*TargetIdx].InEdges.Add(EdgeIdx);
				LayoutEdges.Add(E);
			}
		}
	}

	// ========================================================================
	//  PHASE 1: CYCLE BREAKING — DFS, mark back-edges
	// ========================================================================

	{
		enum class EDFSState : uint8 { White, Gray, Black };
		TArray<EDFSState> State;
		State.SetNumZeroed(LayoutNodes.Num());

		// DFS function (iterative to avoid stack overflow on huge graphs)
		TArray<TPair<int32, int32>> Stack; // (nodeIndex, outEdgeIterator)

		auto DFSFrom = [&](int32 StartIdx)
		{
			Stack.Reset();
			Stack.Push(TPair<int32, int32>(StartIdx, 0));
			State[StartIdx] = EDFSState::Gray;

			while (Stack.Num() > 0)
			{
				auto& Top = Stack.Last();
				int32 CurNode = Top.Key;
				int32& EdgeIter = Top.Value;

				if (EdgeIter >= LayoutNodes[CurNode].OutEdges.Num())
				{
					State[CurNode] = EDFSState::Black;
					Stack.Pop(EAllowShrinking::No);
					continue;
				}

				int32 EdgeIdx = LayoutNodes[CurNode].OutEdges[EdgeIter];
				EdgeIter++;

				FLayoutEdge& Edge = LayoutEdges[EdgeIdx];
				int32 Neighbor = Edge.ToNode;

				if (State[Neighbor] == EDFSState::Gray)
				{
					// Back-edge — reverse it logically
					Edge.bReversed = true;
					// Swap adjacency: move from OutEdges of From / InEdges of To
					// to reverse direction conceptually (but keep in same arrays, flag handles it)
				}
				else if (State[Neighbor] == EDFSState::White)
				{
					State[Neighbor] = EDFSState::Gray;
					Stack.Push(TPair<int32, int32>(Neighbor, 0));
				}
			}
		};

		// Start from all entry nodes first
		for (int32 i = 0; i < LayoutNodes.Num(); i++)
		{
			if (LayoutNodes[i].Kind == ELayoutNodeKind::Entry && State[i] == EDFSState::White)
			{
				DFSFrom(i);
			}
		}
		// Then catch any remaining unvisited nodes
		for (int32 i = 0; i < LayoutNodes.Num(); i++)
		{
			if (State[i] == EDFSState::White)
			{
				DFSFrom(i);
			}
		}
	}

	// ========================================================================
	//  PHASE 2: LAYER ASSIGNMENT (Longest-Path from entries)
	// ========================================================================

	{
		// Initialize all layers to -1
		for (FLayoutNode& LN : LayoutNodes)
		{
			LN.Layer = -1;
		}

		// Assign entry nodes to layer 0
		TArray<int32> Queue;
		for (int32 i = 0; i < LayoutNodes.Num(); i++)
		{
			if (LayoutNodes[i].Kind == ELayoutNodeKind::Entry)
			{
				LayoutNodes[i].Layer = 0;
				Queue.Add(i);
			}
		}

		// BFS along outgoing edges (respecting reversal)
		int32 Head = 0;
		while (Head < Queue.Num())
		{
			int32 CurIdx = Queue[Head++];
			int32 CurLayer = LayoutNodes[CurIdx].Layer;

			for (int32 EI : LayoutNodes[CurIdx].OutEdges)
			{
				const FLayoutEdge& Edge = LayoutEdges[EI];
				if (Edge.bReversed) continue; // Skip back-edges in forward pass

				int32 TargetIdx = Edge.ToNode;
				int32 NewLayer = CurLayer + 1;
				if (NewLayer > LayoutNodes[TargetIdx].Layer)
				{
					LayoutNodes[TargetIdx].Layer = NewLayer;
					Queue.Add(TargetIdx); // May revisit — longest path needs this
				}
			}

			// Also follow "reversed" in-edges as forward edges
			for (int32 EI : LayoutNodes[CurIdx].InEdges)
			{
				const FLayoutEdge& Edge = LayoutEdges[EI];
				if (!Edge.bReversed) continue;

				int32 TargetIdx = Edge.FromNode; // reversed: logical target is FromNode
				int32 NewLayer = CurLayer + 1;
				if (NewLayer > LayoutNodes[TargetIdx].Layer)
				{
					LayoutNodes[TargetIdx].Layer = NewLayer;
					Queue.Add(TargetIdx);
				}
			}
		}

		// Data-only nodes: assign layer = min(consumer_layer) - 1
		// First pass: find nodes still at layer -1 that have connections to layered nodes
		bool bChanged = true;
		int32 MaxIter = LayoutNodes.Num() + 1;
		while (bChanged && MaxIter-- > 0)
		{
			bChanged = false;
			for (int32 i = 0; i < LayoutNodes.Num(); i++)
			{
				FLayoutNode& LN = LayoutNodes[i];
				if (LN.Layer >= 0) continue;

				// Find the minimum layer of any consumer (node we output to)
				int32 MinConsumerLayer = INT32_MAX;
				for (int32 EI : LN.OutEdges)
				{
					const FLayoutEdge& Edge = LayoutEdges[EI];
					int32 TargetIdx = Edge.bReversed ? Edge.FromNode : Edge.ToNode;
					if (LayoutNodes[TargetIdx].Layer >= 0)
					{
						MinConsumerLayer = FMath::Min(MinConsumerLayer, LayoutNodes[TargetIdx].Layer);
					}
				}

				if (MinConsumerLayer != INT32_MAX)
				{
					LN.Layer = FMath::Max(0, MinConsumerLayer - 1);
					bChanged = true;
				}
			}
		}

		// Any still-unassigned nodes (fully disconnected) go to layer 0
		for (FLayoutNode& LN : LayoutNodes)
		{
			if (LN.Layer < 0) LN.Layer = 0;
		}
	}

	// ========================================================================
	//  Build layer arrays
	// ========================================================================

	int32 MaxLayer = 0;
	for (const FLayoutNode& LN : LayoutNodes)
	{
		MaxLayer = FMath::Max(MaxLayer, LN.Layer);
	}

	TArray<TArray<int32>> Layers;
	Layers.SetNum(MaxLayer + 1);
	for (int32 i = 0; i < LayoutNodes.Num(); i++)
	{
		Layers[LayoutNodes[i].Layer].Add(i);
		LayoutNodes[i].OrderInLayer = Layers[LayoutNodes[i].Layer].Num() - 1;
	}

	// ========================================================================
	//  PHASE 3: CROSSING MINIMIZATION (Barycenter + Greedy Switch)
	// ========================================================================

	auto ComputeBarycenter = [&](int32 NodeIdx, int32 AdjacentLayerIdx, bool bLookLeft)
	{
		const FLayoutNode& LN = LayoutNodes[NodeIdx];
		float WeightedSum = 0.f;
		float TotalWeight = 0.f;

		auto ProcessEdge = [&](int32 NeighborIdx, int32 Weight)
		{
			if (LayoutNodes[NeighborIdx].Layer == AdjacentLayerIdx)
			{
				WeightedSum += LayoutNodes[NeighborIdx].OrderInLayer * Weight;
				TotalWeight += Weight;
			}
		};

		if (bLookLeft)
		{
			// Look at predecessors (in-edges)
			for (int32 EI : LN.InEdges)
			{
				const FLayoutEdge& E = LayoutEdges[EI];
				int32 NeighborIdx = E.bReversed ? E.ToNode : E.FromNode;
				ProcessEdge(NeighborIdx, E.Weight);
			}
			// Reversed out-edges also point "left"
			for (int32 EI : LN.OutEdges)
			{
				const FLayoutEdge& E = LayoutEdges[EI];
				if (E.bReversed)
				{
					ProcessEdge(E.ToNode, E.Weight);
				}
			}
		}
		else
		{
			// Look at successors (out-edges)
			for (int32 EI : LN.OutEdges)
			{
				const FLayoutEdge& E = LayoutEdges[EI];
				if (E.bReversed) continue;
				ProcessEdge(E.ToNode, E.Weight);
			}
			// Reversed in-edges also point "right"
			for (int32 EI : LN.InEdges)
			{
				const FLayoutEdge& E = LayoutEdges[EI];
				if (E.bReversed)
				{
					ProcessEdge(E.FromNode, E.Weight);
				}
			}
		}

		return (TotalWeight > 0.f) ? (WeightedSum / TotalWeight) : (float)LN.OrderInLayer;
	};

	// 24 iterations alternating up/down sweeps
	for (int32 Iter = 0; Iter < 24; Iter++)
	{
		bool bDownSweep = (Iter % 2 == 0);

		if (bDownSweep)
		{
			// Sweep layers left to right (0 -> MaxLayer)
			for (int32 LayerIdx = 1; LayerIdx <= MaxLayer; LayerIdx++)
			{
				TArray<int32>& Layer = Layers[LayerIdx];

				// Compute barycenters relative to layer to the left
				for (int32 NI : Layer)
				{
					LayoutNodes[NI].Barycenter = ComputeBarycenter(NI, LayerIdx - 1, /*bLookLeft=*/ true);
				}

				// Sort by barycenter
				Layer.Sort([&](int32 A, int32 B)
				{
					return LayoutNodes[A].Barycenter < LayoutNodes[B].Barycenter;
				});

				// Update OrderInLayer
				for (int32 i = 0; i < Layer.Num(); i++)
				{
					LayoutNodes[Layer[i]].OrderInLayer = i;
				}
			}
		}
		else
		{
			// Sweep layers right to left (MaxLayer -> 0)
			for (int32 LayerIdx = MaxLayer - 1; LayerIdx >= 0; LayerIdx--)
			{
				TArray<int32>& Layer = Layers[LayerIdx];

				// Compute barycenters relative to layer to the right
				for (int32 NI : Layer)
				{
					LayoutNodes[NI].Barycenter = ComputeBarycenter(NI, LayerIdx + 1, /*bLookLeft=*/ false);
				}

				Layer.Sort([&](int32 A, int32 B)
				{
					return LayoutNodes[A].Barycenter < LayoutNodes[B].Barycenter;
				});

				for (int32 i = 0; i < Layer.Num(); i++)
				{
					LayoutNodes[Layer[i]].OrderInLayer = i;
				}
			}
		}

		// Greedy adjacent swap pass on each layer
		for (int32 LayerIdx = 0; LayerIdx <= MaxLayer; LayerIdx++)
		{
			TArray<int32>& Layer = Layers[LayerIdx];
			if (Layer.Num() < 2) continue;

			// Determine which adjacent layer to count crossings against
			int32 AdjLayer = (LayerIdx < MaxLayer) ? LayerIdx + 1 : LayerIdx - 1;
			if (AdjLayer < 0 || AdjLayer > MaxLayer) continue;

			for (int32 i = 0; i < Layer.Num() - 1; i++)
			{
				int32 CrossBefore = CountCrossings(Layer, Layers[AdjLayer], LayoutNodes, LayoutEdges);

				// Try swap
				Swap(Layer[i], Layer[i + 1]);
				LayoutNodes[Layer[i]].OrderInLayer = i;
				LayoutNodes[Layer[i + 1]].OrderInLayer = i + 1;

				int32 CrossAfter = CountCrossings(Layer, Layers[AdjLayer], LayoutNodes, LayoutEdges);

				if (CrossAfter >= CrossBefore)
				{
					// Revert swap
					Swap(Layer[i], Layer[i + 1]);
					LayoutNodes[Layer[i]].OrderInLayer = i;
					LayoutNodes[Layer[i + 1]].OrderInLayer = i + 1;
				}
			}
		}
	}

	// ========================================================================
	//  PHASE 4: COORDINATE ASSIGNMENT
	// ========================================================================

	{
		for (int32 LayerIdx = 0; LayerIdx <= MaxLayer; LayerIdx++)
		{
			const TArray<int32>& Layer = Layers[LayerIdx];
			int32 X = LayerIdx * HSpacing;
			float CurY = 0.f;

			for (int32 i = 0; i < Layer.Num(); i++)
			{
				FLayoutNode& LN = LayoutNodes[Layer[i]];
				LN.FinalX = X;
				LN.FinalY = (int32)CurY;
				CurY += LN.EstHeight + VSpacing;
			}
		}

		// Vertical alignment pass: for each node, try to center it on the average Y
		// of its connected predecessors (if that doesn't cause overlap)
		for (int32 LayerIdx = 1; LayerIdx <= MaxLayer; LayerIdx++)
		{
			const TArray<int32>& Layer = Layers[LayerIdx];
			for (int32 i = 0; i < Layer.Num(); i++)
			{
				FLayoutNode& LN = LayoutNodes[Layer[i]];

				// Compute average Y of predecessors
				float SumY = 0.f;
				int32 PredCount = 0;
				for (int32 EI : LN.InEdges)
				{
					const FLayoutEdge& E = LayoutEdges[EI];
					int32 PredIdx = E.bReversed ? E.ToNode : E.FromNode;
					SumY += LayoutNodes[PredIdx].FinalY;
					PredCount++;
				}

				if (PredCount == 0) continue;

				int32 DesiredY = (int32)(SumY / PredCount);

				// Clamp: don't go above the bottom of the node above us
				if (i > 0)
				{
					FLayoutNode& Above = LayoutNodes[Layer[i - 1]];
					int32 MinY = Above.FinalY + (int32)Above.EstHeight + VSpacing;
					DesiredY = FMath::Max(DesiredY, MinY);
				}

				LN.FinalY = DesiredY;
			}
		}

		// Translate so min position = (50, 50)
		int32 MinX = INT32_MAX, MinY = INT32_MAX;
		for (const FLayoutNode& LN : LayoutNodes)
		{
			MinX = FMath::Min(MinX, LN.FinalX);
			MinY = FMath::Min(MinY, LN.FinalY);
		}

		int32 OffsetX = 50 - MinX;
		int32 OffsetY = 50 - MinY;
		for (FLayoutNode& LN : LayoutNodes)
		{
			LN.FinalX += OffsetX;
			LN.FinalY += OffsetY;
		}
	}

	// ========================================================================
	//  PHASE 5: POST-PROCESSING — Apply positions, resize comments
	// ========================================================================

	int32 NodesRepositioned = 0;
	int32 NodesPinned = 0;

	for (FLayoutNode& LN : LayoutNodes)
	{
		if (LN.bPinned)
		{
			NodesPinned++;
			continue;
		}

		LN.GraphNode->NodePosX = LN.FinalX;
		LN.GraphNode->NodePosY = LN.FinalY;
		NodesRepositioned++;
	}

	// Resize comment nodes to encompass their contained nodes
	// We use original (pre-layout) positions to determine which nodes were inside each comment,
	// then resize the comment to encompass those nodes at their new positions.
	for (UEdGraphNode_Comment* Comment : CommentNodes)
	{
		if (!Comment) continue;

		int32 CX = Comment->NodePosX;
		int32 CY = Comment->NodePosY;
		int32 CW = Comment->NodeWidth;
		int32 CH = Comment->NodeHeight;

		int32 MinX = INT32_MAX, MinY = INT32_MAX;
		int32 MaxX = INT32_MIN, MaxY = INT32_MIN;
		int32 ContainedCount = 0;

		for (const FLayoutNode& LN : LayoutNodes)
		{
			// Was this node originally inside the comment bounds?
			if (LN.OrigX >= CX && LN.OrigX <= CX + CW &&
				LN.OrigY >= CY && LN.OrigY <= CY + CH)
			{
				// Use the node's final position (after layout)
				int32 NX = LN.bPinned ? LN.GraphNode->NodePosX : LN.FinalX;
				int32 NY = LN.bPinned ? LN.GraphNode->NodePosY : LN.FinalY;

				MinX = FMath::Min(MinX, NX);
				MinY = FMath::Min(MinY, NY);
				MaxX = FMath::Max(MaxX, NX + (int32)LN.EstWidth);
				MaxY = FMath::Max(MaxY, NY + (int32)LN.EstHeight);
				ContainedCount++;
			}
		}

		if (ContainedCount > 0)
		{
			const int32 Padding = 40;
			Comment->NodePosX = MinX - Padding;
			Comment->NodePosY = MinY - Padding - 30; // Extra for comment title bar
			Comment->NodeWidth = (MaxX - MinX) + Padding * 2;
			Comment->NodeHeight = (MaxY - MinY) + Padding * 2 + 30;
		}
	}

	// Mark blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	// --- Build result ---
	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("graph_name"), GraphName);
	ResultJson->SetNumberField(TEXT("nodes_repositioned"), NodesRepositioned);
	// Explicit, because "0 repositioned" and "everything was already where the
	// caller put it" read identically otherwise -- the failure new_only used to
	// hide behind.
	ResultJson->SetNumberField(TEXT("nodes_pinned"), NodesPinned);
	ResultJson->SetNumberField(TEXT("total_nodes"), LayoutNodes.Num());
	ResultJson->SetNumberField(TEXT("total_edges"), LayoutEdges.Num());
	ResultJson->SetNumberField(TEXT("layers"), MaxLayer + 1);
	ResultJson->SetStringField(TEXT("layout_mode"), LayoutMode);
	ResultJson->SetStringField(TEXT("formatter_used"), TEXT("monolith"));
	ResultJson->SetNumberField(TEXT("horizontal_spacing"), HSpacing);
	ResultJson->SetNumberField(TEXT("vertical_spacing"), VSpacing);

	// Include per-layer node counts
	TArray<TSharedPtr<FJsonValue>> LayerInfo;
	for (int32 i = 0; i <= MaxLayer; i++)
	{
		auto LObj = MakeShared<FJsonObject>();
		LObj->SetNumberField(TEXT("layer"), i);
		LObj->SetNumberField(TEXT("node_count"), Layers[i].Num());
		LayerInfo.Add(MakeShared<FJsonValueObject>(LObj));
	}
	ResultJson->SetArrayField(TEXT("layer_info"), LayerInfo);

	// Count back-edges
	int32 BackEdgeCount = 0;
	for (const FLayoutEdge& E : LayoutEdges)
	{
		if (E.bReversed) BackEdgeCount++;
	}
	ResultJson->SetNumberField(TEXT("cycles_broken"), BackEdgeCount);

	if (CommentNodes.Num() > 0)
	{
		ResultJson->SetNumberField(TEXT("comment_nodes"), CommentNodes.Num());
	}

	return FMonolithActionResult::Success(ResultJson);
}
