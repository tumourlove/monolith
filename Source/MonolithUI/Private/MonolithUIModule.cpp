#include "MonolithUIModule.h"
#include "MonolithUIActions.h"
#include "MonolithUISlotActions.h"
#include "MonolithUITemplateActions.h"
#include "MonolithUIStylingActions.h"
#include "MonolithUIAnimationActions.h"
#include "MonolithUIBindingActions.h"
#include "MonolithUISettingsActions.h"
#include "MonolithUIAccessibilityActions.h"
#include "MonolithSettings.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"

#if WITH_COMMONUI
#include "CommonUI/MonolithCommonUIActions.h"
#endif

#define LOCTEXT_NAMESPACE "MonolithUI"

void FMonolithUIModule::StartupModule()
{
    if (!GetDefault<UMonolithSettings>()->bEnableUI) return;

    FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
    FMonolithUIActions::RegisterActions(Registry);
    FMonolithUISlotActions::RegisterActions(Registry);
    FMonolithUITemplateActions::RegisterActions(Registry);
    FMonolithUIStylingActions::RegisterActions(Registry);
    FMonolithUIAnimationActions::RegisterActions(Registry);
    FMonolithUIBindingActions::RegisterActions(Registry);
    FMonolithUISettingsActions::RegisterActions(Registry);
    FMonolithUIAccessibilityActions::RegisterActions(Registry);

#if WITH_COMMONUI
    FMonolithCommonUIActions::RegisterAll(Registry);
#endif

    // Dynamic action count — reflects base UMG + any conditionally-registered CommonUI actions.
    const int32 UINamespaceActions = Registry.GetActions(TEXT("ui")).Num();
    UE_LOG(LogMonolith, Log, TEXT("Monolith — UI module loaded (%d ui actions)"), UINamespaceActions);
}

void FMonolithUIModule::ShutdownModule()
{
    FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("ui"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithUIModule, MonolithUI)
