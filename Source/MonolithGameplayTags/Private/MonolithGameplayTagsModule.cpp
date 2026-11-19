#include "MonolithGameplayTagsModule.h"
#include "MonolithGameplayTagsActions.h"
#include "MonolithGameplayTagsWriteActions.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"

#define LOCTEXT_NAMESPACE "FMonolithGameplayTagsModule"

DEFINE_LOG_CATEGORY(LogMonolithGameplayTags);

namespace
{
	const TCHAR* const TagsNamespace = TEXT("gameplay_tags");
}

void FMonolithGameplayTagsModule::StartupModule()
{
	// Gameplay tags are an engine feature every project has, so this namespace is
	// independent of the GAS toggle: turning GAS off must not take tag lookups and
	// tag edits with it.
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableGameplayTags)
	{
		UE_LOG(LogMonolithGameplayTags, Log, TEXT("MonolithGameplayTags: disabled in settings"));
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithGameplayTagsActions::RegisterActions(Registry);
	FMonolithGameplayTagsWriteActions::RegisterActions(Registry);

	// The namespace declares, renames and deletes tags in config files, so the
	// dispatcher is advertised as writing and destructive. The hints are per
	// namespace, not per action; the read actions stay harmless regardless.
	FMonolithDispatcherAnnotations Annotations;
	Annotations.bReadOnlyHint = false;
	Annotations.bDestructiveHint = true;
	Annotations.bIdempotentHint = false;
	Annotations.Title = TEXT("Gameplay Tags");
	Registry.SetDispatcherAnnotations(TagsNamespace, Annotations);

	UE_LOG(LogMonolithGameplayTags, Log, TEXT("MonolithGameplayTags: Loaded (%d actions)"),
		Registry.GetActions(TagsNamespace).Num());
}

void FMonolithGameplayTagsModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TagsNamespace);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithGameplayTagsModule, MonolithGameplayTags)
