// SPDX-License-Identifier: MIT
// FMonolithBulkFillRegistry implementation. Phase 0 framework primitive.

#include "MonolithBulkFillRegistry.h"
#include "MonolithJsonUtils.h"

FMonolithBulkFillRegistry& FMonolithBulkFillRegistry::Get()
{
	static FMonolithBulkFillRegistry Instance;
	return Instance;
}

void FMonolithBulkFillRegistry::RegisterAdapter(
	const FString& TargetNamespace,
	FBulkFillAdapter BulkFill,
	FDescribeAdapter Describe,
	FListTargetsAdapter ListTargets)
{
	if (TargetNamespace.IsEmpty())
	{
		UE_LOG(LogMonolith, Warning, TEXT("FMonolithBulkFillRegistry::RegisterAdapter called with empty namespace — skipped"));
		return;
	}

	FScopeLock Lock(&AdapterLock);
	FAdapterPair& Pair = Adapters.FindOrAdd(TargetNamespace);
	Pair.BulkFill = MoveTemp(BulkFill);
	Pair.Describe = MoveTemp(Describe);
	Pair.ListTargets = MoveTemp(ListTargets);

	UE_LOG(LogMonolith, Log, TEXT("BulkFillRegistry: adapter registered for namespace '%s'"), *TargetNamespace);
}

void FMonolithBulkFillRegistry::UnregisterAdapter(const FString& TargetNamespace)
{
	FScopeLock Lock(&AdapterLock);
	if (Adapters.Remove(TargetNamespace) > 0)
	{
		UE_LOG(LogMonolith, Log, TEXT("BulkFillRegistry: adapter unregistered for namespace '%s'"), *TargetNamespace);
	}
}

FDryRunReport FMonolithBulkFillRegistry::DispatchBulkFill(const FBulkFillSpec& Spec) const
{
	FScopeLock Lock(&AdapterLock);
	const FAdapterPair* Pair = Adapters.Find(Spec.TargetNamespace);
	if (!Pair || !Pair->BulkFill)
	{
		FDryRunReport Empty;
		FBulkFillFieldWrite W;
		W.Path = TEXT("<dispatch>");
		W.bOk = false;
		W.Reason = FString::Printf(TEXT("no bulk_fill adapter registered for namespace '%s'"), *Spec.TargetNamespace);
		Empty.FieldWrites.Add(W);
		Empty.bWouldApply = false;
		Empty.Errors = 1;
		return Empty;
	}
	return Pair->BulkFill(Spec);
}

FSchemaDescriptor FMonolithBulkFillRegistry::DispatchDescribe(const FString& TargetNamespace, const FString& TargetAssetOrAction) const
{
	FScopeLock Lock(&AdapterLock);
	const FAdapterPair* Pair = Adapters.Find(TargetNamespace);
	if (!Pair || !Pair->Describe)
	{
		FSchemaDescriptor Empty;
		Empty.FieldPath = TEXT("<unregistered>");
		Empty.TypeName = FString::Printf(TEXT("no describe adapter for '%s'"), *TargetNamespace);
		return Empty;
	}
	return Pair->Describe(TargetAssetOrAction);
}

TArray<FString> FMonolithBulkFillRegistry::DispatchListTargets(const FString& TargetNamespace) const
{
	FScopeLock Lock(&AdapterLock);
	const FAdapterPair* Pair = Adapters.Find(TargetNamespace);
	if (!Pair || !Pair->ListTargets)
	{
		return TArray<FString>();
	}
	return Pair->ListTargets();
}

TArray<FString> FMonolithBulkFillRegistry::GetRegisteredNamespaces() const
{
	FScopeLock Lock(&AdapterLock);
	TArray<FString> Keys;
	Adapters.GenerateKeyArray(Keys);
	return Keys;
}

bool FMonolithBulkFillRegistry::HasAdapter(const FString& TargetNamespace) const
{
	FScopeLock Lock(&AdapterLock);
	return Adapters.Contains(TargetNamespace);
}
