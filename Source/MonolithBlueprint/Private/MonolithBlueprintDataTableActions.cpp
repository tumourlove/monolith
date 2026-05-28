// SPDX-License-Identifier: MIT
// DataTable dataset-ergonomics actions (Part B) — read/schema/bulk-write/CRUD/round-trip.
//
// Engine-generic: row structs resolve by string; schema + per-field type handling
// delegate to FMonolithReflectionWalker (MonolithCore). Writes reuse the
// FDryRunReport / FBulkFillFieldWrite reporting shape so callers get the same
// {path,current,proposed,ok,reason} surface as bulk_fill.apply. Game-thread only.

#include "MonolithBlueprintDataTableActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "Reflection/MonolithDryRunGuard.h"
#include "Engine/DataTable.h"
#include "DataTableUtils.h"
#include "DataTableEditorUtils.h"
#include "EditorAssetLibrary.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"

namespace MonolithDataTableInternal
{
	// --- Friendly property-name handling -----------------------------------
	// UserDefinedStruct properties carry GUID-suffixed internal names
	// (e.g. "Damage_2_C392053F..."); callers use the friendly DisplayName
	// ("Damage"). The existing add_data_table_row resolver (StructActions.cpp)
	// matches exact / case-insensitive internal / DisplayName / GUID-stripped.
	// Mirror that here so set_data_table_rows accepts the same friendly keys.

	static FString FriendlyPropertyName(const FProperty* Prop)
	{
		const FString DisplayName = Prop->GetMetaData(TEXT("DisplayName"));
		if (!DisplayName.IsEmpty()) { return DisplayName; }
		FString Name = Prop->GetName();
		int32 FirstUnderscore;
		if (Name.FindChar(TEXT('_'), FirstUnderscore))
		{
			if (FirstUnderscore + 1 < Name.Len() && FChar::IsDigit(Name[FirstUnderscore + 1]))
			{
				return Name.Left(FirstUnderscore);
			}
		}
		return Name;
	}

	// Resolve a caller-supplied (possibly friendly) field name to its FProperty.
	static FProperty* ResolveRowProperty(const UScriptStruct* RowStruct, const FString& FieldName)
	{
		if (FProperty* Exact = RowStruct->FindPropertyByName(FName(*FieldName)))
		{
			return Exact;
		}
		for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
		{
			if (It->GetName().Equals(FieldName, ESearchCase::IgnoreCase))
			{
				return *It;
			}
			const FString DisplayName = It->GetMetaData(TEXT("DisplayName"));
			if (!DisplayName.IsEmpty() && DisplayName.Equals(FieldName, ESearchCase::IgnoreCase))
			{
				return *It;
			}
			FString PropName = It->GetName();
			int32 UnderscoreIdx;
			if (PropName.FindChar(TEXT('_'), UnderscoreIdx))
			{
				if (PropName.Left(UnderscoreIdx).Equals(FieldName, ESearchCase::IgnoreCase))
				{
					return *It;
				}
			}
		}
		return nullptr;
	}

	// Build a canonical-keyed tree (internal property names) from a friendly-keyed
	// values object, so FMonolithReflectionWalker (which matches on internal names)
	// resolves UDS columns. Unknown fields are passed through unchanged so the
	// walker reports them as "unknown field" in strict mode.
	static TSharedPtr<FJsonObject> ToCanonicalTree(const UScriptStruct* RowStruct, const TSharedPtr<FJsonObject>& FriendlyValues)
	{
		TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
		for (const auto& Pair : FriendlyValues->Values)
		{
			if (FProperty* Prop = ResolveRowProperty(RowStruct, Pair.Key))
			{
				Tree->SetField(Prop->GetName(), Pair.Value);
			}
			else
			{
				Tree->SetField(Pair.Key, Pair.Value);
			}
		}
		return Tree;
	}

	// Serialise a row to friendly-named {field: stringified-value}.
	static TSharedPtr<FJsonObject> SerializeRow(const UScriptStruct* RowStruct, const uint8* RowData)
	{
		TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
		{
			FProperty* Prop = *It;
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);
			FString ValueStr;
			Prop->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, nullptr, PPF_None);
			Values->SetStringField(FriendlyPropertyName(Prop), ValueStr);
		}
		return Values;
	}

	// Build the inline schema array for a row struct via the shared reflection
	// walker, serialised in the same shape as bulk_fill's DescriptorToJson.
	static TSharedPtr<FJsonObject> DescriptorToJson(const FSchemaDescriptor& Desc)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("field_path"), Desc.FieldPath);
		O->SetStringField(TEXT("type_name"), Desc.TypeName);
		O->SetStringField(TEXT("import_text_form"), Desc.ImportTextForm);
		O->SetBoolField(TEXT("required"), Desc.bRequired);
		O->SetBoolField(TEXT("set_once"), Desc.bSetOnce);
		O->SetNumberField(TEXT("range_min"), Desc.RangeMin);
		O->SetNumberField(TEXT("range_max"), Desc.RangeMax);
		if (Desc.EnumValues.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Vals;
			for (const FString& E : Desc.EnumValues) { Vals.Add(MakeShared<FJsonValueString>(E)); }
			O->SetArrayField(TEXT("enum_values"), Vals);
		}
		if (Desc.Children.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Kids;
			for (const FSchemaDescriptor& C : Desc.Children)
			{
				Kids.Add(MakeShared<FJsonValueObject>(DescriptorToJson(C)));
			}
			O->SetArrayField(TEXT("children"), Kids);
		}
		return O;
	}

	// Build the top-level schema array (the row struct's immediate fields).
	static TArray<TSharedPtr<FJsonValue>> BuildSchemaArray(UScriptStruct* RowStruct)
	{
		TArray<TSharedPtr<FJsonValue>> SchemaArr;
		const FSchemaDescriptor Root = FMonolithReflectionWalker::DescribeStruct(RowStruct);
		for (const FSchemaDescriptor& Child : Root.Children)
		{
			SchemaArr.Add(MakeShared<FJsonValueObject>(DescriptorToJson(Child)));
		}
		return SchemaArr;
	}

	// Resolve a DataTable asset by string with a RowStruct guard.
	static UDataTable* ResolveDataTable(const FString& AssetPath, const UScriptStruct*& OutRowStruct, FString& OutError)
	{
		UDataTable* DataTable = FMonolithAssetUtils::LoadAssetByPath<UDataTable>(AssetPath);
		if (!DataTable)
		{
			OutError = FString::Printf(TEXT("DataTable not found: %s"), *AssetPath);
			return nullptr;
		}
		OutRowStruct = DataTable->GetRowStruct();
		if (!OutRowStruct)
		{
			OutError = FString::Printf(TEXT("DataTable '%s' has no RowStruct set"), *AssetPath);
			return nullptr;
		}
		return DataTable;
	}

#if WITH_EDITOR
	// Compose EDataTableExportFlags from the two boolean params (editor-only export path).
	static EDataTableExportFlags BuildExportFlags(bool bUseJsonObjects, bool bSimpleText)
	{
		EDataTableExportFlags Flags = EDataTableExportFlags::None;
		if (bUseJsonObjects) { Flags |= EDataTableExportFlags::UseJsonObjectsForStructs; }
		if (bSimpleText)     { Flags |= EDataTableExportFlags::UseSimpleText; }
		return Flags;
	}
#endif
}

// ============================================================
//  Registration
// ============================================================

void FMonolithBlueprintDataTableActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("read_data_table"),
		TEXT("Read a DataTable's full contents plus its inline row schema. Returns the row struct, total_rows, an FSchemaDescriptor-shaped 'schema' array (field type_name, import_text_form, enum_values, range, nested children), and a 'rows' array of {row_name, values}. Supersedes get_data_table_rows by inlining schema with data."),
		FMonolithActionHandler::CreateStatic(&HandleReadDataTable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"),     TEXT("string"),  TEXT("DataTable asset path, e.g. /Game/Data/DT_Weapons"))
			.Optional(TEXT("include_schema"), TEXT("boolean"), TEXT("Include the inline row-field schema array (default true)."), TEXT("true"))
			.Optional(TEXT("row_name"),       TEXT("string"),  TEXT("If provided, return only this row. Otherwise return all rows."))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("describe_data_table_schema"),
		TEXT("Return ONLY a DataTable's row schema (no row data). Useful for planning edits to a large table. Returns row_struct, row_struct_path, and an FSchemaDescriptor-shaped 'schema' array."),
		FMonolithActionHandler::CreateStatic(&HandleDescribeDataTableSchema),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("DataTable asset path, e.g. /Game/Data/DT_Weapons"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_data_table_rows"),
		TEXT("Bulk add/update DataTable rows in one call. Each row is {row_name, values:{field:value}, mode?}. Mode is upsert (default), add, or update. Supports dry_run (validate only) and strict (promote coercion/unknown-field/enum-miss to hard errors). Returns an FDryRunReport-shaped per-field result {path,current,proposed,ok,reason}. Fires one editor-refresh broadcast at the end."),
		FMonolithActionHandler::CreateStatic(&HandleSetDataTableRows),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"),  TEXT("DataTable asset path, e.g. /Game/Data/DT_Weapons"))
			.Required(TEXT("rows"),       TEXT("array"),   TEXT("Array of {row_name, values:{field:value}, mode?:\"upsert\"|\"add\"|\"update\"}. Default mode upsert."))
			.Optional(TEXT("dry_run"),    TEXT("boolean"), TEXT("If true, validate only — emit would-be writes but do not persist."), TEXT("false"))
			.Optional(TEXT("strict"),     TEXT("boolean"), TEXT("If true, promote silent drops / unknown fields / enum misses to hard errors."), TEXT("false"))
			.Optional(TEXT("save"),       TEXT("boolean"), TEXT("If true, save the package after applying."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("remove_data_table_row"),
		TEXT("Remove a single row from a DataTable. Refreshes any open DataTable editor."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveDataTableRow),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"),  TEXT("DataTable asset path"))
			.Required(TEXT("row_name"),   TEXT("string"),  TEXT("Row name / key to remove"))
			.Optional(TEXT("save"),       TEXT("boolean"), TEXT("If true, save the package after removing."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("rename_data_table_row"),
		TEXT("Rename a single DataTable row. Refreshes any open DataTable editor."),
		FMonolithActionHandler::CreateStatic(&HandleRenameDataTableRow),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"),  TEXT("DataTable asset path"))
			.Required(TEXT("old_name"),   TEXT("string"),  TEXT("Existing row name"))
			.Required(TEXT("new_name"),   TEXT("string"),  TEXT("New row name"))
			.Optional(TEXT("save"),       TEXT("boolean"), TEXT("If true, save the package after renaming."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("duplicate_data_table_row"),
		TEXT("Duplicate a DataTable row under a new name. Refreshes any open DataTable editor."),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateDataTableRow),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"),  TEXT("DataTable asset path"))
			.Required(TEXT("source_row"), TEXT("string"),  TEXT("Existing row to copy"))
			.Required(TEXT("new_name"),   TEXT("string"),  TEXT("Name for the duplicated row"))
			.Optional(TEXT("save"),       TEXT("boolean"), TEXT("If true, save the package after duplicating."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("export_data_table"),
		TEXT("Export an entire DataTable as a JSON or CSV text blob for token-efficient round-trip editing. Returns row_struct, row_struct_path, total_rows, format, and 'text'. By default nested structs export as clean JSON objects (use_json_objects)."),
		FMonolithActionHandler::CreateStatic(&HandleExportDataTable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"),       TEXT("string"),  TEXT("DataTable asset path"))
			.Optional(TEXT("format"),           TEXT("string"),  TEXT("\"json\" (default) or \"csv\"."), TEXT("json"))
			.Optional(TEXT("use_json_objects"), TEXT("boolean"), TEXT("Export nested structs as JSON objects rather than ExportText blobs (JSON only). Default true."), TEXT("true"))
			.Optional(TEXT("simple_text"),      TEXT("boolean"), TEXT("Export text properties as display strings rather than lossless form. Default false."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("import_data_table"),
		TEXT("Import a JSON or CSV text blob into a DataTable. REPLACES the entire row set (rows not present in the blob are deleted by design). The DataTable must already have a RowStruct set. mode must be \"replace\". Refreshes any open DataTable editor."),
		FMonolithActionHandler::CreateStatic(&HandleImportDataTable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"),  TEXT("DataTable asset path"))
			.Required(TEXT("text"),       TEXT("string"),  TEXT("The JSON or CSV blob to import. REPLACES all existing rows."))
			.Optional(TEXT("format"),     TEXT("string"),  TEXT("\"json\" (default) or \"csv\"."), TEXT("json"))
			.Optional(TEXT("mode"),       TEXT("string"),  TEXT("Only \"replace\" is supported (import wipes existing rows first). Must be passed explicitly."), TEXT("replace"))
			.Optional(TEXT("save"),       TEXT("boolean"), TEXT("If true, save the package after importing."), TEXT("false"))
			.Build());
}

// ============================================================
//  read_data_table  (keystone)
// ============================================================

FMonolithActionResult FMonolithBlueprintDataTableActions::HandleReadDataTable(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithDataTableInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	const UScriptStruct* RowStructConst = nullptr;
	FString Error;
	UDataTable* DataTable = ResolveDataTable(AssetPath, RowStructConst, Error);
	if (!DataTable)
	{
		return FMonolithActionResult::Error(Error);
	}
	UScriptStruct* RowStruct = const_cast<UScriptStruct*>(RowStructConst);

	bool bIncludeSchema = true;
	Params->TryGetBoolField(TEXT("include_schema"), bIncludeSchema);

	FString RowNameFilter;
	Params->TryGetStringField(TEXT("row_name"), RowNameFilter);

	const TMap<FName, uint8*>& RowMap = DataTable->GetRowMap();

	TArray<TSharedPtr<FJsonValue>> RowResults;
	if (!RowNameFilter.IsEmpty())
	{
		const FName RowFName(*RowNameFilter);
		const uint8* const* FoundRow = RowMap.Find(RowFName);
		if (!FoundRow || !(*FoundRow))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Row '%s' not found in DataTable '%s'"), *RowNameFilter, *AssetPath));
		}
		TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
		RowObj->SetStringField(TEXT("row_name"), RowNameFilter);
		RowObj->SetObjectField(TEXT("values"), SerializeRow(RowStruct, *FoundRow));
		RowResults.Add(MakeShared<FJsonValueObject>(RowObj));
	}
	else
	{
		for (const auto& Pair : RowMap)
		{
			TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
			RowObj->SetStringField(TEXT("row_name"), Pair.Key.ToString());
			RowObj->SetObjectField(TEXT("values"), SerializeRow(RowStruct, Pair.Value));
			RowResults.Add(MakeShared<FJsonValueObject>(RowObj));
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("row_struct"), RowStruct->GetName());
	Root->SetStringField(TEXT("row_struct_path"), RowStruct->GetPathName());
	Root->SetNumberField(TEXT("total_rows"), RowMap.Num());
	if (bIncludeSchema)
	{
		Root->SetArrayField(TEXT("schema"), BuildSchemaArray(RowStruct));
	}
	Root->SetArrayField(TEXT("rows"), RowResults);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  describe_data_table_schema  (schema-only)
// ============================================================

FMonolithActionResult FMonolithBlueprintDataTableActions::HandleDescribeDataTableSchema(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithDataTableInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	const UScriptStruct* RowStructConst = nullptr;
	FString Error;
	UDataTable* DataTable = ResolveDataTable(AssetPath, RowStructConst, Error);
	if (!DataTable)
	{
		return FMonolithActionResult::Error(Error);
	}
	UScriptStruct* RowStruct = const_cast<UScriptStruct*>(RowStructConst);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("row_struct"), RowStruct->GetName());
	Root->SetStringField(TEXT("row_struct_path"), RowStruct->GetPathName());
	Root->SetArrayField(TEXT("schema"), BuildSchemaArray(RowStruct));
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  set_data_table_rows  (bulk upsert/add/update)
// ============================================================

FMonolithActionResult FMonolithBlueprintDataTableActions::HandleSetDataTableRows(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithDataTableInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	const TArray<TSharedPtr<FJsonValue>>* RowsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("rows"), RowsArray) || !RowsArray || RowsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: rows (array of {row_name, values, mode?})"));
	}

	const UScriptStruct* RowStructConst = nullptr;
	FString Error;
	UDataTable* DataTable = ResolveDataTable(AssetPath, RowStructConst, Error);
	if (!DataTable)
	{
		return FMonolithActionResult::Error(Error);
	}
	UScriptStruct* RowStruct = const_cast<UScriptStruct*>(RowStructConst);

	FMonolithDryRunGuard Guard(Params);
	const bool bDryRun = Guard.IsDryRun();
	const bool bStrict = Guard.IsStrict();

	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);

	FBulkFillSpec Spec;
	Spec.TargetNamespace = TEXT("blueprint");
	Spec.TargetAsset = AssetPath;
	Spec.bDryRun = bDryRun;
	Spec.bStrict = bStrict;

	// Per-row outcome accumulation. We aggregate field_writes/errors across rows
	// into the response, plus one row-result entry per input row.
	TArray<TSharedPtr<FJsonValue>> RowResults;
	int32 TotalErrors = 0;
	bool bWouldApplyAll = true;
	TArray<FName> ChangedRows;

	// On a real (non-dry) commit, wrap the whole batch in one transaction.
	TUniquePtr<FScopedTransaction> Transaction;
	if (!bDryRun)
	{
		DataTable->SetFlags(RF_Transactional);
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT(
			"MonolithBlueprintDataTableActions", "SetDataTableRows", "Monolith Set DataTable Rows"));
		DataTable->Modify();
	}

	for (const TSharedPtr<FJsonValue>& RowVal : *RowsArray)
	{
		const TSharedPtr<FJsonObject>* RowObjPtr = nullptr;
		if (!RowVal.IsValid() || !RowVal->TryGetObject(RowObjPtr) || !RowObjPtr || !(*RowObjPtr).IsValid())
		{
			TotalErrors++;
			bWouldApplyAll = false;
			TSharedPtr<FJsonObject> Bad = MakeShared<FJsonObject>();
			Bad->SetStringField(TEXT("row_name"), TEXT(""));
			Bad->SetStringField(TEXT("error"), TEXT("row entry is not a JSON object"));
			RowResults.Add(MakeShared<FJsonValueObject>(Bad));
			continue;
		}
		const TSharedPtr<FJsonObject>& RowObj = *RowObjPtr;

		const FString RowName = RowObj->GetStringField(TEXT("row_name"));
		if (RowName.IsEmpty())
		{
			TotalErrors++;
			bWouldApplyAll = false;
			TSharedPtr<FJsonObject> Bad = MakeShared<FJsonObject>();
			Bad->SetStringField(TEXT("error"), TEXT("row entry missing row_name"));
			RowResults.Add(MakeShared<FJsonValueObject>(Bad));
			continue;
		}

		FString Mode = TEXT("upsert");
		RowObj->TryGetStringField(TEXT("mode"), Mode);
		Mode = Mode.ToLower();

		const TSharedPtr<FJsonObject>* ValuesObjPtr = nullptr;
		TSharedPtr<FJsonObject> ValuesObj = MakeShared<FJsonObject>();
		if (RowObj->TryGetObjectField(TEXT("values"), ValuesObjPtr) && ValuesObjPtr && (*ValuesObjPtr).IsValid())
		{
			ValuesObj = *ValuesObjPtr;
		}

		const FName RowFName(*RowName);
		const bool bExists = DataTable->GetRowMap().Contains(RowFName);

		// Mode gating.
		if (Mode == TEXT("add") && bExists)
		{
			TotalErrors++;
			bWouldApplyAll = false;
			TSharedPtr<FJsonObject> Bad = MakeShared<FJsonObject>();
			Bad->SetStringField(TEXT("row_name"), RowName);
			Bad->SetStringField(TEXT("error"), TEXT("mode 'add' but row already exists"));
			RowResults.Add(MakeShared<FJsonValueObject>(Bad));
			continue;
		}
		if (Mode == TEXT("update") && !bExists)
		{
			TotalErrors++;
			bWouldApplyAll = false;
			TSharedPtr<FJsonObject> Bad = MakeShared<FJsonObject>();
			Bad->SetStringField(TEXT("row_name"), RowName);
			Bad->SetStringField(TEXT("error"), TEXT("mode 'update' but row does not exist"));
			RowResults.Add(MakeShared<FJsonValueObject>(Bad));
			continue;
		}

		// Canonical-keyed tree so the walker resolves UDS field names.
		TSharedPtr<FJsonObject> CanonicalTree = ToCanonicalTree(RowStruct, ValuesObj);

		// Allocate a working row buffer. For updates seed it from the existing row
		// so unspecified fields keep their value; for adds start from defaults.
		const int32 StructSize = RowStruct->GetStructureSize();
		uint8* RowData = static_cast<uint8*>(FMemory::Malloc(StructSize, RowStruct->GetMinAlignment()));
		RowStruct->InitializeStruct(RowData);
		if (bExists)
		{
			const uint8* const* Existing = DataTable->GetRowMap().Find(RowFName);
			if (Existing && *Existing)
			{
				RowStruct->CopyScriptStruct(RowData, *Existing);
			}
		}

		// Walk the tree. Dry-run inspects a scratch buffer; commit writes RowData.
		FDryRunReport RowReport = bDryRun
			? FMonolithReflectionWalker::InspectTree(CanonicalTree, RowStruct, RowData, Spec)
			: FMonolithReflectionWalker::WriteTree(CanonicalTree, RowStruct, RowData, nullptr, Spec);

		TotalErrors += RowReport.Errors;
		const bool bRowOk = RowReport.bWouldApply || (bDryRun && RowReport.Errors == 0);
		if (RowReport.Errors > 0 && bStrict)
		{
			bWouldApplyAll = false;
		}

		// On a non-dry commit, persist the row only if it passed (strict honoured
		// by WriteTree's bWouldApply). Otherwise free the buffer without writing.
		if (!bDryRun && RowReport.bWouldApply)
		{
			// uint8*/UScriptStruct overload copies internally and is safe for any
			// row struct (including UserDefinedStructs that do not derive FTableRowBase).
			DataTable->AddRow(RowFName, RowData, RowStruct);
			ChangedRows.Add(RowFName);
		}

		RowStruct->DestroyStruct(RowData);
		FMemory::Free(RowData);

		// Build the per-row result block.
		TSharedPtr<FJsonObject> RowResult = MakeShared<FJsonObject>();
		RowResult->SetStringField(TEXT("row_name"), RowName);
		RowResult->SetBoolField(TEXT("created"), !bExists);
		RowResult->SetBoolField(TEXT("modified"), bExists);
		TArray<TSharedPtr<FJsonValue>> FieldWrites;
		for (const FBulkFillFieldWrite& W : RowReport.FieldWrites)
		{
			TSharedPtr<FJsonObject> FW = MakeShared<FJsonObject>();
			FW->SetStringField(TEXT("path"), W.Path);
			FW->SetStringField(TEXT("current"), W.CurrentValue);
			FW->SetStringField(TEXT("proposed"), W.ProposedValue);
			FW->SetBoolField(TEXT("ok"), W.bOk);
			if (!W.bOk) { FW->SetStringField(TEXT("reason"), W.Reason); }
			FieldWrites.Add(MakeShared<FJsonValueObject>(FW));
		}
		RowResult->SetArrayField(TEXT("field_writes"), FieldWrites);
		RowResult->SetNumberField(TEXT("errors"), RowReport.Errors);
		RowResults.Add(MakeShared<FJsonValueObject>(RowResult));

		// Permissive mode: a per-row coercion miss does not abort the batch; the
		// would_apply summary below still reflects that not everything was clean.
		(void)bRowOk;
	}

	// Strict + any error → roll back the whole batch (mirror bulk_fill adapter).
	if (!bDryRun)
	{
		if (bStrict && TotalErrors > 0)
		{
			Transaction->Cancel();
			bWouldApplyAll = false;
		}
		else
		{
			// One editor-refresh broadcast for the whole batch.
			FDataTableEditorUtils::BroadcastPostChange(DataTable, FDataTableEditorUtils::EDataTableChangeInfo::RowList);
			DataTable->MarkPackageDirty();
			bWouldApplyAll = (TotalErrors == 0) || !bStrict;
		}
		Transaction.Reset();
	}
	else
	{
		bWouldApplyAll = (TotalErrors == 0);
	}

	bool bSaved = false;
	if (!bDryRun && bSave && bWouldApplyAll && ChangedRows.Num() > 0)
	{
		bSaved = UEditorAssetLibrary::SaveLoadedAsset(DataTable, false);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetArrayField(TEXT("rows"), RowResults);
	Root->SetNumberField(TEXT("errors"), TotalErrors);
	Root->SetBoolField(TEXT("would_apply"), bDryRun ? (TotalErrors == 0) : bWouldApplyAll);
	Root->SetBoolField(TEXT("dry_run"), bDryRun);
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  remove_data_table_row
// ============================================================

FMonolithActionResult FMonolithBlueprintDataTableActions::HandleRemoveDataTableRow(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithDataTableInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString RowName = Params->GetStringField(TEXT("row_name"));
	if (AssetPath.IsEmpty()) { return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path")); }
	if (RowName.IsEmpty())   { return FMonolithActionResult::Error(TEXT("Missing required parameter: row_name")); }

	const UScriptStruct* RowStruct = nullptr;
	FString Error;
	UDataTable* DataTable = ResolveDataTable(AssetPath, RowStruct, Error);
	if (!DataTable) { return FMonolithActionResult::Error(Error); }

	// FDataTableEditorUtils::RemoveRow broadcasts RowList internally.
	const bool bRemoved = FDataTableEditorUtils::RemoveRow(DataTable, FName(*RowName));
	if (!bRemoved)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to remove row '%s' from '%s' (row not found?)"), *RowName, *AssetPath));
	}

	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	bool bSaved = bSave ? UEditorAssetLibrary::SaveLoadedAsset(DataTable, false) : false;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("row_name"), RowName);
	Root->SetBoolField(TEXT("removed"), true);
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  rename_data_table_row
// ============================================================

FMonolithActionResult FMonolithBlueprintDataTableActions::HandleRenameDataTableRow(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithDataTableInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString OldName = Params->GetStringField(TEXT("old_name"));
	const FString NewName = Params->GetStringField(TEXT("new_name"));
	if (AssetPath.IsEmpty()) { return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path")); }
	if (OldName.IsEmpty())   { return FMonolithActionResult::Error(TEXT("Missing required parameter: old_name")); }
	if (NewName.IsEmpty())   { return FMonolithActionResult::Error(TEXT("Missing required parameter: new_name")); }

	const UScriptStruct* RowStruct = nullptr;
	FString Error;
	UDataTable* DataTable = ResolveDataTable(AssetPath, RowStruct, Error);
	if (!DataTable) { return FMonolithActionResult::Error(Error); }

	// FDataTableEditorUtils::RenameRow broadcasts RowList internally.
	const bool bRenamed = FDataTableEditorUtils::RenameRow(DataTable, FName(*OldName), FName(*NewName));
	if (!bRenamed)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to rename row '%s' -> '%s' in '%s' (source missing or target name in use?)"),
			*OldName, *NewName, *AssetPath));
	}

	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	bool bSaved = bSave ? UEditorAssetLibrary::SaveLoadedAsset(DataTable, false) : false;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("old_name"), OldName);
	Root->SetStringField(TEXT("new_name"), NewName);
	Root->SetBoolField(TEXT("renamed"), true);
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  duplicate_data_table_row
// ============================================================

FMonolithActionResult FMonolithBlueprintDataTableActions::HandleDuplicateDataTableRow(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithDataTableInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString SourceRow = Params->GetStringField(TEXT("source_row"));
	const FString NewName = Params->GetStringField(TEXT("new_name"));
	if (AssetPath.IsEmpty())  { return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path")); }
	if (SourceRow.IsEmpty())  { return FMonolithActionResult::Error(TEXT("Missing required parameter: source_row")); }
	if (NewName.IsEmpty())    { return FMonolithActionResult::Error(TEXT("Missing required parameter: new_name")); }

	const UScriptStruct* RowStruct = nullptr;
	FString Error;
	UDataTable* DataTable = ResolveDataTable(AssetPath, RowStruct, Error);
	if (!DataTable) { return FMonolithActionResult::Error(Error); }

	// FDataTableEditorUtils::DuplicateRow broadcasts RowList internally; returns
	// the new row's data pointer (nullptr on failure).
	const uint8* NewRowData = FDataTableEditorUtils::DuplicateRow(DataTable, FName(*SourceRow), FName(*NewName));
	if (!NewRowData)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to duplicate row '%s' -> '%s' in '%s' (source missing or target name in use?)"),
			*SourceRow, *NewName, *AssetPath));
	}

	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	bool bSaved = bSave ? UEditorAssetLibrary::SaveLoadedAsset(DataTable, false) : false;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("source_row"), SourceRow);
	Root->SetStringField(TEXT("row_name"), NewName);
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  export_data_table
// ============================================================

FMonolithActionResult FMonolithBlueprintDataTableActions::HandleExportDataTable(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	using namespace MonolithDataTableInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	const UScriptStruct* RowStructConst = nullptr;
	FString Error;
	UDataTable* DataTable = ResolveDataTable(AssetPath, RowStructConst, Error);
	if (!DataTable) { return FMonolithActionResult::Error(Error); }

	FString Format = TEXT("json");
	Params->TryGetStringField(TEXT("format"), Format);
	Format = Format.ToLower();
	if (Format != TEXT("json") && Format != TEXT("csv"))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unsupported format '%s'. Use \"json\" or \"csv\"."), *Format));
	}

	bool bUseJsonObjects = true;
	bool bSimpleText = false;
	Params->TryGetBoolField(TEXT("use_json_objects"), bUseJsonObjects);
	Params->TryGetBoolField(TEXT("simple_text"), bSimpleText);

	const EDataTableExportFlags Flags = BuildExportFlags(bUseJsonObjects, bSimpleText);
	const FString Text = (Format == TEXT("csv"))
		? DataTable->GetTableAsCSV(Flags)
		: DataTable->GetTableAsJSON(Flags);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("row_struct"), RowStructConst->GetName());
	Root->SetStringField(TEXT("row_struct_path"), RowStructConst->GetPathName());
	Root->SetNumberField(TEXT("total_rows"), DataTable->GetRowMap().Num());
	Root->SetStringField(TEXT("format"), Format);
	Root->SetStringField(TEXT("text"), Text);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
#else
	return FMonolithActionResult::Error(TEXT("export_data_table requires an editor build (GetTableAsJSON/CSV are WITH_EDITOR-only)."));
#endif
}

// ============================================================
//  import_data_table  (REPLACE semantics)
// ============================================================

FMonolithActionResult FMonolithBlueprintDataTableActions::HandleImportDataTable(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithDataTableInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString Text = Params->GetStringField(TEXT("text"));
	if (AssetPath.IsEmpty()) { return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path")); }
	if (Text.IsEmpty())      { return FMonolithActionResult::Error(TEXT("Missing required parameter: text")); }

	UDataTable* DataTable = FMonolithAssetUtils::LoadAssetByPath<UDataTable>(AssetPath);
	if (!DataTable)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("DataTable not found: %s"), *AssetPath));
	}

	// CreateTableFrom*String require RowStruct preset; guard with a clear message.
	if (!DataTable->GetRowStruct())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("DataTable '%s' has no RowStruct set — import requires the row struct to exist first (use create_data_table)."),
			*AssetPath));
	}

	FString Format = TEXT("json");
	Params->TryGetStringField(TEXT("format"), Format);
	Format = Format.ToLower();
	if (Format != TEXT("json") && Format != TEXT("csv"))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unsupported format '%s'. Use \"json\" or \"csv\"."), *Format));
	}

	FString Mode = TEXT("replace");
	Params->TryGetStringField(TEXT("mode"), Mode);
	Mode = Mode.ToLower();
	if (Mode != TEXT("replace"))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unsupported mode '%s'. Only \"replace\" is supported — import REPLACES the entire row set (rows absent from the blob are deleted)."),
			*Mode));
	}

	DataTable->SetFlags(RF_Transactional);
	FScopedTransaction Transaction(NSLOCTEXT(
		"MonolithBlueprintDataTableActions", "ImportDataTable", "Monolith Import DataTable"));
	DataTable->Modify();

	// Both CreateTableFrom*String wipe existing rows then populate; return a
	// problems list (empty == clean import).
	const TArray<FString> Problems = (Format == TEXT("csv"))
		? DataTable->CreateTableFromCSVString(Text)
		: DataTable->CreateTableFromJSONString(Text);

	FDataTableEditorUtils::BroadcastPostChange(DataTable, FDataTableEditorUtils::EDataTableChangeInfo::RowList);
	DataTable->MarkPackageDirty();

	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	bool bSaved = bSave ? UEditorAssetLibrary::SaveLoadedAsset(DataTable, false) : false;

	TArray<TSharedPtr<FJsonValue>> ProblemArr;
	for (const FString& P : Problems) { ProblemArr.Add(MakeShared<FJsonValueString>(P)); }

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("rows_written"), DataTable->GetRowMap().Num());
	Root->SetArrayField(TEXT("problems"), ProblemArr);
	Root->SetBoolField(TEXT("replaced"), true);
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), Problems.Num() == 0);
	return FMonolithActionResult::Success(Root);
}
