#pragma once

#include "CoreMinimal.h"
#include "Misc/PackageName.h"

/**
 * Defensive validator for long package paths (e.g. "/Game/Foo/Bar").
 *
 * Added in response to a fatal editor crash where raw JSON input containing
 * "//Game/..." (double leading slash) was passed directly to CreatePackage().
 * CreatePackage asserts on such inputs in UObjectGlobals.cpp:1012. This
 * wrapper converts the assertion into a recoverable error return.
 *
 * Scope note: only wired into the immediate crash site plus the two shared
 * GetOrCreatePackage helpers (MonolithAI, MonolithGAS). The other ~77
 * CreatePackage call sites remain unguarded — follow-up task.
 */
namespace MonolithCore
{
	/**
	 * Validates a long package path.
	 * @param InPath  Package path to check, e.g. "/Game/Foo/Bar".
	 * @return        Empty FString on success; human-readable error message on failure.
	 */
	inline FString ValidatePackagePath(const FString& InPath)
	{
		if (InPath.IsEmpty())
		{
			return TEXT("Package path is empty");
		}

		FText OutReason;
		if (!FPackageName::IsValidLongPackageName(InPath, /*bIncludeReadOnlyRoots=*/false, &OutReason))
		{
			return FString::Printf(TEXT("Invalid package path '%s': %s"), *InPath, *OutReason.ToString());
		}

		return FString();
	}
}
