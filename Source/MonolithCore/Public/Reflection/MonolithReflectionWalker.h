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
class UPackage;

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
	 * True when Prop is a deprecated UPROPERTY: CPF_Deprecated (the _DEPRECATED suffix
	 * form, never saved) or meta=(DeprecatedProperty) (kept for load-time migration only,
	 * e.g. UInputMappingContext::Mappings since UE 5.7, superseded by
	 * DefaultKeyMappings.Mappings). Such a property no longer holds the live data, so a
	 * reflection write to it looks applied but is never read by the engine — and a read
	 * sees an empty or stale value. OutReason carries a caller-facing explanation,
	 * including the property's DeprecationMessage when it has one.
	 */
	static bool IsDeprecatedProperty(const FProperty* Prop, FString& OutReason);

	/**
	 * Result of resolving a dotted+bracket property path down to a single leaf.
	 *
	 * On success: LeafProp + LeafPtr address exactly the value to overwrite, and
	 * the caller routes the value through WriteLeaf (the SAME coercion the bulk
	 * walker uses — scalars/enums/structs/soft-refs/etc.). This is a SURGICAL
	 * write: nested arrays/maps along the path are addressed in place, NOT
	 * cleared+rebuilt the way WriteArray/WriteMap do for a full tree.
	 */
	/**
	 * Owned, default-initialised value of one FProperty. A no-mutation resolve
	 * (FPathResolveOptions::bNoMutation) that meets a missing map key with
	 * bCreateMissingKeys walks the rest of the path inside one of these instead of
	 * adding a pair to the live map. The result keeps it alive.
	 */
	struct MONOLITHCORE_API FPathScratchValue
	{
		explicit FPathScratchValue(const FProperty* InProp);
		~FPathScratchValue();
		FPathScratchValue(const FPathScratchValue&) = delete;
		FPathScratchValue& operator=(const FPathScratchValue&) = delete;

		const FProperty* Prop = nullptr;
		void* Memory = nullptr;
	};

	struct FPathResolveResult
	{
		bool bOk = false;
		FString Error;                  // Human-readable failure reason (empty on success).
		FProperty* LeafProp = nullptr;  // Terminal FProperty to write through.
		void* LeafPtr = nullptr;        // Address of the terminal value inside Container (or inside a scratch value).
		FString LeafTypeName;           // LeafProp->GetCPPType() — echoed back to the caller.

		// UObject that directly holds the leaf: the deepest object the path dereferenced,
		// or FPathResolveOptions::RootObject when the path never crosses an object ref.
		UObject* LeafOwnerObject = nullptr;
		// First member property resolved on LeafOwnerObject (the property to notify in
		// PreEditChange / PostEditChange on that object).
		FProperty* LeafOwnerHeadProp = nullptr;
		// Path names of dereferenced objects whose package differs from
		// FPathResolveOptions::HomePackage (only filled when HomePackage is set).
		TArray<FString> ExternalObjects;

		// A '[Key]' segment named a key that was absent and was created: added to the
		// live map (normal resolve) or resolved against a scratch value (bNoMutation).
		// Only the first (outermost) created entry is described below.
		bool bCreatedMissingKey = false;
		FString CreatedKeyPath;                   // Path prefix up to and including the created key's bracket.
		FProperty* CreatedElementProp = nullptr;  // Map ValueProp of the created entry.
		void* CreatedElementPtr = nullptr;        // Value of the created entry.
		bool bLeafIsCreatedElement = false;       // The path ends at the created entry itself.
		bool bLeafInScratch = false;              // LeafPtr points into a scratch value, not into Container.
		bool bAppliedNewElementValue = false;     // FPathResolveOptions::NewElementValue initialised the created entry.

		// Non-fatal notes (e.g. new_element_value ignored because the key already existed).
		TArray<FString> Warnings;

		// Keeps scratch values alive for as long as the result (and LeafPtr) is used.
		TArray<TSharedPtr<FPathScratchValue>> ScratchValues;
	};

	/** Options for ResolvePathWithOptions. Defaults reproduce ResolvePath(..., false). */
	struct FPathResolveOptions
	{
		// Add a default-initialised value when a '[Key]' segment's key is absent.
		bool bCreateMissingKeys = false;
		// Never mutate Container. A missing key with bCreateMissingKeys resolves the rest
		// of the path against a scratch value (bLeafInScratch on the result). Use for dry runs.
		bool bNoMutation = false;
		// Optional value used to initialise the FIRST map entry the path creates, before
		// the rest of the path is walked. Accepts what WriteLeaf accepts for the map's
		// value type (ImportText text such as "(A=1,B=(C=2))", or a JSON object).
		TSharedPtr<FJsonValue> NewElementValue;
		// Object that owns Container (the asset / CDO). Seeds LeafOwnerObject.
		UObject* RootObject = nullptr;
		// When set, every dereferenced object outside this package is listed in
		// ExternalObjects.
		const UPackage* HomePackage = nullptr;
		// With HomePackage set: fail the resolve on the first dereference into an
		// object outside HomePackage. Writes use this so they cannot land in other assets.
		bool bRefuseExternalObjects = false;
		// Strict coercion when applying NewElementValue.
		bool bStrict = false;
	};

	/**
	 * Walk a dotted+bracket path (e.g. "Standing.Gaits[Jog].Starts.Forward")
	 * from Container/TopStruct down to a single leaf value pointer.
	 *
	 * Path grammar:
	 *   - "."   descends into an FStructProperty member.
	 *   - "[N]" indexes an FArrayProperty (N = integer literal). Out-of-range errors.
	 *   - "[K]" keys an FMapProperty; K is ImportText'd through the map's KeyProp,
	 *           so enum names ("Jog"), integers, FName/string keys all resolve.
	 *           Missing key -> error UNLESS bCreateMissingKeys (then AddPair with a
	 *           default-initialised value, then re-locate the value ptr).
	 *   - "[@Name]" on an array of object refs selects the element whose object
	 *           name matches (case-insensitive), e.g. "Nodes[@MyNode_0]".
	 *   - "[@class=ClassName]" on an array of object refs selects the single element
	 *           of exactly that class (with or without the U prefix). More than one
	 *           match is an error; use [@Name] or [N] then.
	 *   - a bare segment after an FObjectProperty descends into the pointed-at
	 *     UObject's class (the ref must already be set / resolvable in memory).
	 *
	 * Does NOT write the leaf and does NOT touch the edit cradle — the caller owns
	 * the FScopedTransaction + Pre/PostEditChange + MarkPackageDirty, exactly as
	 * set_cdo_property does. Game-thread only (mutates map storage when creating keys).
	 */
	static FPathResolveResult ResolvePath(
		UStruct* TopStruct,
		void* Container,
		const FString& Path,
		bool bCreateMissingKeys);

	/** ResolvePath with the full option set (no-mutation dry runs, new-entry init, object scope). */
	static FPathResolveResult ResolvePathWithOptions(
		UStruct* TopStruct,
		void* Container,
		const FString& Path,
		const FPathResolveOptions& Options);

	/**
	 * Dry-run evaluation of a leaf write. Copies the CURRENT leaf value into a scratch
	 * buffer and writes there, so CurrentValue reports the live value and the leaf is
	 * not mutated. When the leaf already lives in a scratch value (bLeafInScratch) it is
	 * written in place. Strict numeric text checks apply exactly as in a real write.
	 */
	static FBulkFillFieldWrite InspectLeaf(
		const FPathResolveResult& Resolved,
		const TSharedPtr<FJsonValue>& JsonVal,
		UObject* Owner,
		const FBulkFillSpec& Spec,
		FDryRunReport& OutReport,
		const FString& PathLabel);

	/**
	 * For a struct value (ElementProp an FStructProperty), list the fields that still
	 * equal the struct's default, as dotted authored names. A field that differs from
	 * its default and is itself a struct is searched recursively ("Value.Type").
	 * Returns an empty array for non-struct values.
	 */
	static TArray<FString> ListDefaultedStructFields(const FProperty* ElementProp, const void* ElementPtr, int32 MaxDepth = 4);

	/**
	 * Coerce + write a single JSON value into a single resolved leaf (LeafProp +
	 * LeafPtr from ResolvePath). Public, thin forwarder onto the private
	 * DispatchByPropertyType so the surgical-path handler reuses the EXACT same
	 * value coercion as the bulk tree walker (scalars, enums, structs, arrays,
	 * maps, sets, hard + soft object refs, UDS enums) without duplicating it.
	 *
	 * Returns the per-field write outcome. Like the rest of the walker it does NOT
	 * wrap the write in a transaction or fire the edit cradle — the caller owns that.
	 */
	static FBulkFillFieldWrite WriteLeaf(
		FProperty* LeafProp,
		void* LeafPtr,
		const TSharedPtr<FJsonValue>& JsonVal,
		UObject* Owner,
		const FBulkFillSpec& Spec,
		FDryRunReport& OutReport,
		const FString& PathLabel);

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
	// Spec/OutReport are optional: with them, numeric text that ImportText would only
	// partly parse ("abc" -> 0 on a float) is an error under Spec.bStrict, and a
	// SilentDrops entry otherwise.
	static void WriteScalar(FProperty* Prop, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* Owner, FBulkFillFieldWrite& OutWrite, const FBulkFillSpec* Spec = nullptr, FDryRunReport* OutReport = nullptr);
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
