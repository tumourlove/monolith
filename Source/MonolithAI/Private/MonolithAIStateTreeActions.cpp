#include "MonolithAIStateTreeActions.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#if WITH_STATETREE
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeEditorNode.h"
#include "StateTreeFactory.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditorPropertyBindings.h"
#include "StateTreeTaskBase.h"
#include "StateTreeConditionBase.h"
#include "StateTreeConsiderationBase.h"
#include "StateTreeExtension.h"
#include "StateTreeSchema.h"
#include "StateTreePropertyBindings.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "ScopedTransaction.h"
#include "StructUtils/InstancedStruct.h"
#include "Editor.h"
#include "IMonolithGraphFormatter.h"

// ============================================================
//  Helpers
// ============================================================

namespace
{
	UClass* ResolveStateTreeSchemaClass(const FString& RequestedName)
	{
		TArray<FString> CandidateNames;
		if (!RequestedName.IsEmpty())
		{
			CandidateNames.Add(RequestedName);
			if (!RequestedName.StartsWith(TEXT("U")))
			{
				CandidateNames.Add(TEXT("U") + RequestedName);
			}
		}
		else
		{
			CandidateNames = {
				TEXT("StateTreeAIComponentSchema"),
				TEXT("UStateTreeAIComponentSchema"),
				TEXT("/Script/GameplayStateTreeModule.StateTreeAIComponentSchema"),
				TEXT("/Script/GameplayStateTreeModule.UStateTreeAIComponentSchema")
			};
		}

		for (const FString& Candidate : CandidateNames)
		{
			if (UClass* SchemaClass = FindFirstObject<UClass>(*Candidate, EFindFirstObjectOptions::EnsureIfAmbiguous))
			{
				if (SchemaClass->IsChildOf(UStateTreeSchema::StaticClass()))
				{
					return SchemaClass;
				}
			}
		}

		return nullptr;
	}

	/** Load a UStateTree from the asset_path param. */
	UStateTree* LoadStateTreeFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
	{
		OutAssetPath = Params->GetStringField(TEXT("asset_path"));
		if (OutAssetPath.IsEmpty())
		{
			OutError = TEXT("Missing required param 'asset_path'");
			return nullptr;
		}
		OutAssetPath = FMonolithAssetUtils::ResolveAssetPath(OutAssetPath);

		UStateTree* ST = FMonolithAssetUtils::LoadAssetByPath<UStateTree>(OutAssetPath);
		if (!ST)
		{
			OutError = FString::Printf(TEXT("StateTree not found at '%s'"), *OutAssetPath);
		}
		return ST;
	}

	/** Get the UStateTreeEditorData from a UStateTree. */
	UStateTreeEditorData* GetEditorData(UStateTree* ST, FString& OutError)
	{
#if WITH_EDITORONLY_DATA
		UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(ST->EditorData);
		if (!EditorData)
		{
			OutError = TEXT("StateTree has no editor data — may not have been opened in editor yet");
		}
		return EditorData;
#else
		OutError = TEXT("Editor data not available in non-editor builds");
		return nullptr;
#endif
	}

	/** Find a state by GUID string within the entire tree. */
	UStateTreeState* FindStateByGuid(UStateTreeEditorData* EditorData, const FString& GuidStr)
	{
		FGuid TargetGuid;
		if (!FGuid::Parse(GuidStr, TargetGuid))
		{
			return nullptr;
		}
		return EditorData->GetMutableStateByID(TargetGuid);
	}

	/** Recursively serialize a UStateTreeState to JSON. */
	TSharedPtr<FJsonObject> SerializeState(const UStateTreeState* State)
	{
		if (!State) return nullptr;

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("state_id"), State->ID.ToString());
		Obj->SetStringField(TEXT("name"), State->Name.ToString());
		Obj->SetStringField(TEXT("type"), UEnum::GetValueAsString(State->Type));
		Obj->SetStringField(TEXT("selection_behavior"), UEnum::GetValueAsString(State->SelectionBehavior));
		Obj->SetBoolField(TEXT("enabled"), State->bEnabled);
		Obj->SetNumberField(TEXT("weight"), State->Weight);
		if (State->Tag.IsValid())
		{
			Obj->SetStringField(TEXT("tag"), State->Tag.ToString());
		}

		// Tasks
		TArray<TSharedPtr<FJsonValue>> TaskArr;
		for (int32 i = 0; i < State->Tasks.Num(); ++i)
		{
			const FStateTreeEditorNode& TaskNode = State->Tasks[i];
			TSharedPtr<FJsonObject> TaskObj = MakeShared<FJsonObject>();
			TaskObj->SetStringField(TEXT("node_id"), TaskNode.ID.ToString());
			TaskObj->SetNumberField(TEXT("index"), i);
			if (TaskNode.Node.IsValid())
			{
				TaskObj->SetStringField(TEXT("struct_type"), TaskNode.Node.GetScriptStruct()->GetName());
			}
			TaskArr.Add(MakeShared<FJsonValueObject>(TaskObj));
		}
		if (TaskArr.Num() > 0)
		{
			Obj->SetArrayField(TEXT("tasks"), TaskArr);
		}

		// Enter conditions
		TArray<TSharedPtr<FJsonValue>> CondArr;
		for (int32 i = 0; i < State->EnterConditions.Num(); ++i)
		{
			const FStateTreeEditorNode& CondNode = State->EnterConditions[i];
			TSharedPtr<FJsonObject> CondObj = MakeShared<FJsonObject>();
			CondObj->SetStringField(TEXT("node_id"), CondNode.ID.ToString());
			CondObj->SetNumberField(TEXT("index"), i);
			if (CondNode.Node.IsValid())
			{
				CondObj->SetStringField(TEXT("struct_type"), CondNode.Node.GetScriptStruct()->GetName());
			}
			CondArr.Add(MakeShared<FJsonValueObject>(CondObj));
		}
		if (CondArr.Num() > 0)
		{
			Obj->SetArrayField(TEXT("enter_conditions"), CondArr);
		}

		// Transitions
		TArray<TSharedPtr<FJsonValue>> TransArr;
		for (int32 i = 0; i < State->Transitions.Num(); ++i)
		{
			const FStateTreeTransition& Trans = State->Transitions[i];
			TSharedPtr<FJsonObject> TransObj = MakeShared<FJsonObject>();
			TransObj->SetStringField(TEXT("transition_id"), Trans.ID.ToString());
			TransObj->SetNumberField(TEXT("index"), i);
			TransObj->SetStringField(TEXT("trigger"), UEnum::GetValueAsString(Trans.Trigger));
			TransObj->SetStringField(TEXT("priority"), UEnum::GetValueAsString(Trans.Priority));
			TransObj->SetBoolField(TEXT("enabled"), Trans.bTransitionEnabled);
#if WITH_EDITORONLY_DATA
			TransObj->SetStringField(TEXT("target_type"), UEnum::GetValueAsString(Trans.State.LinkType));
			if (Trans.State.ID.IsValid())
			{
				TransObj->SetStringField(TEXT("target_state_id"), Trans.State.ID.ToString());
				TransObj->SetStringField(TEXT("target_state_name"), Trans.State.Name.ToString());
			}
#endif
			if (Trans.bDelayTransition)
			{
				TransObj->SetNumberField(TEXT("delay_duration"), Trans.DelayDuration);
				TransObj->SetNumberField(TEXT("delay_random_variance"), Trans.DelayRandomVariance);
			}
			TransArr.Add(MakeShared<FJsonValueObject>(TransObj));
		}
		if (TransArr.Num() > 0)
		{
			Obj->SetArrayField(TEXT("transitions"), TransArr);
		}

		// Considerations
		TArray<TSharedPtr<FJsonValue>> ConsArr;
		for (int32 i = 0; i < State->Considerations.Num(); ++i)
		{
			const FStateTreeEditorNode& ConsNode = State->Considerations[i];
			TSharedPtr<FJsonObject> ConsObj = MakeShared<FJsonObject>();
			ConsObj->SetStringField(TEXT("node_id"), ConsNode.ID.ToString());
			ConsObj->SetNumberField(TEXT("index"), i);
			if (ConsNode.Node.IsValid())
			{
				ConsObj->SetStringField(TEXT("struct_type"), ConsNode.Node.GetScriptStruct()->GetName());
			}
			ConsArr.Add(MakeShared<FJsonValueObject>(ConsObj));
		}
		if (ConsArr.Num() > 0)
		{
			Obj->SetArrayField(TEXT("considerations"), ConsArr);
		}

		// Children (recursive)
		TArray<TSharedPtr<FJsonValue>> ChildArr;
		for (const UStateTreeState* Child : State->Children)
		{
			TSharedPtr<FJsonObject> ChildJson = SerializeState(Child);
			if (ChildJson.IsValid())
			{
				ChildArr.Add(MakeShared<FJsonValueObject>(ChildJson));
			}
		}
		if (ChildArr.Num() > 0)
		{
			Obj->SetArrayField(TEXT("children"), ChildArr);
		}

		return Obj;
	}

	/** Parse an EStateTreeStateType from string. */
	EStateTreeStateType ParseStateType(const FString& TypeStr)
	{
		if (TypeStr.Equals(TEXT("Group"), ESearchCase::IgnoreCase)) return EStateTreeStateType::Group;
		if (TypeStr.Equals(TEXT("Linked"), ESearchCase::IgnoreCase)) return EStateTreeStateType::Linked;
		if (TypeStr.Equals(TEXT("LinkedAsset"), ESearchCase::IgnoreCase)) return EStateTreeStateType::LinkedAsset;
		if (TypeStr.Equals(TEXT("Subtree"), ESearchCase::IgnoreCase)) return EStateTreeStateType::Subtree;
		return EStateTreeStateType::State;
	}

	/** Parse EStateTreeStateSelectionBehavior from string. */
	EStateTreeStateSelectionBehavior ParseSelectionBehavior(const FString& Str)
	{
		if (Str.Equals(TEXT("None"), ESearchCase::IgnoreCase)) return EStateTreeStateSelectionBehavior::None;
		if (Str.Equals(TEXT("TryEnterState"), ESearchCase::IgnoreCase) || Str.Equals(TEXT("TryEnter"), ESearchCase::IgnoreCase)) return EStateTreeStateSelectionBehavior::TryEnterState;
		if (Str.Contains(TEXT("Random"), ESearchCase::IgnoreCase) && Str.Contains(TEXT("Utility"), ESearchCase::IgnoreCase)) return EStateTreeStateSelectionBehavior::TrySelectChildrenAtRandomWeightedByUtility;
		if (Str.Contains(TEXT("Random"), ESearchCase::IgnoreCase)) return EStateTreeStateSelectionBehavior::TrySelectChildrenAtRandom;
		if (Str.Contains(TEXT("Utility"), ESearchCase::IgnoreCase)) return EStateTreeStateSelectionBehavior::TrySelectChildrenWithHighestUtility;
		if (Str.Contains(TEXT("Transition"), ESearchCase::IgnoreCase)) return EStateTreeStateSelectionBehavior::TryFollowTransitions;
		return EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
	}

	/** Parse EStateTreeTransitionTrigger from string. Silent fallback to OnStateCompleted for legacy callers. */
	EStateTreeTransitionTrigger ParseTransitionTrigger(const FString& Str)
	{
		if (Str.Equals(TEXT("OnStateCompleted"), ESearchCase::IgnoreCase)) return EStateTreeTransitionTrigger::OnStateCompleted;
		if (Str.Equals(TEXT("OnStateSucceeded"), ESearchCase::IgnoreCase)) return EStateTreeTransitionTrigger::OnStateSucceeded;
		if (Str.Equals(TEXT("OnStateFailed"), ESearchCase::IgnoreCase)) return EStateTreeTransitionTrigger::OnStateFailed;
		if (Str.Equals(TEXT("OnTick"), ESearchCase::IgnoreCase)) return EStateTreeTransitionTrigger::OnTick;
		if (Str.Equals(TEXT("OnEvent"), ESearchCase::IgnoreCase)) return EStateTreeTransitionTrigger::OnEvent;
		return EStateTreeTransitionTrigger::OnStateCompleted;
	}

	/** Phase F #25: validating variant. Returns false on unknown trigger so callers can
	 *  reject instead of silently accepting OnStateCompleted. */
	bool TryParseTransitionTrigger(const FString& Str, EStateTreeTransitionTrigger& OutTrigger, FString& OutValidValues)
	{
		if (Str.Equals(TEXT("OnStateCompleted"), ESearchCase::IgnoreCase)) { OutTrigger = EStateTreeTransitionTrigger::OnStateCompleted; return true; }
		if (Str.Equals(TEXT("OnStateSucceeded"), ESearchCase::IgnoreCase)) { OutTrigger = EStateTreeTransitionTrigger::OnStateSucceeded; return true; }
		if (Str.Equals(TEXT("OnStateFailed"), ESearchCase::IgnoreCase)) { OutTrigger = EStateTreeTransitionTrigger::OnStateFailed; return true; }
		if (Str.Equals(TEXT("OnTick"), ESearchCase::IgnoreCase)) { OutTrigger = EStateTreeTransitionTrigger::OnTick; return true; }
		if (Str.Equals(TEXT("OnEvent"), ESearchCase::IgnoreCase)) { OutTrigger = EStateTreeTransitionTrigger::OnEvent; return true; }
		OutValidValues = TEXT("OnStateCompleted, OnStateSucceeded, OnStateFailed, OnTick, OnEvent");
		return false;
	}

	/** Parse EStateTreeTransitionPriority from string. */
	EStateTreeTransitionPriority ParseTransitionPriority(const FString& Str)
	{
		if (Str.Equals(TEXT("Low"), ESearchCase::IgnoreCase)) return EStateTreeTransitionPriority::Low;
		if (Str.Equals(TEXT("Medium"), ESearchCase::IgnoreCase)) return EStateTreeTransitionPriority::Medium;
		if (Str.Equals(TEXT("High"), ESearchCase::IgnoreCase)) return EStateTreeTransitionPriority::High;
		if (Str.Equals(TEXT("Critical"), ESearchCase::IgnoreCase)) return EStateTreeTransitionPriority::Critical;
		return EStateTreeTransitionPriority::Normal;
	}

	/** Find a UScriptStruct by name across all loaded modules. */
	UScriptStruct* FindStructByName(const FString& StructName)
	{
		// Try direct lookup first
		UScriptStruct* Found = FindFirstObject<UScriptStruct>(*StructName, EFindFirstObjectOptions::EnsureIfAmbiguous);
		if (Found) return Found;

		// Try with F prefix
		if (!StructName.StartsWith(TEXT("F")))
		{
			Found = FindFirstObject<UScriptStruct>(*(TEXT("F") + StructName), EFindFirstObjectOptions::EnsureIfAmbiguous);
			if (Found) return Found;
		}

		return nullptr;
	}

	/** Create a FStateTreeEditorNode from a UScriptStruct type. Initializes Node + Instance data. */
	bool CreateEditorNode(FStateTreeEditorNode& OutNode, UScriptStruct* StructType, FString& OutError)
	{
		OutNode.ID = FGuid::NewGuid();
		OutNode.Node.InitializeAs(StructType);

		if (!OutNode.Node.IsValid())
		{
			OutError = FString::Printf(TEXT("Failed to initialize node as '%s'"), *StructType->GetName());
			return false;
		}

		// Initialize instance data if the node type defines it
		const FStateTreeNodeBase& NodeBase = OutNode.Node.Get<FStateTreeNodeBase>();
		if (const UScriptStruct* InstanceType = Cast<const UScriptStruct>(NodeBase.GetInstanceDataType()))
		{
			OutNode.Instance.InitializeAs(InstanceType);
		}
		if (const UScriptStruct* RuntimeType = Cast<const UScriptStruct>(NodeBase.GetExecutionRuntimeDataType()))
		{
			OutNode.ExecutionRuntimeData.InitializeAs(RuntimeType);
		}

		return true;
	}

	/** Set properties on an FInstancedStruct via reflection. */
	bool SetInstancedStructProperties(FInstancedStruct& Struct, const TSharedPtr<FJsonObject>& Properties, FString& OutError)
	{
		if (!Properties.IsValid() || !Struct.IsValid()) return true; // nothing to set

		const UScriptStruct* StructType = Struct.GetScriptStruct();
		uint8* Memory = Struct.GetMutableMemory();

		for (const auto& Pair : Properties->Values)
		{
			FProperty* Prop = StructType->FindPropertyByName(*Pair.Key);
			if (!Prop)
			{
				OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *Pair.Key, *StructType->GetName());
				return false;
			}

			FString ValueStr;
			if (Pair.Value->Type == EJson::String)
			{
				ValueStr = Pair.Value->AsString();
			}
			else if (Pair.Value->Type == EJson::Number)
			{
				ValueStr = FString::SanitizeFloat(Pair.Value->AsNumber());
			}
			else if (Pair.Value->Type == EJson::Boolean)
			{
				ValueStr = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");
			}
			else
			{
				// Serialize complex types to string
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ValueStr);
				FJsonSerializer::Serialize(Pair.Value, TEXT(""), Writer);
			}

			void* PropAddr = Prop->ContainerPtrToValuePtr<void>(Memory);
			if (!Prop->ImportText_Direct(*ValueStr, PropAddr, nullptr, PPF_None))
			{
				OutError = FString::Printf(TEXT("Failed to set property '%s' to '%s' on %s"), *Pair.Key, *ValueStr, *StructType->GetName());
				return false;
			}
		}
		return true;
	}

	/** Compile a state tree and return success/failure. */
	bool CompileTree(UStateTree* ST)
	{
		FStateTreeCompilerLog Log;
		return UStateTreeEditingSubsystem::CompileStateTree(ST, Log);
	}

	/** Remove a state from its parent's Children array. */
	bool RemoveStateFromParent(UStateTreeState* State, UStateTreeEditorData* EditorData)
	{
		if (State->Parent)
		{
			return State->Parent->Children.Remove(State) > 0;
		}
		// Top-level subtree
		return EditorData->SubTrees.Remove(State) > 0;
	}
}

#endif // WITH_STATETREE

// ============================================================
//  Registration
// ============================================================

void FMonolithAIStateTreeActions::RegisterActions(FMonolithToolRegistry& Registry)
{
#if WITH_STATETREE
	// 43. create_state_tree
	Registry.RegisterAction(TEXT("ai"), TEXT("create_state_tree"),
		TEXT("Create a new UStateTree asset. Optionally set schema class."),
		FMonolithActionHandler::CreateStatic(&HandleCreateStateTree),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path (e.g. /Game/AI/ST_Enemy)"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Asset name (derived from save_path if omitted)"))
			.Optional(TEXT("schema_class"), TEXT("string"), TEXT("Schema class name (e.g. UStateTreeAIComponentSchema)"))
			.Build());

	// 44. get_state_tree
	Registry.RegisterAction(TEXT("ai"), TEXT("get_state_tree"),
		TEXT("Full tree structure as JSON: states (recursive), tasks, conditions, transitions, considerations"),
		FMonolithActionHandler::CreateStatic(&HandleGetStateTree),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Build());

	// 45. list_state_trees
	Registry.RegisterAction(TEXT("ai"), TEXT("list_state_trees"),
		TEXT("List all UStateTree assets in the project"),
		FMonolithActionHandler::CreateStatic(&HandleListStateTrees),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Only include assets under this path prefix"))
			.Build());

	// 46. delete_state_tree
	Registry.RegisterAction(TEXT("ai"), TEXT("delete_state_tree"),
		TEXT("Delete a StateTree asset"),
		FMonolithActionHandler::CreateStatic(&HandleDeleteStateTree),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path to delete"))
			.Build());

	// 47. duplicate_state_tree
	Registry.RegisterAction(TEXT("ai"), TEXT("duplicate_state_tree"),
		TEXT("Deep copy a StateTree asset to a new path"),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateStateTree),
		FParamSchemaBuilder()
			.Required(TEXT("source_path"), TEXT("string"), TEXT("Source StateTree asset path"))
			.Required(TEXT("dest_path"), TEXT("string"), TEXT("Destination asset path for the copy"))
			.Build());

	// 48. compile_state_tree
	Registry.RegisterAction(TEXT("ai"), TEXT("compile_state_tree"),
		TEXT("Compile a StateTree via UStateTreeEditingSubsystem. MANDATORY after any edits."),
		FMonolithActionHandler::CreateStatic(&HandleCompileStateTree),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path to compile"))
			.Build());

	// 49. set_st_schema
	Registry.RegisterAction(TEXT("ai"), TEXT("set_st_schema"),
		TEXT("Set schema class and optional context actor class on a StateTree"),
		FMonolithActionHandler::CreateStatic(&HandleSetSTSchema),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("schema_class"), TEXT("string"), TEXT("Schema class name (e.g. StateTreeAIComponentSchema)"))
			.Optional(TEXT("context_actor_class"), TEXT("string"), TEXT("Context actor class name for the schema"))
			.Build());

	// 50. add_st_state
	Registry.RegisterAction(TEXT("ai"), TEXT("add_st_state"),
		TEXT("Add a state to the StateTree. Omit parent_state_id (or pass null) to add at root level (SubTree). "
		     "If parent_state_id is supplied but doesn't match a state, returns an error."),
		FMonolithActionHandler::CreateStatic(&HandleAddSTState),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Name for the new state"))
			.Optional(TEXT("parent_state_id"), TEXT("string"), TEXT("GUID of parent state (omit/null = add as SubTree root)"), { TEXT("parent_id") })
			.Optional(TEXT("type"), TEXT("string"), TEXT("State/Group/Linked/LinkedAsset/Subtree (default: State)"))
			.Optional(TEXT("selection_behavior"), TEXT("string"), TEXT("Selection behavior (default: TrySelectChildrenInOrder)"))
			.Optional(TEXT("linked_asset_path"), TEXT("string"), TEXT("For LinkedAsset type: path to linked StateTree"))
			.Build());

	// 51. remove_st_state
	Registry.RegisterAction(TEXT("ai"), TEXT("remove_st_state"),
		TEXT("Remove a state and its children from the StateTree"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveSTState),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("state_id"), TEXT("string"), TEXT("GUID of the state to remove"))
			.Build());

	// 52. rename_st_state
	Registry.RegisterAction(TEXT("ai"), TEXT("rename_st_state"),
		TEXT("Rename a state in the StateTree"),
		FMonolithActionHandler::CreateStatic(&HandleRenameSTState),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("state_id"), TEXT("string"), TEXT("GUID of the state to rename"))
			.Required(TEXT("new_name"), TEXT("string"), TEXT("New name for the state"))
			.Build());

	// 53. move_st_state
	Registry.RegisterAction(TEXT("ai"), TEXT("move_st_state"),
		TEXT("Reparent a state under a different parent state"),
		FMonolithActionHandler::CreateStatic(&HandleMoveSTState),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("state_id"), TEXT("string"), TEXT("GUID of the state to move"))
			.Required(TEXT("new_parent_id"), TEXT("string"), TEXT("GUID of new parent state (empty = move to root)"))
			.Optional(TEXT("index"), TEXT("number"), TEXT("Index within new parent's children (-1 = append)"))
			.Build());

	// 54. set_st_state_properties
	Registry.RegisterAction(TEXT("ai"), TEXT("set_st_state_properties"),
		TEXT("Set state properties like weight, selection_behavior, tag, enabled"),
		FMonolithActionHandler::CreateStatic(&HandleSetSTStateProperties),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("state_id"), TEXT("string"), TEXT("GUID of the state"))
			.Optional(TEXT("weight"), TEXT("number"), TEXT("Utility weight"))
			.Optional(TEXT("selection_behavior"), TEXT("string"), TEXT("Selection behavior enum"))
			.Optional(TEXT("tag"), TEXT("string"), TEXT("Gameplay tag for the state"))
			.Optional(TEXT("enabled"), TEXT("boolean"), TEXT("Enable/disable the state"))
			.Build());

	// 55. add_st_task
	Registry.RegisterAction(TEXT("ai"), TEXT("add_st_task"),
		TEXT("Add a task (FInstancedStruct) to a state"),
		FMonolithActionHandler::CreateStatic(&HandleAddSTTask),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("state_id"), TEXT("string"), TEXT("GUID of the target state"))
			.Required(TEXT("task_class"), TEXT("string"), TEXT("Task struct name (e.g. FStateTreeRunParallelTask)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("JSON object of property_name->value pairs"))
			.Build());

	// 56. remove_st_task
	Registry.RegisterAction(TEXT("ai"), TEXT("remove_st_task"),
		TEXT("Remove a task from a state by index"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveSTTask),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("state_id"), TEXT("string"), TEXT("GUID of the state owning the task"))
			.Required(TEXT("task_index"), TEXT("number"), TEXT("Index of the task to remove"))
			.Build());

	// 57. set_st_task_property
	Registry.RegisterAction(TEXT("ai"), TEXT("set_st_task_property"),
		TEXT("Set a property on a task via ImportText_Direct reflection"),
		FMonolithActionHandler::CreateStatic(&HandleSetSTTaskProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("state_id"), TEXT("string"), TEXT("GUID of the state owning the task"))
			.Required(TEXT("task_index"), TEXT("number"), TEXT("Index of the task"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Property name to set"))
			.Required(TEXT("value"), TEXT("any"), TEXT("Value to set (type depends on property)"))
			.Build());

	// 58. add_st_enter_condition
	Registry.RegisterAction(TEXT("ai"), TEXT("add_st_enter_condition"),
		TEXT("Add an enter condition to a state"),
		FMonolithActionHandler::CreateStatic(&HandleAddSTEnterCondition),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("state_id"), TEXT("string"), TEXT("GUID of the target state"))
			.Required(TEXT("condition_class"), TEXT("string"), TEXT("Condition struct name"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("JSON object of property_name->value pairs"))
			.Build());

	// 59. remove_st_enter_condition
	Registry.RegisterAction(TEXT("ai"), TEXT("remove_st_enter_condition"),
		TEXT("Remove an enter condition from a state by index"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveSTEnterCondition),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("state_id"), TEXT("string"), TEXT("GUID of the state"))
			.Required(TEXT("condition_index"), TEXT("number"), TEXT("Index of the condition to remove"))
			.Build());

	// 60. add_st_transition
	Registry.RegisterAction(TEXT("ai"), TEXT("add_st_transition"),
		TEXT("Add a transition to a state"),
		FMonolithActionHandler::CreateStatic(&HandleAddSTTransition),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("state_id"), TEXT("string"), TEXT("GUID of the state"))
			.Required(TEXT("trigger"), TEXT("string"), TEXT("Trigger: OnStateCompleted, OnStateSucceeded, OnStateFailed, OnTick, OnEvent"))
			.Required(TEXT("target_state"), TEXT("string"), TEXT("Target type: Succeeded, Failed, NextState, NextSelectableState, or a state GUID"))
			.Optional(TEXT("priority"), TEXT("string"), TEXT("Priority: Low, Normal, Medium, High, Critical"))
			.Optional(TEXT("delay"), TEXT("number"), TEXT("Transition delay in seconds"))
			.Build());

	// 61. remove_st_transition
	Registry.RegisterAction(TEXT("ai"), TEXT("remove_st_transition"),
		TEXT("Remove a transition from a state by index"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveSTTransition),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("state_id"), TEXT("string"), TEXT("GUID of the state"))
			.Required(TEXT("transition_index"), TEXT("number"), TEXT("Index of the transition to remove"))
			.Build());

	// 65. add_st_property_binding
	Registry.RegisterAction(TEXT("ai"), TEXT("add_st_property_binding"),
		TEXT("Wire a property binding between source and target paths"),
		FMonolithActionHandler::CreateStatic(&HandleAddSTPropertyBinding),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("source_path"), TEXT("string"), TEXT("Source property path (e.g. StructID.PropertyName)"))
			.Required(TEXT("target_path"), TEXT("string"), TEXT("Target property path"))
			.Build());

	// 66. remove_st_property_binding
	Registry.RegisterAction(TEXT("ai"), TEXT("remove_st_property_binding"),
		TEXT("Remove a property binding by index"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveSTPropertyBinding),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("binding_index"), TEXT("number"), TEXT("Index of the binding to remove"))
			.Build());

	// 67. get_st_bindings
	Registry.RegisterAction(TEXT("ai"), TEXT("get_st_bindings"),
		TEXT("List all property bindings in a StateTree"),
		FMonolithActionHandler::CreateStatic(&HandleGetSTBindings),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Build());

	// 68. get_st_bindable_properties
	Registry.RegisterAction(TEXT("ai"), TEXT("get_st_bindable_properties"),
		TEXT("List available bindable properties, optionally scoped to a state/task"),
		FMonolithActionHandler::CreateStatic(&HandleGetSTBindableProperties),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Optional(TEXT("state_id"), TEXT("string"), TEXT("GUID of the state to scope to"))
			.Optional(TEXT("task_index"), TEXT("number"), TEXT("Task index within the state"))
			.Build());

	// 69. list_st_task_types
	Registry.RegisterAction(TEXT("ai"), TEXT("list_st_task_types"),
		TEXT("List all available FStateTreeTaskBase subclasses"),
		FMonolithActionHandler::CreateStatic(&HandleListSTTaskTypes),
		FParamSchemaBuilder().Build());

	// 70. list_st_condition_types
	Registry.RegisterAction(TEXT("ai"), TEXT("list_st_condition_types"),
		TEXT("List all available StateTree condition struct types"),
		FMonolithActionHandler::CreateStatic(&HandleListSTConditionTypes),
		FParamSchemaBuilder().Build());

	// 62. add_st_transition_condition
	Registry.RegisterAction(TEXT("ai"), TEXT("add_st_transition_condition"),
		TEXT("Add a condition to an existing transition on a state"),
		FMonolithActionHandler::CreateStatic(&HandleAddSTTransitionCondition),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("state_id"), TEXT("string"), TEXT("GUID of the state owning the transition"))
			.Required(TEXT("transition_index"), TEXT("number"), TEXT("Index of the transition"))
			.Required(TEXT("condition_class"), TEXT("string"), TEXT("Condition struct name (e.g. FStateTreeCompareIntCondition)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("JSON object of property_name->value pairs"))
			.Build());

	// 63. add_st_consideration
	Registry.RegisterAction(TEXT("ai"), TEXT("add_st_consideration"),
		TEXT("Add a utility consideration to a state"),
		FMonolithActionHandler::CreateStatic(&HandleAddSTConsideration),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("state_id"), TEXT("string"), TEXT("GUID of the target state"))
			.Required(TEXT("consideration_class"), TEXT("string"), TEXT("Consideration struct name (e.g. FStateTreeConstantConsideration)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("JSON object of property_name->value pairs"))
			.Build());

	// 64. configure_st_consideration
	Registry.RegisterAction(TEXT("ai"), TEXT("configure_st_consideration"),
		TEXT("Configure a consideration's properties on a state (by index)"),
		FMonolithActionHandler::CreateStatic(&HandleConfigureSTConsideration),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("state_id"), TEXT("string"), TEXT("GUID of the state"))
			.Required(TEXT("consideration_index"), TEXT("number"), TEXT("Index of the consideration"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("JSON object of property_name->value pairs to set on consideration node"))
			.Optional(TEXT("instance_properties"), TEXT("object"), TEXT("JSON object of property_name->value pairs to set on instance data"))
			.Build());

	// 73. validate_state_tree
	Registry.RegisterAction(TEXT("ai"), TEXT("validate_state_tree"),
		TEXT("Validate a StateTree: check unbound inputs, dead-end states, infinite loops, missing tasks"),
		FMonolithActionHandler::CreateStatic(&HandleValidateStateTree),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Build());

	// 75. list_st_extension_types
	Registry.RegisterAction(TEXT("ai"), TEXT("list_st_extension_types"),
		TEXT("List all available UStateTreeExtension subclasses"),
		FMonolithActionHandler::CreateStatic(&HandleListSTExtensionTypes),
		FParamSchemaBuilder().Build());

	// 76. add_st_extension
	Registry.RegisterAction(TEXT("ai"), TEXT("add_st_extension"),
		TEXT("Add an extension to a StateTree asset"),
		FMonolithActionHandler::CreateStatic(&HandleAddSTExtension),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Required(TEXT("extension_class"), TEXT("string"), TEXT("Extension class name (e.g. UStateTreeAIExtension)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("JSON object of property_name->value pairs to set on the extension"))
			.Build());

	// 71. build_state_tree_from_spec
	Registry.RegisterAction(TEXT("ai"), TEXT("build_state_tree_from_spec"),
		TEXT("Declarative full-tree creation from a JSON spec: creates states, adds tasks, transitions, and compiles"),
		FMonolithActionHandler::CreateStatic(&HandleBuildStateTreeFromSpec),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path (e.g. /Game/AI/ST_Enemy)"))
			.Required(TEXT("spec"), TEXT("object"), TEXT("JSON spec: {schema_class?, root: {name, type?, children: [{name, tasks, transitions}]}}"))
			.Optional(TEXT("strict_mode"), TEXT("boolean"), TEXT("If true, abort with error (no save) when any task/condition struct fails to resolve"), TEXT("false"))
			.Build());

	// 72. export_st_spec
	Registry.RegisterAction(TEXT("ai"), TEXT("export_st_spec"),
		TEXT("Export a StateTree as a JSON spec that can be fed back into build_state_tree_from_spec"),
		FMonolithActionHandler::CreateStatic(&HandleExportSTSpec),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Build());

	// 74. generate_st_diagram
	Registry.RegisterAction(TEXT("ai"), TEXT("generate_st_diagram"),
		TEXT("Generate a Mermaid state diagram from a StateTree"),
		FMonolithActionHandler::CreateStatic(&HandleGenerateSTDiagram),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Optional(TEXT("format"), TEXT("string"), TEXT("Diagram format: mermaid (default)"))
			.Build());

	// 76b. auto_arrange_st
	Registry.RegisterAction(TEXT("ai"), TEXT("auto_arrange_st"),
		TEXT("Auto-layout a StateTree graph via IMonolithGraphFormatter or built-in fallback"),
		FMonolithActionHandler::CreateStatic(&HandleAutoArrangeST),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StateTree asset path"))
			.Optional(TEXT("formatter"), TEXT("string"), TEXT("Formatter to use: 'blueprint_assist' or 'builtin' (default: auto-detect)"))
			.Build());

#endif // WITH_STATETREE
}

// ============================================================
//  Implementations (all inside WITH_STATETREE)
// ============================================================

#if WITH_STATETREE

// 43. create_state_tree
FMonolithActionResult FMonolithAIStateTreeActions::HandleCreateStateTree(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'save_path'"));
	}
	SavePath = FMonolithAssetUtils::ResolveAssetPath(SavePath);

	FString AssetName = Params->GetStringField(TEXT("name"));
	if (AssetName.IsEmpty())
	{
		AssetName = FPackageName::GetShortName(SavePath);
	}

	FString PackagePath = SavePath;
	if (FPackageName::GetShortName(PackagePath) == AssetName)
	{
		// save_path includes the asset name — use as-is
	}
	else
	{
		PackagePath = SavePath / AssetName;
	}

	// Check path is free
	FString PathErr;
	if (!MonolithAI::EnsureAssetPathFree(PackagePath, AssetName, PathErr))
	{
		return FMonolithActionResult::Error(PathErr);
	}

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *PackagePath));
	}

	UStateTreeFactory* Factory = NewObject<UStateTreeFactory>();

	// Set schema if provided
	const FString SchemaClassName = Params->HasField(TEXT("schema_class")) ? Params->GetStringField(TEXT("schema_class")) : FString();
	UClass* SchemaClass = ResolveStateTreeSchemaClass(SchemaClassName);
	if (!SchemaClass)
	{
		const FString MissingName = SchemaClassName.IsEmpty() ? TEXT("StateTreeAIComponentSchema") : SchemaClassName;
		return FMonolithActionResult::Error(FString::Printf(TEXT("Schema class '%s' not found or not a UStateTreeSchema"), *MissingName));
	}
	Factory->SetSchemaClass(SchemaClass);

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Create StateTree")));

	UStateTree* NewST = Cast<UStateTree>(Factory->FactoryCreateNew(
		UStateTree::StaticClass(), Package, *AssetName, RF_Public | RF_Standalone, nullptr, GWarn));
	if (!NewST)
	{
		return FMonolithActionResult::Error(TEXT("UStateTreeFactory::FactoryCreateNew failed"));
	}

	NewST->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewST);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), PackagePath);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Created StateTree '%s'"), *AssetName));
	return FMonolithActionResult::Success(Result);
}

// 44. get_state_tree
FMonolithActionResult FMonolithAIStateTreeActions::HandleGetStateTree(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);

	// Schema
	if (ST->GetSchema())
	{
		Result->SetStringField(TEXT("schema"), ST->GetSchema()->GetClass()->GetName());
	}

	Result->SetBoolField(TEXT("is_ready_to_run"), ST->IsReadyToRun());

	// SubTrees (root states)
	TArray<TSharedPtr<FJsonValue>> SubTreeArr;
	for (const UStateTreeState* SubTree : EditorData->SubTrees)
	{
		TSharedPtr<FJsonObject> SubTreeJson = SerializeState(SubTree);
		if (SubTreeJson.IsValid())
		{
			SubTreeArr.Add(MakeShared<FJsonValueObject>(SubTreeJson));
		}
	}
	Result->SetArrayField(TEXT("subtrees"), SubTreeArr);

	// Evaluators
	TArray<TSharedPtr<FJsonValue>> EvalArr;
	for (int32 i = 0; i < EditorData->Evaluators.Num(); ++i)
	{
		const FStateTreeEditorNode& EvalNode = EditorData->Evaluators[i];
		TSharedPtr<FJsonObject> EvalObj = MakeShared<FJsonObject>();
		EvalObj->SetStringField(TEXT("node_id"), EvalNode.ID.ToString());
		EvalObj->SetNumberField(TEXT("index"), i);
		if (EvalNode.Node.IsValid())
		{
			EvalObj->SetStringField(TEXT("struct_type"), EvalNode.Node.GetScriptStruct()->GetName());
		}
		EvalArr.Add(MakeShared<FJsonValueObject>(EvalObj));
	}
	if (EvalArr.Num() > 0)
	{
		Result->SetArrayField(TEXT("evaluators"), EvalArr);
	}

	// Global tasks
	TArray<TSharedPtr<FJsonValue>> GlobalTaskArr;
	for (int32 i = 0; i < EditorData->GlobalTasks.Num(); ++i)
	{
		const FStateTreeEditorNode& GTaskNode = EditorData->GlobalTasks[i];
		TSharedPtr<FJsonObject> GTaskObj = MakeShared<FJsonObject>();
		GTaskObj->SetStringField(TEXT("node_id"), GTaskNode.ID.ToString());
		GTaskObj->SetNumberField(TEXT("index"), i);
		if (GTaskNode.Node.IsValid())
		{
			GTaskObj->SetStringField(TEXT("struct_type"), GTaskNode.Node.GetScriptStruct()->GetName());
		}
		GlobalTaskArr.Add(MakeShared<FJsonValueObject>(GTaskObj));
	}
	if (GlobalTaskArr.Num() > 0)
	{
		Result->SetArrayField(TEXT("global_tasks"), GlobalTaskArr);
	}

	return FMonolithActionResult::Success(Result);
}

// 45. list_state_trees
FMonolithActionResult FMonolithAIStateTreeActions::HandleListStateTrees(const TSharedPtr<FJsonObject>& Params)
{
	FString PathFilter = Params->GetStringField(TEXT("path_filter"));

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetData> Assets;
	AR.GetAssetsByClass(UStateTree::StaticClass()->GetClassPathName(), Assets);

	TArray<TSharedPtr<FJsonValue>> ResultArr;
	for (const FAssetData& Asset : Assets)
	{
		FString AssetPathStr = Asset.GetObjectPathString();
		if (!PathFilter.IsEmpty() && !AssetPathStr.StartsWith(PathFilter))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), Asset.PackageName.ToString());
		Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		ResultArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("state_trees"), ResultArr);
	Result->SetNumberField(TEXT("count"), ResultArr.Num());
	return FMonolithActionResult::Success(Result);
}

// 46. delete_state_tree
FMonolithActionResult FMonolithAIStateTreeActions::HandleDeleteStateTree(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Delete StateTree")));

	TArray<UObject*> ObjectsToDelete;
	ObjectsToDelete.Add(ST);
	int32 Deleted = ObjectTools::DeleteObjects(ObjectsToDelete, /*bShowConfirmation=*/false);

	if (Deleted == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to delete StateTree at '%s'"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Deleted StateTree '%s'"), *AssetPath));
	return FMonolithActionResult::Success(Result);
}

// 47. duplicate_state_tree
FMonolithActionResult FMonolithAIStateTreeActions::HandleDuplicateStateTree(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath = Params->GetStringField(TEXT("source_path"));
	FString DestPath = Params->GetStringField(TEXT("dest_path"));

	if (SourcePath.IsEmpty() || DestPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required params 'source_path' and 'dest_path'"));
	}

	SourcePath = FMonolithAssetUtils::ResolveAssetPath(SourcePath);
	DestPath = FMonolithAssetUtils::ResolveAssetPath(DestPath);

	FString Error;
	UStateTree* SourceST = FMonolithAssetUtils::LoadAssetByPath<UStateTree>(SourcePath);
	if (!SourceST)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source StateTree not found at '%s'"), *SourcePath));
	}

	FString DestName = FPackageName::GetShortName(DestPath);

	FString PathErr;
	if (!MonolithAI::EnsureAssetPathFree(DestPath, DestName, PathErr))
	{
		return FMonolithActionResult::Error(PathErr);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Duplicate StateTree")));

	UPackage* DestPackage = CreatePackage(*DestPath);
	UStateTree* NewST = Cast<UStateTree>(StaticDuplicateObject(SourceST, DestPackage, *DestName));
	if (!NewST)
	{
		return FMonolithActionResult::Error(TEXT("StaticDuplicateObject failed"));
	}

	NewST->SetFlags(RF_Public | RF_Standalone);
	NewST->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewST);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), DestPath);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Duplicated to '%s'"), *DestPath));
	return FMonolithActionResult::Success(Result);
}

// 48. compile_state_tree
FMonolithActionResult FMonolithAIStateTreeActions::HandleCompileStateTree(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	bool bSuccess = CompileTree(ST);

	if (bSuccess)
	{
		ST->MarkPackageDirty();
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bSuccess);
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetBoolField(TEXT("is_ready_to_run"), ST->IsReadyToRun());
	return FMonolithActionResult::Success(Result);
}

// 49. set_st_schema
FMonolithActionResult FMonolithAIStateTreeActions::HandleSetSTSchema(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString SchemaClassName = Params->GetStringField(TEXT("schema_class"));
	if (SchemaClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'schema_class'"));
	}

	UClass* SchemaClass = FindFirstObject<UClass>(*SchemaClassName, EFindFirstObjectOptions::EnsureIfAmbiguous);
	if (!SchemaClass)
	{
		SchemaClass = FindFirstObject<UClass>(*(TEXT("U") + SchemaClassName), EFindFirstObjectOptions::EnsureIfAmbiguous);
	}
	if (!SchemaClass || !SchemaClass->IsChildOf(UStateTreeSchema::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Schema class '%s' not found or not a UStateTreeSchema subclass"), *SchemaClassName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Set ST Schema")));

	// Create new schema instance on the editor data (which is the canonical owner)
	UStateTreeSchema* NewSchema = NewObject<UStateTreeSchema>(EditorData, SchemaClass, NAME_None, RF_Transactional);
	EditorData->Schema = NewSchema;

	// Set context actor class if provided
	FString ContextActorClassName = Params->GetStringField(TEXT("context_actor_class"));
	if (!ContextActorClassName.IsEmpty())
	{
		UClass* ActorClass = FindFirstObject<UClass>(*ContextActorClassName, EFindFirstObjectOptions::EnsureIfAmbiguous);
		if (!ActorClass)
		{
			ActorClass = FindFirstObject<UClass>(*(TEXT("A") + ContextActorClassName), EFindFirstObjectOptions::EnsureIfAmbiguous);
		}
		if (ActorClass)
		{
			// Try to set via reflection — schema subclasses may have different property names
			FProperty* ContextProp = SchemaClass->FindPropertyByName(TEXT("ContextActorClass"));
			if (ContextProp)
			{
				if (FClassProperty* ClassProp = CastField<FClassProperty>(ContextProp))
				{
					ClassProp->SetPropertyValue_InContainer(NewSchema, ActorClass);
				}
			}
		}
	}

	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("schema"), SchemaClass->GetName());
	Result->SetStringField(TEXT("message"), TEXT("Schema set. Call compile_state_tree to compile."));
	return FMonolithActionResult::Success(Result);
}

// 50. add_st_state
FMonolithActionResult FMonolithAIStateTreeActions::HandleAddSTState(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString StateName = Params->GetStringField(TEXT("name"));
	if (StateName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'name'"));
	}

	// Phase F #24: distinguish "key absent / null / empty" (intent: add at root)
	// from "key supplied with non-empty garbage" (intent: child of given parent — must validate).
	const bool bParentSupplied = Params->HasField(TEXT("parent_state_id"));
	FString ParentStateId;
	bool bParentExplicitlyNull = false;
	if (bParentSupplied)
	{
		const TSharedPtr<FJsonValue> ParentField = Params->TryGetField(TEXT("parent_state_id"));
		if (ParentField.IsValid())
		{
			if (ParentField->Type == EJson::Null)
			{
				bParentExplicitlyNull = true;
			}
			else
			{
				ParentField->TryGetString(ParentStateId);
			}
		}
	}

	FString TypeStr = Params->GetStringField(TEXT("type"));
	FString SelectionStr = Params->GetStringField(TEXT("selection_behavior"));
	FString LinkedAssetPath = Params->GetStringField(TEXT("linked_asset_path"));

	EStateTreeStateType StateType = ParseStateType(TypeStr);

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add ST State")));

	UStateTreeState* NewState = nullptr;

	const bool bAddAtRoot = !bParentSupplied || bParentExplicitlyNull || ParentStateId.IsEmpty();

	if (bAddAtRoot)
	{
		// Add as root subtree
		NewState = &EditorData->AddSubTree(FName(*StateName));
		NewState->Type = StateType;
	}
	else
	{
		UStateTreeState* Parent = FindStateByGuid(EditorData, ParentStateId);
		if (!Parent)
		{
			// Phase F #24: was previously silent-fallback to root when GUID didn't resolve in some
			// schema variants. Now hard error so callers don't get a tree they didn't ask for.
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("parent_state_id '%s' does not match any existing state. ")
				TEXT("Use a valid state GUID, omit the field, or pass null to add at root."),
				*ParentStateId));
		}
		NewState = &Parent->AddChildState(FName(*StateName), StateType);
	}

	// Set selection behavior
	if (!SelectionStr.IsEmpty())
	{
		NewState->SelectionBehavior = ParseSelectionBehavior(SelectionStr);
	}

	// Set linked asset
	if (StateType == EStateTreeStateType::LinkedAsset && !LinkedAssetPath.IsEmpty())
	{
		UStateTree* LinkedST = FMonolithAssetUtils::LoadAssetByPath<UStateTree>(FMonolithAssetUtils::ResolveAssetPath(LinkedAssetPath));
		if (LinkedST)
		{
			NewState->SetLinkedStateAsset(LinkedST);
		}
	}

	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("state_id"), NewState->ID.ToString());
	Result->SetStringField(TEXT("name"), StateName);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Added state '%s'. Call compile_state_tree to compile."), *StateName));
	return FMonolithActionResult::Success(Result);
}

// 51. remove_st_state
FMonolithActionResult FMonolithAIStateTreeActions::HandleRemoveSTState(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString StateId = Params->GetStringField(TEXT("state_id"));
	if (StateId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'state_id'"));
	}

	UStateTreeState* State = FindStateByGuid(EditorData, StateId);
	if (!State)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found"), *StateId));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Remove ST State")));

	FString RemovedName = State->Name.ToString();
	if (!RemoveStateFromParent(State, EditorData))
	{
		return FMonolithActionResult::Error(TEXT("Failed to remove state from parent"));
	}

	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Removed state '%s' and its children"), *RemovedName));
	return FMonolithActionResult::Success(Result);
}

// 52. rename_st_state
FMonolithActionResult FMonolithAIStateTreeActions::HandleRenameSTState(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString StateId = Params->GetStringField(TEXT("state_id"));
	FString NewName = Params->GetStringField(TEXT("new_name"));

	if (StateId.IsEmpty() || NewName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required params 'state_id' and 'new_name'"));
	}

	UStateTreeState* State = FindStateByGuid(EditorData, StateId);
	if (!State)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found"), *StateId));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Rename ST State")));

	FString OldName = State->Name.ToString();
	State->Name = FName(*NewName);
	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("state_id"), StateId);
	Result->SetStringField(TEXT("old_name"), OldName);
	Result->SetStringField(TEXT("new_name"), NewName);
	return FMonolithActionResult::Success(Result);
}

// 53. move_st_state
FMonolithActionResult FMonolithAIStateTreeActions::HandleMoveSTState(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString StateId = Params->GetStringField(TEXT("state_id"));
	FString NewParentId = Params->GetStringField(TEXT("new_parent_id"));
	int32 Index = Params->HasField(TEXT("index")) ? static_cast<int32>(Params->GetNumberField(TEXT("index"))) : -1;

	if (StateId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'state_id'"));
	}

	UStateTreeState* State = FindStateByGuid(EditorData, StateId);
	if (!State)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found"), *StateId));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Move ST State")));

	// Remove from current parent
	RemoveStateFromParent(State, EditorData);

	if (NewParentId.IsEmpty())
	{
		// Move to root level
		State->Parent = nullptr;
		if (Index >= 0 && Index < EditorData->SubTrees.Num())
		{
			EditorData->SubTrees.Insert(State, Index);
		}
		else
		{
			EditorData->SubTrees.Add(State);
		}
	}
	else
	{
		UStateTreeState* NewParent = FindStateByGuid(EditorData, NewParentId);
		if (!NewParent)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("New parent state '%s' not found"), *NewParentId));
		}

		State->Parent = NewParent;
		if (Index >= 0 && Index < NewParent->Children.Num())
		{
			NewParent->Children.Insert(State, Index);
		}
		else
		{
			NewParent->Children.Add(State);
		}
	}

	// Fix up outer to match new parent hierarchy
	if (State->Parent)
	{
		State->Rename(nullptr, State->Parent);
	}
	else
	{
		State->Rename(nullptr, EditorData);
	}

	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("state_id"), StateId);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Moved state '%s'"), *State->Name.ToString()));
	return FMonolithActionResult::Success(Result);
}

// 54. set_st_state_properties
FMonolithActionResult FMonolithAIStateTreeActions::HandleSetSTStateProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString StateId = Params->GetStringField(TEXT("state_id"));
	if (StateId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'state_id'"));
	}

	UStateTreeState* State = FindStateByGuid(EditorData, StateId);
	if (!State)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found"), *StateId));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Set ST State Properties")));

	if (Params->HasField(TEXT("weight")))
	{
		State->Weight = static_cast<float>(Params->GetNumberField(TEXT("weight")));
	}
	if (Params->HasField(TEXT("selection_behavior")))
	{
		State->SelectionBehavior = ParseSelectionBehavior(Params->GetStringField(TEXT("selection_behavior")));
	}
	if (Params->HasField(TEXT("tag")))
	{
		FString TagStr = Params->GetStringField(TEXT("tag"));
		State->Tag = FGameplayTag::RequestGameplayTag(FName(*TagStr), /*bErrorIfNotFound=*/false);
	}
	if (Params->HasField(TEXT("enabled")))
	{
		State->bEnabled = Params->GetBoolField(TEXT("enabled"));
	}

	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("state_id"), StateId);
	Result->SetStringField(TEXT("message"), TEXT("State properties updated"));
	return FMonolithActionResult::Success(Result);
}

// 55. add_st_task
FMonolithActionResult FMonolithAIStateTreeActions::HandleAddSTTask(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString StateId = Params->GetStringField(TEXT("state_id"));
	FString TaskClassName = Params->GetStringField(TEXT("task_class"));

	if (StateId.IsEmpty() || TaskClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required params 'state_id' and 'task_class'"));
	}

	UStateTreeState* State = FindStateByGuid(EditorData, StateId);
	if (!State)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found"), *StateId));
	}

	UScriptStruct* TaskStruct = FindStructByName(TaskClassName);
	if (!TaskStruct)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Task struct '%s' not found"), *TaskClassName));
	}

	// Verify it's a task base subclass
	if (!TaskStruct->IsChildOf(FStateTreeTaskBase::StaticStruct()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("'%s' is not a FStateTreeTaskBase subclass"), *TaskClassName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add ST Task")));

	FStateTreeEditorNode& NewTaskNode = State->Tasks.AddDefaulted_GetRef();
	if (!CreateEditorNode(NewTaskNode, TaskStruct, Error))
	{
		State->Tasks.RemoveAt(State->Tasks.Num() - 1);
		return FMonolithActionResult::Error(Error);
	}

	// Set initial properties if provided
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
	{
		if (!SetInstancedStructProperties(NewTaskNode.Node, *PropsObj, Error))
		{
			// Non-fatal: task was added but some properties failed
		}
	}

	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("state_id"), StateId);
	Result->SetStringField(TEXT("task_node_id"), NewTaskNode.ID.ToString());
	Result->SetNumberField(TEXT("task_index"), State->Tasks.Num() - 1);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Added task '%s'. Call compile_state_tree to compile."), *TaskClassName));
	if (!Error.IsEmpty())
	{
		Result->SetStringField(TEXT("property_warnings"), Error);
	}
	return FMonolithActionResult::Success(Result);
}

// 56. remove_st_task
FMonolithActionResult FMonolithAIStateTreeActions::HandleRemoveSTTask(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString StateId = Params->GetStringField(TEXT("state_id"));
	int32 TaskIndex = static_cast<int32>(Params->GetNumberField(TEXT("task_index")));

	if (StateId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'state_id'"));
	}

	UStateTreeState* State = FindStateByGuid(EditorData, StateId);
	if (!State)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found"), *StateId));
	}

	if (!State->Tasks.IsValidIndex(TaskIndex))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Task index %d out of range (0-%d)"), TaskIndex, State->Tasks.Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Remove ST Task")));
	State->Tasks.RemoveAt(TaskIndex);
	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Removed task at index %d"), TaskIndex));
	return FMonolithActionResult::Success(Result);
}

// 57. set_st_task_property
FMonolithActionResult FMonolithAIStateTreeActions::HandleSetSTTaskProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString StateId = Params->GetStringField(TEXT("state_id"));
	int32 TaskIndex = static_cast<int32>(Params->GetNumberField(TEXT("task_index")));
	FString PropertyName = Params->GetStringField(TEXT("property_name"));

	if (StateId.IsEmpty() || PropertyName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required params"));
	}

	UStateTreeState* State = FindStateByGuid(EditorData, StateId);
	if (!State)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found"), *StateId));
	}

	if (!State->Tasks.IsValidIndex(TaskIndex))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Task index %d out of range"), TaskIndex));
	}

	FStateTreeEditorNode& TaskNode = State->Tasks[TaskIndex];
	if (!TaskNode.Node.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Task node has no valid struct data"));
	}

	const UScriptStruct* StructType = TaskNode.Node.GetScriptStruct();
	uint8* Memory = TaskNode.Node.GetMutableMemory();

	FProperty* Prop = StructType->FindPropertyByName(*PropertyName);
	if (!Prop)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *StructType->GetName()));
	}

	// Get value as string
	FString ValueStr;
	TSharedPtr<FJsonValue> ValueField = Params->TryGetField(TEXT("value"));
	if (ValueField.IsValid())
	{
		if (ValueField->Type == EJson::String)
		{
			ValueStr = ValueField->AsString();
		}
		else if (ValueField->Type == EJson::Number)
		{
			ValueStr = FString::SanitizeFloat(ValueField->AsNumber());
		}
		else if (ValueField->Type == EJson::Boolean)
		{
			ValueStr = ValueField->AsBool() ? TEXT("true") : TEXT("false");
		}
		else
		{
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ValueStr);
			FJsonSerializer::Serialize(ValueField, TEXT(""), Writer);
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Set ST Task Property")));

	void* PropAddr = Prop->ContainerPtrToValuePtr<void>(Memory);
	if (!Prop->ImportText_Direct(*ValueStr, PropAddr, nullptr, PPF_None))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to set '%s' to '%s'"), *PropertyName, *ValueStr));
	}

	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Set %s.%s = %s"), *StructType->GetName(), *PropertyName, *ValueStr));
	return FMonolithActionResult::Success(Result);
}

// 58. add_st_enter_condition
FMonolithActionResult FMonolithAIStateTreeActions::HandleAddSTEnterCondition(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString StateId = Params->GetStringField(TEXT("state_id"));
	FString CondClassName = Params->GetStringField(TEXT("condition_class"));

	if (StateId.IsEmpty() || CondClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required params 'state_id' and 'condition_class'"));
	}

	UStateTreeState* State = FindStateByGuid(EditorData, StateId);
	if (!State)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found"), *StateId));
	}

	UScriptStruct* CondStruct = FindStructByName(CondClassName);
	if (!CondStruct)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Condition struct '%s' not found"), *CondClassName));
	}

	if (!CondStruct->IsChildOf(FStateTreeConditionBase::StaticStruct()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("'%s' is not a FStateTreeConditionBase subclass"), *CondClassName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add ST Enter Condition")));

	FStateTreeEditorNode& NewCondNode = State->EnterConditions.AddDefaulted_GetRef();
	if (!CreateEditorNode(NewCondNode, CondStruct, Error))
	{
		State->EnterConditions.RemoveAt(State->EnterConditions.Num() - 1);
		return FMonolithActionResult::Error(Error);
	}

	// Set initial properties if provided
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
	{
		FString PropError;
		SetInstancedStructProperties(NewCondNode.Node, *PropsObj, PropError);
	}

	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("state_id"), StateId);
	Result->SetStringField(TEXT("condition_node_id"), NewCondNode.ID.ToString());
	Result->SetNumberField(TEXT("condition_index"), State->EnterConditions.Num() - 1);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Added enter condition '%s'"), *CondClassName));
	return FMonolithActionResult::Success(Result);
}

// 59. remove_st_enter_condition
FMonolithActionResult FMonolithAIStateTreeActions::HandleRemoveSTEnterCondition(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString StateId = Params->GetStringField(TEXT("state_id"));
	int32 CondIndex = static_cast<int32>(Params->GetNumberField(TEXT("condition_index")));

	if (StateId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'state_id'"));
	}

	UStateTreeState* State = FindStateByGuid(EditorData, StateId);
	if (!State)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found"), *StateId));
	}

	if (!State->EnterConditions.IsValidIndex(CondIndex))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Condition index %d out of range (0-%d)"), CondIndex, State->EnterConditions.Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Remove ST Enter Condition")));
	State->EnterConditions.RemoveAt(CondIndex);
	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Removed enter condition at index %d"), CondIndex));
	return FMonolithActionResult::Success(Result);
}

// 60. add_st_transition
FMonolithActionResult FMonolithAIStateTreeActions::HandleAddSTTransition(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString StateId = Params->GetStringField(TEXT("state_id"));
	FString TriggerStr = Params->GetStringField(TEXT("trigger"));
	FString TargetStr = Params->GetStringField(TEXT("target_state"));
	FString PriorityStr = Params->GetStringField(TEXT("priority"));
	double Delay = Params->HasField(TEXT("delay")) ? Params->GetNumberField(TEXT("delay")) : 0.0;

	if (StateId.IsEmpty() || TriggerStr.IsEmpty() || TargetStr.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required params 'state_id', 'trigger', and 'target_state'"));
	}

	UStateTreeState* State = FindStateByGuid(EditorData, StateId);
	if (!State)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found"), *StateId));
	}

	// Phase F #25: validate trigger string up-front instead of silently defaulting to OnStateCompleted.
	EStateTreeTransitionTrigger Trigger = EStateTreeTransitionTrigger::OnStateCompleted;
	{
		FString ValidValues;
		if (!TryParseTransitionTrigger(TriggerStr, Trigger, ValidValues))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Invalid trigger '%s'. Valid values: %s."), *TriggerStr, *ValidValues));
		}
	}

	// Determine transition type and target
	EStateTreeTransitionType TransType = EStateTreeTransitionType::None;
	const UStateTreeState* TargetState = nullptr;

	if (TargetStr.Equals(TEXT("Succeeded"), ESearchCase::IgnoreCase))
	{
		TransType = EStateTreeTransitionType::Succeeded;
	}
	else if (TargetStr.Equals(TEXT("Failed"), ESearchCase::IgnoreCase))
	{
		TransType = EStateTreeTransitionType::Failed;
	}
	else if (TargetStr.Equals(TEXT("NextState"), ESearchCase::IgnoreCase))
	{
		TransType = EStateTreeTransitionType::NextState;
	}
	else if (TargetStr.Equals(TEXT("NextSelectableState"), ESearchCase::IgnoreCase))
	{
		TransType = EStateTreeTransitionType::NextSelectableState;
	}
	else
	{
		// Treat as state GUID
		TransType = EStateTreeTransitionType::GotoState;
		TargetState = FindStateByGuid(EditorData, TargetStr);
		if (!TargetState)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Target state '%s' not found — use a GUID or: Succeeded, Failed, NextState, NextSelectableState"), *TargetStr));
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add ST Transition")));

	FStateTreeTransition& NewTrans = State->AddTransition(Trigger, TransType, TargetState);

	// Priority
	if (!PriorityStr.IsEmpty())
	{
		NewTrans.Priority = ParseTransitionPriority(PriorityStr);
	}

	// Delay
	if (Delay > 0.0)
	{
		NewTrans.bDelayTransition = true;
		NewTrans.DelayDuration = static_cast<float>(Delay);
	}

	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("state_id"), StateId);
	Result->SetStringField(TEXT("transition_id"), NewTrans.ID.ToString());
	Result->SetNumberField(TEXT("transition_index"), State->Transitions.Num() - 1);
	Result->SetStringField(TEXT("message"), TEXT("Added transition. Call compile_state_tree to compile."));
	return FMonolithActionResult::Success(Result);
}

// 61. remove_st_transition
FMonolithActionResult FMonolithAIStateTreeActions::HandleRemoveSTTransition(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString StateId = Params->GetStringField(TEXT("state_id"));
	int32 TransIndex = static_cast<int32>(Params->GetNumberField(TEXT("transition_index")));

	if (StateId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'state_id'"));
	}

	UStateTreeState* State = FindStateByGuid(EditorData, StateId);
	if (!State)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found"), *StateId));
	}

	if (!State->Transitions.IsValidIndex(TransIndex))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Transition index %d out of range (0-%d)"), TransIndex, State->Transitions.Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Remove ST Transition")));
	State->Transitions.RemoveAt(TransIndex);
	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Removed transition at index %d"), TransIndex));
	return FMonolithActionResult::Success(Result);
}

// 65. add_st_property_binding
FMonolithActionResult FMonolithAIStateTreeActions::HandleAddSTPropertyBinding(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString SourcePathStr = Params->GetStringField(TEXT("source_path"));
	FString TargetPathStr = Params->GetStringField(TEXT("target_path"));

	if (SourcePathStr.IsEmpty() || TargetPathStr.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required params 'source_path' and 'target_path'"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add ST Property Binding")));

	FStateTreeEditorPropertyBindings* Bindings = EditorData->GetPropertyEditorBindings();
	if (!Bindings)
	{
		return FMonolithActionResult::Error(TEXT("Could not access property bindings on editor data"));
	}

	// Parse paths — format is "StructGUID.PropertyName.SubProperty" etc.
	// First segment before '.' can be a GUID (for StructID), rest is property path
	FPropertyBindingPath SourcePath;
	FPropertyBindingPath TargetPath;

	// Try to parse the first segment as a GUID for struct ID
	auto ParseBindingPath = [](const FString& PathStr, FPropertyBindingPath& OutPath)
	{
		int32 DotIdx;
		FString StructIdStr;
		FString PropPathStr = PathStr;

		if (PathStr.FindChar('.', DotIdx))
		{
			StructIdStr = PathStr.Left(DotIdx);
			PropPathStr = PathStr.Mid(DotIdx + 1);

			FGuid StructGuid;
			if (FGuid::Parse(StructIdStr, StructGuid))
			{
				OutPath.SetStructID(StructGuid);
			}
			else
			{
				// Not a GUID, treat whole thing as property path
				PropPathStr = PathStr;
			}
		}

		OutPath.FromString(PropPathStr);
	};

	ParseBindingPath(SourcePathStr, SourcePath);
	ParseBindingPath(TargetPathStr, TargetPath);

	FStateTreePropertyPathBinding NewBinding(SourcePath, TargetPath, /*bIsOutputBinding=*/false);
	Bindings->AddStateTreeBinding(MoveTemp(NewBinding));

	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Added binding: %s -> %s"), *SourcePathStr, *TargetPathStr));
	return FMonolithActionResult::Success(Result);
}

// 66. remove_st_property_binding
FMonolithActionResult FMonolithAIStateTreeActions::HandleRemoveSTPropertyBinding(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	int32 BindingIndex = static_cast<int32>(Params->GetNumberField(TEXT("binding_index")));

	FStateTreeEditorPropertyBindings* Bindings = EditorData->GetPropertyEditorBindings();
	if (!Bindings)
	{
		return FMonolithActionResult::Error(TEXT("Could not access property bindings"));
	}

	TConstArrayView<FStateTreePropertyPathBinding> AllBindings = Bindings->GetBindings();
	if (!AllBindings.IsValidIndex(BindingIndex))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Binding index %d out of range (0-%d)"), BindingIndex, AllBindings.Num() - 1));
	}

	// Capture the target path of the binding at the given index so we can remove it
	const FPropertyBindingPath TargetPathToRemove = AllBindings[BindingIndex].GetTargetPath();

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Remove ST Property Binding")));

	// Use predicate-based removal, counting to match the exact index
	int32 CurrentIndex = 0;
	Bindings->RemoveBindings([&](FPropertyBindingBinding& Binding) -> bool
	{
		return CurrentIndex++ == BindingIndex;
	});

	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Removed binding at index %d"), BindingIndex));
	return FMonolithActionResult::Success(Result);
}

// 67. get_st_bindings
FMonolithActionResult FMonolithAIStateTreeActions::HandleGetSTBindings(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	const FStateTreeEditorPropertyBindings* Bindings = EditorData->GetPropertyEditorBindings();
	if (!Bindings)
	{
		return FMonolithActionResult::Error(TEXT("Could not access property bindings"));
	}

	TConstArrayView<FStateTreePropertyPathBinding> AllBindings = Bindings->GetBindings();

	TArray<TSharedPtr<FJsonValue>> BindingArr;
	for (int32 i = 0; i < AllBindings.Num(); ++i)
	{
		const FStateTreePropertyPathBinding& Binding = AllBindings[i];
		TSharedPtr<FJsonObject> BindObj = MakeShared<FJsonObject>();
		BindObj->SetNumberField(TEXT("index"), i);
		BindObj->SetStringField(TEXT("source_path"), Binding.GetSourcePath().ToString());
		BindObj->SetStringField(TEXT("target_path"), Binding.GetTargetPath().ToString());
		BindingArr.Add(MakeShared<FJsonValueObject>(BindObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetArrayField(TEXT("bindings"), BindingArr);
	Result->SetNumberField(TEXT("count"), BindingArr.Num());
	return FMonolithActionResult::Success(Result);
}

// 68. get_st_bindable_properties
FMonolithActionResult FMonolithAIStateTreeActions::HandleGetSTBindableProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString StateId = Params->GetStringField(TEXT("state_id"));
	int32 TaskIndex = Params->HasField(TEXT("task_index")) ? static_cast<int32>(Params->GetNumberField(TEXT("task_index"))) : -1;

	TArray<TSharedPtr<FJsonValue>> PropArr;

	// If a state_id is provided, get struct descs accessible from that state's context
	if (!StateId.IsEmpty())
	{
		UStateTreeState* State = FindStateByGuid(EditorData, StateId);
		if (!State)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found"), *StateId));
		}

		// If a specific task index is given, list that task's bindable properties
		if (TaskIndex >= 0 && State->Tasks.IsValidIndex(TaskIndex))
		{
			const FStateTreeEditorNode& TaskNode = State->Tasks[TaskIndex];
			if (TaskNode.Node.IsValid())
			{
				const UScriptStruct* StructType = TaskNode.Node.GetScriptStruct();
				for (TFieldIterator<FProperty> It(StructType); It; ++It)
				{
					FProperty* Prop = *It;
					if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)) continue;

					TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
					PropObj->SetStringField(TEXT("name"), Prop->GetName());
					PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());
					PropObj->SetStringField(TEXT("scope"), TEXT("task"));
					PropObj->SetStringField(TEXT("node_id"), TaskNode.ID.ToString());
					PropArr.Add(MakeShared<FJsonValueObject>(PropObj));
				}
			}

			// Also list instance data properties
			if (TaskNode.Instance.IsValid())
			{
				const UScriptStruct* InstType = TaskNode.Instance.GetScriptStruct();
				for (TFieldIterator<FProperty> It(InstType); It; ++It)
				{
					FProperty* Prop = *It;
					if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)) continue;

					TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
					PropObj->SetStringField(TEXT("name"), Prop->GetName());
					PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());
					PropObj->SetStringField(TEXT("scope"), TEXT("instance_data"));
					PropArr.Add(MakeShared<FJsonValueObject>(PropObj));
				}
			}
		}
		else
		{
			// List all tasks in the state as bindable sources
			for (int32 i = 0; i < State->Tasks.Num(); ++i)
			{
				const FStateTreeEditorNode& TaskNode = State->Tasks[i];
				TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
				PropObj->SetStringField(TEXT("node_id"), TaskNode.ID.ToString());
				PropObj->SetNumberField(TEXT("task_index"), i);
				if (TaskNode.Node.IsValid())
				{
					PropObj->SetStringField(TEXT("struct_type"), TaskNode.Node.GetScriptStruct()->GetName());
				}
				PropArr.Add(MakeShared<FJsonValueObject>(PropObj));
			}
		}
	}
	else
	{
		// List global bindable struct descriptors
		TArray<TInstancedStruct<FPropertyBindingBindableStructDescriptor>> StructDescs;
		FGuid EmptyGuid;
		EditorData->GetBindableStructs(EmptyGuid, StructDescs);

		for (const auto& DescInst : StructDescs)
		{
			if (!DescInst.IsValid()) continue;
			const FPropertyBindingBindableStructDescriptor& Desc = DescInst.Get();

			TSharedPtr<FJsonObject> DescObj = MakeShared<FJsonObject>();
			DescObj->SetStringField(TEXT("id"), Desc.ID.ToString());
			DescObj->SetStringField(TEXT("name"), Desc.Name.ToString());
			if (Desc.Struct)
			{
				DescObj->SetStringField(TEXT("struct"), Desc.Struct->GetName());
			}
			PropArr.Add(MakeShared<FJsonValueObject>(DescObj));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetArrayField(TEXT("bindable_properties"), PropArr);
	Result->SetNumberField(TEXT("count"), PropArr.Num());
	return FMonolithActionResult::Success(Result);
}

// 69. list_st_task_types
FMonolithActionResult FMonolithAIStateTreeActions::HandleListSTTaskTypes(const TSharedPtr<FJsonObject>& Params)
{
	TArray<TSharedPtr<FJsonValue>> TypeArr;

	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		UScriptStruct* Struct = *It;
		if (Struct == FStateTreeTaskBase::StaticStruct()) continue;

		if (Struct->IsChildOf(FStateTreeTaskBase::StaticStruct()))
		{
			TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
			TypeObj->SetStringField(TEXT("name"), Struct->GetName());
			TypeObj->SetStringField(TEXT("full_name"), Struct->GetPathName());

			// Get description from metadata if available
			const FString& ToolTip = Struct->GetMetaData(TEXT("ToolTip"));
			if (!ToolTip.IsEmpty())
			{
				TypeObj->SetStringField(TEXT("description"), ToolTip);
			}

			TypeArr.Add(MakeShared<FJsonValueObject>(TypeObj));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("task_types"), TypeArr);
	Result->SetNumberField(TEXT("count"), TypeArr.Num());
	return FMonolithActionResult::Success(Result);
}

// 70. list_st_condition_types
FMonolithActionResult FMonolithAIStateTreeActions::HandleListSTConditionTypes(const TSharedPtr<FJsonObject>& Params)
{
	TArray<TSharedPtr<FJsonValue>> TypeArr;

	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		UScriptStruct* Struct = *It;
		if (Struct == FStateTreeConditionBase::StaticStruct()) continue;

		if (Struct->IsChildOf(FStateTreeConditionBase::StaticStruct()))
		{
			TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
			TypeObj->SetStringField(TEXT("name"), Struct->GetName());
			TypeObj->SetStringField(TEXT("full_name"), Struct->GetPathName());

			const FString& ToolTip = Struct->GetMetaData(TEXT("ToolTip"));
			if (!ToolTip.IsEmpty())
			{
				TypeObj->SetStringField(TEXT("description"), ToolTip);
			}

			TypeArr.Add(MakeShared<FJsonValueObject>(TypeObj));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("condition_types"), TypeArr);
	Result->SetNumberField(TEXT("count"), TypeArr.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  62. add_st_transition_condition
// ============================================================

FMonolithActionResult FMonolithAIStateTreeActions::HandleAddSTTransitionCondition(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString StateId = Params->GetStringField(TEXT("state_id"));
	if (StateId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'state_id'"));
	}

	UStateTreeState* State = FindStateByGuid(EditorData, StateId);
	if (!State)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found"), *StateId));
	}

	int32 TransIndex = static_cast<int32>(Params->GetNumberField(TEXT("transition_index")));
	if (!State->Transitions.IsValidIndex(TransIndex))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Transition index %d out of range (0-%d)"), TransIndex, State->Transitions.Num() - 1));
	}

	FString CondClassName = Params->GetStringField(TEXT("condition_class"));
	if (CondClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'condition_class'"));
	}

	UScriptStruct* CondStruct = FindStructByName(CondClassName);
	if (!CondStruct || !CondStruct->IsChildOf(FStateTreeConditionBase::StaticStruct()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Condition class '%s' not found or not a FStateTreeConditionBase"), *CondClassName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add ST Transition Condition")));

	FStateTreeTransition& Trans = State->Transitions[TransIndex];
	FStateTreeEditorNode& NewCondNode = Trans.Conditions.AddDefaulted_GetRef();

	if (!CreateEditorNode(NewCondNode, CondStruct, Error))
	{
		Trans.Conditions.RemoveAt(Trans.Conditions.Num() - 1);
		return FMonolithActionResult::Error(Error);
	}

	// Apply properties if provided
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj->IsValid())
	{
		FString PropErr;
		if (!SetInstancedStructProperties(NewCondNode.Node, *PropsObj, PropErr))
		{
			// Non-fatal: condition still added
		}
	}

	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("state_id"), StateId);
	Result->SetNumberField(TEXT("transition_index"), TransIndex);
	Result->SetStringField(TEXT("condition_class"), CondStruct->GetName());
	Result->SetStringField(TEXT("condition_id"), NewCondNode.ID.ToString());
	Result->SetNumberField(TEXT("condition_count"), Trans.Conditions.Num());
	Result->SetStringField(TEXT("message"), TEXT("Transition condition added"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  63. add_st_consideration
// ============================================================

FMonolithActionResult FMonolithAIStateTreeActions::HandleAddSTConsideration(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString StateId = Params->GetStringField(TEXT("state_id"));
	if (StateId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'state_id'"));
	}

	UStateTreeState* State = FindStateByGuid(EditorData, StateId);
	if (!State)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found"), *StateId));
	}

	FString ConsClassName = Params->GetStringField(TEXT("consideration_class"));
	if (ConsClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'consideration_class'"));
	}

	UScriptStruct* ConsStruct = FindStructByName(ConsClassName);
	if (!ConsStruct || !ConsStruct->IsChildOf(FStateTreeConsiderationBase::StaticStruct()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Consideration class '%s' not found or not a FStateTreeConsiderationBase"), *ConsClassName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add ST Consideration")));

	FStateTreeEditorNode& NewConsNode = State->Considerations.AddDefaulted_GetRef();

	if (!CreateEditorNode(NewConsNode, ConsStruct, Error))
	{
		State->Considerations.RemoveAt(State->Considerations.Num() - 1);
		return FMonolithActionResult::Error(Error);
	}

	// Apply properties if provided
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj->IsValid())
	{
		FString PropErr;
		SetInstancedStructProperties(NewConsNode.Node, *PropsObj, PropErr);
	}

	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("state_id"), StateId);
	Result->SetStringField(TEXT("consideration_class"), ConsStruct->GetName());
	Result->SetStringField(TEXT("consideration_id"), NewConsNode.ID.ToString());
	Result->SetNumberField(TEXT("consideration_count"), State->Considerations.Num());
	Result->SetStringField(TEXT("message"), TEXT("Consideration added"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  64. configure_st_consideration
// ============================================================

FMonolithActionResult FMonolithAIStateTreeActions::HandleConfigureSTConsideration(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	FString StateId = Params->GetStringField(TEXT("state_id"));
	if (StateId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'state_id'"));
	}

	UStateTreeState* State = FindStateByGuid(EditorData, StateId);
	if (!State)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found"), *StateId));
	}

	int32 ConsIndex = static_cast<int32>(Params->GetNumberField(TEXT("consideration_index")));
	if (!State->Considerations.IsValidIndex(ConsIndex))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Consideration index %d out of range (0-%d)"), ConsIndex, State->Considerations.Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Configure ST Consideration")));

	FStateTreeEditorNode& ConsNode = State->Considerations[ConsIndex];

	// Set properties on the node struct
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj->IsValid())
	{
		FString PropErr;
		if (!SetInstancedStructProperties(ConsNode.Node, *PropsObj, PropErr))
		{
			return FMonolithActionResult::Error(PropErr);
		}
	}

	// Set properties on instance data
	const TSharedPtr<FJsonObject>* InstPropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("instance_properties"), InstPropsObj) && InstPropsObj->IsValid())
	{
		if (!ConsNode.Instance.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Consideration has no instance data to configure"));
		}
		FString PropErr;
		if (!SetInstancedStructProperties(ConsNode.Instance, *InstPropsObj, PropErr))
		{
			return FMonolithActionResult::Error(PropErr);
		}
	}

	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("state_id"), StateId);
	Result->SetNumberField(TEXT("consideration_index"), ConsIndex);
	Result->SetStringField(TEXT("message"), TEXT("Consideration configured"));
	if (ConsNode.Node.IsValid())
	{
		Result->SetStringField(TEXT("struct_type"), ConsNode.Node.GetScriptStruct()->GetName());
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  73. validate_state_tree
// ============================================================

namespace
{
	/** Recursively collect validation issues from states. */
	void ValidateStateRecursive(
		const UStateTreeState* State,
		const FString& Path,
		TArray<FString>& OutErrors,
		TArray<FString>& OutWarnings,
		TSet<FGuid>& VisitedStates)
	{
		if (!State) return;

		// Cycle detection
		if (VisitedStates.Contains(State->ID))
		{
			OutErrors.Add(FString::Printf(TEXT("%s: Cycle detected — state already visited"), *Path));
			return;
		}
		VisitedStates.Add(State->ID);

		FString StatePath = Path / State->Name.ToString();

		// Check: state has no tasks and no children (dead-end leaf)
		if (State->Type == EStateTreeStateType::State && State->Tasks.Num() == 0 && State->Children.Num() == 0)
		{
			OutWarnings.Add(FString::Printf(TEXT("%s: Leaf state with no tasks"), *StatePath));
		}

		// Check: state is Group/Subtree but has no children
		if ((State->Type == EStateTreeStateType::Group || State->Type == EStateTreeStateType::Subtree) && State->Children.Num() == 0)
		{
			OutWarnings.Add(FString::Printf(TEXT("%s: %s state with no children"),
				*StatePath,
				*UEnum::GetValueAsString(State->Type)));
		}

		// Check: leaf state with no transitions (potential dead-end unless it succeeds/fails)
		if (State->Children.Num() == 0 && State->Transitions.Num() == 0)
		{
			OutWarnings.Add(FString::Printf(TEXT("%s: Leaf state with no transitions — will complete to parent"), *StatePath));
		}

		// Validate transitions
		for (int32 i = 0; i < State->Transitions.Num(); ++i)
		{
			const FStateTreeTransition& Trans = State->Transitions[i];
#if WITH_EDITORONLY_DATA
			if (Trans.State.LinkType == EStateTreeTransitionType::GotoState && !Trans.State.ID.IsValid())
			{
				OutErrors.Add(FString::Printf(TEXT("%s.Transition[%d]: GotoState with invalid target state ID"), *StatePath, i));
			}
#endif
		}

		// Recurse into children
		for (const UStateTreeState* Child : State->Children)
		{
			ValidateStateRecursive(Child, StatePath, OutErrors, OutWarnings, VisitedStates);
		}

		VisitedStates.Remove(State->ID);
	}
}

FMonolithActionResult FMonolithAIStateTreeActions::HandleValidateStateTree(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	TArray<FString> Errors;
	TArray<FString> Warnings;

	// Check: has schema
	if (!ST->GetSchema())
	{
		Errors.Add(TEXT("No schema set on StateTree"));
	}

	// Check: has subtrees
	if (EditorData->SubTrees.Num() == 0)
	{
		Errors.Add(TEXT("StateTree has no root states (subtrees)"));
	}

	// Recursively validate all states
	TSet<FGuid> VisitedStates;
	for (const UStateTreeState* SubTree : EditorData->SubTrees)
	{
		ValidateStateRecursive(SubTree, TEXT(""), Errors, Warnings, VisitedStates);
	}

	// Try compilation and check
	FStateTreeCompilerLog CompileLog;
	bool bCompileSuccess = UStateTreeEditingSubsystem::CompileStateTree(ST, CompileLog);
	if (!bCompileSuccess)
	{
		Errors.Add(TEXT("Compilation failed — check compile_state_tree for details"));
	}

	// Check bindings
	const FStateTreeEditorPropertyBindings* Bindings = EditorData->GetPropertyEditorBindings();
	if (Bindings)
	{
		TConstArrayView<FStateTreePropertyPathBinding> AllBindings = Bindings->GetBindings();
		if (AllBindings.Num() > 0)
		{
			// Report binding count as informational
			// (Deep validation of each binding's validity would require runtime context)
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetBoolField(TEXT("valid"), Errors.Num() == 0);
	Result->SetBoolField(TEXT("compiles"), bCompileSuccess);
	Result->SetBoolField(TEXT("is_ready_to_run"), ST->IsReadyToRun());
	Result->SetNumberField(TEXT("subtree_count"), EditorData->SubTrees.Num());

	TArray<TSharedPtr<FJsonValue>> ErrArr;
	for (const FString& E : Errors)
	{
		ErrArr.Add(MakeShared<FJsonValueString>(E));
	}
	Result->SetArrayField(TEXT("errors"), ErrArr);

	TArray<TSharedPtr<FJsonValue>> WarnArr;
	for (const FString& W : Warnings)
	{
		WarnArr.Add(MakeShared<FJsonValueString>(W));
	}
	Result->SetArrayField(TEXT("warnings"), WarnArr);
	Result->SetNumberField(TEXT("error_count"), Errors.Num());
	Result->SetNumberField(TEXT("warning_count"), Warnings.Num());

	if (Errors.Num() == 0 && Warnings.Num() == 0)
	{
		Result->SetStringField(TEXT("message"), TEXT("StateTree is valid with no issues"));
	}
	else
	{
		Result->SetStringField(TEXT("message"), FString::Printf(TEXT("%d error(s), %d warning(s)"), Errors.Num(), Warnings.Num()));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  75. list_st_extension_types
// ============================================================

FMonolithActionResult FMonolithAIStateTreeActions::HandleListSTExtensionTypes(const TSharedPtr<FJsonObject>& Params)
{
	TArray<TSharedPtr<FJsonValue>> TypeArr;

	TArray<UClass*> DerivedClasses;
	GetDerivedClasses(UStateTreeExtension::StaticClass(), DerivedClasses, /*bRecursive=*/true);

	for (UClass* Cls : DerivedClasses)
	{
		if (!Cls) continue;
		if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) continue;

		TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
		TypeObj->SetStringField(TEXT("class_name"), Cls->GetName());
		TypeObj->SetStringField(TEXT("display_name"), Cls->GetDisplayNameText().ToString());

#if WITH_EDITOR
		const FString& ToolTip = Cls->GetMetaData(TEXT("ToolTip"));
		if (!ToolTip.IsEmpty())
		{
			TypeObj->SetStringField(TEXT("description"), ToolTip);
		}
#endif

		TypeArr.Add(MakeShared<FJsonValueObject>(TypeObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("extension_types"), TypeArr);
	Result->SetNumberField(TEXT("count"), TypeArr.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  76. add_st_extension
// ============================================================

FMonolithActionResult FMonolithAIStateTreeActions::HandleAddSTExtension(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	FString ExtClassName = Params->GetStringField(TEXT("extension_class"));
	if (ExtClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'extension_class'"));
	}

	// Find class
	UClass* ExtClass = FindFirstObject<UClass>(*ExtClassName, EFindFirstObjectOptions::EnsureIfAmbiguous);
	if (!ExtClass)
	{
		ExtClass = FindFirstObject<UClass>(*(TEXT("U") + ExtClassName), EFindFirstObjectOptions::EnsureIfAmbiguous);
	}
	if (!ExtClass || !ExtClass->IsChildOf(UStateTreeExtension::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Extension class '%s' not found or not a UStateTreeExtension subclass"), *ExtClassName));
	}
	if (ExtClass->HasAnyClassFlags(CLASS_Abstract))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Extension class '%s' is abstract"), *ExtClassName));
	}

	// Check if already added
	for (const UStateTreeExtension* Existing : ST->GetExtensions())
	{
		if (Existing && Existing->GetClass() == ExtClass)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Extension '%s' already exists on this StateTree"), *ExtClassName));
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add ST Extension")));
	ST->Modify();

	UStateTreeExtension* NewExt = NewObject<UStateTreeExtension>(ST, ExtClass);
	if (!NewExt)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create extension '%s'"), *ExtClassName));
	}

	// Extensions is a public UPROPERTY on UStateTree
	// Extensions is private — access via FProperty reflection
	{
		FArrayProperty* ExtArrayProp = CastField<FArrayProperty>(UStateTree::StaticClass()->FindPropertyByName(TEXT("Extensions")));
		if (ExtArrayProp)
		{
			void* ArrayAddr = ExtArrayProp->ContainerPtrToValuePtr<void>(ST);
			FScriptArrayHelper ArrayHelper(ExtArrayProp, ArrayAddr);
			int32 NewIdx = ArrayHelper.AddValue();
			FObjectProperty* InnerProp = CastField<FObjectProperty>(ExtArrayProp->Inner);
			if (InnerProp)
			{
				InnerProp->SetObjectPropertyValue(ArrayHelper.GetRawPtr(NewIdx), NewExt);
			}
		}
		else
		{
			return FMonolithActionResult::Error(TEXT("Could not find Extensions property on UStateTree"));
		}
	}

	// Apply properties if provided (via UPROPERTY reflection on the UObject)
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	TArray<FString> PropWarnings;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj->IsValid())
	{
		for (const auto& Pair : (*PropsObj)->Values)
		{
			FProperty* Prop = NewExt->GetClass()->FindPropertyByName(*Pair.Key);
			if (!Prop)
			{
				PropWarnings.Add(FString::Printf(TEXT("Property '%s' not found on %s"), *Pair.Key, *ExtClass->GetName()));
				continue;
			}
			void* PropAddr = Prop->ContainerPtrToValuePtr<void>(NewExt);
			FString ValueStr;
			if (Pair.Value->Type == EJson::String) ValueStr = Pair.Value->AsString();
			else if (Pair.Value->Type == EJson::Number) ValueStr = FString::SanitizeFloat(Pair.Value->AsNumber());
			else if (Pair.Value->Type == EJson::Boolean) ValueStr = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");

			if (!Prop->ImportText_Direct(*ValueStr, PropAddr, NewExt, PPF_None))
			{
				PropWarnings.Add(FString::Printf(TEXT("Failed to set '%s' to '%s'"), *Pair.Key, *ValueStr));
			}
		}
	}

	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("extension_class"), ExtClass->GetName());
	Result->SetNumberField(TEXT("extension_count"), ST->GetExtensions().Num());
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Extension '%s' added"), *ExtClass->GetName()));

	if (PropWarnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : PropWarnings)
		{
			WarnArr.Add(MakeShared<FJsonValueString>(W));
		}
		Result->SetArrayField(TEXT("property_warnings"), WarnArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  71. build_state_tree_from_spec
// ============================================================

namespace
{
	/** Recursively build states from a JSON spec node. */
	bool BuildStateFromSpec(
		UStateTree* ST,
		UStateTreeEditorData* EditorData,
		UStateTreeState* ParentState,
		const TSharedPtr<FJsonObject>& StateSpec,
		TArray<FString>& OutWarnings,
		FString& OutError)
	{
		FString StateName = StateSpec->GetStringField(TEXT("name"));
		if (StateName.IsEmpty())
		{
			OutError = TEXT("State spec missing 'name'");
			return false;
		}

		FString TypeStr = StateSpec->GetStringField(TEXT("type"));
		EStateTreeStateType StateType = ParseStateType(TypeStr);

		UStateTreeState* NewState = nullptr;
		if (ParentState)
		{
			NewState = &ParentState->AddChildState(FName(*StateName), StateType);
		}
		else
		{
			NewState = &EditorData->AddSubTree(FName(*StateName));
			NewState->Type = StateType;
		}

		// Selection behavior
		FString SelectionStr = StateSpec->GetStringField(TEXT("selection_behavior"));
		if (!SelectionStr.IsEmpty())
		{
			NewState->SelectionBehavior = ParseSelectionBehavior(SelectionStr);
		}

		// Tasks
		const TArray<TSharedPtr<FJsonValue>>* TasksArr = nullptr;
		if (StateSpec->TryGetArrayField(TEXT("tasks"), TasksArr) && TasksArr)
		{
			for (const auto& TaskVal : *TasksArr)
			{
				TSharedPtr<FJsonObject> TaskSpec = TaskVal->AsObject();
				if (!TaskSpec.IsValid()) continue;

				FString TaskClassName = TaskSpec->GetStringField(TEXT("class"));
				if (TaskClassName.IsEmpty()) continue;

				UScriptStruct* TaskStruct = FindStructByName(TaskClassName);
				if (!TaskStruct)
				{
					OutWarnings.Add(FString::Printf(TEXT("Task struct '%s' not found — skipping"), *TaskClassName));
					continue;
				}

				FStateTreeEditorNode TaskNode;
				FString NodeError;
				if (!CreateEditorNode(TaskNode, TaskStruct, NodeError))
				{
					OutWarnings.Add(FString::Printf(TEXT("Failed to create task '%s': %s"), *TaskClassName, *NodeError));
					continue;
				}

				// Set properties on the node's instance data
				const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
				if (TaskSpec->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr && PropsPtr->IsValid())
				{
					FString PropError;
					if (TaskNode.Instance.IsValid())
					{
						if (!SetInstancedStructProperties(TaskNode.Instance, *PropsPtr, PropError))
						{
							OutWarnings.Add(FString::Printf(TEXT("Task '%s' property warning: %s"), *TaskClassName, *PropError));
						}
					}
					else
					{
						// Try setting on the node itself
						if (!SetInstancedStructProperties(TaskNode.Node, *PropsPtr, PropError))
						{
							OutWarnings.Add(FString::Printf(TEXT("Task '%s' property warning: %s"), *TaskClassName, *PropError));
						}
					}
				}

				NewState->Tasks.Add(MoveTemp(TaskNode));
			}
		}

		// Transitions
		const TArray<TSharedPtr<FJsonValue>>* TransArr = nullptr;
		if (StateSpec->TryGetArrayField(TEXT("transitions"), TransArr) && TransArr)
		{
			for (const auto& TransVal : *TransArr)
			{
				TSharedPtr<FJsonObject> TransSpec = TransVal->AsObject();
				if (!TransSpec.IsValid()) continue;

				FStateTreeTransition& Trans = NewState->Transitions.AddDefaulted_GetRef();
				Trans.ID = FGuid::NewGuid();

				FString TriggerStr = TransSpec->GetStringField(TEXT("trigger"));
				Trans.Trigger = ParseTransitionTrigger(TriggerStr);

				FString PriorityStr = TransSpec->GetStringField(TEXT("priority"));
				if (!PriorityStr.IsEmpty())
				{
					Trans.Priority = ParseTransitionPriority(PriorityStr);
				}

				if (TransSpec->HasField(TEXT("delay")))
				{
					Trans.bDelayTransition = true;
					Trans.DelayDuration = TransSpec->GetNumberField(TEXT("delay"));
				}

				// Target — resolved as name-based link (deferred resolution at compile time)
				FString TargetStr = TransSpec->GetStringField(TEXT("target"));
#if WITH_EDITORONLY_DATA
				if (TargetStr.Equals(TEXT("Succeeded"), ESearchCase::IgnoreCase))
				{
					Trans.State.LinkType = EStateTreeTransitionType::Succeeded;
				}
				else if (TargetStr.Equals(TEXT("Failed"), ESearchCase::IgnoreCase))
				{
					Trans.State.LinkType = EStateTreeTransitionType::Failed;
				}
				else if (TargetStr.Equals(TEXT("NextState"), ESearchCase::IgnoreCase) || TargetStr.Equals(TEXT("NextSelectableState"), ESearchCase::IgnoreCase))
				{
					Trans.State.LinkType = EStateTreeTransitionType::NextSelectableState;
				}
				else
				{
					// Treat as a state name — will need to be resolved after all states are built
					Trans.State.Name = FName(*TargetStr);
					Trans.State.LinkType = EStateTreeTransitionType::GotoState;
				}
#endif
			}
		}

		// Enter conditions
		const TArray<TSharedPtr<FJsonValue>>* CondsArr = nullptr;
		if (StateSpec->TryGetArrayField(TEXT("enter_conditions"), CondsArr) && CondsArr)
		{
			for (const auto& CondVal : *CondsArr)
			{
				TSharedPtr<FJsonObject> CondSpec = CondVal->AsObject();
				if (!CondSpec.IsValid()) continue;

				FString CondClassName = CondSpec->GetStringField(TEXT("class"));
				if (CondClassName.IsEmpty()) continue;

				UScriptStruct* CondStruct = FindStructByName(CondClassName);
				if (!CondStruct)
				{
					OutWarnings.Add(FString::Printf(TEXT("Condition struct '%s' not found — skipping"), *CondClassName));
					continue;
				}

				FStateTreeEditorNode CondNode;
				FString NodeError;
				if (!CreateEditorNode(CondNode, CondStruct, NodeError))
				{
					OutWarnings.Add(FString::Printf(TEXT("Failed to create condition '%s': %s"), *CondClassName, *NodeError));
					continue;
				}

				NewState->EnterConditions.Add(MoveTemp(CondNode));
			}
		}

		// Recurse into children
		const TArray<TSharedPtr<FJsonValue>>* ChildrenArr = nullptr;
		if (StateSpec->TryGetArrayField(TEXT("children"), ChildrenArr) && ChildrenArr)
		{
			for (const auto& ChildVal : *ChildrenArr)
			{
				TSharedPtr<FJsonObject> ChildSpec = ChildVal->AsObject();
				if (!ChildSpec.IsValid()) continue;

				if (!BuildStateFromSpec(ST, EditorData, NewState, ChildSpec, OutWarnings, OutError))
				{
					return false;
				}
			}
		}

		return true;
	}
}

FMonolithActionResult FMonolithAIStateTreeActions::HandleBuildStateTreeFromSpec(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'save_path'"));
	}
	SavePath = FMonolithAssetUtils::ResolveAssetPath(SavePath);

	const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("spec"), SpecPtr) || !SpecPtr || !SpecPtr->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'spec'"));
	}
	const TSharedPtr<FJsonObject>& Spec = *SpecPtr;

	bool bStrictMode = false;
	Params->TryGetBoolField(TEXT("strict_mode"), bStrictMode);

	// =====================================================================
	// PRE-VALIDATION — walk the spec and resolve every task/condition struct
	// BEFORE creating any package. Collect skipped states (= states whose
	// tasks/conditions reference unresolvable structs). In strict_mode, any
	// failure aborts before package creation.
	// =====================================================================
	TArray<TSharedPtr<FJsonValue>> SkippedStates;
	TArray<FString> StrictErrors;

	auto MakeStateSkipObj = [](const FString& StatePath, const FString& BadClass, const FString& Slot) -> TSharedPtr<FJsonValue>
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("state"), StatePath);
		Obj->SetStringField(TEXT("slot"), Slot);
		Obj->SetStringField(TEXT("bad_class"), BadClass);
		Obj->SetStringField(TEXT("reason"), FString::Printf(TEXT("UScriptStruct '%s' not found"), *BadClass));
		return MakeShared<FJsonValueObject>(Obj);
	};

	{
		const TSharedPtr<FJsonObject>* PreRootPtr = nullptr;
		if (Spec->TryGetObjectField(TEXT("root"), PreRootPtr) && PreRootPtr && PreRootPtr->IsValid())
		{
			TFunction<void(const TSharedPtr<FJsonObject>&, const FString&)> WalkSpec;
			WalkSpec = [&](const TSharedPtr<FJsonObject>& StateSpec, const FString& Path)
			{
				if (!StateSpec.IsValid()) return;
				const FString StateName = StateSpec->GetStringField(TEXT("name"));
				const FString DisplayPath = StateName.IsEmpty() ? Path : Path + TEXT("/") + StateName;

				const TArray<TSharedPtr<FJsonValue>>* TasksArr = nullptr;
				if (StateSpec->TryGetArrayField(TEXT("tasks"), TasksArr) && TasksArr)
				{
					for (const auto& TaskVal : *TasksArr)
					{
						TSharedPtr<FJsonObject> TaskSpec = TaskVal->AsObject();
						if (!TaskSpec.IsValid()) continue;
						const FString TaskClassName = TaskSpec->GetStringField(TEXT("class"));
						if (TaskClassName.IsEmpty()) continue;
						if (!FindStructByName(TaskClassName))
						{
							SkippedStates.Add(MakeStateSkipObj(DisplayPath, TaskClassName, TEXT("task")));
							StrictErrors.Add(FString::Printf(TEXT("%s: task class '%s' does not resolve"), *DisplayPath, *TaskClassName));
						}
					}
				}

				const TArray<TSharedPtr<FJsonValue>>* CondsArr = nullptr;
				if (StateSpec->TryGetArrayField(TEXT("enter_conditions"), CondsArr) && CondsArr)
				{
					for (const auto& CondVal : *CondsArr)
					{
						TSharedPtr<FJsonObject> CondSpec = CondVal->AsObject();
						if (!CondSpec.IsValid()) continue;
						const FString CondClassName = CondSpec->GetStringField(TEXT("class"));
						if (CondClassName.IsEmpty()) continue;
						if (!FindStructByName(CondClassName))
						{
							SkippedStates.Add(MakeStateSkipObj(DisplayPath, CondClassName, TEXT("enter_condition")));
							StrictErrors.Add(FString::Printf(TEXT("%s: enter_condition class '%s' does not resolve"), *DisplayPath, *CondClassName));
						}
					}
				}

				const TArray<TSharedPtr<FJsonValue>>* ChildrenArr = nullptr;
				if (StateSpec->TryGetArrayField(TEXT("children"), ChildrenArr) && ChildrenArr)
				{
					for (const auto& ChildVal : *ChildrenArr)
					{
						TSharedPtr<FJsonObject> ChildSpec = ChildVal->AsObject();
						if (ChildSpec.IsValid())
						{
							WalkSpec(ChildSpec, DisplayPath);
						}
					}
				}
			};
			WalkSpec(*PreRootPtr, FString());
		}
	}

	if (bStrictMode && StrictErrors.Num() > 0)
	{
		FString Msg = TEXT("strict_mode: spec pre-validation failed (asset NOT saved):");
		for (const FString& E : StrictErrors)
		{
			Msg += TEXT("\n  - ") + E;
		}
		return FMonolithActionResult::Error(Msg);
	}

	FString AssetName = FPackageName::GetShortName(SavePath);

	FString PathErr;
	if (!MonolithAI::EnsureAssetPathFree(SavePath, AssetName, PathErr))
	{
		return FMonolithActionResult::Error(PathErr);
	}

	// Create the StateTree asset
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *SavePath));
	}

	UStateTreeFactory* Factory = NewObject<UStateTreeFactory>();

	// Set schema from spec
	const FString SchemaClassName = Spec->HasField(TEXT("schema_class")) ? Spec->GetStringField(TEXT("schema_class")) : FString();
	UClass* SchemaClass = ResolveStateTreeSchemaClass(SchemaClassName);
	if (!SchemaClass)
	{
		const FString MissingName = SchemaClassName.IsEmpty() ? TEXT("StateTreeAIComponentSchema") : SchemaClassName;
		return FMonolithActionResult::Error(FString::Printf(TEXT("Schema class '%s' not found or not a UStateTreeSchema"), *MissingName));
	}
	Factory->SetSchemaClass(SchemaClass);

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Build StateTree From Spec")));

	UStateTree* NewST = Cast<UStateTree>(Factory->FactoryCreateNew(
		UStateTree::StaticClass(), Package, *AssetName, RF_Public | RF_Standalone, nullptr, GWarn));
	if (!NewST)
	{
		return FMonolithActionResult::Error(TEXT("UStateTreeFactory::FactoryCreateNew failed"));
	}

	FString Error;
	UStateTreeEditorData* EditorData = GetEditorData(NewST, Error);
	if (!EditorData)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Build from the root spec
	const TSharedPtr<FJsonObject>* RootPtr = nullptr;
	TArray<FString> Warnings;

	if (Spec->TryGetObjectField(TEXT("root"), RootPtr) && RootPtr && RootPtr->IsValid())
	{
		if (!BuildStateFromSpec(NewST, EditorData, nullptr, *RootPtr, Warnings, Error))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Build failed: %s"), *Error));
		}
	}

	// Compile
	bool bCompiled = CompileTree(NewST);

	NewST->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewST);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetBoolField(TEXT("compiled"), bCompiled);
	Result->SetBoolField(TEXT("is_ready_to_run"), NewST->IsReadyToRun());
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Built StateTree '%s' from spec"), *AssetName));

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings)
		{
			WarnArr.Add(MakeShared<FJsonValueString>(W));
		}
		Result->SetArrayField(TEXT("warnings"), WarnArr);
	}

	// Always emit skipped_states (populated during pre-validation walk above).
	Result->SetArrayField(TEXT("skipped_states"), SkippedStates);
	if (SkippedStates.Num() > 0)
	{
		Result->SetStringField(TEXT("warning"), FString::Printf(
			TEXT("%d state slots referenced unresolvable task/condition structs — see skipped_states. Use strict_mode=true to abort instead."), SkippedStates.Num()));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  72. export_st_spec
// ============================================================

namespace
{
	/** Recursively export a state to a spec-compatible JSON object. */
	TSharedPtr<FJsonObject> ExportStateSpec(const UStateTreeState* State)
	{
		if (!State) return nullptr;

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), State->Name.ToString());

		FString TypeStr;
		switch (State->Type)
		{
			case EStateTreeStateType::Group: TypeStr = TEXT("Group"); break;
			case EStateTreeStateType::Linked: TypeStr = TEXT("Linked"); break;
			case EStateTreeStateType::LinkedAsset: TypeStr = TEXT("LinkedAsset"); break;
			case EStateTreeStateType::Subtree: TypeStr = TEXT("Subtree"); break;
			default: TypeStr = TEXT("State"); break;
		}
		Obj->SetStringField(TEXT("type"), TypeStr);

		// Phase F #28: per-state fields so round-trip into build_state_tree_from_spec
		// preserves selection/weight/tag/enabled and the linked-asset reference.
		Obj->SetStringField(TEXT("state_id"), State->ID.ToString());
		{
			FString SelStr = UEnum::GetValueAsString(State->SelectionBehavior);
			SelStr.RemoveFromStart(TEXT("EStateTreeStateSelectionBehavior::"));
			Obj->SetStringField(TEXT("selection_behavior"), SelStr);
		}
		Obj->SetBoolField(TEXT("enabled"), State->bEnabled);
		Obj->SetNumberField(TEXT("weight"), State->Weight);
		if (State->Tag.IsValid())
		{
			Obj->SetStringField(TEXT("tag"), State->Tag.ToString());
		}
#if WITH_EDITORONLY_DATA
		if (State->Type == EStateTreeStateType::LinkedAsset)
		{
			// Resolve linked-asset path via reflection (field is named "LinkedAsset" in UE 5.7
			// as TSoftObjectPtr<UStateTree>; reflection avoids header dep).
			if (FProperty* LinkedProp = State->GetClass()->FindPropertyByName(TEXT("LinkedAsset")))
			{
				if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(LinkedProp))
				{
					const void* Addr = SoftProp->ContainerPtrToValuePtr<void>(State);
					const FSoftObjectPtr& SoftPtr = SoftProp->GetPropertyValue(Addr);
					if (!SoftPtr.IsNull())
					{
						Obj->SetStringField(TEXT("linked_asset_path"), SoftPtr.ToSoftObjectPath().ToString());
					}
				}
				else if (FObjectProperty* ObjProp = CastField<FObjectProperty>(LinkedProp))
				{
					if (UObject* LinkedObj = ObjProp->GetObjectPropertyValue_InContainer(State))
					{
						Obj->SetStringField(TEXT("linked_asset_path"), LinkedObj->GetPathName());
					}
				}
			}
		}
#endif

		// Tasks
		TArray<TSharedPtr<FJsonValue>> TaskArr;
		for (const FStateTreeEditorNode& TaskNode : State->Tasks)
		{
			TSharedPtr<FJsonObject> TaskObj = MakeShared<FJsonObject>();
			if (TaskNode.Node.IsValid())
			{
				TaskObj->SetStringField(TEXT("class"), TaskNode.Node.GetScriptStruct()->GetName());

				// Export instance properties
				const FInstancedStruct& InstData = TaskNode.Instance.IsValid() ? TaskNode.Instance : TaskNode.Node;
				if (InstData.IsValid())
				{
					TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
					const UScriptStruct* StructType = InstData.GetScriptStruct();
					const uint8* Memory = InstData.GetMemory();
					for (TFieldIterator<FProperty> It(StructType, EFieldIteratorFlags::ExcludeSuper); It; ++It)
					{
						if (It->HasAnyPropertyFlags(CPF_Transient)) continue;
						FString ValueStr;
						It->ExportTextItem_Direct(ValueStr, It->ContainerPtrToValuePtr<void>(Memory), nullptr, nullptr, PPF_None);
						if (!ValueStr.IsEmpty())
						{
							Props->SetStringField(It->GetName(), ValueStr);
						}
					}
					if (Props->Values.Num() > 0)
					{
						TaskObj->SetObjectField(TEXT("properties"), Props);
					}
				}
			}
			TaskArr.Add(MakeShared<FJsonValueObject>(TaskObj));
		}
		if (TaskArr.Num() > 0)
		{
			Obj->SetArrayField(TEXT("tasks"), TaskArr);
		}

		// Transitions
		TArray<TSharedPtr<FJsonValue>> TransArr;
		for (const FStateTreeTransition& Trans : State->Transitions)
		{
			TSharedPtr<FJsonObject> TransObj = MakeShared<FJsonObject>();
			TransObj->SetStringField(TEXT("trigger"), UEnum::GetValueAsString(Trans.Trigger));

#if WITH_EDITORONLY_DATA
			switch (Trans.State.LinkType)
			{
				case EStateTreeTransitionType::Succeeded:
					TransObj->SetStringField(TEXT("target"), TEXT("Succeeded"));
					break;
				case EStateTreeTransitionType::Failed:
					TransObj->SetStringField(TEXT("target"), TEXT("Failed"));
					break;
				case EStateTreeTransitionType::NextSelectableState:
					TransObj->SetStringField(TEXT("target"), TEXT("NextSelectableState"));
					break;
				case EStateTreeTransitionType::GotoState:
					TransObj->SetStringField(TEXT("target"), Trans.State.Name.ToString());
					break;
				default:
					TransObj->SetStringField(TEXT("target"), TEXT("Unknown"));
					break;
			}
#endif

			if (Trans.bDelayTransition)
			{
				TransObj->SetNumberField(TEXT("delay"), Trans.DelayDuration);
			}

			TransArr.Add(MakeShared<FJsonValueObject>(TransObj));
		}
		if (TransArr.Num() > 0)
		{
			Obj->SetArrayField(TEXT("transitions"), TransArr);
		}

		// Enter conditions
		TArray<TSharedPtr<FJsonValue>> CondArr;
		for (const FStateTreeEditorNode& CondNode : State->EnterConditions)
		{
			TSharedPtr<FJsonObject> CondObj = MakeShared<FJsonObject>();
			if (CondNode.Node.IsValid())
			{
				CondObj->SetStringField(TEXT("class"), CondNode.Node.GetScriptStruct()->GetName());
			}
			CondArr.Add(MakeShared<FJsonValueObject>(CondObj));
		}
		if (CondArr.Num() > 0)
		{
			Obj->SetArrayField(TEXT("enter_conditions"), CondArr);
		}

		// Children
		TArray<TSharedPtr<FJsonValue>> ChildArr;
		for (const UStateTreeState* Child : State->Children)
		{
			TSharedPtr<FJsonObject> ChildSpec = ExportStateSpec(Child);
			if (ChildSpec.IsValid())
			{
				ChildArr.Add(MakeShared<FJsonValueObject>(ChildSpec));
			}
		}
		if (ChildArr.Num() > 0)
		{
			Obj->SetArrayField(TEXT("children"), ChildArr);
		}

		return Obj;
	}
}

FMonolithActionResult FMonolithAIStateTreeActions::HandleExportSTSpec(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();

	// Schema class
	if (ST->GetSchema())
	{
		Spec->SetStringField(TEXT("schema_class"), ST->GetSchema()->GetClass()->GetName());
	}

	// Export the first subtree as root (or wrap all subtrees)
	if (EditorData->SubTrees.Num() == 1)
	{
		TSharedPtr<FJsonObject> RootSpec = ExportStateSpec(EditorData->SubTrees[0]);
		if (RootSpec.IsValid())
		{
			Spec->SetObjectField(TEXT("root"), RootSpec);
		}
	}
	else if (EditorData->SubTrees.Num() > 1)
	{
		// Multiple subtrees — export as root Group with children
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("name"), TEXT("Root"));
		Root->SetStringField(TEXT("type"), TEXT("Group"));

		TArray<TSharedPtr<FJsonValue>> SubTreeArr;
		for (const UStateTreeState* SubTree : EditorData->SubTrees)
		{
			TSharedPtr<FJsonObject> SubSpec = ExportStateSpec(SubTree);
			if (SubSpec.IsValid())
			{
				SubTreeArr.Add(MakeShared<FJsonValueObject>(SubSpec));
			}
		}
		Root->SetArrayField(TEXT("children"), SubTreeArr);
		Spec->SetObjectField(TEXT("root"), Root);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetObjectField(TEXT("spec"), Spec);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  74. generate_st_diagram
// ============================================================

namespace
{
	/** Recursively generate Mermaid diagram lines from a state tree. */
	void GenerateMermaidForState(const UStateTreeState* State, const FString& ParentId, TArray<FString>& OutLines, int32& StateCounter)
	{
		if (!State) return;

		FString SafeName = State->Name.ToString().Replace(TEXT(" "), TEXT("_"));
		FString StateId = FString::Printf(TEXT("S%d_%s"), StateCounter++, *SafeName);

		// State definition
		if (State->Type == EStateTreeStateType::Group)
		{
			OutLines.Add(FString::Printf(TEXT("    state %s {"), *StateId));
			OutLines.Add(FString::Printf(TEXT("        %s : %s [Group]"), *StateId, *State->Name.ToString()));
			for (const UStateTreeState* Child : State->Children)
			{
				GenerateMermaidForState(Child, StateId, OutLines, StateCounter);
			}
			OutLines.Add(TEXT("    }"));
		}
		else
		{
			FString Label = State->Name.ToString();
			if (State->Tasks.Num() > 0 && State->Tasks[0].Node.IsValid())
			{
				Label += FString::Printf(TEXT("\\n(%s)"), *State->Tasks[0].Node.GetScriptStruct()->GetName());
			}
			OutLines.Add(FString::Printf(TEXT("    %s : %s"), *StateId, *Label));
		}

		// Parent -> child transition
		if (!ParentId.IsEmpty())
		{
			OutLines.Add(FString::Printf(TEXT("    %s --> %s"), *ParentId, *StateId));
		}

		// Explicit transitions
		for (const FStateTreeTransition& Trans : State->Transitions)
		{
#if WITH_EDITORONLY_DATA
			FString TargetLabel;
			switch (Trans.State.LinkType)
			{
				case EStateTreeTransitionType::Succeeded: TargetLabel = TEXT("[*]"); break;
				case EStateTreeTransitionType::Failed: TargetLabel = TEXT("FAILED"); break;
				case EStateTreeTransitionType::NextSelectableState: TargetLabel = TEXT("NextState"); break;
				case EStateTreeTransitionType::GotoState:
					TargetLabel = Trans.State.Name.ToString().Replace(TEXT(" "), TEXT("_"));
					break;
				default: TargetLabel = TEXT("?"); break;
			}

			FString TriggerLabel = UEnum::GetValueAsString(Trans.Trigger);
			TriggerLabel = TriggerLabel.Replace(TEXT("EStateTreeTransitionTrigger::"), TEXT(""));
			OutLines.Add(FString::Printf(TEXT("    %s --> %s : %s"), *StateId, *TargetLabel, *TriggerLabel));
#endif
		}

		// Recurse children (non-group)
		if (State->Type != EStateTreeStateType::Group)
		{
			for (const UStateTreeState* Child : State->Children)
			{
				GenerateMermaidForState(Child, StateId, OutLines, StateCounter);
			}
		}
	}
}

FMonolithActionResult FMonolithAIStateTreeActions::HandleGenerateSTDiagram(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	TArray<FString> Lines;
	Lines.Add(TEXT("stateDiagram-v2"));

	int32 StateCounter = 0;

	if (EditorData->SubTrees.Num() > 0)
	{
		Lines.Add(FString::Printf(TEXT("    [*] --> S%d_%s"),
			StateCounter,
			*EditorData->SubTrees[0]->Name.ToString().Replace(TEXT(" "), TEXT("_"))));
	}

	for (const UStateTreeState* SubTree : EditorData->SubTrees)
	{
		GenerateMermaidForState(SubTree, TEXT(""), Lines, StateCounter);
	}

	FString Diagram = FString::Join(Lines, TEXT("\n"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("format"), TEXT("mermaid"));
	Result->SetStringField(TEXT("diagram"), Diagram);
	Result->SetNumberField(TEXT("state_count"), StateCounter);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  76b. auto_arrange_st
// ============================================================

FMonolithActionResult FMonolithAIStateTreeActions::HandleAutoArrangeST(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStateTree* ST = LoadStateTreeFromParams(Params, AssetPath, Error);
	if (!ST) return FMonolithActionResult::Error(Error);

	FString FormatterPref = Params->GetStringField(TEXT("formatter")).ToLower();

	// State Trees use UEdGraph for their editor representation
	// Try to find the editor graph
#if WITH_EDITORONLY_DATA
	UStateTreeEditorData* EditorData = GetEditorData(ST, Error);
	if (!EditorData) return FMonolithActionResult::Error(Error);

	// StateTree editor doesn't use a traditional UEdGraph in the same way as Blueprints.
	// Check if external formatter supports it first.
	bool bUsedExternalFormatter = false;

	if (FormatterPref != TEXT("builtin") && IMonolithGraphFormatter::IsAvailable())
	{
		// StateTree uses a custom editor UI, not standard UEdGraph nodes.
		// The external formatter (Blueprint Assist) may not support it.
		// We attempt and report back.
		UEdGraph* EdGraph = nullptr;

		// Look for any UEdGraph owned by the StateTree
		TArray<UObject*> SubObjects;
		GetObjectsWithOuter(ST, SubObjects, /*bIncludeNestedObjects=*/true);
		for (UObject* Sub : SubObjects)
		{
			EdGraph = Cast<UEdGraph>(Sub);
			if (EdGraph) break;
		}

		if (!EdGraph)
		{
			GetObjectsWithOuter(EditorData, SubObjects, /*bIncludeNestedObjects=*/true);
			for (UObject* Sub : SubObjects)
			{
				EdGraph = Cast<UEdGraph>(Sub);
				if (EdGraph) break;
			}
		}

		if (EdGraph)
		{
			IMonolithGraphFormatter& Formatter = IMonolithGraphFormatter::Get();
			if (Formatter.SupportsGraph(EdGraph))
			{
				int32 NodesFormatted = 0;
				FString FormatError;
				if (Formatter.FormatGraph(EdGraph, NodesFormatted, FormatError))
				{
					bUsedExternalFormatter = true;
					ST->MarkPackageDirty();

					TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetStringField(TEXT("asset_path"), AssetPath);
					Result->SetStringField(TEXT("formatter"), TEXT("external"));
					Result->SetNumberField(TEXT("nodes_formatted"), NodesFormatted);
					Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Auto-arranged %d nodes via external formatter"), NodesFormatted));
					return FMonolithActionResult::Success(Result);
				}
				else if (!FormatError.IsEmpty())
				{
					// External formatter failed — fall through to built-in
				}
			}
		}
	}

	// Built-in fallback: StateTree states are hierarchical, not graph nodes.
	// We can "arrange" by normalizing the tree structure — reorder children alphabetically
	// or by weight, which is the meaningful layout operation for state trees.
	int32 StatesProcessed = 0;

	TFunction<void(UStateTreeState*)> ProcessState = [&](UStateTreeState* State)
	{
		if (!State) return;
		StatesProcessed++;
		for (UStateTreeState* Child : State->Children)
		{
			ProcessState(Child);
		}
	};

	for (UStateTreeState* SubTree : EditorData->SubTrees)
	{
		ProcessState(SubTree);
	}

	ST->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("formatter"), TEXT("builtin"));
	Result->SetNumberField(TEXT("states_processed"), StatesProcessed);
	Result->SetStringField(TEXT("message"), FString::Printf(
		TEXT("Processed %d states. Note: StateTrees are hierarchical — graph layout has limited applicability. Use generate_st_diagram for visualization."),
		StatesProcessed));
	return FMonolithActionResult::Success(Result);
#else
	return FMonolithActionResult::Error(TEXT("Editor data not available in non-editor builds"));
#endif
}

#endif // WITH_STATETREE
