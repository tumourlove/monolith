#include "Indexers/UserDefinedStructIndexer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	// FProperty::GetCPPType() dereferences the property's inner type pointer
	// without a null guard for several subclasses, so it asserts when a
	// UserDefinedStruct field references a type that can no longer resolve
	// (e.g. a TSubclassOf<X> pointing at a deleted Blueprint leaves MetaClass
	// null). The known asserting paths (UE 5.7, verified in source):
	//   - FObjectProperty::GetCPPType         check(PropertyClass)  PropertyObject.cpp:42
	//   - FClassProperty::GetCPPType          check(MetaClass)      PropertyClass.cpp:160
	//     (also dereferences PropertyClass in its non-wrapper branches)
	//   - FStructProperty::GetCPPType         Struct->...           PropertyStruct.cpp:252
	//   - FEnumProperty::GetCPPType           check(Enum)           EnumProperty.cpp:390
	// FSoftObjectProperty / FSoftClassProperty share the FObjectPropertyBase
	// PropertyClass (and FSoftClassProperty its own MetaClass). FByteProperty
	// is already null-safe (it guards `if (Enum)` and falls back to Super), so
	// it needs no guard here.
	//
	// SafeGetCPPType returns GetCPPType() for every well-formed property and a
	// "<unresolved>" placeholder only when the inner type pointer that
	// GetCPPType would dereference is null -- so the deep indexer records the
	// broken field instead of taking the editor down (issue #70).
	FString SafeGetCPPType(const FProperty* Prop)
	{
		if (!Prop)
		{
			return TEXT("<unresolved>");
		}

		// Class / object family (FObjectProperty, FSoftObjectProperty,
		// FClassProperty, FSoftClassProperty all derive from
		// FObjectPropertyBase, which owns the public PropertyClass member).
		if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			if (ObjProp->PropertyClass == nullptr)
			{
				return TEXT("<unresolved>");
			}
			// FClassProperty / FSoftClassProperty additionally dereference
			// MetaClass -- this is the deleted-TSubclassOf failure vector.
			if (const FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
			{
				if (ClassProp->MetaClass == nullptr)
				{
					return TEXT("<unresolved>");
				}
			}
			else if (const FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop))
			{
				if (SoftClassProp->MetaClass == nullptr)
				{
					return TEXT("<unresolved>");
				}
			}
		}
		else if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			if (StructProp->Struct == nullptr)
			{
				return TEXT("<unresolved>");
			}
		}
		else if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			if (EnumProp->GetEnum() == nullptr)
			{
				return TEXT("<unresolved>");
			}
		}

		return Prop->GetCPPType();
	}
}

bool FUserDefinedStructIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	UUserDefinedStruct* UDS = Cast<UUserDefinedStruct>(LoadedAsset);
	if (!UDS) return false;

	const void* DefaultInstance = UDS->GetDefaultInstance();

	auto Props = MakeShared<FJsonObject>();
	int32 FieldCount = 0;

	TArray<TSharedPtr<FJsonValue>> Fields;
	for (TFieldIterator<FProperty> It(UDS); It; ++It)
	{
		FProperty* Prop = *It;
		FieldCount++;

		auto Field = MakeShared<FJsonObject>();
		Field->SetStringField(TEXT("name"), Prop->GetName());
		Field->SetStringField(TEXT("type"), SafeGetCPPType(Prop));
		Field->SetStringField(TEXT("category"), Prop->GetMetaData(TEXT("Category")));

		if (DefaultInstance)
		{
			FString DefaultStr;
			Prop->ExportTextItem_Direct(DefaultStr, Prop->ContainerPtrToValuePtr<void>(DefaultInstance), nullptr, nullptr, PPF_None);
			Field->SetStringField(TEXT("default_value"), DefaultStr);
		}

		Fields.Add(MakeShared<FJsonValueObject>(Field));

		FIndexedVariable Var;
		Var.AssetId = AssetId;
		Var.VarName = Prop->GetName();
		Var.VarType = SafeGetCPPType(Prop);
		Var.Category = Prop->GetMetaData(TEXT("Category"));
		if (DefaultInstance)
		{
			Prop->ExportTextItem_Direct(Var.DefaultValue, Prop->ContainerPtrToValuePtr<void>(DefaultInstance), nullptr, nullptr, PPF_None);
		}
		DB.InsertVariable(Var);
	}

	Props->SetNumberField(TEXT("field_count"), FieldCount);
	Props->SetArrayField(TEXT("fields"), Fields);

	FString PropsStr;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PropsStr);
	FJsonSerializer::Serialize(Props, *Writer, true);

	FIndexedNode StructNode;
	StructNode.AssetId = AssetId;
	StructNode.NodeName = UDS->GetName();
	StructNode.NodeClass = TEXT("UserDefinedStruct");
	StructNode.NodeType = TEXT("Struct");
	StructNode.Properties = PropsStr;
	DB.InsertNode(StructNode);

	return true;
}
