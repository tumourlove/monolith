#include "MonolithPieSmokeSession.h"
#include "MonolithEditorActions.h" // FMonolithLogCapture / FMonolithLogEntry
#include "MonolithPieObjectActions.h" // Gap 9: shared PIE-object resolver + dotted read

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h" // Gap 9: ACharacter::Jump provocation
#include "GameFramework/PlayerController.h"
#include "GameFramework/Controller.h"
#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "Dom/JsonValue.h" // Gap 9: timeseries sample JSON values
#include "EngineUtils.h"

// Phase 8 (OG-E2/E5): structured actor_setup execution against the live PIE world.
#include "AIController.h"            // AAIController::MoveToLocation (AIModule dep)
#include "Navigation/PathFollowingComponent.h" // EPathFollowingRequestResult tokens
#include "UObject/UObjectGlobals.h"  // LoadObject / LoadClass
#include "UObject/Class.h"           // FProperty iteration / SameType / CopyCompleteValue

#include "UnrealClient.h"        // FViewport (PIE frame capture)
#include "RenderingThread.h"     // FlushRenderingCommands (render-flush before first ReadPixels)
#include "ImageUtils.h"          // FImageUtils::SaveImageAutoFormat
#include "ImageCore.h"           // FImage / ERawImageFormat / EGammaSpace
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "UObject/UnrealType.h"

#include "IPythonScriptPlugin.h" // #4 delayed in-session python probes
#include "PythonScriptTypes.h"   // FPythonCommandEx

// Phase 9 (OG-E3): PIE-session-scoped profiling (CSV profiler + Unreal Insights trace).
// Both live in Core; no new module dependency is required (Core is already a public dep of
// MonolithEditor). FCsvProfiler is gated behind CSV_PROFILER (WITH_ENGINE && !UE_BUILD_SHIPPING
// by default) — when that macro is off the begin/end-capture path compiles out cleanly and the
// session reports "profiling unavailable in this build config" instead of failing to build.
#include "ProfilingDebugging/CsvProfiler.h"   // FCsvProfiler::Get()->BeginCapture / EndCapture
#include "ProfilingDebugging/TraceAuxiliary.h" // FTraceAuxiliary::Start / Stop

DEFINE_LOG_CATEGORY_STATIC(LogMonolithPieSmoke, Log, All);

namespace
{
	// Locate the active PIE world context's UWorld (or nullptr). Self-contained copy
	// of the actions-module helper so the manager carries no cross-TU dependency.
	UWorld* FindActivePieWorld()
	{
		if (!GEditor)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEditor->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World())
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	// Read the pawn's AnimInstance (via its SkeletalMeshComponent) or nullptr.
	UAnimInstance* PawnAnimInstance(APawn* Pawn)
	{
		if (!Pawn)
		{
			return nullptr;
		}
		if (USkeletalMeshComponent* SkelComp = Pawn->FindComponentByClass<USkeletalMeshComponent>())
		{
			return SkelComp->GetAnimInstance();
		}
		return nullptr;
	}

	// True if the pawn's AnimInstance exposes at least one of the requested variables.
	bool PawnHasAnyVar(APawn* Pawn, const TArray<FString>& VarNames)
	{
		if (VarNames.Num() == 0)
		{
			return false;
		}
		UAnimInstance* Anim = PawnAnimInstance(Pawn);
		if (!Anim || !Anim->GetClass())
		{
			return false;
		}
		for (const FString& VarName : VarNames)
		{
			if (Anim->GetClass()->FindPropertyByName(FName(*VarName)))
			{
				return true;
			}
		}
		return false;
	}

	// Resolve the pawn the session samples, in priority order:
	//   (a) class-name-filtered pawn, when a filter is set;
	//   (b) else, a pawn whose AnimInstance actually exposes one of the requested vars
	//       (so the default player pawn's ABP not carrying them is skipped);
	//   (c) else, the first player controller's pawn.
	APawn* ResolveTargetPawn(UWorld* PieWorld, const FString& ClassFilter,
		const TArray<FString>& VarNames)
	{
		if (!PieWorld)
		{
			return nullptr;
		}
		if (!ClassFilter.IsEmpty())
		{
			for (TActorIterator<APawn> It(PieWorld); It; ++It)
			{
				APawn* Candidate = *It;
				if (Candidate && Candidate->GetClass() &&
					Candidate->GetClass()->GetName().Contains(ClassFilter))
				{
					return Candidate;
				}
			}
			return nullptr;
		}
		if (VarNames.Num() > 0)
		{
			for (TActorIterator<APawn> It(PieWorld); It; ++It)
			{
				APawn* Candidate = *It;
				if (PawnHasAnyVar(Candidate, VarNames))
				{
					return Candidate;
				}
			}
		}
		if (APlayerController* PC = PieWorld->GetFirstPlayerController())
		{
			return PC->GetPawn();
		}
		return nullptr;
	}

	// Read one named float/double/bool AnimInstance variable reflectively. Returns false
	// if the property is absent / unsupported (different ABPs expose different vars).
	bool ReadAnimVar(UAnimInstance* Anim, const FString& VarName, FPieSmokeSampleVar& OutVar)
	{
		if (!Anim)
		{
			return false;
		}
		FProperty* Prop = Anim->GetClass()->FindPropertyByName(FName(*VarName));
		if (!Prop)
		{
			return false;
		}
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Anim);
		OutVar.Name = VarName;
		if (const FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{
			OutVar.bIsBool = false;
			OutVar.NumberValue = FloatProp->GetPropertyValue(ValuePtr);
			return true;
		}
		if (const FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{
			OutVar.bIsBool = false;
			OutVar.NumberValue = DoubleProp->GetPropertyValue(ValuePtr);
			return true;
		}
		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			OutVar.bIsBool = true;
			OutVar.BoolValue = BoolProp->GetPropertyValue(ValuePtr);
			return true;
		}
		return false;
	}

	// #7 outcome of one viewport capture attempt: whether bytes were written, and a
	// validity verdict derived from the pixel buffer (uniform/all-black => unrendered).
	struct FCaptureResult
	{
		bool bSaved = false;     // PNG written to disk
		bool bUniform = false;   // every pixel is the same colour (incl. all-black)
		bool bValid = false;     // bSaved && !bUniform — looks like a real render
	};

	// #7 scan a captured pixel buffer for the unrendered signature. GetPIEViewport
	// resolves to the active level viewport; with a hidden editor / no active level
	// viewport it is never rendered into, so ReadPixels returns uninitialised (usually
	// all-black, sometimes a single clear colour) pixels. A frame where every pixel
	// shares one RGB value is flagged uniform => invalid. Sampled (stride) for speed.
	bool PixelsAreUniform(const TArray<FColor>& Pixels)
	{
		if (Pixels.Num() == 0)
		{
			return true; // no pixels => certainly not a real render
		}
		const FColor First = Pixels[0];
		const int32 Stride = FMath::Max(1, Pixels.Num() / 4096); // sample up to ~4k pixels
		for (int32 i = 0; i < Pixels.Num(); i += Stride)
		{
			const FColor& Px = Pixels[i];
			if (Px.R != First.R || Px.G != First.G || Px.B != First.B)
			{
				return false; // colour variation => a real render
			}
		}
		return true;
	}

	// Read the active PIE viewport into a PNG. Returns an FCaptureResult (no crash) —
	// bSaved is false when the viewport / pixels are unavailable. #7 also reports whether
	// the captured frame is uniform (all-black / single colour => unrendered).
	FCaptureResult CapturePieFrame(const FString& OutputPath, bool bFlushBeforeRead)
	{
		FCaptureResult Out;

		FViewport* Viewport = GEditor ? GEditor->GetPIEViewport() : nullptr;
		if (!Viewport)
		{
			return Out;
		}
		const FIntPoint Size = Viewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			return Out;
		}

		// #7 drain the render thread before the first read so ReadPixels sees a fully-rendered
		// back-buffer rather than an un-warmed / uniform one. FlushRenderingCommands() is a
		// no-op when the RHI isn't initialised, so it is safe to call here.
		if (bFlushBeforeRead)
		{
			FlushRenderingCommands();
		}

		TArray<FColor> Pixels;
		if (!Viewport->ReadPixels(Pixels) || Pixels.Num() < Size.X * Size.Y)
		{
			return Out;
		}

		// #7 validity verdict computed BEFORE the alpha overwrite (alpha is forced opaque
		// for the PNG below, which would otherwise mask a genuinely-uniform buffer).
		Out.bUniform = PixelsAreUniform(Pixels);

		for (FColor& Px : Pixels)
		{
			Px.A = 255; // viewport reads can carry scene-depth alpha noise
		}

		const FString Dir = FPaths::GetPath(OutputPath);
		IFileManager::Get().MakeDirectory(*Dir, true);

		FImage Image;
		Image.Init(Size.X, Size.Y, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), Size.X * Size.Y * sizeof(FColor));
		Out.bSaved = FImageUtils::SaveImageAutoFormat(*OutputPath, Image);
		Out.bValid = Out.bSaved && !Out.bUniform;
		return Out;
	}

	// #8 run a python/console script payload against the LIVE PIE world. Shared core of
	// both FireProbe (#4) and FireStage (#8) — same call shapes used everywhere
	// (PC->ConsoleCommand for console, IPythonScriptPlugin::ExecPythonCommandEx for python).
	void RunScriptPayload(const FString& Python, const TArray<FString>& Console,
		UWorld* PieWorld, bool& OutPythonOk, FString& OutPythonOutput)
	{
		if (Console.Num() > 0 && PieWorld)
		{
			APlayerController* PC = PieWorld->GetFirstPlayerController();
			for (const FString& Command : Console)
			{
				if (Command.IsEmpty()) { continue; }
				if (PC) { PC->ConsoleCommand(Command, /*bWriteToLog=*/true); }
				else if (GEngine) { GEngine->Exec(PieWorld, *Command); }
			}
		}

		if (!Python.IsEmpty())
		{
			if (IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get())
			{
				if (PythonPlugin->IsPythonAvailable())
				{
					FPythonCommandEx Cmd;
					Cmd.Command = Python;
					Cmd.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
					OutPythonOk = PythonPlugin->ExecPythonCommandEx(Cmd);
					OutPythonOutput = Cmd.CommandResult;
				}
				else
				{
					OutPythonOutput = TEXT("Python not available in this build.");
				}
			}
			else
			{
				OutPythonOutput = TEXT("IPythonScriptPlugin unavailable.");
			}
		}
	}

	// #8 fire one staged hook exactly once and stamp its outcome.
	void FireStage(FPieSmokeStage& Stage, UWorld* PieWorld, double ElapsedSeconds)
	{
		Stage.bFired = true;
		Stage.FiredAtSeconds = ElapsedSeconds;
		RunScriptPayload(Stage.Python, Stage.Console, PieWorld, Stage.bPythonOk, Stage.PythonOutput);
	}

	// #4 fire one delayed probe against the LIVE PIE world. Uses the SAME mechanism the
	// run_pie_smoke RunScripts start-time path uses (PC->ConsoleCommand for console,
	// IPythonScriptPlugin::ExecPythonCommandEx for python). Records outcome on the probe.
	void FireProbe(FPieSmokeProbe& Probe, UWorld* PieWorld, double ElapsedSeconds)
	{
		Probe.bFired = true;
		Probe.FiredAtSeconds = ElapsedSeconds;
		RunScriptPayload(Probe.Python, Probe.Console, PieWorld, Probe.bPythonOk, Probe.PythonOutput);
	}

	// ── Gap 9: time-series target resolution + typed provocation dispatch ──

	// Resolve the timeseries target object in the live PIE world from the session's stored
	// selector fields. Mirrors MonolithPieObject::Resolve's matching (actor_label/object_name
	// exact, class_name substring; optional component hop or anim-instance hop) but reads the
	// selector off the session struct rather than a JSON params blob.
	UObject* ResolveTimeseriesTarget(UWorld* PieWorld, const FPieSmokeSession& Session)
	{
		if (!PieWorld)
		{
			return nullptr;
		}

		AActor* Actor = nullptr;
		for (TActorIterator<AActor> It(PieWorld); It; ++It)
		{
			AActor* Candidate = *It;
			if (!Candidate) { continue; }
#if WITH_EDITOR
			const FString Label = Candidate->GetActorLabel();
#else
			const FString Label;
#endif
			const FString Name = Candidate->GetName();
			const FString Cls = Candidate->GetClass() ? Candidate->GetClass()->GetName() : FString();

			if (!Session.TargetActorLabel.IsEmpty() && Label == Session.TargetActorLabel) { Actor = Candidate; break; }
			if (!Session.TargetObjectName.IsEmpty() && Name == Session.TargetObjectName)  { Actor = Candidate; break; }
			if (!Session.TargetClassName.IsEmpty() && Cls.Contains(Session.TargetClassName)) { Actor = Candidate; break; }
		}
		if (!Actor)
		{
			return nullptr;
		}

		if (Session.bTargetAnimInstance)
		{
			for (UActorComponent* C : Actor->GetComponents())
			{
				USkeletalMeshComponent* MC = Cast<USkeletalMeshComponent>(C);
				if (!MC) { continue; }
				if (Session.TargetComponentName.IsEmpty() || MC->GetName() == Session.TargetComponentName)
				{
					return MC->GetAnimInstance();
				}
			}
			return nullptr;
		}
		if (!Session.TargetComponentName.IsEmpty())
		{
			for (UActorComponent* C : Actor->GetComponents())
			{
				if (C && C->GetName() == Session.TargetComponentName) { return C; }
			}
			return nullptr;
		}
		return Actor;
	}

	// Resolve the controlled pawn for movement/jump provocations: the timeseries target
	// when it IS a pawn, else the first player controller's pawn. Returns nullptr if none.
	APawn* ResolveProvocationPawn(UWorld* PieWorld, UObject* Target)
	{
		if (APawn* DirectPawn = Cast<APawn>(Target))
		{
			return DirectPawn;
		}
		if (AActor* TargetActor = Cast<AActor>(Target))
		{
			if (APawn* AsPawn = Cast<APawn>(TargetActor)) { return AsPawn; }
		}
		if (PieWorld)
		{
			if (APlayerController* PC = PieWorld->GetFirstPlayerController())
			{
				return PC->GetPawn();
			}
		}
		return nullptr;
	}

	// Fire one typed provocation against the live PIE world. Each provocation fires once;
	// the outcome (dispatched / why-not) is stamped for the report. Never crashes: every
	// target lookup is null-checked and reported rather than dereferenced blindly.
	void FireProvocation(FPieProvocation& Prov, UWorld* PieWorld, UObject* Target, double ElapsedSeconds)
	{
		Prov.bFired = true;
		Prov.FiredAtSeconds = ElapsedSeconds;

		switch (Prov.Action)
		{
		case EPieProvocationAction::SetControlRotation:
		{
			APlayerController* PC = PieWorld ? PieWorld->GetFirstPlayerController() : nullptr;
			if (!PC)
			{
				Prov.Result = TEXT("no_player_controller");
				return;
			}
			PC->SetControlRotation(Prov.Rotation);
			Prov.bDispatched = true;
			Prov.Result = TEXT("set_control_rotation");
			return;
		}
		case EPieProvocationAction::AddMovementInput:
		{
			APawn* Pawn = ResolveProvocationPawn(PieWorld, Target);
			if (!Pawn)
			{
				Prov.Result = TEXT("no_pawn");
				return;
			}
			Pawn->AddMovementInput(Prov.Direction, static_cast<float>(Prov.Scale));
			Prov.bDispatched = true;
			Prov.Result = TEXT("add_movement_input");
			return;
		}
		case EPieProvocationAction::Jump:
		{
			APawn* Pawn = ResolveProvocationPawn(PieWorld, Target);
			ACharacter* Character = Cast<ACharacter>(Pawn);
			if (!Character)
			{
				Prov.Result = Pawn ? TEXT("pawn_not_a_character") : TEXT("no_pawn");
				return;
			}
			Character->Jump();
			Prov.bDispatched = true;
			Prov.Result = TEXT("jump");
			return;
		}
		case EPieProvocationAction::ConsoleCommand:
		{
			if (Prov.Command.IsEmpty())
			{
				Prov.Result = TEXT("empty_command");
				return;
			}
			if (APlayerController* PC = PieWorld ? PieWorld->GetFirstPlayerController() : nullptr)
			{
				PC->ConsoleCommand(Prov.Command, /*bWriteToLog=*/true);
				Prov.bDispatched = true;
			}
			else if (GEngine && PieWorld)
			{
				GEngine->Exec(PieWorld, *Prov.Command);
				Prov.bDispatched = true;
			}
			Prov.Result = Prov.bDispatched ? TEXT("console_command") : TEXT("no_exec_target");
			return;
		}
		default:
			Prov.Result = TEXT("unknown_action");
			return;
		}
	}

	// Phase 8: human-readable token for an AI move request result.
	const TCHAR* MoveRequestToken(EPathFollowingRequestResult::Type Result)
	{
		switch (Result)
		{
		case EPathFollowingRequestResult::Failed:           return TEXT("failed");
		case EPathFollowingRequestResult::AlreadyAtGoal:    return TEXT("already_at_goal");
		case EPathFollowingRequestResult::RequestSuccessful: return TEXT("request_successful");
		default:                                            return TEXT("unknown");
		}
	}

	// Phase 8: copy the DataAsset's reflected fields onto matching-named actor properties.
	// The DA-field -> actor-prop mapping is NOT 1:1: a field is applied only when the actor
	// also declares a property of the SAME NAME and a COMPATIBLE TYPE (FProperty::SameType).
	// Every DA field is bucketed into AppliedFields or UnmatchedFields so the caller gets a
	// STRUCTURED partial-vs-full apply verdict — never a bare log line. Read-only on the DA;
	// writes only into the transient PIE actor (no asset/package dirtying).
	void ApplyDataAssetFields(UObject* DataAsset, AActor* Actor, FPieSmokeSpawnedActorResult& OutResult)
	{
		if (!DataAsset || !Actor)
		{
			return;
		}
		UClass* ActorClass = Actor->GetClass();
		for (TFieldIterator<FProperty> It(DataAsset->GetClass()); It; ++It)
		{
			FProperty* DataProp = *It;
			if (!DataProp)
			{
				continue;
			}
			const FString FieldName = DataProp->GetName();

			// Skip the UObject base bookkeeping props the indexer also skips — they are never
			// meaningful to copy onto a gameplay actor and would only pollute the report.
			if (DataProp->HasAnyPropertyFlags(CPF_Transient))
			{
				continue;
			}

			FProperty* ActorProp = ActorClass->FindPropertyByName(FName(*FieldName));
			if (!ActorProp || !ActorProp->SameType(DataProp))
			{
				OutResult.UnmatchedFields.Add(FieldName);
				continue;
			}

			const void* SrcPtr = DataProp->ContainerPtrToValuePtr<void>(DataAsset);
			void* DestPtr = ActorProp->ContainerPtrToValuePtr<void>(Actor);
			if (SrcPtr && DestPtr)
			{
				ActorProp->CopyCompleteValue(DestPtr, SrcPtr);
				OutResult.AppliedFields.Add(FieldName);
			}
			else
			{
				OutResult.UnmatchedFields.Add(FieldName);
			}
		}
	}

	// Phase 8: execute the declarative actor_setup spec ONCE against the live PIE world.
	// For each entry: resolve the class, load the optional DataAsset, spawn `count` actors,
	// reflectively apply DA fields, and (when move_to is set) issue AAIController::MoveToLocation
	// via the spawned pawn's controller. Every step null-checks + reports rather than crashes;
	// spawn failures are captured, not fatal. Results land in Session.ActorSetupResults.
	void FireActorSetup(FPieSmokeSession& Session, UWorld* PieWorld)
	{
		// Guard against ever spawning into the editor (non-PIE) world.
		if (!PieWorld || PieWorld->WorldType != EWorldType::PIE)
		{
			return;
		}

		for (const FPieSmokeActorSetupEntry& Entry : Session.ActorSetups)
		{
			FPieSmokeActorSetupResult Result;
			Result.ClassPath = Entry.ClassPath;
			Result.ApplyDataAssetPath = Entry.ApplyDataAssetPath;
			Result.RequestedCount = FMath::Max(1, Entry.Count);

			// Resolve the actor class. Mirror the proven nav-harness resolution: LoadClass
			// for a generated-class path, LoadObject as the _C-suffix-omission fallback.
			UClass* ActorClass = LoadClass<AActor>(nullptr, *Entry.ClassPath);
			if (!ActorClass)
			{
				ActorClass = LoadObject<UClass>(nullptr, *Entry.ClassPath);
			}
			Result.bClassResolved = (ActorClass != nullptr);

			// Optionally load the DataAsset whose fields we copy onto each spawned actor.
			UObject* DataAsset = nullptr;
			if (!Entry.ApplyDataAssetPath.IsEmpty())
			{
				DataAsset = LoadObject<UObject>(nullptr, *Entry.ApplyDataAssetPath);
				Result.bDataAssetLoaded = (DataAsset != nullptr);
				if (!DataAsset)
				{
					Result.DataAssetError = TEXT("could not load apply_data_asset path");
				}
			}

			if (!ActorClass)
			{
				// No class -> nothing to spawn; still emit the (failed) entry result.
				Session.ActorSetupResults.Add(MoveTemp(Result));
				continue;
			}

			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

			const int32 SpawnCount = Result.RequestedCount;
			for (int32 i = 0; i < SpawnCount; ++i)
			{
				FPieSmokeSpawnedActorResult ActorResult;

				const FVector Loc = Entry.Locations.IsValidIndex(i)
					? Entry.Locations[i] : FVector::ZeroVector;

				AActor* Spawned = PieWorld->SpawnActor<AActor>(
					ActorClass, Loc, FRotator::ZeroRotator, SpawnParams);
				if (!Spawned)
				{
					ActorResult.bSpawned = false;
					ActorResult.SpawnError = TEXT("SpawnActor returned null");
					Result.Actors.Add(MoveTemp(ActorResult));
					continue;
				}

				ActorResult.bSpawned = true;
				ActorResult.ActorName = Spawned->GetName();
				ActorResult.RuntimeClassPath = Spawned->GetClass()
					? Spawned->GetClass()->GetPathName() : FString();
				Spawned->SetFolderPath(FName(TEXT("Monolith/ActorSetup")));
				++Result.SpawnedCount;

				// Reflective DataAsset-field -> actor-prop apply (structured applied/unmatched).
				if (DataAsset)
				{
					ApplyDataAssetFields(DataAsset, Spawned, ActorResult);
				}

				// AI move: only meaningful for a pawn with (or able to spawn) a controller.
				if (Entry.bHasMoveTo)
				{
					ActorResult.bMoveRequested = true;
					APawn* Pawn = Cast<APawn>(Spawned);
					if (!Pawn)
					{
						ActorResult.MoveResult = TEXT("not_a_pawn");
					}
					else
					{
						AController* Controller = Pawn->GetController();
						if (!Controller)
						{
							// Pawn may not have auto-possessed yet — request a default controller.
							Pawn->SpawnDefaultController();
							Controller = Pawn->GetController();
						}
						AAIController* AICon = Cast<AAIController>(Controller);
						if (!AICon)
						{
							ActorResult.MoveResult = Controller
								? TEXT("controller_not_ai") : TEXT("no_controller");
						}
						else
						{
							// Pass AcceptanceRadius explicitly (-1 = engine default, mirrors
							// MoveToActor's documented default); remaining args use their header
							// defaults. Verified AIController.cpp:591 / AIController.h:171 (5.7).
							const EPathFollowingRequestResult::Type MoveRes =
								AICon->MoveToLocation(Entry.MoveTo, -1.0f);
							ActorResult.bMoveIssued = true;
							ActorResult.MoveResult = MoveRequestToken(MoveRes);
						}
					}
				}

				Result.Actors.Add(MoveTemp(ActorResult));
			}

			Session.ActorSetupResults.Add(MoveTemp(Result));
		}
	}

	// #9 derive a human-readable animation-mode token from the live mode enum.
	const TCHAR* AnimationModeToken(EAnimationMode::Type Mode)
	{
		switch (Mode)
		{
		case EAnimationMode::AnimationBlueprint:  return TEXT("anim_blueprint");
		case EAnimationMode::AnimationSingleNode: return TEXT("single_node");
		case EAnimationMode::AnimationCustomMode: return TEXT("custom");
		default:                                  return TEXT("none");
		}
	}

	// #9 capture / refresh the runtime-identity snapshot from the live pawn each sampled
	// tick. First resolve caches actor/skel/anim-class; subsequent ticks re-check the
	// AnimClass to flag anim_class_changed, and (when set) the expected-class assert.
	void UpdateRuntimeIdentity(FPieSmokeRuntimeIdentity& Id, APawn* Pawn)
	{
		if (!Pawn)
		{
			return;
		}
		USkeletalMeshComponent* SkelComp = Pawn->FindComponentByClass<USkeletalMeshComponent>();
		if (!SkelComp)
		{
			return;
		}

		const FString MeshAnimClassPath = SkelComp->GetAnimClass()
			? SkelComp->GetAnimClass()->GetPathName() : FString();

		if (!Id.bResolved)
		{
			Id.bResolved = true;
			Id.ActorName = Pawn->GetName();
			Id.ActorClass = Pawn->GetClass() ? Pawn->GetClass()->GetName() : FString();
			Id.SkelCompName = SkelComp->GetName();
			if (UAnimInstance* Anim = SkelComp->GetAnimInstance())
			{
				Id.AnimInstanceClassPath = Anim->GetClass() ? Anim->GetClass()->GetPathName() : FString();
			}
			Id.MeshAnimClassPath = MeshAnimClassPath;
			Id.AnimationMode = AnimationModeToken(SkelComp->GetAnimationMode());
		}
		else
		{
			// Re-check AnimClass each tick — a class swap (e.g. linked-anim layer change)
			// flips anim_class_changed and refreshes the reported path/mode.
			if (!MeshAnimClassPath.Equals(Id.MeshAnimClassPath))
			{
				Id.bAnimClassChanged = true;
				Id.MeshAnimClassPath = MeshAnimClassPath;
				if (UAnimInstance* Anim = SkelComp->GetAnimInstance())
				{
					Id.AnimInstanceClassPath = Anim->GetClass() ? Anim->GetClass()->GetPathName() : FString();
				}
				Id.AnimationMode = AnimationModeToken(SkelComp->GetAnimationMode());
			}
		}

		// #9 expected-class assert (substring match, never crashes).
		if (!Id.ExpectedAnimClass.IsEmpty())
		{
			Id.bExpectedChecked = true;
			Id.bExpectedMismatch = !Id.MeshAnimClassPath.Contains(Id.ExpectedAnimClass);
		}
	}

	// Phase 9 (OG-E3): the directory profiling artifacts (CSV + trace) are written into.
	// <project>/Saved/Profiling — the engine's conventional profiling output dir, created
	// on demand so a first-ever run does not silently no-op.
	FString ProfilingOutputDir()
	{
		const FString Dir = FPaths::ProjectDir() / TEXT("Saved/Profiling");
		IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
		return Dir;
	}

	// Phase 9 (OG-E3): start the requested profiling capture(s) for a session. Called ONCE,
	// on the first ready (post-BeginPlay) observer tick, so capture brackets the PIE window
	// and never includes editor-idle frames before BeginPlay. Single-fire guarded by the
	// caller (Session.bProfilingStarted). Never throws / never blocks the frame.
	void StartSessionProfiling(FPieSmokeSession& Session)
	{
		// --- CSV profiler ---
		if (Session.bCsvProfile)
		{
#if CSV_PROFILER
			if (FCsvProfiler* Profiler = FCsvProfiler::Get())
			{
				if (Profiler->IsCapturing())
				{
					// Something else already owns the capture — do NOT hijack it (and do NOT
					// stop it on our teardown). Report rather than fight for ownership.
					Session.CsvStatus = TEXT("a CSV capture was already running; not started by this session");
				}
				else
				{
					const FString Dir = ProfilingOutputDir();
					// A custom Filename is used verbatim by the profiler (no auto extension), so
					// supply the .csv suffix ourselves. We pre-compute the expected path here; the
					// authoritative path is re-read from GetOutputFilename() at stop time.
					const FString FileName = FString::Printf(TEXT("pie_%s.csv"), *Session.Id);
					// NumFramesToCapture = -1 => capture until EndCapture() (i.e. session end).
					Profiler->BeginCapture(/*InNumFramesToCapture=*/-1, Dir, FileName);
					Session.bCsvStarted = true;
					Session.CsvPath = Dir / FileName; // expected path; refined at stop
					Session.CsvStatus = TEXT("capturing");
				}
			}
			else
			{
				Session.CsvStatus = TEXT("FCsvProfiler::Get() returned null");
			}
#else
			Session.bCsvAvailable = false;
			Session.CsvStatus = TEXT("profiling unavailable in this build config (CSV_PROFILER off)");
#endif // CSV_PROFILER
		}

		// --- Unreal Insights trace ---
		if (Session.TraceChannels.Num() > 0)
		{
			if (FTraceAuxiliary::IsConnected())
			{
				// A trace is already active (e.g. started from the command line / Insights).
				// Do not start a second one and do not stop the existing one on teardown.
				Session.TraceStatus = TEXT("a trace was already connected; not started by this session");
			}
			else
			{
				const FString ChannelString = FString::Join(Session.TraceChannels, TEXT(","));
				const FString TraceFile = ProfilingOutputDir() /
					FString::Printf(TEXT("pie_%s.utrace"), *Session.Id);
				const bool bStarted = FTraceAuxiliary::Start(
					FTraceAuxiliary::EConnectionType::File, *TraceFile, *ChannelString);
				if (bStarted)
				{
					Session.bTraceStarted = true;
					// GetTraceDestinationString() is authoritative for where data actually lands.
					const FString Dest = FTraceAuxiliary::GetTraceDestinationString();
					Session.TracePath = Dest.IsEmpty() ? TraceFile : Dest;
					Session.TraceStatus = TEXT("tracing");
				}
				else
				{
					Session.TraceStatus = TEXT("FTraceAuxiliary::Start failed (trace may be disabled in this build)");
				}
			}
		}
	}

	// Phase 9 (OG-E3): the finally-equivalent. Stop/flush any profiling THIS session started,
	// on EVERY session-end path (success, failure, abort). Idempotent: a single-fire guard
	// (Session.bProfilingStopped) means it is safe to call from multiple end paths, and it only
	// ever stops captures THIS session itself started (bCsvStarted / bTraceStarted) so a
	// pre-existing external capture is never collateral-stopped. Never throws / never blocks.
	void StopSessionProfiling(FPieSmokeSession& Session)
	{
		if (Session.bProfilingStopped)
		{
			return;
		}
		Session.bProfilingStopped = true;

#if CSV_PROFILER
		if (Session.bCsvStarted)
		{
			if (FCsvProfiler* Profiler = FCsvProfiler::Get())
			{
				// EndCapture returns a future resolving to the written filename once the async
				// file write completes. We do NOT block the editor frame waiting on it; the
				// in-progress output path is read from GetOutputFilename() for the report.
				Profiler->EndCapture();
				const FString OutFile = Profiler->GetOutputFilename();
				if (!OutFile.IsEmpty())
				{
					Session.CsvPath = OutFile;
				}
				Session.CsvStatus = TEXT("stopped (csv flushing asynchronously)");
			}
			Session.bCsvStarted = false;
		}
#else
		// CSV compiled out — nothing to stop; status already reflects unavailability.
#endif // CSV_PROFILER

		if (Session.bTraceStarted)
		{
			// Only stop if we are still the connected trace (defensive — another stop may have
			// raced in on a crash path). Stop() is a no-op when there is no data connection.
			FTraceAuxiliary::Stop();
			Session.bTraceStarted = false;
			Session.TraceStatus = TEXT("stopped");
		}
	}
}

FPieSmokeSessionManager& FPieSmokeSessionManager::Get()
{
	static FPieSmokeSessionManager Instance;
	return Instance;
}

FString FPieSmokeSessionManager::CreateSession(FPieSmokeSession&& Session)
{
	if (Session.Id.IsEmpty())
	{
		Session.Id = FString::Printf(TEXT("pie_smoke_%u_%s"),
			NextSessionSerial++,
			*FDateTime::Now().ToString(TEXT("%H%M%S")));
	}
	const FString Id = Session.Id;
	Sessions.Add(Id, MoveTemp(Session));
	EnsureObserver();
	return Id;
}

FPieSmokeSession* FPieSmokeSessionManager::Find(const FString& SessionId)
{
	return Sessions.Find(SessionId);
}

int32 FPieSmokeSessionManager::Stop(const FString& SessionId)
{
	int32 Stopped = 0;
	bool bAnyRunning = false;

	for (TPair<FString, FPieSmokeSession>& Pair : Sessions)
	{
		FPieSmokeSession& S = Pair.Value;
		if (SessionId.IsEmpty() || Pair.Key == SessionId)
		{
			if (S.Status == EPieSmokeStatus::Running)
			{
				S.Status = EPieSmokeStatus::Stopped;
				S.bStoppedByTool = true; // #11 lifecycle => stopped-by-tool
				S.LastObservedSeconds = FPlatformTime::Seconds();
				StopSessionProfiling(S); // Phase 9: finally — flush on the tool-driven stop path.
				++Stopped;
			}
		}
	}

	// End PIE if no session still wants it running. We only drive RequestEndPlayMap
	// when nothing Running remains, so concurrent sessions (rare) aren't cut short.
	for (const TPair<FString, FPieSmokeSession>& Pair : Sessions)
	{
		if (Pair.Value.Status == EPieSmokeStatus::Running)
		{
			bAnyRunning = true;
			break;
		}
	}
	if (!bAnyRunning && GEditor && FindActivePieWorld())
	{
		GEditor->RequestEndPlayMap();
		// #11 mark every session whose PIE we just asked to end as teardown-started.
		for (TPair<FString, FPieSmokeSession>& Pair : Sessions)
		{
			Pair.Value.bTeardownStarted = true;
		}
	}
	return Stopped;
}

bool FPieSmokeSessionManager::HasRunningSessions() const
{
	for (const TPair<FString, FPieSmokeSession>& Pair : Sessions)
	{
		if (Pair.Value.Status == EPieSmokeStatus::Running)
		{
			return true;
		}
	}
	return false;
}

void FPieSmokeSessionManager::EnsureObserver()
{
	if (bObserverActive)
	{
		return;
	}
	bObserverActive = true;

	// Single shared frame observer. Runs as part of the editor's REAL frame (after the
	// frame's BeginFrame/dynamic-resolution bracket is already balanced), so it is NOT
	// re-entrant and may READ world/PIE/actor state freely. It MUST NOT call
	// World->Tick / GEditor->Tick / ProcessAsyncLoading — the engine advances those.
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		TEXT("MonolithPieSmokeObserver"), 0.0f,
		[](float DeltaTime) -> bool
		{
			return FPieSmokeSessionManager::Get().OnFrameTick(DeltaTime);
		});

	// Mark sessions inactive the instant PIE ends (crash / manual stop / completion) so
	// the observer never dereferences a torn-down PIE world.
	EndPieHandle = FEditorDelegates::EndPIE.AddRaw(this, &FPieSmokeSessionManager::OnPieEnded);
	PrePieEndedHandle = FEditorDelegates::PrePIEEnded.AddRaw(this, &FPieSmokeSessionManager::OnPieEnded);
}

void FPieSmokeSessionManager::TeardownObserverIfIdle()
{
	if (HasRunningSessions())
	{
		return; // leave the observer installed while any session still runs
	}
	if (!bObserverActive)
	{
		return;
	}
	bObserverActive = false;

	if (TickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	if (EndPieHandle.IsValid())
	{
		FEditorDelegates::EndPIE.Remove(EndPieHandle);
		EndPieHandle.Reset();
	}
	if (PrePieEndedHandle.IsValid())
	{
		FEditorDelegates::PrePIEEnded.Remove(PrePieEndedHandle);
		PrePieEndedHandle.Reset();
	}
}

void FPieSmokeSessionManager::OnPieEnded(const bool /*bIsSimulating*/)
{
	// PIE is going away — every session loses its world. Mark inactive so the next
	// observer tick stops sampling. A Running session whose PIE ended before its
	// duration elapsed is finalised as Complete on the next tick (bPieActive == false).
	for (TPair<FString, FPieSmokeSession>& Pair : Sessions)
	{
		Pair.Value.bPieActive = false;
		// Phase 9 (OG-E3): finally — the PIE world is going away NOW (incl. on a crash/abort
		// that fires EndPIE). Flush profiling here too rather than relying on a later observer
		// tick, so a capture can never outlive the PIE it was bracketing. Idempotent.
		StopSessionProfiling(Pair.Value);
	}
}

bool FPieSmokeSessionManager::OnFrameTick(float /*DeltaTime*/)
{
	UWorld* PieWorld = FindActivePieWorld();

	for (TPair<FString, FPieSmokeSession>& Pair : Sessions)
	{
		FPieSmokeSession& S = Pair.Value;
		if (S.Status != EPieSmokeStatus::Running)
		{
			continue;
		}

		const double Now = FPlatformTime::Seconds();
		S.LastObservedSeconds = Now;
		const double Elapsed = Now - S.StartTimeSeconds;

		// PIE gone (ended / crashed / manually stopped) before duration elapsed.
		if (!S.bPieActive || !PieWorld || !IsValid(PieWorld))
		{
			S.Status = EPieSmokeStatus::Complete;
			StopSessionProfiling(S); // Phase 9: finally — flush on the PIE-gone/abort path.
			continue;
		}

		// World up but not yet begun play — wait for the next frame.
		if (!PieWorld->HasBegunPlay())
		{
			continue;
		}
		S.bReady = true;

		AdvanceSession(S);

		if (Elapsed >= S.DurationSeconds)
		{
			S.Status = EPieSmokeStatus::Complete;
			StopSessionProfiling(S); // Phase 9: finally — flush on the normal-completion path.
		}
	}

	// Stop PIE once no session needs it (and the world is still up).
	if (!HasRunningSessions())
	{
		if (GEditor && FindActivePieWorld())
		{
			GEditor->RequestEndPlayMap();
			// #11 mark every session as teardown-started so lifecycle reflects the
			// requested (still-deferred) EndPlayMap.
			for (TPair<FString, FPieSmokeSession>& Pair : Sessions)
			{
				Pair.Value.bTeardownStarted = true;
			}
		}
		TeardownObserverIfIdle();
		return false; // self-unregister: no work left
	}
	return true;
}

void FPieSmokeSessionManager::AdvanceSession(FPieSmokeSession& Session)
{
	UWorld* PieWorld = FindActivePieWorld();
	if (!PieWorld)
	{
		return;
	}

	// (Re)resolve the tracked pawn — pawns (and their AnimInstances) can spawn a frame
	// or two after BeginPlay, so keep retrying until a valid one is cached.
	APawn* Pawn = Session.TargetPawn.IsValid() ? Session.TargetPawn.Get() : nullptr;
	if (!Pawn)
	{
		Pawn = ResolveTargetPawn(PieWorld, Session.PawnClassFilter, Session.SampleVarNames);
		Session.TargetPawn = Pawn;
	}

	const double SampleTime = FPlatformTime::Seconds() - Session.StartTimeSeconds;
	++Session.ObserverTickCount; // #8 AfterNTicks gate

	// Phase 9 (OG-E3): start session-scoped profiling ONCE on the first ready tick. AdvanceSession
	// is only reached after HasBegunPlay, so this brackets the capture to exactly the PIE window
	// (no pre-BeginPlay editor-idle frames). The matching stop runs on EVERY end path via
	// StopSessionProfiling (the finally-equivalent), so a crash/abort can never leave it capturing.
	if (!Session.bProfilingStarted &&
		(Session.bCsvProfile || Session.TraceChannels.Num() > 0))
	{
		Session.bProfilingStarted = true;
		StartSessionProfiling(Session);
	}

	// #9 refresh the runtime-identity snapshot from the live pawn (cached first tick,
	// re-checked each tick for anim_class_changed + the expected-class assert).
	UpdateRuntimeIdentity(Session.Identity, Pawn);

	// Phase 8 (OG-E2/E5): execute the declarative actor_setup spec ONCE, on the first ready
	// tick (the observer only reaches AdvanceSession after HasBegunPlay, so this is post-
	// BeginPlay). Spawn/apply/move all run against the live PIE world; results are captured
	// for the poll/stop report. Single-fire guarded so re-ticks never re-spawn.
	if (!Session.bActorSetupFired && Session.ActorSetups.Num() > 0)
	{
		Session.bActorSetupFired = true;
		FireActorSetup(Session, PieWorld);
	}

	// #7 view-target wiring, applied once on the first ready tick:
	//   (a) always record the ACTIVE view target so a black frame can be diagnosed;
	//   (b) when view_target_actor was requested, resolve that PIE actor and SetViewTarget
	//       on it so the captured frames frame the intended subject.
	if (Session.ObserverTickCount == 1)
	{
		if (APlayerController* PC = PieWorld->GetFirstPlayerController())
		{
			if (AActor* Current = PC->GetViewTarget())
			{
				Session.ActiveViewTargetName = Current->GetName();
				Session.ActiveViewTargetClass = Current->GetClass() ? Current->GetClass()->GetName() : FString();
			}

			if (!Session.ViewTargetActorRequest.IsEmpty())
			{
				AActor* Resolved = nullptr;
				// Match on Outliner label OR object name OR class-name substring (tolerant
				// resolution). GetActorLabel() is the editor display name a caller sees in the
				// Outliner — matching it first is why an editor-label request now resolves;
				// the object-name / class-name checks remain as fallback. GetActorLabel() is
				// WITH_EDITOR-only, but this whole module (and PIE itself) is editor-only.
				for (TActorIterator<AActor> It(PieWorld); It; ++It)
				{
					AActor* Candidate = *It;
					if (!Candidate) { continue; }
#if WITH_EDITOR
					const FString Label = Candidate->GetActorLabel();
#else
					const FString Label;
#endif
					const FString Nm = Candidate->GetName();
					const FString Cls = Candidate->GetClass() ? Candidate->GetClass()->GetName() : FString();
					if ((!Label.IsEmpty() && Label.Contains(Session.ViewTargetActorRequest)) ||
						Nm.Contains(Session.ViewTargetActorRequest) ||
						Cls.Contains(Session.ViewTargetActorRequest))
					{
						Resolved = Candidate;
						break;
					}
				}
				if (Resolved)
				{
					// APlayerController::SetViewTarget(AActor*, FViewTargetTransitionParams=default).
					PC->SetViewTarget(Resolved);
					Session.ViewTargetActorResolved = Resolved->GetName();
					Session.ViewTargetActorClass = Resolved->GetClass() ? Resolved->GetClass()->GetName() : FString();
					// Refresh the active view target now that we've retargeted.
					Session.ActiveViewTargetName = Session.ViewTargetActorResolved;
					Session.ActiveViewTargetClass = Session.ViewTargetActorClass;
				}
			}
		}
	}

	// #8 staged hooks fired from the observer (PrePie is handled synchronously by the
	// handler before PIE start). Each fires at most once at its lifecycle moment.
	if (Session.Stages.bAny)
	{
		// on_begin_play: first observer tick (the observer only reaches AdvanceSession
		// once HasBegunPlay is true, so this tick already satisfies "begin play").
		if (!Session.Stages.OnBeginPlay.bFired &&
			(!Session.Stages.OnBeginPlay.Python.IsEmpty() || Session.Stages.OnBeginPlay.Console.Num() > 0))
		{
			FireStage(Session.Stages.OnBeginPlay, PieWorld, SampleTime);
		}
		// after_n_ticks: fire once observer ticks reach the requested count.
		if (!Session.Stages.AfterNTicks.bFired &&
			(!Session.Stages.AfterNTicks.Python.IsEmpty() || Session.Stages.AfterNTicks.Console.Num() > 0) &&
			Session.ObserverTickCount >= Session.Stages.AfterNTicks.FireAfterTicks)
		{
			FireStage(Session.Stages.AfterNTicks, PieWorld, SampleTime);
		}
	}

	// #4 fire any due delayed probes against the LIVE PIE world (each fires once).
	for (FPieSmokeProbe& Probe : Session.Probes)
	{
		if (!Probe.bFired && SampleTime >= Probe.AtSeconds)
		{
			FireProbe(Probe, PieWorld, SampleTime);
		}
	}

	// ── Gap 9: time-series variant ────────────────────────────────────────────
	// Resolve the dotted target, fire typed provocations at their times, and sample the
	// dotted (UDS-friendly) variable paths (gated by SampleInterval, capped by MaxSamples).
	// This REPLACES the flat anim-var sampling below for a timeseries session.
	if (Session.bTimeseries)
	{
		// Reuse the cached resolved target while it is still valid; only run the full
		// actor+component scan when the weak pointer is stale (first tick, or after the
		// target actor dies). Keeps per-tick sampling O(1) instead of O(actors x ticks).
		UObject* Target = Session.CachedTimeseriesTarget.IsValid()
			? Session.CachedTimeseriesTarget.Get() : nullptr;
		if (!Target)
		{
			Target = ResolveTimeseriesTarget(PieWorld, Session);
			Session.CachedTimeseriesTarget = Target;
		}

		// Typed provocations fire once each when elapsed crosses their time.
		for (FPieProvocation& Prov : Session.Provocations)
		{
			if (!Prov.bFired && SampleTime >= Prov.AtSeconds)
			{
				FireProvocation(Prov, PieWorld, Target, SampleTime);
			}
		}

		// Sample the dotted vars, gated by SampleInterval and the MaxSamples guard.
		const bool bDue = (Session.LastSampleSeconds < 0.0) ||
			(SampleTime - Session.LastSampleSeconds >= Session.SampleInterval);
		if (bDue && Session.TimeseriesSamples.Num() < Session.MaxSamples)
		{
			FPieTimeseriesSample TSample;
			TSample.TimeSeconds = SampleTime;
			if (Target)
			{
				for (const FString& Path : Session.TimeseriesVarPaths)
				{
					FString TypeName;
					TSharedPtr<FJsonValue> Val = MonolithPieObject::ReadDottedValue(Target, Path, TypeName);
					if (Val.IsValid())
					{
						TSample.Vars.Emplace(Path, Val);
					}
				}
			}
			Session.TimeseriesSamples.Add(MoveTemp(TSample));
			Session.LastSampleSeconds = SampleTime;
		}
		return; // timeseries sessions do not run the flat-var / clip sampling below
	}

	FPieSmokeSample Sample;
	Sample.TimeSeconds = SampleTime;

	if (Pawn)
	{
		if (USkeletalMeshComponent* SkelComp = Pawn->FindComponentByClass<USkeletalMeshComponent>())
		{
			if (UAnimInstance* Anim = SkelComp->GetAnimInstance())
			{
				for (const FString& VarName : Session.SampleVarNames)
				{
					FPieSmokeSampleVar Var;
					if (ReadAnimVar(Anim, VarName, Var))
					{
						Sample.Vars.Add(MoveTemp(Var));
					}
				}
			}
		}
	}

	// Clip variant: capture a viewport frame at most once per CaptureInterval.
	if (Session.bCaptureFrames && !Session.bCaptureDeferred)
	{
		const bool bDue = (Session.LastCaptureSeconds < 0.0) ||
			(SampleTime - Session.LastCaptureSeconds >= Session.CaptureInterval);
		if (bDue)
		{
			// #8 before_capture stage fires once, immediately before the first frame grab.
			if (Session.Stages.bAny && !Session.Stages.BeforeCapture.bFired &&
				(!Session.Stages.BeforeCapture.Python.IsEmpty() || Session.Stages.BeforeCapture.Console.Num() > 0))
			{
				FireStage(Session.Stages.BeforeCapture, PieWorld, SampleTime);
			}

			const FString FramePath = Session.OutputDir /
				FString::Printf(TEXT("frame_%03d.png"), Session.CaptureFrameIndex);

			// include_ui: ride the engine screenshot path (the `shot showui`
			// mechanism) — Slate composites the full backbuffer (game + UMG)
			// and writes the file at END OF FRAME. Asynchronous by nature:
			// bSaved/uniformity can't be verified here, so these frames count
			// as requested-valid and the clip directory holds the truth.
			if (Session.bIncludeUi)
			{
				IFileManager::Get().MakeDirectory(*Session.OutputDir, true);
				FScreenshotRequest::RequestScreenshot(FramePath, /*bShowUI*/true, /*bAddFilenameSuffix*/false);
				Sample.FramePath = FramePath;
				Sample.bFrameUniform = false;
				Sample.bFrameValid = true;
				if (Session.CaptureFrameIndex >= Session.DiscardFirstFrames)
				{
					++Session.ValidFrames;
				}
				Session.LastCaptureSeconds = SampleTime;
				++Session.CaptureFrameIndex;
			}
			else
			{
			// #7 flush the render thread before the very first ReadPixels so a warm-up /
			// uniform first frame is not produced in the first place.
			const bool bFirstCapture = (Session.CaptureFrameIndex == 0);
			const FCaptureResult Cap = CapturePieFrame(FramePath, bFirstCapture);
			if (Cap.bSaved)
			{
				Sample.FramePath = FramePath;
				// #7 per-frame validity verdict (uniform / all-black => unrendered).
				Sample.bFrameUniform = Cap.bUniform;
				Sample.bFrameValid = Cap.bValid;
				// #7 first-frame warm-up: the first DiscardFirstFrames captured frames are saved
				// (the clip stays complete) but excluded from valid/invalid accounting, so an
				// un-warmed first frame can't false-fail the session's captured_ok rollup.
				const bool bCountThisFrame = (Session.CaptureFrameIndex >= Session.DiscardFirstFrames);
				if (bCountThisFrame)
				{
					if (Cap.bValid) { ++Session.ValidFrames; } else { ++Session.InvalidFrames; }
				}
				Session.LastCaptureSeconds = SampleTime;
				++Session.CaptureFrameIndex;
			}
			else if (Session.CaptureFrameIndex == 0 && SampleTime > Session.CaptureInterval * 2.0)
			{
				// Viewport never produced pixels — flag clip capture deferred but keep
				// the session running so anim sampling + log counts still complete.
				Session.bCaptureDeferred = true;
				UE_LOG(LogMonolithPieSmoke, Warning,
					TEXT("capture_pie_movement_clip: PIE viewport unavailable for session %s — capture deferred."),
					*Session.Id);
			}
			} // !bIncludeUi (scene-only ReadPixels path)
		}
	}

	Session.Samples.Add(MoveTemp(Sample));
}
