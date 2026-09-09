#include "MonolithGASInputAssetActions.h"

#include "AssetRegistry/AssetData.h"
#include "Dom/JsonValue.h"
#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "MonolithGASInputAssetCommon.h"
#include "MonolithParamSchema.h"

using namespace MonolithInput;

// ─────────────────────────────────────────────────────────────────────────────
// Local helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	/** path / offset / limit / include_details — shared by both list actions. */
	bool ReadAssetListParams(const TSharedPtr<FJsonObject>& Params, FString& OutPrefix, int32& OutOffset,
		int32& OutLimit, bool& bOutIncludeDetails, FMonolithActionResult& OutError)
	{
		FString Path;
		return ReadOptionalString(Params, TEXT("path"), TEXT("/Game"), Path, OutError)
			&& ParsePackagePrefix(Path, OutPrefix, OutError)
			&& ReadBoundedInt(Params, TEXT("offset"), 0, 0, MAX_int32, OutOffset, OutError)
			&& ReadBoundedInt(Params, TEXT("limit"), DefaultAssetPageLimit, 1, MaxAssetPageLimit, OutLimit, OutError)
			&& ReadOptionalBool(Params, TEXT("include_details"), false, bOutIncludeDetails, OutError);
	}

	/** Identity row for an asset the caller did not ask to load. */
	TSharedPtr<FJsonObject> MakeAssetSummaryRow(const FAssetData& AssetData)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		Row->SetStringField(TEXT("package_path"), AssetData.PackageName.ToString());
		Row->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		return Row;
	}

	/** Explicit, non-empty validation issue. Never a silent skip. */
	TSharedPtr<FJsonObject> MakeValidationIssue(const TCHAR* Type, const TCHAR* Severity, const FString& Message,
		int32 MappingIndex = INDEX_NONE)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("type"), Type);
		Issue->SetStringField(TEXT("severity"), Severity);
		Issue->SetStringField(TEXT("message"), Message);
		if (MappingIndex != INDEX_NONE)
		{
			Issue->SetNumberField(TEXT("mapping_index"), MappingIndex);
		}
		return Issue;
	}

	/** Optional explicit context_paths selector for validate_input_mappings. */
	bool ReadValidationContextPaths(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutPaths,
		FMonolithActionResult& OutError)
	{
		OutPaths.Reset();
		if (!HasParam(Params, TEXT("context_paths")))
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Field = Params->TryGetField(TEXT("context_paths"));
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Field.IsValid() || Field->Type != EJson::Array || !Field->TryGetArray(Values)
			|| !Values || Values->IsEmpty())
		{
			OutError = InvalidParam(TEXT("context_paths"),
				TEXT("expected a non-empty array of canonical asset paths"));
			return false;
		}
		if (Values->Num() > MaxExplicitContextPaths)
		{
			OutError = InvalidParam(TEXT("context_paths"),
				FString::Printf(TEXT("at most %d paths are accepted"), MaxExplicitContextPaths));
			return false;
		}

		TSet<FString> Seen;
		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			FString Path;
			const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
			if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Path) || Path.IsEmpty())
			{
				OutError = InvalidParam(TEXT("context_paths"),
					FString::Printf(TEXT("element %d must be a non-empty string"), Index));
				return false;
			}

			FString ObjectPath;
			if (!ParseObjectPath(Path, TEXT("context_paths"), ObjectPath, OutError))
			{
				return false;
			}
			if (Seen.Contains(ObjectPath))
			{
				OutError = InvalidParam(TEXT("context_paths"),
					FString::Printf(TEXT("duplicate path '%s'"), *ObjectPath));
				return false;
			}
			Seen.Add(ObjectPath);
			OutPaths.Add(ObjectPath);
		}
		OutPaths.Sort();
		return true;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

void FMonolithGASInputAssetActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("input"), TEXT("list_input_actions"),
		TEXT("List Enhanced Input action assets with stable bounded pagination"),
		FMonolithActionHandler::CreateStatic(&HandleListInputActions),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("path"), TEXT("Canonical package root; defaults to /Game"))
			.Optional(TEXT("offset"), TEXT("integer"), TEXT("Zero-based result offset"), TEXT("0"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum assets to return (1-1000)"), TEXT("200"))
			.Optional(TEXT("include_details"), TEXT("boolean"), TEXT("Load only the returned page and include action details"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("get_input_action"),
		TEXT("Inspect one Enhanced Input action asset without modifying it"),
		FMonolithActionHandler::CreateStatic(&HandleGetInputAction),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Canonical InputAction package or object path"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("list_input_mapping_contexts"),
		TEXT("List Enhanced Input mapping contexts with stable bounded pagination"),
		FMonolithActionHandler::CreateStatic(&HandleListInputMappingContexts),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("path"), TEXT("Canonical package root; defaults to /Game"))
			.Optional(TEXT("offset"), TEXT("integer"), TEXT("Zero-based result offset"), TEXT("0"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum assets to return (1-1000)"), TEXT("200"))
			.Optional(TEXT("include_details"), TEXT("boolean"), TEXT("Load only the returned page and include bounded mappings"), TEXT("false"))
			.Optional(TEXT("mapping_limit"), TEXT("integer"), TEXT("Mappings per detailed context (1-500)"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("get_input_mapping_context"),
		TEXT("Inspect one Enhanced Input mapping context with bounded mapping pagination"),
		FMonolithActionHandler::CreateStatic(&HandleGetInputMappingContext),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Canonical InputMappingContext package or object path"))
			.Optional(TEXT("mapping_offset"), TEXT("integer"), TEXT("Zero-based mapping offset"), TEXT("0"))
			.Optional(TEXT("mapping_limit"), TEXT("integer"), TEXT("Maximum mappings to return (1-500)"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("validate_input_mappings"),
		TEXT("Read-only validation for missing actions, invalid keys, duplicate-key warnings, and scan completeness"),
		FMonolithActionHandler::CreateStatic(&HandleValidateInputMappings),
		FParamSchemaBuilder()
			.Optional(TEXT("context_paths"), TEXT("array"), TEXT("Canonical InputMappingContext paths; mutually exclusive with path"))
			.OptionalAssetPath(TEXT("path"), TEXT("Canonical package root when context_paths is omitted; defaults to /Game"))
			.Optional(TEXT("offset"), TEXT("integer"), TEXT("Zero-based context offset"), TEXT("0"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum contexts to validate (1-1000)"), TEXT("200"))
			.Optional(TEXT("mapping_scan_limit"), TEXT("integer"), TEXT("Maximum mappings scanned per context (1-10000)"), TEXT("4096"))
			.Build());
}

// ─────────────────────────────────────────────────────────────────────────────
// list_input_actions
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASInputAssetActions::HandleListInputActions(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePrefix;
	int32 Offset = 0;
	int32 Limit = 0;
	bool bIncludeDetails = false;
	FMonolithActionResult Error;
	if (!ReadAssetListParams(Params, PackagePrefix, Offset, Limit, bIncludeDetails, Error))
	{
		return Error;
	}

	TArray<FAssetData> Assets;
	QueryAssets(UInputAction::StaticClass(), PackagePrefix, Assets);

	const int32 Start = FMath::Min(Offset, Assets.Num());
	const int32 End = static_cast<int32>(FMath::Min<int64>(Assets.Num(), static_cast<int64>(Offset) + Limit));
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(FMath::Max(0, End - Start));
	for (int32 Index = Start; Index < End; ++Index)
	{
		TSharedPtr<FJsonObject> Row = MakeAssetSummaryRow(Assets[Index]);
		// Only the returned page is ever loaded — discovery itself stays registry-only.
		if (bIncludeDetails)
		{
			if (UInputAction* Action = Cast<UInputAction>(Assets[Index].GetAsset()))
			{
				Row = InputActionToJson(Action);
			}
			else
			{
				Row->SetStringField(TEXT("load_error"), TEXT("Asset could not be loaded as UInputAction"));
			}
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), PackagePrefix);
	Result->SetBoolField(TEXT("include_details"), bIncludeDetails);
	Result->SetArrayField(TEXT("actions"), Rows);
	SetPageFields(Result, Assets.Num(), Offset, Limit, Rows.Num());
	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// get_input_action
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASInputAssetActions::HandleGetInputAction(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult Error;
	if (!ReadRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error;
	}

	UObject* Asset = LoadExact(UInputAction::StaticClass(), AssetPath, TEXT("asset_path"),
		TEXT("UInputAction"), Error);
	return Asset
		? FMonolithActionResult::Success(InputActionToJson(CastChecked<UInputAction>(Asset)))
		: Error;
}

// ─────────────────────────────────────────────────────────────────────────────
// list_input_mapping_contexts
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASInputAssetActions::HandleListInputMappingContexts(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePrefix;
	int32 Offset = 0;
	int32 Limit = 0;
	int32 MappingLimit = DefaultMappingPageLimit;
	bool bIncludeDetails = false;
	FMonolithActionResult Error;
	if (!ReadAssetListParams(Params, PackagePrefix, Offset, Limit, bIncludeDetails, Error)
		|| !ReadBoundedInt(Params, TEXT("mapping_limit"), DefaultMappingPageLimit, 1, MaxMappingPageLimit,
			MappingLimit, Error))
	{
		return Error;
	}

	TArray<FAssetData> Assets;
	QueryAssets(UInputMappingContext::StaticClass(), PackagePrefix, Assets);

	const int32 Start = FMath::Min(Offset, Assets.Num());
	const int32 End = static_cast<int32>(FMath::Min<int64>(Assets.Num(), static_cast<int64>(Offset) + Limit));
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(FMath::Max(0, End - Start));
	for (int32 Index = Start; Index < End; ++Index)
	{
		TSharedPtr<FJsonObject> Row = MakeAssetSummaryRow(Assets[Index]);
		if (bIncludeDetails)
		{
			if (UInputMappingContext* Context = Cast<UInputMappingContext>(Assets[Index].GetAsset()))
			{
				Row = MappingContextToJson(Context, 0, MappingLimit);
			}
			else
			{
				Row->SetStringField(TEXT("load_error"), TEXT("Asset could not be loaded as UInputMappingContext"));
			}
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), PackagePrefix);
	Result->SetBoolField(TEXT("include_details"), bIncludeDetails);
	Result->SetNumberField(TEXT("mapping_limit"), MappingLimit);
	Result->SetArrayField(TEXT("contexts"), Rows);
	SetPageFields(Result, Assets.Num(), Offset, Limit, Rows.Num());
	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// get_input_mapping_context
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASInputAssetActions::HandleGetInputMappingContext(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	int32 MappingOffset = 0;
	int32 MappingLimit = DefaultMappingPageLimit;
	FMonolithActionResult Error;
	if (!ReadRequiredString(Params, TEXT("asset_path"), AssetPath, Error)
		|| !ReadBoundedInt(Params, TEXT("mapping_offset"), 0, 0, MAX_int32, MappingOffset, Error)
		|| !ReadBoundedInt(Params, TEXT("mapping_limit"), DefaultMappingPageLimit, 1, MaxMappingPageLimit,
			MappingLimit, Error))
	{
		return Error;
	}

	UObject* Asset = LoadExact(UInputMappingContext::StaticClass(), AssetPath, TEXT("asset_path"),
		TEXT("UInputMappingContext"), Error);
	return Asset
		? FMonolithActionResult::Success(
			MappingContextToJson(CastChecked<UInputMappingContext>(Asset), MappingOffset, MappingLimit))
		: Error;
}

// ─────────────────────────────────────────────────────────────────────────────
// validate_input_mappings
//
// Two independent completeness axes, both reported:
//   page_complete        — every mapping of every context ON THIS PAGE was scanned
//   all_contexts_covered — this page spans the whole selected context set
// `complete` requires both; `valid` requires `complete` AND zero errors.
// Duplicate key assignments are warnings: Enhanced Input legitimately maps one
// key to several actions (modifier chords, context layering).
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASInputAssetActions::HandleValidateInputMappings(const TSharedPtr<FJsonObject>& Params)
{
	const bool bExplicitContexts = HasParam(Params, TEXT("context_paths"));
	if (bExplicitContexts && HasParam(Params, TEXT("path")))
	{
		return InvalidParam(TEXT("path"), TEXT("path and context_paths are mutually exclusive"));
	}

	TArray<FString> ContextPaths;
	FMonolithActionResult Error;
	if (!ReadValidationContextPaths(Params, ContextPaths, Error))
	{
		return Error;
	}

	if (!bExplicitContexts)
	{
		FString Path;
		FString PackagePrefix;
		if (!ReadOptionalString(Params, TEXT("path"), TEXT("/Game"), Path, Error)
			|| !ParsePackagePrefix(Path, PackagePrefix, Error))
		{
			return Error;
		}

		TArray<FAssetData> Assets;
		QueryAssets(UInputMappingContext::StaticClass(), PackagePrefix, Assets);
		ContextPaths.Reserve(Assets.Num());
		for (const FAssetData& Asset : Assets)
		{
			ContextPaths.Add(Asset.GetObjectPathString());
		}
	}

	int32 Offset = 0;
	int32 Limit = DefaultAssetPageLimit;
	int32 MappingScanLimit = DefaultMappingScanLimit;
	if (!ReadBoundedInt(Params, TEXT("offset"), 0, 0, MAX_int32, Offset, Error)
		|| !ReadBoundedInt(Params, TEXT("limit"), DefaultAssetPageLimit, 1, MaxAssetPageLimit, Limit, Error)
		|| !ReadBoundedInt(Params, TEXT("mapping_scan_limit"), DefaultMappingScanLimit, 1, MaxMappingScanLimit,
			MappingScanLimit, Error))
	{
		return Error;
	}

	const int32 Start = FMath::Min(Offset, ContextPaths.Num());
	const int32 End = static_cast<int32>(FMath::Min<int64>(
		ContextPaths.Num(), static_cast<int64>(Offset) + Limit));

	TArray<TSharedPtr<FJsonValue>> ContextResults;
	ContextResults.Reserve(FMath::Max(0, End - Start));
	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	bool bPageComplete = true;

	for (int32 ContextIndex = Start; ContextIndex < End; ++ContextIndex)
	{
		const FString& ContextPath = ContextPaths[ContextIndex];
		TSharedPtr<FJsonObject> ContextResult = MakeShared<FJsonObject>();
		ContextResult->SetStringField(TEXT("context_path"), ContextPath);
		TArray<TSharedPtr<FJsonValue>> Issues;

		FMonolithActionResult LoadError;
		UObject* Asset = LoadExact(UInputMappingContext::StaticClass(), ContextPath, TEXT("context_paths"),
			TEXT("UInputMappingContext"), LoadError);
		if (!Asset)
		{
			// A context we could not load is an error AND a completeness hole — never
			// silently dropped and never substituted with another asset.
			++ErrorCount;
			bPageComplete = false;
			Issues.Add(MakeShared<FJsonValueObject>(MakeValidationIssue(
				TEXT("context_load_failed"), TEXT("error"), LoadError.ErrorMessage)));
			ContextResult->SetBoolField(TEXT("valid"), false);
			ContextResult->SetBoolField(TEXT("complete"), false);
			ContextResult->SetNumberField(TEXT("errors"), 1);
			ContextResult->SetNumberField(TEXT("warnings"), 0);
			ContextResult->SetArrayField(TEXT("issues"), Issues);
			ContextResults.Add(MakeShared<FJsonValueObject>(ContextResult));
			continue;
		}

		UInputMappingContext* Context = CastChecked<UInputMappingContext>(Asset);
		const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();
		const int32 ScanCount = FMath::Min(Mappings.Num(), MappingScanLimit);
		int32 ContextErrors = 0;
		int32 ContextWarnings = 0;

		TMap<FString, TArray<FString>> KeyToActions;
		for (int32 MappingIndex = 0; MappingIndex < ScanCount; ++MappingIndex)
		{
			const FEnhancedActionKeyMapping& Mapping = Mappings[MappingIndex];
			if (!Mapping.Action)
			{
				++ContextErrors;
				Issues.Add(MakeShared<FJsonValueObject>(MakeValidationIssue(TEXT("missing_action"), TEXT("error"),
					TEXT("Mapping has no InputAction"), MappingIndex)));
			}
			if (!Mapping.Key.IsValid())
			{
				++ContextErrors;
				Issues.Add(MakeShared<FJsonValueObject>(MakeValidationIssue(TEXT("invalid_key"), TEXT("error"),
					TEXT("Mapping has an invalid FKey"), MappingIndex)));
			}
			if (Mapping.Action && Mapping.Key.IsValid())
			{
				KeyToActions.FindOrAdd(Mapping.Key.ToString()).AddUnique(Mapping.Action->GetPathName());
			}
		}

		TArray<FString> Keys;
		KeyToActions.GetKeys(Keys);
		Keys.Sort();
		for (const FString& Key : Keys)
		{
			TArray<FString> Actions = KeyToActions.FindChecked(Key);
			if (Actions.Num() < 2)
			{
				continue;
			}
			Actions.Sort();
			++ContextWarnings;

			TSharedPtr<FJsonObject> Issue = MakeValidationIssue(TEXT("duplicate_key_assignment"), TEXT("warning"),
				FString::Printf(TEXT("Key '%s' is assigned to multiple actions"), *Key));
			Issue->SetStringField(TEXT("key"), Key);
			TArray<TSharedPtr<FJsonValue>> ActionValues;
			ActionValues.Reserve(Actions.Num());
			for (const FString& Action : Actions)
			{
				ActionValues.Add(MakeShared<FJsonValueString>(Action));
			}
			Issue->SetArrayField(TEXT("actions"), ActionValues);
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}

		const bool bContextComplete = ScanCount == Mappings.Num();
		if (!bContextComplete)
		{
			// An unscanned tail cannot be reported as clean, so the cutoff is an error.
			++ContextErrors;
			bPageComplete = false;
			Issues.Add(MakeShared<FJsonValueObject>(MakeValidationIssue(
				TEXT("mapping_scan_limit_exceeded"), TEXT("error"),
				FString::Printf(TEXT("Validation scanned %d of %d mappings"), ScanCount, Mappings.Num()))));
		}

		ErrorCount += ContextErrors;
		WarningCount += ContextWarnings;
		ContextResult->SetStringField(TEXT("asset_path"), Context->GetPathName());
		ContextResult->SetNumberField(TEXT("mapping_count"), Mappings.Num());
		ContextResult->SetNumberField(TEXT("mappings_scanned"), ScanCount);
		ContextResult->SetNumberField(TEXT("mapping_scan_limit"), MappingScanLimit);
		ContextResult->SetBoolField(TEXT("complete"), bContextComplete);
		ContextResult->SetBoolField(TEXT("valid"), bContextComplete && ContextErrors == 0);
		ContextResult->SetNumberField(TEXT("errors"), ContextErrors);
		ContextResult->SetNumberField(TEXT("warnings"), ContextWarnings);
		ContextResult->SetArrayField(TEXT("issues"), Issues);
		ContextResults.Add(MakeShared<FJsonValueObject>(ContextResult));
	}

	const bool bAllContextsCovered = Start == 0 && End == ContextPaths.Num();
	const bool bComplete = bPageComplete && bAllContextsCovered;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("valid"), bComplete && ErrorCount == 0);
	Result->SetBoolField(TEXT("complete"), bComplete);
	Result->SetBoolField(TEXT("page_complete"), bPageComplete);
	Result->SetBoolField(TEXT("all_contexts_covered"), bAllContextsCovered);
	Result->SetNumberField(TEXT("errors"), ErrorCount);
	Result->SetNumberField(TEXT("warnings"), WarningCount);
	Result->SetNumberField(TEXT("mapping_scan_limit"), MappingScanLimit);
	Result->SetArrayField(TEXT("contexts"), ContextResults);
	SetPageFields(Result, ContextPaths.Num(), Offset, Limit, ContextResults.Num());
	return FMonolithActionResult::Success(Result);
}
