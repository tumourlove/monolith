#include "MonolithConversationModule.h"

#include "MonolithToolRegistry.h"

#if WITH_COMMONCONVERSATION
#include "MonolithConversationActions.h"
#endif

DEFINE_LOG_CATEGORY(LogMonolithConversation);

void FMonolithConversationModule::StartupModule()
{
#if WITH_COMMONCONVERSATION
	FMonolithConversationActions::RegisterActions(FMonolithToolRegistry::Get());
	UE_LOG(LogMonolithConversation, Log, TEXT("MonolithConversation: Loaded (%d actions)"),
		FMonolithToolRegistry::Get().GetActions(TEXT("conversation")).Num());
#else
	UE_LOG(LogMonolithConversation, Log,
		TEXT("MonolithConversation: CommonConversation plugin not found at compile time, namespace inactive"));
#endif
}

void FMonolithConversationModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("conversation"));
}

IMPLEMENT_MODULE(FMonolithConversationModule, MonolithConversation)
