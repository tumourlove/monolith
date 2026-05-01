// Copyright tumourlove. All Rights Reserved.
// PanelBuilder.cpp
//
// Phase H — multi-child panel sub-builder. Constructs the panel widget,
// attaches it under its parent, and routes per-slot writes through the
// reflection helper using the new ApplyJsonPath translation overload.
//
// Slot configuration uses a *coarse* path: we hand the slot pointer to the
// reflection helper as the root object and let it walk against the slot's
// reflected layout struct (UCanvasPanelSlot, UVerticalBoxSlot, etc.). The
// slot-side allowlist gate is currently a no-op for slot writes — the
// allowlist is keyed by widget token, and slot classes are not in the
// registry at this time. The dispatcher passes raw_mode=true for slot writes
// (effectively the same behaviour the legacy `set_slot_property` action has).

#include "Spec/Builders/PanelBuilder.h"

#include "Spec/UIBuildContext.h"
#include "Spec/UISpec.h"
#include "MonolithUICommon.h"
#include "Registry/MonolithUIRegistrySubsystem.h"
#include "Registry/UIPropertyAllowlist.h"
#include "Registry/UIPropertyPathCache.h"
#include "Registry/UIReflectionHelper.h"

#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/GridSlot.h"
#include "Components/UniformGridSlot.h"
#include "Components/SizeBox.h"
#include "Components/SizeBoxSlot.h"
#include "Components/ScaleBoxSlot.h"
#include "Components/WrapBoxSlot.h"
#include "Components/WidgetSwitcherSlot.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/ProgressBar.h"
#include "Components/Widget.h"
#include "WidgetBlueprint.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace MonolithUI::PanelBuilderInternal
{
    static bool HasAnyPadding(const FMargin& Padding)
    {
        return Padding.GetTotalSpaceAlong<EOrientation::Orient_Horizontal>() != 0
            || Padding.GetTotalSpaceAlong<EOrientation::Orient_Vertical>() != 0;
    }

    static bool HasAuthoredColor(const FLinearColor& Color)
    {
        return !Color.Equals(FLinearColor::Transparent);
    }

    template <typename SlotType>
    static void ApplyAlignmentAndPadding(const FUISpecSlot& S, SlotType* Slot)
    {
        if (!Slot)
        {
            return;
        }
        if (!S.HAlign.IsNone())
        {
            Slot->SetHorizontalAlignment(MonolithUI::ParseHAlign(S.HAlign.ToString()));
        }
        if (!S.VAlign.IsNone())
        {
            Slot->SetVerticalAlignment(MonolithUI::ParseVAlign(S.VAlign.ToString()));
        }
        if (HasAnyPadding(S.Padding))
        {
            Slot->SetPadding(S.Padding);
        }
    }

    static void ApplyAlignmentOnly(const FUISpecSlot& S, UScaleBoxSlot* Slot)
    {
        if (!Slot)
        {
            return;
        }
        if (!S.HAlign.IsNone())
        {
            Slot->SetHorizontalAlignment(MonolithUI::ParseHAlign(S.HAlign.ToString()));
        }
        if (!S.VAlign.IsNone())
        {
            Slot->SetVerticalAlignment(MonolithUI::ParseVAlign(S.VAlign.ToString()));
        }
    }

    /**
     * Construct + attach. Returns the constructed widget on success.
     *
     * Returns nullptr on hard failure (ConstructWidget refused, AddChild
     * refused). The dispatcher never sees a half-constructed state — either
     * we attached cleanly or we returned a null + pushed an error.
     *
     * The auto-naming rule: when Node.Id is empty we let the engine pick
     * a name (NAME_None makes ConstructWidget generate one). When Id is
     * supplied we honour it verbatim — colliding-name handling is the
     * dispatcher's responsibility (the dry-walk's cycle detector catches
     * intra-spec duplicates; cross-spec collisions on an existing WBP are
     * only possible in the in-place edit path which we don't support yet
     * with id-stable merging).
     */
    static UWidget* ConstructAndAttach(
        FUIBuildContext& Context,
        const FUISpecNode& Node,
        UClass* WidgetClass,
        UPanelWidget* ParentPanel)
    {
        if (!Context.TargetWBP || !Context.TargetWBP->WidgetTree)
        {
            FUISpecError E;
            E.Severity = EUISpecErrorSeverity::Error;
            E.Category = TEXT("Internal");
            E.WidgetId = Node.Id;
            E.Message  = TEXT("PanelBuilder: target WBP / WidgetTree is null.");
            Context.Errors.Add(MoveTemp(E));
            return nullptr;
        }

        UWidget* Constructed = Context.TargetWBP->WidgetTree->ConstructWidget<UWidget>(
            WidgetClass, Node.Id);
        if (!Constructed)
        {
            FUISpecError E;
            E.Severity = EUISpecErrorSeverity::Error;
            E.Category = TEXT("Construct");
            E.WidgetId = Node.Id;
            E.Message  = FString::Printf(
                TEXT("ConstructWidget returned null for class '%s'."),
                *WidgetClass->GetName());
            Context.Errors.Add(MoveTemp(E));
            return nullptr;
        }

        // Attach to parent. nullptr ParentPanel means "this is the root" —
        // the dispatcher does the WidgetTree::RootWidget assignment when it
        // sees a null parent on the recursion entry.
        if (ParentPanel)
        {
            UPanelSlot* Slot = ParentPanel->AddChild(Constructed);
            if (!Slot)
            {
                FUISpecError E;
                E.Severity = EUISpecErrorSeverity::Error;
                E.Category = TEXT("Slot");
                E.WidgetId = Node.Id;
                if (!ParentPanel->CanHaveMultipleChildren() && ParentPanel->GetChildrenCount() > 0)
                {
                    UWidget* Existing = ParentPanel->GetChildAt(0);
                    E.Message = FString::Printf(
                        TEXT("AddChild failed: parent '%s' is single-child (%s) and already holds '%s'."),
                        *ParentPanel->GetName(),
                        *ParentPanel->GetClass()->GetName(),
                        Existing ? *Existing->GetName() : TEXT("?"));
                    E.SuggestedFix = TEXT("Wrap the additional children in a VerticalBox/HorizontalBox.");
                }
                else
                {
                    E.Message = TEXT("AddChild returned null slot.");
                }
                Context.Errors.Add(MoveTemp(E));
                return nullptr;
            }
        }

        MonolithUI::RegisterCreatedWidget(Context.TargetWBP, Constructed);
        return Constructed;
    }

    /**
     * Apply the FUISpecSlot fields to whatever slot type the parent gave us.
     * Slot writes are best-effort — we walk a coarse set of common fields
     * (padding / alignment / canvas position+size+anchor) directly on the
     * typed slot subclass. Reflection-driven slot writes are a Phase J
     * extension; v1 covers the common cases the existing add_widget action
     * already supports.
     */
    static void ApplySlotFields(
        FUIBuildContext& Context,
        const FUISpecNode& Node,
        UWidget* Widget)
    {
        if (!Widget || !Widget->Slot)
        {
            return;
        }
        UPanelSlot* Slot = Widget->Slot;
        const FUISpecSlot& S = Node.Slot;

        if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(Slot))
        {
            if (!S.AnchorPreset.IsNone())
            {
                CSlot->SetAnchors(MonolithUI::GetAnchorPreset(S.AnchorPreset.ToString()));
            }
            if (!S.Position.IsNearlyZero())
            {
                CSlot->SetPosition(S.Position);
            }
            if (!S.Size.IsNearlyZero())
            {
                CSlot->SetSize(S.Size);
            }
            if (!S.Alignment.IsNearlyZero())
            {
                CSlot->SetAlignment(S.Alignment);
            }
            if (S.bAutoSize)
            {
                CSlot->SetAutoSize(true);
            }
            if (S.ZOrder != 0)
            {
                CSlot->SetZOrder(S.ZOrder);
            }
        }
        else if (UVerticalBoxSlot* VSlot = Cast<UVerticalBoxSlot>(Slot))
        {
            ApplyAlignmentAndPadding(S, VSlot);
        }
        else if (UHorizontalBoxSlot* HSlot = Cast<UHorizontalBoxSlot>(Slot))
        {
            ApplyAlignmentAndPadding(S, HSlot);
        }
        else if (UOverlaySlot* OSlot = Cast<UOverlaySlot>(Slot))
        {
            ApplyAlignmentAndPadding(S, OSlot);
        }
        else if (UScrollBoxSlot* SSlot = Cast<UScrollBoxSlot>(Slot))
        {
            ApplyAlignmentAndPadding(S, SSlot);
        }
        else if (UGridSlot* GSlot = Cast<UGridSlot>(Slot))
        {
            ApplyAlignmentAndPadding(S, GSlot);
            if (!S.Position.IsNearlyZero())
            {
                GSlot->SetColumn(FMath::RoundToInt(S.Position.X));
                GSlot->SetRow(FMath::RoundToInt(S.Position.Y));
            }
            if (S.Size.X > 0.f)
            {
                GSlot->SetColumnSpan(FMath::RoundToInt(S.Size.X));
            }
            if (S.Size.Y > 0.f)
            {
                GSlot->SetRowSpan(FMath::RoundToInt(S.Size.Y));
            }
            if (S.ZOrder != 0)
            {
                GSlot->SetLayer(S.ZOrder);
            }
        }
        else if (UUniformGridSlot* UGSlot = Cast<UUniformGridSlot>(Slot))
        {
            if (!S.HAlign.IsNone())
            {
                UGSlot->SetHorizontalAlignment(MonolithUI::ParseHAlign(S.HAlign.ToString()));
            }
            if (!S.VAlign.IsNone())
            {
                UGSlot->SetVerticalAlignment(MonolithUI::ParseVAlign(S.VAlign.ToString()));
            }
            if (!S.Position.IsNearlyZero())
            {
                UGSlot->SetColumn(FMath::RoundToInt(S.Position.X));
                UGSlot->SetRow(FMath::RoundToInt(S.Position.Y));
            }
        }
        else if (USizeBoxSlot* SZSlot = Cast<USizeBoxSlot>(Slot))
        {
            ApplyAlignmentAndPadding(S, SZSlot);
        }
        else if (UScaleBoxSlot* SCSlot = Cast<UScaleBoxSlot>(Slot))
        {
            ApplyAlignmentOnly(S, SCSlot);
        }
        else if (UWrapBoxSlot* WSlot = Cast<UWrapBoxSlot>(Slot))
        {
            ApplyAlignmentAndPadding(S, WSlot);
        }
        else if (UWidgetSwitcherSlot* WSSlot = Cast<UWidgetSwitcherSlot>(Slot))
        {
            ApplyAlignmentAndPadding(S, WSSlot);
        }
        else if (UBorderSlot* BSlot = Cast<UBorderSlot>(Slot))
        {
            ApplyAlignmentAndPadding(S, BSlot);
        }
    }

    /**
     * Apply common style fields onto the widget itself (RenderOpacity,
     * Visibility). Style fields more specific to particular widget types
     * are handled by the type-specific builders (LeafBuilder for text /
     * image, EffectSurfaceBuilder for the rounded-rect cluster).
     */
    static void ApplyCommonStyle(
        FUIBuildContext& Context,
        const FUISpecNode& Node,
        UWidget* Widget)
    {
        if (!Widget) return;

        const FUISpecStyle& S = Node.Style;
        Widget->SetRenderOpacity(FMath::Clamp(S.Opacity, 0.f, 1.f));

        if (!S.Visibility.IsNone())
        {
            const FString V = S.Visibility.ToString();
            if      (V == TEXT("Visible"))                Widget->SetVisibility(ESlateVisibility::Visible);
            else if (V == TEXT("Hidden"))                 Widget->SetVisibility(ESlateVisibility::Hidden);
            else if (V == TEXT("Collapsed"))              Widget->SetVisibility(ESlateVisibility::Collapsed);
            else if (V == TEXT("HitTestInvisible"))       Widget->SetVisibility(ESlateVisibility::HitTestInvisible);
            else if (V == TEXT("SelfHitTestInvisible"))   Widget->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
        }

        if (USizeBox* SizeBox = Cast<USizeBox>(Widget))
        {
            if (S.Width > 0.f)
            {
                SizeBox->SetWidthOverride(S.Width);
            }
            if (S.Height > 0.f)
            {
                SizeBox->SetHeightOverride(S.Height);
            }
        }
        if (UBorder* Border = Cast<UBorder>(Widget))
        {
            if (HasAuthoredColor(S.Background))
            {
                Border->SetBrushColor(S.Background);
            }
            if (HasAnyPadding(S.Padding))
            {
                Border->SetPadding(S.Padding);
            }
        }
        if (UProgressBar* ProgressBar = Cast<UProgressBar>(Widget))
        {
            if (HasAuthoredColor(S.Background))
            {
                ProgressBar->SetFillColorAndOpacity(S.Background);
            }
        }
    }
} // namespace MonolithUI::PanelBuilderInternal


UWidget* MonolithUI::PanelBuilder::BuildPanel(
    FUIBuildContext& Context,
    const FUISpecNode& Node,
    UClass* WidgetClass,
    UPanelWidget* ParentPanel)
{
    using namespace MonolithUI::PanelBuilderInternal;

    UWidget* Constructed = ConstructAndAttach(Context, Node, WidgetClass, ParentPanel);
    if (!Constructed)
    {
        return nullptr;
    }

    ApplySlotFields(Context, Node, Constructed);
    ApplyCommonStyle(Context, Node, Constructed);

    return Constructed;
}
