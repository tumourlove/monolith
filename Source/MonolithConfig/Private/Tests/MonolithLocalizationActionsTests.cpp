// SPDX-License-Identifier: MIT
//
// MonolithLocalizationActionsTests — automation coverage for the read-only
// `localization` namespace. Verifies:
//   1. Registration — exactly the four read actions, dispatcher annotated
//      read-only + idempotent, and the bound parameters published in schema.
//   2. Param guards — every out-of-range bound and non-canonical path is
//      rejected as invalid-params (-32602), before any asset is touched.
//   3. Readback — pagination obeys its limits, the after_key cursor advances in
//      stable key order, source strings and metadata truncate explicitly, and
//      completeness is reported separately from truncation.
//   4. Validation — warnings do not invalidate a complete table, an empty table
//      is an error, and a scan cut short can never report valid=true.
//   5. Reads never dirty the inspected package.
//
// The fixture builds in-memory StringTables (registered with the AssetRegistry
// but never saved) so nothing lands on disk. Keys are chosen to be distinct
// under a case-sensitive ordering, which is the order the actions page in.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "MonolithLocalizationActions.h"
#include "MonolithToolRegistry.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/TextKey.h"
#include "Misc/Guid.h"
#include "Runtime/Launch/Resources/Version.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace MonolithLocalizationTestDetail
{
	static const TCHAR* RequiredActions[] = {
		TEXT("list_cultures"),
		TEXT("list_string_tables"),
		TEXT("get_string_table"),
		TEXT("validate_string_table")
	};

	static void EnsureRegistered()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("localization"), TEXT("list_cultures")))
		{
			FMonolithLocalizationActions::RegisterActions(Registry);
		}
	}

	static FMonolithActionResult Execute(const TCHAR* Action, const TSharedPtr<FJsonObject>& Params)
	{
		EnsureRegistered();
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), Action, Params);
	}

	static bool GetBool(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field, bool Default = false)
	{
		bool Value = Default;
		return Json.IsValid() && Json->TryGetBoolField(Field, Value) ? Value : Default;
	}

	static int32 GetInt(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field, int32 Default = 0)
	{
		double Value = static_cast<double>(Default);
		return Json.IsValid() && Json->TryGetNumberField(Field, Value) ? static_cast<int32>(Value) : Default;
	}

	static FString GetString(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field)
	{
		FString Value;
		if (Json.IsValid())
		{
			Json->TryGetStringField(Field, Value);
		}
		return Value;
	}

	static const TArray<TSharedPtr<FJsonValue>>* GetArray(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		return Json.IsValid() && Json->TryGetArrayField(Field, Values) ? Values : nullptr;
	}

	/** First entry object in an entries array whose "key" matches, or null. */
	static TSharedPtr<FJsonObject> FindEntry(const TArray<TSharedPtr<FJsonValue>>* Entries, const TCHAR* Key)
	{
		if (!Entries)
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Entries)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (Value.IsValid() && Value->TryGetObject(Object) && Object && Object->IsValid()
				&& GetString(*Object, TEXT("key")).Equals(Key, ESearchCase::CaseSensitive))
			{
				return *Object;
			}
		}
		return nullptr;
	}

	// UE 5.8 replaces the 2-arg editor-only setter with a dev-notes overload.
	static void SetSourceString(const FStringTableRef& Table, const TCHAR* Key, const FString& SourceString)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8 && WITH_EDITORONLY_DATA
		Table->SetSourceString(FTextKey(Key), SourceString, FString());
#else
		Table->SetSourceString(FTextKey(Key), SourceString);
#endif
	}

	/**
	 * Two in-memory StringTables under a GUID-unique root.
	 *
	 * TableA holds four entries that sort, case-sensitively, as
	 * " Pad" < "Alpha" < "Empty" < "Long" and carry exactly one structural
	 * warning each for edge whitespace and an empty source string, plus three
	 * metadata rows in total. TableB is empty.
	 */
	struct FStringTableFixture
	{
		FString RootPath;
		UPackage* PackageA = nullptr;
		UPackage* PackageB = nullptr;
		UStringTable* TableA = nullptr;
		UStringTable* TableB = nullptr;

		FStringTableFixture()
		{
			RootPath = FString::Printf(TEXT("/Game/__MonolithLocalizationTests/%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
			PackageA = CreatePackage(*(RootPath + TEXT("/ST_A")));
			PackageB = CreatePackage(*(RootPath + TEXT("/ST_B")));
			TableA = NewObject<UStringTable>(PackageA, TEXT("ST_A"), RF_Public | RF_Standalone);
			TableB = NewObject<UStringTable>(PackageB, TEXT("ST_B"), RF_Public | RF_Standalone);

			const FStringTableRef Core = TableA->GetMutableStringTable();
			SetSourceString(Core, TEXT(" Pad"), TEXT("Padded key"));
			SetSourceString(Core, TEXT("Alpha"), TEXT("Primary value"));
			SetSourceString(Core, TEXT("Empty"), FString());
			SetSourceString(Core, TEXT("Long"), TEXT("1234567890"));
			Core->SetMetaData(FTextKey(TEXT("Alpha")), TEXT("Context"), TEXT("Menu"));
			Core->SetMetaData(FTextKey(TEXT("Alpha")), TEXT("Note"), TEXT("Detail"));
			Core->SetMetaData(FTextKey(TEXT("Long")), TEXT("Context"), TEXT("Gameplay"));

			FAssetRegistryModule::AssetCreated(TableA);
			FAssetRegistryModule::AssetCreated(TableB);

			// Baseline for the "reads never dirty a package" assertion.
			PackageA->SetDirtyFlag(false);
			PackageB->SetDirtyFlag(false);
		}

		~FStringTableFixture()
		{
			Discard(TableA, PackageA);
			Discard(TableB, PackageB);
		}

	private:
		static void Discard(UStringTable* Table, UPackage* Package)
		{
			if (Table)
			{
				FAssetRegistryModule::AssetDeleted(Table);
				Table->ClearFlags(RF_Public | RF_Standalone);
				Table->MarkAsGarbage();
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
		}
	};
}

// ---------------------------------------------------------------------------
// 1. Registration + dispatcher annotations
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithLocalizationRegistrationTest,
	"Monolith.Localization.Read.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLocalizationRegistrationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLocalizationTestDetail;

	EnsureRegistered();
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	bool bPassed = true;
	for (const TCHAR* Action : RequiredActions)
	{
		bPassed &= TestTrue(*FString::Printf(TEXT("localization.%s is registered"), Action),
			Registry.HasAction(TEXT("localization"), Action));
	}
	bPassed &= TestEqual(TEXT("The localization namespace owns exactly four actions"),
		Registry.GetActions(TEXT("localization")).Num(), 4);
	bPassed &= TestFalse(TEXT("No authoring action leaked into the read-only namespace"),
		Registry.HasAction(TEXT("localization"), TEXT("create_string_table")));

	const FMonolithDispatcherAnnotations Annotations = Registry.GetDispatcherAnnotations(TEXT("localization"));
	bPassed &= TestTrue(TEXT("localization dispatcher is read-only"), Annotations.bReadOnlyHint);
	bPassed &= TestTrue(TEXT("localization dispatcher is idempotent"), Annotations.bIdempotentHint);
	bPassed &= TestFalse(TEXT("localization dispatcher is not destructive"), Annotations.bDestructiveHint);

	for (const FMonolithActionInfo& Info : Registry.GetActions(TEXT("localization")))
	{
		if (Info.Action == TEXT("get_string_table"))
		{
			bPassed &= TestTrue(TEXT("Entry read schema publishes its cursor and budgets"),
				Info.ParamSchema.IsValid()
					&& Info.ParamSchema->HasField(TEXT("after_key"))
					&& Info.ParamSchema->HasField(TEXT("entry_limit"))
					&& Info.ParamSchema->HasField(TEXT("metadata_limit"))
					&& Info.ParamSchema->HasField(TEXT("text_limit")));
		}
		else if (Info.Action == TEXT("validate_string_table"))
		{
			bPassed &= TestTrue(TEXT("Validation schema publishes its scan and issue bounds"),
				Info.ParamSchema.IsValid()
					&& Info.ParamSchema->HasField(TEXT("scan_limit"))
					&& Info.ParamSchema->HasField(TEXT("issue_offset"))
					&& Info.ParamSchema->HasField(TEXT("issue_limit")));
		}
	}
	return bPassed;
}

// ---------------------------------------------------------------------------
// 2. Parameter guards — bounds and canonical paths
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithLocalizationParamGuardsTest,
	"Monolith.Localization.Read.ParamGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLocalizationParamGuardsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLocalizationTestDetail;

	bool bPassed = true;

	auto ExpectInvalidParams = [this, &bPassed](const TCHAR* What, const TCHAR* Action,
		const TSharedPtr<FJsonObject>& Params)
	{
		const FMonolithActionResult Result = Execute(Action, Params);
		bPassed &= TestFalse(*FString::Printf(TEXT("%s is rejected"), What), Result.bSuccess);
		bPassed &= TestEqual(*FString::Printf(TEXT("%s is invalid-params"), What), Result.ErrorCode, -32602);
	};

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 0);
		ExpectInvalidParams(TEXT("A zero culture limit"), TEXT("list_cultures"), Params);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 501);
		ExpectInvalidParams(TEXT("An over-max culture limit"), TEXT("list_cultures"), Params);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Names;
		for (int32 Index = 0; Index < 257; ++Index)
		{
			Names.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("x-%d"), Index)));
		}
		Params->SetArrayField(TEXT("culture_names"), Names);
		ExpectInvalidParams(TEXT("An oversized culture list"), TEXT("list_cultures"), Params);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), TEXT("/Game/"));
		ExpectInvalidParams(TEXT("A non-canonical package root"), TEXT("list_string_tables"), Params);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), TEXT("/NotAMountPoint/Localization"));
		ExpectInvalidParams(TEXT("An unmounted package root"), TEXT("list_string_tables"), Params);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 1001);
		ExpectInvalidParams(TEXT("An over-max table limit"), TEXT("list_string_tables"), Params);
	}
	{
		// A dotted path whose object name is not the package leaf is a
		// sub-object reference, which these actions do not inspect.
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Localization/ST_UI.Wrong"));
		ExpectInvalidParams(TEXT("A mismatched object leaf"), TEXT("get_string_table"), Params);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		ExpectInvalidParams(TEXT("A missing asset_path"), TEXT("get_string_table"), Params);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Localization/ST_UI"));
		Params->SetStringField(TEXT("after_key"), FString::ChrN(4097, TEXT('x')));
		ExpectInvalidParams(TEXT("An oversized cursor"), TEXT("get_string_table"), Params);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Localization/ST_UI"));
		Params->SetNumberField(TEXT("entry_limit"), 1001);
		ExpectInvalidParams(TEXT("An over-max entry limit"), TEXT("get_string_table"), Params);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Localization/ST_UI"));
		Params->SetNumberField(TEXT("text_limit"), 65537);
		ExpectInvalidParams(TEXT("An over-max text limit"), TEXT("get_string_table"), Params);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Localization/ST_UI"));
		Params->SetNumberField(TEXT("scan_limit"), 0);
		ExpectInvalidParams(TEXT("A zero validation scan"), TEXT("validate_string_table"), Params);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Localization/ST_UI"));
		Params->SetNumberField(TEXT("scan_limit"), 10001);
		ExpectInvalidParams(TEXT("An over-max validation scan"), TEXT("validate_string_table"), Params);
	}
	return bPassed;
}

// ---------------------------------------------------------------------------
// 3 + 4 + 5. Readback, validation, and package cleanliness
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithLocalizationReadbackTest,
	"Monolith.Localization.Read.ReadbackAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLocalizationReadbackTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLocalizationTestDetail;

	FStringTableFixture Fixture;
	bool bPassed = true;

	// --- Cultures -----------------------------------------------------------
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 1);
		const FMonolithActionResult Result = Execute(TEXT("list_cultures"), Params);
		bPassed &= TestTrue(TEXT("Culture discovery succeeds"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("The culture page obeys its limit"),
			GetInt(Result.Result, TEXT("count")), 1);
		bPassed &= TestFalse(TEXT("The current culture is reported"),
			GetString(Result.Result, TEXT("current_culture")).IsEmpty());
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Names;
		Names.Add(MakeShared<FJsonValueString>(TEXT("en")));
		Names.Add(MakeShared<FJsonValueString>(TEXT("zz-NotACulture")));
		Params->SetArrayField(TEXT("culture_names"), Names);
		Params->SetBoolField(TEXT("include_derived"), false);
		const FMonolithActionResult Result = Execute(TEXT("list_cultures"), Params);
		bPassed &= TestTrue(TEXT("Explicit-root culture resolution succeeds"), Result.bSuccess);
		bPassed &= TestTrue(TEXT("Explicit roots are flagged as such"),
			GetBool(Result.Result, TEXT("explicit_names")));
		const TArray<TSharedPtr<FJsonValue>>* Unresolved = GetArray(Result.Result, TEXT("unresolved_names"));
		bPassed &= TestTrue(TEXT("An unknown culture root is reported unresolved"),
			Unresolved != nullptr && Unresolved->Num() == 1);
	}

	// --- Table discovery ----------------------------------------------------
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), Fixture.RootPath);
		Params->SetNumberField(TEXT("limit"), 1);
		Params->SetBoolField(TEXT("include_details"), true);
		const FMonolithActionResult Result = Execute(TEXT("list_string_tables"), Params);
		bPassed &= TestTrue(TEXT("StringTable discovery succeeds"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Both fixture tables are discovered"),
			GetInt(Result.Result, TEXT("total")), 2);
		bPassed &= TestEqual(TEXT("The table page obeys its limit"),
			GetInt(Result.Result, TEXT("count")), 1);
		bPassed &= TestTrue(TEXT("The table page reports another page"),
			GetBool(Result.Result, TEXT("has_more")));

		// Sorted by object path, so ST_A is the first page and details are loaded.
		const TArray<TSharedPtr<FJsonValue>>* Tables = GetArray(Result.Result, TEXT("string_tables"));
		const TSharedPtr<FJsonObject>* FirstRow = nullptr;
		if (Tables && Tables->Num() == 1 && (*Tables)[0].IsValid())
		{
			(*Tables)[0]->TryGetObject(FirstRow);
		}
		bPassed &= TestTrue(TEXT("The returned page carries loaded details"),
			FirstRow && FirstRow->IsValid() && GetInt(*FirstRow, TEXT("entry_count"), -1) == 4);
	}

	// --- Entry paging + cursor ---------------------------------------------
	FString FirstCursor;
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.TableA->GetPathName());
		Params->SetNumberField(TEXT("entry_limit"), 1);
		const FMonolithActionResult Result = Execute(TEXT("get_string_table"), Params);
		bPassed &= TestTrue(TEXT("A bounded entry read succeeds"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("One entry is returned"),
			GetInt(Result.Result, TEXT("entries_returned")), 1);
		bPassed &= TestEqual(TEXT("The whole table is still counted"),
			GetInt(Result.Result, TEXT("entry_count")), 4);
		bPassed &= TestTrue(TEXT("The first page reports more entries"),
			GetBool(Result.Result, TEXT("has_more_entries")));
		bPassed &= TestFalse(TEXT("A partial page cannot claim coverage"),
			GetBool(Result.Result, TEXT("all_entries_covered"), true));
		bPassed &= TestFalse(TEXT("A partial page cannot claim completeness"),
			GetBool(Result.Result, TEXT("complete"), true));
		FirstCursor = GetString(Result.Result, TEXT("next_after_key"));
		bPassed &= TestEqual(TEXT("Paging starts at the smallest key"), FirstCursor, FString(TEXT(" Pad")));
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.TableA->GetPathName());
		Params->SetStringField(TEXT("after_key"), FirstCursor);
		Params->SetNumberField(TEXT("entry_limit"), 1);
		const FMonolithActionResult Result = Execute(TEXT("get_string_table"), Params);
		bPassed &= TestTrue(TEXT("Cursor continuation succeeds"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Entries after the cursor are counted"),
			GetInt(Result.Result, TEXT("entries_after_cursor")), 3);
		bPassed &= TestTrue(TEXT("The cursor advances in stable key order"),
			FindEntry(GetArray(Result.Result, TEXT("entries")), TEXT("Alpha")).IsValid());
		bPassed &= TestFalse(TEXT("A cursor page never claims coverage"),
			GetBool(Result.Result, TEXT("all_entries_covered"), true));
	}

	// --- Metadata budget + text truncation ---------------------------------
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.TableA->GetPathName());
		Params->SetNumberField(TEXT("entry_limit"), 10);
		Params->SetBoolField(TEXT("include_metadata"), true);
		Params->SetNumberField(TEXT("metadata_limit"), 1);
		Params->SetNumberField(TEXT("text_limit"), 5);
		const FMonolithActionResult Result = Execute(TEXT("get_string_table"), Params);
		bPassed &= TestTrue(TEXT("A metadata-bounded read succeeds"), Result.bSuccess);
		bPassed &= TestTrue(TEXT("Every entry fits in the page"),
			GetBool(Result.Result, TEXT("all_entries_covered")));
		bPassed &= TestEqual(TEXT("The shared metadata budget is enforced across entries"),
			GetInt(Result.Result, TEXT("returned_metadata_count")), 1);
		bPassed &= TestEqual(TEXT("Skipped metadata is still counted"),
			GetInt(Result.Result, TEXT("available_metadata_count")), 3);
		bPassed &= TestFalse(TEXT("The metadata cutoff is explicit"),
			GetBool(Result.Result, TEXT("metadata_complete"), true));
		bPassed &= TestFalse(TEXT("A metadata cutoff prevents complete=true"),
			GetBool(Result.Result, TEXT("complete"), true));

		const TSharedPtr<FJsonObject> LongEntry = FindEntry(GetArray(Result.Result, TEXT("entries")), TEXT("Long"));
		bPassed &= TestTrue(TEXT("A long source string is truncated to the text limit"),
			LongEntry.IsValid()
				&& GetString(LongEntry, TEXT("source_string")) == TEXT("12345")
				&& GetBool(LongEntry, TEXT("source_string_truncated"))
				&& GetInt(LongEntry, TEXT("source_string_length")) == 10);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.TableA->GetPathName());
		Params->SetNumberField(TEXT("entry_limit"), 10);
		Params->SetBoolField(TEXT("include_metadata"), true);
		const FMonolithActionResult Result = Execute(TEXT("get_string_table"), Params);
		bPassed &= TestTrue(TEXT("A full projection read succeeds"), Result.bSuccess);
		bPassed &= TestTrue(TEXT("A full projection is complete"),
			GetBool(Result.Result, TEXT("complete")));
		bPassed &= TestEqual(TEXT("A full projection returns every metadata row"),
			GetInt(Result.Result, TEXT("returned_metadata_count")), 3);
	}

	// --- Validation ---------------------------------------------------------
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.TableA->GetPathName());
		Params->SetNumberField(TEXT("scan_limit"), 10);
		Params->SetNumberField(TEXT("issue_limit"), 1);
		const FMonolithActionResult Result = Execute(TEXT("validate_string_table"), Params);
		bPassed &= TestTrue(TEXT("Validation succeeds"), Result.bSuccess);
		bPassed &= TestTrue(TEXT("A full scan is complete"), GetBool(Result.Result, TEXT("complete")));
		bPassed &= TestTrue(TEXT("Warnings do not invalidate a complete table"),
			GetBool(Result.Result, TEXT("valid")));
		bPassed &= TestEqual(TEXT("No structural errors are reported"),
			GetInt(Result.Result, TEXT("errors")), 0);
		bPassed &= TestEqual(TEXT("Edge whitespace and an empty source are warnings"),
			GetInt(Result.Result, TEXT("warnings")), 2);
		bPassed &= TestEqual(TEXT("The issue page obeys its limit"),
			GetInt(Result.Result, TEXT("issues_returned")), 1);
		bPassed &= TestTrue(TEXT("Issue pagination reports the remainder"),
			GetBool(Result.Result, TEXT("has_more_issues")));
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.TableB->GetPathName());
		const FMonolithActionResult Result = Execute(TEXT("validate_string_table"), Params);
		bPassed &= TestTrue(TEXT("Empty-table validation returns structured output"), Result.bSuccess);
		bPassed &= TestTrue(TEXT("Empty-table validation is complete"),
			GetBool(Result.Result, TEXT("complete")));
		bPassed &= TestFalse(TEXT("An empty table is not valid"),
			GetBool(Result.Result, TEXT("valid"), true));
		bPassed &= TestEqual(TEXT("An empty table reports one error"),
			GetInt(Result.Result, TEXT("errors")), 1);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.TableA->GetPathName());
		Params->SetNumberField(TEXT("scan_limit"), 1);
		const FMonolithActionResult Result = Execute(TEXT("validate_string_table"), Params);
		bPassed &= TestTrue(TEXT("A bounded scan returns structured output"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("A bounded scan reports what it covered"),
			GetInt(Result.Result, TEXT("entries_scanned")), 1);
		bPassed &= TestFalse(TEXT("A scan cutoff is incomplete"),
			GetBool(Result.Result, TEXT("complete"), true));
		bPassed &= TestFalse(TEXT("An incomplete scan can never claim valid"),
			GetBool(Result.Result, TEXT("valid"), true));
	}

	// --- Wrong-class + missing asset ---------------------------------------
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.RootPath + TEXT("/ST_DoesNotExist"));
		const FMonolithActionResult Result = Execute(TEXT("get_string_table"), Params);
		bPassed &= TestFalse(TEXT("A missing StringTable is an error"), Result.bSuccess);
	}

	// --- Reads never dirty a package ---------------------------------------
	bPassed &= TestFalse(TEXT("The primary fixture package stays clean"), Fixture.PackageA->IsDirty());
	bPassed &= TestFalse(TEXT("The secondary fixture package stays clean"), Fixture.PackageB->IsDirty());
	return bPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
