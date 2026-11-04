#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Write half of the `gameplay_tags` namespace: declaring, editing, renaming, moving and
 * removing ini-declared gameplay tags.
 *
 * Every write goes through IGameplayTagsEditorModule, the same path the Gameplay
 * Tags page of Project Settings takes, so source control checkout, the ini save
 * and the tag tree refresh behave exactly as they do in the editor UI. The one
 * exception is the dev comment, which the module cannot edit (UpdateTagInINI only
 * touches the restricted-children flag); that edit is made on the declaring tag
 * list directly and saved the way the module saves it.
 *
 * The module reports a refusal only as an editor toast and a log line, never to
 * the caller, so every action validates up front and returns the reason itself.
 * Native tags (declared in C++) and DataTable tags are outside what an ini edit
 * can change, and the actions say so instead of silently doing nothing.
 */
class FMonolithGameplayTagsWriteActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult ListSources(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddSource(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult Add(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult Update(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult Remove(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult Rename(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult Move(const TSharedPtr<FJsonObject>& Params);
};
