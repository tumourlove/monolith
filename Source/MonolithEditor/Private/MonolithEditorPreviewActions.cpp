// Copyright tumourlove. All Rights Reserved.

// =============================================================================
// MonolithEditorPreviewActions.cpp
//
// Phase 3 of plan: 2026-05-26-monolith-editor-preview-expansion.md.
//
// Two composite-capture editor:: actions, both producing PNG output:
//
//   editor::capture_material_grid   — render N materials side-by-side on
//                                      identical preview meshes in ONE scene,
//                                      ONE camera, ONE PNG. Shares lighting +
//                                      HDRI across all cells.
//   editor::capture_with_overlay    — render a static mesh with one of five
//                                      FEngineShowFlags overlays toggled on
//                                      (wireframe / normals / uv_density /
//                                      lightmap_density / shader_complexity).
//
// Both reuse the proven render-target + scene-capture pipeline from
// MonolithEditorActions.cpp::RenderAndSaveCapture. Declarations live in the
// public MonolithEditorActions.h header (Phase 3 block); registrations live
// in MonolithEditorActions.cpp::RegisterActions.
//
// UE 5.7 show-flag verification (offline source_query, plan Section 4 row
// "FEngineShowFlags"):
//   SetWireframe              — Engine/Source/Runtime/Engine/Public/ShowFlags.h
//   SetMeshEdges              — Engine/Source/Runtime/Engine/Public/ShowFlags.h:461
//   SetMeshUVDensityAccuracy  — Engine/Source/Runtime/Engine/Public/ShowFlags.h:501
//   SetLightMapDensity        — Engine/Source/Runtime/Engine/Public/ShowFlags.h:441
//   SetShaderComplexity       — Engine/Source/Runtime/Engine/Public/ShowFlags.h:431
//   (SetVisualizeMeshNormals does NOT exist in UE 5.7 — `normals` mode falls
//   back to SetMeshEdges which renders edges as a wireframe overlay. The
//   limitation is documented in the action description.)
// =============================================================================

#include "MonolithEditorActions.h"
#include "MonolithJsonUtils.h"

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"

#include "AdvancedPreviewScene.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "Materials/MaterialInterface.h"
#include "ShowFlags.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithPreviewActions, Log, All);

// ----- Local helpers ---------------------------------------------------------

namespace
{
	/** Resolve "plane" / "sphere" / "cube" to engine BasicShapes path. */
	static FString ResolvePreviewMeshPath(const FString& Kind)
	{
		if (Kind.Equals(TEXT("sphere"), ESearchCase::IgnoreCase))
		{
			return TEXT("/Engine/BasicShapes/Sphere");
		}
		if (Kind.Equals(TEXT("cube"), ESearchCase::IgnoreCase))
		{
			return TEXT("/Engine/BasicShapes/Cube");
		}
		return TEXT("/Engine/BasicShapes/Plane");
	}

	/** Parse optional {location, rotation, fov} camera object (or string-serialized variant). */
	static void ParseCameraObject(
		const TSharedPtr<FJsonObject>& Params,
		FVector& OutLocation,
		FRotator& OutRotation,
		float& OutFOV,
		bool& bOutCameraProvided)
	{
		bOutCameraProvided = false;
		if (!Params->HasField(TEXT("camera")))
		{
			return;
		}

		const TSharedPtr<FJsonObject>* CameraObj = nullptr;
		TSharedPtr<FJsonObject> ParsedCamera;

		if (!Params->TryGetObjectField(TEXT("camera"), CameraObj))
		{
			FString CameraStr = Params->GetStringField(TEXT("camera"));
			if (!CameraStr.IsEmpty())
			{
				ParsedCamera = FMonolithJsonUtils::Parse(CameraStr);
				CameraObj = &ParsedCamera;
			}
		}

		if (!CameraObj || !(*CameraObj).IsValid())
		{
			return;
		}

		bOutCameraProvided = true;

		if ((*CameraObj)->HasField(TEXT("location")))
		{
			const TArray<TSharedPtr<FJsonValue>>& Loc = (*CameraObj)->GetArrayField(TEXT("location"));
			if (Loc.Num() >= 3)
			{
				OutLocation = FVector(Loc[0]->AsNumber(), Loc[1]->AsNumber(), Loc[2]->AsNumber());
			}
		}
		if ((*CameraObj)->HasField(TEXT("rotation")))
		{
			const TArray<TSharedPtr<FJsonValue>>& Rot = (*CameraObj)->GetArrayField(TEXT("rotation"));
			if (Rot.Num() >= 3)
			{
				OutRotation = FRotator(Rot[0]->AsNumber(), Rot[1]->AsNumber(), Rot[2]->AsNumber());
			}
		}
		if ((*CameraObj)->HasField(TEXT("fov")))
		{
			OutFOV = (float)(*CameraObj)->GetNumberField(TEXT("fov"));
		}
	}

	/** Parse [w, h] array; returns true if a usable pair was extracted. */
	static bool ParseResolutionArray(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		int32& OutW,
		int32& OutH)
	{
		if (!Params->HasField(FieldName))
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>& Arr = Params->GetArrayField(FieldName);
		if (Arr.Num() < 2)
		{
			return false;
		}
		OutW = FMath::Max(1, (int32)Arr[0]->AsNumber());
		OutH = FMath::Max(1, (int32)Arr[1]->AsNumber());
		return true;
	}

	/** Build default timestamp-suffixed output path under Saved/Screenshots/Monolith/<Bucket>/. */
	static FString DefaultOutputPath(const TCHAR* Bucket)
	{
		const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		return FPaths::ProjectDir() / TEXT("Saved/Screenshots/Monolith") / Bucket /
			FString::Printf(TEXT("%s.png"), *Timestamp);
	}

	/** Resolve user-supplied or default output path, normalizing relative paths. */
	static FString ResolveOutputPath(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* DefaultBucket)
	{
		if (Params->HasField(TEXT("output_path")))
		{
			FString OutputPath = Params->GetStringField(TEXT("output_path"));
			if (FPaths::IsRelative(OutputPath))
			{
				OutputPath = FPaths::ProjectDir() / OutputPath;
			}
			return OutputPath;
		}
		return DefaultOutputPath(DefaultBucket);
	}
}

// =============================================================================
// editor::capture_material_grid
// =============================================================================

FMonolithActionResult FMonolithEditorActions::HandleCaptureMaterialGrid(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Params object is null"));
	}

	// material_paths array — required.
	const TArray<TSharedPtr<FJsonValue>>* MaterialPathsArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("material_paths"), MaterialPathsArr) || !MaterialPathsArr)
	{
		return FMonolithActionResult::Error(TEXT("material_paths array is required"));
	}
	if (MaterialPathsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("material_paths array is empty"));
	}
	if (MaterialPathsArr->Num() > 16)
	{
		return FMonolithActionResult::Error(TEXT("material_paths array exceeds 16-entry limit"));
	}

	// Resolve materials — log + skip any that fail to load.
	TArray<UMaterialInterface*> Materials;
	Materials.Reserve(MaterialPathsArr->Num());
	for (const TSharedPtr<FJsonValue>& Val : *MaterialPathsArr)
	{
		if (!Val.IsValid())
		{
			continue;
		}
		const FString Path = Val->AsString();
		if (Path.IsEmpty())
		{
			continue;
		}
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *Path);
		if (!Material)
		{
			UE_LOG(LogMonolithPreviewActions, Warning,
				TEXT("capture_material_grid: failed to load material '%s' — cell will be empty"), *Path);
			continue;
		}
		Materials.Add(Material);
	}

	if (Materials.Num() == 0)
	{
		return FMonolithActionResult::Error(
			TEXT("No materials successfully loaded from material_paths"));
	}

	// resolution — default 1024x1024.
	int32 ResX = 1024, ResY = 1024;
	ParseResolutionArray(Params, TEXT("resolution"), ResX, ResY);

	// columns — default ceil(sqrt(N)) per resolved open question #5.
	const int32 MaterialCount = Materials.Num();
	int32 Columns = (int32)FMath::CeilToInt32(FMath::Sqrt((float)MaterialCount));
	if (Params->HasField(TEXT("columns")))
	{
		double ColsD = (double)Columns;
		Params->TryGetNumberField(TEXT("columns"), ColsD);
		Columns = FMath::Max(1, (int32)ColsD);
	}
	const int32 Rows = (int32)FMath::CeilToInt32((float)MaterialCount / (float)Columns);

	// preview_mesh — default "sphere" for the grid (better material readout than plane).
	FString PreviewMeshKind = TEXT("sphere");
	if (Params->HasField(TEXT("preview_mesh")))
	{
		PreviewMeshKind = Params->GetStringField(TEXT("preview_mesh"));
	}
	const FString PreviewMeshPath = ResolvePreviewMeshPath(PreviewMeshKind);

	UStaticMesh* PreviewMesh = LoadObject<UStaticMesh>(nullptr, *PreviewMeshPath);
	if (!PreviewMesh)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load preview mesh: %s"), *PreviewMeshPath));
	}

	// Per-cell resolution (informational; the grid is one big capture, not stitched).
	const int32 CellW = FMath::Max(1, ResX / Columns);
	const int32 CellH = FMath::Max(1, ResY / Rows);

	// Grid layout in world space. 200 cm centre-to-centre — works for the
	// 100 cm engine sphere/cube/plane at unit scale with margin between cells.
	const float CellSpacing = 200.0f;
	const float TotalGridW = CellSpacing * (Columns - 1);
	const float TotalGridH = CellSpacing * (Rows - 1);
	const FVector GridOriginOffset(0.0f, -TotalGridW * 0.5f, TotalGridH * 0.5f);

	// Camera default: frame the whole grid from -X looking +X.
	// Distance derived from grid extents + FOV (60 deg default) so all cells fit.
	float FOV = 60.0f;
	FVector CameraLocation;
	FRotator CameraRotation;
	bool bCameraProvided = false;
	ParseCameraObject(Params, CameraLocation, CameraRotation, FOV, bCameraProvided);

	if (!bCameraProvided)
	{
		// Auto-frame: required half-extent is max(width, height) / 2 + cell radius (~100).
		const float HalfExtent = FMath::Max(TotalGridW, TotalGridH) * 0.5f + 120.0f;
		const float HalfFOVRad = FMath::DegreesToRadians(FOV * 0.5f);
		const float Distance = HalfExtent / FMath::Tan(HalfFOVRad);
		CameraLocation = FVector(-FMath::Max(300.0f, Distance + 100.0f), 0.0f, 0.0f);
		CameraRotation = FRotator(0.0f, 0.0f, 0.0f); // looking +X
	}

	// Output path.
	const FString OutputPath = ResolveOutputPath(Params, TEXT("CaptureMaterialGrid"));

	check(IsInGameThread());
	const double StartTime = FPlatformTime::Seconds();

	// One shared preview scene — the value-add: identical lighting + HDRI across all cells.
	TSharedPtr<FAdvancedPreviewScene> PreviewScene =
		MakeShareable(new FAdvancedPreviewScene(FPreviewScene::ConstructionValues()));
	PreviewScene->SetFloorVisibility(false);

	UWorld* World = PreviewScene->GetWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("PreviewScene has no UWorld"));
	}

	// Spawn one mesh component per resolved material at its grid position.
	TArray<UStaticMeshComponent*> CellComps;
	CellComps.Reserve(MaterialCount);
	for (int32 i = 0; i < MaterialCount; ++i)
	{
		const int32 Col = i % Columns;
		const int32 Row = i / Columns;

		// Y axis = left-right; Z axis = up-down. X is depth (camera axis).
		const FVector CellLocation = GridOriginOffset + FVector(
			0.0f,
			CellSpacing * Col,
			-CellSpacing * Row);

		UStaticMeshComponent* CellComp = NewObject<UStaticMeshComponent>(
			GetTransientPackage(), NAME_None, RF_Transient);
		CellComp->SetStaticMesh(PreviewMesh);
		CellComp->SetMaterial(0, Materials[i]);
		CellComp->SetRelativeLocation(CellLocation);
		PreviewScene->AddComponent(CellComp, CellComp->GetRelativeTransform());
		CellComps.Add(CellComp);
	}

	// One shared RT sized to the requested total resolution.
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	RT->InitAutoFormat(ResX, ResY);
	RT->ClearColor = FLinearColor(0.18f, 0.18f, 0.18f);
	RT->UpdateResourceImmediate(true);

	// One shared capture component framed to cover all cells.
	USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	CaptureComp->bTickInEditor = false;
	CaptureComp->SetComponentTickEnabled(false);
	CaptureComp->SetVisibility(true);
	CaptureComp->bCaptureEveryFrame = false;
	CaptureComp->bCaptureOnMovement = false;
	CaptureComp->TextureTarget = RT;
	CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
	CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
	CaptureComp->FOVAngle = FOV;
	CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

	CaptureComp->RegisterComponentWithWorld(World);
	CaptureComp->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

	// Single capture → readback → PNG via the proven helper.
	const bool bSuccess = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, OutputPath);

	// Cleanup.
	CaptureComp->TextureTarget = nullptr;
	CaptureComp->UnregisterComponent();
	for (UStaticMeshComponent* CellComp : CellComps)
	{
		PreviewScene->RemoveComponent(CellComp);
	}

	const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	if (!bSuccess)
	{
		return FMonolithActionResult::Error(TEXT("capture_material_grid: render or save failed"));
	}

	// Build result payload.
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("output_file"), OutputPath);
	Result->SetNumberField(TEXT("material_count"), MaterialCount);
	Result->SetNumberField(TEXT("columns"), Columns);
	Result->SetNumberField(TEXT("rows"), Rows);

	TSharedPtr<FJsonObject> CellRes = MakeShared<FJsonObject>();
	CellRes->SetNumberField(TEXT("width"), CellW);
	CellRes->SetNumberField(TEXT("height"), CellH);
	Result->SetObjectField(TEXT("cell_resolution"), CellRes);

	TSharedPtr<FJsonObject> TotalRes = MakeShared<FJsonObject>();
	TotalRes->SetNumberField(TEXT("width"), ResX);
	TotalRes->SetNumberField(TEXT("height"), ResY);
	Result->SetObjectField(TEXT("resolution"), TotalRes);

	Result->SetNumberField(TEXT("capture_time_ms"), ElapsedMs);

	return FMonolithActionResult::Success(Result);
}

// =============================================================================
// editor::capture_with_overlay
// =============================================================================

FMonolithActionResult FMonolithEditorActions::HandleCaptureWithOverlay(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Params object is null"));
	}

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString Mode = Params->GetStringField(TEXT("mode"));

	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}
	if (Mode.IsEmpty())
	{
		return FMonolithActionResult::Error(
			TEXT("mode is required (wireframe | normals | uv_density | lightmap_density | shader_complexity)"));
	}

	// Validate mode upfront so we error before allocating any RHI resources.
	const bool bModeWireframe       = Mode.Equals(TEXT("wireframe"), ESearchCase::IgnoreCase);
	const bool bModeNormals         = Mode.Equals(TEXT("normals"), ESearchCase::IgnoreCase);
	const bool bModeUVDensity       = Mode.Equals(TEXT("uv_density"), ESearchCase::IgnoreCase);
	const bool bModeLightmapDensity = Mode.Equals(TEXT("lightmap_density"), ESearchCase::IgnoreCase);
	const bool bModeShaderComplex   = Mode.Equals(TEXT("shader_complexity"), ESearchCase::IgnoreCase);

	if (!(bModeWireframe || bModeNormals || bModeUVDensity || bModeLightmapDensity || bModeShaderComplex))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unsupported mode '%s' (supported: wireframe, normals, uv_density, lightmap_density, shader_complexity)"),
			*Mode));
	}

	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
	if (!Mesh)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load static mesh: %s"), *AssetPath));
	}

	int32 ResX = 512, ResY = 512;
	ParseResolutionArray(Params, TEXT("resolution"), ResX, ResY);

	// Camera default: -X 200 units, looking +X.
	FVector CameraLocation(200.0f, 0.0f, 100.0f);
	FRotator CameraRotation(0.0f, 180.0f, 0.0f);
	float FOV = 60.0f;
	bool bCameraProvided = false;
	ParseCameraObject(Params, CameraLocation, CameraRotation, FOV, bCameraProvided);

	const FString OutputPath = ResolveOutputPath(Params, TEXT("CaptureWithOverlay"));

	check(IsInGameThread());
	const double StartTime = FPlatformTime::Seconds();

	TSharedPtr<FAdvancedPreviewScene> PreviewScene =
		MakeShareable(new FAdvancedPreviewScene(FPreviewScene::ConstructionValues()));
	PreviewScene->SetFloorVisibility(false);

	UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(
		GetTransientPackage(), NAME_None, RF_Transient);
	MeshComp->SetStaticMesh(Mesh);
	PreviewScene->AddComponent(MeshComp, MeshComp->GetRelativeTransform());

	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	RT->InitAutoFormat(ResX, ResY);
	RT->ClearColor = FLinearColor(0.18f, 0.18f, 0.18f);
	RT->UpdateResourceImmediate(true);

	USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	CaptureComp->bTickInEditor = false;
	CaptureComp->SetComponentTickEnabled(false);
	CaptureComp->SetVisibility(true);
	CaptureComp->bCaptureEveryFrame = false;
	CaptureComp->bCaptureOnMovement = false;
	CaptureComp->TextureTarget = RT;
	CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
	CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
	CaptureComp->FOVAngle = FOV;
	CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

	// Toggle the requested show flag BEFORE first CaptureScene call. Mid-frame
	// show-flag changes are not deterministic (plan Section 8 gotcha).
	if (bModeWireframe)
	{
		CaptureComp->ShowFlags.SetWireframe(true);
	}
	else if (bModeNormals)
	{
		// UE 5.7 has no FEngineShowFlags::SetVisualizeMeshNormals — verified
		// offline via source_query. SetMeshEdges is the closest functional
		// approximation (renders mesh edges as a wireframe overlay on top of
		// the lit pass). Documented in the file header.
		CaptureComp->ShowFlags.SetMeshEdges(true);
	}
	else if (bModeUVDensity)
	{
		CaptureComp->ShowFlags.SetMeshUVDensityAccuracy(true);
	}
	else if (bModeLightmapDensity)
	{
		CaptureComp->ShowFlags.SetLightMapDensity(true);
	}
	else if (bModeShaderComplex)
	{
		CaptureComp->ShowFlags.SetShaderComplexity(true);
	}

	UWorld* World = PreviewScene->GetWorld();
	CaptureComp->RegisterComponentWithWorld(World);
	CaptureComp->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

	const bool bSuccess = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, OutputPath);

	// Cleanup.
	CaptureComp->TextureTarget = nullptr;
	CaptureComp->UnregisterComponent();
	PreviewScene->RemoveComponent(MeshComp);

	const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	if (!bSuccess)
	{
		return FMonolithActionResult::Error(TEXT("capture_with_overlay: render or save failed"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("output_file"), OutputPath);
	Result->SetStringField(TEXT("mode"), Mode);

	TSharedPtr<FJsonObject> ResObj = MakeShared<FJsonObject>();
	ResObj->SetNumberField(TEXT("width"), ResX);
	ResObj->SetNumberField(TEXT("height"), ResY);
	Result->SetObjectField(TEXT("resolution"), ResObj);

	Result->SetNumberField(TEXT("capture_time_ms"), ElapsedMs);

	return FMonolithActionResult::Success(Result);
}
