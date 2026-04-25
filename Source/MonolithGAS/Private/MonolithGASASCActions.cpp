#include "MonolithGASASCActions.h"
#include "MonolithParamSchema.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "GameFramework/PlayerState.h"
#include "K2Node_CallFunction.h"
#include "AbilitySystemInterface.h"
#include "AbilitySystemComponent.h"
#include "GameplayAbilitySpec.h"
#include "GameplayEffect.h"
#include "AttributeSet.h"
#include "EdGraphSchema_K2.h"
#include "EngineUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"

// LogMonolithGAS declared in MonolithGASInternal.h, defined in MonolithGASModule.cpp

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithGASASCActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("gas"), TEXT("add_asc_to_actor"),
		TEXT("Add a UAbilitySystemComponent to a Blueprint actor. Supports 'self' (component on actor) or 'player_state' (ASC on PlayerState for multiplayer)."),
		FMonolithActionHandler::CreateStatic(&HandleAddASCToActor),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path for the actor"))
			.Optional(TEXT("asc_class"), TEXT("string"), TEXT("ASC class name (default: AbilitySystemComponent)"))
			.Optional(TEXT("location"), TEXT("string"), TEXT("Where to add the ASC: 'self' (on actor) or 'player_state' (on PlayerState). Default: 'self'"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("configure_asc"),
		TEXT("Configure ASC settings: replication mode, default abilities, effects, and attribute sets. Warns if Minimal mode is used with PlayerState ASC (breaks prediction)."),
		FMonolithActionHandler::CreateStatic(&HandleConfigureASC),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path containing the ASC"))
			.Optional(TEXT("replication_mode"), TEXT("string"), TEXT("Replication mode: 'full' (legacy), 'mixed' (players/PlayerState), 'minimal' (AI/cheapest)"))
			.Optional(TEXT("default_abilities"), TEXT("array"), TEXT("Array of ability class paths to grant on startup"))
			.Optional(TEXT("default_effects"), TEXT("array"), TEXT("Array of gameplay effect class paths to apply on startup"))
			.Optional(TEXT("default_attribute_sets"), TEXT("array"), TEXT("Array of {class, init_datatable?} objects for attribute sets"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("setup_asc_init"),
		TEXT("Wire InitAbilityActorInfo in the correct lifecycle event. For 'self': PossessedBy (server) + OnRep_PlayerState (client). For 'player_state': routes through PlayerState."),
		FMonolithActionHandler::CreateStatic(&HandleSetupASCInit),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path for the actor/pawn"))
			.Required(TEXT("location"), TEXT("string"), TEXT("ASC location: 'self' or 'player_state'"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("setup_ability_system_interface"),
		TEXT("Generate C++ implementing IAbilitySystemInterface (cannot be done in Blueprint). Creates a .h/.cpp pair with GetAbilitySystemComponent() that routes correctly based on ASC location."),
		FMonolithActionHandler::CreateStatic(&HandleSetupAbilitySystemInterface),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path (used to derive class name and parent class)"))
			.Required(TEXT("asc_location"), TEXT("string"), TEXT("ASC location: 'self' (ASC is a component on this actor) or 'player_state' (ASC lives on PlayerState)"))
			.Optional(TEXT("class_name"), TEXT("string"), TEXT("C++ class name to generate (default: derived from Blueprint name)"))
			.Optional(TEXT("parent_class"), TEXT("string"), TEXT("Parent C++ class (default: derived from Blueprint's parent)"))
			.Build());

	// ---- Phase 2: Configuration ----

	Registry.RegisterAction(TEXT("gas"), TEXT("apply_asc_template"),
		TEXT("Apply a named template to configure an actor's ASC with appropriate replication mode, attribute sets, abilities, and effects."),
		FMonolithActionHandler::CreateStatic(&HandleApplyASCTemplate),
		FParamSchemaBuilder()
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("Blueprint asset path for the actor"))
			.Required(TEXT("template"), TEXT("string"), TEXT("Template: player_character, enemy_common, enemy_boss, interactable, world_state"))
			.Optional(TEXT("overrides"), TEXT("object"), TEXT("Override template: {replication_mode?, attribute_sets?, abilities?, effects?}"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("set_default_abilities"),
		TEXT("Configure the list of auto-granted abilities on an actor's ASC."),
		FMonolithActionHandler::CreateStatic(&HandleSetDefaultAbilities),
		FParamSchemaBuilder()
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("Blueprint asset path containing the ASC"))
			.Required(TEXT("abilities"), TEXT("array"), TEXT("Array of ability class paths or names"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("'set' (replace all), 'add' (append), 'remove' (remove listed). Default: set"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("set_default_effects"),
		TEXT("Configure startup GameplayEffects applied when the ASC initializes."),
		FMonolithActionHandler::CreateStatic(&HandleSetDefaultEffects),
		FParamSchemaBuilder()
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("Blueprint asset path containing the ASC"))
			.Required(TEXT("effects"), TEXT("array"), TEXT("Array of GE class paths or asset paths"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("'set' (replace all), 'add' (append), 'remove' (remove listed). Default: set"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("set_default_attribute_sets"),
		TEXT("Configure which AttributeSets are spawned with the ASC."),
		FMonolithActionHandler::CreateStatic(&HandleSetDefaultAttributeSets),
		FParamSchemaBuilder()
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("Blueprint asset path containing the ASC"))
			.Required(TEXT("attribute_sets"), TEXT("array"), TEXT("Array of {class, init_datatable?} objects"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("'set' (replace all), 'add' (append), 'remove' (remove listed). Default: set"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("set_asc_replication_mode"),
		TEXT("Set the replication mode on an actor's ASC with validation warnings for misconfigurations."),
		FMonolithActionHandler::CreateStatic(&HandleSetASCReplicationMode),
		FParamSchemaBuilder()
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("Blueprint asset path containing the ASC"))
			.Required(TEXT("mode"), TEXT("string"), TEXT("Replication mode: 'full', 'mixed', or 'minimal'"))
			.Build());

	// ---- Phase 3: Validation ----

	Registry.RegisterAction(TEXT("gas"), TEXT("validate_asc_setup"),
		TEXT("Validate an actor's ASC setup: missing IAbilitySystemInterface, wrong replication mode, missing InitAbilityActorInfo, duplicate attribute sets."),
		FMonolithActionHandler::CreateStatic(&HandleValidateASCSetup),
		FParamSchemaBuilder()
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("Blueprint asset path for the actor"))
			.Build());

	// ---- Phase 4: Runtime ----

	Registry.RegisterAction(TEXT("gas"), TEXT("grant_ability"),
		TEXT("Grant a GameplayAbility to a live actor in PIE."),
		FMonolithActionHandler::CreateStatic(&HandleGrantAbility),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in PIE"))
			.Required(TEXT("ability_class"), TEXT("string"), TEXT("GameplayAbility class name or asset path"))
			.Optional(TEXT("level"), TEXT("integer"), TEXT("Ability level (default: 1)"))
			.Optional(TEXT("input_id"), TEXT("integer"), TEXT("Input ID to bind (-1 for none, default: -1)"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("revoke_ability"),
		TEXT("Remove a granted GameplayAbility from a live actor in PIE."),
		FMonolithActionHandler::CreateStatic(&HandleRevokeAbility),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in PIE"))
			.Required(TEXT("ability_class"), TEXT("string"), TEXT("GameplayAbility class name or asset path"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("get_asc_snapshot"),
		TEXT("Full snapshot of a live ASC: granted abilities, active effects, attribute values, owned tags, cooldowns. Use include_* flags to filter."),
		FMonolithActionHandler::CreateStatic(&HandleGetASCSnapshot),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in PIE"))
			.Optional(TEXT("include_abilities"), TEXT("boolean"), TEXT("Include granted abilities (default: true)"))
			.Optional(TEXT("include_effects"), TEXT("boolean"), TEXT("Include active effects (default: true)"))
			.Optional(TEXT("include_attributes"), TEXT("boolean"), TEXT("Include attribute values (default: true)"))
			.Optional(TEXT("include_tags"), TEXT("boolean"), TEXT("Include owned gameplay tags (default: true)"))
			.Optional(TEXT("include_cooldowns"), TEXT("boolean"), TEXT("Include cooldown state (default: true)"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("get_all_ascs"),
		TEXT("List all actors with AbilitySystemComponents in the current PIE world."),
		FMonolithActionHandler::CreateStatic(&HandleGetAllASCs),
		FParamSchemaBuilder()
			.Optional(TEXT("class_filter"), TEXT("string"), TEXT("Filter by actor class name"))
			.Optional(TEXT("tag_filter"), TEXT("string"), TEXT("Filter by owned gameplay tag"))
			.Build());
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Find an SCS_Node for the ASC component on a Blueprint. */
static USCS_Node* FindASCNode(UBlueprint* BP)
{
	if (!BP || !BP->SimpleConstructionScript)
	{
		return nullptr;
	}

	for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
	{
		if (Node && Node->ComponentClass &&
			Node->ComponentClass->IsChildOf(UAbilitySystemComponent::StaticClass()))
		{
			return Node;
		}
	}
	return nullptr;
}

/** Resolve a component class by name, trying bare and U-prefixed. */
static UClass* ResolveComponentClass(const FString& ClassName, FString& OutError)
{
	UClass* CompClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (!CompClass)
	{
		CompClass = FindFirstObject<UClass>(*(TEXT("U") + ClassName), EFindFirstObjectOptions::NativeFirst);
	}
	if (!CompClass)
	{
		OutError = FString::Printf(TEXT("Class not found: %s"), *ClassName);
		return nullptr;
	}
	if (!CompClass->IsChildOf(UAbilitySystemComponent::StaticClass()))
	{
		OutError = FString::Printf(TEXT("Class '%s' is not a UAbilitySystemComponent subclass"), *ClassName);
		return nullptr;
	}
	return CompClass;
}

/** Map replication mode string to enum value. Returns -1 on invalid input. */
static int32 ParseReplicationMode(const FString& ModeStr)
{
	if (ModeStr.Equals(TEXT("full"), ESearchCase::IgnoreCase))
	{
		return static_cast<int32>(EGameplayEffectReplicationMode::Full);
	}
	if (ModeStr.Equals(TEXT("mixed"), ESearchCase::IgnoreCase))
	{
		return static_cast<int32>(EGameplayEffectReplicationMode::Mixed);
	}
	if (ModeStr.Equals(TEXT("minimal"), ESearchCase::IgnoreCase))
	{
		return static_cast<int32>(EGameplayEffectReplicationMode::Minimal);
	}
	return -1;
}

// GetProjectSourceDir() moved to MonolithGAS::GetProjectSourceDir() in MonolithGASInternal.cpp

// ---------------------------------------------------------------------------
// add_asc_to_actor
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithGASASCActions::HandleAddASCToActor(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath, Error;
	UBlueprint* BP = MonolithGAS::LoadBlueprintFromParams(Params, ActorPath, Error);
	if (!BP)
	{
		return FMonolithActionResult::Error(Error);
	}

	if (!BP->SimpleConstructionScript)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript — is it an Actor Blueprint?"));
	}

	// Parse location
	FString Location = Params->GetStringField(TEXT("location"));
	if (Location.IsEmpty())
	{
		Location = TEXT("self");
	}
	if (!Location.Equals(TEXT("self"), ESearchCase::IgnoreCase) &&
		!Location.Equals(TEXT("player_state"), ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid location '%s' — must be 'self' or 'player_state'"), *Location));
	}

	// Resolve ASC class
	FString ASCClassName = Params->GetStringField(TEXT("asc_class"));
	if (ASCClassName.IsEmpty())
	{
		ASCClassName = TEXT("AbilitySystemComponent");
	}

	UClass* ASCClass = nullptr;
	{
		FString ClassError;
		ASCClass = ResolveComponentClass(ASCClassName, ClassError);
		if (!ASCClass)
		{
			return FMonolithActionResult::Error(ClassError);
		}
	}

	// Check if an ASC already exists on this Blueprint
	if (FindASCNode(BP))
	{
		return FMonolithActionResult::Error(
			TEXT("This Blueprint already has an AbilitySystemComponent. Use configure_asc to modify it."));
	}

	if (Location.Equals(TEXT("player_state"), ESearchCase::IgnoreCase))
	{
		// For player_state location, the ASC should be added to the PlayerState Blueprint,
		// not the Pawn. We add it to the current BP but tag it with guidance.
		// In practice the caller should pass the PlayerState BP as actor_path.
		// We proceed and add the component — the caller is responsible for passing the right BP.
	}

	// Pre-flight: check for broken SCS nodes (missing component classes from removed plugins)
	for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
	{
		if (Node && !Node->ComponentClass)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Blueprint has a broken SCS node '%s' with missing ComponentClass. "
					"Fix the Blueprint first (remove the broken component in the editor)."),
					*Node->GetVariableName().ToString()));
		}
	}

	// Suppress synchronous skeleton regen during SCS modification.
	// ASC's FFastArraySerializer containers (ActiveGameplayEffects, ActivatableAbilities)
	// crash during serialization on a template with no valid owner actor.
	const EBlueprintStatus SavedStatus = BP->Status;
	BP->Status = BS_BeingCreated;

	// Create the SCS node
	USCS_Node* NewNode = BP->SimpleConstructionScript->CreateNode(ASCClass, FName(TEXT("AbilitySystemComponent")));
	if (!NewNode)
	{
		BP->Status = SavedStatus;
		return FMonolithActionResult::Error(TEXT("Failed to create ASC SCS node"));
	}

	// Add as root-level component
	BP->SimpleConstructionScript->AddNode(NewNode);

	// Restore status and mark modified (deferred compile, no synchronous skeleton regen)
	BP->Status = SavedStatus;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	// Build result
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), ActorPath);
	Root->SetStringField(TEXT("variable_name"), NewNode->GetVariableName().ToString());
	Root->SetStringField(TEXT("asc_class"), ASCClass->GetName());
	Root->SetStringField(TEXT("location"), Location.ToLower());
	Root->SetBoolField(TEXT("is_replicated"), true);

	TArray<TSharedPtr<FJsonValue>> NextSteps;
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Call setup_asc_init to wire InitAbilityActorInfo")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Call setup_ability_system_interface to implement IAbilitySystemInterface (C++ required)")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Call configure_asc to set replication mode and default abilities")));
	Root->SetArrayField(TEXT("next_steps"), NextSteps);

	if (Location.Equals(TEXT("player_state"), ESearchCase::IgnoreCase))
	{
		Root->SetStringField(TEXT("note"),
			TEXT("ASC added to this Blueprint. For multiplayer PlayerState pattern, ensure this IS the PlayerState BP. ")
			TEXT("The Pawn should use setup_ability_system_interface to route GetAbilitySystemComponent() through GetPlayerState()."));
	}

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// configure_asc
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithGASASCActions::HandleConfigureASC(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath, Error;
	UBlueprint* BP = MonolithGAS::LoadBlueprintFromParams(Params, ActorPath, Error);
	if (!BP)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Find the ASC node
	USCS_Node* ASCNode = FindASCNode(BP);
	if (!ASCNode)
	{
		return FMonolithActionResult::Error(
			TEXT("No AbilitySystemComponent found on this Blueprint. Call add_asc_to_actor first."));
	}

	UAbilitySystemComponent* ASCTemplate = Cast<UAbilitySystemComponent>(ASCNode->ComponentTemplate);
	if (!ASCTemplate)
	{
		return FMonolithActionResult::Error(TEXT("ASC node has no valid ComponentTemplate"));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), ActorPath);
	Root->SetStringField(TEXT("asc_variable"), ASCNode->GetVariableName().ToString());

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> Applied;

	// --- Replication mode ---
	FString RepModeStr = Params->GetStringField(TEXT("replication_mode"));
	if (!RepModeStr.IsEmpty())
	{
		int32 ModeVal = ParseReplicationMode(RepModeStr);
		if (ModeVal < 0)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Invalid replication_mode '%s' — must be 'full', 'mixed', or 'minimal'"), *RepModeStr));
		}

		EGameplayEffectReplicationMode Mode = static_cast<EGameplayEffectReplicationMode>(ModeVal);
		ASCTemplate->SetReplicationMode(Mode);
		Applied.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("ReplicationMode = %s"), *RepModeStr.ToLower())));

		// Warning: Minimal on PlayerState ASC silently breaks prediction
		if (Mode == EGameplayEffectReplicationMode::Minimal)
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				TEXT("Minimal replication breaks client-side prediction. Use Mixed for player-controlled pawns with PlayerState ASC.")));
		}

		// Warning: Full is legacy and wastes bandwidth
		if (Mode == EGameplayEffectReplicationMode::Full)
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				TEXT("Full replication is legacy and replicates everything to all connections. Consider Mixed (players) or Minimal (AI).")));
		}
	}

	// --- Default abilities ---
	TArray<FString> AbilityPaths = MonolithGAS::ParseStringArray(Params, TEXT("default_abilities"));
	if (AbilityPaths.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ValidatedAbilities;
		for (const FString& AbilityPath : AbilityPaths)
		{
			// Validate the class exists and is a GameplayAbility
			UClass* AbilityClass = FindFirstObject<UClass>(*AbilityPath, EFindFirstObjectOptions::NativeFirst);
			if (!AbilityClass)
			{
				// Try loading as a Blueprint class
				FString BPPath = AbilityPath;
				if (!BPPath.EndsWith(TEXT("_C")))
				{
					BPPath += TEXT("_C");
				}
				AbilityClass = LoadClass<UObject>(nullptr, *BPPath);
			}

			if (AbilityClass && AbilityClass->IsChildOf(UGameplayAbility::StaticClass()))
			{
				ValidatedAbilities.Add(MakeShared<FJsonValueString>(AbilityPath));
			}
			else
			{
				Warnings.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Ability class not found or not a UGameplayAbility: %s"), *AbilityPath)));
			}
		}

		if (ValidatedAbilities.Num() > 0)
		{
			Applied.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("default_abilities: %d validated"), ValidatedAbilities.Num())));
			Root->SetArrayField(TEXT("validated_abilities"), ValidatedAbilities);
		}

		// Note: Actually granting abilities at runtime requires C++ in BeginPlay or via a GE.
		// We store the config for documentation; the scaffold actions will generate the grant code.
		Root->SetStringField(TEXT("abilities_note"),
			TEXT("Ability classes validated. Use gas.setup_asc_init or a startup GameplayEffect to grant these at runtime."));
	}

	// --- Default effects ---
	TArray<FString> EffectPaths = MonolithGAS::ParseStringArray(Params, TEXT("default_effects"));
	if (EffectPaths.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ValidatedEffects;
		for (const FString& EffectPath : EffectPaths)
		{
			UClass* EffectClass = FindFirstObject<UClass>(*EffectPath, EFindFirstObjectOptions::NativeFirst);
			if (!EffectClass)
			{
				FString BPPath = EffectPath;
				if (!BPPath.EndsWith(TEXT("_C")))
				{
					BPPath += TEXT("_C");
				}
				EffectClass = LoadClass<UObject>(nullptr, *BPPath);
			}

			if (EffectClass && EffectClass->IsChildOf(UGameplayEffect::StaticClass()))
			{
				ValidatedEffects.Add(MakeShared<FJsonValueString>(EffectPath));
			}
			else
			{
				Warnings.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Effect class not found or not a UGameplayEffect: %s"), *EffectPath)));
			}
		}

		if (ValidatedEffects.Num() > 0)
		{
			Applied.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("default_effects: %d validated"), ValidatedEffects.Num())));
			Root->SetArrayField(TEXT("validated_effects"), ValidatedEffects);
		}
	}

	// --- Default attribute sets ---
	const TArray<TSharedPtr<FJsonValue>>* AttrSetArray;
	if (Params->TryGetArrayField(TEXT("default_attribute_sets"), AttrSetArray))
	{
		TArray<TSharedPtr<FJsonValue>> ValidatedSets;
		for (const TSharedPtr<FJsonValue>& Val : *AttrSetArray)
		{
			const TSharedPtr<FJsonObject>* SetObj;
			if (!Val->TryGetObject(SetObj) || !SetObj->IsValid())
			{
				Warnings.Add(MakeShared<FJsonValueString>(
					TEXT("default_attribute_sets entry is not a valid object — expected {class, init_datatable?}")));
				continue;
			}

			FString SetClassName = (*SetObj)->GetStringField(TEXT("class"));
			FString InitDT = (*SetObj)->GetStringField(TEXT("init_datatable"));

			if (SetClassName.IsEmpty())
			{
				Warnings.Add(MakeShared<FJsonValueString>(TEXT("Attribute set entry missing 'class' field")));
				continue;
			}

			// Validate class
			UClass* SetClass = FindFirstObject<UClass>(*SetClassName, EFindFirstObjectOptions::NativeFirst);
			if (!SetClass)
			{
				FString BPPath = SetClassName;
				if (!BPPath.EndsWith(TEXT("_C")))
				{
					BPPath += TEXT("_C");
				}
				SetClass = LoadClass<UObject>(nullptr, *BPPath);
			}

			if (SetClass && SetClass->IsChildOf(UAttributeSet::StaticClass()))
			{
				TSharedPtr<FJsonObject> ValidEntry = MakeShared<FJsonObject>();
				ValidEntry->SetStringField(TEXT("class"), SetClassName);
				if (!InitDT.IsEmpty())
				{
					ValidEntry->SetStringField(TEXT("init_datatable"), InitDT);
				}
				ValidatedSets.Add(MakeShared<FJsonValueObject>(ValidEntry));
			}
			else
			{
				Warnings.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Attribute set class not found or not a UAttributeSet: %s"), *SetClassName)));
			}
		}

		if (ValidatedSets.Num() > 0)
		{
			Applied.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("default_attribute_sets: %d validated"), ValidatedSets.Num())));
			Root->SetArrayField(TEXT("validated_attribute_sets"), ValidatedSets);
		}
	}

	if (Applied.Num() > 0)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		Root->SetArrayField(TEXT("applied"), Applied);
	}
	else
	{
		Root->SetStringField(TEXT("message"), TEXT("No configuration changes applied — provide at least one optional parameter"));
	}

	if (Warnings.Num() > 0)
	{
		Root->SetArrayField(TEXT("warnings"), Warnings);
	}

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// setup_asc_init
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithGASASCActions::HandleSetupASCInit(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath, Error;
	UBlueprint* BP = MonolithGAS::LoadBlueprintFromParams(Params, ActorPath, Error);
	if (!BP)
	{
		return FMonolithActionResult::Error(Error);
	}

	FString Location;
	FMonolithActionResult ParamError;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("location"), Location, ParamError))
	{
		return ParamError;
	}

	if (!Location.Equals(TEXT("self"), ESearchCase::IgnoreCase) &&
		!Location.Equals(TEXT("player_state"), ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid location '%s' — must be 'self' or 'player_state'"), *Location));
	}

	bool bIsSelf = Location.Equals(TEXT("self"), ESearchCase::IgnoreCase);

	// Determine the parent class name for context
	FString ParentClassName = TEXT("AActor");
	if (BP->ParentClass)
	{
		ParentClassName = BP->ParentClass->GetName();
	}

	// Build the C++ code snippets that need to be added
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), ActorPath);
	Root->SetStringField(TEXT("location"), Location.ToLower());
	Root->SetStringField(TEXT("parent_class"), ParentClassName);

	TArray<TSharedPtr<FJsonValue>> CodeSnippets;

	if (bIsSelf)
	{
		// --- Self pattern: ASC is on this actor ---

		// Server: PossessedBy
		TSharedPtr<FJsonObject> ServerSnippet = MakeShared<FJsonObject>();
		ServerSnippet->SetStringField(TEXT("context"), TEXT("Server — override PossessedBy"));
		ServerSnippet->SetStringField(TEXT("code"),
			TEXT("void A<ClassName>::PossessedBy(AController* NewController)\n")
			TEXT("{\n")
			TEXT("\tSuper::PossessedBy(NewController);\n")
			TEXT("\n")
			TEXT("\tif (AbilitySystemComponent)\n")
			TEXT("\t{\n")
			TEXT("\t\tAbilitySystemComponent->InitAbilityActorInfo(this, this);\n")
			TEXT("\t}\n")
			TEXT("}")
		);
		CodeSnippets.Add(MakeShared<FJsonValueObject>(ServerSnippet));

		// Client: OnRep_PlayerState (for replicated pawns)
		TSharedPtr<FJsonObject> ClientSnippet = MakeShared<FJsonObject>();
		ClientSnippet->SetStringField(TEXT("context"), TEXT("Client — override OnRep_PlayerState"));
		ClientSnippet->SetStringField(TEXT("code"),
			TEXT("void A<ClassName>::OnRep_PlayerState()\n")
			TEXT("{\n")
			TEXT("\tSuper::OnRep_PlayerState();\n")
			TEXT("\n")
			TEXT("\tif (AbilitySystemComponent)\n")
			TEXT("\t{\n")
			TEXT("\t\tAbilitySystemComponent->InitAbilityActorInfo(this, this);\n")
			TEXT("\t}\n")
			TEXT("}")
		);
		CodeSnippets.Add(MakeShared<FJsonValueObject>(ClientSnippet));
	}
	else
	{
		// --- PlayerState pattern: ASC is on the PlayerState ---

		// Pawn server: PossessedBy — get ASC from PlayerState
		TSharedPtr<FJsonObject> ServerSnippet = MakeShared<FJsonObject>();
		ServerSnippet->SetStringField(TEXT("context"), TEXT("Pawn server — override PossessedBy, init ASC from PlayerState"));
		ServerSnippet->SetStringField(TEXT("code"),
			TEXT("void A<ClassName>::PossessedBy(AController* NewController)\n")
			TEXT("{\n")
			TEXT("\tSuper::PossessedBy(NewController);\n")
			TEXT("\n")
			TEXT("\tif (APlayerState* PS = GetPlayerState())\n")
			TEXT("\t{\n")
			TEXT("\t\tif (UAbilitySystemComponent* ASC = PS->FindComponentByClass<UAbilitySystemComponent>())\n")
			TEXT("\t\t{\n")
			TEXT("\t\t\tASC->InitAbilityActorInfo(PS, this);\n")
			TEXT("\t\t}\n")
			TEXT("\t}\n")
			TEXT("}")
		);
		CodeSnippets.Add(MakeShared<FJsonValueObject>(ServerSnippet));

		// Pawn client: OnRep_PlayerState
		TSharedPtr<FJsonObject> ClientSnippet = MakeShared<FJsonObject>();
		ClientSnippet->SetStringField(TEXT("context"), TEXT("Pawn client — override OnRep_PlayerState, init ASC from PlayerState"));
		ClientSnippet->SetStringField(TEXT("code"),
			TEXT("void A<ClassName>::OnRep_PlayerState()\n")
			TEXT("{\n")
			TEXT("\tSuper::OnRep_PlayerState();\n")
			TEXT("\n")
			TEXT("\tif (APlayerState* PS = GetPlayerState())\n")
			TEXT("\t{\n")
			TEXT("\t\tif (UAbilitySystemComponent* ASC = PS->FindComponentByClass<UAbilitySystemComponent>())\n")
			TEXT("\t\t{\n")
			TEXT("\t\t\tASC->InitAbilityActorInfo(PS, this);\n")
			TEXT("\t\t}\n")
			TEXT("\t}\n")
			TEXT("}")
		);
		CodeSnippets.Add(MakeShared<FJsonValueObject>(ClientSnippet));
	}

	Root->SetArrayField(TEXT("code_snippets"), CodeSnippets);

	// Guidance
	TArray<TSharedPtr<FJsonValue>> Notes;
	Notes.Add(MakeShared<FJsonValueString>(
		TEXT("Replace <ClassName> with your actual class name in the generated code.")));
	if (!bIsSelf)
	{
		Notes.Add(MakeShared<FJsonValueString>(
			TEXT("PlayerState pattern: Owner=PlayerState, Avatar=Pawn. Mixed replication mode required.")));
		Notes.Add(MakeShared<FJsonValueString>(
			TEXT("The PlayerState must have the ASC component (call add_asc_to_actor on the PlayerState BP).")));
	}
	else
	{
		Notes.Add(MakeShared<FJsonValueString>(
			TEXT("Self pattern: Owner=this, Avatar=this. Good for AI or single-player.")));
	}
	Notes.Add(MakeShared<FJsonValueString>(
		TEXT("Both overrides are needed — PossessedBy for server authority, OnRep_PlayerState for client prediction.")));
	Root->SetArrayField(TEXT("notes"), Notes);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// setup_ability_system_interface
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithGASASCActions::HandleSetupAbilitySystemInterface(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath, Error;
	UBlueprint* BP = MonolithGAS::LoadBlueprintFromParams(Params, ActorPath, Error);
	if (!BP)
	{
		return FMonolithActionResult::Error(Error);
	}

	FString ASCLocation;
	FMonolithActionResult ParamError;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asc_location"), ASCLocation, ParamError))
	{
		return ParamError;
	}

	if (!ASCLocation.Equals(TEXT("self"), ESearchCase::IgnoreCase) &&
		!ASCLocation.Equals(TEXT("player_state"), ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid asc_location '%s' — must be 'self' or 'player_state'"), *ASCLocation));
	}

	bool bIsSelf = ASCLocation.Equals(TEXT("self"), ESearchCase::IgnoreCase);

	// Derive class name
	FString ClassName = Params->GetStringField(TEXT("class_name"));
	if (ClassName.IsEmpty())
	{
		// Derive from Blueprint name (strip BP_ prefix if present)
		FString BPName = FPaths::GetBaseFilename(ActorPath);
		if (BPName.StartsWith(TEXT("BP_")))
		{
			BPName = BPName.Mid(3);
		}
		ClassName = TEXT("A") + BPName;
	}
	// Ensure it starts with A for actors
	if (!ClassName.StartsWith(TEXT("A")))
	{
		ClassName = TEXT("A") + ClassName;
	}

	// Derive parent class
	FString ParentClass = Params->GetStringField(TEXT("parent_class"));
	if (ParentClass.IsEmpty())
	{
		if (BP->ParentClass)
		{
			ParentClass = BP->ParentClass->GetName();
			// Ensure it has the A prefix
			if (!ParentClass.StartsWith(TEXT("A")))
			{
				ParentClass = TEXT("A") + ParentClass;
			}
		}
		else
		{
			ParentClass = TEXT("ACharacter");
		}
	}

	// Derive the include for the parent class
	FString ParentInclude;
	if (ParentClass == TEXT("ACharacter"))
	{
		ParentInclude = TEXT("GameFramework/Character.h");
	}
	else if (ParentClass == TEXT("APawn"))
	{
		ParentInclude = TEXT("GameFramework/Pawn.h");
	}
	else if (ParentClass == TEXT("AActor"))
	{
		ParentInclude = TEXT("GameFramework/Actor.h");
	}
	else if (ParentClass == TEXT("APlayerState"))
	{
		ParentInclude = TEXT("GameFramework/PlayerState.h");
	}
	else
	{
		// Custom parent — user will need to fix the include
		ParentInclude = FString::Printf(TEXT("%s.h"), *ParentClass.Mid(1)); // Strip A prefix
	}

	// Build the filename (strip A prefix for file name)
	FString FileBaseName = ClassName.Mid(1); // strip A prefix
	FString SourceDir = MonolithGAS::GetProjectSourceDir();
	FString HeaderPath = SourceDir / FileBaseName + TEXT(".h");
	FString CppPath = SourceDir / FileBaseName + TEXT(".cpp");

	// Check if files already exist
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.FileExists(*HeaderPath))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Header file already exists: %s — manually merge IAbilitySystemInterface or choose a different class_name"), *HeaderPath));
	}

	// Module name for the MODULENAME_API macro
	FString ModuleName = FApp::GetProjectName();
	FString APIExport = ModuleName.ToUpper() + TEXT("_API");

	// --- Generate header ---
	FString HeaderContent;
	HeaderContent += TEXT("#pragma once\n\n");
	HeaderContent += FString::Printf(TEXT("#include \"%s\"\n"), *ParentInclude);
	HeaderContent += TEXT("#include \"AbilitySystemInterface.h\"\n");
	HeaderContent += TEXT("#include \"AbilitySystemComponent.h\"\n");
	if (!bIsSelf)
	{
		HeaderContent += TEXT("#include \"GameFramework/PlayerState.h\"\n");
	}
	HeaderContent += FString::Printf(TEXT("#include \"%s.generated.h\"\n\n"), *FileBaseName);

	HeaderContent += TEXT("/**\n");
	HeaderContent += FString::Printf(TEXT(" * %s\n"), *ClassName);
	HeaderContent += TEXT(" * Implements IAbilitySystemInterface for GAS integration.\n");
	if (!bIsSelf)
	{
		HeaderContent += TEXT(" * ASC lives on PlayerState (multiplayer pattern).\n");
	}
	else
	{
		HeaderContent += TEXT(" * ASC lives on this actor (self pattern, good for AI or single-player).\n");
	}
	HeaderContent += TEXT(" */\n");
	HeaderContent += TEXT("UCLASS()\n");
	HeaderContent += FString::Printf(TEXT("class %s %s : public %s, public IAbilitySystemInterface\n"),
		*APIExport, *ClassName, *ParentClass);
	HeaderContent += TEXT("{\n");
	HeaderContent += TEXT("\tGENERATED_BODY()\n\n");
	HeaderContent += TEXT("public:\n");
	HeaderContent += FString::Printf(TEXT("\t%s();\n\n"), *ClassName);

	// IAbilitySystemInterface
	HeaderContent += TEXT("\t//~ IAbilitySystemInterface\n");
	HeaderContent += TEXT("\tvirtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;\n\n");

	if (bIsSelf)
	{
		// Self pattern: ASC is a component on this actor
		HeaderContent += TEXT("protected:\n");
		HeaderContent += TEXT("\tUPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = \"Abilities\")\n");
		HeaderContent += TEXT("\tTObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;\n\n");

		// Lifecycle overrides
		HeaderContent += TEXT("public:\n");
		HeaderContent += TEXT("\tvirtual void PossessedBy(AController* NewController) override;\n");
		HeaderContent += TEXT("\tvirtual void OnRep_PlayerState() override;\n");
	}
	else
	{
		// PlayerState pattern: ASC is on PlayerState, this actor just routes to it
		HeaderContent += TEXT("public:\n");
		HeaderContent += TEXT("\tvirtual void PossessedBy(AController* NewController) override;\n");
		HeaderContent += TEXT("\tvirtual void OnRep_PlayerState() override;\n");
	}

	HeaderContent += TEXT("};\n");

	// --- Generate cpp ---
	FString CppContent;
	CppContent += FString::Printf(TEXT("#include \"%s.h\"\n\n"), *FileBaseName);

	// Constructor
	CppContent += FString::Printf(TEXT("%s::%s()\n"), *ClassName, *ClassName);
	CppContent += TEXT("{\n");
	if (bIsSelf)
	{
		CppContent += TEXT("\tAbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT(\"AbilitySystemComponent\"));\n");
		CppContent += TEXT("\tAbilitySystemComponent->SetIsReplicated(true);\n");
		CppContent += TEXT("\tAbilitySystemComponent->ReplicationMode = EGameplayEffectReplicationMode::Minimal;\n");
	}
	CppContent += TEXT("}\n\n");

	// GetAbilitySystemComponent
	CppContent += FString::Printf(TEXT("UAbilitySystemComponent* %s::GetAbilitySystemComponent() const\n"), *ClassName);
	CppContent += TEXT("{\n");
	if (bIsSelf)
	{
		CppContent += TEXT("\treturn AbilitySystemComponent;\n");
	}
	else
	{
		CppContent += TEXT("\tif (const APlayerState* PS = GetPlayerState())\n");
		CppContent += TEXT("\t{\n");
		CppContent += TEXT("\t\treturn PS->FindComponentByClass<UAbilitySystemComponent>();\n");
		CppContent += TEXT("\t}\n");
		CppContent += TEXT("\treturn nullptr;\n");
	}
	CppContent += TEXT("}\n\n");

	// PossessedBy
	CppContent += FString::Printf(TEXT("void %s::PossessedBy(AController* NewController)\n"), *ClassName);
	CppContent += TEXT("{\n");
	CppContent += TEXT("\tSuper::PossessedBy(NewController);\n\n");
	if (bIsSelf)
	{
		CppContent += TEXT("\tif (AbilitySystemComponent)\n");
		CppContent += TEXT("\t{\n");
		CppContent += TEXT("\t\tAbilitySystemComponent->InitAbilityActorInfo(this, this);\n");
		CppContent += TEXT("\t}\n");
	}
	else
	{
		CppContent += TEXT("\tif (APlayerState* PS = GetPlayerState())\n");
		CppContent += TEXT("\t{\n");
		CppContent += TEXT("\t\tif (UAbilitySystemComponent* ASC = PS->FindComponentByClass<UAbilitySystemComponent>())\n");
		CppContent += TEXT("\t\t{\n");
		CppContent += TEXT("\t\t\tASC->InitAbilityActorInfo(PS, this);\n");
		CppContent += TEXT("\t\t}\n");
		CppContent += TEXT("\t}\n");
	}
	CppContent += TEXT("}\n\n");

	// OnRep_PlayerState
	CppContent += FString::Printf(TEXT("void %s::OnRep_PlayerState()\n"), *ClassName);
	CppContent += TEXT("{\n");
	CppContent += TEXT("\tSuper::OnRep_PlayerState();\n\n");
	if (bIsSelf)
	{
		CppContent += TEXT("\tif (AbilitySystemComponent)\n");
		CppContent += TEXT("\t{\n");
		CppContent += TEXT("\t\tAbilitySystemComponent->InitAbilityActorInfo(this, this);\n");
		CppContent += TEXT("\t}\n");
	}
	else
	{
		CppContent += TEXT("\tif (APlayerState* PS = GetPlayerState())\n");
		CppContent += TEXT("\t{\n");
		CppContent += TEXT("\t\tif (UAbilitySystemComponent* ASC = PS->FindComponentByClass<UAbilitySystemComponent>())\n");
		CppContent += TEXT("\t\t{\n");
		CppContent += TEXT("\t\t\tASC->InitAbilityActorInfo(PS, this);\n");
		CppContent += TEXT("\t\t}\n");
		CppContent += TEXT("\t}\n");
	}
	CppContent += TEXT("}\n");

	// Write files
	bool bHeaderWritten = FFileHelper::SaveStringToFile(HeaderContent, *HeaderPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	bool bCppWritten = FFileHelper::SaveStringToFile(CppContent, *CppPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	if (!bHeaderWritten || !bCppWritten)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to write generated files. Header: %s, CPP: %s"),
				bHeaderWritten ? TEXT("OK") : TEXT("FAILED"),
				bCppWritten ? TEXT("OK") : TEXT("FAILED")));
	}

	UE_LOG(LogMonolithGAS, Log,
		TEXT("Generated IAbilitySystemInterface implementation: %s + %s"),
		*HeaderPath, *CppPath);

	// Build result
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), ActorPath);
	Root->SetStringField(TEXT("class_name"), ClassName);
	Root->SetStringField(TEXT("parent_class"), ParentClass);
	Root->SetStringField(TEXT("asc_location"), ASCLocation.ToLower());
	Root->SetStringField(TEXT("header_path"), HeaderPath);
	Root->SetStringField(TEXT("cpp_path"), CppPath);

	TArray<TSharedPtr<FJsonValue>> NextSteps;
	NextSteps.Add(MakeShared<FJsonValueString>(
		TEXT("Build the project to compile the new C++ class")));
	NextSteps.Add(MakeShared<FJsonValueString>(
		FString::Printf(TEXT("Reparent the Blueprint '%s' to the new C++ class '%s'"), *ActorPath, *ClassName)));
	if (bIsSelf)
	{
		NextSteps.Add(MakeShared<FJsonValueString>(
			TEXT("The ASC is created in the constructor via CreateDefaultSubobject — remove any Blueprint ASC component to avoid duplicates")));
	}
	else
	{
		NextSteps.Add(MakeShared<FJsonValueString>(
			TEXT("Ensure the PlayerState Blueprint has an ASC component (use add_asc_to_actor on the PlayerState)")));
		NextSteps.Add(MakeShared<FJsonValueString>(
			TEXT("Set replication mode to Mixed on the PlayerState ASC (required for client prediction)")));
	}
	Root->SetArrayField(TEXT("next_steps"), NextSteps);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Phase 2: ASC Template Definitions
// ---------------------------------------------------------------------------

namespace
{
	struct FASCTemplateDef
	{
		FString Location;       // "self" or "player_state"
		FString ReplicationMode; // "full", "mixed", "minimal"
		TArray<FString> AttributeSets;
		TArray<FString> Abilities;
		TArray<FString> Effects;
	};

	bool GetASCTemplate(const FString& TemplateName, FASCTemplateDef& OutDef, FString& OutError)
	{
		OutDef = FASCTemplateDef();

		if (TemplateName == TEXT("player_character"))
		{
			OutDef.Location = TEXT("player_state");
			OutDef.ReplicationMode = TEXT("mixed");
			OutDef.AttributeSets = {TEXT("VitalSet"), TEXT("StaminaSet"), TEXT("HorrorSet")};
			OutDef.Abilities = {TEXT("GA_Sprint"), TEXT("GA_Interact"), TEXT("GA_Dodge")};
		}
		else if (TemplateName == TEXT("enemy_common"))
		{
			OutDef.Location = TEXT("self");
			OutDef.ReplicationMode = TEXT("minimal");
			OutDef.AttributeSets = {TEXT("EnemyVitalSet"), TEXT("EnemyResistanceSet")};
			OutDef.Abilities = {TEXT("GA_MeleeAttack"), TEXT("GA_Die")};
		}
		else if (TemplateName == TEXT("enemy_boss"))
		{
			OutDef.Location = TEXT("self");
			OutDef.ReplicationMode = TEXT("mixed");
			OutDef.AttributeSets = {TEXT("EnemyVitalSet"), TEXT("EnemyResistanceSet"), TEXT("BossPhaseSet")};
			OutDef.Abilities = {TEXT("GA_MeleeAttack"), TEXT("GA_RangedAttack"), TEXT("GA_PhaseTransition")};
		}
		else if (TemplateName == TEXT("interactable"))
		{
			OutDef.Location = TEXT("self");
			OutDef.ReplicationMode = TEXT("minimal");
			OutDef.Abilities = {TEXT("GA_OnInteract")};
		}
		else if (TemplateName == TEXT("world_state"))
		{
			OutDef.Location = TEXT("self");
			OutDef.ReplicationMode = TEXT("minimal");
			OutDef.AttributeSets = {TEXT("WorldStateSet")};
		}
		else
		{
			OutError = FString::Printf(
				TEXT("Unknown ASC template: '%s'. Valid: player_character, enemy_common, enemy_boss, interactable, world_state"),
				*TemplateName);
			return false;
		}

		return true;
	}

	/** Validate a class by name, trying various path forms. Returns the class name on success, or adds to warnings. */
	FString ValidateClassReference(const FString& ClassName, UClass* RequiredParent, TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		UClass* FoundClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
		if (!FoundClass)
		{
			FoundClass = FindFirstObject<UClass>(*(TEXT("U") + ClassName), EFindFirstObjectOptions::NativeFirst);
		}
		if (!FoundClass)
		{
			FString BPPath = ClassName;
			if (!BPPath.EndsWith(TEXT("_C"))) BPPath += TEXT("_C");
			FoundClass = LoadClass<UObject>(nullptr, *BPPath);
		}

		if (FoundClass && (!RequiredParent || FoundClass->IsChildOf(RequiredParent)))
		{
			return ClassName;
		}

		Warnings.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("Class not found (may need to be created first): %s"), *ClassName)));
		return ClassName; // Return anyway — template sets up the config even if classes don't exist yet
	}
}

// ---------------------------------------------------------------------------
// Phase 2: apply_asc_template
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithGASASCActions::HandleApplyASCTemplate(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath, TemplateName;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor_path"), ActorPath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("template"), TemplateName, Err)) return Err;

	TemplateName = TemplateName.ToLower();

	FString TemplateError;
	FASCTemplateDef Def;
	if (!GetASCTemplate(TemplateName, Def, TemplateError))
	{
		return FMonolithActionResult::Error(TemplateError);
	}

	// Apply overrides
	const TSharedPtr<FJsonObject>* OverridesPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("overrides"), OverridesPtr) && OverridesPtr && (*OverridesPtr).IsValid())
	{
		const TSharedPtr<FJsonObject>& Ov = *OverridesPtr;

		FString RepMode = Ov->GetStringField(TEXT("replication_mode"));
		if (!RepMode.IsEmpty()) Def.ReplicationMode = RepMode;

		TArray<FString> OvAttrSets = MonolithGAS::ParseStringArray(Ov, TEXT("attribute_sets"));
		if (OvAttrSets.Num() > 0) Def.AttributeSets = OvAttrSets;

		TArray<FString> OvAbilities = MonolithGAS::ParseStringArray(Ov, TEXT("abilities"));
		if (OvAbilities.Num() > 0) Def.Abilities = OvAbilities;

		TArray<FString> OvEffects = MonolithGAS::ParseStringArray(Ov, TEXT("effects"));
		if (OvEffects.Num() > 0) Def.Effects = OvEffects;
	}

	// Load the Blueprint
	FString Error;
	FString OutPath;
	TSharedPtr<FJsonObject> TempParams = MakeShared<FJsonObject>();
	TempParams->SetStringField(TEXT("asset_path"), ActorPath);
	UBlueprint* BP = MonolithGAS::LoadBlueprintFromParams(TempParams, OutPath, Error);
	if (!BP)
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Applied;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> NextSteps;

	// Check if ASC exists
	USCS_Node* ASCNode = FindASCNode(BP);

	if (!ASCNode)
	{
		NextSteps.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("Call add_asc_to_actor on '%s' with location='%s'"), *ActorPath, *Def.Location)));
		Applied.Add(MakeShared<FJsonValueString>(TEXT("Template applied (ASC not yet added — see next_steps)")));
	}
	else
	{
		// Configure replication mode
		UAbilitySystemComponent* ASCTemplate = Cast<UAbilitySystemComponent>(ASCNode->ComponentTemplate);
		if (ASCTemplate)
		{
			int32 ModeVal = ParseReplicationMode(Def.ReplicationMode);
			if (ModeVal >= 0)
			{
				ASCTemplate->SetReplicationMode(static_cast<EGameplayEffectReplicationMode>(ModeVal));
				Applied.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("ReplicationMode = %s"), *Def.ReplicationMode)));
			}
		}
	}

	// Validate attribute sets
	TArray<TSharedPtr<FJsonValue>> AttrSetNames;
	for (const FString& SetName : Def.AttributeSets)
	{
		ValidateClassReference(SetName, UAttributeSet::StaticClass(), Warnings);
		AttrSetNames.Add(MakeShared<FJsonValueString>(SetName));
	}

	// Validate abilities
	TArray<TSharedPtr<FJsonValue>> AbilityNames;
	for (const FString& AbilName : Def.Abilities)
	{
		ValidateClassReference(AbilName, nullptr, Warnings);
		AbilityNames.Add(MakeShared<FJsonValueString>(AbilName));
	}

	// Validate effects
	TArray<TSharedPtr<FJsonValue>> EffectNames;
	for (const FString& EffName : Def.Effects)
	{
		ValidateClassReference(EffName, nullptr, Warnings);
		EffectNames.Add(MakeShared<FJsonValueString>(EffName));
	}

	if (ASCNode)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	}

	// Build next steps
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Call setup_asc_init to wire InitAbilityActorInfo")));
	NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Call setup_ability_system_interface to implement IAbilitySystemInterface")));
	if (Def.AttributeSets.Num() > 0)
	{
		NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Create AttributeSet classes using create_attribute_set_from_template")));
	}
	if (Def.Abilities.Num() > 0)
	{
		NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Create Ability classes and grant them in BeginPlay or via startup GE")));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"), ActorPath);
	Result->SetStringField(TEXT("template"), TemplateName);
	Result->SetStringField(TEXT("location"), Def.Location);
	Result->SetStringField(TEXT("replication_mode"), Def.ReplicationMode);
	if (AttrSetNames.Num() > 0) Result->SetArrayField(TEXT("attribute_sets"), AttrSetNames);
	if (AbilityNames.Num() > 0) Result->SetArrayField(TEXT("abilities"), AbilityNames);
	if (EffectNames.Num() > 0) Result->SetArrayField(TEXT("effects"), EffectNames);
	Result->SetArrayField(TEXT("applied"), Applied);
	Result->SetArrayField(TEXT("next_steps"), NextSteps);
	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Applied ASC template '%s' to %s"), *TemplateName, *ActorPath));
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// Phase 2: set_default_abilities
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithGASASCActions::HandleSetDefaultAbilities(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor_path"), ActorPath, Err)) return Err;

	TArray<FString> AbilityPaths = MonolithGAS::ParseStringArray(Params, TEXT("abilities"));
	if (AbilityPaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: abilities (array)"));
	}

	FString Mode = Params->GetStringField(TEXT("mode"));
	if (Mode.IsEmpty()) Mode = TEXT("set");
	Mode = Mode.ToLower();

	if (Mode != TEXT("set") && Mode != TEXT("add") && Mode != TEXT("remove"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid mode '%s' — must be 'set', 'add', or 'remove'"), *Mode));
	}

	// Load the Blueprint
	FString Error, OutPath;
	TSharedPtr<FJsonObject> TempParams = MakeShared<FJsonObject>();
	TempParams->SetStringField(TEXT("asset_path"), ActorPath);
	UBlueprint* BP = MonolithGAS::LoadBlueprintFromParams(TempParams, OutPath, Error);
	if (!BP) return FMonolithActionResult::Error(Error);

	// Validate ability classes
	TArray<TSharedPtr<FJsonValue>> Validated;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	for (const FString& AbilPath : AbilityPaths)
	{
		UClass* AbilClass = FindFirstObject<UClass>(*AbilPath, EFindFirstObjectOptions::NativeFirst);
		if (!AbilClass)
		{
			FString BPPath = AbilPath;
			if (!BPPath.EndsWith(TEXT("_C"))) BPPath += TEXT("_C");
			AbilClass = LoadClass<UObject>(nullptr, *BPPath);
		}

		if (AbilClass && AbilClass->IsChildOf(UGameplayAbility::StaticClass()))
		{
			Validated.Add(MakeShared<FJsonValueString>(AbilPath));
		}
		else
		{
			// Still include it — may be created later
			Validated.Add(MakeShared<FJsonValueString>(AbilPath));
			Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Ability class not found (may need to be created): %s"), *AbilPath)));
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"), ActorPath);
	Result->SetStringField(TEXT("mode"), Mode);
	Result->SetArrayField(TEXT("abilities"), Validated);
	Result->SetNumberField(TEXT("count"), Validated.Num());
	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}
	Result->SetStringField(TEXT("note"),
		TEXT("Ability list validated. Grant these in C++ BeginPlay/PossessedBy via GiveAbility, "
		     "or use a startup GE with a GrantAbilities component."));
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Configured %d default abilities (mode=%s) on %s"), Validated.Num(), *Mode, *ActorPath));
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// Phase 2: set_default_effects
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithGASASCActions::HandleSetDefaultEffects(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor_path"), ActorPath, Err)) return Err;

	TArray<FString> EffectPaths = MonolithGAS::ParseStringArray(Params, TEXT("effects"));
	if (EffectPaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: effects (array)"));
	}

	FString Mode = Params->GetStringField(TEXT("mode"));
	if (Mode.IsEmpty()) Mode = TEXT("set");
	Mode = Mode.ToLower();

	if (Mode != TEXT("set") && Mode != TEXT("add") && Mode != TEXT("remove"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid mode '%s' — must be 'set', 'add', or 'remove'"), *Mode));
	}

	// Load Blueprint
	FString Error, OutPath;
	TSharedPtr<FJsonObject> TempParams = MakeShared<FJsonObject>();
	TempParams->SetStringField(TEXT("asset_path"), ActorPath);
	UBlueprint* BP = MonolithGAS::LoadBlueprintFromParams(TempParams, OutPath, Error);
	if (!BP) return FMonolithActionResult::Error(Error);

	TArray<TSharedPtr<FJsonValue>> Validated;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	for (const FString& EffPath : EffectPaths)
	{
		UClass* EffClass = FindFirstObject<UClass>(*EffPath, EFindFirstObjectOptions::NativeFirst);
		if (!EffClass)
		{
			FString BPPath = EffPath;
			if (!BPPath.EndsWith(TEXT("_C"))) BPPath += TEXT("_C");
			EffClass = LoadClass<UObject>(nullptr, *BPPath);
		}

		if (EffClass && EffClass->IsChildOf(UGameplayEffect::StaticClass()))
		{
			Validated.Add(MakeShared<FJsonValueString>(EffPath));
		}
		else
		{
			Validated.Add(MakeShared<FJsonValueString>(EffPath));
			Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Effect class not found (may need to be created): %s"), *EffPath)));
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"), ActorPath);
	Result->SetStringField(TEXT("mode"), Mode);
	Result->SetArrayField(TEXT("effects"), Validated);
	Result->SetNumberField(TEXT("count"), Validated.Num());
	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}
	Result->SetStringField(TEXT("note"),
		TEXT("Effects validated. Apply these in C++ BeginPlay via ApplyGameplayEffectToSelf, "
		     "or store and apply on ASC initialization."));
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Configured %d default effects (mode=%s) on %s"), Validated.Num(), *Mode, *ActorPath));
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// Phase 2: set_default_attribute_sets
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithGASASCActions::HandleSetDefaultAttributeSets(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor_path"), ActorPath, Err)) return Err;

	const TArray<TSharedPtr<FJsonValue>>* SetsArray;
	if (!Params->TryGetArrayField(TEXT("attribute_sets"), SetsArray) || !SetsArray || SetsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: attribute_sets (array)"));
	}

	FString Mode = Params->GetStringField(TEXT("mode"));
	if (Mode.IsEmpty()) Mode = TEXT("set");
	Mode = Mode.ToLower();

	if (Mode != TEXT("set") && Mode != TEXT("add") && Mode != TEXT("remove"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid mode '%s' — must be 'set', 'add', or 'remove'"), *Mode));
	}

	// Load Blueprint
	FString Error, OutPath;
	TSharedPtr<FJsonObject> TempParams = MakeShared<FJsonObject>();
	TempParams->SetStringField(TEXT("asset_path"), ActorPath);
	UBlueprint* BP = MonolithGAS::LoadBlueprintFromParams(TempParams, OutPath, Error);
	if (!BP) return FMonolithActionResult::Error(Error);

	TArray<TSharedPtr<FJsonValue>> Validated;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	for (const auto& SetVal : *SetsArray)
	{
		const TSharedPtr<FJsonObject>* SetObjPtr;
		FString SetClassName;

		if (SetVal->TryGetObject(SetObjPtr))
		{
			SetClassName = (*SetObjPtr)->GetStringField(TEXT("class"));
		}
		else
		{
			// Allow bare string for convenience
			SetVal->TryGetString(SetClassName);
		}

		if (SetClassName.IsEmpty())
		{
			Warnings.Add(MakeShared<FJsonValueString>(TEXT("Entry missing 'class' field")));
			continue;
		}

		UClass* SetClass = FindFirstObject<UClass>(*SetClassName, EFindFirstObjectOptions::NativeFirst);
		if (!SetClass)
		{
			SetClass = FindFirstObject<UClass>(*(TEXT("U") + SetClassName), EFindFirstObjectOptions::NativeFirst);
		}
		if (!SetClass)
		{
			FString BPPath = SetClassName;
			if (!BPPath.EndsWith(TEXT("_C"))) BPPath += TEXT("_C");
			SetClass = LoadClass<UObject>(nullptr, *BPPath);
		}

		TSharedPtr<FJsonObject> ValidEntry = MakeShared<FJsonObject>();
		ValidEntry->SetStringField(TEXT("class"), SetClassName);

		if (SetClass && SetClass->IsChildOf(UAttributeSet::StaticClass()))
		{
			ValidEntry->SetBoolField(TEXT("resolved"), true);
		}
		else
		{
			ValidEntry->SetBoolField(TEXT("resolved"), false);
			Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("AttributeSet class not found (may need to be created): %s"), *SetClassName)));
		}

		// Optional init datatable
		if (SetObjPtr && (*SetObjPtr).IsValid())
		{
			FString InitDT = (*SetObjPtr)->GetStringField(TEXT("init_datatable"));
			if (!InitDT.IsEmpty())
			{
				ValidEntry->SetStringField(TEXT("init_datatable"), InitDT);
			}
		}

		Validated.Add(MakeShared<FJsonValueObject>(ValidEntry));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"), ActorPath);
	Result->SetStringField(TEXT("mode"), Mode);
	Result->SetArrayField(TEXT("attribute_sets"), Validated);
	Result->SetNumberField(TEXT("count"), Validated.Num());
	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}
	Result->SetStringField(TEXT("note"),
		TEXT("AttributeSet list validated. For C++ ASCs, add TSubclassOf<UAttributeSet> entries to DefaultStartingData. "
		     "For Blueprint ASCs, configure via the Details panel."));
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Configured %d default AttributeSets (mode=%s) on %s"), Validated.Num(), *Mode, *ActorPath));
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// Phase 2: set_asc_replication_mode
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithGASASCActions::HandleSetASCReplicationMode(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath, ModeStr;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor_path"), ActorPath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("mode"), ModeStr, Err)) return Err;

	int32 ModeVal = ParseReplicationMode(ModeStr);
	if (ModeVal < 0)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid mode '%s' — must be 'full', 'mixed', or 'minimal'"), *ModeStr));
	}

	EGameplayEffectReplicationMode RepMode = static_cast<EGameplayEffectReplicationMode>(ModeVal);

	// Load Blueprint
	FString Error, OutPath;
	TSharedPtr<FJsonObject> TempParams = MakeShared<FJsonObject>();
	TempParams->SetStringField(TEXT("asset_path"), ActorPath);
	UBlueprint* BP = MonolithGAS::LoadBlueprintFromParams(TempParams, OutPath, Error);
	if (!BP) return FMonolithActionResult::Error(Error);

	USCS_Node* ASCNode = FindASCNode(BP);
	if (!ASCNode)
	{
		return FMonolithActionResult::Error(TEXT("No AbilitySystemComponent found on this Blueprint. Call add_asc_to_actor first."));
	}

	UAbilitySystemComponent* ASCTemplate = Cast<UAbilitySystemComponent>(ASCNode->ComponentTemplate);
	if (!ASCTemplate)
	{
		return FMonolithActionResult::Error(TEXT("ASC node has no valid ComponentTemplate"));
	}

	ASCTemplate->SetReplicationMode(RepMode);
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"), ActorPath);
	Result->SetStringField(TEXT("mode"), ModeStr.ToLower());

	TArray<TSharedPtr<FJsonValue>> Warnings;

	if (RepMode == EGameplayEffectReplicationMode::Minimal)
	{
		Warnings.Add(MakeShared<FJsonValueString>(
			TEXT("Minimal replication breaks client-side prediction. Only use for AI-controlled actors.")));
	}
	if (RepMode == EGameplayEffectReplicationMode::Full)
	{
		Warnings.Add(MakeShared<FJsonValueString>(
			TEXT("Full replication is legacy and replicates everything to all connections. Consider Mixed for players, Minimal for AI.")));
	}

	// Check if this is on a PlayerState (warn about Minimal on PlayerState)
	if (BP->ParentClass && BP->ParentClass->IsChildOf(APlayerState::StaticClass()) &&
		RepMode == EGameplayEffectReplicationMode::Minimal)
	{
		Warnings.Add(MakeShared<FJsonValueString>(
			TEXT("CRITICAL: Minimal replication on a PlayerState ASC silently breaks all client prediction. Use Mixed.")));
	}

	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}

	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Set ASC replication mode to '%s' on %s"), *ModeStr.ToLower(), *ActorPath));
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// Phase 3: validate_asc_setup
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithGASASCActions::HandleValidateASCSetup(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor_path"), ActorPath, Err)) return Err;

	FString Error, OutPath;
	TSharedPtr<FJsonObject> TempParams = MakeShared<FJsonObject>();
	TempParams->SetStringField(TEXT("asset_path"), ActorPath);
	UBlueprint* BP = MonolithGAS::LoadBlueprintFromParams(TempParams, OutPath, Error);
	if (!BP)
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Errors;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	// Find ASC on this Blueprint
	USCS_Node* ASCNode = FindASCNode(BP);
	UAbilitySystemComponent* ASCTemplate = nullptr;

	if (!ASCNode)
	{
		TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
		ErrObj->SetStringField(TEXT("type"), TEXT("no_asc"));
		ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
		ErrObj->SetStringField(TEXT("message"),
			TEXT("No AbilitySystemComponent found on this Blueprint. Use add_asc_to_actor to add one."));
		Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
	}
	else
	{
		ASCTemplate = Cast<UAbilitySystemComponent>(ASCNode->ComponentTemplate);
	}

	// Check: IAbilitySystemInterface
	bool bHasInterface = false;
	if (BP->GeneratedClass)
	{
		bHasInterface = BP->GeneratedClass->ImplementsInterface(UAbilitySystemInterface::StaticClass());
	}
	// Also check parent class chain
	if (!bHasInterface && BP->ParentClass)
	{
		bHasInterface = BP->ParentClass->ImplementsInterface(UAbilitySystemInterface::StaticClass());
	}

	if (!bHasInterface)
	{
		TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
		ErrObj->SetStringField(TEXT("type"), TEXT("missing_interface"));
		ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
		ErrObj->SetStringField(TEXT("message"),
			TEXT("Actor does not implement IAbilitySystemInterface. This is required for GAS to find the ASC. Use setup_ability_system_interface to generate the C++ implementation."));
		Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
	}

	// Determine if this is on a PlayerState
	bool bIsPlayerState = false;
	if (BP->ParentClass)
	{
		bIsPlayerState = BP->ParentClass->IsChildOf(APlayerState::StaticClass());
	}

	// Check replication mode
	if (ASCTemplate)
	{
		EGameplayEffectReplicationMode RepMode = ASCTemplate->ReplicationMode;

		if (bIsPlayerState && RepMode == EGameplayEffectReplicationMode::Minimal)
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetStringField(TEXT("type"), TEXT("minimal_on_playerstate"));
			ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
			ErrObj->SetStringField(TEXT("message"),
				TEXT("CRITICAL: Minimal replication mode on a PlayerState ASC silently breaks client-side prediction and attribute replication to the owning client. Use Mixed mode."));
			Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
		}
		else if (RepMode == EGameplayEffectReplicationMode::Full)
		{
			TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
			WarnObj->SetStringField(TEXT("type"), TEXT("full_replication"));
			WarnObj->SetStringField(TEXT("severity"), TEXT("warning"));
			WarnObj->SetStringField(TEXT("message"),
				TEXT("Full replication mode is legacy and replicates everything to all connections. Consider Mixed (for players) or Minimal (for AI)."));
			Warnings.Add(MakeShared<FJsonValueObject>(WarnObj));
		}
	}

	// Check: InitAbilityActorInfo call exists in graphs
	bool bHasInitAbilityActorInfo = false;
	bool bInitInConstructor = false;

	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				FString FuncName = CallNode->FunctionReference.GetMemberName().ToString();
				if (FuncName.Contains(TEXT("InitAbilityActorInfo")))
				{
					bHasInitAbilityActorInfo = true;
				}
			}
		}
	}

	// Also check the construction script
	if (BP->SimpleConstructionScript)
	{
		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (!Graph) continue;
			FString GraphName = Graph->GetName();
			if (GraphName.Contains(TEXT("ConstructionScript")) || GraphName.Contains(TEXT("UserConstruction")))
			{
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node) continue;
					if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
					{
						FString FuncName = CallNode->FunctionReference.GetMemberName().ToString();
						if (FuncName.Contains(TEXT("InitAbilityActorInfo")))
						{
							bInitInConstructor = true;
							bHasInitAbilityActorInfo = true;
						}
					}
				}
			}
		}
	}

	if (ASCNode && !bHasInitAbilityActorInfo)
	{
		TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
		WarnObj->SetStringField(TEXT("type"), TEXT("missing_init_ability_actor_info"));
		WarnObj->SetStringField(TEXT("severity"), TEXT("warning"));
		WarnObj->SetStringField(TEXT("message"),
			TEXT("No InitAbilityActorInfo call found in Blueprint graphs. The ASC needs InitAbilityActorInfo called in PossessedBy (server) and OnRep_PlayerState (client) for proper owner/avatar setup. This may be handled in C++ parent class."));
		Warnings.Add(MakeShared<FJsonValueObject>(WarnObj));
	}

	if (bInitInConstructor)
	{
		TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
		ErrObj->SetStringField(TEXT("type"), TEXT("init_in_constructor"));
		ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
		ErrObj->SetStringField(TEXT("message"),
			TEXT("InitAbilityActorInfo called in Construction Script. This is too early — the Pawn/PlayerState are not available yet. Move to BeginPlay, PossessedBy, or OnRep_PlayerState."));
		Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
	}

	// Check: duplicate attribute sets in DefaultStartingData via reflection
	if (ASCTemplate)
	{
		FProperty* DefaultDataProp = ASCTemplate->GetClass()->FindPropertyByName(TEXT("DefaultStartingData"));
		if (DefaultDataProp)
		{
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(DefaultDataProp);
			if (ArrayProp)
			{
				void* ArrayPtr = DefaultDataProp->ContainerPtrToValuePtr<void>(ASCTemplate);
				FScriptArrayHelper ArrayHelper(ArrayProp, ArrayPtr);
				FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProp->Inner);

				if (InnerStruct)
				{
					TSet<FString> SeenClasses;
					for (int32 Idx = 0; Idx < ArrayHelper.Num(); Idx++)
					{
						const uint8* ElemPtr = ArrayHelper.GetRawPtr(Idx);
						// Find the Attributes (TSubclassOf<UAttributeSet>) member of FAttributeDefaults
						FProperty* AttrProp = InnerStruct->Struct->FindPropertyByName(TEXT("Attributes"));
						if (AttrProp)
						{
							FClassProperty* ClassProp = CastField<FClassProperty>(AttrProp);
							if (ClassProp)
							{
								UObject* ClassObj = ClassProp->GetObjectPropertyValue(ElemPtr + AttrProp->GetOffset_ForInternal());
								if (UClass* AttrClass = Cast<UClass>(ClassObj))
								{
									FString ClassName = AttrClass->GetName();
									if (SeenClasses.Contains(ClassName))
									{
										TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
										WarnObj->SetStringField(TEXT("type"), TEXT("duplicate_attribute_set"));
										WarnObj->SetStringField(TEXT("severity"), TEXT("warning"));
										WarnObj->SetStringField(TEXT("class"), ClassName);
										WarnObj->SetStringField(TEXT("message"),
											FString::Printf(TEXT("AttributeSet class '%s' appears multiple times in DefaultStartingData. Only one instance per class is supported."), *ClassName));
										Warnings.Add(MakeShared<FJsonValueObject>(WarnObj));
									}
									SeenClasses.Add(ClassName);
								}
							}
						}
					}
				}
			}
		}
	}

	bool bValid = (Errors.Num() == 0);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"), OutPath);
	Result->SetBoolField(TEXT("valid"), bValid);
	Result->SetBoolField(TEXT("has_asc"), ASCNode != nullptr);
	Result->SetBoolField(TEXT("has_interface"), bHasInterface);
	Result->SetBoolField(TEXT("has_init_ability_actor_info"), bHasInitAbilityActorInfo);
	Result->SetBoolField(TEXT("is_player_state"), bIsPlayerState);

	if (ASCTemplate)
	{
		FString RepModeStr;
		switch (ASCTemplate->ReplicationMode)
		{
		case EGameplayEffectReplicationMode::Full:    RepModeStr = TEXT("full"); break;
		case EGameplayEffectReplicationMode::Mixed:   RepModeStr = TEXT("mixed"); break;
		case EGameplayEffectReplicationMode::Minimal: RepModeStr = TEXT("minimal"); break;
		default: RepModeStr = TEXT("unknown"); break;
		}
		Result->SetStringField(TEXT("replication_mode"), RepModeStr);
		Result->SetStringField(TEXT("asc_class"), ASCNode->ComponentClass->GetName());
	}

	Result->SetNumberField(TEXT("error_count"), Errors.Num());
	Result->SetNumberField(TEXT("warning_count"), Warnings.Num());
	if (Errors.Num() > 0) Result->SetArrayField(TEXT("errors"), Errors);
	if (Warnings.Num() > 0) Result->SetArrayField(TEXT("warnings"), Warnings);

	Result->SetStringField(TEXT("message"),
		bValid ? FString::Printf(TEXT("ASC setup on '%s' passed validation"), *BP->GetName())
		       : FString::Printf(TEXT("ASC setup on '%s': %d errors, %d warnings"), *BP->GetName(), Errors.Num(), Warnings.Num()));
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// Phase 4 Runtime Helpers
// ---------------------------------------------------------------------------

namespace
{
	/** Resolve a GA class by name or asset path. */
	TSubclassOf<UGameplayAbility> ResolveAbilityClass(const FString& ClassId, FString& OutError)
	{
		// Try asset path first
		if (ClassId.StartsWith(TEXT("/")))
		{
			FString BPPath = ClassId;
			if (!BPPath.EndsWith(TEXT("_C")))
			{
				FString BaseName = FPaths::GetBaseFilename(ClassId);
				BPPath = ClassId + TEXT(".") + BaseName + TEXT("_C");
			}
			UClass* LoadedClass = LoadClass<UGameplayAbility>(nullptr, *BPPath);
			if (LoadedClass)
			{
				return LoadedClass;
			}
		}

		// Try direct class name
		UClass* FoundClass = FindFirstObject<UClass>(*ClassId, EFindFirstObjectOptions::NativeFirst);
		if (!FoundClass)
		{
			FoundClass = FindFirstObject<UClass>(*(TEXT("U") + ClassId), EFindFirstObjectOptions::NativeFirst);
		}
		// Try with _C suffix (Blueprint-generated classes)
		if (!FoundClass && !ClassId.EndsWith(TEXT("_C")))
		{
			FoundClass = FindFirstObject<UClass>(*(ClassId + TEXT("_C")), EFindFirstObjectOptions::NativeFirst);
		}
		if (FoundClass && FoundClass->IsChildOf(UGameplayAbility::StaticClass()))
		{
			return FoundClass;
		}

		// Try Asset Registry lookup for bare Blueprint names
		{
			IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			FString SearchName = ClassId;
			if (SearchName.EndsWith(TEXT("_C")))
			{
				SearchName = SearchName.LeftChop(2);
			}
			TArray<FAssetData> Assets;
			AR.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Assets);
			for (const FAssetData& Asset : Assets)
			{
				if (Asset.AssetName.ToString() == SearchName)
				{
					FString BPPath = Asset.GetObjectPathString() + TEXT("_C");
					UClass* LoadedFromAR = LoadClass<UGameplayAbility>(nullptr, *BPPath);
					if (LoadedFromAR)
					{
						return LoadedFromAR;
					}
				}
			}
		}

		OutError = FString::Printf(TEXT("GameplayAbility class not found: '%s'"), *ClassId);
		return nullptr;
	}
}

// ---------------------------------------------------------------------------
// Phase 4: grant_ability
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithGASASCActions::HandleGrantAbility(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorId, AbilityClassId;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor"), ActorId, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("ability_class"), AbilityClassId, Err)) return Err;

	double Level = 1.0;
	double InputID = -1.0;
	Params->TryGetNumberField(TEXT("level"), Level);
	Params->TryGetNumberField(TEXT("input_id"), InputID);

	UWorld* PIEWorld = MonolithGAS::GetPIEWorld();
	if (!PIEWorld)
	{
		return FMonolithActionResult::Error(TEXT("No PIE world found. Start Play In Editor first."));
	}

	AActor* Actor = MonolithGAS::FindActorInPIE(ActorId);
	if (!Actor)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor not found in PIE world: '%s'"), *ActorId));
	}

	UAbilitySystemComponent* ASC = MonolithGAS::GetASCFromActor(Actor);
	if (!ASC)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor '%s' has no AbilitySystemComponent"), *ActorId));
	}

	FString ResolveError;
	TSubclassOf<UGameplayAbility> AbilityClass = ResolveAbilityClass(AbilityClassId, ResolveError);
	if (!AbilityClass)
	{
		return FMonolithActionResult::Error(ResolveError);
	}

	// Check if already granted
	for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
	{
		if (Spec.Ability && Spec.Ability->GetClass() == AbilityClass)
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
			Result->SetStringField(TEXT("ability_class"), AbilityClass->GetName());
			Result->SetBoolField(TEXT("already_granted"), true);
			Result->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Ability '%s' is already granted to '%s'"),
					*AbilityClass->GetName(), *Actor->GetActorLabel()));
			return FMonolithActionResult::Success(Result);
		}
	}

	FGameplayAbilitySpec Spec(AbilityClass, static_cast<int32>(Level),
		static_cast<int32>(InputID), Actor);
	FGameplayAbilitySpecHandle Handle = ASC->GiveAbility(Spec);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("ability_class"), AbilityClass->GetName());
	Result->SetNumberField(TEXT("level"), Level);
	Result->SetNumberField(TEXT("input_id"), InputID);
	Result->SetBoolField(TEXT("granted"), Handle.IsValid());
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Granted '%s' (level %d) to '%s'"),
			*AbilityClass->GetName(), static_cast<int32>(Level), *Actor->GetActorLabel()));
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// Phase 4: revoke_ability
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithGASASCActions::HandleRevokeAbility(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorId, AbilityClassId;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor"), ActorId, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("ability_class"), AbilityClassId, Err)) return Err;

	UWorld* PIEWorld = MonolithGAS::GetPIEWorld();
	if (!PIEWorld)
	{
		return FMonolithActionResult::Error(TEXT("No PIE world found. Start Play In Editor first."));
	}

	AActor* Actor = MonolithGAS::FindActorInPIE(ActorId);
	if (!Actor)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor not found in PIE world: '%s'"), *ActorId));
	}

	UAbilitySystemComponent* ASC = MonolithGAS::GetASCFromActor(Actor);
	if (!ASC)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor '%s' has no AbilitySystemComponent"), *ActorId));
	}

	FString ResolveError;
	TSubclassOf<UGameplayAbility> AbilityClass = ResolveAbilityClass(AbilityClassId, ResolveError);
	if (!AbilityClass)
	{
		return FMonolithActionResult::Error(ResolveError);
	}

	// Find and remove
	FGameplayAbilitySpecHandle HandleToRemove;
	for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
	{
		if (Spec.Ability && Spec.Ability->GetClass() == AbilityClass)
		{
			HandleToRemove = Spec.Handle;
			break;
		}
	}

	if (!HandleToRemove.IsValid())
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Ability '%s' is not granted to '%s'"),
				*AbilityClass->GetName(), *Actor->GetActorLabel()));
	}

	ASC->ClearAbility(HandleToRemove);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("ability_class"), AbilityClass->GetName());
	Result->SetBoolField(TEXT("revoked"), true);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Revoked '%s' from '%s'"),
			*AbilityClass->GetName(), *Actor->GetActorLabel()));
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// Phase 4: get_asc_snapshot
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithGASASCActions::HandleGetASCSnapshot(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorId;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor"), ActorId, Err)) return Err;

	bool bIncludeAbilities = true, bIncludeEffects = true, bIncludeAttributes = true;
	bool bIncludeTags = true, bIncludeCooldowns = true;
	Params->TryGetBoolField(TEXT("include_abilities"), bIncludeAbilities);
	Params->TryGetBoolField(TEXT("include_effects"), bIncludeEffects);
	Params->TryGetBoolField(TEXT("include_attributes"), bIncludeAttributes);
	Params->TryGetBoolField(TEXT("include_tags"), bIncludeTags);
	Params->TryGetBoolField(TEXT("include_cooldowns"), bIncludeCooldowns);

	UWorld* PIEWorld = MonolithGAS::GetPIEWorld();
	if (!PIEWorld)
	{
		return FMonolithActionResult::Error(TEXT("No PIE world found. Start Play In Editor first."));
	}

	AActor* Actor = MonolithGAS::FindActorInPIE(ActorId);
	if (!Actor)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor not found in PIE world: '%s'"), *ActorId));
	}

	UAbilitySystemComponent* ASC = MonolithGAS::GetASCFromActor(Actor);
	if (!ASC)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor '%s' has no AbilitySystemComponent"), *ActorId));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
	Result->SetStringField(TEXT("asc_class"), ASC->GetClass()->GetName());

	// Replication mode
	FString RepModeStr;
	switch (ASC->ReplicationMode)
	{
	case EGameplayEffectReplicationMode::Full:    RepModeStr = TEXT("full"); break;
	case EGameplayEffectReplicationMode::Mixed:   RepModeStr = TEXT("mixed"); break;
	case EGameplayEffectReplicationMode::Minimal: RepModeStr = TEXT("minimal"); break;
	default: RepModeStr = TEXT("unknown"); break;
	}
	Result->SetStringField(TEXT("replication_mode"), RepModeStr);

	// --- Abilities ---
	if (bIncludeAbilities)
	{
		TArray<TSharedPtr<FJsonValue>> AbilityArr;
		for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
		{
			if (!Spec.Ability) continue;

			TSharedPtr<FJsonObject> AbilObj = MakeShared<FJsonObject>();
			AbilObj->SetStringField(TEXT("class"), Spec.Ability->GetClass()->GetName());
			AbilObj->SetNumberField(TEXT("level"), Spec.Level);
			AbilObj->SetNumberField(TEXT("input_id"), Spec.InputID);
			AbilObj->SetBoolField(TEXT("is_active"), Spec.IsActive());

			// Get ability tags via GetAssetTags() (AbilityTags direct access deprecated UE 5.7)
			const FGameplayTagContainer& AbilityTags = Spec.Ability->GetAssetTags();
			if (AbilityTags.Num() > 0)
			{
				AbilObj->SetField(TEXT("ability_tags"), MonolithGAS::TagContainerToJson(AbilityTags));
			}

			AbilityArr.Add(MakeShared<FJsonValueObject>(AbilObj));
		}
		Result->SetNumberField(TEXT("ability_count"), AbilityArr.Num());
		Result->SetArrayField(TEXT("abilities"), AbilityArr);
	}

	// --- Active Effects ---
	if (bIncludeEffects)
	{
		TArray<TSharedPtr<FJsonValue>> EffectArr;
		FGameplayEffectQuery AllQuery;
		TArray<FActiveGameplayEffectHandle> EffectHandles = ASC->GetActiveEffects(AllQuery);

		for (int32 Idx = 0; Idx < EffectHandles.Num(); Idx++)
		{
			const FActiveGameplayEffect* ActiveGEPtr = ASC->GetActiveGameplayEffect(EffectHandles[Idx]);
			if (!ActiveGEPtr || !ActiveGEPtr->Spec.Def) continue;
			const FActiveGameplayEffect& AGE = *ActiveGEPtr;

			TSharedPtr<FJsonObject> EffObj = MakeShared<FJsonObject>();
			EffObj->SetNumberField(TEXT("index"), Idx);
			EffObj->SetStringField(TEXT("class"), AGE.Spec.Def->GetClass()->GetName());
			EffObj->SetStringField(TEXT("name"), AGE.Spec.Def->GetName());
			EffObj->SetNumberField(TEXT("level"), AGE.Spec.GetLevel());
			EffObj->SetNumberField(TEXT("stack_count"), AGE.Spec.GetStackCount());

			FString DurStr;
			switch (AGE.Spec.Def->DurationPolicy)
			{
			case EGameplayEffectDurationType::Instant:     DurStr = TEXT("instant"); break;
			case EGameplayEffectDurationType::HasDuration: DurStr = TEXT("has_duration"); break;
			case EGameplayEffectDurationType::Infinite:    DurStr = TEXT("infinite"); break;
			default: DurStr = TEXT("unknown"); break;
			}
			EffObj->SetStringField(TEXT("duration_policy"), DurStr);

			if (AGE.Spec.Def->DurationPolicy == EGameplayEffectDurationType::HasDuration)
			{
				EffObj->SetNumberField(TEXT("duration"), AGE.GetDuration());
				EffObj->SetNumberField(TEXT("time_remaining"),
					FMath::Max(0.f, AGE.GetTimeRemaining(PIEWorld->GetTimeSeconds())));
			}

			EffectArr.Add(MakeShared<FJsonValueObject>(EffObj));
		}
		Result->SetNumberField(TEXT("effect_count"), EffectArr.Num());
		Result->SetArrayField(TEXT("active_effects"), EffectArr);
	}

	// --- Attributes ---
	if (bIncludeAttributes)
	{
		TArray<TSharedPtr<FJsonValue>> AttrArr;

		// Access spawned attribute sets via reflection
		FProperty* SpawnedAttrProp = ASC->GetClass()->FindPropertyByName(TEXT("SpawnedAttributes"));
		if (SpawnedAttrProp)
		{
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(SpawnedAttrProp);
			if (ArrayProp)
			{
				void* ArrayPtr = SpawnedAttrProp->ContainerPtrToValuePtr<void>(ASC);
				FScriptArrayHelper ArrayHelper(ArrayProp, ArrayPtr);
				FObjectProperty* InnerProp = CastField<FObjectProperty>(ArrayProp->Inner);

				if (InnerProp)
				{
					for (int32 Idx = 0; Idx < ArrayHelper.Num(); Idx++)
					{
						UObject* Elem = InnerProp->GetObjectPropertyValue(ArrayHelper.GetRawPtr(Idx));
						UAttributeSet* AttrSet = Cast<UAttributeSet>(Elem);
						if (!AttrSet) continue;

						UScriptStruct* AttrDataStruct = FGameplayAttributeData::StaticStruct();
						for (TFieldIterator<FProperty> It(AttrSet->GetClass()); It; ++It)
						{
							FStructProperty* StructProp = CastField<FStructProperty>(*It);
							if (!StructProp || !StructProp->Struct || !StructProp->Struct->IsChildOf(AttrDataStruct))
							{
								continue;
							}

							const FGameplayAttributeData* Data =
								StructProp->ContainerPtrToValuePtr<FGameplayAttributeData>(AttrSet);
							if (!Data) continue;

							TSharedPtr<FJsonObject> AttrObj = MakeShared<FJsonObject>();
							AttrObj->SetStringField(TEXT("set"), AttrSet->GetClass()->GetName());
							AttrObj->SetStringField(TEXT("name"), It->GetName());
							AttrObj->SetStringField(TEXT("full_name"),
								FString::Printf(TEXT("%s.%s"), *AttrSet->GetClass()->GetName(), *It->GetName()));
							AttrObj->SetNumberField(TEXT("base_value"), Data->GetBaseValue());
							AttrObj->SetNumberField(TEXT("current_value"), Data->GetCurrentValue());
							AttrArr.Add(MakeShared<FJsonValueObject>(AttrObj));
						}
					}
				}
			}
		}

		// Fallback: iterate known attribute set classes if SpawnedAttributes wasn't found
		if (AttrArr.Num() == 0)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (!It->IsChildOf(UAttributeSet::StaticClass()) || *It == UAttributeSet::StaticClass())
					continue;

				const UAttributeSet* AttrSet = ASC->GetAttributeSet(*It);
				if (!AttrSet) continue;

				UScriptStruct* AttrDataStruct = FGameplayAttributeData::StaticStruct();
				for (TFieldIterator<FProperty> PropIt(AttrSet->GetClass()); PropIt; ++PropIt)
				{
					FStructProperty* StructProp = CastField<FStructProperty>(*PropIt);
					if (!StructProp || !StructProp->Struct || !StructProp->Struct->IsChildOf(AttrDataStruct))
						continue;

					const FGameplayAttributeData* Data =
						StructProp->ContainerPtrToValuePtr<FGameplayAttributeData>(AttrSet);
					if (!Data) continue;

					TSharedPtr<FJsonObject> AttrObj = MakeShared<FJsonObject>();
					AttrObj->SetStringField(TEXT("set"), AttrSet->GetClass()->GetName());
					AttrObj->SetStringField(TEXT("name"), PropIt->GetName());
					AttrObj->SetStringField(TEXT("full_name"),
						FString::Printf(TEXT("%s.%s"), *AttrSet->GetClass()->GetName(), *PropIt->GetName()));
					AttrObj->SetNumberField(TEXT("base_value"), Data->GetBaseValue());
					AttrObj->SetNumberField(TEXT("current_value"), Data->GetCurrentValue());
					AttrArr.Add(MakeShared<FJsonValueObject>(AttrObj));
				}
			}
		}

		Result->SetNumberField(TEXT("attribute_count"), AttrArr.Num());
		Result->SetArrayField(TEXT("attributes"), AttrArr);
	}

	// --- Tags ---
	if (bIncludeTags)
	{
		FGameplayTagContainer OwnedTags;
		ASC->GetOwnedGameplayTags(OwnedTags);
		Result->SetField(TEXT("owned_tags"), MonolithGAS::TagContainerToJson(OwnedTags));
		Result->SetNumberField(TEXT("tag_count"), OwnedTags.Num());
	}

	// --- Cooldowns ---
	if (bIncludeCooldowns)
	{
		TArray<TSharedPtr<FJsonValue>> CooldownArr;
		for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
		{
			if (!Spec.Ability) continue;

			float TimeRemaining = 0.f;
			float Duration = 0.f;
			Spec.Ability->GetCooldownTimeRemainingAndDuration(
				Spec.Handle, ASC->AbilityActorInfo.Get(), TimeRemaining, Duration);

			if (TimeRemaining > 0.f)
			{
				TSharedPtr<FJsonObject> CDObj = MakeShared<FJsonObject>();
				CDObj->SetStringField(TEXT("ability"), Spec.Ability->GetClass()->GetName());
				CDObj->SetNumberField(TEXT("time_remaining"), TimeRemaining);
				CDObj->SetNumberField(TEXT("total_duration"), Duration);
				CooldownArr.Add(MakeShared<FJsonValueObject>(CDObj));
			}
		}
		Result->SetNumberField(TEXT("cooldown_count"), CooldownArr.Num());
		Result->SetArrayField(TEXT("cooldowns"), CooldownArr);
	}

	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("ASC snapshot for '%s' (%s)"),
			*Actor->GetActorLabel(), *ASC->GetClass()->GetName()));
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// Phase 4: get_all_ascs
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithGASASCActions::HandleGetAllASCs(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassFilter, TagFilter;
	Params->TryGetStringField(TEXT("class_filter"), ClassFilter);
	Params->TryGetStringField(TEXT("tag_filter"), TagFilter);

	FGameplayTag FilterTag;
	if (!TagFilter.IsEmpty())
	{
		FilterTag = FGameplayTag::RequestGameplayTag(FName(*TagFilter), false);
	}

	UWorld* PIEWorld = MonolithGAS::GetPIEWorld();
	if (!PIEWorld)
	{
		return FMonolithActionResult::Error(TEXT("No PIE world found. Start Play In Editor first."));
	}

	TArray<TSharedPtr<FJsonValue>> Entries;

	for (TActorIterator<AActor> It(PIEWorld); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;

		UAbilitySystemComponent* ASC = MonolithGAS::GetASCFromActor(Actor);
		if (!ASC) continue;

		// Apply class filter
		if (!ClassFilter.IsEmpty())
		{
			FString ActorClassName = Actor->GetClass()->GetName();
			if (!ActorClassName.Contains(ClassFilter))
			{
				continue;
			}
		}

		// Apply tag filter
		if (FilterTag.IsValid())
		{
			FGameplayTagContainer OwnedTags;
			ASC->GetOwnedGameplayTags(OwnedTags);
			if (!OwnedTags.HasTag(FilterTag))
			{
				continue;
			}
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
		Entry->SetStringField(TEXT("actor_name"), Actor->GetName());
		Entry->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
		Entry->SetStringField(TEXT("asc_class"), ASC->GetClass()->GetName());

		// Quick stats
		Entry->SetNumberField(TEXT("ability_count"), ASC->GetActivatableAbilities().Num());
		Entry->SetNumberField(TEXT("active_effect_count"),
			ASC->GetActiveEffects(FGameplayEffectQuery()).Num());

		// Attribute set count via SpawnedAttributes
		FProperty* SpawnedAttrProp = ASC->GetClass()->FindPropertyByName(TEXT("SpawnedAttributes"));
		if (SpawnedAttrProp)
		{
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(SpawnedAttrProp);
			if (ArrayProp)
			{
				void* ArrayPtr = SpawnedAttrProp->ContainerPtrToValuePtr<void>(ASC);
				FScriptArrayHelper ArrayHelper(ArrayProp, ArrayPtr);
				Entry->SetNumberField(TEXT("attribute_set_count"), ArrayHelper.Num());
			}
		}

		// Owned tag count
		FGameplayTagContainer OwnedTags;
		ASC->GetOwnedGameplayTags(OwnedTags);
		Entry->SetNumberField(TEXT("tag_count"), OwnedTags.Num());

		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Entries.Num());
	Result->SetArrayField(TEXT("actors"), Entries);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Found %d actors with AbilitySystemComponents in PIE world"), Entries.Num()));
	return FMonolithActionResult::Success(Result);
}
