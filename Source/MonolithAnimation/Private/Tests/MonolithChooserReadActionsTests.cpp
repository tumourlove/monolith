// Copyright tumourlove. All Rights Reserved.

// =============================================================================
// MonolithChooserReadActionsTests.cpp
//
// Coverage for the six bounded, reflection-only chooser read actions
// (FMonolithChooserReadActions): list_chooser_tables, get_chooser_table,
// list_chooser_columns, list_chooser_rows, list_chooser_references,
// validate_chooser_table.
//
// WHAT THESE ASSERT (each maps to a contract that is easy to regress silently):
//   1. RegistrationAndSchemas   — all six register into the closed `chooser`
//      lifecycle and publish their pagination params.
//   2. ParamGuards              — non-canonical paths and wrong-typed params are
//      REJECTED with ErrInvalidParams rather than silently repaired or defaulted.
//   3. EmptyTableValidation     — an empty table is structurally VALID (warnings
//      only), readback is exact, and nothing dirties the package.
//   4. AuthoringRoundTrip       — rows authored through the existing authoring
//      actions read back with the right counts; the deprecated migrated-away
//      RowValues array is ignored; stale CookedResults never inflate row_count;
//      the compact serializer reports its own truncation boundary.
//   5. RootContextAndPayload    — a child table reports the ROOT's context count,
//      and invalid / null result payloads make validation fail with codes.
//   6. DeletedAssetPackageShell — a loaded but EMPTY UPackage shell is not
//      accepted as asset existence (the exact-evidence rule).
//
// SKIP semantics: the asset-backed lanes need the optional Chooser plugin and
// editor-only data. Off-gate they AddInfo and pass; the registration/param lanes
// run unconditionally because the schemas must stay discoverable either way.
// Every fixture asset is disposable and discarded at the end of its test.
// =============================================================================

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

#include "MonolithChooserActions.h"
#include "MonolithChooserAuthoringActions.h"
#include "MonolithChooserReadActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"

#include "AssetRegistry/AssetRegistryModule.h"

#define MONOLITH_CHOOSER_READ_TESTS_ASSET_LANE (WITH_CHOOSER && WITH_EDITORONLY_DATA)

#if MONOLITH_CHOOSER_READ_TESTS_ASSET_LANE
#include "Chooser.h"                 // UChooserTable
#include "ObjectChooser_Asset.h"     // FSoftAssetChooser
#include "Curves/CurveFloat.h"       // disposable referenced asset
#endif

namespace MonolithChooserReadTests
{
	/** Disposable-asset home (agent-safety rule). No trailing slash: this doubles as a path_filter. */
	const TCHAR* const TestFolder = TEXT("/Game/Tests/Monolith/ChooserRead");

	void RegisterChooserActions(FMonolithToolRegistry& Registry)
	{
		if (!Registry.HasAction(TEXT("chooser"), TEXT("list_chooser_tables")))
		{
			FMonolithChooserReadActions::RegisterActions(Registry);
		}
		if (!Registry.HasAction(TEXT("chooser"), TEXT("inspect_chooser")))
		{
			FMonolithChooserActions::RegisterActions(Registry);
		}
		if (!Registry.HasAction(TEXT("chooser"), TEXT("create_chooser_table")))
		{
			FMonolithChooserAuthoringActions::RegisterActions(Registry);
		}
	}

	/**
	 * Take the array by const ref: FMonolithToolRegistry::GetActions returns BY VALUE, so
	 * FindByPredicate on the call expression would hand back a pointer into a destroyed temporary.
	 */
	const FMonolithActionInfo* FindAction(const TArray<FMonolithActionInfo>& Actions, const FString& ActionName)
	{
		return Actions.FindByPredicate(
			[&ActionName](const FMonolithActionInfo& Action) { return Action.Action == ActionName; });
	}

	/** Comma-joined issue codes, so a failing assertion names what validation actually reported. */
	FString JoinIssueCodes(const TSharedPtr<FJsonObject>& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("issues"), Issues) || !Issues)
		{
			return TEXT("<issues unavailable>");
		}
		TArray<FString> Codes;
		Codes.Reserve(Issues->Num());
		for (const TSharedPtr<FJsonValue>& IssueValue : *Issues)
		{
			const TSharedPtr<FJsonObject>* Issue = nullptr;
			FString Code;
			if (IssueValue.IsValid() && IssueValue->TryGetObject(Issue) && Issue && Issue->IsValid()
				&& (*Issue)->TryGetStringField(TEXT("code"), Code))
			{
				Codes.Add(Code);
			}
		}
		return Codes.IsEmpty() ? TEXT("<none>") : FString::Join(Codes, TEXT(", "));
	}

#if MONOLITH_CHOOSER_READ_TESTS_ASSET_LANE
	struct FChooserFixture
	{
		UChooserTable* Table = nullptr;
		UPackage* TablePackage = nullptr;
		UCurveFloat* OutputAsset = nullptr;
		UPackage* OutputPackage = nullptr;
		FString TablePackagePath;
		FString TableObjectPath;
		FString OutputObjectPath;
	};

	template <typename T>
	T* MakeDisposableAsset(const FString& BaseName, UPackage*& OutPackage, FString& OutObjectPath)
	{
		const FString Name = BaseName + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString PackagePath = FString(TestFolder) + TEXT("/") + Name;
		OutPackage = CreatePackage(*PackagePath);
		T* Asset = NewObject<T>(OutPackage, *Name, RF_Public | RF_Standalone | RF_Transactional);
		if (Asset)
		{
			FAssetRegistryModule::AssetCreated(Asset);
			OutPackage->SetDirtyFlag(false);
			OutObjectPath = Asset->GetPathName();
		}
		return Asset;
	}

	FChooserFixture CreateFixture()
	{
		FChooserFixture Fixture;
		Fixture.Table = MakeDisposableAsset<UChooserTable>(
			TEXT("CHT_Read_"), Fixture.TablePackage, Fixture.TableObjectPath);
		if (Fixture.Table)
		{
			Fixture.TablePackagePath = Fixture.TablePackage->GetName();
		}
		Fixture.OutputAsset = MakeDisposableAsset<UCurveFloat>(
			TEXT("Curve_ChooserOutput_"), Fixture.OutputPackage, Fixture.OutputObjectPath);
		return Fixture;
	}

	void DiscardAsset(UObject* Asset, UPackage* Package)
	{
		if (Asset)
		{
			FAssetRegistryModule::AssetDeleted(Asset);
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->Rename(nullptr, GetTransientPackage(),
				REN_DontCreateRedirectors | REN_NonTransactional | REN_AllowPackageLinkerMismatch);
			Asset->MarkAsGarbage();
		}
		if (Package)
		{
			Package->SetDirtyFlag(false);
		}
	}

	void DiscardFixture(FChooserFixture& Fixture)
	{
		DiscardAsset(Fixture.Table, Fixture.TablePackage);
		DiscardAsset(Fixture.OutputAsset, Fixture.OutputPackage);
	}

	/** Append `RowCount` aligned rows through the existing authoring action. */
	bool AuthorBoolColumnAndRows(FMonolithToolRegistry& Registry, const FChooserFixture& Fixture, int32 RowCount)
	{
		TSharedPtr<FJsonObject> ColumnParams = MakeShared<FJsonObject>();
		ColumnParams->SetStringField(TEXT("asset_path"), Fixture.TableObjectPath);
		ColumnParams->SetStringField(TEXT("column_kind"), TEXT("Bool"));
		if (!Registry.ExecuteAction(TEXT("chooser"), TEXT("add_chooser_column"), ColumnParams).bSuccess)
		{
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> Cells;
		Cells.Add(MakeShared<FJsonValueBoolean>(true));
		TSharedPtr<FJsonObject> RowParams = MakeShared<FJsonObject>();
		RowParams->SetStringField(TEXT("asset_path"), Fixture.TableObjectPath);
		RowParams->SetArrayField(TEXT("cells"), Cells);
		RowParams->SetStringField(TEXT("output_psd"), Fixture.OutputObjectPath);
		for (int32 RowIndex = 0; RowIndex < RowCount; ++RowIndex)
		{
			if (!Registry.ExecuteAction(TEXT("chooser"), TEXT("add_chooser_row"), RowParams).bSuccess)
			{
				return false;
			}
		}
		return true;
	}
#endif // MONOLITH_CHOOSER_READ_TESTS_ASSET_LANE
}

// ---------------------------------------------------------------------------
// 1. Registration + published schemas
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithChooserReadRegistrationTest,
	"Monolith.Chooser.Read.RegistrationAndSchemas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithChooserReadRegistrationTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithChooserReadTests::RegisterChooserActions(Registry);

	// Registration is UNCONDITIONAL: the schemas stay discoverable even with the
	// optional Chooser plugin disabled (the handlers then return an availability error).
	const TArray<FString> ExpectedReadActions = {
		TEXT("list_chooser_tables"),
		TEXT("get_chooser_table"),
		TEXT("list_chooser_columns"),
		TEXT("list_chooser_rows"),
		TEXT("list_chooser_references"),
		TEXT("validate_chooser_table")
	};
	for (const FString& Action : ExpectedReadActions)
	{
		TestTrue(*FString::Printf(TEXT("chooser.%s is registered"), *Action),
			Registry.HasAction(TEXT("chooser"), Action));
	}

	const TArray<FMonolithActionInfo> Actions = Registry.GetActions(TEXT("chooser"));
	TestEqual(TEXT("Chooser namespace has the closed 16-action lifecycle"), Actions.Num(), 16);

	TSet<FString> UniqueNames;
	for (const FMonolithActionInfo& Action : Actions)
	{
		UniqueNames.Add(Action.Action);
	}
	TestEqual(TEXT("Chooser action names are unique"), UniqueNames.Num(), Actions.Num());

	// validate_chooser_table must NOT have replaced the compile-oriented validate_chooser.
	TestTrue(TEXT("The compile-oriented validate_chooser still exists alongside the read-only one"),
		Registry.HasAction(TEXT("chooser"), TEXT("validate_chooser")));

	const FMonolithActionInfo* ListAction = MonolithChooserReadTests::FindAction(Actions, TEXT("list_chooser_tables"));
	const FMonolithActionInfo* GetAction = MonolithChooserReadTests::FindAction(Actions, TEXT("get_chooser_table"));
	const FMonolithActionInfo* RowsAction = MonolithChooserReadTests::FindAction(Actions, TEXT("list_chooser_rows"));
	const FMonolithActionInfo* RefsAction = MonolithChooserReadTests::FindAction(Actions, TEXT("list_chooser_references"));

	TestTrue(TEXT("list_chooser_tables publishes bounded pagination"),
		ListAction && ListAction->ParamSchema.IsValid()
			&& ListAction->ParamSchema->HasField(TEXT("offset"))
			&& ListAction->ParamSchema->HasField(TEXT("limit")));
	TestTrue(TEXT("get_chooser_table requires asset_path and publishes a row bound"),
		GetAction && GetAction->ParamSchema.IsValid()
			&& GetAction->ParamSchema->HasField(TEXT("asset_path"))
			&& GetAction->ParamSchema->HasField(TEXT("row_limit")));
	TestTrue(TEXT("list_chooser_rows publishes start_row and limit"),
		RowsAction && RowsAction->ParamSchema.IsValid()
			&& RowsAction->ParamSchema->HasField(TEXT("start_row"))
			&& RowsAction->ParamSchema->HasField(TEXT("limit")));
	TestTrue(TEXT("list_chooser_references publishes bounded pagination"),
		RefsAction && RefsAction->ParamSchema.IsValid()
			&& RefsAction->ParamSchema->HasField(TEXT("offset"))
			&& RefsAction->ParamSchema->HasField(TEXT("limit")));
	return true;
}

// ---------------------------------------------------------------------------
// 2. Parameter guards — reject, never repair
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithChooserReadParamGuardTest,
	"Monolith.Chooser.Read.ParamGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithChooserReadParamGuardTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithChooserReadTests::RegisterChooserActions(Registry);

	// Each case: {action, param, value, why}. All must fail with ErrInvalidParams.
	struct FStringCase { const TCHAR* Action; const TCHAR* Param; const TCHAR* Value; const TCHAR* Why; };
	const FStringCase StringCases[] = {
		{ TEXT("get_chooser_table"),   TEXT("asset_path"),  TEXT("Game/Choosers/CHT_Invalid"),
			TEXT("Relative asset paths are rejected") },
		{ TEXT("get_chooser_table"),   TEXT("asset_path"),  TEXT("\\Game\\Choosers\\CHT_Invalid"),
			TEXT("Backslash asset paths are rejected without normalization") },
		{ TEXT("get_chooser_table"),   TEXT("asset_path"),  TEXT("/Game/Choosers/CHT_Invalid.uasset"),
			TEXT("Filesystem-style asset paths are rejected") },
		{ TEXT("get_chooser_table"),   TEXT("asset_path"),  TEXT("/Game/Choosers/CHT_Invalid.DifferentObject"),
			TEXT("Mismatched top-level object names are rejected") },
		{ TEXT("list_chooser_tables"), TEXT("path_filter"), TEXT("Game/Choosers"),
			TEXT("Relative list filters are rejected") },
		{ TEXT("list_chooser_tables"), TEXT("path_filter"), TEXT(" /Game/Choosers"),
			TEXT("Whitespace-padded list filters are rejected") },
	};
	for (const FStringCase& Case : StringCases)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(Case.Param, Case.Value);
		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("chooser"), Case.Action, Params);
		TestFalse(Case.Why, Result.bSuccess);
		TestEqual(*FString::Printf(TEXT("%s reports ErrInvalidParams"), Case.Why),
			Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("Game/Choosers/CHT_Invalid"));
		const FMonolithActionResult Result =
			Registry.ExecuteAction(TEXT("chooser"), TEXT("get_chooser_table"), Params);
		TestTrue(TEXT("Relative path error explains the canonical path contract"),
			Result.ErrorMessage.Contains(TEXT("canonical Unreal")));
	}

	// Out-of-range and wrong-typed scalars: a bad bound is an error, never a silent default.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 0);
		const FMonolithActionResult Result =
			Registry.ExecuteAction(TEXT("chooser"), TEXT("list_chooser_tables"), Params);
		TestFalse(TEXT("Zero list limit is rejected"), Result.bSuccess);
		TestEqual(TEXT("Limit range failure is ErrInvalidParams"),
			Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 100000);
		const FMonolithActionResult Result =
			Registry.ExecuteAction(TEXT("chooser"), TEXT("list_chooser_tables"), Params);
		TestFalse(TEXT("Over-ceiling list limit is rejected rather than clamped"), Result.bSuccess);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Missing/CHT_Missing.CHT_Missing"));
		Params->SetStringField(TEXT("row_limit"), TEXT("50"));
		const FMonolithActionResult Result =
			Registry.ExecuteAction(TEXT("chooser"), TEXT("get_chooser_table"), Params);
		TestFalse(TEXT("String row_limit is rejected"), Result.bSuccess);
		TestEqual(TEXT("String row_limit failure is ErrInvalidParams"),
			Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Missing/CHT_Missing.CHT_Missing"));
		Params->SetStringField(TEXT("include_rows"), TEXT("true"));
		const FMonolithActionResult Result =
			Registry.ExecuteAction(TEXT("chooser"), TEXT("get_chooser_table"), Params);
		TestFalse(TEXT("String include_rows is rejected"), Result.bSuccess);
		TestEqual(TEXT("String include_rows failure is ErrInvalidParams"),
			Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetBoolField(TEXT("asset_path"), true);
		const FMonolithActionResult Result =
			Registry.ExecuteAction(TEXT("chooser"), TEXT("get_chooser_table"), Params);
		TestFalse(TEXT("Boolean asset_path is rejected"), Result.bSuccess);
		TestEqual(TEXT("Boolean asset_path failure is ErrInvalidParams"),
			Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	}
	return true;
}

// ---------------------------------------------------------------------------
// 3. Empty table: exact readback, warnings-only validation, no dirtying
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithChooserReadEmptyValidationTest,
	"Monolith.Chooser.Read.EmptyTableValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithChooserReadEmptyValidationTest::RunTest(const FString& Parameters)
{
#if !MONOLITH_CHOOSER_READ_TESTS_ASSET_LANE
	AddInfo(TEXT("Chooser plugin or editor-only data unavailable for this target; asset readback is covered by the enabled-host lanes."));
	return true;
#else
	MonolithChooserReadTests::FChooserFixture Fixture = MonolithChooserReadTests::CreateFixture();
	if (!TestNotNull(TEXT("Creates a ChooserTable fixture"), Fixture.Table))
	{
		MonolithChooserReadTests::DiscardFixture(Fixture);
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithChooserReadTests::RegisterChooserActions(Registry);

	// A bare package path must resolve to, and report back, the canonical object path.
	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), Fixture.TablePackagePath);
	GetParams->SetBoolField(TEXT("include_rows"), true);
	const FMonolithActionResult GetResult =
		Registry.ExecuteAction(TEXT("chooser"), TEXT("get_chooser_table"), GetParams);
	TestTrue(TEXT("Package-path readback succeeds"), GetResult.bSuccess);
	if (GetResult.bSuccess)
	{
		TestEqual(TEXT("Readback returns the canonical object path"),
			GetResult.Result->GetStringField(TEXT("asset_path")), Fixture.TableObjectPath);
		TestEqual(TEXT("Empty table reports zero rows"),
			static_cast<int32>(GetResult.Result->GetNumberField(TEXT("row_count"))), 0);
		TestTrue(TEXT("An untruncated reference scan reports itself complete"),
			GetResult.Result->GetBoolField(TEXT("references_complete")));
	}

	TSharedPtr<FJsonObject> ValidateParams = MakeShared<FJsonObject>();
	ValidateParams->SetStringField(TEXT("asset_path"), Fixture.TableObjectPath);
	const FMonolithActionResult ValidateResult =
		Registry.ExecuteAction(TEXT("chooser"), TEXT("validate_chooser_table"), ValidateParams);
	TestTrue(TEXT("Empty-table validation executes"), ValidateResult.bSuccess);
	if (ValidateResult.bSuccess)
	{
		TestTrue(TEXT("Warnings alone do not make an empty table structurally invalid"),
			ValidateResult.Result->GetBoolField(TEXT("valid")));
		TestEqual(TEXT("Empty table has zero validation errors"),
			static_cast<int32>(ValidateResult.Result->GetNumberField(TEXT("error_count"))), 0);
		TestTrue(TEXT("Empty table reports advisory warnings"),
			ValidateResult.Result->GetNumberField(TEXT("warning_count")) >= 2.0);
		TestTrue(TEXT("A fully-walked empty table reports complete=true"),
			ValidateResult.Result->GetBoolField(TEXT("complete")));
	}

	TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
	ListParams->SetStringField(TEXT("path_filter"), MonolithChooserReadTests::TestFolder);
	const FMonolithActionResult ListResult =
		Registry.ExecuteAction(TEXT("chooser"), TEXT("list_chooser_tables"), ListParams);
	TestTrue(TEXT("Exact package-prefix discovery succeeds"), ListResult.bSuccess);
	if (ListResult.bSuccess)
	{
		TestTrue(TEXT("Discovery includes the in-memory ChooserTable fixture"),
			ListResult.Result->GetNumberField(TEXT("total")) >= 1.0);
		TestTrue(TEXT("Discovery reports the ChooserTable class as available"),
			ListResult.Result->GetBoolField(TEXT("available")));
	}

	TestFalse(TEXT("Read and validation actions preserve the package's clean state"),
		Fixture.TablePackage->IsDirty());
	MonolithChooserReadTests::DiscardFixture(Fixture);
	return true;
#endif
}

// ---------------------------------------------------------------------------
// 4. Authoring round-trip: counts, deprecated arrays, stale cooked data, bounds
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithChooserReadAuthoringRoundTripTest,
	"Monolith.Chooser.Read.AuthoringRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithChooserReadAuthoringRoundTripTest::RunTest(const FString& Parameters)
{
#if !MONOLITH_CHOOSER_READ_TESTS_ASSET_LANE
	AddInfo(TEXT("Chooser plugin or editor-only data unavailable for this target; the authoring round-trip is covered by the enabled-host lanes."));
	return true;
#else
	MonolithChooserReadTests::FChooserFixture Fixture = MonolithChooserReadTests::CreateFixture();
	if (!TestNotNull(TEXT("Creates a ChooserTable fixture"), Fixture.Table)
		|| !TestNotNull(TEXT("Creates a referenced output fixture"), Fixture.OutputAsset))
	{
		MonolithChooserReadTests::DiscardFixture(Fixture);
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithChooserReadTests::RegisterChooserActions(Registry);

	if (!TestTrue(TEXT("Existing authoring actions add one Bool column and nine aligned rows"),
		MonolithChooserReadTests::AuthorBoolColumnAndRows(Registry, Fixture, 9)))
	{
		MonolithChooserReadTests::DiscardFixture(Fixture);
		return false;
	}

	// CookedResults is DERIVED data and can be stale after an editor mutation. It must
	// never inflate the authoritative editor row count.
	Fixture.Table->CookedResults = Fixture.Table->ResultsStructs;
	Fixture.Table->CookedResults.AddDefaulted(3);
	Fixture.TablePackage->SetDirtyFlag(false);

	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), Fixture.TableObjectPath);
	GetParams->SetBoolField(TEXT("include_rows"), true);
	const FMonolithActionResult GetResult =
		Registry.ExecuteAction(TEXT("chooser"), TEXT("get_chooser_table"), GetParams);
	TestTrue(TEXT("Authored table readback succeeds"), GetResult.bSuccess);
	if (GetResult.bSuccess)
	{
		TestEqual(TEXT("Readback reports nine rows"),
			static_cast<int32>(GetResult.Result->GetNumberField(TEXT("row_count"))), 9);
		TestEqual(TEXT("Readback exposes twelve stale cooked results without treating them as rows"),
			static_cast<int32>(GetResult.Result->GetNumberField(TEXT("cooked_result_count"))), 12);
		TestEqual(TEXT("Readback reports one column"),
			static_cast<int32>(GetResult.Result->GetNumberField(TEXT("column_count"))), 1);
		TestFalse(TEXT("A one-column table is not reported as cell-truncated"),
			GetResult.Result->GetBoolField(TEXT("row_cells_truncated")));
	}

	TSharedPtr<FJsonObject> ReadParams = MakeShared<FJsonObject>();
	ReadParams->SetStringField(TEXT("asset_path"), Fixture.TableObjectPath);
	const FMonolithActionResult ColumnsResult =
		Registry.ExecuteAction(TEXT("chooser"), TEXT("list_chooser_columns"), ReadParams);
	const FMonolithActionResult RowsResult =
		Registry.ExecuteAction(TEXT("chooser"), TEXT("list_chooser_rows"), ReadParams);
	const FMonolithActionResult ReferencesResult =
		Registry.ExecuteAction(TEXT("chooser"), TEXT("list_chooser_references"), ReadParams);
	const FMonolithActionResult ValidateResult =
		Registry.ExecuteAction(TEXT("chooser"), TEXT("validate_chooser_table"), ReadParams);

	TestTrue(TEXT("Column readback succeeds"), ColumnsResult.bSuccess);
	TestTrue(TEXT("Row readback succeeds"), RowsResult.bSuccess);
	TestTrue(TEXT("Reference readback succeeds"), ReferencesResult.bSuccess);
	TestTrue(TEXT("Structural validation succeeds"), ValidateResult.bSuccess);

	if (ColumnsResult.bSuccess)
	{
		TestEqual(TEXT("Column readback returns one column"),
			static_cast<int32>(ColumnsResult.Result->GetNumberField(TEXT("count"))), 1);
		const TArray<TSharedPtr<FJsonValue>>& Columns = ColumnsResult.Result->GetArrayField(TEXT("columns"));
		const TSharedPtr<FJsonObject>* FirstColumn = nullptr;
		const bool bHasColumn = Columns.Num() == 1 && Columns[0].IsValid()
			&& Columns[0]->TryGetObject(FirstColumn) && FirstColumn && FirstColumn->IsValid();
		TestTrue(TEXT("Column readback returns an object summary"), bHasColumn);
		if (bHasColumn)
		{
			// FBoolColumn's live array is RowValuesWithAny; UHT registers the migrated-away
			// TArray<bool> under the name "RowValues" with CPF_Deprecated. Reading the
			// deprecated one would report zero cells for every bool column.
			TestEqual(TEXT("Bool column readback ignores the deprecated RowValues array"),
				static_cast<int32>((*FirstColumn)->GetNumberField(TEXT("row_value_count"))), 9);
			TestFalse(TEXT("An input column is not classified as an output column"),
				(*FirstColumn)->GetBoolField(TEXT("is_output")));

			FString RowValuesProperty;
			const TSharedPtr<FJsonObject>* Fields = nullptr;
			const TSharedPtr<FJsonObject>* SerializedRowValues = nullptr;
			const bool bHasBoundedRowValues =
				(*FirstColumn)->TryGetStringField(TEXT("row_values_property"), RowValuesProperty)
				&& (*FirstColumn)->TryGetObjectField(TEXT("fields"), Fields) && Fields && Fields->IsValid()
				&& (*Fields)->TryGetObjectField(RowValuesProperty, SerializedRowValues)
				&& SerializedRowValues && SerializedRowValues->IsValid();
			TestTrue(TEXT("Compact column fields expose the active bounded row-value container"),
				bHasBoundedRowValues);
			if (bHasBoundedRowValues)
			{
				TestEqual(TEXT("Bounded serializer preserves the full container count"),
					static_cast<int32>((*SerializedRowValues)->GetNumberField(TEXT("count"))), 9);
				TestEqual(TEXT("Compact serializer emits at most eight row values"),
					(*SerializedRowValues)->GetArrayField(TEXT("items")).Num(), 8);
				TestEqual(TEXT("Compact serializer reports its truncation boundary"),
					static_cast<int32>((*SerializedRowValues)->GetNumberField(TEXT("truncated_after"))), 8);
			}
		}
	}
	if (RowsResult.bSuccess)
	{
		TestEqual(TEXT("Row readback returns nine rows"),
			static_cast<int32>(RowsResult.Result->GetNumberField(TEXT("count"))), 9);
		TestFalse(TEXT("A complete row page reports has_more=false"),
			RowsResult.Result->GetBoolField(TEXT("has_more")));
	}
	if (ReferencesResult.bSuccess)
	{
		TestTrue(TEXT("Reference readback sees the authored output asset"),
			ReferencesResult.Result->GetNumberField(TEXT("total")) >= 1.0);
		TestTrue(TEXT("A bounded reference scan of a small table reports itself complete"),
			ReferencesResult.Result->GetBoolField(TEXT("scan_complete")));
	}
	if (ValidateResult.bSuccess)
	{
		const FString IssueCodes = MonolithChooserReadTests::JoinIssueCodes(ValidateResult.Result);
		TestTrue(*FString::Printf(TEXT("Aligned authored table validates cleanly (issues: %s)"), *IssueCodes),
			ValidateResult.Result->GetBoolField(TEXT("valid")));
		TestEqual(TEXT("Aligned table has no validation errors"),
			static_cast<int32>(ValidateResult.Result->GetNumberField(TEXT("error_count"))), 0);
	}

	TestFalse(TEXT("All readback actions preserve the package's clean state"),
		Fixture.TablePackage->IsDirty());
	MonolithChooserReadTests::DiscardFixture(Fixture);
	return true;
#endif
}

// ---------------------------------------------------------------------------
// 5. Root-chooser context view + result-payload validation
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithChooserReadRootContextAndResultPayloadTest,
	"Monolith.Chooser.Read.RootContextAndResultPayloadValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithChooserReadRootContextAndResultPayloadTest::RunTest(const FString& Parameters)
{
#if !MONOLITH_CHOOSER_READ_TESTS_ASSET_LANE
	AddInfo(TEXT("Chooser plugin or editor-only data unavailable for this target; root-context and payload validation are covered by the enabled-host lanes."));
	return true;
#else
	MonolithChooserReadTests::FChooserFixture Fixture = MonolithChooserReadTests::CreateFixture();
	if (!TestNotNull(TEXT("Creates a root ChooserTable fixture"), Fixture.Table))
	{
		MonolithChooserReadTests::DiscardFixture(Fixture);
		return false;
	}

	// Row 0: a default-constructed (invalid) result struct. Row 1: a known result kind
	// whose target is unset. Both must be reported, with distinct codes.
	Fixture.Table->ContextData.AddDefaulted();
	Fixture.Table->ResultsStructs.AddDefaulted();
	Fixture.Table->DisabledRows.Add(false);
	Fixture.Table->ResultsStructs.Add(FInstancedStruct::Make<FSoftAssetChooser>());
	Fixture.Table->DisabledRows.Add(false);

	UPackage* ChildPackage = nullptr;
	FString ChildObjectPath;
	UChooserTable* Child = MonolithChooserReadTests::MakeDisposableAsset<UChooserTable>(
		TEXT("CHT_ReadChild_"), ChildPackage, ChildObjectPath);
	if (!TestNotNull(TEXT("Creates a child ChooserTable fixture"), Child))
	{
		MonolithChooserReadTests::DiscardFixture(Fixture);
		return false;
	}
	Child->RootChooser = Fixture.Table;
	Fixture.TablePackage->SetDirtyFlag(false);
	ChildPackage->SetDirtyFlag(false);

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithChooserReadTests::RegisterChooserActions(Registry);

	TSharedPtr<FJsonObject> ChildParams = MakeShared<FJsonObject>();
	ChildParams->SetStringField(TEXT("asset_path"), ChildObjectPath);
	const FMonolithActionResult ChildResult =
		Registry.ExecuteAction(TEXT("chooser"), TEXT("get_chooser_table"), ChildParams);
	TestTrue(TEXT("Child table readback succeeds"), ChildResult.bSuccess);
	if (ChildResult.bSuccess)
	{
		// Context parameters are owned by the ROOT chooser, not the child.
		TestEqual(TEXT("Child readback uses the root chooser's context view"),
			static_cast<int32>(ChildResult.Result->GetNumberField(TEXT("context_entry_count"))), 1);
	}

	TSharedPtr<FJsonObject> RootParams = MakeShared<FJsonObject>();
	RootParams->SetStringField(TEXT("asset_path"), Fixture.TableObjectPath);
	const FMonolithActionResult ValidateResult =
		Registry.ExecuteAction(TEXT("chooser"), TEXT("validate_chooser_table"), RootParams);
	TestTrue(TEXT("Result-payload validation executes"), ValidateResult.bSuccess);
	if (ValidateResult.bSuccess)
	{
		const FString IssueCodes = MonolithChooserReadTests::JoinIssueCodes(ValidateResult.Result);
		TestFalse(TEXT("Invalid and null result payloads make validation fail"),
			ValidateResult.Result->GetBoolField(TEXT("valid")));
		TestTrue(*FString::Printf(TEXT("Invalid result struct is reported (issues: %s)"), *IssueCodes),
			IssueCodes.Contains(TEXT("invalid_result_struct")));
		TestTrue(*FString::Printf(TEXT("Known result type with a null target is reported (issues: %s)"), *IssueCodes),
			IssueCodes.Contains(TEXT("invalid_result_payload")));
	}

	TestFalse(TEXT("Readback preserves the root package's clean state"), Fixture.TablePackage->IsDirty());
	TestFalse(TEXT("Readback preserves the child package's clean state"), ChildPackage->IsDirty());
	MonolithChooserReadTests::DiscardAsset(Child, ChildPackage);
	MonolithChooserReadTests::DiscardFixture(Fixture);
	return true;
#endif
}

// ---------------------------------------------------------------------------
// 6. Exact existence evidence: an empty package shell is not an asset
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithChooserReadDeletedAssetPackageShellTest,
	"Monolith.Chooser.Read.DeletedAssetPackageShell",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithChooserReadDeletedAssetPackageShellTest::RunTest(const FString& Parameters)
{
#if !MONOLITH_CHOOSER_READ_TESTS_ASSET_LANE
	AddInfo(TEXT("Chooser plugin or editor-only data unavailable for this target; exact reference evidence is covered by the enabled-host lanes."));
	return true;
#else
	MonolithChooserReadTests::FChooserFixture Fixture = MonolithChooserReadTests::CreateFixture();
	if (!TestNotNull(TEXT("Creates a ChooserTable fixture"), Fixture.Table)
		|| !TestNotNull(TEXT("Creates a referenced output fixture"), Fixture.OutputAsset))
	{
		MonolithChooserReadTests::DiscardFixture(Fixture);
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithChooserReadTests::RegisterChooserActions(Registry);

	if (!TestTrue(TEXT("Adds the fixture input column and result row"),
			MonolithChooserReadTests::AuthorBoolColumnAndRows(Registry, Fixture, 1))
		|| !TestTrue(TEXT("Fixture row contains a mutable result struct"),
			Fixture.Table->ResultsStructs.IsValidIndex(0)))
	{
		MonolithChooserReadTests::DiscardFixture(Fixture);
		return false;
	}

	// A loaded but EMPTY UPackage with no on-disk package and no export. Package residency
	// must not be mistaken for asset existence.
	const FString MissingAssetName =
		TEXT("Curve_MissingChooserOutput_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString MissingPackageName =
		FString(MonolithChooserReadTests::TestFolder) + TEXT("/") + MissingAssetName;
	const FString MissingObjectPath = MissingPackageName + TEXT(".") + MissingAssetName;
	UPackage* MissingPackage = CreatePackage(*MissingPackageName);
	if (!TestNotNull(TEXT("Creates the empty package shell used by the regression"), MissingPackage))
	{
		MonolithChooserReadTests::DiscardFixture(Fixture);
		return false;
	}

	FInstancedStruct& ResultStruct = Fixture.Table->ResultsStructs[0];
	ResultStruct.InitializeAs(FSoftAssetChooser::StaticStruct());
	ResultStruct.GetMutable<FSoftAssetChooser>().Asset = TSoftObjectPtr<UObject>(FSoftObjectPath(MissingObjectPath));
	Fixture.TablePackage->SetDirtyFlag(false);
	MissingPackage->SetDirtyFlag(false);

	TestNotNull(TEXT("The missing asset's empty UPackage shell is loaded"),
		FindPackage(nullptr, *MissingPackageName));
	TestFalse(TEXT("The empty package shell has no on-disk package"),
		FPackageName::DoesPackageExist(MissingPackageName));

	TSharedPtr<FJsonObject> ReadParams = MakeShared<FJsonObject>();
	ReadParams->SetStringField(TEXT("asset_path"), Fixture.TableObjectPath);
	const FMonolithActionResult ReferencesResult =
		Registry.ExecuteAction(TEXT("chooser"), TEXT("list_chooser_references"), ReadParams);
	const FMonolithActionResult ValidateResult =
		Registry.ExecuteAction(TEXT("chooser"), TEXT("validate_chooser_table"), ReadParams);
	if (!TestTrue(TEXT("Reference readback succeeds for a missing soft target"), ReferencesResult.bSuccess)
		|| !TestTrue(TEXT("Structural validation executes for a missing soft target"), ValidateResult.bSuccess))
	{
		MonolithChooserReadTests::DiscardFixture(Fixture);
		return false;
	}

	bool bFoundMissingReference = false;
	bool bMissingReferenceExists = true;
	for (const TSharedPtr<FJsonValue>& Value : ReferencesResult.Result->GetArrayField(TEXT("references")))
	{
		const TSharedPtr<FJsonObject>* Reference = nullptr;
		FString ReferencePath;
		if (Value.IsValid() && Value->TryGetObject(Reference) && Reference && Reference->IsValid()
			&& (*Reference)->TryGetStringField(TEXT("path"), ReferencePath)
			&& ReferencePath.Equals(MissingObjectPath, ESearchCase::CaseSensitive))
		{
			bFoundMissingReference = true;
			(*Reference)->TryGetBoolField(TEXT("exists"), bMissingReferenceExists);
			break;
		}
	}

	TestTrue(TEXT("Reference readback retains the missing soft path"), bFoundMissingReference);
	TestFalse(TEXT("A loaded empty package shell is not accepted as asset existence"),
		bMissingReferenceExists);
	TestFalse(TEXT("Missing soft target makes structural validation invalid"),
		ValidateResult.Result->GetBoolField(TEXT("valid")));
	TestTrue(TEXT("Missing soft target reports unresolved_soft_reference"),
		MonolithChooserReadTests::JoinIssueCodes(ValidateResult.Result).Contains(
			TEXT("unresolved_soft_reference")));
	TestFalse(TEXT("Reference readback and validation preserve the table package's clean state"),
		Fixture.TablePackage->IsDirty());

	MissingPackage->SetDirtyFlag(false);
	MonolithChooserReadTests::DiscardFixture(Fixture);
	return true;
#endif
}

#undef MONOLITH_CHOOSER_READ_TESTS_ASSET_LANE

#endif // WITH_DEV_AUTOMATION_TESTS
