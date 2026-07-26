#include "Misc/AutomationTest.h"
#include "MonolithUpdateReleaseSelector.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithUpdateReleaseSelectorTest
{
	static TSharedPtr<FJsonValue> MakeAsset(const FString& Name, const FString& Url)
	{
		TSharedPtr<FJsonObject> Asset = MakeShared<FJsonObject>();
		Asset->SetStringField(TEXT("name"), Name);
		if (!Url.IsEmpty())
		{
			Asset->SetStringField(TEXT("browser_download_url"), Url);
		}
		return MakeShared<FJsonValueObject>(Asset);
	}

	static TSharedPtr<FJsonObject> MakeRelease(
		const TArray<TSharedPtr<FJsonValue>>& Assets,
		const FString& ZipballUrl = FString())
	{
		TSharedPtr<FJsonObject> Release = MakeShared<FJsonObject>();
		Release->SetArrayField(TEXT("assets"), Assets);
		if (!ZipballUrl.IsEmpty())
		{
			Release->SetStringField(TEXT("zipball_url"), ZipballUrl);
		}
		return Release;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUpdaterBinaryZipSelectionTest,
	"Monolith.Updater.ReleaseSelection.BinaryZipAccepted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUpdaterBinaryZipSelectionTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithUpdateReleaseSelectorTest;

	const FString BinaryUrl = TEXT("https://example.invalid/Monolith-v1.2.3.zip");
	const FMonolithReleaseZipSelection Selection =
		MonolithUpdateReleaseSelector::SelectBinaryZip(
			MakeRelease(
				{MakeAsset(TEXT("Monolith-v1.2.3.zip"), BinaryUrl)},
				TEXT("https://api.github.invalid/source.zip")),
			TEXT("UE5.8"));

	TestTrue(TEXT("explicit release zip is accepted"), Selection.IsSuccess());
	TestEqual(TEXT("explicit release asset wins over zipball_url"), Selection.Url, BinaryUrl);
	TestFalse(TEXT("legacy zip is not engine-tagged"), Selection.bEngineTagged);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUpdaterUnrelatedZipFailsClosedTest,
	"Monolith.Updater.ReleaseSelection.UnrelatedZipDoesNotOfferInstall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUpdaterUnrelatedZipFailsClosedTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithUpdateReleaseSelectorTest;

	const FMonolithReleaseZipSelection Selection =
		MonolithUpdateReleaseSelector::SelectBinaryZip(
			MakeRelease({
				MakeAsset(
					TEXT("SourcePackage.zip"),
					TEXT("https://example.invalid/SourcePackage.zip"))
			}),
			TEXT("UE5.8"));

	TestFalse(TEXT("an unrelated zip does not offer an install"), Selection.IsSuccess());
	TestTrue(
		TEXT("failure reports that no Monolith binary release asset exists"),
		Selection.Failure == EMonolithReleaseZipFailure::NoBinaryZipAsset);
	TestTrue(TEXT("no unrelated download URL escapes the decision"), Selection.Url.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUpdaterEngineMismatchFailsClosedTest,
	"Monolith.Updater.ReleaseSelection.EngineMismatchFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUpdaterEngineMismatchFailsClosedTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithUpdateReleaseSelectorTest;

	const FMonolithReleaseZipSelection Selection =
		MonolithUpdateReleaseSelector::SelectBinaryZip(
			MakeRelease({
				MakeAsset(
					TEXT("Monolith-v1.2.3-UE5.7.zip"),
					TEXT("https://example.invalid/Monolith-v1.2.3-UE5.7.zip"))
			}),
			TEXT("UE5.8"));

	TestFalse(TEXT("wrong-engine release does not offer an install"), Selection.IsSuccess());
	TestTrue(
		TEXT("failure identifies the missing engine build"),
		Selection.Failure == EMonolithReleaseZipFailure::NoMatchingEngineAsset);
	TestTrue(TEXT("no download URL escapes the failed decision"), Selection.Url.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUpdaterEngineAssetPrefixRequiredTest,
	"Monolith.Updater.ReleaseSelection.EngineAssetRequiresMonolithPrefix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUpdaterEngineAssetPrefixRequiredTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithUpdateReleaseSelectorTest;

	const FMonolithReleaseZipSelection Selection =
		MonolithUpdateReleaseSelector::SelectBinaryZip(
			MakeRelease({
				MakeAsset(
					TEXT("OtherPlugin-v1.2.3-UE5.8.zip"),
					TEXT("https://example.invalid/OtherPlugin-v1.2.3-UE5.8.zip"))
			}),
			TEXT("UE5.8"));

	TestFalse(TEXT("another plugin's matching engine zip is rejected"), Selection.IsSuccess());
	TestTrue(
		TEXT("another plugin cannot classify the release as a Monolith engine release"),
		Selection.Failure == EMonolithReleaseZipFailure::NoBinaryZipAsset);
	TestTrue(TEXT("no other-plugin URL escapes the decision"), Selection.Url.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUpdaterUnrelatedEngineZipDoesNotChangeReleaseModeTest,
	"Monolith.Updater.ReleaseSelection.UnrelatedEngineZipDoesNotChangeReleaseMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUpdaterUnrelatedEngineZipDoesNotChangeReleaseModeTest::RunTest(
	const FString& /*Parameters*/)
{
	using namespace MonolithUpdateReleaseSelectorTest;

	const FString MonolithUrl =
		TEXT("https://example.invalid/Monolith-v1.2.3.zip");
	const FMonolithReleaseZipSelection Selection =
		MonolithUpdateReleaseSelector::SelectBinaryZip(
			MakeRelease({
				MakeAsset(
					TEXT("OtherPlugin-v9.9.9-UE5.8.zip"),
					TEXT("https://example.invalid/OtherPlugin-v9.9.9-UE5.8.zip")),
				MakeAsset(TEXT("Monolith-v1.2.3.zip"), MonolithUrl)
			}),
			TEXT("UE5.8"));

	TestTrue(TEXT("the Monolith binary remains selectable"), Selection.IsSuccess());
	TestEqual(TEXT("the unrelated engine ZIP is ignored"), Selection.Url, MonolithUrl);
	TestFalse(
		TEXT("another plugin cannot switch the Monolith release to per-engine mode"),
		Selection.bEngineTagged);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUpdaterZipballOnlyFailsClosedTest,
	"Monolith.Updater.ReleaseSelection.ZipballOnlyDoesNotOfferInstall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUpdaterZipballOnlyFailsClosedTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithUpdateReleaseSelectorTest;

	const FMonolithReleaseZipSelection Selection =
		MonolithUpdateReleaseSelector::SelectBinaryZip(
			MakeRelease({}, TEXT("https://api.github.invalid/repos/source/zipball")),
			TEXT("UE5.8"));

	TestFalse(TEXT("source zipball does not offer an install"), Selection.IsSuccess());
	TestTrue(
		TEXT("failure reports that no binary release asset exists"),
		Selection.Failure == EMonolithReleaseZipFailure::NoBinaryZipAsset);
	TestTrue(TEXT("no download URL escapes the failed decision"), Selection.Url.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUpdaterExactEngineTokenTest,
	"Monolith.Updater.ReleaseSelection.EngineTokenIsBounded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUpdaterExactEngineTokenTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithUpdateReleaseSelectorTest;

	const FString ExactUrl = TEXT("https://example.invalid/Monolith-v1.2.3-UE5.8.zip");
	const FMonolithReleaseZipSelection Selection =
		MonolithUpdateReleaseSelector::SelectBinaryZip(
			MakeRelease({
				MakeAsset(
					TEXT("Monolith-v1.2.3-UE5.80.zip"),
					TEXT("https://example.invalid/Monolith-v1.2.3-UE5.80.zip")),
				MakeAsset(TEXT("Monolith-v1.2.3-UE5.8.zip"), ExactUrl)
			}),
			TEXT("UE5.8"));

	TestTrue(TEXT("exact engine asset is accepted"), Selection.IsSuccess());
	TestEqual(TEXT("UE5.8 does not match UE5.80"), Selection.Url, ExactUrl);
	TestTrue(TEXT("selection is engine-tagged"), Selection.bEngineTagged);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
