#include "MonolithGameplayTagsWriteActions.h"
#include "MonolithGameplayTagsInternal.h"
#include "MonolithParamSchema.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsEditorModule.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "ISourceControlModule.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SourceControlHelpers.h"

using namespace MonolithGameplayTags;

namespace
{
	const TCHAR* const TagsNamespace = TEXT("gameplay_tags");

	/** Referencing packages named per blocked tag; the count is always complete. */
	constexpr int32 MaxListedReferencers = 20;

	const TCHAR* SourceTypeName(EGameplayTagSourceType Type)
	{
		switch (Type)
		{
		case EGameplayTagSourceType::Native:            return TEXT("native");
		case EGameplayTagSourceType::DefaultTagList:    return TEXT("default_ini");
		case EGameplayTagSourceType::TagList:           return TEXT("ini");
		case EGameplayTagSourceType::RestrictedTagList: return TEXT("restricted_ini");
		case EGameplayTagSourceType::DataTable:         return TEXT("data_table");
		default:                                        return TEXT("invalid");
		}
	}

	bool IsIniSource(const FGameplayTagSource* Source)
	{
		return Source && (Source->SourceType == EGameplayTagSourceType::DefaultTagList
			|| Source->SourceType == EGameplayTagSourceType::TagList)
			&& Source->SourceTagList;
	}

	/** Tag sources are named after their ini file; accept the name with or without the extension. */
	FName NormaliseSourceName(FString Name)
	{
		Name.TrimStartAndEndInline();
		if (!Name.IsEmpty() && !Name.EndsWith(TEXT(".ini"), ESearchCase::IgnoreCase))
		{
			Name += TEXT(".ini");
		}
		return FName(*Name);
	}

	/** One tag name or an array of them, trimmed, empty entries dropped. */
	bool ReadTagList(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, TArray<FString>& OutTags, FString& OutError)
	{
		OutTags.Reset();
		if (!Params.IsValid())
		{
			OutError = FString::Printf(TEXT("Missing required parameter: %s"), Field);
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		FString Single;
		if (Params->TryGetArrayField(Field, Array))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				FString Tag;
				if (Value.IsValid() && Value->TryGetString(Tag))
				{
					Tag.TrimStartAndEndInline();
					if (!Tag.IsEmpty())
					{
						OutTags.Add(MoveTemp(Tag));
					}
				}
			}
		}
		else if (Params->TryGetStringField(Field, Single))
		{
			Single.TrimStartAndEndInline();
			if (!Single.IsEmpty())
			{
				OutTags.Add(MoveTemp(Single));
			}
		}

		if (OutTags.Num() == 0)
		{
			OutError = FString::Printf(TEXT("%s must name at least one tag"), Field);
			return false;
		}
		return true;
	}

	bool ReadBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool bDefault)
	{
		bool bValue = bDefault;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(Field, bValue);
		}
		return bValue;
	}

	/** Everything the tag manager knows about one tag's declaration. */
	struct FTagDeclaration
	{
		bool bKnown = false;
		bool bExplicit = false;
		bool bRestricted = false;
		bool bAllowNonRestrictedChildren = true;
		FString Comment;
		TArray<FName> Sources;
	};

	FTagDeclaration ReadDeclaration(const UGameplayTagsManager& TagManager, FName TagName)
	{
		FTagDeclaration Declaration;
		Declaration.bKnown = TagManager.GetTagEditorData(TagName, Declaration.Comment, Declaration.Sources,
			Declaration.bExplicit, Declaration.bRestricted, Declaration.bAllowNonRestrictedChildren);
		return Declaration;
	}

	/**
	 * True when some ini tag list still holds a row declaring exactly this name.
	 * The read-back after a delete or rename cannot use GetTagEditorData or
	 * FindTagNode: both follow GameplayTagRedirects, so once a rename has written
	 * old->new they describe the new tag when asked about the old name.
	 */
	bool HasIniDeclarationRow(const UGameplayTagsManager& TagManager, FName TagName)
	{
		const EGameplayTagSourceType IniTypes[] = {
			EGameplayTagSourceType::DefaultTagList,
			EGameplayTagSourceType::TagList,
			EGameplayTagSourceType::RestrictedTagList
		};
		TArray<const FGameplayTagSource*> Sources;
		for (EGameplayTagSourceType Type : IniTypes)
		{
			Sources.Reset();
			TagManager.FindTagSourcesWithType(Type, Sources);
			for (const FGameplayTagSource* Source : Sources)
			{
				if (!Source)
				{
					continue;
				}
				if (Source->SourceTagList && Source->SourceTagList->GameplayTagList.ContainsByPredicate(
					[TagName](const FGameplayTagTableRow& Row) { return Row.Tag == TagName; }))
				{
					return true;
				}
				if (Source->SourceRestrictedTagList && Source->SourceRestrictedTagList->RestrictedGameplayTagList.ContainsByPredicate(
					[TagName](const FRestrictedGameplayTagTableRow& Row) { return Row.Tag == TagName; }))
				{
					return true;
				}
			}
		}
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> ToJsonStrings(const TArray<FName>& Names)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (FName Name : Names)
		{
			Values.Add(MakeShared<FJsonValueString>(Name.ToString()));
		}
		return Values;
	}

	TArray<TSharedPtr<FJsonValue>> ToJsonStrings(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	/**
	 * Why an ini edit cannot change this declaration, or empty when it can. Native
	 * and DataTable declarations live in C++ and in an asset respectively.
	 */
	FString WhyNotIniEditable(const UGameplayTagsManager& TagManager, const FTagDeclaration& Declaration)
	{
		for (FName SourceName : Declaration.Sources)
		{
			const FGameplayTagSource* Source = TagManager.FindTagSource(SourceName);
			if (!Source)
			{
				continue;
			}
			if (Source->SourceType == EGameplayTagSourceType::Native)
			{
				return FString::Printf(TEXT("declared in C++ (source %s); change the UE_DEFINE_GAMEPLAY_TAG or AddNativeGameplayTag call instead"), *SourceName.ToString());
			}
			if (Source->SourceType == EGameplayTagSourceType::DataTable)
			{
				return FString::Printf(TEXT("declared in the tag DataTable %s; edit that DataTable instead"), *SourceName.ToString());
			}
		}
		return FString();
	}

	/**
	 * Make a config file writable before saving it ourselves, as the tag editor
	 * module does for its own saves: check it out when source control is on,
	 * otherwise clear a read-only flag.
	 */
	void PrepareConfigForWrite(const FString& ConfigFileName)
	{
		const FString FullPath = FPaths::ConvertRelativePathToFull(ConfigFileName);
		if (ISourceControlModule::Get().IsEnabled())
		{
			USourceControlHelpers::CheckOutOrAddFile(FullPath, /*bSilent=*/true);
		}
		else if (IFileManager::Get().FileExists(*FullPath) && IFileManager::Get().IsReadOnly(*FullPath))
		{
			FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*FullPath, false);
		}
	}

	/** Packages referencing a tag, at most MaxListedReferencers of them, plus the full count. */
	void DescribeReferencers(const TArray<FName>& Packages, const TSharedPtr<FJsonObject>& Out)
	{
		Out->SetNumberField(TEXT("referencer_count"), Packages.Num());
		TArray<FName> Listed(Packages);
		Listed.Sort([](FName A, FName B) { return A.Compare(B) < 0; });
		if (Listed.Num() > MaxListedReferencers)
		{
			Listed.SetNum(MaxListedReferencers);
		}
		Out->SetArrayField(TEXT("referencers"), ToJsonStrings(Listed));
	}

	/** Suspends the tag tree rebuild for a batch of edits and rebuilds once at the end. */
	struct FScopedTagTreeRefreshSuspension
	{
		FGuid Token = FGuid::NewGuid();

		FScopedTagTreeRefreshSuspension()
		{
			UGameplayTagsManager::Get().SuspendEditorRefreshGameplayTagTree(Token);
		}

		~FScopedTagTreeRefreshSuspension()
		{
			UGameplayTagsManager::Get().ResumeEditorRefreshGameplayTagTree(Token);
		}
	};
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithGameplayTagsWriteActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TagsNamespace, TEXT("list_sources"),
		TEXT("List the places gameplay tags are declared: ini files (the default DefaultGameplayTags.ini, Config/Tags/*.ini, restricted lists), tag DataTables and native C++. Each source reports its type, config file and how many tags it declares. Use it to pick a `source` for add or move."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameplayTagsWriteActions::ListSources),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TagsNamespace, TEXT("add_source"),
		TEXT("Create a new ini tag source (Config/Tags/<name>.ini) that add and move can then target. The file itself is written with the first tag added to it."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameplayTagsWriteActions::AddSource),
		FParamSchemaBuilder()
			.Required(TEXT("name"), TEXT("string"), TEXT("Source name, with or without .ini, e.g. \"AudioTags\""))
			.Build());

	Registry.RegisterAction(TagsNamespace, TEXT("add"),
		TEXT("Declare one or more gameplay tags in an ini source, as the Add New Gameplay Tag button does. Parents are implied by the dotted name and need no declaration. A tag that already exists explicitly is reported, not duplicated; adding an implicit parent makes it explicit. Every name is validated first and an invalid one comes back with the engine's reason and a suggested fix. The ini is checked out through source control when that is enabled."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameplayTagsWriteActions::Add),
		FParamSchemaBuilder()
			.Required(TEXT("tags"), TEXT("array"), TEXT("Tag names, e.g. [\"Audio.Ambience.Wind\"]. A single string is accepted too."))
			.Optional(TEXT("comment"), TEXT("string"), TEXT("Dev comment written to every tag added by this call"))
			.Optional(TEXT("source"), TEXT("string"), TEXT("Ini source to declare in, e.g. \"DefaultGameplayTags.ini\" or \"AudioTags\" (see list_sources). Default: the project default, or the developer tag file when Project Settings sets one."))
			.Optional(TEXT("restricted"), TEXT("boolean"), TEXT("Declare restricted tags. Needs a restricted source. Default false."))
			.Optional(TEXT("allow_non_restricted_children"), TEXT("boolean"), TEXT("For restricted tags: whether ordinary tags may sit below them. Default true."))
			.Build());

	Registry.RegisterAction(TagsNamespace, TEXT("update"),
		TEXT("Change the dev comment of an ini-declared tag, and for a restricted tag whether ordinary tags may sit below it. Native and DataTable tags are refused with the place to edit them instead."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameplayTagsWriteActions::Update),
		FParamSchemaBuilder()
			.Required(TEXT("tag"), TEXT("string"), TEXT("Full tag name"))
			.Optional(TEXT("comment"), TEXT("string"), TEXT("New dev comment; an empty string clears it"))
			.Optional(TEXT("allow_non_restricted_children"), TEXT("boolean"), TEXT("Restricted tags only"))
			.Build());

	Registry.RegisterAction(TagsNamespace, TEXT("remove"),
		TEXT("Delete ini-declared tags. A tag is refused while anything still uses it: an asset (Asset Registry SearchableName graph, the check the editor itself makes), a C++ string literal or a (TagName=\"...\") config setting in the project. Implicit parents that would be left without children disappear with it and are checked the same way. A tag with children of its own stays as an implicit parent. dry_run reports the verdict without writing."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameplayTagsWriteActions::Remove),
		FParamSchemaBuilder()
			.Required(TEXT("tags"), TEXT("array"), TEXT("Tag names to delete. A single string is accepted too."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Report what would happen without writing. Default false."))
			.Build());

	Registry.RegisterAction(TagsNamespace, TEXT("rename"),
		TEXT("Rename an ini-declared tag, by default with its whole subtree, as Rename does in Project Settings. The engine declares the new name, deletes the old one and writes a GameplayTagRedirect old->new; assets are NOT rewritten - they keep the old name on disk and load as the new one through the redirect until they are resaved. The response lists those assets, plus C++ literals and config settings that still spell the old name, which no redirect fixes in text. The editor asks for a restart for redirects to take full effect. dry_run reports the plan without writing."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameplayTagsWriteActions::Rename),
		FParamSchemaBuilder()
			.Required(TEXT("old_tag"), TEXT("string"), TEXT("Current full tag name"))
			.Required(TEXT("new_tag"), TEXT("string"), TEXT("New full tag name; must not exist explicitly yet"))
			.Optional(TEXT("rename_children"), TEXT("boolean"), TEXT("Rename every declared tag below old_tag too. Default true."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Report what would happen without writing. Default false."))
			.Build());

	Registry.RegisterAction(TagsNamespace, TEXT("move"),
		TEXT("Move ini-declared tags to another ini source, keeping their dev comments. Restricted, native and DataTable tags cannot be moved."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameplayTagsWriteActions::Move),
		FParamSchemaBuilder()
			.Required(TEXT("tags"), TEXT("array"), TEXT("Tag names to move. A single string is accepted too."))
			.Required(TEXT("target_source"), TEXT("string"), TEXT("Destination ini source, e.g. \"AudioTags.ini\" (see list_sources)"))
			.Build());
}

// ============================================================================
// Handlers
// ============================================================================

FMonolithActionResult FMonolithGameplayTagsWriteActions::ListSources(const TSharedPtr<FJsonObject>& Params)
{
	const UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();

	const EGameplayTagSourceType Types[] = {
		EGameplayTagSourceType::DefaultTagList,
		EGameplayTagSourceType::TagList,
		EGameplayTagSourceType::RestrictedTagList,
		EGameplayTagSourceType::DataTable,
		EGameplayTagSourceType::Native
	};

	TArray<TSharedPtr<FJsonValue>> Rows;
	TArray<const FGameplayTagSource*> Sources;
	for (EGameplayTagSourceType Type : Types)
	{
		Sources.Reset();
		TagManager.FindTagSourcesWithType(Type, Sources);
		for (const FGameplayTagSource* Source : Sources)
		{
			if (!Source)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Source->SourceName.ToString());
			Row->SetStringField(TEXT("type"), SourceTypeName(Source->SourceType));

			const FString ConfigFile = Source->GetConfigFileName();
			Row->SetStringField(TEXT("config_file"), ConfigFile.IsEmpty() ? FString() : ToProjectRelative(FPaths::ConvertRelativePathToFull(ConfigFile)));

			int32 TagCount = 0;
			if (Source->SourceTagList)
			{
				TagCount = Source->SourceTagList->GameplayTagList.Num();
			}
			else if (Source->SourceRestrictedTagList)
			{
				TagCount = Source->SourceRestrictedTagList->RestrictedGameplayTagList.Num();
			}
			const bool bIni = IsIniSource(Source) || (Source->SourceType == EGameplayTagSourceType::RestrictedTagList && Source->SourceRestrictedTagList);
			if (bIni)
			{
				Row->SetNumberField(TEXT("tag_count"), TagCount);
			}
			Row->SetBoolField(TEXT("writable"), bIni);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("import_tags_from_config"), TagManager.ShouldImportTagsFromINI());
	Result->SetNumberField(TEXT("total"), Rows.Num());
	Result->SetArrayField(TEXT("sources"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayTagsWriteActions::AddSource(const TSharedPtr<FJsonObject>& Params)
{
	FString RawName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), RawName) || RawName.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: name"));
	}

	const FName SourceName = NormaliseSourceName(RawName);
	const FString SourceString = SourceName.ToString();
	if (SourceString.Contains(TEXT("/")) || SourceString.Contains(TEXT("\\")))
	{
		return FMonolithActionResult::Error(TEXT("name is a file name, not a path: the source is created under Config/Tags"));
	}

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
	if (const FGameplayTagSource* Existing = TagManager.FindTagSource(SourceName))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Tag source %s already exists (%s, %s)"),
			*SourceString, SourceTypeName(Existing->SourceType), *Existing->GetConfigFileName()));
	}

	if (!IGameplayTagsEditorModule::Get().AddNewGameplayTagSource(SourceString))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("The engine refused to add tag source %s; see LogGameplayTags in the output log"), *SourceString));
	}

	const FGameplayTagSource* Created = TagManager.FindTagSource(SourceName);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), SourceString);
	Result->SetStringField(TEXT("config_file"), Created ? ToProjectRelative(FPaths::ConvertRelativePathToFull(Created->GetConfigFileName())) : FString());
	Result->SetBoolField(TEXT("file_written"), false);
	Result->SetStringField(TEXT("note"), TEXT("The ini file is written with the first tag added to this source."));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayTagsWriteActions::Add(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> RequestedTags;
	FString ParamError;
	if (!ReadTagList(Params, TEXT("tags"), RequestedTags, ParamError))
	{
		return FMonolithActionResult::Error(ParamError);
	}

	FString Comment;
	FString RequestedSource;
	Params->TryGetStringField(TEXT("comment"), Comment);
	Params->TryGetStringField(TEXT("source"), RequestedSource);
	const bool bRestricted = ReadBool(Params, TEXT("restricted"), false);
	const bool bAllowNonRestrictedChildren = ReadBool(Params, TEXT("allow_non_restricted_children"), true);

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
	if (!TagManager.ShouldImportTagsFromINI())
	{
		return FMonolithActionResult::Error(TEXT("Import Tags From Config is off in Project Settings > Gameplay Tags, so ini tags are not loaded and none can be added"));
	}

	// The engine would quietly create a missing source and quietly fall back to
	// an ordinary list for a restricted tag, so the source is settled here.
	FName SourceName = NAME_None;
	if (!RequestedSource.TrimStartAndEnd().IsEmpty())
	{
		SourceName = NormaliseSourceName(RequestedSource);
		const FGameplayTagSource* Source = TagManager.FindTagSource(SourceName);
		if (!Source)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("No tag source named %s. Create it with add_source, or pick one from list_sources"), *SourceName.ToString()));
		}
		if (bRestricted && !Source->SourceRestrictedTagList)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("%s is not a restricted tag source; restricted tags need one (list_sources, type restricted_ini)"), *SourceName.ToString()));
		}
		if (!bRestricted && !IsIniSource(Source))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("%s is a %s source; ordinary tags go to an ini source (list_sources, type ini or default_ini)"), *SourceName.ToString(), SourceTypeName(Source->SourceType)));
		}
	}
	else if (bRestricted)
	{
		return FMonolithActionResult::Error(TEXT("restricted tags need an explicit restricted source; pass source (list_sources, type restricted_ini)"));
	}

	const UGameplayTagsSettings* Settings = GetDefault<UGameplayTagsSettings>();

	TArray<TSharedPtr<FJsonValue>> Rows;
	TArray<FString> ToAdd;
	TMap<FString, TSharedPtr<FJsonObject>> RowsByTag;

	for (const FString& Tag : RequestedTags)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("tag"), Tag);
		Rows.Add(MakeShared<FJsonValueObject>(Row));

		if (RowsByTag.Contains(Tag))
		{
			Row->SetStringField(TEXT("status"), TEXT("duplicate"));
			Row->SetStringField(TEXT("error"), TEXT("listed more than once in this request"));
			continue;
		}
		RowsByTag.Add(Tag, Row);

		FText ErrorText;
		FString FixedTag;
		if (!TagManager.IsValidGameplayTagString(Tag, &ErrorText, &FixedTag))
		{
			Row->SetStringField(TEXT("status"), TEXT("invalid"));
			Row->SetStringField(TEXT("error"), ErrorText.ToString());
			Row->SetStringField(TEXT("suggestion"), FixedTag);
			continue;
		}

		const FName TagName(*Tag);
		const FTagDeclaration Existing = ReadDeclaration(TagManager, TagName);
		if (Existing.bKnown && Existing.bExplicit)
		{
			Row->SetStringField(TEXT("status"), TEXT("exists"));
			Row->SetArrayField(TEXT("sources"), ToJsonStrings(Existing.Sources));
			continue;
		}

		// The restricted-hierarchy rules the engine enforces, checked here so the
		// refusal comes back with a reason. The nearest declared ancestor decides.
		FString Ancestor = Tag;
		FString RestrictionError;
		while (Ancestor.Split(TEXT("."), &Ancestor, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
		{
			if (!TagManager.IsDictionaryTag(FName(*Ancestor)))
			{
				continue;
			}
			const FTagDeclaration AncestorDeclaration = ReadDeclaration(TagManager, FName(*Ancestor));
			if (bRestricted && !AncestorDeclaration.bRestricted)
			{
				RestrictionError = FString::Printf(TEXT("a restricted tag cannot sit below %s, which is not restricted"), *Ancestor);
			}
			else if (!bRestricted && AncestorDeclaration.bRestricted && !AncestorDeclaration.bAllowNonRestrictedChildren)
			{
				RestrictionError = FString::Printf(TEXT("%s is restricted and does not allow ordinary tags below it"), *Ancestor);
			}
			break;
		}
		if (!RestrictionError.IsEmpty())
		{
			Row->SetStringField(TEXT("status"), TEXT("refused"));
			Row->SetStringField(TEXT("error"), RestrictionError);
			continue;
		}

		// Adding a tag drops a redirect away from that name; say so instead of losing it silently.
		for (const FGameplayTagRedirect& Redirect : Settings->GameplayTagRedirects)
		{
			if (Redirect.OldTagName == TagName)
			{
				Row->SetStringField(TEXT("replaces_redirect_to"), Redirect.NewTagName.ToString());
			}
		}

		if (Existing.bKnown)
		{
			Row->SetBoolField(TEXT("was_implicit"), true);
		}
		ToAdd.Add(Tag);
	}

	{
		FScopedTagTreeRefreshSuspension Suspension;
		IGameplayTagsEditorModule& TagsEditor = IGameplayTagsEditorModule::Get();
		for (const FString& Tag : ToAdd)
		{
			if (!TagsEditor.AddNewGameplayTagToINI(Tag, Comment, SourceName, bRestricted, bAllowNonRestrictedChildren))
			{
				RowsByTag[Tag]->SetStringField(TEXT("status"), TEXT("failed"));
				RowsByTag[Tag]->SetStringField(TEXT("error"), TEXT("the engine refused the tag; see LogGameplayTags in the output log"));
			}
		}
	}

	// Read back after the tree rebuild: the row reports where the tag really landed.
	int32 AddedCount = 0;
	for (const FString& Tag : ToAdd)
	{
		const TSharedPtr<FJsonObject>& Row = RowsByTag[Tag];
		if (Row->HasField(TEXT("status")))
		{
			continue;
		}
		const FTagDeclaration Added = ReadDeclaration(TagManager, FName(*Tag));
		if (Added.bKnown && Added.bExplicit)
		{
			Row->SetStringField(TEXT("status"), TEXT("added"));
			Row->SetArrayField(TEXT("sources"), ToJsonStrings(Added.Sources));
			++AddedCount;
		}
		else
		{
			Row->SetStringField(TEXT("status"), TEXT("failed"));
			Row->SetStringField(TEXT("error"), TEXT("the engine reported success but the tag is not declared after the refresh"));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("requested"), RequestedTags.Num());
	Result->SetNumberField(TEXT("added"), AddedCount);
	Result->SetArrayField(TEXT("tags"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayTagsWriteActions::Update(const TSharedPtr<FJsonObject>& Params)
{
	FString Tag;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("tag"), Tag) || Tag.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: tag"));
	}
	Tag.TrimStartAndEndInline();

	FString NewComment;
	const bool bSetComment = Params->TryGetStringField(TEXT("comment"), NewComment);
	bool bAllowNonRestrictedChildren = true;
	const bool bSetAllow = Params->TryGetBoolField(TEXT("allow_non_restricted_children"), bAllowNonRestrictedChildren);
	if (!bSetComment && !bSetAllow)
	{
		return FMonolithActionResult::Error(TEXT("Nothing to change: pass comment and/or allow_non_restricted_children"));
	}

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
	const FName TagName(*Tag);
	const FTagDeclaration Declaration = ReadDeclaration(TagManager, TagName);
	if (!Declaration.bKnown)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown tag: %s"), *Tag));
	}
	if (!Declaration.bExplicit)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("%s is implicit - it exists only because tags below it do - so there is no declaration to edit; add it first"), *Tag));
	}
	const FString NotEditable = WhyNotIniEditable(TagManager, Declaration);
	if (!NotEditable.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("%s is %s"), *Tag, *NotEditable));
	}
	if (bSetAllow && !Declaration.bRestricted)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("allow_non_restricted_children applies to restricted tags only, and %s is not restricted"), *Tag));
	}
	if (bSetAllow && Declaration.bAllowNonRestrictedChildren && !bAllowNonRestrictedChildren)
	{
		const FGameplayTagContainer Children = TagManager.RequestGameplayTagDirectDescendantsInDictionary(
			TagManager.RequestGameplayTag(TagName), EGameplayTagSelectionType::NonRestrictedOnly);
		if (!Children.IsEmpty())
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("%s already has ordinary tags below it (%s); remove or move them first"), *Tag, *Children.ToStringSimple()));
		}
	}

	// UpdateTagInINI never touches the comment, so the declaring rows are edited
	// here and saved the way the tag editor module saves its own edits.
	TMap<UObject*, FString> ListsToSave;
	for (FName SourceName : Declaration.Sources)
	{
		const FGameplayTagSource* Source = TagManager.FindTagSource(SourceName);
		if (!Source)
		{
			continue;
		}
		if (Declaration.bRestricted && Source->SourceRestrictedTagList)
		{
			for (FRestrictedGameplayTagTableRow& Row : Source->SourceRestrictedTagList->RestrictedGameplayTagList)
			{
				if (Row.Tag == TagName)
				{
					if (bSetComment)
					{
						Row.DevComment = NewComment;
					}
					if (bSetAllow)
					{
						Row.bAllowNonRestrictedChildren = bAllowNonRestrictedChildren;
					}
					ListsToSave.Add(Source->SourceRestrictedTagList, Source->SourceRestrictedTagList->ConfigFileName);
				}
			}
		}
		else if (!Declaration.bRestricted && Source->SourceTagList)
		{
			for (FGameplayTagTableRow& Row : Source->SourceTagList->GameplayTagList)
			{
				if (Row.Tag == TagName && bSetComment)
				{
					Row.DevComment = NewComment;
					ListsToSave.Add(Source->SourceTagList, Source->SourceTagList->ConfigFileName);
				}
			}
		}
	}

	if (ListsToSave.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No ini row declaring %s was found in its sources (%s)"), *Tag, *FString::JoinBy(Declaration.Sources, TEXT(", "), [](FName Name) { return Name.ToString(); })));
	}

	TArray<FString> SavedFiles;
	for (const TPair<UObject*, FString>& Entry : ListsToSave)
	{
		PrepareConfigForWrite(Entry.Value);
		Entry.Key->TryUpdateDefaultConfigFile(Entry.Value);
		GConfig->LoadFile(Entry.Value);
		SavedFiles.Add(ToProjectRelative(FPaths::ConvertRelativePathToFull(Entry.Value)));
	}
	TagManager.EditorRefreshGameplayTagTree();

	const FTagDeclaration After = ReadDeclaration(TagManager, TagName);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("tag"), Tag);
	Result->SetStringField(TEXT("old_comment"), Declaration.Comment);
	Result->SetStringField(TEXT("comment"), After.Comment);
	if (Declaration.bRestricted)
	{
		Result->SetBoolField(TEXT("allow_non_restricted_children"), After.bAllowNonRestrictedChildren);
	}
	Result->SetArrayField(TEXT("files"), ToJsonStrings(SavedFiles));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayTagsWriteActions::Remove(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> RequestedTags;
	FString ParamError;
	if (!ReadTagList(Params, TEXT("tags"), RequestedTags, ParamError))
	{
		return FMonolithActionResult::Error(ParamError);
	}
	const bool bDryRun = ReadBool(Params, TEXT("dry_run"), false);

	IAssetRegistry* Registry = IAssetRegistry::Get();
	if (!Registry)
	{
		return FMonolithActionResult::Error(TEXT("Asset Registry is not available"));
	}
	if (Registry->IsLoadingAssets())
	{
		return FMonolithActionResult::Error(TEXT("The Asset Registry is still loading; reference checks would be incomplete. Retry when it has finished"));
	}

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();

	FProjectTagEvidence Evidence;
	CollectProjectTagEvidence(Evidence);

	// Children before parents, so removing a parent and its child in one call
	// sees the parent left childless.
	RequestedTags.Sort([](const FString& A, const FString& B)
	{
		int32 DotsA = 0;
		int32 DotsB = 0;
		for (TCHAR Char : A) { DotsA += Char == TEXT('.'); }
		for (TCHAR Char : B) { DotsB += Char == TEXT('.'); }
		return DotsA != DotsB ? DotsA > DotsB : A < B;
	});
	const TSet<FString> RequestedSet(RequestedTags);

	TArray<TSharedPtr<FJsonValue>> Rows;
	TArray<FName> ToRemove;
	TMap<FName, TSharedPtr<FJsonObject>> RowsByTag;

	for (const FString& Tag : RequestedTags)
	{
		const FName TagName(*Tag);
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("tag"), Tag);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
		if (RowsByTag.Contains(TagName))
		{
			Row->SetStringField(TEXT("status"), TEXT("duplicate"));
			continue;
		}
		RowsByTag.Add(TagName, Row);

		const FTagDeclaration Declaration = ReadDeclaration(TagManager, TagName);
		if (!Declaration.bKnown)
		{
			Row->SetStringField(TEXT("status"), TEXT("refused"));
			Row->SetStringField(TEXT("error"), TEXT("unknown tag"));
			continue;
		}
		if (!Declaration.bExplicit)
		{
			Row->SetStringField(TEXT("status"), TEXT("refused"));
			Row->SetStringField(TEXT("error"), TEXT("implicit tag: it exists only because tags below it do; remove those instead"));
			continue;
		}
		const FString NotEditable = WhyNotIniEditable(TagManager, Declaration);
		if (!NotEditable.IsEmpty())
		{
			Row->SetStringField(TEXT("status"), TEXT("refused"));
			Row->SetStringField(TEXT("error"), NotEditable);
			continue;
		}

		const FGameplayTag TagHandle = TagManager.RequestGameplayTag(TagName, /*ErrorIfNotFound=*/false);
		const FGameplayTagContainer Children = TagManager.RequestGameplayTagChildrenInDictionary(TagHandle);
		bool bChildrenStay = false;
		for (const FGameplayTag& Child : Children)
		{
			bChildrenStay |= !RequestedSet.Contains(Child.ToString());
		}

		// What actually stops existing: the tag itself unless children keep it as
		// an implicit parent, and implicit ancestors this tag was the last child
		// of - the same chain the engine walks.
		TArray<FName> Vanishing;
		if (!bChildrenStay)
		{
			Vanishing.Add(TagName);
		}
		FGameplayTag Parent = TagHandle.RequestDirectParent();
		while (!bChildrenStay && Parent.IsValid())
		{
			const TSharedPtr<FGameplayTagNode> ParentNode = TagManager.FindTagNode(Parent);
			if (!ParentNode.IsValid() || ParentNode->IsExplicitTag()
				|| TagManager.RequestGameplayTagChildrenInDictionary(Parent).Num() != 1)
			{
				break;
			}
			Vanishing.Add(Parent.GetTagName());
			Parent = Parent.RequestDirectParent();
		}

		// The engine refuses a tag with asset references even when children keep
		// it alive; that check is mirrored as is. Code and config only matter for
		// names that stop existing.
		TArray<FString> Reasons;
		TArray<FName> AssetCheckTags(Vanishing);
		AssetCheckTags.AddUnique(TagName);
		TArray<FName> Referencers;
		for (FName CheckTag : AssetCheckTags)
		{
			for (FName Package : GetReferencingPackages(*Registry, CheckTag))
			{
				Referencers.AddUnique(Package);
			}
		}
		if (Referencers.Num() > 0)
		{
			Reasons.Add(TEXT("referenced by assets"));
			DescribeReferencers(Referencers, Row);
		}
		for (FName VanishingTag : Vanishing)
		{
			if (const FString* File = Evidence.CppLiterals.Find(VanishingTag.ToString()))
			{
				Reasons.Add(FString::Printf(TEXT("%s is named in C++: %s"), *VanishingTag.ToString(), *ToProjectRelative(*File)));
			}
			if (const FString* File = Evidence.ConfigUses.Find(VanishingTag.ToString()))
			{
				Reasons.Add(FString::Printf(TEXT("%s is used in config: %s"), *VanishingTag.ToString(), *ToProjectRelative(*File)));
			}
		}

		Row->SetBoolField(TEXT("remains_implicit"), bChildrenStay);
		TArray<FName> AlsoRemoved(Vanishing);
		AlsoRemoved.Remove(TagName);
		if (AlsoRemoved.Num() > 0)
		{
			Row->SetArrayField(TEXT("also_removed"), ToJsonStrings(AlsoRemoved));
		}

		if (Reasons.Num() > 0)
		{
			Row->SetStringField(TEXT("status"), TEXT("blocked"));
			Row->SetArrayField(TEXT("reasons"), ToJsonStrings(Reasons));
			continue;
		}

		Row->SetStringField(TEXT("status"), bDryRun ? TEXT("would_remove") : TEXT("pending"));
		ToRemove.Add(TagName);
	}

	int32 RemovedCount = 0;
	if (!bDryRun)
	{
		IGameplayTagsEditorModule& TagsEditor = IGameplayTagsEditorModule::Get();
		for (FName TagName : ToRemove)
		{
			// Each delete rebuilds the tag tree and invalidates every node, so the
			// node is looked up fresh for each tag.
			const TSharedPtr<FGameplayTagNode> Node = TagManager.FindTagNode(TagName);
			const bool bEngineOk = Node.IsValid() && TagsEditor.DeleteTagFromINI(Node);
			const TSharedPtr<FJsonObject>& Row = RowsByTag[TagName];
			if (bEngineOk && !HasIniDeclarationRow(TagManager, TagName))
			{
				Row->SetStringField(TEXT("status"), TEXT("removed"));
				++RemovedCount;
			}
			else
			{
				Row->SetStringField(TEXT("status"), TEXT("failed"));
				Row->SetStringField(TEXT("error"), TEXT("the engine refused the delete; see LogGameplayTags in the output log"));
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetNumberField(TEXT("requested"), RequestedTags.Num());
	Result->SetNumberField(bDryRun ? TEXT("would_remove") : TEXT("removed"), bDryRun ? ToRemove.Num() : RemovedCount);
	Result->SetArrayField(TEXT("tags"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayTagsWriteActions::Rename(const TSharedPtr<FJsonObject>& Params)
{
	FString OldTag;
	FString NewTag;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("old_tag"), OldTag) || OldTag.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: old_tag"));
	}
	if (!Params->TryGetStringField(TEXT("new_tag"), NewTag) || NewTag.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: new_tag"));
	}
	OldTag.TrimStartAndEndInline();
	NewTag.TrimStartAndEndInline();
	const bool bRenameChildren = ReadBool(Params, TEXT("rename_children"), true);
	const bool bDryRun = ReadBool(Params, TEXT("dry_run"), false);

	IAssetRegistry* Registry = IAssetRegistry::Get();
	if (!Registry)
	{
		return FMonolithActionResult::Error(TEXT("Asset Registry is not available"));
	}

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
	const FName OldName(*OldTag);
	const FTagDeclaration OldDeclaration = ReadDeclaration(TagManager, OldName);
	if (!OldDeclaration.bKnown)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown tag: %s"), *OldTag));
	}
	if (!OldDeclaration.bExplicit)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("%s is implicit; rename the declared tags below it instead"), *OldTag));
	}
	if (OldDeclaration.bRestricted)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("%s is restricted; the engine's rename only rewrites ordinary ini lists and would leave the restricted declaration behind"), *OldTag));
	}
	const FString NotEditable = WhyNotIniEditable(TagManager, OldDeclaration);
	if (!NotEditable.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("%s is %s"), *OldTag, *NotEditable));
	}

	FText ErrorText;
	FString FixedTag;
	if (!TagManager.IsValidGameplayTagString(NewTag, &ErrorText, &FixedTag))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("new_tag is not a valid tag name: %s (suggestion: %s)"), *ErrorText.ToString(), *FixedTag));
	}
	if (NewTag.Equals(OldTag, ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(TEXT("new_tag equals old_tag (tag names are case-insensitive)"));
	}
	if (NewTag.StartsWith(OldTag + TEXT("."), ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(TEXT("new_tag cannot sit below old_tag: the subtree would be renamed into itself"));
	}

	// Old name -> new name for every tag the engine will touch.
	TArray<TPair<FName, FString>> Plan;
	Plan.Emplace(OldName, NewTag);
	if (bRenameChildren)
	{
		const FGameplayTagContainer Descendants = TagManager.RequestGameplayTagChildrenInDictionary(TagManager.RequestGameplayTag(OldName));
		for (const FGameplayTag& Descendant : Descendants)
		{
			const FString DescendantString = Descendant.ToString();
			Plan.Emplace(Descendant.GetTagName(), NewTag + DescendantString.RightChop(OldTag.Len()));
		}
	}

	TArray<FString> Blockers;
	for (const TPair<FName, FString>& Step : Plan)
	{
		const FTagDeclaration From = ReadDeclaration(TagManager, Step.Key);
		const FString FromNotEditable = WhyNotIniEditable(TagManager, From);
		if (From.bExplicit && !FromNotEditable.IsEmpty())
		{
			Blockers.Add(FString::Printf(TEXT("%s is %s"), *Step.Key.ToString(), *FromNotEditable));
		}
		if (From.bExplicit && From.bRestricted)
		{
			Blockers.Add(FString::Printf(TEXT("%s is restricted"), *Step.Key.ToString()));
		}
		const FTagDeclaration To = ReadDeclaration(TagManager, FName(*Step.Value));
		if (To.bKnown && To.bExplicit)
		{
			Blockers.Add(FString::Printf(TEXT("%s already exists; renaming onto it would merge two tags"), *Step.Value));
		}
	}
	if (Blockers.Num() > 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Rename refused: %s"), *FString::Join(Blockers, TEXT("; "))));
	}

	FProjectTagEvidence Evidence;
	CollectProjectTagEvidence(Evidence);

	TArray<TSharedPtr<FJsonValue>> PlanRows;
	for (const TPair<FName, FString>& Step : Plan)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("old_tag"), Step.Key.ToString());
		Row->SetStringField(TEXT("new_tag"), Step.Value);
		Row->SetBoolField(TEXT("explicit"), ReadDeclaration(TagManager, Step.Key).bExplicit);
		DescribeReferencers(GetReferencingPackages(*Registry, Step.Key), Row);
		if (const FString* File = Evidence.CppLiterals.Find(Step.Key.ToString()))
		{
			Row->SetStringField(TEXT("cpp_literal"), ToProjectRelative(*File));
		}
		if (const FString* File = Evidence.ConfigUses.Find(Step.Key.ToString()))
		{
			Row->SetStringField(TEXT("config_use"), ToProjectRelative(*File));
		}
		PlanRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetStringField(TEXT("old_tag"), OldTag);
	Result->SetStringField(TEXT("new_tag"), NewTag);
	Result->SetArrayField(TEXT("renames"), PlanRows);
	Result->SetStringField(TEXT("note"), TEXT("Assets keep the old name on disk and load as the new one through the redirect until resaved. C++ literals and config settings listed here are text and still spell the old name."));

	if (bDryRun)
	{
		return FMonolithActionResult::Success(Result);
	}

	// What each explicitly declared tag looked like before, to check the result
	// against: the engine renames children first, so by the time it reaches a
	// parent the new parent name already exists implicitly (implied by the
	// renamed children) and it skips declaring it. The parent would silently lose
	// its declaration and its dev comment.
	struct FExplicitStep
	{
		FName OldName;
		FString NewName;
		FString Comment;
		FName Source;
	};
	TArray<FExplicitStep> ExplicitSteps;
	for (const TPair<FName, FString>& Step : Plan)
	{
		const FTagDeclaration From = ReadDeclaration(TagManager, Step.Key);
		if (From.bExplicit)
		{
			FName IniSource = NAME_None;
			for (FName SourceName : From.Sources)
			{
				if (IsIniSource(TagManager.FindTagSource(SourceName)))
				{
					IniSource = SourceName;
					break;
				}
			}
			ExplicitSteps.Add({ Step.Key, Step.Value, From.Comment, IniSource });
		}
	}

	IGameplayTagsEditorModule& TagsEditor = IGameplayTagsEditorModule::Get();
	if (!TagsEditor.RenameTagInINI(OldTag, NewTag, bRenameChildren))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("The engine refused to rename %s; see LogGameplayTags in the output log"), *OldTag));
	}

	TArray<FString> Redeclared;
	{
		FScopedTagTreeRefreshSuspension Suspension;
		for (const FExplicitStep& Step : ExplicitSteps)
		{
			const FTagDeclaration To = ReadDeclaration(TagManager, FName(*Step.NewName));
			if (!(To.bKnown && To.bExplicit)
				&& TagsEditor.AddNewGameplayTagToINI(Step.NewName, Step.Comment, Step.Source, /*bIsRestrictedTag=*/false, /*bAllowNonRestrictedChildren=*/true))
			{
				Redeclared.Add(Step.NewName);
			}
		}
	}

	TArray<FString> NotDeclared;
	TArray<FString> NotRemoved;
	for (const FExplicitStep& Step : ExplicitSteps)
	{
		const FTagDeclaration To = ReadDeclaration(TagManager, FName(*Step.NewName));
		if (!(To.bKnown && To.bExplicit))
		{
			NotDeclared.Add(Step.NewName);
		}
		if (HasIniDeclarationRow(TagManager, Step.OldName))
		{
			NotRemoved.Add(Step.OldName.ToString());
		}
	}

	const FTagDeclaration After = ReadDeclaration(TagManager, FName(*NewTag));
	Result->SetBoolField(TEXT("new_declared"), NotDeclared.Num() == 0);
	Result->SetBoolField(TEXT("old_removed"), NotRemoved.Num() == 0);
	if (NotDeclared.Num() > 0)
	{
		Result->SetArrayField(TEXT("not_declared"), ToJsonStrings(NotDeclared));
	}
	if (NotRemoved.Num() > 0)
	{
		Result->SetArrayField(TEXT("not_removed"), ToJsonStrings(NotRemoved));
	}
	if (Redeclared.Num() > 0)
	{
		Result->SetArrayField(TEXT("redeclared"), ToJsonStrings(Redeclared));
	}
	Result->SetArrayField(TEXT("sources"), ToJsonStrings(After.Sources));
	Result->SetBoolField(TEXT("restart_recommended"), true);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameplayTagsWriteActions::Move(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> RequestedTags;
	FString ParamError;
	if (!ReadTagList(Params, TEXT("tags"), RequestedTags, ParamError))
	{
		return FMonolithActionResult::Error(ParamError);
	}
	FString RawTarget;
	if (!Params->TryGetStringField(TEXT("target_source"), RawTarget) || RawTarget.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: target_source"));
	}

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
	const FName TargetName = NormaliseSourceName(RawTarget);
	const FGameplayTagSource* Target = TagManager.FindTagSource(TargetName);
	if (!Target)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No tag source named %s. Create it with add_source, or pick one from list_sources"), *TargetName.ToString()));
	}
	if (!IsIniSource(Target))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("%s is a %s source; tags can only be moved to an ini source (type ini or default_ini)"), *TargetName.ToString(), SourceTypeName(Target->SourceType)));
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	TArray<FString> ToMove;
	TMap<FString, TSharedPtr<FJsonObject>> RowsByTag;
	for (const FString& Tag : RequestedTags)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("tag"), Tag);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
		if (RowsByTag.Contains(Tag))
		{
			Row->SetStringField(TEXT("status"), TEXT("duplicate"));
			continue;
		}
		RowsByTag.Add(Tag, Row);

		const FTagDeclaration Declaration = ReadDeclaration(TagManager, FName(*Tag));
		FString Error;
		if (!Declaration.bKnown)
		{
			Error = TEXT("unknown tag");
		}
		else if (!Declaration.bExplicit)
		{
			Error = TEXT("implicit tag: nothing declares it, so there is nothing to move");
		}
		else if (Declaration.bRestricted)
		{
			Error = TEXT("restricted tags cannot be moved");
		}
		else if (!WhyNotIniEditable(TagManager, Declaration).IsEmpty())
		{
			Error = WhyNotIniEditable(TagManager, Declaration);
		}
		else if (Declaration.Sources.Contains(TargetName))
		{
			Error = FString::Printf(TEXT("already declared in %s"), *TargetName.ToString());
		}
		if (!Error.IsEmpty())
		{
			Row->SetStringField(TEXT("status"), TEXT("refused"));
			Row->SetStringField(TEXT("error"), Error);
			continue;
		}
		Row->SetArrayField(TEXT("from"), ToJsonStrings(Declaration.Sources));
		ToMove.Add(Tag);
	}

	TArray<FString> Moved;
	TArray<FString> Failed;
	if (ToMove.Num() > 0)
	{
		IGameplayTagsEditorModule::Get().MoveTagsBetweenINI(ToMove, TargetName, Moved, Failed);
	}
	for (const FString& Tag : ToMove)
	{
		const TSharedPtr<FJsonObject>& Row = RowsByTag[Tag];
		if (Moved.Contains(Tag))
		{
			Row->SetStringField(TEXT("status"), TEXT("moved"));
		}
		else
		{
			Row->SetStringField(TEXT("status"), TEXT("failed"));
			Row->SetStringField(TEXT("error"), TEXT("the engine refused the move; see LogGameplayTags in the output log"));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("target_source"), TargetName.ToString());
	Result->SetNumberField(TEXT("requested"), RequestedTags.Num());
	Result->SetNumberField(TEXT("moved"), Moved.Num());
	Result->SetArrayField(TEXT("tags"), Rows);
	return FMonolithActionResult::Success(Result);
}
