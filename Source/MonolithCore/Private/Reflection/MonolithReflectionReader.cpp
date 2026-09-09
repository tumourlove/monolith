// SPDX-License-Identifier: MIT

#include "Reflection/MonolithReflectionReader.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"
#include "StructUtils/InstancedStruct.h"

TSharedPtr<FJsonValue> FMonolithReflectionReader::PropertyToJsonValue(FProperty* Prop, const void* ValuePtr, const UObject* Owner)
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
				Obj->SetField(Inner->GetName(), PropertyToJsonValue(Inner, InnerPtr, Owner));
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
			StructObj->SetField(Inner->GetName(), PropertyToJsonValue(Inner, InnerPtr, Owner));
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
			Arr.Add(PropertyToJsonValue(ArrayProp->Inner, Helper.GetRawPtr(i), Owner));
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
				Arr.Add(PropertyToJsonValue(SetProp->ElementProp, Helper.GetElementPtr(i), Owner));
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
				MapObj->SetField(KeyStr, PropertyToJsonValue(MapProp->ValueProp, Helper.GetValuePtr(i), Owner));
			}
		}
		return MakeShared<FJsonValueObject>(MapObj);
	}

	// Fallback: ExportTextItem
	FString ExportedStr;
	Prop->ExportTextItem_Direct(ExportedStr, ValuePtr, nullptr, const_cast<UObject*>(Owner), PPF_None);
	return MakeShared<FJsonValueString>(ExportedStr);
}

TSharedPtr<FJsonObject> FMonolithReflectionReader::PropertiesToJsonObject(UObject* Object)
{
	if (!Object) return nullptr;

	auto PropsObj = MakeShared<FJsonObject>();
	UClass* ObjClass = Object->GetClass();

	for (TFieldIterator<FProperty> It(ObjClass, EFieldIteratorFlags::IncludeSuper, EFieldIteratorFlags::ExcludeDeprecated); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop) continue;
		if (Prop->GetOwnerClass() == UObject::StaticClass()) continue;
		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) continue;

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Object);
		PropsObj->SetField(Prop->GetName(), PropertyToJsonValue(Prop, ValuePtr, Object));
	}

	return PropsObj;
}
