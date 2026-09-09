// Copyright tumourlove. All Rights Reserved.
// UIBuildContext.h
//
// Phase H — per-build-pass scratch state for `FUISpecBuilder::Build`. NOT a
// member of the builder (the builder is a stateless dispatcher). NOT a UCLASS
// (it lives entirely on the call stack and outlives only the single build
// pass that owns it). Holding the context as plain C++ keeps the builder
// reentrant in principle and makes the rollback/dry-run wiring local to the
// call site.
//
// Clean-room note: the reference plugin uses a generator-as-stateful-object
// pattern where the same instance accumulates state across calls. We
// explicitly do NOT — every Build pass owns a freshly-constructed
// FUIBuildContext that goes out of scope when the pass returns.
//
// Lifetime contract:
//   * Owned on the stack inside `FUISpecBuilder::Build`.
//   * Destroyed when the call returns (RAII).
//   * Borrowed (not owned) by the per-node sub-builders via `FUIBuildContext&`.
//   * Sub-builders MUST NOT cache pointers into the context across calls.
//
// Field semantics:
//   * `Document` is a borrowed const-ref to the caller-owned spec.
//   * `WidgetIdMap` is the editor-side widget-id -> live UWidget* map, rebuilt
//     post-compile per the H12/H13 contract (see UISpecBuilder.cpp for why).
//   * `PreCreatedStyles` is populated by the H10/H11 pre-create-styles pass
//     BEFORE the first widget is constructed.
//   * `StyleHookCalls` is a test-injection counter so H10 can verify the
//     order-of-operations invariant without cracking open the cache internals.
//   * `bDryRun` and `bTreatWarningsAsErrors` mirror the FUISpecBuilderInputs
//     fields so the per-node sub-builders don't need to dig back through the
//     inputs struct.
//   * Diff/counters are populated regardless of dry-run so the dry-run path
//     can return the same shape the live path returns.

#pragma once

#include "CoreMinimal.h"
#include "Spec/UISpec.h"

class UPanelWidget;
class UWidget;
class UWidgetBlueprint;
class FUIPropertyAllowlist;
class FUIPropertyPathCache;

/**
 * Per-pass scratch state. One instance per `FUISpecBuilder::Build` call.
 *
 * Construction is cheap — all fields default-initialise. The context is
 * populated as the build pipeline progresses; sub-builders read from it,
 * sub-builders write to it, but the *builder itself* does not retain any
 * pointer or reference to it after Build returns.
 */
struct FUIBuildContext
{
    /** Borrowed const-ref to the caller-owned spec document. */
    const FUISpecDocument* Document = nullptr;

    /** Target Widget Blueprint — set after the get-or-create step (H2). */
    UWidgetBlueprint* TargetWBP = nullptr;

    /** Asset path of the target WBP (e.g. "/Game/UI/MyWidget"). */
    FString AssetPath;

    /** Caller-supplied request id (echoed back in the response, may be empty). */
    FString RequestId;

    /** True when the caller asked for dry-run (no commit, no save). */
    bool bDryRun = false;

    /** True when the caller asked warnings to escalate to errors. */
    bool bTreatWarningsAsErrors = false;

    /** True when raw_mode bypasses the per-write allowlist gate. */
    bool bRawMode = false;

    /**
     * True when the pass runs in PATCH mode (issue #139). Patch mode skips the
     * teardown entirely: existing widgets are matched by spec id, reused in
     * place, and only the spec-modelled surface is written on top. Everything
     * the spec cannot express -- FButtonStyle tints, BlurStrength, localization
     * keys, unmodelled slot fields -- survives because the widget and slot
     * UObjects are never destroyed.
     *
     * Rebuild mode (the default) leaves this false and behaves exactly as it
     * did before, so existing callers see no change.
     */
    bool bPatchMode = false;

    /**
     * Patch mode only. Pre-existing widgets keyed by variable name, captured
     * BEFORE the walk. The sub-builders consume entries from here instead of
     * calling ConstructWidget; whatever is never consumed is what the spec
     * dropped, and gets removed in the post-walk sweep.
     */
    TMap<FName, TWeakObjectPtr<UWidget>> ReusableWidgets;

    /** Patch mode only. Ids actually reused this pass (drives the orphan sweep). */
    TSet<FName> ReusedWidgetIds;

    /**
     * Patch mode only. Next spec-order child index per parent panel. Children
     * are placed at the cursor (ShiftChild for an in-place child, InsertChildAt
     * for a moved/new one) so spec order wins without destroying slots; after
     * the walk, children at or beyond the final cursor are the ones the spec
     * no longer names.
     */
    TMap<TWeakObjectPtr<UPanelWidget>, int32> PatchChildCursor;

    /** Counter — widgets reused in place rather than reconstructed (patch mode). */
    int32 NodesReused = 0;

    /**
     * Editor-side variable-name -> live UWidget* map. Populated AFTER compile
     * by walking the post-compile WidgetTree (closes recon takeaway §8).
     * Sub-builders that walk the spec tree pre-compile populate the
     * `PreCompileWidgets` map below; post-compile we copy in the rebuilt
     * pointers.
     */
    TMap<FName, TWeakObjectPtr<UWidget>> WidgetIdMap;

    /**
     * Pre-compile widget pointer map. Populated during the tree walk so
     * sub-builders can resolve parent widgets by id without re-scanning
     * the WidgetTree on every node.
     *
     * THIS MAP GOES STALE after `FKismetEditorUtilities::CompileBlueprint`.
     * The `WidgetIdMap` above is the post-compile rebuild — animation and
     * post-compile passes MUST consult `WidgetIdMap`, not `PreCompileWidgets`.
     */
    TMap<FName, TWeakObjectPtr<UWidget>> PreCompileWidgets;

    /**
     * Pre-created style classes keyed by `(StyleType, hash)`. Populated by
     * the H11 pre-create-styles pass before tree placement. Builders look
     * up resolved style classes here when wiring node `StyleRef` references.
     *
     * Key shape is `<Type>:<8-hex>` (matches FMonolithUIStyleService's
     * derived-name shape). When the spec uses a named style, the mapping
     * also lands at `name:<lookupName>` for direct hit on the second pass.
     */
    TMap<FString, TWeakObjectPtr<UClass>> PreCreatedStyles;

    /**
     * Test-injection counter. Bumped every time the pre-create pass calls
     * the style service. H10 asserts this is non-zero before any widget
     * construction begins.
     */
    int32 StyleHookCalls = 0;

    /** Counter — widgets newly constructed in this pass. */
    int32 NodesCreated = 0;

    /** Counter — widgets whose properties were updated (existing-WBP path). */
    int32 NodesModified = 0;

    /** Counter — widgets removed from the tree (existing-WBP overwrite path). */
    int32 NodesRemoved = 0;

    /**
     * Cached pointer back to the registry's allowlist + path cache so
     * sub-builders don't have to re-resolve through `Get()` per node.
     * Populated by the builder right after Document validation.
     */
    const FUIPropertyAllowlist* Allowlist = nullptr;
    FUIPropertyPathCache* PathCache = nullptr;

    /**
     * Diff entries collected during the pass. On dry-run this is the entire
     * payload returned to the caller; on a live run it's still populated so
     * the response can include "what changed".
     *
     * Each entry is a free-form line; the action handler aggregates them
     * into the response JSON. Format intentionally informal — the LLM-facing
     * shape is `{ kind, json_path, message }` derived in the handler.
     */
    TArray<FString> DiffLines;

    /**
     * Warnings accumulated during the pass that did not fail validation but
     * the caller should know about (e.g. "node X requested unknown style Y;
     * default applied"). Promoted to errors when bTreatWarningsAsErrors.
     */
    TArray<FUISpecError> Warnings;

    /**
     * Errors accumulated during the pass after validation passed but a
     * structural problem was detected mid-build (e.g. parent panel does not
     * accept the requested child class). Trips the rollback path.
     */
    TArray<FUISpecError> Errors;
};
