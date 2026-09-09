#include "MonolithMeshDebugViewActions.h"
#include "MaterialDomain.h"
#include "SceneTypes.h"
#include "Materials/Material.h"
#include "MonolithMeshSpatialRegistry.h"
#include "MonolithMeshUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"

#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "LevelEditorViewport.h"

// Scene capture
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "RenderingThread.h"
#include "TextureResource.h"

// Highlight material
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/StaticMeshComponent.h"

// JSON / file I/O
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithDebugView, Log, All);

// ============================================================================
// Static State
// ============================================================================

/** Actors hidden by toggle_section_view, stored for restoration */
static TArray<TWeakObjectPtr<AActor>> SectionHiddenActors;
static bool bSectionViewActive = false;
static float CurrentSectionClipHeight = 300.0f;

/** Actors hidden by toggle_ceiling_visibility, stored for restoration */
static TArray<TWeakObjectPtr<AActor>> CeilingHiddenActors;
static bool bCeilingsHidden = false;

/** Highlight overlay actors, keyed by room_id */
static TMap<FString, TWeakObjectPtr<AActor>> HighlightActors;

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshDebugViewActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 1. toggle_section_view
	Registry.RegisterAction(TEXT("mesh"), TEXT("toggle_section_view"),
		TEXT("Section-cut debug view: hide all actors above a Z height to reveal building interiors. "
			"Tracks hidden actors for clean restoration. Can resolve clip_height from building_id + floor_index."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshDebugViewActions::ToggleSectionView),
		FParamSchemaBuilder()
			.Required(TEXT("enabled"), TEXT("boolean"), TEXT("Enable or disable section view"))
			.Optional(TEXT("clip_height"), TEXT("number"), TEXT("Z height above which everything is clipped (world space)"), TEXT("300"))
			.Optional(TEXT("building_id"), TEXT("string"), TEXT("If specified, clip_height is relative to building's world_origin Z"))
			.Optional(TEXT("floor_index"), TEXT("integer"), TEXT("If specified with building_id, sets clip_height to the top of that floor"))
			.Optional(TEXT("block_id"), TEXT("string"), TEXT("Block ID for spatial registry lookups"), TEXT("default"))
			.Build());

	// 2. toggle_ceiling_visibility
	Registry.RegisterAction(TEXT("mesh"), TEXT("toggle_ceiling_visibility"),
		TEXT("Show or hide actors tagged with BuildingCeiling and BuildingRoof. "
			"Useful for top-down inspection of procedural buildings without section clipping."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshDebugViewActions::ToggleCeilingVisibility),
		FParamSchemaBuilder()
			.Required(TEXT("visible"), TEXT("boolean"), TEXT("Show or hide ceilings/roofs"))
			.Optional(TEXT("building_id"), TEXT("string"), TEXT("If specified, only toggle for this building's actors"))
			.Optional(TEXT("include_roofs"), TEXT("boolean"), TEXT("Also toggle BuildingRoof tagged actors"), TEXT("true"))
			.Optional(TEXT("include_floors"), TEXT("boolean"), TEXT("Also toggle BuildingFloor tagged actors"), TEXT("false"))
			.Build());

	// 3. capture_floor_plan
	Registry.RegisterAction(TEXT("mesh"), TEXT("capture_floor_plan"),
		TEXT("Orthographic top-down scene capture of a building floor, saved as PNG. "
			"Auto-hides ceilings/roofs before capture and restores afterward. Uses spatial registry for building bounds."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshDebugViewActions::CaptureFloorPlan),
		FParamSchemaBuilder()
			.Required(TEXT("building_id"), TEXT("string"), TEXT("Building ID in spatial registry"))
			.Optional(TEXT("floor_index"), TEXT("integer"), TEXT("Which floor to capture"), TEXT("0"))
			.Optional(TEXT("block_id"), TEXT("string"), TEXT("Block ID for spatial registry"), TEXT("default"))
			.Optional(TEXT("resolution"), TEXT("integer"), TEXT("Image resolution in pixels"), TEXT("1024"))
			.OptionalDiskPath(TEXT("output_path"), TEXT("Full file path for PNG (default: auto-generated in Saved/Monolith/Captures/)"))
			.Optional(TEXT("padding"), TEXT("number"), TEXT("Extra padding around building bounds in cm"), TEXT("100"))
			.Optional(TEXT("hide_ceiling"), TEXT("boolean"), TEXT("Auto-hide ceilings before capture"), TEXT("true"))
			.Optional(TEXT("orthographic"), TEXT("boolean"), TEXT("Use orthographic projection"), TEXT("true"))
			.Build());

	// 4. highlight_room
	Registry.RegisterAction(TEXT("mesh"), TEXT("highlight_room"),
		TEXT("Spawn a translucent overlay box at a room's world bounds for visual debugging. "
			"Tracked by room_id for cleanup. Use clear=true to remove a specific highlight."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshDebugViewActions::HighlightRoom),
		FParamSchemaBuilder()
			.Required(TEXT("room_id"), TEXT("string"), TEXT("Room ID in spatial registry"))
			.Optional(TEXT("block_id"), TEXT("string"), TEXT("Block ID for spatial registry"), TEXT("default"))
			.Optional(TEXT("color"), TEXT("array"), TEXT("[R, G, B, A] normalized color"), TEXT("[1, 0.3, 0, 0.5]"))
			.Optional(TEXT("clear"), TEXT("boolean"), TEXT("Remove highlight instead of adding"), TEXT("false"))
			.Optional(TEXT("highlight_mode"), TEXT("string"), TEXT("'overlay' (translucent box) or 'wireframe' (outline)"), TEXT("overlay"))
			.Build());

	// 5. save_camera_bookmark
	Registry.RegisterAction(TEXT("mesh"), TEXT("save_camera_bookmark"),
		TEXT("Save the current editor viewport camera position and rotation as a named bookmark. "
			"Stored as JSON in Saved/Monolith/CameraBookmarks/."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshDebugViewActions::SaveCameraBookmark),
		FParamSchemaBuilder()
			.Required(TEXT("name"), TEXT("string"), TEXT("Bookmark name"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Optional description"))
			.Build());

	// 6. load_camera_bookmark
	Registry.RegisterAction(TEXT("mesh"), TEXT("load_camera_bookmark"),
		TEXT("Restore the editor viewport camera to a previously saved bookmark position."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshDebugViewActions::LoadCameraBookmark),
		FParamSchemaBuilder()
			.Required(TEXT("name"), TEXT("string"), TEXT("Bookmark name to load"))
			.Build());

	// 7. capture_building_views
	Registry.RegisterAction(TEXT("mesh"), TEXT("capture_building_views"),
		TEXT("Multi-angle diagnostic capture of a building for quality review. "
			"Produces 6 views: floor_plan (orthographic top-down), north/south/east/west (orthographic elevations), "
			"and perspective (45-degree corner view). All saved as PNGs. Uses spatial registry for building bounds."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshDebugViewActions::CaptureBuildingViews),
		FParamSchemaBuilder()
			.Required(TEXT("building_id"), TEXT("string"), TEXT("Building ID in spatial registry"))
			.Optional(TEXT("block_id"), TEXT("string"), TEXT("Block ID for spatial registry"), TEXT("default"))
			.Optional(TEXT("resolution"), TEXT("integer"), TEXT("Image resolution in pixels (square)"), TEXT("512"))
			.OptionalDiskPath(TEXT("output_dir"), TEXT("Directory for PNG output (default: Saved/Monolith/Captures/)"))
			.Optional(TEXT("hide_ceiling"), TEXT("boolean"), TEXT("Auto-hide ceilings/roofs for floor_plan view"), TEXT("true"))
			.Optional(TEXT("padding"), TEXT("number"), TEXT("Extra padding around building bounds in cm"), TEXT("100"))
			.Build());
}

// ============================================================================
// Helpers
// ============================================================================

FString FMonolithMeshDebugViewActions::GetBookmarkDirectory()
{
	return FPaths::ProjectSavedDir() / TEXT("Monolith") / TEXT("CameraBookmarks");
}

FString FMonolithMeshDebugViewActions::GetCapturesDirectory()
{
	return FPaths::ProjectSavedDir() / TEXT("Monolith") / TEXT("Captures");
}

bool FMonolithMeshDebugViewActions::EnsureDirectory(const FString& Path)
{
	IFileManager& FM = IFileManager::Get();
	if (!FM.DirectoryExists(*Path))
	{
		FM.MakeDirectory(*Path, true);
	}
	return FM.DirectoryExists(*Path);
}

// ============================================================================
// toggle_section_view
// ============================================================================

FMonolithActionResult FMonolithMeshDebugViewActions::ToggleSectionView(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	bool bEnabled = false;
	if (!Params->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		return FMonolithActionResult::Error(TEXT("'enabled' (boolean) is required"));
	}

	// If disabling, restore all previously hidden actors
	if (!bEnabled)
	{
		int32 Restored = 0;
		for (const TWeakObjectPtr<AActor>& WeakActor : SectionHiddenActors)
		{
			if (AActor* Actor = WeakActor.Get())
			{
				Actor->SetActorHiddenInGame(false);
				Actor->SetIsTemporarilyHiddenInEditor(false);
				++Restored;
			}
		}
		SectionHiddenActors.Empty();
		bSectionViewActive = false;

		auto Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("enabled"), false);
		Result->SetNumberField(TEXT("clip_height"), CurrentSectionClipHeight);
		Result->SetNumberField(TEXT("actors_shown"), Restored);
		Result->SetNumberField(TEXT("actors_hidden"), 0);
		return FMonolithActionResult::Success(Result);
	}

	// Resolve clip height
	float ClipHeight = 300.0f;
	if (Params->HasField(TEXT("clip_height")))
	{
		ClipHeight = static_cast<float>(Params->GetNumberField(TEXT("clip_height")));
	}

	// If building_id is specified, resolve relative to building origin
	FString BuildingId;
	if (Params->TryGetStringField(TEXT("building_id"), BuildingId) && !BuildingId.IsEmpty())
	{
		FString BlockId = TEXT("default");
		Params->TryGetStringField(TEXT("block_id"), BlockId);

		if (FMonolithMeshSpatialRegistry::HasBlock(BlockId))
		{
			const FSpatialBlock& Block = FMonolithMeshSpatialRegistry::GetBlock(BlockId);
			const FSpatialBuilding* Building = Block.Buildings.Find(BuildingId);
			if (Building)
			{
				float BaseZ = Building->WorldOrigin.Z;

				double FloorIndexDbl = -1.0;
				if (Params->TryGetNumberField(TEXT("floor_index"), FloorIndexDbl) && FloorIndexDbl >= 0.0)
				{
					// Default floor height is 270, floor thickness 3 — total 273 per floor
					// Clip at top of specified floor
					int32 FloorIndex = static_cast<int32>(FloorIndexDbl);
					ClipHeight = BaseZ + (FloorIndex + 1) * 273.0f;
				}
				else
				{
					// Clip height is relative to building origin
					ClipHeight += BaseZ;
				}
			}
			else
			{
				return FMonolithActionResult::Error(
					FString::Printf(TEXT("Building '%s' not found in block '%s'"), *BuildingId, *BlockId));
			}
		}
		else
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Block '%s' not found in spatial registry"), *BlockId));
		}
	}

	// First restore any previously hidden actors from a prior section view
	for (const TWeakObjectPtr<AActor>& WeakActor : SectionHiddenActors)
	{
		if (AActor* Actor = WeakActor.Get())
		{
			Actor->SetActorHiddenInGame(false);
			Actor->SetIsTemporarilyHiddenInEditor(false);
		}
	}
	SectionHiddenActors.Empty();

	// Hide actors above clip height
	int32 HiddenCount = 0;
	int32 ShownCount = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor->IsPendingKillPending())
		{
			continue;
		}

		// Skip non-visible actors (lights, volumes, cameras, etc.)
		// Only hide actors that have mesh components
		UStaticMeshComponent* SMC = Actor->FindComponentByClass<UStaticMeshComponent>();
		if (!SMC)
		{
			continue;
		}

		FVector Origin, BoxExtent;
		Actor->GetActorBounds(false, Origin, BoxExtent);
		float ActorBottomZ = Origin.Z - BoxExtent.Z;

		if (ActorBottomZ > ClipHeight)
		{
			// Entire actor is above clip — hide it
			Actor->SetActorHiddenInGame(true);
			Actor->SetIsTemporarilyHiddenInEditor(true);
			SectionHiddenActors.Add(Actor);
			++HiddenCount;
		}
		else
		{
			++ShownCount;
		}
	}

	bSectionViewActive = true;
	CurrentSectionClipHeight = ClipHeight;

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("enabled"), true);
	Result->SetNumberField(TEXT("clip_height"), ClipHeight);
	Result->SetNumberField(TEXT("actors_hidden"), HiddenCount);
	Result->SetNumberField(TEXT("actors_shown"), ShownCount);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// toggle_ceiling_visibility
// ============================================================================

FMonolithActionResult FMonolithMeshDebugViewActions::ToggleCeilingVisibility(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	bool bVisible = true;
	if (!Params->TryGetBoolField(TEXT("visible"), bVisible))
	{
		return FMonolithActionResult::Error(TEXT("'visible' (boolean) is required"));
	}

	bool bIncludeRoofs = true;
	Params->TryGetBoolField(TEXT("include_roofs"), bIncludeRoofs);

	bool bIncludeFloors = false;
	Params->TryGetBoolField(TEXT("include_floors"), bIncludeFloors);

	FString BuildingId;
	Params->TryGetStringField(TEXT("building_id"), BuildingId);

	// If showing ceilings again, restore previously hidden actors
	if (bVisible && bCeilingsHidden)
	{
		int32 Restored = 0;
		for (const TWeakObjectPtr<AActor>& WeakActor : CeilingHiddenActors)
		{
			if (AActor* Actor = WeakActor.Get())
			{
				Actor->SetActorHiddenInGame(false);
				Actor->SetIsTemporarilyHiddenInEditor(false);
				++Restored;
			}
		}
		CeilingHiddenActors.Empty();
		bCeilingsHidden = false;

		auto Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("visible"), true);
		Result->SetNumberField(TEXT("ceilings_toggled"), Restored);
		Result->SetNumberField(TEXT("roofs_toggled"), 0);
		return FMonolithActionResult::Success(Result);
	}

	if (bVisible)
	{
		// Already visible, nothing to do
		auto Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("visible"), true);
		Result->SetNumberField(TEXT("ceilings_toggled"), 0);
		Result->SetNumberField(TEXT("roofs_toggled"), 0);
		return FMonolithActionResult::Success(Result);
	}

	// Hide ceilings/roofs
	CeilingHiddenActors.Empty();
	int32 CeilingsToggled = 0;
	int32 RoofsToggled = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor->IsPendingKillPending())
		{
			continue;
		}

		// Filter by building_id if specified — check for a matching tag
		if (!BuildingId.IsEmpty())
		{
			bool bBelongsToBuilding = false;
			for (const FName& Tag : Actor->Tags)
			{
				if (Tag.ToString().Contains(BuildingId))
				{
					bBelongsToBuilding = true;
					break;
				}
			}
			if (!bBelongsToBuilding)
			{
				continue;
			}
		}

		bool bIsCeiling = Actor->Tags.Contains(FName(TEXT("BuildingCeiling")));
		bool bIsRoof = Actor->Tags.Contains(FName(TEXT("BuildingRoof")));
		bool bIsFloor = Actor->Tags.Contains(FName(TEXT("BuildingFloor")));

		bool bShouldHide = false;
		if (bIsCeiling)
		{
			bShouldHide = true;
			++CeilingsToggled;
		}
		if (bIsRoof && bIncludeRoofs)
		{
			bShouldHide = true;
			++RoofsToggled;
		}
		if (bIsFloor && bIncludeFloors)
		{
			bShouldHide = true;
			++CeilingsToggled; // Count floors in ceiling total
		}

		if (bShouldHide)
		{
			Actor->SetActorHiddenInGame(true);
			Actor->SetIsTemporarilyHiddenInEditor(true);
			CeilingHiddenActors.Add(Actor);
		}
	}

	bCeilingsHidden = true;

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("visible"), false);
	Result->SetNumberField(TEXT("ceilings_toggled"), CeilingsToggled);
	Result->SetNumberField(TEXT("roofs_toggled"), RoofsToggled);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// capture_floor_plan
// ============================================================================

FMonolithActionResult FMonolithMeshDebugViewActions::CaptureFloorPlan(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FString BuildingId;
	if (!Params->TryGetStringField(TEXT("building_id"), BuildingId) || BuildingId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'building_id' (string) is required"));
	}

	FString BlockId = TEXT("default");
	Params->TryGetStringField(TEXT("block_id"), BlockId);

	if (!FMonolithMeshSpatialRegistry::HasBlock(BlockId))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Block '%s' not found in spatial registry"), *BlockId));
	}

	const FSpatialBlock& Block = FMonolithMeshSpatialRegistry::GetBlock(BlockId);
	const FSpatialBuilding* Building = Block.Buildings.Find(BuildingId);
	if (!Building)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Building '%s' not found in block '%s'"), *BuildingId, *BlockId));
	}

	int32 FloorIndex = 0;
	{
		double V = 0.0;
		if (Params->TryGetNumberField(TEXT("floor_index"), V))
		{
			FloorIndex = static_cast<int32>(V);
		}
	}

	int32 Resolution = 1024;
	{
		double V = 0.0;
		if (Params->TryGetNumberField(TEXT("resolution"), V))
		{
			Resolution = static_cast<int32>(V);
		}
	}
	Resolution = FMath::Clamp(Resolution, 128, 4096);

	float Padding = 100.0f;
	if (Params->HasField(TEXT("padding")))
	{
		Padding = static_cast<float>(Params->GetNumberField(TEXT("padding")));
	}

	bool bHideCeiling = true;
	Params->TryGetBoolField(TEXT("hide_ceiling"), bHideCeiling);

	bool bOrthographic = true;
	Params->TryGetBoolField(TEXT("orthographic"), bOrthographic);

	// Compute building bounds from rooms on the target floor
	FBox FloorBounds(ForceInit);
	const TArray<FString>* FloorRoomIds = Building->FloorToRoomIds.Find(FloorIndex);
	if (FloorRoomIds)
	{
		for (const FString& RoomId : *FloorRoomIds)
		{
			const FSpatialRoom* Room = Block.Rooms.Find(RoomId);
			if (Room && Room->WorldBounds.IsValid)
			{
				FloorBounds += Room->WorldBounds;
			}
		}
	}

	// Fallback: use building origin + footprint polygon extents if no rooms found
	if (!FloorBounds.IsValid)
	{
		FVector2D MinXY(FLT_MAX, FLT_MAX), MaxXY(-FLT_MAX, -FLT_MAX);
		for (const FVector2D& V : Building->FootprintPolygon)
		{
			MinXY.X = FMath::Min(MinXY.X, V.X);
			MinXY.Y = FMath::Min(MinXY.Y, V.Y);
			MaxXY.X = FMath::Max(MaxXY.X, V.X);
			MaxXY.Y = FMath::Max(MaxXY.Y, V.Y);
		}
		if (MinXY.X < FLT_MAX)
		{
			float FloorZ = Building->WorldOrigin.Z + FloorIndex * 273.0f;
			FloorBounds = FBox(
				FVector(MinXY.X, MinXY.Y, FloorZ),
				FVector(MaxXY.X, MaxXY.Y, FloorZ + 270.0f));
		}
		else
		{
			return FMonolithActionResult::Error(TEXT("Could not determine building bounds for capture"));
		}
	}

	// Add padding
	FloorBounds = FloorBounds.ExpandBy(Padding);

	FVector BoundsCenter = FloorBounds.GetCenter();
	FVector BoundsExtent = FloorBounds.GetExtent();
	float OrthoWidth = FMath::Max(BoundsExtent.X, BoundsExtent.Y) * 2.0f;

	// Resolve output path
	FString OutputPath;
	if (!Params->TryGetStringField(TEXT("output_path"), OutputPath) || OutputPath.IsEmpty())
	{
		EnsureDirectory(GetCapturesDirectory());
		OutputPath = GetCapturesDirectory() / FString::Printf(
			TEXT("%s_floor%d_%s.png"), *BuildingId, FloorIndex,
			*FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}

	// --- Temporarily hide ceilings/roofs ---
	TArray<AActor*> TempHiddenActors;
	if (bHideCeiling)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsPendingKillPending())
			{
				continue;
			}
			bool bIsCeiling = Actor->Tags.Contains(FName(TEXT("BuildingCeiling")));
			bool bIsRoof = Actor->Tags.Contains(FName(TEXT("BuildingRoof")));
			if (bIsCeiling || bIsRoof)
			{
				if (!Actor->IsHidden())
				{
					Actor->SetActorHiddenInGame(true);
					Actor->SetIsTemporarilyHiddenInEditor(true);
					TempHiddenActors.Add(Actor);
				}
			}
		}
	}

	// --- Create render target ---
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	RT->RenderTargetFormat = RTF_RGBA8;
	RT->InitCustomFormat(Resolution, Resolution, PF_B8G8R8A8, false);
	RT->ClearColor = FLinearColor::White;
	RT->bAutoGenerateMips = false;
	RT->UpdateResourceImmediate(true);

	// --- Create scene capture component ---
	USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	Capture->bTickInEditor = false;
	Capture->SetComponentTickEnabled(false);
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->bAlwaysPersistRenderingState = true;
	Capture->TextureTarget = RT;
	Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

	if (bOrthographic)
	{
		Capture->ProjectionType = ECameraProjectionMode::Orthographic;
		Capture->OrthoWidth = OrthoWidth;
	}
	else
	{
		Capture->ProjectionType = ECameraProjectionMode::Perspective;
		Capture->FOVAngle = 90.0f;
	}

	// Show flags: unlit for clean floor plan
	Capture->ShowFlags.SetLighting(false);
	Capture->ShowFlags.SetPostProcessing(false);
	Capture->ShowFlags.SetTonemapper(false);
	Capture->ShowFlags.SetEyeAdaptation(false);
	Capture->ShowFlags.SetBloom(false);
	Capture->ShowFlags.SetMotionBlur(false);
	Capture->ShowFlags.SetFog(false);
	Capture->ShowFlags.SetVolumetricFog(false);
	Capture->ShowFlags.SetAtmosphere(false);

	Capture->RegisterComponentWithWorld(World);

	// Position above building center, looking straight down
	float CaptureHeight = FloorBounds.Max.Z + 500.0f;
	FVector CaptureLocation(BoundsCenter.X, BoundsCenter.Y, CaptureHeight);
	FRotator CaptureRotation(-90.0f, 0.0f, 0.0f);
	Capture->SetWorldLocationAndRotation(CaptureLocation, CaptureRotation);

	// Capture
	Capture->CaptureScene();

	// Read pixels
	FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource();
	bool bSaveOk = false;
	if (RTResource)
	{
		TArray<FColor> Pixels;
		if (RTResource->ReadPixels(Pixels) && Pixels.Num() > 0)
		{
			// Ensure output directory
			FString Dir = FPaths::GetPath(OutputPath);
			IFileManager::Get().MakeDirectory(*Dir, true);

			FImage Image;
			Image.Init(Resolution, Resolution, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FColor));
			bSaveOk = FImageUtils::SaveImageAutoFormat(*OutputPath, Image);
		}
	}

	// Cleanup capture
	Capture->TextureTarget = nullptr;
	if (Capture->IsRegistered())
	{
		Capture->UnregisterComponent();
	}

	// Restore ceiling visibility
	for (AActor* Actor : TempHiddenActors)
	{
		if (Actor && !Actor->IsPendingKillPending())
		{
			Actor->SetActorHiddenInGame(false);
			Actor->SetIsTemporarilyHiddenInEditor(false);
		}
	}

	if (!bSaveOk)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to capture or save floor plan to '%s'"), *OutputPath));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("building_id"), BuildingId);
	Result->SetNumberField(TEXT("floor_index"), FloorIndex);
	Result->SetStringField(TEXT("output_path"), OutputPath);
	Result->SetNumberField(TEXT("resolution"), Resolution);

	auto BoundsObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> MinArr, MaxArr;
	MinArr.Add(MakeShared<FJsonValueNumber>(FloorBounds.Min.X));
	MinArr.Add(MakeShared<FJsonValueNumber>(FloorBounds.Min.Y));
	MinArr.Add(MakeShared<FJsonValueNumber>(FloorBounds.Min.Z));
	MaxArr.Add(MakeShared<FJsonValueNumber>(FloorBounds.Max.X));
	MaxArr.Add(MakeShared<FJsonValueNumber>(FloorBounds.Max.Y));
	MaxArr.Add(MakeShared<FJsonValueNumber>(FloorBounds.Max.Z));
	BoundsObj->SetArrayField(TEXT("min"), MinArr);
	BoundsObj->SetArrayField(TEXT("max"), MaxArr);
	Result->SetObjectField(TEXT("bounds"), BoundsObj);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// highlight_room
// ============================================================================

FMonolithActionResult FMonolithMeshDebugViewActions::HighlightRoom(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FString RoomId;
	if (!Params->TryGetStringField(TEXT("room_id"), RoomId) || RoomId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'room_id' (string) is required"));
	}

	bool bClear = false;
	Params->TryGetBoolField(TEXT("clear"), bClear);

	// Clear mode: remove existing highlight for this room
	if (bClear)
	{
		TWeakObjectPtr<AActor>* Existing = HighlightActors.Find(RoomId);
		if (Existing && Existing->IsValid())
		{
			AActor* Actor = Existing->Get();
			World->DestroyActor(Actor);
		}
		HighlightActors.Remove(RoomId);

		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("room_id"), RoomId);
		Result->SetBoolField(TEXT("highlighted"), false);
		Result->SetBoolField(TEXT("cleared"), true);
		return FMonolithActionResult::Success(Result);
	}

	FString BlockId = TEXT("default");
	Params->TryGetStringField(TEXT("block_id"), BlockId);

	if (!FMonolithMeshSpatialRegistry::HasBlock(BlockId))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Block '%s' not found in spatial registry"), *BlockId));
	}

	const FSpatialBlock& Block = FMonolithMeshSpatialRegistry::GetBlock(BlockId);
	const FSpatialRoom* Room = Block.Rooms.Find(RoomId);
	if (!Room)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Room '%s' not found in block '%s'"), *RoomId, *BlockId));
	}

	if (!Room->WorldBounds.IsValid)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Room '%s' has no valid world bounds"), *RoomId));
	}

	// Parse color
	FLinearColor HighlightColor(1.0f, 0.3f, 0.0f, 0.5f);
	const TArray<TSharedPtr<FJsonValue>>* ColorArr = nullptr;
	if (Params->TryGetArrayField(TEXT("color"), ColorArr) && ColorArr && ColorArr->Num() >= 3)
	{
		HighlightColor.R = static_cast<float>((*ColorArr)[0]->AsNumber());
		HighlightColor.G = static_cast<float>((*ColorArr)[1]->AsNumber());
		HighlightColor.B = static_cast<float>((*ColorArr)[2]->AsNumber());
		if (ColorArr->Num() >= 4)
		{
			HighlightColor.A = static_cast<float>((*ColorArr)[3]->AsNumber());
		}
	}

	FString HighlightMode = TEXT("overlay");
	Params->TryGetStringField(TEXT("highlight_mode"), HighlightMode);

	// Remove existing highlight for this room if any
	TWeakObjectPtr<AActor>* Existing = HighlightActors.Find(RoomId);
	if (Existing && Existing->IsValid())
	{
		World->DestroyActor(Existing->Get());
	}

	// Compute room center and extent
	FVector RoomCenter = Room->WorldBounds.GetCenter();
	FVector RoomExtent = Room->WorldBounds.GetExtent();

	// Spawn a cube static mesh actor as overlay
	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = FName(*FString::Printf(TEXT("MonolithHighlight_%s"), *RoomId));
	SpawnParams.bNoFail = true;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AStaticMeshActor* HighlightActor = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(), FTransform(FRotator::ZeroRotator, RoomCenter), SpawnParams);

	if (!HighlightActor)
	{
		return FMonolithActionResult::Error(TEXT("Failed to spawn highlight actor"));
	}

	// Set actor label and tags
	HighlightActor->SetActorLabel(FString::Printf(TEXT("Highlight_%s"), *RoomId));
	HighlightActor->Tags.Add(FName(TEXT("Monolith.DebugHighlight")));
	HighlightActor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Room:%s"), *RoomId)));

	// Organize in outliner
	HighlightActor->SetFolderPath(FName(TEXT("CityBlock/Debug")));

	// Use engine's default cube mesh
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr,
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!CubeMesh)
	{
		World->DestroyActor(HighlightActor);
		return FMonolithActionResult::Error(TEXT("Failed to load /Engine/BasicShapes/Cube mesh"));
	}

	UStaticMeshComponent* SMC = HighlightActor->GetStaticMeshComponent();
	SMC->SetStaticMesh(CubeMesh);

	// Scale to room extent (Cube is 100x100x100 by default, centered at origin)
	FVector Scale = RoomExtent / 50.0f; // 50 = half of 100 (cube size)
	HighlightActor->SetActorScale3D(Scale);

	// Create translucent material
	UMaterial* BaseMat = UMaterial::GetDefaultMaterial(EMaterialDomain::MD_Surface);
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMat, GetTransientPackage());

	if (HighlightMode == TEXT("wireframe"))
	{
		MID->SetScalarParameterValue(TEXT("Opacity"), 0.0f);
		SMC->SetRenderCustomDepth(true);
		SMC->SetCustomDepthStencilValue(255);
	}

	// Override material — use the translucent overlay approach
	// Note: MID on default material won't be translucent by itself since the base
	// material is opaque. Instead, we set the base color and use OverlayMaterial.
	SMC->SetOverlayMaterial(MID);

	// For a proper translucent look, create a simple color-only MID
	// Since we can't change blend mode on the fly, we'll use the overlay approach
	// which renders additively on top of the mesh.
	// For the overlay: set color via the overlay material
	MID->SetVectorParameterValue(TEXT("BaseColor"), HighlightColor);

	// Disable collision so it doesn't interfere with anything
	SMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SMC->SetCastShadow(false);

	// Make it non-selectable in editor to avoid accidental selection
	HighlightActor->SetActorHiddenInGame(false);

	// Track for cleanup
	HighlightActors.Add(RoomId, HighlightActor);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("room_id"), RoomId);
	Result->SetBoolField(TEXT("highlighted"), true);

	TArray<TSharedPtr<FJsonValue>> ColorResult;
	ColorResult.Add(MakeShared<FJsonValueNumber>(HighlightColor.R));
	ColorResult.Add(MakeShared<FJsonValueNumber>(HighlightColor.G));
	ColorResult.Add(MakeShared<FJsonValueNumber>(HighlightColor.B));
	ColorResult.Add(MakeShared<FJsonValueNumber>(HighlightColor.A));
	Result->SetArrayField(TEXT("color"), ColorResult);

	Result->SetStringField(TEXT("actor_name"), HighlightActor->GetActorLabel());
	Result->SetStringField(TEXT("highlight_mode"), HighlightMode);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// save_camera_bookmark
// ============================================================================

FMonolithActionResult FMonolithMeshDebugViewActions::SaveCameraBookmark(const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'name' (string) is required"));
	}

	// Get editor viewport camera
	FLevelEditorViewportClient* ViewportClient = nullptr;
	if (GEditor && GEditor->GetLevelViewportClients().Num() > 0)
	{
		ViewportClient = GEditor->GetLevelViewportClients()[0];
	}
	if (!ViewportClient)
	{
		return FMonolithActionResult::Error(TEXT("No active editor viewport found"));
	}

	FVector CamLocation = ViewportClient->GetViewLocation();
	FRotator CamRotation = ViewportClient->GetViewRotation();

	// Build bookmark JSON
	auto Bookmark = MakeShared<FJsonObject>();
	Bookmark->SetStringField(TEXT("name"), Name);

	FString Description;
	if (Params->TryGetStringField(TEXT("description"), Description) && !Description.IsEmpty())
	{
		Bookmark->SetStringField(TEXT("description"), Description);
	}

	TArray<TSharedPtr<FJsonValue>> LocArr;
	LocArr.Add(MakeShared<FJsonValueNumber>(CamLocation.X));
	LocArr.Add(MakeShared<FJsonValueNumber>(CamLocation.Y));
	LocArr.Add(MakeShared<FJsonValueNumber>(CamLocation.Z));
	Bookmark->SetArrayField(TEXT("location"), LocArr);

	TArray<TSharedPtr<FJsonValue>> RotArr;
	RotArr.Add(MakeShared<FJsonValueNumber>(CamRotation.Pitch));
	RotArr.Add(MakeShared<FJsonValueNumber>(CamRotation.Yaw));
	RotArr.Add(MakeShared<FJsonValueNumber>(CamRotation.Roll));
	Bookmark->SetArrayField(TEXT("rotation"), RotArr);

	Bookmark->SetStringField(TEXT("timestamp"), FDateTime::Now().ToIso8601());

	// Serialize to JSON string
	FString JsonString;
	auto Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(Bookmark, Writer);

	// Save to file
	FString BookmarkDir = GetBookmarkDirectory();
	EnsureDirectory(BookmarkDir);
	FString FilePath = BookmarkDir / FString::Printf(TEXT("%s.json"), *Name);

	if (!FFileHelper::SaveStringToFile(JsonString, *FilePath))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to save bookmark to '%s'"), *FilePath));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Name);
	Result->SetArrayField(TEXT("location"), LocArr);
	Result->SetArrayField(TEXT("rotation"), RotArr);
	Result->SetStringField(TEXT("saved_to"), FilePath);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// load_camera_bookmark
// ============================================================================

FMonolithActionResult FMonolithMeshDebugViewActions::LoadCameraBookmark(const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'name' (string) is required"));
	}

	// Load bookmark JSON
	FString FilePath = GetBookmarkDirectory() / FString::Printf(TEXT("%s.json"), *Name);
	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Bookmark '%s' not found at '%s'"), *Name, *FilePath));
	}

	TSharedPtr<FJsonObject> Bookmark;
	auto Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Bookmark) || !Bookmark.IsValid())
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to parse bookmark JSON from '%s'"), *FilePath));
	}

	// Parse location
	const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
	if (!Bookmark->TryGetArrayField(TEXT("location"), LocArr) || !LocArr || LocArr->Num() < 3)
	{
		return FMonolithActionResult::Error(TEXT("Bookmark missing valid 'location' array"));
	}
	FVector Location(
		static_cast<float>((*LocArr)[0]->AsNumber()),
		static_cast<float>((*LocArr)[1]->AsNumber()),
		static_cast<float>((*LocArr)[2]->AsNumber()));

	// Parse rotation
	const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
	if (!Bookmark->TryGetArrayField(TEXT("rotation"), RotArr) || !RotArr || RotArr->Num() < 3)
	{
		return FMonolithActionResult::Error(TEXT("Bookmark missing valid 'rotation' array"));
	}
	FRotator Rotation(
		static_cast<float>((*RotArr)[0]->AsNumber()),
		static_cast<float>((*RotArr)[1]->AsNumber()),
		static_cast<float>((*RotArr)[2]->AsNumber()));

	// Apply to editor viewport
	FLevelEditorViewportClient* ViewportClient = nullptr;
	if (GEditor && GEditor->GetLevelViewportClients().Num() > 0)
	{
		ViewportClient = GEditor->GetLevelViewportClients()[0];
	}
	if (!ViewportClient)
	{
		return FMonolithActionResult::Error(TEXT("No active editor viewport found"));
	}

	ViewportClient->SetViewLocation(Location);
	ViewportClient->SetViewRotation(Rotation);
	ViewportClient->Invalidate();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Name);

	TArray<TSharedPtr<FJsonValue>> LocResult, RotResult;
	LocResult.Add(MakeShared<FJsonValueNumber>(Location.X));
	LocResult.Add(MakeShared<FJsonValueNumber>(Location.Y));
	LocResult.Add(MakeShared<FJsonValueNumber>(Location.Z));
	RotResult.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
	RotResult.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
	RotResult.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
	Result->SetArrayField(TEXT("location"), LocResult);
	Result->SetArrayField(TEXT("rotation"), RotResult);
	Result->SetBoolField(TEXT("loaded"), true);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Shared Helpers for capture_building_views
// ============================================================================

bool FMonolithMeshDebugViewActions::ComputeBuildingBounds(
	const FSpatialBlock& Block, const FSpatialBuilding& Building, FBox& OutBounds)
{
	OutBounds = FBox(ForceInit);

	// Accumulate bounds from all rooms across all floors
	for (const auto& FloorEntry : Building.FloorToRoomIds)
	{
		for (const FString& RoomId : FloorEntry.Value)
		{
			const FSpatialRoom* Room = Block.Rooms.Find(RoomId);
			if (Room && Room->WorldBounds.IsValid)
			{
				OutBounds += Room->WorldBounds;
			}
		}
	}

	// Fallback: use footprint polygon + estimated height
	if (!OutBounds.IsValid)
	{
		FVector2D MinXY(FLT_MAX, FLT_MAX), MaxXY(-FLT_MAX, -FLT_MAX);
		for (const FVector2D& V : Building.FootprintPolygon)
		{
			MinXY.X = FMath::Min(MinXY.X, V.X);
			MinXY.Y = FMath::Min(MinXY.Y, V.Y);
			MaxXY.X = FMath::Max(MaxXY.X, V.X);
			MaxXY.Y = FMath::Max(MaxXY.Y, V.Y);
		}
		if (MinXY.X < FLT_MAX)
		{
			int32 NumFloors = FMath::Max(Building.FloorToRoomIds.Num(), 1);
			float TotalHeight = NumFloors * 273.0f;
			OutBounds = FBox(
				FVector(MinXY.X, MinXY.Y, Building.WorldOrigin.Z),
				FVector(MaxXY.X, MaxXY.Y, Building.WorldOrigin.Z + TotalHeight));
		}
	}

	return OutBounds.IsValid != 0;
}

TArray<AActor*> FMonolithMeshDebugViewActions::HideCeilingActors(UWorld* World, const FString& BuildingId)
{
	TArray<AActor*> Hidden;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor->IsPendingKillPending())
		{
			continue;
		}

		// Filter by building_id if specified
		if (!BuildingId.IsEmpty())
		{
			bool bBelongsToBuilding = false;
			for (const FName& Tag : Actor->Tags)
			{
				if (Tag.ToString().Contains(BuildingId))
				{
					bBelongsToBuilding = true;
					break;
				}
			}
			if (!bBelongsToBuilding)
			{
				continue;
			}
		}

		bool bIsCeiling = Actor->Tags.Contains(FName(TEXT("BuildingCeiling")));
		bool bIsRoof = Actor->Tags.Contains(FName(TEXT("BuildingRoof")));
		if ((bIsCeiling || bIsRoof) && !Actor->IsHidden())
		{
			Actor->SetActorHiddenInGame(true);
			Actor->SetIsTemporarilyHiddenInEditor(true);
			Hidden.Add(Actor);
		}
	}
	return Hidden;
}

void FMonolithMeshDebugViewActions::RestoreHiddenActors(const TArray<AActor*>& Actors)
{
	for (AActor* Actor : Actors)
	{
		if (Actor && !Actor->IsPendingKillPending())
		{
			Actor->SetActorHiddenInGame(false);
			Actor->SetIsTemporarilyHiddenInEditor(false);
		}
	}
}

bool FMonolithMeshDebugViewActions::CaptureViewToPNG(
	UWorld* World,
	const FVector& CameraLocation,
	const FRotator& CameraRotation,
	int32 Resolution,
	const FString& OutputPath,
	bool bOrthographic,
	float OrthoWidth,
	float FOVAngle)
{
	// Create render target
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	RT->RenderTargetFormat = RTF_RGBA8;
	RT->InitCustomFormat(Resolution, Resolution, PF_B8G8R8A8, false);
	RT->ClearColor = FLinearColor::White;
	RT->bAutoGenerateMips = false;
	RT->UpdateResourceImmediate(true);

	// Create scene capture component
	USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	Capture->bTickInEditor = false;
	Capture->SetComponentTickEnabled(false);
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->bAlwaysPersistRenderingState = true;
	Capture->TextureTarget = RT;
	Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

	if (bOrthographic)
	{
		Capture->ProjectionType = ECameraProjectionMode::Orthographic;
		Capture->OrthoWidth = OrthoWidth;
	}
	else
	{
		Capture->ProjectionType = ECameraProjectionMode::Perspective;
		Capture->FOVAngle = FOVAngle;
	}

	// Unlit show flags for clean diagnostic views
	Capture->ShowFlags.SetLighting(false);
	Capture->ShowFlags.SetPostProcessing(false);
	Capture->ShowFlags.SetTonemapper(false);
	Capture->ShowFlags.SetEyeAdaptation(false);
	Capture->ShowFlags.SetBloom(false);
	Capture->ShowFlags.SetMotionBlur(false);
	Capture->ShowFlags.SetFog(false);
	Capture->ShowFlags.SetVolumetricFog(false);
	Capture->ShowFlags.SetAtmosphere(false);

	Capture->RegisterComponentWithWorld(World);
	Capture->SetWorldLocationAndRotation(CameraLocation, CameraRotation);
	Capture->CaptureScene();

	// Read pixels and save
	FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource();
	bool bSaveOk = false;
	if (RTResource)
	{
		TArray<FColor> Pixels;
		if (RTResource->ReadPixels(Pixels) && Pixels.Num() > 0)
		{
			FString Dir = FPaths::GetPath(OutputPath);
			IFileManager::Get().MakeDirectory(*Dir, true);

			FImage Image;
			Image.Init(Resolution, Resolution, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FColor));
			bSaveOk = FImageUtils::SaveImageAutoFormat(*OutputPath, Image);
		}
	}

	// Cleanup
	Capture->TextureTarget = nullptr;
	if (Capture->IsRegistered())
	{
		Capture->UnregisterComponent();
	}

	return bSaveOk;
}

// ============================================================================
// capture_building_views
// ============================================================================

FMonolithActionResult FMonolithMeshDebugViewActions::CaptureBuildingViews(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// --- Parse params ---

	FString BuildingId;
	if (!Params->TryGetStringField(TEXT("building_id"), BuildingId) || BuildingId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'building_id' (string) is required"));
	}

	FString BlockId = TEXT("default");
	Params->TryGetStringField(TEXT("block_id"), BlockId);

	if (!FMonolithMeshSpatialRegistry::HasBlock(BlockId))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Block '%s' not found in spatial registry"), *BlockId));
	}

	const FSpatialBlock& Block = FMonolithMeshSpatialRegistry::GetBlock(BlockId);
	const FSpatialBuilding* Building = Block.Buildings.Find(BuildingId);
	if (!Building)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Building '%s' not found in block '%s'"), *BuildingId, *BlockId));
	}

	int32 Resolution = 512;
	{
		double V = 0.0;
		if (Params->TryGetNumberField(TEXT("resolution"), V))
		{
			Resolution = static_cast<int32>(V);
		}
	}
	Resolution = FMath::Clamp(Resolution, 128, 4096);

	float Padding = 100.0f;
	if (Params->HasField(TEXT("padding")))
	{
		Padding = static_cast<float>(Params->GetNumberField(TEXT("padding")));
	}

	bool bHideCeiling = true;
	Params->TryGetBoolField(TEXT("hide_ceiling"), bHideCeiling);

	FString OutputDir;
	if (!Params->TryGetStringField(TEXT("output_dir"), OutputDir) || OutputDir.IsEmpty())
	{
		OutputDir = GetCapturesDirectory();
	}
	EnsureDirectory(OutputDir);

	// --- Compute building bounds ---

	FBox BuildingBounds;
	if (!ComputeBuildingBounds(Block, *Building, BuildingBounds))
	{
		return FMonolithActionResult::Error(TEXT("Could not determine building bounds for capture"));
	}

	FBox PaddedBounds = BuildingBounds.ExpandBy(Padding);
	FVector Center = PaddedBounds.GetCenter();
	FVector Extent = PaddedBounds.GetExtent();

	float BuildingWidth  = Extent.X * 2.0f;  // X dimension
	float BuildingDepth  = Extent.Y * 2.0f;  // Y dimension
	float BuildingHeight = Extent.Z * 2.0f;  // Z dimension

	// --- Define the 6 views ---

	struct FViewSpec
	{
		FString Type;
		FVector Location;
		FRotator Rotation;
		bool bOrthographic;
		float OrthoWidth;
		float FOVAngle;
		bool bHideCeiling;
	};

	TArray<FViewSpec> Views;

	// 1. Floor plan — orthographic top-down
	{
		float OrthoW = FMath::Max(BuildingWidth, BuildingDepth) * 1.2f;
		float CaptureZ = PaddedBounds.Max.Z + 500.0f;
		Views.Add({
			TEXT("floor_plan"),
			FVector(Center.X, Center.Y, CaptureZ),
			FRotator(-90.0f, 0.0f, 0.0f),
			true,
			OrthoW,
			0.0f,
			true  // hide ceiling for floor plan
		});
	}

	// 2-5. Cardinal elevation views — orthographic, perpendicular to each wall face
	// Camera distance = max(building_width_along_view, building_height) * 1.5
	{
		// North: camera looks south (-Y), placed at +Y
		float ElevOrthoW = FMath::Max(BuildingWidth, BuildingHeight);
		float Distance = FMath::Max(BuildingDepth, BuildingHeight) * 1.5f;
		Views.Add({
			TEXT("north"),
			FVector(Center.X, Center.Y + Distance, Center.Z),
			FRotator(0.0f, -90.0f, 0.0f),  // Yaw -90 = looking towards -Y (south)
			true,
			ElevOrthoW * 1.2f,
			0.0f,
			false
		});
	}
	{
		// South: camera looks north (+Y), placed at -Y
		float ElevOrthoW = FMath::Max(BuildingWidth, BuildingHeight);
		float Distance = FMath::Max(BuildingDepth, BuildingHeight) * 1.5f;
		Views.Add({
			TEXT("south"),
			FVector(Center.X, Center.Y - Distance, Center.Z),
			FRotator(0.0f, 90.0f, 0.0f),  // Yaw +90 = looking towards +Y (north)
			true,
			ElevOrthoW * 1.2f,
			0.0f,
			false
		});
	}
	{
		// East: camera looks west (-X), placed at +X
		float ElevOrthoW = FMath::Max(BuildingDepth, BuildingHeight);
		float Distance = FMath::Max(BuildingWidth, BuildingHeight) * 1.5f;
		Views.Add({
			TEXT("east"),
			FVector(Center.X + Distance, Center.Y, Center.Z),
			FRotator(0.0f, 180.0f, 0.0f),  // Yaw 180 = looking towards -X (west)
			true,
			ElevOrthoW * 1.2f,
			0.0f,
			false
		});
	}
	{
		// West: camera looks east (+X), placed at -X
		float ElevOrthoW = FMath::Max(BuildingDepth, BuildingHeight);
		float Distance = FMath::Max(BuildingWidth, BuildingHeight) * 1.5f;
		Views.Add({
			TEXT("west"),
			FVector(Center.X - Distance, Center.Y, Center.Z),
			FRotator(0.0f, 0.0f, 0.0f),  // Yaw 0 = looking towards +X (east)
			true,
			ElevOrthoW * 1.2f,
			0.0f,
			false
		});
	}

	// 6. Perspective — 45° corner view from the NE, pitched down 30°
	{
		// Diagonal distance from center — use the bounding sphere radius * 2
		float DiagonalExtent = FMath::Sqrt(Extent.X * Extent.X + Extent.Y * Extent.Y + Extent.Z * Extent.Z);
		float Distance = DiagonalExtent * 2.0f;

		// 45° azimuth (NE corner), 30° pitch down
		float Azimuth = 45.0f;
		float Pitch = -30.0f;
		float AzimuthRad = FMath::DegreesToRadians(Azimuth);

		FVector CamOffset(
			FMath::Cos(AzimuthRad) * FMath::Cos(FMath::DegreesToRadians(Pitch)) * Distance,
			FMath::Sin(AzimuthRad) * FMath::Cos(FMath::DegreesToRadians(Pitch)) * Distance,
			FMath::Sin(FMath::DegreesToRadians(-Pitch)) * Distance  // negate pitch for upward offset
		);

		FVector CamPos = Center + CamOffset;
		// Look back at center
		FRotator LookAt = (Center - CamPos).Rotation();

		Views.Add({
			TEXT("perspective"),
			CamPos,
			LookAt,
			false,
			0.0f,
			60.0f,
			false
		});
	}

	// --- Capture all views ---

	TArray<TSharedPtr<FJsonValue>> ViewResults;
	int32 ViewsCaptured = 0;

	for (const FViewSpec& View : Views)
	{
		// Hide ceiling actors for floor_plan if requested
		TArray<AActor*> TempHidden;
		if (View.bHideCeiling && bHideCeiling)
		{
			TempHidden = HideCeilingActors(World, BuildingId);
		}

		FString FileName = FString::Printf(TEXT("%s_%s.png"), *BuildingId, *View.Type);
		FString FullPath = OutputDir / FileName;

		bool bOk = CaptureViewToPNG(
			World,
			View.Location,
			View.Rotation,
			Resolution,
			FullPath,
			View.bOrthographic,
			View.OrthoWidth,
			View.FOVAngle);

		// Restore hidden actors
		if (TempHidden.Num() > 0)
		{
			RestoreHiddenActors(TempHidden);
		}

		auto ViewObj = MakeShared<FJsonObject>();
		ViewObj->SetStringField(TEXT("type"), View.Type);
		ViewObj->SetStringField(TEXT("path"), FullPath);
		ViewObj->SetBoolField(TEXT("success"), bOk);

		if (!bOk)
		{
			UE_LOG(LogMonolithDebugView, Warning,
				TEXT("capture_building_views: Failed to capture '%s' view for building '%s'"),
				*View.Type, *BuildingId);
		}
		else
		{
			++ViewsCaptured;
		}

		ViewResults.Add(MakeShared<FJsonValueObject>(ViewObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("building_id"), BuildingId);
	Result->SetNumberField(TEXT("views_captured"), ViewsCaptured);
	Result->SetArrayField(TEXT("views"), ViewResults);

	// Include building bounds in result for reference
	auto BoundsObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> MinArr, MaxArr;
	MinArr.Add(MakeShared<FJsonValueNumber>(BuildingBounds.Min.X));
	MinArr.Add(MakeShared<FJsonValueNumber>(BuildingBounds.Min.Y));
	MinArr.Add(MakeShared<FJsonValueNumber>(BuildingBounds.Min.Z));
	MaxArr.Add(MakeShared<FJsonValueNumber>(BuildingBounds.Max.X));
	MaxArr.Add(MakeShared<FJsonValueNumber>(BuildingBounds.Max.Y));
	MaxArr.Add(MakeShared<FJsonValueNumber>(BuildingBounds.Max.Z));
	BoundsObj->SetArrayField(TEXT("min"), MinArr);
	BoundsObj->SetArrayField(TEXT("max"), MaxArr);
	Result->SetObjectField(TEXT("building_bounds"), BoundsObj);

	if (ViewsCaptured == 0)
	{
		return FMonolithActionResult::Error(TEXT("All view captures failed — check editor world and render state"));
	}

	return FMonolithActionResult::Success(Result);
}
