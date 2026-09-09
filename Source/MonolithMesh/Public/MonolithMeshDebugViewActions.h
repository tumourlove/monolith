#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MonolithToolRegistry.h"
#include "MonolithMeshSpatialRegistry.h"

/**
 * SP9: Daredevil Debug View — 6 actions for inspecting procedural buildings.
 *
 * toggle_section_view      — Hide actors above a Z height for section-cut inspection
 * toggle_ceiling_visibility — Show/hide actors tagged BuildingCeiling/BuildingRoof
 * capture_floor_plan       — Orthographic top-down scene capture to PNG
 * highlight_room           — Spawn translucent overlay box at room bounds
 * save_camera_bookmark     — Save editor viewport camera to JSON
 * load_camera_bookmark     — Restore editor viewport camera from JSON
 * capture_building_views   — Multi-angle diagnostic captures (floor plan + 4 elevations + perspective)
 */
class FMonolithMeshDebugViewActions
{
public:
	/** Register all 7 debug view actions */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// ---- Action Handlers ----

	static FMonolithActionResult ToggleSectionView(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ToggleCeilingVisibility(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CaptureFloorPlan(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HighlightRoom(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SaveCameraBookmark(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult LoadCameraBookmark(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CaptureBuildingViews(const TSharedPtr<FJsonObject>& Params);

	// ---- Helpers ----

	/** Get the bookmarks save directory */
	static FString GetBookmarkDirectory();

	/** Get the captures save directory */
	static FString GetCapturesDirectory();

	/** Ensure a directory exists */
	static bool EnsureDirectory(const FString& Path);

	/** Compute full building bounds from all rooms across all floors */
	static bool ComputeBuildingBounds(const struct FSpatialBlock& Block, const struct FSpatialBuilding& Building, FBox& OutBounds);

	/** Temporarily hide ceiling/roof actors, returns list for restoration */
	static TArray<AActor*> HideCeilingActors(UWorld* World, const FString& BuildingId);

	/** Restore previously hidden actors */
	static void RestoreHiddenActors(const TArray<AActor*>& Actors);

	/** Capture a single view to PNG. Returns true on success. */
	static bool CaptureViewToPNG(
		UWorld* World,
		const FVector& CameraLocation,
		const FRotator& CameraRotation,
		int32 Resolution,
		const FString& OutputPath,
		bool bOrthographic,
		float OrthoWidth = 0.0f,
		float FOVAngle = 60.0f);
};
