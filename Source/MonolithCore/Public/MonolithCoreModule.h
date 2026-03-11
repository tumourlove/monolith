#pragma once

#include "Modules/ModuleManager.h"

#define MONOLITH_VERSION TEXT("0.7.1")

class FMonolithHttpServer;

class MONOLITHCORE_API FMonolithCoreModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static inline FMonolithCoreModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FMonolithCoreModule>("MonolithCore");
	}

	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("MonolithCore");
	}

	/** Get the running HTTP server instance */
	FMonolithHttpServer* GetHttpServer() const { return HttpServer.Get(); }

private:
	TUniquePtr<FMonolithHttpServer> HttpServer;

	void RegisterCoreTools();
};
