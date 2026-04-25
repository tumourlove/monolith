// MonolithAIPerceptionScaffoldActions.cpp
// Phase F8 (J-phase) implementation. See header for action contract.
//
// API verification (UE 5.7, editor offline at author time):
//   * USimpleConstructionScript::CreateNode / AddNode
//                              — Engine/Public/Engine/SimpleConstructionScript.h
//                                 (mirrors MonolithAIPerceptionActions:421-429,
//                                  MonolithAINavigationActions:1567-1593).
//   * UAIPerceptionComponent::ConfigureSense / GetSenseConfig<T>
//                              — AIModule/Public/Perception/AIPerceptionComponent.h
//                                 (mirrors MonolithAIPerceptionActions:249-263).
//   * UAISenseConfig_Sight::SightRadius / LoseSightRadius
//                              — AIModule/Public/Perception/AISenseConfig_Sight.h.
//   * UAISenseConfig_Hearing::HearingRange
//                              — AIModule/Public/Perception/AISenseConfig_Hearing.h.
//   * FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified
//                              — Kismet/Public/Kismet2/BlueprintEditorUtils.h.
//
// Failure modes (no crashes — clean ok=false for all):
//   * BP not loadable / not an Actor subclass.
//   * BP has no SimpleConstructionScript (rare for Actor BPs).
//   * Unknown sense name.
//   * Empty senses list (always returns ok=false with explicit error).

#include "MonolithAIPerceptionScaffoldActions.h"
#include "MonolithAIInternal.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"

#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Perception/AISenseConfig_Damage.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"

namespace
{
	/** Find the first SCS_Node whose component class is UAIPerceptionComponent. */
	USCS_Node* FindPerceptionSCSNode(UBlueprint* BP)
	{
		if (!BP || !BP->SimpleConstructionScript) return nullptr;
		for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->ComponentClass &&
				Node->ComponentClass->IsChildOf(UAIPerceptionComponent::StaticClass()))
			{
				return Node;
			}
		}
		return nullptr;
	}
}

void FMonolithAIPerceptionScaffoldActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("ai"), TEXT("add_perception_to_actor"),
		TEXT("Add UAIPerceptionComponent to ANY Actor BP (not just AIControllers) and configure senses in one call. Senses v1: Sight, Hearing, Damage. Optional sight_radius (default 1500) for Sight, hearing_range (default 3000) for Hearing."),
		FMonolithActionHandler::CreateStatic(&HandleAddPerceptionToActor),
		FParamSchemaBuilder()
			.Required(TEXT("actor_bp_path"), TEXT("string"), TEXT("Actor Blueprint asset path (e.g. /Game/Tests/BP_TestActor)"))
			.Required(TEXT("senses"), TEXT("array"), TEXT("Array of sense names. Supported: [\"Sight\", \"Hearing\", \"Damage\"]."))
			.Optional(TEXT("sight_radius"), TEXT("number"), TEXT("Sight radius (cm). Only applied to Sight sense if present."), TEXT("1500"))
			.Optional(TEXT("hearing_range"), TEXT("number"), TEXT("Hearing range (cm). Only applied to Hearing sense if present."), TEXT("3000"))
			.Build());
}

FMonolithActionResult FMonolithAIPerceptionScaffoldActions::HandleAddPerceptionToActor(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorBPPath;
	if (!Params->TryGetStringField(TEXT("actor_bp_path"), ActorBPPath) || ActorBPPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: actor_bp_path"));
	}

	const TArray<TSharedPtr<FJsonValue>>* SensesArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("senses"), SensesArr) || !SensesArr || SensesArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: senses (array). Supported: [\"Sight\", \"Hearing\", \"Damage\"]."));
	}

	// Parse + validate sense names up front so we error before mutating the BP.
	TArray<FString> RequestedSenses;
	TArray<FString> Unsupported;
	for (const TSharedPtr<FJsonValue>& V : *SensesArr)
	{
		FString S;
		if (!V.IsValid() || !V->TryGetString(S) || S.IsEmpty()) continue;
		if (S.Equals(TEXT("Sight"), ESearchCase::IgnoreCase) ||
			S.Equals(TEXT("Hearing"), ESearchCase::IgnoreCase) ||
			S.Equals(TEXT("Damage"), ESearchCase::IgnoreCase))
		{
			RequestedSenses.AddUnique(S);
		}
		else
		{
			Unsupported.AddUnique(S);
		}
	}
	if (Unsupported.Num() > 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown sense(s): [%s]. Supported v1: [Sight, Hearing, Damage]. Touch/Team/Prediction reserved for v2."),
			*FString::Join(Unsupported, TEXT(", "))));
	}
	if (RequestedSenses.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("senses array contained no recognizable values."));
	}

	// Load BP via the centralized 4-tier resolver. Accept any UBlueprint with
	// an Actor-subclass GeneratedClass.
	FString LoadError;
	UObject* Obj = MonolithAI::LoadAssetFromPath(ActorBPPath, LoadError);
	UBlueprint* BP = Cast<UBlueprint>(Obj);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Actor Blueprint not found at '%s' (%s)"), *ActorBPPath, *LoadError));
	}
	if (!BP->GeneratedClass || !BP->GeneratedClass->IsChildOf(AActor::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset at '%s' is not an Actor Blueprint"), *ActorBPPath));
	}
	if (!BP->SimpleConstructionScript)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Actor Blueprint '%s' has no SimpleConstructionScript"), *ActorBPPath));
	}

	double SightRadius = 1500.0;
	double HearingRange = 3000.0;
	Params->TryGetNumberField(TEXT("sight_radius"), SightRadius);
	Params->TryGetNumberField(TEXT("hearing_range"), HearingRange);

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add Perception To Actor")));

	// 1. Add UAIPerceptionComponent SCS node if absent.
	USCS_Node* PerceptionNode = FindPerceptionSCSNode(BP);
	bool bComponentAdded = false;
	if (!PerceptionNode)
	{
		const EBlueprintStatus SavedStatus = BP->Status;
		BP->Status = BS_BeingCreated;

		PerceptionNode = BP->SimpleConstructionScript->CreateNode(
			UAIPerceptionComponent::StaticClass(), FName(TEXT("AIPerceptionComponent")));
		if (!PerceptionNode)
		{
			BP->Status = SavedStatus;
			return FMonolithActionResult::Error(TEXT("Failed to create UAIPerceptionComponent SCS node"));
		}
		BP->SimpleConstructionScript->AddNode(PerceptionNode);
		BP->Status = SavedStatus;
		bComponentAdded = true;
	}

	UAIPerceptionComponent* PerceptionTemplate = Cast<UAIPerceptionComponent>(PerceptionNode->ComponentTemplate);
	if (!PerceptionTemplate)
	{
		return FMonolithActionResult::Error(TEXT("Perception SCS node has no component template"));
	}

	// 2. Add each requested sense config (using ConfigureSense — public API; same
	//    pattern as MonolithAIPerceptionActions::FindOrCreateSenseConfig).
	TArray<TSharedPtr<FJsonValue>> SensesAddedArr;
	for (const FString& Sense : RequestedSenses)
	{
		if (Sense.Equals(TEXT("Sight"), ESearchCase::IgnoreCase))
		{
			UAISenseConfig_Sight* Existing = PerceptionTemplate->GetSenseConfig<UAISenseConfig_Sight>();
			UAISenseConfig_Sight* SightCfg = Existing;
			bool bWasAdded = false;
			if (!SightCfg)
			{
				SightCfg = NewObject<UAISenseConfig_Sight>(PerceptionTemplate);
				bWasAdded = true;
			}
			SightCfg->SightRadius = static_cast<float>(SightRadius);
			// LoseSightRadius gets a +10% margin to avoid identical-edge flicker — same default as the existing configure_sight_sense action.
			SightCfg->LoseSightRadius = static_cast<float>(SightRadius * 1.1);
			if (bWasAdded)
			{
				PerceptionTemplate->ConfigureSense(*SightCfg);
			}

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("sense"), TEXT("Sight"));
			Row->SetBoolField(TEXT("was_added"), bWasAdded);
			Row->SetNumberField(TEXT("sight_radius"), SightRadius);
			SensesAddedArr.Add(MakeShared<FJsonValueObject>(Row));
		}
		else if (Sense.Equals(TEXT("Hearing"), ESearchCase::IgnoreCase))
		{
			UAISenseConfig_Hearing* Existing = PerceptionTemplate->GetSenseConfig<UAISenseConfig_Hearing>();
			UAISenseConfig_Hearing* HearingCfg = Existing;
			bool bWasAdded = false;
			if (!HearingCfg)
			{
				HearingCfg = NewObject<UAISenseConfig_Hearing>(PerceptionTemplate);
				bWasAdded = true;
			}
			HearingCfg->HearingRange = static_cast<float>(HearingRange);
			if (bWasAdded)
			{
				PerceptionTemplate->ConfigureSense(*HearingCfg);
			}

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("sense"), TEXT("Hearing"));
			Row->SetBoolField(TEXT("was_added"), bWasAdded);
			Row->SetNumberField(TEXT("hearing_range"), HearingRange);
			SensesAddedArr.Add(MakeShared<FJsonValueObject>(Row));
		}
		else if (Sense.Equals(TEXT("Damage"), ESearchCase::IgnoreCase))
		{
			UAISenseConfig_Damage* Existing = PerceptionTemplate->GetSenseConfig<UAISenseConfig_Damage>();
			UAISenseConfig_Damage* DamageCfg = Existing;
			bool bWasAdded = false;
			if (!DamageCfg)
			{
				DamageCfg = NewObject<UAISenseConfig_Damage>(PerceptionTemplate);
				bWasAdded = true;
				PerceptionTemplate->ConfigureSense(*DamageCfg);
			}

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("sense"), TEXT("Damage"));
			Row->SetBoolField(TEXT("was_added"), bWasAdded);
			SensesAddedArr.Add(MakeShared<FJsonValueObject>(Row));
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("actor_bp_path"), ActorBPPath);
	Result->SetBoolField(TEXT("perception_component_added"), bComponentAdded);
	Result->SetArrayField(TEXT("senses_added"), SensesAddedArr);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Configured %d sense(s) on '%s' (component %s)."),
			SensesAddedArr.Num(), *ActorBPPath,
			bComponentAdded ? TEXT("created") : TEXT("reused")));
	return FMonolithActionResult::Success(Result);
}
