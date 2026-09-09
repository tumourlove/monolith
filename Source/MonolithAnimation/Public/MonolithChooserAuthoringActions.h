#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Chooser table AUTHORING actions for Monolith — registered under the existing
 * `chooser` namespace (alongside FMonolithChooserActions, which owns the deep
 * inspect / remap / compile-validate side, and FMonolithChooserReadActions, which
 * owns the bounded reflection-only discovery / readback / structural-preflight
 * side). These actions create a UChooserTable from scratch and grow it
 * column-by-column / row-by-row, keeping every parallel per-row array aligned.
 *
 * 4 actions: create_chooser_table, add_chooser_column, add_chooser_row,
 * set_chooser_cell.
 *
 * Operates on UChooserTable assets (Chooser plugin). All handlers are gated
 * behind WITH_CHOOSER; when the Chooser plugin is absent the off-gate stub
 * returns a clean "Chooser plugin not available" error rather than failing to
 * link (mirrors MonolithChooserActions.cpp:99-105).
 *
 * The mutated arrays (ResultsStructs / ColumnsStructs / DisabledRows and every
 * column's per-row value array) are WITH_EDITORONLY_DATA on UChooserTable, so
 * the bodies additionally gate on WITH_EDITORONLY_DATA and return an
 * editor-only error off-gate.
 */
class MONOLITHANIMATION_API FMonolithChooserAuthoringActions
{
public:
	/** Register all chooser-authoring actions with the tool registry. Always registers; gating is per-handler. */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleCreateChooserTable(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddChooserColumn(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddChooserRow(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetChooserCell(const TSharedPtr<FJsonObject>& Params);
};
