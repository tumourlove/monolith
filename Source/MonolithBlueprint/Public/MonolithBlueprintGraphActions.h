#pragma once
#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithBlueprintGraphActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult HandleAddFunction(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleOverrideParentFunction(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveFunction(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRenameFunction(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddMacro(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveMacro(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRenameMacro(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddEventDispatcher(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetFunctionParams(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleImplementInterface(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveInterface(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleReparentBlueprint(const TSharedPtr<FJsonObject>& Params);

	// Wave 5 — Scaffolding & Templates
	static FMonolithActionResult HandleScaffoldInterfaceImplementation(const TSharedPtr<FJsonObject>& Params);

	// Wave 6 — Event Dispatcher CRUD
	static FMonolithActionResult HandleRemoveEventDispatcher(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetEventDispatcherParams(const TSharedPtr<FJsonObject>& Params);
};
