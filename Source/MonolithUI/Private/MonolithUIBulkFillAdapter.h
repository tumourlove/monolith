// SPDX-License-Identifier: MIT
// Private declaration for Phase 4 ui adapter.
// Pattern: mirrors Phase 1 Plugins/Monolith/Source/MonolithBlueprint/Private/MonolithBlueprintBulkFillAdapter.h,
// Phase 2 Plugins/Monolith/Source/MonolithGAS/Private/MonolithGASBulkFillAdapter.h,
// and the per-namespace adapters in the other Monolith modules.
//
// H5 stub-adapter invariant: Register() ALWAYS runs from FMonolithUIModule::StartupModule,
// regardless of WITH_COMMONUI. The adapter body splits per call-shape, NOT around the
// register call — the discover surface stays identical across dev + release builds.
//
// M5 invariant (post-review fix): The split happens INSIDE the adapter:
//   * VANILLA UMG PATHS (gate-free): UDataTable row writers (any FTableRowBase),
//     slot-prop describe (UPanelSlot.Parent walk), widget-property describe over
//     UWidget descendants. These run regardless of WITH_COMMONUI.
//   * COMMONUI PATHS (#if WITH_COMMONUI gated): FCommonInputActionDataBase row
//     writes (the 40-row input-action DT), activatable widget stacks, common
//     buttons, CommonInput key bindings. The `#else` branch returns a clean
//     "CommonUI not available — WITH_COMMONUI=0 in this build" error.
//
// SINGLE-TRANSACTION INVARIANT (2026-04-25 parallel-burst crash mitigation):
// Input-action DT bulk_fill MUST commit via ONE FScopedTransaction wrapping
// ONE Modify() + N AddRow + ONE MarkPackageDirty. Concurrent UI-row-write
// attempts serialise via a process-wide FScopeLock against a static
// FCriticalSection — this is the FIX for the 40-row sequential CRASH that
// surfaced on 2026-04-25 from parallel JSON-RPC burst writes.

#pragma once

#include "CoreMinimal.h"

struct FBulkFillSpec;
struct FDryRunReport;
struct FSchemaDescriptor;

/**
 * Phase 4 — bulk_fill / describe adapter for target_namespace="ui".
 * Self-registers with FMonolithBulkFillRegistry from FMonolithUIModule::StartupModule.
 *
 * Tree shape (per design B.11 / plan §Phase 4):
 *
 *   1. fill_kind=InputActionDataTable (CommonUI — gated WITH_COMMONUI):
 *      {
 *        "fill_kind": "InputActionDataTable",
 *        "rows": {
 *          "Inventory_Open": {
 *            "default_display_name": "Open Inventory",
 *            "keyboard_input_type_info": { "key": "I" },
 *            "gamepad_input_type_info":  { "key": "Gamepad_FaceButton_Top" }
 *          },
 *          ... 40 rows in one call ...
 *        }
 *      }
 *
 *   2. fill_kind=DataTableRows (vanilla UMG — any FTableRowBase struct):
 *      {
 *        "fill_kind": "DataTableRows",
 *        "rows": { "RowName": {field: value, ...}, ... }
 *      }
 *
 *   3. fill_kind=WidgetProperties (vanilla UMG — UUserWidget tree writes):
 *      {
 *        "fill_kind": "WidgetProperties",
 *        "widget_name": "MyTextBlock",
 *        "properties": { "Text": "Hello", "ColorAndOpacity": "#FF0000" }
 *      }
 *
 * Describe (`describe.schema`) accepts either:
 *   - "/Game/UI/WBP_Foo|widget_name=MyButton" → slot-prop describe scoped to
 *     the resolved widget's PARENT panel class (UCanvasPanelSlot vs
 *     UVerticalBoxSlot vs UOverlaySlot vs UHorizontalBoxSlot vs UUniformGridSlot).
 *   - "/Game/UI/WBP_Foo|widget=MyButton|kind=widget" → widget-property describe
 *     over the resolved widget's UClass (per-widget allowlist token).
 *   - "/Game/UI/DT_Foo" or empty → top-level "what fill_kinds does ui support" tree.
 */
class FMonolithUIBulkFillAdapter
{
public:
	/** Register the adapter pair with FMonolithBulkFillRegistry under namespace "ui". */
	static void Register();

	/** Unregister (called from FMonolithUIModule::ShutdownModule). */
	static void Unregister();

	/** Internal: bulk_fill.apply handler for target_namespace="ui". */
	static FDryRunReport UIBulkFill(const FBulkFillSpec& Spec);

	/** Internal: describe.schema handler for target_namespace="ui". */
	static FSchemaDescriptor UIDescribe(const FString& TargetAsset);
};
