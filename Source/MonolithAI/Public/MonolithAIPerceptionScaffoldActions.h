// MonolithAIPerceptionScaffoldActions.h
// Phase F8 (J-phase) — author-time perception scaffolding for ANY actor
// Blueprint (not just AIControllers). The existing
// FMonolithAIPerceptionActions::HandleAddPerceptionComponent is restricted to
// AIController BPs and accepts only a single dominant_sense; this new variant
// accepts any UAIPerceptionComponent-bearing actor target and a senses array.
//
// Action: ai::add_perception_to_actor
//   Adds UAIPerceptionComponent (if absent) to the BP's SCS, then for each
//   requested sense adds the corresponding UAISenseConfig_<Sense> to the
//   component's SensesConfig array. Marks BP modified + compiles.
//
// Supported senses v1: Sight, Hearing, Damage. Touch/Team/Prediction reserved
// for v2 — calling them returns a clear error with the supported list.
#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithAIPerceptionScaffoldActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult HandleAddPerceptionToActor(const TSharedPtr<FJsonObject>& Params);
};
