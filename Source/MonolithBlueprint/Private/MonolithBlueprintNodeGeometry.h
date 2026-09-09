#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"

// ============================================================
//  MonolithBlueprintNodeGeometry  (issue #132)
//
//  Graph-space geometry shared by the two halves of the readable-output fix:
//  add_node's default placement (MonolithBlueprintNodeActions.cpp) and
//  auto_layout's new_only pinning rule (MonolithBlueprintLayoutActions.cpp).
//
//  They MUST agree on what a node occupies. add_node places a node so it does
//  not overlap anything; auto_layout(new_only) decides a node is "new" partly
//  by whether it overlaps something. Two different size estimates would let a
//  node be simultaneously well-placed and considered a pile.
//
//  Slate node sizes are only known after a widget is built, which is not
//  available from an MCP action, so both callers estimate. The estimate is the
//  one auto_layout has always used.
//
//  The namespace is NAMED, never anonymous -- an anonymous namespace in a
//  header collides as C2084/C2011 under the release's forced-full-unity pass.
// ============================================================

namespace MonolithBlueprintNodeGeometry
{
	/** An axis-aligned box in graph space. */
	struct FNodeRect
	{
		float X = 0.f;
		float Y = 0.f;
		float Width = 0.f;
		float Height = 0.f;

		float Right()  const { return X + Width; }
		float Bottom() const { return Y + Height; }
		float Area()   const { return Width * Height; }
	};

	/**
	 * Fraction of the SMALLER node's area that has to be covered before two nodes
	 * count as piled on each other.
	 *
	 * A quarter, so that nodes merely brushing edges (a hand-laid graph where a
	 * wide node's estimated width overshoots its real one) are left alone, while
	 * the case this exists for -- several nodes stacked on the same point, which
	 * is 100% coverage -- is caught with room to spare.
	 */
	constexpr float PileOverlapAreaFraction = 0.25f;

	/** Clearance a freshly-placed node keeps from everything already in the graph. */
	constexpr float PlacementMargin = 60.f;

	inline int32 CountVisiblePins(const UEdGraphNode* Node, EEdGraphPinDirection Dir)
	{
		int32 Count = 0;
		if (!Node) { return 0; }
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && !Pin->bHidden && Pin->Direction == Dir) { Count++; }
		}
		return Count;
	}

	/** Estimated on-screen footprint. Comments carry their real size. */
	inline void EstimateNodeSize(const UEdGraphNode* Node, float& OutWidth, float& OutHeight)
	{
		OutWidth = 300.f;
		OutHeight = 74.f;
		if (!Node) { return; }

		const int32 MaxPins = FMath::Max(
			CountVisiblePins(Node, EGPD_Input), CountVisiblePins(Node, EGPD_Output));
		OutHeight = 50.f + MaxPins * 24.f;
		OutWidth = (Node->NodeWidth > 0) ? (float)Node->NodeWidth : 300.f;

		if (const UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
		{
			OutWidth = (float)Comment->NodeWidth;
			OutHeight = (float)Comment->NodeHeight;
		}
	}

	/** The node's footprint at its CURRENT position. */
	inline FNodeRect GetNodeRect(const UEdGraphNode* Node)
	{
		FNodeRect Rect;
		if (!Node) { return Rect; }
		Rect.X = (float)Node->NodePosX;
		Rect.Y = (float)Node->NodePosY;
		EstimateNodeSize(Node, Rect.Width, Rect.Height);
		return Rect;
	}

	/** Area of the intersection, 0 when they do not meet. */
	inline float OverlapArea(const FNodeRect& A, const FNodeRect& B)
	{
		const float OverlapW = FMath::Min(A.Right(), B.Right()) - FMath::Max(A.X, B.X);
		const float OverlapH = FMath::Min(A.Bottom(), B.Bottom()) - FMath::Max(A.Y, B.Y);
		if (OverlapW <= 0.f || OverlapH <= 0.f)
		{
			return 0.f;
		}
		return OverlapW * OverlapH;
	}

	/**
	 * True when two nodes are piled on each other rather than merely touching.
	 *
	 * Measured against the SMALLER of the two areas: a small pure-getter sitting
	 * fully on top of a wide function call covers a trivial fraction of the big
	 * node, but all of itself, and that is a pile either way.
	 */
	inline bool RectsFormPile(const FNodeRect& A, const FNodeRect& B)
	{
		const float Smaller = FMath::Min(A.Area(), B.Area());
		if (Smaller <= 0.f)
		{
			return false;
		}
		return (OverlapArea(A, B) / Smaller) >= PileOverlapAreaFraction;
	}

	/**
	 * Default placement for a node whose caller supplied no position.
	 *
	 * add_node's position defaulted to [0,0], so every node a caller did not
	 * place landed on the same point -- the unreadable pile of issue #132. Nodes
	 * are instead dropped into the first free cell of a grid anchored on the
	 * graph's existing content, filling a column downwards before starting the
	 * next column to the right, and skipping any cell that would come within
	 * PlacementMargin of something already there.
	 *
	 * An empty graph places at the origin, so nothing changes for the first node
	 * or for a caller who passes an explicit position (this is not called then).
	 *
	 * NewNode must already be in the graph with its pins allocated: it is skipped
	 * when scanning, and its own pin count is what sizes the candidate cell.
	 */
	inline void PlaceNodeInFreeSlot(const UEdGraph* Graph, UEdGraphNode* NewNode)
	{
		if (!Graph || !NewNode) { return; }

		TArray<FNodeRect> Occupied;
		Occupied.Reserve(Graph->Nodes.Num());
		float MinX = 0.f, MinY = 0.f, MaxX = 0.f;
		bool bAny = false;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || Node == NewNode) { continue; }
			const FNodeRect Rect = GetNodeRect(Node);
			Occupied.Add(Rect);
			MinX = bAny ? FMath::Min(MinX, Rect.X) : Rect.X;
			MinY = bAny ? FMath::Min(MinY, Rect.Y) : Rect.Y;
			MaxX = bAny ? FMath::Max(MaxX, Rect.Right()) : Rect.Right();
			bAny = true;
		}

		if (!bAny)
		{
			// First node in the graph: the origin is as good a spot as any, and
			// keeps single-node output identical to the old behaviour.
			NewNode->NodePosX = 0;
			NewNode->NodePosY = 0;
			return;
		}

		FNodeRect Candidate;
		EstimateNodeSize(NewNode, Candidate.Width, Candidate.Height);

		// A column of 8 keeps a run of new nodes inside roughly one screen height
		// before it steps sideways.
		const float ColumnStep = FMath::Max(Candidate.Width, 300.f) + PlacementMargin * 2.f;
		const float RowStep    = FMath::Max(Candidate.Height, 120.f) + PlacementMargin;
		const int32 MaxRows = 8;
		const int32 MaxCols = 32;

		auto IsFree = [&Occupied, &Candidate]() -> bool
		{
			FNodeRect Padded = Candidate;
			Padded.X -= PlacementMargin;
			Padded.Y -= PlacementMargin;
			Padded.Width  += PlacementMargin * 2.f;
			Padded.Height += PlacementMargin * 2.f;
			for (const FNodeRect& Other : Occupied)
			{
				if (OverlapArea(Padded, Other) > 0.f)
				{
					return false;
				}
			}
			return true;
		};

		for (int32 Col = 0; Col < MaxCols; ++Col)
		{
			for (int32 Row = 0; Row < MaxRows; ++Row)
			{
				Candidate.X = MinX + Col * ColumnStep;
				Candidate.Y = MinY + Row * RowStep;
				if (IsFree())
				{
					NewNode->NodePosX = FMath::RoundToInt(Candidate.X);
					NewNode->NodePosY = FMath::RoundToInt(Candidate.Y);
					return;
				}
			}
		}

		// Nothing free inside the scanned area -- park it clear of everything.
		// Right of the widest extent is free by construction.
		NewNode->NodePosX = FMath::RoundToInt(MaxX + PlacementMargin * 2.f);
		NewNode->NodePosY = FMath::RoundToInt(MinY);
	}
}
