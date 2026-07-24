#include "MonolithNiagaraTimingActions.h"
#include "MonolithNiagaraActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithToolRegistry.h"

#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraSimulationStageBase.h"
#include "Editor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithNiagaraTiming, Log, All);

// ============================================================================
//  Local file-static helpers (mirror MonolithNiagaraActions.cpp:686-705 pattern)
// ============================================================================

namespace MonolithNiagaraTimingLocal
{
	static FMonolithActionResult SuccessStr(const FString& Msg)
	{
		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("result"), Msg);
		return FMonolithActionResult::Success(R);
	}

	static FMonolithActionResult SuccessObj(const TSharedRef<FJsonObject>& Obj)
	{
		return FMonolithActionResult::Success(Obj);
	}

	static FString GetAssetPath(const TSharedPtr<FJsonObject>& Params)
	{
		FString Path = Params->GetStringField(TEXT("asset_path"));
		if (Path.IsEmpty()) Path = Params->GetStringField(TEXT("system_path"));
		return Path;
	}

	static UNiagaraSystem* LoadSystem(const FString& SystemPath)
	{
		UNiagaraSystem* System = FMonolithAssetUtils::LoadAssetByPath<UNiagaraSystem>(SystemPath);
		if (!System)
		{
			UE_LOG(LogMonolithNiagaraTiming, Error, TEXT("Failed to load Niagara system: %s"), *SystemPath);
		}
		return System;
	}
}

using namespace MonolithNiagaraTimingLocal;

// ============================================================================
//  Registration
// ============================================================================

void FMonolithNiagaraTimingActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_system_timing"),
		TEXT("**Phase 0 stub.** Read system-level timing fields (warmup, fixed tick delta, require current frame data). Not yet implemented."),
		FMonolithActionHandler::CreateStatic(&HandleGetSystemTiming),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset"))
			.Build());

	Registry.RegisterAction(TEXT("niagara"), TEXT("set_warmup_profile"),
		TEXT("**Phase 0 stub.** Composite write of warmup_time + warmup_tick_delta on a Niagara system. Not yet implemented."),
		FMonolithActionHandler::CreateStatic(&HandleSetWarmupProfile),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset"))
			.Required(TEXT("warmup_time"), TEXT("number"), TEXT("Target warmup time in seconds (snaps to nearest tick_count x tick_delta multiple)"))
			.Optional(TEXT("warmup_tick_delta"), TEXT("number"), TEXT("Tick delta in seconds (default: existing system value, typically 1/15s)"))
			.Build());

	Registry.RegisterAction(TEXT("niagara"), TEXT("set_fixed_tick_delta"),
		TEXT("**Phase 0 stub.** Set bFixedTickDelta + FixedTickDeltaTime on a Niagara system. Not yet implemented."),
		FMonolithActionHandler::CreateStatic(&HandleSetFixedTickDelta),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset"))
			.Required(TEXT("enabled"), TEXT("bool"), TEXT("Enable fixed-tick substepping"))
			.Optional(TEXT("fixed_delta_time"), TEXT("number"), TEXT("Fixed delta time in seconds (only meaningful when enabled=true; default 1/60s if unset)"))
			.Build());

	Registry.RegisterAction(TEXT("niagara"), TEXT("set_require_current_frame_data"),
		TEXT("**Phase 0 stub.** Toggle bRequireCurrentFrameData on a Niagara system. Not yet implemented."),
		FMonolithActionHandler::CreateStatic(&HandleSetRequireCurrentFrameData),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset"))
			.Required(TEXT("require"), TEXT("bool"), TEXT("If true, tick-group dependencies use strict current-frame data; if false, looser previous-frame data permitted"))
			.Build());

	Registry.RegisterAction(TEXT("niagara"), TEXT("set_emitter_loop_profile"),
		TEXT("Composite write of EmitterState loop inputs (LoopBehavior, LoopDuration, LoopDelay, LoopCount, bLoopDelayEnabled, LoopDurationMode). Handles BOTH stateful (UNiagaraEmitter — dispatches per-field through set_module_input_value against module_name=\"EmitterState\") and stateless (UNiagaraStatelessEmitter — direct FProperty reflection on FNiagaraEmitterStateData). Stateless response includes `stateless: true`. `loop_duration_mode` only applies to the stateless branch (stateful path silently ignores it). Non-fatal coherence issues (e.g. loop_count supplied with loop_behavior='Infinite') return a warnings array; the request still succeeds. Provide at least one of the payload fields."),
		FMonolithActionHandler::CreateStatic(&HandleSetEmitterLoopProfile),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset OR standalone UNiagaraStatelessEmitter asset"))
			.Optional(TEXT("emitter"), TEXT("string"), TEXT("Emitter handle id/name (required when asset_path is a UNiagaraSystem; meaningless and ignored when asset_path is a standalone UNiagaraStatelessEmitter)"))
			.Optional(TEXT("loop_behavior"), TEXT("string"), TEXT("ENiagaraLoopBehavior name: 'Once' | 'Multiple' | 'Infinite'"))
			.Optional(TEXT("loop_duration_mode"), TEXT("string"), TEXT("ENiagaraLoopDurationMode name: 'Fixed' | 'Infinite' (stateless-only; ignored by stateful path)"))
			.Optional(TEXT("loop_duration"), TEXT("number"), TEXT("Loop duration in seconds"))
			.Optional(TEXT("loop_delay"), TEXT("number"), TEXT("Delay in seconds before each loop (requires loop_delay_enabled=true to apply)"))
			.Optional(TEXT("loop_count"), TEXT("integer"), TEXT("Number of loops (only meaningful when loop_behavior='Multiple')"))
			.Optional(TEXT("loop_delay_enabled"), TEXT("bool"), TEXT("Enables the LoopDelay input"))
			.Build());

	Registry.RegisterAction(TEXT("niagara"), TEXT("get_emitter_timing_summary"),
		TEXT("Aggregated read of emitter timing: EmitterState loop inputs (behavior/duration/delay/count/delay_enabled), simulation-stage list (name/num_iterations/execute_behavior), and Initialize Particle Lifetime min/max. Returns null for any field that cannot be read (missing module, missing input) — partial data is acceptable. Omit 'emitter' to aggregate across all emitters."),
		FMonolithActionHandler::CreateStatic(&HandleGetEmitterTimingSummary),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset"))
			.Optional(TEXT("emitter"), TEXT("string"), TEXT("Emitter handle id/name (omit to aggregate all emitters)"))
			.Build());

	Registry.RegisterAction(TEXT("niagara"), TEXT("set_sim_stage_iteration_count"),
		TEXT("Alias for set_simulation_stage_property(property=\"NumIterations\"). Sets NumIterations on a UNiagaraSimulationStageGeneric (FNiagaraParameterBindingWithValue — written via reflection ImportText_Direct). Provide exactly one of stage_index / stage_name."),
		FMonolithActionHandler::CreateStatic(&HandleSetSimStageIterationCount),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter handle id/name (same convention as set_simulation_stage_property)"))
			.Optional(TEXT("stage_index"), TEXT("integer"), TEXT("0-based index into the emitter's simulation stages (use this OR stage_name)"))
			.Optional(TEXT("stage_name"), TEXT("string"), TEXT("Simulation stage name (use this OR stage_index)"))
			.Required(TEXT("iterations"), TEXT("integer"), TEXT("Iteration count to write to NumIterations"))
			.Build());

	Registry.RegisterAction(TEXT("niagara"), TEXT("set_sim_stage_execute_behavior"),
		TEXT("Alias for set_simulation_stage_property(property=\"ExecuteBehavior\"). Sets ExecuteBehavior on a UNiagaraSimulationStageGeneric. Provide exactly one of stage_index / stage_name."),
		FMonolithActionHandler::CreateStatic(&HandleSetSimStageExecuteBehavior),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter handle id/name (same convention as set_simulation_stage_property)"))
			.Optional(TEXT("stage_index"), TEXT("integer"), TEXT("0-based index into the emitter's simulation stages (use this OR stage_name)"))
			.Optional(TEXT("stage_name"), TEXT("string"), TEXT("Simulation stage name (use this OR stage_index)"))
			.Required(TEXT("behavior"), TEXT("string"), TEXT("ENiagaraSimStageExecuteBehavior name: 'Always' | 'OnSimulationReset' | 'NotOnSimulationReset' (snake_case also accepted)"))
			.Build());

	Registry.RegisterAction(TEXT("niagara"), TEXT("set_particle_lifetime"),
		TEXT("Set particle Lifetime on the InitializeParticle module of a stateful emitter's particle_spawn script. If only 'min' supplied -> Lifetime Mode = 'Direct', writes the 'Lifetime' input. If both 'min' and 'max' supplied -> Lifetime Mode = 'Random', writes 'Lifetime Min' + 'Lifetime Max'. Dispatches through niagara::set_static_switch_value + niagara::set_module_input_value against module_node='InitializeParticle'. Returns a clean error with a hint if the InitializeParticle module is absent from the emitter."),
		FMonolithActionHandler::CreateStatic(&HandleSetParticleLifetime),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter handle id/name (same convention as set_module_input_value)"))
			.Required(TEXT("min"), TEXT("number"), TEXT("Lifetime in seconds (used as 'Lifetime' for Direct mode, or as 'Lifetime Min' floor for Random mode)"))
			.Optional(TEXT("max"), TEXT("number"), TEXT("Lifetime range ceiling in seconds; when supplied, switches Lifetime Mode to 'Random' and writes 'Lifetime Max'"))
			.Build());
}

// ============================================================================
//  Phase 1 — System-level handlers (real implementations)
// ============================================================================

FMonolithActionResult FMonolithNiagaraTimingActions::HandleGetSystemTiming(const TSharedPtr<FJsonObject>& Params)
{
	const FString SystemPath = GetAssetPath(Params);
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("asset_path"), SystemPath);
	R->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
	R->SetNumberField(TEXT("warmup_tick_count"), System->GetWarmupTickCount());
	R->SetNumberField(TEXT("warmup_tick_delta"), System->GetWarmupTickDelta());
	R->SetBoolField(TEXT("fixed_tick_delta_enabled"), System->HasFixedTickDelta());
	R->SetNumberField(TEXT("fixed_tick_delta_time"), System->GetFixedTickDeltaTime());

	// bRequireCurrentFrameData is a uint8:1 bitfield with no inline getter — reflect it.
	bool bRequireCurrent = false;
	if (FBoolProperty* BP = FindFProperty<FBoolProperty>(UNiagaraSystem::StaticClass(), TEXT("bRequireCurrentFrameData")))
	{
		bRequireCurrent = BP->GetPropertyValue_InContainer(System);
	}
	R->SetBoolField(TEXT("require_current_frame_data"), bRequireCurrent);

	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraTimingActions::HandleSetWarmupProfile(const TSharedPtr<FJsonObject>& Params)
{
	const FString SystemPath = GetAssetPath(Params);

	// warmup_time required (per plan § Phase 1 spec)
	TSharedPtr<FJsonValue> WarmupTimeJV = Params->TryGetField(TEXT("warmup_time"));
	if (!WarmupTimeJV.IsValid() || WarmupTimeJV->Type != EJson::Number)
		return FMonolithActionResult::Error(TEXT("Missing required field: warmup_time (number)"));
	const float WarmupTimeIn = static_cast<float>(WarmupTimeJV->AsNumber());

	// warmup_tick_delta optional
	bool bHasTickDelta = false;
	float TickDeltaIn = 0.0f;
	TSharedPtr<FJsonValue> TickDeltaJV = Params->TryGetField(TEXT("warmup_tick_delta"));
	if (TickDeltaJV.IsValid() && TickDeltaJV->Type == EJson::Number)
	{
		TickDeltaIn = static_cast<float>(TickDeltaJV->AsNumber());
		bHasTickDelta = true;
	}

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetWarmupProfile", "Set Niagara Warmup Profile"));
	System->Modify();

	// SetWarmupTickDelta first so SetWarmupTime resolves count against the new delta
	// (both setters internally call ResolveWarmupTickCount).
	if (bHasTickDelta)
	{
		System->SetWarmupTickDelta(TickDeltaIn);
	}
	System->SetWarmupTime(WarmupTimeIn);

	// Fire PostEditChangeProperty for each touched UPROPERTY so the editor/graph dirties correctly.
	if (FProperty* TimeProp = FindFProperty<FProperty>(UNiagaraSystem::StaticClass(), TEXT("WarmupTime")))
	{
		FPropertyChangedEvent PCE(TimeProp);
		System->PostEditChangeProperty(PCE);
	}
	if (bHasTickDelta)
	{
		if (FProperty* DeltaProp = FindFProperty<FProperty>(UNiagaraSystem::StaticClass(), TEXT("WarmupTickDelta")))
		{
			FPropertyChangedEvent PCE(DeltaProp);
			System->PostEditChangeProperty(PCE);
		}
	}

	System->RequestCompile(false);
	GEditor->EndTransaction();

	// Response: resolved triple so callers observe the snap (WarmupTime rounds to TickCount * TickDelta).
	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("asset_path"), SystemPath);
	R->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
	R->SetNumberField(TEXT("warmup_tick_count"), System->GetWarmupTickCount());
	R->SetNumberField(TEXT("warmup_tick_delta"), System->GetWarmupTickDelta());
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraTimingActions::HandleSetFixedTickDelta(const TSharedPtr<FJsonObject>& Params)
{
	const FString SystemPath = GetAssetPath(Params);

	// enabled required (bool)
	TSharedPtr<FJsonValue> EnabledJV = Params->TryGetField(TEXT("enabled"));
	if (!EnabledJV.IsValid() || EnabledJV->Type != EJson::Boolean)
		return FMonolithActionResult::Error(TEXT("Missing required field: enabled (bool)"));
	const bool bEnabled = EnabledJV->AsBool();

	// fixed_delta_time optional (number)
	bool bHasDeltaTime = false;
	float FixedDeltaIn = 0.0f;
	TSharedPtr<FJsonValue> DeltaJV = Params->TryGetField(TEXT("fixed_delta_time"));
	if (DeltaJV.IsValid() && DeltaJV->Type == EJson::Number)
	{
		FixedDeltaIn = static_cast<float>(DeltaJV->AsNumber());
		bHasDeltaTime = true;
	}

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	FBoolProperty* EnabledProp = FindFProperty<FBoolProperty>(UNiagaraSystem::StaticClass(), TEXT("bFixedTickDelta"));
	if (!EnabledProp)
		return FMonolithActionResult::Error(TEXT("UPROPERTY 'bFixedTickDelta' not found on UNiagaraSystem"));
	FFloatProperty* DeltaProp = nullptr;
	if (bHasDeltaTime)
	{
		DeltaProp = FindFProperty<FFloatProperty>(UNiagaraSystem::StaticClass(), TEXT("FixedTickDeltaTime"));
		if (!DeltaProp)
			return FMonolithActionResult::Error(TEXT("UPROPERTY 'FixedTickDeltaTime' not found on UNiagaraSystem"));
	}

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetFixedTickDelta", "Set Niagara Fixed Tick Delta"));
	System->Modify();

	EnabledProp->SetPropertyValue_InContainer(System, bEnabled);
	if (DeltaProp)
	{
		DeltaProp->SetPropertyValue_InContainer(System, FixedDeltaIn);
	}

	{
		FPropertyChangedEvent PCE(EnabledProp);
		System->PostEditChangeProperty(PCE);
	}
	if (DeltaProp)
	{
		FPropertyChangedEvent PCE(DeltaProp);
		System->PostEditChangeProperty(PCE);
	}

	System->RequestCompile(false);
	GEditor->EndTransaction();

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("asset_path"), SystemPath);
	R->SetBoolField(TEXT("fixed_tick_delta_enabled"), System->HasFixedTickDelta());
	R->SetNumberField(TEXT("fixed_tick_delta_time"), System->GetFixedTickDeltaTime());
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraTimingActions::HandleSetRequireCurrentFrameData(const TSharedPtr<FJsonObject>& Params)
{
	const FString SystemPath = GetAssetPath(Params);

	TSharedPtr<FJsonValue> RequireJV = Params->TryGetField(TEXT("require"));
	if (!RequireJV.IsValid() || RequireJV->Type != EJson::Boolean)
		return FMonolithActionResult::Error(TEXT("Missing required field: require (bool)"));
	const bool bRequire = RequireJV->AsBool();

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	FBoolProperty* Prop = FindFProperty<FBoolProperty>(UNiagaraSystem::StaticClass(), TEXT("bRequireCurrentFrameData"));
	if (!Prop)
		return FMonolithActionResult::Error(TEXT("UPROPERTY 'bRequireCurrentFrameData' not found on UNiagaraSystem"));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetRequireCurrentFrameData", "Set Niagara Require Current Frame Data"));
	System->Modify();

	Prop->SetPropertyValue_InContainer(System, bRequire);

	{
		FPropertyChangedEvent PCE(Prop);
		System->PostEditChangeProperty(PCE);
	}

	System->RequestCompile(false);
	GEditor->EndTransaction();

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("asset_path"), SystemPath);
	R->SetBoolField(TEXT("require_current_frame_data"), Prop->GetPropertyValue_InContainer(System));
	return SuccessObj(R);
}

// ============================================================================
//  Phase 3 — EmitterState routing helpers + composite write + read aggregator
// ============================================================================
//
// Strategy: dispatch through niagara::set_module_input_value (the same registry
// path Phase 2 uses for sim-stage aliases). The canonical handler at
// MonolithNiagaraActions.cpp:3506 accepts module_name="EmitterState" through
// FindModuleNode's name-fallback (MonolithNiagaraActions.cpp:1481 —
// `N->GetFunctionName() == NodeGuidStr`), so we do NOT need a private
// FindEmitterStateModule walker. Single source of truth, no duplicated
// override-pin / type-resolution / DefaultValue-set machinery.
//
// Stateless-emitter detection: FNiagaraEmitterHandle stores stateless emitters
// in a separate StatelessEmitter slot (NiagaraEmitterHandle.h:91 +174),
// accessible via Handle.GetStatelessEmitter(). The forward-declared return
// type means a pointer null-check needs no Internal/Stateless/ header include
// (hazard §7 of the plan — sidestepped).
//

namespace MonolithNiagaraTimingLocal
{
	// EmitterState canonical module name (Q2 of the design spec — accepted risk
	// of 5.8+ rename, one-line code-change recovery).
	static const TCHAR* const EmitterStateModuleName = TEXT("EmitterState");

	// Phase 4 generalisation: helpers take a ModuleName parameter so the same
	// dispatch surface serves EmitterState, InitializeParticle, and future modules.
	// Phase 3 call sites pass EmitterStateModuleName explicitly.

	// Build the JSON envelope set_module_input_value expects:
	//   { asset_path, emitter, module_node: <ModuleName>, input, value }
	// and dispatch via the public registry. Mirrors the Phase 2 sim-stage
	// alias pattern exactly.
	static FMonolithActionResult DispatchModuleInput(
		const FString& SystemPath,
		const FString& Emitter,
		const FString& ModuleName,
		const FString& InputName,
		const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
			return FMonolithActionResult::Error(TEXT("Internal: module-input alias passed null value"));

		TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetStringField(TEXT("asset_path"), SystemPath);
		Envelope->SetStringField(TEXT("emitter"), Emitter);
		Envelope->SetStringField(TEXT("module_node"), ModuleName);
		Envelope->SetStringField(TEXT("input"), InputName);
		Envelope->SetField(TEXT("value"), Value);

		return FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("niagara"), TEXT("set_module_input_value"), Envelope);
	}

	// Read companion — dispatch get_module_input_value for the named module.
	// Returns the inner Result JSON if successful (which carries the `value`
	// string from the override-pin DefaultValue), or nullptr on miss.
	// Aggregator policy = partial data acceptable, so the caller should null-
	// out the field on miss rather than fail the whole action.
	static TSharedPtr<FJsonObject> DispatchGetModuleInput(
		const FString& SystemPath,
		const FString& Emitter,
		const FString& ModuleName,
		const FString& InputName)
	{
		TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetStringField(TEXT("asset_path"), SystemPath);
		Envelope->SetStringField(TEXT("emitter"), Emitter);
		Envelope->SetStringField(TEXT("module_node"), ModuleName);
		Envelope->SetStringField(TEXT("input"), InputName);

		FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("niagara"), TEXT("get_module_input_value"), Envelope);
		if (R.bSuccess && R.Result.IsValid())
		{
			return R.Result;
		}
		return nullptr;
	}

	// Static-switch write companion — dispatches niagara::set_static_switch_value
	// against the named module. Values are passed as raw strings (the upstream
	// handler accepts enum-name passthrough and "true"/"false" for NiagaraBool).
	static FMonolithActionResult DispatchModuleSwitch(
		const FString& SystemPath,
		const FString& Emitter,
		const FString& ModuleName,
		const FString& SwitchName,
		const FString& ValueStr)
	{
		TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetStringField(TEXT("asset_path"), SystemPath);
		Envelope->SetStringField(TEXT("emitter"), Emitter);
		Envelope->SetStringField(TEXT("module_node"), ModuleName);
		Envelope->SetStringField(TEXT("input"), SwitchName);
		Envelope->SetStringField(TEXT("value"), ValueStr);

		return FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("niagara"), TEXT("set_static_switch_value"), Envelope);
	}

	// Static-switch read companion. Returns the inner Result JSON or nullptr on miss.
	static TSharedPtr<FJsonObject> DispatchGetModuleSwitch(
		const FString& SystemPath,
		const FString& Emitter,
		const FString& ModuleName,
		const FString& SwitchName)
	{
		TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetStringField(TEXT("asset_path"), SystemPath);
		Envelope->SetStringField(TEXT("emitter"), Emitter);
		Envelope->SetStringField(TEXT("module_node"), ModuleName);
		Envelope->SetStringField(TEXT("input"), SwitchName);

		FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("niagara"), TEXT("get_static_switch_value"), Envelope);
		if (R.bSuccess && R.Result.IsValid())
		{
			return R.Result;
		}
		return nullptr;
	}

	// InitializeParticle canonical module-node name (verified via PIE 2026-05-28).
	static const TCHAR* const InitializeParticleModuleName = TEXT("InitializeParticle");

	// Convert a JSON loop_behavior string (Once/Multiple/Infinite, case-insensitive)
	// to its canonical enum-name form for ImportText. Returns empty on miss.
	static FString CanonicalizeLoopBehavior(const FString& Input)
	{
		const FString Lower = Input.ToLower();
		if (Lower == TEXT("once"))     return TEXT("Once");
		if (Lower == TEXT("multiple")) return TEXT("Multiple");
		if (Lower == TEXT("infinite")) return TEXT("Infinite");
		return FString();
	}

	// Pull a string `value` field out of a get_module_input_value response.
	// HandleGetModuleInputValue (MonolithNiagaraActions.cpp:6991-7022) writes
	// the override-pin DefaultValue (or "(default)" sentinel) into `value`.
	static bool TryGetSummaryString(const TSharedPtr<FJsonObject>& InputResp, FString& OutValue)
	{
		if (!InputResp.IsValid()) return false;
		const FString Raw = InputResp->GetStringField(TEXT("value"));
		if (Raw.IsEmpty() || Raw == TEXT("(default)")) return false;
		OutValue = Raw;
		return true;
	}

	// Same, but as a double (parse helper for numeric inputs).
	static bool TryGetSummaryDouble(const TSharedPtr<FJsonObject>& InputResp, double& OutValue)
	{
		FString S;
		if (!TryGetSummaryString(InputResp, S)) return false;
		OutValue = FCString::Atod(*S);
		return true;
	}

	static bool TryGetSummaryInt(const TSharedPtr<FJsonObject>& InputResp, int32& OutValue)
	{
		FString S;
		if (!TryGetSummaryString(InputResp, S)) return false;
		OutValue = FCString::Atoi(*S);
		return true;
	}

	static bool TryGetSummaryBool(const TSharedPtr<FJsonObject>& InputResp, bool& OutValue)
	{
		FString S;
		if (!TryGetSummaryString(InputResp, S)) return false;
		OutValue = S.ToBool();
		return true;
	}

	// Canonicalize ENiagaraLoopDurationMode string (Fixed | Infinite, case-insensitive).
	// Returns empty on miss.
	static FString CanonicalizeLoopDurationMode(const FString& Input)
	{
		const FString Lower = Input.ToLower();
		if (Lower == TEXT("fixed"))    return TEXT("Fixed");
		if (Lower == TEXT("infinite")) return TEXT("Infinite");
		return FString();
	}

	// Phase 2 — stateless loop-profile write.
	//
	// Cannot include "Stateless/NiagaraStatelessEmitter.h" (Internal/, not on the
	// MonolithNiagara include path). Access strategy:
	//   - Caller obtains the stateless emitter as UNiagaraEmitterBase* via
	//     FNiagaraEmitterHandle::GetEmitterBase() (NiagaraEmitterBase.h is public).
	//   - All UPROPERTY access on UNiagaraStatelessEmitter::EmitterState
	//     (FNiagaraEmitterStateData, protected member) routes through
	//     FindFProperty + ContainerPtrToValuePtr.
	//   - FNiagaraDistributionRangeFloat literal format verified Phase 2 first-task:
	//     it inherits FNiagaraDistributionBase (Mode + various) and adds two scalar
	//     UPROPERTYs (Min, Max). Single-value write = (Min=N,Max=N).
	//   - Plain PostEditChangeProperty per gotcha hazard #1 (un-versioned class).
	static FMonolithActionResult WriteStatelessLoopProfile(
		UObject* StatelessEmitter,
		const TSharedPtr<FJsonObject>& Params,
		TArray<TSharedPtr<FJsonValue>>& OutWarnings)
	{
		if (!StatelessEmitter)
			return FMonolithActionResult::Error(TEXT("WriteStatelessLoopProfile: null stateless emitter"));

		FStructProperty* StateProp = FindFProperty<FStructProperty>(
			StatelessEmitter->GetClass(), TEXT("EmitterState"));
		if (!StateProp || !StateProp->Struct)
			return FMonolithActionResult::Error(TEXT(
				"UPROPERTY 'EmitterState' (FNiagaraEmitterStateData) not found on UNiagaraStatelessEmitter"));

		void* StateData = StateProp->ContainerPtrToValuePtr<void>(StatelessEmitter);
		UScriptStruct* StateStruct = StateProp->Struct;

		// Collect optional payload fields.
		const TSharedPtr<FJsonValue> BehaviorJV       = Params->TryGetField(TEXT("loop_behavior"));
		const TSharedPtr<FJsonValue> DurationModeJV   = Params->TryGetField(TEXT("loop_duration_mode"));
		const TSharedPtr<FJsonValue> DurationJV       = Params->TryGetField(TEXT("loop_duration"));
		const TSharedPtr<FJsonValue> DelayJV          = Params->TryGetField(TEXT("loop_delay"));
		const TSharedPtr<FJsonValue> CountJV          = Params->TryGetField(TEXT("loop_count"));
		const TSharedPtr<FJsonValue> DelayEnabledJV   = Params->TryGetField(TEXT("loop_delay_enabled"));

		// Canonicalize enum strings up-front so we can early-reject before mutation.
		FString BehaviorCanonical;
		if (BehaviorJV.IsValid() && BehaviorJV->Type == EJson::String)
		{
			BehaviorCanonical = CanonicalizeLoopBehavior(BehaviorJV->AsString());
			if (BehaviorCanonical.IsEmpty())
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Unknown loop_behavior '%s'. Valid: 'Once' | 'Multiple' | 'Infinite'"),
					*BehaviorJV->AsString()));
			}
		}
		FString DurationModeCanonical;
		if (DurationModeJV.IsValid() && DurationModeJV->Type == EJson::String)
		{
			DurationModeCanonical = CanonicalizeLoopDurationMode(DurationModeJV->AsString());
			if (DurationModeCanonical.IsEmpty())
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Unknown loop_duration_mode '%s'. Valid: 'Fixed' | 'Infinite'"),
					*DurationModeJV->AsString()));
			}
		}

		// Coherence warnings — mirror the stateful path's policy (non-fatal).
		if (CountJV.IsValid() && CountJV->Type == EJson::Number)
		{
			const bool bBehaviorIsMultiple = (BehaviorCanonical == TEXT("Multiple"));
			if (BehaviorJV.IsValid() && !bBehaviorIsMultiple)
			{
				OutWarnings.Add(MakeShared<FJsonValueString>(FString::Printf(
					TEXT("loop_count supplied with loop_behavior='%s' — LoopCount only applies when behavior is 'Multiple'."),
					*BehaviorCanonical)));
			}
		}
		if (DelayJV.IsValid() && DelayJV->Type == EJson::Number)
		{
			const bool bDelayExplicitlyEnabled = DelayEnabledJV.IsValid() &&
				DelayEnabledJV->Type == EJson::Boolean && DelayEnabledJV->AsBool();
			if (!bDelayExplicitlyEnabled)
			{
				OutWarnings.Add(MakeShared<FJsonValueString>(TEXT(
					"loop_delay supplied without loop_delay_enabled=true — the LoopDelay value will be set but bLoopDelayEnabled may suppress it.")));
			}
		}

		// Re-init owning system on scope exit. SetDestroyOnAdd(true) per gotcha hazard #2
		// (stateless has no script — Add+destructor pattern is the re-init mechanism).
		FNiagaraSystemUpdateContext UpdateContext;
		UpdateContext.SetDestroyOnAdd(true);
		if (UNiagaraSystem* OwnerSystem = StatelessEmitter->GetTypedOuter<UNiagaraSystem>())
		{
			UpdateContext.Add(OwnerSystem, true);
		}

		StatelessEmitter->Modify();

		// Hoist a single PreEditChange on the OUTER FStructProperty (owned by the
		// UNiagaraStatelessEmitter UClass). The inner FNiagaraEmitterStateData fields
		// are owned by a UScriptStruct, NOT a UClass — invoking PostEditChangeProperty
		// on them trips GetOwner<UClass>() assert in UE 5.7 UnrealType.h:754. Standard
		// UE pattern is to notify on the struct property, not on its inner fields.
		StatelessEmitter->PreEditChange(StateProp);

		auto FireEditChange = [](FProperty* /*Prop*/)
		{
			// Intentionally no-op. Per-field PreEditChange/PostEditChangeProperty
			// would assert because inner properties are UScriptStruct-owned.
			// Single PostEditChangeProperty on the outer StateProp fires after all
			// writes complete (see end of this function).
		};

		auto WriteRangeFloatField = [&](const TCHAR* FieldName, double Value, const TCHAR* JsonName) -> bool
		{
			FStructProperty* Prop = FindFProperty<FStructProperty>(StateStruct, FieldName);
			if (!Prop)
			{
				OutWarnings.Add(MakeShared<FJsonValueString>(FString::Printf(
					TEXT("UPROPERTY '%s' not found on FNiagaraEmitterStateData — skipped."),
					FieldName)));
				return false;
			}
			void* RangeData = Prop->ContainerPtrToValuePtr<void>(StateData);
			// Single-value -> (Min=N,Max=N). Verified Phase 2 first-task: struct layout
			// is `float Min` + `float Max` in FNiagaraDistributionRangeFloat, plus
			// inherited Mode/etc. from FNiagaraDistributionBase (defaults are fine).
			const FString Literal = FString::Printf(TEXT("(Min=%f,Max=%f)"), Value, Value);
			const TCHAR* Result = Prop->ImportText_Direct(*Literal, RangeData, StatelessEmitter, PPF_None, GError);
			if (!Result)
			{
				OutWarnings.Add(MakeShared<FJsonValueString>(FString::Printf(
					TEXT("Failed to ImportText_Direct for %s (literal '%s')."), JsonName, *Literal)));
				return false;
			}
			FireEditChange(Prop);
			return true;
		};

		// LoopBehavior — UByteProperty (enum) — ImportText_Direct with canonical name.
		if (!BehaviorCanonical.IsEmpty())
		{
			FProperty* Prop = StateStruct->FindPropertyByName(TEXT("LoopBehavior"));
			if (!Prop)
			{
				OutWarnings.Add(MakeShared<FJsonValueString>(TEXT(
					"UPROPERTY 'LoopBehavior' not found on FNiagaraEmitterStateData — skipped.")));
			}
			else
			{
				void* FieldData = Prop->ContainerPtrToValuePtr<void>(StateData);
				const TCHAR* R = Prop->ImportText_Direct(*BehaviorCanonical, FieldData, StatelessEmitter, PPF_None, GError);
				if (!R)
				{
					OutWarnings.Add(MakeShared<FJsonValueString>(FString::Printf(
						TEXT("Failed to ImportText_Direct LoopBehavior='%s'."), *BehaviorCanonical)));
				}
				else
				{
					FireEditChange(Prop);
				}
			}
		}

		// LoopDurationMode — enum, same pattern.
		if (!DurationModeCanonical.IsEmpty())
		{
			FProperty* Prop = StateStruct->FindPropertyByName(TEXT("LoopDurationMode"));
			if (!Prop)
			{
				OutWarnings.Add(MakeShared<FJsonValueString>(TEXT(
					"UPROPERTY 'LoopDurationMode' not found on FNiagaraEmitterStateData — skipped.")));
			}
			else
			{
				void* FieldData = Prop->ContainerPtrToValuePtr<void>(StateData);
				const TCHAR* R = Prop->ImportText_Direct(*DurationModeCanonical, FieldData, StatelessEmitter, PPF_None, GError);
				if (!R)
				{
					OutWarnings.Add(MakeShared<FJsonValueString>(FString::Printf(
						TEXT("Failed to ImportText_Direct LoopDurationMode='%s'."), *DurationModeCanonical)));
				}
				else
				{
					FireEditChange(Prop);
				}
			}
		}

		// LoopCount — FIntProperty, plain SetPropertyValue_InContainer.
		if (CountJV.IsValid() && CountJV->Type == EJson::Number)
		{
			FIntProperty* Prop = FindFProperty<FIntProperty>(StateStruct, TEXT("LoopCount"));
			if (!Prop)
			{
				OutWarnings.Add(MakeShared<FJsonValueString>(TEXT(
					"UPROPERTY 'LoopCount' not found on FNiagaraEmitterStateData — skipped.")));
			}
			else
			{
				Prop->SetPropertyValue_InContainer(StateData, static_cast<int32>(CountJV->AsNumber()));
				FireEditChange(Prop);
			}
		}

		// LoopDuration / LoopDelay — FNiagaraDistributionRangeFloat structs.
		if (DurationJV.IsValid() && DurationJV->Type == EJson::Number)
		{
			WriteRangeFloatField(TEXT("LoopDuration"), DurationJV->AsNumber(), TEXT("loop_duration"));
		}
		if (DelayJV.IsValid() && DelayJV->Type == EJson::Number)
		{
			WriteRangeFloatField(TEXT("LoopDelay"), DelayJV->AsNumber(), TEXT("loop_delay"));
		}

		// bLoopDelayEnabled — FBoolProperty (uint32:1 bitfield), SetPropertyValue_InContainer.
		if (DelayEnabledJV.IsValid() && DelayEnabledJV->Type == EJson::Boolean)
		{
			FBoolProperty* Prop = FindFProperty<FBoolProperty>(StateStruct, TEXT("bLoopDelayEnabled"));
			if (!Prop)
			{
				OutWarnings.Add(MakeShared<FJsonValueString>(TEXT(
					"UPROPERTY 'bLoopDelayEnabled' not found on FNiagaraEmitterStateData — skipped.")));
			}
			else
			{
				Prop->SetPropertyValue_InContainer(StateData, DelayEnabledJV->AsBool());
				FireEditChange(Prop);
			}
		}

		// Single PostEditChangeProperty on the outer FStructProperty (UClass-owned).
		// Mirrors the hoisted PreEditChange above. EPropertyChangeType::ValueSet
		// matches what per-field SetValue paths would have fired.
		{
			FPropertyChangedEvent PCE(StateProp, EPropertyChangeType::ValueSet);
			StatelessEmitter->PostEditChangeProperty(PCE);
		}

		// UpdateContext destructor at scope exit triggers system re-init.
		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField(TEXT("success"), true);
		Resp->SetStringField(TEXT("asset_path"), GetAssetPath(Params));
		Resp->SetStringField(TEXT("emitter"), Params->GetStringField(TEXT("emitter")));
		Resp->SetBoolField(TEXT("stateless"), true);
		Resp->SetArrayField(TEXT("warnings"), OutWarnings);
		return FMonolithActionResult::Success(Resp);
	}

	// Phase 3 — stateless loop-profile read. Mirror of WriteStatelessLoopProfile
	// (lines 536-767 of this file). Returns the per-emitter JSON object the
	// aggregator emits; caller chooses whether to embed in a single-element
	// `emitters` array (standalone branch) or append to the system loop.
	//
	// Access strategy identical to the write helper:
	//   - StatelessEmitter is a bare UObject* (concrete UNiagaraStatelessEmitter
	//     header lives in Internal/, not on our include path).
	//   - All field access via FindFProperty + ContainerPtrToValuePtr.
	//   - FNiagaraDistributionRangeFloat.Min is the canonical scalar (the write
	//     helper writes (Min=N,Max=N), so for our writes Min == Max).
	//   - lifetime fields are NOT in scope for Phase 3 — they live on a
	//     different stateless surface (modules) and are deferred. Return null.
	static TSharedPtr<FJsonObject> ReadStatelessLoopProfile(UObject* StatelessEmitter)
	{
		TSharedRef<FJsonObject> EObj = MakeShared<FJsonObject>();
		EObj->SetStringField(TEXT("name"), StatelessEmitter ? StatelessEmitter->GetName() : FString());
		EObj->SetNumberField(TEXT("index"), 0);
		EObj->SetBoolField(TEXT("stateless"), true);

		// Helpers for nullable surfaces (mirror aggregator shape).
		auto SetNull = [&EObj](const TCHAR* Field)
		{
			EObj->SetField(Field, MakeShared<FJsonValueNull>());
		};

		// Stateless emitters have no sim stages by design — explicit empty array.
		EObj->SetArrayField(TEXT("sim_stages"), TArray<TSharedPtr<FJsonValue>>());

		// Lifetime surfaces deferred (different module-level path, not in Phase 3 scope).
		SetNull(TEXT("lifetime"));
		SetNull(TEXT("lifetime_mode"));
		SetNull(TEXT("lifetime_min"));
		SetNull(TEXT("lifetime_max"));

		if (!StatelessEmitter)
		{
			SetNull(TEXT("loop_behavior"));
			SetNull(TEXT("loop_count"));
			SetNull(TEXT("loop_duration_mode"));
			SetNull(TEXT("loop_duration"));
			SetNull(TEXT("loop_delay"));
			SetNull(TEXT("loop_delay_enabled"));
			return EObj;
		}

		FStructProperty* StateProp = FindFProperty<FStructProperty>(
			StatelessEmitter->GetClass(), TEXT("EmitterState"));
		if (!StateProp || !StateProp->Struct)
		{
			SetNull(TEXT("loop_behavior"));
			SetNull(TEXT("loop_count"));
			SetNull(TEXT("loop_duration_mode"));
			SetNull(TEXT("loop_duration"));
			SetNull(TEXT("loop_delay"));
			SetNull(TEXT("loop_delay_enabled"));
			return EObj;
		}

		const void* StateData = StateProp->ContainerPtrToValuePtr<void>(StatelessEmitter);
		UScriptStruct* StateStruct = StateProp->Struct;

		// Enum reader — ExportTextItem_Direct emits the enum element name; we
		// strip the "EnumType::" prefix if the serializer included it so the
		// surface ("Multiple", "Fixed") matches what the write helper accepts.
		auto ReadEnumField = [&](const TCHAR* FieldName, const TCHAR* JsonField)
		{
			FProperty* Prop = StateStruct->FindPropertyByName(FieldName);
			if (!Prop) { SetNull(JsonField); return; }
			FString ValStr;
			Prop->ExportTextItem_Direct(
				ValStr,
				Prop->ContainerPtrToValuePtr<void>(StateData),
				nullptr, StatelessEmitter, PPF_None);
			int32 ColonIdx = INDEX_NONE;
			if (ValStr.FindLastChar(TEXT(':'), ColonIdx) && ColonIdx + 1 < ValStr.Len())
			{
				ValStr = ValStr.Mid(ColonIdx + 1);
			}
			EObj->SetStringField(JsonField, ValStr);
		};

		ReadEnumField(TEXT("LoopBehavior"),     TEXT("loop_behavior"));
		ReadEnumField(TEXT("LoopDurationMode"), TEXT("loop_duration_mode"));

		// LoopCount — FIntProperty.
		if (FIntProperty* CountProp = FindFProperty<FIntProperty>(StateStruct, TEXT("LoopCount")))
		{
			EObj->SetNumberField(TEXT("loop_count"),
				CountProp->GetPropertyValue_InContainer(StateData));
		}
		else { SetNull(TEXT("loop_count")); }

		// LoopDuration / LoopDelay — FNiagaraDistributionRangeFloat. Read Min as
		// the canonical scalar (write helper sets Min == Max for constant writes).
		auto ReadRangeFloatField = [&](const TCHAR* FieldName, const TCHAR* JsonField)
		{
			FStructProperty* RangeProp = FindFProperty<FStructProperty>(StateStruct, FieldName);
			if (!RangeProp || !RangeProp->Struct) { SetNull(JsonField); return; }
			const void* RangeData = RangeProp->ContainerPtrToValuePtr<void>(StateData);
			FFloatProperty* MinProp = FindFProperty<FFloatProperty>(RangeProp->Struct, TEXT("Min"));
			if (!MinProp) { SetNull(JsonField); return; }
			EObj->SetNumberField(JsonField, MinProp->GetPropertyValue_InContainer(RangeData));
		};
		ReadRangeFloatField(TEXT("LoopDuration"), TEXT("loop_duration"));
		ReadRangeFloatField(TEXT("LoopDelay"),    TEXT("loop_delay"));

		// bLoopDelayEnabled — FBoolProperty (uint32:1 bitfield).
		if (FBoolProperty* DelayEnabledProp = FindFProperty<FBoolProperty>(StateStruct, TEXT("bLoopDelayEnabled")))
		{
			EObj->SetBoolField(TEXT("loop_delay_enabled"),
				DelayEnabledProp->GetPropertyValue_InContainer(StateData));
		}
		else { SetNull(TEXT("loop_delay_enabled")); }

		return EObj;
	}
} // namespace MonolithNiagaraTimingLocal

FMonolithActionResult FMonolithNiagaraTimingActions::HandleSetEmitterLoopProfile(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithNiagaraTimingLocal;

	const FString SystemPath = GetAssetPath(Params);
	if (SystemPath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required field: asset_path"));

	// Emitter is optional NOW — only required for system-asset path. Validated below.
	FString Emitter;
	Params->TryGetStringField(TEXT("emitter"), Emitter);

	// Collect optional payload fields up-front so we can early-reject "no fields supplied".
	const TSharedPtr<FJsonValue> BehaviorJV       = Params->TryGetField(TEXT("loop_behavior"));
	const TSharedPtr<FJsonValue> DurationJV       = Params->TryGetField(TEXT("loop_duration"));
	const TSharedPtr<FJsonValue> DelayJV          = Params->TryGetField(TEXT("loop_delay"));
	const TSharedPtr<FJsonValue> CountJV          = Params->TryGetField(TEXT("loop_count"));
	const TSharedPtr<FJsonValue> DelayEnabledJV   = Params->TryGetField(TEXT("loop_delay_enabled"));
	const TSharedPtr<FJsonValue> DurationModeJV   = Params->TryGetField(TEXT("loop_duration_mode"));

	const bool bAnyField =
		(BehaviorJV.IsValid()     && BehaviorJV->Type     == EJson::String)  ||
		(DurationJV.IsValid()     && DurationJV->Type     == EJson::Number)  ||
		(DelayJV.IsValid()        && DelayJV->Type        == EJson::Number)  ||
		(CountJV.IsValid()        && CountJV->Type        == EJson::Number)  ||
		(DelayEnabledJV.IsValid() && DelayEnabledJV->Type == EJson::Boolean) ||
		(DurationModeJV.IsValid() && DurationModeJV->Type == EJson::String);
	if (!bAnyField)
		return FMonolithActionResult::Error(TEXT("Must supply at least one of: loop_behavior, loop_duration_mode, loop_duration, loop_delay, loop_count, loop_delay_enabled"));

	// Standalone UNiagaraStatelessEmitter dispatch — load the asset path as a bare
	// UObject and class-name-string-match against "NiagaraStatelessEmitter". The
	// stateless header lives in Niagara's Internal/ tree (not on our include path),
	// so we cannot static_cast<UNiagaraStatelessEmitter*> here. Class-name match
	// is sufficient: WriteStatelessLoopProfile reflects on EmitterState via
	// FindFProperty and never needs the concrete type.
	if (UObject* LoadedObject = StaticLoadObject(UObject::StaticClass(), nullptr, *SystemPath))
	{
		if (LoadedObject->GetClass() &&
			LoadedObject->GetClass()->GetName() == TEXT("NiagaraStatelessEmitter"))
		{
			// `emitter` is meaningless on standalone stateless asset — accept absent
			// or "self" silently. Any other value is ignored (no error: schema-doc
			// already says it's meaningless for this branch).
			TArray<TSharedPtr<FJsonValue>> StatelessWarnings;
			return WriteStatelessLoopProfile(LoadedObject, Params, StatelessWarnings);
		}
	}

	// System-asset path. `emitter` is mandatory here.
	if (Emitter.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required field: emitter (string) — required when asset_path is a UNiagaraSystem"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	// Resolve emitter handle. Use the public list_emitters / set_module_input_value
	// emitter-resolution contract (string id or name) — we can't reuse
	// FMonolithNiagaraActions::FindEmitterHandleIndex (private static), so we
	// inline a small lookup against the same matchers.
	int32 EIdx = INDEX_NONE;
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		const FNiagaraEmitterHandle& H = Handles[i];
		if (H.GetName().ToString().Equals(Emitter, ESearchCase::IgnoreCase) ||
			H.GetIdName().ToString().Equals(Emitter, ESearchCase::IgnoreCase) ||
			H.GetId().ToString().Equals(Emitter, ESearchCase::IgnoreCase))
		{
			EIdx = i;
			break;
		}
	}
	if (EIdx == INDEX_NONE)
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Emitter '%s' not found. Use list_emitters to get valid emitter names or GUIDs."), *Emitter));

	// Stateless emitter dispatch — Phase 2. The stateless branch writes EmitterState
	// UPROPERTYs via reflection (UNiagaraStatelessEmitter header is in Internal/ and
	// not on our include path; we route via GetEmitterBase() to get a UObject*).
	if (Handles[EIdx].GetStatelessEmitter() != nullptr)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		UObject* StatelessEmitter = Handles[EIdx].GetEmitterBase();
#else
		// UE 5.6 doesn't have the GetEmitterBase() convenience accessor yet, and
		// UNiagaraStatelessEmitter's header lives in Internal/ (not on our include
		// path), so its pointer is compile-time incomplete — read it as a raw
		// UObject* via the struct's own reflection instead of a static_cast.
		UObject* StatelessEmitter = nullptr;
		if (FObjectPropertyBase* StatelessProp = CastField<FObjectPropertyBase>(
				FNiagaraEmitterHandle::StaticStruct()->FindPropertyByName(TEXT("StatelessEmitter"))))
		{
			StatelessEmitter = StatelessProp->GetObjectPropertyValue_InContainer(&Handles[EIdx]);
		}
#endif
		TArray<TSharedPtr<FJsonValue>> StatelessWarnings;
		return WriteStatelessLoopProfile(StatelessEmitter, Params, StatelessWarnings);
	}

	TArray<TSharedPtr<FJsonValue>> Warnings;
	FString BehaviorCanonical;

	if (BehaviorJV.IsValid() && BehaviorJV->Type == EJson::String)
	{
		BehaviorCanonical = CanonicalizeLoopBehavior(BehaviorJV->AsString());
		if (BehaviorCanonical.IsEmpty())
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Unknown loop_behavior '%s'. Valid: 'Once' | 'Multiple' | 'Infinite'"),
				*BehaviorJV->AsString()));
		}
	}

	// loop_duration_mode is stateless-only — warn if supplied to the stateful path.
	if (DurationModeJV.IsValid() && DurationModeJV->Type == EJson::String)
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT(
			"loop_duration_mode supplied to a stateful emitter — ignored. The stateful EmitterState module does not expose a LoopDurationMode user input.")));
	}

	// Non-fatal coherence warnings (per design Q1 — warnings array, not errors).
	if (CountJV.IsValid() && CountJV->Type == EJson::Number)
	{
		const bool bBehaviorIsMultiple = (BehaviorCanonical == TEXT("Multiple"));
		if (BehaviorJV.IsValid() && !bBehaviorIsMultiple)
		{
			Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("loop_count supplied with loop_behavior='%s' — LoopCount only applies when behavior is 'Multiple'."),
				*BehaviorCanonical)));
		}
	}
	if (DelayJV.IsValid() && DelayJV->Type == EJson::Number)
	{
		const bool bDelayExplicitlyEnabled = DelayEnabledJV.IsValid() &&
			DelayEnabledJV->Type == EJson::Boolean && DelayEnabledJV->AsBool();
		if (!bDelayExplicitlyEnabled)
		{
			Warnings.Add(MakeShared<FJsonValueString>(TEXT(
				"loop_delay supplied without loop_delay_enabled=true — the LoopDelay input will be set but bLoopDelayEnabled may suppress it.")));
		}
	}

	// Dispatch each supplied field through set_module_input_value. Track per-field
	// failures and surface them as warnings (so a partial write is observable
	// instead of silently dropped).
	auto AppendWriteFailure = [&Warnings](const FString& Field, const FMonolithActionResult& R)
	{
		Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
			TEXT("Failed to write %s: %s"), *Field, *R.ErrorMessage)));
	};

	// EmitterState exposes its loop topology as two distinct dispatch surfaces
	// (verified by PIE smoke-test 2026-05-28):
	//   - "Loop Behavior" + "UseLoopDelay" are STATIC SWITCHES (display-name with space).
	//   - "Loop Duration" / "Loop Delay" / "Loop Count" are REGULAR MODULE INPUTS
	//     (display-names with spaces, NOT C++ UPROPERTY names).
	if (!BehaviorCanonical.IsEmpty())
	{
		// "Loop Behavior" static switch -- ENiagara_EmitterStateOptions enum-name passthrough.
		FMonolithActionResult R = DispatchModuleSwitch(
			SystemPath, Emitter, EmitterStateModuleName, TEXT("Loop Behavior"), BehaviorCanonical);
		if (!R.bSuccess) AppendWriteFailure(TEXT("Loop Behavior"), R);
	}
	if (DurationJV.IsValid() && DurationJV->Type == EJson::Number)
	{
		// "Loop Duration" module input -- number stringified for the pin DefaultValue.
		TSharedPtr<FJsonValue> Val = MakeShared<FJsonValueString>(
			FString::SanitizeFloat(DurationJV->AsNumber()));
		FMonolithActionResult R = DispatchModuleInput(
			SystemPath, Emitter, EmitterStateModuleName, TEXT("Loop Duration"), Val);
		if (!R.bSuccess) AppendWriteFailure(TEXT("Loop Duration"), R);
	}
	if (DelayJV.IsValid() && DelayJV->Type == EJson::Number)
	{
		// "Loop Delay" module input.
		TSharedPtr<FJsonValue> Val = MakeShared<FJsonValueString>(
			FString::SanitizeFloat(DelayJV->AsNumber()));
		FMonolithActionResult R = DispatchModuleInput(
			SystemPath, Emitter, EmitterStateModuleName, TEXT("Loop Delay"), Val);
		if (!R.bSuccess) AppendWriteFailure(TEXT("Loop Delay"), R);
	}
	if (CountJV.IsValid() && CountJV->Type == EJson::Number)
	{
		// "Loop Count" module input -- integer.
		TSharedPtr<FJsonValue> Val = MakeShared<FJsonValueString>(
			FString::Printf(TEXT("%d"), static_cast<int32>(CountJV->AsNumber())));
		FMonolithActionResult R = DispatchModuleInput(
			SystemPath, Emitter, EmitterStateModuleName, TEXT("Loop Count"), Val);
		if (!R.bSuccess) AppendWriteFailure(TEXT("Loop Count"), R);
	}
	if (DelayEnabledJV.IsValid() && DelayEnabledJV->Type == EJson::Boolean)
	{
		// "UseLoopDelay" static switch -- NiagaraBool, "true"/"false" string.
		const FString BoolStr = DelayEnabledJV->AsBool() ? TEXT("true") : TEXT("false");
		FMonolithActionResult R = DispatchModuleSwitch(
			SystemPath, Emitter, EmitterStateModuleName, TEXT("UseLoopDelay"), BoolStr);
		if (!R.bSuccess) AppendWriteFailure(TEXT("UseLoopDelay"), R);
	}

	TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetBoolField(TEXT("success"), true);
	Resp->SetStringField(TEXT("asset_path"), SystemPath);
	Resp->SetStringField(TEXT("emitter"), Emitter);
	Resp->SetArrayField(TEXT("warnings"), Warnings);
	return SuccessObj(Resp);
}

FMonolithActionResult FMonolithNiagaraTimingActions::HandleGetEmitterTimingSummary(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithNiagaraTimingLocal;

	const FString SystemPath = GetAssetPath(Params);
	if (SystemPath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required field: asset_path"));

	FString FilterEmitter;
	Params->TryGetStringField(TEXT("emitter"), FilterEmitter);

	// Standalone UNiagaraStatelessEmitter dispatch — mirrors the write-side
	// branch in HandleSetEmitterLoopProfile (lines 800-817). Class-name string
	// match because UNiagaraStatelessEmitter lives in Niagara's Internal/ tree
	// (not on our include path). ReadStatelessLoopProfile uses reflection only.
	if (UObject* LoadedObject = StaticLoadObject(UObject::StaticClass(), nullptr, *SystemPath))
	{
		if (LoadedObject->GetClass() &&
			LoadedObject->GetClass()->GetName() == TEXT("NiagaraStatelessEmitter"))
		{
			TArray<TSharedPtr<FJsonValue>> EmittersArr;
			EmittersArr.Add(MakeShared<FJsonValueObject>(
				ReadStatelessLoopProfile(LoadedObject).ToSharedRef()));
			TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
			Resp->SetStringField(TEXT("asset_path"), SystemPath);
			Resp->SetArrayField(TEXT("emitters"), EmittersArr);
			return SuccessObj(Resp);
		}
	}

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	TArray<TSharedPtr<FJsonValue>> EmittersArr;
	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		const FNiagaraEmitterHandle& H = Handles[i];
		const FString HandleName = H.GetName().ToString();

		// Filter by name/id if the caller specified an emitter.
		if (!FilterEmitter.IsEmpty())
		{
			const bool bMatch =
				HandleName.Equals(FilterEmitter, ESearchCase::IgnoreCase) ||
				H.GetIdName().ToString().Equals(FilterEmitter, ESearchCase::IgnoreCase) ||
				H.GetId().ToString().Equals(FilterEmitter, ESearchCase::IgnoreCase);
			if (!bMatch) continue;
		}

		// Stateless emitters dispatch to the Phase 3 reflection reader — it
		// returns the full per-emitter shape (loop_behavior/count/duration/etc.)
		// instead of the prior thin {stateless:true} stub. Re-stamp name+index
		// so the helper's defaults (own name, index=0) don't leak through.
		if (H.GetStatelessEmitter() != nullptr)
		{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			TSharedPtr<FJsonObject> StatelessObj =
				ReadStatelessLoopProfile(H.GetEmitterBase());
#else
			// UE 5.6 doesn't have the GetEmitterBase() convenience accessor yet, and
			// UNiagaraStatelessEmitter's header lives in Internal/ (not on our include
			// path), so its pointer is compile-time incomplete — read it as a raw
			// UObject* via the struct's own reflection instead of a static_cast.
			UObject* StatelessEmitterObj = nullptr;
			if (FObjectPropertyBase* StatelessProp = CastField<FObjectPropertyBase>(
					FNiagaraEmitterHandle::StaticStruct()->FindPropertyByName(TEXT("StatelessEmitter"))))
			{
				StatelessEmitterObj = StatelessProp->GetObjectPropertyValue_InContainer(&H);
			}
			TSharedPtr<FJsonObject> StatelessObj = ReadStatelessLoopProfile(StatelessEmitterObj);
#endif
			StatelessObj->SetStringField(TEXT("name"), HandleName);
			StatelessObj->SetNumberField(TEXT("index"), i);
			EmittersArr.Add(MakeShared<FJsonValueObject>(StatelessObj.ToSharedRef()));
			continue;
		}

		TSharedRef<FJsonObject> EObj = MakeShared<FJsonObject>();
		EObj->SetStringField(TEXT("name"), HandleName);
		EObj->SetNumberField(TEXT("index"), i);
		EObj->SetBoolField(TEXT("stateless"), false);

		// --- EmitterState loop inputs (read via get_module_input_value) ---
		auto SetNullableString = [&EObj](const TCHAR* Field, const FString& Val, bool bValid)
		{
			if (bValid) EObj->SetStringField(Field, Val);
			else        EObj->SetField(Field, MakeShared<FJsonValueNull>());
		};
		auto SetNullableDouble = [&EObj](const TCHAR* Field, double Val, bool bValid)
		{
			if (bValid) EObj->SetNumberField(Field, Val);
			else        EObj->SetField(Field, MakeShared<FJsonValueNull>());
		};
		auto SetNullableInt = [&EObj](const TCHAR* Field, int32 Val, bool bValid)
		{
			if (bValid) EObj->SetNumberField(Field, Val);
			else        EObj->SetField(Field, MakeShared<FJsonValueNull>());
		};
		auto SetNullableBool = [&EObj](const TCHAR* Field, bool Val, bool bValid)
		{
			if (bValid) EObj->SetBoolField(Field, Val);
			else        EObj->SetField(Field, MakeShared<FJsonValueNull>());
		};

		// EmitterState read surfaces match the write surfaces:
		//   - "Loop Behavior" + "UseLoopDelay" via get_static_switch_value
		//   - "Loop Duration" / "Loop Delay" / "Loop Count" via get_module_input_value
		{
			// "Loop Behavior" static switch -- enum-name string passthrough ("Once"/"Multiple"/"Infinite").
			FString S;
			bool bOK = TryGetSummaryString(
				DispatchGetModuleSwitch(SystemPath, HandleName, EmitterStateModuleName, TEXT("Loop Behavior")), S);
			SetNullableString(TEXT("loop_behavior"), S, bOK);
		}
		{
			double D;
			bool bOK = TryGetSummaryDouble(
				DispatchGetModuleInput(SystemPath, HandleName, EmitterStateModuleName, TEXT("Loop Duration")), D);
			SetNullableDouble(TEXT("loop_duration"), D, bOK);
		}
		{
			double D;
			bool bOK = TryGetSummaryDouble(
				DispatchGetModuleInput(SystemPath, HandleName, EmitterStateModuleName, TEXT("Loop Delay")), D);
			SetNullableDouble(TEXT("loop_delay"), D, bOK);
		}
		{
			int32 V;
			bool bOK = TryGetSummaryInt(
				DispatchGetModuleInput(SystemPath, HandleName, EmitterStateModuleName, TEXT("Loop Count")), V);
			SetNullableInt(TEXT("loop_count"), V, bOK);
		}
		{
			// "UseLoopDelay" static switch -- NiagaraBool, "true"/"false" string.
			bool B;
			bool bOK = TryGetSummaryBool(
				DispatchGetModuleSwitch(SystemPath, HandleName, EmitterStateModuleName, TEXT("UseLoopDelay")), B);
			SetNullableBool(TEXT("loop_delay_enabled"), B, bOK);
		}

		// --- Sim-stage list (direct read against the emitter data — no dispatch) ---
		TArray<TSharedPtr<FJsonValue>> StagesArr;
		if (FVersionedNiagaraEmitterData* ED = H.GetEmitterData())
		{
			const TArray<UNiagaraSimulationStageBase*>& Stages = ED->GetSimulationStages();
			for (int32 SI = 0; SI < Stages.Num(); ++SI)
			{
				UNiagaraSimulationStageBase* Stage = Stages[SI];
				if (!Stage) continue;
				TSharedRef<FJsonObject> SO = MakeShared<FJsonObject>();
				SO->SetNumberField(TEXT("index"), SI);
				SO->SetStringField(TEXT("name"), Stage->SimulationStageName.ToString());
				SO->SetBoolField(TEXT("enabled"), Stage->bEnabled != 0);

				if (UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage))
				{
					FString ExecBehaviorStr;
					switch (Generic->ExecuteBehavior)
					{
					case ENiagaraSimStageExecuteBehavior::Always:               ExecBehaviorStr = TEXT("Always"); break;
					case ENiagaraSimStageExecuteBehavior::OnSimulationReset:    ExecBehaviorStr = TEXT("OnSimulationReset"); break;
					case ENiagaraSimStageExecuteBehavior::NotOnSimulationReset: ExecBehaviorStr = TEXT("NotOnSimulationReset"); break;
					default: ExecBehaviorStr = TEXT("Unknown"); break;
					}
					SO->SetStringField(TEXT("execute_behavior"), ExecBehaviorStr);

					// NumIterations is FNiagaraParameterBindingWithValue — read via
					// reflection ExportTextItem_Direct so the binding's serialized
					// form ("(Value=N)") is what surfaces; that matches the write
					// envelope the alias produces.
					if (FProperty* NumIterProp = UNiagaraSimulationStageGeneric::StaticClass()
						->FindPropertyByName(TEXT("NumIterations")))
					{
						FString ValStr;
						NumIterProp->ExportTextItem_Direct(
							ValStr,
							NumIterProp->ContainerPtrToValuePtr<void>(Generic),
							nullptr, Generic, PPF_None);
						SO->SetStringField(TEXT("num_iterations"), ValStr);
					}
					else
					{
						SO->SetField(TEXT("num_iterations"), MakeShared<FJsonValueNull>());
					}
				}
				else
				{
					SO->SetField(TEXT("execute_behavior"), MakeShared<FJsonValueNull>());
					SO->SetField(TEXT("num_iterations"), MakeShared<FJsonValueNull>());
				}
				StagesArr.Add(MakeShared<FJsonValueObject>(SO));
			}
		}
		EObj->SetArrayField(TEXT("sim_stages"), StagesArr);

		// --- InitializeParticle / Lifetime (best-effort) ---
		// Module-node name = "InitializeParticle" (verified PIE 2026-05-28, Phase 4
		// contract). Lifetime exposed via three NiagaraFloat inputs + one static
		// switch: "Lifetime" / "Lifetime Min" / "Lifetime Max" + "Lifetime Mode"
		// ("Direct" | "Random"). We surface the raw mode + the three inputs so
		// callers can branch on mode without re-fetching.
		{
			FString S;
			bool bOK = TryGetSummaryString(
				DispatchGetModuleSwitch(SystemPath, HandleName, InitializeParticleModuleName, TEXT("Lifetime Mode")), S);
			SetNullableString(TEXT("lifetime_mode"), S, bOK);
		}
		{
			double D;
			bool bOK = TryGetSummaryDouble(
				DispatchGetModuleInput(SystemPath, HandleName, InitializeParticleModuleName, TEXT("Lifetime")), D);
			SetNullableDouble(TEXT("lifetime"), D, bOK);
		}
		{
			double D;
			bool bOK = TryGetSummaryDouble(
				DispatchGetModuleInput(SystemPath, HandleName, InitializeParticleModuleName, TEXT("Lifetime Min")), D);
			SetNullableDouble(TEXT("lifetime_min"), D, bOK);
		}
		{
			double D;
			bool bOK = TryGetSummaryDouble(
				DispatchGetModuleInput(SystemPath, HandleName, InitializeParticleModuleName, TEXT("Lifetime Max")), D);
			SetNullableDouble(TEXT("lifetime_max"), D, bOK);
		}

		EmittersArr.Add(MakeShared<FJsonValueObject>(EObj));
	}

	if (!FilterEmitter.IsEmpty() && EmittersArr.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Emitter '%s' not found. Use list_emitters to get valid emitter names or GUIDs."),
			*FilterEmitter));
	}

	TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetStringField(TEXT("asset_path"), SystemPath);
	Resp->SetArrayField(TEXT("emitters"), EmittersArr);
	return SuccessObj(Resp);
}

// ----------------------------------------------------------------------------
//  Phase 2 — Sim-stage alias dispatch helper
// ----------------------------------------------------------------------------
//
// Both Phase-2 actions are thin aliases over niagara::set_simulation_stage_property
// (MonolithNiagaraActions.cpp:11495-11652). We dispatch through the public
// registry rather than calling FMonolithNiagaraActions::HandleSetSimulationStageProperty
// directly because the canonical handler is a private static in another TU
// (same private-static cross-TU issue as Phase 1's LoadSystem). Registry
// dispatch is schema-validated, single source of truth, zero duplication.
//
namespace MonolithNiagaraTimingLocal
{
	// Build the JSON envelope set_simulation_stage_property expects:
	//   { asset_path, emitter, stage_index?, stage_name?, property, value }
	// then dispatch via FMonolithToolRegistry::Get().ExecuteAction.
	static FMonolithActionResult DispatchSimStageAlias(
		const TSharedPtr<FJsonObject>& Params,
		const FString& Property,
		const TSharedPtr<FJsonValue>& Value)
	{
		// Forward asset_path / system_path (canonical handler reads asset_path; the
		// upstream GetAssetPath also accepts system_path so either works).
		const FString SystemPath = GetAssetPath(Params);
		if (SystemPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("Missing required field: asset_path"));

		// Forward emitter (required by canonical handler).
		FString Emitter;
		if (!Params->TryGetStringField(TEXT("emitter"), Emitter) || Emitter.IsEmpty())
			return FMonolithActionResult::Error(TEXT("Missing required field: emitter (string)"));

		// Forward exactly one of stage_index / stage_name (canonical handler
		// validates that at least one is supplied; we mirror that here to give a
		// clearer error attributed to the alias).
		const bool bHasIndex = Params->HasTypedField<EJson::Number>(TEXT("stage_index"));
		const bool bHasName = Params->HasTypedField<EJson::String>(TEXT("stage_name"));
		if (!bHasIndex && !bHasName)
			return FMonolithActionResult::Error(TEXT("Must provide stage_index or stage_name"));

		if (!Value.IsValid())
			return FMonolithActionResult::Error(TEXT("Internal: alias passed null value"));

		TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetStringField(TEXT("asset_path"), SystemPath);
		Envelope->SetStringField(TEXT("emitter"), Emitter);
		if (bHasIndex)
		{
			Envelope->SetNumberField(TEXT("stage_index"), Params->GetNumberField(TEXT("stage_index")));
		}
		if (bHasName)
		{
			Envelope->SetStringField(TEXT("stage_name"), Params->GetStringField(TEXT("stage_name")));
		}
		Envelope->SetStringField(TEXT("property"), Property);
		Envelope->SetField(TEXT("value"), Value);

		return FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("niagara"), TEXT("set_simulation_stage_property"), Envelope);
	}
}

FMonolithActionResult FMonolithNiagaraTimingActions::HandleSetSimStageIterationCount(const TSharedPtr<FJsonObject>& Params)
{
	// iterations: integer (required) — NumIterations on UNiagaraSimulationStageGeneric is
	// FNiagaraParameterBindingWithValue, written via ImportText_Direct fallback at
	// MonolithNiagaraActions.cpp:11600-11635. The fallback stringifies a JSON Number
	// using "%g" before ImportText_Direct, so we forward the integer as a JSON Number.
	TSharedPtr<FJsonValue> IterJV = Params->TryGetField(TEXT("iterations"));
	if (!IterJV.IsValid() || IterJV->Type != EJson::Number)
		return FMonolithActionResult::Error(TEXT("Missing required field: iterations (integer)"));

	const int32 Iterations = static_cast<int32>(IterJV->AsNumber());
	if (Iterations < 0)
		return FMonolithActionResult::Error(TEXT("iterations must be >= 0"));

	TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueString>(FString::Printf(TEXT("(Value=%d)"), Iterations));
	return MonolithNiagaraTimingLocal::DispatchSimStageAlias(Params, TEXT("NumIterations"), Value);
}

FMonolithActionResult FMonolithNiagaraTimingActions::HandleSetSimStageExecuteBehavior(const TSharedPtr<FJsonObject>& Params)
{
	// behavior: string (required) — one of Always / OnSimulationReset / NotOnSimulationReset.
	// The canonical handler at MonolithNiagaraActions.cpp:11572-11578 accepts both
	// PascalCase and snake_case via a .ToLower() comparison, so we forward verbatim
	// and let it do the enum mapping. Validate non-empty here for a clearer error.
	FString Behavior;
	if (!Params->TryGetStringField(TEXT("behavior"), Behavior) || Behavior.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required field: behavior (string: 'Always' | 'OnSimulationReset' | 'NotOnSimulationReset')"));

	TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueString>(Behavior);
	return MonolithNiagaraTimingLocal::DispatchSimStageAlias(Params, TEXT("ExecuteBehavior"), Value);
}

FMonolithActionResult FMonolithNiagaraTimingActions::HandleSetParticleLifetime(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithNiagaraTimingLocal;

	const FString SystemPath = GetAssetPath(Params);
	if (SystemPath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required field: asset_path"));

	FString Emitter;
	if (!Params->TryGetStringField(TEXT("emitter"), Emitter) || Emitter.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required field: emitter (string)"));

	TSharedPtr<FJsonValue> MinJV = Params->TryGetField(TEXT("min"));
	if (!MinJV.IsValid() || MinJV->Type != EJson::Number)
		return FMonolithActionResult::Error(TEXT("Missing required field: min (number)"));
	const double MinValue = MinJV->AsNumber();

	const TSharedPtr<FJsonValue> MaxJV = Params->TryGetField(TEXT("max"));
	const bool bHasMax = MaxJV.IsValid() && MaxJV->Type == EJson::Number;
	const double MaxValue = bHasMax ? MaxJV->AsNumber() : 0.0;

	// Validate emitter handle existence + stateless rejection up-front so we can
	// produce a clean error without mid-dispatch partial-write side effects.
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 EIdx = INDEX_NONE;
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		const FNiagaraEmitterHandle& H = Handles[i];
		if (H.GetName().ToString().Equals(Emitter, ESearchCase::IgnoreCase) ||
			H.GetIdName().ToString().Equals(Emitter, ESearchCase::IgnoreCase) ||
			H.GetId().ToString().Equals(Emitter, ESearchCase::IgnoreCase))
		{
			EIdx = i;
			break;
		}
	}
	if (EIdx == INDEX_NONE)
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Emitter '%s' not found. Use list_emitters to get valid emitter names or GUIDs."), *Emitter));

	if (Handles[EIdx].GetStatelessEmitter() != nullptr)
	{
		return FMonolithActionResult::Error(TEXT(
			"Stateless emitter (UNiagaraStatelessEmitter) detected -- set_particle_lifetime only "
			"supports stateful emitters (InitializeParticle module lives on the particle_spawn script "
			"of UNiagaraEmitter, not on stateless emitters)."));
	}

	// Lifetime Mode: write the static switch FIRST so subsequent input writes
	// target the freshly-active branch ("Lifetime" for Direct, "Lifetime Min"/
	// "Lifetime Max" for Random). The upstream set_static_switch_value handler
	// performs the static-switch compile path; ordering here mirrors the
	// EmitterState loop-profile handler's behavior-first dispatch.
	const FString ModeStr = bHasMax ? TEXT("Random") : TEXT("Direct");
	FMonolithActionResult ModeResult = DispatchModuleSwitch(
		SystemPath, Emitter, InitializeParticleModuleName, TEXT("Lifetime Mode"), ModeStr);
	if (!ModeResult.bSuccess)
	{
		// Module-not-found surfaces as a generic set_module_input_value/
		// set_static_switch_value failure; rewrite with the actionable hint.
		const FString ErrLower = ModeResult.ErrorMessage.ToLower();
		if (ErrLower.Contains(TEXT("not found")) || ErrLower.Contains(TEXT("no module")))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("InitializeParticle module not found on emitter '%s'. Use niagara::add_module to add "
				     "InitializeParticle.InitializeParticle to particle_spawn first. (Upstream: %s)"),
				*Emitter, *ModeResult.ErrorMessage));
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to set Lifetime Mode='%s': %s"), *ModeStr, *ModeResult.ErrorMessage));
	}

	// Now write the numeric input(s) matching the chosen mode.
	if (bHasMax)
	{
		TSharedPtr<FJsonValue> MinVal = MakeShared<FJsonValueString>(FString::SanitizeFloat(MinValue));
		FMonolithActionResult R = DispatchModuleInput(
			SystemPath, Emitter, InitializeParticleModuleName, TEXT("Lifetime Min"), MinVal);
		if (!R.bSuccess)
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to set Lifetime Min: %s"), *R.ErrorMessage));

		TSharedPtr<FJsonValue> MaxVal = MakeShared<FJsonValueString>(FString::SanitizeFloat(MaxValue));
		R = DispatchModuleInput(
			SystemPath, Emitter, InitializeParticleModuleName, TEXT("Lifetime Max"), MaxVal);
		if (!R.bSuccess)
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to set Lifetime Max: %s"), *R.ErrorMessage));
	}
	else
	{
		TSharedPtr<FJsonValue> Val = MakeShared<FJsonValueString>(FString::SanitizeFloat(MinValue));
		FMonolithActionResult R = DispatchModuleInput(
			SystemPath, Emitter, InitializeParticleModuleName, TEXT("Lifetime"), Val);
		if (!R.bSuccess)
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to set Lifetime: %s"), *R.ErrorMessage));
	}

	TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetBoolField(TEXT("success"), true);
	Resp->SetStringField(TEXT("asset_path"), SystemPath);
	Resp->SetStringField(TEXT("emitter"), Emitter);
	Resp->SetStringField(TEXT("lifetime_mode"), ModeStr);
	if (bHasMax)
	{
		Resp->SetNumberField(TEXT("lifetime_min"), MinValue);
		Resp->SetNumberField(TEXT("lifetime_max"), MaxValue);
	}
	else
	{
		Resp->SetNumberField(TEXT("lifetime"), MinValue);
	}
	return SuccessObj(Resp);
}
