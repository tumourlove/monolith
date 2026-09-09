// Copyright tumourlove. All Rights Reserved.
// WidgetReuse.cpp

#include "Spec/Builders/WidgetReuse.h"

#include "Spec/UIBuildContext.h"
#include "Spec/UISpec.h"

#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"

UWidget* MonolithUI::WidgetReuse::TakeReusable(
    FUIBuildContext& Context,
    const FUISpecNode& Node,
    const UClass* WidgetClass)
{
    if (!Context.bPatchMode || Node.Id.IsNone() || !WidgetClass)
    {
        return nullptr;
    }

    const TWeakObjectPtr<UWidget>* Found = Context.ReusableWidgets.Find(Node.Id);
    if (!Found || !Found->IsValid())
    {
        return nullptr;
    }

    UWidget* Existing = Found->Get();
    if (Existing->GetClass() != WidgetClass)
    {
        // Type change: the spec asks for a different widget class under the
        // same id. Reusing would be wrong, so report the (unavoidable) loss and
        // let the caller construct fresh.
        FUISpecError W;
        W.Severity = EUISpecErrorSeverity::Warning;
        W.Category = TEXT("DataLoss.TypeChange");
        W.WidgetId = Node.Id;
        W.Message  = FString::Printf(
            TEXT("Patch: '%s' exists as %s but the spec declares %s. The widget is replaced, ")
            TEXT("so properties outside the spec schema are lost for this node."),
            *Node.Id.ToString(),
            *Existing->GetClass()->GetName(),
            *WidgetClass->GetName());
        W.SuggestedFix = TEXT("Keep the node's type stable, or rename the node so the replacement is explicit.");
        Context.Warnings.Add(MoveTemp(W));
        return nullptr;
    }

    Existing->Modify();
    Context.ReusedWidgetIds.Add(Node.Id);
    ++Context.NodesReused;
    return Existing;
}

bool MonolithUI::WidgetReuse::PlaceInPatchMode(
    FUIBuildContext& Context,
    const FUISpecNode& Node,
    UWidget* Widget,
    UPanelWidget* ParentPanel)
{
    if (!Widget)
    {
        return false;
    }
    if (!ParentPanel)
    {
        // Root node. The dispatcher assigns WidgetTree::RootWidget, but a
        // reused widget promoted from mid-tree would otherwise stay in its old
        // parent's Slots array as well -- root and child at once, which is a
        // cycle. Detach it here; the sweep counts the vacated slot.
        if (UPanelWidget* OldParent = Widget->GetParent())
        {
            OldParent->Modify();
            OldParent->RemoveChild(Widget);
        }
        return true;
    }

    ParentPanel->Modify();

    const TWeakObjectPtr<UPanelWidget> PanelKey(ParentPanel);
    int32& Cursor = Context.PatchChildCursor.FindOrAdd(PanelKey, 0);

    if (Widget->GetParent() == ParentPanel)
    {
        // Already in the right panel: reorder only. ShiftChild moves the entry
        // in the Slots array and leaves the UPanelSlot object alone, so every
        // slot property — modelled or not — is preserved verbatim.
        ParentPanel->ShiftChild(Cursor, Widget);
        ++Cursor;
        return true;
    }

    // Moving in from elsewhere, or brand new. AddChild always mints a fresh
    // slot, but it accepts a template: when the old slot is the same class the
    // new panel uses, hand it over so unmodelled slot state survives the move.
    UPanelSlot* OldSlot = Widget->Slot;
    UPanelSlot* SlotTemplate =
        (OldSlot && ParentPanel->GetSlotClass() == OldSlot->GetClass()) ? OldSlot : nullptr;

    if (ParentPanel->InsertChildAt(Cursor, Widget, SlotTemplate) != nullptr)
    {
        ++Cursor;
        return true;
    }

    FUISpecError E;
    E.Severity = EUISpecErrorSeverity::Error;
    E.Category = TEXT("Slot");
    E.WidgetId = Node.Id;
    if (!ParentPanel->CanHaveMultipleChildren() && ParentPanel->GetChildrenCount() > 0)
    {
        UWidget* Occupant = ParentPanel->GetChildAt(0);
        E.Message = FString::Printf(
            TEXT("Patch: InsertChildAt failed — parent '%s' is single-child (%s) and already holds '%s'."),
            *ParentPanel->GetName(),
            *ParentPanel->GetClass()->GetName(),
            Occupant ? *Occupant->GetName() : TEXT("?"));
        E.SuggestedFix = TEXT("Wrap the additional children in a VerticalBox/HorizontalBox.");
    }
    else
    {
        E.Message = TEXT("Patch: InsertChildAt returned null slot.");
    }
    Context.Errors.Add(MoveTemp(E));
    return false;
}
