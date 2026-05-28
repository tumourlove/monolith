// SPDX-License-Identifier: MIT
// FMonolithBulkFillRegistry — per-namespace adapter table for `bulk_fill` and
// `describe`. Adapters self-register from their module's StartupModule.

#pragma once

#include "CoreMinimal.h"
#include "MonolithBulkFillTypes.h"
#include "MonolithToolRegistry.h"

/**
 * Per-namespace adapter function table for the bulk_fill / describe namespaces.
 * Each module's StartupModule registers its adapter functions here; the central
 * "bulk_fill" + "describe" dispatchers route on the spec's TargetNamespace.
 *
 * Thread model: read on game thread (dispatch path), written on
 * module-startup-thread. FCriticalSection AdapterLock serialises both sides.
 */
class MONOLITHCORE_API FMonolithBulkFillRegistry
{
public:
	static FMonolithBulkFillRegistry& Get();

	/** Function pointer types matching the dispatch shape. */
	using FBulkFillAdapter = TFunction<FDryRunReport(const FBulkFillSpec&)>;
	using FDescribeAdapter = TFunction<FSchemaDescriptor(const FString& /*target_asset_or_action*/)>;

	/**
	 * Per-namespace introspection adapter for `describe.list_targets`.
	 * Returns the set of asset paths or action names the namespace can introspect.
	 * Optional — namespaces without an inventory can leave it unset (the dispatcher
	 * returns an empty array).
	 */
	using FListTargetsAdapter = TFunction<TArray<FString>()>;

	/** Called by per-namespace modules during StartupModule. */
	void RegisterAdapter(
		const FString& TargetNamespace,
		FBulkFillAdapter BulkFill,
		FDescribeAdapter Describe,
		FListTargetsAdapter ListTargets = FListTargetsAdapter());

	void UnregisterAdapter(const FString& TargetNamespace);

	/** Called by the central "bulk_fill" / "describe" action handlers. */
	FDryRunReport DispatchBulkFill(const FBulkFillSpec& Spec) const;
	FSchemaDescriptor DispatchDescribe(const FString& TargetNamespace, const FString& TargetAssetOrAction) const;
	TArray<FString> DispatchListTargets(const FString& TargetNamespace) const;

	/** All currently-registered adapter namespaces. */
	TArray<FString> GetRegisteredNamespaces() const;

	/** True if at least one adapter is registered for the given namespace. */
	bool HasAdapter(const FString& TargetNamespace) const;

private:
	FMonolithBulkFillRegistry() = default;

	struct FAdapterPair
	{
		FBulkFillAdapter BulkFill;
		FDescribeAdapter Describe;
		FListTargetsAdapter ListTargets;
	};

	TMap<FString, FAdapterPair> Adapters;
	mutable FCriticalSection AdapterLock;
};
