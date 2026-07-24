// Copyright tumourlove. All Rights Reserved.
#include "Actions/Hoisted/FontIngestActions.h"

// Monolith registry
#include "MonolithToolRegistry.h"

// Core / JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/UnrealMemory.h"
#include "Misc/FileHelper.h"                    // FFileHelper::LoadFileToArray
#include "Misc/PackageName.h"                   // FPackageName::LongPackageNameToFilename / GetAssetPackageExtension
#include "Misc/Paths.h"                         // FPaths::FileExists
#include "UObject/Package.h"                    // UPackage, SavePackage
#include "UObject/SavePackage.h"                // FSavePackageArgs
#include "UObject/UObjectGlobals.h"             // CreatePackage, NewObject

// Font core
#include "Engine/Font.h"                        // UFont, EFontCacheType, ERuntimeFontSource
#include "Engine/FontFace.h"                    // UFontFace
#include "Fonts/CompositeFont.h"                // FFontFaceData, FFontData, FTypefaceEntry, FCompositeFont, EFontHinting, EFontLoadingPolicy

// Asset registry + asset tools (unique naming)
#include "AssetRegistry/AssetRegistryModule.h"  // FAssetRegistryModule::AssetCreated
#include "AssetToolsModule.h"                   // FAssetToolsModule
#include "IAssetTools.h"                        // IAssetTools::CreateUniqueAssetName
#include "Modules/ModuleManager.h"              // FModuleManager::LoadModuleChecked

namespace MonolithUI::FontIngestInternal
{
    /**
     * Map a loading-policy string to the enum. Unrecognised strings fall back to
     * LazyLoad (the safest runtime default). Returns true iff a recognised value
     * was supplied -- caller can warn on unknown input.
     */
    static bool ParseLoadingPolicy(const FString& S, EFontLoadingPolicy& Out)
    {
        if (S == TEXT("LazyLoad")) { Out = EFontLoadingPolicy::LazyLoad; return true; }
        if (S == TEXT("Stream"))   { Out = EFontLoadingPolicy::Stream;   return true; }
        if (S == TEXT("Inline"))   { Out = EFontLoadingPolicy::Inline;   return true; }
        Out = EFontLoadingPolicy::LazyLoad;
        return false;
    }

    /**
     * Map a hinting string to the enum. Unrecognised strings fall back to Default.
     * Returns true iff a recognised value was supplied.
     */
    static bool ParseHinting(const FString& S, EFontHinting& Out)
    {
        if (S == TEXT("Default"))     { Out = EFontHinting::Default;     return true; }
        if (S == TEXT("Auto"))        { Out = EFontHinting::Auto;        return true; }
        if (S == TEXT("AutoLight"))   { Out = EFontHinting::AutoLight;   return true; }
        if (S == TEXT("Monochrome"))  { Out = EFontHinting::Monochrome;  return true; }
        if (S == TEXT("None"))        { Out = EFontHinting::None;        return true; }
        Out = EFontHinting::Default;
        return false;
    }

    /** Parsed face spec. */
    struct FFaceSpec
    {
        FString Typeface;   // "Regular", "Bold", ...
        FString SourcePath; // Absolute path to TTF on disk
    };

    /** Per-face import outputs, including saved asset path for the result payload. */
    struct FFaceResult
    {
        UFontFace* Face = nullptr;
        FString    AssetPath;   // Long package name, e.g. /Game/UI/Fonts/Example/F_Regular
        FString    Typeface;    // Passed through for typeface-entry construction
    };
} // namespace MonolithUI::FontIngestInternal

FMonolithActionResult MonolithUI::FFontIngestActions::HandleImportFontFamily(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUI::FontIngestInternal;

    if (!Params.IsValid())
    {
        return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
    }

    // --- Required params ---
    FString Destination;
    if (!Params->TryGetStringField(TEXT("destination"), Destination) || Destination.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Missing or empty required param: destination"), -32602);
    }
    if (!Destination.StartsWith(TEXT("/")))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("destination must be a /Game/... directory path (got '%s')"), *Destination),
            -32602);
    }
    if (Destination.EndsWith(TEXT("/")))
    {
        Destination = Destination.LeftChop(1);
    }

    FString FamilyName;
    if (!Params->TryGetStringField(TEXT("family_name"), FamilyName) || FamilyName.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Missing or empty required param: family_name"), -32602);
    }

    const TArray<TSharedPtr<FJsonValue>>* FacesArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("faces"), FacesArr) || !FacesArr || FacesArr->Num() == 0)
    {
        return FMonolithActionResult::Error(TEXT("faces must be a non-empty array"), -32602);
    }

    // --- Optional params ---
    TArray<FString> Warnings;

    EFontLoadingPolicy LoadingPolicy = EFontLoadingPolicy::LazyLoad;
    {
        FString LoadingPolicyStr;
        if (Params->TryGetStringField(TEXT("loading_policy"), LoadingPolicyStr) && !LoadingPolicyStr.IsEmpty())
        {
            if (!ParseLoadingPolicy(LoadingPolicyStr, LoadingPolicy))
            {
                Warnings.Add(FString::Printf(
                    TEXT("Unknown loading_policy '%s' -- falling back to LazyLoad. Supported: LazyLoad|Stream|Inline"),
                    *LoadingPolicyStr));
            }
        }
    }

    EFontHinting Hinting = EFontHinting::Default;
    {
        FString HintingStr;
        if (Params->TryGetStringField(TEXT("hinting"), HintingStr) && !HintingStr.IsEmpty())
        {
            if (!ParseHinting(HintingStr, Hinting))
            {
                Warnings.Add(FString::Printf(
                    TEXT("Unknown hinting '%s' -- falling back to Default. Supported: Default|Auto|AutoLight|Monochrome|None"),
                    *HintingStr));
            }
        }
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    // --- Parse face specs (fail fast on malformed entries before touching disk / packages) ---
    TArray<FFaceSpec> FaceSpecs;
    FaceSpecs.Reserve(FacesArr->Num());
    for (int32 i = 0; i < FacesArr->Num(); ++i)
    {
        const TSharedPtr<FJsonValue>& Entry = (*FacesArr)[i];
        const TSharedPtr<FJsonObject>* FaceObjPtr = nullptr;
        if (!Entry.IsValid() || !Entry->TryGetObject(FaceObjPtr) || !FaceObjPtr || !FaceObjPtr->IsValid())
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("faces[%d] must be an object with {typeface, source_path}"), i),
                -32602);
        }
        const TSharedPtr<FJsonObject>& FaceObj = *FaceObjPtr;

        FFaceSpec Spec;
        if (!FaceObj->TryGetStringField(TEXT("typeface"), Spec.Typeface) || Spec.Typeface.IsEmpty())
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("faces[%d].typeface must be a non-empty string"), i),
                -32602);
        }
        if (!FaceObj->TryGetStringField(TEXT("source_path"), Spec.SourcePath) || Spec.SourcePath.IsEmpty())
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("faces[%d].source_path must be a non-empty string"), i),
                -32602);
        }
        FaceSpecs.Add(MoveTemp(Spec));
    }

    // Guard against duplicate typeface names up-front (two entries both "Regular"
    // would create colliding F_Regular assets and a composite with duplicate key).
    {
        TSet<FString> Seen;
        for (const FFaceSpec& Spec : FaceSpecs)
        {
            bool bAlreadyIn = false;
            Seen.Add(Spec.Typeface, &bAlreadyIn);
            if (bAlreadyIn)
            {
                return FMonolithActionResult::Error(
                    FString::Printf(TEXT("Duplicate typeface '%s' in faces[] -- each entry must be unique"), *Spec.Typeface),
                    -32602);
            }
        }
    }

    // --- Per-face import ---
    // Per-face error doesn't abort the whole batch (log warning, continue). We
    // still require at least one face to succeed before creating the composite UFont.
    FAssetToolsModule& AssetToolsModule =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

    TArray<FFaceResult> FaceResults;
    FaceResults.Reserve(FaceSpecs.Num());

    for (int32 i = 0; i < FaceSpecs.Num(); ++i)
    {
        const FFaceSpec& Spec = FaceSpecs[i];

        if (!FPaths::FileExists(Spec.SourcePath))
        {
            Warnings.Add(FString::Printf(
                TEXT("faces[%d] ('%s'): source_path '%s' does not exist -- skipping"),
                i, *Spec.Typeface, *Spec.SourcePath));
            continue;
        }

        TArray<uint8> TtfBytes;
        if (!FFileHelper::LoadFileToArray(TtfBytes, *Spec.SourcePath) || TtfBytes.Num() == 0)
        {
            Warnings.Add(FString::Printf(
                TEXT("faces[%d] ('%s'): failed to read '%s' or file is empty -- skipping"),
                i, *Spec.Typeface, *Spec.SourcePath));
            continue;
        }

        const FString DesiredFaceAssetName = FString::Printf(TEXT("F_%s"), *Spec.Typeface);
        const FString DesiredFacePackageBase = Destination / DesiredFaceAssetName;

        FString UniqueFacePackageName;
        FString UniqueFaceAssetName;
        AssetToolsModule.Get().CreateUniqueAssetName(
            DesiredFacePackageBase, /*Suffix=*/FString(),
            /*out*/ UniqueFacePackageName, /*out*/ UniqueFaceAssetName);

        UPackage* FacePackage = CreatePackage(*UniqueFacePackageName);
        if (!FacePackage)
        {
            Warnings.Add(FString::Printf(
                TEXT("faces[%d] ('%s'): CreatePackage failed for '%s' -- skipping"),
                i, *Spec.Typeface, *UniqueFacePackageName));
            continue;
        }
        FacePackage->FullyLoad();

        UFontFace* FaceAsset = NewObject<UFontFace>(
            FacePackage, FName(*UniqueFaceAssetName), RF_Public | RF_Standalone);
        if (!FaceAsset)
        {
            Warnings.Add(FString::Printf(
                TEXT("faces[%d] ('%s'): NewObject<UFontFace> failed -- skipping"),
                i, *Spec.Typeface));
            continue;
        }

        // FontFaceData is a FFontFaceDataRef (TSharedRef) -- construct via the
        // static factory so the internal refcount lifecycle is correct.
        FaceAsset->FontFaceData = FFontFaceData::MakeFontFaceData(MoveTemp(TtfBytes));
        FaceAsset->SourceFilename = Spec.SourcePath;
        FaceAsset->Hinting = Hinting;
        FaceAsset->LoadingPolicy = LoadingPolicy;

#if WITH_EDITORONLY_DATA
        FaceAsset->CacheSubFaces();
#endif // WITH_EDITORONLY_DATA

        FaceAsset->PostEditChange();
        FAssetRegistryModule::AssetCreated(FaceAsset);
        FacePackage->MarkPackageDirty();

        if (bSave)
        {
            const FString FacePackageFilename = FPackageName::LongPackageNameToFilename(
                FacePackage->GetName(), FPackageName::GetAssetPackageExtension());

            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
            SaveArgs.SaveFlags = SAVE_NoError;
            const bool bSaved = UPackage::SavePackage(
                FacePackage, FaceAsset, *FacePackageFilename, SaveArgs);
            if (!bSaved)
            {
                Warnings.Add(FString::Printf(
                    TEXT("faces[%d] ('%s'): SavePackage failed for '%s' -- face in-memory but not on disk"),
                    i, *Spec.Typeface, *FacePackageFilename));
            }
        }

        FFaceResult R;
        R.Face = FaceAsset;
        R.AssetPath = UniqueFacePackageName;
        R.Typeface = Spec.Typeface;
        FaceResults.Add(MoveTemp(R));
    }

    if (FaceResults.Num() == 0)
    {
        return FMonolithActionResult::Error(
            TEXT("All face imports failed -- see warnings in prior calls. No composite UFont was created."),
            -32603);
    }

    // --- Create UFont composite ---
    const FString DesiredFamilyPackageBase = Destination / FamilyName;
    FString UniqueFamilyPackageName;
    FString UniqueFamilyAssetName;
    AssetToolsModule.Get().CreateUniqueAssetName(
        DesiredFamilyPackageBase, /*Suffix=*/FString(),
        /*out*/ UniqueFamilyPackageName, /*out*/ UniqueFamilyAssetName);

    UPackage* FamilyPackage = CreatePackage(*UniqueFamilyPackageName);
    if (!FamilyPackage)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Failed to create family package '%s'"), *UniqueFamilyPackageName),
            -32603);
    }
    FamilyPackage->FullyLoad();

    UFont* FamilyFont = NewObject<UFont>(
        FamilyPackage, FName(*UniqueFamilyAssetName), RF_Public | RF_Standalone);
    if (!FamilyFont)
    {
        return FMonolithActionResult::Error(TEXT("NewObject<UFont> failed for family asset"), -32603);
    }

    FamilyFont->FontCacheType = EFontCacheType::Runtime;
    FamilyFont->LegacyFontName = FName(*UniqueFamilyAssetName);

    // UE 5.7: direct public write to UFont::CompositeFont is UE_DEPRECATED -- the
    // header instructs callers to go through GetMutableInternalCompositeFont().
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
    FCompositeFont& Composite = FamilyFont->GetMutableInternalCompositeFont();
#else
    FCompositeFont& Composite = FamilyFont->CompositeFont;
#endif
    Composite.DefaultTypeface.Fonts.Reset();

    for (const FFaceResult& R : FaceResults)
    {
        FTypefaceEntry Entry;
        Entry.Name = FName(*R.Typeface);
        Entry.Font = FFontData(R.Face);
        Composite.DefaultTypeface.Fonts.Add(MoveTemp(Entry));
    }

#if WITH_EDITORONLY_DATA
    Composite.MakeDirty();
#endif // WITH_EDITORONLY_DATA

    FamilyFont->PostEditChange();
    FAssetRegistryModule::AssetCreated(FamilyFont);
    FamilyPackage->MarkPackageDirty();

    if (bSave)
    {
        const FString FamilyPackageFilename = FPackageName::LongPackageNameToFilename(
            FamilyPackage->GetName(), FPackageName::GetAssetPackageExtension());

        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        const bool bSaved = UPackage::SavePackage(
            FamilyPackage, FamilyFont, *FamilyPackageFilename, SaveArgs);
        if (!bSaved)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("UPackage::SavePackage failed for family '%s'"), *FamilyPackageFilename),
                -32603);
        }
    }

    // --- Success payload ---
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("family_asset_path"), UniqueFamilyPackageName);

    TArray<TSharedPtr<FJsonValue>> FacePathsJson;
    FacePathsJson.Reserve(FaceResults.Num());
    for (const FFaceResult& R : FaceResults)
    {
        FacePathsJson.Add(MakeShared<FJsonValueString>(R.AssetPath));
    }
    Result->SetArrayField(TEXT("face_asset_paths"), FacePathsJson);
    Result->SetNumberField(TEXT("faces_imported"), (double)FaceResults.Num());
    Result->SetNumberField(TEXT("faces_requested"), (double)FaceSpecs.Num());

    if (Warnings.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> WarnJson;
        for (const FString& W : Warnings)
        {
            WarnJson.Add(MakeShared<FJsonValueString>(W));
        }
        Result->SetArrayField(TEXT("warnings"), WarnJson);
    }

    return FMonolithActionResult::Success(Result);
}

void MonolithUI::FFontIngestActions::Register(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("ui"),
        TEXT("import_font_family"),
        TEXT("Import a font family (one-or-more TTF files) as a UFont composite asset plus one UFontFace per typeface entry. "
             "Params: destination (string, required, /Game/... output directory), "
             "family_name (string, required, UFont composite asset name), "
             "faces (array<object>, required, non-empty, each { typeface: string (e.g. 'Regular','Bold'), source_path: string (absolute TTF path) }), "
             "loading_policy (string, optional, default 'LazyLoad', one of LazyLoad|Stream|Inline), "
             "hinting (string, optional, default 'Default', one of Default|Auto|AutoLight|Monochrome|None), "
             "save (bool, optional, default true). "
             "Per-face errors don't abort the batch -- if at least one face imports, the composite UFont is still created; "
             "failed faces appear in the warnings array. Returns { family_asset_path, face_asset_paths[], faces_imported, faces_requested, warnings? }."),
        FMonolithActionHandler::CreateStatic(&MonolithUI::FFontIngestActions::HandleImportFontFamily));
}
