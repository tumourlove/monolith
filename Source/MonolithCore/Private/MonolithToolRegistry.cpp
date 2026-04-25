#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"

FMonolithToolRegistry& FMonolithToolRegistry::Get()
{
	static FMonolithToolRegistry Instance;
	return Instance;
}

void FMonolithToolRegistry::RegisterAction(
	const FString& Namespace,
	const FString& Action,
	const FString& Description,
	const FMonolithActionHandler& Handler,
	const TSharedPtr<FJsonObject>& ParamSchema,
	const FString& Category)
{
	FScopeLock Lock(&RegistryLock);

	FString Key = MakeKey(Namespace, Action);

	if (Actions.Contains(Key))
	{
		UE_LOG(LogMonolith, Warning, TEXT("Overwriting existing action: %s"), *Key);
	}

	FRegisteredAction RegAction;
	RegAction.Info.Namespace = Namespace;
	RegAction.Info.Action = Action;
	RegAction.Info.Description = Description;
	RegAction.Info.Category = Category;
	RegAction.Info.ParamSchema = ParamSchema;
	RegAction.Handler = Handler;

	Actions.Add(Key, MoveTemp(RegAction));
	NamespaceActions.FindOrAdd(Namespace).AddUnique(Key);

	UE_LOG(LogMonolith, Verbose, TEXT("Registered action: %s — %s"), *Key, *Description);
}

void FMonolithToolRegistry::UnregisterNamespace(const FString& Namespace)
{
	FScopeLock Lock(&RegistryLock);

	if (TArray<FString>* Keys = NamespaceActions.Find(Namespace))
	{
		for (const FString& Key : *Keys)
		{
			Actions.Remove(Key);
		}
		UE_LOG(LogMonolith, Log, TEXT("Unregistered namespace: %s (%d actions)"), *Namespace, Keys->Num());
		NamespaceActions.Remove(Namespace);
	}
}

FMonolithActionResult FMonolithToolRegistry::ExecuteAction(
	const FString& Namespace,
	const FString& Action,
	const TSharedPtr<FJsonObject>& Params)
{
	FScopeLock Lock(&RegistryLock);

	FString Key = MakeKey(Namespace, Action);
	FRegisteredAction* RegAction = Actions.Find(Key);

	if (!RegAction)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown action: %s.%s"), *Namespace, *Action),
			FMonolithJsonUtils::ErrMethodNotFound
		);
	}

	if (!RegAction->Handler.IsBound())
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Action handler not bound: %s"), *Key),
			FMonolithJsonUtils::ErrInternalError
		);
	}

	// Validate required params from schema before dispatching.
	// Skip asset_path — GetAssetPath() accepts both asset_path and system_path aliases
	// and produces a clear error message itself.
	const FMonolithActionInfo& ActionInfo = RegAction->Info;
	if (ActionInfo.ParamSchema.IsValid() && Params.IsValid())
	{
		TArray<FString> Missing;
		for (const auto& Pair : ActionInfo.ParamSchema->Values)
		{
			if (Pair.Key == TEXT("asset_path")) continue;

			const TSharedPtr<FJsonObject>* ParamDef = nullptr;
			if (Pair.Value->TryGetObject(ParamDef) && ParamDef)
			{
				bool bRequired = false;
				(*ParamDef)->TryGetBoolField(TEXT("required"), bRequired);
				if (bRequired && !Params->HasField(Pair.Key))
				{
					Missing.Add(Pair.Key);
				}
			}
		}
		if (Missing.Num() > 0)
		{
			TArray<FString> Provided;
			for (const auto& P : Params->Values) Provided.Add(P.Key);
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Missing required param(s): [%s]. Provided keys: [%s]"),
					*FString::Join(Missing, TEXT(", ")),
					*FString::Join(Provided, TEXT(", "))));
		}
	}

	// Release lock before executing handler (handlers may take time)
	FMonolithActionHandler HandlerCopy = RegAction->Handler;
	Lock.Unlock();

	return HandlerCopy.Execute(Params.IsValid() ? Params : MakeShared<FJsonObject>());
}

TArray<FString> FMonolithToolRegistry::GetNamespaces() const
{
	FScopeLock Lock(&RegistryLock);
	TArray<FString> Result;
	NamespaceActions.GetKeys(Result);
	return Result;
}

TArray<FMonolithActionInfo> FMonolithToolRegistry::GetActions(const FString& Namespace) const
{
	FScopeLock Lock(&RegistryLock);
	TArray<FMonolithActionInfo> Result;

	if (const TArray<FString>* Keys = NamespaceActions.Find(Namespace))
	{
		for (const FString& Key : *Keys)
		{
			if (const FRegisteredAction* RegAction = Actions.Find(Key))
			{
				Result.Add(RegAction->Info);
			}
		}
	}
	return Result;
}

TArray<FMonolithActionInfo> FMonolithToolRegistry::GetAllActions() const
{
	FScopeLock Lock(&RegistryLock);
	TArray<FMonolithActionInfo> Result;
	for (const auto& Pair : Actions)
	{
		Result.Add(Pair.Value.Info);
	}
	return Result;
}

bool FMonolithToolRegistry::HasAction(const FString& Namespace, const FString& Action) const
{
	FScopeLock Lock(&RegistryLock);
	return Actions.Contains(MakeKey(Namespace, Action));
}

int32 FMonolithToolRegistry::GetActionCount() const
{
	FScopeLock Lock(&RegistryLock);
	return Actions.Num();
}
