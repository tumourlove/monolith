#include "Indexers/GenericAssetIndexer.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundCue.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

bool FGenericAssetIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!LoadedAsset) return false;

	FIndexedNode MetaNode;
	MetaNode.AssetId = AssetId;
	MetaNode.NodeType = TEXT("Metadata");
	MetaNode.NodeName = LoadedAsset->GetName();
	MetaNode.NodeClass = LoadedAsset->GetClass()->GetName();

	auto Props = MakeShared<FJsonObject>();

	if (UStaticMesh* SM = Cast<UStaticMesh>(LoadedAsset))
	{
		if (SM->GetRenderData() && SM->GetRenderData()->LODResources.Num() > 0)
		{
			const FStaticMeshLODResources& LOD0 = SM->GetRenderData()->LODResources[0];
			Props->SetNumberField(TEXT("triangles"), LOD0.GetNumTriangles());
			Props->SetNumberField(TEXT("vertices"), LOD0.GetNumVertices());
			Props->SetNumberField(TEXT("sections"), LOD0.Sections.Num());
		}
		Props->SetNumberField(TEXT("lod_count"), SM->GetNumLODs());
		Props->SetNumberField(TEXT("material_slots"), SM->GetStaticMaterials().Num());

		FBoxSphereBounds Bounds = SM->GetBounds();
		Props->SetStringField(TEXT("bounds_extent"),
			FString::Printf(TEXT("%.1f x %.1f x %.1f"),
				Bounds.BoxExtent.X * 2, Bounds.BoxExtent.Y * 2, Bounds.BoxExtent.Z * 2));

		Props->SetBoolField(TEXT("has_collision"), SM->GetBodySetup() != nullptr);
	}
	else if (USkeletalMesh* SK = Cast<USkeletalMesh>(LoadedAsset))
	{
		Props->SetNumberField(TEXT("lod_count"), SK->GetLODNum());
		Props->SetNumberField(TEXT("material_slots"), SK->GetMaterials().Num());

		if (SK->GetSkeleton())
		{
			Props->SetNumberField(TEXT("bone_count"), SK->GetSkeleton()->GetReferenceSkeleton().GetNum());
			Props->SetStringField(TEXT("skeleton"), SK->GetSkeleton()->GetPathName());
		}

		if (SK->GetPhysicsAsset())
		{
			Props->SetStringField(TEXT("physics_asset"), SK->GetPhysicsAsset()->GetPathName());
		}
	}
	else if (UTexture2D* Tex = Cast<UTexture2D>(LoadedAsset))
	{
		Props->SetNumberField(TEXT("width"), Tex->GetSizeX());
		Props->SetNumberField(TEXT("height"), Tex->GetSizeY());
		Props->SetStringField(TEXT("format"), GPixelFormats[Tex->GetPixelFormat()].Name);
		Props->SetNumberField(TEXT("mip_count"), Tex->GetNumMips());
		Props->SetBoolField(TEXT("srgb"), Tex->SRGB);
		Props->SetBoolField(TEXT("has_alpha"), Tex->HasAlphaChannel());
		Props->SetStringField(TEXT("compression"),
			UEnum::GetValueAsString(Tex->CompressionSettings));
		Props->SetStringField(TEXT("lod_group"),
			UEnum::GetValueAsString(Tex->LODGroup));
		Props->SetStringField(TEXT("filter"),
			UEnum::GetValueAsString(Tex->Filter));
		Props->SetStringField(TEXT("address_x"),
			UEnum::GetValueAsString(Tex->GetTextureAddressX()));
		Props->SetStringField(TEXT("address_y"),
			UEnum::GetValueAsString(Tex->GetTextureAddressY()));
#if WITH_EDITORONLY_DATA
		Props->SetBoolField(TEXT("virtual_texture_streaming"), Tex->VirtualTextureStreaming != 0);
		Props->SetBoolField(TEXT("compression_no_alpha"), Tex->CompressionNoAlpha != 0);
#endif
		// Recommended sampler type for material use
		EMaterialSamplerType SamplerType = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(Tex);
		UEnum* SamplerEnum = StaticEnum<EMaterialSamplerType>();
		if (SamplerEnum)
		{
			Props->SetStringField(TEXT("recommended_sampler_type"),
				SamplerEnum->GetNameStringByValue(static_cast<int64>(SamplerType)));
		}
	}
	else if (USoundWave* Sound = Cast<USoundWave>(LoadedAsset))
	{
		Props->SetNumberField(TEXT("duration"), Sound->Duration);
		Props->SetNumberField(TEXT("sample_rate"), Sound->GetSampleRateForCurrentPlatform());
		Props->SetNumberField(TEXT("channels"), Sound->NumChannels);
		Props->SetBoolField(TEXT("looping"), Sound->bLooping);
	}

	FString PropsStr;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PropsStr);
	FJsonSerializer::Serialize(Props, *Writer, true);
	MetaNode.Properties = PropsStr;

	DB.InsertNode(MetaNode);
	return true;
}
