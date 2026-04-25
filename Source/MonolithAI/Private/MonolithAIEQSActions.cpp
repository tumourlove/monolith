#include "MonolithAIEQSActions.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/EnvQueryContext.h"
#include "EnvironmentQuery/EnvQueryTypes.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "ScopedTransaction.h"

// ============================================================
//  Helpers
// ============================================================

namespace
{
	/** Load a UEnvQuery from the asset_path param. */
	UEnvQuery* LoadEQSFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
	{
		OutAssetPath = Params->GetStringField(TEXT("asset_path"));
		if (OutAssetPath.IsEmpty())
		{
			OutError = TEXT("Missing required param: asset_path");
			return nullptr;
		}

		UObject* Obj = FMonolithAssetUtils::LoadAssetByPath(UEnvQuery::StaticClass(), OutAssetPath);
		UEnvQuery* Query = Cast<UEnvQuery>(Obj);
		if (!Query)
		{
			OutError = FString::Printf(TEXT("EQS query not found: %s"), *OutAssetPath);
			return nullptr;
		}
		return Query;
	}

	/** Get the mutable options array from UEnvQuery. Returns TArray<TObjectPtr<UEnvQueryOption>>&. */
	TArray<TObjectPtr<UEnvQueryOption>>& GetOptionsMutable(UEnvQuery* Query)
	{
		return Query->GetOptionsMutable();
	}

	/** Apply a JSON property value to a UPROPERTY on a UObject (EQS variant, no BT dependency). */
	bool SetEQSPropertyValue(UObject* Obj, const FString& PropName, const TSharedPtr<FJsonValue>& Value, FString& OutError)
	{
		if (!Obj || !Value.IsValid())
		{
			OutError = TEXT("Null object or value");
			return false;
		}

		FProperty* Prop = Obj->GetClass()->FindPropertyByName(*PropName);
		if (!Prop)
		{
			OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *PropName, *Obj->GetClass()->GetName());
			return false;
		}

		void* PropAddr = Prop->ContainerPtrToValuePtr<void>(Obj);

		// FAIDataProviderFloatValue — set DefaultValue directly
		if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			if (StructProp->Struct->GetFName() == FName(TEXT("AIDataProviderFloatValue")))
			{
				FAIDataProviderFloatValue* DataProvider = reinterpret_cast<FAIDataProviderFloatValue*>(PropAddr);
				DataProvider->DefaultValue = (float)Value->AsNumber();
				return true;
			}
			if (StructProp->Struct->GetFName() == FName(TEXT("AIDataProviderBoolValue")))
			{
				FAIDataProviderBoolValue* DataProvider = reinterpret_cast<FAIDataProviderBoolValue*>(PropAddr);
				DataProvider->DefaultValue = Value->AsBool();
				return true;
			}
			// Generic struct — try ImportText
			FString StructStr = Value->AsString();
			const TCHAR* Buffer = *StructStr;
			if (StructProp->ImportText_Direct(Buffer, PropAddr, Obj, PPF_None) != nullptr)
			{
				return true;
			}
			OutError = FString::Printf(TEXT("Failed to import text for struct property '%s'"), *PropName);
			return false;
		}

		// Bool
		if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			BoolProp->SetPropertyValue(PropAddr, Value->AsBool());
			return true;
		}

		// Numeric
		if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
		{
			IntProp->SetPropertyValue(PropAddr, (int32)Value->AsNumber());
			return true;
		}
		if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{
			FloatProp->SetPropertyValue(PropAddr, (float)Value->AsNumber());
			return true;
		}
		if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{
			DoubleProp->SetPropertyValue(PropAddr, Value->AsNumber());
			return true;
		}

		// String types
		if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			StrProp->SetPropertyValue(PropAddr, Value->AsString());
			return true;
		}
		if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			NameProp->SetPropertyValue(PropAddr, FName(*Value->AsString()));
			return true;
		}

		// Enum (byte)
		if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (ByteProp->Enum)
			{
				int64 EnumVal = ByteProp->Enum->GetValueByNameString(Value->AsString());
				if (EnumVal == INDEX_NONE)
				{
					ByteProp->SetPropertyValue(PropAddr, (uint8)Value->AsNumber());
				}
				else
				{
					ByteProp->SetPropertyValue(PropAddr, (uint8)EnumVal);
				}
			}
			else
			{
				ByteProp->SetPropertyValue(PropAddr, (uint8)Value->AsNumber());
			}
			return true;
		}
		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			UEnum* Enum = EnumProp->GetEnum();
			if (Enum)
			{
				int64 EnumVal = Enum->GetValueByNameString(Value->AsString());
				if (EnumVal != INDEX_NONE)
				{
					FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
					UnderlyingProp->SetIntPropertyValue(PropAddr, EnumVal);
					return true;
				}
			}
			OutError = FString::Printf(TEXT("Could not resolve enum value '%s' for property '%s'"), *Value->AsString(), *PropName);
			return false;
		}

		// Object reference
		if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			FString ObjPath = Value->AsString();
			if (ObjPath.IsEmpty())
			{
				ObjProp->SetPropertyValue(PropAddr, nullptr);
				return true;
			}
			UObject* RefObj = FMonolithAssetUtils::LoadAssetByPath(ObjProp->PropertyClass, ObjPath);
			if (!RefObj)
			{
				OutError = FString::Printf(TEXT("Object not found: %s"), *ObjPath);
				return false;
			}
			ObjProp->SetPropertyValue(PropAddr, RefObj);
			return true;
		}

		// Soft class reference
		if (FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop))
		{
			FString ClassPath = Value->AsString();
			FSoftObjectPath SoftPath(ClassPath);
			FSoftObjectPtr SoftPtr(SoftPath);
			SoftClassProp->SetPropertyValue(PropAddr, SoftPtr);
			return true;
		}

		OutError = FString::Printf(TEXT("Unsupported property type for '%s': %s"), *PropName, *Prop->GetClass()->GetName());
		return false;
	}

	/** Apply a JSON object of property_name→value pairs to a UObject. */
	void ApplyEQSProperties(UObject* Obj, const TSharedPtr<FJsonObject>& PropsObj, TArray<FString>& OutErrors)
	{
		if (!Obj || !PropsObj.IsValid()) return;

		for (const auto& Pair : PropsObj->Values)
		{
			FString Error;
			if (!SetEQSPropertyValue(Obj, Pair.Key, Pair.Value, Error))
			{
				OutErrors.Add(Error);
			}
		}
	}

	/** Serialize all UPROPERTYs of a UObject to JSON. */
	TSharedPtr<FJsonObject> SerializeEQSProperties(UObject* Obj)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Obj) return Result;

		for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
		{
			FProperty* Prop = *It;
			if (Prop->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)) continue;

			void* PropAddr = Prop->ContainerPtrToValuePtr<void>(Obj);
			const FString PropName = Prop->GetName();

			// FAIDataProviderFloatValue — serialize DefaultValue
			if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				if (StructProp->Struct->GetFName() == FName(TEXT("AIDataProviderFloatValue")))
				{
					const FAIDataProviderFloatValue* DataProvider = reinterpret_cast<const FAIDataProviderFloatValue*>(PropAddr);
					Result->SetNumberField(PropName, DataProvider->DefaultValue);
					continue;
				}
				if (StructProp->Struct->GetFName() == FName(TEXT("AIDataProviderBoolValue")))
				{
					const FAIDataProviderBoolValue* DataProvider = reinterpret_cast<const FAIDataProviderBoolValue*>(PropAddr);
					Result->SetBoolField(PropName, DataProvider->DefaultValue);
					continue;
				}
				// Generic struct — ExportText
				FString ExportedText;
				Prop->ExportTextItem_Direct(ExportedText, PropAddr, nullptr, Obj, PPF_None);
				if (!ExportedText.IsEmpty())
				{
					Result->SetStringField(PropName, ExportedText);
				}
				continue;
			}

			if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
			{
				Result->SetBoolField(PropName, BoolProp->GetPropertyValue(PropAddr));
				continue;
			}

			if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
			{
				Result->SetNumberField(PropName, IntProp->GetPropertyValue(PropAddr));
				continue;
			}
			if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
			{
				Result->SetNumberField(PropName, FloatProp->GetPropertyValue(PropAddr));
				continue;
			}
			if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
			{
				Result->SetNumberField(PropName, DoubleProp->GetPropertyValue(PropAddr));
				continue;
			}

			if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
			{
				Result->SetStringField(PropName, StrProp->GetPropertyValue(PropAddr));
				continue;
			}
			if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
			{
				Result->SetStringField(PropName, NameProp->GetPropertyValue(PropAddr).ToString());
				continue;
			}

			if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
			{
				if (ByteProp->Enum)
				{
					FString EnumStr = ByteProp->Enum->GetNameStringByValue(ByteProp->GetPropertyValue(PropAddr));
					Result->SetStringField(PropName, EnumStr);
				}
				else
				{
					Result->SetNumberField(PropName, ByteProp->GetPropertyValue(PropAddr));
				}
				continue;
			}
			if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
			{
				UEnum* Enum = EnumProp->GetEnum();
				FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
				int64 Val = 0;
				UnderlyingProp->GetValue_InContainer(Obj, &Val);
				if (Enum)
				{
					Result->SetStringField(PropName, Enum->GetNameStringByValue(Val));
				}
				else
				{
					Result->SetNumberField(PropName, (double)Val);
				}
				continue;
			}

			if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
			{
				UObject* RefObj = ObjProp->GetPropertyValue(PropAddr);
				if (RefObj)
				{
					Result->SetStringField(PropName, RefObj->GetPathName());
				}
				continue;
			}

			if (FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop))
			{
				FSoftObjectPtr SoftRef;
				SoftClassProp->GetValue_InContainer(Obj, &SoftRef);
				if (!SoftRef.IsNull())
				{
					Result->SetStringField(PropName, SoftRef.ToSoftObjectPath().ToString());
				}
				continue;
			}
		}

		return Result;
	}

	/** Serialize a single EQS test to JSON */
	TSharedPtr<FJsonObject> SerializeEQSTest(const UEnvQueryTest* Test, int32 TestIndex)
	{
		TSharedPtr<FJsonObject> TestObj = MakeShared<FJsonObject>();
		if (!Test) return TestObj;

		TestObj->SetNumberField(TEXT("test_index"), TestIndex);
		TestObj->SetStringField(TEXT("test_class"), Test->GetClass()->GetName());

		// Scoring config
		TestObj->SetStringField(TEXT("test_purpose"), StaticEnum<EEnvTestPurpose::Type>()->GetNameStringByValue((int64)Test->TestPurpose));
		TestObj->SetStringField(TEXT("scoring_equation"), StaticEnum<EEnvTestScoreEquation::Type>()->GetNameStringByValue((int64)Test->ScoringEquation));
		TestObj->SetNumberField(TEXT("scoring_factor"), Test->ScoringFactor.DefaultValue);
		TestObj->SetStringField(TEXT("filter_type"), StaticEnum<EEnvTestFilterType::Type>()->GetNameStringByValue((int64)Test->FilterType));

		// Filter values
		TestObj->SetNumberField(TEXT("float_value_min"), Test->FloatValueMin.DefaultValue);
		TestObj->SetNumberField(TEXT("float_value_max"), Test->FloatValueMax.DefaultValue);
		TestObj->SetBoolField(TEXT("bool_value"), Test->BoolValue.DefaultValue);

		// Clamp
		TestObj->SetStringField(TEXT("clamp_min_type"), StaticEnum<EEnvQueryTestClamping::Type>()->GetNameStringByValue((int64)Test->ClampMinType));
		TestObj->SetStringField(TEXT("clamp_max_type"), StaticEnum<EEnvQueryTestClamping::Type>()->GetNameStringByValue((int64)Test->ClampMaxType));
		TestObj->SetNumberField(TEXT("score_clamp_min"), Test->ScoreClampMin.DefaultValue);
		TestObj->SetNumberField(TEXT("score_clamp_max"), Test->ScoreClampMax.DefaultValue);

		// Normalization
		TestObj->SetStringField(TEXT("normalization_type"), StaticEnum<EEQSNormalizationType>()->GetNameStringByValue((int64)Test->NormalizationType));

		// Reference value
		TestObj->SetBoolField(TEXT("define_reference_value"), Test->bDefineReferenceValue);
		TestObj->SetNumberField(TEXT("reference_value"), Test->ReferenceValue.DefaultValue);

		// All properties
		TestObj->SetObjectField(TEXT("properties"), SerializeEQSProperties(const_cast<UEnvQueryTest*>(Test)));

		return TestObj;
	}

	/** Serialize a single EQS option (generator + tests) to JSON */
	TSharedPtr<FJsonObject> SerializeEQSOption(const UEnvQueryOption* Option, int32 OptionIndex)
	{
		TSharedPtr<FJsonObject> OptionObj = MakeShared<FJsonObject>();
		if (!Option) return OptionObj;

		OptionObj->SetNumberField(TEXT("option_index"), OptionIndex);

		// Generator
		if (Option->Generator)
		{
			TSharedPtr<FJsonObject> GenObj = MakeShared<FJsonObject>();
			GenObj->SetStringField(TEXT("generator_class"), Option->Generator->GetClass()->GetName());
			GenObj->SetStringField(TEXT("item_type"), Option->Generator->ItemType ? Option->Generator->ItemType->GetName() : TEXT("None"));
			GenObj->SetObjectField(TEXT("properties"), SerializeEQSProperties(Option->Generator));
			OptionObj->SetObjectField(TEXT("generator"), GenObj);
		}

		// Tests
		TArray<TSharedPtr<FJsonValue>> TestsArr;
		for (int32 i = 0; i < Option->Tests.Num(); ++i)
		{
			TestsArr.Add(MakeShared<FJsonValueObject>(SerializeEQSTest(Option->Tests[i], i)));
		}
		OptionObj->SetArrayField(TEXT("tests"), TestsArr);
		OptionObj->SetNumberField(TEXT("test_count"), Option->Tests.Num());

		return OptionObj;
	}

	/** Find a UClass by name, checking derived classes of a base. Returns nullptr if not found. */
	UClass* FindEQSClassByName(UClass* BaseClass, const FString& ClassName)
	{
		if (!BaseClass) return nullptr;

		// Try exact match first
		UClass* Found = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::EnsureIfAmbiguous);
		if (Found && Found->IsChildOf(BaseClass))
		{
			return Found;
		}

		// Try with common prefixes
		TArray<FString> Candidates = {
			ClassName,
			FString::Printf(TEXT("U%s"), *ClassName),
			FString::Printf(TEXT("EnvQueryGenerator_%s"), *ClassName),
			FString::Printf(TEXT("UEnvQueryGenerator_%s"), *ClassName),
			FString::Printf(TEXT("EnvQueryTest_%s"), *ClassName),
			FString::Printf(TEXT("UEnvQueryTest_%s"), *ClassName),
			FString::Printf(TEXT("EnvQueryContext_%s"), *ClassName),
			FString::Printf(TEXT("UEnvQueryContext_%s"), *ClassName),
		};

		for (const FString& Candidate : Candidates)
		{
			Found = FindFirstObject<UClass>(*Candidate, EFindFirstObjectOptions::EnsureIfAmbiguous);
			if (Found && Found->IsChildOf(BaseClass))
			{
				return Found;
			}
		}

		// Walk all derived classes as fallback
		TArray<UClass*> DerivedClasses;
		GetDerivedClasses(BaseClass, DerivedClasses);
		for (UClass* Derived : DerivedClasses)
		{
			if (Derived->GetName() == ClassName || Derived->GetName().EndsWith(ClassName))
			{
				return Derived;
			}
		}

		return nullptr;
	}

	/** Get option by index with validation. */
	bool GetOptionByIndex(UEnvQuery* Query, int32 OptionIndex, UEnvQueryOption*& OutOption, FString& OutError)
	{
		const TArray<UEnvQueryOption*>& Options = Query->GetOptions();
		if (OptionIndex < 0 || OptionIndex >= Options.Num())
		{
			OutError = FString::Printf(TEXT("Option index %d out of range [0, %d)"), OptionIndex, Options.Num());
			return false;
		}
		OutOption = Options[OptionIndex];
		if (!OutOption)
		{
			OutError = FString::Printf(TEXT("Option at index %d is null"), OptionIndex);
			return false;
		}
		return true;
	}

	/** Get test by option+test index with validation. */
	bool GetTestByIndex(UEnvQuery* Query, int32 OptionIndex, int32 TestIndex, UEnvQueryOption*& OutOption, UEnvQueryTest*& OutTest, FString& OutError)
	{
		if (!GetOptionByIndex(Query, OptionIndex, OutOption, OutError))
		{
			return false;
		}
		if (TestIndex < 0 || TestIndex >= OutOption->Tests.Num())
		{
			OutError = FString::Printf(TEXT("Test index %d out of range [0, %d) for option %d"), TestIndex, OutOption->Tests.Num(), OptionIndex);
			return false;
		}
		OutTest = OutOption->Tests[TestIndex];
		if (!OutTest)
		{
			OutError = FString::Printf(TEXT("Test at index %d in option %d is null"), TestIndex, OptionIndex);
			return false;
		}
		return true;
	}

	/** Collect all classes derived from a base, returning name + tooltip. */
	TArray<TSharedPtr<FJsonValue>> EnumerateDerivedClasses(UClass* BaseClass, bool bIncludeAbstract = false)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		TArray<UClass*> DerivedClasses;
		GetDerivedClasses(BaseClass, DerivedClasses);

		for (UClass* Cls : DerivedClasses)
		{
			if (!Cls) continue;
			if (!bIncludeAbstract && Cls->HasAnyClassFlags(CLASS_Abstract)) continue;
			if (Cls->HasAnyClassFlags(CLASS_Deprecated)) continue;

			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("class_name"), Cls->GetName());
			Item->SetStringField(TEXT("display_name"), Cls->GetName());

			// Get tooltip from metadata if available
#if WITH_EDITOR
			const FString& Tooltip = Cls->GetMetaData(TEXT("ToolTip"));
			if (!Tooltip.IsEmpty())
			{
				Item->SetStringField(TEXT("description"), Tooltip);
			}
#endif

			Items.Add(MakeShared<FJsonValueObject>(Item));
		}

		return Items;
	}
}

// ============================================================
//  Registration
// ============================================================

void FMonolithAIEQSActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 71. create_eqs_query
	Registry.RegisterAction(TEXT("ai"), TEXT("create_eqs_query"),
		TEXT("Create an empty UEnvQuery data asset"),
		FMonolithActionHandler::CreateStatic(&HandleCreateEQSQuery),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path (e.g. /Game/AI/EQS_FindCover)"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Asset name (derived from save_path if omitted)"))
			.Build());

	// 72. get_eqs_query
	Registry.RegisterAction(TEXT("ai"), TEXT("get_eqs_query"),
		TEXT("Full JSON: options, generators, tests, scoring config"),
		FMonolithActionHandler::CreateStatic(&HandleGetEQSQuery),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("EQS query asset path"))
			.Build());

	// 73. list_eqs_queries
	Registry.RegisterAction(TEXT("ai"), TEXT("list_eqs_queries"),
		TEXT("List all UEnvQuery assets in the project"),
		FMonolithActionHandler::CreateStatic(&HandleListEQSQueries),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Only include assets under this path prefix"))
			.Build());

	// 74. delete_eqs_query
	Registry.RegisterAction(TEXT("ai"), TEXT("delete_eqs_query"),
		TEXT("Delete an EQS query asset"),
		FMonolithActionHandler::CreateStatic(&HandleDeleteEQSQuery),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("EQS query asset path to delete"))
			.Build());

	// 75. duplicate_eqs_query
	Registry.RegisterAction(TEXT("ai"), TEXT("duplicate_eqs_query"),
		TEXT("Deep copy an EQS query asset to a new path"),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateEQSQuery),
		FParamSchemaBuilder()
			.Required(TEXT("source_path"), TEXT("string"), TEXT("Source EQS query asset path"))
			.Required(TEXT("dest_path"), TEXT("string"), TEXT("Destination asset path for the copy"))
			.Build());

	// 76. add_eqs_generator
	Registry.RegisterAction(TEXT("ai"), TEXT("add_eqs_generator"),
		TEXT("Add a new option with a generator to an EQS query"),
		FMonolithActionHandler::CreateStatic(&HandleAddEQSGenerator),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("EQS query asset path"))
			.Required(TEXT("generator_class"), TEXT("string"), TEXT("Generator class name (e.g. EnvQueryGenerator_SimpleGrid)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("JSON object of property_name→value pairs to set on the generator"))
			.Build());

	// 77. remove_eqs_generator
	Registry.RegisterAction(TEXT("ai"), TEXT("remove_eqs_generator"),
		TEXT("Remove an option (generator + its tests) at the given index"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveEQSGenerator),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("EQS query asset path"))
			.Required(TEXT("option_index"), TEXT("number"), TEXT("Index of the option to remove"))
			.Build());

	// 78. configure_eqs_generator
	Registry.RegisterAction(TEXT("ai"), TEXT("configure_eqs_generator"),
		TEXT("Set properties on a generator in an EQS query option"),
		FMonolithActionHandler::CreateStatic(&HandleConfigureEQSGenerator),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("EQS query asset path"))
			.Required(TEXT("option_index"), TEXT("number"), TEXT("Index of the option containing the generator"))
			.Required(TEXT("properties"), TEXT("object"), TEXT("JSON object of property_name→value pairs"))
			.Build());

	// 79. add_eqs_test
	Registry.RegisterAction(TEXT("ai"), TEXT("add_eqs_test"),
		TEXT("Add a test to an EQS query option"),
		FMonolithActionHandler::CreateStatic(&HandleAddEQSTest),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("EQS query asset path"))
			.Required(TEXT("option_index"), TEXT("number"), TEXT("Index of the option to add the test to"))
			.Required(TEXT("test_class"), TEXT("string"), TEXT("Test class name (e.g. EnvQueryTest_Distance)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("JSON object of property_name→value pairs to set on the test"))
			.Build());

	// 80. remove_eqs_test
	Registry.RegisterAction(TEXT("ai"), TEXT("remove_eqs_test"),
		TEXT("Remove a test from an EQS query option"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveEQSTest),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("EQS query asset path"))
			.Required(TEXT("option_index"), TEXT("number"), TEXT("Index of the option"))
			.Required(TEXT("test_index"), TEXT("number"), TEXT("Index of the test to remove"))
			.Build());

	// 81. configure_eqs_test
	Registry.RegisterAction(TEXT("ai"), TEXT("configure_eqs_test"),
		TEXT("Set properties on a test in an EQS query option"),
		FMonolithActionHandler::CreateStatic(&HandleConfigureEQSTest),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("EQS query asset path"))
			.Required(TEXT("option_index"), TEXT("number"), TEXT("Index of the option"))
			.Required(TEXT("test_index"), TEXT("number"), TEXT("Index of the test"))
			.Required(TEXT("properties"), TEXT("object"), TEXT("JSON object of property_name→value pairs"))
			.Build());

	// 82. configure_eqs_scoring
	Registry.RegisterAction(TEXT("ai"), TEXT("configure_eqs_scoring"),
		TEXT("Configure scoring on a test: purpose, equation, factor, clamp, normalization"),
		FMonolithActionHandler::CreateStatic(&HandleConfigureEQSScoring),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("EQS query asset path"))
			.Required(TEXT("option_index"), TEXT("number"), TEXT("Index of the option"))
			.Required(TEXT("test_index"), TEXT("number"), TEXT("Index of the test"))
			.Optional(TEXT("purpose"), TEXT("string"), TEXT("EEnvTestPurpose: Filter, Score, FilterAndScore"))
			.Optional(TEXT("equation"), TEXT("string"), TEXT("EEnvTestScoreEquation: Linear, InverseLinear, Square, Constant"))
			.Optional(TEXT("factor"), TEXT("number"), TEXT("Scoring factor (FAIDataProviderFloatValue.DefaultValue)"), { TEXT("scoring_factor") })
			.Optional(TEXT("clamp_min_type"), TEXT("string"), TEXT("EEnvQueryTestClamping::Type for min clamp"))
			.Optional(TEXT("clamp_max_type"), TEXT("string"), TEXT("EEnvQueryTestClamping::Type for max clamp"))
			.Optional(TEXT("score_clamp_min"), TEXT("number"), TEXT("Minimum score clamp value"))
			.Optional(TEXT("score_clamp_max"), TEXT("number"), TEXT("Maximum score clamp value"))
			.Optional(TEXT("normalization_type"), TEXT("string"), TEXT("EEQSNormalizationType: Absolute, RelativeToScores"))
			.Optional(TEXT("define_reference_value"), TEXT("boolean"), TEXT("Whether to use a reference value for scoring"))
			.Optional(TEXT("reference_value"), TEXT("number"), TEXT("Reference value for scoring"))
			.Build());

	// 83. configure_eqs_filter
	Registry.RegisterAction(TEXT("ai"), TEXT("configure_eqs_filter"),
		TEXT("Configure filter on a test: filter type, min/max, bool match"),
		FMonolithActionHandler::CreateStatic(&HandleConfigureEQSFilter),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("EQS query asset path"))
			.Required(TEXT("option_index"), TEXT("number"), TEXT("Index of the option"))
			.Required(TEXT("test_index"), TEXT("number"), TEXT("Index of the test"))
			.Optional(TEXT("filter_type"), TEXT("string"), TEXT("EEnvTestFilterType: Minimum, Maximum, Range, Match"))
			.Optional(TEXT("min"), TEXT("number"), TEXT("Float filter min value"), { TEXT("float_value_min"), TEXT("filter_min") })
			.Optional(TEXT("max"), TEXT("number"), TEXT("Float filter max value"), { TEXT("float_value_max"), TEXT("filter_max") })
			.Optional(TEXT("bool_value"), TEXT("boolean"), TEXT("Bool match value for Match filter type"))
			.Build());

	// 85. list_eqs_generator_types
	Registry.RegisterAction(TEXT("ai"), TEXT("list_eqs_generator_types"),
		TEXT("List all available EQS generator classes"),
		FMonolithActionHandler::CreateStatic(&HandleListEQSGeneratorTypes),
		FParamSchemaBuilder().Build());

	// 86. list_eqs_test_types
	Registry.RegisterAction(TEXT("ai"), TEXT("list_eqs_test_types"),
		TEXT("List all available EQS test classes"),
		FMonolithActionHandler::CreateStatic(&HandleListEQSTestTypes),
		FParamSchemaBuilder().Build());

	// 87. list_eqs_contexts
	Registry.RegisterAction(TEXT("ai"), TEXT("list_eqs_contexts"),
		TEXT("List all available EQS context classes"),
		FMonolithActionHandler::CreateStatic(&HandleListEQSContexts),
		FParamSchemaBuilder().Build());

	// 90. validate_eqs_query
	Registry.RegisterAction(TEXT("ai"), TEXT("validate_eqs_query"),
		TEXT("Validate an EQS query: check empty options, missing contexts, item type mismatches"),
		FMonolithActionHandler::CreateStatic(&HandleValidateEQSQuery),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("EQS query asset path"))
			.Build());

	// 84. reorder_eqs_tests
	Registry.RegisterAction(TEXT("ai"), TEXT("reorder_eqs_tests"),
		TEXT("Reorder tests within an EQS query option"),
		FMonolithActionHandler::CreateStatic(&HandleReorderEQSTests),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("EQS query asset path"))
			.Required(TEXT("option_index"), TEXT("number"), TEXT("Index of the option"))
			.Required(TEXT("new_order"), TEXT("array"), TEXT("Array of test indices in desired order, e.g. [2, 0, 1]"))
			.Build());

	// 88. build_eqs_query_from_spec
	Registry.RegisterAction(TEXT("ai"), TEXT("build_eqs_query_from_spec"),
		TEXT("Declarative full-query creation from a JSON spec with options, generators, and tests"),
		FMonolithActionHandler::CreateStatic(&HandleBuildEQSQueryFromSpec),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path for the new EQS query"))
			.Required(TEXT("spec"), TEXT("object"), TEXT("Full query spec: {options: [{generator: {class, properties?}, tests: [{class, properties?, scoring?, filter?}]}]}"))
			.Build());

	// 91. create_eqs_from_template
	Registry.RegisterAction(TEXT("ai"), TEXT("create_eqs_from_template"),
		TEXT("Create an EQS query from a preset template: find_cover, find_flank, find_patrol_point, find_nearest_item"),
		FMonolithActionHandler::CreateStatic(&HandleCreateEQSFromTemplate),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path for the new EQS query"))
			.Required(TEXT("template"), TEXT("string"), TEXT("Template name: find_cover, find_flank, find_patrol_point, find_nearest_item"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("Override template defaults (e.g. {GridSize: 2000, SpaceBetween: 300})"))
			.Build());
}

// ============================================================
//  71. create_eqs_query
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleCreateEQSQuery(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult))
	{
		return ErrResult;
	}

	FString AssetName = Params->GetStringField(TEXT("name"));
	if (AssetName.IsEmpty())
	{
		AssetName = FPackageName::GetShortName(SavePath);
	}

	FString PackagePath = SavePath;

	// Check path is free
	FString PathError;
	if (!MonolithAI::EnsureAssetPathFree(PackagePath, AssetName, PathError))
	{
		return FMonolithActionResult::Error(PathError);
	}

	FString PkgError;
	UPackage* Package = MonolithAI::GetOrCreatePackage(PackagePath, PkgError);
	if (!Package)
	{
		return FMonolithActionResult::Error(PkgError);
	}

	UEnvQuery* Query = NewObject<UEnvQuery>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!Query)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UEnvQuery object"));
	}

	FAssetRegistryModule::AssetCreated(Query);
	Query->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(PackagePath, TEXT("EQS query created"));
	Result->SetStringField(TEXT("name"), AssetName);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  72. get_eqs_query
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleGetEQSQuery(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UEnvQuery* Query = LoadEQSFromParams(Params, AssetPath, Error);
	if (!Query)
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("name"), Query->GetName());

	const TArray<UEnvQueryOption*>& Options = Query->GetOptions();
	TArray<TSharedPtr<FJsonValue>> OptionsArr;
	for (int32 i = 0; i < Options.Num(); ++i)
	{
		OptionsArr.Add(MakeShared<FJsonValueObject>(SerializeEQSOption(Options[i], i)));
	}
	Result->SetArrayField(TEXT("options"), OptionsArr);
	Result->SetNumberField(TEXT("option_count"), Options.Num());

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  73. list_eqs_queries
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleListEQSQueries(const TSharedPtr<FJsonObject>& Params)
{
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetData> Assets;
	AR.GetAssetsByClass(UEnvQuery::StaticClass()->GetClassPathName(), Assets);

	FString PathFilter = Params->GetStringField(TEXT("path_filter"));

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FAssetData& Asset : Assets)
	{
		FString AssetObjPath = Asset.GetObjectPathString();
		if (!PathFilter.IsEmpty() && !AssetObjPath.StartsWith(PathFilter))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("asset_path"), Asset.PackageName.ToString());
		Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("queries"), Items);
	Result->SetNumberField(TEXT("count"), Items.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  74. delete_eqs_query
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleDeleteEQSQuery(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("asset_path"), AssetPath, ErrResult))
	{
		return ErrResult;
	}

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(UEnvQuery::StaticClass(), AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("EQS query not found: %s"), *AssetPath));
	}

	TArray<UObject*> ObjectsToDelete;
	ObjectsToDelete.Add(Asset);

	int32 NumDeleted = ObjectTools::ForceDeleteObjects(ObjectsToDelete, /*bShowConfirmation=*/false);
	if (NumDeleted == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to delete: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("message"), TEXT("EQS query deleted"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  75. duplicate_eqs_query
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleDuplicateEQSQuery(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath, DestPath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("source_path"), SourcePath, ErrResult))
	{
		return ErrResult;
	}
	if (!MonolithAI::RequireStringParam(Params, TEXT("dest_path"), DestPath, ErrResult))
	{
		return ErrResult;
	}

	UEnvQuery* SourceQuery = Cast<UEnvQuery>(FMonolithAssetUtils::LoadAssetByPath(UEnvQuery::StaticClass(), SourcePath));
	if (!SourceQuery)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source EQS query not found: %s"), *SourcePath));
	}

	FString DestAssetName = FPackageName::GetShortName(DestPath);
	FString PkgError;
	UPackage* DestPackage = MonolithAI::GetOrCreatePackage(DestPath, PkgError);
	if (!DestPackage)
	{
		return FMonolithActionResult::Error(PkgError);
	}

	UEnvQuery* NewQuery = Cast<UEnvQuery>(
		StaticDuplicateObject(SourceQuery, DestPackage, *DestAssetName));
	if (!NewQuery)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to duplicate '%s' to '%s'"), *SourcePath, *DestPath));
	}

	NewQuery->SetFlags(RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(NewQuery);
	NewQuery->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source_path"), SourcePath);
	Result->SetStringField(TEXT("dest_path"), DestPath);
	Result->SetStringField(TEXT("message"), TEXT("EQS query duplicated"));
	Result->SetNumberField(TEXT("option_count"), NewQuery->GetOptions().Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  76. add_eqs_generator
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleAddEQSGenerator(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UEnvQuery* Query = LoadEQSFromParams(Params, AssetPath, Error);
	if (!Query)
	{
		return FMonolithActionResult::Error(Error);
	}

	FString GeneratorClassName;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("generator_class"), GeneratorClassName, ErrResult))
	{
		return ErrResult;
	}

	UClass* GenClass = FindEQSClassByName(UEnvQueryGenerator::StaticClass(), GeneratorClassName);
	if (!GenClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Generator class not found: %s. Use list_eqs_generator_types to see available classes."), *GeneratorClassName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add EQS Generator")));
	Query->Modify();

	// Create option and generator
	UEnvQueryOption* NewOption = NewObject<UEnvQueryOption>(Query);
	UEnvQueryGenerator* NewGenerator = NewObject<UEnvQueryGenerator>(NewOption, GenClass);
	NewOption->Generator = NewGenerator;

	GetOptionsMutable(Query).Add(NewOption);

	// Apply properties if provided
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	TArray<FString> PropErrors;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj->IsValid())
	{
		ApplyEQSProperties(NewGenerator, *PropsObj, PropErrors);
	}

	Query->MarkPackageDirty();

	int32 OptionIndex = Query->GetOptions().Num() - 1;
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("option_index"), OptionIndex);
	Result->SetStringField(TEXT("generator_class"), GenClass->GetName());
	Result->SetStringField(TEXT("message"), TEXT("Generator added"));

	if (PropErrors.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ErrArr;
		for (const FString& Err : PropErrors)
		{
			ErrArr.Add(MakeShared<FJsonValueString>(Err));
		}
		Result->SetArrayField(TEXT("property_warnings"), ErrArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  77. remove_eqs_generator
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleRemoveEQSGenerator(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UEnvQuery* Query = LoadEQSFromParams(Params, AssetPath, Error);
	if (!Query)
	{
		return FMonolithActionResult::Error(Error);
	}

	int32 OptionIndex = (int32)Params->GetNumberField(TEXT("option_index"));

	UEnvQueryOption* Option = nullptr;
	if (!GetOptionByIndex(Query, OptionIndex, Option, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Remove EQS Generator")));
	Query->Modify();

	FString RemovedClassName = Option->Generator ? Option->Generator->GetClass()->GetName() : TEXT("None");
	GetOptionsMutable(Query).RemoveAt(OptionIndex);

	Query->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("removed_option_index"), OptionIndex);
	Result->SetStringField(TEXT("removed_generator_class"), RemovedClassName);
	Result->SetStringField(TEXT("message"), TEXT("Option removed"));
	Result->SetNumberField(TEXT("remaining_options"), Query->GetOptions().Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  78. configure_eqs_generator
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleConfigureEQSGenerator(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UEnvQuery* Query = LoadEQSFromParams(Params, AssetPath, Error);
	if (!Query)
	{
		return FMonolithActionResult::Error(Error);
	}

	int32 OptionIndex = (int32)Params->GetNumberField(TEXT("option_index"));

	UEnvQueryOption* Option = nullptr;
	if (!GetOptionByIndex(Query, OptionIndex, Option, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	if (!Option->Generator)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Option %d has no generator"), OptionIndex));
	}

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("properties"), PropsObj) || !PropsObj->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: properties"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Configure EQS Generator")));
	Query->Modify();

	TArray<FString> PropErrors;
	ApplyEQSProperties(Option->Generator, *PropsObj, PropErrors);

	Query->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("option_index"), OptionIndex);
	Result->SetStringField(TEXT("generator_class"), Option->Generator->GetClass()->GetName());
	Result->SetStringField(TEXT("message"), TEXT("Generator configured"));

	if (PropErrors.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ErrArr;
		for (const FString& Err : PropErrors)
		{
			ErrArr.Add(MakeShared<FJsonValueString>(Err));
		}
		Result->SetArrayField(TEXT("property_warnings"), ErrArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  79. add_eqs_test
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleAddEQSTest(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UEnvQuery* Query = LoadEQSFromParams(Params, AssetPath, Error);
	if (!Query)
	{
		return FMonolithActionResult::Error(Error);
	}

	int32 OptionIndex = (int32)Params->GetNumberField(TEXT("option_index"));

	UEnvQueryOption* Option = nullptr;
	if (!GetOptionByIndex(Query, OptionIndex, Option, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString TestClassName;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("test_class"), TestClassName, ErrResult))
	{
		return ErrResult;
	}

	UClass* TestClass = FindEQSClassByName(UEnvQueryTest::StaticClass(), TestClassName);
	if (!TestClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Test class not found: %s. Use list_eqs_test_types to see available classes."), *TestClassName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add EQS Test")));
	Query->Modify();

	UEnvQueryTest* NewTest = NewObject<UEnvQueryTest>(Option, TestClass);
	Option->Tests.Add(NewTest);

	// Apply properties if provided
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	TArray<FString> PropErrors;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj->IsValid())
	{
		ApplyEQSProperties(NewTest, *PropsObj, PropErrors);
	}

	Query->MarkPackageDirty();

	int32 TestIndex = Option->Tests.Num() - 1;
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("option_index"), OptionIndex);
	Result->SetNumberField(TEXT("test_index"), TestIndex);
	Result->SetStringField(TEXT("test_class"), TestClass->GetName());
	Result->SetStringField(TEXT("message"), TEXT("Test added"));

	if (PropErrors.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ErrArr;
		for (const FString& Err : PropErrors)
		{
			ErrArr.Add(MakeShared<FJsonValueString>(Err));
		}
		Result->SetArrayField(TEXT("property_warnings"), ErrArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  80. remove_eqs_test
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleRemoveEQSTest(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UEnvQuery* Query = LoadEQSFromParams(Params, AssetPath, Error);
	if (!Query)
	{
		return FMonolithActionResult::Error(Error);
	}

	int32 OptionIndex = (int32)Params->GetNumberField(TEXT("option_index"));
	int32 TestIndex = (int32)Params->GetNumberField(TEXT("test_index"));

	UEnvQueryOption* Option = nullptr;
	UEnvQueryTest* Test = nullptr;
	if (!GetTestByIndex(Query, OptionIndex, TestIndex, Option, Test, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Remove EQS Test")));
	Query->Modify();

	FString RemovedClassName = Test->GetClass()->GetName();
	Option->Tests.RemoveAt(TestIndex);

	Query->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("option_index"), OptionIndex);
	Result->SetNumberField(TEXT("removed_test_index"), TestIndex);
	Result->SetStringField(TEXT("removed_test_class"), RemovedClassName);
	Result->SetStringField(TEXT("message"), TEXT("Test removed"));
	Result->SetNumberField(TEXT("remaining_tests"), Option->Tests.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  81. configure_eqs_test
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleConfigureEQSTest(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UEnvQuery* Query = LoadEQSFromParams(Params, AssetPath, Error);
	if (!Query)
	{
		return FMonolithActionResult::Error(Error);
	}

	int32 OptionIndex = (int32)Params->GetNumberField(TEXT("option_index"));
	int32 TestIndex = (int32)Params->GetNumberField(TEXT("test_index"));

	UEnvQueryOption* Option = nullptr;
	UEnvQueryTest* Test = nullptr;
	if (!GetTestByIndex(Query, OptionIndex, TestIndex, Option, Test, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("properties"), PropsObj) || !PropsObj->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: properties"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Configure EQS Test")));
	Query->Modify();

	TArray<FString> PropErrors;
	ApplyEQSProperties(Test, *PropsObj, PropErrors);

	Query->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("option_index"), OptionIndex);
	Result->SetNumberField(TEXT("test_index"), TestIndex);
	Result->SetStringField(TEXT("test_class"), Test->GetClass()->GetName());
	Result->SetStringField(TEXT("message"), TEXT("Test configured"));

	if (PropErrors.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ErrArr;
		for (const FString& Err : PropErrors)
		{
			ErrArr.Add(MakeShared<FJsonValueString>(Err));
		}
		Result->SetArrayField(TEXT("property_warnings"), ErrArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  82. configure_eqs_scoring
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleConfigureEQSScoring(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UEnvQuery* Query = LoadEQSFromParams(Params, AssetPath, Error);
	if (!Query)
	{
		return FMonolithActionResult::Error(Error);
	}

	int32 OptionIndex = (int32)Params->GetNumberField(TEXT("option_index"));
	int32 TestIndex = (int32)Params->GetNumberField(TEXT("test_index"));

	UEnvQueryOption* Option = nullptr;
	UEnvQueryTest* Test = nullptr;
	if (!GetTestByIndex(Query, OptionIndex, TestIndex, Option, Test, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Configure EQS Scoring")));
	Query->Modify();

	// TestPurpose
	FString PurposeStr = Params->GetStringField(TEXT("purpose"));
	if (!PurposeStr.IsEmpty())
	{
		int64 Val = StaticEnum<EEnvTestPurpose::Type>()->GetValueByNameString(PurposeStr);
		if (Val != INDEX_NONE)
		{
			Test->TestPurpose = (EEnvTestPurpose::Type)Val;
		}
		else
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid purpose: '%s'. Use: Filter, Score, FilterAndScore"), *PurposeStr));
		}
	}

	// ScoringEquation
	FString EquationStr = Params->GetStringField(TEXT("equation"));
	if (!EquationStr.IsEmpty())
	{
		int64 Val = StaticEnum<EEnvTestScoreEquation::Type>()->GetValueByNameString(EquationStr);
		if (Val != INDEX_NONE)
		{
			Test->ScoringEquation = (EEnvTestScoreEquation::Type)Val;
		}
		else
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid equation: '%s'. Use: Linear, InverseLinear, Square, Constant"), *EquationStr));
		}
	}

	// ScoringFactor
	bool bRequestedFactor = false;
	float RequestedFactor = 0.0f;
	if (Params->HasField(TEXT("factor")))
	{
		RequestedFactor = (float)Params->GetNumberField(TEXT("factor"));
		Test->ScoringFactor.DefaultValue = RequestedFactor;
		bRequestedFactor = true;
	}

	// Clamp
	FString ClampMinStr = Params->GetStringField(TEXT("clamp_min_type"));
	if (!ClampMinStr.IsEmpty())
	{
		int64 Val = StaticEnum<EEnvQueryTestClamping::Type>()->GetValueByNameString(ClampMinStr);
		if (Val != INDEX_NONE)
		{
			Test->ClampMinType = (EEnvQueryTestClamping::Type)Val;
		}
	}
	FString ClampMaxStr = Params->GetStringField(TEXT("clamp_max_type"));
	if (!ClampMaxStr.IsEmpty())
	{
		int64 Val = StaticEnum<EEnvQueryTestClamping::Type>()->GetValueByNameString(ClampMaxStr);
		if (Val != INDEX_NONE)
		{
			Test->ClampMaxType = (EEnvQueryTestClamping::Type)Val;
		}
	}

	if (Params->HasField(TEXT("score_clamp_min")))
	{
		Test->ScoreClampMin.DefaultValue = (float)Params->GetNumberField(TEXT("score_clamp_min"));
	}
	if (Params->HasField(TEXT("score_clamp_max")))
	{
		Test->ScoreClampMax.DefaultValue = (float)Params->GetNumberField(TEXT("score_clamp_max"));
	}

	// Normalization
	FString NormStr = Params->GetStringField(TEXT("normalization_type"));
	if (!NormStr.IsEmpty())
	{
		int64 Val = StaticEnum<EEQSNormalizationType>()->GetValueByNameString(NormStr);
		if (Val != INDEX_NONE)
		{
			Test->NormalizationType = (EEQSNormalizationType)Val;
		}
	}

	// Reference value
	if (Params->HasField(TEXT("define_reference_value")))
	{
		Test->bDefineReferenceValue = Params->GetBoolField(TEXT("define_reference_value"));
	}
	if (Params->HasField(TEXT("reference_value")))
	{
		Test->ReferenceValue.DefaultValue = (float)Params->GetNumberField(TEXT("reference_value"));
	}

	Query->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("option_index"), OptionIndex);
	Result->SetNumberField(TEXT("test_index"), TestIndex);
	Result->SetStringField(TEXT("message"), TEXT("Scoring configured"));

	// Echo back current state
	Result->SetStringField(TEXT("test_purpose"), StaticEnum<EEnvTestPurpose::Type>()->GetNameStringByValue((int64)Test->TestPurpose));
	Result->SetStringField(TEXT("scoring_equation"), StaticEnum<EEnvTestScoreEquation::Type>()->GetNameStringByValue((int64)Test->ScoringEquation));
	const float ActualFactor = Test->ScoringFactor.DefaultValue;
	Result->SetNumberField(TEXT("scoring_factor"), ActualFactor);

	// verified_value smoking gun — readback after write so callers can confirm.
	if (bRequestedFactor)
	{
		TSharedPtr<FJsonObject> Verified = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> FactorEntry = MakeShared<FJsonObject>();
		FactorEntry->SetNumberField(TEXT("requested"), RequestedFactor);
		FactorEntry->SetNumberField(TEXT("actual"), ActualFactor);
		FactorEntry->SetBoolField(TEXT("match"), FMath::IsNearlyEqual(RequestedFactor, ActualFactor));
		Verified->SetObjectField(TEXT("scoring_factor"), FactorEntry);
		Result->SetObjectField(TEXT("verified_value"), Verified);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  83. configure_eqs_filter
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleConfigureEQSFilter(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UEnvQuery* Query = LoadEQSFromParams(Params, AssetPath, Error);
	if (!Query)
	{
		return FMonolithActionResult::Error(Error);
	}

	int32 OptionIndex = (int32)Params->GetNumberField(TEXT("option_index"));
	int32 TestIndex = (int32)Params->GetNumberField(TEXT("test_index"));

	UEnvQueryOption* Option = nullptr;
	UEnvQueryTest* Test = nullptr;
	if (!GetTestByIndex(Query, OptionIndex, TestIndex, Option, Test, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Configure EQS Filter")));
	Query->Modify();

	// FilterType
	FString FilterTypeStr = Params->GetStringField(TEXT("filter_type"));
	if (!FilterTypeStr.IsEmpty())
	{
		int64 Val = StaticEnum<EEnvTestFilterType::Type>()->GetValueByNameString(FilterTypeStr);
		if (Val != INDEX_NONE)
		{
			Test->FilterType = (EEnvTestFilterType::Type)Val;
		}
		else
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid filter_type: '%s'. Use: Minimum, Maximum, Range, Match"), *FilterTypeStr));
		}
	}

	// Float filter values
	bool bRequestedMin = false;
	float RequestedMin = 0.0f;
	if (Params->HasField(TEXT("min")))
	{
		RequestedMin = (float)Params->GetNumberField(TEXT("min"));
		Test->FloatValueMin.DefaultValue = RequestedMin;
		bRequestedMin = true;
	}
	bool bRequestedMax = false;
	float RequestedMax = 0.0f;
	if (Params->HasField(TEXT("max")))
	{
		RequestedMax = (float)Params->GetNumberField(TEXT("max"));
		Test->FloatValueMax.DefaultValue = RequestedMax;
		bRequestedMax = true;
	}

	// Bool match
	bool bRequestedBool = false;
	bool RequestedBool = false;
	if (Params->HasField(TEXT("bool_value")))
	{
		RequestedBool = Params->GetBoolField(TEXT("bool_value"));
		Test->BoolValue.DefaultValue = RequestedBool;
		bRequestedBool = true;
	}

	Query->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("option_index"), OptionIndex);
	Result->SetNumberField(TEXT("test_index"), TestIndex);
	Result->SetStringField(TEXT("message"), TEXT("Filter configured"));

	// Echo back current state
	Result->SetStringField(TEXT("filter_type"), StaticEnum<EEnvTestFilterType::Type>()->GetNameStringByValue((int64)Test->FilterType));
	const float ActualMin = Test->FloatValueMin.DefaultValue;
	const float ActualMax = Test->FloatValueMax.DefaultValue;
	const bool ActualBool = Test->BoolValue.DefaultValue;
	Result->SetNumberField(TEXT("float_value_min"), ActualMin);
	Result->SetNumberField(TEXT("float_value_max"), ActualMax);
	Result->SetBoolField(TEXT("bool_value"), ActualBool);

	// verified_value smoking gun — readback after write.
	if (bRequestedMin || bRequestedMax || bRequestedBool)
	{
		TSharedPtr<FJsonObject> Verified = MakeShared<FJsonObject>();

		if (bRequestedMin)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("requested"), RequestedMin);
			Entry->SetNumberField(TEXT("actual"), ActualMin);
			Entry->SetBoolField(TEXT("match"), FMath::IsNearlyEqual(RequestedMin, ActualMin));
			Verified->SetObjectField(TEXT("float_value_min"), Entry);
		}
		if (bRequestedMax)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("requested"), RequestedMax);
			Entry->SetNumberField(TEXT("actual"), ActualMax);
			Entry->SetBoolField(TEXT("match"), FMath::IsNearlyEqual(RequestedMax, ActualMax));
			Verified->SetObjectField(TEXT("float_value_max"), Entry);
		}
		if (bRequestedBool)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetBoolField(TEXT("requested"), RequestedBool);
			Entry->SetBoolField(TEXT("actual"), ActualBool);
			Entry->SetBoolField(TEXT("match"), RequestedBool == ActualBool);
			Verified->SetObjectField(TEXT("bool_value"), Entry);
		}

		Result->SetObjectField(TEXT("verified_value"), Verified);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  85. list_eqs_generator_types
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleListEQSGeneratorTypes(const TSharedPtr<FJsonObject>& Params)
{
	TArray<TSharedPtr<FJsonValue>> Items = EnumerateDerivedClasses(UEnvQueryGenerator::StaticClass());

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("generator_types"), Items);
	Result->SetNumberField(TEXT("count"), Items.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  86. list_eqs_test_types
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleListEQSTestTypes(const TSharedPtr<FJsonObject>& Params)
{
	TArray<TSharedPtr<FJsonValue>> Items = EnumerateDerivedClasses(UEnvQueryTest::StaticClass());

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("test_types"), Items);
	Result->SetNumberField(TEXT("count"), Items.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  87. list_eqs_contexts
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleListEQSContexts(const TSharedPtr<FJsonObject>& Params)
{
	TArray<TSharedPtr<FJsonValue>> Items = EnumerateDerivedClasses(UEnvQueryContext::StaticClass());

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("context_types"), Items);
	Result->SetNumberField(TEXT("count"), Items.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  90. validate_eqs_query
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleValidateEQSQuery(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UEnvQuery* Query = LoadEQSFromParams(Params, AssetPath, Error);
	if (!Query)
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> Errors;
	bool bValid = true;

	const TArray<UEnvQueryOption*>& Options = Query->GetOptions();

	if (Options.Num() == 0)
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("Query has no options (generators)")));
	}

	for (int32 OptIdx = 0; OptIdx < Options.Num(); ++OptIdx)
	{
		const UEnvQueryOption* Option = Options[OptIdx];
		FString OptionLabel = FString::Printf(TEXT("Option[%d]"), OptIdx);

		if (!Option)
		{
			Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: Null option"), *OptionLabel)));
			bValid = false;
			continue;
		}

		// Check generator
		if (!Option->Generator)
		{
			Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: No generator"), *OptionLabel)));
			bValid = false;
			continue;
		}

		FString GenName = Option->Generator->GetClass()->GetName();

		// Check item type
		if (!Option->Generator->ItemType)
		{
			Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s (%s): Generator has no ItemType set"), *OptionLabel, *GenName)));
		}

		// Check for empty tests (not necessarily an error, but worth flagging)
		if (Option->Tests.Num() == 0)
		{
			Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s (%s): No tests — items will be scored equally"), *OptionLabel, *GenName)));
		}

		// Validate each test
		for (int32 TestIdx = 0; TestIdx < Option->Tests.Num(); ++TestIdx)
		{
			const UEnvQueryTest* Test = Option->Tests[TestIdx];
			FString TestLabel = FString::Printf(TEXT("%s.Test[%d]"), *OptionLabel, TestIdx);

			if (!Test)
			{
				Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: Null test"), *TestLabel)));
				bValid = false;
				continue;
			}

			FString TestName = Test->GetClass()->GetName();

			// Check if test purpose makes sense with filter type
			if (Test->TestPurpose == EEnvTestPurpose::Filter || Test->TestPurpose == EEnvTestPurpose::FilterAndScore)
			{
				if (Test->FilterType == EEnvTestFilterType::Range)
				{
					if (Test->FloatValueMin.DefaultValue > Test->FloatValueMax.DefaultValue)
					{
						Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
							TEXT("%s (%s): Filter range min (%.2f) > max (%.2f)"),
							*TestLabel, *TestName,
							Test->FloatValueMin.DefaultValue,
							Test->FloatValueMax.DefaultValue)));
					}
				}
			}

			// Check if scoring factor is zero on a scoring test
			if (Test->TestPurpose == EEnvTestPurpose::Score || Test->TestPurpose == EEnvTestPurpose::FilterAndScore)
			{
				if (FMath::IsNearlyZero(Test->ScoringFactor.DefaultValue))
				{
					Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
						TEXT("%s (%s): Scoring factor is 0 — test will have no effect on score"),
						*TestLabel, *TestName)));
				}
			}
		}
	}

	// Cross-option item type consistency check
	UClass* FirstItemType = nullptr;
	for (int32 OptIdx = 0; OptIdx < Options.Num(); ++OptIdx)
	{
		if (!Options[OptIdx] || !Options[OptIdx]->Generator) continue;
		UClass* ItemType = Options[OptIdx]->Generator->ItemType;
		if (!ItemType) continue;

		if (!FirstItemType)
		{
			FirstItemType = ItemType;
		}
		else if (ItemType != FirstItemType)
		{
			Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("Item type mismatch: Option[0] uses %s, Option[%d] uses %s"),
				*FirstItemType->GetName(), OptIdx, *ItemType->GetName())));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetBoolField(TEXT("valid"), bValid);
	Result->SetNumberField(TEXT("option_count"), Options.Num());
	Result->SetArrayField(TEXT("errors"), Errors);
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetNumberField(TEXT("error_count"), Errors.Num());
	Result->SetNumberField(TEXT("warning_count"), Warnings.Num());

	if (bValid && Errors.Num() == 0 && Warnings.Num() == 0)
	{
		Result->SetStringField(TEXT("message"), TEXT("Query is valid with no issues"));
	}
	else if (bValid)
	{
		Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Query is valid but has %d warning(s)"), Warnings.Num()));
	}
	else
	{
		Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Query has %d error(s) and %d warning(s)"), Errors.Num(), Warnings.Num()));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  84. reorder_eqs_tests
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleReorderEQSTests(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UEnvQuery* Query = LoadEQSFromParams(Params, AssetPath, Error);
	if (!Query)
	{
		return FMonolithActionResult::Error(Error);
	}

	int32 OptionIndex = (int32)Params->GetNumberField(TEXT("option_index"));

	UEnvQueryOption* Option = nullptr;
	if (!GetOptionByIndex(Query, OptionIndex, Option, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	const TArray<TSharedPtr<FJsonValue>>* NewOrderArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("new_order"), NewOrderArr) || !NewOrderArr)
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: new_order (array of test indices)"));
	}

	int32 TestCount = Option->Tests.Num();
	TArray<int32> NewOrder;
	TSet<int32> Seen;
	for (const TSharedPtr<FJsonValue>& Val : *NewOrderArr)
	{
		int32 Idx = (int32)Val->AsNumber();
		if (Idx < 0 || Idx >= TestCount)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Test index %d out of range [0, %d)"), Idx, TestCount));
		}
		if (Seen.Contains(Idx))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Duplicate test index %d in new_order"), Idx));
		}
		Seen.Add(Idx);
		NewOrder.Add(Idx);
	}

	if (NewOrder.Num() != TestCount)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("new_order has %d elements but option has %d tests — must include all indices exactly once"), NewOrder.Num(), TestCount));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Reorder EQS Tests")));
	Query->Modify();

	TArray<TObjectPtr<UEnvQueryTest>> OldTests = Option->Tests;
	Option->Tests.Empty(TestCount);
	for (int32 Idx : NewOrder)
	{
		Option->Tests.Add(OldTests[Idx]);
	}

	Query->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("option_index"), OptionIndex);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Reordered %d tests"), TestCount));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  88. build_eqs_query_from_spec — helper
// ============================================================

namespace
{
	/** Apply scoring spec to a test. */
	void ApplyEQSScoringSpec(UEnvQueryTest* Test, const TSharedPtr<FJsonObject>& ScoringObj)
	{
		if (!Test || !ScoringObj.IsValid()) return;

		FString PurposeStr = ScoringObj->GetStringField(TEXT("purpose"));
		if (!PurposeStr.IsEmpty())
		{
			int64 Val = StaticEnum<EEnvTestPurpose::Type>()->GetValueByNameString(PurposeStr);
			if (Val != INDEX_NONE) Test->TestPurpose = (EEnvTestPurpose::Type)Val;
		}

		FString EquationStr = ScoringObj->GetStringField(TEXT("equation"));
		if (!EquationStr.IsEmpty())
		{
			int64 Val = StaticEnum<EEnvTestScoreEquation::Type>()->GetValueByNameString(EquationStr);
			if (Val != INDEX_NONE) Test->ScoringEquation = (EEnvTestScoreEquation::Type)Val;
		}

		if (ScoringObj->HasField(TEXT("factor")))
		{
			Test->ScoringFactor.DefaultValue = (float)ScoringObj->GetNumberField(TEXT("factor"));
		}
	}

	/** Apply filter spec to a test. */
	void ApplyEQSFilterSpec(UEnvQueryTest* Test, const TSharedPtr<FJsonObject>& FilterObj)
	{
		if (!Test || !FilterObj.IsValid()) return;

		FString FilterTypeStr = FilterObj->GetStringField(TEXT("filter_type"));
		if (!FilterTypeStr.IsEmpty())
		{
			int64 Val = StaticEnum<EEnvTestFilterType::Type>()->GetValueByNameString(FilterTypeStr);
			if (Val != INDEX_NONE) Test->FilterType = (EEnvTestFilterType::Type)Val;
		}

		if (FilterObj->HasField(TEXT("min")))
		{
			Test->FloatValueMin.DefaultValue = (float)FilterObj->GetNumberField(TEXT("min"));
		}
		if (FilterObj->HasField(TEXT("max")))
		{
			Test->FloatValueMax.DefaultValue = (float)FilterObj->GetNumberField(TEXT("max"));
		}
		if (FilterObj->HasField(TEXT("bool_value")))
		{
			Test->BoolValue.DefaultValue = FilterObj->GetBoolField(TEXT("bool_value"));
		}
	}
}

// ============================================================
//  88. build_eqs_query_from_spec
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleBuildEQSQueryFromSpec(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult))
	{
		return ErrResult;
	}

	const TSharedPtr<FJsonObject>* SpecObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("spec"), SpecObj) || !SpecObj->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: spec"));
	}

	const TArray<TSharedPtr<FJsonValue>>* OptionsArr = nullptr;
	if (!(*SpecObj)->TryGetArrayField(TEXT("options"), OptionsArr) || !OptionsArr || OptionsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Spec must contain 'options' array with at least one option"));
	}

	// Create the asset
	FString AssetName = FPackageName::GetShortName(SavePath);
	FString PathError;
	if (!MonolithAI::EnsureAssetPathFree(SavePath, AssetName, PathError))
	{
		return FMonolithActionResult::Error(PathError);
	}

	FString PkgError;
	UPackage* Package = MonolithAI::GetOrCreatePackage(SavePath, PkgError);
	if (!Package)
	{
		return FMonolithActionResult::Error(PkgError);
	}

	UEnvQuery* Query = NewObject<UEnvQuery>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!Query)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UEnvQuery object"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Build EQS From Spec")));

	TArray<FString> Warnings;

	for (int32 OptIdx = 0; OptIdx < OptionsArr->Num(); ++OptIdx)
	{
		TSharedPtr<FJsonObject> OptionObjRef = (*OptionsArr)[OptIdx]->AsObject();
		const TSharedPtr<FJsonObject>* OptionObj = &OptionObjRef;
		if (!OptionObjRef.IsValid() || OptionObjRef->Values.Num() == 0)
		{
			Warnings.Add(FString::Printf(TEXT("Option[%d]: Invalid JSON object, skipped"), OptIdx));
			continue;
		}

		// Parse generator
		const TSharedPtr<FJsonObject>* GenObj = nullptr;
		if (!(*OptionObj)->TryGetObjectField(TEXT("generator"), GenObj) || !GenObj->IsValid())
		{
			Warnings.Add(FString::Printf(TEXT("Option[%d]: Missing 'generator' field, skipped"), OptIdx));
			continue;
		}

		FString GenClassName = (*GenObj)->GetStringField(TEXT("class"));
		if (GenClassName.IsEmpty())
		{
			Warnings.Add(FString::Printf(TEXT("Option[%d]: Generator missing 'class', skipped"), OptIdx));
			continue;
		}

		UClass* GenClass = FindEQSClassByName(UEnvQueryGenerator::StaticClass(), GenClassName);
		if (!GenClass)
		{
			Warnings.Add(FString::Printf(TEXT("Option[%d]: Generator class '%s' not found, skipped"), OptIdx, *GenClassName));
			continue;
		}

		UEnvQueryOption* NewOption = NewObject<UEnvQueryOption>(Query);
		UEnvQueryGenerator* NewGen = NewObject<UEnvQueryGenerator>(NewOption, GenClass);
		NewOption->Generator = NewGen;

		// Apply generator properties
		const TSharedPtr<FJsonObject>* GenPropsObj = nullptr;
		if ((*GenObj)->TryGetObjectField(TEXT("properties"), GenPropsObj) && GenPropsObj->IsValid())
		{
			TArray<FString> PropErrors;
			ApplyEQSProperties(NewGen, *GenPropsObj, PropErrors);
			for (const FString& Err : PropErrors)
			{
				Warnings.Add(FString::Printf(TEXT("Option[%d].generator: %s"), OptIdx, *Err));
			}
		}

		// Parse tests
		const TArray<TSharedPtr<FJsonValue>>* TestsArr = nullptr;
		if ((*OptionObj)->TryGetArrayField(TEXT("tests"), TestsArr) && TestsArr)
		{
			for (int32 TestIdx = 0; TestIdx < TestsArr->Num(); ++TestIdx)
			{
				TSharedPtr<FJsonObject> TestObjRef = (*TestsArr)[TestIdx]->AsObject();
				const TSharedPtr<FJsonObject>* TestObj = &TestObjRef;
				if (!TestObjRef.IsValid() || TestObjRef->Values.Num() == 0)
				{
					Warnings.Add(FString::Printf(TEXT("Option[%d].Test[%d]: Invalid JSON, skipped"), OptIdx, TestIdx));
					continue;
				}

				FString TestClassName = (*TestObj)->GetStringField(TEXT("class"));
				if (TestClassName.IsEmpty())
				{
					Warnings.Add(FString::Printf(TEXT("Option[%d].Test[%d]: Missing 'class', skipped"), OptIdx, TestIdx));
					continue;
				}

				UClass* TestClass = FindEQSClassByName(UEnvQueryTest::StaticClass(), TestClassName);
				if (!TestClass)
				{
					Warnings.Add(FString::Printf(TEXT("Option[%d].Test[%d]: Test class '%s' not found, skipped"), OptIdx, TestIdx, *TestClassName));
					continue;
				}

				UEnvQueryTest* NewTest = NewObject<UEnvQueryTest>(NewOption, TestClass);
				NewOption->Tests.Add(NewTest);

				// Apply test properties
				const TSharedPtr<FJsonObject>* TestPropsObj = nullptr;
				if ((*TestObj)->TryGetObjectField(TEXT("properties"), TestPropsObj) && TestPropsObj->IsValid())
				{
					TArray<FString> PropErrors;
					ApplyEQSProperties(NewTest, *TestPropsObj, PropErrors);
					for (const FString& Err : PropErrors)
					{
						Warnings.Add(FString::Printf(TEXT("Option[%d].Test[%d]: %s"), OptIdx, TestIdx, *Err));
					}
				}

				// Apply scoring
				const TSharedPtr<FJsonObject>* ScoringObj = nullptr;
				if ((*TestObj)->TryGetObjectField(TEXT("scoring"), ScoringObj) && ScoringObj->IsValid())
				{
					ApplyEQSScoringSpec(NewTest, *ScoringObj);
				}

				// Apply filter
				const TSharedPtr<FJsonObject>* FilterObj = nullptr;
				if ((*TestObj)->TryGetObjectField(TEXT("filter"), FilterObj) && FilterObj->IsValid())
				{
					ApplyEQSFilterSpec(NewTest, *FilterObj);
				}
			}
		}

		GetOptionsMutable(Query).Add(NewOption);
	}

	FAssetRegistryModule::AssetCreated(Query);
	Query->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetStringField(TEXT("name"), AssetName);
	Result->SetNumberField(TEXT("option_count"), Query->GetOptions().Num());
	Result->SetStringField(TEXT("message"), TEXT("EQS query built from spec"));

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings)
		{
			WarnArr.Add(MakeShared<FJsonValueString>(W));
		}
		Result->SetArrayField(TEXT("warnings"), WarnArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  91. create_eqs_from_template
// ============================================================

FMonolithActionResult FMonolithAIEQSActions::HandleCreateEQSFromTemplate(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult))
	{
		return ErrResult;
	}

	FString TemplateName;
	if (!MonolithAI::RequireStringParam(Params, TEXT("template"), TemplateName, ErrResult))
	{
		return ErrResult;
	}
	TemplateName = TemplateName.ToLower();

	// Build the spec JSON programmatically based on template name
	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> SpecOptions;

	if (TemplateName == TEXT("find_cover"))
	{
		// SimpleGrid + Trace(filter: not visible to querier) + Distance(score, inverse linear)
		TSharedPtr<FJsonObject> Option = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> Gen = MakeShared<FJsonObject>();
		Gen->SetStringField(TEXT("class"), TEXT("EnvQueryGenerator_SimpleGrid"));
		TSharedPtr<FJsonObject> GenProps = MakeShared<FJsonObject>();
		GenProps->SetNumberField(TEXT("GridSize"), 1000.0);
		GenProps->SetNumberField(TEXT("SpaceBetween"), 200.0);
		Gen->SetObjectField(TEXT("properties"), GenProps);
		Option->SetObjectField(TEXT("generator"), Gen);

		TArray<TSharedPtr<FJsonValue>> Tests;

		// Test 1: Trace (filter — not visible to querier)
		TSharedPtr<FJsonObject> TraceTest = MakeShared<FJsonObject>();
		TraceTest->SetStringField(TEXT("class"), TEXT("EnvQueryTest_Trace"));
		TSharedPtr<FJsonObject> TraceScoringObj = MakeShared<FJsonObject>();
		TraceScoringObj->SetStringField(TEXT("purpose"), TEXT("Filter"));
		TraceTest->SetObjectField(TEXT("scoring"), TraceScoringObj);
		TSharedPtr<FJsonObject> TraceFilterObj = MakeShared<FJsonObject>();
		TraceFilterObj->SetStringField(TEXT("filter_type"), TEXT("Match"));
		TraceFilterObj->SetBoolField(TEXT("bool_value"), false);
		TraceTest->SetObjectField(TEXT("filter"), TraceFilterObj);
		Tests.Add(MakeShared<FJsonValueObject>(TraceTest));

		// Test 2: Distance (score, inverse linear)
		TSharedPtr<FJsonObject> DistTest = MakeShared<FJsonObject>();
		DistTest->SetStringField(TEXT("class"), TEXT("EnvQueryTest_Distance"));
		TSharedPtr<FJsonObject> DistScoring = MakeShared<FJsonObject>();
		DistScoring->SetStringField(TEXT("purpose"), TEXT("Score"));
		DistScoring->SetStringField(TEXT("equation"), TEXT("InverseLinear"));
		DistTest->SetObjectField(TEXT("scoring"), DistScoring);
		Tests.Add(MakeShared<FJsonValueObject>(DistTest));

		Option->SetArrayField(TEXT("tests"), Tests);
		SpecOptions.Add(MakeShared<FJsonValueObject>(Option));
	}
	else if (TemplateName == TEXT("find_flank"))
	{
		// OnCircle around target + Dot(filter: behind target) + Distance(score)
		TSharedPtr<FJsonObject> Option = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> Gen = MakeShared<FJsonObject>();
		Gen->SetStringField(TEXT("class"), TEXT("EnvQueryGenerator_OnCircle"));
		TSharedPtr<FJsonObject> GenProps = MakeShared<FJsonObject>();
		GenProps->SetNumberField(TEXT("CircleRadius"), 800.0);
		GenProps->SetNumberField(TEXT("SpaceBetween"), 100.0);
		Gen->SetObjectField(TEXT("properties"), GenProps);
		Option->SetObjectField(TEXT("generator"), Gen);

		TArray<TSharedPtr<FJsonValue>> Tests;

		// Test 1: Dot product (filter — behind target)
		TSharedPtr<FJsonObject> DotTest = MakeShared<FJsonObject>();
		DotTest->SetStringField(TEXT("class"), TEXT("EnvQueryTest_Dot"));
		TSharedPtr<FJsonObject> DotScoring = MakeShared<FJsonObject>();
		DotScoring->SetStringField(TEXT("purpose"), TEXT("Filter"));
		DotTest->SetObjectField(TEXT("scoring"), DotScoring);
		TSharedPtr<FJsonObject> DotFilter = MakeShared<FJsonObject>();
		DotFilter->SetStringField(TEXT("filter_type"), TEXT("Maximum"));
		DotFilter->SetNumberField(TEXT("max"), 0.0);
		DotTest->SetObjectField(TEXT("filter"), DotFilter);
		Tests.Add(MakeShared<FJsonValueObject>(DotTest));

		// Test 2: Distance (score)
		TSharedPtr<FJsonObject> DistTest = MakeShared<FJsonObject>();
		DistTest->SetStringField(TEXT("class"), TEXT("EnvQueryTest_Distance"));
		TSharedPtr<FJsonObject> DistScoring = MakeShared<FJsonObject>();
		DistScoring->SetStringField(TEXT("purpose"), TEXT("Score"));
		DistScoring->SetStringField(TEXT("equation"), TEXT("InverseLinear"));
		DistTest->SetObjectField(TEXT("scoring"), DistScoring);
		Tests.Add(MakeShared<FJsonValueObject>(DistTest));

		Option->SetArrayField(TEXT("tests"), Tests);
		SpecOptions.Add(MakeShared<FJsonValueObject>(Option));
	}
	else if (TemplateName == TEXT("find_patrol_point"))
	{
		// SimpleGrid + PathfindingBatch(filter: reachable) + Distance(score, inverse from querier)
		TSharedPtr<FJsonObject> Option = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> Gen = MakeShared<FJsonObject>();
		Gen->SetStringField(TEXT("class"), TEXT("EnvQueryGenerator_SimpleGrid"));
		TSharedPtr<FJsonObject> GenProps = MakeShared<FJsonObject>();
		GenProps->SetNumberField(TEXT("GridSize"), 2000.0);
		GenProps->SetNumberField(TEXT("SpaceBetween"), 400.0);
		Gen->SetObjectField(TEXT("properties"), GenProps);
		Option->SetObjectField(TEXT("generator"), Gen);

		TArray<TSharedPtr<FJsonValue>> Tests;

		// Test 1: Pathfinding (filter: reachable)
		TSharedPtr<FJsonObject> PathTest = MakeShared<FJsonObject>();
		PathTest->SetStringField(TEXT("class"), TEXT("EnvQueryTest_Pathfinding"));
		TSharedPtr<FJsonObject> PathScoring = MakeShared<FJsonObject>();
		PathScoring->SetStringField(TEXT("purpose"), TEXT("Filter"));
		PathTest->SetObjectField(TEXT("scoring"), PathScoring);
		TSharedPtr<FJsonObject> PathFilter = MakeShared<FJsonObject>();
		PathFilter->SetStringField(TEXT("filter_type"), TEXT("Match"));
		PathFilter->SetBoolField(TEXT("bool_value"), true);
		PathTest->SetObjectField(TEXT("filter"), PathFilter);
		Tests.Add(MakeShared<FJsonValueObject>(PathTest));

		// Test 2: Distance (score, inverse linear from querier)
		TSharedPtr<FJsonObject> DistTest = MakeShared<FJsonObject>();
		DistTest->SetStringField(TEXT("class"), TEXT("EnvQueryTest_Distance"));
		TSharedPtr<FJsonObject> DistScoring = MakeShared<FJsonObject>();
		DistScoring->SetStringField(TEXT("purpose"), TEXT("Score"));
		DistScoring->SetStringField(TEXT("equation"), TEXT("InverseLinear"));
		DistTest->SetObjectField(TEXT("scoring"), DistScoring);
		Tests.Add(MakeShared<FJsonValueObject>(DistTest));

		Option->SetArrayField(TEXT("tests"), Tests);
		SpecOptions.Add(MakeShared<FJsonValueObject>(Option));
	}
	else if (TemplateName == TEXT("find_nearest_item"))
	{
		// ActorsOfClass + Distance(score, inverse linear)
		TSharedPtr<FJsonObject> Option = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> Gen = MakeShared<FJsonObject>();
		Gen->SetStringField(TEXT("class"), TEXT("EnvQueryGenerator_ActorsOfClass"));
		Option->SetObjectField(TEXT("generator"), Gen);

		TArray<TSharedPtr<FJsonValue>> Tests;

		// Test 1: Distance (score, inverse linear)
		TSharedPtr<FJsonObject> DistTest = MakeShared<FJsonObject>();
		DistTest->SetStringField(TEXT("class"), TEXT("EnvQueryTest_Distance"));
		TSharedPtr<FJsonObject> DistScoring = MakeShared<FJsonObject>();
		DistScoring->SetStringField(TEXT("purpose"), TEXT("Score"));
		DistScoring->SetStringField(TEXT("equation"), TEXT("InverseLinear"));
		DistTest->SetObjectField(TEXT("scoring"), DistScoring);
		Tests.Add(MakeShared<FJsonValueObject>(DistTest));

		Option->SetArrayField(TEXT("tests"), Tests);
		SpecOptions.Add(MakeShared<FJsonValueObject>(Option));
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown template: '%s'. Available: find_cover, find_flank, find_patrol_point, find_nearest_item"), *TemplateName));
	}

	Spec->SetArrayField(TEXT("options"), SpecOptions);

	// Delegate to the spec builder by constructing params for it
	TSharedPtr<FJsonObject> SpecParams = MakeShared<FJsonObject>();
	SpecParams->SetStringField(TEXT("save_path"), SavePath);
	SpecParams->SetObjectField(TEXT("spec"), Spec);

	// Apply property overrides if provided — merge into generator properties
	const TSharedPtr<FJsonObject>* OverrideProps = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), OverrideProps) && OverrideProps->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* BuiltOptions = nullptr;
		if (Spec->TryGetArrayField(TEXT("options"), BuiltOptions) && BuiltOptions)
		{
			for (const TSharedPtr<FJsonValue>& OptVal : *BuiltOptions)
			{
				TSharedPtr<FJsonObject> OptObjRef = OptVal->AsObject();
				if (OptObjRef.IsValid() && OptObjRef->Values.Num() > 0)
				{
					const TSharedPtr<FJsonObject>* GenObjPtr = nullptr;
					if (OptObjRef->TryGetObjectField(TEXT("generator"), GenObjPtr) && GenObjPtr->IsValid())
					{
						const TSharedPtr<FJsonObject>* ExistingProps = nullptr;
						TSharedPtr<FJsonObject> MergedProps;
						if ((*GenObjPtr)->TryGetObjectField(TEXT("properties"), ExistingProps) && ExistingProps->IsValid())
						{
							MergedProps = MakeShared<FJsonObject>();
							for (const auto& Pair : (*ExistingProps)->Values)
							{
								MergedProps->SetField(Pair.Key, Pair.Value);
							}
							for (const auto& Pair : (*OverrideProps)->Values)
							{
								MergedProps->SetField(Pair.Key, Pair.Value);
							}
						}
						else
						{
							MergedProps = *OverrideProps;
						}
						(*GenObjPtr)->SetObjectField(TEXT("properties"), MergedProps);
					}
				}
			}
		}
	}

	FMonolithActionResult SpecResult = HandleBuildEQSQueryFromSpec(SpecParams);

	// Tag the result with the template name
	if (SpecResult.bSuccess && SpecResult.Result.IsValid())
	{
		SpecResult.Result->SetStringField(TEXT("template"), TemplateName);
		SpecResult.Result->SetStringField(TEXT("message"), FString::Printf(TEXT("EQS query created from template '%s'"), *TemplateName));
	}

	return SpecResult;
}
