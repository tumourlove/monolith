#pragma once

#include "MonolithAIInternal.h"

class FMonolithAIBehaviorTreeActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// BT CRUD
	static FMonolithActionResult HandleCreateBehaviorTree(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetBehaviorTree(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListBehaviorTrees(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDeleteBehaviorTree(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDuplicateBehaviorTree(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetBTBlackboard(const TSharedPtr<FJsonObject>& Params);

	// Node discovery
	static FMonolithActionResult HandleListBTNodeClasses(const TSharedPtr<FJsonObject>& Params);

	// Node manipulation (Task 5)
	static FMonolithActionResult HandleAddBTNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveBTNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleMoveBTNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddBTDecorator(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveBTDecorator(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddBTService(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveBTService(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetBTNodeProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetBTNodeProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleReorderBTChildren(const TSharedPtr<FJsonObject>& Params);

	// Convenience tasks (Task 6)
	static FMonolithActionResult HandleAddBTRunEQSTask(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddBTSmartObjectTask(const TSharedPtr<FJsonObject>& Params);
	// Phase I2: BT-to-GAS direct ability activation
	static FMonolithActionResult HandleAddBTUseAbilityTask(const TSharedPtr<FJsonObject>& Params);

	// Spec-driven actions (Task 6)
	static FMonolithActionResult HandleBuildBTFromSpec(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleExportBTSpec(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleImportBTSpec(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleValidateBehaviorTree(const TSharedPtr<FJsonObject>& Params);

	// Phase F8 (J-phase): flat graph topology (parent_id + children GUIDs).
	// Differs from get_behavior_tree (recursive nested tree) — emits a flat
	// node array suitable for tools that need to look up a single node by GUID
	// without walking the full tree.
	static FMonolithActionResult HandleGetBTGraph(const TSharedPtr<FJsonObject>& Params);

	// Polish actions (Task 7)
	static FMonolithActionResult HandleCloneBTSubtree(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAutoArrangeBT(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCompareBehaviorTrees(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateBTTaskBlueprint(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateBTDecoratorBlueprint(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateBTServiceBlueprint(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGenerateBTDiagram(const TSharedPtr<FJsonObject>& Params);
};
