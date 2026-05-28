#include "MonolithGuideTool.h"
#include "MonolithJsonUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Optional.h"
#include "HAL/CriticalSection.h"
#include "Interfaces/IPluginManager.h"

// ============================================================================
// Canonical section ordering. The H2 splitter is content-count-agnostic (it
// keys whatever "## <name>" headers exist in the markdown), but the index and
// the unknown-section validation list these six in this exact order so the
// response is deterministic regardless of authoring order in the .md file.
// ============================================================================
static const TArray<FString>& GetCanonicalSectionOrder()
{
	static const TArray<FString> Order = {
		TEXT("onboarding"),
		TEXT("recipes"),
		TEXT("decisions"),
		TEXT("errors"),
		TEXT("skills_map"),
		TEXT("gotchas")
	};
	return Order;
}

// Build a human-readable "onboarding, recipes, decisions, errors, skills_map, gotchas"
// list for the index and the unknown-section error message.
static FString JoinCanonicalSectionNames()
{
	return FString::Join(GetCanonicalSectionOrder(), TEXT(", "));
}

void FMonolithGuideTool::RegisterAll()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> SectionProp = MakeShared<FJsonObject>();
	SectionProp->SetStringField(TEXT("type"), TEXT("string"));
	SectionProp->SetStringField(TEXT("description"),
		TEXT("Optional: return a single H2 section to bound context cost. One of: onboarding, recipes, decisions, errors, skills_map, gotchas. Omit for the full index + all sections."));
	Schema->SetObjectField(TEXT("section"), SectionProp);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("guide"),
		TEXT("Editorial cross-namespace workflow guide for AI agents: onboarding script, cross-namespace recipes, X-vs-Y decision matrices, error-to-recovery maps, namespace->Skill pointers, and Monolith-specific gotchas. Section-keyed (pass section=\"recipes\") to bound context cost. Hand-authored markdown + a live registry overlay (per-namespace action counts, plugin version)."),
		FMonolithActionHandler::CreateStatic(&FMonolithGuideTool::HandleGuide),
		Schema
	);
}

bool FMonolithGuideTool::LoadGuideMarkdown(FString& OutMarkdown, FString& OutErrorMessage)
{
	// Session-lived cache of the successfully-loaded markdown. Guarded by an
	// explicit critical section so a future force-reload param can invalidate
	// it safely (deferred). HandleGuide runs on the HTTP worker thread.
	static FCriticalSection CacheLock;
	static TOptional<FString> CachedMarkdown;

	{
		FScopeLock Lock(&CacheLock);
		if (CachedMarkdown.IsSet())
		{
			OutMarkdown = CachedMarkdown.GetValue();
			return true;
		}
	}

	// Resolve the plugin base dir, then Docs/MONOLITH_GUIDE.md.
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
	if (!Plugin.IsValid())
	{
		OutErrorMessage = TEXT("Could not resolve the 'Monolith' plugin via IPluginManager — the plugin is not mounted. This should be impossible from inside a Monolith action; report at github.com/tumourlove/monolith.");
		return false;
	}

	const FString GuidePath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Docs"), TEXT("MONOLITH_GUIDE.md"));

	FString FileContents;
	if (!FFileHelper::LoadFileToString(FileContents, *GuidePath))
	{
		OutErrorMessage = FString::Printf(
			TEXT("Guide markdown not found or unreadable at '%s'. Restore Docs/MONOLITH_GUIDE.md (it ships in the release zip) or pull the latest Monolith release; then call monolith_guide() again."),
			*GuidePath);
		return false;
	}

	{
		FScopeLock Lock(&CacheLock);
		CachedMarkdown = FileContents;
	}

	OutMarkdown = MoveTemp(FileContents);
	return true;
}

void FMonolithGuideTool::SplitSections(const FString& Markdown, TMap<FString, FString>& OutSections, TArray<FString>& OutOrderedNames)
{
	OutSections.Reset();
	OutOrderedNames.Reset();

	// Preserve blank lines (bCullEmpty=false) so paragraph spacing and fenced
	// code blocks survive inside section bodies.
	TArray<FString> Lines;
	Markdown.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);

	FString CurrentName;
	FString CurrentBody;
	bool bInSection = false;

	auto FlushCurrent = [&OutSections, &OutOrderedNames, &CurrentName, &CurrentBody]()
	{
		if (!CurrentName.IsEmpty())
		{
			// Trim trailing whitespace/newlines accumulated before the next header.
			OutSections.Add(CurrentName, CurrentBody.TrimEnd());
			OutOrderedNames.Add(CurrentName);
		}
	};

	for (const FString& Line : Lines)
	{
		// An H2 header is exactly "## " — NOT "### " (H3) and NOT "#" (H1 title).
		// TrimStartAndEnd handles a leading-space-tolerant match without catching H3.
		const FString Trimmed = Line.TrimStartAndEnd();
		if (Trimmed.StartsWith(TEXT("## ")) && !Trimmed.StartsWith(TEXT("### ")))
		{
			// Close the previous section before opening the new one.
			FlushCurrent();

			// Section key = remainder after "## ", trimmed and lowercased so it
			// matches the canonical lowercase keys regardless of authoring case.
			CurrentName = Trimmed.RightChop(3).TrimStartAndEnd().ToLower();
			CurrentBody.Reset();
			bInSection = true;
			continue;
		}

		if (bInSection)
		{
			CurrentBody += Line;
			CurrentBody += TEXT("\n");
		}
		// Content before the first H2 (the H1 title + intro prose) is intentionally
		// dropped — it is not a keyed section and the overlay carries the metadata.
	}

	FlushCurrent();
}

TSharedPtr<FJsonObject> FMonolithGuideTool::BuildLiveOverlay()
{
	TSharedPtr<FJsonObject> Overlay = MakeShared<FJsonObject>();

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// Per-namespace action counts. Each GetActions call FScopeLocks RegistryLock
	// internally; no additional lock needed here (same precedent as HandleDiscover).
	TArray<FString> Namespaces = Registry.GetNamespaces();
	Namespaces.Sort();

	TArray<TSharedPtr<FJsonValue>> NsArray;
	for (const FString& Ns : Namespaces)
	{
		TSharedPtr<FJsonObject> NsObj = MakeShared<FJsonObject>();
		NsObj->SetStringField(TEXT("name"), Ns);
		NsObj->SetNumberField(TEXT("action_count"), Registry.GetActions(Ns).Num());
		// A namespace that appears in GetNamespaces() has registered actions, so
		// it is by definition available in this editor session.
		NsObj->SetBoolField(TEXT("available"), true);
		NsArray.Add(MakeShared<FJsonValueObject>(NsObj));
	}
	Overlay->SetArrayField(TEXT("namespaces"), NsArray);
	Overlay->SetNumberField(TEXT("total_actions"), Registry.GetActionCount());

	// Plugin version from the descriptor.
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
	if (Plugin.IsValid())
	{
		Overlay->SetStringField(TEXT("plugin_version"), Plugin->GetDescriptor().VersionName);
	}

	return Overlay;
}

FMonolithActionResult FMonolithGuideTool::HandleGuide(const TSharedPtr<FJsonObject>& Params)
{
	// Load + split the editorial markdown.
	FString Markdown;
	FString LoadError;
	if (!LoadGuideMarkdown(Markdown, LoadError))
	{
		return FMonolithActionResult::Error(LoadError, FMonolithJsonUtils::ErrInternalError);
	}

	TMap<FString, FString> Sections;
	TArray<FString> OrderedNames;
	SplitSections(Markdown, Sections, OrderedNames);

	const TArray<FString>& CanonicalOrder = GetCanonicalSectionOrder();

	// Optional section filter.
	FString RequestedSection;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("section"), RequestedSection);
		RequestedSection = RequestedSection.TrimStartAndEnd().ToLower();
	}

	// ----- Section-keyed dispatch -----
	if (!RequestedSection.IsEmpty())
	{
		const FString* Body = Sections.Find(RequestedSection);
		if (!Body)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Unknown section '%s'. Valid sections: %s."),
					*RequestedSection, *JoinCanonicalSectionNames()),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("section"), RequestedSection);
		Result->SetStringField(TEXT("content"), *Body);

		// next_sections = the other canonical sections (so the agent can chain
		// follow-up pulls without re-fetching the index).
		TArray<TSharedPtr<FJsonValue>> NextSections;
		for (const FString& Name : CanonicalOrder)
		{
			if (!Name.Equals(RequestedSection) && Sections.Contains(Name))
			{
				NextSections.Add(MakeShared<FJsonValueString>(Name));
			}
		}
		Result->SetArrayField(TEXT("next_sections"), NextSections);

		Result->SetObjectField(TEXT("live_overlay"), BuildLiveOverlay());
		return FMonolithActionResult::Success(Result);
	}

	// ----- No-arg dispatch: index + all section bodies + overlay -----
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
	if (Plugin.IsValid())
	{
		Result->SetStringField(TEXT("version"), Plugin->GetDescriptor().VersionName);
	}

	// sections = ordered list of the canonical section names that are present.
	TArray<TSharedPtr<FJsonValue>> SectionNames;
	TSharedPtr<FJsonObject> ContentObj = MakeShared<FJsonObject>();
	for (const FString& Name : CanonicalOrder)
	{
		const FString* Body = Sections.Find(Name);
		if (Body)
		{
			SectionNames.Add(MakeShared<FJsonValueString>(Name));
			ContentObj->SetStringField(Name, *Body);
		}
	}
	Result->SetArrayField(TEXT("sections"), SectionNames);
	Result->SetObjectField(TEXT("content"), ContentObj);

	Result->SetObjectField(TEXT("live_overlay"), BuildLiveOverlay());

	return FMonolithActionResult::Success(Result);
}
