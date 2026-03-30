#include "MonolithCoreModule.h"
#include "MonolithHttpServer.h"
#include "MonolithSettings.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithCoreTools.h"
#include "Misc/FileHelper.h"
#include "GenericPlatform/GenericPlatformProcess.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FMonolithCoreModule"

void FMonolithCoreModule::StartupModule()
{
	UE_LOG(LogMonolith, Log, TEXT("Monolith %s — Core module initializing"), MONOLITH_VERSION);

	// Register core discovery/status tools
	RegisterCoreTools();

	// Start HTTP server
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	int32 Port = Settings ? Settings->ServerPort : 9316;

	HttpServer = MakeUnique<FMonolithHttpServer>();
	if (HttpServer->Start(Port))
	{
		WriteSentinelFile(Port);
	}
	else
	{
		UE_LOG(LogMonolith, Error, TEXT("Failed to start MCP server on port %d"), Port);
	}
}

void FMonolithCoreModule::ShutdownModule()
{
	RemoveSentinelFile();

	if (HttpServer.IsValid())
	{
		HttpServer->Stop();
		HttpServer.Reset();
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("monolith"));

	UE_LOG(LogMonolith, Log, TEXT("Monolith — Core module shut down"));
}

void FMonolithCoreModule::RegisterCoreTools()
{
	FMonolithCoreTools::RegisterAll();
}

FString FMonolithCoreModule::GetSentinelFilePath() const
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
	if (Plugin.IsValid())
	{
		return Plugin->GetBaseDir() / TEXT("Saved") / TEXT(".monolith_running");
	}
	return FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("Monolith"), TEXT("Saved"), TEXT(".monolith_running"));
}

void FMonolithCoreModule::WriteSentinelFile(int32 Port)
{
	TSharedPtr<FJsonObject> Sentinel = MakeShared<FJsonObject>();
	Sentinel->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
	Sentinel->SetNumberField(TEXT("port"), Port);
	Sentinel->SetStringField(TEXT("version"), MONOLITH_VERSION);
	Sentinel->SetStringField(TEXT("started"), FDateTime::UtcNow().ToIso8601());

	FString Body;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	FJsonSerializer::Serialize(Sentinel.ToSharedRef(), Writer);

	const FString Path = GetSentinelFilePath();
	if (FFileHelper::SaveStringToFile(Body, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogMonolith, Log, TEXT("Sentinel file written: %s"), *Path);
	}
	else
	{
		UE_LOG(LogMonolith, Warning, TEXT("Failed to write sentinel file: %s"), *Path);
	}
}

void FMonolithCoreModule::RemoveSentinelFile()
{
	const FString Path = GetSentinelFilePath();
	if (FPaths::FileExists(Path))
	{
		IFileManager::Get().Delete(*Path);
		UE_LOG(LogMonolith, Log, TEXT("Sentinel file removed: %s"), *Path);
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithCoreModule, MonolithCore)
