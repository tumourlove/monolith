#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Skeleton-level authoring actions (animation namespace):
 *
 *   Slots / slot groups   — get_skeleton_slots, add_skeleton_slot, remove_skeleton_slot, set_slot_group
 *   Blend profiles        — get_blend_profiles, create_blend_profile, set_blend_profile_bones, remove_blend_profile
 *   Curve metadata        — get_skeleton_curve_metadata, add_skeleton_curve, remove_skeleton_curve,
 *                           set_skeleton_curve_metadata
 *
 * Engine surface used (identical in UE 5.7 and 5.8 headers):
 *   USkeleton::GetSlotGroups / ContainsSlotName / GetSlotGroupName / SetSlotGroupName /
 *     AddSlotGroupName / FindAnimSlotGroup / RemoveSlotName / RemoveSlotGroup        (Skeleton.h)
 *   USkeleton::BlendProfiles (public UPROPERTY) / GetBlendProfile / CreateNewBlendProfile  (Skeleton.h)
 *   UBlendProfile::Mode (public UPROPERTY) / SetBoneBlendScale / RemoveEntry / ProfileEntries (BlendProfile.h)
 *   USkeleton::AddCurveMetaData / GetCurveMetaData / RemoveCurveMetaData /
 *     SetCurveMetaDataMaterial / SetCurveMetaDataMorphTarget                      (Skeleton.h, WITH_EDITOR)
 *   UAnimCurveMetaData::SetCurveMetaDataBoneLinks (virtual, via IInterface_AnimCurveMetaData) (AnimCurveMetadata.h)
 *
 * Every mutation runs inside an FScopedTransaction, calls Modify() on the
 * touched objects, marks the skeleton package dirty, and optionally saves
 * (`save`, default false).
 */
class FMonolithAnimationSkeletonActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// Slots
	static FMonolithActionResult HandleGetSkeletonSlots(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddSkeletonSlot(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveSkeletonSlot(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetSlotGroup(const TSharedPtr<FJsonObject>& Params);

	// Blend profiles
	static FMonolithActionResult HandleGetBlendProfiles(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateBlendProfile(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetBlendProfileBones(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveBlendProfile(const TSharedPtr<FJsonObject>& Params);

	// Curve metadata
	static FMonolithActionResult HandleGetSkeletonCurveMetadata(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddSkeletonCurve(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveSkeletonCurve(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetSkeletonCurveMetadata(const TSharedPtr<FJsonObject>& Params);
};
