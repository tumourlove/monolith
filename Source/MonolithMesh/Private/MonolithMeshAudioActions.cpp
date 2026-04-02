#include "MonolithMeshAudioActions.h"
#include "MonolithMeshAcoustics.h"
#include "MonolithMeshUtils.h"
#include "MonolithMeshAnalysis.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithSettings.h"

#include "Engine/World.h"
#include "Engine/DataTable.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"
#include "Components/BrushComponent.h"
#include "CollisionQueryParams.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "Sound/AudioVolume.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "UObject/Package.h"

// ============================================================================
// Helpers
// ============================================================================

namespace
{
	TArray<TSharedPtr<FJsonValue>> MAud_VecToArr(const FVector& V)
	{
		return MonolithMeshAnalysis::VectorToJsonArray(V);
	}

	/** Parse an array of [x,y,z] arrays from a JSON field */
	bool MAud_ParseVectorArray(const TSharedPtr<FJsonObject>& Params, const FString& Key, TArray<FVector>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!Params->TryGetArrayField(Key, Arr) || Arr->Num() == 0)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Val : *Arr)
		{
			const TArray<TSharedPtr<FJsonValue>>* Inner;
			if (Val->TryGetArray(Inner) && Inner->Num() >= 3)
			{
				FVector V;
				V.X = (*Inner)[0]->AsNumber();
				V.Y = (*Inner)[1]->AsNumber();
				V.Z = (*Inner)[2]->AsNumber();
				Out.Add(V);
			}
		}

		return Out.Num() > 0;
	}

	/** Scoped undo transaction */
	struct FScopedAudioTransaction
	{
		bool bOwns;
		FScopedAudioTransaction(const FText& Desc)
			: bOwns(true)
		{
			if (GEditor) { GEditor->BeginTransaction(Desc); }
		}
		~FScopedAudioTransaction()
		{
			if (bOwns && GEditor) { GEditor->EndTransaction(); }
		}
		void Cancel()
		{
			if (bOwns && GEditor) { GEditor->CancelTransaction(0); bOwns = false; }
		}
	};

	/** Convert Unreal cm^3 to m^3 */
	float CmCubedToMCubed(float CmCubed)
	{
		// 1m = 100cm, so 1m^3 = 1e6 cm^3
		return CmCubed / 1e6f;
	}

	/** Convert Unreal cm^2 to m^2 */
	float CmSquaredToMSquared(float CmSquared)
	{
		return CmSquared / 1e4f;
	}

	/** Parse an integer from JSON number field (TryGetNumberField takes double&) */
	bool TryGetInt(const TSharedPtr<FJsonObject>& Params, const FString& Key, int32& Out)
	{
		double D;
		if (Params->TryGetNumberField(Key, D))
		{
			Out = static_cast<int32>(D);
			return true;
		}
		return false;
	}

	/** Parse a float from JSON number field */
	bool TryGetFloat(const TSharedPtr<FJsonObject>& Params, const FString& Key, float& Out)
	{
		double D;
		if (Params->TryGetNumberField(Key, D))
		{
			Out = static_cast<float>(D);
			return true;
		}
		return false;
	}

	/** Trace down from a location to get floor surface type */
	EPhysicalSurface TraceFloorSurface(UWorld* World, const FVector& Location, FHitResult& OutHit)
	{
		FCollisionQueryParams QP(SCENE_QUERY_STAT(MonolithFloorTrace), true);
		QP.bReturnPhysicalMaterial = true;

		FVector Start = Location;
		FVector End = Location - FVector(0, 0, 500.0f);

		if (World->LineTraceSingleByChannel(OutHit, Start, End, ECC_Visibility, QP))
		{
			if (OutHit.PhysMaterial.IsValid())
			{
				return OutHit.PhysMaterial->SurfaceType;
			}
		}

		return SurfaceType_Default;
	}

	/** Find a volume actor by name (BlockingVolume, AudioVolume, or generic) */
	AActor* FindVolumeByName(UWorld* World, const FString& VolumeName, FString& OutError)
	{
		// Check AudioVolumes first
		for (TActorIterator<AAudioVolume> It(World); It; ++It)
		{
			if (It->GetActorNameOrLabel() == VolumeName || It->GetActorLabel() == VolumeName
				|| It->GetFName().ToString() == VolumeName)
			{
				return *It;
			}
		}

		// Then blocking volumes
		AActor* Actor = MonolithMeshUtils::FindActorByName(VolumeName, OutError);
		return Actor;
	}

	/** Get volume bounds (origin + extent) from a named actor */
	bool GetVolumeBounds(const FString& VolumeName, FVector& OutOrigin, FVector& OutExtent, FString& OutError)
	{
		UWorld* World = MonolithMeshUtils::GetEditorWorld();
		if (!World)
		{
			OutError = TEXT("No editor world available");
			return false;
		}

		AActor* Volume = FindVolumeByName(World, VolumeName, OutError);
		if (!Volume)
		{
			return false;
		}

		Volume->GetActorBounds(false, OutOrigin, OutExtent);
		return true;
	}

	/** Acoustic properties as JSON object */
	TSharedPtr<FJsonObject> AcousticPropsToJson(const MonolithMeshAcoustics::FAcousticProperties& Props)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("surface"), Props.SurfaceName);
		Obj->SetNumberField(TEXT("absorption"), Props.Absorption);
		Obj->SetNumberField(TEXT("transmission_loss_db"), Props.TransmissionLossdB);
		Obj->SetNumberField(TEXT("footstep_loudness"), Props.FootstepLoudness);
		return Obj;
	}
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshAudioActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// === Read-Only (7) ===

	// 1. get_audio_volumes
	Registry.RegisterAction(TEXT("mesh"), TEXT("get_audio_volumes"),
		TEXT("Enumerate all AAudioVolume actors. Returns reverb/interior settings, priority, bounds. Flags uncovered regions."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAudioActions::GetAudioVolumes),
		FParamSchemaBuilder()
			.Optional(TEXT("include_details"), TEXT("boolean"), TEXT("Include full reverb/interior settings"), TEXT("true"))
			.Build());

	// 2. get_surface_materials
	Registry.RegisterAction(TEXT("mesh"), TEXT("get_surface_materials"),
		TEXT("Cast rays in all directions to catalog physical materials in a volume or region. Returns material breakdown with acoustic properties."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAudioActions::GetSurfaceMaterials),
		FParamSchemaBuilder()
			.Optional(TEXT("volume_name"), TEXT("string"), TEXT("Name of a volume to scan"))
			.Optional(TEXT("region_min"), TEXT("array"), TEXT("Min corner of region [x, y, z]"))
			.Optional(TEXT("region_max"), TEXT("array"), TEXT("Max corner of region [x, y, z]"))
			.Optional(TEXT("ray_count"), TEXT("integer"), TEXT("Number of rays per origin"), TEXT("64"))
			.Build());

	// 3. estimate_footstep_sound
	Registry.RegisterAction(TEXT("mesh"), TEXT("estimate_footstep_sound"),
		TEXT("Downward trace at a location to determine floor surface type and footstep loudness factor."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAudioActions::EstimateFootstepSound),
		FParamSchemaBuilder()
			.Required(TEXT("location"), TEXT("array"), TEXT("World position [x, y, z]"))
			.Build());

	// 4. analyze_room_acoustics
	Registry.RegisterAction(TEXT("mesh"), TEXT("analyze_room_acoustics"),
		TEXT("Sample surfaces via raycasts in a volume, compute area-weighted absorption. Sabine RT60: 0.161 * Volume_m3 / TotalAbsorption. Classify: dead/dry/live/echo."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAudioActions::AnalyzeRoomAcoustics),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Name of the volume to analyze"))
			.Optional(TEXT("ray_count"), TEXT("integer"), TEXT("Number of surface sample rays"), TEXT("128"))
			.Build());

	// 5. analyze_sound_propagation
	Registry.RegisterAction(TEXT("mesh"), TEXT("analyze_sound_propagation"),
		TEXT("Analyzes sound propagation between two points. Direct trace (wall occlusion) + indirect navmesh path (doorway propagation). Returns whichever path has better audibility."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAudioActions::AnalyzeSoundPropagation),
		FParamSchemaBuilder()
			.Required(TEXT("from"), TEXT("array"), TEXT("Source position [x, y, z]"))
			.Required(TEXT("to"), TEXT("array"), TEXT("Listener position [x, y, z]"))
			.Optional(TEXT("include_occlusion"), TEXT("boolean"), TEXT("Include wall occlusion analysis"), TEXT("true"))
			.Build());

	// 6. find_loud_surfaces
	Registry.RegisterAction(TEXT("mesh"), TEXT("find_loud_surfaces"),
		TEXT("Find surfaces with high footstep loudness (metal, glass, gravel) in a volume or region. Returns locations, areas, detection radii."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAudioActions::FindLoudSurfaces),
		FParamSchemaBuilder()
			.Optional(TEXT("volume_name"), TEXT("string"), TEXT("Volume to search"))
			.Optional(TEXT("region_min"), TEXT("array"), TEXT("Min corner [x, y, z]"))
			.Optional(TEXT("region_max"), TEXT("array"), TEXT("Max corner [x, y, z]"))
			.Optional(TEXT("loudness_threshold"), TEXT("number"), TEXT("Minimum footstep loudness to flag"), TEXT("0.5"))
			.Build());

	// 7. find_sound_paths
	Registry.RegisterAction(TEXT("mesh"), TEXT("find_sound_paths"),
		TEXT("Multi-method sound path finder: direct trace, first-bounce reflections (image-source), and navmesh indirect path (doorway propagation). Returns all viable paths sorted by attenuation."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAudioActions::FindSoundPaths),
		FParamSchemaBuilder()
			.Required(TEXT("from"), TEXT("array"), TEXT("Sound source position [x, y, z]"))
			.Required(TEXT("to"), TEXT("array"), TEXT("Listener position [x, y, z]"))
			.Optional(TEXT("max_bounces"), TEXT("integer"), TEXT("Max reflection bounces (1-3)"), TEXT("2"))
			.Optional(TEXT("candidate_surfaces"), TEXT("integer"), TEXT("Number of surfaces to test as reflectors"), TEXT("16"))
			.Build());

	// === Horror AI (4) ===

	// 8. can_ai_hear_from
	Registry.RegisterAction(TEXT("mesh"), TEXT("can_ai_hear_from"),
		TEXT("Can AI hear the player? Direct trace (wall occlusion) + indirect navmesh path (doorway propagation). Uses best path. Returns yes/faintly/no + detection radius."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAudioActions::CanAiHearFrom),
		FParamSchemaBuilder()
			.Required(TEXT("ai_location"), TEXT("array"), TEXT("AI position [x, y, z]"))
			.Required(TEXT("player_location"), TEXT("array"), TEXT("Player position [x, y, z]"))
			.Optional(TEXT("surface_type"), TEXT("string"), TEXT("Floor surface override (auto-detect if omitted)"))
			.Optional(TEXT("ai_hearing_range"), TEXT("number"), TEXT("AI hearing range in cm"), TEXT("2000"))
			.Build());

	// 9. get_stealth_map
	Registry.RegisterAction(TEXT("mesh"), TEXT("get_stealth_map"),
		TEXT("Grid-sample a volume: per-cell footstep loudness + AI detection radius. Returns heatmap data."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAudioActions::GetStealthMap),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Volume to grid-sample"))
			.Optional(TEXT("grid_size"), TEXT("number"), TEXT("Grid cell size in cm"), TEXT("100"))
			.Optional(TEXT("ai_hearing_range"), TEXT("number"), TEXT("AI hearing range for detection radius calc"), TEXT("2000"))
			.Build());

	// 10. find_quiet_path
	Registry.RegisterAction(TEXT("mesh"), TEXT("find_quiet_path"),
		TEXT("Sample candidate navmesh paths between two points, score by surface loudness along path. Returns lowest-loudness route."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAudioActions::FindQuietPath),
		FParamSchemaBuilder()
			.Required(TEXT("start"), TEXT("array"), TEXT("Start position [x, y, z]"))
			.Required(TEXT("end"), TEXT("array"), TEXT("End position [x, y, z]"))
			.Optional(TEXT("max_loudness"), TEXT("number"), TEXT("Maximum acceptable footstep loudness 0-1"), TEXT("0.3"))
			.Build());

	// 11. suggest_audio_volumes
	Registry.RegisterAction(TEXT("mesh"), TEXT("suggest_audio_volumes"),
		TEXT("Given room geometry + surface materials, suggest AAudioVolume reverb settings based on RT60 + material classification."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAudioActions::SuggestAudioVolumes),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Volume to analyze"))
			.Build());

	// === Write (3) ===

	// 12. create_audio_volume
	Registry.RegisterAction(TEXT("mesh"), TEXT("create_audio_volume"),
		TEXT("Spawn an AAudioVolume matching a blocking volume's shape. Set reverb/interior settings. Undo transaction."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAudioActions::CreateAudioVolume),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Name of the blocking volume to match shape from"))
			.Optional(TEXT("reverb_preset"), TEXT("string"), TEXT("Reverb preset name"))
			.Optional(TEXT("priority"), TEXT("number"), TEXT("Audio volume priority"), TEXT("0"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Label for the new audio volume"))
			.Build());

	// 13. set_surface_type
	Registry.RegisterAction(TEXT("mesh"), TEXT("set_surface_type"),
		TEXT("Set physical material surface type override on a mesh actor's component. Undo transaction."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAudioActions::SetSurfaceType),
		FParamSchemaBuilder()
			.Required(TEXT("actor_name"), TEXT("string"), TEXT("Actor to modify"))
			.Required(TEXT("surface_type"), TEXT("string"), TEXT("Physical surface type name (e.g. Metal, Carpet, Wood)"))
			.Build());

	// 14. create_surface_datatable
	Registry.RegisterAction(TEXT("mesh"), TEXT("create_surface_datatable"),
		TEXT("Bootstrap the acoustic system: create a DataTable with surface properties and register surface types via UPhysicsSettings CDO."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAudioActions::CreateSurfaceDataTable),
		FParamSchemaBuilder()
			.Optional(TEXT("template"), TEXT("string"), TEXT("Template to use: 'horror_default'"), TEXT("horror_default"))
			.Optional(TEXT("save_path"), TEXT("string"), TEXT("Asset path for the DataTable (default from settings)"))
			.Build());
}

// ============================================================================
// 1. get_audio_volumes
// ============================================================================

FMonolithActionResult FMonolithMeshAudioActions::GetAudioVolumes(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	bool bIncludeDetails = true;
	Params->TryGetBoolField(TEXT("include_details"), bIncludeDetails);

	TArray<TSharedPtr<FJsonValue>> VolumesArr;

	for (TActorIterator<AAudioVolume> It(World); It; ++It)
	{
		AAudioVolume* AV = *It;
		auto VolObj = MakeShared<FJsonObject>();

		VolObj->SetStringField(TEXT("name"), AV->GetActorNameOrLabel());
		VolObj->SetBoolField(TEXT("enabled"), AV->GetEnabled());
		VolObj->SetNumberField(TEXT("priority"), AV->GetPriority());

		// Bounds
		FVector Origin, Extent;
		AV->GetActorBounds(false, Origin, Extent);
		VolObj->SetArrayField(TEXT("center"), MAud_VecToArr(Origin));
		VolObj->SetArrayField(TEXT("extent"), MAud_VecToArr(Extent * 2.0));

		if (bIncludeDetails)
		{
			// Reverb settings
			const FReverbSettings& Reverb = AV->GetReverbSettings();
			auto RevObj = MakeShared<FJsonObject>();
			RevObj->SetBoolField(TEXT("activate"), Reverb.bApplyReverb);
			RevObj->SetNumberField(TEXT("volume"), Reverb.Volume);
			RevObj->SetNumberField(TEXT("fade_time"), Reverb.FadeTime);
			VolObj->SetObjectField(TEXT("reverb"), RevObj);

			// Interior settings
			const FInteriorSettings& Interior = AV->GetInteriorSettings();
			auto IntObj = MakeShared<FJsonObject>();
			IntObj->SetNumberField(TEXT("exterior_volume"), Interior.ExteriorVolume);
			IntObj->SetNumberField(TEXT("exterior_time"), Interior.ExteriorTime);
			IntObj->SetNumberField(TEXT("interior_volume"), Interior.InteriorVolume);
			IntObj->SetNumberField(TEXT("interior_time"), Interior.InteriorTime);
			VolObj->SetObjectField(TEXT("interior"), IntObj);
		}

		VolumesArr.Add(MakeShared<FJsonValueObject>(VolObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("audio_volumes"), VolumesArr);
	Result->SetNumberField(TEXT("count"), VolumesArr.Num());

	// Flag: check if scene has uncovered regions
	if (VolumesArr.Num() == 0)
	{
		Result->SetStringField(TEXT("warning"), TEXT("No audio volumes in scene. All areas use global reverb defaults."));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 2. get_surface_materials
// ============================================================================

FMonolithActionResult FMonolithMeshAudioActions::GetSurfaceMaterials(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Determine scan region
	FVector Origin, Extent;
	FString VolumeName;
	bool bHasRegion = false;

	if (Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		FString Error;
		if (!GetVolumeBounds(VolumeName, Origin, Extent, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
		bHasRegion = true;
	}
	else
	{
		FVector RegionMin, RegionMax;
		if (MonolithMeshUtils::ParseVector(Params, TEXT("region_min"), RegionMin)
			&& MonolithMeshUtils::ParseVector(Params, TEXT("region_max"), RegionMax))
		{
			Origin = (RegionMin + RegionMax) * 0.5f;
			Extent = (RegionMax - RegionMin) * 0.5f;
			bHasRegion = true;
		}
	}

	if (!bHasRegion)
	{
		return FMonolithActionResult::Error(TEXT("Specify volume_name or region_min/region_max"));
	}

	int32 RayCount = 64;
	TryGetInt(Params, TEXT("ray_count"), RayCount);
	RayCount = FMath::Clamp(RayCount, 8, 512);

	FCollisionQueryParams QP(SCENE_QUERY_STAT(MonolithSurfaceMaterials), true);
	QP.bReturnPhysicalMaterial = true;

	// Sample from multiple origins within the volume
	TMap<FString, int32> MaterialCounts;
	TMap<FString, MonolithMeshAcoustics::FAcousticProperties> MaterialProps;
	int32 TotalHits = 0;

	// 5 sample origins: center + 4 quadrants
	TArray<FVector> Origins;
	Origins.Add(Origin);
	Origins.Add(Origin + FVector(Extent.X * 0.5f, Extent.Y * 0.5f, 0));
	Origins.Add(Origin + FVector(-Extent.X * 0.5f, Extent.Y * 0.5f, 0));
	Origins.Add(Origin + FVector(Extent.X * 0.5f, -Extent.Y * 0.5f, 0));
	Origins.Add(Origin + FVector(-Extent.X * 0.5f, -Extent.Y * 0.5f, 0));

	const float MaxDist = Extent.Size();
	const int32 RaysPerOrigin = FMath::Max(RayCount / Origins.Num(), 4);

	for (const FVector& SampleOrigin : Origins)
	{
		for (int32 i = 0; i < RaysPerOrigin; ++i)
		{
			// Uniform sphere distribution
			float Theta = FMath::DegreesToRadians(360.0f * i / RaysPerOrigin);
			float Phi = FMath::Acos(1.0f - 2.0f * (float(i) + 0.5f) / RaysPerOrigin);
			FVector Dir(
				FMath::Sin(Phi) * FMath::Cos(Theta),
				FMath::Sin(Phi) * FMath::Sin(Theta),
				FMath::Cos(Phi)
			);

			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, SampleOrigin, SampleOrigin + Dir * MaxDist, ECC_Visibility, QP))
			{
				EPhysicalSurface Surface = SurfaceType_Default;
				if (Hit.PhysMaterial.IsValid())
				{
					Surface = Hit.PhysMaterial->SurfaceType;
				}

				auto Props = MonolithMeshAcoustics::GetPropertiesForSurface(Surface);
				MaterialCounts.FindOrAdd(Props.SurfaceName)++;
				MaterialProps.FindOrAdd(Props.SurfaceName) = Props;
				TotalHits++;
			}
		}
	}

	// Build result
	TArray<TSharedPtr<FJsonValue>> MaterialsArr;
	for (const auto& Pair : MaterialCounts)
	{
		auto MatObj = MakeShared<FJsonObject>();
		MatObj->SetStringField(TEXT("surface"), Pair.Key);
		MatObj->SetNumberField(TEXT("hit_count"), Pair.Value);
		MatObj->SetNumberField(TEXT("fraction"), TotalHits > 0 ? (float)Pair.Value / TotalHits : 0.0f);

		if (const auto* Props = MaterialProps.Find(Pair.Key))
		{
			MatObj->SetObjectField(TEXT("acoustic_properties"), AcousticPropsToJson(*Props));
		}

		MaterialsArr.Add(MakeShared<FJsonValueObject>(MatObj));
	}

	// Sort by hit count descending
	MaterialsArr.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		return A->AsObject()->GetNumberField(TEXT("hit_count")) > B->AsObject()->GetNumberField(TEXT("hit_count"));
	});

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("materials"), MaterialsArr);
	Result->SetNumberField(TEXT("total_hits"), TotalHits);
	Result->SetNumberField(TEXT("total_rays"), RayCount);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 3. estimate_footstep_sound
// ============================================================================

FMonolithActionResult FMonolithMeshAudioActions::EstimateFootstepSound(const TSharedPtr<FJsonObject>& Params)
{
	FVector Location;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: location (array of 3 numbers)"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FHitResult Hit;
	EPhysicalSurface Surface = TraceFloorSurface(World, Location, Hit);
	auto Props = MonolithMeshAcoustics::GetPropertiesForSurface(Surface);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("surface_type"), Props.SurfaceName);
	Result->SetNumberField(TEXT("footstep_loudness"), Props.FootstepLoudness);
	Result->SetNumberField(TEXT("absorption"), Props.Absorption);

	// Loudness classification
	FString LoudnessClass;
	if (Props.FootstepLoudness < 0.15f) LoudnessClass = TEXT("silent");
	else if (Props.FootstepLoudness < 0.3f) LoudnessClass = TEXT("quiet");
	else if (Props.FootstepLoudness < 0.5f) LoudnessClass = TEXT("moderate");
	else if (Props.FootstepLoudness < 0.7f) LoudnessClass = TEXT("loud");
	else LoudnessClass = TEXT("very_loud");
	Result->SetStringField(TEXT("loudness_class"), LoudnessClass);

	if (Hit.bBlockingHit)
	{
		Result->SetArrayField(TEXT("floor_hit"), MAud_VecToArr(Hit.ImpactPoint));
		if (Hit.GetActor())
		{
			Result->SetStringField(TEXT("floor_actor"), Hit.GetActor()->GetActorNameOrLabel());
		}
	}
	else
	{
		Result->SetStringField(TEXT("warning"), TEXT("No floor detected below location (void)"));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 4. analyze_room_acoustics
// ============================================================================

FMonolithActionResult FMonolithMeshAudioActions::AnalyzeRoomAcoustics(const TSharedPtr<FJsonObject>& Params)
{
	FString VolumeName;
	if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: volume_name"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FString Error;
	FVector Origin, Extent;
	if (!GetVolumeBounds(VolumeName, Origin, Extent, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	int32 RayCount = 128;
	TryGetInt(Params, TEXT("ray_count"), RayCount);
	RayCount = FMath::Clamp(RayCount, 16, 1024);

	FCollisionQueryParams QP(SCENE_QUERY_STAT(MonolithRoomAcoustics), true);
	QP.bReturnPhysicalMaterial = true;

	// Approximate room volume in cm^3 (box approximation)
	float VolumeCm3 = Extent.X * Extent.Y * Extent.Z * 8.0f; // Full box = 2*ext per axis
	float VolumeM3 = CmCubedToMCubed(VolumeCm3);

	// Cast rays from center in all directions, collect surface samples
	TMap<FString, float> MaterialAreaM2; // material -> total area estimate in m^2
	float TotalSurfaceAreaM2 = 0.0f;
	float TotalAbsorption = 0.0f; // Sum(area * absorption)
	int32 HitCount = 0;

	// Estimate total interior surface area of the bounding box
	float BoxSurfaceAreaCm2 = 2.0f * (
		(Extent.X * 2.0f) * (Extent.Y * 2.0f) +
		(Extent.Y * 2.0f) * (Extent.Z * 2.0f) +
		(Extent.X * 2.0f) * (Extent.Z * 2.0f)
	);
	float BoxSurfaceAreaM2 = CmSquaredToMSquared(BoxSurfaceAreaCm2);

	TMap<FString, int32> MaterialHits;
	const float MaxDist = Extent.Size();

	for (int32 i = 0; i < RayCount; ++i)
	{
		// Fibonacci sphere for uniform distribution
		float GoldenRatio = (1.0f + FMath::Sqrt(5.0f)) / 2.0f;
		float Theta = 2.0f * PI * i / GoldenRatio;
		float Phi = FMath::Acos(1.0f - 2.0f * (float(i) + 0.5f) / RayCount);

		FVector Dir(
			FMath::Sin(Phi) * FMath::Cos(Theta),
			FMath::Sin(Phi) * FMath::Sin(Theta),
			FMath::Cos(Phi)
		);

		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, Origin, Origin + Dir * MaxDist, ECC_Visibility, QP))
		{
			EPhysicalSurface Surface = SurfaceType_Default;
			if (Hit.PhysMaterial.IsValid())
			{
				Surface = Hit.PhysMaterial->SurfaceType;
			}

			auto Props = MonolithMeshAcoustics::GetPropertiesForSurface(Surface);
			MaterialHits.FindOrAdd(Props.SurfaceName)++;
			HitCount++;
		}
	}

	// Distribute surface area proportionally by hit count
	for (const auto& Pair : MaterialHits)
	{
		float Fraction = (float)Pair.Value / FMath::Max(HitCount, 1);
		float AreaM2 = BoxSurfaceAreaM2 * Fraction;
		MaterialAreaM2.Add(Pair.Key, AreaM2);
		TotalSurfaceAreaM2 += AreaM2;

		auto Props = MonolithMeshAcoustics::GetPropertiesForName(Pair.Key);
		TotalAbsorption += AreaM2 * Props.Absorption;
	}

	// Sabine RT60
	float RT60 = MonolithMeshAcoustics::ComputeSabineRT60(VolumeM3, TotalAbsorption);
	auto AcousticType = MonolithMeshAcoustics::ClassifyRT60(RT60);

	// Average absorption coefficient
	float AvgAbsorption = TotalSurfaceAreaM2 > 0 ? TotalAbsorption / TotalSurfaceAreaM2 : 0.02f;

	// Build result
	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("rt60_seconds"), RT60);
	Result->SetStringField(TEXT("classification"), MonolithMeshAcoustics::AcousticTypeToString(AcousticType));
	Result->SetNumberField(TEXT("volume_m3"), VolumeM3);
	Result->SetNumberField(TEXT("surface_area_m2"), TotalSurfaceAreaM2);
	Result->SetNumberField(TEXT("total_absorption_sabins"), TotalAbsorption);
	Result->SetNumberField(TEXT("average_absorption"), AvgAbsorption);

	// Material breakdown
	TArray<TSharedPtr<FJsonValue>> MaterialsArr;
	TMap<FString, float> MaterialFractions;
	for (const auto& Pair : MaterialHits)
	{
		float Fraction = (float)Pair.Value / FMath::Max(HitCount, 1);
		MaterialFractions.Add(Pair.Key, Fraction);

		auto MatObj = MakeShared<FJsonObject>();
		MatObj->SetStringField(TEXT("surface"), Pair.Key);
		MatObj->SetNumberField(TEXT("fraction"), Fraction);
		MatObj->SetNumberField(TEXT("area_m2"), MaterialAreaM2.FindRef(Pair.Key));

		auto Props = MonolithMeshAcoustics::GetPropertiesForName(Pair.Key);
		MatObj->SetNumberField(TEXT("absorption"), Props.Absorption);

		MaterialsArr.Add(MakeShared<FJsonValueObject>(MatObj));
	}
	Result->SetArrayField(TEXT("materials"), MaterialsArr);

	// Reverb suggestion
	auto Suggestion = MonolithMeshAcoustics::SuggestReverbSettings(RT60, MaterialFractions);
	auto SugObj = MakeShared<FJsonObject>();
	SugObj->SetNumberField(TEXT("reverb_volume"), Suggestion.Volume);
	SugObj->SetNumberField(TEXT("decay_time"), Suggestion.DecayTime);
	SugObj->SetNumberField(TEXT("density"), Suggestion.Density);
	SugObj->SetNumberField(TEXT("diffusion"), Suggestion.Diffusion);
	SugObj->SetNumberField(TEXT("air_absorption_hf"), Suggestion.AirAbsorptionHF);
	SugObj->SetStringField(TEXT("notes"), Suggestion.Notes);
	Result->SetObjectField(TEXT("reverb_suggestion"), SugObj);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 5. analyze_sound_propagation
// ============================================================================

FMonolithActionResult FMonolithMeshAudioActions::AnalyzeSoundPropagation(const TSharedPtr<FJsonObject>& Params)
{
	FVector From, To;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("from"), From))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: from"));
	}
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("to"), To))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: to"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	bool bIncludeOcclusion = true;
	Params->TryGetBoolField(TEXT("include_occlusion"), bIncludeOcclusion);

	float Distance = FVector::Dist(From, To);
	float DistAtten = MonolithMeshAcoustics::ComputeDistanceAttenuation(Distance);

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("distance_cm"), Distance);
	Result->SetNumberField(TEXT("distance_attenuation"), DistAtten);

	if (bIncludeOcclusion)
	{
		int32 WallCount;
		float TotalLossdB;
		float OcclusionFactor = MonolithMeshAcoustics::TraceOcclusion(World, From, To, WallCount, TotalLossdB);

		float DirectCombinedFactor = DistAtten * OcclusionFactor;

		// Direct path sub-object
		auto DirectPathObj = MakeShared<FJsonObject>();
		DirectPathObj->SetNumberField(TEXT("wall_count"), WallCount);
		DirectPathObj->SetNumberField(TEXT("total_transmission_loss_db"), TotalLossdB);
		DirectPathObj->SetNumberField(TEXT("occlusion_factor"), OcclusionFactor);
		DirectPathObj->SetNumberField(TEXT("combined_factor"), DirectCombinedFactor);

		// Classify direct path audibility
		FString DirectAudibility;
		if (DirectCombinedFactor > 0.1f)
		{
			DirectAudibility = TEXT("yes");
		}
		else if (DirectCombinedFactor > 0.01f)
		{
			DirectAudibility = TEXT("faintly");
		}
		else
		{
			DirectAudibility = TEXT("no");
		}
		DirectPathObj->SetStringField(TEXT("can_hear"), DirectAudibility);
		Result->SetObjectField(TEXT("direct_path"), DirectPathObj);

		// Also set top-level fields for backward compatibility
		Result->SetNumberField(TEXT("wall_count"), WallCount);
		Result->SetNumberField(TEXT("total_transmission_loss_db"), TotalLossdB);
		Result->SetNumberField(TEXT("occlusion_factor"), OcclusionFactor);
		Result->SetNumberField(TEXT("combined_factor"), DirectCombinedFactor);

		// --- Indirect navmesh path (doorway propagation) ---
		float BestFactor = DirectCombinedFactor;
		FString BestPath = TEXT("direct");
		FString BestAudibility = DirectAudibility;

		auto IndirectResult = MonolithMeshAcoustics::FindIndirectNavmeshPath(World, From, To);

		if (IndirectResult.bFound)
		{
			float IndirectFactor = IndirectResult.AttenuationFactor;

			auto IndirectPathObj = MakeShared<FJsonObject>();
			IndirectPathObj->SetStringField(TEXT("via"), TEXT("navmesh"));
			IndirectPathObj->SetNumberField(TEXT("distance_cm"), IndirectResult.PathDistance);
			IndirectPathObj->SetNumberField(TEXT("attenuation"), IndirectFactor);
			IndirectPathObj->SetNumberField(TEXT("segment_count"), FMath::Max(0, IndirectResult.PathPoints.Num() - 1));

			FString IndirectAudibility;
			if (IndirectFactor > 0.1f)
			{
				IndirectAudibility = TEXT("yes");
			}
			else if (IndirectFactor > 0.01f)
			{
				IndirectAudibility = TEXT("faintly");
			}
			else
			{
				IndirectAudibility = TEXT("no");
			}
			IndirectPathObj->SetStringField(TEXT("can_hear"), IndirectAudibility);

			Result->SetObjectField(TEXT("indirect_path"), IndirectPathObj);

			// Pick the better path
			if (IndirectFactor > DirectCombinedFactor)
			{
				BestFactor = IndirectFactor;
				BestPath = TEXT("indirect");
				BestAudibility = IndirectAudibility;
			}
		}
		else if (!IndirectResult.bNavmeshAvailable)
		{
			auto IndirectPathObj = MakeShared<FJsonObject>();
			IndirectPathObj->SetStringField(TEXT("note"), IndirectResult.Note);
			Result->SetObjectField(TEXT("indirect_path"), IndirectPathObj);
		}

		Result->SetStringField(TEXT("best_path"), BestPath);
		Result->SetStringField(TEXT("can_be_heard"), BestAudibility);

		// Estimated dB at listener using best path
		float EstimatedReductionDb = 0.0f;
		if (BestFactor > SMALL_NUMBER)
		{
			EstimatedReductionDb = -20.0f * FMath::LogX(10.0f, BestFactor);
		}
		else
		{
			EstimatedReductionDb = 120.0f; // Effectively inaudible
		}
		Result->SetNumberField(TEXT("estimated_reduction_db"), EstimatedReductionDb);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 6. find_loud_surfaces
// ============================================================================

FMonolithActionResult FMonolithMeshAudioActions::FindLoudSurfaces(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FVector Origin, Extent;
	FString VolumeName;
	bool bHasRegion = false;

	if (Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		FString Error;
		if (!GetVolumeBounds(VolumeName, Origin, Extent, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
		bHasRegion = true;
	}
	else
	{
		FVector RegionMin, RegionMax;
		if (MonolithMeshUtils::ParseVector(Params, TEXT("region_min"), RegionMin)
			&& MonolithMeshUtils::ParseVector(Params, TEXT("region_max"), RegionMax))
		{
			Origin = (RegionMin + RegionMax) * 0.5f;
			Extent = (RegionMax - RegionMin) * 0.5f;
			bHasRegion = true;
		}
	}

	if (!bHasRegion)
	{
		return FMonolithActionResult::Error(TEXT("Specify volume_name or region_min/region_max"));
	}

	float LoudnessThreshold = 0.5f;
	TryGetFloat(Params, TEXT("loudness_threshold"), LoudnessThreshold);

	// Grid-sample the floor within the region
	const float GridStep = 100.0f; // 1m grid
	FVector RegionMin = Origin - Extent;
	FVector RegionMax = Origin + Extent;

	struct FLoudSpot
	{
		FVector Location;
		FString Surface;
		float Loudness;
	};
	TArray<FLoudSpot> LoudSpots;

	FCollisionQueryParams QP(SCENE_QUERY_STAT(MonolithLoudSurface), true);
	QP.bReturnPhysicalMaterial = true;

	for (float X = RegionMin.X; X <= RegionMax.X; X += GridStep)
	{
		for (float Y = RegionMin.Y; Y <= RegionMax.Y; Y += GridStep)
		{
			FVector TraceStart(X, Y, RegionMax.Z);
			FVector TraceEnd(X, Y, RegionMin.Z);

			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, QP))
			{
				EPhysicalSurface Surface = SurfaceType_Default;
				if (Hit.PhysMaterial.IsValid())
				{
					Surface = Hit.PhysMaterial->SurfaceType;
				}

				auto Props = MonolithMeshAcoustics::GetPropertiesForSurface(Surface);
				if (Props.FootstepLoudness >= LoudnessThreshold)
				{
					FLoudSpot Spot;
					Spot.Location = Hit.ImpactPoint;
					Spot.Surface = Props.SurfaceName;
					Spot.Loudness = Props.FootstepLoudness;
					LoudSpots.Add(MoveTemp(Spot));
				}
			}
		}
	}

	// Cluster nearby loud spots and compute approximate areas
	TArray<TSharedPtr<FJsonValue>> SpotsArr;
	for (const FLoudSpot& Spot : LoudSpots)
	{
		auto SpotObj = MakeShared<FJsonObject>();
		SpotObj->SetArrayField(TEXT("location"), MAud_VecToArr(Spot.Location));
		SpotObj->SetStringField(TEXT("surface"), Spot.Surface);
		SpotObj->SetNumberField(TEXT("loudness"), Spot.Loudness);
		// Detection radius estimate: loudness * base_hearing_range
		SpotObj->SetNumberField(TEXT("estimated_detection_radius_cm"), Spot.Loudness * 2000.0f);
		SpotsArr.Add(MakeShared<FJsonValueObject>(SpotObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("loud_spots"), SpotsArr);
	Result->SetNumberField(TEXT("count"), LoudSpots.Num());
	Result->SetNumberField(TEXT("loudness_threshold"), LoudnessThreshold);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 7. find_sound_paths
// ============================================================================

FMonolithActionResult FMonolithMeshAudioActions::FindSoundPaths(const TSharedPtr<FJsonObject>& Params)
{
	FVector From, To;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("from"), From))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: from"));
	}
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("to"), To))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: to"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	int32 MaxBounces = 2;
	TryGetInt(Params, TEXT("max_bounces"), MaxBounces);

	int32 CandidateSurfaces = 16;
	TryGetInt(Params, TEXT("candidate_surfaces"), CandidateSurfaces);

	auto SoundPaths = MonolithMeshAcoustics::FindSoundPaths(World, From, To, MaxBounces, CandidateSurfaces);

	TArray<TSharedPtr<FJsonValue>> PathsArr;
	for (const auto& Path : SoundPaths)
	{
		auto PathObj = MakeShared<FJsonObject>();
		PathObj->SetBoolField(TEXT("direct"), Path.bDirect);
		PathObj->SetNumberField(TEXT("bounce_count"), Path.BounceCount);
		PathObj->SetNumberField(TEXT("total_distance_cm"), Path.TotalDistance);
		PathObj->SetNumberField(TEXT("attenuation_factor"), Path.AttenuationFactor);

		TArray<TSharedPtr<FJsonValue>> PointsArr;
		for (const FVector& Pt : Path.Points)
		{
			PointsArr.Add(MakeShared<FJsonValueArray>(MAud_VecToArr(Pt)));
		}
		PathObj->SetArrayField(TEXT("points"), PointsArr);

		if (Path.WallMaterials.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> MatArr;
			for (const FString& Mat : Path.WallMaterials)
			{
				MatArr.Add(MakeShared<FJsonValueString>(Mat));
			}
			PathObj->SetArrayField(TEXT("reflection_materials"), MatArr);
		}

		PathsArr.Add(MakeShared<FJsonValueObject>(PathObj));
	}

	// === Navmesh indirect path (doorway propagation) ===
	auto IndirectResult = MonolithMeshAcoustics::FindIndirectNavmeshPath(World, From, To);
	if (IndirectResult.bFound)
	{
		auto IndirectObj = MakeShared<FJsonObject>();
		IndirectObj->SetBoolField(TEXT("direct"), false);
		IndirectObj->SetNumberField(TEXT("bounce_count"), 0);
		IndirectObj->SetNumberField(TEXT("total_distance_cm"), IndirectResult.PathDistance);
		IndirectObj->SetNumberField(TEXT("attenuation_factor"), IndirectResult.AttenuationFactor);
		IndirectObj->SetStringField(TEXT("type"), TEXT("navmesh_indirect"));

		TArray<TSharedPtr<FJsonValue>> IndirectPointsArr;
		for (const FVector& Pt : IndirectResult.PathPoints)
		{
			IndirectPointsArr.Add(MakeShared<FJsonValueArray>(MAud_VecToArr(Pt)));
		}
		IndirectObj->SetArrayField(TEXT("points"), IndirectPointsArr);

		// Insert in sorted position (by attenuation, strongest first)
		bool bInserted = false;
		for (int32 i = 0; i < PathsArr.Num(); ++i)
		{
			const TSharedPtr<FJsonObject>* ExistingObj;
			if (PathsArr[i]->TryGetObject(ExistingObj))
			{
				double ExistingAtten = 0.0;
				(*ExistingObj)->TryGetNumberField(TEXT("attenuation_factor"), ExistingAtten);
				if (IndirectResult.AttenuationFactor > ExistingAtten)
				{
					PathsArr.Insert(MakeShared<FJsonValueObject>(IndirectObj), i);
					bInserted = true;
					break;
				}
			}
		}
		if (!bInserted)
		{
			PathsArr.Add(MakeShared<FJsonValueObject>(IndirectObj));
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("paths"), PathsArr);
	Result->SetNumberField(TEXT("path_count"), PathsArr.Num());

	if (!IndirectResult.bNavmeshAvailable && !IndirectResult.bFound)
	{
		Result->SetStringField(TEXT("navmesh_note"), IndirectResult.Note);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 8. can_ai_hear_from
// ============================================================================

FMonolithActionResult FMonolithMeshAudioActions::CanAiHearFrom(const TSharedPtr<FJsonObject>& Params)
{
	FVector AiLocation, PlayerLocation;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("ai_location"), AiLocation))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: ai_location"));
	}
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("player_location"), PlayerLocation))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: player_location"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	float AiHearingRange = 2000.0f;
	TryGetFloat(Params, TEXT("ai_hearing_range"), AiHearingRange);

	// Determine floor surface at player position
	FString SurfaceOverride;
	MonolithMeshAcoustics::FAcousticProperties FloorProps;

	if (Params->TryGetStringField(TEXT("surface_type"), SurfaceOverride) && !SurfaceOverride.IsEmpty())
	{
		FloorProps = MonolithMeshAcoustics::GetPropertiesForName(SurfaceOverride);
	}
	else
	{
		FHitResult Hit;
		EPhysicalSurface Surface = TraceFloorSurface(World, PlayerLocation, Hit);
		FloorProps = MonolithMeshAcoustics::GetPropertiesForSurface(Surface);
	}

	// Compute detection
	float DirectDistance = FVector::Dist(AiLocation, PlayerLocation);

	// Effective sound radius = hearing_range * footstep_loudness
	float EffectiveRadius = AiHearingRange * FloorProps.FootstepLoudness;

	// === Direct path: wall occlusion ===
	int32 WallCount;
	float TotalLossdB;
	float OcclusionFactor = MonolithMeshAcoustics::TraceOcclusion(World, PlayerLocation, AiLocation, WallCount, TotalLossdB);

	float DirectEffectiveRadius = EffectiveRadius * OcclusionFactor;

	FString DirectAudibility;
	if (DirectDistance <= DirectEffectiveRadius)
	{
		DirectAudibility = TEXT("yes");
	}
	else if (DirectDistance <= DirectEffectiveRadius * 1.5f)
	{
		DirectAudibility = TEXT("faintly");
	}
	else
	{
		DirectAudibility = TEXT("no");
	}

	// === Indirect path: navmesh doorway propagation ===
	FString IndirectAudibility = TEXT("no");
	float IndirectDistance = 0.0f;
	float IndirectAttenuation = 0.0f;
	bool bIndirectFound = false;
	FString IndirectNote;

	auto IndirectResult = MonolithMeshAcoustics::FindIndirectNavmeshPath(World, PlayerLocation, AiLocation);
	if (IndirectResult.bFound)
	{
		bIndirectFound = true;
		IndirectDistance = IndirectResult.PathDistance;
		IndirectAttenuation = IndirectResult.AttenuationFactor;

		// For indirect path: effective radius uses distance-only attenuation (no wall occlusion)
		// Check if the AI is within hearing range along the navmesh path distance
		if (IndirectDistance <= EffectiveRadius)
		{
			IndirectAudibility = TEXT("yes");
		}
		else if (IndirectDistance <= EffectiveRadius * 1.5f)
		{
			IndirectAudibility = TEXT("faintly");
		}
		else
		{
			IndirectAudibility = TEXT("no");
		}
	}
	else
	{
		IndirectNote = IndirectResult.Note;
	}

	// === Pick the best path ===
	// Rank: yes > faintly > no
	auto AudibilityRank = [](const FString& A) -> int32
	{
		if (A == TEXT("yes")) return 2;
		if (A == TEXT("faintly")) return 1;
		return 0;
	};

	FString BestPath = TEXT("direct");
	FString BestAudibility = DirectAudibility;
	float BestEffectiveRadius = DirectEffectiveRadius;

	if (bIndirectFound && AudibilityRank(IndirectAudibility) > AudibilityRank(DirectAudibility))
	{
		BestPath = TEXT("indirect");
		BestAudibility = IndirectAudibility;
		// For indirect, effective radius is just the base radius (no wall penalty)
		BestEffectiveRadius = EffectiveRadius;
	}

	auto Result = MakeShared<FJsonObject>();

	// Direct path details
	auto DirectPathObj = MakeShared<FJsonObject>();
	DirectPathObj->SetNumberField(TEXT("wall_count"), WallCount);
	DirectPathObj->SetNumberField(TEXT("occlusion_factor"), OcclusionFactor);
	DirectPathObj->SetStringField(TEXT("can_hear"), DirectAudibility);
	DirectPathObj->SetNumberField(TEXT("effective_radius_cm"), DirectEffectiveRadius);
	Result->SetObjectField(TEXT("direct_path"), DirectPathObj);

	// Indirect path details
	if (bIndirectFound)
	{
		auto IndirectPathObj = MakeShared<FJsonObject>();
		IndirectPathObj->SetStringField(TEXT("via"), TEXT("navmesh"));
		IndirectPathObj->SetNumberField(TEXT("distance_cm"), IndirectDistance);
		IndirectPathObj->SetNumberField(TEXT("attenuation"), IndirectAttenuation);
		IndirectPathObj->SetStringField(TEXT("can_hear"), IndirectAudibility);
		Result->SetObjectField(TEXT("indirect_path"), IndirectPathObj);
	}
	else if (!IndirectResult.bNavmeshAvailable)
	{
		auto IndirectPathObj = MakeShared<FJsonObject>();
		IndirectPathObj->SetStringField(TEXT("note"), IndirectNote);
		Result->SetObjectField(TEXT("indirect_path"), IndirectPathObj);
	}

	// Best path selection
	Result->SetStringField(TEXT("best_path"), BestPath);
	Result->SetStringField(TEXT("can_hear"), BestAudibility);
	Result->SetStringField(TEXT("floor_surface"), FloorProps.SurfaceName);
	Result->SetNumberField(TEXT("footstep_loudness"), FloorProps.FootstepLoudness);
	Result->SetNumberField(TEXT("distance_cm"), DirectDistance);
	Result->SetNumberField(TEXT("effective_hearing_radius_cm"), BestEffectiveRadius);
	Result->SetNumberField(TEXT("detection_radius_cm"), BestEffectiveRadius);

	// Backward compat fields
	Result->SetNumberField(TEXT("wall_count"), WallCount);
	Result->SetNumberField(TEXT("occlusion_factor"), OcclusionFactor);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 9. get_stealth_map
// ============================================================================

FMonolithActionResult FMonolithMeshAudioActions::GetStealthMap(const TSharedPtr<FJsonObject>& Params)
{
	FString VolumeName;
	if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: volume_name"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FString Error;
	FVector Origin, Extent;
	if (!GetVolumeBounds(VolumeName, Origin, Extent, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	float GridSize = 100.0f;
	TryGetFloat(Params, TEXT("grid_size"), GridSize);
	GridSize = FMath::Max(GridSize, 25.0f);

	float AiHearingRange = 2000.0f;
	TryGetFloat(Params, TEXT("ai_hearing_range"), AiHearingRange);

	FVector RegionMin = Origin - Extent;
	FVector RegionMax = Origin + Extent;

	FCollisionQueryParams QP(SCENE_QUERY_STAT(MonolithStealthMap), true);
	QP.bReturnPhysicalMaterial = true;

	TArray<TSharedPtr<FJsonValue>> CellsArr;
	int32 CellCount = 0;

	// Limit total cells to prevent explosion
	const int32 MaxCells = 2500;
	int32 XSteps = FMath::CeilToInt32((RegionMax.X - RegionMin.X) / GridSize);
	int32 YSteps = FMath::CeilToInt32((RegionMax.Y - RegionMin.Y) / GridSize);
	if (XSteps * YSteps > MaxCells)
	{
		GridSize = FMath::Sqrt((RegionMax.X - RegionMin.X) * (RegionMax.Y - RegionMin.Y) / MaxCells);
		XSteps = FMath::CeilToInt32((RegionMax.X - RegionMin.X) / GridSize);
		YSteps = FMath::CeilToInt32((RegionMax.Y - RegionMin.Y) / GridSize);
	}

	for (int32 XI = 0; XI <= XSteps; ++XI)
	{
		float X = RegionMin.X + XI * GridSize;
		for (int32 YI = 0; YI <= YSteps; ++YI)
		{
			float Y = RegionMin.Y + YI * GridSize;
			FVector TraceStart(X, Y, RegionMax.Z);
			FVector TraceEnd(X, Y, RegionMin.Z);

			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, QP))
			{
				EPhysicalSurface Surface = SurfaceType_Default;
				if (Hit.PhysMaterial.IsValid())
				{
					Surface = Hit.PhysMaterial->SurfaceType;
				}

				auto Props = MonolithMeshAcoustics::GetPropertiesForSurface(Surface);
				float DetectionRadius = AiHearingRange * Props.FootstepLoudness;

				auto CellObj = MakeShared<FJsonObject>();
				CellObj->SetArrayField(TEXT("position"), MAud_VecToArr(FVector(X, Y, Hit.ImpactPoint.Z)));
				CellObj->SetStringField(TEXT("surface"), Props.SurfaceName);
				CellObj->SetNumberField(TEXT("loudness"), Props.FootstepLoudness);
				CellObj->SetNumberField(TEXT("detection_radius_cm"), DetectionRadius);

				CellsArr.Add(MakeShared<FJsonValueObject>(CellObj));
				CellCount++;
			}
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("cells"), CellsArr);
	Result->SetNumberField(TEXT("cell_count"), CellCount);
	Result->SetNumberField(TEXT("grid_size_cm"), GridSize);
	Result->SetStringField(TEXT("volume"), VolumeName);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 10. find_quiet_path
// ============================================================================

FMonolithActionResult FMonolithMeshAudioActions::FindQuietPath(const TSharedPtr<FJsonObject>& Params)
{
	FVector Start, End;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("start"), Start))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: start"));
	}
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("end"), End))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: end"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	float MaxLoudness = 0.3f;
	TryGetFloat(Params, TEXT("max_loudness"), MaxLoudness);

	// Find the main navmesh path
	TArray<FVector> MainPath;
	float MainDist;
	if (!MonolithMeshAnalysis::FindNavPath(World, Start, End, MainPath, MainDist))
	{
		return FMonolithActionResult::Error(TEXT("No navmesh path found between start and end"));
	}

	FCollisionQueryParams QP(SCENE_QUERY_STAT(MonolithQuietPath), true);
	QP.bReturnPhysicalMaterial = true;

	// Score the main path by sampling floor surfaces along it
	auto ScorePath = [&](const TArray<FVector>& Path) -> float
	{
		if (Path.Num() == 0) return 1.0f;

		float TotalLoudness = 0.0f;
		int32 Samples = 0;

		// Sample every ~100cm along the path
		for (int32 i = 0; i < Path.Num(); ++i)
		{
			FHitResult Hit;
			FVector TraceStart = Path[i] + FVector(0, 0, 50);
			FVector TraceEnd = Path[i] - FVector(0, 0, 200);

			if (World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, QP))
			{
				EPhysicalSurface Surface = SurfaceType_Default;
				if (Hit.PhysMaterial.IsValid())
				{
					Surface = Hit.PhysMaterial->SurfaceType;
				}
				auto Props = MonolithMeshAcoustics::GetPropertiesForSurface(Surface);
				TotalLoudness += Props.FootstepLoudness;
				Samples++;
			}
		}

		return Samples > 0 ? TotalLoudness / Samples : 1.0f;
	};

	float MainScore = ScorePath(MainPath);

	// Build path result with per-point surface data
	TArray<TSharedPtr<FJsonValue>> PathPointsArr;
	for (const FVector& Pt : MainPath)
	{
		auto PtObj = MakeShared<FJsonObject>();
		PtObj->SetArrayField(TEXT("position"), MAud_VecToArr(Pt));

		FHitResult Hit;
		FVector TraceStart = Pt + FVector(0, 0, 50);
		FVector TraceEnd = Pt - FVector(0, 0, 200);
		if (World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, QP))
		{
			EPhysicalSurface Surface = SurfaceType_Default;
			if (Hit.PhysMaterial.IsValid())
			{
				Surface = Hit.PhysMaterial->SurfaceType;
			}
			auto Props = MonolithMeshAcoustics::GetPropertiesForSurface(Surface);
			PtObj->SetStringField(TEXT("surface"), Props.SurfaceName);
			PtObj->SetNumberField(TEXT("loudness"), Props.FootstepLoudness);
		}

		PathPointsArr.Add(MakeShared<FJsonValueObject>(PtObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("average_loudness"), MainScore);
	Result->SetBoolField(TEXT("meets_threshold"), MainScore <= MaxLoudness);
	Result->SetNumberField(TEXT("max_loudness_threshold"), MaxLoudness);
	Result->SetNumberField(TEXT("path_distance_cm"), MainDist);
	Result->SetArrayField(TEXT("path_points"), PathPointsArr);

	if (MainScore > MaxLoudness)
	{
		Result->SetStringField(TEXT("warning"),
			FString::Printf(TEXT("Path average loudness %.2f exceeds threshold %.2f. No quieter alternative found on navmesh."),
				MainScore, MaxLoudness));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 11. suggest_audio_volumes
// ============================================================================

FMonolithActionResult FMonolithMeshAudioActions::SuggestAudioVolumes(const TSharedPtr<FJsonObject>& Params)
{
	FString VolumeName;
	if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: volume_name"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FString Error;
	FVector Origin, Extent;
	if (!GetVolumeBounds(VolumeName, Origin, Extent, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	// Compute room acoustics inline (reusing analyze logic)
	float VolumeCm3 = Extent.X * Extent.Y * Extent.Z * 8.0f;
	float VolumeM3 = CmCubedToMCubed(VolumeCm3);

	float BoxSurfaceAreaCm2 = 2.0f * (
		(Extent.X * 2.0f) * (Extent.Y * 2.0f) +
		(Extent.Y * 2.0f) * (Extent.Z * 2.0f) +
		(Extent.X * 2.0f) * (Extent.Z * 2.0f)
	);
	float BoxSurfaceAreaM2 = CmSquaredToMSquared(BoxSurfaceAreaCm2);

	FCollisionQueryParams QP(SCENE_QUERY_STAT(MonolithSuggestAudio), true);
	QP.bReturnPhysicalMaterial = true;

	TMap<FString, int32> MaterialHits;
	int32 HitCount = 0;
	const int32 RayCount = 96;
	const float MaxDist = Extent.Size();

	for (int32 i = 0; i < RayCount; ++i)
	{
		float GoldenRatio = (1.0f + FMath::Sqrt(5.0f)) / 2.0f;
		float Theta = 2.0f * PI * i / GoldenRatio;
		float Phi = FMath::Acos(1.0f - 2.0f * (float(i) + 0.5f) / RayCount);

		FVector Dir(
			FMath::Sin(Phi) * FMath::Cos(Theta),
			FMath::Sin(Phi) * FMath::Sin(Theta),
			FMath::Cos(Phi)
		);

		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, Origin, Origin + Dir * MaxDist, ECC_Visibility, QP))
		{
			EPhysicalSurface Surface = SurfaceType_Default;
			if (Hit.PhysMaterial.IsValid())
			{
				Surface = Hit.PhysMaterial->SurfaceType;
			}
			auto Props = MonolithMeshAcoustics::GetPropertiesForSurface(Surface);
			MaterialHits.FindOrAdd(Props.SurfaceName)++;
			HitCount++;
		}
	}

	// Compute absorption
	float TotalAbsorption = 0.0f;
	TMap<FString, float> MaterialFractions;
	for (const auto& Pair : MaterialHits)
	{
		float Fraction = (float)Pair.Value / FMath::Max(HitCount, 1);
		MaterialFractions.Add(Pair.Key, Fraction);
		float AreaM2 = BoxSurfaceAreaM2 * Fraction;
		auto Props = MonolithMeshAcoustics::GetPropertiesForName(Pair.Key);
		TotalAbsorption += AreaM2 * Props.Absorption;
	}

	float RT60 = MonolithMeshAcoustics::ComputeSabineRT60(VolumeM3, TotalAbsorption);
	auto Suggestion = MonolithMeshAcoustics::SuggestReverbSettings(RT60, MaterialFractions);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("volume_name"), VolumeName);
	Result->SetNumberField(TEXT("rt60_seconds"), RT60);
	Result->SetStringField(TEXT("classification"), Suggestion.Classification);

	auto SettingsObj = MakeShared<FJsonObject>();
	SettingsObj->SetNumberField(TEXT("reverb_volume"), Suggestion.Volume);
	SettingsObj->SetNumberField(TEXT("decay_time"), Suggestion.DecayTime);
	SettingsObj->SetNumberField(TEXT("density"), Suggestion.Density);
	SettingsObj->SetNumberField(TEXT("diffusion"), Suggestion.Diffusion);
	SettingsObj->SetNumberField(TEXT("air_absorption_hf"), Suggestion.AirAbsorptionHF);
	Result->SetObjectField(TEXT("suggested_settings"), SettingsObj);
	Result->SetStringField(TEXT("notes"), Suggestion.Notes);

	// Check if an audio volume already covers this space
	bool bHasExistingAudioVolume = false;
	for (TActorIterator<AAudioVolume> It(World); It; ++It)
	{
		if (It->EncompassesPoint(Origin))
		{
			bHasExistingAudioVolume = true;
			Result->SetStringField(TEXT("existing_audio_volume"), It->GetActorNameOrLabel());
			break;
		}
	}

	if (!bHasExistingAudioVolume)
	{
		Result->SetStringField(TEXT("recommendation"),
			TEXT("No audio volume covers this region. Use create_audio_volume to add one with these settings."));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 12. create_audio_volume
// ============================================================================

FMonolithActionResult FMonolithMeshAudioActions::CreateAudioVolume(const TSharedPtr<FJsonObject>& Params)
{
	FString VolumeName;
	if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: volume_name"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FString Error;
	FVector Origin, Extent;
	if (!GetVolumeBounds(VolumeName, Origin, Extent, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	float Priority = 0.0f;
	TryGetFloat(Params, TEXT("priority"), Priority);

	FString Label;
	if (!Params->TryGetStringField(TEXT("label"), Label))
	{
		Label = FString::Printf(TEXT("AudioVolume_%s"), *VolumeName);
	}

	FScopedAudioTransaction Transaction(FText::FromString(TEXT("Monolith: Create Audio Volume")));

	// Spawn the audio volume
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AAudioVolume* AudioVol = World->SpawnActor<AAudioVolume>(Origin, FRotator::ZeroRotator, SpawnParams);
	if (!AudioVol)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(TEXT("Failed to spawn AAudioVolume"));
	}

	AudioVol->SetActorLabel(Label);
	AudioVol->SetActorScale3D(Extent / 100.0f); // AudioVolume default brush is 200x200x200 (100 half-extent)
	AudioVol->SetPriority(Priority);
	AudioVol->SetEnabled(true);

	// Apply reverb settings if requested
	FString ReverbPreset;
	if (Params->TryGetStringField(TEXT("reverb_preset"), ReverbPreset))
	{
		// For now, just enable reverb. The preset would need to be a ReverbEffect asset.
		FReverbSettings ReverbSettings;
		ReverbSettings.bApplyReverb = true;
		ReverbSettings.Volume = 0.5f;
		ReverbSettings.FadeTime = 0.5f;
		AudioVol->SetReverbSettings(ReverbSettings);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), AudioVol->GetActorNameOrLabel());
	Result->SetArrayField(TEXT("location"), MAud_VecToArr(AudioVol->GetActorLocation()));
	Result->SetNumberField(TEXT("priority"), Priority);
	Result->SetStringField(TEXT("matched_volume"), VolumeName);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 13. set_surface_type
// ============================================================================

FMonolithActionResult FMonolithMeshAudioActions::SetSurfaceType(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: actor_name"));
	}

	FString SurfaceTypeName;
	if (!Params->TryGetStringField(TEXT("surface_type"), SurfaceTypeName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: surface_type"));
	}

	FString Error;
	AActor* Actor = MonolithMeshUtils::FindActorByName(ActorName, Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Find the physical surface enum from name
	EPhysicalSurface TargetSurface = SurfaceType_Default;
	bool bFoundSurface = false;

	const UPhysicsSettings* PhysSettings = GetDefault<UPhysicsSettings>();
	for (const FPhysicalSurfaceName& Entry : PhysSettings->PhysicalSurfaces)
	{
		if (Entry.Name.ToString().Equals(SurfaceTypeName, ESearchCase::IgnoreCase))
		{
			TargetSurface = Entry.Type;
			bFoundSurface = true;
			break;
		}
	}

	if (!bFoundSurface)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Surface type '%s' not registered in PhysicsSettings. Use create_surface_datatable first."),
			*SurfaceTypeName));
	}

	// Find or create a physical material with this surface type
	FString PhysMatName = FString::Printf(TEXT("PM_%s"), *SurfaceTypeName);
	UPhysicalMaterial* PhysMat = NewObject<UPhysicalMaterial>(GetTransientPackage(), FName(*PhysMatName));
	PhysMat->SurfaceType = TargetSurface;

	FScopedAudioTransaction Transaction(FText::FromString(TEXT("Monolith: Set Surface Type")));

	// Apply to all primitive components on the actor
	int32 ComponentsModified = 0;
	TArray<UPrimitiveComponent*> Components;
	Actor->GetComponents<UPrimitiveComponent>(Components);

	for (UPrimitiveComponent* Comp : Components)
	{
		Comp->Modify();
		Comp->SetPhysMaterialOverride(PhysMat);
		ComponentsModified++;
	}

	if (ComponentsModified == 0)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Actor '%s' has no primitive components to set surface type on"), *ActorName));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), ActorName);
	Result->SetStringField(TEXT("surface_type"), SurfaceTypeName);
	Result->SetNumberField(TEXT("components_modified"), ComponentsModified);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 14. create_surface_datatable
// ============================================================================

FMonolithActionResult FMonolithMeshAudioActions::CreateSurfaceDataTable(const TSharedPtr<FJsonObject>& Params)
{
	FString Template = TEXT("horror_default");
	Params->TryGetStringField(TEXT("template"), Template);

	FString SavePath;
	if (!Params->TryGetStringField(TEXT("save_path"), SavePath))
	{
		SavePath = GetDefault<UMonolithSettings>()->SurfaceAcousticsTablePath;
	}

	if (SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("No save_path specified and no default in MonolithSettings"));
	}

	// Convert asset path to package path
	FString PackagePath = SavePath;
	FString AssetName;
	{
		int32 LastSlash;
		if (PackagePath.FindLastChar('/', LastSlash))
		{
			AssetName = PackagePath.Mid(LastSlash + 1);
		}
		else
		{
			AssetName = PackagePath;
		}
	}

	FScopedAudioTransaction Transaction(FText::FromString(TEXT("Monolith: Create Surface DataTable")));

	// Step 1: Register surface types via UPhysicsSettings CDO
	UPhysicsSettings* PhysSettingsMutable = GetMutableDefault<UPhysicsSettings>();
	PhysSettingsMutable->Modify();

	auto Defaults = MonolithMeshAcoustics::GetHardcodedDefaults();
	int32 SurfacesRegistered = 0;

	// Map of surface names to their intended EPhysicalSurface values
	struct FSurfaceMapping
	{
		const TCHAR* Name;
		EPhysicalSurface Type;
	};

	static const FSurfaceMapping Mappings[] =
	{
		{ TEXT("Metal"),       SurfaceType1  },
		{ TEXT("Tile"),        SurfaceType2  },
		{ TEXT("Carpet"),      SurfaceType3  },
		{ TEXT("Wood"),        SurfaceType4  },
		{ TEXT("Glass"),       SurfaceType5  },
		{ TEXT("Water"),       SurfaceType6  },
		{ TEXT("Dirt"),        SurfaceType7  },
		{ TEXT("Gravel"),      SurfaceType8  },
		{ TEXT("Fabric"),      SurfaceType9  },
		{ TEXT("BrokenGlass"), SurfaceType10 },
		{ TEXT("Flesh"),       SurfaceType11 },
		{ TEXT("Concrete"),    SurfaceType12 },
	};

	for (const FSurfaceMapping& Mapping : Mappings)
	{
		// Check if already registered
		bool bAlreadyExists = false;
		for (const FPhysicalSurfaceName& Existing : PhysSettingsMutable->PhysicalSurfaces)
		{
			if (Existing.Type == Mapping.Type)
			{
				bAlreadyExists = true;
				break;
			}
		}

		if (!bAlreadyExists)
		{
			FPhysicalSurfaceName NewEntry;
			NewEntry.Type = Mapping.Type;
			NewEntry.Name = FName(Mapping.Name);
			PhysSettingsMutable->PhysicalSurfaces.Add(NewEntry);
			SurfacesRegistered++;
		}
	}

	// Save the physics settings via UPROPERTY serialization
	PhysSettingsMutable->SaveConfig();

	// Step 2: Create the DataTable
	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *PackagePath));
	}

	UDataTable* DataTable = NewObject<UDataTable>(Package, FName(*AssetName), RF_Public | RF_Standalone);
	DataTable->RowStruct = FAcousticSurfaceRow::StaticStruct();

	// Populate rows from the template
	for (const auto& Pair : Defaults)
	{
		FAcousticSurfaceRow RowData;
		RowData.AbsorptionCoefficient = Pair.Value.Absorption;
		RowData.TransmissionLossdB = Pair.Value.TransmissionLossdB;
		RowData.FootstepLoudness = Pair.Value.FootstepLoudness;
		RowData.DisplayName = Pair.Value.SurfaceName;
		DataTable->AddRow(FName(*Pair.Key), RowData);
	}

	// Notify the asset registry
	FAssetRegistryModule::AssetCreated(DataTable);
	DataTable->MarkPackageDirty();

	// Save the package
	FString FilePath = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Package, DataTable, *FilePath, SaveArgs);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("datatable_path"), SavePath);
	Result->SetNumberField(TEXT("row_count"), Defaults.Num());
	Result->SetNumberField(TEXT("surfaces_registered"), SurfacesRegistered);
	Result->SetStringField(TEXT("template"), Template);
	Result->SetBoolField(TEXT("physics_settings_saved"), true);

	TArray<TSharedPtr<FJsonValue>> SurfaceNames;
	for (const auto& Pair : Defaults)
	{
		SurfaceNames.Add(MakeShared<FJsonValueString>(Pair.Key));
	}
	Result->SetArrayField(TEXT("surfaces"), SurfaceNames);

	return FMonolithActionResult::Success(Result);
}
