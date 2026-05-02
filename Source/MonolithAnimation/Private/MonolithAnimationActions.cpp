#include "MonolithAnimationActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationAsset.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/Skeleton.h"
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
#include "Rig/IKRigDefinition.h"
#include "Rig/IKRigSkeleton.h"
#include "RigEditor/IKRigController.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "ControlRigBlueprintLegacy.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyElements.h"
#include "Rigs/RigHierarchyDefines.h"
#include "Rigs/RigHierarchyController.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Editor.h"
#include "AnimationStateMachineSchema.h"
#include "AnimGraphNode_TransitionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "K2Node_VariableGet.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/BodyInstance.h"

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
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Montage asset path"))
			.Required(TEXT("section_name"), TEXT("string"), TEXT("Name for the new section"))
			.Required(TEXT("start_time"), TEXT("number"), TEXT("Start time in seconds"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("delete_montage_section"),
		TEXT("Delete a section from an animation montage by index"),
		FMonolithActionHandler::CreateStatic(&HandleDeleteMontageSection),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Montage asset path"))
			.Required(TEXT("section_index"), TEXT("integer"), TEXT("Index of the section to delete"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_section_next"),
		TEXT("Set the next section for a montage section"),
		FMonolithActionHandler::CreateStatic(&HandleSetSectionNext),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Montage asset path"))
			.Required(TEXT("section_name"), TEXT("string"), TEXT("Name of the section"))
			.Required(TEXT("next_section_name"), TEXT("string"), TEXT("Name of the next section to play"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_section_time"),
		TEXT("Set the start time of a montage section"),
		FMonolithActionHandler::CreateStatic(&HandleSetSectionTime),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Montage asset path"))
			.Required(TEXT("section_name"), TEXT("string"), TEXT("Name of the section"))
			.Required(TEXT("new_time"), TEXT("number"), TEXT("New start time in seconds"))
			.Build());

	// BlendSpace Samples
	Registry.RegisterAction(TEXT("animation"), TEXT("add_blendspace_sample"),
		TEXT("Add a sample to a blend space"),
		FMonolithActionHandler::CreateStatic(&HandleAddBlendSpaceSample),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("BlendSpace asset path"))
			.Required(TEXT("anim_path"), TEXT("string"), TEXT("Animation sequence asset path"))
			.Required(TEXT("x"), TEXT("number"), TEXT("X axis value"))
			.Required(TEXT("y"), TEXT("number"), TEXT("Y axis value"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("edit_blendspace_sample"),
		TEXT("Edit a blend space sample position and optionally its animation"),
		FMonolithActionHandler::CreateStatic(&HandleEditBlendSpaceSample),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("BlendSpace asset path"))
			.Required(TEXT("sample_index"), TEXT("integer"), TEXT("Index of the sample to edit"))
			.Required(TEXT("x"), TEXT("number"), TEXT("New X axis value"))
			.Required(TEXT("y"), TEXT("number"), TEXT("New Y axis value"))
			.Optional(TEXT("anim_path"), TEXT("string"), TEXT("New animation sequence asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("delete_blendspace_sample"),
		TEXT("Delete a sample from a blend space by index"),
		FMonolithActionHandler::CreateStatic(&HandleDeleteBlendSpaceSample),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("BlendSpace asset path"))
			.Required(TEXT("sample_index"), TEXT("integer"), TEXT("Index of the sample to delete"))
			.Build());

	// ABP Graph Reading
	Registry.RegisterAction(TEXT("animation"), TEXT("get_state_machines"),
		TEXT("Get all state machines in an animation blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleGetStateMachines),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_state_info"),
		TEXT("Get detailed info about a state in a state machine"),
		FMonolithActionHandler::CreateStatic(&HandleGetStateInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("machine_name"), TEXT("string"), TEXT("State machine name"))
			.Required(TEXT("state_name"), TEXT("string"), TEXT("State name"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_transitions"),
		TEXT("Get all transitions in a state machine"),
		FMonolithActionHandler::CreateStatic(&HandleGetTransitions),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Optional(TEXT("machine_name"), TEXT("string"), TEXT("Filter to a specific state machine"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_blend_nodes"),
		TEXT("Get blend nodes in an animation blueprint graph"),
		FMonolithActionHandler::CreateStatic(&HandleGetBlendNodes),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Filter to a specific graph"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_linked_layers"),
		TEXT("Get linked animation layers in an animation blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleGetLinkedLayers),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_graphs"),
		TEXT("Get all graphs in an animation blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleGetGraphs),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_nodes"),
		TEXT("Get animation nodes with optional class filter"),
		FMonolithActionHandler::CreateStatic(&HandleGetNodes),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Optional(TEXT("node_class_filter"), TEXT("string"), TEXT("Only include nodes whose class contains this substring"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Filter to a specific graph"))
			.Build());

	// Notify Editing
	Registry.RegisterAction(TEXT("animation"), TEXT("set_notify_time"),
		TEXT("Set the trigger time of an animation notify"),
		FMonolithActionHandler::CreateStatic(&HandleSetNotifyTime),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation asset path"))
			.Required(TEXT("notify_index"), TEXT("integer"), TEXT("Index of the notify"))
			.Required(TEXT("new_time"), TEXT("number"), TEXT("New trigger time in seconds"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_notify_duration"),
		TEXT("Set the duration of a state animation notify"),
		FMonolithActionHandler::CreateStatic(&HandleSetNotifyDuration),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation asset path"))
			.Required(TEXT("notify_index"), TEXT("integer"), TEXT("Index of the notify"))
			.Required(TEXT("new_duration"), TEXT("number"), TEXT("New duration in seconds"))
			.Build());

	// Bone Tracks
	Registry.RegisterAction(TEXT("animation"), TEXT("set_bone_track_keys"),
		TEXT("Set position, rotation, and scale keys on a bone track"),
		FMonolithActionHandler::CreateStatic(&HandleSetBoneTrackKeys),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation sequence asset path"))
			.Required(TEXT("bone_name"), TEXT("string"), TEXT("Bone name"))
			.Required(TEXT("positions_json"), TEXT("string"), TEXT("JSON array of position keys"))
			.Required(TEXT("rotations_json"), TEXT("string"), TEXT("JSON array of rotation keys"))
			.Required(TEXT("scales_json"), TEXT("string"), TEXT("JSON array of scale keys"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_bone_track"),
		TEXT("Add a bone track to an animation sequence"),
		FMonolithActionHandler::CreateStatic(&HandleAddBoneTrack),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation sequence asset path"))
			.Required(TEXT("bone_name"), TEXT("string"), TEXT("Bone name to add"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_bone_track"),
		TEXT("Remove a bone track from an animation sequence"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveBoneTrack),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation sequence asset path"))
			.Required(TEXT("bone_name"), TEXT("string"), TEXT("Bone name to remove"))
			.Optional(TEXT("include_children"), TEXT("bool"), TEXT("Also remove child bone tracks"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("copy_bone_pose_between_sequences"),
		TEXT("Copy evaluated bone transforms (track + ref pose fallback) from a source AnimSequence at a given time to a destination AnimSequence as keys"),
		FMonolithActionHandler::CreateStatic(&HandleCopyBonePoseBetweenSequences),
		FParamSchemaBuilder()
			.Required(TEXT("source_path"), TEXT("string"), TEXT("Source AnimSequence asset path"))
			.Required(TEXT("dest_path"), TEXT("string"), TEXT("Destination AnimSequence asset path"))
			.Required(TEXT("bone_names"), TEXT("array"), TEXT("Array of bone names to copy"))
			.Optional(TEXT("source_time"), TEXT("number"), TEXT("Time in seconds on source to evaluate (default 0.0)"), TEXT("0.0"))
			.Optional(TEXT("apply_to_all_dest_frames"), TEXT("bool"), TEXT("If true, write same value to every destination frame (static pose). If false, write only frame 0."), TEXT("true"))
			.Build());

	// Virtual Bones
	Registry.RegisterAction(TEXT("animation"), TEXT("add_virtual_bone"),
		TEXT("Add a virtual bone to a skeleton"),
		FMonolithActionHandler::CreateStatic(&HandleAddVirtualBone),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Skeleton asset path"))
			.Required(TEXT("source_bone"), TEXT("string"), TEXT("Source bone name"))
			.Required(TEXT("target_bone"), TEXT("string"), TEXT("Target bone name"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_virtual_bones"),
		TEXT("Remove virtual bones from a skeleton"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveVirtualBones),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Skeleton asset path"))
			.Required(TEXT("bone_names"), TEXT("array"), TEXT("Array of virtual bone names to remove"))
			.Build());

	// Skeleton Info
	Registry.RegisterAction(TEXT("animation"), TEXT("get_skeleton_info"),
		TEXT("Get skeleton bone hierarchy and virtual bones"),
		FMonolithActionHandler::CreateStatic(&HandleGetSkeletonInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Skeleton asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_skeletal_mesh_info"),
		TEXT("Get skeletal mesh info including morph targets, sockets, LODs, and materials"),
		FMonolithActionHandler::CreateStatic(&HandleGetSkeletalMeshInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Skeletal mesh asset path"))
			.Build());

	// Wave 1 — Read Actions
	Registry.RegisterAction(TEXT("animation"), TEXT("get_sequence_info"),
		TEXT("Get animation sequence metadata (duration, frames, root motion, compression, etc.)"),
		FMonolithActionHandler::CreateStatic(&HandleGetSequenceInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_sequence_notifies"),
		TEXT("Get all notifies on an animation asset"),
		FMonolithActionHandler::CreateStatic(&HandleGetSequenceNotifies),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation asset path (sequence, montage, composite)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_bone_track_keys"),
		TEXT("Get position/rotation/scale keys for a bone track"),
		FMonolithActionHandler::CreateStatic(&HandleGetBoneTrackKeys),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Required(TEXT("bone_name"), TEXT("string"), TEXT("Bone name to read"))
			.Optional(TEXT("start_frame"), TEXT("integer"), TEXT("Start frame (default 0)"), TEXT("0"))
			.Optional(TEXT("end_frame"), TEXT("integer"), TEXT("End frame (default -1 = all)"), TEXT("-1"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_sequence_curves"),
		TEXT("Get float and transform curves on an animation sequence"),
		FMonolithActionHandler::CreateStatic(&HandleGetSequenceCurves),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_montage_info"),
		TEXT("Get montage metadata including sections, slots, blend settings"),
		FMonolithActionHandler::CreateStatic(&HandleGetMontageInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Montage asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_blend_space_info"),
		TEXT("Get blend space samples and axis settings"),
		FMonolithActionHandler::CreateStatic(&HandleGetBlendSpaceInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("BlendSpace asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_skeleton_sockets"),
		TEXT("Get sockets from a skeleton or skeletal mesh"),
		FMonolithActionHandler::CreateStatic(&HandleGetSkeletonSockets),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Skeleton or SkeletalMesh asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_abp_info"),
		TEXT("Get animation blueprint overview (skeleton, graphs, state machines, variables, interfaces)"),
		FMonolithActionHandler::CreateStatic(&HandleGetAbpInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Build());

	// Wave 2 — Notify CRUD
	Registry.RegisterAction(TEXT("animation"), TEXT("add_notify"),
		TEXT("Add a point notify to an animation asset"),
		FMonolithActionHandler::CreateStatic(&HandleAddNotify),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation asset path"))
			.Required(TEXT("notify_class"), TEXT("string"), TEXT("Notify class name (e.g. AnimNotify_PlaySound)"))
			.Required(TEXT("time"), TEXT("number"), TEXT("Trigger time in seconds"))
			.Optional(TEXT("track_name"), TEXT("string"), TEXT("Notify track name"), TEXT("1"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_notify_state"),
		TEXT("Add a state notify (with duration) to an animation asset"),
		FMonolithActionHandler::CreateStatic(&HandleAddNotifyState),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation asset path"))
			.Required(TEXT("notify_class"), TEXT("string"), TEXT("NotifyState class name (e.g. AnimNotifyState_Trail)"))
			.Required(TEXT("time"), TEXT("number"), TEXT("Start time in seconds"))
			.Required(TEXT("duration"), TEXT("number"), TEXT("Duration in seconds"))
			.Optional(TEXT("track_name"), TEXT("string"), TEXT("Notify track name"), TEXT("1"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_notify"),
		TEXT("Remove a notify by index from an animation asset"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveNotify),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation asset path"))
			.Required(TEXT("notify_index"), TEXT("integer"), TEXT("Index of notify to remove"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_notify_track"),
		TEXT("Move a notify to a different track"),
		FMonolithActionHandler::CreateStatic(&HandleSetNotifyTrack),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation asset path"))
			.Required(TEXT("notify_index"), TEXT("integer"), TEXT("Index of notify to move"))
			.Required(TEXT("track_index"), TEXT("integer"), TEXT("Target track index"))
			.Build());

	// Wave 3 — Curve CRUD
	Registry.RegisterAction(TEXT("animation"), TEXT("list_curves"),
		TEXT("List all animation curves on a sequence (float and transform)"),
		FMonolithActionHandler::CreateStatic(&HandleListCurves),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Optional(TEXT("include_keys"), TEXT("bool"), TEXT("Include key data in response"), TEXT("false"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_curve"),
		TEXT("Add a float or transform curve to an animation sequence"),
		FMonolithActionHandler::CreateStatic(&HandleAddCurve),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Required(TEXT("curve_name"), TEXT("string"), TEXT("Name for the new curve"))
			.Optional(TEXT("curve_type"), TEXT("string"), TEXT("Float or Transform (default Float)"), TEXT("Float"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_curve"),
		TEXT("Remove a curve from an animation sequence"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveCurve),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Required(TEXT("curve_name"), TEXT("string"), TEXT("Name of curve to remove"))
			.Optional(TEXT("curve_type"), TEXT("string"), TEXT("Float or Transform (default Float)"), TEXT("Float"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_curve_keys"),
		TEXT("Set keys on a float curve (replaces existing keys)"),
		FMonolithActionHandler::CreateStatic(&HandleSetCurveKeys),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Required(TEXT("curve_name"), TEXT("string"), TEXT("Curve name"))
			.Required(TEXT("keys_json"), TEXT("string"), TEXT("JSON array: [{\"time\":0.0,\"value\":1.0,\"interp\":\"cubic\"}, ...]"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_curve_keys"),
		TEXT("Get all keys from a float curve"),
		FMonolithActionHandler::CreateStatic(&HandleGetCurveKeys),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Required(TEXT("curve_name"), TEXT("string"), TEXT("Curve name"))
			.Build());

	// Wave 4 — Skeleton + BlendSpace
	Registry.RegisterAction(TEXT("animation"), TEXT("add_socket"),
		TEXT("Add a socket to a skeleton"),
		FMonolithActionHandler::CreateStatic(&HandleAddSocket),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Skeleton asset path"))
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
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Skeleton asset path"))
			.Required(TEXT("socket_name"), TEXT("string"), TEXT("Socket name to remove"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_socket_transform"),
		TEXT("Set the transform of a skeleton socket"),
		FMonolithActionHandler::CreateStatic(&HandleSetSocketTransform),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Skeleton asset path"))
			.Required(TEXT("socket_name"), TEXT("string"), TEXT("Socket name"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("[x, y, z] relative location"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("[pitch, yaw, roll] relative rotation"))
			.Optional(TEXT("scale"), TEXT("array"), TEXT("[x, y, z] relative scale"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_skeleton_curves"),
		TEXT("Get all registered animation curve names from a skeleton"),
		FMonolithActionHandler::CreateStatic(&HandleGetSkeletonCurves),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Skeleton asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_blend_space_axis"),
		TEXT("Configure a blend space axis (name, range, grid divisions)"),
		FMonolithActionHandler::CreateStatic(&HandleSetBlendSpaceAxis),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("BlendSpace asset path"))
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
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Optional(TEXT("enable_root_motion"), TEXT("bool"), TEXT("Enable/disable root motion"))
			.Optional(TEXT("root_motion_lock"), TEXT("string"), TEXT("AnimFirstFrame, Zero, or RefPose"))
			.Optional(TEXT("force_root_lock"), TEXT("bool"), TEXT("Force root lock even without root motion"))
			.Build());

	// Wave 5 — Creation + Montage
	Registry.RegisterAction(TEXT("animation"), TEXT("create_sequence"),
		TEXT("Create a new empty animation sequence"),
		FMonolithActionHandler::CreateStatic(&HandleCreateSequence),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path for the new sequence (e.g. /Game/Anims/MyAnim)"))
			.Required(TEXT("skeleton_path"), TEXT("string"), TEXT("Skeleton asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("duplicate_sequence"),
		TEXT("Duplicate an animation sequence to a new path"),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateSequence),
		FParamSchemaBuilder()
			.Required(TEXT("source_path"), TEXT("string"), TEXT("Source animation asset path"))
			.Required(TEXT("dest_path"), TEXT("string"), TEXT("Destination asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("create_montage"),
		TEXT("Create a new animation montage with skeleton"),
		FMonolithActionHandler::CreateStatic(&HandleCreateMontage),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path for the new montage"))
			.Required(TEXT("skeleton_path"), TEXT("string"), TEXT("Skeleton asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_montage_blend"),
		TEXT("Set blend in/out times and auto blend out on a montage"),
		FMonolithActionHandler::CreateStatic(&HandleSetMontageBlend),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Montage asset path"))
			.Optional(TEXT("blend_in_time"), TEXT("number"), TEXT("Blend in duration in seconds"))
			.Optional(TEXT("blend_out_time"), TEXT("number"), TEXT("Blend out duration in seconds"))
			.Optional(TEXT("blend_out_trigger_time"), TEXT("number"), TEXT("Time before end to trigger blend out"))
			.Optional(TEXT("enable_auto_blend_out"), TEXT("bool"), TEXT("Enable automatic blend out"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_montage_slot"),
		TEXT("Add a slot track to a montage"),
		FMonolithActionHandler::CreateStatic(&HandleAddMontageSlot),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Montage asset path"))
			.Required(TEXT("slot_name"), TEXT("string"), TEXT("Slot name (e.g. DefaultSlot)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_montage_slot"),
		TEXT("Rename a slot track on a montage by index"),
		FMonolithActionHandler::CreateStatic(&HandleSetMontageSlot),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Montage asset path"))
			.Required(TEXT("slot_index"), TEXT("integer"), TEXT("Index of the slot track"))
			.Required(TEXT("slot_name"), TEXT("string"), TEXT("New slot name"))
			.Build());

	// Wave 7 — Anim Modifiers + Composites
	Registry.RegisterAction(TEXT("animation"), TEXT("apply_anim_modifier"),
		TEXT("Apply an animation modifier class to a sequence"),
		FMonolithActionHandler::CreateStatic(&HandleApplyAnimModifier),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Required(TEXT("modifier_class"), TEXT("string"), TEXT("Modifier class name (e.g. UAnimationModifier_CreateCurve)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("list_anim_modifiers"),
		TEXT("List animation modifiers applied to a sequence"),
		FMonolithActionHandler::CreateStatic(&HandleListAnimModifiers),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_composite_info"),
		TEXT("Get segments and metadata from an animation composite"),
		FMonolithActionHandler::CreateStatic(&HandleGetCompositeInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimComposite asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_composite_segment"),
		TEXT("Add a segment to an animation composite"),
		FMonolithActionHandler::CreateStatic(&HandleAddCompositeSegment),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimComposite asset path"))
			.Required(TEXT("anim_path"), TEXT("string"), TEXT("Animation sequence to add"))
			.Optional(TEXT("start_pos"), TEXT("number"), TEXT("Start position in composite timeline"), TEXT("0.0"))
			.Optional(TEXT("play_rate"), TEXT("number"), TEXT("Playback rate"), TEXT("1.0"))
			.Optional(TEXT("looping_count"), TEXT("integer"), TEXT("Number of loops"), TEXT("1"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_composite_segment"),
		TEXT("Remove a segment from an animation composite by index"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveCompositeSegment),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimComposite asset path"))
			.Required(TEXT("segment_index"), TEXT("integer"), TEXT("Index of the segment to remove"))
			.Build());

	// Wave 8a — IKRig
	Registry.RegisterAction(TEXT("animation"), TEXT("get_ikrig_info"),
		TEXT("Get IK Rig asset info: solvers, goals, retarget chains, and skeleton overview"),
		FMonolithActionHandler::CreateStatic(&HandleGetIKRigInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("IKRig asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_ik_solver"),
		TEXT("Add a solver to an IK Rig asset, optionally setting a root bone and goals"),
		FMonolithActionHandler::CreateStatic(&HandleAddIKSolver),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("IKRig asset path"))
			.Required(TEXT("solver_type"), TEXT("string"), TEXT("Solver type (e.g. FullBodyIKSolver or /Script/IKRig.FullBodyIKSolver)"))
			.Optional(TEXT("root_bone"), TEXT("string"), TEXT("Root bone name for the solver"))
			.Optional(TEXT("goals"), TEXT("array"), TEXT("Array of {name, bone} goal objects to create and connect"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_retargeter_info"),
		TEXT("Get IK Retargeter asset info: source/target rigs, preview meshes, and chain mappings"),
		FMonolithActionHandler::CreateStatic(&HandleGetRetargeterInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("IKRetargeter asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_retarget_chain_mapping"),
		TEXT("Set chain mappings on an IK Retargeter via auto-map or manual source/target pair"),
		FMonolithActionHandler::CreateStatic(&HandleSetRetargetChainMapping),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("IKRetargeter asset path"))
			.Optional(TEXT("auto_map"), TEXT("string"), TEXT("Auto-map mode: exact, fuzzy, or clear"))
			.Optional(TEXT("source_chain"), TEXT("string"), TEXT("Source chain name for manual mapping"))
			.Optional(TEXT("target_chain"), TEXT("string"), TEXT("Target chain name for manual mapping"))
			.Build());

	// Wave 8b — Control Rig Read
	Registry.RegisterAction(TEXT("animation"), TEXT("get_control_rig_info"),
		TEXT("Get Control Rig hierarchy info: elements by type with parents, control settings, and counts"),
		FMonolithActionHandler::CreateStatic(&HandleGetControlRigInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ControlRigBlueprint asset path"))
			.Optional(TEXT("element_type"), TEXT("string"), TEXT("Filter: bone, control, null, curve, all (default: all)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_control_rig_variables"),
		TEXT("Get animatable controls and blueprint variables from a Control Rig asset"),
		FMonolithActionHandler::CreateStatic(&HandleGetControlRigVariables),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ControlRigBlueprint asset path"))
			.Build());

	// Wave 8c — Control Rig Write
	Registry.RegisterAction(TEXT("animation"), TEXT("add_control_rig_element"),
		TEXT("Add a bone, control, or null element to a Control Rig hierarchy"),
		FMonolithActionHandler::CreateStatic(&HandleAddControlRigElement),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("ControlRigBlueprint asset path"))
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
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("machine_name"), TEXT("string"), TEXT("State machine name (exact, as shown in get_state_machines)"))
			.Required(TEXT("state_name"), TEXT("string"), TEXT("Name for the new state"))
			.Optional(TEXT("position_x"), TEXT("integer"), TEXT("Node X position (default: 200)"), TEXT("200"))
			.Optional(TEXT("position_y"), TEXT("integer"), TEXT("Node Y position (default: 0)"), TEXT("0"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_transition"),
		TEXT("Add a transition between two states in a state machine"),
		FMonolithActionHandler::CreateStatic(&HandleAddTransition),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("machine_name"), TEXT("string"), TEXT("State machine name"))
			.Required(TEXT("from_state"), TEXT("string"), TEXT("Source state name"))
			.Required(TEXT("to_state"), TEXT("string"), TEXT("Destination state name"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_transition_rule"),
		TEXT("Wire a boolean variable as the condition for a state machine transition"),
		FMonolithActionHandler::CreateStatic(&HandleSetTransitionRule),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("machine_name"), TEXT("string"), TEXT("State machine name"))
			.Required(TEXT("from_state"), TEXT("string"), TEXT("Source state name"))
			.Required(TEXT("to_state"), TEXT("string"), TEXT("Destination state name"))
			.Required(TEXT("variable_name"), TEXT("string"), TEXT("Boolean variable name to use as transition condition"))
			.Build());

	// Wave 9 — ABP Read Enhancements
	Registry.RegisterAction(TEXT("animation"), TEXT("get_abp_variables"),
		TEXT("List all variables in an animation blueprint with types and defaults"),
		FMonolithActionHandler::CreateStatic(&HandleGetAbpVariables),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_abp_linked_assets"),
		TEXT("Find all animation assets referenced by an animation blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleGetAbpLinkedAssets),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Build());

	// Wave 11 — Asset Creation + Setup
	Registry.RegisterAction(TEXT("animation"), TEXT("create_blend_space"),
		TEXT("Create a new 2D BlendSpace asset with skeleton and optional axis config"),
		FMonolithActionHandler::CreateStatic(&HandleCreateBlendSpace),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path for the new BlendSpace (e.g. /Game/Anims/BS_Locomotion)"))
			.Required(TEXT("skeleton_path"), TEXT("string"), TEXT("Skeleton asset path"))
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
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path for the new BlendSpace1D"))
			.Required(TEXT("skeleton_path"), TEXT("string"), TEXT("Skeleton asset path"))
			.Optional(TEXT("axis_name"), TEXT("string"), TEXT("Axis display name (default: None)"))
			.Optional(TEXT("axis_min"), TEXT("number"), TEXT("Axis minimum value (default: 0)"))
			.Optional(TEXT("axis_max"), TEXT("number"), TEXT("Axis maximum value (default: 100)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("create_aim_offset"),
		TEXT("Create a new 2D AimOffset asset with Yaw/Pitch axis defaults"),
		FMonolithActionHandler::CreateStatic(&HandleCreateAimOffset),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path for the new AimOffset"))
			.Required(TEXT("skeleton_path"), TEXT("string"), TEXT("Skeleton asset path"))
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
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path for the new AimOffset1D"))
			.Required(TEXT("skeleton_path"), TEXT("string"), TEXT("Skeleton asset path"))
			.Optional(TEXT("axis_name"), TEXT("string"), TEXT("Axis display name (default: Yaw)"))
			.Optional(TEXT("axis_min"), TEXT("number"), TEXT("Axis minimum (default: -180)"))
			.Optional(TEXT("axis_max"), TEXT("number"), TEXT("Axis maximum (default: 180)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("create_composite"),
		TEXT("Create a new AnimComposite asset with skeleton"),
		FMonolithActionHandler::CreateStatic(&HandleCreateComposite),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path for the new composite"))
			.Required(TEXT("skeleton_path"), TEXT("string"), TEXT("Skeleton asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("create_anim_blueprint"),
		TEXT("Create a new Animation Blueprint asset with skeleton"),
		FMonolithActionHandler::CreateStatic(&HandleCreateAnimBlueprint),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path for the new ABP (e.g. /Game/ABP/ABP_Character)"))
			.Required(TEXT("skeleton_path"), TEXT("string"), TEXT("Skeleton asset path"))
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
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Optional(TEXT("rate_scale"), TEXT("number"), TEXT("Playback rate scale (default 1.0)"))
			.Optional(TEXT("loop"), TEXT("bool"), TEXT("Whether the sequence loops"))
			.Optional(TEXT("interpolation"), TEXT("string"), TEXT("Interpolation type: Linear or Step"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_additive_settings"),
		TEXT("Configure additive animation settings on a sequence (triggers DDC rebuild)"),
		FMonolithActionHandler::CreateStatic(&HandleSetAdditiveSettings),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Optional(TEXT("additive_anim_type"), TEXT("string"), TEXT("NoAdditive, LocalSpace, or MeshSpace"))
			.Optional(TEXT("ref_pose_type"), TEXT("string"), TEXT("RefPose, AnimScaled, AnimFrame, or LocalAnimFrame"))
			.Optional(TEXT("ref_frame_index"), TEXT("integer"), TEXT("Reference frame index for AnimFrame/LocalAnimFrame modes"))
			.Optional(TEXT("ref_pose_seq"), TEXT("string"), TEXT("Reference pose sequence asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_compression_settings"),
		TEXT("Assign bone and/or curve compression settings assets to a sequence"),
		FMonolithActionHandler::CreateStatic(&HandleSetCompressionSettings),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Optional(TEXT("bone_compression"), TEXT("string"), TEXT("Bone compression settings asset path"))
			.Optional(TEXT("curve_compression"), TEXT("string"), TEXT("Curve compression settings asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("get_sync_markers"),
		TEXT("Read all authored sync markers from an animation sequence"),
		FMonolithActionHandler::CreateStatic(&HandleGetSyncMarkers),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("add_sync_marker"),
		TEXT("Add an authored sync marker to a sequence"),
		FMonolithActionHandler::CreateStatic(&HandleAddSyncMarker),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Required(TEXT("marker_name"), TEXT("string"), TEXT("Sync marker name (e.g. FootDown)"))
			.Required(TEXT("time"), TEXT("number"), TEXT("Time in seconds"))
			.Optional(TEXT("track_index"), TEXT("integer"), TEXT("Track index (default 0)"), TEXT("0"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_sync_marker"),
		TEXT("Remove sync markers by name (all with that name) or by index"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveSyncMarker),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Optional(TEXT("marker_name"), TEXT("string"), TEXT("Remove all markers with this name"))
			.Optional(TEXT("marker_index"), TEXT("integer"), TEXT("Remove specific marker by index"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("rename_sync_marker"),
		TEXT("Rename all sync markers with a given name to a new name"),
		FMonolithActionHandler::CreateStatic(&HandleRenameSyncMarker),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Required(TEXT("old_name"), TEXT("string"), TEXT("Current marker name"))
			.Required(TEXT("new_name"), TEXT("string"), TEXT("New marker name"))
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
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Montage asset path"))
			.Required(TEXT("anim_path"), TEXT("string"), TEXT("AnimSequence to reference"))
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
			.Required(TEXT("source_path"), TEXT("string"), TEXT("Source animation asset path"))
			.Required(TEXT("target_path"), TEXT("string"), TEXT("Target animation asset path"))
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
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path for the new montage"))
			.Required(TEXT("skeleton_path"), TEXT("string"), TEXT("Skeleton asset path"))
			.Optional(TEXT("slot_name"), TEXT("string"), TEXT("Slot name (default 'DefaultSlot')"))
			.Optional(TEXT("sections"), TEXT("array"), TEXT("Array of {name, anim_path, start_time?, next_section?}"))
			.Optional(TEXT("blend"), TEXT("object"), TEXT("{blend_in_time?, blend_out_time?, blend_out_trigger_time?, enable_auto_blend_out?}"))
			.Optional(TEXT("notifies"), TEXT("array"), TEXT("Array of {notify_class, time, duration?, track_name?}"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("build_sequence_from_poses"),
		TEXT("Build an AnimSequence from per-frame bone transforms using IAnimationDataController"),
		FMonolithActionHandler::CreateStatic(&HandleBuildSequenceFromPoses),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Target AnimSequence path (created if missing)"))
			.Required(TEXT("skeleton_path"), TEXT("string"), TEXT("Skeleton asset path"))
			.Required(TEXT("frames"), TEXT("array"), TEXT("Array of {bones: [{name, location:[x,y,z], rotation:[x,y,z,w], scale:[x,y,z]}, ...]}"))
			.Optional(TEXT("frame_rate"), TEXT("number"), TEXT("Frame rate (default 30)"))
			.Build());

	// Wave 14 — Notify Properties
	Registry.RegisterAction(TEXT("animation"), TEXT("set_notify_properties"),
		TEXT("Set UPROPERTY values on a notify object using reflection (ImportText_Direct). Works on both instant and state notifies."),
		FMonolithActionHandler::CreateStatic(&HandleSetNotifyProperties),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation asset path (sequence, montage, composite)"))
			.Required(TEXT("notify_index"), TEXT("integer"), TEXT("Index of the notify in the Notifies array"))
			.Required(TEXT("properties"), TEXT("object"), TEXT("Object of property_name:value pairs. Values use UE text import format (same as Details panel copy/paste)."))
			.Build());

	// Wave 15 — Physics Assets + IK Chains
	Registry.RegisterAction(TEXT("animation"), TEXT("get_physics_asset_info"),
		TEXT("Read all bodies, constraints, profiles, and solver settings from a physics asset"),
		FMonolithActionHandler::CreateStatic(&HandleGetPhysicsAssetInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Physics asset path"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_body_properties"),
		TEXT("Modify mass, physics type, collision, damping on a physics body identified by bone name"),
		FMonolithActionHandler::CreateStatic(&HandleSetBodyProperties),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Physics asset path"))
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
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Physics asset path"))
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
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("IK Rig asset path"))
			.Required(TEXT("chain_name"), TEXT("string"), TEXT("Name for the chain"))
			.Required(TEXT("start_bone"), TEXT("string"), TEXT("Start bone name"))
			.Required(TEXT("end_bone"), TEXT("string"), TEXT("End bone name"))
			.Optional(TEXT("goal_name"), TEXT("string"), TEXT("Goal to associate with this chain"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("remove_retarget_chain"),
		TEXT("Remove a retarget chain from an IK Rig asset"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveRetargetChain),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("IK Rig asset path"))
			.Required(TEXT("chain_name"), TEXT("string"), TEXT("Name of the chain to remove"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("set_retarget_chain_bones"),
		TEXT("Update start/end bones of an existing retarget chain in an IK Rig"),
		FMonolithActionHandler::CreateStatic(&HandleSetRetargetChainBones),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("IK Rig asset path"))
			.Required(TEXT("chain_name"), TEXT("string"), TEXT("Name of the chain to modify"))
			.Optional(TEXT("start_bone"), TEXT("string"), TEXT("New start bone name"))
			.Optional(TEXT("end_bone"), TEXT("string"), TEXT("New end bone name"))
			.Optional(TEXT("goal_name"), TEXT("string"), TEXT("New goal name"))
			.Optional(TEXT("new_name"), TEXT("string"), TEXT("Rename the chain"))
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
	GEditor->EndTransaction();

	if (Index == INDEX_NONE)
		return FMonolithActionResult::Error(TEXT("Failed to add sample"));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("index"), Index);
	Root->SetStringField(TEXT("animation"), AnimPath);
	Root->SetNumberField(TEXT("x"), X);
	Root->SetNumberField(TEXT("y"), Y);
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

	GEditor->EndTransaction();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("index"), SampleIndex);
	Root->SetNumberField(TEXT("x"), X);
	Root->SetNumberField(TEXT("y"), Y);
	if (!AnimPath.IsEmpty()) Root->SetStringField(TEXT("animation"), AnimPath);
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
	GEditor->EndTransaction();

	if (!bSuccess)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to delete sample at index %d"), SampleIndex));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("deleted_index"), SampleIndex);
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
			NodesArr.Add(MakeShared<FJsonValueObject>(NodeObj));
		}
	}

	Root->SetArrayField(TEXT("nodes"), NodesArr);
	Root->SetNumberField(TEXT("count"), NodesArr.Num());
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

	const FRawAnimSequenceTrack* RawTrackPtr = nullptr;
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	const TArray<FBoneAnimationTrack>& BoneTracks = DataModel->GetBoneAnimationTracks();
PRAGMA_ENABLE_DEPRECATION_WARNINGS
	for (const FBoneAnimationTrack& Track : BoneTracks)
	{
		if (Track.Name == FName(*BoneName))
		{
			RawTrackPtr = &Track.InternalTrackData;
			break;
		}
	}
	if (!RawTrackPtr)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Bone track not found: %s"), *BoneName));

	const FRawAnimSequenceTrack& RawTrack = *RawTrackPtr;

	int32 NumPosKeys = RawTrack.PosKeys.Num();
	int32 NumRotKeys = RawTrack.RotKeys.Num();
	int32 NumScaleKeys = RawTrack.ScaleKeys.Num();
	int32 MaxKeys = FMath::Max3(NumPosKeys, NumRotKeys, NumScaleKeys);

	if (EndFrame < 0 || EndFrame >= MaxKeys)
		EndFrame = MaxKeys - 1;
	if (StartFrame < 0) StartFrame = 0;
	if (StartFrame > EndFrame)
		return FMonolithActionResult::Error(FString::Printf(TEXT("start_frame (%d) > end_frame (%d)"), StartFrame, EndFrame));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("bone_name"), BoneName);
	Root->SetNumberField(TEXT("num_keys"), MaxKeys);

	// Positions
	TArray<TSharedPtr<FJsonValue>> PosArr;
	for (int32 i = StartFrame; i <= EndFrame && i < NumPosKeys; ++i)
	{
		TArray<TSharedPtr<FJsonValue>> Vec;
		Vec.Add(MakeShared<FJsonValueNumber>(RawTrack.PosKeys[i].X));
		Vec.Add(MakeShared<FJsonValueNumber>(RawTrack.PosKeys[i].Y));
		Vec.Add(MakeShared<FJsonValueNumber>(RawTrack.PosKeys[i].Z));
		PosArr.Add(MakeShared<FJsonValueArray>(Vec));
	}
	Root->SetArrayField(TEXT("positions"), PosArr);

	// Rotations
	TArray<TSharedPtr<FJsonValue>> RotArr;
	for (int32 i = StartFrame; i <= EndFrame && i < NumRotKeys; ++i)
	{
		TArray<TSharedPtr<FJsonValue>> Quat;
		Quat.Add(MakeShared<FJsonValueNumber>(RawTrack.RotKeys[i].X));
		Quat.Add(MakeShared<FJsonValueNumber>(RawTrack.RotKeys[i].Y));
		Quat.Add(MakeShared<FJsonValueNumber>(RawTrack.RotKeys[i].Z));
		Quat.Add(MakeShared<FJsonValueNumber>(RawTrack.RotKeys[i].W));
		RotArr.Add(MakeShared<FJsonValueArray>(Quat));
	}
	Root->SetArrayField(TEXT("rotations"), RotArr);

	// Scales
	TArray<TSharedPtr<FJsonValue>> ScaleArr;
	for (int32 i = StartFrame; i <= EndFrame && i < NumScaleKeys; ++i)
	{
		TArray<TSharedPtr<FJsonValue>> Vec;
		Vec.Add(MakeShared<FJsonValueNumber>(RawTrack.ScaleKeys[i].X));
		Vec.Add(MakeShared<FJsonValueNumber>(RawTrack.ScaleKeys[i].Y));
		Vec.Add(MakeShared<FJsonValueNumber>(RawTrack.ScaleKeys[i].Z));
		ScaleArr.Add(MakeShared<FJsonValueArray>(Vec));
	}
	Root->SetArrayField(TEXT("scales"), ScaleArr);

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

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("curve_name"), CurveName);
	Root->SetNumberField(TEXT("num_keys"), Keys.Num());
	Root->SetArrayField(TEXT("keys"), KeysArray);
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

	UAnimationModifier* Modifier = NewObject<UAnimationModifier>(GetTransientPackage(), ModifierUClass);
	if (!Modifier)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create modifier instance"));
	}

	Modifier->ApplyToAnimationSequence(Seq);
	Seq->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("modifier_class"), ModifierUClass->GetName());
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
		SolversArr.Add(MakeShared<FJsonValueObject>(SolverObj));
	}
	Root->SetArrayField(TEXT("solvers"), SolversArr);
	Root->SetNumberField(TEXT("solver_count"), NumSolvers);

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

	// Normalize solver type — add package prefix if bare name
	if (!SolverType.Contains(TEXT("/")))
	{
		SolverType = FString::Printf(TEXT("/Script/IKRig.%s"), *SolverType);
	}

	int32 SolverIdx = C->AddSolver(SolverType);
	if (SolverIdx < 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to add solver of type '%s' — check type name"), *SolverType));
	}

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
	Root->SetStringField(TEXT("solver_type"), SolverType);
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

FMonolithActionResult FMonolithAnimationActions::HandleSetTransitionRule(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath    = Params->GetStringField(TEXT("asset_path"));
	FString MachineName  = Params->GetStringField(TEXT("machine_name"));
	FString FromState    = Params->GetStringField(TEXT("from_state"));
	FString ToState      = Params->GetStringField(TEXT("to_state"));
	FString VariableName = Params->GetStringField(TEXT("variable_name"));

	if (MachineName.IsEmpty())  return FMonolithActionResult::Error(TEXT("Missing required parameter: machine_name"));
	if (FromState.IsEmpty())    return FMonolithActionResult::Error(TEXT("Missing required parameter: from_state"));
	if (ToState.IsEmpty())      return FMonolithActionResult::Error(TEXT("Missing required parameter: to_state"));
	if (VariableName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: variable_name"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	// Validate variable exists and is boolean
	const FBPVariableDescription* VarDesc = nullptr;
	for (const FBPVariableDescription& V : ABP->NewVariables)
	{
		if (V.VarName.ToString() == VariableName)
		{
			VarDesc = &V;
			break;
		}
	}
	if (!VarDesc)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Variable '%s' not found in ABP. Use get_abp_variables to list available variables."), *VariableName));
	}
	if (!VarDesc->VarType.PinCategory.ToString().Equals(TEXT("bool"), ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Variable '%s' is type '%s', not bool. Transition rules require a boolean variable."),
			*VariableName, *VarDesc->VarType.PinCategory.ToString()));
	}

	UAnimationStateMachineGraph* SMGraph = FindStateMachineGraphByName(ABP, MachineName);
	if (!SMGraph) return FMonolithActionResult::Error(FString::Printf(TEXT("State machine '%s' not found in ABP"), *MachineName));

	// Find the transition node connecting the two states
	UAnimStateTransitionNode* TransNode = nullptr;
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
			TransNode = TN;
			break;
		}
	}
	if (!TransNode)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No transition found from '%s' to '%s'. Use add_transition first."), *FromState, *ToState));
	}

	UEdGraph* RuleGraph = TransNode->GetBoundGraph();
	if (!RuleGraph)
	{
		return FMonolithActionResult::Error(TEXT("Transition has no bound rule graph"));
	}

	// Find the UAnimGraphNode_TransitionResult node in the rule graph
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

	// Find bCanEnterTransition input pin on the result node
	UEdGraphPin* ResultPin = ResultNode->FindPin(TEXT("bCanEnterTransition"), EGPD_Input);
	if (!ResultPin)
	{
		return FMonolithActionResult::Error(TEXT("Could not find bCanEnterTransition pin on transition result node"));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Transition Rule")));
	RuleGraph->Modify();

	// Clear any existing wiring before connecting our getter
	ResultPin->BreakAllPinLinks();

	// Spawn a UK2Node_VariableGet for the boolean variable
	UK2Node_VariableGet* VarGetNode = NewObject<UK2Node_VariableGet>(RuleGraph);
	VarGetNode->VariableReference.SetSelfMember(FName(*VariableName));
	RuleGraph->AddNode(VarGetNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
	VarGetNode->NodePosX = ResultNode->NodePosX - 200;
	VarGetNode->NodePosY = ResultNode->NodePosY;
	VarGetNode->AllocateDefaultPins();

	// Find the output value pin — first try the variable name, then any non-self output pin
	UEdGraphPin* GetterOutputPin = VarGetNode->FindPin(FName(*VariableName), EGPD_Output);
	if (!GetterOutputPin)
	{
		for (UEdGraphPin* Pin : VarGetNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && Pin->PinName != TEXT("self"))
			{
				GetterOutputPin = Pin;
				break;
			}
		}
	}

	bool bWired = false;
	if (GetterOutputPin)
	{
		const UEdGraphSchema* RuleSchema = RuleGraph->GetSchema();
		if (RuleSchema)
		{
			bWired = RuleSchema->TryCreateConnection(GetterOutputPin, ResultPin);
		}
	}

	GEditor->EndTransaction();

	FKismetEditorUtilities::CompileBlueprint(ABP);
	ABP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("machine_name"), MachineName);
	Root->SetStringField(TEXT("from_state"), FromState);
	Root->SetStringField(TEXT("to_state"), ToState);
	Root->SetStringField(TEXT("variable_name"), VariableName);
	Root->SetBoolField(TEXT("pin_wired"), bWired);
	if (!bWired)
	{
		Root->SetStringField(TEXT("warning"), TEXT("Variable getter node was created but output pin could not be found. Manual wiring may be needed."));
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
			if (Pair.Key != TEXT("op"))
			{
				SubParams->SetField(Pair.Key, Pair.Value);
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
		const FString& PropName = Pair.Key;
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
