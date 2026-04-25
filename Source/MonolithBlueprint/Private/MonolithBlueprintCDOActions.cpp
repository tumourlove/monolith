#include "MonolithBlueprintCDOActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "UObject/UnrealType.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "JsonObjectConverter.h"
#include "StructUtils/InstancedStruct.h"
#include "ScopedTransaction.h"
#include "MonolithBlueprintEditCradle.h"

// --- Registration ---

void FMonolithBlueprintCDOActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_cdo_properties"),
		TEXT("Read all CDO (Class Default Object) properties from a Blueprint or any UObject asset. "
			 "Essential for GameplayEffects (Duration, Modifiers, Tags, Stacking), AbilitySets, InputActions, "
			 "and any asset whose config is stored as UPROPERTY defaults rather than Blueprint graph nodes."),
		FMonolithActionHandler::CreateStatic(&HandleGetCDOProperties),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path (e.g. /Game/Blueprints/BP_MyActor or /Game/Data/DA_MyData)"))
			.Optional(TEXT("category_filter"), TEXT("string"), TEXT("Only include properties whose category contains this string"))
			.Optional(TEXT("include_parent_defaults"), TEXT("boolean"), TEXT("If true, include properties inherited from native parent class (default: true)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_cdo_property"),
		TEXT("Set a property value on a Blueprint CDO or UObject asset (DataAsset, GameplayEffect, etc.). "
			 "Write counterpart to get_cdo_properties. Supports numeric, boolean, string, enum, struct, "
			 "and any type that supports ImportText."),
		FMonolithActionHandler::CreateStatic(&HandleSetCDOProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint or UObject asset path (e.g. /Game/Data/DA_MyData)"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Property name to set (case-insensitive fallback)"))
			.Required(TEXT("value"), TEXT("any"), TEXT("New value — string, number, bool, or ImportText format for structs (e.g. \"(X=1.0,Y=2.0,Z=3.0)\")"))
			.Build());
}

// --- Property serialization helpers ---

namespace MonolithCDOInternal
{
	TSharedPtr<FJsonValue> PropertyToJsonValue(FProperty* Prop, const void* ValuePtr, const UObject* CDO)
	{
		if (!Prop || !ValuePtr) return MakeShared<FJsonValueNull>();

		// Numeric types
		if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
		{
			if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
			{
				if (ByteProp->Enum)
				{
					uint8 Val = ByteProp->GetPropertyValue(ValuePtr);
					return MakeShared<FJsonValueString>(ByteProp->Enum->GetNameStringByValue(Val));
				}
			}
			if (NumProp->IsInteger())
				return MakeShared<FJsonValueNumber>(static_cast<double>(NumProp->GetSignedIntPropertyValue(ValuePtr)));
			if (NumProp->IsFloatingPoint())
				return MakeShared<FJsonValueNumber>(NumProp->GetFloatingPointPropertyValue(ValuePtr));
		}

		// Bool
		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
			return MakeShared<FJsonValueBoolean>(BoolProp->GetPropertyValue(ValuePtr));

		// Enum
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			FString Val;
			EnumProp->ExportTextItem_Direct(Val, ValuePtr, nullptr, nullptr, PPF_None);
			return MakeShared<FJsonValueString>(Val);
		}

		// String types
		if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
			return MakeShared<FJsonValueString>(StrProp->GetPropertyValue(ValuePtr));
		if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
			return MakeShared<FJsonValueString>(NameProp->GetPropertyValue(ValuePtr).ToString());
		if (const FTextProperty* TextProp = CastField<FTextProperty>(Prop))
			return MakeShared<FJsonValueString>(TextProp->GetPropertyValue(ValuePtr).ToString());

		// Soft/class/object references
		if (const FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
		{
			const FSoftObjectPtr& Ref = *static_cast<const FSoftObjectPtr*>(ValuePtr);
			return MakeShared<FJsonValueString>(Ref.ToSoftObjectPath().ToString());
		}
		if (const FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop))
		{
			const FSoftObjectPtr& Ref = *static_cast<const FSoftObjectPtr*>(ValuePtr);
			return MakeShared<FJsonValueString>(Ref.ToSoftObjectPath().ToString());
		}
		if (const FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
		{
			UClass* ClassVal = Cast<UClass>(ClassProp->GetObjectPropertyValue(ValuePtr));
			return MakeShared<FJsonValueString>(ClassVal ? ClassVal->GetPathName() : TEXT("None"));
		}
		if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			UObject* ObjVal = ObjProp->GetObjectPropertyValue(ValuePtr);
			return MakeShared<FJsonValueString>(ObjVal ? ObjVal->GetPathName() : TEXT("None"));
		}

		// TInstancedStruct — unwrap and emit the concrete struct with a __struct tag
		if (const FStructProperty* InstancedStructProp = CastField<FStructProperty>(Prop);
			InstancedStructProp && InstancedStructProp->Struct == TBaseStructure<FInstancedStruct>::Get())
		{
			const FInstancedStruct& Instanced = *static_cast<const FInstancedStruct*>(ValuePtr);
			const UScriptStruct* InnerStruct = Instanced.GetScriptStruct();
			const uint8* InnerMem = Instanced.GetMemory();
			auto Obj = MakeShared<FJsonObject>();
			if (InnerStruct && InnerMem)
			{
				Obj->SetStringField(TEXT("__struct"), InnerStruct->GetPathName());
				for (TFieldIterator<FProperty> It(InnerStruct); It; ++It)
				{
					FProperty* Inner = *It;
					const void* InnerPtr = Inner->ContainerPtrToValuePtr<void>(InnerMem);
					Obj->SetField(Inner->GetName(), PropertyToJsonValue(Inner, InnerPtr, CDO));
				}
			}
			return MakeShared<FJsonValueObject>(Obj);
		}

		// Struct — recurse
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			auto StructObj = MakeShared<FJsonObject>();
			for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
			{
				FProperty* Inner = *It;
				const void* InnerPtr = Inner->ContainerPtrToValuePtr<void>(ValuePtr);
				StructObj->SetField(Inner->GetName(), PropertyToJsonValue(Inner, InnerPtr, CDO));
			}
			return MakeShared<FJsonValueObject>(StructObj);
		}

		// Array — recurse
		if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			FScriptArrayHelper Helper(ArrayProp, ValuePtr);
			for (int32 i = 0; i < Helper.Num(); ++i)
			{
				Arr.Add(PropertyToJsonValue(ArrayProp->Inner, Helper.GetRawPtr(i), CDO));
			}
			return MakeShared<FJsonValueArray>(Arr);
		}

		// Set
		if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			FScriptSetHelper Helper(SetProp, ValuePtr);
			for (int32 i = 0; i < Helper.Num(); ++i)
			{
				if (Helper.IsValidIndex(i))
					Arr.Add(PropertyToJsonValue(SetProp->ElementProp, Helper.GetElementPtr(i), CDO));
			}
			return MakeShared<FJsonValueArray>(Arr);
		}

		// Map
		if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
		{
			auto MapObj = MakeShared<FJsonObject>();
			FScriptMapHelper Helper(MapProp, ValuePtr);
			for (int32 i = 0; i < Helper.Num(); ++i)
			{
				if (Helper.IsValidIndex(i))
				{
					FString KeyStr;
					MapProp->KeyProp->ExportTextItem_Direct(KeyStr, Helper.GetKeyPtr(i), nullptr, nullptr, PPF_None);
					MapObj->SetField(KeyStr, PropertyToJsonValue(MapProp->ValueProp, Helper.GetValuePtr(i), CDO));
				}
			}
			return MakeShared<FJsonValueObject>(MapObj);
		}

		// Fallback: ExportTextItem
		FString ExportedStr;
		Prop->ExportTextItem_Direct(ExportedStr, ValuePtr, nullptr, const_cast<UObject*>(CDO), PPF_None);
		return MakeShared<FJsonValueString>(ExportedStr);
	}
}

// --- Handler ---

FMonolithActionResult FMonolithBlueprintCDOActions::HandleGetCDOProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	// Try Blueprint first (has GeneratedClass -> CDO), then fall back to any UObject
	UObject* TargetObject = nullptr;
	UClass* TargetClass = nullptr;

	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (BP && BP->GeneratedClass)
	{
		TargetClass = BP->GeneratedClass;
		TargetObject = TargetClass->GetDefaultObject(false);
	}
	else
	{
		// Not a Blueprint — try as a generic UObject (DataAsset, GameplayEffect, etc.)
		TargetObject = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
		if (TargetObject)
		{
			TargetClass = TargetObject->GetClass();
		}
	}

	if (!TargetObject || !TargetClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found or has no class: %s"), *AssetPath));
	}

	// Find the native parent class
	UClass* NativeParent = TargetClass;
	while (NativeParent && !NativeParent->HasAnyClassFlags(CLASS_Native))
	{
		NativeParent = NativeParent->GetSuperClass();
	}

	FString CategoryFilter;
	if (Params->HasField(TEXT("category_filter")))
	{
		CategoryFilter = Params->GetStringField(TEXT("category_filter"));
	}

	bool bIncludeParentDefaults = true;
	if (Params->HasField(TEXT("include_parent_defaults")))
	{
		bIncludeParentDefaults = Params->GetBoolField(TEXT("include_parent_defaults"));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("native_class"), NativeParent ? NativeParent->GetName() : TEXT("Unknown"));
	Root->SetStringField(TEXT("parent_class"), TargetClass->GetSuperClass() ? TargetClass->GetSuperClass()->GetName() : TEXT("None"));

	TArray<TSharedPtr<FJsonValue>> PropsArr;

	for (TFieldIterator<FProperty> It(TargetClass, EFieldIteratorFlags::IncludeSuper, EFieldIteratorFlags::ExcludeDeprecated); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop) continue;

		UClass* OwnerClass = Prop->GetOwnerClass();
		if (OwnerClass == UObject::StaticClass()) continue;

		if (!bIncludeParentDefaults && OwnerClass != TargetClass) continue;

		FString Category = Prop->GetMetaData(TEXT("Category"));
		if (!CategoryFilter.IsEmpty() && !Category.Contains(CategoryFilter)) continue;

		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) continue;

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(TargetObject);

		auto PropObj = MakeShared<FJsonObject>();
		PropObj->SetStringField(TEXT("name"), Prop->GetName());
		PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());
		PropObj->SetStringField(TEXT("category"), Category);
		PropObj->SetStringField(TEXT("owner_class"), OwnerClass->GetName());
		PropObj->SetField(TEXT("value"), MonolithCDOInternal::PropertyToJsonValue(Prop, ValuePtr, TargetObject));

		if (Prop->HasAnyPropertyFlags(CPF_Net))
			PropObj->SetBoolField(TEXT("replicated"), true);
		if (Prop->HasAnyPropertyFlags(CPF_EditConst))
			PropObj->SetBoolField(TEXT("edit_const"), true);

		PropsArr.Add(MakeShared<FJsonValueObject>(PropObj));
	}

	Root->SetArrayField(TEXT("properties"), PropsArr);
	Root->SetNumberField(TEXT("property_count"), PropsArr.Num());

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// set_cdo_property
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithBlueprintCDOActions::HandleSetCDOProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	FString PropertyName = Params->GetStringField(TEXT("property_name"));
	if (PropertyName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: property_name"));
	}

	if (!Params->HasField(TEXT("value")))
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: value"));
	}

	// --- Load asset: Blueprint CDO or generic UObject (same dual-path as get_cdo_properties) ---
	UObject* TargetObject = nullptr;
	UClass* TargetClass = nullptr;
	UBlueprint* BP = nullptr;

	BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (BP && BP->GeneratedClass)
	{
		TargetClass = BP->GeneratedClass;
		TargetObject = TargetClass->GetDefaultObject(false);
	}
	else
	{
		BP = nullptr; // Not a Blueprint
		TargetObject = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
		if (TargetObject)
		{
			TargetClass = TargetObject->GetClass();
		}
	}

	if (!TargetObject || !TargetClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found or has no class: %s"), *AssetPath));
	}

	// --- Find property (exact match, then case-insensitive fallback) ---
	FProperty* Prop = TargetClass->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		for (TFieldIterator<FProperty> It(TargetClass); It; ++It)
		{
			if (It->GetName().Equals(PropertyName, ESearchCase::IgnoreCase))
			{
				Prop = *It;
				break;
			}
		}
	}
	if (!Prop)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Property '%s' not found on %s"), *PropertyName, *TargetClass->GetName()));
	}

	// --- Read old value ---
	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(TargetObject);

	FString OldValue;
	Prop->ExportText_Direct(OldValue, ValuePtr, ValuePtr, TargetObject, PPF_None);

	// --- Engine edit cradle (matches Details panel write path) ---
	// Without this, cross-package TObjectPtr refs survive in memory but get silently
	// dropped by FLinkerSave's harvest walk on the next save (#29). The Details panel's
	// IPropertyHandle::SetValueFromFormattedString wraps the write in exactly this cradle:
	// transaction → Modify → PreEditChange(chain) → write → PostEditChangeChainProperty.
	// Bug-investigator H1 hypothesis: the engine's edit-side bookkeeping (transaction
	// buffer, OnObjectModified delegates, FOverridableManager overridden-property map)
	// must register the change for the harvester to register the import.
	TargetObject->SetFlags(RF_Transactional);
	FScopedTransaction Transaction(NSLOCTEXT("MonolithBlueprintCDOActions", "SetCDOProperty", "Monolith Set CDO Property"));
	TargetObject->Modify();

	FEditPropertyChain PropertyChain;
	PropertyChain.AddHead(Prop);
	PropertyChain.SetActivePropertyNode(Prop);
	TargetObject->PreEditChange(PropertyChain);

	// --- Set the value (JSON-aware for structs/arrays/maps, ImportText for scalars) ---
	const TSharedPtr<FJsonValue>& JsonVal = Params->TryGetField(TEXT("value"));

	if (JsonVal->Type == EJson::Object || JsonVal->Type == EJson::Array)
	{
		// Use FJsonObjectConverter for complex types (structs, TMaps, TArrays)
		if (!FJsonObjectConverter::JsonValueToUProperty(JsonVal, Prop, ValuePtr, 0, 0))
		{
			// Serialize the JSON value for error reporting (truncated)
			FString JsonStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
			FJsonSerializer::Serialize(JsonVal.ToSharedRef(), TEXT("value"), Writer);
			Writer->Close();
			if (JsonStr.Len() > 500) { JsonStr = JsonStr.Left(500) + TEXT("..."); }

			// Cancel the transaction so the failed-write doesn't pollute the undo buffer
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to set property '%s' from JSON — FJsonObjectConverter rejected the format. "
					 "Ensure the JSON structure matches the UProperty layout. Preview: %s"),
				*PropertyName, *JsonStr));
		}
	}
	else
	{
		// Scalar types: use ImportText
		FString ValStr;
		if (JsonVal->Type == EJson::Number)
		{
			ValStr = FString::SanitizeFloat(JsonVal->AsNumber());
		}
		else if (JsonVal->Type == EJson::Boolean)
		{
			ValStr = JsonVal->AsBool() ? TEXT("true") : TEXT("false");
		}
		else
		{
			ValStr = JsonVal->AsString();
		}

		const TCHAR* ImportResult = Prop->ImportText_Direct(*ValStr, ValuePtr, TargetObject, PPF_None);
		if (!ImportResult)
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to set property '%s' to value '%s' — ImportText rejected the format. "
					 "For structs use ImportText syntax e.g. \"(X=1.0,Y=2.0,Z=3.0)\", for enums use the display name."),
				*PropertyName, *ValStr));
		}
	}

	// FJsonObjectConverter outers new subobjects to /Engine/Transient when its container
	// isn't a UObject (JsonObjectConverter.cpp:964); FLinkerSave drops those refs at save.
	// Reparent before FireFullCradle so the cradle's Pre/Post fires on correct outers.
	MonolithEditCradle::ReparentTransientInstancedSubobjects(TargetObject, Prop);

	// Recursive cradle: fires PostEditChangeChainProperty for every nested
	// sub-property that contains an object reference, ensuring FOverridableManager
	// marks each inner TObjectPtr as overridden.
	MonolithEditCradle::FireFullCradle(TargetObject, Prop);

	// --- Read back new value for confirmation ---
	FString NewValue;
	Prop->ExportText_Direct(NewValue, ValuePtr, ValuePtr, TargetObject, PPF_None);

	// --- Mark dirty (PostEditChangeChainProperty handles property-change notifications;
	//     MarkBlueprintAsModified handles BP-asset-level recompile bookkeeping) ---
	if (BP)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	}
	else
	{
		TargetObject->MarkPackageDirty();
	}

	// --- Build response ---
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("property_name"), Prop->GetName());
	Root->SetStringField(TEXT("old_value"), OldValue);
	Root->SetStringField(TEXT("new_value"), NewValue);

	return FMonolithActionResult::Success(Root);
}
