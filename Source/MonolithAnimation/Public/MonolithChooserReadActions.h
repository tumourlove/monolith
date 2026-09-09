#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Bounded, reflection-only Chooser discovery, readback and structural validation —
 * registered under the existing `chooser` namespace alongside FMonolithChooserActions
 * (deep inspection / remap) and FMonolithChooserAuthoringActions (create / grow).
 *
 * 6 actions: list_chooser_tables, get_chooser_table, list_chooser_columns,
 * list_chooser_rows, list_chooser_references, validate_chooser_table.
 *
 * These handlers NEVER transact, compile, save, mutate or dirty a package. That is
 * the whole point of the surface: `validate_chooser_table` is deliberately DISTINCT
 * from `chooser:validate_chooser`, which runs a compile-oriented pass. Do not merge
 * the two.
 *
 * Unlike the sibling chooser files these bodies are NOT gated on WITH_CHOOSER: every
 * read goes through UE reflection (FindFProperty / FScriptArrayHelper / FInstancedStruct),
 * so the same code compiles and runs whether or not the optional Chooser plugin is
 * linked. When the plugin is absent /Script/Chooser.ChooserTable does not resolve and
 * the asset-backed handlers return a clean ErrOptionalDepUnavailable; registry-only
 * discovery still answers with metadata rather than synthesized data.
 *
 * Every response is finite. Each traversal has an explicit bound (tables, rows,
 * columns, references, depth, fields, container elements, string length, global
 * property visits) and every cutoff is reported through a truncation or completeness
 * field, so an incomplete answer is never indistinguishable from a complete one.
 */
class MONOLITHANIMATION_API FMonolithChooserReadActions
{
public:
	/** Register all chooser read actions with the tool registry. Always registers. */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleListChooserTables(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetChooserTable(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListChooserColumns(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListChooserRows(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListChooserReferences(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleValidateChooserTable(const TSharedPtr<FJsonObject>& Params);
};
