#include "MonolithNiagaraLayoutActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "IMonolithGraphFormatter.h"

#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraCommon.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithNiagaraLayout, Log, All);

// ============================================================================
//  Registration
// ============================================================================

void FMonolithNiagaraLayoutActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("niagara"), TEXT("auto_layout"),
		TEXT("**Experimental.** Auto-layout nodes in a Niagara graph using Blueprint Assist. "
			"Layout quality depends on graph complexity. Best results with simple module graphs. "
			"Asset must be open in the editor. No built-in Monolith formatter exists for Niagara graphs."),
		FMonolithActionHandler::CreateStatic(&HandleAutoLayout),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("NiagaraSystem asset path"))
			.Optional(TEXT("emitter"), TEXT("string"), TEXT("Filter to a specific emitter's graph (by name or handle ID)"))
			.Optional(TEXT("script_usage"), TEXT("string"),
				TEXT("Script usage filter: 'system', 'emitter', 'particle'. "
					"System spawn/update share one graph; emitter spawn/update/particle spawn/update share another per emitter. "
					"Defaults to all graphs."))
			.Optional(TEXT("formatter"), TEXT("string"),
				TEXT("Formatter: 'auto' (default, uses BA if available), 'blueprint_assist' (BA or error), "
					"'monolith' (not supported for Niagara — returns error)"),
				TEXT("auto"))
			.Build());
}

// ============================================================================
//  Helpers
// ============================================================================

namespace
{
	/** Normalize asset_path with common alias fallback */
	FString NL_GetAssetPath(const TSharedPtr<FJsonObject>& Params)
	{
		FString Path = Params->GetStringField(TEXT("asset_path"));
		if (Path.IsEmpty()) Path = Params->GetStringField(TEXT("system_path"));
		return Path;
	}

	/**
	 * Resolve script_usage string to the set of graph "slots" to format.
	 * Returns false if the string is unrecognized.
	 *
	 * Because Niagara graphs are shared:
	 *   "system"   -> system graph
	 *   "emitter"  -> each emitter graph (same graph as particle)
	 *   "particle" -> each emitter graph (same graph as emitter)
	 *   empty      -> all graphs (system + all emitter)
	 */
	struct FGraphSlot
	{
		bool bSystemGraph = false;
		bool bEmitterGraphs = false;
	};

	bool ResolveScriptUsageFilter(const FString& UsageStr, FGraphSlot& OutSlot)
	{
		if (UsageStr.IsEmpty())
		{
			OutSlot.bSystemGraph = true;
			OutSlot.bEmitterGraphs = true;
			return true;
		}

		FString Lower = UsageStr.ToLower();
		if (Lower == TEXT("system"))
		{
			OutSlot.bSystemGraph = true;
			return true;
		}
		if (Lower == TEXT("emitter") || Lower == TEXT("particle"))
		{
			OutSlot.bEmitterGraphs = true;
			return true;
		}
		// Also accept the full script usage names
		if (Lower == TEXT("module") || Lower == TEXT("dynamic_input") || Lower == TEXT("function"))
		{
			// These are module-level script types — they live within the emitter graph
			OutSlot.bEmitterGraphs = true;
			return true;
		}
		return false;
	}

	/** Find emitter handle index by name or ID string */
	int32 FindEmitterHandleIndex(UNiagaraSystem* System, const FString& NameOrId)
	{
		const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
		for (int32 i = 0; i < Handles.Num(); ++i)
		{
			if (Handles[i].GetUniqueInstanceName() == NameOrId || Handles[i].GetName().ToString() == NameOrId)
			{
				return i;
			}
			// Also match by ID GUID string
			if (Handles[i].GetId().ToString(EGuidFormats::DigitsWithHyphensLower) == NameOrId
				|| Handles[i].GetId().ToString(EGuidFormats::Digits) == NameOrId)
			{
				return i;
			}
		}
		return INDEX_NONE;
	}

	/** Get the system-level graph (shared by SystemSpawn + SystemUpdate scripts) */
	UNiagaraGraph* GetSystemGraph(UNiagaraSystem* System)
	{
		UNiagaraScript* Script = System->GetSystemSpawnScript();
		if (!Script) return nullptr;
		UNiagaraScriptSource* Src = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
		return Src ? Src->NodeGraph : nullptr;
	}

	/** Get an emitter's graph (shared by EmitterSpawn/Update + ParticleSpawn/Update) */
	UNiagaraGraph* GetEmitterGraph(UNiagaraSystem* System, int32 EmitterIndex)
	{
		const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
		if (!Handles.IsValidIndex(EmitterIndex)) return nullptr;
		FVersionedNiagaraEmitterData* ED = Handles[EmitterIndex].GetEmitterData();
		if (!ED) return nullptr;
		UNiagaraScriptSource* Src = Cast<UNiagaraScriptSource>(ED->GraphSource);
		return Src ? Src->NodeGraph : nullptr;
	}

	/** Format a single graph with BA. Returns per-graph result JSON. */
	TSharedPtr<FJsonObject> FormatOneGraph(
		IMonolithGraphFormatter& Formatter,
		UNiagaraGraph* Graph,
		const FString& GraphLabel,
		bool& bOutSuccess,
		FString& OutError)
	{
		auto GraphResult = MakeShared<FJsonObject>();
		GraphResult->SetStringField(TEXT("graph"), GraphLabel);
		GraphResult->SetStringField(TEXT("graph_class"), Graph->GetClass()->GetName());

		if (!Formatter.SupportsGraph(Graph))
		{
			GraphResult->SetBoolField(TEXT("success"), false);
			GraphResult->SetStringField(TEXT("error"), TEXT("Formatter does not support this Niagara graph type"));
			bOutSuccess = false;
			OutError = FString::Printf(TEXT("Graph '%s' not supported by formatter"), *GraphLabel);
			return GraphResult;
		}

		int32 NodesFormatted = 0;
		FString ErrorMessage;
		if (Formatter.FormatGraph(Graph, NodesFormatted, ErrorMessage))
		{
			GraphResult->SetBoolField(TEXT("success"), true);
			GraphResult->SetNumberField(TEXT("nodes_formatted"), NodesFormatted);

			FMonolithFormatterInfo Info = Formatter.GetFormatterInfo(Graph);
			GraphResult->SetStringField(TEXT("formatter_type"), Info.FormatterType);

			bOutSuccess = true;
		}
		else
		{
			GraphResult->SetBoolField(TEXT("success"), false);
			GraphResult->SetStringField(TEXT("error"), ErrorMessage);
			bOutSuccess = false;
			OutError = ErrorMessage;
		}
		return GraphResult;
	}
}

// ============================================================================
//  Handler
// ============================================================================

FMonolithActionResult FMonolithNiagaraLayoutActions::HandleAutoLayout(const TSharedPtr<FJsonObject>& Params)
{
	// --- Parse params ---
	FString AssetPath = NL_GetAssetPath(Params);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));
	}

	FString EmitterFilter = Params->GetStringField(TEXT("emitter"));
	FString ScriptUsageStr = Params->GetStringField(TEXT("script_usage"));
	FString Formatter = Params->GetStringField(TEXT("formatter"));
	if (Formatter.IsEmpty()) Formatter = TEXT("auto");

	// --- Validate formatter ---
	if (Formatter == TEXT("monolith"))
	{
		return FMonolithActionResult::Error(
			TEXT("No built-in Monolith formatter exists for Niagara graphs. "
				"Use formatter='auto' or install Blueprint Assist."));
	}

	if (Formatter != TEXT("auto") && Formatter != TEXT("blueprint_assist"))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown formatter '%s'. Valid options: 'auto', 'blueprint_assist', 'monolith'"), *Formatter));
	}

	// --- Parse script_usage filter ---
	FGraphSlot Slot;
	if (!ResolveScriptUsageFilter(ScriptUsageStr, Slot))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown script_usage '%s'. Valid: 'system', 'emitter', 'particle', 'module', 'dynamic_input', 'function'"),
			*ScriptUsageStr));
	}

	// --- Load system ---
	UNiagaraSystem* System = FMonolithAssetUtils::LoadAssetByPath<UNiagaraSystem>(AssetPath);
	if (!System)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to load Niagara system: %s"), *AssetPath));
	}

	// --- Check formatter availability ---
	bool bExplicitBA = (Formatter == TEXT("blueprint_assist"));
	bool bBAAvailable = IMonolithGraphFormatter::IsAvailable();

	if (!bBAAvailable)
	{
		if (bExplicitBA)
		{
			return FMonolithActionResult::Error(
				TEXT("Blueprint Assist formatter is not available. "
					"Install Blueprint Assist (paid marketplace plugin) and restart the editor."));
		}
		// auto mode, no BA
		return FMonolithActionResult::Error(
			TEXT("No formatter available for Niagara graphs. "
				"Install Blueprint Assist (paid marketplace plugin) for auto_layout support. "
				"No built-in Monolith formatter exists for Niagara."));
	}

	IMonolithGraphFormatter& FormatterImpl = IMonolithGraphFormatter::Get();

	// --- Collect graphs to format ---
	struct FGraphEntry
	{
		UNiagaraGraph* Graph;
		FString Label;
	};
	TArray<FGraphEntry> GraphsToFormat;

	// System graph
	if (Slot.bSystemGraph && EmitterFilter.IsEmpty())
	{
		UNiagaraGraph* SysGraph = GetSystemGraph(System);
		if (SysGraph)
		{
			GraphsToFormat.Add({ SysGraph, TEXT("System") });
		}
	}

	// Emitter graphs
	if (Slot.bEmitterGraphs)
	{
		const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

		if (!EmitterFilter.IsEmpty())
		{
			// Specific emitter
			int32 Idx = FindEmitterHandleIndex(System, EmitterFilter);
			if (Idx == INDEX_NONE)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Emitter '%s' not found in system '%s'"), *EmitterFilter, *AssetPath));
			}
			UNiagaraGraph* EGraph = GetEmitterGraph(System, Idx);
			if (EGraph)
			{
				GraphsToFormat.Add({ EGraph, Handles[Idx].GetUniqueInstanceName() });
			}
		}
		else
		{
			// All emitters
			for (int32 i = 0; i < Handles.Num(); ++i)
			{
				UNiagaraGraph* EGraph = GetEmitterGraph(System, i);
				if (EGraph)
				{
					// Deduplicate — system + emitter might alias if somehow the same graph object
					bool bAlready = false;
					for (const FGraphEntry& Existing : GraphsToFormat)
					{
						if (Existing.Graph == EGraph)
						{
							bAlready = true;
							break;
						}
					}
					if (!bAlready)
					{
						GraphsToFormat.Add({ EGraph, Handles[i].GetUniqueInstanceName() });
					}
				}
			}
		}
	}

	if (GraphsToFormat.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No graphs found matching the specified filters"));
	}

	// --- Format each graph ---
	TArray<TSharedPtr<FJsonValue>> GraphResults;
	int32 TotalNodesFormatted = 0;
	int32 SuccessCount = 0;
	int32 FailCount = 0;
	FString LastError;

	for (const FGraphEntry& Entry : GraphsToFormat)
	{
		bool bSuccess = false;
		FString Error;
		auto GraphResult = FormatOneGraph(FormatterImpl, Entry.Graph, Entry.Label, bSuccess, Error);

		if (bSuccess)
		{
			++SuccessCount;
			double N = 0;
			GraphResult->TryGetNumberField(TEXT("nodes_formatted"), N);
			TotalNodesFormatted += static_cast<int32>(N);
		}
		else
		{
			++FailCount;
			LastError = Error;
		}

		GraphResults.Add(MakeShared<FJsonValueObject>(GraphResult));
	}

	// --- Build response ---
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("formatter_used"), TEXT("blueprint_assist"));
	Result->SetNumberField(TEXT("graphs_processed"), GraphsToFormat.Num());
	Result->SetNumberField(TEXT("graphs_succeeded"), SuccessCount);
	Result->SetNumberField(TEXT("graphs_failed"), FailCount);
	Result->SetNumberField(TEXT("total_nodes_formatted"), TotalNodesFormatted);
	Result->SetArrayField(TEXT("graphs"), GraphResults);

	if (FailCount > 0 && SuccessCount == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("All %d graph(s) failed to format. Last error: %s"), FailCount, *LastError));
	}

	if (FailCount > 0)
	{
		Result->SetStringField(TEXT("warning"), FString::Printf(
			TEXT("%d of %d graph(s) failed to format"), FailCount, GraphsToFormat.Num()));
	}

	return FMonolithActionResult::Success(Result);
}
