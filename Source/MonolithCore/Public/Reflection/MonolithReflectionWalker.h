// SPDX-License-Identifier: MIT
// FMonolithReflectionWalker — JSON-tree -> FProperty schema walker.
// Game-thread only. Adapters in Phases 1-5 invoke WriteTree / InspectTree / DescribeStruct.

#pragma once

#include "CoreMinimal.h"
#include "MonolithBulkFillTypes.h"
#include "Dom/JsonObject.h"

class FProperty;
class FStructProperty;
class FArrayProperty;
class FMapProperty;
class FSetProperty;
class FObjectProperty;
class FSoftObjectProperty;
class FEnumProperty;
class UStruct;

/**
 * Static helper library that walks a JSON tree against a UStruct's FProperty schema.
 * Game-thread only (UE reflection writes serialise on the game thread; see plan §10).
 *
 * The walker emits per-field write outcomes. It does NOT wrap writes in a transaction
 * or PreEditChange/PostEditChange — that is the calling adapter's responsibility
 * (mirrors the cradle pattern at MonolithBlueprintCDOActions.cpp:417-424).
 */
class MONOLITHCORE_API FMonolithReflectionWalker
{
public:
	/**
	 * Walk a tree against a class/struct and write matching fields onto Container.
	 * Wrapped in an FScopedTransaction by the caller (the adapter — NOT this helper).
	 */
	static FDryRunReport WriteTree(
		const TSharedPtr<FJsonObject>& Tree,
		UStruct* TopStruct,
		void* Container,
		UObject* OwnerForCradle,
		const FBulkFillSpec& Spec);

	/**
	 * Same walk WITHOUT mutation — emits would-be writes only. Used by dry_run.
	 * Routes every ImportText_Direct call through a per-field scratch buffer so the
	 * report captures grammar-accept/reject WITHOUT touching Container.
	 */
	static FDryRunReport InspectTree(
		const TSharedPtr<FJsonObject>& Tree,
		UStruct* TopStruct,
		const void* Container,
		const FBulkFillSpec& Spec);

	/**
	 * Build a recursive FSchemaDescriptor tree for a class/struct.
	 * Used by `describe`.
	 */
	static FSchemaDescriptor DescribeStruct(UStruct* TopStruct, int32 MaxDepth = 16);

	/**
	 * Lookup helper: exact then case-insensitive fallback.
	 * Mirrors existing MonolithBlueprintCDOActions.cpp:385-396 pattern so behaviour
	 * matches set_cdo_property.
	 */
	static FProperty* FindPropertyForwarding(UStruct* Struct, const FString& Name);

	/**
	 * Recover the UEnum backing a UserDefinedEnum field inside a UserDefinedStruct.
	 *
	 * A UserDefinedEnum field compiles to a plain numeric FProperty with no Enum
	 * association at runtime: UUserDefinedEnum is always ECppForm::Namespaced, and
	 * the KismetCompiler only emits an FEnumProperty for ECppForm::EnumClass. The
	 * UEnum is therefore only recoverable from editor-only UDS metadata
	 * (FStructVariableDescription::SubCategoryObject).
	 *
	 * Returns the UEnum* for such a field, or nullptr for everything else (native
	 * enums already surface as FEnumProperty/FByteProperty and need no recovery).
	 * Editor-only — returns nullptr in non-editor builds.
	 */
	static UEnum* RecoverUserDefinedEnum(const FProperty* Prop);

	/**
	 * Map an incoming token to the integer value of a recovered UserDefinedEnum
	 * field. The token may be a friendly display name, the authored short/full
	 * name, or a bare integer. Returns false if the field is not a recoverable UDS
	 * enum, or if the token does not resolve to one of its enumerators. Bare-integer
	 * tokens are intentionally NOT resolved here so callers preserve back-compat by
	 * falling through to their existing ImportText path.
	 */
	static bool ResolveUserDefinedEnumToken(const FProperty* Prop, const FString& Token, int64& OutValue);

private:
	// Inner switch — routes a single JSON value to its FProperty subtype handler.
	static void DispatchByPropertyType(
		FProperty* Prop,
		void* ValuePtr,
		const TSharedPtr<FJsonValue>& JsonVal,
		UObject* Owner,
		const FBulkFillSpec& Spec,
		FDryRunReport& OutReport,
		const FString& PathPrefix,
		FBulkFillFieldWrite& OutWrite);

	// Per-type ImportText grammar emitters. Each writes one value into one FProperty.
	// Signature: returns bOk + Reason via the FBulkFillFieldWrite the caller passes in.
	static void WriteScalar(FProperty* Prop, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* Owner, FBulkFillFieldWrite& OutWrite);
	static void WriteStruct(FStructProperty* StructProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* Owner, const FBulkFillSpec& Spec, FDryRunReport& OutReport, const FString& PathPrefix, FBulkFillFieldWrite& OutWrite);
	static void WriteArray(FArrayProperty* ArrayProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* Owner, const FBulkFillSpec& Spec, FDryRunReport& OutReport, const FString& PathPrefix, FBulkFillFieldWrite& OutWrite);
	static void WriteMap(FMapProperty* MapProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* Owner, const FBulkFillSpec& Spec, FDryRunReport& OutReport, const FString& PathPrefix, FBulkFillFieldWrite& OutWrite);
	static void WriteSet(FSetProperty* SetProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* Owner, const FBulkFillSpec& Spec, FDryRunReport& OutReport, const FString& PathPrefix, FBulkFillFieldWrite& OutWrite);
	static void WriteObjectRef(FObjectProperty* ObjProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* Container, FBulkFillFieldWrite& OutWrite);
	static void WriteSoftObjectRef(FSoftObjectProperty* SoftProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, FBulkFillFieldWrite& OutWrite);
	static void WriteEnum(FEnumProperty* EnumProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, FBulkFillFieldWrite& OutWrite);

	// Describe-side helpers — populate clamp metadata + ImportText sample forms.
	static void PopulateClampMeta(FProperty* Prop, FSchemaDescriptor& OutDesc);
};
