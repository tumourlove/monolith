#pragma once

#include "Modules/ModuleManager.h"

#define MONOLITH_VERSION TEXT("0.14.5")

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

	/** Console-command target: stop and restart the HTTP server on its configured port. */
	static void RestartHttpServer();

private:
	TUniquePtr<FMonolithHttpServer> HttpServer;

	void RegisterCoreTools();
	void WriteSentinelFile(int32 Port);
	void RemoveSentinelFile();
	FString GetSentinelFilePath() const;

	/** Touch plugin files if Monolith.uplugin shows a future mtime (cross-TZ ZIP extraction artifact). */
	void NormalizeFutureMtimesIfNeeded();
};
