#include "Indexers/GameplayTagIndexer.h"
#include "MonolithSettings.h"
#include "GameplayTagsManager.h"
#include "GameplayTagContainer.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

bool FGameplayTagIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();

	// Get the root gameplay tag nodes
	FGameplayTagContainer AllTags;
	TagManager.RequestAllGameplayTags(AllTags, true);

	int32 TagsInserted = 0;

	// Walk the full tag tree via the tag manager's node structure
	TArray<TSharedPtr<FGameplayTagNode>> RootNodes;
	TagManager.GetFilteredGameplayRootTags(FString(), RootNodes);

	for (const TSharedPtr<FGameplayTagNode>& RootNode : RootNodes)
	{
		if (RootNode.IsValid())
		{
			IndexTagNode(*RootNode, DB);
			TagsInserted++;
		}
	}

	// Now scan assets for tag references
	ScanAssetTagReferences(DB);

	UE_LOG(LogMonolithIndex, Log, TEXT("GameplayTagIndexer: indexed %d root tag trees, scanned assets for references"), TagsInserted);
	return true;
}

int64 FGameplayTagIndexer::IndexTagNode(const FGameplayTagNode& Node, FMonolithIndexDatabase& DB)
{
	FString TagName = Node.GetCompleteTagString();
	if (TagName.IsEmpty())
	{
		return -1;
	}

	// Determine parent tag name
	FString ParentTag;
	TSharedPtr<FGameplayTagNode> ParentNode = Node.GetParentTagNode();
	if (ParentNode.IsValid())
	{
		ParentTag = ParentNode->GetCompleteTagString();
	}

	// Insert or get existing tag
	int64 TagId = DB.GetOrCreateTag(TagName, ParentTag);
	if (TagId < 0)
	{
		return -1;
	}

	// Recurse into children
	TArray<TSharedPtr<FGameplayTagNode>> ChildNodes = Node.GetChildTagNodes();
	for (const TSharedPtr<FGameplayTagNode>& Child : ChildNodes)
	{
		if (Child.IsValid())
		{
			IndexTagNode(*Child, DB);
		}
	}

	return TagId;
}

void FGameplayTagIndexer::ScanAssetTagReferences(FMonolithIndexDatabase& DB)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetData> AllAssets;
	FARFilter Filter;
	for (const FName& ContentPath : UMonolithSettings::GetIndexedContentPaths())
	{
		Filter.PackagePaths.Add(ContentPath);
	}
	Filter.bRecursivePaths = true;
	Registry.GetAssets(Filter, AllAssets);

	int32 RefsInserted = 0;

	// The asset registry stores gameplay tags in asset metadata under known tag value keys.
	// Common key names used by UE for GameplayTagContainers in asset registry:
	static const FName OwnedGameplayTagsKey(TEXT("OwnedGameplayTags"));
	static const FName GameplayTagsKey(TEXT("GameplayTags"));

	for (const FAssetData& Asset : AllAssets)
	{
		int64 AssetId = DB.GetAssetId(Asset.PackageName.ToString());
		if (AssetId < 0) continue;

		// Check known tag value keys in asset registry metadata
		TArray<const FName*> TagKeys = { &OwnedGameplayTagsKey, &GameplayTagsKey };

		for (const FName* KeyPtr : TagKeys)
		{
			FString TagValueString;
			if (Asset.GetTagValue(*KeyPtr, TagValueString) && !TagValueString.IsEmpty())
			{
				// Parse the tag container string - format is comma-separated tag names
				// or parenthesized format: (GameplayTags=((TagName="Tag.Name"),(TagName="Tag.Other")))
				TArray<FString> ParsedTags;

				if (TagValueString.Contains(TEXT("TagName=")))
				{
					// Structured format: extract TagName values
					FString Remaining = TagValueString;
					FString TagToken = TEXT("TagName=\"");
					int32 Pos = 0;
					while ((Pos = Remaining.Find(TagToken, ESearchCase::CaseSensitive)) != INDEX_NONE)
					{
						Remaining.RightChopInline(Pos + TagToken.Len());
						int32 EndQuote = Remaining.Find(TEXT("\""), ESearchCase::CaseSensitive);
						if (EndQuote != INDEX_NONE)
						{
							ParsedTags.Add(Remaining.Left(EndQuote));
							Remaining.RightChopInline(EndQuote + 1);
						}
					}
				}
				else
				{
					// Simple comma-separated format
					TagValueString.ParseIntoArray(ParsedTags, TEXT(","));
					for (FString& Tag : ParsedTags)
					{
						Tag.TrimStartAndEndInline();
					}
				}

				// Insert references for each found tag
				FString Context = KeyPtr->ToString();
				for (const FString& TagName : ParsedTags)
				{
					if (TagName.IsEmpty()) continue;

					int64 TagId = DB.GetOrCreateTag(TagName, FString());
					if (TagId < 0) continue;

					FIndexedTagReference Ref;
					Ref.TagId = TagId;
					Ref.AssetId = AssetId;
					Ref.Context = Context;
					DB.InsertTagReference(Ref);
					RefsInserted++;
				}
			}
		}

		// Also scan all asset registry tag values for any that look like gameplay tags
		// (custom properties that store FGameplayTag or FGameplayTagContainer)
		Asset.EnumerateTags([&](TPair<FName, FAssetTagValueRef> TagPair)
		{
			// Skip the keys we already processed
			if (TagPair.Key == OwnedGameplayTagsKey || TagPair.Key == GameplayTagsKey)
			{
				return;
			}

			FString Value = TagPair.Value.GetValue();
			// Heuristic: if value contains "TagName=" it likely has gameplay tags
			if (Value.Contains(TEXT("TagName=\"")))
			{
				FString Remaining = Value;
				FString TagToken = TEXT("TagName=\"");
				int32 Pos = 0;
				while ((Pos = Remaining.Find(TagToken, ESearchCase::CaseSensitive)) != INDEX_NONE)
				{
					Remaining.RightChopInline(Pos + TagToken.Len());
					int32 EndQuote = Remaining.Find(TEXT("\""), ESearchCase::CaseSensitive);
					if (EndQuote != INDEX_NONE)
					{
						FString TagName = Remaining.Left(EndQuote);
						Remaining.RightChopInline(EndQuote + 1);

						if (!TagName.IsEmpty())
						{
							int64 TagId = DB.GetOrCreateTag(TagName, FString());
							if (TagId < 0) return;

							FIndexedTagReference Ref;
							Ref.TagId = TagId;
							Ref.AssetId = AssetId;
							Ref.Context = TagPair.Key.ToString();
							DB.InsertTagReference(Ref);
							RefsInserted++;
						}
					}
				}
			}
		});
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("GameplayTagIndexer: inserted %d tag references across assets"), RefsInserted);
}
