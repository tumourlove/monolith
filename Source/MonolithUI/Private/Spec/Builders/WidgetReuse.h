// Copyright tumourlove. All Rights Reserved.
// WidgetReuse.h
//
// Patch-mode (issue #139) placement helpers shared by LeafBuilder and
// PanelBuilder. Both sub-builders have their own `ConstructAndAttach`; in
// patch mode both need the same two behaviours, so they live here rather than
// being duplicated:
//
//   1. Match a spec node to a pre-existing widget by id + class, so the widget
//      UObject (and everything on it the spec cannot model) survives.
//   2. Place that widget under its spec-declared parent WITHOUT destroying the
//      slot when it is already there — `ShiftChild` reorders the Slots array
//      in place and keeps the existing UPanelSlot; `InsertChildAt` with the old
//      slot as a template carries slot state across an actual reparent.
//
// Rebuild mode never calls into here — its ConstructWidget + AddChild path is
// untouched.
//
// Lives under Private/ — internal to the spec builder pipeline.

#pragma once

#include "CoreMinimal.h"

class UClass;
class UPanelWidget;
class UWidget;
struct FUIBuildContext;
struct FUISpecNode;

namespace MonolithUI::WidgetReuse
{
    /**
     * Patch mode: return the pre-existing widget registered under `Node.Id`
     * when its UClass matches `WidgetClass` exactly, else nullptr.
     *
     * The class match is exact (not IsA) on purpose: a spec that changes a
     * node's type genuinely needs a new widget, and silently keeping the old
     * one would be its own species of the bug this mode exists to fix. The
     * caller falls back to construction and the old widget is swept as an
     * orphan, with the class change reported.
     *
     * Marks the id as reused and bumps `Context.NodesReused`.
     */
    UWidget* TakeReusable(FUIBuildContext& Context, const FUISpecNode& Node, const UClass* WidgetClass);

    /**
     * Patch mode: place `Widget` under `ParentPanel` at that panel's next
     * spec-order index. A null `ParentPanel` means "this node is the root" and
     * is a no-op (the dispatcher assigns WidgetTree::RootWidget).
     *
     * Returns false and pushes an error into `Context` when the panel refuses
     * the child (single-child panel already occupied, etc.).
     */
    bool PlaceInPatchMode(
        FUIBuildContext& Context,
        const FUISpecNode& Node,
        UWidget* Widget,
        UPanelWidget* ParentPanel);
}
