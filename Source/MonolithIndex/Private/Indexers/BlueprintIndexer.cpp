#include "Indexers/BlueprintIndexer.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

bool FBlueprintIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset);
	if (!Blueprint) return false;

	// Index all graphs
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph)
		{
			IndexGraph(Graph, DB, AssetId);
		}
	}

	// Index variables
	IndexVariables(Blueprint, DB, AssetId);

	return true;
}

void FBlueprintIndexer::IndexGraph(UEdGraph* Graph, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!Graph) return;

	// Map from UEdGraphNode* to DB node ID for connection resolution
	TMap<UEdGraphNode*, int64> NodeIdMap;

	// Index all nodes
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;

		FIndexedNode IndexedNode;
		IndexedNode.AssetId = AssetId;
		IndexedNode.NodeName = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		IndexedNode.NodeClass = Node->GetClass()->GetName();
		IndexedNode.PosX = Node->NodePosX;
		IndexedNode.PosY = Node->NodePosY;

		// Determine node type
		if (Cast<UK2Node_Event>(Node))
		{
			IndexedNode.NodeType = TEXT("Event");
		}
		else if (UK2Node_CallFunction* FuncNode = Cast<UK2Node_CallFunction>(Node))
		{
			IndexedNode.NodeType = TEXT("FunctionCall");

			auto PropsObj = MakeShared<FJsonObject>();
			PropsObj->SetStringField(TEXT("function"),
				FuncNode->FunctionReference.GetMemberName().ToString());
			if (FuncNode->FunctionReference.GetMemberParentClass())
			{
				PropsObj->SetStringField(TEXT("target_class"),
					FuncNode->FunctionReference.GetMemberParentClass()->GetName());
			}
			FString PropsStr;
			auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PropsStr);
			FJsonSerializer::Serialize(PropsObj, *Writer, true);
			IndexedNode.Properties = PropsStr;
		}
		else if (Cast<UK2Node_VariableGet>(Node) || Cast<UK2Node_VariableSet>(Node))
		{
			IndexedNode.NodeType = TEXT("Variable");
		}
		else
		{
			IndexedNode.NodeType = TEXT("Other");
		}

		int64 NodeId = DB.InsertNode(IndexedNode);
		if (NodeId >= 0)
		{
			NodeIdMap.Add(Node, NodeId);
		}
	}

	// Index connections by walking output pins
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;

		int64* SourceNodeId = NodeIdMap.Find(Node);
		if (!SourceNodeId) continue;

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output) continue;

			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;

				int64* TargetNodeId = NodeIdMap.Find(LinkedPin->GetOwningNode());
				if (!TargetNodeId) continue;

				FIndexedConnection Conn;
				Conn.SourceNodeId = *SourceNodeId;
				Conn.SourcePin = Pin->PinName.ToString();
				Conn.TargetNodeId = *TargetNodeId;
				Conn.TargetPin = LinkedPin->PinName.ToString();

				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
				{
					Conn.PinType = TEXT("Exec");
				}
				else
				{
					Conn.PinType = Pin->PinType.PinCategory.ToString();
				}

				DB.InsertConnection(Conn);
			}
		}
	}
}

void FBlueprintIndexer::IndexVariables(UBlueprint* Blueprint, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!Blueprint) return;

	for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
	{
		FIndexedVariable Var;
		Var.AssetId = AssetId;
		Var.VarName = VarDesc.VarName.ToString();
		Var.VarType = VarDesc.VarType.PinCategory.ToString();
		Var.Category = VarDesc.Category.ToString();
		Var.DefaultValue = VarDesc.DefaultValue;
		if (Var.DefaultValue.IsEmpty() &&
			Blueprint->GeneratedClass &&
			Blueprint->GeneratedClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
		{
			UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject(false);
			if (CDO && CDO->HasAnyFlags(RF_ClassDefaultObject))
			{
				FProperty* Prop = Blueprint->GeneratedClass->FindPropertyByName(VarDesc.VarName);
				if (Prop)
				{
					const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
					Prop->ExportTextItem_Direct(Var.DefaultValue, ValuePtr, nullptr, CDO, PPF_None);
				}
			}
		}
		Var.bIsExposed = !!(VarDesc.PropertyFlags & CPF_ExposeOnSpawn);
		Var.bIsReplicated = !!(VarDesc.PropertyFlags & CPF_Net);

		DB.InsertVariable(Var);
	}
}
