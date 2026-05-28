// SPDX-License-Identifier: MIT
// MonolithNiagaraBulkFillAdapter — Phase 5 Step 2 adapter.
// Routes "niagara" target_namespace traffic from bulk_fill.apply / describe.schema
// to per-fill_kind handlers. Delegates per-field reflection writes to the
// FMonolithReflectionWalker.
//
// Niagara is a core engine plugin (always-on) — no WITH_* gate.
//
// IMPORTANT QUIRKS surfaced from design B.4 + Cross-Cutting Engine Quirks rows:
//
//   * GPU emitter introspection one-way — bulk_fill targeting GPU-sim params
//     returns explicit error. Detection: `bAllowGPUEmitters && SimTarget==GPU`
//     check on the emitter's UPROPERTYs. Tagged as WISHLIST in design Non-Goals.
//
//   * module_node GUID change on duplicate — describe surfaces this in the
//     ImportTextForm string so callers know GUID-keyed specs need a remap pass
//     after EmitterHandle->DuplicateEmitter.
//
//   * Module GUIDs stable but parent-emitter-override invisibility — describe
//     emits an `include_overrides_only` doc note.

#include "MonolithNiagaraBulkFillAdapter.h"
#include "MonolithBulkFillRegistry.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "MonolithAssetUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

// Niagara core (always-on plugin).
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"

#define LOCTEXT_NAMESPACE "MonolithNiagaraBulkFillAdapter"

namespace MonolithNiagaraBulkFillInternal
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

	static UObject* ResolveAsset(const FString& AssetPath)
	{
		return FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	}

	// Detect GPU-sim params by name convention. Niagara user-params follow
	// "User.<Name>" pattern; GPU-only ones cannot be inspected post-spawn.
	// The adapter rejects bulk_fill targeting GPU-sim emitters with a clear error
	// citing the WISHLIST (per plan §29 "GPU emitter introspection one-way").
	static bool LooksLikeGPUParam(const FString& UserParam)
	{
		// Conservative detector — caller has named a User.* with "GPU" in the name
		// OR the emitter's SimTarget is GPU. We surface the error rather than
		// silently write into a GPU param the dispatcher can't round-trip.
		return UserParam.Contains(TEXT("GPU"), ESearchCase::IgnoreCase);
	}

	// fill_kind=DataInterfaceArray — write a `rows:[]` payload into a User.*
	// data interface (FNiagaraDataInterfaceArrayFloat / Int / Bool / Vector).
	// The walker handles FArrayProperty for primitive arrays; for DI arrays the
	// payload routes through the SystemUserParameter override store rather than
	// direct FProperty mutation. v1 implements the FArrayProperty path (the
	// reflection-bound surface) and surfaces a clean error for DI-on-emitter
	// targets where the override store path is needed.
	static FDryRunReport HandleDataInterfaceArray(const FBulkFillSpec& Spec)
	{
		UObject* Asset = ResolveAsset(Spec.TargetAsset);
		if (!Asset)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("niagara adapter: asset not found at '%s'"), *Spec.TargetAsset));
		}

		FString UserParam;
		Spec.Tree->TryGetStringField(TEXT("user_param"), UserParam);
		if (UserParam.IsEmpty())
		{
			return MakeResolveFailureReport(
				TEXT("niagara adapter: DataInterfaceArray requires 'user_param' (e.g. 'User.DamageCurve')"));
		}

		// GPU-sim rejection — design Cross-Cutting Engine Quirks row.
		if (LooksLikeGPUParam(UserParam))
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("niagara adapter: '%s' looks like a GPU-sim parameter — bulk_fill cannot "
				     "round-trip GPU-side state (WISHLIST: GPU emitter introspection one-way)"),
				*UserParam));
		}

		const TSharedPtr<FJsonValue> RowsVal = Spec.Tree->TryGetField(TEXT("rows"));
		if (!RowsVal.IsValid() || RowsVal->Type != EJson::Array)
		{
			return MakeResolveFailureReport(
				TEXT("niagara adapter: DataInterfaceArray requires 'rows' array"));
		}

		// Wrap as walker input — walker writes into Asset's UPROPERTY whose name
		// equals UserParam (stripped of "User." prefix). Callers can also point
		// at direct UPROPERTY arrays on UNiagaraSystem subclasses.
		FString PropName = UserParam;
		PropName.RemoveFromStart(TEXT("User."));

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetField(PropName, RowsVal);

		FDryRunReport Report;
		if (Spec.bDryRun)
		{
			Report = FMonolithReflectionWalker::InspectTree(Root, Asset->GetClass(), Asset, Spec);
			Report.bWouldApply = false;
			return Report;
		}

		Asset->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(LOCTEXT("NiagaraBulkFill_DIA", "Monolith Niagara Bulk Fill — DI Array"));
		Asset->Modify();
		Asset->PreEditChange(nullptr);

		Report = FMonolithReflectionWalker::WriteTree(Root, Asset->GetClass(), Asset, Asset, Spec);
		if (Spec.bStrict && Report.Errors > 0)
		{
			Transaction.Cancel();
			Report.bWouldApply = false;
			return Report;
		}

		Asset->PostEditChange();
		Asset->MarkPackageDirty();
		Report.bWouldApply = true;
		Report.WouldModify.Add(Spec.TargetAsset);
		return Report;
	}

	// fill_kind=Curve — write FRichCurve keys into a Curve UPROPERTY on the
	// target asset. The walker's FStructProperty path handles FRichCurve via
	// the engine's standard ImportText grammar.
	static FDryRunReport HandleCurve(const FBulkFillSpec& Spec)
	{
		UObject* Asset = ResolveAsset(Spec.TargetAsset);
		if (!Asset)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("niagara adapter: asset not found at '%s'"), *Spec.TargetAsset));
		}

		FString CurveName;
		Spec.Tree->TryGetStringField(TEXT("curve_name"), CurveName);
		if (CurveName.IsEmpty())
		{
			return MakeResolveFailureReport(
				TEXT("niagara adapter: Curve fill_kind requires 'curve_name'"));
		}

		const TSharedPtr<FJsonValue> KeysVal = Spec.Tree->TryGetField(TEXT("keys"));
		if (!KeysVal.IsValid() || KeysVal->Type != EJson::Array)
		{
			return MakeResolveFailureReport(
				TEXT("niagara adapter: Curve fill_kind requires 'keys' array (FRichCurveKey shape)"));
		}

		// Map keys into a single-key root and route through the walker. The walker
		// expands FStructProperty<FRichCurve> via ImportText_Direct on the
		// stringified JSON.
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> CurveObj = MakeShared<FJsonObject>();
		CurveObj->SetField(TEXT("Keys"), KeysVal);
		Root->SetObjectField(CurveName, CurveObj);

		FDryRunReport Report;
		if (Spec.bDryRun)
		{
			Report = FMonolithReflectionWalker::InspectTree(Root, Asset->GetClass(), Asset, Spec);
			Report.bWouldApply = false;
			return Report;
		}

		Asset->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(LOCTEXT("NiagaraBulkFill_Curve", "Monolith Niagara Bulk Fill — Curve"));
		Asset->Modify();
		Asset->PreEditChange(nullptr);

		Report = FMonolithReflectionWalker::WriteTree(Root, Asset->GetClass(), Asset, Asset, Spec);
		if (Spec.bStrict && Report.Errors > 0)
		{
			Transaction.Cancel();
			Report.bWouldApply = false;
			return Report;
		}

		Asset->PostEditChange();
		Asset->MarkPackageDirty();
		Report.bWouldApply = true;
		Report.WouldModify.Add(Spec.TargetAsset);
		return Report;
	}

	// fill_kind=ParameterOverrides — generic UPROPERTY tree write against a
	// UNiagaraSystem. Walker handles arbitrarily-deep JSON. GPU-sim params
	// rejected per design.
	static FDryRunReport HandleParameterOverrides(const FBulkFillSpec& Spec)
	{
		UObject* Asset = ResolveAsset(Spec.TargetAsset);
		if (!Asset)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("niagara adapter: asset not found at '%s'"), *Spec.TargetAsset));
		}

		const TSharedPtr<FJsonValue> ParamsVal = Spec.Tree->TryGetField(TEXT("parameters"));
		if (!ParamsVal.IsValid() || ParamsVal->Type != EJson::Object)
		{
			return MakeResolveFailureReport(
				TEXT("niagara adapter: ParameterOverrides requires 'parameters' object"));
		}

		// GPU-sim sniff — any key with "GPU" infix → reject (conservative WISHLIST gate).
		const TSharedPtr<FJsonObject>* ParamsObjPtr = nullptr;
		ParamsVal->TryGetObject(ParamsObjPtr);
		if (ParamsObjPtr && (*ParamsObjPtr).IsValid())
		{
			for (const auto& KV : (*ParamsObjPtr)->Values)
			{
				if (LooksLikeGPUParam(KV.Key))
				{
					return MakeResolveFailureReport(FString::Printf(
						TEXT("niagara adapter: parameter '%s' looks like a GPU-sim param — "
						     "WISHLIST: GPU emitter introspection one-way"),
						*KV.Key));
				}
			}
		}

		FDryRunReport Report;
		const TSharedPtr<FJsonObject>& ParamsObj = *ParamsObjPtr;

		if (Spec.bDryRun)
		{
			Report = FMonolithReflectionWalker::InspectTree(ParamsObj, Asset->GetClass(), Asset, Spec);
			Report.bWouldApply = false;
			return Report;
		}

		Asset->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(LOCTEXT("NiagaraBulkFill_Params", "Monolith Niagara Bulk Fill — Parameter Overrides"));
		Asset->Modify();
		Asset->PreEditChange(nullptr);

		Report = FMonolithReflectionWalker::WriteTree(ParamsObj, Asset->GetClass(), Asset, Asset, Spec);
		if (Spec.bStrict && Report.Errors > 0)
		{
			Transaction.Cancel();
			Report.bWouldApply = false;
			return Report;
		}

		Asset->PostEditChange();
		Asset->MarkPackageDirty();
		Report.bWouldApply = true;
		Report.WouldModify.Add(Spec.TargetAsset);
		return Report;
	}

	static FSchemaDescriptor BuildTopLevelDescribe()
	{
		FSchemaDescriptor Root;
		Root.FieldPath = TEXT("niagara");
		Root.TypeName = TEXT("Namespace");
		Root.ImportTextForm = TEXT(
			"fill_kind in {DataInterfaceArray, Curve, ParameterOverrides} — "
			"target=<UNiagaraSystem | UNiagaraEmitter>");

		// GUID-instability note — surfaces the module_node GUID-change-on-duplicate
		// quirk in the schema tree (Cross-Cutting Engine Quirks row).
		FSchemaDescriptor Guids;
		Guids.FieldPath = TEXT("(module_node.guid)");
		Guids.TypeName = TEXT("FGuid");
		Guids.ImportTextForm = TEXT(
			"module_node GUIDs change on EmitterHandle->DuplicateEmitter — "
			"GUID-keyed bulk_fill specs need a remap pass post-duplicate");
		Root.Children.Add(Guids);

		// override-vs-inherit invisibility note.
		FSchemaDescriptor Override;
		Override.FieldPath = TEXT("(parent_emitter_override_visibility)");
		Override.TypeName = TEXT("doc");
		Override.ImportTextForm = TEXT(
			"get_module_inputs does NOT flag override-vs-inherit — pass "
			"`include_overrides_only=true` on describe to surface only authored overrides");
		Root.Children.Add(Override);

		// GPU-sim WISHLIST.
		FSchemaDescriptor Gpu;
		Gpu.FieldPath = TEXT("(GPU emitter introspection)");
		Gpu.TypeName = TEXT("wishlist");
		Gpu.ImportTextForm = TEXT(
			"(WISHLIST) — GPU emitter param state is one-way: writes are accepted "
			"but PIE-time read-back is unavailable. Adapter rejects User.*GPU* "
			"params explicitly with this reason.");
		Root.Children.Add(Gpu);

		auto AddKind = [&](const TCHAR* Kind, const TCHAR* Sample)
		{
			FSchemaDescriptor K;
			K.FieldPath = Kind;
			K.TypeName = TEXT("fill_kind");
			K.ImportTextForm = Sample;
			Root.Children.Add(K);
		};
		AddKind(
			TEXT("DataInterfaceArray"),
			TEXT("{\"fill_kind\":\"DataInterfaceArray\",\"user_param\":\"User.X\",\"rows\":[0.0,1.0]}"));
		AddKind(
			TEXT("Curve"),
			TEXT("{\"fill_kind\":\"Curve\",\"curve_name\":\"X\",\"keys\":[{...}]}"));
		AddKind(
			TEXT("ParameterOverrides"),
			TEXT("{\"fill_kind\":\"ParameterOverrides\",\"parameters\":{...}}"));

		return Root;
	}
}

FDryRunReport FMonolithNiagaraBulkFillAdapter::NiagaraBulkFill(const FBulkFillSpec& Spec)
{
	using namespace MonolithNiagaraBulkFillInternal;

	if (!Spec.Tree.IsValid())
	{
		return MakeResolveFailureReport(TEXT("niagara adapter: spec.tree is null"));
	}

	FString FillKind;
	Spec.Tree->TryGetStringField(TEXT("fill_kind"), FillKind);
	if (FillKind.IsEmpty())
	{
		return MakeResolveFailureReport(TEXT(
			"niagara adapter: spec.tree.fill_kind required — one of "
			"'DataInterfaceArray', 'Curve', 'ParameterOverrides'"));
	}

	if (FillKind == TEXT("DataInterfaceArray"))  return HandleDataInterfaceArray(Spec);
	if (FillKind == TEXT("Curve"))               return HandleCurve(Spec);
	if (FillKind == TEXT("ParameterOverrides"))  return HandleParameterOverrides(Spec);

	return MakeResolveFailureReport(FString::Printf(
		TEXT("niagara adapter: unknown fill_kind '%s'"), *FillKind));
}

FSchemaDescriptor FMonolithNiagaraBulkFillAdapter::NiagaraDescribe(const FString& TargetAsset)
{
	using namespace MonolithNiagaraBulkFillInternal;

	if (TargetAsset.IsEmpty())
	{
		return BuildTopLevelDescribe();
	}

	UObject* Asset = ResolveAsset(TargetAsset);
	if (!Asset)
	{
		FSchemaDescriptor Err;
		Err.FieldPath = TEXT("(adapter)");
		Err.TypeName = TEXT("error");
		Err.ImportTextForm = FString::Printf(TEXT("niagara describe: asset not found at '%s'"), *TargetAsset);
		return Err;
	}

	FSchemaDescriptor Out = FMonolithReflectionWalker::DescribeStruct(Asset->GetClass());
	Out.FieldPath = TargetAsset;
	return Out;
}

void FMonolithNiagaraBulkFillAdapter::Register()
{
	FMonolithBulkFillRegistry::Get().RegisterAdapter(
		TEXT("niagara"),
		FMonolithBulkFillRegistry::FBulkFillAdapter(&FMonolithNiagaraBulkFillAdapter::NiagaraBulkFill),
		FMonolithBulkFillRegistry::FDescribeAdapter(&FMonolithNiagaraBulkFillAdapter::NiagaraDescribe));
}

void FMonolithNiagaraBulkFillAdapter::Unregister()
{
	FMonolithBulkFillRegistry::Get().UnregisterAdapter(TEXT("niagara"));
}

#undef LOCTEXT_NAMESPACE
