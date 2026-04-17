#include "Indexers/GASIndexer.h"
#include "MonolithSettings.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetCompilingManager.h"
#include "Engine/Blueprint.h"
#include "UObject/UObjectIterator.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "GameplayTagContainer.h"
#include "GameplayAbilitySpec.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "AttributeSet.h"
#include "GameplayCueNotify_Static.h"
#include "GameplayCueNotify_Actor.h"

// ─────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────

FString FGASIndexer::JsonToString(TSharedPtr<FJsonObject> JsonObj)
{
	FString Out;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(JsonObj, *Writer, true);
	return Out;
}

TArray<TSharedPtr<FJsonValue>> FGASIndexer::ExtractTagContainer(UObject* CDO, const FString& PropertyName)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	if (!CDO) return Result;

	FProperty* Prop = CDO->GetClass()->FindPropertyByName(*PropertyName);
	if (!Prop) return Result;

	// FGameplayTagContainer is a struct property
	FStructProperty* StructProp = CastField<FStructProperty>(Prop);
	if (!StructProp) return Result;

	const void* ValuePtr = StructProp->ContainerPtrToValuePtr<void>(CDO);
	const FGameplayTagContainer* TagContainer = static_cast<const FGameplayTagContainer*>(ValuePtr);
	if (!TagContainer) return Result;

	for (const FGameplayTag& Tag : *TagContainer)
	{
		Result.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}
	return Result;
}

// ─────────────────────────────────────────────────────────────
// Helper: find Blueprint CDOs by native parent class
// ─────────────────────────────────────────────────────────────

namespace
{
	struct FBPWithCDO
	{
		UObject* CDO;
		FString AssetPath;
		int64 AssetId;
	};

	TArray<FBPWithCDO> FindBlueprintCDOsOfClass(UClass* NativeBaseClass, FMonolithIndexDatabase& DB)
	{
		TArray<FBPWithCDO> Results;
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		TArray<FAssetData> BPAssets;
		FARFilter Filter;
		for (const FName& ContentPath : UMonolithSettings::GetIndexedContentPaths())
		{
			Filter.PackagePaths.Add(ContentPath);
		}
		Filter.bRecursivePaths = true;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Registry.GetAssets(Filter, BPAssets);

		// Finish pending asset compilations before loading blueprints
		// This prevents reentrant texture compiler crashes
		FAssetCompilingManager::Get().FinishAllCompilation();

		for (const FAssetData& AssetData : BPAssets)
		{
			UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
			if (!BP || !BP->GeneratedClass) continue;
			if (!BP->GeneratedClass->IsChildOf(NativeBaseClass)) continue;

			UObject* CDO = BP->GeneratedClass->GetDefaultObject(false);
			if (!CDO) continue;

			int64 Id = DB.GetAssetId(AssetData.PackageName.ToString());
			if (Id < 0) continue;

			Results.Add({ CDO, AssetData.GetObjectPathString(), Id });
		}
		return Results;
	}
}

// ─────────────────────────────────────────────────────────────
// Main sentinel entry point
// ─────────────────────────────────────────────────────────────

bool FGASIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	// Collected cross-reference data
	TArray<FAbilityRef> AbilityRefs;
	TArray<FEffectRef> EffectRefs;
	TArray<FAttributeSetRef> AttributeSetRefs;
	TArray<FCueRef> CueRefs;

	// Map from asset path to effect node ID for cross-referencing
	TMap<FString, int64> EffectPathToNodeId;

	int32 TotalIndexed = 0;

	// ── 1. GameplayAbilities ──
	{
		TArray<FBPWithCDO> Abilities = FindBlueprintCDOsOfClass(UGameplayAbility::StaticClass(), DB);
		for (auto& Entry : Abilities)
		{
			int64 NodeId = IndexAbility(Entry.CDO, Entry.AssetPath, DB, Entry.AssetId);
			if (NodeId >= 0)
			{
				FAbilityRef Ref;
				Ref.NodeId = NodeId;

				// Extract cost/cooldown class paths via CDO reflection
				FProperty* CostProp = Entry.CDO->GetClass()->FindPropertyByName(TEXT("CostGameplayEffectClass"));
				if (CostProp)
				{
					FClassProperty* ClassProp = CastField<FClassProperty>(CostProp);
					if (ClassProp)
					{
						UClass* CostClass = Cast<UClass>(ClassProp->GetPropertyValue_InContainer(Entry.CDO));
						if (CostClass)
						{
							Ref.CostEffectPath = CostClass->GetPathName();
						}
					}
				}

				FProperty* CooldownProp = Entry.CDO->GetClass()->FindPropertyByName(TEXT("CooldownGameplayEffectClass"));
				if (CooldownProp)
				{
					FClassProperty* ClassProp = CastField<FClassProperty>(CooldownProp);
					if (ClassProp)
					{
						UClass* CooldownClass = Cast<UClass>(ClassProp->GetPropertyValue_InContainer(Entry.CDO));
						if (CooldownClass)
						{
							Ref.CooldownEffectPath = CooldownClass->GetPathName();
						}
					}
				}

				AbilityRefs.Add(MoveTemp(Ref));
				TotalIndexed++;
			}
		}
	}

	// ── 2. GameplayEffects ──
	{
		TArray<FBPWithCDO> Effects = FindBlueprintCDOsOfClass(UGameplayEffect::StaticClass(), DB);
		for (auto& Entry : Effects)
		{
			int64 NodeId = IndexEffect(Entry.CDO, Entry.AssetPath, DB, Entry.AssetId);
			if (NodeId >= 0)
			{
				EffectPathToNodeId.Add(Entry.CDO->GetClass()->GetPathName(), NodeId);

				FEffectRef Ref;
				Ref.NodeId = NodeId;

				// Collect modifier attribute references
				FProperty* ModsProp = Entry.CDO->GetClass()->FindPropertyByName(TEXT("Modifiers"));
				if (ModsProp)
				{
					FArrayProperty* ArrayProp = CastField<FArrayProperty>(ModsProp);
					if (ArrayProp)
					{
						FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Entry.CDO));
						for (int32 i = 0; i < ArrayHelper.Num(); i++)
						{
							void* ElemPtr = ArrayHelper.GetRawPtr(i);
							FStructProperty* InnerStructProp = CastField<FStructProperty>(ArrayProp->Inner);
							if (!InnerStructProp) continue;

							// Get the Attribute field from FGameplayModifierInfo
							FProperty* AttrProp = InnerStructProp->Struct->FindPropertyByName(TEXT("Attribute"));
							if (!AttrProp) continue;

							FStructProperty* AttrStructProp = CastField<FStructProperty>(AttrProp);
							if (!AttrStructProp) continue;

							const void* AttrPtr = AttrStructProp->ContainerPtrToValuePtr<void>(ElemPtr);

							// FGameplayAttribute has AttributeName and an owning struct/class
							// Export text to get the full attribute reference
							FString AttrExport;
							AttrStructProp->ExportTextItem_Direct(AttrExport, AttrPtr, nullptr, nullptr, PPF_None);
							if (!AttrExport.IsEmpty())
							{
								Ref.ModifiedAttributes.Add(AttrExport);
							}
						}
					}
				}

				// Collect gameplay cue tags
				FProperty* CuesProp = Entry.CDO->GetClass()->FindPropertyByName(TEXT("GameplayCues"));
				if (CuesProp)
				{
					FArrayProperty* CuesArrayProp = CastField<FArrayProperty>(CuesProp);
					if (CuesArrayProp)
					{
						FScriptArrayHelper CueHelper(CuesArrayProp, CuesArrayProp->ContainerPtrToValuePtr<void>(Entry.CDO));
						for (int32 i = 0; i < CueHelper.Num(); i++)
						{
							void* CueElemPtr = CueHelper.GetRawPtr(i);
							FStructProperty* CueInnerProp = CastField<FStructProperty>(CuesArrayProp->Inner);
							if (!CueInnerProp) continue;

							// FGameplayEffectCue has GameplayCueTags (FGameplayTagContainer)
							FProperty* CueTagsProp = CueInnerProp->Struct->FindPropertyByName(TEXT("GameplayCueTags"));
							if (!CueTagsProp) continue;

							FStructProperty* CueTagsStructProp = CastField<FStructProperty>(CueTagsProp);
							if (!CueTagsStructProp) continue;

							const void* CueTagsPtr = CueTagsStructProp->ContainerPtrToValuePtr<void>(CueElemPtr);
							const FGameplayTagContainer* CueTagContainer = static_cast<const FGameplayTagContainer*>(CueTagsPtr);
							if (CueTagContainer)
							{
								for (const FGameplayTag& Tag : *CueTagContainer)
								{
									Ref.CueTags.Add(Tag.ToString());
								}
							}
						}
					}
				}

				EffectRefs.Add(MoveTemp(Ref));
				TotalIndexed++;
			}
		}
	}

	// ── 3. AttributeSets ──
	{
		// Blueprint-based attribute sets
		TArray<FBPWithCDO> AttrSets = FindBlueprintCDOsOfClass(UAttributeSet::StaticClass(), DB);
		for (auto& Entry : AttrSets)
		{
			int64 NodeId = IndexAttributeSet(Entry.CDO->GetClass(), DB, Entry.AssetId);
			if (NodeId >= 0)
			{
				FAttributeSetRef Ref;
				Ref.NodeId = NodeId;
				Ref.ClassName = Entry.CDO->GetClass()->GetName();
				for (TFieldIterator<FProperty> It(Entry.CDO->GetClass()); It; ++It)
				{
					FStructProperty* SP = CastField<FStructProperty>(*It);
					if (SP && SP->Struct && SP->Struct->GetName() == TEXT("GameplayAttributeData"))
					{
						Ref.AttributeNames.Add(FString::Printf(TEXT("%s.%s"), *Ref.ClassName, *It->GetName()));
					}
				}
				AttributeSetRefs.Add(MoveTemp(Ref));
				TotalIndexed++;
			}
		}

		// Native attribute sets (C++ classes not from Blueprints)
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class->IsChildOf(UAttributeSet::StaticClass())) continue;
			if (Class == UAttributeSet::StaticClass()) continue;
			if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists)) continue;
			// Skip Blueprint-generated classes (already handled above)
			if (Class->ClassGeneratedBy) continue;

			// For native classes, use -1 as AssetId (no package in project content)
			int64 NodeId = IndexAttributeSet(Class, DB, -1);
			if (NodeId >= 0)
			{
				FAttributeSetRef Ref;
				Ref.NodeId = NodeId;
				Ref.ClassName = Class->GetName();
				for (TFieldIterator<FProperty> PropIt(Class); PropIt; ++PropIt)
				{
					FStructProperty* SP = CastField<FStructProperty>(*PropIt);
					if (SP && SP->Struct && SP->Struct->GetName() == TEXT("GameplayAttributeData"))
					{
						Ref.AttributeNames.Add(FString::Printf(TEXT("%s.%s"), *Ref.ClassName, *PropIt->GetName()));
					}
				}
				AttributeSetRefs.Add(MoveTemp(Ref));
				TotalIndexed++;
			}
		}
	}

	// ── 4. GameplayCues ──
	{
		// Static cues
		TArray<FBPWithCDO> StaticCues = FindBlueprintCDOsOfClass(UGameplayCueNotify_Static::StaticClass(), DB);
		for (auto& Entry : StaticCues)
		{
			int64 NodeId = IndexGameplayCue(Entry.CDO, Entry.AssetPath, DB, Entry.AssetId, false);
			if (NodeId >= 0)
			{
				FCueRef Ref;
				Ref.NodeId = NodeId;

				FProperty* TagProp = Entry.CDO->GetClass()->FindPropertyByName(TEXT("GameplayCueTag"));
				if (TagProp)
				{
					FStructProperty* TagStructProp = CastField<FStructProperty>(TagProp);
					if (TagStructProp)
					{
						const void* TagPtr = TagStructProp->ContainerPtrToValuePtr<void>(Entry.CDO);
						const FGameplayTag* Tag = static_cast<const FGameplayTag*>(TagPtr);
						if (Tag && Tag->IsValid())
						{
							Ref.CueTag = Tag->ToString();
						}
					}
				}

				CueRefs.Add(MoveTemp(Ref));
				TotalIndexed++;
			}
		}

		// Actor cues
		TArray<FBPWithCDO> ActorCues = FindBlueprintCDOsOfClass(AGameplayCueNotify_Actor::StaticClass(), DB);
		for (auto& Entry : ActorCues)
		{
			int64 NodeId = IndexGameplayCue(Entry.CDO, Entry.AssetPath, DB, Entry.AssetId, true);
			if (NodeId >= 0)
			{
				FCueRef Ref;
				Ref.NodeId = NodeId;

				FProperty* TagProp = Entry.CDO->GetClass()->FindPropertyByName(TEXT("GameplayCueTag"));
				if (TagProp)
				{
					FStructProperty* TagStructProp = CastField<FStructProperty>(TagProp);
					if (TagStructProp)
					{
						const void* TagPtr = TagStructProp->ContainerPtrToValuePtr<void>(Entry.CDO);
						const FGameplayTag* Tag = static_cast<const FGameplayTag*>(TagPtr);
						if (Tag && Tag->IsValid())
						{
							Ref.CueTag = Tag->ToString();
						}
					}
				}

				CueRefs.Add(MoveTemp(Ref));
				TotalIndexed++;
			}
		}
	}

	// ── 5. Cross-references ──

	// Ability -> Effect (cost/cooldown)
	for (const FAbilityRef& AbRef : AbilityRefs)
	{
		auto TryConnect = [&](const FString& EffectPath, const FString& PinName)
		{
			if (EffectPath.IsEmpty()) return;
			int64* EffectNodeId = EffectPathToNodeId.Find(EffectPath);
			if (EffectNodeId)
			{
				FIndexedConnection Conn;
				Conn.SourceNodeId = AbRef.NodeId;
				Conn.SourcePin = PinName;
				Conn.TargetNodeId = *EffectNodeId;
				Conn.TargetPin = TEXT("Self");
				Conn.PinType = TEXT("GAS_Reference");
				DB.InsertConnection(Conn);
			}
		};
		TryConnect(AbRef.CostEffectPath, TEXT("CostEffect"));
		TryConnect(AbRef.CooldownEffectPath, TEXT("CooldownEffect"));
	}

	// Effect -> AttributeSet (modifier targets)
	for (const FEffectRef& ERef : EffectRefs)
	{
		for (const FString& AttrExport : ERef.ModifiedAttributes)
		{
			for (const FAttributeSetRef& ASRef : AttributeSetRefs)
			{
				for (const FString& AttrName : ASRef.AttributeNames)
				{
					// Match if the exported attribute reference contains the attribute set class + attribute name
					if (AttrExport.Contains(ASRef.ClassName) && AttrExport.Contains(AttrName.RightChop(AttrName.Find(TEXT(".")) + 1)))
					{
						FIndexedConnection Conn;
						Conn.SourceNodeId = ERef.NodeId;
						Conn.SourcePin = TEXT("Modifier");
						Conn.TargetNodeId = ASRef.NodeId;
						Conn.TargetPin = AttrName;
						Conn.PinType = TEXT("GAS_ModifiesAttribute");
						DB.InsertConnection(Conn);
						break; // One connection per attribute match
					}
				}
			}
		}

		// Effect -> GameplayCue (cue tags)
		for (const FString& CueTagStr : ERef.CueTags)
		{
			for (const FCueRef& CR : CueRefs)
			{
				if (!CR.CueTag.IsEmpty() && CueTagStr.Contains(CR.CueTag))
				{
					FIndexedConnection Conn;
					Conn.SourceNodeId = ERef.NodeId;
					Conn.SourcePin = TEXT("GameplayCue");
					Conn.TargetNodeId = CR.NodeId;
					Conn.TargetPin = TEXT("Self");
					Conn.PinType = TEXT("GAS_TriggersCue");
					DB.InsertConnection(Conn);
				}
			}
		}
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("GASIndexer: indexed %d GAS assets (%d abilities, %d effects, %d attribute sets, %d cues)"),
		TotalIndexed, AbilityRefs.Num(), EffectRefs.Num(), AttributeSetRefs.Num(), CueRefs.Num());
	return true;
}

// ─────────────────────────────────────────────────────────────
// IndexAbility
// ─────────────────────────────────────────────────────────────

int64 FGASIndexer::IndexAbility(UObject* CDO, const FString& AssetPath, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!CDO) return -1;

	auto Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("asset_path"), AssetPath);
	Props->SetStringField(TEXT("class"), CDO->GetClass()->GetName());

	// Tag containers
	Props->SetArrayField(TEXT("ability_tags"), ExtractTagContainer(CDO, TEXT("AbilityTags")));
	Props->SetArrayField(TEXT("cancel_abilities_with_tag"), ExtractTagContainer(CDO, TEXT("CancelAbilitiesWithTag")));
	Props->SetArrayField(TEXT("block_abilities_with_tag"), ExtractTagContainer(CDO, TEXT("BlockAbilitiesWithTag")));
	Props->SetArrayField(TEXT("activation_required_tags"), ExtractTagContainer(CDO, TEXT("ActivationRequiredTags")));
	Props->SetArrayField(TEXT("activation_blocked_tags"), ExtractTagContainer(CDO, TEXT("ActivationBlockedTags")));

	// Instancing policy (via reflection — may be protected)
	FProperty* InstancingProp = CDO->GetClass()->FindPropertyByName(TEXT("InstancingPolicy"));
	if (InstancingProp)
	{
		FByteProperty* ByteProp = CastField<FByteProperty>(InstancingProp);
		FEnumProperty* EnumProp = CastField<FEnumProperty>(InstancingProp);
		if (EnumProp)
		{
			FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
			const void* ValuePtr = EnumProp->ContainerPtrToValuePtr<void>(CDO);
			int64 Value = UnderlyingProp->GetSignedIntPropertyValue(ValuePtr);
			const UEnum* EnumDef = EnumProp->GetEnum();
			if (EnumDef)
			{
				Props->SetStringField(TEXT("instancing_policy"), EnumDef->GetNameStringByValue(Value));
			}
		}
		else if (ByteProp && ByteProp->Enum)
		{
			uint8 Value = ByteProp->GetPropertyValue_InContainer(CDO);
			Props->SetStringField(TEXT("instancing_policy"), ByteProp->Enum->GetNameStringByValue(Value));
		}
	}

	// Net execution policy (via reflection)
	FProperty* NetExecProp = CDO->GetClass()->FindPropertyByName(TEXT("NetExecutionPolicy"));
	if (NetExecProp)
	{
		FByteProperty* ByteProp = CastField<FByteProperty>(NetExecProp);
		FEnumProperty* EnumProp = CastField<FEnumProperty>(NetExecProp);
		if (EnumProp)
		{
			FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
			const void* ValuePtr = EnumProp->ContainerPtrToValuePtr<void>(CDO);
			int64 Value = UnderlyingProp->GetSignedIntPropertyValue(ValuePtr);
			const UEnum* EnumDef = EnumProp->GetEnum();
			if (EnumDef)
			{
				Props->SetStringField(TEXT("net_execution_policy"), EnumDef->GetNameStringByValue(Value));
			}
		}
		else if (ByteProp && ByteProp->Enum)
		{
			uint8 Value = ByteProp->GetPropertyValue_InContainer(CDO);
			Props->SetStringField(TEXT("net_execution_policy"), ByteProp->Enum->GetNameStringByValue(Value));
		}
	}

	// Cost effect class
	FProperty* CostProp = CDO->GetClass()->FindPropertyByName(TEXT("CostGameplayEffectClass"));
	if (CostProp)
	{
		FClassProperty* ClassProp = CastField<FClassProperty>(CostProp);
		if (ClassProp)
		{
			UClass* CostClass = Cast<UClass>(ClassProp->GetPropertyValue_InContainer(CDO));
			Props->SetStringField(TEXT("cost_effect"), CostClass ? CostClass->GetName() : TEXT("None"));
		}
	}

	// Cooldown effect class
	FProperty* CooldownProp = CDO->GetClass()->FindPropertyByName(TEXT("CooldownGameplayEffectClass"));
	if (CooldownProp)
	{
		FClassProperty* ClassProp = CastField<FClassProperty>(CooldownProp);
		if (ClassProp)
		{
			UClass* CooldownClass = Cast<UClass>(ClassProp->GetPropertyValue_InContainer(CDO));
			Props->SetStringField(TEXT("cooldown_effect"), CooldownClass ? CooldownClass->GetName() : TEXT("None"));
		}
	}

	FIndexedNode Node;
	Node.AssetId = AssetId;
	Node.NodeName = CDO->GetClass()->GetName();
	Node.NodeClass = TEXT("GameplayAbility");
	Node.NodeType = TEXT("GameplayAbility");
	Node.Properties = JsonToString(Props);
	return DB.InsertNode(Node);
}

// ─────────────────────────────────────────────────────────────
// IndexEffect
// ─────────────────────────────────────────────────────────────

int64 FGASIndexer::IndexEffect(UObject* CDO, const FString& AssetPath, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!CDO) return -1;

	auto Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("asset_path"), AssetPath);
	Props->SetStringField(TEXT("class"), CDO->GetClass()->GetName());

	// Duration policy
	FProperty* DurationProp = CDO->GetClass()->FindPropertyByName(TEXT("DurationPolicy"));
	if (DurationProp)
	{
		FByteProperty* ByteProp = CastField<FByteProperty>(DurationProp);
		FEnumProperty* EnumProp = CastField<FEnumProperty>(DurationProp);
		if (EnumProp)
		{
			FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
			const void* ValuePtr = EnumProp->ContainerPtrToValuePtr<void>(CDO);
			int64 Value = UnderlyingProp->GetSignedIntPropertyValue(ValuePtr);
			const UEnum* EnumDef = EnumProp->GetEnum();
			if (EnumDef)
			{
				Props->SetStringField(TEXT("duration_policy"), EnumDef->GetNameStringByValue(Value));
			}
		}
		else if (ByteProp && ByteProp->Enum)
		{
			uint8 Value = ByteProp->GetPropertyValue_InContainer(CDO);
			Props->SetStringField(TEXT("duration_policy"), ByteProp->Enum->GetNameStringByValue(Value));
		}
	}

	// Period
	FProperty* PeriodProp = CDO->GetClass()->FindPropertyByName(TEXT("Period"));
	if (PeriodProp)
	{
		FFloatProperty* FloatProp = CastField<FFloatProperty>(PeriodProp);
		if (FloatProp)
		{
			Props->SetNumberField(TEXT("period"), FloatProp->GetPropertyValue_InContainer(CDO));
		}
	}

	// Stacking type
	FProperty* StackProp = CDO->GetClass()->FindPropertyByName(TEXT("StackingType"));
	if (StackProp)
	{
		FByteProperty* ByteProp = CastField<FByteProperty>(StackProp);
		FEnumProperty* EnumProp = CastField<FEnumProperty>(StackProp);
		if (EnumProp)
		{
			FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
			const void* ValuePtr = EnumProp->ContainerPtrToValuePtr<void>(CDO);
			int64 Value = UnderlyingProp->GetSignedIntPropertyValue(ValuePtr);
			const UEnum* EnumDef = EnumProp->GetEnum();
			if (EnumDef)
			{
				Props->SetStringField(TEXT("stacking_type"), EnumDef->GetNameStringByValue(Value));
			}
		}
		else if (ByteProp && ByteProp->Enum)
		{
			uint8 Value = ByteProp->GetPropertyValue_InContainer(CDO);
			Props->SetStringField(TEXT("stacking_type"), ByteProp->Enum->GetNameStringByValue(Value));
		}
	}

	// Modifiers array
	FProperty* ModsProp = CDO->GetClass()->FindPropertyByName(TEXT("Modifiers"));
	if (ModsProp)
	{
		FArrayProperty* ArrayProp = CastField<FArrayProperty>(ModsProp);
		if (ArrayProp)
		{
			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(CDO));
			TArray<TSharedPtr<FJsonValue>> ModArray;

			for (int32 i = 0; i < ArrayHelper.Num(); i++)
			{
				void* ElemPtr = ArrayHelper.GetRawPtr(i);
				FStructProperty* InnerStructProp = CastField<FStructProperty>(ArrayProp->Inner);
				if (!InnerStructProp) continue;

				auto ModObj = MakeShared<FJsonObject>();

				// Attribute
				FProperty* AttrProp = InnerStructProp->Struct->FindPropertyByName(TEXT("Attribute"));
				if (AttrProp)
				{
					FString AttrExport;
					FStructProperty* AttrStructProp = CastField<FStructProperty>(AttrProp);
					if (AttrStructProp)
					{
						const void* AttrPtr = AttrStructProp->ContainerPtrToValuePtr<void>(ElemPtr);
						AttrStructProp->ExportTextItem_Direct(AttrExport, AttrPtr, nullptr, nullptr, PPF_None);
					}
					ModObj->SetStringField(TEXT("attribute"), AttrExport);
				}

				// ModifierOp
				FProperty* OpProp = InnerStructProp->Struct->FindPropertyByName(TEXT("ModifierOp"));
				if (OpProp)
				{
					FByteProperty* ByteProp = CastField<FByteProperty>(OpProp);
					FEnumProperty* EnumProp = CastField<FEnumProperty>(OpProp);
					if (EnumProp)
					{
						FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
						const void* ValuePtr = EnumProp->ContainerPtrToValuePtr<void>(ElemPtr);
						int64 Value = UnderlyingProp->GetSignedIntPropertyValue(ValuePtr);
						const UEnum* EnumDef = EnumProp->GetEnum();
						if (EnumDef) ModObj->SetStringField(TEXT("modifier_op"), EnumDef->GetNameStringByValue(Value));
					}
					else if (ByteProp && ByteProp->Enum)
					{
						uint8 Value = ByteProp->GetPropertyValue_InContainer(ElemPtr);
						ModObj->SetStringField(TEXT("modifier_op"), ByteProp->Enum->GetNameStringByValue(Value));
					}
				}

				// ModifierMagnitude — extract magnitude type
				FProperty* MagProp = InnerStructProp->Struct->FindPropertyByName(TEXT("ModifierMagnitude"));
				if (MagProp)
				{
					FStructProperty* MagStructProp = CastField<FStructProperty>(MagProp);
					if (MagStructProp)
					{
						const void* MagPtr = MagStructProp->ContainerPtrToValuePtr<void>(ElemPtr);
						// Get MagnitudeCalculationType from FGameplayEffectModifierMagnitude
						FProperty* MagTypeProp = MagStructProp->Struct->FindPropertyByName(TEXT("MagnitudeCalculationType"));
						if (MagTypeProp)
						{
							FByteProperty* MagByteProp = CastField<FByteProperty>(MagTypeProp);
							FEnumProperty* MagEnumProp = CastField<FEnumProperty>(MagTypeProp);
							if (MagEnumProp)
							{
								FNumericProperty* UnderlyingProp = MagEnumProp->GetUnderlyingProperty();
								const void* ValuePtr = MagEnumProp->ContainerPtrToValuePtr<void>(MagPtr);
								int64 Value = UnderlyingProp->GetSignedIntPropertyValue(ValuePtr);
								const UEnum* EnumDef = MagEnumProp->GetEnum();
								if (EnumDef) ModObj->SetStringField(TEXT("magnitude_type"), EnumDef->GetNameStringByValue(Value));
							}
							else if (MagByteProp && MagByteProp->Enum)
							{
								uint8 Value = *MagByteProp->ContainerPtrToValuePtr<uint8>(MagPtr);
								ModObj->SetStringField(TEXT("magnitude_type"), MagByteProp->Enum->GetNameStringByValue(Value));
							}
						}
					}
				}

				ModArray.Add(MakeShared<FJsonValueObject>(ModObj));
			}
			Props->SetArrayField(TEXT("modifiers"), ModArray);
		}
	}

	// GameplayCues array
	FProperty* CuesProp = CDO->GetClass()->FindPropertyByName(TEXT("GameplayCues"));
	if (CuesProp)
	{
		FArrayProperty* CuesArrayProp = CastField<FArrayProperty>(CuesProp);
		if (CuesArrayProp)
		{
			FScriptArrayHelper CueHelper(CuesArrayProp, CuesArrayProp->ContainerPtrToValuePtr<void>(CDO));
			TArray<TSharedPtr<FJsonValue>> CueArray;

			for (int32 i = 0; i < CueHelper.Num(); i++)
			{
				void* CueElemPtr = CueHelper.GetRawPtr(i);
				FStructProperty* CueInnerProp = CastField<FStructProperty>(CuesArrayProp->Inner);
				if (!CueInnerProp) continue;

				// Extract cue tags
				FProperty* CueTagsProp = CueInnerProp->Struct->FindPropertyByName(TEXT("GameplayCueTags"));
				if (CueTagsProp)
				{
					FStructProperty* CueTagsStructProp = CastField<FStructProperty>(CueTagsProp);
					if (CueTagsStructProp)
					{
						const void* CueTagsPtr = CueTagsStructProp->ContainerPtrToValuePtr<void>(CueElemPtr);
						const FGameplayTagContainer* CueTagContainer = static_cast<const FGameplayTagContainer*>(CueTagsPtr);
						if (CueTagContainer)
						{
							for (const FGameplayTag& Tag : *CueTagContainer)
							{
								CueArray.Add(MakeShared<FJsonValueString>(Tag.ToString()));
							}
						}
					}
				}
			}
			Props->SetArrayField(TEXT("gameplay_cues"), CueArray);
		}
	}

	// GE Components via default subobjects
	{
		TArray<TSharedPtr<FJsonValue>> ComponentArray;
		TArray<UObject*> SubObjects;
		CDO->GetDefaultSubobjects(SubObjects);
		for (UObject* SubObj : SubObjects)
		{
			if (SubObj)
			{
				ComponentArray.Add(MakeShared<FJsonValueString>(SubObj->GetClass()->GetName()));
			}
		}
		if (ComponentArray.Num() > 0)
		{
			Props->SetArrayField(TEXT("components"), ComponentArray);
		}
	}

	FIndexedNode Node;
	Node.AssetId = AssetId;
	Node.NodeName = CDO->GetClass()->GetName();
	Node.NodeClass = TEXT("GameplayEffect");
	Node.NodeType = TEXT("GameplayEffect");
	Node.Properties = JsonToString(Props);
	return DB.InsertNode(Node);
}

// ─────────────────────────────────────────────────────────────
// IndexAttributeSet
// ─────────────────────────────────────────────────────────────

int64 FGASIndexer::IndexAttributeSet(UClass* Class, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!Class) return -1;

	UObject* CDO = Class->GetDefaultObject(false);
	if (!CDO) return -1;

	auto Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("class"), Class->GetName());
	Props->SetStringField(TEXT("parent_class"), Class->GetSuperClass() ? Class->GetSuperClass()->GetName() : TEXT("None"));
	Props->SetBoolField(TEXT("is_native"), Class->ClassGeneratedBy == nullptr);

	TArray<TSharedPtr<FJsonValue>> Attributes;
	for (TFieldIterator<FProperty> It(Class); It; ++It)
	{
		FStructProperty* StructProp = CastField<FStructProperty>(*It);
		if (!StructProp || !StructProp->Struct) continue;
		if (StructProp->Struct->GetName() != TEXT("GameplayAttributeData")) continue;

		auto AttrObj = MakeShared<FJsonObject>();
		AttrObj->SetStringField(TEXT("name"), It->GetName());

		// Extract default value via reflection
		const void* ValuePtr = StructProp->ContainerPtrToValuePtr<void>(CDO);
		// FGameplayAttributeData has BaseValue and CurrentValue (both float)
		FProperty* BaseValueProp = StructProp->Struct->FindPropertyByName(TEXT("BaseValue"));
		if (BaseValueProp)
		{
			FFloatProperty* FloatProp = CastField<FFloatProperty>(BaseValueProp);
			FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(BaseValueProp);
			if (FloatProp)
			{
				float Val = *FloatProp->ContainerPtrToValuePtr<float>(ValuePtr);
				AttrObj->SetNumberField(TEXT("base_value"), Val);
			}
			else if (DoubleProp)
			{
				double Val = *DoubleProp->ContainerPtrToValuePtr<double>(ValuePtr);
				AttrObj->SetNumberField(TEXT("base_value"), Val);
			}
		}

		// Owning class for the attribute
		AttrObj->SetStringField(TEXT("owning_class"), Class->GetName());

		Attributes.Add(MakeShared<FJsonValueObject>(AttrObj));
	}
	Props->SetArrayField(TEXT("attributes"), Attributes);

	FIndexedNode Node;
	Node.AssetId = AssetId;
	Node.NodeName = Class->GetName();
	Node.NodeClass = TEXT("AttributeSet");
	Node.NodeType = TEXT("AttributeSet");
	Node.Properties = JsonToString(Props);
	return DB.InsertNode(Node);
}

// ─────────────────────────────────────────────────────────────
// IndexGameplayCue
// ─────────────────────────────────────────────────────────────

int64 FGASIndexer::IndexGameplayCue(UObject* CDO, const FString& AssetPath, FMonolithIndexDatabase& DB, int64 AssetId, bool bIsActor)
{
	if (!CDO) return -1;

	auto Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("asset_path"), AssetPath);
	Props->SetStringField(TEXT("class"), CDO->GetClass()->GetName());
	Props->SetStringField(TEXT("notify_type"), bIsActor ? TEXT("Actor") : TEXT("Static"));

	// GameplayCueTag
	FProperty* TagProp = CDO->GetClass()->FindPropertyByName(TEXT("GameplayCueTag"));
	if (TagProp)
	{
		FStructProperty* TagStructProp = CastField<FStructProperty>(TagProp);
		if (TagStructProp)
		{
			const void* TagPtr = TagStructProp->ContainerPtrToValuePtr<void>(CDO);
			const FGameplayTag* Tag = static_cast<const FGameplayTag*>(TagPtr);
			if (Tag && Tag->IsValid())
			{
				Props->SetStringField(TEXT("gameplay_cue_tag"), Tag->ToString());
			}
			else
			{
				Props->SetStringField(TEXT("gameplay_cue_tag"), TEXT("None"));
			}
		}
	}

	FIndexedNode Node;
	Node.AssetId = AssetId;
	Node.NodeName = CDO->GetClass()->GetName();
	Node.NodeClass = bIsActor ? TEXT("GameplayCueNotify_Actor") : TEXT("GameplayCueNotify_Static");
	Node.NodeType = TEXT("GameplayCue");
	Node.Properties = JsonToString(Props);
	return DB.InsertNode(Node);
}
