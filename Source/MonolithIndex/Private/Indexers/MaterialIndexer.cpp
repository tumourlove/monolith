#include "Indexers/MaterialIndexer.h"
#include "Engine/Texture.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialFunction.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

bool FMaterialIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (UMaterial* Material = Cast<UMaterial>(LoadedAsset))
	{
		IndexMaterialExpressions(Material, DB, AssetId);
		return true;
	}

	if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(LoadedAsset))
	{
		IndexMaterialInstance(MIC, DB, AssetId);
		return true;
	}

	if (UMaterialFunction* MatFunc = Cast<UMaterialFunction>(LoadedAsset))
	{
		// Material functions also have expressions via the base interface
		for (UMaterialExpression* Expr : MatFunc->GetExpressions())
		{
			if (!Expr) continue;

			FIndexedNode Node;
			Node.AssetId = AssetId;
			Node.NodeName = Expr->GetName();
			Node.NodeClass = Expr->GetClass()->GetName();
			Node.NodeType = TEXT("Expression");
			Node.PosX = Expr->MaterialExpressionEditorX;
			Node.PosY = Expr->MaterialExpressionEditorY;
			DB.InsertNode(Node);
		}
		return true;
	}

	return false;
}

void FMaterialIndexer::IndexMaterialExpressions(UMaterial* Material, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!Material) return;

	TMap<UMaterialExpression*, int64> ExpressionIdMap;

	for (UMaterialExpression* Expr : Material->GetExpressions())
	{
		if (!Expr) continue;

		FIndexedNode Node;
		Node.AssetId = AssetId;
		Node.NodeName = Expr->GetName();
		Node.NodeClass = Expr->GetClass()->GetName();
		Node.PosX = Expr->MaterialExpressionEditorX;
		Node.PosY = Expr->MaterialExpressionEditorY;

		if (UMaterialExpressionScalarParameter* ScalarParam = Cast<UMaterialExpressionScalarParameter>(Expr))
		{
			Node.NodeType = TEXT("ScalarParameter");

			FIndexedParameter Param;
			Param.AssetId = AssetId;
			Param.ParamName = ScalarParam->ParameterName.ToString();
			Param.ParamType = TEXT("Scalar");
			Param.ParamGroup = ScalarParam->Group.ToString();
			Param.DefaultValue = FString::SanitizeFloat(ScalarParam->DefaultValue);
			Param.Source = TEXT("Material");
			DB.InsertParameter(Param);

			auto Props = MakeShared<FJsonObject>();
			Props->SetStringField(TEXT("parameter_name"), ScalarParam->ParameterName.ToString());
			Props->SetNumberField(TEXT("default_value"), ScalarParam->DefaultValue);
			FString PropsStr;
			auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PropsStr);
			FJsonSerializer::Serialize(Props, *Writer, true);
			Node.Properties = PropsStr;
		}
		else if (UMaterialExpressionVectorParameter* VecParam = Cast<UMaterialExpressionVectorParameter>(Expr))
		{
			Node.NodeType = TEXT("VectorParameter");

			FIndexedParameter Param;
			Param.AssetId = AssetId;
			Param.ParamName = VecParam->ParameterName.ToString();
			Param.ParamType = TEXT("Vector");
			Param.ParamGroup = VecParam->Group.ToString();
			Param.DefaultValue = VecParam->DefaultValue.ToString();
			Param.Source = TEXT("Material");
			DB.InsertParameter(Param);
		}
		else if (UMaterialExpressionTextureObjectParameter* TexParam = Cast<UMaterialExpressionTextureObjectParameter>(Expr))
		{
			Node.NodeType = TEXT("TextureParameter");

			FIndexedParameter Param;
			Param.AssetId = AssetId;
			Param.ParamName = TexParam->ParameterName.ToString();
			Param.ParamType = TEXT("Texture");
			Param.ParamGroup = TexParam->Group.ToString();
			Param.DefaultValue = TexParam->Texture ? TexParam->Texture->GetPathName() : TEXT("");
			Param.Source = TEXT("Material");
			DB.InsertParameter(Param);
		}
		else if (UMaterialExpressionStaticBoolParameter* BoolParam = Cast<UMaterialExpressionStaticBoolParameter>(Expr))
		{
			Node.NodeType = TEXT("StaticBoolParameter");

			FIndexedParameter Param;
			Param.AssetId = AssetId;
			Param.ParamName = BoolParam->ParameterName.ToString();
			Param.ParamType = TEXT("StaticBool");
			Param.ParamGroup = BoolParam->Group.ToString();
			Param.DefaultValue = BoolParam->DefaultValue ? TEXT("true") : TEXT("false");
			Param.Source = TEXT("Material");
			DB.InsertParameter(Param);
		}
		else if (Cast<UMaterialExpressionFunctionInput>(Expr))
		{
			Node.NodeType = TEXT("FunctionInput");
		}
		else if (Cast<UMaterialExpressionFunctionOutput>(Expr))
		{
			Node.NodeType = TEXT("FunctionOutput");
		}
		else
		{
			Node.NodeType = TEXT("Expression");
		}

		int64 NodeId = DB.InsertNode(Node);
		if (NodeId >= 0)
		{
			ExpressionIdMap.Add(Expr, NodeId);
		}
	}

	// Index connections between expressions using FExpressionInputIterator
	for (UMaterialExpression* Expr : Material->GetExpressions())
	{
		if (!Expr) continue;

		int64* TargetNodeId = ExpressionIdMap.Find(Expr);
		if (!TargetNodeId) continue;

		int32 InputIdx = 0;
		for (FExpressionInputIterator It(Expr); It; ++It, ++InputIdx)
		{
			if (!It->Expression) continue;

			int64* SourceNodeId = ExpressionIdMap.Find(It->Expression);
			if (!SourceNodeId) continue;

			FIndexedConnection Conn;
			Conn.SourceNodeId = *SourceNodeId;
			Conn.SourcePin = FString::Printf(TEXT("Output_%d"), It->OutputIndex);
			Conn.TargetNodeId = *TargetNodeId;
			Conn.TargetPin = FString::Printf(TEXT("Input_%d"), InputIdx);
			Conn.PinType = TEXT("Material");

			DB.InsertConnection(Conn);
		}
	}
}

void FMaterialIndexer::IndexMaterialInstance(UMaterialInstanceConstant* MIC, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!MIC) return;

	for (const FScalarParameterValue& ScalarParam : MIC->ScalarParameterValues)
	{
		FIndexedParameter Param;
		Param.AssetId = AssetId;
		Param.ParamName = ScalarParam.ParameterInfo.Name.ToString();
		Param.ParamType = TEXT("Scalar");
		Param.DefaultValue = FString::SanitizeFloat(ScalarParam.ParameterValue);
		Param.Source = TEXT("MaterialInstance");
		DB.InsertParameter(Param);
	}

	for (const FVectorParameterValue& VecParam : MIC->VectorParameterValues)
	{
		FIndexedParameter Param;
		Param.AssetId = AssetId;
		Param.ParamName = VecParam.ParameterInfo.Name.ToString();
		Param.ParamType = TEXT("Vector");
		Param.DefaultValue = VecParam.ParameterValue.ToString();
		Param.Source = TEXT("MaterialInstance");
		DB.InsertParameter(Param);
	}

	for (const FTextureParameterValue& TexParam : MIC->TextureParameterValues)
	{
		FIndexedParameter Param;
		Param.AssetId = AssetId;
		Param.ParamName = TexParam.ParameterInfo.Name.ToString();
		Param.ParamType = TEXT("Texture");
		Param.DefaultValue = TexParam.ParameterValue ? TexParam.ParameterValue->GetPathName() : TEXT("");
		Param.Source = TEXT("MaterialInstance");
		DB.InsertParameter(Param);
	}
}
