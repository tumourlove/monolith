#include "Indexers/InputActionIndexer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "InputAction.h"
#include "InputTriggers.h"
#include "InputModifiers.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

bool FInputActionIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	UInputAction* IA = Cast<UInputAction>(LoadedAsset);
	if (!IA) return false;

	auto Props = MakeShared<FJsonObject>();

	Props->SetStringField(TEXT("value_type"), UEnum::GetValueAsString(IA->ValueType));

	if (!IA->ActionDescription.IsEmpty())
	{
		Props->SetStringField(TEXT("description"), IA->ActionDescription.ToString());
	}

	Props->SetBoolField(TEXT("consume_input"), IA->bConsumeInput);
	Props->SetBoolField(TEXT("trigger_when_paused"), IA->bTriggerWhenPaused);

	TArray<TSharedPtr<FJsonValue>> TriggerArray;
	for (const UInputTrigger* Trigger : IA->Triggers)
	{
		if (Trigger)
		{
			TriggerArray.Add(MakeShared<FJsonValueString>(Trigger->GetClass()->GetName()));
		}
	}
	Props->SetArrayField(TEXT("triggers"), TriggerArray);

	TArray<TSharedPtr<FJsonValue>> ModifierArray;
	for (const UInputModifier* Modifier : IA->Modifiers)
	{
		if (Modifier)
		{
			ModifierArray.Add(MakeShared<FJsonValueString>(Modifier->GetClass()->GetName()));
		}
	}
	Props->SetArrayField(TEXT("modifiers"), ModifierArray);

	FString PropsStr;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PropsStr);
	FJsonSerializer::Serialize(Props, *Writer, true);

	FIndexedNode ActionNode;
	ActionNode.AssetId = AssetId;
	ActionNode.NodeName = IA->GetName();
	ActionNode.NodeClass = TEXT("InputAction");
	ActionNode.NodeType = TEXT("InputAction");
	ActionNode.Properties = PropsStr;
	DB.InsertNode(ActionNode);

	return true;
}
