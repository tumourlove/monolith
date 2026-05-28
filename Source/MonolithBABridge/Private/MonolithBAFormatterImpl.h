#pragma once
#include "IMonolithGraphFormatter.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMonolithBABridge, Log, All);

#if WITH_BLUEPRINT_ASSIST

class FBAGraphHandler;

class FMonolithBAFormatterImpl : public IMonolithGraphFormatter
{
public:
	virtual bool SupportsGraph(UEdGraph* Graph) const override;
	virtual bool FormatGraph(UEdGraph* Graph, int32& OutNodesFormatted,
		FString& OutErrorMessage) override;
	virtual FMonolithFormatterInfo GetFormatterInfo(UEdGraph* Graph) const override;

private:
	/**
	 * Find BA's active handler for a graph. Three-tier fallback:
	 *  1. Pointer equality on GetFocusedEdGraph()
	 *  2. GraphGuid match
	 *  3. GetOuter() + GetName() match
	 */
	TSharedPtr<FBAGraphHandler> FindHandlerForGraph(UEdGraph* Graph) const;
};

#endif // WITH_BLUEPRINT_ASSIST
