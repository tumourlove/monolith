#pragma once

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

/** Log category for the MonolithLevelSequence module (matches MonolithAI / MonolithGAS pattern). */
DECLARE_LOG_CATEGORY_EXTERN(LogMonolithLevelSequence, Log, All);

class FMonolithLevelSequenceModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	FDelegateHandle PostEngineInitHandle;
};
