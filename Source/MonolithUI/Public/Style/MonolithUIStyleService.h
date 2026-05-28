// Copyright tumourlove. All Rights Reserved.
// MonolithUIStyleService.h — Phase G
//
// Two-key (name + content-hash) cache for CommonUI style assets. Resolves a
// requested style spec via a three-step chain:
//
//   1. Cache-by-name    — instant hit on the FString asset name we last wrote.
//   2. Canonical-library — scan `UMonolithUISettings::CanonicalLibraryPath`
//                          for a same-typed asset with matching name.
//   3. Cache-by-hash    — instant hit on the canonicalised property buffer
//                          hash we computed last time the same fields were
//                          requested under a different name.
//
// Anything that misses all three steps falls through to asset creation
// (`CreateNewStyleAsset`), which produces a Blueprint whose parent is the
// requested style class (UCommonButtonStyle / UCommonTextStyle /
// UCommonBorderStyle), applies the property bag to the CDO, and saves to
// `UMonolithUISettings::GeneratedStylesPath`.
//
// Why a static singleton instead of a UEditorSubsystem member:
//   Phase E (parallel to this work) is editing
//   `MonolithUIRegistrySubsystem.cpp` for curated-mapping additions. Owning
//   the StyleService as a subsystem field would require touching the same
//   .cpp file. Static singleton keeps Phase G self-contained — the subsystem
//   header is unaffected, both phases can land independently, and the
//   ownership choice is documented (see `Get()` rationale).
//
// Clean-room: identifiers, struct shape, and pipeline order are OURS.
// Hash function is FCrc::MemCrc32 over a deterministic byte buffer
// (canonical-form-then-CRC32). No reference plugin code consulted.
//
// CommonUI gating: this entire module compiles to an empty TU when
// `WITH_COMMONUI=0`. The action handlers that consume the service are
// already gated; the service itself is gated to keep the symbol surface
// honest (no exported accessor returning a UCommonButtonStyle when the type
// doesn't exist).

#pragma once

#if WITH_COMMONUI

#include "CoreMinimal.h"
#include "Templates/SubclassOf.h"
#include "UObject/StrongObjectPtr.h"
#include "Dom/JsonObject.h"

class UCommonButtonStyle;
class UCommonTextStyle;
class UCommonBorderStyle;
class UObject;

/**
 * Cached style entry. The class pointer is stored as `TStrongObjectPtr<UClass>`
 * so the Blueprint-generated style class isn't GC'd between cache hits — the
 * whole point of a cache is that the resolved class survives until evicted.
 */
struct MONOLITHUI_API FUIStyleEntry
{
    /** Asset name as the caller requested it (e.g. "BS_Primary"). */
    FString AssetName;

    /** Folder the class was created/found in (normalised, no trailing slash). */
    FString PackagePath;

    /** Resolved class — the BP-generated `_C` class that widgets consume. */
    TStrongObjectPtr<UClass> StyleClass;

    /** CRC32 of the canonical property buffer that produced this entry. */
    uint32 ContentHash = 0;

    /** Engine type token: "Button", "Text", or "Border". */
    FName StyleType;

    /** Monotonic stamp for LRU ordering. Bumped on every cache hit. */
    int64 LastAccessStamp = 0;
};

/**
 * Result of a `ResolveOrCreate` call. Carries the resolved class plus enough
 * metadata for the diagnostic dump action to report what happened.
 */
struct MONOLITHUI_API FUIStyleResolution
{
    /** Resolved class — null only on hard failure (invalid input). */
    UClass* StyleClass = nullptr;

    /** Asset name as it ended up on disk (may differ if dedup picked a name). */
    FString AssetName;

    /** Folder the class lives in (normalised). */
    FString PackagePath;

    /** "name_cache" | "library" | "hash_cache" | "created". */
    FString ResolvedVia;

    /** True if the call newly created the asset (vs. resolved from cache or library). */
    bool bWasCreated = false;

    /** Empty on success; populated with a diagnostic when the call fails. */
    FString Error;

    bool IsValid() const { return StyleClass != nullptr && Error.IsEmpty(); }
};

/**
 * Cache statistics — surfaced by `ui::dump_style_cache_stats`.
 */
struct MONOLITHUI_API FUIStyleCacheStats
{
    int32 CacheSize = 0;
    int64 Hits = 0;
    int64 Misses = 0;
    int64 Evictions = 0;
    int32 ButtonCount = 0;
    int32 TextCount = 0;
    int32 BorderCount = 0;
};

/**
 * Singleton service. Not a UCLASS — owning the cache as plain C++ avoids the
 * UClass/CDO churn of editor subsystems and keeps the lifetime explicitly
 * tied to the module (see `MonolithUIModule.cpp` for shutdown).
 */
class MONOLITHUI_API FMonolithUIStyleService
{
public:
    /**
     * Process-wide accessor. The instance is constructed on first use and
     * lives until module shutdown calls `Shutdown()`. Editor subsystems
     * initialise late; constructing here on demand is the simplest way to
     * have the service available from any action handler.
     */
    static FMonolithUIStyleService& Get();

    /**
     * Tear down the singleton. Called from `FMonolithUIModule::ShutdownModule`
     * so cached UClass references release cleanly before the module DLL
     * unloads (otherwise the strong-object pointers would log a "leaked"
     * warning at editor exit).
     */
    static void Shutdown();

    /**
     * Resolve a style for the given (StyleClass, AssetName, Properties) tuple.
     * Walks the three-step resolution chain; falls through to creation on
     * total miss. `StyleClass` MUST be one of UCommonButtonStyle /
     * UCommonTextStyle / UCommonBorderStyle (or a subclass).
     *
     * @param StyleClass     Engine type — drives the expected style category.
     * @param AssetName      Name to use if creating; also used for cache key.
     *                       If empty, the service derives a stable name from
     *                       the content hash.
     * @param PackagePath    Folder for new assets. Empty = use settings default.
     * @param Properties     JSON object of {fieldName: value} applied to the
     *                       new style's CDO. Determines the content hash.
     * @return Resolution with class + metadata + how-resolved tag.
     */
    FUIStyleResolution ResolveOrCreate(
        UClass* StyleClass,
        const FString& AssetName,
        const FString& PackagePath,
        const TSharedPtr<FJsonObject>& Properties);

    /** Reset the cache + counters. Used by tests for isolation. */
    void Reset();

    /** Snapshot of cache stats for the diagnostic action. */
    FUIStyleCacheStats GetStats() const;

    /**
     * Compute the canonical content hash for a properties bag against a
     * specific style class. Exposed for tests so they can verify two-bag
     * equivalence without going through the resolver. Two-arg form retained
     * for backward compatibility — calls the three-arg overload with an empty
     * AssetName (NOT recommended for cache-lookup paths post Bug #1 fix).
     */
    static uint32 ComputeContentHash(
        UClass* StyleClass,
        const TSharedPtr<FJsonObject>& Properties);

    /**
     * Three-arg overload: mixes the asset_name into the canonical buffer so
     * two empty-property-bag requests under different names produce distinct
     * hashes. Bug #1 fix (2026-05-16 UI gap audit) — see implementation for
     * the failure-mode write-up.
     */
    static uint32 ComputeContentHash(
        UClass* StyleClass,
        const FString& AssetName,
        const TSharedPtr<FJsonObject>& Properties);

    /**
     * Public dtor — required because TUniquePtr<FMonolithUIStyleService>'s
     * default deleter calls `delete` from a non-friend context. The singleton
     * is still externally non-constructible (private ctor); only Get() forms
     * the instance via TUniquePtr internals.
     */
    ~FMonolithUIStyleService() = default;

private:
    FMonolithUIStyleService() = default;
    FMonolithUIStyleService(const FMonolithUIStyleService&) = delete;
    FMonolithUIStyleService& operator=(const FMonolithUIStyleService&) = delete;

    /** Internal: name → entry index in `Entries`. */
    TMap<FString, int32> NameIndex;

    /** Internal: hash → entry index in `Entries`. */
    TMap<uint32, int32> HashIndex;

    /** Backing store; entries deleted via swap-with-last to keep indices stable on insert. */
    TArray<FUIStyleEntry> Entries;

    /** Monotonic stamp generator for LRU ordering. */
    int64 NextStamp = 1;

    /** Telemetry. */
    int64 HitCount = 0;
    int64 MissCount = 0;
    int64 EvictionCount = 0;

    /** Thread-safety (editor actions can fire from PIE callbacks under unusual conditions). */
    mutable FCriticalSection CacheLock;

    /** Insert or update an entry; evicts LRU if past the configured cap. */
    void InsertEntry(FUIStyleEntry&& Entry);

    /** Drop the least-recently-used entry. Caller holds CacheLock. */
    void EvictOldest();

    /** Look up the canonical library folder for a same-named, same-typed asset. */
    UClass* TryFindInCanonicalLibrary(
        UClass* StyleClass,
        const FString& AssetName) const;

    /** Materialise a new style asset on disk. */
    UClass* CreateNewStyleAsset(
        UClass* StyleClass,
        const FString& PackagePath,
        const FString& AssetName,
        const TSharedPtr<FJsonObject>& Properties,
        FString& OutError);

    /** "Button" | "Text" | "Border" derived from the engine class. */
    static FName StyleTypeToken(UClass* StyleClass);

    /** Derive a stable name from a hash when the caller didn't supply one. */
    static FString DeriveNameFromHash(UClass* StyleClass, uint32 Hash);
};

#endif // WITH_COMMONUI
