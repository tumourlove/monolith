#pragma once

#include "MonolithIndexer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Indexes Gameplay Ability System assets: GameplayAbilities, GameplayEffects,
 * AttributeSets, and GameplayCues. Extracts tag containers, modifiers,
 * attributes, and builds cross-references between them.
 * Uses sentinel class "__GAS__" for dispatch.
 */
class FGASIndexer : public IMonolithIndexer
{
public:
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("__GAS__") };
	}

	virtual bool IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual FString GetName() const override { return TEXT("GASIndexer"); }
	virtual bool IsSentinel() const override { return true; }

private:
	/** Index a single GameplayAbility Blueprint CDO */
	int64 IndexAbility(UObject* CDO, const FString& AssetPath, FMonolithIndexDatabase& DB, int64 AssetId);

	/** Index a single GameplayEffect Blueprint CDO */
	int64 IndexEffect(UObject* CDO, const FString& AssetPath, FMonolithIndexDatabase& DB, int64 AssetId);

	/** Index an AttributeSet class (Blueprint or native) */
	int64 IndexAttributeSet(UClass* Class, FMonolithIndexDatabase& DB, int64 AssetId);

	/** Index a GameplayCue notify (Static or Actor) */
	int64 IndexGameplayCue(UObject* CDO, const FString& AssetPath, FMonolithIndexDatabase& DB, int64 AssetId, bool bIsActor);

	/** Extract FGameplayTagContainer as JSON array of tag strings via reflection */
	TArray<TSharedPtr<FJsonValue>> ExtractTagContainer(UObject* CDO, const FString& PropertyName);

	/** Serialize a JSON object to a condensed string */
	static FString JsonToString(TSharedPtr<FJsonObject> JsonObj);

	// Cross-reference data collected during indexing
	struct FEffectRef
	{
		int64 NodeId;
		TArray<FString> ModifiedAttributes; // "AttributeSetClass.AttributeName"
		TArray<FString> CueTags;
	};

	struct FAbilityRef
	{
		int64 NodeId;
		FString CostEffectPath;
		FString CooldownEffectPath;
	};

	struct FAttributeSetRef
	{
		int64 NodeId;
		FString ClassName;
		TArray<FString> AttributeNames; // "ClassName.AttrName"
	};

	struct FCueRef
	{
		int64 NodeId;
		FString CueTag;
	};
};
