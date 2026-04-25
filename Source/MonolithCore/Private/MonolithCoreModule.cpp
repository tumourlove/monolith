#include "MonolithCoreModule.h"
#include "MonolithHttpServer.h"
#include "MonolithSettings.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithCoreTools.h"
#include "Misc/FileHelper.h"
#include "GenericPlatform/GenericPlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/IConsoleManager.h"

#define LOCTEXT_NAMESPACE "FMonolithCoreModule"

static FAutoConsoleCommand GMonolithRestartCmd(
	TEXT("Monolith.Restart"),
	TEXT("Restart the Monolith MCP HTTP server on its configured port."),
	FConsoleCommandDelegate::CreateStatic(&FMonolithCoreModule::RestartHttpServer)
);

void FMonolithCoreModule::StartupModule()
{
	UE_LOG(LogMonolith, Log, TEXT("Monolith %s — Core module initializing"), MONOLITH_VERSION);

	// Self-heal future-dated mtimes from cross-TZ ZIP extraction.
	NormalizeFutureMtimesIfNeeded();

	// Skip MCP server + sentinel in commandlets (cook/compile). The running editor already holds port 9316
	// and a second bind attempt surfaces as UAT ExitCode=1. Commandlets have no MCP consumer anyway.
	if (IsRunningCommandlet())
	{
		UE_LOG(LogMonolith, Log, TEXT("Monolith — commandlet detected, skipping MCP server startup"));
		return;
	}

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

void FMonolithCoreModule::NormalizeFutureMtimesIfNeeded()
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
	if (!Plugin.IsValid())
	{
		return;
	}

	const FString PluginDir = Plugin->GetBaseDir();
	const FString UpluginPath = PluginDir / TEXT("Monolith.uplugin");

	const FDateTime UpluginMtime = IFileManager::Get().GetTimeStamp(*UpluginPath);
	if (UpluginMtime == FDateTime::MinValue())
	{
		return;
	}

	const FDateTime NowUtc = FDateTime::UtcNow();
	if (UpluginMtime <= NowUtc)
	{
		return;
	}

	UE_LOG(LogMonolith, Warning, TEXT("Monolith.uplugin mtime %s is in the future (now %s) — normalizing plugin file timestamps"),
		*UpluginMtime.ToIso8601(), *NowUtc.ToIso8601());

	TArray<FString> AllFiles;
	IFileManager::Get().FindFilesRecursive(AllFiles, *PluginDir, TEXT("*"), true, false);

	int32 Touched = 0;
	int32 Failed = 0;
	for (const FString& File : AllFiles)
	{
		if (IFileManager::Get().SetTimeStamp(*File, NowUtc)) { ++Touched; } else { ++Failed; }
	}

	UE_LOG(LogMonolith, Log, TEXT("Normalized %d file(s), %d failed"), Touched, Failed);
}

void FMonolithCoreModule::RestartHttpServer()
{
	if (!IsAvailable())
	{
		UE_LOG(LogMonolith, Warning, TEXT("Monolith.Restart: MonolithCore module not loaded"));
		return;
	}

	FMonolithCoreModule& Module = Get();
	if (!Module.HttpServer.IsValid())
	{
		UE_LOG(LogMonolith, Warning, TEXT("Monolith.Restart: HTTP server instance missing"));
		return;
	}

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	const int32 Port = Settings ? Settings->ServerPort : 9316;

	UE_LOG(LogMonolith, Log, TEXT("Monolith.Restart: restarting HTTP server on port %d"), Port);
	if (Module.HttpServer->Restart(Port))
	{
		Module.WriteSentinelFile(Port);
		UE_LOG(LogMonolith, Log, TEXT("Monolith.Restart: success"));
	}
	else
	{
		UE_LOG(LogMonolith, Error, TEXT("Monolith.Restart: failed to rebind port %d"), Port);
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithCoreModule, MonolithCore)
