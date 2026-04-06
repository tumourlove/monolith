#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class UMaterial;
class UMaterialExpression;

/**
 * Material domain action handlers for Monolith.
 * Ported from MaterialMCPReaderLibrary — 14 proven actions.
 */
class FMonolithMaterialActions
{
public:
	/** Register all material actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// --- Action handlers (each takes JSON params, returns FMonolithActionResult) ---
	static FMonolithActionResult GetAllExpressions(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetExpressionDetails(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetFullConnectionGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DisconnectExpression(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BuildMaterialGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BeginTransaction(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult EndTransaction(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ExportMaterialGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ImportMaterialGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateMaterial(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RenderPreview(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetThumbnail(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateCustomHLSLNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetLayerInfo(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 2: Asset creation & properties ---
	static FMonolithActionResult CreateMaterial(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateMaterialInstance(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetMaterialProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DeleteExpression(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 2B: Parameter management, recompile, duplicate ---
	static FMonolithActionResult GetMaterialParameters(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetInstanceParameter(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RecompileMaterial(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DuplicateMaterial(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 2C: Advanced utilities ---
	static FMonolithActionResult GetCompilationStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetExpressionProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ConnectExpressions(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 3: Free wins (graph utilities & inspection) ---
	static FMonolithActionResult AutoLayout(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DuplicateExpression(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListExpressionClasses(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetExpressionConnections(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult MoveExpression(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetMaterialProperties(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 4: Instance & property improvements ---
	static FMonolithActionResult GetInstanceParameters(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetInstanceParameters(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetInstanceParent(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ClearInstanceParameter(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SaveMaterial(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 5: Graph editing power ---
	static FMonolithActionResult UpdateCustomHlslNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ReplaceExpression(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetExpressionPinInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RenameExpression(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListMaterialInstances(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 6: Material Functions ---
	static FMonolithActionResult CreateMaterialFunction(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BuildFunctionGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetFunctionInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ExportFunctionGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetFunctionMetadata(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult UpdateMaterialFunction(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DeleteFunctionExpression(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 7: Batch & Advanced ---
	static FMonolithActionResult BatchSetMaterialProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BatchRecompile(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ImportTexture(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 8: Compound workflows ---
	static FMonolithActionResult CreatePbrMaterialFromDisk(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 9: Function instances ---
	static FMonolithActionResult CreateFunctionInstance(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetFunctionInstanceParameter(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetFunctionInstanceInfo(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 10: Function utilities ---
	static FMonolithActionResult LayoutFunctionExpressions(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RenameFunctionParameterGroup(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 11: Material expansion ---
	static FMonolithActionResult ClearGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DeleteExpressions(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetTextureProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult PreviewTexture(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult PreviewTextures(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CheckTilingQuality(const TSharedPtr<FJsonObject>& Params);

private:
	/** Load a UMaterial from an asset path. Returns nullptr on failure. */
	static UMaterial* LoadBaseMaterial(const FString& AssetPath);

	/** Serialize a single expression node to JSON. */
	static TSharedPtr<FJsonObject> SerializeExpression(const UMaterialExpression* Expression);

	/**
	 * Shared helper for building expression graphs in both Materials and MaterialFunctions.
	 * Handles node creation (standard + Custom HLSL), property setting, and connection wiring.
	 * Returns nodes_created, connections_made, id_to_name map via the ResultJson out param.
	 * The CreateExpressionFunc callback abstracts the difference between CreateMaterialExpression
	 * and CreateMaterialExpressionInFunction.
	 */
	using FCreateExpressionFunc = TFunction<UMaterialExpression*(UClass* ExprClass, int32 PosX, int32 PosY)>;
	static void BuildGraphFromSpec(
		const TSharedPtr<FJsonObject>& Spec,
		const FCreateExpressionFunc& CreateExpressionFunc,
		TMap<FString, UMaterialExpression*>& IdToExpr,
		int32& OutNodesCreated,
		int32& OutConnectionsMade,
		TArray<TSharedPtr<FJsonValue>>& OutErrors);
};
