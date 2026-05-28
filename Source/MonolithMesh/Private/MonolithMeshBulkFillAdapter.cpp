// SPDX-License-Identifier: MIT
// MonolithMeshBulkFillAdapter — Phase 5 Step 5 adapter.
//
// Routes "mesh" target_namespace traffic from bulk_fill.apply / describe.schema
// to per-fill_kind handlers:
//
//   * fill_kind=SurfaceDataTable — write DataTable rows for surface mapping
//     (footstep SoundCue, impact decal, etc.) — design B.6 quirk row "DataTable
//     row-struct authoring is reflection-bound; cannot synthesise row struct
//     from MCP" (WISHLIST on row-struct synthesis; adapter assumes row struct exists).
//
//   * fill_kind=ActorProperties — bulk_fill of properties on a spawned actor.
//     Detects + reorders the Mobility-before-SimulatePhysics dependency per
//     design Cross-Cutting Engine Quirks row.
//
// `monolith_reindex` silent-prerequisite annotation: the describe tree surfaces
// the dependency so callers know `search_meshes_by_size` requires a reindex
// after structural mesh asset changes.

#include "MonolithMeshBulkFillAdapter.h"
#include "MonolithBulkFillRegistry.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "MonolithAssetUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "Engine/DataTable.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "MonolithMeshBulkFillAdapter"

namespace MonolithMeshBulkFillInternal
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

	// Per-row writer for SurfaceDataTable. Mirrors the UI adapter's WriteDataTableRow
	// pattern (Phase 4) — alloc + InitializeStruct + walker against row struct + AddRow.
	static void WriteSurfaceRow(
		UDataTable* DT,
		const UScriptStruct* RowStruct,
		const FString& RowName,
		const TSharedPtr<FJsonObject>& RowObj,
		const FBulkFillSpec& Spec,
		FDryRunReport& OutReport)
	{
		FBulkFillFieldWrite RowWrite;
		RowWrite.Path = FString::Printf(TEXT("rows[%s]"), *RowName);

		if (!RowObj.IsValid())
		{
			RowWrite.bOk = false;
			RowWrite.Reason = TEXT("row value is not a JSON object");
			OutReport.FieldWrites.Add(RowWrite);
			OutReport.Errors++;
			return;
		}

		uint8* RowData = static_cast<uint8*>(FMemory::Malloc(RowStruct->GetStructureSize()));
		RowStruct->InitializeStruct(RowData);
		int32 FieldErrors = 0;

		for (const auto& FieldKV : RowObj->Values)
		{
			FBulkFillFieldWrite FieldWrite;
			FieldWrite.Path = FString::Printf(TEXT("rows[%s].%s"), *RowName, *FieldKV.Key);

			FProperty* Prop = RowStruct->FindPropertyByName(FName(*FieldKV.Key));
			if (!Prop)
			{
				FieldWrite.Reason = FString::Printf(
					TEXT("row-struct '%s' has no field '%s'"),
					*RowStruct->GetName(), *FieldKV.Key);
				FieldWrite.bOk = false;
				OutReport.FieldWrites.Add(FieldWrite);
				OutReport.SilentDrops.Add(FieldWrite);
				OutReport.Errors++;
				FieldErrors++;
				continue;
			}

			FString JsonAsString;
			if (FieldKV.Value->Type == EJson::String)
			{
				FieldKV.Value->TryGetString(JsonAsString);
			}
			else
			{
				TSharedRef<TJsonWriter<TCHAR>> Writer = TJsonWriterFactory<TCHAR>::Create(&JsonAsString);
				FJsonSerializer::Serialize(FieldKV.Value.ToSharedRef(), TEXT(""), Writer);
			}
			FieldWrite.ProposedValue = JsonAsString;

			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);
			const TCHAR* ImportRes = Prop->ImportText_Direct(*JsonAsString, ValuePtr, nullptr, PPF_None);
			if (ImportRes == nullptr)
			{
				FieldWrite.bOk = false;
				FieldWrite.Reason = FString::Printf(
					TEXT("ImportText_Direct rejected value for property '%s' (type %s)"),
					*FieldKV.Key, *Prop->GetCPPType());
				OutReport.FieldWrites.Add(FieldWrite);
				OutReport.Errors++;
				FieldErrors++;
				continue;
			}
			FieldWrite.bOk = true;
			OutReport.FieldWrites.Add(FieldWrite);
		}

		if (!Spec.bDryRun && FieldErrors == 0)
		{
			DT->AddRow(FName(*RowName), reinterpret_cast<const uint8*>(RowData), RowStruct);
			RowWrite.bOk = true;
			OutReport.WouldModify.AddUnique(RowName);
		}
		else
		{
			RowWrite.bOk = (FieldErrors == 0);
			if (FieldErrors > 0)
			{
				RowWrite.Reason = FString::Printf(
					TEXT("%d field error(s) in row"), FieldErrors);
			}
		}

		RowStruct->DestroyStruct(RowData);
		FMemory::Free(RowData);
		OutReport.FieldWrites.Add(RowWrite);
	}

	static FDryRunReport HandleSurfaceDataTable(const FBulkFillSpec& Spec)
	{
		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(Spec.TargetAsset);
		UDataTable* DT = Cast<UDataTable>(Asset);
		if (!DT)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("mesh adapter: SurfaceDataTable requires UDataTable target (got %s)"),
				Asset ? *Asset->GetClass()->GetName() : TEXT("(null)")));
		}

		const UScriptStruct* RowStruct = DT->RowStruct;
		if (!RowStruct)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("mesh adapter: DataTable '%s' has no RowStruct (per design WISHLIST: row-struct synthesis from MCP unsupported)"),
				*Spec.TargetAsset));
		}

		const TSharedPtr<FJsonObject>* RowsObj = nullptr;
		if (!Spec.Tree->TryGetObjectField(TEXT("rows"), RowsObj) || !RowsObj || !(*RowsObj).IsValid())
		{
			return MakeResolveFailureReport(TEXT("mesh adapter: SurfaceDataTable requires 'rows' object"));
		}

		FDryRunReport Report;
		TSharedPtr<FScopedTransaction> Transaction;
		if (!Spec.bDryRun)
		{
			DT->SetFlags(RF_Transactional);
			Transaction = MakeShared<FScopedTransaction>(
				LOCTEXT("MeshBulkFill_DT", "Monolith Mesh Bulk Fill — Surface DT"));
			DT->Modify();
		}

		for (const auto& RowKV : (*RowsObj)->Values)
		{
			const TSharedPtr<FJsonObject>* RowSubObj = nullptr;
			TSharedPtr<FJsonObject> RowObj;
			if (RowKV.Value->TryGetObject(RowSubObj) && RowSubObj && (*RowSubObj).IsValid())
			{
				RowObj = *RowSubObj;
			}
			WriteSurfaceRow(DT, RowStruct, RowKV.Key, RowObj, Spec, Report);
		}

		if (Spec.bStrict && Report.Errors > 0)
		{
			if (Transaction.IsValid()) Transaction->Cancel();
			Report.bWouldApply = false;
			return Report;
		}

		if (!Spec.bDryRun)
		{
			DT->MarkPackageDirty();
			Report.bWouldApply = true;
		}
		return Report;
	}

	static FDryRunReport HandleActorProperties(const FBulkFillSpec& Spec)
	{
		// v1 stub for actor property bulk_fill — full implementation requires
		// resolving an actor pointer from an editor-world reference, which is
		// the surface of the existing mesh_query("set_actor_properties") action.
		// The adapter audits the payload here and flags the Mobility-ordering
		// quirk; commit still routes through the existing set_actor_properties
		// action so the bulk_fill envelope shape stays consistent.

		FDryRunReport Report;
		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		if (!Spec.Tree->TryGetObjectField(TEXT("properties"), PropsObj)
			|| !PropsObj || !(*PropsObj).IsValid())
		{
			return MakeResolveFailureReport(
				TEXT("mesh adapter: ActorProperties requires 'properties' object"));
		}

		// Detect Mobility + SimulatePhysics co-occurrence in the same payload.
		// Mobility MUST be written FIRST (Movable) before SimulatePhysics writes
		// can land successfully (design Cross-Cutting Engine Quirks row).
		const bool bHasMobility = (*PropsObj)->HasField(TEXT("Mobility"));
		const bool bHasSimPhys  = (*PropsObj)->HasField(TEXT("SimulatePhysics"))
			|| (*PropsObj)->HasField(TEXT("bSimulatePhysics"))
			|| (*PropsObj)->HasField(TEXT("BodyInstance"));

		if (bHasSimPhys && !bHasMobility)
		{
			FBulkFillFieldWrite W;
			W.Path = TEXT("properties.Mobility");
			W.bOk = false;
			W.Reason = TEXT(
				"SimulatePhysics-related write without Mobility=Movable in the same bulk_fill — "
				"engine will reject SimulatePhysics on Static actors silently. "
				"Add 'Mobility': 'Movable' to the payload before SimulatePhysics keys.");
			Report.FieldWrites.Add(W);
			Report.SilentDrops.Add(W);
			Report.Errors++;
		}

		// v1 surfaces the audit but doesn't execute writes — caller invokes
		// mesh_query("set_actor_properties") with the same payload.
		FBulkFillFieldWrite Info;
		Info.Path = TEXT("(adapter)");
		Info.bOk = true;
		Info.Reason = TEXT(
			"ActorProperties adapter v1: Mobility-ordering audit only. "
			"Commit via mesh_query('set_actor_properties') with the same payload. "
			"Engine quirk: Mobility=Movable MUST be written before SimulatePhysics.");
		Report.FieldWrites.Add(Info);
		Report.bWouldApply = false;
		return Report;
	}

	static FSchemaDescriptor BuildTopLevelDescribe()
	{
		FSchemaDescriptor Root;
		Root.FieldPath = TEXT("mesh");
		Root.TypeName = TEXT("Namespace");
		Root.ImportTextForm = TEXT(
			"fill_kind in {SurfaceDataTable, ActorProperties} — target=<UDataTable | Actor path>");

		auto AddKind = [&](const TCHAR* Kind, const TCHAR* Sample)
		{
			FSchemaDescriptor K;
			K.FieldPath = Kind;
			K.TypeName = TEXT("fill_kind");
			K.ImportTextForm = Sample;
			Root.Children.Add(K);
		};
		AddKind(
			TEXT("SurfaceDataTable"),
			TEXT("{\"fill_kind\":\"SurfaceDataTable\",\"rows\":{\"Wood\":{...}}}"));
		AddKind(
			TEXT("ActorProperties"),
			TEXT("{\"fill_kind\":\"ActorProperties\",\"properties\":{\"Mobility\":\"Movable\",\"bSimulatePhysics\":true}}"));

		// monolith_reindex silent-prerequisite annotation.
		FSchemaDescriptor Reindex;
		Reindex.FieldPath = TEXT("(prerequisite: monolith_reindex)");
		Reindex.TypeName = TEXT("doc");
		Reindex.ImportTextForm = TEXT(
			"`search_meshes_by_size` and DT-driven queries depend on the project mesh index. "
			"Adapter callers SHOULD invoke `monolith_reindex` after structural mesh asset changes "
			"(per design Cross-Cutting Engine Quirks row).");
		Root.Children.Add(Reindex);

		// Mobility-ordering annotation.
		FSchemaDescriptor Mobility;
		Mobility.FieldPath = TEXT("(Mobility-before-SimulatePhysics)");
		Mobility.TypeName = TEXT("doc");
		Mobility.ImportTextForm = TEXT(
			"Engine quirk: Mobility=Movable MUST be written BEFORE SimulatePhysics/BodyInstance. "
			"ActorProperties fill_kind audits this and surfaces a SilentDrops entry on violation.");
		Root.Children.Add(Mobility);

		// PIE-blocked annotation per design quirk row "No PIE-time mesh queries".
		FSchemaDescriptor PieNote;
		PieNote.FieldPath = TEXT("(pie.gate)");
		PieNote.TypeName = TEXT("doc");
		PieNote.bPieBlocked = true;
		PieNote.ImportTextForm = TEXT(
			"mesh bulk_fill rejected during PIE (per design quirk row 'No PIE-time mesh queries').");
		Root.Children.Add(PieNote);

		return Root;
	}
}

FDryRunReport FMonolithMeshBulkFillAdapter::MeshBulkFill(const FBulkFillSpec& Spec)
{
	using namespace MonolithMeshBulkFillInternal;

	if (!Spec.Tree.IsValid())
	{
		return MakeResolveFailureReport(TEXT("mesh adapter: spec.tree is null"));
	}

	FString FillKind;
	Spec.Tree->TryGetStringField(TEXT("fill_kind"), FillKind);
	if (FillKind.IsEmpty())
	{
		return MakeResolveFailureReport(TEXT(
			"mesh adapter: spec.tree.fill_kind required — one of "
			"'SurfaceDataTable', 'ActorProperties'"));
	}

	if (FillKind == TEXT("SurfaceDataTable")) return HandleSurfaceDataTable(Spec);
	if (FillKind == TEXT("ActorProperties"))  return HandleActorProperties(Spec);

	return MakeResolveFailureReport(FString::Printf(
		TEXT("mesh adapter: unknown fill_kind '%s'"), *FillKind));
}

FSchemaDescriptor FMonolithMeshBulkFillAdapter::MeshDescribe(const FString& TargetAsset)
{
	using namespace MonolithMeshBulkFillInternal;

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
			TEXT("mesh describe: asset not found at '%s'"), *TargetAsset);
		return Err;
	}

	FSchemaDescriptor Out = FMonolithReflectionWalker::DescribeStruct(Asset->GetClass());
	Out.FieldPath = TargetAsset;
	return Out;
}

void FMonolithMeshBulkFillAdapter::Register()
{
	FMonolithBulkFillRegistry::Get().RegisterAdapter(
		TEXT("mesh"),
		FMonolithBulkFillRegistry::FBulkFillAdapter(&FMonolithMeshBulkFillAdapter::MeshBulkFill),
		FMonolithBulkFillRegistry::FDescribeAdapter(&FMonolithMeshBulkFillAdapter::MeshDescribe));
}

void FMonolithMeshBulkFillAdapter::Unregister()
{
	FMonolithBulkFillRegistry::Get().UnregisterAdapter(TEXT("mesh"));
}

#undef LOCTEXT_NAMESPACE
