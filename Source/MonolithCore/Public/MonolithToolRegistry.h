#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Result of an action execution */
struct FMonolithActionResult
{
	bool bSuccess = false;
	TSharedPtr<FJsonObject> Result;
	FString ErrorMessage;
	int32 ErrorCode = 0;

	static FMonolithActionResult Success(const TSharedPtr<FJsonObject>& InResult)
	{
		FMonolithActionResult R;
		R.bSuccess = true;
		R.Result = InResult;
		return R;
	}

	static FMonolithActionResult Error(const FString& Message, int32 Code = -32603)
	{
		FMonolithActionResult R;
		R.bSuccess = false;
		R.ErrorMessage = Message;
		R.ErrorCode = Code;
		return R;
	}
};

/** Delegate type for action handlers */
DECLARE_DELEGATE_RetVal_OneParam(FMonolithActionResult, FMonolithActionHandler, const TSharedPtr<FJsonObject>& /* Params */);

/** Metadata describing a registered action */
struct FMonolithActionInfo
{
	FString Namespace;
	FString Action;
	FString Description;
	FString Category;                     // Optional sub-grouping within a namespace (e.g. "CommonUI" inside "ui"). Empty = uncategorized.
	TSharedPtr<FJsonObject> ParamSchema;  // JSON Schema for parameter validation
};

/**
 * Central registry for all Monolith tool actions.
 * Domain modules register actions here. The HTTP server dispatches through this.
 */
class MONOLITHCORE_API FMonolithToolRegistry
{
public:
	static FMonolithToolRegistry& Get();

	/**
	 * Register an action handler.
	 * @param Namespace   The tool namespace (e.g., "blueprint", "material")
	 * @param Action      The action name (e.g., "list_graphs", "get_node")
	 * @param Description Human-readable description of what this action does
	 * @param Handler     The delegate to execute
	 * @param ParamSchema Optional JSON Schema describing expected parameters
	 */
	void RegisterAction(
		const FString& Namespace,
		const FString& Action,
		const FString& Description,
		const FMonolithActionHandler& Handler,
		const TSharedPtr<FJsonObject>& ParamSchema = nullptr,
		const FString& Category = FString()  // Optional sub-group within namespace — defaults to uncategorized
	);

	/** Unregister all actions in a namespace (called during module shutdown) */
	void UnregisterNamespace(const FString& Namespace);

	/** Execute an action by namespace + action name */
	FMonolithActionResult ExecuteAction(const FString& Namespace, const FString& Action, const TSharedPtr<FJsonObject>& Params);

	/** Get all registered namespaces */
	TArray<FString> GetNamespaces() const;

	/** Get all actions in a namespace */
	TArray<FMonolithActionInfo> GetActions(const FString& Namespace) const;

	/** Get all actions across all namespaces */
	TArray<FMonolithActionInfo> GetAllActions() const;

	/** Check if a specific action exists */
	bool HasAction(const FString& Namespace, const FString& Action) const;

	/** Get total number of registered actions */
	int32 GetActionCount() const;

private:
	FMonolithToolRegistry() = default;

	struct FRegisteredAction
	{
		FMonolithActionInfo Info;
		FMonolithActionHandler Handler;
	};

	/** Map of "namespace.action" → registered action */
	TMap<FString, FRegisteredAction> Actions;

	/** Map of namespace → list of action keys */
	TMap<FString, TArray<FString>> NamespaceActions;

	static FString MakeKey(const FString& Namespace, const FString& Action)
	{
		return Namespace + TEXT(".") + Action;
	}

	mutable FCriticalSection RegistryLock;
};
