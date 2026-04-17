#pragma once

#include "Modules/ModuleManager.h"

class FMonolithMetaSoundModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
