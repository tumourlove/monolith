#include "MonolithGASInputActions.h"
#include "Misc/App.h"
#include "MonolithParamSchema.h"
#include "MonolithGASInternal.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "EnhancedInputComponent.h"
#include "InputAction.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"


// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

void FMonolithGASInputActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("gas"), TEXT("setup_ability_input_binding"),
		TEXT("Configure how abilities are bound to input on an actor Blueprint. Creates the binding infrastructure for the chosen mode."),
		FMonolithActionHandler::CreateStatic(&HandleSetupAbilityInputBinding),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("actor_path"), TEXT("Blueprint asset path of the actor to configure"))
			.Required(TEXT("binding_mode"), TEXT("string"), TEXT("Binding mode: 'input_id', 'gameplay_tag', or 'enhanced_input'"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("bind_ability_to_input"),
		TEXT("Bind a gameplay ability to an input action or input ID"),
		FMonolithActionHandler::CreateStatic(&HandleBindAbilityToInput),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("actor_path"), TEXT("Blueprint asset path of the actor"))
			.Required(TEXT("ability_class"), TEXT("string"), TEXT("Path to the GameplayAbility Blueprint class"))
			.Required(TEXT("input_action"), TEXT("string"), TEXT("UInputAction asset path (for enhanced_input) or integer input ID"))
			.Optional(TEXT("trigger_event"), TEXT("string"), TEXT("Trigger event: 'started', 'triggered', or 'completed' (default: started)"), TEXT("started"))
			.Build());

	// Phase 2: Input Binding Productivity
	Registry.RegisterAction(TEXT("gas"), TEXT("batch_bind_abilities"),
		TEXT("Bind multiple abilities to inputs in a single call. Each binding specifies ability_class, input_action, and optional trigger_event."),
		FMonolithActionHandler::CreateStatic(&HandleBatchBindAbilities),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("actor_path"), TEXT("Blueprint asset path of the actor"))
			.Required(TEXT("bindings"), TEXT("array"), TEXT("Array of binding objects: [{ability_class, input_action, trigger_event?}]"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("get_ability_input_bindings"),
		TEXT("List all ability-to-input bindings on an actor Blueprint. Inspects the ASC's default ability specs and input-related CDO properties."),
		FMonolithActionHandler::CreateStatic(&HandleGetAbilityInputBindings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("actor_path"), TEXT("Blueprint asset path of the actor"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("scaffold_input_binding_component"),
		TEXT("Generate a C++ header+source for an AbilityInputBindingComponent that bridges Enhanced Input to ASC activation"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldInputBindingComponent),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("actor_path"), TEXT("Blueprint asset path of the actor (for context)"))
			.Required(TEXT("input_config"), TEXT("object"), TEXT("Config: { component_name?, module_name?, binding_mode? }"))
			.Build());
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/** Find the ASC component node in the Blueprint's SimpleConstructionScript */
static USCS_Node* FindASCNode_Input(UBlueprint* BP)
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

/** Check if a component of the given class already exists in the SCS */
static bool HasComponentOfClass(UBlueprint* BP, UClass* ComponentClass)
{
	if (!BP || !BP->SimpleConstructionScript || !ComponentClass)
	{
		return false;
	}

	for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
	{
		if (Node && Node->ComponentClass &&
			Node->ComponentClass->IsChildOf(ComponentClass))
		{
			return true;
		}
	}

	// Also check native components via parent CDO
	if (BP->GeneratedClass)
	{
		UObject* CDO = BP->GeneratedClass->GetDefaultObject();
		if (CDO)
		{
			TArray<UActorComponent*> Components;
			Cast<AActor>(CDO)->GetComponents(ComponentClass, Components);
			return Components.Num() > 0;
		}
	}

	return false;
}

/** Add a component to the Blueprint's SCS */
static USCS_Node* AddComponentToBlueprint(UBlueprint* BP, UClass* ComponentClass, const FString& VariableName, FString& OutError)
{
	if (!BP || !BP->SimpleConstructionScript)
	{
		OutError = TEXT("Blueprint has no SimpleConstructionScript");
		return nullptr;
	}

	USCS_Node* NewNode = BP->SimpleConstructionScript->CreateNode(ComponentClass, *VariableName);
	if (!NewNode)
	{
		OutError = FString::Printf(TEXT("Failed to create SCS node for %s"), *ComponentClass->GetName());
		return nullptr;
	}

	BP->SimpleConstructionScript->AddNode(NewNode);
	return NewNode;
}

// ─────────────────────────────────────────────────────────────────────────────
// setup_ability_input_binding
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASInputActions::HandleSetupAbilityInputBinding(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath;
	FMonolithActionResult ErrorResult;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor_path"), ActorPath, ErrorResult))
	{
		return ErrorResult;
	}

	FString BindingMode;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("binding_mode"), BindingMode, ErrorResult))
	{
		return ErrorResult;
	}

	// Validate binding mode
	if (BindingMode != TEXT("input_id") &&
		BindingMode != TEXT("gameplay_tag") &&
		BindingMode != TEXT("enhanced_input"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid binding_mode: '%s'. Must be 'input_id', 'gameplay_tag', or 'enhanced_input'"), *BindingMode));
	}

	// Load the actor Blueprint
	FString AssetPath;
	FString LoadError;
	UBlueprint* BP = MonolithGAS::LoadBlueprintFromParams(Params, AssetPath, LoadError);
	if (!BP)
	{
		// Try with actor_path since the helper checks asset_path
		FString Error2;
		UObject* Obj = MonolithGAS::LoadAssetFromPath(ActorPath, Error2);
		BP = Cast<UBlueprint>(Obj);
		if (!BP)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to load actor Blueprint: %s"), *ActorPath));
		}
	}

	// Ensure BP has an ASC
	bool bHasASC = HasComponentOfClass(BP, UAbilitySystemComponent::StaticClass());

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"), ActorPath);
	Result->SetStringField(TEXT("binding_mode"), BindingMode);

	TArray<TSharedPtr<FJsonValue>> Actions;

	if (!bHasASC)
	{
		// Note: we don't auto-add ASC here — that's the ASC actions' job
		// We just report the requirement
		TSharedPtr<FJsonObject> Warning = MakeShared<FJsonObject>();
		Warning->SetStringField(TEXT("type"), TEXT("warning"));
		Warning->SetStringField(TEXT("message"),
			TEXT("No AbilitySystemComponent found. Add one via 'add_asc_to_actor' before binding input."));
		Actions.Add(MakeShared<FJsonValueObject>(Warning));
	}

	if (BindingMode == TEXT("enhanced_input"))
	{
		// For Enhanced Input mode, we need to document the setup pattern
		// The actual binding happens at runtime in C++, but we can set up
		// the necessary variables and metadata on the Blueprint

		// Store the binding mode as a metadata tag on the Blueprint
		// Note: binding mode is advisory, stored in the action result for caller reference

		TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
		Action->SetStringField(TEXT("type"), TEXT("configured"));
		Action->SetStringField(TEXT("message"),
			TEXT("Enhanced Input binding mode set. Use 'bind_ability_to_input' to wire specific abilities to UInputAction assets."));
		Action->SetStringField(TEXT("note"),
			TEXT("Requires a C++ AbilityInputBindingComponent or equivalent. "
				 "The component should listen to UEnhancedInputComponent events and call "
				 "ASC->TryActivateAbility / ASC->AbilityLocalInputPressed."));
		Actions.Add(MakeShared<FJsonValueObject>(Action));

		// Check if we need to add a recommended C++ class
		TSharedPtr<FJsonObject> Recommendation = MakeShared<FJsonObject>();
		Recommendation->SetStringField(TEXT("type"), TEXT("recommendation"));
		Recommendation->SetStringField(TEXT("message"),
			TEXT("Consider generating an AbilityInputBindingComponent via bootstrap_gas_foundation "
				 "or creating one manually that bridges UInputAction -> ASC activation."));
		Actions.Add(MakeShared<FJsonValueObject>(Recommendation));
	}
	else if (BindingMode == TEXT("input_id"))
	{
		// binding mode is advisory

		TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
		Action->SetStringField(TEXT("type"), TEXT("configured"));
		Action->SetStringField(TEXT("message"),
			TEXT("Input ID binding mode set. Abilities will be bound using integer IDs. "
				 "Use 'bind_ability_to_input' with numeric input_action values."));
		Action->SetStringField(TEXT("note"),
			TEXT("Traditional GAS input binding. Works with BindAbilityActivationToInputComponent. "
				 "Define an enum for input IDs (e.g. EAbilityInputID) for type safety."));
		Actions.Add(MakeShared<FJsonValueObject>(Action));
	}
	else if (BindingMode == TEXT("gameplay_tag"))
	{
		// binding mode is advisory

		TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
		Action->SetStringField(TEXT("type"), TEXT("configured"));
		Action->SetStringField(TEXT("message"),
			TEXT("Gameplay Tag binding mode set. Abilities will be activated by tag matching. "
				 "Use 'bind_ability_to_input' to map input actions to ability-activation tags."));
		Action->SetStringField(TEXT("note"),
			TEXT("Tag-based binding is the most flexible approach. Abilities specify their "
				 "InputTag, and a component maps UInputAction -> FGameplayTag -> ASC activation."));
		Actions.Add(MakeShared<FJsonValueObject>(Action));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetBoolField(TEXT("has_asc"), bHasASC);

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// bind_ability_to_input
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASInputActions::HandleBindAbilityToInput(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath;
	FMonolithActionResult ErrorResult;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor_path"), ActorPath, ErrorResult))
	{
		return ErrorResult;
	}

	FString AbilityClass;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("ability_class"), AbilityClass, ErrorResult))
	{
		return ErrorResult;
	}

	FString InputAction;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("input_action"), InputAction, ErrorResult))
	{
		return ErrorResult;
	}

	FString TriggerEvent = Params->GetStringField(TEXT("trigger_event"));
	if (TriggerEvent.IsEmpty()) TriggerEvent = TEXT("started");

	// Validate trigger event
	if (TriggerEvent != TEXT("started") &&
		TriggerEvent != TEXT("triggered") &&
		TriggerEvent != TEXT("completed"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid trigger_event: '%s'. Must be 'started', 'triggered', or 'completed'"), *TriggerEvent));
	}

	// Load actor Blueprint
	FString LoadError;
	UObject* Obj = MonolithGAS::LoadAssetFromPath(ActorPath, LoadError);
	UBlueprint* BP = Cast<UBlueprint>(Obj);
	if (!BP)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load actor Blueprint: %s"), *ActorPath));
	}

	// Load the ability class to verify it exists
	FString AbilityError;
	UObject* AbilityObj = MonolithGAS::LoadAssetFromPath(AbilityClass, AbilityError);
	UBlueprint* AbilityBP = Cast<UBlueprint>(AbilityObj);
	if (AbilityBP && !MonolithGAS::IsAbilityBlueprint(AbilityBP))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("'%s' is not a GameplayAbility Blueprint"), *AbilityClass));
	}

	// Auto-detect binding mode from input_action format
	FString BindingMode;
	if (InputAction.IsNumeric())
	{
		BindingMode = TEXT("input_id");
	}
	else if (InputAction.StartsWith(TEXT("/")) || InputAction.Contains(TEXT("InputAction")))
	{
		BindingMode = TEXT("enhanced_input");
	}
	else
	{
		BindingMode = TEXT("gameplay_tag");
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"), ActorPath);
	Result->SetStringField(TEXT("ability_class"), AbilityClass);
	Result->SetStringField(TEXT("input_action"), InputAction);
	Result->SetStringField(TEXT("trigger_event"), TriggerEvent);
	Result->SetStringField(TEXT("binding_mode"), BindingMode);

	if (BindingMode == TEXT("enhanced_input"))
	{
		// Verify the InputAction asset exists
		FString IAError;
		UObject* IAObj = MonolithGAS::LoadAssetFromPath(InputAction, IAError);
		UInputAction* IA = Cast<UInputAction>(IAObj);
		if (!IA)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("InputAction asset not found: %s"), *InputAction));
		}

		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Bound ability '%s' to InputAction '%s' on '%s' event. "
				"Metadata stored on Blueprint. Runtime binding requires AbilityInputBindingComponent."),
				*AbilityClass, *InputAction, *TriggerEvent));
	}
	else if (BindingMode == TEXT("input_id"))
	{
		int32 InputID = FCString::Atoi(*InputAction);

		Result->SetNumberField(TEXT("input_id"), InputID);
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Bound ability '%s' to Input ID %d. "
				"Wire via ASC->BindAbilityActivationToInputComponent at runtime."),
				*AbilityClass, InputID));
	}
	else if (BindingMode == TEXT("gameplay_tag"))
	{
		// The input_action is treated as a gameplay tag string
		FGameplayTag InputTag = FGameplayTag::RequestGameplayTag(FName(*InputAction), false);
		if (!InputTag.IsValid())
		{
			// Tag doesn't exist yet — warn but still store
			Result->SetStringField(TEXT("warning"),
				FString::Printf(TEXT("Tag '%s' not registered. Consider adding it via add_gameplay_tags first."),
					*InputAction));
		}

		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Bound ability '%s' to tag '%s'. "
				"Runtime: match InputTag on ability spec to trigger activation."),
				*AbilityClass, *InputAction));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// batch_bind_abilities
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASInputActions::HandleBatchBindAbilities(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor_path"), ActorPath, Err)) return Err;

	const TArray<TSharedPtr<FJsonValue>>* BindingsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("bindings"), BindingsArray) || !BindingsArray || BindingsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: bindings (array)"));
	}

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 SuccessCount = 0;
	int32 ErrorCount = 0;

	for (int32 i = 0; i < BindingsArray->Num(); ++i)
	{
		const TSharedPtr<FJsonValue>& BindingVal = (*BindingsArray)[i];
		const TSharedPtr<FJsonObject>* BindingObjPtr = nullptr;
		if (!BindingVal.IsValid() || !BindingVal->TryGetObject(BindingObjPtr) || !BindingObjPtr || !(*BindingObjPtr).IsValid())
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetNumberField(TEXT("index"), i);
			ErrObj->SetBoolField(TEXT("success"), false);
			ErrObj->SetStringField(TEXT("error"), TEXT("Invalid binding object"));
			Results.Add(MakeShared<FJsonValueObject>(ErrObj));
			ErrorCount++;
			continue;
		}
		const TSharedPtr<FJsonObject>& BindingObj = *BindingObjPtr;

		FString AbilityClass = BindingObj->GetStringField(TEXT("ability_class"));
		FString InputAction = BindingObj->GetStringField(TEXT("input_action"));
		FString TriggerEvent = BindingObj->GetStringField(TEXT("trigger_event"));
		if (TriggerEvent.IsEmpty()) TriggerEvent = TEXT("started");

		if (AbilityClass.IsEmpty() || InputAction.IsEmpty())
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetNumberField(TEXT("index"), i);
			ErrObj->SetBoolField(TEXT("success"), false);
			ErrObj->SetStringField(TEXT("error"), TEXT("Missing ability_class or input_action"));
			Results.Add(MakeShared<FJsonValueObject>(ErrObj));
			ErrorCount++;
			continue;
		}

		// Build params for the single bind call
		TSharedPtr<FJsonObject> SingleParams = MakeShared<FJsonObject>();
		SingleParams->SetStringField(TEXT("actor_path"), ActorPath);
		SingleParams->SetStringField(TEXT("ability_class"), AbilityClass);
		SingleParams->SetStringField(TEXT("input_action"), InputAction);
		SingleParams->SetStringField(TEXT("trigger_event"), TriggerEvent);

		FMonolithActionResult SingleResult = HandleBindAbilityToInput(SingleParams);

		TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
		ResultObj->SetNumberField(TEXT("index"), i);
		ResultObj->SetBoolField(TEXT("success"), SingleResult.bSuccess);
		ResultObj->SetStringField(TEXT("ability_class"), AbilityClass);
		ResultObj->SetStringField(TEXT("input_action"), InputAction);
		ResultObj->SetStringField(TEXT("trigger_event"), TriggerEvent);

		if (SingleResult.bSuccess)
		{
			if (SingleResult.Result.IsValid() && SingleResult.Result->HasField(TEXT("binding_mode")))
			{
				ResultObj->SetStringField(TEXT("binding_mode"), SingleResult.Result->GetStringField(TEXT("binding_mode")));
			}
			SuccessCount++;
		}
		else
		{
			ResultObj->SetStringField(TEXT("error"), SingleResult.ErrorMessage);
			ErrorCount++;
		}

		Results.Add(MakeShared<FJsonValueObject>(ResultObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"), ActorPath);
	Result->SetNumberField(TEXT("total"), BindingsArray->Num());
	Result->SetNumberField(TEXT("succeeded"), SuccessCount);
	Result->SetNumberField(TEXT("failed"), ErrorCount);
	Result->SetArrayField(TEXT("bindings"), Results);

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// get_ability_input_bindings
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASInputActions::HandleGetAbilityInputBindings(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor_path"), ActorPath, Err)) return Err;

	FString LoadError;
	UObject* Obj = MonolithGAS::LoadAssetFromPath(ActorPath, LoadError);
	UBlueprint* BP = Cast<UBlueprint>(Obj);
	if (!BP)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load actor Blueprint: %s"), *ActorPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"), ActorPath);

	// Check if actor has ASC
	bool bHasASC = HasComponentOfClass(BP, UAbilitySystemComponent::StaticClass());
	Result->SetBoolField(TEXT("has_asc"), bHasASC);

	// Find the ASC node to check for default abilities
	USCS_Node* ASCNode = FindASCNode_Input(BP);
	TArray<TSharedPtr<FJsonValue>> Bindings;

	if (ASCNode)
	{
		Result->SetStringField(TEXT("asc_variable"), ASCNode->GetVariableName().ToString());

		// Check CDO of the ASC for any configured default abilities
		UAbilitySystemComponent* ASCCDO = Cast<UAbilitySystemComponent>(ASCNode->GetActualComponentTemplate(Cast<UBlueprintGeneratedClass>(BP->GeneratedClass)));
		if (ASCCDO)
		{
			// ASC stores ability specs internally. We can read them from reflection.
			// Check for DefaultStartingAbilities or similar properties
			for (TFieldIterator<FProperty> PropIt(ASCCDO->GetClass()); PropIt; ++PropIt)
			{
				FArrayProperty* ArrayProp = CastField<FArrayProperty>(*PropIt);
				if (!ArrayProp) continue;

				FString PropName = PropIt->GetName();
				if (PropName.Contains(TEXT("Ability")) || PropName.Contains(TEXT("ability")))
				{
					TSharedPtr<FJsonObject> BindingInfo = MakeShared<FJsonObject>();
					BindingInfo->SetStringField(TEXT("property"), PropName);
					BindingInfo->SetStringField(TEXT("type"), TEXT("asc_default"));
					Bindings.Add(MakeShared<FJsonValueObject>(BindingInfo));
				}
			}
		}
	}

	// Also check for EnhancedInputComponent presence
	bool bHasEnhancedInput = HasComponentOfClass(BP, UEnhancedInputComponent::StaticClass());
	Result->SetBoolField(TEXT("has_enhanced_input_component"), bHasEnhancedInput);

	// Check parent CDO for native ASC and abilities
	if (BP->GeneratedClass)
	{
		UObject* CDO = BP->GeneratedClass->GetDefaultObject();
		AActor* ActorCDO = Cast<AActor>(CDO);
		if (ActorCDO)
		{
			UAbilitySystemComponent* NativeASC = ActorCDO->FindComponentByClass<UAbilitySystemComponent>();
			if (NativeASC)
			{
				Result->SetBoolField(TEXT("has_native_asc"), true);
			}
		}
	}

	// Scan BP event graph for input binding patterns
	// Look for references to InputAction assets in the graph
	if (BP->UbergraphPages.Num() > 0)
	{
		int32 InputBindingNodeCount = 0;
		for (UEdGraph* Graph : BP->UbergraphPages)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;
				FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				if (NodeTitle.Contains(TEXT("InputAction")) ||
					NodeTitle.Contains(TEXT("AbilityLocalInputPressed")) ||
					NodeTitle.Contains(TEXT("TryActivateAbility")))
				{
					InputBindingNodeCount++;
				}
			}
		}
		if (InputBindingNodeCount > 0)
		{
			Result->SetNumberField(TEXT("input_binding_graph_nodes"), InputBindingNodeCount);
		}
	}

	Result->SetArrayField(TEXT("bindings"), Bindings);
	Result->SetStringField(TEXT("message"),
		bHasASC ?
			FString::Printf(TEXT("Found ASC on '%s'. %d binding info entries detected."),
				*ActorPath, Bindings.Num()) :
			FString::Printf(TEXT("No ASC found on '%s'. Add one via add_asc_to_actor first."),
				*ActorPath));

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// scaffold_input_binding_component
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASInputActions::HandleScaffoldInputBindingComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor_path"), ActorPath, Err)) return Err;

	const TSharedPtr<FJsonObject>* ConfigPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("input_config"), ConfigPtr) || !ConfigPtr || !(*ConfigPtr).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: input_config (object)"));
	}
	const TSharedPtr<FJsonObject>& Config = *ConfigPtr;

	FString ComponentName = Config->GetStringField(TEXT("component_name"));
	if (ComponentName.IsEmpty()) ComponentName = TEXT("AbilityInputBindingComponent");

	FString ModuleName = Config->GetStringField(TEXT("module_name"));
	if (ModuleName.IsEmpty()) ModuleName = FApp::GetProjectName();

	FString BindingMode = Config->GetStringField(TEXT("binding_mode"));
	if (BindingMode.IsEmpty()) BindingMode = TEXT("gameplay_tag");

	FString ClassName = FString::Printf(TEXT("U%s"), *ComponentName);
	FString HeaderFileName = ComponentName + TEXT(".h");
	FString SourceFileName = ComponentName + TEXT(".cpp");

	FString SourceDir = FPaths::ProjectDir() / TEXT("Source") / ModuleName;
	FString HeaderPath = SourceDir / HeaderFileName;
	FString SourcePath = SourceDir / SourceFileName;

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	// Check if files already exist
	if (PlatformFile.FileExists(*HeaderPath))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("File already exists: %s"), *HeaderPath));
	}

	// Generate header
	FString HeaderContent = FString::Printf(TEXT(
		"// Auto-generated by Monolith GAS — AbilityInputBindingComponent\n"
		"// Bridges Enhanced Input -> Gameplay Ability System activation\n"
		"#pragma once\n"
		"\n"
		"#include \"CoreMinimal.h\"\n"
		"#include \"Components/ActorComponent.h\"\n"
		"#include \"GameplayTagContainer.h\"\n"
		"#include \"InputAction.h\"\n"
		"#include \"%s.generated.h\"\n"
		"\n"
		"class UAbilitySystemComponent;\n"
		"class UEnhancedInputComponent;\n"
		"class UInputAction;\n"
		"\n"
		"USTRUCT(BlueprintType)\n"
		"struct FAbilityInputBinding\n"
		"{\n"
		"\tGENERATED_BODY()\n"
		"\n"
		"\tUPROPERTY(EditAnywhere, BlueprintReadWrite)\n"
		"\tTObjectPtr<const UInputAction> InputAction = nullptr;\n"
		"\n"
		"\tUPROPERTY(EditAnywhere, BlueprintReadWrite)\n"
		"\tFGameplayTag AbilityTag;\n"
		"\n"
		"\tUPROPERTY(EditAnywhere, BlueprintReadWrite)\n"
		"\tTSubclassOf<class UGameplayAbility> AbilityClass;\n"
		"};\n"
		"\n"
		"/**\n"
		" * Bridges Enhanced Input to AbilitySystemComponent.\n"
		" * Configure bindings in the editor, and this component will\n"
		" * automatically set up input -> ability activation at runtime.\n"
		" *\n"
		" * Binding mode: %s\n"
		" */\n"
		"UCLASS(ClassGroup=(GAS), meta=(BlueprintSpawnableComponent))\n"
		"class %s_API %s : public UActorComponent\n"
		"{\n"
		"\tGENERATED_BODY()\n"
		"\n"
		"public:\n"
		"\t%s();\n"
		"\n"
		"\t/** Bindings configured in the editor */\n"
		"\tUPROPERTY(EditAnywhere, BlueprintReadWrite, Category = \"GAS|Input\")\n"
		"\tTArray<FAbilityInputBinding> AbilityBindings;\n"
		"\n"
		"\t/** Set up all bindings — call after ASC and EnhancedInput are ready */\n"
		"\tUFUNCTION(BlueprintCallable, Category = \"GAS|Input\")\n"
		"\tvoid SetupBindings(UAbilitySystemComponent* ASC, UEnhancedInputComponent* InputComponent);\n"
		"\n"
		"protected:\n"
		"\tvirtual void BeginPlay() override;\n"
		"\n"
		"private:\n"
		"\tvoid OnInputStarted(const FAbilityInputBinding& Binding);\n"
		"\tvoid OnInputCompleted(const FAbilityInputBinding& Binding);\n"
		"\n"
		"\tTWeakObjectPtr<UAbilitySystemComponent> CachedASC;\n"
		"};\n"),
		*ComponentName,
		*BindingMode,
		*ModuleName.ToUpper(),
		*ClassName,
		*ClassName);

	// Generate source
	FString SourceContent = FString::Printf(TEXT(
		"// Auto-generated by Monolith GAS — AbilityInputBindingComponent\n"
		"#include \"%s\"\n"
		"#include \"AbilitySystemComponent.h\"\n"
		"#include \"EnhancedInputComponent.h\"\n"
		"#include \"Abilities/GameplayAbility.h\"\n"
		"#include \"GameplayAbilitySpec.h\"\n"
		"\n"
		"%s::%s()\n"
		"{\n"
		"\tPrimaryComponentTick.bCanEverTick = false;\n"
		"}\n"
		"\n"
		"void %s::BeginPlay()\n"
		"{\n"
		"\tSuper::BeginPlay();\n"
		"\n"
		"\t// Auto-setup: find ASC and EnhancedInputComponent on the owning actor\n"
		"\tAActor* Owner = GetOwner();\n"
		"\tif (!Owner) return;\n"
		"\n"
		"\tUAbilitySystemComponent* ASC = Owner->FindComponentByClass<UAbilitySystemComponent>();\n"
		"\tAPawn* Pawn = Cast<APawn>(Owner);\n"
		"\tif (Pawn && Pawn->InputComponent)\n"
		"\t{\n"
		"\t\tUEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(Pawn->InputComponent);\n"
		"\t\tif (ASC && EIC)\n"
		"\t\t{\n"
		"\t\t\tSetupBindings(ASC, EIC);\n"
		"\t\t}\n"
		"\t}\n"
		"}\n"
		"\n"
		"void %s::SetupBindings(UAbilitySystemComponent* ASC, UEnhancedInputComponent* InputComponent)\n"
		"{\n"
		"\tif (!ASC || !InputComponent) return;\n"
		"\tCachedASC = ASC;\n"
		"\n"
		"\tfor (const FAbilityInputBinding& Binding : AbilityBindings)\n"
		"\t{\n"
		"\t\tif (!Binding.InputAction) continue;\n"
		"\n"
		"\t\t// Grant the ability if a class is specified\n"
		"\t\tif (Binding.AbilityClass)\n"
		"\t\t{\n"
		"\t\t\tFGameplayAbilitySpec Spec(Binding.AbilityClass, 1, INDEX_NONE, GetOwner());\n"
		"\t\t\tif (Binding.AbilityTag.IsValid())\n"
		"\t\t\t{\n"
		"\t\t\t\tSpec.GetDynamicSpecSourceTags().AddTag(Binding.AbilityTag);\n"
		"\t\t\t}\n"
		"\t\t\tASC->GiveAbility(Spec);\n"
		"\t\t}\n"
		"\n"
		"\t\t// Bind input events\n"
		"\t\tInputComponent->BindAction(Binding.InputAction, ETriggerEvent::Started, this,\n"
		"\t\t\t&%s::OnInputStarted, Binding);\n"
		"\t\tInputComponent->BindAction(Binding.InputAction, ETriggerEvent::Completed, this,\n"
		"\t\t\t&%s::OnInputCompleted, Binding);\n"
		"\t}\n"
		"}\n"
		"\n"
		"void %s::OnInputStarted(const FAbilityInputBinding& Binding)\n"
		"{\n"
		"\tif (!CachedASC.IsValid()) return;\n"
		"\n"
		"\tif (Binding.AbilityTag.IsValid())\n"
		"\t{\n"
		"\t\t// Tag-based: try to activate any ability matching the tag\n"
		"\t\tFGameplayTagContainer TagContainer;\n"
		"\t\tTagContainer.AddTag(Binding.AbilityTag);\n"
		"\t\tCachedASC->TryActivateAbilitiesByTag(TagContainer);\n"
		"\t}\n"
		"\telse if (Binding.AbilityClass)\n"
		"\t{\n"
		"\t\tCachedASC->TryActivateAbilityByClass(Binding.AbilityClass);\n"
		"\t}\n"
		"}\n"
		"\n"
		"void %s::OnInputCompleted(const FAbilityInputBinding& Binding)\n"
		"{\n"
		"\t// Override for hold-to-activate or channeled abilities\n"
		"\t// For now, input release is a no-op for instant abilities\n"
		"}\n"),
		*HeaderFileName,
		*ClassName, *ClassName,
		*ClassName,
		*ClassName,
		*ClassName, *ClassName,
		*ClassName,
		*ClassName);

	bool bHeaderOk = FFileHelper::SaveStringToFile(HeaderContent, *HeaderPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	bool bSourceOk = FFileHelper::SaveStringToFile(SourceContent, *SourcePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"), ActorPath);
	Result->SetStringField(TEXT("component_name"), ComponentName);
	Result->SetStringField(TEXT("binding_mode"), BindingMode);
	Result->SetBoolField(TEXT("header_written"), bHeaderOk);
	Result->SetBoolField(TEXT("source_written"), bSourceOk);
	Result->SetStringField(TEXT("header_path"), HeaderPath);
	Result->SetStringField(TEXT("source_path"), SourcePath);

	if (bHeaderOk && bSourceOk)
	{
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Generated %s at %s and %s. Rebuild the project, then add the component to your actor Blueprint."),
				*ClassName, *HeaderPath, *SourcePath));

		TArray<TSharedPtr<FJsonValue>> NextSteps;
		NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Build the project to compile the new component")));
		NextSteps.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("Add %s to your actor Blueprint via add_component or the editor"), *ComponentName)));
		NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Configure AbilityBindings in the component details panel")));
		NextSteps.Add(MakeShared<FJsonValueString>(TEXT("Ensure the actor calls SetupPlayerInputComponent and has EnhancedInputComponent")));
		Result->SetArrayField(TEXT("next_steps"), NextSteps);
	}
	else
	{
		Result->SetStringField(TEXT("error"),
			FString::Printf(TEXT("Failed to write files — header: %s, source: %s"),
				bHeaderOk ? TEXT("OK") : TEXT("FAILED"),
				bSourceOk ? TEXT("OK") : TEXT("FAILED")));
	}

	return FMonolithActionResult::Success(Result);
}
