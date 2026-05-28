#include "MonolithBAFormatterImpl.h"

#if WITH_BLUEPRINT_ASSIST

#include "BlueprintAssistTabHandler.h"
#include "BlueprintAssistGraphHandler.h"
#include "BlueprintAssistSettings.h"
#include "BlueprintAssistUtils.h"
#include "EdGraph/EdGraph.h"

bool FMonolithBAFormatterImpl::SupportsGraph(UEdGraph* Graph) const
{
	if (!Graph)
	{
		return false;
	}

	FBAFormatterSettings* Settings = UBASettings::FindFormatterSettings(Graph);
	if (Settings && Settings->bEnabled)
	{
		return true;
	}

	return FBAUtils::IsBlueprintGraph(Graph);
}

bool FMonolithBAFormatterImpl::FormatGraph(
	UEdGraph* Graph,
	int32& OutNodesFormatted,
	FString& OutErrorMessage)
{
	if (!Graph)
	{
		OutErrorMessage = TEXT("Graph is null");
		return false;
	}

	// 1. Find BA's handler for this graph
	TSharedPtr<FBAGraphHandler> Handler = FindHandlerForGraph(Graph);
	if (!Handler.IsValid())
	{
		OutErrorMessage = TEXT("No active editor tab found for this graph. "
			"The asset must be open in the editor for BA formatting to work.");
		return false;
	}

	// 2. Check if BA is still caching node sizes
	if (Handler->IsCalculatingNodeSize())
	{
		OutErrorMessage = TEXT("Blueprint Assist is still caching node sizes. "
			"Try again in a moment.");
		return false;
	}

	// 3. Check if this graph type is supported
	if (!SupportsGraph(Graph))
	{
		OutErrorMessage = FString::Printf(
			TEXT("BA does not have formatter settings for graph type '%s'"),
			*Graph->GetClass()->GetName());
		return false;
	}

	// 4. Dispatch formatting
	// FormatAllEvents() and SimpleFormatAll() create their own FScopedTransaction
	// -- do NOT wrap in another transaction.
	FBAFormatterSettings* Settings = UBASettings::FindFormatterSettings(Graph);
	if (!Settings || FBAUtils::IsBlueprintGraph(Graph))
	{
		// Multi-root Blueprint graphs use FormatAllEvents
		Handler->FormatAllEvents();
	}
	else
	{
		// Simple/BT formatter
		Handler->SimpleFormatAll();
	}

	// 5. Report node count
	OutNodesFormatted = Graph->Nodes.Num();

	return true;
}

FMonolithFormatterInfo FMonolithBAFormatterImpl::GetFormatterInfo(UEdGraph* Graph) const
{
	FMonolithFormatterInfo Info;

	if (!Graph)
	{
		Info.FormatterType = TEXT("Unsupported");
		Info.GraphClassName = TEXT("None");
		return Info;
	}

	Info.GraphClassName = Graph->GetClass()->GetName();

	FBAFormatterSettings* Settings = UBASettings::FindFormatterSettings(Graph);
	if (Settings)
	{
		switch (Settings->FormatterType)
		{
		case EBAFormatterType::Blueprint:
			Info.FormatterType = TEXT("Blueprint");
			break;
		case EBAFormatterType::BehaviorTree:
			Info.FormatterType = TEXT("BehaviorTree");
			break;
		case EBAFormatterType::Simple:
			Info.FormatterType = TEXT("Simple");
			break;
		default:
			Info.FormatterType = TEXT("Unknown");
			break;
		}
		Info.bIsSupported = Settings->bEnabled;
	}
	else if (FBAUtils::IsBlueprintGraph(Graph))
	{
		Info.FormatterType = TEXT("Blueprint");
		Info.bIsSupported = true;
	}
	else
	{
		Info.FormatterType = TEXT("Unsupported");
		Info.bIsSupported = false;
	}

	return Info;
}

TSharedPtr<FBAGraphHandler> FMonolithBAFormatterImpl::FindHandlerForGraph(UEdGraph* Graph) const
{
	if (!Graph)
	{
		return nullptr;
	}

	TArray<TSharedPtr<FBAGraphHandler>> AllHandlers =
		FBATabHandler::Get().GetAllGraphHandlers();

	// Tier 1: pointer equality (most reliable, works 99% of the time)
	for (const TSharedPtr<FBAGraphHandler>& Handler : AllHandlers)
	{
		if (Handler.IsValid() && Handler->GetFocusedEdGraph() == Graph)
		{
			return Handler;
		}
	}

	// Tier 2: GraphGuid match (covers case where different UEdGraph*
	// points to the same logical graph -- shouldn't happen but defensive)
	for (const TSharedPtr<FBAGraphHandler>& Handler : AllHandlers)
	{
		if (!Handler.IsValid())
		{
			continue;
		}
		UEdGraph* HGraph = Handler->GetFocusedEdGraph();
		if (HGraph && HGraph->GraphGuid == Graph->GraphGuid)
		{
			return Handler;
		}
	}

	// Tier 3: Outer + Name match (last resort for multi-graph assets
	// where the focused graph in a tab doesn't match our target)
	for (const TSharedPtr<FBAGraphHandler>& Handler : AllHandlers)
	{
		if (!Handler.IsValid())
		{
			continue;
		}
		UEdGraph* HGraph = Handler->GetFocusedEdGraph();
		if (HGraph
			&& HGraph->GetOuter() == Graph->GetOuter()
			&& HGraph->GetName() == Graph->GetName())
		{
			return Handler;
		}
	}

	return nullptr;
}

#endif // WITH_BLUEPRINT_ASSIST
