#include "MonolithAIRuntimeActions.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AIPerceptionSystem.h"
#include "Perception/AISense.h"
#include "Perception/AISense_Sight.h"
#include "Perception/AISense_Hearing.h"
#include "Perception/AISense_Damage.h"
#include "Perception/AISense_Touch.h"
#include "Perception/AISense_Team.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_Point.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_ActorBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "Engine/Engine.h"
#include "Editor.h"
#include "EngineUtils.h"

#if WITH_STATETREE
#include "StateTree.h"
#include "StateTreeReference.h"
#include "Components/StateTreeComponent.h"
#endif

#if WITH_SMARTOBJECTS
#include "SmartObjectSubsystem.h"
#include "SmartObjectDefinition.h"
#include "SmartObjectComponent.h"
#include "SmartObjectRequestTypes.h"
#include "StructUtils/InstancedStruct.h"
#endif

// ============================================================
//  PIE guard macro — every handler must use this
// ============================================================

#define PIE_GUARD() \
	if (!GEditor || !GEditor->IsPlayingSessionInEditor()) \
	{ \
		return FMonolithActionResult::Error(TEXT("This action requires Play-In-Editor to be running")); \
	}

// ============================================================
//  Shared helpers
// ============================================================

namespace
{
	/** Resolve actor param → AActor* in PIE. Accepts label, name, or path. */
	AActor* ResolveActorParam(const TSharedPtr<FJsonObject>& Params, const FString& ParamName, FMonolithActionResult& OutError)
	{
		FString ActorId = Params->GetStringField(ParamName);
		if (ActorId.IsEmpty())
		{
			OutError = FMonolithActionResult::Error(FString::Printf(TEXT("Missing required param '%s'"), *ParamName));
			return nullptr;
		}

		AActor* Actor = MonolithAI::FindActorInPIE(ActorId);
		if (!Actor)
		{
			OutError = FMonolithActionResult::Error(FString::Printf(TEXT("Actor '%s' not found in PIE world"), *ActorId));
			return nullptr;
		}
		return Actor;
	}

	/** Get the AIController from a pawn or the actor itself if it IS a controller. */
	AAIController* GetAIControllerFromActor(AActor* Actor)
	{
		if (AAIController* AIC = Cast<AAIController>(Actor))
		{
			return AIC;
		}
		if (APawn* Pawn = Cast<APawn>(Actor))
		{
			return Cast<AAIController>(Pawn->GetController());
		}
		return nullptr;
	}

	/** Get AIController from actor param, with full error reporting. */
	AAIController* ResolveAIController(const TSharedPtr<FJsonObject>& Params, FMonolithActionResult& OutError)
	{
		AActor* Actor = ResolveActorParam(Params, TEXT("actor"), OutError);
		if (!Actor) return nullptr;

		AAIController* AIC = GetAIControllerFromActor(Actor);
		if (!AIC)
		{
			OutError = FMonolithActionResult::Error(FString::Printf(
				TEXT("Actor '%s' has no AI controller"), *Actor->GetActorLabel()));
			return nullptr;
		}
		return AIC;
	}

	/** Get BlackboardComponent from actor param. */
	UBlackboardComponent* ResolveBBComponent(const TSharedPtr<FJsonObject>& Params, FMonolithActionResult& OutError)
	{
		AAIController* AIC = ResolveAIController(Params, OutError);
		if (!AIC) return nullptr;

		UBlackboardComponent* BB = AIC->GetBlackboardComponent();
		if (!BB)
		{
			OutError = FMonolithActionResult::Error(TEXT("AI controller has no Blackboard component"));
			return nullptr;
		}
		return BB;
	}

	/** Get BehaviorTreeComponent from actor param. */
	UBehaviorTreeComponent* ResolveBTComponent(const TSharedPtr<FJsonObject>& Params, FMonolithActionResult& OutError)
	{
		AAIController* AIC = ResolveAIController(Params, OutError);
		if (!AIC) return nullptr;

		UBehaviorTreeComponent* BTComp = Cast<UBehaviorTreeComponent>(AIC->BrainComponent);
		if (!BTComp)
		{
			OutError = FMonolithActionResult::Error(TEXT("AI controller has no BehaviorTree component"));
			return nullptr;
		}
		return BTComp;
	}

	/** Read a BB key value as a JSON-friendly string/value, given the key ID and BB component. */
	void SerializeBBValue(UBlackboardComponent* BB, FBlackboard::FKey KeyID, const FString& KeyTypeName, TSharedPtr<FJsonObject>& OutObj)
	{
		if (KeyTypeName.Contains(TEXT("Float")))
		{
			OutObj->SetNumberField(TEXT("value"), BB->GetValueAsFloat(BB->GetKeyName(KeyID)));
			OutObj->SetStringField(TEXT("type"), TEXT("float"));
		}
		else if (KeyTypeName.Contains(TEXT("Int")))
		{
			OutObj->SetNumberField(TEXT("value"), BB->GetValueAsInt(BB->GetKeyName(KeyID)));
			OutObj->SetStringField(TEXT("type"), TEXT("int"));
		}
		else if (KeyTypeName.Contains(TEXT("Bool")))
		{
			OutObj->SetBoolField(TEXT("value"), BB->GetValueAsBool(BB->GetKeyName(KeyID)));
			OutObj->SetStringField(TEXT("type"), TEXT("bool"));
		}
		else if (KeyTypeName.Contains(TEXT("String")))
		{
			OutObj->SetStringField(TEXT("value"), BB->GetValueAsString(BB->GetKeyName(KeyID)));
			OutObj->SetStringField(TEXT("type"), TEXT("string"));
		}
		else if (KeyTypeName.Contains(TEXT("Name")))
		{
			OutObj->SetStringField(TEXT("value"), BB->GetValueAsName(BB->GetKeyName(KeyID)).ToString());
			OutObj->SetStringField(TEXT("type"), TEXT("name"));
		}
		else if (KeyTypeName.Contains(TEXT("Vector")))
		{
			FVector V = BB->GetValueAsVector(BB->GetKeyName(KeyID));
			OutObj->SetStringField(TEXT("value"), FString::Printf(TEXT("(%.2f, %.2f, %.2f)"), V.X, V.Y, V.Z));
			OutObj->SetStringField(TEXT("type"), TEXT("vector"));
		}
		else if (KeyTypeName.Contains(TEXT("Rotator")))
		{
			FRotator R = BB->GetValueAsRotator(BB->GetKeyName(KeyID));
			OutObj->SetStringField(TEXT("value"), FString::Printf(TEXT("(P=%.2f, Y=%.2f, R=%.2f)"), R.Pitch, R.Yaw, R.Roll));
			OutObj->SetStringField(TEXT("type"), TEXT("rotator"));
		}
		else if (KeyTypeName.Contains(TEXT("Object")))
		{
			UObject* Obj = BB->GetValueAsObject(BB->GetKeyName(KeyID));
			if (Obj)
			{
				OutObj->SetStringField(TEXT("value"), Obj->GetName());
				OutObj->SetStringField(TEXT("value_path"), Obj->GetPathName());
				if (AActor* A = Cast<AActor>(Obj))
				{
					FVector Loc = A->GetActorLocation();
					OutObj->SetStringField(TEXT("value_location"), FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), Loc.X, Loc.Y, Loc.Z));
				}
			}
			else
			{
				OutObj->SetStringField(TEXT("value"), TEXT("None"));
			}
			OutObj->SetStringField(TEXT("type"), TEXT("object"));
		}
		else if (KeyTypeName.Contains(TEXT("Enum")))
		{
			OutObj->SetNumberField(TEXT("value"), static_cast<double>(BB->GetValueAsEnum(BB->GetKeyName(KeyID))));
			OutObj->SetStringField(TEXT("type"), TEXT("enum"));
		}
		else if (KeyTypeName.Contains(TEXT("Class")))
		{
			UClass* C = BB->GetValueAsClass(BB->GetKeyName(KeyID));
			OutObj->SetStringField(TEXT("value"), C ? C->GetPathName() : TEXT("None"));
			OutObj->SetStringField(TEXT("type"), TEXT("class"));
		}
		else
		{
			OutObj->SetStringField(TEXT("value"), BB->GetValueAsString(BB->GetKeyName(KeyID)));
			OutObj->SetStringField(TEXT("type"), TEXT("unknown"));
		}
	}

	/** Make a quick actor summary JSON object. */
	TSharedPtr<FJsonObject> MakeActorSummary(AActor* Actor)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Actor) return Obj;
		Obj->SetStringField(TEXT("name"), Actor->GetName());
		Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
		FVector Loc = Actor->GetActorLocation();
		Obj->SetStringField(TEXT("location"), FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), Loc.X, Loc.Y, Loc.Z));
		return Obj;
	}
}

// ============================================================
//  Registration
// ============================================================

void FMonolithAIRuntimeActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 167. runtime_get_bb_value
	Registry.RegisterAction(TEXT("ai"), TEXT("runtime_get_bb_value"),
		TEXT("Read a Blackboard value from a running AI's blackboard component (PIE only)"),
		FMonolithActionHandler::CreateStatic(&HandleRuntimeGetBBValue),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in PIE"))
			.Required(TEXT("key_name"), TEXT("string"), TEXT("Blackboard key name to read"))
			.Build());

	// 168. runtime_set_bb_value
	Registry.RegisterAction(TEXT("ai"), TEXT("runtime_set_bb_value"),
		TEXT("Write a Blackboard value on a running AI (PIE only)"),
		FMonolithActionHandler::CreateStatic(&HandleRuntimeSetBBValue),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in PIE"))
			.Required(TEXT("key_name"), TEXT("string"), TEXT("Blackboard key name to write"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Value to set (auto-converted based on key type)"))
			.Build());

	// 169. runtime_clear_bb_value
	Registry.RegisterAction(TEXT("ai"), TEXT("runtime_clear_bb_value"),
		TEXT("Clear a Blackboard key on a running AI (PIE only)"),
		FMonolithActionHandler::CreateStatic(&HandleRuntimeClearBBValue),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in PIE"))
			.Required(TEXT("key_name"), TEXT("string"), TEXT("Blackboard key name to clear"))
			.Build());

	// 170. runtime_get_bt_state
	Registry.RegisterAction(TEXT("ai"), TEXT("runtime_get_bt_state"),
		TEXT("Get BehaviorTree runtime state: active node, tree status, pending aborts (PIE only)"),
		FMonolithActionHandler::CreateStatic(&HandleRuntimeGetBTState),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in PIE"))
			.Build());

	// 171. runtime_start_bt
	Registry.RegisterAction(TEXT("ai"), TEXT("runtime_start_bt"),
		TEXT("Start or restart a BehaviorTree on an AI controller (PIE only)"),
		FMonolithActionHandler::CreateStatic(&HandleRuntimeStartBT),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in PIE"))
			.Optional(TEXT("bt_path"), TEXT("string"), TEXT("BehaviorTree asset path (uses controller default if omitted)"))
			.Optional(TEXT("run_mode"), TEXT("string"), TEXT("'looped' (default) or 'single_run'"))
			.Build());

	// 172. runtime_stop_bt
	Registry.RegisterAction(TEXT("ai"), TEXT("runtime_stop_bt"),
		TEXT("Stop BehaviorTree execution on an AI controller (PIE only)"),
		FMonolithActionHandler::CreateStatic(&HandleRuntimeStopBT),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in PIE"))
			.Build());

	// 173. runtime_get_bt_execution_path
	Registry.RegisterAction(TEXT("ai"), TEXT("runtime_get_bt_execution_path"),
		TEXT("Snapshot of BT execution: active branch, current task, decorator states (PIE only)"),
		FMonolithActionHandler::CreateStatic(&HandleRuntimeGetBTExecutionPath),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in PIE"))
			.Build());

	// 174. runtime_get_perceived_actors
	Registry.RegisterAction(TEXT("ai"), TEXT("runtime_get_perceived_actors"),
		TEXT("List all actors currently perceived by an AI's perception component (PIE only)"),
		FMonolithActionHandler::CreateStatic(&HandleRuntimeGetPerceivedActors),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Observer actor label, name, or path in PIE"))
			.Optional(TEXT("sense_filter"), TEXT("string"), TEXT("Filter by sense type: Sight, Hearing, Damage, Touch, Team"))
			.Build());

	// 175. runtime_check_perception
	Registry.RegisterAction(TEXT("ai"), TEXT("runtime_check_perception"),
		TEXT("Check if a target actor is perceived by an observer, and by which senses (PIE only)"),
		FMonolithActionHandler::CreateStatic(&HandleRuntimeCheckPerception),
		FParamSchemaBuilder()
			.Required(TEXT("observer_actor"), TEXT("string"), TEXT("Observer actor label, name, or path"))
			.Required(TEXT("target_actor"), TEXT("string"), TEXT("Target actor label, name, or path"))
			.Build());

	// 176. runtime_report_noise
	Registry.RegisterAction(TEXT("ai"), TEXT("runtime_report_noise"),
		TEXT("Fire a noise event at a location for AI hearing (PIE only)"),
		FMonolithActionHandler::CreateStatic(&HandleRuntimeReportNoise),
		FParamSchemaBuilder()
			.Required(TEXT("location"), TEXT("object"), TEXT("World location as {x,y,z} or 'X,Y,Z'"))
			.Optional(TEXT("loudness"), TEXT("number"), TEXT("Loudness multiplier (default 1.0)"))
			.Optional(TEXT("instigator"), TEXT("string"), TEXT("Actor that caused the noise (label/name/path)"))
			.Optional(TEXT("tag"), TEXT("string"), TEXT("Noise tag for filtering"))
			.Build());

	// 177. runtime_get_st_active_states
	Registry.RegisterAction(TEXT("ai"), TEXT("runtime_get_st_active_states"),
		TEXT("Get active StateTree states and task statuses (PIE only)"),
		FMonolithActionHandler::CreateStatic(&HandleRuntimeGetSTActiveStates),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in PIE"))
			.Build());

	// 178. runtime_send_st_event
	Registry.RegisterAction(TEXT("ai"), TEXT("runtime_send_st_event"),
		TEXT("Send an FStateTreeEvent to a running StateTree component (PIE only)"),
		FMonolithActionHandler::CreateStatic(&HandleRuntimeSendSTEvent),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in PIE"))
			.Required(TEXT("event_tag"), TEXT("string"), TEXT("Gameplay tag for the event (e.g. 'AI.Event.Alert')"))
			.Build());

	// 179. runtime_find_smart_objects
	Registry.RegisterAction(TEXT("ai"), TEXT("runtime_find_smart_objects"),
		TEXT("Find available Smart Object slots near an actor (PIE only)"),
		FMonolithActionHandler::CreateStatic(&HandleRuntimeFindSmartObjects),
		FParamSchemaBuilder()
			.Required(TEXT("querier_actor"), TEXT("string"), TEXT("Actor performing the query (label/name/path)"))
			.Optional(TEXT("activity_tags"), TEXT("string"), TEXT("Comma-separated gameplay tags to filter by activity"))
			.Optional(TEXT("radius"), TEXT("number"), TEXT("Search radius in units (default 2000)"))
			.Build());

	// 180. runtime_run_eqs_query
	Registry.RegisterAction(TEXT("ai"), TEXT("runtime_run_eqs_query"),
		TEXT("Run an EQS query synchronously and return scored results (PIE only)"),
		FMonolithActionHandler::CreateStatic(&HandleRuntimeRunEQSQuery),
		FParamSchemaBuilder()
			.Required(TEXT("querier_actor"), TEXT("string"), TEXT("Actor to use as querier (label/name/path)"))
			.Required(TEXT("query_path"), TEXT("string"), TEXT("EQS query asset path (e.g. /Game/AI/EQS/EQS_FindCover)"))
			.Optional(TEXT("max_results"), TEXT("number"), TEXT("Max results to return (default 10)"))
			.Build());
}

// ============================================================
//  167. runtime_get_bb_value
// ============================================================

FMonolithActionResult FMonolithAIRuntimeActions::HandleRuntimeGetBBValue(const TSharedPtr<FJsonObject>& Params)
{
	PIE_GUARD();

	FMonolithActionResult Err;
	UBlackboardComponent* BB = ResolveBBComponent(Params, Err);
	if (!BB) return Err;

	FString KeyName = Params->GetStringField(TEXT("key_name"));
	if (KeyName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'key_name'"));
	}

	FBlackboard::FKey KeyID = BB->GetKeyID(FName(*KeyName));
	if (KeyID == FBlackboard::InvalidKey)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blackboard key '%s' not found"), *KeyName));
	}

	const FBlackboardEntry* Entry = nullptr;
	if (BB->GetBlackboardAsset())
	{
		for (const FBlackboardEntry& E : BB->GetBlackboardAsset()->Keys)
		{
			if (E.EntryName == FName(*KeyName))
			{
				Entry = &E;
				break;
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("key_name"), KeyName);

	FString KeyTypeName = Entry && Entry->KeyType ? Entry->KeyType->GetClass()->GetName() : TEXT("Unknown");
	SerializeBBValue(BB, KeyID, KeyTypeName, Result);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  168. runtime_set_bb_value
// ============================================================

FMonolithActionResult FMonolithAIRuntimeActions::HandleRuntimeSetBBValue(const TSharedPtr<FJsonObject>& Params)
{
	PIE_GUARD();

	FMonolithActionResult Err;
	UBlackboardComponent* BB = ResolveBBComponent(Params, Err);
	if (!BB) return Err;

	FString KeyName = Params->GetStringField(TEXT("key_name"));
	if (KeyName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'key_name'"));
	}

	FBlackboard::FKey KeyID = BB->GetKeyID(FName(*KeyName));
	if (KeyID == FBlackboard::InvalidKey)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blackboard key '%s' not found"), *KeyName));
	}

	// Determine the key type from the BB asset
	const FBlackboardEntry* Entry = nullptr;
	if (BB->GetBlackboardAsset())
	{
		for (const FBlackboardEntry& E : BB->GetBlackboardAsset()->Keys)
		{
			if (E.EntryName == FName(*KeyName))
			{
				Entry = &E;
				break;
			}
		}
	}

	if (!Entry || !Entry->KeyType)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Cannot determine type for BB key '%s'"), *KeyName));
	}

	FString KeyTypeName = Entry->KeyType->GetClass()->GetName();
	FName KeyFName(*KeyName);

	// The value param could be string, number, or bool in JSON — read as string for parsing
	FString ValueStr;
	if (Params->HasTypedField<EJson::String>(TEXT("value")))
	{
		ValueStr = Params->GetStringField(TEXT("value"));
	}
	else if (Params->HasTypedField<EJson::Number>(TEXT("value")))
	{
		ValueStr = FString::SanitizeFloat(Params->GetNumberField(TEXT("value")));
	}
	else if (Params->HasTypedField<EJson::Boolean>(TEXT("value")))
	{
		ValueStr = Params->GetBoolField(TEXT("value")) ? TEXT("true") : TEXT("false");
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'value'"));
	}

	if (KeyTypeName.Contains(TEXT("Float")))
	{
		BB->SetValueAsFloat(KeyFName, FCString::Atof(*ValueStr));
	}
	else if (KeyTypeName.Contains(TEXT("Int")))
	{
		BB->SetValueAsInt(KeyFName, FCString::Atoi(*ValueStr));
	}
	else if (KeyTypeName.Contains(TEXT("Bool")))
	{
		bool bVal = ValueStr.Equals(TEXT("true"), ESearchCase::IgnoreCase) || ValueStr == TEXT("1");
		BB->SetValueAsBool(KeyFName, bVal);
	}
	else if (KeyTypeName.Contains(TEXT("String")))
	{
		BB->SetValueAsString(KeyFName, ValueStr);
	}
	else if (KeyTypeName.Contains(TEXT("Name")))
	{
		BB->SetValueAsName(KeyFName, FName(*ValueStr));
	}
	else if (KeyTypeName.Contains(TEXT("Vector")))
	{
		FVector V;
		TArray<FString> Parts;
		ValueStr.Replace(TEXT("("), TEXT("")).Replace(TEXT(")"), TEXT("")).ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() >= 3)
		{
			V.X = FCString::Atof(*Parts[0].TrimStartAndEnd());
			V.Y = FCString::Atof(*Parts[1].TrimStartAndEnd());
			V.Z = FCString::Atof(*Parts[2].TrimStartAndEnd());
			BB->SetValueAsVector(KeyFName, V);
		}
		else
		{
			return FMonolithActionResult::Error(TEXT("Vector value must be 'X,Y,Z' format"));
		}
	}
	else if (KeyTypeName.Contains(TEXT("Rotator")))
	{
		TArray<FString> Parts;
		ValueStr.Replace(TEXT("("), TEXT("")).Replace(TEXT(")"), TEXT("")).ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() >= 3)
		{
			FRotator R;
			R.Pitch = FCString::Atof(*Parts[0].TrimStartAndEnd());
			R.Yaw = FCString::Atof(*Parts[1].TrimStartAndEnd());
			R.Roll = FCString::Atof(*Parts[2].TrimStartAndEnd());
			BB->SetValueAsRotator(KeyFName, R);
		}
		else
		{
			return FMonolithActionResult::Error(TEXT("Rotator value must be 'Pitch,Yaw,Roll' format"));
		}
	}
	else if (KeyTypeName.Contains(TEXT("Object")))
	{
		// Try to find the object in PIE world by name/label
		AActor* Obj = MonolithAI::FindActorInPIE(ValueStr);
		if (!Obj)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Cannot find actor '%s' in PIE for Object BB key"), *ValueStr));
		}
		BB->SetValueAsObject(KeyFName, Obj);
	}
	else if (KeyTypeName.Contains(TEXT("Enum")))
	{
		BB->SetValueAsEnum(KeyFName, static_cast<uint8>(FCString::Atoi(*ValueStr)));
	}
	else if (KeyTypeName.Contains(TEXT("Class")))
	{
		UClass* C = FindFirstObject<UClass>(*ValueStr, EFindFirstObjectOptions::EnsureIfAmbiguous);
		if (!C)
		{
			C = LoadObject<UClass>(nullptr, *ValueStr);
		}
		if (!C)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Class not found: '%s'"), *ValueStr));
		}
		BB->SetValueAsClass(KeyFName, C);
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unsupported BB key type: %s"), *KeyTypeName));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("key_name"), KeyName);
	Result->SetStringField(TEXT("value_set"), ValueStr);
	Result->SetStringField(TEXT("key_type"), KeyTypeName);
	Result->SetStringField(TEXT("message"), TEXT("Blackboard value updated"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  169. runtime_clear_bb_value
// ============================================================

FMonolithActionResult FMonolithAIRuntimeActions::HandleRuntimeClearBBValue(const TSharedPtr<FJsonObject>& Params)
{
	PIE_GUARD();

	FMonolithActionResult Err;
	UBlackboardComponent* BB = ResolveBBComponent(Params, Err);
	if (!BB) return Err;

	FString KeyName = Params->GetStringField(TEXT("key_name"));
	if (KeyName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'key_name'"));
	}

	FBlackboard::FKey KeyID = BB->GetKeyID(FName(*KeyName));
	if (KeyID == FBlackboard::InvalidKey)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blackboard key '%s' not found"), *KeyName));
	}

	BB->ClearValue(KeyID);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("key_name"), KeyName);
	Result->SetStringField(TEXT("message"), TEXT("Blackboard key cleared"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  170. runtime_get_bt_state
// ============================================================

FMonolithActionResult FMonolithAIRuntimeActions::HandleRuntimeGetBTState(const TSharedPtr<FJsonObject>& Params)
{
	PIE_GUARD();

	FMonolithActionResult Err;
	UBehaviorTreeComponent* BTComp = ResolveBTComponent(Params, Err);
	if (!BTComp) return Err;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Tree status
	Result->SetBoolField(TEXT("is_running"), BTComp->IsRunning());
	Result->SetBoolField(TEXT("is_paused"), BTComp->IsPaused());

	// Active node
	const UBTNode* ActiveNode = BTComp->GetActiveNode();
	if (ActiveNode)
	{
		Result->SetStringField(TEXT("active_node_name"), ActiveNode->GetNodeName());
		Result->SetStringField(TEXT("active_node_class"), ActiveNode->GetClass()->GetName());
		Result->SetNumberField(TEXT("active_node_index"), ActiveNode->GetExecutionIndex());
	}
	else
	{
		Result->SetStringField(TEXT("active_node_name"), TEXT("None"));
	}

	// Tree asset
	const UBehaviorTree* TreeAsset = BTComp->GetCurrentTree();
	if (TreeAsset)
	{
		Result->SetStringField(TEXT("tree_asset"), TreeAsset->GetPathName());
		Result->SetStringField(TEXT("tree_name"), TreeAsset->GetName());
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  171. runtime_start_bt
// ============================================================

FMonolithActionResult FMonolithAIRuntimeActions::HandleRuntimeStartBT(const TSharedPtr<FJsonObject>& Params)
{
	PIE_GUARD();

	FMonolithActionResult Err;
	AAIController* AIC = ResolveAIController(Params, Err);
	if (!AIC) return Err;

	UBehaviorTree* BT = nullptr;
	FString BTPath = Params->GetStringField(TEXT("bt_path"));
	if (!BTPath.IsEmpty())
	{
		BT = Cast<UBehaviorTree>(MonolithAI::ResolveAsset(UBehaviorTree::StaticClass(), BTPath));
		if (!BT)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("BehaviorTree not found: %s"), *BTPath));
		}
	}

	FString RunMode = Params->GetStringField(TEXT("run_mode")).ToLower();
	EBTExecutionMode::Type Mode = EBTExecutionMode::Looped;
	if (RunMode == TEXT("single_run"))
	{
		Mode = EBTExecutionMode::SingleRun;
	}

	// If no BT path given, try the tree currently on the component
	if (!BT)
	{
		UBehaviorTreeComponent* BTComp = Cast<UBehaviorTreeComponent>(AIC->BrainComponent);
		if (BTComp)
		{
			BT = const_cast<UBehaviorTree*>(BTComp->GetCurrentTree());
		}
	}

	if (!BT)
	{
		return FMonolithActionResult::Error(TEXT("No BehaviorTree specified and none currently assigned"));
	}

	bool bSuccess = AIC->RunBehaviorTree(BT);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("started"), bSuccess);
	Result->SetStringField(TEXT("tree"), BT->GetName());
	Result->SetStringField(TEXT("run_mode"), Mode == EBTExecutionMode::Looped ? TEXT("looped") : TEXT("single_run"));
	if (!bSuccess)
	{
		Result->SetStringField(TEXT("message"), TEXT("RunBehaviorTree returned false — check BB compatibility"));
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  172. runtime_stop_bt
// ============================================================

FMonolithActionResult FMonolithAIRuntimeActions::HandleRuntimeStopBT(const TSharedPtr<FJsonObject>& Params)
{
	PIE_GUARD();

	FMonolithActionResult Err;
	UBehaviorTreeComponent* BTComp = ResolveBTComponent(Params, Err);
	if (!BTComp) return Err;

	bool bWasRunning = BTComp->IsRunning();
	BTComp->StopTree(EBTStopMode::Safe);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("was_running"), bWasRunning);
	Result->SetStringField(TEXT("message"), bWasRunning ? TEXT("BehaviorTree stopped") : TEXT("BehaviorTree was not running"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  173. runtime_get_bt_execution_path
// ============================================================

FMonolithActionResult FMonolithAIRuntimeActions::HandleRuntimeGetBTExecutionPath(const TSharedPtr<FJsonObject>& Params)
{
	PIE_GUARD();

	FMonolithActionResult Err;
	UBehaviorTreeComponent* BTComp = ResolveBTComponent(Params, Err);
	if (!BTComp) return Err;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("is_running"), BTComp->IsRunning());

	const UBehaviorTree* TreeAsset = BTComp->GetCurrentTree();
	if (TreeAsset)
	{
		Result->SetStringField(TEXT("tree_name"), TreeAsset->GetName());
	}

	// Active node info
	const UBTNode* ActiveNode = BTComp->GetActiveNode();
	if (ActiveNode)
	{
		TSharedPtr<FJsonObject> ActiveObj = MakeShared<FJsonObject>();
		ActiveObj->SetStringField(TEXT("name"), ActiveNode->GetNodeName());
		ActiveObj->SetStringField(TEXT("class"), ActiveNode->GetClass()->GetName());
		ActiveObj->SetNumberField(TEXT("execution_index"), ActiveNode->GetExecutionIndex());
		ActiveObj->SetNumberField(TEXT("tree_depth"), ActiveNode->GetTreeDepth());

		// Check if it's a task
		if (const UBTTaskNode* Task = Cast<UBTTaskNode>(ActiveNode))
		{
			ActiveObj->SetStringField(TEXT("node_type"), TEXT("task"));
		}
		else if (const UBTCompositeNode* Composite = Cast<UBTCompositeNode>(ActiveNode))
		{
			ActiveObj->SetStringField(TEXT("node_type"), TEXT("composite"));
			ActiveObj->SetStringField(TEXT("composite_class"), Composite->GetClass()->GetName());
		}
		else
		{
			ActiveObj->SetStringField(TEXT("node_type"), TEXT("other"));
		}

		Result->SetObjectField(TEXT("active_node"), ActiveObj);
	}

	// Walk from active node up through parents to build the execution path
	if (ActiveNode)
	{
		TArray<TSharedPtr<FJsonValue>> PathArr;
		TArray<const UBTNode*> Chain;

		// Build chain from active node to root
		const UBTNode* Node = ActiveNode;
		int32 SafetyLimit = 50;
		while (Node && SafetyLimit-- > 0)
		{
			Chain.Add(Node);
			Node = Node->GetParentNode();
		}

		// Reverse so root is first
		Algo::Reverse(Chain);

		for (const UBTNode* PathNode : Chain)
		{
			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("name"), PathNode->GetNodeName());
			NodeObj->SetStringField(TEXT("class"), PathNode->GetClass()->GetName());
			NodeObj->SetNumberField(TEXT("execution_index"), PathNode->GetExecutionIndex());
			NodeObj->SetNumberField(TEXT("tree_depth"), PathNode->GetTreeDepth());

			if (Cast<UBTTaskNode>(PathNode))
			{
				NodeObj->SetStringField(TEXT("node_type"), TEXT("task"));
			}
			else if (const UBTCompositeNode* Composite = Cast<UBTCompositeNode>(PathNode))
			{
				NodeObj->SetStringField(TEXT("node_type"), TEXT("composite"));
				NodeObj->SetNumberField(TEXT("child_count"), Composite->Children.Num());

				// Include decorators from children of this composite
				TArray<TSharedPtr<FJsonValue>> DecArr;
				for (const FBTCompositeChild& Child : Composite->Children)
				{
					for (const TObjectPtr<UBTDecorator>& Dec : Child.Decorators)
					{
						if (Dec)
						{
							TSharedPtr<FJsonObject> DecObj = MakeShared<FJsonObject>();
							DecObj->SetStringField(TEXT("name"), Dec->GetNodeName());
							DecObj->SetStringField(TEXT("class"), Dec->GetClass()->GetName());
							DecArr.Add(MakeShared<FJsonValueObject>(DecObj));
						}
					}
				}
				if (DecArr.Num() > 0)
				{
					NodeObj->SetArrayField(TEXT("decorators"), DecArr);
				}
			}
			else
			{
				NodeObj->SetStringField(TEXT("node_type"), TEXT("other"));
			}

			PathArr.Add(MakeShared<FJsonValueObject>(NodeObj));
		}

		Result->SetArrayField(TEXT("execution_path"), PathArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  174. runtime_get_perceived_actors
// ============================================================

FMonolithActionResult FMonolithAIRuntimeActions::HandleRuntimeGetPerceivedActors(const TSharedPtr<FJsonObject>& Params)
{
	PIE_GUARD();

	FMonolithActionResult Err;
	AAIController* AIC = ResolveAIController(Params, Err);
	if (!AIC) return Err;

	UAIPerceptionComponent* PerComp = AIC->GetPerceptionComponent();
	if (!PerComp)
	{
		return FMonolithActionResult::Error(TEXT("AI controller has no Perception component"));
	}

	FString SenseFilter = Params->GetStringField(TEXT("sense_filter"));
	TSubclassOf<UAISense> SenseClass = nullptr;
	if (!SenseFilter.IsEmpty())
	{
		if (SenseFilter.Equals(TEXT("Sight"), ESearchCase::IgnoreCase))
			SenseClass = UAISense_Sight::StaticClass();
		else if (SenseFilter.Equals(TEXT("Hearing"), ESearchCase::IgnoreCase))
			SenseClass = UAISense_Hearing::StaticClass();
		else if (SenseFilter.Equals(TEXT("Damage"), ESearchCase::IgnoreCase))
			SenseClass = UAISense_Damage::StaticClass();
		else if (SenseFilter.Equals(TEXT("Touch"), ESearchCase::IgnoreCase))
			SenseClass = UAISense_Touch::StaticClass();
		else if (SenseFilter.Equals(TEXT("Team"), ESearchCase::IgnoreCase))
			SenseClass = UAISense_Team::StaticClass();
	}

	TArray<AActor*> PerceivedActors;
	PerComp->GetCurrentlyPerceivedActors(SenseClass, PerceivedActors);

	TArray<TSharedPtr<FJsonValue>> ActorArr;
	for (AActor* Actor : PerceivedActors)
	{
		if (!Actor) continue;

		TSharedPtr<FJsonObject> ActorObj = MakeActorSummary(Actor);

		// Determine which senses detect this actor
		const FActorPerceptionInfo* Info = PerComp->GetActorInfo(*Actor);
		if (Info)
		{
			TArray<TSharedPtr<FJsonValue>> SenseArr;
			for (const FAIStimulus& Stim : Info->LastSensedStimuli)
			{
				if (Stim.IsActive() && Stim.IsValid())
				{
					TSubclassOf<UAISense> StimSenseClass = UAIPerceptionSystem::GetSenseClassForStimulus(AIC->GetWorld(), Stim);
					if (StimSenseClass)
					{
						SenseArr.Add(MakeShared<FJsonValueString>(StimSenseClass->GetName()));
					}
				}
			}
			if (SenseArr.Num() > 0)
			{
				ActorObj->SetArrayField(TEXT("sensed_by"), SenseArr);
			}
		}

		ActorArr.Add(MakeShared<FJsonValueObject>(ActorObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("perceived_actors"), ActorArr);
	Result->SetNumberField(TEXT("count"), ActorArr.Num());
	if (!SenseFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("sense_filter"), SenseFilter);
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  175. runtime_check_perception
// ============================================================

FMonolithActionResult FMonolithAIRuntimeActions::HandleRuntimeCheckPerception(const TSharedPtr<FJsonObject>& Params)
{
	PIE_GUARD();

	FMonolithActionResult Err;

	// Resolve observer
	FString ObserverId = Params->GetStringField(TEXT("observer_actor"));
	if (ObserverId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'observer_actor'"));
	}
	AActor* ObserverActor = MonolithAI::FindActorInPIE(ObserverId);
	if (!ObserverActor)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Observer actor '%s' not found in PIE"), *ObserverId));
	}

	AAIController* AIC = GetAIControllerFromActor(ObserverActor);
	if (!AIC)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Observer '%s' has no AI controller"), *ObserverId));
	}

	UAIPerceptionComponent* PerComp = AIC->GetPerceptionComponent();
	if (!PerComp)
	{
		return FMonolithActionResult::Error(TEXT("Observer's AI controller has no Perception component"));
	}

	// Resolve target
	FString TargetId = Params->GetStringField(TEXT("target_actor"));
	if (TargetId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'target_actor'"));
	}
	AActor* TargetActor = MonolithAI::FindActorInPIE(TargetId);
	if (!TargetActor)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Target actor '%s' not found in PIE"), *TargetId));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("observer"), ObserverActor->GetActorLabel());
	Result->SetStringField(TEXT("target"), TargetActor->GetActorLabel());

	// Check which senses see the target
	const FActorPerceptionInfo* Info = PerComp->GetActorInfo(*TargetActor);
	bool bIsPerceived = false;
	TArray<TSharedPtr<FJsonValue>> ActiveSenses;

	if (Info)
	{
		for (const FAIStimulus& Stim : Info->LastSensedStimuli)
		{
			if (Stim.IsActive() && Stim.IsValid())
			{
				bIsPerceived = true;
				TSubclassOf<UAISense> SenseClass = UAIPerceptionSystem::GetSenseClassForStimulus(AIC->GetWorld(), Stim);
				if (SenseClass)
				{
					TSharedPtr<FJsonObject> SenseObj = MakeShared<FJsonObject>();
					SenseObj->SetStringField(TEXT("sense"), SenseClass->GetName());
					SenseObj->SetNumberField(TEXT("strength"), Stim.Strength);
					SenseObj->SetNumberField(TEXT("age"), Stim.GetAge());
					FVector StimulusLoc = Stim.StimulusLocation;
					SenseObj->SetStringField(TEXT("stimulus_location"),
						FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), StimulusLoc.X, StimulusLoc.Y, StimulusLoc.Z));
					ActiveSenses.Add(MakeShared<FJsonValueObject>(SenseObj));
				}
			}
		}
	}

	Result->SetBoolField(TEXT("is_perceived"), bIsPerceived);
	Result->SetArrayField(TEXT("active_senses"), ActiveSenses);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  176. runtime_report_noise
// ============================================================

FMonolithActionResult FMonolithAIRuntimeActions::HandleRuntimeReportNoise(const TSharedPtr<FJsonObject>& Params)
{
	PIE_GUARD();

	UWorld* World = MonolithAI::GetPIEWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No PIE world found"));
	}

	// Parse location — supports both JSON object {"x":N,"y":N,"z":N} and string "X,Y,Z"
	FVector Location = FVector::ZeroVector;
	const TSharedPtr<FJsonObject>* LocObj = nullptr;
	if (Params->TryGetObjectField(TEXT("location"), LocObj) && LocObj && (*LocObj).IsValid())
	{
		Location.X = (*LocObj)->GetNumberField(TEXT("x"));
		Location.Y = (*LocObj)->GetNumberField(TEXT("y"));
		Location.Z = (*LocObj)->GetNumberField(TEXT("z"));
	}
	else
	{
		FString LocationStr = Params->GetStringField(TEXT("location"));
		if (LocationStr.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Missing required param 'location' — use {\"x\":N,\"y\":N,\"z\":N} or \"X,Y,Z\""));
		}
		TArray<FString> Parts;
		LocationStr.Replace(TEXT("("), TEXT("")).Replace(TEXT(")"), TEXT("")).ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() < 3)
		{
			return FMonolithActionResult::Error(TEXT("Location must be {x,y,z} object or 'X,Y,Z' string"));
		}
		Location.X = FCString::Atof(*Parts[0].TrimStartAndEnd());
		Location.Y = FCString::Atof(*Parts[1].TrimStartAndEnd());
		Location.Z = FCString::Atof(*Parts[2].TrimStartAndEnd());
	}

	float Loudness = 1.0f;
	if (Params->HasField(TEXT("loudness")))
	{
		Loudness = static_cast<float>(Params->GetNumberField(TEXT("loudness")));
	}

	AActor* Instigator = nullptr;
	FString InstigatorStr = Params->GetStringField(TEXT("instigator"));
	if (!InstigatorStr.IsEmpty())
	{
		Instigator = MonolithAI::FindActorInPIE(InstigatorStr);
		if (!Instigator)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Instigator actor '%s' not found in PIE"), *InstigatorStr));
		}
	}

	FName NoiseTag = NAME_None;
	FString TagStr = Params->GetStringField(TEXT("tag"));
	if (!TagStr.IsEmpty())
	{
		NoiseTag = FName(*TagStr);
	}

	// Use the static template OnEvent to fire noise directly into the perception system
	FAINoiseEvent NoiseEvent(Instigator, Location, Loudness, 0.0f, NoiseTag);
	UAIPerceptionSystem::OnEvent<FAINoiseEvent>(World, NoiseEvent);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("location"), FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), Location.X, Location.Y, Location.Z));
	Result->SetNumberField(TEXT("loudness"), Loudness);
	if (Instigator)
	{
		Result->SetStringField(TEXT("instigator"), Instigator->GetActorLabel());
	}
	if (NoiseTag != NAME_None)
	{
		Result->SetStringField(TEXT("tag"), NoiseTag.ToString());
	}
	Result->SetStringField(TEXT("message"), TEXT("Noise event reported"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  177. runtime_get_st_active_states (StateTree)
// ============================================================

FMonolithActionResult FMonolithAIRuntimeActions::HandleRuntimeGetSTActiveStates(const TSharedPtr<FJsonObject>& Params)
{
	PIE_GUARD();

#if WITH_STATETREE
	FMonolithActionResult Err;
	AActor* Actor = ResolveActorParam(Params, TEXT("actor"), Err);
	if (!Actor) return Err;

	// Find StateTree component — could be on the actor directly or via AI controller brain
	UStateTreeComponent* STComp = Actor->FindComponentByClass<UStateTreeComponent>();
	if (!STComp)
	{
		// Try through AI controller
		AAIController* AIC = GetAIControllerFromActor(Actor);
		if (AIC)
		{
			// BrainComponent might be a StateTree
			STComp = Cast<UStateTreeComponent>(AIC->BrainComponent);
			if (!STComp)
			{
				// Check on the controller actor itself
				STComp = AIC->FindComponentByClass<UStateTreeComponent>();
			}
		}
	}

	if (!STComp)
	{
		return FMonolithActionResult::Error(TEXT("No StateTree component found on actor or its AI controller"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("is_running"), STComp->IsRunning());

	// Get active states info
	TArray<TSharedPtr<FJsonValue>> StatesArr;

	// Use GetActiveStates to enumerate
	TArray<FName> ActiveStateNames;
	// StateTreeRef is protected — access via FProperty reflection
	const UStateTree* ST = nullptr;
	if (FStructProperty* RefProp = CastField<FStructProperty>(STComp->GetClass()->FindPropertyByName(TEXT("StateTreeRef"))))
	{
		const void* RefPtr = RefProp->ContainerPtrToValuePtr<void>(STComp);
		const FStateTreeReference* StateTreeRef = static_cast<const FStateTreeReference*>(RefPtr);
		if (StateTreeRef)
		{
			ST = StateTreeRef->GetStateTree();
		}
	}
	if (ST)
	{
		Result->SetStringField(TEXT("state_tree"), ST->GetName());
		Result->SetStringField(TEXT("state_tree_path"), ST->GetPathName());
	}

	// We can get active states from the instance data via debug info
	// UStateTreeComponent has GetDebugInfoString() for a text dump
	FString DebugInfo = STComp->GetDebugInfoString();
	if (!DebugInfo.IsEmpty())
	{
		Result->SetStringField(TEXT("debug_info"), DebugInfo);
	}

	return FMonolithActionResult::Success(Result);
#else
	return FMonolithActionResult::Error(TEXT("StateTree support not compiled (WITH_STATETREE=0)"));
#endif
}

// ============================================================
//  178. runtime_send_st_event (StateTree)
// ============================================================

FMonolithActionResult FMonolithAIRuntimeActions::HandleRuntimeSendSTEvent(const TSharedPtr<FJsonObject>& Params)
{
	PIE_GUARD();

#if WITH_STATETREE
	FMonolithActionResult Err;
	AActor* Actor = ResolveActorParam(Params, TEXT("actor"), Err);
	if (!Actor) return Err;

	FString EventTagStr = Params->GetStringField(TEXT("event_tag"));
	if (EventTagStr.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'event_tag'"));
	}

	FGameplayTag EventTag = FGameplayTag::RequestGameplayTag(FName(*EventTagStr), false);
	if (!EventTag.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Gameplay tag '%s' is not registered"), *EventTagStr));
	}

	// Find StateTree component
	UStateTreeComponent* STComp = Actor->FindComponentByClass<UStateTreeComponent>();
	if (!STComp)
	{
		AAIController* AIC = GetAIControllerFromActor(Actor);
		if (AIC)
		{
			STComp = Cast<UStateTreeComponent>(AIC->BrainComponent);
			if (!STComp)
			{
				STComp = AIC->FindComponentByClass<UStateTreeComponent>();
			}
		}
	}

	if (!STComp)
	{
		return FMonolithActionResult::Error(TEXT("No StateTree component found on actor or its AI controller"));
	}

	if (!STComp->IsRunning())
	{
		return FMonolithActionResult::Error(TEXT("StateTree component is not running"));
	}

	STComp->SendStateTreeEvent(EventTag);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("event_tag"), EventTagStr);
	Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("message"), TEXT("StateTree event sent"));
	return FMonolithActionResult::Success(Result);
#else
	return FMonolithActionResult::Error(TEXT("StateTree support not compiled (WITH_STATETREE=0)"));
#endif
}

// ============================================================
//  179. runtime_find_smart_objects
// ============================================================

FMonolithActionResult FMonolithAIRuntimeActions::HandleRuntimeFindSmartObjects(const TSharedPtr<FJsonObject>& Params)
{
	PIE_GUARD();

#if WITH_SMARTOBJECTS
	UWorld* World = MonolithAI::GetPIEWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No PIE world found"));
	}

	FMonolithActionResult Err;
	AActor* QuerierActor = ResolveActorParam(Params, TEXT("querier_actor"), Err);
	if (!QuerierActor) return Err;

	double Radius = 2000.0;
	if (Params->HasField(TEXT("radius")))
	{
		Radius = Params->GetNumberField(TEXT("radius"));
	}

	// Parse activity tags
	FGameplayTagContainer ActivityTags;
	FString ActivityTagsStr = Params->GetStringField(TEXT("activity_tags"));
	if (!ActivityTagsStr.IsEmpty())
	{
		TArray<FString> TagParts;
		ActivityTagsStr.ParseIntoArray(TagParts, TEXT(","));
		for (const FString& TagStr : TagParts)
		{
			FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagStr.TrimStartAndEnd()), false);
			if (Tag.IsValid())
			{
				ActivityTags.AddTag(Tag);
			}
		}
	}

	USmartObjectSubsystem* SOSubsystem = World->GetSubsystem<USmartObjectSubsystem>();
	if (!SOSubsystem)
	{
		return FMonolithActionResult::Error(TEXT("SmartObjectSubsystem not available in PIE world"));
	}

	// Build request
	FSmartObjectRequest Request;
	Request.Filter.ActivityRequirements = FGameplayTagQuery::MakeQuery_MatchAnyTags(ActivityTags);
	Request.QueryBox = FBox(
		QuerierActor->GetActorLocation() - FVector(Radius),
		QuerierActor->GetActorLocation() + FVector(Radius));

	TArray<FSmartObjectRequestResult> Results;
	SOSubsystem->FindSmartObjects(Request, Results, FConstStructView());

	TArray<TSharedPtr<FJsonValue>> ItemsArr;
	for (const FSmartObjectRequestResult& SOResult : Results)
	{
		TSharedPtr<FJsonObject> ItemObj = MakeShared<FJsonObject>();

		// Get the smart object component from the request result
		USmartObjectComponent* SOComp = SOSubsystem->GetSmartObjectComponentByRequestResult(SOResult);
		if (SOComp)
		{
			AActor* SOActor = SOComp->GetOwner();
			if (SOActor)
			{
				ItemObj->SetStringField(TEXT("actor_name"), SOActor->GetName());
				ItemObj->SetStringField(TEXT("actor_label"), SOActor->GetActorLabel());
				FVector Loc = SOActor->GetActorLocation();
				ItemObj->SetStringField(TEXT("location"), FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), Loc.X, Loc.Y, Loc.Z));

				double Dist = FVector::Dist(QuerierActor->GetActorLocation(), Loc);
				ItemObj->SetNumberField(TEXT("distance"), Dist);
			}

			const USmartObjectDefinition* Def = SOComp->GetDefinition();
			if (Def)
			{
				ItemObj->SetStringField(TEXT("definition"), Def->GetName());
			}
		}

		ItemsArr.Add(MakeShared<FJsonValueObject>(ItemObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("smart_objects"), ItemsArr);
	Result->SetNumberField(TEXT("count"), ItemsArr.Num());
	Result->SetNumberField(TEXT("search_radius"), Radius);
	return FMonolithActionResult::Success(Result);
#else
	return FMonolithActionResult::Error(TEXT("SmartObjects support not compiled (WITH_SMARTOBJECTS=0)"));
#endif
}

// ============================================================
//  180. runtime_run_eqs_query
// ============================================================

FMonolithActionResult FMonolithAIRuntimeActions::HandleRuntimeRunEQSQuery(const TSharedPtr<FJsonObject>& Params)
{
	PIE_GUARD();

	UWorld* World = MonolithAI::GetPIEWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No PIE world found"));
	}

	FMonolithActionResult Err;
	AActor* QuerierActor = ResolveActorParam(Params, TEXT("querier_actor"), Err);
	if (!QuerierActor) return Err;

	FString QueryPath = Params->GetStringField(TEXT("query_path"));
	if (QueryPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'query_path'"));
	}

	UEnvQuery* QueryTemplate = Cast<UEnvQuery>(FMonolithAssetUtils::LoadAssetByPath(UEnvQuery::StaticClass(), QueryPath));
	if (!QueryTemplate)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("EQS query not found: %s"), *QueryPath));
	}

	int32 MaxResults = 10;
	if (Params->HasField(TEXT("max_results")))
	{
		MaxResults = FMath::Max(1, FMath::RoundToInt32(Params->GetNumberField(TEXT("max_results"))));
	}

	// Run instant query
	UEnvQueryManager* EQSManager = UEnvQueryManager::GetCurrent(World);
	if (!EQSManager)
	{
		return FMonolithActionResult::Error(TEXT("EnvQueryManager not available in PIE world"));
	}

	FEnvQueryRequest QueryRequest(QueryTemplate, QuerierActor);
	TSharedPtr<FEnvQueryResult> QueryResult = EQSManager->RunInstantQuery(QueryRequest, EEnvQueryRunMode::AllMatching);

	if (!QueryResult.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("EQS query returned no result"));
	}

	if (!QueryResult->IsSuccessful())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("EQS query failed with status %d"), static_cast<int32>(QueryResult->GetRawStatus())));
	}

	// Sort items by score descending
	QueryResult->Items.Sort([](const FEnvQueryItem& A, const FEnvQueryItem& B) { return A.Score > B.Score; });

	TArray<TSharedPtr<FJsonValue>> ItemsArr;
	int32 NumItems = FMath::Min(QueryResult->Items.Num(), MaxResults);

	for (int32 i = 0; i < NumItems; ++i)
	{
		const FEnvQueryItem& Item = QueryResult->Items[i];
		if (!Item.IsValid()) continue;

		TSharedPtr<FJsonObject> ItemObj = MakeShared<FJsonObject>();
		ItemObj->SetNumberField(TEXT("score"), Item.Score);
		ItemObj->SetNumberField(TEXT("rank"), i);

		// Try to get location
		FVector ItemLoc = QueryResult->GetItemAsLocation(i);
		ItemObj->SetStringField(TEXT("location"), FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), ItemLoc.X, ItemLoc.Y, ItemLoc.Z));

		// Check if it's an actor-based result
		if (QueryResult->ItemType && QueryResult->ItemType->IsChildOf(UEnvQueryItemType_ActorBase::StaticClass()))
		{
			AActor* ItemActor = QueryResult->GetItemAsActor(i);
			if (ItemActor)
			{
				ItemObj->SetStringField(TEXT("actor_name"), ItemActor->GetName());
				ItemObj->SetStringField(TEXT("actor_label"), ItemActor->GetActorLabel());
				ItemObj->SetStringField(TEXT("actor_class"), ItemActor->GetClass()->GetName());
			}
		}

		ItemsArr.Add(MakeShared<FJsonValueObject>(ItemObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("query"), QueryTemplate->GetName());
	Result->SetNumberField(TEXT("total_items"), QueryResult->Items.Num());
	Result->SetNumberField(TEXT("returned_items"), ItemsArr.Num());
	Result->SetArrayField(TEXT("items"), ItemsArr);
	return FMonolithActionResult::Success(Result);
}

#undef PIE_GUARD
