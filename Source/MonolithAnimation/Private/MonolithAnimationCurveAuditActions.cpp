// SPDX-License-Identifier: MIT
// Bulk float-curve actions for anim sequences: copy_sequence_curves / audit_sequence_curves.
//
// Writes go through IAnimationDataController (AddCurve / SetCurveKeys /
// SetCurveFlags / SetCurveColor) inside one editor transaction per call and one
// controller bracket per target, so each target is a single undo step's worth of
// model notifications. Game-thread only.

#include "MonolithAnimationCurveAuditActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace MonolithSeqCurveAuditInternal
{
	/**
	 * Read a string-list param. Accepts a JSON array of strings, a JSON-encoded
	 * array string (some MCP clients stringify nested arrays), or a single string.
	 * Returns false only when the field is present but unusable.
	 */
	static bool SeqCurveReadStringList(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, TArray<FString>& Out, FString& OutError)
	{
		Out.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		TArray<TSharedPtr<FJsonValue>> Parsed;
		if (!Params->TryGetArrayField(Field, Arr) || !Arr)
		{
			FString Str;
			if (!Params->TryGetStringField(Field, Str))
			{
				return true; // absent
			}
			Str.TrimStartAndEndInline();
			if (Str.IsEmpty())
			{
				return true;
			}
			if (Str.StartsWith(TEXT("[")))
			{
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Str);
				if (!FJsonSerializer::Deserialize(Reader, Parsed))
				{
					OutError = FString::Printf(TEXT("'%s' is not a valid JSON array of strings"), Field);
					return false;
				}
				Arr = &Parsed;
			}
			else
			{
				Out.Add(Str);
				return true;
			}
		}

		for (int32 Index = 0; Index < Arr->Num(); ++Index)
		{
			FString Item;
			if (!(*Arr)[Index].IsValid() || !(*Arr)[Index]->TryGetString(Item))
			{
				OutError = FString::Printf(TEXT("%s[%d] is not a string"), Field, Index);
				return false;
			}
			Item.TrimStartAndEndInline();
			if (!Item.IsEmpty())
			{
				Out.AddUnique(Item);
			}
		}
		return true;
	}

	/** Every UAnimSequence (including subclasses) under a content folder. */
	static void SeqCurveCollectFolder(FString Folder, bool bRecursive, TArray<FString>& OutPaths)
	{
		while (Folder.Len() > 1 && Folder.EndsWith(TEXT("/")))
		{
			Folder.LeftChopInline(1);
		}

		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FARFilter Filter;
		Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.PackagePaths.Add(FName(*Folder));
		Filter.bRecursivePaths = bRecursive;

		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);
		Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
		for (const FAssetData& Asset : Assets)
		{
			OutPaths.AddUnique(Asset.GetObjectPathString());
		}
	}

	static TArray<TSharedPtr<FJsonValue>> SeqCurveToJsonArray(const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(In.Num());
		for (const FString& S : In)
		{
			Out.Add(MakeShared<FJsonValueString>(S));
		}
		return Out;
	}

	/** Copy of a float curve's keys with time scaled by TimeScale (tangents rescaled to keep the same shape). */
	static TArray<FRichCurveKey> SeqCurveRetimedKeys(const FRichCurve& Source, double TimeScale)
	{
		TArray<FRichCurveKey> Keys = Source.GetConstRefOfKeys();
		if (!FMath::IsNearlyEqual(TimeScale, 1.0) && TimeScale > UE_SMALL_NUMBER)
		{
			const float Scale = static_cast<float>(TimeScale);
			for (FRichCurveKey& Key : Keys)
			{
				Key.Time *= Scale;
				Key.ArriveTangent /= Scale;
				Key.LeaveTangent /= Scale;
				Key.ArriveTangentWeight *= Scale;
				Key.LeaveTangentWeight *= Scale;
			}
		}
		return Keys;
	}
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithAnimationCurveAuditActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("animation"), TEXT("copy_sequence_curves"),
		TEXT("Copy float curves (keys, curve flags and editor colour) from one AnimSequence to one or more target AnimSequences "
		     "through the animation data controller. Copies every float curve unless curves[] filters them. Curves that already "
		     "exist on a target are skipped unless overwrite=true. retime=\"scale\" stretches key times to each target's play "
		     "length. One undo transaction per call. dry_run=true reports the plan without writing. Returns per-target added / "
		     "overwritten / skipped curve lists and a count of keys landing outside the target's play length."),
		FMonolithActionHandler::CreateStatic(&HandleCopySequenceCurves),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("source"), TEXT("AnimSequence to copy curves from"))
			.Optional(TEXT("targets"), TEXT("array"), TEXT("AnimSequence asset paths to copy curves into"))
			.Optional(TEXT("target_folder"), TEXT("string"), TEXT("Content folder whose AnimSequences are added to targets (recursive)"))
			.Optional(TEXT("curves"), TEXT("array"), TEXT("Float curve names to copy (default: all float curves on the source)"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Replace keys of curves that already exist on a target (default false)"), TEXT("false"))
			.Optional(TEXT("retime"), TEXT("string"), TEXT("\"none\" (default) keeps key times; \"scale\" scales them by target_length / source_length"), TEXT("none"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Report what would change without writing (default false)"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save each modified target package (default false)"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("audit_sequence_curves"),
		TEXT("Audit float curves across AnimSequences. Checks each sequence against required_curves[] (or the float-curve set of a "
		     "reference sequence) and reports, per sequence, the missing curves, the extra curves not in the required set, and "
		     "required curves that exist but have no keys. Also returns how many sequences miss each required curve. Read-only."),
		FMonolithActionHandler::CreateStatic(&HandleAuditSequenceCurves),
		FParamSchemaBuilder()
			.Optional(TEXT("assets"), TEXT("array"), TEXT("AnimSequence asset paths to audit"))
			.Optional(TEXT("folder"), TEXT("string"), TEXT("Content folder whose AnimSequences are audited"))
			.Optional(TEXT("recursive"), TEXT("boolean"), TEXT("Include subfolders of folder (default true)"), TEXT("true"))
			.Optional(TEXT("required_curves"), TEXT("array"), TEXT("Curve names every sequence must have"))
			.OptionalAssetPath(TEXT("reference"), TEXT("AnimSequence whose float curves form the required set (merged with required_curves)"))
			.Optional(TEXT("only_failing"), TEXT("boolean"), TEXT("Only list sequences with missing or empty required curves (default false)"), TEXT("false"))
			.Build());
}

// ---------------------------------------------------------------------------
// copy_sequence_curves
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationCurveAuditActions::HandleCopySequenceCurves(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithSeqCurveAuditInternal;

	const FString SourcePath = Params->GetStringField(TEXT("source"));
	if (SourcePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: source"));
	}
	UAnimSequence* Source = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(SourcePath);
	if (!Source)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source AnimSequence not found: %s"), *SourcePath));
	}
	const IAnimationDataModel* SourceModel = Source->GetDataModel();
	if (!SourceModel)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source has no animation data model: %s"), *SourcePath));
	}

	FString ListError;
	TArray<FString> TargetPaths;
	if (!SeqCurveReadStringList(Params, TEXT("targets"), TargetPaths, ListError))
	{
		return FMonolithActionResult::Error(ListError);
	}
	FString TargetFolder;
	if (Params->TryGetStringField(TEXT("target_folder"), TargetFolder) && !TargetFolder.IsEmpty())
	{
		SeqCurveCollectFolder(TargetFolder, /*bRecursive=*/true, TargetPaths);
	}
	if (TargetPaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No targets. Pass targets (array of AnimSequence paths) and/or target_folder."));
	}

	TArray<FString> CurveFilter;
	if (!SeqCurveReadStringList(Params, TEXT("curves"), CurveFilter, ListError))
	{
		return FMonolithActionResult::Error(ListError);
	}

	FString Retime = TEXT("none");
	Params->TryGetStringField(TEXT("retime"), Retime);
	Retime = Retime.ToLower();
	if (Retime != TEXT("none") && Retime != TEXT("scale"))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("retime must be \"none\" or \"scale\", got '%s'"), *Retime));
	}

	bool bOverwrite = false, bDryRun = false, bSave = false;
	Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	Params->TryGetBoolField(TEXT("save"), bSave);

	// Resolve the source curves to copy.
	TArray<const FFloatCurve*> SourceCurves;
	TArray<FString> MissingOnSource;
	if (CurveFilter.Num() > 0)
	{
		for (const FString& Name : CurveFilter)
		{
			const FAnimationCurveIdentifier Id(FName(*Name), ERawCurveTrackTypes::RCT_Float);
			if (const FFloatCurve* Curve = SourceModel->FindFloatCurve(Id))
			{
				SourceCurves.Add(Curve);
			}
			else
			{
				MissingOnSource.Add(Name);
			}
		}
	}
	else
	{
		for (const FFloatCurve& Curve : SourceModel->GetFloatCurves())
		{
			SourceCurves.Add(&Curve);
		}
	}
	if (SourceCurves.Num() == 0)
	{
		return FMonolithActionResult::Error(MissingOnSource.Num() > 0
			? FString::Printf(TEXT("None of the requested curves exist on the source: %s"), *FString::Join(MissingOnSource, TEXT(", ")))
			: FString::Printf(TEXT("Source has no float curves: %s"), *SourcePath));
	}

	// Snapshot the source data first: a target may share nothing with the source, but
	// copying out avoids holding pointers into the source model while other models change.
	struct FSeqCurveSnapshot
	{
		FName Name;
		int32 Flags = 0;
		FLinearColor Color = FLinearColor::White;
		bool bHasColor = false;
		FRichCurve Curve;
	};
	TArray<FSeqCurveSnapshot> Snapshots;
	for (const FFloatCurve* Curve : SourceCurves)
	{
		FSeqCurveSnapshot& Snap = Snapshots.AddDefaulted_GetRef();
		Snap.Name = Curve->GetName();
		Snap.Flags = Curve->GetCurveTypeFlags();
		Snap.Curve = Curve->FloatCurve;
#if WITH_EDITORONLY_DATA
		Snap.Color = Curve->Color;
		Snap.bHasColor = true;
#endif
	}
	const double SourceLength = static_cast<double>(Source->GetPlayLength());

	TOptional<FScopedTransaction> Transaction;
	if (!bDryRun)
	{
		Transaction.Emplace(NSLOCTEXT("MonolithAnimationCurveAuditActions", "CopySequenceCurves", "Monolith Copy Sequence Curves"));
	}

	TArray<TSharedPtr<FJsonValue>> TargetResults;
	TArray<FString> TargetErrors;
	int32 TotalAdded = 0, TotalOverwritten = 0, TotalSkipped = 0, TargetsModified = 0;

	for (const FString& TargetPath : TargetPaths)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), TargetPath);

		UAnimSequence* Target = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(TargetPath);
		if (!Target)
		{
			const FString Err = FString::Printf(TEXT("AnimSequence not found: %s"), *TargetPath);
			Entry->SetStringField(TEXT("error"), Err);
			TargetErrors.Add(Err);
			TargetResults.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}
		if (Target == Source)
		{
			Entry->SetStringField(TEXT("error"), TEXT("Target is the source sequence; skipped"));
			TargetResults.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}
		const IAnimationDataModel* TargetModel = Target->GetDataModel();
		if (!TargetModel)
		{
			const FString Err = FString::Printf(TEXT("Target has no animation data model: %s"), *TargetPath);
			Entry->SetStringField(TEXT("error"), Err);
			TargetErrors.Add(Err);
			TargetResults.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		const double TargetLength = static_cast<double>(Target->GetPlayLength());
		const double TimeScale = (Retime == TEXT("scale") && SourceLength > UE_SMALL_NUMBER) ? TargetLength / SourceLength : 1.0;

		TArray<FString> Added, Overwritten, Skipped, Failed;
		int32 KeysOutside = 0;

		// Plan first so dry_run and the real write report identically.
		struct FSeqCurvePlanItem { int32 SnapIndex; bool bExists; };
		TArray<FSeqCurvePlanItem> Plan;
		for (int32 SnapIndex = 0; SnapIndex < Snapshots.Num(); ++SnapIndex)
		{
			const FAnimationCurveIdentifier Id(Snapshots[SnapIndex].Name, ERawCurveTrackTypes::RCT_Float);
			const bool bExists = TargetModel->FindFloatCurve(Id) != nullptr;
			if (bExists && !bOverwrite)
			{
				Skipped.Add(Snapshots[SnapIndex].Name.ToString());
				continue;
			}
			Plan.Add({ SnapIndex, bExists });
		}

		if (Plan.Num() > 0 && !bDryRun)
		{
			Target->Modify();
			IAnimationDataController& Controller = Target->GetController();
			Controller.OpenBracket(NSLOCTEXT("MonolithAnimationCurveAuditActions", "CopySequenceCurvesBracket", "Copy Sequence Curves"));
			for (const FSeqCurvePlanItem& Item : Plan)
			{
				const FSeqCurveSnapshot& Snap = Snapshots[Item.SnapIndex];
				const FAnimationCurveIdentifier Id(Snap.Name, ERawCurveTrackTypes::RCT_Float);
				bool bOk = Item.bExists ? Controller.SetCurveFlags(Id, Snap.Flags) : Controller.AddCurve(Id, Snap.Flags);
				if (bOk)
				{
					bOk = Controller.SetCurveKeys(Id, SeqCurveRetimedKeys(Snap.Curve, TimeScale));
				}
				if (bOk && Snap.bHasColor)
				{
					Controller.SetCurveColor(Id, Snap.Color);
				}
				if (!bOk)
				{
					Failed.Add(Snap.Name.ToString());
				}
				else
				{
					(Item.bExists ? Overwritten : Added).Add(Snap.Name.ToString());
				}
			}
			Controller.CloseBracket();
			if (Added.Num() + Overwritten.Num() > 0)
			{
				Target->MarkPackageDirty();
				++TargetsModified;
			}
		}
		else
		{
			for (const FSeqCurvePlanItem& Item : Plan)
			{
				(Item.bExists ? Overwritten : Added).Add(Snapshots[Item.SnapIndex].Name.ToString());
			}
		}

		// Keys that land outside the target's play length are kept but reported.
		for (const FSeqCurvePlanItem& Item : Plan)
		{
			for (const FRichCurveKey& Key : Snapshots[Item.SnapIndex].Curve.GetConstRefOfKeys())
			{
				const double T = static_cast<double>(Key.Time) * TimeScale;
				if (T < -UE_KINDA_SMALL_NUMBER || T > TargetLength + UE_KINDA_SMALL_NUMBER)
				{
					++KeysOutside;
				}
			}
		}

		bool bSaved = false;
		FString SaveError;
		if (bSave && !bDryRun && (Added.Num() + Overwritten.Num() > 0))
		{
			bSaved = FMonolithAssetUtils::SaveAssetPackage(Target->GetPackage(), &SaveError);
		}

		TotalAdded += Added.Num();
		TotalOverwritten += Overwritten.Num();
		TotalSkipped += Skipped.Num();

		Entry->SetNumberField(TEXT("play_length"), TargetLength);
		Entry->SetNumberField(TEXT("time_scale"), TimeScale);
		Entry->SetArrayField(bDryRun ? TEXT("would_add") : TEXT("added"), SeqCurveToJsonArray(Added));
		Entry->SetArrayField(bDryRun ? TEXT("would_overwrite") : TEXT("overwritten"), SeqCurveToJsonArray(Overwritten));
		Entry->SetArrayField(TEXT("skipped_existing"), SeqCurveToJsonArray(Skipped));
		if (Failed.Num() > 0)
		{
			Entry->SetArrayField(TEXT("failed"), SeqCurveToJsonArray(Failed));
		}
		Entry->SetNumberField(TEXT("keys_outside_play_length"), KeysOutside);
		if (bSave && !bDryRun)
		{
			Entry->SetBoolField(TEXT("saved"), bSaved);
			if (!SaveError.IsEmpty())
			{
				Entry->SetStringField(TEXT("save_error"), SaveError);
			}
		}
		TargetResults.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("source"), SourcePath);
	Root->SetNumberField(TEXT("source_play_length"), SourceLength);
	Root->SetNumberField(TEXT("source_curve_count"), Snapshots.Num());
	if (MissingOnSource.Num() > 0)
	{
		Root->SetArrayField(TEXT("missing_on_source"), SeqCurveToJsonArray(MissingOnSource));
	}
	Root->SetBoolField(TEXT("dry_run"), bDryRun);
	Root->SetNumberField(TEXT("target_count"), TargetPaths.Num());
	Root->SetNumberField(TEXT("targets_modified"), TargetsModified);
	Root->SetNumberField(bDryRun ? TEXT("curves_would_add") : TEXT("curves_added"), TotalAdded);
	Root->SetNumberField(bDryRun ? TEXT("curves_would_overwrite") : TEXT("curves_overwritten"), TotalOverwritten);
	Root->SetNumberField(TEXT("curves_skipped_existing"), TotalSkipped);
	Root->SetArrayField(TEXT("targets"), TargetResults);
	if (TargetErrors.Num() > 0)
	{
		Root->SetArrayField(TEXT("errors"), SeqCurveToJsonArray(TargetErrors));
	}
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// audit_sequence_curves
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationCurveAuditActions::HandleAuditSequenceCurves(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithSeqCurveAuditInternal;

	FString ListError;
	TArray<FString> AssetPaths;
	if (!SeqCurveReadStringList(Params, TEXT("assets"), AssetPaths, ListError))
	{
		return FMonolithActionResult::Error(ListError);
	}
	FString Folder;
	if (Params->TryGetStringField(TEXT("folder"), Folder) && !Folder.IsEmpty())
	{
		bool bRecursive = true;
		Params->TryGetBoolField(TEXT("recursive"), bRecursive);
		SeqCurveCollectFolder(Folder, bRecursive, AssetPaths);
	}
	if (AssetPaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No sequences to audit. Pass assets (array of AnimSequence paths) and/or folder."));
	}

	TArray<FString> Required;
	if (!SeqCurveReadStringList(Params, TEXT("required_curves"), Required, ListError))
	{
		return FMonolithActionResult::Error(ListError);
	}
	FString ReferencePath;
	if (Params->TryGetStringField(TEXT("reference"), ReferencePath) && !ReferencePath.IsEmpty())
	{
		UAnimSequence* Reference = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(ReferencePath);
		if (!Reference || !Reference->GetDataModel())
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Reference AnimSequence not found: %s"), *ReferencePath));
		}
		for (const FFloatCurve& Curve : Reference->GetDataModel()->GetFloatCurves())
		{
			Required.AddUnique(Curve.GetName().ToString());
		}
	}
	if (Required.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Nothing to audit against. Pass required_curves (array of curve names) and/or reference (an AnimSequence)."));
	}

	bool bOnlyFailing = false;
	Params->TryGetBoolField(TEXT("only_failing"), bOnlyFailing);

	TSet<FString> RequiredSet;
	for (const FString& Name : Required)
	{
		RequiredSet.Add(Name);
	}
	TMap<FString, int32> MissingCounts;
	TArray<TSharedPtr<FJsonValue>> Results;
	TArray<FString> LoadErrors;
	int32 Scanned = 0, Failing = 0, Complete = 0;

	for (const FString& AssetPath : AssetPaths)
	{
		UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
		const IAnimationDataModel* Model = Seq ? Seq->GetDataModel() : nullptr;
		if (!Model)
		{
			LoadErrors.Add(FString::Printf(TEXT("AnimSequence not found or has no data model: %s"), *AssetPath));
			continue;
		}
		++Scanned;

		TMap<FString, int32> Present; // name -> key count
		for (const FFloatCurve& Curve : Model->GetFloatCurves())
		{
			Present.Add(Curve.GetName().ToString(), Curve.FloatCurve.GetNumKeys());
		}

		TArray<FString> Missing, Empty, Extra;
		for (const FString& Name : Required)
		{
			if (const int32* NumKeys = Present.Find(Name))
			{
				if (*NumKeys == 0)
				{
					Empty.Add(Name);
				}
			}
			else
			{
				Missing.Add(Name);
				MissingCounts.FindOrAdd(Name)++;
			}
		}
		for (const TPair<FString, int32>& Pair : Present)
		{
			if (!RequiredSet.Contains(Pair.Key))
			{
				Extra.Add(Pair.Key);
			}
		}
		Extra.Sort();

		const bool bFails = Missing.Num() > 0 || Empty.Num() > 0;
		if (bFails) { ++Failing; } else { ++Complete; }
		if (bOnlyFailing && !bFails)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), AssetPath);
		Entry->SetNumberField(TEXT("float_curve_count"), Present.Num());
		Entry->SetBoolField(TEXT("complete"), !bFails);
		Entry->SetArrayField(TEXT("missing"), SeqCurveToJsonArray(Missing));
		Entry->SetArrayField(TEXT("empty"), SeqCurveToJsonArray(Empty));
		Entry->SetArrayField(TEXT("extra"), SeqCurveToJsonArray(Extra));
		Results.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TArray<TSharedPtr<FJsonValue>> MissingSummary;
	for (const FString& Name : Required)
	{
		if (const int32* Count = MissingCounts.Find(Name))
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("curve"), Name);
			Obj->SetNumberField(TEXT("missing_in"), *Count);
			MissingSummary.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("required_curves"), SeqCurveToJsonArray(Required));
	if (!ReferencePath.IsEmpty())
	{
		Root->SetStringField(TEXT("reference"), ReferencePath);
	}
	Root->SetNumberField(TEXT("sequences_scanned"), Scanned);
	Root->SetNumberField(TEXT("sequences_complete"), Complete);
	Root->SetNumberField(TEXT("sequences_failing"), Failing);
	Root->SetArrayField(TEXT("missing_summary"), MissingSummary);
	Root->SetArrayField(TEXT("sequences"), Results);
	if (LoadErrors.Num() > 0)
	{
		Root->SetArrayField(TEXT("errors"), SeqCurveToJsonArray(LoadErrors));
	}
	return FMonolithActionResult::Success(Root);
}
