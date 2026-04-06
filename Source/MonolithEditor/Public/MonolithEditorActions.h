#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"
#include "Misc/OutputDevice.h"
#include "Components/SceneCaptureComponent2D.h" // ESceneCaptureSource

struct FMonolithLogEntry
{
	double Timestamp;
	FName Category;
	ELogVerbosity::Type Verbosity;
	FString Message;
};

class FMonolithLogCapture : public FOutputDevice
{
public:
	static constexpr int32 MaxEntries = 10000;

	void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;

	TArray<FMonolithLogEntry> GetRecentEntries(int32 Count) const;
	TArray<FMonolithLogEntry> SearchEntries(const FString& Pattern, const FString& CategoryFilter, ELogVerbosity::Type MaxVerbosity, int32 Limit) const;
	TArray<FMonolithLogEntry> GetEntriesSince(double SinceTimestamp, const TArray<FName>& CategoryFilter, ELogVerbosity::Type MaxVerbosity, int32 Limit) const;
	TArray<FString> GetActiveCategories() const;

	int32 GetCountByVerbosity(ELogVerbosity::Type Verbosity) const;
	int32 GetTotalCount() const;
	int32 CountErrorsSince(double SinceTimestamp) const;

private:
	mutable FCriticalSection Lock;
	TArray<FMonolithLogEntry> RingBuffer;
	int32 WriteIndex = 0;
	bool bWrapped = false;

	int32 TotalFatal = 0;
	int32 TotalError = 0;
	int32 TotalWarning = 0;
	int32 TotalLog = 0;
	int32 TotalVerbose = 0;
};

class FMonolithEditorActions
{
public:
	static void RegisterActions(FMonolithLogCapture* LogCapture);
	static void InitLiveCodingDelegate();

	static FMonolithActionResult HandleTriggerBuild(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetBuildErrors(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetBuildStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetBuildSummary(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSearchBuildOutput(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetCompileOutput(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetRecentLogs(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSearchLogs(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleTailLog(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetLogCategories(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetLogStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetCrashContext(const TSharedPtr<FJsonObject>& Params);

	// --- Capture actions ---
	static FMonolithActionResult HandleCaptureScenePreview(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCaptureSequenceFrames(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCaptureSystemGif(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleImportTexture(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetViewportInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleStitchFlipbook(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDeleteAssets(const TSharedPtr<FJsonObject>& Params);

	static void OnLiveCodingPatchComplete();

private:
	static FMonolithLogCapture* CachedLogCapture;

	static double LastCompileTimestamp;
	static FString LastCompileResult;
	static bool bIsCompiling;
	static bool bPatchApplied;
	static double LastCompileEndTimestamp;

	// Capture helpers
	static bool CaptureNiagaraFrame(
		class UNiagaraSystem* System, float SeekTime,
		const FVector& CameraLocation, const FRotator& CameraRotation, float FOV,
		int32 ResX, int32 ResY, const FString& OutputPath,
		ESceneCaptureSource CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR);

	static bool CaptureMaterialFrame(
		class UMaterialInterface* Material, const FString& MeshType,
		const FVector& CameraLocation, const FRotator& CameraRotation, float FOV,
		int32 ResX, int32 ResY, const FString& OutputPath,
		ESceneCaptureSource CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR,
		float UVTiling = 1.0f,
		const FLinearColor& BackgroundColor = FLinearColor(0.18f, 0.18f, 0.18f));

	static bool RenderAndSaveCapture(
		class USceneCaptureComponent2D* CaptureComp,
		class UTextureRenderTarget2D* RT,
		int32 ResX, int32 ResY, const FString& OutputPath);
};
