#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Control Rig graph read/write action handlers for Monolith.
 * Graph reading, node spawning/removal, pin wiring/unwiring, pin defaults, member variables.
 */
class MONOLITHANIMATION_API FMonolithControlRigWriteActions
{
public:
	/** Register all Control Rig graph actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleGetControlRigGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddControlRigNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleConnectControlRigPins(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveControlRigNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleBreakControlRigLink(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetControlRigPinDefault(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddControlRigVariable(const TSharedPtr<FJsonObject>& Params);
};
