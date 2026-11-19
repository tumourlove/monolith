#include "MonolithMaterialMeasureActions.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialFunction.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Comment.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "EdGraph/EdGraphPin.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "IMaterialEditor.h"
#include "Misc/App.h"

#include "Framework/Application/SlateApplication.h"
#include "Layout/ArrangedChildren.h"
#include "Layout/Geometry.h"
#include "Widgets/SWidget.h"

#include "GraphEditor.h"
#include "SGraphPanel.h"
#include "SGraphNode.h"
#include "SGraphPin.h"

// The measurement core mirrors MonolithBlueprintGraphMeasure
// (MonolithBlueprint/Private/MonolithBlueprintMeasureActions.cpp). It is kept
// file-local here rather than shared so neither module depends on the other.

namespace
{
	constexpr int32 MaterialMeasureMaxWalkDepth = 64;

	struct FMaterialMeasuredPin
	{
		const UEdGraphPin* Pin = nullptr;
		FVector2f Centre = FVector2f::ZeroVector;
		FVector2f Size = FVector2f::ZeroVector;
	};

	struct FMaterialMeasuredNode
	{
		FVector2f Size = FVector2f::ZeroVector;
		TArray<FMaterialMeasuredPin> Pins;
	};

	float MaterialMeasureRound(float Value)
	{
		return FMath::RoundToFloat(Value * 10.f) / 10.f;
	}

	bool MaterialMeasureCanMeasure(FString& OutError)
	{
		if (IsRunningCommandlet())
		{
			OutError = TEXT("Node measurement needs the editor UI; it is not available in a commandlet.");
			return false;
		}
		if (!GEditor)
		{
			OutError = TEXT("Node measurement needs the editor (GEditor is null).");
			return false;
		}
		if (!FApp::CanEverRender())
		{
			OutError = TEXT("Node measurement needs a rendering editor; it is not available with -nullrhi. Relaunch without -nullrhi (-unattended is fine).");
			return false;
		}
		if (!FSlateApplication::IsInitialized())
		{
			OutError = TEXT("Node measurement needs Slate, and the Slate application is not initialized.");
			return false;
		}
		return true;
	}

	void MaterialMeasureWalkPins(
		const SWidget& Widget,
		const FGeometry& Geometry,
		const TMap<const SWidget*, const UEdGraphPin*>& PinWidgets,
		TArray<FMaterialMeasuredPin>& OutPins,
		int32 Depth)
	{
		if (Depth > MaterialMeasureMaxWalkDepth || OutPins.Num() >= PinWidgets.Num())
		{
			return;
		}

		FArrangedChildren Arranged(EVisibility::Visible);
		Widget.ArrangeChildren(Geometry, Arranged, /*bUpdateAttributes=*/true);

		for (int32 Index = 0; Index < Arranged.Num(); ++Index)
		{
			const FArrangedWidget& Child = Arranged[Index];
			const SWidget& ChildWidget = Child.Widget.Get();

			if (const UEdGraphPin* const* Pin = PinWidgets.Find(&ChildWidget))
			{
				FMaterialMeasuredPin& Out = OutPins.AddDefaulted_GetRef();
				Out.Pin = *Pin;
				const FVector2f Centre = Child.Geometry.GetAbsolutePositionAtCoordinates(FVector2f(0.5f, 0.5f));
				const FVector2f Size = Child.Geometry.GetLocalSize();
				Out.Centre = Centre;
				Out.Size = Size;
				continue;
			}

			MaterialMeasureWalkPins(ChildWidget, Child.Geometry, PinWidgets, OutPins, Depth + 1);
		}
	}

	void MaterialMeasureOneNode(SGraphNode& NodeWidget, FMaterialMeasuredNode& OutNode)
	{
		NodeWidget.SlatePrepass(1.0f);
		const FVector2f Desired = NodeWidget.GetDesiredSize();
		OutNode.Size = Desired;

		TArray<TSharedRef<SWidget>> PinWidgetRefs;
		NodeWidget.GetPins(PinWidgetRefs);

		TMap<const SWidget*, const UEdGraphPin*> PinWidgets;
		PinWidgets.Reserve(PinWidgetRefs.Num());
		for (const TSharedRef<SWidget>& PinWidget : PinWidgetRefs)
		{
			const TSharedRef<SGraphPin> GraphPin = StaticCastSharedRef<SGraphPin>(PinWidget);
			if (const UEdGraphPin* Pin = GraphPin->GetPinObj())
			{
				PinWidgets.Add(&PinWidget.Get(), Pin);
			}
		}

		if (PinWidgets.Num() == 0 || Desired.X <= 0.f || Desired.Y <= 0.f)
		{
			return;
		}

		const FGeometry Root = FGeometry::MakeRoot(Desired, FSlateLayoutTransform());
		MaterialMeasureWalkPins(NodeWidget, Root, PinWidgets, OutNode.Pins, 0);
	}

	/** The id other material actions use for this node: the expression name where there is one. */
	FString MaterialMeasureNodeId(const UEdGraphNode* Node, FString& OutKind)
	{
		if (const UMaterialGraphNode* ExprNode = Cast<UMaterialGraphNode>(Node))
		{
			OutKind = TEXT("expression");
			return ExprNode->MaterialExpression ? ExprNode->MaterialExpression->GetName() : Node->GetName();
		}
		if (const UMaterialGraphNode_Comment* CommentNode = Cast<UMaterialGraphNode_Comment>(Node))
		{
			OutKind = TEXT("comment");
			return CommentNode->MaterialExpressionComment ? CommentNode->MaterialExpressionComment->GetName() : Node->GetName();
		}
		if (Cast<UMaterialGraphNode_Root>(Node))
		{
			OutKind = TEXT("root");
			return Node->GetName();
		}
		OutKind = TEXT("other");
		return Node->GetName();
	}
}

// ============================================================================
//  Registration
// ============================================================================

void FMonolithMaterialMeasureActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("material"), TEXT("measure_graph_nodes"),
		TEXT("Measure the real on-screen size of material graph nodes and the position of each pin. "
			"Opens the material (or material function) in the material editor, lays the nodes out at zoom 1:1 and returns "
			"per-node w/h plus each visible pin's row centre (x, y) relative to the node's top-left corner, in graph units. "
			"Node ids are expression names (as in get_all_expressions); the output node has kind 'root'. Needs the editor UI: "
			"fails cleanly under -nullrhi or in a commandlet. Read-only apart from opening the editor."),
		FMonolithActionHandler::CreateStatic(&HandleMeasureGraphNodes),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Material or material function asset path"))
			.Optional(TEXT("node_ids"), TEXT("array"), TEXT("Expression names to measure. Default: every node in the graph"))
			.Optional(TEXT("include_pins"), TEXT("boolean"), TEXT("Include per-pin centre offsets"), TEXT("true"))
			.Optional(TEXT("restore_view"), TEXT("boolean"), TEXT("Restore the graph editor's previous view offset and zoom after measuring"), TEXT("true"))
			.Build());
}

// ============================================================================
//  measure_graph_nodes
// ============================================================================

FMonolithActionResult FMonolithMaterialMeasureActions::HandleMeasureGraphNodes(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required."));
	}

	UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!Asset || !(Asset->IsA<UMaterial>() || Asset->IsA<UMaterialFunction>()))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("'%s' is not a material or material function. Material instances have no node graph."), *AssetPath));
	}

	bool bIncludePins = true;
	Params->TryGetBoolField(TEXT("include_pins"), bIncludePins);
	bool bRestoreView = true;
	Params->TryGetBoolField(TEXT("restore_view"), bRestoreView);

	TSet<FString> WantedIds;
	const TArray<TSharedPtr<FJsonValue>>* NodeIdsArr = nullptr;
	if (Params->TryGetArrayField(TEXT("node_ids"), NodeIdsArr) && NodeIdsArr)
	{
		for (const TSharedPtr<FJsonValue>& Value : *NodeIdsArr)
		{
			const FString Id = Value.IsValid() ? Value->AsString() : FString();
			if (!Id.IsEmpty()) WantedIds.Add(Id);
		}
	}

	FString Error;
	if (!MaterialMeasureCanMeasure(Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditors || !AssetEditors->OpenEditorForAsset(Asset))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Could not open the material editor for '%s'."), *AssetPath));
	}

	IAssetEditorInstance* EditorInstance = AssetEditors->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false);
	if (!EditorInstance || EditorInstance->GetEditorName() != FName(TEXT("MaterialEditor")))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("'%s' did not open in the material editor (editor: %s)."),
			*AssetPath, EditorInstance ? *EditorInstance->GetEditorName().ToString() : TEXT("none")));
	}

	// The material editor edits a transient copy; its graph is the one on screen.
	IMaterialEditor* MaterialEditor = static_cast<IMaterialEditor*>(EditorInstance);
	UMaterial* PreviewMaterial = Cast<UMaterial>(MaterialEditor->GetMaterialInterface());
	UEdGraph* Graph = PreviewMaterial ? PreviewMaterial->MaterialGraph.Get() : nullptr;
	if (!Graph)
	{
		return FMonolithActionResult::Error(TEXT("The material editor has no graph for this asset."));
	}

	TSharedPtr<SGraphEditor> GraphEditor = SGraphEditor::FindGraphEditorForGraph(Graph);
	SGraphPanel* Panel = GraphEditor.IsValid() ? GraphEditor->GetGraphPanel() : nullptr;
	if (!Panel)
	{
		return FMonolithActionResult::Error(TEXT("The material editor's graph widget was not found."));
	}

	FVector2f OldLocation = FVector2f::ZeroVector;
	float OldZoom = 1.f;
	GraphEditor->GetViewLocation(OldLocation, OldZoom);
	GraphEditor->SetViewLocation(OldLocation, 1.0f);
	Panel->Update();

	TArray<TSharedPtr<FJsonValue>> NodesArr;
	TArray<TSharedPtr<FJsonValue>> MissingArr;
	TSet<FString> FoundIds;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;

		FString Kind;
		const FString Id = MaterialMeasureNodeId(Node, Kind);
		if (WantedIds.Num() > 0 && !WantedIds.Contains(Id)) continue;
		FoundIds.Add(Id);

		TSharedPtr<SGraphNode> NodeWidget = Panel->GetNodeWidgetFromGuid(Node->NodeGuid);
		if (!NodeWidget.IsValid())
		{
			MissingArr.Add(MakeShared<FJsonValueString>(Id));
			continue;
		}

		FMaterialMeasuredNode M;
		MaterialMeasureOneNode(*NodeWidget, M);

		TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
		NObj->SetStringField(TEXT("id"), Id);
		NObj->SetStringField(TEXT("kind"), Kind);
		NObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());

		TArray<TSharedPtr<FJsonValue>> PosArr;
		PosArr.Add(MakeShared<FJsonValueNumber>(Node->NodePosX));
		PosArr.Add(MakeShared<FJsonValueNumber>(Node->NodePosY));
		NObj->SetArrayField(TEXT("pos"), PosArr);

		NObj->SetNumberField(TEXT("w"), MaterialMeasureRound(M.Size.X));
		NObj->SetNumberField(TEXT("h"), MaterialMeasureRound(M.Size.Y));

		if (bIncludePins)
		{
			TArray<TSharedPtr<FJsonValue>> PinsArr;
			for (const FMaterialMeasuredPin& P : M.Pins)
			{
				if (!P.Pin) continue;
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"), P.Pin->PinName.ToString());
				PObj->SetStringField(TEXT("direction"), P.Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
				PObj->SetNumberField(TEXT("x"), MaterialMeasureRound(P.Centre.X));
				PObj->SetNumberField(TEXT("y"), MaterialMeasureRound(P.Centre.Y));
				PObj->SetNumberField(TEXT("w"), MaterialMeasureRound(P.Size.X));
				PObj->SetNumberField(TEXT("h"), MaterialMeasureRound(P.Size.Y));
				PinsArr.Add(MakeShared<FJsonValueObject>(PObj));
			}
			NObj->SetArrayField(TEXT("pins"), PinsArr);
		}

		NodesArr.Add(MakeShared<FJsonValueObject>(NObj));
	}

	if (bRestoreView && OldZoom > 0.f)
	{
		GraphEditor->SetViewLocation(OldLocation, OldZoom);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("asset_type"), Asset->IsA<UMaterial>() ? TEXT("Material") : TEXT("MaterialFunction"));
	Result->SetNumberField(TEXT("zoom"), 1.0);
	Result->SetStringField(TEXT("units"), TEXT("graph units at zoom 1:1; pin x/y are row centres relative to the node's top-left"));
	Result->SetNumberField(TEXT("node_count"), NodesArr.Num());
	Result->SetArrayField(TEXT("nodes"), NodesArr);
	if (MissingArr.Num() > 0)
	{
		Result->SetArrayField(TEXT("unmeasured"), MissingArr);
	}

	if (WantedIds.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> UnknownArr;
		for (const FString& Id : WantedIds)
		{
			if (!FoundIds.Contains(Id)) UnknownArr.Add(MakeShared<FJsonValueString>(Id));
		}
		if (UnknownArr.Num() > 0)
		{
			Result->SetArrayField(TEXT("unknown_node_ids"), UnknownArr);
		}
	}

	return FMonolithActionResult::Success(Result);
}
