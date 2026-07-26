#include "MonolithUpdateReleaseSelector.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	bool IsMonolithBinaryZipName(const FString& Name)
	{
		return Name.StartsWith(TEXT("Monolith-"), ESearchCase::IgnoreCase)
			&& Name.EndsWith(TEXT(".zip"), ESearchCase::IgnoreCase);
	}

	bool MatchesEngineBinaryZipName(const FString& Name, const FString& EngineTag)
	{
		return IsMonolithBinaryZipName(Name)
			&& Name.EndsWith(
				FString::Printf(TEXT("-%s.zip"), *EngineTag),
				ESearchCase::IgnoreCase);
	}
}

FMonolithReleaseZipSelection MonolithUpdateReleaseSelector::SelectBinaryZip(
	const TSharedPtr<FJsonObject>& Release,
	const FString& EngineTag)
{
	FMonolithReleaseZipSelection Selection;

	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	if (!Release.IsValid() || !Release->TryGetArrayField(TEXT("assets"), Assets) || !Assets)
	{
		return Selection;
	}

	bool bPerEngineRelease = false;
	FString FirstZipUrl;
	FString MatchingZipUrl;

	for (const TSharedPtr<FJsonValue>& AssetValue : *Assets)
	{
		const TSharedPtr<FJsonObject>* AssetObject = nullptr;
		if (!AssetValue.IsValid()
			|| !AssetValue->TryGetObject(AssetObject)
			|| !AssetObject
			|| !AssetObject->IsValid())
		{
			continue;
		}

		FString Name;
		(*AssetObject)->TryGetStringField(TEXT("name"), Name);
		if (!Name.EndsWith(TEXT(".zip"), ESearchCase::IgnoreCase))
		{
			continue;
		}

		// GitHub releases can contain source bundles and unrelated archives.
		// Only release artifacts carrying the Monolith prefix are eligible.
		if (!IsMonolithBinaryZipName(Name))
		{
			continue;
		}

		// An unrelated plugin's engine-tagged ZIP must not turn an otherwise
		// engine-agnostic Monolith release into a per-engine Monolith release.
		if (Name.Contains(TEXT("-UE5."), ESearchCase::IgnoreCase))
		{
			bPerEngineRelease = true;
		}

		FString DownloadUrl;
		(*AssetObject)->TryGetStringField(TEXT("browser_download_url"), DownloadUrl);
		if (DownloadUrl.IsEmpty())
		{
			continue;
		}

		if (FirstZipUrl.IsEmpty())
		{
			FirstZipUrl = DownloadUrl;
		}

		// Match the stable prefix/suffix contract without coupling selection to
		// the release tag's hand-entered version text. The engine token must be
		// the final suffix, so UE5.8 cannot match UE5.80 or UE5.18.
		if (MatchingZipUrl.IsEmpty()
			&& MatchesEngineBinaryZipName(Name, EngineTag))
		{
			MatchingZipUrl = DownloadUrl;
		}
	}

	if (bPerEngineRelease)
	{
		if (MatchingZipUrl.IsEmpty())
		{
			Selection.Failure = EMonolithReleaseZipFailure::NoMatchingEngineAsset;
			return Selection;
		}

		Selection.Url = MoveTemp(MatchingZipUrl);
		Selection.bEngineTagged = true;
		Selection.Failure = EMonolithReleaseZipFailure::None;
		return Selection;
	}

	if (!FirstZipUrl.IsEmpty())
	{
		Selection.Url = MoveTemp(FirstZipUrl);
		Selection.Failure = EMonolithReleaseZipFailure::None;
	}

	return Selection;
}
