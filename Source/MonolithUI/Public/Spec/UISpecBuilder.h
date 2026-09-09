// Copyright tumourlove. All Rights Reserved.
// UISpecBuilder.h
//
// Phase H — the centerpiece of the MonolithUI architecture expansion. Drives
// the transactional `ui::build_ui_from_spec` pipeline:
//
//     parse → validate → resolve parent → DRY-WALK
//        → get-or-create WBP → pre-create styles → walk tree
//        → build animations → compile → rebuild widget-id map
//        → save (or rollback on dry-run / failure)
//
// Atomicity strategy (split per the plan §1.4 step 8 + R3 risk mitigation):
//
//   * `FScopedTransaction` wraps the property-write phase on an EXISTING WBP
//     so undo / rollback works for in-place edits.
//   * For the CREATE-NEW path, `FScopedTransaction` does NOT roll back the
//     `CreatePackage` + `UWidgetBlueprintFactory::FactoryCreateNew` side
//     effects on the in-memory package map. We therefore (a) dry-walk the
//     entire spec FIRST (validate every node, resolve every UClass, hash
//     every style) and only call CreatePackage after that succeeds, and
//     (b) on post-create failure manually `Package->MarkAsGarbage()` +
//     `ObjectTools::DeleteSingleObject` on the new asset to undo the disk
//     artefact. This is the H2 contract.
//
// Clean-room: identifiers, dispatch order, FUIBuildContext shape are OURS.
// The reference plugin uses generator-as-stateful-object — we explicitly do
// not (FUIBuildContext is per-pass scratch, NOT a member of the builder).
//
// Threading: editor-only, main-thread. `FKismetEditorUtilities::CompileBlueprint`
// is main-thread; `IAssetTools::CreateUniqueAssetName` is main-thread; the
// transaction system is main-thread. Compile-time-gated by WITH_EDITOR.

#pragma once

#include "CoreMinimal.h"
#include "Spec/UISpec.h"
#include "Spec/UISpecLossAudit.h"

class UWidgetBlueprint;

/**
 * How one Build pass treats a pre-existing WBP (issue #139).
 *
 * `Rebuild` is the historical — and still default — behaviour: the widget tree
 * is torn down and re-created from the spec, so every property the spec schema
 * does not model reverts to its class default. That teardown is now audited
 * up-front (`FUISpecLossAudit`) instead of happening silently.
 *
 * `Patch` reuses existing widgets by id and writes only the spec-modelled
 * surface on top, leaving everything else — including slot objects and
 * localization keys — untouched.
 */
enum class EUISpecBuildMode : uint8
{
    /** Full teardown + recreate. Default; unchanged from pre-#139 behaviour. */
    Rebuild,

    /** Differential update: reuse by id, touch only what the spec names. */
    Patch,
};

/**
 * Caller-supplied inputs to one Build pass. Mirrors the MCP action's params
 * shape (asset_path / mode / overwrite / dry_run / treat_warnings_as_errors /
 * raw_mode / request_id) plus the pre-parsed FUISpecDocument the validator
 * already saw.
 *
 * Keeping these in a single struct lets us add fields without bloating the
 * Build signature when v2 adds e.g. a manifest hash or per-widget override list.
 */
struct MONOLITHUI_API FUISpecBuilderInputs
{
    /** Teardown-vs-patch policy for a pre-existing WBP. Defaults to Rebuild. */
    EUISpecBuildMode Mode = EUISpecBuildMode::Rebuild;

    /** Parsed spec document. The caller (MCP handler) parses + validates first. */
    const FUISpecDocument* Document = nullptr;

    /** Long-package asset path for the WBP (e.g. "/Game/UI/MyMenu"). */
    FString AssetPath;

    /** When true, an existing WBP at AssetPath is fully replaced. Default true. */
    bool bOverwrite = true;

    /** When true, validate + walk + report; do not commit any side effect. */
    bool bDryRun = false;

    /** When true, validator warnings escalate to errors and abort the build. */
    bool bTreatWarningsAsErrors = false;

    /** When true, bypass per-property allowlist gating (legacy compat). */
    bool bRawMode = false;

    /** Optional caller-supplied request id; echoed back in the response. */
    FString RequestId;
};

/**
 * Result of one Build pass. Always populated; never throws. `bSuccess` is
 * the gate; on false, `Errors` is non-empty and the asset is untouched
 * (per the H2 atomicity contract).
 *
 * Counters mirror the FUIBuildContext fields — exposed here so the action
 * handler can assemble the JSON response without reaching into the context.
 */
struct MONOLITHUI_API FUISpecBuilderResult
{
    /** True when the build pass committed (or, in dry-run, would have committed). */
    bool bSuccess = false;

    /** Asset path of the WBP that was built / would be built. */
    FString AssetPath;

    /** Echoed back from inputs for caller correlation. */
    FString RequestId;

    /** Validator findings produced by the pre-build validation gate. */
    FUISpecValidationResult Validation;

    /** Errors raised by the builder itself (post-validation, mid-build). */
    TArray<FUISpecError> Errors;

    /** Warnings raised by the builder. Promoted to errors if treat_warnings_as_errors. */
    TArray<FUISpecError> Warnings;

    /** Number of widgets newly created in this pass. */
    int32 NodesCreated = 0;

    /** Number of widgets whose properties were updated. */
    int32 NodesModified = 0;

    /** Number of widgets removed (existing-WBP overwrite path). */
    int32 NodesRemoved = 0;

    /** Number of widgets reused in place rather than reconstructed (patch mode only). */
    int32 NodesReused = 0;

    /** Echo of the mode this pass ran in, so the response is never ambiguous. */
    EUISpecBuildMode Mode = EUISpecBuildMode::Rebuild;

    /**
     * True when this pass tore an existing widget tree down (rebuild mode over
     * a pre-existing WBP). The counter triple `created:N, modified:0, removed:N`
     * reads like bookkeeping; this flag is the unambiguous signal.
     */
    bool bTeardown = false;

    /**
     * Pre-flight audit of everything a teardown was about to reset. Populated
     * whenever a pre-existing WBP is rebuilt (including dry-run) and left empty
     * for create-new and patch passes. Its findings are also mirrored into
     * `Validation.Warnings` so `warning_count` reflects them.
     */
    FUISpecLossAuditResult LossAudit;

    /** Free-form diff lines. Always populated; richer when bDryRun=true. */
    TArray<FString> DiffLines;
};

/**
 * Stateless builder. Static methods only — no per-instance state, so concurrent
 * builds on different WBPs are safe in principle (in practice main-thread-only
 * because the editor APIs we call into require it).
 */
class MONOLITHUI_API FUISpecBuilder
{
public:
    /**
     * Single entry point. Runs the full pipeline described in the file header.
     *
     * Returns a populated FUISpecBuilderResult — never throws, never crashes
     * on bad input. Atomicity contract (per H2): on any failure between the
     * dry-walk and the final commit, the asset is rolled back to its previous
     * state (existing-WBP path) or removed entirely (create-new path).
     */
    static FUISpecBuilderResult Build(const FUISpecBuilderInputs& Inputs);
};
