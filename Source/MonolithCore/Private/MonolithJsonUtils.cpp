#include "MonolithJsonUtils.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"

DEFINE_LOG_CATEGORY(LogMonolith);

TSharedPtr<FJsonObject> FMonolithJsonUtils::SuccessResponse(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonValue>& Result)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	if (Id.IsValid())
	{
		Response->SetField(TEXT("id"), Id);
	}
	if (Result.IsValid())
	{
		Response->SetField(TEXT("result"), Result);
	}
	else
	{
		Response->SetField(TEXT("result"), MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
	}
	return Response;
}

TSharedPtr<FJsonObject> FMonolithJsonUtils::ErrorResponse(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message, const TSharedPtr<FJsonValue>& Data)
{
	TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
	ErrorObj->SetNumberField(TEXT("code"), Code);
	ErrorObj->SetStringField(TEXT("message"), Message);
	if (Data.IsValid())
	{
		ErrorObj->SetField(TEXT("data"), Data);
	}

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	if (Id.IsValid())
	{
		Response->SetField(TEXT("id"), Id);
	}
	else
	{
		Response->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
	}
	Response->SetObjectField(TEXT("error"), ErrorObj);
	return Response;
}

TSharedPtr<FJsonObject> FMonolithJsonUtils::SuccessObject(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& ResultObj)
{
	return SuccessResponse(Id, MakeShared<FJsonValueObject>(ResultObj));
}

TSharedPtr<FJsonObject> FMonolithJsonUtils::SuccessString(const TSharedPtr<FJsonValue>& Id, const FString& Message)
{
	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("message"), Message);
	return SuccessObject(Id, ResultObj);
}

FString FMonolithJsonUtils::Serialize(const TSharedPtr<FJsonObject>& JsonObject)
{
	FString OutputString;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	return OutputString;
}

TSharedPtr<FJsonObject> FMonolithJsonUtils::Parse(const FString& JsonString)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		return nullptr;
	}
	return JsonObject;
}

TSharedRef<FJsonValueArray> FMonolithJsonUtils::StringArrayToJson(const TArray<FString>& Strings)
{
	TArray<TSharedPtr<FJsonValue>> JsonArray;
	for (const FString& Str : Strings)
	{
		JsonArray.Add(MakeShared<FJsonValueString>(Str));
	}
	return MakeShared<FJsonValueArray>(JsonArray);
}

// =============================================================================
//  Survivor B — Universal Response Shaping
//
//  Phase 1 of plan §3.B (Docs/plans/2026-05-27-mcp-llm-ergonomics.md).
//  TOP-LEVEL KEYS ONLY. JSONPath / nested traversal is out-of-scope (plan §2).
// =============================================================================

namespace MonolithResponseShapingDetail
{
	/** Read a string-array param into a TSet for O(1) membership. Returns false if absent or empty. */
	static bool ReadStringArrayParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, TSet<FString>& Out)
	{
		Out.Reset();
		if (!Params.IsValid())
		{
			return false;
		}

		// Canonical native-array path (covers automation tests + well-formed JSON callers).
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params->TryGetArrayField(Key, Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				FString S;
				if (V.IsValid() && V->TryGetString(S))
				{
					Out.Add(S);
				}
			}
			return Out.Num() > 0;
		}

		// String-fallback path: Claude Code serializes top-level array args as JSON-encoded
		// strings (e.g. `_fields:"[\"count\"]"`). Mirror the unwrap pattern used for "params"
		// in MonolithHttpServer.cpp:691-701.
		FString StrValue;
		if (Params->TryGetStringField(Key, StrValue) && !StrValue.IsEmpty())
		{
			TSharedPtr<FJsonValue> ParsedValue;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(StrValue);
			if (FJsonSerializer::Deserialize(Reader, ParsedValue) && ParsedValue.IsValid() && ParsedValue->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& ParsedArr = ParsedValue->AsArray();
				for (const TSharedPtr<FJsonValue>& V : ParsedArr)
				{
					FString S;
					if (V.IsValid() && V->TryGetString(S))
					{
						Out.Add(S);
					}
				}
				UE_LOG(LogMonolith, Verbose, TEXT("ReadStringArrayParam: recovered stringified JSON array for key '%s' (%d entries)"), Key, Out.Num());
				return Out.Num() > 0;
			}
		}

		return false;
	}

	/** True if the JSON value is an array AND every element is a JSON object. Empty arrays return false. */
	static bool IsArrayOfObjects(const TSharedPtr<FJsonValue>& Val)
	{
		if (!Val.IsValid() || Val->Type != EJson::Array)
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Val->TryGetArray(Arr) || !Arr || Arr->Num() == 0)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Elem : *Arr)
		{
			if (!Elem.IsValid() || Elem->Type != EJson::Object)
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * Phase 1.1 — `_row_fields` per-row whitelist.
	 *
	 * Locate the unique top-level array-of-objects key in Response. If exactly
	 * one such key exists, mutate each row in-place to retain only keys in
	 * RowFieldsSet. If multiple exist, emit an ambiguity warning and skip.
	 * If none exist, emit a "no list payload found" warning and skip.
	 * Empty RowFieldsSet is a no-op with warning.
	 */
	static void ApplyRowFieldsFilter(
		TSharedPtr<FJsonObject>& Response,
		const TSet<FString>& RowFieldsSet,
		TArray<FString>& Warnings)
	{
		if (!Response.IsValid())
		{
			return;
		}
		if (RowFieldsSet.Num() == 0)
		{
			Warnings.Add(TEXT("`_row_fields:[]` is empty — no-op. Provide one or more row field names to filter list payloads."));
			return;
		}

		// Find all top-level array-of-objects keys (the candidate list payloads).
		TArray<FString> ListPayloadKeys;
		for (const auto& Pair : Response->Values)
		{
			if (IsArrayOfObjects(Pair.Value))
			{
				ListPayloadKeys.Add(MonolithKeyToString(Pair.Key));
			}
		}

		if (ListPayloadKeys.Num() == 0)
		{
			Warnings.Add(TEXT("`_row_fields` skipped — no top-level array-of-objects (list payload) found in response."));
			return;
		}
		if (ListPayloadKeys.Num() > 1)
		{
			Warnings.Add(FString::Printf(
				TEXT("_row_fields ambiguous: multiple list payloads found (%s) — use _path_fields"),
				*FString::Join(ListPayloadKeys, TEXT(", "))));
			return;
		}

		// Exactly one list payload — filter each row.
		const FString& ListKey = ListPayloadKeys[0];
		const TArray<TSharedPtr<FJsonValue>>* RowsPtr = nullptr;
		if (!Response->TryGetArrayField(ListKey, RowsPtr) || !RowsPtr)
		{
			return;
		}

		// Track the union of all keys present across rows, and whether any
		// requested field actually matched a row key, so we can warn on a
		// total miss (the confusing "[{},{}]" case).
		TSet<FString> UnionKeys;
		bool bAnyRequestedKeyMatched = false;

		for (const TSharedPtr<FJsonValue>& RowVal : *RowsPtr)
		{
			if (!RowVal.IsValid() || RowVal->Type != EJson::Object)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* RowObjPtr = nullptr;
			if (!RowVal->TryGetObject(RowObjPtr) || !RowObjPtr || !(*RowObjPtr).IsValid())
			{
				continue;
			}
			TSharedPtr<FJsonObject> RowObj = *RowObjPtr;

			TArray<FString> ExistingKeys;
			for (const auto& KVP : RowObj->Values) { ExistingKeys.Add(MonolithKeyToString(KVP.Key)); }
			for (const FString& K : ExistingKeys)
			{
				UnionKeys.Add(K);
				if (RowFieldsSet.Contains(K))
				{
					bAnyRequestedKeyMatched = true;
				}
				else
				{
					RowObj->RemoveField(K);
				}
			}
		}

		// Total miss: not one requested field matched any row key. This is the
		// silent-failure case (rows collapse to empty objects with no signal).
		if (!bAnyRequestedKeyMatched && UnionKeys.Num() > 0)
		{
			TArray<FString> SortedKeys = UnionKeys.Array();
			SortedKeys.Sort();
			Warnings.Add(FString::Printf(
				TEXT("_row_fields matched no keys in the list payload; available row keys: [%s]"),
				*FString::Join(SortedKeys, TEXT(", "))));
		}
	}

	/**
	 * Walk a dotted path through Source. Returns the JSON value at the leaf,
	 * or an invalid TSharedPtr if any segment is missing or non-traversable.
	 * Path segments traverse objects only (we do NOT index into arrays).
	 */
	static TSharedPtr<FJsonValue> WalkDottedPath(
		const TSharedPtr<FJsonObject>& Source,
		const TArray<FString>& Segments)
	{
		if (!Source.IsValid() || Segments.Num() == 0)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> CurrentObj = Source;
		for (int32 Idx = 0; Idx < Segments.Num(); ++Idx)
		{
			const FString& Seg = Segments[Idx];
			if (!CurrentObj.IsValid() || !CurrentObj->HasField(Seg))
			{
				return nullptr;
			}
			TSharedPtr<FJsonValue> Val = CurrentObj->TryGetField(Seg);
			if (!Val.IsValid())
			{
				return nullptr;
			}
			if (Idx == Segments.Num() - 1)
			{
				return Val;
			}
			// Must descend further — value at this level must be an object.
			if (Val->Type != EJson::Object)
			{
				return nullptr;
			}
			const TSharedPtr<FJsonObject>* NextObjPtr = nullptr;
			if (!Val->TryGetObject(NextObjPtr) || !NextObjPtr || !(*NextObjPtr).IsValid())
			{
				return nullptr;
			}
			CurrentObj = *NextObjPtr;
		}
		return nullptr;
	}

	/**
	 * Insert a JSON value at a dotted path into a destination object, creating
	 * intermediate objects as needed. Existing intermediates that are not objects
	 * are silently overwritten (we own Dest and started empty).
	 */
	static void InsertAtDottedPath(
		TSharedPtr<FJsonObject>& Dest,
		const TArray<FString>& Segments,
		const TSharedPtr<FJsonValue>& Leaf)
	{
		if (!Dest.IsValid() || Segments.Num() == 0 || !Leaf.IsValid())
		{
			return;
		}

		TSharedPtr<FJsonObject> CurrentObj = Dest;
		for (int32 Idx = 0; Idx < Segments.Num(); ++Idx)
		{
			const FString& Seg = Segments[Idx];
			if (Idx == Segments.Num() - 1)
			{
				CurrentObj->SetField(Seg, Leaf);
				return;
			}

			// Need an intermediate object at this segment.
			TSharedPtr<FJsonObject> NextObj;
			if (CurrentObj->HasField(Seg))
			{
				TSharedPtr<FJsonValue> Existing = CurrentObj->TryGetField(Seg);
				if (Existing.IsValid() && Existing->Type == EJson::Object)
				{
					const TSharedPtr<FJsonObject>* ExistingObjPtr = nullptr;
					if (Existing->TryGetObject(ExistingObjPtr) && ExistingObjPtr && (*ExistingObjPtr).IsValid())
					{
						NextObj = *ExistingObjPtr;
					}
				}
			}
			if (!NextObj.IsValid())
			{
				NextObj = MakeShared<FJsonObject>();
				CurrentObj->SetObjectField(Seg, NextObj);
			}
			CurrentObj = NextObj;
		}
	}

	/**
	 * Phase 1.1 — `_path_fields` dotted-path retention.
	 *
	 * For each "a.b.c" path string, walk Response; if the leaf exists, copy it
	 * into a fresh structure mirroring the path. Missing paths drop cleanly.
	 * Response is REPLACED with the built structure (so caller sees a minimal
	 * object containing only the matched leaves).
	 *
	 * Empty PathFieldsSet is a no-op with warning.
	 */
	static void ApplyPathFieldsFilter(
		TSharedPtr<FJsonObject>& Response,
		const TSet<FString>& PathFieldsSet,
		TArray<FString>& Warnings)
	{
		if (!Response.IsValid())
		{
			return;
		}
		if (PathFieldsSet.Num() == 0)
		{
			Warnings.Add(TEXT("`_path_fields:[]` is empty — no-op. Provide one or more dotted paths (e.g., \"uclass.class_name\")."));
			return;
		}

		TSharedPtr<FJsonObject> Built = MakeShared<FJsonObject>();
		bool bAnyPathResolved = false;
		for (const FString& Path : PathFieldsSet)
		{
			TArray<FString> Segments;
			Path.ParseIntoArray(Segments, TEXT("."), /*InCullEmpty=*/true);
			if (Segments.Num() == 0)
			{
				continue;
			}

			TSharedPtr<FJsonValue> Leaf = WalkDottedPath(Response, Segments);
			if (!Leaf.IsValid())
			{
				continue; // Missing path — clean drop, no warning.
			}
			bAnyPathResolved = true;
			InsertAtDottedPath(Built, Segments, Leaf);
		}

		// Total miss: not one requested dotted path resolved. List the
		// top-level keys so the caller can correct their path names.
		if (!bAnyPathResolved)
		{
			TArray<FString> TopLevelKeys;
			for (const auto& KVP : Response->Values) { TopLevelKeys.Add(MonolithKeyToString(KVP.Key)); }
			TopLevelKeys.Sort();
			Warnings.Add(FString::Printf(
				TEXT("_path_fields matched no paths; top-level keys available: [%s]"),
				*FString::Join(TopLevelKeys, TEXT(", "))));
		}

		// Swap Response contents with Built. Keep the same object handle.
		Response->Values.Empty();
		for (const auto& Pair : Built->Values)
		{
			Response->SetField(MonolithKeyToString(Pair.Key), Pair.Value);
		}
	}

	/** A value counts as "empty" for _compact_json if it is null, "", {}, or []. Numbers/bools/nonempty pass. */
	static bool IsEmptyForCompact(const TSharedPtr<FJsonValue>& Val)
	{
		if (!Val.IsValid() || Val->IsNull())
		{
			return true;
		}
		switch (Val->Type)
		{
		case EJson::String:
		{
			FString S;
			Val->TryGetString(S);
			return S.IsEmpty();
		}
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (Val->TryGetArray(Arr) && Arr)
			{
				return Arr->Num() == 0;
			}
			return true;
		}
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Val->TryGetObject(Obj) && Obj && (*Obj).IsValid())
			{
				return (*Obj)->Values.Num() == 0;
			}
			return true;
		}
		default:
			return false; // numbers / bools always retained
		}
	}
}

void ApplyResponseShaping(
	TSharedPtr<FJsonObject>& Response,
	const TSharedPtr<FJsonObject>& Params,
	TArray<FString>& Warnings)
{
	if (!Response.IsValid() || !Params.IsValid())
	{
		return;
	}

	TSet<FString> FieldsSet;
	TSet<FString> OmitSet;
	TSet<FString> RowFieldsSet;
	TSet<FString> PathFieldsSet;
	const bool bHasFields     = MonolithResponseShapingDetail::ReadStringArrayParam(Params, TEXT("_fields"),      FieldsSet);
	const bool bHasOmit       = MonolithResponseShapingDetail::ReadStringArrayParam(Params, TEXT("_omit"),        OmitSet);
	// _row_fields / _path_fields: presence check is "key exists" (so empty-array
	// can trigger the documented warning). Use HasField rather than the bool
	// returned by ReadStringArrayParam, which only signals non-empty success.
	const bool bHasRowFields  = Params->HasField(TEXT("_row_fields"));
	const bool bHasPathFields = Params->HasField(TEXT("_path_fields"));
	if (bHasRowFields)
	{
		MonolithResponseShapingDetail::ReadStringArrayParam(Params, TEXT("_row_fields"), RowFieldsSet);
	}
	if (bHasPathFields)
	{
		MonolithResponseShapingDetail::ReadStringArrayParam(Params, TEXT("_path_fields"), PathFieldsSet);
	}

	bool bCompact = false;
	if (!Params->TryGetBoolField(TEXT("_compact_json"), bCompact))
	{
		// String-fallback: Claude Code may serialize the bool as the string "true"/"false".
		FString CompactStr;
		if (Params->TryGetStringField(TEXT("_compact_json"), CompactStr) && !CompactStr.IsEmpty())
		{
			bCompact = CompactStr.Equals(TEXT("true"), ESearchCase::IgnoreCase) || FCString::ToBool(*CompactStr);
			if (bCompact)
			{
				UE_LOG(LogMonolith, Verbose, TEXT("ApplyResponseShaping: recovered stringified bool for '_compact_json' ('%s')"), *CompactStr);
			}
		}
	}

	// Mutually exclusive: _fields wins, _omit ignored, warn the caller.
	bool bApplyOmit = bHasOmit;
	if (bHasFields && bHasOmit)
	{
		Warnings.Add(TEXT("`_fields` and `_omit` are mutually exclusive; honoring `_fields`, ignoring `_omit`."));
		bApplyOmit = false;
	}

	// _fields whitelist — retain only matching top-level keys.
	if (bHasFields)
	{
		TArray<FString> Existing;
		for (const auto& KVP : Response->Values) { Existing.Add(MonolithKeyToString(KVP.Key)); }
		for (const FString& K : Existing)
		{
			if (!FieldsSet.Contains(K))
			{
				Response->RemoveField(K);
			}
		}
	}
	// _omit blacklist — drop matching top-level keys (only when _fields not active).
	else if (bApplyOmit)
	{
		for (const FString& K : OmitSet)
		{
			Response->RemoveField(K);
		}
	}

	// Phase 1.1 — _row_fields per-row whitelist on the unique list payload.
	// Runs AFTER _fields/_omit (so if _fields whitelisted away the list payload
	// the row filter is a clean no-op with a "no list payload" warning).
	if (bHasRowFields)
	{
		MonolithResponseShapingDetail::ApplyRowFieldsFilter(Response, RowFieldsSet, Warnings);
	}

	// Phase 1.1 — _path_fields dotted-path retention. REPLACES Response with a
	// freshly-built structure containing only matched leaves. Runs AFTER
	// _row_fields so row filtering applies before the path projection (which
	// can preserve a filtered list payload via a path that lands on its key).
	if (bHasPathFields)
	{
		MonolithResponseShapingDetail::ApplyPathFieldsFilter(Response, PathFieldsSet, Warnings);
	}

	// _compact_json — drop top-level keys whose value is null/""/{}/[]. Runs AFTER fields/omit.
	if (bCompact)
	{
		TArray<FString> Existing;
		for (const auto& KVP : Response->Values) { Existing.Add(MonolithKeyToString(KVP.Key)); }
		for (const FString& K : Existing)
		{
			const TSharedPtr<FJsonValue> Val = Response->TryGetField(K);
			if (MonolithResponseShapingDetail::IsEmptyForCompact(Val))
			{
				Response->RemoveField(K);
			}
		}
	}
}
