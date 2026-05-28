// SPDX-License-Identifier: MIT
// Private declaration for Phase 1 blueprint pilot adapter.
// Pattern: mirrors Phase 0 Plugins/Monolith/Source/MonolithCore/Private/Actions/MonolithBulkFillActions.h.

#pragma once

#include "CoreMinimal.h"

struct FBulkFillSpec;
struct FDryRunReport;
struct FSchemaDescriptor;

/**
 * Phase 1 pilot — bulk_fill / describe adapter for target_namespace="blueprint".
 * Self-registers with FMonolithBulkFillRegistry from FMonolithBlueprintModule::StartupModule.
 *
 * Targets Blueprint CDOs (BP->GeneratedClass->GetDefaultObject(false)) AND generic
 * UObject assets (DataAsset, DataTable, GameplayEffect, AbilitySet, InputAction, etc.) —
 * the same dual-path the existing FMonolithBlueprintCDOActions::HandleSetCDOProperty
 * supports.
 */
class FMonolithBlueprintBulkFillAdapter
{
public:
	/** Register the adapter pair with FMonolithBulkFillRegistry under namespace "blueprint". */
	static void Register();

	/** Unregister (called from FMonolithBlueprintModule::ShutdownModule). */
	static void Unregister();

	/** Internal: bulk_fill.apply handler for target_namespace="blueprint". */
	static FDryRunReport BlueprintBulkFill(const FBulkFillSpec& Spec);

	/** Internal: describe.schema handler for target_namespace="blueprint". */
	static FSchemaDescriptor BlueprintDescribe(const FString& TargetAsset);
};
