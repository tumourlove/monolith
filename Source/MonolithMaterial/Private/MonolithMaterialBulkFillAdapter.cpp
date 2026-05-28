// SPDX-License-Identifier: MIT
// MonolithMaterialBulkFillAdapter — Phase 5 Step 3 adapter.
// Routes "material" target_namespace traffic from bulk_fill.apply / describe.schema.
//
// IMPORTANT QUIRKS surfaced from design B.5 + Cross-Cutting Engine Quirks rows:
//
//   * `VectorParameter.DefaultValue` is silently ignored by build_material_graph.
//     Adapter records that field as a SilentDrops entry whenever it appears in
//     BuildMaterialGraph payload.
//
//   * `material_outputs` block silently no-ops on build_material_graph. Same
//     SilentDrops treatment.
//
//   * `clear_existing:false` sometimes still clears expressions. SilentDrops entry.
//
//   * MaterialAttributeLayers reflection-hostile (Non-Goals §29). Adapter rejects
//     any payload with `layer_*` keys with an explicit WISHLIST error pointing
//     at the design Non-Goals row.
//
// MIC param writes go through the canonical SetScalarParameterValueEditorOnly /
// SetVectorParameterValueEditorOnly / SetTextureParameterValueEditorOnly /
// SetStaticSwitchParameterValueEditorOnly API. Verified via offline source_query:
//   Plugins/Chromalith/Source/ChromalithEditor/Private/ChromalithMaterialFactory.cpp:591
// uses the same surface — project precedent.

#include "MonolithMaterialBulkFillAdapter.h"
#include "MonolithBulkFillRegistry.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "MonolithAssetUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
// FMaterialParameterInfo arrives transitively from MaterialInstanceConstant.h;
// project precedent at MonolithMaterialActions.cpp:27-onward uses the same include set.
#include "Engine/Texture.h"

#define LOCTEXT_NAMESPACE "MonolithMaterialBulkFillAdapter"

namespace MonolithMaterialBulkFillInternal
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

	// Detect layer-targeted writes — Non-Goals §29 rejects these explicitly.
	static bool LooksLikeLayeredParam(const FString& Name)
	{
		return Name.StartsWith(TEXT("Layer."), ESearchCase::IgnoreCase)
			|| Name.Contains(TEXT("MaterialAttributeLayers"), ESearchCase::IgnoreCase);
	}

	// fill_kind=MICParameters — write the four canonical param maps onto a
	// UMaterialInstanceConstant. Each map's keys become FMaterialParameterInfo
	// names; values map to the four typed setters.
	static FDryRunReport HandleMICParameters(const FBulkFillSpec& Spec)
	{
		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(Spec.TargetAsset);
		UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Asset);
		if (!MIC)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("material adapter: target '%s' is not a UMaterialInstanceConstant (got %s)"),
				*Spec.TargetAsset,
				Asset ? *Asset->GetClass()->GetName() : TEXT("(null)")));
		}

		FDryRunReport Report;

		auto RecordField = [&](const FString& Path, const FString& Value, bool bOk, const FString& Reason = FString())
		{
			FBulkFillFieldWrite W;
			W.Path = Path;
			W.ProposedValue = Value;
			W.bOk = bOk;
			W.Reason = Reason;
			Report.FieldWrites.Add(W);
			if (!bOk) Report.Errors++;
		};

		TSharedPtr<FScopedTransaction> Transaction;
		if (!Spec.bDryRun)
		{
			MIC->SetFlags(RF_Transactional);
			Transaction = MakeShared<FScopedTransaction>(
				LOCTEXT("MaterialBulkFill_MIC", "Monolith Material Bulk Fill — MIC Parameters"));
			MIC->Modify();
		}

		// scalars: {Name: float}
		const TSharedPtr<FJsonObject>* ScalarsObj = nullptr;
		if (Spec.Tree->TryGetObjectField(TEXT("scalars"), ScalarsObj) && ScalarsObj && (*ScalarsObj).IsValid())
		{
			for (const auto& KV : (*ScalarsObj)->Values)
			{
				if (LooksLikeLayeredParam(KV.Key))
				{
					RecordField(FString::Printf(TEXT("scalars[%s]"), *KV.Key),
						TEXT("(layered param)"), false,
						TEXT("MaterialAttributeLayers writes rejected — WISHLIST per design Non-Goals §29"));
					continue;
				}
				double V = 0.0;
				if (!KV.Value->TryGetNumber(V))
				{
					RecordField(FString::Printf(TEXT("scalars[%s]"), *KV.Key),
						TEXT(""), false, TEXT("scalar value must be a number"));
					continue;
				}
				const float FloatV = static_cast<float>(V);
				if (!Spec.bDryRun)
				{
					MIC->SetScalarParameterValueEditorOnly(
						FMaterialParameterInfo(*KV.Key), FloatV);
				}
				RecordField(FString::Printf(TEXT("scalars[%s]"), *KV.Key),
					FString::SanitizeFloat(FloatV), true);
			}
		}

		// vectors: {Name: {r,g,b,a}}
		const TSharedPtr<FJsonObject>* VectorsObj = nullptr;
		if (Spec.Tree->TryGetObjectField(TEXT("vectors"), VectorsObj) && VectorsObj && (*VectorsObj).IsValid())
		{
			for (const auto& KV : (*VectorsObj)->Values)
			{
				if (LooksLikeLayeredParam(KV.Key))
				{
					RecordField(FString::Printf(TEXT("vectors[%s]"), *KV.Key),
						TEXT("(layered param)"), false,
						TEXT("MaterialAttributeLayers writes rejected — WISHLIST per design Non-Goals §29"));
					continue;
				}
				const TSharedPtr<FJsonObject>* RgbaObj = nullptr;
				if (!KV.Value->TryGetObject(RgbaObj) || !RgbaObj || !(*RgbaObj).IsValid())
				{
					RecordField(FString::Printf(TEXT("vectors[%s]"), *KV.Key),
						TEXT(""), false, TEXT("vector value must be {r,g,b,a} object"));
					continue;
				}
				double R=0.0, G=0.0, B=0.0, A=1.0;
				(*RgbaObj)->TryGetNumberField(TEXT("r"), R);
				(*RgbaObj)->TryGetNumberField(TEXT("g"), G);
				(*RgbaObj)->TryGetNumberField(TEXT("b"), B);
				(*RgbaObj)->TryGetNumberField(TEXT("a"), A);
				const FLinearColor LC((float)R, (float)G, (float)B, (float)A);
				if (!Spec.bDryRun)
				{
					MIC->SetVectorParameterValueEditorOnly(
						FMaterialParameterInfo(*KV.Key), LC);
				}
				RecordField(FString::Printf(TEXT("vectors[%s]"), *KV.Key),
					LC.ToString(), true);
			}
		}

		// textures: {Name: AssetPath}
		const TSharedPtr<FJsonObject>* TexturesObj = nullptr;
		if (Spec.Tree->TryGetObjectField(TEXT("textures"), TexturesObj) && TexturesObj && (*TexturesObj).IsValid())
		{
			for (const auto& KV : (*TexturesObj)->Values)
			{
				if (LooksLikeLayeredParam(KV.Key))
				{
					RecordField(FString::Printf(TEXT("textures[%s]"), *KV.Key),
						TEXT("(layered param)"), false,
						TEXT("MaterialAttributeLayers writes rejected — WISHLIST"));
					continue;
				}
				FString TexPath;
				if (!KV.Value->TryGetString(TexPath))
				{
					RecordField(FString::Printf(TEXT("textures[%s]"), *KV.Key),
						TEXT(""), false, TEXT("texture value must be an asset path string"));
					continue;
				}
				UTexture* Tex = Cast<UTexture>(FMonolithAssetUtils::LoadAssetByPath(TexPath));
				if (!Tex)
				{
					RecordField(FString::Printf(TEXT("textures[%s]"), *KV.Key),
						TexPath, false,
						FString::Printf(TEXT("texture asset '%s' not found"), *TexPath));
					continue;
				}
				if (!Spec.bDryRun)
				{
					MIC->SetTextureParameterValueEditorOnly(
						FMaterialParameterInfo(*KV.Key), Tex);
				}
				RecordField(FString::Printf(TEXT("textures[%s]"), *KV.Key), TexPath, true);
			}
		}

		// switches: {Name: bool}
		const TSharedPtr<FJsonObject>* SwitchesObj = nullptr;
		if (Spec.Tree->TryGetObjectField(TEXT("switches"), SwitchesObj) && SwitchesObj && (*SwitchesObj).IsValid())
		{
			for (const auto& KV : (*SwitchesObj)->Values)
			{
				if (LooksLikeLayeredParam(KV.Key))
				{
					RecordField(FString::Printf(TEXT("switches[%s]"), *KV.Key),
						TEXT("(layered param)"), false,
						TEXT("MaterialAttributeLayers writes rejected — WISHLIST"));
					continue;
				}
				bool BoolV = false;
				if (!KV.Value->TryGetBool(BoolV))
				{
					RecordField(FString::Printf(TEXT("switches[%s]"), *KV.Key),
						TEXT(""), false, TEXT("switch value must be a bool"));
					continue;
				}
				if (!Spec.bDryRun)
				{
					// Project precedent: MonolithMaterialActions.cpp:2775 uses the
					// 2-arg form (no FGuid override) — third FGuid is defaulted
					// internally by the engine.
					MIC->SetStaticSwitchParameterValueEditorOnly(
						FMaterialParameterInfo(*KV.Key), BoolV);
				}
				RecordField(FString::Printf(TEXT("switches[%s]"), *KV.Key),
					BoolV ? TEXT("true") : TEXT("false"), true);
			}
		}

		if (Spec.bStrict && Report.Errors > 0)
		{
			if (Transaction.IsValid()) Transaction->Cancel();
			Report.bWouldApply = false;
			return Report;
		}

		if (!Spec.bDryRun)
		{
			// PostEditChange flushes the MIC's MaterialUpdateContext on the next
			// editor tick — matches the canonical UpdateMaterialInstance flow.
			MIC->PostEditChange();
			MIC->MarkPackageDirty();
			Report.bWouldApply = true;
			Report.WouldModify.Add(Spec.TargetAsset);
		}
		else
		{
			Report.bWouldApply = false;
		}
		return Report;
	}

	// fill_kind=BuildMaterialGraph — wrapper for build_material_graph.
	// v1 surfaces silent-drops in the report; the actual graph-build still happens
	// via the existing material_query("build_material_graph") action. The adapter
	// here exists to: (a) annotate the SilentDrops list before dispatch, (b) reject
	// MaterialAttributeLayers writes with the WISHLIST error.
	static FDryRunReport HandleBuildMaterialGraph(const FBulkFillSpec& Spec)
	{
		FDryRunReport Report;

		// Scan the payload for known silent-drop hazards.
		auto AddSilentDrop = [&](const FString& Path, const FString& Reason)
		{
			FBulkFillFieldWrite W;
			W.Path = Path;
			W.bOk = true; // walker accepted but the underlying action no-ops
			W.Reason = Reason;
			Report.SilentDrops.Add(W);
		};

		const TSharedPtr<FJsonObject>* GraphSpecObj = nullptr;
		if (Spec.Tree->TryGetObjectField(TEXT("graph_spec"), GraphSpecObj)
			&& GraphSpecObj && (*GraphSpecObj).IsValid())
		{
			// VectorParameter.DefaultValue silent-drop check.
			const TArray<TSharedPtr<FJsonValue>>* NodesArr = nullptr;
			if ((*GraphSpecObj)->TryGetArrayField(TEXT("nodes"), NodesArr))
			{
				for (int32 i = 0; i < NodesArr->Num(); ++i)
				{
					const TSharedPtr<FJsonObject>* NodeObj = nullptr;
					if ((*NodesArr)[i]->TryGetObject(NodeObj) && NodeObj && (*NodeObj).IsValid())
					{
						FString NodeType;
						(*NodeObj)->TryGetStringField(TEXT("type"), NodeType);
						if (NodeType.Contains(TEXT("VectorParameter"))
							&& (*NodeObj)->HasField(TEXT("DefaultValue")))
						{
							AddSilentDrop(
								FString::Printf(TEXT("nodes[%d].DefaultValue"), i),
								TEXT("VectorParameter.DefaultValue silently ignored by build_material_graph "
								     "(per design Cross-Cutting Engine Quirks row)"));
						}
					}
				}
			}

			// material_outputs no-op silent-drop check.
			if ((*GraphSpecObj)->HasField(TEXT("material_outputs")))
			{
				AddSilentDrop(
					TEXT("graph_spec.material_outputs"),
					TEXT("material_outputs block silently no-ops on build_material_graph "
					     "(per design Cross-Cutting Engine Quirks row)"));
			}

			// clear_existing:false sometimes-still-clears silent-drop check.
			bool bClearExisting = true;
			if ((*GraphSpecObj)->TryGetBoolField(TEXT("clear_existing"), bClearExisting) && !bClearExisting)
			{
				AddSilentDrop(
					TEXT("graph_spec.clear_existing=false"),
					TEXT("clear_existing:false sometimes still clears existing expressions "
					     "(per design Cross-Cutting Engine Quirks row)"));
			}

			// MaterialAttributeLayers rejection.
			if ((*GraphSpecObj)->HasField(TEXT("material_attribute_layers")))
			{
				return MakeResolveFailureReport(
					TEXT("material adapter: MaterialAttributeLayers writes rejected — "
					     "WISHLIST per design Non-Goals §29 (MaterialAttributeLayers reflection-hostile)"));
			}
		}

		// v1 stub — adapter doesn't drive build_material_graph itself. The report
		// surfaces SilentDrops; callers run the underlying action and merge results.
		Report.bWouldApply = false;
		FBulkFillFieldWrite Info;
		Info.Path = TEXT("(adapter)");
		Info.bOk = true;
		Info.Reason = TEXT(
			"BuildMaterialGraph adapter v1: SilentDrops scan complete. Call "
			"material_query('build_material_graph') with the same graph_spec to commit. "
			"The adapter does NOT drive the build itself — it audits the spec.");
		Report.FieldWrites.Add(Info);
		return Report;
	}

	static FSchemaDescriptor BuildTopLevelDescribe()
	{
		FSchemaDescriptor Root;
		Root.FieldPath = TEXT("material");
		Root.TypeName = TEXT("Namespace");
		Root.ImportTextForm = TEXT(
			"fill_kind in {MICParameters, BuildMaterialGraph} — target=<UMaterialInstanceConstant | UMaterial>");

		FSchemaDescriptor MIC;
		MIC.FieldPath = TEXT("MICParameters");
		MIC.TypeName = TEXT("fill_kind");
		MIC.ImportTextForm = TEXT(
			"{\"fill_kind\":\"MICParameters\",\"scalars\":{...},\"vectors\":{...},\"textures\":{...},\"switches\":{...}}");
		Root.Children.Add(MIC);

		FSchemaDescriptor BMG;
		BMG.FieldPath = TEXT("BuildMaterialGraph");
		BMG.TypeName = TEXT("fill_kind");
		BMG.ImportTextForm = TEXT(
			"{\"fill_kind\":\"BuildMaterialGraph\",\"graph_spec\":{...}} — "
			"v1 audits only; callers must run material_query('build_material_graph') to commit");
		Root.Children.Add(BMG);

		FSchemaDescriptor Wish;
		Wish.FieldPath = TEXT("(MaterialAttributeLayers)");
		Wish.TypeName = TEXT("wishlist");
		Wish.ImportTextForm = TEXT(
			"(WISHLIST) — MaterialAttributeLayers writes rejected. Reflection-hostile per design Non-Goals §29.");
		Root.Children.Add(Wish);

		return Root;
	}
}

FDryRunReport FMonolithMaterialBulkFillAdapter::MaterialBulkFill(const FBulkFillSpec& Spec)
{
	using namespace MonolithMaterialBulkFillInternal;

	if (!Spec.Tree.IsValid())
	{
		return MakeResolveFailureReport(TEXT("material adapter: spec.tree is null"));
	}

	FString FillKind;
	Spec.Tree->TryGetStringField(TEXT("fill_kind"), FillKind);
	if (FillKind.IsEmpty())
	{
		return MakeResolveFailureReport(TEXT(
			"material adapter: spec.tree.fill_kind required — one of "
			"'MICParameters', 'BuildMaterialGraph'"));
	}

	if (FillKind == TEXT("MICParameters"))      return HandleMICParameters(Spec);
	if (FillKind == TEXT("BuildMaterialGraph")) return HandleBuildMaterialGraph(Spec);

	return MakeResolveFailureReport(FString::Printf(
		TEXT("material adapter: unknown fill_kind '%s'"), *FillKind));
}

FSchemaDescriptor FMonolithMaterialBulkFillAdapter::MaterialDescribe(const FString& TargetAsset)
{
	using namespace MonolithMaterialBulkFillInternal;

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
			TEXT("material describe: asset not found at '%s'"), *TargetAsset);
		return Err;
	}

	FSchemaDescriptor Out = FMonolithReflectionWalker::DescribeStruct(Asset->GetClass());
	Out.FieldPath = TargetAsset;
	return Out;
}

void FMonolithMaterialBulkFillAdapter::Register()
{
	FMonolithBulkFillRegistry::Get().RegisterAdapter(
		TEXT("material"),
		FMonolithBulkFillRegistry::FBulkFillAdapter(&FMonolithMaterialBulkFillAdapter::MaterialBulkFill),
		FMonolithBulkFillRegistry::FDescribeAdapter(&FMonolithMaterialBulkFillAdapter::MaterialDescribe));
}

void FMonolithMaterialBulkFillAdapter::Unregister()
{
	FMonolithBulkFillRegistry::Get().UnregisterAdapter(TEXT("material"));
}

#undef LOCTEXT_NAMESPACE
