// Copyright tumourlove. All Rights Reserved.
// MonolithUIStyleService.cpp — Phase G

#include "Style/MonolithUIStyleService.h"

#if WITH_COMMONUI

#include "MonolithUISettings.h"
#include "MonolithUICommon.h"
#include "CommonUI/MonolithCommonUIHelpers.h"

#include "CommonButtonBase.h"   // UCommonButtonStyle
#include "CommonTextBlock.h"    // UCommonTextStyle
#include "CommonBorder.h"       // UCommonBorderStyle

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Crc.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
    // Singleton storage. Held by raw unique_ptr so module shutdown can drop
    // it cleanly before the DLL unloads. UStrongObjectPtr inside the entries
    // would log "leaked" warnings if we let the static destruct run after the
    // editor's UObject system has already torn down.
    TUniquePtr<FMonolithUIStyleService> GStyleServiceInstance;

    /**
     * Canonicalise a JSON value into a deterministic byte buffer for hashing.
     *
     * Rules (clean-room derived from "stable hash" requirements):
     *   - Object: sort fields by name (ASCII), recurse on each value.
     *   - Array: preserve order (semantically meaningful), recurse.
     *   - Number: render with `%.6f` to fix float precision drift between
     *             builds. Integers passed through TryGetNumber lose nothing
     *             below 2^53; %.6f is fine for UI-spec numerics (sizes,
     *             colours, padding).
     *   - String: UTF-8 bytes prefixed with quote markers `"..."`.
     *   - Bool:   `true` / `false` literal text.
     *   - Null:   `null` literal.
     *
     * The output is fed straight into FCrc::MemCrc32. Sorting the object
     * fields means {color,size} and {size,color} hash to the same value —
     * which is the entire point of dedup.
     */
    void AppendCanonical(FString& Out, const TSharedPtr<FJsonValue>& Value);

    void AppendCanonicalObject(FString& Out, const TSharedPtr<FJsonObject>& Object)
    {
        if (!Object.IsValid())
        {
            Out.Append(TEXT("{}"));
            return;
        }

        TArray<FString> Keys;
        Keys.Reserve(Object->Values.Num());
        for (const auto& Pair : Object->Values)
        {
            Keys.Add(Pair.Key);
        }
        Keys.Sort();

        Out.Append(TEXT("{"));
        for (int32 i = 0; i < Keys.Num(); ++i)
        {
            if (i > 0) Out.Append(TEXT(","));
            Out.Append(TEXT("\""));
            Out.Append(Keys[i]);
            Out.Append(TEXT("\":"));

            const TSharedPtr<FJsonValue>& Field = Object->Values.FindChecked(Keys[i]);
            AppendCanonical(Out, Field);
        }
        Out.Append(TEXT("}"));
    }

    void AppendCanonical(FString& Out, const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid() || Value->Type == EJson::Null)
        {
            Out.Append(TEXT("null"));
            return;
        }

        switch (Value->Type)
        {
            case EJson::Boolean:
                Out.Append(Value->AsBool() ? TEXT("true") : TEXT("false"));
                return;

            case EJson::Number:
                Out.Append(FString::Printf(TEXT("%.6f"), Value->AsNumber()));
                return;

            case EJson::String:
                Out.Append(TEXT("\""));
                Out.Append(Value->AsString());
                Out.Append(TEXT("\""));
                return;

            case EJson::Array:
            {
                const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
                Out.Append(TEXT("["));
                for (int32 i = 0; i < Arr.Num(); ++i)
                {
                    if (i > 0) Out.Append(TEXT(","));
                    AppendCanonical(Out, Arr[i]);
                }
                Out.Append(TEXT("]"));
                return;
            }

            case EJson::Object:
                AppendCanonicalObject(Out, Value->AsObject());
                return;

            default:
                Out.Append(TEXT("null"));
                return;
        }
    }
}

// -----------------------------------------------------------------------------
// Singleton lifecycle
// -----------------------------------------------------------------------------

FMonolithUIStyleService& FMonolithUIStyleService::Get()
{
    if (!GStyleServiceInstance.IsValid())
    {
        GStyleServiceInstance = TUniquePtr<FMonolithUIStyleService>(new FMonolithUIStyleService());
    }
    return *GStyleServiceInstance;
}

void FMonolithUIStyleService::Shutdown()
{
    GStyleServiceInstance.Reset();
}

void FMonolithUIStyleService::Reset()
{
    FScopeLock Lock(&CacheLock);
    NameIndex.Reset();
    HashIndex.Reset();
    Entries.Reset();
    NextStamp = 1;
    HitCount = 0;
    MissCount = 0;
    EvictionCount = 0;
}

// -----------------------------------------------------------------------------
// Hash + type helpers
// -----------------------------------------------------------------------------

uint32 FMonolithUIStyleService::ComputeContentHash(
    UClass* StyleClass,
    const TSharedPtr<FJsonObject>& Properties)
{
    // Two-arg overload retained for callers that don't carry an asset_name
    // (e.g. unit tests verifying property-bag equivalence). New production
    // call sites should prefer the three-arg overload below.
    return ComputeContentHash(StyleClass, FString(), Properties);
}

uint32 FMonolithUIStyleService::ComputeContentHash(
    UClass* StyleClass,
    const FString& AssetName,
    const TSharedPtr<FJsonObject>& Properties)
{
    // Mix the class name + asset_name into the hash so a Button-style with the
    // same fields as a Text-style still gets a distinct hash, AND so two
    // requests under the same class with an empty property bag but different
    // asset names hash to different buckets. The latter clause closes Bug #1
    // from the 2026-05-16 UI gap audit: previously,
    //   create_common_text_style("CTS_A", {}) followed by
    //   create_common_text_style("CTS_B", {})
    // collided on the same content-hash (Step 3 of ResolveOrCreate) and the
    // second call returned the first asset's resolution. Including AssetName
    // in the canonical buffer means each name produces a distinct hash.
    FString Buffer;
    Buffer.Reserve(256);
    Buffer.Append(StyleClass ? StyleClass->GetName() : TEXT("<null>"));
    Buffer.Append(TEXT("|"));
    Buffer.Append(AssetName);
    Buffer.Append(TEXT("|"));
    AppendCanonicalObject(Buffer, Properties);

    // StringCast<UTF8CHAR> gives us a deterministic byte view to hash. The
    // older FTCHARToUTF8 alias is marked deprecated in UE 5.x in favour of
    // this form (see Containers/StringConv.h:1016). Length() returns the
    // converted character count, multiplied by sizeof(UTF8CHAR) for the byte
    // span MemCrc32 wants.
    const auto Utf8 = StringCast<UTF8CHAR>(*Buffer);
    return FCrc::MemCrc32(Utf8.Get(), Utf8.Length() * sizeof(UTF8CHAR), 0);
}

FName FMonolithUIStyleService::StyleTypeToken(UClass* StyleClass)
{
    if (!StyleClass) return NAME_None;
    if (StyleClass->IsChildOf(UCommonButtonStyle::StaticClass())) return FName(TEXT("Button"));
    if (StyleClass->IsChildOf(UCommonTextStyle::StaticClass()))   return FName(TEXT("Text"));
    if (StyleClass->IsChildOf(UCommonBorderStyle::StaticClass())) return FName(TEXT("Border"));
    return FName(*StyleClass->GetName());
}

FString FMonolithUIStyleService::DeriveNameFromHash(UClass* StyleClass, uint32 Hash)
{
    // Hex-suffixed prefix so generated names stay scannable in the content
    // browser. The "Auto_" prefix flags the asset as service-generated for
    // anyone hand-auditing the styles folder.
    const FName Token = StyleTypeToken(StyleClass);
    return FString::Printf(TEXT("Auto_%s_%08X"), *Token.ToString(), Hash);
}

// -----------------------------------------------------------------------------
// Cache discipline
// -----------------------------------------------------------------------------

void FMonolithUIStyleService::InsertEntry(FUIStyleEntry&& Entry)
{
    // Caller holds CacheLock.
    const FString Name = Entry.AssetName;
    const uint32 Hash = Entry.ContentHash;

    Entry.LastAccessStamp = NextStamp++;

    const int32 Idx = Entries.Add(MoveTemp(Entry));
    NameIndex.Add(Name, Idx);
    HashIndex.Add(Hash, Idx);

    const UMonolithUISettings* Settings = UMonolithUISettings::Get();
    const int32 Cap = Settings ? Settings->StyleCacheCap : 200;
    if (Cap > 0)
    {
        while (Entries.Num() > Cap)
        {
            EvictOldest();
        }
    }
}

void FMonolithUIStyleService::EvictOldest()
{
    // Caller holds CacheLock. Linear scan — Entries.Num() is bounded by the
    // configured cap (200 default), so a full pass costs a couple of microseconds.
    if (Entries.Num() == 0) return;

    int32 OldestIdx = 0;
    int64 OldestStamp = Entries[0].LastAccessStamp;
    for (int32 i = 1; i < Entries.Num(); ++i)
    {
        if (Entries[i].LastAccessStamp < OldestStamp)
        {
            OldestStamp = Entries[i].LastAccessStamp;
            OldestIdx = i;
        }
    }

    const FUIStyleEntry& Doomed = Entries[OldestIdx];
    NameIndex.Remove(Doomed.AssetName);
    HashIndex.Remove(Doomed.ContentHash);

    // RemoveAtSwap rewrites whichever entry was at the end into OldestIdx —
    // re-point its index entries to the new slot so subsequent lookups don't
    // chase a stale offset.
    const int32 LastIdx = Entries.Num() - 1;
    if (OldestIdx != LastIdx)
    {
        const FUIStyleEntry& Moved = Entries[LastIdx];
        NameIndex[Moved.AssetName] = OldestIdx;
        HashIndex[Moved.ContentHash] = OldestIdx;
    }
    Entries.RemoveAtSwap(OldestIdx, EAllowShrinking::No);
    ++EvictionCount;
}

// -----------------------------------------------------------------------------
// Canonical library lookup
// -----------------------------------------------------------------------------

UClass* FMonolithUIStyleService::TryFindInCanonicalLibrary(
    UClass* StyleClass,
    const FString& AssetName) const
{
    if (!StyleClass || AssetName.IsEmpty())
    {
        return nullptr;
    }

    const UMonolithUISettings* Settings = UMonolithUISettings::Get();
    if (!Settings) return nullptr;

    const FString Folder = UMonolithUISettings::NormalizeFolderPath(Settings->CanonicalLibraryPath);
    if (Folder.IsEmpty())
    {
        return nullptr;
    }

    // Library lookup works on Blueprint assets. Construct the candidate path
    // and try LoadObject<UBlueprint>; the BP's GeneratedClass is what we want.
    const FString CandidatePath = FString::Printf(TEXT("%s/%s.%s"), *Folder, *AssetName, *AssetName);
    UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *CandidatePath);
    if (!BP || !BP->GeneratedClass)
    {
        return nullptr;
    }

    if (!BP->GeneratedClass->IsChildOf(StyleClass))
    {
        // Same name, wrong style category. Refuse — caller meant a different asset.
        return nullptr;
    }

    return BP->GeneratedClass;
}

// -----------------------------------------------------------------------------
// Asset creation
// -----------------------------------------------------------------------------

UClass* FMonolithUIStyleService::CreateNewStyleAsset(
    UClass* StyleClass,
    const FString& PackagePath,
    const FString& AssetName,
    const TSharedPtr<FJsonObject>& Properties,
    FString& OutError)
{
    if (!StyleClass)
    {
        OutError = TEXT("CreateNewStyleAsset: StyleClass is null");
        return nullptr;
    }

    const FString Folder = UMonolithUISettings::NormalizeFolderPath(PackagePath);
    if (Folder.IsEmpty())
    {
        OutError = TEXT("CreateNewStyleAsset: PackagePath is empty");
        return nullptr;
    }

    // Use AssetTools to dedup the on-disk name. If two callers race on the
    // same hash → same derived name, the second one gets a `_1` suffix
    // rather than colliding. This is the secondary defence behind the
    // hash-based dedup at the cache layer (per plan G3 review note).
    FAssetToolsModule& AssetToolsModule =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

    FString FinalPackageName;
    FString FinalAssetName;
    AssetToolsModule.Get().CreateUniqueAssetName(
        FString::Printf(TEXT("%s/%s"), *Folder, *AssetName),
        FString(),
        FinalPackageName,
        FinalAssetName);

    UPackage* Package = CreatePackage(*FinalPackageName);
    if (!Package)
    {
        OutError = FString::Printf(TEXT("CreateNewStyleAsset: CreatePackage failed for '%s'"), *FinalPackageName);
        return nullptr;
    }
    Package->FullyLoad();

    // Create a Blueprint whose parent is the native style class. The 5-arg
    // overload auto-resolves BlueprintClass / BlueprintGeneratedClass — same
    // pattern the existing CreateStyleAsset helper uses.
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        StyleClass, Package, FName(*FinalAssetName),
        BPTYPE_Normal, FName(TEXT("MonolithUI")));

    if (!BP || !BP->GeneratedClass)
    {
        OutError = FString::Printf(
            TEXT("CreateNewStyleAsset: CreateBlueprint failed for '%s' (parent: %s)"),
            *FinalAssetName, *StyleClass->GetName());
        return nullptr;
    }

    // Apply property overrides to the CDO via the existing JSON-set helper.
    UObject* CDO = BP->GeneratedClass->GetDefaultObject();
    if (CDO && Properties.IsValid())
    {
        for (const auto& Pair : Properties->Values)
        {
            FProperty* P = FindFProperty<FProperty>(StyleClass, *Pair.Key);
            if (P)
            {
                MonolithCommonUI::SetPropertyFromJson(CDO, P, Pair.Value);
            }
        }
    }

    FKismetEditorUtilities::CompileBlueprint(BP);

    FAssetRegistryModule::AssetCreated(BP);
    Package->MarkPackageDirty();

    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    UPackage::SavePackage(
        Package, BP,
        *FPackageName::LongPackageNameToFilename(FinalPackageName, FPackageName::GetAssetPackageExtension()),
        SaveArgs);

    return BP->GeneratedClass;
}

// -----------------------------------------------------------------------------
// Public entry point
// -----------------------------------------------------------------------------

FUIStyleResolution FMonolithUIStyleService::ResolveOrCreate(
    UClass* StyleClass,
    const FString& AssetName,
    const FString& PackagePath,
    const TSharedPtr<FJsonObject>& Properties)
{
    FUIStyleResolution Result;

    if (!StyleClass)
    {
        Result.Error = TEXT("StyleClass is null");
        return Result;
    }
    if (!StyleClass->IsChildOf(UCommonButtonStyle::StaticClass())
        && !StyleClass->IsChildOf(UCommonTextStyle::StaticClass())
        && !StyleClass->IsChildOf(UCommonBorderStyle::StaticClass()))
    {
        Result.Error = FString::Printf(
            TEXT("StyleClass '%s' is not a UCommonButton/Text/BorderStyle"),
            *StyleClass->GetName());
        return Result;
    }

    // Derive the asset name first using the property-bag-only hash (no name
    // mix-in), so a caller who omits AssetName still gets a deterministic
    // "Auto_<type>_<hash>" suffix derived from properties alone. Then compute
    // the FINAL content-hash that includes the EffectiveName — Bug #1 fix:
    // without the name mix-in, two empty-property-bag requests under different
    // asset_names collided in the Step-3 hash cache and returned the wrong
    // asset on the second call.
    const uint32 PropOnlyHash = ComputeContentHash(StyleClass, Properties);
    const FString EffectiveName = AssetName.IsEmpty()
        ? DeriveNameFromHash(StyleClass, PropOnlyHash)
        : AssetName;
    const uint32 ContentHash = ComputeContentHash(StyleClass, EffectiveName, Properties);

    // ---- Step 1: cache by name -------------------------------------------
    {
        FScopeLock Lock(&CacheLock);
        if (const int32* IdxPtr = NameIndex.Find(EffectiveName))
        {
            FUIStyleEntry& Entry = Entries[*IdxPtr];
            // Dedup is keyed on (name + content-hash). If the same name comes
            // back with a different content bag, fall through to creation
            // (which will dedup-name via AssetTools). Honouring this avoids
            // returning the wrong style when a caller reuses a name.
            if (Entry.ContentHash == ContentHash
                && Entry.StyleClass.IsValid()
                && Entry.StyleClass->IsChildOf(StyleClass))
            {
                Entry.LastAccessStamp = NextStamp++;
                ++HitCount;

                Result.StyleClass = Entry.StyleClass.Get();
                Result.AssetName = Entry.AssetName;
                Result.PackagePath = Entry.PackagePath;
                Result.ResolvedVia = TEXT("name_cache");
                Result.bWasCreated = false;
                return Result;
            }
        }
    }

    // ---- Step 2: canonical library ---------------------------------------
    if (UClass* LibClass = TryFindInCanonicalLibrary(StyleClass, EffectiveName))
    {
        FUIStyleEntry Entry;
        Entry.AssetName = EffectiveName;
        Entry.PackagePath = UMonolithUISettings::NormalizeFolderPath(
            UMonolithUISettings::Get()->CanonicalLibraryPath);
        Entry.StyleClass = TStrongObjectPtr<UClass>(LibClass);
        Entry.ContentHash = ContentHash;
        Entry.StyleType = StyleTypeToken(StyleClass);

        FScopeLock Lock(&CacheLock);
        ++MissCount;  // Library hit still counts as cache miss for telemetry.
        InsertEntry(MoveTemp(Entry));

        Result.StyleClass = LibClass;
        Result.AssetName = EffectiveName;
        Result.PackagePath = UMonolithUISettings::NormalizeFolderPath(
            UMonolithUISettings::Get()->CanonicalLibraryPath);
        Result.ResolvedVia = TEXT("library");
        Result.bWasCreated = false;
        return Result;
    }

    // ---- Step 3: cache by hash -------------------------------------------
    {
        FScopeLock Lock(&CacheLock);
        if (const int32* IdxPtr = HashIndex.Find(ContentHash))
        {
            FUIStyleEntry& Entry = Entries[*IdxPtr];
            if (Entry.StyleClass.IsValid()
                && Entry.StyleClass->IsChildOf(StyleClass))
            {
                Entry.LastAccessStamp = NextStamp++;
                ++HitCount;

                Result.StyleClass = Entry.StyleClass.Get();
                Result.AssetName = Entry.AssetName;
                Result.PackagePath = Entry.PackagePath;
                Result.ResolvedVia = TEXT("hash_cache");
                Result.bWasCreated = false;
                return Result;
            }
        }
    }

    // ---- Fall through: create --------------------------------------------
    const UMonolithUISettings* Settings = UMonolithUISettings::Get();
    const FString TargetFolder = PackagePath.IsEmpty()
        ? (Settings ? UMonolithUISettings::NormalizeFolderPath(Settings->GeneratedStylesPath) : FString(TEXT("/Game/UI/Styles")))
        : UMonolithUISettings::NormalizeFolderPath(PackagePath);

    FString CreateError;
    UClass* NewClass = CreateNewStyleAsset(StyleClass, TargetFolder, EffectiveName, Properties, CreateError);
    if (!NewClass)
    {
        Result.Error = CreateError;
        return Result;
    }

    FUIStyleEntry Entry;
    Entry.AssetName = EffectiveName;
    Entry.PackagePath = TargetFolder;
    Entry.StyleClass = TStrongObjectPtr<UClass>(NewClass);
    Entry.ContentHash = ContentHash;
    Entry.StyleType = StyleTypeToken(StyleClass);

    {
        FScopeLock Lock(&CacheLock);
        ++MissCount;
        InsertEntry(MoveTemp(Entry));
    }

    Result.StyleClass = NewClass;
    Result.AssetName = EffectiveName;
    Result.PackagePath = TargetFolder;
    Result.ResolvedVia = TEXT("created");
    Result.bWasCreated = true;
    return Result;
}

// -----------------------------------------------------------------------------
// Stats
// -----------------------------------------------------------------------------

FUIStyleCacheStats FMonolithUIStyleService::GetStats() const
{
    FScopeLock Lock(&CacheLock);

    FUIStyleCacheStats Stats;
    Stats.CacheSize = Entries.Num();
    Stats.Hits = HitCount;
    Stats.Misses = MissCount;
    Stats.Evictions = EvictionCount;

    static const FName ButtonName(TEXT("Button"));
    static const FName TextName(TEXT("Text"));
    static const FName BorderName(TEXT("Border"));
    for (const FUIStyleEntry& E : Entries)
    {
        if (E.StyleType == ButtonName)      ++Stats.ButtonCount;
        else if (E.StyleType == TextName)   ++Stats.TextCount;
        else if (E.StyleType == BorderName) ++Stats.BorderCount;
    }

    return Stats;
}

#endif // WITH_COMMONUI
