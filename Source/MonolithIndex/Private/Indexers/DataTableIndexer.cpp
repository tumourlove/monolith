#include "Indexers/DataTableIndexer.h"
#include "MonolithSettings.h"
#include "Engine/DataTable.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/UnrealType.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

bool FDataTableIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	// This indexer ignores individual asset params -- processes ALL DataTable assets at once
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetData> DataTableAssets;
	FARFilter Filter;
	for (const FName& ContentPath : UMonolithSettings::GetIndexedContentPaths())
	{
		Filter.PackagePaths.Add(ContentPath);
	}
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UDataTable::StaticClass()->GetClassPathName());
	Registry.GetAssets(Filter, DataTableAssets);

	int32 RowsInserted = 0;

	for (const FAssetData& DTAssetData : DataTableAssets)
	{
		int64 DTAssetId = DB.GetAssetId(DTAssetData.PackageName.ToString());
		if (DTAssetId < 0) continue;

		// Load the DataTable
		UDataTable* DataTable = Cast<UDataTable>(DTAssetData.GetAsset());
		if (!DataTable) continue;

		const UScriptStruct* RowStruct = DataTable->GetRowStruct();
		if (!RowStruct) continue;

		// Iterate all rows
		const TMap<FName, uint8*>& RowMap = DataTable->GetRowMap();
		for (const auto& Pair : RowMap)
		{
			FIndexedDataTableRow IndexedRow;
			IndexedRow.AssetId = DTAssetId;
			IndexedRow.RowName = Pair.Key.ToString();
			IndexedRow.RowData = RowStructToJson(RowStruct, Pair.Value);

			if (DB.InsertDataTableRow(IndexedRow) >= 0)
			{
				RowsInserted++;
			}
		}
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("DataTableIndexer: inserted %d rows from %d DataTable assets"),
		RowsInserted, DataTableAssets.Num());
	return true;
}

FString FDataTableIndexer::RowStructToJson(const UScriptStruct* RowStruct, const void* RowData)
{
	auto JsonObj = MakeShared<FJsonObject>();

	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop) continue;

		const FString PropName = Prop->GetName();
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);

		if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
		{
			if (NumProp->IsInteger())
			{
				JsonObj->SetNumberField(PropName, static_cast<double>(NumProp->GetSignedIntPropertyValue(ValuePtr)));
			}
			else if (NumProp->IsFloatingPoint())
			{
				JsonObj->SetNumberField(PropName, NumProp->GetFloatingPointPropertyValue(ValuePtr));
			}
		}
		else if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			JsonObj->SetBoolField(PropName, BoolProp->GetPropertyValue(ValuePtr));
		}
		else if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			JsonObj->SetStringField(PropName, StrProp->GetPropertyValue(ValuePtr));
		}
		else if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			JsonObj->SetStringField(PropName, NameProp->GetPropertyValue(ValuePtr).ToString());
		}
		else if (const FTextProperty* TextProp = CastField<FTextProperty>(Prop))
		{
			JsonObj->SetStringField(PropName, TextProp->GetPropertyValue(ValuePtr).ToString());
		}
		else if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			FString EnumValue;
			Prop->ExportTextItem_Direct(EnumValue, ValuePtr, nullptr, nullptr, PPF_None);
			JsonObj->SetStringField(PropName, EnumValue);
		}
		else if (const FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Prop))
		{
			const FSoftObjectPtr& SoftPtr = *static_cast<const FSoftObjectPtr*>(ValuePtr);
			JsonObj->SetStringField(PropName, SoftPtr.ToSoftObjectPath().ToString());
		}
		else
		{
			// Fallback: use ExportTextItem for structs, arrays, etc.
			FString ExportedValue;
			Prop->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, nullptr, PPF_None);
			JsonObj->SetStringField(PropName, ExportedValue);
		}
	}

	FString Result;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
	FJsonSerializer::Serialize(JsonObj, *Writer, true);
	return Result;
}
