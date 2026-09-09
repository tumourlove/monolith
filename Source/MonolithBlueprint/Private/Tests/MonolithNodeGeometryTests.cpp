// Copyright tumourlove. All Rights Reserved.

// =============================================================================
// MonolithNodeGeometryTests.cpp
//
// Tests for the graph-space geometry behind issue #132 -- add_node's default
// placement and auto_layout(new_only)'s pinning rule.
//
// Both halves had to ship together, and both read the same rules out of
// MonolithBlueprintNodeGeometry.h, so the rules are what is worth locking:
//
//   1. A PILE is measured by area against the smaller node, not by "do these
//      rectangles touch". new_only unpins piles; if edge contact counted as a
//      pile, a hand-laid graph whose estimated node widths overshoot slightly
//      would be torn up by a mode whose whole promise is that it leaves placed
//      work alone.
//   2. Default placement never lands a node on top of another. That is the
//      property the issue is about: without it every unplaced node stacked on
//      the origin. It is asserted as a property (no overlaps, repeatable)
//      rather than against fixed coordinates, so the spacing constants stay
//      free to change.
// =============================================================================

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "MonolithBlueprintNodeGeometry.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "UObject/Package.h"

namespace MonolithNodeGeometryTests
{
	using MonolithBlueprintNodeGeometry::FNodeRect;

	static FNodeRect MakeRect(float X, float Y, float W, float H)
	{
		FNodeRect R;
		R.X = X; R.Y = Y; R.Width = W; R.Height = H;
		return R;
	}

	/**
	 * Comment nodes carry an explicit size, so a fixture built from them has an
	 * exactly known footprint instead of one estimated from pin counts.
	 */
	static UEdGraphNode* AddOccupant(UEdGraph* Graph, int32 X, int32 Y, int32 W = 300, int32 H = 100)
	{
		UEdGraphNode_Comment* Node = NewObject<UEdGraphNode_Comment>(Graph);
		Node->NodePosX = X;
		Node->NodePosY = Y;
		Node->NodeWidth = W;
		Node->NodeHeight = H;
		Graph->Nodes.Add(Node);
		return Node;
	}
}

// ---------------------------------------------------------------------------
// Test 1: what counts as a pile.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNodePileDetectionTest,
	"Monolith.NodeLayout.PileDetection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNodePileDetectionTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithNodeGeometryTests;
	using MonolithBlueprintNodeGeometry::RectsFormPile;
	using MonolithBlueprintNodeGeometry::OverlapArea;

	const FNodeRect Base = MakeRect(0.f, 0.f, 300.f, 100.f);

	// The case the fix exists for: everything stacked on the same point.
	TestTrue(TEXT("two nodes at the same position are a pile"),
		RectsFormPile(Base, MakeRect(0.f, 0.f, 300.f, 100.f)));

	// Far apart.
	TestFalse(TEXT("separated nodes are not a pile"),
		RectsFormPile(Base, MakeRect(600.f, 0.f, 300.f, 100.f)));
	TestEqual(TEXT("separated nodes have no overlap area"),
		OverlapArea(Base, MakeRect(600.f, 0.f, 300.f, 100.f)), 0.f);

	// Edges exactly touching is contact, not a pile -- "not merely touching
	// edges" is the whole point of measuring area.
	TestFalse(TEXT("nodes sharing an edge are not a pile"),
		RectsFormPile(Base, MakeRect(300.f, 0.f, 300.f, 100.f)));

	// A 30px slip on a 300px-wide node is 10% -- under the threshold, so a
	// hand-laid graph whose estimated widths overshoot is left alone.
	TestFalse(TEXT("a 10% sliver of overlap is not a pile"),
		RectsFormPile(Base, MakeRect(270.f, 0.f, 300.f, 100.f)));

	// Half-covered is unambiguously a pile.
	TestTrue(TEXT("a half-covered node is a pile"),
		RectsFormPile(Base, MakeRect(150.f, 0.f, 300.f, 100.f)));

	// A small node sitting entirely on a large one covers little OF the large
	// node but all of itself. Measuring against the smaller area is what catches
	// a getter dropped on top of a wide function call.
	TestTrue(TEXT("a small node fully inside a large one is a pile"),
		RectsFormPile(MakeRect(0.f, 0.f, 1200.f, 600.f), MakeRect(100.f, 100.f, 80.f, 40.f)));

	// Degenerate input must not divide by zero or claim a pile.
	TestFalse(TEXT("a zero-area node is never a pile"),
		RectsFormPile(Base, MakeRect(10.f, 10.f, 0.f, 0.f)));

	return true;
}

// ---------------------------------------------------------------------------
// Test 2: default placement puts nodes in clear space.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNodeDefaultPlacementTest,
	"Monolith.NodeLayout.DefaultPlacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNodeDefaultPlacementTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithNodeGeometryTests;
	using MonolithBlueprintNodeGeometry::GetNodeRect;
	using MonolithBlueprintNodeGeometry::OverlapArea;
	using MonolithBlueprintNodeGeometry::PlaceNodeInFreeSlot;

	UEdGraph* Graph = NewObject<UEdGraph>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!TestNotNull(TEXT("transient graph is created"), Graph))
	{
		return false;
	}

	// An empty graph still places at the origin, so a caller who adds exactly one
	// node sees no change from the old [0,0] default.
	{
		UEdGraphNode* First = AddOccupant(Graph, 0, 0);
		PlaceNodeInFreeSlot(Graph, First);
		TestEqual(TEXT("the first node in an empty graph goes to x=0"), First->NodePosX, 0);
		TestEqual(TEXT("the first node in an empty graph goes to y=0"), First->NodePosY, 0);
	}

	// Every subsequent node lands clear of everything already there. Each one is
	// added at the origin first, which is exactly what the old default did.
	const int32 NodesToPlace = 12;
	for (int32 i = 0; i < NodesToPlace; ++i)
	{
		UEdGraphNode* Node = AddOccupant(Graph, 0, 0);
		PlaceNodeInFreeSlot(Graph, Node);
	}

	int32 OverlappingPairs = 0;
	for (int32 A = 0; A < Graph->Nodes.Num(); ++A)
	{
		for (int32 B = A + 1; B < Graph->Nodes.Num(); ++B)
		{
			if (OverlapArea(GetNodeRect(Graph->Nodes[A]), GetNodeRect(Graph->Nodes[B])) > 0.f)
			{
				OverlappingPairs++;
			}
		}
	}
	TestEqual(TEXT("no two placed nodes overlap"), OverlappingPairs, 0);

	// The whole complaint in issue #132 was a pile on the origin.
	int32 AtOrigin = 0;
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodePosX == 0 && Node->NodePosY == 0) { AtOrigin++; }
	}
	TestEqual(TEXT("only the first node sits on the origin"), AtOrigin, 1);

	// Placement is a pure function of the graph's contents: asking again for a
	// node that is already in a free slot returns the same answer.
	{
		UEdGraphNode* Last = Graph->Nodes.Last();
		const int32 X = Last->NodePosX;
		const int32 Y = Last->NodePosY;
		PlaceNodeInFreeSlot(Graph, Last);
		TestEqual(TEXT("re-placing a node is stable in x"), Last->NodePosX, X);
		TestEqual(TEXT("re-placing a node is stable in y"), Last->NodePosY, Y);
	}

	// An occupant far from the origin moves the anchor with it -- placement is
	// relative to existing content, not to a fixed point.
	{
		UEdGraph* Offset = NewObject<UEdGraph>(GetTransientPackage(), NAME_None, RF_Transient);
		AddOccupant(Offset, 5000, 4000);
		UEdGraphNode* Node = AddOccupant(Offset, 0, 0);
		PlaceNodeInFreeSlot(Offset, Node);
		TestTrue(TEXT("the new node is placed near the existing content, not at the origin"),
			Node->NodePosX >= 4000 && Node->NodePosY >= 3000);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
