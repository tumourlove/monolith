#pragma once
#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Standalone curve asset actions (blueprint namespace).
 *
 * - create_curve_asset : create a UCurveFloat / UCurveVector / UCurveLinearColor asset, optionally seeded with keys.
 * - get_curve_asset_keys     : read keys from any of the three curve asset types, per channel.
 * - set_curve_asset_keys     : write keys into one or more channels of any of the three curve asset types.
 *
 * UCurveVector::FloatCurves[3] and UCurveLinearColor::FloatCurves[4] are plain
 * FRichCurve UPROPERTY arrays that script code cannot edit, so key writes go
 * through the FRichCurve API directly. Channel names: float -> "value",
 * vector -> "x"|"y"|"z", linear color -> "r"|"g"|"b"|"a". Game-thread only.
 */
class FMonolithBlueprintCurveAssetActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult HandleCreateCurveAsset(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetCurveKeys(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetCurveKeys(const TSharedPtr<FJsonObject>& Params);
};
