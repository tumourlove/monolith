// SPDX-License-Identifier: MIT
// Monolith MCP Ergonomics framework primitives — Phase 0.
// Adapters in subsequent phases (1-5) consume these types via FMonolithBulkFillRegistry.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MonolithBulkFillTypes.generated.h"

/**
 * Per-key write outcome inside a bulk_fill transaction.
 * Emitted by FMonolithReflectionWalker; consumed by FBulkFillResult / FDryRunReport.
 */
USTRUCT(BlueprintType)
struct MONOLITHCORE_API FBulkFillFieldWrite
{
	GENERATED_BODY()

	/** Dotted path inside the tree (e.g. "tests[2].score_equation", "Properties[Icon:soft]"). */
	UPROPERTY(BlueprintReadOnly, Category="Monolith|BulkFill")
	FString Path;

	/** Stringified previous value (via FProperty::ExportText_Direct). Empty if the field was unset. */
	UPROPERTY(BlueprintReadOnly, Category="Monolith|BulkFill")
	FString CurrentValue;

	/** Stringified proposed value. */
	UPROPERTY(BlueprintReadOnly, Category="Monolith|BulkFill")
	FString ProposedValue;

	/** True if the walker accepted this write. False = type mismatch / unknown field / enum miss / clamp / silent-drop hazard. */
	UPROPERTY(BlueprintReadOnly, Category="Monolith|BulkFill")
	bool bOk = false;

	/** Human-readable reason on failure (e.g. "enum value not found; did you mean 'Constant'?"). */
	UPROPERTY(BlueprintReadOnly, Category="Monolith|BulkFill")
	FString Reason;
};

/**
 * Input to a bulk_fill or dry-run pass.
 * Adapter handlers consume an FBulkFillSpec; the framework walker fills it.
 */
USTRUCT(BlueprintType)
struct MONOLITHCORE_API FBulkFillSpec
{
	GENERATED_BODY()

	/** Routed to per-namespace adapter ("blueprint", "gas", "inventory", "ui", "ai", "niagara", "material", "audio", "mesh", "animation", "logicdriver", "combograph"). */
	UPROPERTY(BlueprintReadOnly, Category="Monolith|BulkFill")
	FString TargetNamespace;

	/** Asset path (e.g. "/Game/Items/DA_HealingPotion"). */
	UPROPERTY(BlueprintReadOnly, Category="Monolith|BulkFill")
	FString TargetAsset;

	/**
	 * JSON tree of properties to walk.
	 * NOT a UPROPERTY — TSharedPtr<FJsonObject> is not USTRUCT-serialisable.
	 * Adapters set this field in C++ only (the registry dispatcher hydrates it from
	 * the JSON-RPC params before calling the adapter).
	 */
	TSharedPtr<FJsonObject> Tree;

	/** When true, validate but don't persist. */
	UPROPERTY(BlueprintReadOnly, Category="Monolith|BulkFill")
	bool bDryRun = false;

	/** When true, promote silent drops / clamps / unknown-fields to hard errors. Default false (status-quo permissive). */
	UPROPERTY(BlueprintReadOnly, Category="Monolith|BulkFill")
	bool bStrict = false;
};

/**
 * Output of either a bulk_fill commit OR a dry_run inspection.
 * One structure for both code paths so the dispatcher's two return shapes stay aligned.
 */
USTRUCT(BlueprintType)
struct MONOLITHCORE_API FDryRunReport
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Monolith|BulkFill")
	TArray<FString> WouldCreate;

	UPROPERTY(BlueprintReadOnly, Category="Monolith|BulkFill")
	TArray<FString> WouldModify;

	UPROPERTY(BlueprintReadOnly, Category="Monolith|BulkFill")
	TArray<FBulkFillFieldWrite> FieldWrites;

	/** Fields the walker accepted but the underlying action silently no-ops (clamps, set-once, build_*_from_spec known-drops). */
	UPROPERTY(BlueprintReadOnly, Category="Monolith|BulkFill")
	TArray<FBulkFillFieldWrite> SilentDrops;

	/** True if the underlying action actually persisted. False on dry_run or on failure. */
	UPROPERTY(BlueprintReadOnly, Category="Monolith|BulkFill")
	bool bWouldApply = false;

	/** Count of FieldWrites where bOk==false. */
	UPROPERTY(BlueprintReadOnly, Category="Monolith|BulkFill")
	int32 Errors = 0;
};

/**
 * Per-field schema description emitted by `describe`.
 * NOT JSON Schema standard — custom rich-type tree per Design Decision Q3.
 */
USTRUCT(BlueprintType)
struct MONOLITHCORE_API FSchemaDescriptor
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Monolith|Describe")
	FString FieldPath;

	/** UE reflection type ("int32", "float", "FName", "TSoftObjectPtr<UTexture2D>", "TMap<FName,FString>"). */
	UPROPERTY(BlueprintReadOnly, Category="Monolith|Describe")
	FString TypeName;

	/** Concrete ImportText form example (e.g. for FGameplayTagContainer: '(GameplayTags=((TagName="Item.Healing")))'). */
	UPROPERTY(BlueprintReadOnly, Category="Monolith|Describe")
	FString ImportTextForm;

	UPROPERTY(BlueprintReadOnly, Category="Monolith|Describe")
	bool bRequired = false;

	UPROPERTY(BlueprintReadOnly, Category="Monolith|Describe")
	bool bSetOnce = false;

	UPROPERTY(BlueprintReadOnly, Category="Monolith|Describe")
	bool bPieBlocked = false;

	/** Sibling field whose presence enables this one (engine quirk gating — e.g. logicdriver instanced subobjects). */
	UPROPERTY(BlueprintReadOnly, Category="Monolith|Describe")
	FString ConditionalOn;

	/** For FEnumProperty fields: full list of legal value-name strings. */
	UPROPERTY(BlueprintReadOnly, Category="Monolith|Describe")
	TArray<FString> EnumValues;

	/**
	 * Nested fields for FStructProperty / FArrayProperty / FMapProperty / FSetProperty. Empty for scalars.
	 *
	 * NOT a UPROPERTY -- UE 5.7 reflection forbids USTRUCT recursion via TArray<Self>
	 * ("'Struct' recursion via arrays is unsupported for properties"). C++ access is unchanged.
	 * JSON serialisation walks this field explicitly in
	 * FMonolithBulkFillActions::DescriptorToJson (MonolithBulkFillActions.cpp), NOT via
	 * FJsonObjectConverter::UStructToJsonObject. Losing reflection on Children is therefore
	 * invisible to MCP clients -- the dispatcher's hand-rolled walker recurses unchanged.
	 * Blueprint readers lose access to the Children tree (acceptable: schema descriptors
	 * are consumed in C++ / over JSON-RPC, never inside a BP graph).
	 */
	TArray<FSchemaDescriptor> Children;

	/**
	 * Numeric clamp lower bound (per Design Decision Q3, locked — M1).
	 * Populated when the FProperty carries `UIMin` / `ClampMin` meta. Both 0 = no clamp known.
	 * Stored as float for the type-erased descriptor shape (callers downcast per TypeName).
	 */
	UPROPERTY(BlueprintReadWrite, Category="Monolith|Describe")
	float RangeMin = 0.f;

	/** Numeric clamp upper bound. Populated from `UIMax` / `ClampMax` meta. */
	UPROPERTY(BlueprintReadWrite, Category="Monolith|Describe")
	float RangeMax = 0.f;
};
