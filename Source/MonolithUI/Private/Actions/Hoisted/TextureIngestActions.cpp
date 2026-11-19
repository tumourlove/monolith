// Copyright tumourlove. All Rights Reserved.
#include "Actions/Hoisted/TextureIngestActions.h"

// Monolith registry
#include "MonolithToolRegistry.h"

// Core / JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Base64.h"                        // FBase64::Decode
#include "Misc/PackageName.h"                   // FPackageName::GetLongPackagePath / LongPackageNameToFilename / GetAssetPackageExtension
#include "UObject/Package.h"                    // UPackage, SavePackage
#include "UObject/SavePackage.h"                // FSavePackageArgs
#include "UObject/UObjectGlobals.h"             // CreatePackage, NewObject
#include "HAL/UnrealMemory.h"                   // FMemory::Memcpy

// Image decoding
#include "IImageWrapper.h"                      // IImageWrapper, ERGBFormat, EImageFormat
#include "IImageWrapperModule.h"                // IImageWrapperModule
#include "Modules/ModuleManager.h"              // FModuleManager::LoadModuleChecked

// Texture creation
#include "Engine/Texture.h"                     // TSF_BGRA8, TextureCompressionSettings, TextureMipGenSettings
#include "Engine/Texture2D.h"                   // UTexture2D
#include "Engine/TextureDefines.h"              // TEXTUREGROUP_UI, TEXTUREGROUP_World, etc.
#include "TextureResource.h"                    // FTexturePlatformData, FTexture2DMipMap
#include "PixelFormat.h"                        // PF_B8G8R8A8

// Asset registry + asset tools (unique naming)
#include "AssetRegistry/AssetRegistryModule.h"  // FAssetRegistryModule::AssetCreated
#include "AssetToolsModule.h"                   // FAssetToolsModule
#include "IAssetTools.h"                        // IAssetTools::CreateUniqueAssetName
#include "MonolithAssetUtils.h"

namespace MonolithUI::TextureIngestInternal
{
    // Map a "png" / "jpg" / "jpeg" / "bmp" / "exr" / "tga" hint to an EImageFormat.
    // NOTE: UE 5.7 EImageFormat has NO WebP member (checked against IImageWrapper.h:26-69).
    // Returns EImageFormat::Invalid on unknown hint so the caller can error out with -32602.
    static EImageFormat ParseFormatHint(const FString& Hint)
    {
        const FString Lower = Hint.ToLower();
        if (Lower == TEXT("png"))                               { return EImageFormat::PNG;  }
        if (Lower == TEXT("jpg") || Lower == TEXT("jpeg"))      { return EImageFormat::JPEG; }
        if (Lower == TEXT("bmp"))                               { return EImageFormat::BMP;  }
        if (Lower == TEXT("exr"))                               { return EImageFormat::EXR;  }
        if (Lower == TEXT("tga"))                               { return EImageFormat::TGA;  }
        if (Lower == TEXT("hdr"))                               { return EImageFormat::HDR;  }
        if (Lower == TEXT("tif") || Lower == TEXT("tiff"))      { return EImageFormat::TIFF; }
        if (Lower == TEXT("dds"))                               { return EImageFormat::DDS;  }
        return EImageFormat::Invalid;
    }

    // Map a "TC_Default" / "TC_Grayscale" / ... string to TextureCompressionSettings.
    // Unrecognised strings fall back to TC_Default.
    static TextureCompressionSettings ParseCompression(const FString& S)
    {
        if (S == TEXT("TC_Default"))                 { return TC_Default;                 }
        if (S == TEXT("TC_Normalmap"))               { return TC_Normalmap;               }
        if (S == TEXT("TC_Masks"))                   { return TC_Masks;                   }
        if (S == TEXT("TC_Grayscale"))               { return TC_Grayscale;               }
        if (S == TEXT("TC_Displacementmap"))         { return TC_Displacementmap;         }
        if (S == TEXT("TC_VectorDisplacementmap"))   { return TC_VectorDisplacementmap;   }
        if (S == TEXT("TC_HDR"))                     { return TC_HDR;                     }
        if (S == TEXT("TC_EditorIcon"))              { return TC_EditorIcon;              }
        if (S == TEXT("TC_Alpha"))                   { return TC_Alpha;                   }
        if (S == TEXT("TC_DistanceFieldFont"))       { return TC_DistanceFieldFont;       }
        if (S == TEXT("TC_HDR_Compressed"))          { return TC_HDR_Compressed;          }
        if (S == TEXT("TC_BC7"))                     { return TC_BC7;                     }
        if (S == TEXT("TC_HalfFloat"))               { return TC_HalfFloat;               }
        if (S == TEXT("TC_LQ"))                      { return TC_LQ;                      }
        if (S == TEXT("TC_EncodedReflectionCapture")){ return TC_EncodedReflectionCapture;}
        if (S == TEXT("TC_SingleFloat"))             { return TC_SingleFloat;             }
        return TC_Default;
    }

    // Map a "TMGS_FromTextureGroup" / "TMGS_NoMipmaps" / ... string to TextureMipGenSettings.
    // Unrecognised strings fall back to TMGS_NoMipmaps (UI default -- matches spec).
    static TextureMipGenSettings ParseMipGen(const FString& S)
    {
        if (S == TEXT("TMGS_FromTextureGroup"))  { return TMGS_FromTextureGroup;  }
        if (S == TEXT("TMGS_SimpleAverage"))     { return TMGS_SimpleAverage;     }
        if (S == TEXT("TMGS_Sharpen0"))          { return TMGS_Sharpen0;          }
        if (S == TEXT("TMGS_Sharpen1"))          { return TMGS_Sharpen1;          }
        if (S == TEXT("TMGS_Sharpen2"))          { return TMGS_Sharpen2;          }
        if (S == TEXT("TMGS_Sharpen3"))          { return TMGS_Sharpen3;          }
        if (S == TEXT("TMGS_Sharpen4"))          { return TMGS_Sharpen4;          }
        if (S == TEXT("TMGS_Sharpen5"))          { return TMGS_Sharpen5;          }
        if (S == TEXT("TMGS_Sharpen6"))          { return TMGS_Sharpen6;          }
        if (S == TEXT("TMGS_Sharpen7"))          { return TMGS_Sharpen7;          }
        if (S == TEXT("TMGS_Sharpen8"))          { return TMGS_Sharpen8;          }
        if (S == TEXT("TMGS_Sharpen9"))          { return TMGS_Sharpen9;          }
        if (S == TEXT("TMGS_Sharpen10"))         { return TMGS_Sharpen10;         }
        if (S == TEXT("TMGS_NoMipmaps"))         { return TMGS_NoMipmaps;         }
        if (S == TEXT("TMGS_LeaveExistingMips")) { return TMGS_LeaveExistingMips; }
        if (S == TEXT("TMGS_Blur1"))             { return TMGS_Blur1;             }
        if (S == TEXT("TMGS_Blur2"))             { return TMGS_Blur2;             }
        if (S == TEXT("TMGS_Blur3"))             { return TMGS_Blur3;             }
        if (S == TEXT("TMGS_Blur4"))             { return TMGS_Blur4;             }
        if (S == TEXT("TMGS_Blur5"))             { return TMGS_Blur5;             }
        return TMGS_NoMipmaps;
    }

    // Map a "TEXTUREGROUP_UI" / "TEXTUREGROUP_World" / ... string to a TextureGroup enum.
    // Unrecognised strings fall back to TEXTUREGROUP_UI (spec default).
    static TextureGroup ParseLODGroup(const FString& S)
    {
        if (S == TEXT("TEXTUREGROUP_World"))                { return TEXTUREGROUP_World;                }
        if (S == TEXT("TEXTUREGROUP_WorldNormalMap"))       { return TEXTUREGROUP_WorldNormalMap;       }
        if (S == TEXT("TEXTUREGROUP_WorldSpecular"))        { return TEXTUREGROUP_WorldSpecular;        }
        if (S == TEXT("TEXTUREGROUP_Character"))            { return TEXTUREGROUP_Character;            }
        if (S == TEXT("TEXTUREGROUP_CharacterNormalMap"))   { return TEXTUREGROUP_CharacterNormalMap;   }
        if (S == TEXT("TEXTUREGROUP_CharacterSpecular"))    { return TEXTUREGROUP_CharacterSpecular;    }
        if (S == TEXT("TEXTUREGROUP_Weapon"))               { return TEXTUREGROUP_Weapon;               }
        if (S == TEXT("TEXTUREGROUP_WeaponNormalMap"))      { return TEXTUREGROUP_WeaponNormalMap;      }
        if (S == TEXT("TEXTUREGROUP_WeaponSpecular"))       { return TEXTUREGROUP_WeaponSpecular;       }
        if (S == TEXT("TEXTUREGROUP_Vehicle"))              { return TEXTUREGROUP_Vehicle;              }
        if (S == TEXT("TEXTUREGROUP_VehicleNormalMap"))     { return TEXTUREGROUP_VehicleNormalMap;     }
        if (S == TEXT("TEXTUREGROUP_VehicleSpecular"))      { return TEXTUREGROUP_VehicleSpecular;      }
        if (S == TEXT("TEXTUREGROUP_Cinematic"))            { return TEXTUREGROUP_Cinematic;            }
        if (S == TEXT("TEXTUREGROUP_Effects"))              { return TEXTUREGROUP_Effects;              }
        if (S == TEXT("TEXTUREGROUP_EffectsNotFiltered"))   { return TEXTUREGROUP_EffectsNotFiltered;   }
        if (S == TEXT("TEXTUREGROUP_Skybox"))               { return TEXTUREGROUP_Skybox;               }
        if (S == TEXT("TEXTUREGROUP_UI"))                   { return TEXTUREGROUP_UI;                   }
        if (S == TEXT("TEXTUREGROUP_Lightmap"))             { return TEXTUREGROUP_Lightmap;             }
        if (S == TEXT("TEXTUREGROUP_Shadowmap"))            { return TEXTUREGROUP_Shadowmap;            }
        return TEXTUREGROUP_UI;
    }
} // namespace MonolithUI::TextureIngestInternal

FMonolithActionResult MonolithUI::FTextureIngestActions::HandleImportTextureFromBytes(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUI::TextureIngestInternal;

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
            FString::Printf(TEXT("destination must be a long package path like /Game/Foo/Bar (got '%s')"), *Destination),
            -32602);
    }
    if (Destination.EndsWith(TEXT(".uasset")))
    {
        Destination = Destination.LeftChop(7);
    }

    FString BytesB64;
    if (!Params->TryGetStringField(TEXT("bytes_b64"), BytesB64) || BytesB64.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Missing or empty required param: bytes_b64"), -32602);
    }

    FString FormatHint;
    if (!Params->TryGetStringField(TEXT("format_hint"), FormatHint) || FormatHint.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Missing or empty required param: format_hint"), -32602);
    }

    const EImageFormat Format = ParseFormatHint(FormatHint);
    if (Format == EImageFormat::Invalid)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Unknown format_hint '%s' (supported: png, jpg, jpeg, bmp, exr, tga, hdr, tif, tiff, dds)"), *FormatHint),
            -32602);
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    TextureCompressionSettings Compression = TC_Default;
    bool bSRGB = true;
    TextureMipGenSettings MipGen = TMGS_NoMipmaps;
    TextureGroup LODGroup = TEXTUREGROUP_UI;

    const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
    if (Params->TryGetObjectField(TEXT("settings"), SettingsObj) && SettingsObj && SettingsObj->IsValid())
    {
        FString CompressionStr;
        if ((*SettingsObj)->TryGetStringField(TEXT("compression_settings"), CompressionStr))
        {
            Compression = ParseCompression(CompressionStr);
        }

        bool bSRGBValue = true;
        if ((*SettingsObj)->TryGetBoolField(TEXT("srgb"), bSRGBValue))
        {
            bSRGB = bSRGBValue;
        }

        FString MipGenStr;
        if ((*SettingsObj)->TryGetStringField(TEXT("mip_gen_settings"), MipGenStr))
        {
            MipGen = ParseMipGen(MipGenStr);
        }

        FString LODGroupStr;
        if ((*SettingsObj)->TryGetStringField(TEXT("lod_group"), LODGroupStr))
        {
            LODGroup = ParseLODGroup(LODGroupStr);
        }
    }

    // --- Base64 decode ---
    TArray<uint8> CompressedBytes;
    if (!FBase64::Decode(BytesB64, CompressedBytes) || CompressedBytes.Num() == 0)
    {
        return FMonolithActionResult::Error(TEXT("Base64 decode of bytes_b64 failed or produced empty buffer"), -32602);
    }

    // --- Image wrapper: decode compressed bytes to raw BGRA8 ---
    IImageWrapperModule& ImageWrapperModule =
        FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

    TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(Format);
    if (!Wrapper.IsValid())
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Failed to create IImageWrapper for format '%s'"), *FormatHint),
            -32603);
    }

    if (!Wrapper->SetCompressed(CompressedBytes.GetData(), CompressedBytes.Num()))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("IImageWrapper::SetCompressed failed for '%s' bytes"), *FormatHint),
            -32603);
    }

    // GetRaw(BGRA, 8) is the documented happy path for 8-bit PNG/JPEG/BMP input;
    // wrapper implementations handle the RGBA<->BGRA swizzle internally.
    TArray<uint8> RawBgra;
    if (!Wrapper->GetRaw(ERGBFormat::BGRA, /*BitDepth=*/8, RawBgra) || RawBgra.Num() == 0)
    {
        return FMonolithActionResult::Error(
            TEXT("IImageWrapper::GetRaw(ERGBFormat::BGRA, 8) failed or produced empty buffer"),
            -32603);
    }

    const int32 W = Wrapper->GetWidth();
    const int32 H = Wrapper->GetHeight();
    if (W <= 0 || H <= 0)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Decoded image has invalid dimensions: %dx%d"), W, H),
            -32603);
    }

    const int64 ExpectedBytes = (int64)W * (int64)H * 4;
    if (RawBgra.Num() < ExpectedBytes)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Decoded pixel buffer too small: got %d, expected %lld for %dx%d BGRA8"),
                RawBgra.Num(), ExpectedBytes, W, H),
            -32603);
    }

    // --- Resolve a unique package + asset name ---
    FAssetToolsModule& AssetToolsModule =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

    FString UniquePackageName;
    FString UniqueAssetName;
    AssetToolsModule.Get().CreateUniqueAssetName(
        Destination, /*Suffix=*/FString(),
        /*out*/ UniquePackageName, /*out*/ UniqueAssetName);

    // --- Create package + texture ---
    UPackage* Package = CreatePackage(*UniquePackageName);
    if (!Package)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Failed to create package '%s'"), *UniquePackageName),
            -32603);
    }
    Package->FullyLoad();

    UTexture2D* Texture = NewObject<UTexture2D>(
        Package, FName(*UniqueAssetName), RF_Public | RF_Standalone);
    if (!Texture)
    {
        return FMonolithActionResult::Error(TEXT("Failed to create UTexture2D object"), -32603);
    }

    // --- Platform data (runtime GPU side) ---
    FTexturePlatformData* PlatformData = new FTexturePlatformData();
    PlatformData->SizeX = W;
    PlatformData->SizeY = H;
    PlatformData->PixelFormat = PF_B8G8R8A8;
    PlatformData->SetNumSlices(1);

    FTexture2DMipMap* Mip = new FTexture2DMipMap();
    Mip->SizeX = W;
    Mip->SizeY = H;
    PlatformData->Mips.Add(Mip);

    Mip->BulkData.Lock(LOCK_READ_WRITE);
    void* MipData = Mip->BulkData.Realloc(ExpectedBytes);
    FMemory::Memcpy(MipData, RawBgra.GetData(), ExpectedBytes);
    Mip->BulkData.Unlock();

    Texture->SetPlatformData(PlatformData);

    // --- Source data (editor side -- required for save-to-disk) ---
#if WITH_EDITOR
    Texture->Source.Init(W, H, /*NumSlices=*/1, /*NumMips=*/1, TSF_BGRA8, /*NewData=*/nullptr);
    {
        uint8* SourceData = Texture->Source.LockMip(0);
        if (SourceData)
        {
            FMemory::Memcpy(SourceData, RawBgra.GetData(), ExpectedBytes);
            Texture->Source.UnlockMip(0);
        }
        else
        {
            return FMonolithActionResult::Error(
                TEXT("UTexture2D::Source::LockMip(0) returned null after Init"), -32603);
        }
    }
#endif // WITH_EDITOR

    // --- Apply settings ---
    Texture->CompressionSettings = Compression;
    Texture->SRGB = bSRGB;
    Texture->MipGenSettings = MipGen;
    Texture->LODGroup = LODGroup;
    Texture->AddressX = TA_Clamp;
    Texture->AddressY = TA_Clamp;

    // --- Finalise ---
    Texture->UpdateResource();
    Texture->PostEditChange();

    FAssetRegistryModule::AssetCreated(Texture);

    Package->MarkPackageDirty();

    if (bSave)
    {
        const FString PackageFilename = FPackageName::LongPackageNameToFilename(
            Package->GetName(), FPackageName::GetAssetPackageExtension());

        const bool bSaved = FMonolithAssetUtils::SaveAssetPackage(Package);
        if (!bSaved)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("UPackage::SavePackage failed for '%s'"), *PackageFilename),
                -32603);
        }
    }

    const FString ResultAssetPath = UniquePackageName;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), ResultAssetPath);
    ResultObj->SetNumberField(TEXT("width"), (double)W);
    ResultObj->SetNumberField(TEXT("height"), (double)H);
    ResultObj->SetNumberField(TEXT("size_bytes"), (double)ExpectedBytes);
    return FMonolithActionResult::Success(ResultObj);
}

void MonolithUI::FTextureIngestActions::Register(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("ui"),
        TEXT("import_texture_from_bytes"),
        TEXT("Decode a base64-encoded image (PNG / JPEG / BMP / EXR / TGA / HDR / TIFF / DDS) and import it as a UTexture2D asset. "
             "Params: destination (string, required, /Game/... path without .uasset), "
             "bytes_b64 (string, required, base64 image bytes), "
             "format_hint (string, required, one of png|jpg|jpeg|bmp|exr|tga|hdr|tif|tiff|dds), "
             "settings (object, optional: compression_settings, srgb, mip_gen_settings, lod_group), "
             "save (bool, optional, default true)."),
        FMonolithActionHandler::CreateStatic(&MonolithUI::FTextureIngestActions::HandleImportTextureFromBytes));
}
