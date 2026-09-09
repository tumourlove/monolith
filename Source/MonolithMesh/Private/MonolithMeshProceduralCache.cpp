#include "MonolithMeshProceduralCache.h"
#include "Policies/CondensedJsonPrintPolicy.h"

#include "MonolithJsonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/PackageName.h"
#include "HAL/PlatformFileManager.h"

// ============================================================================
// Excluded keys — transient params that don't affect mesh geometry
// ============================================================================

const TSet<FString> FMonolithMeshProceduralCache::ExcludeKeys = {
	TEXT("handle"),
	TEXT("save_path"),
	TEXT("overwrite"),
	TEXT("place_in_scene"),
	TEXT("location"),
	TEXT("rotation"),
	TEXT("label"),
	TEXT("snap_to_floor"),
	TEXT("use_cache"),
	TEXT("auto_save")
};

// ============================================================================
// Singleton
// ============================================================================

FMonolithMeshProceduralCache& FMonolithMeshProceduralCache::Get()
{
	static FMonolithMeshProceduralCache Instance;
	return Instance;
}

// ============================================================================
// Directory / path helpers
// ============================================================================

FString FMonolithMeshProceduralCache::GetCacheDirectory()
{
	FString Dir = FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Saved") / TEXT("Monolith") / TEXT("ProceduralCache");
	IFileManager::Get().MakeDirectory(*Dir, true);
	return Dir;
}

FString FMonolithMeshProceduralCache::GetManifestPath()
{
	return GetCacheDirectory() / TEXT("manifest.json");
}

// ============================================================================
// Manifest I/O
// ============================================================================

void FMonolithMeshProceduralCache::EnsureLoaded()
{
	if (!bManifestLoaded)
	{
		LoadManifest();
	}
}

void FMonolithMeshProceduralCache::LoadManifest()
{
	bManifestLoaded = true;

	const FString Path = GetManifestPath();
	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *Path))
	{
		// No manifest on disk yet — start fresh
		Manifest = MakeShared<FJsonObject>();
		Manifest->SetNumberField(TEXT("version"), 1);
		Manifest->SetObjectField(TEXT("entries"), MakeShared<FJsonObject>());
		return;
	}

	TSharedPtr<FJsonObject> Parsed;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[MonolithCache] Failed to parse manifest, starting fresh: %s"), *Path);
		Manifest = MakeShared<FJsonObject>();
		Manifest->SetNumberField(TEXT("version"), 1);
		Manifest->SetObjectField(TEXT("entries"), MakeShared<FJsonObject>());
		return;
	}

	Manifest = Parsed;

	// Ensure entries object exists
	if (!Manifest->HasTypedField<EJson::Object>(TEXT("entries")))
	{
		Manifest->SetObjectField(TEXT("entries"), MakeShared<FJsonObject>());
	}
}

void FMonolithMeshProceduralCache::SaveManifest()
{
	if (!Manifest.IsValid())
	{
		return;
	}

	FString JsonStr;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonStr);
	if (!FJsonSerializer::Serialize(Manifest.ToSharedRef(), Writer))
	{
		UE_LOG(LogTemp, Error, TEXT("[MonolithCache] Failed to serialize manifest"));
		return;
	}

	const FString ManifestPath = GetManifestPath();
	const FString TempPath = ManifestPath + TEXT(".tmp");

	// Atomic write: write to temp file, then rename
	if (!FFileHelper::SaveStringToFile(JsonStr, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogTemp, Error, TEXT("[MonolithCache] Failed to write temp manifest: %s"), *TempPath);
		return;
	}

	// Move temp -> final (replace existing)
	if (!IFileManager::Get().Move(*ManifestPath, *TempPath, /*bReplace=*/true, /*bEvenIfReadOnly=*/true))
	{
		UE_LOG(LogTemp, Error, TEXT("[MonolithCache] Failed to rename temp manifest to: %s"), *ManifestPath);
		// Clean up temp file on failure
		IFileManager::Get().Delete(*TempPath);
	}
}

// ============================================================================
// Sorted canonical JSON serialization
// ============================================================================

FString FMonolithMeshProceduralCache::SerializeValue(const TSharedPtr<FJsonValue>& Val)
{
	if (!Val.IsValid() || Val->IsNull())
	{
		return TEXT("null");
	}

	switch (Val->Type)
	{
	case EJson::Object:
		return SortedJsonSerialize(Val->AsObject());

	case EJson::Array:
		return SortedArraySerialize(Val->AsArray());

	case EJson::Number:
		return FString::Printf(TEXT("%.2f"), Val->AsNumber());

	case EJson::String:
		{
			// Escape quotes in the string value
			FString Escaped = Val->AsString();
			Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
			Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
			return TEXT("\"") + Escaped + TEXT("\"");
		}

	case EJson::Boolean:
		return Val->AsBool() ? TEXT("true") : TEXT("false");

	default:
		return TEXT("null");
	}
}

FString FMonolithMeshProceduralCache::SortedJsonSerialize(const TSharedPtr<FJsonObject>& Obj)
{
	if (!Obj.IsValid())
	{
		return TEXT("{}");
	}

	// Extract keys and sort alphabetically
	TArray<FString> Keys;
	for (const auto& KVPair : Obj->Values) { Keys.Add(MonolithKeyToString(KVPair.Key)); }
	Keys.Sort();

	FString Result = TEXT("{");
	for (int32 i = 0; i < Keys.Num(); ++i)
	{
		if (i > 0)
		{
			Result += TEXT(",");
		}

		// Escape key
		FString EscapedKey = Keys[i];
		EscapedKey.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		EscapedKey.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Result += TEXT("\"") + EscapedKey + TEXT("\":");

		const TSharedPtr<FJsonValue>& Val = Obj->Values[MonolithMakeJsonKey(Keys[i])];
		Result += SerializeValue(Val);
	}
	Result += TEXT("}");
	return Result;
}

FString FMonolithMeshProceduralCache::SortedArraySerialize(const TArray<TSharedPtr<FJsonValue>>& Arr)
{
	FString Result = TEXT("[");
	for (int32 i = 0; i < Arr.Num(); ++i)
	{
		if (i > 0)
		{
			Result += TEXT(",");
		}
		Result += SerializeValue(Arr[i]);
	}
	Result += TEXT("]");
	return Result;
}

// ============================================================================
// Hash computation
// ============================================================================

FString FMonolithMeshProceduralCache::ComputeHash(const FString& ActionName, const TSharedPtr<FJsonObject>& Params)
{
	// Build canonical object: action name + identity-only params
	TSharedPtr<FJsonObject> Canonical = MakeShared<FJsonObject>();
	Canonical->SetStringField(TEXT("_action"), ActionName);

	if (Params.IsValid())
	{
		for (const auto& Pair : Params->Values)
		{
			if (!ExcludeKeys.Contains(MonolithKeyToString(Pair.Key)))
			{
				Canonical->SetField(Pair.Key, Pair.Value);
			}
		}
	}

	// Serialize to sorted canonical JSON
	const FString CanonicalStr = SortedJsonSerialize(Canonical);

	// MD5 hash
	FMD5 Md5;
	auto Utf8 = StringCast<UTF8CHAR>(*CanonicalStr);
	Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());

	uint8 Digest[16];
	Md5.Final(Digest);

	return BytesToHex(Digest, 16).ToLower();
}

// ============================================================================
// Cache lookup
// ============================================================================

bool FMonolithMeshProceduralCache::TryGetCached(const FString& Hash, FString& OutAssetPath)
{
	EnsureLoaded();

	const TSharedPtr<FJsonObject>* EntriesPtr = nullptr;
	if (!Manifest->TryGetObjectField(TEXT("entries"), EntriesPtr) || !EntriesPtr || !(*EntriesPtr).IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>& Entries = *EntriesPtr;
	const TSharedPtr<FJsonObject>* EntryPtr = nullptr;
	if (!Entries->TryGetObjectField(Hash, EntryPtr) || !EntryPtr || !(*EntryPtr).IsValid())
	{
		return false;
	}

	const FString AssetPath = (*EntryPtr)->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		// Malformed entry — remove it
		Entries->RemoveField(Hash);
		SaveManifest();
		return false;
	}

	// Verify asset still exists on disk
	if (!FPackageName::DoesPackageExist(AssetPath))
	{
		// Stale entry — asset was deleted. Remove and save.
		Entries->RemoveField(Hash);
		SaveManifest();
		UE_LOG(LogTemp, Log, TEXT("[MonolithCache] Removed stale entry %s -> %s"), *Hash.Left(8), *AssetPath);
		return false;
	}

	OutAssetPath = AssetPath;
	return true;
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshProceduralCache::Register(const FString& Hash, const FString& AssetPath,
	const FString& ActionName, const FString& Type, int32 TriangleCount,
	const TSharedPtr<FJsonObject>& Params)
{
	EnsureLoaded();

	TSharedPtr<FJsonObject> Entries = Manifest->GetObjectField(TEXT("entries"));
	if (!Entries.IsValid())
	{
		Entries = MakeShared<FJsonObject>();
		Manifest->SetObjectField(TEXT("entries"), Entries);
	}

	// Build entry
	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("asset_path"), AssetPath);
	Entry->SetStringField(TEXT("action"), ActionName);
	Entry->SetStringField(TEXT("type"), Type);
	Entry->SetNumberField(TEXT("triangle_count"), TriangleCount);
	Entry->SetStringField(TEXT("created_utc"), FDateTime::UtcNow().ToIso8601());

	// Store the canonical params JSON for reference
	if (Params.IsValid())
	{
		// Serialize the identity params (excluding transient keys) for the manifest
		TSharedPtr<FJsonObject> IdentityParams = MakeShared<FJsonObject>();
		for (const auto& Pair : Params->Values)
		{
			if (!ExcludeKeys.Contains(MonolithKeyToString(Pair.Key)))
			{
				IdentityParams->SetField(Pair.Key, Pair.Value);
			}
		}

		FString ParamsStr;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> ParamsWriter =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ParamsStr);
		FJsonSerializer::Serialize(IdentityParams.ToSharedRef(), ParamsWriter);
		Entry->SetStringField(TEXT("params_json"), ParamsStr);
	}

	Entries->SetObjectField(Hash, Entry);
	SaveManifest();

	UE_LOG(LogTemp, Log, TEXT("[MonolithCache] Registered %s -> %s (%s, %d tris)"),
		*Hash.Left(8), *AssetPath, *Type, TriangleCount);
}

// ============================================================================
// Auto-path generation
// ============================================================================

FString FMonolithMeshProceduralCache::GenerateAutoPath(const FString& Category, const FString& Type,
	float Width, float Depth, float Height, const FString& Hash, int32 Seed)
{
	const int32 W = FMath::RoundToInt(Width);
	const int32 D = FMath::RoundToInt(Depth);
	const int32 H = FMath::RoundToInt(Height);
	const FString Hash6 = Hash.Left(6);

	FString Name;
	if (Seed >= 0)
	{
		Name = FString::Printf(TEXT("SM_%s_%dx%dx%d_s%d_%s"), *Type, W, D, H, Seed, *Hash6);
	}
	else
	{
		Name = FString::Printf(TEXT("SM_%s_%dx%dx%d_%s"), *Type, W, D, H, *Hash6);
	}

	return FString::Printf(TEXT("/Game/Generated/%s/%s/%s"), *Category, *Type, *Name);
}

// ============================================================================
// Cache management
// ============================================================================

int32 FMonolithMeshProceduralCache::ValidateCache()
{
	EnsureLoaded();

	TSharedPtr<FJsonObject> Entries = Manifest->GetObjectField(TEXT("entries"));
	if (!Entries.IsValid())
	{
		return 0;
	}

	// Collect stale hashes first (can't modify while iterating)
	TArray<FString> StaleHashes;
	for (const auto& Pair : Entries->Values)
	{
		const TSharedPtr<FJsonObject>* EntryObj = nullptr;
		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
		{
			StaleHashes.Add(MonolithKeyToString(Pair.Key));
			continue;
		}

		const TSharedPtr<FJsonObject> Entry = Pair.Value->AsObject();
		const FString AssetPath = Entry->GetStringField(TEXT("asset_path"));
		if (AssetPath.IsEmpty() || !FPackageName::DoesPackageExist(AssetPath))
		{
			StaleHashes.Add(MonolithKeyToString(Pair.Key));
		}
	}

	// Remove stale entries
	for (const FString& Hash : StaleHashes)
	{
		Entries->RemoveField(Hash);
	}

	if (StaleHashes.Num() > 0)
	{
		SaveManifest();
		UE_LOG(LogTemp, Log, TEXT("[MonolithCache] Validated cache: removed %d stale entries"), StaleHashes.Num());
	}

	return StaleHashes.Num();
}

int32 FMonolithMeshProceduralCache::ClearCache(const FString& TypeFilter)
{
	EnsureLoaded();

	TSharedPtr<FJsonObject> Entries = Manifest->GetObjectField(TEXT("entries"));
	if (!Entries.IsValid())
	{
		return 0;
	}

	if (TypeFilter.IsEmpty())
	{
		// Clear everything
		const int32 Count = Entries->Values.Num();
		Manifest->SetObjectField(TEXT("entries"), MakeShared<FJsonObject>());
		SaveManifest();
		UE_LOG(LogTemp, Log, TEXT("[MonolithCache] Cleared all %d cache entries"), Count);
		return Count;
	}

	// Filter by type
	TArray<FString> ToRemove;
	for (const auto& Pair : Entries->Values)
	{
		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
		{
			continue;
		}

		const TSharedPtr<FJsonObject> Entry = Pair.Value->AsObject();
		if (Entry->GetStringField(TEXT("type")).Equals(TypeFilter, ESearchCase::IgnoreCase))
		{
			ToRemove.Add(MonolithKeyToString(Pair.Key));
		}
	}

	for (const FString& Hash : ToRemove)
	{
		Entries->RemoveField(Hash);
	}

	if (ToRemove.Num() > 0)
	{
		SaveManifest();
		UE_LOG(LogTemp, Log, TEXT("[MonolithCache] Cleared %d entries of type '%s'"), ToRemove.Num(), *TypeFilter);
	}

	return ToRemove.Num();
}

TSharedPtr<FJsonObject> FMonolithMeshProceduralCache::GetStats()
{
	EnsureLoaded();

	auto Result = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject> Entries = Manifest->GetObjectField(TEXT("entries"));
	if (!Entries.IsValid())
	{
		Result->SetNumberField(TEXT("total_entries"), 0);
		Result->SetObjectField(TEXT("by_type"), MakeShared<FJsonObject>());
		return Result;
	}

	// Count by type
	TMap<FString, int32> TypeCounts;
	for (const auto& Pair : Entries->Values)
	{
		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
		{
			continue;
		}

		const TSharedPtr<FJsonObject> Entry = Pair.Value->AsObject();
		const FString Type = Entry->GetStringField(TEXT("type"));
		TypeCounts.FindOrAdd(Type, 0)++;
	}

	Result->SetNumberField(TEXT("total_entries"), Entries->Values.Num());

	auto ByType = MakeShared<FJsonObject>();
	for (const auto& Pair : TypeCounts)
	{
		ByType->SetNumberField(Pair.Key, Pair.Value);
	}
	Result->SetObjectField(TEXT("by_type"), ByType);

	Result->SetStringField(TEXT("manifest_path"), GetManifestPath());

	return Result;
}

TSharedPtr<FJsonObject> FMonolithMeshProceduralCache::ListEntries(const FString& TypeFilter, int32 Limit)
{
	EnsureLoaded();

	auto Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> EntryArray;

	const TSharedPtr<FJsonObject> Entries = Manifest->GetObjectField(TEXT("entries"));
	if (!Entries.IsValid())
	{
		Result->SetArrayField(TEXT("entries"), EntryArray);
		Result->SetNumberField(TEXT("total"), 0);
		return Result;
	}

	int32 Count = 0;
	for (const auto& Pair : Entries->Values)
	{
		if (Count >= Limit)
		{
			break;
		}

		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
		{
			continue;
		}

		const TSharedPtr<FJsonObject> Entry = Pair.Value->AsObject();

		// Apply type filter if specified
		if (!TypeFilter.IsEmpty())
		{
			if (!Entry->GetStringField(TEXT("type")).Equals(TypeFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		// Clone the entry and add the hash as a field
		auto ListEntry = MakeShared<FJsonObject>();
		ListEntry->SetStringField(TEXT("hash"), Pair.Key);
		for (const auto& Field : Entry->Values)
		{
			ListEntry->SetField(Field.Key, Field.Value);
		}

		EntryArray.Add(MakeShared<FJsonValueObject>(ListEntry));
		Count++;
	}

	Result->SetArrayField(TEXT("entries"), EntryArray);
	Result->SetNumberField(TEXT("total"), Entries->Values.Num());
	if (!TypeFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("filter"), TypeFilter);
		Result->SetNumberField(TEXT("matched"), Count);
	}

	return Result;
}
