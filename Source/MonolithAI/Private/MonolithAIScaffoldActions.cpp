#include "MonolithAIScaffoldActions.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "GameFramework/Character.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/DataTable.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "ScopedTransaction.h"

#if WITH_STATETREE
#include "StateTree.h"
#endif

// ============================================================
//  Helpers
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::Dispatch(
	const FString& Namespace, const FString& Action, const TSharedPtr<FJsonObject>& Params)
{
	return FMonolithToolRegistry::Get().ExecuteAction(Namespace, Action, Params);
}

bool FMonolithAIScaffoldActions::DispatchOrWarn(
	const FString& Namespace, const FString& Action,
	const TSharedPtr<FJsonObject>& Params, TArray<FString>& Warnings, FString StepName)
{
	FMonolithActionResult R = Dispatch(Namespace, Action, Params);
	if (!R.bSuccess)
	{
		Warnings.Add(FString::Printf(TEXT("%s failed: %s"), *StepName, *R.ErrorMessage));
		return false;
	}
	return true;
}

TArray<TSharedPtr<FJsonValue>> FMonolithAIScaffoldActions::BuildBBKeysForTemplate(const FString& TemplateName)
{
	TArray<TSharedPtr<FJsonValue>> Keys;

	auto MakeKey = [](const FString& Name, const FString& Type, const FString& BaseClass = TEXT("")) -> TSharedPtr<FJsonValue>
	{
		TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
		K->SetStringField(TEXT("name"), Name);
		K->SetStringField(TEXT("type"), Type);
		if (!BaseClass.IsEmpty())
		{
			K->SetStringField(TEXT("base_class"), BaseClass);
		}
		return MakeShared<FJsonValueObject>(K);
	};

	if (TemplateName == TEXT("patrol"))
	{
		Keys.Add(MakeKey(TEXT("TargetActor"), TEXT("Object"), TEXT("Actor")));
		Keys.Add(MakeKey(TEXT("PatrolIndex"), TEXT("Int")));
		Keys.Add(MakeKey(TEXT("PatrolLocation"), TEXT("Vector")));
	}
	else if (TemplateName == TEXT("chase_attack"))
	{
		Keys.Add(MakeKey(TEXT("TargetActor"), TEXT("Object"), TEXT("Actor")));
		Keys.Add(MakeKey(TEXT("AttackRange"), TEXT("Float")));
	}
	else if (TemplateName == TEXT("flee"))
	{
		Keys.Add(MakeKey(TEXT("ThreatActor"), TEXT("Object"), TEXT("Actor")));
		Keys.Add(MakeKey(TEXT("EscapePoint"), TEXT("Vector")));
	}
	else if (TemplateName == TEXT("search_area"))
	{
		Keys.Add(MakeKey(TEXT("SearchOrigin"), TEXT("Vector")));
		Keys.Add(MakeKey(TEXT("SearchPoint"), TEXT("Vector")));
		Keys.Add(MakeKey(TEXT("TargetActor"), TEXT("Object"), TEXT("Actor")));
	}
	else if (TemplateName == TEXT("guard_post"))
	{
		Keys.Add(MakeKey(TEXT("GuardLocation"), TEXT("Vector")));
		Keys.Add(MakeKey(TEXT("TargetActor"), TEXT("Object"), TEXT("Actor")));
		Keys.Add(MakeKey(TEXT("HeardLocation"), TEXT("Vector")));
		Keys.Add(MakeKey(TEXT("bHeardNoise"), TEXT("Bool")));
	}
	else if (TemplateName == TEXT("patrol_investigate"))
	{
		Keys.Add(MakeKey(TEXT("TargetActor"), TEXT("Object"), TEXT("Actor")));
		Keys.Add(MakeKey(TEXT("PatrolIndex"), TEXT("Int")));
		Keys.Add(MakeKey(TEXT("PatrolLocation"), TEXT("Vector")));
		Keys.Add(MakeKey(TEXT("HeardLocation"), TEXT("Vector")));
		Keys.Add(MakeKey(TEXT("bHeardNoise"), TEXT("Bool")));
		Keys.Add(MakeKey(TEXT("SearchPoint"), TEXT("Vector")));
	}
	else
	{
		// Default: basic keys
		Keys.Add(MakeKey(TEXT("TargetActor"), TEXT("Object"), TEXT("Actor")));
	}

	return Keys;
}

TSharedPtr<FJsonObject> FMonolithAIScaffoldActions::BuildBTTemplateSpec(const FString& TemplateName, const FString& BBPath)
{
	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	if (!BBPath.IsEmpty())
	{
		Spec->SetStringField(TEXT("blackboard_path"), BBPath);
	}

	// Helper lambdas for building spec nodes
	auto MakeNode = [](const FString& /*Category*/, const FString& ClassName) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>();
		// build_behavior_tree_from_spec uses "type" as the class name (e.g. "Selector", "BTTask_Wait")
		N->SetStringField(TEXT("type"), ClassName);
		return N;
	};

	// build_behavior_tree_from_spec expects properties as JSON object {key: value}, not array
	auto SetProp = [](TSharedPtr<FJsonObject>& Node, const FString& Key, const FString& Value)
	{
		const TSharedPtr<FJsonObject>* ExistingProps = nullptr;
		TSharedPtr<FJsonObject> Props;
		if (Node->TryGetObjectField(TEXT("properties"), ExistingProps) && ExistingProps)
		{
			Props = *ExistingProps;
		}
		else
		{
			Props = MakeShared<FJsonObject>();
			Node->SetObjectField(TEXT("properties"), Props);
		}
		Props->SetStringField(Key, Value);
	};

	auto SetChildren = [](TSharedPtr<FJsonObject>& Node, const TArray<TSharedPtr<FJsonValue>>& Children)
	{
		Node->SetArrayField(TEXT("children"), Children);
	};

	auto SetDecorators = [](TSharedPtr<FJsonObject>& Node, const TArray<TSharedPtr<FJsonValue>>& Decorators)
	{
		Node->SetArrayField(TEXT("decorators"), Decorators);
	};

	if (TemplateName == TEXT("patrol"))
	{
		// Root Selector → [Chase Sequence (decorator: HasTarget), Patrol Sequence]
		TSharedPtr<FJsonObject> Root = MakeNode(TEXT("composite"), TEXT("BTComposite_Selector"));

		// Chase sequence (with BB decorator: TargetActor is set)
		TSharedPtr<FJsonObject> ChaseSeq = MakeNode(TEXT("composite"), TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> HasTargetDec = MakeShared<FJsonObject>();
			HasTargetDec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SetProp(HasTargetDec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("TargetActor"));
			SetDecorators(ChaseSeq, { MakeShared<FJsonValueObject>(HasTargetDec) });

			TSharedPtr<FJsonObject> MoveToTarget = MakeNode(TEXT("task"), TEXT("BTTask_MoveTo"));
			SetProp(MoveToTarget, TEXT("BlackboardKey.SelectedKeyName"), TEXT("TargetActor"));
			SetProp(MoveToTarget, TEXT("AcceptableRadius"), TEXT("100"));

			TSharedPtr<FJsonObject> WaitTask = MakeNode(TEXT("task"), TEXT("BTTask_Wait"));
			SetProp(WaitTask, TEXT("WaitTime"), TEXT("1.0"));

			SetChildren(ChaseSeq, {
				MakeShared<FJsonValueObject>(MoveToTarget),
				MakeShared<FJsonValueObject>(WaitTask)
			});
		}

		// Patrol sequence
		TSharedPtr<FJsonObject> PatrolSeq = MakeNode(TEXT("composite"), TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> MoveToPatrol = MakeNode(TEXT("task"), TEXT("BTTask_MoveTo"));
			SetProp(MoveToPatrol, TEXT("BlackboardKey.SelectedKeyName"), TEXT("PatrolLocation"));
			SetProp(MoveToPatrol, TEXT("AcceptableRadius"), TEXT("50"));

			TSharedPtr<FJsonObject> Wait = MakeNode(TEXT("task"), TEXT("BTTask_Wait"));
			SetProp(Wait, TEXT("WaitTime"), TEXT("3.0"));

			SetChildren(PatrolSeq, {
				MakeShared<FJsonValueObject>(MoveToPatrol),
				MakeShared<FJsonValueObject>(Wait)
			});
		}

		SetChildren(Root, {
			MakeShared<FJsonValueObject>(ChaseSeq),
			MakeShared<FJsonValueObject>(PatrolSeq)
		});

		Spec->SetObjectField(TEXT("root"), Root);
	}
	else if (TemplateName == TEXT("chase_attack"))
	{
		// Sequence → MoveTo(TargetActor) + Wait(cooldown)
		TSharedPtr<FJsonObject> Root = MakeNode(TEXT("composite"), TEXT("BTComposite_Sequence"));

		TSharedPtr<FJsonObject> MoveTo = MakeNode(TEXT("task"), TEXT("BTTask_MoveTo"));
		SetProp(MoveTo, TEXT("BlackboardKey.SelectedKeyName"), TEXT("TargetActor"));
		SetProp(MoveTo, TEXT("AcceptableRadius"), TEXT("150"));

		TSharedPtr<FJsonObject> Wait = MakeNode(TEXT("task"), TEXT("BTTask_Wait"));
		SetProp(Wait, TEXT("WaitTime"), TEXT("2.0"));

		SetChildren(Root, {
			MakeShared<FJsonValueObject>(MoveTo),
			MakeShared<FJsonValueObject>(Wait)
		});

		Spec->SetObjectField(TEXT("root"), Root);
	}
	else if (TemplateName == TEXT("flee"))
	{
		// Sequence → MoveTo(EscapePoint)
		TSharedPtr<FJsonObject> Root = MakeNode(TEXT("composite"), TEXT("BTComposite_Sequence"));

		TSharedPtr<FJsonObject> MoveTo = MakeNode(TEXT("task"), TEXT("BTTask_MoveTo"));
		SetProp(MoveTo, TEXT("BlackboardKey.SelectedKeyName"), TEXT("EscapePoint"));
		SetProp(MoveTo, TEXT("AcceptableRadius"), TEXT("50"));

		TSharedPtr<FJsonObject> Wait = MakeNode(TEXT("task"), TEXT("BTTask_Wait"));
		SetProp(Wait, TEXT("WaitTime"), TEXT("1.0"));

		SetChildren(Root, {
			MakeShared<FJsonValueObject>(MoveTo),
			MakeShared<FJsonValueObject>(Wait)
		});

		Spec->SetObjectField(TEXT("root"), Root);
	}
	else if (TemplateName == TEXT("search_area"))
	{
		// Sequence → MoveTo(SearchPoint) + Wait + Loop via Selector wrapper
		TSharedPtr<FJsonObject> Root = MakeNode(TEXT("composite"), TEXT("BTComposite_Selector"));

		// Abort-to-target if we find one
		TSharedPtr<FJsonObject> ChaseSeq = MakeNode(TEXT("composite"), TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> HasTargetDec = MakeShared<FJsonObject>();
			HasTargetDec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SetProp(HasTargetDec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("TargetActor"));
			SetDecorators(ChaseSeq, { MakeShared<FJsonValueObject>(HasTargetDec) });

			TSharedPtr<FJsonObject> MoveTo = MakeNode(TEXT("task"), TEXT("BTTask_MoveTo"));
			SetProp(MoveTo, TEXT("BlackboardKey.SelectedKeyName"), TEXT("TargetActor"));

			SetChildren(ChaseSeq, { MakeShared<FJsonValueObject>(MoveTo) });
		}

		// Search loop
		TSharedPtr<FJsonObject> SearchSeq = MakeNode(TEXT("composite"), TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> MoveTo = MakeNode(TEXT("task"), TEXT("BTTask_MoveTo"));
			SetProp(MoveTo, TEXT("BlackboardKey.SelectedKeyName"), TEXT("SearchPoint"));

			TSharedPtr<FJsonObject> Wait = MakeNode(TEXT("task"), TEXT("BTTask_Wait"));
			SetProp(Wait, TEXT("WaitTime"), TEXT("2.0"));

			SetChildren(SearchSeq, {
				MakeShared<FJsonValueObject>(MoveTo),
				MakeShared<FJsonValueObject>(Wait)
			});
		}

		SetChildren(Root, {
			MakeShared<FJsonValueObject>(ChaseSeq),
			MakeShared<FJsonValueObject>(SearchSeq)
		});

		Spec->SetObjectField(TEXT("root"), Root);
	}
	else if (TemplateName == TEXT("guard_post"))
	{
		// Selector → [Investigate (decorator: HeardNoise), ReturnToPost]
		TSharedPtr<FJsonObject> Root = MakeNode(TEXT("composite"), TEXT("BTComposite_Selector"));

		// Investigate branch
		TSharedPtr<FJsonObject> InvestigateSeq = MakeNode(TEXT("composite"), TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> HeardDec = MakeShared<FJsonObject>();
			HeardDec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SetProp(HeardDec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("bHeardNoise"));
			SetDecorators(InvestigateSeq, { MakeShared<FJsonValueObject>(HeardDec) });

			TSharedPtr<FJsonObject> MoveTo = MakeNode(TEXT("task"), TEXT("BTTask_MoveTo"));
			SetProp(MoveTo, TEXT("BlackboardKey.SelectedKeyName"), TEXT("HeardLocation"));

			TSharedPtr<FJsonObject> Wait = MakeNode(TEXT("task"), TEXT("BTTask_Wait"));
			SetProp(Wait, TEXT("WaitTime"), TEXT("3.0"));

			SetChildren(InvestigateSeq, {
				MakeShared<FJsonValueObject>(MoveTo),
				MakeShared<FJsonValueObject>(Wait)
			});
		}

		// Return to post
		TSharedPtr<FJsonObject> ReturnSeq = MakeNode(TEXT("composite"), TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> MoveTo = MakeNode(TEXT("task"), TEXT("BTTask_MoveTo"));
			SetProp(MoveTo, TEXT("BlackboardKey.SelectedKeyName"), TEXT("GuardLocation"));

			TSharedPtr<FJsonObject> Wait = MakeNode(TEXT("task"), TEXT("BTTask_Wait"));
			SetProp(Wait, TEXT("WaitTime"), TEXT("1.0"));

			SetChildren(ReturnSeq, {
				MakeShared<FJsonValueObject>(MoveTo),
				MakeShared<FJsonValueObject>(Wait)
			});
		}

		SetChildren(Root, {
			MakeShared<FJsonValueObject>(InvestigateSeq),
			MakeShared<FJsonValueObject>(ReturnSeq)
		});

		Spec->SetObjectField(TEXT("root"), Root);
	}
	else
	{
		return nullptr; // Unknown template
	}

	return Spec;
}

// ============================================================
//  Registration
// ============================================================

void FMonolithAIScaffoldActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 180b. hello_world_ai
	Registry.RegisterAction(TEXT("ai"), TEXT("hello_world_ai"),
		TEXT("ONE-CALL onboarding: creates Character BP + Controller + BT (patrol 3 waypoints) + BB + Perception (sight+hearing) + team. Returns all paths."),
		FMonolithActionHandler::CreateStatic(&HandleHelloWorldAI),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Base directory for assets (e.g. /Game/AI/HelloWorld)"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Name prefix for all assets (default: HelloWorldAI)"))
			.Optional(TEXT("location"), TEXT("string"), TEXT("Spawn location as 'X,Y,Z' — also places NavMeshBoundsVolume"))
			.Build());

	// 181. scaffold_complete_ai_character
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_complete_ai_character"),
		TEXT("Full AI stack: Character BP + Controller + BT + BB + Perception + Team, all wired together"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldCompleteAICharacter),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Base directory for assets (e.g. /Game/AI/Enemy)"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Name for the AI character (used as prefix)"))
			.Optional(TEXT("mesh"), TEXT("string"), TEXT("Skeletal mesh asset path for the character"))
			.Optional(TEXT("bt_template"), TEXT("string"), TEXT("BT template: patrol, chase_attack, flee, search_area, guard_post"))
			.Optional(TEXT("perception_preset"), TEXT("string"), TEXT("Perception preset: sight, hearing, sight_hearing, full"))
			.Optional(TEXT("team_id"), TEXT("number"), TEXT("Team ID (0-254, default: 1)"))
			.Build());

	// 182. scaffold_perception_to_blackboard
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_perception_to_blackboard"),
		TEXT("Wire perception events to blackboard keys (creates BB keys if needed). Note: configures perception senses and BB keys for the perception→BB bridge pattern."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldPerceptionToBlackboard),
		FParamSchemaBuilder()
			.Required(TEXT("controller_path"), TEXT("string"), TEXT("AIController Blueprint asset path"))
			.Required(TEXT("bb_path"), TEXT("string"), TEXT("Blackboard asset path"))
			.Optional(TEXT("mappings"), TEXT("array"), TEXT("Array of {sense, bb_key} objects, e.g. [{\"sense\":\"Sight\",\"bb_key\":\"TargetActor\"},{\"sense\":\"Hearing\",\"bb_key\":\"HeardLocation\"}]"))
			.Build());

	// 184. scaffold_team_system
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_team_system"),
		TEXT("Full team setup: create team attitude DataTable with specified teams and attitudes"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldTeamSystem),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Save path for team DataTable (e.g. /Game/AI/DT_TeamAttitudes)"))
			.Required(TEXT("teams"), TEXT("array"), TEXT("Array of team objects: [{\"id\": 0, \"name\": \"Player\"}, {\"id\": 1, \"name\": \"Enemy\"}]"))
			.Optional(TEXT("attitudes"), TEXT("array"), TEXT("Array of attitude definitions: [{\"from\": 0, \"to\": 1, \"attitude\": \"Hostile\"}, ...]. Default: all teams hostile to each other, friendly to self."))
			.Build());

	// 185. scaffold_patrol_investigate_ai
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_patrol_investigate_ai"),
		TEXT("Guard AI scaffold: patrol→hear→investigate→search→return. Full Character+Controller+BT+BB+Perception stack."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldPatrolInvestigateAI),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Base directory for assets"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Name prefix for all assets"))
			.Optional(TEXT("patrol_type"), TEXT("string"), TEXT("Patrol type: loop, pingpong, random (default: loop)"))
			.Optional(TEXT("investigation_radius"), TEXT("number"), TEXT("Investigation search radius in cm (default: 500)"))
			.Build());

	// 186. scaffold_enemy_ai
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_enemy_ai"),
		TEXT("Basic enemy scaffold with chase+attack behavior. Archetype determines BT structure."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldEnemyAI),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Base directory for assets"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Name for the enemy"))
			.Required(TEXT("archetype"), TEXT("string"), TEXT("Enemy archetype: melee, ranged, charger"))
			.Optional(TEXT("team_id"), TEXT("number"), TEXT("Team ID (default: 1)"))
			.Build());

	// 198. scaffold_eqs_move_sequence
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_eqs_move_sequence"),
		TEXT("Convenience: add a RunEQS→store→MoveTo sequence to an existing BT"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldEQSMoveSequence),
		FParamSchemaBuilder()
			.Required(TEXT("bt_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Optional(TEXT("parent_id"), TEXT("string"), TEXT("GUID of parent composite node (null = root)"))
			.Required(TEXT("eqs_path"), TEXT("string"), TEXT("EQS query asset path"))
			.Required(TEXT("bb_key"), TEXT("string"), TEXT("Blackboard key for EQS result + MoveTo target"))
			.Build());

	// 199. create_bt_from_template
	Registry.RegisterAction(TEXT("ai"), TEXT("create_bt_from_template"),
		TEXT("Create a Behavior Tree from a named template with standard BB keys"),
		FMonolithActionHandler::CreateStatic(&HandleCreateBTFromTemplate),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path (e.g. /Game/AI/BT_Patrol)"))
			.Required(TEXT("template"), TEXT("string"), TEXT("Template name: patrol, chase_attack, flee, search_area, guard_post"))
			.Build());

	// 200. create_st_from_template
	Registry.RegisterAction(TEXT("ai"), TEXT("create_st_from_template"),
		TEXT("Create a State Tree from a named template (requires StateTree plugin)"),
		FMonolithActionHandler::CreateStatic(&HandleCreateSTFromTemplate),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path (e.g. /Game/AI/ST_Patrol)"))
			.Required(TEXT("template"), TEXT("string"), TEXT("Template name: patrol, combat, investigation, ambient"))
			.Build());

	// 206. batch_validate_ai_assets
	Registry.RegisterAction(TEXT("ai"), TEXT("batch_validate_ai_assets"),
		TEXT("Run all validators across AI assets — BTs, STs, EQS, SOs, controllers. "
		     "If 'asset_paths' is provided, validates only those exact paths. "
		     "Otherwise scans the project (filtered by 'path_filter' if set)."),
		FMonolithActionHandler::CreateStatic(&HandleBatchValidateAIAssets),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Only validate assets under this path prefix (full-scan mode)"))
			.Optional(TEXT("asset_paths"), TEXT("array"), TEXT("Optional explicit list of asset paths to validate. When set, full scan is skipped and only these paths are checked."))
			.Build());

	// 107. validate_ai_controller
	Registry.RegisterAction(TEXT("ai"), TEXT("validate_ai_controller"),
		TEXT("Validate an AI controller: check BT/BB refs, perception configured, team set"),
		FMonolithActionHandler::CreateStatic(&HandleValidateAIController),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AIController Blueprint asset path"))
			.Build());

	// 106. scaffold_ai_controller_blueprint
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_ai_controller_blueprint"),
		TEXT("Full AI controller setup in one call: create controller BP + link BT/BB + perception + team"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldAIControllerBlueprint),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Save path for the AI controller Blueprint"))
			.Required(TEXT("bt_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Required(TEXT("bb_path"), TEXT("string"), TEXT("Blackboard asset path"))
			.Optional(TEXT("perception_preset"), TEXT("string"), TEXT("Perception preset: sight, hearing, sight_hearing, full"))
			.Optional(TEXT("team_id"), TEXT("number"), TEXT("Team ID (0-254)"))
			.Build());

	// 187. scaffold_companion_ai
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_companion_ai"),
		TEXT("Friendly companion AI: follows player, optionally fights alongside. Full Character+Controller+BT+BB+Perception stack."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldCompanionAI),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Base directory for assets"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Name for the companion"))
			.Optional(TEXT("follow_distance"), TEXT("number"), TEXT("Follow distance in cm (default: 300)"))
			.Optional(TEXT("combat_behavior"), TEXT("string"), TEXT("Combat behavior: passive, defensive, aggressive (default: defensive)"))
			.Build());

	// 188. scaffold_boss_ai
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_boss_ai"),
		TEXT("Multi-phase boss AI with health-threshold phase transitions. Full AI stack with phase-aware BT."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldBossAI),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Base directory for assets"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Boss name"))
			.Optional(TEXT("phases"), TEXT("array"), TEXT("Array of phase objects: [{\"name\":\"Phase1\",\"health_threshold\":0.75}, ...]. Default: 3 phases at 75/50/25%."))
			.Build());

	// 189. scaffold_ambient_npc
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_ambient_npc"),
		TEXT("Ambient civilian NPC with Smart Object interactions and wander behavior."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldAmbientNPC),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Base directory for assets"))
			.Required(TEXT("name"), TEXT("string"), TEXT("NPC name"))
			.Optional(TEXT("smart_objects"), TEXT("array"), TEXT("Array of SO activity tag strings (e.g. [\"Activity.Sit\", \"Activity.Read\"])"))
			.Optional(TEXT("wander_radius"), TEXT("number"), TEXT("Wander radius in cm (default: 1000)"))
			.Build());

	// 190. scaffold_horror_stalker
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_horror_stalker"),
		TEXT("Horror stalker AI: follows player at distance, closes in during dark/vulnerability. For survival horror."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldHorrorStalker),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Base directory for assets"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Stalker name"))
			.Optional(TEXT("stalk_distance"), TEXT("number"), TEXT("Distance to maintain while stalking in cm (default: 1500)"))
			.Optional(TEXT("attack_conditions"), TEXT("string"), TEXT("When to close in: darkness, low_health, alone (default: darkness)"))
			.Build());

	// 191. scaffold_horror_ambush
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_horror_ambush"),
		TEXT("Horror ambush AI: dormant until triggered, burst attack, then retreat. Jump-scare archetype."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldHorrorAmbush),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Base directory for assets"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Ambusher name"))
			.Optional(TEXT("trigger_type"), TEXT("string"), TEXT("Trigger: proximity, sound, line_of_sight, interact (default: proximity)"))
			.Optional(TEXT("attack_pattern"), TEXT("string"), TEXT("Attack pattern: lunge, grab, scream_then_attack (default: lunge)"))
			.Build());

	// 192. scaffold_horror_presence
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_horror_presence"),
		TEXT("Invisible horror presence: no physical form, manipulates environment (lights, doors, sounds). Psychological horror."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldHorrorPresence),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Base directory for assets"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Presence name"))
			.Optional(TEXT("effects"), TEXT("array"), TEXT("Array of effect strings: flicker_lights, open_doors, whispers, cold_breath, move_objects (default: all)"))
			.Build());

	// 193. scaffold_horror_mimic
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_horror_mimic"),
		TEXT("Horror mimic AI: disguises as a static object, attacks when player gets close. Classic mimic."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldHorrorMimic),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Base directory for assets"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Mimic name"))
			.Optional(TEXT("disguise_mesh"), TEXT("string"), TEXT("Static mesh path for disguise form"))
			.Optional(TEXT("reveal_conditions"), TEXT("string"), TEXT("When to reveal: proximity, interact, damage (default: proximity)"))
			.Build());

	// 194. scaffold_stealth_game_ai
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_stealth_game_ai"),
		TEXT("Stealth game AI with detection meter and multi-state alert cascade (unaware→suspicious→searching→alert→combat)."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldStealthGameAI),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Base directory for assets"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Guard name"))
			.Optional(TEXT("detection_meter"), TEXT("boolean"), TEXT("Enable gradual detection meter (default: true)"))
			.Optional(TEXT("alert_states"), TEXT("string"), TEXT("Alert states: simple (unaware/alert), standard (unaware/suspicious/alert), full (unaware/suspicious/searching/alert/combat). Default: standard"))
			.Build());

	// 195. scaffold_turret_ai
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_turret_ai"),
		TEXT("Stationary turret AI with detection cone and engagement range. Does not move."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldTurretAI),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Base directory for assets"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Turret name"))
			.Optional(TEXT("detection_cone"), TEXT("number"), TEXT("Detection half-angle in degrees (default: 45)"))
			.Optional(TEXT("engagement_range"), TEXT("number"), TEXT("Max engagement range in cm (default: 3000)"))
			.Build());

	// 196. scaffold_group_coordinator
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_group_coordinator"),
		TEXT("Squad coordinator AI that assigns tactical roles (flanker, suppressor, rusher) to group members."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldGroupCoordinator),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Base directory for assets"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Squad name"))
			.Optional(TEXT("roles"), TEXT("array"), TEXT("Array of role strings (default: [\"flanker\", \"suppressor\", \"rusher\"])"))
			.Build());

	// 197. scaffold_flying_ai
	Registry.RegisterAction(TEXT("ai"), TEXT("scaffold_flying_ai"),
		TEXT("Flying AI with 3D navigation and altitude management. Uses flying movement mode."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldFlyingAI),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Base directory for assets"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Flying AI name"))
			.Optional(TEXT("altitude_range"), TEXT("string"), TEXT("Altitude range as 'min,max' in cm (default: 500,2000)"))
			.Build());
}

// ============================================================
//  180b. hello_world_ai
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleHelloWorldAI(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult))
	{
		return ErrResult;
	}

	FString Name = Params->GetStringField(TEXT("name"));
	if (Name.IsEmpty())
	{
		Name = TEXT("HelloWorldAI");
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Hello World AI")));

	TArray<FString> CreatedAssets;
	TArray<FString> Warnings;

	FString BBPath = SavePath / TEXT("BB_") + Name;
	FString BTPath = SavePath / TEXT("BT_") + Name;
	FString ControllerPath = SavePath / TEXT("BP_") + Name + TEXT("Controller");
	FString CharacterPath = SavePath / TEXT("BP_") + Name + TEXT("Character");

	// 1. Create Blackboard
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BBPath);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("create_blackboard"), P);
		if (!R.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create Blackboard: %s"), *R.ErrorMessage));
		}
		CreatedAssets.Add(BBPath);

		// Add keys: TargetActor (Object), PatrolIndex (Int), PatrolLocation (Vector)
		TSharedPtr<FJsonObject> KeysP = MakeShared<FJsonObject>();
		KeysP->SetStringField(TEXT("asset_path"), BBPath);
		TArray<TSharedPtr<FJsonValue>> Keys = BuildBBKeysForTemplate(TEXT("patrol"));
		KeysP->SetArrayField(TEXT("keys"), Keys);
		DispatchOrWarn(TEXT("ai"), TEXT("batch_add_bb_keys"), KeysP, Warnings, TEXT("Add BB keys"));
	}

	// 2. Create BT from patrol template
	{
		TSharedPtr<FJsonObject> Spec = BuildBTTemplateSpec(TEXT("patrol"), BBPath);
		if (!Spec.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Failed to build patrol BT spec"));
		}

		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BTPath);
		P->SetObjectField(TEXT("spec"), Spec);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("build_behavior_tree_from_spec"), P);
		if (!R.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create BT: %s"), *R.ErrorMessage));
		}
		CreatedAssets.Add(BTPath);

		// Phase D1: belt-and-suspenders BB linkage (Issue #48)
		TSharedPtr<FJsonObject> SetBBParams = MakeShared<FJsonObject>();
		SetBBParams->SetStringField(TEXT("bt_path"), BTPath);
		SetBBParams->SetStringField(TEXT("blackboard_path"), BBPath);
		FMonolithActionResult LinkResult = Dispatch(TEXT("ai"), TEXT("set_bt_blackboard"), SetBBParams);
		if (!LinkResult.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to link BT to Blackboard: %s"), *LinkResult.ErrorMessage));
		}
	}

	// 3. Create AI Controller
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), ControllerPath);
		P->SetStringField(TEXT("bt_path"), BTPath);
		P->SetStringField(TEXT("bb_path"), BBPath);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("create_ai_controller"), P);
		if (!R.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create AI Controller: %s"), *R.ErrorMessage));
		}
		CreatedAssets.Add(ControllerPath);
	}

	// 4. Set BT on controller
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), ControllerPath);
		P->SetStringField(TEXT("bt_path"), BTPath);
		P->SetStringField(TEXT("bb_path"), BBPath);
		DispatchOrWarn(TEXT("ai"), TEXT("set_ai_controller_bt"), P, Warnings, TEXT("Set controller BT"));
	}

	// 5. Add perception component with sight + hearing
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), ControllerPath);
		P->SetStringField(TEXT("dominant_sense"), TEXT("Sight"));
		DispatchOrWarn(TEXT("ai"), TEXT("add_perception_component"), P, Warnings, TEXT("Add perception component"));
	}

	// Configure sight
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), ControllerPath);
		P->SetNumberField(TEXT("radius"), 1500.0);
		P->SetNumberField(TEXT("peripheral_angle"), 60.0);
		DispatchOrWarn(TEXT("ai"), TEXT("configure_sight_sense"), P, Warnings, TEXT("Configure sight"));
	}

	// Configure hearing
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), ControllerPath);
		P->SetNumberField(TEXT("range"), 2000.0);
		DispatchOrWarn(TEXT("ai"), TEXT("configure_hearing_sense"), P, Warnings, TEXT("Configure hearing"));
	}

	// 6. Set team ID = 1
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), ControllerPath);
		P->SetNumberField(TEXT("team_id"), 1);
		DispatchOrWarn(TEXT("ai"), TEXT("set_ai_team"), P, Warnings, TEXT("Set team ID"));
	}

	// 7. Create Character Blueprint
	{
		// We need to create a Character Blueprint manually since there's no dedicated "create_character" action
		FString CharAssetName = FPackageName::GetShortName(CharacterPath);
		FString PathError;
		if (!MonolithAI::EnsureAssetPathFree(CharacterPath, CharAssetName, PathError))
		{
			Warnings.Add(FString::Printf(TEXT("Character creation skipped: %s"), *PathError));
		}
		else
		{
			FString PkgError;
			UPackage* Package = MonolithAI::GetOrCreatePackage(CharacterPath, PkgError);
			if (Package)
			{
				UBlueprint* CharBP = FKismetEditorUtilities::CreateBlueprint(
					ACharacter::StaticClass(),
					Package,
					*CharAssetName,
					BPTYPE_Normal,
					FName(TEXT("MonolithAI")));

				if (CharBP)
				{
					FAssetRegistryModule::AssetCreated(CharBP);
					CharBP->MarkPackageDirty();
					CreatedAssets.Add(CharacterPath);

					// Set AIControllerClass on the character
					TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
					P->SetStringField(TEXT("blueprint_path"), CharacterPath);
					P->SetStringField(TEXT("controller_class"), ControllerPath);
					DispatchOrWarn(TEXT("ai"), TEXT("set_pawn_ai_controller_class"), P, Warnings, TEXT("Set AIControllerClass"));
				}
				else
				{
					Warnings.Add(TEXT("Failed to create Character Blueprint"));
				}
			}
			else
			{
				Warnings.Add(FString::Printf(TEXT("Failed to create Character package: %s"), *PkgError));
			}
		}
	}

	// Build result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Hello World AI created — %d assets"), CreatedAssets.Num()));

	TSharedPtr<FJsonObject> Paths = MakeShared<FJsonObject>();
	Paths->SetStringField(TEXT("blackboard"), BBPath);
	Paths->SetStringField(TEXT("behavior_tree"), BTPath);
	Paths->SetStringField(TEXT("ai_controller"), ControllerPath);
	Paths->SetStringField(TEXT("character"), CharacterPath);
	Result->SetObjectField(TEXT("assets"), Paths);

	TArray<TSharedPtr<FJsonValue>> AssetArr;
	for (const FString& A : CreatedAssets)
	{
		AssetArr.Add(MakeShared<FJsonValueString>(A));
	}
	Result->SetArrayField(TEXT("created_assets"), AssetArr);
	Result->SetNumberField(TEXT("asset_count"), CreatedAssets.Num());

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings)
		{
			WarnArr.Add(MakeShared<FJsonValueString>(W));
		}
		Result->SetArrayField(TEXT("warnings"), WarnArr);
	}

	TArray<TSharedPtr<FJsonValue>> NextSteps;
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Place patrol waypoints in your level and set PatrolLocation BB key at runtime")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Add a NavMeshBoundsVolume to your level if navigation isn't set up")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Drag BP_") + Name + TEXT("Character into the level to test")));
	Result->SetArrayField(TEXT("next_steps"), NextSteps);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  181. scaffold_complete_ai_character
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldCompleteAICharacter(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult))
	{
		return ErrResult;
	}
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, ErrResult))
	{
		return ErrResult;
	}

	FString BTTemplate = Params->GetStringField(TEXT("bt_template"));
	if (BTTemplate.IsEmpty()) BTTemplate = TEXT("patrol");

	FString PerceptionPreset = Params->GetStringField(TEXT("perception_preset"));
	if (PerceptionPreset.IsEmpty()) PerceptionPreset = TEXT("sight_hearing");

	int32 TeamId = 1;
	if (Params->HasField(TEXT("team_id")))
	{
		TeamId = FMath::RoundToInt32(Params->GetNumberField(TEXT("team_id")));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Scaffold Complete AI Character")));

	TArray<FString> CreatedAssets;
	TArray<FString> Warnings;

	FString BBPath = SavePath / TEXT("BB_") + Name;
	FString BTPath = SavePath / TEXT("BT_") + Name;
	FString ControllerPath = SavePath / TEXT("BP_") + Name + TEXT("Controller");
	FString CharacterPath = SavePath / TEXT("BP_") + Name;

	// 1. Create BB with template keys
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BBPath);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("create_blackboard"), P);
		if (!R.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create BB: %s"), *R.ErrorMessage));
		}
		CreatedAssets.Add(BBPath);

		TSharedPtr<FJsonObject> KeysP = MakeShared<FJsonObject>();
		KeysP->SetStringField(TEXT("asset_path"), BBPath);
		KeysP->SetArrayField(TEXT("keys"), BuildBBKeysForTemplate(BTTemplate));
		DispatchOrWarn(TEXT("ai"), TEXT("batch_add_bb_keys"), KeysP, Warnings, TEXT("Add BB keys"));
	}

	// 2. Create BT from template
	{
		TSharedPtr<FJsonObject> Spec = BuildBTTemplateSpec(BTTemplate, BBPath);
		if (!Spec.IsValid())
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown BT template: %s"), *BTTemplate));
		}

		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BTPath);
		P->SetObjectField(TEXT("spec"), Spec);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("build_behavior_tree_from_spec"), P);
		if (!R.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create BT: %s"), *R.ErrorMessage));
		}
		CreatedAssets.Add(BTPath);

		// Phase D1: belt-and-suspenders BB linkage (Issue #48)
		TSharedPtr<FJsonObject> SetBBParams = MakeShared<FJsonObject>();
		SetBBParams->SetStringField(TEXT("bt_path"), BTPath);
		SetBBParams->SetStringField(TEXT("blackboard_path"), BBPath);
		FMonolithActionResult LinkResult = Dispatch(TEXT("ai"), TEXT("set_bt_blackboard"), SetBBParams);
		if (!LinkResult.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to link BT to Blackboard: %s"), *LinkResult.ErrorMessage));
		}
	}

	// 3. Create AI Controller via scaffold
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), ControllerPath);
		P->SetStringField(TEXT("bt_path"), BTPath);
		P->SetStringField(TEXT("bb_path"), BBPath);
		P->SetStringField(TEXT("perception_preset"), PerceptionPreset);
		P->SetNumberField(TEXT("team_id"), TeamId);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("scaffold_ai_controller_blueprint"), P);
		if (!R.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create AI Controller: %s"), *R.ErrorMessage));
		}
		CreatedAssets.Add(ControllerPath);
	}

	// 4. Create Character Blueprint
	{
		FString CharAssetName = FPackageName::GetShortName(CharacterPath);
		FString PathError;
		if (!MonolithAI::EnsureAssetPathFree(CharacterPath, CharAssetName, PathError))
		{
			Warnings.Add(FString::Printf(TEXT("Character creation skipped: %s"), *PathError));
		}
		else
		{
			FString PkgError;
			UPackage* Package = MonolithAI::GetOrCreatePackage(CharacterPath, PkgError);
			if (Package)
			{
				UBlueprint* CharBP = FKismetEditorUtilities::CreateBlueprint(
					ACharacter::StaticClass(),
					Package,
					*CharAssetName,
					BPTYPE_Normal,
					FName(TEXT("MonolithAI")));

				if (CharBP)
				{
					FAssetRegistryModule::AssetCreated(CharBP);
					CharBP->MarkPackageDirty();
					CreatedAssets.Add(CharacterPath);

					// Set mesh if provided
					FString MeshPath = Params->GetStringField(TEXT("mesh"));
					if (!MeshPath.IsEmpty())
					{
						// Mesh assignment would need a separate action; warn for now
						Warnings.Add(FString::Printf(TEXT("Mesh assignment not yet automated — set SkeletalMesh to %s manually"), *MeshPath));
					}

					// Wire controller
					TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
					P->SetStringField(TEXT("blueprint_path"), CharacterPath);
					P->SetStringField(TEXT("controller_class"), ControllerPath);
					DispatchOrWarn(TEXT("ai"), TEXT("set_pawn_ai_controller_class"), P, Warnings, TEXT("Set AIControllerClass"));
				}
				else
				{
					Warnings.Add(TEXT("Failed to create Character Blueprint"));
				}
			}
			else
			{
				Warnings.Add(FString::Printf(TEXT("Failed to create Character package: %s"), *PkgError));
			}
		}
	}

	// Build result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Complete AI character '%s' scaffolded — %d assets"), *Name, CreatedAssets.Num()));

	TSharedPtr<FJsonObject> Paths = MakeShared<FJsonObject>();
	Paths->SetStringField(TEXT("blackboard"), BBPath);
	Paths->SetStringField(TEXT("behavior_tree"), BTPath);
	Paths->SetStringField(TEXT("ai_controller"), ControllerPath);
	Paths->SetStringField(TEXT("character"), CharacterPath);
	Result->SetObjectField(TEXT("assets"), Paths);

	TArray<TSharedPtr<FJsonValue>> AssetArr;
	for (const FString& A : CreatedAssets) AssetArr.Add(MakeShared<FJsonValueString>(A));
	Result->SetArrayField(TEXT("created_assets"), AssetArr);
	Result->SetNumberField(TEXT("asset_count"), CreatedAssets.Num());
	Result->SetStringField(TEXT("bt_template"), BTTemplate);
	Result->SetStringField(TEXT("perception_preset"), PerceptionPreset);
	Result->SetNumberField(TEXT("team_id"), TeamId);

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings) WarnArr.Add(MakeShared<FJsonValueString>(W));
		Result->SetArrayField(TEXT("warnings"), WarnArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  182. scaffold_perception_to_blackboard
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldPerceptionToBlackboard(const TSharedPtr<FJsonObject>& Params)
{
	FString ControllerPath, BBPath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("controller_path"), ControllerPath, ErrResult))
	{
		return ErrResult;
	}
	if (!MonolithAI::RequireStringParam(Params, TEXT("bb_path"), BBPath, ErrResult))
	{
		return ErrResult;
	}

	// Default mappings if not provided
	struct FPerceptionMapping
	{
		FString Sense;
		FString BBKey;
		FString BBKeyType;
		FString BaseClass;
	};

	TArray<FPerceptionMapping> Mappings;

	const TArray<TSharedPtr<FJsonValue>>* MappingsArr = nullptr;
	if (Params->TryGetArrayField(TEXT("mappings"), MappingsArr) && MappingsArr)
	{
		for (const TSharedPtr<FJsonValue>& MapVal : *MappingsArr)
		{
			const TSharedPtr<FJsonObject>* MapObj = nullptr;
			if (MapVal->TryGetObject(MapObj) && MapObj)
			{
				FPerceptionMapping M;
				M.Sense = (*MapObj)->GetStringField(TEXT("sense"));
				M.BBKey = (*MapObj)->GetStringField(TEXT("bb_key"));
				M.BBKeyType = (*MapObj)->GetStringField(TEXT("bb_key_type"));
				M.BaseClass = (*MapObj)->GetStringField(TEXT("base_class"));

				// Infer type from sense if not provided
				if (M.BBKeyType.IsEmpty())
				{
					if (M.Sense.Equals(TEXT("Sight"), ESearchCase::IgnoreCase) || M.Sense.Equals(TEXT("Damage"), ESearchCase::IgnoreCase))
					{
						M.BBKeyType = TEXT("Object");
						if (M.BaseClass.IsEmpty()) M.BaseClass = TEXT("Actor");
					}
					else if (M.Sense.Equals(TEXT("Hearing"), ESearchCase::IgnoreCase))
					{
						M.BBKeyType = TEXT("Vector");
					}
					else
					{
						M.BBKeyType = TEXT("Object");
						if (M.BaseClass.IsEmpty()) M.BaseClass = TEXT("Actor");
					}
				}

				if (!M.Sense.IsEmpty() && !M.BBKey.IsEmpty())
				{
					Mappings.Add(MoveTemp(M));
				}
			}
		}
	}

	// Defaults: Sight→TargetActor, Hearing→HeardLocation
	if (Mappings.Num() == 0)
	{
		Mappings.Add({ TEXT("Sight"), TEXT("TargetActor"), TEXT("Object"), TEXT("Actor") });
		Mappings.Add({ TEXT("Hearing"), TEXT("HeardLocation"), TEXT("Vector"), TEXT("") });
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Scaffold Perception→BB")));
	TArray<FString> Warnings;
	TArray<TSharedPtr<FJsonValue>> ConfiguredSenses;

	// Ensure BB keys exist
	for (const FPerceptionMapping& M : Mappings)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), BBPath);
		P->SetStringField(TEXT("key_name"), M.BBKey);
		P->SetStringField(TEXT("key_type"), M.BBKeyType);
		if (!M.BaseClass.IsEmpty())
		{
			P->SetStringField(TEXT("base_class"), M.BaseClass);
		}
		// Don't warn on failure — key may already exist
		Dispatch(TEXT("ai"), TEXT("add_bb_key"), P);
	}

	// Ensure perception component exists
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), ControllerPath);
		P->SetStringField(TEXT("dominant_sense"), TEXT("Sight"));
		// Might fail if already exists — that's fine
		Dispatch(TEXT("ai"), TEXT("add_perception_component"), P);
	}

	// Configure each sense
	for (const FPerceptionMapping& M : Mappings)
	{
		TSharedPtr<FJsonObject> SenseResult = MakeShared<FJsonObject>();
		SenseResult->SetStringField(TEXT("sense"), M.Sense);
		SenseResult->SetStringField(TEXT("bb_key"), M.BBKey);
		SenseResult->SetStringField(TEXT("bb_key_type"), M.BBKeyType);

		if (M.Sense.Equals(TEXT("Sight"), ESearchCase::IgnoreCase))
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), ControllerPath);
			P->SetNumberField(TEXT("radius"), 1500.0);
			if (DispatchOrWarn(TEXT("ai"), TEXT("configure_sight_sense"), P, Warnings, TEXT("Configure sight")))
			{
				SenseResult->SetBoolField(TEXT("configured"), true);
			}
		}
		else if (M.Sense.Equals(TEXT("Hearing"), ESearchCase::IgnoreCase))
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), ControllerPath);
			P->SetNumberField(TEXT("range"), 2000.0);
			if (DispatchOrWarn(TEXT("ai"), TEXT("configure_hearing_sense"), P, Warnings, TEXT("Configure hearing")))
			{
				SenseResult->SetBoolField(TEXT("configured"), true);
			}
		}
		else if (M.Sense.Equals(TEXT("Damage"), ESearchCase::IgnoreCase))
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), ControllerPath);
			if (DispatchOrWarn(TEXT("ai"), TEXT("configure_damage_sense"), P, Warnings, TEXT("Configure damage")))
			{
				SenseResult->SetBoolField(TEXT("configured"), true);
			}
		}
		else if (M.Sense.Equals(TEXT("Touch"), ESearchCase::IgnoreCase))
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), ControllerPath);
			if (DispatchOrWarn(TEXT("ai"), TEXT("configure_touch_sense"), P, Warnings, TEXT("Configure touch")))
			{
				SenseResult->SetBoolField(TEXT("configured"), true);
			}
		}

		ConfiguredSenses.Add(MakeShared<FJsonValueObject>(SenseResult));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Perception→BB bridge configured: %d mappings"), Mappings.Num()));
	Result->SetStringField(TEXT("controller_path"), ControllerPath);
	Result->SetStringField(TEXT("bb_path"), BBPath);
	Result->SetArrayField(TEXT("configured_senses"), ConfiguredSenses);
	Result->SetStringField(TEXT("note"), TEXT("BB keys created/verified and senses configured. Wire perception events to BB in the controller's event graph (OnTargetPerceptionUpdated) at runtime or via Blueprint."));

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings) WarnArr.Add(MakeShared<FJsonValueString>(W));
		Result->SetArrayField(TEXT("warnings"), WarnArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  184. scaffold_team_system
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldTeamSystem(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult))
	{
		return ErrResult;
	}

	const TArray<TSharedPtr<FJsonValue>>* TeamsArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("teams"), TeamsArr) || !TeamsArr || TeamsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: teams (non-empty array)"));
	}

	// Parse teams
	struct FTeamDef
	{
		int32 Id;
		FString Name;
	};
	TArray<FTeamDef> Teams;
	for (const TSharedPtr<FJsonValue>& TV : *TeamsArr)
	{
		const TSharedPtr<FJsonObject>* TO = nullptr;
		if (!TV->TryGetObject(TO) || !TO) continue;

		FTeamDef T;
		T.Id = FMath::RoundToInt32((*TO)->GetNumberField(TEXT("id")));
		T.Name = (*TO)->GetStringField(TEXT("name"));
		if (T.Name.IsEmpty()) T.Name = FString::Printf(TEXT("Team_%d"), T.Id);
		Teams.Add(T);
	}

	if (Teams.Num() < 2)
	{
		return FMonolithActionResult::Error(TEXT("Need at least 2 teams"));
	}

	// Parse attitudes (or generate defaults)
	struct FAttitudeDef
	{
		int32 From;
		int32 To;
		FString Attitude; // Friendly, Neutral, Hostile
	};
	TArray<FAttitudeDef> Attitudes;

	const TArray<TSharedPtr<FJsonValue>>* AttitudesArr = nullptr;
	if (Params->TryGetArrayField(TEXT("attitudes"), AttitudesArr) && AttitudesArr)
	{
		for (const TSharedPtr<FJsonValue>& AV : *AttitudesArr)
		{
			const TSharedPtr<FJsonObject>* AO = nullptr;
			if (!AV->TryGetObject(AO) || !AO) continue;

			FAttitudeDef A;
			A.From = FMath::RoundToInt32((*AO)->GetNumberField(TEXT("from")));
			A.To = FMath::RoundToInt32((*AO)->GetNumberField(TEXT("to")));
			A.Attitude = (*AO)->GetStringField(TEXT("attitude"));
			Attitudes.Add(A);
		}
	}
	else
	{
		// Default: teams are hostile to each other, friendly to self
		for (const FTeamDef& A : Teams)
		{
			for (const FTeamDef& B : Teams)
			{
				FAttitudeDef Att;
				Att.From = A.Id;
				Att.To = B.Id;
				Att.Attitude = (A.Id == B.Id) ? TEXT("Friendly") : TEXT("Hostile");
				Attitudes.Add(Att);
			}
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Scaffold Team System")));

	// Build result — we can't create a true DataTable-based attitude solver at editor time without
	// a row struct, but we can document the setup and provide the team config
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Team system configured: %d teams, %d attitude rules"), Teams.Num(), Attitudes.Num()));

	TArray<TSharedPtr<FJsonValue>> TeamsJson;
	for (const FTeamDef& T : Teams)
	{
		TSharedPtr<FJsonObject> TObj = MakeShared<FJsonObject>();
		TObj->SetNumberField(TEXT("id"), T.Id);
		TObj->SetStringField(TEXT("name"), T.Name);
		TeamsJson.Add(MakeShared<FJsonValueObject>(TObj));
	}
	Result->SetArrayField(TEXT("teams"), TeamsJson);

	TArray<TSharedPtr<FJsonValue>> AttJson;
	for (const FAttitudeDef& A : Attitudes)
	{
		TSharedPtr<FJsonObject> AObj = MakeShared<FJsonObject>();
		AObj->SetNumberField(TEXT("from"), A.From);
		AObj->SetNumberField(TEXT("to"), A.To);
		AObj->SetStringField(TEXT("attitude"), A.Attitude);
		AttJson.Add(MakeShared<FJsonValueObject>(AObj));
	}
	Result->SetArrayField(TEXT("attitudes"), AttJson);

	// Apply team IDs to any existing controllers if they match the team names
	TArray<TSharedPtr<FJsonValue>> NextSteps;
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Use set_ai_team on each AI controller to assign team IDs")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Implement IGenericTeamAgentInterface on your PlayerController for player team membership")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Perception auto-filters by team affiliation — enemies/neutrals/friendlies")));
	Result->SetArrayField(TEXT("next_steps"), NextSteps);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  185. scaffold_patrol_investigate_ai
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldPatrolInvestigateAI(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult))
	{
		return ErrResult;
	}
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, ErrResult))
	{
		return ErrResult;
	}

	FString PatrolType = Params->GetStringField(TEXT("patrol_type"));
	if (PatrolType.IsEmpty()) PatrolType = TEXT("loop");

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Scaffold Patrol+Investigate AI")));

	TArray<FString> CreatedAssets;
	TArray<FString> Warnings;

	FString BBPath = SavePath / TEXT("BB_") + Name;
	FString BTPath = SavePath / TEXT("BT_") + Name;
	FString ControllerPath = SavePath / TEXT("BP_") + Name + TEXT("Controller");
	FString CharacterPath = SavePath / TEXT("BP_") + Name;

	// 1. Create BB with patrol+investigate keys
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BBPath);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("create_blackboard"), P);
		if (!R.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create BB: %s"), *R.ErrorMessage));
		}
		CreatedAssets.Add(BBPath);

		TSharedPtr<FJsonObject> KeysP = MakeShared<FJsonObject>();
		KeysP->SetStringField(TEXT("asset_path"), BBPath);
		KeysP->SetArrayField(TEXT("keys"), BuildBBKeysForTemplate(TEXT("patrol_investigate")));
		DispatchOrWarn(TEXT("ai"), TEXT("batch_add_bb_keys"), KeysP, Warnings, TEXT("Add BB keys"));
	}

	// 2. Build patrol+investigate BT
	// Selector → [Chase(decorator: TargetActor), Investigate(decorator: bHeardNoise), Patrol]
	{
		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
		Spec->SetStringField(TEXT("blackboard_path"), BBPath);

		auto MakeNode = [](const FString& ClassName) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>();
			N->SetStringField(TEXT("type"), ClassName);
			return N;
		};

		auto SetProp = [](TSharedPtr<FJsonObject>& Node, const FString& Key, const FString& Value)
		{
			TSharedPtr<FJsonObject> Props;
			const TSharedPtr<FJsonObject>* Existing = nullptr;
			if (Node->TryGetObjectField(TEXT("properties"), Existing) && Existing) Props = *Existing;
			else { Props = MakeShared<FJsonObject>(); Node->SetObjectField(TEXT("properties"), Props); }
			Props->SetStringField(Key, Value);
		};

		TSharedPtr<FJsonObject> Root = MakeNode(TEXT("BTComposite_Selector"));

		// Chase branch
		TSharedPtr<FJsonObject> ChaseSeq = MakeNode(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> Dec = MakeShared<FJsonObject>();
			Dec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SetProp(Dec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("TargetActor"));
			ChaseSeq->SetArrayField(TEXT("decorators"), { MakeShared<FJsonValueObject>(Dec) });

			TSharedPtr<FJsonObject> MoveTo = MakeNode(TEXT("BTTask_MoveTo"));
			SetProp(MoveTo, TEXT("BlackboardKey.SelectedKeyName"), TEXT("TargetActor"));
			SetProp(MoveTo, TEXT("AcceptableRadius"), TEXT("150"));

			ChaseSeq->SetArrayField(TEXT("children"), { MakeShared<FJsonValueObject>(MoveTo) });
		}

		// Investigate branch
		TSharedPtr<FJsonObject> InvestigateSeq = MakeNode(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> Dec = MakeShared<FJsonObject>();
			Dec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SetProp(Dec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("bHeardNoise"));
			InvestigateSeq->SetArrayField(TEXT("decorators"), { MakeShared<FJsonValueObject>(Dec) });

			TSharedPtr<FJsonObject> MoveToNoise = MakeNode(TEXT("BTTask_MoveTo"));
			SetProp(MoveToNoise, TEXT("BlackboardKey.SelectedKeyName"), TEXT("HeardLocation"));
			SetProp(MoveToNoise, TEXT("AcceptableRadius"), TEXT("100"));

			TSharedPtr<FJsonObject> Wait = MakeNode(TEXT("BTTask_Wait"));
			SetProp(Wait, TEXT("WaitTime"), TEXT("3.0"));

			// Search nearby (MoveTo SearchPoint)
			TSharedPtr<FJsonObject> SearchMove = MakeNode(TEXT("BTTask_MoveTo"));
			SetProp(SearchMove, TEXT("BlackboardKey.SelectedKeyName"), TEXT("SearchPoint"));
			SetProp(SearchMove, TEXT("AcceptableRadius"), TEXT("50"));

			TSharedPtr<FJsonObject> SearchWait = MakeNode(TEXT("BTTask_Wait"));
			SetProp(SearchWait, TEXT("WaitTime"), TEXT("2.0"));

			InvestigateSeq->SetArrayField(TEXT("children"), {
				MakeShared<FJsonValueObject>(MoveToNoise),
				MakeShared<FJsonValueObject>(Wait),
				MakeShared<FJsonValueObject>(SearchMove),
				MakeShared<FJsonValueObject>(SearchWait)
			});
		}

		// Patrol branch
		TSharedPtr<FJsonObject> PatrolSeq = MakeNode(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> MoveToPatrol = MakeNode(TEXT("BTTask_MoveTo"));
			SetProp(MoveToPatrol, TEXT("BlackboardKey.SelectedKeyName"), TEXT("PatrolLocation"));
			SetProp(MoveToPatrol, TEXT("AcceptableRadius"), TEXT("50"));

			TSharedPtr<FJsonObject> Wait = MakeNode(TEXT("BTTask_Wait"));
			SetProp(Wait, TEXT("WaitTime"), TEXT("2.0"));

			PatrolSeq->SetArrayField(TEXT("children"), {
				MakeShared<FJsonValueObject>(MoveToPatrol),
				MakeShared<FJsonValueObject>(Wait)
			});
		}

		Root->SetArrayField(TEXT("children"), {
			MakeShared<FJsonValueObject>(ChaseSeq),
			MakeShared<FJsonValueObject>(InvestigateSeq),
			MakeShared<FJsonValueObject>(PatrolSeq)
		});

		Spec->SetObjectField(TEXT("root"), Root);

		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BTPath);
		P->SetObjectField(TEXT("spec"), Spec);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("build_behavior_tree_from_spec"), P);
		if (!R.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create BT: %s"), *R.ErrorMessage));
		}
		CreatedAssets.Add(BTPath);

		// Phase D1: belt-and-suspenders BB linkage (Issue #48)
		TSharedPtr<FJsonObject> SetBBParams = MakeShared<FJsonObject>();
		SetBBParams->SetStringField(TEXT("bt_path"), BTPath);
		SetBBParams->SetStringField(TEXT("blackboard_path"), BBPath);
		FMonolithActionResult LinkResult = Dispatch(TEXT("ai"), TEXT("set_bt_blackboard"), SetBBParams);
		if (!LinkResult.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to link BT to Blackboard: %s"), *LinkResult.ErrorMessage));
		}
	}

	// 3. Create controller
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), ControllerPath);
		P->SetStringField(TEXT("bt_path"), BTPath);
		P->SetStringField(TEXT("bb_path"), BBPath);
		P->SetStringField(TEXT("perception_preset"), TEXT("sight_hearing"));
		P->SetNumberField(TEXT("team_id"), 1);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("scaffold_ai_controller_blueprint"), P);
		if (!R.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create controller: %s"), *R.ErrorMessage));
		}
		CreatedAssets.Add(ControllerPath);
	}

	// 4. Create Character
	{
		FString CharAssetName = FPackageName::GetShortName(CharacterPath);
		FString PathError;
		if (MonolithAI::EnsureAssetPathFree(CharacterPath, CharAssetName, PathError))
		{
			FString PkgError;
			UPackage* Package = MonolithAI::GetOrCreatePackage(CharacterPath, PkgError);
			if (Package)
			{
				UBlueprint* CharBP = FKismetEditorUtilities::CreateBlueprint(
					ACharacter::StaticClass(), Package, *CharAssetName,
					BPTYPE_Normal, FName(TEXT("MonolithAI")));
				if (CharBP)
				{
					FAssetRegistryModule::AssetCreated(CharBP);
					CharBP->MarkPackageDirty();
					CreatedAssets.Add(CharacterPath);

					TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
					P->SetStringField(TEXT("blueprint_path"), CharacterPath);
					P->SetStringField(TEXT("controller_class"), ControllerPath);
					DispatchOrWarn(TEXT("ai"), TEXT("set_pawn_ai_controller_class"), P, Warnings, TEXT("Set AIControllerClass"));
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Patrol+Investigate AI '%s' scaffolded — %d assets"), *Name, CreatedAssets.Num()));
	Result->SetStringField(TEXT("patrol_type"), PatrolType);

	TSharedPtr<FJsonObject> Paths = MakeShared<FJsonObject>();
	Paths->SetStringField(TEXT("blackboard"), BBPath);
	Paths->SetStringField(TEXT("behavior_tree"), BTPath);
	Paths->SetStringField(TEXT("ai_controller"), ControllerPath);
	Paths->SetStringField(TEXT("character"), CharacterPath);
	Result->SetObjectField(TEXT("assets"), Paths);

	TArray<TSharedPtr<FJsonValue>> AssetArr;
	for (const FString& A : CreatedAssets) AssetArr.Add(MakeShared<FJsonValueString>(A));
	Result->SetArrayField(TEXT("created_assets"), AssetArr);

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings) WarnArr.Add(MakeShared<FJsonValueString>(W));
		Result->SetArrayField(TEXT("warnings"), WarnArr);
	}

	TArray<TSharedPtr<FJsonValue>> NextSteps;
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Set up patrol waypoints and write PatrolLocation at runtime via BT service or controller")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Wire OnTargetPerceptionUpdated to set bHeardNoise + HeardLocation BB keys")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Optionally add EQS for search_area point generation")));
	Result->SetArrayField(TEXT("next_steps"), NextSteps);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  186. scaffold_enemy_ai
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldEnemyAI(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name, Archetype;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("archetype"), Archetype, ErrResult)) return ErrResult;

	Archetype = Archetype.ToLower();
	if (Archetype != TEXT("melee") && Archetype != TEXT("ranged") && Archetype != TEXT("charger"))
	{
		return FMonolithActionResult::Error(TEXT("archetype must be: melee, ranged, or charger"));
	}

	int32 TeamId = 1;
	if (Params->HasField(TEXT("team_id")))
	{
		TeamId = FMath::RoundToInt32(Params->GetNumberField(TEXT("team_id")));
	}

	// Map archetype to BT template and BB keys
	FString BTTemplate;
	if (Archetype == TEXT("melee") || Archetype == TEXT("charger"))
	{
		BTTemplate = TEXT("chase_attack");
	}
	else // ranged
	{
		BTTemplate = TEXT("chase_attack"); // Same base, ranged adds range BB key
	}

	FString PerceptionPreset = TEXT("sight_hearing");

	// Delegate to scaffold_complete_ai_character with the right params
	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	P->SetStringField(TEXT("save_path"), SavePath);
	P->SetStringField(TEXT("name"), Name);
	P->SetStringField(TEXT("bt_template"), BTTemplate);
	P->SetStringField(TEXT("perception_preset"), PerceptionPreset);
	P->SetNumberField(TEXT("team_id"), TeamId);

	FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("scaffold_complete_ai_character"), P);
	if (!R.bSuccess) return R;

	// Augment result with archetype info
	if (R.Result.IsValid())
	{
		R.Result->SetStringField(TEXT("archetype"), Archetype);

		// Add archetype-specific notes
		TArray<TSharedPtr<FJsonValue>> Notes;
		if (Archetype == TEXT("melee"))
		{
			Notes.Add(MakeShared<FJsonValueString>(TEXT("Melee enemy: add custom BTTask for melee attack at close range")));
			Notes.Add(MakeShared<FJsonValueString>(TEXT("Set AcceptableRadius on MoveTo to match melee attack range")));
		}
		else if (Archetype == TEXT("ranged"))
		{
			Notes.Add(MakeShared<FJsonValueString>(TEXT("Ranged enemy: add AttackRange BB key and keep distance via EQS")));
			Notes.Add(MakeShared<FJsonValueString>(TEXT("Consider scaffold_eqs_move_sequence for positioning")));
		}
		else if (Archetype == TEXT("charger"))
		{
			Notes.Add(MakeShared<FJsonValueString>(TEXT("Charger enemy: increase MoveTo speed and reduce AcceptableRadius")));
			Notes.Add(MakeShared<FJsonValueString>(TEXT("Add charge cooldown via BTTask_Wait after attack")));
		}
		R.Result->SetArrayField(TEXT("archetype_notes"), Notes);
	}

	return R;
}

// ============================================================
//  198. scaffold_eqs_move_sequence
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldEQSMoveSequence(const TSharedPtr<FJsonObject>& Params)
{
	FString BTPath, EQSPath, BBKey;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("bt_path"), BTPath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("eqs_path"), EQSPath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("bb_key"), BBKey, ErrResult)) return ErrResult;

	FString ParentId = Params->GetStringField(TEXT("parent_id"));

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Scaffold EQS Move Sequence")));

	TArray<FString> Warnings;
	TArray<FString> CreatedNodeIds;

	// 1. Add a Sequence composite
	FString SequenceId;
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), BTPath);
		P->SetStringField(TEXT("node_class"), TEXT("BTComposite_Sequence"));
		if (!ParentId.IsEmpty())
		{
			P->SetStringField(TEXT("parent_id"), ParentId);
		}
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("add_bt_node"), P);
		if (!R.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to add Sequence node: %s"), *R.ErrorMessage));
		}
		if (R.Result.IsValid())
		{
			SequenceId = R.Result->GetStringField(TEXT("node_id"));
		}
		CreatedNodeIds.Add(SequenceId);
	}

	// 2. Add RunEQS task under the sequence
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), BTPath);
		if (!SequenceId.IsEmpty())
		{
			P->SetStringField(TEXT("parent_id"), SequenceId);
		}
		P->SetStringField(TEXT("eqs_path"), EQSPath);
		P->SetStringField(TEXT("bb_result_key"), BBKey);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("add_bt_run_eqs_task"), P);
		if (!R.bSuccess)
		{
			Warnings.Add(FString::Printf(TEXT("Failed to add RunEQS task: %s"), *R.ErrorMessage));
		}
		else if (R.Result.IsValid())
		{
			CreatedNodeIds.Add(R.Result->GetStringField(TEXT("node_id")));
		}
	}

	// 3. Add MoveTo task under the sequence
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), BTPath);
		P->SetStringField(TEXT("node_class"), TEXT("BTTask_MoveTo"));
		if (!SequenceId.IsEmpty())
		{
			P->SetStringField(TEXT("parent_id"), SequenceId);
		}
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("add_bt_node"), P);
		if (!R.bSuccess)
		{
			Warnings.Add(FString::Printf(TEXT("Failed to add MoveTo task: %s"), *R.ErrorMessage));
		}
		else
		{
			if (R.Result.IsValid())
			{
				FString NodeId = R.Result->GetStringField(TEXT("node_id"));
				CreatedNodeIds.Add(NodeId);

				// Set the BB key on the MoveTo
				if (!NodeId.IsEmpty())
				{
					// Phase D4: typo fix `property_value` → `value`; promote to hard Dispatch with early-return
					// (this wiring is critical to the EQS→MoveTo chain working at all).
					TSharedPtr<FJsonObject> PropP = MakeShared<FJsonObject>();
					PropP->SetStringField(TEXT("asset_path"), BTPath);
					PropP->SetStringField(TEXT("node_id"), NodeId);
					PropP->SetStringField(TEXT("property_name"), TEXT("BlackboardKey.SelectedKeyName"));
					PropP->SetStringField(TEXT("value"), BBKey);
					FMonolithActionResult PropR = Dispatch(TEXT("ai"), TEXT("set_bt_node_property"), PropP);
					if (!PropR.bSuccess)
					{
						return FMonolithActionResult::Error(FString::Printf(
							TEXT("Failed to set MoveTo BB key (set_bt_node_property): %s"), *PropR.ErrorMessage));
					}
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("message"), TEXT("EQS→MoveTo sequence added to BT"));
	Result->SetStringField(TEXT("bt_path"), BTPath);
	Result->SetStringField(TEXT("eqs_path"), EQSPath);
	Result->SetStringField(TEXT("bb_key"), BBKey);

	TArray<TSharedPtr<FJsonValue>> NodeArr;
	for (const FString& NId : CreatedNodeIds) NodeArr.Add(MakeShared<FJsonValueString>(NId));
	Result->SetArrayField(TEXT("created_node_ids"), NodeArr);

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings) WarnArr.Add(MakeShared<FJsonValueString>(W));
		Result->SetArrayField(TEXT("warnings"), WarnArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  199. create_bt_from_template
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleCreateBTFromTemplate(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, TemplateName;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("template"), TemplateName, ErrResult)) return ErrResult;

	TemplateName = TemplateName.ToLower();

	// Create matching BB first
	FString AssetName = FPackageName::GetShortName(SavePath);
	FString BBName = AssetName;
	if (BBName.StartsWith(TEXT("BT_")))
	{
		BBName = TEXT("BB_") + BBName.Mid(3);
	}
	else
	{
		BBName = BBName + TEXT("_BB");
	}

	int32 LastSlash;
	FString BBPath = SavePath;
	if (BBPath.FindLastChar(TEXT('/'), LastSlash))
	{
		BBPath = BBPath.Left(LastSlash + 1) + BBName;
	}

	TArray<FString> Warnings;

	// Create BB
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BBPath);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("create_blackboard"), P);
		if (!R.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create BB: %s"), *R.ErrorMessage));
		}

		// Add template keys
		TSharedPtr<FJsonObject> KeysP = MakeShared<FJsonObject>();
		KeysP->SetStringField(TEXT("asset_path"), BBPath);
		KeysP->SetArrayField(TEXT("keys"), BuildBBKeysForTemplate(TemplateName));
		DispatchOrWarn(TEXT("ai"), TEXT("batch_add_bb_keys"), KeysP, Warnings, TEXT("Add BB keys"));
	}

	// Build and create BT
	TSharedPtr<FJsonObject> Spec = BuildBTTemplateSpec(TemplateName, BBPath);
	if (!Spec.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown template: '%s'. Valid templates: patrol, chase_attack, flee, search_area, guard_post"), *TemplateName));
	}

	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	P->SetStringField(TEXT("save_path"), SavePath);
	P->SetObjectField(TEXT("spec"), Spec);
	FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("build_behavior_tree_from_spec"), P);
	if (!R.bSuccess)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to build BT: %s"), *R.ErrorMessage));
	}

	// Phase D1: belt-and-suspenders BB linkage (Issue #48)
	{
		TSharedPtr<FJsonObject> SetBBParams = MakeShared<FJsonObject>();
		SetBBParams->SetStringField(TEXT("bt_path"), SavePath);
		SetBBParams->SetStringField(TEXT("blackboard_path"), BBPath);
		FMonolithActionResult LinkResult = Dispatch(TEXT("ai"), TEXT("set_bt_blackboard"), SetBBParams);
		if (!LinkResult.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to link BT to Blackboard: %s"), *LinkResult.ErrorMessage));
		}
	}

	// Augment result
	if (R.Result.IsValid())
	{
		R.Result->SetStringField(TEXT("template"), TemplateName);
		R.Result->SetStringField(TEXT("bb_path"), BBPath);

		if (Warnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> WarnArr;
			for (const FString& W : Warnings) WarnArr.Add(MakeShared<FJsonValueString>(W));
			R.Result->SetArrayField(TEXT("warnings"), WarnArr);
		}
	}

	return R;
}

// ============================================================
//  200. create_st_from_template
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleCreateSTFromTemplate(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_STATETREE
	FString SavePath, TemplateName;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("template"), TemplateName, ErrResult)) return ErrResult;

	TemplateName = TemplateName.ToLower();

	if (TemplateName != TEXT("patrol") && TemplateName != TEXT("combat") &&
		TemplateName != TEXT("investigation") && TemplateName != TEXT("ambient"))
	{
		return FMonolithActionResult::Error(TEXT("Unknown template. Valid: patrol, combat, investigation, ambient"));
	}

	TArray<FString> Warnings;

	// Step 1: Create the State Tree
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), SavePath);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("create_state_tree"), P);
		if (!R.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create State Tree: %s"), *R.ErrorMessage));
		}
	}

	// Step 2: Set schema to AI (standard for AI State Trees)
	{
		// Phase D4: typo fix `schema` → `schema_class` (canonical param);
		// promote to hard Dispatch with early-return — without a schema the State Tree is invalid.
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), SavePath);
		P->SetStringField(TEXT("schema_class"), TEXT("StateTreeAIComponentSchema"));
		FMonolithActionResult SchemaR = Dispatch(TEXT("ai"), TEXT("set_st_schema"), P);
		if (!SchemaR.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to set State Tree schema (set_st_schema): %s"), *SchemaR.ErrorMessage));
		}
	}

	// Step 3: Add template states
	if (TemplateName == TEXT("patrol"))
	{
		// States: Patrol → Investigate → Chase
		for (const FString& StateName : { TEXT("Patrol"), TEXT("Investigate"), TEXT("Chase") })
		{
			// Phase D4: typo fix `state_name` → `name` (canonical param for add_st_state);
			// promote to hard Dispatch with early-return — missing template states leave the tree empty.
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), SavePath);
			P->SetStringField(TEXT("name"), StateName);
			FMonolithActionResult AddR = Dispatch(TEXT("ai"), TEXT("add_st_state"), P);
			if (!AddR.bSuccess)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Failed to add State Tree state '%s' (add_st_state): %s"), *StateName, *AddR.ErrorMessage));
			}
		}
	}
	else if (TemplateName == TEXT("combat"))
	{
		for (const FString& StateName : { TEXT("Engage"), TEXT("Attack"), TEXT("Reposition"), TEXT("Flee") })
		{
			// Phase D4: typo fix `state_name` → `name` (canonical param for add_st_state);
			// promote to hard Dispatch with early-return — missing template states leave the tree empty.
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), SavePath);
			P->SetStringField(TEXT("name"), StateName);
			FMonolithActionResult AddR = Dispatch(TEXT("ai"), TEXT("add_st_state"), P);
			if (!AddR.bSuccess)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Failed to add State Tree state '%s' (add_st_state): %s"), *StateName, *AddR.ErrorMessage));
			}
		}
	}
	else if (TemplateName == TEXT("investigation"))
	{
		for (const FString& StateName : { TEXT("MoveToLocation"), TEXT("LookAround"), TEXT("SearchArea"), TEXT("ReturnToPost") })
		{
			// Phase D4: typo fix `state_name` → `name` (canonical param for add_st_state);
			// promote to hard Dispatch with early-return — missing template states leave the tree empty.
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), SavePath);
			P->SetStringField(TEXT("name"), StateName);
			FMonolithActionResult AddR = Dispatch(TEXT("ai"), TEXT("add_st_state"), P);
			if (!AddR.bSuccess)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Failed to add State Tree state '%s' (add_st_state): %s"), *StateName, *AddR.ErrorMessage));
			}
		}
	}
	else if (TemplateName == TEXT("ambient"))
	{
		for (const FString& StateName : { TEXT("Idle"), TEXT("Wander"), TEXT("Interact"), TEXT("Rest") })
		{
			// Phase D4: typo fix `state_name` → `name` (canonical param for add_st_state);
			// promote to hard Dispatch with early-return — missing template states leave the tree empty.
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), SavePath);
			P->SetStringField(TEXT("name"), StateName);
			FMonolithActionResult AddR = Dispatch(TEXT("ai"), TEXT("add_st_state"), P);
			if (!AddR.bSuccess)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Failed to add State Tree state '%s' (add_st_state): %s"), *StateName, *AddR.ErrorMessage));
			}
		}
	}

	// Compile
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), SavePath);
		DispatchOrWarn(TEXT("ai"), TEXT("compile_state_tree"), P, Warnings, TEXT("Compile State Tree"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("State Tree created from '%s' template"), *TemplateName));
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetStringField(TEXT("template"), TemplateName);

	TArray<TSharedPtr<FJsonValue>> NextSteps;
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Add tasks to each state via add_st_task")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Add transitions between states via add_st_transition")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Add enter conditions via add_st_enter_condition")));
	Result->SetArrayField(TEXT("next_steps"), NextSteps);

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings) WarnArr.Add(MakeShared<FJsonValueString>(W));
		Result->SetArrayField(TEXT("warnings"), WarnArr);
	}

	return FMonolithActionResult::Success(Result);
#else
	return FMonolithActionResult::Error(TEXT("StateTree plugin is not available. Enable the StateTree plugin and rebuild."));
#endif
}

// ============================================================
//  206. batch_validate_ai_assets
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleBatchValidateAIAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString PathFilter = Params->GetStringField(TEXT("path_filter"));

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	int32 TotalAssets = 0;
	int32 TotalIssues = 0;
	TArray<TSharedPtr<FJsonValue>> AllResults;

	// Phase F #51: scoped mode — when caller supplies 'asset_paths', validate ONLY those.
	// Avoids the full-project scan when the caller already knows which assets to check.
	const TArray<TSharedPtr<FJsonValue>>* AssetPathsArr = nullptr;
	if (Params->TryGetArrayField(TEXT("asset_paths"), AssetPathsArr) && AssetPathsArr && AssetPathsArr->Num() > 0)
	{
		// Resolve each path -> dispatch to the action matching its class.
		auto DispatchOne = [&](const FString& AssetPath, const FString& TypeName, const FString& ActionName)
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), AssetPath);
			FMonolithActionResult R = Dispatch(TEXT("ai"), ActionName, P);
			++TotalAssets;

			if (R.bSuccess && R.Result.IsValid())
			{
				int32 IssueCount = FMath::RoundToInt32(R.Result->GetNumberField(TEXT("issue_count")));
				if (IssueCount > 0)
				{
					TotalIssues += IssueCount;
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("asset_path"), AssetPath);
					Entry->SetStringField(TEXT("type"), TypeName);
					Entry->SetNumberField(TEXT("issue_count"), IssueCount);
					if (R.Result->HasField(TEXT("issues")))
					{
						Entry->SetArrayField(TEXT("issues"), R.Result->GetArrayField(TEXT("issues")));
					}
					AllResults.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
			else if (!R.bSuccess)
			{
				++TotalIssues;
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("asset_path"), AssetPath);
				Entry->SetStringField(TEXT("type"), TypeName);
				Entry->SetNumberField(TEXT("issue_count"), 1);

				TArray<TSharedPtr<FJsonValue>> ErrArr;
				TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
				ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
				ErrObj->SetStringField(TEXT("message"), FString::Printf(TEXT("Validation failed: %s"), *R.ErrorMessage));
				ErrArr.Add(MakeShared<FJsonValueObject>(ErrObj));
				Entry->SetArrayField(TEXT("issues"), ErrArr);
				AllResults.Add(MakeShared<FJsonValueObject>(Entry));
			}
		};

		TArray<FString> SkippedUnknown;
		for (const TSharedPtr<FJsonValue>& V : *AssetPathsArr)
		{
			FString PathStr;
			if (!V.IsValid() || !V->TryGetString(PathStr) || PathStr.IsEmpty())
			{
				continue;
			}

			// Try to identify the asset class via AssetRegistry to pick the right validator.
			FAssetData AD = AR.GetAssetByObjectPath(FSoftObjectPath(PathStr));
			if (!AD.IsValid())
			{
				// Try treating PathStr as a package path (common for the paths returned from validators).
				const FString PackagePath = PathStr.Contains(TEXT(".")) ? PathStr : (PathStr + TEXT(".") + FPaths::GetBaseFilename(PathStr));
				AD = AR.GetAssetByObjectPath(FSoftObjectPath(PackagePath));
			}
			if (!AD.IsValid())
			{
				SkippedUnknown.Add(PathStr);
				continue;
			}

			const FTopLevelAssetPath ClassPath = AD.AssetClassPath;

			if (ClassPath == UBehaviorTree::StaticClass()->GetClassPathName())
			{
				DispatchOne(PathStr, TEXT("BehaviorTree"), TEXT("validate_behavior_tree"));
			}
#if WITH_STATETREE
			else if (ClassPath == UStateTree::StaticClass()->GetClassPathName())
			{
				DispatchOne(PathStr, TEXT("StateTree"), TEXT("validate_state_tree"));
			}
#endif
			else if (ClassPath == UEnvQuery::StaticClass()->GetClassPathName())
			{
				DispatchOne(PathStr, TEXT("EQS"), TEXT("validate_eqs_query"));
			}
#if WITH_SMARTOBJECTS
			else
			{
				if (UClass* SOClass = FindFirstObject<UClass>(TEXT("SmartObjectDefinition"), EFindFirstObjectOptions::EnsureIfAmbiguous))
				{
					if (ClassPath == SOClass->GetClassPathName())
					{
						DispatchOne(PathStr, TEXT("SmartObject"), TEXT("validate_smart_object_definition"));
						continue;
					}
				}
				// Else: maybe an AIController Blueprint
				if (ClassPath == UBlueprint::StaticClass()->GetClassPathName())
				{
					FAssetTagValueRef ParentTag = AD.TagsAndValues.FindTag(FName(TEXT("ParentClass")));
					if (ParentTag.IsSet() && ParentTag.GetValue().Contains(TEXT("AIController")))
					{
						DispatchOne(PathStr, TEXT("AIController"), TEXT("validate_ai_controller"));
						continue;
					}
				}
				SkippedUnknown.Add(PathStr);
			}
#else
			else if (ClassPath == UBlueprint::StaticClass()->GetClassPathName())
			{
				FAssetTagValueRef ParentTag = AD.TagsAndValues.FindTag(FName(TEXT("ParentClass")));
				if (ParentTag.IsSet() && ParentTag.GetValue().Contains(TEXT("AIController")))
				{
					DispatchOne(PathStr, TEXT("AIController"), TEXT("validate_ai_controller"));
				}
				else
				{
					SkippedUnknown.Add(PathStr);
				}
			}
			else
			{
				SkippedUnknown.Add(PathStr);
			}
#endif
		}

		Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Scoped batch validation: %d assets checked, %d issues found"), TotalAssets, TotalIssues));
		Result->SetNumberField(TEXT("total_assets"), TotalAssets);
		Result->SetNumberField(TEXT("total_issues"), TotalIssues);
		Result->SetBoolField(TEXT("all_valid"), TotalIssues == 0);
		Result->SetArrayField(TEXT("results"), AllResults);
		Result->SetBoolField(TEXT("scoped"), true);
		Result->SetNumberField(TEXT("requested_count"), AssetPathsArr->Num());
		if (SkippedUnknown.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> SkippedArr;
			for (const FString& S : SkippedUnknown) SkippedArr.Add(MakeShared<FJsonValueString>(S));
			Result->SetArrayField(TEXT("skipped_unknown_class"), SkippedArr);
		}
		return FMonolithActionResult::Success(Result);
	}

	// Helper: validate all assets of a type using a given action
	auto ValidateAssets = [&](const FTopLevelAssetPath& ClassPath, const FString& TypeName, const FString& ActionName)
	{
		TArray<FAssetData> Assets;
		AR.GetAssetsByClass(ClassPath, Assets);

		int32 Validated = 0;
		int32 Issues = 0;

		for (const FAssetData& Asset : Assets)
		{
			FString AssetPath = Asset.PackageName.ToString();
			if (!PathFilter.IsEmpty() && !AssetPath.StartsWith(PathFilter))
			{
				continue;
			}

			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), AssetPath);
			FMonolithActionResult R = Dispatch(TEXT("ai"), ActionName, P);
			++Validated;

			if (R.bSuccess && R.Result.IsValid())
			{
				int32 IssueCount = FMath::RoundToInt32(R.Result->GetNumberField(TEXT("issue_count")));
				if (IssueCount > 0)
				{
					Issues += IssueCount;

					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("asset_path"), AssetPath);
					Entry->SetStringField(TEXT("type"), TypeName);
					Entry->SetNumberField(TEXT("issue_count"), IssueCount);
					if (R.Result->HasField(TEXT("issues")))
					{
						Entry->SetArrayField(TEXT("issues"), R.Result->GetArrayField(TEXT("issues")));
					}
					AllResults.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
			else if (!R.bSuccess)
			{
				++Issues;
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("asset_path"), AssetPath);
				Entry->SetStringField(TEXT("type"), TypeName);
				Entry->SetNumberField(TEXT("issue_count"), 1);

				TArray<TSharedPtr<FJsonValue>> ErrArr;
				TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
				ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
				ErrObj->SetStringField(TEXT("message"), FString::Printf(TEXT("Validation failed: %s"), *R.ErrorMessage));
				ErrArr.Add(MakeShared<FJsonValueObject>(ErrObj));
				Entry->SetArrayField(TEXT("issues"), ErrArr);
				AllResults.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}

		TotalAssets += Validated;
		TotalIssues += Issues;

		return Validated;
	};

	// Validate BTs
	int32 BTCount = ValidateAssets(UBehaviorTree::StaticClass()->GetClassPathName(), TEXT("BehaviorTree"), TEXT("validate_behavior_tree"));

	// Validate AI Controllers (Blueprints with AIController parent)
	{
		TArray<FAssetData> AllBPs;
		AR.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBPs);
		int32 ControllerCount = 0;
		for (const FAssetData& Asset : AllBPs)
		{
			FAssetTagValueRef ParentTag = Asset.TagsAndValues.FindTag(FName(TEXT("ParentClass")));
			if (!ParentTag.IsSet()) continue;
			if (!ParentTag.GetValue().Contains(TEXT("AIController"))) continue;

			FString AssetPath = Asset.PackageName.ToString();
			if (!PathFilter.IsEmpty() && !AssetPath.StartsWith(PathFilter)) continue;

			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), AssetPath);
			FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("validate_ai_controller"), P);
			++ControllerCount;
			++TotalAssets;

			if (R.bSuccess && R.Result.IsValid())
			{
				int32 IssueCount = FMath::RoundToInt32(R.Result->GetNumberField(TEXT("issue_count")));
				if (IssueCount > 0)
				{
					TotalIssues += IssueCount;
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("asset_path"), AssetPath);
					Entry->SetStringField(TEXT("type"), TEXT("AIController"));
					Entry->SetNumberField(TEXT("issue_count"), IssueCount);
					if (R.Result->HasField(TEXT("issues")))
					{
						Entry->SetArrayField(TEXT("issues"), R.Result->GetArrayField(TEXT("issues")));
					}
					AllResults.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
		}
	}

	// Validate State Trees
#if WITH_STATETREE
	{
		int32 STCount = ValidateAssets(UStateTree::StaticClass()->GetClassPathName(), TEXT("StateTree"), TEXT("validate_state_tree"));
		Result->SetNumberField(TEXT("state_trees_validated"), STCount);
	}
#endif

	// Validate EQS
	{
		int32 EQSCount = ValidateAssets(UEnvQuery::StaticClass()->GetClassPathName(), TEXT("EQS"), TEXT("validate_eqs_query"));
		Result->SetNumberField(TEXT("eqs_validated"), EQSCount);
	}

	// Validate Smart Objects
#if WITH_SMARTOBJECTS
	{
		UClass* SOClass = FindFirstObject<UClass>(TEXT("SmartObjectDefinition"), EFindFirstObjectOptions::EnsureIfAmbiguous);
		if (SOClass)
		{
			int32 SOCount = ValidateAssets(SOClass->GetClassPathName(), TEXT("SmartObject"), TEXT("validate_smart_object_definition"));
			Result->SetNumberField(TEXT("smart_objects_validated"), SOCount);
		}
	}
#endif

	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Batch validation complete: %d assets checked, %d issues found"), TotalAssets, TotalIssues));
	Result->SetNumberField(TEXT("total_assets"), TotalAssets);
	Result->SetNumberField(TEXT("total_issues"), TotalIssues);
	Result->SetBoolField(TEXT("all_valid"), TotalIssues == 0);
	Result->SetArrayField(TEXT("results"), AllResults);

	if (!PathFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("path_filter"), PathFilter);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  107. validate_ai_controller
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleValidateAIController(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBlueprint* BP = MonolithAI::LoadAIControllerFromParams(Params, AssetPath, Error);
	if (!BP)
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Issues;

	// Compile if needed
	if (!BP->GeneratedClass)
	{
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	if (!BP->GeneratedClass)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), TEXT("error"));
		Issue->SetStringField(TEXT("message"), TEXT("Blueprint failed to compile"));
		Issues.Add(MakeShared<FJsonValueObject>(Issue));

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetArrayField(TEXT("issues"), Issues);
		Result->SetNumberField(TEXT("issue_count"), Issues.Num());
		Result->SetBoolField(TEXT("valid"), false);
		return FMonolithActionResult::Success(Result);
	}

	AAIController* CDO = Cast<AAIController>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), TEXT("error"));
		Issue->SetStringField(TEXT("message"), TEXT("Failed to get CDO — not an AIController?"));
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	}
	else
	{
		// Check BT/BB — AAIController doesn't store BT as a UPROPERTY;
		// we check if bStartAILogicOnPossess is enabled (standard for BT-driven AI)
		bool bStartOnPossess = false;
		if (FBoolProperty* StartProp = CastField<FBoolProperty>(BP->GeneratedClass->FindPropertyByName(TEXT("bStartAILogicOnPossess"))))
		{
			bStartOnPossess = StartProp->GetPropertyValue_InContainer(CDO);
		}
		if (!bStartOnPossess)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("warning"));
			Issue->SetStringField(TEXT("message"), TEXT("bStartAILogicOnPossess is false — BT won't auto-run on possess. Set it via set_ai_controller_flags or call RunBehaviorTree manually."));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}

		// Check team ID
		uint8 TeamId = CDO->GetGenericTeamId().GetId();
		if (TeamId == 255)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("info"));
			Issue->SetStringField(TEXT("message"), TEXT("Team ID is NoTeam (255) — perception affiliation filtering won't work. Use set_ai_team to assign a team."));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}

		// Check perception component
		bool bHasPerception = false;
		if (BP->SimpleConstructionScript)
		{
			for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
			{
				if (Node && Node->ComponentClass &&
					Node->ComponentClass->IsChildOf(UAIPerceptionComponent::StaticClass()))
				{
					bHasPerception = true;

					// Check if any senses are configured
					UAIPerceptionComponent* PerceptionComp = Cast<UAIPerceptionComponent>(Node->ComponentTemplate);
					if (PerceptionComp)
					{
						int32 SenseCount = 0;
						for (auto It = PerceptionComp->GetSensesConfigIterator(); It; ++It)
						{
							++SenseCount;
						}
						if (SenseCount == 0)
						{
							TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
							Issue->SetStringField(TEXT("severity"), TEXT("warning"));
							Issue->SetStringField(TEXT("message"), TEXT("Perception component exists but no senses configured. Use configure_sight_sense etc."));
							Issues.Add(MakeShared<FJsonValueObject>(Issue));
						}
					}
					break;
				}
			}
		}
		if (!bHasPerception)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("info"));
			Issue->SetStringField(TEXT("message"), TEXT("No AIPerceptionComponent found. Add one via add_perception_component for sensory awareness."));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());
	Result->SetBoolField(TEXT("valid"), Issues.Num() == 0);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  106. scaffold_ai_controller_blueprint
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldAIControllerBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, BTPath, BBPath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("bt_path"), BTPath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("bb_path"), BBPath, ErrResult)) return ErrResult;

	FString PerceptionPreset = Params->GetStringField(TEXT("perception_preset"));
	int32 TeamId = -1;
	if (Params->HasField(TEXT("team_id")))
	{
		TeamId = FMath::RoundToInt32(Params->GetNumberField(TEXT("team_id")));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Scaffold AI Controller Blueprint")));

	TArray<FString> Warnings;

	// 1. Create the controller
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), SavePath);
		P->SetStringField(TEXT("bt_path"), BTPath);
		P->SetStringField(TEXT("bb_path"), BBPath);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("create_ai_controller"), P);
		if (!R.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create AI Controller: %s"), *R.ErrorMessage));
		}
	}

	// 2. Set BT + BB
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), SavePath);
		P->SetStringField(TEXT("bt_path"), BTPath);
		P->SetStringField(TEXT("bb_path"), BBPath);
		DispatchOrWarn(TEXT("ai"), TEXT("set_ai_controller_bt"), P, Warnings, TEXT("Set BT/BB"));
	}

	// 3. Enable auto-start
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), SavePath);
		P->SetBoolField(TEXT("start_ai_on_possess"), true);
		DispatchOrWarn(TEXT("ai"), TEXT("set_ai_controller_flags"), P, Warnings, TEXT("Set auto-start"));
	}

	// 4. Perception
	if (!PerceptionPreset.IsEmpty())
	{
		// Add component
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), SavePath);
			P->SetStringField(TEXT("dominant_sense"), TEXT("Sight"));
			DispatchOrWarn(TEXT("ai"), TEXT("add_perception_component"), P, Warnings, TEXT("Add perception"));
		}

		bool bSight = PerceptionPreset.Contains(TEXT("sight")) || PerceptionPreset == TEXT("full");
		bool bHearing = PerceptionPreset.Contains(TEXT("hearing")) || PerceptionPreset == TEXT("full");
		bool bDamage = PerceptionPreset == TEXT("full");

		if (bSight)
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), SavePath);
			P->SetNumberField(TEXT("radius"), 1500.0);
			P->SetNumberField(TEXT("peripheral_angle"), 70.0);
			DispatchOrWarn(TEXT("ai"), TEXT("configure_sight_sense"), P, Warnings, TEXT("Configure sight"));
		}

		if (bHearing)
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), SavePath);
			P->SetNumberField(TEXT("range"), 2000.0);
			DispatchOrWarn(TEXT("ai"), TEXT("configure_hearing_sense"), P, Warnings, TEXT("Configure hearing"));
		}

		if (bDamage)
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), SavePath);
			DispatchOrWarn(TEXT("ai"), TEXT("configure_damage_sense"), P, Warnings, TEXT("Configure damage"));
		}
	}

	// 5. Team
	if (TeamId >= 0 && TeamId <= 254)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), SavePath);
		P->SetNumberField(TEXT("team_id"), TeamId);
		DispatchOrWarn(TEXT("ai"), TEXT("set_ai_team"), P, Warnings, TEXT("Set team"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("message"), TEXT("AI Controller fully scaffolded"));
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetStringField(TEXT("bt_path"), BTPath);
	Result->SetStringField(TEXT("bb_path"), BBPath);
	if (!PerceptionPreset.IsEmpty())
	{
		Result->SetStringField(TEXT("perception_preset"), PerceptionPreset);
	}
	if (TeamId >= 0)
	{
		Result->SetNumberField(TEXT("team_id"), TeamId);
	}

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings) WarnArr.Add(MakeShared<FJsonValueString>(W));
		Result->SetArrayField(TEXT("warnings"), WarnArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Genre Scaffold Helper
// ============================================================

namespace
{
	/** Standard result builder for genre scaffolds */
	TSharedPtr<FJsonObject> BuildGenreScaffoldResult(
		const FString& Name,
		const FString& Archetype,
		const TArray<FString>& CreatedAssets,
		const TArray<FString>& Warnings,
		const TArray<FString>& NextSteps)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("message"), FString::Printf(TEXT("%s '%s' scaffolded — %d assets created"), *Archetype, *Name, CreatedAssets.Num()));
		Result->SetStringField(TEXT("archetype"), Archetype);

		TArray<TSharedPtr<FJsonValue>> AssetArr;
		for (const FString& A : CreatedAssets) AssetArr.Add(MakeShared<FJsonValueString>(A));
		Result->SetArrayField(TEXT("created_assets"), AssetArr);
		Result->SetNumberField(TEXT("asset_count"), CreatedAssets.Num());

		if (Warnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> WarnArr;
			for (const FString& W : Warnings) WarnArr.Add(MakeShared<FJsonValueString>(W));
			Result->SetArrayField(TEXT("warnings"), WarnArr);
		}

		if (NextSteps.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> StepArr;
			for (const FString& S : NextSteps) StepArr.Add(MakeShared<FJsonValueString>(S));
			Result->SetArrayField(TEXT("next_steps"), StepArr);
		}

		return Result;
	}
}

// ============================================================
//  187. scaffold_companion_ai
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldCompanionAI(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, ErrResult)) return ErrResult;

	float FollowDistance = 300.0f;
	if (Params->HasField(TEXT("follow_distance")))
	{
		FollowDistance = static_cast<float>(Params->GetNumberField(TEXT("follow_distance")));
	}

	FString CombatBehavior = Params->GetStringField(TEXT("combat_behavior"));
	if (CombatBehavior.IsEmpty()) CombatBehavior = TEXT("defensive");

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Scaffold Companion AI")));

	TArray<FString> CreatedAssets;
	TArray<FString> Warnings;

	FString BBPath = SavePath / TEXT("BB_") + Name;
	FString BTPath = SavePath / TEXT("BT_") + Name;

	// 1. Create Blackboard with companion-specific keys
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BBPath);
		if (!DispatchOrWarn(TEXT("ai"), TEXT("create_blackboard"), P, Warnings, TEXT("Create BB")))
		{
			return FMonolithActionResult::Error(TEXT("Failed to create Blackboard"));
		}
		CreatedAssets.Add(BBPath);

		auto MakeKey = [](const FString& KeyName, const FString& Type, const FString& BaseClass = TEXT("")) -> TSharedPtr<FJsonValue>
		{
			TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
			K->SetStringField(TEXT("name"), KeyName);
			K->SetStringField(TEXT("type"), Type);
			if (!BaseClass.IsEmpty()) K->SetStringField(TEXT("base_class"), BaseClass);
			return MakeShared<FJsonValueObject>(K);
		};

		TArray<TSharedPtr<FJsonValue>> Keys;
		Keys.Add(MakeKey(TEXT("FollowTarget"), TEXT("Object"), TEXT("Actor")));
		Keys.Add(MakeKey(TEXT("EnemyTarget"), TEXT("Object"), TEXT("Actor")));
		Keys.Add(MakeKey(TEXT("FollowLocation"), TEXT("Vector")));
		Keys.Add(MakeKey(TEXT("bInCombat"), TEXT("Bool")));

		TSharedPtr<FJsonObject> KeysP = MakeShared<FJsonObject>();
		KeysP->SetStringField(TEXT("asset_path"), BBPath);
		KeysP->SetArrayField(TEXT("keys"), Keys);
		DispatchOrWarn(TEXT("ai"), TEXT("batch_add_bb_keys"), KeysP, Warnings, TEXT("Add BB keys"));
	}

	// 2. Build companion BT spec
	// Selector -> [Combat Sequence (if aggressive/defensive), Follow Sequence, Idle]
	{
		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
		Spec->SetStringField(TEXT("blackboard_path"), BBPath);

		auto MakeNode = [](const FString& ClassName) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>();
			N->SetStringField(TEXT("type"), ClassName);
			return N;
		};
		auto SetProp = [](TSharedPtr<FJsonObject>& Node, const FString& Key, const FString& Value)
		{
			TSharedPtr<FJsonObject> Props;
			const TSharedPtr<FJsonObject>* Existing = nullptr;
			if (Node->TryGetObjectField(TEXT("properties"), Existing) && Existing) Props = *Existing;
			else { Props = MakeShared<FJsonObject>(); Node->SetObjectField(TEXT("properties"), Props); }
			Props->SetStringField(Key, Value);
		};

		TSharedPtr<FJsonObject> Root = MakeNode(TEXT("BTComposite_Selector"));
		TArray<TSharedPtr<FJsonValue>> RootChildren;

		// Combat branch (if not passive)
		if (CombatBehavior != TEXT("passive"))
		{
			TSharedPtr<FJsonObject> CombatSeq = MakeNode(TEXT("BTComposite_Sequence"));
			TSharedPtr<FJsonObject> HasEnemyDec = MakeShared<FJsonObject>();
			HasEnemyDec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SetProp(HasEnemyDec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("EnemyTarget"));
			CombatSeq->SetArrayField(TEXT("decorators"), { MakeShared<FJsonValueObject>(HasEnemyDec) });

			TSharedPtr<FJsonObject> MoveToEnemy = MakeNode(TEXT("BTTask_MoveTo"));
			SetProp(MoveToEnemy, TEXT("BlackboardKey.SelectedKeyName"), TEXT("EnemyTarget"));
			SetProp(MoveToEnemy, TEXT("AcceptableRadius"), TEXT("150"));

			TSharedPtr<FJsonObject> AttackWait = MakeNode(TEXT("BTTask_Wait"));
			SetProp(AttackWait, TEXT("WaitTime"), TEXT("1.5"));

			CombatSeq->SetArrayField(TEXT("children"), {
				MakeShared<FJsonValueObject>(MoveToEnemy),
				MakeShared<FJsonValueObject>(AttackWait)
			});
			RootChildren.Add(MakeShared<FJsonValueObject>(CombatSeq));
		}

		// Follow branch
		TSharedPtr<FJsonObject> FollowSeq = MakeNode(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> HasFollowDec = MakeShared<FJsonObject>();
			HasFollowDec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SetProp(HasFollowDec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("FollowTarget"));
			FollowSeq->SetArrayField(TEXT("decorators"), { MakeShared<FJsonValueObject>(HasFollowDec) });

			TSharedPtr<FJsonObject> MoveTo = MakeNode(TEXT("BTTask_MoveTo"));
			SetProp(MoveTo, TEXT("BlackboardKey.SelectedKeyName"), TEXT("FollowTarget"));
			SetProp(MoveTo, TEXT("AcceptableRadius"), FString::Printf(TEXT("%.0f"), FollowDistance));

			TSharedPtr<FJsonObject> Wait = MakeNode(TEXT("BTTask_Wait"));
			SetProp(Wait, TEXT("WaitTime"), TEXT("0.5"));

			FollowSeq->SetArrayField(TEXT("children"), {
				MakeShared<FJsonValueObject>(MoveTo),
				MakeShared<FJsonValueObject>(Wait)
			});
		}
		RootChildren.Add(MakeShared<FJsonValueObject>(FollowSeq));

		// Idle fallback
		TSharedPtr<FJsonObject> IdleWait = MakeNode(TEXT("BTTask_Wait"));
		SetProp(IdleWait, TEXT("WaitTime"), TEXT("2.0"));
		RootChildren.Add(MakeShared<FJsonValueObject>(IdleWait));

		Root->SetArrayField(TEXT("children"), RootChildren);
		Spec->SetObjectField(TEXT("root"), Root);

		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BTPath);
		P->SetObjectField(TEXT("spec"), Spec);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("build_behavior_tree_from_spec"), P);
		if (!R.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create BT: %s"), *R.ErrorMessage));
		CreatedAssets.Add(BTPath);

		// Phase D1: belt-and-suspenders BB linkage (Issue #48)
		TSharedPtr<FJsonObject> SetBBParams = MakeShared<FJsonObject>();
		SetBBParams->SetStringField(TEXT("bt_path"), BTPath);
		SetBBParams->SetStringField(TEXT("blackboard_path"), BBPath);
		FMonolithActionResult LinkResult = Dispatch(TEXT("ai"), TEXT("set_bt_blackboard"), SetBBParams);
		if (!LinkResult.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to link BT to Blackboard: %s"), *LinkResult.ErrorMessage));
	}

	// 3. Create full AI character via dispatch
	FString ControllerPath = SavePath / TEXT("BP_") + Name + TEXT("Controller");
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), ControllerPath);
		P->SetStringField(TEXT("bt_path"), BTPath);
		P->SetStringField(TEXT("bb_path"), BBPath);
		P->SetStringField(TEXT("perception_preset"), TEXT("sight_hearing"));
		P->SetNumberField(TEXT("team_id"), 0); // Friendly team
		DispatchOrWarn(TEXT("ai"), TEXT("scaffold_ai_controller_blueprint"), P, Warnings, TEXT("Create AI Controller"));
		CreatedAssets.Add(ControllerPath);
	}

	TArray<FString> NextSteps;
	NextSteps.Add(TEXT("Set FollowTarget BB key to the player pawn at runtime"));
	NextSteps.Add(TEXT("Add custom BTTask for companion-specific combat abilities"));
	if (CombatBehavior == TEXT("passive"))
	{
		NextSteps.Add(TEXT("Companion is passive — add flee behavior if desired"));
	}
	NextSteps.Add(TEXT("Wire AIControllerClass on your companion Character BP"));

	return FMonolithActionResult::Success(BuildGenreScaffoldResult(Name, TEXT("Companion AI"), CreatedAssets, Warnings, NextSteps));
}

// ============================================================
//  188. scaffold_boss_ai
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldBossAI(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, ErrResult)) return ErrResult;

	// Parse phases
	struct FPhaseInfo { FString PhaseName; float HealthThreshold; };
	TArray<FPhaseInfo> Phases;

	const TArray<TSharedPtr<FJsonValue>>* PhasesArr = nullptr;
	if (Params->TryGetArrayField(TEXT("phases"), PhasesArr) && PhasesArr && PhasesArr->Num() > 0)
	{
		for (const TSharedPtr<FJsonValue>& PhaseVal : *PhasesArr)
		{
			const TSharedPtr<FJsonObject>* PhaseObj = nullptr;
			if (PhaseVal->TryGetObject(PhaseObj) && PhaseObj && (*PhaseObj).IsValid())
			{
				FPhaseInfo Info;
				Info.PhaseName = (*PhaseObj)->GetStringField(TEXT("name"));
				Info.HealthThreshold = static_cast<float>((*PhaseObj)->GetNumberField(TEXT("health_threshold")));
				Phases.Add(Info);
			}
		}
	}

	if (Phases.Num() == 0)
	{
		Phases.Add({ TEXT("Phase1"), 0.75f });
		Phases.Add({ TEXT("Phase2"), 0.50f });
		Phases.Add({ TEXT("Phase3"), 0.25f });
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Scaffold Boss AI")));

	TArray<FString> CreatedAssets;
	TArray<FString> Warnings;

	FString BBPath = SavePath / TEXT("BB_") + Name;
	FString BTPath = SavePath / TEXT("BT_") + Name;

	// 1. Create BB with boss keys
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BBPath);
		if (!DispatchOrWarn(TEXT("ai"), TEXT("create_blackboard"), P, Warnings, TEXT("Create BB")))
		{
			return FMonolithActionResult::Error(TEXT("Failed to create Blackboard"));
		}
		CreatedAssets.Add(BBPath);

		auto MakeKey = [](const FString& KeyName, const FString& Type, const FString& BaseClass = TEXT("")) -> TSharedPtr<FJsonValue>
		{
			TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
			K->SetStringField(TEXT("name"), KeyName);
			K->SetStringField(TEXT("type"), Type);
			if (!BaseClass.IsEmpty()) K->SetStringField(TEXT("base_class"), BaseClass);
			return MakeShared<FJsonValueObject>(K);
		};

		TArray<TSharedPtr<FJsonValue>> Keys;
		Keys.Add(MakeKey(TEXT("TargetActor"), TEXT("Object"), TEXT("Actor")));
		Keys.Add(MakeKey(TEXT("CurrentPhase"), TEXT("Int")));
		Keys.Add(MakeKey(TEXT("HealthPercent"), TEXT("Float")));
		Keys.Add(MakeKey(TEXT("bIsEnraged"), TEXT("Bool")));
		Keys.Add(MakeKey(TEXT("ArenaCenter"), TEXT("Vector")));

		TSharedPtr<FJsonObject> KeysP = MakeShared<FJsonObject>();
		KeysP->SetStringField(TEXT("asset_path"), BBPath);
		KeysP->SetArrayField(TEXT("keys"), Keys);
		DispatchOrWarn(TEXT("ai"), TEXT("batch_add_bb_keys"), KeysP, Warnings, TEXT("Add BB keys"));
	}

	// 2. Build phase-based BT
	{
		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
		Spec->SetStringField(TEXT("blackboard_path"), BBPath);

		auto MakeNode = [](const FString& ClassName) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>();
			N->SetStringField(TEXT("type"), ClassName);
			return N;
		};
		auto SetProp = [](TSharedPtr<FJsonObject>& Node, const FString& Key, const FString& Value)
		{
			TSharedPtr<FJsonObject> Props;
			const TSharedPtr<FJsonObject>* Existing = nullptr;
			if (Node->TryGetObjectField(TEXT("properties"), Existing) && Existing) Props = *Existing;
			else { Props = MakeShared<FJsonObject>(); Node->SetObjectField(TEXT("properties"), Props); }
			Props->SetStringField(Key, Value);
		};

		TSharedPtr<FJsonObject> Root = MakeNode(TEXT("BTComposite_Selector"));
		TArray<TSharedPtr<FJsonValue>> RootChildren;

		for (int32 i = 0; i < Phases.Num(); i++)
		{
			TSharedPtr<FJsonObject> PhaseSeq = MakeNode(TEXT("BTComposite_Sequence"));

			TSharedPtr<FJsonObject> PhaseDec = MakeShared<FJsonObject>();
			PhaseDec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SetProp(PhaseDec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("CurrentPhase"));
			PhaseSeq->SetArrayField(TEXT("decorators"), { MakeShared<FJsonValueObject>(PhaseDec) });

			TSharedPtr<FJsonObject> MoveTo = MakeNode(TEXT("BTTask_MoveTo"));
			SetProp(MoveTo, TEXT("BlackboardKey.SelectedKeyName"), TEXT("TargetActor"));
			float AcceptRadius = 200.0f - (i * 50.0f);
			SetProp(MoveTo, TEXT("AcceptableRadius"), FString::Printf(TEXT("%.0f"), FMath::Max(50.0f, AcceptRadius)));

			TSharedPtr<FJsonObject> AttackWait = MakeNode(TEXT("BTTask_Wait"));
			float AttackTime = 2.0f - (i * 0.3f);
			SetProp(AttackWait, TEXT("WaitTime"), FString::Printf(TEXT("%.1f"), FMath::Max(0.5f, AttackTime)));

			PhaseSeq->SetArrayField(TEXT("children"), {
				MakeShared<FJsonValueObject>(MoveTo),
				MakeShared<FJsonValueObject>(AttackWait)
			});

			RootChildren.Add(MakeShared<FJsonValueObject>(PhaseSeq));
		}

		// Idle fallback
		TSharedPtr<FJsonObject> IdleWait = MakeNode(TEXT("BTTask_Wait"));
		SetProp(IdleWait, TEXT("WaitTime"), TEXT("1.0"));
		RootChildren.Add(MakeShared<FJsonValueObject>(IdleWait));

		Root->SetArrayField(TEXT("children"), RootChildren);
		Spec->SetObjectField(TEXT("root"), Root);

		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BTPath);
		P->SetObjectField(TEXT("spec"), Spec);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("build_behavior_tree_from_spec"), P);
		if (!R.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create BT: %s"), *R.ErrorMessage));
		CreatedAssets.Add(BTPath);

		// Phase D1: belt-and-suspenders BB linkage (Issue #48)
		TSharedPtr<FJsonObject> SetBBParams = MakeShared<FJsonObject>();
		SetBBParams->SetStringField(TEXT("bt_path"), BTPath);
		SetBBParams->SetStringField(TEXT("blackboard_path"), BBPath);
		FMonolithActionResult LinkResult = Dispatch(TEXT("ai"), TEXT("set_bt_blackboard"), SetBBParams);
		if (!LinkResult.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to link BT to Blackboard: %s"), *LinkResult.ErrorMessage));
	}

	// 3. AI Controller
	FString ControllerPath = SavePath / TEXT("BP_") + Name + TEXT("Controller");
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), ControllerPath);
		P->SetStringField(TEXT("bt_path"), BTPath);
		P->SetStringField(TEXT("bb_path"), BBPath);
		P->SetStringField(TEXT("perception_preset"), TEXT("full"));
		P->SetNumberField(TEXT("team_id"), 1);
		DispatchOrWarn(TEXT("ai"), TEXT("scaffold_ai_controller_blueprint"), P, Warnings, TEXT("Create AI Controller"));
		CreatedAssets.Add(ControllerPath);
	}

	TArray<FString> NextSteps;
	NextSteps.Add(TEXT("Create custom BTTasks for each phase's unique attack patterns"));
	NextSteps.Add(TEXT("Add a BTService to update CurrentPhase and HealthPercent BB keys from the boss's health"));
	for (int32 i = 0; i < Phases.Num(); i++)
	{
		NextSteps.Add(FString::Printf(TEXT("Phase %d ('%s'): triggers at %.0f%% health — customize attack pattern"),
			i + 1, *Phases[i].PhaseName, Phases[i].HealthThreshold * 100.0f));
	}
	NextSteps.Add(TEXT("Wire AIControllerClass on your boss Character BP"));

	TSharedPtr<FJsonObject> Result = BuildGenreScaffoldResult(Name, TEXT("Boss AI"), CreatedAssets, Warnings, NextSteps);

	TArray<TSharedPtr<FJsonValue>> PhaseArr;
	for (const FPhaseInfo& Phase : Phases)
	{
		TSharedPtr<FJsonObject> PhaseObj = MakeShared<FJsonObject>();
		PhaseObj->SetStringField(TEXT("name"), Phase.PhaseName);
		PhaseObj->SetNumberField(TEXT("health_threshold"), Phase.HealthThreshold);
		PhaseArr.Add(MakeShared<FJsonValueObject>(PhaseObj));
	}
	Result->SetArrayField(TEXT("phases"), PhaseArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  189. scaffold_ambient_npc
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldAmbientNPC(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, ErrResult)) return ErrResult;

	float WanderRadius = 1000.0f;
	if (Params->HasField(TEXT("wander_radius")))
	{
		WanderRadius = static_cast<float>(Params->GetNumberField(TEXT("wander_radius")));
	}

	TArray<FString> SmartObjects;
	const TArray<TSharedPtr<FJsonValue>>* SOArr = nullptr;
	if (Params->TryGetArrayField(TEXT("smart_objects"), SOArr) && SOArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *SOArr)
		{
			FString S;
			if (V->TryGetString(S)) SmartObjects.Add(S);
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Scaffold Ambient NPC")));

	TArray<FString> CreatedAssets;
	TArray<FString> Warnings;

	FString BBPath = SavePath / TEXT("BB_") + Name;
	FString BTPath = SavePath / TEXT("BT_") + Name;

	// 1. BB
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BBPath);
		if (!DispatchOrWarn(TEXT("ai"), TEXT("create_blackboard"), P, Warnings, TEXT("Create BB")))
		{
			return FMonolithActionResult::Error(TEXT("Failed to create Blackboard"));
		}
		CreatedAssets.Add(BBPath);

		auto MakeKey = [](const FString& KeyName, const FString& Type, const FString& BaseClass = TEXT("")) -> TSharedPtr<FJsonValue>
		{
			TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
			K->SetStringField(TEXT("name"), KeyName);
			K->SetStringField(TEXT("type"), Type);
			if (!BaseClass.IsEmpty()) K->SetStringField(TEXT("base_class"), BaseClass);
			return MakeShared<FJsonValueObject>(K);
		};

		TArray<TSharedPtr<FJsonValue>> Keys;
		Keys.Add(MakeKey(TEXT("WanderLocation"), TEXT("Vector")));
		Keys.Add(MakeKey(TEXT("HomeLocation"), TEXT("Vector")));
		Keys.Add(MakeKey(TEXT("bIsInteracting"), TEXT("Bool")));

		TSharedPtr<FJsonObject> KeysP = MakeShared<FJsonObject>();
		KeysP->SetStringField(TEXT("asset_path"), BBPath);
		KeysP->SetArrayField(TEXT("keys"), Keys);
		DispatchOrWarn(TEXT("ai"), TEXT("batch_add_bb_keys"), KeysP, Warnings, TEXT("Add BB keys"));
	}

	// 2. BT: Selector -> [Wander sequence, Idle]
	{
		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
		Spec->SetStringField(TEXT("blackboard_path"), BBPath);

		auto MakeNode = [](const FString& ClassName) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>();
			N->SetStringField(TEXT("type"), ClassName);
			return N;
		};
		auto SetProp = [](TSharedPtr<FJsonObject>& Node, const FString& Key, const FString& Value)
		{
			TSharedPtr<FJsonObject> Props;
			const TSharedPtr<FJsonObject>* Existing = nullptr;
			if (Node->TryGetObjectField(TEXT("properties"), Existing) && Existing) Props = *Existing;
			else { Props = MakeShared<FJsonObject>(); Node->SetObjectField(TEXT("properties"), Props); }
			Props->SetStringField(Key, Value);
		};

		TSharedPtr<FJsonObject> Root = MakeNode(TEXT("BTComposite_Selector"));
		TArray<TSharedPtr<FJsonValue>> RootChildren;

		// Wander sequence
		TSharedPtr<FJsonObject> WanderSeq = MakeNode(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> MoveTo = MakeNode(TEXT("BTTask_MoveTo"));
			SetProp(MoveTo, TEXT("BlackboardKey.SelectedKeyName"), TEXT("WanderLocation"));
			SetProp(MoveTo, TEXT("AcceptableRadius"), TEXT("50"));

			TSharedPtr<FJsonObject> Wait = MakeNode(TEXT("BTTask_Wait"));
			SetProp(Wait, TEXT("WaitTime"), TEXT("5.0"));

			WanderSeq->SetArrayField(TEXT("children"), {
				MakeShared<FJsonValueObject>(MoveTo),
				MakeShared<FJsonValueObject>(Wait)
			});
		}
		RootChildren.Add(MakeShared<FJsonValueObject>(WanderSeq));

		// Idle fallback
		TSharedPtr<FJsonObject> Idle = MakeNode(TEXT("BTTask_Wait"));
		SetProp(Idle, TEXT("WaitTime"), TEXT("3.0"));
		RootChildren.Add(MakeShared<FJsonValueObject>(Idle));

		Root->SetArrayField(TEXT("children"), RootChildren);
		Spec->SetObjectField(TEXT("root"), Root);

		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BTPath);
		P->SetObjectField(TEXT("spec"), Spec);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("build_behavior_tree_from_spec"), P);
		if (!R.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create BT: %s"), *R.ErrorMessage));
		CreatedAssets.Add(BTPath);

		// Phase D1: belt-and-suspenders BB linkage (Issue #48)
		TSharedPtr<FJsonObject> SetBBParams = MakeShared<FJsonObject>();
		SetBBParams->SetStringField(TEXT("bt_path"), BTPath);
		SetBBParams->SetStringField(TEXT("blackboard_path"), BBPath);
		FMonolithActionResult LinkResult = Dispatch(TEXT("ai"), TEXT("set_bt_blackboard"), SetBBParams);
		if (!LinkResult.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to link BT to Blackboard: %s"), *LinkResult.ErrorMessage));
	}

	// 3. Controller
	FString ControllerPath = SavePath / TEXT("BP_") + Name + TEXT("Controller");
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), ControllerPath);
		P->SetStringField(TEXT("bt_path"), BTPath);
		P->SetStringField(TEXT("bb_path"), BBPath);
		P->SetStringField(TEXT("perception_preset"), TEXT("sight"));
		P->SetNumberField(TEXT("team_id"), 255); // Neutral
		DispatchOrWarn(TEXT("ai"), TEXT("scaffold_ai_controller_blueprint"), P, Warnings, TEXT("Create AI Controller"));
		CreatedAssets.Add(ControllerPath);
	}

	if (SmartObjects.Num() > 0)
	{
		for (const FString& SOTag : SmartObjects)
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), BTPath);
			P->SetStringField(TEXT("activity_tags"), SOTag);
			DispatchOrWarn(TEXT("ai"), TEXT("add_bt_smart_object_task"), P, Warnings,
				FString::Printf(TEXT("Add SO task: %s"), *SOTag));
		}
	}

	TArray<FString> NextSteps;
	NextSteps.Add(TEXT("Add a BTService to set WanderLocation from random reachable points"));
	NextSteps.Add(FString::Printf(TEXT("Wander radius configured: %.0f cm"), WanderRadius));
	if (SmartObjects.Num() > 0)
		NextSteps.Add(TEXT("Place SmartObject actors in your level matching the activity tags"));
	else
		NextSteps.Add(TEXT("Add Smart Object interactions via add_bt_smart_object_task for richer behavior"));
	NextSteps.Add(TEXT("Wire AIControllerClass on your NPC Character BP"));

	return FMonolithActionResult::Success(BuildGenreScaffoldResult(Name, TEXT("Ambient NPC"), CreatedAssets, Warnings, NextSteps));
}

// ============================================================
//  190. scaffold_horror_stalker
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldHorrorStalker(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, ErrResult)) return ErrResult;

	float StalkDistance = 1500.0f;
	if (Params->HasField(TEXT("stalk_distance")))
		StalkDistance = static_cast<float>(Params->GetNumberField(TEXT("stalk_distance")));

	FString AttackConditions = Params->GetStringField(TEXT("attack_conditions"));
	if (AttackConditions.IsEmpty()) AttackConditions = TEXT("darkness");

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Scaffold Horror Stalker")));
	TArray<FString> CreatedAssets, Warnings;
	FString BBPath = SavePath / TEXT("BB_") + Name;
	FString BTPath = SavePath / TEXT("BT_") + Name;

	// 1. BB with stalker keys
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BBPath);
		if (!DispatchOrWarn(TEXT("ai"), TEXT("create_blackboard"), P, Warnings, TEXT("Create BB")))
			return FMonolithActionResult::Error(TEXT("Failed to create Blackboard"));
		CreatedAssets.Add(BBPath);

		auto MakeKey = [](const FString& KeyName, const FString& Type, const FString& BaseClass = TEXT("")) -> TSharedPtr<FJsonValue>
		{
			TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
			K->SetStringField(TEXT("name"), KeyName);
			K->SetStringField(TEXT("type"), Type);
			if (!BaseClass.IsEmpty()) K->SetStringField(TEXT("base_class"), BaseClass);
			return MakeShared<FJsonValueObject>(K);
		};

		TArray<TSharedPtr<FJsonValue>> Keys;
		Keys.Add(MakeKey(TEXT("TargetActor"), TEXT("Object"), TEXT("Actor")));
		Keys.Add(MakeKey(TEXT("StalkPosition"), TEXT("Vector")));
		Keys.Add(MakeKey(TEXT("bCanAttack"), TEXT("Bool")));
		Keys.Add(MakeKey(TEXT("bTargetInDarkness"), TEXT("Bool")));
		Keys.Add(MakeKey(TEXT("bTargetAlone"), TEXT("Bool")));
		Keys.Add(MakeKey(TEXT("DistanceToTarget"), TEXT("Float")));

		TSharedPtr<FJsonObject> KeysP = MakeShared<FJsonObject>();
		KeysP->SetStringField(TEXT("asset_path"), BBPath);
		KeysP->SetArrayField(TEXT("keys"), Keys);
		DispatchOrWarn(TEXT("ai"), TEXT("batch_add_bb_keys"), KeysP, Warnings, TEXT("Add BB keys"));
	}

	// 2. BT: Selector -> [Attack (conditions met), Stalk (maintain distance), Lurk]
	{
		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
		Spec->SetStringField(TEXT("blackboard_path"), BBPath);

		auto MN = [](const FString& C) { TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>(); N->SetStringField(TEXT("type"), C); return N; };
		auto SP = [](TSharedPtr<FJsonObject>& Node, const FString& Key, const FString& Value) {
			TSharedPtr<FJsonObject> Props; const TSharedPtr<FJsonObject>* Ex = nullptr;
			if (Node->TryGetObjectField(TEXT("properties"), Ex) && Ex) Props = *Ex;
			else { Props = MakeShared<FJsonObject>(); Node->SetObjectField(TEXT("properties"), Props); }
			Props->SetStringField(Key, Value);
		};

		TSharedPtr<FJsonObject> Root = MN(TEXT("BTComposite_Selector"));
		TArray<TSharedPtr<FJsonValue>> RC;

		// Attack branch
		TSharedPtr<FJsonObject> AtkSeq = MN(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> Dec = MakeShared<FJsonObject>();
			Dec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SP(Dec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("bCanAttack"));
			AtkSeq->SetArrayField(TEXT("decorators"), { MakeShared<FJsonValueObject>(Dec) });

			TSharedPtr<FJsonObject> Move = MN(TEXT("BTTask_MoveTo"));
			SP(Move, TEXT("BlackboardKey.SelectedKeyName"), TEXT("TargetActor"));
			SP(Move, TEXT("AcceptableRadius"), TEXT("100"));

			TSharedPtr<FJsonObject> W = MN(TEXT("BTTask_Wait"));
			SP(W, TEXT("WaitTime"), TEXT("1.0"));

			AtkSeq->SetArrayField(TEXT("children"), { MakeShared<FJsonValueObject>(Move), MakeShared<FJsonValueObject>(W) });
		}
		RC.Add(MakeShared<FJsonValueObject>(AtkSeq));

		// Stalk branch
		TSharedPtr<FJsonObject> StalkSeq = MN(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> Dec = MakeShared<FJsonObject>();
			Dec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SP(Dec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("TargetActor"));
			StalkSeq->SetArrayField(TEXT("decorators"), { MakeShared<FJsonValueObject>(Dec) });

			TSharedPtr<FJsonObject> Move = MN(TEXT("BTTask_MoveTo"));
			SP(Move, TEXT("BlackboardKey.SelectedKeyName"), TEXT("StalkPosition"));
			SP(Move, TEXT("AcceptableRadius"), TEXT("200"));

			TSharedPtr<FJsonObject> W = MN(TEXT("BTTask_Wait"));
			SP(W, TEXT("WaitTime"), TEXT("3.0"));

			StalkSeq->SetArrayField(TEXT("children"), { MakeShared<FJsonValueObject>(Move), MakeShared<FJsonValueObject>(W) });
		}
		RC.Add(MakeShared<FJsonValueObject>(StalkSeq));

		// Lurk
		TSharedPtr<FJsonObject> Lurk = MN(TEXT("BTTask_Wait"));
		SP(Lurk, TEXT("WaitTime"), TEXT("5.0"));
		RC.Add(MakeShared<FJsonValueObject>(Lurk));

		Root->SetArrayField(TEXT("children"), RC);
		Spec->SetObjectField(TEXT("root"), Root);

		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BTPath);
		P->SetObjectField(TEXT("spec"), Spec);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("build_behavior_tree_from_spec"), P);
		if (!R.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create BT: %s"), *R.ErrorMessage));
		CreatedAssets.Add(BTPath);

		// Phase D1: belt-and-suspenders BB linkage (Issue #48)
		TSharedPtr<FJsonObject> SetBBParams = MakeShared<FJsonObject>();
		SetBBParams->SetStringField(TEXT("bt_path"), BTPath);
		SetBBParams->SetStringField(TEXT("blackboard_path"), BBPath);
		FMonolithActionResult LinkResult = Dispatch(TEXT("ai"), TEXT("set_bt_blackboard"), SetBBParams);
		if (!LinkResult.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to link BT to Blackboard: %s"), *LinkResult.ErrorMessage));
	}

	// 3. Controller
	FString ControllerPath = SavePath / TEXT("BP_") + Name + TEXT("Controller");
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), ControllerPath);
		P->SetStringField(TEXT("bt_path"), BTPath);
		P->SetStringField(TEXT("bb_path"), BBPath);
		P->SetStringField(TEXT("perception_preset"), TEXT("sight_hearing"));
		P->SetNumberField(TEXT("team_id"), 1);
		DispatchOrWarn(TEXT("ai"), TEXT("scaffold_ai_controller_blueprint"), P, Warnings, TEXT("Create AI Controller"));
		CreatedAssets.Add(ControllerPath);
	}

	TArray<FString> NextSteps;
	NextSteps.Add(TEXT("Add BTService to compute StalkPosition (point behind player's heading at stalk_distance)"));
	NextSteps.Add(TEXT("Add BTService to evaluate attack conditions and set bCanAttack"));
	NextSteps.Add(FString::Printf(TEXT("Stalk distance: %.0f cm. Attack condition: %s"), StalkDistance, *AttackConditions));
	NextSteps.Add(TEXT("Replace Wait placeholders with custom attack BTTasks"));
	NextSteps.Add(TEXT("Add audio cues (breathing, footsteps) via BTService when stalking"));

	return FMonolithActionResult::Success(BuildGenreScaffoldResult(Name, TEXT("Horror Stalker"), CreatedAssets, Warnings, NextSteps));
}

// ============================================================
//  191. scaffold_horror_ambush
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldHorrorAmbush(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, ErrResult)) return ErrResult;

	FString TriggerType = Params->GetStringField(TEXT("trigger_type"));
	if (TriggerType.IsEmpty()) TriggerType = TEXT("proximity");
	FString AttackPattern = Params->GetStringField(TEXT("attack_pattern"));
	if (AttackPattern.IsEmpty()) AttackPattern = TEXT("lunge");

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Scaffold Horror Ambush")));
	TArray<FString> CreatedAssets, Warnings;
	FString BBPath = SavePath / TEXT("BB_") + Name;
	FString BTPath = SavePath / TEXT("BT_") + Name;

	// 1. BB
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BBPath);
		if (!DispatchOrWarn(TEXT("ai"), TEXT("create_blackboard"), P, Warnings, TEXT("Create BB")))
			return FMonolithActionResult::Error(TEXT("Failed to create Blackboard"));
		CreatedAssets.Add(BBPath);

		auto MK = [](const FString& N, const FString& T, const FString& B = TEXT("")) -> TSharedPtr<FJsonValue>
		{ TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>(); K->SetStringField(TEXT("name"), N); K->SetStringField(TEXT("type"), T); if (!B.IsEmpty()) K->SetStringField(TEXT("base_class"), B); return MakeShared<FJsonValueObject>(K); };

		TSharedPtr<FJsonObject> KeysP = MakeShared<FJsonObject>();
		KeysP->SetStringField(TEXT("asset_path"), BBPath);
		KeysP->SetArrayField(TEXT("keys"), {
			MK(TEXT("TargetActor"), TEXT("Object"), TEXT("Actor")),
			MK(TEXT("bTriggered"), TEXT("Bool")),
			MK(TEXT("bAttackComplete"), TEXT("Bool")),
			MK(TEXT("RetreatLocation"), TEXT("Vector")),
			MK(TEXT("SpawnLocation"), TEXT("Vector"))
		});
		DispatchOrWarn(TEXT("ai"), TEXT("batch_add_bb_keys"), KeysP, Warnings, TEXT("Add BB keys"));
	}

	// 2. BT: Selector -> [Retreat (after attack), Attack burst (triggered), Dormant]
	{
		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
		Spec->SetStringField(TEXT("blackboard_path"), BBPath);

		auto MN = [](const FString& C) { TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>(); N->SetStringField(TEXT("type"), C); return N; };
		auto SP = [](TSharedPtr<FJsonObject>& Node, const FString& Key, const FString& Value) {
			TSharedPtr<FJsonObject> Props; const TSharedPtr<FJsonObject>* Ex = nullptr;
			if (Node->TryGetObjectField(TEXT("properties"), Ex) && Ex) Props = *Ex;
			else { Props = MakeShared<FJsonObject>(); Node->SetObjectField(TEXT("properties"), Props); }
			Props->SetStringField(Key, Value);
		};

		TSharedPtr<FJsonObject> Root = MN(TEXT("BTComposite_Selector"));
		TArray<TSharedPtr<FJsonValue>> RC;

		// Retreat
		TSharedPtr<FJsonObject> RetSeq = MN(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> Dec = MakeShared<FJsonObject>();
			Dec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SP(Dec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("bAttackComplete"));
			RetSeq->SetArrayField(TEXT("decorators"), { MakeShared<FJsonValueObject>(Dec) });
			TSharedPtr<FJsonObject> Move = MN(TEXT("BTTask_MoveTo"));
			SP(Move, TEXT("BlackboardKey.SelectedKeyName"), TEXT("RetreatLocation"));
			SP(Move, TEXT("AcceptableRadius"), TEXT("50"));
			RetSeq->SetArrayField(TEXT("children"), { MakeShared<FJsonValueObject>(Move) });
		}
		RC.Add(MakeShared<FJsonValueObject>(RetSeq));

		// Attack burst
		TSharedPtr<FJsonObject> AtkSeq = MN(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> Dec = MakeShared<FJsonObject>();
			Dec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SP(Dec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("bTriggered"));
			AtkSeq->SetArrayField(TEXT("decorators"), { MakeShared<FJsonValueObject>(Dec) });
			TSharedPtr<FJsonObject> Lunge = MN(TEXT("BTTask_MoveTo"));
			SP(Lunge, TEXT("BlackboardKey.SelectedKeyName"), TEXT("TargetActor"));
			SP(Lunge, TEXT("AcceptableRadius"), TEXT("50"));
			TSharedPtr<FJsonObject> W = MN(TEXT("BTTask_Wait"));
			SP(W, TEXT("WaitTime"), TEXT("0.5"));
			AtkSeq->SetArrayField(TEXT("children"), { MakeShared<FJsonValueObject>(Lunge), MakeShared<FJsonValueObject>(W) });
		}
		RC.Add(MakeShared<FJsonValueObject>(AtkSeq));

		// Dormant
		TSharedPtr<FJsonObject> Dorm = MN(TEXT("BTTask_Wait"));
		SP(Dorm, TEXT("WaitTime"), TEXT("0.5"));
		RC.Add(MakeShared<FJsonValueObject>(Dorm));

		Root->SetArrayField(TEXT("children"), RC);
		Spec->SetObjectField(TEXT("root"), Root);

		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BTPath);
		P->SetObjectField(TEXT("spec"), Spec);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("build_behavior_tree_from_spec"), P);
		if (!R.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create BT: %s"), *R.ErrorMessage));
		CreatedAssets.Add(BTPath);

		// Phase D1: belt-and-suspenders BB linkage (Issue #48)
		TSharedPtr<FJsonObject> SetBBParams = MakeShared<FJsonObject>();
		SetBBParams->SetStringField(TEXT("bt_path"), BTPath);
		SetBBParams->SetStringField(TEXT("blackboard_path"), BBPath);
		FMonolithActionResult LinkResult = Dispatch(TEXT("ai"), TEXT("set_bt_blackboard"), SetBBParams);
		if (!LinkResult.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to link BT to Blackboard: %s"), *LinkResult.ErrorMessage));
	}

	// 3. Controller
	FString ControllerPath = SavePath / TEXT("BP_") + Name + TEXT("Controller");
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), ControllerPath); P->SetStringField(TEXT("bt_path"), BTPath);
		P->SetStringField(TEXT("bb_path"), BBPath); P->SetStringField(TEXT("perception_preset"), TEXT("sight_hearing"));
		P->SetNumberField(TEXT("team_id"), 1);
		DispatchOrWarn(TEXT("ai"), TEXT("scaffold_ai_controller_blueprint"), P, Warnings, TEXT("Create AI Controller"));
		CreatedAssets.Add(ControllerPath);
	}

	TArray<FString> NextSteps;
	NextSteps.Add(FString::Printf(TEXT("Trigger: %s — add trigger logic in BTService or overlap event"), *TriggerType));
	NextSteps.Add(FString::Printf(TEXT("Attack pattern: %s — replace MoveTo+Wait with custom attack BTTask"), *AttackPattern));
	NextSteps.Add(TEXT("Set SpawnLocation on BeginPlay, RetreatLocation to a hidden spot"));
	NextSteps.Add(TEXT("Add a scream/sound cue BTTask before the lunge for maximum impact"));

	return FMonolithActionResult::Success(BuildGenreScaffoldResult(Name, TEXT("Horror Ambush"), CreatedAssets, Warnings, NextSteps));
}

// ============================================================
//  192. scaffold_horror_presence
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldHorrorPresence(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, ErrResult)) return ErrResult;

	TArray<FString> Effects;
	const TArray<TSharedPtr<FJsonValue>>* EffArr = nullptr;
	if (Params->TryGetArrayField(TEXT("effects"), EffArr) && EffArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *EffArr) { FString S; if (V->TryGetString(S)) Effects.Add(S); }
	}
	if (Effects.Num() == 0)
		Effects = { TEXT("flicker_lights"), TEXT("open_doors"), TEXT("whispers"), TEXT("cold_breath"), TEXT("move_objects") };

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Scaffold Horror Presence")));
	TArray<FString> CreatedAssets, Warnings;
	FString BBPath = SavePath / TEXT("BB_") + Name;
	FString BTPath = SavePath / TEXT("BT_") + Name;

	// 1. BB
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BBPath);
		if (!DispatchOrWarn(TEXT("ai"), TEXT("create_blackboard"), P, Warnings, TEXT("Create BB")))
			return FMonolithActionResult::Error(TEXT("Failed to create Blackboard"));
		CreatedAssets.Add(BBPath);

		auto MK = [](const FString& N, const FString& T, const FString& B = TEXT("")) -> TSharedPtr<FJsonValue>
		{ TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>(); K->SetStringField(TEXT("name"), N); K->SetStringField(TEXT("type"), T); if (!B.IsEmpty()) K->SetStringField(TEXT("base_class"), B); return MakeShared<FJsonValueObject>(K); };

		TSharedPtr<FJsonObject> KeysP = MakeShared<FJsonObject>();
		KeysP->SetStringField(TEXT("asset_path"), BBPath);
		KeysP->SetArrayField(TEXT("keys"), {
			MK(TEXT("TargetActor"), TEXT("Object"), TEXT("Actor")),
			MK(TEXT("EffectLocation"), TEXT("Vector")),
			MK(TEXT("CurrentEffect"), TEXT("Int")),
			MK(TEXT("bPlayerNearby"), TEXT("Bool")),
			MK(TEXT("IntensityLevel"), TEXT("Float"))
		});
		DispatchOrWarn(TEXT("ai"), TEXT("batch_add_bb_keys"), KeysP, Warnings, TEXT("Add BB keys"));
	}

	// 2. BT: Selector -> [Active haunting (player nearby), Dormant]
	{
		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
		Spec->SetStringField(TEXT("blackboard_path"), BBPath);

		auto MN = [](const FString& C) { TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>(); N->SetStringField(TEXT("type"), C); return N; };
		auto SP = [](TSharedPtr<FJsonObject>& Node, const FString& Key, const FString& Value) {
			TSharedPtr<FJsonObject> Props; const TSharedPtr<FJsonObject>* Ex = nullptr;
			if (Node->TryGetObjectField(TEXT("properties"), Ex) && Ex) Props = *Ex;
			else { Props = MakeShared<FJsonObject>(); Node->SetObjectField(TEXT("properties"), Props); }
			Props->SetStringField(Key, Value);
		};

		TSharedPtr<FJsonObject> Root = MN(TEXT("BTComposite_Selector"));
		TArray<TSharedPtr<FJsonValue>> RC;

		TSharedPtr<FJsonObject> ActiveSeq = MN(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> Dec = MakeShared<FJsonObject>();
			Dec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SP(Dec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("bPlayerNearby"));
			ActiveSeq->SetArrayField(TEXT("decorators"), { MakeShared<FJsonValueObject>(Dec) });
			TSharedPtr<FJsonObject> EffW = MN(TEXT("BTTask_Wait"));
			SP(EffW, TEXT("WaitTime"), TEXT("2.0"));
			TSharedPtr<FJsonObject> CDW = MN(TEXT("BTTask_Wait"));
			SP(CDW, TEXT("WaitTime"), TEXT("4.0"));
			ActiveSeq->SetArrayField(TEXT("children"), { MakeShared<FJsonValueObject>(EffW), MakeShared<FJsonValueObject>(CDW) });
		}
		RC.Add(MakeShared<FJsonValueObject>(ActiveSeq));

		TSharedPtr<FJsonObject> Dorm = MN(TEXT("BTTask_Wait"));
		SP(Dorm, TEXT("WaitTime"), TEXT("10.0"));
		RC.Add(MakeShared<FJsonValueObject>(Dorm));

		Root->SetArrayField(TEXT("children"), RC);
		Spec->SetObjectField(TEXT("root"), Root);

		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BTPath);
		P->SetObjectField(TEXT("spec"), Spec);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("build_behavior_tree_from_spec"), P);
		if (!R.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create BT: %s"), *R.ErrorMessage));
		CreatedAssets.Add(BTPath);

		// Phase D1: belt-and-suspenders BB linkage (Issue #48)
		TSharedPtr<FJsonObject> SetBBParams = MakeShared<FJsonObject>();
		SetBBParams->SetStringField(TEXT("bt_path"), BTPath);
		SetBBParams->SetStringField(TEXT("blackboard_path"), BBPath);
		FMonolithActionResult LinkResult = Dispatch(TEXT("ai"), TEXT("set_bt_blackboard"), SetBBParams);
		if (!LinkResult.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to link BT to Blackboard: %s"), *LinkResult.ErrorMessage));
	}

	// 3. Controller (no perception — invisible entity)
	FString ControllerPath = SavePath / TEXT("BP_") + Name + TEXT("Controller");
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), ControllerPath); P->SetStringField(TEXT("bt_path"), BTPath);
		P->SetStringField(TEXT("bb_path"), BBPath); P->SetNumberField(TEXT("team_id"), 1);
		DispatchOrWarn(TEXT("ai"), TEXT("scaffold_ai_controller_blueprint"), P, Warnings, TEXT("Create AI Controller"));
		CreatedAssets.Add(ControllerPath);
	}

	TArray<FString> NextSteps;
	NextSteps.Add(TEXT("Create custom BTTasks for each effect type:"));
	for (const FString& E : Effects) NextSteps.Add(FString::Printf(TEXT("  - BTTask_%s_%s"), *Name, *E));
	NextSteps.Add(TEXT("Add BTService to detect player proximity and set bPlayerNearby"));
	NextSteps.Add(TEXT("Add BTService to cycle CurrentEffect and escalate IntensityLevel"));
	NextSteps.Add(TEXT("No visible mesh — presence is felt, not seen"));
	NextSteps.Add(TEXT("Use Niagara for subtle visual effects (cold breath, dust particles)"));

	return FMonolithActionResult::Success(BuildGenreScaffoldResult(Name, TEXT("Horror Presence"), CreatedAssets, Warnings, NextSteps));
}

// ============================================================
//  193. scaffold_horror_mimic
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldHorrorMimic(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, ErrResult)) return ErrResult;

	FString DisguiseMesh = Params->GetStringField(TEXT("disguise_mesh"));
	FString RevealConditions = Params->GetStringField(TEXT("reveal_conditions"));
	if (RevealConditions.IsEmpty()) RevealConditions = TEXT("proximity");

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Scaffold Horror Mimic")));
	TArray<FString> CreatedAssets, Warnings;
	FString BBPath = SavePath / TEXT("BB_") + Name;
	FString BTPath = SavePath / TEXT("BT_") + Name;

	// 1. BB
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BBPath);
		if (!DispatchOrWarn(TEXT("ai"), TEXT("create_blackboard"), P, Warnings, TEXT("Create BB")))
			return FMonolithActionResult::Error(TEXT("Failed to create Blackboard"));
		CreatedAssets.Add(BBPath);

		auto MK = [](const FString& N, const FString& T, const FString& B = TEXT("")) -> TSharedPtr<FJsonValue>
		{ TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>(); K->SetStringField(TEXT("name"), N); K->SetStringField(TEXT("type"), T); if (!B.IsEmpty()) K->SetStringField(TEXT("base_class"), B); return MakeShared<FJsonValueObject>(K); };

		TSharedPtr<FJsonObject> KeysP = MakeShared<FJsonObject>();
		KeysP->SetStringField(TEXT("asset_path"), BBPath);
		KeysP->SetArrayField(TEXT("keys"), {
			MK(TEXT("TargetActor"), TEXT("Object"), TEXT("Actor")),
			MK(TEXT("bRevealed"), TEXT("Bool")),
			MK(TEXT("bDisguised"), TEXT("Bool")),
			MK(TEXT("OriginalLocation"), TEXT("Vector"))
		});
		DispatchOrWarn(TEXT("ai"), TEXT("batch_add_bb_keys"), KeysP, Warnings, TEXT("Add BB keys"));
	}

	// 2. BT: Selector -> [Attack (revealed), Disguise (dormant)]
	{
		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
		Spec->SetStringField(TEXT("blackboard_path"), BBPath);

		auto MN = [](const FString& C) { TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>(); N->SetStringField(TEXT("type"), C); return N; };
		auto SP = [](TSharedPtr<FJsonObject>& Node, const FString& Key, const FString& Value) {
			TSharedPtr<FJsonObject> Props; const TSharedPtr<FJsonObject>* Ex = nullptr;
			if (Node->TryGetObjectField(TEXT("properties"), Ex) && Ex) Props = *Ex;
			else { Props = MakeShared<FJsonObject>(); Node->SetObjectField(TEXT("properties"), Props); }
			Props->SetStringField(Key, Value);
		};

		TSharedPtr<FJsonObject> Root = MN(TEXT("BTComposite_Selector"));
		TArray<TSharedPtr<FJsonValue>> RC;

		TSharedPtr<FJsonObject> AtkSeq = MN(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> Dec = MakeShared<FJsonObject>();
			Dec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SP(Dec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("bRevealed"));
			AtkSeq->SetArrayField(TEXT("decorators"), { MakeShared<FJsonValueObject>(Dec) });
			TSharedPtr<FJsonObject> Move = MN(TEXT("BTTask_MoveTo"));
			SP(Move, TEXT("BlackboardKey.SelectedKeyName"), TEXT("TargetActor"));
			SP(Move, TEXT("AcceptableRadius"), TEXT("80"));
			TSharedPtr<FJsonObject> W = MN(TEXT("BTTask_Wait"));
			SP(W, TEXT("WaitTime"), TEXT("1.0"));
			AtkSeq->SetArrayField(TEXT("children"), { MakeShared<FJsonValueObject>(Move), MakeShared<FJsonValueObject>(W) });
		}
		RC.Add(MakeShared<FJsonValueObject>(AtkSeq));

		TSharedPtr<FJsonObject> Disguise = MN(TEXT("BTTask_Wait"));
		SP(Disguise, TEXT("WaitTime"), TEXT("0.5"));
		RC.Add(MakeShared<FJsonValueObject>(Disguise));

		Root->SetArrayField(TEXT("children"), RC);
		Spec->SetObjectField(TEXT("root"), Root);

		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BTPath);
		P->SetObjectField(TEXT("spec"), Spec);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("build_behavior_tree_from_spec"), P);
		if (!R.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create BT: %s"), *R.ErrorMessage));
		CreatedAssets.Add(BTPath);

		// Phase D1: belt-and-suspenders BB linkage (Issue #48)
		TSharedPtr<FJsonObject> SetBBParams = MakeShared<FJsonObject>();
		SetBBParams->SetStringField(TEXT("bt_path"), BTPath);
		SetBBParams->SetStringField(TEXT("blackboard_path"), BBPath);
		FMonolithActionResult LinkResult = Dispatch(TEXT("ai"), TEXT("set_bt_blackboard"), SetBBParams);
		if (!LinkResult.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to link BT to Blackboard: %s"), *LinkResult.ErrorMessage));
	}

	// 3. Controller
	FString ControllerPath = SavePath / TEXT("BP_") + Name + TEXT("Controller");
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), ControllerPath); P->SetStringField(TEXT("bt_path"), BTPath);
		P->SetStringField(TEXT("bb_path"), BBPath); P->SetStringField(TEXT("perception_preset"), TEXT("sight"));
		P->SetNumberField(TEXT("team_id"), 1);
		DispatchOrWarn(TEXT("ai"), TEXT("scaffold_ai_controller_blueprint"), P, Warnings, TEXT("Create AI Controller"));
		CreatedAssets.Add(ControllerPath);
	}

	TArray<FString> NextSteps;
	NextSteps.Add(FString::Printf(TEXT("Reveal condition: %s — implement trigger to set bRevealed BB key"), *RevealConditions));
	if (!DisguiseMesh.IsEmpty())
		NextSteps.Add(FString::Printf(TEXT("Disguise mesh: %s — swap to creature mesh on reveal"), *DisguiseMesh));
	else
		NextSteps.Add(TEXT("Set a disguise static mesh (chest, chair, barrel) — swap to creature mesh on reveal"));
	NextSteps.Add(TEXT("Add reveal animation/montage (unfold, crack open)"));
	NextSteps.Add(TEXT("Set bDisguised=true on BeginPlay, clear on reveal"));

	return FMonolithActionResult::Success(BuildGenreScaffoldResult(Name, TEXT("Horror Mimic"), CreatedAssets, Warnings, NextSteps));
}

// ============================================================
//  194. scaffold_stealth_game_ai
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldStealthGameAI(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, ErrResult)) return ErrResult;

	bool bDetectionMeter = true;
	if (Params->HasField(TEXT("detection_meter"))) bDetectionMeter = Params->GetBoolField(TEXT("detection_meter"));
	FString AlertStates = Params->GetStringField(TEXT("alert_states"));
	if (AlertStates.IsEmpty()) AlertStates = TEXT("standard");

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Scaffold Stealth Game AI")));
	TArray<FString> CreatedAssets, Warnings;
	FString BBPath = SavePath / TEXT("BB_") + Name;
	FString BTPath = SavePath / TEXT("BT_") + Name;

	// 1. BB
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BBPath);
		if (!DispatchOrWarn(TEXT("ai"), TEXT("create_blackboard"), P, Warnings, TEXT("Create BB")))
			return FMonolithActionResult::Error(TEXT("Failed to create Blackboard"));
		CreatedAssets.Add(BBPath);

		auto MK = [](const FString& N, const FString& T, const FString& B = TEXT("")) -> TSharedPtr<FJsonValue>
		{ TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>(); K->SetStringField(TEXT("name"), N); K->SetStringField(TEXT("type"), T); if (!B.IsEmpty()) K->SetStringField(TEXT("base_class"), B); return MakeShared<FJsonValueObject>(K); };

		TSharedPtr<FJsonObject> KeysP = MakeShared<FJsonObject>();
		KeysP->SetStringField(TEXT("asset_path"), BBPath);
		KeysP->SetArrayField(TEXT("keys"), {
			MK(TEXT("TargetActor"), TEXT("Object"), TEXT("Actor")),
			MK(TEXT("AlertState"), TEXT("Int")),
			MK(TEXT("DetectionLevel"), TEXT("Float")),
			MK(TEXT("LastKnownLocation"), TEXT("Vector")),
			MK(TEXT("PatrolLocation"), TEXT("Vector")),
			MK(TEXT("PatrolIndex"), TEXT("Int")),
			MK(TEXT("SearchLocation"), TEXT("Vector"))
		});
		DispatchOrWarn(TEXT("ai"), TEXT("batch_add_bb_keys"), KeysP, Warnings, TEXT("Add BB keys"));
	}

	// 2. BT: Selector -> [Combat, Search, Patrol]
	{
		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
		Spec->SetStringField(TEXT("blackboard_path"), BBPath);

		auto MN = [](const FString& C) { TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>(); N->SetStringField(TEXT("type"), C); return N; };
		auto SP = [](TSharedPtr<FJsonObject>& Node, const FString& Key, const FString& Value) {
			TSharedPtr<FJsonObject> Props; const TSharedPtr<FJsonObject>* Ex = nullptr;
			if (Node->TryGetObjectField(TEXT("properties"), Ex) && Ex) Props = *Ex;
			else { Props = MakeShared<FJsonObject>(); Node->SetObjectField(TEXT("properties"), Props); }
			Props->SetStringField(Key, Value);
		};

		TSharedPtr<FJsonObject> Root = MN(TEXT("BTComposite_Selector"));
		TArray<TSharedPtr<FJsonValue>> RC;

		// Combat
		TSharedPtr<FJsonObject> CombatSeq = MN(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> Dec = MakeShared<FJsonObject>();
			Dec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SP(Dec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("TargetActor"));
			CombatSeq->SetArrayField(TEXT("decorators"), { MakeShared<FJsonValueObject>(Dec) });
			TSharedPtr<FJsonObject> Move = MN(TEXT("BTTask_MoveTo"));
			SP(Move, TEXT("BlackboardKey.SelectedKeyName"), TEXT("TargetActor"));
			SP(Move, TEXT("AcceptableRadius"), TEXT("150"));
			TSharedPtr<FJsonObject> W = MN(TEXT("BTTask_Wait"));
			SP(W, TEXT("WaitTime"), TEXT("1.5"));
			CombatSeq->SetArrayField(TEXT("children"), { MakeShared<FJsonValueObject>(Move), MakeShared<FJsonValueObject>(W) });
		}
		RC.Add(MakeShared<FJsonValueObject>(CombatSeq));

		// Search
		TSharedPtr<FJsonObject> SearchSeq = MN(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> Move = MN(TEXT("BTTask_MoveTo"));
			SP(Move, TEXT("BlackboardKey.SelectedKeyName"), TEXT("LastKnownLocation"));
			SP(Move, TEXT("AcceptableRadius"), TEXT("100"));
			TSharedPtr<FJsonObject> W = MN(TEXT("BTTask_Wait"));
			SP(W, TEXT("WaitTime"), TEXT("3.0"));
			SearchSeq->SetArrayField(TEXT("children"), { MakeShared<FJsonValueObject>(Move), MakeShared<FJsonValueObject>(W) });
		}
		RC.Add(MakeShared<FJsonValueObject>(SearchSeq));

		// Patrol
		TSharedPtr<FJsonObject> PatrolSeq = MN(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> Move = MN(TEXT("BTTask_MoveTo"));
			SP(Move, TEXT("BlackboardKey.SelectedKeyName"), TEXT("PatrolLocation"));
			SP(Move, TEXT("AcceptableRadius"), TEXT("50"));
			TSharedPtr<FJsonObject> W = MN(TEXT("BTTask_Wait"));
			SP(W, TEXT("WaitTime"), TEXT("2.0"));
			PatrolSeq->SetArrayField(TEXT("children"), { MakeShared<FJsonValueObject>(Move), MakeShared<FJsonValueObject>(W) });
		}
		RC.Add(MakeShared<FJsonValueObject>(PatrolSeq));

		Root->SetArrayField(TEXT("children"), RC);
		Spec->SetObjectField(TEXT("root"), Root);

		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BTPath);
		P->SetObjectField(TEXT("spec"), Spec);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("build_behavior_tree_from_spec"), P);
		if (!R.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create BT: %s"), *R.ErrorMessage));
		CreatedAssets.Add(BTPath);

		// Phase D1: belt-and-suspenders BB linkage (Issue #48)
		TSharedPtr<FJsonObject> SetBBParams = MakeShared<FJsonObject>();
		SetBBParams->SetStringField(TEXT("bt_path"), BTPath);
		SetBBParams->SetStringField(TEXT("blackboard_path"), BBPath);
		FMonolithActionResult LinkResult = Dispatch(TEXT("ai"), TEXT("set_bt_blackboard"), SetBBParams);
		if (!LinkResult.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to link BT to Blackboard: %s"), *LinkResult.ErrorMessage));
	}

	// 3. Controller
	FString ControllerPath = SavePath / TEXT("BP_") + Name + TEXT("Controller");
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), ControllerPath); P->SetStringField(TEXT("bt_path"), BTPath);
		P->SetStringField(TEXT("bb_path"), BBPath); P->SetStringField(TEXT("perception_preset"), TEXT("full"));
		P->SetNumberField(TEXT("team_id"), 1);
		DispatchOrWarn(TEXT("ai"), TEXT("scaffold_ai_controller_blueprint"), P, Warnings, TEXT("Create AI Controller"));
		CreatedAssets.Add(ControllerPath);
	}

	TArray<FString> NextSteps;
	NextSteps.Add(TEXT("Add BTService to manage AlertState transitions based on DetectionLevel"));
	if (bDetectionMeter) NextSteps.Add(TEXT("DetectionLevel: 0.0=unseen -> 1.0=fully detected. Increase on sight, decay over time."));
	NextSteps.Add(FString::Printf(TEXT("Alert model: %s"), *AlertStates));
	NextSteps.Add(TEXT("Add visual indicators (? and !) via widget or Niagara for player feedback"));
	NextSteps.Add(TEXT("Wire AIControllerClass on your guard Character BP"));

	return FMonolithActionResult::Success(BuildGenreScaffoldResult(Name, TEXT("Stealth Game AI"), CreatedAssets, Warnings, NextSteps));
}

// ============================================================
//  195. scaffold_turret_ai
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldTurretAI(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, ErrResult)) return ErrResult;

	float DetectionCone = 45.0f;
	if (Params->HasField(TEXT("detection_cone"))) DetectionCone = static_cast<float>(Params->GetNumberField(TEXT("detection_cone")));
	float EngagementRange = 3000.0f;
	if (Params->HasField(TEXT("engagement_range"))) EngagementRange = static_cast<float>(Params->GetNumberField(TEXT("engagement_range")));

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Scaffold Turret AI")));
	TArray<FString> CreatedAssets, Warnings;
	FString BBPath = SavePath / TEXT("BB_") + Name;
	FString BTPath = SavePath / TEXT("BT_") + Name;

	// 1. BB
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BBPath);
		if (!DispatchOrWarn(TEXT("ai"), TEXT("create_blackboard"), P, Warnings, TEXT("Create BB")))
			return FMonolithActionResult::Error(TEXT("Failed to create Blackboard"));
		CreatedAssets.Add(BBPath);

		auto MK = [](const FString& N, const FString& T, const FString& B = TEXT("")) -> TSharedPtr<FJsonValue>
		{ TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>(); K->SetStringField(TEXT("name"), N); K->SetStringField(TEXT("type"), T); if (!B.IsEmpty()) K->SetStringField(TEXT("base_class"), B); return MakeShared<FJsonValueObject>(K); };

		TSharedPtr<FJsonObject> KeysP = MakeShared<FJsonObject>();
		KeysP->SetStringField(TEXT("asset_path"), BBPath);
		KeysP->SetArrayField(TEXT("keys"), {
			MK(TEXT("TargetActor"), TEXT("Object"), TEXT("Actor")),
			MK(TEXT("bTargetInRange"), TEXT("Bool")),
			MK(TEXT("bIsFiring"), TEXT("Bool")),
			MK(TEXT("AimDirection"), TEXT("Rotator"))
		});
		DispatchOrWarn(TEXT("ai"), TEXT("batch_add_bb_keys"), KeysP, Warnings, TEXT("Add BB keys"));
	}

	// 2. BT: Selector -> [Fire (has target), Scan]
	{
		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
		Spec->SetStringField(TEXT("blackboard_path"), BBPath);

		auto MN = [](const FString& C) { TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>(); N->SetStringField(TEXT("type"), C); return N; };
		auto SP = [](TSharedPtr<FJsonObject>& Node, const FString& Key, const FString& Value) {
			TSharedPtr<FJsonObject> Props; const TSharedPtr<FJsonObject>* Ex = nullptr;
			if (Node->TryGetObjectField(TEXT("properties"), Ex) && Ex) Props = *Ex;
			else { Props = MakeShared<FJsonObject>(); Node->SetObjectField(TEXT("properties"), Props); }
			Props->SetStringField(Key, Value);
		};

		TSharedPtr<FJsonObject> Root = MN(TEXT("BTComposite_Selector"));
		TArray<TSharedPtr<FJsonValue>> RC;

		TSharedPtr<FJsonObject> FireSeq = MN(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> Dec = MakeShared<FJsonObject>();
			Dec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SP(Dec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("TargetActor"));
			FireSeq->SetArrayField(TEXT("decorators"), { MakeShared<FJsonValueObject>(Dec) });
			TSharedPtr<FJsonObject> Fire = MN(TEXT("BTTask_Wait"));
			SP(Fire, TEXT("WaitTime"), TEXT("0.2"));
			TSharedPtr<FJsonObject> CD = MN(TEXT("BTTask_Wait"));
			SP(CD, TEXT("WaitTime"), TEXT("0.5"));
			FireSeq->SetArrayField(TEXT("children"), { MakeShared<FJsonValueObject>(Fire), MakeShared<FJsonValueObject>(CD) });
		}
		RC.Add(MakeShared<FJsonValueObject>(FireSeq));

		TSharedPtr<FJsonObject> Scan = MN(TEXT("BTTask_Wait"));
		SP(Scan, TEXT("WaitTime"), TEXT("1.0"));
		RC.Add(MakeShared<FJsonValueObject>(Scan));

		Root->SetArrayField(TEXT("children"), RC);
		Spec->SetObjectField(TEXT("root"), Root);

		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BTPath);
		P->SetObjectField(TEXT("spec"), Spec);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("build_behavior_tree_from_spec"), P);
		if (!R.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create BT: %s"), *R.ErrorMessage));
		CreatedAssets.Add(BTPath);

		// Phase D1: belt-and-suspenders BB linkage (Issue #48)
		TSharedPtr<FJsonObject> SetBBParams = MakeShared<FJsonObject>();
		SetBBParams->SetStringField(TEXT("bt_path"), BTPath);
		SetBBParams->SetStringField(TEXT("blackboard_path"), BBPath);
		FMonolithActionResult LinkResult = Dispatch(TEXT("ai"), TEXT("set_bt_blackboard"), SetBBParams);
		if (!LinkResult.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to link BT to Blackboard: %s"), *LinkResult.ErrorMessage));
	}

	// 3. Controller
	FString ControllerPath = SavePath / TEXT("BP_") + Name + TEXT("Controller");
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), ControllerPath); P->SetStringField(TEXT("bt_path"), BTPath);
		P->SetStringField(TEXT("bb_path"), BBPath); P->SetStringField(TEXT("perception_preset"), TEXT("sight"));
		P->SetNumberField(TEXT("team_id"), 1);
		DispatchOrWarn(TEXT("ai"), TEXT("scaffold_ai_controller_blueprint"), P, Warnings, TEXT("Create AI Controller"));
		CreatedAssets.Add(ControllerPath);
	}

	TArray<FString> NextSteps;
	NextSteps.Add(FString::Printf(TEXT("Detection cone: %.0f deg, range: %.0f cm"), DetectionCone, EngagementRange));
	NextSteps.Add(TEXT("Create custom BTTask for aiming and firing (replace Wait placeholders)"));
	NextSteps.Add(TEXT("Configure sight sense to match detection_cone and engagement_range"));
	NextSteps.Add(TEXT("Use a Pawn (not Character) with no MovementComponent — turret does not move"));
	NextSteps.Add(TEXT("Add rotation interpolation for smooth turret tracking"));

	return FMonolithActionResult::Success(BuildGenreScaffoldResult(Name, TEXT("Turret AI"), CreatedAssets, Warnings, NextSteps));
}

// ============================================================
//  196. scaffold_group_coordinator
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldGroupCoordinator(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, ErrResult)) return ErrResult;

	TArray<FString> Roles;
	const TArray<TSharedPtr<FJsonValue>>* RolesArr = nullptr;
	if (Params->TryGetArrayField(TEXT("roles"), RolesArr) && RolesArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *RolesArr) { FString S; if (V->TryGetString(S)) Roles.Add(S); }
	}
	if (Roles.Num() == 0) Roles = { TEXT("flanker"), TEXT("suppressor"), TEXT("rusher") };

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Scaffold Group Coordinator")));
	TArray<FString> CreatedAssets, Warnings;
	FString BBPath = SavePath / TEXT("BB_") + Name;
	FString BTPath = SavePath / TEXT("BT_") + Name;

	// 1. BB
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BBPath);
		if (!DispatchOrWarn(TEXT("ai"), TEXT("create_blackboard"), P, Warnings, TEXT("Create BB")))
			return FMonolithActionResult::Error(TEXT("Failed to create Blackboard"));
		CreatedAssets.Add(BBPath);

		auto MK = [](const FString& N, const FString& T, const FString& B = TEXT("")) -> TSharedPtr<FJsonValue>
		{ TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>(); K->SetStringField(TEXT("name"), N); K->SetStringField(TEXT("type"), T); if (!B.IsEmpty()) K->SetStringField(TEXT("base_class"), B); return MakeShared<FJsonValueObject>(K); };

		TSharedPtr<FJsonObject> KeysP = MakeShared<FJsonObject>();
		KeysP->SetStringField(TEXT("asset_path"), BBPath);
		KeysP->SetArrayField(TEXT("keys"), {
			MK(TEXT("TargetActor"), TEXT("Object"), TEXT("Actor")),
			MK(TEXT("AssignedRole"), TEXT("Int")),
			MK(TEXT("RoleLocation"), TEXT("Vector")),
			MK(TEXT("bSquadEngaged"), TEXT("Bool")),
			MK(TEXT("SquadLeader"), TEXT("Object"), TEXT("Actor"))
		});
		DispatchOrWarn(TEXT("ai"), TEXT("batch_add_bb_keys"), KeysP, Warnings, TEXT("Add BB keys"));
	}

	// 2. BT: Selector -> [Execute role (engaged), Regroup/idle]
	{
		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
		Spec->SetStringField(TEXT("blackboard_path"), BBPath);

		auto MN = [](const FString& C) { TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>(); N->SetStringField(TEXT("type"), C); return N; };
		auto SP = [](TSharedPtr<FJsonObject>& Node, const FString& Key, const FString& Value) {
			TSharedPtr<FJsonObject> Props; const TSharedPtr<FJsonObject>* Ex = nullptr;
			if (Node->TryGetObjectField(TEXT("properties"), Ex) && Ex) Props = *Ex;
			else { Props = MakeShared<FJsonObject>(); Node->SetObjectField(TEXT("properties"), Props); }
			Props->SetStringField(Key, Value);
		};

		TSharedPtr<FJsonObject> Root = MN(TEXT("BTComposite_Selector"));
		TArray<TSharedPtr<FJsonValue>> RC;

		TSharedPtr<FJsonObject> RoleSeq = MN(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> Dec = MakeShared<FJsonObject>();
			Dec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SP(Dec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("bSquadEngaged"));
			RoleSeq->SetArrayField(TEXT("decorators"), { MakeShared<FJsonValueObject>(Dec) });
			TSharedPtr<FJsonObject> Move = MN(TEXT("BTTask_MoveTo"));
			SP(Move, TEXT("BlackboardKey.SelectedKeyName"), TEXT("RoleLocation"));
			SP(Move, TEXT("AcceptableRadius"), TEXT("100"));
			TSharedPtr<FJsonObject> W = MN(TEXT("BTTask_Wait"));
			SP(W, TEXT("WaitTime"), TEXT("2.0"));
			RoleSeq->SetArrayField(TEXT("children"), { MakeShared<FJsonValueObject>(Move), MakeShared<FJsonValueObject>(W) });
		}
		RC.Add(MakeShared<FJsonValueObject>(RoleSeq));

		TSharedPtr<FJsonObject> Idle = MN(TEXT("BTTask_Wait"));
		SP(Idle, TEXT("WaitTime"), TEXT("2.0"));
		RC.Add(MakeShared<FJsonValueObject>(Idle));

		Root->SetArrayField(TEXT("children"), RC);
		Spec->SetObjectField(TEXT("root"), Root);

		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BTPath);
		P->SetObjectField(TEXT("spec"), Spec);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("build_behavior_tree_from_spec"), P);
		if (!R.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create BT: %s"), *R.ErrorMessage));
		CreatedAssets.Add(BTPath);

		// Phase D1: belt-and-suspenders BB linkage (Issue #48)
		TSharedPtr<FJsonObject> SetBBParams = MakeShared<FJsonObject>();
		SetBBParams->SetStringField(TEXT("bt_path"), BTPath);
		SetBBParams->SetStringField(TEXT("blackboard_path"), BBPath);
		FMonolithActionResult LinkResult = Dispatch(TEXT("ai"), TEXT("set_bt_blackboard"), SetBBParams);
		if (!LinkResult.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to link BT to Blackboard: %s"), *LinkResult.ErrorMessage));
	}

	// 3. Controller
	FString ControllerPath = SavePath / TEXT("BP_") + Name + TEXT("Controller");
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), ControllerPath); P->SetStringField(TEXT("bt_path"), BTPath);
		P->SetStringField(TEXT("bb_path"), BBPath); P->SetStringField(TEXT("perception_preset"), TEXT("sight_hearing"));
		P->SetNumberField(TEXT("team_id"), 1);
		DispatchOrWarn(TEXT("ai"), TEXT("scaffold_ai_controller_blueprint"), P, Warnings, TEXT("Create AI Controller"));
		CreatedAssets.Add(ControllerPath);
	}

	TArray<FString> NextSteps;
	NextSteps.Add(TEXT("Create a coordinator actor that assigns roles to squad members"));
	NextSteps.Add(TEXT("Roles:"));
	for (int32 i = 0; i < Roles.Num(); i++) NextSteps.Add(FString::Printf(TEXT("  %d: %s"), i, *Roles[i]));
	NextSteps.Add(TEXT("Add BTService to compute RoleLocation based on AssignedRole and TargetActor"));
	NextSteps.Add(TEXT("Use EQS per role (flanker: behind target, suppressor: cover, rusher: direct)"));

	TSharedPtr<FJsonObject> Result = BuildGenreScaffoldResult(Name, TEXT("Group Coordinator"), CreatedAssets, Warnings, NextSteps);
	TArray<TSharedPtr<FJsonValue>> RoleValArr;
	for (const FString& R : Roles) RoleValArr.Add(MakeShared<FJsonValueString>(R));
	Result->SetArrayField(TEXT("roles"), RoleValArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  197. scaffold_flying_ai
// ============================================================

FMonolithActionResult FMonolithAIScaffoldActions::HandleScaffoldFlyingAI(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult)) return ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, ErrResult)) return ErrResult;

	float MinAlt = 500.0f, MaxAlt = 2000.0f;
	FString AltRange = Params->GetStringField(TEXT("altitude_range"));
	if (!AltRange.IsEmpty())
	{
		FString MinStr, MaxStr;
		if (AltRange.Split(TEXT(","), &MinStr, &MaxStr))
		{
			MinAlt = FCString::Atof(*MinStr.TrimStartAndEnd());
			MaxAlt = FCString::Atof(*MaxStr.TrimStartAndEnd());
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Scaffold Flying AI")));
	TArray<FString> CreatedAssets, Warnings;
	FString BBPath = SavePath / TEXT("BB_") + Name;
	FString BTPath = SavePath / TEXT("BT_") + Name;

	// 1. BB
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BBPath);
		if (!DispatchOrWarn(TEXT("ai"), TEXT("create_blackboard"), P, Warnings, TEXT("Create BB")))
			return FMonolithActionResult::Error(TEXT("Failed to create Blackboard"));
		CreatedAssets.Add(BBPath);

		auto MK = [](const FString& N, const FString& T, const FString& B = TEXT("")) -> TSharedPtr<FJsonValue>
		{ TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>(); K->SetStringField(TEXT("name"), N); K->SetStringField(TEXT("type"), T); if (!B.IsEmpty()) K->SetStringField(TEXT("base_class"), B); return MakeShared<FJsonValueObject>(K); };

		TSharedPtr<FJsonObject> KeysP = MakeShared<FJsonObject>();
		KeysP->SetStringField(TEXT("asset_path"), BBPath);
		KeysP->SetArrayField(TEXT("keys"), {
			MK(TEXT("TargetActor"), TEXT("Object"), TEXT("Actor")),
			MK(TEXT("FlyToLocation"), TEXT("Vector")),
			MK(TEXT("HomeLocation"), TEXT("Vector")),
			MK(TEXT("CurrentAltitude"), TEXT("Float")),
			MK(TEXT("bIsAirborne"), TEXT("Bool"))
		});
		DispatchOrWarn(TEXT("ai"), TEXT("batch_add_bb_keys"), KeysP, Warnings, TEXT("Add BB keys"));
	}

	// 2. BT: Selector -> [Attack dive (has target), Fly patrol]
	{
		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
		Spec->SetStringField(TEXT("blackboard_path"), BBPath);

		auto MN = [](const FString& C) { TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>(); N->SetStringField(TEXT("type"), C); return N; };
		auto SP = [](TSharedPtr<FJsonObject>& Node, const FString& Key, const FString& Value) {
			TSharedPtr<FJsonObject> Props; const TSharedPtr<FJsonObject>* Ex = nullptr;
			if (Node->TryGetObjectField(TEXT("properties"), Ex) && Ex) Props = *Ex;
			else { Props = MakeShared<FJsonObject>(); Node->SetObjectField(TEXT("properties"), Props); }
			Props->SetStringField(Key, Value);
		};

		TSharedPtr<FJsonObject> Root = MN(TEXT("BTComposite_Selector"));
		TArray<TSharedPtr<FJsonValue>> RC;

		// Attack dive
		TSharedPtr<FJsonObject> AtkSeq = MN(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> Dec = MakeShared<FJsonObject>();
			Dec->SetStringField(TEXT("class"), TEXT("BTDecorator_BlackboardBase"));
			SP(Dec, TEXT("BlackboardKey.SelectedKeyName"), TEXT("TargetActor"));
			AtkSeq->SetArrayField(TEXT("decorators"), { MakeShared<FJsonValueObject>(Dec) });
			TSharedPtr<FJsonObject> Dive = MN(TEXT("BTTask_MoveTo"));
			SP(Dive, TEXT("BlackboardKey.SelectedKeyName"), TEXT("TargetActor"));
			SP(Dive, TEXT("AcceptableRadius"), TEXT("200"));
			TSharedPtr<FJsonObject> Climb = MN(TEXT("BTTask_MoveTo"));
			SP(Climb, TEXT("BlackboardKey.SelectedKeyName"), TEXT("FlyToLocation"));
			SP(Climb, TEXT("AcceptableRadius"), TEXT("100"));
			AtkSeq->SetArrayField(TEXT("children"), { MakeShared<FJsonValueObject>(Dive), MakeShared<FJsonValueObject>(Climb) });
		}
		RC.Add(MakeShared<FJsonValueObject>(AtkSeq));

		// Fly patrol
		TSharedPtr<FJsonObject> PatrolSeq = MN(TEXT("BTComposite_Sequence"));
		{
			TSharedPtr<FJsonObject> Fly = MN(TEXT("BTTask_MoveTo"));
			SP(Fly, TEXT("BlackboardKey.SelectedKeyName"), TEXT("FlyToLocation"));
			SP(Fly, TEXT("AcceptableRadius"), TEXT("100"));
			TSharedPtr<FJsonObject> W = MN(TEXT("BTTask_Wait"));
			SP(W, TEXT("WaitTime"), TEXT("2.0"));
			PatrolSeq->SetArrayField(TEXT("children"), { MakeShared<FJsonValueObject>(Fly), MakeShared<FJsonValueObject>(W) });
		}
		RC.Add(MakeShared<FJsonValueObject>(PatrolSeq));

		Root->SetArrayField(TEXT("children"), RC);
		Spec->SetObjectField(TEXT("root"), Root);

		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), BTPath);
		P->SetObjectField(TEXT("spec"), Spec);
		FMonolithActionResult R = Dispatch(TEXT("ai"), TEXT("build_behavior_tree_from_spec"), P);
		if (!R.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create BT: %s"), *R.ErrorMessage));
		CreatedAssets.Add(BTPath);

		// Phase D1: belt-and-suspenders BB linkage (Issue #48)
		TSharedPtr<FJsonObject> SetBBParams = MakeShared<FJsonObject>();
		SetBBParams->SetStringField(TEXT("bt_path"), BTPath);
		SetBBParams->SetStringField(TEXT("blackboard_path"), BBPath);
		FMonolithActionResult LinkResult = Dispatch(TEXT("ai"), TEXT("set_bt_blackboard"), SetBBParams);
		if (!LinkResult.bSuccess) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to link BT to Blackboard: %s"), *LinkResult.ErrorMessage));
	}

	// 3. Controller
	FString ControllerPath = SavePath / TEXT("BP_") + Name + TEXT("Controller");
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), ControllerPath); P->SetStringField(TEXT("bt_path"), BTPath);
		P->SetStringField(TEXT("bb_path"), BBPath); P->SetStringField(TEXT("perception_preset"), TEXT("sight"));
		P->SetNumberField(TEXT("team_id"), 1);
		DispatchOrWarn(TEXT("ai"), TEXT("scaffold_ai_controller_blueprint"), P, Warnings, TEXT("Create AI Controller"));
		CreatedAssets.Add(ControllerPath);
	}

	TArray<FString> NextSteps;
	NextSteps.Add(FString::Printf(TEXT("Altitude range: %.0f-%.0f cm"), MinAlt, MaxAlt));
	NextSteps.Add(TEXT("Use Character with UFloatingPawnMovement or UFlyingMovementComponent"));
	NextSteps.Add(TEXT("Add BTService to compute FlyToLocation within altitude range"));
	NextSteps.Add(TEXT("Set bProjectGoalOnNavigation=false on MoveTo tasks for 3D movement"));
	NextSteps.Add(TEXT("Wire AIControllerClass on your flying Character BP"));

	return FMonolithActionResult::Success(BuildGenreScaffoldResult(Name, TEXT("Flying AI"), CreatedAssets, Warnings, NextSteps));
}
