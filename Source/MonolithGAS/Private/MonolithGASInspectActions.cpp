#include "MonolithGASInspectActions.h"
#include "MonolithParamSchema.h"
#include "MonolithGASInternal.h"

#include "Abilities/GameplayAbility.h"
#include "AttributeSet.h"
#include "GameplayEffect.h"
#include "GameplayEffectComponent.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "AbilitySystemComponent.h"
#include "GameplayCueNotify_Static.h"
#include "GameplayCueNotify_Actor.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonWriter.h"
#include "EngineUtils.h"


// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

void FMonolithGASInspectActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("gas"), TEXT("export_gas_manifest"),
		TEXT("Export full GAS architecture as JSON: all abilities, effects, attribute sets, ASCs, cues, tags, and their relationships"),
		FMonolithActionHandler::CreateStatic(&HandleExportGASManifest),
		FParamSchemaBuilder()
			.Optional(TEXT("format"), TEXT("string"), TEXT("Output format: 'json' (default)"), TEXT("json"))
			.Optional(TEXT("include_relationships"), TEXT("boolean"), TEXT("Include cross-asset references (effect->attribute, ability->effect, cue->effect)"), TEXT("true"))
			.Optional(TEXT("output_path"), TEXT("string"), TEXT("File path to write manifest (default: returns inline)"))
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Restrict to assets under this path"))
			.Build());

	// Phase 4: Runtime Debug (PIE required)
	Registry.RegisterAction(TEXT("gas"), TEXT("snapshot_gas_state"),
		TEXT("Full JSON dump of all actors' ASC state in PIE: abilities, effects, attributes, tags"),
		FMonolithActionHandler::CreateStatic(&HandleSnapshotGASState),
		FParamSchemaBuilder()
			.Optional(TEXT("class_filter"), TEXT("string"), TEXT("Only include actors of this class (e.g. 'BP_PlayerCharacter')"))
			.Optional(TEXT("include_modifiers"), TEXT("boolean"), TEXT("Include active modifier breakdown per attribute"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("get_tag_state"),
		TEXT("Dump all owned, blocked, and loose gameplay tags on a live actor in PIE"),
		FMonolithActionHandler::CreateStatic(&HandleGetTagState),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor name, label, or path in PIE world"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("get_cooldown_state"),
		TEXT("List all abilities with active cooldowns and time remaining on a live actor in PIE"),
		FMonolithActionHandler::CreateStatic(&HandleGetCooldownState),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor name, label, or path in PIE world"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("trace_ability_activation"),
		TEXT("Dry-run CanActivateAbility on an actor and report each check result (tags, cost, cooldown, blocked)"),
		FMonolithActionHandler::CreateStatic(&HandleTraceAbilityActivation),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor name, label, or path in PIE world"))
			.Required(TEXT("ability_class"), TEXT("string"), TEXT("Ability class name or asset path to trace"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("compare_gas_states"),
		TEXT("Diff two snapshot JSON objects and report added/removed/changed abilities, effects, attributes, tags"),
		FMonolithActionHandler::CreateStatic(&HandleCompareGASStates),
		FParamSchemaBuilder()
			.Required(TEXT("snapshot_a"), TEXT("object"), TEXT("First snapshot (from snapshot_gas_state)"))
			.Required(TEXT("snapshot_b"), TEXT("object"), TEXT("Second snapshot (from snapshot_gas_state)"))
			.Build());
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

/** Collect FGameplayTagContainer values from a CDO property */
FGameplayTagContainer GetTagContainerProp(UObject* CDO, const FString& PropName)
{
	FGameplayTagContainer Result;
	FProperty* Prop = CDO->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop) return Result;

	FStructProperty* StructProp = CastField<FStructProperty>(Prop);
	if (StructProp && StructProp->Struct == FGameplayTagContainer::StaticStruct())
	{
		FGameplayTagContainer* Container = StructProp->ContainerPtrToValuePtr<FGameplayTagContainer>(CDO);
		if (Container) Result = *Container;
	}
	return Result;
}

/** Read a soft class reference from CDO */
FString GetSoftClassProp(UObject* CDO, const FString& PropName)
{
	FProperty* Prop = CDO->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop) return TEXT("");

	if (FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
	{
		UObject* Val = ClassProp->GetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(CDO));
		if (Val) return Val->GetPathName();
	}
	if (FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop))
	{
		FSoftObjectPtr* SoftPtr = SoftClassProp->ContainerPtrToValuePtr<FSoftObjectPtr>(CDO);
		if (SoftPtr && !SoftPtr->IsNull()) return SoftPtr->ToString();
	}
	return TEXT("");
}

/** Convert tag container to JSON array */
TArray<TSharedPtr<FJsonValue>> TagsToJsonArray(const FGameplayTagContainer& Container)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FGameplayTag& Tag : Container)
	{
		Arr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}
	return Arr;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// export_gas_manifest
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASInspectActions::HandleExportGASManifest(const TSharedPtr<FJsonObject>& Params)
{
	FString Format = Params->GetStringField(TEXT("format"));
	if (Format.IsEmpty()) Format = TEXT("json");

	bool bIncludeRelationships = true;
	if (Params->HasField(TEXT("include_relationships")))
	{
		bIncludeRelationships = Params->GetBoolField(TEXT("include_relationships"));
	}

	FString OutputPath = Params->GetStringField(TEXT("output_path"));
	FString PathFilter = Params->GetStringField(TEXT("path_filter"));

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Scan all Blueprints
	TArray<FAssetData> AllBPs;
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	if (!PathFilter.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PathFilter));
		Filter.bRecursivePaths = true;
	}
	AssetRegistry.GetAssets(Filter, AllBPs);

	// ── Abilities ──
	TArray<TSharedPtr<FJsonValue>> AbilitiesArr;
	for (const FAssetData& AssetData : AllBPs)
	{
		// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
		FAssetTagValueRef AbParentTag = AssetData.TagsAndValues.FindTag(FName("ParentClass"));
		FAssetTagValueRef AbNativeParentTag = AssetData.TagsAndValues.FindTag(FName("NativeParentClass"));
		FString AbParentPath = AbParentTag.IsSet() ? AbParentTag.GetValue() : (AbNativeParentTag.IsSet() ? AbNativeParentTag.GetValue() : TEXT(""));
		if (!AbParentPath.Contains(TEXT("GameplayAbility")))
		{
			continue;
		}

		UObject* Obj = AssetData.GetAsset();
		UBlueprint* BP = Cast<UBlueprint>(Obj);
		if (!BP || !MonolithGAS::IsAbilityBlueprint(BP)) continue;

		UGameplayAbility* AbilityCDO = MonolithGAS::GetBlueprintCDO<UGameplayAbility>(BP);
		if (!AbilityCDO) continue;

		TSharedPtr<FJsonObject> AbilityObj = MakeShared<FJsonObject>();
		AbilityObj->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		AbilityObj->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		AbilityObj->SetStringField(TEXT("parent_class"), AbilityCDO->GetClass()->GetSuperClass()->GetName());

		// Tags
		FGameplayTagContainer AbilityTags = GetTagContainerProp(AbilityCDO, TEXT("AbilityTags"));
		if (AbilityTags.Num() > 0)
		{
			AbilityObj->SetArrayField(TEXT("ability_tags"), TagsToJsonArray(AbilityTags));
		}

		FGameplayTagContainer CancelTags = GetTagContainerProp(AbilityCDO, TEXT("CancelAbilitiesWithTag"));
		if (CancelTags.Num() > 0)
		{
			AbilityObj->SetArrayField(TEXT("cancel_abilities_with_tag"), TagsToJsonArray(CancelTags));
		}

		FGameplayTagContainer BlockTags = GetTagContainerProp(AbilityCDO, TEXT("BlockAbilitiesWithTag"));
		if (BlockTags.Num() > 0)
		{
			AbilityObj->SetArrayField(TEXT("block_abilities_with_tag"), TagsToJsonArray(BlockTags));
		}

		FGameplayTagContainer ActivationReq = GetTagContainerProp(AbilityCDO, TEXT("ActivationRequiredTags"));
		if (ActivationReq.Num() > 0)
		{
			AbilityObj->SetArrayField(TEXT("activation_required_tags"), TagsToJsonArray(ActivationReq));
		}

		FGameplayTagContainer ActivationBlocked = GetTagContainerProp(AbilityCDO, TEXT("ActivationBlockedTags"));
		if (ActivationBlocked.Num() > 0)
		{
			AbilityObj->SetArrayField(TEXT("activation_blocked_tags"), TagsToJsonArray(ActivationBlocked));
		}

		// Cost / Cooldown references
		if (bIncludeRelationships)
		{
			FString CostClass = GetSoftClassProp(AbilityCDO, TEXT("CostGameplayEffectClass"));
			if (!CostClass.IsEmpty()) AbilityObj->SetStringField(TEXT("cost_effect"), CostClass);

			FString CooldownClass = GetSoftClassProp(AbilityCDO, TEXT("CooldownGameplayEffectClass"));
			if (!CooldownClass.IsEmpty()) AbilityObj->SetStringField(TEXT("cooldown_effect"), CooldownClass);
		}

		AbilitiesArr.Add(MakeShared<FJsonValueObject>(AbilityObj));
	}

	// ── Gameplay Effects ──
	TArray<TSharedPtr<FJsonValue>> EffectsArr;
	TMap<FString, TArray<FString>> EffectCueTags; // for relationships
	TMap<FString, TArray<FString>> EffectAttributes; // for relationships

	for (const FAssetData& AssetData : AllBPs)
	{
		// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
		FAssetTagValueRef GEParentTag = AssetData.TagsAndValues.FindTag(FName("ParentClass"));
		FAssetTagValueRef GENativeParentTag = AssetData.TagsAndValues.FindTag(FName("NativeParentClass"));
		FString GEParentPath = GEParentTag.IsSet() ? GEParentTag.GetValue() : (GENativeParentTag.IsSet() ? GENativeParentTag.GetValue() : TEXT(""));
		if (!GEParentPath.Contains(TEXT("GameplayEffect")))
		{
			continue;
		}

		UObject* Obj = AssetData.GetAsset();
		UBlueprint* BP = Cast<UBlueprint>(Obj);
		if (!BP || !MonolithGAS::IsGameplayEffectBlueprint(BP)) continue;

		UGameplayEffect* GE = MonolithGAS::GetBlueprintCDO<UGameplayEffect>(BP);
		if (!GE) continue;

		TSharedPtr<FJsonObject> EffectObj = MakeShared<FJsonObject>();
		EffectObj->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		EffectObj->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());

		// Duration policy via reflection
		FProperty* DurProp = GE->GetClass()->FindPropertyByName(TEXT("DurationPolicy"));
		if (DurProp)
		{
			uint8* ValPtr = DurProp->ContainerPtrToValuePtr<uint8>(GE);
			if (ValPtr)
			{
				switch (*ValPtr)
				{
				case 0: EffectObj->SetStringField(TEXT("duration_policy"), TEXT("Instant")); break;
				case 1: EffectObj->SetStringField(TEXT("duration_policy"), TEXT("Infinite")); break;
				case 2: EffectObj->SetStringField(TEXT("duration_policy"), TEXT("HasDuration")); break;
				}
			}
		}

		// Modifiers
		FProperty* ModsProp = GE->GetClass()->FindPropertyByName(TEXT("Modifiers"));
		if (ModsProp)
		{
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(ModsProp);
			if (ArrayProp)
			{
				FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(GE));
				EffectObj->SetNumberField(TEXT("modifier_count"), ArrayHelper.Num());

				if (bIncludeRelationships)
				{
					TArray<FString>& Attrs = EffectAttributes.FindOrAdd(AssetData.GetObjectPathString());
					// Read attribute names from modifiers
					FStructProperty* InnerProp = CastField<FStructProperty>(ArrayProp->Inner);
					if (InnerProp)
					{
						for (int32 i = 0; i < ArrayHelper.Num(); ++i)
						{
							void* ElemPtr = ArrayHelper.GetRawPtr(i);
							FProperty* AttrProp = InnerProp->Struct->FindPropertyByName(TEXT("Attribute"));
							if (AttrProp)
							{
								FString AttrStr;
								AttrProp->ExportTextItem_Direct(AttrStr, ElemPtr, nullptr, nullptr, 0);
								if (!AttrStr.IsEmpty()) Attrs.AddUnique(AttrStr);
							}
						}
					}
				}
			}
		}

		// Cue tags
		FProperty* CuesProp = GE->GetClass()->FindPropertyByName(TEXT("GameplayCues"));
		if (CuesProp)
		{
			FInheritedTagContainer* CueTags = CuesProp->ContainerPtrToValuePtr<FInheritedTagContainer>(GE);
			if (CueTags && CueTags->Added.Num() > 0)
			{
				EffectObj->SetArrayField(TEXT("cue_tags"), TagsToJsonArray(CueTags->Added));
				if (bIncludeRelationships)
				{
					TArray<FString>& Cues = EffectCueTags.FindOrAdd(AssetData.GetObjectPathString());
					for (const FGameplayTag& Tag : CueTags->Added)
					{
						Cues.Add(Tag.ToString());
					}
				}
			}
		}

		// Stacking
		FProperty* StackProp = GE->GetClass()->FindPropertyByName(TEXT("StackingType"));
		if (StackProp)
		{
			uint8* ValPtr = StackProp->ContainerPtrToValuePtr<uint8>(GE);
			if (ValPtr && *ValPtr != 0)
			{
				switch (*ValPtr)
				{
				case 1: EffectObj->SetStringField(TEXT("stacking_type"), TEXT("AggregateBySource")); break;
				case 2: EffectObj->SetStringField(TEXT("stacking_type"), TEXT("AggregateByTarget")); break;
				}

				FProperty* LimitProp = GE->GetClass()->FindPropertyByName(TEXT("StackLimitCount"));
				if (LimitProp)
				{
					int32* LimitPtr = LimitProp->ContainerPtrToValuePtr<int32>(GE);
					if (LimitPtr) EffectObj->SetNumberField(TEXT("stack_limit"), *LimitPtr);
				}
			}
		}

		// GE Components
		TArray<UObject*> Subobjects;
		GE->GetDefaultSubobjects(Subobjects);
		if (Subobjects.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> CompArr;
			for (UObject* Sub : Subobjects)
			{
				if (Sub && Sub->GetClass()->IsChildOf(UGameplayEffectComponent::StaticClass()))
				{
					CompArr.Add(MakeShared<FJsonValueString>(Sub->GetClass()->GetName()));
				}
			}
			if (CompArr.Num() > 0)
			{
				EffectObj->SetArrayField(TEXT("ge_components"), CompArr);
			}
		}

		EffectsArr.Add(MakeShared<FJsonValueObject>(EffectObj));
	}

	// ── Attribute Sets ──
	TArray<TSharedPtr<FJsonValue>> AttributeSetsArr;
	for (const FAssetData& AssetData : AllBPs)
	{
		// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
		FAssetTagValueRef ASParentTag = AssetData.TagsAndValues.FindTag(FName("ParentClass"));
		FAssetTagValueRef ASNativeParentTag = AssetData.TagsAndValues.FindTag(FName("NativeParentClass"));
		FString ASParentPath = ASParentTag.IsSet() ? ASParentTag.GetValue() : (ASNativeParentTag.IsSet() ? ASNativeParentTag.GetValue() : TEXT(""));
		if (!ASParentPath.Contains(TEXT("AttributeSet")))
		{
			continue;
		}

		UObject* Obj = AssetData.GetAsset();
		UBlueprint* BP = Cast<UBlueprint>(Obj);
		if (!BP || !MonolithGAS::IsAttributeSetBlueprint(BP)) continue;

		UAttributeSet* SetCDO = MonolithGAS::GetBlueprintCDO<UAttributeSet>(BP);
		if (!SetCDO) continue;

		TSharedPtr<FJsonObject> SetObj = MakeShared<FJsonObject>();
		SetObj->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		SetObj->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		SetObj->SetStringField(TEXT("parent_class"), SetCDO->GetClass()->GetSuperClass()->GetName());

		// List attributes (FGameplayAttributeData properties)
		TArray<TSharedPtr<FJsonValue>> AttrsArr;
		for (TFieldIterator<FProperty> PropIt(SetCDO->GetClass()); PropIt; ++PropIt)
		{
			FStructProperty* StructProp = CastField<FStructProperty>(*PropIt);
			if (!StructProp) continue;
			// Check for FGameplayAttributeData or subclasses
			if (StructProp->Struct->GetFName().ToString().Contains(TEXT("GameplayAttributeData")))
			{
				TSharedPtr<FJsonObject> AttrObj = MakeShared<FJsonObject>();
				AttrObj->SetStringField(TEXT("name"), PropIt->GetName());
				AttrObj->SetStringField(TEXT("type"), StructProp->Struct->GetName());

				// Read default base value
				void* DataPtr = StructProp->ContainerPtrToValuePtr<void>(SetCDO);
				FProperty* BaseValProp = StructProp->Struct->FindPropertyByName(TEXT("BaseValue"));
				if (BaseValProp)
				{
					if (FFloatProperty* FloatBase = CastField<FFloatProperty>(BaseValProp))
					{
						float Val = FloatBase->GetPropertyValue(FloatBase->ContainerPtrToValuePtr<void>(DataPtr));
						AttrObj->SetNumberField(TEXT("base_value"), Val);
					}
				}

				AttrsArr.Add(MakeShared<FJsonValueObject>(AttrObj));
			}
		}
		if (AttrsArr.Num() > 0)
		{
			SetObj->SetArrayField(TEXT("attributes"), AttrsArr);
		}

		AttributeSetsArr.Add(MakeShared<FJsonValueObject>(SetObj));
	}

	// ── ASC Blueprints ──
	TArray<TSharedPtr<FJsonValue>> ASCArr;
	for (const FAssetData& AssetData : AllBPs)
	{
		// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
		FAssetTagValueRef ASCParentTag = AssetData.TagsAndValues.FindTag(FName("ParentClass"));
		FAssetTagValueRef ASCNativeParentTag = AssetData.TagsAndValues.FindTag(FName("NativeParentClass"));
		FString ASCParentPath = ASCParentTag.IsSet() ? ASCParentTag.GetValue() : (ASCNativeParentTag.IsSet() ? ASCNativeParentTag.GetValue() : TEXT(""));
		if (!ASCParentPath.Contains(TEXT("AbilitySystemComponent")))
		{
			continue;
		}

		UObject* Obj = AssetData.GetAsset();
		UBlueprint* BP = Cast<UBlueprint>(Obj);
		if (!BP || !BP->GeneratedClass) continue;

		UObject* CDO = BP->GeneratedClass->GetDefaultObject();
		if (!CDO || !CDO->GetClass()->IsChildOf(UAbilitySystemComponent::StaticClass())) continue;

		TSharedPtr<FJsonObject> ASCObj = MakeShared<FJsonObject>();
		ASCObj->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		ASCObj->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		ASCObj->SetStringField(TEXT("parent_class"), CDO->GetClass()->GetSuperClass()->GetName());

		// Replication mode
		FProperty* RepModeProp = CDO->GetClass()->FindPropertyByName(TEXT("ReplicationMode"));
		if (RepModeProp)
		{
			uint8* ValPtr = RepModeProp->ContainerPtrToValuePtr<uint8>(CDO);
			if (ValPtr)
			{
				switch (*ValPtr)
				{
				case 0: ASCObj->SetStringField(TEXT("replication_mode"), TEXT("Full")); break;
				case 1: ASCObj->SetStringField(TEXT("replication_mode"), TEXT("Mixed")); break;
				case 2: ASCObj->SetStringField(TEXT("replication_mode"), TEXT("Minimal")); break;
				}
			}
		}

		ASCArr.Add(MakeShared<FJsonValueObject>(ASCObj));
	}

	// ── GameplayCue Notifies ──
	TArray<TSharedPtr<FJsonValue>> CuesArr;
	TMap<FString, FString> CueTagToAsset; // for relationships

	for (const FAssetData& AssetData : AllBPs)
	{
		// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
		FAssetTagValueRef CueParentTag = AssetData.TagsAndValues.FindTag(FName("ParentClass"));
		FAssetTagValueRef CueNativeParentTag = AssetData.TagsAndValues.FindTag(FName("NativeParentClass"));
		FString CueParentPath = CueParentTag.IsSet() ? CueParentTag.GetValue() : (CueNativeParentTag.IsSet() ? CueNativeParentTag.GetValue() : TEXT(""));
		if (!CueParentPath.Contains(TEXT("GameplayCueNotify")))
		{
			continue;
		}

		UObject* Obj = AssetData.GetAsset();
		UBlueprint* BP = Cast<UBlueprint>(Obj);
		if (!BP || !BP->GeneratedClass) continue;

		UObject* CDO = BP->GeneratedClass->GetDefaultObject();
		if (!CDO) continue;

		bool bIsStatic = CDO->GetClass()->IsChildOf(UGameplayCueNotify_Static::StaticClass());
		bool bIsActor = CDO->GetClass()->IsChildOf(AGameplayCueNotify_Actor::StaticClass());
		if (!bIsStatic && !bIsActor) continue;

		TSharedPtr<FJsonObject> CueObj = MakeShared<FJsonObject>();
		CueObj->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		CueObj->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		CueObj->SetStringField(TEXT("type"), bIsActor ? TEXT("looping") : TEXT("burst"));

		FProperty* CueTagProp = CDO->GetClass()->FindPropertyByName(TEXT("GameplayCueTag"));
		if (CueTagProp)
		{
			FGameplayTag* TagPtr = CueTagProp->ContainerPtrToValuePtr<FGameplayTag>(CDO);
			if (TagPtr && TagPtr->IsValid())
			{
				FString TagStr = TagPtr->ToString();
				CueObj->SetStringField(TEXT("cue_tag"), TagStr);
				CueTagToAsset.Add(TagStr, AssetData.GetObjectPathString());
			}
		}

		CuesArr.Add(MakeShared<FJsonValueObject>(CueObj));
	}

	// ── Tags ──
	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
	FGameplayTagContainer AllTags;
	TagManager.RequestAllGameplayTags(AllTags, false);

	TArray<TSharedPtr<FJsonValue>> TagsArr;
	for (const FGameplayTag& Tag : AllTags)
	{
		TagsArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}

	// ── Build manifest ──
	TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();

	// Summary
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("abilities"), AbilitiesArr.Num());
	Summary->SetNumberField(TEXT("gameplay_effects"), EffectsArr.Num());
	Summary->SetNumberField(TEXT("attribute_sets"), AttributeSetsArr.Num());
	Summary->SetNumberField(TEXT("asc_blueprints"), ASCArr.Num());
	Summary->SetNumberField(TEXT("gameplay_cues"), CuesArr.Num());
	Summary->SetNumberField(TEXT("gameplay_tags"), TagsArr.Num());
	Manifest->SetObjectField(TEXT("summary"), Summary);

	// Data
	Manifest->SetArrayField(TEXT("abilities"), AbilitiesArr);
	Manifest->SetArrayField(TEXT("gameplay_effects"), EffectsArr);
	Manifest->SetArrayField(TEXT("attribute_sets"), AttributeSetsArr);
	Manifest->SetArrayField(TEXT("asc_blueprints"), ASCArr);
	Manifest->SetArrayField(TEXT("gameplay_cues"), CuesArr);
	Manifest->SetArrayField(TEXT("gameplay_tags"), TagsArr);

	// ── Relationships ──
	if (bIncludeRelationships)
	{
		TArray<TSharedPtr<FJsonValue>> RelationshipsArr;

		// Effect -> Cue relationships
		for (const auto& Pair : EffectCueTags)
		{
			for (const FString& CueTag : Pair.Value)
			{
				TSharedPtr<FJsonObject> Rel = MakeShared<FJsonObject>();
				Rel->SetStringField(TEXT("type"), TEXT("effect_triggers_cue"));
				Rel->SetStringField(TEXT("source"), Pair.Key);
				Rel->SetStringField(TEXT("target_tag"), CueTag);

				FString* CueAsset = CueTagToAsset.Find(CueTag);
				if (CueAsset)
				{
					Rel->SetStringField(TEXT("target_asset"), *CueAsset);
					Rel->SetBoolField(TEXT("handler_exists"), true);
				}
				else
				{
					Rel->SetBoolField(TEXT("handler_exists"), false);
				}

				RelationshipsArr.Add(MakeShared<FJsonValueObject>(Rel));
			}
		}

		// Effect -> Attribute relationships
		for (const auto& Pair : EffectAttributes)
		{
			for (const FString& AttrStr : Pair.Value)
			{
				TSharedPtr<FJsonObject> Rel = MakeShared<FJsonObject>();
				Rel->SetStringField(TEXT("type"), TEXT("effect_modifies_attribute"));
				Rel->SetStringField(TEXT("source"), Pair.Key);
				Rel->SetStringField(TEXT("target_attribute"), AttrStr);
				RelationshipsArr.Add(MakeShared<FJsonValueObject>(Rel));
			}
		}

		Manifest->SetArrayField(TEXT("relationships"), RelationshipsArr);
	}

	// Serialize
	FString JsonOutput;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonOutput);
	FJsonSerializer::Serialize(Manifest.ToSharedRef(), Writer);

	// Write to file or return inline
	bool bWritten = false;
	if (!OutputPath.IsEmpty())
	{
		bWritten = FFileHelper::SaveStringToFile(JsonOutput, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (!bWritten)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to write manifest to: %s"), *OutputPath));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("format"), Format);
	Result->SetObjectField(TEXT("summary"), Summary);

	if (!OutputPath.IsEmpty())
	{
		Result->SetStringField(TEXT("output_path"), OutputPath);
		Result->SetBoolField(TEXT("written"), bWritten);
		Result->SetNumberField(TEXT("file_size_bytes"), JsonOutput.Len());
	}
	else
	{
		// Return the full manifest inline
		Result->SetObjectField(TEXT("manifest"), Manifest);
	}

	if (!PathFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("path_filter"), PathFilter);
	}

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// PIE helpers moved to MonolithGAS::GetPIEWorld/FindActorInPIE/GetASCFromActor in MonolithGASInternal.cpp

namespace
{

/** Serialize a single actor's ASC state to JSON */
TSharedPtr<FJsonObject> SerializeActorASCState(AActor* Actor, UAbilitySystemComponent* ASC, bool bIncludeModifiers)
{
	TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
	ActorObj->SetStringField(TEXT("actor_name"), Actor->GetName());
	ActorObj->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	ActorObj->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
	ActorObj->SetStringField(TEXT("actor_path"), Actor->GetPathName());

	// ── Granted Abilities ──
	TArray<TSharedPtr<FJsonValue>> AbilitiesArr;
	const TArray<FGameplayAbilitySpec>& ActivatableAbilities =
		ASC->GetActivatableAbilities();
	for (const FGameplayAbilitySpec& Spec : ActivatableAbilities)
	{
		TSharedPtr<FJsonObject> AbObj = MakeShared<FJsonObject>();
		AbObj->SetStringField(TEXT("ability_class"),
			Spec.Ability ? Spec.Ability->GetClass()->GetName() : TEXT("null"));
		AbObj->SetNumberField(TEXT("level"), Spec.Level);
		AbObj->SetNumberField(TEXT("input_id"), Spec.InputID);
		AbObj->SetBoolField(TEXT("is_active"), Spec.IsActive());
		AbObj->SetStringField(TEXT("handle"), Spec.Handle.ToString());
		AbilitiesArr.Add(MakeShared<FJsonValueObject>(AbObj));
	}
	ActorObj->SetArrayField(TEXT("abilities"), AbilitiesArr);

	// ── Active Effects ──
	TArray<TSharedPtr<FJsonValue>> EffectsArr;
	const FActiveGameplayEffectsContainer& ActiveEffects = ASC->GetActiveGameplayEffects();
	for (const FActiveGameplayEffect& ActiveGE : &ActiveEffects)
	{
		TSharedPtr<FJsonObject> EfObj = MakeShared<FJsonObject>();
		if (ActiveGE.Spec.Def)
		{
			EfObj->SetStringField(TEXT("effect_class"), ActiveGE.Spec.Def->GetClass()->GetName());
		}
		EfObj->SetStringField(TEXT("handle"), ActiveGE.Handle.ToString());
		EfObj->SetNumberField(TEXT("stack_count"), ActiveGE.Spec.GetStackCount());

		float Duration = ActiveGE.GetDuration();
		float TimeRemaining = ActiveGE.GetTimeRemaining(ASC->GetWorld()->GetTimeSeconds());
		EfObj->SetNumberField(TEXT("duration"), Duration);
		EfObj->SetNumberField(TEXT("time_remaining"), TimeRemaining);

		EffectsArr.Add(MakeShared<FJsonValueObject>(EfObj));
	}
	ActorObj->SetArrayField(TEXT("active_effects"), EffectsArr);

	// ── Attributes ──
	TArray<TSharedPtr<FJsonValue>> AttrsArr;
	const TArray<UAttributeSet*>& AttributeSets = ASC->GetSpawnedAttributes();
	for (UAttributeSet* Set : AttributeSets)
	{
		if (!Set) continue;
		TSharedPtr<FJsonObject> SetObj = MakeShared<FJsonObject>();
		SetObj->SetStringField(TEXT("set_class"), Set->GetClass()->GetName());

		TArray<TSharedPtr<FJsonValue>> AttrValues;
		for (TFieldIterator<FProperty> PropIt(Set->GetClass()); PropIt; ++PropIt)
		{
			FStructProperty* StructProp = CastField<FStructProperty>(*PropIt);
			if (!StructProp) continue;
			if (!StructProp->Struct->GetFName().ToString().Contains(TEXT("GameplayAttributeData")))
				continue;

			void* DataPtr = StructProp->ContainerPtrToValuePtr<void>(Set);
			TSharedPtr<FJsonObject> AttrObj = MakeShared<FJsonObject>();
			AttrObj->SetStringField(TEXT("name"), PropIt->GetName());

			// Read BaseValue and CurrentValue via reflection
			FProperty* BaseValProp = StructProp->Struct->FindPropertyByName(TEXT("BaseValue"));
			FProperty* CurrValProp = StructProp->Struct->FindPropertyByName(TEXT("CurrentValue"));

			if (BaseValProp)
			{
				if (FFloatProperty* FP = CastField<FFloatProperty>(BaseValProp))
				{
					float Val = FP->GetPropertyValue(FP->ContainerPtrToValuePtr<void>(DataPtr));
					AttrObj->SetNumberField(TEXT("base_value"), Val);
				}
			}
			if (CurrValProp)
			{
				if (FFloatProperty* FP = CastField<FFloatProperty>(CurrValProp))
				{
					float Val = FP->GetPropertyValue(FP->ContainerPtrToValuePtr<void>(DataPtr));
					AttrObj->SetNumberField(TEXT("current_value"), Val);
				}
			}

			AttrValues.Add(MakeShared<FJsonValueObject>(AttrObj));
		}
		SetObj->SetArrayField(TEXT("attributes"), AttrValues);
		AttrsArr.Add(MakeShared<FJsonValueObject>(SetObj));
	}
	ActorObj->SetArrayField(TEXT("attribute_sets"), AttrsArr);

	// ── Owned Tags ──
	FGameplayTagContainer OwnedTags;
	ASC->GetOwnedGameplayTags(OwnedTags);
	TArray<TSharedPtr<FJsonValue>> OwnedTagArr;
	for (const FGameplayTag& Tag : OwnedTags)
	{
		OwnedTagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}
	ActorObj->SetArrayField(TEXT("owned_tags"), OwnedTagArr);

	return ActorObj;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// snapshot_gas_state
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASInspectActions::HandleSnapshotGASState(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithGAS::GetPIEWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No PIE world found. Start Play-In-Editor first."));
	}

	FString ClassFilter = Params->GetStringField(TEXT("class_filter"));
	bool bIncludeModifiers = false;
	if (Params->HasField(TEXT("include_modifiers")))
	{
		bIncludeModifiers = Params->GetBoolField(TEXT("include_modifiers"));
	}

	TArray<TSharedPtr<FJsonValue>> ActorsArr;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;

		// Class filter
		if (!ClassFilter.IsEmpty())
		{
			FString ActorClassName = Actor->GetClass()->GetName();
			if (!ActorClassName.Contains(ClassFilter))
				continue;
		}

		UAbilitySystemComponent* ASC = MonolithGAS::GetASCFromActor(Actor);
		if (!ASC) continue;

		TSharedPtr<FJsonObject> ActorState = SerializeActorASCState(Actor, ASC, bIncludeModifiers);
		ActorsArr.Add(MakeShared<FJsonValueObject>(ActorState));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("actor_count"), ActorsArr.Num());
	Result->SetArrayField(TEXT("actors"), ActorsArr);
	Result->SetStringField(TEXT("world"), World->GetName());

	if (!ClassFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("class_filter"), ClassFilter);
	}

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// get_tag_state
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASInspectActions::HandleGetTagState(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorIdent;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor"), ActorIdent, Err))
		return Err;

	AActor* Actor = MonolithGAS::FindActorInPIE(ActorIdent);
	if (!Actor)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Actor '%s' not found in PIE world. Is PIE running?"), *ActorIdent));
	}

	UAbilitySystemComponent* ASC = MonolithGAS::GetASCFromActor(Actor);
	if (!ASC)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Actor '%s' has no AbilitySystemComponent."), *ActorIdent));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetName());
	Result->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());

	// Owned tags (aggregate of all sources: GEs, explicit, etc.)
	FGameplayTagContainer OwnedTags;
	ASC->GetOwnedGameplayTags(OwnedTags);
	TArray<TSharedPtr<FJsonValue>> OwnedArr;
	for (const FGameplayTag& Tag : OwnedTags)
	{
		OwnedArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}
	Result->SetArrayField(TEXT("owned_tags"), OwnedArr);
	Result->SetNumberField(TEXT("owned_tag_count"), OwnedArr.Num());

	// Blocked ability tags (tags that block abilities from activating)
	FGameplayTagContainer BlockedTags;
	ASC->GetBlockedAbilityTags(BlockedTags);
	TArray<TSharedPtr<FJsonValue>> BlockedArr;
	for (const FGameplayTag& Tag : BlockedTags)
	{
		BlockedArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}
	Result->SetArrayField(TEXT("blocked_ability_tags"), BlockedArr);
	Result->SetNumberField(TEXT("blocked_tag_count"), BlockedArr.Num());

	// Tag counts (how many sources grant each tag — useful for debugging stacking)
	TArray<TSharedPtr<FJsonValue>> TagCountsArr;
	for (const FGameplayTag& Tag : OwnedTags)
	{
		int32 Count = ASC->GetTagCount(Tag);
		TSharedPtr<FJsonObject> CountObj = MakeShared<FJsonObject>();
		CountObj->SetStringField(TEXT("tag"), Tag.ToString());
		CountObj->SetNumberField(TEXT("count"), Count);
		TagCountsArr.Add(MakeShared<FJsonValueObject>(CountObj));
	}
	Result->SetArrayField(TEXT("tag_counts"), TagCountsArr);

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// get_cooldown_state
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASInspectActions::HandleGetCooldownState(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorIdent;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor"), ActorIdent, Err))
		return Err;

	AActor* Actor = MonolithGAS::FindActorInPIE(ActorIdent);
	if (!Actor)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Actor '%s' not found in PIE world. Is PIE running?"), *ActorIdent));
	}

	UAbilitySystemComponent* ASC = MonolithGAS::GetASCFromActor(Actor);
	if (!ASC)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Actor '%s' has no AbilitySystemComponent."), *ActorIdent));
	}

	float WorldTime = ASC->GetWorld()->GetTimeSeconds();

	TArray<TSharedPtr<FJsonValue>> CooldownsArr;
	const TArray<FGameplayAbilitySpec>& Abilities = ASC->GetActivatableAbilities();

	for (const FGameplayAbilitySpec& Spec : Abilities)
	{
		if (!Spec.Ability) continue;

		// Get cooldown tags from the ability CDO
		const FGameplayTagContainer* CooldownTags = Spec.Ability->GetCooldownTags();
		if (!CooldownTags || CooldownTags->Num() == 0) continue;

		// Check if any cooldown GE is active with these tags
		float TimeRemaining = 0.f;
		float Duration = 0.f;

		FGameplayEffectQuery Query;
		Query.OwningTagQuery = FGameplayTagQuery::MakeQuery_MatchAllTags(*CooldownTags);

		TArray<TPair<float, float>> DurationsAndRemaining = ASC->GetActiveEffectsTimeRemainingAndDuration(Query);
		for (const TPair<float, float>& Pair : DurationsAndRemaining)
		{
			if (Pair.Key > TimeRemaining)
			{
				TimeRemaining = Pair.Key;
				Duration = Pair.Value;
			}
		}

		if (TimeRemaining > 0.f)
		{
			TSharedPtr<FJsonObject> CdObj = MakeShared<FJsonObject>();
			CdObj->SetStringField(TEXT("ability_class"), Spec.Ability->GetClass()->GetName());
			CdObj->SetStringField(TEXT("handle"), Spec.Handle.ToString());
			CdObj->SetNumberField(TEXT("time_remaining"), TimeRemaining);
			CdObj->SetNumberField(TEXT("duration"), Duration);

			TArray<TSharedPtr<FJsonValue>> CdTagArr;
			for (const FGameplayTag& Tag : *CooldownTags)
			{
				CdTagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
			}
			CdObj->SetArrayField(TEXT("cooldown_tags"), CdTagArr);

			CooldownsArr.Add(MakeShared<FJsonValueObject>(CdObj));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetName());
	Result->SetNumberField(TEXT("world_time"), WorldTime);
	Result->SetNumberField(TEXT("abilities_on_cooldown"), CooldownsArr.Num());
	Result->SetArrayField(TEXT("cooldowns"), CooldownsArr);

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// trace_ability_activation
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASInspectActions::HandleTraceAbilityActivation(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorIdent;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor"), ActorIdent, Err))
		return Err;

	FString AbilityClassStr;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("ability_class"), AbilityClassStr, Err))
		return Err;

	AActor* Actor = MonolithGAS::FindActorInPIE(ActorIdent);
	if (!Actor)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Actor '%s' not found in PIE world. Is PIE running?"), *ActorIdent));
	}

	UAbilitySystemComponent* ASC = MonolithGAS::GetASCFromActor(Actor);
	if (!ASC)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Actor '%s' has no AbilitySystemComponent."), *ActorIdent));
	}

	// Normalize the ability class identifier to a bare class name for matching
	FString MatchName = AbilityClassStr;
	// Strip asset path prefix: "/Game/Path/GA_Foo" -> "GA_Foo"
	if (MatchName.Contains(TEXT("/")))
	{
		MatchName = FPaths::GetBaseFilename(MatchName);
	}
	// Strip _C suffix if present
	if (MatchName.EndsWith(TEXT("_C")))
	{
		MatchName = MatchName.LeftChop(2);
	}

	// Find the ability spec
	const FGameplayAbilitySpec* FoundSpec = nullptr;
	const TArray<FGameplayAbilitySpec>& Abilities = ASC->GetActivatableAbilities();
	for (const FGameplayAbilitySpec& Spec : Abilities)
	{
		if (!Spec.Ability) continue;
		FString ClassName = Spec.Ability->GetClass()->GetName();
		// Runtime class names for Blueprint abilities end with _C
		FString BareClassName = ClassName;
		if (BareClassName.EndsWith(TEXT("_C")))
		{
			BareClassName = BareClassName.LeftChop(2);
		}
		if (BareClassName == MatchName || ClassName == MatchName || BareClassName.Contains(MatchName))
		{
			FoundSpec = &Spec;
			break;
		}
	}

	if (!FoundSpec)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Ability '%s' not found in granted abilities on actor '%s'."), *AbilityClassStr, *ActorIdent));
	}

	UGameplayAbility* AbilityCDO = FoundSpec->Ability;
	TArray<TSharedPtr<FJsonValue>> Checks;
	bool bAllPassed = true;

	// ── Check 1: Tag requirements (ActivationRequiredTags) ──
	{
		FProperty* Prop = AbilityCDO->GetClass()->FindPropertyByName(TEXT("ActivationRequiredTags"));
		if (Prop)
		{
			FGameplayTagContainer* RequiredTags = Prop->ContainerPtrToValuePtr<FGameplayTagContainer>(AbilityCDO);
			if (RequiredTags && RequiredTags->Num() > 0)
			{
				FGameplayTagContainer OwnedTags;
				ASC->GetOwnedGameplayTags(OwnedTags);
				bool bHasAll = OwnedTags.HasAll(*RequiredTags);

				TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
				Check->SetStringField(TEXT("check"), TEXT("activation_required_tags"));
				Check->SetBoolField(TEXT("passed"), bHasAll);

				TArray<TSharedPtr<FJsonValue>> ReqArr;
				for (const FGameplayTag& Tag : *RequiredTags)
				{
					ReqArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
				}
				Check->SetArrayField(TEXT("required_tags"), ReqArr);

				if (!bHasAll)
				{
					bAllPassed = false;
					FGameplayTagContainer Missing;
					for (const FGameplayTag& Tag : *RequiredTags)
					{
						if (!OwnedTags.HasTag(Tag))
						{
							Missing.AddTag(Tag);
						}
					}
					TArray<TSharedPtr<FJsonValue>> MissingArr;
					for (const FGameplayTag& Tag : Missing)
					{
						MissingArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
					}
					Check->SetArrayField(TEXT("missing_tags"), MissingArr);
				}

				Checks.Add(MakeShared<FJsonValueObject>(Check));
			}
		}
	}

	// ── Check 2: Blocked tags (ActivationBlockedTags) ──
	{
		FProperty* Prop = AbilityCDO->GetClass()->FindPropertyByName(TEXT("ActivationBlockedTags"));
		if (Prop)
		{
			FGameplayTagContainer* BlockedTags = Prop->ContainerPtrToValuePtr<FGameplayTagContainer>(AbilityCDO);
			if (BlockedTags && BlockedTags->Num() > 0)
			{
				FGameplayTagContainer OwnedTags;
				ASC->GetOwnedGameplayTags(OwnedTags);
				bool bHasAnyBlocked = OwnedTags.HasAny(*BlockedTags);

				TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
				Check->SetStringField(TEXT("check"), TEXT("activation_blocked_tags"));
				Check->SetBoolField(TEXT("passed"), !bHasAnyBlocked);

				if (bHasAnyBlocked)
				{
					bAllPassed = false;
					FGameplayTagContainer ActiveBlocked = OwnedTags.Filter(*BlockedTags);
					TArray<TSharedPtr<FJsonValue>> BlockArr;
					for (const FGameplayTag& Tag : ActiveBlocked)
					{
						BlockArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
					}
					Check->SetArrayField(TEXT("blocking_tags_present"), BlockArr);
				}

				Checks.Add(MakeShared<FJsonValueObject>(Check));
			}
		}
	}

	// ── Check 3: Source tags (SourceRequiredTags / SourceBlockedTags) ──
	{
		FProperty* SrcReqProp = AbilityCDO->GetClass()->FindPropertyByName(TEXT("SourceRequiredTags"));
		if (SrcReqProp)
		{
			FGameplayTagContainer* SrcReqTags = SrcReqProp->ContainerPtrToValuePtr<FGameplayTagContainer>(AbilityCDO);
			if (SrcReqTags && SrcReqTags->Num() > 0)
			{
				FGameplayTagContainer OwnedTags;
				ASC->GetOwnedGameplayTags(OwnedTags);
				bool bPass = OwnedTags.HasAll(*SrcReqTags);

				TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
				Check->SetStringField(TEXT("check"), TEXT("source_required_tags"));
				Check->SetBoolField(TEXT("passed"), bPass);
				if (!bPass) bAllPassed = false;
				Checks.Add(MakeShared<FJsonValueObject>(Check));
			}
		}

		FProperty* SrcBlkProp = AbilityCDO->GetClass()->FindPropertyByName(TEXT("SourceBlockedTags"));
		if (SrcBlkProp)
		{
			FGameplayTagContainer* SrcBlkTags = SrcBlkProp->ContainerPtrToValuePtr<FGameplayTagContainer>(AbilityCDO);
			if (SrcBlkTags && SrcBlkTags->Num() > 0)
			{
				FGameplayTagContainer OwnedTags;
				ASC->GetOwnedGameplayTags(OwnedTags);
				bool bPass = !OwnedTags.HasAny(*SrcBlkTags);

				TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
				Check->SetStringField(TEXT("check"), TEXT("source_blocked_tags"));
				Check->SetBoolField(TEXT("passed"), bPass);
				if (!bPass) bAllPassed = false;
				Checks.Add(MakeShared<FJsonValueObject>(Check));
			}
		}
	}

	// ── Check 4: Cooldown ──
	{
		const FGameplayTagContainer* CooldownTags = AbilityCDO->GetCooldownTags();
		if (CooldownTags && CooldownTags->Num() > 0)
		{
			FGameplayEffectQuery Query;
			Query.OwningTagQuery = FGameplayTagQuery::MakeQuery_MatchAllTags(*CooldownTags);

			bool bOnCooldown = false;
			float MaxRemaining = 0.f;

			TArray<TPair<float, float>> DurationsAndRemaining = ASC->GetActiveEffectsTimeRemainingAndDuration(Query);
			for (const TPair<float, float>& Pair : DurationsAndRemaining)
			{
				if (Pair.Key > 0.f)
				{
					bOnCooldown = true;
					MaxRemaining = FMath::Max(MaxRemaining, Pair.Key);
				}
			}

			TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
			Check->SetStringField(TEXT("check"), TEXT("cooldown"));
			Check->SetBoolField(TEXT("passed"), !bOnCooldown);
			if (bOnCooldown)
			{
				bAllPassed = false;
				Check->SetNumberField(TEXT("time_remaining"), MaxRemaining);
			}
			Checks.Add(MakeShared<FJsonValueObject>(Check));
		}
	}

	// ── Check 5: Cost affordability ──
	{
		FProperty* CostProp = AbilityCDO->GetClass()->FindPropertyByName(TEXT("CostGameplayEffectClass"));
		if (CostProp)
		{
			FClassProperty* ClassProp = CastField<FClassProperty>(CostProp);
			UClass* CostClass = nullptr;
			if (ClassProp)
			{
				UObject* Val = ClassProp->GetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(AbilityCDO));
				CostClass = Cast<UClass>(Val);
			}

			if (CostClass)
			{
				TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
				Check->SetStringField(TEXT("check"), TEXT("cost"));
				Check->SetStringField(TEXT("cost_effect_class"), CostClass->GetName());

				// Check affordability via the ability's CheckCost
				const UGameplayAbility* AbilityToCheck = FoundSpec->GetPrimaryInstance();
				if (!AbilityToCheck) AbilityToCheck = AbilityCDO;
				bool bCanAfford = AbilityToCheck->CheckCost(FoundSpec->Handle, ASC->AbilityActorInfo.Get());

				Check->SetBoolField(TEXT("passed"), bCanAfford);
				if (!bCanAfford) bAllPassed = false;

				Checks.Add(MakeShared<FJsonValueObject>(Check));
			}
		}
	}

	// ── Check 6: Blocked by other abilities ──
	{
		FGameplayTagContainer BlockedTags;
		ASC->GetBlockedAbilityTags(BlockedTags);

		FProperty* AbilityTagsProp = MonolithGAS::FindAbilityAssetTagsProperty(AbilityCDO->GetClass());
		if (AbilityTagsProp && BlockedTags.Num() > 0)
		{
			FGameplayTagContainer* AbilityTags = AbilityTagsProp->ContainerPtrToValuePtr<FGameplayTagContainer>(AbilityCDO);
			if (AbilityTags && AbilityTags->Num() > 0)
			{
				bool bIsBlocked = AbilityTags->HasAny(BlockedTags);

				TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
				Check->SetStringField(TEXT("check"), TEXT("blocked_by_active_abilities"));
				Check->SetBoolField(TEXT("passed"), !bIsBlocked);

				if (bIsBlocked)
				{
					bAllPassed = false;
					FGameplayTagContainer MatchingBlocked = AbilityTags->Filter(BlockedTags);
					TArray<TSharedPtr<FJsonValue>> BlockArr;
					for (const FGameplayTag& Tag : MatchingBlocked)
					{
						BlockArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
					}
					Check->SetArrayField(TEXT("blocked_by_tags"), BlockArr);
				}

				Checks.Add(MakeShared<FJsonValueObject>(Check));
			}
		}
	}

	// ── Check 7: Already active (instancing check) ──
	{
		FProperty* InstProp = AbilityCDO->GetClass()->FindPropertyByName(TEXT("InstancingPolicy"));
		if (InstProp)
		{
			uint8* ValPtr = InstProp->ContainerPtrToValuePtr<uint8>(AbilityCDO);
			if (ValPtr)
			{
				// 0=NonInstanced, 1=InstancedPerActor, 2=InstancedPerExecution
				uint8 Policy = *ValPtr;
				bool bIsActive = FoundSpec->IsActive();

				if (Policy != 2 && bIsActive) // NonInstanced or InstancedPerActor, already active
				{
					// Check if ability allows re-triggering
					FBoolProperty* RetriggerProp = CastField<FBoolProperty>(
						AbilityCDO->GetClass()->FindPropertyByName(TEXT("bRetriggerInstancedAbility")));
					bool bCanRetrigger = RetriggerProp ?
						RetriggerProp->GetPropertyValue_InContainer(AbilityCDO) : false;

					TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
					Check->SetStringField(TEXT("check"), TEXT("already_active"));
					Check->SetBoolField(TEXT("passed"), bCanRetrigger);
					Check->SetBoolField(TEXT("is_currently_active"), true);
					Check->SetBoolField(TEXT("can_retrigger"), bCanRetrigger);

					FString PolicyStr;
					switch (Policy)
					{
					case 0: PolicyStr = TEXT("NonInstanced"); break;
					case 1: PolicyStr = TEXT("InstancedPerActor"); break;
					default: PolicyStr = TEXT("Unknown"); break;
					}
					Check->SetStringField(TEXT("instancing_policy"), PolicyStr);

					if (!bCanRetrigger) bAllPassed = false;
					Checks.Add(MakeShared<FJsonValueObject>(Check));
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetName());
	Result->SetStringField(TEXT("ability_class"), AbilityCDO->GetClass()->GetName());
	Result->SetBoolField(TEXT("can_activate"), bAllPassed);
	Result->SetNumberField(TEXT("checks_performed"), Checks.Num());
	Result->SetArrayField(TEXT("checks"), Checks);

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// compare_gas_states
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

/** Diff helper: compare two JSON arrays of objects by a key field, report added/removed/changed */
void DiffJsonArrayByKey(
	const TArray<TSharedPtr<FJsonValue>>* ArrA,
	const TArray<TSharedPtr<FJsonValue>>* ArrB,
	const FString& KeyField,
	const FString& Category,
	TArray<TSharedPtr<FJsonValue>>& OutDiffs)
{
	TMap<FString, TSharedPtr<FJsonObject>> MapA, MapB;

	if (ArrA)
	{
		for (const TSharedPtr<FJsonValue>& Val : *ArrA)
		{
			if (Val->Type == EJson::Object)
			{
				TSharedPtr<FJsonObject> Obj = Val->AsObject();
				FString Key = Obj->GetStringField(KeyField);
				if (!Key.IsEmpty()) MapA.Add(Key, Obj);
			}
		}
	}

	if (ArrB)
	{
		for (const TSharedPtr<FJsonValue>& Val : *ArrB)
		{
			if (Val->Type == EJson::Object)
			{
				TSharedPtr<FJsonObject> Obj = Val->AsObject();
				FString Key = Obj->GetStringField(KeyField);
				if (!Key.IsEmpty()) MapB.Add(Key, Obj);
			}
		}
	}

	// Removed (in A but not in B)
	for (const auto& Pair : MapA)
	{
		if (!MapB.Contains(Pair.Key))
		{
			TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
			Diff->SetStringField(TEXT("type"), TEXT("removed"));
			Diff->SetStringField(TEXT("category"), Category);
			Diff->SetStringField(TEXT("key"), Pair.Key);
			OutDiffs.Add(MakeShared<FJsonValueObject>(Diff));
		}
	}

	// Added (in B but not in A)
	for (const auto& Pair : MapB)
	{
		if (!MapA.Contains(Pair.Key))
		{
			TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
			Diff->SetStringField(TEXT("type"), TEXT("added"));
			Diff->SetStringField(TEXT("category"), Category);
			Diff->SetStringField(TEXT("key"), Pair.Key);
			OutDiffs.Add(MakeShared<FJsonValueObject>(Diff));
		}
	}
}

/** Diff helper: compare two JSON arrays of strings */
void DiffStringArrays(
	const TArray<TSharedPtr<FJsonValue>>* ArrA,
	const TArray<TSharedPtr<FJsonValue>>* ArrB,
	const FString& Category,
	TArray<TSharedPtr<FJsonValue>>& OutDiffs)
{
	TSet<FString> SetA, SetB;
	if (ArrA)
	{
		for (const TSharedPtr<FJsonValue>& V : *ArrA) SetA.Add(V->AsString());
	}
	if (ArrB)
	{
		for (const TSharedPtr<FJsonValue>& V : *ArrB) SetB.Add(V->AsString());
	}

	for (const FString& S : SetA)
	{
		if (!SetB.Contains(S))
		{
			TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
			Diff->SetStringField(TEXT("type"), TEXT("removed"));
			Diff->SetStringField(TEXT("category"), Category);
			Diff->SetStringField(TEXT("value"), S);
			OutDiffs.Add(MakeShared<FJsonValueObject>(Diff));
		}
	}

	for (const FString& S : SetB)
	{
		if (!SetA.Contains(S))
		{
			TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
			Diff->SetStringField(TEXT("type"), TEXT("added"));
			Diff->SetStringField(TEXT("category"), Category);
			Diff->SetStringField(TEXT("value"), S);
			OutDiffs.Add(MakeShared<FJsonValueObject>(Diff));
		}
	}
}

/** Diff attribute values between two actor snapshots */
void DiffAttributes(
	const TSharedPtr<FJsonObject>& ActorA,
	const TSharedPtr<FJsonObject>& ActorB,
	const FString& ActorName,
	TArray<TSharedPtr<FJsonValue>>& OutDiffs)
{
	// Build flat maps of attribute name -> current_value for each snapshot
	TMap<FString, double> ValsA, ValsB;

	auto ExtractAttrs = [](const TSharedPtr<FJsonObject>& ActorObj, TMap<FString, double>& OutVals)
	{
		const TArray<TSharedPtr<FJsonValue>>* Sets = nullptr;
		if (ActorObj->TryGetArrayField(TEXT("attribute_sets"), Sets))
		{
			for (const TSharedPtr<FJsonValue>& SetVal : *Sets)
			{
				TSharedPtr<FJsonObject> SetObj = SetVal->AsObject();
				if (!SetObj) continue;
				FString SetClass = SetObj->GetStringField(TEXT("set_class"));
				const TArray<TSharedPtr<FJsonValue>>* Attrs = nullptr;
				if (SetObj->TryGetArrayField(TEXT("attributes"), Attrs))
				{
					for (const TSharedPtr<FJsonValue>& AttrVal : *Attrs)
					{
						TSharedPtr<FJsonObject> AttrObj = AttrVal->AsObject();
						if (!AttrObj) continue;
						FString Key = SetClass + TEXT(".") + AttrObj->GetStringField(TEXT("name"));
						double Val = 0;
						AttrObj->TryGetNumberField(TEXT("current_value"), Val);
						OutVals.Add(Key, Val);
					}
				}
			}
		}
	};

	ExtractAttrs(ActorA, ValsA);
	ExtractAttrs(ActorB, ValsB);

	// Find changed values
	for (const auto& Pair : ValsA)
	{
		double* BVal = ValsB.Find(Pair.Key);
		if (!BVal)
		{
			TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
			Diff->SetStringField(TEXT("type"), TEXT("removed"));
			Diff->SetStringField(TEXT("category"), TEXT("attribute"));
			Diff->SetStringField(TEXT("actor_name"), ActorName);
			Diff->SetStringField(TEXT("attribute"), Pair.Key);
			Diff->SetNumberField(TEXT("old_value"), Pair.Value);
			OutDiffs.Add(MakeShared<FJsonValueObject>(Diff));
		}
		else if (!FMath::IsNearlyEqual(Pair.Value, *BVal, 0.001))
		{
			TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
			Diff->SetStringField(TEXT("type"), TEXT("changed"));
			Diff->SetStringField(TEXT("category"), TEXT("attribute"));
			Diff->SetStringField(TEXT("actor_name"), ActorName);
			Diff->SetStringField(TEXT("attribute"), Pair.Key);
			Diff->SetNumberField(TEXT("old_value"), Pair.Value);
			Diff->SetNumberField(TEXT("new_value"), *BVal);
			Diff->SetNumberField(TEXT("delta"), *BVal - Pair.Value);
			OutDiffs.Add(MakeShared<FJsonValueObject>(Diff));
		}
	}

	for (const auto& Pair : ValsB)
	{
		if (!ValsA.Contains(Pair.Key))
		{
			TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
			Diff->SetStringField(TEXT("type"), TEXT("added"));
			Diff->SetStringField(TEXT("category"), TEXT("attribute"));
			Diff->SetStringField(TEXT("actor_name"), ActorName);
			Diff->SetStringField(TEXT("attribute"), Pair.Key);
			Diff->SetNumberField(TEXT("new_value"), Pair.Value);
			OutDiffs.Add(MakeShared<FJsonValueObject>(Diff));
		}
	}
}

} // anonymous namespace

FMonolithActionResult FMonolithGASInspectActions::HandleCompareGASStates(const TSharedPtr<FJsonObject>& Params)
{
	const TSharedPtr<FJsonObject>* SnapshotA = nullptr;
	const TSharedPtr<FJsonObject>* SnapshotB = nullptr;

	if (!Params->TryGetObjectField(TEXT("snapshot_a"), SnapshotA) || !*SnapshotA)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: snapshot_a (object)"));
	}
	if (!Params->TryGetObjectField(TEXT("snapshot_b"), SnapshotB) || !*SnapshotB)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: snapshot_b (object)"));
	}

	TArray<TSharedPtr<FJsonValue>> AllDiffs;

	// Get actor arrays from both snapshots
	const TArray<TSharedPtr<FJsonValue>>* ActorsA = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ActorsB = nullptr;
	(*SnapshotA)->TryGetArrayField(TEXT("actors"), ActorsA);
	(*SnapshotB)->TryGetArrayField(TEXT("actors"), ActorsB);

	// Build per-actor maps for cross-referencing
	TMap<FString, TSharedPtr<FJsonObject>> ActorMapA, ActorMapB;
	if (ActorsA)
	{
		for (const TSharedPtr<FJsonValue>& V : *ActorsA)
		{
			TSharedPtr<FJsonObject> Obj = V->AsObject();
			if (Obj) ActorMapA.Add(Obj->GetStringField(TEXT("actor_name")), Obj);
		}
	}
	if (ActorsB)
	{
		for (const TSharedPtr<FJsonValue>& V : *ActorsB)
		{
			TSharedPtr<FJsonObject> Obj = V->AsObject();
			if (Obj) ActorMapB.Add(Obj->GetStringField(TEXT("actor_name")), Obj);
		}
	}

	// Diff actor presence
	for (const auto& Pair : ActorMapA)
	{
		if (!ActorMapB.Contains(Pair.Key))
		{
			TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
			Diff->SetStringField(TEXT("type"), TEXT("removed"));
			Diff->SetStringField(TEXT("category"), TEXT("actor"));
			Diff->SetStringField(TEXT("actor_name"), Pair.Key);
			AllDiffs.Add(MakeShared<FJsonValueObject>(Diff));
		}
	}
	for (const auto& Pair : ActorMapB)
	{
		if (!ActorMapA.Contains(Pair.Key))
		{
			TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
			Diff->SetStringField(TEXT("type"), TEXT("added"));
			Diff->SetStringField(TEXT("category"), TEXT("actor"));
			Diff->SetStringField(TEXT("actor_name"), Pair.Key);
			AllDiffs.Add(MakeShared<FJsonValueObject>(Diff));
		}
	}

	// For each actor in both snapshots, diff their internals
	for (const auto& Pair : ActorMapA)
	{
		TSharedPtr<FJsonObject>* BObj = ActorMapB.Find(Pair.Key);
		if (!BObj) continue;

		FString ActorPrefix = Pair.Key + TEXT(".");

		// Diff abilities
		const TArray<TSharedPtr<FJsonValue>>* AbA = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* AbB = nullptr;
		Pair.Value->TryGetArrayField(TEXT("abilities"), AbA);
		(*BObj)->TryGetArrayField(TEXT("abilities"), AbB);
		DiffJsonArrayByKey(AbA, AbB, TEXT("ability_class"), ActorPrefix + TEXT("ability"), AllDiffs);

		// Diff active effects
		const TArray<TSharedPtr<FJsonValue>>* EfA = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* EfB = nullptr;
		Pair.Value->TryGetArrayField(TEXT("active_effects"), EfA);
		(*BObj)->TryGetArrayField(TEXT("active_effects"), EfB);
		DiffJsonArrayByKey(EfA, EfB, TEXT("effect_class"), ActorPrefix + TEXT("effect"), AllDiffs);

		// Diff tags
		const TArray<TSharedPtr<FJsonValue>>* TagA = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* TagB = nullptr;
		Pair.Value->TryGetArrayField(TEXT("owned_tags"), TagA);
		(*BObj)->TryGetArrayField(TEXT("owned_tags"), TagB);
		DiffStringArrays(TagA, TagB, ActorPrefix + TEXT("tag"), AllDiffs);

		// Diff attributes
		DiffAttributes(Pair.Value, *BObj, Pair.Key, AllDiffs);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("diff_count"), AllDiffs.Num());
	Result->SetBoolField(TEXT("identical"), AllDiffs.Num() == 0);
	Result->SetArrayField(TEXT("diffs"), AllDiffs);

	// Summary counts
	int32 Added = 0, Removed = 0, Changed = 0;
	for (const TSharedPtr<FJsonValue>& V : AllDiffs)
	{
		FString Type = V->AsObject()->GetStringField(TEXT("type"));
		if (Type == TEXT("added")) Added++;
		else if (Type == TEXT("removed")) Removed++;
		else if (Type == TEXT("changed")) Changed++;
	}

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("added"), Added);
	Summary->SetNumberField(TEXT("removed"), Removed);
	Summary->SetNumberField(TEXT("changed"), Changed);
	Result->SetObjectField(TEXT("summary"), Summary);

	return FMonolithActionResult::Success(Result);
}
