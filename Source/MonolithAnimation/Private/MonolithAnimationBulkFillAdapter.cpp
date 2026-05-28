// SPDX-License-Identifier: MIT
// MonolithAnimationBulkFillAdapter — Phase 5 Step 6 adapter.
//
// Routes "animation" target_namespace traffic from bulk_fill.apply / describe.schema
// to per-fill_kind handlers:
//
//   * fill_kind=PoseSearchDatabase — bulk-populate a UPoseSearchDatabase asset
//     from a flat `entries:[]` JSON array. Replaces 40+ add_database_animation
//     round-trips per locomotion set (design B.3 pain point).
//
//   * fill_kind=NotifyApplyTemplate — audit-only v1; commit through existing
//     animation_query actions.
//
// PoseSearch writes route via FMonolithReflectionWalker against the asset's
// UPROPERTY surface. The walker handles FStructProperty<FPoseSearchDatabaseAnimationAssetBase>
// via ImportText_Direct on the stringified JSON.

#include "MonolithAnimationBulkFillAdapter.h"
#include "MonolithBulkFillRegistry.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "MonolithAssetUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "MonolithAnimationBulkFillAdapter"

namespace MonolithAnimationBulkFillInternal
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

	// fill_kind=PoseSearchDatabase handler. Resolves the target asset and writes
	// the `entries:[]` array into the database's UPROPERTY container (commonly
	// named `AnimationAssets` on UPoseSearchDatabase). The walker handles the
	// FArrayProperty<FInstancedStruct<FPoseSearchDatabaseAnimationAssetBase>>
	// expansion via per-element ImportText.
	static FDryRunReport HandlePoseSearchDatabase(const FBulkFillSpec& Spec)
	{
		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(Spec.TargetAsset);
		if (!Asset)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("animation adapter: asset not found at '%s'"), *Spec.TargetAsset));
		}

		// Soft class-check by name to avoid hard-linking the PoseSearch module
		// (Engine plugin, but defensive). UPoseSearchDatabase is the canonical
		// target — caller passes a different asset class at their own risk.
		if (!Asset->GetClass()->GetName().Contains(TEXT("PoseSearch")))
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("animation adapter: PoseSearchDatabase fill_kind requires a PoseSearch asset (got %s)"),
				*Asset->GetClass()->GetName()));
		}

		const TSharedPtr<FJsonValue> EntriesVal = Spec.Tree->TryGetField(TEXT("entries"));
		if (!EntriesVal.IsValid() || EntriesVal->Type != EJson::Array)
		{
			return MakeResolveFailureReport(
				TEXT("animation adapter: PoseSearchDatabase requires 'entries' array"));
		}

		// Walker accepts a single-key root with the entries array. The walker's
		// FindPropertyForwarding handles case-insensitive lookup so callers can
		// use either "entries" or "AnimationAssets" (the actual UPROPERTY name).
		FString EntriesKey = TEXT("AnimationAssets");
		// Allow caller-override of the field name in case the database asset
		// uses a non-default property name.
		Spec.Tree->TryGetStringField(TEXT("entries_field"), EntriesKey);

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetField(EntriesKey, EntriesVal);

		FDryRunReport Report;
		if (Spec.bDryRun)
		{
			Report = FMonolithReflectionWalker::InspectTree(Root, Asset->GetClass(), Asset, Spec);
			Report.bWouldApply = false;
			return Report;
		}

		Asset->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(LOCTEXT("AnimBulkFill_PSDB", "Monolith Animation Bulk Fill — PoseSearch DB"));
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

	// fill_kind=NotifyApplyTemplate — v1 audit-only.
	static FDryRunReport HandleNotifyApplyTemplate(const FBulkFillSpec& Spec)
	{
		FDryRunReport Report;
		FBulkFillFieldWrite Info;
		Info.Path = TEXT("(adapter)");
		Info.bOk = true;
		Info.Reason = TEXT(
			"NotifyApplyTemplate adapter v1: audit-only. "
			"Commit via animation_query's notify/curve actions with per-sequence dispatch.");
		Report.FieldWrites.Add(Info);
		Report.bWouldApply = false;
		return Report;
	}

	static FSchemaDescriptor BuildTopLevelDescribe()
	{
		FSchemaDescriptor Root;
		Root.FieldPath = TEXT("animation");
		Root.TypeName = TEXT("Namespace");
		Root.ImportTextForm = TEXT(
			"fill_kind in {PoseSearchDatabase, NotifyApplyTemplate} — target=<asset>");

		auto AddKind = [&](const TCHAR* Kind, const TCHAR* Sample)
		{
			FSchemaDescriptor K;
			K.FieldPath = Kind;
			K.TypeName = TEXT("fill_kind");
			K.ImportTextForm = Sample;
			Root.Children.Add(K);
		};
		AddKind(
			TEXT("PoseSearchDatabase"),
			TEXT("{\"fill_kind\":\"PoseSearchDatabase\",\"entries\":[{\"animation\":\"/Game/A_Idle\",\"looping\":true}]}"));
		AddKind(
			TEXT("NotifyApplyTemplate"),
			TEXT("{\"fill_kind\":\"NotifyApplyTemplate\",\"folder\":\"/Game/Anim/Walks\",\"name_glob\":\"A_Walk_*\",\"template\":{...}}"));

		// CHT_ chooser-table gap (WISHLIST per plan §29).
		FSchemaDescriptor ChooserGap;
		ChooserGap.FieldPath = TEXT("(CHT chooser-table support)");
		ChooserGap.TypeName = TEXT("wishlist");
		ChooserGap.ImportTextForm = TEXT(
			"(WISHLIST) — Chooser Table (CHT_) asset writes are NOT in v1. "
			"Chooser is reflection-bound but the row-wise spec semantics need "
			"a separate fill_kind not implemented this phase.");
		Root.Children.Add(ChooserGap);

		return Root;
	}
}

FDryRunReport FMonolithAnimationBulkFillAdapter::AnimationBulkFill(const FBulkFillSpec& Spec)
{
	using namespace MonolithAnimationBulkFillInternal;

	if (!Spec.Tree.IsValid())
	{
		return MakeResolveFailureReport(TEXT("animation adapter: spec.tree is null"));
	}

	FString FillKind;
	Spec.Tree->TryGetStringField(TEXT("fill_kind"), FillKind);
	if (FillKind.IsEmpty())
	{
		return MakeResolveFailureReport(TEXT(
			"animation adapter: spec.tree.fill_kind required — one of "
			"'PoseSearchDatabase', 'NotifyApplyTemplate'"));
	}

	if (FillKind == TEXT("PoseSearchDatabase"))  return HandlePoseSearchDatabase(Spec);
	if (FillKind == TEXT("NotifyApplyTemplate")) return HandleNotifyApplyTemplate(Spec);

	return MakeResolveFailureReport(FString::Printf(
		TEXT("animation adapter: unknown fill_kind '%s'"), *FillKind));
}

FSchemaDescriptor FMonolithAnimationBulkFillAdapter::AnimationDescribe(const FString& TargetAsset)
{
	using namespace MonolithAnimationBulkFillInternal;

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
			TEXT("animation describe: asset not found at '%s'"), *TargetAsset);
		return Err;
	}

	FSchemaDescriptor Out = FMonolithReflectionWalker::DescribeStruct(Asset->GetClass());
	Out.FieldPath = TargetAsset;
	return Out;
}

void FMonolithAnimationBulkFillAdapter::Register()
{
	FMonolithBulkFillRegistry::Get().RegisterAdapter(
		TEXT("animation"),
		FMonolithBulkFillRegistry::FBulkFillAdapter(&FMonolithAnimationBulkFillAdapter::AnimationBulkFill),
		FMonolithBulkFillRegistry::FDescribeAdapter(&FMonolithAnimationBulkFillAdapter::AnimationDescribe));
}

void FMonolithAnimationBulkFillAdapter::Unregister()
{
	FMonolithBulkFillRegistry::Get().UnregisterAdapter(TEXT("animation"));
}

#undef LOCTEXT_NAMESPACE
