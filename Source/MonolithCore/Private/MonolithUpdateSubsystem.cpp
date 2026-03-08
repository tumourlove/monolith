#include "MonolithUpdateSubsystem.h"
#include "MonolithCoreModule.h"
#include "MonolithHttpServer.h"
#include "MonolithSettings.h"
#include "MonolithJsonUtils.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Containers/Ticker.h"
#include "Widgets/SWindow.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Text/SRichTextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Styling/AppStyle.h"

#define MONOLITH_GITHUB_API TEXT("https://api.github.com/repos/tumourlove/monolith/releases/latest")

// ─── FMonolithVersionInfo ───────────────────────────────────────────────────

FString FMonolithVersionInfo::GetVersionFilePath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Monolith"), TEXT("version.json"));
}

FMonolithVersionInfo FMonolithVersionInfo::LoadFromDisk()
{
	FMonolithVersionInfo Info;
	Info.Current = MONOLITH_VERSION;

	FString FilePath = GetVersionFilePath();
	FString JsonString;
	if (FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		TSharedPtr<FJsonObject> JsonObj = FMonolithJsonUtils::Parse(JsonString);
		if (JsonObj.IsValid())
		{
			// Current version always comes from compiled MONOLITH_VERSION — never from disk.
			// version.json only stores pending update state.
			JsonObj->TryGetStringField(TEXT("pending"), Info.Pending);
			JsonObj->TryGetBoolField(TEXT("staging"), Info.bStaging);
		}
	}

	return Info;
}

void FMonolithVersionInfo::SaveToDisk() const
{
	TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
	JsonObj->SetStringField(TEXT("current"), Current);
	if (Pending.IsEmpty())
	{
		JsonObj->SetField(TEXT("pending"), MakeShared<FJsonValueNull>());
	}
	else
	{
		JsonObj->SetStringField(TEXT("pending"), Pending);
	}
	JsonObj->SetBoolField(TEXT("staging"), bStaging);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObj.ToSharedRef(), Writer);

	FString FilePath = GetVersionFilePath();
	FString Dir = FPaths::GetPath(FilePath);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*Dir))
	{
		PlatformFile.CreateDirectoryTree(*Dir);
	}

	FFileHelper::SaveStringToFile(OutputString, *FilePath);
}

// ─── UMonolithUpdateSubsystem ───────────────────────────────────────────────

void UMonolithUpdateSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	VersionInfo = FMonolithVersionInfo::LoadFromDisk();

	// Check for staged update from previous session
	if (VersionInfo.bStaging)
	{
		ApplyStagedUpdate();
	}

	// Auto-check for updates if enabled
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && Settings->bAutoUpdateEnabled)
	{
		// Delay the check so the editor finishes loading
		TWeakObjectPtr<UMonolithUpdateSubsystem> WeakThis(this);
		UpdateCheckTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([WeakThis](float)
			{
				if (UMonolithUpdateSubsystem* Self = WeakThis.Get())
				{
					Self->CheckForUpdate();
				}
				return false; // One-shot
			}),
			5.0f
		);
	}
}

void UMonolithUpdateSubsystem::Deinitialize()
{
	if (UpdateCheckTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(UpdateCheckTickerHandle);
		UpdateCheckTickerHandle.Reset();
	}

	// Run the swap logic here — OnPreExit fires too late (after subsystem teardown)
	// or may not fire at all. Deinitialize is the last reliable hook we have.
	OnPreExit();

	if (PreExitHandle.IsValid())
	{
		FCoreDelegates::OnPreExit.Remove(PreExitHandle);
		PreExitHandle.Reset();
	}

	Super::Deinitialize();
}

void UMonolithUpdateSubsystem::CheckForUpdate()
{
	UE_LOG(LogMonolith, Log, TEXT("Checking for Monolith updates..."));

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(MONOLITH_GITHUB_API);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("User-Agent"), TEXT("Monolith-UE-Plugin"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/vnd.github.v3+json"));

	TWeakObjectPtr<UMonolithUpdateSubsystem> WeakSelf(this);
	Request->OnProcessRequestComplete().BindLambda(
		[WeakSelf](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
		{
			UMonolithUpdateSubsystem* Self = WeakSelf.Get();
			if (!Self) return;

			if (!bConnectedSuccessfully || !HttpResponse.IsValid())
			{
				UE_LOG(LogMonolith, Warning, TEXT("Failed to reach GitHub API for update check"));
				return;
			}

			if (HttpResponse->GetResponseCode() != 200)
			{
				UE_LOG(LogMonolith, Warning, TEXT("GitHub API returned %d"), HttpResponse->GetResponseCode());
				return;
			}

			TSharedPtr<FJsonObject> JsonObj = FMonolithJsonUtils::Parse(HttpResponse->GetContentAsString());
			if (!JsonObj.IsValid())
			{
				UE_LOG(LogMonolith, Warning, TEXT("Failed to parse GitHub release JSON"));
				return;
			}

			FString TagName;
			if (!JsonObj->TryGetStringField(TEXT("tag_name"), TagName))
			{
				UE_LOG(LogMonolith, Warning, TEXT("No tag_name in release JSON"));
				return;
			}

			FString RemoteVersion = ParseVersionFromTag(TagName);
			FString CurrentVersion = Self->VersionInfo.Current;

			UE_LOG(LogMonolith, Log, TEXT("Current: %s, Latest: %s"), *CurrentVersion, *RemoteVersion);

			if (CompareVersions(CurrentVersion, RemoteVersion) > 0)
			{
				// Find the zip asset URL
				FString ZipUrl;
				const TArray<TSharedPtr<FJsonValue>>* Assets;
				if (JsonObj->TryGetArrayField(TEXT("assets"), Assets))
				{
					for (const TSharedPtr<FJsonValue>& AssetVal : *Assets)
					{
						const TSharedPtr<FJsonObject>* AssetObj;
						if (AssetVal->TryGetObject(AssetObj))
						{
							FString Name;
							(*AssetObj)->TryGetStringField(TEXT("name"), Name);
							if (Name.EndsWith(TEXT(".zip")))
							{
								(*AssetObj)->TryGetStringField(TEXT("browser_download_url"), ZipUrl);
								break;
							}
						}
					}
				}

				// Fallback to zipball_url if no zip asset
				if (ZipUrl.IsEmpty())
				{
					JsonObj->TryGetStringField(TEXT("zipball_url"), ZipUrl);
				}

				if (!ZipUrl.IsEmpty())
				{
					FString ReleaseNotes;
					JsonObj->TryGetStringField(TEXT("body"), ReleaseNotes);
					Self->ShowUpdateNotification(RemoteVersion, ZipUrl, ReleaseNotes);
				}
				else
				{
					UE_LOG(LogMonolith, Warning, TEXT("New version %s available but no download URL found"), *RemoteVersion);
				}
			}
			else
			{
				UE_LOG(LogMonolith, Log, TEXT("Monolith is up to date"));
			}
		}
	);

	Request->ProcessRequest();
}

FString UMonolithUpdateSubsystem::ParseVersionFromTag(const FString& Tag)
{
	FString Version = Tag;
	Version.TrimStartAndEndInline();
	if (Version.StartsWith(TEXT("v")) || Version.StartsWith(TEXT("V")))
	{
		Version.RightChopInline(1);
	}
	return Version;
}

int32 UMonolithUpdateSubsystem::CompareVersions(const FString& Current, const FString& Remote)
{
	auto ParseParts = [](const FString& Ver, int32& Major, int32& Minor, int32& Patch)
	{
		TArray<FString> Parts;
		Ver.ParseIntoArray(Parts, TEXT("."));
		Major = Parts.IsValidIndex(0) ? FCString::Atoi(*Parts[0]) : 0;
		Minor = Parts.IsValidIndex(1) ? FCString::Atoi(*Parts[1]) : 0;
		Patch = Parts.IsValidIndex(2) ? FCString::Atoi(*Parts[2]) : 0;
	};

	int32 CMajor, CMinor, CPatch;
	int32 RMajor, RMinor, RPatch;
	ParseParts(Current, CMajor, CMinor, CPatch);
	ParseParts(Remote, RMajor, RMinor, RPatch);

	if (RMajor != CMajor) return RMajor - CMajor;
	if (RMinor != CMinor) return RMinor - CMinor;
	return RPatch - CPatch;
}

void UMonolithUpdateSubsystem::ShowUpdateNotification(const FString& NewVersion, const FString& ZipUrl, const FString& ReleaseNotes)
{
	// Log release notes to Output Log
	UE_LOG(LogMonolith, Log, TEXT("===== Monolith %s Release Notes ====="), *NewVersion);
	if (!ReleaseNotes.IsEmpty())
	{
		TArray<FString> Lines;
		ReleaseNotes.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			UE_LOG(LogMonolith, Log, TEXT("  %s"), *Line);
		}
	}
	UE_LOG(LogMonolith, Log, TEXT("====================================="));

	// Show the full dialog window
	ShowUpdateDialog(NewVersion, ZipUrl, ReleaseNotes);
}

void UMonolithUpdateSubsystem::ShowUpdateDialog(const FString& NewVersion, const FString& ZipUrl, const FString& ReleaseNotes)
{
	// Don't open duplicate windows
	if (UpdateDialogWindow.IsValid())
	{
		UpdateDialogWindow.Pin()->BringToFront();
		return;
	}

	// --- Convert markdown-ish release notes to cleaner display text ---
	FString DisplayNotes = ReleaseNotes;
	if (DisplayNotes.IsEmpty())
	{
		DisplayNotes = TEXT("No release notes provided.");
	}

	// Captures for lambdas
	FString CapturedUrl = ZipUrl;
	FString CapturedVersion = NewVersion;
	TWeakObjectPtr<UMonolithUpdateSubsystem> WeakSelf(this);

	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::Format(NSLOCTEXT("Monolith", "UpdateDialogTitle", "Monolith {0} Available"), FText::FromString(NewVersion)))
		.SizingRule(ESizingRule::UserSized)
		.ClientSize(FVector2D(600, 500))
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	TWeakPtr<SWindow> WeakWindow = Window;

	Window->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(16.0f)
		[
			SNew(SVerticalBox)

			// --- Header ---
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 8)
			[
				SNew(STextBlock)
				.Text(FText::Format(
					NSLOCTEXT("Monolith", "UpdateHeader", "Monolith {0} is available (you have {1})"),
					FText::FromString(NewVersion),
					FText::FromString(VersionInfo.Current)))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
			]

			// --- Separator ---
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 8)
			[
				SNew(SSeparator)
			]

			// --- Release Notes Label ---
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(NSLOCTEXT("Monolith", "ReleaseNotesLabel", "Release Notes:"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]

			// --- Scrollable Release Notes ---
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(0, 0, 0, 12)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
				.Padding(8.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SNew(STextBlock)
						.Text(FText::FromString(DisplayNotes))
						.AutoWrapText(true)
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
					]
				]
			]

			// --- Buttons ---
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 8, 0)
				[
					SNew(SButton)
					.Text(NSLOCTEXT("Monolith", "RemindLater", "Remind Me Later"))
					.OnClicked_Lambda([WeakWindow]() -> FReply
					{
						if (TSharedPtr<SWindow> Win = WeakWindow.Pin())
						{
							Win->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(NSLOCTEXT("Monolith", "InstallUpdate", "Install Update"))
					.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("PrimaryButton"))
					.OnClicked_Lambda([WeakSelf, CapturedUrl, CapturedVersion, WeakWindow]() -> FReply
					{
						if (UMonolithUpdateSubsystem* Self = WeakSelf.Get())
						{
							Self->DownloadUpdate(CapturedUrl, CapturedVersion);
						}
						if (TSharedPtr<SWindow> Win = WeakWindow.Pin())
						{
							Win->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]
			]
		]
	);

	UpdateDialogWindow = Window;
	FSlateApplication::Get().AddWindow(Window);

	UE_LOG(LogMonolith, Log, TEXT("Update dialog shown for version %s"), *NewVersion);
}

void UMonolithUpdateSubsystem::DownloadUpdate(const FString& ZipUrl, const FString& Version)
{
	if (bUpdateInProgress)
	{
		UE_LOG(LogMonolith, Warning, TEXT("Update already in progress"));
		return;
	}

	bUpdateInProgress = true;
	UE_LOG(LogMonolith, Log, TEXT("Downloading Monolith %s from %s"), *Version, *ZipUrl);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(ZipUrl);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("User-Agent"), TEXT("Monolith-UE-Plugin"));

	FString CapturedVersion = Version;
	TWeakObjectPtr<UMonolithUpdateSubsystem> WeakSelf(this);
	Request->OnProcessRequestComplete().BindLambda(
		[WeakSelf, CapturedVersion](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
		{
			UMonolithUpdateSubsystem* Self = WeakSelf.Get();
			if (!Self) return;

			if (!bConnectedSuccessfully || !HttpResponse.IsValid() || HttpResponse->GetResponseCode() != 200)
			{
				UE_LOG(LogMonolith, Error, TEXT("Failed to download update (HTTP %d)"),
					HttpResponse.IsValid() ? HttpResponse->GetResponseCode() : 0);
				Self->bUpdateInProgress = false;
				return;
			}

			Self->OnDownloadComplete(CapturedVersion, true, HttpResponse->GetContent());
		}
	);

	Request->ProcessRequest();
}

void UMonolithUpdateSubsystem::OnDownloadComplete(const FString& Version, bool bSuccess, const TArray<uint8>& Data)
{
	if (!bSuccess || Data.Num() == 0)
	{
		UE_LOG(LogMonolith, Error, TEXT("Update download failed or empty"));
		bUpdateInProgress = false;
		return;
	}

	// Save zip to temp
	FString TempZipPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Monolith"), TEXT("update.zip"));
	FString TempDir = FPaths::GetPath(TempZipPath);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*TempDir))
	{
		PlatformFile.CreateDirectoryTree(*TempDir);
	}

	if (!FFileHelper::SaveArrayToFile(Data, *TempZipPath))
	{
		UE_LOG(LogMonolith, Error, TEXT("Failed to save update zip to %s"), *TempZipPath);
		bUpdateInProgress = false;
		return;
	}

	UE_LOG(LogMonolith, Log, TEXT("Update zip saved (%d bytes), extracting to staging..."), Data.Num());

	// Extract to staging directory
	FString StagingDir = GetStagingPath();
	if (PlatformFile.DirectoryExists(*StagingDir))
	{
		PlatformFile.DeleteDirectoryRecursively(*StagingDir);
	}

	if (!ExtractZipToDirectory(TempZipPath, StagingDir))
	{
		UE_LOG(LogMonolith, Error, TEXT("Failed to extract update zip"));
		bUpdateInProgress = false;
		return;
	}

	// Clean up temp zip
	PlatformFile.DeleteFile(*TempZipPath);

	// Update version.json
	VersionInfo.Pending = Version;
	VersionInfo.bStaging = true;
	VersionInfo.SaveToDisk();

	bUpdateInProgress = false;

	UE_LOG(LogMonolith, Log, TEXT("Monolith %s staged for hot-swap on editor exit."), *Version);

	// Register the pre-exit swap so the update applies when the editor closes
	PendingStagingDir = StagingDir;
	RegisterPreExitSwap();

	// Notify user
	FNotificationInfo Info(FText::Format(
		NSLOCTEXT("Monolith", "UpdateStaged", "Monolith {0} will be installed when you close the editor."),
		FText::FromString(Version)
	));
	Info.ExpireDuration = 10.0f;
	Info.bUseThrobber = false;
	FSlateNotificationManager::Get().AddNotification(Info);
}

bool UMonolithUpdateSubsystem::ExtractZipToDirectory(const FString& ZipPath, const FString& DestDir)
{
	FString ConvertedZip = FPaths::ConvertRelativePathToFull(ZipPath);
	FString ConvertedDest = FPaths::ConvertRelativePathToFull(DestDir);

	int32 ReturnCode = -1;

#if PLATFORM_WINDOWS
	ConvertedZip.ReplaceInline(TEXT("/"), TEXT("\\"));
	ConvertedDest.ReplaceInline(TEXT("/"), TEXT("\\"));

	FString Args = FString::Printf(
		TEXT("-NoProfile -NonInteractive -Command \"Expand-Archive -Path '%s' -DestinationPath '%s' -Force\""),
		*ConvertedZip, *ConvertedDest
	);

	FPlatformProcess::ExecProcess(
		TEXT("powershell.exe"),
		*Args,
		&ReturnCode,
		nullptr,
		nullptr
	);

	if (ReturnCode != 0)
	{
		UE_LOG(LogMonolith, Error, TEXT("PowerShell Expand-Archive failed (exit code %d)"), ReturnCode);
		return false;
	}
#elif PLATFORM_MAC || PLATFORM_LINUX
	// Ensure destination directory exists
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*ConvertedDest))
	{
		PlatformFile.CreateDirectoryTree(*ConvertedDest);
	}

	FString Args = FString::Printf(
		TEXT("-xzf \"%s\" -C \"%s\""),
		*ConvertedZip, *ConvertedDest
	);

	FPlatformProcess::ExecProcess(
		TEXT("/usr/bin/tar"),
		*Args,
		&ReturnCode,
		nullptr,
		nullptr
	);

	if (ReturnCode != 0)
	{
		// Fallback: try unzip for plain .zip files (tar may fail on non-gzipped zips)
		FString UnzipArgs = FString::Printf(
			TEXT("-o \"%s\" -d \"%s\""),
			*ConvertedZip, *ConvertedDest
		);

		FPlatformProcess::ExecProcess(
			TEXT("/usr/bin/unzip"),
			*UnzipArgs,
			&ReturnCode,
			nullptr,
			nullptr
		);

		if (ReturnCode != 0)
		{
			UE_LOG(LogMonolith, Error, TEXT("Failed to extract update archive (tar and unzip both failed, exit code %d)"), ReturnCode);
			return false;
		}
	}
#else
	UE_LOG(LogMonolith, Error, TEXT("Update extraction not supported on this platform"));
	return false;
#endif

	return true;
}

void UMonolithUpdateSubsystem::ApplyStagedUpdate()
{
	FString StagingDir = GetStagingPath();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.DirectoryExists(*StagingDir))
	{
		UE_LOG(LogMonolith, Warning, TEXT("Staging directory not found, clearing staging flag"));
		VersionInfo.bStaging = false;
		VersionInfo.Pending.Empty();
		VersionInfo.SaveToDisk();
		return;
	}

	// The staging dir may contain a nested folder from the zip.
	// Find the actual content root (look for Monolith.uplugin).
	FString ContentRoot = StagingDir;
	TArray<FString> FoundFiles;
	PlatformFile.FindFilesRecursively(FoundFiles, *StagingDir, TEXT(".uplugin"));
	if (FoundFiles.Num() > 0)
	{
		ContentRoot = FPaths::GetPath(FoundFiles[0]);
	}

	PendingStagingDir = ContentRoot;
	RegisterPreExitSwap();

	UE_LOG(LogMonolith, Log, TEXT("Monolith %s will be installed when you close the editor."), *VersionInfo.Pending);

	FNotificationInfo Info(FText::Format(
		NSLOCTEXT("Monolith", "UpdatePending", "Monolith {0} will be installed when you close the editor."),
		FText::FromString(VersionInfo.Pending)
	));
	Info.ExpireDuration = 10.0f;
	Info.bUseThrobber = false;
	FSlateNotificationManager::Get().AddNotification(Info);
}

void UMonolithUpdateSubsystem::RegisterPreExitSwap()
{
	if (PreExitHandle.IsValid())
	{
		return; // Already registered
	}

	PreExitHandle = FCoreDelegates::OnPreExit.AddUObject(this, &UMonolithUpdateSubsystem::OnPreExit);
	UE_LOG(LogMonolith, Log, TEXT("Pre-exit swap registered — update will apply on editor close"));
}

void UMonolithUpdateSubsystem::OnPreExit()
{
	if (PendingStagingDir.IsEmpty())
	{
		return;
	}

	FString PluginDir = FPaths::ConvertRelativePathToFull(GetPluginPath());
	FString StagingDir = FPaths::ConvertRelativePathToFull(PendingStagingDir);

	if (!WriteSwapScript(StagingDir, PluginDir))
	{
		UE_LOG(LogMonolith, Error, TEXT("Failed to write swap script — update will not be applied"));
		return;
	}

	// Clear staging state — the new compiled MONOLITH_VERSION will be the current version after swap
	VersionInfo.Pending.Empty();
	VersionInfo.bStaging = false;
	VersionInfo.SaveToDisk();

	// Launch the swap script detached so it survives editor shutdown
	FString ScriptPath;
	FString ScriptDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Monolith"));
#if PLATFORM_WINDOWS
	ScriptPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ScriptDir, TEXT("monolith_swap.bat")));
	ScriptPath.ReplaceInline(TEXT("/"), TEXT("\\"));

	// Write a tiny launcher script that opens the real script in a new, independent window.
	// This ensures the terminal survives editor shutdown.
	FString LauncherPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ScriptDir, TEXT("monolith_launch.bat")));
	LauncherPath.ReplaceInline(TEXT("/"), TEXT("\\"));
	FString LauncherContent = FString::Printf(
		TEXT("@echo off\r\nstart \"Monolith Updater\" cmd /c \"%s\"\r\n"),
		*ScriptPath
	);
	FFileHelper::SaveStringToFile(LauncherContent, *LauncherPath);

	FString Args = FString::Printf(TEXT("/c \"%s\""), *LauncherPath);
	uint32 ProcessId = 0;
	FProcHandle Proc = FPlatformProcess::CreateProc(
		TEXT("cmd.exe"), *Args,
		true,  // bLaunchDetached
		true,  // bLaunchHidden — launcher is hidden, it spawns the visible window
		false, // bLaunchReallyHidden
		&ProcessId,
		0,     // PriorityModifier
		nullptr, nullptr
	);
#elif PLATFORM_MAC || PLATFORM_LINUX
	ScriptPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ScriptDir, TEXT("monolith_swap.sh")));

	FString Args = FString::Printf(TEXT("\"%s\""), *ScriptPath);
	uint32 ProcessId = 0;
	FProcHandle Proc = FPlatformProcess::CreateProc(
		TEXT("/bin/bash"), *Args,
		true,  // bLaunchDetached
		false, // bLaunchHidden
		false, // bLaunchReallyHidden
		&ProcessId,
		0,     // PriorityModifier
		nullptr, nullptr
	);
#endif

	if (Proc.IsValid())
	{
		UE_LOG(LogMonolith, Log, TEXT("Monolith update will be applied after editor closes (PID %u)."), ProcessId);
		FPlatformProcess::CloseProc(Proc);
	}
	else
	{
		UE_LOG(LogMonolith, Error, TEXT("Failed to launch swap script at %s"), *ScriptPath);
	}
}

bool UMonolithUpdateSubsystem::WriteSwapScript(const FString& StagingDir, const FString& PluginDir)
{
	FString ScriptDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Monolith"));
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*ScriptDir))
	{
		PlatformFile.CreateDirectoryTree(*ScriptDir);
	}

	FString ScriptContent;
	FString ScriptPath;

#if PLATFORM_WINDOWS
	FString WinPluginDir = PluginDir;
	FString WinStagingDir = StagingDir;
	FString WinBackupDir = PluginDir + TEXT("_Backup");
	WinPluginDir.ReplaceInline(TEXT("/"), TEXT("\\"));
	WinStagingDir.ReplaceInline(TEXT("/"), TEXT("\\"));
	WinBackupDir.ReplaceInline(TEXT("/"), TEXT("\\"));

	// Get just the parent dir and folder name for the ren command
	FString PluginParent = FPaths::GetPath(WinPluginDir);
	FString PluginFolderName = FPaths::GetCleanFilename(WinPluginDir);
	FString BackupFolderName = PluginFolderName + TEXT("_Backup");

	ScriptContent = FString::Printf(
		TEXT("@echo off\r\n")
		TEXT("title Monolith Updater\r\n")
		TEXT("echo.\r\n")
		TEXT("echo  ========================================\r\n")
		TEXT("echo   Monolith Update Ready\r\n")
		TEXT("echo  ========================================\r\n")
		TEXT("echo.\r\n")
		TEXT("echo  Waiting for Unreal Editor to close...\r\n")
		TEXT("echo.\r\n")
		TEXT("for /L %%%%i in (10,-1,1) do (\r\n")
		TEXT("    echo  %%%%i seconds remaining...\r\n")
		TEXT("    timeout /t 1 /nobreak > nul\r\n")
		TEXT(")\r\n")
		TEXT("echo.\r\n")
		TEXT("set /p CONFIRM=\"  Install update? (Y/N): \"\r\n")
		TEXT("if /i not \"%%CONFIRM%%\"==\"Y\" (\r\n")
		TEXT("    echo.\r\n")
		TEXT("    echo  Update cancelled.\r\n")
		TEXT("    timeout /t 3 > nul\r\n")
		TEXT("    exit /b 0\r\n")
		TEXT(")\r\n")
		TEXT("echo.\r\n")
		TEXT("echo  Backing up current installation...\r\n")
		TEXT("if exist \"%s\" rmdir /s /q \"%s\"\r\n")
		TEXT("cd /d \"%s\"\r\n")
		TEXT("ren \"%s\" \"%s\"\r\n")
		TEXT("if errorlevel 1 (\r\n")
		TEXT("    echo  Editor still running, waiting longer...\r\n")
		TEXT("    timeout /t 10 /nobreak > nul\r\n")
		TEXT("    ren \"%s\" \"%s\"\r\n")
		TEXT(")\r\n")
		TEXT("if errorlevel 1 (\r\n")
		TEXT("    echo.\r\n")
		TEXT("    echo  ERROR: Could not rename plugin folder.\r\n")
		TEXT("    echo  Make sure the Unreal Editor is fully closed.\r\n")
		TEXT("    pause\r\n")
		TEXT("    exit /b 1\r\n")
		TEXT(")\r\n")
		TEXT("echo  Installing new version...\r\n")
		TEXT("xcopy /s /e /i /q \"%s\\*\" \"%s\\\"\r\n")
		TEXT("if errorlevel 1 (\r\n")
		TEXT("    echo  ERROR: Copy failed, restoring backup...\r\n")
		TEXT("    ren \"%s\" \"%s\"\r\n")
		TEXT("    pause\r\n")
		TEXT("    exit /b 1\r\n")
		TEXT(")\r\n")
		TEXT("rem Preserve .git if it exists (developer workflow)\r\n")
		TEXT("if exist \"%s\\.git\" (\r\n")
		TEXT("    echo  Preserving git repository...\r\n")
		TEXT("    xcopy /s /e /i /q /h \"%s\\.git\" \"%s\\.git\\\"\r\n")
		TEXT(")\r\n")
		TEXT("if exist \"%s\\.gitignore\" copy /y \"%s\\.gitignore\" \"%s\\.gitignore\" > nul\r\n")
		TEXT("if exist \"%s\\.github\" xcopy /s /e /i /q /h \"%s\\.github\" \"%s\\.github\\\"\r\n")
		TEXT("echo  Cleaning up...\r\n")
		TEXT("rmdir /s /q \"%s\"\r\n")
		TEXT("rmdir /s /q \"%s\"\r\n")
		TEXT("echo.\r\n")
		TEXT("echo  ========================================\r\n")
		TEXT("echo   Monolith updated successfully!\r\n")
		TEXT("echo   You can now relaunch the editor.\r\n")
		TEXT("echo  ========================================\r\n")
		TEXT("echo.\r\n")
		TEXT("timeout /t 5 > nul\r\n"),
		*WinBackupDir, *WinBackupDir,
		*PluginParent,
		*PluginFolderName, *BackupFolderName,
		*PluginFolderName, *BackupFolderName,
		*WinStagingDir, *WinPluginDir,
		*BackupFolderName, *PluginFolderName,
		// Preserve .git from backup
		*WinBackupDir, *WinBackupDir, *WinPluginDir,
		*WinBackupDir, *WinBackupDir, *WinPluginDir,
		*WinBackupDir, *WinBackupDir, *WinPluginDir,
		*WinBackupDir,
		*WinStagingDir
	);

	ScriptPath = FPaths::Combine(ScriptDir, TEXT("monolith_swap.bat"));
#elif PLATFORM_MAC || PLATFORM_LINUX
	FString BackupDir = PluginDir + TEXT("_Backup");

	ScriptContent = FString::Printf(
		TEXT("#!/bin/bash\n")
		TEXT("sleep 3\n")
		TEXT("rm -rf \"%s\"\n")
		TEXT("mv \"%s\" \"%s\" || { sleep 5; mv \"%s\" \"%s\"; }\n")
		TEXT("cp -r \"%s/.\" \"%s/\"\n")
		TEXT("if [ $? -ne 0 ]; then mv \"%s\" \"%s\"; echo 'FAILED'; exit 1; fi\n")
		TEXT("# Preserve .git if it exists (developer workflow)\n")
		TEXT("[ -d \"%s/.git\" ] && cp -r \"%s/.git\" \"%s/.git\"\n")
		TEXT("[ -f \"%s/.gitignore\" ] && cp \"%s/.gitignore\" \"%s/.gitignore\"\n")
		TEXT("[ -d \"%s/.github\" ] && cp -r \"%s/.github\" \"%s/.github\"\n")
		TEXT("rm -rf \"%s\" \"%s\"\n")
		TEXT("echo 'Monolith updated successfully.'\n"),
		*BackupDir,
		*PluginDir, *BackupDir, *PluginDir, *BackupDir,
		*StagingDir, *PluginDir,
		*BackupDir, *PluginDir,
		// Preserve .git from backup
		*BackupDir, *BackupDir, *PluginDir,
		*BackupDir, *BackupDir, *PluginDir,
		*BackupDir, *BackupDir, *PluginDir,
		*BackupDir, *StagingDir
	);

	ScriptPath = FPaths::Combine(ScriptDir, TEXT("monolith_swap.sh"));
#else
	UE_LOG(LogMonolith, Error, TEXT("Swap script generation not supported on this platform"));
	return false;
#endif

	if (!FFileHelper::SaveStringToFile(ScriptContent, *ScriptPath))
	{
		UE_LOG(LogMonolith, Error, TEXT("Failed to write swap script to %s"), *ScriptPath);
		return false;
	}

#if PLATFORM_MAC || PLATFORM_LINUX
	// Make the script executable
	FPlatformProcess::ExecProcess(TEXT("/bin/chmod"), *FString::Printf(TEXT("+x \"%s\""), *ScriptPath), nullptr, nullptr, nullptr);
#endif

	UE_LOG(LogMonolith, Log, TEXT("Swap script written to %s"), *ScriptPath);
	return true;
}

FString UMonolithUpdateSubsystem::GetStagingPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Monolith"), TEXT("Staging"));
}

FString UMonolithUpdateSubsystem::GetPluginPath()
{
	return FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("Monolith"));
}
