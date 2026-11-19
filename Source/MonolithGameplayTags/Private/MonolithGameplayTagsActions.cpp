#include "MonolithGameplayTagsActions.h"
#include "MonolithGameplayTagsModule.h"
#include "MonolithGameplayTagsInternal.h"
#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetIdentifier.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsSettings.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

using namespace MonolithGameplayTags;

namespace
{
	const TCHAR* const TagsNamespace = TEXT("gameplay_tags");

	constexpr int32 DefaultReferencerLimit = 200;
	constexpr int32 MaxReferencerLimit = 5000;
	constexpr int32 DefaultListLimit = 500;
	constexpr int32 MaxListLimit = 5000;

	/** True when a dependency edge points at a gameplay tag rather than at some other searchable name. */
	bool IsGameplayTagIdentifier(const FAssetIdentifier& Identifier)
	{
		const UScriptStruct* TagStruct = FGameplayTag::StaticStruct();
		return !Identifier.ValueName.IsNone()
			&& Identifier.ObjectName == TagStruct->GetFName()
			&& Identifier.PackageName == TagStruct->GetOutermost()->GetFName();
	}

	/** Read an optional integer parameter, clamped into range. */
	int32 ReadLimit(const TSharedPtr<FJsonObject>& Params, int32 Default, int32 Max)
	{
		int32 Requested = 0;
		if (Params.IsValid() && Params->TryGetNumberField(TEXT("limit"), Requested))
		{
			return FMath::Clamp(Requested, 1, Max);
		}
		return Default;
	}

	/** Read and normalise an optional tag-prefix filter. */
	FString ReadPrefix(const TSharedPtr<FJsonObject>& Params)
	{
		FString Prefix;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("prefix"), Prefix);
			Prefix.TrimStartAndEndInline();
		}
		return Prefix;
	}

	/** A tag matches an empty prefix, its own name, or a name it sits under. */
	bool MatchesPrefix(const FString& TagString, const FString& Prefix)
	{
		if (Prefix.IsEmpty())
		{
			return true;
		}
		return TagString == Prefix || TagString.StartsWith(Prefix + TEXT("."), ESearchCase::IgnoreCase);
	}

	/** The tag itself plus every tag below it in the hierarchy. */
	TArray<FName> CollectTagSubtree(const UGameplayTagsManager& TagManager, const FGameplayTag& Tag, FName SelfName)
	{
		TArray<FName> Result;
		Result.Add(SelfName);

		if (Tag.IsValid())
		{
			const FGameplayTagContainer Children = TagManager.RequestGameplayTagChildren(Tag);
			for (const FGameplayTag& Child : Children)
			{
				Result.AddUnique(Child.GetTagName());
			}
		}
		return Result;
	}

	/**
	 * Packages referencing any of the supplied tags, each package listed once with
	 * the tag that pulled it in. Under a subtree query one package often references
	 * several tags of that subtree, and listing it per tag would misreport the count.
	 */
	void CollectReferencerPackages(const IAssetRegistry& Registry, const TArray<FName>& TagNames,
		TMap<FName, FName>& OutPackageToTag, TArray<FName>& OutOrderedPackages)
	{
		for (FName TagName : TagNames)
		{
			TArray<FAssetIdentifier> Referencers;
			Registry.GetReferencers(MakeTagIdentifier(TagName), Referencers,
				UE::AssetRegistry::EDependencyCategory::SearchableName);

			for (const FAssetIdentifier& Referencer : Referencers)
			{
				if (Referencer.PackageName.IsNone() || OutPackageToTag.Contains(Referencer.PackageName))
				{
					continue;
				}
				OutPackageToTag.Add(Referencer.PackageName, TagName);
				OutOrderedPackages.Add(Referencer.PackageName);
			}
		}

		OutOrderedPackages.Sort([](FName A, FName B) { return A.Compare(B) < 0; });
	}

	/** Every tag name that at least one package references directly. */
	TSet<FName> CollectTagsWithReferences(const IAssetRegistry& Registry, const TArray<FName>& TagNames)
	{
		TSet<FName> Result;
		for (FName TagName : TagNames)
		{
			TArray<FAssetIdentifier> Referencers;
			Registry.GetReferencers(MakeTagIdentifier(TagName), Referencers,
				UE::AssetRegistry::EDependencyCategory::SearchableName);
			if (Referencers.Num() > 0)
			{
				Result.Add(TagName);
			}
		}
		return Result;
	}

	/**
	 * Describe the asset living in a package. A package can hold several top-level
	 * assets (a Level Sequence with a texture saved into it, for one), and the
	 * registry returns them in no particular order. The asset named after the
	 * package is the one a caller means; any other non-redirector is the fallback.
	 */
	void DescribePackage(const IAssetRegistry& Registry, FName PackageName, FString& OutAssetName, FString& OutAssetClass)
	{
		static const FTopLevelAssetPath RedirectorPath(TEXT("/Script/CoreUObject"), TEXT("ObjectRedirector"));

		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(PackageName, Assets, /*bIncludeOnlyOnDiskAssets=*/true);

		const FName ShortName = FPackageName::GetShortFName(PackageName);
		const FAssetData* Chosen = nullptr;
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetClassPath == RedirectorPath)
			{
				continue;
			}
			if (Asset.AssetName == ShortName)
			{
				Chosen = &Asset;
				break;
			}
			if (!Chosen)
			{
				Chosen = &Asset;
			}
		}

		if (Chosen)
		{
			OutAssetName = Chosen->AssetName.ToString();
			OutAssetClass = Chosen->AssetClassPath.GetAssetName().ToString();
			return;
		}

		// No asset data: the reference points at a package the registry has not
		// scanned, or one that was deleted while the reference survived. Report the
		// package's leaf name so the row still says something useful.
		const FString PackageString = PackageName.ToString();
		int32 SlashIndex = INDEX_NONE;
		OutAssetName = PackageString.FindLastChar(TEXT('/'), SlashIndex)
			? PackageString.Mid(SlashIndex + 1)
			: PackageString;
		OutAssetClass = FString();
	}

	/** Fill in hierarchy and authoring metadata for one tag, when the tag manager knows it. */
	void DescribeTagNode(const UGameplayTagsManager& TagManager, FName TagName, const TSharedPtr<FJsonObject>& Out)
	{
		const TSharedPtr<FGameplayTagNode> Node = TagManager.FindTagNode(TagName);
		if (!Node.IsValid())
		{
			return;
		}

		const TSharedPtr<FGameplayTagNode> ParentNode = Node->GetParentTagNode();
		Out->SetStringField(TEXT("parent"), ParentNode.IsValid() ? ParentNode->GetCompleteTagString() : FString());
		Out->SetNumberField(TEXT("direct_children"), Node->GetChildTagNodes().Num());

		const FString DevComment = Node->GetDevComment();
		if (!DevComment.IsEmpty())
		{
			Out->SetStringField(TEXT("dev_comment"), DevComment);
		}

		TArray<TSharedPtr<FJsonValue>> Sources;
		for (FName SourceName : Node->GetAllSourceNames())
		{
			Sources.Add(MakeShared<FJsonValueString>(SourceName.ToString()));
		}
		Out->SetArrayField(TEXT("sources"), Sources);
	}

	/** Strip an object suffix so both /Game/A/B and /Game/A/B.B resolve to the package name. */
	FString ToPackagePath(FString Path)
	{
		Path.TrimStartAndEndInline();
		int32 DotIndex = INDEX_NONE;
		if (Path.FindChar(TEXT('.'), DotIndex))
		{
			Path.LeftInline(DotIndex);
		}
		return Path;
	}

	/**
	 * Declared tag redirects, old name to new.
	 *
	 * FGameplayTagRedirectors owns the authoritative map, but it is not exported
	 * from the GameplayTags module, so the redirects are re-read from the config
	 * objects that feed it: the project's settings plus every ini-backed tag source
	 * (modules ship their own under Config/Tags).
	 */
	void CollectTagRedirects(const UGameplayTagsManager& TagManager, TMap<FName, FName>& OutRedirects)
	{
		auto AddFromList = [&OutRedirects](const UGameplayTagsList* List)
		{
			if (!List)
			{
				return;
			}
			for (const FGameplayTagRedirect& Redirect : List->GameplayTagRedirects)
			{
				if (!Redirect.OldTagName.IsNone() && !Redirect.NewTagName.IsNone())
				{
					OutRedirects.Add(Redirect.OldTagName, Redirect.NewTagName);
				}
			}
		};

		AddFromList(GetDefault<UGameplayTagsSettings>());

		const EGameplayTagSourceType IniSourceTypes[] = {
			EGameplayTagSourceType::DefaultTagList,
			EGameplayTagSourceType::TagList
		};

		TArray<const FGameplayTagSource*> Sources;
		for (EGameplayTagSourceType SourceType : IniSourceTypes)
		{
			Sources.Reset();
			TagManager.FindTagSourcesWithType(SourceType, Sources);
			for (const FGameplayTagSource* Source : Sources)
			{
				if (Source)
				{
					AddFromList(Source->SourceTagList);
				}
			}
		}
	}

	/** Follow a redirect chain to its end. Returns NAME_None when the name is not redirected. */
	FName ResolveRedirect(const TMap<FName, FName>& Redirects, FName TagName)
	{
		constexpr int32 MaxRedirectDepth = 10;

		FName Current = TagName;
		for (int32 Depth = 0; Depth < MaxRedirectDepth; ++Depth)
		{
			const FName* Next = Redirects.Find(Current);
			if (!Next || *Next == Current)
			{
				break;
			}
			Current = *Next;
		}
		return Current == TagName ? NAME_None : Current;
	}

	/** All tags the manager knows, optionally narrowed to a prefix, sorted by name. */
	TArray<FName> GatherTagNames(const UGameplayTagsManager& TagManager, const FString& Prefix)
	{
		FGameplayTagContainer AllTags;
		TagManager.RequestAllGameplayTags(AllTags, /*OnlyIncludeDictionaryTags=*/false);

		TArray<FName> Names;
		Names.Reserve(AllTags.Num());
		for (const FGameplayTag& Tag : AllTags)
		{
			if (MatchesPrefix(Tag.ToString(), Prefix))
			{
				Names.Add(Tag.GetTagName());
			}
		}

		Names.Sort([](FName A, FName B) { return A.Compare(B) < 0; });
		return Names;
	}
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithGameplayTagsActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TagsNamespace, TEXT("ping"),
		TEXT("Smoke test - returns {status:ok, module:MonolithGameplayTags} plus the tag count the tag manager knows and whether the Asset Registry has finished loading."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameplayTagsActions::Ping),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TagsNamespace, TEXT("find_references"),
		TEXT("List the assets that reference a gameplay tag, read from the Asset Registry SearchableName dependency graph. Finds tags wherever they are serialized: asset properties, graph nodes, components on a level. A reference is recorded against the exact tag written in the asset, so pass include_children=true to sweep a whole subtree. Only on-disk state is known: an edited but unsaved asset is not reported."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameplayTagsActions::FindReferences),
		FParamSchemaBuilder()
			.Required(TEXT("tag"), TEXT("string"), TEXT("Full tag name, e.g. \"Ability.Combat.Melee\". An unregistered tag is not an error - references can outlive the declaration."))
			.Optional(TEXT("include_children"), TEXT("boolean"), TEXT("Also query every tag below this one in the hierarchy. Default false."))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum referencing packages to return, clamped to 1-5000. Default 200."))
			.Build());

	Registry.RegisterAction(TagsNamespace, TEXT("get_asset_tags"),
		TEXT("List the gameplay tags one asset references, the reverse of find_references. Reads the asset's own SearchableName dependencies, so it covers tags held by nested objects (Flow nodes, components) as well as plain properties. Accepts a package path or an object path."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameplayTagsActions::GetAssetTags),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Package or object path, e.g. \"/Game/Abilities/GA_Melee\" or \"/Game/Abilities/GA_Melee.GA_Melee\""))
			.Build());

	Registry.RegisterAction(TagsNamespace, TEXT("list"),
		TEXT("List gameplay tags known to the tag manager with their parent, direct-child count, declaring sources and dev comment. Live data, no reindex involved. Narrow with prefix."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameplayTagsActions::List),
		FParamSchemaBuilder()
			.Optional(TEXT("prefix"), TEXT("string"), TEXT("Only tags equal to this or below it, e.g. \"Ability.Combat\". Empty lists every tag."))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum tags to return, clamped to 1-5000. Default 500."))
			.Build());

	Registry.RegisterAction(TagsNamespace, TEXT("get_tag_info"),
		TEXT("Describe one gameplay tag: registration state, parent, direct children, declaring sources, dev comment, and how many packages reference it directly versus anywhere in its subtree."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameplayTagsActions::GetTagInfo),
		FParamSchemaBuilder()
			.Required(TEXT("tag"), TEXT("string"), TEXT("Full tag name, e.g. \"Ability.Combat.Melee\""))
			.Build());

	Registry.RegisterAction(TagsNamespace, TEXT("find_unused"),
		TEXT("Find tags nothing uses - no asset, no project C++, no project config - and that can be removed by editing its declaring source (an ini or a tag DataTable, listed in `sources`). A parent tag counts as used when anything in its subtree is used, because leaf tags are what code and assets normally carry; `branch_root` marks the row where a dead branch starts, the one name to delete for all of it. Assets come from the Asset Registry. C++ evidence is a string literal holding the exact tag name in the project's own sources (Monolith and plugins built on it excluded), read straight from disk on every call, so fresh code counts without a source reindex. Config counts too, through the (TagName=\"A.B\") form a tag-valued setting is saved in; declarations (Tag=\"A.B\") and redirects do not count. `code_referenced_tags` names the file that rescued each tag. Native tags (declared in C++) are never in `unused`: they go to `native_unreferenced_tags` with the project file that declares them, or an empty `declared_in` when the engine or a plugin does. A tag assembled at runtime from pieces leaves no literal anywhere, so confirm a candidate before deleting it."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameplayTagsActions::FindUnused),
		FParamSchemaBuilder()
			.Optional(TEXT("prefix"), TEXT("string"), TEXT("Only consider tags equal to this or below it. Empty scans every tag."))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum tags to return, clamped to 1-5000. Default 500."))
			.Optional(TEXT("scan_source"), TEXT("boolean"), TEXT("Cross-check candidates against C++ sources. Default true. Turning it off makes the call cheaper and the answer asset-only."))
			.Build());

	Registry.RegisterAction(TagsNamespace, TEXT("find_broken"),
		TEXT("Find assets referencing tags the tag manager no longer knows - a tag deleted or renamed out from under content. Walks every package's SearchableName dependencies, so it covers tags held by nested objects too. A reference covered by a GameplayTagRedirect is reported separately under `redirected` rather than as broken: those still resolve on load, but the asset carries the old name until it is resaved."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameplayTagsActions::FindBroken),
		FParamSchemaBuilder()
			.Optional(TEXT("path_prefix"), TEXT("string"), TEXT("Only packages under this path, e.g. \"/Game/\" or \"/MyPlugin/\". Empty scans every mounted package."))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum distinct broken tags to return, clamped to 1-5000. Default 500."))
			.Build());
}

// ============================================================================
// Handlers
// ============================================================================

FMonolithActionResult FMonolithGameplayTagsActions::Ping(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("module"), TEXT("MonolithGameplayTags"));

	FGameplayTagContainer AllTags;
	UGameplayTagsManager::Get().RequestAllGameplayTags(AllTags, /*OnlyIncludeDictionaryTags=*/false);
	Result->SetNumberField(TEXT("tags_known"), AllTags.Num());

	const IAssetRegistry* Registry = IAssetRegistry::Get();
	Result->SetBoolField(TEXT("asset_registry_available"), Registry != nullptr);
	Result->SetBoolField(TEXT("asset_registry_loading"), Registry != nullptr && Registry->IsLoadingAssets());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayTagsActions::FindReferences(const TSharedPtr<FJsonObject>& Params)
{
	FString TagString;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("tag"), TagString))
	{
		return FMonolithActionResult::Error(
			TEXT("'tag' parameter is required, e.g. \"Ability.Combat.Melee\""), -32602);
	}

	TagString.TrimStartAndEndInline();
	if (TagString.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'tag' must not be empty"), -32602);
	}

	IAssetRegistry* Registry = IAssetRegistry::Get();
	if (!Registry)
	{
		return FMonolithActionResult::Error(TEXT("Asset Registry is not available"));
	}

	bool bIncludeChildren = false;
	Params->TryGetBoolField(TEXT("include_children"), bIncludeChildren);
	const int32 Limit = ReadLimit(Params, DefaultReferencerLimit, MaxReferencerLimit);

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
	const FName TagName(*TagString);
	const FGameplayTag Tag = TagManager.RequestGameplayTag(TagName, /*ErrorIfNotFound=*/false);

	// An unregistered tag is answered rather than rejected: a reference can outlive
	// the declaration that created it, and surfacing exactly those is what makes
	// this action useful for cleanup. `registered` in the response tells them apart.
	TArray<FName> TagsToQuery;
	if (bIncludeChildren)
	{
		TagsToQuery = CollectTagSubtree(TagManager, Tag, TagName);
	}
	else
	{
		TagsToQuery.Add(TagName);
	}

	TMap<FName, FName> PackageToTag;
	TArray<FName> OrderedPackages;
	CollectReferencerPackages(*Registry, TagsToQuery, PackageToTag, OrderedPackages);

	const int32 Total = OrderedPackages.Num();
	const int32 Reported = FMath::Min(Total, Limit);

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Reported);

	for (int32 Index = 0; Index < Reported; ++Index)
	{
		const FName PackageName = OrderedPackages[Index];

		FString AssetName;
		FString AssetClass;
		DescribePackage(*Registry, PackageName, AssetName, AssetClass);

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("package_path"), PackageName.ToString());
		Row->SetStringField(TEXT("asset_name"), AssetName);
		Row->SetStringField(TEXT("asset_class"), AssetClass);
		Row->SetStringField(TEXT("via_tag"), PackageToTag[PackageName].ToString());
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("tag"), TagString);
	Result->SetBoolField(TEXT("registered"), Tag.IsValid());
	Result->SetBoolField(TEXT("include_children"), bIncludeChildren);
	Result->SetNumberField(TEXT("tags_queried"), TagsToQuery.Num());
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetNumberField(TEXT("returned"), Reported);
	Result->SetBoolField(TEXT("truncated"), Reported < Total);
	Result->SetArrayField(TEXT("referencers"), Rows);

	// The registry answers from what it has scanned so far, so a mid-scan call can
	// under-report. Say so rather than let a short list read as a complete one.
	if (Registry->IsLoadingAssets())
	{
		Result->SetBoolField(TEXT("asset_registry_loading"), true);
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayTagsActions::GetAssetTags(const TSharedPtr<FJsonObject>& Params)
{
	FString RawPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), RawPath))
	{
		return FMonolithActionResult::Error(
			TEXT("'asset_path' parameter is required, e.g. \"/Game/Abilities/GA_Melee\""), -32602);
	}

	const FString PackagePath = ToPackagePath(RawPath);
	if (PackagePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'asset_path' must not be empty"), -32602);
	}

	IAssetRegistry* Registry = IAssetRegistry::Get();
	if (!Registry)
	{
		return FMonolithActionResult::Error(TEXT("Asset Registry is not available"));
	}

	const FName PackageName(*PackagePath);

	TArray<FAssetIdentifier> Dependencies;
	Registry->GetDependencies(FAssetIdentifier(PackageName), Dependencies,
		UE::AssetRegistry::EDependencyCategory::SearchableName);

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();

	TArray<FName> TagNames;
	for (const FAssetIdentifier& Dependency : Dependencies)
	{
		if (IsGameplayTagIdentifier(Dependency))
		{
			TagNames.AddUnique(Dependency.ValueName);
		}
	}
	TagNames.Sort([](FName A, FName B) { return A.Compare(B) < 0; });

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(TagNames.Num());
	for (FName Name : TagNames)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("tag"), Name.ToString());
		Row->SetBoolField(TEXT("registered"), TagManager.RequestGameplayTag(Name, /*ErrorIfNotFound=*/false).IsValid());
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	FString AssetName;
	FString AssetClass;
	DescribePackage(*Registry, PackageName, AssetName, AssetClass);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetStringField(TEXT("asset_name"), AssetName);
	Result->SetStringField(TEXT("asset_class"), AssetClass);
	// An empty asset_class means the registry has no asset data for this package,
	// which usually means the path is wrong. Distinguish that from "asset exists
	// and carries no tags", because both otherwise return an empty tag list.
	Result->SetBoolField(TEXT("package_known"), !AssetClass.IsEmpty());
	Result->SetNumberField(TEXT("total"), TagNames.Num());
	Result->SetArrayField(TEXT("tags"), Rows);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayTagsActions::List(const TSharedPtr<FJsonObject>& Params)
{
	const FString Prefix = ReadPrefix(Params);
	const int32 Limit = ReadLimit(Params, DefaultListLimit, MaxListLimit);

	const UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
	const TArray<FName> TagNames = GatherTagNames(TagManager, Prefix);

	const int32 Total = TagNames.Num();
	const int32 Reported = FMath::Min(Total, Limit);

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Reported);

	for (int32 Index = 0; Index < Reported; ++Index)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("tag"), TagNames[Index].ToString());
		DescribeTagNode(TagManager, TagNames[Index], Row);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("prefix"), Prefix);
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetNumberField(TEXT("returned"), Reported);
	Result->SetBoolField(TEXT("truncated"), Reported < Total);
	Result->SetArrayField(TEXT("tags"), Rows);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayTagsActions::GetTagInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString TagString;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("tag"), TagString))
	{
		return FMonolithActionResult::Error(
			TEXT("'tag' parameter is required, e.g. \"Ability.Combat.Melee\""), -32602);
	}

	TagString.TrimStartAndEndInline();
	if (TagString.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'tag' must not be empty"), -32602);
	}

	IAssetRegistry* Registry = IAssetRegistry::Get();
	if (!Registry)
	{
		return FMonolithActionResult::Error(TEXT("Asset Registry is not available"));
	}

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
	const FName TagName(*TagString);
	const FGameplayTag Tag = TagManager.RequestGameplayTag(TagName, /*ErrorIfNotFound=*/false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("tag"), TagString);
	Result->SetBoolField(TEXT("registered"), Tag.IsValid());
	DescribeTagNode(TagManager, TagName, Result);

	if (const TSharedPtr<FGameplayTagNode> Node = TagManager.FindTagNode(TagName))
	{
		TArray<TSharedPtr<FJsonValue>> Children;
		for (const TSharedPtr<FGameplayTagNode>& Child : Node->GetChildTagNodes())
		{
			if (Child.IsValid())
			{
				Children.Add(MakeShared<FJsonValueString>(Child->GetCompleteTagString()));
			}
		}
		Result->SetArrayField(TEXT("children"), Children);
	}

	TMap<FName, FName> DirectPackages;
	TArray<FName> DirectOrdered;
	CollectReferencerPackages(*Registry, { TagName }, DirectPackages, DirectOrdered);
	Result->SetNumberField(TEXT("references_direct"), DirectOrdered.Num());

	const TArray<FName> Subtree = CollectTagSubtree(TagManager, Tag, TagName);
	TMap<FName, FName> SubtreePackages;
	TArray<FName> SubtreeOrdered;
	CollectReferencerPackages(*Registry, Subtree, SubtreePackages, SubtreeOrdered);
	Result->SetNumberField(TEXT("references_subtree"), SubtreeOrdered.Num());
	Result->SetNumberField(TEXT("subtree_size"), Subtree.Num());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayTagsActions::FindUnused(const TSharedPtr<FJsonObject>& Params)
{
	const FString Prefix = ReadPrefix(Params);
	const int32 Limit = ReadLimit(Params, DefaultListLimit, MaxListLimit);

	IAssetRegistry* Registry = IAssetRegistry::Get();
	if (!Registry)
	{
		return FMonolithActionResult::Error(TEXT("Asset Registry is not available"));
	}

	bool bScanSource = true;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("scan_source"), bScanSource);
	}

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();

	// Referenced-ness is computed over every tag once, not per candidate: a parent
	// counts as used when anything in its subtree is referenced, and asking the
	// registry again for each ancestor would repeat the same queries.
	const TArray<FName> AllTagNames = GatherTagNames(TagManager, FString());
	const TSet<FName> Referenced = CollectTagsWithReferences(*Registry, AllTagNames);

	// Pass 1: everything the assets do not account for becomes a candidate. Native
	// tags are declared in C++ and cannot be removed from an ini, so they are kept
	// apart, and so is any branch a native tag keeps alive from below.
	TArray<FName> Candidates;
	TMap<FName, TArray<FName>> CandidateSubtrees;
	TArray<FName> NativeCandidates;
	TSet<FName> NativeTags;
	int32 NativeBranchesSkipped = 0;

	for (FName TagName : AllTagNames)
	{
		if (IsNativeTag(TagManager, TagName))
		{
			NativeTags.Add(TagName);
		}
	}

	for (FName TagName : AllTagNames)
	{
		if (!MatchesPrefix(TagName.ToString(), Prefix))
		{
			continue;
		}

		const FGameplayTag Tag = TagManager.RequestGameplayTag(TagName, /*ErrorIfNotFound=*/false);
		TArray<FName> Subtree = CollectTagSubtree(TagManager, Tag, TagName);

		bool bUsedByAsset = false;
		bool bHasNativeBelow = false;
		for (FName SubtreeTag : Subtree)
		{
			bUsedByAsset |= Referenced.Contains(SubtreeTag);
			bHasNativeBelow |= SubtreeTag != TagName && NativeTags.Contains(SubtreeTag);
		}
		if (bUsedByAsset)
		{
			continue;
		}

		if (NativeTags.Contains(TagName))
		{
			NativeCandidates.Add(TagName);
			continue;
		}
		if (bHasNativeBelow)
		{
			++NativeBranchesSkipped;
			continue;
		}

		Candidates.Add(TagName);
		CandidateSubtrees.Add(TagName, MoveTemp(Subtree));
	}

	// Pass 2: assets are only half the picture on a project that broadcasts tags
	// from C++ - those tags sit in no package at all. Evidence is a string literal
	// holding the exact tag name in the project's own C++, tooling plugins
	// excluded, or a tag-valued setting in the project's config, which is saved as
	// (TagName="A.B"). The files are read from disk every time: Monolith's source index
	// is shared with the engine's source, so a phrase query for a common word
	// walks thousands of engine lines before a project filter can apply, and the
	// index is rebuilt by hand, so fresh code would be missed anyway. Native
	// candidates are checked for themselves only, and only when declared
	// explicitly, to say whether the project or the engine declares them; an
	// implicit parent has no declaration to find.
	TMap<FName, FString> CodeEvidence;
	FString SourceCheckMode = TEXT("skipped");
	FProjectTagEvidence Evidence;
	double SourceCheckSeconds = 0.0;

	if (bScanSource)
	{
		const double CheckStart = FPlatformTime::Seconds();
		SourceCheckMode = TEXT("disk");
		CollectProjectTagEvidence(Evidence);

		for (FName Candidate : Candidates)
		{
			for (FName SubtreeTag : CandidateSubtrees[Candidate])
			{
				if (const FString* File = Evidence.FindAny(SubtreeTag.ToString()))
				{
					CodeEvidence.Add(Candidate, *File);
					break;
				}
			}
		}
		// A config use is evidence a tag is used, never that the config declares
		// it, so only C++ literals feed declared_in.
		for (FName NativeTag : NativeCandidates)
		{
			const TSharedPtr<FGameplayTagNode> Node = TagManager.FindTagNode(NativeTag);
			if (Node.IsValid() && Node->IsExplicitTag())
			{
				if (const FString* File = Evidence.CppLiterals.Find(NativeTag.ToString()))
				{
					CodeEvidence.Add(NativeTag, *File);
				}
			}
		}

		SourceCheckSeconds = FPlatformTime::Seconds() - CheckStart;
	}

	TArray<FName> Unused;
	TArray<FName> CodeReferenced;
	for (FName Candidate : Candidates)
	{
		if (CodeEvidence.Contains(Candidate))
		{
			CodeReferenced.Add(Candidate);
		}
		else
		{
			Unused.Add(Candidate);
		}
	}

	// Every candidate's whole subtree is already dead, so the useful signal is
	// where a dead branch starts: a row whose parent is not itself unused is the
	// one name to delete for the whole branch.
	TSet<FName> UnusedSet;
	UnusedSet.Append(Unused);

	const int32 Total = Unused.Num();
	const int32 Reported = FMath::Min(Total, Limit);

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Reported);
	for (int32 Index = 0; Index < Reported; ++Index)
	{
		const FName TagName = Unused[Index];
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("tag"), TagName.ToString());
		DescribeTagNode(TagManager, TagName, Row);

		const TSharedPtr<FGameplayTagNode> Node = TagManager.FindTagNode(TagName);
		const TSharedPtr<FGameplayTagNode> ParentNode = Node.IsValid() ? Node->GetParentTagNode() : TSharedPtr<FGameplayTagNode>();
		const bool bParentUnused = ParentNode.IsValid() && UnusedSet.Contains(ParentNode->GetCompleteTagName());
		Row->SetBoolField(TEXT("explicit"), Node.IsValid() && Node->IsExplicitTag());
		Row->SetBoolField(TEXT("branch_root"), !bParentUnused);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("prefix"), Prefix);
	Result->SetNumberField(TEXT("tags_scanned"), AllTagNames.Num());
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetNumberField(TEXT("returned"), Reported);
	Result->SetBoolField(TEXT("truncated"), Reported < Total);
	Result->SetArrayField(TEXT("unused"), Rows);

	// Native tags with no asset reference: not deletable from an ini, so never in
	// `unused`. declared_in names the project file that declares the tag, or is
	// empty when the declaration lives outside the project (engine or a plugin).
	const int32 NativeReported = FMath::Min(NativeCandidates.Num(), Limit);
	TArray<TSharedPtr<FJsonValue>> NativeRows;
	NativeRows.Reserve(NativeReported);
	for (int32 Index = 0; Index < NativeReported; ++Index)
	{
		const FName TagName = NativeCandidates[Index];
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("tag"), TagName.ToString());
		const TSharedPtr<FGameplayTagNode> Node = TagManager.FindTagNode(TagName);
		Row->SetBoolField(TEXT("explicit"), Node.IsValid() && Node->IsExplicitTag());
		const FString* DeclaredIn = CodeEvidence.Find(TagName);
		Row->SetStringField(TEXT("declared_in"), DeclaredIn ? ToProjectRelative(*DeclaredIn) : FString());
		NativeRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetNumberField(TEXT("native_unreferenced"), NativeCandidates.Num());
	Result->SetArrayField(TEXT("native_unreferenced_tags"), NativeRows);
	Result->SetNumberField(TEXT("native_branches_skipped"), NativeBranchesSkipped);

	Result->SetBoolField(TEXT("scan_source"), bScanSource);
	Result->SetStringField(TEXT("source_check"), SourceCheckMode);
	if (bScanSource)
	{
		Result->SetNumberField(TEXT("source_files_scanned"), Evidence.SourceFileCount);
		Result->SetNumberField(TEXT("config_files_scanned"), Evidence.ConfigFileCount);
		Result->SetNumberField(TEXT("source_check_seconds"), FMath::RoundToFloat(SourceCheckSeconds * 100.0) / 100.0);
		Result->SetNumberField(TEXT("code_referenced"), CodeReferenced.Num());

		// Name the tags the source scan rescued and the file that rescued each, so
		// a caller can check the evidence instead of trusting it.
		constexpr int32 MaxReportedCodeReferenced = 50;
		TArray<TSharedPtr<FJsonValue>> CodeRows;
		for (int32 Index = 0; Index < FMath::Min(CodeReferenced.Num(), MaxReportedCodeReferenced); ++Index)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("tag"), CodeReferenced[Index].ToString());
			Row->SetStringField(TEXT("file"), ToProjectRelative(CodeEvidence[CodeReferenced[Index]]));
			CodeRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Result->SetArrayField(TEXT("code_referenced_tags"), CodeRows);
	}

	if (Registry->IsLoadingAssets())
	{
		Result->SetBoolField(TEXT("asset_registry_loading"), true);
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayTagsActions::FindBroken(const TSharedPtr<FJsonObject>& Params)
{
	IAssetRegistry* Registry = IAssetRegistry::Get();
	if (!Registry)
	{
		return FMonolithActionResult::Error(TEXT("Asset Registry is not available"));
	}

	FString PathPrefix;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("path_prefix"), PathPrefix);
		PathPrefix.TrimStartAndEndInline();
	}
	const int32 Limit = ReadLimit(Params, DefaultListLimit, MaxListLimit);

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();

	TMap<FName, FName> Redirects;
	CollectTagRedirects(TagManager, Redirects);

	enum class ETagState : uint8
	{
		Known,
		Redirected,
		Broken
	};

	// A project holds far more references than distinct tags, so each name is
	// resolved once and the verdict cached.
	TMap<FName, ETagState> TagStates;
	TMap<FName, FString> RedirectTargets;
	TMap<FName, TArray<FName>> BrokenReferencers;
	TMap<FName, TArray<FName>> RedirectedReferencers;

	int32 PackagesScanned = 0;
	int32 PackagesWithTags = 0;
	const double ScanStart = FPlatformTime::Seconds();

	TArray<FAssetIdentifier> Dependencies;
	Registry->EnumerateAllPackages(
		[&](FName PackageName, const FAssetPackageData& PackageData)
		{
			const FString PackageString = PackageName.ToString();
			if (PackageString.StartsWith(TEXT("/Script/")) || PackageString.StartsWith(TEXT("/Temp/")))
			{
				return;
			}
			if (!PathPrefix.IsEmpty() && !PackageString.StartsWith(PathPrefix))
			{
				return;
			}

			++PackagesScanned;

			Dependencies.Reset();
			Registry->GetDependencies(FAssetIdentifier(PackageName), Dependencies,
				UE::AssetRegistry::EDependencyCategory::SearchableName);

			bool bCarriesTag = false;
			for (const FAssetIdentifier& Dependency : Dependencies)
			{
				if (!IsGameplayTagIdentifier(Dependency))
				{
					continue;
				}
				bCarriesTag = true;

				const FName TagName = Dependency.ValueName;
				ETagState State = ETagState::Known;

				if (const ETagState* Cached = TagStates.Find(TagName))
				{
					State = *Cached;
				}
				else
				{
					if (!TagManager.RequestGameplayTag(TagName, /*ErrorIfNotFound=*/false).IsValid())
					{
						// Unregistered is not automatically broken: a redirect keeps the
						// reference working until the asset is resaved under the new name.
						// A redirect pointing at a tag that is itself gone does not save
						// anything, so that case stays broken.
						const FName RedirectTarget = ResolveRedirect(Redirects, TagName);
						if (!RedirectTarget.IsNone()
							&& TagManager.RequestGameplayTag(RedirectTarget, /*ErrorIfNotFound=*/false).IsValid())
						{
							State = ETagState::Redirected;
							RedirectTargets.Add(TagName, RedirectTarget.ToString());
						}
						else
						{
							State = ETagState::Broken;
						}
					}
					TagStates.Add(TagName, State);
				}

				if (State == ETagState::Broken)
				{
					BrokenReferencers.FindOrAdd(TagName).Add(PackageName);
				}
				else if (State == ETagState::Redirected)
				{
					RedirectedReferencers.FindOrAdd(TagName).Add(PackageName);
				}
			}

			if (bCarriesTag)
			{
				++PackagesWithTags;
			}
		});

	const double ScanSeconds = FPlatformTime::Seconds() - ScanStart;

	TArray<FName> BrokenTags;
	BrokenReferencers.GetKeys(BrokenTags);
	BrokenTags.Sort([](FName A, FName B) { return A.Compare(B) < 0; });

	constexpr int32 MaxReferencersPerTag = 20;

	TArray<TSharedPtr<FJsonValue>> BrokenRows;
	const int32 Reported = FMath::Min(BrokenTags.Num(), Limit);
	for (int32 Index = 0; Index < Reported; ++Index)
	{
		const FName TagName = BrokenTags[Index];
		TArray<FName>& Packages = BrokenReferencers[TagName];
		Packages.Sort([](FName A, FName B) { return A.Compare(B) < 0; });

		TArray<TSharedPtr<FJsonValue>> ReferencerRows;
		for (int32 PackageIndex = 0; PackageIndex < FMath::Min(Packages.Num(), MaxReferencersPerTag); ++PackageIndex)
		{
			FString AssetName;
			FString AssetClass;
			DescribePackage(*Registry, Packages[PackageIndex], AssetName, AssetClass);

			TSharedPtr<FJsonObject> ReferencerRow = MakeShared<FJsonObject>();
			ReferencerRow->SetStringField(TEXT("package_path"), Packages[PackageIndex].ToString());
			ReferencerRow->SetStringField(TEXT("asset_name"), AssetName);
			ReferencerRow->SetStringField(TEXT("asset_class"), AssetClass);
			ReferencerRows.Add(MakeShared<FJsonValueObject>(ReferencerRow));
		}

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("tag"), TagName.ToString());
		Row->SetNumberField(TEXT("referencer_count"), Packages.Num());
		Row->SetArrayField(TEXT("referencers"), ReferencerRows);
		BrokenRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TArray<FName> RedirectedTags;
	RedirectedReferencers.GetKeys(RedirectedTags);
	RedirectedTags.Sort([](FName A, FName B) { return A.Compare(B) < 0; });

	constexpr int32 MaxReportedRedirects = 50;
	TArray<TSharedPtr<FJsonValue>> RedirectedRows;
	for (int32 Index = 0; Index < FMath::Min(RedirectedTags.Num(), MaxReportedRedirects); ++Index)
	{
		const FName TagName = RedirectedTags[Index];
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("tag"), TagName.ToString());
		Row->SetStringField(TEXT("redirects_to"), RedirectTargets.FindRef(TagName));
		Row->SetNumberField(TEXT("referencer_count"), RedirectedReferencers[TagName].Num());
		RedirectedRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path_prefix"), PathPrefix);
	Result->SetNumberField(TEXT("packages_scanned"), PackagesScanned);
	Result->SetNumberField(TEXT("packages_with_tags"), PackagesWithTags);
	Result->SetNumberField(TEXT("distinct_tags_seen"), TagStates.Num());
	Result->SetNumberField(TEXT("scan_seconds"), FMath::RoundToFloat(ScanSeconds * 100.0) / 100.0);
	Result->SetNumberField(TEXT("total"), BrokenTags.Num());
	Result->SetNumberField(TEXT("returned"), Reported);
	Result->SetBoolField(TEXT("truncated"), Reported < BrokenTags.Num());
	Result->SetArrayField(TEXT("broken"), BrokenRows);
	Result->SetNumberField(TEXT("redirected_total"), RedirectedTags.Num());
	Result->SetArrayField(TEXT("redirected"), RedirectedRows);

	if (Registry->IsLoadingAssets())
	{
		Result->SetBoolField(TEXT("asset_registry_loading"), true);
	}

	return FMonolithActionResult::Success(Result);
}
