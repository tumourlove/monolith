#include "Actions/ProjectExportAssetTextAction.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"
#include "Exporters/Exporter.h"
#include "UnrealExporter.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "Misc/StringOutputDevice.h"
#else
// UE 5.6 hasn't split FStringOutputDevice out of UnrealString.h yet.
#include "Containers/UnrealString.h"
#endif
#include "UObject/UObjectHash.h"
#include "UObject/UObjectMarks.h"

namespace
{
	// Default byte budget for a returned T3D dump (256 KB). T3D for a large,
	// fully-wired asset can be multiple MB -- the budget plus grep_pattern are
	// the safety rail (a silent mid-object truncation produces misleading grep
	// results, so we hard-error past the budget instead).
	constexpr int32 DefaultMaxBytes = 256 * 1024;

	// Absolute ceiling on the caller-supplied max_bytes. Asking past this is a
	// hard error -- the action is an inspection escape hatch, not a bulk dumper.
	constexpr int32 MaxBytesCeiling = 4 * 1024 * 1024;

	// Lines of context emitted on each side of a grep match.
	constexpr int32 GrepContextLines = 3;

	/**
	 * Filter T3D text to only the lines matching Pattern (case-insensitive
	 * substring), each surrounded by GrepContextLines of context. Disjoint
	 * windows are separated by a "..." marker line. Returns the number of
	 * matching lines via OutMatchCount.
	 */
	FString GrepWithContext(const FString& Text, const FString& Pattern, int32& OutMatchCount)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);

		const FString PatternLower = Pattern.ToLower();
		TArray<bool> Keep;
		Keep.Init(false, Lines.Num());
		OutMatchCount = 0;

		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			if (Lines[i].ToLower().Contains(PatternLower))
			{
				++OutMatchCount;
				const int32 Start = FMath::Max(0, i - GrepContextLines);
				const int32 End = FMath::Min(Lines.Num() - 1, i + GrepContextLines);
				for (int32 k = Start; k <= End; ++k)
				{
					Keep[k] = true;
				}
			}
		}

		FString Out;
		bool bPrevKept = false;
		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			if (Keep[i])
			{
				if (!bPrevKept && !Out.IsEmpty())
				{
					Out += TEXT("...\n");
				}
				Out += Lines[i];
				Out += TEXT("\n");
				bPrevKept = true;
			}
			else
			{
				bPrevKept = false;
			}
		}
		return Out;
	}

	/** Locate a sub-object of Asset whose object name OR class name contains Filter (case-insensitive). */
	UObject* FindSubObjectByFilter(UObject* Asset, const FString& Filter, TArray<FString>& OutCandidates)
	{
		TArray<UObject*> Inners;
		GetObjectsWithOuter(Asset, Inners, /*bIncludeNestedObjects=*/true);

		const FString FilterLower = Filter.ToLower();
		UObject* Match = nullptr;
		for (UObject* Inner : Inners)
		{
			if (!Inner)
			{
				continue;
			}
			OutCandidates.Add(FString::Printf(TEXT("%s (%s)"), *Inner->GetName(), *Inner->GetClass()->GetName()));
			if (!Match)
			{
				const bool bNameHit = Inner->GetName().ToLower().Contains(FilterLower);
				const bool bClassHit = Inner->GetClass()->GetName().ToLower().Contains(FilterLower);
				if (bNameHit || bClassHit)
				{
					Match = Inner;
				}
			}
		}
		return Match;
	}
}

FString FProjectExportAssetTextAction::GetDescription()
{
	return TEXT("Universal escape hatch: export an asset to its native T3D text dump (or grepped excerpts) "
		"and return it directly. PREFER the typed read actions first -- get_node_details for Blueprint/AnimGraph "
		"nodes, inspect_chooser for chooser tables, list_graphs for graph structure. Reach for export_asset_text "
		"only when no typed action exposes the surface you need. T3D for a large asset is big: scope with "
		"object_filter and/or narrow with grep_pattern, and respect max_bytes (hard error past the budget rather "
		"than silent truncation).");
}

FMonolithActionResult FProjectExportAssetTextAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'asset_path' parameter is required"), -32602);
	}

	// Resolve max_bytes (optional). Hard-error if the caller asks past the ceiling.
	int32 MaxBytes = DefaultMaxBytes;
	double MaxBytesRaw = 0.0;
	if (Params->TryGetNumberField(TEXT("max_bytes"), MaxBytesRaw))
	{
		MaxBytes = static_cast<int32>(MaxBytesRaw);
		if (MaxBytes <= 0)
		{
			return FMonolithActionResult::Error(TEXT("'max_bytes' must be a positive integer"), -32602);
		}
		if (MaxBytes > MaxBytesCeiling)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("'max_bytes' (%d) exceeds the ceiling of %d. Scope the export with object_filter and/or "
					"grep_pattern instead of raising the budget."), MaxBytes, MaxBytesCeiling), -32602);
		}
	}

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	// Optional: scope to a sub-object by name/class substring.
	UObject* ExportRoot = Asset;
	FString ResolvedObject;
	const FString ObjectFilter = Params->GetStringField(TEXT("object_filter"));
	if (!ObjectFilter.IsEmpty())
	{
		TArray<FString> Candidates;
		UObject* SubObject = FindSubObjectByFilter(Asset, ObjectFilter, Candidates);
		if (!SubObject)
		{
			FString Hint = Candidates.Num() > 0
				? FString::Printf(TEXT(" Available sub-objects: %s"),
					*FString::Join(Candidates.Num() > 30 ? TArray<FString>(Candidates.GetData(), 30) : Candidates, TEXT(", ")))
				: FString(TEXT(" (asset has no sub-objects)"));
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("No sub-object matched object_filter '%s' in %s.%s"), *ObjectFilter, *AssetPath, *Hint));
		}
		ExportRoot = SubObject;
		ResolvedObject = FString::Printf(TEXT("%s (%s)"), *SubObject->GetName(), *SubObject->GetClass()->GetName());
	}

	// Native T3D export to string (established copy/clipboard pattern:
	// UnMarkAllObjects then ExportToOutputDevice into an FStringOutputDevice).
	UnMarkAllObjects(EObjectMark(OBJECTMARK_TagExp | OBJECTMARK_TagImp));
	FStringOutputDevice Buffer;
	const FExportObjectInnerContext Context;
	UExporter::ExportToOutputDevice(&Context, ExportRoot, /*InExporter=*/nullptr, Buffer,
		TEXT("T3D"), /*Indent=*/0, PPF_Copy, /*bInSelectedOnly=*/false);

	FString FullText = MoveTemp(Buffer);
	const int32 FullBytes = FullText.Len();

	// Optional grep narrowing.
	const FString GrepPattern = Params->GetStringField(TEXT("grep_pattern"));
	FString PayloadText = FullText;
	int32 MatchCount = 0;
	const bool bGrepped = !GrepPattern.IsEmpty();
	if (bGrepped)
	{
		PayloadText = GrepWithContext(FullText, GrepPattern, MatchCount);
	}

	const int32 PayloadBytes = PayloadText.Len();

	// Byte-budget enforcement: hard error rather than silent truncation.
	if (PayloadBytes > MaxBytes)
	{
		FString Advice;
		if (!bGrepped)
		{
			Advice = TEXT(" Narrow it with grep_pattern, scope it with object_filter, or raise max_bytes (up to the ceiling).");
		}
		else
		{
			Advice = TEXT(" The grep_pattern still matches too much -- use a more specific pattern, add object_filter, or raise max_bytes (up to the ceiling).");
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Export payload is %d bytes, over the max_bytes budget of %d.%s"),
			PayloadBytes, MaxBytes, *Advice));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	if (!ResolvedObject.IsEmpty())
	{
		Result->SetStringField(TEXT("object"), ResolvedObject);
	}
	Result->SetNumberField(TEXT("full_bytes"), FullBytes);
	Result->SetNumberField(TEXT("returned_bytes"), PayloadBytes);
	if (bGrepped)
	{
		Result->SetStringField(TEXT("grep_pattern"), GrepPattern);
		Result->SetNumberField(TEXT("match_count"), MatchCount);
	}
	Result->SetStringField(TEXT("text"), PayloadText);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectExportAssetTextAction::GetSchema()
{
	return FParamSchemaBuilder()
		.RequiredAssetPath(TEXT("asset_path"), TEXT("Package path of the asset to export (e.g. /Game/Path/MyAsset)"))
		.Optional(TEXT("object_filter"), TEXT("string"),
			TEXT("Optional name/class substring (case-insensitive) scoping the export to a single matching sub-object instead of the whole asset"))
		.Optional(TEXT("grep_pattern"), TEXT("string"),
			TEXT("Optional case-insensitive substring; returns only matching lines plus a few lines of surrounding context"))
		.Optional(TEXT("max_bytes"), TEXT("number"),
			TEXT("Optional byte budget for the returned text (default 262144). Hard error if the payload exceeds it -- narrow with grep_pattern/object_filter rather than expecting silent truncation"))
		.Build();
}
