#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Read-only Enhanced Input asset discovery, bounded inspection and validation.
 *
 * The `input` namespace is registered independently of the optional GAS authoring
 * toggle: UInputAction and UInputMappingContext are engine Enhanced Input assets,
 * not GAS assets, so they stay reachable when GAS integration is disabled.
 *
 * These five handlers never transact, compile, save, mutate or dirty a package.
 * The authoring half of the namespace lives in FMonolithGASInputAuthoringActions.
 */
class MONOLITHGAS_API FMonolithGASInputAssetActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleListInputActions(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetInputAction(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListInputMappingContexts(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetInputMappingContext(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleValidateInputMappings(const TSharedPtr<FJsonObject>& Params);
};
