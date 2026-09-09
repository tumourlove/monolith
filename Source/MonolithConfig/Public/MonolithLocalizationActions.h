#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Read-only localization preflight for the `localization` namespace.
 *
 * 4 actions: culture discovery plus bounded StringTable discovery, entry
 * readback and validation. Every list these handlers return is capped by a
 * documented bound, and each readback reports separately whether it covered the
 * whole asset.
 *
 * These handlers use internationalization and AssetRegistry read APIs only —
 * they never transact, save, mutate or dirty an inspected package. StringTable
 * AUTHORING stays on the `blueprint` namespace (read_string_table /
 * set_string_table_entries / remove_string_table_entry).
 */
class FMonolithLocalizationActions
{
public:
	/** Register all localization actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// --- Action handlers ---
	static FMonolithActionResult ListCultures(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListStringTables(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetStringTable(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateStringTable(const TSharedPtr<FJsonObject>& Params);
};
