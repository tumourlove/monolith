#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
// UE 5.8 changed FJsonObject::Values keys from FString to UE::FSharedString
// (UE::TSharedString<TCHAR>); SharedString.h provides its definition.
#include "Containers/SharedString.h"
#endif

MONOLITHCORE_API DECLARE_LOG_CATEGORY_EXTERN(LogMonolith, Log, All);

/**
 * Cross-version JSON-object key conversion (5.7 + 5.8).
 *
 * UE 5.8 changed `FJsonObject::Values` from `TMap<FString, ...>` to
 * `TMap<UE::FSharedString, ...>` (`UE::TSharedString<TCHAR>`). That shared-string
 * type has no `.ToLower()`, no implicit `FString` conversion, and no `operator+`/`[]`,
 * so any `for (const auto& Pair : Obj->Values)` site that treated `Pair.Key` as an
 * `FString` stops compiling on 5.8.
 *
 * `MonolithKeyToString(Pair.Key)` yields an `FString` from either key type, keeping
 * every call site version-agnostic. The FString overload is a zero-copy passthrough
 * (valid on both engines); the shared-string overload exists only on 5.8 and copies
 * from the key's null-terminated buffer (`operator*`).
 */
FORCEINLINE const FString& MonolithKeyToString(const FString& Key) { return Key; }
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
FORCEINLINE FString MonolithKeyToString(const UE::FSharedString& Key) { return FString(*Key); }
#endif

/**
 * Reverse direction: build an `FJsonObject::Values` lookup key from a runtime FString,
 * for `Obj->Values.Find(...)` / `.FindChecked(...)` / `operator[]` by a string computed
 * at runtime. On 5.8 the map is keyed by `UE::FSharedString` and FString does not
 * implicitly convert (it would require two user-defined conversions); this makes the key
 * explicitly. On 5.7 it is a zero-copy passthrough.
 */
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
FORCEINLINE UE::FSharedString MonolithMakeJsonKey(const FString& Key) { return UE::FSharedString(*Key); }
#else
FORCEINLINE const FString& MonolithMakeJsonKey(const FString& Key) { return Key; }
#endif

class MONOLITHCORE_API FMonolithJsonUtils
{
public:
	/** Create a success JSON-RPC response wrapping a result object */
	static TSharedPtr<FJsonObject> SuccessResponse(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonValue>& Result);

	/** Create an error JSON-RPC response */
	static TSharedPtr<FJsonObject> ErrorResponse(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message, const TSharedPtr<FJsonValue>& Data = nullptr);

	/** Convenience: wrap a JSON object as a success result */
	static TSharedPtr<FJsonObject> SuccessObject(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& ResultObj);

	/** Convenience: wrap a simple string as a success result */
	static TSharedPtr<FJsonObject> SuccessString(const TSharedPtr<FJsonValue>& Id, const FString& Message);

	/** Serialize a FJsonObject to a compact JSON string */
	static FString Serialize(const TSharedPtr<FJsonObject>& JsonObject);

	/** Parse a JSON string into a FJsonObject. Returns nullptr on failure. */
	static TSharedPtr<FJsonObject> Parse(const FString& JsonString);

	/** Create a JSON array from a TArray of strings */
	static TSharedRef<FJsonValueArray> StringArrayToJson(const TArray<FString>& Strings);

	// --- JSON-RPC 2.0 Error Codes ---
	static constexpr int32 ErrParseError = -32700;
	static constexpr int32 ErrInvalidRequest = -32600;
	static constexpr int32 ErrMethodNotFound = -32601;
	static constexpr int32 ErrInvalidParams = -32602;
	static constexpr int32 ErrInternalError = -32603;

	// --- Monolith server-defined error codes (JSON-RPC -32000..-32099 range) ---
	//
	// R3b / §5.5 Error Contract — emitted when an action's underlying
	// optional sibling/marketplace plugin is absent. The action exists in
	// the registry; the call cannot be served because the underlying
	// type/asset/subsystem is missing. First consumer: the EffectSurface
	// action handlers when the optional provider is not loaded. Distinct from
	// `ErrInternalError` so telemetry / LLM error counters do not conflate
	// "feature gracefully unavailable" with "server choked".
	//
	// Reserved range -32011..-32019 left open for future "optional dep"
	// codes (e.g., `ErrFeatureGated` if we later distinguish "missing
	// plugin" from "feature flag off").
	static constexpr int32 ErrOptionalDepUnavailable = -32010;
};

/**
 * Survivor B — universal response-shaping post-filter.
 *
 * Reads the opt-in universal params from `Params` and mutates `Response`
 * in-place. Phase 1 of plan §3.B (2026-05-27-mcp-llm-ergonomics.md):
 *
 *  - `_fields:["a","b"]`    — keep only these TOP-LEVEL response keys
 *  - `_omit:["debug_info"]` — drop these TOP-LEVEL response keys
 *  - `_compact_json:true`   — drop top-level keys whose value is null/""/{}/[]
 *
 * Phase 1.1 — nested-payload shaping (item #3 of the RI ergonomics handover,
 * 2026-05-29-ri-ergonomics-improvements-handover.md):
 *
 *  - `_row_fields:["title","source_path"]`
 *      When the response has exactly ONE top-level array-of-objects key (the
 *      "list payload" — e.g., `decisions[]`, `uproperties[]`), filter each row
 *      to retain only the named keys. If multiple list payloads exist the
 *      filter is ambiguous and skipped (warning emitted — use `_path_fields`
 *      instead). Empty `_row_fields:[]` is a no-op (warning emitted).
 *
 *  - `_path_fields:["uclass.class_name","uclass.parent_class"]`
 *      Dotted-path retention. Walks each `a.b.c` path through Response; any
 *      missing segment causes a clean skip (no warning, just dropped). The
 *      Response is REPLACED with a freshly-built structure containing only
 *      the matched leaves. Use when `_row_fields` ambiguity warns or when
 *      the bulk lives behind a single named envelope (e.g., `uclass.*`).
 *
 * Order of operations within ApplyResponseShaping:
 *   1. _fields / _omit  (top-level whitelist / blacklist; mutually exclusive)
 *   2. _row_fields      (per-row whitelist on the unique list payload)
 *   3. _path_fields     (dotted-path retention — rebuilds Response)
 *   4. _compact_json    (drop top-level keys whose value is null/""/{}/[])
 *
 * If both `_fields` and `_omit` are non-empty, `_fields` wins and a warning
 * is appended. Empty `_fields` is a no-op (does NOT drop everything).
 *
 * `Warnings` is APPENDED to, not replaced; caller already owns the
 * K3 `warnings[]` channel and post-attaches to `ActionResult.Result`.
 */
MONOLITHCORE_API void ApplyResponseShaping(
	TSharedPtr<FJsonObject>& Response,
	const TSharedPtr<FJsonObject>& Params,
	TArray<FString>& Warnings);
