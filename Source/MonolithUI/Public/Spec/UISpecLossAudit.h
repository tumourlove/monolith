// Copyright tumourlove. All Rights Reserved.
// UISpecLossAudit.h
//
// Pre-flight data-loss audit for `ui::build_ui_from_spec` in its default
// (rebuild) mode.
//
// Why this exists: a rebuild is a full teardown. `FUISpecBuilder` empties the
// WidgetTree and re-constructs every widget from the spec, so every property
// the spec schema does NOT model silently reverts to its class default. The
// spec models a deliberately small surface (see `FUISpecNode`: Slot / Style /
// Content / Effect / CommonUI); a hand-styled WBP carries far more than that
// -- FButtonStyle tints, UBackgroundBlur::BlurStrength, tooltips, clipping,
// render transforms. None of it survives, and before this audit none of it was
// reported either: the response read `created:N, modified:0, removed:N` with
// `warning_count: 0`.
//
// What the audit does: walk the LIVE widget tree before the teardown and, for
// every widget, diff each editable UPROPERTY against its class default (CDO).
// Anything that differs AND is not restored by the spec builders is a property
// the rebuild is about to drop -- one finding each, named by widget + dotted
// property path (e.g. `Button_Play.WidgetStyle.Normal.TintColor`).
//
// Localization is audited the same way: `FUISpecContent::Text` is a plain
// FString, so the builder re-creates every FText via `FText::FromString`,
// which mints a culture-invariant text with no namespace/key. Any keyed FText
// on the source widget therefore loses its loc mapping, and gets its own
// finding.
//
// Source of truth for "modelled": the SPEC BUILDERS (LeafBuilder /
// PanelBuilder / EffectSurfaceBuilder), NOT the property allowlist. The
// allowlist is what `set_widget_property` may WRITE (it includes e.g.
// `Button.Style -> WidgetStyle`); the spec document has no field that carries
// a FButtonStyle, so allowlist membership says nothing about round-trip
// survival. `GetModelledPathPrefixes` mirrors the builders' write sites
// literally -- when a builder gains a write, add the prefix there.
//
// Threading: editor-only, main-thread. Pure read; never mutates the WBP.

#pragma once

#include "CoreMinimal.h"
#include "Spec/UISpec.h"

class UPanelSlot;
class UWidget;
class UWidgetBlueprint;

/** Classification of a single loss finding. Mirrors `FUISpecError::Category`. */
enum class EUISpecLossKind : uint8
{
    /** A widget property that differs from its class default and the spec cannot carry. */
    UnmodelledProperty,
    /** A parent-slot property that differs from its slot-class default and the spec cannot carry. */
    UnmodelledSlotProperty,
    /** A keyed FText whose namespace/key the rebuild will discard. */
    LocalizationKey,
};

/**
 * One property the rebuild is about to reset. `PropertyPath` is already
 * widget-qualified so the string can be surfaced verbatim to the caller.
 */
struct MONOLITHUI_API FUISpecLossFinding
{
    /** Widget variable name (`UWidget::GetFName`). */
    FName WidgetId;

    /** Widget UClass name, for disambiguation in the report. */
    FString WidgetClass;

    /** Widget-qualified dotted path, e.g. `Button_Play.WidgetStyle.Normal.TintColor`. */
    FString PropertyPath;

    /** Exported current value, truncated. Empty when export failed. */
    FString CurrentValue;

    /** Exported class-default value, truncated. Empty when export failed. */
    FString DefaultValue;

    /** What kind of loss this is. */
    EUISpecLossKind Kind = EUISpecLossKind::UnmodelledProperty;
};

/**
 * Aggregate of one audit pass. `Findings` is capped (see `MaxFindings` in the
 * .cpp) so a pathological tree cannot flood the response; `TruncatedCount`
 * carries the overflow so the caller still learns the true magnitude.
 */
struct MONOLITHUI_API FUISpecLossAuditResult
{
    /** Every property the rebuild would reset, in tree order. */
    TArray<FUISpecLossFinding> Findings;

    /** Widgets visited. */
    int32 WidgetsAudited = 0;

    /** Widgets with at least one finding. */
    int32 WidgetsWithLoss = 0;

    /** Findings of kind LocalizationKey (subset of `Findings`). */
    int32 LocalizationKeysReset = 0;

    /** Findings suppressed by the per-property / global caps. */
    int32 TruncatedCount = 0;

    /** True when the pass found anything at all. */
    bool HasLoss() const { return Findings.Num() > 0 || TruncatedCount > 0; }
};

/**
 * Stateless auditor. Static methods only, matching `FUISpecBuilder` /
 * `FUISpecSerializer`.
 */
class MONOLITHUI_API FUISpecLossAudit
{
public:
    /**
     * Walk `WBP`'s widget tree and report every property a rebuild would
     * reset. Never throws; returns an empty result for a null WBP or a WBP
     * with no WidgetTree.
     */
    static FUISpecLossAuditResult Audit(const UWidgetBlueprint* WBP);

    /**
     * Project an audit result into `FUISpecError` warnings so it can ride the
     * existing validation surface (`FUISpecValidationResult::Warnings` feeds
     * the `warning_count` line in `ToLLMReport`).
     *
     * Emits one warning per finding plus a leading summary warning, so a
     * caller that only reads `warning_count` still sees a non-zero number and
     * a caller that reads the messages gets the exact property list.
     */
    static void AppendAsWarnings(
        const FUISpecLossAuditResult& AuditResult,
        TArray<FUISpecError>& OutWarnings);

    /**
     * Dotted engine-side path prefixes that the spec builders restore for
     * `Widget`. A property path is considered modelled when it equals one of
     * these prefixes or is a descendant of one.
     *
     * Exposed (rather than kept file-static) so tests can assert the table
     * against the builders without reaching through the audit entry point.
     */
    static void GetModelledPathPrefixes(const UWidget* Widget, TSet<FString>& OutPrefixes);

    /** Same, for a widget's parent slot object. */
    static void GetModelledSlotPathPrefixes(const UPanelSlot* Slot, TSet<FString>& OutPrefixes);
};
