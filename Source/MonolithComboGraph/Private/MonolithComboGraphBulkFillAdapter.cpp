// SPDX-License-Identifier: MIT
// MonolithComboGraphBulkFillAdapter — Phase 5 Step 8 adapter (LAST).
//
// H5 stub-adapter invariant: Register() runs unconditionally. Adapter body
// switches on WITH_COMBOGRAPH:
//   - WITH_COMBOGRAPH=1: real reflection-walker handler for effect-container
//     and edge fields per design B.9.
//   - WITH_COMBOGRAPH=0: returns clean "ComboGraph not available" error so
//     `monolith_discover("combograph")` action surface stays identical across
//     dev + release builds.
//
// **TargetType lock (Step 8 post-review invariant):**
// `TargetType` is NOT a UPROPERTY — per design Cross-Cutting Engine Quirks row,
// it requires custom serialisation. Writes targeting `TargetType` MUST return
// an EXPLICIT unsupported-field error pointing at the v1.1 custom-serialisation
// hook. NOT a silent no-op. The error reason names the future hook so callers
// have a clear pointer to the migration path.
//
// EdGraph WITH_EDITORONLY_DATA quirk: `layout_combo_graph` silently no-ops
// unless the asset has been opened in the editor at least once (per design
// Cross-Cutting Engine Quirks row "EdGraph is WITH_EDITORONLY_DATA, lazily
// materialises"). Describe surfaces this annotation in the schema tree.

#include "MonolithComboGraphBulkFillAdapter.h"
#include "MonolithBulkFillRegistry.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "MonolithAssetUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "MonolithComboGraphBulkFillAdapter"

namespace MonolithComboGraphBulkFillInternal
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

	// The explicit unsupported-field error for TargetType writes. Load-bearing
	// per Step 8 post-review lock. Pointer to v1.1 hook is part of the message.
	static FString MakeTargetTypeUnsupportedReason()
	{
		return TEXT(
			"combograph adapter: TargetType field is NOT a UPROPERTY — bulk_fill cannot "
			"reflection-write this field (per design Cross-Cutting Engine Quirks row). "
			"v1.1 custom-serialisation hook is the future fix; tracked as the post-Phase-5 "
			"WISHLIST item 'ComboGraph TargetType custom serialiser'. Use existing "
			"combograph_query actions for TargetType writes in the meantime. "
			"THIS IS AN EXPLICIT ERROR — not a silent no-op.");
	}

#if WITH_COMBOGRAPH
	// Scan a JSON object recursively for "TargetType" keys. Returns the dotted
	// path of the first occurrence, or empty string if none. Used to emit the
	// explicit unsupported-field error from the BulkFill dispatcher BEFORE any
	// reflection writes land.
	static FString FindTargetTypeKey(const TSharedPtr<FJsonObject>& Obj, const FString& Path)
	{
		if (!Obj.IsValid()) return FString();
		for (const auto& KV : Obj->Values)
		{
			if (KV.Key.Equals(TEXT("TargetType"), ESearchCase::IgnoreCase))
			{
				return Path.IsEmpty()
					? KV.Key
					: FString::Printf(TEXT("%s.%s"), *Path, *KV.Key);
			}
			const TSharedPtr<FJsonObject>* SubObj = nullptr;
			if (KV.Value->TryGetObject(SubObj) && SubObj && (*SubObj).IsValid())
			{
				const FString Found = FindTargetTypeKey(
					*SubObj,
					Path.IsEmpty() ? KV.Key : FString::Printf(TEXT("%s.%s"), *Path, *KV.Key));
				if (!Found.IsEmpty()) return Found;
			}
			else if (KV.Value->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& Arr = KV.Value->AsArray();
				for (int32 i = 0; i < Arr.Num(); ++i)
				{
					const TSharedPtr<FJsonObject>* ArrObj = nullptr;
					if (Arr[i]->TryGetObject(ArrObj) && ArrObj && (*ArrObj).IsValid())
					{
						const FString Found = FindTargetTypeKey(
							*ArrObj,
							FString::Printf(TEXT("%s%s[%d]"),
								*Path,
								Path.IsEmpty() ? TEXT("") : TEXT("."),
								i));
						if (!Found.IsEmpty()) return Found;
					}
				}
			}
		}
		return FString();
	}

	// fill_kind=EffectContainers handler. Routes the JSON tree through the
	// reflection walker, but pre-scans for TargetType keys and rejects with the
	// EXPLICIT unsupported-field error if found.
	static FDryRunReport HandleEffectContainers(const FBulkFillSpec& Spec)
	{
		// TargetType pre-scan — explicit unsupported-field error per Step 8 lock.
		const FString TargetTypePath = FindTargetTypeKey(Spec.Tree, FString());
		if (!TargetTypePath.IsEmpty())
		{
			FDryRunReport Report;
			FBulkFillFieldWrite W;
			W.Path = TargetTypePath;
			W.bOk = false;
			W.Reason = MakeTargetTypeUnsupportedReason();
			Report.FieldWrites.Add(W);
			Report.Errors = 1;
			Report.bWouldApply = false;
			return Report;
		}

		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(Spec.TargetAsset);
		if (!Asset)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("combograph adapter: asset not found at '%s'"), *Spec.TargetAsset));
		}

		const TSharedPtr<FJsonValue> ContainersVal = Spec.Tree->TryGetField(TEXT("containers"));
		if (!ContainersVal.IsValid())
		{
			return MakeResolveFailureReport(
				TEXT("combograph adapter: EffectContainers requires 'containers' field"));
		}

		// Wrap as walker root — walker writes the array into the asset's
		// "Containers" or "EffectContainers" UPROPERTY (case-insensitive forwarding).
		FString ContainersField = TEXT("EffectContainers");
		Spec.Tree->TryGetStringField(TEXT("containers_field"), ContainersField);
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetField(ContainersField, ContainersVal);

		FDryRunReport Report;
		if (Spec.bDryRun)
		{
			Report = FMonolithReflectionWalker::InspectTree(Root, Asset->GetClass(), Asset, Spec);
			Report.bWouldApply = false;
			return Report;
		}

		Asset->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(LOCTEXT("ComboGraphBulkFill_EC", "Monolith ComboGraph Bulk Fill — Effect Containers"));
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

	// fill_kind=Edges handler. Edges keyed by composite {from_id, to_id} per
	// design B.9. v1: routes through the walker against the asset's edge UPROPERTY
	// container (typically `Edges` or `Transitions`).
	static FDryRunReport HandleEdges(const FBulkFillSpec& Spec)
	{
		// Same TargetType guard.
		const FString TargetTypePath = FindTargetTypeKey(Spec.Tree, FString());
		if (!TargetTypePath.IsEmpty())
		{
			FDryRunReport Report;
			FBulkFillFieldWrite W;
			W.Path = TargetTypePath;
			W.bOk = false;
			W.Reason = MakeTargetTypeUnsupportedReason();
			Report.FieldWrites.Add(W);
			Report.Errors = 1;
			Report.bWouldApply = false;
			return Report;
		}

		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(Spec.TargetAsset);
		if (!Asset)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("combograph adapter: asset not found at '%s'"), *Spec.TargetAsset));
		}

		const TSharedPtr<FJsonValue> EdgesVal = Spec.Tree->TryGetField(TEXT("edges"));
		if (!EdgesVal.IsValid())
		{
			return MakeResolveFailureReport(
				TEXT("combograph adapter: Edges fill_kind requires 'edges' field"));
		}

		FString EdgesField = TEXT("Edges");
		Spec.Tree->TryGetStringField(TEXT("edges_field"), EdgesField);
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetField(EdgesField, EdgesVal);

		FDryRunReport Report;
		if (Spec.bDryRun)
		{
			Report = FMonolithReflectionWalker::InspectTree(Root, Asset->GetClass(), Asset, Spec);
			Report.bWouldApply = false;
			return Report;
		}

		Asset->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(LOCTEXT("ComboGraphBulkFill_Edges", "Monolith ComboGraph Bulk Fill — Edges"));
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

	static FSchemaDescriptor BuildTopLevelDescribe()
	{
		FSchemaDescriptor Root;
		Root.FieldPath = TEXT("combograph");
		Root.TypeName = TEXT("Namespace");
		Root.ImportTextForm = TEXT(
			"fill_kind in {EffectContainers, Edges} — target=<ComboGraph asset>");

		auto AddKind = [&](const TCHAR* Kind, const TCHAR* Sample)
		{
			FSchemaDescriptor K;
			K.FieldPath = Kind;
			K.TypeName = TEXT("fill_kind");
			K.ImportTextForm = Sample;
			Root.Children.Add(K);
		};
		AddKind(
			TEXT("EffectContainers"),
			TEXT("{\"fill_kind\":\"EffectContainers\",\"containers\":[{...}]}"));
		AddKind(
			TEXT("Edges"),
			TEXT("{\"fill_kind\":\"Edges\",\"edges\":[{\"from_id\":\"A\",\"to_id\":\"B\",...}]}"));

		// TargetType explicit-error annotation — Step 8 post-review lock.
		FSchemaDescriptor TargetType;
		TargetType.FieldPath = TEXT("TargetType");
		TargetType.TypeName = TEXT("(non-UPROPERTY — custom-serialisation only)");
		TargetType.ImportTextForm = MakeTargetTypeUnsupportedReason();
		Root.Children.Add(TargetType);

		// EdGraph WITH_EDITORONLY_DATA lazy-materialise annotation.
		FSchemaDescriptor EdGraph;
		EdGraph.FieldPath = TEXT("(layout_combo_graph)");
		EdGraph.TypeName = TEXT("doc");
		EdGraph.ImportTextForm = TEXT(
			"EdGraph is WITH_EDITORONLY_DATA and lazily materialises — `layout_combo_graph` "
			"silently no-ops unless the asset has been opened in the editor at least once. "
			"Dry_run report flags this with a SilentDrops entry on layout writes.");
		Root.Children.Add(EdGraph);

		return Root;
	}
#endif // WITH_COMBOGRAPH
}

FDryRunReport FMonolithComboGraphBulkFillAdapter::ComboGraphBulkFill(const FBulkFillSpec& Spec)
{
	using namespace MonolithComboGraphBulkFillInternal;

#if WITH_COMBOGRAPH
	if (!Spec.Tree.IsValid())
	{
		return MakeResolveFailureReport(TEXT("combograph adapter: spec.tree is null"));
	}

	FString FillKind;
	Spec.Tree->TryGetStringField(TEXT("fill_kind"), FillKind);
	if (FillKind.IsEmpty())
	{
		return MakeResolveFailureReport(TEXT(
			"combograph adapter: spec.tree.fill_kind required — one of "
			"'EffectContainers', 'Edges'"));
	}

	if (FillKind == TEXT("EffectContainers")) return HandleEffectContainers(Spec);
	if (FillKind == TEXT("Edges"))            return HandleEdges(Spec);

	return MakeResolveFailureReport(FString::Printf(
		TEXT("combograph adapter: unknown fill_kind '%s'"), *FillKind));
#else
	// H5 stub-adapter branch — preserves discover surface symmetry.
	return MakeResolveFailureReport(TEXT(
		"combograph adapter: ComboGraph not available — WITH_COMBOGRAPH=0 in this build. "
		"Install the ComboGraph plugin to enable effect-container / edge bulk_fill."));
#endif
}

FSchemaDescriptor FMonolithComboGraphBulkFillAdapter::ComboGraphDescribe(const FString& TargetAsset)
{
	using namespace MonolithComboGraphBulkFillInternal;

#if WITH_COMBOGRAPH
	if (TargetAsset.IsEmpty())
	{
		return BuildTopLevelDescribe();
	}

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(TargetAsset);
	if (!Asset)
	{
		FSchemaDescriptor Err;
		Err.FieldPath = TEXT("(adapter)");
		Err.TypeName = TEXT("error");
		Err.ImportTextForm = FString::Printf(
			TEXT("combograph describe: asset not found at '%s'"), *TargetAsset);
		return Err;
	}

	FSchemaDescriptor Out = FMonolithReflectionWalker::DescribeStruct(Asset->GetClass());
	Out.FieldPath = TargetAsset;
	return Out;
#else
	FSchemaDescriptor Empty;
	Empty.FieldPath = TEXT("(adapter)");
	Empty.TypeName = TEXT("error");
	Empty.ImportTextForm = TEXT(
		"combograph describe: ComboGraph not available — WITH_COMBOGRAPH=0 in this build.");
	return Empty;
#endif
}

void FMonolithComboGraphBulkFillAdapter::Register()
{
	// H5 invariant — Register() runs unconditionally.
	FMonolithBulkFillRegistry::Get().RegisterAdapter(
		TEXT("combograph"),
		FMonolithBulkFillRegistry::FBulkFillAdapter(&FMonolithComboGraphBulkFillAdapter::ComboGraphBulkFill),
		FMonolithBulkFillRegistry::FDescribeAdapter(&FMonolithComboGraphBulkFillAdapter::ComboGraphDescribe));
}

void FMonolithComboGraphBulkFillAdapter::Unregister()
{
	FMonolithBulkFillRegistry::Get().UnregisterAdapter(TEXT("combograph"));
}

#undef LOCTEXT_NAMESPACE
