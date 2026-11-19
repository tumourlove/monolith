#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Bulk float-curve actions for UAnimSequence assets (animation namespace).
 * - copy_sequence_curves  : copy float curves (keys, flags, colour) from one sequence to many, via IAnimationDataController.
 * - audit_sequence_curves : report missing / extra / empty curves per sequence against a required set or a reference sequence.
 */
class MONOLITHANIMATION_API FMonolithAnimationCurveAuditActions
{
public:
	/** Register all curve copy / audit actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleCopySequenceCurves(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAuditSequenceCurves(const TSharedPtr<FJsonObject>& Params);
};
