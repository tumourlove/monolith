#include "MonolithEditorActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "EditorAssetLibrary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/App.h"

#if PLATFORM_WINDOWS
#include "ILiveCodingModule.h"
#endif

// Capture action includes
#include "ProceduralMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "AdvancedPreviewScene.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraWorldManager.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "ImageUtils.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AutomatedAssetImportData.h"
#include "Engine/Texture2D.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "TextureResource.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/SavePackage.h"
#include "LevelEditorViewport.h"
#include "PixelFormat.h"
#include "ObjectTools.h"

// --- Compile state ---

FMonolithLogCapture* FMonolithEditorActions::CachedLogCapture = nullptr;
double FMonolithEditorActions::LastCompileTimestamp = 0.0;
FString FMonolithEditorActions::LastCompileResult = TEXT("none");
bool FMonolithEditorActions::bIsCompiling = false;
bool FMonolithEditorActions::bPatchApplied = false;
double FMonolithEditorActions::LastCompileEndTimestamp = 0.0;

// --- Log capture ---

void FMonolithLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	FScopeLock ScopeLock(&Lock);

	FMonolithLogEntry Entry;
	Entry.Timestamp = FPlatformTime::Seconds();
	Entry.Category = Category;
	Entry.Verbosity = Verbosity;
	Entry.Message = V;

	if (RingBuffer.Num() < MaxEntries)
	{
		RingBuffer.Add(MoveTemp(Entry));
	}
	else
	{
		RingBuffer[WriteIndex] = MoveTemp(Entry);
		bWrapped = true;
	}
	WriteIndex = (WriteIndex + 1) % MaxEntries;

	switch (Verbosity)
	{
	case ELogVerbosity::Fatal: ++TotalFatal; break;
	case ELogVerbosity::Error: ++TotalError; break;
	case ELogVerbosity::Warning: ++TotalWarning; break;
	case ELogVerbosity::Display:
	case ELogVerbosity::Log: ++TotalLog; break;
	case ELogVerbosity::Verbose:
	case ELogVerbosity::VeryVerbose: ++TotalVerbose; break;
	default: break;
	}
}

TArray<FMonolithLogEntry> FMonolithLogCapture::GetRecentEntries(int32 Count) const
{
	FScopeLock ScopeLock(&Lock);
	TArray<FMonolithLogEntry> Result;

	int32 Total = RingBuffer.Num();
	int32 Start = bWrapped ? WriteIndex : 0;
	int32 Num = FMath::Min(Count, Total);
	int32 Begin = bWrapped ? (WriteIndex - Num + Total) % Total : FMath::Max(0, Total - Num);

	for (int32 i = 0; i < Num; ++i)
	{
		int32 Idx = (Begin + i) % Total;
		Result.Add(RingBuffer[Idx]);
	}
	return Result;
}

TArray<FMonolithLogEntry> FMonolithLogCapture::SearchEntries(const FString& Pattern, const FString& CategoryFilter, ELogVerbosity::Type MaxVerbosity, int32 Limit) const
{
	FScopeLock ScopeLock(&Lock);
	TArray<FMonolithLogEntry> Result;

	FString PatternLower = Pattern.ToLower();
	int32 Total = RingBuffer.Num();
	int32 Start = bWrapped ? WriteIndex : 0;

	for (int32 i = 0; i < Total && Result.Num() < Limit; ++i)
	{
		int32 Idx = (Start + i) % Total;
		const FMonolithLogEntry& Entry = RingBuffer[Idx];

		if (Entry.Verbosity > MaxVerbosity) continue;
		if (!CategoryFilter.IsEmpty() && Entry.Category != FName(*CategoryFilter)) continue;
		if (!PatternLower.IsEmpty() && !Entry.Message.ToLower().Contains(PatternLower)) continue;

		Result.Add(Entry);
	}
	return Result;
}

TArray<FString> FMonolithLogCapture::GetActiveCategories() const
{
	FScopeLock ScopeLock(&Lock);
	TSet<FString> Categories;
	for (const FMonolithLogEntry& Entry : RingBuffer)
	{
		Categories.Add(Entry.Category.ToString());
	}
	return Categories.Array();
}

int32 FMonolithLogCapture::GetCountByVerbosity(ELogVerbosity::Type Verbosity) const
{
	FScopeLock ScopeLock(&Lock);
	switch (Verbosity)
	{
	case ELogVerbosity::Fatal: return TotalFatal;
	case ELogVerbosity::Error: return TotalError;
	case ELogVerbosity::Warning: return TotalWarning;
	case ELogVerbosity::Log: return TotalLog;
	case ELogVerbosity::Verbose: return TotalVerbose;
	default: return 0;
	}
}

int32 FMonolithLogCapture::GetTotalCount() const
{
	FScopeLock ScopeLock(&Lock);
	return TotalFatal + TotalError + TotalWarning + TotalLog + TotalVerbose;
}

TArray<FMonolithLogEntry> FMonolithLogCapture::GetEntriesSince(double SinceTimestamp, const TArray<FName>& CategoryFilter, ELogVerbosity::Type MaxVerbosity, int32 Limit) const
{
	FScopeLock ScopeLock(&Lock);
	TArray<FMonolithLogEntry> Result;

	int32 Total = RingBuffer.Num();
	int32 Start = bWrapped ? WriteIndex : 0;

	for (int32 i = 0; i < Total && Result.Num() < Limit; ++i)
	{
		int32 Idx = (Start + i) % Total;
		const FMonolithLogEntry& Entry = RingBuffer[Idx];

		if (Entry.Timestamp < SinceTimestamp) continue;
		if (Entry.Verbosity > MaxVerbosity) continue;
		if (CategoryFilter.Num() > 0 && !CategoryFilter.Contains(Entry.Category)) continue;

		Result.Add(Entry);
	}
	return Result;
}

int32 FMonolithLogCapture::CountErrorsSince(double SinceTimestamp) const
{
	FScopeLock ScopeLock(&Lock);
	int32 Count = 0;
	int32 Total = RingBuffer.Num();
	int32 Start = bWrapped ? WriteIndex : 0;

	for (int32 i = 0; i < Total; ++i)
	{
		int32 Idx = (Start + i) % Total;
		const FMonolithLogEntry& Entry = RingBuffer[Idx];
		if (Entry.Timestamp >= SinceTimestamp && Entry.Verbosity <= ELogVerbosity::Error)
		{
			++Count;
		}
	}
	return Count;
}

// --- Helpers ---

static FString VerbosityToString(ELogVerbosity::Type V)
{
	switch (V)
	{
	case ELogVerbosity::Fatal: return TEXT("fatal");
	case ELogVerbosity::Error: return TEXT("error");
	case ELogVerbosity::Warning: return TEXT("warning");
	case ELogVerbosity::Display: return TEXT("display");
	case ELogVerbosity::Log: return TEXT("log");
	case ELogVerbosity::Verbose: return TEXT("verbose");
	case ELogVerbosity::VeryVerbose: return TEXT("very_verbose");
	default: return TEXT("unknown");
	}
}

static ELogVerbosity::Type StringToVerbosity(const FString& S)
{
	if (S == TEXT("fatal")) return ELogVerbosity::Fatal;
	if (S == TEXT("error")) return ELogVerbosity::Error;
	if (S == TEXT("warning")) return ELogVerbosity::Warning;
	if (S == TEXT("display")) return ELogVerbosity::Display;
	if (S == TEXT("verbose")) return ELogVerbosity::Verbose;
	if (S == TEXT("very_verbose")) return ELogVerbosity::VeryVerbose;
	return ELogVerbosity::Log;
}

static TSharedPtr<FJsonObject> LogEntryToJson(const FMonolithLogEntry& Entry)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("timestamp"), Entry.Timestamp);
	Obj->SetStringField(TEXT("category"), Entry.Category.ToString());
	Obj->SetStringField(TEXT("verbosity"), VerbosityToString(Entry.Verbosity));
	Obj->SetStringField(TEXT("message"), Entry.Message);
	return Obj;
}

// --- Live Coding delegate ---

void FMonolithEditorActions::InitLiveCodingDelegate()
{
#if PLATFORM_WINDOWS
	ILiveCodingModule* LC = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LC)
	{
		LC->GetOnPatchCompleteDelegate().AddStatic(&FMonolithEditorActions::OnLiveCodingPatchComplete);
	}
#endif
}

void FMonolithEditorActions::OnLiveCodingPatchComplete()
{
	bIsCompiling = false;
	bPatchApplied = true;
	LastCompileResult = TEXT("success");
	LastCompileEndTimestamp = FPlatformTime::Seconds();
}

static FString TimestampToIso(double PlatformSeconds)
{
	if (PlatformSeconds <= 0.0) return TEXT("never");
	FDateTime Now = FDateTime::UtcNow();
	double CurrentSeconds = FPlatformTime::Seconds();
	double Delta = CurrentSeconds - PlatformSeconds;
	FDateTime EventTime = Now - FTimespan::FromSeconds(Delta);
	return EventTime.ToIso8601();
}

// --- Registration ---

void FMonolithEditorActions::RegisterActions(FMonolithLogCapture* LogCapture)
{
	CachedLogCapture = LogCapture;
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(TEXT("editor"), TEXT("trigger_build"),
		TEXT("Trigger a Live Coding compile"),
		FMonolithActionHandler::CreateStatic(&HandleTriggerBuild),
		FParamSchemaBuilder()
			.Optional(TEXT("wait"), TEXT("bool"), TEXT("Block until compile finishes"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("live_compile"),
		TEXT("Trigger a Live Coding compile (alias for trigger_build)"),
		FMonolithActionHandler::CreateStatic(&HandleTriggerBuild),
		FParamSchemaBuilder()
			.Optional(TEXT("wait"), TEXT("bool"), TEXT("Block until compile finishes"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_build_errors"),
		TEXT("Get build errors and warnings"),
		FMonolithActionHandler::CreateStatic(&HandleGetBuildErrors),
		FParamSchemaBuilder()
			.Optional(TEXT("since"), TEXT("number"), TEXT("Only errors from the last N seconds ago"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Filter to a specific log category"))
			.Optional(TEXT("compile_only"), TEXT("bool"), TEXT("Filter to compile categories only"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_build_status"),
		TEXT("Check compile status: compiling, last_result, last_compile_time, errors_since_compile, patch_applied"),
		FMonolithActionHandler::CreateStatic(&HandleGetBuildStatus),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_build_summary"),
		TEXT("Get summary of last build (errors, warnings, time)"),
		FMonolithActionHandler::CreateStatic(&HandleGetBuildSummary),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("search_build_output"),
		TEXT("Search build log output by pattern"),
		FMonolithActionHandler::CreateStatic(&HandleSearchBuildOutput),
		FParamSchemaBuilder()
			.Required(TEXT("pattern"), TEXT("string"), TEXT("Search pattern"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results to return"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_recent_logs"),
		TEXT("Get recent editor log entries"),
		FMonolithActionHandler::CreateStatic(&HandleGetRecentLogs),
		FParamSchemaBuilder()
			.Optional(TEXT("count"), TEXT("integer"), TEXT("Number of entries to return"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("search_logs"),
		TEXT("Search log entries by category, verbosity, and text pattern"),
		FMonolithActionHandler::CreateStatic(&HandleSearchLogs),
		FParamSchemaBuilder()
			.Optional(TEXT("pattern"), TEXT("string"), TEXT("Text pattern to search for"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Log category filter"))
			.Optional(TEXT("verbosity"), TEXT("string"), TEXT("Max verbosity level (error, warning, log, verbose)"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results to return"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("tail_log"),
		TEXT("Get last N log lines"),
		FMonolithActionHandler::CreateStatic(&HandleTailLog),
		FParamSchemaBuilder()
			.Optional(TEXT("count"), TEXT("integer"), TEXT("Number of lines to return"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_log_categories"),
		TEXT("List active log categories"),
		FMonolithActionHandler::CreateStatic(&HandleGetLogCategories),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_log_stats"),
		TEXT("Get log statistics by verbosity level"),
		FMonolithActionHandler::CreateStatic(&HandleGetLogStats),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_compile_output"),
		TEXT("Get structured compile report: result, time, log lines from compile categories, error/warning counts, patch status"),
		FMonolithActionHandler::CreateStatic(&HandleGetCompileOutput),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_crash_context"),
		TEXT("Get last crash/ensure context information"),
		FMonolithActionHandler::CreateStatic(&HandleGetCrashContext),
		MakeShared<FJsonObject>());

	// --- Capture actions ---

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_scene_preview"),
		TEXT("Capture a screenshot of an asset (Niagara, material) rendered in a preview scene"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureScenePreview),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path to preview"))
			.Required(TEXT("asset_type"), TEXT("string"), TEXT("niagara | material"))
			.Optional(TEXT("preview_mesh"), TEXT("string"), TEXT("Mesh for materials: plane, sphere, cube"), TEXT("plane"))
			.Optional(TEXT("seek_time"), TEXT("number"), TEXT("Advance Niagara sim to this time (seconds)"), TEXT("0.0"))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r], fov:60}"))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height]"), TEXT("[512,512]"))
			.Optional(TEXT("output_path"), TEXT("string"), TEXT("Output PNG path (absolute or relative to project)"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_sequence_frames"),
		TEXT("Capture multiple frames of an animated effect at specified timestamps"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureSequenceFrames),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path to preview"))
			.Required(TEXT("asset_type"), TEXT("string"), TEXT("niagara"))
			.Required(TEXT("timestamps"), TEXT("array"), TEXT("Array of capture times in seconds"))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r], fov:60}"))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height]"), TEXT("[512,512]"))
			.Optional(TEXT("output_dir"), TEXT("string"), TEXT("Output directory for frame PNGs"))
			.Optional(TEXT("filename_prefix"), TEXT("string"), TEXT("Prefix for frame files"), TEXT("frame"))
			.Optional(TEXT("persistent"), TEXT("bool"), TEXT("Use persistent component (preserves ribbons/accumulation). Default: false (per-frame recreate)."))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("import_texture"),
		TEXT("Import an external image as a UTexture2D with configurable settings"),
		FMonolithActionHandler::CreateStatic(&HandleImportTexture),
		FParamSchemaBuilder()
			.Required(TEXT("source_path"), TEXT("string"), TEXT("Absolute path to source image (PNG, TGA, EXR, HDR)"))
			.Required(TEXT("destination"), TEXT("string"), TEXT("UE asset path for imported texture"))
			.Optional(TEXT("settings"), TEXT("object"), TEXT("{compression, srgb, tiling, max_size, lod_group}"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("stitch_flipbook"),
		TEXT("Stitch individual frame images into a flipbook atlas texture and import as UTexture2D"),
		FMonolithActionHandler::CreateStatic(&HandleStitchFlipbook),
		FParamSchemaBuilder()
			.Required(TEXT("frame_paths"), TEXT("array"), TEXT("Ordered array of absolute file paths to frame PNGs"))
			.Required(TEXT("dest_path"), TEXT("string"), TEXT("UE asset path for the output texture (e.g. /Game/AgentTraining/Textures/T_FB_001)"))
			.Required(TEXT("grid"), TEXT("array"), TEXT("[columns, rows] grid layout (e.g. [4, 4] for 16 frames)"))
			.Optional(TEXT("srgb"), TEXT("bool"), TEXT("sRGB color space (true for color, false for masks)"), TEXT("true"))
			.Optional(TEXT("no_mipmaps"), TEXT("bool"), TEXT("Disable mipmap generation to prevent atlas bleed"), TEXT("true"))
			.Optional(TEXT("delete_sources"), TEXT("bool"), TEXT("Delete source PNG files after successful stitch"), TEXT("true"))
			.Optional(TEXT("lod_group"), TEXT("string"), TEXT("Texture LOD group"), TEXT("TEXTUREGROUP_Effects"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("delete_assets"),
		TEXT("Delete UE assets by path. Optional safety: restrict to allowed path prefixes"),
		FMonolithActionHandler::CreateStatic(&HandleDeleteAssets),
		FParamSchemaBuilder()
			.Required(TEXT("asset_paths"), TEXT("array"), TEXT("Array of UE asset paths to delete"))
			.Optional(TEXT("allowed_prefixes"), TEXT("array"), TEXT("If set, only paths starting with one of these prefixes can be deleted (e.g. [\"/Game/AgentTraining/\"])"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_viewport_info"),
		TEXT("Get current editor viewport camera position, rotation, FOV, and resolution"),
		FMonolithActionHandler::CreateStatic(&HandleGetViewportInfo),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_system_gif"),
		TEXT("Capture a Niagara system as a sequence of PNG frames with optional GIF encoding via ffmpeg or python"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureSystemGif),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Optional(TEXT("duration_seconds"), TEXT("number"), TEXT("Capture duration in seconds (default: 2.0)"))
			.Optional(TEXT("fps"), TEXT("integer"), TEXT("Frames per second (default: 15)"))
			.Optional(TEXT("resolution"), TEXT("integer"), TEXT("Output resolution width/height in pixels (default: 256)"))
			.Optional(TEXT("output_path"), TEXT("string"), TEXT("Output directory (default: Saved/Screenshots/Monolith/GIF_<timestamp>)"))
			.Optional(TEXT("encoder"), TEXT("string"), TEXT("frames_only (default), ffmpeg, or python — opt-in GIF encoding"))
			.Build());

	InitLiveCodingDelegate();
}

// --- Build actions ---

FMonolithActionResult FMonolithEditorActions::HandleTriggerBuild(const TSharedPtr<FJsonObject>& Params)
{
#if PLATFORM_WINDOWS
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (!LiveCoding)
	{
		return FMonolithActionResult::Error(TEXT("Live Coding module not available"));
	}

	if (!LiveCoding->IsEnabledForSession() && !LiveCoding->IsEnabledByDefault())
	{
		LiveCoding->EnableByDefault(true);
		LiveCoding->EnableForSession(true);
	}

	if (LiveCoding->IsCompiling())
	{
		return FMonolithActionResult::Error(TEXT("A compile is already in progress"));
	}

	bool bWait = false;
	if (Params->HasField(TEXT("wait")))
	{
		bWait = Params->GetBoolField(TEXT("wait"));
	}

	LastCompileTimestamp = FPlatformTime::Seconds();
	bIsCompiling = true;
	bPatchApplied = false;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	if (bWait)
	{
		ELiveCodingCompileResult CompileResult;
		bool bStarted = LiveCoding->Compile(ELiveCodingCompileFlags::WaitForCompletion, &CompileResult);

		bIsCompiling = false;
		LastCompileEndTimestamp = FPlatformTime::Seconds();
		Root->SetBoolField(TEXT("started"), bStarted);

		FString ResultStr;
		switch (CompileResult)
		{
		case ELiveCodingCompileResult::Success: ResultStr = TEXT("success"); bPatchApplied = true; break;
		case ELiveCodingCompileResult::NoChanges: ResultStr = TEXT("no_changes"); break;
		case ELiveCodingCompileResult::Failure: ResultStr = TEXT("failure"); break;
		case ELiveCodingCompileResult::Cancelled: ResultStr = TEXT("cancelled"); break;
		case ELiveCodingCompileResult::CompileStillActive: ResultStr = TEXT("compile_still_active"); break;
		case ELiveCodingCompileResult::NotStarted: ResultStr = TEXT("not_started"); break;
		default: ResultStr = TEXT("unknown"); break;
		}
		LastCompileResult = ResultStr;
		Root->SetStringField(TEXT("result"), ResultStr);
	}
	else
	{
		LiveCoding->Compile();
		LastCompileResult = TEXT("in_progress");
		Root->SetBoolField(TEXT("started"), true);
		Root->SetStringField(TEXT("result"), TEXT("in_progress"));
	}

	return FMonolithActionResult::Success(Root);
#else
	return FMonolithActionResult::Error(TEXT("Live Coding is only available on Windows"));
#endif
}

FMonolithActionResult FMonolithEditorActions::HandleGetBuildErrors(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ErrorsArr;
	TArray<TSharedPtr<FJsonValue>> WarningsArr;

	// Determine time window
	double SinceTimestamp = LastCompileTimestamp; // Default: since last compile
	if (Params->HasField(TEXT("since")))
	{
		double SecondsAgo = Params->GetNumberField(TEXT("since"));
		SinceTimestamp = FPlatformTime::Seconds() - SecondsAgo;
	}

	// Build category filter
	TArray<FName> CategoryFilter;
	bool bCompileOnly = false;
	if (Params->HasField(TEXT("compile_only")))
	{
		bCompileOnly = Params->GetBoolField(TEXT("compile_only"));
	}
	if (bCompileOnly)
	{
		CategoryFilter.Add(FName(TEXT("LogLiveCoding")));
		CategoryFilter.Add(FName(TEXT("LogCompile")));
		CategoryFilter.Add(FName(TEXT("LogLinker")));
	}
	else if (Params->HasField(TEXT("category")))
	{
		CategoryFilter.Add(FName(*Params->GetStringField(TEXT("category"))));
	}

	if (CachedLogCapture)
	{
		TArray<FMonolithLogEntry> Entries = CachedLogCapture->GetEntriesSince(
			SinceTimestamp, CategoryFilter, ELogVerbosity::Warning, 500);

		for (const FMonolithLogEntry& Entry : Entries)
		{
			if (Entry.Verbosity <= ELogVerbosity::Error)
			{
				TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
				ErrObj->SetStringField(TEXT("message"), Entry.Message);
				ErrObj->SetStringField(TEXT("category"), Entry.Category.ToString());
				ErrObj->SetStringField(TEXT("verbosity"), VerbosityToString(Entry.Verbosity));
				ErrorsArr.Add(MakeShared<FJsonValueObject>(ErrObj));
			}
			else if (Entry.Verbosity == ELogVerbosity::Warning)
			{
				TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
				WarnObj->SetStringField(TEXT("message"), Entry.Message);
				WarnObj->SetStringField(TEXT("category"), Entry.Category.ToString());
				WarningsArr.Add(MakeShared<FJsonValueObject>(WarnObj));
			}
		}
	}

	Root->SetNumberField(TEXT("error_count"), ErrorsArr.Num());
	Root->SetArrayField(TEXT("errors"), ErrorsArr);
	Root->SetNumberField(TEXT("warning_count"), WarningsArr.Num());
	Root->SetArrayField(TEXT("warnings"), WarningsArr);
	Root->SetStringField(TEXT("since"), TimestampToIso(SinceTimestamp));

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleGetBuildStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

#if PLATFORM_WINDOWS
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LiveCoding)
	{
		bool bCurrentlyCompiling = LiveCoding->IsCompiling();

		// Update tracked state if LC reports done but we haven't caught it yet
		if (bIsCompiling && !bCurrentlyCompiling)
		{
			bIsCompiling = false;
			if (LastCompileEndTimestamp < LastCompileTimestamp)
			{
				LastCompileEndTimestamp = FPlatformTime::Seconds();
			}
		}

		Root->SetBoolField(TEXT("live_coding_available"), true);
		Root->SetBoolField(TEXT("live_coding_started"), LiveCoding->HasStarted());
		Root->SetBoolField(TEXT("live_coding_enabled"), LiveCoding->IsEnabledForSession());
		Root->SetBoolField(TEXT("compiling"), bCurrentlyCompiling);
	}
	else
	{
		Root->SetBoolField(TEXT("live_coding_available"), false);
		Root->SetBoolField(TEXT("compiling"), false);
	}
#else
	Root->SetBoolField(TEXT("live_coding_available"), false);
	Root->SetBoolField(TEXT("compiling"), false);
#endif

	Root->SetStringField(TEXT("last_result"), LastCompileResult);
	Root->SetStringField(TEXT("last_compile_time"), TimestampToIso(LastCompileTimestamp));
	Root->SetBoolField(TEXT("patch_applied"), bPatchApplied);

	if (CachedLogCapture && LastCompileTimestamp > 0.0)
	{
		Root->SetNumberField(TEXT("errors_since_compile"), CachedLogCapture->CountErrorsSince(LastCompileTimestamp));
	}
	else
	{
		Root->SetNumberField(TEXT("errors_since_compile"), 0);
	}

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleGetBuildSummary(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	// Get error/warning counts from log capture
	int32 ErrorCount = 0;
	int32 WarningCount = 0;

	if (CachedLogCapture)
	{
		ErrorCount = CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Error);
		WarningCount = CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Warning);
	}

	Root->SetNumberField(TEXT("total_errors"), ErrorCount);
	Root->SetNumberField(TEXT("total_warnings"), WarningCount);

#if PLATFORM_WINDOWS
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LiveCoding)
	{
		Root->SetBoolField(TEXT("compiling"), LiveCoding->IsCompiling());
		Root->SetBoolField(TEXT("live_coding_started"), LiveCoding->HasStarted());
	}
#endif

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleSearchBuildOutput(const TSharedPtr<FJsonObject>& Params)
{
	FString Pattern = Params->GetStringField(TEXT("pattern"));
	if (Pattern.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: pattern"));
	}

	int32 Limit = 100;
	if (Params->HasField(TEXT("limit")))
	{
		Limit = static_cast<int32>(Params->GetNumberField(TEXT("limit")));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Matches;

	if (CachedLogCapture)
	{
		// Search for compile-related messages matching the pattern
		TArray<FMonolithLogEntry> Entries = CachedLogCapture->SearchEntries(
			Pattern, TEXT(""), ELogVerbosity::VeryVerbose, Limit);

		for (const FMonolithLogEntry& Entry : Entries)
		{
			Matches.Add(MakeShared<FJsonValueObject>(LogEntryToJson(Entry)));
		}
	}

	Root->SetStringField(TEXT("pattern"), Pattern);
	Root->SetNumberField(TEXT("match_count"), Matches.Num());
	Root->SetArrayField(TEXT("matches"), Matches);

	return FMonolithActionResult::Success(Root);
}

// --- Compile output ---

FMonolithActionResult FMonolithEditorActions::HandleGetCompileOutput(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	Root->SetStringField(TEXT("last_result"), LastCompileResult);
	Root->SetStringField(TEXT("last_compile_time"), TimestampToIso(LastCompileTimestamp));
	Root->SetStringField(TEXT("last_compile_end_time"), TimestampToIso(LastCompileEndTimestamp));
	Root->SetBoolField(TEXT("patch_applied"), bPatchApplied);
	Root->SetBoolField(TEXT("compiling"), bIsCompiling);

	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	TArray<TSharedPtr<FJsonValue>> LogLines;

	if (CachedLogCapture && LastCompileTimestamp > 0.0)
	{
		// Get all log lines from compile-related categories since last compile
		TArray<FName> CompileCategories;
		CompileCategories.Add(FName(TEXT("LogLiveCoding")));
		CompileCategories.Add(FName(TEXT("LogCompile")));
		CompileCategories.Add(FName(TEXT("LogLinker")));

		TArray<FMonolithLogEntry> Entries = CachedLogCapture->GetEntriesSince(
			LastCompileTimestamp, CompileCategories, ELogVerbosity::VeryVerbose, 500);

		for (const FMonolithLogEntry& Entry : Entries)
		{
			LogLines.Add(MakeShared<FJsonValueObject>(LogEntryToJson(Entry)));
			if (Entry.Verbosity <= ELogVerbosity::Error) ++ErrorCount;
			else if (Entry.Verbosity == ELogVerbosity::Warning) ++WarningCount;
		}
	}

	Root->SetNumberField(TEXT("error_count"), ErrorCount);
	Root->SetNumberField(TEXT("warning_count"), WarningCount);
	Root->SetNumberField(TEXT("log_line_count"), LogLines.Num());
	Root->SetArrayField(TEXT("compile_log"), LogLines);

	return FMonolithActionResult::Success(Root);
}

// --- Log actions ---

FMonolithActionResult FMonolithEditorActions::HandleGetRecentLogs(const TSharedPtr<FJsonObject>& Params)
{
	int32 Count = 100;
	if (Params->HasField(TEXT("count")))
	{
		Count = static_cast<int32>(Params->GetNumberField(TEXT("count")));
	}
	else if (Params->HasField(TEXT("max")))
	{
		Count = static_cast<int32>(Params->GetNumberField(TEXT("max")));
	}
	Count = FMath::Clamp(Count, 1, 1000);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> LogArr;

	if (CachedLogCapture)
	{
		TArray<FMonolithLogEntry> Entries = CachedLogCapture->GetRecentEntries(Count);
		for (const FMonolithLogEntry& Entry : Entries)
		{
			LogArr.Add(MakeShared<FJsonValueObject>(LogEntryToJson(Entry)));
		}
	}

	Root->SetNumberField(TEXT("count"), LogArr.Num());
	Root->SetArrayField(TEXT("entries"), LogArr);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleSearchLogs(const TSharedPtr<FJsonObject>& Params)
{
	FString Pattern = Params->GetStringField(TEXT("pattern"));
	FString Category = Params->GetStringField(TEXT("category"));
	FString VerbosityStr = Params->GetStringField(TEXT("verbosity"));
	ELogVerbosity::Type MaxVerbosity = VerbosityStr.IsEmpty() ? ELogVerbosity::VeryVerbose : StringToVerbosity(VerbosityStr);

	int32 Limit = 200;
	if (Params->HasField(TEXT("limit")))
	{
		Limit = static_cast<int32>(Params->GetNumberField(TEXT("limit")));
	}
	Limit = FMath::Clamp(Limit, 1, 2000);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> LogArr;

	if (CachedLogCapture)
	{
		TArray<FMonolithLogEntry> Entries = CachedLogCapture->SearchEntries(Pattern, Category, MaxVerbosity, Limit);
		for (const FMonolithLogEntry& Entry : Entries)
		{
			LogArr.Add(MakeShared<FJsonValueObject>(LogEntryToJson(Entry)));
		}
	}

	Root->SetNumberField(TEXT("match_count"), LogArr.Num());
	Root->SetArrayField(TEXT("entries"), LogArr);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleTailLog(const TSharedPtr<FJsonObject>& Params)
{
	int32 Count = 50;
	if (Params->HasField(TEXT("count")))
	{
		Count = static_cast<int32>(Params->GetNumberField(TEXT("count")));
	}
	Count = FMath::Clamp(Count, 1, 500);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Lines;

	if (CachedLogCapture)
	{
		TArray<FMonolithLogEntry> Entries = CachedLogCapture->GetRecentEntries(Count);
		for (const FMonolithLogEntry& Entry : Entries)
		{
			FString Line = FString::Printf(TEXT("[%s][%s] %s"),
				*Entry.Category.ToString(),
				*VerbosityToString(Entry.Verbosity),
				*Entry.Message);
			Lines.Add(MakeShared<FJsonValueString>(Line));
		}
	}

	Root->SetNumberField(TEXT("count"), Lines.Num());
	Root->SetArrayField(TEXT("lines"), Lines);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleGetLogCategories(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> CatArr;

	if (CachedLogCapture)
	{
		TArray<FString> Categories = CachedLogCapture->GetActiveCategories();
		Categories.Sort();
		for (const FString& Cat : Categories)
		{
			CatArr.Add(MakeShared<FJsonValueString>(Cat));
		}
	}

	Root->SetNumberField(TEXT("count"), CatArr.Num());
	Root->SetArrayField(TEXT("categories"), CatArr);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleGetLogStats(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	if (CachedLogCapture)
	{
		Root->SetNumberField(TEXT("total"), CachedLogCapture->GetTotalCount());
		Root->SetNumberField(TEXT("fatal"), CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Fatal));
		Root->SetNumberField(TEXT("error"), CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Error));
		Root->SetNumberField(TEXT("warning"), CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Warning));
		Root->SetNumberField(TEXT("log"), CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Log));
		Root->SetNumberField(TEXT("verbose"), CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Verbose));
	}
	else
	{
		Root->SetNumberField(TEXT("total"), 0);
		Root->SetStringField(TEXT("status"), TEXT("log_capture_not_initialized"));
	}

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleGetCrashContext(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	// Check for crash log file on disk
	FString CrashLogPath = FPaths::ProjectLogDir() / TEXT("CrashContext.runtime-xml");
	bool bHasCrashLog = FPaths::FileExists(CrashLogPath);
	Root->SetBoolField(TEXT("has_crash_context"), bHasCrashLog);

	if (bHasCrashLog)
	{
		FString CrashXml;
		if (FFileHelper::LoadFileToString(CrashXml, *CrashLogPath))
		{
			// Truncate if very large
			if (CrashXml.Len() > 4096)
			{
				CrashXml = CrashXml.Left(4096) + TEXT("...(truncated)");
			}
			Root->SetStringField(TEXT("crash_xml"), CrashXml);
		}
	}

	// Also check ensure log
	FString EnsureLogPath = FPaths::ProjectLogDir() / TEXT("Ensures.log");
	if (FPaths::FileExists(EnsureLogPath))
	{
		FString EnsureLog;
		if (FFileHelper::LoadFileToString(EnsureLog, *EnsureLogPath))
		{
			if (EnsureLog.Len() > 4096)
			{
				EnsureLog = EnsureLog.Right(4096);
			}
			Root->SetStringField(TEXT("ensure_log"), EnsureLog);
		}
	}

	// Provide recent errors/fatals from log capture
	if (CachedLogCapture)
	{
		TArray<FMonolithLogEntry> ErrorEntries = CachedLogCapture->SearchEntries(
			TEXT(""), TEXT(""), ELogVerbosity::Error, 20);
		TArray<TSharedPtr<FJsonValue>> RecentErrors;
		for (const FMonolithLogEntry& Entry : ErrorEntries)
		{
			RecentErrors.Add(MakeShared<FJsonValueObject>(LogEntryToJson(Entry)));
		}
		Root->SetArrayField(TEXT("recent_errors"), RecentErrors);
	}

	return FMonolithActionResult::Success(Root);
}

// --- Capture helpers ---

bool FMonolithEditorActions::RenderAndSaveCapture(
	USceneCaptureComponent2D* CaptureComp,
	UTextureRenderTarget2D* RT,
	int32 ResX, int32 ResY,
	const FString& OutputPath)
{
	if (!CaptureComp || !RT)
	{
		return false;
	}

	// Trigger the capture — submits render commands to the render thread
	CaptureComp->CaptureScene();

	// Use GameThread_GetRenderTargetResource — the non-GameThread variant
	// asserts IsInRenderingThread() which crashes when called from game thread.
	FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource();
	if (!RTResource)
	{
		UE_LOG(LogMonolith, Error, TEXT("CaptureScenePreview: Failed to get RT resource"));
		return false;
	}

	// ReadPixels internally calls FlushRenderingCommands() to synchronize the GPU readback
	TArray<FColor> Pixels;
	bool bReadOk = RTResource->ReadPixels(Pixels);

	if (!bReadOk || Pixels.Num() == 0)
	{
		UE_LOG(LogMonolith, Error, TEXT("CaptureScenePreview: ReadPixels failed (read=%d, count=%d)"),
			bReadOk, Pixels.Num());
		return false;
	}

	// Ensure output directory exists
	FString Dir = FPaths::GetPath(OutputPath);
	IFileManager::Get().MakeDirectory(*Dir, true);

	// Encode as PNG and save
	FImage Image;
	Image.Init(ResX, ResY, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FColor));

	return FImageUtils::SaveImageAutoFormat(*OutputPath, Image);
}

bool FMonolithEditorActions::CaptureNiagaraFrame(
	UNiagaraSystem* System,
	float SeekTime,
	const FVector& CameraLocation,
	const FRotator& CameraRotation,
	float FOV,
	int32 ResX, int32 ResY,
	const FString& OutputPath,
	ESceneCaptureSource CaptureSource)
{
	if (!System)
	{
		return false;
	}

	// Create preview scene with black background (no environment lighting)
	// VFX effects (especially fire, emissives) need a dark background to evaluate properly
	FPreviewScene::ConstructionValues CVs;
	CVs.bDefaultLighting = false;
	CVs.LightBrightness = 0.0f;
	CVs.SkyBrightness = 0.0f;
	TSharedPtr<FAdvancedPreviewScene> PreviewScene =
		MakeShareable(new FAdvancedPreviewScene(CVs));
	PreviewScene->SetFloorVisibility(false);
	PreviewScene->SetEnvironmentVisibility(false);

	// Create Niagara component
	UNiagaraComponent* NiagaraComp = NewObject<UNiagaraComponent>(
		GetTransientPackage(), NAME_None, RF_Transient);
	NiagaraComp->CastShadow = false;
	NiagaraComp->bCastDynamicShadow = false;
	NiagaraComp->SetAllowScalability(false);
	NiagaraComp->SetAsset(System);
	NiagaraComp->SetForceSolo(true);
	NiagaraComp->SetAgeUpdateMode(ENiagaraAgeUpdateMode::DesiredAge);
	NiagaraComp->SetCanRenderWhileSeeking(true);
	NiagaraComp->SetMaxSimTime(0.0f);
	NiagaraComp->Activate(true);

	PreviewScene->AddComponent(NiagaraComp, NiagaraComp->GetRelativeTransform());

	// Seek to desired time
	const float SeekDelta = 1.0f / 30.0f;
	UWorld* World = NiagaraComp->GetWorld();

	if (SeekTime > 0.0f)
	{
		NiagaraComp->SetSeekDelta(SeekDelta);
		NiagaraComp->SeekToDesiredAge(SeekTime);

		if (World)
		{
			World->TimeSeconds = SeekTime;
			World->UnpausedTimeSeconds = SeekTime;
			World->RealTimeSeconds = SeekTime;
			World->DeltaRealTimeSeconds = SeekDelta;
			World->DeltaTimeSeconds = SeekDelta;
			World->Tick(ELevelTick::LEVELTICK_PauseTick, 0.0f);
		}

		NiagaraComp->TickComponent(SeekDelta, ELevelTick::LEVELTICK_All, nullptr);

		if (World)
		{
			World->SendAllEndOfFrameUpdates();
			if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(World))
			{
				WorldManager->FlushComputeAndDeferredQueues(true);  // Wait for GPU
			}
		}
	}

	// Warm-up ticks: pump the world + component so GPU particle buffers are populated.
	// Runs even at SeekTime==0 — particles need frames to spawn and fill GPU buffers.
	if (World)
	{
		constexpr int32 WarmUpFrames = 3;
		for (int32 i = 0; i < WarmUpFrames; i++)
		{
			World->Tick(ELevelTick::LEVELTICK_PauseTick, SeekDelta);
			NiagaraComp->TickComponent(SeekDelta, ELevelTick::LEVELTICK_All, nullptr);
			World->SendAllEndOfFrameUpdates();
			if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(World))
			{
				WorldManager->FlushComputeAndDeferredQueues(true);
			}
			FlushRenderingCommands();
		}
	}

	// Wait for any in-flight shader compilation before capture
	if (GShaderCompilingManager)
	{
		GShaderCompilingManager->FinishAllCompilation();
	}
	FlushRenderingCommands();

	// Create render target
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	RT->InitAutoFormat(ResX, ResY);
	RT->ClearColor = FLinearColor::Black;
	RT->UpdateResourceImmediate(true);

	// Create scene capture component (same as Baker)
	USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	CaptureComp->bTickInEditor = false;
	CaptureComp->SetComponentTickEnabled(false);
	CaptureComp->SetVisibility(true);
	CaptureComp->bCaptureEveryFrame = false;
	CaptureComp->bCaptureOnMovement = false;
	CaptureComp->TextureTarget = RT;
	CaptureComp->CaptureSource = CaptureSource;
	CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
	CaptureComp->FOVAngle = FOV;
	CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

	// Register with the preview scene's world (World already declared above)
	CaptureComp->RegisterComponentWithWorld(World);
	CaptureComp->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

	// Capture and save
	bool bSuccess = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, OutputPath);

	// Cleanup
	CaptureComp->TextureTarget = nullptr;
	CaptureComp->UnregisterComponent();
	PreviewScene->RemoveComponent(NiagaraComp);

	return bSuccess;
}

bool FMonolithEditorActions::CaptureMaterialFrame(
	UMaterialInterface* Material,
	const FString& MeshType,
	const FVector& CameraLocation,
	const FRotator& CameraRotation,
	float FOV,
	int32 ResX, int32 ResY,
	const FString& OutputPath,
	ESceneCaptureSource CaptureSource,
	float UVTiling,
	const FLinearColor& BackgroundColor)
{
	if (!Material)
	{
		return false;
	}

	// Create preview scene
	TSharedPtr<FAdvancedPreviewScene> PreviewScene =
		MakeShareable(new FAdvancedPreviewScene(FPreviewScene::ConstructionValues()));
	PreviewScene->SetFloorVisibility(false);

	UPrimitiveComponent* SpawnedMeshComp = nullptr;

	if (!FMath::IsNearlyEqual(UVTiling, 1.0f) && (MeshType.Equals(TEXT("plane"), ESearchCase::IgnoreCase) || MeshType.IsEmpty()))
	{
		// Build a procedural quad with scaled UVs for tiling preview
		UProceduralMeshComponent* ProcMeshComp = NewObject<UProceduralMeshComponent>(
			GetTransientPackage(), NAME_None, RF_Transient);

		const float HalfSize = 100.0f; // 200x200 cm quad
		TArray<FVector> Vertices;
		Vertices.Add(FVector(-HalfSize, -HalfSize, 0.0f));
		Vertices.Add(FVector( HalfSize, -HalfSize, 0.0f));
		Vertices.Add(FVector( HalfSize,  HalfSize, 0.0f));
		Vertices.Add(FVector(-HalfSize,  HalfSize, 0.0f));

		TArray<int32> Triangles = { 0, 1, 2, 0, 2, 3 };

		TArray<FVector> Normals;
		Normals.Init(FVector::UpVector, 4);

		TArray<FVector2D> UV0;
		UV0.Add(FVector2D(0.0f, 0.0f));
		UV0.Add(FVector2D(UVTiling, 0.0f));
		UV0.Add(FVector2D(UVTiling, UVTiling));
		UV0.Add(FVector2D(0.0f, UVTiling));

		TArray<FColor> VertexColors;
		VertexColors.Init(FColor::White, 4);

		TArray<FProcMeshTangent> Tangents;
		Tangents.Init(FProcMeshTangent(1.0f, 0.0f, 0.0f), 4);

		ProcMeshComp->CreateMeshSection(0, Vertices, Triangles, Normals, UV0, VertexColors, Tangents, false);
		ProcMeshComp->SetMaterial(0, const_cast<UMaterialInterface*>(Material));
		ProcMeshComp->SetRelativeScale3D(FVector(2.0f, 2.0f, 1.0f));
		SpawnedMeshComp = ProcMeshComp;
	}
	else
	{
		// Standard static mesh path
		FString MeshPath;
		if (MeshType.Equals(TEXT("sphere"), ESearchCase::IgnoreCase))
		{
			MeshPath = TEXT("/Engine/BasicShapes/Sphere");
		}
		else if (MeshType.Equals(TEXT("cube"), ESearchCase::IgnoreCase))
		{
			MeshPath = TEXT("/Engine/BasicShapes/Cube");
		}
		else if (MeshType.Equals(TEXT("cylinder"), ESearchCase::IgnoreCase))
		{
			MeshPath = TEXT("/Engine/BasicShapes/Cylinder");
		}
		else // default: plane
		{
			MeshPath = TEXT("/Engine/BasicShapes/Plane");
		}

		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
		if (!Mesh)
		{
			UE_LOG(LogMonolith, Error, TEXT("CaptureMaterialFrame: Failed to load mesh %s"), *MeshPath);
			return false;
		}

		UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(
			GetTransientPackage(), NAME_None, RF_Transient);
		MeshComp->SetStaticMesh(Mesh);
		MeshComp->SetMaterial(0, const_cast<UMaterialInterface*>(Material));
		MeshComp->SetRelativeScale3D(FVector(2.0f, 2.0f, 1.0f));
		SpawnedMeshComp = MeshComp;
	}

	PreviewScene->AddComponent(SpawnedMeshComp, SpawnedMeshComp->GetRelativeTransform());

	// Create render target
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	RT->InitAutoFormat(ResX, ResY);
	RT->ClearColor = BackgroundColor;
	RT->UpdateResourceImmediate(true);

	// Create scene capture
	USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	CaptureComp->bTickInEditor = false;
	CaptureComp->SetComponentTickEnabled(false);
	CaptureComp->SetVisibility(true);
	CaptureComp->bCaptureEveryFrame = false;
	CaptureComp->bCaptureOnMovement = false;
	CaptureComp->TextureTarget = RT;
	CaptureComp->CaptureSource = CaptureSource;
	CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
	CaptureComp->FOVAngle = FOV;
	CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

	UWorld* World = PreviewScene->GetWorld();
	CaptureComp->RegisterComponentWithWorld(World);
	CaptureComp->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

	// Capture and save
	bool bSuccess = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, OutputPath);

	// Cleanup
	CaptureComp->TextureTarget = nullptr;
	CaptureComp->UnregisterComponent();
	PreviewScene->RemoveComponent(SpawnedMeshComp);

	return bSuccess;
}

// --- Capture action handlers ---

FMonolithActionResult FMonolithEditorActions::HandleCaptureScenePreview(
	const TSharedPtr<FJsonObject>& Params)
{
	// Parse required params
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString AssetType = Params->GetStringField(TEXT("asset_type"));

	if (AssetPath.IsEmpty() || AssetType.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and asset_type are required"));
	}

	// Parse optional params
	float SeekTime = 0.0f;
	if (Params->HasField(TEXT("seek_time")))
	{
		SeekTime = (float)Params->GetNumberField(TEXT("seek_time"));
	}

	FString PreviewMesh = TEXT("plane");
	if (Params->HasField(TEXT("preview_mesh")))
	{
		PreviewMesh = Params->GetStringField(TEXT("preview_mesh"));
	}

	int32 ResX = 512, ResY = 512;
	if (Params->HasField(TEXT("resolution")))
	{
		const TArray<TSharedPtr<FJsonValue>>& ResArray = Params->GetArrayField(TEXT("resolution"));
		if (ResArray.Num() >= 2)
		{
			ResX = (int32)ResArray[0]->AsNumber();
			ResY = (int32)ResArray[1]->AsNumber();
		}
	}

	// Parse camera
	FVector CameraLocation(200.0f, 0.0f, 100.0f);
	FRotator CameraRotation(0.0f, 180.0f, 0.0f);
	float FOV = 60.0f;

	if (Params->HasField(TEXT("camera")))
	{
		const TSharedPtr<FJsonObject>* CameraObj = nullptr;
		TSharedPtr<FJsonObject> ParsedCamera;

		// Handle both object and string-serialized (Claude Code quirk)
		if (!Params->TryGetObjectField(TEXT("camera"), CameraObj))
		{
			FString CameraStr = Params->GetStringField(TEXT("camera"));
			if (!CameraStr.IsEmpty())
			{
				ParsedCamera = FMonolithJsonUtils::Parse(CameraStr);
				CameraObj = &ParsedCamera;
			}
		}

		if (CameraObj && (*CameraObj).IsValid())
		{
			if ((*CameraObj)->HasField(TEXT("location")))
			{
				const TArray<TSharedPtr<FJsonValue>>& Loc = (*CameraObj)->GetArrayField(TEXT("location"));
				if (Loc.Num() >= 3)
				{
					CameraLocation = FVector(Loc[0]->AsNumber(), Loc[1]->AsNumber(), Loc[2]->AsNumber());
				}
			}
			if ((*CameraObj)->HasField(TEXT("rotation")))
			{
				const TArray<TSharedPtr<FJsonValue>>& Rot = (*CameraObj)->GetArrayField(TEXT("rotation"));
				if (Rot.Num() >= 3)
				{
					CameraRotation = FRotator(Rot[0]->AsNumber(), Rot[1]->AsNumber(), Rot[2]->AsNumber());
				}
			}
			if ((*CameraObj)->HasField(TEXT("fov")))
			{
				FOV = (float)(*CameraObj)->GetNumberField(TEXT("fov"));
			}
		}
	}

	// Generate output path
	FString OutputPath;
	if (Params->HasField(TEXT("output_path")))
	{
		OutputPath = Params->GetStringField(TEXT("output_path"));
		if (FPaths::IsRelative(OutputPath))
		{
			OutputPath = FPaths::ProjectDir() / OutputPath;
		}
	}
	else
	{
		FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		FString SafeName = FPaths::GetBaseFilename(AssetPath);
		OutputPath = FPaths::ProjectDir() / TEXT("Saved/Screenshots/Monolith") /
			FString::Printf(TEXT("%s_%s.png"), *Timestamp, *SafeName);
	}

	// UE's FHttpServerModule dispatches handlers on the game thread via FTicker,
	// so we're already on the game thread here. Call capture functions directly.
	check(IsInGameThread());

	double StartTime = FPlatformTime::Seconds();
	bool bSuccess = false;

	if (AssetType.Equals(TEXT("niagara"), ESearchCase::IgnoreCase))
	{
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
		if (!System)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to load Niagara system: %s"), *AssetPath));
		}
		bSuccess = CaptureNiagaraFrame(System, SeekTime, CameraLocation, CameraRotation,
			FOV, ResX, ResY, OutputPath);
	}
	else if (AssetType.Equals(TEXT("material"), ESearchCase::IgnoreCase))
	{
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *AssetPath);
		if (!Material)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to load material: %s"), *AssetPath));
		}
		float UVTiling = 1.0f;
		if (Params->HasField(TEXT("uv_tiling")))
		{
			UVTiling = (float)Params->GetNumberField(TEXT("uv_tiling"));
			if (UVTiling <= 0.0f) UVTiling = 1.0f;
		}

		FLinearColor BgColor(0.18f, 0.18f, 0.18f);
		if (Params->HasField(TEXT("background_color")))
		{
			const TArray<TSharedPtr<FJsonValue>>& BgArr = Params->GetArrayField(TEXT("background_color"));
			if (BgArr.Num() >= 3)
			{
				BgColor = FLinearColor(
					(float)BgArr[0]->AsNumber(),
					(float)BgArr[1]->AsNumber(),
					(float)BgArr[2]->AsNumber(),
					BgArr.Num() >= 4 ? (float)BgArr[3]->AsNumber() : 1.0f);
			}
		}

		bSuccess = CaptureMaterialFrame(Material, PreviewMesh, CameraLocation, CameraRotation,
			FOV, ResX, ResY, OutputPath, ESceneCaptureSource::SCS_FinalToneCurveHDR,
			UVTiling, BgColor);
	}
	else
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unsupported asset_type: %s (supported: niagara, material)"), *AssetType));
	}

	double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	if (!bSuccess)
	{
		return FMonolithActionResult::Error(TEXT("Capture failed — check log for details"));
	}

	// Return result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("output_file"), OutputPath);
	TSharedPtr<FJsonObject> ResObj = MakeShared<FJsonObject>();
	ResObj->SetNumberField(TEXT("width"), ResX);
	ResObj->SetNumberField(TEXT("height"), ResY);
	Result->SetObjectField(TEXT("resolution"), ResObj);
	Result->SetNumberField(TEXT("seek_time"), SeekTime);
	Result->SetNumberField(TEXT("capture_time_ms"), ElapsedMs);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleCaptureSequenceFrames(
	const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString AssetType = Params->GetStringField(TEXT("asset_type"));

	if (AssetPath.IsEmpty() || AssetType.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and asset_type are required"));
	}

	if (!Params->HasField(TEXT("timestamps")))
	{
		return FMonolithActionResult::Error(TEXT("timestamps array is required"));
	}

	// Parse timestamps
	TArray<float> Timestamps;
	const TArray<TSharedPtr<FJsonValue>>& TimestampArray = Params->GetArrayField(TEXT("timestamps"));
	for (const auto& Val : TimestampArray)
	{
		Timestamps.Add((float)Val->AsNumber());
	}
	Timestamps.Sort();

	if (Timestamps.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("timestamps array is empty"));
	}

	// Parse optional params (same as capture_scene_preview)
	int32 ResX = 512, ResY = 512;
	if (Params->HasField(TEXT("resolution")))
	{
		const TArray<TSharedPtr<FJsonValue>>& ResArray = Params->GetArrayField(TEXT("resolution"));
		if (ResArray.Num() >= 2)
		{
			ResX = (int32)ResArray[0]->AsNumber();
			ResY = (int32)ResArray[1]->AsNumber();
		}
	}

	FVector CameraLocation(200.0f, 0.0f, 100.0f);
	FRotator CameraRotation(0.0f, 180.0f, 0.0f);
	float FOV = 60.0f;
	// Same camera parsing as HandleCaptureScenePreview (with string fallback)
	if (Params->HasField(TEXT("camera")))
	{
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
		if (CameraObj && (*CameraObj).IsValid())
		{
			if ((*CameraObj)->HasField(TEXT("location")))
			{
				const TArray<TSharedPtr<FJsonValue>>& Loc = (*CameraObj)->GetArrayField(TEXT("location"));
				if (Loc.Num() >= 3) CameraLocation = FVector(Loc[0]->AsNumber(), Loc[1]->AsNumber(), Loc[2]->AsNumber());
			}
			if ((*CameraObj)->HasField(TEXT("rotation")))
			{
				const TArray<TSharedPtr<FJsonValue>>& Rot = (*CameraObj)->GetArrayField(TEXT("rotation"));
				if (Rot.Num() >= 3) CameraRotation = FRotator(Rot[0]->AsNumber(), Rot[1]->AsNumber(), Rot[2]->AsNumber());
			}
			if ((*CameraObj)->HasField(TEXT("fov")))
			{
				FOV = (float)(*CameraObj)->GetNumberField(TEXT("fov"));
			}
		}
	}

	FString OutputDir;
	if (Params->HasField(TEXT("output_dir")))
	{
		OutputDir = Params->GetStringField(TEXT("output_dir"));
		if (FPaths::IsRelative(OutputDir))
		{
			OutputDir = FPaths::ProjectDir() / OutputDir;
		}
	}
	else
	{
		FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		FString SafeName = FPaths::GetBaseFilename(AssetPath);
		OutputDir = FPaths::ProjectDir() / TEXT("Saved/Screenshots/Monolith") /
			FString::Printf(TEXT("%s_%s"), *Timestamp, *SafeName);
	}

	FString FilenamePrefix = TEXT("frame");
	if (Params->HasField(TEXT("filename_prefix")))
	{
		FilenamePrefix = Params->GetStringField(TEXT("filename_prefix"));
	}

	// Currently only supports Niagara for multi-frame
	if (!AssetType.Equals(TEXT("niagara"), ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(TEXT("capture_sequence_frames currently only supports asset_type: niagara"));
	}

	// Already on game thread (UE HTTP server dispatches via FTicker)
	check(IsInGameThread());

	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
	if (!System)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load: %s"), *AssetPath));
	}

	bool bPersistent = Params->HasField(TEXT("persistent")) && Params->GetBoolField(TEXT("persistent"));

	double StartTime = FPlatformTime::Seconds();
	TArray<TSharedPtr<FJsonValue>> FrameResults;

	if (bPersistent)
	{
		// PERSISTENT MODE: Create component ONCE, advance through time, capture at intervals.
		// Preserves ribbons, particle accumulation, and inter-frame state.
		FPreviewScene::ConstructionValues CVs;
		CVs.bDefaultLighting = false;
		CVs.LightBrightness = 0.0f;
		CVs.SkyBrightness = 0.0f;
		TSharedPtr<FAdvancedPreviewScene> PreviewScene =
			MakeShareable(new FAdvancedPreviewScene(CVs));
		PreviewScene->SetFloorVisibility(false);
		PreviewScene->SetEnvironmentVisibility(false);

		UNiagaraComponent* NiagaraComp = NewObject<UNiagaraComponent>(
			GetTransientPackage(), NAME_None, RF_Transient);
		NiagaraComp->CastShadow = false;
		NiagaraComp->bCastDynamicShadow = false;
		NiagaraComp->SetAllowScalability(false);
		NiagaraComp->SetAsset(System);
		NiagaraComp->SetForceSolo(true);
		NiagaraComp->SetAgeUpdateMode(ENiagaraAgeUpdateMode::DesiredAge);
		NiagaraComp->SetCanRenderWhileSeeking(true);
		NiagaraComp->SetMaxSimTime(0.0f);
		NiagaraComp->Activate(true);

		PreviewScene->AddComponent(NiagaraComp, NiagaraComp->GetRelativeTransform());

		UWorld* World = NiagaraComp->GetWorld();
		const float TickDelta = 1.0f / 30.0f;
		float CurrentTime = 0.0f;

		// Sort timestamps to ensure we advance monotonically
		TArray<float> SortedTimestamps = Timestamps;
		SortedTimestamps.Sort();

		for (int32 i = 0; i < SortedTimestamps.Num(); i++)
		{
			float TargetTime = SortedTimestamps[i];

			// Advance from current time to target time
			NiagaraComp->SetSeekDelta(TickDelta);
			NiagaraComp->SeekToDesiredAge(TargetTime);

			if (World)
			{
				World->TimeSeconds = TargetTime;
				World->DeltaTimeSeconds = TickDelta;
				World->Tick(ELevelTick::LEVELTICK_PauseTick, TickDelta);
			}

			NiagaraComp->TickComponent(TickDelta, ELevelTick::LEVELTICK_All, nullptr);

			// GPU flush for particle buffers
			if (World)
			{
				World->SendAllEndOfFrameUpdates();
				if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(World))
				{
					WorldManager->FlushComputeAndDeferredQueues(true);
				}
			}
			FlushRenderingCommands();

			// Warm-up extra tick so GPU buffers are populated
			if (World)
			{
				World->Tick(ELevelTick::LEVELTICK_PauseTick, TickDelta);
				NiagaraComp->TickComponent(TickDelta, ELevelTick::LEVELTICK_All, nullptr);
				World->SendAllEndOfFrameUpdates();
				if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(World))
				{
					WorldManager->FlushComputeAndDeferredQueues(true);
				}
				FlushRenderingCommands();
			}

			// Set up capture component and render
			UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
			RT->InitAutoFormat(ResX, ResY);
			RT->ClearColor = FLinearColor::Black;
			RT->UpdateResourceImmediate(true);

			USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
				GetTransientPackage(), NAME_None, RF_Transient);
			CaptureComp->TextureTarget = RT;
			CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
			CaptureComp->bCaptureEveryFrame = false;
			CaptureComp->bCaptureOnMovement = false;
			CaptureComp->bAlwaysPersistRenderingState = true;
			CaptureComp->FOVAngle = FOV;
			CaptureComp->SetRelativeLocation(CameraLocation);
			CaptureComp->SetRelativeRotation(CameraRotation);

			PreviewScene->AddComponent(CaptureComp, FTransform::Identity);

			FString FramePath = OutputDir / FString::Printf(TEXT("%s_%03d_t%.2f.png"),
				*FilenamePrefix, i, TargetTime);

			bool bOk = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, FramePath);

			PreviewScene->RemoveComponent(CaptureComp);

			TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
			FrameObj->SetNumberField(TEXT("timestamp"), TargetTime);
			FrameObj->SetStringField(TEXT("file"), FramePath);
			FrameObj->SetBoolField(TEXT("success"), bOk);
			FrameResults.Add(MakeShared<FJsonValueObject>(FrameObj));

			CurrentTime = TargetTime;
		}

		// Cleanup
		PreviewScene->RemoveComponent(NiagaraComp);
		NiagaraComp->DeactivateImmediate();
	}
	else
	{
		// PER-FRAME MODE: Use CaptureNiagaraFrame per frame — the proven working path
		// (DesiredAge + warm-up ticks + GPU flush). Reliable but recreates component each frame.
		for (int32 i = 0; i < Timestamps.Num(); i++)
		{
			float T = Timestamps[i];
			FString FramePath = OutputDir / FString::Printf(TEXT("%s_%03d_t%.2f.png"),
				*FilenamePrefix, i, T);

			bool bOk = CaptureNiagaraFrame(System, T, CameraLocation, CameraRotation,
				FOV, ResX, ResY, FramePath);

			TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
			FrameObj->SetNumberField(TEXT("timestamp"), T);
			FrameObj->SetStringField(TEXT("file"), FramePath);
			FrameObj->SetBoolField(TEXT("success"), bOk);
			FrameResults.Add(MakeShared<FJsonValueObject>(FrameObj));
		}
	}

	double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("persistent_mode"), bPersistent);
	Result->SetArrayField(TEXT("frames"), FrameResults);
	Result->SetNumberField(TEXT("total_capture_time_ms"), ElapsedMs);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleImportTexture(
	const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath = Params->GetStringField(TEXT("source_path"));
	FString Destination = Params->GetStringField(TEXT("destination"));

	if (SourcePath.IsEmpty() || Destination.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("source_path and destination are required"));
	}

	// Verify source file exists
	if (!FPaths::FileExists(SourcePath))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Source file not found: %s"), *SourcePath));
	}

	// Import using AssetTools
	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	ImportData->Filenames.Add(SourcePath);
	ImportData->DestinationPath = FPackageName::GetLongPackagePath(Destination);
	ImportData->bReplaceExisting = true;

	FAssetToolsModule& AssetToolsModule =
		FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	TArray<UObject*> ImportedAssets = AssetToolsModule.Get().ImportAssetsAutomated(ImportData);

	if (ImportedAssets.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Import failed — no assets imported"));
	}

	UTexture2D* Texture = Cast<UTexture2D>(ImportedAssets[0]);
	if (!Texture)
	{
		return FMonolithActionResult::Error(TEXT("Imported asset is not a Texture2D"));
	}

	// Apply optional settings
	if (Params->HasField(TEXT("settings")))
	{
		const TSharedPtr<FJsonObject>* SettingsObj;
		// Handle string-serialized params (Claude Code quirk)
		TSharedPtr<FJsonObject> ParsedSettings;
		if (Params->TryGetObjectField(TEXT("settings"), SettingsObj))
		{
			ParsedSettings = *SettingsObj;
		}
		else
		{
			FString SettingsStr = Params->GetStringField(TEXT("settings"));
			if (!SettingsStr.IsEmpty())
			{
				ParsedSettings = FMonolithJsonUtils::Parse(SettingsStr);
			}
		}

		if (ParsedSettings.IsValid())
		{
			// Compression
			if (ParsedSettings->HasField(TEXT("compression")))
			{
				FString Comp = ParsedSettings->GetStringField(TEXT("compression"));
				if (Comp == TEXT("TC_Normalmap")) Texture->CompressionSettings = TC_Normalmap;
				else if (Comp == TEXT("TC_Masks")) Texture->CompressionSettings = TC_Masks;
				else if (Comp == TEXT("TC_HDR")) Texture->CompressionSettings = TC_HDR;
				else if (Comp == TEXT("TC_VectorDisplacementmap")) Texture->CompressionSettings = TC_VectorDisplacementmap;
				else Texture->CompressionSettings = TC_Default;
			}

			// sRGB
			if (ParsedSettings->HasField(TEXT("srgb")))
			{
				Texture->SRGB = ParsedSettings->GetBoolField(TEXT("srgb"));
			}

			// Tiling
			if (ParsedSettings->HasField(TEXT("tiling")))
			{
				if (ParsedSettings->GetBoolField(TEXT("tiling")))
				{
					Texture->AddressX = TA_Wrap;
					Texture->AddressY = TA_Wrap;
				}
			}

			// Max size
			if (ParsedSettings->HasField(TEXT("max_size")))
			{
				int32 MaxSize = (int32)ParsedSettings->GetNumberField(TEXT("max_size"));
				if (MaxSize > 0)
				{
					Texture->MaxTextureSize = MaxSize;
				}
			}

			// LOD group
			if (ParsedSettings->HasField(TEXT("lod_group")))
			{
				FString LODGroup = ParsedSettings->GetStringField(TEXT("lod_group"));
				if (LODGroup == TEXT("TEXTUREGROUP_WorldNormalMap")) Texture->LODGroup = TEXTUREGROUP_WorldNormalMap;
				else if (LODGroup == TEXT("TEXTUREGROUP_Effects")) Texture->LODGroup = TEXTUREGROUP_Effects;
				else if (LODGroup == TEXT("TEXTUREGROUP_EffectsNotFiltered")) Texture->LODGroup = TEXTUREGROUP_EffectsNotFiltered;
				// Default: TEXTUREGROUP_World (already default)
			}
		}
	}

	Texture->UpdateResource();
	Texture->PostEditChange();
	Texture->MarkPackageDirty();

	// Save the package
	UPackage* Package = Texture->GetOutermost();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	UPackage::SavePackage(Package, Texture, *PackageFilename, SaveArgs);

	// Return result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), Destination);
	Result->SetNumberField(TEXT("size_x"), Texture->GetSizeX());
	Result->SetNumberField(TEXT("size_y"), Texture->GetSizeY());
	Result->SetStringField(TEXT("format"), GPixelFormats[Texture->GetPixelFormat()].Name);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleStitchFlipbook(
	const TSharedPtr<FJsonObject>& Params)
{
	// --- Extract required params ---
	FString DestPath = Params->GetStringField(TEXT("dest_path"));
	if (DestPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("dest_path is required"));
	}

	// Parse frame_paths array
	const TArray<TSharedPtr<FJsonValue>>* FramePathsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("frame_paths"), FramePathsArray) || !FramePathsArray || FramePathsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("frame_paths array is required and must not be empty"));
	}

	TArray<FString> FramePaths;
	for (const auto& Val : *FramePathsArray)
	{
		FString Path;
		if (Val->TryGetString(Path) && !Path.IsEmpty())
		{
			FramePaths.Add(Path);
		}
	}

	if (FramePaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No valid file paths in frame_paths"));
	}

	// Parse grid [cols, rows]
	const TArray<TSharedPtr<FJsonValue>>* GridArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("grid"), GridArray) || !GridArray || GridArray->Num() != 2)
	{
		return FMonolithActionResult::Error(TEXT("grid must be an array of [columns, rows]"));
	}
	int32 GridCols = static_cast<int32>((*GridArray)[0]->AsNumber());
	int32 GridRows = static_cast<int32>((*GridArray)[1]->AsNumber());

	if (GridCols <= 0 || GridRows <= 0)
	{
		return FMonolithActionResult::Error(TEXT("grid columns and rows must be positive"));
	}

	int32 ExpectedFrames = GridCols * GridRows;
	if (FramePaths.Num() != ExpectedFrames)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("frame_paths has %d entries but grid %dx%d expects %d"),
			FramePaths.Num(), GridCols, GridRows, ExpectedFrames));
	}

	// Optional params
	bool bSRGB = !Params->HasField(TEXT("srgb")) || Params->GetBoolField(TEXT("srgb"));
	bool bNoMipmaps = !Params->HasField(TEXT("no_mipmaps")) || Params->GetBoolField(TEXT("no_mipmaps"));
	bool bDeleteSources = !Params->HasField(TEXT("delete_sources")) || Params->GetBoolField(TEXT("delete_sources"));

	FString LODGroupStr = TEXT("TEXTUREGROUP_Effects");
	if (Params->HasField(TEXT("lod_group")))
	{
		LODGroupStr = Params->GetStringField(TEXT("lod_group"));
	}

	// --- Load all frame images ---
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

	int32 FrameWidth = 0;
	int32 FrameHeight = 0;
	TArray<TArray<FColor>> FramePixels;
	FramePixels.SetNum(FramePaths.Num());

	for (int32 i = 0; i < FramePaths.Num(); i++)
	{
		const FString& FilePath = FramePaths[i];

		if (!FPaths::FileExists(FilePath))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Frame file not found: %s"), *FilePath));
		}

		TArray<uint8> FileData;
		if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to read frame file: %s"), *FilePath));
		}

		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!ImageWrapper.IsValid() || !ImageWrapper->SetCompressed(FileData.GetData(), FileData.Num()))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to decode PNG: %s"), *FilePath));
		}

		int32 W = ImageWrapper->GetWidth();
		int32 H = ImageWrapper->GetHeight();

		// Validate all frames are same size
		if (i == 0)
		{
			FrameWidth = W;
			FrameHeight = H;
		}
		else if (W != FrameWidth || H != FrameHeight)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Frame %d (%s) is %dx%d but frame 0 is %dx%d — all frames must be the same size"),
				i, *FilePath, W, H, FrameWidth, FrameHeight));
		}

		TArray<uint8> RawData;
		if (!ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, RawData))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to decompress frame %d: %s"), i, *FilePath));
		}

		// Convert raw bytes to FColor array
		FramePixels[i].SetNum(W * H);
		FMemory::Memcpy(FramePixels[i].GetData(), RawData.GetData(), W * H * sizeof(FColor));
	}

	// --- Compose atlas ---
	int32 AtlasWidth = FrameWidth * GridCols;
	int32 AtlasHeight = FrameHeight * GridRows;
	TArray<FColor> AtlasPixels;
	AtlasPixels.SetNumZeroed(AtlasWidth * AtlasHeight);

	for (int32 FrameIdx = 0; FrameIdx < FramePaths.Num(); FrameIdx++)
	{
		int32 Col = FrameIdx % GridCols;
		int32 Row = FrameIdx / GridCols;
		int32 OffsetX = Col * FrameWidth;
		int32 OffsetY = Row * FrameHeight;

		const TArray<FColor>& Src = FramePixels[FrameIdx];
		for (int32 Y = 0; Y < FrameHeight; Y++)
		{
			for (int32 X = 0; X < FrameWidth; X++)
			{
				int32 SrcIdx = Y * FrameWidth + X;
				int32 DstIdx = (OffsetY + Y) * AtlasWidth + (OffsetX + X);
				AtlasPixels[DstIdx] = Src[SrcIdx];
			}
		}
	}

	// --- Create UTexture2D ---
	FString PackagePath = FPackageName::GetLongPackagePath(DestPath);
	FString AssetName = FPackageName::GetLongPackageAssetName(DestPath);

	// Ensure unique package name
	FString PackageName = PackagePath / AssetName;
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to create package: %s"), *PackageName));
	}
	Package->FullyLoad();

	UTexture2D* Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!Texture)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UTexture2D"));
	}

	// Configure platform data
	FTexturePlatformData* PlatformData = new FTexturePlatformData();
	PlatformData->SizeX = AtlasWidth;
	PlatformData->SizeY = AtlasHeight;
	PlatformData->PixelFormat = PF_B8G8R8A8;
	PlatformData->SetNumSlices(1);

	FTexture2DMipMap* Mip = new FTexture2DMipMap();
	Mip->SizeX = AtlasWidth;
	Mip->SizeY = AtlasHeight;
	PlatformData->Mips.Add(Mip);

	// Copy pixel data into mip
	Mip->BulkData.Lock(LOCK_READ_WRITE);
	void* MipData = Mip->BulkData.Realloc(AtlasWidth * AtlasHeight * sizeof(FColor));
	FMemory::Memcpy(MipData, AtlasPixels.GetData(), AtlasWidth * AtlasHeight * sizeof(FColor));
	Mip->BulkData.Unlock();

	Texture->SetPlatformData(PlatformData);

	// Apply texture settings
	Texture->Source.Init(AtlasWidth, AtlasHeight, 1, 1, TSF_BGRA8, nullptr);
	{
		uint8* SourceData = Texture->Source.LockMip(0);
		FMemory::Memcpy(SourceData, AtlasPixels.GetData(), AtlasWidth * AtlasHeight * sizeof(FColor));
		Texture->Source.UnlockMip(0);
	}

	Texture->SRGB = bSRGB;
	Texture->CompressionSettings = TC_Default;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;

	if (bNoMipmaps)
	{
		Texture->MipGenSettings = TMGS_NoMipmaps;
	}

	// LOD group
	if (LODGroupStr == TEXT("TEXTUREGROUP_Effects"))
	{
		Texture->LODGroup = TEXTUREGROUP_Effects;
	}
	else if (LODGroupStr == TEXT("TEXTUREGROUP_EffectsNotFiltered"))
	{
		Texture->LODGroup = TEXTUREGROUP_EffectsNotFiltered;
	}
	else if (LODGroupStr == TEXT("TEXTUREGROUP_World"))
	{
		Texture->LODGroup = TEXTUREGROUP_World;
	}

	Texture->UpdateResource();
	Texture->PostEditChange();
	Texture->MarkPackageDirty();

	// Save
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	bool bSaved = UPackage::SavePackage(Package, Texture, *PackageFilename, SaveArgs);

	if (!bSaved)
	{
		return FMonolithActionResult::Error(TEXT("Failed to save flipbook texture package"));
	}

	// --- Delete source files if requested ---
	int32 DeletedCount = 0;
	if (bDeleteSources)
	{
		for (const FString& FilePath : FramePaths)
		{
			if (IFileManager::Get().Delete(*FilePath))
			{
				DeletedCount++;
			}
		}
	}

	// --- Return result ---
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("texture_path"), DestPath);

	TArray<TSharedPtr<FJsonValue>> ResArray;
	ResArray.Add(MakeShared<FJsonValueNumber>(AtlasWidth));
	ResArray.Add(MakeShared<FJsonValueNumber>(AtlasHeight));
	Result->SetArrayField(TEXT("resolution"), ResArray);

	Result->SetNumberField(TEXT("frame_count"), FramePaths.Num());
	Result->SetNumberField(TEXT("frame_width"), FrameWidth);
	Result->SetNumberField(TEXT("frame_height"), FrameHeight);

	TArray<TSharedPtr<FJsonValue>> GridResult;
	GridResult.Add(MakeShared<FJsonValueNumber>(GridCols));
	GridResult.Add(MakeShared<FJsonValueNumber>(GridRows));
	Result->SetArrayField(TEXT("grid"), GridResult);

	if (bDeleteSources)
	{
		Result->SetNumberField(TEXT("sources_deleted"), DeletedCount);
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleGetViewportInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	// Get the active level editor viewport
	FLevelEditorViewportClient* ViewportClient = nullptr;
	if (GEditor && GEditor->GetLevelViewportClients().Num() > 0)
	{
		ViewportClient = GEditor->GetLevelViewportClients()[0];
	}

	if (!ViewportClient)
	{
		return FMonolithActionResult::Error(TEXT("No active viewport found"));
	}

	FVector CamLocation = ViewportClient->GetViewLocation();
	FRotator CamRotation = ViewportClient->GetViewRotation();
	float FOV = ViewportClient->ViewFOV;

	FIntPoint ViewportSize = ViewportClient->Viewport->GetSizeXY();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("active_viewport"), 0);

	TSharedPtr<FJsonObject> ResObj = MakeShared<FJsonObject>();
	ResObj->SetNumberField(TEXT("width"), ViewportSize.X);
	ResObj->SetNumberField(TEXT("height"), ViewportSize.Y);
	Result->SetObjectField(TEXT("resolution"), ResObj);

	TArray<TSharedPtr<FJsonValue>> LocArr;
	LocArr.Add(MakeShared<FJsonValueNumber>(CamLocation.X));
	LocArr.Add(MakeShared<FJsonValueNumber>(CamLocation.Y));
	LocArr.Add(MakeShared<FJsonValueNumber>(CamLocation.Z));
	Result->SetArrayField(TEXT("camera_location"), LocArr);

	TArray<TSharedPtr<FJsonValue>> RotArr;
	RotArr.Add(MakeShared<FJsonValueNumber>(CamRotation.Pitch));
	RotArr.Add(MakeShared<FJsonValueNumber>(CamRotation.Yaw));
	RotArr.Add(MakeShared<FJsonValueNumber>(CamRotation.Roll));
	Result->SetArrayField(TEXT("camera_rotation"), RotArr);

	Result->SetNumberField(TEXT("fov"), FOV);
	Result->SetBoolField(TEXT("realtime"), ViewportClient->IsRealtime());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleDeleteAssets(
	const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* AssetPathsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("asset_paths"), AssetPathsArray) || !AssetPathsArray || AssetPathsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("asset_paths array is required and must not be empty"));
	}

	TArray<FString> AssetPaths;
	for (const auto& Val : *AssetPathsArray)
	{
		FString Path;
		if (Val->TryGetString(Path) && !Path.IsEmpty())
		{
			AssetPaths.Add(Path);
		}
	}

	if (AssetPaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No valid paths in asset_paths"));
	}

	// Optional safety: restrict deletion to allowed prefixes
	TArray<FString> AllowedPrefixes;
	const TArray<TSharedPtr<FJsonValue>>* PrefixArray = nullptr;
	if (Params->TryGetArrayField(TEXT("allowed_prefixes"), PrefixArray) && PrefixArray)
	{
		for (const auto& PVal : *PrefixArray)
		{
			FString Prefix;
			if (PVal->TryGetString(Prefix) && !Prefix.IsEmpty())
			{
				AllowedPrefixes.Add(Prefix);
			}
		}
	}

	if (AllowedPrefixes.Num() > 0)
	{
		for (const FString& Path : AssetPaths)
		{
			bool bAllowed = false;
			for (const FString& Prefix : AllowedPrefixes)
			{
				if (Path.StartsWith(Prefix))
				{
					bAllowed = true;
					break;
				}
			}
			if (!bAllowed)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Refusing to delete '%s' — not under any allowed prefix. Allowed: %s"),
					*Path, *FString::Join(AllowedPrefixes, TEXT(", "))));
			}
		}
	}

	// Load and delete each asset
	TArray<UObject*> ObjectsToDelete;
	TArray<FString> NotFound;

	for (const FString& Path : AssetPaths)
	{
		UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);
		if (Asset)
		{
			ObjectsToDelete.Add(Asset);
		}
		else
		{
			NotFound.Add(Path);
		}
	}

	int32 NumDeleted = 0;
	if (ObjectsToDelete.Num() > 0)
	{
		NumDeleted = ObjectTools::DeleteObjects(ObjectsToDelete, /*bShowConfirmation=*/false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), NumDeleted == ObjectsToDelete.Num() && NotFound.Num() == 0);
	Result->SetNumberField(TEXT("deleted"), NumDeleted);
	Result->SetNumberField(TEXT("requested"), AssetPaths.Num());
	Result->SetNumberField(TEXT("found"), ObjectsToDelete.Num());

	if (NotFound.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> NotFoundArr;
		for (const FString& P : NotFound)
		{
			NotFoundArr.Add(MakeShared<FJsonValueString>(P));
		}
		Result->SetArrayField(TEXT("not_found"), NotFoundArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Action: capture_system_gif
// Captures a Niagara system as a sequence of PNG frames with optional GIF encoding.
// Default mode: frames_only — returns array of PNG paths (always works, no deps).
// Optional: encoder: "ffmpeg" or "python" for GIF encoding.
// ============================================================================
FMonolithActionResult FMonolithEditorActions::HandleCaptureSystemGif(
	const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("asset_path is required"));

	double DurationSeconds = Params->HasField(TEXT("duration_seconds")) ? Params->GetNumberField(TEXT("duration_seconds")) : 2.0;
	int32 FPS = Params->HasField(TEXT("fps")) ? static_cast<int32>(Params->GetNumberField(TEXT("fps"))) : 15;
	int32 Resolution = Params->HasField(TEXT("resolution")) ? static_cast<int32>(Params->GetNumberField(TEXT("resolution"))) : 256;
	FString Encoder = Params->HasField(TEXT("encoder")) ? Params->GetStringField(TEXT("encoder")).ToLower() : TEXT("frames_only");

	if (FPS <= 0) FPS = 15;
	if (Resolution <= 0) Resolution = 256;
	if (DurationSeconds <= 0) DurationSeconds = 2.0;

	// Output directory
	FString OutputDir;
	if (Params->HasField(TEXT("output_path")))
	{
		OutputDir = Params->GetStringField(TEXT("output_path"));
		if (FPaths::IsRelative(OutputDir))
		{
			OutputDir = FPaths::ProjectDir() / OutputDir;
		}
	}
	else
	{
		FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		FString SafeName = FPaths::GetBaseFilename(AssetPath);
		OutputDir = FPaths::ProjectDir() / TEXT("Saved/Screenshots/Monolith") /
			FString::Printf(TEXT("GIF_%s_%s"), *Timestamp, *SafeName);
	}
	IFileManager::Get().MakeDirectory(*OutputDir, true);

	// Load system
	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
	if (!System)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load Niagara system: %s"), *AssetPath));

	// Generate timestamps
	int32 FrameCount = FMath::Max(1, static_cast<int32>(DurationSeconds * FPS));
	TArray<float> Timestamps;
	for (int32 i = 0; i < FrameCount; i++)
	{
		Timestamps.Add(static_cast<float>(i) / static_cast<float>(FPS));
	}

	// Build params for capture_sequence_frames (persistent mode)
	TArray<TSharedPtr<FJsonValue>> TimestampValues;
	for (float T : Timestamps)
	{
		TimestampValues.Add(MakeShared<FJsonValueNumber>(T));
	}

	TSharedRef<FJsonObject> CaptureParams = MakeShared<FJsonObject>();
	CaptureParams->SetStringField(TEXT("asset_path"), AssetPath);
	CaptureParams->SetStringField(TEXT("asset_type"), TEXT("niagara"));
	CaptureParams->SetArrayField(TEXT("timestamps"), TimestampValues);
	CaptureParams->SetStringField(TEXT("output_dir"), OutputDir);
	CaptureParams->SetStringField(TEXT("filename_prefix"), TEXT("gif_frame"));
	CaptureParams->SetBoolField(TEXT("persistent"), true);

	// Set resolution
	TArray<TSharedPtr<FJsonValue>> ResArr;
	ResArr.Add(MakeShared<FJsonValueNumber>(Resolution));
	ResArr.Add(MakeShared<FJsonValueNumber>(Resolution));
	CaptureParams->SetArrayField(TEXT("resolution"), ResArr);

	// Capture frames
	FMonolithActionResult CaptureResult = HandleCaptureSequenceFrames(CaptureParams);
	if (!CaptureResult.bSuccess)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Frame capture failed: %s"), *CaptureResult.ErrorMessage));

	// Collect frame paths from the capture result
	TArray<FString> FramePaths;
	const TArray<TSharedPtr<FJsonValue>>* FramesArr = nullptr;
	if (CaptureResult.Result.IsValid() && CaptureResult.Result->TryGetArrayField(TEXT("frames"), FramesArr))
	{
		for (const auto& FV : *FramesArr)
		{
			const TSharedPtr<FJsonObject> FrameObj = FV->AsObject();
			if (FrameObj.IsValid())
			{
				FString FilePath = FrameObj->GetStringField(TEXT("file"));
				if (!FilePath.IsEmpty())
					FramePaths.Add(FilePath);
			}
		}
	}

	// Build result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("frame_count"), FramePaths.Num());
	Result->SetNumberField(TEXT("duration"), DurationSeconds);
	Result->SetNumberField(TEXT("fps"), FPS);
	Result->SetNumberField(TEXT("resolution"), Resolution);
	Result->SetStringField(TEXT("output_dir"), OutputDir);

	// Always include frame paths
	TArray<TSharedPtr<FJsonValue>> PathArr;
	for (const FString& P : FramePaths)
		PathArr.Add(MakeShared<FJsonValueString>(P));
	Result->SetArrayField(TEXT("frame_paths"), PathArr);

	// Optional GIF encoding
	if (Encoder != TEXT("frames_only") && FramePaths.Num() > 0)
	{
		FString GifPath = OutputDir / TEXT("output.gif");

		if (Encoder == TEXT("ffmpeg"))
		{
			FString InputPattern = OutputDir / TEXT("gif_frame_%04d.png");
			FString FFmpegArgs = FString::Printf(
				TEXT("-y -framerate %d -i \"%s\" -vf \"scale=%d:-1:flags=lanczos\" -loop 0 \"%s\""),
				FPS, *InputPattern, Resolution, *GifPath);

			FString FFmpegPath = TEXT("ffmpeg");
			int32 ReturnCode = -1;
			FString StdOut, StdErr;

			// Try to run ffmpeg
			bool bLaunched = FPlatformProcess::ExecProcess(*FFmpegPath, *FFmpegArgs, &ReturnCode, &StdOut, &StdErr);

			if (bLaunched && ReturnCode == 0 && IFileManager::Get().FileExists(*GifPath))
			{
				Result->SetStringField(TEXT("gif_path"), GifPath);
				Result->SetStringField(TEXT("encoder_used"), TEXT("ffmpeg"));
			}
			else
			{
				Result->SetStringField(TEXT("encoder_error"),
					FString::Printf(TEXT("ffmpeg failed (code %d). Ensure ffmpeg is in PATH. stderr: %s"),
						ReturnCode, *StdErr.Left(500)));
			}
		}
		else if (Encoder == TEXT("python"))
		{
			// Build a quick python one-liner using imageio
			FString FrameListStr;
			for (const FString& P : FramePaths)
			{
				if (!FrameListStr.IsEmpty()) FrameListStr += TEXT(",");
				FString Escaped = P;
				Escaped.ReplaceInline(TEXT("\\"), TEXT("/"));
				FrameListStr += FString::Printf(TEXT("'%s'"), *Escaped);
			}

			FString PyScript = FString::Printf(
				TEXT("import imageio; frames=[imageio.imread(p) for p in [%s]]; imageio.mimsave('%s',frames,duration=%f,loop=0)"),
				*FrameListStr,
				*GifPath.Replace(TEXT("\\"), TEXT("/")),
				1.0 / FPS);

			FString PythonPath = TEXT("python");
			FString PythonArgs = FString::Printf(TEXT("-c \"%s\""), *PyScript);

			int32 ReturnCode = -1;
			FString StdOut, StdErr;
			bool bLaunched = FPlatformProcess::ExecProcess(*PythonPath, *PythonArgs, &ReturnCode, &StdOut, &StdErr);

			if (bLaunched && ReturnCode == 0 && IFileManager::Get().FileExists(*GifPath))
			{
				Result->SetStringField(TEXT("gif_path"), GifPath);
				Result->SetStringField(TEXT("encoder_used"), TEXT("python"));
			}
			else
			{
				Result->SetStringField(TEXT("encoder_error"),
					FString::Printf(TEXT("python imageio failed (code %d). Ensure python + imageio are installed. stderr: %s"),
						ReturnCode, *StdErr.Left(500)));
			}
		}
		else
		{
			Result->SetStringField(TEXT("encoder_error"),
				FString::Printf(TEXT("Unknown encoder '%s'. Valid: frames_only, ffmpeg, python"), *Encoder));
		}
	}

	return FMonolithActionResult::Success(Result);
}
