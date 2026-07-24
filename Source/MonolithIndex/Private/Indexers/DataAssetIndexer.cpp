#include "Indexers/DataAssetIndexer.h"
#include "Reflection/MonolithReflectionReader.h"
#include "UObject/UnrealType.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

bool FDataAssetIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!LoadedAsset) return false;

	TSharedPtr<FJsonObject> Props = SerializeObjectProperties(LoadedAsset);
	if (!Props) return false;

	FString PropsStr;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PropsStr);
	FJsonSerializer::Serialize(Props.ToSharedRef(), *Writer, true);

	FIndexedNode Node;
	Node.AssetId = AssetId;
	Node.NodeType = TEXT("DataAsset");
	Node.NodeName = LoadedAsset->GetName();
	Node.NodeClass = LoadedAsset->GetClass()->GetName();
	Node.Properties = PropsStr;
	DB.InsertNode(Node);

	// Also index individual properties as variables for FTS search
	UClass* ObjClass = LoadedAsset->GetClass();
	for (TFieldIterator<FProperty> It(ObjClass, EFieldIteratorFlags::IncludeSuper, EFieldIteratorFlags::ExcludeDeprecated); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop) continue;
		if (Prop->GetOwnerClass() == UObject::StaticClass()) continue;
		if (Prop->GetOwnerClass() == UDataAsset::StaticClass()) continue;
		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) continue;

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(LoadedAsset);

		FString DefaultStr;
		Prop->ExportTextItem_Direct(DefaultStr, ValuePtr, nullptr, LoadedAsset, PPF_None);

		if (DefaultStr.IsEmpty() || DefaultStr == TEXT("None") || DefaultStr == TEXT("()")) continue;

		FIndexedVariable Var;
		Var.AssetId = AssetId;
		Var.VarName = Prop->GetName();
		Var.VarType = Prop->GetCPPType();
		Var.Category = Prop->GetMetaData(TEXT("Category"));
		Var.DefaultValue = DefaultStr;
		Var.bIsExposed = false;
		Var.bIsReplicated = !!(Prop->PropertyFlags & CPF_Net);

		DB.InsertVariable(Var);
	}

	return true;
}

TSharedPtr<FJsonObject> FDataAssetIndexer::SerializeObjectProperties(UObject* Object)
{
	if (!Object) return nullptr;

	auto Root = MakeShared<FJsonObject>();
	UClass* ObjClass = Object->GetClass();

	Root->SetStringField(TEXT("native_class"), ObjClass->GetName());

	auto PropsObj = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(ObjClass, EFieldIteratorFlags::IncludeSuper, EFieldIteratorFlags::ExcludeDeprecated); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop) continue;
		if (Prop->GetOwnerClass() == UObject::StaticClass()) continue;
		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) continue;

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Object);
		// Shared FProperty->JSON serializer (hoisted into MonolithCore). The
		// former FDataAssetIndexer::PropertyToJsonValue copy was subsumed by it.
		PropsObj->SetField(Prop->GetName(), FMonolithReflectionReader::PropertyToJsonValue(Prop, ValuePtr, Object));
	}

	Root->SetObjectField(TEXT("properties"), PropsObj);
	return Root;
}
