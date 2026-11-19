// SPDX-License-Identifier: MIT
// Fixture types for the property-path automation tests
// (MonolithReflectionPathTest.cpp). The shapes mirror a GUID-keyed map whose
// entries hold a tagged-union value, the layout behind issue #172.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "MonolithReflectionPathTestTypes.generated.h"

USTRUCT()
struct FMonolithPathTestUnionValue
{
	GENERATED_BODY()

	// Tag that says which member is live. 0 = invalid.
	UPROPERTY()
	int32 TypeTag = 0;

	UPROPERTY()
	float Float = 0.f;

	UPROPERTY()
	FName Label;
};

USTRUCT()
struct FMonolithPathTestEntry
{
	GENERATED_BODY()

	UPROPERTY()
	bool bEnable = false;

	UPROPERTY()
	FMonolithPathTestUnionValue Value;

	UPROPERTY()
	FName CachedName;
};

USTRUCT()
struct FMonolithPathTestRoot
{
	GENERATED_BODY()

	UPROPERTY()
	TMap<FGuid, FMonolithPathTestEntry> Overrides;
};
