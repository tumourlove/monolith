#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Enhanced Input authoring: per-key modifiers/triggers on a UInputMappingContext,
 * and the direct trigger/modifier arrays on a UInputAction.
 *
 * These exist because instanced (EditInline) modifiers and triggers cannot be
 * authored correctly from script. Scripted writes that assign the whole mappings
 * array back onto the context drop every instanced subobject: the mapping slots
 * survive with the right count, but the modifiers resolve to null after a reload,
 * with nothing in the editor or the log to say the write was lost.
 *
 * The fix is entirely in construction: an instanced subobject must be outered to
 * the UObject that owns the property (the mapping context, not the input action and
 * not the transient package), with the owner's RF_PropagateToSubObjects flags — the
 * same outer and flags the details panel uses in
 * SPropertyEditorEditInline::OnClassPicked. Only objects whose outer chain lies
 * inside the package being saved are harvested as exports; anything else becomes an
 * import that resolves to null on load.
 *
 * Registered in the same `input` namespace as FMonolithGASInputAssetActions and
 * bounded by the same MonolithInput limits, so authored arrays can never exceed
 * what the read actions return untruncated.
 */
class MONOLITHGAS_API FMonolithGASInputAuthoringActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleAddMappingModifier(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddMappingTrigger(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveMappingModifier(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveMappingTrigger(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetMappingModifiers(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetMappingTriggers(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetInputActionModifiers(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetInputActionTriggers(const TSharedPtr<FJsonObject>& Params);
};
