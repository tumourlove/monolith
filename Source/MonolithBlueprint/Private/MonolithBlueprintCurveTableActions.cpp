// SPDX-License-Identifier: MIT
// CurveTable dataset-ergonomics actions (Part B) — read/set-keys/row-CRUD.
//
// Engine-generic: the table resolves by string. Curve rows are FRealCurve*
// (FRichCurve*/FSimpleCurve*) stored in UCurveTable::RowMap, so key
// (de)serialization is bespoke rather than reflection-walked. A fresh table is
// ECurveTableMode::Empty and the FIRST AddRichCurve/AddSimpleCurve locks
// rich-vs-simple permanently — mode-mismatched writes are rejected. Row
// mutations are bracketed by FCurveTableEditorUtils::BroadcastPre/PostChange
// (the only thing that utility provides). Game-thread only.

#include "MonolithBlueprintCurveTableActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"
#include "Engine/CurveTable.h"
#include "CurveTableEditorUtils.h"
#include "Curves/RichCurve.h"
#include "Curves/SimpleCurve.h"
#include "Curves/RealCurve.h"
#include "EditorAssetLibrary.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

namespace MonolithCurveTableInternal
{
	// --- interp-mode string <-> enum --------------------------------------

	// Parse the caller's interp_mode string. Returns false on an unknown token.
	static bool ParseInterpMode(const FString& InModeStr, ERichCurveInterpMode& OutMode)
	{
		const FString Lower = InModeStr.ToLower();
		if (Lower == TEXT("linear"))   { OutMode = RCIM_Linear;   return true; }
		if (Lower == TEXT("constant")) { OutMode = RCIM_Constant; return true; }
		if (Lower == TEXT("cubic"))    { OutMode = RCIM_Cubic;    return true; }
		return false;
	}

	static FString InterpModeToString(ERichCurveInterpMode InMode)
	{
		switch (InMode)
		{
		case RCIM_Linear:   return TEXT("linear");
		case RCIM_Constant: return TEXT("constant");
		case RCIM_Cubic:    return TEXT("cubic");
		default:            return TEXT("none");
		}
	}

	// cubic interp must use a RichCurve; linear/constant fit a SimpleCurve.
	static bool InterpModeRequiresRich(ERichCurveInterpMode InMode)
	{
		return InMode == RCIM_Cubic;
	}

	static FString CurveTableModeToString(ECurveTableMode InMode)
	{
		switch (InMode)
		{
		case ECurveTableMode::RichCurves:   return TEXT("rich");
		case ECurveTableMode::SimpleCurves: return TEXT("simple");
		default:                            return TEXT("empty");
		}
	}

	// --- resolution -------------------------------------------------------

	static UCurveTable* ResolveCurveTable(const FString& AssetPath, FString& OutError)
	{
		UCurveTable* CurveTable = FMonolithAssetUtils::LoadAssetByPath<UCurveTable>(AssetPath);
		if (!CurveTable)
		{
			OutError = FString::Printf(TEXT("CurveTable not found: %s"), *AssetPath);
			return nullptr;
		}
		return CurveTable;
	}

	// --- key serialization ------------------------------------------------

	// Serialise a rich curve's keys (time/value/interp_mode + cubic tangents).
	static TArray<TSharedPtr<FJsonValue>> SerializeRichKeys(const FRichCurve* Curve)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		if (!Curve) { return Out; }
		for (const FRichCurveKey& Key : Curve->GetConstRefOfKeys())
		{
			TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
			K->SetNumberField(TEXT("time"), Key.Time);
			K->SetNumberField(TEXT("value"), Key.Value);
			K->SetStringField(TEXT("interp_mode"), InterpModeToString(Key.InterpMode.GetValue()));
			K->SetNumberField(TEXT("arrive_tangent"), Key.ArriveTangent);
			K->SetNumberField(TEXT("leave_tangent"), Key.LeaveTangent);
			Out.Add(MakeShared<FJsonValueObject>(K));
		}
		return Out;
	}

	// Serialise a simple curve's keys (time/value; interp mode is curve-wide).
	static TArray<TSharedPtr<FJsonValue>> SerializeSimpleKeys(const FSimpleCurve* Curve)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		if (!Curve) { return Out; }
		for (const FSimpleCurveKey& Key : Curve->GetConstRefOfKeys())
		{
			TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
			K->SetNumberField(TEXT("time"), Key.Time);
			K->SetNumberField(TEXT("value"), Key.Value);
			Out.Add(MakeShared<FJsonValueObject>(K));
		}
		return Out;
	}

	// Build the per-row JSON block for one curve, branching on table mode.
	static TSharedPtr<FJsonObject> SerializeRow(const UCurveTable* CurveTable, FName RowName, const FRealCurve* Curve)
	{
		TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
		RowObj->SetStringField(TEXT("row_name"), RowName.ToString());

		if (CurveTable->GetCurveTableMode() == ECurveTableMode::SimpleCurves)
		{
			const FSimpleCurve* Simple = static_cast<const FSimpleCurve*>(Curve);
			RowObj->SetStringField(TEXT("interp_mode"),
				Simple ? InterpModeToString(Simple->GetKeyInterpMode()) : TEXT("linear"));
			RowObj->SetArrayField(TEXT("keys"), SerializeSimpleKeys(Simple));
		}
		else
		{
			// RichCurves (or Empty rows that were added as rich).
			RowObj->SetArrayField(TEXT("keys"), SerializeRichKeys(static_cast<const FRichCurve*>(Curve)));
		}
		return RowObj;
	}
}

// ============================================================
//  Registration
// ============================================================

void FMonolithBlueprintCurveTableActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("read_curve_table"),
		TEXT("Read a CurveTable's curves. Returns mode (\"rich\"|\"simple\"|\"empty\"), total_rows, and a 'rows' array of {row_name, keys:[{time,value,...}]}. Rich curves include per-key interp_mode + arrive/leave tangents; simple curves carry a curve-wide interp_mode and time/value keys only. Pass row_name to read a single curve."),
		FMonolithActionHandler::CreateStatic(&HandleReadCurveTable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("CurveTable asset path, e.g. /Game/Data/CT_Damage"))
			.Optional(TEXT("row_name"),   TEXT("string"), TEXT("If provided, return only this curve row. Otherwise return all rows."))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_curve_table_keys"),
		TEXT("Write keys into a CurveTable curve row, creating the row if needed. interp_mode picks rich-vs-simple (cubic -> rich; linear/constant -> simple). mode \"replace\" (default) clears existing keys first; \"merge\" keeps them. A fresh table is mode-empty and the FIRST write locks rich-vs-simple permanently — mismatched later writes are rejected. Returns {row_name, keys_written, created_row, mode, saved}."),
		FMonolithActionHandler::CreateStatic(&HandleSetCurveTableKeys),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"),  TEXT("string"),  TEXT("CurveTable asset path"))
			.Required(TEXT("row_name"),    TEXT("string"),  TEXT("Curve row name to write"))
			.Required(TEXT("keys"),        TEXT("array"),   TEXT("Array of {time:number, value:number}."))
			.Optional(TEXT("mode"),        TEXT("string"),  TEXT("\"replace\" (default, clears keys first) or \"merge\" (adds without clearing)."), TEXT("replace"))
			.Optional(TEXT("interp_mode"), TEXT("string"),  TEXT("\"linear\" (default), \"constant\", or \"cubic\". cubic -> rich curve; others -> simple. Must match the table's locked mode after the first write."), TEXT("linear"))
			.Optional(TEXT("save"),        TEXT("boolean"), TEXT("If true, save the package after writing."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_curve_table_row"),
		TEXT("Add an empty curve row to a CurveTable. interp_mode picks rich-vs-simple on the FIRST row of a fresh table (which locks the table mode). Returns {row_name, created}."),
		FMonolithActionHandler::CreateStatic(&HandleAddCurveTableRow),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"),  TEXT("string"),  TEXT("CurveTable asset path"))
			.Required(TEXT("row_name"),    TEXT("string"),  TEXT("New curve row name"))
			.Optional(TEXT("interp_mode"), TEXT("string"),  TEXT("\"linear\" (default)/\"constant\" -> simple, \"cubic\" -> rich. Determines the locked table mode for a fresh table."), TEXT("linear"))
			.Optional(TEXT("save"),        TEXT("boolean"), TEXT("If true, save the package after adding."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("remove_curve_table_row"),
		TEXT("Remove a curve row from a CurveTable. Refreshes any open CurveTable editor. Returns {row_name, removed}."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveCurveTableRow),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"),  TEXT("CurveTable asset path"))
			.Required(TEXT("row_name"),   TEXT("string"),  TEXT("Curve row name to remove"))
			.Optional(TEXT("save"),       TEXT("boolean"), TEXT("If true, save the package after removing."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("rename_curve_table_row"),
		TEXT("Rename a curve row in a CurveTable. Refreshes any open CurveTable editor. Returns {old_name, new_name, renamed}."),
		FMonolithActionHandler::CreateStatic(&HandleRenameCurveTableRow),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"),  TEXT("CurveTable asset path"))
			.Required(TEXT("old_name"),   TEXT("string"),  TEXT("Existing curve row name"))
			.Required(TEXT("new_name"),   TEXT("string"),  TEXT("New curve row name"))
			.Optional(TEXT("save"),       TEXT("boolean"), TEXT("If true, save the package after renaming."), TEXT("false"))
			.Build());
}

// ============================================================
//  read_curve_table
// ============================================================

FMonolithActionResult FMonolithBlueprintCurveTableActions::HandleReadCurveTable(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithCurveTableInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	FString Error;
	UCurveTable* CurveTable = ResolveCurveTable(AssetPath, Error);
	if (!CurveTable)
	{
		return FMonolithActionResult::Error(Error);
	}

	FString RowNameFilter;
	Params->TryGetStringField(TEXT("row_name"), RowNameFilter);

	const TMap<FName, FRealCurve*>& RowMap = CurveTable->GetRowMap();

	TArray<TSharedPtr<FJsonValue>> RowResults;
	if (!RowNameFilter.IsEmpty())
	{
		const FName RowFName(*RowNameFilter);
		FRealCurve* const* Found = RowMap.Find(RowFName);
		if (!Found || !(*Found))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Row '%s' not found in CurveTable '%s'"), *RowNameFilter, *AssetPath));
		}
		RowResults.Add(MakeShared<FJsonValueObject>(SerializeRow(CurveTable, RowFName, *Found)));
	}
	else
	{
		for (const TPair<FName, FRealCurve*>& Pair : RowMap)
		{
			RowResults.Add(MakeShared<FJsonValueObject>(SerializeRow(CurveTable, Pair.Key, Pair.Value)));
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("mode"), CurveTableModeToString(CurveTable->GetCurveTableMode()));
	Root->SetNumberField(TEXT("total_rows"), RowMap.Num());
	Root->SetArrayField(TEXT("rows"), RowResults);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  set_curve_table_keys
// ============================================================

FMonolithActionResult FMonolithBlueprintCurveTableActions::HandleSetCurveTableKeys(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithCurveTableInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString RowName = Params->GetStringField(TEXT("row_name"));
	if (AssetPath.IsEmpty()) { return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path")); }
	if (RowName.IsEmpty())   { return FMonolithActionResult::Error(TEXT("Missing required parameter: row_name")); }

	const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("keys"), KeysArray) || !KeysArray)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: keys (array of {time, value})"));
	}

	FString Error;
	UCurveTable* CurveTable = ResolveCurveTable(AssetPath, Error);
	if (!CurveTable) { return FMonolithActionResult::Error(Error); }

	FString ModeStr = TEXT("replace");
	Params->TryGetStringField(TEXT("mode"), ModeStr);
	ModeStr = ModeStr.ToLower();
	if (ModeStr != TEXT("replace") && ModeStr != TEXT("merge"))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unsupported mode '%s'. Use \"replace\" or \"merge\"."), *ModeStr));
	}
	const bool bReplace = (ModeStr == TEXT("replace"));

	FString InterpStr = TEXT("linear");
	Params->TryGetStringField(TEXT("interp_mode"), InterpStr);
	ERichCurveInterpMode InterpMode = RCIM_Linear;
	if (!ParseInterpMode(InterpStr, InterpMode))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unsupported interp_mode '%s'. Use \"linear\", \"constant\", or \"cubic\"."), *InterpStr));
	}

	const bool bWantRich = InterpModeRequiresRich(InterpMode);

	// Mode-lock enforcement: a non-empty table is permanently rich or simple.
	const ECurveTableMode TableMode = CurveTable->GetCurveTableMode();
	if (TableMode == ECurveTableMode::RichCurves && !bWantRich)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("CurveTable '%s' is locked to RICH curves; interp_mode '%s' would need a simple curve. Use \"cubic\" for this table."),
			*AssetPath, *InterpStr));
	}
	if (TableMode == ECurveTableMode::SimpleCurves && bWantRich)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("CurveTable '%s' is locked to SIMPLE curves; interp_mode \"cubic\" would need a rich curve. Use \"linear\" or \"constant\" for this table."),
			*AssetPath));
	}

	CurveTable->SetFlags(RF_Transactional);
	FScopedTransaction Transaction(NSLOCTEXT(
		"MonolithBlueprintCurveTableActions", "SetCurveTableKeys", "Monolith Set CurveTable Keys"));
	CurveTable->Modify();
	FCurveTableEditorUtils::BroadcastPreChange(CurveTable, FCurveTableEditorUtils::ECurveTableChangeInfo::RowData);

	const FName RowFName(*RowName);
	const bool bExisted = CurveTable->GetRowMap().Contains(RowFName);

	int32 KeysWritten = 0;

	if (bWantRich)
	{
		// AddRichCurve returns the existing curve if present, else creates+locks.
		FRichCurve& Curve = CurveTable->AddRichCurve(RowFName);
		if (bReplace) { Curve.Reset(); }
		for (const TSharedPtr<FJsonValue>& KeyVal : *KeysArray)
		{
			const TSharedPtr<FJsonObject>* KeyObjPtr = nullptr;
			if (!KeyVal.IsValid() || !KeyVal->TryGetObject(KeyObjPtr) || !KeyObjPtr || !(*KeyObjPtr).IsValid())
			{
				continue;
			}
			double Time = 0.0;
			double Value = 0.0;
			(*KeyObjPtr)->TryGetNumberField(TEXT("time"), Time);
			(*KeyObjPtr)->TryGetNumberField(TEXT("value"), Value);
			const FKeyHandle Handle = Curve.AddKey(static_cast<float>(Time), static_cast<float>(Value));
			Curve.SetKeyInterpMode(Handle, InterpMode);
			++KeysWritten;
		}
	}
	else
	{
		FSimpleCurve& Curve = CurveTable->AddSimpleCurve(RowFName);
		if (bReplace) { Curve.Reset(); }
		// Simple curves carry a single curve-wide interp mode (cubic disallowed).
		Curve.SetKeyInterpMode(InterpMode);
		for (const TSharedPtr<FJsonValue>& KeyVal : *KeysArray)
		{
			const TSharedPtr<FJsonObject>* KeyObjPtr = nullptr;
			if (!KeyVal.IsValid() || !KeyVal->TryGetObject(KeyObjPtr) || !KeyObjPtr || !(*KeyObjPtr).IsValid())
			{
				continue;
			}
			double Time = 0.0;
			double Value = 0.0;
			(*KeyObjPtr)->TryGetNumberField(TEXT("time"), Time);
			(*KeyObjPtr)->TryGetNumberField(TEXT("value"), Value);
			Curve.AddKey(static_cast<float>(Time), static_cast<float>(Value));
			++KeysWritten;
		}
	}

	FCurveTableEditorUtils::BroadcastPostChange(CurveTable, FCurveTableEditorUtils::ECurveTableChangeInfo::RowData);
	CurveTable->MarkPackageDirty();

	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	bool bSaved = bSave ? UEditorAssetLibrary::SaveLoadedAsset(CurveTable, false) : false;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("row_name"), RowName);
	Root->SetNumberField(TEXT("keys_written"), KeysWritten);
	Root->SetBoolField(TEXT("created_row"), !bExisted);
	Root->SetStringField(TEXT("mode"), CurveTableModeToString(CurveTable->GetCurveTableMode()));
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  add_curve_table_row
// ============================================================

FMonolithActionResult FMonolithBlueprintCurveTableActions::HandleAddCurveTableRow(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithCurveTableInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString RowName = Params->GetStringField(TEXT("row_name"));
	if (AssetPath.IsEmpty()) { return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path")); }
	if (RowName.IsEmpty())   { return FMonolithActionResult::Error(TEXT("Missing required parameter: row_name")); }

	FString Error;
	UCurveTable* CurveTable = ResolveCurveTable(AssetPath, Error);
	if (!CurveTable) { return FMonolithActionResult::Error(Error); }

	FString InterpStr = TEXT("linear");
	Params->TryGetStringField(TEXT("interp_mode"), InterpStr);
	ERichCurveInterpMode InterpMode = RCIM_Linear;
	if (!ParseInterpMode(InterpStr, InterpMode))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unsupported interp_mode '%s'. Use \"linear\", \"constant\", or \"cubic\"."), *InterpStr));
	}
	const bool bWantRich = InterpModeRequiresRich(InterpMode);

	const ECurveTableMode TableMode = CurveTable->GetCurveTableMode();
	if (TableMode == ECurveTableMode::RichCurves && !bWantRich)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("CurveTable '%s' is locked to RICH curves; use interp_mode \"cubic\"."), *AssetPath));
	}
	if (TableMode == ECurveTableMode::SimpleCurves && bWantRich)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("CurveTable '%s' is locked to SIMPLE curves; use interp_mode \"linear\" or \"constant\"."), *AssetPath));
	}

	const FName RowFName(*RowName);
	if (CurveTable->GetRowMap().Contains(RowFName))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Row '%s' already exists in CurveTable '%s'"), *RowName, *AssetPath));
	}

	CurveTable->SetFlags(RF_Transactional);
	FScopedTransaction Transaction(NSLOCTEXT(
		"MonolithBlueprintCurveTableActions", "AddCurveTableRow", "Monolith Add CurveTable Row"));
	CurveTable->Modify();
	FCurveTableEditorUtils::BroadcastPreChange(CurveTable, FCurveTableEditorUtils::ECurveTableChangeInfo::RowList);

	if (bWantRich)
	{
		CurveTable->AddRichCurve(RowFName);
	}
	else
	{
		FSimpleCurve& Curve = CurveTable->AddSimpleCurve(RowFName);
		Curve.SetKeyInterpMode(InterpMode);
	}

	FCurveTableEditorUtils::BroadcastPostChange(CurveTable, FCurveTableEditorUtils::ECurveTableChangeInfo::RowList);
	CurveTable->MarkPackageDirty();

	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	bool bSaved = bSave ? UEditorAssetLibrary::SaveLoadedAsset(CurveTable, false) : false;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("row_name"), RowName);
	Root->SetBoolField(TEXT("created"), true);
	Root->SetStringField(TEXT("mode"), CurveTableModeToString(CurveTable->GetCurveTableMode()));
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  remove_curve_table_row
// ============================================================

FMonolithActionResult FMonolithBlueprintCurveTableActions::HandleRemoveCurveTableRow(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithCurveTableInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString RowName = Params->GetStringField(TEXT("row_name"));
	if (AssetPath.IsEmpty()) { return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path")); }
	if (RowName.IsEmpty())   { return FMonolithActionResult::Error(TEXT("Missing required parameter: row_name")); }

	FString Error;
	UCurveTable* CurveTable = ResolveCurveTable(AssetPath, Error);
	if (!CurveTable) { return FMonolithActionResult::Error(Error); }

	const FName RowFName(*RowName);
	if (!CurveTable->GetRowMap().Contains(RowFName))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Row '%s' not found in CurveTable '%s'"), *RowName, *AssetPath));
	}

	CurveTable->SetFlags(RF_Transactional);
	FScopedTransaction Transaction(NSLOCTEXT(
		"MonolithBlueprintCurveTableActions", "RemoveCurveTableRow", "Monolith Remove CurveTable Row"));
	CurveTable->Modify();
	FCurveTableEditorUtils::BroadcastPreChange(CurveTable, FCurveTableEditorUtils::ECurveTableChangeInfo::RowList);

	// UCurveTable::RemoveRow returns void and deletes the associated curve.
	CurveTable->RemoveRow(RowFName);

	FCurveTableEditorUtils::BroadcastPostChange(CurveTable, FCurveTableEditorUtils::ECurveTableChangeInfo::RowList);
	CurveTable->MarkPackageDirty();

	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	bool bSaved = bSave ? UEditorAssetLibrary::SaveLoadedAsset(CurveTable, false) : false;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("row_name"), RowName);
	Root->SetBoolField(TEXT("removed"), true);
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  rename_curve_table_row
// ============================================================

FMonolithActionResult FMonolithBlueprintCurveTableActions::HandleRenameCurveTableRow(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithCurveTableInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString OldName = Params->GetStringField(TEXT("old_name"));
	const FString NewName = Params->GetStringField(TEXT("new_name"));
	if (AssetPath.IsEmpty()) { return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path")); }
	if (OldName.IsEmpty())   { return FMonolithActionResult::Error(TEXT("Missing required parameter: old_name")); }
	if (NewName.IsEmpty())   { return FMonolithActionResult::Error(TEXT("Missing required parameter: new_name")); }

	FString Error;
	UCurveTable* CurveTable = ResolveCurveTable(AssetPath, Error);
	if (!CurveTable) { return FMonolithActionResult::Error(Error); }

	FName OldFName(*OldName);
	FName NewFName(*NewName);
	if (!CurveTable->GetRowMap().Contains(OldFName))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Row '%s' not found in CurveTable '%s'"), *OldName, *AssetPath));
	}
	if (CurveTable->GetRowMap().Contains(NewFName))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Target row name '%s' already exists in CurveTable '%s'"), *NewName, *AssetPath));
	}

	CurveTable->SetFlags(RF_Transactional);
	FScopedTransaction Transaction(NSLOCTEXT(
		"MonolithBlueprintCurveTableActions", "RenameCurveTableRow", "Monolith Rename CurveTable Row"));
	CurveTable->Modify();
	FCurveTableEditorUtils::BroadcastPreChange(CurveTable, FCurveTableEditorUtils::ECurveTableChangeInfo::RowList);

	// UCurveTable::RenameRow takes non-const FName& references.
	CurveTable->RenameRow(OldFName, NewFName);

	FCurveTableEditorUtils::BroadcastPostChange(CurveTable, FCurveTableEditorUtils::ECurveTableChangeInfo::RowList);
	CurveTable->MarkPackageDirty();

	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	bool bSaved = bSave ? UEditorAssetLibrary::SaveLoadedAsset(CurveTable, false) : false;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("old_name"), OldName);
	Root->SetStringField(TEXT("new_name"), NewName);
	Root->SetBoolField(TEXT("renamed"), true);
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}
