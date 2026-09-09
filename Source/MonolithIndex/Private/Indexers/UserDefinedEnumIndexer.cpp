#include "Indexers/UserDefinedEnumIndexer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Engine/UserDefinedEnum.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

bool FUserDefinedEnumIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	UUserDefinedEnum* UDE = Cast<UUserDefinedEnum>(LoadedAsset);
	if (!UDE) return false;

	auto Props = MakeShared<FJsonObject>();
	int32 NumEnums = UDE->NumEnums();
	int32 EntryCount = FMath::Max(0, NumEnums - 1);
	Props->SetNumberField(TEXT("entry_count"), EntryCount);

	TArray<TSharedPtr<FJsonValue>> Entries;
	for (int32 i = 0; i < EntryCount; ++i)
	{
		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), UDE->GetNameByIndex(i).ToString());
		Entry->SetStringField(TEXT("display_name"), UDE->GetDisplayNameTextByIndex(i).ToString());
		Entry->SetNumberField(TEXT("value"), UDE->GetValueByIndex(i));
		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Props->SetArrayField(TEXT("entries"), Entries);

	FString PropsStr;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PropsStr);
	FJsonSerializer::Serialize(Props, *Writer, true);

	FIndexedNode EnumNode;
	EnumNode.AssetId = AssetId;
	EnumNode.NodeName = UDE->GetName();
	EnumNode.NodeClass = TEXT("UserDefinedEnum");
	EnumNode.NodeType = TEXT("Enum");
	EnumNode.Properties = PropsStr;
	DB.InsertNode(EnumNode);

	for (int32 i = 0; i < EntryCount; ++i)
	{
		FIndexedVariable Var;
		Var.AssetId = AssetId;
		Var.VarName = UDE->GetNameByIndex(i).ToString();
		Var.VarType = TEXT("EnumEntry");
		Var.Category = UDE->GetName();
		Var.DefaultValue = FString::Printf(TEXT("%lld"), UDE->GetValueByIndex(i));
		DB.InsertVariable(Var);
	}

	return true;
}
