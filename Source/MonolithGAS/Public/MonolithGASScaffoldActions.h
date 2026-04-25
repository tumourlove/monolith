#pragma once

#include "MonolithGASInternal.h"

class FMonolithGASScaffoldActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// Phase 1: Scaffolding
	static FMonolithActionResult HandleBootstrapGASFoundation(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleValidateGASSetup(const TSharedPtr<FJsonObject>& Params);

	// Phase 2: Scaffolding
	static FMonolithActionResult HandleScaffoldGASProject(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleScaffoldDamagePipeline(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleScaffoldStatusEffect(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleScaffoldWeaponAbility(const TSharedPtr<FJsonObject>& Params);

	// Phase F8 (J-phase): author-time ability grant via ASC-CDO startup-array reflection
	static FMonolithActionResult HandleGrantAbilityToPawn(const TSharedPtr<FJsonObject>& Params);
};
