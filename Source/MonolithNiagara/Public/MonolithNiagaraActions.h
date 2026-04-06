#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"
#include "NiagaraCommon.h"

class UNiagaraSystem;
class UNiagaraNodeOutput;
class UNiagaraNodeFunctionCall;
class UNiagaraScript;
class UNiagaraGraph;
class UNiagaraRendererProperties;
class UNiagaraDataInterface;
struct FVersionedNiagaraEmitterData;
struct FNiagaraVariable;
struct FNiagaraParameterStore;

/**
 * Niagara domain action handlers for Monolith.
 * 89 actions across system, module, parameter, renderer, DI, diagnostics, NPC, effect type, and advanced domains.
 * Waves 1-6 + Phases 3-7.
 * Fixed for UE 5.7 API compatibility.
 */
class FMonolithNiagaraActions
{
public:
	/** Register all niagara actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// --- System (8) ---
	static FMonolithActionResult HandleAddEmitter(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveEmitter(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDuplicateEmitter(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetEmitterEnabled(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleReorderEmitters(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetEmitterProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRequestCompile(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateSystem(const TSharedPtr<FJsonObject>& Params);

	// --- Module (12) ---
	static FMonolithActionResult HandleGetOrderedModules(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetModuleInputs(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetModuleGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddModule(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveModule(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleMoveModule(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetModuleEnabled(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetModuleInputValue(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetModuleInputBinding(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetModuleInputDI(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateModuleFromHLSL(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateFunctionFromHLSL(const TSharedPtr<FJsonObject>& Params);

	// --- Parameter (9) ---
	static FMonolithActionResult HandleGetAllParameters(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetUserParameters(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetParameterValue(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetParameterType(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleTraceParameterBinding(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddUserParameter(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveUserParameter(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetParameterDefault(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetCurveValue(const TSharedPtr<FJsonObject>& Params);

	// --- Renderer (6) ---
	static FMonolithActionResult HandleAddRenderer(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveRenderer(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetRendererMaterial(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetRendererProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetRendererBindings(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetRendererBinding(const TSharedPtr<FJsonObject>& Params);

	// --- Read/Discovery (4) ---
	static FMonolithActionResult HandleListEmitters(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListRenderers(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListModuleScripts(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListRendererProperties(const TSharedPtr<FJsonObject>& Params);

	// --- Batch (2) ---
	static FMonolithActionResult HandleBatchExecute(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateSystemFromSpec(const TSharedPtr<FJsonObject>& Params);

	// --- Data Interface (1) ---
	static FMonolithActionResult HandleGetDIFunctions(const TSharedPtr<FJsonObject>& Params);

	// --- HLSL (1) ---
	static FMonolithActionResult HandleGetCompiledGPUHLSL(const TSharedPtr<FJsonObject>& Params);

	// --- Diagnostics (1) ---
	static FMonolithActionResult HandleGetSystemDiagnostics(const TSharedPtr<FJsonObject>& Params);

	// --- System Property (2) ---
	static FMonolithActionResult HandleGetSystemProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetSystemProperty(const TSharedPtr<FJsonObject>& Params);

	// --- Static Switch (1) ---
	static FMonolithActionResult HandleSetStaticSwitchValue(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 2: Summary & Discovery (4 new) ---
	static FMonolithActionResult HandleGetSystemSummary(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetEmitterSummary(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListEmitterProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetModuleInputValue(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 3: DI Curve & Configuration (2 new) ---
	static FMonolithActionResult HandleConfigureCurveKeys(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleConfigureDataInterface(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 4: System Management (5 new) ---
	static FMonolithActionResult HandleDuplicateSystem(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetFixedBounds(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetEffectType(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateEmitter(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleExportSystemSpec(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 5: Dynamic Inputs (3 new) ---
	static FMonolithActionResult HandleAddDynamicInput(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetDynamicInputValue(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSearchDynamicInputs(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 6: Advanced (3 new) ---
	static FMonolithActionResult HandleAddEventHandler(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleValidateSystem(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddSimulationStage(const TSharedPtr<FJsonObject>& Params);

	// --- Composite Helpers (1 new) ---
	static FMonolithActionResult HandleSetSpawnShape(const TSharedPtr<FJsonObject>& Params);

	// --- Phase 3: Dynamic Input Features (5 new) ---
	static FMonolithActionResult HandleListDynamicInputs(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetDynamicInputTree(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveDynamicInput(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetDynamicInputValue(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetDynamicInputInputs(const TSharedPtr<FJsonObject>& Params);

	// --- Phase 4: Module & Emitter Management (2 new) ---
	static FMonolithActionResult HandleRenameEmitter(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetEmitterProperty(const TSharedPtr<FJsonObject>& Params);

	// --- Phase 5: Renderer & DI Improvements (4 new) ---
	static FMonolithActionResult HandleListAvailableRenderers(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetRendererMesh(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleConfigureRibbon(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleConfigureSubUV(const TSharedPtr<FJsonObject>& Params);

	// --- Phase 6A: Event Handlers, Simulation Stages, Module Outputs (7 new) ---
	static FMonolithActionResult HandleGetEventHandlers(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetEventHandlerProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveEventHandler(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetSimulationStages(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetSimulationStageProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveSimulationStage(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetModuleOutputParameters(const TSharedPtr<FJsonObject>& Params);

	// --- Phase 6B: NPC Support (5 new) ---
	static FMonolithActionResult HandleCreateNPC(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetNPC(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddNPCParameter(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveNPCParameter(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetNPCDefault(const TSharedPtr<FJsonObject>& Params);

	// --- Phase 6B: Effect Type CRUD (3 new) ---
	static FMonolithActionResult HandleCreateEffectType(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetEffectType(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetEffectTypeProperty(const TSharedPtr<FJsonObject>& Params);

	// --- Phase 6B: Parameter Discovery (1 new) ---
	static FMonolithActionResult HandleGetAvailableParameters(const TSharedPtr<FJsonObject>& Params);

	// --- Phase 6B: Preview (1 new) ---
	static FMonolithActionResult HandlePreviewSystem(const TSharedPtr<FJsonObject>& Params);

	// --- Phase 7: Advanced Features (3 new) ---
	static FMonolithActionResult HandleDiffSystems(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSaveEmitterAsTemplate(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCloneModuleOverrides(const TSharedPtr<FJsonObject>& Params);

	// --- Phase 8: Expansion (4 new) ---
	static FMonolithActionResult HandleSaveSystem(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetStaticSwitchValue(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleImportSystemSpec(const TSharedPtr<FJsonObject>& Params);

	// --- Phase 9: Medium Priority Expansion (5 new) ---
	static FMonolithActionResult HandleGetDIProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleClearEmitterModules(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetModuleScriptInputs(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetScalabilitySettings(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetScalabilitySettings(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListSystems(const TSharedPtr<FJsonObject>& Params);

	// --- Phase 10: Low Priority & QoL (3 new) ---
	static FMonolithActionResult HandleDuplicateModule(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetEmitterParent(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRenameUserParameter(const TSharedPtr<FJsonObject>& Params);

	// --- Helpers (public for use by free functions) ---
	static FString SerializeParameterValue(const FNiagaraVariable& Variable, const FNiagaraParameterStore& Store);

private:
	// --- Internal helpers ---
	static UNiagaraSystem* LoadSystem(const FString& SystemPath);
	static int32 FindEmitterHandleIndex(UNiagaraSystem* System, const FString& HandleIdOrName);
	static bool ResolveScriptUsage(const FString& UsageString, ENiagaraScriptUsage& OutUsage);
	static FString UsageToString(ENiagaraScriptUsage Usage);
	static UNiagaraGraph* GetGraphForUsage(UNiagaraSystem* System, const FString& EmitterHandleId, ENiagaraScriptUsage Usage);
	static UNiagaraNodeOutput* FindOutputNode(UNiagaraSystem* System, const FString& EmitterHandleId, ENiagaraScriptUsage Usage);
	static UNiagaraNodeFunctionCall* FindModuleNode(UNiagaraSystem* System, const FString& EmitterHandleId, const FString& NodeGuidStr, ENiagaraScriptUsage* OutUsage = nullptr);
	static UNiagaraNodeFunctionCall* FindFunctionCallNode(UNiagaraSystem* System, const FString& EmitterHandleId, const FString& NodeGuidStr);
	static UClass* ResolveRendererClass(const FString& RendererClass);
	static UNiagaraRendererProperties* GetRenderer(UNiagaraSystem* System, const FString& EmitterHandleId, int32 RendererIndex, FVersionedNiagaraEmitterData** OutEmitterData = nullptr);
	static FNiagaraTypeDefinition ResolveNiagaraType(const FString& TypeName, bool* bOutFellBack = nullptr);
	static FNiagaraVariable MakeUserVariable(const FString& ParamName, const FNiagaraTypeDefinition& TypeDef);
	static FString JsonObjectToString(const TSharedRef<FJsonObject>& JsonObj);
	static FString JsonArrayToString(const TArray<TSharedPtr<FJsonValue>>& JsonArray);
	static FString JsonValueToString(const TSharedPtr<FJsonValue>& Value);

	// DI override resolution helper — walks override pin upstream to find the DI UObject
	static UNiagaraDataInterface* FindDIFromOverridePin(UNiagaraNodeFunctionCall* ModuleNode, const FName& MatchedFullName, const FNiagaraTypeDefinition& InputType);

	// Shared helper: applies a JSON spec (emitters, user params, renderers, modules) to an existing system.
	// Used by both create_system_from_spec and import_system_spec.
	static int32 ApplySpecToSystem(UNiagaraSystem* System, const FString& SystemPath,
		const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors);

	// HLSL script creation helper
	static FMonolithActionResult CreateScriptFromHLSL(const TSharedPtr<FJsonObject>& Params, ENiagaraScriptUsage Usage);
};
