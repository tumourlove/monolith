#include "MonolithAIModule.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithAIBlackboardActions.h"
#include "MonolithAIBehaviorTreeActions.h"
#include "MonolithAIStateTreeActions.h"
#include "MonolithAIEQSActions.h"
#include "MonolithAIControllerActions.h"
#include "MonolithAIPerceptionActions.h"
#include "MonolithAIPerceptionScaffoldActions.h"  // F8: add_perception_to_actor
#include "MonolithAISmartObjectActions.h"
#include "MonolithAINavigationActions.h"
#include "MonolithAIRuntimeActions.h"
#include "MonolithAIScaffoldActions.h"
#include "MonolithAIDiscoveryActions.h"
#include "MonolithAIAdvancedActions.h"
#include "MonolithAIIndexer.h"
#include "MonolithIndexSubsystem.h"
#include "Editor.h"

DEFINE_LOG_CATEGORY(LogMonolithAI);

void FMonolithAIModule::StartupModule()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableAI)
	{
		UE_LOG(LogMonolithAI, Log,
			TEXT("MonolithAI: AI integration disabled in settings"));
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithAIBlackboardActions::RegisterActions(Registry);
	FMonolithAIBehaviorTreeActions::RegisterActions(Registry);
	FMonolithAIStateTreeActions::RegisterActions(Registry);
	FMonolithAIEQSActions::RegisterActions(Registry);
	FMonolithAIControllerActions::RegisterActions(Registry);
	FMonolithAIPerceptionActions::RegisterActions(Registry);
	FMonolithAIPerceptionScaffoldActions::RegisterActions(Registry);  // F8: add_perception_to_actor
	FMonolithAISmartObjectActions::RegisterActions(Registry);
	FMonolithAINavigationActions::RegisterActions(Registry);
	FMonolithAIRuntimeActions::RegisterActions(Registry);
	FMonolithAIScaffoldActions::RegisterActions(Registry);
	FMonolithAIDiscoveryActions::RegisterActions(Registry);
	FMonolithAIAdvancedActions::RegisterActions(Registry);

	// Register the AI deep indexer into MonolithIndex (deferred until editor subsystems are ready)
	if (Settings->bIndexAI)
	{
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddLambda([this]()
		{
			if (GEditor)
			{
				if (UMonolithIndexSubsystem* IndexSS = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>())
				{
					IndexSS->RegisterIndexer(MakeShared<FAIIndexer>());
					UE_LOG(LogMonolithAI, Log, TEXT("MonolithAI: Registered FAIIndexer into MonolithIndex"));
				}
			}
		});
	}

	int32 ActionCount = Registry.GetActions(TEXT("ai")).Num();
	const TCHAR* MassStatus =
#if WITH_MASSENTITY
		TEXT("available");
#else
		TEXT("not installed");
#endif
	UE_LOG(LogMonolithAI, Log, TEXT("MonolithAI: Loaded (%d actions, MassEntity=%s)"), ActionCount, MassStatus);
}

void FMonolithAIModule::ShutdownModule()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("ai"));
}

IMPLEMENT_MODULE(FMonolithAIModule, MonolithAI)
