// SPDX-License-Identifier: MIT
// Automation tests for project search error classification and input validation.
// Plan: Plugins/Monolith/Docs/plans/2026-08-01-pr-issue-sweep.md (Phase 1, T06/T07)
//
// Goals:
//   - Column-qualified search (`asset_name:` / `node_name:`) returns results.
//     This is the regression lock. It used to work by ACCIDENT: the asset query
//     errored with `no such column`, `while (Step() == Row)` silently swallowed
//     that as end-of-results, and the node query then answered. Classifying every
//     per-table error as a hard failure would have turned a working feature into
//     a -32602. It now works BY DESIGN via the not-applicable tier -- see
//     SPEC_MonolithIndex.md "Search Error Contract".
//   - A genuine FTS5 syntax error is InvalidQuery, not zero results.
//   - A column no table exposes is InvalidQuery, and the message names it.
//   - A valid query with no matches is still a SUCCESS.
//   - Action-level validation (query type/empty/length, limit type) is -32602.
//
// The database tests use their own temp SQLite file, so they need no project
// index, no editor subsystem and no indexing state -- they are deterministic.
//
// Lives under Private/Tests/ for the same UBT auto-include reason as the
// MonolithCore tests in that module's folder.

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Math/NumericLimits.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Dom/JsonObject.h"
#include "Actions/ProjectSearchAction.h"
#include "MonolithIndexDatabase.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithProjectSearchTestDetail
{
	// Single unambiguous tokens. `unicode61` splits on underscores and `porter`
	// stems, but both sides of the comparison go through the same tokenizer.
	static const TCHAR* AssetToken = TEXT("MonolithSearchFixtureAsset");
	static const TCHAR* NodeToken = TEXT("MonolithSearchFixtureNode");

	/** Temp index database seeded with one asset and one graph node. */
	struct FFixture
	{
		FMonolithIndexDatabase Database;
		FString DbPath;

		bool Open()
		{
			DbPath = FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("MonolithTests"),
				FString::Printf(TEXT("ProjectSearch_%s.db"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

			if (!Database.Open(DbPath))
			{
				return false;
			}

			FIndexedAsset Asset;
			Asset.PackagePath = TEXT("/Game/Tests/Monolith/MonolithSearchFixtureAsset");
			Asset.AssetName = AssetToken;
			Asset.AssetClass = TEXT("Blueprint");
			Asset.ModuleName = TEXT("Game");
			Asset.Description = TEXT("project search fixture");
			const int64 AssetId = Database.InsertAsset(Asset);
			if (AssetId <= 0)
			{
				return false;
			}

			FIndexedNode Node;
			Node.AssetId = AssetId;
			Node.NodeName = NodeToken;
			Node.NodeClass = TEXT("K2Node_IfThenElse");
			Node.NodeType = TEXT("flow");
			return Database.InsertNode(Node) > 0;
		}

		void Destroy()
		{
			Database.Close();
			if (!DbPath.IsEmpty())
			{
				FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*DbPath);
			}
		}
	};

	/** Run the action with a params object built by the caller. */
	static FMonolithActionResult InvokeAction(const TSharedPtr<FJsonObject>& Params)
	{
		return FProjectSearchAction::Execute(Params);
	}
}

// ---------------------------------------------------------------------------
// Test 1: column-qualified search still returns results (the regression lock).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithProjectSearchColumnQualifiedTest,
	"Monolith.ProjectSearch.ColumnQualified",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithProjectSearchColumnQualifiedTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithProjectSearchTestDetail;

	FFixture Fixture;
	if (!TestTrue(TEXT("fixture database opens and seeds"), Fixture.Open()))
	{
		Fixture.Destroy();
		return false;
	}
	ON_SCOPE_EXIT { Fixture.Destroy(); };

	TArray<FSearchResult> Results;
	FString Error;

	// An asset-column filter: fts_nodes has no `asset_name`, so it reports
	// `no such column` and is skipped rather than failing the whole search.
	const EMonolithProjectSearchStatus AssetStatus = Fixture.Database.FullTextSearch(
		FString::Printf(TEXT("asset_name:%s"), AssetToken), 50, Results, Error);
	TestEqual(TEXT("asset_name: filter succeeds"), AssetStatus, EMonolithProjectSearchStatus::Succeeded);
	TestTrue(TEXT("asset_name: filter returns the fixture asset"), Results.Num() > 0);

	// The mirror image: fts_assets has no `node_name`.
	const EMonolithProjectSearchStatus NodeStatus = Fixture.Database.FullTextSearch(
		FString::Printf(TEXT("node_name:%s"), NodeToken), 50, Results, Error);
	TestEqual(TEXT("node_name: filter succeeds"), NodeStatus, EMonolithProjectSearchStatus::Succeeded);
	TestTrue(TEXT("node_name: filter returns the fixture node"), Results.Num() > 0);

	// A filter spanning both tables cannot be answered by either one.
	const EMonolithProjectSearchStatus SpanStatus = Fixture.Database.FullTextSearch(
		FString::Printf(TEXT("asset_name:%s OR node_name:%s"), AssetToken, NodeToken), 50, Results, Error);
	TestEqual(TEXT("cross-table filter is an invalid query"), SpanStatus, EMonolithProjectSearchStatus::InvalidQuery);
	TestEqual(TEXT("cross-table filter yields no partial results"), Results.Num(), 0);

	return true;
}

// ---------------------------------------------------------------------------
// Test 2: error classification -- syntax errors and unknown columns are caller
// errors; a valid query with no matches is a success.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithProjectSearchErrorClassificationTest,
	"Monolith.ProjectSearch.ErrorClassification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithProjectSearchErrorClassificationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithProjectSearchTestDetail;

	FFixture Fixture;
	if (!TestTrue(TEXT("fixture database opens and seeds"), Fixture.Open()))
	{
		Fixture.Destroy();
		return false;
	}
	ON_SCOPE_EXIT { Fixture.Destroy(); };

	TArray<FSearchResult> Results;
	FString Error;

	// Unbalanced parenthesis: "fts5: syntax error near ...".
	const EMonolithProjectSearchStatus UnbalancedStatus = Fixture.Database.FullTextSearch(
		FString::Printf(TEXT("%s AND ("), AssetToken), 50, Results, Error);
	TestEqual(TEXT("unbalanced paren is an invalid query"), UnbalancedStatus, EMonolithProjectSearchStatus::InvalidQuery);
	TestFalse(TEXT("invalid query reports a reason"), Error.IsEmpty());

	// `NEAR/3` is not FTS5 syntax. It was documented for years and always errored;
	// this asserts it errors LOUDLY rather than returning zero results.
	const EMonolithProjectSearchStatus BadNearStatus = Fixture.Database.FullTextSearch(
		FString::Printf(TEXT("%s NEAR/3 Other"), AssetToken), 50, Results, Error);
	TestEqual(TEXT("NEAR/3 is an invalid query"), BadNearStatus, EMonolithProjectSearchStatus::InvalidQuery);

	// The form that actually parses.
	const EMonolithProjectSearchStatus GoodNearStatus = Fixture.Database.FullTextSearch(
		FString::Printf(TEXT("NEAR(%s Other, 3)"), AssetToken), 50, Results, Error);
	TestEqual(TEXT("NEAR(a b, N) is valid"), GoodNearStatus, EMonolithProjectSearchStatus::Succeeded);

	// A column no table exposes -- and the message must name it, so a typo is a
	// five-second fix rather than a filed issue.
	const EMonolithProjectSearchStatus UnknownColumnStatus = Fixture.Database.FullTextSearch(
		TEXT("monolith_no_such_column:Anything"), 50, Results, Error);
	TestEqual(TEXT("unknown column is an invalid query"), UnknownColumnStatus, EMonolithProjectSearchStatus::InvalidQuery);
	TestTrue(TEXT("unknown-column error names the offending column"),
		Error.Contains(TEXT("monolith_no_such_column")));
	TestTrue(TEXT("unknown-column error lists the valid asset columns"),
		Error.Contains(TEXT("package_path")));
	TestTrue(TEXT("unknown-column error lists the valid node columns"),
		Error.Contains(TEXT("node_class")));

	// A valid query that simply matches nothing is NOT an error.
	const EMonolithProjectSearchStatus NoMatchStatus = Fixture.Database.FullTextSearch(
		TEXT("MonolithSearchFixtureNothingMatchesThis"), 50, Results, Error);
	TestEqual(TEXT("zero matches is a success"), NoMatchStatus, EMonolithProjectSearchStatus::Succeeded);
	TestEqual(TEXT("zero matches returns no results"), Results.Num(), 0);

	// A closed database is an INTERNAL failure, not a caller error.
	FMonolithIndexDatabase Unopened;
	const EMonolithProjectSearchStatus ClosedStatus =
		Unopened.FullTextSearch(TEXT("Anything"), 50, Results, Error);
	TestEqual(TEXT("closed database is an internal error"), ClosedStatus, EMonolithProjectSearchStatus::InternalError);

	return true;
}

// ---------------------------------------------------------------------------
// Test 3: `limit` is clamped to 1-1000 rather than passed through.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithProjectSearchLimitClampTest,
	"Monolith.ProjectSearch.LimitClamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithProjectSearchLimitClampTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithProjectSearchTestDetail;

	FFixture Fixture;
	if (!TestTrue(TEXT("fixture database opens and seeds"), Fixture.Open()))
	{
		Fixture.Destroy();
		return false;
	}
	ON_SCOPE_EXIT { Fixture.Destroy(); };

	TArray<FSearchResult> Results;
	FString Error;

	// limit=0 used to reach SQLite as `LIMIT 0`, which returns nothing at all.
	const EMonolithProjectSearchStatus ZeroStatus =
		Fixture.Database.FullTextSearch(AssetToken, 0, Results, Error);
	TestEqual(TEXT("limit 0 succeeds"), ZeroStatus, EMonolithProjectSearchStatus::Succeeded);
	TestTrue(TEXT("limit 0 clamps up to 1 rather than returning nothing"), Results.Num() > 0);

	// A negative limit is clamped the same way.
	const EMonolithProjectSearchStatus NegativeStatus =
		Fixture.Database.FullTextSearch(AssetToken, -5, Results, Error);
	TestEqual(TEXT("negative limit succeeds"), NegativeStatus, EMonolithProjectSearchStatus::Succeeded);
	TestTrue(TEXT("negative limit clamps up to 1"), Results.Num() > 0);

	// The top of the range must not overflow the bound LIMIT parameter.
	const EMonolithProjectSearchStatus HugeStatus =
		Fixture.Database.FullTextSearch(AssetToken, TNumericLimits<int32>::Max(), Results, Error);
	TestEqual(TEXT("huge limit succeeds"), HugeStatus, EMonolithProjectSearchStatus::Succeeded);
	TestTrue(TEXT("huge limit still returns the match"), Results.Num() > 0);
	TestTrue(TEXT("huge limit is clamped to 1000"), Results.Num() <= 1000);

	return true;
}

// ---------------------------------------------------------------------------
// Test 4: action-level parameter validation is -32602.
// Every case here returns before the action touches GEditor or the subsystem,
// so this test needs no index and no editor state.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithProjectSearchValidationTest,
	"Monolith.ProjectSearch.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithProjectSearchValidationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithProjectSearchTestDetail;

	auto ExpectInvalidParams = [this](const TCHAR* What, const TSharedPtr<FJsonObject>& Params)
	{
		const FMonolithActionResult R = InvokeAction(Params);
		TestFalse(FString::Printf(TEXT("%s fails"), What), R.bSuccess);
		TestEqual(FString::Printf(TEXT("%s is -32602"), What), R.ErrorCode, -32602);
	};

	// Missing `query`.
	ExpectInvalidParams(TEXT("missing query"), MakeShared<FJsonObject>());

	// Whitespace-only `query` trims to empty.
	TSharedPtr<FJsonObject> Blank = MakeShared<FJsonObject>();
	Blank->SetStringField(TEXT("query"), TEXT("   \t  "));
	ExpectInvalidParams(TEXT("whitespace-only query"), Blank);

	// Over the 4096-character cap. The cap bounds the parse work a single call
	// can hand the game thread, where FullTextSearch runs.
	TSharedPtr<FJsonObject> TooLong = MakeShared<FJsonObject>();
	TooLong->SetStringField(TEXT("query"), FString::ChrN(4097, TEXT('a')));
	ExpectInvalidParams(TEXT("over-length query"), TooLong);

	// Exactly at the cap is accepted by validation (it proceeds past this point,
	// so we only assert it is not rejected as a bad parameter).
	TSharedPtr<FJsonObject> AtCap = MakeShared<FJsonObject>();
	AtCap->SetStringField(TEXT("query"), FString::ChrN(4096, TEXT('a')));
	const FMonolithActionResult AtCapResult = InvokeAction(AtCap);
	TestNotEqual(TEXT("query exactly at the cap is not a length rejection"),
		AtCapResult.ErrorCode, -32602);

	// Non-integer `limit`.
	TSharedPtr<FJsonObject> FractionalLimit = MakeShared<FJsonObject>();
	FractionalLimit->SetStringField(TEXT("query"), AssetToken);
	FractionalLimit->SetNumberField(TEXT("limit"), 2.5);
	ExpectInvalidParams(TEXT("fractional limit"), FractionalLimit);

	// Non-numeric `limit`.
	TSharedPtr<FJsonObject> TextLimit = MakeShared<FJsonObject>();
	TextLimit->SetStringField(TEXT("query"), AssetToken);
	TextLimit->SetStringField(TEXT("limit"), TEXT("lots"));
	ExpectInvalidParams(TEXT("non-numeric limit"), TextLimit);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
