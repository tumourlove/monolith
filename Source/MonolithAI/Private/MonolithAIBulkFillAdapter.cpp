// SPDX-License-Identifier: MIT
// MonolithAIBulkFillAdapter — Phase 5 Step 1 adapter for the FMonolithBulkFillRegistry.
// Routes "ai" target_namespace traffic from the central bulk_fill.apply /
// describe.schema dispatchers to:
//
//   * fill_kind=EQSTests — walks `tests:[]` against UEnvQuery's Options/Tests
//     UPROPERTY graph via the Phase 0 reflection walker (the canonical 200-cell
//     EQS test pain point — design B.1).
//   * fill_kind=BlackboardKeys — walks `keys:{}` against UBlackboardData.Keys
//     UPROPERTY array; vector-keys auto-route through dict form (per
//     Cross-Cutting Engine Quirks row "Vector params dict-only").
//   * fill_kind=SmartObjectSlots — walks `slots:[]` against USmartObjectDefinition.
//
// The adapter delegates all per-field reflection writes to FMonolithReflectionWalker,
// which already handles FStructProperty/FArrayProperty/FMapProperty/FSetProperty/
// FObjectProperty/FSoftObjectProperty/FEnumProperty per Phase 0.
//
// H5 invariant — Register() runs unconditionally (no WITH_* gate needed: AIModule is
// always-on engine core).

#include "MonolithAIBulkFillAdapter.h"
#include "MonolithBulkFillRegistry.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "MonolithAssetUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

// AIModule headers (always-on UE 5.7 core).
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "BehaviorTree/BlackboardData.h"

#define LOCTEXT_NAMESPACE "MonolithAIBulkFillAdapter"

namespace MonolithAIBulkFillInternal
{
	// Mirror Phase 1-4 resolve-failure shape so the dispatcher receives a uniform
	// error tree whether the walker ran or not.
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

	// Resolve the target asset to a UObject*. Caller class-checks afterwards.
	static UObject* ResolveAsset(const FString& AssetPath)
	{
		return FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	}

	// Wrap a JSON sub-tree under a single-key root so we can hand it to
	// FMonolithReflectionWalker::WriteTree against the target's full reflection
	// surface. Used to write `tests:[]` directly into UEnvQuery without manually
	// driving FArrayProperty.
	static TSharedPtr<FJsonObject> WrapAsRoot(const FString& Key, const TSharedPtr<FJsonValue>& Value)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetField(Key, Value);
		return Root;
	}

	// fill_kind=EQSTests — adapter handler.
	// Two valid targets:
	//   * UEnvQuery (writes into Options[0].Tests — the canonical layout)
	//   * UEnvQueryOption (writes directly into Tests)
	// Tree key `tests` is an array; `context_bindings` is a dict (writes onto the
	// UEnvQuery's reflection surface for context-typed UPROPERTYs).
	static FDryRunReport HandleEQSTests(const FBulkFillSpec& Spec)
	{
		UObject* Asset = ResolveAsset(Spec.TargetAsset);
		if (!Asset)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("ai adapter: asset not found at '%s'"), *Spec.TargetAsset));
		}

		const TSharedPtr<FJsonValue> TestsVal = Spec.Tree->TryGetField(TEXT("tests"));
		if (!TestsVal.IsValid() || TestsVal->Type != EJson::Array)
		{
			return MakeResolveFailureReport(
				TEXT("ai adapter: EQSTests fill_kind requires 'tests' array"));
		}

		FDryRunReport Report;

		// Dry-run path: drive the walker against the asset's class without mutating.
		// Walker emits per-test ImportText accept/reject — surfaces the "score_equation
		// = 'Constnt'" enum-miss case from design example.
		if (Spec.bDryRun)
		{
			TSharedPtr<FJsonObject> Root = WrapAsRoot(TEXT("tests"), TestsVal);
			Report = FMonolithReflectionWalker::InspectTree(Root, Asset->GetClass(), Asset, Spec);
			Report.bWouldApply = false;
			return Report;
		}

		Asset->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(LOCTEXT("AIBulkFill_EQS", "Monolith AI Bulk Fill — EQS Tests"));
		Asset->Modify();
		Asset->PreEditChange(nullptr);

		TSharedPtr<FJsonObject> Root = WrapAsRoot(TEXT("tests"), TestsVal);
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

	// fill_kind=BlackboardKeys — adapter handler.
	// Vector-key default values arrive as JSON dict {x,y,z} — the walker's
	// FStructProperty path handles FVector ImportText via JSON-to-string conversion.
	static FDryRunReport HandleBlackboardKeys(const FBulkFillSpec& Spec)
	{
		UObject* Asset = ResolveAsset(Spec.TargetAsset);
		UBlackboardData* BB = Cast<UBlackboardData>(Asset);
		if (!BB)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("ai adapter: BlackboardKeys requires UBlackboardData asset, got %s"),
				Asset ? *Asset->GetClass()->GetName() : TEXT("(null)")));
		}

		const TSharedPtr<FJsonValue> KeysVal = Spec.Tree->TryGetField(TEXT("keys"));
		if (!KeysVal.IsValid() || KeysVal->Type != EJson::Object)
		{
			return MakeResolveFailureReport(
				TEXT("ai adapter: BlackboardKeys fill_kind requires 'keys' object"));
		}

		FDryRunReport Report;

		if (Spec.bDryRun)
		{
			// Surfaces type-mismatch & vector-dict-only quirk via the walker.
			TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetField(TEXT("Keys"), KeysVal);
			Report = FMonolithReflectionWalker::InspectTree(Root, BB->GetClass(), BB, Spec);
			Report.bWouldApply = false;
			return Report;
		}

		BB->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(LOCTEXT("AIBulkFill_BB", "Monolith AI Bulk Fill — Blackboard Keys"));
		BB->Modify();
		BB->PreEditChange(nullptr);

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetField(TEXT("Keys"), KeysVal);
		Report = FMonolithReflectionWalker::WriteTree(Root, BB->GetClass(), BB, BB, Spec);

		if (Spec.bStrict && Report.Errors > 0)
		{
			Transaction.Cancel();
			Report.bWouldApply = false;
			return Report;
		}

		BB->PostEditChange();
		BB->MarkPackageDirty();
		Report.bWouldApply = true;
		Report.WouldModify.Add(Spec.TargetAsset);
		return Report;
	}

	// fill_kind=SmartObjectSlots — adapter handler.
	// Delegates to the walker against the asset's UClass with `slots:[]` re-mapped
	// to the UPROPERTY name (USmartObjectDefinition::Slots).
	static FDryRunReport HandleSmartObjectSlots(const FBulkFillSpec& Spec)
	{
		UObject* Asset = ResolveAsset(Spec.TargetAsset);
		if (!Asset)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("ai adapter: asset not found at '%s'"), *Spec.TargetAsset));
		}

		const TSharedPtr<FJsonValue> SlotsVal = Spec.Tree->TryGetField(TEXT("slots"));
		if (!SlotsVal.IsValid() || SlotsVal->Type != EJson::Array)
		{
			return MakeResolveFailureReport(
				TEXT("ai adapter: SmartObjectSlots fill_kind requires 'slots' array"));
		}

		FDryRunReport Report;
		TSharedPtr<FJsonObject> Root = WrapAsRoot(TEXT("Slots"), SlotsVal);

		if (Spec.bDryRun)
		{
			Report = FMonolithReflectionWalker::InspectTree(Root, Asset->GetClass(), Asset, Spec);
			Report.bWouldApply = false;
			return Report;
		}

		Asset->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(LOCTEXT("AIBulkFill_SO", "Monolith AI Bulk Fill — SmartObject Slots"));
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

	// Build the top-level "ai" describe descriptor. Surfaces the engine-quirk
	// annotations callers need: set-once fields, lose_sight_radius clamp,
	// asset_path/save_path divergence on scaffold actions.
	static FSchemaDescriptor BuildTopLevelDescribe()
	{
		FSchemaDescriptor Root;
		Root.FieldPath = TEXT("ai");
		Root.TypeName = TEXT("Namespace");
		Root.ImportTextForm = TEXT(
			"fill_kind in {EQSTests, BlackboardKeys, SmartObjectSlots} — "
			"target=<EQS asset | UBlackboardData | USmartObjectDefinition>");

		// Surface set-once fields (sense affiliation, dominant sense) per
		// Cross-Cutting Engine Quirks row "Set-once fields (sense affiliation,
		// dominant sense)". bSetOnce=true is the load-bearing flag here.
		auto AddSetOnceField = [&](const TCHAR* Name, const TCHAR* Type, const TCHAR* Note)
		{
			FSchemaDescriptor F;
			F.FieldPath = Name;
			F.TypeName = Type;
			F.bSetOnce = true;
			F.ImportTextForm = Note;
			Root.Children.Add(F);
		};
		AddSetOnceField(
			TEXT("AISenseConfig_Sight.AffiliationFlags"),
			TEXT("FAISenseAffiliationFilter"),
			TEXT("(set-once — mutation attempts after first config silently no-op)"));
		AddSetOnceField(
			TEXT("AIPerceptionComponent.DominantSense"),
			TEXT("TSubclassOf<UAISense>"),
			TEXT("(set-once — mutation attempts after construction silently no-op)"));

		// lose_sight_radius clamp annotation — 1.1x sight_radius (engine quirk row).
		FSchemaDescriptor LoseSight;
		LoseSight.FieldPath = TEXT("AISenseConfig_Sight.LoseSightRadius");
		LoseSight.TypeName = TEXT("float");
		LoseSight.ImportTextForm = TEXT(
			"value (clamped to >= 1.1 * SightRadius silently if smaller — dry_run reports clamp)");
		Root.Children.Add(LoseSight);

		// fill_kind catalogue.
		auto AddKind = [&](const TCHAR* Kind, const TCHAR* Sample)
		{
			FSchemaDescriptor K;
			K.FieldPath = Kind;
			K.TypeName = TEXT("fill_kind");
			K.ImportTextForm = Sample;
			Root.Children.Add(K);
		};
		AddKind(
			TEXT("EQSTests"),
			TEXT("{\"fill_kind\":\"EQSTests\",\"tests\":[{\"weight\":1.0,...}]}"));
		AddKind(
			TEXT("BlackboardKeys"),
			TEXT("{\"fill_kind\":\"BlackboardKeys\",\"keys\":{\"MoveTarget\":{\"type\":\"Vector\"}}}"));
		AddKind(
			TEXT("SmartObjectSlots"),
			TEXT("{\"fill_kind\":\"SmartObjectSlots\",\"slots\":[{...}]}"));

		// EQS test-class enumeration via TObjectIterator (legal `type:` values).
		FSchemaDescriptor TestClasses;
		TestClasses.FieldPath = TEXT("(eqs.test.type allowed values)");
		TestClasses.TypeName = TEXT("UClass<UEnvQueryTest>");
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* C = *It;
			if (C && C->IsChildOf(UEnvQueryTest::StaticClass())
				&& !C->HasAnyClassFlags(CLASS_Abstract))
			{
				TestClasses.EnumValues.Add(C->GetName());
			}
		}
		Root.Children.Add(TestClasses);

		return Root;
	}
}

// ---------------------------------------------------------------------------
// FBulkFillAdapter — invoked from bulk_fill.apply for target_namespace="ai"
// ---------------------------------------------------------------------------
FDryRunReport FMonolithAIBulkFillAdapter::AIBulkFill(const FBulkFillSpec& Spec)
{
	using namespace MonolithAIBulkFillInternal;

	if (!Spec.Tree.IsValid())
	{
		return MakeResolveFailureReport(TEXT("ai adapter: spec.tree is null"));
	}

	FString FillKind;
	Spec.Tree->TryGetStringField(TEXT("fill_kind"), FillKind);
	if (FillKind.IsEmpty())
	{
		return MakeResolveFailureReport(TEXT(
			"ai adapter: spec.tree.fill_kind required — one of "
			"'EQSTests', 'BlackboardKeys', 'SmartObjectSlots'"));
	}

	if (FillKind == TEXT("EQSTests"))         return HandleEQSTests(Spec);
	if (FillKind == TEXT("BlackboardKeys"))   return HandleBlackboardKeys(Spec);
	if (FillKind == TEXT("SmartObjectSlots")) return HandleSmartObjectSlots(Spec);

	return MakeResolveFailureReport(FString::Printf(
		TEXT("ai adapter: unknown fill_kind '%s' (supported: EQSTests, BlackboardKeys, SmartObjectSlots)"),
		*FillKind));
}

// ---------------------------------------------------------------------------
// FDescribeAdapter — invoked from describe.schema for target_namespace="ai"
// ---------------------------------------------------------------------------
FSchemaDescriptor FMonolithAIBulkFillAdapter::AIDescribe(const FString& TargetAsset)
{
	using namespace MonolithAIBulkFillInternal;

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
		Err.ImportTextForm = FString::Printf(TEXT("ai describe: asset not found at '%s'"), *TargetAsset);
		return Err;
	}

	// Asset-specific describe — walk the resolved class's FProperty tree.
	FSchemaDescriptor Out = FMonolithReflectionWalker::DescribeStruct(Asset->GetClass());
	Out.FieldPath = TargetAsset;
	return Out;
}

// ---------------------------------------------------------------------------
// Registration entry-points.
// ---------------------------------------------------------------------------
void FMonolithAIBulkFillAdapter::Register()
{
	FMonolithBulkFillRegistry::Get().RegisterAdapter(
		TEXT("ai"),
		FMonolithBulkFillRegistry::FBulkFillAdapter(&FMonolithAIBulkFillAdapter::AIBulkFill),
		FMonolithBulkFillRegistry::FDescribeAdapter(&FMonolithAIBulkFillAdapter::AIDescribe));
}

void FMonolithAIBulkFillAdapter::Unregister()
{
	FMonolithBulkFillRegistry::Get().UnregisterAdapter(TEXT("ai"));
}

#undef LOCTEXT_NAMESPACE
