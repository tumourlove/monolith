// SPDX-License-Identifier: MIT
// Read-only localization preflight — culture discovery and bounded StringTable
// inspection.
//
// Everything here is a projection: each action states its bounds up front and
// reports, separately, whether the page it returned covered the whole asset. A
// truncated scan must never read as a clean one, so `validate_string_table`
// reports valid=true only when the scan completed AND found no errors.
//
// Engine-generic: reads go through UStringTable::GetStringTable() (the const
// FStringTable) and the AssetRegistry. Nothing here calls Modify,
// MarkPackageDirty, SetSourceString or any transaction. Game-thread only.
//
// NOT implemented on purpose: a case-insensitive duplicate-key audit. FTextKey
// identity does not retain simultaneous "Case" and "case" rows in a
// StringTable, so the check is unreachable on both UE 5.7 and UE 5.8.

#include "MonolithLocalizationActions.h"

#include "MonolithAssetUtils.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithParamSchema.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/AllowShrinking.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/TextKey.h"
#include "Misc/PackageName.h"
#include "UObject/Class.h"
#include "UObject/Package.h"

namespace MonolithLocalizationInternal
{
	// ------------------------------------------------------------------
	//  Hard bounds. Every list this dispatcher can return is capped here.
	// ------------------------------------------------------------------
	constexpr int32 DefaultCultureLimit   = 100;
	constexpr int32 MaxCultureLimit       = 500;
	constexpr int32 MaxCultureNames       = 256;
	constexpr int32 DefaultTableLimit     = 200;
	constexpr int32 MaxTableLimit         = 1000;
	constexpr int32 DefaultEntryLimit     = 200;
	constexpr int32 MaxEntryLimit         = 1000;
	constexpr int32 DefaultMetadataLimit  = 512;
	constexpr int32 MaxMetadataLimit      = 4096;
	constexpr int32 DefaultTextLimit      = 4096;
	constexpr int32 MaxTextLimit          = 65536;
	constexpr int32 DefaultScanLimit      = 4096;
	constexpr int32 MaxScanLimit          = 10000;
	constexpr int32 DefaultIssueLimit     = 200;
	constexpr int32 MaxIssueLimit         = 1000;
	constexpr int32 MaxCursorLength       = 4096;

	/** JSON-RPC "invalid params". */
	constexpr int32 ErrInvalidParams = -32602;

	static FMonolithActionResult BadParam(const TCHAR* Field, const FString& Detail)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid parameter '%s': %s"), Field, *Detail), ErrInvalidParams);
	}

	/**
	 * Reads one action's parameters, latching the first failure. A handler can
	 * therefore read every field it needs and test validity once, instead of
	 * threading an error out of each individual parse.
	 */
	class FParamReader
	{
	public:
		explicit FParamReader(const TSharedPtr<FJsonObject>& InParams) : Params(InParams) {}

		bool IsValid() const { return bOk; }
		const FMonolithActionResult& GetError() const { return Error; }

		bool Has(const TCHAR* Field) const { return Params.IsValid() && Params->HasField(Field); }

		/** Integer in [Min, Max]. Rejects non-numbers, fractions and out-of-range values. */
		int32 Int(const TCHAR* Field, int32 Default, int32 Min, int32 Max)
		{
			if (!bOk || !Has(Field)) { return Default; }

			const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
			double Number = 0.0;
			if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(Number))
			{
				Fail(Field, TEXT("expected an integer JSON number"));
				return Default;
			}
			if (!FMath::IsFinite(Number)
				|| Number != FMath::TruncToDouble(Number)
				|| Number < static_cast<double>(Min)
				|| Number > static_cast<double>(Max))
			{
				Fail(Field, FString::Printf(TEXT("expected an integer in the range %d..%d"), Min, Max));
				return Default;
			}
			return static_cast<int32>(Number);
		}

		bool Flag(const TCHAR* Field, bool Default)
		{
			if (!bOk || !Has(Field)) { return Default; }

			const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
			bool Out = Default;
			if (!Value.IsValid() || Value->Type != EJson::Boolean || !Value->TryGetBool(Out))
			{
				Fail(Field, TEXT("expected a boolean"));
				return Default;
			}
			return Out;
		}

		FString Str(const TCHAR* Field, const FString& Default = FString())
		{
			if (!bOk || !Has(Field)) { return Default; }

			const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
			FString Out;
			if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Out))
			{
				Fail(Field, TEXT("expected a string"));
				return Default;
			}
			return Out;
		}

		FString RequiredStr(const TCHAR* Field)
		{
			if (bOk && !Has(Field))
			{
				Fail(Field, TEXT("field is required"));
				return FString();
			}
			const FString Out = Str(Field);
			if (bOk && Out.IsEmpty())
			{
				Fail(Field, TEXT("expected a non-empty string"));
			}
			return Out;
		}

		/** Non-empty array of non-empty strings, capped at MaxCount elements. */
		TArray<FString> StrArray(const TCHAR* Field, int32 MaxCount)
		{
			TArray<FString> Out;
			if (!bOk || !Has(Field)) { return Out; }

			const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Value.IsValid() || Value->Type != EJson::Array || !Value->TryGetArray(Items)
				|| Items == nullptr || Items->IsEmpty())
			{
				Fail(Field, TEXT("expected a non-empty string array"));
				return Out;
			}
			if (Items->Num() > MaxCount)
			{
				Fail(Field, FString::Printf(TEXT("at most %d values are accepted"), MaxCount));
				return Out;
			}

			Out.Reserve(Items->Num());
			for (int32 Index = 0; Index < Items->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Item = (*Items)[Index];
				FString Text;
				if (!Item.IsValid() || Item->Type != EJson::String || !Item->TryGetString(Text) || Text.IsEmpty())
				{
					Fail(Field, FString::Printf(TEXT("element %d must be a non-empty string"), Index));
					Out.Reset();
					return Out;
				}
				Out.Add(MoveTemp(Text));
			}
			return Out;
		}

		void Fail(const TCHAR* Field, const FString& Detail)
		{
			if (bOk)
			{
				bOk = false;
				Error = BadParam(Field, Detail);
			}
		}

	private:
		TSharedPtr<FJsonObject> Params;
		FMonolithActionResult Error;
		bool bOk = true;
	};

	// ------------------------------------------------------------------
	//  Path validation — canonical mounted paths only.
	// ------------------------------------------------------------------

	/**
	 * Accepts a canonical mounted package root such as "/Game" or
	 * "/Game/Localization".
	 *
	 * IsValidLongPackageName already rejects trailing slashes, "//",
	 * backslashes, ':', '.', whitespace and unmounted roots. Validating
	 * "<Input>/x" additionally lets a bare mount root through, which on its own
	 * is too short to be a package name.
	 */
	static bool ParsePackageRoot(const FString& Input, FMonolithActionResult& OutError)
	{
		if (!FPackageName::IsValidLongPackageName(Input + TEXT("/x")))
		{
			OutError = BadParam(TEXT("path"),
				TEXT("expected a canonical mounted package root, e.g. /Game or /Game/Localization"));
			return false;
		}
		return true;
	}

	/**
	 * Accepts a canonical mounted package path ("/Game/Localization/ST_UI") or
	 * the matching top-level object path ("/Game/Localization/ST_UI.ST_UI").
	 * A dotted path whose object name is not the package leaf is rejected —
	 * these actions inspect assets, not sub-objects.
	 */
	static bool ParseAssetPath(const FString& Input, const TCHAR* Field, FString& OutObjectPath,
		FMonolithActionResult& OutError)
	{
		FString PackagePath;
		const FString Reason = MonolithCore::ValidatePackagePath(Input, PackagePath);
		if (!Reason.IsEmpty())
		{
			OutError = BadParam(Field, Reason);
			return false;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
		if (AssetName.IsEmpty())
		{
			OutError = BadParam(Field, TEXT("path has no asset name"));
			return false;
		}

		OutObjectPath = PackagePath + TEXT(".") + AssetName;
		return true;
	}

	static UStringTable* LoadStringTable(const FString& Input, const TCHAR* Field, FString& OutObjectPath,
		FMonolithActionResult& OutError)
	{
		if (!ParseAssetPath(Input, Field, OutObjectPath, OutError))
		{
			return nullptr;
		}

		// Look the asset up as a bare UObject so a wrong-class hit reports what
		// it actually found instead of a bare "not found".
		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(UObject::StaticClass(), OutObjectPath);
		if (!Asset)
		{
			OutError = FMonolithActionResult::Error(
				FString::Printf(TEXT("StringTable not found: %s"), *OutObjectPath));
			return nullptr;
		}

		UStringTable* Table = Cast<UStringTable>(Asset);
		if (!Table)
		{
			OutError = FMonolithActionResult::Error(FString::Printf(
				TEXT("Asset '%s' is a %s, not a StringTable"), *OutObjectPath, *Asset->GetClass()->GetName()));
		}
		return Table;
	}

	// ------------------------------------------------------------------
	//  Bounded key selection.
	// ------------------------------------------------------------------

	/**
	 * Total order over StringTable keys.
	 *
	 * FTextKey identity is case sensitive but FString's own operator< is NOT
	 * (it runs Stricmp), so page order and the after_key cursor both go through
	 * this comparator to stay on one definition of "smallest".
	 */
	static int32 CompareKeys(const FString& A, const FString& B)
	{
		return A.Compare(B, ESearchCase::CaseSensitive);
	}

	static void TrimToSmallestKeys(TArray<FString>& Keys, int32 KeepCount)
	{
		Keys.Sort([](const FString& A, const FString& B) { return CompareKeys(A, B) < 0; });
		if (Keys.Num() > KeepCount)
		{
			Keys.SetNum(KeepCount, EAllowShrinking::No);
		}
	}

	/**
	 * Selects the KeepCount smallest keys that sort strictly after AfterKey.
	 *
	 * The working buffer is compacted every time it reaches 2*KeepCount, so
	 * peak memory tracks the requested page size, not the size of the table —
	 * a million-entry table costs the same as a hundred-entry one.
	 *
	 * @param OutTotal     every key the table enumerated.
	 * @param OutEligible  keys that sorted after the cursor (== OutTotal when no cursor).
	 */
	static void CollectSmallestKeys(const FStringTableConstRef& Table, const FString* AfterKey, int32 KeepCount,
		TArray<FString>& OutKeys, int32& OutTotal, int32& OutEligible)
	{
		OutKeys.Reset();
		OutTotal = 0;
		OutEligible = 0;

		const int32 CompactAt = FMath::Max(KeepCount, 1) * 2;
		Table->EnumerateKeysAndSourceStrings(
			[AfterKey, KeepCount, CompactAt, &OutKeys, &OutTotal, &OutEligible](const FTextKey& Key, const FString&) -> bool
			{
				++OutTotal;
				if (KeepCount <= 0)
				{
					return true;
				}

				FString KeyString = Key.ToString();
				if (AfterKey && CompareKeys(KeyString, *AfterKey) <= 0)
				{
					return true;
				}

				++OutEligible;
				OutKeys.Add(MoveTemp(KeyString));
				if (OutKeys.Num() >= CompactAt)
				{
					TrimToSmallestKeys(OutKeys, KeepCount);
				}
				return true;
			});

		TrimToSmallestKeys(OutKeys, KeepCount);
	}

	static int32 CountEntries(const FStringTableConstRef& Table)
	{
		int32 Count = 0;
		Table->EnumerateKeysAndSourceStrings([&Count](const FTextKey&, const FString&) -> bool
		{
			++Count;
			return true;
		});
		return Count;
	}

	// ------------------------------------------------------------------
	//  Bounded text + metadata.
	// ------------------------------------------------------------------

	struct FBoundedText
	{
		FString Value;
		int32 SourceLength = 0;

		bool IsTruncated() const { return Value.Len() < SourceLength; }
	};

	static FBoundedText BoundText(const FString& Value, int32 Limit)
	{
		FBoundedText Result;
		Result.SourceLength = Value.Len();
		Result.Value = Value.Left(Limit);
		return Result;
	}

	/** Emits `<Field>`, `<Field>_length` (pre-truncation) and `<Field>_truncated`. */
	static void SetBoundedText(const TSharedPtr<FJsonObject>& Json, const FString& Field, const FBoundedText& Text)
	{
		Json->SetStringField(Field, Text.Value);
		Json->SetNumberField(Field + TEXT("_length"), Text.SourceLength);
		Json->SetBoolField(Field + TEXT("_truncated"), Text.IsTruncated());
	}

	struct FMetadataRow
	{
		FString Name;
		FBoundedText Value;
	};

	static void TrimToSmallestMetadata(TArray<FMetadataRow>& Rows, int32 KeepCount)
	{
		Rows.Sort([](const FMetadataRow& A, const FMetadataRow& B) { return CompareKeys(A.Name, B.Name) < 0; });
		if (Rows.Num() > KeepCount)
		{
			Rows.SetNum(KeepCount, EAllowShrinking::No);
		}
	}

	/**
	 * Metadata rows for one entry, smallest-name first, capped at KeepCount.
	 * KeepCount == 0 still counts (so the caller can report what it skipped)
	 * but materializes nothing.
	 */
	static void CollectMetadata(const FStringTableConstRef& Table, const FString& Key, int32 KeepCount,
		int32 TextLimit, TArray<FMetadataRow>& OutRows, int32& OutTotal)
	{
		OutRows.Reset();
		OutTotal = 0;

		const int32 CompactAt = FMath::Max(KeepCount, 1) * 2;
		Table->EnumerateMetaData(FTextKey(Key),
			[KeepCount, CompactAt, TextLimit, &OutRows, &OutTotal](FName MetadataId, const FString& MetadataValue) -> bool
			{
				++OutTotal;
				if (KeepCount <= 0)
				{
					return true;
				}

				FMetadataRow Row;
				Row.Name = MetadataId.ToString();
				Row.Value = BoundText(MetadataValue, TextLimit);
				OutRows.Add(MoveTemp(Row));
				if (OutRows.Num() >= CompactAt)
				{
					TrimToSmallestMetadata(OutRows, KeepCount);
				}
				return true;
			});

		TrimToSmallestMetadata(OutRows, KeepCount);
	}

	// ------------------------------------------------------------------
	//  JSON shaping.
	// ------------------------------------------------------------------

	static void SetPage(const TSharedPtr<FJsonObject>& Json, int32 Total, int32 Offset, int32 Limit, int32 Count)
	{
		Json->SetNumberField(TEXT("total"), Total);
		Json->SetNumberField(TEXT("offset"), Offset);
		Json->SetNumberField(TEXT("limit"), Limit);
		Json->SetNumberField(TEXT("count"), Count);
		Json->SetBoolField(TEXT("has_more"), static_cast<int64>(Offset) + Count < Total);
	}

	static void AddTableIdentity(const TSharedPtr<FJsonObject>& Json, const UStringTable* Table)
	{
		const FStringTableConstRef StringTable = Table->GetStringTable();
		Json->SetStringField(TEXT("asset_path"), Table->GetPathName());
		Json->SetStringField(TEXT("package_path"), Table->GetOutermost()->GetName());
		Json->SetStringField(TEXT("name"), Table->GetName());
		Json->SetStringField(TEXT("table_id"), Table->GetStringTableId().ToString());
		Json->SetStringField(TEXT("namespace"), StringTable->GetNamespace());
		Json->SetBoolField(TEXT("is_internal"), StringTable->IsInternal());
	}

	static TSharedPtr<FJsonObject> CultureToJson(const FCultureRef& Culture)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Culture->GetName());
		Json->SetStringField(TEXT("native_name"), Culture->GetNativeName());
		Json->SetStringField(TEXT("english_name"), Culture->GetEnglishName());
		Json->SetStringField(TEXT("display_name"), Culture->GetDisplayName());
		Json->SetStringField(TEXT("two_letter_iso"), Culture->GetTwoLetterISOLanguageName());
		Json->SetStringField(TEXT("three_letter_iso"), Culture->GetThreeLetterISOLanguageName());
		return Json;
	}

	// ------------------------------------------------------------------
	//  Validation issues.
	// ------------------------------------------------------------------

	struct FValidationIssue
	{
		FString Code;
		FString Message;
		FString Key;
		bool bIsError = false;
	};

	static void AddIssue(TArray<FValidationIssue>& Issues, const TCHAR* Code, bool bIsError,
		const FString& Message, const FString& Key = FString())
	{
		FValidationIssue Issue;
		Issue.Code = Code;
		Issue.Message = Message;
		Issue.Key = Key;
		Issue.bIsError = bIsError;
		Issues.Add(MoveTemp(Issue));
	}

	static TSharedPtr<FJsonObject> IssueToJson(const FValidationIssue& Issue)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("code"), Issue.Code);
		Json->SetStringField(TEXT("severity"), Issue.bIsError ? TEXT("error") : TEXT("warning"));
		Json->SetStringField(TEXT("message"), Issue.Message);
		if (!Issue.Key.IsEmpty())
		{
			Json->SetStringField(TEXT("key"), Issue.Key);
		}
		return Json;
	}
}

// ============================================================
//  Registration
// ============================================================

void FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	using namespace MonolithLocalizationInternal;

	// Every action in this namespace is a pure projection, so the dispatcher as
	// a whole is safe to advertise as read-only and idempotent.
	FMonolithDispatcherAnnotations Annotations;
	Annotations.bReadOnlyHint = true;
	Annotations.bIdempotentHint = true;
	Annotations.Title = TEXT("Localization discovery and StringTable validation");
	Registry.SetDispatcherAnnotations(TEXT("localization"), Annotations);

	Registry.RegisterAction(TEXT("localization"), TEXT("list_cultures"),
		TEXT("List Unreal cultures with bounded pagination. Without culture_names it pages every known culture; with them it resolves those roots (optionally including derived cultures) and reports the ones that did not resolve. Returns {current_culture, current_language, current_locale, unresolved_names, cultures:[{name, native_name, english_name, display_name, two_letter_iso, three_letter_iso}], total, offset, limit, count, has_more}."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::ListCultures),
		FParamSchemaBuilder()
			.Optional(TEXT("culture_names"), TEXT("array"),   TEXT("Culture roots to resolve, e.g. [\"en\",\"fr\"]. Max 256. Omit to list every known culture."))
			.Optional(TEXT("include_derived"), TEXT("boolean"), TEXT("Include cultures derived from each requested root (only used with culture_names)."), TEXT("true"))
			.Optional(TEXT("offset"),        TEXT("integer"), TEXT("Zero-based result offset."), TEXT("0"))
			.Optional(TEXT("limit"),         TEXT("integer"), TEXT("Max cultures to return (1-500)."), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("localization"), TEXT("list_string_tables"),
		TEXT("Discover StringTable assets under a mounted package root via the AssetRegistry, with bounded pagination. include_details loads ONLY the returned page and adds namespace, table_id, is_internal and entry_count per row. Returns {path, string_tables:[{asset_path, package_path, name, ...}], total, offset, limit, count, has_more}."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::ListStringTables),
		FParamSchemaBuilder()
			.OptionalAssetPathWithDefault(TEXT("path"), TEXT("Canonical mounted package root to search recursively, e.g. /Game/Localization"), TEXT("/Game"))
			.Optional(TEXT("offset"),          TEXT("integer"), TEXT("Zero-based result offset."), TEXT("0"))
			.Optional(TEXT("limit"),           TEXT("integer"), TEXT("Max tables to return (1-1000)."), TEXT("200"))
			.Optional(TEXT("include_details"), TEXT("boolean"), TEXT("Load the returned page and include namespace + entry count."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("localization"), TEXT("get_string_table"),
		TEXT("Read one bounded page of StringTable entries in stable key order, resuming from an exclusive after_key cursor. Metadata rows and text values have independent budgets. Returns {entry_count, entries_after_cursor, entries:[{key, source_string, source_string_length, source_string_truncated, metadata?}], has_more_entries, next_after_key?, all_entries_covered, metadata_complete, complete}."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::GetStringTable),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Canonical StringTable package path or matching top-level object path"))
			.Optional(TEXT("after_key"),        TEXT("string"),  TEXT("Exclusive cursor: return keys sorting strictly after this one. Max 4096 chars."))
			.Optional(TEXT("entry_limit"),      TEXT("integer"), TEXT("Max entries to return (1-1000)."), TEXT("200"))
			.Optional(TEXT("include_metadata"), TEXT("boolean"), TEXT("Include per-entry metadata rows within the shared metadata_limit budget."), TEXT("false"))
			.Optional(TEXT("metadata_limit"),   TEXT("integer"), TEXT("Total metadata rows returned across the whole page (0-4096)."), TEXT("512"))
			.Optional(TEXT("text_limit"),       TEXT("integer"), TEXT("Max characters per source string or metadata value (1-65536)."), TEXT("4096"))
			.Build());

	Registry.RegisterAction(TEXT("localization"), TEXT("validate_string_table"),
		TEXT("Validate a bounded prefix of a StringTable's keys and source strings in stable key order, with paginated issues. Reports errors (empty key, empty table, scan_limit exceeded) apart from warnings (edge whitespace, empty source string). valid=true ONLY when the scan covered every entry AND found zero errors. Returns {entry_count, entries_scanned, complete, valid, errors, warnings, issues:[{code, severity, message, key?}], issue_total, has_more_issues}."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::ValidateStringTable),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Canonical StringTable package path or matching top-level object path"))
			.Optional(TEXT("scan_limit"),   TEXT("integer"), TEXT("Max entries validated, in key order (1-10000). A table larger than this reports complete=false."), TEXT("4096"))
			.Optional(TEXT("issue_offset"), TEXT("integer"), TEXT("Zero-based issue offset."), TEXT("0"))
			.Optional(TEXT("issue_limit"),  TEXT("integer"), TEXT("Max issues to return (1-1000)."), TEXT("200"))
			.Build());
}

// ============================================================
//  list_cultures
// ============================================================

FMonolithActionResult FMonolithLocalizationActions::ListCultures(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithLocalizationInternal;

	FParamReader Reader(Params);
	const bool bNamesProvided       = Reader.Has(TEXT("culture_names"));
	const TArray<FString> Requested = Reader.StrArray(TEXT("culture_names"), MaxCultureNames);
	const bool bIncludeDerived      = Reader.Flag(TEXT("include_derived"), true);
	const int32 Offset              = Reader.Int(TEXT("offset"), 0, 0, MAX_int32);
	const int32 Limit               = Reader.Int(TEXT("limit"), DefaultCultureLimit, 1, MaxCultureLimit);
	if (!Reader.IsValid())
	{
		return Reader.GetError();
	}

	FInternationalization& I18N = FInternationalization::Get();

	TArray<FCultureRef> Cultures;
	TArray<TSharedPtr<FJsonValue>> Unresolved;
	if (bNamesProvided)
	{
		Cultures = I18N.GetAvailableCultures(Requested, bIncludeDerived);
		for (const FString& Name : Requested)
		{
			if (!I18N.GetCulture(Name).IsValid())
			{
				Unresolved.Add(MakeShared<FJsonValueString>(Name));
			}
		}
	}
	else
	{
		TArray<FString> Names;
		I18N.GetCultureNames(Names);
		Cultures.Reserve(Names.Num());
		for (const FString& Name : Names)
		{
			if (const FCulturePtr Culture = I18N.GetCulture(Name))
			{
				Cultures.Add(Culture.ToSharedRef());
			}
		}
	}

	Cultures.Sort([](const FCultureRef& A, const FCultureRef& B)
	{
		return CompareKeys(A->GetName(), B->GetName()) < 0;
	});

	// Overlapping roots ("en" and "en-US") can surface the same culture twice —
	// page over a de-duplicated view so offsets stay meaningful.
	TArray<FCultureRef> Unique;
	Unique.Reserve(Cultures.Num());
	for (const FCultureRef& Culture : Cultures)
	{
		if (Unique.IsEmpty() || CompareKeys(Unique.Last()->GetName(), Culture->GetName()) != 0)
		{
			Unique.Add(Culture);
		}
	}

	const int32 Start = FMath::Min(Offset, Unique.Num());
	const int32 End = static_cast<int32>(FMath::Min<int64>(Unique.Num(), static_cast<int64>(Offset) + Limit));
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(FMath::Max(End - Start, 0));
	for (int32 Index = Start; Index < End; ++Index)
	{
		Rows.Add(MakeShared<FJsonValueObject>(CultureToJson(Unique[Index])));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("current_culture"), I18N.GetCurrentCulture()->GetName());
	Result->SetStringField(TEXT("current_language"), I18N.GetCurrentLanguage()->GetName());
	Result->SetStringField(TEXT("current_locale"), I18N.GetCurrentLocale()->GetName());
	Result->SetBoolField(TEXT("explicit_names"), bNamesProvided);
	Result->SetBoolField(TEXT("include_derived"), bIncludeDerived);
	Result->SetArrayField(TEXT("unresolved_names"), Unresolved);
	Result->SetArrayField(TEXT("cultures"), Rows);
	Result->SetBoolField(TEXT("read_only"), true);
	SetPage(Result, Unique.Num(), Offset, Limit, Rows.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  list_string_tables
// ============================================================

FMonolithActionResult FMonolithLocalizationActions::ListStringTables(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithLocalizationInternal;

	FParamReader Reader(Params);
	const FString PackageRoot   = Reader.Str(TEXT("path"), TEXT("/Game"));
	const int32 Offset          = Reader.Int(TEXT("offset"), 0, 0, MAX_int32);
	const int32 Limit           = Reader.Int(TEXT("limit"), DefaultTableLimit, 1, MaxTableLimit);
	const bool bIncludeDetails  = Reader.Flag(TEXT("include_details"), false);
	if (!Reader.IsValid())
	{
		return Reader.GetError();
	}

	FMonolithActionResult PathError;
	if (!ParsePackageRoot(PackageRoot, PathError))
	{
		return PathError;
	}

	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	if (!AssetRegistry)
	{
		return FMonolithActionResult::Error(TEXT("AssetRegistry is unavailable"));
	}

	FARFilter Filter;
	Filter.ClassPaths.Add(UStringTable::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.PackagePaths.Add(FName(*PackageRoot));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AssetRegistry->GetAssets(Filter, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return CompareKeys(A.GetObjectPathString(), B.GetObjectPathString()) < 0;
	});

	const int32 Start = FMath::Min(Offset, Assets.Num());
	const int32 End = static_cast<int32>(FMath::Min<int64>(Assets.Num(), static_cast<int64>(Offset) + Limit));
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(FMath::Max(End - Start, 0));
	for (int32 Index = Start; Index < End; ++Index)
	{
		const FAssetData& AssetData = Assets[Index];
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		Row->SetStringField(TEXT("package_path"), AssetData.PackageName.ToString());
		Row->SetStringField(TEXT("name"), AssetData.AssetName.ToString());

		// Only the returned page is loaded — discovery itself stays registry-only.
		if (bIncludeDetails)
		{
			if (UStringTable* Table = Cast<UStringTable>(AssetData.GetAsset()))
			{
				AddTableIdentity(Row, Table);
				Row->SetNumberField(TEXT("entry_count"), CountEntries(Table->GetStringTable()));
			}
			else
			{
				Row->SetStringField(TEXT("load_error"), TEXT("asset could not be loaded as a StringTable"));
			}
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), PackageRoot);
	Result->SetBoolField(TEXT("include_details"), bIncludeDetails);
	Result->SetArrayField(TEXT("string_tables"), Rows);
	Result->SetBoolField(TEXT("read_only"), true);
	SetPage(Result, Assets.Num(), Offset, Limit, Rows.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  get_string_table
// ============================================================

FMonolithActionResult FMonolithLocalizationActions::GetStringTable(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithLocalizationInternal;

	FParamReader Reader(Params);
	const FString AssetPath      = Reader.RequiredStr(TEXT("asset_path"));
	const bool bCursorProvided   = Reader.Has(TEXT("after_key"));
	const FString AfterKey       = Reader.Str(TEXT("after_key"));
	const int32 EntryLimit       = Reader.Int(TEXT("entry_limit"), DefaultEntryLimit, 1, MaxEntryLimit);
	const bool bIncludeMetadata  = Reader.Flag(TEXT("include_metadata"), false);
	const int32 MetadataLimit    = Reader.Int(TEXT("metadata_limit"), DefaultMetadataLimit, 0, MaxMetadataLimit);
	const int32 TextLimit        = Reader.Int(TEXT("text_limit"), DefaultTextLimit, 1, MaxTextLimit);
	if (Reader.IsValid() && AfterKey.Len() > MaxCursorLength)
	{
		Reader.Fail(TEXT("after_key"),
			FString::Printf(TEXT("must not exceed %d characters"), MaxCursorLength));
	}
	if (!Reader.IsValid())
	{
		return Reader.GetError();
	}

	FString ObjectPath;
	FMonolithActionResult LoadError;
	UStringTable* Table = LoadStringTable(AssetPath, TEXT("asset_path"), ObjectPath, LoadError);
	if (!Table)
	{
		return LoadError;
	}

	const FStringTableConstRef StringTable = Table->GetStringTable();

	// Ask for one key past the page so "is there another page?" needs no second scan.
	TArray<FString> Keys;
	int32 EntryCount = 0;
	int32 EligibleCount = 0;
	CollectSmallestKeys(StringTable, bCursorProvided ? &AfterKey : nullptr, EntryLimit + 1,
		Keys, EntryCount, EligibleCount);

	const bool bHasMoreEntries = Keys.Num() > EntryLimit;
	if (bHasMoreEntries)
	{
		Keys.SetNum(EntryLimit, EAllowShrinking::No);
	}

	int32 RemainingMetadata = bIncludeMetadata ? MetadataLimit : 0;
	int32 AvailableMetadata = 0;
	int32 ReturnedMetadata = 0;
	bool bMetadataComplete = true;

	TArray<TSharedPtr<FJsonValue>> Entries;
	Entries.Reserve(Keys.Num());
	for (const FString& Key : Keys)
	{
		FString SourceString;
		if (!StringTable->GetSourceString(FTextKey(Key), SourceString))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("StringTable entry '%s' disappeared during readback"), *Key));
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("key"), Key);
		Entry->SetNumberField(TEXT("key_length"), Key.Len());
		SetBoundedText(Entry, TEXT("source_string"), BoundText(SourceString, TextLimit));

		if (bIncludeMetadata)
		{
			TArray<FMetadataRow> MetadataRows;
			int32 MetadataCount = 0;
			CollectMetadata(StringTable, Key, RemainingMetadata, TextLimit, MetadataRows, MetadataCount);

			TArray<TSharedPtr<FJsonValue>> MetadataValues;
			MetadataValues.Reserve(MetadataRows.Num());
			for (const FMetadataRow& Row : MetadataRows)
			{
				TSharedPtr<FJsonObject> RowJson = MakeShared<FJsonObject>();
				RowJson->SetStringField(TEXT("name"), Row.Name);
				SetBoundedText(RowJson, TEXT("value"), Row.Value);
				MetadataValues.Add(MakeShared<FJsonValueObject>(RowJson));
			}

			Entry->SetArrayField(TEXT("metadata"), MetadataValues);
			Entry->SetNumberField(TEXT("metadata_count"), MetadataCount);
			Entry->SetNumberField(TEXT("metadata_returned"), MetadataRows.Num());
			Entry->SetBoolField(TEXT("metadata_truncated"), MetadataRows.Num() < MetadataCount);

			AvailableMetadata += MetadataCount;
			ReturnedMetadata += MetadataRows.Num();
			RemainingMetadata = FMath::Max(0, RemainingMetadata - MetadataRows.Num());
			bMetadataComplete &= MetadataRows.Num() == MetadataCount;
		}
		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// A cursor page is by definition a slice, so it can never claim coverage.
	const bool bAllEntriesCovered = !bCursorProvided && !bHasMoreEntries;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	AddTableIdentity(Result, Table);
	Result->SetNumberField(TEXT("entry_count"), EntryCount);
	Result->SetNumberField(TEXT("entries_after_cursor"), EligibleCount);
	Result->SetNumberField(TEXT("entry_limit"), EntryLimit);
	Result->SetNumberField(TEXT("entries_returned"), Entries.Num());
	Result->SetArrayField(TEXT("entries"), Entries);
	Result->SetBoolField(TEXT("has_more_entries"), bHasMoreEntries);
	Result->SetBoolField(TEXT("all_entries_covered"), bAllEntriesCovered);
	Result->SetBoolField(TEXT("include_metadata"), bIncludeMetadata);
	Result->SetNumberField(TEXT("metadata_limit"), MetadataLimit);
	Result->SetNumberField(TEXT("available_metadata_count"), AvailableMetadata);
	Result->SetNumberField(TEXT("returned_metadata_count"), ReturnedMetadata);
	Result->SetBoolField(TEXT("metadata_complete"), bMetadataComplete);
	Result->SetNumberField(TEXT("text_limit"), TextLimit);
	Result->SetBoolField(TEXT("complete"), bAllEntriesCovered && bMetadataComplete);
	Result->SetBoolField(TEXT("read_only"), true);
	if (bCursorProvided)
	{
		Result->SetStringField(TEXT("after_key"), AfterKey);
	}
	if (bHasMoreEntries && !Keys.IsEmpty())
	{
		Result->SetStringField(TEXT("next_after_key"), Keys.Last());
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  validate_string_table
// ============================================================

FMonolithActionResult FMonolithLocalizationActions::ValidateStringTable(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithLocalizationInternal;

	FParamReader Reader(Params);
	const FString AssetPath  = Reader.RequiredStr(TEXT("asset_path"));
	const int32 ScanLimit    = Reader.Int(TEXT("scan_limit"), DefaultScanLimit, 1, MaxScanLimit);
	const int32 IssueOffset  = Reader.Int(TEXT("issue_offset"), 0, 0, MAX_int32);
	const int32 IssueLimit   = Reader.Int(TEXT("issue_limit"), DefaultIssueLimit, 1, MaxIssueLimit);
	if (!Reader.IsValid())
	{
		return Reader.GetError();
	}

	FString ObjectPath;
	FMonolithActionResult LoadError;
	UStringTable* Table = LoadStringTable(AssetPath, TEXT("asset_path"), ObjectPath, LoadError);
	if (!Table)
	{
		return LoadError;
	}

	const FStringTableConstRef StringTable = Table->GetStringTable();

	TArray<FString> Keys;
	int32 EntryCount = 0;
	int32 EligibleCount = 0;
	CollectSmallestKeys(StringTable, nullptr, ScanLimit, Keys, EntryCount, EligibleCount);
	const bool bComplete = EntryCount <= ScanLimit;

	TArray<FValidationIssue> Issues;
	for (const FString& Key : Keys)
	{
		FString SourceString;
		if (!StringTable->GetSourceString(FTextKey(Key), SourceString))
		{
			AddIssue(Issues, TEXT("source_lookup_failed"), /*bIsError=*/true,
				TEXT("The entry disappeared while validation was reading it."), Key);
			continue;
		}

		if (Key.IsEmpty())
		{
			AddIssue(Issues, TEXT("empty_key"), /*bIsError=*/true,
				TEXT("StringTable entry has an empty key."));
		}

		FString TrimmedKey = Key;
		TrimmedKey.TrimStartAndEndInline();
		if (CompareKeys(TrimmedKey, Key) != 0)
		{
			AddIssue(Issues, TEXT("key_edge_whitespace"), /*bIsError=*/false,
				TEXT("StringTable key has leading or trailing whitespace."), Key);
		}

		if (SourceString.IsEmpty())
		{
			AddIssue(Issues, TEXT("empty_source_string"), /*bIsError=*/false,
				TEXT("StringTable entry has an empty source string."), Key);
		}
	}

	if (EntryCount == 0)
	{
		AddIssue(Issues, TEXT("empty_table"), /*bIsError=*/true, TEXT("StringTable has no entries."));
	}
	// An incomplete scan is itself an error, so a bounded run can never read as clean.
	if (!bComplete)
	{
		AddIssue(Issues, TEXT("scan_limit_exceeded"), /*bIsError=*/true, FString::Printf(
			TEXT("Validation scanned %d of %d entries; raise scan_limit for full coverage."),
			Keys.Num(), EntryCount));
	}

	Issues.Sort([](const FValidationIssue& A, const FValidationIssue& B)
	{
		const int32 ByKey = CompareKeys(A.Key, B.Key);
		if (ByKey != 0) { return ByKey < 0; }
		const int32 ByCode = CompareKeys(A.Code, B.Code);
		if (ByCode != 0) { return ByCode < 0; }
		return CompareKeys(A.Message, B.Message) < 0;
	});

	int32 ErrorCount = 0;
	for (const FValidationIssue& Issue : Issues)
	{
		ErrorCount += Issue.bIsError ? 1 : 0;
	}

	const int32 Start = FMath::Min(IssueOffset, Issues.Num());
	const int32 End = static_cast<int32>(FMath::Min<int64>(Issues.Num(), static_cast<int64>(IssueOffset) + IssueLimit));
	TArray<TSharedPtr<FJsonValue>> IssueRows;
	IssueRows.Reserve(FMath::Max(End - Start, 0));
	for (int32 Index = Start; Index < End; ++Index)
	{
		IssueRows.Add(MakeShared<FJsonValueObject>(IssueToJson(Issues[Index])));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	AddTableIdentity(Result, Table);
	Result->SetNumberField(TEXT("entry_count"), EntryCount);
	Result->SetNumberField(TEXT("entries_scanned"), Keys.Num());
	Result->SetNumberField(TEXT("scan_limit"), ScanLimit);
	Result->SetBoolField(TEXT("complete"), bComplete);
	Result->SetBoolField(TEXT("valid"), bComplete && ErrorCount == 0);
	Result->SetNumberField(TEXT("errors"), ErrorCount);
	Result->SetNumberField(TEXT("warnings"), Issues.Num() - ErrorCount);
	Result->SetNumberField(TEXT("issue_total"), Issues.Num());
	Result->SetNumberField(TEXT("issue_offset"), IssueOffset);
	Result->SetNumberField(TEXT("issue_limit"), IssueLimit);
	Result->SetNumberField(TEXT("issues_returned"), IssueRows.Num());
	Result->SetBoolField(TEXT("has_more_issues"),
		static_cast<int64>(IssueOffset) + IssueRows.Num() < Issues.Num());
	Result->SetArrayField(TEXT("issues"), IssueRows);
	Result->SetBoolField(TEXT("read_only"), true);
	return FMonolithActionResult::Success(Result);
}
