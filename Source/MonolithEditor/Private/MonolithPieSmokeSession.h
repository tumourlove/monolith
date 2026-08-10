#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/WeakObjectPtr.h"

class UWorld;
class APawn;
class FMonolithLogCapture;
class FJsonValue;

// One timestamped sample of the named AnimInstance variables on the tracked pawn.
// Values are stored loosely (float OR bool) keyed by the variable name; the poll
// report summarises min/max/last per name.
struct FPieSmokeSampleVar
{
	FString Name;
	bool bIsBool = false;
	double NumberValue = 0.0; // valid when !bIsBool
	bool BoolValue = false;   // valid when bIsBool
};

struct FPieSmokeSample
{
	double TimeSeconds = 0.0; // seconds since the session's marker
	TArray<FPieSmokeSampleVar> Vars;
	// Clip variant only: path of the viewport frame captured at this sample (empty otherwise).
	FString FramePath;
	// #7 per-frame validity: a captured frame that is all-black or a single uniform colour
	// is treated as an unrendered (invalid) capture. Only meaningful when FramePath is set.
	bool bFrameValid = false;
	bool bFrameUniform = false; // single solid colour (incl. all-black) => not a real render
};

// #8 staged startup-Python / console hooks. Each stage fires AT MOST ONCE from the
// per-frame observer (reusing the probe firing mechanism), except PrePie which fires
// synchronously before PIE starts. Empty Python+Console => the stage is inert.
//   PrePie       : before StartPieInternal (driven synchronously by the handler).
//   OnBeginPlay  : first observer tick where the PIE world HasBegunPlay.
//   AfterNTicks  : first observer tick at/after FireAfterTicks observer ticks.
//   BeforeCapture: first observer tick a clip frame is about to be captured.
struct FPieSmokeStage
{
	FString Python;
	TArray<FString> Console;
	int32 FireAfterTicks = 0; // AfterNTicks only
	bool bFired = false;
	double FiredAtSeconds = -1.0;
	bool bPythonOk = false;
	FString PythonOutput;
};

// #8 the four staged hooks, keyed by lifecycle moment. Stored on the session; fired by
// the observer (PrePie excepted — the handler runs it synchronously before PIE start).
struct FPieSmokeStages
{
	FPieSmokeStage PrePie;
	FPieSmokeStage OnBeginPlay;
	FPieSmokeStage AfterNTicks;
	FPieSmokeStage BeforeCapture;
	bool bAny = false; // true if any stage carries a script (skip observer work otherwise)
};

// #9 clip runtime-identity snapshot. Cached the first sampled tick a valid AnimInstance
// is found, then re-checked each sampled tick so anim_class_changed can be detected.
struct FPieSmokeRuntimeIdentity
{
	bool bResolved = false;             // a target actor + skel comp were found at least once
	FString ActorName;
	FString ActorClass;
	FString SkelCompName;
	FString AnimInstanceClassPath;      // Anim->GetClass()->GetPathName()
	FString MeshAnimClassPath;          // SkelComp->GetAnimClass()->GetPathName()
	FString AnimationMode;              // single_node | anim_blueprint | custom | none
	bool bAnimClassChanged = false;     // MeshAnimClassPath differed across sampled ticks

	// #9 optional assert: when ExpectedAnimClass is set, bExpectedMismatch flags when the
	// live MeshAnimClassPath does not contain it (substring, path-or-name tolerant).
	FString ExpectedAnimClass;
	bool bExpectedChecked = false;
	bool bExpectedMismatch = false;
};

enum class EPieSmokeStatus : uint8
{
	Running,
	Complete,
	Stopped,
	Error,
};

// #3 grouped log-pattern semantics. Each group is a list of case-insensitive
// substring patterns matched against post-marker log lines.
//   MustAbsent  : any match fails ok (the legacy flat-array behaviour).
//   MustPresent : every pattern must match >= 1 for ok.
//   ObserveOnly : counted + reported, never affects ok.
//   Warn        : counted + surfaced in a warnings list, never affects ok.
struct FPieSmokeLogGroups
{
	TArray<FString> MustAbsent;
	TArray<FString> MustPresent;
	TArray<FString> ObserveOnly;
	TArray<FString> Warn;
};

// #4 a delayed in-session probe: fire `Python` (and/or `Console`) against the LIVE
// PIE world once session-elapsed reaches AtSeconds. Stored per session with a fired
// flag so the per-frame observer runs each probe exactly once.
struct FPieSmokeProbe
{
	double AtSeconds = 0.0;
	FString Python;
	TArray<FString> Console;
	bool bFired = false;
	double FiredAtSeconds = -1.0;
	bool bPythonOk = false;
	FString PythonOutput; // CommandResult / exception trace, when available
};

// Gap 9: a typed timed provocation fired ONCE against the live PIE world when session
// elapsed crosses AtSeconds. Generalises the probe_scripts "fire-once" mechanism into
// first-class typed actions (set_control_rotation / add_movement_input / jump /
// console_command). Params are interpreted per Action; outcome stamped for the report.
enum class EPieProvocationAction : uint8
{
	SetControlRotation,  // APlayerController::SetControlRotation({pitch,yaw,roll})
	AddMovementInput,    // APawn::AddMovementInput({x,y,z} direction, scale)
	Jump,                // ACharacter::Jump()
	ConsoleCommand,      // PC->ConsoleCommand(command) on the PIE world
	Unknown,
};

struct FPieProvocation
{
	double AtSeconds = 0.0;
	EPieProvocationAction Action = EPieProvocationAction::Unknown;
	FString RawAction;                    // echoed action token for the report

	// set_control_rotation
	FRotator Rotation = FRotator::ZeroRotator;
	// add_movement_input
	FVector Direction = FVector::ForwardVector;
	double Scale = 1.0;
	// console_command
	FString Command;

	bool bFired = false;
	double FiredAtSeconds = -1.0;
	bool bDispatched = false;             // a target was found and the call was made
	FString Result;                       // outcome / why-not token
};

// Gap 9: one timestamped sample of the dotted (UDS-friendly) variable paths read off the
// resolved timeseries target. Stored as pre-serialised JSON values so per-tick reads carry
// no extra read-back pass; the poll report assembles them under {t, vars:{...}}.
struct FPieTimeseriesSample
{
	double TimeSeconds = 0.0;
	// One entry per requested variable path that resolved on this tick. Value is the
	// already-serialised JSON value from the shared PIE-object reader.
	TArray<TPair<FString, TSharedPtr<FJsonValue>>> Vars;
};

// Phase 8 (OG-E2/E5): one entry of the declarative `actor_setup` block. Generic
// spawn-and-configure spec executed ONCE against the LIVE PIE world on the first
// ready observer tick. Fully general-purpose: ClassPath is any BP/native actor
// class, ApplyDataAssetPath is any UObject DataAsset whose reflected fields are
// copied onto matching-named actor properties.
struct FPieSmokeActorSetupEntry
{
	FString ClassPath;                 // BP (/Game/.../BP_Foo[.BP_Foo_C]) or native class path
	int32 Count = 1;                   // number of actors to spawn (clamped >= 1)
	TArray<FVector> Locations;         // per-actor spawn locations; index i falls back to origin
	FString ApplyDataAssetPath;        // optional DataAsset whose fields are copied onto each actor
	bool bHasMoveTo = false;           // true when MoveTo was supplied
	FVector MoveTo = FVector::ZeroVector; // AAIController::MoveToLocation destination
};

// Phase 8: per-actor outcome of one spawned actor inside an actor_setup entry. Captured
// at execution time so poll/stop can return a STRUCTURED report (callers distinguish a
// partial apply from a full one programmatically — not via a log line).
struct FPieSmokeSpawnedActorResult
{
	bool bSpawned = false;
	FString ActorName;
	FString RuntimeClassPath;          // resolved spawned class path (BP_Foo_C etc.)
	FString SpawnError;                // populated when bSpawned == false

	// DataAsset field -> actor property apply outcome (the mapping is NOT 1:1).
	TArray<FString> AppliedFields;     // fields copied onto a matching actor prop
	TArray<FString> UnmatchedFields;   // DA fields the actor did not declare (name+type)

	// AI move outcome (only meaningful when the entry requested move_to).
	bool bMoveRequested = false;
	bool bMoveIssued = false;          // a controller was found and MoveToLocation was called
	FString MoveResult;               // request-result token, or why no move was issued
};

// Phase 8: result of executing one actor_setup entry (spawned actors + class resolution).
struct FPieSmokeActorSetupResult
{
	FString ClassPath;                 // echoed request class
	bool bClassResolved = false;       // the class path loaded to a UClass
	FString ApplyDataAssetPath;        // echoed request DataAsset (empty when none)
	bool bDataAssetLoaded = false;     // the DataAsset path loaded (only when requested)
	FString DataAssetError;            // why the DataAsset failed to load (when requested + failed)
	int32 RequestedCount = 0;
	int32 SpawnedCount = 0;
	TArray<FPieSmokeSpawnedActorResult> Actors;
};

// A single async PIE-smoke session. Advanced exclusively by the frame observer
// (FTSTicker callback) running inside the editor's REAL frame — never by an MCP
// handler. All access is game-thread-only, so no locking is required.
struct FPieSmokeSession
{
	FString Id;
	EPieSmokeStatus Status = EPieSmokeStatus::Running;

	double StartTimeSeconds = 0.0;   // FPlatformTime::Seconds() at the marker emit
	double DurationSeconds = 5.0;
	double LastObservedSeconds = 0.0;

	FString Marker;
	FString MapName;
	TArray<FString> LogPatterns; // legacy flat list (kept for back-compat reporting)
	TArray<FString> SampleVarNames;

	// #3 grouped patterns (flat LogPatterns are mirrored into LogGroups.MustAbsent).
	FPieSmokeLogGroups LogGroups;

	// #10 teardown-vs-active-runtime bucketing. The first log line containing
	// IgnoreAfterPattern (default "BeginTearingDown") splits post-marker entries into
	// an active-runtime bucket (before) and a teardown bucket (after). ok is computed
	// from the active-runtime bucket only. When bTeardownAllowed is true, teardown-bucket
	// hits never affect ok (they are merely reported).
	FString IgnoreAfterPattern = TEXT("BeginTearingDown");
	bool bTeardownAllowed = true;

	// #4 delayed in-session probes, fired by the per-frame observer.
	TArray<FPieSmokeProbe> Probes;

	// Gap 9: time-series variant. When bTimeseries is true the observer reads the dotted
	// (UDS-friendly) TimeseriesVarPaths off the resolved target each sample tick (gated by
	// SampleInterval), fires typed Provocations at their times, and accumulates
	// TimeseriesSamples — instead of the flat anim-var sampling the smoke path uses.
	bool bTimeseries = false;
	TArray<FString> TimeseriesVarPaths;       // dotted UDS member paths to read each tick
	double SampleInterval = 0.0;              // min seconds between samples (0 = per-tick)
	double LastSampleSeconds = -1.0;          // last sample-relative time a sample was taken
	int32 MaxSamples = 2048;                  // hard cap on accumulated samples (payload guard)

	// Target selector for the timeseries read (resolved via the shared PIE-object resolver,
	// reused by Gap 8). actor_label/object_name/class_name + optional component/anim_instance.
	FString TargetActorLabel;
	FString TargetObjectName;
	FString TargetClassName;
	FString TargetComponentName;
	bool bTargetAnimInstance = false;

	// Cached resolved timeseries target. The full actor+component scan runs only when this
	// weak pointer is stale (first tick, or after the target actor dies); a still-valid
	// cached pointer is reused, so per-tick sampling is O(1) instead of O(actors x ticks).
	TWeakObjectPtr<UObject> CachedTimeseriesTarget;

	TArray<FPieProvocation> Provocations;     // typed timed provocations, fired once each
	TArray<FPieTimeseriesSample> TimeseriesSamples;

	// Phase 8 (OG-E2/E5): declarative actor_setup spec. Executed ONCE on the first ready
	// observer tick (after BeginPlay) against the LIVE PIE world; results captured in
	// ActorSetupResults for the poll/stop report. bActorSetupFired guards single-fire.
	TArray<FPieSmokeActorSetupEntry> ActorSetups;
	TArray<FPieSmokeActorSetupResult> ActorSetupResults;
	bool bActorSetupFired = false;

	// #11 explicit lifecycle string, derived at report time from (Status, bPieActive,
	// resident PIE world). One of: running | capture-complete-pie-open |
	// teardown-started | teardown-complete | stopped-by-tool.
	bool bStoppedByTool = false;     // set by Stop() so lifecycle reports stopped-by-tool
	bool bTeardownStarted = false;   // set when RequestEndPlayMap is driven for this set

	// Resolved lazily on the first tick where the PIE pawn exists.
	TWeakObjectPtr<APawn> TargetPawn;
	// Optional pawn-class name filter (resolves a pawn whose class name contains this).
	FString PawnClassFilter;

	TArray<FPieSmokeSample> Samples;

	bool bPieActive = true;          // cleared by the EndPIE / PrePIEEnded delegate
	bool bReady = false;             // PIE world valid + HasBegunPlay seen at least once
	FString ErrorReason;

	// --- Clip variant (capture_pie_movement_clip) ---
	bool bCaptureFrames = false;
	double CaptureInterval = 0.25;
	double LastCaptureSeconds = -1.0; // last sample-relative time a frame was captured
	FString OutputDir;
	int32 CaptureFrameIndex = 0;
	bool bCaptureDeferred = false;    // set if viewport capture is unavailable in this build path
	/** Capture the composited backbuffer (game + UMG/Slate UI) via the engine
	 *  screenshot path — the `shot showui` mechanism — instead of the raw
	 *  scene-only viewport read. Async (file lands at end of frame), so
	 *  uniform-frame validity checks don't apply to these frames. NOTE: with
	 *  PIE docked in the editor the backbuffer is the whole editor window;
	 *  use new-window PIE for a clean game-plus-UI clip. */
	bool bIncludeUi = false;

	// #7 first-frame warm-up policy. The first DiscardFirstFrames captured frames are saved to
	// disk (so the clip is complete) but are NOT counted toward ValidFrames / InvalidFrames —
	// the very first ReadPixels after PIE start can return a uniform / un-warmed buffer that
	// would otherwise be a false invalid. Default 1; 0 disables the warm-up.
	int32 DiscardFirstFrames = 1;

	// #7 capture-validity reporting (clip variant). The view target is resolved/applied at
	// session begin; the per-frame counts are tallied as frames are captured.
	FString ViewTargetActorRequest;   // optional view_target_actor param (name substring)
	FString ViewTargetActorResolved;  // actor we actually SetViewTarget'd (empty = none)
	FString ViewTargetActorClass;     // its class name
	FString ActiveViewTargetName;     // PC->GetViewTarget() name at session begin
	FString ActiveViewTargetClass;
	int32 ValidFrames = 0;            // captured frames that looked like a real render
	int32 InvalidFrames = 0;         // all-black / single-uniform-colour frames

	// #8 staged startup hooks.
	FPieSmokeStages Stages;
	int32 ObserverTickCount = 0;     // increments each AdvanceSession call (AfterNTicks gate)

	// #9 clip runtime-identity snapshot (cached + re-checked across ticks).
	FPieSmokeRuntimeIdentity Identity;

	// Phase 9 (OG-E3): PIE-session-scoped profiling. Capture is bracketed to EXACTLY the PIE
	// window — started on the first ready (post-BeginPlay) observer tick and stopped on EVERY
	// session-end path (success, failure, abort) via StopSessionProfiling(). All fields are
	// game-thread-only like the rest of the session.
	//
	//   bCsvProfile     : caller requested csv_profile=true.
	//   TraceChannels   : caller-supplied trace_channels (empty => no trace requested).
	// The bStarted/bStopped flags single-fire the begin/end so re-ticks never re-arm and the
	// stop is idempotent (so it is safe to call from multiple end paths).
	bool bCsvProfile = false;          // request flag
	bool bCsvStarted = false;          // BeginCapture issued for this session
	bool bCsvAvailable = true;         // false when CSV_PROFILER is compiled out (reported)
	FString CsvPath;                   // resolved output .csv path (filled at stop)
	FString CsvStatus;                 // human-readable status / "unavailable" reason

	TArray<FString> TraceChannels;     // requested trace channels (empty => no trace)
	bool bTraceStarted = false;        // FTraceAuxiliary::Start issued for this session
	FString TracePath;                 // resolved .utrace destination path (filled at start)
	FString TraceStatus;               // human-readable status / failure reason

	bool bProfilingStarted = false;    // single-fire guard for StartSessionProfiling
	bool bProfilingStopped = false;    // single-fire guard for StopSessionProfiling (finally)
};

// Process-lifetime registry of running/finished PIE-smoke sessions plus the single
// shared frame observer. Game-thread-only (MCP calls + the ticker all run there) so
// the maps need no synchronisation. Implemented as a Meyers singleton.
class FPieSmokeSessionManager
{
public:
	static FPieSmokeSessionManager& Get();

	// Create + register a session, install the frame observer + EndPIE delegate if
	// not already present, and return the new session id.
	FString CreateSession(FPieSmokeSession&& Session);

	// Look up a session by id (nullptr if unknown).
	FPieSmokeSession* Find(const FString& SessionId);

	// Force-stop a session (RequestEndPlayMap + mark Stopped). When SessionId is
	// empty, stops every running session. Returns the number stopped.
	int32 Stop(const FString& SessionId);

	// True if any session is still Running.
	bool HasRunningSessions() const;

	// The shared log-capture pointer (post-marker counts read from it).
	void SetLogCapture(FMonolithLogCapture* InCapture) { LogCapture = InCapture; }
	FMonolithLogCapture* GetLogCapture() const { return LogCapture; }

private:
	FPieSmokeSessionManager() = default;

	// FTSTicker callback — fires on the editor's real frame (non-reentrant). Returns
	// true while the observer should keep ticking, false to self-unregister.
	bool OnFrameTick(float DeltaTime);

	// Advance one running session for this frame: resolve pawn, sample anim vars,
	// optionally capture a frame, and finish it when its duration elapses.
	void AdvanceSession(FPieSmokeSession& Session);

	// Bind / unbind the editor PIE-end delegates (marks sessions inactive so the
	// observer stops dereferencing a torn-down PIE world).
	void EnsureObserver();
	void TeardownObserverIfIdle();

	void OnPieEnded(const bool bIsSimulating);

	TMap<FString, FPieSmokeSession> Sessions;
	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle EndPieHandle;
	FDelegateHandle PrePieEndedHandle;
	bool bObserverActive = false;

	FMonolithLogCapture* LogCapture = nullptr;
	uint32 NextSessionSerial = 1;
};
