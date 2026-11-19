#include "MonolithGameplayTagsInternal.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	/**
	 * Plugin directories whose C++ is tooling rather than project code: Monolith
	 * itself and every plugin that builds on it. Their sources quote tag-shaped
	 * strings as pin names, examples and presets ("Input", "GameplayCue"), and
	 * counting those would keep dead tags alive.
	 */
	TArray<FString> GatherToolingPluginDirs()
	{
		static const FString MonolithPluginName = TEXT("Monolith");

		TArray<FString> Dirs;
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
		{
			bool bIsTooling = Plugin->GetName() == MonolithPluginName;
			for (const FPluginReferenceDescriptor& Dependency : Plugin->GetDescriptor().Plugins)
			{
				bIsTooling |= Dependency.Name == MonolithPluginName;
			}
			if (bIsTooling)
			{
				FString Dir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir()).Replace(TEXT("\\"), TEXT("/"));
				if (!Dir.EndsWith(TEXT("/")))
				{
					Dir += TEXT("/");
				}
				Dirs.Add(MoveTemp(Dir));
			}
		}
		return Dirs;
	}

	bool IsUnderAnyDir(const FString& Path, const TArray<FString>& Dirs)
	{
		for (const FString& Dir : Dirs)
		{
			if (Path.StartsWith(Dir, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool IsTagChar(TCHAR Char)
	{
		return FChar::IsAlnum(Char) || Char == TEXT('_') || Char == TEXT('.');
	}

	/** Project and plugin C++ files on disk, minus tooling plugins, normalised to forward slashes. */
	TArray<FString> GatherProjectSourceFiles(const TArray<FString>& ExcludedDirs)
	{
		TArray<FString> Files;
		const FString ProjectSourceDir = FPaths::ProjectDir() / TEXT("Source");
		const FString PluginsDir = FPaths::ProjectPluginsDir();

		IFileManager& FileManager = IFileManager::Get();
		FileManager.FindFilesRecursive(Files, *ProjectSourceDir, TEXT("*.h"), true, false, /*bClearFileNames=*/true);
		FileManager.FindFilesRecursive(Files, *ProjectSourceDir, TEXT("*.cpp"), true, false, /*bClearFileNames=*/false);
		FileManager.FindFilesRecursive(Files, *PluginsDir, TEXT("*.h"), true, false, /*bClearFileNames=*/false);
		FileManager.FindFilesRecursive(Files, *PluginsDir, TEXT("*.cpp"), true, false, /*bClearFileNames=*/false);

		TArray<FString> Result;
		Result.Reserve(Files.Num());
		for (FString& File : Files)
		{
			FString Normalised = FPaths::ConvertRelativePathToFull(File).Replace(TEXT("\\"), TEXT("/"));
			// Generated and build output is not code anyone wrote.
			if (Normalised.Contains(TEXT("/Intermediate/")) || Normalised.Contains(TEXT("/Binaries/"))
				|| IsUnderAnyDir(Normalised, ExcludedDirs))
			{
				continue;
			}
			Result.Add(MoveTemp(Normalised));
		}
		return Result;
	}

	/** Project, platform and plugin config files on disk, minus tooling plugins, normalised to forward slashes. */
	TArray<FString> GatherProjectConfigFiles(const TArray<FString>& ExcludedDirs)
	{
		TArray<FString> Files;
		IFileManager& FileManager = IFileManager::Get();
		FileManager.FindFilesRecursive(Files, *FPaths::ProjectConfigDir(), TEXT("*.ini"), true, false, /*bClearFileNames=*/true);
		FileManager.FindFilesRecursive(Files, *(FPaths::ProjectDir() / TEXT("Platforms")), TEXT("*.ini"), true, false, /*bClearFileNames=*/false);
		FileManager.FindFilesRecursive(Files, *FPaths::ProjectPluginsDir(), TEXT("*.ini"), true, false, /*bClearFileNames=*/false);

		TArray<FString> Result;
		Result.Reserve(Files.Num());
		for (FString& File : Files)
		{
			FString Normalised = FPaths::ConvertRelativePathToFull(File).Replace(TEXT("\\"), TEXT("/"));
			// Only files the engine reads as config; Saved holds generated copies of them.
			if (!Normalised.Contains(TEXT("/Config/")) || Normalised.Contains(TEXT("/Saved/"))
				|| Normalised.Contains(TEXT("/Intermediate/")) || IsUnderAnyDir(Normalised, ExcludedDirs))
			{
				continue;
			}
			Result.Add(MoveTemp(Normalised));
		}
		return Result;
	}

	/** Run one extractor over every file, mapping each harvested tag name to the first file that has it. */
	void CollectFromFiles(const TArray<FString>& Files, TMap<FString, FString>& OutTagToFile,
		void (*Extract)(const FString&, const FString&, TMap<FString, FString>&))
	{
		FString Text;
		for (const FString& File : Files)
		{
			if (FFileHelper::LoadFileToString(Text, *File))
			{
				Extract(Text, File, OutTagToFile);
			}
		}
	}
}

namespace MonolithGameplayTags
{
	void ExtractTagLiterals(const FString& Text, const FString& File, TMap<FString, FString>& OutLiteralToFile)
	{
		const TCHAR* Cursor = *Text;
		const TCHAR* End = Cursor + Text.Len();

		while (Cursor < End)
		{
			const TCHAR Char = *Cursor;
			const TCHAR Next = (Cursor + 1 < End) ? *(Cursor + 1) : TEXT('\0');

			if (Char == TEXT('/') && Next == TEXT('/'))
			{
				while (Cursor < End && *Cursor != TEXT('\n'))
				{
					++Cursor;
				}
				continue;
			}
			if (Char == TEXT('/') && Next == TEXT('*'))
			{
				Cursor += 2;
				while (Cursor + 1 < End && !(*Cursor == TEXT('*') && *(Cursor + 1) == TEXT('/')))
				{
					++Cursor;
				}
				Cursor = FMath::Min(Cursor + 2, End);
				continue;
			}
			if (Char == TEXT('\'') || Char == TEXT('"'))
			{
				const TCHAR Quote = Char;
				const TCHAR* Start = ++Cursor;
				bool bTagShaped = true;
				while (Cursor < End && *Cursor != Quote && *Cursor != TEXT('\n'))
				{
					if (*Cursor == TEXT('\\') && Cursor + 1 < End)
					{
						bTagShaped = false;
						++Cursor;
					}
					else if (!IsTagChar(*Cursor))
					{
						bTagShaped = false;
					}
					++Cursor;
				}

				const int32 Length = static_cast<int32>(Cursor - Start);
				if (Quote == TEXT('"') && bTagShaped && Length > 0 && Length < 256
					&& *Start != TEXT('.') && *(Cursor - 1) != TEXT('.'))
				{
					OutLiteralToFile.FindOrAdd(FString::ConstructFromPtrSize(Start, Length), File);
				}
				Cursor = FMath::Min(Cursor + 1, End);
				continue;
			}
			++Cursor;
		}
	}

	void ExtractConfigTagUses(const FString& Text, const FString& File, TMap<FString, FString>& OutTagToFile)
	{
		static const FString Key = TEXT("TagName=\"");

		int32 SearchFrom = 0;
		while (true)
		{
			const int32 KeyIndex = Text.Find(Key, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (KeyIndex == INDEX_NONE)
			{
				return;
			}
			SearchFrom = KeyIndex + Key.Len();

			if (KeyIndex > 0 && IsTagChar(Text[KeyIndex - 1]))
			{
				continue;
			}

			int32 End = SearchFrom;
			while (End < Text.Len() && IsTagChar(Text[End]))
			{
				++End;
			}
			if (End < Text.Len() && Text[End] == TEXT('"') && End > SearchFrom)
			{
				OutTagToFile.FindOrAdd(Text.Mid(SearchFrom, End - SearchFrom), File);
			}
		}
	}

	FAssetIdentifier MakeTagIdentifier(FName TagName)
	{
		return FAssetIdentifier(FGameplayTag::StaticStruct(), TagName);
	}

	TArray<FName> GetReferencingPackages(const IAssetRegistry& Registry, FName TagName)
	{
		TArray<FAssetIdentifier> Referencers;
		Registry.GetReferencers(MakeTagIdentifier(TagName), Referencers,
			UE::AssetRegistry::EDependencyCategory::SearchableName);

		TArray<FName> Packages;
		for (const FAssetIdentifier& Referencer : Referencers)
		{
			if (!Referencer.PackageName.IsNone())
			{
				Packages.AddUnique(Referencer.PackageName);
			}
		}
		return Packages;
	}

	bool IsNativeTag(const UGameplayTagsManager& TagManager, FName TagName)
	{
		const TSharedPtr<FGameplayTagNode> Node = TagManager.FindTagNode(TagName);
		if (!Node.IsValid())
		{
			return false;
		}
		for (FName SourceName : Node->GetAllSourceNames())
		{
			const FGameplayTagSource* Source = TagManager.FindTagSource(SourceName);
			if (Source && Source->SourceType == EGameplayTagSourceType::Native)
			{
				return true;
			}
		}
		return false;
	}

	FString GetNormalisedProjectDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()).Replace(TEXT("\\"), TEXT("/"));
	}

	FString ToProjectRelative(const FString& Path)
	{
		const FString ProjectDir = GetNormalisedProjectDir();
		return Path.StartsWith(ProjectDir, ESearchCase::IgnoreCase) ? Path.RightChop(ProjectDir.Len()) : Path;
	}

	const FString* FProjectTagEvidence::FindAny(const FString& TagName) const
	{
		if (const FString* File = CppLiterals.Find(TagName))
		{
			return File;
		}
		return ConfigUses.Find(TagName);
	}

	void CollectProjectTagEvidence(FProjectTagEvidence& Out)
	{
		const TArray<FString> ToolingDirs = GatherToolingPluginDirs();
		const TArray<FString> SourceFiles = GatherProjectSourceFiles(ToolingDirs);
		const TArray<FString> ConfigFiles = GatherProjectConfigFiles(ToolingDirs);
		Out.SourceFileCount = SourceFiles.Num();
		Out.ConfigFileCount = ConfigFiles.Num();

		CollectFromFiles(SourceFiles, Out.CppLiterals, &ExtractTagLiterals);
		CollectFromFiles(ConfigFiles, Out.ConfigUses, &ExtractConfigTagUses);
	}
}
