#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetIdentifier.h"

class IAssetRegistry;
class UGameplayTagsManager;

/** Helpers shared by the read and write halves of the `gameplay_tags` namespace. */
namespace MonolithGameplayTags
{
	/**
	 * The identifier shape the engine writes for a tag reference: the FGameplayTag
	 * script struct as the referenced object, the tag name as the value. Mirrors
	 * GameplayTagsEditorModule, which resolves tag referencers the same way.
	 */
	FAssetIdentifier MakeTagIdentifier(FName TagName);

	/** Packages that reference exactly this tag, from the Asset Registry SearchableName graph. */
	TArray<FName> GetReferencingPackages(const IAssetRegistry& Registry, FName TagName);

	/** True when C++ declares the tag: an ini edit cannot remove it. */
	bool IsNativeTag(const UGameplayTagsManager& TagManager, FName TagName);

	/** Project directory as an absolute path with forward slashes, the form source paths are compared in. */
	FString GetNormalisedProjectDir();

	/** A path relative to the project directory when it sits under it, unchanged otherwise. */
	FString ToProjectRelative(const FString& Path);

	/**
	 * Where the project's own text names tags: string literals in its C++, and
	 * (TagName="A.B") uses in its config. Monolith and the plugins built on it are
	 * left out. Keys are tag names (compared without case, as FString keys are),
	 * values the first file that holds them.
	 */
	struct FProjectTagEvidence
	{
		TMap<FString, FString> CppLiterals;
		TMap<FString, FString> ConfigUses;
		int32 SourceFileCount = 0;
		int32 ConfigFileCount = 0;

		/** The file that names the tag, C++ first; null when nothing does. */
		const FString* FindAny(const FString& TagName) const;
	};

	/** Read the project's C++ and config from disk and collect FProjectTagEvidence. */
	void CollectProjectTagEvidence(FProjectTagEvidence& Out);

	/**
	 * Harvest the tag-shaped string literals of a C++ file: the contents of every
	 * "..." made only of identifier characters and dots. A tag reaches C++ as a
	 * literal - UE_DEFINE_GAMEPLAY_TAG, RequestGameplayTag(TEXT("A.B")) - so a bare
	 * word such as an Input variable, or a tag name inside a comment, is no
	 * evidence and is skipped. Comments and character literals are stepped over
	 * so their quotes cannot open a phantom string.
	 */
	void ExtractTagLiterals(const FString& Text, const FString& File, TMap<FString, FString>& OutLiteralToFile);

	/**
	 * Harvest the tag uses of a config file. An FGameplayTag set from Project
	 * Settings is written as its export text, (TagName="A.B"), inside whatever
	 * setting holds it - platform traits, surface-type maps, any developer
	 * setting. Declarations use a different key (Tag="A.B" in a GameplayTagList)
	 * and redirects use OldTagName/NewTagName, so requiring TagName= to start on a
	 * non-identifier character keeps both out.
	 */
	void ExtractConfigTagUses(const FString& Text, const FString& File, TMap<FString, FString>& OutTagToFile);
}
