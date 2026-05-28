#pragma once
#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * DataTable dataset-ergonomics actions (Part B).
 *
 * All actions live in the "blueprint" namespace beside the existing
 * create_data_table / add_data_table_row / get_data_table_rows family. Each is
 * engine-generic: row structs resolve by string, and schema/type handling is
 * delegated to FMonolithReflectionWalker (MonolithCore) so this file never
 * reinvents reflection. Game-thread only.
 */
class FMonolithBlueprintDataTableActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// Read + schema
	static FMonolithActionResult HandleReadDataTable(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDescribeDataTableSchema(const TSharedPtr<FJsonObject>& Params);

	// Bulk upsert/add/update (dry_run + strict, FDryRunReport-shaped per field)
	static FMonolithActionResult HandleSetDataTableRows(const TSharedPtr<FJsonObject>& Params);

	// Row CRUD
	static FMonolithActionResult HandleRemoveDataTableRow(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRenameDataTableRow(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDuplicateDataTableRow(const TSharedPtr<FJsonObject>& Params);

	// Whole-table JSON/CSV round-trip
	static FMonolithActionResult HandleExportDataTable(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleImportDataTable(const TSharedPtr<FJsonObject>& Params);
};
