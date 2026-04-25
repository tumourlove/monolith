#include "MonolithAIPerceptionActions.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#include "AIController.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AIPerceptionStimuliSourceComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Perception/AISenseConfig_Damage.h"
#include "Perception/AISenseConfig_Touch.h"
#include "Perception/AISenseConfig_Team.h"
#include "Perception/AISenseConfig_Prediction.h"
#include "Perception/AISense_Sight.h"
#include "Perception/AISense_Hearing.h"
#include "Perception/AISense_Damage.h"
#include "Perception/AISense_Touch.h"
#include "Perception/AISense_Team.h"
#include "Perception/AISense_Prediction.h"
#include "Perception/AIPerceptionSystem.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Editor.h"
#include "ScopedTransaction.h"

// ============================================================
//  Registration
// ============================================================

void FMonolithAIPerceptionActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 109. add_perception_component
	Registry.RegisterAction(TEXT("ai"), TEXT("add_perception_component"),
		TEXT("Add UAIPerceptionComponent to an AI controller Blueprint via SCS"),
		FMonolithActionHandler::CreateStatic(&HandleAddPerceptionComponent),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AIController Blueprint asset path"))
			.Optional(TEXT("dominant_sense"), TEXT("string"), TEXT("Dominant sense type: Sight, Hearing, Damage, Touch, Team, Prediction"))
			.Build());

	// 110. get_perception_config
	Registry.RegisterAction(TEXT("ai"), TEXT("get_perception_config"),
		TEXT("Read all perception senses, params, and affiliation filters from an AI controller Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleGetPerceptionConfig),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AIController Blueprint asset path"))
			.Build());

	// 111. configure_sight_sense
	Registry.RegisterAction(TEXT("ai"), TEXT("configure_sight_sense"),
		TEXT("Configure sight sense on a perception component (creates if absent). Reflection-writes to UAISenseConfig_Sight + base UAISenseConfig (MaxAge, bStartsEnabled)."),
		FMonolithActionHandler::CreateStatic(&HandleConfigureSightSense),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AIController Blueprint asset path"))
			.Required(TEXT("radius"), TEXT("number"), TEXT("Sight radius (cm)"))
			.Optional(TEXT("lose_radius"), TEXT("number"), TEXT("Lose-sight radius (cm, default: radius * 1.1)"))
			.Optional(TEXT("peripheral_angle"), TEXT("number"), TEXT("Peripheral vision half-angle in degrees (default: 90)"))
			.Optional(TEXT("affiliation"), TEXT("object"), TEXT("Detection affiliation: {enemies: bool, neutrals: bool, friendlies: bool}"))
			.Optional(TEXT("auto_success_range"), TEXT("number"), TEXT("Auto-success range from last seen location (-1 to disable)"))
			.Optional(TEXT("pov_offset"), TEXT("number"), TEXT("Point-of-view backward offset"))
			.Optional(TEXT("near_clipping_radius"), TEXT("number"), TEXT("Distance below which sight detection is disabled (cm). Reflection-write to UAISenseConfig_Sight::NearClippingRadius."))
			.Optional(TEXT("auto_register_all_pawns"), TEXT("boolean"), TEXT("Auto-register every pawn as a sight stimulus source (writes class default UAISense_Sight::bAutoRegisterAllPawnsAsSources — affects all sight senses globally)."))
			.Optional(TEXT("max_age"), TEXT("number"), TEXT("Stimulus max age in seconds (0 = never expires). Reflection-write to UAISenseConfig::MaxAge."))
			.Optional(TEXT("starts_enabled"), TEXT("boolean"), TEXT("Whether sense fires immediately on creation (default: true). Reflection-write to UAISenseConfig::bStartsEnabled bitfield."))
			.Build());

	// 112. configure_hearing_sense
	Registry.RegisterAction(TEXT("ai"), TEXT("configure_hearing_sense"),
		TEXT("Configure hearing sense on a perception component (creates if absent). Reflection-writes to UAISenseConfig_Hearing + base UAISenseConfig (MaxAge, bStartsEnabled)."),
		FMonolithActionHandler::CreateStatic(&HandleConfigureHearingSense),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AIController Blueprint asset path"))
			.Required(TEXT("range"), TEXT("number"), TEXT("Hearing range (cm)"))
			.Optional(TEXT("affiliation"), TEXT("object"), TEXT("Detection affiliation: {enemies: bool, neutrals: bool, friendlies: bool}"))
			.Optional(TEXT("max_age"), TEXT("number"), TEXT("Stimulus max age in seconds (0 = never expires). Reflection-write to UAISenseConfig::MaxAge."))
			.Optional(TEXT("starts_enabled"), TEXT("boolean"), TEXT("Whether sense fires immediately on creation (default: true). Reflection-write to UAISenseConfig::bStartsEnabled bitfield."))
			.Build());

	// 113. configure_damage_sense
	Registry.RegisterAction(TEXT("ai"), TEXT("configure_damage_sense"),
		TEXT("Configure damage sense on a perception component (creates if absent). Reflection-writes to UAISenseConfig_Damage + base UAISenseConfig (MaxAge, bStartsEnabled)."),
		FMonolithActionHandler::CreateStatic(&HandleConfigureDamageSense),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AIController Blueprint asset path"))
			.Optional(TEXT("implementation"), TEXT("string"), TEXT("Custom damage sense implementation class"))
			.Optional(TEXT("max_age"), TEXT("number"), TEXT("Stimulus max age in seconds (0 = never expires). Reflection-write to UAISenseConfig::MaxAge."))
			.Optional(TEXT("starts_enabled"), TEXT("boolean"), TEXT("Whether sense fires immediately on creation (default: true). Reflection-write to UAISenseConfig::bStartsEnabled bitfield."))
			.Build());

	// 114. configure_touch_sense
	Registry.RegisterAction(TEXT("ai"), TEXT("configure_touch_sense"),
		TEXT("Configure touch sense on a perception component (creates if absent). Reflection-writes to UAISenseConfig_Touch + base UAISenseConfig (MaxAge, bStartsEnabled). Note: UAISenseConfig_Touch has no Implementation UPROPERTY in UE 5.7 — class is hardcoded."),
		FMonolithActionHandler::CreateStatic(&HandleConfigureTouchSense),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AIController Blueprint asset path"))
			.Optional(TEXT("affiliation"), TEXT("object"), TEXT("Detection affiliation: {enemies: bool, neutrals: bool, friendlies: bool}"))
			.Optional(TEXT("max_age"), TEXT("number"), TEXT("Stimulus max age in seconds (0 = never expires). Reflection-write to UAISenseConfig::MaxAge."))
			.Optional(TEXT("starts_enabled"), TEXT("boolean"), TEXT("Whether sense fires immediately on creation (default: true). Reflection-write to UAISenseConfig::bStartsEnabled bitfield."))
			.Build());

	// 117. remove_sense
	Registry.RegisterAction(TEXT("ai"), TEXT("remove_sense"),
		TEXT("Remove a sense configuration from a perception component"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveSense),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AIController Blueprint asset path"))
			.Required(TEXT("sense_type"), TEXT("string"), TEXT("Sense type to remove: Sight, Hearing, Damage, Touch, Team, Prediction"))
			.Build());

	// 118. add_stimuli_source_component
	Registry.RegisterAction(TEXT("ai"), TEXT("add_stimuli_source_component"),
		TEXT("Add UAIPerceptionStimuliSourceComponent to any actor Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleAddStimuliSourceComponent),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Actor Blueprint asset path"))
			.Required(TEXT("register_as_source_for"), TEXT("array"), TEXT("Array of sense types to register as source for: [\"Sight\", \"Hearing\"]"))
			.Optional(TEXT("auto_register"), TEXT("boolean"), TEXT("Auto-register as source on BeginPlay (default: true)"))
			.Build());

	// 119. configure_stimuli_source
	Registry.RegisterAction(TEXT("ai"), TEXT("configure_stimuli_source"),
		TEXT("Configure which senses a stimuli source component registers for"),
		FMonolithActionHandler::CreateStatic(&HandleConfigureStimuliSource),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Actor Blueprint asset path"))
			.Required(TEXT("sense_types"), TEXT("array"), TEXT("Array of sense types to register for: [\"Sight\", \"Hearing\"]"))
			.Optional(TEXT("auto_register"), TEXT("boolean"), TEXT("Auto-register as source on BeginPlay"))
			.Build());

	// 126. validate_perception_setup
	Registry.RegisterAction(TEXT("ai"), TEXT("validate_perception_setup"),
		TEXT("Validate perception setup: senses configured, affiliation set, dominant sense assigned"),
		FMonolithActionHandler::CreateStatic(&HandleValidatePerceptionSetup),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AIController Blueprint asset path"))
			.Build());

	// 218. get_ai_system_config
	Registry.RegisterAction(TEXT("ai"), TEXT("get_ai_system_config"),
		TEXT("Read UAISystem global settings (perception aging, stimulus limits, etc.)"),
		FMonolithActionHandler::CreateStatic(&HandleGetAISystemConfig),
		FParamSchemaBuilder()
			.Build());
}

// ============================================================
//  Helpers
// ============================================================

USCS_Node* FMonolithAIPerceptionActions::FindPerceptionNode(UBlueprint* BP)
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

USCS_Node* FMonolithAIPerceptionActions::FindStimuliSourceNode(UBlueprint* BP)
{
	if (!BP || !BP->SimpleConstructionScript) return nullptr;

	for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
	{
		if (Node && Node->ComponentClass &&
			Node->ComponentClass->IsChildOf(UAIPerceptionStimuliSourceComponent::StaticClass()))
		{
			return Node;
		}
	}
	return nullptr;
}

UAIPerceptionComponent* FMonolithAIPerceptionActions::GetPerceptionTemplate(USCS_Node* Node)
{
	if (!Node || !Node->ComponentTemplate) return nullptr;
	return Cast<UAIPerceptionComponent>(Node->ComponentTemplate);
}

void FMonolithAIPerceptionActions::ParseAffiliation(const TSharedPtr<FJsonObject>& Params, const FString& FieldName,
	bool& bEnemies, bool& bNeutrals, bool& bFriendlies)
{
	// Defaults
	bEnemies = true;
	bNeutrals = false;
	bFriendlies = false;

	if (!Params->HasField(FieldName))
	{
		return;
	}

	// Try as object: {"enemies": true, "neutrals": true, "friendlies": false}
	const TSharedPtr<FJsonObject>* AffObj = nullptr;
	if (Params->TryGetObjectField(FieldName, AffObj) && AffObj && (*AffObj)->Values.Num() > 0)
	{
		if ((*AffObj)->HasField(TEXT("enemies")))
			bEnemies = (*AffObj)->GetBoolField(TEXT("enemies"));
		if ((*AffObj)->HasField(TEXT("neutrals")))
			bNeutrals = (*AffObj)->GetBoolField(TEXT("neutrals"));
		if ((*AffObj)->HasField(TEXT("friendlies")))
			bFriendlies = (*AffObj)->GetBoolField(TEXT("friendlies"));
		return;
	}

	// Try as comma-separated string: "enemies,neutrals"
	FString AffStr = Params->GetStringField(FieldName);
	if (!AffStr.IsEmpty())
	{
		bEnemies = false;
		bNeutrals = false;
		bFriendlies = false;

		TArray<FString> Parts;
		AffStr.ParseIntoArray(Parts, TEXT(","));
		for (FString& Part : Parts)
		{
			Part.TrimStartAndEndInline();
			if (Part.Equals(TEXT("enemies"), ESearchCase::IgnoreCase))
				bEnemies = true;
			else if (Part.Equals(TEXT("neutrals"), ESearchCase::IgnoreCase))
				bNeutrals = true;
			else if (Part.Equals(TEXT("friendlies"), ESearchCase::IgnoreCase))
				bFriendlies = true;
		}
	}
}

TSharedPtr<FJsonObject> FMonolithAIPerceptionActions::AffiliationToJson(const FAISenseAffiliationFilter& Filter)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("enemies"), Filter.bDetectEnemies);
	Obj->SetBoolField(TEXT("neutrals"), Filter.bDetectNeutrals);
	Obj->SetBoolField(TEXT("friendlies"), Filter.bDetectFriendlies);
	return Obj;
}

template<typename TSenseConfig>
TSenseConfig* FMonolithAIPerceptionActions::FindOrCreateSenseConfig(UAIPerceptionComponent* PerceptionComp)
{
	if (!PerceptionComp) return nullptr;

	// Check existing via public template getter
	TSenseConfig* Existing = PerceptionComp->GetSenseConfig<TSenseConfig>();
	if (Existing)
	{
		return Existing;
	}

	// Create new and register via public ConfigureSense API
	TSenseConfig* NewConfig = NewObject<TSenseConfig>(PerceptionComp);
	PerceptionComp->ConfigureSense(*NewConfig);
	return NewConfig;
}

template<typename TSenseConfig>
TSenseConfig* FMonolithAIPerceptionActions::FindSenseConfig(UAIPerceptionComponent* PerceptionComp)
{
	if (!PerceptionComp) return nullptr;
	return PerceptionComp->GetSenseConfig<TSenseConfig>();
}

UClass* FMonolithAIPerceptionActions::ResolveSenseConfigClass(const FString& SenseType)
{
	if (SenseType.Equals(TEXT("Sight"), ESearchCase::IgnoreCase))
		return UAISenseConfig_Sight::StaticClass();
	if (SenseType.Equals(TEXT("Hearing"), ESearchCase::IgnoreCase))
		return UAISenseConfig_Hearing::StaticClass();
	if (SenseType.Equals(TEXT("Damage"), ESearchCase::IgnoreCase))
		return UAISenseConfig_Damage::StaticClass();
	if (SenseType.Equals(TEXT("Touch"), ESearchCase::IgnoreCase))
		return UAISenseConfig_Touch::StaticClass();
	if (SenseType.Equals(TEXT("Team"), ESearchCase::IgnoreCase))
		return UAISenseConfig_Team::StaticClass();
	if (SenseType.Equals(TEXT("Prediction"), ESearchCase::IgnoreCase))
		return UAISenseConfig_Prediction::StaticClass();
	return nullptr;
}

/** Resolve sense type string to UAISense subclass (for stimuli source registration) */
static TSubclassOf<UAISense> ResolveSenseClass(const FString& SenseType)
{
	if (SenseType.Equals(TEXT("Sight"), ESearchCase::IgnoreCase))
		return UAISense_Sight::StaticClass();
	if (SenseType.Equals(TEXT("Hearing"), ESearchCase::IgnoreCase))
		return UAISense_Hearing::StaticClass();
	if (SenseType.Equals(TEXT("Damage"), ESearchCase::IgnoreCase))
		return UAISense_Damage::StaticClass();
	if (SenseType.Equals(TEXT("Touch"), ESearchCase::IgnoreCase))
		return UAISense_Touch::StaticClass();
	if (SenseType.Equals(TEXT("Team"), ESearchCase::IgnoreCase))
		return UAISense_Team::StaticClass();
	if (SenseType.Equals(TEXT("Prediction"), ESearchCase::IgnoreCase))
		return UAISense_Prediction::StaticClass();
	return nullptr;
}

TSharedPtr<FJsonObject> FMonolithAIPerceptionActions::SenseConfigToJson(UAISenseConfig* SenseConfig)
{
	if (!SenseConfig) return nullptr;

	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("sense_class"), SenseConfig->GetClass()->GetName());

	if (UAISenseConfig_Sight* Sight = Cast<UAISenseConfig_Sight>(SenseConfig))
	{
		Obj->SetStringField(TEXT("type"), TEXT("Sight"));
		Obj->SetNumberField(TEXT("sight_radius"), Sight->SightRadius);
		Obj->SetNumberField(TEXT("lose_sight_radius"), Sight->LoseSightRadius);
		Obj->SetNumberField(TEXT("peripheral_vision_angle"), Sight->PeripheralVisionAngleDegrees);
		Obj->SetNumberField(TEXT("auto_success_range"), Sight->AutoSuccessRangeFromLastSeenLocation);
		Obj->SetNumberField(TEXT("pov_backward_offset"), Sight->PointOfViewBackwardOffset);
		Obj->SetNumberField(TEXT("near_clipping_radius"), Sight->NearClippingRadius);
		Obj->SetObjectField(TEXT("affiliation"), AffiliationToJson(Sight->DetectionByAffiliation));
		if (Sight->Implementation)
		{
			Obj->SetStringField(TEXT("implementation"), Sight->Implementation->GetName());
		}
		// CDO-level UAISense_Sight::bAutoRegisterAllPawnsAsSources (global, not per-config — surfaced so callers can see it)
		if (const UAISense_Sight* SenseCDO = GetDefault<UAISense_Sight>())
		{
			if (FBoolProperty* Prop = CastField<FBoolProperty>(
					UAISense::StaticClass()->FindPropertyByName(TEXT("bAutoRegisterAllPawnsAsSources"))))
			{
				Obj->SetBoolField(TEXT("auto_register_all_pawns"), Prop->GetPropertyValue_InContainer(SenseCDO));
			}
		}
	}
	else if (UAISenseConfig_Hearing* Hearing = Cast<UAISenseConfig_Hearing>(SenseConfig))
	{
		Obj->SetStringField(TEXT("type"), TEXT("Hearing"));
		Obj->SetNumberField(TEXT("hearing_range"), Hearing->HearingRange);
		Obj->SetObjectField(TEXT("affiliation"), AffiliationToJson(Hearing->DetectionByAffiliation));
		if (Hearing->Implementation)
		{
			Obj->SetStringField(TEXT("implementation"), Hearing->Implementation->GetName());
		}
	}
	else if (UAISenseConfig_Damage* Damage = Cast<UAISenseConfig_Damage>(SenseConfig))
	{
		Obj->SetStringField(TEXT("type"), TEXT("Damage"));
		if (Damage->Implementation)
		{
			Obj->SetStringField(TEXT("implementation"), Damage->Implementation->GetName());
		}
	}
	else if (UAISenseConfig_Touch* Touch = Cast<UAISenseConfig_Touch>(SenseConfig))
	{
		Obj->SetStringField(TEXT("type"), TEXT("Touch"));
		Obj->SetObjectField(TEXT("affiliation"), AffiliationToJson(Touch->DetectionByAffiliation));
		// UAISenseConfig_Touch has no Implementation UPROPERTY in UE 5.7 — class is hardcoded.
	}
	else if (UAISenseConfig_Team* Team = Cast<UAISenseConfig_Team>(SenseConfig))
	{
		Obj->SetStringField(TEXT("type"), TEXT("Team"));
		// UAISenseConfig_Team has no Implementation UPROPERTY in UE 5.7 — class is hardcoded.
	}
	else if (UAISenseConfig_Prediction* Prediction = Cast<UAISenseConfig_Prediction>(SenseConfig))
	{
		Obj->SetStringField(TEXT("type"), TEXT("Prediction"));
		// UAISenseConfig_Prediction has no Implementation UPROPERTY in UE 5.7 — class is hardcoded.
	}
	else
	{
		Obj->SetStringField(TEXT("type"), SenseConfig->GetClass()->GetName());
	}

	// Common fields (UAISenseConfig base class)
	Obj->SetNumberField(TEXT("max_age"), SenseConfig->GetMaxAge());
	// bStartsEnabled — protected uint32:1, read via reflection so we don't bind to the deprecated IsEnabled() accessor
	if (FBoolProperty* StartsProp = CastField<FBoolProperty>(
			UAISenseConfig::StaticClass()->FindPropertyByName(TEXT("bStartsEnabled"))))
	{
		Obj->SetBoolField(TEXT("starts_enabled"), StartsProp->GetPropertyValue_InContainer(SenseConfig));
	}

	return Obj;
}

// ============================================================
//  109. add_perception_component
// ============================================================

FMonolithActionResult FMonolithAIPerceptionActions::HandleAddPerceptionComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBlueprint* BP = MonolithAI::LoadAIControllerFromParams(Params, AssetPath, Error);
	if (!BP)
	{
		return FMonolithActionResult::Error(Error);
	}

	if (!BP->SimpleConstructionScript)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript — is it an Actor Blueprint?"));
	}

	// Check if perception component already exists
	if (FindPerceptionNode(BP))
	{
		return FMonolithActionResult::Error(
			TEXT("This AIController already has a UAIPerceptionComponent. Use configure_sight_sense etc. to modify it."));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add Perception Component")));

	// Suppress skeleton regen during SCS modification
	const EBlueprintStatus SavedStatus = BP->Status;
	BP->Status = BS_BeingCreated;

	USCS_Node* NewNode = BP->SimpleConstructionScript->CreateNode(
		UAIPerceptionComponent::StaticClass(), FName(TEXT("AIPerceptionComponent")));
	if (!NewNode)
	{
		BP->Status = SavedStatus;
		return FMonolithActionResult::Error(TEXT("Failed to create perception component SCS node"));
	}

	BP->SimpleConstructionScript->AddNode(NewNode);

	// Set dominant sense if specified
	FString DominantSense = Params->GetStringField(TEXT("dominant_sense"));
	UAIPerceptionComponent* PerceptionComp = GetPerceptionTemplate(NewNode);
	if (PerceptionComp && !DominantSense.IsEmpty())
	{
		TSubclassOf<UAISense> SenseClass = ResolveSenseClass(DominantSense);
		if (SenseClass)
		{
			PerceptionComp->SetDominantSense(*SenseClass);
		}
	}

	BP->Status = SavedStatus;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(AssetPath, TEXT("AIPerceptionComponent added"));
	Result->SetStringField(TEXT("variable_name"), NewNode->GetVariableName().ToString());
	if (!DominantSense.IsEmpty())
	{
		Result->SetStringField(TEXT("dominant_sense"), DominantSense);
	}

	TArray<TSharedPtr<FJsonValue>> NextSteps;
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Use configure_sight_sense, configure_hearing_sense, etc. to add senses")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Use validate_perception_setup to check configuration")));
	Result->SetArrayField(TEXT("next_steps"), NextSteps);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  110. get_perception_config
// ============================================================

FMonolithActionResult FMonolithAIPerceptionActions::HandleGetPerceptionConfig(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBlueprint* BP = MonolithAI::LoadAIControllerFromParams(Params, AssetPath, Error);
	if (!BP)
	{
		return FMonolithActionResult::Error(Error);
	}

	USCS_Node* PerceptionNode = FindPerceptionNode(BP);
	if (!PerceptionNode)
	{
		return FMonolithActionResult::Error(
			TEXT("No UAIPerceptionComponent found on this AIController. Use add_perception_component first."));
	}

	UAIPerceptionComponent* PerceptionComp = GetPerceptionTemplate(PerceptionNode);
	if (!PerceptionComp)
	{
		return FMonolithActionResult::Error(TEXT("Failed to get perception component template"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("variable_name"), PerceptionNode->GetVariableName().ToString());

	// Senses — iterate via public API
	TArray<TSharedPtr<FJsonValue>> SensesArr;
	for (auto It = PerceptionComp->GetSensesConfigIterator(); It; ++It)
	{
		UAISenseConfig* Sense = *It;
		TSharedPtr<FJsonObject> SenseObj = SenseConfigToJson(Sense);
		if (SenseObj.IsValid())
		{
			SensesArr.Add(MakeShared<FJsonValueObject>(SenseObj));
		}
	}
	Result->SetArrayField(TEXT("senses"), SensesArr);
	Result->SetNumberField(TEXT("sense_count"), SensesArr.Num());

	// Dominant sense
	TSubclassOf<UAISense> DomSense = PerceptionComp->GetDominantSense();
	if (DomSense)
	{
		Result->SetStringField(TEXT("dominant_sense"), DomSense->GetName());
	}
	else
	{
		Result->SetStringField(TEXT("dominant_sense"), TEXT("None"));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Helper: load controller + get perception comp (or error)
// ============================================================

namespace
{
	struct FPerceptionContext
	{
		UBlueprint* BP = nullptr;
		FString AssetPath;
		USCS_Node* PerceptionNode = nullptr;
		UAIPerceptionComponent* PerceptionComp = nullptr;
	};

	bool ResolvePerceptionContext(const TSharedPtr<FJsonObject>& Params, FPerceptionContext& Ctx, FMonolithActionResult& OutError)
	{
		FString Error;
		Ctx.BP = MonolithAI::LoadAIControllerFromParams(Params, Ctx.AssetPath, Error);
		if (!Ctx.BP)
		{
			OutError = FMonolithActionResult::Error(Error);
			return false;
		}

		Ctx.PerceptionNode = FMonolithAIPerceptionActions::FindPerceptionNode(Ctx.BP);
		if (!Ctx.PerceptionNode)
		{
			OutError = FMonolithActionResult::Error(
				TEXT("No UAIPerceptionComponent found. Use add_perception_component first."));
			return false;
		}

		Ctx.PerceptionComp = FMonolithAIPerceptionActions::GetPerceptionTemplate(Ctx.PerceptionNode);
		if (!Ctx.PerceptionComp)
		{
			OutError = FMonolithActionResult::Error(TEXT("Failed to get perception component template"));
			return false;
		}

		return true;
	}

	/**
	 * Apply UAISenseConfig base-class params (MaxAge, bStartsEnabled) to the given sense config via reflection,
	 * and emit verified_value entries into OutVerified for each one the caller passed.
	 *
	 * Both fields live on the protected base class — reflection lookup uses UAISenseConfig::StaticClass() so it
	 * works for any subclass instance. Mirrors the pre-existing Phase B max_age block on configure_damage_sense.
	 *
	 * @param SenseConfig  The concrete sense config instance (Sight/Hearing/Damage/Touch — must derive UAISenseConfig).
	 * @param Params       JSON params from the action invocation. Looks for "max_age" (number) and "starts_enabled" (bool).
	 * @param OutVerified  Verified-value JSON object the caller emits under top-level "verified_value".
	 * @param OutWarnings  Append-only warnings array; gets entries when reflection lookup fails (silent reverts).
	 */
	void ApplyBaseSenseConfigParams(
		UAISenseConfig* SenseConfig,
		const TSharedPtr<FJsonObject>& Params,
		const TSharedPtr<FJsonObject>& OutVerified,
		TArray<TSharedPtr<FJsonValue>>& OutWarnings)
	{
		if (!SenseConfig || !Params.IsValid() || !OutVerified.IsValid())
		{
			return;
		}

		// max_age — protected float on UAISenseConfig
		if (Params->HasField(TEXT("max_age")))
		{
			const float Requested = (float)Params->GetNumberField(TEXT("max_age"));
			float Actual = 0.0f;
			bool bWrote = false;
			if (FFloatProperty* MaxAgeProp = CastField<FFloatProperty>(
					UAISenseConfig::StaticClass()->FindPropertyByName(TEXT("MaxAge"))))
			{
				MaxAgeProp->SetPropertyValue_InContainer(SenseConfig, Requested);
				Actual = MaxAgeProp->GetPropertyValue_InContainer(SenseConfig);
				bWrote = true;
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("requested"), Requested);
			Entry->SetNumberField(TEXT("actual"), Actual);
			Entry->SetBoolField(TEXT("match"), bWrote && FMath::IsNearlyEqual(Requested, Actual));
			OutVerified->SetObjectField(TEXT("max_age"), Entry);

			if (!bWrote)
			{
				OutWarnings.Add(MakeShared<FJsonValueString>(
					TEXT("UAISenseConfig::MaxAge property not found via reflection — write skipped.")));
			}
		}

		// bStartsEnabled — uint32:1 bitfield UPROPERTY (UHT exposes as FBoolProperty)
		if (Params->HasField(TEXT("starts_enabled")))
		{
			const bool Requested = Params->GetBoolField(TEXT("starts_enabled"));
			bool Actual = false;
			bool bWrote = false;
			if (FBoolProperty* StartsProp = CastField<FBoolProperty>(
					UAISenseConfig::StaticClass()->FindPropertyByName(TEXT("bStartsEnabled"))))
			{
				StartsProp->SetPropertyValue_InContainer(SenseConfig, Requested);
				Actual = StartsProp->GetPropertyValue_InContainer(SenseConfig);
				bWrote = true;
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetBoolField(TEXT("requested"), Requested);
			Entry->SetBoolField(TEXT("actual"), Actual);
			Entry->SetBoolField(TEXT("match"), bWrote && Requested == Actual);
			OutVerified->SetObjectField(TEXT("starts_enabled"), Entry);

			if (!bWrote)
			{
				OutWarnings.Add(MakeShared<FJsonValueString>(
					TEXT("UAISenseConfig::bStartsEnabled property not found via reflection — write skipped.")));
			}
		}
	}
}

// ============================================================
//  111. configure_sight_sense
// ============================================================

FMonolithActionResult FMonolithAIPerceptionActions::HandleConfigureSightSense(const TSharedPtr<FJsonObject>& Params)
{
	FPerceptionContext Ctx;
	FMonolithActionResult ErrResult;
	if (!ResolvePerceptionContext(Params, Ctx, ErrResult))
	{
		return ErrResult;
	}

	double Radius = Params->GetNumberField(TEXT("radius"));
	if (Radius <= 0.0)
	{
		return FMonolithActionResult::Error(TEXT("radius must be a positive number"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Configure Sight Sense")));

	UAISenseConfig_Sight* Sight = FindOrCreateSenseConfig<UAISenseConfig_Sight>(Ctx.PerceptionComp);
	if (!Sight)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create sight sense config"));
	}

	Sight->SightRadius = static_cast<float>(Radius);

	// Lose radius: default to radius * 1.1
	if (Params->HasField(TEXT("lose_radius")))
	{
		Sight->LoseSightRadius = static_cast<float>(Params->GetNumberField(TEXT("lose_radius")));
	}
	else
	{
		Sight->LoseSightRadius = Sight->SightRadius * 1.1f;
	}

	// Peripheral vision angle
	if (Params->HasField(TEXT("peripheral_angle")))
	{
		Sight->PeripheralVisionAngleDegrees = static_cast<float>(Params->GetNumberField(TEXT("peripheral_angle")));
	}

	// Auto success range
	if (Params->HasField(TEXT("auto_success_range")))
	{
		Sight->AutoSuccessRangeFromLastSeenLocation = static_cast<float>(Params->GetNumberField(TEXT("auto_success_range")));
	}

	// POV offset
	if (Params->HasField(TEXT("pov_offset")))
	{
		Sight->PointOfViewBackwardOffset = static_cast<float>(Params->GetNumberField(TEXT("pov_offset")));
	}

	// Affiliation
	bool bEnemies, bNeutrals, bFriendlies;
	ParseAffiliation(Params, TEXT("affiliation"), bEnemies, bNeutrals, bFriendlies);
	if (Params->HasField(TEXT("affiliation")))
	{
		Sight->DetectionByAffiliation.bDetectEnemies = bEnemies;
		Sight->DetectionByAffiliation.bDetectNeutrals = bNeutrals;
		Sight->DetectionByAffiliation.bDetectFriendlies = bFriendlies;
	}

	// Per-sense additions: collect verified_value entries for sight-specific reflection writes
	TSharedPtr<FJsonObject> Verified = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Warnings;

	// near_clipping_radius — UAISenseConfig_Sight::NearClippingRadius (float)
	if (Params->HasField(TEXT("near_clipping_radius")))
	{
		const float Requested = (float)Params->GetNumberField(TEXT("near_clipping_radius"));
		float Actual = 0.0f;
		bool bWrote = false;
		if (FFloatProperty* Prop = CastField<FFloatProperty>(
				UAISenseConfig_Sight::StaticClass()->FindPropertyByName(TEXT("NearClippingRadius"))))
		{
			Prop->SetPropertyValue_InContainer(Sight, Requested);
			Actual = Prop->GetPropertyValue_InContainer(Sight);
			bWrote = true;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("requested"), Requested);
		Entry->SetNumberField(TEXT("actual"), Actual);
		Entry->SetBoolField(TEXT("match"), bWrote && FMath::IsNearlyEqual(Requested, Actual));
		Verified->SetObjectField(TEXT("near_clipping_radius"), Entry);

		if (!bWrote)
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				TEXT("UAISenseConfig_Sight::NearClippingRadius property not found via reflection — write skipped.")));
		}
	}

	// auto_register_all_pawns — UAISense::bAutoRegisterAllPawnsAsSources lives on the runtime sense class default
	// object (EditDefaultsOnly). Writing here mutates the CDO so all sight-sense instances inherit the new value.
	if (Params->HasField(TEXT("auto_register_all_pawns")))
	{
		const bool Requested = Params->GetBoolField(TEXT("auto_register_all_pawns"));
		bool Actual = false;
		bool bWrote = false;
		if (UAISense_Sight* SenseCDO = GetMutableDefault<UAISense_Sight>())
		{
			if (FBoolProperty* Prop = CastField<FBoolProperty>(
					UAISense::StaticClass()->FindPropertyByName(TEXT("bAutoRegisterAllPawnsAsSources"))))
			{
				Prop->SetPropertyValue_InContainer(SenseCDO, Requested);
				Actual = Prop->GetPropertyValue_InContainer(SenseCDO);
				bWrote = true;
			}
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetBoolField(TEXT("requested"), Requested);
		Entry->SetBoolField(TEXT("actual"), Actual);
		Entry->SetBoolField(TEXT("match"), bWrote && Requested == Actual);
		Entry->SetStringField(TEXT("note"), TEXT("CDO-level write — affects all UAISense_Sight instances globally"));
		Verified->SetObjectField(TEXT("auto_register_all_pawns"), Entry);

		if (!bWrote)
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				TEXT("UAISense::bAutoRegisterAllPawnsAsSources property not found via reflection — write skipped.")));
		}
	}

	// Base-class params (max_age, bStartsEnabled) lifted via shared helper
	ApplyBaseSenseConfigParams(Sight, Params, Verified, Warnings);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.BP);

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(Ctx.AssetPath, TEXT("Sight sense configured"));
	Result->SetObjectField(TEXT("config"), SenseConfigToJson(Sight));
	if (Verified->Values.Num() > 0)
	{
		Result->SetObjectField(TEXT("verified_value"), Verified);
	}
	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  112. configure_hearing_sense
// ============================================================

FMonolithActionResult FMonolithAIPerceptionActions::HandleConfigureHearingSense(const TSharedPtr<FJsonObject>& Params)
{
	FPerceptionContext Ctx;
	FMonolithActionResult ErrResult;
	if (!ResolvePerceptionContext(Params, Ctx, ErrResult))
	{
		return ErrResult;
	}

	double Range = Params->GetNumberField(TEXT("range"));
	if (Range <= 0.0)
	{
		return FMonolithActionResult::Error(TEXT("range must be a positive number"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Configure Hearing Sense")));

	UAISenseConfig_Hearing* Hearing = FindOrCreateSenseConfig<UAISenseConfig_Hearing>(Ctx.PerceptionComp);
	if (!Hearing)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create hearing sense config"));
	}

	Hearing->HearingRange = static_cast<float>(Range);

	// Affiliation
	bool bEnemies, bNeutrals, bFriendlies;
	ParseAffiliation(Params, TEXT("affiliation"), bEnemies, bNeutrals, bFriendlies);
	if (Params->HasField(TEXT("affiliation")))
	{
		Hearing->DetectionByAffiliation.bDetectEnemies = bEnemies;
		Hearing->DetectionByAffiliation.bDetectNeutrals = bNeutrals;
		Hearing->DetectionByAffiliation.bDetectFriendlies = bFriendlies;
	}

	// Base-class params (max_age, bStartsEnabled) via shared helper
	TSharedPtr<FJsonObject> Verified = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Warnings;
	ApplyBaseSenseConfigParams(Hearing, Params, Verified, Warnings);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.BP);

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(Ctx.AssetPath, TEXT("Hearing sense configured"));
	Result->SetObjectField(TEXT("config"), SenseConfigToJson(Hearing));
	if (Verified->Values.Num() > 0)
	{
		Result->SetObjectField(TEXT("verified_value"), Verified);
	}
	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  113. configure_damage_sense
// ============================================================

FMonolithActionResult FMonolithAIPerceptionActions::HandleConfigureDamageSense(const TSharedPtr<FJsonObject>& Params)
{
	FPerceptionContext Ctx;
	FMonolithActionResult ErrResult;
	if (!ResolvePerceptionContext(Params, Ctx, ErrResult))
	{
		return ErrResult;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Configure Damage Sense")));

	UAISenseConfig_Damage* Damage = FindOrCreateSenseConfig<UAISenseConfig_Damage>(Ctx.PerceptionComp);
	if (!Damage)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create damage sense config"));
	}

	// Custom implementation class
	FString ImplStr = Params->GetStringField(TEXT("implementation"));
	if (!ImplStr.IsEmpty())
	{
		UClass* ImplClass = FindFirstObject<UClass>(*ImplStr, EFindFirstObjectOptions::NativeFirst);
		if (!ImplClass)
		{
			ImplClass = FindFirstObject<UClass>(*(TEXT("U") + ImplStr), EFindFirstObjectOptions::NativeFirst);
		}
		if (ImplClass && ImplClass->IsChildOf(UAISense_Damage::StaticClass()))
		{
			Damage->Implementation = ImplClass;
		}
		else
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Damage sense implementation class not found or not a UAISense_Damage subclass: %s"), *ImplStr));
		}
	}

	// Base-class params (max_age, bStartsEnabled) via shared helper. Replaces the prior Phase B inline
	// max_age block — same semantics, now extended with bStartsEnabled and reused across all sense actions.
	TSharedPtr<FJsonObject> Verified = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Warnings;
	ApplyBaseSenseConfigParams(Damage, Params, Verified, Warnings);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.BP);

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(Ctx.AssetPath, TEXT("Damage sense configured"));
	Result->SetObjectField(TEXT("config"), SenseConfigToJson(Damage));

	if (Verified->Values.Num() > 0)
	{
		Result->SetObjectField(TEXT("verified_value"), Verified);
	}
	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  114. configure_touch_sense
// ============================================================

FMonolithActionResult FMonolithAIPerceptionActions::HandleConfigureTouchSense(const TSharedPtr<FJsonObject>& Params)
{
	FPerceptionContext Ctx;
	FMonolithActionResult ErrResult;
	if (!ResolvePerceptionContext(Params, Ctx, ErrResult))
	{
		return ErrResult;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Configure Touch Sense")));

	UAISenseConfig_Touch* Touch = FindOrCreateSenseConfig<UAISenseConfig_Touch>(Ctx.PerceptionComp);
	if (!Touch)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create touch sense config"));
	}

	// Affiliation
	bool bEnemies, bNeutrals, bFriendlies;
	ParseAffiliation(Params, TEXT("affiliation"), bEnemies, bNeutrals, bFriendlies);
	if (Params->HasField(TEXT("affiliation")))
	{
		Touch->DetectionByAffiliation.bDetectEnemies = bEnemies;
		Touch->DetectionByAffiliation.bDetectNeutrals = bNeutrals;
		Touch->DetectionByAffiliation.bDetectFriendlies = bFriendlies;
	}

	// Base-class params (max_age, bStartsEnabled) via shared helper.
	// NOTE: UAISenseConfig_Touch has no Implementation UPROPERTY in UE 5.7 (verified via source_query) —
	// the runtime sense class is hardcoded by GetSenseImplementation(). No "implementation" param is wired.
	TSharedPtr<FJsonObject> Verified = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Warnings;
	ApplyBaseSenseConfigParams(Touch, Params, Verified, Warnings);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.BP);

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(Ctx.AssetPath, TEXT("Touch sense configured"));
	Result->SetObjectField(TEXT("config"), SenseConfigToJson(Touch));
	if (Verified->Values.Num() > 0)
	{
		Result->SetObjectField(TEXT("verified_value"), Verified);
	}
	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  117. remove_sense
// ============================================================

FMonolithActionResult FMonolithAIPerceptionActions::HandleRemoveSense(const TSharedPtr<FJsonObject>& Params)
{
	FPerceptionContext Ctx;
	FMonolithActionResult ErrResult;
	if (!ResolvePerceptionContext(Params, Ctx, ErrResult))
	{
		return ErrResult;
	}

	FString SenseType;
	if (!MonolithAI::RequireStringParam(Params, TEXT("sense_type"), SenseType, ErrResult))
	{
		return ErrResult;
	}

	UClass* ConfigClass = ResolveSenseConfigClass(SenseType);
	if (!ConfigClass)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown sense type: %s. Valid types: Sight, Hearing, Damage, Touch, Team, Prediction"), *SenseType));
	}

	// Access SensesConfig via reflection (it's protected)
	FArrayProperty* SensesConfigProp = CastField<FArrayProperty>(
		UAIPerceptionComponent::StaticClass()->FindPropertyByName(TEXT("SensesConfig")));
	if (!SensesConfigProp)
	{
		return FMonolithActionResult::Error(TEXT("Failed to find SensesConfig property via reflection"));
	}

	// Cast the array directly — we know the layout is TArray<TObjectPtr<UAISenseConfig>>
	void* ArrayPtr = SensesConfigProp->ContainerPtrToValuePtr<void>(Ctx.PerceptionComp);
	TArray<TObjectPtr<UAISenseConfig>>* SensesArray = static_cast<TArray<TObjectPtr<UAISenseConfig>>*>(ArrayPtr);
	if (!SensesArray)
	{
		return FMonolithActionResult::Error(TEXT("Failed to access SensesConfig array"));
	}

	// Find the matching sense config
	int32 RemovedIndex = INDEX_NONE;
	for (int32 i = 0; i < SensesArray->Num(); ++i)
	{
		UAISenseConfig* Sense = (*SensesArray)[i];
		if (Sense && Sense->GetClass()->IsChildOf(ConfigClass))
		{
			RemovedIndex = i;
			break;
		}
	}

	if (RemovedIndex == INDEX_NONE)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("No %s sense config found on this perception component"), *SenseType));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Remove Sense")));

	SensesArray->RemoveAt(RemovedIndex);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.BP);

	int32 Remaining = SensesArray->Num();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(Ctx.AssetPath,
		FString::Printf(TEXT("%s sense removed"), *SenseType));
	Result->SetStringField(TEXT("removed_sense"), SenseType);
	Result->SetNumberField(TEXT("remaining_senses"), Remaining);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  118. add_stimuli_source_component
// ============================================================

FMonolithActionResult FMonolithAIPerceptionActions::HandleAddStimuliSourceComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("asset_path"), AssetPath, ErrResult))
	{
		return ErrResult;
	}

	// Load any Actor Blueprint (not just AI controllers)
	UBlueprint* BP = Cast<UBlueprint>(FMonolithAssetUtils::LoadAssetByPath(UBlueprint::StaticClass(), AssetPath));
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	if (!BP->SimpleConstructionScript)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript — is it an Actor Blueprint?"));
	}

	// Check if already has one
	if (FindStimuliSourceNode(BP))
	{
		return FMonolithActionResult::Error(
			TEXT("This Blueprint already has a UAIPerceptionStimuliSourceComponent. Use configure_stimuli_source to modify it."));
	}

	// Parse sense types
	TArray<FString> SenseTypes = MonolithAI::ParseStringArray(Params, TEXT("register_as_source_for"));
	if (SenseTypes.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("register_as_source_for must contain at least one sense type"));
	}

	// Validate sense types
	TArray<TSubclassOf<UAISense>> SenseClasses;
	for (const FString& ST : SenseTypes)
	{
		TSubclassOf<UAISense> SC = ResolveSenseClass(ST);
		if (!SC)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Unknown sense type: %s. Valid: Sight, Hearing, Damage, Touch, Team, Prediction"), *ST));
		}
		SenseClasses.Add(SC);
	}

	bool bAutoRegister = true;
	if (Params->HasField(TEXT("auto_register")))
	{
		bAutoRegister = Params->GetBoolField(TEXT("auto_register"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add Stimuli Source Component")));

	const EBlueprintStatus SavedStatus = BP->Status;
	BP->Status = BS_BeingCreated;

	USCS_Node* NewNode = BP->SimpleConstructionScript->CreateNode(
		UAIPerceptionStimuliSourceComponent::StaticClass(), FName(TEXT("AIPerceptionStimuliSource")));
	if (!NewNode)
	{
		BP->Status = SavedStatus;
		return FMonolithActionResult::Error(TEXT("Failed to create stimuli source SCS node"));
	}

	BP->SimpleConstructionScript->AddNode(NewNode);

	// Configure the template via reflection (members are protected)
	UAIPerceptionStimuliSourceComponent* StimuliComp = Cast<UAIPerceptionStimuliSourceComponent>(NewNode->ComponentTemplate);
	if (StimuliComp)
	{
		// bAutoRegisterAsSource — bitfield, set via reflection
		if (FBoolProperty* AutoRegProp = CastField<FBoolProperty>(
			UAIPerceptionStimuliSourceComponent::StaticClass()->FindPropertyByName(TEXT("bAutoRegisterAsSource"))))
		{
			AutoRegProp->SetPropertyValue_InContainer(StimuliComp, bAutoRegister);
		}

		// RegisterAsSourceForSenses — TArray<TSubclassOf<UAISense>>
		FArrayProperty* SensesProp = CastField<FArrayProperty>(
			UAIPerceptionStimuliSourceComponent::StaticClass()->FindPropertyByName(TEXT("RegisterAsSourceForSenses")));
		if (SensesProp)
		{
			void* ArrayPtr = SensesProp->ContainerPtrToValuePtr<void>(StimuliComp);
			TArray<TSubclassOf<UAISense>>* SensesArray = static_cast<TArray<TSubclassOf<UAISense>>*>(ArrayPtr);
			if (SensesArray)
			{
				for (const TSubclassOf<UAISense>& SenseClass : SenseClasses)
				{
					SensesArray->AddUnique(SenseClass);
				}
			}
		}
	}

	BP->Status = SavedStatus;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(AssetPath, TEXT("StimuliSourceComponent added"));
	Result->SetStringField(TEXT("variable_name"), NewNode->GetVariableName().ToString());
	Result->SetBoolField(TEXT("auto_register"), bAutoRegister);

	TArray<TSharedPtr<FJsonValue>> RegisteredArr;
	for (const FString& ST : SenseTypes)
	{
		RegisteredArr.Add(MakeShared<FJsonValueString>(ST));
	}
	Result->SetArrayField(TEXT("registered_senses"), RegisteredArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  119. configure_stimuli_source
// ============================================================

FMonolithActionResult FMonolithAIPerceptionActions::HandleConfigureStimuliSource(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("asset_path"), AssetPath, ErrResult))
	{
		return ErrResult;
	}

	UBlueprint* BP = Cast<UBlueprint>(FMonolithAssetUtils::LoadAssetByPath(UBlueprint::StaticClass(), AssetPath));
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	USCS_Node* SourceNode = FindStimuliSourceNode(BP);
	if (!SourceNode)
	{
		return FMonolithActionResult::Error(
			TEXT("No UAIPerceptionStimuliSourceComponent found. Use add_stimuli_source_component first."));
	}

	UAIPerceptionStimuliSourceComponent* StimuliComp = Cast<UAIPerceptionStimuliSourceComponent>(SourceNode->ComponentTemplate);
	if (!StimuliComp)
	{
		return FMonolithActionResult::Error(TEXT("Failed to get stimuli source component template"));
	}

	TArray<FString> SenseTypes = MonolithAI::ParseStringArray(Params, TEXT("sense_types"));
	if (SenseTypes.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("sense_types must contain at least one sense type"));
	}

	// Validate all types first
	TArray<TSubclassOf<UAISense>> SenseClasses;
	for (const FString& ST : SenseTypes)
	{
		TSubclassOf<UAISense> SC = ResolveSenseClass(ST);
		if (!SC)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Unknown sense type: %s. Valid: Sight, Hearing, Damage, Touch, Team, Prediction"), *ST));
		}
		SenseClasses.Add(SC);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Configure Stimuli Source")));

	// Replace the registered stimuli list via reflection (protected member)
	FArrayProperty* SensesProp = CastField<FArrayProperty>(
		UAIPerceptionStimuliSourceComponent::StaticClass()->FindPropertyByName(TEXT("RegisterAsSourceForSenses")));
	if (SensesProp)
	{
		void* ArrayPtr = SensesProp->ContainerPtrToValuePtr<void>(StimuliComp);
		TArray<TSubclassOf<UAISense>>* SensesArray = static_cast<TArray<TSubclassOf<UAISense>>*>(ArrayPtr);
		if (SensesArray)
		{
			SensesArray->Empty();
			for (const TSubclassOf<UAISense>& SenseClass : SenseClasses)
			{
				SensesArray->Add(SenseClass);
			}
		}
	}

	// Auto-register flag via reflection
	bool bAutoRegisterVal = true;
	if (Params->HasField(TEXT("auto_register")))
	{
		bAutoRegisterVal = Params->GetBoolField(TEXT("auto_register"));
		if (FBoolProperty* AutoRegProp = CastField<FBoolProperty>(
			UAIPerceptionStimuliSourceComponent::StaticClass()->FindPropertyByName(TEXT("bAutoRegisterAsSource"))))
		{
			AutoRegProp->SetPropertyValue_InContainer(StimuliComp, bAutoRegisterVal);
		}
	}
	else
	{
		// Read current value
		if (FBoolProperty* AutoRegProp = CastField<FBoolProperty>(
			UAIPerceptionStimuliSourceComponent::StaticClass()->FindPropertyByName(TEXT("bAutoRegisterAsSource"))))
		{
			bAutoRegisterVal = AutoRegProp->GetPropertyValue_InContainer(StimuliComp);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(AssetPath, TEXT("Stimuli source configured"));
	Result->SetBoolField(TEXT("auto_register"), bAutoRegisterVal);

	TArray<TSharedPtr<FJsonValue>> RegisteredArr;
	for (const FString& ST : SenseTypes)
	{
		RegisteredArr.Add(MakeShared<FJsonValueString>(ST));
	}
	Result->SetArrayField(TEXT("registered_senses"), RegisteredArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  126. validate_perception_setup
// ============================================================

FMonolithActionResult FMonolithAIPerceptionActions::HandleValidatePerceptionSetup(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBlueprint* BP = MonolithAI::LoadAIControllerFromParams(Params, AssetPath, Error);
	if (!BP)
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);

	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<TSharedPtr<FJsonValue>> Recommendations;
	bool bValid = true;

	// Check: perception component exists
	USCS_Node* PerceptionNode = FindPerceptionNode(BP);
	if (!PerceptionNode)
	{
		Issues.Add(MakeShared<FJsonValueString>(TEXT("No UAIPerceptionComponent found")));
		Recommendations.Add(MakeShared<FJsonValueString>(TEXT("Call add_perception_component to add one")));
		bValid = false;
	}
	else
	{
		UAIPerceptionComponent* PerceptionComp = GetPerceptionTemplate(PerceptionNode);
		if (!PerceptionComp)
		{
			Issues.Add(MakeShared<FJsonValueString>(TEXT("Perception component template is null")));
			bValid = false;
		}
		else
		{
			// Count senses via iterator
			int32 SenseCount = 0;
			for (auto It = PerceptionComp->GetSensesConfigIterator(); It; ++It)
			{
				++SenseCount;
			}

			// Check: at least one sense configured
			if (SenseCount == 0)
			{
				Issues.Add(MakeShared<FJsonValueString>(TEXT("No senses configured on perception component")));
				Recommendations.Add(MakeShared<FJsonValueString>(
					TEXT("Add senses via configure_sight_sense, configure_hearing_sense, etc.")));
				bValid = false;
			}

			// Check: dominant sense assigned
			TSubclassOf<UAISense> DomSense = PerceptionComp->GetDominantSense();
			if (!DomSense)
			{
				Issues.Add(MakeShared<FJsonValueString>(TEXT("No dominant sense assigned")));
				Recommendations.Add(MakeShared<FJsonValueString>(
					TEXT("Set dominant_sense when calling add_perception_component, or re-add the component")));
			}

			// Check: each sense has affiliation filters set (for senses that use them)
			for (auto It = PerceptionComp->GetSensesConfigIterator(); It; ++It)
			{
				UAISenseConfig* Sense = *It;
				if (UAISenseConfig_Sight* Sight = Cast<UAISenseConfig_Sight>(Sense))
				{
					if (!Sight->DetectionByAffiliation.bDetectEnemies &&
						!Sight->DetectionByAffiliation.bDetectNeutrals &&
						!Sight->DetectionByAffiliation.bDetectFriendlies)
					{
						Issues.Add(MakeShared<FJsonValueString>(TEXT("Sight sense detects nothing — all affiliation flags are false")));
						bValid = false;
					}
				}
				else if (UAISenseConfig_Hearing* Hearing = Cast<UAISenseConfig_Hearing>(Sense))
				{
					if (!Hearing->DetectionByAffiliation.bDetectEnemies &&
						!Hearing->DetectionByAffiliation.bDetectNeutrals &&
						!Hearing->DetectionByAffiliation.bDetectFriendlies)
					{
						Issues.Add(MakeShared<FJsonValueString>(TEXT("Hearing sense detects nothing — all affiliation flags are false")));
						bValid = false;
					}
				}
			}

			// Check: sight lose_radius > sight_radius
			if (UAISenseConfig_Sight* Sight = FindSenseConfig<UAISenseConfig_Sight>(PerceptionComp))
			{
				if (Sight->LoseSightRadius < Sight->SightRadius)
				{
					Issues.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("Sight LoseSightRadius (%.0f) < SightRadius (%.0f) — will cause flickering detections"),
							Sight->LoseSightRadius, Sight->SightRadius)));
				}
			}

			Result->SetNumberField(TEXT("sense_count"), SenseCount);
		}
	}

	// Check: team ID set (useful for affiliation detection)
	if (BP->GeneratedClass)
	{
		AAIController* CDO = Cast<AAIController>(BP->GeneratedClass->GetDefaultObject());
		if (CDO)
		{
			uint8 TeamId = CDO->GetGenericTeamId().GetId();
			Result->SetNumberField(TEXT("team_id"), TeamId);
			if (TeamId == 255)
			{
				Recommendations.Add(MakeShared<FJsonValueString>(
					TEXT("Team ID is 255 (NoTeam). Set via set_ai_team for affiliation-based detection.")));
			}
		}
	}

	Result->SetBoolField(TEXT("valid"), bValid);
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetArrayField(TEXT("recommendations"), Recommendations);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  218. get_ai_system_config
// ============================================================

FMonolithActionResult FMonolithAIPerceptionActions::HandleGetAISystemConfig(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithAI::GetPIEWorld();
	if (!World)
	{
		// Try editor world
		World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (World)
	{
		UAIPerceptionSystem* PerceptionSystem = UAIPerceptionSystem::GetCurrent(World);
		if (PerceptionSystem)
		{
			Result->SetBoolField(TEXT("perception_system_active"), true);
		}
		else
		{
			Result->SetBoolField(TEXT("perception_system_active"), false);
		}
	}

	// Read from UAISystem CDO (project-level config)
	UClass* AISystemClass = FindFirstObject<UClass>(TEXT("UAISystem"), EFindFirstObjectOptions::NativeFirst);
	if (AISystemClass)
	{
		UObject* CDO = AISystemClass->GetDefaultObject();
		if (CDO)
		{
			// Read common properties via reflection
			auto ReadBoolProp = [&](const FString& PropName)
			{
				if (FBoolProperty* Prop = CastField<FBoolProperty>(AISystemClass->FindPropertyByName(*PropName)))
				{
					Result->SetBoolField(PropName, Prop->GetPropertyValue_InContainer(CDO));
				}
			};
			auto ReadFloatProp = [&](const FString& PropName)
			{
				if (FFloatProperty* Prop = CastField<FFloatProperty>(AISystemClass->FindPropertyByName(*PropName)))
				{
					Result->SetNumberField(PropName, Prop->GetPropertyValue_InContainer(CDO));
				}
				else if (FDoubleProperty* DProp = CastField<FDoubleProperty>(AISystemClass->FindPropertyByName(*PropName)))
				{
					Result->SetNumberField(PropName, DProp->GetPropertyValue_InContainer(CDO));
				}
			};

			ReadBoolProp(TEXT("bAllowControllersAsStimulationSources"));
			ReadFloatProp(TEXT("AcceptanceRadius"));
			ReadFloatProp(TEXT("PathfollowingRegularPathPointAcceptanceRadius"));
			ReadFloatProp(TEXT("PathfollowingNavLinkAcceptanceRadius"));
		}
	}

	// INI-based AI system configuration
	FString AISystemClassName;
	if (GConfig)
	{
		GConfig->GetString(TEXT("/Script/AIModule.AISystem"), TEXT("PerceptionSystemClassName"), AISystemClassName, GEngineIni);
		if (!AISystemClassName.IsEmpty())
		{
			Result->SetStringField(TEXT("perception_system_class"), AISystemClassName);
		}
	}

	Result->SetStringField(TEXT("message"), TEXT("UAISystem global settings read from CDO and project INI"));
	return FMonolithActionResult::Success(Result);
}
