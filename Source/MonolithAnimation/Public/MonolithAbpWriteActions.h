#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * ABP graph node wiring actions for Monolith — Wave 7.
 * 3 core actions: add_anim_graph_node, connect_anim_graph_pins, set_state_animation.
 * Places animation nodes inside state graphs or the main AnimGraph and wires them.
 */
class MONOLITHANIMATION_API FMonolithAbpWriteActions
{
public:
	/** Register all ABP graph wiring actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleAddAnimGraphNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleConnectAnimGraphPins(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetStateAnimation(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddVariableGet(const TSharedPtr<FJsonObject>& Params);
};
