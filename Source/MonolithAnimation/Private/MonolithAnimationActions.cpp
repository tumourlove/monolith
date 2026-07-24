#include "MonolithAnimationActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithPropertyAccessReader.h"
#include "MonolithAnimNodeBindingReader.h" // Gap 2 (function bindings) + Gap 12 (pin bindings) read helpers

#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationAsset.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/Skeleton.h"
#include "Animation/PreviewAssetAttachComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateEntryNode.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "AnimationBlueprintLibrary.h"
#include "AnimPose.h" // derive_foot_sync_markers signal 5: component-space pose eval (UAnimPoseExtensions, FAnimPose, EAnimPoseSpaces)
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimComposite.h"
#include "Animation/AimOffsetBlendSpace.h"
#include "Animation/AimOffsetBlendSpace1D.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/AnimInstance.h"
#include "AnimationModifier.h"
#include "AnimationModifiersAssetUserData.h" // apply_anim_modifier persist path (T1-L3 ALT) — stack-register via AddAnimationModifierOfClass
#include "Rig/IKRigDefinition.h"
#include "Rig/IKRigSkeleton.h"
#include "Rig/Solvers/IKRigSolverBase.h" // FIKRigSolverBase::StaticStruct() for add_ik_solver struct enumeration
#include "JsonObjectConverter.h" // T2-1 get_ikrig_info: reflective solver/bone/goal settings struct -> JSON
#include "RigEditor/IKRigController.h"
#include "UObject/UObjectIterator.h"     // TObjectIterator<UStruct> — enumerate live IKRig solver-struct table
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetOps.h" // FIKRetargetOpBase / FIKRetargetOpSettingsBase — get_retargeter_info ops[] reflective read
#include "Retargeter/IKRetargetChainMapping.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "RetargetEditor/IKRetargetBatchOperation.h" // batch_retarget_animations — RunRetarget + FIKRetargetBatchOperationContext
#include "EditorAnimUtils.h"                          // EditorAnimUtils::FNameDuplicationRule (output folder + rename rule)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "ControlRigBlueprintLegacy.h"
#else
// UE 5.6 hasn't split the legacy Control Rig Blueprint out of ControlRigBlueprint.h yet.
#include "ControlRigBlueprint.h"
#endif
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyElements.h"
#include "Rigs/RigHierarchyDefines.h"
#include "Rigs/RigHierarchyController.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h" // FObjectProperty / FindFProperty (reflective Chooser read)
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Editor.h"
#include "AnimationStateMachineSchema.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateGraph.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_TransitionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "K2Node_VariableGet.h"
#include "K2Node_CallFunction.h"          // compare/Abs nodes for float transition rules (Phase 6)
#include "Kismet/KismetMathLibrary.h"     // *_DoubleDouble comparisons + Abs(double)
#include "Kismet2/CompilerResultsLog.h"   // compile + error harvest on rule authoring
#include "Logging/TokenizedMessage.h"     // EMessageSeverity for harvested compiler messages
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/BodyInstance.h"

#if WITH_CHOOSER
// Phase-2 read-only recursive chooser-tree collector (same module, MonolithAnimation).
// Used to expand a referenced chooser's nested tree when recursive:true is requested.
#include "MonolithChooserTreeCollector.h"
// Chooser.h gives us the complete UChooserTable type for the Cast<> in the recursive branch.
// (Chooser.Build.cs exposes this header; the module dep is added under bHasChooser.)
#include "Chooser.h"
#endif

// ---------------------------------------------------------------------------
// AnimGraph chooser-node resolution (file-local helpers)
// ---------------------------------------------------------------------------
//
// The EvaluateChooser graph node exists in TWO forms, BOTH declared in a PRIVATE engine
// header (ChooserUncooked/Private/EvaluateChooserNode.h) that we deliberately do NOT and
// CANNOT #include:
//   - v1  UK2Node_EvaluateChooser  : UCLASS(MinimalAPI, Hidden), DEPRECATED ("old
//                                     implementation, not accessible to create new
//                                     instances"). A modern AnimBP will not contain a
//                                     freshly-created v1 node, but a legacy one may exist.
//   - v2  UK2Node_EvaluateChooser2 : the modern node a real AnimBP contains today.
//
// Both are plain UK2Node subclasses (NOT UAnimGraphNode_*), and on BOTH the `Chooser`
// UPROPERTY (TObjectPtr<UChooserTable>) is C++ `private`. We therefore:
//   1. Match the node by class-name PREFIX "K2Node_EvaluateChooser" — this prefix is
//      INTENTIONAL: it catches BOTH v1 and v2 in one test, so we never miss a legacy node
//      while still resolving modern v2 nodes.
//   2. Read the `Chooser` reference REFLECTIVELY via FindFProperty<FObjectProperty> +
//      GetObjectPropertyValue_InContainer. Reflection bypasses C++ access control, which is
//      exactly why this path is mandatory: a hard cast / Private-header #include would be
//      impossible to compile AND would still hit an inaccessible C++ member.
//
// DO NOT "fix" this into a hard cast or a #include of EvaluateChooserNode.h — it is a
// Private module header and the property is private. The reflective read is correct and
// deliberate.
namespace MonolithAnimGraphChooser
{
	/** Class-name prefix that matches BOTH v1 (UK2Node_EvaluateChooser) and v2
	 *  (UK2Node_EvaluateChooser2). GetClass()->GetName() drops the leading 'U'. */
	static const TCHAR* const EvaluateChooserClassPrefix = TEXT("K2Node_EvaluateChooser");

	/** True if Node is an EvaluateChooser graph node (v1 or v2), matched by class-name prefix. */
	static bool IsEvaluateChooserNode(const UEdGraphNode* Node)
	{
		return Node && Node->GetClass()->GetName().StartsWith(EvaluateChooserClassPrefix);
	}

	/**
	 * Reflectively resolve the chooser asset referenced by an EvaluateChooser node.
	 *
	 * Reads the PRIVATE `Chooser` UPROPERTY by reflection (see file-header comment for why a
	 * hard cast/include is impossible AND undesirable). Writes resolution status into OutObj:
	 *   resolved        : bool — true only when the Chooser property was found AND held an asset
	 *   chooser_asset   : string — asset path name, or empty
	 *   resolve_detail  : string — present only on failure, why resolution failed
	 *
	 * @return the referenced UObject* (the UChooserTable) or nullptr on any failure.
	 */
	static UObject* ResolveChooserAsset(const UEdGraphNode* Node, const TSharedPtr<FJsonObject>& OutObj)
	{
		OutObj->SetBoolField(TEXT("resolved"), false);
		OutObj->SetStringField(TEXT("chooser_asset"), FString());

		if (!Node)
		{
			OutObj->SetStringField(TEXT("resolve_detail"), TEXT("null node"));
			return nullptr;
		}

		// Read the private `Chooser` UPROPERTY reflectively — see file-header comment.
		FObjectProperty* ChooserProp = FindFProperty<FObjectProperty>(Node->GetClass(), TEXT("Chooser"));
		if (!ChooserProp)
		{
			OutObj->SetStringField(TEXT("resolve_detail"),
				TEXT("no 'Chooser' FObjectProperty on node class (engine API may have changed)"));
			return nullptr;
		}

		UObject* ChooserObj = ChooserProp->GetObjectPropertyValue_InContainer(Node);
		if (!ChooserObj)
		{
			OutObj->SetStringField(TEXT("resolve_detail"), TEXT("'Chooser' property is unset (no asset assigned)"));
			return nullptr;
		}

		OutObj->SetBoolField(TEXT("resolved"), true);
		OutObj->SetStringField(TEXT("chooser_asset"), ChooserObj->GetPathName());
		return ChooserObj;
	}
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithAnimationActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// Montage Sections
	Registry.RegisterAction(TEXT("animation"), TEXT("add_montage_section"),
		TEXT("Add a section to an animation montage"),
		FMonolithActionHandler::CreateStatic(&HandleAddMontageSection),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Montage asset path"))
			.Required(TEXT("section_name"), TEXT("string"), TEXT("Name for the new section"))
			.Required(TEXT("start_time"), TEXT("number"), TEXT("Start time in seconds"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("delete_montage_section"),
		TEXT("Delete a section from an animation montage by index"),
		FMonolithActionHandler::CreateStatic(&HandleDeleteMontageSection),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Montage asset path"))
			.Required(TEXT("section_index"), TEXT("integer"), TEXT("Index of the section to delete"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_section_next"),
		TEXT("Set the next section for a montage section"),
		FMonolithActionHandler::CreateStatic(&HandleSetSectionNext),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Montage asset path"))
			.Required(TEXT("section_name"), TEXT("string"), TEXT("Name of the section"))
			.Required(TEXT("next_section_name"), TEXT("string"), TEXT("Name of the next section to play"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_section_time"),
		TEXT("Set the start time of a montage section"),
		FMonolithActionHandler::CreateStatic(&HandleSetSectionTime),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Montage asset path"))
			.Required(TEXT("section_name"), TEXT("string"), TEXT("Name of the section"))
			.Required(TEXT("new_time"), TEXT("number"), TEXT("New start time in seconds"))
			.Build());

	// BlendSpace Samples
	Registry.RegisterAction(TEXT("animation"), TEXT("add_blendspace_sample"),
		TEXT("Add a sample to a blend space"),
		FMonolithActionHandler::CreateStatic(&HandleAddBlendSpaceSample),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("BlendSpace asset path"))
			.RequiredAssetPath(TEXT("anim_path"), TEXT("Animation sequence asset path"))
			.Required(TEXT("x"), TEXT("number"), TEXT("X axis value"))
			.Required(TEXT("y"), TEXT("number"), TEXT("Y axis value"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("edit_blendspace_sample"),
		TEXT("Edit a blend space sample position and optionally its animation"),
		FMonolithActionHandler::CreateStatic(&HandleEditBlendSpaceSample),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("BlendSpace asset path"))
			.Required(TEXT("sample_index"), TEXT("integer"), TEXT("Index of the sample to edit"))
			.Required(TEXT("x"), TEXT("number"), TEXT("New X axis value"))
			.Required(TEXT("y"), TEXT("number"), TEXT("New Y axis value"))
			.OptionalAssetPath(TEXT("anim_path"), TEXT("New animation sequence asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("delete_blendspace_sample"),
		TEXT("Delete a sample from a blend space by index"),
		FMonolithActionHandler::CreateStatic(&HandleDeleteBlendSpaceSample),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("BlendSpace asset path"))
			.Required(TEXT("sample_index"), TEXT("integer"), TEXT("Index of the sample to delete"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("bake_blend_space"),
		TEXT("Rebuild a blend space's triangulation/grid (FBlendSpaceData) by running ResampleData(). "
			 "Required after programmatic sample/axis edits — without it the runtime reads an empty "
			 "triangulation and the blend space evaluates to bind/A-pose while the editor preview looks fine. "
			 "Works on BlendSpace and BlendSpace1D. Marks the package dirty; saving is the caller's concern."),
		FMonolithActionHandler::CreateStatic(&HandleBakeBlendSpace),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("BlendSpace or BlendSpace1D asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_blend_space_interpolation"),
		TEXT("Set a blend space's input-interpolation settings: 'use_grid' toggles bInterpolateUsingGrid "
			 "(true = runtime uses the grid, false = runtime uses the triangulation), and "
			 "'preferred_triangulation_direction' chooses the edge direction for ambiguous triangulation. "
			 "Rebuilds the data (ResampleData) so the chosen structure is populated; marks the package dirty."),
		FMonolithActionHandler::CreateStatic(&HandleSetBlendSpaceInterpolation),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("BlendSpace asset path"))
			.Optional(TEXT("use_grid"), TEXT("bool"), TEXT("Set bInterpolateUsingGrid: true = grid interpolation, false = triangulation"))
			.Optional(TEXT("preferred_triangulation_direction"), TEXT("string"), TEXT("None, Tangential, or Radial"))
			.Build());

	// ABP Graph Reading
	Registry.RegisterAction(TEXT("animation"), TEXT("get_state_machines"),
		TEXT("Get all state machines in an animation blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleGetStateMachines),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_state_info"),
		TEXT("Get detailed info about a state in a state machine"),
		FMonolithActionHandler::CreateStatic(&HandleGetStateInfo),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("machine_name"), TEXT("string"), TEXT("State machine name"))
			.Required(TEXT("state_name"), TEXT("string"), TEXT("State name"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_transitions"),
		TEXT("Get all transitions in a state machine"),
		FMonolithActionHandler::CreateStatic(&HandleGetTransitions),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Optional(TEXT("machine_name"), TEXT("string"), TEXT("Filter to a specific state machine"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_blend_nodes"),
		TEXT("Get blend nodes in an animation blueprint graph"),
		FMonolithActionHandler::CreateStatic(&HandleGetBlendNodes),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Filter to a specific graph"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_linked_layers"),
		TEXT("Get linked animation layers in an animation blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleGetLinkedLayers),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_graphs"),
		TEXT("Get all graphs in an animation blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleGetGraphs),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_nodes"),
		TEXT("Get animation nodes with optional class filter"),
		FMonolithActionHandler::CreateStatic(&HandleGetNodes),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Optional(TEXT("node_class_filter"), TEXT("string"), TEXT("Only include nodes whose class contains this substring"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Filter to a specific graph"))
			.Optional(TEXT("include_anim_graph"), TEXT("bool"), TEXT("Also traverse the main AnimGraph (and all graphs) and surface EvaluateChooser nodes with their referenced chooser asset"), TEXT("false"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_anim_graph_choosers"),
		TEXT("Enumerate the AnimBlueprint's EvaluateChooser graph nodes and report each node's resolved chooser asset path"),
		FMonolithActionHandler::CreateStatic(&HandleGetAnimGraphChoosers),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Optional(TEXT("recursive"), TEXT("bool"), TEXT("Expand each referenced chooser's full nested tree (root->child) in the output"), TEXT("false"))
			.Build());

	// --- Anim-node bindings: function (Gap 2) + pin property (Gap 12) ---
	Registry.RegisterAction(TEXT("animation"), TEXT("get_anim_node_function_bindings"),
		TEXT("Read the per-node function bindings (On Initial Update / On Become Relevant / On Update) on an animation graph node. ")
		TEXT("Each binding reports function_name, member_parent_class, is_self_context and thread_safe. ")
		TEXT("Omit node_id to list every node that has any non-empty function binding. ")
		TEXT("Example: { asset_path: '/Game/Anim/ABP_Char', node_id: 'AnimGraphNode_SequencePlayer_0' }."),
		FMonolithActionHandler::CreateStatic(&HandleGetAnimNodeFunctionBindings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Optional(TEXT("node_id"), TEXT("string"), TEXT("Node name or NodeGuid. Omit to return all nodes that have any function binding."))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Filter to a specific graph"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("set_anim_node_function_binding"),
		TEXT("Bind (or clear) a function on an animation graph node's On Initial Update / On Become Relevant / On Update slot. ")
		TEXT("Validates like the engine: the function must match the thread-safe anim-update prototype signature and be thread-safe ")
		TEXT("(hard reject unless allow_non_thread_safe=true). Pass an empty function_name to clear. function_class targets an external ")
		TEXT("library class; omit it to bind a function authored on the Animation Blueprint itself. ")
		TEXT("Example: { asset_path: '/Game/Anim/ABP_Char', node_id: 'AnimGraphNode_BlendSpacePlayer_0', binding: 'update', function_name: 'UpdateSpeed' }."),
		FMonolithActionHandler::CreateStatic(&HandleSetAnimNodeFunctionBinding),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Node name or NodeGuid"))
			.Required(TEXT("binding"), TEXT("string"), TEXT("Which slot: initial_update | become_relevant | update"))
			.Optional(TEXT("function_name"), TEXT("string"), TEXT("Function to bind. Empty/omitted clears the binding."))
			.Optional(TEXT("function_class"), TEXT("string"), TEXT("External library class path for the function. Omit for a self-member on the Animation Blueprint."))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Filter to a specific graph"))
			.Optional(TEXT("recompile"), TEXT("bool"), TEXT("Recompile the Animation Blueprint after the change"), TEXT("true"))
			.Optional(TEXT("allow_non_thread_safe"), TEXT("bool"), TEXT("Override the thread-safe hard reject (binding a non-thread-safe function can corrupt worker-thread anim evaluation)"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("get_anim_node_pin_bindings"),
		TEXT("Read the property-access PIN bindings on an animation graph node (distinct from function bindings). ")
		TEXT("Each entry reports pin, path (the property-access chain), type (Property/Function) and is_bound. ")
		TEXT("Omit node_id to list every node that has any pin binding. ")
		TEXT("Example: { asset_path: '/Game/Anim/ABP_Char', node_id: 'AnimGraphNode_ModifyBone_0' }."),
		FMonolithActionHandler::CreateStatic(&HandleGetAnimNodePinBindings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Optional(TEXT("node_id"), TEXT("string"), TEXT("Node name or NodeGuid. Omit to return all nodes that have any pin binding."))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Filter to a specific graph"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("set_anim_node_pin_binding"),
		TEXT("Bind (or clear) a PIN on an animation graph node to a property-access path. ")
		TEXT("Pass path as a string array (e.g. ['CharacterState','Speed']); an empty/omitted path clears the binding. ")
		TEXT("After the write the node is reconstructed so the binding's pin type is re-derived, then the Animation Blueprint is recompiled. ")
		TEXT("Works even when the node has no existing binding: the binding object is created on demand if absent. ")
		TEXT("Example: { asset_path: '/Game/Anim/ABP_Char', node_id: 'AnimGraphNode_ModifyBone_0', pin: 'Alpha', path: ['CharacterState','Alpha'] }."),
		FMonolithActionHandler::CreateStatic(&HandleSetAnimNodePinBinding),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Node name or NodeGuid"))
			.Required(TEXT("pin"), TEXT("string"), TEXT("Pin (property) name to bind"))
			.Optional(TEXT("path"), TEXT("array"), TEXT("Property-access chain as a string array. Empty/omitted clears the binding."))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Filter to a specific graph"))
			.Optional(TEXT("recompile"), TEXT("bool"), TEXT("Recompile the Animation Blueprint after the change"), TEXT("true"))
			.Build());

	// Notify Editing
	Registry.RegisterAction(TEXT("animation"), TEXT("set_notify_time"),
		TEXT("Set the trigger time of an animation notify"),
		FMonolithActionHandler::CreateStatic(&HandleSetNotifyTime),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation asset path"))
			.Required(TEXT("notify_index"), TEXT("integer"), TEXT("Index of the notify"))
			.Required(TEXT("new_time"), TEXT("number"), TEXT("New trigger time in seconds"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_notify_duration"),
		TEXT("Set the duration of a state animation notify"),
		FMonolithActionHandler::CreateStatic(&HandleSetNotifyDuration),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation asset path"))
			.Required(TEXT("notify_index"), TEXT("integer"), TEXT("Index of the notify"))
			.Required(TEXT("new_duration"), TEXT("number"), TEXT("New duration in seconds"))
			.Build());

	// Bone Tracks
	Registry.RegisterAction(TEXT("animation"), TEXT("set_bone_track_keys"),
		TEXT("Set position, rotation, and scale keys on a bone track"),
		FMonolithActionHandler::CreateStatic(&HandleSetBoneTrackKeys),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation sequence asset path"))
			.Required(TEXT("bone_name"), TEXT("string"), TEXT("Bone name"))
			.Required(TEXT("positions_json"), TEXT("string"), TEXT("JSON array of position keys"))
			.Required(TEXT("rotations_json"), TEXT("string"), TEXT("JSON array of rotation keys"))
			.Required(TEXT("scales_json"), TEXT("string"), TEXT("JSON array of scale keys"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_bone_track"),
		TEXT("Add a bone track to an animation sequence"),
		FMonolithActionHandler::CreateStatic(&HandleAddBoneTrack),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation sequence asset path"))
			.Required(TEXT("bone_name"), TEXT("string"), TEXT("Bone name to add"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_bone_track"),
		TEXT("Remove a bone track from an animation sequence"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveBoneTrack),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation sequence asset path"))
			.Required(TEXT("bone_name"), TEXT("string"), TEXT("Bone name to remove"))
			.Optional(TEXT("include_children"), TEXT("bool"), TEXT("Also remove child bone tracks"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("copy_bone_pose_between_sequences"),
		TEXT("Copy evaluated bone transforms (track + ref pose fallback) from a source AnimSequence at a given time to a destination AnimSequence as keys"),
		FMonolithActionHandler::CreateStatic(&HandleCopyBonePoseBetweenSequences),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("source_path"), TEXT("Source AnimSequence asset path"))
			.RequiredAssetPath(TEXT("dest_path"), TEXT("Destination AnimSequence asset path"))
			.Required(TEXT("bone_names"), TEXT("array"), TEXT("Array of bone names to copy"))
			.Optional(TEXT("source_time"), TEXT("number"), TEXT("Time in seconds on source to evaluate (default 0.0)"), TEXT("0.0"))
			.Optional(TEXT("apply_to_all_dest_frames"), TEXT("bool"), TEXT("If true, write same value to every destination frame (static pose). If false, write only frame 0."), TEXT("true"))
			.Build());

	// Virtual Bones
	Registry.RegisterAction(TEXT("animation"), TEXT("add_virtual_bone"),
		TEXT("Add a virtual bone to a skeleton"),
		FMonolithActionHandler::CreateStatic(&HandleAddVirtualBone),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Skeleton asset path"))
			.Required(TEXT("source_bone"), TEXT("string"), TEXT("Source bone name"))
			.Required(TEXT("target_bone"), TEXT("string"), TEXT("Target bone name"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_virtual_bones"),
		TEXT("Remove virtual bones from a skeleton"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveVirtualBones),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Skeleton asset path"))
			.Required(TEXT("bone_names"), TEXT("array"), TEXT("Array of virtual bone names to remove"))
			.Build());

	// Skeleton Info
	Registry.RegisterAction(TEXT("animation"), TEXT("get_skeleton_info"),
		TEXT("Get skeleton bone hierarchy and virtual bones"),
		FMonolithActionHandler::CreateStatic(&HandleGetSkeletonInfo),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Skeleton asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_skeletal_mesh_info"),
		TEXT("Get skeletal mesh info including morph targets, sockets, LODs, and materials"),
		FMonolithActionHandler::CreateStatic(&HandleGetSkeletalMeshInfo),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Skeletal mesh asset path"))
			.Build());

	// Wave 1 — Read Actions
	Registry.RegisterAction(TEXT("animation"), TEXT("get_sequence_info"),
		TEXT("Get animation sequence metadata (duration, frames, root motion, compression, etc.)"),
		FMonolithActionHandler::CreateStatic(&HandleGetSequenceInfo),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_sequence_notifies"),
		TEXT("Get all notifies on an animation asset"),
		FMonolithActionHandler::CreateStatic(&HandleGetSequenceNotifies),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation asset path (sequence, montage, composite)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_bone_track_keys"),
		TEXT("Get position/rotation/scale keys for a bone track"),
		FMonolithActionHandler::CreateStatic(&HandleGetBoneTrackKeys),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Required(TEXT("bone_name"), TEXT("string"), TEXT("Bone name to read"))
			.Optional(TEXT("start_frame"), TEXT("integer"), TEXT("Start frame (default 0)"), TEXT("0"))
			.Optional(TEXT("end_frame"), TEXT("integer"), TEXT("End frame (default -1 = all)"), TEXT("-1"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("list_bone_tracks"),
		TEXT("List all bone names that have tracks (animated bones) in an AnimSequence"),
		FMonolithActionHandler::CreateStatic(&HandleListBoneTracks),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_sequence_curves"),
		TEXT("Get float and transform curves on an animation sequence"),
		FMonolithActionHandler::CreateStatic(&HandleGetSequenceCurves),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_montage_info"),
		TEXT("Get montage metadata including sections, slots, blend settings"),
		FMonolithActionHandler::CreateStatic(&HandleGetMontageInfo),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Montage asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_blend_space_info"),
		TEXT("Get blend space samples and axis settings"),
		FMonolithActionHandler::CreateStatic(&HandleGetBlendSpaceInfo),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("BlendSpace asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_skeleton_sockets"),
		TEXT("Get sockets from a skeleton or skeletal mesh"),
		FMonolithActionHandler::CreateStatic(&HandleGetSkeletonSockets),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Skeleton or SkeletalMesh asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_skeleton_preview_attached_assets"),
		TEXT("Get assets attached to a skeleton's preview scene (Persona [Preview Only] entries: socket + asset path per pair)"),
		FMonolithActionHandler::CreateStatic(&HandleGetSkeletonPreviewAttachedAssets),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Skeleton asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_bone_ref_pose"),
		TEXT("Get reference (bind) pose transforms of bones on a Skeleton or SkeletalMesh — returns parent-relative AND component-space transforms without spawning an actor"),
		FMonolithActionHandler::CreateStatic(&HandleGetBoneRefPose),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Skeleton or SkeletalMesh asset path"))
			.Optional(TEXT("bone_names"), TEXT("array"), TEXT("Specific bone names to query (default: all bones)"), TEXT("[]"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_animated_bone_transform"),
		TEXT("Evaluate one bone's FK-composed transform on an AnimSequence at a specific frame or time (extends get_bone_ref_pose's bind-pose compose to an animated frame). "
			 "Provide exactly one of frame (integer) / time (seconds); frame wins if both given. space: component (default) or world both return FAnimPose World space (root-relative, root motion folded in); local returns parent-relative."),
		FMonolithActionHandler::CreateStatic(&HandleGetAnimatedBoneTransform),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Required(TEXT("bone_name"), TEXT("string"), TEXT("Bone to evaluate"))
			.Optional(TEXT("frame"), TEXT("integer"), TEXT("Frame index to evaluate at (mutually exclusive with time; takes precedence if both supplied)"))
			.Optional(TEXT("time"), TEXT("number"), TEXT("Time in seconds to evaluate at (used when frame is absent)"))
			.Optional(TEXT("space"), TEXT("string"), TEXT("component (default) / world (both = FAnimPose World/component space) or local (parent-relative)"), TEXT("component"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_abp_info"),
		TEXT("Get animation blueprint overview (skeleton, graphs, state machines, variables, interfaces)"),
		FMonolithActionHandler::CreateStatic(&HandleGetAbpInfo),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Build());

	// Wave 2 — Notify CRUD
	Registry.RegisterAction(TEXT("animation"), TEXT("add_notify"),
		TEXT("Add a point notify to an animation asset"),
		FMonolithActionHandler::CreateStatic(&HandleAddNotify),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation asset path"))
			.Required(TEXT("notify_class"), TEXT("string"), TEXT("Notify class name (e.g. AnimNotify_PlaySound)"))
			.Required(TEXT("time"), TEXT("number"), TEXT("Trigger time in seconds"))
			.Optional(TEXT("track_name"), TEXT("string"), TEXT("Notify track name"), TEXT("1"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_notify_state"),
		TEXT("Add a state notify (with duration) to an animation asset"),
		FMonolithActionHandler::CreateStatic(&HandleAddNotifyState),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation asset path"))
			.Required(TEXT("notify_class"), TEXT("string"), TEXT("NotifyState class name (e.g. AnimNotifyState_Trail)"))
			.Required(TEXT("time"), TEXT("number"), TEXT("Start time in seconds"))
			.Required(TEXT("duration"), TEXT("number"), TEXT("Duration in seconds"))
			.Optional(TEXT("track_name"), TEXT("string"), TEXT("Notify track name"), TEXT("1"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_notify"),
		TEXT("Remove a notify by index from an animation asset"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveNotify),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation asset path"))
			.Required(TEXT("notify_index"), TEXT("integer"), TEXT("Index of notify to remove"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_notify_track"),
		TEXT("Move a notify to a different track"),
		FMonolithActionHandler::CreateStatic(&HandleSetNotifyTrack),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation asset path"))
			.Required(TEXT("notify_index"), TEXT("integer"), TEXT("Index of notify to move"))
			.Required(TEXT("track_index"), TEXT("integer"), TEXT("Target track index"))
			.Build());

	// Wave 3 — Curve CRUD
	Registry.RegisterAction(TEXT("animation"), TEXT("list_curves"),
		TEXT("List all animation curves on a sequence (float and transform)"),
		FMonolithActionHandler::CreateStatic(&HandleListCurves),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Optional(TEXT("include_keys"), TEXT("bool"), TEXT("Include key data in response"), TEXT("false"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_curve"),
		TEXT("Add a float or transform curve to an animation sequence"),
		FMonolithActionHandler::CreateStatic(&HandleAddCurve),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Required(TEXT("curve_name"), TEXT("string"), TEXT("Name for the new curve"))
			.Optional(TEXT("curve_type"), TEXT("string"), TEXT("Float or Transform (default Float)"), TEXT("Float"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_curve"),
		TEXT("Remove a curve from an animation sequence"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveCurve),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Required(TEXT("curve_name"), TEXT("string"), TEXT("Name of curve to remove"))
			.Optional(TEXT("curve_type"), TEXT("string"), TEXT("Float or Transform (default Float)"), TEXT("Float"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_curve_keys"),
		TEXT("Set keys on a float curve (replaces existing keys)"),
		FMonolithActionHandler::CreateStatic(&HandleSetCurveKeys),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Required(TEXT("curve_name"), TEXT("string"), TEXT("Curve name"))
			.Required(TEXT("keys_json"), TEXT("string"), TEXT("JSON array: [{\"time\":0.0,\"value\":1.0,\"interp\":\"cubic\"}, ...]"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_curve_keys"),
		TEXT("Get all keys from a float curve"),
		FMonolithActionHandler::CreateStatic(&HandleGetCurveKeys),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Required(TEXT("curve_name"), TEXT("string"), TEXT("Curve name"))
			.Build());

	// Wave 4 — Skeleton + BlendSpace
	Registry.RegisterAction(TEXT("animation"), TEXT("add_socket"),
		TEXT("Add a socket to a skeleton"),
		FMonolithActionHandler::CreateStatic(&HandleAddSocket),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Skeleton asset path"))
			.Required(TEXT("bone_name"), TEXT("string"), TEXT("Parent bone name"))
			.Required(TEXT("socket_name"), TEXT("string"), TEXT("Name for the new socket"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("[x, y, z] relative location"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("[pitch, yaw, roll] relative rotation"))
			.Optional(TEXT("scale"), TEXT("array"), TEXT("[x, y, z] relative scale"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_socket"),
		TEXT("Remove a socket from a skeleton"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveSocket),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Skeleton asset path"))
			.Required(TEXT("socket_name"), TEXT("string"), TEXT("Socket name to remove"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_socket_transform"),
		TEXT("Set the transform of a skeleton socket"),
		FMonolithActionHandler::CreateStatic(&HandleSetSocketTransform),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Skeleton asset path"))
			.Required(TEXT("socket_name"), TEXT("string"), TEXT("Socket name"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("[x, y, z] relative location"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("[pitch, yaw, roll] relative rotation"))
			.Optional(TEXT("scale"), TEXT("array"), TEXT("[x, y, z] relative scale"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_skeleton_curves"),
		TEXT("Get all registered animation curve names from a skeleton"),
		FMonolithActionHandler::CreateStatic(&HandleGetSkeletonCurves),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Skeleton asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_blend_space_axis"),
		TEXT("Configure a blend space axis (name, range, grid divisions)"),
		FMonolithActionHandler::CreateStatic(&HandleSetBlendSpaceAxis),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("BlendSpace asset path"))
			.Required(TEXT("axis"), TEXT("string"), TEXT("X or Y"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Display name for the axis"))
			.Optional(TEXT("min"), TEXT("number"), TEXT("Minimum axis value"))
			.Optional(TEXT("max"), TEXT("number"), TEXT("Maximum axis value"))
			.Optional(TEXT("grid_divisions"), TEXT("integer"), TEXT("Number of grid divisions"))
			.Optional(TEXT("snap_to_grid"), TEXT("bool"), TEXT("Snap samples to grid"))
			.Optional(TEXT("wrap_input"), TEXT("bool"), TEXT("Wrap input outside range"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_root_motion_settings"),
		TEXT("Configure root motion settings on an animation sequence"),
		FMonolithActionHandler::CreateStatic(&HandleSetRootMotionSettings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Optional(TEXT("enable_root_motion"), TEXT("bool"), TEXT("Enable/disable root motion"))
			.Optional(TEXT("root_motion_lock"), TEXT("string"), TEXT("AnimFirstFrame, Zero, or RefPose"))
			.Optional(TEXT("force_root_lock"), TEXT("bool"), TEXT("Force root lock even without root motion"))
			.Build());

	// Wave 5 — Creation + Montage
	Registry.RegisterAction(TEXT("animation"), TEXT("create_sequence"),
		TEXT("Create a new empty animation sequence"),
		FMonolithActionHandler::CreateStatic(&HandleCreateSequence),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new sequence (e.g. /Game/Anims/MyAnim)"))
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("duplicate_sequence"),
		TEXT("Duplicate an animation sequence to a new path"),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateSequence),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("source_path"), TEXT("Source animation asset path"))
			.RequiredAssetPath(TEXT("dest_path"), TEXT("Destination asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("create_montage"),
		TEXT("Create a new animation montage with skeleton"),
		FMonolithActionHandler::CreateStatic(&HandleCreateMontage),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new montage"))
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_montage_blend"),
		TEXT("Set blend in/out times and auto blend out on a montage"),
		FMonolithActionHandler::CreateStatic(&HandleSetMontageBlend),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Montage asset path"))
			.Optional(TEXT("blend_in_time"), TEXT("number"), TEXT("Blend in duration in seconds"))
			.Optional(TEXT("blend_out_time"), TEXT("number"), TEXT("Blend out duration in seconds"))
			.Optional(TEXT("blend_out_trigger_time"), TEXT("number"), TEXT("Time before end to trigger blend out"))
			.Optional(TEXT("enable_auto_blend_out"), TEXT("bool"), TEXT("Enable automatic blend out"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_montage_slot"),
		TEXT("Add a slot track to a montage"),
		FMonolithActionHandler::CreateStatic(&HandleAddMontageSlot),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Montage asset path"))
			.Required(TEXT("slot_name"), TEXT("string"), TEXT("Slot name (e.g. DefaultSlot)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_montage_slot"),
		TEXT("Rename a slot track on a montage by index"),
		FMonolithActionHandler::CreateStatic(&HandleSetMontageSlot),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Montage asset path"))
			.Required(TEXT("slot_index"), TEXT("integer"), TEXT("Index of the slot track"))
			.Required(TEXT("slot_name"), TEXT("string"), TEXT("New slot name"))
			.Build());

	// Wave 7 — Anim Modifiers + Composites
	Registry.RegisterAction(TEXT("animation"), TEXT("apply_anim_modifier"),
		TEXT("Apply an animation modifier class to a sequence. Optionally set modifier properties reflectively and persist the modifier into the asset's AnimationModifiers stack so it survives save/reload."),
		FMonolithActionHandler::CreateStatic(&HandleApplyAnimModifier),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Required(TEXT("modifier_class"), TEXT("string"), TEXT("Modifier class name (e.g. UAnimationModifier_CreateCurve)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("Map of modifier property name -> value to set reflectively before apply (enum values accept the enumerator name)"))
			.Optional(TEXT("persist"), TEXT("bool"), TEXT("If true, register the modifier in the asset's AnimationModifiers user-data stack (survives save/reload). Default false = transient apply only."), TEXT("false"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("list_anim_modifiers"),
		TEXT("List animation modifiers applied to a sequence"),
		FMonolithActionHandler::CreateStatic(&HandleListAnimModifiers),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_composite_info"),
		TEXT("Get segments and metadata from an animation composite"),
		FMonolithActionHandler::CreateStatic(&HandleGetCompositeInfo),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimComposite asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_composite_segment"),
		TEXT("Add a segment to an animation composite"),
		FMonolithActionHandler::CreateStatic(&HandleAddCompositeSegment),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimComposite asset path"))
			.RequiredAssetPath(TEXT("anim_path"), TEXT("Animation sequence to add"))
			.Optional(TEXT("start_pos"), TEXT("number"), TEXT("Start position in composite timeline"), TEXT("0.0"))
			.Optional(TEXT("play_rate"), TEXT("number"), TEXT("Playback rate"), TEXT("1.0"))
			.Optional(TEXT("looping_count"), TEXT("integer"), TEXT("Number of loops"), TEXT("1"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_composite_segment"),
		TEXT("Remove a segment from an animation composite by index"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveCompositeSegment),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimComposite asset path"))
			.Required(TEXT("segment_index"), TEXT("integer"), TEXT("Index of the segment to remove"))
			.Build());

	// Wave 8a — IKRig
	Registry.RegisterAction(TEXT("animation"), TEXT("get_ikrig_info"),
		TEXT("Get IK Rig asset info: solvers, goals, retarget chains, and skeleton overview"),
		FMonolithActionHandler::CreateStatic(&HandleGetIKRigInfo),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("IKRig asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_ik_solver"),
		TEXT("Add a solver to an IK Rig asset, optionally setting a root bone and goals"),
		FMonolithActionHandler::CreateStatic(&HandleAddIKSolver),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("IKRig asset path"))
			.Required(TEXT("solver_type"), TEXT("string"), TEXT("Solver type — friendly alias (fullbodyik/fbik, limb, pole, bodymover, settransform, stretchlimb) or the exact reflected struct name (e.g. IKRigFullBodyIKSolver). Resolved against the live solver-struct table; an unknown value returns the available list."))
			.Optional(TEXT("root_bone"), TEXT("string"), TEXT("Root/start bone for the solver (meaningful for solvers that use a start bone, e.g. FullBodyIK)"))
			.Optional(TEXT("goals"), TEXT("array"), TEXT("Array of {name, bone} goal objects to create and connect"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_ik_solver"),
		TEXT("Remove a solver from an IK Rig asset by stack index"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveIKSolver),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("IKRig asset path"))
			.Required(TEXT("solver_index"), TEXT("integer"), TEXT("Index of the solver to remove (0-based stack index)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_retargeter_info"),
		TEXT("Get IK Retargeter asset info: source/target rigs, preview meshes, and chain mappings"),
		FMonolithActionHandler::CreateStatic(&HandleGetRetargeterInfo),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("IKRetargeter asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_retarget_chain_mapping"),
		TEXT("Set chain mappings on an IK Retargeter via auto-map or manual source/target pair"),
		FMonolithActionHandler::CreateStatic(&HandleSetRetargetChainMapping),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("IKRetargeter asset path"))
			.Optional(TEXT("auto_map"), TEXT("string"), TEXT("Auto-map mode: exact, fuzzy, or clear"))
			.Optional(TEXT("source_chain"), TEXT("string"), TEXT("Source chain name for manual mapping"))
			.Optional(TEXT("target_chain"), TEXT("string"), TEXT("Target chain name for manual mapping"))
			.Build());

	// Wave 8b — Control Rig Read
	Registry.RegisterAction(TEXT("animation"), TEXT("get_control_rig_info"),
		TEXT("Get Control Rig hierarchy info: elements by type with parents, control settings, and counts"),
		FMonolithActionHandler::CreateStatic(&HandleGetControlRigInfo),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("ControlRigBlueprint asset path"))
			.Optional(TEXT("element_type"), TEXT("string"), TEXT("Filter: bone, control, null, curve, all (default: all)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_control_rig_variables"),
		TEXT("Get animatable controls and blueprint variables from a Control Rig asset"),
		FMonolithActionHandler::CreateStatic(&HandleGetControlRigVariables),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("ControlRigBlueprint asset path"))
			.Build());

	// Wave 8c — Control Rig Write
	Registry.RegisterAction(TEXT("animation"), TEXT("add_control_rig_element"),
		TEXT("Add a bone, control, or null element to a Control Rig hierarchy"),
		FMonolithActionHandler::CreateStatic(&HandleAddControlRigElement),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("ControlRigBlueprint asset path"))
			.Required(TEXT("element_type"), TEXT("string"), TEXT("Element type: bone, control, or null"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Name for the new element"))
			.Optional(TEXT("parent"), TEXT("string"), TEXT("Parent element name"))
			.Optional(TEXT("parent_type"), TEXT("string"), TEXT("Parent element type (default: same as element_type)"))
			.Optional(TEXT("control_type"), TEXT("string"), TEXT("For controls: Float, Integer, Transform, Rotator, Position, Scale, Vector2D (default: Transform)"))
			.Optional(TEXT("animatable"), TEXT("bool"), TEXT("Whether control is animatable (default: true)"), TEXT("true"))
			.Optional(TEXT("transform"), TEXT("object"), TEXT("Initial transform: {tx, ty, tz, rx, ry, rz}"))
			.Build());

	// Wave 10 — ABP Write
	Registry.RegisterAction(TEXT("animation"), TEXT("add_state_to_machine"),
		TEXT("Add a state node to an existing state machine in an animation blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleAddStateToMachine),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("machine_name"), TEXT("string"), TEXT("State machine name (exact, as shown in get_state_machines)"))
			.Required(TEXT("state_name"), TEXT("string"), TEXT("Name for the new state"))
			.Optional(TEXT("position_x"), TEXT("integer"), TEXT("Node X position (default: 200)"), TEXT("200"))
			.Optional(TEXT("position_y"), TEXT("integer"), TEXT("Node Y position (default: 0)"), TEXT("0"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_conduit"),
		TEXT("Add a conduit node to an existing state machine. A conduit is a shared transition hub: transitions route INTO it and its internal boolean rule gates onward transitions. "
			 "NOTE: a conduit's bound graph is a TRANSITION-LOGIC graph (a boolean rule graph), NOT an anim/pose graph - it has no pose sink, so do not target it with pose-pin actions "
			 "(set_state_result_source, add_anim_graph_node with a player node, etc.). The result reports graph_kind='transition_logic'. Transitions to/from the conduit use the existing add_transition. "
			 "Recompiles the blueprint; marks the package dirty (saving is the caller's concern)."),
		FMonolithActionHandler::CreateStatic(&HandleAddConduit),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("machine_name"), TEXT("string"), TEXT("State machine name (exact, as shown in get_state_machines)"))
			.Required(TEXT("conduit_name"), TEXT("string"), TEXT("Name for the new conduit"))
			.Optional(TEXT("position_x"), TEXT("integer"), TEXT("Node X position (default: 200)"), TEXT("200"))
			.Optional(TEXT("position_y"), TEXT("integer"), TEXT("Node Y position (default: 0)"), TEXT("0"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_transition"),
		TEXT("Add a transition between two states in a state machine"),
		FMonolithActionHandler::CreateStatic(&HandleAddTransition),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("machine_name"), TEXT("string"), TEXT("State machine name"))
			.Required(TEXT("from_state"), TEXT("string"), TEXT("Source state name"))
			.Required(TEXT("to_state"), TEXT("string"), TEXT("Destination state name"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_transition_rule"),
		TEXT("Author a state machine transition's condition: a boolean variable, the sequence-player auto rule, a numeric comparison (var/Abs(var) vs constant), or a compound AND/OR expression of multiple comparison terms. Transaction-safe: rolls back on compile failure with no dirty package."),
		FMonolithActionHandler::CreateStatic(&HandleSetTransitionRule),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("machine_name"), TEXT("string"), TEXT("State machine name"))
			.Required(TEXT("from_state"), TEXT("string"), TEXT("Source state name"))
			.Required(TEXT("to_state"), TEXT("string"), TEXT("Destination state name"))
			.Optional(TEXT("variable_name"), TEXT("string"), TEXT("Legacy/back-compat: boolean variable name. Equivalent to rule={kind:bool, variable:<name>}. Use 'rule' for non-bool conditions."))
			.Optional(TEXT("rule"), TEXT("object"), TEXT("Structured rule. String 'auto'/'automatic' or a bool variable name also accepted. Object forms: {kind:'bool', variable:'X'} | {kind:'auto'} | {kind:'compare', lhs:'X' or 'Abs(X)', op:'>'|'<'|'>='|'<='|'=='|'!=', rhs:<number>} | {kind:'expression', combine:'and'|'or' (default 'and'), terms:[{lhs:'X' or 'Abs(X)', op:<compare op>, rhs:<number>, abs?:bool, negate?:bool}, ...]}. The 'expression' kind folds its terms through chained BooleanAND/BooleanOR (a single term degrades to a plain compare)."))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_transition_rule"),
		TEXT("Read back a state machine transition's current rule as structured data (kind=auto/bool/compare/expression/none/custom, operands, op, rhs, comparison string; expression rules report combine + decoded terms)."),
		FMonolithActionHandler::CreateStatic(&HandleGetTransitionRule),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("machine_name"), TEXT("string"), TEXT("State machine name"))
			.Required(TEXT("from_state"), TEXT("string"), TEXT("Source state name"))
			.Required(TEXT("to_state"), TEXT("string"), TEXT("Destination state name"))
			.Build());

	// State-machine editing — removal + entry re-point.
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_anim_state"),
		TEXT("Remove a state from a state machine by name. Also removes the state's dependent transitions "
			 "(incoming + outgoing) when remove_dependent_transitions is true (default); if false and "
			 "transitions exist, errors rather than orphaning them. Refuses to remove the current entry state "
			 "(re-point it with set_anim_entry_state first). The state's inner anim graph is torn down "
			 "automatically. Recompiles the blueprint; marks the package dirty (saving is the caller's concern)."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveAnimState),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("machine_name"), TEXT("string"), TEXT("State machine name"))
			.Required(TEXT("state_name"), TEXT("string"), TEXT("State to remove"))
			.Optional(TEXT("remove_dependent_transitions"), TEXT("bool"), TEXT("Also remove transitions into/out of this state (default: true). If false and transitions exist, the call errors."), TEXT("true"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_anim_entry_state"),
		TEXT("Re-point a state machine's entry/initial state to a different existing state. Breaks the entry "
			 "node's current link and wires it to the named state's input pin. Recompiles the blueprint; "
			 "marks the package dirty (saving is the caller's concern)."),
		FMonolithActionHandler::CreateStatic(&HandleSetAnimEntryState),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("machine_name"), TEXT("string"), TEXT("State machine name"))
			.Required(TEXT("state_name"), TEXT("string"), TEXT("State to make the entry/initial state"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_anim_transition"),
		TEXT("Remove the transition between two named states (directed from_state -> to_state). The transition's "
			 "rule subgraph is torn down automatically. Recompiles the blueprint; marks the package dirty "
			 "(saving is the caller's concern)."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveAnimTransition),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("machine_name"), TEXT("string"), TEXT("State machine name"))
			.Required(TEXT("from_state"), TEXT("string"), TEXT("Source state name"))
			.Required(TEXT("to_state"), TEXT("string"), TEXT("Destination state name"))
			.Build());

	// Wave 16 — State Machine Authoring (#13 / #14)
	Registry.RegisterAction(TEXT("animation"), TEXT("create_state_machine"),
		TEXT("Spawn a new state machine node into an Animation Blueprint's anim graph (auto-creates the SM graph + entry node)"),
		FMonolithActionHandler::CreateStatic(&HandleCreateStateMachine),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Optional(TEXT("state_machine_name"), TEXT("string"), TEXT("Name for the new state machine (default: 'New State Machine')"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Target anim graph name when the ABP has multiple anim graphs (e.g. layered ABPs). Default: the first graph with an AnimationGraphSchema"))
			.Optional(TEXT("position_x"), TEXT("integer"), TEXT("Node X position (default: 200)"), TEXT("200"))
			.Optional(TEXT("position_y"), TEXT("integer"), TEXT("Node Y position (default: 200)"), TEXT("200"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("build_state_machine"),
		TEXT("Declarative state-machine builder: creates the SM then adds states, transitions, and rules in one transaction"),
		FMonolithActionHandler::CreateStatic(&HandleBuildStateMachine),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Optional(TEXT("state_machine_name"), TEXT("string"), TEXT("Name for the state machine (default: 'New State Machine')"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Target anim graph name for layered ABPs (default: first AnimationGraphSchema graph)"))
			.Required(TEXT("states"), TEXT("array"), TEXT("Array of {name, animation?} state specs"))
			.Optional(TEXT("transitions"), TEXT("array"), TEXT("Array of {from, to, rule?} transition specs. rule may be a bool variable name, 'auto'/'automatic' for the sequence-player auto rule, or a structured object {kind:'compare', lhs:'X' or 'Abs(X)', op:'>'|'<'|'>='|'<='|'=='|'!=', rhs:<number>}"))
			.Optional(TEXT("entry_state"), TEXT("string"), TEXT("State to wire as the initial/entry state"))
			.Build());

	// Wave 9 — ABP Read Enhancements
	Registry.RegisterAction(TEXT("animation"), TEXT("get_abp_variables"),
		TEXT("List all variables in an animation blueprint with types and defaults"),
		FMonolithActionHandler::CreateStatic(&HandleGetAbpVariables),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_abp_linked_assets"),
		TEXT("Find all animation assets referenced by an animation blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleGetAbpLinkedAssets),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Build());

	// Skeleton Compatibility — required for legacy UE4 anims on UE5 mannequin etc.
	Registry.RegisterAction(TEXT("animation"), TEXT("get_compatible_skeletons"),
		TEXT("List skeletons declared compatible with the given Skeleton (USkeleton::CompatibleSkeletons)"),
		FMonolithActionHandler::CreateStatic(&HandleGetCompatibleSkeletons),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Skeleton asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_compatible_skeleton"),
		TEXT("Declare another Skeleton as compatible — lets anims authored on the other skeleton play on this one without retargeting"),
		FMonolithActionHandler::CreateStatic(&HandleAddCompatibleSkeleton),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Skeleton asset path (the one being declared compatible)"))
			.Required(TEXT("compatible_with"), TEXT("string"), TEXT("Skeleton asset path to add to the compatibility list"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the modified skeleton asset (default true)"), TEXT("true"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_compatible_skeleton"),
		TEXT("Remove a skeleton from another's CompatibleSkeletons array"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveCompatibleSkeleton),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Skeleton asset path"))
			.Required(TEXT("compatible_with"), TEXT("string"), TEXT("Skeleton asset path to remove from the compatibility list"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the modified skeleton asset (default true)"), TEXT("true"))
			.Build());

	// Wave 11 — Asset Creation + Setup
	Registry.RegisterAction(TEXT("animation"), TEXT("create_blend_space"),
		TEXT("Create a new 2D BlendSpace asset with skeleton and optional axis config"),
		FMonolithActionHandler::CreateStatic(&HandleCreateBlendSpace),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new BlendSpace (e.g. /Game/Anims/BS_Locomotion)"))
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"))
			.Optional(TEXT("axis_x_name"), TEXT("string"), TEXT("X axis display name (default: None)"))
			.Optional(TEXT("axis_x_min"), TEXT("number"), TEXT("X axis minimum value (default: 0)"))
			.Optional(TEXT("axis_x_max"), TEXT("number"), TEXT("X axis maximum value (default: 100)"))
			.Optional(TEXT("axis_y_name"), TEXT("string"), TEXT("Y axis display name (default: None)"))
			.Optional(TEXT("axis_y_min"), TEXT("number"), TEXT("Y axis minimum value (default: 0)"))
			.Optional(TEXT("axis_y_max"), TEXT("number"), TEXT("Y axis maximum value (default: 100)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("create_blend_space_1d"),
		TEXT("Create a new 1D BlendSpace asset with skeleton and optional axis config"),
		FMonolithActionHandler::CreateStatic(&HandleCreateBlendSpace1D),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new BlendSpace1D"))
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"))
			.Optional(TEXT("axis_name"), TEXT("string"), TEXT("Axis display name (default: None)"))
			.Optional(TEXT("axis_min"), TEXT("number"), TEXT("Axis minimum value (default: 0)"))
			.Optional(TEXT("axis_max"), TEXT("number"), TEXT("Axis maximum value (default: 100)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("create_aim_offset"),
		TEXT("Create a new 2D AimOffset asset with Yaw/Pitch axis defaults"),
		FMonolithActionHandler::CreateStatic(&HandleCreateAimOffset),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new AimOffset"))
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"))
			.Optional(TEXT("axis_x_name"), TEXT("string"), TEXT("X axis display name (default: Yaw)"))
			.Optional(TEXT("axis_x_min"), TEXT("number"), TEXT("X axis minimum (default: -180)"))
			.Optional(TEXT("axis_x_max"), TEXT("number"), TEXT("X axis maximum (default: 180)"))
			.Optional(TEXT("axis_y_name"), TEXT("string"), TEXT("Y axis display name (default: Pitch)"))
			.Optional(TEXT("axis_y_min"), TEXT("number"), TEXT("Y axis minimum (default: -90)"))
			.Optional(TEXT("axis_y_max"), TEXT("number"), TEXT("Y axis maximum (default: 90)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("create_aim_offset_1d"),
		TEXT("Create a new 1D AimOffset asset with Yaw axis default"),
		FMonolithActionHandler::CreateStatic(&HandleCreateAimOffset1D),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new AimOffset1D"))
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"))
			.Optional(TEXT("axis_name"), TEXT("string"), TEXT("Axis display name (default: Yaw)"))
			.Optional(TEXT("axis_min"), TEXT("number"), TEXT("Axis minimum (default: -180)"))
			.Optional(TEXT("axis_max"), TEXT("number"), TEXT("Axis maximum (default: 180)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("create_composite"),
		TEXT("Create a new AnimComposite asset with skeleton"),
		FMonolithActionHandler::CreateStatic(&HandleCreateComposite),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new composite"))
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("create_anim_blueprint"),
		TEXT("Create a new Animation Blueprint asset with skeleton"),
		FMonolithActionHandler::CreateStatic(&HandleCreateAnimBlueprint),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new ABP (e.g. /Game/ABP/ABP_Character)"))
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"))
			.Optional(TEXT("parent_class"), TEXT("string"), TEXT("Parent class name (default: AnimInstance)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("compare_skeletons"),
		TEXT("Compare two skeletons for bone compatibility — useful before retargeting or sharing animations"),
		FMonolithActionHandler::CreateStatic(&HandleCompareSkeletons),
		FParamSchemaBuilder()
			.Required(TEXT("skeleton_a"), TEXT("string"), TEXT("First skeleton asset path"))
			.Required(TEXT("skeleton_b"), TEXT("string"), TEXT("Second skeleton asset path"))
			.Build());

	// Wave 12 — Sequence Properties + Sync Markers
	Registry.RegisterAction(TEXT("animation"), TEXT("set_sequence_properties"),
		TEXT("Set playback properties on an AnimSequence (rate_scale, loop, interpolation)"),
		FMonolithActionHandler::CreateStatic(&HandleSetSequenceProperties),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Optional(TEXT("rate_scale"), TEXT("number"), TEXT("Playback rate scale (default 1.0)"))
			.Optional(TEXT("loop"), TEXT("bool"), TEXT("Whether the sequence loops"))
			.Optional(TEXT("interpolation"), TEXT("string"), TEXT("Interpolation type: Linear or Step"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_additive_settings"),
		TEXT("Configure additive animation settings on a sequence (triggers DDC rebuild)"),
		FMonolithActionHandler::CreateStatic(&HandleSetAdditiveSettings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Optional(TEXT("additive_anim_type"), TEXT("string"), TEXT("NoAdditive, LocalSpace, or MeshSpace"))
			.Optional(TEXT("ref_pose_type"), TEXT("string"), TEXT("RefPose, AnimScaled, AnimFrame, or LocalAnimFrame"))
			.Optional(TEXT("ref_frame_index"), TEXT("integer"), TEXT("Reference frame index for AnimFrame/LocalAnimFrame modes"))
			.Optional(TEXT("ref_pose_seq"), TEXT("string"), TEXT("Reference pose sequence asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_compression_settings"),
		TEXT("Assign bone and/or curve compression settings assets to a sequence"),
		FMonolithActionHandler::CreateStatic(&HandleSetCompressionSettings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Optional(TEXT("bone_compression"), TEXT("string"), TEXT("Bone compression settings asset path"))
			.Optional(TEXT("curve_compression"), TEXT("string"), TEXT("Curve compression settings asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_sync_markers"),
		TEXT("Read all authored sync markers from an animation sequence"),
		FMonolithActionHandler::CreateStatic(&HandleGetSyncMarkers),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_sync_marker"),
		TEXT("Add an authored sync marker to a sequence"),
		FMonolithActionHandler::CreateStatic(&HandleAddSyncMarker),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Required(TEXT("marker_name"), TEXT("string"), TEXT("Sync marker name (e.g. FootDown)"))
			.Required(TEXT("time"), TEXT("number"), TEXT("Time in seconds"))
			.Optional(TEXT("track_index"), TEXT("integer"), TEXT("Track index (default 0)"), TEXT("0"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_sync_marker"),
		TEXT("Remove sync markers by name (all with that name) or by index"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveSyncMarker),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Optional(TEXT("marker_name"), TEXT("string"), TEXT("Remove all markers with this name"))
			.Optional(TEXT("marker_index"), TEXT("integer"), TEXT("Remove specific marker by index"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("rename_sync_marker"),
		TEXT("Rename all sync markers with a given name to a new name"),
		FMonolithActionHandler::CreateStatic(&HandleRenameSyncMarker),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Required(TEXT("old_name"), TEXT("string"), TEXT("Current marker name"))
			.Required(TEXT("new_name"), TEXT("string"), TEXT("New marker name"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("derive_foot_sync_markers"),
		TEXT("Auto-derive left/right foot-plant sync markers on an AnimSequence from data already in the clip, via a 5-signal availability cascade (first available wins): existing markers -> footstep notifies -> contact_l/_r curves -> Phase curve extrema -> component-space foot-bone speed minima (native port of the engine FootstepAnimEventsModifier FootBoneSpeed technique). An explicit method=from_bones mode (source='bones') derives plants from per-frame foot-bone vertical position + planar-speed minima. Project-agnostic: all names/bones/thresholds are overridable. Honours dry_run."),
		FMonolithActionHandler::CreateStatic(&HandleDeriveFootSyncMarkers),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("AnimSequence asset path"))
			.Optional(TEXT("left_marker_name"), TEXT("string"), TEXT("Marker name written for left foot plants (default L_Foot)"), TEXT("L_Foot"))
			.Optional(TEXT("right_marker_name"), TEXT("string"), TEXT("Marker name written for right foot plants (default R_Foot)"), TEXT("R_Foot"))
			.Optional(TEXT("track_index"), TEXT("integer"), TEXT("Sync-marker track index (default 0)"), TEXT("0"))
			.Optional(TEXT("method"), TEXT("string"), TEXT("auto|existing|notifies|contact|phase|footspeed|from_bones (default auto). Non-auto forces a single signal and errors cleanly if that signal is unavailable. from_bones = planar-speed-minima + ground-height plant detection from foot bones (reports source='bones')."), TEXT("auto"))
			.Optional(TEXT("foot_bones"), TEXT("object"), TEXT("{left, right} foot bone names for the footspeed / from_bones signals. If omitted, common names are auto-resolved against the skeleton."))
			.Optional(TEXT("speed_threshold"), TEXT("number"), TEXT("Top-level alias for thresholds.speed_threshold (planar-speed valley threshold, used by from_bones). Overrides the thresholds-object value."))
			.Optional(TEXT("ground_height_threshold"), TEXT("number"), TEXT("Top-level alias for thresholds.ground_threshold (cm above the lowest observed foot Z within which a planar-speed valley counts as a ground contact; from_bones mode)."))
			.Optional(TEXT("thresholds"), TEXT("object"), TEXT("{contact_mid, contact_low, speed_threshold, sample_rate, debounce_fraction, ground_threshold} — all optional, per-signal defaults applied."))
			.Optional(TEXT("notify_track_patterns"), TEXT("object"), TEXT("{left, right} case-insensitive substring patterns used to classify footstep-notify foot side by track name (defaults 'footstep left'/'footstep right')."))
			.Optional(TEXT("phase_invert"), TEXT("boolean"), TEXT("Flip L/R polarity of the Phase signal (default false; +1=left)"), TEXT("false"))
			.Optional(TEXT("clear_existing"), TEXT("boolean"), TEXT("Remove pre-existing markers named left/right_marker_name before writing, for idempotency (default true). Ignored when source=existing."), TEXT("true"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Compute and report derived times without mutating the asset (default false)"), TEXT("false"))
			.Build());

	// Wave 13 — Batch Ops + Montage Completion
	Registry.RegisterAction(TEXT("animation"), TEXT("batch_execute"),
		TEXT("Execute multiple animation actions in a single transaction"),
		FMonolithActionHandler::CreateStatic(&HandleBatchExecute),
		FParamSchemaBuilder()
			.Required(TEXT("operations"), TEXT("array"), TEXT("Array of {op, ...params} objects. Params are flat inline, not nested."))
			.Optional(TEXT("stop_on_error"), TEXT("boolean"), TEXT("Stop on first error (default false)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_montage_anim_segment"),
		TEXT("Add an animation segment to a montage slot track"),
		FMonolithActionHandler::CreateStatic(&HandleAddMontageAnimSegment),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Montage asset path"))
			.RequiredAssetPath(TEXT("anim_path"), TEXT("AnimSequence to reference"))
			.Optional(TEXT("slot_index"), TEXT("integer"), TEXT("Slot track index (default 0)"))
			.Optional(TEXT("start_pos"), TEXT("number"), TEXT("Position in montage timeline (default: auto-append after last segment)"))
			.Optional(TEXT("anim_start_time"), TEXT("number"), TEXT("Clip start time within source anim (default 0.0)"))
			.Optional(TEXT("anim_end_time"), TEXT("number"), TEXT("Clip end time within source anim (default: full length)"))
			.Optional(TEXT("play_rate"), TEXT("number"), TEXT("Playback rate (default 1.0)"))
			.Optional(TEXT("looping_count"), TEXT("integer"), TEXT("Loop count (default 1)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("clone_notify_setup"),
		TEXT("Copy all notifies from one animation asset to another with optional time scaling"),
		FMonolithActionHandler::CreateStatic(&HandleCloneNotifySetup),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("source_path"), TEXT("Source animation asset path"))
			.RequiredAssetPath(TEXT("target_path"), TEXT("Target animation asset path"))
			.Optional(TEXT("time_scale"), TEXT("number"), TEXT("Manual time scale factor (default 1.0)"))
			.Optional(TEXT("auto_scale"), TEXT("boolean"), TEXT("Auto-compute scale from duration ratio (default false)"))
			.Optional(TEXT("replace_existing"), TEXT("boolean"), TEXT("Clear target notifies first (default false)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("bulk_add_notify"),
		TEXT("Add the same notify type to multiple animation assets at once"),
		FMonolithActionHandler::CreateStatic(&HandleBulkAddNotify),
		FParamSchemaBuilder()
			.Required(TEXT("asset_paths"), TEXT("array"), TEXT("Array of animation asset paths"))
			.Required(TEXT("notify_class"), TEXT("string"), TEXT("Notify class name"))
			.Required(TEXT("time"), TEXT("number"), TEXT("Trigger time"))
			.Optional(TEXT("time_mode"), TEXT("string"), TEXT("'absolute' or 'normalized' (0.0-1.0, default 'absolute')"))
			.Optional(TEXT("duration"), TEXT("number"), TEXT("Duration for notify states"))
			.Optional(TEXT("track_name"), TEXT("string"), TEXT("Notify track name (default '1')"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("create_montage_from_sections"),
		TEXT("Create a montage with slot, anim segments, sections, blend, and notifies in one call"),
		FMonolithActionHandler::CreateStatic(&HandleCreateMontageFromSections),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new montage"))
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"))
			.Optional(TEXT("slot_name"), TEXT("string"), TEXT("Slot name (default 'DefaultSlot')"))
			.Optional(TEXT("sections"), TEXT("array"), TEXT("Array of {name, anim_path, start_time?, next_section?}"))
			.Optional(TEXT("blend"), TEXT("object"), TEXT("{blend_in_time?, blend_out_time?, blend_out_trigger_time?, enable_auto_blend_out?}"))
			.Optional(TEXT("notifies"), TEXT("array"), TEXT("Array of {notify_class, time, duration?, track_name?}"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("build_sequence_from_poses"),
		TEXT("Build an AnimSequence from per-frame bone transforms using IAnimationDataController"),
		FMonolithActionHandler::CreateStatic(&HandleBuildSequenceFromPoses),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Target AnimSequence path (created if missing)"))
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"))
			.Required(TEXT("frames"), TEXT("array"), TEXT("Array of {bones: [{name, location:[x,y,z], rotation:[x,y,z,w], scale:[x,y,z]}, ...]}"))
			.Optional(TEXT("frame_rate"), TEXT("number"), TEXT("Frame rate (default 30)"))
			.Build());

	// Wave 14 — Notify Properties
	Registry.RegisterAction(TEXT("animation"), TEXT("set_notify_properties"),
		TEXT("Set UPROPERTY values on a notify object using reflection (ImportText_Direct). Works on both instant and state notifies."),
		FMonolithActionHandler::CreateStatic(&HandleSetNotifyProperties),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation asset path (sequence, montage, composite)"))
			.Required(TEXT("notify_index"), TEXT("integer"), TEXT("Index of the notify in the Notifies array"))
			.Required(TEXT("properties"), TEXT("object"), TEXT("Object of property_name:value pairs. Values use UE text import format (same as Details panel copy/paste)."))
			.Build());

	// Wave 15 — Physics Assets + IK Chains
	Registry.RegisterAction(TEXT("animation"), TEXT("get_physics_asset_info"),
		TEXT("Read all bodies, constraints, profiles, and solver settings from a physics asset"),
		FMonolithActionHandler::CreateStatic(&HandleGetPhysicsAssetInfo),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Physics asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_body_properties"),
		TEXT("Modify mass, physics type, collision, damping on a physics body identified by bone name"),
		FMonolithActionHandler::CreateStatic(&HandleSetBodyProperties),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Physics asset path"))
			.Required(TEXT("bone_name"), TEXT("string"), TEXT("Bone name identifying the body"))
			.Optional(TEXT("mass"), TEXT("number"), TEXT("Mass override in kg (enables bOverrideMass)"))
			.Optional(TEXT("physics_type"), TEXT("string"), TEXT("Default, Kinematic, or Simulated"))
			.Optional(TEXT("collision_enabled"), TEXT("boolean"), TEXT("Enable/disable collision"))
			.Optional(TEXT("collision_profile"), TEXT("string"), TEXT("Collision profile name"))
			.Optional(TEXT("linear_damping"), TEXT("number"), TEXT("Linear damping"))
			.Optional(TEXT("angular_damping"), TEXT("number"), TEXT("Angular damping"))
			.Optional(TEXT("enable_gravity"), TEXT("boolean"), TEXT("Enable gravity"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_constraint_properties"),
		TEXT("Modify angular/linear limits on a physics constraint by index or bone pair"),
		FMonolithActionHandler::CreateStatic(&HandleSetConstraintProperties),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Physics asset path"))
			.Optional(TEXT("constraint_index"), TEXT("integer"), TEXT("Constraint index (alternative to bone pair)"))
			.Optional(TEXT("bone_1"), TEXT("string"), TEXT("Child bone name (with bone_2, alternative to constraint_index)"))
			.Optional(TEXT("bone_2"), TEXT("string"), TEXT("Parent bone name (with bone_1, alternative to constraint_index)"))
			.Optional(TEXT("swing1_motion"), TEXT("string"), TEXT("Free, Limited, or Locked"))
			.Optional(TEXT("swing1_limit"), TEXT("number"), TEXT("Swing1 limit in degrees"))
			.Optional(TEXT("swing2_motion"), TEXT("string"), TEXT("Free, Limited, or Locked"))
			.Optional(TEXT("swing2_limit"), TEXT("number"), TEXT("Swing2 limit in degrees"))
			.Optional(TEXT("twist_motion"), TEXT("string"), TEXT("Free, Limited, or Locked"))
			.Optional(TEXT("twist_limit"), TEXT("number"), TEXT("Twist limit in degrees"))
			.Optional(TEXT("disable_collision"), TEXT("boolean"), TEXT("Disable collision between constrained bodies"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_retarget_chain"),
		TEXT("Add a retarget chain to an IK Rig asset"),
		FMonolithActionHandler::CreateStatic(&HandleAddRetargetChain),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("IK Rig asset path"))
			.Required(TEXT("chain_name"), TEXT("string"), TEXT("Name for the chain"))
			.Required(TEXT("start_bone"), TEXT("string"), TEXT("Start bone name"))
			.Required(TEXT("end_bone"), TEXT("string"), TEXT("End bone name"))
			.Optional(TEXT("goal_name"), TEXT("string"), TEXT("Goal to associate with this chain"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_retarget_chain"),
		TEXT("Remove a retarget chain from an IK Rig asset"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveRetargetChain),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("IK Rig asset path"))
			.Required(TEXT("chain_name"), TEXT("string"), TEXT("Name of the chain to remove"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_retarget_chain_bones"),
		TEXT("Update start/end bones of an existing retarget chain in an IK Rig"),
		FMonolithActionHandler::CreateStatic(&HandleSetRetargetChainBones),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("IK Rig asset path"))
			.Required(TEXT("chain_name"), TEXT("string"), TEXT("Name of the chain to modify"))
			.Optional(TEXT("start_bone"), TEXT("string"), TEXT("New start bone name"))
			.Optional(TEXT("end_bone"), TEXT("string"), TEXT("New end bone name"))
			.Optional(TEXT("goal_name"), TEXT("string"), TEXT("New goal name"))
			.Optional(TEXT("new_name"), TEXT("string"), TEXT("Rename the chain"))
			.Build());

	// --- Retarget CREATE/RUN pack (gap only) ---
	Registry.RegisterAction(TEXT("animation"), TEXT("create_ik_rig"),
		TEXT("Create a new IK Rig asset that previews a given skeletal mesh. The mesh's skeleton is loaded into the IK Rig (use add_ik_solver / add_retarget_chain to populate it)."),
		FMonolithActionHandler::CreateStatic(&HandleCreateIKRig),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Destination IK Rig asset path (e.g. /Game/Path/IK_MyRig)"))
			.Required(TEXT("skeletal_mesh_path"), TEXT("string"), TEXT("Skeletal mesh the IK Rig previews; its skeleton is loaded into the rig"))
			.Optional(TEXT("pelvis_bone"), TEXT("string"), TEXT("Retarget root bone (pelvis). Set after the mesh is assigned."))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("create_ik_retargeter"),
		TEXT("Create a new IK Retargeter asset. Optionally assign source/target IK Rigs here, or later via set_retargeter_rigs."),
		FMonolithActionHandler::CreateStatic(&HandleCreateIKRetargeter),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Destination IK Retargeter asset path (e.g. /Game/Path/RTG_MyRetarget)"))
			.Optional(TEXT("source_ik_rig_path"), TEXT("string"), TEXT("Source IK Rig asset path"))
			.Optional(TEXT("target_ik_rig_path"), TEXT("string"), TEXT("Target IK Rig asset path"))
			.Optional(TEXT("auto_map"), TEXT("string"), TEXT("Chain auto-map mode when both rigs are set: 'fuzzy' (default, name-similarity) or 'exact'"), TEXT("fuzzy"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_retargeter_rigs"),
		TEXT("Set the source and target IK Rigs (and optional preview meshes) on an existing IK Retargeter."),
		FMonolithActionHandler::CreateStatic(&HandleSetRetargeterRigs),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("IK Retargeter asset path"))
			.Required(TEXT("source_ik_rig_path"), TEXT("string"), TEXT("Source IK Rig asset path"))
			.Required(TEXT("target_ik_rig_path"), TEXT("string"), TEXT("Target IK Rig asset path"))
			.Optional(TEXT("source_preview_mesh"), TEXT("string"), TEXT("Source preview skeletal mesh path"))
			.Optional(TEXT("target_preview_mesh"), TEXT("string"), TEXT("Target preview skeletal mesh path"))
			.Optional(TEXT("auto_map"), TEXT("string"), TEXT("Chain auto-map mode for seeded ops: 'fuzzy' (default) or 'exact'"), TEXT("fuzzy"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("batch_retarget_animations"),
		TEXT("Duplicate and retarget a list of animation assets cross-skeleton using an IK Retargeter. Outputs new clips bound to the target skeleton into output_folder."),
		FMonolithActionHandler::CreateStatic(&HandleBatchRetargetAnimations),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("retargeter_path"), TEXT("IK Retargeter asset path"))
			.Required(TEXT("source_anims"), TEXT("array"), TEXT("Array of source animation asset paths to retarget"))
			.Required(TEXT("output_folder"), TEXT("string"), TEXT("Destination folder for retargeted clips (e.g. /Game/Path/Retargeted)"))
			.Optional(TEXT("source_mesh"), TEXT("string"), TEXT("Source skeletal mesh (defaults to the retargeter's source preview mesh)"))
			.Optional(TEXT("target_mesh"), TEXT("string"), TEXT("Target skeletal mesh (defaults to the retargeter's target preview mesh)"))
			.Optional(TEXT("name_prefix"), TEXT("string"), TEXT("Prefix added to each output asset name"))
			.Optional(TEXT("name_suffix"), TEXT("string"), TEXT("Suffix added to each output asset name"))
			.Optional(TEXT("search"), TEXT("string"), TEXT("Substring to search for in source names (replaced with 'replace')"))
			.Optional(TEXT("replace"), TEXT("string"), TEXT("Replacement for the 'search' substring"))
			.Optional(TEXT("include_referenced"), TEXT("bool"), TEXT("Also retarget assets referenced by the inputs (default: false)"), TEXT("false"))
			.Optional(TEXT("overwrite"), TEXT("bool"), TEXT("Overwrite existing output files instead of creating uniquely-named copies (default: false)"), TEXT("false"))
			.Optional(TEXT("auto_map"), TEXT("string"), TEXT("Chain auto-map mode if ops must be seeded on run: 'fuzzy' (default) or 'exact'"), TEXT("fuzzy"))
			.Build());

}

// ---------------------------------------------------------------------------
// Montage Sections
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleAddMontageSection(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SectionName = Params->GetStringField(TEXT("section_name"));
	double StartTime = Params->GetNumberField(TEXT("start_time"));

	UAnimMontage* Montage = FMonolithAssetUtils::LoadAssetByPath<UAnimMontage>(AssetPath);
	if (!Montage) return FMonolithActionResult::Error(FString::Printf(TEXT("Montage not found: %s"), *AssetPath));

	GEditor->BeginTransaction(FText::FromString(TEXT("Add Montage Section")));
	Montage->Modify();

	int32 Index = Montage->AddAnimCompositeSection(FName(*SectionName), static_cast<float>(StartTime));

	GEditor->EndTransaction();

	if (Index == INDEX_NONE)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to add section '%s' (name may already exist)"), *SectionName));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("section_name"), SectionName);
	Root->SetNumberField(TEXT("index"), Index);
	Root->SetNumberField(TEXT("start_time"), StartTime);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleDeleteMontageSection(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	int32 SectionIndex = static_cast<int32>(Params->GetNumberField(TEXT("section_index")));

	UAnimMontage* Montage = FMonolithAssetUtils::LoadAssetByPath<UAnimMontage>(AssetPath);
	if (!Montage) return FMonolithActionResult::Error(FString::Printf(TEXT("Montage not found: %s"), *AssetPath));

	if (!Montage->IsValidSectionIndex(SectionIndex))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid section index: %d"), SectionIndex));

	if (Montage->CompositeSections.Num() <= 1)
		return FMonolithActionResult::Error(TEXT("Cannot delete the last remaining montage section"));

	FName SectionName = Montage->GetSectionName(SectionIndex);

	GEditor->BeginTransaction(FText::FromString(TEXT("Delete Montage Section")));
	Montage->Modify();
	bool bSuccess = Montage->DeleteAnimCompositeSection(SectionIndex);
	GEditor->EndTransaction();

	if (!bSuccess)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to delete section at index %d"), SectionIndex));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("deleted_section"), SectionName.ToString());
	Root->SetNumberField(TEXT("index"), SectionIndex);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetSectionNext(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SectionName = Params->GetStringField(TEXT("section_name"));
	FString NextSectionName = Params->GetStringField(TEXT("next_section_name"));

	UAnimMontage* Montage = FMonolithAssetUtils::LoadAssetByPath<UAnimMontage>(AssetPath);
	if (!Montage) return FMonolithActionResult::Error(FString::Printf(TEXT("Montage not found: %s"), *AssetPath));

	int32 SectionIndex = Montage->GetSectionIndex(FName(*SectionName));
	if (SectionIndex == INDEX_NONE)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Section not found: %s"), *SectionName));

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Section Next")));
	Montage->Modify();
	FCompositeSection& Section = Montage->GetAnimCompositeSection(SectionIndex);
	Section.NextSectionName = FName(*NextSectionName);
	GEditor->EndTransaction();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("section"), SectionName);
	Root->SetStringField(TEXT("next_section"), NextSectionName);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetSectionTime(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SectionName = Params->GetStringField(TEXT("section_name"));
	float NewTime = static_cast<float>(Params->GetNumberField(TEXT("new_time")));

	UAnimMontage* Montage = FMonolithAssetUtils::LoadAssetByPath<UAnimMontage>(AssetPath);
	if (!Montage) return FMonolithActionResult::Error(FString::Printf(TEXT("Montage not found: %s"), *AssetPath));

	int32 SectionIndex = Montage->GetSectionIndex(FName(*SectionName));
	if (SectionIndex == INDEX_NONE)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Section not found: %s"), *SectionName));

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Section Time")));
	Montage->Modify();
	FCompositeSection& Section = Montage->GetAnimCompositeSection(SectionIndex);
	Section.SetTime(NewTime);
	Section.Link(Montage, NewTime, 0);
	GEditor->EndTransaction();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("section"), SectionName);
	Root->SetNumberField(TEXT("new_time"), NewTime);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// BlendSpace Samples
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleAddBlendSpaceSample(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString AnimPath = Params->GetStringField(TEXT("anim_path"));
	float X = static_cast<float>(Params->GetNumberField(TEXT("x")));
	float Y = static_cast<float>(Params->GetNumberField(TEXT("y")));

	UBlendSpace* BS = FMonolithAssetUtils::LoadAssetByPath<UBlendSpace>(AssetPath);
	if (!BS) return FMonolithActionResult::Error(FString::Printf(TEXT("BlendSpace not found: %s"), *AssetPath));

	UAnimSequence* Anim = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AnimPath);
	if (!Anim) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AnimPath));

	USkeleton* BSSkeleton = BS->GetSkeleton();
	USkeleton* AnimSkeleton = Anim->GetSkeleton();
	if (BSSkeleton && AnimSkeleton && BSSkeleton != AnimSkeleton)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Skeleton mismatch: blend space uses '%s' but animation uses '%s'"),
			*BSSkeleton->GetName(), *AnimSkeleton->GetName()));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Add BlendSpace Sample")));
	BS->Modify();
	FVector SampleValue(X, Y, 0.0f);
	int32 Index = BS->AddSample(Anim, SampleValue);
	BS->ValidateSampleData();   // clamp/validate sample positions
	BS->ResampleData();         // rebuild FBlendSpaceData triangulation — REQUIRED for runtime
	GEditor->EndTransaction();
	BS->MarkPackageDirty();     // outside the transaction — dirty is not transactional state

	if (Index == INDEX_NONE)
		return FMonolithActionResult::Error(TEXT("Failed to add sample"));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("index"), Index);
	Root->SetStringField(TEXT("animation"), AnimPath);
	Root->SetNumberField(TEXT("x"), X);
	Root->SetNumberField(TEXT("y"), Y);
	Root->SetBoolField(TEXT("baked"), true);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleEditBlendSpaceSample(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	int32 SampleIndex = static_cast<int32>(Params->GetNumberField(TEXT("sample_index")));
	float X = static_cast<float>(Params->GetNumberField(TEXT("x")));
	float Y = static_cast<float>(Params->GetNumberField(TEXT("y")));
	FString AnimPath;
	Params->TryGetStringField(TEXT("anim_path"), AnimPath);

	UBlendSpace* BS = FMonolithAssetUtils::LoadAssetByPath<UBlendSpace>(AssetPath);
	if (!BS) return FMonolithActionResult::Error(FString::Printf(TEXT("BlendSpace not found: %s"), *AssetPath));

	if (!BS->IsValidBlendSampleIndex(SampleIndex))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid sample index: %d"), SampleIndex));

	GEditor->BeginTransaction(FText::FromString(TEXT("Edit BlendSpace Sample")));
	BS->Modify();

	FVector NewValue(X, Y, 0.0f);
	BS->EditSampleValue(SampleIndex, NewValue);

	if (!AnimPath.IsEmpty())
	{
		UAnimSequence* Anim = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AnimPath);
		if (Anim)
		{
			BS->DeleteSample(SampleIndex);
			BS->AddSample(Anim, NewValue);
		}
	}

	BS->ValidateSampleData();   // clamp/validate sample positions
	BS->ResampleData();         // rebuild FBlendSpaceData triangulation — REQUIRED for runtime
	GEditor->EndTransaction();
	BS->MarkPackageDirty();     // outside the transaction — dirty is not transactional state

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("index"), SampleIndex);
	Root->SetNumberField(TEXT("x"), X);
	Root->SetNumberField(TEXT("y"), Y);
	if (!AnimPath.IsEmpty()) Root->SetStringField(TEXT("animation"), AnimPath);
	Root->SetBoolField(TEXT("baked"), true);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleDeleteBlendSpaceSample(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	int32 SampleIndex = static_cast<int32>(Params->GetNumberField(TEXT("sample_index")));

	UBlendSpace* BS = FMonolithAssetUtils::LoadAssetByPath<UBlendSpace>(AssetPath);
	if (!BS) return FMonolithActionResult::Error(FString::Printf(TEXT("BlendSpace not found: %s"), *AssetPath));

	if (!BS->IsValidBlendSampleIndex(SampleIndex))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid sample index: %d"), SampleIndex));

	GEditor->BeginTransaction(FText::FromString(TEXT("Delete BlendSpace Sample")));
	BS->Modify();
	bool bSuccess = BS->DeleteSample(SampleIndex);
	BS->ValidateSampleData();   // clamp/validate sample positions
	BS->ResampleData();         // rebuild FBlendSpaceData triangulation — REQUIRED for runtime
	GEditor->EndTransaction();
	BS->MarkPackageDirty();     // outside the transaction — dirty is not transactional state

	if (!bSuccess)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to delete sample at index %d"), SampleIndex));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("deleted_index"), SampleIndex);
	Root->SetBoolField(TEXT("baked"), true);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleBakeBlendSpace(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UBlendSpace* BS = FMonolithAssetUtils::LoadAssetByPath<UBlendSpace>(AssetPath);
	if (!BS) return FMonolithActionResult::Error(FString::Printf(TEXT("BlendSpace not found: %s"), *AssetPath));

	const int32 SampleCount = BS->GetBlendSamples().Num();

	BS->ValidateSampleData();   // clamp/validate sample positions
	BS->ResampleData();         // rebuild FBlendSpaceData triangulation — REQUIRED for runtime
	BS->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetBoolField(TEXT("baked"), true);
	Root->SetNumberField(TEXT("sample_count"), SampleCount);
	Root->SetBoolField(TEXT("has_blendspace_data"), !BS->GetBlendSpaceData().IsEmpty());
	Root->SetBoolField(TEXT("saved"), false);
	// 2D triangulation needs >= 3 samples; fewer than that is degenerate (resample is a no-op for 0).
	if (SampleCount > 0 && SampleCount < 3 && !BS->IsA<UBlendSpace1D>())
		Root->SetStringField(TEXT("warning"),
			TEXT("Blend space has fewer than 3 samples — 2D triangulation is degenerate until more are added."));
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetBlendSpaceInterpolation(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UBlendSpace* BS = FMonolithAssetUtils::LoadAssetByPath<UBlendSpace>(AssetPath);
	if (!BS) return FMonolithActionResult::Error(FString::Printf(TEXT("BlendSpace not found: %s"), *AssetPath));

	if (Params->HasField(TEXT("use_grid")))
		BS->bInterpolateUsingGrid = Params->GetBoolField(TEXT("use_grid"));

	if (Params->HasField(TEXT("preferred_triangulation_direction")))
	{
		FString DirStr = Params->GetStringField(TEXT("preferred_triangulation_direction"));
		if (DirStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			BS->PreferredTriangulationDirection = EPreferredTriangulationDirection::None;
		else if (DirStr.Equals(TEXT("Tangential"), ESearchCase::IgnoreCase))
			BS->PreferredTriangulationDirection = EPreferredTriangulationDirection::Tangential;
		else if (DirStr.Equals(TEXT("Radial"), ESearchCase::IgnoreCase))
			BS->PreferredTriangulationDirection = EPreferredTriangulationDirection::Radial;
		else
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Invalid preferred_triangulation_direction '%s' — must be None, Tangential, or Radial"), *DirStr));
	}

	// Rebuild so the chosen interpolation structure (grid samples / triangulation) is populated.
	BS->ResampleData();
	BS->MarkPackageDirty();

	const TCHAR* DirName = TEXT("Tangential");
	switch (BS->PreferredTriangulationDirection)
	{
	case EPreferredTriangulationDirection::None:       DirName = TEXT("None"); break;
	case EPreferredTriangulationDirection::Tangential: DirName = TEXT("Tangential"); break;
	case EPreferredTriangulationDirection::Radial:     DirName = TEXT("Radial"); break;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetBoolField(TEXT("use_grid"), BS->bInterpolateUsingGrid);
	Root->SetStringField(TEXT("preferred_triangulation_direction"), DirName);
	Root->SetBoolField(TEXT("has_blendspace_data"), !BS->GetBlendSpaceData().IsEmpty());
	Root->SetBoolField(TEXT("saved"), false);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Notify Editing
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleSetNotifyTime(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	int32 NotifyIndex = static_cast<int32>(Params->GetNumberField(TEXT("notify_index")));
	float NewTime = static_cast<float>(Params->GetNumberField(TEXT("new_time")));

	UAnimSequenceBase* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequenceBase>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("Animation asset not found: %s"), *AssetPath));

	if (!Seq->Notifies.IsValidIndex(NotifyIndex))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid notify index: %d (total: %d)"), NotifyIndex, Seq->Notifies.Num()));

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Notify Time")));
	Seq->Modify();
	Seq->Notifies[NotifyIndex].SetTime(NewTime);
	GEditor->EndTransaction();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("index"), NotifyIndex);
	Root->SetNumberField(TEXT("new_time"), NewTime);
	Root->SetStringField(TEXT("notify_name"), Seq->Notifies[NotifyIndex].NotifyName.ToString());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetNotifyDuration(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	int32 NotifyIndex = static_cast<int32>(Params->GetNumberField(TEXT("notify_index")));
	float NewDuration = static_cast<float>(Params->GetNumberField(TEXT("new_duration")));

	UAnimSequenceBase* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequenceBase>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("Animation asset not found: %s"), *AssetPath));

	if (!Seq->Notifies.IsValidIndex(NotifyIndex))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid notify index: %d (total: %d)"), NotifyIndex, Seq->Notifies.Num()));

	if (!Seq->Notifies[NotifyIndex].NotifyStateClass)
		return FMonolithActionResult::Error(TEXT("Notify is not a state notify (no duration)"));

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Notify Duration")));
	Seq->Modify();
	Seq->Notifies[NotifyIndex].SetDuration(NewDuration);
	GEditor->EndTransaction();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("index"), NotifyIndex);
	Root->SetNumberField(TEXT("new_duration"), NewDuration);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Bone Tracks
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleAddBoneTrack(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString BoneName = Params->GetStringField(TEXT("bone_name"));

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	IAnimationDataController& Controller = Seq->GetController();

	GEditor->BeginTransaction(FText::FromString(TEXT("Add Bone Track")));
	Seq->Modify();
	Controller.AddBoneCurve(FName(*BoneName));
	GEditor->EndTransaction();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("bone_name"), BoneName);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleRemoveBoneTrack(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString BoneName = Params->GetStringField(TEXT("bone_name"));
	bool bIncludeChildren = false;
	Params->TryGetBoolField(TEXT("include_children"), bIncludeChildren);

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	IAnimationDataController& Controller = Seq->GetController();

	// Null check skeleton for include_children
	if (bIncludeChildren && !Seq->GetSkeleton())
		return FMonolithActionResult::Error(TEXT("Skeleton is null — cannot resolve children"));

	TArray<FName> BonesToRemove;
	FName TargetBone(*BoneName);
	BonesToRemove.Add(TargetBone);

	if (bIncludeChildren && Seq->GetSkeleton())
	{
		const FReferenceSkeleton& RefSkel = Seq->GetSkeleton()->GetReferenceSkeleton();
		int32 BoneIndex = RefSkel.FindBoneIndex(TargetBone);
		if (BoneIndex != INDEX_NONE)
		{
			for (int32 i = 0; i < RefSkel.GetNum(); ++i)
			{
				int32 ParentIdx = i;
				while (ParentIdx != INDEX_NONE)
				{
					if (ParentIdx == BoneIndex && i != BoneIndex)
					{
						BonesToRemove.Add(RefSkel.GetBoneName(i));
						break;
					}
					ParentIdx = RefSkel.GetParentIndex(ParentIdx);
				}
			}
		}
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Remove Bone Track")));
	Seq->Modify();
	int32 RemovedCount = 0;
	for (const FName& Bone : BonesToRemove)
	{
		if (Controller.RemoveBoneTrack(Bone))
		{
			++RemovedCount;
		}
	}
	GEditor->EndTransaction();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("bone_name"), BoneName);
	Root->SetBoolField(TEXT("include_children"), bIncludeChildren);
	Root->SetNumberField(TEXT("removed_count"), RemovedCount);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetBoneTrackKeys(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString BoneName = Params->GetStringField(TEXT("bone_name"));
	FString PositionsJson = Params->GetStringField(TEXT("positions_json"));
	FString RotationsJson = Params->GetStringField(TEXT("rotations_json"));
	FString ScalesJson = Params->GetStringField(TEXT("scales_json"));

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	// Parse positions: [[x,y,z], ...]
	TArray<FVector> Positions;
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PositionsJson);
		TArray<TSharedPtr<FJsonValue>> Arr;
		if (FJsonSerializer::Deserialize(Reader, Arr))
		{
			for (const auto& Val : Arr)
			{
				const TArray<TSharedPtr<FJsonValue>>* Inner;
				if (Val->TryGetArray(Inner) && Inner->Num() >= 3)
				{
					Positions.Add(FVector((*Inner)[0]->AsNumber(), (*Inner)[1]->AsNumber(), (*Inner)[2]->AsNumber()));
				}
			}
		}
	}

	// Parse rotations: [[x,y,z,w], ...]
	TArray<FQuat> Rotations;
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RotationsJson);
		TArray<TSharedPtr<FJsonValue>> Arr;
		if (FJsonSerializer::Deserialize(Reader, Arr))
		{
			for (const auto& Val : Arr)
			{
				const TArray<TSharedPtr<FJsonValue>>* Inner;
				if (Val->TryGetArray(Inner) && Inner->Num() >= 4)
				{
					Rotations.Add(FQuat((*Inner)[0]->AsNumber(), (*Inner)[1]->AsNumber(), (*Inner)[2]->AsNumber(), (*Inner)[3]->AsNumber()));
				}
			}
		}
	}

	// Parse scales: [[x,y,z], ...]
	TArray<FVector> Scales;
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ScalesJson);
		TArray<TSharedPtr<FJsonValue>> Arr;
		if (FJsonSerializer::Deserialize(Reader, Arr))
		{
			for (const auto& Val : Arr)
			{
				const TArray<TSharedPtr<FJsonValue>>* Inner;
				if (Val->TryGetArray(Inner) && Inner->Num() >= 3)
				{
					Scales.Add(FVector((*Inner)[0]->AsNumber(), (*Inner)[1]->AsNumber(), (*Inner)[2]->AsNumber()));
				}
			}
		}
	}

	IAnimationDataController& Controller = Seq->GetController();

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Bone Track Keys")));
	Seq->Modify();

	int32 NumKeys = FMath::Max3(Positions.Num(), Rotations.Num(), Scales.Num());
	Controller.SetBoneTrackKeys(FName(*BoneName), Positions, Rotations, Scales);

	GEditor->EndTransaction();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("bone_name"), BoneName);
	Root->SetNumberField(TEXT("num_keys"), NumKeys);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Virtual Bones
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleAddVirtualBone(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SourceBone = Params->GetStringField(TEXT("source_bone"));
	FString TargetBone = Params->GetStringField(TEXT("target_bone"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(AssetPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *AssetPath));

	const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
	if (RefSkel.FindBoneIndex(FName(*SourceBone)) == INDEX_NONE)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source bone not found in skeleton: %s"), *SourceBone));
	}
	if (RefSkel.FindBoneIndex(FName(*TargetBone)) == INDEX_NONE)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Target bone not found in skeleton: %s"), *TargetBone));
	}

	FName VBoneName;

	GEditor->BeginTransaction(FText::FromString(TEXT("Add Virtual Bone")));
	Skeleton->Modify();
	bool bSuccess = Skeleton->AddNewVirtualBone(FName(*SourceBone), FName(*TargetBone), VBoneName);
	GEditor->EndTransaction();

	if (!bSuccess)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to add virtual bone from '%s' to '%s'"), *SourceBone, *TargetBone));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("virtual_bone"), VBoneName.ToString());
	Root->SetStringField(TEXT("source"), SourceBone);
	Root->SetStringField(TEXT("target"), TargetBone);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleRemoveVirtualBones(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(AssetPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *AssetPath));

	// Extract bone names from JSON array
	TArray<FString> BoneNames;
	const TArray<TSharedPtr<FJsonValue>>* BoneNamesArray;
	if (Params->TryGetArrayField(TEXT("bone_names"), BoneNamesArray))
	{
		for (const auto& Val : *BoneNamesArray)
		{
			BoneNames.Add(Val->AsString());
		}
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Remove Virtual Bones")));
	Skeleton->Modify();

	TArray<FString> Removed;
	TArray<FString> NotFound;
	if (BoneNames.Num() == 0)
	{
		TArray<FName> AllVBNames;
		for (const FVirtualBone& VB : Skeleton->GetVirtualBones())
		{
			AllVBNames.Add(VB.VirtualBoneName);
		}
		if (AllVBNames.Num() > 0)
		{
			Skeleton->RemoveVirtualBones(AllVBNames);
		}
		Removed.Add(TEXT("all"));
	}
	else
	{
		for (const FString& BoneName : BoneNames)
		{
			bool bFound = false;
			for (const FVirtualBone& VB : Skeleton->GetVirtualBones())
			{
				if (VB.VirtualBoneName == FName(*BoneName))
				{
					bFound = true;
					break;
				}
			}
			if (bFound)
			{
				Skeleton->RemoveVirtualBones({FName(*BoneName)});
				Removed.Add(BoneName);
			}
			else
			{
				NotFound.Add(BoneName);
			}
		}

		if (Removed.Num() == 0 && NotFound.Num() > 0)
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(TEXT("No virtual bones found matching the given names"));
		}
	}

	GEditor->EndTransaction();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> RemovedArr;
	for (const FString& R : Removed)
		RemovedArr.Add(MakeShared<FJsonValueString>(R));
	Root->SetArrayField(TEXT("removed"), RemovedArr);

	if (NotFound.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> NotFoundArr;
		for (const FString& NF : NotFound)
			NotFoundArr.Add(MakeShared<FJsonValueString>(NF));
		Root->SetArrayField(TEXT("not_found"), NotFoundArr);
	}

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Skeleton Info
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleGetSkeletonInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(AssetPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *AssetPath));

	const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
	int32 BoneCount = RefSkel.GetNum();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("bone_count"), BoneCount);

	TArray<TSharedPtr<FJsonValue>> BonesArr;
	for (int32 i = 0; i < BoneCount; ++i)
	{
		TSharedPtr<FJsonObject> BoneObj = MakeShared<FJsonObject>();
		BoneObj->SetNumberField(TEXT("index"), i);
		BoneObj->SetStringField(TEXT("name"), RefSkel.GetBoneName(i).ToString());
		BoneObj->SetNumberField(TEXT("parent_index"), RefSkel.GetParentIndex(i));
		if (RefSkel.GetParentIndex(i) != INDEX_NONE)
			BoneObj->SetStringField(TEXT("parent_name"), RefSkel.GetBoneName(RefSkel.GetParentIndex(i)).ToString());
		BonesArr.Add(MakeShared<FJsonValueObject>(BoneObj));
	}
	Root->SetArrayField(TEXT("bones"), BonesArr);

	const TArray<FVirtualBone>& VBones = Skeleton->GetVirtualBones();
	TArray<TSharedPtr<FJsonValue>> VBonesArr;
	for (const FVirtualBone& VB : VBones)
	{
		TSharedPtr<FJsonObject> VBObj = MakeShared<FJsonObject>();
		VBObj->SetStringField(TEXT("name"), VB.VirtualBoneName.ToString());
		VBObj->SetStringField(TEXT("source"), VB.SourceBoneName.ToString());
		VBObj->SetStringField(TEXT("target"), VB.TargetBoneName.ToString());
		VBonesArr.Add(MakeShared<FJsonValueObject>(VBObj));
	}
	Root->SetArrayField(TEXT("virtual_bones"), VBonesArr);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetSkeletalMeshInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	USkeletalMesh* Mesh = FMonolithAssetUtils::LoadAssetByPath<USkeletalMesh>(AssetPath);
	if (!Mesh) return FMonolithActionResult::Error(FString::Printf(TEXT("SkeletalMesh not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	USkeleton* Skel = Mesh->GetSkeleton();
	Root->SetStringField(TEXT("skeleton"), Skel ? Skel->GetPathName() : TEXT("None"));

	// Morph targets
	TArray<TSharedPtr<FJsonValue>> MorphArr;
	TArray<FString> MorphNames = Mesh->K2_GetAllMorphTargetNames();
	for (const FString& MorphName : MorphNames)
	{
		MorphArr.Add(MakeShared<FJsonValueString>(MorphName));
	}
	Root->SetArrayField(TEXT("morph_targets"), MorphArr);
	Root->SetNumberField(TEXT("morph_target_count"), MorphArr.Num());

	// Sockets
	TArray<TSharedPtr<FJsonValue>> SocketArr;
	for (int32 i = 0; i < Mesh->NumSockets(); ++i)
	{
		USkeletalMeshSocket* Sock = Mesh->GetSocketByIndex(i);
		if (!Sock) continue;
		TSharedPtr<FJsonObject> SockObj = MakeShared<FJsonObject>();
		SockObj->SetStringField(TEXT("name"), Sock->SocketName.ToString());
		SockObj->SetStringField(TEXT("bone"), Sock->BoneName.ToString());
		SocketArr.Add(MakeShared<FJsonValueObject>(SockObj));
	}
	Root->SetArrayField(TEXT("sockets"), SocketArr);
	Root->SetNumberField(TEXT("socket_count"), SocketArr.Num());

	// LOD count
	int32 LODCount = 0;
	if (FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering())
	{
		LODCount = RenderData->LODRenderData.Num();
	}
	Root->SetNumberField(TEXT("lod_count"), LODCount);

	// Materials
	TArray<TSharedPtr<FJsonValue>> MatArr;
	for (int32 i = 0; i < Mesh->GetMaterials().Num(); ++i)
	{
		const FSkeletalMaterial& MatSlot = Mesh->GetMaterials()[i];
		TSharedPtr<FJsonObject> MatObj = MakeShared<FJsonObject>();
		MatObj->SetNumberField(TEXT("index"), i);
		MatObj->SetStringField(TEXT("name"), MatSlot.MaterialSlotName.ToString());
		MatObj->SetStringField(TEXT("material"), MatSlot.MaterialInterface ? MatSlot.MaterialInterface->GetPathName() : TEXT("None"));
		MatArr.Add(MakeShared<FJsonValueObject>(MatObj));
	}
	Root->SetArrayField(TEXT("materials"), MatArr);
	Root->SetNumberField(TEXT("material_count"), MatArr.Num());

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// ABP Graph Reading
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleGetStateMachines(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	TArray<TSharedPtr<FJsonValue>> MachinesArr;

	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node);
			if (!SMNode) continue;

			UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
			if (!SMGraph) continue;

			TSharedPtr<FJsonObject> MachineObj = MakeShared<FJsonObject>();
			FString SMName = SMNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			int32 NewlineIdx = INDEX_NONE;
			if (SMName.FindChar(TEXT('\n'), NewlineIdx))
			{
				SMName.LeftInline(NewlineIdx);
			}
			MachineObj->SetStringField(TEXT("name"), SMName);
			MachineObj->SetStringField(TEXT("graph"), Graph->GetName());

			if (SMGraph->EntryNode)
			{
				for (UEdGraphPin* Pin : SMGraph->EntryNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
					{
						UAnimStateNode* EntryState = Cast<UAnimStateNode>(Pin->LinkedTo[0]->GetOwningNode());
						if (EntryState)
						{
							MachineObj->SetStringField(TEXT("entry_state"), EntryState->GetStateName());
						}
					}
				}
			}

			TArray<TSharedPtr<FJsonValue>> StatesArr;
			TArray<TSharedPtr<FJsonValue>> TransitionsArr;

			for (UEdGraphNode* SMChildNode : SMGraph->Nodes)
			{
				if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(SMChildNode))
				{
					TSharedPtr<FJsonObject> StateObj = MakeShared<FJsonObject>();
					StateObj->SetStringField(TEXT("name"), StateNode->GetStateName());
					TArray<TSharedPtr<FJsonValue>> PosArr;
					PosArr.Add(MakeShared<FJsonValueNumber>(StateNode->NodePosX));
					PosArr.Add(MakeShared<FJsonValueNumber>(StateNode->NodePosY));
					StateObj->SetArrayField(TEXT("position"), PosArr);
					StatesArr.Add(MakeShared<FJsonValueObject>(StateObj));
				}
				else if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(SMChildNode))
				{
					TSharedPtr<FJsonObject> TransObj = MakeShared<FJsonObject>();
					UAnimStateNodeBase* PrevState = TransNode->GetPreviousState();
					UAnimStateNodeBase* NextState = TransNode->GetNextState();
					TransObj->SetStringField(TEXT("from"), PrevState ? PrevState->GetStateName() : TEXT("?"));
					TransObj->SetStringField(TEXT("to"), NextState ? NextState->GetStateName() : TEXT("?"));
					TransObj->SetStringField(TEXT("from_type"), PrevState ? (Cast<UAnimStateConduitNode>(PrevState) ? TEXT("conduit") : TEXT("state")) : TEXT("unknown"));
					TransObj->SetStringField(TEXT("to_type"), NextState ? (Cast<UAnimStateConduitNode>(NextState) ? TEXT("conduit") : TEXT("state")) : TEXT("unknown"));
					TransObj->SetNumberField(TEXT("cross_fade_duration"), TransNode->CrossfadeDuration);
					TransObj->SetStringField(TEXT("blend_mode"),
						TransNode->BlendMode == EAlphaBlendOption::Linear ? TEXT("Linear") :
						TransNode->BlendMode == EAlphaBlendOption::Cubic ? TEXT("Cubic") : TEXT("Other"));
					TransObj->SetBoolField(TEXT("bidirectional"), TransNode->Bidirectional);
					TransitionsArr.Add(MakeShared<FJsonValueObject>(TransObj));
				}
			}

			MachineObj->SetArrayField(TEXT("states"), StatesArr);
			MachineObj->SetArrayField(TEXT("transitions"), TransitionsArr);
			MachineObj->SetNumberField(TEXT("state_count"), StatesArr.Num());
			MachineObj->SetNumberField(TEXT("transition_count"), TransitionsArr.Num());
			MachinesArr.Add(MakeShared<FJsonValueObject>(MachineObj));
		}
	}

	Root->SetArrayField(TEXT("state_machines"), MachinesArr);
	Root->SetNumberField(TEXT("count"), MachinesArr.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetStateInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString MachineName = Params->GetStringField(TEXT("machine_name"));
	FString StateName = Params->GetStringField(TEXT("state_name"));

	if (MachineName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: machine_name"));
	}
	if (StateName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: state_name"));
	}

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node);
			if (!SMNode) continue;

			FString SMTitle = SMNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			int32 NewlineIdx = INDEX_NONE;
			if (SMTitle.FindChar(TEXT('\n'), NewlineIdx))
			{
				SMTitle.LeftInline(NewlineIdx);
			}
			if (SMTitle != MachineName) continue;

			UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
			if (!SMGraph) continue;

			for (UEdGraphNode* SMChildNode : SMGraph->Nodes)
			{
				UAnimStateNode* StateNode = Cast<UAnimStateNode>(SMChildNode);
				if (!StateNode || StateNode->GetStateName() != StateName) continue;

				TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
				Root->SetStringField(TEXT("state_name"), StateName);
				Root->SetStringField(TEXT("machine_name"), MachineName);

				TArray<TSharedPtr<FJsonValue>> PosArr;
				PosArr.Add(MakeShared<FJsonValueNumber>(StateNode->NodePosX));
				PosArr.Add(MakeShared<FJsonValueNumber>(StateNode->NodePosY));
				Root->SetArrayField(TEXT("position"), PosArr);

				UEdGraph* StateGraph = StateNode->GetBoundGraph();
				TArray<TSharedPtr<FJsonValue>> NodesArr;
				if (StateGraph)
				{
					for (UEdGraphNode* InnerNode : StateGraph->Nodes)
					{
						UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(InnerNode);
						if (!AnimNode) continue;
						TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
						NodeObj->SetStringField(TEXT("class"), AnimNode->GetClass()->GetName());
						NodeObj->SetStringField(TEXT("title"), AnimNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
						NodesArr.Add(MakeShared<FJsonValueObject>(NodeObj));
					}
				}
				Root->SetArrayField(TEXT("nodes"), NodesArr);
				Root->SetNumberField(TEXT("node_count"), NodesArr.Num());

				return FMonolithActionResult::Success(Root);
			}
		}
	}

	return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found in machine '%s'"), *StateName, *MachineName));
}

FMonolithActionResult FMonolithAnimationActions::HandleGetTransitions(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString MachineName = Params->GetStringField(TEXT("machine_name"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	// Helper lambda to collect transitions from a state machine graph
	auto CollectTransitions = [](UAnimationStateMachineGraph* SMGraph, TArray<TSharedPtr<FJsonValue>>& OutArr)
	{
		for (UEdGraphNode* SMChildNode : SMGraph->Nodes)
		{
			UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(SMChildNode);
			if (!TransNode) continue;

			TSharedPtr<FJsonObject> TransObj = MakeShared<FJsonObject>();
			UAnimStateNodeBase* PrevState = TransNode->GetPreviousState();
			UAnimStateNodeBase* NextState = TransNode->GetNextState();
			TransObj->SetStringField(TEXT("from"), PrevState ? PrevState->GetStateName() : TEXT("?"));
			TransObj->SetStringField(TEXT("to"), NextState ? NextState->GetStateName() : TEXT("?"));
			TransObj->SetStringField(TEXT("from_type"), PrevState ? (Cast<UAnimStateConduitNode>(PrevState) ? TEXT("conduit") : TEXT("state")) : TEXT("unknown"));
			TransObj->SetStringField(TEXT("to_type"), NextState ? (Cast<UAnimStateConduitNode>(NextState) ? TEXT("conduit") : TEXT("state")) : TEXT("unknown"));
			TransObj->SetNumberField(TEXT("cross_fade_duration"), TransNode->CrossfadeDuration);
			TransObj->SetStringField(TEXT("blend_mode"),
				TransNode->BlendMode == EAlphaBlendOption::Linear ? TEXT("Linear") :
				TransNode->BlendMode == EAlphaBlendOption::Cubic ? TEXT("Cubic") : TEXT("Other"));
			TransObj->SetBoolField(TEXT("bidirectional"), TransNode->Bidirectional);

			UEdGraph* RuleGraph = TransNode->GetBoundGraph();
			TArray<TSharedPtr<FJsonValue>> RuleNodesArr;
			if (RuleGraph)
			{
				for (UEdGraphNode* RuleNode : RuleGraph->Nodes)
				{
					TSharedPtr<FJsonObject> RuleObj = MakeShared<FJsonObject>();
					RuleObj->SetStringField(TEXT("class"), RuleNode->GetClass()->GetName());
					RuleObj->SetStringField(TEXT("title"), RuleNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
					RuleNodesArr.Add(MakeShared<FJsonValueObject>(RuleObj));
				}
			}
			TransObj->SetArrayField(TEXT("rule_nodes"), RuleNodesArr);
			OutArr.Add(MakeShared<FJsonValueObject>(TransObj));
		}
	};

	// If machine_name is empty, return transitions from ALL state machines
	bool bMatchAll = MachineName.IsEmpty();
	TArray<TSharedPtr<FJsonValue>> AllTransitionsArr;
	bool bFoundAny = false;

	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node);
			if (!SMNode) continue;

			FString SMTitle = SMNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			int32 NewlineIdx = INDEX_NONE;
			if (SMTitle.FindChar(TEXT('\n'), NewlineIdx))
			{
				SMTitle.LeftInline(NewlineIdx);
			}
			if (!bMatchAll && SMTitle != MachineName) continue;

			UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
			if (!SMGraph) continue;

			bFoundAny = true;

			if (!bMatchAll)
			{
				// Specific machine match — return immediately
				TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
				Root->SetStringField(TEXT("machine_name"), MachineName);
				TArray<TSharedPtr<FJsonValue>> TransitionsArr;
				CollectTransitions(SMGraph, TransitionsArr);
				Root->SetArrayField(TEXT("transitions"), TransitionsArr);
				Root->SetNumberField(TEXT("count"), TransitionsArr.Num());
				return FMonolithActionResult::Success(Root);
			}

			// Collect from all machines
			CollectTransitions(SMGraph, AllTransitionsArr);
		}
	}

	// Return collected results (may be empty)
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("machine_name"), MachineName);
	Result->SetArrayField(TEXT("transitions"), AllTransitionsArr);
	Result->SetNumberField(TEXT("count"), AllTransitionsArr.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetBlendNodes(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (!Graph) continue;
		if (!GraphName.IsEmpty() && Graph->GetName() != GraphName) continue;

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("graph_name"), Graph->GetName());
		TArray<TSharedPtr<FJsonValue>> NodesArr;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
			if (!AnimNode) continue;

			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("class"), AnimNode->GetClass()->GetName());
			NodeObj->SetStringField(TEXT("title"), AnimNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

			TArray<TSharedPtr<FJsonValue>> PinsArr;
			for (UEdGraphPin* Pin : AnimNode->Pins)
			{
				if (!Pin || Pin->LinkedTo.Num() == 0) continue;
				TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
				PinObj->SetStringField(TEXT("name"), Pin->GetName());
				PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
				PinObj->SetNumberField(TEXT("connections"), Pin->LinkedTo.Num());
				PinsArr.Add(MakeShared<FJsonValueObject>(PinObj));
			}
			NodeObj->SetArrayField(TEXT("connected_pins"), PinsArr);
			NodesArr.Add(MakeShared<FJsonValueObject>(NodeObj));
		}

		Root->SetArrayField(TEXT("nodes"), NodesArr);
		Root->SetNumberField(TEXT("count"), NodesArr.Num());
		return FMonolithActionResult::Success(Root);
	}

	return FMonolithActionResult::Error(FString::Printf(TEXT("Graph '%s' not found"), *GraphName));
}

FMonolithActionResult FMonolithAnimationActions::HandleGetGraphs(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	TArray<TSharedPtr<FJsonValue>> GraphsArr;

	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (!Graph) continue;

		TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
		GraphObj->SetStringField(TEXT("name"), Graph->GetName());
		GraphObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());

		int32 AnimNodeCount = 0;
		int32 StateMachineCount = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Cast<UAnimGraphNode_StateMachine>(Node)) StateMachineCount++;
			if (Cast<UAnimGraphNode_Base>(Node)) AnimNodeCount++;
		}
		GraphObj->SetNumberField(TEXT("anim_node_count"), AnimNodeCount);
		GraphObj->SetNumberField(TEXT("state_machine_count"), StateMachineCount);
		GraphsArr.Add(MakeShared<FJsonValueObject>(GraphObj));
	}

	Root->SetArrayField(TEXT("graphs"), GraphsArr);
	Root->SetNumberField(TEXT("count"), GraphsArr.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetNodes(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString NodeClassFilter;
	Params->TryGetStringField(TEXT("node_class_filter"), NodeClassFilter);
	FString GraphFilter;
	Params->TryGetStringField(TEXT("graph_name"), GraphFilter);
	bool bIncludeAnimGraph = false;
	Params->TryGetBoolField(TEXT("include_anim_graph"), bIncludeAnimGraph);

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	if (!NodeClassFilter.IsEmpty())
	{
		Root->SetStringField(TEXT("filter_class"), NodeClassFilter);
	}
	if (!GraphFilter.IsEmpty())
	{
		Root->SetStringField(TEXT("graph_name"), GraphFilter);
	}
	TArray<TSharedPtr<FJsonValue>> NodesArr;

	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (!Graph) continue;
		if (!GraphFilter.IsEmpty() && Graph->GetName() != GraphFilter) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
			if (!AnimNode) continue;

			FString ClassName = AnimNode->GetClass()->GetName();
			if (!NodeClassFilter.IsEmpty() && !ClassName.Contains(NodeClassFilter)) continue;

			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("class"), ClassName);
			NodeObj->SetStringField(TEXT("name"), AnimNode->GetName());
			NodeObj->SetStringField(TEXT("title"), AnimNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
			NodeObj->SetStringField(TEXT("graph"), Graph->GetName());

			TArray<TSharedPtr<FJsonValue>> PinsArr;
			for (UEdGraphPin* Pin : AnimNode->Pins)
			{
				if (!Pin || Pin->LinkedTo.Num() == 0) continue;
				TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
				PinObj->SetStringField(TEXT("name"), Pin->GetName());
				PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
				PinObj->SetNumberField(TEXT("connections"), Pin->LinkedTo.Num());
				PinsArr.Add(MakeShared<FJsonValueObject>(PinObj));
			}
			NodeObj->SetArrayField(TEXT("connected_pins"), PinsArr);

			// Gap 2 — compact function-binding block { initial_update/become_relevant/update:
			// name-or-null }, omitted when all three are empty. Gap 12 — compact pin-binding
			// list [{pin, path}], omitted when empty. Both keep the payload lean.
			MonolithAnimNodeBindingReader::SerializeCompactFunctionBindings(AnimNode, NodeObj);
			MonolithAnimNodeBindingReader::SerializeCompactPinBindings(AnimNode, NodeObj);

			NodesArr.Add(MakeShared<FJsonValueObject>(NodeObj));
		}
	}

	// Optional: also traverse the main AnimGraph (and all other graphs) for EvaluateChooser
	// K2Nodes — these are NOT UAnimGraphNode_Base subclasses, so the loop above skips them.
	// Surfacing them with their reflectively-resolved chooser asset is the additive behavior
	// gated behind include_anim_graph (default off, so the existing output shape is unchanged).
	if (bIncludeAnimGraph)
	{
		TArray<UEdGraph*> AllGraphs;
		ABP->GetAllGraphs(AllGraphs); // anim graph + function graphs + ubergraph pages + sub-graphs
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			if (!GraphFilter.IsEmpty() && Graph->GetName() != GraphFilter) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;

				// EvaluateChooser and PropertyAccess K2Nodes are NOT UAnimGraphNode_Base
				// subclasses, so the primary loop skips them. Surface both here: the chooser
				// node carries a reflectively-resolved chooser asset; the PropertyAccess node
				// carries a reflectively-resolved property-access path (Gap 1).
				const bool bIsChooser = MonolithAnimGraphChooser::IsEvaluateChooserNode(Node);
				const bool bIsPropertyAccess = (Node->GetClass()->GetName() == TEXT("K2Node_PropertyAccess"));
				if (!bIsChooser && !bIsPropertyAccess) continue;

				FString ClassName = Node->GetClass()->GetName();
				if (!NodeClassFilter.IsEmpty() && !ClassName.Contains(NodeClassFilter)) continue;

				TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
				NodeObj->SetStringField(TEXT("class"), ClassName);
				NodeObj->SetStringField(TEXT("name"), Node->GetName());
				NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
				NodeObj->SetStringField(TEXT("graph"), Graph->GetName());
				NodeObj->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());

				if (bIsChooser)
				{
					// Reflectively resolve the private `Chooser` UPROPERTY (see file-header comment).
					MonolithAnimGraphChooser::ResolveChooserAsset(Node, NodeObj);
				}
				if (bIsPropertyAccess)
				{
					// Reflectively resolve the private PropertyAccess path (shared helper, Gap 1).
					MonolithPropertyAccessReader::SerializePropertyAccessBlock(Node, NodeObj);
				}

				TArray<TSharedPtr<FJsonValue>> PinsArr;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin || Pin->LinkedTo.Num() == 0) continue;
					TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
					PinObj->SetStringField(TEXT("name"), Pin->GetName());
					PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
					PinObj->SetNumberField(TEXT("connections"), Pin->LinkedTo.Num());
					PinsArr.Add(MakeShared<FJsonValueObject>(PinObj));
				}
				NodeObj->SetArrayField(TEXT("connected_pins"), PinsArr);
				NodesArr.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
		}
	}

	Root->SetArrayField(TEXT("nodes"), NodesArr);
	Root->SetNumberField(TEXT("count"), NodesArr.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetAnimGraphChoosers(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	bool bRecursive = false;
	Params->TryGetBoolField(TEXT("recursive"), bRecursive);

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetBoolField(TEXT("recursive"), bRecursive);
	TArray<TSharedPtr<FJsonValue>> ChoosersArr;

	// Walk EVERY graph in the AnimBP (main AnimGraph, function graphs, ubergraph pages,
	// nested sub-graphs) — an EvaluateChooser K2Node can live in any of them.
	TArray<UEdGraph*> AllGraphs;
	ABP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			// Prefix match catches BOTH the deprecated v1 and the modern v2 node — see the
			// file-header comment on MonolithAnimGraphChooser for the v1/v2 rationale.
			if (!MonolithAnimGraphChooser::IsEvaluateChooserNode(Node)) continue;

			TSharedPtr<FJsonObject> ChooserObj = MakeShared<FJsonObject>();
			ChooserObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
			ChooserObj->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
			ChooserObj->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
			ChooserObj->SetStringField(TEXT("graph"), Graph->GetName());

			// Reflectively resolve the private `Chooser` UPROPERTY (see file-header comment for
			// why this is deliberately a reflective read, never a hard cast / Private #include).
			UObject* ChooserAsset = MonolithAnimGraphChooser::ResolveChooserAsset(Node, ChooserObj);

			// Surface the node's output-pin link endpoints (chooser nodes feed downstream nodes).
			TArray<TSharedPtr<FJsonValue>> OutputLinksArr;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (!Linked || !Linked->GetOwningNodeUnchecked()) continue;
					TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
					LinkObj->SetStringField(TEXT("from_pin"), Pin->GetName());
					LinkObj->SetStringField(TEXT("to_node"), Linked->GetOwningNode()->GetName());
					LinkObj->SetStringField(TEXT("to_pin"), Linked->GetName());
					OutputLinksArr.Add(MakeShared<FJsonValueObject>(LinkObj));
				}
			}
			ChooserObj->SetArrayField(TEXT("output_pin_links"), OutputLinksArr);

			// Optional recursive expansion of the referenced chooser's nested tree, via the
			// Phase-2 shared collector. Two gates: WITH_CHOOSER (the UChooserTable type + the
			// collector) AND WITH_EDITORONLY_DATA (the chooser ROW DATA the collector walks).
			// Outside those configs (cooked/release) the expansion is simply skipped — the node
			// + resolved asset path still surface, keeping the action release-build-safe.
			if (bRecursive && ChooserAsset)
			{
#if WITH_CHOOSER && WITH_EDITORONLY_DATA
				if (UChooserTable* Table = Cast<UChooserTable>(ChooserAsset))
				{
					// Supply our own visited-set — REQUIRED by the collector's recursion-entry
					// contract; it forces the cycle guard so reuse here cannot loop forever.
					TSet<UChooserTable*> VisitedTables;
					TSharedPtr<FJsonObject> Tree = MonolithChooserTree::CollectTree(Table, VisitedTables);
					if (Tree.IsValid())
					{
						ChooserObj->SetObjectField(TEXT("chooser_tree"), Tree);
					}
				}
#endif // WITH_CHOOSER && WITH_EDITORONLY_DATA
			}

			ChoosersArr.Add(MakeShared<FJsonValueObject>(ChooserObj));
		}
	}

	Root->SetArrayField(TEXT("choosers"), ChoosersArr);
	Root->SetNumberField(TEXT("count"), ChoosersArr.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetLinkedLayers(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	TArray<TSharedPtr<FJsonValue>> LayersArr;

	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_LinkedAnimLayer* LayerNode = Cast<UAnimGraphNode_LinkedAnimLayer>(Node);
			if (!LayerNode) continue;

			TSharedPtr<FJsonObject> LayerObj = MakeShared<FJsonObject>();
			LayerObj->SetStringField(TEXT("title"), LayerNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
			LayerObj->SetStringField(TEXT("graph"), Graph->GetName());
			LayerObj->SetStringField(TEXT("class"), LayerNode->GetClass()->GetName());
			LayersArr.Add(MakeShared<FJsonValueObject>(LayerObj));
		}
	}

	Root->SetArrayField(TEXT("linked_layers"), LayersArr);
	Root->SetNumberField(TEXT("count"), LayersArr.Num());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Wave 1 — Read Actions
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleGetSequenceInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	USkeleton* Skel = Seq->GetSkeleton();
	Root->SetStringField(TEXT("skeleton"), Skel ? Skel->GetPathName() : TEXT("None"));

	Root->SetNumberField(TEXT("duration"), Seq->GetPlayLength());

	const IAnimationDataModel* DataModel = Seq->GetDataModel();
	if (DataModel)
	{
		Root->SetNumberField(TEXT("num_frames"), DataModel->GetNumberOfFrames());
		Root->SetNumberField(TEXT("num_keys"), DataModel->GetNumberOfKeys());
	}

	FFrameRate SampleRate = Seq->GetSamplingFrameRate();
	Root->SetNumberField(TEXT("sample_rate"), SampleRate.AsDecimal());
	Root->SetStringField(TEXT("frame_rate"), FString::Printf(TEXT("%d/%d"), SampleRate.Numerator, SampleRate.Denominator));

	Root->SetBoolField(TEXT("has_root_motion"), Seq->bEnableRootMotion);
	Root->SetBoolField(TEXT("force_root_lock"), Seq->bForceRootLock);

	FString RootMotionLockStr;
	switch (Seq->RootMotionRootLock.GetValue())
	{
	case ERootMotionRootLock::RefPose: RootMotionLockStr = TEXT("RefPose"); break;
	case ERootMotionRootLock::AnimFirstFrame: RootMotionLockStr = TEXT("AnimFirstFrame"); break;
	case ERootMotionRootLock::Zero: RootMotionLockStr = TEXT("Zero"); break;
	default: RootMotionLockStr = TEXT("Unknown"); break;
	}
	Root->SetStringField(TEXT("root_motion_lock"), RootMotionLockStr);

	FString AdditiveStr;
	switch (Seq->AdditiveAnimType.GetValue())
	{
	case EAdditiveAnimationType::AAT_None: AdditiveStr = TEXT("None"); break;
	case EAdditiveAnimationType::AAT_LocalSpaceBase: AdditiveStr = TEXT("LocalSpaceBase"); break;
	case EAdditiveAnimationType::AAT_RotationOffsetMeshSpace: AdditiveStr = TEXT("RotationOffsetMeshSpace"); break;
	default: AdditiveStr = TEXT("Unknown"); break;
	}
	Root->SetStringField(TEXT("additive_type"), AdditiveStr);

	Root->SetStringField(TEXT("interpolation"),
		Seq->Interpolation == EAnimInterpolationType::Linear ? TEXT("Linear") : TEXT("Step"));

	Root->SetNumberField(TEXT("rate_scale"), Seq->RateScale);
	Root->SetBoolField(TEXT("is_looping"), Seq->bLoop);

	// Compression scheme - use CurveCompressionCodec as a proxy if available
	Root->SetStringField(TEXT("compression_scheme"), TEXT("Default"));

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetSequenceNotifies(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimSequenceBase* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequenceBase>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("Animation asset not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	TArray<TSharedPtr<FJsonValue>> NotifiesArr;
	for (int32 i = 0; i < Seq->Notifies.Num(); ++i)
	{
		const FAnimNotifyEvent& Event = Seq->Notifies[i];
		TSharedPtr<FJsonObject> NotifyObj = MakeShared<FJsonObject>();

		NotifyObj->SetNumberField(TEXT("index"), i);
		NotifyObj->SetStringField(TEXT("name"), Event.NotifyName.ToString());
		NotifyObj->SetNumberField(TEXT("time"), Event.GetTime());
		NotifyObj->SetNumberField(TEXT("duration"), Event.GetDuration());
		NotifyObj->SetNumberField(TEXT("trigger_time_offset"), Event.TriggerTimeOffset);
		NotifyObj->SetStringField(TEXT("notify_class"),
			Event.Notify ? Event.Notify->GetClass()->GetName() : TEXT(""));
		NotifyObj->SetStringField(TEXT("state_class"),
			Event.NotifyStateClass ? Event.NotifyStateClass->GetClass()->GetName() : TEXT(""));
		NotifyObj->SetNumberField(TEXT("track_index"), Event.TrackIndex);

		// Get track name if valid
		FString TrackName;
		if (Seq->AnimNotifyTracks.IsValidIndex(Event.TrackIndex))
		{
			TrackName = Seq->AnimNotifyTracks[Event.TrackIndex].TrackName.ToString();
		}
		NotifyObj->SetStringField(TEXT("track_name"), TrackName);
		NotifyObj->SetBoolField(TEXT("is_state"), Event.NotifyStateClass != nullptr);

		NotifiesArr.Add(MakeShared<FJsonValueObject>(NotifyObj));
	}

	Root->SetArrayField(TEXT("notifies"), NotifiesArr);
	Root->SetNumberField(TEXT("count"), NotifiesArr.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetBoneTrackKeys(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString BoneName = Params->GetStringField(TEXT("bone_name"));
	int32 StartFrame = 0;
	int32 EndFrame = -1;

	double TempVal;
	if (Params->TryGetNumberField(TEXT("start_frame"), TempVal))
		StartFrame = static_cast<int32>(TempVal);
	if (Params->TryGetNumberField(TEXT("end_frame"), TempVal))
		EndFrame = static_cast<int32>(TempVal);

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	const IAnimationDataModel* DataModel = Seq->GetDataModel();
	if (!DataModel) return FMonolithActionResult::Error(TEXT("No animation data model"));

	const FName BoneFName(*BoneName);
	if (!DataModel->IsValidBoneTrackName(BoneFName))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Bone track not found: %s"), *BoneName));

	// Use non-deprecated API: evaluate the bone track at every keyframe via
	// GetBoneTrackTransforms. Works regardless of underlying compressed storage.
	TArray<FTransform> AllTransforms;
	DataModel->GetBoneTrackTransforms(BoneFName, AllTransforms);
	const int32 MaxKeys = AllTransforms.Num();

	if (EndFrame < 0 || EndFrame >= MaxKeys)
		EndFrame = MaxKeys - 1;
	if (StartFrame < 0) StartFrame = 0;
	if (MaxKeys == 0)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Bone track has no keys: %s"), *BoneName));
	if (StartFrame > EndFrame)
		return FMonolithActionResult::Error(FString::Printf(TEXT("start_frame (%d) > end_frame (%d)"), StartFrame, EndFrame));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("bone_name"), BoneName);
	Root->SetNumberField(TEXT("num_keys"), MaxKeys);

	TArray<TSharedPtr<FJsonValue>> PosArr;
	TArray<TSharedPtr<FJsonValue>> RotArr;
	TArray<TSharedPtr<FJsonValue>> ScaleArr;
	for (int32 i = StartFrame; i <= EndFrame; ++i)
	{
		const FTransform& Xf = AllTransforms[i];
		const FVector& Pos = Xf.GetLocation();
		const FQuat& Rot = Xf.GetRotation();
		const FVector& Scl = Xf.GetScale3D();

		TArray<TSharedPtr<FJsonValue>> P;
		P.Add(MakeShared<FJsonValueNumber>(Pos.X));
		P.Add(MakeShared<FJsonValueNumber>(Pos.Y));
		P.Add(MakeShared<FJsonValueNumber>(Pos.Z));
		PosArr.Add(MakeShared<FJsonValueArray>(P));

		TArray<TSharedPtr<FJsonValue>> Q;
		Q.Add(MakeShared<FJsonValueNumber>(Rot.X));
		Q.Add(MakeShared<FJsonValueNumber>(Rot.Y));
		Q.Add(MakeShared<FJsonValueNumber>(Rot.Z));
		Q.Add(MakeShared<FJsonValueNumber>(Rot.W));
		RotArr.Add(MakeShared<FJsonValueArray>(Q));

		TArray<TSharedPtr<FJsonValue>> S;
		S.Add(MakeShared<FJsonValueNumber>(Scl.X));
		S.Add(MakeShared<FJsonValueNumber>(Scl.Y));
		S.Add(MakeShared<FJsonValueNumber>(Scl.Z));
		ScaleArr.Add(MakeShared<FJsonValueArray>(S));
	}
	Root->SetArrayField(TEXT("positions"), PosArr);
	Root->SetArrayField(TEXT("rotations"), RotArr);
	Root->SetArrayField(TEXT("scales"), ScaleArr);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleListBoneTracks(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	const IAnimationDataModel* DataModel = Seq->GetDataModel();
	if (!DataModel) return FMonolithActionResult::Error(TEXT("No animation data model"));

	TArray<FName> BoneNames;
	DataModel->GetBoneTrackNames(BoneNames);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("count"), BoneNames.Num());
	TArray<TSharedPtr<FJsonValue>> NameArr;
	for (const FName& N : BoneNames)
	{
		NameArr.Add(MakeShared<FJsonValueString>(N.ToString()));
	}
	Root->SetArrayField(TEXT("bone_names"), NameArr);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetSequenceCurves(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	const IAnimationDataModel* DataModel = Seq->GetDataModel();
	if (!DataModel) return FMonolithActionResult::Error(TEXT("No animation data model"));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	TArray<TSharedPtr<FJsonValue>> CurvesArr;

	const FAnimationCurveData& CurveData = DataModel->GetCurveData();

	// Float curves
	for (const FFloatCurve& Curve : CurveData.FloatCurves)
	{
		TSharedPtr<FJsonObject> CurveObj = MakeShared<FJsonObject>();
		CurveObj->SetStringField(TEXT("name"), Curve.GetName().ToString());
		CurveObj->SetStringField(TEXT("type"), TEXT("Float"));
		CurveObj->SetNumberField(TEXT("num_keys"), Curve.FloatCurve.GetNumKeys());
		CurvesArr.Add(MakeShared<FJsonValueObject>(CurveObj));
	}

	// Transform curves
	for (const FTransformCurve& Curve : CurveData.TransformCurves)
	{
		TSharedPtr<FJsonObject> CurveObj = MakeShared<FJsonObject>();
		CurveObj->SetStringField(TEXT("name"), Curve.GetName().ToString());
		CurveObj->SetStringField(TEXT("type"), TEXT("Transform"));
		CurveObj->SetNumberField(TEXT("num_keys"), 0); // Transform curves don't have a simple key count
		CurvesArr.Add(MakeShared<FJsonValueObject>(CurveObj));
	}

	Root->SetArrayField(TEXT("curves"), CurvesArr);
	Root->SetNumberField(TEXT("count"), CurvesArr.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetMontageInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimMontage* Montage = FMonolithAssetUtils::LoadAssetByPath<UAnimMontage>(AssetPath);
	if (!Montage) return FMonolithActionResult::Error(FString::Printf(TEXT("Montage not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	USkeleton* Skel = Montage->GetSkeleton();
	Root->SetStringField(TEXT("skeleton"), Skel ? Skel->GetPathName() : TEXT("None"));
	Root->SetNumberField(TEXT("duration"), Montage->GetPlayLength());
	Root->SetNumberField(TEXT("rate_scale"), Montage->RateScale);
	Root->SetNumberField(TEXT("blend_in_time"), Montage->BlendIn.GetBlendTime());
	Root->SetNumberField(TEXT("blend_out_time"), Montage->BlendOut.GetBlendTime());
	Root->SetNumberField(TEXT("blend_out_trigger_time"), Montage->BlendOutTriggerTime);
	Root->SetBoolField(TEXT("enable_auto_blend_out"), Montage->bEnableAutoBlendOut);

	// Sections
	TArray<TSharedPtr<FJsonValue>> SectionsArr;
	for (int32 i = 0; i < Montage->CompositeSections.Num(); ++i)
	{
		const FCompositeSection& Section = Montage->CompositeSections[i];
		TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetNumberField(TEXT("index"), i);
		SectionObj->SetStringField(TEXT("name"), Section.SectionName.ToString());
		SectionObj->SetNumberField(TEXT("time"), Section.GetTime());
		SectionObj->SetStringField(TEXT("next_section"), Section.NextSectionName.ToString());
		SectionsArr.Add(MakeShared<FJsonValueObject>(SectionObj));
	}
	Root->SetArrayField(TEXT("sections"), SectionsArr);

	// Slots
	TArray<TSharedPtr<FJsonValue>> SlotsArr;
	for (int32 i = 0; i < Montage->SlotAnimTracks.Num(); ++i)
	{
		TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
		SlotObj->SetNumberField(TEXT("index"), i);
		SlotObj->SetStringField(TEXT("slot_name"), Montage->SlotAnimTracks[i].SlotName.ToString());
		SlotsArr.Add(MakeShared<FJsonValueObject>(SlotObj));
	}
	Root->SetArrayField(TEXT("slots"), SlotsArr);

	Root->SetNumberField(TEXT("notify_count"), Montage->Notifies.Num());

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetBlendSpaceInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UBlendSpace* BS = FMonolithAssetUtils::LoadAssetByPath<UBlendSpace>(AssetPath);
	if (!BS) return FMonolithActionResult::Error(FString::Printf(TEXT("BlendSpace not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	USkeleton* Skel = BS->GetSkeleton();
	Root->SetStringField(TEXT("skeleton"), Skel ? Skel->GetPathName() : TEXT("None"));
	Root->SetBoolField(TEXT("is_1d"), BS->IsA<UBlendSpace1D>());

	// Axis X
	auto MakeAxisObj = [](const FBlendParameter& Param) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> AxisObj = MakeShared<FJsonObject>();
		AxisObj->SetStringField(TEXT("name"), Param.DisplayName);
		AxisObj->SetNumberField(TEXT("min"), Param.Min);
		AxisObj->SetNumberField(TEXT("max"), Param.Max);
		AxisObj->SetNumberField(TEXT("grid_divisions"), Param.GridNum);
		AxisObj->SetBoolField(TEXT("snap_to_grid"), Param.bSnapToGrid);
		AxisObj->SetBoolField(TEXT("wrap_input"), Param.bWrapInput);
		return AxisObj;
	};

	Root->SetObjectField(TEXT("axis_x"), MakeAxisObj(BS->GetBlendParameter(0)));
	Root->SetObjectField(TEXT("axis_y"), MakeAxisObj(BS->GetBlendParameter(1)));

	// Interpolation structure (T3-5 companion). interpolate_using_grid = the authored
	// "Interpolate using grid" flag; triangulation_baked = whether the runtime
	// triangulation/grid structure has actually been built (empty until ResampleData).
	Root->SetBoolField(TEXT("interpolate_using_grid"), BS->bInterpolateUsingGrid);
	Root->SetBoolField(TEXT("triangulation_baked"), !BS->GetBlendSpaceData().IsEmpty());

	// Samples
	const TArray<FBlendSample>& Samples = BS->GetBlendSamples();
	TArray<TSharedPtr<FJsonValue>> SamplesArr;
	for (int32 i = 0; i < Samples.Num(); ++i)
	{
		const FBlendSample& Sample = Samples[i];
		TSharedPtr<FJsonObject> SampleObj = MakeShared<FJsonObject>();
		SampleObj->SetNumberField(TEXT("index"), i);
		SampleObj->SetStringField(TEXT("animation"),
			Sample.Animation ? Sample.Animation->GetPathName() : TEXT("None"));
		SampleObj->SetNumberField(TEXT("x"), Sample.SampleValue.X);
		SampleObj->SetNumberField(TEXT("y"), Sample.SampleValue.Y);
		SampleObj->SetNumberField(TEXT("rate_scale"), Sample.RateScale);

		// Authored root-motion speed (cm/s) — distance the root travels over the clip
		// divided by play length, scaled by RateScale. Root-locked clips (root motion
		// disabled) cannot report a speed: emit the explicit "unknowable" signal rather
		// than a misleading 0 (gotcha 6). Planar (XY) distance only — vertical motion is
		// not locomotion speed.
		if (UAnimSequence* SampleSeq = Sample.Animation)
		{
			const float PlayLength = SampleSeq->GetPlayLength();
			if (!SampleSeq->HasRootMotion())
			{
				SampleObj->SetBoolField(TEXT("root_motion_speed_known"), false);
				SampleObj->SetStringField(TEXT("root_motion_speed_note"),
					TEXT("root motion disabled (root-locked) — authored speed is unknowable"));
			}
			else if (PlayLength <= KINDA_SMALL_NUMBER)
			{
				SampleObj->SetBoolField(TEXT("root_motion_speed_known"), false);
				SampleObj->SetStringField(TEXT("root_motion_speed_note"),
					TEXT("zero-length clip — speed is undefined"));
			}
			else
			{
				const FTransform RootDelta = SampleSeq->ExtractRootMotionFromRange(
					0.0, static_cast<double>(PlayLength), FAnimExtractContext());
				const FVector T = RootDelta.GetTranslation();
				const float PlanarDist = FVector(T.X, T.Y, 0.0).Size();
				const float RateScale = (Sample.RateScale != 0.0f) ? Sample.RateScale : 1.0f;
				SampleObj->SetBoolField(TEXT("root_motion_speed_known"), true);
				SampleObj->SetNumberField(TEXT("root_motion_speed_cms"),
					(PlanarDist / PlayLength) * RateScale);
			}
		}
		else
		{
			SampleObj->SetBoolField(TEXT("root_motion_speed_known"), false);
		}

		SamplesArr.Add(MakeShared<FJsonValueObject>(SampleObj));
	}
	Root->SetArrayField(TEXT("samples"), SamplesArr);
	Root->SetNumberField(TEXT("sample_count"), SamplesArr.Num());

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetSkeletonSockets(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	// Try skeleton first, then skeletal mesh
	TArray<USkeletalMeshSocket*> SocketList;
	FString SourceType;

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(AssetPath);
	if (Skeleton)
	{
		SocketList = Skeleton->Sockets;
		SourceType = TEXT("Skeleton");
	}
	else
	{
		USkeletalMesh* Mesh = FMonolithAssetUtils::LoadAssetByPath<USkeletalMesh>(AssetPath);
		if (!Mesh)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton or SkeletalMesh not found: %s"), *AssetPath));

		for (int32 i = 0; i < Mesh->NumSockets(); ++i)
		{
			if (USkeletalMeshSocket* Sock = Mesh->GetSocketByIndex(i))
				SocketList.Add(Sock);
		}
		SourceType = TEXT("SkeletalMesh");
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("source_type"), SourceType);

	TArray<TSharedPtr<FJsonValue>> SocketsArr;
	for (USkeletalMeshSocket* Sock : SocketList)
	{
		if (!Sock) continue;
		TSharedPtr<FJsonObject> SockObj = MakeShared<FJsonObject>();
		SockObj->SetStringField(TEXT("name"), Sock->SocketName.ToString());
		SockObj->SetStringField(TEXT("bone"), Sock->BoneName.ToString());

		TArray<TSharedPtr<FJsonValue>> LocArr;
		LocArr.Add(MakeShared<FJsonValueNumber>(Sock->RelativeLocation.X));
		LocArr.Add(MakeShared<FJsonValueNumber>(Sock->RelativeLocation.Y));
		LocArr.Add(MakeShared<FJsonValueNumber>(Sock->RelativeLocation.Z));
		SockObj->SetArrayField(TEXT("location"), LocArr);

		TArray<TSharedPtr<FJsonValue>> RotArr;
		RotArr.Add(MakeShared<FJsonValueNumber>(Sock->RelativeRotation.Pitch));
		RotArr.Add(MakeShared<FJsonValueNumber>(Sock->RelativeRotation.Yaw));
		RotArr.Add(MakeShared<FJsonValueNumber>(Sock->RelativeRotation.Roll));
		SockObj->SetArrayField(TEXT("rotation"), RotArr);

		TArray<TSharedPtr<FJsonValue>> ScaleArr;
		ScaleArr.Add(MakeShared<FJsonValueNumber>(Sock->RelativeScale.X));
		ScaleArr.Add(MakeShared<FJsonValueNumber>(Sock->RelativeScale.Y));
		ScaleArr.Add(MakeShared<FJsonValueNumber>(Sock->RelativeScale.Z));
		SockObj->SetArrayField(TEXT("scale"), ScaleArr);

		SocketsArr.Add(MakeShared<FJsonValueObject>(SockObj));
	}

	Root->SetArrayField(TEXT("sockets"), SocketsArr);
	Root->SetNumberField(TEXT("count"), SocketsArr.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetSkeletonPreviewAttachedAssets(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(AssetPath);
	if (!Skeleton)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *AssetPath));

	const FPreviewAssetAttachContainer& Container = Skeleton->PreviewAttachedAssetContainer;
	const int32 NumAttached = Container.Num();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	TArray<TSharedPtr<FJsonValue>> AttachedArr;
	for (int32 i = 0; i < NumAttached; ++i)
	{
		const FPreviewAttachedObjectPair& Pair = Container[i];
		UObject* AttachedObject = Pair.GetAttachedObject();
		const FName AttachPoint = Pair.AttachedTo;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("attach_point"), AttachPoint.ToString());
		Entry->SetStringField(TEXT("attached_object"),
			AttachedObject ? AttachedObject->GetPathName() : TEXT("None"));
		Entry->SetStringField(TEXT("attached_object_class"),
			AttachedObject ? AttachedObject->GetClass()->GetName() : TEXT("None"));

		AttachedArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	Root->SetArrayField(TEXT("attached_objects"), AttachedArr);
	Root->SetNumberField(TEXT("count"), AttachedArr.Num());

	// FPreviewAssetAttachContainer does NOT store relative transforms — Persona
	// attaches preview assets at the socket origin with the asset's natural
	// orientation. Any visual placement comes from (a) the socket's position on
	// the skeleton and (b) the attached mesh's own pivot. There is no hidden
	// FTransform to recover here.
	Root->SetBoolField(TEXT("transforms_stored"), false);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetBoneRefPose(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	const FReferenceSkeleton* RefSkel = nullptr;
	FString SourceType;

	if (USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(AssetPath))
	{
		RefSkel = &Skeleton->GetReferenceSkeleton();
		SourceType = TEXT("Skeleton");
	}
	else if (USkeletalMesh* Mesh = FMonolithAssetUtils::LoadAssetByPath<USkeletalMesh>(AssetPath))
	{
		RefSkel = &Mesh->GetRefSkeleton();
		SourceType = TEXT("SkeletalMesh");
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton or SkeletalMesh not found: %s"), *AssetPath));
	}

	const TArray<FTransform>& RefBonePose = RefSkel->GetRefBonePose(); // parent-relative
	const int32 NumBones = RefSkel->GetNum();

	// Compute component-space transforms by walking the hierarchy once.
	TArray<FTransform> ComponentSpace;
	ComponentSpace.SetNum(NumBones);
	for (int32 i = 0; i < NumBones; ++i)
	{
		const int32 ParentIdx = RefSkel->GetParentIndex(i);
		ComponentSpace[i] = (ParentIdx >= 0)
			? RefBonePose[i] * ComponentSpace[ParentIdx]
			: RefBonePose[i];
	}

	// Resolve which bones to emit (default = all).
	TArray<int32> BoneIndices;
	const TArray<TSharedPtr<FJsonValue>>* RequestedBones = nullptr;
	if (Params->TryGetArrayField(TEXT("bone_names"), RequestedBones) && RequestedBones && RequestedBones->Num() > 0)
	{
		for (const TSharedPtr<FJsonValue>& Val : *RequestedBones)
		{
			if (!Val.IsValid()) continue;
			const FName BoneName(*Val->AsString());
			const int32 Idx = RefSkel->FindBoneIndex(BoneName);
			if (Idx != INDEX_NONE)
				BoneIndices.Add(Idx);
		}
	}
	else
	{
		BoneIndices.Reserve(NumBones);
		for (int32 i = 0; i < NumBones; ++i)
			BoneIndices.Add(i);
	}

	auto WriteVec = [](const FVector& V) {
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	};
	auto WriteRot = [](const FRotator& R) {
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("pitch"), R.Pitch);
		O->SetNumberField(TEXT("yaw"), R.Yaw);
		O->SetNumberField(TEXT("roll"), R.Roll);
		return O;
	};
	auto WriteXform = [&](const FTransform& T) {
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetObjectField(TEXT("location"), WriteVec(T.GetLocation()));
		O->SetObjectField(TEXT("rotation"), WriteRot(T.GetRotation().Rotator()));
		O->SetObjectField(TEXT("scale"), WriteVec(T.GetScale3D()));
		return O;
	};

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("source_type"), SourceType);
	Root->SetNumberField(TEXT("bone_count"), NumBones);

	TArray<TSharedPtr<FJsonValue>> BonesArr;
	for (int32 BoneIdx : BoneIndices)
	{
		const FName BoneName = RefSkel->GetBoneName(BoneIdx);
		const int32 ParentIdx = RefSkel->GetParentIndex(BoneIdx);

		TSharedPtr<FJsonObject> BoneObj = MakeShared<FJsonObject>();
		BoneObj->SetNumberField(TEXT("index"), BoneIdx);
		BoneObj->SetStringField(TEXT("name"), BoneName.ToString());
		BoneObj->SetNumberField(TEXT("parent_index"), ParentIdx);
		if (ParentIdx >= 0)
			BoneObj->SetStringField(TEXT("parent_name"), RefSkel->GetBoneName(ParentIdx).ToString());
		BoneObj->SetObjectField(TEXT("local"), WriteXform(RefBonePose[BoneIdx]));
		BoneObj->SetObjectField(TEXT("component"), WriteXform(ComponentSpace[BoneIdx]));

		BonesArr.Add(MakeShared<FJsonValueObject>(BoneObj));
	}

	Root->SetArrayField(TEXT("bones"), BonesArr);
	return FMonolithActionResult::Success(Root);
}

// T3-2: evaluate ONE bone's transform on an animation at a specific frame/time.
// Extends the get_bone_ref_pose FK-compose idea from the BIND pose to an ANIMATED
// frame: where get_bone_ref_pose walks the reference skeleton, this evaluates the
// sequence's animated pose via UAnimPoseExtensions (which performs the FK compose
// internally) and returns the requested bone's transform.
//
// Inputs: asset_path (AnimSequence), bone_name, exactly one of {frame:int, time:sec}
// (frame wins if both present), space ("component"|"world", default "component").
// EAnimPoseSpaces has only Local and World — FAnimPose's World space IS component
// space (root-relative, no actor placement). We therefore map both "component" and
// "world" to EAnimPoseSpaces::World here (a clip has no world/actor context) and echo
// the requested space; "local" is also accepted and maps to EAnimPoseSpaces::Local
// (parent-relative), matching get_bone_ref_pose's `local` field.
FMonolithActionResult FMonolithAnimationActions::HandleGetAnimatedBoneTransform(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString BoneName  = Params->GetStringField(TEXT("bone_name"));

	if (BoneName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("bone_name is required"));
	}

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	// Validate the bone exists on the sequence's skeleton up front (clearer error than
	// a silent identity transform from GetBonePose).
	const FName BoneFName(*BoneName);
	if (USkeleton* Skeleton = Seq->GetSkeleton())
	{
		if (Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneFName) == INDEX_NONE)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Bone '%s' not found in skeleton of %s"), *BoneName, *AssetPath));
		}
	}

	// Space param: component (default) / world both map to FAnimPose World space; local
	// maps to Local (parent-relative).
	FString SpaceStr = TEXT("component");
	Params->TryGetStringField(TEXT("space"), SpaceStr);
	const FString SpaceLower = SpaceStr.ToLower();
	EAnimPoseSpaces PoseSpace = EAnimPoseSpaces::World;
	if (SpaceLower == TEXT("local"))
	{
		PoseSpace = EAnimPoseSpaces::Local;
	}
	else if (SpaceLower != TEXT("component") && SpaceLower != TEXT("world"))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid space '%s'. Expected one of: component, world, local."), *SpaceStr));
	}

	// frame OR time — frame takes precedence when both supplied.
	const IAnimationDataModel* DataModel = Seq->GetDataModel();
	const int32 NumFrames = DataModel ? DataModel->GetNumberOfFrames() : 0;
	const float PlayLength = Seq->GetPlayLength();

	// Eval options mirror the footspeed signal path (raw data, root motion folded into
	// the pose so World/component transforms include locomotion).
	FAnimPoseEvaluationOptions EvalOptions;
	EvalOptions.EvaluationType = EAnimDataEvalType::Raw;
	EvalOptions.bShouldRetarget = true;
	EvalOptions.bExtractRootMotion = false;
	EvalOptions.bIncorporateRootMotionIntoPose = true;
	EvalOptions.OptionalSkeletalMesh = nullptr;
	EvalOptions.bRetrieveAdditiveAsFullPose = true;
	EvalOptions.bEvaluateCurves = false;

	FAnimPose Pose;
	bool bUsedFrame = false;
	int32 ResolvedFrame = INDEX_NONE;
	float ResolvedTime = 0.0f;

	double FrameNum = 0.0;
	double TimeSec = 0.0;
	const bool bHasFrame = Params->TryGetNumberField(TEXT("frame"), FrameNum);
	const bool bHasTime  = Params->TryGetNumberField(TEXT("time"), TimeSec);

	if (!bHasFrame && !bHasTime)
	{
		return FMonolithActionResult::Error(TEXT("Provide one of: frame (integer) or time (seconds)"));
	}

	if (bHasFrame)
	{
		ResolvedFrame = static_cast<int32>(FrameNum);
		if (NumFrames > 0 && (ResolvedFrame < 0 || ResolvedFrame > NumFrames))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("frame %d out of range [0, %d] for %s"), ResolvedFrame, NumFrames, *AssetPath));
		}
		UAnimPoseExtensions::GetAnimPoseAtFrame(Seq, ResolvedFrame, EvalOptions, Pose);
		bUsedFrame = true;
	}
	else
	{
		ResolvedTime = static_cast<float>(TimeSec);
		if (ResolvedTime < 0.0f || ResolvedTime > PlayLength)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("time %.4f out of range [0, %.4f] for %s"), ResolvedTime, PlayLength, *AssetPath));
		}
		UAnimPoseExtensions::GetAnimPoseAtTime(Seq, static_cast<double>(ResolvedTime), EvalOptions, Pose);
	}

	if (!Pose.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to evaluate a valid pose on %s (the sequence may have no bone data)"), *AssetPath));
	}

	const FTransform BoneXform = UAnimPoseExtensions::GetBonePose(Pose, BoneFName, PoseSpace);

	auto WriteVec = [](const FVector& V) {
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	};
	auto WriteRot = [](const FRotator& R) {
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("pitch"), R.Pitch);
		O->SetNumberField(TEXT("yaw"), R.Yaw);
		O->SetNumberField(TEXT("roll"), R.Roll);
		return O;
	};

	TSharedPtr<FJsonObject> XformObj = MakeShared<FJsonObject>();
	XformObj->SetObjectField(TEXT("location"), WriteVec(BoneXform.GetLocation()));
	XformObj->SetObjectField(TEXT("rotation"), WriteRot(BoneXform.GetRotation().Rotator()));
	XformObj->SetObjectField(TEXT("scale"), WriteVec(BoneXform.GetScale3D()));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("bone_name"), BoneName);
	Root->SetStringField(TEXT("space"), (PoseSpace == EAnimPoseSpaces::Local) ? TEXT("local") : SpaceLower);
	Root->SetNumberField(TEXT("num_frames"), NumFrames);
	Root->SetNumberField(TEXT("play_length"), PlayLength);
	Root->SetBoolField(TEXT("queried_by_frame"), bUsedFrame);
	if (bUsedFrame)
	{
		Root->SetNumberField(TEXT("frame"), ResolvedFrame);
	}
	else
	{
		Root->SetNumberField(TEXT("time"), ResolvedTime);
	}
	Root->SetObjectField(TEXT("transform"), XformObj);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetAbpInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	USkeleton* Skel = ABP->TargetSkeleton;
	Root->SetStringField(TEXT("skeleton"), Skel ? Skel->GetPathName() : TEXT("None"));
	Root->SetStringField(TEXT("target_skeleton"), Skel ? Skel->GetPathName() : TEXT("None"));
	Root->SetStringField(TEXT("parent_class"),
		ABP->ParentClass ? ABP->ParentClass->GetName() : TEXT("None"));

	// Count state machines
	int32 StateMachineCount = 0;
	TArray<TSharedPtr<FJsonValue>> GraphNames;
	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (!Graph) continue;
		GraphNames.Add(MakeShared<FJsonValueString>(Graph->GetName()));
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Cast<UAnimGraphNode_StateMachine>(Node))
				++StateMachineCount;
		}
	}
	for (UEdGraph* Graph : ABP->UbergraphPages)
	{
		if (!Graph) continue;
		GraphNames.Add(MakeShared<FJsonValueString>(Graph->GetName()));
	}

	Root->SetNumberField(TEXT("state_machine_count"), StateMachineCount);
	Root->SetNumberField(TEXT("graph_count"), GraphNames.Num());
	Root->SetArrayField(TEXT("graphs"), GraphNames);
	Root->SetNumberField(TEXT("variable_count"), ABP->NewVariables.Num());

	// Interfaces
	TArray<TSharedPtr<FJsonValue>> InterfacesArr;
	for (const FBPInterfaceDescription& Iface : ABP->ImplementedInterfaces)
	{
		if (Iface.Interface)
			InterfacesArr.Add(MakeShared<FJsonValueString>(Iface.Interface->GetName()));
	}
	Root->SetArrayField(TEXT("interfaces"), InterfacesArr);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Wave 2 — Notify CRUD
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleAddNotify(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString NotifyClassName = Params->GetStringField(TEXT("notify_class"));
	float Time = static_cast<float>(Params->GetNumberField(TEXT("time")));
	FString TrackName = TEXT("1");
	Params->TryGetStringField(TEXT("track_name"), TrackName);

	UAnimSequenceBase* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequenceBase>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("Animation asset not found: %s"), *AssetPath));

	if (Time < 0.f || Time > Seq->GetPlayLength())
		return FMonolithActionResult::Error(FString::Printf(TEXT("Time %.3f out of range [0, %.3f]"), Time, Seq->GetPlayLength()));

	UClass* NotifyClass = FindFirstObject<UClass>(*NotifyClassName, EFindFirstObjectOptions::NativeFirst);
	if (!NotifyClass)
		NotifyClass = FindFirstObject<UClass>(*FString::Printf(TEXT("AnimNotify_%s"), *NotifyClassName), EFindFirstObjectOptions::NativeFirst);
	if (!NotifyClass || !NotifyClass->IsChildOf(UAnimNotify::StaticClass()))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Notify class not found or not a UAnimNotify subclass: %s"), *NotifyClassName));

	UAnimNotify* NewNotify = NewObject<UAnimNotify>(Seq, NotifyClass);
	if (!NewNotify)
		return FMonolithActionResult::Error(TEXT("Failed to create notify instance"));

	GEditor->BeginTransaction(FText::FromString(TEXT("Add Notify")));
	Seq->Modify();

	UAnimationBlueprintLibrary::AddAnimationNotifyEventObject(Seq, Time, NewNotify, FName(*TrackName));
	Seq->RefreshCacheData();

	GEditor->EndTransaction();
	Seq->MarkPackageDirty();

	int32 NewIndex = Seq->Notifies.Num() - 1;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("index"), NewIndex);
	Root->SetStringField(TEXT("notify_class"), NotifyClass->GetName());
	Root->SetNumberField(TEXT("time"), Time);
	Root->SetStringField(TEXT("track_name"), TrackName);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleAddNotifyState(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString NotifyClassName = Params->GetStringField(TEXT("notify_class"));
	float Time = static_cast<float>(Params->GetNumberField(TEXT("time")));
	float Duration = static_cast<float>(Params->GetNumberField(TEXT("duration")));
	FString TrackName = TEXT("1");
	Params->TryGetStringField(TEXT("track_name"), TrackName);

	UAnimSequenceBase* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequenceBase>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("Animation asset not found: %s"), *AssetPath));

	if (Time < 0.f || Time > Seq->GetPlayLength())
		return FMonolithActionResult::Error(FString::Printf(TEXT("Time %.3f out of range [0, %.3f]"), Time, Seq->GetPlayLength()));
	if (Duration <= 0.f)
		return FMonolithActionResult::Error(TEXT("Duration must be > 0"));
	if (Time + Duration > Seq->GetPlayLength())
		return FMonolithActionResult::Error(FString::Printf(TEXT("Time + Duration (%.3f) exceeds play length (%.3f)"), Time + Duration, Seq->GetPlayLength()));

	UClass* NotifyClass = FindFirstObject<UClass>(*NotifyClassName, EFindFirstObjectOptions::NativeFirst);
	if (!NotifyClass)
		NotifyClass = FindFirstObject<UClass>(*FString::Printf(TEXT("AnimNotifyState_%s"), *NotifyClassName), EFindFirstObjectOptions::NativeFirst);
	if (!NotifyClass || !NotifyClass->IsChildOf(UAnimNotifyState::StaticClass()))
		return FMonolithActionResult::Error(FString::Printf(TEXT("NotifyState class not found or not a UAnimNotifyState subclass: %s"), *NotifyClassName));

	UAnimNotifyState* NewNotifyState = NewObject<UAnimNotifyState>(Seq, NotifyClass);
	if (!NewNotifyState)
		return FMonolithActionResult::Error(TEXT("Failed to create notify state instance"));

	GEditor->BeginTransaction(FText::FromString(TEXT("Add Notify State")));
	Seq->Modify();

	UAnimationBlueprintLibrary::AddAnimationNotifyStateEventObject(Seq, Time, Duration, NewNotifyState, FName(*TrackName));
	Seq->RefreshCacheData();

	GEditor->EndTransaction();
	Seq->MarkPackageDirty();

	int32 NewIndex = Seq->Notifies.Num() - 1;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("index"), NewIndex);
	Root->SetStringField(TEXT("notify_class"), NotifyClass->GetName());
	Root->SetNumberField(TEXT("time"), Time);
	Root->SetNumberField(TEXT("duration"), Duration);
	Root->SetStringField(TEXT("track_name"), TrackName);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleRemoveNotify(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	int32 NotifyIndex = static_cast<int32>(Params->GetNumberField(TEXT("notify_index")));

	UAnimSequenceBase* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequenceBase>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("Animation asset not found: %s"), *AssetPath));

	if (!Seq->Notifies.IsValidIndex(NotifyIndex))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid notify index: %d (total: %d)"), NotifyIndex, Seq->Notifies.Num()));

	FName RemovedName = Seq->Notifies[NotifyIndex].NotifyName;

	GEditor->BeginTransaction(FText::FromString(TEXT("Remove Notify")));
	Seq->Modify();

	Seq->Notifies.RemoveAt(NotifyIndex);
	Seq->RefreshCacheData();

	GEditor->EndTransaction();
	Seq->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("removed_index"), NotifyIndex);
	Root->SetStringField(TEXT("removed_name"), RemovedName.ToString());
	Root->SetNumberField(TEXT("remaining_count"), Seq->Notifies.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetNotifyTrack(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	int32 NotifyIndex = static_cast<int32>(Params->GetNumberField(TEXT("notify_index")));
	int32 TrackIndex = static_cast<int32>(Params->GetNumberField(TEXT("track_index")));

	UAnimSequenceBase* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequenceBase>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("Animation asset not found: %s"), *AssetPath));

	if (!Seq->Notifies.IsValidIndex(NotifyIndex))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid notify index: %d (total: %d)"), NotifyIndex, Seq->Notifies.Num()));

	if (!Seq->AnimNotifyTracks.IsValidIndex(TrackIndex))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid track index: %d (total: %d)"), TrackIndex, Seq->AnimNotifyTracks.Num()));

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Notify Track")));
	Seq->Modify();

	Seq->Notifies[NotifyIndex].TrackIndex = TrackIndex;
	Seq->RefreshCacheData();

	GEditor->EndTransaction();
	Seq->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("notify_index"), NotifyIndex);
	Root->SetStringField(TEXT("notify_name"), Seq->Notifies[NotifyIndex].NotifyName.ToString());
	Root->SetNumberField(TEXT("new_track_index"), TrackIndex);
	Root->SetStringField(TEXT("track_name"), Seq->AnimNotifyTracks[TrackIndex].TrackName.ToString());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Wave 3 — Curve CRUD (5 actions)
// ---------------------------------------------------------------------------

static FString InterpModeToString(ERichCurveInterpMode Mode)
{
	switch (Mode)
	{
	case RCIM_Constant: return TEXT("constant");
	case RCIM_Linear:   return TEXT("linear");
	case RCIM_Cubic:    return TEXT("cubic");
	default:            return TEXT("unknown");
	}
}

static ERichCurveInterpMode StringToInterpMode(const FString& Str)
{
	if (Str.Equals(TEXT("constant"), ESearchCase::IgnoreCase)) return RCIM_Constant;
	if (Str.Equals(TEXT("linear"), ESearchCase::IgnoreCase))   return RCIM_Linear;
	return RCIM_Cubic; // default
}

FMonolithActionResult FMonolithAnimationActions::HandleListCurves(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	bool bIncludeKeys = Params->HasField(TEXT("include_keys")) && Params->GetBoolField(TEXT("include_keys"));

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	const IAnimationDataModel* DataModel = Seq->GetDataModel();
	if (!DataModel) return FMonolithActionResult::Error(TEXT("No animation data model"));

	TArray<TSharedPtr<FJsonValue>> CurvesArray;

	// Float curves
	for (const FFloatCurve& Curve : DataModel->GetFloatCurves())
	{
		TSharedPtr<FJsonObject> CurveObj = MakeShared<FJsonObject>();
		CurveObj->SetStringField(TEXT("name"), Curve.GetName().ToString());
		CurveObj->SetStringField(TEXT("type"), TEXT("Float"));
		CurveObj->SetNumberField(TEXT("num_keys"), Curve.FloatCurve.GetNumKeys());

		if (Curve.FloatCurve.GetNumKeys() > 0)
		{
			const TArray<FRichCurveKey>& Keys = Curve.FloatCurve.GetConstRefOfKeys();
			float MinVal = Keys[0].Value, MaxVal = Keys[0].Value;
			for (const FRichCurveKey& Key : Keys)
			{
				MinVal = FMath::Min(MinVal, Key.Value);
				MaxVal = FMath::Max(MaxVal, Key.Value);
			}
			CurveObj->SetNumberField(TEXT("min_value"), MinVal);
			CurveObj->SetNumberField(TEXT("max_value"), MaxVal);

			if (bIncludeKeys)
			{
				TArray<TSharedPtr<FJsonValue>> KeysArray;
				for (const FRichCurveKey& Key : Keys)
				{
					TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
					KeyObj->SetNumberField(TEXT("time"), Key.Time);
					KeyObj->SetNumberField(TEXT("value"), Key.Value);
					KeyObj->SetStringField(TEXT("interp_mode"), InterpModeToString(Key.InterpMode));
					KeysArray.Add(MakeShared<FJsonValueObject>(KeyObj));
				}
				CurveObj->SetArrayField(TEXT("keys"), KeysArray);
			}
		}

		CurvesArray.Add(MakeShared<FJsonValueObject>(CurveObj));
	}

	// Transform curves
	for (const FTransformCurve& Curve : DataModel->GetTransformCurves())
	{
		TSharedPtr<FJsonObject> CurveObj = MakeShared<FJsonObject>();
		CurveObj->SetStringField(TEXT("name"), Curve.GetName().ToString());
		CurveObj->SetStringField(TEXT("type"), TEXT("Transform"));
		CurveObj->SetNumberField(TEXT("num_keys"), 0); // Transform curves have sub-curves
		CurvesArray.Add(MakeShared<FJsonValueObject>(CurveObj));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("count"), CurvesArray.Num());
	Root->SetArrayField(TEXT("curves"), CurvesArray);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleAddCurve(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString CurveName = Params->GetStringField(TEXT("curve_name"));
	FString CurveTypeStr = Params->HasField(TEXT("curve_type")) ? Params->GetStringField(TEXT("curve_type")) : TEXT("Float");

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	ERawCurveTrackTypes CurveType = CurveTypeStr.Equals(TEXT("Transform"), ESearchCase::IgnoreCase)
		? ERawCurveTrackTypes::RCT_Transform
		: ERawCurveTrackTypes::RCT_Float;

	const FAnimationCurveIdentifier CurveId(FName(*CurveName), CurveType);

	// Check if curve already exists
	if (Seq->GetDataModel()->FindCurve(CurveId))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Curve '%s' already exists"), *CurveName));

	IAnimationDataController& Controller = Seq->GetController();
	Controller.OpenBracket(FText::FromString(TEXT("Add Curve")));
	bool bSuccess = Controller.AddCurve(CurveId);
	Controller.CloseBracket();

	if (!bSuccess)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to add curve '%s'"), *CurveName));

	Seq->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("curve_name"), CurveName);
	Root->SetStringField(TEXT("curve_type"), CurveTypeStr);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleRemoveCurve(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString CurveName = Params->GetStringField(TEXT("curve_name"));
	FString CurveTypeStr = Params->HasField(TEXT("curve_type")) ? Params->GetStringField(TEXT("curve_type")) : TEXT("Float");

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	ERawCurveTrackTypes CurveType = CurveTypeStr.Equals(TEXT("Transform"), ESearchCase::IgnoreCase)
		? ERawCurveTrackTypes::RCT_Transform
		: ERawCurveTrackTypes::RCT_Float;

	const FAnimationCurveIdentifier CurveId(FName(*CurveName), CurveType);

	if (!Seq->GetDataModel()->FindCurve(CurveId))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Curve '%s' not found"), *CurveName));

	IAnimationDataController& Controller = Seq->GetController();
	Controller.OpenBracket(FText::FromString(TEXT("Remove Curve")));
	bool bSuccess = Controller.RemoveCurve(CurveId);
	Controller.CloseBracket();

	if (!bSuccess)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to remove curve '%s'"), *CurveName));

	Seq->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("curve_name"), CurveName);
	Root->SetBoolField(TEXT("removed"), true);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetCurveKeys(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString CurveName = Params->GetStringField(TEXT("curve_name"));
	FString KeysJson = Params->GetStringField(TEXT("keys_json"));

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	const FAnimationCurveIdentifier CurveId(FName(*CurveName), ERawCurveTrackTypes::RCT_Float);

	if (!Seq->GetDataModel()->FindCurve(CurveId))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Curve '%s' not found — add it first"), *CurveName));

	// Parse keys JSON
	TArray<TSharedPtr<FJsonValue>> JsonKeys;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(KeysJson);
	if (!FJsonSerializer::Deserialize(Reader, JsonKeys))
		return FMonolithActionResult::Error(TEXT("Failed to parse keys_json — expected JSON array"));

	TArray<FRichCurveKey> Keys;
	for (const TSharedPtr<FJsonValue>& KeyVal : JsonKeys)
	{
		const TSharedPtr<FJsonObject>* KeyObjPtr;
		if (!KeyVal->TryGetObject(KeyObjPtr)) continue;
		const TSharedPtr<FJsonObject>& KeyObj = *KeyObjPtr;

		FRichCurveKey Key;
		Key.Time = static_cast<float>(KeyObj->GetNumberField(TEXT("time")));
		Key.Value = static_cast<float>(KeyObj->GetNumberField(TEXT("value")));
		if (KeyObj->HasField(TEXT("interp")))
			Key.InterpMode = StringToInterpMode(KeyObj->GetStringField(TEXT("interp")));
		else
			Key.InterpMode = RCIM_Cubic;
		Keys.Add(Key);
	}

	IAnimationDataController& Controller = Seq->GetController();
	Controller.OpenBracket(FText::FromString(TEXT("Set Curve Keys")));
	bool bSuccess = Controller.SetCurveKeys(CurveId, Keys);
	Controller.CloseBracket();

	if (!bSuccess)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to set keys on curve '%s'"), *CurveName));

	Seq->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("curve_name"), CurveName);
	Root->SetNumberField(TEXT("num_keys"), Keys.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetCurveKeys(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString CurveName = Params->GetStringField(TEXT("curve_name"));

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	const FAnimationCurveIdentifier CurveId(FName(*CurveName), ERawCurveTrackTypes::RCT_Float);
	const FAnimCurveBase* CurveBase = Seq->GetDataModel()->FindCurve(CurveId);
	if (!CurveBase)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Curve '%s' not found"), *CurveName));

	const FFloatCurve* FloatCurve = static_cast<const FFloatCurve*>(CurveBase);
	const TArray<FRichCurveKey>& Keys = FloatCurve->FloatCurve.GetConstRefOfKeys();

	TArray<TSharedPtr<FJsonValue>> KeysArray;
	for (const FRichCurveKey& Key : Keys)
	{
		TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
		KeyObj->SetNumberField(TEXT("time"), Key.Time);
		KeyObj->SetNumberField(TEXT("value"), Key.Value);
		KeyObj->SetStringField(TEXT("interp_mode"), InterpModeToString(Key.InterpMode));
		KeysArray.Add(MakeShared<FJsonValueObject>(KeyObj));
	}

	// --- T3-4: monotonic + sign summary flags over the key VALUES (in time order). ---
	// monotonic: "increasing" (non-decreasing), "decreasing" (non-increasing), "constant"
	//   (all equal), "none" (mixed) or "empty"/"single" for degenerate key counts.
	// sign: "positive" (all > 0), "negative" (all < 0), "zero" (all == 0),
	//   "non_negative" (>= 0 with at least one 0), "non_positive" (<= 0 with at least one 0),
	//   "mixed" (both signs present), or "empty". Keys are already authored in time order
	//   (FRichCurve stores keys time-sorted); no re-sort needed.
	FString Monotonic;
	FString Sign;
	if (Keys.Num() == 0)
	{
		Monotonic = TEXT("empty");
		Sign = TEXT("empty");
	}
	else
	{
		bool bNonDecreasing = true;
		bool bNonIncreasing = true;
		bool bAnyPositive = false;
		bool bAnyNegative = false;
		bool bAnyZero = false;
		for (int32 i = 0; i < Keys.Num(); ++i)
		{
			const float V = Keys[i].Value;
			if (V > 0.0f)      bAnyPositive = true;
			else if (V < 0.0f) bAnyNegative = true;
			else               bAnyZero = true;

			if (i > 0)
			{
				const float Prev = Keys[i - 1].Value;
				if (V < Prev) bNonDecreasing = false;
				if (V > Prev) bNonIncreasing = false;
			}
		}

		if (Keys.Num() == 1)
		{
			Monotonic = TEXT("single");
		}
		else if (bNonDecreasing && bNonIncreasing)
		{
			Monotonic = TEXT("constant");
		}
		else if (bNonDecreasing)
		{
			Monotonic = TEXT("increasing");
		}
		else if (bNonIncreasing)
		{
			Monotonic = TEXT("decreasing");
		}
		else
		{
			Monotonic = TEXT("none");
		}

		if (bAnyPositive && bAnyNegative)       Sign = TEXT("mixed");
		else if (bAnyPositive && bAnyZero)       Sign = TEXT("non_negative");
		else if (bAnyNegative && bAnyZero)       Sign = TEXT("non_positive");
		else if (bAnyPositive)                   Sign = TEXT("positive");
		else if (bAnyNegative)                   Sign = TEXT("negative");
		else                                     Sign = TEXT("zero");
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("curve_name"), CurveName);
	Root->SetNumberField(TEXT("num_keys"), Keys.Num());
	Root->SetArrayField(TEXT("keys"), KeysArray);
	Root->SetStringField(TEXT("monotonic"), Monotonic);
	Root->SetStringField(TEXT("sign"), Sign);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Wave 4 — Skeleton + BlendSpace (6 actions)
// ---------------------------------------------------------------------------

static FVector ParseVectorFromJsonArray(const TArray<TSharedPtr<FJsonValue>>& Arr, const FVector& Default)
{
	if (Arr.Num() < 3) return Default;
	return FVector(Arr[0]->AsNumber(), Arr[1]->AsNumber(), Arr[2]->AsNumber());
}

static FRotator ParseRotatorFromJsonArray(const TArray<TSharedPtr<FJsonValue>>& Arr, const FRotator& Default)
{
	if (Arr.Num() < 3) return Default;
	return FRotator(Arr[0]->AsNumber(), Arr[1]->AsNumber(), Arr[2]->AsNumber());
}

FMonolithActionResult FMonolithAnimationActions::HandleAddSocket(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString BoneName = Params->GetStringField(TEXT("bone_name"));
	FString SocketName = Params->GetStringField(TEXT("socket_name"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(AssetPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *AssetPath));

	// Validate bone exists
	if (Skeleton->GetReferenceSkeleton().FindBoneIndex(FName(*BoneName)) == INDEX_NONE)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Bone '%s' not found in skeleton"), *BoneName));

	// Check socket doesn't already exist
	if (Skeleton->FindSocket(FName(*SocketName)))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Socket '%s' already exists"), *SocketName));

	USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Skeleton);
	Socket->SocketName = FName(*SocketName);
	Socket->BoneName = FName(*BoneName);

	// Parse optional transform
	const TArray<TSharedPtr<FJsonValue>>* LocationArr;
	if (Params->TryGetArrayField(TEXT("location"), LocationArr))
		Socket->RelativeLocation = ParseVectorFromJsonArray(*LocationArr, FVector::ZeroVector);

	const TArray<TSharedPtr<FJsonValue>>* RotationArr;
	if (Params->TryGetArrayField(TEXT("rotation"), RotationArr))
		Socket->RelativeRotation = ParseRotatorFromJsonArray(*RotationArr, FRotator::ZeroRotator);

	const TArray<TSharedPtr<FJsonValue>>* ScaleArr;
	if (Params->TryGetArrayField(TEXT("scale"), ScaleArr))
		Socket->RelativeScale = ParseVectorFromJsonArray(*ScaleArr, FVector::OneVector);
	else
		Socket->RelativeScale = FVector::OneVector;

	Skeleton->Sockets.Add(Socket);
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("socket_name"), SocketName);
	Root->SetStringField(TEXT("bone_name"), BoneName);
	TArray<TSharedPtr<FJsonValue>> LocArr;
	LocArr.Add(MakeShared<FJsonValueNumber>(Socket->RelativeLocation.X));
	LocArr.Add(MakeShared<FJsonValueNumber>(Socket->RelativeLocation.Y));
	LocArr.Add(MakeShared<FJsonValueNumber>(Socket->RelativeLocation.Z));
	Root->SetArrayField(TEXT("location"), LocArr);
	TArray<TSharedPtr<FJsonValue>> RotArr;
	RotArr.Add(MakeShared<FJsonValueNumber>(Socket->RelativeRotation.Pitch));
	RotArr.Add(MakeShared<FJsonValueNumber>(Socket->RelativeRotation.Yaw));
	RotArr.Add(MakeShared<FJsonValueNumber>(Socket->RelativeRotation.Roll));
	Root->SetArrayField(TEXT("rotation"), RotArr);
	TArray<TSharedPtr<FJsonValue>> SclArr;
	SclArr.Add(MakeShared<FJsonValueNumber>(Socket->RelativeScale.X));
	SclArr.Add(MakeShared<FJsonValueNumber>(Socket->RelativeScale.Y));
	SclArr.Add(MakeShared<FJsonValueNumber>(Socket->RelativeScale.Z));
	Root->SetArrayField(TEXT("scale"), SclArr);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleRemoveSocket(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SocketName = Params->GetStringField(TEXT("socket_name"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(AssetPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *AssetPath));

	USkeletalMeshSocket* FoundSocket = nullptr;
	for (USkeletalMeshSocket* Socket : Skeleton->Sockets)
	{
		if (Socket && Socket->SocketName == FName(*SocketName))
		{
			FoundSocket = Socket;
			break;
		}
	}

	if (!FoundSocket)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Socket '%s' not found"), *SocketName));

	Skeleton->Sockets.Remove(FoundSocket);
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("removed_socket"), SocketName);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetSocketTransform(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SocketName = Params->GetStringField(TEXT("socket_name"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(AssetPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *AssetPath));

	USkeletalMeshSocket* FoundSocket = nullptr;
	for (USkeletalMeshSocket* Socket : Skeleton->Sockets)
	{
		if (Socket && Socket->SocketName == FName(*SocketName))
		{
			FoundSocket = Socket;
			break;
		}
	}

	if (!FoundSocket)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Socket '%s' not found"), *SocketName));

	bool bAnySet = false;

	const TArray<TSharedPtr<FJsonValue>>* LocationArr;
	if (Params->TryGetArrayField(TEXT("location"), LocationArr))
	{
		FoundSocket->RelativeLocation = ParseVectorFromJsonArray(*LocationArr, FoundSocket->RelativeLocation);
		bAnySet = true;
	}

	const TArray<TSharedPtr<FJsonValue>>* RotationArr;
	if (Params->TryGetArrayField(TEXT("rotation"), RotationArr))
	{
		FoundSocket->RelativeRotation = ParseRotatorFromJsonArray(*RotationArr, FoundSocket->RelativeRotation);
		bAnySet = true;
	}

	const TArray<TSharedPtr<FJsonValue>>* ScaleArr;
	if (Params->TryGetArrayField(TEXT("scale"), ScaleArr))
	{
		FoundSocket->RelativeScale = ParseVectorFromJsonArray(*ScaleArr, FoundSocket->RelativeScale);
		bAnySet = true;
	}

	if (!bAnySet)
		return FMonolithActionResult::Error(TEXT("At least one of location, rotation, or scale must be provided"));

	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("socket_name"), SocketName);
	TArray<TSharedPtr<FJsonValue>> LocArr;
	LocArr.Add(MakeShared<FJsonValueNumber>(FoundSocket->RelativeLocation.X));
	LocArr.Add(MakeShared<FJsonValueNumber>(FoundSocket->RelativeLocation.Y));
	LocArr.Add(MakeShared<FJsonValueNumber>(FoundSocket->RelativeLocation.Z));
	Root->SetArrayField(TEXT("location"), LocArr);
	TArray<TSharedPtr<FJsonValue>> RotArr;
	RotArr.Add(MakeShared<FJsonValueNumber>(FoundSocket->RelativeRotation.Pitch));
	RotArr.Add(MakeShared<FJsonValueNumber>(FoundSocket->RelativeRotation.Yaw));
	RotArr.Add(MakeShared<FJsonValueNumber>(FoundSocket->RelativeRotation.Roll));
	Root->SetArrayField(TEXT("rotation"), RotArr);
	TArray<TSharedPtr<FJsonValue>> SclArr;
	SclArr.Add(MakeShared<FJsonValueNumber>(FoundSocket->RelativeScale.X));
	SclArr.Add(MakeShared<FJsonValueNumber>(FoundSocket->RelativeScale.Y));
	SclArr.Add(MakeShared<FJsonValueNumber>(FoundSocket->RelativeScale.Z));
	Root->SetArrayField(TEXT("scale"), SclArr);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetSkeletonCurves(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(AssetPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *AssetPath));

	TArray<TSharedPtr<FJsonValue>> CurvesArray;
	Skeleton->ForEachCurveMetaData([&CurvesArray](FName CurveName, const FCurveMetaData& MetaData)
	{
		TSharedPtr<FJsonObject> CurveObj = MakeShared<FJsonObject>();
		CurveObj->SetStringField(TEXT("name"), CurveName.ToString());
		CurvesArray.Add(MakeShared<FJsonValueObject>(CurveObj));
	});

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("count"), CurvesArray.Num());
	Root->SetArrayField(TEXT("curves"), CurvesArray);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetBlendSpaceAxis(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString AxisStr = Params->GetStringField(TEXT("axis"));

	UBlendSpace* BS = FMonolithAssetUtils::LoadAssetByPath<UBlendSpace>(AssetPath);
	if (!BS) return FMonolithActionResult::Error(FString::Printf(TEXT("BlendSpace not found: %s"), *AssetPath));

	int32 AxisIndex;
	if (AxisStr.Equals(TEXT("X"), ESearchCase::IgnoreCase))
		AxisIndex = 0;
	else if (AxisStr.Equals(TEXT("Y"), ESearchCase::IgnoreCase))
		AxisIndex = 1;
	else
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid axis '%s' — must be X or Y"), *AxisStr));

	// Access BlendParameters via UProperty reflection since it's protected
	FBlendParameter* BlendParam = nullptr;
	{
		FProperty* Prop = UBlendSpace::StaticClass()->FindPropertyByName(TEXT("BlendParameters"));
		if (Prop)
		{
			// BlendParameters is a C array of FBlendParameter[3], offset points to first element
			BlendParam = reinterpret_cast<FBlendParameter*>(Prop->ContainerPtrToValuePtr<uint8>(BS));
			BlendParam += AxisIndex;
		}
	}

	if (!BlendParam)
		return FMonolithActionResult::Error(TEXT("Failed to access BlendParameters via reflection"));

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Blend Space Axis")));
	BS->Modify();

	if (Params->HasField(TEXT("name")))
		BlendParam->DisplayName = Params->GetStringField(TEXT("name"));
	if (Params->HasField(TEXT("min")))
		BlendParam->Min = static_cast<float>(Params->GetNumberField(TEXT("min")));
	if (Params->HasField(TEXT("max")))
		BlendParam->Max = static_cast<float>(Params->GetNumberField(TEXT("max")));
	if (Params->HasField(TEXT("grid_divisions")))
		BlendParam->GridNum = static_cast<int32>(Params->GetNumberField(TEXT("grid_divisions")));
	if (Params->HasField(TEXT("snap_to_grid")))
		BlendParam->bSnapToGrid = Params->GetBoolField(TEXT("snap_to_grid"));
	if (Params->HasField(TEXT("wrap_input")))
		BlendParam->bWrapInput = Params->GetBoolField(TEXT("wrap_input"));

	// Validate min < max
	if (BlendParam->Min >= BlendParam->Max)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(FString::Printf(TEXT("min (%.2f) must be less than max (%.2f)"), BlendParam->Min, BlendParam->Max));
	}

	BS->ValidateSampleData();
	BS->ResampleData();         // rebuild FBlendSpaceData triangulation — REQUIRED for runtime

	GEditor->EndTransaction();
	BS->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("axis"), AxisStr.ToUpper());
	Root->SetStringField(TEXT("name"), BlendParam->DisplayName);
	Root->SetNumberField(TEXT("min"), BlendParam->Min);
	Root->SetNumberField(TEXT("max"), BlendParam->Max);
	Root->SetNumberField(TEXT("grid_divisions"), BlendParam->GridNum);
	Root->SetBoolField(TEXT("snap_to_grid"), BlendParam->bSnapToGrid);
	Root->SetBoolField(TEXT("wrap_input"), BlendParam->bWrapInput);
	Root->SetBoolField(TEXT("baked"), true);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetRootMotionSettings(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	bool bAnySet = false;

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Root Motion Settings")));
	Seq->Modify();

	if (Params->HasField(TEXT("enable_root_motion")))
	{
		Seq->bEnableRootMotion = Params->GetBoolField(TEXT("enable_root_motion"));
		bAnySet = true;
	}

	if (Params->HasField(TEXT("root_motion_lock")))
	{
		FString LockStr = Params->GetStringField(TEXT("root_motion_lock"));
		if (LockStr.Equals(TEXT("AnimFirstFrame"), ESearchCase::IgnoreCase))
			Seq->RootMotionRootLock = ERootMotionRootLock::AnimFirstFrame;
		else if (LockStr.Equals(TEXT("Zero"), ESearchCase::IgnoreCase))
			Seq->RootMotionRootLock = ERootMotionRootLock::Zero;
		else if (LockStr.Equals(TEXT("RefPose"), ESearchCase::IgnoreCase))
			Seq->RootMotionRootLock = ERootMotionRootLock::RefPose;
		else
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid root_motion_lock: '%s' — use AnimFirstFrame, Zero, or RefPose"), *LockStr));
		}
		bAnySet = true;
	}

	if (Params->HasField(TEXT("force_root_lock")))
	{
		Seq->bForceRootLock = Params->GetBoolField(TEXT("force_root_lock"));
		bAnySet = true;
	}

	if (!bAnySet)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("At least one of enable_root_motion, root_motion_lock, or force_root_lock must be provided"));
	}

	GEditor->EndTransaction();
	Seq->MarkPackageDirty();

	// Map enum back to string
	FString LockName;
	switch (Seq->RootMotionRootLock)
	{
	case ERootMotionRootLock::AnimFirstFrame: LockName = TEXT("AnimFirstFrame"); break;
	case ERootMotionRootLock::Zero:           LockName = TEXT("Zero"); break;
	case ERootMotionRootLock::RefPose:        LockName = TEXT("RefPose"); break;
	default:                                  LockName = TEXT("Unknown"); break;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetBoolField(TEXT("enable_root_motion"), Seq->bEnableRootMotion);
	Root->SetStringField(TEXT("root_motion_lock"), LockName);
	Root->SetBoolField(TEXT("force_root_lock"), Seq->bForceRootLock);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Wave 5 — Creation + Montage
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleCreateSequence(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SkeletonPath = Params->GetStringField(TEXT("skeleton_path"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(SkeletonPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath));

	// Extract asset name from path
	FString AssetName;
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	}
	AssetName = AssetPath.Mid(LastSlash + 1);

	// Check if asset already exists
	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));
	}

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));
	}

	UAnimSequence* Seq = NewObject<UAnimSequence>(Pkg, FName(*AssetName), RF_Public | RF_Standalone);
	if (!Seq)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UAnimSequence object"));
	}

	Seq->SetSkeleton(Skeleton);
	FAssetRegistryModule::AssetCreated(Seq);
	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Seq->GetPathName());
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleDuplicateSequence(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath = Params->GetStringField(TEXT("source_path"));
	FString DestPath = Params->GetStringField(TEXT("dest_path"));

	// Check source exists
	UObject* SourceObj = FMonolithAssetUtils::LoadAssetByPath<UObject>(SourcePath);
	if (!SourceObj)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source asset not found: %s"), *SourcePath));
	}

	// Check dest doesn't exist
	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(DestPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *DestPath));
	}

	UObject* DuplicatedObj = UEditorAssetLibrary::DuplicateAsset(SourcePath, DestPath);
	if (!DuplicatedObj)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to duplicate '%s' to '%s'"), *SourcePath, *DestPath));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("source_path"), SourcePath);
	Root->SetStringField(TEXT("dest_path"), DuplicatedObj->GetPathName());
	Root->SetStringField(TEXT("asset_class"), DuplicatedObj->GetClass()->GetName());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleCreateMontage(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SkeletonPath = Params->GetStringField(TEXT("skeleton_path"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(SkeletonPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath));

	// Extract asset name from path
	FString AssetName;
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	}
	AssetName = AssetPath.Mid(LastSlash + 1);

	// Check if asset already exists
	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));
	}

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));
	}

	UAnimMontage* Montage = NewObject<UAnimMontage>(Pkg, FName(*AssetName), RF_Public | RF_Standalone);
	if (!Montage)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UAnimMontage object"));
	}

	Montage->SetSkeleton(Skeleton);

	// Add default slot if none exists
	if (Montage->SlotAnimTracks.Num() == 0)
	{
		Montage->AddSlot(FName(TEXT("DefaultSlot")));
	}

	// Add default section at time 0
	Montage->AddAnimCompositeSection(FName(TEXT("Default")), 0.0f);

	FAssetRegistryModule::AssetCreated(Montage);
	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	Root->SetNumberField(TEXT("slot_count"), Montage->SlotAnimTracks.Num());
	Root->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetMontageBlend(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimMontage* Montage = FMonolithAssetUtils::LoadAssetByPath<UAnimMontage>(AssetPath);
	if (!Montage) return FMonolithActionResult::Error(FString::Printf(TEXT("Montage not found: %s"), *AssetPath));

	bool bAnySet = false;

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Montage Blend")));
	Montage->Modify();

	if (Params->HasField(TEXT("blend_in_time")))
	{
		Montage->BlendIn.SetBlendTime(static_cast<float>(Params->GetNumberField(TEXT("blend_in_time"))));
		bAnySet = true;
	}
	if (Params->HasField(TEXT("blend_out_time")))
	{
		Montage->BlendOut.SetBlendTime(static_cast<float>(Params->GetNumberField(TEXT("blend_out_time"))));
		bAnySet = true;
	}
	if (Params->HasField(TEXT("blend_out_trigger_time")))
	{
		Montage->BlendOutTriggerTime = static_cast<float>(Params->GetNumberField(TEXT("blend_out_trigger_time")));
		bAnySet = true;
	}
	if (Params->HasField(TEXT("enable_auto_blend_out")))
	{
		Montage->bEnableAutoBlendOut = Params->GetBoolField(TEXT("enable_auto_blend_out"));
		bAnySet = true;
	}

	if (!bAnySet)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("At least one of blend_in_time, blend_out_time, blend_out_trigger_time, or enable_auto_blend_out must be provided"));
	}

	GEditor->EndTransaction();
	Montage->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("blend_in_time"), Montage->BlendIn.GetBlendTime());
	Root->SetNumberField(TEXT("blend_out_time"), Montage->BlendOut.GetBlendTime());
	Root->SetNumberField(TEXT("blend_out_trigger_time"), Montage->BlendOutTriggerTime);
	Root->SetBoolField(TEXT("enable_auto_blend_out"), Montage->bEnableAutoBlendOut);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleAddMontageSlot(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SlotName = Params->GetStringField(TEXT("slot_name"));

	UAnimMontage* Montage = FMonolithAssetUtils::LoadAssetByPath<UAnimMontage>(AssetPath);
	if (!Montage) return FMonolithActionResult::Error(FString::Printf(TEXT("Montage not found: %s"), *AssetPath));

	// Check if slot already exists
	for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
	{
		if (Track.SlotName == FName(*SlotName))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Slot '%s' already exists on montage"), *SlotName));
		}
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Add Montage Slot")));
	Montage->Modify();

	Montage->AddSlot(FName(*SlotName));

	GEditor->EndTransaction();
	Montage->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("slot_name"), SlotName);
	Root->SetNumberField(TEXT("slot_count"), Montage->SlotAnimTracks.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetMontageSlot(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	int32 SlotIndex = static_cast<int32>(Params->GetNumberField(TEXT("slot_index")));
	FString SlotName = Params->GetStringField(TEXT("slot_name"));

	UAnimMontage* Montage = FMonolithAssetUtils::LoadAssetByPath<UAnimMontage>(AssetPath);
	if (!Montage) return FMonolithActionResult::Error(FString::Printf(TEXT("Montage not found: %s"), *AssetPath));

	if (SlotIndex < 0 || SlotIndex >= Montage->SlotAnimTracks.Num())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid slot index %d (montage has %d slots)"), SlotIndex, Montage->SlotAnimTracks.Num()));
	}

	FName OldName = Montage->SlotAnimTracks[SlotIndex].SlotName;

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Montage Slot")));
	Montage->Modify();
	Montage->SlotAnimTracks[SlotIndex].SlotName = FName(*SlotName);
	GEditor->EndTransaction();
	Montage->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("slot_index"), SlotIndex);
	Root->SetStringField(TEXT("old_slot_name"), OldName.ToString());
	Root->SetStringField(TEXT("new_slot_name"), SlotName);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Wave 7 — Anim Modifiers + Composites
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleApplyAnimModifier(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ModifierClass = Params->GetStringField(TEXT("modifier_class"));

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	// Try to find the class — with and without U prefix
	UClass* ModifierUClass = FindFirstObject<UClass>(*ModifierClass, EFindFirstObjectOptions::NativeFirst);
	if (!ModifierUClass && !ModifierClass.StartsWith(TEXT("U")))
	{
		ModifierUClass = FindFirstObject<UClass>(*(TEXT("U") + ModifierClass), EFindFirstObjectOptions::NativeFirst);
	}
	if (!ModifierUClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Modifier class not found: %s"), *ModifierClass));
	}

	if (!ModifierUClass->IsChildOf(UAnimationModifier::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Class '%s' is not a UAnimationModifier subclass"), *ModifierClass));
	}

	// Optional reflective property set + persistence (T1-L3 ALT path).
	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	const bool bHasProperties = Params->TryGetObjectField(TEXT("properties"), PropertiesObj)
		&& PropertiesObj && PropertiesObj->IsValid();
	bool bPersist = false;
	Params->TryGetBoolField(TEXT("persist"), bPersist);

	// Helper: set each requested property on a modifier instance via reflection.
	// Returns the count successfully applied; collects unresolved keys for the echo.
	auto ApplyProperties = [](UAnimationModifier* Mod, const TSharedPtr<FJsonObject>& Props,
		TArray<TSharedPtr<FJsonValue>>& OutUnresolved) -> int32
	{
		int32 Applied = 0;
		for (const auto& Pair : Props->Values)
		{
			const FString PairKeyStr = MonolithKeyToString(Pair.Key);
			FProperty* Prop = Mod->GetClass()->FindPropertyByName(FName(*PairKeyStr));
			if (!Prop || !Pair.Value.IsValid())
			{
				OutUnresolved.Add(MakeShared<FJsonValueString>(PairKeyStr));
				continue;
			}
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Mod);
			bool bSet = false;
			if (FBoolProperty* BoolP = CastField<FBoolProperty>(Prop))
			{
				BoolP->SetPropertyValue(ValuePtr, Pair.Value->AsBool()); bSet = true;
			}
			else if (FNumericProperty* NumP = CastField<FNumericProperty>(Prop))
			{
				// Enum-backed numeric: accept the enumerator NAME (string) — gotcha 3,
				// never trust raw int order for EDistanceCurve_Axis-style enums.
				if (UEnum* Enum = NumP->GetIntPropertyEnum())
				{
					FString EnumStr; double EnumNum = 0.0;
					if (Pair.Value->TryGetString(EnumStr))
					{
						const int64 Val = Enum->GetValueByNameString(EnumStr);
						if (Val != INDEX_NONE) { NumP->SetIntPropertyValue(ValuePtr, Val); bSet = true; }
					}
					else if (Pair.Value->TryGetNumber(EnumNum))
					{
						NumP->SetIntPropertyValue(ValuePtr, static_cast<int64>(EnumNum)); bSet = true;
					}
				}
				else if (NumP->IsFloatingPoint())
				{
					NumP->SetFloatingPointPropertyValue(ValuePtr, Pair.Value->AsNumber()); bSet = true;
				}
				else
				{
					NumP->SetIntPropertyValue(ValuePtr, static_cast<int64>(Pair.Value->AsNumber())); bSet = true;
				}
			}
			else if (FEnumProperty* EnumP = CastField<FEnumProperty>(Prop))
			{
				UEnum* Enum = EnumP->GetEnum();
				FString EnumStr;
				if (Enum && Pair.Value->TryGetString(EnumStr))
				{
					const int64 Val = Enum->GetValueByNameString(EnumStr);
					if (Val != INDEX_NONE)
					{
						EnumP->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, Val); bSet = true;
					}
				}
			}
			else if (FStrProperty* StrP = CastField<FStrProperty>(Prop))
			{
				StrP->SetPropertyValue(ValuePtr, Pair.Value->AsString()); bSet = true;
			}
			else if (FNameProperty* NameP = CastField<FNameProperty>(Prop))
			{
				NameP->SetPropertyValue(ValuePtr, FName(*Pair.Value->AsString())); bSet = true;
			}
			else
			{
				// Fallback: import the JSON value as text (covers structs / arrays).
				FString AsText;
				if (Pair.Value->TryGetString(AsText) &&
					Prop->ImportText_Direct(*AsText, ValuePtr, Mod, PPF_None) != nullptr)
				{
					bSet = true;
				}
			}

			if (bSet) ++Applied;
			else OutUnresolved.Add(MakeShared<FJsonValueString>(PairKeyStr));
		}
		return Applied;
	};

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("modifier_class"), ModifierUClass->GetName());
	TArray<TSharedPtr<FJsonValue>> Unresolved;
	int32 AppliedProps = 0;

	if (bPersist)
	{
		// PERSIST PATH (gotcha 7 MUST-PROVE): register the modifier in the
		// AnimationModifiersAssetUserData stack (the same stack list_anim_modifiers
		// reads) so it survives save/reload, THEN set properties + apply. The public
		// static creates + registers the instance and the owning user-data object.
		if (!UAnimationModifiersAssetUserData::AddAnimationModifierOfClass(Seq, ModifierUClass))
		{
			return FMonolithActionResult::Error(TEXT("Failed to register modifier in the AnimationModifiers stack"));
		}

		// Recover the freshly-registered instance: locate the user-data object on the
		// sequence and take the last instance of the requested class (it was Add()ed last).
		UAnimationModifier* Registered = nullptr;
		if (const TArray<UAssetUserData*>* UserDataArray = Seq->GetAssetUserDataArray())
		{
			for (UAssetUserData* UserData : *UserDataArray)
			{
				UAnimationModifiersAssetUserData* ModData = Cast<UAnimationModifiersAssetUserData>(UserData);
				if (!ModData) continue;
				const TArray<UAnimationModifier*>& Instances = ModData->GetAnimationModifierInstances();
				for (int32 i = Instances.Num() - 1; i >= 0; --i)
				{
					if (Instances[i] && Instances[i]->GetClass() == ModifierUClass)
					{
						Registered = Instances[i];
						break;
					}
				}
				if (Registered) break;
			}
		}

		if (!Registered)
		{
			return FMonolithActionResult::Error(TEXT("Modifier registered but could not be recovered from the stack for apply"));
		}

		if (bHasProperties)
		{
			Registered->Modify();
			AppliedProps = ApplyProperties(Registered, *PropertiesObj, Unresolved);
		}
		Registered->ApplyToAnimationSequence(Seq);
		Root->SetBoolField(TEXT("persisted"), true);
		Root->SetStringField(TEXT("modifier_name"), Registered->GetName());
	}
	else
	{
		// LEGACY (default) PATH — transient instance, non-persistent. Backward-compatible.
		UAnimationModifier* Modifier = NewObject<UAnimationModifier>(GetTransientPackage(), ModifierUClass);
		if (!Modifier)
		{
			return FMonolithActionResult::Error(TEXT("Failed to create modifier instance"));
		}
		if (bHasProperties)
		{
			AppliedProps = ApplyProperties(Modifier, *PropertiesObj, Unresolved);
		}
		Modifier->ApplyToAnimationSequence(Seq);
		Root->SetBoolField(TEXT("persisted"), false);
	}

	Seq->MarkPackageDirty();

	if (bHasProperties)
	{
		Root->SetNumberField(TEXT("properties_applied"), AppliedProps);
		if (Unresolved.Num() > 0) Root->SetArrayField(TEXT("properties_unresolved"), Unresolved);
	}
	Root->SetStringField(TEXT("status"), TEXT("applied"));
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleListAnimModifiers(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	TArray<TSharedPtr<FJsonValue>> ModArray;

	// Iterate asset user data to find animation modifiers
	const TArray<UAssetUserData*>* UserDataArray = Seq->GetAssetUserDataArray();
	if (UserDataArray)
	{
		for (UAssetUserData* UserData : *UserDataArray)
		{
			if (!UserData) continue;

			// Check if this is the AnimationModifiersAssetUserData
			if (UserData->GetClass()->GetName().Contains(TEXT("AnimationModifiersAssetUserData")))
			{
				// Use reflection to get the modifiers array
				FArrayProperty* ModifiersProp = CastField<FArrayProperty>(UserData->GetClass()->FindPropertyByName(TEXT("AnimationModifierInstances")));
				if (ModifiersProp)
				{
					FScriptArrayHelper ArrayHelper(ModifiersProp, ModifiersProp->ContainerPtrToValuePtr<void>(UserData));
					FObjectProperty* InnerProp = CastField<FObjectProperty>(ModifiersProp->Inner);
					if (InnerProp)
					{
						for (int32 i = 0; i < ArrayHelper.Num(); ++i)
						{
							UObject* ModObj = InnerProp->GetObjectPropertyValue(ArrayHelper.GetElementPtr(i));
							if (ModObj)
							{
								TSharedPtr<FJsonObject> ModJson = MakeShared<FJsonObject>();
								ModJson->SetNumberField(TEXT("index"), i);
								ModJson->SetStringField(TEXT("class"), ModObj->GetClass()->GetName());
								ModJson->SetStringField(TEXT("name"), ModObj->GetName());
								ModArray.Add(MakeShared<FJsonValueObject>(ModJson));
							}
						}
					}
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetArrayField(TEXT("modifiers"), ModArray);
	Root->SetNumberField(TEXT("count"), ModArray.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetCompositeInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimComposite* Composite = FMonolithAssetUtils::LoadAssetByPath<UAnimComposite>(AssetPath);
	if (!Composite) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimComposite not found: %s"), *AssetPath));

	TArray<TSharedPtr<FJsonValue>> SegArray;
	const TArray<FAnimSegment>& Segments = Composite->AnimationTrack.AnimSegments;
	for (int32 i = 0; i < Segments.Num(); ++i)
	{
		const FAnimSegment& Seg = Segments[i];
		TSharedPtr<FJsonObject> SegJson = MakeShared<FJsonObject>();
		SegJson->SetNumberField(TEXT("index"), i);

		UAnimSequenceBase* AnimRef = Seg.GetAnimReference();
		SegJson->SetStringField(TEXT("anim_reference"), AnimRef ? AnimRef->GetPathName() : TEXT("None"));
		SegJson->SetNumberField(TEXT("start_pos"), Seg.StartPos);
		SegJson->SetNumberField(TEXT("anim_start_time"), Seg.AnimStartTime);
		SegJson->SetNumberField(TEXT("anim_end_time"), Seg.AnimEndTime);
		SegJson->SetNumberField(TEXT("anim_play_rate"), Seg.AnimPlayRate);
		SegJson->SetNumberField(TEXT("looping_count"), Seg.LoopingCount);

		SegArray.Add(MakeShared<FJsonValueObject>(SegJson));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("skeleton"), Composite->GetSkeleton() ? Composite->GetSkeleton()->GetPathName() : TEXT("None"));
	Root->SetNumberField(TEXT("duration"), Composite->GetPlayLength());
	Root->SetArrayField(TEXT("segments"), SegArray);
	Root->SetNumberField(TEXT("segment_count"), Segments.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleAddCompositeSegment(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString AnimPath = Params->GetStringField(TEXT("anim_path"));
	float StartPos = Params->HasField(TEXT("start_pos")) ? static_cast<float>(Params->GetNumberField(TEXT("start_pos"))) : 0.0f;
	float PlayRate = Params->HasField(TEXT("play_rate")) ? static_cast<float>(Params->GetNumberField(TEXT("play_rate"))) : 1.0f;
	int32 LoopingCount = Params->HasField(TEXT("looping_count")) ? static_cast<int32>(Params->GetNumberField(TEXT("looping_count"))) : 1;

	UAnimComposite* Composite = FMonolithAssetUtils::LoadAssetByPath<UAnimComposite>(AssetPath);
	if (!Composite) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimComposite not found: %s"), *AssetPath));

	UAnimSequenceBase* Anim = FMonolithAssetUtils::LoadAssetByPath<UAnimSequenceBase>(AnimPath);
	if (!Anim) return FMonolithActionResult::Error(FString::Printf(TEXT("Animation not found: %s"), *AnimPath));

	GEditor->BeginTransaction(FText::FromString(TEXT("Add Composite Segment")));
	Composite->Modify();

	FAnimSegment NewSeg;
	NewSeg.SetAnimReference(Anim);
	NewSeg.StartPos = StartPos;
	NewSeg.AnimStartTime = 0.0f;
	NewSeg.AnimEndTime = Anim->GetPlayLength();
	NewSeg.AnimPlayRate = PlayRate;
	NewSeg.LoopingCount = LoopingCount;

	int32 NewIndex = Composite->AnimationTrack.AnimSegments.Add(NewSeg);

	GEditor->EndTransaction();
	Composite->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("segment_index"), NewIndex);
	Root->SetStringField(TEXT("anim_reference"), AnimPath);
	Root->SetNumberField(TEXT("start_pos"), StartPos);
	Root->SetNumberField(TEXT("play_rate"), PlayRate);
	Root->SetNumberField(TEXT("looping_count"), LoopingCount);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleRemoveCompositeSegment(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	int32 SegmentIndex = static_cast<int32>(Params->GetNumberField(TEXT("segment_index")));

	UAnimComposite* Composite = FMonolithAssetUtils::LoadAssetByPath<UAnimComposite>(AssetPath);
	if (!Composite) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimComposite not found: %s"), *AssetPath));

	TArray<FAnimSegment>& Segments = Composite->AnimationTrack.AnimSegments;
	if (SegmentIndex < 0 || SegmentIndex >= Segments.Num())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid segment index %d (composite has %d segments)"), SegmentIndex, Segments.Num()));
	}

	// Capture info before removal
	UAnimSequenceBase* AnimRef = Segments[SegmentIndex].GetAnimReference();
	FString AnimName = AnimRef ? AnimRef->GetPathName() : TEXT("None");

	GEditor->BeginTransaction(FText::FromString(TEXT("Remove Composite Segment")));
	Composite->Modify();

	Segments.RemoveAt(SegmentIndex);

	GEditor->EndTransaction();
	Composite->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("removed_index"), SegmentIndex);
	Root->SetStringField(TEXT("removed_anim"), AnimName);
	Root->SetNumberField(TEXT("remaining_segments"), Segments.Num());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Wave 9 — ABP Read Enhancements
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleGetAbpVariables(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	TArray<TSharedPtr<FJsonValue>> VarsArr;
	for (const FBPVariableDescription& Var : ABP->NewVariables)
	{
		TSharedPtr<FJsonObject> VarJson = MakeShared<FJsonObject>();
		VarJson->SetStringField(TEXT("name"), Var.VarName.ToString());

		FString TypeStr = Var.VarType.PinCategory.ToString();
		const FString PinCat = TypeStr;
		if ((PinCat == TEXT("object") || PinCat == TEXT("struct")) && Var.VarType.PinSubCategoryObject.IsValid())
		{
			TypeStr = FString::Printf(TEXT("%s:%s"), *PinCat, *Var.VarType.PinSubCategoryObject->GetName());
		}
		VarJson->SetStringField(TEXT("type"), TypeStr);
		VarJson->SetStringField(TEXT("default_value"), Var.DefaultValue);
		VarJson->SetStringField(TEXT("category"), Var.Category.ToString());
		VarJson->SetBoolField(TEXT("blueprint_visible"), (Var.PropertyFlags & CPF_BlueprintVisible) != 0);
		VarJson->SetBoolField(TEXT("edit_instance"), (Var.PropertyFlags & CPF_DisableEditOnInstance) == 0);
		VarsArr.Add(MakeShared<FJsonValueObject>(VarJson));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetArrayField(TEXT("variables"), VarsArr);
	Root->SetNumberField(TEXT("count"), VarsArr.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetAbpLinkedAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	FName PackageName = FName(*ABP->GetPackage()->GetName());

	TArray<FAssetIdentifier> Deps;
	AR.GetDependencies(FAssetIdentifier(PackageName), Deps, UE::AssetRegistry::EDependencyCategory::Package);

	TArray<TSharedPtr<FJsonValue>> SequencesArr;
	TArray<TSharedPtr<FJsonValue>> MontagesArr;
	TArray<TSharedPtr<FJsonValue>> BlendSpacesArr;
	TArray<TSharedPtr<FJsonValue>> CompositesArr;
	TArray<TSharedPtr<FJsonValue>> LinkedAbpArr;

	for (const FAssetIdentifier& Dep : Deps)
	{
		TArray<FAssetData> PackageAssets;
		AR.GetAssetsByPackageName(Dep.PackageName, PackageAssets);
		for (const FAssetData& DepData : PackageAssets)
		{
			if (!DepData.IsValid()) continue;

			FString ClassName = DepData.AssetClassPath.GetAssetName().ToString();
			FString DepPath = DepData.GetObjectPathString();

			if (ClassName == TEXT("AnimSequence"))
			{
				SequencesArr.Add(MakeShared<FJsonValueString>(DepPath));
			}
			else if (ClassName == TEXT("AnimMontage"))
			{
				MontagesArr.Add(MakeShared<FJsonValueString>(DepPath));
			}
			else if (ClassName == TEXT("BlendSpace") || ClassName == TEXT("BlendSpace1D") ||
			         ClassName == TEXT("AimOffsetBlendSpace") || ClassName == TEXT("AimOffsetBlendSpace1D"))
			{
				BlendSpacesArr.Add(MakeShared<FJsonValueString>(DepPath));
			}
			else if (ClassName == TEXT("AnimComposite"))
			{
				CompositesArr.Add(MakeShared<FJsonValueString>(DepPath));
			}
			else if (ClassName == TEXT("AnimBlueprint"))
			{
				LinkedAbpArr.Add(MakeShared<FJsonValueString>(DepPath));
			}
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetArrayField(TEXT("sequences"), SequencesArr);
	Root->SetArrayField(TEXT("montages"), MontagesArr);
	Root->SetArrayField(TEXT("blend_spaces"), BlendSpacesArr);
	Root->SetArrayField(TEXT("composites"), CompositesArr);
	Root->SetArrayField(TEXT("linked_anim_blueprints"), LinkedAbpArr);
	Root->SetNumberField(TEXT("total_dependencies"), Deps.Num());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Skeleton Compatibility — wraps USkeleton::CompatibleSkeletons
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleGetCompatibleSkeletons(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(AssetPath);
	if (!Skeleton)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	TArray<TSharedPtr<FJsonValue>> CompatArr;
	for (const TSoftObjectPtr<USkeleton>& Compat : Skeleton->GetCompatibleSkeletons())
	{
		const FString CompatPath = Compat.ToSoftObjectPath().ToString();
		if (!CompatPath.IsEmpty())
			CompatArr.Add(MakeShared<FJsonValueString>(CompatPath));
	}

	Root->SetArrayField(TEXT("compatible_skeletons"), CompatArr);
	Root->SetNumberField(TEXT("count"), CompatArr.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleAddCompatibleSkeleton(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString CompatPath = Params->GetStringField(TEXT("compatible_with"));
	bool bSave = true;
	Params->TryGetBoolField(TEXT("save"), bSave);

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(AssetPath);
	if (!Skeleton)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *AssetPath));

	USkeleton* Compat = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(CompatPath);
	if (!Compat)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Compatible Skeleton not found: %s"), *CompatPath));

	if (Skeleton == Compat)
		return FMonolithActionResult::Error(TEXT("Cannot mark a skeleton compatible with itself"));

	// Idempotency: skip if already present.
	bool bAlreadyCompatible = false;
	for (const TSoftObjectPtr<USkeleton>& Existing : Skeleton->GetCompatibleSkeletons())
	{
		if (Existing.Get() == Compat)
		{
			bAlreadyCompatible = true;
			break;
		}
	}

	if (!bAlreadyCompatible)
	{
		Skeleton->AddCompatibleSkeleton(Compat);
		Skeleton->MarkPackageDirty();
		if (bSave)
		{
			UEditorAssetLibrary::SaveAsset(AssetPath, /*bOnlyIfIsDirty*/ false);
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("compatible_with"), CompatPath);
	Root->SetBoolField(TEXT("added"), !bAlreadyCompatible);
	Root->SetBoolField(TEXT("already_compatible"), bAlreadyCompatible);
	Root->SetNumberField(TEXT("count"), Skeleton->GetCompatibleSkeletons().Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleRemoveCompatibleSkeleton(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString CompatPath = Params->GetStringField(TEXT("compatible_with"));
	bool bSave = true;
	Params->TryGetBoolField(TEXT("save"), bSave);

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(AssetPath);
	if (!Skeleton)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *AssetPath));

	// USkeleton::RemoveCompatibleSkeleton() exists in 5.7+.
	USkeleton* Compat = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(CompatPath);
	if (!Compat)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Compatible Skeleton not found: %s"), *CompatPath));

	bool bWasCompatible = false;
	for (const TSoftObjectPtr<USkeleton>& Existing : Skeleton->GetCompatibleSkeletons())
	{
		if (Existing.Get() == Compat)
		{
			bWasCompatible = true;
			break;
		}
	}

	bool bRemoved = false;
	if (bWasCompatible)
	{
		Skeleton->RemoveCompatibleSkeleton(Compat);
		Skeleton->MarkPackageDirty();
		bRemoved = true;
		if (bSave)
		{
			UEditorAssetLibrary::SaveAsset(AssetPath, /*bOnlyIfIsDirty*/ false);
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("compatible_with"), CompatPath);
	Root->SetBoolField(TEXT("removed"), bRemoved);
	Root->SetBoolField(TEXT("was_compatible"), bWasCompatible);
	Root->SetNumberField(TEXT("count"), Skeleton->GetCompatibleSkeletons().Num());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Wave 8b — Control Rig Read
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleGetControlRigInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UControlRigBlueprint* CRB = FMonolithAssetUtils::LoadAssetByPath<UControlRigBlueprint>(AssetPath);
	if (!CRB) return FMonolithActionResult::Error(FString::Printf(TEXT("ControlRigBlueprint not found: %s"), *AssetPath));

	URigHierarchy* H = CRB->Hierarchy;
	if (!H) return FMonolithActionResult::Error(TEXT("ControlRigBlueprint has no hierarchy"));

	// Optional element type filter
	FString FilterStr;
	bool bHasFilter = Params->TryGetStringField(TEXT("element_type"), FilterStr);
	FilterStr.ToLowerInline();

	auto TypeMatchesFilter = [&](ERigElementType Type) -> bool
	{
		if (!bHasFilter || FilterStr.IsEmpty() || FilterStr == TEXT("all")) return true;
		if (FilterStr == TEXT("bone"))    return Type == ERigElementType::Bone;
		if (FilterStr == TEXT("control")) return Type == ERigElementType::Control;
		if (FilterStr == TEXT("null"))    return Type == ERigElementType::Null;
		if (FilterStr == TEXT("curve"))   return Type == ERigElementType::Curve;
		return true;
	};

	TArray<FRigElementKey> AllKeys = H->GetAllKeys();

	TArray<TSharedPtr<FJsonValue>> ElementsArr;
	int32 BoneCount = 0, ControlCount = 0, NullCount = 0, CurveCount = 0, OtherCount = 0;
	for (const FRigElementKey& Key : AllKeys)
	{
		if (!TypeMatchesFilter(Key.Type)) continue;

		switch (Key.Type)
		{
		case ERigElementType::Bone:    ++BoneCount;    break;
		case ERigElementType::Control: ++ControlCount; break;
		case ERigElementType::Null:    ++NullCount;    break;
		case ERigElementType::Curve:   ++CurveCount;   break;
		default:                       ++OtherCount;   break;
		}

		TSharedPtr<FJsonObject> ElemObj = MakeShared<FJsonObject>();
		ElemObj->SetStringField(TEXT("name"), Key.Name.ToString());

		// Type string
		FString TypeStr = StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(Key.Type));
		ElemObj->SetStringField(TEXT("type"), TypeStr);

		// Parent
		FRigElementKey ParentKey = H->GetFirstParent(Key);
		ElemObj->SetStringField(TEXT("parent"), ParentKey.IsValid() ? ParentKey.Name.ToString() : TEXT(""));

		// Control-specific info
		if (Key.Type == ERigElementType::Control)
		{
			FRigControlElement* CE = H->Find<FRigControlElement>(Key);
			if (CE)
			{
				FString ControlTypeStr = StaticEnum<ERigControlType>()->GetNameStringByValue(static_cast<int64>(CE->Settings.ControlType));
				ElemObj->SetStringField(TEXT("control_type"), ControlTypeStr);
				ElemObj->SetBoolField(TEXT("animatable"), CE->Settings.IsAnimatable());
				ElemObj->SetStringField(TEXT("display_name"), CE->Settings.DisplayName.IsNone() ? Key.Name.ToString() : CE->Settings.DisplayName.ToString());
			}
		}

		ElementsArr.Add(MakeShared<FJsonValueObject>(ElemObj));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("total_elements"), ElementsArr.Num());
	TSharedPtr<FJsonObject> CountsObj = MakeShared<FJsonObject>();
	CountsObj->SetNumberField(TEXT("bones"), BoneCount);
	CountsObj->SetNumberField(TEXT("controls"), ControlCount);
	CountsObj->SetNumberField(TEXT("nulls"), NullCount);
	CountsObj->SetNumberField(TEXT("curves"), CurveCount);
	CountsObj->SetNumberField(TEXT("other"), OtherCount);
	Root->SetObjectField(TEXT("counts"), CountsObj);
	if (bHasFilter && !FilterStr.IsEmpty())
		Root->SetStringField(TEXT("filter"), FilterStr);
	Root->SetArrayField(TEXT("elements"), ElementsArr);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetControlRigVariables(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UControlRigBlueprint* CRB = FMonolithAssetUtils::LoadAssetByPath<UControlRigBlueprint>(AssetPath);
	if (!CRB) return FMonolithActionResult::Error(FString::Printf(TEXT("ControlRigBlueprint not found: %s"), *AssetPath));

	// Part 1 — Animatable controls from hierarchy
	TArray<TSharedPtr<FJsonValue>> AnimControlsArr;
	URigHierarchy* H = CRB->Hierarchy;
	if (H)
	{
		TArray<FRigElementKey> AllKeys = H->GetAllKeys();
		for (const FRigElementKey& Key : AllKeys)
		{
			if (Key.Type != ERigElementType::Control) continue;

			FRigControlElement* CE = H->Find<FRigControlElement>(Key);
			if (!CE || !CE->Settings.IsAnimatable()) continue;

			TSharedPtr<FJsonObject> CtrlObj = MakeShared<FJsonObject>();
			CtrlObj->SetStringField(TEXT("name"), Key.Name.ToString());

			FString ControlTypeStr = StaticEnum<ERigControlType>()->GetNameStringByValue(static_cast<int64>(CE->Settings.ControlType));
			CtrlObj->SetStringField(TEXT("control_type"), ControlTypeStr);

			FString AnimTypeStr = StaticEnum<ERigControlAnimationType>()->GetNameStringByValue(static_cast<int64>(CE->Settings.AnimationType));
			CtrlObj->SetStringField(TEXT("animation_type"), AnimTypeStr);

			CtrlObj->SetStringField(TEXT("display_name"), CE->Settings.DisplayName.IsNone() ? Key.Name.ToString() : CE->Settings.DisplayName.ToString());

			FRigElementKey ParentKey = H->GetFirstParent(Key);
			CtrlObj->SetStringField(TEXT("parent"), ParentKey.IsValid() ? ParentKey.Name.ToString() : TEXT(""));

			AnimControlsArr.Add(MakeShared<FJsonValueObject>(CtrlObj));
		}
	}

	// Part 2 — Blueprint variables (from UBlueprint::NewVariables)
	TArray<TSharedPtr<FJsonValue>> BpVarsArr;
	for (const FBPVariableDescription& Var : CRB->NewVariables)
	{
		TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), Var.VarName.ToString());
		VarObj->SetStringField(TEXT("type"), Var.VarType.PinCategory.ToString());
		VarObj->SetStringField(TEXT("default_value"), Var.DefaultValue);
		VarObj->SetStringField(TEXT("category"), Var.Category.ToString());
		VarObj->SetBoolField(TEXT("blueprint_visible"), (Var.PropertyFlags & CPF_BlueprintVisible) != 0);
		BpVarsArr.Add(MakeShared<FJsonValueObject>(VarObj));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetArrayField(TEXT("animatable_controls"), AnimControlsArr);
	Root->SetNumberField(TEXT("animatable_control_count"), AnimControlsArr.Num());
	Root->SetArrayField(TEXT("blueprint_variables"), BpVarsArr);
	Root->SetNumberField(TEXT("blueprint_variable_count"), BpVarsArr.Num());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Wave 8c — Control Rig Write
// ---------------------------------------------------------------------------

static ERigElementType ParseRigElementType(const FString& Str)
{
	if (Str.Equals(TEXT("bone"),    ESearchCase::IgnoreCase)) return ERigElementType::Bone;
	if (Str.Equals(TEXT("control"), ESearchCase::IgnoreCase)) return ERigElementType::Control;
	if (Str.Equals(TEXT("null"),    ESearchCase::IgnoreCase)) return ERigElementType::Null;
	if (Str.Equals(TEXT("curve"),   ESearchCase::IgnoreCase)) return ERigElementType::Curve;
	return ERigElementType::Bone;
}

static ERigControlType ParseRigControlType(const FString& Str)
{
	if (Str.Equals(TEXT("Float"),      ESearchCase::IgnoreCase)) return ERigControlType::Float;
	if (Str.Equals(TEXT("Integer"),    ESearchCase::IgnoreCase)) return ERigControlType::Integer;
	if (Str.Equals(TEXT("Bool"),       ESearchCase::IgnoreCase)) return ERigControlType::Bool;
	if (Str.Equals(TEXT("Transform"),  ESearchCase::IgnoreCase)) return ERigControlType::Transform;
	if (Str.Equals(TEXT("Rotator"),    ESearchCase::IgnoreCase)) return ERigControlType::Rotator;
	if (Str.Equals(TEXT("Position"),   ESearchCase::IgnoreCase)) return ERigControlType::Position;
	if (Str.Equals(TEXT("Scale"),      ESearchCase::IgnoreCase)) return ERigControlType::Scale;
	if (Str.Equals(TEXT("ScaleFloat"), ESearchCase::IgnoreCase)) return ERigControlType::ScaleFloat;
	if (Str.Equals(TEXT("Vector2D"),   ESearchCase::IgnoreCase)) return ERigControlType::Vector2D;
	return ERigControlType::Transform;
}

FMonolithActionResult FMonolithAnimationActions::HandleAddControlRigElement(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath    = Params->GetStringField(TEXT("asset_path"));
	FString ElementTypeStr = Params->GetStringField(TEXT("element_type"));
	FString Name         = Params->GetStringField(TEXT("name"));

	if (Name.IsEmpty())
		return FMonolithActionResult::Error(TEXT("name must not be empty"));

	UControlRigBlueprint* CRB = FMonolithAssetUtils::LoadAssetByPath<UControlRigBlueprint>(AssetPath);
	if (!CRB) return FMonolithActionResult::Error(FString::Printf(TEXT("ControlRigBlueprint not found: %s"), *AssetPath));

	URigHierarchyController* HC = CRB->GetHierarchyController();
	if (!HC) return FMonolithActionResult::Error(TEXT("Failed to get hierarchy controller"));

	// Parse element type
	ElementTypeStr.ToLowerInline();
	if (ElementTypeStr != TEXT("bone") && ElementTypeStr != TEXT("control") && ElementTypeStr != TEXT("null"))
		return FMonolithActionResult::Error(TEXT("Invalid element_type — use bone, control, or null"));

	ERigElementType ElemType = ParseRigElementType(ElementTypeStr);

	// Parse parent (optional)
	FRigElementKey ParentKey;
	FString ParentName;
	if (Params->TryGetStringField(TEXT("parent"), ParentName) && !ParentName.IsEmpty())
	{
		FString ParentTypeStr;
		ERigElementType ParentElemType = ERigElementType::Bone;
		if (Params->TryGetStringField(TEXT("parent_type"), ParentTypeStr) && !ParentTypeStr.IsEmpty())
			ParentElemType = ParseRigElementType(ParentTypeStr);

		ParentKey = FRigElementKey(FName(*ParentName), ParentElemType);

		// Validate parent exists
		URigHierarchy* H = CRB->Hierarchy;
		if (!H || !H->Find(ParentKey))
			return FMonolithActionResult::Error(FString::Printf(TEXT("Parent element not found: %s"), *ParentName));
	}

	// Parse optional transform {tx, ty, tz, rx, ry, rz}
	FTransform Xform = FTransform::Identity;
	const TSharedPtr<FJsonObject>* XformObj = nullptr;
	if (Params->TryGetObjectField(TEXT("transform"), XformObj) && XformObj && (*XformObj)->Values.Num() > 0)
	{
		double TX = 0, TY = 0, TZ = 0, RX = 0, RY = 0, RZ = 0;
		(*XformObj)->TryGetNumberField(TEXT("tx"), TX);
		(*XformObj)->TryGetNumberField(TEXT("ty"), TY);
		(*XformObj)->TryGetNumberField(TEXT("tz"), TZ);
		(*XformObj)->TryGetNumberField(TEXT("rx"), RX);
		(*XformObj)->TryGetNumberField(TEXT("ry"), RY);
		(*XformObj)->TryGetNumberField(TEXT("rz"), RZ);
		Xform.SetTranslation(FVector(TX, TY, TZ));
		Xform.SetRotation(FQuat(FRotator(RX, RY, RZ)));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Add Control Rig Element")));
	static_cast<UBlueprint*>(CRB)->Modify();

	FRigElementKey ResultKey;

	if (ElementTypeStr == TEXT("bone"))
	{
		ResultKey = HC->AddBone(FName(*Name), ParentKey, Xform, /*bTransformInGlobal=*/false,
			ERigBoneType::User, /*bSetupUndo=*/true, /*bPrintPythonCommand=*/false);
	}
	else if (ElementTypeStr == TEXT("null"))
	{
		ResultKey = HC->AddNull(FName(*Name), ParentKey, Xform, /*bTransformInGlobal=*/false,
			/*bSetupUndo=*/true, /*bPrintPythonCommand=*/false);
	}
	else // control
	{
		// Parse control_type
		FString ControlTypeStr;
		if (!Params->TryGetStringField(TEXT("control_type"), ControlTypeStr) || ControlTypeStr.IsEmpty())
			ControlTypeStr = TEXT("Transform");

		// Parse animatable flag
		bool bAnimatable = true;
		Params->TryGetBoolField(TEXT("animatable"), bAnimatable);

		FRigControlSettings Settings;
		Settings.ControlType = ParseRigControlType(ControlTypeStr);
		Settings.AnimationType = bAnimatable
			? ERigControlAnimationType::AnimationControl
			: ERigControlAnimationType::ProxyControl;
		Settings.DisplayName = FName(*Name);

		// Use SetFromTransform for safe type-correct value initialization.
		// GetIdentityValue() calls SetFromTransform(Identity, ControlType, PrimaryAxis)
		// and handles all storage type variants (FTransform_Float, FVector3f, float, etc.)
		FRigControlValue InitVal = Settings.GetIdentityValue();

		// If a custom transform was provided, apply it for transform-capable control types
		if (XformObj && (*XformObj)->Values.Num() > 0)
		{
			const ERigControlType CT = Settings.ControlType;
			if (CT == ERigControlType::Transform ||
				CT == ERigControlType::Position   ||
				CT == ERigControlType::Scale      ||
				CT == ERigControlType::Rotator)
			{
				InitVal.SetFromTransform(Xform, CT, Settings.PrimaryAxis);
			}
		}

		ResultKey = HC->AddControl(FName(*Name), ParentKey, Settings, InitVal,
			FTransform::Identity, FTransform::Identity,
			/*bSetupUndo=*/true, /*bPrintPythonCommand=*/false);
	}

	GEditor->EndTransaction();

	if (!ResultKey.IsValid())
		return FMonolithActionResult::Error(TEXT("Failed to create element — AddBone/AddNull/AddControl returned invalid key"));

	// Mandatory: without RequestRigVMInit the editor shows "Data missing please force a recompile"
	CRB->RequestRigVMInit();
	CRB->MarkPackageDirty();

	// Build result
	FString ResultTypeStr = StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(ResultKey.Type));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("name"),         ResultKey.Name.ToString());
	Root->SetStringField(TEXT("element_type"), ResultTypeStr);
	Root->SetStringField(TEXT("parent"),       ParentKey.IsValid() ? ParentKey.Name.ToString() : TEXT(""));
	Root->SetStringField(TEXT("asset_path"),   AssetPath);
	if (ElementTypeStr == TEXT("control"))
	{
		FString ControlTypeStr;
		Params->TryGetStringField(TEXT("control_type"), ControlTypeStr);
		if (ControlTypeStr.IsEmpty()) ControlTypeStr = TEXT("Transform");
		Root->SetStringField(TEXT("control_type"), ControlTypeStr);
	}
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Wave 8a — IKRig
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleGetIKRigInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UIKRigDefinition* Asset = FMonolithAssetUtils::LoadAssetByPath<UIKRigDefinition>(AssetPath);
	if (!Asset) return FMonolithActionResult::Error(FString::Printf(TEXT("IKRigDefinition not found: %s"), *AssetPath));

	UIKRigController* C = UIKRigController::GetController(Asset);
	if (!C) return FMonolithActionResult::Error(TEXT("Failed to get IKRigController"));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	// Preview mesh
	USkeletalMesh* PreviewMesh = Asset->GetPreviewMesh();
	Root->SetStringField(TEXT("preview_mesh"), PreviewMesh ? PreviewMesh->GetPathName() : TEXT("None"));

	// Pelvis / retarget root
	Root->SetStringField(TEXT("pelvis_bone"), C->GetRetargetRoot().ToString());

	// Skeleton
	const FIKRigSkeleton& Skel = C->GetIKRigSkeleton();
	Root->SetNumberField(TEXT("bone_count"), Skel.BoneNames.Num());

	// Solvers
	TArray<TSharedPtr<FJsonValue>> SolversArr;
	const int32 NumSolvers = C->GetNumSolvers();
	const TArray<FInstancedStruct>& SolverStructs = Asset->GetSolverStructs();
	for (int32 i = 0; i < NumSolvers; ++i)
	{
		TSharedPtr<FJsonObject> SolverObj = MakeShared<FJsonObject>();
		SolverObj->SetNumberField(TEXT("index"), i);
		SolverObj->SetBoolField(TEXT("enabled"), C->GetSolverEnabled(i));
		SolverObj->SetStringField(TEXT("start_bone"), C->GetStartBone(i).ToString());

		FString TypeName = TEXT("Unknown");
		if (SolverStructs.IsValidIndex(i) && SolverStructs[i].GetScriptStruct())
		{
			TypeName = SolverStructs[i].GetScriptStruct()->GetName();
		}
		SolverObj->SetStringField(TEXT("type"), TypeName);
		SolverObj->SetStringField(TEXT("label"), C->GetSolverUniqueName(i));

		// --- T2-1: reflective per-solver settings dump (solver-agnostic). ---
		// Each solver is a FIKRigSolverBase-derived USTRUCT stored in the FInstancedStruct
		// stack; its concrete UScriptStruct carries the solver's own settings PLUS any
		// per-bone / per-goal settings arrays (e.g. AllBoneSettings on FullBodyIK). One
		// UStructToJsonObject over the whole struct surfaces all of them reflectively,
		// regardless of solver type. The settings are read-only here; const GetMemory() is
		// sufficient. Degrades to an omitted "settings" field on any drift, never errors.
		if (SolverStructs.IsValidIndex(i))
		{
			const UScriptStruct* SolverStruct = SolverStructs[i].GetScriptStruct();
			const uint8* SolverMemory = SolverStructs[i].GetMemory();
			if (SolverStruct && SolverMemory)
			{
				TSharedRef<FJsonObject> SettingsJson = MakeShared<FJsonObject>();
				if (FJsonObjectConverter::UStructToJsonObject(SolverStruct, SolverMemory, SettingsJson, 0, 0))
				{
					SolverObj->SetObjectField(TEXT("settings"), SettingsJson);
				}
			}
		}
		SolversArr.Add(MakeShared<FJsonValueObject>(SolverObj));
	}
	Root->SetArrayField(TEXT("solvers"), SolversArr);
	Root->SetNumberField(TEXT("solver_count"), NumSolvers);

	// --- T2-1: per-bone settings summary (which bones carry solver settings / are excluded). ---
	// Walks the live skeleton bone list and reports, per bone, whether it has settings in
	// any solver (DoesBoneHaveSettings) and whether it is excluded from all solvers
	// (GetBoneExcluded). The concrete per-solver field values live in each solver's
	// reflective "settings" dump above; this is the cross-solver bone-level index.
	TArray<TSharedPtr<FJsonValue>> BoneSettingsArr;
	for (const FName& BoneName : Skel.BoneNames)
	{
		const bool bHasSettings = C->DoesBoneHaveSettings(BoneName);
		const bool bExcluded = C->GetBoneExcluded(BoneName);
		if (!bHasSettings && !bExcluded)
		{
			continue; // only surface bones that deviate from the default (clean payload)
		}
		TSharedPtr<FJsonObject> BoneObj = MakeShared<FJsonObject>();
		BoneObj->SetStringField(TEXT("bone"), BoneName.ToString());
		BoneObj->SetBoolField(TEXT("has_settings"), bHasSettings);
		BoneObj->SetBoolField(TEXT("excluded"), bExcluded);
		BoneSettingsArr.Add(MakeShared<FJsonValueObject>(BoneObj));
	}
	Root->SetArrayField(TEXT("bone_settings"), BoneSettingsArr);
	Root->SetNumberField(TEXT("bone_settings_count"), BoneSettingsArr.Num());

	// Goals
	TArray<TSharedPtr<FJsonValue>> GoalsArr;
	const TArray<UIKRigEffectorGoal*>& Goals = C->GetAllGoals();
	for (const UIKRigEffectorGoal* Goal : Goals)
	{
		if (!Goal) continue;
		TSharedPtr<FJsonObject> GoalObj = MakeShared<FJsonObject>();
		GoalObj->SetStringField(TEXT("name"), Goal->GoalName.ToString());
		GoalObj->SetStringField(TEXT("bone"), Goal->BoneName.ToString());
		GoalObj->SetBoolField(TEXT("connected"), C->IsGoalConnectedToAnySolver(Goal->GoalName));
		GoalsArr.Add(MakeShared<FJsonValueObject>(GoalObj));
	}
	Root->SetArrayField(TEXT("goals"), GoalsArr);
	Root->SetNumberField(TEXT("goal_count"), GoalsArr.Num());

	// Retarget chains
	TArray<TSharedPtr<FJsonValue>> ChainsArr;
	const TArray<FBoneChain>& Chains = C->GetRetargetChains();
	for (const FBoneChain& Chain : Chains)
	{
		TSharedPtr<FJsonObject> ChainObj = MakeShared<FJsonObject>();
		ChainObj->SetStringField(TEXT("name"), Chain.ChainName.ToString());
		ChainObj->SetStringField(TEXT("start_bone"), Chain.StartBone.BoneName.ToString());
		ChainObj->SetStringField(TEXT("end_bone"), Chain.EndBone.BoneName.ToString());
		ChainObj->SetStringField(TEXT("goal"), Chain.IKGoalName.ToString());
		ChainsArr.Add(MakeShared<FJsonValueObject>(ChainObj));
	}
	Root->SetArrayField(TEXT("retarget_chains"), ChainsArr);
	Root->SetNumberField(TEXT("retarget_chain_count"), ChainsArr.Num());

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleAddIKSolver(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SolverType = Params->GetStringField(TEXT("solver_type"));

	UIKRigDefinition* Asset = FMonolithAssetUtils::LoadAssetByPath<UIKRigDefinition>(AssetPath);
	if (!Asset) return FMonolithActionResult::Error(FString::Printf(TEXT("IKRigDefinition not found: %s"), *AssetPath));

	UIKRigController* C = UIKRigController::GetController(Asset);
	if (!C) return FMonolithActionResult::Error(TEXT("Failed to get IKRigController"));

	if (SolverType.IsEmpty())
		return FMonolithActionResult::Error(TEXT("solver_type is required"));

	// Resolve the solver type by enumerating the live FIKRigSolverBase struct table
	// (engine pattern: IKRigEditorController.cpp:854-882). The struct table is the source of
	// truth — never a hardcoded /Script/... path. The alias map is a friendly-name convenience
	// ON TOP of the enumeration; it carries both the bare spelling and the '...solver'-suffixed
	// spelling so solver_type:"FullBodyIKSolver" resolves via alias before the gated substring
	// branch is ever reached.
	static const TMap<FString, FString> Aliases = {       // friendly (lowercase) -> canonical Struct->GetName()
		{TEXT("fullbodyik"),        TEXT("IKRigFullBodyIKSolver")},
		{TEXT("fbik"),              TEXT("IKRigFullBodyIKSolver")},
		{TEXT("fullbodyiksolver"),  TEXT("IKRigFullBodyIKSolver")},
		{TEXT("limb"),              TEXT("IKRigLimbSolver")},
		{TEXT("limbsolver"),        TEXT("IKRigLimbSolver")},
		{TEXT("pole"),              TEXT("IKRigPoleSolver")},
		{TEXT("polesolver"),        TEXT("IKRigPoleSolver")},
		{TEXT("bodymover"),         TEXT("IKRigBodyMoverSolver")},
		{TEXT("bodymoversolver"),   TEXT("IKRigBodyMoverSolver")},
		{TEXT("settransform"),      TEXT("IKRigSetTransform")},
		{TEXT("stretchlimb"),       TEXT("IKRigStretchLimbSolver")},
		{TEXT("stretchlimbsolver"), TEXT("IKRigStretchLimbSolver")},
	};
	const FString LowerType = SolverType.ToLower();
	const FString Want = Aliases.Contains(LowerType) ? Aliases[LowerType] : SolverType;

	// First pass: collect every candidate struct + remember any exact identity-style match.
	// NEVER break on a substring hit — TObjectIterator order is not stable, and e.g. both
	// IKRigLimbSolver and IKRigStretchLimbSolver contain "LimbSolver", so a first-hit substring
	// match is non-deterministic. The engine matches by struct identity, so we resolve
	// deterministically: alias/exact/prefix first, gated-unique substring only as a last resort.
	UScriptStruct* Exact = nullptr;
	TArray<UScriptStruct*> SubstringMatches;
	TArray<FString> Available;
	for (TObjectIterator<UStruct> It; It; ++It)
	{
		UScriptStruct* S = Cast<UScriptStruct>(*It);
		if (!S || !S->IsNative() || !S->IsChildOf(FIKRigSolverBase::StaticStruct())) continue;
		if (S == FIKRigSolverBase::StaticStruct()) continue;          // skip base struct
		const FString Name = S->GetName();
		Available.Add(Name);
		// exact match on Struct->GetName(); also accept the leading-'F' C++ spelling.
		if (Name.Equals(Want, ESearchCase::IgnoreCase)
			|| Name.Equals(FString::Printf(TEXT("F%s"), *Want), ESearchCase::IgnoreCase))
		{
			Exact = S;   // do NOT break — keep enumerating so 'Available' is complete for errors
		}
		else if (Name.Contains(Want, ESearchCase::IgnoreCase))
		{
			SubstringMatches.Add(S);
		}
	}

	UScriptStruct* Resolved = Exact;
	if (!Resolved)
	{
		// Last-resort substring: fire ONLY when exactly one struct contains the term.
		if (SubstringMatches.Num() == 1)
		{
			Resolved = SubstringMatches[0];
		}
		else if (SubstringMatches.Num() > 1)
		{
			TArray<FString> Names;
			for (UScriptStruct* S : SubstringMatches) Names.Add(S->GetName());
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Solver type '%s' is ambiguous — matches %d solvers: %s. Use the exact struct name."),
				*SolverType, SubstringMatches.Num(), *FString::Join(Names, TEXT(", "))));
		}
	}
	if (!Resolved)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Solver type '%s' not found. Available: %s"), *SolverType, *FString::Join(Available, TEXT(", "))));
	}

	const int32 SolverIdx = C->AddSolver(Resolved);   // UScriptStruct* overload — IKRigController.h:151
	if (SolverIdx < 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to add solver of type '%s' (resolved '%s') — AddSolver returned INDEX_NONE"),
			*SolverType, *Resolved->GetName()));
	}

	// Canonical resolved name for the result payload.
	const FString ResolvedName = Resolved->GetName();

	// Optional root bone
	FString RootBone;
	bool bStartBoneSet = false;
	if (Params->TryGetStringField(TEXT("root_bone"), RootBone) && !RootBone.IsEmpty())
	{
		bStartBoneSet = C->SetStartBone(FName(*RootBone), SolverIdx);
	}

	// Optional goals array
	TArray<FString> CreatedGoals;
	TArray<FString> SkippedGoals;
	TArray<FString> Warnings;
	const TArray<TSharedPtr<FJsonValue>>* GoalsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("goals"), GoalsArray) && GoalsArray)
	{
		const FIKRigSkeleton& Skel = C->GetIKRigSkeleton();
		if (Skel.BoneNames.Num() == 0)
		{
			// Can't validate bones — skeleton not loaded (no preview mesh assigned)
			// Skip bone pre-check and let AddNewGoal handle validation
			Warnings.Add(TEXT("IKRig has no skeleton data loaded — bone validation skipped. Assign a preview mesh for reliable goal creation."));
		}
		for (const TSharedPtr<FJsonValue>& GoalVal : *GoalsArray)
		{
			const TSharedPtr<FJsonObject>* GoalObjPtr = nullptr;
			if (!GoalVal->TryGetObject(GoalObjPtr) || !GoalObjPtr) continue;

			FString GoalNameStr, BoneNameStr;
			if (!(*GoalObjPtr)->TryGetStringField(TEXT("name"), GoalNameStr) ||
				!(*GoalObjPtr)->TryGetStringField(TEXT("bone"), BoneNameStr))
			{
				continue;
			}

			// Validate bone exists (only when skeleton is populated)
			if (Skel.BoneNames.Num() > 0 && !Skel.BoneNames.Contains(FName(*BoneNameStr)))
			{
				SkippedGoals.Add(FString::Printf(TEXT("%s (bone '%s' not found)"), *GoalNameStr, *BoneNameStr));
				continue;
			}

			FName CreatedGoalName = C->AddNewGoal(FName(*GoalNameStr), FName(*BoneNameStr));
			if (!CreatedGoalName.IsNone())
			{
				C->ConnectGoalToSolver(CreatedGoalName, SolverIdx);
				CreatedGoals.Add(CreatedGoalName.ToString());
			}
		}
	}

	Asset->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("solver_index"), SolverIdx);
	Root->SetStringField(TEXT("solver_type"), ResolvedName);
	Root->SetStringField(TEXT("label"), C->GetSolverUniqueName(SolverIdx));

	if (!RootBone.IsEmpty())
	{
		Root->SetBoolField(TEXT("start_bone_set"), bStartBoneSet);
		if (!bStartBoneSet)
		{
			Root->SetStringField(TEXT("start_bone_warning"), FString::Printf(TEXT("SetStartBone failed for '%s' — bone may not exist in skeleton"), *RootBone));
		}
	}

	TArray<TSharedPtr<FJsonValue>> GoalNamesArr;
	for (const FString& GoalName : CreatedGoals)
	{
		GoalNamesArr.Add(MakeShared<FJsonValueString>(GoalName));
	}
	Root->SetArrayField(TEXT("created_goals"), GoalNamesArr);

	TArray<TSharedPtr<FJsonValue>> SkippedGoalsArr;
	for (const FString& Skipped : SkippedGoals)
	{
		SkippedGoalsArr.Add(MakeShared<FJsonValueString>(Skipped));
	}
	Root->SetArrayField(TEXT("skipped_goals"), SkippedGoalsArr);

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArr;
		for (const FString& W : Warnings)
		{
			WarningsArr.Add(MakeShared<FJsonValueString>(W));
		}
		Root->SetArrayField(TEXT("warnings"), WarningsArr);
	}

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleRemoveIKSolver(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UIKRigDefinition* Asset = FMonolithAssetUtils::LoadAssetByPath<UIKRigDefinition>(AssetPath);
	if (!Asset) return FMonolithActionResult::Error(FString::Printf(TEXT("IKRigDefinition not found: %s"), *AssetPath));

	UIKRigController* C = UIKRigController::GetController(Asset);
	if (!C) return FMonolithActionResult::Error(TEXT("Failed to get IKRigController"));

	if (!Params->HasField(TEXT("solver_index")))
		return FMonolithActionResult::Error(TEXT("solver_index is required"));
	const int32 SolverIndex = static_cast<int32>(Params->GetNumberField(TEXT("solver_index")));

	const int32 NumSolvers = C->GetNumSolvers();
	if (SolverIndex < 0 || SolverIndex >= NumSolvers)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid solver_index %d — IK Rig has %d solver(s) (valid range 0..%d)."),
			SolverIndex, NumSolvers, NumSolvers - 1));
	}

	// RemoveSolver re-validates the index internally and returns false on failure.
	if (!C->RemoveSolver(SolverIndex))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("RemoveSolver failed for index %d (IK Rig has %d solver(s))."), SolverIndex, NumSolvers));
	}

	Asset->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("removed_index"), SolverIndex);
	Root->SetNumberField(TEXT("solver_count_after"), C->GetNumSolvers());
	Root->SetBoolField(TEXT("saved"), false);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Retarget op-settings reflective reader (T1-R3 read companion for get_retargeter_info)
// ---------------------------------------------------------------------------
//
// The IK Retargeter op stack stores its settings as op-specific UStructs
// (FIKRetarget*OpSettings, all deriving FIKRetargetOpSettingsBase). These structs
// carry no UFunctions and differ per op type, so we surface them generically by
// walking the concrete UScriptStruct's UPROPERTYs. This is READ-ONLY: we never
// mutate the settings, so the by-value SetSettings copy-mutate-set discipline
// (gotcha 5) is not in play here.
//
// Leaf properties are serialised with ExportTextItem_Direct (the same path the
// engine uses for clipboard / config text), which round-trips every numeric /
// enum / name / string type. Nested structs recurse; arrays/maps/sets fall back
// to the exported text form (keeps the dump compact + always-valid JSON).
namespace MonolithRetargetOpReader
{
	TSharedPtr<FJsonObject> StructToJson(const UScriptStruct* Struct, const void* Data);

	static TSharedPtr<FJsonValue> PropertyToJson(const FProperty* Prop, const void* ValuePtr)
	{
		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			return MakeShared<FJsonValueBoolean>(BoolProp->GetPropertyValue(ValuePtr));
		}
		if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
		{
			// Enum-backed numerics surface as their enumerator name (gotcha 3: never
			// trust raw int order — EFKChainRotationMode has non-sequential values).
			if (UEnum* Enum = NumProp->GetIntPropertyEnum())
			{
				const int64 EnumVal = NumProp->GetSignedIntPropertyValue(ValuePtr);
				return MakeShared<FJsonValueString>(Enum->GetNameStringByValue(EnumVal));
			}
			if (NumProp->IsFloatingPoint())
			{
				return MakeShared<FJsonValueNumber>(NumProp->GetFloatingPointPropertyValue(ValuePtr));
			}
			return MakeShared<FJsonValueNumber>(static_cast<double>(NumProp->GetSignedIntPropertyValue(ValuePtr)));
		}
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			UEnum* Enum = EnumProp->GetEnum();
			const int64 EnumVal = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			return MakeShared<FJsonValueString>(Enum ? Enum->GetNameStringByValue(EnumVal)
												   : FString::FromInt(static_cast<int32>(EnumVal)));
		}
		if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			return MakeShared<FJsonValueString>(StrProp->GetPropertyValue(ValuePtr));
		}
		if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			return MakeShared<FJsonValueString>(NameProp->GetPropertyValue(ValuePtr).ToString());
		}
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			return MakeShared<FJsonValueObject>(StructToJson(StructProp->Struct, ValuePtr));
		}
		// Arrays / maps / sets / objects: export to text (compact, always valid).
		FString Exported;
		Prop->ExportTextItem_Direct(Exported, ValuePtr, /*DefaultValue=*/nullptr,
			/*Parent=*/nullptr, PPF_None, /*ExportRootScope=*/nullptr);
		return MakeShared<FJsonValueString>(Exported);
	}

	TSharedPtr<FJsonObject> StructToJson(const UScriptStruct* Struct, const void* Data)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Struct || !Data) return Obj;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Prop = *It;
			if (!Prop) continue;
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Data);
			Obj->SetField(Prop->GetName(), PropertyToJson(Prop, ValuePtr));
		}
		return Obj;
	}
}

FMonolithActionResult FMonolithAnimationActions::HandleGetRetargeterInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UIKRetargeter* Asset = FMonolithAssetUtils::LoadAssetByPath<UIKRetargeter>(AssetPath);
	if (!Asset) return FMonolithActionResult::Error(FString::Printf(TEXT("IKRetargeter not found: %s"), *AssetPath));

	UIKRetargeterController* C = UIKRetargeterController::GetController(Asset);
	if (!C) return FMonolithActionResult::Error(TEXT("Failed to get UIKRetargeterController"));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	// Source / target rigs
	const UIKRigDefinition* SourceRig = C->GetIKRig(ERetargetSourceOrTarget::Source);
	const UIKRigDefinition* TargetRig = C->GetIKRig(ERetargetSourceOrTarget::Target);
	Root->SetStringField(TEXT("source_rig"), SourceRig ? SourceRig->GetPathName() : TEXT("None"));
	Root->SetStringField(TEXT("target_rig"), TargetRig ? TargetRig->GetPathName() : TEXT("None"));

	// Preview meshes
	USkeletalMesh* SourceMesh = C->GetPreviewMesh(ERetargetSourceOrTarget::Source);
	USkeletalMesh* TargetMesh = C->GetPreviewMesh(ERetargetSourceOrTarget::Target);
	Root->SetStringField(TEXT("source_preview_mesh"), SourceMesh ? SourceMesh->GetPathName() : TEXT("None"));
	Root->SetStringField(TEXT("target_preview_mesh"), TargetMesh ? TargetMesh->GetPathName() : TEXT("None"));

	// Op count
	const int32 NumOps = C->GetNumRetargetOps();
	Root->SetNumberField(TEXT("retarget_op_count"), NumOps);

	// Op stack (T1-R3 read companion). For each op in the stack emit its index,
	// name, type (op struct name), enabled flag, and a reflective dump of the op's
	// settings struct. The op is reached via GetRetargetOpByIndex(i); its own struct
	// type comes from GetType() and the live settings struct from GetSettingsConst()
	// + GetSettingsType(). This is purely read-only — gotcha 5 (op settings returned
	// by-value on write) does not apply here because we only read the live settings,
	// never copy-mutate-set.
	TArray<TSharedPtr<FJsonValue>> OpsArr;
	for (int32 OpIdx = 0; OpIdx < NumOps; ++OpIdx)
	{
		TSharedPtr<FJsonObject> OpObj = MakeShared<FJsonObject>();
		OpObj->SetNumberField(TEXT("index"), OpIdx);
		OpObj->SetStringField(TEXT("name"), C->GetOpName(OpIdx).ToString());
		OpObj->SetBoolField(TEXT("enabled"), C->GetRetargetOpEnabled(OpIdx));

		if (const FIKRetargetOpBase* Op = C->GetRetargetOpByIndex(OpIdx))
		{
			const UScriptStruct* OpStruct = Op->GetType();
			OpObj->SetStringField(TEXT("type"), OpStruct ? OpStruct->GetName() : TEXT("Unknown"));

			const UScriptStruct* SettingsStruct = Op->GetSettingsType();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			const FIKRetargetOpSettingsBase* OpSettings = Op->GetSettingsConst();
#else
			// UE 5.6 only exposes the non-const GetSettings() accessor.
			const FIKRetargetOpSettingsBase* OpSettings = const_cast<FIKRetargetOpBase*>(Op)->GetSettings();
#endif
			if (SettingsStruct && OpSettings)
			{
				OpObj->SetStringField(TEXT("settings_type"), SettingsStruct->GetName());
				OpObj->SetObjectField(TEXT("settings"),
					MonolithRetargetOpReader::StructToJson(SettingsStruct,
						reinterpret_cast<const void*>(OpSettings)));
			}
		}
		else
		{
			OpObj->SetStringField(TEXT("type"), TEXT("Unknown"));
		}
		OpsArr.Add(MakeShared<FJsonValueObject>(OpObj));
	}
	Root->SetArrayField(TEXT("ops"), OpsArr);

	// Chain mappings — iterate all target chains and query per-chain source
	TArray<TSharedPtr<FJsonValue>> MappingsArr;
	if (TargetRig)
	{
		const TArray<FBoneChain>& TargetChains = TargetRig->GetRetargetChains();
		for (const FBoneChain& Chain : TargetChains)
		{
			FName SourceChain = C->GetSourceChain(Chain.ChainName);
			if (!SourceChain.IsNone())
			{
				TSharedPtr<FJsonObject> PairObj = MakeShared<FJsonObject>();
				PairObj->SetStringField(TEXT("target_chain"), Chain.ChainName.ToString());
				PairObj->SetStringField(TEXT("source_chain"), SourceChain.ToString());
				MappingsArr.Add(MakeShared<FJsonValueObject>(PairObj));
			}
		}
	}
	Root->SetArrayField(TEXT("chain_mappings"), MappingsArr);
	Root->SetNumberField(TEXT("chain_mapping_count"), MappingsArr.Num());

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetRetargetChainMapping(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UIKRetargeter* Asset = FMonolithAssetUtils::LoadAssetByPath<UIKRetargeter>(AssetPath);
	if (!Asset) return FMonolithActionResult::Error(FString::Printf(TEXT("IKRetargeter not found: %s"), *AssetPath));

	UIKRetargeterController* C = UIKRetargeterController::GetController(Asset);
	if (!C) return FMonolithActionResult::Error(TEXT("Failed to get UIKRetargeterController"));

	FString AutoMapStr;
	FString SourceChain, TargetChain;
	const bool bHasAutoMap = Params->TryGetStringField(TEXT("auto_map"), AutoMapStr);
	const bool bHasSourceChain = Params->TryGetStringField(TEXT("source_chain"), SourceChain);
	const bool bHasTargetChain = Params->TryGetStringField(TEXT("target_chain"), TargetChain);

	if (!bHasAutoMap && !(bHasSourceChain && bHasTargetChain))
	{
		return FMonolithActionResult::Error(TEXT("Must provide either 'auto_map' or both 'source_chain' and 'target_chain'"));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Retarget Chain Mapping")));
	Asset->Modify();

	if (bHasAutoMap)
	{
		EAutoMapChainType MapType = EAutoMapChainType::Fuzzy;
		if (AutoMapStr.Equals(TEXT("exact"), ESearchCase::IgnoreCase))
		{
			MapType = EAutoMapChainType::Exact;
		}
		else if (AutoMapStr.Equals(TEXT("clear"), ESearchCase::IgnoreCase))
		{
			MapType = EAutoMapChainType::Clear;
		}
		else if (!AutoMapStr.Equals(TEXT("fuzzy"), ESearchCase::IgnoreCase))
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Unknown auto_map value '%s' — use 'exact', 'fuzzy', or 'clear'"), *AutoMapStr));
		}
		C->AutoMapChains(MapType, true);
	}
	else
	{
		bool bOk = C->SetSourceChain(FName(*SourceChain), FName(*TargetChain));
		if (!bOk)
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to set mapping: source chain '%s' or target chain '%s' not found"), *SourceChain, *TargetChain));
		}
	}

	GEditor->EndTransaction();
	Asset->MarkPackageDirty();

	// Return resulting mappings for confirmation — iterate all target chains
	TArray<TSharedPtr<FJsonValue>> MappingsArr;
	const UIKRigDefinition* TargetRig = C->GetIKRig(ERetargetSourceOrTarget::Target);
	if (TargetRig)
	{
		const TArray<FBoneChain>& TargetChains = TargetRig->GetRetargetChains();
		for (const FBoneChain& Chain : TargetChains)
		{
			FName MappedSource = C->GetSourceChain(Chain.ChainName);
			if (!MappedSource.IsNone())
			{
				TSharedPtr<FJsonObject> PairObj = MakeShared<FJsonObject>();
				PairObj->SetStringField(TEXT("target_chain"), Chain.ChainName.ToString());
				PairObj->SetStringField(TEXT("source_chain"), MappedSource.ToString());
				MappingsArr.Add(MakeShared<FJsonValueObject>(PairObj));
			}
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetArrayField(TEXT("chain_mappings"), MappingsArr);
	Root->SetNumberField(TEXT("chain_mapping_count"), MappingsArr.Num());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Wave 10 — ABP Write Experimental
// ---------------------------------------------------------------------------

// Helper: find a state machine graph by machine name (exact match on node title, same logic as get_state_machines)
static UAnimationStateMachineGraph* FindStateMachineGraphByName(UAnimBlueprint* ABP, const FString& MachineName)
{
	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node);
			if (!SMNode) continue;

			FString SMTitle = SMNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			int32 NewlineIdx = INDEX_NONE;
			if (SMTitle.FindChar(TEXT('\n'), NewlineIdx))
			{
				SMTitle.LeftInline(NewlineIdx);
			}
			if (SMTitle == MachineName)
			{
				return Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
			}
		}
	}
	return nullptr;
}

// Helper: true if VariableName resolves to an inherited Blueprint-visible bool on the
// ABP's skeleton/generated/parent class chain (e.g. a BlueprintReadOnly bool UPROPERTY on a
// native AnimInstance parent). NewVariables-only validation misses these, so transition-rule
// gates use this fallback before rejecting. Downstream SetSelfMember already resolves them.
static bool IsInheritedBlueprintVisibleBool(UAnimBlueprint* ABP, const FString& VariableName)
{
	if (!ABP) return false;
	UClass* SearchClass = ABP->SkeletonGeneratedClass ? ABP->SkeletonGeneratedClass : ABP->GeneratedClass;
	if (!SearchClass) return false;
	const FBoolProperty* BoolProp = FindFProperty<FBoolProperty>(SearchClass, *VariableName);
	return BoolProp && BoolProp->HasAnyPropertyFlags(CPF_BlueprintVisible);
}

// Helper: true if VariableName resolves to an inherited Blueprint-visible float/double
// UPROPERTY on the ABP's skeleton/generated/parent class chain. Mirrors the inherited-bool
// fallback above for Phase 6 float-comparison operands (a compare LHS may be an inherited C++
// float UPROPERTY such as a movement-speed accessor on a native AnimInstance parent).
// Blueprint "float" maps to FDoubleProperty in UE5; legacy FFloatProperty is also accepted.
static bool IsInheritedBlueprintVisibleFloat(UAnimBlueprint* ABP, const FString& VariableName)
{
	if (!ABP) return false;
	UClass* SearchClass = ABP->SkeletonGeneratedClass ? ABP->SkeletonGeneratedClass : ABP->GeneratedClass;
	if (!SearchClass) return false;
	if (const FDoubleProperty* DblProp = FindFProperty<FDoubleProperty>(SearchClass, *VariableName))
	{
		return DblProp->HasAnyPropertyFlags(CPF_BlueprintVisible);
	}
	if (const FFloatProperty* FltProp = FindFProperty<FFloatProperty>(SearchClass, *VariableName))
	{
		return FltProp->HasAnyPropertyFlags(CPF_BlueprintVisible);
	}
	return false;
}

// Helper: validate that VariableName is a usable float/numeric operand for a compare rule —
// either a BP-declared NewVariables float/double/int, or an inherited Blueprint-visible
// float/double UPROPERTY. Returns true if usable; fills OutFoundCategory for diagnostics.
static bool IsUsableFloatOperand(UAnimBlueprint* ABP, const FString& VariableName, FString& OutFoundCategory)
{
	if (!ABP) return false;
	for (const FBPVariableDescription& V : ABP->NewVariables)
	{
		if (V.VarName.ToString() == VariableName)
		{
			const FString Cat = V.VarType.PinCategory.ToString();
			OutFoundCategory = Cat;
			// UE5 numeric pin categories: "real" (float/double) and "int"/"int64".
			return Cat.Equals(TEXT("real"), ESearchCase::IgnoreCase)
				|| Cat.Equals(TEXT("float"), ESearchCase::IgnoreCase)
				|| Cat.Equals(TEXT("double"), ESearchCase::IgnoreCase)
				|| Cat.Equals(TEXT("int"), ESearchCase::IgnoreCase)
				|| Cat.Equals(TEXT("int64"), ESearchCase::IgnoreCase);
		}
	}
	if (IsInheritedBlueprintVisibleFloat(ABP, VariableName))
	{
		OutFoundCategory = TEXT("inherited-float");
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Phase 6 — float/compare transition rules (shared rule-graph authoring)
// ---------------------------------------------------------------------------
//
// Maps a compare operator token to the KismetMathLibrary double-comparison function name.
// Blueprint "float" is a double in UE5, so the *_DoubleDouble variants are correct.
// Verified offline (UE 5.7 KismetMathLibrary.h): Greater_DoubleDouble / Less_DoubleDouble /
// GreaterEqual_DoubleDouble / LessEqual_DoubleDouble / EqualEqual_DoubleDouble /
// NotEqual_DoubleDouble, all (double A, double B) -> bool.
static FName CompareOpToKismetFunctionName(const FString& Op)
{
	if (Op == TEXT(">"))  return TEXT("Greater_DoubleDouble");
	if (Op == TEXT("<"))  return TEXT("Less_DoubleDouble");
	if (Op == TEXT(">=")) return TEXT("GreaterEqual_DoubleDouble");
	if (Op == TEXT("<=")) return TEXT("LessEqual_DoubleDouble");
	if (Op == TEXT("==")) return TEXT("EqualEqual_DoubleDouble");
	if (Op == TEXT("!=")) return TEXT("NotEqual_DoubleDouble");
	return NAME_None;
}

// Resolve the variable-value output pin from a freshly-allocated VariableGet node, tolerant of
// pin-name variance (named pin first, then any non-self output). Mirrors the existing
// bool-rule wiring at HandleSetTransitionRule.
static UEdGraphPin* FindVariableGetOutputPin(UK2Node_VariableGet* VarGetNode, const FString& VariableName)
{
	if (!VarGetNode) return nullptr;
	if (UEdGraphPin* Named = VarGetNode->FindPin(FName(*VariableName), EGPD_Output))
	{
		return Named;
	}
	for (UEdGraphPin* Pin : VarGetNode->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinName != TEXT("self"))
		{
			return Pin;
		}
	}
	return nullptr;
}

// Helper: find a state node by exact name within a state machine graph
static UAnimStateNode* FindStateNodeByName(UAnimationStateMachineGraph* SMGraph, const FString& StateName)
{
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node);
		if (StateNode && StateNode->GetStateName() == StateName)
		{
			return StateNode;
		}
	}
	return nullptr;
}

FMonolithActionResult FMonolithAnimationActions::HandleAddStateToMachine(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath   = Params->GetStringField(TEXT("asset_path"));
	FString MachineName = Params->GetStringField(TEXT("machine_name"));
	FString StateName   = Params->GetStringField(TEXT("state_name"));

	double TempVal;
	int32 PosX = 200;
	int32 PosY = 0;
	if (Params->TryGetNumberField(TEXT("position_x"), TempVal)) PosX = static_cast<int32>(TempVal);
	if (Params->TryGetNumberField(TEXT("position_y"), TempVal)) PosY = static_cast<int32>(TempVal);

	if (MachineName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: machine_name"));
	if (StateName.IsEmpty())   return FMonolithActionResult::Error(TEXT("Missing required parameter: state_name"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	UAnimationStateMachineGraph* SMGraph = FindStateMachineGraphByName(ABP, MachineName);
	if (!SMGraph) return FMonolithActionResult::Error(FString::Printf(TEXT("State machine '%s' not found in ABP"), *MachineName));

	// Reject duplicate state names up front
	if (FindStateNodeByName(SMGraph, StateName))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("A state named '%s' already exists in machine '%s'"), *StateName, *MachineName));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Add State to Machine")));
	SMGraph->Modify();

	// SpawnNodeFromTemplate follows the same code path as the editor's drag-drop.
	// It calls PostPlacedNewNode() which creates the BoundGraph subgraph.
	UAnimStateNode* NewNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(
		SMGraph,
		NewObject<UAnimStateNode>(SMGraph),
		FVector2f(static_cast<float>(PosX), static_cast<float>(PosY)),
		/*bSelectNewNode=*/false);

	if (!NewNode)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("Failed to spawn state node"));
	}

	if (!NewNode->BoundGraph)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("State node created but BoundGraph is null — state may be corrupt"));
	}

	// Rename via the BoundGraph — this propagates the name correctly so GetStateName() returns the right value
	if (NewNode->BoundGraph)
	{
		TSharedPtr<INameValidatorInterface> NameValidator = FNameValidatorFactory::MakeValidator(NewNode);
		FBlueprintEditorUtils::RenameGraphWithSuggestion(NewNode->BoundGraph, NameValidator, StateName);
	}

	GEditor->EndTransaction();

	FKismetEditorUtilities::CompileBlueprint(ABP);
	ABP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("machine_name"), MachineName);
	Root->SetStringField(TEXT("state_name"), NewNode->GetStateName());
	Root->SetNumberField(TEXT("position_x"), NewNode->NodePosX);
	Root->SetNumberField(TEXT("position_y"), NewNode->NodePosY);
	return FMonolithActionResult::Success(Root);
}

// Find an existing conduit node in a state machine graph by its display name.
// A conduit derives its name from its BoundGraph, exactly like a state.
static UAnimStateConduitNode* FindConduitNodeByName(UAnimationStateMachineGraph* SMGraph, const FString& ConduitName)
{
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		UAnimStateConduitNode* Conduit = Cast<UAnimStateConduitNode>(Node);
		if (Conduit && Conduit->GetStateName() == ConduitName)
		{
			return Conduit;
		}
	}
	return nullptr;
}

FMonolithActionResult FMonolithAnimationActions::HandleAddConduit(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath   = Params->GetStringField(TEXT("asset_path"));
	FString MachineName = Params->GetStringField(TEXT("machine_name"));
	FString ConduitName = Params->GetStringField(TEXT("conduit_name"));

	double TempVal;
	int32 PosX = 200;
	int32 PosY = 0;
	if (Params->TryGetNumberField(TEXT("position_x"), TempVal)) PosX = static_cast<int32>(TempVal);
	if (Params->TryGetNumberField(TEXT("position_y"), TempVal)) PosY = static_cast<int32>(TempVal);

	if (MachineName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: machine_name"));
	if (ConduitName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: conduit_name"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	UAnimationStateMachineGraph* SMGraph = FindStateMachineGraphByName(ABP, MachineName);
	if (!SMGraph) return FMonolithActionResult::Error(FString::Printf(TEXT("State machine '%s' not found in ABP"), *MachineName));

	// Reject name collisions against existing states AND conduits (both share the
	// state-name namespace, since a conduit's name derives from its BoundGraph).
	if (FindStateNodeByName(SMGraph, ConduitName))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("A state named '%s' already exists in machine '%s'"), *ConduitName, *MachineName));
	}
	if (FindConduitNodeByName(SMGraph, ConduitName))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("A conduit named '%s' already exists in machine '%s'"), *ConduitName, *MachineName));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Add Conduit to Machine")));
	SMGraph->Modify();

	// Conduits spawn through the same state-node template path as UAnimStateNode.
	// SpawnNodeFromTemplate runs the editor drag-drop code path, whose
	// PostPlacedNewNode() creates the conduit's BoundGraph subgraph.
	UAnimStateConduitNode* NewNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateConduitNode>(
		SMGraph,
		NewObject<UAnimStateConduitNode>(SMGraph),
		FVector2f(static_cast<float>(PosX), static_cast<float>(PosY)),
		/*bSelectNewNode=*/false);

	if (!NewNode)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("Failed to spawn conduit node"));
	}

	// IMPORTANT: a conduit's BoundGraph is a TRANSITION-LOGIC graph (a boolean rule
	// graph), NOT an anim/pose graph. It has no pose sink — pose-pin actions
	// (set_state_result_source, add_anim_graph_node with a player node, etc.) must
	// NOT target it. See AnimStateConduitNode.h:19.
	if (!NewNode->BoundGraph)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("Conduit node created but BoundGraph is null — conduit may be corrupt"));
	}

	// Rename via the BoundGraph so GetStateName() returns the desired conduit name.
	{
		TSharedPtr<INameValidatorInterface> NameValidator = FNameValidatorFactory::MakeValidator(NewNode);
		FBlueprintEditorUtils::RenameGraphWithSuggestion(NewNode->BoundGraph, NameValidator, ConduitName);
	}

	GEditor->EndTransaction();

	FKismetEditorUtilities::CompileBlueprint(ABP);
	ABP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("machine_name"), MachineName);
	Root->SetStringField(TEXT("conduit_name"), NewNode->GetStateName());
	Root->SetStringField(TEXT("bound_graph"), NewNode->BoundGraph->GetName());
	// Tag the bound graph kind so callers do not target it with pose-pin actions.
	Root->SetStringField(TEXT("graph_kind"), TEXT("transition_logic"));
	Root->SetNumberField(TEXT("position_x"), NewNode->NodePosX);
	Root->SetNumberField(TEXT("position_y"), NewNode->NodePosY);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleAddTransition(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath   = Params->GetStringField(TEXT("asset_path"));
	FString MachineName = Params->GetStringField(TEXT("machine_name"));
	FString FromState   = Params->GetStringField(TEXT("from_state"));
	FString ToState     = Params->GetStringField(TEXT("to_state"));

	if (MachineName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: machine_name"));
	if (FromState.IsEmpty())   return FMonolithActionResult::Error(TEXT("Missing required parameter: from_state"));
	if (ToState.IsEmpty())     return FMonolithActionResult::Error(TEXT("Missing required parameter: to_state"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	UAnimationStateMachineGraph* SMGraph = FindStateMachineGraphByName(ABP, MachineName);
	if (!SMGraph) return FMonolithActionResult::Error(FString::Printf(TEXT("State machine '%s' not found in ABP"), *MachineName));

	UAnimStateNode* FromNode = FindStateNodeByName(SMGraph, FromState);
	if (!FromNode) return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found in machine '%s'"), *FromState, *MachineName));

	UAnimStateNode* ToNode = FindStateNodeByName(SMGraph, ToState);
	if (!ToNode) return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found in machine '%s'"), *ToState, *MachineName));

	UEdGraphPin* OutputPin = FromNode->GetOutputPin();
	UEdGraphPin* InputPin  = ToNode->GetInputPin();

	if (!OutputPin) return FMonolithActionResult::Error(FString::Printf(TEXT("Source state '%s' has no output pin"), *FromState));
	if (!InputPin)  return FMonolithActionResult::Error(FString::Printf(TEXT("Target state '%s' has no input pin"), *ToState));

	GEditor->BeginTransaction(FText::FromString(TEXT("Add Transition")));
	SMGraph->Modify();

	// TryCreateConnection internally creates the UAnimStateTransitionNode — do NOT create it manually
	const UAnimationStateMachineSchema* Schema = Cast<UAnimationStateMachineSchema>(SMGraph->GetSchema());
	if (!Schema)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("State machine graph has unexpected or null schema"));
	}
	const bool bConnected = Schema->TryCreateConnection(OutputPin, InputPin);

	GEditor->EndTransaction();

	if (!bConnected)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("TryCreateConnection failed for '%s' -> '%s'. States may already be connected or the connection is invalid."),
			*FromState, *ToState));
	}

	FKismetEditorUtilities::CompileBlueprint(ABP);
	ABP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("machine_name"), MachineName);
	Root->SetStringField(TEXT("from_state"), FromState);
	Root->SetStringField(TEXT("to_state"), ToState);
	return FMonolithActionResult::Success(Root);
}

// Helper: find the single entry node of a state machine graph. A well-formed SM has exactly one.
static UAnimStateEntryNode* FindEntryNode(UAnimationStateMachineGraph* SMGraph)
{
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		if (UAnimStateEntryNode* Entry = Cast<UAnimStateEntryNode>(Node))
		{
			return Entry;
		}
	}
	return nullptr;
}

// Helper: resolve the state currently wired to the entry node's output pin, or nullptr if none.
static UAnimStateNodeBase* GetEntryTargetState(UAnimStateEntryNode* EntryNode)
{
	if (!EntryNode) return nullptr;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	UEdGraphPin* EntryOut = EntryNode->GetOutputPin();
#else
	// UE 5.6's UAnimStateEntryNode has no GetOutputPin() convenience accessor yet.
	UEdGraphPin* EntryOut = nullptr;
	for (UEdGraphPin* P : EntryNode->Pins)
	{
		if (P && P->Direction == EGPD_Output) { EntryOut = P; break; }
	}
#endif
	if (!EntryOut) return nullptr;
	for (UEdGraphPin* Linked : EntryOut->LinkedTo)
	{
		if (Linked)
		{
			if (UAnimStateNodeBase* State = Cast<UAnimStateNodeBase>(Linked->GetOwningNode()))
			{
				return State;
			}
		}
	}
	return nullptr;
}

FMonolithActionResult FMonolithAnimationActions::HandleRemoveAnimState(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor unavailable"));

	FString AssetPath   = Params->GetStringField(TEXT("asset_path"));
	FString MachineName = Params->GetStringField(TEXT("machine_name"));
	FString StateName   = Params->GetStringField(TEXT("state_name"));

	if (MachineName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: machine_name"));
	if (StateName.IsEmpty())   return FMonolithActionResult::Error(TEXT("Missing required parameter: state_name"));

	bool bRemoveDependentTransitions = true;
	Params->TryGetBoolField(TEXT("remove_dependent_transitions"), bRemoveDependentTransitions);

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	UAnimationStateMachineGraph* SMGraph = FindStateMachineGraphByName(ABP, MachineName);
	if (!SMGraph) return FMonolithActionResult::Error(FString::Printf(TEXT("State machine '%s' not found in ABP"), *MachineName));

	UAnimStateNode* StateNode = FindStateNodeByName(SMGraph, StateName);
	if (!StateNode) return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found in machine '%s'"), *StateName, *MachineName));

	// Guard: refuse to remove the current entry-target — it would leave the entry node dangling.
	// Caller should re-point the entry via set_anim_entry_state first.
	if (UAnimStateEntryNode* EntryNode = FindEntryNode(SMGraph))
	{
		if (GetEntryTargetState(EntryNode) == StateNode)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("State '%s' is the current entry state of machine '%s'. Re-point the entry with set_anim_entry_state before removing it."),
				*StateName, *MachineName));
		}
	}

	// Enumerate ALL transitions whose from-state OR to-state is this state, by scanning the graph's
	// transition nodes and matching the directed endpoints by name. UAnimStateNodeBase::GetTransitionList
	// is NOT a complete dependency set: it returns this state's outgoing transitions plus only its
	// *bidirectional* incoming ones (AnimStateNodeBase.cpp) — an incoming non-bidirectional transition is
	// missed. Such a transition is still swept when the state is removed (its link to us breaks, triggering
	// UAnimStateTransitionNode::PinConnectionListChanged self-destruct), so counting via GetTransitionList
	// under-reports. A full-graph scan keyed on both endpoints captures every dependent transition.
	// Snapshot into local arrays up front so the count and names reflect exactly what gets swept, and so the
	// removal loop iterates a stable copy (RemoveNode mutates SMGraph->Nodes and cascades self-destructs).
	TArray<UAnimStateTransitionNode*> DepTransitions;
	TArray<FString> DepTransitionLabels;
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		UAnimStateTransitionNode* Trans = Cast<UAnimStateTransitionNode>(Node);
		if (!Trans) continue;
		UAnimStateNodeBase* Prev = Trans->GetPreviousState();
		UAnimStateNodeBase* Next = Trans->GetNextState();
		const bool bTouchesState =
			(Prev && Prev->GetStateName() == StateName) ||
			(Next && Next->GetStateName() == StateName);
		if (!bTouchesState) continue;
		DepTransitions.Add(Trans);
		DepTransitionLabels.Add(FString::Printf(TEXT("%s->%s"),
			Prev ? *Prev->GetStateName() : TEXT("?"),
			Next ? *Next->GetStateName() : TEXT("?")));
	}

	if (!bRemoveDependentTransitions && DepTransitions.Num() > 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("State '%s' has %d dependent transition(s): %s. Pass remove_dependent_transitions:true to remove them too."),
			*StateName, DepTransitions.Num(), *FString::Join(DepTransitionLabels, TEXT(", "))));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Remove Anim State")));
	SMGraph->Modify();

	// Remove the dependent transitions FIRST, before the state, iterating the up-front snapshot (never a
	// live node list — RemoveNode mutates SMGraph->Nodes and each removal can cascade self-destructs).
	// Do NOT pre-break their links — UAnimStateTransitionNode::PinConnectionListChanged self-destroys the
	// node when links hit zero (AnimStateTransitionNode.cpp), which would invalidate the pointer before
	// RemoveNode runs. RemoveNode breaks links + invokes DestroyNode (rule-subgraph teardown) itself.
	// Build the reported list from the pre-captured snapshot labels so the count is exactly what we sweep,
	// regardless of the cascade order. Removing the transitions here means the subsequent state removal has
	// nothing left to cascade onto them (their pointers are already gone), so no double-remove can occur.
	TArray<TSharedPtr<FJsonValue>> RemovedTransitions;
	if (bRemoveDependentTransitions)
	{
		for (int32 Index = 0; Index < DepTransitions.Num(); ++Index)
		{
			UAnimStateTransitionNode* Trans = DepTransitions[Index];
			if (!Trans) continue;
			RemovedTransitions.Add(MakeShared<FJsonValueString>(DepTransitionLabels[Index]));
			FBlueprintEditorUtils::RemoveNode(ABP, Trans, /*bDontRecompile=*/true);
		}
	}

	// Remove the state node. UAnimStateNode::DestroyNode() auto-collects the state's BoundGraph via
	// FBlueprintEditorUtils::RemoveGraph(..., EGraphRemoveFlags::Recompile) — do NOT call RemoveGraph
	// here (double-remove). The Recompile flag inside DestroyNode fires a recompile regardless of
	// bDontRecompile; the final CompileBlueprint below is the authoritative pass.
	FBlueprintEditorUtils::RemoveNode(ABP, StateNode, /*bDontRecompile=*/true);

	GEditor->EndTransaction();

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ABP);
	FKismetEditorUtilities::CompileBlueprint(ABP);
	ABP->MarkPackageDirty();

	const bool bCompileOk = (ABP->Status == EBlueprintStatus::BS_UpToDate
		|| ABP->Status == EBlueprintStatus::BS_UpToDateWithWarnings);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("machine_name"), MachineName);
	Root->SetStringField(TEXT("removed_state"), StateName);
	Root->SetArrayField(TEXT("removed_transitions"), RemovedTransitions);
	Root->SetNumberField(TEXT("removed_transition_count"), RemovedTransitions.Num());
	Root->SetBoolField(TEXT("compile_ok"), bCompileOk);
	Root->SetBoolField(TEXT("saved"), false);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetAnimEntryState(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor unavailable"));

	FString AssetPath   = Params->GetStringField(TEXT("asset_path"));
	FString MachineName = Params->GetStringField(TEXT("machine_name"));
	FString StateName   = Params->GetStringField(TEXT("state_name"));

	if (MachineName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: machine_name"));
	if (StateName.IsEmpty())   return FMonolithActionResult::Error(TEXT("Missing required parameter: state_name"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	UAnimationStateMachineGraph* SMGraph = FindStateMachineGraphByName(ABP, MachineName);
	if (!SMGraph) return FMonolithActionResult::Error(FString::Printf(TEXT("State machine '%s' not found in ABP"), *MachineName));

	UAnimStateEntryNode* EntryNode = FindEntryNode(SMGraph);
	if (!EntryNode) return FMonolithActionResult::Error(FString::Printf(TEXT("State machine '%s' has no entry node (corrupt)"), *MachineName));

	UAnimStateNode* TargetState = FindStateNodeByName(SMGraph, StateName);
	if (!TargetState) return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found in machine '%s'"), *StateName, *MachineName));

	// Capture the prior entry target for the result (and the unchanged fast-path).
	UAnimStateNodeBase* PrevTarget = GetEntryTargetState(EntryNode);
	const FString PrevName = PrevTarget ? PrevTarget->GetStateName() : FString();

	if (PrevTarget == TargetState)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("asset_path"), AssetPath);
		Root->SetStringField(TEXT("machine_name"), MachineName);
		Root->SetStringField(TEXT("previous_entry_state"), PrevName);
		Root->SetStringField(TEXT("new_entry_state"), StateName);
		Root->SetBoolField(TEXT("unchanged"), true);
		Root->SetBoolField(TEXT("compile_ok"), true);
		Root->SetBoolField(TEXT("saved"), false);
		return FMonolithActionResult::Success(Root);
	}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	UEdGraphPin* EntryOut = EntryNode->GetOutputPin();
#else
	// UE 5.6's UAnimStateEntryNode has no GetOutputPin() convenience accessor yet.
	UEdGraphPin* EntryOut = nullptr;
	for (UEdGraphPin* P : EntryNode->Pins)
	{
		if (P && P->Direction == EGPD_Output) { EntryOut = P; break; }
	}
#endif
	if (!EntryOut) return FMonolithActionResult::Error(TEXT("Entry node has no output pin"));
	UEdGraphPin* StateIn = TargetState->GetInputPin();
	if (!StateIn) return FMonolithActionResult::Error(FString::Printf(TEXT("Target state '%s' has no input pin"), *StateName));

	const UAnimationStateMachineSchema* Schema = Cast<UAnimationStateMachineSchema>(SMGraph->GetSchema());
	if (!Schema) return FMonolithActionResult::Error(TEXT("State machine graph has unexpected or null schema"));

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Anim Entry State")));
	SMGraph->Modify();

	// Break the entry's current link(s), then connect to the new state's input pin. The entry node's
	// output pin has no self-destruct behavior (unlike transition nodes), so breaking here is safe.
	EntryOut->BreakAllPinLinks();
	const bool bConnected = Schema->TryCreateConnection(EntryOut, StateIn);

	GEditor->EndTransaction();

	if (!bConnected)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to wire entry node to state '%s'"), *StateName));
	}

	FKismetEditorUtilities::CompileBlueprint(ABP);
	ABP->MarkPackageDirty();

	const bool bCompileOk = (ABP->Status == EBlueprintStatus::BS_UpToDate
		|| ABP->Status == EBlueprintStatus::BS_UpToDateWithWarnings);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("machine_name"), MachineName);
	if (PrevName.IsEmpty())
	{
		Root->SetField(TEXT("previous_entry_state"), MakeShared<FJsonValueNull>());
	}
	else
	{
		Root->SetStringField(TEXT("previous_entry_state"), PrevName);
	}
	Root->SetStringField(TEXT("new_entry_state"), StateName);
	Root->SetBoolField(TEXT("compile_ok"), bCompileOk);
	Root->SetBoolField(TEXT("saved"), false);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleRemoveAnimTransition(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor unavailable"));

	FString AssetPath   = Params->GetStringField(TEXT("asset_path"));
	FString MachineName = Params->GetStringField(TEXT("machine_name"));
	FString FromState   = Params->GetStringField(TEXT("from_state"));
	FString ToState     = Params->GetStringField(TEXT("to_state"));

	if (MachineName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: machine_name"));
	if (FromState.IsEmpty())   return FMonolithActionResult::Error(TEXT("Missing required parameter: from_state"));
	if (ToState.IsEmpty())     return FMonolithActionResult::Error(TEXT("Missing required parameter: to_state"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	UAnimationStateMachineGraph* SMGraph = FindStateMachineGraphByName(ABP, MachineName);
	if (!SMGraph) return FMonolithActionResult::Error(FString::Printf(TEXT("State machine '%s' not found in ABP"), *MachineName));

	UAnimStateNode* FromNode = FindStateNodeByName(SMGraph, FromState);
	if (!FromNode) return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found in machine '%s'"), *FromState, *MachineName));

	UAnimStateNode* ToNode = FindStateNodeByName(SMGraph, ToState);
	if (!ToNode) return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found in machine '%s'"), *ToState, *MachineName));

	// Find the transition from FromState -> ToState. Use the source state's transition list and match
	// the directed endpoints (GetPreviousState/GetNextState), mirroring the build_state_machine lookup.
	TArray<UAnimStateTransitionNode*> Transitions;
	FromNode->GetTransitionList(Transitions);

	UAnimStateTransitionNode* TargetTransition = nullptr;
	int32 MatchCount = 0;
	for (UAnimStateTransitionNode* Trans : Transitions)
	{
		if (!Trans) continue;
		UAnimStateNodeBase* Prev = Trans->GetPreviousState();
		UAnimStateNodeBase* Next = Trans->GetNextState();
		if (Prev && Next && Prev->GetStateName() == FromState && Next->GetStateName() == ToState)
		{
			if (!TargetTransition) TargetTransition = Trans;
			++MatchCount;
		}
	}

	if (!TargetTransition)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No transition '%s'->'%s' found in machine '%s'"), *FromState, *ToState, *MachineName));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Remove Anim Transition")));
	SMGraph->Modify();

	// Do NOT pre-break the transition's pin links — UAnimStateTransitionNode::PinConnectionListChanged
	// self-destroys the node when links hit zero (AnimStateTransitionNode.cpp), invalidating the pointer.
	// FBlueprintEditorUtils::RemoveNode breaks links + invokes DestroyNode (rule-subgraph teardown) itself.
	FBlueprintEditorUtils::RemoveNode(ABP, TargetTransition, /*bDontRecompile=*/true);

	GEditor->EndTransaction();

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ABP);
	FKismetEditorUtilities::CompileBlueprint(ABP);
	ABP->MarkPackageDirty();

	const bool bCompileOk = (ABP->Status == EBlueprintStatus::BS_UpToDate
		|| ABP->Status == EBlueprintStatus::BS_UpToDateWithWarnings);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("machine_name"), MachineName);
	Root->SetStringField(TEXT("from_state"), FromState);
	Root->SetStringField(TEXT("to_state"), ToState);
	Root->SetBoolField(TEXT("removed"), true);
	Root->SetNumberField(TEXT("matched_transition_count"), MatchCount);
	Root->SetBoolField(TEXT("compile_ok"), bCompileOk);
	Root->SetBoolField(TEXT("saved"), false);
	return FMonolithActionResult::Success(Root);
}

// One comparison term of a compound expression rule: the same per-term grammar the `compare`
// kind already parses (lhs operand variable, optional Abs(...), op, numeric rhs), plus an
// optional per-term boolean negation applied via Not_PreBool.
struct FExprTerm
{
	FString Variable;     // lhs operand variable name
	bool    bUseAbs = false; // wrap lhs in Abs(...)
	FString Op;           // one of > < >= <= == !=
	double  Rhs = 0.0;    // constant right-hand side
	bool    bNegate = false; // wrap the term's bool result in Not_PreBool
};

// Parsed representation of a structured transition rule. A plain-string `rule` collapses to
// kind=bool (variable) or kind=auto, preserving full back-compat with the legacy string form.
struct FParsedTransitionRule
{
	enum class EKind { Bool, Auto, Compare, Expression, Invalid };
	EKind   Kind = EKind::Invalid;
	FString Variable;     // bool: variable name; compare: lhs operand variable name
	bool    bUseAbs = false; // compare: wrap lhs in Abs(...)
	FString Op;           // compare: one of > < >= <= == !=
	double  Rhs = 0.0;    // compare: constant right-hand side
	FString ExpressionText; // expression kind: raw text (deferred)
	TArray<FExprTerm> Terms; // expression kind: structured AND/OR terms
	FString Combine;      // expression kind: "and" | "or" (default "and")
	FString ParseError;   // populated when Kind == Invalid
};

// Parse the JSON `rule` field into FParsedTransitionRule. Accepts BOTH the legacy plain-string
// form (back-compat: "auto"/"automatic" -> auto, anything else -> bool variable) AND the
// structured object form { kind: bool|auto|compare|expression, ... }.
static FParsedTransitionRule ParseTransitionRule(const TSharedPtr<FJsonObject>& Params)
{
	FParsedTransitionRule Out;

	// Legacy/back-compat: a plain string `rule` field.
	FString RuleStr;
	if (Params->TryGetStringField(TEXT("rule"), RuleStr))
	{
		if (RuleStr.Equals(TEXT("auto"), ESearchCase::IgnoreCase) || RuleStr.Equals(TEXT("automatic"), ESearchCase::IgnoreCase))
		{
			Out.Kind = FParsedTransitionRule::EKind::Auto;
		}
		else
		{
			Out.Kind = FParsedTransitionRule::EKind::Bool;
			Out.Variable = RuleStr;
		}
		return Out;
	}

	// Structured object `rule`.
	const TSharedPtr<FJsonObject>* RuleObjPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("rule"), RuleObjPtr) || !RuleObjPtr || !RuleObjPtr->IsValid())
	{
		Out.Kind = FParsedTransitionRule::EKind::Invalid;
		Out.ParseError = TEXT("Missing 'rule'. Provide either a string (bool variable name or 'auto') or an object { kind: bool|auto|compare|expression, ... }.");
		return Out;
	}
	const TSharedPtr<FJsonObject>& RuleObj = *RuleObjPtr;

	FString Kind = RuleObj->HasField(TEXT("kind")) ? RuleObj->GetStringField(TEXT("kind")) : FString();
	if (Kind.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
	{
		Out.Kind = FParsedTransitionRule::EKind::Auto;
		return Out;
	}
	if (Kind.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
	{
		FString VarName;
		if (!RuleObj->TryGetStringField(TEXT("variable"), VarName) || VarName.IsEmpty())
		{
			Out.Kind = FParsedTransitionRule::EKind::Invalid;
			Out.ParseError = TEXT("rule.kind=bool requires a non-empty 'variable' field.");
			return Out;
		}
		Out.Kind = FParsedTransitionRule::EKind::Bool;
		Out.Variable = VarName;
		return Out;
	}
	if (Kind.Equals(TEXT("compare"), ESearchCase::IgnoreCase))
	{
		FString Lhs;
		if (!RuleObj->TryGetStringField(TEXT("lhs"), Lhs) || Lhs.IsEmpty())
		{
			Out.Kind = FParsedTransitionRule::EKind::Invalid;
			Out.ParseError = TEXT("rule.kind=compare requires a non-empty 'lhs' field (variable name, optionally wrapped as Abs(Var)).");
			return Out;
		}
		// Allow lhs of the form "Abs(Var)" as a convenience for the common magnitude compare.
		FString LhsTrimmed = Lhs.TrimStartAndEnd();
		if (LhsTrimmed.StartsWith(TEXT("Abs(")) && LhsTrimmed.EndsWith(TEXT(")")))
		{
			Out.bUseAbs = true;
			Out.Variable = LhsTrimmed.Mid(4, LhsTrimmed.Len() - 5).TrimStartAndEnd();
		}
		else
		{
			// Explicit abs flag also supported.
			RuleObj->TryGetBoolField(TEXT("abs"), Out.bUseAbs);
			Out.Variable = LhsTrimmed;
		}
		if (Out.Variable.IsEmpty())
		{
			Out.Kind = FParsedTransitionRule::EKind::Invalid;
			Out.ParseError = TEXT("rule.kind=compare 'lhs' resolved to an empty operand name.");
			return Out;
		}

		FString Op;
		if (!RuleObj->TryGetStringField(TEXT("op"), Op) || CompareOpToKismetFunctionName(Op).IsNone())
		{
			Out.Kind = FParsedTransitionRule::EKind::Invalid;
			Out.ParseError = TEXT("rule.kind=compare requires 'op' to be one of: > < >= <= == != .");
			return Out;
		}
		Out.Op = Op;

		double Rhs = 0.0;
		if (!RuleObj->TryGetNumberField(TEXT("rhs"), Rhs))
		{
			Out.Kind = FParsedTransitionRule::EKind::Invalid;
			Out.ParseError = TEXT("rule.kind=compare requires a numeric 'rhs' constant.");
			return Out;
		}
		Out.Rhs = Rhs;
		Out.Kind = FParsedTransitionRule::EKind::Compare;
		return Out;
	}
	if (Kind.Equals(TEXT("expression"), ESearchCase::IgnoreCase))
	{
		// Structured compound rule: an array of comparison `terms` (each reusing the `compare`
		// grammar) reduced through a single `combine` boolean operator. No free-form text parser.
		FString Combine = TEXT("and");
		if (RuleObj->HasField(TEXT("combine")))
		{
			RuleObj->TryGetStringField(TEXT("combine"), Combine);
		}
		Combine = Combine.TrimStartAndEnd();
		if (!Combine.Equals(TEXT("and"), ESearchCase::IgnoreCase) && !Combine.Equals(TEXT("or"), ESearchCase::IgnoreCase))
		{
			Out.Kind = FParsedTransitionRule::EKind::Invalid;
			Out.ParseError = FString::Printf(TEXT("rule.kind=expression 'combine' must be 'and' or 'or' (got '%s')."), *Combine);
			return Out;
		}
		Out.Combine = Combine.ToLower();

		const TArray<TSharedPtr<FJsonValue>>* TermsArr = nullptr;
		if (!RuleObj->TryGetArrayField(TEXT("terms"), TermsArr) || !TermsArr)
		{
			Out.Kind = FParsedTransitionRule::EKind::Invalid;
			Out.ParseError = TEXT("rule.kind=expression requires a 'terms' array of { lhs, op, rhs, abs?, negate? } objects.");
			return Out;
		}
		if (TermsArr->Num() == 0)
		{
			Out.Kind = FParsedTransitionRule::EKind::Invalid;
			Out.ParseError = TEXT("rule.kind=expression requires at least one term in 'terms'.");
			return Out;
		}

		for (int32 Ti = 0; Ti < TermsArr->Num(); ++Ti)
		{
			const TSharedPtr<FJsonValue>& TermVal = (*TermsArr)[Ti];
			const TSharedPtr<FJsonObject>* TermObjPtr = nullptr;
			if (!TermVal.IsValid() || !TermVal->TryGetObject(TermObjPtr) || !TermObjPtr || !TermObjPtr->IsValid())
			{
				Out.Kind = FParsedTransitionRule::EKind::Invalid;
				Out.ParseError = FString::Printf(TEXT("rule.kind=expression term %d is not an object { lhs, op, rhs, abs?, negate? }."), Ti);
				return Out;
			}
			const TSharedPtr<FJsonObject>& TermObj = *TermObjPtr;

			FExprTerm Term;

			FString Lhs;
			if (!TermObj->TryGetStringField(TEXT("lhs"), Lhs) || Lhs.IsEmpty())
			{
				Out.Kind = FParsedTransitionRule::EKind::Invalid;
				Out.ParseError = FString::Printf(TEXT("rule.kind=expression term %d requires a non-empty 'lhs' (variable name, optionally 'Abs(Var)')."), Ti);
				return Out;
			}
			// Allow lhs of the form "Abs(Var)" (mirrors the compare grammar).
			FString LhsTrimmed = Lhs.TrimStartAndEnd();
			if (LhsTrimmed.StartsWith(TEXT("Abs(")) && LhsTrimmed.EndsWith(TEXT(")")))
			{
				Term.bUseAbs = true;
				Term.Variable = LhsTrimmed.Mid(4, LhsTrimmed.Len() - 5).TrimStartAndEnd();
			}
			else
			{
				TermObj->TryGetBoolField(TEXT("abs"), Term.bUseAbs);
				Term.Variable = LhsTrimmed;
			}
			if (Term.Variable.IsEmpty())
			{
				Out.Kind = FParsedTransitionRule::EKind::Invalid;
				Out.ParseError = FString::Printf(TEXT("rule.kind=expression term %d 'lhs' resolved to an empty operand name."), Ti);
				return Out;
			}

			FString Op;
			if (!TermObj->TryGetStringField(TEXT("op"), Op) || CompareOpToKismetFunctionName(Op).IsNone())
			{
				Out.Kind = FParsedTransitionRule::EKind::Invalid;
				Out.ParseError = FString::Printf(TEXT("rule.kind=expression term %d requires 'op' to be one of: > < >= <= == != ."), Ti);
				return Out;
			}
			Term.Op = Op;

			double Rhs = 0.0;
			if (!TermObj->TryGetNumberField(TEXT("rhs"), Rhs))
			{
				Out.Kind = FParsedTransitionRule::EKind::Invalid;
				Out.ParseError = FString::Printf(TEXT("rule.kind=expression term %d requires a numeric 'rhs' constant."), Ti);
				return Out;
			}
			Term.Rhs = Rhs;

			TermObj->TryGetBoolField(TEXT("negate"), Term.bNegate);

			Out.Terms.Add(Term);
		}

		Out.Kind = FParsedTransitionRule::EKind::Expression;
		return Out;
	}

	Out.Kind = FParsedTransitionRule::EKind::Invalid;
	Out.ParseError = FString::Printf(TEXT("Unknown rule.kind '%s'. Supported: bool, auto, compare, expression."), *Kind);
	return Out;
}

// Build ONE comparison's nodes into a transition rule graph: VariableGet(lhs) -> [optional Abs]
// -> Compare(op, rhs), and RETURN the comparison's bool ReturnValue pin (NOT wired to anything).
// Pure node authoring; the CALLER owns Modify()/BreakAllPinLinks(), the transaction, the result
// wiring, the compile, and rollback. RowY/ColBaseX let the caller stack multiple subgraphs
// vertically (expression terms) or place a single one (compare). Returns the bool result pin on
// success; on failure returns nullptr and fills OutError so the caller can roll back.
static UEdGraphPin* BuildCompareSubgraph(
	UEdGraph* RuleGraph,
	const FString& Lhs,
	bool bUseAbs,
	const FString& Op,
	double Rhs,
	int32 ColBaseX,
	int32 RowY,
	FString& OutError)
{
	if (!RuleGraph)
	{
		OutError = TEXT("Internal: null rule graph.");
		return nullptr;
	}

	const UEdGraphSchema* RuleSchema = RuleGraph->GetSchema();
	if (!RuleSchema)
	{
		OutError = TEXT("Rule graph has no schema.");
		return nullptr;
	}

	// 1) VariableGet for the lhs operand. SetSelfMember resolves inherited members too
	//    (same idiom as the bool-rule path).
	UK2Node_VariableGet* VarGetNode = NewObject<UK2Node_VariableGet>(RuleGraph);
	VarGetNode->VariableReference.SetSelfMember(FName(*Lhs));
	RuleGraph->AddNode(VarGetNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
	VarGetNode->NodePosX = ColBaseX - 600;
	VarGetNode->NodePosY = RowY;
	VarGetNode->AllocateDefaultPins();

	UEdGraphPin* VarOutPin = FindVariableGetOutputPin(VarGetNode, Lhs);
	if (!VarOutPin)
	{
		OutError = FString::Printf(TEXT("Could not resolve output pin for operand '%s' (variable may not exist or is not readable)."), *Lhs);
		return nullptr;
	}

	// 2) Optional Abs(double) node.
	UEdGraphPin* LhsValuePin = VarOutPin;
	if (bUseAbs)
	{
		UFunction* AbsFn = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Abs"));
		if (!AbsFn)
		{
			OutError = TEXT("Internal: UKismetMathLibrary::Abs not found.");
			return nullptr;
		}
		UK2Node_CallFunction* AbsNode = NewObject<UK2Node_CallFunction>(RuleGraph);
		AbsNode->SetFromFunction(AbsFn);
		RuleGraph->AddNode(AbsNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		AbsNode->NodePosX = ColBaseX - 400;
		AbsNode->NodePosY = RowY;
		AbsNode->AllocateDefaultPins();

		UEdGraphPin* AbsInPin  = AbsNode->FindPin(TEXT("A"), EGPD_Input);
		UEdGraphPin* AbsOutPin = AbsNode->FindPin(TEXT("ReturnValue"), EGPD_Output);
		if (!AbsInPin || !AbsOutPin)
		{
			OutError = TEXT("Abs node missing expected pins (A / ReturnValue).");
			return nullptr;
		}
		if (!RuleSchema->TryCreateConnection(VarOutPin, AbsInPin))
		{
			OutError = FString::Printf(TEXT("Failed to wire operand '%s' into Abs."), *Lhs);
			return nullptr;
		}
		LhsValuePin = AbsOutPin;
	}

	// 3) Comparison node.
	const FName CmpFnName = CompareOpToKismetFunctionName(Op);
	UFunction* CmpFn = UKismetMathLibrary::StaticClass()->FindFunctionByName(CmpFnName);
	if (!CmpFn)
	{
		OutError = FString::Printf(TEXT("Internal: comparison function '%s' not found."), *CmpFnName.ToString());
		return nullptr;
	}
	UK2Node_CallFunction* CmpNode = NewObject<UK2Node_CallFunction>(RuleGraph);
	CmpNode->SetFromFunction(CmpFn);
	RuleGraph->AddNode(CmpNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
	CmpNode->NodePosX = ColBaseX - 200;
	CmpNode->NodePosY = RowY;
	CmpNode->AllocateDefaultPins();

	UEdGraphPin* CmpAPin   = CmpNode->FindPin(TEXT("A"), EGPD_Input);
	UEdGraphPin* CmpBPin   = CmpNode->FindPin(TEXT("B"), EGPD_Input);
	UEdGraphPin* CmpRetPin = CmpNode->FindPin(TEXT("ReturnValue"), EGPD_Output);
	if (!CmpAPin || !CmpBPin || !CmpRetPin)
	{
		OutError = TEXT("Comparison node missing expected pins (A / B / ReturnValue).");
		return nullptr;
	}

	if (!RuleSchema->TryCreateConnection(LhsValuePin, CmpAPin))
	{
		OutError = TEXT("Failed to wire operand into comparison input A.");
		return nullptr;
	}
	// rhs constant -> default value on the B pin.
	CmpBPin->DefaultValue = FString::SanitizeFloat(Rhs);

	OutError.Empty();
	return CmpRetPin;
}

// Author a single float-compare rule into a transition's bound rule graph: builds the comparison
// subgraph and wires its bool result straight into result.bCanEnterTransition. Pure node authoring
// + wiring only; the CALLER owns the transaction, compile, and rollback. Returns true on full
// wire-through; on any failure fills OutError and the caller must roll the transaction back.
static bool AuthorCompareRuleNodes(
	UEdGraph* RuleGraph,
	UAnimGraphNode_TransitionResult* ResultNode,
	UEdGraphPin* ResultPin,
	const FString& Lhs,
	bool bUseAbs,
	const FString& Op,
	double Rhs,
	FString& OutError)
{
	if (!RuleGraph || !ResultNode || !ResultPin)
	{
		OutError = TEXT("Internal: null rule graph / result node / result pin.");
		return false;
	}

	const UEdGraphSchema* RuleSchema = RuleGraph->GetSchema();
	if (!RuleSchema)
	{
		OutError = TEXT("Rule graph has no schema.");
		return false;
	}

	RuleGraph->Modify();
	ResultPin->BreakAllPinLinks();

	UEdGraphPin* CmpRetPin = BuildCompareSubgraph(
		RuleGraph, Lhs, bUseAbs, Op, Rhs,
		/*ColBaseX=*/ResultNode->NodePosX, /*RowY=*/ResultNode->NodePosY, OutError);
	if (!CmpRetPin)
	{
		return false;
	}

	// Compare result -> bCanEnterTransition.
	if (!RuleSchema->TryCreateConnection(CmpRetPin, ResultPin))
	{
		OutError = TEXT("Failed to wire comparison result into bCanEnterTransition.");
		return false;
	}

	OutError.Empty();
	return true;
}

// Author a compound expression rule: build one compare subgraph per term, apply Not_PreBool to any
// negated term, then left-fold the per-term bool pins through chained BooleanAND/BooleanOR (per
// Combine) and wire the final folded pin into result.bCanEnterTransition. A single term degrades to
// a plain compare (no fold node). Before authoring, the existing rule nodes (everything except the
// result node) are removed so re-authoring does not litter the graph. Pure node authoring; the
// CALLER owns the transaction, compile, and rollback. Returns true on success; on any failure fills
// OutError and the caller must roll the transaction back.
static bool AuthorExpressionRuleNodes(
	UAnimBlueprint* ABP,
	UEdGraph* RuleGraph,
	UAnimGraphNode_TransitionResult* ResultNode,
	UEdGraphPin* ResultPin,
	const TArray<FExprTerm>& Terms,
	const FString& Combine,
	FString& OutError)
{
	if (!ABP || !RuleGraph || !ResultNode || !ResultPin)
	{
		OutError = TEXT("Internal: null ABP / rule graph / result node / result pin.");
		return false;
	}
	if (Terms.Num() == 0)
	{
		OutError = TEXT("kind:expression requires at least one term.");
		return false;
	}

	const UEdGraphSchema* RuleSchema = RuleGraph->GetSchema();
	if (!RuleSchema)
	{
		OutError = TEXT("Rule graph has no schema.");
		return false;
	}

	RuleGraph->Modify();
	ResultPin->BreakAllPinLinks();

	// Clear the prior rule graph (keep the result node) so re-authoring leaves no orphaned nodes.
	// RemoveNode breaks links + invokes DestroyNode (proper rule-subgraph teardown); inside the
	// caller's open transaction so rollback restores them. bDontRecompile — the caller's compile
	// is authoritative.
	{
		TArray<UEdGraphNode*> NodesToRemove;
		for (UEdGraphNode* N : RuleGraph->Nodes)
		{
			if (N && N != ResultNode)
			{
				NodesToRemove.Add(N);
			}
		}
		for (UEdGraphNode* N : NodesToRemove)
		{
			FBlueprintEditorUtils::RemoveNode(ABP, N, /*bDontRecompile=*/true);
		}
	}

	const int32 ColBaseX = ResultNode->NodePosX;
	const int32 BaseY = ResultNode->NodePosY;

	// 1) Build one comparison subgraph per term, capturing its bool result pin (optionally negated).
	TArray<UEdGraphPin*> TermBoolPins;
	TermBoolPins.Reserve(Terms.Num());
	for (int32 Ti = 0; Ti < Terms.Num(); ++Ti)
	{
		const FExprTerm& Term = Terms[Ti];
		const int32 RowY = BaseY + Ti * 150;

		UEdGraphPin* TermBoolPin = BuildCompareSubgraph(
			RuleGraph, Term.Variable, Term.bUseAbs, Term.Op, Term.Rhs, ColBaseX, RowY, OutError);
		if (!TermBoolPin)
		{
			OutError = FString::Printf(TEXT("term %d (%s): %s"), Ti, *Term.Variable, *OutError);
			return false;
		}

		// Optional per-term negation via Not_PreBool.
		if (Term.bNegate)
		{
			UFunction* NotFn = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Not_PreBool"));
			if (!NotFn)
			{
				OutError = TEXT("Internal: UKismetMathLibrary::Not_PreBool not found.");
				return false;
			}
			UK2Node_CallFunction* NotNode = NewObject<UK2Node_CallFunction>(RuleGraph);
			NotNode->SetFromFunction(NotFn);
			RuleGraph->AddNode(NotNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			NotNode->NodePosX = ColBaseX - 120;
			NotNode->NodePosY = RowY;
			NotNode->AllocateDefaultPins();

			UEdGraphPin* NotInPin  = NotNode->FindPin(TEXT("A"), EGPD_Input);
			UEdGraphPin* NotOutPin = NotNode->FindPin(TEXT("ReturnValue"), EGPD_Output);
			if (!NotInPin || !NotOutPin)
			{
				OutError = FString::Printf(TEXT("term %d: Not_PreBool node missing expected pins (A / ReturnValue)."), Ti);
				return false;
			}
			if (!RuleSchema->TryCreateConnection(TermBoolPin, NotInPin))
			{
				OutError = FString::Printf(TEXT("term %d: failed to wire comparison into Not_PreBool."), Ti);
				return false;
			}
			TermBoolPin = NotOutPin;
		}

		TermBoolPins.Add(TermBoolPin);
	}

	// 2) Single term -> wire its bool straight into the result (no fold node).
	UEdGraphPin* FinalBoolPin = TermBoolPins[0];

	// 3) Multiple terms -> left-fold through chained BooleanAND / BooleanOR.
	if (TermBoolPins.Num() > 1)
	{
		const bool bUseAnd = Combine.Equals(TEXT("and"), ESearchCase::IgnoreCase);
		const FName BoolFnName = bUseAnd ? FName(TEXT("BooleanAND")) : FName(TEXT("BooleanOR"));
		UFunction* BoolFn = UKismetMathLibrary::StaticClass()->FindFunctionByName(BoolFnName);
		if (!BoolFn)
		{
			OutError = FString::Printf(TEXT("Internal: UKismetMathLibrary::%s not found."), *BoolFnName.ToString());
			return false;
		}

		UEdGraphPin* AccPin = TermBoolPins[0];
		for (int32 Ki = 1; Ki < TermBoolPins.Num(); ++Ki)
		{
			UK2Node_CallFunction* FoldNode = NewObject<UK2Node_CallFunction>(RuleGraph);
			FoldNode->SetFromFunction(BoolFn);
			RuleGraph->AddNode(FoldNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			FoldNode->NodePosX = ColBaseX - 60;
			FoldNode->NodePosY = BaseY + Ki * 150;
			FoldNode->AllocateDefaultPins();

			UEdGraphPin* FoldAPin   = FoldNode->FindPin(TEXT("A"), EGPD_Input);
			UEdGraphPin* FoldBPin   = FoldNode->FindPin(TEXT("B"), EGPD_Input);
			UEdGraphPin* FoldRetPin = FoldNode->FindPin(TEXT("ReturnValue"), EGPD_Output);
			if (!FoldAPin || !FoldBPin || !FoldRetPin)
			{
				OutError = FString::Printf(TEXT("%s node missing expected pins (A / B / ReturnValue)."), *BoolFnName.ToString());
				return false;
			}
			if (!RuleSchema->TryCreateConnection(AccPin, FoldAPin) ||
				!RuleSchema->TryCreateConnection(TermBoolPins[Ki], FoldBPin))
			{
				OutError = FString::Printf(TEXT("Failed to wire term %d into the %s fold chain."), Ki, *BoolFnName.ToString());
				return false;
			}
			AccPin = FoldRetPin;
		}
		FinalBoolPin = AccPin;
	}

	// 4) Final folded (or single) bool -> bCanEnterTransition.
	if (!RuleSchema->TryCreateConnection(FinalBoolPin, ResultPin))
	{
		OutError = TEXT("Failed to wire expression result into bCanEnterTransition.");
		return false;
	}

	OutError.Empty();
	return true;
}

// Locate a transition node by from/to state names within a state machine graph.
static UAnimStateTransitionNode* FindTransitionNode(UAnimationStateMachineGraph* SMGraph, const FString& FromState, const FString& ToState)
{
	if (!SMGraph) return nullptr;
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		UAnimStateTransitionNode* TN = Cast<UAnimStateTransitionNode>(Node);
		if (!TN) continue;
		UAnimStateNodeBase* PrevState = TN->GetPreviousState();
		UAnimStateNodeBase* NextState = TN->GetNextState();
		if (PrevState && NextState &&
			PrevState->GetStateName() == FromState &&
			NextState->GetStateName() == ToState)
		{
			return TN;
		}
	}
	return nullptr;
}

// Harvest compiler errors/warnings from a results log into string arrays. Mirrors the
// canonical capture in MonolithCommonUITemplateActions.cpp.
static void HarvestCompilerMessages(const FCompilerResultsLog& Results, TArray<FString>& OutErrors, TArray<FString>& OutWarnings)
{
	for (const TSharedRef<FTokenizedMessage>& M : Results.Messages)
	{
		const FString Text = M->ToText().ToString();
		const EMessageSeverity::Type Sev = M->GetSeverity();
		if (Sev == EMessageSeverity::Error)
		{
			OutErrors.Add(Text);
		}
		else if (Sev == EMessageSeverity::Warning || Sev == EMessageSeverity::PerformanceWarning)
		{
			OutWarnings.Add(Text);
		}
	}
}

static FString BlueprintStatusToString(EBlueprintStatus Status)
{
	switch (Status)
	{
		case BS_UpToDate:             return TEXT("BS_UpToDate");
		case BS_UpToDateWithWarnings: return TEXT("BS_UpToDateWithWarnings");
		case BS_Dirty:                return TEXT("BS_Dirty");
		case BS_Error:                return TEXT("BS_Error");
		case BS_BeingCreated:         return TEXT("BS_BeingCreated");
		default:                      return TEXT("BS_Unknown");
	}
}

FMonolithActionResult FMonolithAnimationActions::HandleSetTransitionRule(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath    = Params->GetStringField(TEXT("asset_path"));
	FString MachineName  = Params->GetStringField(TEXT("machine_name"));
	FString FromState    = Params->GetStringField(TEXT("from_state"));
	FString ToState      = Params->GetStringField(TEXT("to_state"));

	if (MachineName.IsEmpty())  return FMonolithActionResult::Error(TEXT("Missing required parameter: machine_name"));
	if (FromState.IsEmpty())    return FMonolithActionResult::Error(TEXT("Missing required parameter: from_state"));
	if (ToState.IsEmpty())      return FMonolithActionResult::Error(TEXT("Missing required parameter: to_state"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	// --- Resolve the rule spec (back-compat) ---------------------------------------------
	// Legacy form: a bare `variable_name` (string) meaning a bool-variable rule. New form: a
	// structured `rule` object (or plain `rule` string). If both are absent, error.
	FParsedTransitionRule ParsedRule;
	FString LegacyVariableName = Params->GetStringField(TEXT("variable_name"));
	if (!Params->HasField(TEXT("rule")) && !LegacyVariableName.IsEmpty())
	{
		ParsedRule.Kind = FParsedTransitionRule::EKind::Bool;
		ParsedRule.Variable = LegacyVariableName;
	}
	else
	{
		ParsedRule = ParseTransitionRule(Params);
	}

	if (ParsedRule.Kind == FParsedTransitionRule::EKind::Invalid)
	{
		return FMonolithActionResult::Error(ParsedRule.ParseError.IsEmpty()
			? TEXT("Invalid rule. Provide 'variable_name' (legacy bool), or 'rule' as a string or { kind: bool|auto|compare|expression }.")
			: ParsedRule.ParseError);
	}

	// --- Operand validation (before touching the graph) ----------------------------------
	if (ParsedRule.Kind == FParsedTransitionRule::EKind::Bool)
	{
		const FBPVariableDescription* VarDesc = nullptr;
		for (const FBPVariableDescription& V : ABP->NewVariables)
		{
			if (V.VarName.ToString() == ParsedRule.Variable) { VarDesc = &V; break; }
		}
		if (VarDesc)
		{
			if (!VarDesc->VarType.PinCategory.ToString().Equals(TEXT("bool"), ESearchCase::IgnoreCase))
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Variable '%s' is type '%s', not bool. kind:bool rules require a boolean variable."),
					*ParsedRule.Variable, *VarDesc->VarType.PinCategory.ToString()));
			}
		}
		else if (!IsInheritedBlueprintVisibleBool(ABP, ParsedRule.Variable))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Variable '%s' not found in ABP (no BP variable or inherited Blueprint-visible bool). Use get_abp_variables to list available variables."), *ParsedRule.Variable));
		}
	}
	else if (ParsedRule.Kind == FParsedTransitionRule::EKind::Compare)
	{
		FString FoundCat;
		if (!IsUsableFloatOperand(ABP, ParsedRule.Variable, FoundCat))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Compare operand '%s' is not a usable numeric variable (no BP float/int or inherited Blueprint-visible float). Use get_abp_variables to list available variables."), *ParsedRule.Variable));
		}
	}
	else if (ParsedRule.Kind == FParsedTransitionRule::EKind::Expression)
	{
		if (ParsedRule.Terms.Num() == 0)
		{
			return FMonolithActionResult::Error(TEXT("kind:expression requires at least one term in 'terms'."));
		}
		// Each term's operand must be a usable numeric, exactly as kind:compare requires.
		for (int32 Ti = 0; Ti < ParsedRule.Terms.Num(); ++Ti)
		{
			FString FoundCat;
			if (!IsUsableFloatOperand(ABP, ParsedRule.Terms[Ti].Variable, FoundCat))
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("kind:expression term %d operand '%s' is not a usable numeric variable (no BP float/int or inherited Blueprint-visible float). Use get_abp_variables to list available variables."),
					Ti, *ParsedRule.Terms[Ti].Variable));
			}
		}
	}

	UAnimationStateMachineGraph* SMGraph = FindStateMachineGraphByName(ABP, MachineName);
	if (!SMGraph) return FMonolithActionResult::Error(FString::Printf(TEXT("State machine '%s' not found in ABP"), *MachineName));

	UAnimStateTransitionNode* TransNode = FindTransitionNode(SMGraph, FromState, ToState);
	if (!TransNode)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No transition found from '%s' to '%s'. Use add_transition first."), *FromState, *ToState));
	}

	// --- auto rule: no rule-graph wiring required ----------------------------------------
	if (ParsedRule.Kind == FParsedTransitionRule::EKind::Auto)
	{
		const bool bWasDirty = ABP->GetOutermost()->IsDirty();
		GEditor->BeginTransaction(FText::FromString(TEXT("Set Transition Rule (auto)")));
		TransNode->Modify();
		TransNode->bAutomaticRuleBasedOnSequencePlayerInState = true;
		GEditor->EndTransaction();

		FCompilerResultsLog Results; Results.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(ABP, EBlueprintCompileOptions::None, &Results);

		TArray<FString> Errors, Warnings;
		HarvestCompilerMessages(Results, Errors, Warnings);

		if (ABP->Status == BS_Error)
		{
			// Transaction already committed — revert via the editor undo buffer.
			GEditor->UndoTransaction();
			FKismetEditorUtilities::CompileBlueprint(ABP);
			ABP->GetOutermost()->SetDirtyFlag(bWasDirty);
			return FMonolithActionResult::Error(
				TEXT("Auto rule compiled with errors — rolled back, no package change."));
		}

		ABP->MarkPackageDirty();

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("asset_path"), AssetPath);
		Root->SetStringField(TEXT("machine_name"), MachineName);
		Root->SetStringField(TEXT("from_state"), FromState);
		Root->SetStringField(TEXT("to_state"), ToState);
		Root->SetStringField(TEXT("rule_kind"), TEXT("auto"));
		Root->SetStringField(TEXT("compile_status"), BlueprintStatusToString(ABP->Status));
		return FMonolithActionResult::Success(Root);
	}

	UEdGraph* RuleGraph = TransNode->GetBoundGraph();
	if (!RuleGraph)
	{
		return FMonolithActionResult::Error(TEXT("Transition has no bound rule graph"));
	}

	UAnimGraphNode_TransitionResult* ResultNode = nullptr;
	for (UEdGraphNode* N : RuleGraph->Nodes)
	{
		ResultNode = Cast<UAnimGraphNode_TransitionResult>(N);
		if (ResultNode) break;
	}
	if (!ResultNode)
	{
		return FMonolithActionResult::Error(TEXT("Transition rule graph has no result node"));
	}

	UEdGraphPin* ResultPin = ResultNode->FindPin(TEXT("bCanEnterTransition"), EGPD_Input);
	if (!ResultPin)
	{
		return FMonolithActionResult::Error(TEXT("Could not find bCanEnterTransition pin on transition result node"));
	}

	// --- Transaction-safe authoring ------------------------------------------------------
	// Rollback mechanism: record the package's pre-authoring dirty state, wrap the node
	// authoring in a transaction (RuleGraph->Modify() snapshots the prior graph state), then
	// compile AFTER closing the transaction. Two rollback paths:
	//   * authoring fails BEFORE EndTransaction (operand/pin wire) -> CancelTransaction(Index)
	//     discards the still-open transaction outright.
	//   * compile fails AFTER EndTransaction (BS_Error) -> UndoTransaction() reverts the
	//     committed authoring via the editor undo buffer.
	// Either path then recompiles to a clean state and restores the dirty flag to its prior
	// value, leaving NO half-authored graph and NO dirtied package. Only on success do we
	// MarkPackageDirty.
	const bool bWasDirty = ABP->GetOutermost()->IsDirty();
	const int32 TxIndex = GEditor->BeginTransaction(FText::FromString(TEXT("Set Transition Rule")));

	FString AuthorError;
	bool bAuthored = false;

	if (ParsedRule.Kind == FParsedTransitionRule::EKind::Bool)
	{
		RuleGraph->Modify();
		ResultPin->BreakAllPinLinks();

		UK2Node_VariableGet* VarGetNode = NewObject<UK2Node_VariableGet>(RuleGraph);
		VarGetNode->VariableReference.SetSelfMember(FName(*ParsedRule.Variable));
		RuleGraph->AddNode(VarGetNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		VarGetNode->NodePosX = ResultNode->NodePosX - 200;
		VarGetNode->NodePosY = ResultNode->NodePosY;
		VarGetNode->AllocateDefaultPins();

		UEdGraphPin* GetterOutputPin = FindVariableGetOutputPin(VarGetNode, ParsedRule.Variable);
		const UEdGraphSchema* RuleSchema = RuleGraph->GetSchema();
		if (GetterOutputPin && RuleSchema && RuleSchema->TryCreateConnection(GetterOutputPin, ResultPin))
		{
			bAuthored = true;
		}
		else
		{
			AuthorError = FString::Printf(TEXT("Failed to wire bool variable '%s' into the rule result."), *ParsedRule.Variable);
		}
	}
	else if (ParsedRule.Kind == FParsedTransitionRule::EKind::Compare)
	{
		bAuthored = AuthorCompareRuleNodes(RuleGraph, ResultNode, ResultPin,
			ParsedRule.Variable, ParsedRule.bUseAbs, ParsedRule.Op, ParsedRule.Rhs, AuthorError);
	}
	else if (ParsedRule.Kind == FParsedTransitionRule::EKind::Expression)
	{
		bAuthored = AuthorExpressionRuleNodes(ABP, RuleGraph, ResultNode, ResultPin,
			ParsedRule.Terms, ParsedRule.Combine, AuthorError);
	}

	if (!bAuthored)
	{
		GEditor->CancelTransaction(TxIndex);
		// Recompile to restore a clean, consistent state, then reset the dirty flag.
		FKismetEditorUtilities::CompileBlueprint(ABP);
		ABP->GetOutermost()->SetDirtyFlag(bWasDirty);
		return FMonolithActionResult::Error(FString::Printf(TEXT("Rule authoring failed (rolled back, no package change): %s"), *AuthorError));
	}

	GEditor->EndTransaction();

	// Compile + harvest. If the compile errored, roll back the authored nodes.
	FCompilerResultsLog Results; Results.bSilentMode = true;
	FKismetEditorUtilities::CompileBlueprint(ABP, EBlueprintCompileOptions::None, &Results);

	TArray<FString> Errors, Warnings;
	HarvestCompilerMessages(Results, Errors, Warnings);

	if (ABP->Status == BS_Error)
	{
		// Transaction already committed — revert via the editor undo buffer, recompile to a
		// clean state, restore dirty flag.
		GEditor->UndoTransaction();
		FKismetEditorUtilities::CompileBlueprint(ABP);
		ABP->GetOutermost()->SetDirtyFlag(bWasDirty);

		TArray<TSharedPtr<FJsonValue>> ErrArr;
		for (const FString& E : Errors) { ErrArr.Add(MakeShared<FJsonValueString>(E)); }
		TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
		ErrObj->SetArrayField(TEXT("compile_errors"), ErrArr);
		return FMonolithActionResult::Error(
			TEXT("Rule compiled with errors — rolled back, no package change. See compile_errors."))
			.WithErrorData(ErrObj);
	}

	ABP->MarkPackageDirty();

	// --- Success payload (rule-graph readback of the authored comparison) ----------------
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("machine_name"), MachineName);
	Root->SetStringField(TEXT("from_state"), FromState);
	Root->SetStringField(TEXT("to_state"), ToState);
	Root->SetStringField(TEXT("compile_status"), BlueprintStatusToString(ABP->Status));

	if (ParsedRule.Kind == FParsedTransitionRule::EKind::Bool)
	{
		Root->SetStringField(TEXT("rule_kind"), TEXT("bool"));
		Root->SetStringField(TEXT("variable_name"), ParsedRule.Variable);
		Root->SetBoolField(TEXT("pin_wired"), true);
	}
	else if (ParsedRule.Kind == FParsedTransitionRule::EKind::Compare)
	{
		Root->SetStringField(TEXT("rule_kind"), TEXT("compare"));
		const FString LhsDisplay = ParsedRule.bUseAbs
			? FString::Printf(TEXT("Abs(%s)"), *ParsedRule.Variable)
			: ParsedRule.Variable;
		Root->SetStringField(TEXT("lhs"), LhsDisplay);
		Root->SetStringField(TEXT("operand"), ParsedRule.Variable);
		Root->SetBoolField(TEXT("abs"), ParsedRule.bUseAbs);
		Root->SetStringField(TEXT("op"), ParsedRule.Op);
		Root->SetNumberField(TEXT("rhs"), ParsedRule.Rhs);
		Root->SetStringField(TEXT("comparison"),
			FString::Printf(TEXT("%s %s %s"), *LhsDisplay, *ParsedRule.Op, *FString::SanitizeFloat(ParsedRule.Rhs)));
		Root->SetBoolField(TEXT("pin_wired"), true);
	}
	else // Expression
	{
		Root->SetStringField(TEXT("rule_kind"), TEXT("expression"));
		Root->SetStringField(TEXT("combine"), ParsedRule.Combine);
		Root->SetNumberField(TEXT("term_count"), ParsedRule.Terms.Num());

		TArray<TSharedPtr<FJsonValue>> TermArr;
		TArray<FString> CompareStrings;
		for (const FExprTerm& Term : ParsedRule.Terms)
		{
			const FString LhsDisplay = Term.bUseAbs ? FString::Printf(TEXT("Abs(%s)"), *Term.Variable) : Term.Variable;
			const FString Cmp = FString::Printf(TEXT("%s%s %s %s"),
				Term.bNegate ? TEXT("NOT ") : TEXT(""),
				*LhsDisplay, *Term.Op, *FString::SanitizeFloat(Term.Rhs));
			CompareStrings.Add(Cmp);

			TSharedPtr<FJsonObject> TermObj = MakeShared<FJsonObject>();
			TermObj->SetStringField(TEXT("lhs"), LhsDisplay);
			TermObj->SetStringField(TEXT("operand"), Term.Variable);
			TermObj->SetBoolField(TEXT("abs"), Term.bUseAbs);
			TermObj->SetStringField(TEXT("op"), Term.Op);
			TermObj->SetNumberField(TEXT("rhs"), Term.Rhs);
			TermObj->SetBoolField(TEXT("negate"), Term.bNegate);
			TermObj->SetStringField(TEXT("comparison"), Cmp);
			TermArr.Add(MakeShared<FJsonValueObject>(TermObj));
		}
		Root->SetArrayField(TEXT("terms"), TermArr);

		const FString Joiner = ParsedRule.Combine.Equals(TEXT("or"), ESearchCase::IgnoreCase) ? TEXT(" OR ") : TEXT(" AND ");
		Root->SetStringField(TEXT("expression"), FString::Join(CompareStrings, *Joiner));
		Root->SetBoolField(TEXT("pin_wired"), true);
	}

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings) { WarnArr.Add(MakeShared<FJsonValueString>(W)); }
		Root->SetArrayField(TEXT("compile_warnings"), WarnArr);
	}
	return FMonolithActionResult::Success(Root);
}

// Map a KismetMathLibrary double-comparison function name back to its operator token (inverse of
// CompareOpToKismetFunctionName). Returns empty for a non-comparison function.
static FString KismetCompareFnToOp(const FName& N)
{
	if (N == TEXT("Greater_DoubleDouble"))      return TEXT(">");
	if (N == TEXT("Less_DoubleDouble"))         return TEXT("<");
	if (N == TEXT("GreaterEqual_DoubleDouble")) return TEXT(">=");
	if (N == TEXT("LessEqual_DoubleDouble"))    return TEXT("<=");
	if (N == TEXT("EqualEqual_DoubleDouble"))   return TEXT("==");
	if (N == TEXT("NotEqual_DoubleDouble"))     return TEXT("!=");
	return FString();
}

// Decode a single bool-producing node (a comparison CallFunction, optionally fronted by Not_PreBool)
// into a structured term JSON object { lhs, operand, abs, op, rhs, negate, comparison }. Returns the
// object on a recognized comparison term, or nullptr if the node is not a decodable compare term.
static TSharedPtr<FJsonObject> DecodeExpressionTerm(UEdGraphNode* BoolNode)
{
	bool bNegate = false;
	UK2Node_CallFunction* CmpNode = Cast<UK2Node_CallFunction>(BoolNode);

	// Peel an optional Not_PreBool wrapper to reach the comparison node.
	if (CmpNode && CmpNode->FunctionReference.GetMemberName() == TEXT("Not_PreBool"))
	{
		bNegate = true;
		UK2Node_CallFunction* NotNode = CmpNode;
		CmpNode = nullptr;
		if (UEdGraphPin* NotAPin = NotNode->FindPin(TEXT("A"), EGPD_Input))
		{
			if (NotAPin->LinkedTo.Num() > 0 && NotAPin->LinkedTo[0])
			{
				CmpNode = Cast<UK2Node_CallFunction>(NotAPin->LinkedTo[0]->GetOwningNode());
			}
		}
	}

	if (!CmpNode) return nullptr;
	const FString Op = KismetCompareFnToOp(CmpNode->FunctionReference.GetMemberName());
	if (Op.IsEmpty()) return nullptr;

	double Rhs = 0.0;
	if (UEdGraphPin* BPin = CmpNode->FindPin(TEXT("B"), EGPD_Input))
	{
		Rhs = FCString::Atod(*BPin->DefaultValue);
	}

	bool bUsesAbs = false;
	FString OperandName;
	if (UEdGraphPin* APin = CmpNode->FindPin(TEXT("A"), EGPD_Input))
	{
		if (APin->LinkedTo.Num() > 0 && APin->LinkedTo[0])
		{
			UEdGraphNode* UpstreamNode = APin->LinkedTo[0]->GetOwningNode();
			if (UK2Node_CallFunction* MaybeAbs = Cast<UK2Node_CallFunction>(UpstreamNode))
			{
				if (MaybeAbs->FunctionReference.GetMemberName() == TEXT("Abs"))
				{
					bUsesAbs = true;
					if (UEdGraphPin* AbsAPin = MaybeAbs->FindPin(TEXT("A"), EGPD_Input))
					{
						if (AbsAPin->LinkedTo.Num() > 0 && AbsAPin->LinkedTo[0])
						{
							if (UK2Node_VariableGet* InnerVar = Cast<UK2Node_VariableGet>(AbsAPin->LinkedTo[0]->GetOwningNode()))
							{
								OperandName = InnerVar->VariableReference.GetMemberName().ToString();
							}
						}
					}
				}
			}
			else if (UK2Node_VariableGet* DirectVar = Cast<UK2Node_VariableGet>(UpstreamNode))
			{
				OperandName = DirectVar->VariableReference.GetMemberName().ToString();
			}
		}
	}

	const FString LhsDisplay = bUsesAbs ? FString::Printf(TEXT("Abs(%s)"), *OperandName) : OperandName;
	TSharedPtr<FJsonObject> TermObj = MakeShared<FJsonObject>();
	TermObj->SetStringField(TEXT("lhs"), LhsDisplay);
	TermObj->SetStringField(TEXT("operand"), OperandName);
	TermObj->SetBoolField(TEXT("abs"), bUsesAbs);
	TermObj->SetStringField(TEXT("op"), Op);
	TermObj->SetNumberField(TEXT("rhs"), Rhs);
	TermObj->SetBoolField(TEXT("negate"), bNegate);
	TermObj->SetStringField(TEXT("comparison"),
		FString::Printf(TEXT("%s%s %s %s"), bNegate ? TEXT("NOT ") : TEXT(""), *LhsDisplay, *Op, *FString::SanitizeFloat(Rhs)));
	return TermObj;
}

// Read back a transition's current rule (kind + operands + comparison) as structured data.
// Inspects the transition's bound rule graph: bAutomaticRuleBasedOnSequencePlayerInState ->
// auto; a single bool VariableGet feeding the result -> bool; a comparison CallFunction (with
// optional Abs upstream) -> compare; a BooleanAND/BooleanOR fold (or a single Not_PreBool) feeding
// the result -> expression with decoded terms. Anything else is reported as kind:custom with the
// node titles, rather than failing.
FMonolithActionResult FMonolithAnimationActions::HandleGetTransitionRule(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath   = Params->GetStringField(TEXT("asset_path"));
	FString MachineName = Params->GetStringField(TEXT("machine_name"));
	FString FromState   = Params->GetStringField(TEXT("from_state"));
	FString ToState     = Params->GetStringField(TEXT("to_state"));

	if (MachineName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: machine_name"));
	if (FromState.IsEmpty())   return FMonolithActionResult::Error(TEXT("Missing required parameter: from_state"));
	if (ToState.IsEmpty())     return FMonolithActionResult::Error(TEXT("Missing required parameter: to_state"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	UAnimationStateMachineGraph* SMGraph = FindStateMachineGraphByName(ABP, MachineName);
	if (!SMGraph) return FMonolithActionResult::Error(FString::Printf(TEXT("State machine '%s' not found in ABP"), *MachineName));

	UAnimStateTransitionNode* TransNode = FindTransitionNode(SMGraph, FromState, ToState);
	if (!TransNode)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No transition found from '%s' to '%s'."), *FromState, *ToState));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("machine_name"), MachineName);
	Root->SetStringField(TEXT("from_state"), FromState);
	Root->SetStringField(TEXT("to_state"), ToState);

	if (TransNode->bAutomaticRuleBasedOnSequencePlayerInState)
	{
		Root->SetStringField(TEXT("rule_kind"), TEXT("auto"));
		return FMonolithActionResult::Success(Root);
	}

	UEdGraph* RuleGraph = TransNode->GetBoundGraph();
	UAnimGraphNode_TransitionResult* ResultNode = nullptr;
	if (RuleGraph)
	{
		for (UEdGraphNode* N : RuleGraph->Nodes)
		{
			ResultNode = Cast<UAnimGraphNode_TransitionResult>(N);
			if (ResultNode) break;
		}
	}
	UEdGraphPin* ResultPin = ResultNode ? ResultNode->FindPin(TEXT("bCanEnterTransition"), EGPD_Input) : nullptr;

	if (!ResultPin || ResultPin->LinkedTo.Num() == 0)
	{
		Root->SetStringField(TEXT("rule_kind"), TEXT("none"));
		return FMonolithActionResult::Success(Root);
	}

	UEdGraphNode* DrivingNode = ResultPin->LinkedTo[0] ? ResultPin->LinkedTo[0]->GetOwningNode() : nullptr;

	// Bool: a VariableGet drives the result directly.
	if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(DrivingNode))
	{
		Root->SetStringField(TEXT("rule_kind"), TEXT("bool"));
		Root->SetStringField(TEXT("variable_name"), VarGet->VariableReference.GetMemberName().ToString());
		return FMonolithActionResult::Success(Root);
	}

	// Expression: a BooleanAND / BooleanOR fold (multi-term), or a single Not_PreBool (one negated
	// term), drives the result. Walk the fold chain back to its leaf comparison terms.
	if (UK2Node_CallFunction* DrivingFn = Cast<UK2Node_CallFunction>(DrivingNode))
	{
		const FName DrivingFnName = DrivingFn->FunctionReference.GetMemberName();
		const bool bIsAnd = DrivingFnName == TEXT("BooleanAND");
		const bool bIsOr  = DrivingFnName == TEXT("BooleanOR");
		const bool bIsNot = DrivingFnName == TEXT("Not_PreBool");

		if (bIsAnd || bIsOr || bIsNot)
		{
			// Collect leaf bool-producing nodes in author order. The fold is a strict left-fold:
			// each BooleanAND/OR node's A pin links to the previous accumulator (the upstream fold
			// node, or the first term) and its B pin links to the next term. Walk the chain of A
			// pins iteratively, gathering B-pin terms, then the innermost A leaf is term 0. A leaf
			// is a compare or Not_PreBool node.
			auto UpstreamOf = [](UK2Node_CallFunction* Fn, const TCHAR* PinName) -> UEdGraphNode*
			{
				if (UEdGraphPin* P = Fn->FindPin(PinName, EGPD_Input))
				{
					if (P->LinkedTo.Num() > 0 && P->LinkedTo[0]) { return P->LinkedTo[0]->GetOwningNode(); }
				}
				return nullptr;
			};

			TArray<UEdGraphNode*> LeafNodes; // reverse author order while walking; reversed below
			UEdGraphNode* Cursor = DrivingNode;
			int32 FoldGuard = 0;
			while (Cursor && FoldGuard++ < 256)
			{
				UK2Node_CallFunction* Fn = Cast<UK2Node_CallFunction>(Cursor);
				const FName FnName = Fn ? Fn->FunctionReference.GetMemberName() : NAME_None;
				if (Fn && (FnName == TEXT("BooleanAND") || FnName == TEXT("BooleanOR")))
				{
					LeafNodes.Add(UpstreamOf(Fn, TEXT("B")));   // this fold's right-hand term
					Cursor = UpstreamOf(Fn, TEXT("A"));          // descend into the accumulator
				}
				else
				{
					LeafNodes.Add(Cursor); // innermost leaf (term 0) or the single Not_PreBool
					break;
				}
			}
			// Restore author order (term 0 .. term N-1) — the walk gathered them innermost-last.
			for (int32 Lo = 0, Hi = LeafNodes.Num() - 1; Lo < Hi; ++Lo, --Hi)
			{
				UEdGraphNode* Tmp = LeafNodes[Lo];
				LeafNodes[Lo] = LeafNodes[Hi];
				LeafNodes[Hi] = Tmp;
			}

			TArray<TSharedPtr<FJsonValue>> TermArr;
			TArray<FString> CompareStrings;
			bool bAllDecoded = true;
			for (UEdGraphNode* Leaf : LeafNodes)
			{
				TSharedPtr<FJsonObject> TermObj = DecodeExpressionTerm(Leaf);
				if (!TermObj) { bAllDecoded = false; break; }
				CompareStrings.Add(TermObj->GetStringField(TEXT("comparison")));
				TermArr.Add(MakeShared<FJsonValueObject>(TermObj));
			}

			if (bAllDecoded && TermArr.Num() > 0)
			{
				const FString Combine = bIsOr ? TEXT("or") : TEXT("and");
				Root->SetStringField(TEXT("rule_kind"), TEXT("expression"));
				Root->SetStringField(TEXT("combine"), Combine);
				Root->SetNumberField(TEXT("term_count"), TermArr.Num());
				Root->SetArrayField(TEXT("terms"), TermArr);
				const FString Joiner = bIsOr ? TEXT(" OR ") : TEXT(" AND ");
				Root->SetStringField(TEXT("expression"), FString::Join(CompareStrings, *Joiner));
				return FMonolithActionResult::Success(Root);
			}
			// Fall through to the compare/custom decode below if the chain was not cleanly decodable.
		}
	}

	// Compare: a comparison CallFunction drives the result.
	if (UK2Node_CallFunction* CmpNode = Cast<UK2Node_CallFunction>(DrivingNode))
	{
		const FName FnName = CmpNode->FunctionReference.GetMemberName();
		const FString Op = KismetCompareFnToOp(FnName);
		if (Op.IsEmpty())
		{
			Root->SetStringField(TEXT("rule_kind"), TEXT("custom"));
			Root->SetStringField(TEXT("driving_function"), FnName.ToString());
			return FMonolithActionResult::Success(Root);
		}

		Root->SetStringField(TEXT("rule_kind"), TEXT("compare"));
		Root->SetStringField(TEXT("op"), Op);

		// rhs = default value on the B pin.
		double Rhs = 0.0;
		if (UEdGraphPin* BPin = CmpNode->FindPin(TEXT("B"), EGPD_Input))
		{
			Rhs = FCString::Atod(*BPin->DefaultValue);
		}
		Root->SetNumberField(TEXT("rhs"), Rhs);

		// lhs operand: trace the A pin upstream through an optional Abs node to the VariableGet.
		bool bUsesAbs = false;
		FString OperandName;
		if (UEdGraphPin* APin = CmpNode->FindPin(TEXT("A"), EGPD_Input))
		{
			if (APin->LinkedTo.Num() > 0 && APin->LinkedTo[0])
			{
				UEdGraphNode* UpstreamNode = APin->LinkedTo[0]->GetOwningNode();
				if (UK2Node_CallFunction* MaybeAbs = Cast<UK2Node_CallFunction>(UpstreamNode))
				{
					if (MaybeAbs->FunctionReference.GetMemberName() == TEXT("Abs"))
					{
						bUsesAbs = true;
						if (UEdGraphPin* AbsAPin = MaybeAbs->FindPin(TEXT("A"), EGPD_Input))
						{
							if (AbsAPin->LinkedTo.Num() > 0 && AbsAPin->LinkedTo[0])
							{
								if (UK2Node_VariableGet* InnerVar = Cast<UK2Node_VariableGet>(AbsAPin->LinkedTo[0]->GetOwningNode()))
								{
									OperandName = InnerVar->VariableReference.GetMemberName().ToString();
								}
							}
						}
					}
				}
				else if (UK2Node_VariableGet* DirectVar = Cast<UK2Node_VariableGet>(UpstreamNode))
				{
					OperandName = DirectVar->VariableReference.GetMemberName().ToString();
				}
			}
		}
		Root->SetBoolField(TEXT("abs"), bUsesAbs);
		Root->SetStringField(TEXT("operand"), OperandName);
		const FString LhsDisplay = bUsesAbs ? FString::Printf(TEXT("Abs(%s)"), *OperandName) : OperandName;
		Root->SetStringField(TEXT("lhs"), LhsDisplay);
		Root->SetStringField(TEXT("comparison"),
			FString::Printf(TEXT("%s %s %s"), *LhsDisplay, *Op, *FString::SanitizeFloat(Rhs)));
		return FMonolithActionResult::Success(Root);
	}

	// Unrecognized driver — report it without failing.
	Root->SetStringField(TEXT("rule_kind"), TEXT("custom"));
	if (DrivingNode)
	{
		Root->SetStringField(TEXT("driving_node_title"), DrivingNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
	}
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Wave 16 — State Machine Authoring (#13 create_state_machine / #14 build_state_machine)
// ---------------------------------------------------------------------------

// Find the top-level anim graph in an ABP. If GraphName is non-empty, require an
// exact name match; otherwise return the first graph whose schema is a
// UAnimationGraphSchema (handles layered ABPs that have multiple anim graphs).
static UEdGraph* FindAnimGraph(UAnimBlueprint* ABP, const FString& GraphName)
{
	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (!Graph) continue;
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema || !Schema->IsA<UAnimationGraphSchema>()) continue;
		if (GraphName.IsEmpty() || Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}
	return nullptr;
}

// Spawn a state machine node into AnimGraph. Renames the auto-created
// EditorStateMachineGraph (the SM node title derives from it) to DesiredName.
// Caller owns the transaction / compile / save. Returns the spawned node.
static UAnimGraphNode_StateMachine* SpawnStateMachineNode(UEdGraph* AnimGraph, const FString& DesiredName, int32 PosX, int32 PosY)
{
	UAnimGraphNode_StateMachine* SMNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimGraphNode_StateMachine>(
		AnimGraph,
		NewObject<UAnimGraphNode_StateMachine>(AnimGraph),
		FVector2f(static_cast<float>(PosX), static_cast<float>(PosY)),
		/*bSelectNewNode=*/false);

	if (!SMNode || !SMNode->EditorStateMachineGraph)
	{
		return nullptr;
	}

	if (!DesiredName.IsEmpty())
	{
		TSharedPtr<INameValidatorInterface> NameValidator = FNameValidatorFactory::MakeValidator(SMNode);
		FBlueprintEditorUtils::RenameGraphWithSuggestion(SMNode->EditorStateMachineGraph, NameValidator, DesiredName);
	}
	return SMNode;
}

// Best-effort readback of a state machine graph's states + transitions for result JSON.
static void StateMachineGraphToJson(UAnimationStateMachineGraph* SMGraph, TSharedPtr<FJsonObject>& OutRoot)
{
	TArray<TSharedPtr<FJsonValue>> StatesArr;
	TArray<TSharedPtr<FJsonValue>> TransArr;
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
		{
			StatesArr.Add(MakeShared<FJsonValueString>(StateNode->GetStateName()));
		}
		else if (UAnimStateTransitionNode* TN = Cast<UAnimStateTransitionNode>(Node))
		{
			UAnimStateNodeBase* Prev = TN->GetPreviousState();
			UAnimStateNodeBase* Next = TN->GetNextState();
			if (Prev && Next)
			{
				TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
				T->SetStringField(TEXT("from"), Prev->GetStateName());
				T->SetStringField(TEXT("to"), Next->GetStateName());
				TransArr.Add(MakeShared<FJsonValueObject>(T));
			}
		}
	}
	OutRoot->SetArrayField(TEXT("states"), StatesArr);
	OutRoot->SetArrayField(TEXT("transitions"), TransArr);
}

FMonolithActionResult FMonolithAnimationActions::HandleCreateStateMachine(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SMName    = Params->HasField(TEXT("state_machine_name")) ? Params->GetStringField(TEXT("state_machine_name")) : TEXT("New State Machine");
	FString GraphName = Params->HasField(TEXT("graph_name")) ? Params->GetStringField(TEXT("graph_name")) : FString();

	double TempVal;
	int32 PosX = 200;
	int32 PosY = 200;
	if (Params->TryGetNumberField(TEXT("position_x"), TempVal)) PosX = static_cast<int32>(TempVal);
	if (Params->TryGetNumberField(TEXT("position_y"), TempVal)) PosY = static_cast<int32>(TempVal);

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	UEdGraph* AnimGraph = FindAnimGraph(ABP, GraphName);
	if (!AnimGraph)
	{
		return FMonolithActionResult::Error(GraphName.IsEmpty()
			? FString::Printf(TEXT("No anim graph (UAnimationGraphSchema) found in ABP '%s'"), *AssetPath)
			: FString::Printf(TEXT("Anim graph '%s' not found in ABP '%s'"), *GraphName, *AssetPath));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Create State Machine")));
	AnimGraph->Modify();

	UAnimGraphNode_StateMachine* SMNode = SpawnStateMachineNode(AnimGraph, SMName, PosX, PosY);
	if (!SMNode)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("Failed to spawn state machine node (SpawnNodeFromTemplate returned null or EditorStateMachineGraph was not created)"));
	}

	GEditor->EndTransaction();

	FKismetEditorUtilities::CompileBlueprint(ABP);
	ABP->MarkPackageDirty();

	UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);

	// The SM node's display title derives from the SM graph name.
	FString FinalTitle = SMNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	int32 NL;
	if (FinalTitle.FindChar(TEXT('\n'), NL)) FinalTitle.LeftInline(NL);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("anim_graph"), AnimGraph->GetName());
	Root->SetStringField(TEXT("state_machine_name"), FinalTitle);
	Root->SetStringField(TEXT("state_machine_graph"), SMGraph ? SMGraph->GetName() : TEXT(""));
	if (SMGraph)
	{
		StateMachineGraphToJson(SMGraph, Root);
	}
	return FMonolithActionResult::Success(Root);
}

// Add a state into SMGraph and rename its BoundGraph to StateName. Mirrors
// HandleAddStateToMachine. Caller owns transaction/compile/save.
static UAnimStateNode* BuilderAddState(UAnimationStateMachineGraph* SMGraph, const FString& StateName)
{
	UAnimStateNode* NewNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(
		SMGraph,
		NewObject<UAnimStateNode>(SMGraph),
		FVector2f(0.0f, 0.0f),
		/*bSelectNewNode=*/false);
	if (!NewNode || !NewNode->BoundGraph) return nullptr;

	TSharedPtr<INameValidatorInterface> NameValidator = FNameValidatorFactory::MakeValidator(NewNode);
	FBlueprintEditorUtils::RenameGraphWithSuggestion(NewNode->BoundGraph, NameValidator, StateName);
	return NewNode;
}

// Wire a sequence player into a state's inner anim graph and connect it to the
// state result pose pin. Returns true on full wire-up.
static bool BuilderSetStateAnimation(UAnimStateNode* StateNode, UAnimSequenceBase* Sequence)
{
	UEdGraph* StateGraph = StateNode->GetBoundGraph();
	if (!StateGraph) return false;

	UEdGraphPin* PoseSinkPin = StateNode->GetPoseSinkPinInsideState();
	if (!PoseSinkPin) return false;

	UAnimGraphNode_SequencePlayer* SeqNode = NewObject<UAnimGraphNode_SequencePlayer>(StateGraph, NAME_None, RF_Transactional);
	StateGraph->AddNode(SeqNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	SeqNode->CreateNewGuid();
	SeqNode->NodePosX = PoseSinkPin->GetOwningNode()->NodePosX - 300;
	SeqNode->NodePosY = PoseSinkPin->GetOwningNode()->NodePosY;
	// Bind the sequence before pin generation (mirrors the surgery-file ordering).
	SeqNode->Node.SetSequence(Sequence);
	SeqNode->AllocateDefaultPins();
	SeqNode->PostPlacedNewNode(); // UEdGraphNode::PostPlacedNewNode() takes no args in UE 5.7

	// Anim asset players expose a single output pose pin.
	UEdGraphPin* OutputPosePin = nullptr;
	for (UEdGraphPin* Pin : SeqNode->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output)
		{
			OutputPosePin = Pin;
			break;
		}
	}
	if (!OutputPosePin) return false;

	const UEdGraphSchema* Schema = StateGraph->GetSchema();
	return Schema ? Schema->TryCreateConnection(OutputPosePin, PoseSinkPin) : false;
}

FMonolithActionResult FMonolithAnimationActions::HandleBuildStateMachine(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SMName    = Params->HasField(TEXT("state_machine_name")) ? Params->GetStringField(TEXT("state_machine_name")) : TEXT("New State Machine");
	FString GraphName = Params->HasField(TEXT("graph_name")) ? Params->GetStringField(TEXT("graph_name")) : FString();
	FString EntryState = Params->HasField(TEXT("entry_state")) ? Params->GetStringField(TEXT("entry_state")) : FString();

	const TArray<TSharedPtr<FJsonValue>>* StatesJson = nullptr;
	if (!Params->TryGetArrayField(TEXT("states"), StatesJson) || !StatesJson || StatesJson->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: states (array of {name, animation?})"));
	}

	const TArray<TSharedPtr<FJsonValue>>* TransJson = nullptr;
	Params->TryGetArrayField(TEXT("transitions"), TransJson);

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	UEdGraph* AnimGraph = FindAnimGraph(ABP, GraphName);
	if (!AnimGraph)
	{
		return FMonolithActionResult::Error(GraphName.IsEmpty()
			? FString::Printf(TEXT("No anim graph (UAnimationGraphSchema) found in ABP '%s'"), *AssetPath)
			: FString::Printf(TEXT("Anim graph '%s' not found in ABP '%s'"), *GraphName, *AssetPath));
	}

	TArray<TSharedPtr<FJsonValue>> StatesReport;
	TArray<TSharedPtr<FJsonValue>> TransReport;

	GEditor->BeginTransaction(FText::FromString(TEXT("Build State Machine")));
	AnimGraph->Modify();

	// 1. Create the state machine.
	UAnimGraphNode_StateMachine* SMNode = SpawnStateMachineNode(AnimGraph, SMName, 200, 200);
	if (!SMNode || !SMNode->EditorStateMachineGraph)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("Failed to spawn state machine node"));
	}
	UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
	if (!SMGraph)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("State machine graph has unexpected type"));
	}
	SMGraph->Modify();

	// 2. Add states (+ optional animation).
	for (const TSharedPtr<FJsonValue>& SV : *StatesJson)
	{
		const TSharedPtr<FJsonObject>* StateObj = nullptr;
		if (!SV.IsValid() || !SV->TryGetObject(StateObj) || !StateObj || !(*StateObj)->HasField(TEXT("name")))
		{
			continue;
		}
		FString StateName = (*StateObj)->GetStringField(TEXT("name"));

		TSharedPtr<FJsonObject> Rep = MakeShared<FJsonObject>();
		Rep->SetStringField(TEXT("name"), StateName);

		if (FindStateNodeByName(SMGraph, StateName))
		{
			Rep->SetBoolField(TEXT("created"), false);
			Rep->SetStringField(TEXT("note"), TEXT("duplicate state name skipped"));
			StatesReport.Add(MakeShared<FJsonValueObject>(Rep));
			continue;
		}

		UAnimStateNode* StateNode = BuilderAddState(SMGraph, StateName);
		if (!StateNode)
		{
			Rep->SetBoolField(TEXT("created"), false);
			Rep->SetStringField(TEXT("note"), TEXT("spawn failed"));
			StatesReport.Add(MakeShared<FJsonValueObject>(Rep));
			continue;
		}
		Rep->SetBoolField(TEXT("created"), true);

		// Optional animation wiring.
		if ((*StateObj)->HasField(TEXT("animation")))
		{
			FString AnimPath = (*StateObj)->GetStringField(TEXT("animation"));
			if (!AnimPath.IsEmpty())
			{
				UAnimSequenceBase* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequenceBase>(AnimPath);
				if (!Seq)
				{
					Rep->SetStringField(TEXT("animation_note"), FString::Printf(TEXT("animation asset not found: %s"), *AnimPath));
				}
				else
				{
					bool bWired = BuilderSetStateAnimation(StateNode, Seq);
					Rep->SetBoolField(TEXT("animation_wired"), bWired);
					if (!bWired)
					{
						Rep->SetStringField(TEXT("animation_note"), TEXT("sequence player created but pose pin wiring failed"));
					}
				}
			}
		}
		StatesReport.Add(MakeShared<FJsonValueObject>(Rep));
	}

	// 3. Entry state wiring.
	bool bEntryWired = false;
	if (!EntryState.IsEmpty())
	{
		UAnimStateNode* EntryTarget = FindStateNodeByName(SMGraph, EntryState);
		UAnimStateEntryNode* EntryNode = nullptr;
		for (UEdGraphNode* N : SMGraph->Nodes)
		{
			EntryNode = Cast<UAnimStateEntryNode>(N);
			if (EntryNode) break;
		}
		if (EntryTarget && EntryNode)
		{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			UEdGraphPin* EntryOut = EntryNode->GetOutputPin();
#else
			// UE 5.6's UAnimStateEntryNode has no GetOutputPin() convenience accessor yet.
			UEdGraphPin* EntryOut = nullptr;
			for (UEdGraphPin* P : EntryNode->Pins)
			{
				if (P && P->Direction == EGPD_Output) { EntryOut = P; break; }
			}
#endif
			UEdGraphPin* StateIn  = EntryTarget->GetInputPin();
			const UAnimationStateMachineSchema* Schema = Cast<UAnimationStateMachineSchema>(SMGraph->GetSchema());
			if (EntryOut && StateIn && Schema)
			{
				bEntryWired = Schema->TryCreateConnection(EntryOut, StateIn);
			}
		}
	}

	// 4. Transitions (+ optional rule).
	if (TransJson)
	{
		const UAnimationStateMachineSchema* SMSchema = Cast<UAnimationStateMachineSchema>(SMGraph->GetSchema());
		for (const TSharedPtr<FJsonValue>& TV : *TransJson)
		{
			const TSharedPtr<FJsonObject>* TObj = nullptr;
			if (!TV.IsValid() || !TV->TryGetObject(TObj) || !TObj) continue;
			FString From = (*TObj)->HasField(TEXT("from")) ? (*TObj)->GetStringField(TEXT("from")) : FString();
			FString To   = (*TObj)->HasField(TEXT("to"))   ? (*TObj)->GetStringField(TEXT("to"))   : FString();

			TSharedPtr<FJsonObject> Rep = MakeShared<FJsonObject>();
			Rep->SetStringField(TEXT("from"), From);
			Rep->SetStringField(TEXT("to"), To);

			UAnimStateNode* FromNode = FindStateNodeByName(SMGraph, From);
			UAnimStateNode* ToNode   = FindStateNodeByName(SMGraph, To);
			if (!FromNode || !ToNode)
			{
				Rep->SetBoolField(TEXT("created"), false);
				Rep->SetStringField(TEXT("note"), TEXT("from/to state not found"));
				TransReport.Add(MakeShared<FJsonValueObject>(Rep));
				continue;
			}

			UEdGraphPin* OutPin = FromNode->GetOutputPin();
			UEdGraphPin* InPin  = ToNode->GetInputPin();
			bool bConnected = (OutPin && InPin && SMSchema) ? SMSchema->TryCreateConnection(OutPin, InPin) : false;
			Rep->SetBoolField(TEXT("created"), bConnected);
			if (!bConnected)
			{
				Rep->SetStringField(TEXT("note"), TEXT("TryCreateConnection failed (states may already be connected)"));
				TransReport.Add(MakeShared<FJsonValueObject>(Rep));
				continue;
			}

			// Locate the just-created transition node.
			UAnimStateTransitionNode* TransNode = nullptr;
			for (UEdGraphNode* N : SMGraph->Nodes)
			{
				UAnimStateTransitionNode* TN = Cast<UAnimStateTransitionNode>(N);
				if (!TN) continue;
				UAnimStateNodeBase* P = TN->GetPreviousState();
				UAnimStateNodeBase* Nx = TN->GetNextState();
				if (P && Nx && P->GetStateName() == From && Nx->GetStateName() == To)
				{
					TransNode = TN;
					break;
				}
			}

			// Optional rule.
			if ((*TObj)->HasField(TEXT("rule")) && TransNode)
			{
				// Structured compare rule: { kind:'compare', lhs, op, rhs }. Parsed via the
				// shared ParseTransitionRule so build_state_machine matches set_transition_rule.
				// (auto / bool string forms continue to use the legacy fast path below.)
				FParsedTransitionRule Parsed = ParseTransitionRule(*TObj);
				if (Parsed.Kind == FParsedTransitionRule::EKind::Compare)
				{
					FString FoundCat;
					if (!IsUsableFloatOperand(ABP, Parsed.Variable, FoundCat))
					{
						Rep->SetStringField(TEXT("rule_deferred"), FString::Printf(
							TEXT("compare operand '%s' is not a usable numeric variable."), *Parsed.Variable));
					}
					else
					{
						UEdGraph* RuleGraph = TransNode->GetBoundGraph();
						UAnimGraphNode_TransitionResult* ResultNode = nullptr;
						if (RuleGraph)
						{
							for (UEdGraphNode* N : RuleGraph->Nodes)
							{
								ResultNode = Cast<UAnimGraphNode_TransitionResult>(N);
								if (ResultNode) break;
							}
						}
						UEdGraphPin* ResultPin = ResultNode ? ResultNode->FindPin(TEXT("bCanEnterTransition"), EGPD_Input) : nullptr;
						FString CmpErr;
						const bool bCmpOk = ResultPin && AuthorCompareRuleNodes(
							RuleGraph, ResultNode, ResultPin, Parsed.Variable, Parsed.bUseAbs, Parsed.Op, Parsed.Rhs, CmpErr);
						const FString LhsDisplay = Parsed.bUseAbs ? FString::Printf(TEXT("Abs(%s)"), *Parsed.Variable) : Parsed.Variable;
						Rep->SetStringField(TEXT("rule_applied"), bCmpOk
							? FString::Printf(TEXT("compare %s %s %s"), *LhsDisplay, *Parsed.Op, *FString::SanitizeFloat(Parsed.Rhs))
							: FString::Printf(TEXT("compare authoring failed: %s"), *CmpErr));
					}
					TransReport.Add(MakeShared<FJsonValueObject>(Rep));
					continue;
				}
				if (Parsed.Kind == FParsedTransitionRule::EKind::Expression)
				{
					// Inline expression authoring is intentionally DEFERRED here (parity decision):
					// the standalone set_transition_rule action covers kind:expression. Call it after
					// build_state_machine for any compound AND/OR transition rule.
					Rep->SetStringField(TEXT("rule_deferred"), TEXT("kind:expression deferred in build_state_machine; use the standalone set_transition_rule action for compound AND/OR rules."));
					TransReport.Add(MakeShared<FJsonValueObject>(Rep));
					continue;
				}

				// Object-form auto/bool: resolve the variable name from the parsed spec so a
				// structured { kind:'auto' } / { kind:'bool', variable } also works here. A plain
				// string `rule` yields an empty GetStringField only when it was an object, so we
				// prefer the parsed value when the raw string is empty.
				FString Rule = (*TObj)->GetStringField(TEXT("rule"));
				if (Rule.IsEmpty())
				{
					if (Parsed.Kind == FParsedTransitionRule::EKind::Auto)
					{
						Rule = TEXT("auto");
					}
					else if (Parsed.Kind == FParsedTransitionRule::EKind::Bool)
					{
						Rule = Parsed.Variable;
					}
				}
				if (Rule.Equals(TEXT("auto"), ESearchCase::IgnoreCase) || Rule.Equals(TEXT("automatic"), ESearchCase::IgnoreCase))
				{
					// Sequence-player auto rule — no graph logic required.
					TransNode->Modify();
					TransNode->bAutomaticRuleBasedOnSequencePlayerInState = true;
					Rep->SetStringField(TEXT("rule_applied"), TEXT("automatic"));
				}
				else
				{
					// Treat as a boolean variable name. Validate it is a bool var
					// (mirrors HandleSetTransitionRule's policy); anything else is deferred.
					// Accept either a BP-declared NewVariables bool OR an inherited
					// Blueprint-visible bool UPROPERTY on a native AnimInstance parent.
					const FBPVariableDescription* VarDesc = nullptr;
					for (const FBPVariableDescription& V : ABP->NewVariables)
					{
						if (V.VarName.ToString() == Rule) { VarDesc = &V; break; }
					}
					const bool bInheritedBool = !VarDesc && IsInheritedBlueprintVisibleBool(ABP, Rule);
					if (!VarDesc && !bInheritedBool)
					{
						Rep->SetStringField(TEXT("rule_deferred"), FString::Printf(TEXT("unsupported rule expression (deferred): '%s' is not a known ABP variable. Only bool variables and 'auto' are supported this pass."), *Rule));
					}
					else if (VarDesc && !VarDesc->VarType.PinCategory.ToString().Equals(TEXT("bool"), ESearchCase::IgnoreCase))
					{
						Rep->SetStringField(TEXT("rule_deferred"), FString::Printf(TEXT("unsupported rule expression (deferred): variable '%s' is type '%s', not bool."), *Rule, *VarDesc->VarType.PinCategory.ToString()));
					}
					else
					{
						UEdGraph* RuleGraph = TransNode->GetBoundGraph();
						UAnimGraphNode_TransitionResult* ResultNode = nullptr;
						if (RuleGraph)
						{
							for (UEdGraphNode* N : RuleGraph->Nodes)
							{
								ResultNode = Cast<UAnimGraphNode_TransitionResult>(N);
								if (ResultNode) break;
							}
						}
						UEdGraphPin* ResultPin = ResultNode ? ResultNode->FindPin(TEXT("bCanEnterTransition"), EGPD_Input) : nullptr;
						bool bRuleWired = false;
						if (RuleGraph && ResultPin)
						{
							RuleGraph->Modify();
							ResultPin->BreakAllPinLinks();
							UK2Node_VariableGet* VarGetNode = NewObject<UK2Node_VariableGet>(RuleGraph);
							VarGetNode->VariableReference.SetSelfMember(FName(*Rule));
							RuleGraph->AddNode(VarGetNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
							VarGetNode->NodePosX = ResultNode->NodePosX - 200;
							VarGetNode->NodePosY = ResultNode->NodePosY;
							VarGetNode->AllocateDefaultPins();

							UEdGraphPin* GetterOut = VarGetNode->FindPin(FName(*Rule), EGPD_Output);
							if (!GetterOut)
							{
								for (UEdGraphPin* Pin : VarGetNode->Pins)
								{
									if (Pin && Pin->Direction == EGPD_Output && Pin->PinName != TEXT("self")) { GetterOut = Pin; break; }
								}
							}
							const UEdGraphSchema* RuleSchema = RuleGraph->GetSchema();
							if (GetterOut && RuleSchema)
							{
								bRuleWired = RuleSchema->TryCreateConnection(GetterOut, ResultPin);
							}
						}
						Rep->SetStringField(TEXT("rule_applied"), bRuleWired
							? FString::Printf(TEXT("bool variable '%s'"), *Rule)
							: FString::Printf(TEXT("bool variable '%s' (getter created, wiring uncertain)"), *Rule));
					}
				}
			}

			TransReport.Add(MakeShared<FJsonValueObject>(Rep));
		}
	}

	GEditor->EndTransaction();

	FKismetEditorUtilities::CompileBlueprint(ABP);
	ABP->MarkPackageDirty();

	FString FinalTitle = SMNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	int32 NL2;
	if (FinalTitle.FindChar(TEXT('\n'), NL2)) FinalTitle.LeftInline(NL2);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("anim_graph"), AnimGraph->GetName());
	Root->SetStringField(TEXT("state_machine_name"), FinalTitle);
	Root->SetStringField(TEXT("state_machine_graph"), SMGraph->GetName());
	Root->SetArrayField(TEXT("states_report"), StatesReport);
	Root->SetArrayField(TEXT("transitions_report"), TransReport);
	if (!EntryState.IsEmpty())
	{
		Root->SetStringField(TEXT("entry_state"), EntryState);
		Root->SetBoolField(TEXT("entry_wired"), bEntryWired);
	}
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Helper: Configure BlendSpace axis via reflection (BlendParameters is protected)
// ---------------------------------------------------------------------------

static bool ConfigureBlendSpaceAxis(UBlendSpace* BS, int32 AxisIndex, const FString& Name, float Min, float Max)
{
	FProperty* Prop = UBlendSpace::StaticClass()->FindPropertyByName(TEXT("BlendParameters"));
	if (!Prop) return false;

	FBlendParameter* BlendParam = reinterpret_cast<FBlendParameter*>(Prop->ContainerPtrToValuePtr<uint8>(BS));
	BlendParam += AxisIndex;
	BlendParam->DisplayName = Name;
	BlendParam->Min = Min;
	BlendParam->Max = Max;
	return true;
}

static void BlendSpaceAxisToJson(UBlendSpace* BS, int32 AxisIndex, const FString& FieldPrefix, TSharedPtr<FJsonObject>& Root)
{
	FProperty* Prop = UBlendSpace::StaticClass()->FindPropertyByName(TEXT("BlendParameters"));
	if (!Prop) return;

	const FBlendParameter* BlendParam = reinterpret_cast<const FBlendParameter*>(Prop->ContainerPtrToValuePtr<const uint8>(BS));
	BlendParam += AxisIndex;

	TSharedPtr<FJsonObject> AxisObj = MakeShared<FJsonObject>();
	AxisObj->SetStringField(TEXT("name"), BlendParam->DisplayName);
	AxisObj->SetNumberField(TEXT("min"), BlendParam->Min);
	AxisObj->SetNumberField(TEXT("max"), BlendParam->Max);
	Root->SetObjectField(FieldPrefix, AxisObj);
}

// ---------------------------------------------------------------------------
// create_blend_space
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleCreateBlendSpace(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SkeletonPath = Params->GetStringField(TEXT("skeleton_path"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(SkeletonPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath));

	FString AssetName;
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	AssetName = AssetPath.Mid(LastSlash + 1);

	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));

	UBlendSpace* BS = NewObject<UBlendSpace>(Pkg, FName(*AssetName), RF_Public | RF_Standalone);
	if (!BS) return FMonolithActionResult::Error(TEXT("Failed to create UBlendSpace object"));

	BS->SetSkeleton(Skeleton);

	// Optional axis configuration
	if (Params->HasField(TEXT("axis_x_name")) || Params->HasField(TEXT("axis_x_min")) || Params->HasField(TEXT("axis_x_max")))
	{
		FString XName = Params->HasField(TEXT("axis_x_name")) ? Params->GetStringField(TEXT("axis_x_name")) : TEXT("None");
		float XMin = Params->HasField(TEXT("axis_x_min")) ? static_cast<float>(Params->GetNumberField(TEXT("axis_x_min"))) : 0.0f;
		float XMax = Params->HasField(TEXT("axis_x_max")) ? static_cast<float>(Params->GetNumberField(TEXT("axis_x_max"))) : 100.0f;
		ConfigureBlendSpaceAxis(BS, 0, XName, XMin, XMax);
	}
	if (Params->HasField(TEXT("axis_y_name")) || Params->HasField(TEXT("axis_y_min")) || Params->HasField(TEXT("axis_y_max")))
	{
		FString YName = Params->HasField(TEXT("axis_y_name")) ? Params->GetStringField(TEXT("axis_y_name")) : TEXT("None");
		float YMin = Params->HasField(TEXT("axis_y_min")) ? static_cast<float>(Params->GetNumberField(TEXT("axis_y_min"))) : 0.0f;
		float YMax = Params->HasField(TEXT("axis_y_max")) ? static_cast<float>(Params->GetNumberField(TEXT("axis_y_max"))) : 100.0f;
		ConfigureBlendSpaceAxis(BS, 1, YName, YMin, YMax);
	}

	FAssetRegistryModule::AssetCreated(BS);
	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), BS->GetPathName());
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	BlendSpaceAxisToJson(BS, 0, TEXT("axis_x"), Root);
	BlendSpaceAxisToJson(BS, 1, TEXT("axis_y"), Root);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// create_blend_space_1d
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleCreateBlendSpace1D(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SkeletonPath = Params->GetStringField(TEXT("skeleton_path"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(SkeletonPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath));

	FString AssetName;
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	AssetName = AssetPath.Mid(LastSlash + 1);

	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));

	UBlendSpace1D* BS = NewObject<UBlendSpace1D>(Pkg, FName(*AssetName), RF_Public | RF_Standalone);
	if (!BS) return FMonolithActionResult::Error(TEXT("Failed to create UBlendSpace1D object"));

	BS->SetSkeleton(Skeleton);

	// Optional axis configuration (1D only uses axis 0)
	if (Params->HasField(TEXT("axis_name")) || Params->HasField(TEXT("axis_min")) || Params->HasField(TEXT("axis_max")))
	{
		FString AxisName = Params->HasField(TEXT("axis_name")) ? Params->GetStringField(TEXT("axis_name")) : TEXT("None");
		float AxisMin = Params->HasField(TEXT("axis_min")) ? static_cast<float>(Params->GetNumberField(TEXT("axis_min"))) : 0.0f;
		float AxisMax = Params->HasField(TEXT("axis_max")) ? static_cast<float>(Params->GetNumberField(TEXT("axis_max"))) : 100.0f;
		ConfigureBlendSpaceAxis(BS, 0, AxisName, AxisMin, AxisMax);
	}

	FAssetRegistryModule::AssetCreated(BS);
	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), BS->GetPathName());
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	BlendSpaceAxisToJson(BS, 0, TEXT("axis"), Root);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// create_aim_offset
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleCreateAimOffset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SkeletonPath = Params->GetStringField(TEXT("skeleton_path"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(SkeletonPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath));

	FString AssetName;
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	AssetName = AssetPath.Mid(LastSlash + 1);

	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));

	UAimOffsetBlendSpace* AO = NewObject<UAimOffsetBlendSpace>(Pkg, FName(*AssetName), RF_Public | RF_Standalone);
	if (!AO) return FMonolithActionResult::Error(TEXT("Failed to create UAimOffsetBlendSpace object"));

	AO->SetSkeleton(Skeleton);

	// Default Yaw/Pitch axes for aim offsets, overridable via params
	{
		FString XName = Params->HasField(TEXT("axis_x_name")) ? Params->GetStringField(TEXT("axis_x_name")) : TEXT("Yaw");
		float XMin = Params->HasField(TEXT("axis_x_min")) ? static_cast<float>(Params->GetNumberField(TEXT("axis_x_min"))) : -180.0f;
		float XMax = Params->HasField(TEXT("axis_x_max")) ? static_cast<float>(Params->GetNumberField(TEXT("axis_x_max"))) : 180.0f;
		ConfigureBlendSpaceAxis(AO, 0, XName, XMin, XMax);
	}
	{
		FString YName = Params->HasField(TEXT("axis_y_name")) ? Params->GetStringField(TEXT("axis_y_name")) : TEXT("Pitch");
		float YMin = Params->HasField(TEXT("axis_y_min")) ? static_cast<float>(Params->GetNumberField(TEXT("axis_y_min"))) : -90.0f;
		float YMax = Params->HasField(TEXT("axis_y_max")) ? static_cast<float>(Params->GetNumberField(TEXT("axis_y_max"))) : 90.0f;
		ConfigureBlendSpaceAxis(AO, 1, YName, YMin, YMax);
	}

	FAssetRegistryModule::AssetCreated(AO);
	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AO->GetPathName());
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	BlendSpaceAxisToJson(AO, 0, TEXT("axis_x"), Root);
	BlendSpaceAxisToJson(AO, 1, TEXT("axis_y"), Root);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// create_aim_offset_1d
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleCreateAimOffset1D(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SkeletonPath = Params->GetStringField(TEXT("skeleton_path"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(SkeletonPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath));

	FString AssetName;
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	AssetName = AssetPath.Mid(LastSlash + 1);

	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));

	UAimOffsetBlendSpace1D* AO = NewObject<UAimOffsetBlendSpace1D>(Pkg, FName(*AssetName), RF_Public | RF_Standalone);
	if (!AO) return FMonolithActionResult::Error(TEXT("Failed to create UAimOffsetBlendSpace1D object"));

	AO->SetSkeleton(Skeleton);

	// Default Yaw axis for 1D aim offsets
	{
		FString AxisName = Params->HasField(TEXT("axis_name")) ? Params->GetStringField(TEXT("axis_name")) : TEXT("Yaw");
		float AxisMin = Params->HasField(TEXT("axis_min")) ? static_cast<float>(Params->GetNumberField(TEXT("axis_min"))) : -180.0f;
		float AxisMax = Params->HasField(TEXT("axis_max")) ? static_cast<float>(Params->GetNumberField(TEXT("axis_max"))) : 180.0f;
		ConfigureBlendSpaceAxis(AO, 0, AxisName, AxisMin, AxisMax);
	}

	FAssetRegistryModule::AssetCreated(AO);
	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AO->GetPathName());
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	BlendSpaceAxisToJson(AO, 0, TEXT("axis"), Root);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// create_composite
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleCreateComposite(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SkeletonPath = Params->GetStringField(TEXT("skeleton_path"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(SkeletonPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath));

	FString AssetName;
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	AssetName = AssetPath.Mid(LastSlash + 1);

	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));

	UAnimComposite* Composite = NewObject<UAnimComposite>(Pkg, FName(*AssetName), RF_Public | RF_Standalone);
	if (!Composite) return FMonolithActionResult::Error(TEXT("Failed to create UAnimComposite object"));

	Composite->SetSkeleton(Skeleton);
	FAssetRegistryModule::AssetCreated(Composite);
	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Composite->GetPathName());
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// create_anim_blueprint
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleCreateAnimBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SkeletonPath = Params->GetStringField(TEXT("skeleton_path"));
	FString ParentClassName = Params->HasField(TEXT("parent_class")) ? Params->GetStringField(TEXT("parent_class")) : TEXT("AnimInstance");

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(SkeletonPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath));

	FString AssetName;
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	AssetName = AssetPath.Mid(LastSlash + 1);

	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));

	// Resolve parent class
	UClass* ParentClass = nullptr;
	if (ParentClassName.Equals(TEXT("AnimInstance"), ESearchCase::IgnoreCase) ||
		ParentClassName.Equals(TEXT("UAnimInstance"), ESearchCase::IgnoreCase))
	{
		ParentClass = UAnimInstance::StaticClass();
	}
	else
	{
		// Try to find the class by name — support both "UMyClass" and "MyClass" forms
		FString CleanName = ParentClassName;
		if (CleanName.StartsWith(TEXT("U")))
		{
			CleanName = CleanName.Mid(1);
		}
		ParentClass = FindFirstObject<UClass>(*CleanName, EFindFirstObjectOptions::NativeFirst);
		if (!ParentClass)
		{
			// Try with full path
			ParentClass = LoadObject<UClass>(nullptr, *ParentClassName);
		}
		if (!ParentClass || !ParentClass->IsChildOf(UAnimInstance::StaticClass()))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Parent class '%s' not found or not derived from UAnimInstance"), *ParentClassName));
		}
	}

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));

	UAnimBlueprint* AnimBP = CastChecked<UAnimBlueprint>(
		FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Pkg,
			FName(*AssetName),
			BPTYPE_Normal,
			UAnimBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None
		)
	);

	if (!AnimBP)
		return FMonolithActionResult::Error(TEXT("Failed to create Animation Blueprint via FKismetEditorUtilities::CreateBlueprint"));

	// Set skeleton on the ABP and both generated classes
	AnimBP->TargetSkeleton = Skeleton;
	if (UAnimBlueprintGeneratedClass* GenClass = Cast<UAnimBlueprintGeneratedClass>(AnimBP->GeneratedClass))
	{
		GenClass->TargetSkeleton = Skeleton;
	}
	if (UAnimBlueprintGeneratedClass* SkelGenClass = Cast<UAnimBlueprintGeneratedClass>(AnimBP->SkeletonGeneratedClass))
	{
		SkelGenClass->TargetSkeleton = Skeleton;
	}

	FAssetRegistryModule::AssetCreated(AnimBP);
	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AnimBP->GetPathName());
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	Root->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	if (AnimBP->GeneratedClass)
	{
		Root->SetStringField(TEXT("generated_class"), AnimBP->GeneratedClass->GetPathName());
	}
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// compare_skeletons
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleCompareSkeletons(const TSharedPtr<FJsonObject>& Params)
{
	FString SkeletonPathA = Params->GetStringField(TEXT("skeleton_a"));
	FString SkeletonPathB = Params->GetStringField(TEXT("skeleton_b"));

	USkeleton* SkeletonA = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(SkeletonPathA);
	if (!SkeletonA) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton A not found: %s"), *SkeletonPathA));

	USkeleton* SkeletonB = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(SkeletonPathB);
	if (!SkeletonB) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton B not found: %s"), *SkeletonPathB));

	const FReferenceSkeleton& RefA = SkeletonA->GetReferenceSkeleton();
	const FReferenceSkeleton& RefB = SkeletonB->GetReferenceSkeleton();

	// Build bone name sets
	TSet<FName> BonesA;
	TSet<FName> BonesB;
	for (int32 i = 0; i < RefA.GetRawBoneNum(); ++i)
		BonesA.Add(RefA.GetBoneName(i));
	for (int32 i = 0; i < RefB.GetRawBoneNum(); ++i)
		BonesB.Add(RefB.GetBoneName(i));

	// Find matching, missing in A, missing in B
	TArray<FName> Matching;
	TArray<FName> MissingInA;
	TArray<FName> MissingInB;

	for (const FName& Bone : BonesA)
	{
		if (BonesB.Contains(Bone))
			Matching.Add(Bone);
		else
			MissingInB.Add(Bone);
	}
	for (const FName& Bone : BonesB)
	{
		if (!BonesA.Contains(Bone))
			MissingInA.Add(Bone);
	}

	// Check hierarchy match for common bones
	bool bHierarchyMatches = true;
	for (const FName& Bone : Matching)
	{
		int32 IdxA = RefA.FindBoneIndex(Bone);
		int32 IdxB = RefB.FindBoneIndex(Bone);
		int32 ParentA = RefA.GetParentIndex(IdxA);
		int32 ParentB = RefB.GetParentIndex(IdxB);

		// Both root or both have matching parent names
		FName ParentNameA = (ParentA != INDEX_NONE) ? RefA.GetBoneName(ParentA) : NAME_None;
		FName ParentNameB = (ParentB != INDEX_NONE) ? RefB.GetBoneName(ParentB) : NAME_None;

		if (ParentNameA != ParentNameB)
		{
			bHierarchyMatches = false;
			break;
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("skeleton_a"), SkeletonPathA);
	Root->SetStringField(TEXT("skeleton_b"), SkeletonPathB);
	Root->SetNumberField(TEXT("bone_count_a"), RefA.GetRawBoneNum());
	Root->SetNumberField(TEXT("bone_count_b"), RefB.GetRawBoneNum());
	Root->SetNumberField(TEXT("matching_bones"), Matching.Num());
	Root->SetBoolField(TEXT("hierarchy_matches"), bHierarchyMatches);

	// Missing in A (bones that B has but A doesn't)
	TArray<TSharedPtr<FJsonValue>> MissingInAArr;
	for (const FName& Bone : MissingInA)
		MissingInAArr.Add(MakeShared<FJsonValueString>(Bone.ToString()));
	Root->SetArrayField(TEXT("missing_in_a"), MissingInAArr);

	// Missing in B (bones that A has but B doesn't)
	TArray<TSharedPtr<FJsonValue>> MissingInBArr;
	for (const FName& Bone : MissingInB)
		MissingInBArr.Add(MakeShared<FJsonValueString>(Bone.ToString()));
	Root->SetArrayField(TEXT("missing_in_b"), MissingInBArr);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Wave 12 — Sequence Properties + Sync Markers
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleSetSequenceProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	bool bAnySet = false;

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Sequence Properties")));
	Seq->Modify();

	if (Params->HasField(TEXT("rate_scale")))
	{
		Seq->RateScale = static_cast<float>(Params->GetNumberField(TEXT("rate_scale")));
		bAnySet = true;
	}

	if (Params->HasField(TEXT("loop")))
	{
		Seq->bLoop = Params->GetBoolField(TEXT("loop"));
		bAnySet = true;
	}

	if (Params->HasField(TEXT("interpolation")))
	{
		FString InterpStr = Params->GetStringField(TEXT("interpolation"));
		if (InterpStr.Equals(TEXT("Linear"), ESearchCase::IgnoreCase))
			Seq->Interpolation = EAnimInterpolationType::Linear;
		else if (InterpStr.Equals(TEXT("Step"), ESearchCase::IgnoreCase))
			Seq->Interpolation = EAnimInterpolationType::Step;
		else
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid interpolation: '%s' — use Linear or Step"), *InterpStr));
		}
		bAnySet = true;
	}

	if (!bAnySet)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("At least one of rate_scale, loop, or interpolation must be provided"));
	}

	GEditor->EndTransaction();
	Seq->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("rate_scale"), Seq->RateScale);
	Root->SetBoolField(TEXT("loop"), Seq->bLoop);
	Root->SetStringField(TEXT("interpolation"),
		Seq->Interpolation == EAnimInterpolationType::Linear ? TEXT("Linear") : TEXT("Step"));
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetAdditiveSettings(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	bool bAnySet = false;

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Additive Settings")));
	Seq->Modify();

	if (Params->HasField(TEXT("additive_anim_type")))
	{
		FString TypeStr = Params->GetStringField(TEXT("additive_anim_type"));
		if (TypeStr.Equals(TEXT("NoAdditive"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			Seq->AdditiveAnimType = EAdditiveAnimationType::AAT_None;
		else if (TypeStr.Equals(TEXT("LocalSpace"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("LocalSpaceBase"), ESearchCase::IgnoreCase))
			Seq->AdditiveAnimType = EAdditiveAnimationType::AAT_LocalSpaceBase;
		else if (TypeStr.Equals(TEXT("MeshSpace"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("RotationOffsetMeshSpace"), ESearchCase::IgnoreCase))
			Seq->AdditiveAnimType = EAdditiveAnimationType::AAT_RotationOffsetMeshSpace;
		else
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid additive_anim_type: '%s' — use NoAdditive, LocalSpace, or MeshSpace"), *TypeStr));
		}
		bAnySet = true;
	}

	if (Params->HasField(TEXT("ref_pose_type")))
	{
		FString RefStr = Params->GetStringField(TEXT("ref_pose_type"));
		if (RefStr.Equals(TEXT("RefPose"), ESearchCase::IgnoreCase))
			Seq->RefPoseType = EAdditiveBasePoseType::ABPT_RefPose;
		else if (RefStr.Equals(TEXT("AnimScaled"), ESearchCase::IgnoreCase))
			Seq->RefPoseType = EAdditiveBasePoseType::ABPT_AnimScaled;
		else if (RefStr.Equals(TEXT("AnimFrame"), ESearchCase::IgnoreCase))
			Seq->RefPoseType = EAdditiveBasePoseType::ABPT_AnimFrame;
		else if (RefStr.Equals(TEXT("LocalAnimFrame"), ESearchCase::IgnoreCase))
			Seq->RefPoseType = EAdditiveBasePoseType::ABPT_LocalAnimFrame;
		else
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid ref_pose_type: '%s' — use RefPose, AnimScaled, AnimFrame, or LocalAnimFrame"), *RefStr));
		}
		bAnySet = true;
	}

	if (Params->HasField(TEXT("ref_frame_index")))
	{
		Seq->RefFrameIndex = static_cast<int32>(Params->GetNumberField(TEXT("ref_frame_index")));
		bAnySet = true;
	}

	if (Params->HasField(TEXT("ref_pose_seq")))
	{
		FString RefSeqPath = Params->GetStringField(TEXT("ref_pose_seq"));
		if (RefSeqPath.IsEmpty())
		{
			Seq->RefPoseSeq = nullptr;
		}
		else
		{
			UAnimSequence* RefSeq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(RefSeqPath);
			if (!RefSeq)
			{
				GEditor->EndTransaction();
				return FMonolithActionResult::Error(FString::Printf(TEXT("Reference pose sequence not found: %s"), *RefSeqPath));
			}
			Seq->RefPoseSeq = RefSeq;
		}
		bAnySet = true;
	}

	if (!bAnySet)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("At least one of additive_anim_type, ref_pose_type, ref_frame_index, or ref_pose_seq must be provided"));
	}

	GEditor->EndTransaction();

	// PostEditChangeProperty triggers additive delta recomputation / DDC rebuild
	FPropertyChangedEvent PropEvent(
		UAnimSequence::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UAnimSequence, AdditiveAnimType)),
		EPropertyChangeType::ValueSet
	);
	Seq->PostEditChangeProperty(PropEvent);

	Seq->MarkPackageDirty();

	// Build response
	FString AdditiveStr;
	switch (Seq->AdditiveAnimType.GetValue())
	{
	case EAdditiveAnimationType::AAT_None:                       AdditiveStr = TEXT("NoAdditive"); break;
	case EAdditiveAnimationType::AAT_LocalSpaceBase:             AdditiveStr = TEXT("LocalSpace"); break;
	case EAdditiveAnimationType::AAT_RotationOffsetMeshSpace:    AdditiveStr = TEXT("MeshSpace"); break;
	default:                                                     AdditiveStr = TEXT("Unknown"); break;
	}

	FString RefPoseStr;
	switch (Seq->RefPoseType.GetValue())
	{
	case EAdditiveBasePoseType::ABPT_RefPose:        RefPoseStr = TEXT("RefPose"); break;
	case EAdditiveBasePoseType::ABPT_AnimScaled:     RefPoseStr = TEXT("AnimScaled"); break;
	case EAdditiveBasePoseType::ABPT_AnimFrame:      RefPoseStr = TEXT("AnimFrame"); break;
	case EAdditiveBasePoseType::ABPT_LocalAnimFrame: RefPoseStr = TEXT("LocalAnimFrame"); break;
	default:                                         RefPoseStr = TEXT("None"); break;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("additive_anim_type"), AdditiveStr);
	Root->SetStringField(TEXT("ref_pose_type"), RefPoseStr);
	Root->SetNumberField(TEXT("ref_frame_index"), Seq->RefFrameIndex);
	Root->SetStringField(TEXT("ref_pose_seq"), Seq->RefPoseSeq ? Seq->RefPoseSeq->GetPathName() : TEXT(""));
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetCompressionSettings(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	bool bAnySet = false;

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Compression Settings")));
	Seq->Modify();

	if (Params->HasField(TEXT("bone_compression")))
	{
		FString BoneCompPath = Params->GetStringField(TEXT("bone_compression"));
		if (BoneCompPath.IsEmpty())
		{
			Seq->BoneCompressionSettings = nullptr;
		}
		else
		{
			UAnimBoneCompressionSettings* BoneComp = FMonolithAssetUtils::LoadAssetByPath<UAnimBoneCompressionSettings>(BoneCompPath);
			if (!BoneComp)
			{
				GEditor->EndTransaction();
				return FMonolithActionResult::Error(FString::Printf(TEXT("Bone compression settings not found: %s"), *BoneCompPath));
			}
			Seq->BoneCompressionSettings = BoneComp;
		}
		bAnySet = true;
	}

	if (Params->HasField(TEXT("curve_compression")))
	{
		FString CurveCompPath = Params->GetStringField(TEXT("curve_compression"));
		if (CurveCompPath.IsEmpty())
		{
			Seq->CurveCompressionSettings = nullptr;
		}
		else
		{
			UAnimCurveCompressionSettings* CurveComp = FMonolithAssetUtils::LoadAssetByPath<UAnimCurveCompressionSettings>(CurveCompPath);
			if (!CurveComp)
			{
				GEditor->EndTransaction();
				return FMonolithActionResult::Error(FString::Printf(TEXT("Curve compression settings not found: %s"), *CurveCompPath));
			}
			Seq->CurveCompressionSettings = CurveComp;
		}
		bAnySet = true;
	}

	if (!bAnySet)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("At least one of bone_compression or curve_compression must be provided"));
	}

	GEditor->EndTransaction();
	Seq->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("bone_compression"), Seq->BoneCompressionSettings ? Seq->BoneCompressionSettings->GetPathName() : TEXT(""));
	Root->SetStringField(TEXT("curve_compression"), Seq->CurveCompressionSettings ? Seq->CurveCompressionSettings->GetPathName() : TEXT(""));
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetSyncMarkers(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	TArray<TSharedPtr<FJsonValue>> MarkersArr;
	for (int32 i = 0; i < Seq->AuthoredSyncMarkers.Num(); ++i)
	{
		const FAnimSyncMarker& Marker = Seq->AuthoredSyncMarkers[i];
		TSharedPtr<FJsonObject> MarkerObj = MakeShared<FJsonObject>();
		MarkerObj->SetStringField(TEXT("name"), Marker.MarkerName.ToString());
		MarkerObj->SetNumberField(TEXT("time"), Marker.Time);
		MarkerObj->SetNumberField(TEXT("index"), i);
#if WITH_EDITORONLY_DATA
		MarkerObj->SetNumberField(TEXT("track_index"), Marker.TrackIndex);
		MarkerObj->SetStringField(TEXT("guid"), Marker.Guid.ToString());
#endif
		MarkersArr.Add(MakeShared<FJsonValueObject>(MarkerObj));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("marker_count"), Seq->AuthoredSyncMarkers.Num());
	Root->SetArrayField(TEXT("markers"), MarkersArr);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleAddSyncMarker(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString MarkerName = Params->GetStringField(TEXT("marker_name"));
	float Time = static_cast<float>(Params->GetNumberField(TEXT("time")));
	int32 TrackIndex = Params->HasField(TEXT("track_index")) ? static_cast<int32>(Params->GetNumberField(TEXT("track_index"))) : 0;

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	GEditor->BeginTransaction(FText::FromString(TEXT("Add Sync Marker")));
	Seq->Modify();

	FAnimSyncMarker NewMarker;
	NewMarker.MarkerName = FName(*MarkerName);
	NewMarker.Time = Time;
#if WITH_EDITORONLY_DATA
	NewMarker.TrackIndex = TrackIndex;
	NewMarker.Guid = FGuid::NewGuid();
#endif

	int32 NewIndex = Seq->AuthoredSyncMarkers.Add(NewMarker);
	Seq->RefreshSyncMarkerDataFromAuthored();

	GEditor->EndTransaction();
	Seq->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("marker_name"), MarkerName);
	Root->SetNumberField(TEXT("time"), Time);
	Root->SetNumberField(TEXT("track_index"), TrackIndex);
	Root->SetNumberField(TEXT("index"), NewIndex);
	Root->SetNumberField(TEXT("total_markers"), Seq->AuthoredSyncMarkers.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleRemoveSyncMarker(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	bool bHasName = Params->HasField(TEXT("marker_name"));
	bool bHasIndex = Params->HasField(TEXT("marker_index"));

	if (!bHasName && !bHasIndex)
		return FMonolithActionResult::Error(TEXT("Either marker_name or marker_index must be provided"));

	GEditor->BeginTransaction(FText::FromString(TEXT("Remove Sync Marker")));
	Seq->Modify();

	int32 RemovedCount = 0;

	if (bHasIndex)
	{
		int32 MarkerIndex = static_cast<int32>(Params->GetNumberField(TEXT("marker_index")));
		if (!Seq->AuthoredSyncMarkers.IsValidIndex(MarkerIndex))
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid marker_index: %d (have %d markers)"), MarkerIndex, Seq->AuthoredSyncMarkers.Num()));
		}
		Seq->AuthoredSyncMarkers.RemoveAt(MarkerIndex);
		RemovedCount = 1;
	}
	else
	{
		FString MarkerNameStr = Params->GetStringField(TEXT("marker_name"));
		FName NameToRemove(*MarkerNameStr);

		TArray<FName> NamesToRemove;
		NamesToRemove.Add(NameToRemove);

		int32 CountBefore = Seq->AuthoredSyncMarkers.Num();
		Seq->RemoveSyncMarkers(NamesToRemove);
		RemovedCount = CountBefore - Seq->AuthoredSyncMarkers.Num();
	}

	Seq->RefreshSyncMarkerDataFromAuthored();

	GEditor->EndTransaction();
	Seq->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("removed_count"), RemovedCount);
	Root->SetNumberField(TEXT("remaining_markers"), Seq->AuthoredSyncMarkers.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleRenameSyncMarker(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString OldName = Params->GetStringField(TEXT("old_name"));
	FString NewName = Params->GetStringField(TEXT("new_name"));

	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));

	// Count how many markers have the old name before renaming
	FName OldFName(*OldName);
	int32 RenamedCount = 0;
	for (const FAnimSyncMarker& Marker : Seq->AuthoredSyncMarkers)
	{
		if (Marker.MarkerName == OldFName)
			++RenamedCount;
	}

	if (RenamedCount == 0)
		return FMonolithActionResult::Error(FString::Printf(TEXT("No sync markers found with name '%s'"), *OldName));

	GEditor->BeginTransaction(FText::FromString(TEXT("Rename Sync Marker")));
	Seq->Modify();

	Seq->RenameSyncMarkers(FName(*OldName), FName(*NewName));
	Seq->RefreshSyncMarkerDataFromAuthored();

	GEditor->EndTransaction();
	Seq->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("old_name"), OldName);
	Root->SetStringField(TEXT("new_name"), NewName);
	Root->SetNumberField(TEXT("renamed_count"), RenamedCount);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Wave 13 — Batch Ops + Montage Completion
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleBatchExecute(const TSharedPtr<FJsonObject>& Params)
{
	// Parse operations — handle both EJson::Array (normal) and EJson::String (Claude Code quirk)
	TArray<TSharedPtr<FJsonValue>> Ops;
	TSharedPtr<FJsonValue> OpsField = Params->TryGetField(TEXT("operations"));
	if (!OpsField.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required field: operations"));
	}
	if (OpsField->Type == EJson::Array)
	{
		Ops = OpsField->AsArray();
	}
	else if (OpsField->Type == EJson::String)
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OpsField->AsString());
		if (!FJsonSerializer::Deserialize(Reader, Ops))
		{
			return FMonolithActionResult::Error(TEXT("Failed to parse operations string as JSON array"));
		}
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("'operations' must be an array"));
	}

	if (Ops.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("operations array is empty"));
	}

	bool bStopOnError = false;
	Params->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "AnimBatchExec", "Animation Batch Execute"));

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 Ok = 0, Fail = 0;

	for (int32 i = 0; i < Ops.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Op = Ops[i]->AsObject();
		TSharedRef<FJsonObject> RO = MakeShared<FJsonObject>();
		RO->SetNumberField(TEXT("index"), i);

		if (!Op.IsValid())
		{
			RO->SetStringField(TEXT("op"), TEXT("(invalid)"));
			RO->SetBoolField(TEXT("success"), false);
			RO->SetStringField(TEXT("error"), TEXT("Operation entry is not a valid JSON object"));
			Results.Add(MakeShared<FJsonValueObject>(RO));
			Fail++;
			if (bStopOnError) break;
			continue;
		}

		FString OpName;
		if (!Op->TryGetStringField(TEXT("op"), OpName) || OpName.IsEmpty())
		{
			FString HintName;
			Op->TryGetStringField(TEXT("action"), HintName);
			FString Hint = HintName.IsEmpty()
				? TEXT("Each operation must have an \"op\" key with the action name, plus flat inline params (not nested under \"params\").")
				: FString::Printf(TEXT("Use \"op\" key, not \"action\". Found \"action\":\"%s\". Params must be flat inline, not nested."), *HintName);
			RO->SetStringField(TEXT("op"), TEXT("(missing)"));
			RO->SetBoolField(TEXT("success"), false);
			RO->SetStringField(TEXT("error"), Hint);
			Results.Add(MakeShared<FJsonValueObject>(RO));
			Fail++;
			if (bStopOnError) break;
			continue;
		}
		RO->SetStringField(TEXT("op"), OpName);

		// Build sub-params: copy all op fields (asset_path comes from op, not outer params)
		TSharedRef<FJsonObject> SubParams = MakeShared<FJsonObject>();
		for (auto& Pair : Op->Values)
		{
			const FString OpFieldKey = MonolithKeyToString(Pair.Key);
			if (OpFieldKey != TEXT("op"))
			{
				SubParams->SetField(OpFieldKey, Pair.Value);
			}
		}

		FMonolithActionResult SubResult = FMonolithActionResult::Error(FString::Printf(TEXT("Unknown op: %s"), *OpName));

		// Notify ops
		if      (OpName == TEXT("add_notify"))               SubResult = HandleAddNotify(SubParams);
		else if (OpName == TEXT("add_notify_state"))          SubResult = HandleAddNotifyState(SubParams);
		else if (OpName == TEXT("remove_notify"))             SubResult = HandleRemoveNotify(SubParams);
		else if (OpName == TEXT("set_notify_time"))           SubResult = HandleSetNotifyTime(SubParams);
		else if (OpName == TEXT("set_notify_duration"))       SubResult = HandleSetNotifyDuration(SubParams);
		else if (OpName == TEXT("set_notify_track"))          SubResult = HandleSetNotifyTrack(SubParams);
		else if (OpName == TEXT("set_notify_properties"))     SubResult = HandleSetNotifyProperties(SubParams);
		// Montage section ops
		else if (OpName == TEXT("add_montage_section"))       SubResult = HandleAddMontageSection(SubParams);
		else if (OpName == TEXT("delete_montage_section"))    SubResult = HandleDeleteMontageSection(SubParams);
		else if (OpName == TEXT("set_section_next"))          SubResult = HandleSetSectionNext(SubParams);
		else if (OpName == TEXT("set_section_time"))          SubResult = HandleSetSectionTime(SubParams);
		// Montage slot/blend ops
		else if (OpName == TEXT("add_montage_slot"))          SubResult = HandleAddMontageSlot(SubParams);
		else if (OpName == TEXT("set_montage_slot"))          SubResult = HandleSetMontageSlot(SubParams);
		else if (OpName == TEXT("set_montage_blend"))         SubResult = HandleSetMontageBlend(SubParams);
		else if (OpName == TEXT("add_montage_anim_segment")) SubResult = HandleAddMontageAnimSegment(SubParams);
		// Curve ops
		else if (OpName == TEXT("add_curve"))                 SubResult = HandleAddCurve(SubParams);
		else if (OpName == TEXT("remove_curve"))              SubResult = HandleRemoveCurve(SubParams);
		else if (OpName == TEXT("set_curve_keys"))            SubResult = HandleSetCurveKeys(SubParams);
		// Bone track ops
		else if (OpName == TEXT("add_bone_track"))            SubResult = HandleAddBoneTrack(SubParams);
		else if (OpName == TEXT("set_bone_track_keys"))       SubResult = HandleSetBoneTrackKeys(SubParams);
		else if (OpName == TEXT("remove_bone_track"))         SubResult = HandleRemoveBoneTrack(SubParams);
		else if (OpName == TEXT("copy_bone_pose_between_sequences")) SubResult = HandleCopyBonePoseBetweenSequences(SubParams);
		// BlendSpace ops
		else if (OpName == TEXT("add_blendspace_sample"))     SubResult = HandleAddBlendSpaceSample(SubParams);
		else if (OpName == TEXT("edit_blendspace_sample"))    SubResult = HandleEditBlendSpaceSample(SubParams);
		else if (OpName == TEXT("delete_blendspace_sample"))  SubResult = HandleDeleteBlendSpaceSample(SubParams);
		else if (OpName == TEXT("bake_blend_space"))          SubResult = HandleBakeBlendSpace(SubParams);
		else if (OpName == TEXT("set_blend_space_interpolation")) SubResult = HandleSetBlendSpaceInterpolation(SubParams);
		// State machine editing ops
		else if (OpName == TEXT("remove_anim_state"))         SubResult = HandleRemoveAnimState(SubParams);
		else if (OpName == TEXT("set_anim_entry_state"))      SubResult = HandleSetAnimEntryState(SubParams);
		else if (OpName == TEXT("remove_anim_transition"))    SubResult = HandleRemoveAnimTransition(SubParams);

		else if (OpName == TEXT("add_ik_solver"))             SubResult = HandleAddIKSolver(SubParams);
		else if (OpName == TEXT("remove_ik_solver"))          SubResult = HandleRemoveIKSolver(SubParams);
		// Socket ops
		else if (OpName == TEXT("add_socket"))                SubResult = HandleAddSocket(SubParams);
		else if (OpName == TEXT("remove_socket"))             SubResult = HandleRemoveSocket(SubParams);
		else if (OpName == TEXT("set_socket_transform"))      SubResult = HandleSetSocketTransform(SubParams);
		// Sync marker ops
		else if (OpName == TEXT("add_sync_marker"))           SubResult = HandleAddSyncMarker(SubParams);
		else if (OpName == TEXT("remove_sync_marker"))        SubResult = HandleRemoveSyncMarker(SubParams);
		else if (OpName == TEXT("rename_sync_marker"))        SubResult = HandleRenameSyncMarker(SubParams);
		// Sequence property ops
		else if (OpName == TEXT("set_sequence_properties"))   SubResult = HandleSetSequenceProperties(SubParams);
		else if (OpName == TEXT("set_additive_settings"))     SubResult = HandleSetAdditiveSettings(SubParams);
		else if (OpName == TEXT("set_compression_settings"))  SubResult = HandleSetCompressionSettings(SubParams);
		else if (OpName == TEXT("set_root_motion_settings"))  SubResult = HandleSetRootMotionSettings(SubParams);
		else if (OpName == TEXT("set_blend_space_axis"))      SubResult = HandleSetBlendSpaceAxis(SubParams);
		// Read ops (useful in batch for gathering info)
		else if (OpName == TEXT("get_sequence_info"))         SubResult = HandleGetSequenceInfo(SubParams);
		else if (OpName == TEXT("get_sequence_notifies"))     SubResult = HandleGetSequenceNotifies(SubParams);
		else if (OpName == TEXT("get_montage_info"))          SubResult = HandleGetMontageInfo(SubParams);
		else if (OpName == TEXT("get_blend_space_info"))      SubResult = HandleGetBlendSpaceInfo(SubParams);
		else if (OpName == TEXT("get_sequence_curves"))       SubResult = HandleGetSequenceCurves(SubParams);
		else if (OpName == TEXT("get_bone_track_keys"))       SubResult = HandleGetBoneTrackKeys(SubParams);
		else if (OpName == TEXT("list_bone_tracks"))          SubResult = HandleListBoneTracks(SubParams);
		else if (OpName == TEXT("get_curve_keys"))            SubResult = HandleGetCurveKeys(SubParams);
		else if (OpName == TEXT("list_curves"))               SubResult = HandleListCurves(SubParams);
		else if (OpName == TEXT("get_sync_markers"))          SubResult = HandleGetSyncMarkers(SubParams);

		RO->SetBoolField(TEXT("success"), SubResult.bSuccess);
		if (!SubResult.bSuccess)
		{
			RO->SetStringField(TEXT("error"), SubResult.ErrorMessage);
		}
		if (SubResult.bSuccess && SubResult.Result.IsValid())
		{
			RO->SetObjectField(TEXT("data"), SubResult.Result);
		}

		Results.Add(MakeShared<FJsonValueObject>(RO));
		if (SubResult.bSuccess) Ok++; else Fail++;

		if (!SubResult.bSuccess && bStopOnError) break;
	}

	GEditor->EndTransaction();

	TSharedRef<FJsonObject> Final = MakeShared<FJsonObject>();
	Final->SetBoolField(TEXT("success"), Fail == 0);
	Final->SetNumberField(TEXT("total"), Ops.Num());
	Final->SetNumberField(TEXT("succeeded"), Ok);
	Final->SetNumberField(TEXT("failed"), Fail);
	Final->SetArrayField(TEXT("results"), Results);

	return FMonolithActionResult::Success(Final);
}

// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleAddMontageAnimSegment(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString AnimPath = Params->GetStringField(TEXT("anim_path"));
	int32 SlotIndex = Params->HasField(TEXT("slot_index")) ? static_cast<int32>(Params->GetNumberField(TEXT("slot_index"))) : 0;
	float PlayRate = Params->HasField(TEXT("play_rate")) ? static_cast<float>(Params->GetNumberField(TEXT("play_rate"))) : 1.0f;
	int32 LoopingCount = Params->HasField(TEXT("looping_count")) ? static_cast<int32>(Params->GetNumberField(TEXT("looping_count"))) : 1;

	UAnimMontage* Montage = FMonolithAssetUtils::LoadAssetByPath<UAnimMontage>(AssetPath);
	if (!Montage) return FMonolithActionResult::Error(FString::Printf(TEXT("Montage not found: %s"), *AssetPath));

	if (SlotIndex < 0 || SlotIndex >= Montage->SlotAnimTracks.Num())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid slot_index %d (montage has %d slots)"), SlotIndex, Montage->SlotAnimTracks.Num()));
	}

	UAnimSequenceBase* Anim = FMonolithAssetUtils::LoadAssetByPath<UAnimSequenceBase>(AnimPath);
	if (!Anim) return FMonolithActionResult::Error(FString::Printf(TEXT("Animation not found: %s"), *AnimPath));

	// Auto-calculate StartPos from existing segments if not provided
	float StartPos = 0.0f;
	if (Params->HasField(TEXT("start_pos")))
	{
		StartPos = static_cast<float>(Params->GetNumberField(TEXT("start_pos")));
	}
	else
	{
		for (const FAnimSegment& Seg : Montage->SlotAnimTracks[SlotIndex].AnimTrack.AnimSegments)
		{
			StartPos = FMath::Max(StartPos, Seg.StartPos + Seg.GetLength());
		}
	}

	float AnimStartTime = Params->HasField(TEXT("anim_start_time")) ? static_cast<float>(Params->GetNumberField(TEXT("anim_start_time"))) : 0.0f;
	float AnimEndTime = Params->HasField(TEXT("anim_end_time")) ? static_cast<float>(Params->GetNumberField(TEXT("anim_end_time"))) : Anim->GetPlayLength();

	GEditor->BeginTransaction(FText::FromString(TEXT("Add Montage Anim Segment")));
	Montage->Modify();

	FAnimSegment NewSeg;
	NewSeg.SetAnimReference(Anim);
	NewSeg.StartPos = StartPos;
	NewSeg.AnimStartTime = AnimStartTime;
	NewSeg.AnimEndTime = AnimEndTime;
	NewSeg.AnimPlayRate = PlayRate;
	NewSeg.LoopingCount = LoopingCount;

	int32 NewIndex = Montage->SlotAnimTracks[SlotIndex].AnimTrack.AnimSegments.Add(NewSeg);

	GEditor->EndTransaction();
	Montage->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("slot_index"), SlotIndex);
	Root->SetNumberField(TEXT("segment_index"), NewIndex);
	Root->SetStringField(TEXT("anim_reference"), AnimPath);
	Root->SetNumberField(TEXT("start_pos"), StartPos);
	Root->SetNumberField(TEXT("anim_start_time"), AnimStartTime);
	Root->SetNumberField(TEXT("anim_end_time"), AnimEndTime);
	Root->SetNumberField(TEXT("play_rate"), PlayRate);
	Root->SetNumberField(TEXT("looping_count"), LoopingCount);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleCloneNotifySetup(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath = Params->GetStringField(TEXT("source_path"));
	FString TargetPath = Params->GetStringField(TEXT("target_path"));
	float TimeScale = Params->HasField(TEXT("time_scale")) ? static_cast<float>(Params->GetNumberField(TEXT("time_scale"))) : 1.0f;
	bool bAutoScale = false;
	Params->TryGetBoolField(TEXT("auto_scale"), bAutoScale);
	bool bReplaceExisting = false;
	Params->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);

	UAnimSequenceBase* Source = FMonolithAssetUtils::LoadAssetByPath<UAnimSequenceBase>(SourcePath);
	if (!Source) return FMonolithActionResult::Error(FString::Printf(TEXT("Source animation not found: %s"), *SourcePath));

	UAnimSequenceBase* Target = FMonolithAssetUtils::LoadAssetByPath<UAnimSequenceBase>(TargetPath);
	if (!Target) return FMonolithActionResult::Error(FString::Printf(TEXT("Target animation not found: %s"), *TargetPath));

	if (Source == Target)
		return FMonolithActionResult::Error(TEXT("Source and target cannot be the same asset"));

	if (Source->Notifies.Num() == 0)
		return FMonolithActionResult::Error(TEXT("Source has no notifies to clone"));

	// Compute time scale
	if (bAutoScale && Source->GetPlayLength() > 0.f)
	{
		TimeScale = Target->GetPlayLength() / Source->GetPlayLength();
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Clone Notify Setup")));
	Target->Modify();

	// Clear existing notifies if requested
	if (bReplaceExisting)
	{
		Target->Notifies.Empty();
	}

	int32 ClonedCount = 0;
	int32 SkippedCount = 0;

	for (const FAnimNotifyEvent& SrcEvent : Source->Notifies)
	{
		float ScaledTime = SrcEvent.GetTime() * TimeScale;

		// Clamp to target play length
		if (ScaledTime > Target->GetPlayLength())
		{
			ScaledTime = Target->GetPlayLength();
		}

		if (SrcEvent.Notify)
		{
			// Clone instant notify
			UAnimNotify* ClonedNotify = DuplicateObject<UAnimNotify>(SrcEvent.Notify, Target);
			if (ClonedNotify)
			{
				// Ensure target has a track for this notify
				FName TrackName = TEXT("1");
				if (SrcEvent.TrackIndex >= 0 && SrcEvent.TrackIndex < Source->AnimNotifyTracks.Num())
				{
					TrackName = Source->AnimNotifyTracks[SrcEvent.TrackIndex].TrackName;
				}

				// Create track on target if needed
				bool bTrackFound = false;
				for (const FAnimNotifyTrack& Track : Target->AnimNotifyTracks)
				{
					if (Track.TrackName == TrackName) { bTrackFound = true; break; }
				}
				if (!bTrackFound)
				{
					Target->AnimNotifyTracks.AddDefaulted();
					Target->AnimNotifyTracks.Last().TrackName = TrackName;
				}

				UAnimationBlueprintLibrary::AddAnimationNotifyEventObject(Target, ScaledTime, ClonedNotify, TrackName);
				ClonedCount++;
			}
			else
			{
				SkippedCount++;
			}
		}
		else if (SrcEvent.NotifyStateClass)
		{
			// Clone state notify
			UAnimNotifyState* ClonedState = DuplicateObject<UAnimNotifyState>(SrcEvent.NotifyStateClass, Target);
			if (ClonedState)
			{
				float ScaledDuration = SrcEvent.GetDuration() * TimeScale;
				// Clamp duration so it doesn't exceed target length
				if (ScaledTime + ScaledDuration > Target->GetPlayLength())
				{
					ScaledDuration = Target->GetPlayLength() - ScaledTime;
				}
				if (ScaledDuration <= 0.f)
				{
					SkippedCount++;
					continue;
				}

				FName TrackName = TEXT("1");
				if (SrcEvent.TrackIndex >= 0 && SrcEvent.TrackIndex < Source->AnimNotifyTracks.Num())
				{
					TrackName = Source->AnimNotifyTracks[SrcEvent.TrackIndex].TrackName;
				}

				bool bTrackFound = false;
				for (const FAnimNotifyTrack& Track : Target->AnimNotifyTracks)
				{
					if (Track.TrackName == TrackName) { bTrackFound = true; break; }
				}
				if (!bTrackFound)
				{
					Target->AnimNotifyTracks.AddDefaulted();
					Target->AnimNotifyTracks.Last().TrackName = TrackName;
				}

				UAnimationBlueprintLibrary::AddAnimationNotifyStateEventObject(Target, ScaledTime, ScaledDuration, ClonedState, TrackName);
				ClonedCount++;
			}
			else
			{
				SkippedCount++;
			}
		}
		else
		{
			// Skeleton notify (name-based, no UObject) — skip these for now
			SkippedCount++;
		}
	}

	Target->RefreshCacheData();
	GEditor->EndTransaction();
	Target->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("source_path"), SourcePath);
	Root->SetStringField(TEXT("target_path"), TargetPath);
	Root->SetNumberField(TEXT("time_scale"), TimeScale);
	Root->SetNumberField(TEXT("cloned_count"), ClonedCount);
	Root->SetNumberField(TEXT("skipped_count"), SkippedCount);
	Root->SetNumberField(TEXT("target_notify_count"), Target->Notifies.Num());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleBulkAddNotify(const TSharedPtr<FJsonObject>& Params)
{
	// Parse asset_paths — handle both Array and String (Claude Code quirk)
	TArray<TSharedPtr<FJsonValue>> PathValues;
	TSharedPtr<FJsonValue> PathsField = Params->TryGetField(TEXT("asset_paths"));
	if (!PathsField.IsValid())
		return FMonolithActionResult::Error(TEXT("Missing required field: asset_paths"));

	if (PathsField->Type == EJson::Array)
	{
		PathValues = PathsField->AsArray();
	}
	else if (PathsField->Type == EJson::String)
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PathsField->AsString());
		if (!FJsonSerializer::Deserialize(Reader, PathValues))
			return FMonolithActionResult::Error(TEXT("Failed to parse asset_paths string as JSON array"));
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("'asset_paths' must be an array"));
	}

	if (PathValues.Num() == 0)
		return FMonolithActionResult::Error(TEXT("asset_paths array is empty"));

	FString NotifyClassName = Params->GetStringField(TEXT("notify_class"));
	float Time = static_cast<float>(Params->GetNumberField(TEXT("time")));
	FString TimeMode = TEXT("absolute");
	Params->TryGetStringField(TEXT("time_mode"), TimeMode);
	bool bIsNormalized = TimeMode.Equals(TEXT("normalized"), ESearchCase::IgnoreCase);

	float Duration = 0.f;
	bool bIsState = Params->HasField(TEXT("duration"));
	if (bIsState)
	{
		Duration = static_cast<float>(Params->GetNumberField(TEXT("duration")));
	}

	FString TrackName = TEXT("1");
	Params->TryGetStringField(TEXT("track_name"), TrackName);

	// Resolve notify class
	bool bIsNotifyState = false;
	UClass* NotifyClass = FindFirstObject<UClass>(*NotifyClassName, EFindFirstObjectOptions::NativeFirst);
	if (!NotifyClass)
		NotifyClass = FindFirstObject<UClass>(*FString::Printf(TEXT("AnimNotify_%s"), *NotifyClassName), EFindFirstObjectOptions::NativeFirst);
	if (!NotifyClass)
		NotifyClass = FindFirstObject<UClass>(*FString::Printf(TEXT("AnimNotifyState_%s"), *NotifyClassName), EFindFirstObjectOptions::NativeFirst);

	if (!NotifyClass)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Notify class not found: %s"), *NotifyClassName));

	if (NotifyClass->IsChildOf(UAnimNotifyState::StaticClass()))
	{
		bIsNotifyState = true;
		if (!bIsState)
			return FMonolithActionResult::Error(TEXT("Notify class is a state notify — 'duration' parameter is required"));
	}
	else if (!NotifyClass->IsChildOf(UAnimNotify::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Class '%s' is not a UAnimNotify or UAnimNotifyState subclass"), *NotifyClassName));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Bulk Add Notify")));

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 Ok = 0, Fail = 0;

	for (int32 i = 0; i < PathValues.Num(); ++i)
	{
		FString AssetPath = PathValues[i]->AsString();
		TSharedRef<FJsonObject> RO = MakeShared<FJsonObject>();
		RO->SetNumberField(TEXT("index"), i);
		RO->SetStringField(TEXT("asset_path"), AssetPath);

		UAnimSequenceBase* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequenceBase>(AssetPath);
		if (!Seq)
		{
			RO->SetBoolField(TEXT("success"), false);
			RO->SetStringField(TEXT("error"), FString::Printf(TEXT("Animation asset not found: %s"), *AssetPath));
			Results.Add(MakeShared<FJsonValueObject>(RO));
			Fail++;
			continue;
		}

		float ActualTime = bIsNormalized ? Time * Seq->GetPlayLength() : Time;

		if (ActualTime < 0.f || ActualTime > Seq->GetPlayLength())
		{
			RO->SetBoolField(TEXT("success"), false);
			RO->SetStringField(TEXT("error"), FString::Printf(TEXT("Time %.3f out of range [0, %.3f]"), ActualTime, Seq->GetPlayLength()));
			Results.Add(MakeShared<FJsonValueObject>(RO));
			Fail++;
			continue;
		}

		Seq->Modify();

		if (bIsNotifyState)
		{
			float ActualDuration = bIsNormalized ? Duration * Seq->GetPlayLength() : Duration;
			if (ActualTime + ActualDuration > Seq->GetPlayLength())
				ActualDuration = Seq->GetPlayLength() - ActualTime;

			UAnimNotifyState* NewState = NewObject<UAnimNotifyState>(Seq, NotifyClass);
			UAnimationBlueprintLibrary::AddAnimationNotifyStateEventObject(Seq, ActualTime, ActualDuration, NewState, FName(*TrackName));
			Seq->RefreshCacheData();
			Seq->MarkPackageDirty();

			RO->SetBoolField(TEXT("success"), true);
			RO->SetNumberField(TEXT("time"), ActualTime);
			RO->SetNumberField(TEXT("duration"), ActualDuration);
		}
		else
		{
			UAnimNotify* NewNotify = NewObject<UAnimNotify>(Seq, NotifyClass);
			UAnimationBlueprintLibrary::AddAnimationNotifyEventObject(Seq, ActualTime, NewNotify, FName(*TrackName));
			Seq->RefreshCacheData();
			Seq->MarkPackageDirty();

			RO->SetBoolField(TEXT("success"), true);
			RO->SetNumberField(TEXT("time"), ActualTime);
		}

		Results.Add(MakeShared<FJsonValueObject>(RO));
		Ok++;
	}

	GEditor->EndTransaction();

	TSharedRef<FJsonObject> Final = MakeShared<FJsonObject>();
	Final->SetBoolField(TEXT("success"), Fail == 0);
	Final->SetNumberField(TEXT("total"), PathValues.Num());
	Final->SetNumberField(TEXT("succeeded"), Ok);
	Final->SetNumberField(TEXT("failed"), Fail);
	Final->SetStringField(TEXT("notify_class"), NotifyClass->GetName());
	Final->SetArrayField(TEXT("results"), Results);
	return FMonolithActionResult::Success(Final);
}

// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleCreateMontageFromSections(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SkeletonPath = Params->GetStringField(TEXT("skeleton_path"));
	FString SlotName = TEXT("DefaultSlot");
	Params->TryGetStringField(TEXT("slot_name"), SlotName);

	// Step 1: Create the montage
	TSharedRef<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), AssetPath);
	CreateParams->SetStringField(TEXT("skeleton_path"), SkeletonPath);

	FMonolithActionResult CreateResult = HandleCreateMontage(CreateParams);
	if (!CreateResult.bSuccess)
		return CreateResult;

	UAnimMontage* Montage = FMonolithAssetUtils::LoadAssetByPath<UAnimMontage>(AssetPath);
	if (!Montage)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Created montage but failed to load: %s"), *AssetPath));

	GEditor->BeginTransaction(FText::FromString(TEXT("Create Montage From Sections")));
	Montage->Modify();

	TArray<FString> Errors;

	// Step 2: Rename default slot if needed
	if (SlotName != TEXT("DefaultSlot") && Montage->SlotAnimTracks.Num() > 0)
	{
		Montage->SlotAnimTracks[0].SlotName = FName(*SlotName);
	}

	// Step 3: Process sections
	const TArray<TSharedPtr<FJsonValue>>* SectionsArr = nullptr;
	if (Params->TryGetArrayField(TEXT("sections"), SectionsArr) && SectionsArr)
	{
		// Remove the auto-created "Default" section if user is providing custom sections
		if (SectionsArr->Num() > 0)
		{
			// Clear all existing composite sections
			Montage->CompositeSections.Empty();
		}

		for (int32 i = 0; i < SectionsArr->Num(); ++i)
		{
			TSharedPtr<FJsonObject> SecObj = (*SectionsArr)[i]->AsObject();
			if (!SecObj.IsValid()) continue;

			FString SectionName = SecObj->GetStringField(TEXT("name"));
			float StartTime = SecObj->HasField(TEXT("start_time")) ? static_cast<float>(SecObj->GetNumberField(TEXT("start_time"))) : 0.0f;

			// Add section
			Montage->AddAnimCompositeSection(FName(*SectionName), StartTime);

			// Add anim segment if anim_path provided
			FString SectionAnimPath;
			if (SecObj->TryGetStringField(TEXT("anim_path"), SectionAnimPath) && !SectionAnimPath.IsEmpty())
			{
				UAnimSequenceBase* Anim = FMonolithAssetUtils::LoadAssetByPath<UAnimSequenceBase>(SectionAnimPath);
				if (Anim)
				{
					FAnimSegment NewSeg;
					NewSeg.SetAnimReference(Anim);
					NewSeg.StartPos = StartTime;
					NewSeg.AnimStartTime = 0.0f;
					NewSeg.AnimEndTime = Anim->GetPlayLength();
					NewSeg.AnimPlayRate = 1.0f;
					NewSeg.LoopingCount = 1;

					if (Montage->SlotAnimTracks.Num() > 0)
					{
						Montage->SlotAnimTracks[0].AnimTrack.AnimSegments.Add(NewSeg);
					}
				}
				else
				{
					Errors.Add(FString::Printf(TEXT("Section '%s': animation not found: %s"), *SectionName, *SectionAnimPath));
				}
			}
		}

		// Set section flow (next_section links) in a second pass
		for (int32 i = 0; i < SectionsArr->Num(); ++i)
		{
			TSharedPtr<FJsonObject> SecObj = (*SectionsArr)[i]->AsObject();
			if (!SecObj.IsValid()) continue;

			FString NextSection;
			if (SecObj->TryGetStringField(TEXT("next_section"), NextSection) && !NextSection.IsEmpty())
			{
				FString SectionName = SecObj->GetStringField(TEXT("name"));
				int32 SecIdx = Montage->GetSectionIndex(FName(*SectionName));
				if (SecIdx != INDEX_NONE)
				{
					FCompositeSection& Sec = Montage->GetAnimCompositeSection(SecIdx);
					Sec.NextSectionName = FName(*NextSection);
				}
			}
		}
	}

	// Step 4: Apply blend settings
	const TSharedPtr<FJsonObject>* BlendObj = nullptr;
	if (Params->TryGetObjectField(TEXT("blend"), BlendObj) && BlendObj && BlendObj->IsValid())
	{
		if ((*BlendObj)->HasField(TEXT("blend_in_time")))
			Montage->BlendIn.SetBlendTime(static_cast<float>((*BlendObj)->GetNumberField(TEXT("blend_in_time"))));
		if ((*BlendObj)->HasField(TEXT("blend_out_time")))
			Montage->BlendOut.SetBlendTime(static_cast<float>((*BlendObj)->GetNumberField(TEXT("blend_out_time"))));
		if ((*BlendObj)->HasField(TEXT("blend_out_trigger_time")))
			Montage->BlendOutTriggerTime = static_cast<float>((*BlendObj)->GetNumberField(TEXT("blend_out_trigger_time")));
		bool bAutoBlendOut = true;
		if ((*BlendObj)->TryGetBoolField(TEXT("enable_auto_blend_out"), bAutoBlendOut))
			Montage->bEnableAutoBlendOut = bAutoBlendOut;
	}

	// Step 5: Add notifies
	const TArray<TSharedPtr<FJsonValue>>* NotifiesArr = nullptr;
	if (Params->TryGetArrayField(TEXT("notifies"), NotifiesArr) && NotifiesArr)
	{
		for (const auto& NVal : *NotifiesArr)
		{
			TSharedPtr<FJsonObject> NObj = NVal->AsObject();
			if (!NObj.IsValid()) continue;

			FString NClassName = NObj->GetStringField(TEXT("notify_class"));
			float NTime = static_cast<float>(NObj->GetNumberField(TEXT("time")));
			FString NTrackName = TEXT("1");
			NObj->TryGetStringField(TEXT("track_name"), NTrackName);

			UClass* NClass = FindFirstObject<UClass>(*NClassName, EFindFirstObjectOptions::NativeFirst);
			if (!NClass)
				NClass = FindFirstObject<UClass>(*FString::Printf(TEXT("AnimNotify_%s"), *NClassName), EFindFirstObjectOptions::NativeFirst);
			if (!NClass)
				NClass = FindFirstObject<UClass>(*FString::Printf(TEXT("AnimNotifyState_%s"), *NClassName), EFindFirstObjectOptions::NativeFirst);

			if (!NClass)
			{
				Errors.Add(FString::Printf(TEXT("Notify class not found: %s"), *NClassName));
				continue;
			}

			if (NClass->IsChildOf(UAnimNotifyState::StaticClass()))
			{
				float NDuration = NObj->HasField(TEXT("duration")) ? static_cast<float>(NObj->GetNumberField(TEXT("duration"))) : 0.1f;
				UAnimNotifyState* NS = NewObject<UAnimNotifyState>(Montage, NClass);
				UAnimationBlueprintLibrary::AddAnimationNotifyStateEventObject(Montage, NTime, NDuration, NS, FName(*NTrackName));
			}
			else if (NClass->IsChildOf(UAnimNotify::StaticClass()))
			{
				UAnimNotify* N = NewObject<UAnimNotify>(Montage, NClass);
				UAnimationBlueprintLibrary::AddAnimationNotifyEventObject(Montage, NTime, N, FName(*NTrackName));
			}
		}
		Montage->RefreshCacheData();
	}

	GEditor->EndTransaction();
	Montage->MarkPackageDirty();

	// Build response (reuse get_montage_info-style output)
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	Root->SetStringField(TEXT("skeleton"), Montage->GetSkeleton() ? Montage->GetSkeleton()->GetPathName() : TEXT("None"));
	Root->SetStringField(TEXT("slot_name"), SlotName);
	Root->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
	Root->SetNumberField(TEXT("slot_count"), Montage->SlotAnimTracks.Num());
	Root->SetNumberField(TEXT("notify_count"), Montage->Notifies.Num());

	if (Errors.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ErrArr;
		for (const FString& Err : Errors)
			ErrArr.Add(MakeShared<FJsonValueString>(Err));
		Root->SetArrayField(TEXT("warnings"), ErrArr);
	}

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleBuildSequenceFromPoses(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SkeletonPath = Params->GetStringField(TEXT("skeleton_path"));
	int32 FrameRate = Params->HasField(TEXT("frame_rate")) ? static_cast<int32>(Params->GetNumberField(TEXT("frame_rate"))) : 30;

	if (FrameRate <= 0) FrameRate = 30;

	// Parse frames array — handle both Array and String (Claude Code quirk)
	TArray<TSharedPtr<FJsonValue>> FramesArr;
	TSharedPtr<FJsonValue> FramesField = Params->TryGetField(TEXT("frames"));
	if (!FramesField.IsValid())
		return FMonolithActionResult::Error(TEXT("Missing required field: frames"));

	if (FramesField->Type == EJson::Array)
	{
		FramesArr = FramesField->AsArray();
	}
	else if (FramesField->Type == EJson::String)
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FramesField->AsString());
		if (!FJsonSerializer::Deserialize(Reader, FramesArr))
			return FMonolithActionResult::Error(TEXT("Failed to parse frames string as JSON array"));
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("'frames' must be an array"));
	}

	if (FramesArr.Num() == 0)
		return FMonolithActionResult::Error(TEXT("frames array is empty"));

	int32 FrameCount = FramesArr.Num();

	// Load skeleton
	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(SkeletonPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath));

	// Create or load sequence
	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq)
	{
		// Create new sequence
		FString AssetName;
		int32 LastSlash;
		if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
		AssetName = AssetPath.Mid(LastSlash + 1);

		UPackage* Pkg = CreatePackage(*AssetPath);
		if (!Pkg) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));

		Seq = NewObject<UAnimSequence>(Pkg, FName(*AssetName), RF_Public | RF_Standalone);
		if (!Seq) return FMonolithActionResult::Error(TEXT("Failed to create UAnimSequence object"));

		Seq->SetSkeleton(Skeleton);
		FAssetRegistryModule::AssetCreated(Seq);
	}

	// Collect unique bone names across all frames
	TSet<FName> BoneNameSet;
	for (const auto& FrameVal : FramesArr)
	{
		TSharedPtr<FJsonObject> FrameObj = FrameVal->AsObject();
		if (!FrameObj.IsValid()) continue;

		const TArray<TSharedPtr<FJsonValue>>* BonesArr = nullptr;
		if (FrameObj->TryGetArrayField(TEXT("bones"), BonesArr) && BonesArr)
		{
			for (const auto& BoneVal : *BonesArr)
			{
				TSharedPtr<FJsonObject> BoneObj = BoneVal->AsObject();
				if (BoneObj.IsValid() && BoneObj->HasField(TEXT("name")))
				{
					BoneNameSet.Add(FName(*BoneObj->GetStringField(TEXT("name"))));
				}
			}
		}
	}

	if (BoneNameSet.Num() == 0)
		return FMonolithActionResult::Error(TEXT("No bone data found in frames"));

	// Build per-bone arrays: BoneName -> { Positions[], Rotations[], Scales[] }
	struct FBoneTrackData
	{
		TArray<FVector> Positions;
		TArray<FQuat> Rotations;
		TArray<FVector> Scales;
	};

	TMap<FName, FBoneTrackData> BoneDataMap;
	for (const FName& BN : BoneNameSet)
	{
		FBoneTrackData& Data = BoneDataMap.Add(BN);
		Data.Positions.SetNum(FrameCount);
		Data.Rotations.SetNum(FrameCount);
		Data.Scales.SetNum(FrameCount);
		// Initialize with identity
		for (int32 f = 0; f < FrameCount; ++f)
		{
			Data.Positions[f] = FVector::ZeroVector;
			Data.Rotations[f] = FQuat::Identity;
			Data.Scales[f] = FVector::OneVector;
		}
	}

	// Parse per-frame data
	for (int32 f = 0; f < FrameCount; ++f)
	{
		TSharedPtr<FJsonObject> FrameObj = FramesArr[f]->AsObject();
		if (!FrameObj.IsValid()) continue;

		const TArray<TSharedPtr<FJsonValue>>* BonesArr = nullptr;
		if (!FrameObj->TryGetArrayField(TEXT("bones"), BonesArr) || !BonesArr) continue;

		for (const auto& BoneVal : *BonesArr)
		{
			TSharedPtr<FJsonObject> BoneObj = BoneVal->AsObject();
			if (!BoneObj.IsValid()) continue;

			FName BoneName(*BoneObj->GetStringField(TEXT("name")));
			FBoneTrackData* Data = BoneDataMap.Find(BoneName);
			if (!Data) continue;

			const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
			if (BoneObj->TryGetArrayField(TEXT("location"), LocArr) && LocArr && LocArr->Num() >= 3)
			{
				Data->Positions[f] = FVector((*LocArr)[0]->AsNumber(), (*LocArr)[1]->AsNumber(), (*LocArr)[2]->AsNumber());
			}

			const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
			if (BoneObj->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr && RotArr->Num() >= 4)
			{
				Data->Rotations[f] = FQuat((*RotArr)[0]->AsNumber(), (*RotArr)[1]->AsNumber(), (*RotArr)[2]->AsNumber(), (*RotArr)[3]->AsNumber());
			}

			const TArray<TSharedPtr<FJsonValue>>* ScaleArr = nullptr;
			if (BoneObj->TryGetArrayField(TEXT("scale"), ScaleArr) && ScaleArr && ScaleArr->Num() >= 3)
			{
				Data->Scales[f] = FVector((*ScaleArr)[0]->AsNumber(), (*ScaleArr)[1]->AsNumber(), (*ScaleArr)[2]->AsNumber());
			}
		}
	}

	// Apply data via IAnimationDataController
	IAnimationDataController& Controller = Seq->GetController();

	Controller.OpenBracket(FText::FromString(TEXT("Build Sequence From Poses")), false);

	Controller.SetFrameRate(FFrameRate(FrameRate, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(FrameCount - 1), false);

	for (auto& Pair : BoneDataMap)
	{
		Controller.AddBoneCurve(Pair.Key, false);
		Controller.SetBoneTrackKeys(Pair.Key, Pair.Value.Positions, Pair.Value.Rotations, Pair.Value.Scales, false);
	}

	Controller.CloseBracket(false);

	Seq->MarkPackageDirty();

	float Duration = (FrameCount > 1) ? static_cast<float>(FrameCount - 1) / static_cast<float>(FrameRate) : 0.f;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Seq->GetPathName());
	Root->SetNumberField(TEXT("frame_count"), FrameCount);
	Root->SetNumberField(TEXT("frame_rate"), FrameRate);
	Root->SetNumberField(TEXT("duration"), Duration);
	Root->SetNumberField(TEXT("bone_count"), BoneNameSet.Num());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// set_notify_properties — Wave 14
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleSetNotifyProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	int32 NotifyIndex = static_cast<int32>(Params->GetNumberField(TEXT("notify_index")));

	UAnimSequenceBase* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequenceBase>(AssetPath);
	if (!Seq) return FMonolithActionResult::Error(FString::Printf(TEXT("Animation asset not found: %s"), *AssetPath));

	if (NotifyIndex < 0 || NotifyIndex >= Seq->Notifies.Num())
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid notify_index %d (asset has %d notifies)"), NotifyIndex, Seq->Notifies.Num()));

	// Get the UObject target — instant notify or state notify
	FAnimNotifyEvent& NotifyEvent = Seq->Notifies[NotifyIndex];
	UObject* Target = nullptr;
	FString NotifyType;
	if (NotifyEvent.Notify)
	{
		Target = NotifyEvent.Notify;
		NotifyType = TEXT("AnimNotify");
	}
	else if (NotifyEvent.NotifyStateClass)
	{
		Target = NotifyEvent.NotifyStateClass;
		NotifyType = TEXT("AnimNotifyState");
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Notify at index %d has no Notify or NotifyStateClass object"), NotifyIndex));
	}

	// Parse properties object
	const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("properties"), PropsPtr) || !PropsPtr || !PropsPtr->IsValid())
	{
		// Handle Claude Code string serialization quirk
		FString PropsString;
		if (Params->TryGetStringField(TEXT("properties"), PropsString))
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PropsString);
			TSharedPtr<FJsonObject> ParsedProps;
			if (FJsonSerializer::Deserialize(Reader, ParsedProps) && ParsedProps.IsValid())
			{
				// Store parsed object back — use a local for the pointer
				const_cast<FJsonObject*>(Params.Get())->SetObjectField(TEXT("properties"), ParsedProps);
				Params->TryGetObjectField(TEXT("properties"), PropsPtr);
			}
			else
			{
				return FMonolithActionResult::Error(TEXT("Failed to parse 'properties' as JSON object"));
			}
		}
		else
		{
			return FMonolithActionResult::Error(TEXT("'properties' parameter is required and must be a JSON object"));
		}
	}

	if (!PropsPtr || !PropsPtr->IsValid() || (*PropsPtr)->Values.Num() == 0)
		return FMonolithActionResult::Error(TEXT("'properties' object is empty"));

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Notify Properties")));
	Seq->Modify();

	TArray<TSharedPtr<FJsonValue>> ResultArray;
	bool bAnyFailed = false;

	for (auto& Pair : (*PropsPtr)->Values)
	{
		const FString PropName = MonolithKeyToString(Pair.Key);
		FString ValueStr = Pair.Value->AsString();

		TSharedPtr<FJsonObject> PropResult = MakeShared<FJsonObject>();
		PropResult->SetStringField(TEXT("property"), PropName);
		PropResult->SetStringField(TEXT("requested_value"), ValueStr);

		// Find property — exact match first, case-insensitive fallback
		FProperty* Prop = Target->GetClass()->FindPropertyByName(FName(*PropName));
		if (!Prop)
		{
			for (TFieldIterator<FProperty> It(Target->GetClass()); It; ++It)
			{
				if (It->GetName().Equals(PropName, ESearchCase::IgnoreCase))
				{
					Prop = *It;
					break;
				}
			}
		}

		if (!Prop)
		{
			PropResult->SetBoolField(TEXT("success"), false);
			PropResult->SetStringField(TEXT("error"), FString::Printf(TEXT("Property '%s' not found on %s"), *PropName, *Target->GetClass()->GetName()));
			ResultArray.Add(MakeShared<FJsonValueObject>(PropResult));
			bAnyFailed = true;
			continue;
		}

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Target);

		// Read old value for reporting
		FString OldValue;
		Prop->ExportText_Direct(OldValue, ValuePtr, ValuePtr, Target, PPF_None);

		// Set new value
		const TCHAR* ImportResult = Prop->ImportText_Direct(*ValueStr, ValuePtr, Target, PPF_None);
		if (!ImportResult)
		{
			PropResult->SetBoolField(TEXT("success"), false);
			PropResult->SetStringField(TEXT("error"), FString::Printf(TEXT("ImportText_Direct failed for '%s' with value '%s'"), *PropName, *ValueStr));
			PropResult->SetStringField(TEXT("old_value"), OldValue);
			ResultArray.Add(MakeShared<FJsonValueObject>(PropResult));
			bAnyFailed = true;
			continue;
		}

		PropResult->SetBoolField(TEXT("success"), true);
		PropResult->SetStringField(TEXT("old_value"), OldValue);

		// Read back new value to confirm
		FString NewValue;
		Prop->ExportText_Direct(NewValue, ValuePtr, ValuePtr, Target, PPF_None);
		PropResult->SetStringField(TEXT("new_value"), NewValue);

		ResultArray.Add(MakeShared<FJsonValueObject>(PropResult));
	}

	GEditor->EndTransaction();

	// Refresh notify cache
	Seq->RefreshCacheData();
	Seq->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("notify_index"), NotifyIndex);
	Root->SetStringField(TEXT("notify_class"), Target->GetClass()->GetName());
	Root->SetStringField(TEXT("notify_type"), NotifyType);
	Root->SetArrayField(TEXT("results"), ResultArray);
	Root->SetBoolField(TEXT("all_succeeded"), !bAnyFailed);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Wave 15 — Physics Assets
// ---------------------------------------------------------------------------

static FString PhysicsTypeToString(EPhysicsType Type)
{
	switch (Type)
	{
		case PhysType_Default:    return TEXT("Default");
		case PhysType_Kinematic:  return TEXT("Kinematic");
		case PhysType_Simulated:  return TEXT("Simulated");
		default:                  return TEXT("Unknown");
	}
}

static EPhysicsType StringToPhysicsType(const FString& Str)
{
	if (Str.Equals(TEXT("Kinematic"), ESearchCase::IgnoreCase)) return PhysType_Kinematic;
	if (Str.Equals(TEXT("Simulated"), ESearchCase::IgnoreCase)) return PhysType_Simulated;
	return PhysType_Default;
}

static FString ConstraintMotionToString(EAngularConstraintMotion Motion)
{
	switch (Motion)
	{
		case ACM_Free:    return TEXT("Free");
		case ACM_Limited: return TEXT("Limited");
		case ACM_Locked:  return TEXT("Locked");
		default:          return TEXT("Unknown");
	}
}

static EAngularConstraintMotion StringToConstraintMotion(const FString& Str)
{
	if (Str.Equals(TEXT("Free"), ESearchCase::IgnoreCase))    return ACM_Free;
	if (Str.Equals(TEXT("Limited"), ESearchCase::IgnoreCase)) return ACM_Limited;
	return ACM_Locked;
}

static FString GetShapeTypeString(const UBodySetup* BodySetup)
{
	if (!BodySetup) return TEXT("None");
	const FKAggregateGeom& Geom = BodySetup->AggGeom;
	if (Geom.SphylElems.Num() > 0) return TEXT("Capsule");
	if (Geom.SphereElems.Num() > 0) return TEXT("Sphere");
	if (Geom.BoxElems.Num() > 0) return TEXT("Box");
	if (Geom.ConvexElems.Num() > 0) return TEXT("ConvexHull");
	if (Geom.TaperedCapsuleElems.Num() > 0) return TEXT("TaperedCapsule");
	return TEXT("None");
}

FMonolithActionResult FMonolithAnimationActions::HandleGetPhysicsAssetInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UPhysicsAsset* PhysAsset = FMonolithAssetUtils::LoadAssetByPath<UPhysicsAsset>(AssetPath);
	if (!PhysAsset) return FMonolithActionResult::Error(FString::Printf(TEXT("Physics asset not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), PhysAsset->GetPathName());

	// Bodies
	TArray<TSharedPtr<FJsonValue>> BodiesArr;
	for (int32 i = 0; i < PhysAsset->SkeletalBodySetups.Num(); ++i)
	{
		USkeletalBodySetup* BodySetup = PhysAsset->SkeletalBodySetups[i];
		if (!BodySetup) continue;

		TSharedPtr<FJsonObject> BodyObj = MakeShared<FJsonObject>();
		BodyObj->SetNumberField(TEXT("index"), i);
		BodyObj->SetStringField(TEXT("bone_name"), BodySetup->BoneName.ToString());
		BodyObj->SetStringField(TEXT("physics_type"), PhysicsTypeToString(BodySetup->PhysicsType));
		BodyObj->SetStringField(TEXT("shape_type"), GetShapeTypeString(BodySetup));

		const FBodyInstance& BI = BodySetup->DefaultInstance;
		BodyObj->SetNumberField(TEXT("mass"), BI.GetMassOverride());
		BodyObj->SetBoolField(TEXT("override_mass"), BI.bOverrideMass);
		BodyObj->SetNumberField(TEXT("linear_damping"), BI.LinearDamping);
		BodyObj->SetNumberField(TEXT("angular_damping"), BI.AngularDamping);
		BodyObj->SetStringField(TEXT("collision_profile"), BI.GetCollisionProfileName().ToString());
		BodyObj->SetBoolField(TEXT("simulate_physics"), BI.bSimulatePhysics);
		BodyObj->SetBoolField(TEXT("enable_gravity"), BI.bEnableGravity);

		// Geometry counts
		TSharedPtr<FJsonObject> GeomObj = MakeShared<FJsonObject>();
		const FKAggregateGeom& Geom = BodySetup->AggGeom;
		GeomObj->SetNumberField(TEXT("spheres"), Geom.SphereElems.Num());
		GeomObj->SetNumberField(TEXT("boxes"), Geom.BoxElems.Num());
		GeomObj->SetNumberField(TEXT("capsules"), Geom.SphylElems.Num());
		GeomObj->SetNumberField(TEXT("convex_hulls"), Geom.ConvexElems.Num());
		GeomObj->SetNumberField(TEXT("tapered_capsules"), Geom.TaperedCapsuleElems.Num());
		BodyObj->SetObjectField(TEXT("geometry"), GeomObj);

		BodiesArr.Add(MakeShared<FJsonValueObject>(BodyObj));
	}
	Root->SetArrayField(TEXT("bodies"), BodiesArr);
	Root->SetNumberField(TEXT("body_count"), BodiesArr.Num());

	// Constraints
	TArray<TSharedPtr<FJsonValue>> ConstraintsArr;
	for (int32 i = 0; i < PhysAsset->ConstraintSetup.Num(); ++i)
	{
		UPhysicsConstraintTemplate* CT = PhysAsset->ConstraintSetup[i];
		if (!CT) continue;

		const FConstraintInstance& CI = CT->DefaultInstance;
		const FConstraintProfileProperties& Profile = CI.ProfileInstance;

		TSharedPtr<FJsonObject> ConstraintObj = MakeShared<FJsonObject>();
		ConstraintObj->SetNumberField(TEXT("index"), i);
		ConstraintObj->SetStringField(TEXT("joint_name"), CI.JointName.ToString());
		ConstraintObj->SetStringField(TEXT("bone_1"), CI.ConstraintBone1.ToString());
		ConstraintObj->SetStringField(TEXT("bone_2"), CI.ConstraintBone2.ToString());

		// Angular limits
		TSharedPtr<FJsonObject> AngularObj = MakeShared<FJsonObject>();
		AngularObj->SetStringField(TEXT("swing1_motion"), ConstraintMotionToString(static_cast<EAngularConstraintMotion>(Profile.ConeLimit.Swing1Motion.GetValue())));
		AngularObj->SetNumberField(TEXT("swing1_limit"), Profile.ConeLimit.Swing1LimitDegrees);
		AngularObj->SetStringField(TEXT("swing2_motion"), ConstraintMotionToString(static_cast<EAngularConstraintMotion>(Profile.ConeLimit.Swing2Motion.GetValue())));
		AngularObj->SetNumberField(TEXT("swing2_limit"), Profile.ConeLimit.Swing2LimitDegrees);
		AngularObj->SetStringField(TEXT("twist_motion"), ConstraintMotionToString(static_cast<EAngularConstraintMotion>(Profile.TwistLimit.TwistMotion.GetValue())));
		AngularObj->SetNumberField(TEXT("twist_limit"), Profile.TwistLimit.TwistLimitDegrees);
		ConstraintObj->SetObjectField(TEXT("angular"), AngularObj);

		// Linear limits
		TSharedPtr<FJsonObject> LinearObj = MakeShared<FJsonObject>();
		LinearObj->SetNumberField(TEXT("limit"), Profile.LinearLimit.Limit);
		ConstraintObj->SetObjectField(TEXT("linear"), LinearObj);

		ConstraintObj->SetBoolField(TEXT("disable_collision"), Profile.bDisableCollision);

		ConstraintsArr.Add(MakeShared<FJsonValueObject>(ConstraintObj));
	}
	Root->SetArrayField(TEXT("constraints"), ConstraintsArr);
	Root->SetNumberField(TEXT("constraint_count"), ConstraintsArr.Num());

	// Physical animation profiles
#if WITH_EDITORONLY_DATA
	TArray<TSharedPtr<FJsonValue>> ProfilesArr;
	for (const FName& ProfileName : PhysAsset->GetPhysicalAnimationProfileNames())
	{
		ProfilesArr.Add(MakeShared<FJsonValueString>(ProfileName.ToString()));
	}
	Root->SetArrayField(TEXT("physical_animation_profiles"), ProfilesArr);

	TArray<TSharedPtr<FJsonValue>> ConstraintProfilesArr;
	for (const FName& ProfileName : PhysAsset->GetConstraintProfileNames())
	{
		ConstraintProfilesArr.Add(MakeShared<FJsonValueString>(ProfileName.ToString()));
	}
	Root->SetArrayField(TEXT("constraint_profiles"), ConstraintProfilesArr);
#endif

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetBodyProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString BoneName = Params->GetStringField(TEXT("bone_name"));

	UPhysicsAsset* PhysAsset = FMonolithAssetUtils::LoadAssetByPath<UPhysicsAsset>(AssetPath);
	if (!PhysAsset) return FMonolithActionResult::Error(FString::Printf(TEXT("Physics asset not found: %s"), *AssetPath));

	int32 BodyIdx = PhysAsset->FindBodyIndex(FName(*BoneName));
	if (BodyIdx == INDEX_NONE)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Body not found for bone: %s"), *BoneName));

	USkeletalBodySetup* BodySetup = PhysAsset->SkeletalBodySetups[BodyIdx];
	if (!BodySetup) return FMonolithActionResult::Error(TEXT("BodySetup is null"));

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Body Properties")));
	BodySetup->Modify();

	FBodyInstance& BI = BodySetup->DefaultInstance;
	TArray<FString> ModifiedProps;

	// Mass
	double MassVal;
	if (Params->TryGetNumberField(TEXT("mass"), MassVal))
	{
		BI.SetMassOverride(static_cast<float>(MassVal), true);
		ModifiedProps.Add(TEXT("mass"));
	}

	// Physics type
	FString PhysTypeStr;
	if (Params->TryGetStringField(TEXT("physics_type"), PhysTypeStr) && !PhysTypeStr.IsEmpty())
	{
		BodySetup->PhysicsType = StringToPhysicsType(PhysTypeStr);
		ModifiedProps.Add(TEXT("physics_type"));
	}

	// Collision enabled
	bool bCollisionEnabled;
	if (Params->TryGetBoolField(TEXT("collision_enabled"), bCollisionEnabled))
	{
		BI.SetCollisionEnabled(bCollisionEnabled ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
		ModifiedProps.Add(TEXT("collision_enabled"));
	}

	// Collision profile
	FString ProfileName;
	if (Params->TryGetStringField(TEXT("collision_profile"), ProfileName) && !ProfileName.IsEmpty())
	{
		BI.SetCollisionProfileName(FName(*ProfileName));
		ModifiedProps.Add(TEXT("collision_profile"));
	}

	// Linear damping
	double LinDamp;
	if (Params->TryGetNumberField(TEXT("linear_damping"), LinDamp))
	{
		BI.LinearDamping = static_cast<float>(LinDamp);
		ModifiedProps.Add(TEXT("linear_damping"));
	}

	// Angular damping
	double AngDamp;
	if (Params->TryGetNumberField(TEXT("angular_damping"), AngDamp))
	{
		BI.AngularDamping = static_cast<float>(AngDamp);
		ModifiedProps.Add(TEXT("angular_damping"));
	}

	// Enable gravity
	bool bGravity;
	if (Params->TryGetBoolField(TEXT("enable_gravity"), bGravity))
	{
		BI.bEnableGravity = bGravity;
		ModifiedProps.Add(TEXT("enable_gravity"));
	}

	GEditor->EndTransaction();
	PhysAsset->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("bone_name"), BoneName);

	TArray<TSharedPtr<FJsonValue>> ModArr;
	for (const FString& P : ModifiedProps)
	{
		ModArr.Add(MakeShared<FJsonValueString>(P));
	}
	Root->SetArrayField(TEXT("modified"), ModArr);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetConstraintProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UPhysicsAsset* PhysAsset = FMonolithAssetUtils::LoadAssetByPath<UPhysicsAsset>(AssetPath);
	if (!PhysAsset) return FMonolithActionResult::Error(FString::Printf(TEXT("Physics asset not found: %s"), *AssetPath));

	// Find constraint by index or bone pair
	int32 ConstraintIdx = INDEX_NONE;
	double IdxVal;
	if (Params->TryGetNumberField(TEXT("constraint_index"), IdxVal))
	{
		ConstraintIdx = static_cast<int32>(IdxVal);
	}
	else
	{
		FString Bone1, Bone2;
		if (Params->TryGetStringField(TEXT("bone_1"), Bone1) && Params->TryGetStringField(TEXT("bone_2"), Bone2)
			&& !Bone1.IsEmpty() && !Bone2.IsEmpty())
		{
			// Search through constraints for matching bone pair
			for (int32 i = 0; i < PhysAsset->ConstraintSetup.Num(); ++i)
			{
				if (!PhysAsset->ConstraintSetup[i]) continue;
				const FConstraintInstance& CI = PhysAsset->ConstraintSetup[i]->DefaultInstance;
				if ((CI.ConstraintBone1 == FName(*Bone1) && CI.ConstraintBone2 == FName(*Bone2)) ||
					(CI.ConstraintBone1 == FName(*Bone2) && CI.ConstraintBone2 == FName(*Bone1)))
				{
					ConstraintIdx = i;
					break;
				}
			}
		}
	}

	if (ConstraintIdx == INDEX_NONE || !PhysAsset->ConstraintSetup.IsValidIndex(ConstraintIdx))
		return FMonolithActionResult::Error(TEXT("Constraint not found. Provide constraint_index or bone_1+bone_2 pair."));

	UPhysicsConstraintTemplate* CT = PhysAsset->ConstraintSetup[ConstraintIdx];
	if (!CT) return FMonolithActionResult::Error(TEXT("Constraint template is null"));

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Constraint Properties")));
	CT->Modify();

	FConstraintProfileProperties& Profile = CT->DefaultInstance.ProfileInstance;
	TArray<FString> ModifiedProps;

	// Swing1
	FString Swing1MotionStr;
	if (Params->TryGetStringField(TEXT("swing1_motion"), Swing1MotionStr) && !Swing1MotionStr.IsEmpty())
	{
		Profile.ConeLimit.Swing1Motion = StringToConstraintMotion(Swing1MotionStr);
		ModifiedProps.Add(TEXT("swing1_motion"));
	}
	double Swing1Limit;
	if (Params->TryGetNumberField(TEXT("swing1_limit"), Swing1Limit))
	{
		Profile.ConeLimit.Swing1LimitDegrees = static_cast<float>(Swing1Limit);
		ModifiedProps.Add(TEXT("swing1_limit"));
	}

	// Swing2
	FString Swing2MotionStr;
	if (Params->TryGetStringField(TEXT("swing2_motion"), Swing2MotionStr) && !Swing2MotionStr.IsEmpty())
	{
		Profile.ConeLimit.Swing2Motion = StringToConstraintMotion(Swing2MotionStr);
		ModifiedProps.Add(TEXT("swing2_motion"));
	}
	double Swing2Limit;
	if (Params->TryGetNumberField(TEXT("swing2_limit"), Swing2Limit))
	{
		Profile.ConeLimit.Swing2LimitDegrees = static_cast<float>(Swing2Limit);
		ModifiedProps.Add(TEXT("swing2_limit"));
	}

	// Twist
	FString TwistMotionStr;
	if (Params->TryGetStringField(TEXT("twist_motion"), TwistMotionStr) && !TwistMotionStr.IsEmpty())
	{
		Profile.TwistLimit.TwistMotion = StringToConstraintMotion(TwistMotionStr);
		ModifiedProps.Add(TEXT("twist_motion"));
	}
	double TwistLimit;
	if (Params->TryGetNumberField(TEXT("twist_limit"), TwistLimit))
	{
		Profile.TwistLimit.TwistLimitDegrees = static_cast<float>(TwistLimit);
		ModifiedProps.Add(TEXT("twist_limit"));
	}

	// Disable collision
	bool bDisableCollision;
	if (Params->TryGetBoolField(TEXT("disable_collision"), bDisableCollision))
	{
		Profile.bDisableCollision = bDisableCollision;
		ModifiedProps.Add(TEXT("disable_collision"));
	}

	CT->UpdateProfileInstance();
	GEditor->EndTransaction();
	PhysAsset->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("constraint_index"), ConstraintIdx);
	Root->SetStringField(TEXT("bone_1"), CT->DefaultInstance.ConstraintBone1.ToString());
	Root->SetStringField(TEXT("bone_2"), CT->DefaultInstance.ConstraintBone2.ToString());

	TArray<TSharedPtr<FJsonValue>> ModArr;
	for (const FString& P : ModifiedProps)
	{
		ModArr.Add(MakeShared<FJsonValueString>(P));
	}
	Root->SetArrayField(TEXT("modified"), ModArr);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Wave 15 — IK Rig Retarget Chains
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleAddRetargetChain(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ChainName = Params->GetStringField(TEXT("chain_name"));
	FString StartBone = Params->GetStringField(TEXT("start_bone"));
	FString EndBone = Params->GetStringField(TEXT("end_bone"));

	UIKRigDefinition* Asset = FMonolithAssetUtils::LoadAssetByPath<UIKRigDefinition>(AssetPath);
	if (!Asset) return FMonolithActionResult::Error(FString::Printf(TEXT("IKRigDefinition not found: %s"), *AssetPath));

	UIKRigController* C = UIKRigController::GetController(Asset);
	if (!C) return FMonolithActionResult::Error(TEXT("Failed to get IKRigController"));

	FString GoalStr;
	FName GoalName = NAME_None;
	if (Params->TryGetStringField(TEXT("goal_name"), GoalStr) && !GoalStr.IsEmpty())
	{
		GoalName = FName(*GoalStr);
	}

	FName ResultName = C->AddRetargetChain(FName(*ChainName), FName(*StartBone), FName(*EndBone), GoalName);
	if (ResultName.IsNone())
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to add retarget chain '%s'"), *ChainName));

	C->SortRetargetChains();
	Asset->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("chain_name"), ResultName.ToString());
	Root->SetStringField(TEXT("start_bone"), StartBone);
	Root->SetStringField(TEXT("end_bone"), EndBone);
	Root->SetStringField(TEXT("goal"), GoalName.ToString());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleRemoveRetargetChain(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ChainName = Params->GetStringField(TEXT("chain_name"));

	UIKRigDefinition* Asset = FMonolithAssetUtils::LoadAssetByPath<UIKRigDefinition>(AssetPath);
	if (!Asset) return FMonolithActionResult::Error(FString::Printf(TEXT("IKRigDefinition not found: %s"), *AssetPath));

	UIKRigController* C = UIKRigController::GetController(Asset);
	if (!C) return FMonolithActionResult::Error(TEXT("Failed to get IKRigController"));

	bool bSuccess = C->RemoveRetargetChain(FName(*ChainName));
	if (!bSuccess)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to remove retarget chain '%s' — chain may not exist"), *ChainName));

	Asset->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("removed_chain"), ChainName);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetRetargetChainBones(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ChainName = Params->GetStringField(TEXT("chain_name"));

	UIKRigDefinition* Asset = FMonolithAssetUtils::LoadAssetByPath<UIKRigDefinition>(AssetPath);
	if (!Asset) return FMonolithActionResult::Error(FString::Printf(TEXT("IKRigDefinition not found: %s"), *AssetPath));

	UIKRigController* C = UIKRigController::GetController(Asset);
	if (!C) return FMonolithActionResult::Error(TEXT("Failed to get IKRigController"));

	// Verify chain exists
	const FBoneChain* Chain = C->GetRetargetChainByName(FName(*ChainName));
	if (!Chain)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Retarget chain not found: %s"), *ChainName));

	TArray<FString> ModifiedProps;
	FName CurrentChainName = FName(*ChainName);

	// Start bone
	FString StartBone;
	if (Params->TryGetStringField(TEXT("start_bone"), StartBone) && !StartBone.IsEmpty())
	{
		bool bOk = C->SetRetargetChainStartBone(CurrentChainName, FName(*StartBone));
		if (!bOk) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to set start bone '%s' on chain '%s'"), *StartBone, *ChainName));
		ModifiedProps.Add(TEXT("start_bone"));
	}

	// End bone
	FString EndBone;
	if (Params->TryGetStringField(TEXT("end_bone"), EndBone) && !EndBone.IsEmpty())
	{
		bool bOk = C->SetRetargetChainEndBone(CurrentChainName, FName(*EndBone));
		if (!bOk) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to set end bone '%s' on chain '%s'"), *EndBone, *ChainName));
		ModifiedProps.Add(TEXT("end_bone"));
	}

	// Goal
	FString GoalStr;
	if (Params->TryGetStringField(TEXT("goal_name"), GoalStr) && !GoalStr.IsEmpty())
	{
		bool bOk = C->SetRetargetChainGoal(CurrentChainName, FName(*GoalStr));
		if (!bOk) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to set goal '%s' on chain '%s'"), *GoalStr, *ChainName));
		ModifiedProps.Add(TEXT("goal_name"));
	}

	// Rename (do last since it changes the name we use to reference it)
	FString NewName;
	if (Params->TryGetStringField(TEXT("new_name"), NewName) && !NewName.IsEmpty())
	{
		FName RenamedName = C->RenameRetargetChain(CurrentChainName, FName(*NewName));
		if (RenamedName.IsNone())
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to rename chain '%s' to '%s'"), *ChainName, *NewName));
		CurrentChainName = RenamedName;
		ModifiedProps.Add(TEXT("new_name"));
	}

	Asset->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("chain_name"), CurrentChainName.ToString());

	TArray<TSharedPtr<FJsonValue>> ModArr;
	for (const FString& P : ModifiedProps)
	{
		ModArr.Add(MakeShared<FJsonValueString>(P));
	}
	Root->SetArrayField(TEXT("modified"), ModArr);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Retarget CREATE/RUN pack — create_ik_rig, create_ik_retargeter,
// set_retargeter_rigs, batch_retarget_animations.
//
// These four fill the only gap left by the shipped IK Rig / retargeter MUTATION
// actions: the ABILITY TO CREATE the assets and to run a cross-skeleton batch
// retarget. The mutation actions (add_ik_solver / add_retarget_chain /
// set_retarget_chain_mapping / ...) all require the asset to already exist.
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationActions::HandleCreateIKRig(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString MeshPath = Params->GetStringField(TEXT("skeletal_mesh_path"));

	USkeletalMesh* Mesh = FMonolithAssetUtils::LoadAssetByPath<USkeletalMesh>(MeshPath);
	if (!Mesh) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeletal mesh not found: %s"), *MeshPath));

	// Extract asset name from path
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	}
	const FString AssetName = AssetPath.Mid(LastSlash + 1);

	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));
	}

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));

	UIKRigDefinition* Rig = NewObject<UIKRigDefinition>(Pkg, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (!Rig) return FMonolithActionResult::Error(TEXT("Failed to create UIKRigDefinition object"));

	UIKRigController* C = UIKRigController::GetController(Rig);
	if (!C) return FMonolithActionResult::Error(TEXT("Failed to get IKRigController for new rig"));

	// Loads the mesh's skeleton into the rig's IKRigSkeleton.
	const bool bMeshSet = C->SetSkeletalMesh(Mesh);
	if (!bMeshSet)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("SetSkeletalMesh failed — mesh '%s' incompatible with IK Rig"), *MeshPath));
	}

	// Optional retarget root (pelvis) — must be set after the skeleton is loaded.
	bool bPelvisSet = false;
	FString PelvisBone;
	if (Params->TryGetStringField(TEXT("pelvis_bone"), PelvisBone) && !PelvisBone.IsEmpty())
	{
		bPelvisSet = C->SetRetargetRoot(FName(*PelvisBone));
	}

	FAssetRegistryModule::AssetCreated(Rig);
	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Rig->GetPathName());
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetStringField(TEXT("preview_mesh"), Mesh->GetPathName());
	Root->SetNumberField(TEXT("bone_count"), C->GetIKRigSkeleton().BoneNames.Num());
	if (!PelvisBone.IsEmpty())
	{
		Root->SetBoolField(TEXT("pelvis_set"), bPelvisSet);
		Root->SetStringField(TEXT("pelvis_bone"), C->GetRetargetRoot().ToString());
	}
	return FMonolithActionResult::Success(Root);
}

// Seed the default retarget-op stack and auto-map FK/IK chains so a freshly
// created (NewObject'd) retargeter actually transfers motion.
//
// A NewObject'd UIKRetargeter has an EMPTY op stack. The editor's asset factory
// (UIKRetargetFactory::FactoryCreateNew) seeds it via Controller->AddDefaultOps()
// right after creation; a programmatically created one never gets that, so
// UIKRetargetBatchOperation::RunRetarget transfers nothing and bakes a static
// pose into full-length tracks (frozen output). This replicates the engine's own
// seeding sequence (mirrors FProceduralRetargetAssets::AutoGenerateIKRetargetAsset
// in SRetargetAnimAssetsWindow.cpp): rigs are assigned first, then default ops are
// added, then chains are auto-mapped. AddDefaultOps is idempotent — ops already
// present are not re-added — so this is safe to call on an existing retargeter too.
//
// Returns the number of ops on the stack after seeding (for reporting).
static int32 SeedRetargeterDefaultOps(UIKRetargeterController* C, EAutoMapChainType AutoMapType)
{
	if (!C)
	{
		return 0;
	}

	// Add the default op set (Pelvis Motion, FK Chains, Run IK Rig, IK Chains,
	// Root Motion, Curve Remap) if not already present, and run each op's initial
	// setup so chain mappings are reinitialized against the assigned rigs.
	C->AddDefaultOps();

	// Auto-map the source->target retarget chains on every op that has a chain
	// mapping (FK/IK chains ops). Without a chain mapping there is nothing for
	// RunRetarget to transfer. bForceRemap=true so a re-seed re-maps cleanly.
	C->AutoMapChains(AutoMapType, /*bForceRemap=*/true, /*InOpName=*/NAME_None);

	return C->GetNumRetargetOps();
}

// Parse the optional "auto_map" param ("fuzzy" | "exact"); defaults to Fuzzy,
// matching the engine's default-op behavior for humanoid retargets.
static EAutoMapChainType ParseAutoMapType(const TSharedPtr<FJsonObject>& Params)
{
	FString AutoMapStr;
	if (Params->TryGetStringField(TEXT("auto_map"), AutoMapStr) && AutoMapStr.Equals(TEXT("exact"), ESearchCase::IgnoreCase))
	{
		return EAutoMapChainType::Exact;
	}
	return EAutoMapChainType::Fuzzy;
}

FMonolithActionResult FMonolithAnimationActions::HandleCreateIKRetargeter(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	}
	const FString AssetName = AssetPath.Mid(LastSlash + 1);

	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));
	}

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));

	UIKRetargeter* Retargeter = NewObject<UIKRetargeter>(Pkg, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (!Retargeter) return FMonolithActionResult::Error(TEXT("Failed to create UIKRetargeter object"));

	UIKRetargeterController* C = UIKRetargeterController::GetController(Retargeter);
	if (!C) return FMonolithActionResult::Error(TEXT("Failed to get UIKRetargeterController for new retargeter"));

	// Optional source/target rigs assigned at creation.
	FString SourceRigPath, TargetRigPath;
	bool bSourceSet = false, bTargetSet = false;
	if (Params->TryGetStringField(TEXT("source_ik_rig_path"), SourceRigPath) && !SourceRigPath.IsEmpty())
	{
		UIKRigDefinition* SourceRig = FMonolithAssetUtils::LoadAssetByPath<UIKRigDefinition>(SourceRigPath);
		if (!SourceRig) return FMonolithActionResult::Error(FString::Printf(TEXT("Source IK Rig not found: %s"), *SourceRigPath));
		C->SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
		bSourceSet = true;
	}
	if (Params->TryGetStringField(TEXT("target_ik_rig_path"), TargetRigPath) && !TargetRigPath.IsEmpty())
	{
		UIKRigDefinition* TargetRig = FMonolithAssetUtils::LoadAssetByPath<UIKRigDefinition>(TargetRigPath);
		if (!TargetRig) return FMonolithActionResult::Error(FString::Printf(TEXT("Target IK Rig not found: %s"), *TargetRigPath));
		C->SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);
		bTargetSet = true;
	}

	// If both rigs were assigned at creation, seed the default op stack + auto-map
	// chains so the retargeter is immediately usable by batch_retarget_animations.
	// Without this, a NewObject'd retargeter has an empty op stack and produces
	// frozen (static-pose) output. If only one/zero rigs given here, seeding is
	// deferred to set_retargeter_rigs (which has both rigs by definition).
	int32 OpCount = 0;
	if (bSourceSet && bTargetSet)
	{
		OpCount = SeedRetargeterDefaultOps(C, ParseAutoMapType(Params));
	}

	FAssetRegistryModule::AssetCreated(Retargeter);
	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Retargeter->GetPathName());
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetBoolField(TEXT("source_rig_set"), bSourceSet);
	Root->SetBoolField(TEXT("target_rig_set"), bTargetSet);
	Root->SetNumberField(TEXT("retarget_op_count"), OpCount);
	Root->SetBoolField(TEXT("default_ops_seeded"), OpCount > 0);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetRetargeterRigs(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SourceRigPath = Params->GetStringField(TEXT("source_ik_rig_path"));
	FString TargetRigPath = Params->GetStringField(TEXT("target_ik_rig_path"));

	UIKRetargeter* Retargeter = FMonolithAssetUtils::LoadAssetByPath<UIKRetargeter>(AssetPath);
	if (!Retargeter) return FMonolithActionResult::Error(FString::Printf(TEXT("IKRetargeter not found: %s"), *AssetPath));

	UIKRetargeterController* C = UIKRetargeterController::GetController(Retargeter);
	if (!C) return FMonolithActionResult::Error(TEXT("Failed to get UIKRetargeterController"));

	UIKRigDefinition* SourceRig = FMonolithAssetUtils::LoadAssetByPath<UIKRigDefinition>(SourceRigPath);
	if (!SourceRig) return FMonolithActionResult::Error(FString::Printf(TEXT("Source IK Rig not found: %s"), *SourceRigPath));
	UIKRigDefinition* TargetRig = FMonolithAssetUtils::LoadAssetByPath<UIKRigDefinition>(TargetRigPath);
	if (!TargetRig) return FMonolithActionResult::Error(FString::Printf(TEXT("Target IK Rig not found: %s"), *TargetRigPath));

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Retargeter Rigs")));
	Retargeter->Modify();

	C->SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
	C->SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);

	// Seed the default op stack + auto-map chains now that BOTH rigs are assigned.
	// SetIKRig only reinitializes chain mappings on ops that ALREADY exist, so on a
	// freshly created retargeter (empty op stack) the rigs alone produce no motion.
	// AddDefaultOps is idempotent, so re-running set_retargeter_rigs is harmless.
	const int32 OpCount = SeedRetargeterDefaultOps(C, ParseAutoMapType(Params));

	// Optional preview meshes.
	bool bSourceMeshSet = false, bTargetMeshSet = false;
	FString SourceMeshPath, TargetMeshPath;
	if (Params->TryGetStringField(TEXT("source_preview_mesh"), SourceMeshPath) && !SourceMeshPath.IsEmpty())
	{
		USkeletalMesh* SourceMesh = FMonolithAssetUtils::LoadAssetByPath<USkeletalMesh>(SourceMeshPath);
		if (!SourceMesh)
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Source preview mesh not found: %s"), *SourceMeshPath));
		}
		C->SetPreviewMesh(ERetargetSourceOrTarget::Source, SourceMesh);
		bSourceMeshSet = true;
	}
	if (Params->TryGetStringField(TEXT("target_preview_mesh"), TargetMeshPath) && !TargetMeshPath.IsEmpty())
	{
		USkeletalMesh* TargetMesh = FMonolithAssetUtils::LoadAssetByPath<USkeletalMesh>(TargetMeshPath);
		if (!TargetMesh)
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Target preview mesh not found: %s"), *TargetMeshPath));
		}
		C->SetPreviewMesh(ERetargetSourceOrTarget::Target, TargetMesh);
		bTargetMeshSet = true;
	}

	GEditor->EndTransaction();
	Retargeter->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("source_rig"), SourceRig->GetPathName());
	Root->SetStringField(TEXT("target_rig"), TargetRig->GetPathName());
	Root->SetBoolField(TEXT("source_preview_mesh_set"), bSourceMeshSet);
	Root->SetBoolField(TEXT("target_preview_mesh_set"), bTargetMeshSet);
	Root->SetNumberField(TEXT("retarget_op_count"), OpCount);
	Root->SetBoolField(TEXT("default_ops_seeded"), OpCount > 0);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleBatchRetargetAnimations(const TSharedPtr<FJsonObject>& Params)
{
	FString RetargeterPath = Params->GetStringField(TEXT("retargeter_path"));
	FString OutputFolder = Params->GetStringField(TEXT("output_folder"));

	UIKRetargeter* Retargeter = FMonolithAssetUtils::LoadAssetByPath<UIKRetargeter>(RetargeterPath);
	if (!Retargeter) return FMonolithActionResult::Error(FString::Printf(TEXT("IKRetargeter not found: %s"), *RetargeterPath));

	UIKRetargeterController* C = UIKRetargeterController::GetController(Retargeter);
	if (!C) return FMonolithActionResult::Error(TEXT("Failed to get UIKRetargeterController"));

	// Self-heal: a retargeter created before the default-op seeding fix (or via raw
	// NewObject) has an EMPTY op stack and would bake frozen output. If the asset
	// has both IK rigs assigned but no ops, seed the default op stack + auto-map
	// chains here so RunRetarget actually transfers motion. No-op when ops already
	// exist (AddDefaultOps is idempotent), so configured retargeters are untouched.
	bool bOpsSeededOnRun = false;
	if (C->GetNumRetargetOps() == 0 &&
		C->GetIKRig(ERetargetSourceOrTarget::Source) != nullptr &&
		C->GetIKRig(ERetargetSourceOrTarget::Target) != nullptr)
	{
		Retargeter->Modify();
		SeedRetargeterDefaultOps(C, ParseAutoMapType(Params));
		Retargeter->MarkPackageDirty();
		bOpsSeededOnRun = true;
	}

	// Resolve source/target meshes — explicit param wins, else fall back to the
	// retargeter's configured preview meshes.
	USkeletalMesh* SourceMesh = nullptr;
	USkeletalMesh* TargetMesh = nullptr;
	FString SourceMeshPath, TargetMeshPath;
	if (Params->TryGetStringField(TEXT("source_mesh"), SourceMeshPath) && !SourceMeshPath.IsEmpty())
	{
		SourceMesh = FMonolithAssetUtils::LoadAssetByPath<USkeletalMesh>(SourceMeshPath);
		if (!SourceMesh) return FMonolithActionResult::Error(FString::Printf(TEXT("Source mesh not found: %s"), *SourceMeshPath));
	}
	else
	{
		SourceMesh = C->GetPreviewMesh(ERetargetSourceOrTarget::Source);
	}
	if (Params->TryGetStringField(TEXT("target_mesh"), TargetMeshPath) && !TargetMeshPath.IsEmpty())
	{
		TargetMesh = FMonolithAssetUtils::LoadAssetByPath<USkeletalMesh>(TargetMeshPath);
		if (!TargetMesh) return FMonolithActionResult::Error(FString::Printf(TEXT("Target mesh not found: %s"), *TargetMeshPath));
	}
	else
	{
		TargetMesh = C->GetPreviewMesh(ERetargetSourceOrTarget::Target);
	}

	if (!SourceMesh) return FMonolithActionResult::Error(TEXT("No source mesh — pass 'source_mesh' or set the retargeter's source preview mesh"));
	if (!TargetMesh) return FMonolithActionResult::Error(TEXT("No target mesh — pass 'target_mesh' or set the retargeter's target preview mesh"));
	if (SourceMesh == TargetMesh) return FMonolithActionResult::Error(TEXT("Source and target meshes must differ"));

	// Collect source animation assets.
	const TArray<TSharedPtr<FJsonValue>>* AnimsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("source_anims"), AnimsArray) || !AnimsArray || AnimsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("'source_anims' must be a non-empty array of animation asset paths"));
	}

	TArray<TWeakObjectPtr<UObject>> AssetsToRetarget;
	TArray<FString> ResolvedSources;
	TArray<FString> SkippedSources;
	for (const TSharedPtr<FJsonValue>& Val : *AnimsArray)
	{
		FString AnimPath;
		if (!Val.IsValid() || !Val->TryGetString(AnimPath) || AnimPath.IsEmpty()) continue;
		UAnimationAsset* Anim = FMonolithAssetUtils::LoadAssetByPath<UAnimationAsset>(AnimPath);
		if (!Anim)
		{
			SkippedSources.Add(AnimPath);
			continue;
		}
		AssetsToRetarget.Add(Anim);
		ResolvedSources.Add(Anim->GetPathName());
	}

	if (AssetsToRetarget.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No valid animation assets resolved from 'source_anims'"));
	}

	// Build the batch context. NameRule.FolderPath is the output folder.
	FIKRetargetBatchOperationContext Context;
	Context.AssetsToRetarget = AssetsToRetarget;
	Context.SourceMesh = SourceMesh;
	Context.TargetMesh = TargetMesh;
	Context.IKRetargetAsset = Retargeter;
	Context.bIncludeReferencedAssets = false;
	Context.bOverwriteExistingFiles = false;

	Params->TryGetStringField(TEXT("name_prefix"), Context.NameRule.Prefix);
	Params->TryGetStringField(TEXT("name_suffix"), Context.NameRule.Suffix);
	Params->TryGetStringField(TEXT("search"), Context.NameRule.ReplaceFrom);
	Params->TryGetStringField(TEXT("replace"), Context.NameRule.ReplaceTo);
	Context.NameRule.FolderPath = OutputFolder;

	bool bIncludeRef = false;
	if (Params->TryGetBoolField(TEXT("include_referenced"), bIncludeRef)) Context.bIncludeReferencedAssets = bIncludeRef;
	bool bOverwrite = false;
	if (Params->TryGetBoolField(TEXT("overwrite"), bOverwrite)) Context.bOverwriteExistingFiles = bOverwrite;

	if (!Context.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Batch context invalid — check source/target meshes and retargeter"));
	}

	// Compute the expected output asset names BEFORE the run so we can resolve the
	// created assets (UIKRetargetBatchOperation::GetNewAssets is private).
	TArray<FString> ExpectedPaths;
	const FString NormalizedFolder = OutputFolder.EndsWith(TEXT("/")) ? OutputFolder.LeftChop(1) : OutputFolder;
	for (const TWeakObjectPtr<UObject>& WeakAsset : AssetsToRetarget)
	{
		if (UObject* Asset = WeakAsset.Get())
		{
			const FString NewName = Context.NameRule.Rename(Asset);
			ExpectedPaths.Add(FString::Printf(TEXT("%s/%s.%s"), *NormalizedFolder, *NewName, *NewName));
		}
	}

	// Run the synchronous batch retarget (uses FScopedSlowTask internally).
	UIKRetargetBatchOperation* BatchOp = NewObject<UIKRetargetBatchOperation>(GetTransientPackage());
	BatchOp->RunRetarget(Context);

	// Verify created assets by loading the expected output paths.
	TArray<FString> CreatedPaths;
	for (const FString& Expected : ExpectedPaths)
	{
		// Expected is "/Game/Folder/Name.Name"; LoadAssetByPath accepts the long path.
		const FString PackagePath = Expected.Contains(TEXT(".")) ? Expected.Left(Expected.Find(TEXT("."))) : Expected;
		if (UObject* Created = FMonolithAssetUtils::LoadAssetByPath<UObject>(PackagePath))
		{
			CreatedPaths.Add(Created->GetPathName());
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("retargeter_path"), RetargeterPath);
	Root->SetStringField(TEXT("output_folder"), OutputFolder);
	Root->SetStringField(TEXT("source_mesh"), SourceMesh->GetPathName());
	Root->SetStringField(TEXT("target_mesh"), TargetMesh->GetPathName());
	Root->SetNumberField(TEXT("requested_count"), AssetsToRetarget.Num());
	Root->SetNumberField(TEXT("created_count"), CreatedPaths.Num());
	Root->SetBoolField(TEXT("ops_seeded_on_run"), bOpsSeededOnRun);
	Root->SetNumberField(TEXT("retarget_op_count"), C->GetNumRetargetOps());

	TArray<TSharedPtr<FJsonValue>> CreatedArr;
	for (const FString& P : CreatedPaths) CreatedArr.Add(MakeShared<FJsonValueString>(P));
	Root->SetArrayField(TEXT("created_assets"), CreatedArr);

	TArray<TSharedPtr<FJsonValue>> SkippedArr;
	for (const FString& P : SkippedSources) SkippedArr.Add(MakeShared<FJsonValueString>(P));
	Root->SetArrayField(TEXT("skipped_sources"), SkippedArr);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// copy_bone_pose_between_sequences — read evaluated pose from source anim,
// write as keys to dest anim. Resolves ref pose for bones that have no
// explicit track in the source (common when an FBX was imported with sparse
// keys). Useful for fixing partial T-pose / wrong arm pose on a target anim
// without touching its other bones.
// ---------------------------------------------------------------------------
FMonolithActionResult FMonolithAnimationActions::HandleCopyBonePoseBetweenSequences(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath = Params->GetStringField(TEXT("source_path"));
	FString DestPath = Params->GetStringField(TEXT("dest_path"));

	double SourceTime = 0.0;
	Params->TryGetNumberField(TEXT("source_time"), SourceTime);

	bool bApplyToAllFrames = true;
	Params->TryGetBoolField(TEXT("apply_to_all_dest_frames"), bApplyToAllFrames);

	const TArray<TSharedPtr<FJsonValue>>* BoneNamesArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("bone_names"), BoneNamesArr) || !BoneNamesArr || BoneNamesArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required field: bone_names"));
	}

	UAnimSequence* SourceSeq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(SourcePath);
	if (!SourceSeq) return FMonolithActionResult::Error(FString::Printf(TEXT("Source AnimSequence not found: %s"), *SourcePath));

	UAnimSequence* DestSeq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(DestPath);
	if (!DestSeq) return FMonolithActionResult::Error(FString::Printf(TEXT("Dest AnimSequence not found: %s"), *DestPath));

	// Clamp SourceTime to the source sequence's playable range. Out-of-range
	// values (negative, or beyond GetPlayLength()) produce undefined sampling
	// in UAnimSequence::GetBoneTransform — clamp-and-report keeps callers
	// productive without surprising silent extrapolation.
	const double OriginalSourceTime = SourceTime;
	const double SourcePlayLength = static_cast<double>(SourceSeq->GetPlayLength());
	SourceTime = FMath::Clamp(SourceTime, 0.0, SourcePlayLength);
	const bool bSourceTimeClamped = !FMath::IsNearlyEqual(SourceTime, OriginalSourceTime);
	if (bSourceTimeClamped)
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("copy_bone_pose_between_sequences: source_time clamped from %f to %f (play_length=%f)"),
			OriginalSourceTime, SourceTime, SourcePlayLength);
	}

	USkeleton* SourceSkel = SourceSeq->GetSkeleton();
	USkeleton* DestSkel = DestSeq->GetSkeleton();
	if (!SourceSkel || !DestSkel) return FMonolithActionResult::Error(TEXT("Source or destination has no skeleton assigned"));

	const IAnimationDataModel* DestDataModel = DestSeq->GetDataModel();
	if (!DestDataModel) return FMonolithActionResult::Error(TEXT("Destination has no animation data model"));

	// NumberOfFrames + 1 = number of keys (0..NumberOfFrames inclusive)
	const int32 DestNumKeys = FMath::Max(1, DestDataModel->GetNumberOfFrames() + 1);
	const int32 KeysToWrite = bApplyToAllFrames ? DestNumKeys : 1;

	IAnimationDataController& Controller = DestSeq->GetController();
	Controller.OpenBracket(FText::FromString(TEXT("Copy Bone Pose Between Sequences")), false);

	TArray<FString> CopiedBones;
	TArray<TSharedPtr<FJsonValue>> SkippedJson;

	const FReferenceSkeleton& SourceRefSkel = SourceSkel->GetReferenceSkeleton();
	const FReferenceSkeleton& DestRefSkel = DestSkel->GetReferenceSkeleton();

	for (int32 Idx = 0; Idx < BoneNamesArr->Num(); ++Idx)
	{
		const TSharedPtr<FJsonValue>& Val = (*BoneNamesArr)[Idx];
		// Element-type guard: FJsonValue::AsString() silently returns "" for
		// non-string values (numbers, objects, null, bools), which would
		// otherwise be skipped without surfacing the bad input to the caller.
		if (!Val.IsValid() || Val->Type != EJson::String)
		{
			Controller.CloseBracket(false);
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("bone_names[%d] is not a string"), Idx),
				-32602);
		}
		const FString BoneNameStr = Val->AsString();
		const FName BoneName(*BoneNameStr);

		const int32 SourceBoneIdx = SourceRefSkel.FindBoneIndex(BoneName);
		const int32 DestBoneIdx = DestRefSkel.FindBoneIndex(BoneName);

		if (SourceBoneIdx == INDEX_NONE || DestBoneIdx == INDEX_NONE)
		{
			TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
			Skip->SetStringField(TEXT("bone_name"), BoneNameStr);
			Skip->SetStringField(TEXT("reason"),
				SourceBoneIdx == INDEX_NONE ? TEXT("not in source skeleton") : TEXT("not in dest skeleton"));
			SkippedJson.Add(MakeShared<FJsonValueObject>(Skip));
			continue;
		}

		// Evaluate source bone transform at SourceTime. UAnimSequence::GetBoneTransform
		// uses the raw track if present and falls back to the skeleton's ref pose
		// if the bone has no track — which is exactly what we want.
		FTransform BoneXform = FTransform::Identity;
		SourceSeq->GetBoneTransform(BoneXform, FSkeletonPoseBoneIndex(SourceBoneIdx),
		                            FAnimExtractContext(SourceTime), /*bUseRawData=*/true);

		// Build per-frame arrays for dest. For a static pose, all frames share
		// the same value; otherwise only frame 0 is set.
		TArray<FVector> Positions;
		TArray<FQuat> Rotations;
		TArray<FVector> Scales;
		Positions.SetNum(KeysToWrite);
		Rotations.SetNum(KeysToWrite);
		Scales.SetNum(KeysToWrite);
		const FVector P = BoneXform.GetLocation();
		const FQuat R = BoneXform.GetRotation();
		const FVector S = BoneXform.GetScale3D();
		for (int32 i = 0; i < KeysToWrite; ++i)
		{
			Positions[i] = P;
			Rotations[i] = R;
			Scales[i] = S;
		}

		// AddBoneCurve is idempotent — safe even if track exists. Then overwrite keys.
		Controller.AddBoneCurve(BoneName, false);
		Controller.SetBoneTrackKeys(BoneName, Positions, Rotations, Scales, false);

		CopiedBones.Add(BoneNameStr);
	}

	Controller.CloseBracket(false);
	DestSeq->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("source_path"), SourcePath);
	Root->SetStringField(TEXT("dest_path"), DestPath);
	Root->SetNumberField(TEXT("source_time"), SourceTime);
	if (bSourceTimeClamped)
	{
		Root->SetNumberField(TEXT("original_source_time"), OriginalSourceTime);
		Root->SetNumberField(TEXT("clamped_source_time"), SourceTime);
	}
	Root->SetBoolField(TEXT("apply_to_all_dest_frames"), bApplyToAllFrames);
	Root->SetNumberField(TEXT("keys_written_per_bone"), KeysToWrite);

	TArray<TSharedPtr<FJsonValue>> CopiedJson;
	for (const FString& B : CopiedBones)
	{
		CopiedJson.Add(MakeShared<FJsonValueString>(B));
	}
	Root->SetArrayField(TEXT("copied_bones"), CopiedJson);
	Root->SetArrayField(TEXT("skipped_bones"), SkippedJson);

	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  Anim-node bindings — function (Gap 2) + pin property (Gap 12)
//
//  Function bindings: three public FMemberReference UPROPERTYs on
//  UAnimGraphNode_Base. The setter mirrors UAnimGraphNode_Base::ValidateFunctionRef
//  (AnimGraphNode_Base.cpp:259) — resolve UFunction, prototype-signature check,
//  thread-safe gate — BEFORE writing the member, then recompile.
//
//  Pin bindings: reflective read/write of the unlinkable
//  UAnimGraphNodeBinding_Base::PropertyBindings map. The setter mirrors the
//  engine's own binding-widget write (AnimGraphNodeBinding_Base.cpp:486-503):
//  build FAnimGraphNodePropertyBinding, add to the map, then ReconstructNode().
// ============================================================

namespace MonolithAnimNodeBindingHelpers
{
	// Resolve a UAnimGraphNode_Base in an AnimBP by node_id (matched against the
	// node's GetName() OR its NodeGuid string — the serializer emits both forms).
	// Walks every graph (anim graph + function graphs + sub-graphs) unless a
	// graph_name filter is supplied.
	static UAnimGraphNode_Base* FindAnimNode(UAnimBlueprint* ABP, const FString& NodeId, const FString& GraphFilter)
	{
		if (!ABP || NodeId.IsEmpty()) return nullptr;

		TArray<UEdGraph*> AllGraphs;
		ABP->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			if (!GraphFilter.IsEmpty() && Graph->GetName() != GraphFilter) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
				if (!AnimNode) continue;
				if (AnimNode->GetName() == NodeId || AnimNode->NodeGuid.ToString() == NodeId)
				{
					return AnimNode;
				}
			}
		}
		return nullptr;
	}

	// Map the binding param value -> the matching FMemberReference UPROPERTY on the
	// node, plus the property name (needed to read the PrototypeFunction metadata).
	static FMemberReference* ResolveFunctionRef(UAnimGraphNode_Base* AnimNode, const FString& Binding, FName& OutPropertyName)
	{
		if (Binding == TEXT("initial_update"))
		{
			OutPropertyName = GET_MEMBER_NAME_CHECKED(UAnimGraphNode_Base, InitialUpdateFunction);
			return &AnimNode->InitialUpdateFunction;
		}
		if (Binding == TEXT("become_relevant"))
		{
			OutPropertyName = GET_MEMBER_NAME_CHECKED(UAnimGraphNode_Base, BecomeRelevantFunction);
			return &AnimNode->BecomeRelevantFunction;
		}
		if (Binding == TEXT("update"))
		{
			OutPropertyName = GET_MEMBER_NAME_CHECKED(UAnimGraphNode_Base, UpdateFunction);
			return &AnimNode->UpdateFunction;
		}
		OutPropertyName = NAME_None;
		return nullptr;
	}
}

FMonolithActionResult FMonolithAnimationActions::HandleGetAnimNodeFunctionBindings(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString NodeId;
	Params->TryGetStringField(TEXT("node_id"), NodeId);
	FString GraphFilter;
	Params->TryGetStringField(TEXT("graph_name"), GraphFilter);

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	// Resolve the owning class for the thread_safe flag (skeleton class first, fall
	// back to generated class) — mirrors the engine validator's resolution target.
	UClass* OwnerClass = ABP->SkeletonGeneratedClass ? ABP->SkeletonGeneratedClass : ABP->GeneratedClass;

	auto EmitNode = [OwnerClass](UAnimGraphNode_Base* AnimNode, UEdGraph* Graph) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("node_id"), AnimNode->GetName());
		NodeObj->SetStringField(TEXT("node_guid"), AnimNode->NodeGuid.ToString());
		NodeObj->SetStringField(TEXT("class"), AnimNode->GetClass()->GetName());
		NodeObj->SetStringField(TEXT("title"), AnimNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		if (Graph) NodeObj->SetStringField(TEXT("graph"), Graph->GetName());
		NodeObj->SetObjectField(TEXT("initial_update"),
			MonolithAnimNodeBindingReader::SerializeFunctionBinding(AnimNode->InitialUpdateFunction, OwnerClass));
		NodeObj->SetObjectField(TEXT("become_relevant"),
			MonolithAnimNodeBindingReader::SerializeFunctionBinding(AnimNode->BecomeRelevantFunction, OwnerClass));
		NodeObj->SetObjectField(TEXT("update"),
			MonolithAnimNodeBindingReader::SerializeFunctionBinding(AnimNode->UpdateFunction, OwnerClass));
		return NodeObj;
	};

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	TArray<TSharedPtr<FJsonValue>> NodesArr;

	if (!NodeId.IsEmpty())
	{
		// Single node — locate it (re-walk graphs to also recover its owning graph).
		UAnimGraphNode_Base* AnimNode = MonolithAnimNodeBindingHelpers::FindAnimNode(ABP, NodeId, GraphFilter);
		if (!AnimNode)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Anim node not found: %s"), *NodeId));
		}
		NodesArr.Add(MakeShared<FJsonValueObject>(EmitNode(AnimNode, AnimNode->GetGraph())));
	}
	else
	{
		// All nodes that carry ANY non-empty function binding.
		TArray<UEdGraph*> AllGraphs;
		ABP->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			if (!GraphFilter.IsEmpty() && Graph->GetName() != GraphFilter) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
				if (!AnimNode) continue;
				if (!MonolithAnimNodeBindingReader::HasAnyFunctionBinding(AnimNode)) continue;
				NodesArr.Add(MakeShared<FJsonValueObject>(EmitNode(AnimNode, Graph)));
			}
		}
	}

	Root->SetArrayField(TEXT("nodes"), NodesArr);
	Root->SetNumberField(TEXT("count"), NodesArr.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetAnimNodeFunctionBinding(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString NodeId = Params->GetStringField(TEXT("node_id"));
	FString Binding = Params->GetStringField(TEXT("binding"));
	FString FunctionName;
	Params->TryGetStringField(TEXT("function_name"), FunctionName);
	FString FunctionClassPath;
	Params->TryGetStringField(TEXT("function_class"), FunctionClassPath);
	FString GraphFilter;
	Params->TryGetStringField(TEXT("graph_name"), GraphFilter);
	bool bRecompile = true;
	Params->TryGetBoolField(TEXT("recompile"), bRecompile);
	bool bAllowNonThreadSafe = false;
	Params->TryGetBoolField(TEXT("allow_non_thread_safe"), bAllowNonThreadSafe);

	if (Binding != TEXT("initial_update") && Binding != TEXT("become_relevant") && Binding != TEXT("update"))
	{
		return FMonolithActionResult::Error(TEXT("binding must be one of: initial_update, become_relevant, update"));
	}

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	UAnimGraphNode_Base* AnimNode = MonolithAnimNodeBindingHelpers::FindAnimNode(ABP, NodeId, GraphFilter);
	if (!AnimNode) return FMonolithActionResult::Error(FString::Printf(TEXT("Anim node not found: %s"), *NodeId));

	FName PropertyName = NAME_None;
	FMemberReference* Ref = MonolithAnimNodeBindingHelpers::ResolveFunctionRef(AnimNode, Binding, PropertyName);
	if (!Ref) return FMonolithActionResult::Error(TEXT("Could not resolve the function-binding member reference"));

	UClass* OwnerClass = ABP->SkeletonGeneratedClass ? ABP->SkeletonGeneratedClass : ABP->GeneratedClass;

	const bool bClearing = FunctionName.IsEmpty();

	ABP->Modify();

	if (bClearing)
	{
		// Reset to default (empty) — the validator treats GetMemberName()==NAME_None
		// as "no binding". SetSelfMember(NAME_None) restores that state.
		Ref->SetSelfMember(NAME_None);
	}
	else
	{
		// Resolve the target UFunction. Default: self-member on the AnimBP class.
		// Optional function_class: an external library class.
		UClass* FunctionClass = nullptr;
		if (!FunctionClassPath.IsEmpty())
		{
			FunctionClass = FindObject<UClass>(nullptr, *FunctionClassPath);
			if (!FunctionClass)
			{
				FunctionClass = LoadObject<UClass>(nullptr, *FunctionClassPath);
			}
			if (!FunctionClass)
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("function_class not found: %s"), *FunctionClassPath));
			}
		}

		UClass* ResolveClass = FunctionClass ? FunctionClass : OwnerClass;
		UFunction* Function = ResolveClass ? ResolveClass->FindFunctionByName(FName(*FunctionName)) : nullptr;
		if (!Function)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Function '%s' not found on %s"), *FunctionName,
				ResolveClass ? *ResolveClass->GetName() : TEXT("<null class>")));
		}

		// --- Validate-before-commit, mirroring UAnimGraphNode_Base::ValidateFunctionRef ---
		// Prototype signature: read the PrototypeFunction metadata off the node property
		// and require IsSignatureCompatibleWith (same as the engine validator).
		if (const FProperty* Property = AnimNode->GetClass()->FindPropertyByName(PropertyName))
		{
			const FString& PrototypeFunctionName = Property->GetMetaData(TEXT("PrototypeFunction"));
			const UFunction* PrototypeFunction = PrototypeFunctionName.IsEmpty()
				? nullptr : FindObject<UFunction>(nullptr, *PrototypeFunctionName);
			if (PrototypeFunction && !PrototypeFunction->IsSignatureCompatibleWith(Function))
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Function '%s' signature is not compatible with the anim-update prototype (%s)"),
					*FunctionName, *PrototypeFunctionName));
			}
		}

		// Thread-safe gate: HARD REJECT a non-thread-safe function unless explicitly
		// allowed. Binding a non-thread-safe function to a worker-thread anim-update
		// slot silently corrupts evaluation — the engine validator errors here too.
		const bool bThreadSafe = FBlueprintEditorUtils::HasFunctionBlueprintThreadSafeMetaData(Function);
		if (!bThreadSafe && !bAllowNonThreadSafe)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Function '%s' is not thread-safe; refusing to bind it to an anim-update slot. ")
				TEXT("Mark it BlueprintThreadSafe or pass allow_non_thread_safe=true to override."),
				*FunctionName));
		}

		// Commit the member reference.
		if (FunctionClass)
		{
			Ref->SetExternalMember(FName(*FunctionName), FunctionClass);
		}
		else
		{
			Ref->SetSelfMember(FName(*FunctionName));
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(ABP);

	// Queue an extension refresh so the next compile regenerates the anim subsystem
	// set for the changed binding. Become-relevant / initial-update functions depend on
	// FAnimSubsystemInstance_NodeRelevancy; without this the subsystem is omitted from the
	// generated class and a bound dispatcher hits a null subsystem at runtime. Covers both
	// the bind and clear branches (clearing may drop the last node needing the extension).
	ABP->RequestRefreshExtensions();

	bool bCompiled = false;
	if (bRecompile)
	{
		FKismetEditorUtilities::CompileBlueprint(ABP);
		bCompiled = true;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("node_id"), AnimNode->GetName());
	Root->SetStringField(TEXT("binding"), Binding);
	Root->SetBoolField(TEXT("cleared"), bClearing);
	if (!bClearing) Root->SetStringField(TEXT("function_name"), FunctionName);
	Root->SetBoolField(TEXT("recompiled"), bCompiled);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleGetAnimNodePinBindings(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString NodeId;
	Params->TryGetStringField(TEXT("node_id"), NodeId);
	FString GraphFilter;
	Params->TryGetStringField(TEXT("graph_name"), GraphFilter);

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	auto EmitNode = [](UAnimGraphNode_Base* AnimNode, UEdGraph* Graph) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("node_id"), AnimNode->GetName());
		NodeObj->SetStringField(TEXT("node_guid"), AnimNode->NodeGuid.ToString());
		NodeObj->SetStringField(TEXT("class"), AnimNode->GetClass()->GetName());
		NodeObj->SetStringField(TEXT("title"), AnimNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		if (Graph) NodeObj->SetStringField(TEXT("graph"), Graph->GetName());

		TArray<TSharedPtr<FJsonValue>> Entries;
		FString Note;
		MonolithAnimNodeBindingReader::ReadPinBindings(AnimNode, Entries, Note);

		// --- T3-1: ALSO emit WIRE-LINKED input pins (graph-edge bindings). ---
		// The reader above surfaces only PROPERTY/FUNCTION bindings (the node's
		// PropertyBindings map). A pin can instead be driven by a graph wire from an
		// upstream node's output pin; those carry no PropertyBindings entry. For each
		// INPUT pin with at least one link, emit a { pin, type:"Link", source_node_id,
		// source_pin } entry alongside the property-bound entries. Property-bound and
		// wire-linked are mutually exclusive per pin in practice (a bound pin is hidden),
		// so this is additive — existing property-bound output is untouched.
		int32 LinkCount = 0;
		for (UEdGraphPin* Pin : AnimNode->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || Pin->LinkedTo.Num() == 0)
			{
				continue;
			}
			for (UEdGraphPin* SourcePin : Pin->LinkedTo)
			{
				if (!SourcePin) continue;
				UEdGraphNode* SourceNode = SourcePin->GetOwningNodeUnchecked();

				TSharedPtr<FJsonObject> LinkEntry = MakeShared<FJsonObject>();
				LinkEntry->SetStringField(TEXT("pin"), Pin->PinName.ToString());
				LinkEntry->SetStringField(TEXT("type"), TEXT("Link"));
				LinkEntry->SetStringField(TEXT("source_node_id"), SourceNode ? SourceNode->GetName() : FString());
				LinkEntry->SetStringField(TEXT("source_pin"), SourcePin->PinName.ToString());
				Entries.Add(MakeShared<FJsonValueObject>(LinkEntry));
				++LinkCount;
			}
		}

		NodeObj->SetArrayField(TEXT("pin_bindings"), Entries);
		NodeObj->SetNumberField(TEXT("linked_pin_count"), LinkCount);
		if (!Note.IsEmpty()) NodeObj->SetStringField(TEXT("note"), Note);
		return NodeObj;
	};

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	TArray<TSharedPtr<FJsonValue>> NodesArr;

	if (!NodeId.IsEmpty())
	{
		UAnimGraphNode_Base* AnimNode = MonolithAnimNodeBindingHelpers::FindAnimNode(ABP, NodeId, GraphFilter);
		if (!AnimNode) return FMonolithActionResult::Error(FString::Printf(TEXT("Anim node not found: %s"), *NodeId));
		NodesArr.Add(MakeShared<FJsonValueObject>(EmitNode(AnimNode, AnimNode->GetGraph())));
	}
	else
	{
		// All nodes that carry any pin binding.
		TArray<UEdGraph*> AllGraphs;
		ABP->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			if (!GraphFilter.IsEmpty() && Graph->GetName() != GraphFilter) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
				if (!AnimNode) continue;
				TArray<TSharedPtr<FJsonValue>> Probe;
				FString ProbeNote;
				const bool bHasPropertyBinding =
					MonolithAnimNodeBindingReader::ReadPinBindings(AnimNode, Probe, ProbeNote) > 0;

				// T3-1: also include nodes that carry ONLY wire-linked input pins (no
				// property binding). The reader probe above misses those; check for any
				// linked input pin so the all-nodes sweep surfaces graph-edge bindings too.
				bool bHasLinkedInput = false;
				if (!bHasPropertyBinding)
				{
					for (UEdGraphPin* Pin : AnimNode->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0)
						{
							bHasLinkedInput = true;
							break;
						}
					}
				}

				if (bHasPropertyBinding || bHasLinkedInput)
				{
					NodesArr.Add(MakeShared<FJsonValueObject>(EmitNode(AnimNode, Graph)));
				}
			}
		}
	}

	Root->SetArrayField(TEXT("nodes"), NodesArr);
	Root->SetNumberField(TEXT("count"), NodesArr.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationActions::HandleSetAnimNodePinBinding(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString NodeId = Params->GetStringField(TEXT("node_id"));
	FString Pin = Params->GetStringField(TEXT("pin"));
	FString GraphFilter;
	Params->TryGetStringField(TEXT("graph_name"), GraphFilter);
	bool bRecompile = true;
	Params->TryGetBoolField(TEXT("recompile"), bRecompile);

	// path: string array (empty/null clears by removing the pin's map entry).
	TArray<FString> Path;
	const TArray<TSharedPtr<FJsonValue>>* PathArr = nullptr;
	if (Params->TryGetArrayField(TEXT("path"), PathArr) && PathArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *PathArr)
		{
			FString Seg;
			if (V.IsValid() && V->TryGetString(Seg)) Path.Add(Seg);
		}
	}
	const bool bClearing = (Path.Num() == 0);

	if (Pin.IsEmpty()) return FMonolithActionResult::Error(TEXT("pin (PropertyName) is required"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	UAnimGraphNode_Base* AnimNode = MonolithAnimNodeBindingHelpers::FindAnimNode(ABP, NodeId, GraphFilter);
	if (!AnimNode) return FMonolithActionResult::Error(FString::Printf(TEXT("Anim node not found: %s"), *NodeId));

	const FName PinName(*Pin);
	ABP->Modify();

	// The node's binding object is type UAnimGraphNodeBinding, which is only
	// forward-declared in reachable headers (defining header in AnimGraph/Internal/).
	// We MUST treat it as a plain UObject* end-to-end — never GetBinding()/
	// GetMutableBinding() (incomplete return type) and never RemoveBindings() (a virtual
	// on the incomplete class). Both clear and write run entirely through FScriptMapHelper
	// on the reflectively-resolved PropertyBindings map.
	FMapProperty* MapProp = nullptr;
	void* MapPtr = nullptr;
	UObject* BindingObj = nullptr;
	bool bHasMap = MonolithAnimNodeBindingReader::ResolvePropertyBindingsMap(AnimNode, MapProp, MapPtr, BindingObj);

	if (bClearing)
	{
		// Remove the pin's map entry (this is what UAnimGraphNodeBinding_Base::RemoveBindings
		// does internally). If the node has no binding object / map, there is nothing to
		// clear — treat as a benign no-op rather than an error.
		if (bHasMap)
		{
			if (BindingObj) BindingObj->Modify();
			FScriptMapHelper Helper(MapProp, MapPtr);
			Helper.RemovePair(&PinName); // find-by-key + remove; no-op if absent
		}
	}
	else
	{
		// Zero-bootstrap: if the node has no binding object yet, create it ourselves,
		// mirroring the engine's lazy-create. UAnimGraphNode_Base::EnsureBindingsArePresent()
		// is PROTECTED (AnimGraphNode_Base.h:658, under the protected: at :579), so it is
		// NOT callable from this external module. Instead we replicate its body
		// (AnimGraphNode_Base.cpp:200-215): NewObject the binding from the ABP's default
		// binding class, falling back to AnimGraphNodeBinding_Base when the default is null,
		// and write it onto the node's `Binding` UPROPERTY reflectively (the concrete
		// UAnimGraphNodeBinding_Base type lives in AnimGraph/Internal and is not reachable
		// here, so we resolve its UClass by path and treat the binding as a plain UObject*).
		// We then RE-RESOLVE the PropertyBindings map (now present) and proceed with the same
		// AddPair + ReconstructNode path used when a binding already existed.
		if (!bHasMap)
		{
			// 1. ABP's configured default binding class (public inline, AnimBlueprint.h:245).
			UClass* BindingClass = ABP->GetDefaultBindingClass();
			// 2. Engine fallback when the default is null (AnimGraphNode_Base.cpp:207-210).
			//    UAnimGraphNodeBinding_Base is not includable from this module; resolve its
			//    UClass by script path (the AllowedClasses meta at AnimBlueprint.h:285).
			if (!BindingClass)
			{
				BindingClass = FindObject<UClass>(nullptr, TEXT("/Script/AnimGraph.AnimGraphNodeBinding_Base"));
			}
			if (!BindingClass)
			{
				return FMonolithActionResult::Error(
					TEXT("Failed to resolve a binding class for this node (the ABP has no default ")
					TEXT("binding class and AnimGraphNodeBinding_Base could not be found)."));
			}

			// 3. Create the binding subobject outered to the node (mirrors AnimGraphNode_Base.cpp:212).
			UObject* NewBindingObj = NewObject<UObject>(AnimNode, BindingClass);

			// 4. Write it onto the node's `Binding` FObjectProperty reflectively. This is the
			//    same property the reader reads at MonolithAnimNodeBindingReader.h:139-141.
			//    SetObjectPropertyValue_InContainer is public (CoreUObject UnrealType.h:2851).
			FObjectProperty* BindProp = FindFProperty<FObjectProperty>(AnimNode->GetClass(), TEXT("Binding"));
			if (!BindProp)
			{
				return FMonolithActionResult::Error(
					TEXT("Node has no reflectable 'Binding' property (layout drift); cannot create a binding."));
			}
			BindProp->SetObjectPropertyValue_InContainer(AnimNode, NewBindingObj);

			// 5. Re-resolve the now-present PropertyBindings map.
			bHasMap = MonolithAnimNodeBindingReader::ResolvePropertyBindingsMap(AnimNode, MapProp, MapPtr, BindingObj);
			if (!bHasMap)
			{
				return FMonolithActionResult::Error(
					TEXT("Failed to create a binding object on this node (no PropertyBindings map after ")
					TEXT("bootstrap). The node may lack a UAnimBlueprint outer."));
			}
		}

		FStructProperty* ValueStructProp = CastField<FStructProperty>(MapProp->ValueProp);
		if (!ValueStructProp || ValueStructProp->Struct != FAnimGraphNodePropertyBinding::StaticStruct())
		{
			return FMonolithActionResult::Error(TEXT("PropertyBindings value type is not FAnimGraphNodePropertyBinding"));
		}
		FNameProperty* KeyProp = CastField<FNameProperty>(MapProp->KeyProp);
		if (!KeyProp)
		{
			return FMonolithActionResult::Error(TEXT("PropertyBindings key type is not FName"));
		}

		// Build the binding value. Mirrors AnimGraphNodeBinding_Base.cpp:486-499.
		// NOTE: PinType/PromotedPinType (derived from the property path by
		// UAnimGraphNode_Base::RecalculateBindingType) are not set here; we trigger
		// re-derivation via ReconstructNode() below — see the re-derive comment.
		// (RecalculateBindingType is PUBLIC + UE_API in 5.7, AnimGraphNode_Base.h:649,
		// but driving it through ReconstructNode keeps a single re-derive path.)
		// PathAsText is a display field; set it to the dotted path so the binding reads
		// cleanly even before the reconstruct refreshes it.
		FAnimGraphNodePropertyBinding NewBinding;
		NewBinding.PropertyName = PinName;
		NewBinding.PropertyPath = Path;
		NewBinding.PathAsText = FText::FromString(FString::Join(Path, TEXT(".")));
		NewBinding.Type = EAnimGraphNodePropertyBindingType::Property;
		NewBinding.bIsBound = true;

		if (BindingObj) BindingObj->Modify();

		// AddPair overwrites an existing entry for the same key (engine PropertyBindings.Add
		// semantics) and rehashes internally — no manual pre-remove / Rehash needed.
		FScriptMapHelper Helper(MapProp, MapPtr);
		Helper.AddPair(&PinName, &NewBinding);
	}

	// --- Re-derive trigger (the plan's CRITICAL OPEN DETAIL) ---
	// Verified in engine source: the binding-type re-derivation lives in
	// UAnimGraphNode_Base::RecalculateBindingType (PUBLIC + UE_API in 5.7,
	// AnimGraphNode_Base.h:649 — it is reachable, but we drive it through the engine's
	// own re-derive path rather than calling it directly). It is invoked by
	// UAnimGraphNodeBinding_Base::OnReconstructNode, which loops over every entry in
	// PropertyBindings (AnimGraphNodeBinding_Base.cpp:80-90). That override fires from the
	// engine's own write path via AnimGraphNode->ReconstructNode()
	// (AnimGraphNodeBinding_Base.cpp:503). UEdGraphNode::ReconstructNode() is PUBLIC
	// (EdGraphNode.h:711), so we call it directly to re-derive PinType/PromotedPinType
	// before compiling. CompileBlueprint alone is NOT relied upon for the re-derive.
	AnimNode->ReconstructNode();
	FBlueprintEditorUtils::MarkBlueprintAsModified(ABP);

	bool bCompiled = false;
	if (bRecompile)
	{
		FKismetEditorUtilities::CompileBlueprint(ABP);
		bCompiled = true;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("node_id"), AnimNode->GetName());
	Root->SetStringField(TEXT("pin"), Pin);
	Root->SetBoolField(TEXT("cleared"), bClearing);
	if (!bClearing)
	{
		TArray<TSharedPtr<FJsonValue>> PathOut;
		for (const FString& Seg : Path) PathOut.Add(MakeShared<FJsonValueString>(Seg));
		Root->SetArrayField(TEXT("path"), PathOut);
	}
	Root->SetBoolField(TEXT("recompiled"), bCompiled);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// derive_foot_sync_markers — 5-signal foot-plant cascade
// ---------------------------------------------------------------------------
//
// Auto-derives left/right foot-plant sync markers from data already in a clip.
// A robustness/availability cascade picks the first signal that yields plants:
//   1. existing  — markers already named left/right_marker_name (ground truth)
//   2. notifies  — footstep notifies, foot side from TRACK NAME (pattern match)
//   3. contact   — contact_l/_r float curves, rising-edge + hysteresis + debounce
//   4. phase     — single Phase sawtooth curve, key extrema (+1=L, -1=R)
//   5. footspeed — component-space foot-bone speed minima (native port of the
//                  engine UFootstepAnimEventsModifier FootBoneSpeed technique)
//
// All five signals funnel into one output path so `source`/`confidence` are
// consistent. Marker write reuses the proven HandleAddSyncMarker envelope
// (push FAnimSyncMarker into AuthoredSyncMarkers + RefreshSyncMarkerDataFromAuthored).
//
// File-local helpers below are deliberately namespace-scoped (Monolith::FootSync)
// to avoid file-local-symbol collisions under full-unity release builds.
namespace Monolith { namespace FootSync {

// Tunable thresholds + sample config, populated from the `thresholds` param.
struct FFootSyncConfig
{
	float ContactMid       = 0.5f;  // rising-edge crossing threshold (contact curve)
	float ContactLow       = 0.1f;  // hysteresis re-arm threshold (contact curve)
	float SpeedThreshold   = 0.1f;  // normalized speed-valley threshold (footspeed)
	float SampleRate       = 60.0f; // Hz, contact-curve resample + footspeed pose eval
	float DebounceFraction = 0.5f;  // x estimated stride period; collapses heel-toe double-bump
	float GroundThreshold  = 4.0f;  // cm above GroundLevel — footspeed air-phase tiebreaker
};

// Read a float curve from the data model by name (non-deprecated FName identifier).
static const FFloatCurve* FindFloatCurveByName(const IAnimationDataModel* DataModel, const FName& CurveName)
{
	if (!DataModel || CurveName.IsNone())
	{
		return nullptr;
	}
	const FAnimationCurveIdentifier CurveId(CurveName, ERawCurveTrackTypes::RCT_Float);
	return DataModel->FindFloatCurve(CurveId);
}

// Rising-edge plant detection on a contact curve, with hysteresis re-arm and a
// stride-period debounce. Mirrors research §3a: take the FIRST (heel-strike)
// edge, suppress further detections until the curve drops below ContactLow,
// and additionally suppress for DebounceFraction x estimated stride period.
static void DetectContactPlants(const FFloatCurve& Curve, float PlayLength,
	const FFootSyncConfig& Cfg, TArray<float>& OutTimes)
{
	OutTimes.Reset();
	if (PlayLength <= 0.0f || Cfg.SampleRate <= 0.0f)
	{
		return;
	}

	const float Step = 1.0f / Cfg.SampleRate;
	const int32 NumSamples = FMath::Max(2, FMath::TruncToInt(PlayLength / Step) + 1);

	// Pass 1: raw rising edges (mid-threshold upward crossing) with hysteresis.
	TArray<float> RawEdges;
	bool bArmed = true; // armed = allowed to detect a new rising edge
	float PrevVal = Curve.Evaluate(0.0f);
	for (int32 i = 1; i < NumSamples; ++i)
	{
		const float T = FMath::Min(static_cast<float>(i) * Step, PlayLength);
		const float Val = Curve.Evaluate(T);

		if (bArmed && PrevVal < Cfg.ContactMid && Val >= Cfg.ContactMid)
		{
			// Linear-interpolate the exact crossing time for sub-sample accuracy.
			const float Denom = (Val - PrevVal);
			const float Frac = FMath::IsNearlyZero(Denom) ? 0.0f : (Cfg.ContactMid - PrevVal) / Denom;
			const float PrevT = FMath::Min(static_cast<float>(i - 1) * Step, PlayLength);
			RawEdges.Add(PrevT + Frac * (T - PrevT));
			bArmed = false; // re-arm only after dropping below ContactLow
		}
		else if (!bArmed && Val < Cfg.ContactLow)
		{
			bArmed = true;
		}
		PrevVal = Val;
	}

	if (RawEdges.Num() == 0)
	{
		return;
	}

	// Estimate stride period for the debounce window: mean inter-edge spacing,
	// falling back to PlayLength / edge count when only one edge exists.
	float StridePeriod;
	if (RawEdges.Num() >= 2)
	{
		float Sum = 0.0f;
		for (int32 i = 1; i < RawEdges.Num(); ++i)
		{
			Sum += (RawEdges[i] - RawEdges[i - 1]);
		}
		StridePeriod = Sum / static_cast<float>(RawEdges.Num() - 1);
	}
	else
	{
		StridePeriod = PlayLength / FMath::Max(1, RawEdges.Num());
	}
	const float Refractory = FMath::Max(0.0f, Cfg.DebounceFraction) * StridePeriod;

	// Pass 2: refractory debounce collapses heel-toe double-bumps to one plant.
	float LastAccepted = -FLT_MAX;
	for (float Edge : RawEdges)
	{
		if (Edge - LastAccepted >= Refractory)
		{
			OutTimes.Add(Edge);
			LastAccepted = Edge;
		}
	}
}

// Phase-curve plant extraction: read authored key extrema directly (keys sit on
// extrema per research §3b). +1 keys -> left plants, -1 keys -> right (phase_invert swaps).
// If the curve is a 0..1 ramp instead of a -1..+1 sawtooth, fall back to derivative
// sign-change at period boundaries (lower confidence — flagged by caller).
static void DetectPhasePlants(const FFloatCurve& Curve, bool bInvert,
	TArray<float>& OutLeft, TArray<float>& OutRight, bool& bOutHeuristic)
{
	OutLeft.Reset();
	OutRight.Reset();
	bOutHeuristic = false;

	const TArray<FRichCurveKey>& Keys = Curve.FloatCurve.GetConstRefOfKeys();
	if (Keys.Num() < 2)
	{
		return;
	}

	// Range probe: a true -1..+1 sawtooth has min ~ -1 and max ~ +1.
	float MinV = Keys[0].Value, MaxV = Keys[0].Value;
	for (const FRichCurveKey& K : Keys)
	{
		MinV = FMath::Min(MinV, K.Value);
		MaxV = FMath::Max(MaxV, K.Value);
	}

	const bool bBipolar = (MinV < -0.5f && MaxV > 0.5f);
	if (bBipolar)
	{
		// Local extrema -> plants, INTERIOR keys only. The first/last authored keys
		// are deliberately skipped: on a looping clip the trailing key sits on the
		// loop-wrap (e.g. a +1 at t ~= PlayLength that is really the NEXT cycle's
		// plant), and the leading key is the same plant's loop-boundary continuity
		// key (often at a negative time). Treating a boundary key as its own
		// neighbour would always qualify it as an extremum and double-count the
		// wrap plant (the t=4.0265 bug). The genuine plant for each stride is the
		// interior extremum; the loop-wrap copy is filtered by both this interior
		// restriction and the caller's global [0, PlayLength) guard.
		for (int32 i = 1; i < Keys.Num() - 1; ++i)
		{
			const float Prev = Keys[i - 1].Value;
			const float Next = Keys[i + 1].Value;
			const float Cur = Keys[i].Value;
			const bool bLocalMax = (Cur >= Prev && Cur >= Next) && Cur > 0.5f;
			const bool bLocalMin = (Cur <= Prev && Cur <= Next) && Cur < -0.5f;
			if (bLocalMax)
			{
				(bInvert ? OutRight : OutLeft).Add(Keys[i].Time);
			}
			else if (bLocalMin)
			{
				(bInvert ? OutLeft : OutRight).Add(Keys[i].Time);
			}
		}
	}
	else
	{
		// 0..1 ramp convention: detect period boundaries via derivative sign change.
		// Each downward discontinuity (ramp reset) is a stride boundary; alternate L/R.
		bOutHeuristic = true;
		bool bLeftTurn = !bInvert;
		for (int32 i = 1; i < Keys.Num(); ++i)
		{
			if (Keys[i].Value < Keys[i - 1].Value - 0.25f) // ramp wrapped down
			{
				(bLeftTurn ? OutLeft : OutRight).Add(Keys[i].Time);
				bLeftTurn = !bLeftTurn;
			}
		}
	}
}

// Component-space foot-bone speed minima (signal 5). Native port of
// UFootstepAnimEventsModifier::OnApply_Implementation FootBoneSpeed path
// (FootstepAnimEventsModifier.cpp:24-195, ComputeBoneSpeed .cpp:316-325,
// CanWePlaceEventAtSample .cpp:305-314). Evaluates the pose ONCE per sample via a
// single GetAnimPoseAtTimeIntervals call over the full time array (review fix #2:
// no per-call bone-container re-init from GetAnimPoseAtTime twice per sample).
static void DetectFootSpeedPlants(const UAnimSequence* Seq, const FName& FootBone,
	const FFootSyncConfig& Cfg, TArray<float>& OutTimes)
{
	OutTimes.Reset();
	if (!Seq || FootBone.IsNone() || Cfg.SampleRate <= 0.0f)
	{
		return;
	}

	const float PlayLength = Seq->GetPlayLength();
	if (PlayLength <= 0.0f)
	{
		return; // static pose / single frame -> no plants
	}

	const float Step = 1.0f / Cfg.SampleRate;
	const int32 NumSamples = FMath::TruncToInt(PlayLength / Step);
	if (NumSamples < 3)
	{
		return;
	}

	// Build the full time array, then evaluate ALL poses in one call.
	// Each pose i is at time i*Step; speed at sample i uses pose i and pose i+1.
	TArray<double> Times;
	Times.Reserve(NumSamples + 1);
	for (int32 i = 0; i <= NumSamples; ++i)
	{
		Times.Add(static_cast<double>(FMath::Clamp(static_cast<float>(i) * Step, 0.0f, PlayLength)));
	}

	// Match the engine modifier's eval options: Raw data, root motion incorporated
	// so the foot's world trajectory includes locomotion. Field order mirrors the
	// FAnimPoseEvaluationOptions definition (AnimPose.h): EvaluationType,
	// bShouldRetarget, bExtractRootMotion, bIncorporateRootMotionIntoPose,
	// OptionalSkeletalMesh, bRetrieveAdditiveAsFullPose, bEvaluateCurves.
	FAnimPoseEvaluationOptions EvalOptions;
	EvalOptions.EvaluationType = EAnimDataEvalType::Raw;
	EvalOptions.bShouldRetarget = true;
	EvalOptions.bExtractRootMotion = false;
	EvalOptions.bIncorporateRootMotionIntoPose = true;
	EvalOptions.OptionalSkeletalMesh = nullptr;
	EvalOptions.bRetrieveAdditiveAsFullPose = true;
	EvalOptions.bEvaluateCurves = false; // we only need bone transforms

	TArray<FAnimPose> Poses;
	UAnimPoseExtensions::GetAnimPoseAtTimeIntervals(Seq, Times, EvalOptions, Poses);
	if (Poses.Num() < NumSamples + 1)
	{
		return; // eval failed or produced too few poses
	}

	// Pass 1: per-clip min/max foot speed + ground level.
	TArray<float> Speeds;
	Speeds.SetNumZeroed(NumSamples);
	float MinSpeed = FLT_MAX, MaxSpeed = 0.0f, GroundLevel = FLT_MAX;
	for (int32 i = 0; i < NumSamples; ++i)
	{
		const FTransform& Cur = UAnimPoseExtensions::GetBonePose(Poses[i], FootBone, EAnimPoseSpaces::World);
		const FTransform& Next = UAnimPoseExtensions::GetBonePose(Poses[i + 1], FootBone, EAnimPoseSpaces::World);
		const double Dist = (Next.GetLocation() - Cur.GetLocation()).Length();
		const float Speed = static_cast<float>(Dist / Step);
		Speeds[i] = Speed;
		MinSpeed = FMath::Min(MinSpeed, Speed);
		MaxSpeed = FMath::Max(MaxSpeed, Speed);
		GroundLevel = FMath::Min(GroundLevel, static_cast<float>(Cur.GetLocation().Z));
	}

	const float SpeedRange = MaxSpeed - MinSpeed;
	if (FMath::IsNearlyZero(SpeedRange))
	{
		return; // no motion variation -> static pose, no plants
	}

	// Pass 2: normalized-speed valley detection (engine FootBoneSpeed path).
	// Track the time of the smallest below-threshold speed; place a plant on the
	// UPWARD crossing back through threshold, back-dated to that valley time.
	float PrevNorm = (Speeds[0] - MinSpeed) / SpeedRange;
	float TimeAtMin = FLT_MAX;
	float MinBelow = FLT_MAX;
	for (int32 i = 1; i < NumSamples; ++i)
	{
		const float Norm = (Speeds[i] - MinSpeed) / SpeedRange;
		const float SampleTime = FMath::Min(static_cast<float>(i) * Step, PlayLength);

		if (Norm < Cfg.SpeedThreshold && i > 1)
		{
			if (Norm < MinBelow && FMath::Abs(MinBelow - Norm) >= 0.01f)
			{
				TimeAtMin = SampleTime;
				MinBelow = Norm;
			}
		}

		// Upward crossing back through threshold = end of stance -> emit valley.
		const bool bUpwardCross = (PrevNorm < Cfg.SpeedThreshold && Norm >= Cfg.SpeedThreshold);
		if (bUpwardCross && i > 1 && TimeAtMin < FLT_MAX)
		{
			// Ground-height tiebreaker (secondary cue): reject obvious air-phase
			// valleys when the foot is far above the lowest observed Z. Height is
			// fragile on slopes, so speed stays primary — accept regardless, but
			// keep the probe for future tuning / diagnostics.
			const int32 ValleyIdx = FMath::Clamp(FMath::RoundToInt(TimeAtMin / Step), 0, NumSamples - 1);
			const FTransform& ValleyXf = UAnimPoseExtensions::GetBonePose(Poses[ValleyIdx], FootBone, EAnimPoseSpaces::World);
			const float FootZ = static_cast<float>(ValleyXf.GetLocation().Z);
			(void)FootZ; (void)GroundLevel; // tiebreaker reserved; speed is authoritative
			OutTimes.Add(TimeAtMin);
			TimeAtMin = FLT_MAX;
			MinBelow = FLT_MAX;
		}
		PrevNorm = Norm;
	}
}

// T2-2: planar-speed-minima + ground-height plant detection (from_bones mode).
// Distinct from DetectFootSpeedPlants (signal 5), which uses full 3D speed valleys.
// from_bones isolates the GROUND CONTACT specifically by requiring BOTH (a) a planar
// (XY) speed minimum below threshold AND (b) the foot's vertical (Z) position within
// GroundThreshold of the lowest observed Z for that foot. This rejects the swing-apex
// pause (low 3D speed but airborne) that pure-speed detection can mis-fire on.
// Mirrors the DetectFootSpeedPlants eval setup (single GetAnimPoseAtTimeIntervals call,
// World-space bone transforms, root motion folded in).
static void DetectBonePlants(const UAnimSequence* Seq, const FName& FootBone,
	const FFootSyncConfig& Cfg, TArray<float>& OutTimes)
{
	OutTimes.Reset();
	if (!Seq || FootBone.IsNone() || Cfg.SampleRate <= 0.0f)
	{
		return;
	}

	const float PlayLength = Seq->GetPlayLength();
	if (PlayLength <= 0.0f)
	{
		return;
	}

	const float Step = 1.0f / Cfg.SampleRate;
	const int32 NumSamples = FMath::TruncToInt(PlayLength / Step);
	if (NumSamples < 3)
	{
		return;
	}

	TArray<double> Times;
	Times.Reserve(NumSamples + 1);
	for (int32 i = 0; i <= NumSamples; ++i)
	{
		Times.Add(static_cast<double>(FMath::Clamp(static_cast<float>(i) * Step, 0.0f, PlayLength)));
	}

	FAnimPoseEvaluationOptions EvalOptions;
	EvalOptions.EvaluationType = EAnimDataEvalType::Raw;
	EvalOptions.bShouldRetarget = true;
	EvalOptions.bExtractRootMotion = false;
	EvalOptions.bIncorporateRootMotionIntoPose = true;
	EvalOptions.OptionalSkeletalMesh = nullptr;
	EvalOptions.bRetrieveAdditiveAsFullPose = true;
	EvalOptions.bEvaluateCurves = false;

	TArray<FAnimPose> Poses;
	UAnimPoseExtensions::GetAnimPoseAtTimeIntervals(Seq, Times, EvalOptions, Poses);
	if (Poses.Num() < NumSamples + 1)
	{
		return;
	}

	// Pass 1: per-sample PLANAR speed (XY only) + per-sample Z; track ground level.
	TArray<float> PlanarSpeeds;
	TArray<float> FootZ;
	PlanarSpeeds.SetNumZeroed(NumSamples);
	FootZ.SetNumZeroed(NumSamples);
	float MinSpeed = FLT_MAX, MaxSpeed = 0.0f, GroundLevel = FLT_MAX;
	for (int32 i = 0; i < NumSamples; ++i)
	{
		const FTransform& Cur = UAnimPoseExtensions::GetBonePose(Poses[i], FootBone, EAnimPoseSpaces::World);
		const FTransform& Next = UAnimPoseExtensions::GetBonePose(Poses[i + 1], FootBone, EAnimPoseSpaces::World);
		const FVector CurLoc = Cur.GetLocation();
		const FVector NextLoc = Next.GetLocation();
		const double PlanarDist = FVector2D(NextLoc.X - CurLoc.X, NextLoc.Y - CurLoc.Y).Size();
		const float Speed = static_cast<float>(PlanarDist / Step);
		PlanarSpeeds[i] = Speed;
		FootZ[i] = static_cast<float>(CurLoc.Z);
		MinSpeed = FMath::Min(MinSpeed, Speed);
		MaxSpeed = FMath::Max(MaxSpeed, Speed);
		GroundLevel = FMath::Min(GroundLevel, static_cast<float>(CurLoc.Z));
	}

	const float SpeedRange = MaxSpeed - MinSpeed;
	if (FMath::IsNearlyZero(SpeedRange))
	{
		return; // no planar motion -> no plants
	}

	// Pass 2: planar-speed valley detection gated by ground proximity. Place a plant on
	// the upward crossing back through threshold, back-dated to the in-stance valley time,
	// only if the foot was within GroundThreshold of GroundLevel at the valley.
	float PrevNorm = (PlanarSpeeds[0] - MinSpeed) / SpeedRange;
	float TimeAtMin = FLT_MAX;
	float MinBelow = FLT_MAX;
	int32 ValleyIdx = INDEX_NONE;
	for (int32 i = 1; i < NumSamples; ++i)
	{
		const float Norm = (PlanarSpeeds[i] - MinSpeed) / SpeedRange;
		const float SampleTime = FMath::Min(static_cast<float>(i) * Step, PlayLength);

		if (Norm < Cfg.SpeedThreshold && i > 1)
		{
			if (Norm < MinBelow)
			{
				TimeAtMin = SampleTime;
				MinBelow = Norm;
				ValleyIdx = i;
			}
		}

		const bool bUpwardCross = (PrevNorm < Cfg.SpeedThreshold && Norm >= Cfg.SpeedThreshold);
		if (bUpwardCross && i > 1 && TimeAtMin < FLT_MAX && ValleyIdx != INDEX_NONE)
		{
			// Ground-height gate: the foot must be near the floor at the valley to count
			// as a plant (rejects airborne swing-apex pauses).
			const bool bNearGround = (FootZ[ValleyIdx] - GroundLevel) <= Cfg.GroundThreshold;
			if (bNearGround)
			{
				OutTimes.Add(TimeAtMin);
			}
			TimeAtMin = FLT_MAX;
			MinBelow = FLT_MAX;
			ValleyIdx = INDEX_NONE;
		}
		PrevNorm = Norm;
	}
}

// Auto-resolve a foot bone for one side against the skeleton's bone list.
// Returns NAME_None if none of the candidates is present.
static FName ResolveFootBone(const FReferenceSkeleton& RefSkel, const TArray<FName>& Candidates)
{
	for (const FName& Cand : Candidates)
	{
		if (RefSkel.FindBoneIndex(Cand) != INDEX_NONE)
		{
			return Cand;
		}
	}
	return NAME_None;
}

}} // namespace Monolith::FootSync

FMonolithActionResult FMonolithAnimationActions::HandleDeriveFootSyncMarkers(const TSharedPtr<FJsonObject>& Params)
{
	using namespace Monolith::FootSync;

	// --- Parse params ---
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	FString LeftMarkerName = TEXT("L_Foot");
	FString RightMarkerName = TEXT("R_Foot");
	Params->TryGetStringField(TEXT("left_marker_name"), LeftMarkerName);
	Params->TryGetStringField(TEXT("right_marker_name"), RightMarkerName);

	int32 TrackIndex = 0;
	{
		double TmpTrack;
		if (Params->TryGetNumberField(TEXT("track_index"), TmpTrack))
		{
			TrackIndex = static_cast<int32>(TmpTrack);
		}
	}

	FString Method = TEXT("auto");
	Params->TryGetStringField(TEXT("method"), Method);
	Method = Method.ToLower();
	if (Method != TEXT("auto") && Method != TEXT("existing") && Method != TEXT("notifies")
		&& Method != TEXT("contact") && Method != TEXT("phase") && Method != TEXT("footspeed")
		&& Method != TEXT("from_bones"))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid method '%s'. Expected one of: auto, existing, notifies, contact, phase, footspeed, from_bones."), *Method));
	}

	bool bPhaseInvert = false;
	Params->TryGetBoolField(TEXT("phase_invert"), bPhaseInvert);

	bool bClearExisting = true;
	Params->TryGetBoolField(TEXT("clear_existing"), bClearExisting);

	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	// Thresholds (per-signal defaults; each individually overridable).
	FFootSyncConfig Cfg;
	{
		const TSharedPtr<FJsonObject>* ThreshObj = nullptr;
		if (Params->TryGetObjectField(TEXT("thresholds"), ThreshObj) && ThreshObj)
		{
			double V;
			if ((*ThreshObj)->TryGetNumberField(TEXT("contact_mid"), V))       Cfg.ContactMid = static_cast<float>(V);
			if ((*ThreshObj)->TryGetNumberField(TEXT("contact_low"), V))       Cfg.ContactLow = static_cast<float>(V);
			if ((*ThreshObj)->TryGetNumberField(TEXT("speed_threshold"), V))   Cfg.SpeedThreshold = static_cast<float>(V);
			if ((*ThreshObj)->TryGetNumberField(TEXT("sample_rate"), V))       Cfg.SampleRate = static_cast<float>(V);
			if ((*ThreshObj)->TryGetNumberField(TEXT("debounce_fraction"), V)) Cfg.DebounceFraction = static_cast<float>(V);
			if ((*ThreshObj)->TryGetNumberField(TEXT("ground_threshold"), V))  Cfg.GroundThreshold = static_cast<float>(V);
		}
	}
	// T2-2 (from_bones): convenience top-level aliases for the two levers this mode keys
	// on — speed_threshold (planar-speed valley) and ground_height_threshold (cm above the
	// lowest observed foot Z). They override the thresholds-object values when supplied, so
	// callers can pass them directly without nesting under `thresholds`.
	{
		double V;
		if (Params->TryGetNumberField(TEXT("speed_threshold"), V))        Cfg.SpeedThreshold = static_cast<float>(V);
		if (Params->TryGetNumberField(TEXT("ground_height_threshold"), V)) Cfg.GroundThreshold = static_cast<float>(V);
	}
	if (Cfg.SampleRate <= 0.0f)
	{
		return FMonolithActionResult::Error(TEXT("thresholds.sample_rate must be > 0"));
	}

	// Notify track patterns (side discrimination for signal 2).
	FString NotifyLeftPattern = TEXT("footstep left");
	FString NotifyRightPattern = TEXT("footstep right");
	{
		const TSharedPtr<FJsonObject>* PatObj = nullptr;
		if (Params->TryGetObjectField(TEXT("notify_track_patterns"), PatObj) && PatObj)
		{
			(*PatObj)->TryGetStringField(TEXT("left"), NotifyLeftPattern);
			(*PatObj)->TryGetStringField(TEXT("right"), NotifyRightPattern);
		}
	}

	// Explicit foot bones (signal 5). Empty => auto-resolve later.
	FString ExplicitLeftBone, ExplicitRightBone;
	{
		const TSharedPtr<FJsonObject>* BonesObj = nullptr;
		if (Params->TryGetObjectField(TEXT("foot_bones"), BonesObj) && BonesObj)
		{
			(*BonesObj)->TryGetStringField(TEXT("left"), ExplicitLeftBone);
			(*BonesObj)->TryGetStringField(TEXT("right"), ExplicitRightBone);
		}
	}

	// --- Load asset ---
	UAnimSequence* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Seq)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("AnimSequence not found: %s"), *AssetPath));
	}

	const FName LeftFName(*LeftMarkerName);
	const FName RightFName(*RightMarkerName);

	const float PlayLength = Seq->GetPlayLength();
	const IAnimationDataModel* DataModel = Seq->GetDataModel();

	// Result accumulators (filled by the winning signal).
	TArray<float> LeftTimes;
	TArray<float> RightTimes;
	FString Source;
	FString Confidence;
	TArray<TSharedPtr<FJsonValue>> Notes;
	FName UsedLeftBone = NAME_None;
	FName UsedRightBone = NAME_None;

	const bool bForce = (Method != TEXT("auto"));
	auto WantSignal = [&](const TCHAR* Name) -> bool
	{
		return !bForce || Method == Name;
	};

	// ---- Signal 1: existing markers ----
	if (WantSignal(TEXT("existing")) && LeftTimes.Num() == 0 && RightTimes.Num() == 0)
	{
		for (const FAnimSyncMarker& M : Seq->AuthoredSyncMarkers)
		{
			if (M.MarkerName == LeftFName)  LeftTimes.Add(M.Time);
			if (M.MarkerName == RightFName) RightTimes.Add(M.Time);
		}
		if (LeftTimes.Num() > 0 || RightTimes.Num() > 0)
		{
			Source = TEXT("existing");
			Confidence = TEXT("ground_truth");
		}
		else if (Method == TEXT("existing"))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("method=existing: no markers named '%s'/'%s' on %s"), *LeftMarkerName, *RightMarkerName, *AssetPath));
		}
	}

	// ---- Signal 2: footstep notifies (foot from TRACK NAME) ----
	if (Source.IsEmpty() && WantSignal(TEXT("notifies")))
	{
		const FString LeftPatLower = NotifyLeftPattern.ToLower();
		const FString RightPatLower = NotifyRightPattern.ToLower();
		TArray<float> NotifyLeft, NotifyRight;
		TArray<TPair<float, FString>> UnclassifiedByTime; // for class-suffix / alternate fallback
		bool bUsedClassSuffix = false;
		bool bUsedAlternate = false;

		for (const FAnimNotifyEvent& Event : Seq->Notifies)
		{
			FString TrackName;
			if (Seq->AnimNotifyTracks.IsValidIndex(Event.TrackIndex))
			{
				TrackName = Seq->AnimNotifyTracks[Event.TrackIndex].TrackName.ToString();
			}
			const FString TrackLower = TrackName.ToLower();
			const float EventTime = Event.GetTime();

			if (!LeftPatLower.IsEmpty() && TrackLower.Contains(LeftPatLower))
			{
				NotifyLeft.Add(EventTime);
			}
			else if (!RightPatLower.IsEmpty() && TrackLower.Contains(RightPatLower))
			{
				NotifyRight.Add(EventTime);
			}
			else
			{
				// Fallback (a): class-name suffix token _L / _R.
				const FString ClassName = Event.Notify ? Event.Notify->GetClass()->GetName() : FString();
				const FString ClassLower = ClassName.ToLower();
				if (ClassLower.EndsWith(TEXT("_l")) || ClassLower.EndsWith(TEXT("_l_c")))
				{
					NotifyLeft.Add(EventTime);
					bUsedClassSuffix = true;
				}
				else if (ClassLower.EndsWith(TEXT("_r")) || ClassLower.EndsWith(TEXT("_r_c")))
				{
					NotifyRight.Add(EventTime);
					bUsedClassSuffix = true;
				}
				else
				{
					UnclassifiedByTime.Add(TPair<float, FString>(EventTime, ClassName));
				}
			}
		}

		// Fallback (b): alternate L/R by ascending time for whatever stayed unclassified.
		if (UnclassifiedByTime.Num() > 0 && (NotifyLeft.Num() + NotifyRight.Num()) == 0)
		{
			UnclassifiedByTime.Sort([](const TPair<float, FString>& A, const TPair<float, FString>& B)
			{
				return A.Key < B.Key;
			});
			bool bLeftTurn = true;
			for (const TPair<float, FString>& P : UnclassifiedByTime)
			{
				(bLeftTurn ? NotifyLeft : NotifyRight).Add(P.Key);
				bLeftTurn = !bLeftTurn;
			}
			bUsedAlternate = true;
		}

		if (NotifyLeft.Num() > 0 || NotifyRight.Num() > 0)
		{
			NotifyLeft.Sort();
			NotifyRight.Sort();
			LeftTimes = MoveTemp(NotifyLeft);
			RightTimes = MoveTemp(NotifyRight);
			Source = TEXT("notifies");
			Confidence = bUsedAlternate ? TEXT("heuristic") : (bUsedClassSuffix ? TEXT("high") : TEXT("very_high"));
			if (bUsedClassSuffix) Notes.Add(MakeShared<FJsonValueString>(TEXT("classified some notifies by class-name suffix")));
			if (bUsedAlternate)   Notes.Add(MakeShared<FJsonValueString>(TEXT("fell back to alternate-by-time side classification")));
		}
		else if (Method == TEXT("notifies"))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("method=notifies: no footstep notifies matched patterns '%s'/'%s' on %s"),
				*NotifyLeftPattern, *NotifyRightPattern, *AssetPath));
		}
	}

	// ---- Signal 3: contact_l / contact_r float curves ----
	if (Source.IsEmpty() && WantSignal(TEXT("contact")))
	{
		const FFloatCurve* ContactL = FindFloatCurveByName(DataModel, FName(TEXT("contact_l")));
		const FFloatCurve* ContactR = FindFloatCurveByName(DataModel, FName(TEXT("contact_r")));
		if (ContactL && ContactR)
		{
			TArray<float> CL, CR;
			DetectContactPlants(*ContactL, PlayLength, Cfg, CL);
			DetectContactPlants(*ContactR, PlayLength, Cfg, CR);
			if (CL.Num() > 0 || CR.Num() > 0)
			{
				LeftTimes = MoveTemp(CL);
				RightTimes = MoveTemp(CR);
				Source = TEXT("contact");
				Confidence = TEXT("high");
			}
		}
		if (Source.IsEmpty() && Method == TEXT("contact"))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("method=contact: contact_l/contact_r curves not present (or no plants) on %s"), *AssetPath));
		}
	}

	// ---- Signal 4: Phase curve ----
	if (Source.IsEmpty() && WantSignal(TEXT("phase")))
	{
		const FFloatCurve* PhaseCurve = FindFloatCurveByName(DataModel, FName(TEXT("Phase")));
		if (PhaseCurve)
		{
			TArray<float> PL, PR;
			bool bHeuristic = false;
			DetectPhasePlants(*PhaseCurve, bPhaseInvert, PL, PR, bHeuristic);
			// Out-of-range loop-boundary keys (negative-time continuity key, loop-wrap
			// key at/after PlayLength) are dropped by the global guard applied after the
			// cascade resolves — no phase-specific filter needed here.
			if (PL.Num() > 0 || PR.Num() > 0)
			{
				PL.Sort();
				PR.Sort();
				LeftTimes = MoveTemp(PL);
				RightTimes = MoveTemp(PR);
				Source = TEXT("phase");
				Confidence = bHeuristic ? TEXT("heuristic") : TEXT("high");
				if (bHeuristic) Notes.Add(MakeShared<FJsonValueString>(TEXT("Phase curve was a 0..1 ramp; used derivative-boundary heuristic")));
			}
		}
		if (Source.IsEmpty() && Method == TEXT("phase"))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("method=phase: Phase curve not present (or no extrema) on %s"), *AssetPath));
		}
	}

	// ---- T2-2: from_bones — planar-speed minima gated by ground height ----
	// Explicit-only mode (NOT part of the auto cascade): fires only when method=from_bones.
	// Resolves foot bones the same way footspeed does (explicit foot_bones override, else
	// common-name auto-resolve), then uses DetectBonePlants (planar XY speed valley +
	// ground-Z gate) and reports source="bones".
	if (Source.IsEmpty() && Method == TEXT("from_bones"))
	{
		USkeleton* Skeleton = Seq->GetSkeleton();
		if (!Skeleton)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("method=from_bones: no skeleton on %s"), *AssetPath));
		}

		const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();

		if (!ExplicitLeftBone.IsEmpty())
		{
			UsedLeftBone = (RefSkel.FindBoneIndex(FName(*ExplicitLeftBone)) != INDEX_NONE) ? FName(*ExplicitLeftBone) : NAME_None;
		}
		else
		{
			UsedLeftBone = ResolveFootBone(RefSkel,
				{ FName(TEXT("foot_l")), FName(TEXT("ball_l")), FName(TEXT("LeftFoot")), FName(TEXT("L_Foot")) });
		}
		if (!ExplicitRightBone.IsEmpty())
		{
			UsedRightBone = (RefSkel.FindBoneIndex(FName(*ExplicitRightBone)) != INDEX_NONE) ? FName(*ExplicitRightBone) : NAME_None;
		}
		else
		{
			UsedRightBone = ResolveFootBone(RefSkel,
				{ FName(TEXT("foot_r")), FName(TEXT("ball_r")), FName(TEXT("RightFoot")), FName(TEXT("R_Foot")) });
		}

		if (UsedLeftBone.IsNone() && UsedRightBone.IsNone())
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("method=from_bones: could not resolve any foot bone on %s (supply foot_bones)"), *AssetPath));
		}

		if (!UsedLeftBone.IsNone())  DetectBonePlants(Seq, UsedLeftBone, Cfg, LeftTimes);
		if (!UsedRightBone.IsNone()) DetectBonePlants(Seq, UsedRightBone, Cfg, RightTimes);

		Source = TEXT("bones");
		Confidence = (LeftTimes.Num() > 0 || RightTimes.Num() > 0) ? TEXT("high") : TEXT("heuristic");
		if (LeftTimes.Num() == 0 && RightTimes.Num() == 0)
		{
			Notes.Add(MakeShared<FJsonValueString>(TEXT("no ground-contact plants found (static pose, or thresholds too tight)")));
		}
	}

	// ---- Signal 5: component-space foot-bone speed minima ----
	if (Source.IsEmpty() && WantSignal(TEXT("footspeed")))
	{
		USkeleton* Skeleton = Seq->GetSkeleton();
		if (!Skeleton)
		{
			if (Method == TEXT("footspeed"))
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("method=footspeed: no skeleton on %s"), *AssetPath));
			}
		}
		else
		{
			const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();

			// Resolve foot bones: explicit override, else common-name auto-resolve.
			if (!ExplicitLeftBone.IsEmpty())
			{
				UsedLeftBone = (RefSkel.FindBoneIndex(FName(*ExplicitLeftBone)) != INDEX_NONE) ? FName(*ExplicitLeftBone) : NAME_None;
			}
			else
			{
				UsedLeftBone = ResolveFootBone(RefSkel,
					{ FName(TEXT("foot_l")), FName(TEXT("ball_l")), FName(TEXT("LeftFoot")), FName(TEXT("L_Foot")) });
			}
			if (!ExplicitRightBone.IsEmpty())
			{
				UsedRightBone = (RefSkel.FindBoneIndex(FName(*ExplicitRightBone)) != INDEX_NONE) ? FName(*ExplicitRightBone) : NAME_None;
			}
			else
			{
				UsedRightBone = ResolveFootBone(RefSkel,
					{ FName(TEXT("foot_r")), FName(TEXT("ball_r")), FName(TEXT("RightFoot")), FName(TEXT("R_Foot")) });
			}

			if (UsedLeftBone.IsNone() && UsedRightBone.IsNone())
			{
				if (Method == TEXT("footspeed"))
				{
					return FMonolithActionResult::Error(FString::Printf(
						TEXT("method=footspeed: could not resolve any foot bone on %s (supply foot_bones)"), *AssetPath));
				}
			}
			else
			{
				if (!UsedLeftBone.IsNone())  DetectFootSpeedPlants(Seq, UsedLeftBone, Cfg, LeftTimes);
				if (!UsedRightBone.IsNone()) DetectFootSpeedPlants(Seq, UsedRightBone, Cfg, RightTimes);

				// footspeed is the universal fallback: it claims the source even when it
				// yields zero plants (e.g. a static pose), so auto does not error out.
				Source = TEXT("footspeed");
				Confidence = (LeftTimes.Num() > 0 || RightTimes.Num() > 0) ? TEXT("high") : TEXT("heuristic");
				if (LeftTimes.Num() == 0 && RightTimes.Num() == 0)
				{
					Notes.Add(MakeShared<FJsonValueString>(TEXT("static pose, no plants")));
				}
			}
		}
	}

	// ---- No signal fired (auto exhausted) ----
	if (Source.IsEmpty())
	{
		// auto with nothing available — succeed empty with a note rather than error.
		Source = TEXT("none");
		Confidence = TEXT("none");
		Notes.Add(MakeShared<FJsonValueString>(TEXT("no foot-plant signal available in this clip")));
	}

	// ---- Global out-of-range guard (applies to EVERY signal) ----
	// A sync marker at/after PlayLength is invalid (it lands on the loop-wrap, e.g.
	// the phase detector's spurious extremum at t ~= PlayLength on a looping clip),
	// and a negative time is a loop-boundary continuity artefact. Drop both from
	// both arrays AFTER detection so no signal can emit an out-of-range marker.
	// Epsilon keeps a plant that sits exactly on the last frame from tripping the
	// >= bound due to float rounding, while still excluding the wrap copy.
	{
		const float UpperBound = PlayLength - 1e-4f;
		auto WithinRange = [UpperBound](float T) -> bool
		{
			return T >= 0.0f && T < UpperBound;
		};
		const int32 LeftBefore = LeftTimes.Num();
		const int32 RightBefore = RightTimes.Num();
		LeftTimes.RemoveAll([&](float T) { return !WithinRange(T); });
		RightTimes.RemoveAll([&](float T) { return !WithinRange(T); });
		const int32 DroppedOutOfRange = (LeftBefore - LeftTimes.Num()) + (RightBefore - RightTimes.Num());
		if (DroppedOutOfRange > 0)
		{
			Notes.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("dropped %d out-of-range marker(s) outside [0, %.4f)"), DroppedOutOfRange, PlayLength)));
		}
	}

	// ---- Idempotency clear (skip when source==existing — those markers ARE the answer) ----
	int32 ClearedExisting = 0;
	const bool bShouldClear = bClearExisting && !bDryRun && Source != TEXT("existing");
	const bool bWillWrite = !bDryRun && Source != TEXT("existing") && Source != TEXT("none")
		&& (LeftTimes.Num() > 0 || RightTimes.Num() > 0);

	if (bShouldClear || bWillWrite)
	{
		GEditor->BeginTransaction(FText::FromString(TEXT("Derive Foot Sync Markers")));
		Seq->Modify();

		if (bShouldClear)
		{
			TArray<FName> NamesToRemove;
			NamesToRemove.Add(LeftFName);
			if (RightFName != LeftFName) NamesToRemove.Add(RightFName);
			const int32 CountBefore = Seq->AuthoredSyncMarkers.Num();
			Seq->RemoveSyncMarkers(NamesToRemove);
			ClearedExisting = CountBefore - Seq->AuthoredSyncMarkers.Num();
		}

		// ---- Write markers (review fix #1: AuthoredSyncMarkers + single refresh) ----
		if (bWillWrite)
		{
			auto PushMarker = [&](const FName& Name, float Time)
			{
				FAnimSyncMarker NewMarker;
				NewMarker.MarkerName = Name;
				NewMarker.Time = Time;
#if WITH_EDITORONLY_DATA
				NewMarker.TrackIndex = TrackIndex;
				NewMarker.Guid = FGuid::NewGuid();
#endif
				Seq->AuthoredSyncMarkers.Add(NewMarker);
			};
			for (float T : LeftTimes)  PushMarker(LeftFName, T);
			for (float T : RightTimes) PushMarker(RightFName, T);
		}

		Seq->RefreshSyncMarkerDataFromAuthored();
		GEditor->EndTransaction();
		Seq->MarkPackageDirty();
	}

	const int32 MarkersWritten = bWillWrite ? (LeftTimes.Num() + RightTimes.Num()) : 0;

	// ---- Build result JSON ----
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("source"), Source);
	Root->SetStringField(TEXT("confidence"), Confidence);
	Root->SetBoolField(TEXT("dry_run"), bDryRun);
	Root->SetStringField(TEXT("left_marker_name"), LeftMarkerName);
	Root->SetStringField(TEXT("right_marker_name"), RightMarkerName);
	Root->SetNumberField(TEXT("track_index"), TrackIndex);
	Root->SetNumberField(TEXT("cleared_existing"), ClearedExisting);

	auto TimesToJson = [](const TArray<float>& Times) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("count"), Times.Num());
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (float T : Times) Arr.Add(MakeShared<FJsonValueNumber>(T));
		Obj->SetArrayField(TEXT("times"), Arr);
		return Obj;
	};
	Root->SetObjectField(TEXT("left"), TimesToJson(LeftTimes));
	Root->SetObjectField(TEXT("right"), TimesToJson(RightTimes));
	Root->SetNumberField(TEXT("markers_written"), MarkersWritten);

	if (!UsedLeftBone.IsNone() || !UsedRightBone.IsNone())
	{
		TSharedPtr<FJsonObject> BonesUsed = MakeShared<FJsonObject>();
		BonesUsed->SetStringField(TEXT("left"), UsedLeftBone.ToString());
		BonesUsed->SetStringField(TEXT("right"), UsedRightBone.ToString());
		Root->SetObjectField(TEXT("foot_bones_used"), BonesUsed);
	}

	Root->SetArrayField(TEXT("notes"), Notes);
	return FMonolithActionResult::Success(Root);
}
