#include "MonolithMeshLightingCapture.h"
#include "UObject/Package.h"

#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/LightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Engine/Light.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/DirectionalLight.h"
#include "Engine/RectLight.h"
#include "EngineUtils.h"
#include "CollisionQueryParams.h"
#include "ShowFlags.h"
#include "RenderingThread.h"
#include "TextureResource.h"

// ============================================================================
// Scene Capture Helpers
// ============================================================================

UTextureRenderTarget2D* MonolithLightingCapture::CreateTransientRT(int32 Width, int32 Height)
{
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	RT->RenderTargetFormat = RTF_RGBA16f;  // HDR for luminance
	RT->InitCustomFormat(Width, Height, PF_FloatRGBA, false);
	RT->ClearColor = FLinearColor::Black;
	RT->bAutoGenerateMips = false;
	RT->UpdateResourceImmediate(true);
	return RT;
}

USceneCaptureComponent2D* MonolithLightingCapture::CreateLightingCapture(UWorld* World, UTextureRenderTarget2D* RT)
{
	USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	Capture->bTickInEditor = false;
	Capture->SetComponentTickEnabled(false);
	Capture->SetVisibility(true);
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->bAlwaysPersistRenderingState = true;
	Capture->TextureTarget = RT;
	Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
	Capture->ProjectionType = ECameraProjectionMode::Perspective;
	Capture->FOVAngle = 90.0f;
	Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

	// Show flags: lighting only — disable post-process auto-exposure, bloom, etc.
	// We want raw illumination values, not tone-mapped
	Capture->ShowFlags.SetPostProcessing(false);
	Capture->ShowFlags.SetTonemapper(false);
	Capture->ShowFlags.SetEyeAdaptation(false);
	Capture->ShowFlags.SetBloom(false);
	Capture->ShowFlags.SetMotionBlur(false);
	Capture->ShowFlags.SetFog(false);
	Capture->ShowFlags.SetVolumetricFog(false);
	Capture->ShowFlags.SetAtmosphere(false);
	// Keep lighting-related flags ON
	Capture->ShowFlags.SetLighting(true);
	Capture->ShowFlags.SetGlobalIllumination(true);

	// Manual exposure: fixed so we get consistent, raw luminance values
	Capture->PostProcessSettings.bOverride_AutoExposureBias = true;
	Capture->PostProcessSettings.AutoExposureBias = 0.0f;
	Capture->PostProcessSettings.bOverride_AutoExposureMethod = true;
	Capture->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
	// Disable physical camera exposure to get linear luminance output
	Capture->PostProcessSettings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	Capture->PostProcessSettings.AutoExposureApplyPhysicalCameraExposure = false;
	Capture->PostProcessBlendWeight = 1.0f;

	Capture->RegisterComponentWithWorld(World);
	return Capture;
}

USceneCaptureComponent2D* MonolithLightingCapture::CreateOrthoLightingCapture(UWorld* World, UTextureRenderTarget2D* RT, float OrthoWidth)
{
	USceneCaptureComponent2D* Capture = CreateLightingCapture(World, RT);
	Capture->ProjectionType = ECameraProjectionMode::Orthographic;
	Capture->OrthoWidth = OrthoWidth;
	return Capture;
}

bool MonolithLightingCapture::CaptureAtPoint(USceneCaptureComponent2D* Capture, const FVector& Location,
	const FRotator& Rotation, TArray<FLinearColor>& OutPixels)
{
	if (!Capture || !Capture->TextureTarget)
	{
		return false;
	}

	Capture->SetWorldLocationAndRotation(Location, Rotation);
	Capture->CaptureScene();

	return ReadPixelsHDR(Capture->TextureTarget, OutPixels);
}

bool MonolithLightingCapture::ReadPixelsHDR(UTextureRenderTarget2D* RT, TArray<FLinearColor>& OutPixels)
{
	if (!RT)
	{
		return false;
	}

	FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource();
	if (!RTResource)
	{
		return false;
	}

	// ReadLinearColorPixels handles HDR formats and flushes rendering commands
	return RTResource->ReadLinearColorPixels(OutPixels);
}

void MonolithLightingCapture::CleanupCapture(USceneCaptureComponent2D* Capture)
{
	if (Capture)
	{
		Capture->TextureTarget = nullptr;
		if (Capture->IsRegistered())
		{
			Capture->UnregisterComponent();
		}
	}
}

// ============================================================================
// Luminance Math
// ============================================================================

float MonolithLightingCapture::ComputeLuminance(const FLinearColor& Color)
{
	// Rec. 709 luminance coefficients
	return 0.2126f * Color.R + 0.7152f * Color.G + 0.0722f * Color.B;
}

float MonolithLightingCapture::AverageLuminance(const TArray<FLinearColor>& Pixels)
{
	if (Pixels.Num() == 0)
	{
		return 0.0f;
	}

	double Sum = 0.0;
	for (const FLinearColor& P : Pixels)
	{
		Sum += ComputeLuminance(P);
	}
	return static_cast<float>(Sum / Pixels.Num());
}

FLinearColor MonolithLightingCapture::AverageColor(const TArray<FLinearColor>& Pixels)
{
	if (Pixels.Num() == 0)
	{
		return FLinearColor::Black;
	}

	double R = 0.0, G = 0.0, B = 0.0;
	for (const FLinearColor& P : Pixels)
	{
		R += P.R;
		G += P.G;
		B += P.B;
	}
	const double Inv = 1.0 / Pixels.Num();
	return FLinearColor(static_cast<float>(R * Inv), static_cast<float>(G * Inv), static_cast<float>(B * Inv), 1.0f);
}

float MonolithLightingCapture::EstimateColorTemperature(const FLinearColor& Color)
{
	// McCamy's approximation: estimate CCT from chromaticity coordinates
	// First convert to CIE XYZ
	const float X = 0.4124f * Color.R + 0.3576f * Color.G + 0.1805f * Color.B;
	const float Y = 0.2126f * Color.R + 0.7152f * Color.G + 0.0722f * Color.B;
	const float Z = 0.0193f * Color.R + 0.1192f * Color.G + 0.9505f * Color.B;

	const float Sum = X + Y + Z;
	if (Sum < 1e-6f)
	{
		return 6500.0f; // Default for black
	}

	const float x = X / Sum;
	const float y = Y / Sum;

	// McCamy's formula
	const float n = (x - 0.3320f) / (0.1858f - y);
	float CCT = 449.0f * n * n * n + 3525.0f * n * n + 6823.3f * n + 5520.33f;
	return FMath::Clamp(CCT, 1000.0f, 40000.0f);
}

// ============================================================================
// Analytic Light Sampling
// ============================================================================

TArray<MonolithLightingCapture::FLightInfo> MonolithLightingCapture::GatherLights(UWorld* World)
{
	TArray<FLightInfo> Lights;
	if (!World)
	{
		return Lights;
	}

	for (TActorIterator<ALight> It(World); It; ++It)
	{
		ALight* LightActor = *It;
		ULightComponent* LightComp = LightActor->GetLightComponent();
		if (!LightComp || !LightComp->IsVisible())
		{
			continue;
		}

		FLightInfo Info;
		Info.Name = LightActor->GetActorLabel();
		if (Info.Name.IsEmpty())
		{
			Info.Name = LightActor->GetName();
		}
		Info.Location = LightActor->GetActorLocation();
		Info.Rotation = LightActor->GetActorRotation();
		Info.Intensity = LightComp->Intensity;
		Info.Color = LightComp->GetLightColor();
		Info.bCastsShadows = LightComp->CastShadows;

		if (LightComp->IsA<UPointLightComponent>())
		{
			UPointLightComponent* PLC = Cast<UPointLightComponent>(LightComp);
			Info.Type = TEXT("Point");
			Info.AttenuationRadius = PLC->AttenuationRadius;
		}
		else if (LightComp->IsA<USpotLightComponent>())
		{
			USpotLightComponent* SLC = Cast<USpotLightComponent>(LightComp);
			Info.Type = TEXT("Spot");
			Info.AttenuationRadius = SLC->AttenuationRadius;
		}
		else if (LightComp->IsA<UDirectionalLightComponent>())
		{
			Info.Type = TEXT("Directional");
			Info.AttenuationRadius = 0.0f; // Infinite
		}
		else if (LightComp->IsA<URectLightComponent>())
		{
			URectLightComponent* RLC = Cast<URectLightComponent>(LightComp);
			Info.Type = TEXT("Rect");
			Info.AttenuationRadius = RLC->AttenuationRadius;
		}
		else
		{
			Info.Type = TEXT("Unknown");
		}

		// Color temperature from the light component if using temperature
		if (LightComp->bUseTemperature)
		{
			Info.ColorTemperature = LightComp->Temperature;
		}
		else
		{
			Info.ColorTemperature = EstimateColorTemperature(Info.Color);
		}

		Lights.Add(MoveTemp(Info));
	}

	return Lights;
}

TArray<MonolithLightingCapture::FLightInfo> MonolithLightingCapture::GatherLightsInRegion(UWorld* World, const FBox& Region)
{
	TArray<FLightInfo> All = GatherLights(World);
	TArray<FLightInfo> InRegion;

	for (const FLightInfo& L : All)
	{
		if (L.Type == TEXT("Directional"))
		{
			// Directional lights always contribute
			InRegion.Add(L);
		}
		else if (Region.IsInside(L.Location) ||
			FVector::Dist(L.Location, Region.GetCenter()) < L.AttenuationRadius + Region.GetExtent().GetMax())
		{
			InRegion.Add(L);
		}
	}

	return InRegion;
}

float MonolithLightingCapture::ComputeAnalyticLuminance(UWorld* World, const FVector& Point,
	const TArray<FLightInfo>& Lights, int32& OutDominantLight, bool bTraceShadows)
{
	// Ambient fraction for shadowed lights — a small amount leaks through
	// to approximate indirect bounce even in full shadow
	constexpr float ShadowAmbientFraction = 0.05f;

	OutDominantLight = -1;
	float TotalLuminance = 0.0f;
	float MaxContribution = 0.0f;

	FCollisionQueryParams ShadowTraceParams(SCENE_QUERY_STAT(MonolithAnalyticShadow), true);

	for (int32 i = 0; i < Lights.Num(); ++i)
	{
		const FLightInfo& L = Lights[i];
		float Contribution = 0.0f;

		if (L.Type == TEXT("Directional"))
		{
			// Directional light: constant intensity (no falloff)
			// Use dot product with light direction for surface-facing approximation
			Contribution = L.Intensity * 0.01f; // Scale down — directional intensity is in lux
		}
		else
		{
			const float Dist = FVector::Dist(Point, L.Location);
			if (Dist > L.AttenuationRadius && L.AttenuationRadius > 0.0f)
			{
				continue; // Outside attenuation radius
			}

			// UE uses inverse-square with smooth attenuation falloff
			// Simplified: I / (d^2 + 1) * attenuation_falloff
			const float DistSq = FMath::Max(Dist * Dist, 1.0f);
			float Falloff = L.Intensity / DistSq;

			// Smooth attenuation at radius boundary (UE's windowing function)
			if (L.AttenuationRadius > 0.0f)
			{
				const float Ratio = Dist / L.AttenuationRadius;
				const float RatioSq = Ratio * Ratio;
				const float Window = FMath::Square(FMath::Max(0.0f, 1.0f - RatioSq));
				Falloff *= Window;
			}

			// Spot light cone attenuation
			if (L.Type == TEXT("Spot"))
			{
				const FVector LightDir = L.Rotation.Vector();
				const FVector ToPoint = (Point - L.Location).GetSafeNormal();
				const float CosAngle = FVector::DotProduct(LightDir, ToPoint);
				// Simple cone falloff (approximation — real UE uses inner/outer cone)
				Falloff *= FMath::Max(0.0f, CosAngle);
			}

			Contribution = Falloff;
		}

		// Weight by light color luminance
		float ColorLum = ComputeLuminance(L.Color);
		Contribution *= FMath::Max(ColorLum, 0.01f);

		// Shadow trace: if geometry blocks the path from point to light, reduce
		// contribution to a small ambient fraction instead of full value
		if (bTraceShadows && World && Contribution > 0.0f)
		{
			FVector LightPos;
			if (L.Type == TEXT("Directional"))
			{
				// Trace in the reverse of the light direction, far enough to catch occluders
				LightPos = Point - L.Rotation.Vector() * 50000.0f;
			}
			else
			{
				LightPos = L.Location;
			}

			FHitResult ShadowHit;
			if (World->LineTraceSingleByChannel(ShadowHit, Point, LightPos, ECC_Visibility, ShadowTraceParams))
			{
				// Light is blocked — reduce to ambient leak
				Contribution *= ShadowAmbientFraction;
			}
		}

		TotalLuminance += Contribution;
		if (Contribution > MaxContribution)
		{
			MaxContribution = Contribution;
			OutDominantLight = i;
		}
	}

	return TotalLuminance;
}

bool MonolithLightingCapture::IsInShadow(UWorld* World, const FVector& Point, const FVector& LightLocation)
{
	if (!World)
	{
		return true;
	}

	FHitResult Hit;
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MonolithShadowTrace), true);
	// Trace from point toward light — if hit, point is in shadow
	return World->LineTraceSingleByChannel(Hit, Point, LightLocation, ECC_Visibility, QueryParams);
}

// ============================================================================
// Atlas Capture
// ============================================================================

bool MonolithLightingCapture::CapturePointsAtlas(UWorld* World, const TArray<FVector>& Points, int32 TileSize,
	int32 MaxTilesPerRow, TArray<FLinearColor>& OutPixels, int32& OutAtlasWidth, int32& OutAtlasHeight)
{
	if (!World || Points.Num() == 0)
	{
		return false;
	}

	const int32 NumPoints = Points.Num();
	const int32 TilesPerRow = FMath::Min(MaxTilesPerRow, NumPoints);
	const int32 NumRows = FMath::DivideAndRoundUp(NumPoints, TilesPerRow);
	OutAtlasWidth = TilesPerRow * TileSize;
	OutAtlasHeight = NumRows * TileSize;

	// For atlas approach: capture each point individually into a small RT,
	// then composite into the output array. Scene capture doesn't support
	// sub-region rendering, so we capture per-point but batch the ReadPixels.
	// Actually — we'll capture each point with its own 8x8 RT and accumulate.
	// The real optimization is using one RT and re-capturing + reading per batch.

	// Simpler approach: single RT per point, accumulate results
	UTextureRenderTarget2D* RT = CreateTransientRT(TileSize, TileSize);
	USceneCaptureComponent2D* Capture = CreateLightingCapture(World, RT);

	OutPixels.SetNum(OutAtlasWidth * OutAtlasHeight);
	// Initialize to black
	for (FLinearColor& P : OutPixels)
	{
		P = FLinearColor::Black;
	}

	bool bAnySuccess = false;
	for (int32 i = 0; i < NumPoints; ++i)
	{
		TArray<FLinearColor> TilePixels;
		// Capture looking downward for consistency
		if (CaptureAtPoint(Capture, Points[i], FRotator(-90.0f, 0.0f, 0.0f), TilePixels))
		{
			bAnySuccess = true;
			// Copy into atlas
			const int32 TileCol = i % TilesPerRow;
			const int32 TileRow = i / TilesPerRow;
			for (int32 y = 0; y < TileSize && y < TilePixels.Num() / TileSize; ++y)
			{
				for (int32 x = 0; x < TileSize; ++x)
				{
					const int32 SrcIdx = y * TileSize + x;
					const int32 DstIdx = (TileRow * TileSize + y) * OutAtlasWidth + (TileCol * TileSize + x);
					if (SrcIdx < TilePixels.Num() && DstIdx < OutPixels.Num())
					{
						OutPixels[DstIdx] = TilePixels[SrcIdx];
					}
				}
			}
		}
	}

	CleanupCapture(Capture);
	// RT is transient, will be GC'd
	return bAnySuccess;
}

float MonolithLightingCapture::ExtractTileLuminance(const TArray<FLinearColor>& AtlasPixels, int32 AtlasWidth,
	int32 TileIndex, int32 TileSize, int32 TilesPerRow)
{
	const int32 TileCol = TileIndex % TilesPerRow;
	const int32 TileRow = TileIndex / TilesPerRow;
	double Sum = 0.0;
	int32 Count = 0;

	for (int32 y = 0; y < TileSize; ++y)
	{
		for (int32 x = 0; x < TileSize; ++x)
		{
			const int32 Idx = (TileRow * TileSize + y) * AtlasWidth + (TileCol * TileSize + x);
			if (Idx < AtlasPixels.Num())
			{
				Sum += ComputeLuminance(AtlasPixels[Idx]);
				++Count;
			}
		}
	}

	return Count > 0 ? static_cast<float>(Sum / Count) : 0.0f;
}

FLinearColor MonolithLightingCapture::ExtractTileColor(const TArray<FLinearColor>& AtlasPixels, int32 AtlasWidth,
	int32 TileIndex, int32 TileSize, int32 TilesPerRow)
{
	const int32 TileCol = TileIndex % TilesPerRow;
	const int32 TileRow = TileIndex / TilesPerRow;
	double R = 0.0, G = 0.0, B = 0.0;
	int32 Count = 0;

	for (int32 y = 0; y < TileSize; ++y)
	{
		for (int32 x = 0; x < TileSize; ++x)
		{
			const int32 Idx = (TileRow * TileSize + y) * AtlasWidth + (TileCol * TileSize + x);
			if (Idx < AtlasPixels.Num())
			{
				R += AtlasPixels[Idx].R;
				G += AtlasPixels[Idx].G;
				B += AtlasPixels[Idx].B;
				++Count;
			}
		}
	}

	if (Count == 0) return FLinearColor::Black;
	const double Inv = 1.0 / Count;
	return FLinearColor(static_cast<float>(R * Inv), static_cast<float>(G * Inv), static_cast<float>(B * Inv), 1.0f);
}

// ============================================================================
// Coverage Map
// ============================================================================

MonolithLightingCapture::FCoverageResult MonolithLightingCapture::CaptureCoverageMap(
	UWorld* World, const FVector& Center, const FVector& Extent,
	int32 Resolution, float DarkThreshold, float BrightThreshold)
{
	FCoverageResult Result;
	if (!World)
	{
		return Result;
	}

	Resolution = FMath::Clamp(Resolution, 16, 256);
	const float OrthoWidth = Extent.X * 2.0f;

	UTextureRenderTarget2D* RT = CreateTransientRT(Resolution, Resolution);
	USceneCaptureComponent2D* Capture = CreateOrthoLightingCapture(World, RT, OrthoWidth);

	// Position above center, looking down
	const FVector CapturePos = FVector(Center.X, Center.Y, Center.Z + Extent.Z + 500.0f);
	Capture->SetWorldLocationAndRotation(CapturePos, FRotator(-90.0f, 0.0f, 0.0f));
	Capture->OrthoWidth = FMath::Max(Extent.X, Extent.Y) * 2.0f;
	Capture->CaptureScene();

	TArray<FLinearColor> Pixels;
	if (!ReadPixelsHDR(RT, Pixels))
	{
		CleanupCapture(Capture);
		return Result;
	}

	Result.GridWidth = Resolution;
	Result.GridHeight = Resolution;
	Result.LuminanceGrid.SetNum(Resolution * Resolution);

	int32 LitCount = 0, ShadowCount = 0, DarkCount = 0;
	const int32 Total = Resolution * Resolution;

	for (int32 i = 0; i < FMath::Min(Pixels.Num(), Total); ++i)
	{
		float Lum = ComputeLuminance(Pixels[i]);
		Result.LuminanceGrid[i] = Lum;

		if (Lum >= BrightThreshold)
		{
			++LitCount;
		}
		else if (Lum >= DarkThreshold)
		{
			++ShadowCount;
		}
		else
		{
			++DarkCount;
		}
	}

	const float InvTotal = Total > 0 ? 100.0f / Total : 0.0f;
	Result.PercentLit = LitCount * InvTotal;
	Result.PercentShadow = ShadowCount * InvTotal;
	Result.PercentDark = DarkCount * InvTotal;

	CleanupCapture(Capture);
	return Result;
}

// ============================================================================
// Dark Zone Flood Fill
// ============================================================================

TArray<MonolithLightingCapture::FDarkZone> MonolithLightingCapture::FloodFillDarkZones(
	const TArray<float>& Grid, int32 GridW, int32 GridH,
	float Threshold, const FVector& WorldMin, float CellSize, int32 MinCells)
{
	TArray<FDarkZone> Zones;
	TArray<bool> Visited;
	Visited.SetNumZeroed(GridW * GridH);

	for (int32 StartIdx = 0; StartIdx < GridW * GridH; ++StartIdx)
	{
		if (Visited[StartIdx] || Grid[StartIdx] >= Threshold)
		{
			continue;
		}

		// Flood fill from this cell
		TArray<int32> Stack;
		TArray<int32> Region;
		Stack.Push(StartIdx);
		Visited[StartIdx] = true;

		double LumSum = 0.0;

		while (Stack.Num() > 0)
		{
			const int32 Idx = Stack.Pop(EAllowShrinking::No);
			Region.Add(Idx);
			LumSum += Grid[Idx];

			const int32 CX = Idx % GridW;
			const int32 CY = Idx / GridW;

			// 4-connected neighbors
			static const int32 DX[] = {-1, 1, 0, 0};
			static const int32 DY[] = {0, 0, -1, 1};
			for (int32 D = 0; D < 4; ++D)
			{
				const int32 NX = CX + DX[D];
				const int32 NY = CY + DY[D];
				if (NX < 0 || NX >= GridW || NY < 0 || NY >= GridH)
				{
					continue;
				}
				const int32 NIdx = NY * GridW + NX;
				if (!Visited[NIdx] && Grid[NIdx] < Threshold)
				{
					Visited[NIdx] = true;
					Stack.Push(NIdx);
				}
			}
		}

		if (Region.Num() < MinCells)
		{
			continue;
		}

		FDarkZone Zone;
		Zone.CellCount = Region.Num();
		Zone.AreaSqCm = Region.Num() * CellSize * CellSize;
		Zone.AverageLuminance = static_cast<float>(LumSum / Region.Num());

		// Compute world-space center
		double SumX = 0.0, SumY = 0.0;
		for (int32 Idx : Region)
		{
			SumX += (Idx % GridW) * CellSize + CellSize * 0.5;
			SumY += (Idx / GridW) * CellSize + CellSize * 0.5;
		}
		Zone.WorldCenter = FVector(
			WorldMin.X + static_cast<float>(SumX / Region.Num()),
			WorldMin.Y + static_cast<float>(SumY / Region.Num()),
			WorldMin.Z);

		Zones.Add(MoveTemp(Zone));
	}

	return Zones;
}
