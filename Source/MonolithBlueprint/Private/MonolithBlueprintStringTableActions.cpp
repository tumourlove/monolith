// SPDX-License-Identifier: MIT
// StringTable dataset-ergonomics actions (Part B) — read/set-entries/remove.
//
// Engine-generic: the table resolves by string. Editing goes through the wrapped
// FStringTable (Core module) via UStringTable::GetMutableStringTable().
// SetSourceString is natively an upsert. There is NO editor-refresh broadcast
// for StringTables (unlike CurveTable/DataTable) — mutations MarkPackageDirty +
// Modify and an open tab may need a reselect to refresh. Game-thread only.

#include "MonolithBlueprintStringTableActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/TextKey.h"
#include "EditorAssetLibrary.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

namespace MonolithStringTableInternal
{
	static UStringTable* ResolveStringTable(const FString& AssetPath, FString& OutError)
	{
		UStringTable* StringTable = FMonolithAssetUtils::LoadAssetByPath<UStringTable>(AssetPath);
		if (!StringTable)
		{
			OutError = FString::Printf(TEXT("StringTable not found: %s"), *AssetPath);
			return nullptr;
		}
		return StringTable;
	}
}

// ============================================================
//  Registration
// ============================================================

void FMonolithBlueprintStringTableActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("read_string_table"),
		TEXT("Read a StringTable's entries plus its namespace. Returns {asset_path, namespace, total_entries, entries:[{key, source_string, meta?:{id:value}}]}."),
		FMonolithActionHandler::CreateStatic(&HandleReadStringTable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"),    TEXT("string"),  TEXT("StringTable asset path, e.g. /Game/Localization/ST_UI"))
			.Optional(TEXT("include_meta"),  TEXT("boolean"), TEXT("Include per-entry meta-data (default false)."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_string_table_entries"),
		TEXT("Add/update StringTable entries. Each entry is {key, source_string}; SetSourceString is natively an upsert. mode \"upsert\" (default) keeps existing entries; \"replace\" clears the table first. Optional namespace sets the table-wide namespace. Returns {entries_written, removed, namespace, saved}."),
		FMonolithActionHandler::CreateStatic(&HandleSetStringTableEntries),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"),  TEXT("StringTable asset path"))
			.Required(TEXT("entries"),    TEXT("array"),   TEXT("Array of {key:string, source_string:string}."))
			.Optional(TEXT("mode"),       TEXT("string"),  TEXT("\"upsert\" (default) or \"replace\" (clears all entries first)."), TEXT("upsert"))
			.Optional(TEXT("namespace"),  TEXT("string"),  TEXT("If provided, set the table-wide namespace used by all entries."))
			.Optional(TEXT("save"),       TEXT("boolean"), TEXT("If true, save the package after writing."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("remove_string_table_entry"),
		TEXT("Remove a single entry (and its meta-data) from a StringTable by key. Returns {key, removed}."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveStringTableEntry),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"),  TEXT("StringTable asset path"))
			.Required(TEXT("key"),        TEXT("string"),  TEXT("Entry key to remove"))
			.Optional(TEXT("save"),       TEXT("boolean"), TEXT("If true, save the package after removing."), TEXT("false"))
			.Build());
}

// ============================================================
//  read_string_table
// ============================================================

FMonolithActionResult FMonolithBlueprintStringTableActions::HandleReadStringTable(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithStringTableInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	FString Error;
	UStringTable* StringTable = ResolveStringTable(AssetPath, Error);
	if (!StringTable)
	{
		return FMonolithActionResult::Error(Error);
	}

	bool bIncludeMeta = false;
	Params->TryGetBoolField(TEXT("include_meta"), bIncludeMeta);

	const FStringTableConstRef Table = StringTable->GetStringTable();

	TArray<TSharedPtr<FJsonValue>> Entries;
	Table->EnumerateKeysAndSourceStrings(
		[&Entries, &Table, bIncludeMeta](const FTextKey& Key, const FString& SourceString) -> bool
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("key"), Key.ToString());
			Entry->SetStringField(TEXT("source_string"), SourceString);

			if (bIncludeMeta)
			{
				TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
				bool bHasMeta = false;
				Table->EnumerateMetaData(Key,
					[&Meta, &bHasMeta](FName MetaId, const FString& MetaValue) -> bool
					{
						Meta->SetStringField(MetaId.ToString(), MetaValue);
						bHasMeta = true;
						return true;
					});
				if (bHasMeta)
				{
					Entry->SetObjectField(TEXT("meta"), Meta);
				}
			}

			Entries.Add(MakeShared<FJsonValueObject>(Entry));
			return true;
		});

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("namespace"), Table->GetNamespace());
	Root->SetNumberField(TEXT("total_entries"), Entries.Num());
	Root->SetArrayField(TEXT("entries"), Entries);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  set_string_table_entries
// ============================================================

FMonolithActionResult FMonolithBlueprintStringTableActions::HandleSetStringTableEntries(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithStringTableInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	const TArray<TSharedPtr<FJsonValue>>* EntriesArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("entries"), EntriesArray) || !EntriesArray)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: entries (array of {key, source_string})"));
	}

	FString Error;
	UStringTable* StringTable = ResolveStringTable(AssetPath, Error);
	if (!StringTable) { return FMonolithActionResult::Error(Error); }

	FString ModeStr = TEXT("upsert");
	Params->TryGetStringField(TEXT("mode"), ModeStr);
	ModeStr = ModeStr.ToLower();
	if (ModeStr != TEXT("upsert") && ModeStr != TEXT("replace"))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unsupported mode '%s'. Use \"upsert\" or \"replace\"."), *ModeStr));
	}
	const bool bReplace = (ModeStr == TEXT("replace"));

	StringTable->SetFlags(RF_Transactional);
	FScopedTransaction Transaction(NSLOCTEXT(
		"MonolithBlueprintStringTableActions", "SetStringTableEntries", "Monolith Set StringTable Entries"));
	StringTable->Modify();

	const FStringTableRef Table = StringTable->GetMutableStringTable();

	int32 RemovedCount = 0;
	if (bReplace)
	{
		// Count what we are clearing for the response, then wipe.
		Table->EnumerateKeysAndSourceStrings(
			[&RemovedCount](const FTextKey&, const FString&) -> bool { ++RemovedCount; return true; });
		Table->ClearSourceStrings();
	}

	FString Namespace;
	if (Params->TryGetStringField(TEXT("namespace"), Namespace))
	{
		Table->SetNamespace(FTextKey(*Namespace));
	}

	int32 EntriesWritten = 0;
	for (const TSharedPtr<FJsonValue>& EntryVal : *EntriesArray)
	{
		const TSharedPtr<FJsonObject>* EntryObjPtr = nullptr;
		if (!EntryVal.IsValid() || !EntryVal->TryGetObject(EntryObjPtr) || !EntryObjPtr || !(*EntryObjPtr).IsValid())
		{
			continue;
		}

		FString Key;
		FString SourceString;
		if (!(*EntryObjPtr)->TryGetStringField(TEXT("key"), Key) || Key.IsEmpty())
		{
			continue;
		}
		(*EntryObjPtr)->TryGetStringField(TEXT("source_string"), SourceString);

		// SetSourceString replaces any existing data for that key (upsert).
		Table->SetSourceString(FTextKey(*Key), SourceString);
		++EntriesWritten;
	}

	StringTable->MarkPackageDirty();

	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	bool bSaved = bSave ? UEditorAssetLibrary::SaveLoadedAsset(StringTable, false) : false;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("entries_written"), EntriesWritten);
	Root->SetNumberField(TEXT("removed"), RemovedCount);
	Root->SetStringField(TEXT("namespace"), Table->GetNamespace());
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  remove_string_table_entry
// ============================================================

FMonolithActionResult FMonolithBlueprintStringTableActions::HandleRemoveStringTableEntry(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithStringTableInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString Key = Params->GetStringField(TEXT("key"));
	if (AssetPath.IsEmpty()) { return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path")); }
	if (Key.IsEmpty())       { return FMonolithActionResult::Error(TEXT("Missing required parameter: key")); }

	FString Error;
	UStringTable* StringTable = ResolveStringTable(AssetPath, Error);
	if (!StringTable) { return FMonolithActionResult::Error(Error); }

	const FStringTableRef Table = StringTable->GetMutableStringTable();

	// Confirm the key exists so we can report removed=false rather than silently
	// claiming success on a missing key.
	FString Existing;
	const bool bExisted = Table->GetSourceString(FTextKey(*Key), Existing);
	if (!bExisted)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Key '%s' not found in StringTable '%s'"), *Key, *AssetPath));
	}

	StringTable->SetFlags(RF_Transactional);
	FScopedTransaction Transaction(NSLOCTEXT(
		"MonolithBlueprintStringTableActions", "RemoveStringTableEntry", "Monolith Remove StringTable Entry"));
	StringTable->Modify();

	Table->RemoveSourceString(FTextKey(*Key));
	StringTable->MarkPackageDirty();

	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	bool bSaved = bSave ? UEditorAssetLibrary::SaveLoadedAsset(StringTable, false) : false;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("key"), Key);
	Root->SetBoolField(TEXT("removed"), true);
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}
