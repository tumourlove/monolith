#include "MonolithAIControllerActions.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "NavFilters/NavigationQueryFilter.h"
#include "GenericTeamAgentInterface.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "EngineUtils.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "ScopedTransaction.h"

// ============================================================
//  Registration
// ============================================================

void FMonolithAIControllerActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 93. create_ai_controller
	Registry.RegisterAction(TEXT("ai"), TEXT("create_ai_controller"),
		TEXT("Create an AAIController Blueprint, optionally setting default BT and BB"),
		FMonolithActionHandler::CreateStatic(&HandleCreateAIController),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path (e.g. /Game/AI/BP_EnemyAIController)"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Asset name (derived from save_path if omitted)"))
			.Optional(TEXT("bt_path"), TEXT("string"), TEXT("Default Behavior Tree asset path"))
			.Optional(TEXT("bb_path"), TEXT("string"), TEXT("Default Blackboard asset path"))
			.Build());

	// 94. get_ai_controller
	Registry.RegisterAction(TEXT("ai"), TEXT("get_ai_controller"),
		TEXT("Read AI controller config: default BT, BB, perception setup, flags"),
		FMonolithActionHandler::CreateStatic(&HandleGetAIController),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AIController Blueprint asset path"))
			.Build());

	// 95. list_ai_controllers
	Registry.RegisterAction(TEXT("ai"), TEXT("list_ai_controllers"),
		TEXT("List all AAIController Blueprint assets in the project"),
		FMonolithActionHandler::CreateStatic(&HandleListAIControllers),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Only include assets under this path prefix"))
			.Build());

	// 96. set_ai_controller_bt
	Registry.RegisterAction(TEXT("ai"), TEXT("set_ai_controller_bt"),
		TEXT("Set default Behavior Tree and optionally Blackboard on an AI controller CDO"),
		FMonolithActionHandler::CreateStatic(&HandleSetAIControllerBT),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AIController Blueprint asset path"))
			.Required(TEXT("bt_path"), TEXT("string"), TEXT("Behavior Tree asset path"))
			.Optional(TEXT("bb_path"), TEXT("string"), TEXT("Blackboard asset path (uses BT's BB if omitted)"))
			.Build());

	// 100b. set_pawn_ai_controller_class
	Registry.RegisterAction(TEXT("ai"), TEXT("set_pawn_ai_controller_class"),
		TEXT("Set AIControllerClass on a Pawn or Character Blueprint CDO"),
		FMonolithActionHandler::CreateStatic(&HandleSetPawnAIControllerClass),
		FParamSchemaBuilder()
			.Required(TEXT("blueprint_path"), TEXT("string"), TEXT("Pawn/Character Blueprint asset path"))
			.Required(TEXT("controller_class"), TEXT("string"), TEXT("AIController class or Blueprint path"))
			.Build());

	// 97. set_ai_controller_flags
	Registry.RegisterAction(TEXT("ai"), TEXT("set_ai_controller_flags"),
		TEXT("Set boolean config flags on an AI controller CDO (wants_player_state, start_ai_on_possess, etc.)"),
		FMonolithActionHandler::CreateStatic(&HandleSetAIControllerFlags),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AIController Blueprint asset path"))
			.Optional(TEXT("wants_player_state"), TEXT("boolean"), TEXT("Whether this controller creates a PlayerState"))
			.Optional(TEXT("start_ai_on_possess"), TEXT("boolean"), TEXT("Auto-start AI logic when possessing a pawn"))
			.Optional(TEXT("skip_extra_los_checks"), TEXT("boolean"), TEXT("Skip extra line-of-sight checks (bSkipExtraLOSChecks)"))
			.Optional(TEXT("allow_strafe"), TEXT("boolean"), TEXT("Allow strafing movement"))
			.Build());

	// 98. set_ai_team
	Registry.RegisterAction(TEXT("ai"), TEXT("set_ai_team"),
		TEXT("Set the generic team ID (0-254) on an AI controller CDO for affiliation-based perception"),
		FMonolithActionHandler::CreateStatic(&HandleSetAITeam),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AIController Blueprint asset path"))
			.Required(TEXT("team_id"), TEXT("number"), TEXT("Team ID (0-254, 255 = NoTeam)"))
			.Build());

	// 99. get_ai_team
	Registry.RegisterAction(TEXT("ai"), TEXT("get_ai_team"),
		TEXT("Read the generic team ID from an AI controller CDO"),
		FMonolithActionHandler::CreateStatic(&HandleGetAITeam),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AIController Blueprint asset path"))
			.Build());

	// 103. spawn_ai_actor
	Registry.RegisterAction(TEXT("ai"), TEXT("spawn_ai_actor"),
		TEXT("Spawn an AI pawn/character Blueprint actor in the editor level"),
		FMonolithActionHandler::CreateStatic(&HandleSpawnAIActor),
		FParamSchemaBuilder()
			.Required(TEXT("class_path"), TEXT("string"), TEXT("Blueprint asset path (e.g. /Game/AI/BP_Enemy)"))
			.Required(TEXT("location"), TEXT("object"), TEXT("Spawn location as {x,y,z}"))
			.Optional(TEXT("rotation"), TEXT("object"), TEXT("Spawn rotation as {pitch,yaw,roll}"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Actor label in the Outliner"))
			.Optional(TEXT("folder_path"), TEXT("string"), TEXT("World Outliner folder (default: AI)"))
			.Build());

	// 104. get_ai_actors
	Registry.RegisterAction(TEXT("ai"), TEXT("get_ai_actors"),
		TEXT("List all AI-controlled actors in the current PIE world"),
		FMonolithActionHandler::CreateStatic(&HandleGetAIActors),
		FParamSchemaBuilder()
			.Optional(TEXT("class_filter"), TEXT("string"), TEXT("Filter by actor class name (e.g. 'BP_EnemyCharacter')"))
			.Build());
}

// ============================================================
//  93. create_ai_controller
// ============================================================

FMonolithActionResult FMonolithAIControllerActions::HandleCreateAIController(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult))
	{
		return ErrResult;
	}

	FString AssetName = Params->GetStringField(TEXT("name"));
	if (AssetName.IsEmpty())
	{
		AssetName = FPackageName::GetShortName(SavePath);
	}

	FString PackagePath = SavePath;

	// Check path is free
	FString PathError;
	if (!MonolithAI::EnsureAssetPathFree(PackagePath, AssetName, PathError))
	{
		return FMonolithActionResult::Error(PathError);
	}

	FString PkgError;
	UPackage* Package = MonolithAI::GetOrCreatePackage(PackagePath, PkgError);
	if (!Package)
	{
		return FMonolithActionResult::Error(PkgError);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Create AI Controller")));

	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		AAIController::StaticClass(),
		Package,
		*AssetName,
		BPTYPE_Normal,
		FName(TEXT("MonolithAI")));

	if (!NewBP)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create AIController Blueprint"));
	}

	// AAIController doesn't have a simple DefaultBehaviorTree UPROPERTY.
	// BT is run via RunBehaviorTree() at runtime, typically triggered by bStartAILogicOnPossess.
	// We validate the BT/BB exist and enable auto-start. The actual BT assignment
	// happens at runtime in the controller's OnPossess → RunBehaviorTree flow.
	// Users should override RunBehaviorTree in their BP or set the BT in BeginPlay.
	FString BTPath = Params->GetStringField(TEXT("bt_path"));
	FString BBPath = Params->GetStringField(TEXT("bb_path"));
	bool bBTValid = false;

	if (!BTPath.IsEmpty())
	{
		UBehaviorTree* BT = Cast<UBehaviorTree>(FMonolithAssetUtils::LoadAssetByPath(UBehaviorTree::StaticClass(), BTPath));
		if (BT)
		{
			bBTValid = true;
			// Compile so we can access CDO
			FKismetEditorUtilities::CompileBlueprint(NewBP);
			if (NewBP->GeneratedClass)
			{
				AAIController* CDO = Cast<AAIController>(NewBP->GeneratedClass->GetDefaultObject());
				if (CDO)
				{
					// Enable auto-start on possess
					if (FBoolProperty* StartProp = CastField<FBoolProperty>(NewBP->GeneratedClass->FindPropertyByName(TEXT("bStartAILogicOnPossess"))))
					{
						StartProp->SetPropertyValue_InContainer(CDO, true);
					}
				}
			}
		}
		else
		{
			UE_LOG(LogMonolithAI, Warning, TEXT("BT not found: %s"), *BTPath);
		}
	}

	FAssetRegistryModule::AssetCreated(NewBP);
	NewBP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(PackagePath, TEXT("AI Controller Blueprint created"));
	Result->SetStringField(TEXT("name"), AssetName);
	Result->SetStringField(TEXT("parent_class"), TEXT("AAIController"));
	if (!BTPath.IsEmpty())
	{
		Result->SetStringField(TEXT("bt_path"), BTPath);
		Result->SetBoolField(TEXT("bt_validated"), bBTValid);
		Result->SetStringField(TEXT("bt_note"), TEXT("BT runs via RunBehaviorTree() at runtime. bStartAILogicOnPossess enabled. Override RunBehaviorTree in BP or set BT in BeginPlay."));
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  94. get_ai_controller
// ============================================================

FMonolithActionResult FMonolithAIControllerActions::HandleGetAIController(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBlueprint* BP = MonolithAI::LoadAIControllerFromParams(Params, AssetPath, Error);
	if (!BP)
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("name"), BP->GetName());

	if (BP->GeneratedClass)
	{
		Result->SetStringField(TEXT("generated_class"), BP->GeneratedClass->GetPathName());
		Result->SetStringField(TEXT("parent_class"), BP->GeneratedClass->GetSuperClass()->GetName());

		AAIController* CDO = Cast<AAIController>(BP->GeneratedClass->GetDefaultObject());
		if (CDO)
		{
			// Public flags
			Result->SetBoolField(TEXT("allow_strafe"), !!CDO->bAllowStrafe);
			Result->SetBoolField(TEXT("wants_player_state"), !!CDO->bWantsPlayerState);

			// Protected flags via reflection
			UClass* AIClass = BP->GeneratedClass;
			if (FBoolProperty* StartProp = CastField<FBoolProperty>(AIClass->FindPropertyByName(TEXT("bStartAILogicOnPossess"))))
			{
				Result->SetBoolField(TEXT("start_logic_on_possess"), StartProp->GetPropertyValue_InContainer(CDO));
			}
			if (FBoolProperty* StopProp = CastField<FBoolProperty>(AIClass->FindPropertyByName(TEXT("bStopAILogicOnUnpossess"))))
			{
				Result->SetBoolField(TEXT("stop_logic_on_unpossess"), StopProp->GetPropertyValue_InContainer(CDO));
			}

			// Perception component
			if (CDO->PerceptionComponent)
			{
				TSharedPtr<FJsonObject> PerceptionObj = MakeShared<FJsonObject>();
				PerceptionObj->SetStringField(TEXT("class"), CDO->PerceptionComponent->GetClass()->GetName());
				Result->SetObjectField(TEXT("perception"), PerceptionObj);
			}

			// Brain component (BehaviorTreeComponent)
			if (CDO->BrainComponent)
			{
				TSharedPtr<FJsonObject> BrainObj = MakeShared<FJsonObject>();
				BrainObj->SetStringField(TEXT("class"), CDO->BrainComponent->GetClass()->GetName());

				UBehaviorTreeComponent* BTComp = Cast<UBehaviorTreeComponent>(CDO->BrainComponent);
				if (BTComp)
				{
					BrainObj->SetStringField(TEXT("type"), TEXT("BehaviorTree"));
				}

				Result->SetObjectField(TEXT("brain_component"), BrainObj);
			}

			// Navigation filter
			if (CDO->GetDefaultNavigationFilterClass())
			{
				Result->SetStringField(TEXT("navigation_filter"), CDO->GetDefaultNavigationFilterClass()->GetName());
			}
		}
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  95. list_ai_controllers
// ============================================================

FMonolithActionResult FMonolithAIControllerActions::HandleListAIControllers(const TSharedPtr<FJsonObject>& Params)
{
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetData> Assets;
	AR.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Assets);

	FString PathFilter = Params->GetStringField(TEXT("path_filter"));

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FAssetData& Asset : Assets)
	{
		// Check if this Blueprint's parent is AAIController
		FAssetTagValueRef ParentClassTag = Asset.TagsAndValues.FindTag(FName(TEXT("ParentClass")));
		if (!ParentClassTag.IsSet())
		{
			continue;
		}

		FString ParentClassPath = ParentClassTag.GetValue();
		// Check if the parent class is AAIController or a subclass
		// The tag stores the native parent class path like "/Script/AIModule.AIController"
		if (!ParentClassPath.Contains(TEXT("AIController")))
		{
			continue;
		}

		FString ObjPath = Asset.GetObjectPathString();
		if (!PathFilter.IsEmpty() && !ObjPath.StartsWith(PathFilter))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("asset_path"), Asset.PackageName.ToString());
		Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Item->SetStringField(TEXT("parent_class"), ParentClassPath);

		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("ai_controllers"), Items);
	Result->SetNumberField(TEXT("count"), Items.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  96. set_ai_controller_bt
// ============================================================

FMonolithActionResult FMonolithAIControllerActions::HandleSetAIControllerBT(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBlueprint* BP = MonolithAI::LoadAIControllerFromParams(Params, AssetPath, Error);
	if (!BP)
	{
		return FMonolithActionResult::Error(Error);
	}

	FString BTPath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("bt_path"), BTPath, ErrResult))
	{
		return ErrResult;
	}

	UBehaviorTree* BT = Cast<UBehaviorTree>(FMonolithAssetUtils::LoadAssetByPath(UBehaviorTree::StaticClass(), BTPath));
	if (!BT)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Behavior Tree not found: %s"), *BTPath));
	}

	// Optionally load BB (use BT's BB if not specified)
	FString BBPath = Params->GetStringField(TEXT("bb_path"));
	UBlackboardData* BB = nullptr;
	if (!BBPath.IsEmpty())
	{
		BB = Cast<UBlackboardData>(FMonolithAssetUtils::LoadAssetByPath(UBlackboardData::StaticClass(), BBPath));
		if (!BB)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Blackboard not found: %s"), *BBPath));
		}
	}
	else if (BT->BlackboardAsset)
	{
		BB = BT->BlackboardAsset;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Set AI Controller BT")));

	if (!BP->GeneratedClass)
	{
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	if (!BP->GeneratedClass)
	{
		return FMonolithActionResult::Error(TEXT("Failed to compile AIController Blueprint"));
	}

	AAIController* CDO = Cast<AAIController>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO)
	{
		return FMonolithActionResult::Error(TEXT("Failed to get AIController CDO"));
	}

	// Enable auto-start on possess via reflection (protected member)
	if (FBoolProperty* StartProp = CastField<FBoolProperty>(BP->GeneratedClass->FindPropertyByName(TEXT("bStartAILogicOnPossess"))))
	{
		StartProp->SetPropertyValue_InContainer(CDO, true);
	}

	BP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(AssetPath, TEXT("AI Controller BT updated"));
	Result->SetStringField(TEXT("bt_path"), BT->GetPathName());
	Result->SetBoolField(TEXT("start_logic_on_possess"), true);
	if (BB)
	{
		Result->SetStringField(TEXT("bb_path"), BB->GetPathName());
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  100b. set_pawn_ai_controller_class
// ============================================================

FMonolithActionResult FMonolithAIControllerActions::HandleSetPawnAIControllerClass(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintPath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("blueprint_path"), BlueprintPath, ErrResult))
	{
		return ErrResult;
	}

	FString ControllerClassStr;
	if (!MonolithAI::RequireStringParam(Params, TEXT("controller_class"), ControllerClassStr, ErrResult))
	{
		return ErrResult;
	}

	// Load the Pawn/Character Blueprint
	UBlueprint* PawnBP = Cast<UBlueprint>(FMonolithAssetUtils::LoadAssetByPath(UBlueprint::StaticClass(), BlueprintPath));
	if (!PawnBP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
	}

	if (!PawnBP->GeneratedClass || !PawnBP->GeneratedClass->IsChildOf(APawn::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("'%s' is not a Pawn/Character Blueprint"), *BlueprintPath));
	}

	// Resolve the controller class
	UClass* ControllerClass = nullptr;

	// Try loading as a Blueprint first
	UBlueprint* ControllerBP = Cast<UBlueprint>(FMonolithAssetUtils::LoadAssetByPath(UBlueprint::StaticClass(), ControllerClassStr));
	if (ControllerBP && ControllerBP->GeneratedClass)
	{
		ControllerClass = ControllerBP->GeneratedClass;
	}
	else
	{
		// Try as a native class
		ControllerClass = FindFirstObject<UClass>(*ControllerClassStr, EFindFirstObjectOptions::EnsureIfAmbiguous);
		if (!ControllerClass)
		{
			ControllerClass = LoadObject<UClass>(nullptr, *ControllerClassStr);
		}
	}

	if (!ControllerClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Controller class not found: %s"), *ControllerClassStr));
	}

	if (!ControllerClass->IsChildOf(AController::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("'%s' is not a Controller class"), *ControllerClassStr));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Set Pawn AI Controller Class")));

	APawn* CDO = Cast<APawn>(PawnBP->GeneratedClass->GetDefaultObject());
	if (!CDO)
	{
		return FMonolithActionResult::Error(TEXT("Failed to get Pawn CDO"));
	}

	CDO->AIControllerClass = ControllerClass;
	PawnBP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(BlueprintPath, TEXT("AIControllerClass updated on Pawn CDO"));
	Result->SetStringField(TEXT("controller_class"), ControllerClass->GetPathName());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  97. set_ai_controller_flags
// ============================================================

FMonolithActionResult FMonolithAIControllerActions::HandleSetAIControllerFlags(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBlueprint* BP = MonolithAI::LoadAIControllerFromParams(Params, AssetPath, Error);
	if (!BP)
	{
		return FMonolithActionResult::Error(Error);
	}

	if (!BP->GeneratedClass)
	{
		FKismetEditorUtilities::CompileBlueprint(BP);
	}
	if (!BP->GeneratedClass)
	{
		return FMonolithActionResult::Error(TEXT("Failed to compile AIController Blueprint"));
	}

	AAIController* CDO = Cast<AAIController>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO)
	{
		return FMonolithActionResult::Error(TEXT("Failed to get AIController CDO"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Set AI Controller Flags")));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	int32 ChangedCount = 0;

	// wants_player_state (public)
	if (Params->HasField(TEXT("wants_player_state")))
	{
		bool bVal = Params->GetBoolField(TEXT("wants_player_state"));
		CDO->bWantsPlayerState = bVal;
		Result->SetBoolField(TEXT("wants_player_state"), bVal);
		++ChangedCount;
	}

	// allow_strafe (public)
	if (Params->HasField(TEXT("allow_strafe")))
	{
		bool bVal = Params->GetBoolField(TEXT("allow_strafe"));
		CDO->bAllowStrafe = bVal;
		Result->SetBoolField(TEXT("allow_strafe"), bVal);
		++ChangedCount;
	}

	// start_ai_on_possess (protected — via reflection)
	if (Params->HasField(TEXT("start_ai_on_possess")))
	{
		bool bVal = Params->GetBoolField(TEXT("start_ai_on_possess"));
		if (FBoolProperty* Prop = CastField<FBoolProperty>(BP->GeneratedClass->FindPropertyByName(TEXT("bStartAILogicOnPossess"))))
		{
			Prop->SetPropertyValue_InContainer(CDO, bVal);
			Result->SetBoolField(TEXT("start_ai_on_possess"), bVal);
			++ChangedCount;
		}
	}

	// skip_extra_los_checks (protected — via reflection)
	if (Params->HasField(TEXT("skip_extra_los_checks")))
	{
		bool bVal = Params->GetBoolField(TEXT("skip_extra_los_checks"));
		if (FBoolProperty* Prop = CastField<FBoolProperty>(BP->GeneratedClass->FindPropertyByName(TEXT("bSkipExtraLOSChecks"))))
		{
			Prop->SetPropertyValue_InContainer(CDO, bVal);
			Result->SetBoolField(TEXT("skip_extra_los_checks"), bVal);
			++ChangedCount;
		}
	}

	if (ChangedCount == 0)
	{
		return FMonolithActionResult::Error(TEXT("No valid flags provided. Supported: wants_player_state, allow_strafe, start_ai_on_possess, skip_extra_los_checks"));
	}

	BP->MarkPackageDirty();

	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("%d flag(s) updated"), ChangedCount));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  98. set_ai_team
// ============================================================

FMonolithActionResult FMonolithAIControllerActions::HandleSetAITeam(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBlueprint* BP = MonolithAI::LoadAIControllerFromParams(Params, AssetPath, Error);
	if (!BP)
	{
		return FMonolithActionResult::Error(Error);
	}

	double TeamIdDouble = Params->GetNumberField(TEXT("team_id"));
	int32 TeamId = FMath::RoundToInt32(TeamIdDouble);
	if (TeamId < 0 || TeamId > 255)
	{
		return FMonolithActionResult::Error(TEXT("team_id must be 0-255 (255 = NoTeam)"));
	}

	if (!BP->GeneratedClass)
	{
		FKismetEditorUtilities::CompileBlueprint(BP);
	}
	if (!BP->GeneratedClass)
	{
		return FMonolithActionResult::Error(TEXT("Failed to compile AIController Blueprint"));
	}

	AAIController* CDO = Cast<AAIController>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO)
	{
		return FMonolithActionResult::Error(TEXT("Failed to get AIController CDO"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Set AI Team")));

	// SetGenericTeamId is public on AAIController
	CDO->SetGenericTeamId(FGenericTeamId(static_cast<uint8>(TeamId)));

	BP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(AssetPath, TEXT("Team ID updated"));
	Result->SetNumberField(TEXT("team_id"), TeamId);
	// Verify readback
	Result->SetNumberField(TEXT("verified_team_id"), static_cast<int32>(CDO->GetGenericTeamId().GetId()));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  99. get_ai_team
// ============================================================

FMonolithActionResult FMonolithAIControllerActions::HandleGetAITeam(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBlueprint* BP = MonolithAI::LoadAIControllerFromParams(Params, AssetPath, Error);
	if (!BP)
	{
		return FMonolithActionResult::Error(Error);
	}

	if (!BP->GeneratedClass)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no GeneratedClass — needs compilation"));
	}

	AAIController* CDO = Cast<AAIController>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO)
	{
		return FMonolithActionResult::Error(TEXT("Failed to get AIController CDO"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);

	uint8 TeamId = CDO->GetGenericTeamId().GetId();
	Result->SetNumberField(TEXT("team_id"), static_cast<int32>(TeamId));
	Result->SetBoolField(TEXT("is_no_team"), TeamId == 255);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  103. spawn_ai_actor
// ============================================================

FMonolithActionResult FMonolithAIControllerActions::HandleSpawnAIActor(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassPath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("class_path"), ClassPath, ErrResult))
	{
		return ErrResult;
	}

	// Parse location
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
		return FMonolithActionResult::Error(TEXT("Missing required param 'location' as {x,y,z}"));
	}

	// Parse optional rotation
	FRotator Rotation = FRotator::ZeroRotator;
	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (Params->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj && (*RotObj).IsValid())
	{
		Rotation.Pitch = (*RotObj)->GetNumberField(TEXT("pitch"));
		Rotation.Yaw = (*RotObj)->GetNumberField(TEXT("yaw"));
		Rotation.Roll = (*RotObj)->GetNumberField(TEXT("roll"));
	}

	// Load the Blueprint. Resolver's tier-1 normalization handles the
	// /Game/Foo/Bar -> /Game/Foo/Bar.Bar form, so the previous redundant
	// fallback (which re-loaded the same path with no transformation — a
	// silent no-op typo) has been removed.
	UBlueprint* BP = Cast<UBlueprint>(FMonolithAssetUtils::LoadAssetByPath(UBlueprint::StaticClass(), ClassPath));
	if (!BP || !BP->GeneratedClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found or not compiled: %s"), *ClassPath));
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Spawn AI Actor")));

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	AActor* SpawnedActor = World->SpawnActor(BP->GeneratedClass, &Location, &Rotation, SpawnParams);
	if (!SpawnedActor)
	{
		return FMonolithActionResult::Error(TEXT("Failed to spawn actor"));
	}

	// Set label if provided
	FString Label = Params->GetStringField(TEXT("label"));
	if (!Label.IsEmpty())
	{
		SpawnedActor->SetActorLabel(Label);
	}

	// Set folder path — ALWAYS set, default to "AI"
	FString FolderPath = Params->GetStringField(TEXT("folder_path"));
	if (FolderPath.IsEmpty())
	{
		FolderPath = TEXT("AI");
	}
	SpawnedActor->SetFolderPath(*FolderPath);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), SpawnedActor->GetName());
	Result->SetStringField(TEXT("actor_label"), SpawnedActor->GetActorLabel());
	Result->SetStringField(TEXT("class"), BP->GeneratedClass->GetName());
	Result->SetStringField(TEXT("location"), FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), Location.X, Location.Y, Location.Z));
	Result->SetStringField(TEXT("folder_path"), FolderPath);
	Result->SetStringField(TEXT("message"), TEXT("AI actor spawned"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  104. get_ai_actors
// ============================================================

FMonolithActionResult FMonolithAIControllerActions::HandleGetAIActors(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithAI::GetPIEWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No PIE world found. Start Play-In-Editor first."));
	}

	FString ClassFilter = Params->GetStringField(TEXT("class_filter"));

	TArray<TSharedPtr<FJsonValue>> ActorArr;

	for (TActorIterator<APawn> It(World); It; ++It)
	{
		APawn* Pawn = *It;
		if (!Pawn) continue;

		AAIController* AIC = Cast<AAIController>(Pawn->GetController());
		if (!AIC) continue;

		// Class filter
		if (!ClassFilter.IsEmpty())
		{
			FString ClassName = Pawn->GetClass()->GetName();
			if (!ClassName.Contains(ClassFilter))
			{
				continue;
			}
		}

		TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("name"), Pawn->GetName());
		ActorObj->SetStringField(TEXT("class"), Pawn->GetClass()->GetName());
		ActorObj->SetStringField(TEXT("label"), Pawn->GetActorLabel());
		ActorObj->SetStringField(TEXT("controller_class"), AIC->GetClass()->GetName());

		// Location
		FVector Loc = Pawn->GetActorLocation();
		ActorObj->SetStringField(TEXT("location"),
			FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), Loc.X, Loc.Y, Loc.Z));

		// Team
		ActorObj->SetNumberField(TEXT("team_id"), static_cast<int32>(AIC->GetGenericTeamId().GetId()));

		// Has perception?
		ActorObj->SetBoolField(TEXT("has_perception"), AIC->GetPerceptionComponent() != nullptr);

		// BT running?
		UBehaviorTreeComponent* BTComp = Cast<UBehaviorTreeComponent>(AIC->BrainComponent);
		if (BTComp)
		{
			ActorObj->SetBoolField(TEXT("bt_running"), BTComp->IsRunning());
		}

		ActorArr.Add(MakeShared<FJsonValueObject>(ActorObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("ai_actors"), ActorArr);
	Result->SetNumberField(TEXT("count"), ActorArr.Num());
	return FMonolithActionResult::Success(Result);
}
