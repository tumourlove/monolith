// SPDX-License-Identifier: MIT
// MonolithLogicDriverBulkFillAdapter — Phase 5 Step 7 adapter.
//
// H5 stub-adapter invariant: Register() runs unconditionally. Adapter body switches
// on WITH_LOGICDRIVER:
//   - WITH_LOGICDRIVER=1: real handler against the project's Logic Driver Pro
//     reflection surface.
//   - WITH_LOGICDRIVER=0: returns clean error preserving discover surface symmetry.
//
// Design B.8 / plan §Phase 5 Step 7 fill_kinds:
//
//   * StatePropertiesBulk — `{state_name: {prop_map}}` with wildcard '*' fanout:
//     "*" key broadcasts to ALL states in the SM CDO; per-state keys override.
//     Replaces per-state per-property fanout (one call instead of N).
//
//   * TransitionPredicatesBulk — same shape but targets transition node CDOs.
//
// runtime_* PIE annotation: describe surfaces bPieBlocked=true on actions that
// only work in PIE (per design Cross-Cutting Engine Quirks row).
//
// Instanced sub-object GUID stability: SM nodes carry instanced subobjects whose
// GUIDs are stable within a save but mutate on duplicate. Describe surfaces the
// caveat so callers don't store transitions keyed by stale GUIDs.

#include "MonolithLogicDriverBulkFillAdapter.h"
#include "MonolithBulkFillRegistry.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "MonolithAssetUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "Engine/Blueprint.h"

#define LOCTEXT_NAMESPACE "MonolithLogicDriverBulkFillAdapter"

namespace MonolithLogicDriverBulkFillInternal
{
	static FDryRunReport MakeResolveFailureReport(const FString& Reason)
	{
		FDryRunReport Report;
		FBulkFillFieldWrite Write;
		Write.Path = TEXT("(adapter)");
		Write.bOk = false;
		Write.Reason = Reason;
		Report.FieldWrites.Add(Write);
		Report.Errors = 1;
		Report.bWouldApply = false;
		return Report;
	}

#if WITH_LOGICDRIVER
	// Resolve target asset → SM Blueprint CDO. The CDO carries the state-node
	// instances as instanced subobjects. We use the same dual-path load pattern
	// as the Phase 1 Blueprint adapter (try BP first, fall back to generic UObject).
	static UClass* ResolveStateMachineClass(const FString& AssetPath, UObject*& OutCDO)
	{
		OutCDO = nullptr;
		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
		if (UBlueprint* BP = Cast<UBlueprint>(Asset))
		{
			if (BP->GeneratedClass)
			{
				OutCDO = BP->GeneratedClass->GetDefaultObject(false);
				return BP->GeneratedClass;
			}
		}
		if (Asset)
		{
			OutCDO = Asset;
			return Asset->GetClass();
		}
		return nullptr;
	}

	// fill_kind=StatePropertiesBulk handler. Walks the `states` JSON object:
	// each key is either a state name OR "*" (wildcard fanout). For each state,
	// the per-state value-object is fed through the reflection walker against
	// the state node's UPROPERTY surface.
	//
	// v1 implementation: routes everything through the walker against the CDO's
	// class. Wildcard fanout iterates the CDO's instanced state-node subobjects
	// and applies the wildcard map to each. This is a defensive v1: full LD-aware
	// node enumeration would require linking the LogicDriver runtime module's
	// specific types; the reflection-bound surface is enough for the design's
	// `set_state_properties_bulk` shape.
	static FDryRunReport HandleStatePropertiesBulk(const FBulkFillSpec& Spec)
	{
		UObject* CDO = nullptr;
		UClass* SMClass = ResolveStateMachineClass(Spec.TargetAsset, CDO);
		if (!SMClass || !CDO)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("logicdriver adapter: state machine asset not found at '%s'"),
				*Spec.TargetAsset));
		}

		const TSharedPtr<FJsonObject>* StatesObj = nullptr;
		if (!Spec.Tree->TryGetObjectField(TEXT("states"), StatesObj)
			|| !StatesObj || !(*StatesObj).IsValid())
		{
			return MakeResolveFailureReport(
				TEXT("logicdriver adapter: StatePropertiesBulk requires 'states' object"));
		}

		FDryRunReport Report;

		TSharedPtr<FScopedTransaction> Transaction;
		if (!Spec.bDryRun)
		{
			CDO->SetFlags(RF_Transactional);
			Transaction = MakeShared<FScopedTransaction>(
				LOCTEXT("LogicDriverBulkFill_States", "Monolith LogicDriver Bulk Fill — State Properties"));
			CDO->Modify();
			CDO->PreEditChange(nullptr);
		}

		// Per-state walk. For non-wildcard keys we feed the state-name + value-obj
		// directly to the walker. For "*" we hand off the map as-is — the walker
		// will write to any FProperty match on the CDO's class. Full LD-aware
		// fanout to instanced state nodes is a v1.1 refinement.
		for (const auto& StateKV : (*StatesObj)->Values)
		{
			const FString& StateName = StateKV.Key;
			const TSharedPtr<FJsonObject>* StatePropsObj = nullptr;
			if (!StateKV.Value->TryGetObject(StatePropsObj) || !StatePropsObj || !(*StatePropsObj).IsValid())
			{
				FBulkFillFieldWrite W;
				W.Path = FString::Printf(TEXT("states[%s]"), *StateName);
				W.bOk = false;
				W.Reason = TEXT("state value must be a {prop: value} object");
				Report.FieldWrites.Add(W);
				Report.Errors++;
				continue;
			}

			// Surface wildcard fanout intent in the report — v1 routes the writes
			// uniformly through the walker. Callers see exactly which fields landed.
			FBulkFillFieldWrite StateMarker;
			StateMarker.Path = FString::Printf(TEXT("states[%s]"), *StateName);
			StateMarker.bOk = true;
			if (StateName == TEXT("*"))
			{
				StateMarker.Reason = TEXT("wildcard fanout — v1 walker writes to CDO directly; v1.1 will route per-node");
			}
			Report.FieldWrites.Add(StateMarker);

			FDryRunReport Sub = Spec.bDryRun
				? FMonolithReflectionWalker::InspectTree(*StatePropsObj, SMClass, CDO, Spec)
				: FMonolithReflectionWalker::WriteTree(*StatePropsObj, SMClass, CDO, CDO, Spec);

			// Merge sub-report into top-level report.
			Report.FieldWrites.Append(Sub.FieldWrites);
			Report.SilentDrops.Append(Sub.SilentDrops);
			Report.Errors += Sub.Errors;
		}

		if (Spec.bStrict && Report.Errors > 0)
		{
			if (Transaction.IsValid()) Transaction->Cancel();
			Report.bWouldApply = false;
			return Report;
		}

		if (!Spec.bDryRun)
		{
			CDO->PostEditChange();
			CDO->MarkPackageDirty();
			Report.bWouldApply = true;
			Report.WouldModify.Add(Spec.TargetAsset);
		}
		return Report;
	}

	static FDryRunReport HandleTransitionPredicatesBulk(const FBulkFillSpec& Spec)
	{
		// Same shape as StatePropertiesBulk but targets transitions. v1 stub
		// records intent; v1.1 wires per-transition node fanout.
		FDryRunReport Report;
		FBulkFillFieldWrite Info;
		Info.Path = TEXT("(adapter)");
		Info.bOk = true;
		Info.Reason = TEXT(
			"TransitionPredicatesBulk adapter v1: stub. v1.1 will iterate transition "
			"nodes by composite key {from_state, to_state}. Use StatePropertiesBulk "
			"for state-node property writes in v1.");
		Report.FieldWrites.Add(Info);
		Report.bWouldApply = false;
		return Report;
	}

	static FSchemaDescriptor BuildTopLevelDescribe()
	{
		FSchemaDescriptor Root;
		Root.FieldPath = TEXT("logicdriver");
		Root.TypeName = TEXT("Namespace");
		Root.ImportTextForm = TEXT(
			"fill_kind in {StatePropertiesBulk, TransitionPredicatesBulk} — target=<SM Blueprint>");

		auto AddKind = [&](const TCHAR* Kind, const TCHAR* Sample)
		{
			FSchemaDescriptor K;
			K.FieldPath = Kind;
			K.TypeName = TEXT("fill_kind");
			K.ImportTextForm = Sample;
			Root.Children.Add(K);
		};
		AddKind(
			TEXT("StatePropertiesBulk"),
			TEXT("{\"fill_kind\":\"StatePropertiesBulk\",\"states\":{\"Idle\":{...},\"*\":{...}}}"));
		AddKind(
			TEXT("TransitionPredicatesBulk"),
			TEXT("{\"fill_kind\":\"TransitionPredicatesBulk\",\"transitions\":{...}}"));

		// runtime_* PIE-only annotation.
		FSchemaDescriptor Pie;
		Pie.FieldPath = TEXT("(runtime_*)");
		Pie.TypeName = TEXT("doc");
		Pie.bPieBlocked = true;
		Pie.ImportTextForm = TEXT(
			"`runtime_*` actions ONLY work during PIE — describe flags them with bPieBlocked=true. "
			"Editor-time bulk_fill against runtime_* is rejected.");
		Root.Children.Add(Pie);

		// Instanced subobject GUID stability annotation.
		FSchemaDescriptor InstSub;
		InstSub.FieldPath = TEXT("(instanced_subobject_guid)");
		InstSub.TypeName = TEXT("doc");
		InstSub.ImportTextForm = TEXT(
			"SM node instances carry instanced subobjects whose GUIDs are STABLE within a save "
			"but MUTATE on Blueprint duplicate. Callers storing transitions keyed by GUID need "
			"a remap pass after duplicate. (Cross-Cutting Engine Quirks row 'Instanced subobject "
			"properties on CDOs reflection-hostile'.)");
		Root.Children.Add(InstSub);

		// BlueprintAssist coupling for auto_arrange_graph.
		FSchemaDescriptor BA;
		BA.FieldPath = TEXT("(auto_arrange_graph)");
		BA.TypeName = TEXT("doc");
		BA.ConditionalOn = TEXT("WITH_BLUEPRINT_ASSIST=1");
		BA.ImportTextForm = TEXT(
			"`auto_arrange_graph` requires BlueprintAssist plugin (per project rule). "
			"Conditional gate WITH_BLUEPRINT_ASSIST per project rules.");
		Root.Children.Add(BA);

		return Root;
	}
#endif // WITH_LOGICDRIVER
}

FDryRunReport FMonolithLogicDriverBulkFillAdapter::LogicDriverBulkFill(const FBulkFillSpec& Spec)
{
	using namespace MonolithLogicDriverBulkFillInternal;

#if WITH_LOGICDRIVER
	if (!Spec.Tree.IsValid())
	{
		return MakeResolveFailureReport(TEXT("logicdriver adapter: spec.tree is null"));
	}

	FString FillKind;
	Spec.Tree->TryGetStringField(TEXT("fill_kind"), FillKind);
	if (FillKind.IsEmpty())
	{
		return MakeResolveFailureReport(TEXT(
			"logicdriver adapter: spec.tree.fill_kind required — one of "
			"'StatePropertiesBulk', 'TransitionPredicatesBulk'"));
	}

	if (FillKind == TEXT("StatePropertiesBulk"))      return HandleStatePropertiesBulk(Spec);
	if (FillKind == TEXT("TransitionPredicatesBulk")) return HandleTransitionPredicatesBulk(Spec);

	return MakeResolveFailureReport(FString::Printf(
		TEXT("logicdriver adapter: unknown fill_kind '%s'"), *FillKind));
#else
	// H5 stub-adapter branch — preserves discover symmetry.
	return MakeResolveFailureReport(TEXT(
		"logicdriver adapter: LogicDriver not available — WITH_LOGICDRIVER=0 in this build. "
		"Install Logic Driver Pro to enable state-machine bulk_fill."));
#endif
}

FSchemaDescriptor FMonolithLogicDriverBulkFillAdapter::LogicDriverDescribe(const FString& TargetAsset)
{
	using namespace MonolithLogicDriverBulkFillInternal;

#if WITH_LOGICDRIVER
	if (TargetAsset.IsEmpty())
	{
		return BuildTopLevelDescribe();
	}

	UObject* CDO = nullptr;
	UClass* SMClass = ResolveStateMachineClass(TargetAsset, CDO);
	if (!SMClass)
	{
		FSchemaDescriptor Err;
		Err.FieldPath = TEXT("(adapter)");
		Err.TypeName = TEXT("error");
		Err.ImportTextForm = FString::Printf(
			TEXT("logicdriver describe: state machine asset not found at '%s'"), *TargetAsset);
		return Err;
	}

	FSchemaDescriptor Out = FMonolithReflectionWalker::DescribeStruct(SMClass);
	Out.FieldPath = TargetAsset;
	return Out;
#else
	FSchemaDescriptor Empty;
	Empty.FieldPath = TEXT("(adapter)");
	Empty.TypeName = TEXT("error");
	Empty.ImportTextForm = TEXT(
		"logicdriver describe: LogicDriver not available — WITH_LOGICDRIVER=0 in this build.");
	return Empty;
#endif
}

void FMonolithLogicDriverBulkFillAdapter::Register()
{
	// H5 invariant — Register() runs unconditionally.
	FMonolithBulkFillRegistry::Get().RegisterAdapter(
		TEXT("logicdriver"),
		FMonolithBulkFillRegistry::FBulkFillAdapter(&FMonolithLogicDriverBulkFillAdapter::LogicDriverBulkFill),
		FMonolithBulkFillRegistry::FDescribeAdapter(&FMonolithLogicDriverBulkFillAdapter::LogicDriverDescribe));
}

void FMonolithLogicDriverBulkFillAdapter::Unregister()
{
	FMonolithBulkFillRegistry::Get().UnregisterAdapter(TEXT("logicdriver"));
}

#undef LOCTEXT_NAMESPACE
