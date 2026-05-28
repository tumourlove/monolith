#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Editorial cross-namespace workflow guide for AI agents — primary audience is
 * EXTERNAL public-Monolith users with no project CLAUDE.md routing or private skills.
 * Hybrid: hand-authored markdown at Plugins/Monolith/Docs/MONOLITH_GUIDE.md
 * + live registry overlay (action counts, gate status, plugin version).
 * Registered under the "monolith" namespace as action "guide".
 */
class FMonolithGuideTool
{
public:
	/** Register the guide action (called from FMonolithCoreTools::RegisterAll). */
	static void RegisterAll();

	/** monolith.guide — return the editorial guide, optionally filtered to a named H2 section. */
	static FMonolithActionResult HandleGuide(const TSharedPtr<FJsonObject>& Params);

private:
	/** Load MONOLITH_GUIDE.md from the plugin Docs/ dir. Cached after first successful load. */
	static bool LoadGuideMarkdown(FString& OutMarkdown, FString& OutErrorMessage);

	/** Split markdown into named sections keyed by H2 header ("## <name>"). */
	static void SplitSections(const FString& Markdown, TMap<FString, FString>& OutSections, TArray<FString>& OutOrderedNames);

	/** Live overlay JSON: per-namespace action counts + gate status + plugin version. */
	static TSharedPtr<FJsonObject> BuildLiveOverlay();
};
