#include "Modules/ModuleManager.h"
#include "IMonolithGraphFormatter.h"
#include "MonolithBAFormatterImpl.h"
#include "MonolithSettings.h"

DEFINE_LOG_CATEGORY(LogMonolithBABridge);

class FMonolithBABridgeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
		if (!Settings || !Settings->bEnableBlueprintAssist)
		{
			UE_LOG(LogMonolithBABridge, Log,
				TEXT("MonolithBABridge: Blueprint Assist integration disabled in settings"));
			return;
		}

#if WITH_BLUEPRINT_ASSIST
		Formatter = MakeUnique<FMonolithBAFormatterImpl>();
		IModularFeatures::Get().RegisterModularFeature(
			IMonolithGraphFormatter::GetModularFeatureName(),
			Formatter.Get());
		UE_LOG(LogMonolithBABridge, Log,
			TEXT("MonolithBABridge: Registered BA graph formatter"));
#else
		UE_LOG(LogMonolithBABridge, Log,
			TEXT("MonolithBABridge: Blueprint Assist not found at compile time, bridge inactive"));
#endif
	}

	virtual void ShutdownModule() override
	{
#if WITH_BLUEPRINT_ASSIST
		if (Formatter.IsValid())
		{
			IModularFeatures::Get().UnregisterModularFeature(
				IMonolithGraphFormatter::GetModularFeatureName(),
				Formatter.Get());
			Formatter.Reset();
		}
#endif
	}

private:
#if WITH_BLUEPRINT_ASSIST
	TUniquePtr<FMonolithBAFormatterImpl> Formatter;
#endif
};

IMPLEMENT_MODULE(FMonolithBABridgeModule, MonolithBABridge)
