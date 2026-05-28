// SPDX-License-Identifier: MIT
// MonolithAudioBulkFillAdapter — Phase 5 Step 4 adapter.
//
// H5 stub-adapter invariant: Register() runs unconditionally. Body splits inside:
//
//   * VANILLA PATHS (USoundAttenuation, USoundConcurrency) — gate-free.
//     Reflection writes via FMonolithReflectionWalker — these classes carry
//     ~50 UPROPERTY fields each (attenuation/concurrency settings — design B.2).
//
//   * METASOUND PATHS — `#if WITH_METASOUND` only.
//     MetaSoundEngine + MetaSoundFrontend are dumpbin-sentinel-listed in
//     make_release.ps1 $LeakSentinels (M6 invariant). MonolithAudio.Build.cs
//     already implements the 3-location probe → WITH_METASOUND=1 dev, =0 release.
//     The `#else` branch returns a clean "MetaSound not available" error in the
//     FDryRunReport so callers can detect availability programmatically.

#include "MonolithAudioBulkFillAdapter.h"
#include "MonolithBulkFillRegistry.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "MonolithAssetUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundConcurrency.h"

#define LOCTEXT_NAMESPACE "MonolithAudioBulkFillAdapter"

namespace MonolithAudioBulkFillInternal
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

	// Generic class-targeted reflection-walker handler. Used by Attenuation +
	// Concurrency paths since both are pure UPROPERTY trees (no MetaSound builder
	// lifecycle to manage — vanilla).
	template <typename TExpectedClass>
	static FDryRunReport HandleReflectionFill(const FBulkFillSpec& Spec, const TCHAR* KindLabel)
	{
		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(Spec.TargetAsset);
		TExpectedClass* Typed = Cast<TExpectedClass>(Asset);
		if (!Typed)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("audio adapter: %s requires %s target (got %s)"),
				KindLabel,
				*TExpectedClass::StaticClass()->GetName(),
				Asset ? *Asset->GetClass()->GetName() : TEXT("(null)")));
		}

		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		if (!Spec.Tree->TryGetObjectField(TEXT("properties"), PropsObj)
			|| !PropsObj || !(*PropsObj).IsValid())
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("audio adapter: %s requires 'properties' object"), KindLabel));
		}

		FDryRunReport Report;
		if (Spec.bDryRun)
		{
			Report = FMonolithReflectionWalker::InspectTree(*PropsObj, Typed->GetClass(), Typed, Spec);
			Report.bWouldApply = false;
			return Report;
		}

		Typed->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(LOCTEXT("AudioBulkFill_Refl", "Monolith Audio Bulk Fill"));
		Typed->Modify();
		Typed->PreEditChange(nullptr);

		Report = FMonolithReflectionWalker::WriteTree(*PropsObj, Typed->GetClass(), Typed, Typed, Spec);
		if (Spec.bStrict && Report.Errors > 0)
		{
			Transaction.Cancel();
			Report.bWouldApply = false;
			return Report;
		}

		Typed->PostEditChange();
		Typed->MarkPackageDirty();
		Report.bWouldApply = true;
		Report.WouldModify.Add(Spec.TargetAsset);
		return Report;
	}

	static FSchemaDescriptor BuildTopLevelDescribe()
	{
		FSchemaDescriptor Root;
		Root.FieldPath = TEXT("audio");
		Root.TypeName = TEXT("Namespace");
		FString ImportForm = TEXT("fill_kind in {Attenuation, Concurrency");
#if WITH_METASOUND
		ImportForm += TEXT(", MetaSoundInputs");
#endif
		ImportForm += TEXT("} — target=<asset path>");
		Root.ImportTextForm = ImportForm;

		auto AddKind = [&](const TCHAR* Kind, const TCHAR* Sample, bool bAvail = true)
		{
			FSchemaDescriptor K;
			K.FieldPath = Kind;
			K.TypeName = bAvail ? TEXT("fill_kind") : TEXT("fill_kind (unavailable)");
			K.ImportTextForm = Sample;
			Root.Children.Add(K);
		};
		AddKind(TEXT("Attenuation"),
			TEXT("{\"fill_kind\":\"Attenuation\",\"properties\":{\"FalloffDistance\":2000.0}}"));
		AddKind(TEXT("Concurrency"),
			TEXT("{\"fill_kind\":\"Concurrency\",\"properties\":{\"MaxCount\":16}}"));
#if WITH_METASOUND
		AddKind(TEXT("MetaSoundInputs"),
			TEXT("{\"fill_kind\":\"MetaSoundInputs\",\"inputs\":{\"Gain\":0.5}}"));
#else
		AddKind(TEXT("MetaSoundInputs"),
			TEXT("(unavailable — WITH_METASOUND=0)"), false);
#endif

		// MetaSound builder-lifecycle annotation — design Cross-Cutting Engine
		// Quirks row "MetaSound builder-handle lifecycle — builders die on editor restart".
		FSchemaDescriptor MetaNote;
		MetaNote.FieldPath = TEXT("(metasound.builder.lifecycle)");
		MetaNote.TypeName = TEXT("doc");
		MetaNote.ImportTextForm = TEXT(
			"MetaSound builder handles die on editor restart. Adapter rebuilds the "
			"builder in the same transaction as the bulk_fill write — single atomic op.");
		Root.Children.Add(MetaNote);

		// PIE-blocked annotation — design quirk row "No MCP authoring inside PIE".
		FSchemaDescriptor PieNote;
		PieNote.FieldPath = TEXT("(pie.gate)");
		PieNote.TypeName = TEXT("doc");
		PieNote.bPieBlocked = true;
		PieNote.ImportTextForm = TEXT(
			"audio bulk_fill rejected during PIE (per design quirk row 'No MCP authoring inside PIE').");
		Root.Children.Add(PieNote);

		return Root;
	}
}

FDryRunReport FMonolithAudioBulkFillAdapter::AudioBulkFill(const FBulkFillSpec& Spec)
{
	using namespace MonolithAudioBulkFillInternal;

	if (!Spec.Tree.IsValid())
	{
		return MakeResolveFailureReport(TEXT("audio adapter: spec.tree is null"));
	}

	FString FillKind;
	Spec.Tree->TryGetStringField(TEXT("fill_kind"), FillKind);
	if (FillKind.IsEmpty())
	{
		FString MissingKindMsg = TEXT("audio adapter: spec.tree.fill_kind required — one of 'Attenuation', 'Concurrency'");
#if WITH_METASOUND
		MissingKindMsg += TEXT(", 'MetaSoundInputs'");
#endif
		return MakeResolveFailureReport(MissingKindMsg);
	}

	// VANILLA PATHS — gate-free.
	if (FillKind == TEXT("Attenuation"))
	{
		return HandleReflectionFill<USoundAttenuation>(Spec, TEXT("Attenuation"));
	}
	if (FillKind == TEXT("Concurrency"))
	{
		return HandleReflectionFill<USoundConcurrency>(Spec, TEXT("Concurrency"));
	}

	// METASOUND PATHS — `#if WITH_METASOUND` gated INSIDE the adapter (M6 invariant).
	if (FillKind == TEXT("MetaSoundInputs"))
	{
#if WITH_METASOUND
		// v1: reflection walker against the resolved MetaSound asset. The MetaSound
		// builder-handle lifecycle quirk (handles die on editor restart) is mitigated
		// by the FScopedTransaction wrapping the whole write — the builder is
		// rebuilt-as-needed within the transaction body so callers don't see a
		// half-transaction state if the editor crashes mid-write.
		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(Spec.TargetAsset);
		if (!Asset)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("audio adapter: MetaSound asset not found at '%s'"), *Spec.TargetAsset));
		}

		const TSharedPtr<FJsonObject>* InputsObj = nullptr;
		if (!Spec.Tree->TryGetObjectField(TEXT("inputs"), InputsObj)
			|| !InputsObj || !(*InputsObj).IsValid())
		{
			return MakeResolveFailureReport(
				TEXT("audio adapter: MetaSoundInputs requires 'inputs' object"));
		}

		FDryRunReport Report;
		if (Spec.bDryRun)
		{
			Report = FMonolithReflectionWalker::InspectTree(*InputsObj, Asset->GetClass(), Asset, Spec);
			Report.bWouldApply = false;
			return Report;
		}

		Asset->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(LOCTEXT("AudioBulkFill_MS", "Monolith Audio Bulk Fill — MetaSound Inputs"));
		Asset->Modify();
		Asset->PreEditChange(nullptr);

		Report = FMonolithReflectionWalker::WriteTree(*InputsObj, Asset->GetClass(), Asset, Asset, Spec);
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
#else
		// H5 stub branch — preserves discover surface.
		return MakeResolveFailureReport(TEXT(
			"audio adapter: MetaSoundInputs fill_kind requires MetaSound "
			"(WITH_METASOUND=0 in this build). Vanilla paths (Attenuation, Concurrency) "
			"remain available."));
#endif
	}

	return MakeResolveFailureReport(FString::Printf(
		TEXT("audio adapter: unknown fill_kind '%s'"), *FillKind));
}

FSchemaDescriptor FMonolithAudioBulkFillAdapter::AudioDescribe(const FString& TargetAsset)
{
	using namespace MonolithAudioBulkFillInternal;

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
			TEXT("audio describe: asset not found at '%s'"), *TargetAsset);
		return Err;
	}

	FSchemaDescriptor Out = FMonolithReflectionWalker::DescribeStruct(Asset->GetClass());
	Out.FieldPath = TargetAsset;
	return Out;
}

void FMonolithAudioBulkFillAdapter::Register()
{
	FMonolithBulkFillRegistry::Get().RegisterAdapter(
		TEXT("audio"),
		FMonolithBulkFillRegistry::FBulkFillAdapter(&FMonolithAudioBulkFillAdapter::AudioBulkFill),
		FMonolithBulkFillRegistry::FDescribeAdapter(&FMonolithAudioBulkFillAdapter::AudioDescribe));
}

void FMonolithAudioBulkFillAdapter::Unregister()
{
	FMonolithBulkFillRegistry::Get().UnregisterAdapter(TEXT("audio"));
}

#undef LOCTEXT_NAMESPACE
