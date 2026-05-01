// Copyright tumourlove. All Rights Reserved.
// UISpecSerializer.h
//
// Phase J -- inverse of `FUISpecBuilder`. Reads a live `UWidgetBlueprint` and
// produces an `FUISpecDocument` populated such that
//
//     Build(Serialize(WBP)) == WBP'
//
// holds for every property the Phase B/C allowlist exposes (and a handful of
// adjacent fields that are required for structural fidelity but live outside
// the allowlist -- see the lossy-boundary catalogue at the bottom of this
// header).
//
// What roundtrips:
//   * Widget tree topology (root + recursive children, names, classes)
//   * ALL stock UMG panel-slot types (Canvas, Vertical, Horizontal, Overlay,
//     ScrollBox, Grid, UniformGrid, SizeBox, ScaleBox, WrapBox, WidgetSwitcher,
//     Border) -- not just the four the legacy `SerializeSlotProperties` knew.
//   * Style sub-bag (width/height/padding/background/borderColor/borderWidth/
//     opacity/visibility) read via the registry's curated property mapping +
//     direct reflection where the mapping is silent.
//   * Content sub-bag (text/font size/font color/wrap mode/brush path/placeholder)
//   * Effect sub-bag (corner radii / smoothness / solid color / drop shadow array
//     / inner shadow array / backdrop blur strength) when the widget IS-A
//     UEffectSurface. Drop / inner shadow arrays serialise via a hand-rolled
//     array path -- the Phase H ApplyJsonPath helper deliberately does not
//     navigate TArray container properties, so Phase J implements the symmetric
//     read AND write here.
//   * CommonUI sub-bag (input layer / input mode / styleRefs[]) when the widget
//     IS-A CommonActivatableWidget / CommonUserWidget (and WITH_COMMONUI=1).
//   * Named animations -- iterates `WBP->Animations`, captures display label,
//     loop mode, autoplay, and reconstructs FUISpecAnimation entries.
//   * Document-scoped Styles map -- v1 mirrors any named entries already on
//     the source spec when re-serializing; for a fresh dump from an existing
//     WBP (no prior spec source) the Styles map is left empty, since the spec
//     format's "named styles" concept doesn't have a direct UMG counterpart
//     beyond the CommonUI style class table.
//
// What is LOSSY (documented in SPEC_MonolithUI.md "Spec Serializer (M5 -- J)"):
//   * Style asset class references (CommonUI buttons / text styles) serialise
//     as the asset path -- on rebuild we resolve the path, but a manual move /
//     rename of the style asset between dump and build will break the link.
//   * Native widget bindings (graph-bound delegates, widget native ticks) are
//     captured by NAME ONLY. The spec carries an animationRef-style entry; the
//     graph wiring itself is not roundtripped (UISpec is a UMG layout format,
//     not a Blueprint graph format).
//   * Editor-only metadata (DisplayLabel-vs-Name divergence, DesignerSize) is
//     dropped intentionally -- these don't drive runtime behaviour.
//   * Per-property animation curve TANGENT data: keyframes serialise with
//     {time, scalarValue, easing}; rich Bezier tangents (ArriveTangent /
//     LeaveTangent / ArriveWeight / LeaveWeight) are populated only if the
//     source MovieScene track exposes them via the v1 reader. Curve shapes
//     beyond Linear / Cubic / Constant fall back to Linear with a warning.
//   * Slot fields outside the per-slot-class subset documented below silently
//     default. The validator on the rebuild side warns on this rather than
//     erroring -- the spec is intentionally permissive here.
//
// Threading: editor-only, main-thread. Walks live UObject memory via
// reflection; no off-thread access. WITH_EDITOR-gated.
//
// Clean-room: serializer dispatch shape (per-slot-class branch + per-sub-bag
// reader) is OURS. The "globalAnimations + globalStyles + rootWidget" layout
// of the FUISpecDocument is one valid shape -- we re-derived ours from CSS
// box-model first principles + the existing MonolithUI action surface.

#pragma once

#include "CoreMinimal.h"
#include "Spec/UISpec.h"

class UWidgetBlueprint;

/**
 * Caller-supplied inputs to one Dump pass. Mirrors `FUISpecBuilderInputs` so
 * a future caller can transform a spec document round-trip-style:
 *
 *     auto Dumped = FUISpecSerializer::Dump({.AssetPath = "/Game/UI/X"});
 *     FUISpecBuilder::Build({.Document = &Dumped.Document, .AssetPath = "..."});
 */
struct MONOLITHUI_API FUISpecSerializerInputs
{
    /** Long-package asset path for the WBP to read (e.g. "/Game/UI/MyMenu"). */
    FString AssetPath;

    /**
     * When true, include serialised default-value fields even when they match
     * the engine UPROPERTY default. Default false -- omitting defaults keeps
     * the resulting JSON small and lets the validator's "missing-key" warnings
     * actually fire on the rebuild side.
     */
    bool bEmitDefaults = false;

    /**
     * Optional caller-supplied request id; echoed back on the result so the
     * action handler can route a response on the parallel Phase K wire.
     */
    FString RequestId;
};

/**
 * Result of one Dump pass. `bSuccess` is the gate; on false, `Errors` is
 * non-empty and `Document` is left default-constructed (Root = nullptr).
 *
 * The result struct mirrors `FUISpecBuilderResult`'s shape so action-handler
 * code can compose the two passes without type acrobatics.
 */
struct MONOLITHUI_API FUISpecSerializerResult
{
    /** True when the serialiser produced a usable document. */
    bool bSuccess = false;

    /** Asset path of the WBP that was read. */
    FString AssetPath;

    /** Echoed back from inputs for caller correlation. */
    FString RequestId;

    /** The reconstructed document. Root is non-null on bSuccess=true. */
    FUISpecDocument Document;

    /** Errors raised during serialisation. */
    TArray<FUISpecError> Errors;

    /**
     * Warnings about lossy-boundary fields that could not be perfectly
     * captured. Populated even on bSuccess=true -- the validator on the
     * rebuild side may then escalate them per `treat_warnings_as_errors`.
     */
    TArray<FUISpecError> Warnings;

    /** Number of widgets visited during the dump. */
    int32 NodesVisited = 0;

    /** Number of animations captured. */
    int32 AnimationsCaptured = 0;
};

/**
 * Stateless serialiser. Static methods only -- no per-instance state, so
 * concurrent dumps on different WBPs are safe in principle (in practice
 * editor-main-thread because the reflection walk we do touches live UObjects).
 */
class MONOLITHUI_API FUISpecSerializer
{
public:
    /**
     * Single entry point. Loads the WBP at `Inputs.AssetPath` and walks its
     * widget tree + animation collection to populate a FUISpecDocument.
     *
     * Returns a populated FUISpecSerializerResult -- never throws, never
     * crashes on bad input. `Document.Root` is null on failure.
     */
    static FUISpecSerializerResult Dump(const FUISpecSerializerInputs& Inputs);

    /**
     * Convenience: Dump a WBP that's already loaded (skips the asset-path
     * resolve + load). Used by tests and automation so they can drive a
     * known-good WBP without touching the asset registry.
     */
    static FUISpecSerializerResult DumpFromWBP(
        UWidgetBlueprint* WBP,
        const FString& AssetPath = FString(),
        const FString& RequestId = FString());
};
