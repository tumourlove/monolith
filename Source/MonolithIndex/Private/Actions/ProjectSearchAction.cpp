#include "Actions/ProjectSearchAction.h"
#include "MonolithIndexDatabase.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

// Named (never anonymous) so the forced-full-unity release pass cannot collide
// this constant with a same-named file-local in a sibling translation unit.
namespace MonolithProjectSearchActionDetail
{
	/**
	 * Upper bound on the query string. SQLite's own MATCH grammar is bounded and
	 * the query is bound as a parameter, so this is defence in depth: it caps the
	 * parse work one MCP call can hand the game thread, and it is the standing
	 * mitigation if any query-analysing layer is ever added above the bound MATCH.
	 */
	static constexpr int32 MaxQueryLength = 4096;

	/**
	 * Upper bound on the asset_class filter list. Each entry becomes one bound
	 * placeholder in an IN clause, so this caps how large a statement a single MCP
	 * call can ask SQLite to prepare. Well above any real filter -- the project has
	 * a few dozen asset classes in total.
	 */
	static constexpr int32 MaxClassFilters = 32;
}

FMonolithActionResult FProjectSearchAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("query"), Query))
	{
		return FMonolithActionResult::Error(TEXT("'query' must be a string"), -32602);
	}

	Query.TrimStartAndEndInline();
	if (Query.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'query' must not be empty"), -32602);
	}
	if (Query.Len() > MonolithProjectSearchActionDetail::MaxQueryLength)
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("'query' must be %d characters or fewer (got %d)"),
				MonolithProjectSearchActionDetail::MaxQueryLength,
				Query.Len()),
			-32602);
	}

	int32 Limit = 50;
	if (Params->HasField(TEXT("limit")))
	{
		double RawLimit = 0.0;
		if (!Params->TryGetNumberField(TEXT("limit"), RawLimit)
			|| !FMath::IsFinite(RawLimit)
			|| FMath::FloorToDouble(RawLimit) != RawLimit)
		{
			return FMonolithActionResult::Error(TEXT("'limit' must be an integer"), -32602);
		}
		Limit = static_cast<int32>(FMath::Clamp(RawLimit, 1.0, 1000.0));
	}

	// asset_class accepts a bare string or an array of strings, matching how other
	// actions take single-or-many. Absent means no filtering, which is the old behaviour.
	//
	// Type is checked against EJson rather than via TryGetStringField, because that
	// coerces: a numeric 7 would come back as the string "7" and become a filter for a
	// class named "7" -- zero results dressed up as a valid search instead of a rejection.
	TArray<FString> AssetClassFilter;
	const TSharedPtr<FJsonValue> ClassField = Params->TryGetField(TEXT("asset_class"));
	if (ClassField.IsValid() && ClassField->Type != EJson::Null)
	{
		auto AddClassName = [&AssetClassFilter](FString ClassName)
		{
			ClassName.TrimStartAndEndInline();
			if (!ClassName.IsEmpty())
			{
				AssetClassFilter.AddUnique(ClassName);
			}
		};

		if (ClassField->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Entry : ClassField->AsArray())
			{
				if (!Entry.IsValid() || Entry->Type != EJson::String)
				{
					return FMonolithActionResult::Error(
						TEXT("'asset_class' array entries must be strings"), -32602);
				}
				AddClassName(Entry->AsString());
			}
		}
		else if (ClassField->Type == EJson::String)
		{
			AddClassName(ClassField->AsString());
		}
		else
		{
			return FMonolithActionResult::Error(
				TEXT("'asset_class' must be a string or an array of strings"), -32602);
		}

		// A filter that survives trimming to nothing is a caller mistake, not a request
		// for unfiltered results -- silently widening the search would be the wrong guess.
		if (AssetClassFilter.Num() == 0)
		{
			return FMonolithActionResult::Error(
				TEXT("'asset_class' must name at least one non-empty class"), -32602);
		}
		if (AssetClassFilter.Num() > MonolithProjectSearchActionDetail::MaxClassFilters)
		{
			return FMonolithActionResult::Error(
				FString::Printf(
					TEXT("'asset_class' must list %d classes or fewer (got %d)"),
					MonolithProjectSearchActionDetail::MaxClassFilters,
					AssetClassFilter.Num()),
				-32602);
		}
	}

	UMonolithIndexSubsystem* Subsystem = GEditor
		? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>()
		: nullptr;
	if (!Subsystem)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem not available"));
	}

	if (Subsystem->IsIndexing())
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Indexing is currently in progress"));
		Result->SetNumberField(TEXT("progress"), Subsystem->GetProgress());
		return FMonolithActionResult::Success(Result);
	}

	// Go straight to the database so a caller's bad query stays distinguishable
	// from an index failure; the subsystem's own Search() wrapper cannot report
	// which of the two happened.
	FMonolithIndexDatabase* Database = Subsystem->GetDatabase();
	if (!Database || !Database->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Project index database is not open"));
	}

	TArray<FSearchResult> SearchResults;
	FString SearchError;
	const EMonolithProjectSearchStatus SearchStatus =
		Database->FullTextSearch(Query, Limit, SearchResults, SearchError, AssetClassFilter);
	if (SearchStatus != EMonolithProjectSearchStatus::Succeeded)
	{
		const bool bInvalidQuery = SearchStatus == EMonolithProjectSearchStatus::InvalidQuery;
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("%s: %s"),
				bInvalidQuery ? TEXT("Invalid FTS5 query") : TEXT("Project search failed"),
				*SearchError),
			bInvalidQuery ? -32602 : -32603);
	}

	auto Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ResultsArr;
	for (const FSearchResult& SR : SearchResults)
	{
		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), SR.AssetPath);
		Entry->SetStringField(TEXT("asset_name"), SR.AssetName);
		Entry->SetStringField(TEXT("asset_class"), SR.AssetClass);
		Entry->SetStringField(TEXT("module_name"), SR.ModuleName);
		Entry->SetStringField(TEXT("match_context"), SR.MatchContext);
		Entry->SetNumberField(TEXT("rank"), SR.Rank);
		ResultsArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("results"), ResultsArr);
	Result->SetNumberField(TEXT("count"), SearchResults.Num());

	// Echo the applied filter so a zero-result response is self-explaining: the caller
	// can see whether the filter was understood without re-running the query.
	if (AssetClassFilter.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> FilterArr;
		for (const FString& ClassName : AssetClassFilter)
		{
			FilterArr.Add(MakeShared<FJsonValueString>(ClassName));
		}
		Result->SetArrayField(TEXT("asset_class_filter"), FilterArr);
	}

	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectSearchAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("query"), TEXT("string"), TEXT("FTS5 search query (supports AND, OR, NOT, quoted phrases, prefix*, NEAR(a b, N)); max 4096 characters"))
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum results to return (clamped to 1-1000)"), TEXT("50"))
		.Optional(TEXT("asset_class"), TEXT("string|array"), TEXT("Restrict results to these asset classes, e.g. \"Blueprint\" or [\"Blueprint\",\"WidgetBlueprint\"]. Case-insensitive; max 32 entries. Applied inside the query, so 'limit' counts matching rows only. Node-text hits are filtered by their OWNING asset's class"), TEXT("(unfiltered)"))
		.Build();
}
