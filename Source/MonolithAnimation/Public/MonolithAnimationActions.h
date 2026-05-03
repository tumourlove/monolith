#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class UAnimMontage;
class UBlendSpace;
class UAnimBlueprint;
class UAnimSequence;
class USkeleton;
class USkeletalMesh;

/**
 * Animation domain action handlers for Monolith.
 * Ported from AnimationMCPReaderLibrary — 23 proven actions.
 */
class FMonolithAnimationActions
{
public:
	/** Register all animation actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// --- Montage Sections (4) ---
	static FMonolithActionResult HandleAddMontageSection(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDeleteMontageSection(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetSectionNext(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetSectionTime(const TSharedPtr<FJsonObject>& Params);

	// --- BlendSpace Samples (3) ---
	static FMonolithActionResult HandleAddBlendSpaceSample(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleEditBlendSpaceSample(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDeleteBlendSpaceSample(const TSharedPtr<FJsonObject>& Params);

	// --- ABP Graph Reading (7) ---
	static FMonolithActionResult HandleGetStateMachines(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetStateInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetTransitions(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetBlendNodes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetLinkedLayers(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetGraphs(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetNodes(const TSharedPtr<FJsonObject>& Params);

	// --- Notify Editing (2) ---
	static FMonolithActionResult HandleSetNotifyTime(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetNotifyDuration(const TSharedPtr<FJsonObject>& Params);

	// --- Bone Tracks (3) ---
	static FMonolithActionResult HandleSetBoneTrackKeys(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddBoneTrack(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveBoneTrack(const TSharedPtr<FJsonObject>& Params);

	// --- Bone Pose Copy (1) ---
	// Reads the evaluated pose (raw track + ref pose fallback) for a list of
	// bones at a given time on the source AnimSequence, then writes those
	// transforms as keys to the destination AnimSequence. Useful when a target
	// anim has T-pose / wrong values on a subset of bones (e.g. left arm) and
	// you want to import a clean pose from a working anim without touching
	// the rest of the target.
	static FMonolithActionResult HandleCopyBonePoseBetweenSequences(const TSharedPtr<FJsonObject>& Params);

	// --- Virtual Bones (2) ---
	static FMonolithActionResult HandleAddVirtualBone(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveVirtualBones(const TSharedPtr<FJsonObject>& Params);

	// --- Skeleton Info (2) ---
	static FMonolithActionResult HandleGetSkeletonInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetSkeletalMeshInfo(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 1: Read Actions (8) ---
	static FMonolithActionResult HandleGetSequenceInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetSequenceNotifies(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetBoneTrackKeys(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListBoneTracks(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetSequenceCurves(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetMontageInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetBlendSpaceInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetSkeletonSockets(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetAbpInfo(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 2: Notify CRUD (4) ---
	static FMonolithActionResult HandleAddNotify(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddNotifyState(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveNotify(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetNotifyTrack(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 3: Curve CRUD (5) ---
	static FMonolithActionResult HandleListCurves(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddCurve(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveCurve(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetCurveKeys(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetCurveKeys(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 4: Skeleton + BlendSpace (6) ---
	static FMonolithActionResult HandleAddSocket(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveSocket(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetSocketTransform(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetSkeletonCurves(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetBlendSpaceAxis(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetRootMotionSettings(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 5: Creation + Montage (6) ---
	static FMonolithActionResult HandleCreateSequence(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDuplicateSequence(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateMontage(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetMontageBlend(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddMontageSlot(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetMontageSlot(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 7: Anim Modifiers + Composites (5) ---
	static FMonolithActionResult HandleApplyAnimModifier(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListAnimModifiers(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetCompositeInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddCompositeSegment(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveCompositeSegment(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 8a: IKRig (4) ---
	static FMonolithActionResult HandleGetIKRigInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddIKSolver(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetRetargeterInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetRetargetChainMapping(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 8b: Control Rig Read (2) ---
	static FMonolithActionResult HandleGetControlRigInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetControlRigVariables(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 8c: Control Rig Write (1) ---
	static FMonolithActionResult HandleAddControlRigElement(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 9: ABP Read Enhancements (2) ---
	static FMonolithActionResult HandleGetAbpVariables(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetAbpLinkedAssets(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 10: ABP Write Experimental (3) ---
	static FMonolithActionResult HandleAddStateToMachine(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddTransition(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetTransitionRule(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 14: Notify Properties (1) ---
	static FMonolithActionResult HandleSetNotifyProperties(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 15: Physics Assets + IK Chains (6) ---
	static FMonolithActionResult HandleGetPhysicsAssetInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetBodyProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetConstraintProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddRetargetChain(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveRetargetChain(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetRetargetChainBones(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 11: Asset Creation + Setup (7 in this file) ---
	static FMonolithActionResult HandleCreateBlendSpace(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateBlendSpace1D(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateAimOffset(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateAimOffset1D(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateComposite(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateAnimBlueprint(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCompareSkeletons(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 12: Sequence Properties + Sync Markers (7) ---
	static FMonolithActionResult HandleSetSequenceProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetAdditiveSettings(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetCompressionSettings(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetSyncMarkers(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddSyncMarker(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveSyncMarker(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRenameSyncMarker(const TSharedPtr<FJsonObject>& Params);

	// --- Wave 13: Batch Ops + Montage Completion (6) ---
	static FMonolithActionResult HandleBatchExecute(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddMontageAnimSegment(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCloneNotifySetup(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleBulkAddNotify(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateMontageFromSections(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleBuildSequenceFromPoses(const TSharedPtr<FJsonObject>& Params);
};
