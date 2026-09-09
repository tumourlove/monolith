#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Chooser table authoring actions for Monolith — new `chooser` namespace,
 * registered from within the MonolithAnimation module (no new module).
 *
 * 6 deep-inspection and mutation actions: inspect_chooser, duplicate_chooser_tree,
 * set_context_object_class, set_result_asset_reference,
 * set_evaluate_chooser_result_reference, validate_chooser. Note that validate_chooser
 * runs a COMPILE pass; the strictly read-only structural preflight is the separate
 * validate_chooser_table owned by FMonolithChooserReadActions, which also owns the
 * bounded reflection-only discovery / readback surface.
 *
 * Operates on UChooserTable assets (Chooser plugin). All handlers are gated behind
 * WITH_CHOOSER; when the Chooser plugin is absent the off-gate stub returns a clean
 * "Chooser plugin not available" error rather than failing to link.
 */
class MONOLITHANIMATION_API FMonolithChooserActions
{
public:
	/** Register all chooser actions with the tool registry. Always registers; gating is per-handler. */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleInspectChooser(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDuplicateChooserTree(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetContextObjectClass(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetResultAssetReference(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetEvaluateChooserResultReference(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleValidateChooser(const TSharedPtr<FJsonObject>& Params);
};
