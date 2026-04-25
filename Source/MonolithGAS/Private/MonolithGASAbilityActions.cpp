#include "MonolithGASAbilityActions.h"
#include "MonolithParamSchema.h"
#include "MonolithGASInternal.h"
#include "MonolithAssetUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Abilities/GameplayAbility.h"
#include "Abilities/Tasks/AbilityTask.h"
#include "GameplayEffect.h"
#include "GameplayTagContainer.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "K2Node_LatentAbilityCall.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Event.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphNode_Comment.h"
#include "EditorAssetLibrary.h"

// ============================================================
//  Tag container name -> CDO pointer mapping
// ============================================================

namespace
{
	// ── Property type validation ──────────────────────────────────────────
	// Returns true if FProperty's underlying type matches the requested C++ type T.
	// Prevents silent memory corruption from mismatched ContainerPtrToValuePtr<T> casts.

	template<typename T>
	bool ValidatePropertyType(FProperty* Prop, FName PropName)
	{
		if (!Prop) return false;

		// Bool properties: FBoolProperty uses bitfield storage, must use dedicated accessors
		if constexpr (std::is_same_v<T, bool>)
		{
			if (!CastField<FBoolProperty>(Prop))
			{
				UE_LOG(LogMonolithGAS, Warning,
					TEXT("ValidatePropertyType: Property '%s' is not a bool (actual: %s)"),
					*PropName.ToString(), *Prop->GetCPPType());
				return false;
			}
			return true;
		}
		// Object pointer properties: check class compatibility
		else if constexpr (std::is_pointer_v<T> && std::is_base_of_v<UObject, std::remove_pointer_t<T>>)
		{
			const FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop);
			if (!ObjProp || !ObjProp->PropertyClass->IsChildOf(std::remove_pointer_t<T>::StaticClass()))
			{
				UE_LOG(LogMonolithGAS, Warning,
					TEXT("ValidatePropertyType: Property '%s' is not a compatible object property (actual: %s)"),
					*PropName.ToString(), *Prop->GetCPPType());
				return false;
			}
			return true;
		}
		// Numeric / enum / other: check size at minimum to catch gross mismatches
		else
		{
			if (Prop->GetSize() != sizeof(T))
			{
				UE_LOG(LogMonolithGAS, Warning,
					TEXT("ValidatePropertyType: Property '%s' size mismatch (expected %zu, actual %d). CPP type: %s"),
					*PropName.ToString(), sizeof(T), Prop->GetSize(), *Prop->GetCPPType());
				return false;
			}
			return true;
		}
	}

	// ── Reflection helpers ────────────────────────────────────────────────

	// Reflection helper: set any UPROPERTY by name via FProperty reflection (type-safe)
	template<typename T>
	void SetPropertyByName(UObject* Obj, FName PropName, const T& Value)
	{
		if (!Obj) return;
		FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropName);
		if (Prop)
		{
			if (!ValidatePropertyType<T>(Prop, PropName)) return;
			T* Ptr = Prop->ContainerPtrToValuePtr<T>(Obj);
			if (Ptr) *Ptr = Value;
		}
	}

	template<typename T>
	T* GetPropertyPtrByName(UObject* Obj, FName PropName)
	{
		if (!Obj) return nullptr;
		FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropName);
		if (!Prop) return nullptr;
		if (!ValidatePropertyType<T>(Prop, PropName)) return nullptr;
		return Prop->ContainerPtrToValuePtr<T>(Obj);
	}

	// Reflection helper: get/set a bool UPROPERTY by name on any UObject
	bool GetBoolProperty(const UObject* Obj, FName PropName, bool DefaultVal = false)
	{
		if (!Obj) return DefaultVal;
		FBoolProperty* Prop = CastField<FBoolProperty>(Obj->GetClass()->FindPropertyByName(PropName));
		if (!Prop) return DefaultVal;
		return Prop->GetPropertyValue_InContainer(Obj);
	}

	void SetBoolProperty(UObject* Obj, FName PropName, bool Value)
	{
		if (!Obj) return;
		FBoolProperty* Prop = CastField<FBoolProperty>(Obj->GetClass()->FindPropertyByName(PropName));
		if (Prop)
		{
			Prop->SetPropertyValue_InContainer(Obj, Value);
		}
	}

	// Returns a pointer to the named tag container on a UGameplayAbility CDO via reflection.
	FGameplayTagContainer* GetTagContainerByName(UGameplayAbility* AbilityCDO, const FString& ContainerName)
	{
		return GetPropertyPtrByName<FGameplayTagContainer>(AbilityCDO, FName(*ContainerName));
	}

	// All 10 container names for iteration
	static const TArray<FString> AllContainerNames = {
		TEXT("AbilityTags"),
		TEXT("CancelAbilitiesWithTag"),
		TEXT("BlockAbilitiesWithTag"),
		TEXT("ActivationOwnedTags"),
		TEXT("ActivationRequiredTags"),
		TEXT("ActivationBlockedTags"),
		TEXT("SourceRequiredTags"),
		TEXT("SourceBlockedTags"),
		TEXT("TargetRequiredTags"),
		TEXT("TargetBlockedTags")
	};

	// Helper: load BP, get ability CDO, return error result if anything fails
	struct FAbilityCDOContext
	{
		UBlueprint* BP = nullptr;
		UGameplayAbility* CDO = nullptr;
		FString AssetPath;

		bool Load(const TSharedPtr<FJsonObject>& Params, FMonolithActionResult& OutError)
		{
			FString Error;
			BP = MonolithGAS::LoadBlueprintFromParams(Params, AssetPath, Error);
			if (!BP)
			{
				OutError = FMonolithActionResult::Error(Error);
				return false;
			}
			if (!MonolithGAS::IsAbilityBlueprint(BP))
			{
				OutError = FMonolithActionResult::Error(
					FString::Printf(TEXT("'%s' is not a GameplayAbility Blueprint"), *AssetPath));
				return false;
			}
			CDO = MonolithGAS::GetBlueprintCDO<UGameplayAbility>(BP);
			if (!CDO)
			{
				OutError = FMonolithActionResult::Error(
					FString::Printf(TEXT("Failed to get CDO for '%s'"), *AssetPath));
				return false;
			}
			return true;
		}

		void MarkModified()
		{
			if (BP)
			{
				BP->Modify();
				FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
			}
		}
	};

	// Serialize a tag container to JSON array
	TSharedPtr<FJsonValue> ContainerToJson(const FGameplayTagContainer& Container)
	{
		return MonolithGAS::TagContainerToJson(Container);
	}

	constexpr int32 LegacyNonInstancedPolicyValue = 0;

	bool IsLegacyNonInstancedPolicy(EGameplayAbilityInstancingPolicy::Type Policy)
	{
		return static_cast<int32>(Policy) == LegacyNonInstancedPolicyValue;
	}

	// Parse enum string for instancing policy
	bool ParseInstancingPolicy(const FString& Str, EGameplayAbilityInstancingPolicy::Type& Out)
	{
		if (Str == TEXT("NonInstanced"))           { Out = static_cast<EGameplayAbilityInstancingPolicy::Type>(LegacyNonInstancedPolicyValue); return true; }
		if (Str == TEXT("InstancedPerActor"))       { Out = EGameplayAbilityInstancingPolicy::InstancedPerActor; return true; }
		if (Str == TEXT("InstancedPerExecution"))   { Out = EGameplayAbilityInstancingPolicy::InstancedPerExecution; return true; }
		return false;
	}

	// Parse enum string for net execution policy
	bool ParseNetExecutionPolicy(const FString& Str, EGameplayAbilityNetExecutionPolicy::Type& Out)
	{
		if (Str == TEXT("LocalPredicted"))  { Out = EGameplayAbilityNetExecutionPolicy::LocalPredicted; return true; }
		if (Str == TEXT("LocalOnly"))       { Out = EGameplayAbilityNetExecutionPolicy::LocalOnly; return true; }
		if (Str == TEXT("ServerInitiated")) { Out = EGameplayAbilityNetExecutionPolicy::ServerInitiated; return true; }
		if (Str == TEXT("ServerOnly"))      { Out = EGameplayAbilityNetExecutionPolicy::ServerOnly; return true; }
		return false;
	}

	// Parse enum string for net security policy
	bool ParseNetSecurityPolicy(const FString& Str, EGameplayAbilityNetSecurityPolicy::Type& Out)
	{
		if (Str == TEXT("ClientOrServer"))            { Out = EGameplayAbilityNetSecurityPolicy::ClientOrServer; return true; }
		if (Str == TEXT("ServerOnlyExecution"))       { Out = EGameplayAbilityNetSecurityPolicy::ServerOnlyExecution; return true; }
		if (Str == TEXT("ServerOnlyTermination"))     { Out = EGameplayAbilityNetSecurityPolicy::ServerOnlyTermination; return true; }
		if (Str == TEXT("ServerOnly"))                { Out = EGameplayAbilityNetSecurityPolicy::ServerOnly; return true; }
		return false;
	}

	// Enum to string helpers
	FString InstancingPolicyToString(EGameplayAbilityInstancingPolicy::Type P)
	{
		if (IsLegacyNonInstancedPolicy(P))
		{
			return TEXT("NonInstanced");
		}

		switch (P)
		{
		case EGameplayAbilityInstancingPolicy::InstancedPerActor:     return TEXT("InstancedPerActor");
		case EGameplayAbilityInstancingPolicy::InstancedPerExecution: return TEXT("InstancedPerExecution");
		default: return TEXT("Unknown");
		}
	}

	FString NetExecutionPolicyToString(EGameplayAbilityNetExecutionPolicy::Type P)
	{
		switch (P)
		{
		case EGameplayAbilityNetExecutionPolicy::LocalPredicted:  return TEXT("LocalPredicted");
		case EGameplayAbilityNetExecutionPolicy::LocalOnly:       return TEXT("LocalOnly");
		case EGameplayAbilityNetExecutionPolicy::ServerInitiated: return TEXT("ServerInitiated");
		case EGameplayAbilityNetExecutionPolicy::ServerOnly:      return TEXT("ServerOnly");
		default: return TEXT("Unknown");
		}
	}

	FString NetSecurityPolicyToString(EGameplayAbilityNetSecurityPolicy::Type P)
	{
		switch (P)
		{
		case EGameplayAbilityNetSecurityPolicy::ClientOrServer:        return TEXT("ClientOrServer");
		case EGameplayAbilityNetSecurityPolicy::ServerOnlyExecution:   return TEXT("ServerOnlyExecution");
		case EGameplayAbilityNetSecurityPolicy::ServerOnlyTermination: return TEXT("ServerOnlyTermination");
		case EGameplayAbilityNetSecurityPolicy::ServerOnly:            return TEXT("ServerOnly");
		default: return TEXT("Unknown");
		}
	}

	// Parse trigger source enum
	bool ParseTriggerSource(const FString& Str, EGameplayAbilityTriggerSource::Type& Out)
	{
		if (Str == TEXT("GameplayEvent"))     { Out = EGameplayAbilityTriggerSource::GameplayEvent; return true; }
		if (Str == TEXT("OwnedTagAdded"))     { Out = EGameplayAbilityTriggerSource::OwnedTagAdded; return true; }
		if (Str == TEXT("OwnedTagPresent"))   { Out = EGameplayAbilityTriggerSource::OwnedTagPresent; return true; }
		return false;
	}

	FString TriggerSourceToString(EGameplayAbilityTriggerSource::Type S)
	{
		switch (S)
		{
		case EGameplayAbilityTriggerSource::GameplayEvent:   return TEXT("GameplayEvent");
		case EGameplayAbilityTriggerSource::OwnedTagAdded:   return TEXT("OwnedTagAdded");
		case EGameplayAbilityTriggerSource::OwnedTagPresent: return TEXT("OwnedTagPresent");
		default: return TEXT("Unknown");
		}
	}
}

// ============================================================
//  Registration
// ============================================================

void FMonolithGASAbilityActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("gas"), TEXT("create_ability"),
		TEXT("Create a new GameplayAbility Blueprint asset"),
		FMonolithActionHandler::CreateStatic(&HandleCreateAbility),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path to save the new ability (e.g. /Game/Abilities/GA_MyAbility)"))
			.Optional(TEXT("parent_class"), TEXT("string"), TEXT("Parent class name (default: GameplayAbility)"), TEXT("GameplayAbility"))
			.Optional(TEXT("display_name"), TEXT("string"), TEXT("Display name for the ability"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("get_ability_info"),
		TEXT("Read all CDO properties for a GameplayAbility: tags, policies, cost, cooldown, triggers, flags"),
		FMonolithActionHandler::CreateStatic(&HandleGetAbilityInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayAbility Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("list_abilities"),
		TEXT("Find all GameplayAbility Blueprints in project with optional filters"),
		FMonolithActionHandler::CreateStatic(&HandleListAbilities),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Only include assets under this path prefix"))
			.Optional(TEXT("tag_filter"), TEXT("string"), TEXT("Only include abilities with this tag in AbilityTags"))
			.Optional(TEXT("parent_class_filter"), TEXT("string"), TEXT("Only include abilities inheriting from this class"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("compile_ability"),
		TEXT("Compile a GameplayAbility Blueprint and return errors/warnings"),
		FMonolithActionHandler::CreateStatic(&HandleCompileAbility),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayAbility Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("set_ability_tags"),
		TEXT("Set, add, or remove tags on any of the 10 tag containers on a GameplayAbility"),
		FMonolithActionHandler::CreateStatic(&HandleSetAbilityTags),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayAbility Blueprint asset path"))
			.Required(TEXT("container"), TEXT("string"), TEXT("Tag container name: AbilityTags, CancelAbilitiesWithTag, BlockAbilitiesWithTag, ActivationOwnedTags, ActivationRequiredTags, ActivationBlockedTags, SourceRequiredTags, SourceBlockedTags, TargetRequiredTags, TargetBlockedTags"))
			.Required(TEXT("tags"), TEXT("array"), TEXT("Array of gameplay tag strings"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("set (replace), add, or remove (default: set)"), TEXT("set"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("get_ability_tags"),
		TEXT("Read tag containers from a GameplayAbility. Returns one or all containers."),
		FMonolithActionHandler::CreateStatic(&HandleGetAbilityTags),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayAbility Blueprint asset path"))
			.Optional(TEXT("container"), TEXT("string"), TEXT("Specific container name to read (returns all if omitted)"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("set_ability_policy"),
		TEXT("Set instancing, net execution, and/or net security policies on a GameplayAbility"),
		FMonolithActionHandler::CreateStatic(&HandleSetAbilityPolicy),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayAbility Blueprint asset path"))
			.Optional(TEXT("instancing_policy"), TEXT("string"), TEXT("NonInstanced, InstancedPerActor, or InstancedPerExecution"))
			.Optional(TEXT("net_execution_policy"), TEXT("string"), TEXT("LocalPredicted, LocalOnly, ServerInitiated, or ServerOnly"))
			.Optional(TEXT("net_security_policy"), TEXT("string"), TEXT("ClientOrServer, ServerOnlyExecution, ServerOnlyTermination, or ServerOnly"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("set_ability_cost"),
		TEXT("Set the CostGameplayEffectClass on a GameplayAbility"),
		FMonolithActionHandler::CreateStatic(&HandleSetAbilityCost),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayAbility Blueprint asset path"))
			.Required(TEXT("cost_effect_class"), TEXT("string"), TEXT("GameplayEffect class/asset path for the cost (or empty to clear)"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("set_ability_cooldown"),
		TEXT("Set the CooldownGameplayEffectClass on a GameplayAbility"),
		FMonolithActionHandler::CreateStatic(&HandleSetAbilityCooldown),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayAbility Blueprint asset path"))
			.Required(TEXT("cooldown_effect_class"), TEXT("string"), TEXT("GameplayEffect class/asset path for the cooldown (or empty to clear)"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("set_ability_triggers"),
		TEXT("Replace the AbilityTriggers array on a GameplayAbility"),
		FMonolithActionHandler::CreateStatic(&HandleSetAbilityTriggers),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayAbility Blueprint asset path"))
			.Required(TEXT("triggers"), TEXT("array"), TEXT("Array of {tag, trigger_source} objects. trigger_source: GameplayEvent, OwnedTagAdded, OwnedTagPresent"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("set_ability_flags"),
		TEXT("Set boolean behavior flags on a GameplayAbility"),
		FMonolithActionHandler::CreateStatic(&HandleSetAbilityFlags),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayAbility Blueprint asset path"))
			.Optional(TEXT("replicate_input_directly"), TEXT("boolean"), TEXT("bReplicateInputDirectly"))
			.Optional(TEXT("retrigger_instanced_ability"), TEXT("boolean"), TEXT("bRetriggerInstancedAbility"))
			.Optional(TEXT("server_respects_remote_ability_cancellation"), TEXT("boolean"), TEXT("bServerRespectsRemoteAbilityCancellation"))
			.Build());

	// ---- Phase 2: Graph Building ----

	Registry.RegisterAction(TEXT("gas"), TEXT("add_ability_task_node"),
		TEXT("Place a UK2Node_LatentAbilityCall in an ability Blueprint graph"),
		FMonolithActionHandler::CreateStatic(&HandleAddAbilityTaskNode),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayAbility Blueprint asset path"))
			.Required(TEXT("task_class"), TEXT("string"), TEXT("UAbilityTask subclass name (e.g. UAbilityTask_WaitGameplayEvent)"))
			.Optional(TEXT("factory_function"), TEXT("string"), TEXT("Static factory function name (auto-detected if omitted)"))
			.Optional(TEXT("position"), TEXT("object"), TEXT("{ x, y } node position"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("add_commit_and_end_flow"),
		TEXT("Scaffold CommitAbility -> Branch -> EndAbility boilerplate in an ability graph"),
		FMonolithActionHandler::CreateStatic(&HandleAddCommitAndEndFlow),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayAbility Blueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (default: EventGraph)"))
			.Optional(TEXT("position"), TEXT("object"), TEXT("{ x, y } start position for the flow"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("add_effect_application"),
		TEXT("Add ApplyGameplayEffectToOwner or ApplyGameplayEffectToTarget node pre-wired"),
		FMonolithActionHandler::CreateStatic(&HandleAddEffectApplication),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayAbility Blueprint asset path"))
			.Required(TEXT("effect_class"), TEXT("string"), TEXT("GameplayEffect class or asset path"))
			.Optional(TEXT("target"), TEXT("string"), TEXT("'self' or 'target' (default: self)"), TEXT("self"))
			.Optional(TEXT("position"), TEXT("object"), TEXT("{ x, y } node position"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("add_gameplay_cue_node"),
		TEXT("Add a GameplayCue Execute/Add/Remove invocation node"),
		FMonolithActionHandler::CreateStatic(&HandleAddGameplayCueNode),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayAbility Blueprint asset path"))
			.Required(TEXT("cue_tag"), TEXT("string"), TEXT("GameplayCue tag (e.g. GameplayCue.Combat.Hit)"))
			.Required(TEXT("type"), TEXT("string"), TEXT("'execute', 'add', or 'remove'"))
			.Optional(TEXT("position"), TEXT("object"), TEXT("{ x, y } node position"))
			.Build());

	// ---- Phase 2: Productivity ----

	Registry.RegisterAction(TEXT("gas"), TEXT("create_ability_from_template"),
		TEXT("Create a fully-configured ability from a named template (ability_instant, ability_montage, etc.)"),
		FMonolithActionHandler::CreateStatic(&HandleCreateAbilityFromTemplate),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path to save the new ability"))
			.Required(TEXT("template"), TEXT("string"), TEXT("Template name: ability_instant, ability_montage, ability_channeled, ability_passive, ability_toggle, ability_heal_self"))
			.Optional(TEXT("overrides"), TEXT("object"), TEXT("Override template defaults: { instancing_policy, net_execution_policy, ability_tags[], ... }"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("build_ability_from_spec"),
		TEXT("Declarative one-shot: create an ability from a full spec defining tags, policies, and graph flow"),
		FMonolithActionHandler::CreateStatic(&HandleBuildAbilityFromSpec),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path to save the new ability"))
			.Required(TEXT("spec"), TEXT("object"), TEXT("Full ability spec: { parent_class?, display_name?, instancing_policy?, net_execution_policy?, net_security_policy?, tags: { AbilityTags:[], ... }, cost_effect_class?, cooldown_effect_class?, triggers:[], flags:{} }"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("batch_create_abilities"),
		TEXT("Create multiple abilities in one call"),
		FMonolithActionHandler::CreateStatic(&HandleBatchCreateAbilities),
		FParamSchemaBuilder()
			.Required(TEXT("abilities"), TEXT("array"), TEXT("Array of ability specs, each with save_path + any create_ability/build_ability_from_spec params"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("duplicate_ability"),
		TEXT("Deep-copy a GameplayAbility Blueprint with optional tag renaming"),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateAbility),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Source GameplayAbility Blueprint asset path"))
			.Required(TEXT("new_path"), TEXT("string"), TEXT("Destination asset path for the copy"))
			.Optional(TEXT("rename_tags"), TEXT("object"), TEXT("Tag rename map: { \"old.tag\": \"new.tag\", ... }"))
			.Build());

	// ---- Phase 2: Task Analysis ----

	Registry.RegisterAction(TEXT("gas"), TEXT("list_ability_tasks"),
		TEXT("Enumerate all loaded UAbilityTask subclasses with pin schemas"),
		FMonolithActionHandler::CreateStatic(&HandleListAbilityTasks),
		FParamSchemaBuilder()
			.Optional(TEXT("category_filter"), TEXT("string"), TEXT("Filter by category substring (e.g. 'Wait', 'Play', 'SpawnActor')"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("get_ability_task_pins"),
		TEXT("Get full pin schema for a specific ability task class: factory params, output delegates, exec pins"),
		FMonolithActionHandler::CreateStatic(&HandleGetAbilityTaskPins),
		FParamSchemaBuilder()
			.Required(TEXT("task_class"), TEXT("string"), TEXT("UAbilityTask subclass name (e.g. UAbilityTask_WaitGameplayEvent)"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("wire_ability_task_delegate"),
		TEXT("Connect a task delegate output pin to a target exec pin"),
		FMonolithActionHandler::CreateStatic(&HandleWireAbilityTaskDelegate),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayAbility Blueprint asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Task node ID (from add_ability_task_node response)"))
			.Required(TEXT("delegate_name"), TEXT("string"), TEXT("Delegate output pin name (e.g. OnCompleted, OnCancelled)"))
			.Required(TEXT("target_node_id"), TEXT("string"), TEXT("Target node ID to wire the delegate exec to"))
			.Optional(TEXT("target_pin"), TEXT("string"), TEXT("Target exec input pin name (default: execute)"), TEXT("execute"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("get_ability_graph_flow"),
		TEXT("Analyze ability graph: detect pattern type, missing EndAbility/CommitAbility, dangling delegates"),
		FMonolithActionHandler::CreateStatic(&HandleGetAbilityGraphFlow),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayAbility Blueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph to analyze (default: EventGraph)"))
			.Build());

	// ---- Phase 3: Validation & Analysis ----

	Registry.RegisterAction(TEXT("gas"), TEXT("validate_ability"),
		TEXT("Deep validation: CommitAbility reachable, EndAbility reachable, tag conflicts, missing cost/cooldown refs, contradictory tags, NonInstanced with state"),
		FMonolithActionHandler::CreateStatic(&HandleValidateAbility),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GameplayAbility Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("find_abilities_by_tag"),
		TEXT("Find abilities that have/cancel/block/require a specific tag"),
		FMonolithActionHandler::CreateStatic(&HandleFindAbilitiesByTag),
		FParamSchemaBuilder()
			.Required(TEXT("tag"), TEXT("string"), TEXT("Gameplay tag to search for"))
			.Optional(TEXT("match_type"), TEXT("string"), TEXT("'exact' or 'partial' (default: exact)"), TEXT("exact"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("get_ability_tag_matrix"),
		TEXT("Build cancel/block relationship map between abilities"),
		FMonolithActionHandler::CreateStatic(&HandleGetAbilityTagMatrix),
		FParamSchemaBuilder()
			.Optional(TEXT("asset_paths"), TEXT("array"), TEXT("Array of ability asset paths (default: all project abilities)"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("validate_ability_blueprint"),
		TEXT("Check for unhandled delegates, missing EndAbility, tasks in wrong BP type, NonInstanced misuse"),
		FMonolithActionHandler::CreateStatic(&HandleValidateAbilityBlueprint),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path to validate"))
			.Build());

	// Phase 4: Advanced
	Registry.RegisterAction(TEXT("gas"), TEXT("scaffold_custom_ability_task"),
		TEXT("Generate C++ header and implementation files for a custom UAbilityTask subclass with delegates and factory function"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldCustomAbilityTask),
		FParamSchemaBuilder()
			.Required(TEXT("class_name"), TEXT("string"), TEXT("Name for the task class (without U prefix, added automatically)"))
			.Required(TEXT("parameters"), TEXT("array"), TEXT("Array of {name, type} objects for factory function parameters"))
			.Required(TEXT("delegates"), TEXT("array"), TEXT("Array of {name, params?} objects for broadcast delegates"))
			.Build());
}

// ============================================================
//  create_ability
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleCreateAbility(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("save_path"), SavePath, Err))
	{
		return Err;
	}

	// Resolve parent class
	FString ParentClassName = Params->GetStringField(TEXT("parent_class"));
	if (ParentClassName.IsEmpty())
	{
		ParentClassName = TEXT("GameplayAbility");
	}

	UClass* ParentClass = nullptr;

	// Try finding the class by name — check for asset path (Blueprint parent) first
	if (ParentClassName.StartsWith(TEXT("/")))
	{
		// It's an asset path — load the Blueprint and use its generated class
		UObject* Obj = FMonolithAssetUtils::LoadAssetByPath(UBlueprint::StaticClass(), ParentClassName);
		UBlueprint* ParentBP = Cast<UBlueprint>(Obj);
		if (ParentBP && ParentBP->GeneratedClass)
		{
			ParentClass = ParentBP->GeneratedClass;
		}
	}

	if (!ParentClass)
	{
		// Try native class lookup — strip leading "U" if present for FindFirstObject
		FString ClassName = ParentClassName;
		if (!ClassName.StartsWith(TEXT("U")))
		{
			ClassName = TEXT("U") + ClassName;
		}
		ParentClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
		if (!ParentClass)
		{
			// Try without the U prefix
			ParentClass = FindFirstObject<UClass>(*ParentClassName, EFindFirstObjectOptions::NativeFirst);
		}
	}

	if (!ParentClass)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Parent class not found: %s"), *ParentClassName));
	}

	if (!ParentClass->IsChildOf(UGameplayAbility::StaticClass()))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Parent class '%s' is not a GameplayAbility subclass"), *ParentClassName));
	}

	// Extract asset name from the save path
	FString AssetName = FPackageName::GetLongPackageAssetName(SavePath);
	if (AssetName.IsEmpty())
	{
		// Fallback: use the last segment of the path
		int32 LastSlash;
		if (SavePath.FindLastChar(TEXT('/'), LastSlash))
		{
			AssetName = SavePath.Mid(LastSlash + 1);
		}
		else
		{
			AssetName = SavePath;
		}
	}

	// Check if asset already exists (AssetRegistry + in-memory multi-tier check)
	FString ExistError;
	if (!MonolithGAS::EnsureAssetPathFree(SavePath, AssetName, ExistError))
	{
		return FMonolithActionResult::Error(ExistError);
	}

	// Create package
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to create package at: %s"), *SavePath));
	}
	Package->FullyLoad();

	// Create the Blueprint
	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());

	if (!NewBP)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to create ability Blueprint at: %s"), *SavePath));
	}

	// Set display name if provided
	FString DisplayName = Params->GetStringField(TEXT("display_name"));
	if (!DisplayName.IsEmpty())
	{
		NewBP->BlueprintDisplayName = DisplayName;
	}

	// Compile to ensure GeneratedClass is valid
	FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::SkipGarbageCollection);

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewBP);

	// Save to disk
	FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	bool bSaved = UPackage::SavePackage(Package, NewBP, *PackageFilename, SaveArgs);

	// Build result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	Result->SetBoolField(TEXT("saved"), bSaved);

	if (NewBP->GeneratedClass)
	{
		Result->SetStringField(TEXT("generated_class"), NewBP->GeneratedClass->GetPathName());
	}

	if (!DisplayName.IsEmpty())
	{
		Result->SetStringField(TEXT("display_name"), DisplayName);
	}

	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Created GameplayAbility '%s' with parent '%s'"), *AssetName, *ParentClass->GetName()));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  get_ability_info
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleGetAbilityInfo(const TSharedPtr<FJsonObject>& Params)
{
	FAbilityCDOContext Ctx;
	FMonolithActionResult Err;
	if (!Ctx.Load(Params, Err))
	{
		return Err;
	}

	UGameplayAbility* CDO = Ctx.CDO;
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Ctx.AssetPath);
	Result->SetStringField(TEXT("class_name"), Ctx.BP->GeneratedClass->GetName());
	Result->SetStringField(TEXT("parent_class"), Ctx.BP->ParentClass ? Ctx.BP->ParentClass->GetName() : TEXT("None"));

	// Policies
	TSharedPtr<FJsonObject> Policies = MakeShared<FJsonObject>();
	Policies->SetStringField(TEXT("instancing_policy"), InstancingPolicyToString(CDO->GetInstancingPolicy()));
	Policies->SetStringField(TEXT("net_execution_policy"), NetExecutionPolicyToString(CDO->GetNetExecutionPolicy()));
	Policies->SetStringField(TEXT("net_security_policy"), NetSecurityPolicyToString(CDO->GetNetSecurityPolicy()));
	Result->SetObjectField(TEXT("policies"), Policies);

	// Tag containers
	TSharedPtr<FJsonObject> Tags = MakeShared<FJsonObject>();
	for (const FString& ContainerName : AllContainerNames)
	{
		FGameplayTagContainer* Container = GetTagContainerByName(CDO, ContainerName);
		if (Container)
		{
			Tags->SetField(ContainerName, ContainerToJson(*Container));
		}
	}
	Result->SetObjectField(TEXT("tags"), Tags);

	// Cost
	if (CDO->GetCostGameplayEffect())
	{
		Result->SetStringField(TEXT("cost_effect_class"), CDO->GetCostGameplayEffect()->GetPathName());
	}
	else
	{
		Result->SetStringField(TEXT("cost_effect_class"), TEXT(""));
	}

	// Cooldown
	if (CDO->GetCooldownGameplayEffect())
	{
		Result->SetStringField(TEXT("cooldown_effect_class"), CDO->GetCooldownGameplayEffect()->GetPathName());
	}
	else
	{
		Result->SetStringField(TEXT("cooldown_effect_class"), TEXT(""));
	}

	// Triggers
	TArray<TSharedPtr<FJsonValue>> TriggersArray;
	TArray<FAbilityTriggerData>* Triggers = GetPropertyPtrByName<TArray<FAbilityTriggerData>>(CDO, FName(TEXT("AbilityTriggers")));
	for (const FAbilityTriggerData& Trigger : (Triggers ? *Triggers : TArray<FAbilityTriggerData>()))
	{
		TSharedPtr<FJsonObject> TrigObj = MakeShared<FJsonObject>();
		TrigObj->SetStringField(TEXT("tag"), Trigger.TriggerTag.ToString());
		TrigObj->SetStringField(TEXT("trigger_source"), TriggerSourceToString(Trigger.TriggerSource));
		TriggersArray.Add(MakeShared<FJsonValueObject>(TrigObj));
	}
	Result->SetArrayField(TEXT("triggers"), TriggersArray);

	// Flags
	TSharedPtr<FJsonObject> Flags = MakeShared<FJsonObject>();
	Flags->SetBoolField(TEXT("replicate_input_directly"), GetBoolProperty(CDO, FName(TEXT("bReplicateInputDirectly"))));
	Flags->SetBoolField(TEXT("retrigger_instanced_ability"), GetBoolProperty(CDO, FName(TEXT("bRetriggerInstancedAbility"))));
	Flags->SetBoolField(TEXT("server_respects_remote_ability_cancellation"), GetBoolProperty(CDO, FName(TEXT("bServerRespectsRemoteAbilityCancellation"))));
	Result->SetObjectField(TEXT("flags"), Flags);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  list_abilities
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleListAbilities(const TSharedPtr<FJsonObject>& Params)
{
	FString PathFilter = Params->GetStringField(TEXT("path_filter"));
	FString TagFilter = Params->GetStringField(TEXT("tag_filter"));
	FString ParentClassFilter = Params->GetStringField(TEXT("parent_class_filter"));

	// Use asset registry to find all Blueprint assets
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	if (!PathFilter.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PathFilter));
		Filter.bRecursivePaths = true;
	}

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	// Resolve parent class filter if specified
	UClass* ParentFilterClass = nullptr;
	if (!ParentClassFilter.IsEmpty())
	{
		FString ClassName = ParentClassFilter;
		if (!ClassName.StartsWith(TEXT("U")))
		{
			ClassName = TEXT("U") + ClassName;
		}
		ParentFilterClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	}

	// Parse tag filter
	FGameplayTag FilterTag;
	if (!TagFilter.IsEmpty())
	{
		FilterTag = MonolithGAS::StringToTag(TagFilter);
	}

	TArray<TSharedPtr<FJsonValue>> AbilityList;

	for (const FAssetData& AssetData : Assets)
	{
		// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
		FAssetTagValueRef ParentTag = AssetData.TagsAndValues.FindTag(FName("ParentClass"));
		FAssetTagValueRef NativeParentTag = AssetData.TagsAndValues.FindTag(FName("NativeParentClass"));
		FString ParentPath = ParentTag.IsSet() ? ParentTag.GetValue() : (NativeParentTag.IsSet() ? NativeParentTag.GetValue() : TEXT(""));
		if (!ParentPath.Contains(TEXT("GameplayAbility")))
		{
			continue;
		}

		UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
		if (!BP || !BP->GeneratedClass)
		{
			continue;
		}

		// Must be a GameplayAbility subclass
		if (!BP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
		{
			continue;
		}

		// Parent class filter
		if (ParentFilterClass && !BP->GeneratedClass->IsChildOf(ParentFilterClass))
		{
			continue;
		}

		// Tag filter
		if (FilterTag.IsValid())
		{
			UGameplayAbility* CDO = MonolithGAS::GetBlueprintCDO<UGameplayAbility>(BP);
			FGameplayTagContainer* AbilityTagsPtr = CDO ? GetPropertyPtrByName<FGameplayTagContainer>(CDO, FName(TEXT("AbilityTags"))) : nullptr;
			if (!AbilityTagsPtr || !AbilityTagsPtr->HasTag(FilterTag))
			{
				continue;
			}
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		Entry->SetStringField(TEXT("class_name"), BP->GeneratedClass->GetName());
		Entry->SetStringField(TEXT("parent_class"), BP->ParentClass ? BP->ParentClass->GetName() : TEXT("None"));

		// Include ability tags for quick scanning
		UGameplayAbility* CDO = MonolithGAS::GetBlueprintCDO<UGameplayAbility>(BP);
		if (CDO)
		{
			FGameplayTagContainer* TagsPtr = GetPropertyPtrByName<FGameplayTagContainer>(CDO, FName(TEXT("AbilityTags")));
			if (TagsPtr) Entry->SetField(TEXT("ability_tags"), ContainerToJson(*TagsPtr));
			Entry->SetStringField(TEXT("instancing_policy"), InstancingPolicyToString(CDO->GetInstancingPolicy()));
		}

		AbilityList.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("abilities"), AbilityList);
	Result->SetNumberField(TEXT("count"), AbilityList.Num());

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  compile_ability
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleCompileAbility(const TSharedPtr<FJsonObject>& Params)
{
	FAbilityCDOContext Ctx;
	FMonolithActionResult Err;
	if (!Ctx.Load(Params, Err))
	{
		return Err;
	}

	FCompilerResultsLog Results;
	FKismetEditorUtilities::CompileBlueprint(Ctx.BP, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

	// Collect errors/warnings from nodes
	TArray<TSharedPtr<FJsonValue>> Errors;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	{
		TArray<UEdGraph*> AllGraphs;
		Ctx.BP->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || !Node->bHasCompilerMessage) continue;
				if (Node->ErrorMsg.IsEmpty()) continue;

				TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
				MsgObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
				MsgObj->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
				MsgObj->SetStringField(TEXT("graph"), Graph->GetName());
				MsgObj->SetStringField(TEXT("message"), Node->ErrorMsg);

				if (Node->ErrorType == EMessageSeverity::Error)
				{
					Errors.Add(MakeShared<FJsonValueObject>(MsgObj));
				}
				else
				{
					Warnings.Add(MakeShared<FJsonValueObject>(MsgObj));
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Ctx.AssetPath);

	bool bHasErrors = Errors.Num() > 0;
	Result->SetStringField(TEXT("status"), bHasErrors ? TEXT("error") : TEXT("success"));
	Result->SetArrayField(TEXT("errors"), Errors);
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetNumberField(TEXT("error_count"), Errors.Num());
	Result->SetNumberField(TEXT("warning_count"), Warnings.Num());

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  set_ability_tags
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleSetAbilityTags(const TSharedPtr<FJsonObject>& Params)
{
	FAbilityCDOContext Ctx;
	FMonolithActionResult Err;
	if (!Ctx.Load(Params, Err))
	{
		return Err;
	}

	FString ContainerName;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("container"), ContainerName, Err))
	{
		return Err;
	}

	FGameplayTagContainer* Container = GetTagContainerByName(Ctx.CDO, ContainerName);
	if (!Container)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown tag container: '%s'. Valid containers: %s"),
				*ContainerName, *FString::Join(AllContainerNames, TEXT(", "))));
	}

	// Parse tags from the array param. F.7b — collect dropped tag strings to surface as warnings on the response.
	TArray<FString> SkippedTags;
	FGameplayTagContainer InputTags = MonolithGAS::ParseTagContainer(Params, TEXT("tags"), SkippedTags);

	// Determine mode
	FString Mode = Params->GetStringField(TEXT("mode"));
	if (Mode.IsEmpty())
	{
		Mode = TEXT("set");
	}

	if (Mode == TEXT("set"))
	{
		*Container = InputTags;
	}
	else if (Mode == TEXT("add"))
	{
		Container->AppendTags(InputTags);
	}
	else if (Mode == TEXT("remove"))
	{
		Container->RemoveTags(InputTags);
	}
	else
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid mode '%s'. Use 'set', 'add', or 'remove'."), *Mode));
	}

	Ctx.MarkModified();

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(Ctx.AssetPath,
		FString::Printf(TEXT("Tags %s on container '%s'"),
			Mode == TEXT("set") ? TEXT("set") : (Mode == TEXT("add") ? TEXT("added") : TEXT("removed")),
			*ContainerName));
	Result->SetField(TEXT("current_tags"), ContainerToJson(*Container));

	// F.7b — surface dropped tag strings as a "warnings" array on the response.
	if (SkippedTags.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Warnings;
		for (const FString& T : SkippedTags)
		{
			Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("tags '%s' is not a registered GameplayTag — dropped"), *T)));
		}
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  get_ability_tags
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleGetAbilityTags(const TSharedPtr<FJsonObject>& Params)
{
	FAbilityCDOContext Ctx;
	FMonolithActionResult Err;
	if (!Ctx.Load(Params, Err))
	{
		return Err;
	}

	FString ContainerName = Params->GetStringField(TEXT("container"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Ctx.AssetPath);

	if (!ContainerName.IsEmpty())
	{
		// Return single container
		FGameplayTagContainer* Container = GetTagContainerByName(Ctx.CDO, ContainerName);
		if (!Container)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Unknown tag container: '%s'. Valid containers: %s"),
					*ContainerName, *FString::Join(AllContainerNames, TEXT(", "))));
		}
		Result->SetField(ContainerName, ContainerToJson(*Container));
	}
	else
	{
		// Return all containers
		TSharedPtr<FJsonObject> Tags = MakeShared<FJsonObject>();
		for (const FString& Name : AllContainerNames)
		{
			FGameplayTagContainer* Container = GetTagContainerByName(Ctx.CDO, Name);
			if (Container)
			{
				Tags->SetField(Name, ContainerToJson(*Container));
			}
		}
		Result->SetObjectField(TEXT("tags"), Tags);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  set_ability_policy
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleSetAbilityPolicy(const TSharedPtr<FJsonObject>& Params)
{
	FAbilityCDOContext Ctx;
	FMonolithActionResult Err;
	if (!Ctx.Load(Params, Err))
	{
		return Err;
	}

	bool bChanged = false;

	// Instancing policy
	if (Params->HasField(TEXT("instancing_policy")))
	{
		FString Val = Params->GetStringField(TEXT("instancing_policy"));
		EGameplayAbilityInstancingPolicy::Type Policy;
		if (!ParseInstancingPolicy(Val, Policy))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Invalid instancing_policy: '%s'. Valid: NonInstanced, InstancedPerActor, InstancedPerExecution"), *Val));
		}
		SetPropertyByName<uint8>(Ctx.CDO, FName(TEXT("InstancingPolicy")), static_cast<uint8>(Policy));
		bChanged = true;
	}

	// Net execution policy
	if (Params->HasField(TEXT("net_execution_policy")))
	{
		FString Val = Params->GetStringField(TEXT("net_execution_policy"));
		EGameplayAbilityNetExecutionPolicy::Type Policy;
		if (!ParseNetExecutionPolicy(Val, Policy))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Invalid net_execution_policy: '%s'. Valid: LocalPredicted, LocalOnly, ServerInitiated, ServerOnly"), *Val));
		}
		SetPropertyByName<uint8>(Ctx.CDO, FName(TEXT("NetExecutionPolicy")), static_cast<uint8>(Policy));
		bChanged = true;
	}

	// Net security policy
	if (Params->HasField(TEXT("net_security_policy")))
	{
		FString Val = Params->GetStringField(TEXT("net_security_policy"));
		EGameplayAbilityNetSecurityPolicy::Type Policy;
		if (!ParseNetSecurityPolicy(Val, Policy))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Invalid net_security_policy: '%s'. Valid: ClientOrServer, ServerOnlyExecution, ServerOnlyTermination, ServerOnly"), *Val));
		}
		SetPropertyByName<uint8>(Ctx.CDO, FName(TEXT("NetSecurityPolicy")), static_cast<uint8>(Policy));
		bChanged = true;
	}

	if (!bChanged)
	{
		return FMonolithActionResult::Error(TEXT("No policy fields provided. Set at least one of: instancing_policy, net_execution_policy, net_security_policy"));
	}

	Ctx.MarkModified();

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(Ctx.AssetPath, TEXT("Policies updated"));
	TSharedPtr<FJsonObject> Policies = MakeShared<FJsonObject>();
	Policies->SetStringField(TEXT("instancing_policy"), InstancingPolicyToString(Ctx.CDO->GetInstancingPolicy()));
	Policies->SetStringField(TEXT("net_execution_policy"), NetExecutionPolicyToString(Ctx.CDO->GetNetExecutionPolicy()));
	Policies->SetStringField(TEXT("net_security_policy"), NetSecurityPolicyToString(Ctx.CDO->GetNetSecurityPolicy()));
	Result->SetObjectField(TEXT("policies"), Policies);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  set_ability_cost
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleSetAbilityCost(const TSharedPtr<FJsonObject>& Params)
{
	FAbilityCDOContext Ctx;
	FMonolithActionResult Err;
	if (!Ctx.Load(Params, Err))
	{
		return Err;
	}

	FString CostClassPath;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("cost_effect_class"), CostClassPath, Err))
	{
		return Err;
	}

	if (CostClassPath.IsEmpty())
	{
		// Clear the cost
		SetPropertyByName(Ctx.CDO, FName(TEXT("CostGameplayEffectClass")), TSubclassOf<UGameplayEffect>(nullptr));
		Ctx.MarkModified();

		TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(Ctx.AssetPath, TEXT("Cost effect cleared"));
		return FMonolithActionResult::Success(Result);
	}

	// Load the GE class — it could be a Blueprint or a native class
	UClass* GEClass = nullptr;

	// Try as Blueprint asset path first
	if (CostClassPath.StartsWith(TEXT("/")))
	{
		UObject* Obj = FMonolithAssetUtils::LoadAssetByPath(UBlueprint::StaticClass(), CostClassPath);
		UBlueprint* GEBP = Cast<UBlueprint>(Obj);
		if (GEBP && GEBP->GeneratedClass)
		{
			GEClass = GEBP->GeneratedClass;
		}

		// Also try loading as the generated class directly (path ending in _C)
		if (!GEClass)
		{
			FString ClassPath = CostClassPath;
			if (!ClassPath.EndsWith(TEXT("_C")))
			{
				ClassPath += TEXT("_C");
			}
			GEClass = LoadClass<UGameplayEffect>(nullptr, *ClassPath);
		}
	}

	// Try as native class name
	if (!GEClass)
	{
		FString ClassName = CostClassPath;
		if (!ClassName.StartsWith(TEXT("U")))
		{
			ClassName = TEXT("U") + ClassName;
		}
		GEClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	}

	if (!GEClass || !GEClass->IsChildOf(UGameplayEffect::StaticClass()))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("'%s' is not a valid GameplayEffect class"), *CostClassPath));
	}

	SetPropertyByName(Ctx.CDO, FName(TEXT("CostGameplayEffectClass")), TSubclassOf<UGameplayEffect>(GEClass));
	Ctx.MarkModified();

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(Ctx.AssetPath,
		FString::Printf(TEXT("Cost effect set to '%s'"), *GEClass->GetName()));
	Result->SetStringField(TEXT("cost_effect_class"), GEClass->GetPathName());

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  set_ability_cooldown
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleSetAbilityCooldown(const TSharedPtr<FJsonObject>& Params)
{
	FAbilityCDOContext Ctx;
	FMonolithActionResult Err;
	if (!Ctx.Load(Params, Err))
	{
		return Err;
	}

	FString CooldownClassPath;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("cooldown_effect_class"), CooldownClassPath, Err))
	{
		return Err;
	}

	if (CooldownClassPath.IsEmpty())
	{
		// Clear the cooldown
		SetPropertyByName(Ctx.CDO, FName(TEXT("CooldownGameplayEffectClass")), TSubclassOf<UGameplayEffect>(nullptr));
		Ctx.MarkModified();

		TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(Ctx.AssetPath, TEXT("Cooldown effect cleared"));
		return FMonolithActionResult::Success(Result);
	}

	// Load the GE class — same logic as cost
	UClass* GEClass = nullptr;

	if (CooldownClassPath.StartsWith(TEXT("/")))
	{
		UObject* Obj = FMonolithAssetUtils::LoadAssetByPath(UBlueprint::StaticClass(), CooldownClassPath);
		UBlueprint* GEBP = Cast<UBlueprint>(Obj);
		if (GEBP && GEBP->GeneratedClass)
		{
			GEClass = GEBP->GeneratedClass;
		}

		if (!GEClass)
		{
			FString ClassPath = CooldownClassPath;
			if (!ClassPath.EndsWith(TEXT("_C")))
			{
				ClassPath += TEXT("_C");
			}
			GEClass = LoadClass<UGameplayEffect>(nullptr, *ClassPath);
		}
	}

	if (!GEClass)
	{
		FString ClassName = CooldownClassPath;
		if (!ClassName.StartsWith(TEXT("U")))
		{
			ClassName = TEXT("U") + ClassName;
		}
		GEClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	}

	if (!GEClass || !GEClass->IsChildOf(UGameplayEffect::StaticClass()))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("'%s' is not a valid GameplayEffect class"), *CooldownClassPath));
	}

	SetPropertyByName(Ctx.CDO, FName(TEXT("CooldownGameplayEffectClass")), TSubclassOf<UGameplayEffect>(GEClass));
	Ctx.MarkModified();

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(Ctx.AssetPath,
		FString::Printf(TEXT("Cooldown effect set to '%s'"), *GEClass->GetName()));
	Result->SetStringField(TEXT("cooldown_effect_class"), GEClass->GetPathName());

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  set_ability_triggers
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleSetAbilityTriggers(const TSharedPtr<FJsonObject>& Params)
{
	FAbilityCDOContext Ctx;
	FMonolithActionResult Err;
	if (!Ctx.Load(Params, Err))
	{
		return Err;
	}

	const TArray<TSharedPtr<FJsonValue>>* TriggersArray;
	if (!Params->TryGetArrayField(TEXT("triggers"), TriggersArray))
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: triggers (array of {tag, trigger_source})"));
	}

	TArray<FAbilityTriggerData> NewTriggers;

	for (int32 i = 0; i < TriggersArray->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>* TrigObj;
		if (!(*TriggersArray)[i]->TryGetObject(TrigObj))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("triggers[%d] is not a valid object"), i));
		}

		FString TagStr = (*TrigObj)->GetStringField(TEXT("tag"));
		FString SourceStr = (*TrigObj)->GetStringField(TEXT("trigger_source"));

		if (TagStr.IsEmpty())
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("triggers[%d].tag is missing or empty"), i));
		}

		FGameplayTag Tag = MonolithGAS::StringToTag(TagStr);
		if (!Tag.IsValid())
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("triggers[%d].tag '%s' is not a valid gameplay tag"), i, *TagStr));
		}

		EGameplayAbilityTriggerSource::Type Source = EGameplayAbilityTriggerSource::GameplayEvent;
		if (!SourceStr.IsEmpty())
		{
			if (!ParseTriggerSource(SourceStr, Source))
			{
				return FMonolithActionResult::Error(
					FString::Printf(TEXT("triggers[%d].trigger_source '%s' is invalid. Valid: GameplayEvent, OwnedTagAdded, OwnedTagPresent"), i, *SourceStr));
			}
		}

		FAbilityTriggerData TriggerData;
		TriggerData.TriggerTag = Tag;
		TriggerData.TriggerSource = Source;
		NewTriggers.Add(TriggerData);
	}

	SetPropertyByName(Ctx.CDO, FName(TEXT("AbilityTriggers")), NewTriggers);
	Ctx.MarkModified();

	// Build result with the new triggers
	TArray<TSharedPtr<FJsonValue>> ResultTriggers;
	for (const FAbilityTriggerData& T : NewTriggers)
	{
		TSharedPtr<FJsonObject> TObj = MakeShared<FJsonObject>();
		TObj->SetStringField(TEXT("tag"), T.TriggerTag.ToString());
		TObj->SetStringField(TEXT("trigger_source"), TriggerSourceToString(T.TriggerSource));
		ResultTriggers.Add(MakeShared<FJsonValueObject>(TObj));
	}

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(Ctx.AssetPath,
		FString::Printf(TEXT("Set %d trigger(s)"), NewTriggers.Num()));
	Result->SetArrayField(TEXT("triggers"), ResultTriggers);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  set_ability_flags
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleSetAbilityFlags(const TSharedPtr<FJsonObject>& Params)
{
	FAbilityCDOContext Ctx;
	FMonolithActionResult Err;
	if (!Ctx.Load(Params, Err))
	{
		return Err;
	}

	bool bChanged = false;

	if (Params->HasField(TEXT("replicate_input_directly")))
	{
		SetBoolProperty(Ctx.CDO, FName(TEXT("bReplicateInputDirectly")), Params->GetBoolField(TEXT("replicate_input_directly")));
		bChanged = true;
	}

	if (Params->HasField(TEXT("retrigger_instanced_ability")))
	{
		SetBoolProperty(Ctx.CDO, FName(TEXT("bRetriggerInstancedAbility")), Params->GetBoolField(TEXT("retrigger_instanced_ability")));
		bChanged = true;
	}

	if (Params->HasField(TEXT("server_respects_remote_ability_cancellation")))
	{
		SetBoolProperty(Ctx.CDO, FName(TEXT("bServerRespectsRemoteAbilityCancellation")), Params->GetBoolField(TEXT("server_respects_remote_ability_cancellation")));
		bChanged = true;
	}

	if (!bChanged)
	{
		return FMonolithActionResult::Error(TEXT("No flag fields provided. Set at least one of: replicate_input_directly, retrigger_instanced_ability, server_respects_remote_ability_cancellation"));
	}

	Ctx.MarkModified();

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(Ctx.AssetPath, TEXT("Flags updated"));
	TSharedPtr<FJsonObject> Flags = MakeShared<FJsonObject>();
	Flags->SetBoolField(TEXT("replicate_input_directly"), GetBoolProperty(Ctx.CDO, FName(TEXT("bReplicateInputDirectly"))));
	Flags->SetBoolField(TEXT("retrigger_instanced_ability"), GetBoolProperty(Ctx.CDO, FName(TEXT("bRetriggerInstancedAbility"))));
	Flags->SetBoolField(TEXT("server_respects_remote_ability_cancellation"), GetBoolProperty(Ctx.CDO, FName(TEXT("bServerRespectsRemoteAbilityCancellation"))));
	Result->SetObjectField(TEXT("flags"), Flags);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 2: Graph Building
// ============================================================

// ------------------------------------------------------------
//  Phase 2 Helpers
// ------------------------------------------------------------
namespace
{
	void ParsePosition(const TSharedPtr<FJsonObject>& Params, int32& OutX, int32& OutY, int32 DefaultX = 0, int32 DefaultY = 0)
	{
		OutX = DefaultX;
		OutY = DefaultY;
		const TSharedPtr<FJsonObject>* PosObj;
		if (Params->TryGetObjectField(TEXT("position"), PosObj))
		{
			OutX = static_cast<int32>((*PosObj)->GetNumberField(TEXT("x")));
			OutY = static_cast<int32>((*PosObj)->GetNumberField(TEXT("y")));
		}
	}

	// Serialize node info to JSON for responses
	TSharedPtr<FJsonObject> NodeToResultJson(UEdGraphNode* Node)
	{
		TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
		NObj->SetStringField(TEXT("node_id"), Node->GetName());
		NObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
		NObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

		TArray<TSharedPtr<FJsonValue>> PinsArr;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->bHidden) continue;
			TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
			PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
			PinObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
			PinsArr.Add(MakeShared<FJsonValueObject>(PinObj));
		}
		NObj->SetArrayField(TEXT("pins"), PinsArr);
		return NObj;
	}

	// Find the event graph in an ability BP (UbergraphPages[0])
	UEdGraph* GetAbilityEventGraph(UBlueprint* BP, const FString& GraphName)
	{
		if (!GraphName.IsEmpty())
		{
			for (UEdGraph* Graph : BP->UbergraphPages)
			{
				if (Graph && Graph->GetName() == GraphName) return Graph;
			}
			for (UEdGraph* Graph : BP->FunctionGraphs)
			{
				if (Graph && Graph->GetName() == GraphName) return Graph;
			}
			return nullptr;
		}
		return BP->UbergraphPages.Num() > 0 ? BP->UbergraphPages[0] : nullptr;
	}

	// Find an ability task's static factory function on its class
	UFunction* FindTaskFactoryFunction(UClass* TaskClass, const FString& FunctionHint)
	{
		if (!TaskClass) return nullptr;

		// If a specific function was requested, try it first
		if (!FunctionHint.IsEmpty())
		{
			UFunction* Func = TaskClass->FindFunctionByName(FName(*FunctionHint));
			if (Func && Func->HasAllFunctionFlags(FUNC_Static))
			{
				return Func;
			}
		}

		// Auto-detect: find the first static factory function on the class
		UFunction* BestFunc = nullptr;
		for (TFieldIterator<UFunction> It(TaskClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			UFunction* Func = *It;
			if (!Func->HasAllFunctionFlags(FUNC_Static | FUNC_BlueprintCallable))
			{
				continue;
			}

			// The return type should be a pointer to this task class or a parent
			FObjectPropertyBase* ReturnProp = nullptr;
			for (TFieldIterator<FProperty> PropIt(Func); PropIt; ++PropIt)
			{
				if (PropIt->HasAllPropertyFlags(CPF_ReturnParm | CPF_Parm))
				{
					ReturnProp = CastField<FObjectPropertyBase>(*PropIt);
					break;
				}
			}

			if (ReturnProp && ReturnProp->PropertyClass && ReturnProp->PropertyClass->IsChildOf(UAbilityTask::StaticClass()))
			{
				BestFunc = Func;
				break;
			}
		}
		return BestFunc;
	}

	// Create a UK2Node_CallFunction for a given function name
	UK2Node_CallFunction* CreateCallFunctionNode(UEdGraph* Graph, const FString& FunctionName, UClass* SearchClass, int32 PosX, int32 PosY)
	{
		UFunction* Func = nullptr;

		if (SearchClass)
		{
			Func = SearchClass->FindFunctionByName(FName(*FunctionName));
			if (!Func)
			{
				Func = SearchClass->FindFunctionByName(FName(*(TEXT("K2_") + FunctionName)));
			}
		}

		if (!Func)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				Func = It->FindFunctionByName(FName(*FunctionName));
				if (Func) break;
				Func = It->FindFunctionByName(FName(*(TEXT("K2_") + FunctionName)));
				if (Func) break;
			}
		}

		if (!Func) return nullptr;

		UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
		CallNode->SetFromFunction(Func);
		CallNode->NodePosX = PosX;
		CallNode->NodePosY = PosY;
		Graph->AddNode(CallNode, /*bUserAction=*/true, /*bSelectNewNode=*/false);
		CallNode->AllocateDefaultPins();
		return CallNode;
	}
}

// ============================================================
//  add_ability_task_node
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleAddAbilityTaskNode(const TSharedPtr<FJsonObject>& Params)
{
	FAbilityCDOContext Ctx;
	FMonolithActionResult Err;
	if (!Ctx.Load(Params, Err))
	{
		return Err;
	}

	FString TaskClassName;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("task_class"), TaskClassName, Err))
	{
		return Err;
	}

	// Normalize: add U prefix if missing
	FString NormalizedClassName = TaskClassName;
	if (!NormalizedClassName.StartsWith(TEXT("U")))
	{
		NormalizedClassName = TEXT("U") + NormalizedClassName;
	}

	// Find the task class
	UClass* TaskClass = FindFirstObject<UClass>(*NormalizedClassName, EFindFirstObjectOptions::NativeFirst);
	if (!TaskClass)
	{
		TaskClass = FindFirstObject<UClass>(*TaskClassName, EFindFirstObjectOptions::NativeFirst);
	}
	if (!TaskClass || !TaskClass->IsChildOf(UAbilityTask::StaticClass()))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("'%s' is not a valid UAbilityTask subclass"), *TaskClassName));
	}

	// Find the factory function
	FString FactoryFuncHint = Params->GetStringField(TEXT("factory_function"));
	UFunction* FactoryFunc = FindTaskFactoryFunction(TaskClass, FactoryFuncHint);
	if (!FactoryFunc)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("No static factory function found on '%s'. Specify 'factory_function' explicitly."), *TaskClass->GetName()));
	}

	// Get graph
	UEdGraph* Graph = GetAbilityEventGraph(Ctx.BP, FString());
	if (!Graph)
	{
		return FMonolithActionResult::Error(TEXT("No event graph found in ability Blueprint"));
	}

	int32 PosX, PosY;
	ParsePosition(Params, PosX, PosY, 400, 0);

	// Create the latent ability call node
	UK2Node_LatentAbilityCall* TaskNode = NewObject<UK2Node_LatentAbilityCall>(Graph);
	// ProxyFactoryFunctionName/ProxyFactoryClass/ProxyClass are protected in UE 5.7 — set via reflection
	{
		FProperty* FFNProp = TaskNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryFunctionName"));
		if (FFNProp) { *FFNProp->ContainerPtrToValuePtr<FName>(TaskNode) = FactoryFunc->GetFName(); }
		FProperty* FFCProp = TaskNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryClass"));
		if (FFCProp) { *FFCProp->ContainerPtrToValuePtr<UClass*>(TaskNode) = TaskClass; }
		FProperty* PCProp = TaskNode->GetClass()->FindPropertyByName(TEXT("ProxyClass"));
		if (PCProp) { *PCProp->ContainerPtrToValuePtr<UClass*>(TaskNode) = TaskClass; }
	}
	TaskNode->NodePosX = PosX;
	TaskNode->NodePosY = PosY;
	Graph->AddNode(TaskNode, /*bUserAction=*/true, /*bSelectNewNode=*/false);
	TaskNode->AllocateDefaultPins();

	Ctx.MarkModified();

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(Ctx.AssetPath,
		FString::Printf(TEXT("Added ability task node '%s' via '%s'"),
			*TaskClass->GetName(), *FactoryFunc->GetName()));
	Result->SetObjectField(TEXT("node"), NodeToResultJson(TaskNode));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  add_commit_and_end_flow
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleAddCommitAndEndFlow(const TSharedPtr<FJsonObject>& Params)
{
	FAbilityCDOContext Ctx;
	FMonolithActionResult Err;
	if (!Ctx.Load(Params, Err))
	{
		return Err;
	}

	FString GraphName = Params->GetStringField(TEXT("graph_name"));
	UEdGraph* Graph = GetAbilityEventGraph(Ctx.BP, GraphName);
	if (!Graph)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Graph '%s' not found"), *GraphName));
	}

	int32 PosX, PosY;
	ParsePosition(Params, PosX, PosY, 300, 0);

	// 1. CommitAbility node (UGameplayAbility::CommitAbility)
	UK2Node_CallFunction* CommitNode = CreateCallFunctionNode(
		Graph, TEXT("CommitAbility"), UGameplayAbility::StaticClass(), PosX, PosY);
	if (!CommitNode)
	{
		CommitNode = CreateCallFunctionNode(
			Graph, TEXT("K2_CommitAbility"), UGameplayAbility::StaticClass(), PosX, PosY);
	}
	if (!CommitNode)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create CommitAbility node. Function not found on UGameplayAbility."));
	}

	// 2. Branch node
	UK2Node_IfThenElse* BranchNode = NewObject<UK2Node_IfThenElse>(Graph);
	BranchNode->NodePosX = PosX + 300;
	BranchNode->NodePosY = PosY;
	Graph->AddNode(BranchNode, /*bUserAction=*/true, /*bSelectNewNode=*/false);
	BranchNode->AllocateDefaultPins();

	// 3. EndAbility node (for the false/failure branch)
	UK2Node_CallFunction* EndAbilityNode = CreateCallFunctionNode(
		Graph, TEXT("EndAbility"), UGameplayAbility::StaticClass(), PosX + 600, PosY + 200);
	if (!EndAbilityNode)
	{
		EndAbilityNode = CreateCallFunctionNode(
			Graph, TEXT("K2_EndAbility"), UGameplayAbility::StaticClass(), PosX + 600, PosY + 200);
	}

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

	// Wire CommitAbility exec out -> Branch exec in
	UEdGraphPin* CommitExecOut = CommitNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* BranchExecIn = BranchNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	if (CommitExecOut && BranchExecIn)
	{
		Schema->TryCreateConnection(CommitExecOut, BranchExecIn);
	}

	// Wire CommitAbility return value -> Branch condition
	UEdGraphPin* CommitReturnPin = CommitNode->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
	UEdGraphPin* BranchCondition = BranchNode->FindPin(UEdGraphSchema_K2::PN_Condition, EGPD_Input);
	if (CommitReturnPin && BranchCondition)
	{
		Schema->TryCreateConnection(CommitReturnPin, BranchCondition);
	}

	// Wire Branch False -> EndAbility
	if (EndAbilityNode)
	{
		UEdGraphPin* BranchFalse = BranchNode->FindPin(UEdGraphSchema_K2::PN_Else, EGPD_Output);
		UEdGraphPin* EndExecIn = EndAbilityNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		if (BranchFalse && EndExecIn)
		{
			Schema->TryCreateConnection(BranchFalse, EndExecIn);
		}
	}

	Ctx.MarkModified();

	// Build result with all created nodes
	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(Ctx.AssetPath,
		TEXT("Added CommitAbility -> Branch -> EndAbility flow"));

	TArray<TSharedPtr<FJsonValue>> NodesArr;
	NodesArr.Add(MakeShared<FJsonValueObject>(NodeToResultJson(CommitNode)));
	NodesArr.Add(MakeShared<FJsonValueObject>(NodeToResultJson(BranchNode)));
	if (EndAbilityNode)
	{
		NodesArr.Add(MakeShared<FJsonValueObject>(NodeToResultJson(EndAbilityNode)));
	}
	Result->SetArrayField(TEXT("nodes"), NodesArr);

	// Return the Branch True pin name so callers know where to continue
	Result->SetStringField(TEXT("continue_from_node"), BranchNode->GetName());
	Result->SetStringField(TEXT("continue_from_pin"), UEdGraphSchema_K2::PN_Then.ToString());

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  add_effect_application
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleAddEffectApplication(const TSharedPtr<FJsonObject>& Params)
{
	FAbilityCDOContext Ctx;
	FMonolithActionResult Err;
	if (!Ctx.Load(Params, Err))
	{
		return Err;
	}

	FString EffectClassPath;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("effect_class"), EffectClassPath, Err))
	{
		return Err;
	}

	FString Target = Params->GetStringField(TEXT("target"));
	if (Target.IsEmpty()) Target = TEXT("self");

	bool bToSelf = (Target.ToLower() == TEXT("self") || Target.ToLower() == TEXT("owner"));

	UEdGraph* Graph = GetAbilityEventGraph(Ctx.BP, FString());
	if (!Graph)
	{
		return FMonolithActionResult::Error(TEXT("No event graph found in ability Blueprint"));
	}

	int32 PosX, PosY;
	ParsePosition(Params, PosX, PosY, 400, 0);

	// Try the Spec-based versions first, fall back to the simpler class-based ones
	FString FuncName = bToSelf
		? TEXT("K2_ApplyGameplayEffectSpecToOwner")
		: TEXT("K2_ApplyGameplayEffectSpecToTarget");

	UK2Node_CallFunction* ApplyNode = CreateCallFunctionNode(
		Graph, FuncName, UGameplayAbility::StaticClass(), PosX, PosY);

	if (!ApplyNode)
	{
		FuncName = bToSelf
			? TEXT("BP_ApplyGameplayEffectToOwner")
			: TEXT("BP_ApplyGameplayEffectToTarget");
		ApplyNode = CreateCallFunctionNode(
			Graph, FuncName, UGameplayAbility::StaticClass(), PosX, PosY);
	}

	if (!ApplyNode)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to find ApplyGameplayEffect function for target '%s'"), *Target));
	}

	// Try to set the GameplayEffectClass default pin value
	for (UEdGraphPin* Pin : ApplyNode->Pins)
	{
		if (Pin && !Pin->bHidden && Pin->PinName.ToString().Contains(TEXT("GameplayEffectClass")))
		{
			FString ClassRefPath = EffectClassPath;
			if (!ClassRefPath.EndsWith(TEXT("_C")))
			{
				ClassRefPath += TEXT("_C");
			}
			Pin->DefaultValue = ClassRefPath;
			break;
		}
	}

	Ctx.MarkModified();

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(Ctx.AssetPath,
		FString::Printf(TEXT("Added %s node for effect '%s'"), *FuncName, *EffectClassPath));
	Result->SetObjectField(TEXT("node"), NodeToResultJson(ApplyNode));
	Result->SetStringField(TEXT("target"), Target);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  add_gameplay_cue_node
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleAddGameplayCueNode(const TSharedPtr<FJsonObject>& Params)
{
	FAbilityCDOContext Ctx;
	FMonolithActionResult Err;
	if (!Ctx.Load(Params, Err))
	{
		return Err;
	}

	FString CueTag;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("cue_tag"), CueTag, Err))
	{
		return Err;
	}

	FString Type;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("type"), Type, Err))
	{
		return Err;
	}

	Type = Type.ToLower();

	// Map type to function name
	FString FuncName;
	if (Type == TEXT("execute"))
	{
		FuncName = TEXT("K2_ExecuteGameplayCue");
	}
	else if (Type == TEXT("add"))
	{
		FuncName = TEXT("K2_AddGameplayCue");
	}
	else if (Type == TEXT("remove"))
	{
		FuncName = TEXT("K2_RemoveGameplayCue");
	}
	else
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid type '%s'. Use 'execute', 'add', or 'remove'."), *Type));
	}

	UEdGraph* Graph = GetAbilityEventGraph(Ctx.BP, FString());
	if (!Graph)
	{
		return FMonolithActionResult::Error(TEXT("No event graph found in ability Blueprint"));
	}

	int32 PosX, PosY;
	ParsePosition(Params, PosX, PosY, 400, 0);

	// K2_ GameplayCue variants are on UGameplayAbility itself
	UK2Node_CallFunction* CueNode = CreateCallFunctionNode(
		Graph, FuncName, UGameplayAbility::StaticClass(), PosX, PosY);

	if (!CueNode)
	{
		// Try on UAbilitySystemComponent
		CueNode = CreateCallFunctionNode(
			Graph, FuncName, UAbilitySystemComponent::StaticClass(), PosX, PosY);
	}

	if (!CueNode)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to find function '%s'"), *FuncName));
	}

	// Set the GameplayCueTag default pin value
	for (UEdGraphPin* Pin : CueNode->Pins)
	{
		if (Pin && !Pin->bHidden &&
			(Pin->PinName.ToString().Contains(TEXT("GameplayCueTag")) ||
			 Pin->PinName.ToString().Contains(TEXT("GameplayCue"))))
		{
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			{
				Pin->DefaultValue = CueTag;
				break;
			}
		}
	}

	Ctx.MarkModified();

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(Ctx.AssetPath,
		FString::Printf(TEXT("Added GameplayCue %s node for tag '%s'"), *Type, *CueTag));
	Result->SetObjectField(TEXT("node"), NodeToResultJson(CueNode));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 2: Productivity
// ============================================================

// ============================================================
//  create_ability_from_template
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleCreateAbilityFromTemplate(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("save_path"), SavePath, Err))
	{
		return Err;
	}

	FString TemplateName;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("template"), TemplateName, Err))
	{
		return Err;
	}

	// Template defaults
	FString InstancingPolicy = TEXT("InstancedPerActor");
	FString NetExecPolicy = TEXT("LocalPredicted");
	FString NetSecPolicy = TEXT("ClientOrServer");
	TArray<FString> AbilityTags;

	if (TemplateName == TEXT("ability_instant"))
	{
		AbilityTags.Add(TEXT("Ability.Type.Instant"));
	}
	else if (TemplateName == TEXT("ability_montage"))
	{
		AbilityTags.Add(TEXT("Ability.Type.Montage"));
	}
	else if (TemplateName == TEXT("ability_channeled"))
	{
		AbilityTags.Add(TEXT("Ability.Type.Channeled"));
	}
	else if (TemplateName == TEXT("ability_passive"))
	{
		AbilityTags.Add(TEXT("Ability.Type.Passive"));
		NetExecPolicy = TEXT("ServerOnly");
	}
	else if (TemplateName == TEXT("ability_toggle"))
	{
		AbilityTags.Add(TEXT("Ability.Type.Toggle"));
	}
	else if (TemplateName == TEXT("ability_heal_self"))
	{
		AbilityTags.Add(TEXT("Ability.Survival.Heal"));
	}
	else if (TemplateName == TEXT("ability_reload"))
	{
		AbilityTags.Add(TEXT("Ability.Combat.Reload"));
	}
	else if (TemplateName == TEXT("ability_flee_panic"))
	{
		AbilityTags.Add(TEXT("Ability.Horror.FleePanic"));
		NetExecPolicy = TEXT("ServerOnly");
	}
	else if (TemplateName == TEXT("ability_investigate"))
	{
		AbilityTags.Add(TEXT("Ability.Horror.Investigate"));
	}
	else if (TemplateName == TEXT("ability_barricade"))
	{
		AbilityTags.Add(TEXT("Ability.Survival.Barricade"));
	}
	else if (TemplateName == TEXT("ability_charge_attack"))
	{
		AbilityTags.Add(TEXT("Ability.Combat.ChargeAttack"));
	}
	else if (TemplateName == TEXT("ability_combo_chain"))
	{
		AbilityTags.Add(TEXT("Ability.Combat.ComboChain"));
	}
	else
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown template: '%s'. Valid templates: ability_instant, ability_montage, ability_channeled, ability_passive, ability_toggle, ability_heal_self, ability_reload, ability_flee_panic, ability_investigate, ability_barricade, ability_charge_attack, ability_combo_chain"), *TemplateName));
	}

	// Apply overrides from params
	const TSharedPtr<FJsonObject>* Overrides;
	if (Params->TryGetObjectField(TEXT("overrides"), Overrides))
	{
		if ((*Overrides)->HasField(TEXT("instancing_policy")))
			InstancingPolicy = (*Overrides)->GetStringField(TEXT("instancing_policy"));
		if ((*Overrides)->HasField(TEXT("net_execution_policy")))
			NetExecPolicy = (*Overrides)->GetStringField(TEXT("net_execution_policy"));
		if ((*Overrides)->HasField(TEXT("net_security_policy")))
			NetSecPolicy = (*Overrides)->GetStringField(TEXT("net_security_policy"));

		const TArray<TSharedPtr<FJsonValue>>* OverrideTags;
		if ((*Overrides)->TryGetArrayField(TEXT("ability_tags"), OverrideTags))
		{
			AbilityTags.Empty();
			for (const auto& TagVal : *OverrideTags)
			{
				AbilityTags.Add(TagVal->AsString());
			}
		}
	}

	// Step 1: Create the ability Blueprint
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("save_path"), SavePath);

	FString ParentClass = TEXT("GameplayAbility");
	if (Params->TryGetObjectField(TEXT("overrides"), Overrides) && (*Overrides)->HasField(TEXT("parent_class")))
	{
		ParentClass = (*Overrides)->GetStringField(TEXT("parent_class"));
	}
	CreateParams->SetStringField(TEXT("parent_class"), ParentClass);

	FMonolithActionResult CreateResult = HandleCreateAbility(CreateParams);
	if (!CreateResult.bSuccess)
	{
		return CreateResult;
	}

	// Step 2: Set policies
	TSharedPtr<FJsonObject> PolicyParams = MakeShared<FJsonObject>();
	PolicyParams->SetStringField(TEXT("asset_path"), SavePath);
	PolicyParams->SetStringField(TEXT("instancing_policy"), InstancingPolicy);
	PolicyParams->SetStringField(TEXT("net_execution_policy"), NetExecPolicy);
	PolicyParams->SetStringField(TEXT("net_security_policy"), NetSecPolicy);
	HandleSetAbilityPolicy(PolicyParams);

	// Step 3: Set ability tags
	if (AbilityTags.Num() > 0)
	{
		TSharedPtr<FJsonObject> TagParams = MakeShared<FJsonObject>();
		TagParams->SetStringField(TEXT("asset_path"), SavePath);
		TagParams->SetStringField(TEXT("container"), TEXT("AbilityTags"));
		TArray<TSharedPtr<FJsonValue>> TagValues;
		for (const FString& Tag : AbilityTags)
		{
			TagValues.Add(MakeShared<FJsonValueString>(Tag));
		}
		TagParams->SetArrayField(TEXT("tags"), TagValues);
		TagParams->SetStringField(TEXT("mode"), TEXT("set"));
		HandleSetAbilityTags(TagParams);
	}

	// Step 4: Set cost/cooldown from overrides if present
	if (Params->TryGetObjectField(TEXT("overrides"), Overrides))
	{
		if ((*Overrides)->HasField(TEXT("cost_effect_class")))
		{
			TSharedPtr<FJsonObject> CostParams = MakeShared<FJsonObject>();
			CostParams->SetStringField(TEXT("asset_path"), SavePath);
			CostParams->SetStringField(TEXT("cost_effect_class"), (*Overrides)->GetStringField(TEXT("cost_effect_class")));
			HandleSetAbilityCost(CostParams);
		}
		if ((*Overrides)->HasField(TEXT("cooldown_effect_class")))
		{
			TSharedPtr<FJsonObject> CDParams = MakeShared<FJsonObject>();
			CDParams->SetStringField(TEXT("asset_path"), SavePath);
			CDParams->SetStringField(TEXT("cooldown_effect_class"), (*Overrides)->GetStringField(TEXT("cooldown_effect_class")));
			HandleSetAbilityCooldown(CDParams);
		}
	}

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(SavePath,
		FString::Printf(TEXT("Created ability from template '%s'"), *TemplateName));
	Result->SetStringField(TEXT("template"), TemplateName);
	Result->SetStringField(TEXT("instancing_policy"), InstancingPolicy);
	Result->SetStringField(TEXT("net_execution_policy"), NetExecPolicy);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  build_ability_from_spec
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleBuildAbilityFromSpec(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("save_path"), SavePath, Err))
	{
		return Err;
	}

	const TSharedPtr<FJsonObject>* SpecPtr;
	if (!Params->TryGetObjectField(TEXT("spec"), SpecPtr))
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: spec (object)"));
	}
	const TSharedPtr<FJsonObject>& Spec = *SpecPtr;

	// Step 1: Create ability
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("save_path"), SavePath);
	if (Spec->HasField(TEXT("parent_class")))
	{
		CreateParams->SetStringField(TEXT("parent_class"), Spec->GetStringField(TEXT("parent_class")));
	}
	if (Spec->HasField(TEXT("display_name")))
	{
		CreateParams->SetStringField(TEXT("display_name"), Spec->GetStringField(TEXT("display_name")));
	}

	FMonolithActionResult CreateResult = HandleCreateAbility(CreateParams);
	if (!CreateResult.bSuccess)
	{
		return CreateResult;
	}

	TArray<FString> AppliedSteps;

	// Step 2: Set policies
	{
		bool bHasPolicy = Spec->HasField(TEXT("instancing_policy"))
			|| Spec->HasField(TEXT("net_execution_policy"))
			|| Spec->HasField(TEXT("net_security_policy"));

		if (bHasPolicy)
		{
			TSharedPtr<FJsonObject> PolicyParams = MakeShared<FJsonObject>();
			PolicyParams->SetStringField(TEXT("asset_path"), SavePath);
			if (Spec->HasField(TEXT("instancing_policy")))
				PolicyParams->SetStringField(TEXT("instancing_policy"), Spec->GetStringField(TEXT("instancing_policy")));
			if (Spec->HasField(TEXT("net_execution_policy")))
				PolicyParams->SetStringField(TEXT("net_execution_policy"), Spec->GetStringField(TEXT("net_execution_policy")));
			if (Spec->HasField(TEXT("net_security_policy")))
				PolicyParams->SetStringField(TEXT("net_security_policy"), Spec->GetStringField(TEXT("net_security_policy")));

			FMonolithActionResult PolicyResult = HandleSetAbilityPolicy(PolicyParams);
			if (PolicyResult.bSuccess) AppliedSteps.Add(TEXT("policies"));
		}
	}

	// Step 3: Set tags
	{
		const TSharedPtr<FJsonObject>* TagsObj;
		if (Spec->TryGetObjectField(TEXT("tags"), TagsObj))
		{
			for (const auto& Pair : (*TagsObj)->Values)
			{
				const TArray<TSharedPtr<FJsonValue>>* TagArr;
				if (Pair.Value->TryGetArray(TagArr) && TagArr->Num() > 0)
				{
					TSharedPtr<FJsonObject> TagParams = MakeShared<FJsonObject>();
					TagParams->SetStringField(TEXT("asset_path"), SavePath);
					TagParams->SetStringField(TEXT("container"), Pair.Key);
					TagParams->SetArrayField(TEXT("tags"), *TagArr);
					TagParams->SetStringField(TEXT("mode"), TEXT("set"));
					HandleSetAbilityTags(TagParams);
				}
			}
			AppliedSteps.Add(TEXT("tags"));
		}
	}

	// Step 4: Cost
	if (Spec->HasField(TEXT("cost_effect_class")))
	{
		TSharedPtr<FJsonObject> CostParams = MakeShared<FJsonObject>();
		CostParams->SetStringField(TEXT("asset_path"), SavePath);
		CostParams->SetStringField(TEXT("cost_effect_class"), Spec->GetStringField(TEXT("cost_effect_class")));
		FMonolithActionResult CostResult = HandleSetAbilityCost(CostParams);
		if (CostResult.bSuccess) AppliedSteps.Add(TEXT("cost"));
	}

	// Step 5: Cooldown
	if (Spec->HasField(TEXT("cooldown_effect_class")))
	{
		TSharedPtr<FJsonObject> CDParams = MakeShared<FJsonObject>();
		CDParams->SetStringField(TEXT("asset_path"), SavePath);
		CDParams->SetStringField(TEXT("cooldown_effect_class"), Spec->GetStringField(TEXT("cooldown_effect_class")));
		FMonolithActionResult CDResult = HandleSetAbilityCooldown(CDParams);
		if (CDResult.bSuccess) AppliedSteps.Add(TEXT("cooldown"));
	}

	// Step 6: Triggers
	{
		const TArray<TSharedPtr<FJsonValue>>* TriggersArr;
		if (Spec->TryGetArrayField(TEXT("triggers"), TriggersArr) && TriggersArr->Num() > 0)
		{
			TSharedPtr<FJsonObject> TrigParams = MakeShared<FJsonObject>();
			TrigParams->SetStringField(TEXT("asset_path"), SavePath);
			TrigParams->SetArrayField(TEXT("triggers"), *TriggersArr);
			FMonolithActionResult TrigResult = HandleSetAbilityTriggers(TrigParams);
			if (TrigResult.bSuccess) AppliedSteps.Add(TEXT("triggers"));
		}
	}

	// Step 7: Flags
	{
		const TSharedPtr<FJsonObject>* FlagsObj;
		if (Spec->TryGetObjectField(TEXT("flags"), FlagsObj))
		{
			TSharedPtr<FJsonObject> FlagParams = MakeShared<FJsonObject>();
			FlagParams->SetStringField(TEXT("asset_path"), SavePath);
			if ((*FlagsObj)->HasField(TEXT("replicate_input_directly")))
				FlagParams->SetBoolField(TEXT("replicate_input_directly"), (*FlagsObj)->GetBoolField(TEXT("replicate_input_directly")));
			if ((*FlagsObj)->HasField(TEXT("retrigger_instanced_ability")))
				FlagParams->SetBoolField(TEXT("retrigger_instanced_ability"), (*FlagsObj)->GetBoolField(TEXT("retrigger_instanced_ability")));
			if ((*FlagsObj)->HasField(TEXT("server_respects_remote_ability_cancellation")))
				FlagParams->SetBoolField(TEXT("server_respects_remote_ability_cancellation"), (*FlagsObj)->GetBoolField(TEXT("server_respects_remote_ability_cancellation")));

			FMonolithActionResult FlagResult = HandleSetAbilityFlags(FlagParams);
			if (FlagResult.bSuccess) AppliedSteps.Add(TEXT("flags"));
		}
	}

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(SavePath,
		FString::Printf(TEXT("Built ability from spec. Applied: %s"),
			*FString::Join(AppliedSteps, TEXT(", "))));

	TArray<TSharedPtr<FJsonValue>> StepsArr;
	for (const FString& Step : AppliedSteps)
	{
		StepsArr.Add(MakeShared<FJsonValueString>(Step));
	}
	Result->SetArrayField(TEXT("applied_steps"), StepsArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  batch_create_abilities
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleBatchCreateAbilities(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* AbilitiesArr;
	if (!Params->TryGetArrayField(TEXT("abilities"), AbilitiesArr) || AbilitiesArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: abilities (array)"));
	}

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 SuccessCount = 0;
	int32 FailCount = 0;

	for (int32 i = 0; i < AbilitiesArr->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>* AbilitySpec;
		if (!(*AbilitiesArr)[i]->TryGetObject(AbilitySpec))
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetNumberField(TEXT("index"), i);
			ErrObj->SetStringField(TEXT("error"), TEXT("Entry is not a valid object"));
			Results.Add(MakeShared<FJsonValueObject>(ErrObj));
			FailCount++;
			continue;
		}

		FString SavePath = (*AbilitySpec)->GetStringField(TEXT("save_path"));
		if (SavePath.IsEmpty())
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetNumberField(TEXT("index"), i);
			ErrObj->SetStringField(TEXT("error"), TEXT("Missing save_path"));
			Results.Add(MakeShared<FJsonValueObject>(ErrObj));
			FailCount++;
			continue;
		}

		FMonolithActionResult AbilityResult;

		if ((*AbilitySpec)->HasField(TEXT("spec")))
		{
			TSharedPtr<FJsonObject> BuildParams = MakeShared<FJsonObject>();
			BuildParams->SetStringField(TEXT("save_path"), SavePath);
			BuildParams->SetObjectField(TEXT("spec"), (*AbilitySpec)->GetObjectField(TEXT("spec")));
			AbilityResult = HandleBuildAbilityFromSpec(BuildParams);
		}
		else if ((*AbilitySpec)->HasField(TEXT("template")))
		{
			AbilityResult = HandleCreateAbilityFromTemplate(*AbilitySpec);
		}
		else
		{
			AbilityResult = HandleCreateAbility(*AbilitySpec);
		}

		TSharedPtr<FJsonObject> EntryResult = MakeShared<FJsonObject>();
		EntryResult->SetNumberField(TEXT("index"), i);
		EntryResult->SetStringField(TEXT("save_path"), SavePath);
		EntryResult->SetBoolField(TEXT("success"), AbilityResult.bSuccess);
		if (!AbilityResult.bSuccess)
		{
			EntryResult->SetStringField(TEXT("error"), AbilityResult.ErrorMessage);
			FailCount++;
		}
		else
		{
			SuccessCount++;
		}
		Results.Add(MakeShared<FJsonValueObject>(EntryResult));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("results"), Results);
	Result->SetNumberField(TEXT("total"), AbilitiesArr->Num());
	Result->SetNumberField(TEXT("succeeded"), SuccessCount);
	Result->SetNumberField(TEXT("failed"), FailCount);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Batch create: %d/%d succeeded"), SuccessCount, AbilitiesArr->Num()));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  duplicate_ability
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleDuplicateAbility(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, Err))
	{
		return Err;
	}

	FString NewPath;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("new_path"), NewPath, Err))
	{
		return Err;
	}

	// Validate source is an ability
	{
		FString LoadErr;
		UBlueprint* SourceBP = MonolithGAS::LoadBlueprintFromParams(Params, AssetPath, LoadErr);
		if (!SourceBP)
		{
			return FMonolithActionResult::Error(LoadErr);
		}
		if (!MonolithGAS::IsAbilityBlueprint(SourceBP))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("'%s' is not a GameplayAbility Blueprint"), *AssetPath));
		}
	}

	// Duplicate using UEditorAssetLibrary
	UObject* DupObj = UEditorAssetLibrary::DuplicateAsset(AssetPath, NewPath);
	if (!DupObj)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to duplicate '%s' to '%s'"), *AssetPath, *NewPath));
	}

	UBlueprint* DupBP = Cast<UBlueprint>(DupObj);

	// Apply tag renaming if requested
	const TSharedPtr<FJsonObject>* RenameTagsObj;
	int32 TagsRenamed = 0;
	if (DupBP && Params->TryGetObjectField(TEXT("rename_tags"), RenameTagsObj))
	{
		UGameplayAbility* DupCDO = MonolithGAS::GetBlueprintCDO<UGameplayAbility>(DupBP);
		if (DupCDO)
		{
			// Build rename map
			TMap<FString, FString> RenameMap;
			for (const auto& Pair : (*RenameTagsObj)->Values)
			{
				RenameMap.Add(Pair.Key, Pair.Value->AsString());
			}

			// Apply to all tag containers
			for (const FString& ContainerName : AllContainerNames)
			{
				FGameplayTagContainer* Container = GetTagContainerByName(DupCDO, ContainerName);
				if (!Container) continue;

				FGameplayTagContainer NewContainer;
				for (auto It = Container->CreateConstIterator(); It; ++It)
				{
					FString TagStr = It->ToString();
					if (const FString* Replacement = RenameMap.Find(TagStr))
					{
						FGameplayTag NewTag = MonolithGAS::StringToTag(*Replacement);
						if (NewTag.IsValid())
						{
							NewContainer.AddTag(NewTag);
							TagsRenamed++;
						}
						else
						{
							NewContainer.AddTag(*It);
						}
					}
					else
					{
						NewContainer.AddTag(*It);
					}
				}
				*Container = NewContainer;
			}

			// Also rename in triggers
			TArray<FAbilityTriggerData>* Triggers = GetPropertyPtrByName<TArray<FAbilityTriggerData>>(DupCDO, FName(TEXT("AbilityTriggers")));
			if (Triggers)
			{
				for (FAbilityTriggerData& Trigger : *Triggers)
				{
					FString TagStr = Trigger.TriggerTag.ToString();
					if (const FString* Replacement = RenameMap.Find(TagStr))
					{
						FGameplayTag NewTag = MonolithGAS::StringToTag(*Replacement);
						if (NewTag.IsValid())
						{
							Trigger.TriggerTag = NewTag;
							TagsRenamed++;
						}
					}
				}
			}

			if (TagsRenamed > 0)
			{
				FBlueprintEditorUtils::MarkBlueprintAsModified(DupBP);
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(NewPath,
		FString::Printf(TEXT("Duplicated '%s' to '%s'%s"),
			*AssetPath, *NewPath,
			TagsRenamed > 0 ? *FString::Printf(TEXT(" (%d tags renamed)"), TagsRenamed) : TEXT("")));
	Result->SetStringField(TEXT("source_path"), AssetPath);
	if (TagsRenamed > 0)
	{
		Result->SetNumberField(TEXT("tags_renamed"), TagsRenamed);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 2: Task Analysis
// ============================================================

// ============================================================
//  list_ability_tasks
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleListAbilityTasks(const TSharedPtr<FJsonObject>& Params)
{
	FString CategoryFilter = Params->GetStringField(TEXT("category_filter"));

	TArray<TSharedPtr<FJsonValue>> TaskList;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class->IsChildOf(UAbilityTask::StaticClass()) || Class == UAbilityTask::StaticClass())
		{
			continue;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}

		FString ClassName = Class->GetName();

		// Apply category filter
		if (!CategoryFilter.IsEmpty() && !ClassName.Contains(CategoryFilter))
		{
			continue;
		}

		TSharedPtr<FJsonObject> TaskObj = MakeShared<FJsonObject>();
		TaskObj->SetStringField(TEXT("class_name"), ClassName);
		TaskObj->SetStringField(TEXT("full_path"), Class->GetPathName());

		// Find factory functions
		TArray<TSharedPtr<FJsonValue>> FactoryFuncs;
		for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
		{
			UFunction* Func = *FuncIt;
			if (!Func->HasAllFunctionFlags(FUNC_Static | FUNC_BlueprintCallable))
			{
				continue;
			}

			bool bReturnsTask = false;
			for (TFieldIterator<FProperty> PropIt(Func); PropIt; ++PropIt)
			{
				if (PropIt->HasAllPropertyFlags(CPF_ReturnParm | CPF_Parm))
				{
					FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(*PropIt);
					if (ObjProp && ObjProp->PropertyClass && ObjProp->PropertyClass->IsChildOf(UAbilityTask::StaticClass()))
					{
						bReturnsTask = true;
					}
					break;
				}
			}

			if (bReturnsTask)
			{
				TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
				FuncObj->SetStringField(TEXT("name"), Func->GetName());

				TArray<TSharedPtr<FJsonValue>> ParamArr;
				for (TFieldIterator<FProperty> PropIt(Func); PropIt; ++PropIt)
				{
					if (PropIt->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
					if (PropIt->GetName() == TEXT("OwningAbility")) continue;

					TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
					ParamObj->SetStringField(TEXT("name"), PropIt->GetName());
					ParamObj->SetStringField(TEXT("type"), PropIt->GetCPPType());
					ParamArr.Add(MakeShared<FJsonValueObject>(ParamObj));
				}
				FuncObj->SetArrayField(TEXT("params"), ParamArr);
				FactoryFuncs.Add(MakeShared<FJsonValueObject>(FuncObj));
			}
		}
		TaskObj->SetArrayField(TEXT("factory_functions"), FactoryFuncs);

		// List output delegates
		TArray<TSharedPtr<FJsonValue>> DelegateArr;
		for (TFieldIterator<FMulticastDelegateProperty> DelegateIt(Class, EFieldIteratorFlags::ExcludeSuper); DelegateIt; ++DelegateIt)
		{
			DelegateArr.Add(MakeShared<FJsonValueString>(DelegateIt->GetName()));
		}
		TaskObj->SetArrayField(TEXT("delegates"), DelegateArr);

		TaskList.Add(MakeShared<FJsonValueObject>(TaskObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("tasks"), TaskList);
	Result->SetNumberField(TEXT("count"), TaskList.Num());
	if (!CategoryFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("category_filter"), CategoryFilter);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  get_ability_task_pins
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleGetAbilityTaskPins(const TSharedPtr<FJsonObject>& Params)
{
	FString TaskClassName;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("task_class"), TaskClassName, Err))
	{
		return Err;
	}

	FString NormalizedName = TaskClassName;
	if (!NormalizedName.StartsWith(TEXT("U")))
	{
		NormalizedName = TEXT("U") + NormalizedName;
	}

	UClass* TaskClass = FindFirstObject<UClass>(*NormalizedName, EFindFirstObjectOptions::NativeFirst);
	if (!TaskClass)
	{
		TaskClass = FindFirstObject<UClass>(*TaskClassName, EFindFirstObjectOptions::NativeFirst);
	}
	if (!TaskClass || !TaskClass->IsChildOf(UAbilityTask::StaticClass()))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("'%s' is not a valid UAbilityTask subclass"), *TaskClassName));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("class_name"), TaskClass->GetName());

	// Create a transient graph + node to inspect pins
	UEdGraph* TempGraph = NewObject<UEdGraph>(GetTransientPackage(), NAME_None, RF_Transient);
	TempGraph->Schema = UEdGraphSchema_K2::StaticClass();

	UFunction* FactoryFunc = FindTaskFactoryFunction(TaskClass, FString());

	if (FactoryFunc)
	{
		UK2Node_LatentAbilityCall* TempNode = NewObject<UK2Node_LatentAbilityCall>(TempGraph, NAME_None, RF_Transient);
		// Protected in UE 5.7 — set via reflection
		{
			FProperty* FFNProp = TempNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryFunctionName"));
			if (FFNProp) { *FFNProp->ContainerPtrToValuePtr<FName>(TempNode) = FactoryFunc->GetFName(); }
			FProperty* FFCProp = TempNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryClass"));
			if (FFCProp) { *FFCProp->ContainerPtrToValuePtr<UClass*>(TempNode) = TaskClass; }
			FProperty* PCProp = TempNode->GetClass()->FindPropertyByName(TEXT("ProxyClass"));
			if (PCProp) { *PCProp->ContainerPtrToValuePtr<UClass*>(TempNode) = TaskClass; }
		}
		TempGraph->AddNode(TempNode, false, false);
		TempNode->AllocateDefaultPins();

		Result->SetStringField(TEXT("factory_function"), FactoryFunc->GetName());

		TArray<TSharedPtr<FJsonValue>> InputPins;
		TArray<TSharedPtr<FJsonValue>> OutputPins;
		TArray<TSharedPtr<FJsonValue>> DelegatePins;
		TArray<TSharedPtr<FJsonValue>> ExecPins;

		for (const UEdGraphPin* Pin : TempNode->Pins)
		{
			if (!Pin || Pin->bHidden) continue;

			TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
			PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));

			FString TypeStr;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
				TypeStr = TEXT("exec");
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
				TypeStr = TEXT("delegate");
			else if (Pin->PinType.PinSubCategoryObject.IsValid())
				TypeStr = FString::Printf(TEXT("%s:%s"), *Pin->PinType.PinCategory.ToString(), *Pin->PinType.PinSubCategoryObject->GetName());
			else
				TypeStr = Pin->PinType.PinCategory.ToString();

			PinObj->SetStringField(TEXT("type"), TypeStr);
			if (!Pin->DefaultValue.IsEmpty())
				PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);

			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				if (Pin->Direction == EGPD_Output)
					DelegatePins.Add(MakeShared<FJsonValueObject>(PinObj));
				else
					ExecPins.Add(MakeShared<FJsonValueObject>(PinObj));
			}
			else if (Pin->Direction == EGPD_Input)
			{
				InputPins.Add(MakeShared<FJsonValueObject>(PinObj));
			}
			else
			{
				OutputPins.Add(MakeShared<FJsonValueObject>(PinObj));
			}
		}

		Result->SetArrayField(TEXT("input_pins"), InputPins);
		Result->SetArrayField(TEXT("output_pins"), OutputPins);
		Result->SetArrayField(TEXT("delegate_pins"), DelegatePins);
		Result->SetArrayField(TEXT("exec_pins"), ExecPins);

		TempNode->DestroyNode();
	}
	else
	{
		Result->SetStringField(TEXT("warning"), TEXT("No factory function found; pin schema unavailable"));
	}

	// List raw delegate properties from the class
	TArray<TSharedPtr<FJsonValue>> ClassDelegates;
	for (TFieldIterator<FMulticastDelegateProperty> It(TaskClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		TSharedPtr<FJsonObject> DelObj = MakeShared<FJsonObject>();
		DelObj->SetStringField(TEXT("name"), It->GetName());

		if (UFunction* SigFunc = It->SignatureFunction)
		{
			TArray<TSharedPtr<FJsonValue>> SigParams;
			for (TFieldIterator<FProperty> PropIt(SigFunc); PropIt; ++PropIt)
			{
				if (PropIt->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
				TSharedPtr<FJsonObject> SigParam = MakeShared<FJsonObject>();
				SigParam->SetStringField(TEXT("name"), PropIt->GetName());
				SigParam->SetStringField(TEXT("type"), PropIt->GetCPPType());
				SigParams.Add(MakeShared<FJsonValueObject>(SigParam));
			}
			DelObj->SetArrayField(TEXT("signature"), SigParams);
		}

		ClassDelegates.Add(MakeShared<FJsonValueObject>(DelObj));
	}
	Result->SetArrayField(TEXT("class_delegates"), ClassDelegates);

	TempGraph->MarkAsGarbage();

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  wire_ability_task_delegate
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleWireAbilityTaskDelegate(const TSharedPtr<FJsonObject>& Params)
{
	FAbilityCDOContext Ctx;
	FMonolithActionResult Err;
	if (!Ctx.Load(Params, Err))
	{
		return Err;
	}

	FString NodeId;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("node_id"), NodeId, Err))
	{
		return Err;
	}

	FString DelegateName;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("delegate_name"), DelegateName, Err))
	{
		return Err;
	}

	FString TargetNodeId;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("target_node_id"), TargetNodeId, Err))
	{
		return Err;
	}

	FString TargetPinName = Params->GetStringField(TEXT("target_pin"));
	if (TargetPinName.IsEmpty()) TargetPinName = TEXT("execute");

	// Find both nodes across all graphs
	UEdGraphNode* TaskNode = nullptr;
	UEdGraphNode* TargetNode = nullptr;

	TArray<UEdGraph*> AllGraphs;
	Ctx.BP->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (Node->GetName() == NodeId) TaskNode = Node;
			if (Node->GetName() == TargetNodeId) TargetNode = Node;
		}
	}

	if (!TaskNode)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Task node '%s' not found"), *NodeId));
	}

	if (!TargetNode)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Target node '%s' not found"), *TargetNodeId));
	}

	// Find delegate output pin on task node
	UEdGraphPin* DelegatePin = nullptr;
	for (UEdGraphPin* Pin : TaskNode->Pins)
	{
		if (Pin && !Pin->bHidden && Pin->Direction == EGPD_Output &&
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
			Pin->PinName.ToString() == DelegateName)
		{
			DelegatePin = Pin;
			break;
		}
	}

	if (!DelegatePin)
	{
		TArray<FString> DelegateNames;
		for (UEdGraphPin* Pin : TaskNode->Pins)
		{
			if (Pin && !Pin->bHidden && Pin->Direction == EGPD_Output &&
				Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				DelegateNames.Add(Pin->PinName.ToString());
			}
		}
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Delegate pin '%s' not found on node '%s'. Available exec outputs: %s"),
				*DelegateName, *NodeId, *FString::Join(DelegateNames, TEXT(", "))));
	}

	// Find target exec input pin
	UEdGraphPin* TargetPin = nullptr;
	for (UEdGraphPin* Pin : TargetNode->Pins)
	{
		if (Pin && !Pin->bHidden && Pin->Direction == EGPD_Input &&
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
			Pin->PinName.ToString() == TargetPinName)
		{
			TargetPin = Pin;
			break;
		}
	}

	if (!TargetPin)
	{
		// Fallback: any exec input
		for (UEdGraphPin* Pin : TargetNode->Pins)
		{
			if (Pin && !Pin->bHidden && Pin->Direction == EGPD_Input &&
				Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				TargetPin = Pin;
				break;
			}
		}
	}

	if (!TargetPin)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("No exec input pin '%s' found on target node '%s'"),
				*TargetPinName, *TargetNodeId));
	}

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	bool bConnected = Schema->TryCreateConnection(DelegatePin, TargetPin);
	if (!bConnected)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("TryCreateConnection failed: '%s.%s' -> '%s.%s'"),
				*NodeId, *DelegateName, *TargetNodeId, *TargetPin->PinName.ToString()));
	}

	Ctx.MarkModified();

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(Ctx.AssetPath,
		FString::Printf(TEXT("Wired delegate '%s' on '%s' to '%s'"),
			*DelegateName, *NodeId, *TargetNodeId));
	Result->SetStringField(TEXT("source_node"), NodeId);
	Result->SetStringField(TEXT("source_pin"), DelegateName);
	Result->SetStringField(TEXT("target_node"), TargetNodeId);
	Result->SetStringField(TEXT("target_pin"), TargetPin->PinName.ToString());

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  get_ability_graph_flow
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleGetAbilityGraphFlow(const TSharedPtr<FJsonObject>& Params)
{
	FAbilityCDOContext Ctx;
	FMonolithActionResult Err;
	if (!Ctx.Load(Params, Err))
	{
		return Err;
	}

	FString GraphName = Params->GetStringField(TEXT("graph_name"));
	UEdGraph* Graph = GetAbilityEventGraph(Ctx.BP, GraphName);
	if (!Graph)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Graph '%s' not found"), GraphName.IsEmpty() ? TEXT("EventGraph") : *GraphName));
	}

	// Analyze the graph
	bool bHasCommitAbility = false;
	bool bHasEndAbility = false;
	bool bHasActivateAbility = false;
	int32 AbilityTaskCount = 0;
	TArray<FString> DanglingDelegates;
	TArray<TSharedPtr<FJsonValue>> NodeSummary;
	FString DetectedPattern = TEXT("custom");

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;

		FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		FString NodeClass = Node->GetClass()->GetName();

		if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
		{
			FString FuncName = CallNode->FunctionReference.GetMemberName().ToString();

			if (FuncName.Contains(TEXT("CommitAbility")))
			{
				bHasCommitAbility = true;
			}
			if (FuncName.Contains(TEXT("EndAbility")))
			{
				bHasEndAbility = true;
			}
		}

		if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			FString EventName = EventNode->EventReference.GetMemberName().ToString();
			if (EventName.Contains(TEXT("ActivateAbility")))
			{
				bHasActivateAbility = true;
			}
		}

		// Check for latent ability task nodes
		if (Node->IsA<UK2Node_LatentAbilityCall>())
		{
			AbilityTaskCount++;

			// Check for dangling delegate pins
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && !Pin->bHidden &&
					Pin->Direction == EGPD_Output &&
					Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
					Pin->LinkedTo.Num() == 0)
				{
					DanglingDelegates.Add(FString::Printf(TEXT("%s.%s"),
						*Node->GetName(), *Pin->PinName.ToString()));
				}
			}
		}

		// Build node summary (skip comments)
		if (!Node->IsA<UEdGraphNode_Comment>())
		{
			TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
			NObj->SetStringField(TEXT("id"), Node->GetName());
			NObj->SetStringField(TEXT("class"), NodeClass);
			NObj->SetStringField(TEXT("title"), NodeTitle);
			NodeSummary.Add(MakeShared<FJsonValueObject>(NObj));
		}
	}

	// Detect pattern
	if (AbilityTaskCount == 0 && bHasCommitAbility && bHasEndAbility)
	{
		DetectedPattern = TEXT("instant");
	}
	else if (AbilityTaskCount > 0 && bHasCommitAbility)
	{
		DetectedPattern = TEXT("async_with_commit");
	}
	else if (AbilityTaskCount > 0 && !bHasCommitAbility)
	{
		DetectedPattern = TEXT("async_no_commit");
	}
	else if (NodeSummary.Num() <= 2)
	{
		DetectedPattern = TEXT("empty");
	}

	// Build warnings
	TArray<TSharedPtr<FJsonValue>> Warnings;

	if (!bHasCommitAbility)
	{
		if (Ctx.CDO->GetCostGameplayEffect() || Ctx.CDO->GetCooldownGameplayEffect())
		{
			TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
			WarnObj->SetStringField(TEXT("type"), TEXT("missing_commit"));
			WarnObj->SetStringField(TEXT("message"), TEXT("Ability has cost/cooldown effect but no CommitAbility call in graph. Cost/cooldown will never be applied."));
			Warnings.Add(MakeShared<FJsonValueObject>(WarnObj));
		}
	}

	if (!bHasEndAbility && NodeSummary.Num() > 2)
	{
		TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
		WarnObj->SetStringField(TEXT("type"), TEXT("missing_end_ability"));
		WarnObj->SetStringField(TEXT("message"), TEXT("No EndAbility call found. Ability may leak (never terminate)."));
		Warnings.Add(MakeShared<FJsonValueObject>(WarnObj));
	}

	for (const FString& Dangling : DanglingDelegates)
	{
		TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
		WarnObj->SetStringField(TEXT("type"), TEXT("dangling_delegate"));
		WarnObj->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Unhandled delegate output: %s"), *Dangling));
		Warnings.Add(MakeShared<FJsonValueObject>(WarnObj));
	}

	// Build result
	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(Ctx.AssetPath, TEXT("Graph flow analysis complete"));
	Result->SetStringField(TEXT("graph_name"), Graph->GetName());
	Result->SetStringField(TEXT("detected_pattern"), DetectedPattern);

	TSharedPtr<FJsonObject> FlowInfo = MakeShared<FJsonObject>();
	FlowInfo->SetBoolField(TEXT("has_activate_ability"), bHasActivateAbility);
	FlowInfo->SetBoolField(TEXT("has_commit_ability"), bHasCommitAbility);
	FlowInfo->SetBoolField(TEXT("has_end_ability"), bHasEndAbility);
	FlowInfo->SetNumberField(TEXT("ability_task_count"), AbilityTaskCount);
	FlowInfo->SetNumberField(TEXT("total_nodes"), NodeSummary.Num());
	Result->SetObjectField(TEXT("flow"), FlowInfo);

	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetNumberField(TEXT("warning_count"), Warnings.Num());
	Result->SetArrayField(TEXT("nodes"), NodeSummary);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 3: validate_ability
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleValidateAbility(const TSharedPtr<FJsonObject>& Params)
{
	FAbilityCDOContext Ctx;
	FMonolithActionResult Err;
	if (!Ctx.Load(Params, Err))
	{
		return Err;
	}

	TArray<TSharedPtr<FJsonValue>> Errors;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	// --- Graph analysis: CommitAbility / EndAbility reachability ---
	bool bHasCommitAbility = false;
	bool bHasEndAbility = false;
	bool bHasActivateAbility = false;
	int32 AbilityTaskCount = 0;
	TArray<FString> DanglingDelegates;

	for (UEdGraph* Graph : Ctx.BP->UbergraphPages)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;

			if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				FString FuncName = CallNode->FunctionReference.GetMemberName().ToString();
				if (FuncName.Contains(TEXT("CommitAbility"))) bHasCommitAbility = true;
				if (FuncName.Contains(TEXT("EndAbility")))    bHasEndAbility = true;
			}

			if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				FString EventName = EventNode->EventReference.GetMemberName().ToString();
				if (EventName.Contains(TEXT("ActivateAbility"))) bHasActivateAbility = true;
			}

			if (Node->IsA<UK2Node_LatentAbilityCall>())
			{
				AbilityTaskCount++;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && !Pin->bHidden &&
						Pin->Direction == EGPD_Output &&
						Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
						Pin->LinkedTo.Num() == 0)
					{
						DanglingDelegates.Add(FString::Printf(TEXT("%s.%s"),
							*Node->GetName(), *Pin->PinName.ToString()));
					}
				}
			}
		}
	}

	// Check: cost/cooldown exist but CommitAbility missing
	UGameplayEffect* CostGE = Ctx.CDO->GetCostGameplayEffect();
	UGameplayEffect* CooldownGE = Ctx.CDO->GetCooldownGameplayEffect();

	if (!bHasCommitAbility && (CostGE || CooldownGE))
	{
		TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
		ErrObj->SetStringField(TEXT("type"), TEXT("missing_commit_ability"));
		ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
		ErrObj->SetStringField(TEXT("message"),
			TEXT("Ability has cost/cooldown effect but no CommitAbility call. Cost/cooldown will never be applied."));
		Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
	}

	// Check: missing EndAbility (if non-trivial graph)
	if (!bHasEndAbility && bHasActivateAbility)
	{
		TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
		ErrObj->SetStringField(TEXT("type"), TEXT("missing_end_ability"));
		ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
		ErrObj->SetStringField(TEXT("message"),
			TEXT("No EndAbility call found. Ability will never terminate (leak)."));
		Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
	}

	// Check: dangling delegates
	for (const FString& Dangling : DanglingDelegates)
	{
		TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
		WarnObj->SetStringField(TEXT("type"), TEXT("dangling_delegate"));
		WarnObj->SetStringField(TEXT("severity"), TEXT("warning"));
		WarnObj->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Unhandled delegate output: %s"), *Dangling));
		Warnings.Add(MakeShared<FJsonValueObject>(WarnObj));
	}

	// Check: cooldown GE must be HasDuration type (GetCooldownGameplayEffect returns CDO directly)
	if (CooldownGE)
	{
		if (CooldownGE->DurationPolicy == EGameplayEffectDurationType::Instant)
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetStringField(TEXT("type"), TEXT("instant_cooldown_effect"));
			ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
			ErrObj->SetStringField(TEXT("message"),
				TEXT("CooldownGameplayEffectClass is an Instant GE. Cooldowns must use HasDuration."));
			Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
		}
	}

	// Check: contradictory tags (ActivationRequiredTags overlap with ActivationBlockedTags)
	FGameplayTagContainer* RequiredTags = GetTagContainerByName(Ctx.CDO, TEXT("ActivationRequiredTags"));
	FGameplayTagContainer* BlockedTags = GetTagContainerByName(Ctx.CDO, TEXT("ActivationBlockedTags"));
	if (RequiredTags && BlockedTags && RequiredTags->Num() > 0 && BlockedTags->Num() > 0)
	{
		FGameplayTagContainer Overlap = RequiredTags->Filter(*BlockedTags);
		if (Overlap.Num() > 0)
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetStringField(TEXT("type"), TEXT("contradictory_tags"));
			ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
			TArray<TSharedPtr<FJsonValue>> OverlapArr;
			for (const FGameplayTag& T : Overlap)
			{
				OverlapArr.Add(MakeShared<FJsonValueString>(T.ToString()));
			}
			ErrObj->SetArrayField(TEXT("tags"), OverlapArr);
			ErrObj->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Tags appear in both ActivationRequiredTags and ActivationBlockedTags — ability can never activate. Overlap: %s"), *Overlap.ToString()));
			Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
		}
	}

	// Check: NonInstanced policy (deprecated, causes issues with state)
	EGameplayAbilityInstancingPolicy::Type InstPolicy = Ctx.CDO->GetInstancingPolicy();
	if (IsLegacyNonInstancedPolicy(InstPolicy))
	{
		// Check if ability has any variables/state defined in BP
		bool bHasVariables = false;
		for (FBPVariableDescription& Var : Ctx.BP->NewVariables)
		{
			if (!Var.VarName.ToString().StartsWith(TEXT("__")) && Var.VarName != TEXT("UberGraphFrame"))
			{
				bHasVariables = true;
				break;
			}
		}
		if (bHasVariables)
		{
			TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
			WarnObj->SetStringField(TEXT("type"), TEXT("non_instanced_with_state"));
			WarnObj->SetStringField(TEXT("severity"), TEXT("warning"));
			WarnObj->SetStringField(TEXT("message"),
				TEXT("NonInstanced ability has Blueprint variables. NonInstanced abilities share a single instance — variables are not per-activation. Use InstancedPerActor or InstancedPerExecution."));
			Warnings.Add(MakeShared<FJsonValueObject>(WarnObj));
		}
	}

	// Build result
	bool bValid = (Errors.Num() == 0);
	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(Ctx.AssetPath,
		bValid ? TEXT("Ability passed validation") : TEXT("Ability has validation issues"));
	Result->SetBoolField(TEXT("valid"), bValid);
	Result->SetNumberField(TEXT("error_count"), Errors.Num());
	Result->SetNumberField(TEXT("warning_count"), Warnings.Num());
	if (Errors.Num() > 0) Result->SetArrayField(TEXT("errors"), Errors);
	if (Warnings.Num() > 0) Result->SetArrayField(TEXT("warnings"), Warnings);

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetBoolField(TEXT("has_activate_ability"), bHasActivateAbility);
	Summary->SetBoolField(TEXT("has_commit_ability"), bHasCommitAbility);
	Summary->SetBoolField(TEXT("has_end_ability"), bHasEndAbility);
	Summary->SetNumberField(TEXT("ability_task_count"), AbilityTaskCount);
	Summary->SetStringField(TEXT("instancing_policy"), InstancingPolicyToString(InstPolicy));
	Summary->SetBoolField(TEXT("has_cost_effect"), CostGE != nullptr);
	Summary->SetBoolField(TEXT("has_cooldown_effect"), CooldownGE != nullptr);
	Result->SetObjectField(TEXT("summary"), Summary);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 3: find_abilities_by_tag
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleFindAbilitiesByTag(const TSharedPtr<FJsonObject>& Params)
{
	FString TagStr;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("tag"), TagStr, Err)) return Err;

	FString MatchType = TEXT("exact");
	Params->TryGetStringField(TEXT("match_type"), MatchType);
	bool bPartial = MatchType.Equals(TEXT("partial"), ESearchCase::IgnoreCase);

	FGameplayTag SearchTag = FGameplayTag::RequestGameplayTag(FName(*TagStr), false);
	if (!bPartial && !SearchTag.IsValid())
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Tag '%s' is not registered. Use match_type='partial' for substring matching."), *TagStr));
	}

	// Scan all ability Blueprints via asset registry
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> AllBlueprints;
	AR.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBlueprints, true);

	TArray<TSharedPtr<FJsonValue>> Results;

	for (const FAssetData& Asset : AllBlueprints)
	{
		// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
		FAssetTagValueRef ParentTag = Asset.TagsAndValues.FindTag(FName("ParentClass"));
		FAssetTagValueRef NativeParentTag = Asset.TagsAndValues.FindTag(FName("NativeParentClass"));
		FString ParentPath = ParentTag.IsSet() ? ParentTag.GetValue() : (NativeParentTag.IsSet() ? NativeParentTag.GetValue() : TEXT(""));
		if (!ParentPath.Contains(TEXT("GameplayAbility")))
		{
			continue;
		}

		UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
		if (!BP || !MonolithGAS::IsAbilityBlueprint(BP)) continue;

		UGameplayAbility* CDO = MonolithGAS::GetBlueprintCDO<UGameplayAbility>(BP);
		if (!CDO) continue;

		FString AssetPath = Asset.GetObjectPathString();
		// Remove _C suffix if present
		AssetPath.RemoveFromEnd(TEXT("_C"));
		// Convert to package path
		if (AssetPath.Contains(TEXT(".")))
		{
			AssetPath = FPackageName::ObjectPathToPackageName(AssetPath);
		}

		// Check each container for tag match
		TArray<TSharedPtr<FJsonValue>> MatchedContainers;

		for (const FString& ContainerName : AllContainerNames)
		{
			FGameplayTagContainer* Container = GetTagContainerByName(CDO, ContainerName);
			if (!Container || Container->Num() == 0) continue;

			bool bMatched = false;
			if (bPartial)
			{
				for (const FGameplayTag& Tag : *Container)
				{
					if (Tag.ToString().Contains(TagStr))
					{
						bMatched = true;
						break;
					}
				}
			}
			else
			{
				bMatched = Container->HasTag(SearchTag);
			}

			if (bMatched)
			{
				MatchedContainers.Add(MakeShared<FJsonValueString>(ContainerName));
			}
		}

		if (MatchedContainers.Num() > 0)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("asset_path"), AssetPath);
			Entry->SetStringField(TEXT("name"), BP->GetName());
			Entry->SetArrayField(TEXT("matched_containers"), MatchedContainers);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("tag"), TagStr);
	Result->SetStringField(TEXT("match_type"), bPartial ? TEXT("partial") : TEXT("exact"));
	Result->SetNumberField(TEXT("count"), Results.Num());
	Result->SetArrayField(TEXT("abilities"), Results);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Found %d abilities matching tag '%s' (%s)"), Results.Num(), *TagStr, *MatchType));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 3: get_ability_tag_matrix
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleGetAbilityTagMatrix(const TSharedPtr<FJsonObject>& Params)
{
	// Collect ability BPs - either specified or all
	TArray<UBlueprint*> AbilityBPs;
	const TArray<TSharedPtr<FJsonValue>>* PathsArray = nullptr;

	if (Params->TryGetArrayField(TEXT("asset_paths"), PathsArray) && PathsArray && PathsArray->Num() > 0)
	{
		for (const auto& Val : *PathsArray)
		{
			FString PathStr;
			if (!Val->TryGetString(PathStr)) continue;

			FString Error, OutPath;
			TSharedPtr<FJsonObject> TempParams = MakeShared<FJsonObject>();
			TempParams->SetStringField(TEXT("asset_path"), PathStr);
			UBlueprint* BP = MonolithGAS::LoadBlueprintFromParams(TempParams, OutPath, Error);
			if (BP && MonolithGAS::IsAbilityBlueprint(BP))
			{
				AbilityBPs.Add(BP);
			}
		}
	}
	else
	{
		// Scan all abilities from asset registry
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> AllBlueprints;
		AR.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBlueprints, true);

		for (const FAssetData& Asset : AllBlueprints)
		{
			// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
			FAssetTagValueRef ParentTag = Asset.TagsAndValues.FindTag(FName("ParentClass"));
			FAssetTagValueRef NativeParentTag = Asset.TagsAndValues.FindTag(FName("NativeParentClass"));
			FString ParentPath = ParentTag.IsSet() ? ParentTag.GetValue() : (NativeParentTag.IsSet() ? NativeParentTag.GetValue() : TEXT(""));
			if (!ParentPath.Contains(TEXT("GameplayAbility")))
			{
				continue;
			}

			UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
			if (BP && MonolithGAS::IsAbilityBlueprint(BP))
			{
				AbilityBPs.Add(BP);
			}
		}
	}

	// Build the matrix: for each ability, extract AbilityTags, CancelAbilitiesWithTag, BlockAbilitiesWithTag
	struct FAbilityTagEntry
	{
		FString Path;
		FString Name;
		FGameplayTagContainer AbilityTags;
		FGameplayTagContainer CancelTags;
		FGameplayTagContainer BlockTags;
	};

	TArray<FAbilityTagEntry> Entries;
	for (UBlueprint* BP : AbilityBPs)
	{
		UGameplayAbility* CDO = MonolithGAS::GetBlueprintCDO<UGameplayAbility>(BP);
		if (!CDO) continue;

		FAbilityTagEntry Entry;
		Entry.Path = BP->GetPathName();
		Entry.Name = BP->GetName();

		if (FGameplayTagContainer* Tags = GetTagContainerByName(CDO, TEXT("AbilityTags")))
			Entry.AbilityTags = *Tags;
		if (FGameplayTagContainer* Tags = GetTagContainerByName(CDO, TEXT("CancelAbilitiesWithTag")))
			Entry.CancelTags = *Tags;
		if (FGameplayTagContainer* Tags = GetTagContainerByName(CDO, TEXT("BlockAbilitiesWithTag")))
			Entry.BlockTags = *Tags;

		Entries.Add(MoveTemp(Entry));
	}

	// Build relationships
	TArray<TSharedPtr<FJsonValue>> Relationships;
	TArray<TSharedPtr<FJsonValue>> CircularBlocks;

	for (int32 i = 0; i < Entries.Num(); i++)
	{
		for (int32 j = 0; j < Entries.Num(); j++)
		{
			if (i == j) continue;

			// Does ability i cancel ability j?
			if (Entries[i].CancelTags.Num() > 0 && Entries[j].AbilityTags.Num() > 0)
			{
				if (Entries[j].AbilityTags.HasAny(Entries[i].CancelTags))
				{
					TSharedPtr<FJsonObject> Rel = MakeShared<FJsonObject>();
					Rel->SetStringField(TEXT("source"), Entries[i].Name);
					Rel->SetStringField(TEXT("target"), Entries[j].Name);
					Rel->SetStringField(TEXT("type"), TEXT("cancels"));
					Relationships.Add(MakeShared<FJsonValueObject>(Rel));
				}
			}

			// Does ability i block ability j?
			if (Entries[i].BlockTags.Num() > 0 && Entries[j].AbilityTags.Num() > 0)
			{
				if (Entries[j].AbilityTags.HasAny(Entries[i].BlockTags))
				{
					TSharedPtr<FJsonObject> Rel = MakeShared<FJsonObject>();
					Rel->SetStringField(TEXT("source"), Entries[i].Name);
					Rel->SetStringField(TEXT("target"), Entries[j].Name);
					Rel->SetStringField(TEXT("type"), TEXT("blocks"));
					Relationships.Add(MakeShared<FJsonValueObject>(Rel));

					// Check for circular blocking
					if (Entries[j].BlockTags.Num() > 0 && Entries[i].AbilityTags.Num() > 0 &&
						Entries[i].AbilityTags.HasAny(Entries[j].BlockTags))
					{
						TSharedPtr<FJsonObject> Circ = MakeShared<FJsonObject>();
						Circ->SetStringField(TEXT("ability_a"), Entries[i].Name);
						Circ->SetStringField(TEXT("ability_b"), Entries[j].Name);
						Circ->SetStringField(TEXT("message"),
							FString::Printf(TEXT("%s and %s mutually block each other — neither can activate while the other is active"),
								*Entries[i].Name, *Entries[j].Name));
						// Avoid duplicates (i,j) and (j,i)
						if (i < j)
						{
							CircularBlocks.Add(MakeShared<FJsonValueObject>(Circ));
						}
					}
				}
			}
		}
	}

	// Serialize abilities summary
	TArray<TSharedPtr<FJsonValue>> AbilitySummary;
	for (const FAbilityTagEntry& E : Entries)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), E.Name);
		Obj->SetStringField(TEXT("path"), E.Path);
		Obj->SetField(TEXT("ability_tags"), ContainerToJson(E.AbilityTags));
		Obj->SetField(TEXT("cancel_tags"), ContainerToJson(E.CancelTags));
		Obj->SetField(TEXT("block_tags"), ContainerToJson(E.BlockTags));
		AbilitySummary.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("ability_count"), Entries.Num());
	Result->SetArrayField(TEXT("abilities"), AbilitySummary);
	Result->SetNumberField(TEXT("relationship_count"), Relationships.Num());
	Result->SetArrayField(TEXT("relationships"), Relationships);
	if (CircularBlocks.Num() > 0)
	{
		Result->SetArrayField(TEXT("circular_blocks"), CircularBlocks);
		Result->SetNumberField(TEXT("circular_block_count"), CircularBlocks.Num());
	}
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Tag matrix: %d abilities, %d relationships, %d circular blocks"),
			Entries.Num(), Relationships.Num(), CircularBlocks.Num()));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 3: validate_ability_blueprint
// ============================================================

FMonolithActionResult FMonolithGASAbilityActions::HandleValidateAbilityBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, Err)) return Err;

	FString Error, OutPath;
	TSharedPtr<FJsonObject> TempParams = MakeShared<FJsonObject>();
	TempParams->SetStringField(TEXT("asset_path"), AssetPath);
	UBlueprint* BP = MonolithGAS::LoadBlueprintFromParams(TempParams, OutPath, Error);
	if (!BP)
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Errors;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	bool bIsAbilityBP = MonolithGAS::IsAbilityBlueprint(BP);

	// Check: ability tasks in a non-ability Blueprint
	bool bHasAbilityTasks = false;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->IsA<UK2Node_LatentAbilityCall>())
			{
				bHasAbilityTasks = true;
				if (!bIsAbilityBP)
				{
					TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
					ErrObj->SetStringField(TEXT("type"), TEXT("task_in_wrong_bp"));
					ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
					ErrObj->SetStringField(TEXT("node"), Node->GetName());
					ErrObj->SetStringField(TEXT("message"),
						FString::Printf(TEXT("Ability task node '%s' placed in non-GameplayAbility Blueprint. Ability tasks only work in GameplayAbility subclasses."),
							*Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString()));
					Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
				}
			}
		}
	}

	// If not an ability BP, we just check for misplaced tasks
	if (!bIsAbilityBP)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), OutPath);
		Result->SetBoolField(TEXT("is_ability_blueprint"), false);
		Result->SetBoolField(TEXT("valid"), Errors.Num() == 0);
		Result->SetNumberField(TEXT("error_count"), Errors.Num());
		if (Errors.Num() > 0) Result->SetArrayField(TEXT("errors"), Errors);
		Result->SetStringField(TEXT("message"),
			Errors.Num() > 0
				? TEXT("Non-ability Blueprint has misplaced ability task nodes")
				: TEXT("Blueprint is not a GameplayAbility — no ability-specific validation applies"));
		return FMonolithActionResult::Success(Result);
	}

	UGameplayAbility* CDO = MonolithGAS::GetBlueprintCDO<UGameplayAbility>(BP);
	if (!CDO)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to get CDO for '%s'"), *OutPath));
	}

	// Scan all graphs for detailed validation
	bool bHasCommitAbility = false;
	bool bHasEndAbility = false;
	bool bHasActivateAbility = false;
	int32 TaskNodeCount = 0;
	TArray<FString> AllDanglingDelegates;

	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;

			if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				FString FuncName = CallNode->FunctionReference.GetMemberName().ToString();
				if (FuncName.Contains(TEXT("CommitAbility"))) bHasCommitAbility = true;
				if (FuncName.Contains(TEXT("EndAbility")))    bHasEndAbility = true;
			}

			if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				FString EventName = EventNode->EventReference.GetMemberName().ToString();
				if (EventName.Contains(TEXT("ActivateAbility"))) bHasActivateAbility = true;
			}

			if (Node->IsA<UK2Node_LatentAbilityCall>())
			{
				TaskNodeCount++;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && !Pin->bHidden &&
						Pin->Direction == EGPD_Output &&
						Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
						Pin->LinkedTo.Num() == 0)
					{
						AllDanglingDelegates.Add(FString::Printf(TEXT("%s.%s"),
							*Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString(),
							*Pin->PinName.ToString()));
					}
				}
			}
		}
	}

	// Check: missing EndAbility
	if (!bHasEndAbility && bHasActivateAbility)
	{
		TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
		ErrObj->SetStringField(TEXT("type"), TEXT("missing_end_ability"));
		ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
		ErrObj->SetStringField(TEXT("message"),
			TEXT("No EndAbility call found in any graph. The ability will never terminate."));
		Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
	}

	// Check: missing CommitAbility when cost/cooldown exists
	UGameplayEffect* CostGE = CDO->GetCostGameplayEffect();
	UGameplayEffect* CooldownGE = CDO->GetCooldownGameplayEffect();
	if (!bHasCommitAbility && (CostGE || CooldownGE))
	{
		TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
		ErrObj->SetStringField(TEXT("type"), TEXT("missing_commit_ability"));
		ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
		ErrObj->SetStringField(TEXT("message"),
			TEXT("Ability has cost/cooldown set but no CommitAbility node. Cost/cooldown will never apply."));
		Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
	}

	// Check: unhandled delegates
	for (const FString& Dangling : AllDanglingDelegates)
	{
		TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
		WarnObj->SetStringField(TEXT("type"), TEXT("unhandled_delegate"));
		WarnObj->SetStringField(TEXT("severity"), TEXT("warning"));
		WarnObj->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Unhandled task delegate: %s. Unhandled delegates can cause ability leaks if EndAbility is not called."), *Dangling));
		Warnings.Add(MakeShared<FJsonValueObject>(WarnObj));
	}

	// Check: NonInstanced with variables
	EGameplayAbilityInstancingPolicy::Type InstPolicy = CDO->GetInstancingPolicy();
	if (IsLegacyNonInstancedPolicy(InstPolicy) && BP->NewVariables.Num() > 0)
	{
		int32 UserVarCount = 0;
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			if (!Var.VarName.ToString().StartsWith(TEXT("__")) && Var.VarName != TEXT("UberGraphFrame"))
			{
				UserVarCount++;
			}
		}
		if (UserVarCount > 0)
		{
			TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
			WarnObj->SetStringField(TEXT("type"), TEXT("non_instanced_with_state"));
			WarnObj->SetStringField(TEXT("severity"), TEXT("warning"));
			WarnObj->SetStringField(TEXT("message"),
				FString::Printf(TEXT("NonInstanced ability has %d user variables. These are shared across all activations — use InstancedPerActor instead."), UserVarCount));
			Warnings.Add(MakeShared<FJsonValueObject>(WarnObj));
		}
	}

	// Check: contradictory tags
	FGameplayTagContainer* RequiredTags = GetTagContainerByName(CDO, TEXT("ActivationRequiredTags"));
	FGameplayTagContainer* BlockedTags = GetTagContainerByName(CDO, TEXT("ActivationBlockedTags"));
	if (RequiredTags && BlockedTags && RequiredTags->Num() > 0 && BlockedTags->Num() > 0)
	{
		FGameplayTagContainer Overlap = RequiredTags->Filter(*BlockedTags);
		if (Overlap.Num() > 0)
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetStringField(TEXT("type"), TEXT("contradictory_activation_tags"));
			ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
			ErrObj->SetStringField(TEXT("message"),
				FString::Printf(TEXT("ActivationRequiredTags and ActivationBlockedTags overlap on: %s. Ability can never activate."), *Overlap.ToString()));
			Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
		}
	}

	bool bValid = (Errors.Num() == 0);
	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(OutPath,
		bValid ? TEXT("Ability Blueprint passed validation") : TEXT("Ability Blueprint has issues"));
	Result->SetBoolField(TEXT("is_ability_blueprint"), true);
	Result->SetBoolField(TEXT("valid"), bValid);
	Result->SetNumberField(TEXT("error_count"), Errors.Num());
	Result->SetNumberField(TEXT("warning_count"), Warnings.Num());
	if (Errors.Num() > 0) Result->SetArrayField(TEXT("errors"), Errors);
	if (Warnings.Num() > 0) Result->SetArrayField(TEXT("warnings"), Warnings);

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetBoolField(TEXT("has_activate_ability"), bHasActivateAbility);
	Summary->SetBoolField(TEXT("has_commit_ability"), bHasCommitAbility);
	Summary->SetBoolField(TEXT("has_end_ability"), bHasEndAbility);
	Summary->SetNumberField(TEXT("task_node_count"), TaskNodeCount);
	Summary->SetNumberField(TEXT("dangling_delegate_count"), AllDanglingDelegates.Num());
	Summary->SetStringField(TEXT("instancing_policy"), InstancingPolicyToString(InstPolicy));
	Result->SetObjectField(TEXT("summary"), Summary);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  scaffold_custom_ability_task
// ============================================================

#include "Misc/FileHelper.h"

FMonolithActionResult FMonolithGASAbilityActions::HandleScaffoldCustomAbilityTask(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("class_name"), ClassName, Err))
		return Err;

	// Ensure U prefix
	FString FullClassName = ClassName;
	if (!FullClassName.StartsWith(TEXT("U")))
	{
		FullClassName = TEXT("U") + FullClassName;
	}
	// Strip U for the "short" name used in factory function and file names
	FString ShortName = FullClassName.Mid(1);

	// Parse parameters array
	struct FTaskParam
	{
		FString Name;
		FString Type;
	};
	TArray<FTaskParam> TaskParams;

	const TArray<TSharedPtr<FJsonValue>>* ParamsArr = nullptr;
	if (Params->TryGetArrayField(TEXT("parameters"), ParamsArr))
	{
		for (const TSharedPtr<FJsonValue>& Val : *ParamsArr)
		{
			TSharedPtr<FJsonObject> Obj = Val->AsObject();
			if (!Obj) continue;
			FTaskParam P;
			P.Name = Obj->GetStringField(TEXT("name"));
			P.Type = Obj->GetStringField(TEXT("type"));
			if (!P.Name.IsEmpty() && !P.Type.IsEmpty())
			{
				TaskParams.Add(P);
			}
		}
	}

	// Parse delegates array
	struct FTaskDelegate
	{
		FString Name;
		FString DelegateParams; // Optional extra params for the delegate signature
	};
	TArray<FTaskDelegate> TaskDelegates;

	const TArray<TSharedPtr<FJsonValue>>* DelegatesArr = nullptr;
	if (Params->TryGetArrayField(TEXT("delegates"), DelegatesArr))
	{
		for (const TSharedPtr<FJsonValue>& Val : *DelegatesArr)
		{
			TSharedPtr<FJsonObject> Obj = Val->AsObject();
			if (!Obj) continue;
			FTaskDelegate D;
			D.Name = Obj->GetStringField(TEXT("name"));
			D.DelegateParams = Obj->GetStringField(TEXT("params"));
			if (!D.Name.IsEmpty())
			{
				TaskDelegates.Add(D);
			}
		}
	}

	if (TaskDelegates.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("At least one delegate is required for a custom ability task."));
	}

	// ── Build the GENERATED_UCLASS_BODY API string for the module ──
	FString ModuleAPI = TEXT("LEVIATHAN_API");

	// ── Generate Header ──
	FString Header;
	Header += TEXT("#pragma once\n\n");
	Header += TEXT("#include \"CoreMinimal.h\"\n");
	Header += TEXT("#include \"Abilities/Tasks/AbilityTask.h\"\n");
	Header += FString::Printf(TEXT("#include \"%s.generated.h\"\n\n"), *ShortName);

	// Delegate declarations
	for (const FTaskDelegate& Del : TaskDelegates)
	{
		FString DelegateMacroName = FString::Printf(TEXT("F%s_%s"), *ShortName, *Del.Name);
		if (Del.DelegateParams.IsEmpty())
		{
			Header += FString::Printf(TEXT("DECLARE_DYNAMIC_MULTICAST_DELEGATE(%s);\n"), *DelegateMacroName);
		}
		else
		{
			Header += FString::Printf(TEXT("DECLARE_DYNAMIC_MULTICAST_DELEGATE_%s(%s);\n"),
				*Del.DelegateParams, *DelegateMacroName);
		}
	}

	Header += TEXT("\n/**\n");
	Header += FString::Printf(TEXT(" * %s - Custom Ability Task\n"), *ShortName);
	Header += TEXT(" * Auto-generated by MonolithGAS scaffold_custom_ability_task\n");
	Header += TEXT(" */\n");
	Header += TEXT("UCLASS()\n");
	Header += FString::Printf(TEXT("class %s %s : public UAbilityTask\n"), *ModuleAPI, *FullClassName);
	Header += TEXT("{\n");
	Header += TEXT("\tGENERATED_BODY()\n\n");
	Header += TEXT("public:\n");

	// Delegate UPROPERTY members
	for (const FTaskDelegate& Del : TaskDelegates)
	{
		FString DelegateMacroName = FString::Printf(TEXT("F%s_%s"), *ShortName, *Del.Name);
		Header += FString::Printf(TEXT("\tUPROPERTY(BlueprintAssignable)\n"));
		Header += FString::Printf(TEXT("\t%s %s;\n\n"), *DelegateMacroName, *Del.Name);
	}

	// Factory static function
	Header += FString::Printf(TEXT("\tUFUNCTION(BlueprintCallable, Category = \"Ability|Tasks\", meta = (HidePin = \"OwningAbility\", DefaultToSelf = \"OwningAbility\", BlueprintInternalUseOnly = \"true\"))\n"));
	Header += FString::Printf(TEXT("\tstatic %s* Create%s(UGameplayAbility* OwningAbility"), *FullClassName, *ShortName);
	for (const FTaskParam& P : TaskParams)
	{
		Header += FString::Printf(TEXT(", %s %s"), *P.Type, *P.Name);
	}
	Header += TEXT(");\n\n");

	Header += TEXT("protected:\n");
	Header += TEXT("\tvirtual void Activate() override;\n");
	Header += TEXT("\tvirtual void OnDestroy(bool bInOwnerFinished) override;\n\n");

	// Member variables for stored params
	if (TaskParams.Num() > 0)
	{
		Header += TEXT("private:\n");
		for (const FTaskParam& P : TaskParams)
		{
			Header += FString::Printf(TEXT("\t%s Stored_%s;\n"), *P.Type, *P.Name);
		}
	}

	Header += TEXT("};\n");

	// ── Generate Source ──
	FString Source;
	Source += FString::Printf(TEXT("#include \"%s.h\"\n\n"), *ShortName);

	// Factory implementation
	Source += FString::Printf(TEXT("%s* %s::Create%s(UGameplayAbility* OwningAbility"),
		*FullClassName, *FullClassName, *ShortName);
	for (const FTaskParam& P : TaskParams)
	{
		Source += FString::Printf(TEXT(", %s %s"), *P.Type, *P.Name);
	}
	Source += TEXT(")\n{\n");
	Source += FString::Printf(TEXT("\t%s* Task = NewAbilityTask<%s>(OwningAbility);\n"), *FullClassName, *FullClassName);
	for (const FTaskParam& P : TaskParams)
	{
		Source += FString::Printf(TEXT("\tTask->Stored_%s = %s;\n"), *P.Name, *P.Name);
	}
	Source += TEXT("\treturn Task;\n");
	Source += TEXT("}\n\n");

	// Activate
	Source += FString::Printf(TEXT("void %s::Activate()\n"), *FullClassName);
	Source += TEXT("{\n");
	Source += TEXT("\tSuper::Activate();\n\n");
	Source += TEXT("\t// TODO: Implement task activation logic\n");
	Source += TEXT("\t// Call delegate.Broadcast() when task completes\n");
	if (TaskDelegates.Num() > 0)
	{
		Source += FString::Printf(TEXT("\t// Example: %s.Broadcast();\n"), *TaskDelegates[0].Name);
	}
	Source += TEXT("}\n\n");

	// OnDestroy
	Source += FString::Printf(TEXT("void %s::OnDestroy(bool bInOwnerFinished)\n"), *FullClassName);
	Source += TEXT("{\n");
	Source += TEXT("\t// TODO: Clean up any bound delegates or timers\n\n");
	Source += TEXT("\tSuper::OnDestroy(bInOwnerFinished);\n");
	Source += TEXT("}\n");

	// ── Write files ──
	FString ProjectSourceDir = FPaths::ProjectDir() / TEXT("Source") / TEXT("Leviathan");
	FString HeaderPath = ProjectSourceDir / FString::Printf(TEXT("%s.h"), *ShortName);
	FString SourcePath = ProjectSourceDir / FString::Printf(TEXT("%s.cpp"), *ShortName);

	bool bHeaderWritten = FFileHelper::SaveStringToFile(Header, *HeaderPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	bool bSourceWritten = FFileHelper::SaveStringToFile(Source, *SourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	if (!bHeaderWritten || !bSourceWritten)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to write files. Header: %s (%s), Source: %s (%s)"),
			*HeaderPath, bHeaderWritten ? TEXT("OK") : TEXT("FAILED"),
			*SourcePath, bSourceWritten ? TEXT("OK") : TEXT("FAILED")));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("class_name"), FullClassName);
	Result->SetStringField(TEXT("header_path"), HeaderPath);
	Result->SetStringField(TEXT("source_path"), SourcePath);
	Result->SetNumberField(TEXT("parameter_count"), TaskParams.Num());
	Result->SetNumberField(TEXT("delegate_count"), TaskDelegates.Num());
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Generated custom ability task %s. Rebuild required to use."), *FullClassName));

	// List what was generated
	TArray<TSharedPtr<FJsonValue>> DelegateNames;
	for (const FTaskDelegate& Del : TaskDelegates)
	{
		DelegateNames.Add(MakeShared<FJsonValueString>(Del.Name));
	}
	Result->SetArrayField(TEXT("delegates"), DelegateNames);

	TArray<TSharedPtr<FJsonValue>> ParamNames;
	for (const FTaskParam& P : TaskParams)
	{
		TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
		PObj->SetStringField(TEXT("name"), P.Name);
		PObj->SetStringField(TEXT("type"), P.Type);
		ParamNames.Add(MakeShared<FJsonValueObject>(PObj));
	}
	Result->SetArrayField(TEXT("parameters"), ParamNames);

	return FMonolithActionResult::Success(Result);
}
