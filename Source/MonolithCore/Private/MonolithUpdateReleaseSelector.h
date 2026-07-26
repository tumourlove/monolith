#pragma once

#include "CoreMinimal.h"

class FJsonObject;

enum class EMonolithReleaseZipFailure : uint8
{
	None,
	NoBinaryZipAsset,
	NoMatchingEngineAsset
};

/**
 * Pure release-metadata decision used by the updater before it offers an
 * install. A successful decision always points at an explicit GitHub release
 * asset whose name follows the Monolith binary prefix/suffix contract;
 * repository source zipballs and unrelated archives are outside this contract.
 */
struct FMonolithReleaseZipSelection
{
	FString Url;
	bool bEngineTagged = false;
	EMonolithReleaseZipFailure Failure = EMonolithReleaseZipFailure::NoBinaryZipAsset;

	bool IsSuccess() const
	{
		return Failure == EMonolithReleaseZipFailure::None && !Url.IsEmpty();
	}
};

namespace MonolithUpdateReleaseSelector
{
	FMonolithReleaseZipSelection SelectBinaryZip(
		const TSharedPtr<FJsonObject>& Release,
		const FString& EngineTag);
}
