// Automation coverage for gameplay_tags find_broken's package walk. The walk once
// called GetDependencies from inside EnumerateAllPackages, which deadlocks as soon
// as the asset registry gatherer queues a write between the two reads. That race
// cannot be forced from a test, so these check the walk itself: a full scan
// returns, and path_prefix narrows the packages it visits.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Dom/JsonObject.h"
#include "MonolithToolRegistry.h"

namespace MonolithGameplayTagsFindBrokenTestDetail
{
	FMonolithActionResult RunFindBroken(const FString& PathPrefix)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		if (!PathPrefix.IsEmpty())
		{
			Params->SetStringField(TEXT("path_prefix"), PathPrefix);
		}
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("gameplay_tags"), TEXT("find_broken"), Params);
	}

	int32 IntField(const FMonolithActionResult& Result, const TCHAR* Field)
	{
		int32 Value = -1;
		if (Result.Result.IsValid())
		{
			Result.Result->TryGetNumberField(Field, Value);
		}
		return Value;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGameplayTagsFindBrokenScanTest,
	"Monolith.GameplayTags.FindBroken.FullScan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameplayTagsFindBrokenScanTest::RunTest(const FString& Parameters)
{
	using namespace MonolithGameplayTagsFindBrokenTestDetail;

	const FMonolithActionResult Result = RunFindBroken(FString());
	if (!TestTrue(TEXT("find_broken with no path_prefix returns success"), Result.bSuccess))
	{
		AddError(Result.ErrorMessage);
		return false;
	}

	const int32 Scanned = IntField(Result, TEXT("packages_scanned"));
	TestTrue(TEXT("the walk visits the registry's packages"), Scanned > 0);
	TestTrue(TEXT("packages carrying tags are a subset of those scanned"),
		IntField(Result, TEXT("packages_with_tags")) >= 0 && IntField(Result, TEXT("packages_with_tags")) <= Scanned);
	TestTrue(TEXT("returned never exceeds total"),
		IntField(Result, TEXT("returned")) <= IntField(Result, TEXT("total")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGameplayTagsFindBrokenPrefixTest,
	"Monolith.GameplayTags.FindBroken.PathPrefix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameplayTagsFindBrokenPrefixTest::RunTest(const FString& Parameters)
{
	using namespace MonolithGameplayTagsFindBrokenTestDetail;

	// The gatherer may still be adding packages, so the narrower scan runs first.
	const FMonolithActionResult Engine = RunFindBroken(TEXT("/Engine/"));
	const FMonolithActionResult All = RunFindBroken(FString());
	const FMonolithActionResult None = RunFindBroken(TEXT("/MonolithNoSuchMount/"));
	if (!TestTrue(TEXT("every call returns success"), All.bSuccess && Engine.bSuccess && None.bSuccess))
	{
		return false;
	}

	const int32 EngineScanned = IntField(Engine, TEXT("packages_scanned"));
	TestTrue(TEXT("/Engine/ prefix visits engine content"), EngineScanned > 0);
	TestTrue(TEXT("/Engine/ prefix visits no more than the full scan"),
		EngineScanned <= IntField(All, TEXT("packages_scanned")));
	TestEqual(TEXT("a prefix matching no package scans nothing"), IntField(None, TEXT("packages_scanned")), 0);
	TestEqual(TEXT("a prefix matching no package reports no broken tag"), IntField(None, TEXT("total")), 0);
	return true;
}

#endif
