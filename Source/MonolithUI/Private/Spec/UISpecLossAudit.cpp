// Copyright tumourlove. All Rights Reserved.
// UISpecLossAudit.cpp
//
// Implementation of the pre-teardown data-loss audit. See the header for the
// rationale; the short version is that a rebuild resets every property the
// spec cannot carry, and until this pass ran it did so with no diagnostic.
//
// Diff strategy: `FProperty::Identical_InContainer(Instance, CDO)` against the
// widget's own class default. That is the same comparison the details panel
// uses to decide whether to bold a property, so "differs from CDO" lines up
// with "a human authored this". We then descend FStructProperty leaves so the
// report can name `WidgetStyle.Normal.TintColor` rather than the opaque
// `WidgetStyle`.
//
// Scope filter: only `CPF_Edit` properties are audited. Non-editable
// UPROPERTYs are engine bookkeeping (`bIsVariable`, `bCreatedByConstructionScript`,
// delegate multicast lists) and are either recreated correctly by the builder
// or meaningless to a spec author -- reporting them would bury the real
// findings.
//
// UE 5.7 / 5.8 note: 5.8 tightened `Identical_InContainer` / `ExportTextItem_Direct`
// to take `TNotNull<const void*>` for their first data argument. `TNotNull` has
// an implicit converting constructor from any pointer convertible to the inner
// type, so the raw-pointer call sites below compile unchanged on both engines
// (every pointer we pass is non-null by construction).

#include "Spec/UISpecLossAudit.h"

#include "MonolithUICommon.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelSlot.h"

// Slot classes -- the modelled-prefix table mirrors LeafBuilder/PanelBuilder
// branch-for-branch, so it needs the same slot headers they do.
#include "Components/CanvasPanelSlot.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/GridSlot.h"
#include "Components/UniformGridSlot.h"
#include "Components/SizeBoxSlot.h"
#include "Components/ScaleBoxSlot.h"
#include "Components/WrapBoxSlot.h"
#include "Components/WidgetSwitcherSlot.h"
#include "Components/BorderSlot.h"

// Widget classes the content/style appliers know about.
#include "Components/TextBlock.h"
#include "Components/RichTextBlock.h"
#include "Components/TextWidgetTypes.h"
#include "Components/Image.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/SizeBox.h"
#include "Components/Border.h"
#include "Components/ProgressBar.h"

#include "Internationalization/Text.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace MonolithUI::LossAuditInternal
{
    /** Hard cap on emitted findings so a pathological tree can't flood the response. */
    static constexpr int32 MaxFindings = 200;

    /** Cap on leaves reported for one top-level property (a full FButtonStyle diff is dozens). */
    static constexpr int32 MaxLeavesPerProperty = 8;

    /**
     * How many dotted segments below a top-level property we descend before
     * giving up and reporting the struct wholesale. 3 reaches
     * `WidgetStyle.Normal.TintColor` and `Config.Border.Width`, which is where
     * the useful naming stops.
     */
    static constexpr int32 MaxStructDepth = 3;

    /** Exported values are for human/LLM eyeballs, not re-import -- keep them short. */
    static constexpr int32 MaxValueChars = 160;

    /** One diffing leaf discovered under a top-level property. */
    struct FLeafDiff
    {
        FString Path;
        FString CurrentValue;
        FString DefaultValue;
    };

    /**
     * Structs we treat as atomic values: descending into them yields noise
     * (`.R`, `.G`, `.B`, `.A`) rather than signal. Everything else is walked
     * so composite styles can be named down to the field that changed.
     */
    static bool IsValueLikeStruct(const UStruct* Struct)
    {
        if (!Struct)
        {
            return true;
        }
        static const TSet<FName> ValueLike = {
            FName(TEXT("LinearColor")), FName(TEXT("Color")),
            FName(TEXT("Vector")),      FName(TEXT("Vector2D")),  FName(TEXT("Vector4")),
            FName(TEXT("Vector2f")),    FName(TEXT("Vector3f")),  FName(TEXT("Vector4f")),
            FName(TEXT("Rotator")),     FName(TEXT("Quat")),      FName(TEXT("Transform")),
            FName(TEXT("Margin")),      FName(TEXT("SlateColor")), FName(TEXT("SlateFontInfo")),
            FName(TEXT("IntPoint")),    FName(TEXT("IntVector")), FName(TEXT("Box2D")),
            FName(TEXT("Guid")),        FName(TEXT("DateTime")),  FName(TEXT("Timespan")),
            FName(TEXT("Anchors")),     FName(TEXT("SlateChildSize")),
            FName(TEXT("DataTableRowHandle")),
        };
        return ValueLike.Contains(Struct->GetFName());
    }

    /**
     * True when `Path` is covered by a modelled prefix -- either an exact
     * match or a descendant of one (`Config.Shape.CornerRadii` covers
     * `Config.Shape.CornerRadii.X`).
     */
    static bool IsModelled(const TSet<FString>& Prefixes, const FString& Path)
    {
        if (Prefixes.Contains(Path))
        {
            return true;
        }
        for (const FString& Prefix : Prefixes)
        {
            if (Path.Len() > Prefix.Len()
                && Path.StartsWith(Prefix, ESearchCase::CaseSensitive)
                && Path[Prefix.Len()] == TEXT('.'))
            {
                return true;
            }
        }
        return false;
    }

    /**
     * True when any modelled prefix lives BELOW `Path` -- i.e. descending is
     * still worthwhile because part of this subtree is restored and part is
     * not. Used to keep walking `Config` when only `Config.Shape.*` is modelled.
     */
    static bool HasModelledDescendant(const TSet<FString>& Prefixes, const FString& Path)
    {
        for (const FString& Prefix : Prefixes)
        {
            if (Prefix.Len() > Path.Len()
                && Prefix.StartsWith(Path, ESearchCase::CaseSensitive)
                && Prefix[Path.Len()] == TEXT('.'))
            {
                return true;
            }
        }
        return false;
    }

    static FString ExportTruncated(const FProperty* Prop, const void* ValuePtr)
    {
        if (!Prop || !ValuePtr)
        {
            return FString();
        }
        FString Out;
        Prop->ExportTextItem_Direct(Out, ValuePtr, /*DefaultValue=*/nullptr,
            /*Parent=*/nullptr, PPF_None);
        Out.ReplaceInline(TEXT("\n"), TEXT(" "));
        Out.ReplaceInline(TEXT("\r"), TEXT(""));
        if (Out.Len() > MaxValueChars)
        {
            Out = Out.Left(MaxValueChars) + TEXT("...");
        }
        return Out;
    }

    /**
     * Recursive diff of one property. `InstanceContainer` / `DefaultContainer`
     * are container bases (NOT offset) -- the FProperty applies its own offset.
     *
     * Appends one entry per differing leaf. A struct whose sub-properties all
     * turn out to be modelled (or which we refuse to descend) is reported as a
     * single entry named after the struct itself.
     */
    static void CollectDiffLeaves(
        const FProperty* Prop,
        const void* InstanceContainer,
        const void* DefaultContainer,
        const FString& ParentPath,
        const TSet<FString>& ModelledPrefixes,
        int32 Depth,
        TArray<FLeafDiff>& Out)
    {
        if (!Prop || !InstanceContainer || !DefaultContainer)
        {
            return;
        }

        const FString Path = ParentPath.IsEmpty()
            ? Prop->GetName()
            : ParentPath + TEXT(".") + Prop->GetName();

        // Fully restored by the builders -- nothing here can be lost.
        if (IsModelled(ModelledPrefixes, Path))
        {
            return;
        }

        // Value matches the class default: the rebuild reproduces it anyway.
        //
        // PPF_DeepComparison matters for `Instanced` UPROPERTYs (UWidget has
        // several -- Navigation, AccessibleWidgetData). Without it,
        // FObjectPropertyBase::Identical compares raw pointers, and a
        // per-instance subobject never equals the CDO's even when its contents
        // are identical -- every such widget would report a phantom loss. Deep
        // comparison is also what the details panel's differs-from-default
        // check uses, so "flagged here" lines up with "bold in the editor".
        if (Prop->Identical_InContainer(InstanceContainer, DefaultContainer,
            /*ArrayIndex=*/0, PPF_DeepComparison))
        {
            return;
        }

        const FStructProperty* AsStruct = CastField<FStructProperty>(Prop);
        const UStruct* InnerStruct = AsStruct ? AsStruct->Struct.Get() : nullptr;
        const bool bDescend = InnerStruct
            && !IsValueLikeStruct(InnerStruct)
            && (Depth < MaxStructDepth || HasModelledDescendant(ModelledPrefixes, Path));

        if (bDescend)
        {
            const void* InstSub = AsStruct->ContainerPtrToValuePtr<void>(InstanceContainer);
            const void* DefSub  = AsStruct->ContainerPtrToValuePtr<void>(DefaultContainer);

            const int32 Before = Out.Num();
            for (TFieldIterator<FProperty> It(InnerStruct); It; ++It)
            {
                CollectDiffLeaves(*It, InstSub, DefSub, Path, ModelledPrefixes, Depth + 1, Out);
                if (Out.Num() - Before >= MaxLeavesPerProperty)
                {
                    break;
                }
            }

            // Descent named the differing leaves -- done.
            if (Out.Num() > Before)
            {
                return;
            }
            // Otherwise it found nothing nameable (every child modelled, or the
            // delta lives in a non-reflected part of the struct). Fall through
            // and report the struct itself so the loss is still surfaced.
        }

        FLeafDiff Leaf;
        Leaf.Path         = Path;
        Leaf.CurrentValue = ExportTruncated(Prop, Prop->ContainerPtrToValuePtr<void>(InstanceContainer));
        Leaf.DefaultValue = ExportTruncated(Prop, Prop->ContainerPtrToValuePtr<void>(DefaultContainer));
        Out.Add(MoveTemp(Leaf));
    }

    /** Properties we never audit regardless of class. */
    static bool ShouldSkipProperty(const FProperty* Prop)
    {
        if (!Prop)
        {
            return true;
        }
        // Only designer-editable state. Everything else is engine bookkeeping.
        if (!Prop->HasAnyPropertyFlags(CPF_Edit))
        {
            return true;
        }
        if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
        {
            return true;
        }
        // Delegates are re-bound by name after the rebuild (the reporter
        // confirmed K2Node_ComponentBoundEvent survives), and their exported
        // text is noise.
        if (CastField<FMulticastDelegateProperty>(Prop) || CastField<FDelegateProperty>(Prop))
        {
            return true;
        }
        return false;
    }

    /**
     * Report any keyed FText on `Widget`. The builders rebuild text through
     * `FText::FromString`, which produces a culture-invariant text: the
     * namespace and key are gone, and any string-table / loc mapping keyed off
     * them breaks silently.
     */
    static void CollectLocalizationLosses(
        const UWidget* Widget,
        TArray<FUISpecLossFinding>& Out,
        int32& InOutTruncated)
    {
        for (TFieldIterator<FTextProperty> It(Widget->GetClass()); It; ++It)
        {
            const FTextProperty* TextProp = *It;
            if (!TextProp || TextProp->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
            {
                continue;
            }

            const FText& Value = TextProp->GetPropertyValue_InContainer(Widget);
            if (Value.IsEmpty())
            {
                continue;
            }

            const TOptional<FString> Namespace = FTextInspector::GetNamespace(Value);
            const TOptional<FString> Key       = FTextInspector::GetKey(Value);
            const bool bHasNamespace = Namespace.IsSet() && !Namespace.GetValue().IsEmpty();
            const bool bHasKey       = Key.IsSet() && !Key.GetValue().IsEmpty();
            if (!bHasNamespace && !bHasKey)
            {
                continue;
            }

            if (Out.Num() >= MaxFindings)
            {
                ++InOutTruncated;
                continue;
            }

            FUISpecLossFinding Finding;
            Finding.Kind         = EUISpecLossKind::LocalizationKey;
            Finding.WidgetId     = Widget->GetFName();
            Finding.WidgetClass  = Widget->GetClass()->GetName();
            Finding.PropertyPath = FString::Printf(TEXT("%s.%s"),
                *Widget->GetName(), *TextProp->GetName());
            Finding.CurrentValue = FString::Printf(TEXT("namespace='%s' key='%s'"),
                bHasNamespace ? *Namespace.GetValue() : TEXT(""),
                bHasKey ? *Key.GetValue() : TEXT(""));
            Finding.DefaultValue = TEXT("namespace='' key=<fresh GUID>");
            Out.Add(MoveTemp(Finding));
        }
    }

    /** Diff every editable property on `Object` against `Defaults`. */
    static void AuditObject(
        const UObject* Object,
        const UObject* Defaults,
        const UWidget* OwningWidget,
        const TSet<FString>& ModelledPrefixes,
        EUISpecLossKind Kind,
        const TCHAR* PathPrefix,
        TArray<FUISpecLossFinding>& Out,
        int32& InOutTruncated)
    {
        if (!Object || !Defaults || !OwningWidget)
        {
            return;
        }

        for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
        {
            const FProperty* Prop = *It;
            if (ShouldSkipProperty(Prop))
            {
                continue;
            }

            TArray<FLeafDiff> Leaves;
            CollectDiffLeaves(Prop, Object, Defaults, FString(), ModelledPrefixes,
                /*Depth=*/0, Leaves);

            for (const FLeafDiff& Leaf : Leaves)
            {
                if (Out.Num() >= MaxFindings)
                {
                    ++InOutTruncated;
                    continue;
                }
                FUISpecLossFinding Finding;
                Finding.Kind         = Kind;
                Finding.WidgetId     = OwningWidget->GetFName();
                Finding.WidgetClass  = OwningWidget->GetClass()->GetName();
                Finding.PropertyPath = FString::Printf(TEXT("%s.%s%s"),
                    *OwningWidget->GetName(), PathPrefix, *Leaf.Path);
                Finding.CurrentValue = Leaf.CurrentValue;
                Finding.DefaultValue = Leaf.DefaultValue;
                Out.Add(MoveTemp(Finding));
            }
        }
    }
} // namespace MonolithUI::LossAuditInternal


// -----------------------------------------------------------------------------
// Modelled-prefix tables
// -----------------------------------------------------------------------------
//
// These mirror the spec builders' write sites literally. They are NOT the
// property allowlist: the allowlist governs what `set_widget_property` may
// write (it maps `Button.Style -> WidgetStyle`), whereas `FUISpecNode` has no
// field capable of carrying a FButtonStyle. Only what a builder writes from the
// spec survives a rebuild, so only that belongs here.
//
// When LeafBuilder / PanelBuilder / EffectSurfaceBuilder gains a write, add the
// engine-side path prefix below or the audit will report a false positive.

void FUISpecLossAudit::GetModelledPathPrefixes(const UWidget* Widget, TSet<FString>& OutPrefixes)
{
    if (!Widget)
    {
        return;
    }

    // LeafBuilder::ApplyCommonStyle -- every widget.
    OutPrefixes.Add(TEXT("RenderOpacity"));
    OutPrefixes.Add(TEXT("Visibility"));

    // The parent slot is audited separately against its own slot-class default.
    OutPrefixes.Add(TEXT("Slot"));

    if (Widget->IsA<UTextLayoutWidget>())
    {
        // LeafBuilder::ApplyContent -- SetAutoWrapText on TextBlock/RichTextBlock.
        OutPrefixes.Add(TEXT("AutoWrapText"));
    }
    if (Widget->IsA<UTextBlock>())
    {
        OutPrefixes.Add(TEXT("Text"));
        OutPrefixes.Add(TEXT("Font"));
        OutPrefixes.Add(TEXT("ColorAndOpacity"));
    }
    else if (Widget->IsA<URichTextBlock>())
    {
        OutPrefixes.Add(TEXT("Text"));
    }
    else if (Widget->IsA<UImage>())
    {
        // SetBrushFromTexture / SetBrushFromMaterial write the whole brush.
        OutPrefixes.Add(TEXT("Brush"));
    }
    else if (Widget->IsA<UEditableText>() || Widget->IsA<UEditableTextBox>())
    {
        OutPrefixes.Add(TEXT("Text"));
        OutPrefixes.Add(TEXT("HintText"));
    }

    if (Widget->IsA<USizeBox>())
    {
        OutPrefixes.Add(TEXT("WidthOverride"));
        OutPrefixes.Add(TEXT("HeightOverride"));
        OutPrefixes.Add(TEXT("MinDesiredWidth"));
        OutPrefixes.Add(TEXT("MinDesiredHeight"));
        OutPrefixes.Add(TEXT("MaxDesiredWidth"));
        OutPrefixes.Add(TEXT("MaxDesiredHeight"));
        // The typed setters flip the matching override bits.
        OutPrefixes.Add(TEXT("bOverride_WidthOverride"));
        OutPrefixes.Add(TEXT("bOverride_HeightOverride"));
        OutPrefixes.Add(TEXT("bOverride_MinDesiredWidth"));
        OutPrefixes.Add(TEXT("bOverride_MinDesiredHeight"));
        OutPrefixes.Add(TEXT("bOverride_MaxDesiredWidth"));
        OutPrefixes.Add(TEXT("bOverride_MaxDesiredHeight"));
    }
    if (Widget->IsA<UBorder>())
    {
        OutPrefixes.Add(TEXT("BrushColor"));
        OutPrefixes.Add(TEXT("Padding"));
    }
    if (Widget->IsA<UProgressBar>())
    {
        OutPrefixes.Add(TEXT("FillColorAndOpacity"));
    }

    // UEffectSurface ships from an optional provider plugin, so it is matched
    // by class NAME rather than a compile-time type -- the class may not exist
    // in a stripped build. EffectSurfaceBuilder::ApplyEffect writes exactly
    // four curated paths (Shape.CornerRadii, Shape.Smoothness, Fill.SolidColor,
    // BackdropBlur.Strength); the rest of FEffectSurfaceConfig -- Border, Glow,
    // gradient stops, and BOTH shadow stacks -- is read by the serializer but
    // never written back, so it is genuinely lost on a rebuild.
    for (const UClass* C = Widget->GetClass(); C; C = C->GetSuperClass())
    {
        if (C->GetFName() == FName(TEXT("EffectSurface")))
        {
            OutPrefixes.Add(TEXT("Config.Shape.CornerRadii"));
            OutPrefixes.Add(TEXT("Config.Shape.Smoothness"));
            OutPrefixes.Add(TEXT("Config.Fill.SolidColor"));
            OutPrefixes.Add(TEXT("Config.BackdropBlur.Strength"));
            break;
        }
    }
}

void FUISpecLossAudit::GetModelledSlotPathPrefixes(const UPanelSlot* Slot, TSet<FString>& OutPrefixes)
{
    if (!Slot)
    {
        return;
    }

    // Mirrors LeafBuilder::ApplySlotFields / PanelBuilder::ApplySlotFields
    // branch-for-branch. Order matters the same way theirs does (most-derived
    // slot classes first is not required here -- these are all siblings).
    if (Slot->IsA<UCanvasPanelSlot>())
    {
        // SetAnchors / SetPosition / SetSize / SetAlignment all land in LayoutData.
        OutPrefixes.Add(TEXT("LayoutData"));
        OutPrefixes.Add(TEXT("bAutoSize"));
        OutPrefixes.Add(TEXT("ZOrder"));
        return;
    }
    if (Slot->IsA<UVerticalBoxSlot>() || Slot->IsA<UHorizontalBoxSlot>())
    {
        OutPrefixes.Add(TEXT("Padding"));
        OutPrefixes.Add(TEXT("HorizontalAlignment"));
        OutPrefixes.Add(TEXT("VerticalAlignment"));
        OutPrefixes.Add(TEXT("Size"));
        return;
    }
    if (Slot->IsA<UGridSlot>())
    {
        OutPrefixes.Add(TEXT("Padding"));
        OutPrefixes.Add(TEXT("HorizontalAlignment"));
        OutPrefixes.Add(TEXT("VerticalAlignment"));
        OutPrefixes.Add(TEXT("Row"));
        OutPrefixes.Add(TEXT("Column"));
        OutPrefixes.Add(TEXT("RowSpan"));
        OutPrefixes.Add(TEXT("ColumnSpan"));
        OutPrefixes.Add(TEXT("Layer"));
        return;
    }
    if (Slot->IsA<UUniformGridSlot>())
    {
        OutPrefixes.Add(TEXT("HorizontalAlignment"));
        OutPrefixes.Add(TEXT("VerticalAlignment"));
        OutPrefixes.Add(TEXT("Row"));
        OutPrefixes.Add(TEXT("Column"));
        return;
    }
    if (Slot->IsA<UScaleBoxSlot>())
    {
        // ApplyAlignmentOnly -- padding is deliberately NOT written here.
        OutPrefixes.Add(TEXT("HorizontalAlignment"));
        OutPrefixes.Add(TEXT("VerticalAlignment"));
        return;
    }
    if (Slot->IsA<UOverlaySlot>()
        || Slot->IsA<UScrollBoxSlot>()
        || Slot->IsA<USizeBoxSlot>()
        || Slot->IsA<UWrapBoxSlot>()
        || Slot->IsA<UWidgetSwitcherSlot>()
        || Slot->IsA<UBorderSlot>())
    {
        OutPrefixes.Add(TEXT("Padding"));
        OutPrefixes.Add(TEXT("HorizontalAlignment"));
        OutPrefixes.Add(TEXT("VerticalAlignment"));
        return;
    }
    // Unknown slot class: the builders write nothing to it, so nothing is
    // modelled and everything that differs from default gets reported.
}


// -----------------------------------------------------------------------------
// FUISpecLossAudit::Audit
// -----------------------------------------------------------------------------

FUISpecLossAuditResult FUISpecLossAudit::Audit(const UWidgetBlueprint* WBP)
{
    using namespace MonolithUI::LossAuditInternal;

    FUISpecLossAuditResult Result;
    if (!WBP || !WBP->WidgetTree)
    {
        return Result;
    }

    TArray<UWidget*> AllWidgets;
    WBP->WidgetTree->GetAllWidgets(AllWidgets);

    for (const UWidget* Widget : AllWidgets)
    {
        if (!Widget || !Widget->GetClass())
        {
            continue;
        }
        ++Result.WidgetsAudited;

        const int32 BeforeWidget = Result.Findings.Num();

        const UObject* WidgetDefaults = Widget->GetClass()->GetDefaultObject();
        if (WidgetDefaults)
        {
            TSet<FString> ModelledPrefixes;
            GetModelledPathPrefixes(Widget, ModelledPrefixes);
            AuditObject(Widget, WidgetDefaults, Widget, ModelledPrefixes,
                EUISpecLossKind::UnmodelledProperty, TEXT(""),
                Result.Findings, Result.TruncatedCount);
        }

        // Parent slot. Slot objects are recreated by AddChild on the rebuild,
        // so anything the builders don't rewrite reverts to slot-class default.
        if (const UPanelSlot* Slot = Widget->Slot.Get())
        {
            if (Slot->GetClass())
            {
                if (const UObject* SlotDefaults = Slot->GetClass()->GetDefaultObject())
                {
                    TSet<FString> ModelledSlotPrefixes;
                    GetModelledSlotPathPrefixes(Slot, ModelledSlotPrefixes);
                    AuditObject(Slot, SlotDefaults, Widget, ModelledSlotPrefixes,
                        EUISpecLossKind::UnmodelledSlotProperty, TEXT("Slot."),
                        Result.Findings, Result.TruncatedCount);
                }
            }
        }

        const int32 BeforeLoc = Result.Findings.Num();
        CollectLocalizationLosses(Widget, Result.Findings, Result.TruncatedCount);
        Result.LocalizationKeysReset += (Result.Findings.Num() - BeforeLoc);

        if (Result.Findings.Num() > BeforeWidget)
        {
            ++Result.WidgetsWithLoss;
        }
    }

    if (Result.HasLoss())
    {
        UE_LOG(LogMonolithUISpec, Warning,
            TEXT("UISpecLossAudit: '%s' rebuild would reset %d propert%s across %d of %d widget(s)%s."),
            *WBP->GetName(),
            Result.Findings.Num(),
            Result.Findings.Num() == 1 ? TEXT("y") : TEXT("ies"),
            Result.WidgetsWithLoss,
            Result.WidgetsAudited,
            Result.TruncatedCount > 0
                ? *FString::Printf(TEXT(" (+%d suppressed)"), Result.TruncatedCount)
                : TEXT(""));
    }

    return Result;
}

void FUISpecLossAudit::AppendAsWarnings(
    const FUISpecLossAuditResult& AuditResult,
    TArray<FUISpecError>& OutWarnings)
{
    if (!AuditResult.HasLoss())
    {
        return;
    }

    // Summary first, so a caller that reads only the head of the warning list
    // still learns the magnitude and the remedy.
    {
        FUISpecError Summary;
        Summary.Severity = EUISpecErrorSeverity::Warning;
        Summary.Category = TEXT("DataLoss");
        Summary.JsonPath = TEXT("/rootWidget");
        Summary.Message  = FString::Printf(
            TEXT("mode=rebuild tears the existing widget tree down and re-creates it from the spec. ")
            TEXT("%d propert%s on %d of %d widget(s) differ from class defaults and are NOT modelled by ")
            TEXT("the spec schema -- they will be reset%s."),
            AuditResult.Findings.Num(),
            AuditResult.Findings.Num() == 1 ? TEXT("y") : TEXT("ies"),
            AuditResult.WidgetsWithLoss,
            AuditResult.WidgetsAudited,
            AuditResult.TruncatedCount > 0
                ? *FString::Printf(TEXT(" (+%d further findings suppressed)"), AuditResult.TruncatedCount)
                : TEXT(""));
        Summary.SuggestedFix = TEXT("Pass mode=\"patch\" to update only what the spec names and leave everything else intact, or dry_run=true to preview this list without mutating the asset.");
        OutWarnings.Add(MoveTemp(Summary));
    }

    for (const FUISpecLossFinding& Finding : AuditResult.Findings)
    {
        FUISpecError W;
        W.Severity = EUISpecErrorSeverity::Warning;
        W.WidgetId = Finding.WidgetId;
        W.JsonPath = FString::Printf(TEXT("/rootWidget/%s"), *Finding.WidgetId.ToString());

        switch (Finding.Kind)
        {
        case EUISpecLossKind::LocalizationKey:
            W.Category = TEXT("DataLoss.Localization");
            W.Message  = FString::Printf(
                TEXT("%s carries a localization key (%s). The spec stores text as a plain string, so ")
                TEXT("the rebuild re-creates it via FText::FromString: the namespace is cleared and a ")
                TEXT("fresh key GUID is minted, breaking existing loc mappings."),
                *Finding.PropertyPath, *Finding.CurrentValue);
            W.SuggestedFix = TEXT("Use mode=\"patch\" -- it leaves an unchanged FText untouched, preserving its namespace and key.");
            break;

        case EUISpecLossKind::UnmodelledSlotProperty:
            W.Category = TEXT("DataLoss.Slot");
            W.Message  = FString::Printf(
                TEXT("%s = %s is not modelled by the spec schema and the rebuild recreates the slot, ")
                TEXT("so it will reset to %s."),
                *Finding.PropertyPath, *Finding.CurrentValue, *Finding.DefaultValue);
            W.SuggestedFix = TEXT("Use mode=\"patch\" to keep the existing slot object, or re-apply this property afterwards via ui::set_widget_property.");
            break;

        case EUISpecLossKind::UnmodelledProperty:
        default:
            W.Category = TEXT("DataLoss.Property");
            W.Message  = FString::Printf(
                TEXT("%s = %s is not modelled by the spec schema and will reset to the class default %s."),
                *Finding.PropertyPath, *Finding.CurrentValue, *Finding.DefaultValue);
            W.SuggestedFix = TEXT("Use mode=\"patch\" to keep the existing widget object, or re-apply this property afterwards via ui::set_widget_property.");
            break;
        }

        OutWarnings.Add(MoveTemp(W));
    }
}
