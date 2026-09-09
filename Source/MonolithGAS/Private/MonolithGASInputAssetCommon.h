#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithToolRegistry.h"
#include "UObject/Object.h"
#include "UObject/ObjectPtr.h"

struct FAssetData;
struct FEnhancedActionKeyMapping;
class UInputAction;
class UInputMappingContext;

/**
 * Shared parameter, path and JSON helpers for the `input` namespace.
 *
 * Every bound the namespace exposes is declared here so the read actions
 * (MonolithGASInputAssetActions) and the authoring actions
 * (MonolithGASInputAuthoringActions) agree on the same limits — an authored
 * trigger/modifier list can never grow past what a read can return without
 * truncating.
 */
namespace MonolithInput
{
	/** Asset-discovery pagination: list_input_actions / list_input_mapping_contexts / validate_input_mappings. */
	constexpr int32 DefaultAssetPageLimit = 200;
	constexpr int32 MaxAssetPageLimit = 1000;

	/** Per-context mapping pagination: get_input_mapping_context, and details rows of list_input_mapping_contexts. */
	constexpr int32 DefaultMappingPageLimit = 100;
	constexpr int32 MaxMappingPageLimit = 500;

	/** Instanced trigger/modifier array bounds. Enforced on read (truncation flags) AND on write (refusal). */
	constexpr int32 MaxInstancedPerAction = 256;
	constexpr int32 MaxInstancedPerMapping = 64;

	/** validate_input_mappings per-context mapping scan bound. */
	constexpr int32 DefaultMappingScanLimit = 4096;
	constexpr int32 MaxMappingScanLimit = 10000;

	/** Maximum explicit context_paths accepted by validate_input_mappings. */
	constexpr int32 MaxExplicitContextPaths = 1000;

	/** JSON-RPC invalid-params error, tagged with the offending field. */
	FMonolithActionResult InvalidParam(const TCHAR* Field, const FString& Detail);

	bool HasParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field);

	/** Reads an optional JSON number, rejecting non-integers and out-of-range values. */
	bool ReadBoundedInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, int32 DefaultValue,
		int32 MinValue, int32 MaxValue, int32& OutValue, FMonolithActionResult& OutError);

	bool ReadOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool bDefaultValue,
		bool& bOutValue, FMonolithActionResult& OutError);

	bool ReadRequiredString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field,
		FString& OutValue, FMonolithActionResult& OutError);

	bool ReadOptionalString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, const FString& DefaultValue,
		FString& OutValue, FMonolithActionResult& OutError);

	/** Validates a canonical mounted package prefix such as /Game/Input. No object suffix, no trailing slash. */
	bool ParsePackagePrefix(const FString& Input, FString& OutPrefix, FMonolithActionResult& OutError);

	/**
	 * Accepts a canonical package path (/Game/Input/IA_Jump) or the matching top-level
	 * object path (/Game/Input/IA_Jump.IA_Jump) and normalises to the object path.
	 * An object name that disagrees with the package leaf is rejected rather than coerced.
	 */
	bool ParseObjectPath(const FString& Input, const TCHAR* Field, FString& OutObjectPath, FMonolithActionResult& OutError);

	/** Loads exactly the asset named by Input. Never falls back to another asset, never creates one. */
	UObject* LoadExact(UClass* ExpectedClass, const FString& Input, const TCHAR* Field,
		const TCHAR* ExpectedTypeName, FMonolithActionResult& OutError);

	/** Recursive AssetRegistry query under PackagePrefix, sorted by object path for stable pagination. */
	void QueryAssets(UClass* AssetClass, const FString& PackagePrefix, TArray<FAssetData>& OutAssets);

	/** { class, class_path } summary of an instanced trigger/modifier. */
	TSharedPtr<FJsonObject> DescribeInstanced(const UObject* Object);

	/** Bounded JSON array of instanced objects; reports whether the array was cut short. */
	template <typename TObject>
	TArray<TSharedPtr<FJsonValue>> DescribeInstancedArray(const TArray<TObjectPtr<TObject>>& Objects,
		int32 Limit, bool& bOutTruncated)
	{
		const int32 Count = FMath::Min(Objects.Num(), Limit);
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Values.Add(MakeShared<FJsonValueObject>(DescribeInstanced(Objects[Index].Get())));
		}
		bOutTruncated = Objects.Num() > Count;
		return Values;
	}

	/** total / offset / limit / count / has_more. */
	void SetPageFields(const TSharedPtr<FJsonObject>& Json, int32 Total, int32 Offset, int32 Limit, int32 Count);

	/** Full InputAction description, with triggers/modifiers bounded by MaxInstancedPerAction. */
	TSharedPtr<FJsonObject> InputActionToJson(const UInputAction* Action);

	/** One key mapping, with triggers/modifiers bounded by MaxInstancedPerMapping. */
	TSharedPtr<FJsonObject> MappingToJson(const FEnhancedActionKeyMapping& Mapping, int32 Index);

	/** Mapping context header plus a [MappingOffset, MappingOffset + MappingLimit) window of its mappings. */
	TSharedPtr<FJsonObject> MappingContextToJson(const UInputMappingContext* Context, int32 MappingOffset, int32 MappingLimit);
}
