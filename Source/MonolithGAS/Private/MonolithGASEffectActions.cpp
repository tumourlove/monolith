#include "MonolithGASEffectActions.h"
#include "MonolithParamSchema.h"
#include "MonolithGASInternal.h"

#include "GameplayEffect.h"
#include "GameplayEffectExecutionCalculation.h"
#include "GameplayModMagnitudeCalculation.h"
#include "AttributeSet.h"
#include "AbilitySystemInterface.h"
#include "GameplayTagContainer.h"
#include "EngineUtils.h"
#include "GameplayEffectComponent.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/BlockAbilityTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/CancelAbilityTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagRequirementsGameplayEffectComponent.h"
#include "GameplayEffectComponents/AdditionalEffectsGameplayEffectComponent.h"
#include "GameplayEffectComponents/ImmunityGameplayEffectComponent.h"
#include "GameplayEffectComponents/RemoveOtherGameplayEffectComponent.h"
#include "GameplayEffectComponents/ChanceToApplyGameplayEffectComponent.h"
#include "GameplayEffectComponents/CustomCanApplyGameplayEffectComponent.h"
#include "GameplayEffectComponents/AbilitiesGameplayEffectComponent.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"

// LogMonolithGAS defined in MonolithGASModule.cpp

// ============================================================
//  Helpers
// ============================================================

namespace
{

/** Load a GE Blueprint from Params (wraps MonolithGAS::LoadGameplayEffectBP). */
bool LoadGEFromParams(
	const TSharedPtr<FJsonObject>& Params,
	UBlueprint*& OutBP,
	UGameplayEffect*& OutGE,
	FString& OutAssetPath,
	FMonolithActionResult& OutError)
{
	FString Error;
	OutAssetPath = Params->GetStringField(TEXT("asset_path"));
	if (OutAssetPath.IsEmpty())
	{
		OutError = FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
		return false;
	}
	if (!MonolithGAS::LoadGameplayEffectBP(OutAssetPath, OutBP, OutGE, Error))
	{
		OutError = FMonolithActionResult::Error(Error);
		return false;
	}
	return true;
}

/** Get all GEComponents from a UGameplayEffect via subobject collection (GEComponents is protected). */
TArray<UGameplayEffectComponent*> GetGEComponents(UGameplayEffect* GE)
{
	TArray<UGameplayEffectComponent*> Result;
	if (!GE) return Result;
	TArray<UObject*> Subobjects;
	GE->GetDefaultSubobjects(Subobjects);
	for (UObject* Sub : Subobjects)
	{
		if (UGameplayEffectComponent* Comp = Cast<UGameplayEffectComponent>(Sub))
		{
			Result.Add(Comp);
		}
	}
	return Result;
}

/** Count GE components. */
int32 GetGEComponentCount(UGameplayEffect* GE)
{
	return GetGEComponents(GE).Num();
}

/**
 * Mark BP as modified after CDO changes AND persist to disk.
 *
 * Prior implementation used MarkBlueprintAsModified only — that flips dirty flags but does
 * not recompile the BPGC nor flush to .uasset. After editor restart the CDO mutations were
 * silently lost (same class of bug that hit add_attribute on GBA AttributeSets, 2026-04-25).
 *
 * Recipe matches HandleCreateAttributeSet's persistence tail:
 *   Modify -> MarkBlueprintAsStructurallyModified -> CompileBlueprint -> SavePackage.
 *
 * GE component edits qualify as structural (subobject collection changes), so the structural
 * variant is required; the non-structural call would skip dependent compile passes.
 */
void MarkModified(UBlueprint* BP)
{
	if (!BP)
	{
		return;
	}

	BP->Modify();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

	UPackage* OuterPackage = BP->GetOutermost();
	if (OuterPackage)
	{
		const FString PackageFilename = FPackageName::LongPackageNameToFilename(
			OuterPackage->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone;
		UPackage::SavePackage(OuterPackage, BP, *PackageFilename, SaveArgs);
	}
}

/** Parse "ClassName.PropertyName" into an FGameplayAttribute. */
FGameplayAttribute ParseAttribute(const FString& AttrString, FString& OutError)
{
	FString ClassName, PropName;
	if (!AttrString.Split(TEXT("."), &ClassName, &PropName))
	{
		OutError = FString::Printf(TEXT("Attribute must be 'ClassName.PropertyName', got: %s"), *AttrString);
		return FGameplayAttribute();
	}

	// Find the UClass (try with U prefix for attribute sets)
	UClass* AttrClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (!AttrClass)
	{
		AttrClass = FindFirstObject<UClass>(*(TEXT("U") + ClassName), EFindFirstObjectOptions::NativeFirst);
	}
	// Attempt 3: Blueprint generated class (_C suffix)
	if (!AttrClass)
	{
		AttrClass = FindFirstObject<UClass>(*(ClassName + TEXT("_C")), EFindFirstObjectOptions::NativeFirst);
	}
	// Attempt 4: Full asset path load (e.g. /Game/GAS/AS_Vitals.AS_Vitals_C)
	if (!AttrClass && ClassName.Contains(TEXT("/")))
	{
		FString ClassPath = ClassName;
		if (!ClassPath.EndsWith(TEXT("_C")))
		{
			ClassPath += TEXT("_C");
		}
		AttrClass = StaticLoadClass(UAttributeSet::StaticClass(), nullptr, *ClassPath);
	}
	if (!AttrClass)
	{
		OutError = FString::Printf(TEXT("Attribute class not found: %s"), *ClassName);
		return FGameplayAttribute();
	}

	FProperty* Prop = FindFProperty<FProperty>(AttrClass, FName(*PropName));
	if (!Prop)
	{
		OutError = FString::Printf(TEXT("Property '%s' not found on class '%s'"), *PropName, *ClassName);
		return FGameplayAttribute();
	}

	return FGameplayAttribute(Prop);
}

/** Convert FGameplayAttribute to "ClassName.PropertyName" string. */
FString AttributeToString(const FGameplayAttribute& Attr)
{
	if (!Attr.IsValid())
	{
		return TEXT("");
	}

	FString ClassName;
	if (Attr.GetAttributeSetClass())
	{
		ClassName = Attr.GetAttributeSetClass()->GetName();
	}
	FString PropName = Attr.GetName();
	return FString::Printf(TEXT("%s.%s"), *ClassName, *PropName);
}

/** Parse EGameplayModOp from string. */
bool ParseModifierOp(const FString& OpStr, EGameplayModOp::Type& OutOp, FString& OutError)
{
	if (OpStr.Equals(TEXT("Add"), ESearchCase::IgnoreCase) || OpStr.Equals(TEXT("Additive"), ESearchCase::IgnoreCase))
	{
		OutOp = EGameplayModOp::Additive;
	}
	else if (OpStr.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase) || OpStr.Equals(TEXT("MultiplyAdditive"), ESearchCase::IgnoreCase))
	{
		OutOp = EGameplayModOp::MultiplyAdditive;
	}
	else if (OpStr.Equals(TEXT("MultiplyCompound"), ESearchCase::IgnoreCase))
	{
		OutOp = EGameplayModOp::MultiplyCompound;
	}
	else if (OpStr.Equals(TEXT("Divide"), ESearchCase::IgnoreCase))
	{
		OutOp = EGameplayModOp::DivideAdditive;
	}
	else if (OpStr.Equals(TEXT("Override"), ESearchCase::IgnoreCase))
	{
		OutOp = EGameplayModOp::Override;
	}
	else if (OpStr.Equals(TEXT("AddFinal"), ESearchCase::IgnoreCase))
	{
		OutOp = EGameplayModOp::AddFinal;
	}
	else
	{
		OutError = FString::Printf(TEXT("Unknown modifier operation: %s. Valid: Add, Multiply, MultiplyCompound, Divide, Override, AddFinal"), *OpStr);
		return false;
	}
	return true;
}

/** Convert EGameplayModOp to string. */
FString ModifierOpToString(EGameplayModOp::Type Op)
{
	switch (Op)
	{
	case EGameplayModOp::Additive:          return TEXT("Add");
	case EGameplayModOp::MultiplyAdditive:  return TEXT("Multiply");
	case EGameplayModOp::MultiplyCompound:  return TEXT("MultiplyCompound");
	case EGameplayModOp::DivideAdditive:    return TEXT("Divide");
	case EGameplayModOp::Override:          return TEXT("Override");
	case EGameplayModOp::AddFinal:          return TEXT("AddFinal");
	default:                                return TEXT("Unknown");
	}
}

/** Convert EGameplayEffectDurationType to string. */
FString DurationPolicyToString(EGameplayEffectDurationType Policy)
{
	switch (Policy)
	{
	case EGameplayEffectDurationType::Instant:     return TEXT("instant");
	case EGameplayEffectDurationType::HasDuration: return TEXT("has_duration");
	case EGameplayEffectDurationType::Infinite:    return TEXT("infinite");
	default:                                       return TEXT("unknown");
	}
}

/** Parse duration policy string. */
bool ParseDurationPolicy(const FString& Str, EGameplayEffectDurationType& OutPolicy, FString& OutError)
{
	if (Str.Equals(TEXT("instant"), ESearchCase::IgnoreCase))
	{
		OutPolicy = EGameplayEffectDurationType::Instant;
	}
	else if (Str.Equals(TEXT("has_duration"), ESearchCase::IgnoreCase))
	{
		OutPolicy = EGameplayEffectDurationType::HasDuration;
	}
	else if (Str.Equals(TEXT("infinite"), ESearchCase::IgnoreCase))
	{
		OutPolicy = EGameplayEffectDurationType::Infinite;
	}
	else
	{
		OutError = FString::Printf(TEXT("Invalid duration_policy: '%s'. Valid: instant, has_duration, infinite"), *Str);
		return false;
	}
	return true;
}

/** Convert magnitude calc type to string. */
FString MagnitudeTypeToString(EGameplayEffectMagnitudeCalculation Type)
{
	switch (Type)
	{
	case EGameplayEffectMagnitudeCalculation::ScalableFloat:          return TEXT("scalable_float");
	case EGameplayEffectMagnitudeCalculation::AttributeBased:         return TEXT("attribute_based");
	case EGameplayEffectMagnitudeCalculation::CustomCalculationClass: return TEXT("custom_calculation");
	case EGameplayEffectMagnitudeCalculation::SetByCaller:            return TEXT("set_by_caller");
	default:                                                          return TEXT("unknown");
	}
}

/** Serialize a modifier magnitude to JSON. */
TSharedPtr<FJsonObject> MagnitudeToJson(const FGameplayEffectModifierMagnitude& Mag)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), MagnitudeTypeToString(Mag.GetMagnitudeCalculationType()));

	float StaticValue = 0.f;
	if (Mag.GetStaticMagnitudeIfPossible(1.f, StaticValue))
	{
		Obj->SetNumberField(TEXT("value"), StaticValue);
	}

	if (Mag.GetMagnitudeCalculationType() == EGameplayEffectMagnitudeCalculation::SetByCaller)
	{
		const FSetByCallerFloat& SBC = Mag.GetSetByCallerFloat();
		if (SBC.DataTag.IsValid())
		{
			Obj->SetStringField(TEXT("tag"), SBC.DataTag.ToString());
		}
		if (SBC.DataName != NAME_None)
		{
			Obj->SetStringField(TEXT("data_name"), SBC.DataName.ToString());
		}
	}

	if (Mag.GetMagnitudeCalculationType() == EGameplayEffectMagnitudeCalculation::CustomCalculationClass)
	{
		TSubclassOf<UGameplayModMagnitudeCalculation> CalcClass = Mag.GetCustomMagnitudeCalculationClass();
		if (CalcClass)
		{
			Obj->SetStringField(TEXT("calculation_class"), CalcClass->GetPathName());
		}
	}

	return Obj;
}

/** Serialize a single FGameplayModifierInfo to JSON. */
TSharedPtr<FJsonObject> ModifierToJson(const FGameplayModifierInfo& Mod, int32 Index)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("index"), Index);
	Obj->SetStringField(TEXT("attribute"), AttributeToString(Mod.Attribute));
	Obj->SetStringField(TEXT("operation"), ModifierOpToString(Mod.ModifierOp));
	Obj->SetObjectField(TEXT("magnitude"), MagnitudeToJson(Mod.ModifierMagnitude));
	return Obj;
}

/** Configure a FGameplayEffectModifierMagnitude from JSON params. */
bool ConfigureMagnitude(
	const TSharedPtr<FJsonObject>& MagObj,
	FGameplayEffectModifierMagnitude& OutMag,
	FString& OutError)
{
	FString TypeStr = MagObj->GetStringField(TEXT("type"));
	if (TypeStr.IsEmpty())
	{
		TypeStr = TEXT("scalable_float");
	}

	if (TypeStr == TEXT("scalable_float"))
	{
		float Value = 0.f;
		if (MagObj->HasField(TEXT("value")))
		{
			Value = MagObj->GetNumberField(TEXT("value"));
		}
		OutMag = FGameplayEffectModifierMagnitude(FScalableFloat(Value));
	}
	else if (TypeStr == TEXT("set_by_caller"))
	{
		FSetByCallerFloat SBC;
		FString TagStr = MagObj->GetStringField(TEXT("tag"));
		if (!TagStr.IsEmpty())
		{
			SBC.DataTag = FGameplayTag::RequestGameplayTag(FName(*TagStr), false);
			if (!SBC.DataTag.IsValid())
			{
				OutError = FString::Printf(TEXT("Invalid SetByCaller tag: %s"), *TagStr);
				return false;
			}
		}
		FString DataName = MagObj->GetStringField(TEXT("data_name"));
		if (!DataName.IsEmpty())
		{
			SBC.DataName = FName(*DataName);
		}
		OutMag = FGameplayEffectModifierMagnitude(SBC);
	}
	else if (TypeStr == TEXT("attribute_based"))
	{
		FAttributeBasedFloat ABF;
		FString SourceAttr = MagObj->GetStringField(TEXT("source_attribute"));
		if (!SourceAttr.IsEmpty())
		{
			FString AttrError;
			ABF.BackingAttribute = FGameplayEffectAttributeCaptureDefinition(
				ParseAttribute(SourceAttr, AttrError),
				EGameplayEffectAttributeCaptureSource::Source,
				false);
			if (!AttrError.IsEmpty())
			{
				OutError = AttrError;
				return false;
			}
		}
		if (MagObj->HasField(TEXT("coefficient")))
		{
			ABF.Coefficient = FScalableFloat(MagObj->GetNumberField(TEXT("coefficient")));
		}
		if (MagObj->HasField(TEXT("pre_multiply_additive_value")))
		{
			ABF.PreMultiplyAdditiveValue = FScalableFloat(MagObj->GetNumberField(TEXT("pre_multiply_additive_value")));
		}
		if (MagObj->HasField(TEXT("post_multiply_additive_value")))
		{
			ABF.PostMultiplyAdditiveValue = FScalableFloat(MagObj->GetNumberField(TEXT("post_multiply_additive_value")));
		}
		OutMag = FGameplayEffectModifierMagnitude(ABF);
	}
	else if (TypeStr == TEXT("custom_calculation"))
	{
		FCustomCalculationBasedFloat CCF;
		FString CalcClassPath = MagObj->GetStringField(TEXT("calculation_class"));
		if (!CalcClassPath.IsEmpty())
		{
			UClass* CalcClass = FindFirstObject<UClass>(*CalcClassPath, EFindFirstObjectOptions::NativeFirst);
			if (!CalcClass)
			{
				// Try loading
				CalcClass = LoadClass<UGameplayModMagnitudeCalculation>(nullptr, *CalcClassPath);
			}
			if (!CalcClass || !CalcClass->IsChildOf(UGameplayModMagnitudeCalculation::StaticClass()))
			{
				OutError = FString::Printf(TEXT("Calculation class not found or invalid: %s"), *CalcClassPath);
				return false;
			}
			CCF.CalculationClassMagnitude = CalcClass;
		}
		if (MagObj->HasField(TEXT("coefficient")))
		{
			CCF.Coefficient = FScalableFloat(MagObj->GetNumberField(TEXT("coefficient")));
		}
		OutMag = FGameplayEffectModifierMagnitude(CCF);
	}
	else
	{
		OutError = FString::Printf(TEXT("Unknown magnitude type: %s. Valid: scalable_float, set_by_caller, attribute_based, custom_calculation"), *TypeStr);
		return false;
	}

	return true;
}

/** Serialize a GE component to JSON (type + basic info). */
TSharedPtr<FJsonObject> GEComponentToJson(const UGameplayEffectComponent* Comp)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!Comp)
	{
		return Obj;
	}

	Obj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());

	// Dispatch per known component type
	if (const UAssetTagsGameplayEffectComponent* AssetTags = Cast<UAssetTagsGameplayEffectComponent>(Comp))
	{
		Obj->SetStringField(TEXT("type"), TEXT("asset_tags"));
		Obj->SetField(TEXT("tags"), MonolithGAS::TagContainerToJson(AssetTags->GetConfiguredAssetTagChanges().Added));
	}
	else if (const UTargetTagsGameplayEffectComponent* TargetTags = Cast<UTargetTagsGameplayEffectComponent>(Comp))
	{
		Obj->SetStringField(TEXT("type"), TEXT("target_tags"));
		Obj->SetField(TEXT("tags"), MonolithGAS::TagContainerToJson(TargetTags->GetConfiguredTargetTagChanges().Added));
	}
	else if (const UBlockAbilityTagsGameplayEffectComponent* BlockTags = Cast<UBlockAbilityTagsGameplayEffectComponent>(Comp))
	{
		Obj->SetStringField(TEXT("type"), TEXT("block_abilities"));
		Obj->SetField(TEXT("tags"), MonolithGAS::TagContainerToJson(BlockTags->GetConfiguredBlockedAbilityTagChanges().Added));
	}
	else if (const UTargetTagRequirementsGameplayEffectComponent* TagReqs = Cast<UTargetTagRequirementsGameplayEffectComponent>(Comp))
	{
		Obj->SetStringField(TEXT("type"), TEXT("target_tag_requirements"));
		TSharedPtr<FJsonObject> Config = MakeShared<FJsonObject>();
		Config->SetField(TEXT("application_require_tags"), MonolithGAS::TagContainerToJson(TagReqs->ApplicationTagRequirements.RequireTags));
		Config->SetField(TEXT("application_ignore_tags"), MonolithGAS::TagContainerToJson(TagReqs->ApplicationTagRequirements.IgnoreTags));
		Config->SetField(TEXT("ongoing_require_tags"), MonolithGAS::TagContainerToJson(TagReqs->OngoingTagRequirements.RequireTags));
		Config->SetField(TEXT("ongoing_ignore_tags"), MonolithGAS::TagContainerToJson(TagReqs->OngoingTagRequirements.IgnoreTags));
		Config->SetField(TEXT("removal_require_tags"), MonolithGAS::TagContainerToJson(TagReqs->RemovalTagRequirements.RequireTags));
		Config->SetField(TEXT("removal_ignore_tags"), MonolithGAS::TagContainerToJson(TagReqs->RemovalTagRequirements.IgnoreTags));
		Obj->SetObjectField(TEXT("config"), Config);
	}
	else if (const UAdditionalEffectsGameplayEffectComponent* AddEffects = Cast<UAdditionalEffectsGameplayEffectComponent>(Comp))
	{
		Obj->SetStringField(TEXT("type"), TEXT("additional_effects"));
		TSharedPtr<FJsonObject> Config = MakeShared<FJsonObject>();
		Config->SetBoolField(TEXT("copy_data_from_original_spec"), AddEffects->bOnApplicationCopyDataFromOriginalSpec);
		Config->SetNumberField(TEXT("on_application_count"), AddEffects->OnApplicationGameplayEffects.Num());
		Config->SetNumberField(TEXT("on_complete_always_count"), AddEffects->OnCompleteAlways.Num());
		Config->SetNumberField(TEXT("on_complete_normal_count"), AddEffects->OnCompleteNormal.Num());
		Config->SetNumberField(TEXT("on_complete_premature_count"), AddEffects->OnCompletePrematurely.Num());
		Obj->SetObjectField(TEXT("config"), Config);
	}
	else if (const UImmunityGameplayEffectComponent* Immunity = Cast<UImmunityGameplayEffectComponent>(Comp))
	{
		Obj->SetStringField(TEXT("type"), TEXT("immunity"));
		Obj->SetNumberField(TEXT("query_count"), Immunity->ImmunityQueries.Num());
	}
	else if (const URemoveOtherGameplayEffectComponent* RemoveOther = Cast<URemoveOtherGameplayEffectComponent>(Comp))
	{
		Obj->SetStringField(TEXT("type"), TEXT("remove_other"));
		Obj->SetNumberField(TEXT("query_count"), RemoveOther->RemoveGameplayEffectQueries.Num());
	}
	else if (const UChanceToApplyGameplayEffectComponent* Chance = Cast<UChanceToApplyGameplayEffectComponent>(Comp))
	{
		Obj->SetStringField(TEXT("type"), TEXT("chance_to_apply"));
		float ChanceVal = Chance->GetChanceToApplyToTarget().GetValueAtLevel(1.f);
		Obj->SetNumberField(TEXT("chance"), ChanceVal);
	}
	else if (const UCustomCanApplyGameplayEffectComponent* CustomApply = Cast<UCustomCanApplyGameplayEffectComponent>(Comp))
	{
		Obj->SetStringField(TEXT("type"), TEXT("custom_can_apply"));
		TArray<TSharedPtr<FJsonValue>> ClassArr;
		for (const auto& Cls : CustomApply->ApplicationRequirements)
		{
			if (Cls)
			{
				ClassArr.Add(MakeShared<FJsonValueString>(Cls->GetPathName()));
			}
		}
		Obj->SetArrayField(TEXT("requirement_classes"), ClassArr);
	}
	else if (Cast<UAbilitiesGameplayEffectComponent>(Comp))
	{
		Obj->SetStringField(TEXT("type"), TEXT("grant_abilities"));
	}
	else if (const UCancelAbilityTagsGameplayEffectComponent* CancelTags = Cast<UCancelAbilityTagsGameplayEffectComponent>(Comp))
	{
		Obj->SetStringField(TEXT("type"), TEXT("cancel_abilities"));
	}
	else
	{
		Obj->SetStringField(TEXT("type"), TEXT("custom"));
	}

	return Obj;
}

/** Map component_type string to UClass. */
UClass* ResolveComponentClass(const FString& TypeStr, FString& OutError)
{
	static TMap<FString, UClass*> TypeMap;
	if (TypeMap.Num() == 0)
	{
		TypeMap.Add(TEXT("asset_tags"),                UAssetTagsGameplayEffectComponent::StaticClass());
		TypeMap.Add(TEXT("target_tags"),               UTargetTagsGameplayEffectComponent::StaticClass());
		TypeMap.Add(TEXT("block_abilities"),           UBlockAbilityTagsGameplayEffectComponent::StaticClass());
		TypeMap.Add(TEXT("cancel_abilities"),          UCancelAbilityTagsGameplayEffectComponent::StaticClass());
		TypeMap.Add(TEXT("target_tag_requirements"),   UTargetTagRequirementsGameplayEffectComponent::StaticClass());
		TypeMap.Add(TEXT("additional_effects"),        UAdditionalEffectsGameplayEffectComponent::StaticClass());
		TypeMap.Add(TEXT("immunity"),                  UImmunityGameplayEffectComponent::StaticClass());
		TypeMap.Add(TEXT("remove_other"),              URemoveOtherGameplayEffectComponent::StaticClass());
		TypeMap.Add(TEXT("chance_to_apply"),           UChanceToApplyGameplayEffectComponent::StaticClass());
		TypeMap.Add(TEXT("custom_can_apply"),          UCustomCanApplyGameplayEffectComponent::StaticClass());
		TypeMap.Add(TEXT("grant_abilities"),           UAbilitiesGameplayEffectComponent::StaticClass());
	}

	if (UClass** Found = TypeMap.Find(TypeStr))
	{
		return *Found;
	}

	OutError = FString::Printf(TEXT("Unknown component_type: '%s'. Valid: asset_tags, target_tags, block_abilities, cancel_abilities, target_tag_requirements, additional_effects, immunity, remove_other, chance_to_apply, custom_can_apply, grant_abilities"), *TypeStr);
	return nullptr;
}

/** Apply tag-based config to an FInheritedTagContainer, then call the component's set method. */
void ApplyTagConfig(const TSharedPtr<FJsonObject>& Config, FInheritedTagContainer& Container)
{
	// "tags" field = array of tags to add
	const TArray<TSharedPtr<FJsonValue>>* TagArr;
	if (Config->TryGetArrayField(TEXT("tags"), TagArr))
	{
		for (const auto& Val : *TagArr)
		{
			FString TagStr;
			if (Val->TryGetString(TagStr))
			{
				FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagStr), false);
				if (Tag.IsValid())
				{
					Container.Added.AddTag(Tag);
				}
			}
		}
	}
	// "remove_tags" field = tags to remove from inherited
	if (Config->TryGetArrayField(TEXT("remove_tags"), TagArr))
	{
		for (const auto& Val : *TagArr)
		{
			FString TagStr;
			if (Val->TryGetString(TagStr))
			{
				FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagStr), false);
				if (Tag.IsValid())
				{
					Container.Removed.AddTag(Tag);
				}
			}
		}
	}
}

	/** Parse stacking type. */
	bool ParseStackingType(const FString& Str, EGameplayEffectStackingType& OutType, FString& OutError)
	{
	if (Str.Equals(TEXT("none"), ESearchCase::IgnoreCase))
	{
		OutType = EGameplayEffectStackingType::None;
	}
	else if (Str.Equals(TEXT("aggregate_by_source"), ESearchCase::IgnoreCase))
	{
		OutType = EGameplayEffectStackingType::AggregateBySource;
	}
	else if (Str.Equals(TEXT("aggregate_by_target"), ESearchCase::IgnoreCase))
	{
		OutType = EGameplayEffectStackingType::AggregateByTarget;
	}
	else
	{
		OutError = FString::Printf(TEXT("Invalid stacking_type: '%s'. Valid: none, aggregate_by_source, aggregate_by_target"), *Str);
		return false;
	}
	return true;
	}

	EGameplayEffectStackingType GetGameplayEffectStackingTypeValue(const UGameplayEffect* GE)
	{
		if (!GE)
		{
			return EGameplayEffectStackingType::None;
		}

		EGameplayEffectStackingType StackType = EGameplayEffectStackingType::None;
		if (FProperty* StackProp = UGameplayEffect::StaticClass()->FindPropertyByName(TEXT("StackingType")))
		{
			StackProp->CopyCompleteValue(&StackType, StackProp->ContainerPtrToValuePtr<void>(GE));
		}
		return StackType;
	}

	void SetGameplayEffectStackingTypeValue(UGameplayEffect* GE, EGameplayEffectStackingType StackType)
	{
		if (!GE)
		{
			return;
		}

		if (FProperty* StackProp = UGameplayEffect::StaticClass()->FindPropertyByName(TEXT("StackingType")))
		{
			StackProp->CopyCompleteValue(StackProp->ContainerPtrToValuePtr<void>(GE), &StackType);
		}
	}

} // anonymous namespace

// ============================================================
//  Registration
// ============================================================

void FMonolithGASEffectActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 1. create_gameplay_effect
	Registry.RegisterAction(TEXT("gas"), TEXT("create_gameplay_effect"),
		TEXT("Create a new GameplayEffect Blueprint asset with the specified duration policy"),
		FMonolithActionHandler::CreateStatic(&HandleCreateGameplayEffect),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path, e.g. /Game/GAS/Effects/GE_Damage"))
			.Required(TEXT("duration_policy"), TEXT("string"), TEXT("Duration type: instant, has_duration, infinite"))
			.Optional(TEXT("parent_class"), TEXT("string"), TEXT("Parent class (default: GameplayEffect)"))
			.Build());

	// 2. get_gameplay_effect
	Registry.RegisterAction(TEXT("gas"), TEXT("get_gameplay_effect"),
		TEXT("Read all configuration from a GameplayEffect: duration, modifiers, components, stacking, cues, executions"),
		FMonolithActionHandler::CreateStatic(&HandleGetGameplayEffect),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayEffect Blueprint asset path"))
			.Build());

	// 3. list_gameplay_effects
	Registry.RegisterAction(TEXT("gas"), TEXT("list_gameplay_effects"),
		TEXT("Find GameplayEffect Blueprints matching filters via AssetRegistry"),
		FMonolithActionHandler::CreateStatic(&HandleListGameplayEffects),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Content path prefix to search, e.g. /Game/GAS/Effects"))
			.Optional(TEXT("duration_policy"), TEXT("string"), TEXT("Filter by duration: instant, has_duration, infinite"))
			.Optional(TEXT("tag_filter"), TEXT("string"), TEXT("Filter by asset tag (substring match)"))
			.Optional(TEXT("attribute_filter"), TEXT("string"), TEXT("Filter by modifier attribute (substring match)"))
			.Build());

	// 4. add_modifier
	Registry.RegisterAction(TEXT("gas"), TEXT("add_modifier"),
		TEXT("Add an attribute modifier to a GameplayEffect"),
		FMonolithActionHandler::CreateStatic(&HandleAddModifier),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayEffect Blueprint asset path"))
			.Required(TEXT("attribute"), TEXT("string"), TEXT("Attribute as ClassName.PropertyName"))
			.Required(TEXT("operation"), TEXT("string"), TEXT("Modifier op: Add, Multiply, MultiplyCompound, Divide, Override, AddFinal"))
			.Required(TEXT("magnitude"), TEXT("object"), TEXT("Magnitude config: {type, value?, tag?, source_attribute?, calculation_class?}"))
			.Build());

	// 5. set_modifier
	Registry.RegisterAction(TEXT("gas"), TEXT("set_modifier"),
		TEXT("Update an existing modifier by index"),
		FMonolithActionHandler::CreateStatic(&HandleSetModifier),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayEffect Blueprint asset path"))
			.Required(TEXT("modifier_index"), TEXT("integer"), TEXT("Index of the modifier to update"))
			.Optional(TEXT("attribute"), TEXT("string"), TEXT("New attribute as ClassName.PropertyName"))
			.Optional(TEXT("operation"), TEXT("string"), TEXT("New modifier op"))
			.Optional(TEXT("magnitude"), TEXT("object"), TEXT("New magnitude config"))
			.Build());

	// 6. remove_modifier
	Registry.RegisterAction(TEXT("gas"), TEXT("remove_modifier"),
		TEXT("Remove a modifier by index or by attribute name"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveModifier),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayEffect Blueprint asset path"))
			.Optional(TEXT("modifier_index"), TEXT("integer"), TEXT("Index of the modifier to remove"))
			.Optional(TEXT("attribute"), TEXT("string"), TEXT("Remove first modifier matching this attribute"))
			.Build());

	// 7. list_modifiers
	Registry.RegisterAction(TEXT("gas"), TEXT("list_modifiers"),
		TEXT("List all modifiers on a GameplayEffect with attribute, operation, and magnitude details"),
		FMonolithActionHandler::CreateStatic(&HandleListModifiers),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayEffect Blueprint asset path"))
			.Build());

	// 8. add_ge_component
	Registry.RegisterAction(TEXT("gas"), TEXT("add_ge_component"),
		TEXT("Add a GE component (asset_tags, target_tags, block_abilities, etc.) to a GameplayEffect"),
		FMonolithActionHandler::CreateStatic(&HandleAddGEComponent),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayEffect Blueprint asset path"))
			.Required(TEXT("component_type"), TEXT("string"), TEXT("Component type: asset_tags, target_tags, block_abilities, cancel_abilities, target_tag_requirements, additional_effects, immunity, remove_other, chance_to_apply, custom_can_apply, grant_abilities"))
			.Required(TEXT("config"), TEXT("object"), TEXT("Type-specific configuration (e.g. {tags: [...]} for tag components)"))
			.Build());

	// 9. set_ge_component
	Registry.RegisterAction(TEXT("gas"), TEXT("set_ge_component"),
		TEXT("Update an existing GE component's configuration"),
		FMonolithActionHandler::CreateStatic(&HandleSetGEComponent),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayEffect Blueprint asset path"))
			.Required(TEXT("component_type"), TEXT("string"), TEXT("Component type to update"))
			.Required(TEXT("config"), TEXT("object"), TEXT("New configuration"))
			.Optional(TEXT("index"), TEXT("integer"), TEXT("Index if multiple of same type (default: 0)"))
			.Build());

	// 10. set_effect_stacking
	Registry.RegisterAction(TEXT("gas"), TEXT("set_effect_stacking"),
		TEXT("Configure stacking behavior for a GameplayEffect"),
		FMonolithActionHandler::CreateStatic(&HandleSetEffectStacking),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayEffect Blueprint asset path"))
			.Required(TEXT("stacking_type"), TEXT("string"), TEXT("none, aggregate_by_source, aggregate_by_target"))
			.Optional(TEXT("stack_limit"), TEXT("integer"), TEXT("Maximum stack count (0 or -1 for unlimited)"))
			.Optional(TEXT("stack_duration_refresh_policy"), TEXT("string"), TEXT("RefreshOnSuccessfulApplication or NeverRefresh"))
			.Optional(TEXT("stack_period_reset_policy"), TEXT("string"), TEXT("ResetOnSuccessfulApplication or NeverReset"))
			.Optional(TEXT("stack_expiration_policy"), TEXT("string"), TEXT("ClearEntireStack, RemoveSingleStackAndRefreshDuration, RefreshDuration"))
			.Build());

	// 11. set_duration
	Registry.RegisterAction(TEXT("gas"), TEXT("set_duration"),
		TEXT("Set the duration policy and optional duration magnitude of a GameplayEffect"),
		FMonolithActionHandler::CreateStatic(&HandleSetDuration),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayEffect Blueprint asset path"))
			.Required(TEXT("duration_policy"), TEXT("string"), TEXT("instant, has_duration, infinite"))
			.Optional(TEXT("duration_magnitude"), TEXT("number"), TEXT("Duration in seconds (for has_duration)"))
			.Build());

	// 12. set_period
	Registry.RegisterAction(TEXT("gas"), TEXT("set_period"),
		TEXT("Set the periodic execution interval for a GameplayEffect"),
		FMonolithActionHandler::CreateStatic(&HandleSetPeriod),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayEffect Blueprint asset path"))
			.Required(TEXT("period"), TEXT("number"), TEXT("Period in seconds (0 to disable)"))
			.Optional(TEXT("execute_on_application"), TEXT("boolean"), TEXT("Execute once immediately on application (default: false)"))
			.Build());

	// ---- Phase 2: Productivity ----

	// 13. create_effect_from_template
	Registry.RegisterAction(TEXT("gas"), TEXT("create_effect_from_template"),
		TEXT("Create a GameplayEffect from a named survival horror template with all modifiers, tags, stacking, and components pre-configured."),
		FMonolithActionHandler::CreateStatic(&HandleCreateEffectFromTemplate),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path for the new GE Blueprint"))
			.Required(TEXT("template"), TEXT("string"), TEXT("Template: instant_damage, dot_damage, heal_instant, heal_over_time, status_burning, status_frozen, status_bleeding, status_poisoned, fear_spike, sanity_drain, cooldown, stamina_cost, difficulty_preset, accessibility_override"))
			.Optional(TEXT("overrides"), TEXT("object"), TEXT("Override template values: {duration?, period?, magnitude?, stacking?, tags?: {asset_tags?, target_tags?}}"))
			.Build());

	// 14. build_effect_from_spec
	Registry.RegisterAction(TEXT("gas"), TEXT("build_effect_from_spec"),
		TEXT("Create a fully-configured GameplayEffect from a declarative specification in one call."),
		FMonolithActionHandler::CreateStatic(&HandleBuildEffectFromSpec),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path for the new GE Blueprint"))
			.Required(TEXT("spec"), TEXT("object"), TEXT("Full GE spec: {duration_policy, duration_magnitude?, period?, execute_on_application?, modifiers: [{attribute, operation, magnitude}], stacking?: {type, limit?, ...}, components?: [{type, config}]}"))
			.Build());

	// 15. batch_create_effects
	Registry.RegisterAction(TEXT("gas"), TEXT("batch_create_effects"),
		TEXT("Create multiple GameplayEffect Blueprints in one call. Each entry can use a template or inline spec."),
		FMonolithActionHandler::CreateStatic(&HandleBatchCreateEffects),
		FParamSchemaBuilder()
			.Required(TEXT("effects"), TEXT("array"), TEXT("Array of {save_path, template?, spec?, overrides?} — provide either template or spec per entry"))
			.Build());

	// 16. add_execution
	Registry.RegisterAction(TEXT("gas"), TEXT("add_execution"),
		TEXT("Add a UGameplayEffectExecutionCalculation to a GameplayEffect with optional scoped modifiers."),
		FMonolithActionHandler::CreateStatic(&HandleAddExecution),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayEffect Blueprint asset path"))
			.Required(TEXT("calculation_class"), TEXT("string"), TEXT("Execution calculation class name or path"))
			.Optional(TEXT("scoped_modifiers"), TEXT("array"), TEXT("Array of {attribute, operation, magnitude} captured for the execution calc"))
			.Build());

	// 17. duplicate_gameplay_effect
	Registry.RegisterAction(TEXT("gas"), TEXT("duplicate_gameplay_effect"),
		TEXT("Clone an existing GameplayEffect Blueprint with optional modifications."),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateGameplayEffect),
		FParamSchemaBuilder()
			.Required(TEXT("source_path"), TEXT("string"), TEXT("Source GE Blueprint asset path"))
			.Required(TEXT("dest_path"), TEXT("string"), TEXT("Destination asset path for the clone"))
			.Optional(TEXT("overrides"), TEXT("object"), TEXT("Override fields: {duration_policy?, modifiers?: [{attribute, operation, magnitude}]}"))
			.Build());

	// 18. delete_gameplay_effect
	Registry.RegisterAction(TEXT("gas"), TEXT("delete_gameplay_effect"),
		TEXT("Delete a GameplayEffect Blueprint asset after checking for references."),
		FMonolithActionHandler::CreateStatic(&HandleDeleteGameplayEffect),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayEffect Blueprint asset path to delete"))
			.Optional(TEXT("force"), TEXT("boolean"), TEXT("Force delete even if references exist (default: false)"))
			.Build());

	// ---- Phase 3: Validation & Analysis ----

	Registry.RegisterAction(TEXT("gas"), TEXT("validate_effect"),
		TEXT("Validate a GameplayEffect for common issues: period on instant, stacking on instant, missing attributes, missing SetByCaller tags, deprecated flat tag properties."),
		FMonolithActionHandler::CreateStatic(&HandleValidateEffect),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayEffect Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("get_effect_interaction_matrix"),
		TEXT("Analyze which effects remove, grant immunity to, or block each other."),
		FMonolithActionHandler::CreateStatic(&HandleGetEffectInteractionMatrix),
		FParamSchemaBuilder()
			.Optional(TEXT("asset_paths"), TEXT("array"), TEXT("Array of GE asset paths (default: all project GEs)"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("remove_ge_component"),
		TEXT("Remove a GE component by type and optional index."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveGEComponent),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayEffect Blueprint asset path"))
			.Required(TEXT("component_type"), TEXT("string"), TEXT("Component type: asset_tags, target_tags, block_abilities, etc."))
			.Optional(TEXT("index"), TEXT("integer"), TEXT("Index if multiple of same type exist (default: 0)"))
			.Build());

	// ---- Phase 4: Runtime ----

	Registry.RegisterAction(TEXT("gas"), TEXT("get_active_effects"),
		TEXT("List all active GameplayEffects on a live actor in PIE. Optionally filter by effect class or tag."),
		FMonolithActionHandler::CreateStatic(&HandleGetActiveEffects),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in PIE"))
			.Optional(TEXT("filter_class"), TEXT("string"), TEXT("Filter by GE class name"))
			.Optional(TEXT("filter_tag"), TEXT("string"), TEXT("Filter by asset tag on the GE"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("get_effect_modifiers_breakdown"),
		TEXT("Show how an attribute's current value is calculated from active modifiers on a live actor."),
		FMonolithActionHandler::CreateStatic(&HandleGetEffectModifiersBreakdown),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in PIE"))
			.Required(TEXT("attribute"), TEXT("string"), TEXT("Attribute as ClassName.PropertyName"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("apply_effect"),
		TEXT("Apply a GameplayEffect to a live actor in PIE (debug tool). Supports SetByCaller magnitudes."),
		FMonolithActionHandler::CreateStatic(&HandleApplyEffect),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in PIE"))
			.Required(TEXT("effect_class"), TEXT("string"), TEXT("GameplayEffect class name or asset path"))
			.Optional(TEXT("level"), TEXT("number"), TEXT("Effect level (default: 1)"))
			.Optional(TEXT("set_by_caller"), TEXT("object"), TEXT("Object mapping tag string to float magnitude"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("remove_effect"),
		TEXT("Remove an active GameplayEffect from a live actor by handle index or class name."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveEffect),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in PIE"))
			.Optional(TEXT("effect_handle"), TEXT("integer"), TEXT("Active effect handle index (from get_active_effects)"))
			.Optional(TEXT("effect_class"), TEXT("string"), TEXT("Remove all instances of this GE class"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("simulate_effect_stack"),
		TEXT("Simulate applying a sequence of GE-like modifiers to an attribute state. No PIE required."),
		FMonolithActionHandler::CreateStatic(&HandleSimulateEffectStack),
		FParamSchemaBuilder()
			.Required(TEXT("attribute_state"), TEXT("object"), TEXT("Initial state: {base_value: float}"))
			.Required(TEXT("effects"), TEXT("array"), TEXT("Array of {operation: Add|Multiply|..., magnitude: float}"))
			.Build());
}

// ============================================================
//  1. create_gameplay_effect
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleCreateGameplayEffect(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("save_path"), SavePath, Err)) return Err;

	FString DurationStr;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("duration_policy"), DurationStr, Err)) return Err;

	EGameplayEffectDurationType DurationPolicy;
	FString ParseError;
	if (!ParseDurationPolicy(DurationStr, DurationPolicy, ParseError))
	{
		return FMonolithActionResult::Error(ParseError);
	}

	// Resolve parent class
	FString ParentClassName = Params->GetStringField(TEXT("parent_class"));
	UClass* ParentClass = UGameplayEffect::StaticClass();
	if (!ParentClassName.IsEmpty())
	{
		ParentClass = FindFirstObject<UClass>(*ParentClassName, EFindFirstObjectOptions::NativeFirst);
		if (!ParentClass)
		{
			ParentClass = FindFirstObject<UClass>(*(TEXT("U") + ParentClassName), EFindFirstObjectOptions::NativeFirst);
		}
		if (!ParentClass || !ParentClass->IsChildOf(UGameplayEffect::StaticClass()))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Parent class '%s' not found or not a GameplayEffect subclass"), *ParentClassName));
		}
	}

	// Extract asset name
	int32 LastSlash;
	if (!SavePath.FindLastChar(TEXT('/'), LastSlash))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid save_path: %s"), *SavePath));
	}
	FString AssetName = SavePath.Mid(LastSlash + 1);
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("save_path must not end with '/'"));
	}

	// Check for existing asset (AssetRegistry + in-memory multi-tier check)
	FString ExistError;
	if (!MonolithGAS::EnsureAssetPathFree(SavePath, AssetName, ExistError))
	{
		return FMonolithActionResult::Error(ExistError);
	}

	// Create package
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *SavePath));
	}
	Package->FullyLoad();

	// Create Blueprint with UGameplayEffect parent
	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());

	if (!NewBP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create GE Blueprint at: %s"), *SavePath));
	}

	// Set DurationPolicy on CDO
	UGameplayEffect* GE_CDO = MonolithGAS::GetBlueprintCDO<UGameplayEffect>(NewBP);
	if (GE_CDO)
	{
		GE_CDO->DurationPolicy = DurationPolicy;
	}

	// Compile so the CDO is properly set up
	FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::SkipGarbageCollection);

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewBP);

	// Save to disk
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	FSavePackageResultStruct SaveResult = UPackage::Save(Package, NewBP,
		*FPackageName::LongPackageNameToFilename(SavePath, FPackageName::GetAssetPackageExtension()), SaveArgs);
	bool bSaved = (SaveResult.Result == ESavePackageResult::Success);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetStringField(TEXT("duration_policy"), DurationPolicyToString(DurationPolicy));
	Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  2. get_gameplay_effect
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleGetGameplayEffect(const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* BP = nullptr;
	UGameplayEffect* GE = nullptr;
	FString AssetPath;
	FMonolithActionResult Err;
	if (!LoadGEFromParams(Params, BP, GE, AssetPath, Err)) return Err;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("parent_class"), BP->ParentClass ? BP->ParentClass->GetName() : TEXT(""));

	// Duration
	Result->SetStringField(TEXT("duration_policy"), DurationPolicyToString(GE->DurationPolicy));
	if (GE->DurationPolicy == EGameplayEffectDurationType::HasDuration)
	{
		Result->SetObjectField(TEXT("duration_magnitude"), MagnitudeToJson(GE->DurationMagnitude));
	}

	// Period
	{
		float PeriodVal = GE->Period.GetValueAtLevel(1.f);
		Result->SetNumberField(TEXT("period"), PeriodVal);
		Result->SetBoolField(TEXT("execute_on_application"), GE->bExecutePeriodicEffectOnApplication);
	}

	// Modifiers
	{
		TArray<TSharedPtr<FJsonValue>> ModArr;
		for (int32 i = 0; i < GE->Modifiers.Num(); ++i)
		{
			ModArr.Add(MakeShared<FJsonValueObject>(ModifierToJson(GE->Modifiers[i], i)));
		}
		Result->SetArrayField(TEXT("modifiers"), ModArr);
	}

	// Executions
	{
		TArray<TSharedPtr<FJsonValue>> ExecArr;
		for (const FGameplayEffectExecutionDefinition& Exec : GE->Executions)
		{
			TSharedPtr<FJsonObject> ExecObj = MakeShared<FJsonObject>();
			if (Exec.CalculationClass)
			{
				ExecObj->SetStringField(TEXT("calculation_class"), Exec.CalculationClass->GetPathName());
			}
			ExecArr.Add(MakeShared<FJsonValueObject>(ExecObj));
		}
		Result->SetArrayField(TEXT("executions"), ExecArr);
	}

	// Components
	{
		TArray<TSharedPtr<FJsonValue>> CompArr;
		for (UGameplayEffectComponent* Comp : GetGEComponents(GE))
		{
			CompArr.Add(MakeShared<FJsonValueObject>(GEComponentToJson(Comp)));
		}
		Result->SetArrayField(TEXT("components"), CompArr);
	}

	// Stacking
	{
		TSharedPtr<FJsonObject> StackObj = MakeShared<FJsonObject>();
		EGameplayEffectStackingType StackType = GetGameplayEffectStackingTypeValue(GE);
		FString StackStr;
		switch (StackType)
		{
		case EGameplayEffectStackingType::None:                StackStr = TEXT("none"); break;
		case EGameplayEffectStackingType::AggregateBySource:   StackStr = TEXT("aggregate_by_source"); break;
		case EGameplayEffectStackingType::AggregateByTarget:   StackStr = TEXT("aggregate_by_target"); break;
		}
		StackObj->SetStringField(TEXT("stacking_type"), StackStr);
		StackObj->SetNumberField(TEXT("stack_limit"), GE->StackLimitCount);

		// Duration refresh policy
		switch (GE->StackDurationRefreshPolicy)
		{
		case EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication:
			StackObj->SetStringField(TEXT("stack_duration_refresh_policy"), TEXT("RefreshOnSuccessfulApplication")); break;
		case EGameplayEffectStackingDurationPolicy::NeverRefresh:
			StackObj->SetStringField(TEXT("stack_duration_refresh_policy"), TEXT("NeverRefresh")); break;
		}

		// Period reset policy
		switch (GE->StackPeriodResetPolicy)
		{
		case EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication:
			StackObj->SetStringField(TEXT("stack_period_reset_policy"), TEXT("ResetOnSuccessfulApplication")); break;
		case EGameplayEffectStackingPeriodPolicy::NeverReset:
			StackObj->SetStringField(TEXT("stack_period_reset_policy"), TEXT("NeverReset")); break;
		}

		// Expiration policy
		switch (GE->StackExpirationPolicy)
		{
		case EGameplayEffectStackingExpirationPolicy::ClearEntireStack:
			StackObj->SetStringField(TEXT("stack_expiration_policy"), TEXT("ClearEntireStack")); break;
		case EGameplayEffectStackingExpirationPolicy::RemoveSingleStackAndRefreshDuration:
			StackObj->SetStringField(TEXT("stack_expiration_policy"), TEXT("RemoveSingleStackAndRefreshDuration")); break;
		case EGameplayEffectStackingExpirationPolicy::RefreshDuration:
			StackObj->SetStringField(TEXT("stack_expiration_policy"), TEXT("RefreshDuration")); break;
		}

		Result->SetObjectField(TEXT("stacking"), StackObj);
	}

	// Gameplay Cues
	{
		TArray<TSharedPtr<FJsonValue>> CueArr;
		for (const FGameplayEffectCue& Cue : GE->GameplayCues)
		{
			TSharedPtr<FJsonObject> CueObj = MakeShared<FJsonObject>();
			CueObj->SetField(TEXT("tags"), MonolithGAS::TagContainerToJson(Cue.GameplayCueTags));
			CueObj->SetNumberField(TEXT("min_level"), Cue.MinLevel);
			CueObj->SetNumberField(TEXT("max_level"), Cue.MaxLevel);
			CueArr.Add(MakeShared<FJsonValueObject>(CueObj));
		}
		Result->SetArrayField(TEXT("gameplay_cues"), CueArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  3. list_gameplay_effects
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleListGameplayEffects(const TSharedPtr<FJsonObject>& Params)
{
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FString PathFilter = Params->GetStringField(TEXT("path_filter"));
	FString DurationFilter = Params->GetStringField(TEXT("duration_policy"));
	FString TagFilter = Params->GetStringField(TEXT("tag_filter"));
	FString AttributeFilter = Params->GetStringField(TEXT("attribute_filter"));

	// Search for all Blueprint assets
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	if (!PathFilter.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PathFilter));
		Filter.bRecursivePaths = true;
	}

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> ResultArr;

	for (const FAssetData& Asset : Assets)
	{
		// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
		FAssetTagValueRef ParentTag = Asset.TagsAndValues.FindTag(FName("ParentClass"));
		FAssetTagValueRef NativeParentTag = Asset.TagsAndValues.FindTag(FName("NativeParentClass"));
		FString ParentPath = ParentTag.IsSet() ? ParentTag.GetValue() : (NativeParentTag.IsSet() ? NativeParentTag.GetValue() : TEXT(""));
		if (!ParentPath.Contains(TEXT("GameplayEffect")))
		{
			continue;
		}

		UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
		if (!BP || !MonolithGAS::IsGameplayEffectBlueprint(BP))
		{
			continue;
		}

		UGameplayEffect* GE = MonolithGAS::GetBlueprintCDO<UGameplayEffect>(BP);
		if (!GE)
		{
			continue;
		}

		// Duration filter
		if (!DurationFilter.IsEmpty())
		{
			FString GEDuration = DurationPolicyToString(GE->DurationPolicy);
			if (!GEDuration.Equals(DurationFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		// Tag filter (substring match on asset tags from any AssetTagsComponent)
		if (!TagFilter.IsEmpty())
		{
			bool bTagMatch = false;
			for (UGameplayEffectComponent* Comp : GetGEComponents(GE))
			{
				if (const UAssetTagsGameplayEffectComponent* AssetTags = Cast<UAssetTagsGameplayEffectComponent>(Comp))
				{
					for (const FGameplayTag& Tag : AssetTags->GetConfiguredAssetTagChanges().Added)
					{
						if (Tag.ToString().Contains(TagFilter))
						{
							bTagMatch = true;
							break;
						}
					}
				}
				if (bTagMatch) break;
			}
			if (!bTagMatch) continue;
		}

		// Attribute filter (substring match on modifier attributes)
		if (!AttributeFilter.IsEmpty())
		{
			bool bAttrMatch = false;
			for (const FGameplayModifierInfo& Mod : GE->Modifiers)
			{
				FString AttrStr = AttributeToString(Mod.Attribute);
				if (AttrStr.Contains(AttributeFilter))
				{
					bAttrMatch = true;
					break;
				}
			}
			if (!bAttrMatch) continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), Asset.GetObjectPathString());
		Entry->SetStringField(TEXT("asset_name"), Asset.AssetName.ToString());
		Entry->SetStringField(TEXT("duration_policy"), DurationPolicyToString(GE->DurationPolicy));
		Entry->SetNumberField(TEXT("modifier_count"), GE->Modifiers.Num());
		Entry->SetNumberField(TEXT("component_count"), GetGEComponentCount(GE));
		ResultArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("effects"), ResultArr);
	Result->SetNumberField(TEXT("count"), ResultArr.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  4. add_modifier
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleAddModifier(const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* BP = nullptr;
	UGameplayEffect* GE = nullptr;
	FString AssetPath;
	FMonolithActionResult Err;
	if (!LoadGEFromParams(Params, BP, GE, AssetPath, Err)) return Err;

	// Parse attribute
	FString AttrStr;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("attribute"), AttrStr, Err)) return Err;

	FString AttrError;
	FGameplayAttribute Attr = ParseAttribute(AttrStr, AttrError);
	if (!AttrError.IsEmpty())
	{
		return FMonolithActionResult::Error(AttrError);
	}

	// Parse operation
	FString OpStr;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("operation"), OpStr, Err)) return Err;

	EGameplayModOp::Type Op;
	FString OpError;
	if (!ParseModifierOp(OpStr, Op, OpError))
	{
		return FMonolithActionResult::Error(OpError);
	}

	// Parse magnitude
	const TSharedPtr<FJsonObject>* MagObjPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("magnitude"), MagObjPtr) || !MagObjPtr || !(*MagObjPtr).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: magnitude (object)"));
	}

	FGameplayEffectModifierMagnitude Magnitude;
	FString MagError;
	if (!ConfigureMagnitude(*MagObjPtr, Magnitude, MagError))
	{
		return FMonolithActionResult::Error(MagError);
	}

	// Build the modifier
	FGameplayModifierInfo NewMod;
	NewMod.Attribute = Attr;
	NewMod.ModifierOp = Op;
	NewMod.ModifierMagnitude = Magnitude;

	GE->Modifiers.Add(NewMod);
	MarkModified(BP);

	int32 NewIndex = GE->Modifiers.Num() - 1;

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(AssetPath,
		FString::Printf(TEXT("Added modifier at index %d"), NewIndex));
	Result->SetNumberField(TEXT("modifier_index"), NewIndex);
	Result->SetObjectField(TEXT("modifier"), ModifierToJson(NewMod, NewIndex));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  5. set_modifier
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleSetModifier(const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* BP = nullptr;
	UGameplayEffect* GE = nullptr;
	FString AssetPath;
	FMonolithActionResult Err;
	if (!LoadGEFromParams(Params, BP, GE, AssetPath, Err)) return Err;

	if (!Params->HasField(TEXT("modifier_index")))
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: modifier_index"));
	}
	int32 Index = static_cast<int32>(Params->GetNumberField(TEXT("modifier_index")));

	if (!GE->Modifiers.IsValidIndex(Index))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("modifier_index %d out of range (0-%d)"), Index, GE->Modifiers.Num() - 1));
	}

	FGameplayModifierInfo& Mod = GE->Modifiers[Index];

	// Update attribute if provided
	FString AttrStr = Params->GetStringField(TEXT("attribute"));
	if (!AttrStr.IsEmpty())
	{
		FString AttrError;
		FGameplayAttribute NewAttr = ParseAttribute(AttrStr, AttrError);
		if (!AttrError.IsEmpty())
		{
			return FMonolithActionResult::Error(AttrError);
		}
		Mod.Attribute = NewAttr;
	}

	// Update operation if provided
	FString OpStr = Params->GetStringField(TEXT("operation"));
	if (!OpStr.IsEmpty())
	{
		EGameplayModOp::Type Op;
		FString OpError;
		if (!ParseModifierOp(OpStr, Op, OpError))
		{
			return FMonolithActionResult::Error(OpError);
		}
		Mod.ModifierOp = Op;
	}

	// Update magnitude if provided
	const TSharedPtr<FJsonObject>* MagObjPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("magnitude"), MagObjPtr) && MagObjPtr && (*MagObjPtr).IsValid())
	{
		FGameplayEffectModifierMagnitude NewMag;
		FString MagError;
		if (!ConfigureMagnitude(*MagObjPtr, NewMag, MagError))
		{
			return FMonolithActionResult::Error(MagError);
		}
		Mod.ModifierMagnitude = NewMag;
	}

	MarkModified(BP);

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(AssetPath,
		FString::Printf(TEXT("Updated modifier at index %d"), Index));
	Result->SetObjectField(TEXT("modifier"), ModifierToJson(Mod, Index));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  6. remove_modifier
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleRemoveModifier(const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* BP = nullptr;
	UGameplayEffect* GE = nullptr;
	FString AssetPath;
	FMonolithActionResult Err;
	if (!LoadGEFromParams(Params, BP, GE, AssetPath, Err)) return Err;

	bool bHasIndex = Params->HasField(TEXT("modifier_index"));
	FString AttrStr = Params->GetStringField(TEXT("attribute"));

	if (!bHasIndex && AttrStr.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Either modifier_index or attribute must be specified"));
	}

	int32 RemoveIndex = -1;

	if (bHasIndex)
	{
		RemoveIndex = static_cast<int32>(Params->GetNumberField(TEXT("modifier_index")));
	}
	else
	{
		// Find by attribute
		FString AttrError;
		FGameplayAttribute TargetAttr = ParseAttribute(AttrStr, AttrError);
		if (!AttrError.IsEmpty())
		{
			return FMonolithActionResult::Error(AttrError);
		}

		for (int32 i = 0; i < GE->Modifiers.Num(); ++i)
		{
			if (GE->Modifiers[i].Attribute == TargetAttr)
			{
				RemoveIndex = i;
				break;
			}
		}

		if (RemoveIndex < 0)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("No modifier found for attribute: %s"), *AttrStr));
		}
	}

	if (!GE->Modifiers.IsValidIndex(RemoveIndex))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("modifier_index %d out of range (0-%d)"), RemoveIndex, GE->Modifiers.Num() - 1));
	}

	FString RemovedAttr = AttributeToString(GE->Modifiers[RemoveIndex].Attribute);
	GE->Modifiers.RemoveAt(RemoveIndex);
	MarkModified(BP);

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(AssetPath,
		FString::Printf(TEXT("Removed modifier at index %d (%s)"), RemoveIndex, *RemovedAttr));
	Result->SetNumberField(TEXT("remaining_count"), GE->Modifiers.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  7. list_modifiers
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleListModifiers(const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* BP = nullptr;
	UGameplayEffect* GE = nullptr;
	FString AssetPath;
	FMonolithActionResult Err;
	if (!LoadGEFromParams(Params, BP, GE, AssetPath, Err)) return Err;

	TArray<TSharedPtr<FJsonValue>> ModArr;
	for (int32 i = 0; i < GE->Modifiers.Num(); ++i)
	{
		ModArr.Add(MakeShared<FJsonValueObject>(ModifierToJson(GE->Modifiers[i], i)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetArrayField(TEXT("modifiers"), ModArr);
	Result->SetNumberField(TEXT("count"), ModArr.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  8. add_ge_component
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleAddGEComponent(const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* BP = nullptr;
	UGameplayEffect* GE = nullptr;
	FString AssetPath;
	FMonolithActionResult Err;
	if (!LoadGEFromParams(Params, BP, GE, AssetPath, Err)) return Err;

	FString ComponentType;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("component_type"), ComponentType, Err)) return Err;

	const TSharedPtr<FJsonObject>* ConfigPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("config"), ConfigPtr) || !ConfigPtr || !(*ConfigPtr).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: config (object)"));
	}
	const TSharedPtr<FJsonObject>& Config = *ConfigPtr;

	FString ClassError;
	UClass* CompClass = ResolveComponentClass(ComponentType, ClassError);
	if (!CompClass)
	{
		return FMonolithActionResult::Error(ClassError);
	}

	// F.7b — accumulate per-field skipped tag strings across the if/else chain so we can surface
	// "warnings" on the response below. Declared up here so all component-type branches can use them.
	TArray<FString> SkippedAppRequire, SkippedAppIgnore;
	TArray<FString> SkippedOngoingRequire, SkippedOngoingIgnore;
	TArray<FString> SkippedRemovalRequire, SkippedRemovalIgnore;

	// Create the component as a subobject of the GE CDO using the same pattern as UGameplayEffect::AddComponent<T>()
	UGameplayEffectComponent* NewComp = NewObject<UGameplayEffectComponent>(GE, CompClass, NAME_None,
		GE->GetMaskedFlags(RF_PropagateToSubObjects) | RF_Transactional);
	if (!NewComp)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create component of type: %s"), *ComponentType));
	}

	// Add to the GEComponents array via the UPROPERTY (since the array is protected)
	FArrayProperty* ArrayProp = CastField<FArrayProperty>(UGameplayEffect::StaticClass()->FindPropertyByName(TEXT("GEComponents")));
	if (ArrayProp)
	{
		FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(GE));
		ArrayHelper.AddValue();
		FObjectProperty* InnerProp = CastField<FObjectProperty>(ArrayProp->Inner);
		if (InnerProp)
		{
			InnerProp->SetObjectPropertyValue(ArrayHelper.GetRawPtr(ArrayHelper.Num() - 1), NewComp);
		}
	}

	// Configure based on type
	if (ComponentType == TEXT("asset_tags"))
	{
		UAssetTagsGameplayEffectComponent* Comp = CastChecked<UAssetTagsGameplayEffectComponent>(NewComp);
		FInheritedTagContainer Tags;
		ApplyTagConfig(Config, Tags);
		Comp->SetAndApplyAssetTagChanges(Tags);
	}
	else if (ComponentType == TEXT("target_tags"))
	{
		UTargetTagsGameplayEffectComponent* Comp = CastChecked<UTargetTagsGameplayEffectComponent>(NewComp);
		FInheritedTagContainer Tags;
		ApplyTagConfig(Config, Tags);
		Comp->SetAndApplyTargetTagChanges(Tags);
	}
	else if (ComponentType == TEXT("block_abilities"))
	{
		UBlockAbilityTagsGameplayEffectComponent* Comp = CastChecked<UBlockAbilityTagsGameplayEffectComponent>(NewComp);
		FInheritedTagContainer Tags;
		ApplyTagConfig(Config, Tags);
		Comp->SetAndApplyBlockedAbilityTagChanges(Tags);
	}
	else if (ComponentType == TEXT("cancel_abilities"))
	{
		UCancelAbilityTagsGameplayEffectComponent* Comp = CastChecked<UCancelAbilityTagsGameplayEffectComponent>(NewComp);
		FInheritedTagContainer WithTags;
		FInheritedTagContainer WithoutTags;
		ApplyTagConfig(Config, WithTags);
		// Support "without_tags" as a secondary config
		const TSharedPtr<FJsonObject>* WithoutConfigPtr = nullptr;
		if (Config->TryGetObjectField(TEXT("without_tags_config"), WithoutConfigPtr) && WithoutConfigPtr)
		{
			ApplyTagConfig(*WithoutConfigPtr, WithoutTags);
		}
		Comp->SetAndApplyCanceledAbilityTagChanges(WithTags, WithoutTags);
	}
	else if (ComponentType == TEXT("target_tag_requirements"))
	{
		UTargetTagRequirementsGameplayEffectComponent* Comp = CastChecked<UTargetTagRequirementsGameplayEffectComponent>(NewComp);

		// F.7b — collect dropped tags per field, surface as warnings on the response.
		// Application requirements
		Comp->ApplicationTagRequirements.RequireTags = MonolithGAS::ParseTagContainer(Config, TEXT("application_require_tags"), SkippedAppRequire);
		Comp->ApplicationTagRequirements.IgnoreTags  = MonolithGAS::ParseTagContainer(Config, TEXT("application_ignore_tags"),  SkippedAppIgnore);

		// Ongoing requirements
		Comp->OngoingTagRequirements.RequireTags     = MonolithGAS::ParseTagContainer(Config, TEXT("ongoing_require_tags"),     SkippedOngoingRequire);
		Comp->OngoingTagRequirements.IgnoreTags      = MonolithGAS::ParseTagContainer(Config, TEXT("ongoing_ignore_tags"),      SkippedOngoingIgnore);

		// Removal requirements
		Comp->RemovalTagRequirements.RequireTags     = MonolithGAS::ParseTagContainer(Config, TEXT("removal_require_tags"),     SkippedRemovalRequire);
		Comp->RemovalTagRequirements.IgnoreTags      = MonolithGAS::ParseTagContainer(Config, TEXT("removal_ignore_tags"),      SkippedRemovalIgnore);
	}
	else if (ComponentType == TEXT("chance_to_apply"))
	{
		UChanceToApplyGameplayEffectComponent* Comp = CastChecked<UChanceToApplyGameplayEffectComponent>(NewComp);
		float Chance = 1.0f;
		if (Config->HasField(TEXT("chance")))
		{
			Chance = Config->GetNumberField(TEXT("chance"));
		}
		Comp->SetChanceToApplyToTarget(FScalableFloat(Chance));
	}
	else if (ComponentType == TEXT("additional_effects"))
	{
		UAdditionalEffectsGameplayEffectComponent* Comp = CastChecked<UAdditionalEffectsGameplayEffectComponent>(NewComp);
		if (Config->HasField(TEXT("copy_data_from_original_spec")))
		{
			Comp->bOnApplicationCopyDataFromOriginalSpec = Config->GetBoolField(TEXT("copy_data_from_original_spec"));
		}
		// Note: actual effect references require asset path resolution which is complex.
		// This sets up the component skeleton; effects are added via subsequent calls or BP editing.
	}
	else if (ComponentType == TEXT("grant_abilities"))
	{
		// Skeleton setup — ability configs added via BP or subsequent calls
		UAbilitiesGameplayEffectComponent* Comp = CastChecked<UAbilitiesGameplayEffectComponent>(NewComp);
		(void)Comp; // configured later
	}
	// immunity, remove_other, custom_can_apply: skeleton creation is enough, detailed config via BP

	MarkModified(BP);

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(AssetPath,
		FString::Printf(TEXT("Added %s component"), *ComponentType));
	Result->SetStringField(TEXT("component_type"), ComponentType);
	Result->SetNumberField(TEXT("component_count"), GetGEComponentCount(GE));

	// F.7b — surface dropped tag strings as a "warnings" array on the response.
	{
		TArray<TSharedPtr<FJsonValue>> Warnings;
		auto AppendSkipped = [&](const TCHAR* FieldName, const TArray<FString>& Skipped)
		{
			for (const FString& T : Skipped)
			{
				Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
					TEXT("%s '%s' is not a registered GameplayTag — dropped"), FieldName, *T)));
			}
		};
		AppendSkipped(TEXT("application_require_tags"), SkippedAppRequire);
		AppendSkipped(TEXT("application_ignore_tags"),  SkippedAppIgnore);
		AppendSkipped(TEXT("ongoing_require_tags"),     SkippedOngoingRequire);
		AppendSkipped(TEXT("ongoing_ignore_tags"),      SkippedOngoingIgnore);
		AppendSkipped(TEXT("removal_require_tags"),     SkippedRemovalRequire);
		AppendSkipped(TEXT("removal_ignore_tags"),      SkippedRemovalIgnore);
		if (Warnings.Num() > 0)
		{
			Result->SetArrayField(TEXT("warnings"), Warnings);
		}
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  9. set_ge_component
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleSetGEComponent(const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* BP = nullptr;
	UGameplayEffect* GE = nullptr;
	FString AssetPath;
	FMonolithActionResult Err;
	if (!LoadGEFromParams(Params, BP, GE, AssetPath, Err)) return Err;

	FString ComponentType;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("component_type"), ComponentType, Err)) return Err;

	const TSharedPtr<FJsonObject>* ConfigPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("config"), ConfigPtr) || !ConfigPtr || !(*ConfigPtr).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: config (object)"));
	}
	const TSharedPtr<FJsonObject>& Config = *ConfigPtr;

	FString ClassError;
	UClass* CompClass = ResolveComponentClass(ComponentType, ClassError);
	if (!CompClass)
	{
		return FMonolithActionResult::Error(ClassError);
	}

	// F.7b — accumulate per-field skipped tag strings across the target_tag_requirements branch.
	TArray<FString> SkippedAppRequire, SkippedAppIgnore;
	TArray<FString> SkippedOngoingRequire, SkippedOngoingIgnore;
	TArray<FString> SkippedRemovalRequire, SkippedRemovalIgnore;

	// Find existing component of this type
	int32 TargetIndex = 0;
	if (Params->HasField(TEXT("index")))
	{
		TargetIndex = static_cast<int32>(Params->GetNumberField(TEXT("index")));
	}

	int32 FoundCount = 0;
	UGameplayEffectComponent* FoundComp = nullptr;
	for (UGameplayEffectComponent* Comp : GetGEComponents(GE))
	{
		if (Comp && Comp->IsA(CompClass))
		{
			if (FoundCount == TargetIndex)
			{
				FoundComp = Comp;
				break;
			}
			++FoundCount;
		}
	}

	if (!FoundComp)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("No %s component found at index %d"), *ComponentType, TargetIndex));
	}

	// Re-configure
	if (ComponentType == TEXT("asset_tags"))
	{
		UAssetTagsGameplayEffectComponent* Comp = CastChecked<UAssetTagsGameplayEffectComponent>(FoundComp);
		FInheritedTagContainer Tags;
		ApplyTagConfig(Config, Tags);
		Comp->SetAndApplyAssetTagChanges(Tags);
	}
	else if (ComponentType == TEXT("target_tags"))
	{
		UTargetTagsGameplayEffectComponent* Comp = CastChecked<UTargetTagsGameplayEffectComponent>(FoundComp);
		FInheritedTagContainer Tags;
		ApplyTagConfig(Config, Tags);
		Comp->SetAndApplyTargetTagChanges(Tags);
	}
	else if (ComponentType == TEXT("block_abilities"))
	{
		UBlockAbilityTagsGameplayEffectComponent* Comp = CastChecked<UBlockAbilityTagsGameplayEffectComponent>(FoundComp);
		FInheritedTagContainer Tags;
		ApplyTagConfig(Config, Tags);
		Comp->SetAndApplyBlockedAbilityTagChanges(Tags);
	}
	else if (ComponentType == TEXT("cancel_abilities"))
	{
		UCancelAbilityTagsGameplayEffectComponent* Comp = CastChecked<UCancelAbilityTagsGameplayEffectComponent>(FoundComp);
		FInheritedTagContainer WithTags;
		FInheritedTagContainer WithoutTags;
		ApplyTagConfig(Config, WithTags);
		const TSharedPtr<FJsonObject>* WithoutConfigPtr = nullptr;
		if (Config->TryGetObjectField(TEXT("without_tags_config"), WithoutConfigPtr) && WithoutConfigPtr)
		{
			ApplyTagConfig(*WithoutConfigPtr, WithoutTags);
		}
		Comp->SetAndApplyCanceledAbilityTagChanges(WithTags, WithoutTags);
	}
	else if (ComponentType == TEXT("target_tag_requirements"))
	{
		UTargetTagRequirementsGameplayEffectComponent* Comp = CastChecked<UTargetTagRequirementsGameplayEffectComponent>(FoundComp);
		// F.7b — collect dropped tags per field, surface as warnings on the response.
		if (Config->HasField(TEXT("application_require_tags")))
			Comp->ApplicationTagRequirements.RequireTags = MonolithGAS::ParseTagContainer(Config, TEXT("application_require_tags"), SkippedAppRequire);
		if (Config->HasField(TEXT("application_ignore_tags")))
			Comp->ApplicationTagRequirements.IgnoreTags  = MonolithGAS::ParseTagContainer(Config, TEXT("application_ignore_tags"),  SkippedAppIgnore);
		if (Config->HasField(TEXT("ongoing_require_tags")))
			Comp->OngoingTagRequirements.RequireTags     = MonolithGAS::ParseTagContainer(Config, TEXT("ongoing_require_tags"),     SkippedOngoingRequire);
		if (Config->HasField(TEXT("ongoing_ignore_tags")))
			Comp->OngoingTagRequirements.IgnoreTags      = MonolithGAS::ParseTagContainer(Config, TEXT("ongoing_ignore_tags"),      SkippedOngoingIgnore);
		if (Config->HasField(TEXT("removal_require_tags")))
			Comp->RemovalTagRequirements.RequireTags     = MonolithGAS::ParseTagContainer(Config, TEXT("removal_require_tags"),     SkippedRemovalRequire);
		if (Config->HasField(TEXT("removal_ignore_tags")))
			Comp->RemovalTagRequirements.IgnoreTags      = MonolithGAS::ParseTagContainer(Config, TEXT("removal_ignore_tags"),      SkippedRemovalIgnore);
	}
	else if (ComponentType == TEXT("chance_to_apply"))
	{
		UChanceToApplyGameplayEffectComponent* Comp = CastChecked<UChanceToApplyGameplayEffectComponent>(FoundComp);
		if (Config->HasField(TEXT("chance")))
		{
			Comp->SetChanceToApplyToTarget(FScalableFloat(Config->GetNumberField(TEXT("chance"))));
		}
	}
	else if (ComponentType == TEXT("additional_effects"))
	{
		UAdditionalEffectsGameplayEffectComponent* Comp = CastChecked<UAdditionalEffectsGameplayEffectComponent>(FoundComp);
		if (Config->HasField(TEXT("copy_data_from_original_spec")))
		{
			Comp->bOnApplicationCopyDataFromOriginalSpec = Config->GetBoolField(TEXT("copy_data_from_original_spec"));
		}
	}

	MarkModified(BP);

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(AssetPath,
		FString::Printf(TEXT("Updated %s component at index %d"), *ComponentType, TargetIndex));

	// F.7b — surface dropped tag strings as a "warnings" array on the response.
	{
		TArray<TSharedPtr<FJsonValue>> Warnings;
		auto AppendSkipped = [&](const TCHAR* FieldName, const TArray<FString>& Skipped)
		{
			for (const FString& T : Skipped)
			{
				Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
					TEXT("%s '%s' is not a registered GameplayTag — dropped"), FieldName, *T)));
			}
		};
		AppendSkipped(TEXT("application_require_tags"), SkippedAppRequire);
		AppendSkipped(TEXT("application_ignore_tags"),  SkippedAppIgnore);
		AppendSkipped(TEXT("ongoing_require_tags"),     SkippedOngoingRequire);
		AppendSkipped(TEXT("ongoing_ignore_tags"),      SkippedOngoingIgnore);
		AppendSkipped(TEXT("removal_require_tags"),     SkippedRemovalRequire);
		AppendSkipped(TEXT("removal_ignore_tags"),      SkippedRemovalIgnore);
		if (Warnings.Num() > 0)
		{
			Result->SetArrayField(TEXT("warnings"), Warnings);
		}
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  10. set_effect_stacking
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleSetEffectStacking(const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* BP = nullptr;
	UGameplayEffect* GE = nullptr;
	FString AssetPath;
	FMonolithActionResult Err;
	if (!LoadGEFromParams(Params, BP, GE, AssetPath, Err)) return Err;

	FString StackTypeStr;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("stacking_type"), StackTypeStr, Err)) return Err;

	EGameplayEffectStackingType StackType;
	FString ParseError;
	if (!ParseStackingType(StackTypeStr, StackType, ParseError))
	{
		return FMonolithActionResult::Error(ParseError);
	}

	// Set StackingType via reflection (SetStackingType is not exported)
	SetGameplayEffectStackingTypeValue(GE, StackType);

	if (Params->HasField(TEXT("stack_limit")))
	{
		GE->StackLimitCount = static_cast<int32>(Params->GetNumberField(TEXT("stack_limit")));
	}

	FString DurRefreshStr = Params->GetStringField(TEXT("stack_duration_refresh_policy"));
	if (!DurRefreshStr.IsEmpty())
	{
		if (DurRefreshStr.Equals(TEXT("RefreshOnSuccessfulApplication"), ESearchCase::IgnoreCase))
		{
			GE->StackDurationRefreshPolicy = EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication;
		}
		else if (DurRefreshStr.Equals(TEXT("NeverRefresh"), ESearchCase::IgnoreCase))
		{
			GE->StackDurationRefreshPolicy = EGameplayEffectStackingDurationPolicy::NeverRefresh;
		}
		else
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Invalid stack_duration_refresh_policy: '%s'. Valid: RefreshOnSuccessfulApplication, NeverRefresh"), *DurRefreshStr));
		}
	}

	FString PeriodResetStr = Params->GetStringField(TEXT("stack_period_reset_policy"));
	if (!PeriodResetStr.IsEmpty())
	{
		if (PeriodResetStr.Equals(TEXT("ResetOnSuccessfulApplication"), ESearchCase::IgnoreCase))
		{
			GE->StackPeriodResetPolicy = EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication;
		}
		else if (PeriodResetStr.Equals(TEXT("NeverReset"), ESearchCase::IgnoreCase))
		{
			GE->StackPeriodResetPolicy = EGameplayEffectStackingPeriodPolicy::NeverReset;
		}
		else
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Invalid stack_period_reset_policy: '%s'. Valid: ResetOnSuccessfulApplication, NeverReset"), *PeriodResetStr));
		}
	}

	FString ExpirationStr = Params->GetStringField(TEXT("stack_expiration_policy"));
	if (!ExpirationStr.IsEmpty())
	{
		if (ExpirationStr.Equals(TEXT("ClearEntireStack"), ESearchCase::IgnoreCase))
		{
			GE->StackExpirationPolicy = EGameplayEffectStackingExpirationPolicy::ClearEntireStack;
		}
		else if (ExpirationStr.Equals(TEXT("RemoveSingleStackAndRefreshDuration"), ESearchCase::IgnoreCase))
		{
			GE->StackExpirationPolicy = EGameplayEffectStackingExpirationPolicy::RemoveSingleStackAndRefreshDuration;
		}
		else if (ExpirationStr.Equals(TEXT("RefreshDuration"), ESearchCase::IgnoreCase))
		{
			GE->StackExpirationPolicy = EGameplayEffectStackingExpirationPolicy::RefreshDuration;
		}
		else
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Invalid stack_expiration_policy: '%s'. Valid: ClearEntireStack, RemoveSingleStackAndRefreshDuration, RefreshDuration"), *ExpirationStr));
		}
	}

	MarkModified(BP);

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(AssetPath, TEXT("Stacking configuration updated"));
	Result->SetStringField(TEXT("stacking_type"), StackTypeStr);
	if (Params->HasField(TEXT("stack_limit")))
	{
		Result->SetNumberField(TEXT("stack_limit"), GE->StackLimitCount);
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  11. set_duration
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleSetDuration(const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* BP = nullptr;
	UGameplayEffect* GE = nullptr;
	FString AssetPath;
	FMonolithActionResult Err;
	if (!LoadGEFromParams(Params, BP, GE, AssetPath, Err)) return Err;

	FString DurationStr;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("duration_policy"), DurationStr, Err)) return Err;

	EGameplayEffectDurationType DurationPolicy;
	FString ParseError;
	if (!ParseDurationPolicy(DurationStr, DurationPolicy, ParseError))
	{
		return FMonolithActionResult::Error(ParseError);
	}

	GE->DurationPolicy = DurationPolicy;

	// Set duration magnitude for HasDuration
	if (DurationPolicy == EGameplayEffectDurationType::HasDuration && Params->HasField(TEXT("duration_magnitude")))
	{
		float DurationValue = Params->GetNumberField(TEXT("duration_magnitude"));
		GE->DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DurationValue));
	}

	MarkModified(BP);

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(AssetPath, TEXT("Duration updated"));
	Result->SetStringField(TEXT("duration_policy"), DurationPolicyToString(DurationPolicy));
	if (DurationPolicy == EGameplayEffectDurationType::HasDuration && Params->HasField(TEXT("duration_magnitude")))
	{
		Result->SetNumberField(TEXT("duration_magnitude"), Params->GetNumberField(TEXT("duration_magnitude")));
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  12. set_period
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleSetPeriod(const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* BP = nullptr;
	UGameplayEffect* GE = nullptr;
	FString AssetPath;
	FMonolithActionResult Err;
	if (!LoadGEFromParams(Params, BP, GE, AssetPath, Err)) return Err;

	if (!Params->HasField(TEXT("period")))
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: period"));
	}

	float PeriodValue = Params->GetNumberField(TEXT("period"));
	GE->Period = FScalableFloat(PeriodValue);

	if (Params->HasField(TEXT("execute_on_application")))
	{
		GE->bExecutePeriodicEffectOnApplication = Params->GetBoolField(TEXT("execute_on_application"));
	}

	MarkModified(BP);

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(AssetPath, TEXT("Period updated"));
	Result->SetNumberField(TEXT("period"), PeriodValue);
	Result->SetBoolField(TEXT("execute_on_application"), GE->bExecutePeriodicEffectOnApplication);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 2: Effect Template Definitions
// ============================================================

namespace
{
	struct FEffectModifierDef
	{
		FString Attribute;
		FString Operation;
		FString MagnitudeType;
		float Value;
		FString SetByCallerTag;
	};

	struct FEffectTemplateDef
	{
		FString DurationPolicy;
		float DurationMagnitude;
		float Period;
		bool bExecuteOnApplication;
		TArray<FEffectModifierDef> Modifiers;
		TArray<FString> AssetTags;
		TArray<FString> TargetTags;
		TArray<FString> RemoveEffectsWithTags;
		FString StackingType;
		int32 StackLimit;
	};

	bool GetEffectTemplate(const FString& TemplateName, FEffectTemplateDef& OutDef, FString& OutError)
	{
		OutDef = FEffectTemplateDef();
		OutDef.DurationMagnitude = 0.f;
		OutDef.Period = 0.f;
		OutDef.bExecuteOnApplication = false;
		OutDef.StackingType = TEXT("none");
		OutDef.StackLimit = 0;

		if (TemplateName == TEXT("instant_damage"))
		{
			OutDef.DurationPolicy = TEXT("instant");
			OutDef.Modifiers.Add({TEXT(""), TEXT("Add"), TEXT("set_by_caller"), 0.f, TEXT("SetByCaller.Damage.Base")});
			OutDef.AssetTags.Add(TEXT("Damage.Type.Generic"));
		}
		else if (TemplateName == TEXT("dot_damage"))
		{
			OutDef.DurationPolicy = TEXT("has_duration");
			OutDef.DurationMagnitude = 5.f;
			OutDef.Period = 1.f;
			OutDef.bExecuteOnApplication = false;
			OutDef.Modifiers.Add({TEXT(""), TEXT("Add"), TEXT("scalable_float"), -5.f, TEXT("")});
			OutDef.AssetTags.Add(TEXT("Status.DOT"));
			OutDef.StackingType = TEXT("aggregate_by_source");
			OutDef.StackLimit = 5;
		}
		else if (TemplateName == TEXT("heal_instant"))
		{
			OutDef.DurationPolicy = TEXT("instant");
			OutDef.Modifiers.Add({TEXT(""), TEXT("Add"), TEXT("set_by_caller"), 0.f, TEXT("SetByCaller.Heal.Amount")});
		}
		else if (TemplateName == TEXT("heal_over_time"))
		{
			OutDef.DurationPolicy = TEXT("has_duration");
			OutDef.DurationMagnitude = 10.f;
			OutDef.Period = 1.f;
			OutDef.bExecuteOnApplication = false;
			OutDef.Modifiers.Add({TEXT(""), TEXT("Add"), TEXT("scalable_float"), 5.f, TEXT("")});
		}
		else if (TemplateName == TEXT("status_burning"))
		{
			OutDef.DurationPolicy = TEXT("has_duration");
			OutDef.DurationMagnitude = 5.f;
			OutDef.Period = 1.f;
			OutDef.bExecuteOnApplication = true;
			OutDef.Modifiers.Add({TEXT(""), TEXT("Add"), TEXT("scalable_float"), -3.f, TEXT("")});
			OutDef.AssetTags.Add(TEXT("Status.Burning"));
			OutDef.TargetTags.Add(TEXT("Status.Burning"));
			OutDef.RemoveEffectsWithTags.Add(TEXT("Status.Frozen"));
			OutDef.StackingType = TEXT("aggregate_by_target");
			OutDef.StackLimit = 5;
		}
		else if (TemplateName == TEXT("status_frozen"))
		{
			OutDef.DurationPolicy = TEXT("has_duration");
			OutDef.DurationMagnitude = 3.f;
			OutDef.Modifiers.Add({TEXT(""), TEXT("Multiply"), TEXT("scalable_float"), 0.f, TEXT("")});
			OutDef.AssetTags.Add(TEXT("Status.Frozen"));
			OutDef.TargetTags.Add(TEXT("Status.Frozen"));
			OutDef.RemoveEffectsWithTags.Add(TEXT("Status.Burning"));
		}
		else if (TemplateName == TEXT("status_bleeding"))
		{
			OutDef.DurationPolicy = TEXT("has_duration");
			OutDef.DurationMagnitude = 10.f;
			OutDef.Period = 2.f;
			OutDef.bExecuteOnApplication = false;
			OutDef.Modifiers.Add({TEXT(""), TEXT("Add"), TEXT("scalable_float"), -2.f, TEXT("")});
			OutDef.AssetTags.Add(TEXT("Status.Bleeding"));
			OutDef.TargetTags.Add(TEXT("Status.Bleeding"));
			OutDef.StackingType = TEXT("aggregate_by_target");
			OutDef.StackLimit = 10;
		}
		else if (TemplateName == TEXT("status_poisoned"))
		{
			OutDef.DurationPolicy = TEXT("has_duration");
			OutDef.DurationMagnitude = 8.f;
			OutDef.Period = 3.f;
			OutDef.bExecuteOnApplication = false;
			OutDef.Modifiers.Add({TEXT(""), TEXT("Add"), TEXT("scalable_float"), -4.f, TEXT("")});
			OutDef.AssetTags.Add(TEXT("Status.Poisoned"));
			// ExecCalc should check for this tag to skip shield reduction
			OutDef.AssetTags.Add(TEXT("Status.Poisoned.BypassShield"));
			OutDef.TargetTags.Add(TEXT("Status.Poisoned"));
		}
		else if (TemplateName == TEXT("fear_spike"))
		{
			// Two-phase: instant spike (applied on application) + periodic decay
			OutDef.DurationPolicy = TEXT("has_duration");
			OutDef.DurationMagnitude = 5.f;
			OutDef.Period = 1.f;
			OutDef.bExecuteOnApplication = true;
			// Modifier 1: Instant fear spike via SetByCaller (applied on application)
			OutDef.Modifiers.Add({TEXT(""), TEXT("Add"), TEXT("set_by_caller"), 0.f, TEXT("SetByCaller.Horror.FearAmount")});
			// Modifier 2: Periodic decay each second
			OutDef.Modifiers.Add({TEXT(""), TEXT("Add"), TEXT("scalable_float"), -5.f, TEXT("")});
			OutDef.AssetTags.Add(TEXT("State.Horror.FearHigh"));
		}
		else if (TemplateName == TEXT("sanity_drain"))
		{
			OutDef.DurationPolicy = TEXT("has_duration");
			OutDef.DurationMagnitude = 30.f;
			OutDef.Period = 1.f;
			OutDef.bExecuteOnApplication = false;
			OutDef.Modifiers.Add({TEXT(""), TEXT("Add"), TEXT("scalable_float"), -1.f, TEXT("")});
		}
		else if (TemplateName == TEXT("cooldown"))
		{
			OutDef.DurationPolicy = TEXT("has_duration");
			OutDef.DurationMagnitude = 1.f;
			OutDef.AssetTags.Add(TEXT("Cooldown.Ability.Generic"));
		}
		else if (TemplateName == TEXT("stamina_cost"))
		{
			OutDef.DurationPolicy = TEXT("instant");
			OutDef.Modifiers.Add({TEXT(""), TEXT("Add"), TEXT("set_by_caller"), 0.f, TEXT("SetByCaller.Stamina.Cost")});
		}
		else if (TemplateName == TEXT("difficulty_preset"))
		{
			OutDef.DurationPolicy = TEXT("infinite");
			// Damage multiplier (agents override per difficulty level)
			OutDef.Modifiers.Add({TEXT("IncomingDamage"), TEXT("Multiply"), TEXT("scalable_float"), 1.f, TEXT("")});
			// Stamina regen multiplier
			OutDef.Modifiers.Add({TEXT("StaminaRegenRate"), TEXT("Multiply"), TEXT("scalable_float"), 1.f, TEXT("")});
			// Horror intensity scalar
			OutDef.Modifiers.Add({TEXT("HorrorIntensity"), TEXT("Multiply"), TEXT("scalable_float"), 1.f, TEXT("")});
		}
		else if (TemplateName == TEXT("accessibility_override"))
		{
			OutDef.DurationPolicy = TEXT("infinite");
			OutDef.AssetTags.Add(TEXT("State.Accessibility.Active"));
			// Disable horror effects for hospice patients
			OutDef.Modifiers.Add({TEXT("HorrorIntensity"), TEXT("Override"), TEXT("scalable_float"), 0.f, TEXT("")});
			// 90% damage reduction
			OutDef.Modifiers.Add({TEXT("DamageResistance"), TEXT("Override"), TEXT("scalable_float"), 0.9f, TEXT("")});
			// Fear immunity
			OutDef.Modifiers.Add({TEXT("FearResistance"), TEXT("Override"), TEXT("scalable_float"), 100.f, TEXT("")});
			OutDef.AssetTags.Add(TEXT("State.Accessibility.HorrorReduced"));
			OutDef.AssetTags.Add(TEXT("State.Accessibility.DamageReduced"));
		}
		else if (TemplateName == TEXT("init_player_stats"))
		{
			OutDef.DurationPolicy = TEXT("instant");
			// Vitals
			OutDef.Modifiers.Add({TEXT("Health"), TEXT("Override"), TEXT("scalable_float"), 100.f, TEXT("")});
			OutDef.Modifiers.Add({TEXT("MaxHealth"), TEXT("Override"), TEXT("scalable_float"), 100.f, TEXT("")});
			OutDef.Modifiers.Add({TEXT("Shield"), TEXT("Override"), TEXT("scalable_float"), 50.f, TEXT("")});
			OutDef.Modifiers.Add({TEXT("MaxShield"), TEXT("Override"), TEXT("scalable_float"), 50.f, TEXT("")});
			OutDef.Modifiers.Add({TEXT("Armor"), TEXT("Override"), TEXT("scalable_float"), 0.f, TEXT("")});
			// Stamina
			OutDef.Modifiers.Add({TEXT("Stamina"), TEXT("Override"), TEXT("scalable_float"), 100.f, TEXT("")});
			OutDef.Modifiers.Add({TEXT("MaxStamina"), TEXT("Override"), TEXT("scalable_float"), 100.f, TEXT("")});
			OutDef.Modifiers.Add({TEXT("StaminaRegenRate"), TEXT("Override"), TEXT("scalable_float"), 10.f, TEXT("")});
			// Horror
			OutDef.Modifiers.Add({TEXT("Sanity"), TEXT("Override"), TEXT("scalable_float"), 100.f, TEXT("")});
			OutDef.Modifiers.Add({TEXT("Fear"), TEXT("Override"), TEXT("scalable_float"), 0.f, TEXT("")});
			OutDef.Modifiers.Add({TEXT("HeartRate"), TEXT("Override"), TEXT("scalable_float"), 70.f, TEXT("")});
			OutDef.Modifiers.Add({TEXT("Paranoia"), TEXT("Override"), TEXT("scalable_float"), 0.f, TEXT("")});
			OutDef.Modifiers.Add({TEXT("FearResistance"), TEXT("Override"), TEXT("scalable_float"), 0.f, TEXT("")});
			OutDef.Modifiers.Add({TEXT("HorrorIntensity"), TEXT("Override"), TEXT("scalable_float"), 0.f, TEXT("")});
		}
		else if (TemplateName == TEXT("init_enemy_stats"))
		{
			// Values should be scaled by level via curve tables or SetByCaller overrides
			OutDef.DurationPolicy = TEXT("instant");
			OutDef.Modifiers.Add({TEXT("Health"), TEXT("Override"), TEXT("scalable_float"), 100.f, TEXT("")});
			OutDef.Modifiers.Add({TEXT("MaxHealth"), TEXT("Override"), TEXT("scalable_float"), 100.f, TEXT("")});
			OutDef.Modifiers.Add({TEXT("StaggerHealth"), TEXT("Override"), TEXT("scalable_float"), 50.f, TEXT("")});
			OutDef.Modifiers.Add({TEXT("MaxStaggerHealth"), TEXT("Override"), TEXT("scalable_float"), 50.f, TEXT("")});
		}
		else
		{
			OutError = FString::Printf(
				TEXT("Unknown effect template: '%s'. Valid: instant_damage, dot_damage, heal_instant, heal_over_time, "
				     "status_burning, status_frozen, status_bleeding, status_poisoned, fear_spike, sanity_drain, "
				     "cooldown, stamina_cost, difficulty_preset, accessibility_override, init_player_stats, init_enemy_stats"),
				*TemplateName);
			return false;
		}

		return true;
	}

	/** Helper: Create a GE Blueprint, set duration, compile, and return the BP + CDO. */
	UBlueprint* CreateGEBlueprint(const FString& SavePath, EGameplayEffectDurationType DurationPolicy,
		UGameplayEffect*& OutGE, FMonolithActionResult& OutError)
	{
		int32 LastSlash;
		if (!SavePath.FindLastChar(TEXT('/'), LastSlash))
		{
			OutError = FMonolithActionResult::Error(FString::Printf(TEXT("Invalid save_path: %s"), *SavePath));
			return nullptr;
		}
		FString AssetName = SavePath.Mid(LastSlash + 1);
		if (AssetName.IsEmpty())
		{
			OutError = FMonolithActionResult::Error(TEXT("save_path must not end with '/'"));
			return nullptr;
		}

		// Check for existing asset (AssetRegistry + in-memory multi-tier check)
		FString ExistError;
		if (!MonolithGAS::EnsureAssetPathFree(SavePath, AssetName, ExistError))
		{
			OutError = FMonolithActionResult::Error(ExistError);
			return nullptr;
		}

		UPackage* Package = CreatePackage(*SavePath);
		if (!Package)
		{
			OutError = FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *SavePath));
			return nullptr;
		}
		Package->FullyLoad();

		UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
			UGameplayEffect::StaticClass(),
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());

		if (!NewBP)
		{
			OutError = FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create GE Blueprint at: %s"), *SavePath));
			return nullptr;
		}

		OutGE = MonolithGAS::GetBlueprintCDO<UGameplayEffect>(NewBP);
		if (OutGE)
		{
			OutGE->DurationPolicy = DurationPolicy;
		}

		FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::SkipGarbageCollection);
		return NewBP;
	}

	/** Helper: Save a GE Blueprint package to disk. */
	bool SaveGEPackage(UBlueprint* BP, const FString& SavePath)
	{
		UPackage* Package = BP->GetPackage();
		Package->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(BP);

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		FSavePackageResultStruct SaveResult = UPackage::Save(Package, BP,
			*FPackageName::LongPackageNameToFilename(SavePath, FPackageName::GetAssetPackageExtension()), SaveArgs);
		return SaveResult.Result == ESavePackageResult::Success;
	}

	/** Helper: Add a modifier to a GE CDO from def struct. Attribute string empty = use template's convention. */
	void AddModifierFromDef(UGameplayEffect* GE, const FEffectModifierDef& Def, const FString& FallbackAttribute)
	{
		FGameplayModifierInfo NewMod;

		FString AttrStr = Def.Attribute.IsEmpty() ? FallbackAttribute : Def.Attribute;
		if (!AttrStr.IsEmpty())
		{
			FString AttrError;
			NewMod.Attribute = ParseAttribute(AttrStr, AttrError);
			// If attribute can't be resolved yet (class not compiled), leave it unset
		}

		FString OpError;
		EGameplayModOp::Type TempOp;
		if (ParseModifierOp(Def.Operation, TempOp, OpError)) { NewMod.ModifierOp = TempOp; }

		if (Def.MagnitudeType == TEXT("set_by_caller"))
		{
			FSetByCallerFloat SBC;
			if (!Def.SetByCallerTag.IsEmpty())
			{
				SBC.DataTag = FGameplayTag::RequestGameplayTag(FName(*Def.SetByCallerTag), false);
			}
			NewMod.ModifierMagnitude = FGameplayEffectModifierMagnitude(SBC);
		}
		else
		{
			NewMod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Def.Value));
		}

		GE->Modifiers.Add(NewMod);
	}

	/** Helper: Add asset tags GE component. */
	void AddAssetTagsComponent(UGameplayEffect* GE, const TArray<FString>& Tags)
	{
		if (Tags.Num() == 0) return;

		UAssetTagsGameplayEffectComponent* Comp = NewObject<UAssetTagsGameplayEffectComponent>(GE,
			UAssetTagsGameplayEffectComponent::StaticClass(), NAME_None,
			GE->GetMaskedFlags(RF_PropagateToSubObjects) | RF_Transactional);

		FInheritedTagContainer TagContainer;
		for (const FString& TagStr : Tags)
		{
			FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagStr), false);
			if (Tag.IsValid())
			{
				TagContainer.Added.AddTag(Tag);
			}
		}
		Comp->SetAndApplyAssetTagChanges(TagContainer);

		// Add to GEComponents via reflection
		FArrayProperty* ArrayProp = CastField<FArrayProperty>(UGameplayEffect::StaticClass()->FindPropertyByName(TEXT("GEComponents")));
		if (ArrayProp)
		{
			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(GE));
			ArrayHelper.AddValue();
			FObjectProperty* InnerProp = CastField<FObjectProperty>(ArrayProp->Inner);
			if (InnerProp)
			{
				InnerProp->SetObjectPropertyValue(ArrayHelper.GetRawPtr(ArrayHelper.Num() - 1), Comp);
			}
		}
	}

	/** Helper: Add target tags GE component. */
	void AddTargetTagsComponent(UGameplayEffect* GE, const TArray<FString>& Tags)
	{
		if (Tags.Num() == 0) return;

		UTargetTagsGameplayEffectComponent* Comp = NewObject<UTargetTagsGameplayEffectComponent>(GE,
			UTargetTagsGameplayEffectComponent::StaticClass(), NAME_None,
			GE->GetMaskedFlags(RF_PropagateToSubObjects) | RF_Transactional);

		FInheritedTagContainer TagContainer;
		for (const FString& TagStr : Tags)
		{
			FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagStr), false);
			if (Tag.IsValid())
			{
				TagContainer.Added.AddTag(Tag);
			}
		}
		Comp->SetAndApplyTargetTagChanges(TagContainer);

		FArrayProperty* ArrayProp = CastField<FArrayProperty>(UGameplayEffect::StaticClass()->FindPropertyByName(TEXT("GEComponents")));
		if (ArrayProp)
		{
			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(GE));
			ArrayHelper.AddValue();
			FObjectProperty* InnerProp = CastField<FObjectProperty>(ArrayProp->Inner);
			if (InnerProp)
			{
				InnerProp->SetObjectPropertyValue(ArrayHelper.GetRawPtr(ArrayHelper.Num() - 1), Comp);
			}
		}
	}

	/** Helper: Add a RemoveOther GE component that removes effects matching given tags. */
	void AddRemoveOtherComponent(UGameplayEffect* GE, const TArray<FString>& Tags)
	{
		if (Tags.Num() == 0) return;

		URemoveOtherGameplayEffectComponent* Comp = NewObject<URemoveOtherGameplayEffectComponent>(GE,
			URemoveOtherGameplayEffectComponent::StaticClass(), NAME_None,
			GE->GetMaskedFlags(RF_PropagateToSubObjects) | RF_Transactional);

		// Build a query to match effects with these asset tags
		FGameplayTagContainer TagContainer;
		for (const FString& TagStr : Tags)
		{
			FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagStr), false);
			if (Tag.IsValid())
			{
				TagContainer.AddTag(Tag);
			}
		}
		FGameplayEffectQuery Query;
		Query.EffectTagQuery = FGameplayTagQuery::MakeQuery_MatchAnyTags(TagContainer);
		Comp->RemoveGameplayEffectQueries.Add(Query);

		FArrayProperty* ArrayProp = CastField<FArrayProperty>(UGameplayEffect::StaticClass()->FindPropertyByName(TEXT("GEComponents")));
		if (ArrayProp)
		{
			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(GE));
			ArrayHelper.AddValue();
			FObjectProperty* InnerProp = CastField<FObjectProperty>(ArrayProp->Inner);
			if (InnerProp)
			{
				InnerProp->SetObjectPropertyValue(ArrayHelper.GetRawPtr(ArrayHelper.Num() - 1), Comp);
			}
		}
	}

	/** Helper: Configure stacking on a GE CDO. */
	void ConfigureStacking(UGameplayEffect* GE, const FString& StackType, int32 StackLimit)
	{
		if (StackType == TEXT("none")) return;

		EGameplayEffectStackingType Type = EGameplayEffectStackingType::None;
		if (StackType == TEXT("aggregate_by_source"))
			Type = EGameplayEffectStackingType::AggregateBySource;
		else if (StackType == TEXT("aggregate_by_target"))
			Type = EGameplayEffectStackingType::AggregateByTarget;

		SetGameplayEffectStackingTypeValue(GE, Type);

		GE->StackLimitCount = StackLimit;
		GE->StackDurationRefreshPolicy = EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication;
		GE->StackPeriodResetPolicy = EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication;
	}
}

// ============================================================
//  13. create_effect_from_template
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleCreateEffectFromTemplate(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, TemplateName;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("save_path"), SavePath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("template"), TemplateName, Err)) return Err;

	TemplateName = TemplateName.ToLower();

	FString TemplateError;
	FEffectTemplateDef Def;
	if (!GetEffectTemplate(TemplateName, Def, TemplateError))
	{
		return FMonolithActionResult::Error(TemplateError);
	}

	// Apply overrides
	const TSharedPtr<FJsonObject>* OverridesPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("overrides"), OverridesPtr) && OverridesPtr && (*OverridesPtr).IsValid())
	{
		const TSharedPtr<FJsonObject>& Ov = *OverridesPtr;
		if (Ov->HasField(TEXT("duration"))) Def.DurationMagnitude = Ov->GetNumberField(TEXT("duration"));
		if (Ov->HasField(TEXT("period"))) Def.Period = Ov->GetNumberField(TEXT("period"));
		if (Ov->HasField(TEXT("magnitude")) && Def.Modifiers.Num() > 0)
		{
			Def.Modifiers[0].Value = Ov->GetNumberField(TEXT("magnitude"));
		}
		FString StackOverride = Ov->GetStringField(TEXT("stacking"));
		if (!StackOverride.IsEmpty()) Def.StackingType = StackOverride;

		const TSharedPtr<FJsonObject>* TagsPtr = nullptr;
		if (Ov->TryGetObjectField(TEXT("tags"), TagsPtr) && TagsPtr && (*TagsPtr).IsValid())
		{
			TArray<FString> ExtraAssetTags = MonolithGAS::ParseStringArray(*TagsPtr, TEXT("asset_tags"));
			Def.AssetTags.Append(ExtraAssetTags);
			TArray<FString> ExtraTargetTags = MonolithGAS::ParseStringArray(*TagsPtr, TEXT("target_tags"));
			Def.TargetTags.Append(ExtraTargetTags);
		}
	}

	// Parse duration policy
	EGameplayEffectDurationType DurationPolicy;
	FString ParseError;
	if (!ParseDurationPolicy(Def.DurationPolicy, DurationPolicy, ParseError))
	{
		return FMonolithActionResult::Error(ParseError);
	}

	// Create the GE
	UGameplayEffect* GE = nullptr;
	FMonolithActionResult CreateError;
	UBlueprint* NewBP = CreateGEBlueprint(SavePath, DurationPolicy, GE, CreateError);
	if (!NewBP)
	{
		return CreateError;
	}

	// Configure duration magnitude
	if (DurationPolicy == EGameplayEffectDurationType::HasDuration && Def.DurationMagnitude > 0.f)
	{
		GE->DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Def.DurationMagnitude));
	}

	// Configure period
	if (Def.Period > 0.f)
	{
		GE->Period = FScalableFloat(Def.Period);
		GE->bExecutePeriodicEffectOnApplication = Def.bExecuteOnApplication;
	}

	// Add modifiers — use empty fallback attribute (templates leave attribute blank when class isn't compiled yet)
	for (const FEffectModifierDef& ModDef : Def.Modifiers)
	{
		AddModifierFromDef(GE, ModDef, TEXT(""));
	}

	// Add tag components
	AddAssetTagsComponent(GE, Def.AssetTags);
	AddTargetTagsComponent(GE, Def.TargetTags);
	AddRemoveOtherComponent(GE, Def.RemoveEffectsWithTags);

	// Configure stacking
	ConfigureStacking(GE, Def.StackingType, Def.StackLimit);

	MarkModified(NewBP);
	bool bSaved = SaveGEPackage(NewBP, SavePath);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetStringField(TEXT("template"), TemplateName);
	Result->SetStringField(TEXT("duration_policy"), Def.DurationPolicy);
	Result->SetNumberField(TEXT("modifier_count"), GE->Modifiers.Num());
	Result->SetNumberField(TEXT("component_count"), GetGEComponentCount(GE));
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Created GameplayEffect from template '%s' with %d modifiers"),
			*TemplateName, GE->Modifiers.Num()));

	// Note about unresolved attributes
	bool bHasUnresolved = false;
	for (const FGameplayModifierInfo& Mod : GE->Modifiers)
	{
		if (!Mod.Attribute.IsValid()) bHasUnresolved = true;
	}
	if (bHasUnresolved)
	{
		Result->SetStringField(TEXT("attribute_note"),
			TEXT("Some modifier attributes are unresolved — assign them after creating the AttributeSet classes via set_modifier."));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  14. build_effect_from_spec
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleBuildEffectFromSpec(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("save_path"), SavePath, Err)) return Err;

	const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("spec"), SpecPtr) || !SpecPtr || !(*SpecPtr).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: spec (object)"));
	}
	const TSharedPtr<FJsonObject>& Spec = *SpecPtr;

	// Duration policy (required in spec)
	FString DurationStr = Spec->GetStringField(TEXT("duration_policy"));
	if (DurationStr.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("spec.duration_policy is required"));
	}

	EGameplayEffectDurationType DurationPolicy;
	FString ParseError;
	if (!ParseDurationPolicy(DurationStr, DurationPolicy, ParseError))
	{
		return FMonolithActionResult::Error(ParseError);
	}

	// Create the GE
	UGameplayEffect* GE = nullptr;
	FMonolithActionResult CreateError;
	UBlueprint* NewBP = CreateGEBlueprint(SavePath, DurationPolicy, GE, CreateError);
	if (!NewBP) return CreateError;

	TArray<TSharedPtr<FJsonValue>> Warnings;

	// Duration magnitude
	if (Spec->HasField(TEXT("duration_magnitude")) && DurationPolicy == EGameplayEffectDurationType::HasDuration)
	{
		GE->DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Spec->GetNumberField(TEXT("duration_magnitude"))));
	}

	// Period
	if (Spec->HasField(TEXT("period")))
	{
		GE->Period = FScalableFloat(Spec->GetNumberField(TEXT("period")));
		if (Spec->HasField(TEXT("execute_on_application")))
		{
			GE->bExecutePeriodicEffectOnApplication = Spec->GetBoolField(TEXT("execute_on_application"));
		}
	}

	// Modifiers
	const TArray<TSharedPtr<FJsonValue>>* ModArray;
	if (Spec->TryGetArrayField(TEXT("modifiers"), ModArray))
	{
		for (int32 i = 0; i < ModArray->Num(); i++)
		{
			const TSharedPtr<FJsonObject>* ModObjPtr;
			if (!(*ModArray)[i]->TryGetObject(ModObjPtr)) continue;
			const TSharedPtr<FJsonObject>& ModObj = *ModObjPtr;

			FGameplayModifierInfo NewMod;

			// Attribute
			FString AttrStr = ModObj->GetStringField(TEXT("attribute"));
			if (!AttrStr.IsEmpty())
			{
				FString AttrError;
				NewMod.Attribute = ParseAttribute(AttrStr, AttrError);
				if (!AttrError.IsEmpty())
				{
					Warnings.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("Modifier %d: %s"), i, *AttrError)));
				}
			}

			// Operation
			FString OpStr = ModObj->GetStringField(TEXT("operation"));
			if (!OpStr.IsEmpty())
			{
				FString OpError;
				EGameplayModOp::Type TempOp;
				if (!ParseModifierOp(OpStr, TempOp, OpError))
				{
					Warnings.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("Modifier %d: %s"), i, *OpError)));
				}
				else { NewMod.ModifierOp = TempOp; }
			}

			// Magnitude
			const TSharedPtr<FJsonObject>* MagObjPtr = nullptr;
			if (ModObj->TryGetObjectField(TEXT("magnitude"), MagObjPtr) && MagObjPtr && (*MagObjPtr).IsValid())
			{
				FString MagError;
				if (!ConfigureMagnitude(*MagObjPtr, NewMod.ModifierMagnitude, MagError))
				{
					Warnings.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("Modifier %d magnitude: %s"), i, *MagError)));
				}
			}
			else if (ModObj->HasField(TEXT("value")))
			{
				// Simple shorthand: just a value
				NewMod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(ModObj->GetNumberField(TEXT("value"))));
			}

			GE->Modifiers.Add(NewMod);
		}
	}

	// Stacking
	const TSharedPtr<FJsonObject>* StackPtr = nullptr;
	if (Spec->TryGetObjectField(TEXT("stacking"), StackPtr) && StackPtr && (*StackPtr).IsValid())
	{
		FString StackType = (*StackPtr)->GetStringField(TEXT("type"));
		int32 StackLimit = 0;
		if ((*StackPtr)->HasField(TEXT("limit")))
		{
			StackLimit = static_cast<int32>((*StackPtr)->GetNumberField(TEXT("limit")));
		}
		ConfigureStacking(GE, StackType, StackLimit);
	}

	// Components
	const TArray<TSharedPtr<FJsonValue>>* CompArray;
	if (Spec->TryGetArrayField(TEXT("components"), CompArray))
	{
		for (const auto& CompVal : *CompArray)
		{
			const TSharedPtr<FJsonObject>* CompObjPtr;
			if (!CompVal->TryGetObject(CompObjPtr)) continue;
			const TSharedPtr<FJsonObject>& CompObj = *CompObjPtr;

			FString CompType = CompObj->GetStringField(TEXT("type"));
			if (CompType.IsEmpty()) continue;

			// Delegate to add_ge_component logic by building params
			TSharedPtr<FJsonObject> CompParams = MakeShared<FJsonObject>();
			CompParams->SetStringField(TEXT("asset_path"), SavePath);
			CompParams->SetStringField(TEXT("component_type"), CompType);

			const TSharedPtr<FJsonObject>* CompConfig = nullptr;
			if (CompObj->TryGetObjectField(TEXT("config"), CompConfig) && CompConfig && (*CompConfig).IsValid())
			{
				CompParams->SetObjectField(TEXT("config"), *CompConfig);
			}
			else
			{
				// Create config from CompObj itself (for convenience)
				CompParams->SetObjectField(TEXT("config"), CompObj);
			}

			// We directly create the component since we already have the GE CDO
			FString ClassError;
			UClass* CompClass = ResolveComponentClass(CompType, ClassError);
			if (!CompClass)
			{
				Warnings.Add(MakeShared<FJsonValueString>(ClassError));
				continue;
			}

			UGameplayEffectComponent* NewComp = NewObject<UGameplayEffectComponent>(GE, CompClass, NAME_None,
				GE->GetMaskedFlags(RF_PropagateToSubObjects) | RF_Transactional);

			FArrayProperty* ArrayProp = CastField<FArrayProperty>(UGameplayEffect::StaticClass()->FindPropertyByName(TEXT("GEComponents")));
			if (ArrayProp && NewComp)
			{
				FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(GE));
				ArrayHelper.AddValue();
				FObjectProperty* InnerProp = CastField<FObjectProperty>(ArrayProp->Inner);
				if (InnerProp)
				{
					InnerProp->SetObjectPropertyValue(ArrayHelper.GetRawPtr(ArrayHelper.Num() - 1), NewComp);
				}
			}
		}
	}

	// Tags shorthand (not via components, just for convenience)
	TArray<FString> AssetTags = MonolithGAS::ParseStringArray(Spec, TEXT("asset_tags"));
	if (AssetTags.Num() > 0) AddAssetTagsComponent(GE, AssetTags);

	TArray<FString> TargetTags = MonolithGAS::ParseStringArray(Spec, TEXT("target_tags"));
	if (TargetTags.Num() > 0) AddTargetTagsComponent(GE, TargetTags);

	MarkModified(NewBP);
	bool bSaved = SaveGEPackage(NewBP, SavePath);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetStringField(TEXT("duration_policy"), DurationStr);
	Result->SetNumberField(TEXT("modifier_count"), GE->Modifiers.Num());
	Result->SetNumberField(TEXT("component_count"), GetGEComponentCount(GE));
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Built GameplayEffect from spec: %d modifiers, %d components"),
			GE->Modifiers.Num(), GetGEComponentCount(GE)));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  15. batch_create_effects
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleBatchCreateEffects(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* EffectsArray;
	if (!Params->TryGetArrayField(TEXT("effects"), EffectsArray) || !EffectsArray || EffectsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: effects (array)"));
	}

	TArray<TSharedPtr<FJsonValue>> Results;
	TArray<TSharedPtr<FJsonValue>> Errors;
	int32 SuccessCount = 0;

	for (int32 i = 0; i < EffectsArray->Num(); i++)
	{
		const TSharedPtr<FJsonObject>* EffObjPtr;
		if (!(*EffectsArray)[i]->TryGetObject(EffObjPtr)) continue;
		const TSharedPtr<FJsonObject>& EffObj = *EffObjPtr;

		FString SavePath = EffObj->GetStringField(TEXT("save_path"));
		if (SavePath.IsEmpty())
		{
			Errors.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Effect %d: missing save_path"), i)));
			continue;
		}

		FString TemplateName = EffObj->GetStringField(TEXT("template"));
		FMonolithActionResult EffResult;

		if (!TemplateName.IsEmpty())
		{
			// Template-based creation
			TSharedPtr<FJsonObject> TemplateParams = MakeShared<FJsonObject>();
			TemplateParams->SetStringField(TEXT("save_path"), SavePath);
			TemplateParams->SetStringField(TEXT("template"), TemplateName);

			const TSharedPtr<FJsonObject>* OverridesPtr = nullptr;
			if (EffObj->TryGetObjectField(TEXT("overrides"), OverridesPtr) && OverridesPtr)
			{
				TemplateParams->SetObjectField(TEXT("overrides"), *OverridesPtr);
			}

			EffResult = HandleCreateEffectFromTemplate(TemplateParams);
		}
		else
		{
			// Spec-based creation
			const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
			if (EffObj->TryGetObjectField(TEXT("spec"), SpecPtr) && SpecPtr && (*SpecPtr).IsValid())
			{
				TSharedPtr<FJsonObject> SpecParams = MakeShared<FJsonObject>();
				SpecParams->SetStringField(TEXT("save_path"), SavePath);
				SpecParams->SetObjectField(TEXT("spec"), *SpecPtr);
				EffResult = HandleBuildEffectFromSpec(SpecParams);
			}
			else
			{
				Errors.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Effect %d (%s): provide either 'template' or 'spec'"), i, *SavePath)));
				continue;
			}
		}

		if (EffResult.bSuccess)
		{
			TSharedPtr<FJsonObject> SuccObj = MakeShared<FJsonObject>();
			SuccObj->SetNumberField(TEXT("index"), i);
			SuccObj->SetStringField(TEXT("asset_path"), SavePath);
			SuccObj->SetBoolField(TEXT("success"), true);
			Results.Add(MakeShared<FJsonValueObject>(SuccObj));
			SuccessCount++;
		}
		else
		{
			Errors.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Effect %d (%s): %s"), i, *SavePath, *EffResult.ErrorMessage)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("total"), EffectsArray->Num());
	Result->SetNumberField(TEXT("succeeded"), SuccessCount);
	Result->SetNumberField(TEXT("failed"), Errors.Num());
	Result->SetArrayField(TEXT("results"), Results);
	if (Errors.Num() > 0)
	{
		Result->SetArrayField(TEXT("errors"), Errors);
	}
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Batch: %d/%d effects created"), SuccessCount, EffectsArray->Num()));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  16. add_execution
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleAddExecution(const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* BP = nullptr;
	UGameplayEffect* GE = nullptr;
	FString AssetPath;
	FMonolithActionResult Err;
	if (!LoadGEFromParams(Params, BP, GE, AssetPath, Err)) return Err;

	FString CalcClassPath;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("calculation_class"), CalcClassPath, Err)) return Err;

	// Resolve the execution calculation class
	UClass* CalcClass = FindFirstObject<UClass>(*CalcClassPath, EFindFirstObjectOptions::NativeFirst);
	if (!CalcClass)
	{
		CalcClass = FindFirstObject<UClass>(*(TEXT("U") + CalcClassPath), EFindFirstObjectOptions::NativeFirst);
	}
	if (!CalcClass)
	{
		CalcClass = LoadClass<UGameplayEffectExecutionCalculation>(nullptr, *CalcClassPath);
	}
	if (!CalcClass || !CalcClass->IsChildOf(UGameplayEffectExecutionCalculation::StaticClass()))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Execution calculation class not found or invalid: %s"), *CalcClassPath));
	}

	// Build the execution definition
	FGameplayEffectExecutionDefinition ExecDef;
	ExecDef.CalculationClass = CalcClass;

	// Optional scoped modifiers
	const TArray<TSharedPtr<FJsonValue>>* ScopedArray;
	if (Params->TryGetArrayField(TEXT("scoped_modifiers"), ScopedArray) && ScopedArray)
	{
		for (const auto& ScopedVal : *ScopedArray)
		{
			const TSharedPtr<FJsonObject>* ScopedObjPtr;
			if (!ScopedVal->TryGetObject(ScopedObjPtr)) continue;
			const TSharedPtr<FJsonObject>& ScopedObj = *ScopedObjPtr;

			FGameplayEffectExecutionScopedModifierInfo ScopedMod;

			FString AttrStr = ScopedObj->GetStringField(TEXT("attribute"));
			if (!AttrStr.IsEmpty())
			{
				FString AttrError;
				FGameplayAttribute Attr = ParseAttribute(AttrStr, AttrError);
				if (!AttrError.IsEmpty()) continue;

				ScopedMod.CapturedAttribute = FGameplayEffectAttributeCaptureDefinition(
					Attr,
					EGameplayEffectAttributeCaptureSource::Source,
					false);
			}

			FString OpStr = ScopedObj->GetStringField(TEXT("operation"));
			if (!OpStr.IsEmpty())
			{
				FString OpError;
				EGameplayModOp::Type TempOp;
				if (ParseModifierOp(OpStr, TempOp, OpError)) { ScopedMod.ModifierOp = TempOp; }
			}

			const TSharedPtr<FJsonObject>* MagObjPtr = nullptr;
			if (ScopedObj->TryGetObjectField(TEXT("magnitude"), MagObjPtr) && MagObjPtr && (*MagObjPtr).IsValid())
			{
				FString MagError;
				ConfigureMagnitude(*MagObjPtr, ScopedMod.ModifierMagnitude, MagError);
			}

			ExecDef.CalculationModifiers.Add(ScopedMod);
		}
	}

	GE->Executions.Add(ExecDef);
	MarkModified(BP);

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(AssetPath,
		FString::Printf(TEXT("Added execution calculation: %s"), *CalcClass->GetName()));
	Result->SetStringField(TEXT("calculation_class"), CalcClass->GetPathName());
	Result->SetNumberField(TEXT("execution_index"), GE->Executions.Num() - 1);
	Result->SetNumberField(TEXT("scoped_modifier_count"), ExecDef.CalculationModifiers.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  17. duplicate_gameplay_effect
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleDuplicateGameplayEffect(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath, DestPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("source_path"), SourcePath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("dest_path"), DestPath, Err)) return Err;

	// Load source
	TSharedPtr<FJsonObject> SourceParams = MakeShared<FJsonObject>();
	SourceParams->SetStringField(TEXT("asset_path"), SourcePath);
	UBlueprint* SourceBP = nullptr;
	UGameplayEffect* SourceGE = nullptr;
	FString SourceAssetPath;
	FMonolithActionResult LoadErr;
	if (!LoadGEFromParams(SourceParams, SourceBP, SourceGE, SourceAssetPath, LoadErr)) return LoadErr;

	// Create destination GE with same duration policy
	UGameplayEffect* DestGE = nullptr;
	FMonolithActionResult CreateErr;
	UBlueprint* DestBP = CreateGEBlueprint(DestPath, SourceGE->DurationPolicy, DestGE, CreateErr);
	if (!DestBP) return CreateErr;

	// Copy modifiers
	DestGE->Modifiers = SourceGE->Modifiers;

	// Copy duration magnitude
	DestGE->DurationMagnitude = SourceGE->DurationMagnitude;

	// Copy period
	DestGE->Period = SourceGE->Period;
	DestGE->bExecutePeriodicEffectOnApplication = SourceGE->bExecutePeriodicEffectOnApplication;

	// Copy stacking (via reflection)
	{
		SetGameplayEffectStackingTypeValue(DestGE, GetGameplayEffectStackingTypeValue(SourceGE));
	}
	DestGE->StackLimitCount = SourceGE->StackLimitCount;
	DestGE->StackDurationRefreshPolicy = SourceGE->StackDurationRefreshPolicy;
	DestGE->StackPeriodResetPolicy = SourceGE->StackPeriodResetPolicy;
	DestGE->StackExpirationPolicy = SourceGE->StackExpirationPolicy;

	// Copy executions
	DestGE->Executions = SourceGE->Executions;

	// Apply overrides
	const TSharedPtr<FJsonObject>* OverridesPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("overrides"), OverridesPtr) && OverridesPtr && (*OverridesPtr).IsValid())
	{
		const TSharedPtr<FJsonObject>& Ov = *OverridesPtr;

		// Override duration policy
		FString DurStr = Ov->GetStringField(TEXT("duration_policy"));
		if (!DurStr.IsEmpty())
		{
			EGameplayEffectDurationType NewPolicy;
			FString ParseError;
			if (ParseDurationPolicy(DurStr, NewPolicy, ParseError))
			{
				DestGE->DurationPolicy = NewPolicy;
			}
		}

		// Override modifiers (full replacement)
		const TArray<TSharedPtr<FJsonValue>>* ModArray;
		if (Ov->TryGetArrayField(TEXT("modifiers"), ModArray) && ModArray->Num() > 0)
		{
			DestGE->Modifiers.Empty();
			for (const auto& ModVal : *ModArray)
			{
				const TSharedPtr<FJsonObject>* ModObjPtr;
				if (!ModVal->TryGetObject(ModObjPtr)) continue;
				const TSharedPtr<FJsonObject>& ModObj = *ModObjPtr;

				FGameplayModifierInfo NewMod;

				FString AttrStr = ModObj->GetStringField(TEXT("attribute"));
				if (!AttrStr.IsEmpty())
				{
					FString AttrError;
					NewMod.Attribute = ParseAttribute(AttrStr, AttrError);
				}

				FString OpStr = ModObj->GetStringField(TEXT("operation"));
				if (!OpStr.IsEmpty())
				{
					FString OpError;
					EGameplayModOp::Type TempOp;
					if (ParseModifierOp(OpStr, TempOp, OpError)) { NewMod.ModifierOp = TempOp; }
				}

				const TSharedPtr<FJsonObject>* MagObjPtr = nullptr;
				if (ModObj->TryGetObjectField(TEXT("magnitude"), MagObjPtr) && MagObjPtr && (*MagObjPtr).IsValid())
				{
					FString MagError;
					ConfigureMagnitude(*MagObjPtr, NewMod.ModifierMagnitude, MagError);
				}
				else if (ModObj->HasField(TEXT("value")))
				{
					NewMod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(ModObj->GetNumberField(TEXT("value"))));
				}

				DestGE->Modifiers.Add(NewMod);
			}
		}
	}

	MarkModified(DestBP);
	bool bSaved = SaveGEPackage(DestBP, DestPath);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source_path"), SourcePath);
	Result->SetStringField(TEXT("dest_path"), DestPath);
	Result->SetStringField(TEXT("duration_policy"), DurationPolicyToString(DestGE->DurationPolicy));
	Result->SetNumberField(TEXT("modifier_count"), DestGE->Modifiers.Num());
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Duplicated GE from '%s' to '%s'"), *SourcePath, *DestPath));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  18. delete_gameplay_effect
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleDeleteGameplayEffect(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, Err)) return Err;

	bool bForce = false;
	Params->TryGetBoolField(TEXT("force"), bForce);

	// Load the asset to verify it exists and is a GE
	TSharedPtr<FJsonObject> LoadParams = MakeShared<FJsonObject>();
	LoadParams->SetStringField(TEXT("asset_path"), AssetPath);
	UBlueprint* BP = nullptr;
	UGameplayEffect* GE = nullptr;
	FString LoadedPath;
	FMonolithActionResult LoadErr;
	if (!LoadGEFromParams(LoadParams, BP, GE, LoadedPath, LoadErr)) return LoadErr;

	// Check for references
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetIdentifier> Referencers;
	AR.GetReferencers(FAssetIdentifier(FName(*AssetPath)), Referencers);

	if (Referencers.Num() > 0 && !bForce)
	{
		TArray<TSharedPtr<FJsonValue>> RefArray;
		for (const FAssetIdentifier& Ref : Referencers)
		{
			RefArray.Add(MakeShared<FJsonValueString>(Ref.ToString()));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetBoolField(TEXT("deleted"), false);
		Result->SetNumberField(TEXT("reference_count"), Referencers.Num());
		Result->SetArrayField(TEXT("referenced_by"), RefArray);
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Cannot delete: %d assets reference this GE. Use force=true to delete anyway."), Referencers.Num()));
		return FMonolithActionResult::Success(Result);
	}

	// Delete the asset
	TArray<UObject*> ObjectsToDelete;
	ObjectsToDelete.Add(BP);

	int32 DeletedCount = ObjectTools::DeleteObjects(ObjectsToDelete, !bForce);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetBoolField(TEXT("deleted"), DeletedCount > 0);
	if (Referencers.Num() > 0)
	{
		Result->SetNumberField(TEXT("reference_count"), Referencers.Num());
		Result->SetBoolField(TEXT("force_deleted"), true);
	}
	Result->SetStringField(TEXT("message"),
		DeletedCount > 0
			? FString::Printf(TEXT("Deleted GameplayEffect: %s"), *AssetPath)
			: FString::Printf(TEXT("Failed to delete: %s"), *AssetPath));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 3: validate_effect
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleValidateEffect(const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* BP = nullptr;
	UGameplayEffect* GE = nullptr;
	FString AssetPath;
	FMonolithActionResult LoadErr;
	if (!LoadGEFromParams(Params, BP, GE, AssetPath, LoadErr)) return LoadErr;

	TArray<TSharedPtr<FJsonValue>> Errors;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	bool bIsInstant = (GE->DurationPolicy == EGameplayEffectDurationType::Instant);

	// Check: period on Instant GE
	{
		float Period = GE->Period.GetValueAtLevel(1.f);
		if (bIsInstant && Period > 0.f)
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetStringField(TEXT("type"), TEXT("period_on_instant"));
			ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
			ErrObj->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Period (%.2f) set on Instant GE. Period is ignored for Instant effects — use HasDuration."), Period));
			Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
		}
	}

	// Check: stacking on Instant GE
	{
		if (bIsInstant && GetGameplayEffectStackingTypeValue(GE) != EGameplayEffectStackingType::None)
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetStringField(TEXT("type"), TEXT("stacking_on_instant"));
			ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
			ErrObj->SetStringField(TEXT("message"),
				TEXT("Stacking configured on Instant effect. Instant effects apply once and never stack — use HasDuration or Infinite."));
			Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
		}
	}

	// Check: modifier targets unknown attribute
	for (int32 i = 0; i < GE->Modifiers.Num(); i++)
	{
		const FGameplayModifierInfo& Mod = GE->Modifiers[i];
		if (!Mod.Attribute.IsValid())
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetStringField(TEXT("type"), TEXT("invalid_modifier_attribute"));
			ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
			ErrObj->SetNumberField(TEXT("modifier_index"), i);
			ErrObj->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Modifier[%d] has no valid attribute set. The attribute class may not be loaded or may not exist."), i));
			Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
		}

		// Check: SetByCaller magnitude without tag
		if (Mod.ModifierMagnitude.GetMagnitudeCalculationType() == EGameplayEffectMagnitudeCalculation::SetByCaller)
		{
			const FSetByCallerFloat& SBC = Mod.ModifierMagnitude.GetSetByCallerFloat();
			if (!SBC.DataTag.IsValid() && SBC.DataName == NAME_None)
			{
				TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
				ErrObj->SetStringField(TEXT("type"), TEXT("missing_setbycaller_tag"));
				ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
				ErrObj->SetNumberField(TEXT("modifier_index"), i);
				ErrObj->SetStringField(TEXT("message"),
					FString::Printf(TEXT("Modifier[%d] uses SetByCaller magnitude but has no tag or data name. The magnitude will be 0."), i));
				Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
			}
		}
	}

	// Check: deprecated flat tag properties (pre-5.3 pattern)
	// These are the old properties that should be using GE Components instead
	static const TArray<FName> DeprecatedTagProperties = {
		TEXT("InheritableGameplayEffectTags"),
		TEXT("InheritableOwnedTagsContainer"),
		TEXT("OngoingTagRequirements"),
		TEXT("ApplicationTagRequirements"),
		TEXT("RemovalTagRequirements"),
		TEXT("RemoveGameplayEffectsWithTags")
	};

	for (const FName& PropName : DeprecatedTagProperties)
	{
		FProperty* Prop = GE->GetClass()->FindPropertyByName(PropName);
		if (Prop)
		{
			// Check if the deprecated property has any data
			FGameplayTagContainer* Container = Prop->ContainerPtrToValuePtr<FGameplayTagContainer>(GE);
			if (Container && Container->Num() > 0)
			{
				TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
				WarnObj->SetStringField(TEXT("type"), TEXT("deprecated_flat_tag_property"));
				WarnObj->SetStringField(TEXT("severity"), TEXT("warning"));
				WarnObj->SetStringField(TEXT("property"), PropName.ToString());
				WarnObj->SetStringField(TEXT("message"),
					FString::Printf(TEXT("Deprecated flat tag property '%s' has data. Use GE Components instead (5.3+ architecture)."), *PropName.ToString()));
				Warnings.Add(MakeShared<FJsonValueObject>(WarnObj));
			}
		}
	}

	// Check: GE component presence
	TArray<UGameplayEffectComponent*> Components = GetGEComponents(GE);

	bool bValid = (Errors.Num() == 0);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetBoolField(TEXT("valid"), bValid);
	Result->SetStringField(TEXT("duration_policy"), DurationPolicyToString(GE->DurationPolicy));
	Result->SetNumberField(TEXT("modifier_count"), GE->Modifiers.Num());
	Result->SetNumberField(TEXT("component_count"), Components.Num());
	Result->SetNumberField(TEXT("execution_count"), GE->Executions.Num());
	Result->SetNumberField(TEXT("error_count"), Errors.Num());
	Result->SetNumberField(TEXT("warning_count"), Warnings.Num());
	if (Errors.Num() > 0) Result->SetArrayField(TEXT("errors"), Errors);
	if (Warnings.Num() > 0) Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetStringField(TEXT("message"),
		bValid ? FString::Printf(TEXT("GE '%s' passed validation"), *BP->GetName())
		       : FString::Printf(TEXT("GE '%s': %d errors, %d warnings"), *BP->GetName(), Errors.Num(), Warnings.Num()));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 3: get_effect_interaction_matrix
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleGetEffectInteractionMatrix(const TSharedPtr<FJsonObject>& Params)
{
	// Collect GE Blueprints
	struct FGEEntry
	{
		FString Path;
		FString Name;
		UBlueprint* BP;
		UGameplayEffect* GE;
	};

	TArray<FGEEntry> GEs;
	const TArray<TSharedPtr<FJsonValue>>* PathsArray = nullptr;

	if (Params->TryGetArrayField(TEXT("asset_paths"), PathsArray) && PathsArray && PathsArray->Num() > 0)
	{
		for (const auto& Val : *PathsArray)
		{
			FString PathStr;
			if (!Val->TryGetString(PathStr)) continue;

			TSharedPtr<FJsonObject> TempParams = MakeShared<FJsonObject>();
			TempParams->SetStringField(TEXT("asset_path"), PathStr);
			UBlueprint* GeBP = nullptr;
			UGameplayEffect* GeGE = nullptr;
			FString OutPath;
			FMonolithActionResult TempErr;
			if (LoadGEFromParams(TempParams, GeBP, GeGE, OutPath, TempErr))
			{
				FGEEntry Entry;
				Entry.Path = OutPath;
				Entry.Name = GeBP->GetName();
				Entry.BP = GeBP;
				Entry.GE = GeGE;
				GEs.Add(Entry);
			}
		}
	}
	else
	{
		// Scan all GEs
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> AllBlueprints;
		AR.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBlueprints, true);

		for (const FAssetData& Asset : AllBlueprints)
		{
			// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
			FAssetTagValueRef ParentTag = Asset.TagsAndValues.FindTag(FName("ParentClass"));
			FAssetTagValueRef NativeParentTag = Asset.TagsAndValues.FindTag(FName("NativeParentClass"));
			FString ParentPath = ParentTag.IsSet() ? ParentTag.GetValue() : (NativeParentTag.IsSet() ? NativeParentTag.GetValue() : TEXT(""));
			if (!ParentPath.Contains(TEXT("GameplayEffect")))
			{
				continue;
			}

			UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
			if (!BP || !MonolithGAS::IsGameplayEffectBlueprint(BP)) continue;

			UGameplayEffect* GE = MonolithGAS::GetBlueprintCDO<UGameplayEffect>(BP);
			if (!GE) continue;

			FGEEntry Entry;
			Entry.Path = Asset.PackageName.ToString();
			Entry.Name = BP->GetName();
			Entry.BP = BP;
			Entry.GE = GE;
			GEs.Add(Entry);
		}
	}

	TArray<TSharedPtr<FJsonValue>> Interactions;

	for (int32 i = 0; i < GEs.Num(); i++)
	{
		TArray<UGameplayEffectComponent*> Components = GetGEComponents(GEs[i].GE);

		for (const UGameplayEffectComponent* Comp : Components)
		{
			// Check RemoveOther components
			if (const URemoveOtherGameplayEffectComponent* RemoveComp = Cast<URemoveOtherGameplayEffectComponent>(Comp))
			{
				for (const FGameplayEffectQuery& Query : RemoveComp->RemoveGameplayEffectQueries)
				{
					// Check which effects this might remove by comparing asset tags
					for (int32 j = 0; j < GEs.Num(); j++)
					{
						if (i == j) continue;

						// Get asset tags of the target
						TArray<UGameplayEffectComponent*> TargetComps = GetGEComponents(GEs[j].GE);
						for (const UGameplayEffectComponent* TargetComp : TargetComps)
						{
							if (const UAssetTagsGameplayEffectComponent* AssetTags = Cast<UAssetTagsGameplayEffectComponent>(TargetComp))
							{
								const FInheritedTagContainer& TagChanges = AssetTags->GetConfiguredAssetTagChanges();
								if (Query.EffectTagQuery.IsEmpty() == false)
								{
									if (Query.EffectTagQuery.Matches(TagChanges.Added))
									{
										TSharedPtr<FJsonObject> Interaction = MakeShared<FJsonObject>();
										Interaction->SetStringField(TEXT("source"), GEs[i].Name);
										Interaction->SetStringField(TEXT("target"), GEs[j].Name);
										Interaction->SetStringField(TEXT("type"), TEXT("removes"));
										Interactions.Add(MakeShared<FJsonValueObject>(Interaction));
									}
								}
							}
						}
					}
				}
			}

			// Check Immunity components
			if (const UImmunityGameplayEffectComponent* ImmunityComp = Cast<UImmunityGameplayEffectComponent>(Comp))
			{
				for (const FGameplayEffectQuery& Query : ImmunityComp->ImmunityQueries)
				{
					for (int32 j = 0; j < GEs.Num(); j++)
					{
						if (i == j) continue;

						TArray<UGameplayEffectComponent*> TargetComps = GetGEComponents(GEs[j].GE);
						for (const UGameplayEffectComponent* TargetComp : TargetComps)
						{
							if (const UAssetTagsGameplayEffectComponent* AssetTags = Cast<UAssetTagsGameplayEffectComponent>(TargetComp))
							{
								const FInheritedTagContainer& TagChanges = AssetTags->GetConfiguredAssetTagChanges();
								if (!Query.EffectTagQuery.IsEmpty() && Query.EffectTagQuery.Matches(TagChanges.Added))
								{
									TSharedPtr<FJsonObject> Interaction = MakeShared<FJsonObject>();
									Interaction->SetStringField(TEXT("source"), GEs[i].Name);
									Interaction->SetStringField(TEXT("target"), GEs[j].Name);
									Interaction->SetStringField(TEXT("type"), TEXT("immune_to"));
									Interactions.Add(MakeShared<FJsonValueObject>(Interaction));
								}
							}
						}
					}
				}
			}

			// Check BlockAbility components
			if (const UBlockAbilityTagsGameplayEffectComponent* BlockComp = Cast<UBlockAbilityTagsGameplayEffectComponent>(Comp))
			{
				const FInheritedTagContainer& BlockTags = BlockComp->GetConfiguredBlockedAbilityTagChanges();
				if (BlockTags.Added.Num() > 0)
				{
					TSharedPtr<FJsonObject> Interaction = MakeShared<FJsonObject>();
					Interaction->SetStringField(TEXT("source"), GEs[i].Name);
					Interaction->SetStringField(TEXT("type"), TEXT("blocks_abilities"));
					Interaction->SetField(TEXT("blocked_tags"), MonolithGAS::TagContainerToJson(BlockTags.Added));
					Interactions.Add(MakeShared<FJsonValueObject>(Interaction));
				}
			}
		}
	}

	// Build summary
	TArray<TSharedPtr<FJsonValue>> GESummary;
	for (const FGEEntry& E : GEs)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), E.Name);
		Obj->SetStringField(TEXT("path"), E.Path);
		Obj->SetStringField(TEXT("duration"), DurationPolicyToString(E.GE->DurationPolicy));
		Obj->SetNumberField(TEXT("modifier_count"), E.GE->Modifiers.Num());
		GESummary.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("effect_count"), GEs.Num());
	Result->SetArrayField(TEXT("effects"), GESummary);
	Result->SetNumberField(TEXT("interaction_count"), Interactions.Num());
	Result->SetArrayField(TEXT("interactions"), Interactions);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Interaction matrix: %d effects, %d interactions"), GEs.Num(), Interactions.Num()));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 3: remove_ge_component
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleRemoveGEComponent(const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* BP = nullptr;
	UGameplayEffect* GE = nullptr;
	FString AssetPath;
	FMonolithActionResult LoadErr;
	if (!LoadGEFromParams(Params, BP, GE, AssetPath, LoadErr)) return LoadErr;

	FString TypeStr;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("component_type"), TypeStr, Err)) return Err;

	int32 TargetIndex = 0;
	Params->TryGetNumberField(TEXT("index"), TargetIndex);

	// Resolve the component class
	FString ResolveError;
	UClass* CompClass = ResolveComponentClass(TypeStr, ResolveError);
	if (!CompClass)
	{
		return FMonolithActionResult::Error(ResolveError);
	}

	// Find matching components
	TArray<UGameplayEffectComponent*> AllComps = GetGEComponents(GE);
	TArray<UGameplayEffectComponent*> MatchingComps;
	for (UGameplayEffectComponent* Comp : AllComps)
	{
		if (Comp && Comp->IsA(CompClass))
		{
			MatchingComps.Add(Comp);
		}
	}

	if (MatchingComps.Num() == 0)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("No '%s' component found on GE '%s'"), *TypeStr, *BP->GetName()));
	}

	if (TargetIndex >= MatchingComps.Num())
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Index %d out of range — only %d '%s' component(s) exist"),
				TargetIndex, MatchingComps.Num(), *TypeStr));
	}

	UGameplayEffectComponent* CompToRemove = MatchingComps[TargetIndex];

	// Access GEComponents array via FProperty reflection (it's protected)
	FProperty* GEComponentsProp = GE->GetClass()->FindPropertyByName(TEXT("GEComponents"));
	if (!GEComponentsProp)
	{
		return FMonolithActionResult::Error(TEXT("Failed to find GEComponents property via reflection"));
	}

	FArrayProperty* ArrayProp = CastField<FArrayProperty>(GEComponentsProp);
	if (!ArrayProp)
	{
		return FMonolithActionResult::Error(TEXT("GEComponents is not an array property"));
	}

	void* ArrayPtr = GEComponentsProp->ContainerPtrToValuePtr<void>(GE);
	FScriptArrayHelper ArrayHelper(ArrayProp, ArrayPtr);

	// Find and remove the target component from the raw array
	bool bRemoved = false;
	FObjectProperty* InnerProp = CastField<FObjectProperty>(ArrayProp->Inner);
	if (InnerProp)
	{
		for (int32 Idx = ArrayHelper.Num() - 1; Idx >= 0; Idx--)
		{
			UObject* Elem = InnerProp->GetObjectPropertyValue(ArrayHelper.GetRawPtr(Idx));
			if (Elem == CompToRemove)
			{
				ArrayHelper.RemoveValues(Idx, 1);
				bRemoved = true;
				break;
			}
		}
	}

	if (!bRemoved)
	{
		return FMonolithActionResult::Error(TEXT("Failed to remove component from GEComponents array"));
	}

	CompToRemove->MarkAsGarbage();

	MarkModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("component_type"), TypeStr);
	Result->SetNumberField(TEXT("index"), TargetIndex);
	Result->SetBoolField(TEXT("removed"), true);
	Result->SetNumberField(TEXT("remaining_components"), GetGEComponentCount(GE));
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Removed '%s' component (index %d) from '%s'. %d components remaining."),
			*TypeStr, TargetIndex, *BP->GetName(), GetGEComponentCount(GE)));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 4 Runtime Helpers
// ============================================================

namespace
{
	/** Resolve a GE class by name or path. */
	TSubclassOf<UGameplayEffect> ResolveGEClass(const FString& ClassId, FString& OutError)
	{
		// Try loading as an asset path first
		if (ClassId.StartsWith(TEXT("/")))
		{
			FString BPPath = ClassId;
			if (!BPPath.EndsWith(TEXT("_C")))
			{
				// Try "AssetPath.AssetName_C" pattern
				FString BaseName = FPaths::GetBaseFilename(ClassId);
				BPPath = ClassId + TEXT(".") + BaseName + TEXT("_C");
			}
			UClass* LoadedClass = LoadClass<UGameplayEffect>(nullptr, *BPPath);
			if (LoadedClass)
			{
				return LoadedClass;
			}
		}

		// Try direct class name lookup
		UClass* FoundClass = FindFirstObject<UClass>(*ClassId, EFindFirstObjectOptions::NativeFirst);
		if (!FoundClass)
		{
			FoundClass = FindFirstObject<UClass>(*(TEXT("U") + ClassId), EFindFirstObjectOptions::NativeFirst);
		}
		// Try with _C suffix (Blueprint-generated classes)
		if (!FoundClass && !ClassId.EndsWith(TEXT("_C")))
		{
			FoundClass = FindFirstObject<UClass>(*(ClassId + TEXT("_C")), EFindFirstObjectOptions::NativeFirst);
		}
		if (FoundClass && FoundClass->IsChildOf(UGameplayEffect::StaticClass()))
		{
			return FoundClass;
		}

		// Try Asset Registry lookup for bare Blueprint names
		{
			IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			FString SearchName = ClassId;
			if (SearchName.EndsWith(TEXT("_C")))
			{
				SearchName = SearchName.LeftChop(2);
			}
			TArray<FAssetData> Assets;
			AR.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Assets);
			for (const FAssetData& Asset : Assets)
			{
				if (Asset.AssetName.ToString() == SearchName)
				{
					FString BPPath = Asset.GetObjectPathString() + TEXT("_C");
					UClass* LoadedFromAR = LoadClass<UGameplayEffect>(nullptr, *BPPath);
					if (LoadedFromAR)
					{
						return LoadedFromAR;
					}
				}
			}
		}

		OutError = FString::Printf(TEXT("GameplayEffect class not found: '%s'"), *ClassId);
		return nullptr;
	}
}

// ============================================================
//  Phase 4: get_active_effects
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleGetActiveEffects(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorId;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor"), ActorId, Err)) return Err;

	FString FilterClass, FilterTag;
	Params->TryGetStringField(TEXT("filter_class"), FilterClass);
	Params->TryGetStringField(TEXT("filter_tag"), FilterTag);

	FGameplayTag FilterGameplayTag;
	if (!FilterTag.IsEmpty())
	{
		FilterGameplayTag = FGameplayTag::RequestGameplayTag(FName(*FilterTag), false);
	}

	UWorld* PIEWorld = MonolithGAS::GetPIEWorld();
	if (!PIEWorld)
	{
		return FMonolithActionResult::Error(TEXT("No PIE world found. Start Play In Editor first."));
	}

	AActor* Actor = MonolithGAS::FindActorInPIE(ActorId);
	if (!Actor)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor not found in PIE world: '%s'"), *ActorId));
	}

	UAbilitySystemComponent* ASC = MonolithGAS::GetASCFromActor(Actor);
	if (!ASC)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor '%s' has no AbilitySystemComponent"), *ActorId));
	}

	TArray<TSharedPtr<FJsonValue>> EffectEntries;
	FGameplayEffectQuery AllQuery;
	TArray<FActiveGameplayEffectHandle> EffectHandles = ASC->GetActiveEffects(AllQuery);

	for (int32 Idx = 0; Idx < EffectHandles.Num(); Idx++)
	{
		const FActiveGameplayEffect* ActiveGEPtr = ASC->GetActiveGameplayEffect(EffectHandles[Idx]);
		if (!ActiveGEPtr || !ActiveGEPtr->Spec.Def) continue;
		const FActiveGameplayEffect& AGE = *ActiveGEPtr;

		FString EffectClassName = AGE.Spec.Def->GetClass()->GetName();

		// Apply class filter
		if (!FilterClass.IsEmpty() && !EffectClassName.Contains(FilterClass))
		{
			continue;
		}

		// Apply tag filter
		if (FilterGameplayTag.IsValid())
		{
			const FGameplayTagContainer& AssetTags = AGE.Spec.Def->GetAssetTags();
			if (!AssetTags.HasTag(FilterGameplayTag))
			{
				continue;
			}
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("index"), Idx);
		Entry->SetStringField(TEXT("effect_class"), EffectClassName);
		Entry->SetStringField(TEXT("effect_name"), AGE.Spec.Def->GetName());
		Entry->SetStringField(TEXT("duration_policy"), DurationPolicyToString(AGE.Spec.Def->DurationPolicy));
		Entry->SetNumberField(TEXT("level"), AGE.Spec.GetLevel());
		Entry->SetNumberField(TEXT("stack_count"), AGE.Spec.GetStackCount());

		// Duration info
		float Duration = AGE.GetDuration();
		float TimeRemaining = AGE.GetTimeRemaining(PIEWorld->GetTimeSeconds());
		Entry->SetNumberField(TEXT("duration"), Duration);
		if (AGE.Spec.Def->DurationPolicy == EGameplayEffectDurationType::HasDuration)
		{
			Entry->SetNumberField(TEXT("time_remaining"), FMath::Max(0.f, TimeRemaining));
		}

		// Modifiers summary
		TArray<TSharedPtr<FJsonValue>> ModArr;
		for (int32 ModIdx = 0; ModIdx < AGE.Spec.Def->Modifiers.Num(); ModIdx++)
		{
			const FGameplayModifierInfo& Mod = AGE.Spec.Def->Modifiers[ModIdx];
			ModArr.Add(MakeShared<FJsonValueObject>(ModifierToJson(Mod, ModIdx)));
		}
		Entry->SetArrayField(TEXT("modifiers"), ModArr);

		// Source
		if (AGE.Spec.GetEffectContext().GetInstigator())
		{
			Entry->SetStringField(TEXT("instigator"), AGE.Spec.GetEffectContext().GetInstigator()->GetName());
		}

		EffectEntries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Result->SetNumberField(TEXT("count"), EffectEntries.Num());
	Result->SetNumberField(TEXT("total_active"), EffectHandles.Num());
	Result->SetArrayField(TEXT("active_effects"), EffectEntries);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Found %d active effects on '%s' (total: %d)"),
			EffectEntries.Num(), *Actor->GetActorLabel(), EffectHandles.Num()));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 4: get_effect_modifiers_breakdown
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleGetEffectModifiersBreakdown(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorId, AttrStr;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor"), ActorId, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("attribute"), AttrStr, Err)) return Err;

	UWorld* PIEWorld = MonolithGAS::GetPIEWorld();
	if (!PIEWorld)
	{
		return FMonolithActionResult::Error(TEXT("No PIE world found. Start Play In Editor first."));
	}

	AActor* Actor = MonolithGAS::FindActorInPIE(ActorId);
	if (!Actor)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor not found in PIE world: '%s'"), *ActorId));
	}

	UAbilitySystemComponent* ASC = MonolithGAS::GetASCFromActor(Actor);
	if (!ASC)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor '%s' has no AbilitySystemComponent"), *ActorId));
	}

	// Parse the attribute
	FString AttrError;
	FGameplayAttribute Attribute = ParseAttribute(AttrStr, AttrError);
	if (!Attribute.IsValid())
	{
		return FMonolithActionResult::Error(AttrError);
	}

	// Get base value
	float BaseValue = ASC->GetNumericAttributeBase(Attribute);

	// Walk active effects to find modifiers that affect this attribute
	TArray<TSharedPtr<FJsonValue>> Modifiers;
	FGameplayEffectQuery AllQuery;
	TArray<FActiveGameplayEffectHandle> EffectHandles = ASC->GetActiveEffects(AllQuery);

	float Additive = 0.f;
	float MultiplyAdditive = 0.f;
	float MultiplyCompound = 1.f;
	float DivideAdditive = 0.f;
	float AddFinal = 0.f;

	for (const FActiveGameplayEffectHandle& EffHandle : EffectHandles)
	{
		const FActiveGameplayEffect* ActiveGEPtr = ASC->GetActiveGameplayEffect(EffHandle);
		if (!ActiveGEPtr || !ActiveGEPtr->Spec.Def) continue;
		const FActiveGameplayEffect& AGE = *ActiveGEPtr;

		for (int32 ModIdx = 0; ModIdx < AGE.Spec.Def->Modifiers.Num(); ModIdx++)
		{
			const FGameplayModifierInfo& Mod = AGE.Spec.Def->Modifiers[ModIdx];
			if (!Mod.Attribute.IsValid() || Mod.Attribute != Attribute)
			{
				continue;
			}

			// Get the evaluated magnitude
			float Magnitude = 0.f;
			AGE.Spec.Def->Modifiers[ModIdx].ModifierMagnitude.AttemptCalculateMagnitude(AGE.Spec, Magnitude);

			// Apply stack count
			int32 StackCount = AGE.Spec.GetStackCount();
			float EffectiveMagnitude = Magnitude * StackCount;

			TSharedPtr<FJsonObject> ModEntry = MakeShared<FJsonObject>();
			ModEntry->SetStringField(TEXT("effect"), AGE.Spec.Def->GetName());
			ModEntry->SetStringField(TEXT("operation"), ModifierOpToString(Mod.ModifierOp));
			ModEntry->SetNumberField(TEXT("magnitude"), Magnitude);
			ModEntry->SetNumberField(TEXT("stack_count"), StackCount);
			ModEntry->SetNumberField(TEXT("effective_magnitude"), EffectiveMagnitude);
			Modifiers.Add(MakeShared<FJsonValueObject>(ModEntry));

			// Accumulate per GAS evaluation order
			switch (Mod.ModifierOp)
			{
			case EGameplayModOp::Additive:          Additive += EffectiveMagnitude; break;
			case EGameplayModOp::MultiplyAdditive:  MultiplyAdditive += EffectiveMagnitude; break;
			case EGameplayModOp::MultiplyCompound:  MultiplyCompound *= EffectiveMagnitude; break;
			case EGameplayModOp::DivideAdditive:    DivideAdditive += EffectiveMagnitude; break;
			case EGameplayModOp::AddFinal:          AddFinal += EffectiveMagnitude; break;
			default: break;
			}
		}
	}

	// Calculate the aggregated result following GAS evaluation order:
	// ((BaseValue + Additive) * (1 + MultiplyAdditive) * MultiplyCompound) / (1 + DivideAdditive) + AddFinal
	float Computed = BaseValue + Additive;
	if (MultiplyAdditive != 0.f) Computed *= (1.f + MultiplyAdditive);
	Computed *= MultiplyCompound;
	if (DivideAdditive != 0.f) Computed /= (1.f + DivideAdditive);
	Computed += AddFinal;

	bool bFound = false;
	float ActualCurrent = ASC->GetGameplayAttributeValue(Attribute, bFound);

	TSharedPtr<FJsonObject> Breakdown = MakeShared<FJsonObject>();
	Breakdown->SetNumberField(TEXT("base_value"), BaseValue);
	Breakdown->SetNumberField(TEXT("additive_sum"), Additive);
	Breakdown->SetNumberField(TEXT("multiply_additive_sum"), MultiplyAdditive);
	Breakdown->SetNumberField(TEXT("multiply_compound_product"), MultiplyCompound);
	Breakdown->SetNumberField(TEXT("divide_additive_sum"), DivideAdditive);
	Breakdown->SetNumberField(TEXT("add_final_sum"), AddFinal);
	Breakdown->SetNumberField(TEXT("computed_value"), Computed);
	Breakdown->SetNumberField(TEXT("actual_current_value"), ActualCurrent);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("attribute"), AttrStr);
	Result->SetNumberField(TEXT("modifier_count"), Modifiers.Num());
	Result->SetArrayField(TEXT("modifiers"), Modifiers);
	Result->SetObjectField(TEXT("breakdown"), Breakdown);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Attribute '%s': base=%.4f, computed=%.4f, actual=%.4f (%d active modifiers)"),
			*AttrStr, BaseValue, Computed, ActualCurrent, Modifiers.Num()));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 4: apply_effect
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleApplyEffect(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorId, EffectClassId;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor"), ActorId, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("effect_class"), EffectClassId, Err)) return Err;

	double Level = 1.0;
	Params->TryGetNumberField(TEXT("level"), Level);

	UWorld* PIEWorld = MonolithGAS::GetPIEWorld();
	if (!PIEWorld)
	{
		return FMonolithActionResult::Error(TEXT("No PIE world found. Start Play In Editor first."));
	}

	AActor* Actor = MonolithGAS::FindActorInPIE(ActorId);
	if (!Actor)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor not found in PIE world: '%s'"), *ActorId));
	}

	UAbilitySystemComponent* ASC = MonolithGAS::GetASCFromActor(Actor);
	if (!ASC)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor '%s' has no AbilitySystemComponent"), *ActorId));
	}

	// Resolve the GE class
	FString ResolveError;
	TSubclassOf<UGameplayEffect> GEClass = ResolveGEClass(EffectClassId, ResolveError);
	if (!GEClass)
	{
		return FMonolithActionResult::Error(ResolveError);
	}

	// Create and apply the effect spec
	FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
	Context.AddSourceObject(Actor);
	FGameplayEffectSpecHandle SpecHandle = ASC->MakeOutgoingSpec(GEClass, static_cast<float>(Level), Context);

	if (!SpecHandle.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Failed to create GameplayEffect spec"));
	}

	// Apply set_by_caller magnitudes
	const TSharedPtr<FJsonObject>* SetByCallerObj;
	if (Params->TryGetObjectField(TEXT("set_by_caller"), SetByCallerObj) && SetByCallerObj && (*SetByCallerObj).IsValid())
	{
		for (const auto& Pair : (*SetByCallerObj)->Values)
		{
			double Magnitude = 0.0;
			if (Pair.Value->TryGetNumber(Magnitude))
			{
				FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*Pair.Key), false);
				if (Tag.IsValid())
				{
					SpecHandle.Data->SetSetByCallerMagnitude(Tag, static_cast<float>(Magnitude));
				}
				else
				{
					return FMonolithActionResult::Error(
						FString::Printf(TEXT("Invalid SetByCaller tag: '%s'"), *Pair.Key));
				}
			}
		}
	}

	FActiveGameplayEffectHandle ActiveHandle = ASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());

	if (!ActiveHandle.IsValid() && GEClass.GetDefaultObject()->DurationPolicy != EGameplayEffectDurationType::Instant)
	{
		return FMonolithActionResult::Error(TEXT("Failed to apply GameplayEffect (may have been blocked by tag requirements or immunity)"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("effect_class"), GEClass->GetName());
	Result->SetNumberField(TEXT("level"), Level);
	Result->SetBoolField(TEXT("applied"), true);
	Result->SetStringField(TEXT("duration_policy"),
		DurationPolicyToString(GEClass.GetDefaultObject()->DurationPolicy));
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Applied '%s' (level %.0f) to '%s'"),
			*GEClass->GetName(), Level, *Actor->GetActorLabel()));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 4: remove_effect
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleRemoveEffect(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorId;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor"), ActorId, Err)) return Err;

	UWorld* PIEWorld = MonolithGAS::GetPIEWorld();
	if (!PIEWorld)
	{
		return FMonolithActionResult::Error(TEXT("No PIE world found. Start Play In Editor first."));
	}

	AActor* Actor = MonolithGAS::FindActorInPIE(ActorId);
	if (!Actor)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor not found in PIE world: '%s'"), *ActorId));
	}

	UAbilitySystemComponent* ASC = MonolithGAS::GetASCFromActor(Actor);
	if (!ASC)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor '%s' has no AbilitySystemComponent"), *ActorId));
	}

	double HandleIndex = -1.0;
	FString EffectClassStr;
	bool bHasHandle = Params->TryGetNumberField(TEXT("effect_handle"), HandleIndex);
	bool bHasClass = Params->TryGetStringField(TEXT("effect_class"), EffectClassStr);

	if (!bHasHandle && !bHasClass)
	{
		return FMonolithActionResult::Error(TEXT("Must provide either 'effect_handle' (int) or 'effect_class' (string)"));
	}

	int32 RemovedCount = 0;

	if (bHasHandle)
	{
		// Remove by handle — find the active effect at this index
		FGameplayEffectQuery AllQuery;
		TArray<FActiveGameplayEffectHandle> AllHandles = ASC->GetActiveEffects(AllQuery);
		int32 Idx = static_cast<int32>(HandleIndex);
		if (Idx >= 0 && Idx < AllHandles.Num())
		{
			if (ASC->RemoveActiveGameplayEffect(AllHandles[Idx]))
			{
				RemovedCount = 1;
			}
		}
		else
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Effect handle index %d out of range (0-%d)"),
					Idx, AllHandles.Num() - 1));
		}
	}
	else if (bHasClass)
	{
		// Remove by class
		FString ResolveError;
		TSubclassOf<UGameplayEffect> GEClass = ResolveGEClass(EffectClassStr, ResolveError);
		if (!GEClass)
		{
			return FMonolithActionResult::Error(ResolveError);
		}

		// Collect handles to remove
		TArray<FActiveGameplayEffectHandle> HandlesToRemove;
		FGameplayEffectQuery AllQuery;
		TArray<FActiveGameplayEffectHandle> AllHandles = ASC->GetActiveEffects(AllQuery);
		for (const FActiveGameplayEffectHandle& EffHandle : AllHandles)
		{
			const FActiveGameplayEffect* ActiveGEPtr = ASC->GetActiveGameplayEffect(EffHandle);
			if (ActiveGEPtr && ActiveGEPtr->Spec.Def && ActiveGEPtr->Spec.Def->GetClass() == GEClass)
			{
				HandlesToRemove.Add(EffHandle);
			}
		}

		for (const FActiveGameplayEffectHandle& Handle : HandlesToRemove)
		{
			if (ASC->RemoveActiveGameplayEffect(Handle))
			{
				RemovedCount++;
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Result->SetNumberField(TEXT("removed_count"), RemovedCount);
	if (bHasHandle)
	{
		Result->SetNumberField(TEXT("handle_index"), static_cast<int32>(HandleIndex));
	}
	if (bHasClass)
	{
		Result->SetStringField(TEXT("effect_class"), EffectClassStr);
	}
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Removed %d effect(s) from '%s'"),
			RemovedCount, *Actor->GetActorLabel()));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 4: simulate_effect_stack
// ============================================================

FMonolithActionResult FMonolithGASEffectActions::HandleSimulateEffectStack(const TSharedPtr<FJsonObject>& Params)
{
	const TSharedPtr<FJsonObject>* StateObj;
	if (!Params->TryGetObjectField(TEXT("attribute_state"), StateObj) || !StateObj || !(*StateObj).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: attribute_state (object with base_value)"));
	}

	double BaseValue = 0.0;
	if (!(*StateObj)->TryGetNumberField(TEXT("base_value"), BaseValue))
	{
		return FMonolithActionResult::Error(TEXT("attribute_state must contain 'base_value' (number)"));
	}

	const TArray<TSharedPtr<FJsonValue>>* EffectsArr;
	if (!Params->TryGetArrayField(TEXT("effects"), EffectsArr) || !EffectsArr || EffectsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: effects (array)"));
	}

	// GAS evaluation order aggregators
	float Additive = 0.f;
	float MultiplyAdditive = 0.f;
	float MultiplyCompound = 1.f;
	float DivideAdditive = 0.f;
	float Override = 0.f;
	bool bHasOverride = false;
	float AddFinal = 0.f;

	TArray<TSharedPtr<FJsonValue>> Steps;

	for (int32 Idx = 0; Idx < EffectsArr->Num(); Idx++)
	{
		const TSharedPtr<FJsonObject>* EffectObj;
		if (!(*EffectsArr)[Idx]->TryGetObject(EffectObj) || !EffectObj) continue;

		FString OpStr;
		(*EffectObj)->TryGetStringField(TEXT("operation"), OpStr);
		double Magnitude = 0.0;
		(*EffectObj)->TryGetNumberField(TEXT("magnitude"), Magnitude);

		FString Error;
		EGameplayModOp::Type Op;
		if (!ParseModifierOp(OpStr, Op, Error))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Effect [%d]: %s"), Idx, *Error));
		}

		float Mag = static_cast<float>(Magnitude);

		switch (Op)
		{
		case EGameplayModOp::Additive:          Additive += Mag; break;
		case EGameplayModOp::MultiplyAdditive:  MultiplyAdditive += Mag; break;
		case EGameplayModOp::MultiplyCompound:  MultiplyCompound *= Mag; break;
		case EGameplayModOp::DivideAdditive:    DivideAdditive += Mag; break;
		case EGameplayModOp::Override:          Override = Mag; bHasOverride = true; break;
		case EGameplayModOp::AddFinal:          AddFinal += Mag; break;
		default: break;
		}

		TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetNumberField(TEXT("index"), Idx);
		Step->SetStringField(TEXT("operation"), ModifierOpToString(Op));
		Step->SetNumberField(TEXT("magnitude"), Mag);
		Steps.Add(MakeShared<FJsonValueObject>(Step));
	}

	// Compute final value following GAS aggregation
	float FinalValue;
	if (bHasOverride)
	{
		FinalValue = Override;
	}
	else
	{
		FinalValue = static_cast<float>(BaseValue) + Additive;
		if (MultiplyAdditive != 0.f) FinalValue *= (1.f + MultiplyAdditive);
		FinalValue *= MultiplyCompound;
		if (DivideAdditive != 0.f) FinalValue /= (1.f + DivideAdditive);
	}
	FinalValue += AddFinal;

	TSharedPtr<FJsonObject> Breakdown = MakeShared<FJsonObject>();
	Breakdown->SetNumberField(TEXT("base_value"), BaseValue);
	Breakdown->SetNumberField(TEXT("additive_sum"), Additive);
	Breakdown->SetNumberField(TEXT("multiply_additive_sum"), MultiplyAdditive);
	Breakdown->SetNumberField(TEXT("multiply_compound_product"), MultiplyCompound);
	Breakdown->SetNumberField(TEXT("divide_additive_sum"), DivideAdditive);
	Breakdown->SetBoolField(TEXT("has_override"), bHasOverride);
	if (bHasOverride) Breakdown->SetNumberField(TEXT("override_value"), Override);
	Breakdown->SetNumberField(TEXT("add_final_sum"), AddFinal);
	Breakdown->SetNumberField(TEXT("final_value"), FinalValue);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("base_value"), BaseValue);
	Result->SetNumberField(TEXT("final_value"), FinalValue);
	Result->SetNumberField(TEXT("effect_count"), Steps.Num());
	Result->SetArrayField(TEXT("effects_applied"), Steps);
	Result->SetObjectField(TEXT("breakdown"), Breakdown);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Simulated %d effects: base=%.4f -> final=%.4f"),
			Steps.Num(), BaseValue, FinalValue));
	return FMonolithActionResult::Success(Result);
}
