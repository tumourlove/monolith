// Copyright tumourlove. All Rights Reserved.

// =============================================================================
// MonolithEditorInspectActions.cpp
//
// Phase 2 of plan: 2026-05-26-monolith-editor-preview-expansion.md.
//
// Two new editor:: actions, both pure structured-data introspection — no
// render path, no scene capture, no thumbnail. Bodies registered from
// MonolithEditorActions.cpp::RegisterActions; declarations in the public
// MonolithEditorActions.h header.
//
//   editor::inspect_material_pbr      — walk a UMaterialInterface's parameter
//                                        set; classify base-color/normal/
//                                        roughness/metallic textures; detect
//                                        ORM/ARM/MRA packing by name.
//   editor::inspect_texture_channels  — lock UTexture2D source mip 0, compute
//                                        per-channel R/G/B/A min/max/mean,
//                                        optionally emit 4 grayscale split
//                                        PNGs for visual debugging.
//
// Both return their JSON keys at the top level (resolved open question #6
// in the plan) — keys go directly on FMonolithActionResult::Result, not
// wrapped under a nested "result" object.
// =============================================================================

#include "MonolithEditorActions.h"

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"

#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameters.h"

#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h" // ETextureSourceFormat

#include "PixelFormat.h"           // GetPixelFormatString
#include "ImageCore.h"             // FImage, ERawImageFormat, EGammaSpace
#include "ImageUtils.h"            // FImageUtils::SaveImageAutoFormat

// ----- Local helpers -----------------------------------------------------

namespace
{
	/** Lowercase token-substring test used by all classification heuristics. */
	static bool ContainsAnyToken(const FString& LowerName, std::initializer_list<const TCHAR*> Tokens)
	{
		for (const TCHAR* Token : Tokens)
		{
			if (LowerName.Contains(Token))
			{
				return true;
			}
		}
		return false;
	}

	/** Word-boundary token check (avoids "arm" matching "alarm"). */
	static bool ContainsDelimitedToken(const FString& LowerName, const TCHAR* Token)
	{
		const FString TokenString(Token);
		int32 SearchStart = 0;
		while (SearchStart < LowerName.Len())
		{
			const int32 FoundIndex = LowerName.Find(TokenString, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchStart);
			if (FoundIndex == INDEX_NONE)
			{
				return false;
			}
			const int32 BeforeIndex = FoundIndex - 1;
			const int32 AfterIndex = FoundIndex + TokenString.Len();
			const bool bBeforeBoundary = BeforeIndex < 0 || !FChar::IsAlnum(LowerName[BeforeIndex]);
			const bool bAfterBoundary = AfterIndex >= LowerName.Len() || !FChar::IsAlnum(LowerName[AfterIndex]);
			if (bBeforeBoundary && bAfterBoundary)
			{
				return true;
			}
			SearchStart = FoundIndex + 1;
		}
		return false;
	}

	static bool IsLikelyBaseColorName(const FString& LowerName)
	{
		return ContainsAnyToken(LowerName, { TEXT("basecolor"), TEXT("base_color"), TEXT("albedo"), TEXT("diffuse") });
	}

	static bool IsLikelyNormalName(const FString& LowerName)
	{
		return ContainsAnyToken(LowerName, { TEXT("normal"), TEXT("normalmap"), TEXT("normal_map") });
	}

	static bool IsLikelyRoughnessName(const FString& LowerName)
	{
		// Exclude obvious packed variants — those land on the packed_*_detected flags.
		if (LowerName.Contains(TEXT("metalrough")) || LowerName.Contains(TEXT("metallicroughness")))
		{
			return false;
		}
		return ContainsAnyToken(LowerName, { TEXT("roughness"), TEXT("rough_"), TEXT("_rough") });
	}

	static bool IsLikelyMetallicName(const FString& LowerName)
	{
		if (LowerName.Contains(TEXT("metalrough")) || LowerName.Contains(TEXT("metallicroughness")))
		{
			return false;
		}
		return ContainsAnyToken(LowerName, { TEXT("metallic"), TEXT("metalness"), TEXT("metal_"), TEXT("_metal") });
	}

	static bool IsLikelyOrmPackedName(const FString& LowerName)
	{
		return ContainsAnyToken(LowerName, {
				TEXT("occlusionroughnessmetal"),
				TEXT("occlusion_roughness_metal"),
				TEXT("ao_roughness_metallic")
			})
			|| ContainsDelimitedToken(LowerName, TEXT("orm"));
	}

	static bool IsLikelyArmPackedName(const FString& LowerName)
	{
		return ContainsAnyToken(LowerName, {
				TEXT("ambientroughnessmetal"),
				TEXT("ambient_roughness_metal")
			})
			|| ContainsDelimitedToken(LowerName, TEXT("arm"));
	}

	static bool IsLikelyMraPackedName(const FString& LowerName)
	{
		return ContainsAnyToken(LowerName, {
				TEXT("metallicroughnessao"),
				TEXT("metallic_roughness_ao"),
				TEXT("metallicroughnessocclusion"),
				TEXT("metallic_roughness_occlusion")
			})
			|| ContainsDelimitedToken(LowerName, TEXT("mra"));
	}

	/** Classify a UMaterialInterface to a short type string for the JSON payload. */
	static FString ClassifyMaterialClass(const UMaterialInterface* Material)
	{
		if (!Material)
		{
			return TEXT("Unknown");
		}
		if (Material->IsA<UMaterialInstanceConstant>())
		{
			return TEXT("MaterialInstanceConstant");
		}
		if (Material->IsA<UMaterialInstanceDynamic>())
		{
			return TEXT("MaterialInstanceDynamic");
		}
		if (Material->IsA<UMaterialInstance>())
		{
			return TEXT("MaterialInstance");
		}
		if (Material->IsA<UMaterial>())
		{
			return TEXT("Material");
		}
		return Material->GetClass() ? Material->GetClass()->GetName() : TEXT("Unknown");
	}
}

// ----- editor::inspect_material_pbr -------------------------------------

FMonolithActionResult FMonolithEditorActions::HandleInspectMaterialPBR(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing params"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *AssetPath);
	if (!Material)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load material interface: %s"), *AssetPath));
	}

	// Walk parameter sets via the virtual UMaterialInterface API. These are
	// available on BOTH base UMaterial and UMaterialInstance subclasses
	// (verified via offline source_query against UE 5.7 source).
	TArray<FMaterialParameterInfo> ScalarInfos;
	TArray<FGuid>                  ScalarGuids;
	Material->GetAllScalarParameterInfo(ScalarInfos, ScalarGuids);

	TArray<FMaterialParameterInfo> VectorInfos;
	TArray<FGuid>                  VectorGuids;
	Material->GetAllVectorParameterInfo(VectorInfos, VectorGuids);

	TArray<FMaterialParameterInfo> TextureInfos;
	TArray<FGuid>                  TextureGuids;
	Material->GetAllTextureParameterInfo(TextureInfos, TextureGuids);

	TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
	SlotObj->SetStringField(TEXT("slot_name"), TEXT("Material"));

	// Scalar params
	TArray<TSharedPtr<FJsonValue>> ScalarArr;
	for (const FMaterialParameterInfo& Info : ScalarInfos)
	{
		float Value = 0.0f;
		if (Material->GetScalarParameterValue(Info, Value))
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Info.Name.ToString());
			Entry->SetNumberField(TEXT("value"), Value);
			ScalarArr.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}
	SlotObj->SetArrayField(TEXT("scalar_params"), ScalarArr);

	// Vector params
	TArray<TSharedPtr<FJsonValue>> VectorArr;
	for (const FMaterialParameterInfo& Info : VectorInfos)
	{
		FLinearColor Value = FLinearColor::Black;
		if (Material->GetVectorParameterValue(Info, Value))
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Info.Name.ToString());
			TArray<TSharedPtr<FJsonValue>> RGBA;
			RGBA.Add(MakeShared<FJsonValueNumber>(Value.R));
			RGBA.Add(MakeShared<FJsonValueNumber>(Value.G));
			RGBA.Add(MakeShared<FJsonValueNumber>(Value.B));
			RGBA.Add(MakeShared<FJsonValueNumber>(Value.A));
			Entry->SetArrayField(TEXT("value"), RGBA);
			VectorArr.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}
	SlotObj->SetArrayField(TEXT("vector_params"), VectorArr);

	// Texture params + classification
	TArray<TSharedPtr<FJsonValue>> TextureArr;
	UTexture* BaseColorTex   = nullptr;
	UTexture* NormalTex      = nullptr;
	UTexture* RoughnessTex   = nullptr;
	UTexture* MetallicTex    = nullptr;
	bool bOrmDetected = false;
	bool bArmDetected = false;
	bool bMraDetected = false;

	for (const FMaterialParameterInfo& Info : TextureInfos)
	{
		UTexture* TextureValue = nullptr;
		if (!Material->GetTextureParameterValue(Info, TextureValue))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Info.Name.ToString());
		if (TextureValue)
		{
			Entry->SetStringField(TEXT("texture_path"), TextureValue->GetPathName());
		}
		else
		{
			Entry->SetField(TEXT("texture_path"), MakeShared<FJsonValueNull>());
		}
		TextureArr.Add(MakeShared<FJsonValueObject>(Entry));

		// Classification: param name + bound texture name (when present).
		const FString LowerParamName = Info.Name.ToString().ToLower();
		const FString LowerTextureName = TextureValue ? TextureValue->GetName().ToLower() : FString();
		const FString CompositeLower = LowerParamName + TEXT(" ") + LowerTextureName;

		const bool bIsOrm = IsLikelyOrmPackedName(LowerParamName) || IsLikelyOrmPackedName(LowerTextureName);
		const bool bIsArm = IsLikelyArmPackedName(LowerParamName) || IsLikelyArmPackedName(LowerTextureName);
		const bool bIsMra = IsLikelyMraPackedName(LowerParamName) || IsLikelyMraPackedName(LowerTextureName);

		if (bIsOrm) bOrmDetected = true;
		if (bIsArm) bArmDetected = true;
		if (bIsMra) bMraDetected = true;

		if (!BaseColorTex && (IsLikelyBaseColorName(LowerParamName) || IsLikelyBaseColorName(LowerTextureName)))
		{
			BaseColorTex = TextureValue;
		}
		if (!NormalTex && (IsLikelyNormalName(LowerParamName) || IsLikelyNormalName(LowerTextureName)))
		{
			NormalTex = TextureValue;
		}
		if (!RoughnessTex && (IsLikelyRoughnessName(LowerParamName) || IsLikelyRoughnessName(LowerTextureName)))
		{
			RoughnessTex = TextureValue;
		}
		if (!MetallicTex && (IsLikelyMetallicName(LowerParamName) || IsLikelyMetallicName(LowerTextureName)))
		{
			MetallicTex = TextureValue;
		}
	}
	SlotObj->SetArrayField(TEXT("texture_params"), TextureArr);

	auto SetTextureOrNull = [&SlotObj](const TCHAR* FieldName, UTexture* Tex)
	{
		if (Tex)
		{
			SlotObj->SetStringField(FieldName, Tex->GetPathName());
		}
		else
		{
			SlotObj->SetField(FieldName, MakeShared<FJsonValueNull>());
		}
	};
	SetTextureOrNull(TEXT("base_color_texture"), BaseColorTex);
	SetTextureOrNull(TEXT("normal_texture"),     NormalTex);
	SetTextureOrNull(TEXT("roughness_texture"),  RoughnessTex);
	SetTextureOrNull(TEXT("metallic_texture"),   MetallicTex);
	SlotObj->SetBoolField(TEXT("packed_orm_detected"), bOrmDetected);
	SlotObj->SetBoolField(TEXT("packed_arm_detected"), bArmDetected);
	SlotObj->SetBoolField(TEXT("packed_mra_detected"), bMraDetected);

	// UV channel count — best-effort. UE 5.7 doesn't expose a stable per-
	// material UV-channel-count query on the public UMaterialInterface, so
	// default to 1 per the plan's "best-effort; 1 if unknown" rule.
	SlotObj->SetNumberField(TEXT("uv_channel_count"), 1);

	// Top-level payload (resolved open question #6 — no nested "result" wrapper).
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("material_class"), ClassifyMaterialClass(Material));

	TArray<TSharedPtr<FJsonValue>> Slots;
	Slots.Add(MakeShared<FJsonValueObject>(SlotObj));
	Result->SetArrayField(TEXT("slots"), Slots);

	return FMonolithActionResult::Success(Result);
}

// ----- editor::inspect_texture_channels ---------------------------------

FMonolithActionResult FMonolithEditorActions::HandleInspectTextureChannels(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing params"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	bool bEmitSplits = false;
	Params->TryGetBoolField(TEXT("emit_splits"), bEmitSplits);

	UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *AssetPath);
	if (!Texture)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load UTexture2D: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);

	// Runtime pixel format — what the GPU sees post-build. May differ from the
	// source format (e.g. PF_DXT5 / PF_BC1 for cooked textures).
	const EPixelFormat PixelFormat = Texture->GetPixelFormat();
	Result->SetStringField(TEXT("format"), FString(GetPixelFormatString(PixelFormat)));
	Result->SetBoolField(TEXT("srgb"), Texture->SRGB);

	// Source-mip read — editor-only. Source format is the AUTHORED format,
	// independent of the GPU pixel format.
#if WITH_EDITOR
	if (!Texture->Source.IsValid() || Texture->Source.GetNumMips() <= 0)
	{
		// Still return a useful payload even when source bytes aren't accessible.
		Result->SetNumberField(TEXT("width"), Texture->GetSizeX());
		Result->SetNumberField(TEXT("height"), Texture->GetSizeY());
		Result->SetBoolField(TEXT("has_alpha"), false);
		Result->SetField(TEXT("channel_stats"), MakeShared<FJsonValueNull>());
		Result->SetField(TEXT("splits"), MakeShared<FJsonValueNull>());
		Result->SetStringField(TEXT("warning"),
			TEXT("Texture has no readable source data (cooked-only?). Returning runtime dimensions; channel_stats unavailable."));
		return FMonolithActionResult::Success(Result);
	}

	const int32 Width = Texture->Source.GetSizeX();
	const int32 Height = Texture->Source.GetSizeY();
	Result->SetNumberField(TEXT("width"), Width);
	Result->SetNumberField(TEXT("height"), Height);

	const ETextureSourceFormat SourceFormat = Texture->Source.GetFormat();

	// We only support TSF_BGRA8 for channel stats in this pass — the most common
	// case and the format Lumen / Substrate authoring pipelines emit. Other
	// formats (G8/G16/RGBA16/RGBA32F) return a clean "not supported" warning
	// rather than a misleading stat.
	if (SourceFormat != TSF_BGRA8)
	{
		Result->SetBoolField(TEXT("has_alpha"), false);
		Result->SetField(TEXT("channel_stats"), MakeShared<FJsonValueNull>());
		Result->SetField(TEXT("splits"), MakeShared<FJsonValueNull>());
		Result->SetStringField(TEXT("warning"),
			FString::Printf(TEXT("Source format %d not yet supported for channel inspection (supported: TSF_BGRA8)."),
				static_cast<int32>(SourceFormat)));
		return FMonolithActionResult::Success(Result);
	}

	const uint8* MipData = Texture->Source.LockMipReadOnly(0);
	if (!MipData)
	{
		Result->SetField(TEXT("channel_stats"), MakeShared<FJsonValueNull>());
		Result->SetField(TEXT("splits"), MakeShared<FJsonValueNull>());
		Result->SetStringField(TEXT("warning"), TEXT("LockMipReadOnly returned null."));
		return FMonolithActionResult::Success(Result);
	}

	const int64 PixelCount = static_cast<int64>(Width) * static_cast<int64>(Height);

	uint64 SumR = 0, SumG = 0, SumB = 0, SumA = 0;
	uint8 MinR = 255, MinG = 255, MinB = 255, MinA = 255;
	uint8 MaxR = 0,   MaxG = 0,   MaxB = 0,   MaxA = 0;
	bool bHasNonOpaqueAlpha = false;

	// TSF_BGRA8 — byte order is B,G,R,A.
	for (int64 i = 0; i < PixelCount; ++i)
	{
		const uint8 B = MipData[i * 4 + 0];
		const uint8 G = MipData[i * 4 + 1];
		const uint8 R = MipData[i * 4 + 2];
		const uint8 A = MipData[i * 4 + 3];

		SumR += R; SumG += G; SumB += B; SumA += A;
		MinR = FMath::Min(MinR, R); MaxR = FMath::Max(MaxR, R);
		MinG = FMath::Min(MinG, G); MaxG = FMath::Max(MaxG, G);
		MinB = FMath::Min(MinB, B); MaxB = FMath::Max(MaxB, B);
		MinA = FMath::Min(MinA, A); MaxA = FMath::Max(MaxA, A);

		if (A < 255)
		{
			bHasNonOpaqueAlpha = true;
		}
	}

	const double InvCount = PixelCount > 0 ? 1.0 / static_cast<double>(PixelCount) : 0.0;
	const double MeanR = SumR * InvCount;
	const double MeanG = SumG * InvCount;
	const double MeanB = SumB * InvCount;
	const double MeanA = SumA * InvCount;

	auto MakeChannelStats = [](uint8 InMin, uint8 InMax, double InMean) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("min"), InMin);
		Obj->SetNumberField(TEXT("max"), InMax);
		Obj->SetNumberField(TEXT("mean"), InMean);
		return Obj;
	};

	TSharedPtr<FJsonObject> ChannelStats = MakeShared<FJsonObject>();
	ChannelStats->SetObjectField(TEXT("r"), MakeChannelStats(MinR, MaxR, MeanR));
	ChannelStats->SetObjectField(TEXT("g"), MakeChannelStats(MinG, MaxG, MeanG));
	ChannelStats->SetObjectField(TEXT("b"), MakeChannelStats(MinB, MaxB, MeanB));
	ChannelStats->SetObjectField(TEXT("a"), MakeChannelStats(MinA, MaxA, MeanA));

	Result->SetBoolField(TEXT("has_alpha"), bHasNonOpaqueAlpha);
	Result->SetObjectField(TEXT("channel_stats"), ChannelStats);

	// Optional per-channel split PNGs. Each PNG is grayscale (channel value
	// replicated to R/G/B, alpha forced opaque) — keeps the format compatible
	// with any PNG viewer and avoids alpha confusion when inspecting masks.
	if (bEmitSplits)
	{
		FString OutputDir;
		Params->TryGetStringField(TEXT("output_dir"), OutputDir);
		if (OutputDir.IsEmpty())
		{
			OutputDir = FPaths::ProjectDir()
				/ TEXT("Saved/Tests/Monolith/InspectTexture")
				/ Texture->GetName();
		}
		else if (FPaths::IsRelative(OutputDir))
		{
			OutputDir = FPaths::ProjectDir() / OutputDir;
		}
		IFileManager::Get().MakeDirectory(*OutputDir, /*Tree=*/true);

		struct FChannelEmit { const TCHAR* Suffix; int32 Offset; };
		// Source byte order is BGRA → offsets 2,1,0,3 = R,G,B,A.
		const FChannelEmit ChannelEmits[4] = {
			{ TEXT("r"), 2 },
			{ TEXT("g"), 1 },
			{ TEXT("b"), 0 },
			{ TEXT("a"), 3 }
		};

		TSharedPtr<FJsonObject> SplitsObj = MakeShared<FJsonObject>();
		bool bAnyWriteFailed = false;

		for (const FChannelEmit& Emit : ChannelEmits)
		{
			FImage Image;
			Image.Init(Width, Height, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			uint8* DestBytes = reinterpret_cast<uint8*>(Image.RawData.GetData());

			for (int64 i = 0; i < PixelCount; ++i)
			{
				const uint8 ChannelValue = MipData[i * 4 + Emit.Offset];
				// Replicate to all three colour channels; force opaque alpha so
				// the PNG renders as a clean grayscale preview.
				DestBytes[i * 4 + 0] = ChannelValue; // B
				DestBytes[i * 4 + 1] = ChannelValue; // G
				DestBytes[i * 4 + 2] = ChannelValue; // R
				DestBytes[i * 4 + 3] = 255;          // A
			}

			const FString PngPath = OutputDir / FString::Printf(TEXT("%s_%s.png"), *Texture->GetName(), Emit.Suffix);
			const bool bOk = FImageUtils::SaveImageAutoFormat(*PngPath, Image);
			if (bOk)
			{
				SplitsObj->SetStringField(FString::Printf(TEXT("%s_png_path"), Emit.Suffix), PngPath);
			}
			else
			{
				bAnyWriteFailed = true;
				UE_LOG(LogTemp, Warning,
					TEXT("inspect_texture_channels: failed to write split PNG for channel %s at %s"),
					Emit.Suffix, *PngPath);
			}
		}

		if (bAnyWriteFailed && SplitsObj->Values.Num() == 0)
		{
			Result->SetField(TEXT("splits"), MakeShared<FJsonValueNull>());
		}
		else
		{
			Result->SetObjectField(TEXT("splits"), SplitsObj);
		}
	}
	else
	{
		Result->SetField(TEXT("splits"), MakeShared<FJsonValueNull>());
	}

	Texture->Source.UnlockMip(0);
#else
	// Non-editor build — source data API is editor-only. Stripped builds of
	// MonolithEditor never reach here (the whole module is Editor-typed), but
	// keep the guard so the file stays compilable in arbitrary build configs.
	Result->SetNumberField(TEXT("width"), Texture->GetSizeX());
	Result->SetNumberField(TEXT("height"), Texture->GetSizeY());
	Result->SetBoolField(TEXT("has_alpha"), false);
	Result->SetField(TEXT("channel_stats"), MakeShared<FJsonValueNull>());
	Result->SetField(TEXT("splits"), MakeShared<FJsonValueNull>());
	Result->SetStringField(TEXT("warning"), TEXT("FTextureSource is editor-only; channel_stats unavailable in non-editor builds."));
#endif

	return FMonolithActionResult::Success(Result);
}
