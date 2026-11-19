#include "MonolithAnimationSkeletonActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"

#include "Animation/Skeleton.h"
#include "Animation/BlendProfile.h"
#include "Animation/AnimCurveMetadata.h"
#include "Animation/BoneReference.h"
#include "ReferenceSkeleton.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "EditorAssetLibrary.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "MonolithAnimationSkeletonActions"

// ---------------------------------------------------------------------------
// File-local helpers. Names carry the SkelAuthoring prefix so they stay unique
// when the module is compiled as a single unity blob.
// ---------------------------------------------------------------------------
namespace
{
	USkeleton* SkelAuthoringLoadSkeleton(const TSharedPtr<FJsonObject>& Params, FString& OutPath, FString& OutError)
	{
		OutPath = Params->GetStringField(TEXT("skeleton_path"));
		if (OutPath.IsEmpty())
		{
			OutError = TEXT("'skeleton_path' is required (Skeleton asset path, e.g. /Game/Tests/Monolith/SK_Test_Skeleton).");
			return nullptr;
		}
		USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(OutPath);
		if (!Skeleton)
		{
			OutError = FString::Printf(TEXT("Skeleton not found: %s (the path must point to a USkeleton asset, not a SkeletalMesh)."), *OutPath);
		}
		return Skeleton;
	}

	bool SkelAuthoringWantsSave(const TSharedPtr<FJsonObject>& Params)
	{
		bool bSave = false;
		Params->TryGetBoolField(TEXT("save"), bSave);
		return bSave;
	}

	/** Mark dirty and optionally save. Returns the saved flag for the result payload. */
	bool SkelAuthoringFinish(USkeleton* Skeleton, bool bSave)
	{
		Skeleton->MarkPackageDirty();
		if (bSave)
		{
			return UEditorAssetLibrary::SaveAsset(Skeleton->GetPathName(), /*bOnlyIfIsDirty=*/false);
		}
		return false;
	}

	/**
	 * Collect names from either a single-string field or an array-of-strings field.
	 * Returns false with OutError set if neither yields a non-empty name.
	 */
	bool SkelAuthoringCollectNames(const TSharedPtr<FJsonObject>& Params, const TCHAR* SingleKey, const TCHAR* ArrayKey,
		TArray<FName>& OutNames, FString& OutError)
	{
		FString Single;
		if (Params->TryGetStringField(SingleKey, Single) && !Single.TrimStartAndEnd().IsEmpty())
		{
			OutNames.AddUnique(FName(*Single.TrimStartAndEnd()));
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params->TryGetArrayField(ArrayKey, Arr))
		{
			for (const TSharedPtr<FJsonValue>& Val : *Arr)
			{
				FString S;
				if (!Val.IsValid() || !Val->TryGetString(S) || S.TrimStartAndEnd().IsEmpty())
				{
					OutError = FString::Printf(TEXT("Every item in '%s' must be a non-empty string."), ArrayKey);
					return false;
				}
				OutNames.AddUnique(FName(*S.TrimStartAndEnd()));
			}
		}
		if (OutNames.Num() == 0)
		{
			OutError = FString::Printf(TEXT("Provide '%s' (string) or '%s' (array of strings)."), SingleKey, ArrayKey);
			return false;
		}
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> SkelAuthoringNamesToJson(const TArray<FName>& Names)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Names.Num());
		for (const FName& N : Names)
		{
			Out.Add(MakeShared<FJsonValueString>(N.ToString()));
		}
		return Out;
	}

	// --- Slots --------------------------------------------------------------

	TSharedPtr<FJsonObject> SkelAuthoringSlotsToJson(const USkeleton* Skeleton)
	{
		TArray<TSharedPtr<FJsonValue>> GroupsArr;
		int32 SlotCount = 0;
		for (const FAnimSlotGroup& Group : Skeleton->GetSlotGroups())
		{
			TSharedPtr<FJsonObject> GroupObj = MakeShared<FJsonObject>();
			GroupObj->SetStringField(TEXT("group"), Group.GroupName.ToString());
			GroupObj->SetArrayField(TEXT("slots"), SkelAuthoringNamesToJson(Group.SlotNames));
			GroupObj->SetNumberField(TEXT("slot_count"), Group.SlotNames.Num());
			SlotCount += Group.SlotNames.Num();
			GroupsArr.Add(MakeShared<FJsonValueObject>(GroupObj));
		}
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("groups"), GroupsArr);
		Root->SetNumberField(TEXT("group_count"), GroupsArr.Num());
		Root->SetNumberField(TEXT("slot_count"), SlotCount);
		Root->SetStringField(TEXT("default_group"), FAnimSlotGroup::DefaultGroupName.ToString());
		Root->SetStringField(TEXT("default_slot"), FAnimSlotGroup::DefaultSlotName.ToString());
		return Root;
	}

	// --- Blend profiles ----------------------------------------------------

	const TCHAR* SkelAuthoringBlendModeToString(EBlendProfileMode Mode)
	{
		switch (Mode)
		{
			case EBlendProfileMode::TimeFactor:   return TEXT("time_factor");
			case EBlendProfileMode::WeightFactor: return TEXT("weight_factor");
			case EBlendProfileMode::BlendMask:    return TEXT("blend_mask");
			default:                              return TEXT("unknown");
		}
	}

	bool SkelAuthoringParseBlendMode(const FString& In, EBlendProfileMode& Out)
	{
		FString S = In.TrimStartAndEnd().Replace(TEXT("_"), TEXT("")).Replace(TEXT(" "), TEXT(""));
		if (S.Equals(TEXT("timefactor"), ESearchCase::IgnoreCase) || S.Equals(TEXT("time"), ESearchCase::IgnoreCase))
		{
			Out = EBlendProfileMode::TimeFactor; return true;
		}
		if (S.Equals(TEXT("weightfactor"), ESearchCase::IgnoreCase) || S.Equals(TEXT("weight"), ESearchCase::IgnoreCase))
		{
			Out = EBlendProfileMode::WeightFactor; return true;
		}
		if (S.Equals(TEXT("blendmask"), ESearchCase::IgnoreCase) || S.Equals(TEXT("mask"), ESearchCase::IgnoreCase))
		{
			Out = EBlendProfileMode::BlendMask; return true;
		}
		return false;
	}

	TSharedPtr<FJsonObject> SkelAuthoringBlendProfileToJson(const UBlendProfile* Profile)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Profile->GetName());
		Obj->SetStringField(TEXT("mode"), SkelAuthoringBlendModeToString(Profile->GetMode()));
		Obj->SetNumberField(TEXT("default_scale"), Profile->GetDefaultBlendScale());
		TArray<TSharedPtr<FJsonValue>> EntriesArr;
		for (const FBlendProfileBoneEntry& Entry : Profile->ProfileEntries)
		{
			TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("bone"), Entry.BoneReference.BoneName.ToString());
			E->SetNumberField(TEXT("scale"), Entry.BlendScale);
			EntriesArr.Add(MakeShared<FJsonValueObject>(E));
		}
		Obj->SetNumberField(TEXT("entry_count"), EntriesArr.Num());
		Obj->SetArrayField(TEXT("entries"), EntriesArr);
		return Obj;
	}

	TArray<FString> SkelAuthoringBlendProfileNames(const USkeleton* Skeleton)
	{
		TArray<FString> Names;
		for (const TObjectPtr<UBlendProfile>& P : Skeleton->BlendProfiles)
		{
			if (P)
			{
				Names.Add(P->GetName());
			}
		}
		return Names;
	}

	/**
	 * Change a profile's mode. Mirrors FEditableSkeleton::SetBlendProfileMode: when switching
	 * to/from blend_mask the default scale changes (1.0 <-> 0.0), so entries are re-applied
	 * from the end so any that now equal the default are dropped. Caller owns the transaction.
	 */
	void SkelAuthoringApplyBlendMode(UBlendProfile* Profile, EBlendProfileMode NewMode)
	{
		const bool bWasBlendMask = Profile->IsBlendMask();
		Profile->SetFlags(RF_Transactional);
		Profile->Modify();
		Profile->Mode = NewMode;
		if (bWasBlendMask != Profile->IsBlendMask())
		{
			for (int32 EntryIndex = Profile->GetNumBlendEntries() - 1; EntryIndex >= 0; --EntryIndex)
			{
				const FBlendProfileBoneEntry& Entry = Profile->ProfileEntries[EntryIndex];
				Profile->SetBoneBlendScale(Entry.BoneReference.BoneName, Entry.BlendScale);
			}
		}
	}

	// --- Curve metadata ----------------------------------------------------

	TSharedPtr<FJsonObject> SkelAuthoringCurveToJson(FName CurveName, const FCurveMetaData& MetaData)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), CurveName.ToString());
		Obj->SetBoolField(TEXT("material"), MetaData.Type.bMaterial);
		Obj->SetBoolField(TEXT("morph_target"), MetaData.Type.bMorphtarget);
		TArray<TSharedPtr<FJsonValue>> Bones;
		for (const FBoneReference& Ref : MetaData.LinkedBones)
		{
			Bones.Add(MakeShared<FJsonValueString>(Ref.BoneName.ToString()));
		}
		Obj->SetArrayField(TEXT("linked_bones"), Bones);
		// 255 is the engine sentinel for "evaluate at every LOD"; echo it as -1 for clarity.
		Obj->SetNumberField(TEXT("max_lod"), MetaData.MaxLOD == 0xFF ? -1 : static_cast<int32>(MetaData.MaxLOD));
		return Obj;
	}
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithAnimationSkeletonActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// --- Slots ---

	Registry.RegisterAction(TEXT("animation"), TEXT("get_skeleton_slots"),
		TEXT("List a Skeleton's montage slot groups and the slot names registered in each. Slot nodes in "
			 "Animation Blueprints and montage slot tracks reference these names."),
		FMonolithActionHandler::CreateStatic(&HandleGetSkeletonSlots),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"), {TEXT("skeleton")})
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("add_skeleton_slot"),
		TEXT("Register a montage slot name on a Skeleton. 'group_name' selects the slot group "
			 "(created if missing; defaults to the engine default group). If the slot already exists "
			 "and 'group_name' is given, the slot is moved to that group. Idempotent."),
		FMonolithActionHandler::CreateStatic(&HandleAddSkeletonSlot),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"), {TEXT("skeleton")})
			.Required(TEXT("slot_name"), TEXT("string"), TEXT("Slot name to register"))
			.Optional(TEXT("group_name"), TEXT("string"), TEXT("Slot group; created if it does not exist. Default: DefaultGroup"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the skeleton asset after the change (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("remove_skeleton_slot"),
		TEXT("Remove a slot name, or a whole slot group (and every slot in it), from a Skeleton. Pass "
			 "exactly one of 'slot_name' or 'group_name'. The engine default group and default slot "
			 "cannot be removed. The engine does not check references: a slot still used by a montage "
			 "or slot node is re-registered when that asset next loads."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveSkeletonSlot),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"), {TEXT("skeleton")})
			.Optional(TEXT("slot_name"), TEXT("string"), TEXT("Slot name to remove"))
			.Optional(TEXT("group_name"), TEXT("string"), TEXT("Slot group to remove (with all its slots)"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the skeleton asset after the change (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("set_slot_group"),
		TEXT("Move an already-registered slot to another slot group on a Skeleton (the group is created "
			 "if missing). Use add_skeleton_slot to register a new slot."),
		FMonolithActionHandler::CreateStatic(&HandleSetSlotGroup),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"), {TEXT("skeleton")})
			.Required(TEXT("slot_name"), TEXT("string"), TEXT("Registered slot name to move"))
			.Required(TEXT("group_name"), TEXT("string"), TEXT("Destination slot group"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the skeleton asset after the change (default false)"))
			.Build());

	// --- Blend profiles ---

	Registry.RegisterAction(TEXT("animation"), TEXT("get_blend_profiles"),
		TEXT("List a Skeleton's blend profiles with mode (time_factor|weight_factor|blend_mask), default "
			 "scale and per-bone entries {bone, scale}. Pass 'profile' to read one profile."),
		FMonolithActionHandler::CreateStatic(&HandleGetBlendProfiles),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"), {TEXT("skeleton")})
			.Optional(TEXT("profile"), TEXT("string"), TEXT("Only return this blend profile"), {TEXT("name")})
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("create_blend_profile"),
		TEXT("Create a new, empty blend profile on a Skeleton. 'mode': time_factor (per-bone transition "
			 "time multiplier, 0..1), weight_factor (per-bone blend weight multiplier, engine default) or "
			 "blend_mask (per-bone alpha for Layered Blend Per Bone; unlisted bones default to 0). "
			 "Fails if a profile with that name exists. Populate it with set_blend_profile_bones."),
		FMonolithActionHandler::CreateStatic(&HandleCreateBlendProfile),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"), {TEXT("skeleton")})
			.Required(TEXT("name"), TEXT("string"), TEXT("New blend profile name"), {TEXT("profile")})
			.Optional(TEXT("mode"), TEXT("string"), TEXT("time_factor | weight_factor | blend_mask (default weight_factor)"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the skeleton asset after the change (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("set_blend_profile_bones"),
		TEXT("Set per-bone scales on an existing blend profile. 'bones' is an array of "
			 "{bone, scale, recurse?, remove?}: recurse applies the scale to every descendant bone too; "
			 "remove:true deletes that bone's entry. An entry whose scale equals the profile default "
			 "(1.0, or 0.0 for blend_mask) is dropped by the engine. Optional 'clear' empties the profile "
			 "first; optional 'mode' changes the profile mode before the bones are applied."),
		FMonolithActionHandler::CreateStatic(&HandleSetBlendProfileBones),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"), {TEXT("skeleton")})
			.Required(TEXT("profile"), TEXT("string"), TEXT("Blend profile name"), {TEXT("name")})
			.Optional(TEXT("bones"), TEXT("array"), TEXT("Array of {bone: string, scale: number, recurse?: bool, remove?: bool}"))
			.Optional(TEXT("clear"), TEXT("boolean"), TEXT("Remove all existing entries before applying 'bones' (default false)"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("Change the profile mode: time_factor | weight_factor | blend_mask"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the skeleton asset after the change (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("remove_blend_profile"),
		TEXT("Remove a blend profile from a Skeleton. Animation Blueprint nodes, blend spaces or "
			 "transitions that referenced it lose the reference."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveBlendProfile),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"), {TEXT("skeleton")})
			.Required(TEXT("profile"), TEXT("string"), TEXT("Blend profile name"), {TEXT("name")})
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the skeleton asset after the change (default false)"))
			.Build());

	// --- Curve metadata ---

	Registry.RegisterAction(TEXT("animation"), TEXT("get_skeleton_curve_metadata"),
		TEXT("Read curve metadata registered on a Skeleton: per curve {name, material, morph_target, "
			 "linked_bones[], max_lod (-1 = all LODs)}. Pass 'curve' or 'curves' to filter."),
		FMonolithActionHandler::CreateStatic(&HandleGetSkeletonCurveMetadata),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"), {TEXT("skeleton")})
			.Optional(TEXT("curve"), TEXT("string"), TEXT("Only return this curve"))
			.Optional(TEXT("curves"), TEXT("array"), TEXT("Only return these curves (array of names)"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("add_skeleton_curve"),
		TEXT("Register one or more curve names in a Skeleton's curve metadata (idempotent; existing "
			 "entries are left untouched). Optional 'material' / 'morph_target' flags are applied to the "
			 "newly added curves only; use set_skeleton_curve_metadata to change existing curves."),
		FMonolithActionHandler::CreateStatic(&HandleAddSkeletonCurve),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"), {TEXT("skeleton")})
			.Optional(TEXT("curve"), TEXT("string"), TEXT("Curve name to add"))
			.Optional(TEXT("curves"), TEXT("array"), TEXT("Curve names to add (array of strings)"))
			.Optional(TEXT("material"), TEXT("boolean"), TEXT("Flag new curves as material curves"))
			.Optional(TEXT("morph_target"), TEXT("boolean"), TEXT("Flag new curves as morph-target curves"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the skeleton asset after the change (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("remove_skeleton_curve"),
		TEXT("Remove one or more curves from a Skeleton's curve metadata. Curve data stored on "
			 "animation sequences is not touched (use remove_curve for that)."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveSkeletonCurve),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"), {TEXT("skeleton")})
			.Optional(TEXT("curve"), TEXT("string"), TEXT("Curve name to remove"))
			.Optional(TEXT("curves"), TEXT("array"), TEXT("Curve names to remove (array of strings)"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the skeleton asset after the change (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("set_skeleton_curve_metadata"),
		TEXT("Edit a Skeleton curve's metadata: 'material' / 'morph_target' flags, 'linked_bones' "
			 "(replaces the list; the curve is culled when none of these bones is in the evaluated LOD; "
			 "[] clears) and 'max_lod' (-1 = all LODs). Fields that are omitted are left unchanged. "
			 "The curve is registered first if missing unless 'create_if_missing' is false."),
		FMonolithActionHandler::CreateStatic(&HandleSetSkeletonCurveMetadata),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"), {TEXT("skeleton")})
			.Required(TEXT("curve"), TEXT("string"), TEXT("Curve name"), {TEXT("curve_name")})
			.Optional(TEXT("material"), TEXT("boolean"), TEXT("Material curve flag"))
			.Optional(TEXT("morph_target"), TEXT("boolean"), TEXT("Morph-target curve flag"))
			.Optional(TEXT("linked_bones"), TEXT("array"), TEXT("Bone names linked to this curve (replaces the existing list; [] clears)"))
			.Optional(TEXT("max_lod"), TEXT("integer"), TEXT("Lowest LOD index at which the curve is evaluated; -1 = all LODs"))
			.Optional(TEXT("create_if_missing"), TEXT("boolean"), TEXT("Register the curve if it is not present (default true)"), TEXT("true"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the skeleton asset after the change (default false)"))
			.Build());
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationSkeletonActions::HandleGetSkeletonSlots(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Err;
	USkeleton* Skeleton = SkelAuthoringLoadSkeleton(Params, Path, Err);
	if (!Skeleton) return FMonolithActionResult::Error(Err);

	TSharedPtr<FJsonObject> Root = SkelAuthoringSlotsToJson(Skeleton);
	Root->SetStringField(TEXT("skeleton_path"), Path);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationSkeletonActions::HandleAddSkeletonSlot(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Err;
	USkeleton* Skeleton = SkelAuthoringLoadSkeleton(Params, Path, Err);
	if (!Skeleton) return FMonolithActionResult::Error(Err);

	const FString SlotStr = Params->GetStringField(TEXT("slot_name")).TrimStartAndEnd();
	if (SlotStr.IsEmpty()) return FMonolithActionResult::Error(TEXT("'slot_name' must be a non-empty string."));
	const FName SlotName(*SlotStr);

	FString GroupStr;
	const bool bHasGroup = Params->TryGetStringField(TEXT("group_name"), GroupStr) && !GroupStr.TrimStartAndEnd().IsEmpty();
	const FName GroupName = bHasGroup ? FName(*GroupStr.TrimStartAndEnd()) : FAnimSlotGroup::DefaultGroupName;

	const bool bAlreadyRegistered = Skeleton->ContainsSlotName(SlotName);
	const FName PreviousGroup = bAlreadyRegistered ? Skeleton->GetSlotGroupName(SlotName) : NAME_None;
	const bool bGroupExisted = Skeleton->FindAnimSlotGroup(GroupName) != nullptr;

	// Already in the requested group (or already registered and no group was requested) -> no-op.
	const bool bNoOp = bAlreadyRegistered && (!bHasGroup || PreviousGroup == GroupName);

	bool bSaved = false;
	if (!bNoOp)
	{
		const FScopedTransaction Transaction(LOCTEXT("AddSkeletonSlot", "Add Skeleton Slot"));
		Skeleton->Modify();
		Skeleton->SetSlotGroupName(SlotName, GroupName); // creates the group when missing
		bSaved = SkelAuthoringFinish(Skeleton, SkelAuthoringWantsSave(Params));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("skeleton_path"), Path);
	Root->SetStringField(TEXT("slot_name"), SlotName.ToString());
	Root->SetStringField(TEXT("group_name"), Skeleton->GetSlotGroupName(SlotName).ToString());
	Root->SetBoolField(TEXT("added"), !bAlreadyRegistered);
	Root->SetBoolField(TEXT("moved"), bAlreadyRegistered && !bNoOp);
	if (bAlreadyRegistered) Root->SetStringField(TEXT("previous_group"), PreviousGroup.ToString());
	Root->SetBoolField(TEXT("group_created"), !bNoOp && !bGroupExisted);
	Root->SetBoolField(TEXT("changed"), !bNoOp);
	Root->SetBoolField(TEXT("saved"), bSaved);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationSkeletonActions::HandleRemoveSkeletonSlot(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Err;
	USkeleton* Skeleton = SkelAuthoringLoadSkeleton(Params, Path, Err);
	if (!Skeleton) return FMonolithActionResult::Error(Err);

	FString SlotStr, GroupStr;
	const bool bHasSlot = Params->TryGetStringField(TEXT("slot_name"), SlotStr) && !SlotStr.TrimStartAndEnd().IsEmpty();
	const bool bHasGroup = Params->TryGetStringField(TEXT("group_name"), GroupStr) && !GroupStr.TrimStartAndEnd().IsEmpty();
	if (bHasSlot == bHasGroup)
	{
		return FMonolithActionResult::Error(TEXT("Pass exactly one of 'slot_name' (remove one slot) or 'group_name' (remove a slot group and all its slots)."));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("skeleton_path"), Path);

	if (bHasSlot)
	{
		const FName SlotName(*SlotStr.TrimStartAndEnd());
		if (SlotName == FAnimSlotGroup::DefaultSlotName)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("'%s' is the engine default slot and cannot be removed."), *SlotName.ToString()));
		}
		if (!Skeleton->ContainsSlotName(SlotName))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Slot '%s' is not registered on %s. Use get_skeleton_slots to list registered slots."), *SlotName.ToString(), *Path));
		}
		const FName GroupName = Skeleton->GetSlotGroupName(SlotName);
		{
			const FScopedTransaction Transaction(LOCTEXT("RemoveSkeletonSlot", "Remove Skeleton Slot"));
			Skeleton->Modify();
			Skeleton->RemoveSlotName(SlotName);
		}
		Root->SetStringField(TEXT("removed_slot"), SlotName.ToString());
		Root->SetStringField(TEXT("group_name"), GroupName.ToString());
	}
	else
	{
		const FName GroupName(*GroupStr.TrimStartAndEnd());
		if (GroupName == FAnimSlotGroup::DefaultGroupName)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("'%s' is the engine default slot group and cannot be removed."), *GroupName.ToString()));
		}
		const FAnimSlotGroup* Group = Skeleton->FindAnimSlotGroup(GroupName);
		// USkeleton::RemoveSlotGroup dereferences the group without a null check — validate first.
		if (!Group)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Slot group '%s' does not exist on %s. Use get_skeleton_slots to list groups."), *GroupName.ToString(), *Path));
		}
		const TArray<FName> RemovedSlots = Group->SlotNames;
		{
			const FScopedTransaction Transaction(LOCTEXT("RemoveSkeletonSlotGroup", "Remove Skeleton Slot Group"));
			Skeleton->Modify();
			Skeleton->RemoveSlotGroup(GroupName);
		}
		Root->SetStringField(TEXT("removed_group"), GroupName.ToString());
		Root->SetArrayField(TEXT("removed_slots"), SkelAuthoringNamesToJson(RemovedSlots));
	}

	Root->SetBoolField(TEXT("saved"), SkelAuthoringFinish(Skeleton, SkelAuthoringWantsSave(Params)));
	Root->SetStringField(TEXT("note"), TEXT("References are not checked: montages or slot nodes that still use a removed slot re-register it when they load."));
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationSkeletonActions::HandleSetSlotGroup(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Err;
	USkeleton* Skeleton = SkelAuthoringLoadSkeleton(Params, Path, Err);
	if (!Skeleton) return FMonolithActionResult::Error(Err);

	const FString SlotStr = Params->GetStringField(TEXT("slot_name")).TrimStartAndEnd();
	const FString GroupStr = Params->GetStringField(TEXT("group_name")).TrimStartAndEnd();
	if (SlotStr.IsEmpty() || GroupStr.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'slot_name' and 'group_name' must both be non-empty strings."));
	}
	const FName SlotName(*SlotStr);
	const FName GroupName(*GroupStr);

	if (!Skeleton->ContainsSlotName(SlotName))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Slot '%s' is not registered on %s. Use add_skeleton_slot to register it."), *SlotName.ToString(), *Path));
	}

	const FName PreviousGroup = Skeleton->GetSlotGroupName(SlotName);
	const bool bGroupExisted = Skeleton->FindAnimSlotGroup(GroupName) != nullptr;
	const bool bChanged = PreviousGroup != GroupName;
	bool bSaved = false;
	if (bChanged)
	{
		const FScopedTransaction Transaction(LOCTEXT("SetSlotGroup", "Set Slot Group"));
		Skeleton->Modify();
		Skeleton->SetSlotGroupName(SlotName, GroupName);
		bSaved = SkelAuthoringFinish(Skeleton, SkelAuthoringWantsSave(Params));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("skeleton_path"), Path);
	Root->SetStringField(TEXT("slot_name"), SlotName.ToString());
	Root->SetStringField(TEXT("previous_group"), PreviousGroup.ToString());
	Root->SetStringField(TEXT("group_name"), Skeleton->GetSlotGroupName(SlotName).ToString());
	Root->SetBoolField(TEXT("group_created"), bChanged && !bGroupExisted);
	Root->SetBoolField(TEXT("changed"), bChanged);
	Root->SetBoolField(TEXT("saved"), bSaved);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Blend profiles
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationSkeletonActions::HandleGetBlendProfiles(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Err;
	USkeleton* Skeleton = SkelAuthoringLoadSkeleton(Params, Path, Err);
	if (!Skeleton) return FMonolithActionResult::Error(Err);

	FString Filter;
	Params->TryGetStringField(TEXT("profile"), Filter);
	Filter.TrimStartAndEndInline();

	TArray<TSharedPtr<FJsonValue>> ProfilesArr;
	if (!Filter.IsEmpty())
	{
		UBlendProfile* Profile = Skeleton->GetBlendProfile(FName(*Filter));
		if (!Profile)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Blend profile '%s' not found on %s. Existing: [%s]"),
				*Filter, *Path, *FString::Join(SkelAuthoringBlendProfileNames(Skeleton), TEXT(", "))));
		}
		ProfilesArr.Add(MakeShared<FJsonValueObject>(SkelAuthoringBlendProfileToJson(Profile)));
	}
	else
	{
		for (const TObjectPtr<UBlendProfile>& Profile : Skeleton->BlendProfiles)
		{
			if (Profile)
			{
				ProfilesArr.Add(MakeShared<FJsonValueObject>(SkelAuthoringBlendProfileToJson(Profile)));
			}
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("skeleton_path"), Path);
	Root->SetNumberField(TEXT("count"), ProfilesArr.Num());
	Root->SetArrayField(TEXT("blend_profiles"), ProfilesArr);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationSkeletonActions::HandleCreateBlendProfile(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Err;
	USkeleton* Skeleton = SkelAuthoringLoadSkeleton(Params, Path, Err);
	if (!Skeleton) return FMonolithActionResult::Error(Err);

	const FString NameStr = Params->GetStringField(TEXT("name")).TrimStartAndEnd();
	if (NameStr.IsEmpty()) return FMonolithActionResult::Error(TEXT("'name' must be a non-empty string."));
	FText InvalidReason;
	if (!FName::IsValidXName(NameStr, INVALID_OBJECTNAME_CHARACTERS, &InvalidReason))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid blend profile name '%s': %s"), *NameStr, *InvalidReason.ToString()));
	}
	const FName ProfileName(*NameStr);

	bool bHasMode = false;
	EBlendProfileMode Mode = EBlendProfileMode::WeightFactor;
	FString ModeStr;
	if (Params->TryGetStringField(TEXT("mode"), ModeStr) && !ModeStr.TrimStartAndEnd().IsEmpty())
	{
		if (!SkelAuthoringParseBlendMode(ModeStr, Mode))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid mode '%s'. Valid: time_factor, weight_factor, blend_mask."), *ModeStr));
		}
		bHasMode = true;
	}

	if (Skeleton->GetBlendProfile(ProfileName))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blend profile '%s' already exists on %s. Use set_blend_profile_bones to edit it."), *NameStr, *Path));
	}
	// CreateNewBlendProfile does NewObject(Skeleton, Name): any other sub-object already holding
	// that name would be replaced in place. Refuse rather than clobber it.
	if (UObject* Existing = StaticFindObjectFast(nullptr, Skeleton, ProfileName))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Name '%s' is already used by a %s sub-object of the skeleton; choose another blend profile name."),
			*NameStr, *Existing->GetClass()->GetName()));
	}

	UBlendProfile* Profile = nullptr;
	{
		const FScopedTransaction Transaction(LOCTEXT("CreateBlendProfile", "Create Blend Profile"));
		Skeleton->Modify();
		Profile = Skeleton->CreateNewBlendProfile(ProfileName); // RF_Public | RF_Transactional
		if (Profile && bHasMode)
		{
			Profile->Modify();
			Profile->Mode = Mode; // empty profile: no entries to re-normalise
		}
	}
	if (!Profile)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("USkeleton::CreateNewBlendProfile returned null for '%s'."), *NameStr));
	}

	TSharedPtr<FJsonObject> Root = SkelAuthoringBlendProfileToJson(Profile);
	Root->SetStringField(TEXT("skeleton_path"), Path);
	Root->SetBoolField(TEXT("created"), true);
	Root->SetBoolField(TEXT("saved"), SkelAuthoringFinish(Skeleton, SkelAuthoringWantsSave(Params)));
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationSkeletonActions::HandleSetBlendProfileBones(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Err;
	USkeleton* Skeleton = SkelAuthoringLoadSkeleton(Params, Path, Err);
	if (!Skeleton) return FMonolithActionResult::Error(Err);

	const FString ProfileStr = Params->GetStringField(TEXT("profile")).TrimStartAndEnd();
	UBlendProfile* Profile = ProfileStr.IsEmpty() ? nullptr : Skeleton->GetBlendProfile(FName(*ProfileStr));
	if (!Profile)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blend profile '%s' not found on %s. Existing: [%s]. Use create_blend_profile first."),
			*ProfileStr, *Path, *FString::Join(SkelAuthoringBlendProfileNames(Skeleton), TEXT(", "))));
	}

	bool bHasMode = false;
	EBlendProfileMode NewMode = Profile->GetMode();
	FString ModeStr;
	if (Params->TryGetStringField(TEXT("mode"), ModeStr) && !ModeStr.TrimStartAndEnd().IsEmpty())
	{
		if (!SkelAuthoringParseBlendMode(ModeStr, NewMode))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid mode '%s'. Valid: time_factor, weight_factor, blend_mask."), *ModeStr));
		}
		bHasMode = true;
	}

	bool bClear = false;
	Params->TryGetBoolField(TEXT("clear"), bClear);

	// Parse + validate all bone entries before mutating anything.
	struct FSkelAuthoringBoneOp
	{
		FName Bone;
		int32 BoneIndex = INDEX_NONE;
		float Scale = 1.f;
		bool bRecurse = false;
		bool bRemove = false;
	};
	TArray<FSkelAuthoringBoneOp> Ops;

	const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
	const TArray<TSharedPtr<FJsonValue>>* BonesArr = nullptr;
	if (Params->TryGetArrayField(TEXT("bones"), BonesArr))
	{
		for (int32 i = 0; i < BonesArr->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>* EntryObj = nullptr;
			if (!(*BonesArr)[i].IsValid() || !(*BonesArr)[i]->TryGetObject(EntryObj) || !EntryObj || !EntryObj->IsValid())
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("bones[%d] must be an object {bone, scale, recurse?, remove?}."), i));
			}
			FSkelAuthoringBoneOp Op;
			FString BoneStr;
			if (!(*EntryObj)->TryGetStringField(TEXT("bone"), BoneStr) || BoneStr.TrimStartAndEnd().IsEmpty())
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("bones[%d] requires a non-empty 'bone' field."), i));
			}
			Op.Bone = FName(*BoneStr.TrimStartAndEnd());
			Op.BoneIndex = RefSkel.FindBoneIndex(Op.Bone);
			if (Op.BoneIndex == INDEX_NONE)
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("bones[%d]: bone '%s' not found in skeleton %s."), i, *Op.Bone.ToString(), *Path));
			}
			(*EntryObj)->TryGetBoolField(TEXT("recurse"), Op.bRecurse);
			(*EntryObj)->TryGetBoolField(TEXT("remove"), Op.bRemove);
			double ScaleD = 0.0;
			if ((*EntryObj)->TryGetNumberField(TEXT("scale"), ScaleD))
			{
				Op.Scale = static_cast<float>(ScaleD);
			}
			else if (!Op.bRemove)
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("bones[%d] ('%s') requires a numeric 'scale' (or remove:true)."), i, *Op.Bone.ToString()));
			}
			Ops.Add(Op);
		}
	}

	if (Ops.Num() == 0 && !bClear && !bHasMode)
	{
		return FMonolithActionResult::Error(TEXT("Nothing to do: provide 'bones', 'clear':true and/or 'mode'."));
	}

	TArray<TSharedPtr<FJsonValue>> AppliedArr;
	{
		const FScopedTransaction Transaction(LOCTEXT("SetBlendProfileBones", "Set Blend Profile Bones"));
		Profile->SetFlags(RF_Transactional);
		Profile->Modify();

		if (bClear)
		{
			Profile->ProfileEntries.Reset();
		}
		if (bHasMode && NewMode != Profile->GetMode())
		{
			SkelAuthoringApplyBlendMode(Profile, NewMode);
		}

		for (const FSkelAuthoringBoneOp& Op : Ops)
		{
			TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
			A->SetStringField(TEXT("bone"), Op.Bone.ToString());
			if (Op.bRemove)
			{
				Profile->RemoveEntry(Op.BoneIndex);
				if (Op.bRecurse)
				{
					for (int32 Child = Op.BoneIndex + 1; Child < RefSkel.GetNum(); ++Child)
					{
						if (RefSkel.BoneIsChildOf(Child, Op.BoneIndex))
						{
							Profile->RemoveEntry(Child);
						}
					}
				}
				A->SetBoolField(TEXT("removed"), true);
			}
			else
			{
				Profile->SetBoneBlendScale(Op.BoneIndex, Op.Scale, Op.bRecurse, /*bCreate=*/true);
				A->SetNumberField(TEXT("scale"), Op.Scale);
			}
			A->SetBoolField(TEXT("recurse"), Op.bRecurse);
			AppliedArr.Add(MakeShared<FJsonValueObject>(A));
		}
	}

	TArray<FString> Warnings;
	if (Profile->GetMode() == EBlendProfileMode::TimeFactor)
	{
		for (const FSkelAuthoringBoneOp& Op : Ops)
		{
			if (!Op.bRemove && (Op.Scale < 0.f || Op.Scale > 1.f))
			{
				Warnings.Add(FString::Printf(TEXT("'%s' scale %.3f is outside 0..1; time_factor profiles are clamped to that range at runtime."),
					*Op.Bone.ToString(), Op.Scale));
			}
		}
	}

	TSharedPtr<FJsonObject> Root = SkelAuthoringBlendProfileToJson(Profile);
	Root->SetStringField(TEXT("skeleton_path"), Path);
	Root->SetArrayField(TEXT("applied"), AppliedArr);
	Root->SetBoolField(TEXT("cleared"), bClear);
	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> W;
		for (const FString& S : Warnings) W.Add(MakeShared<FJsonValueString>(S));
		Root->SetArrayField(TEXT("warnings"), W);
	}
	Root->SetBoolField(TEXT("saved"), SkelAuthoringFinish(Skeleton, SkelAuthoringWantsSave(Params)));
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationSkeletonActions::HandleRemoveBlendProfile(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Err;
	USkeleton* Skeleton = SkelAuthoringLoadSkeleton(Params, Path, Err);
	if (!Skeleton) return FMonolithActionResult::Error(Err);

	const FString ProfileStr = Params->GetStringField(TEXT("profile")).TrimStartAndEnd();
	UBlendProfile* Profile = ProfileStr.IsEmpty() ? nullptr : Skeleton->GetBlendProfile(FName(*ProfileStr));
	if (!Profile)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blend profile '%s' not found on %s. Existing: [%s]"),
			*ProfileStr, *Path, *FString::Join(SkelAuthoringBlendProfileNames(Skeleton), TEXT(", "))));
	}

	{
		// Mirrors FEditableSkeleton::RemoveBlendProfile. The removed object is also renamed to a
		// unique name first so a later create_blend_profile with the same name does not collide
		// with the pending-GC sub-object.
		const FScopedTransaction Transaction(LOCTEXT("RemoveBlendProfile", "Remove Blend Profile"));
		Skeleton->Modify();
		Profile->SetFlags(RF_Transactional);
		Profile->Modify();
		Skeleton->BlendProfiles.Remove(Profile);
		const FName Graveyard = MakeUniqueObjectName(Skeleton, UBlendProfile::StaticClass(),
			FName(*FString::Printf(TEXT("%s_REMOVED"), *ProfileStr)));
		Profile->Rename(*Graveyard.ToString(), Skeleton, REN_DontCreateRedirectors);
		Profile->MarkAsGarbage();
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("skeleton_path"), Path);
	Root->SetStringField(TEXT("removed_profile"), ProfileStr);
	Root->SetNumberField(TEXT("remaining_count"), Skeleton->BlendProfiles.Num());
	Root->SetBoolField(TEXT("saved"), SkelAuthoringFinish(Skeleton, SkelAuthoringWantsSave(Params)));
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Curve metadata
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationSkeletonActions::HandleGetSkeletonCurveMetadata(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Err;
	USkeleton* Skeleton = SkelAuthoringLoadSkeleton(Params, Path, Err);
	if (!Skeleton) return FMonolithActionResult::Error(Err);

	TArray<FName> Filter;
	FString IgnoredErr;
	const bool bFiltered = Params->HasField(TEXT("curve")) || Params->HasField(TEXT("curves"));
	if (bFiltered && !SkelAuthoringCollectNames(Params, TEXT("curve"), TEXT("curves"), Filter, IgnoredErr))
	{
		return FMonolithActionResult::Error(IgnoredErr);
	}

	TArray<TSharedPtr<FJsonValue>> CurvesArr;
	TArray<FString> Missing;
	if (bFiltered)
	{
		for (const FName& Name : Filter)
		{
			if (const FCurveMetaData* MD = Skeleton->GetCurveMetaData(Name))
			{
				CurvesArr.Add(MakeShared<FJsonValueObject>(SkelAuthoringCurveToJson(Name, *MD)));
			}
			else
			{
				Missing.Add(Name.ToString());
			}
		}
	}
	else
	{
		Skeleton->ForEachCurveMetaData([&CurvesArr](FName CurveName, const FCurveMetaData& MD)
		{
			CurvesArr.Add(MakeShared<FJsonValueObject>(SkelAuthoringCurveToJson(CurveName, MD)));
		});
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("skeleton_path"), Path);
	Root->SetNumberField(TEXT("count"), CurvesArr.Num());
	Root->SetArrayField(TEXT("curves"), CurvesArr);
	if (Missing.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> M;
		for (const FString& S : Missing) M.Add(MakeShared<FJsonValueString>(S));
		Root->SetArrayField(TEXT("missing"), M);
	}
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationSkeletonActions::HandleAddSkeletonCurve(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Err;
	USkeleton* Skeleton = SkelAuthoringLoadSkeleton(Params, Path, Err);
	if (!Skeleton) return FMonolithActionResult::Error(Err);

	TArray<FName> Names;
	if (!SkelAuthoringCollectNames(Params, TEXT("curve"), TEXT("curves"), Names, Err))
	{
		return FMonolithActionResult::Error(Err);
	}

	bool bMaterial = false, bMorph = false;
	const bool bHasMaterial = Params->TryGetBoolField(TEXT("material"), bMaterial);
	const bool bHasMorph = Params->TryGetBoolField(TEXT("morph_target"), bMorph);

	TArray<FName> Added, Existing;
	{
		FScopedTransaction Transaction(LOCTEXT("AddSkeletonCurve", "Add Skeleton Curve"));
		Skeleton->Modify();
		for (const FName& Name : Names)
		{
			// bTransact=false: we already own the outer transaction.
			if (Skeleton->AddCurveMetaData(Name, /*bTransact=*/false))
			{
				Added.Add(Name);
				if (bHasMaterial) Skeleton->SetCurveMetaDataMaterial(Name, bMaterial);
				if (bHasMorph) Skeleton->SetCurveMetaDataMorphTarget(Name, bMorph);
			}
			else
			{
				Existing.Add(Name);
			}
		}
		if (Added.Num() == 0)
		{
			// Nothing changed — drop the empty undo entry.
			Transaction.Cancel();
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("skeleton_path"), Path);
	Root->SetArrayField(TEXT("added"), SkelAuthoringNamesToJson(Added));
	Root->SetArrayField(TEXT("already_present"), SkelAuthoringNamesToJson(Existing));
	Root->SetNumberField(TEXT("total_curves"), Skeleton->GetNumCurveMetaData());
	Root->SetBoolField(TEXT("saved"), Added.Num() > 0 ? SkelAuthoringFinish(Skeleton, SkelAuthoringWantsSave(Params)) : false);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationSkeletonActions::HandleRemoveSkeletonCurve(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Err;
	USkeleton* Skeleton = SkelAuthoringLoadSkeleton(Params, Path, Err);
	if (!Skeleton) return FMonolithActionResult::Error(Err);

	TArray<FName> Names;
	if (!SkelAuthoringCollectNames(Params, TEXT("curve"), TEXT("curves"), Names, Err))
	{
		return FMonolithActionResult::Error(Err);
	}

	TArray<FName> Removed, NotFound;
	for (const FName& Name : Names)
	{
		(Skeleton->GetCurveMetaData(Name) ? Removed : NotFound).Add(Name);
	}

	if (Removed.Num() > 0)
	{
		const FScopedTransaction Transaction(LOCTEXT("RemoveSkeletonCurve", "Remove Skeleton Curve"));
		Skeleton->Modify();
		Skeleton->RemoveCurveMetaData(TArrayView<FName>(Removed));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("skeleton_path"), Path);
	Root->SetArrayField(TEXT("removed"), SkelAuthoringNamesToJson(Removed));
	Root->SetArrayField(TEXT("not_found"), SkelAuthoringNamesToJson(NotFound));
	Root->SetNumberField(TEXT("total_curves"), Skeleton->GetNumCurveMetaData());
	Root->SetBoolField(TEXT("saved"), Removed.Num() > 0 ? SkelAuthoringFinish(Skeleton, SkelAuthoringWantsSave(Params)) : false);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithAnimationSkeletonActions::HandleSetSkeletonCurveMetadata(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Err;
	USkeleton* Skeleton = SkelAuthoringLoadSkeleton(Params, Path, Err);
	if (!Skeleton) return FMonolithActionResult::Error(Err);

	const FString CurveStr = Params->GetStringField(TEXT("curve")).TrimStartAndEnd();
	if (CurveStr.IsEmpty()) return FMonolithActionResult::Error(TEXT("'curve' must be a non-empty string."));
	const FName CurveName(*CurveStr);

	bool bCreateIfMissing = true;
	Params->TryGetBoolField(TEXT("create_if_missing"), bCreateIfMissing);

	bool bMaterial = false, bMorph = false;
	const bool bHasMaterial = Params->TryGetBoolField(TEXT("material"), bMaterial);
	const bool bHasMorph = Params->TryGetBoolField(TEXT("morph_target"), bMorph);

	// linked_bones: validate every name against the reference skeleton up-front.
	const TArray<TSharedPtr<FJsonValue>>* BonesArr = nullptr;
	const bool bHasBones = Params->TryGetArrayField(TEXT("linked_bones"), BonesArr);
	TArray<FBoneReference> BoneRefs;
	if (bHasBones)
	{
		const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
		for (int32 i = 0; i < BonesArr->Num(); ++i)
		{
			FString BoneStr;
			if (!(*BonesArr)[i].IsValid() || !(*BonesArr)[i]->TryGetString(BoneStr) || BoneStr.TrimStartAndEnd().IsEmpty())
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("linked_bones[%d] must be a non-empty bone name string."), i));
			}
			const FName BoneName(*BoneStr.TrimStartAndEnd());
			if (RefSkel.FindBoneIndex(BoneName) == INDEX_NONE)
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("linked_bones[%d]: bone '%s' not found in skeleton %s."), i, *BoneName.ToString(), *Path));
			}
			BoneRefs.AddUnique(FBoneReference(BoneName));
		}
	}

	int32 MaxLodIn = 0;
	const bool bHasMaxLod = Params->TryGetNumberField(TEXT("max_lod"), MaxLodIn);
	if (bHasMaxLod && (MaxLodIn < -1 || MaxLodIn > 254))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("'max_lod' must be -1 (all LODs) or 0..254, got %d."), MaxLodIn));
	}

	if (!bHasMaterial && !bHasMorph && !bHasBones && !bHasMaxLod)
	{
		return FMonolithActionResult::Error(TEXT("Nothing to set: provide at least one of 'material', 'morph_target', 'linked_bones', 'max_lod'."));
	}

	const bool bExisted = Skeleton->GetCurveMetaData(CurveName) != nullptr;
	if (!bExisted && !bCreateIfMissing)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Curve '%s' is not registered on %s and create_if_missing is false. Use add_skeleton_curve or omit create_if_missing."),
			*CurveStr, *Path));
	}

	{
		FScopedTransaction Transaction(LOCTEXT("SetSkeletonCurveMetadata", "Set Skeleton Curve Metadata"));
		Skeleton->Modify();
		if (!bExisted)
		{
			Skeleton->AddCurveMetaData(CurveName, /*bTransact=*/false);
		}
		if (bHasMaterial) Skeleton->SetCurveMetaDataMaterial(CurveName, bMaterial);
		if (bHasMorph) Skeleton->SetCurveMetaDataMorphTarget(CurveName, bMorph);

		if (bHasBones || bHasMaxLod)
		{
			// USkeleton has no bone-links wrapper; go through the curve-metadata asset user data
			// (created by AddCurveMetaData above if it did not exist). SetCurveMetaDataBoneLinks is
			// virtual, so it resolves through the vtable despite UAnimCurveMetaData being MinimalAPI.
			UAnimCurveMetaData* MetaObj = Skeleton->GetAssetUserData<UAnimCurveMetaData>();
			const FCurveMetaData* Current = Skeleton->GetCurveMetaData(CurveName);
			if (!MetaObj || !Current)
			{
				Transaction.Cancel();
				return FMonolithActionResult::Error(FString::Printf(TEXT("Curve metadata object missing on %s after registering '%s'."), *Path, *CurveStr));
			}
			const TArray<FBoneReference> FinalBones = bHasBones ? BoneRefs : Current->LinkedBones;
			const uint8 FinalLod = bHasMaxLod ? (MaxLodIn < 0 ? uint8(0xFF) : static_cast<uint8>(MaxLodIn)) : Current->MaxLOD;
			IInterface_AnimCurveMetaData* MetaInterface = MetaObj;
			MetaInterface->SetCurveMetaDataBoneLinks(CurveName, FinalBones, FinalLod, Skeleton);
		}
	}

	const FCurveMetaData* Final = Skeleton->GetCurveMetaData(CurveName);
	TSharedPtr<FJsonObject> Root = Final ? SkelAuthoringCurveToJson(CurveName, *Final) : MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("skeleton_path"), Path);
	Root->SetBoolField(TEXT("created"), !bExisted);
	Root->SetBoolField(TEXT("saved"), SkelAuthoringFinish(Skeleton, SkelAuthoringWantsSave(Params)));
	return FMonolithActionResult::Success(Root);
}

#undef LOCTEXT_NAMESPACE
