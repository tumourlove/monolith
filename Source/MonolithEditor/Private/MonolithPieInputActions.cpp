#include "MonolithPieInputActions.h"
#include "MonolithPieObjectActions.h"
#include "MonolithParamSchema.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/LocalPlayer.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/SpectatorPawn.h"
#include "Kismet/GameplayStatics.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/UObjectGlobals.h"

// pie_inject_key: raw key events through APlayerController::InputKey(const FInputKeyEventArgs&).
// The FInputKeyEventArgs overload and FInputKeyEventArgs::CreateSimulated have the same
// signatures in UE 5.7 and 5.8, so no ENGINE_MINOR_VERSION gate is needed.
#include "InputKeyEventArgs.h"
#include "InputCoreTypes.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/PlayerInput.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/PlatformTime.h"

#include "InputAction.h"
#include "InputActionValue.h"
#include "EnhancedInputSubsystems.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithPieInput, Log, All);

// =============================================================================
//  Shared PIE-input plumbing (game-thread-only).
// =============================================================================

namespace
{
	// Resolve the active PIE world (first EWorldType::PIE context), or nullptr.
	// Mirrors MonolithPieObject::FindPieWorld so the input actions share one notion of
	// "the running PIE world".
	UWorld* GetPieWorld()
	{
		return MonolithPieObject::FindPieWorld();
	}

	// Resolve the player controller for player_index against the PIE world. Index 0 is the
	// usual single-player case. Returns nullptr if PIE isn't running or the index has no PC.
	APlayerController* ResolvePlayerController(UWorld* PieWorld, int32 PlayerIndex)
	{
		if (!PieWorld)
		{
			return nullptr;
		}
		return UGameplayStatics::GetPlayerController(PieWorld, PlayerIndex);
	}

	// Read pitch/yaw/roll out of a params blob into a rotator (missing components default 0).
	FRotator ReadRotator(const TSharedPtr<FJsonObject>& Params)
	{
		double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
		Params->TryGetNumberField(TEXT("pitch"), Pitch);
		Params->TryGetNumberField(TEXT("yaw"), Yaw);
		Params->TryGetNumberField(TEXT("roll"), Roll);
		return FRotator(static_cast<double>(Pitch), static_cast<double>(Yaw), static_cast<double>(Roll));
	}

	// ── Held-rotation / repeated-input re-apply registry ──────────────────
	//
	// A single shared FTSTicker re-applies any active "holds" each editor frame. Holds are
	// keyed by player_index. A hold counts down its remaining frames every tick; at zero it is
	// dropped. When no holds remain the ticker self-unregisters. World validity is checked each
	// tick so a torn-down PIE world is never dereferenced.

	struct FHeldRotation
	{
		int32 PlayerIndex = 0;
		FRotator Rotation = FRotator::ZeroRotator;
		int32 FramesRemaining = 0; // re-applied while > 0
	};

	struct FHeldInput
	{
		int32 PlayerIndex = 0;
		TWeakObjectPtr<const UInputAction> Action;
		FInputActionValue Value;
		int32 FramesRemaining = 0; // injected while > 0
	};

	class FPieInputHoldRegistry
	{
	public:
		static FPieInputHoldRegistry& Get()
		{
			static FPieInputHoldRegistry Singleton;
			return Singleton;
		}

		// Replace any existing rotation hold for this player_index (latest write wins) and
		// ensure the ticker is running. FramesRemaining of 0 means a one-shot (no hold queued).
		void SetRotationHold(int32 PlayerIndex, const FRotator& Rotation, int32 HoldFrames)
		{
			if (HoldFrames <= 0)
			{
				return; // one-shot already applied by the handler; nothing to queue
			}
			for (FHeldRotation& H : HeldRotations)
			{
				if (H.PlayerIndex == PlayerIndex)
				{
					H.Rotation = Rotation;
					H.FramesRemaining = HoldFrames;
					EnsureTicker();
					return;
				}
			}
			FHeldRotation New;
			New.PlayerIndex = PlayerIndex;
			New.Rotation = Rotation;
			New.FramesRemaining = HoldFrames;
			HeldRotations.Add(New);
			EnsureTicker();
		}

		// Queue a repeated input injection. RepeatFrames of <= 1 means the handler already did
		// the single injection; nothing is queued. RepeatFrames > 1 queues (RepeatFrames - 1)
		// additional per-frame injections.
		void SetInputHold(int32 PlayerIndex, const UInputAction* Action, const FInputActionValue& Value, int32 ExtraFrames)
		{
			if (!Action || ExtraFrames <= 0)
			{
				return;
			}
			FHeldInput New;
			New.PlayerIndex = PlayerIndex;
			New.Action = Action;
			New.Value = Value;
			New.FramesRemaining = ExtraFrames;
			HeldInputs.Add(New);
			EnsureTicker();
		}

		// Drop every hold AND remove the ticker (called on PIE end + module shutdown). The
		// RemoveTicker is load-bearing: FTSTicker is an engine-lifetime singleton and the ticker
		// is bound with CreateRaw(this,...). Merely emptying the hold arrays would leave that
		// ticker registered, so if the module unloads while it is live it would fire into freed
		// registry memory — a use-after-free. StopTicker tears it down deterministically.
		void ClearAll()
		{
			HeldRotations.Reset();
			HeldInputs.Reset();
			StopTicker();
		}

	private:
		FPieInputHoldRegistry() = default;

		void EnsureTicker()
		{
			if (TickerHandle.IsValid())
			{
				return;
			}
			TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateRaw(this, &FPieInputHoldRegistry::OnTick), 0.0f);
		}

		// Unregister the shared ticker if it is live. RemoveTicker is a STATIC FTSTicker API
		// (verified offline: Containers/Ticker.h line 66 — `static void RemoveTicker(FDelegateHandle)`),
		// unlike AddTicker which is an instance method on GetCoreTicker().
		void StopTicker()
		{
			if (TickerHandle.IsValid())
			{
				FTSTicker::RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
		}

		bool OnTick(float /*DeltaTime*/)
		{
			UWorld* PieWorld = GetPieWorld();
			if (!PieWorld || PieWorld->bIsTearingDown)
			{
				// PIE gone (or tearing down) — drop all holds and stop ticking. Returning false
				// makes the engine auto-remove this ticker, so we only clear our cached handle
				// here (no RemoveTicker — that would double-remove). With the PIE-end hook also
				// calling StopTicker(), this guard is a belt-and-braces second line of defence.
				HeldRotations.Reset();
				HeldInputs.Reset();
				TickerHandle.Reset();
				return false;
			}

			// Re-apply rotation holds (executed each frame — best-effort vs per-tick camera
			// systems that may re-write ControlRotation later in the same frame).
			for (int32 Index = HeldRotations.Num() - 1; Index >= 0; --Index)
			{
				FHeldRotation& H = HeldRotations[Index];
				if (APlayerController* PC = ResolvePlayerController(PieWorld, H.PlayerIndex))
				{
					PC->SetControlRotation(H.Rotation);
				}
				if (--H.FramesRemaining <= 0)
				{
					HeldRotations.RemoveAt(Index);
				}
			}

			// Re-inject repeated input actions.
			for (int32 Index = HeldInputs.Num() - 1; Index >= 0; --Index)
			{
				FHeldInput& H = HeldInputs[Index];
				const UInputAction* Action = H.Action.Get();
				bool bDropped = (Action == nullptr);
				if (Action)
				{
					if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
						ULocalPlayer::GetSubsystemFromController<UEnhancedInputLocalPlayerSubsystem>(
							ResolvePlayerController(PieWorld, H.PlayerIndex)))
					{
						Subsystem->InjectInputForAction(Action, H.Value, {}, {});
					}
					else
					{
						bDropped = true; // no subsystem — stop trying
					}
				}
				if (bDropped || --H.FramesRemaining <= 0)
				{
					HeldInputs.RemoveAt(Index);
				}
			}

			if (HeldRotations.Num() == 0 && HeldInputs.Num() == 0)
			{
				TickerHandle.Reset();
				return false; // self-unregister
			}
			return true;
		}

		TArray<FHeldRotation> HeldRotations;
		TArray<FHeldInput> HeldInputs;
		FTSTicker::FDelegateHandle TickerHandle;
	};

	// ── Spectator detach/reverse state ────────────────────────────────────
	//
	// Stores the originally-possessed pawn per player_index so pie_possess_spectator_free can
	// re-possess on disable. Game-thread-only; cleared on PIE end.

	struct FSpectatorState
	{
		int32 PlayerIndex = 0;
		TWeakObjectPtr<APawn> OriginalPawn;
	};

	class FPieSpectatorRegistry
	{
	public:
		static FPieSpectatorRegistry& Get()
		{
			static FPieSpectatorRegistry Singleton;
			return Singleton;
		}

		void StoreOriginalPawn(int32 PlayerIndex, APawn* Pawn)
		{
			for (FSpectatorState& S : States)
			{
				if (S.PlayerIndex == PlayerIndex)
				{
					S.OriginalPawn = Pawn;
					return;
				}
			}
			FSpectatorState New;
			New.PlayerIndex = PlayerIndex;
			New.OriginalPawn = Pawn;
			States.Add(New);
		}

		// Returns the stored original pawn for this player_index (may be stale/null) and clears
		// the entry. Returns nullptr if none was stored.
		APawn* TakeOriginalPawn(int32 PlayerIndex)
		{
			for (int32 Index = 0; Index < States.Num(); ++Index)
			{
				if (States[Index].PlayerIndex == PlayerIndex)
				{
					APawn* Pawn = States[Index].OriginalPawn.Get();
					States.RemoveAt(Index);
					return Pawn;
				}
			}
			return nullptr;
		}

		bool HasState(int32 PlayerIndex) const
		{
			for (const FSpectatorState& S : States)
			{
				if (S.PlayerIndex == PlayerIndex) { return true; }
			}
			return false;
		}

		void ClearAll() { States.Reset(); }

	private:
		FPieSpectatorRegistry() = default;
		TArray<FSpectatorState> States;
	};
}

// =============================================================================
//  Raw key sequences (pie_inject_key)
// =============================================================================
//
// A raw key injection is a short ordered sequence of key events (modifier presses, the key
// itself, the release, modifier releases). Each step may require a minimum number of engine
// frames and a minimum wall-clock time since the previous step, so a press and its release never
// land in the same frame and chord modifiers are down before the chorded key is pressed. Steps
// that are ready run immediately in the handler; the rest run from a shared FTSTicker that
// self-unregisters when no sequences remain and is cleared on PIE end.

namespace
{
	// Frames between ordered steps. Two frames (not one) guarantee that at least one full PIE
	// world tick (PlayerTick -> ProcessPlayerInput) runs between the steps regardless of whether
	// the handler / core ticker ran before or after the world tick in the frame that queued them.
	constexpr int32 PieKeyOrderedStepFrameGap = 2;

	struct FPieKeyStep
	{
		FKey Key;
		EInputEvent Event = IE_Pressed;
		float Amount = 1.0f;
		int32 MinFrameGap = 0;          // engine frames required since the previous step
		double MinSecondsSincePrev = 0; // wall-clock seconds required since the previous step
		bool bRepeatAxisWhileWaiting = false; // while waiting, re-inject RepeatAmount on Key each frame (IE_Axis)
		float RepeatAmount = 0.0f;
	};

	struct FPieKeySequence
	{
		TWeakObjectPtr<APlayerController> Controller;
		TArray<FPieKeyStep> Steps;
		int32 NextStep = 0;
		uint64 LastStepFrame = 0;
		double LastStepTime = 0.0;
	};

	// The input device to stamp on injected events: the primary device of the controller's
	// platform user, so APlayerController::InputKey's bFilterInputByPlatformUser check accepts it.
	// Falls back to the mapper's default device when the user has no mapped device.
	FInputDeviceId PieKeyResolveInputDevice(const APlayerController* PC)
	{
		IPlatformInputDeviceMapper& Mapper = IPlatformInputDeviceMapper::Get();
		FInputDeviceId Device = Mapper.GetPrimaryInputDeviceForUser(PC->GetPlatformUserId());
		if (!Device.IsValid())
		{
			Device = Mapper.GetDefaultInputDevice();
		}
		return Device;
	}

	// Send one key event to the controller. Returns APlayerController::InputKey's result
	// (true when some input handler consumed it).
	bool PieKeySendEvent(APlayerController* PC, const FKey& Key, EInputEvent Event, float Amount)
	{
		const FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(
			Key, Event, Amount, /*InNumSamplesOverride=*/-1, PieKeyResolveInputDevice(PC));
		return PC->InputKey(Args);
	}

	// Run every ready step of a sequence. Returns true when the sequence is finished (all steps
	// ran, or the controller is gone). OutExecuted / bOutAnyConsumed report on this call only.
	bool PieKeyAdvanceSequence(FPieKeySequence& Seq, int32& OutExecuted, bool& bOutAnyConsumed)
	{
		APlayerController* PC = Seq.Controller.Get();
		if (!PC || !PC->GetWorld() || PC->GetWorld()->bIsTearingDown)
		{
			return true;
		}

		while (Seq.NextStep < Seq.Steps.Num())
		{
			const FPieKeyStep& Step = Seq.Steps[Seq.NextStep];
			const uint64 FramesSince = GFrameCounter - Seq.LastStepFrame;
			const double SecondsSince = FPlatformTime::Seconds() - Seq.LastStepTime;
			if (FramesSince < static_cast<uint64>(Step.MinFrameGap) || SecondsSince < Step.MinSecondsSincePrev)
			{
				if (Step.bRepeatAxisWhileWaiting && FramesSince > 0)
				{
					PieKeySendEvent(PC, Step.Key, IE_Axis, Step.RepeatAmount);
				}
				return false;
			}

			bOutAnyConsumed |= PieKeySendEvent(PC, Step.Key, Step.Event, Step.Amount);
			++OutExecuted;
			Seq.LastStepFrame = GFrameCounter;
			Seq.LastStepTime = FPlatformTime::Seconds();
			++Seq.NextStep;
		}
		return true;
	}

	class FPieKeySequenceRegistry
	{
	public:
		static FPieKeySequenceRegistry& Get()
		{
			static FPieKeySequenceRegistry Singleton;
			return Singleton;
		}

		void Add(FPieKeySequence&& Seq)
		{
			Sequences.Add(MoveTemp(Seq));
			EnsureTicker();
		}

		int32 NumPending() const { return Sequences.Num(); }

		// Drop all pending sequences and remove the ticker (PIE end + module shutdown). The
		// RemoveTicker matters for the same reason as FPieInputHoldRegistry::ClearAll: the ticker
		// is bound CreateRaw to this singleton.
		void ClearAll()
		{
			Sequences.Reset();
			if (TickerHandle.IsValid())
			{
				FTSTicker::RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
		}

	private:
		FPieKeySequenceRegistry() = default;

		void EnsureTicker()
		{
			if (!TickerHandle.IsValid())
			{
				TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateRaw(this, &FPieKeySequenceRegistry::OnTick), 0.0f);
			}
		}

		bool OnTick(float /*DeltaTime*/)
		{
			UWorld* PieWorld = GetPieWorld();
			if (!PieWorld || PieWorld->bIsTearingDown)
			{
				// Returning false auto-removes the ticker; only clear the cached handle.
				Sequences.Reset();
				TickerHandle.Reset();
				return false;
			}

			for (int32 Index = Sequences.Num() - 1; Index >= 0; --Index)
			{
				int32 Executed = 0;
				bool bConsumed = false;
				if (PieKeyAdvanceSequence(Sequences[Index], Executed, bConsumed))
				{
					Sequences.RemoveAt(Index);
				}
			}

			if (Sequences.Num() == 0)
			{
				TickerHandle.Reset();
				return false;
			}
			return true;
		}

		TArray<FPieKeySequence> Sequences;
		FTSTicker::FDelegateHandle TickerHandle;
	};
}

// Drop all per-session input/spectator state on PIE end so the re-apply ticker never touches a
// torn-down PIE world.
namespace
{
	void ClearPieInputState()
	{
		FPieInputHoldRegistry::Get().ClearAll();
		FPieSpectatorRegistry::Get().ClearAll();
		FPieKeySequenceRegistry::Get().ClearAll();
	}

	// A single PIE-end hook. PrePIEEnded fires earlier in teardown than EndPIE (before the world
	// is fully gone), which is the safer point to stop the re-apply ticker — and ClearAll's
	// StopTicker is idempotent, so one hook is sufficient. (EndPIE would be redundant.)
	FDelegateHandle GPrePieEndedHandle;
}

void FMonolithPieInputActions::RegisterPieEndHook()
{
	if (!GPrePieEndedHandle.IsValid())
	{
		GPrePieEndedHandle = FEditorDelegates::PrePIEEnded.AddLambda([](const bool /*bIsSimulating*/) { ClearPieInputState(); });
	}
}

void FMonolithPieInputActions::UnregisterPieEndHook()
{
	if (GPrePieEndedHandle.IsValid())
	{
		FEditorDelegates::PrePIEEnded.Remove(GPrePieEndedHandle);
		GPrePieEndedHandle.Reset();
	}
	ClearPieInputState();
}

// =============================================================================
//  UInputAction resolution (by asset path or short name)
// =============================================================================

namespace
{
	// Resolve a UInputAction from a path or short name. A path-like string ("/Game/...") is
	// loaded directly; otherwise the asset registry is searched for a UInputAction whose asset
	// name matches. Returns nullptr + a reason when unresolved.
	const UInputAction* ResolveInputAction(const FString& Spec, FString& OutError)
	{
		if (Spec.IsEmpty())
		{
			OutError = TEXT("input_action is empty");
			return nullptr;
		}

		// Path-like: load directly.
		if (Spec.StartsWith(TEXT("/")))
		{
			if (const UInputAction* Action = LoadObject<UInputAction>(nullptr, *Spec))
			{
				return Action;
			}
			OutError = FString::Printf(TEXT("No UInputAction asset at path '%s'"), *Spec);
			return nullptr;
		}

		// Short name: scan the asset registry for a UInputAction whose name matches.
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		AR.GetAssetsByClass(UInputAction::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);

		const UInputAction* FirstMatch = nullptr;
		int32 MatchCount = 0;
		for (const FAssetData& Data : Assets)
		{
			if (Data.AssetName.ToString() == Spec)
			{
				if (const UInputAction* Action = Cast<UInputAction>(Data.GetAsset()))
				{
					if (!FirstMatch) { FirstMatch = Action; }
					++MatchCount;
				}
			}
		}
		if (FirstMatch)
		{
			if (MatchCount > 1)
			{
				UE_LOG(LogMonolithPieInput, Warning,
					TEXT("input_action '%s' matched %d assets — using the first. Pass a full /Game/... path to disambiguate."),
					*Spec, MatchCount);
			}
			return FirstMatch;
		}

		OutError = FString::Printf(TEXT("No UInputAction asset named '%s' found (pass a /Game/... path to load directly)"), *Spec);
		return nullptr;
	}

	// Build an FInputActionValue from a JSON value:
	//   bool        -> Boolean
	//   number      -> Axis1D
	//   array[2]    -> Axis2D
	//   array[3]    -> Axis3D
	// Returns false + a reason on an unsupported shape.
	bool BuildInputActionValue(const TSharedPtr<FJsonValue>& Value, FInputActionValue& OutValue, FString& OutError)
	{
		if (!Value.IsValid())
		{
			OutError = TEXT("value is missing");
			return false;
		}
		switch (Value->Type)
		{
		case EJson::Boolean:
			OutValue = FInputActionValue(Value->AsBool());
			return true;
		case EJson::Number:
			OutValue = FInputActionValue(static_cast<float>(Value->AsNumber()));
			return true;
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
			if (Arr.Num() == 2)
			{
				OutValue = FInputActionValue(FVector2D(Arr[0]->AsNumber(), Arr[1]->AsNumber()));
				return true;
			}
			if (Arr.Num() == 3)
			{
				OutValue = FInputActionValue(FVector(Arr[0]->AsNumber(), Arr[1]->AsNumber(), Arr[2]->AsNumber()));
				return true;
			}
			OutError = FString::Printf(TEXT("value array must have 2 (Axis2D) or 3 (Axis3D) elements, got %d"), Arr.Num());
			return false;
		}
		default:
			OutError = TEXT("value must be a bool, number, array[2], or array[3]");
			return false;
		}
	}
}

// =============================================================================
//  Registration
// =============================================================================

void FMonolithPieInputActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("editor"), TEXT("pie_set_control_rotation"),
		TEXT("Set the control rotation on a Play-In-Editor player controller (APlayerController::SetControlRotation). "
		     "MUTATES LIVE PIE STATE. Params: pitch / yaw / roll (degrees; omitted components default to 0), optional player_index (default 0). "
		     "hold_frames (default 0) re-applies the rotation each frame for that many frames so it can outlast a per-tick camera/control system that "
		     "re-writes ControlRotation. The hold is BEST-EFFORT, not frame-perfect: a camera director that runs later in the frame can still win for that "
		     "frame, and exact determinism in PIE is not guaranteed. The re-apply hook clears itself on PIE end. No-ops with a clean error when PIE is not running."),
		FMonolithActionHandler::CreateStatic(&HandleSetControlRotation),
		FParamSchemaBuilder()
			.Optional(TEXT("pitch"), TEXT("number"), TEXT("Control-rotation pitch in degrees (default 0)."), TEXT("0"))
			.Optional(TEXT("yaw"), TEXT("number"), TEXT("Control-rotation yaw in degrees (default 0)."), TEXT("0"))
			.Optional(TEXT("roll"), TEXT("number"), TEXT("Control-rotation roll in degrees (default 0)."), TEXT("0"))
			.Optional(TEXT("player_index"), TEXT("number"), TEXT("Local player index (default 0)."), TEXT("0"))
			.Optional(TEXT("hold_frames"), TEXT("number"), TEXT("Re-apply the rotation each frame for this many frames (default 0 = one-shot). Best-effort vs per-tick camera systems."), TEXT("0"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("pie_inject_input_action"),
		TEXT("Inject a value for an Enhanced Input action into a live PIE local player (UEnhancedInputLocalPlayerSubsystem::InjectInputForAction), "
		     "running that action's modifiers and triggers as if real input arrived. MUTATES LIVE PIE STATE. "
		     "input_action is a UInputAction asset path (/Game/...) or short asset name (registry-resolved; first match wins). "
		     "value maps to FInputActionValue by JSON shape: bool -> Boolean, number -> Axis1D, array[2] -> Axis2D, array[3] -> Axis3D. "
		     "Optional player_index (default 0); repeat_frames (default 1) re-injects the value each frame for that many frames. "
		     "No-ops with a clean error when PIE is not running or the action asset cannot be resolved."),
		FMonolithActionHandler::CreateStatic(&HandleInjectInputAction),
		FParamSchemaBuilder()
			.Required(TEXT("input_action"), TEXT("string"), TEXT("UInputAction asset path (/Game/...) or short asset name."))
			.Required(TEXT("value"), TEXT("any"), TEXT("Value to inject: bool (Boolean), number (Axis1D), array[2] (Axis2D), or array[3] (Axis3D)."))
			.Optional(TEXT("player_index"), TEXT("number"), TEXT("Local player index (default 0)."), TEXT("0"))
			.Optional(TEXT("repeat_frames"), TEXT("number"), TEXT("Inject the value each frame for this many frames (default 1 = single injection)."), TEXT("1"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("pie_possess_spectator_free"),
		TEXT("Detach a PIE player controller to a free-fly spectator pawn for camera-independent observation, or re-possess the original pawn. "
		     "MUTATES LIVE PIE STATE. enable=true stores the currently-possessed pawn, unpossesses it, and switches the controller to the Spectating state "
		     "(APlayerController::ChangeState(NAME_Spectating)) which spawns a spectator pawn from the game mode's SpectatorClass. enable=false re-possesses the "
		     "stored original pawn (ChangeState(NAME_Playing) + Possess). The original pawn is held as a weak pointer and cleared on PIE end. "
		     "No-ops with a clean error when PIE is not running. Note: spectator spawning depends on the game mode providing a SpectatorClass; if none is "
		     "configured the controller still enters the Spectating state but no free-fly pawn is created."),
		FMonolithActionHandler::CreateStatic(&HandlePossessSpectatorFree),
		FParamSchemaBuilder()
			.Required(TEXT("enable"), TEXT("bool"), TEXT("true = detach to a free-fly spectator; false = re-possess the original pawn."))
			.Optional(TEXT("player_index"), TEXT("number"), TEXT("Local player index (default 0)."), TEXT("0"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("pie_inject_key"),
		TEXT("Inject a raw key event into a live PIE player controller through APlayerController::InputKey, stamped with the primary input device of "
		     "the controller's platform user. Unlike pie_inject_input_action this goes through the normal key path, so Enhanced Input mappings "
		     "(including chorded actions and modifiers/triggers), legacy key bindings and Blueprint Debug Key events all see it. MUTATES LIVE PIE STATE. "
		     "key is an FKey name (EKeys), e.g. SpaceBar, W, LeftShift, LeftMouseButton, Gamepad_FaceButton_Bottom, MouseX, Gamepad_LeftX. "
		     "event: pressed (key down, stays down until a released call), released, tap (press now, release on a later frame after hold_seconds), "
		     "axis (analog keys only: send axis_value; with hold_seconds > 0 the value is re-sent every frame for that long and then zeroed). "
		     "modifiers: keys held around the main key; they are pressed first, the main key follows at least two frames later, and they are released "
		     "at least two frames after the main key's release (for pressed: modifiers then key; for released: key then modifiers). "
		     "A press and its release are never sent in the same frame. Pending steps are dropped on PIE end. "
		     "No-ops with a clean error when PIE is not running or a key name is not a valid FKey."),
		FMonolithActionHandler::CreateStatic(&HandleInjectKey),
		FParamSchemaBuilder()
			.Required(TEXT("key"), TEXT("string"), TEXT("FKey name, e.g. SpaceBar, E, LeftShift, LeftMouseButton, Gamepad_FaceButton_Bottom, MouseX, Gamepad_LeftX."))
			.Optional(TEXT("event"), TEXT("string"), TEXT("pressed | released | tap | axis (default tap)."), TEXT("tap"))
			.Optional(TEXT("axis_value"), TEXT("number"), TEXT("Value for event=axis (default 1.0). Ignored for other events."), TEXT("1.0"))
			.Optional(TEXT("hold_seconds"), TEXT("number"), TEXT("tap: time the key stays down before release (default 0 = released two frames later). axis: time to keep re-sending the value before zeroing it (default 0 = single sample, no auto-zero). Max 60."), TEXT("0"))
			.Optional(TEXT("modifiers"), TEXT("array"), TEXT("Digital keys to hold around the main key, e.g. [\"LeftShift\", \"LeftControl\"]. A comma-separated string is also accepted."))
			.Optional(TEXT("player_index"), TEXT("number"), TEXT("Local player index (default 0)."), TEXT("0"))
			.Build());

	UE_LOG(LogMonolithPieInput, Log, TEXT("MonolithEditor: registered 4 PIE input/control actions"));
}

// =============================================================================
//  pie_set_control_rotation
// =============================================================================

FMonolithActionResult FMonolithPieInputActions::HandleSetControlRotation(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* PieWorld = GetPieWorld();
	if (!PieWorld)
	{
		return FMonolithActionResult::Error(TEXT("PIE not running — start Play-In-Editor first"));
	}

	int32 PlayerIndex = 0;
	{
		double Idx = 0.0;
		if (Params->TryGetNumberField(TEXT("player_index"), Idx)) { PlayerIndex = static_cast<int32>(Idx); }
	}

	APlayerController* PC = ResolvePlayerController(PieWorld, PlayerIndex);
	if (!PC)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No player controller at player_index %d in the running PIE world"), PlayerIndex));
	}

	const FRotator Rotation = ReadRotator(Params);

	int32 HoldFrames = 0;
	{
		double Frames = 0.0;
		if (Params->TryGetNumberField(TEXT("hold_frames"), Frames)) { HoldFrames = FMath::Max(0, static_cast<int32>(Frames)); }
	}

	// Apply once immediately, then queue the per-frame re-apply (if any).
	PC->SetControlRotation(Rotation);
	if (HoldFrames > 0)
	{
		FPieInputHoldRegistry::Get().SetRotationHold(PlayerIndex, Rotation, HoldFrames);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("player_index"), PlayerIndex);
	Root->SetNumberField(TEXT("pitch"), Rotation.Pitch);
	Root->SetNumberField(TEXT("yaw"), Rotation.Yaw);
	Root->SetNumberField(TEXT("roll"), Rotation.Roll);
	Root->SetNumberField(TEXT("hold_frames"), HoldFrames);
	Root->SetBoolField(TEXT("applied"), true);
	Root->SetBoolField(TEXT("best_effort_hold"), HoldFrames > 0);
	return FMonolithActionResult::Success(Root);
}

// =============================================================================
//  pie_inject_input_action
// =============================================================================

FMonolithActionResult FMonolithPieInputActions::HandleInjectInputAction(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* PieWorld = GetPieWorld();
	if (!PieWorld)
	{
		return FMonolithActionResult::Error(TEXT("PIE not running — start Play-In-Editor first"));
	}

	FString ActionSpec;
	if (!Params->TryGetStringField(TEXT("input_action"), ActionSpec) || ActionSpec.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("pie_inject_input_action requires 'input_action' (asset path or name)"));
	}

	FString ResolveError;
	const UInputAction* Action = ResolveInputAction(ActionSpec, ResolveError);
	if (!Action)
	{
		return FMonolithActionResult::Error(ResolveError);
	}

	const TSharedPtr<FJsonValue> ValueField = Params->TryGetField(TEXT("value"));
	FInputActionValue ActionValue;
	FString ValueError;
	if (!BuildInputActionValue(ValueField, ActionValue, ValueError))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("pie_inject_input_action 'value' invalid: %s"), *ValueError));
	}

	int32 PlayerIndex = 0;
	{
		double Idx = 0.0;
		if (Params->TryGetNumberField(TEXT("player_index"), Idx)) { PlayerIndex = static_cast<int32>(Idx); }
	}

	int32 RepeatFrames = 1;
	{
		double Frames = 1.0;
		if (Params->TryGetNumberField(TEXT("repeat_frames"), Frames)) { RepeatFrames = FMath::Max(1, static_cast<int32>(Frames)); }
	}

	APlayerController* PC = ResolvePlayerController(PieWorld, PlayerIndex);
	if (!PC)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No player controller at player_index %d in the running PIE world"), PlayerIndex));
	}

	UEnhancedInputLocalPlayerSubsystem* Subsystem =
		ULocalPlayer::GetSubsystemFromController<UEnhancedInputLocalPlayerSubsystem>(PC);
	if (!Subsystem)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Player %d has no UEnhancedInputLocalPlayerSubsystem (is Enhanced Input the active input system?)"), PlayerIndex));
	}

	// Inject once now; queue any additional per-frame injections.
	Subsystem->InjectInputForAction(Action, ActionValue, {}, {});
	if (RepeatFrames > 1)
	{
		FPieInputHoldRegistry::Get().SetInputHold(PlayerIndex, Action, ActionValue, RepeatFrames - 1);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("input_action"), Action->GetPathName());
	Root->SetNumberField(TEXT("player_index"), PlayerIndex);
	Root->SetNumberField(TEXT("repeat_frames"), RepeatFrames);
	Root->SetBoolField(TEXT("injected"), true);
	return FMonolithActionResult::Success(Root);
}

// =============================================================================
//  pie_possess_spectator_free
// =============================================================================

FMonolithActionResult FMonolithPieInputActions::HandlePossessSpectatorFree(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* PieWorld = GetPieWorld();
	if (!PieWorld)
	{
		return FMonolithActionResult::Error(TEXT("PIE not running — start Play-In-Editor first"));
	}

	bool bEnable = false;
	if (!Params->TryGetBoolField(TEXT("enable"), bEnable))
	{
		return FMonolithActionResult::Error(TEXT("pie_possess_spectator_free requires 'enable' (bool)"));
	}

	int32 PlayerIndex = 0;
	{
		double Idx = 0.0;
		if (Params->TryGetNumberField(TEXT("player_index"), Idx)) { PlayerIndex = static_cast<int32>(Idx); }
	}

	APlayerController* PC = ResolvePlayerController(PieWorld, PlayerIndex);
	if (!PC)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No player controller at player_index %d in the running PIE world"), PlayerIndex));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("player_index"), PlayerIndex);
	Root->SetBoolField(TEXT("enable"), bEnable);

	if (bEnable)
	{
		// Store the currently-possessed pawn for reversal, then detach to a spectator.
		APawn* OriginalPawn = PC->GetPawn();
		FPieSpectatorRegistry::Get().StoreOriginalPawn(PlayerIndex, OriginalPawn);

		// ChangeState(NAME_Spectating) routes through BeginSpectatingState(), which
		// unpossesses the current pawn and spawns a spectator pawn (from GameState->SpectatorClass).
		PC->ChangeState(NAME_Spectating);

		const APawn* Spectator = PC->GetSpectatorPawn();
		Root->SetStringField(TEXT("original_pawn"), OriginalPawn ? OriginalPawn->GetName() : TEXT("None"));
		Root->SetStringField(TEXT("spectator_pawn"), Spectator ? Spectator->GetName() : TEXT("None"));
		Root->SetBoolField(TEXT("spectating"), PC->IsInState(NAME_Spectating));
		if (!Spectator)
		{
			Root->SetStringField(TEXT("note"),
				TEXT("Controller entered the Spectating state but no spectator pawn was spawned — the game mode likely has no SpectatorClass configured."));
		}
		return FMonolithActionResult::Success(Root);
	}

	// Disable: re-possess the stored original pawn.
	APawn* OriginalPawn = FPieSpectatorRegistry::Get().TakeOriginalPawn(PlayerIndex);
	if (!OriginalPawn)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No stored original pawn for player_index %d — was pie_possess_spectator_free called with enable=true first? (the pawn may also have been destroyed)"),
			PlayerIndex));
	}

	// Leave the Spectating state, then re-possess. ChangeState(NAME_Playing) tears down the
	// spectator pawn; Possess re-attaches the controller to the original pawn.
	PC->ChangeState(NAME_Playing);
	PC->Possess(OriginalPawn);

	Root->SetStringField(TEXT("repossessed_pawn"), OriginalPawn->GetName());
	Root->SetBoolField(TEXT("spectating"), PC->IsInState(NAME_Spectating));
	return FMonolithActionResult::Success(Root);
}

// =============================================================================
//  pie_inject_key
// =============================================================================

namespace
{
	// Parse an FKey name and validate it against the registered EKeys details.
	bool PieKeyParseKey(const FString& Name, FKey& OutKey, FString& OutError)
	{
		const FString Trimmed = Name.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			OutError = TEXT("key name is empty");
			return false;
		}
		OutKey = FKey(FName(*Trimmed));
		if (!OutKey.IsValid())
		{
			OutError = FString::Printf(
				TEXT("'%s' is not a valid FKey name. Use EKeys names such as SpaceBar, E, LeftShift, LeftMouseButton, Gamepad_FaceButton_Bottom, MouseX, Gamepad_LeftX."),
				*Trimmed);
			return false;
		}
		return true;
	}
}

FMonolithActionResult FMonolithPieInputActions::HandleInjectKey(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* PieWorld = GetPieWorld();
	if (!PieWorld)
	{
		return FMonolithActionResult::Error(TEXT("PIE not running — start Play-In-Editor first"));
	}

	FString KeyName;
	if (!Params->TryGetStringField(TEXT("key"), KeyName) || KeyName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("pie_inject_key requires 'key' (an FKey name, e.g. SpaceBar)"));
	}

	FKey Key;
	FString KeyError;
	if (!PieKeyParseKey(KeyName, Key, KeyError))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("pie_inject_key 'key': %s"), *KeyError));
	}

	FString EventName = TEXT("tap");
	Params->TryGetStringField(TEXT("event"), EventName);
	EventName = EventName.TrimStartAndEnd().ToLower();
	const bool bPressed = EventName == TEXT("pressed") || EventName == TEXT("press");
	const bool bReleased = EventName == TEXT("released") || EventName == TEXT("release");
	const bool bTap = EventName == TEXT("tap");
	const bool bAxis = EventName == TEXT("axis");
	if (!bPressed && !bReleased && !bTap && !bAxis)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("pie_inject_key 'event' must be pressed, released, tap or axis (got '%s')"), *EventName));
	}

	if (Key.IsAxis2D() || Key.IsAxis3D())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Key '%s' is a 2D/3D axis key; inject its 1D component keys instead (e.g. MouseX / MouseY, Gamepad_LeftX / Gamepad_LeftY)."),
			*Key.ToString()));
	}
	if (bAxis && !Key.IsAnalog())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("event=axis needs an analog key; '%s' is digital. Use pressed / released / tap for digital keys."), *Key.ToString()));
	}
	if (!bAxis && Key.IsAnalog())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("'%s' is an analog key; use event=axis with axis_value (or the digital variant of the key, e.g. Gamepad_LeftTrigger instead of Gamepad_LeftTriggerAxis)."),
			*Key.ToString()));
	}

	double AxisValue = 1.0;
	Params->TryGetNumberField(TEXT("axis_value"), AxisValue);

	double HoldSeconds = 0.0;
	Params->TryGetNumberField(TEXT("hold_seconds"), HoldSeconds);
	HoldSeconds = FMath::Clamp(HoldSeconds, 0.0, 60.0);

	int32 PlayerIndex = 0;
	{
		double Idx = 0.0;
		if (Params->TryGetNumberField(TEXT("player_index"), Idx)) { PlayerIndex = static_cast<int32>(Idx); }
	}

	// Modifiers: a JSON array of key names, or a comma-separated string (array params can
	// arrive stringified through some MCP transports).
	TArray<FString> ModifierNames;
	{
		const TArray<TSharedPtr<FJsonValue>>* ModArr = nullptr;
		FString ModString;
		if (Params->TryGetArrayField(TEXT("modifiers"), ModArr) && ModArr)
		{
			for (const TSharedPtr<FJsonValue>& Val : *ModArr)
			{
				FString S;
				if (!Val.IsValid() || !Val->TryGetString(S))
				{
					return FMonolithActionResult::Error(TEXT("pie_inject_key 'modifiers' entries must be key-name strings"));
				}
				ModifierNames.Add(S);
			}
		}
		else if (Params->TryGetStringField(TEXT("modifiers"), ModString) && !ModString.TrimStartAndEnd().IsEmpty())
		{
			FString Body = ModString.TrimStartAndEnd();
			Body.RemoveFromStart(TEXT("["));
			Body.RemoveFromEnd(TEXT("]"));
			Body.ParseIntoArray(ModifierNames, TEXT(","), /*InCullEmpty=*/true);
			for (FString& S : ModifierNames)
			{
				S = S.TrimStartAndEnd().TrimQuotes();
			}
		}
	}

	TArray<FKey> Modifiers;
	for (const FString& Name : ModifierNames)
	{
		FKey Mod;
		FString ModError;
		if (!PieKeyParseKey(Name, Mod, ModError))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("pie_inject_key 'modifiers': %s"), *ModError));
		}
		if (Mod.IsAnalog())
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("pie_inject_key 'modifiers': '%s' is an analog key; modifiers must be digital keys"), *Mod.ToString()));
		}
		if (Mod == Key)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("pie_inject_key 'modifiers': '%s' is the main key; list only the keys held around it"), *Mod.ToString()));
		}
		Modifiers.AddUnique(Mod);
	}

	APlayerController* PC = ResolvePlayerController(PieWorld, PlayerIndex);
	if (!PC)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No player controller at player_index %d in the running PIE world"), PlayerIndex));
	}
	if (!PC->PlayerInput)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Player controller at player_index %d has no PlayerInput (not a local player controller?)"), PlayerIndex));
	}

	// Build the ordered step list.
	const bool bHasModifiers = Modifiers.Num() > 0;
	TArray<FPieKeyStep> Steps;
	auto AddStep = [&Steps](const FKey& InKey, EInputEvent InEvent, float InAmount, int32 InGap, double InSeconds,
		bool bInRepeat = false, float InRepeatAmount = 0.0f)
	{
		FPieKeyStep& Step = Steps.AddDefaulted_GetRef();
		Step.Key = InKey;
		Step.Event = InEvent;
		Step.Amount = InAmount;
		Step.MinFrameGap = InGap;
		Step.MinSecondsSincePrev = InSeconds;
		Step.bRepeatAxisWhileWaiting = bInRepeat;
		Step.RepeatAmount = InRepeatAmount;
	};
	auto AddModifierPresses = [&]()
	{
		for (const FKey& Mod : Modifiers) { AddStep(Mod, IE_Pressed, 1.0f, 0, 0.0); }
	};
	auto AddModifierReleases = [&](int32 FirstGap)
	{
		for (int32 Index = Modifiers.Num() - 1; Index >= 0; --Index)
		{
			AddStep(Modifiers[Index], IE_Released, 0.0f, Index == Modifiers.Num() - 1 ? FirstGap : 0, 0.0);
		}
	};
	const int32 KeyGap = bHasModifiers ? PieKeyOrderedStepFrameGap : 0;

	if (bPressed)
	{
		AddModifierPresses();
		AddStep(Key, IE_Pressed, 1.0f, KeyGap, 0.0);
	}
	else if (bReleased)
	{
		AddStep(Key, IE_Released, 0.0f, 0, 0.0);
		AddModifierReleases(PieKeyOrderedStepFrameGap);
	}
	else if (bTap)
	{
		AddModifierPresses();
		AddStep(Key, IE_Pressed, 1.0f, KeyGap, 0.0);
		AddStep(Key, IE_Released, 0.0f, PieKeyOrderedStepFrameGap, HoldSeconds);
		AddModifierReleases(PieKeyOrderedStepFrameGap);
	}
	else // axis
	{
		AddModifierPresses();
		AddStep(Key, IE_Axis, static_cast<float>(AxisValue), KeyGap, 0.0);
		if (HoldSeconds > 0.0)
		{
			// Re-send the value every frame while held, then zero it.
			AddStep(Key, IE_Axis, 0.0f, PieKeyOrderedStepFrameGap, HoldSeconds, /*bInRepeat=*/true, static_cast<float>(AxisValue));
		}
		if (bHasModifiers)
		{
			AddModifierReleases(PieKeyOrderedStepFrameGap);
		}
	}

	FPieKeySequence Seq;
	Seq.Controller = PC;
	Seq.Steps = MoveTemp(Steps);
	Seq.LastStepFrame = GFrameCounter;
	Seq.LastStepTime = FPlatformTime::Seconds();
	const int32 TotalSteps = Seq.Steps.Num();

	int32 ExecutedNow = 0;
	bool bConsumedNow = false;
	const bool bFinished = PieKeyAdvanceSequence(Seq, ExecutedNow, bConsumedNow);
	if (!bFinished)
	{
		FPieKeySequenceRegistry::Get().Add(MoveTemp(Seq));
	}

	// Report the device / user the events were stamped with, and whether InputKey's
	// platform-user filter could reject them.
	const FInputDeviceId Device = PieKeyResolveInputDevice(PC);
	const FPlatformUserId ControllerUser = PC->GetPlatformUserId();
	const FPlatformUserId DeviceUser = IPlatformInputDeviceMapper::Get().GetUserForInputDevice(Device);
	const bool bFilterByUser = GetDefault<UInputSettings>()->bFilterInputByPlatformUser;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("key"), Key.ToString());
	Root->SetStringField(TEXT("event"), EventName);
	Root->SetNumberField(TEXT("player_index"), PlayerIndex);
	if (bAxis) { Root->SetNumberField(TEXT("axis_value"), AxisValue); }
	Root->SetNumberField(TEXT("hold_seconds"), HoldSeconds);
	TArray<TSharedPtr<FJsonValue>> ModJson;
	for (const FKey& Mod : Modifiers) { ModJson.Add(MakeShared<FJsonValueString>(Mod.ToString())); }
	Root->SetArrayField(TEXT("modifiers"), ModJson);
	Root->SetNumberField(TEXT("input_device_id"), Device.GetId());
	Root->SetNumberField(TEXT("platform_user_id"), ControllerUser.GetInternalId());
	Root->SetNumberField(TEXT("total_steps"), TotalSteps);
	Root->SetNumberField(TEXT("steps_sent_now"), ExecutedNow);
	Root->SetNumberField(TEXT("steps_scheduled"), TotalSteps - ExecutedNow);
	Root->SetBoolField(TEXT("consumed_now"), bConsumedNow);
	Root->SetBoolField(TEXT("completed"), bFinished);
	if (bFilterByUser && DeviceUser != ControllerUser)
	{
		Root->SetStringField(TEXT("warning"),
			TEXT("bFilterInputByPlatformUser is on and the resolved input device does not belong to this controller's platform user; APlayerController::InputKey will drop these events."));
	}
	if (bAxis && HoldSeconds <= 0.0)
	{
		Root->SetStringField(TEXT("note"),
			TEXT("Single axis sample with no auto-zero. Gamepad axes can keep the last value; pass hold_seconds to send a value for a duration and zero it afterwards."));
	}
	return FMonolithActionResult::Success(Root);
}
