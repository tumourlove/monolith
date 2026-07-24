#include "MonolithSkeletonRetargetActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h" // MonolithKeyToString (5.7/5.8 JSON key shim)
#include "Reflection/MonolithReflectionReader.h"

#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h" // GEditor->BeginTransaction / EndTransaction

// IK Rig (T2-1)
#include "Rig/IKRigDefinition.h"                 // UIKRigDefinition
#include "RigEditor/IKRigController.h"           // UIKRigController (IKRigEditor module)
#include "Rig/Solvers/IKRigSolverBase.h"         // FIKRigSolverBase, FIKRigBoneSettingsBase

// ---------------------------------------------------------------------------
// Mode string <-> namespaced enum (parse/echo by NAME — never by raw int).
//
// EBoneTranslationRetargetingMode is `namespace { enum Type : int }`
// (Skeleton.h:69-86). All five members verified live (UE 5.7):
//   Animation, Skeleton, AnimationScaled, AnimationRelative, OrientAndScale.
// ---------------------------------------------------------------------------

bool FMonolithSkeletonRetargetActions::ParseTranslationRetargetMode(
	const FString& In, EBoneTranslationRetargetingMode::Type& Out)
{
	const FString S = In.TrimStartAndEnd();
	if (S.Equals(TEXT("Animation"), ESearchCase::IgnoreCase))
	{
		Out = EBoneTranslationRetargetingMode::Animation; return true;
	}
	if (S.Equals(TEXT("Skeleton"), ESearchCase::IgnoreCase))
	{
		Out = EBoneTranslationRetargetingMode::Skeleton; return true;
	}
	if (S.Equals(TEXT("AnimationScaled"), ESearchCase::IgnoreCase))
	{
		Out = EBoneTranslationRetargetingMode::AnimationScaled; return true;
	}
	if (S.Equals(TEXT("AnimationRelative"), ESearchCase::IgnoreCase))
	{
		Out = EBoneTranslationRetargetingMode::AnimationRelative; return true;
	}
	if (S.Equals(TEXT("OrientAndScale"), ESearchCase::IgnoreCase))
	{
		Out = EBoneTranslationRetargetingMode::OrientAndScale; return true;
	}
	return false;
}

namespace
{
	const TCHAR* TranslationRetargetModeToString(EBoneTranslationRetargetingMode::Type Mode)
	{
		switch (Mode)
		{
			case EBoneTranslationRetargetingMode::Animation:         return TEXT("Animation");
			case EBoneTranslationRetargetingMode::Skeleton:          return TEXT("Skeleton");
			case EBoneTranslationRetargetingMode::AnimationScaled:   return TEXT("AnimationScaled");
			case EBoneTranslationRetargetingMode::AnimationRelative: return TEXT("AnimationRelative");
			case EBoneTranslationRetargetingMode::OrientAndScale:    return TEXT("OrientAndScale");
			default:                                                 return TEXT("Animation");
		}
	}

	// biped_locomotion preset — role-keyed mode map.
	// Applied through the GENERIC setter (this is a thin convenience layer):
	//   root   -> Animation       (let the root translate freely)
	//   pelvis -> AnimationScaled  (scale pelvis height to target proportions)
	//   ik_*   -> Animation        (IK targets must follow source translation)
	//   rest   -> Skeleton         (fixed translation; rotation-only retarget)
	//
	// Matched case-insensitively against the bone name. A name is "pelvis" if it
	// equals/contains "pelvis" or "hips"; "root" if it equals "root"; IK if it
	// begins with "ik_". First match wins in that priority order.
	EBoneTranslationRetargetingMode::Type BipedLocomotionModeForBone(const FString& BoneNameLower)
	{
		if (BoneNameLower.Equals(TEXT("root"), ESearchCase::IgnoreCase))
		{
			return EBoneTranslationRetargetingMode::Animation;
		}
		if (BoneNameLower.Contains(TEXT("pelvis"), ESearchCase::IgnoreCase) ||
			BoneNameLower.Contains(TEXT("hips"), ESearchCase::IgnoreCase))
		{
			return EBoneTranslationRetargetingMode::AnimationScaled;
		}
		if (BoneNameLower.StartsWith(TEXT("ik_"), ESearchCase::IgnoreCase))
		{
			return EBoneTranslationRetargetingMode::Animation;
		}
		return EBoneTranslationRetargetingMode::Skeleton;
	}

	// -----------------------------------------------------------------------
	// T2-1 helpers — IK Rig per-solver per-bone settings (reflective).
	//
	// The concrete bone-settings struct is solver-specific. We get the live
	// struct memory + its concrete UScriptStruct from the solver, then walk
	// the struct's FProperty schema to read/write fields by name. This mirrors
	// the engine's own access path (IKRigEditorController.cpp:972-1016).
	// -----------------------------------------------------------------------

	// The base FIKRigBoneSettingsBase carries a single bookkeeping field, `Bone`
	// (IKRigSolverBase.h:52), which is identity metadata, not a user-editable
	// setting. We never let the caller overwrite it and we omit it from echoes.
	bool IsBaseBookkeepingField(const FProperty* Prop)
	{
		return Prop && Prop->GetFName() == FName(TEXT("Bone"));
	}

	// Reflectively serialise every non-bookkeeping field of a concrete bone-
	// settings struct into a flat JSON object keyed by property name.
	TSharedPtr<FJsonObject> BoneSettingsToJson(
		const UScriptStruct* ConcreteType, const void* StructMemory, const UObject* Owner)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!ConcreteType || !StructMemory)
		{
			return Out;
		}

		for (TFieldIterator<FProperty> It(ConcreteType); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop || IsBaseBookkeepingField(Prop))
			{
				continue;
			}
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(StructMemory);
			Out->SetField(Prop->GetName(),
				FMonolithReflectionReader::PropertyToJsonValue(Prop, ValuePtr, Owner));
		}
		return Out;
	}

	// Solver-type label for echoes. GetNiceName() is WITH_EDITOR-only on the
	// solver; fall back to the concrete UStruct name when unavailable.
	FString SolverDisplayName(const UScriptStruct* SolverType)
	{
		return SolverType ? SolverType->GetName() : FString(TEXT("(unknown)"));
	}
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithSkeletonRetargetActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("animation"), TEXT("set_bone_translation_retargeting"),
		TEXT("Set per-bone translation retargeting mode on a Skeleton (legacy USkeleton retargeting). "
			 "Controls how each bone's animated translation is interpreted when anims authored for one "
			 "skeleton play on a differently-proportioned one. Provide 'entries' as a list of "
			 "{bone, mode} (mode in Animation|Skeleton|AnimationScaled|AnimationRelative|OrientAndScale), "
			 "and/or set 'preset':'biped_locomotion' to apply a role-keyed map "
			 "(root=Animation, pelvis=AnimationScaled, ik_*=Animation, rest=Skeleton) across all bones. "
			 "Explicit 'entries' override the preset for the bones they name. 'recursive':true also "
			 "applies each entry's mode to that bone's children (bChildrenToo)."),
		FMonolithActionHandler::CreateStatic(&HandleSetBoneTranslationRetargeting),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"))
			.Optional(TEXT("entries"), TEXT("array"),
				TEXT("Array of {bone, mode} objects. mode: Animation|Skeleton|AnimationScaled|AnimationRelative|OrientAndScale"))
			.Optional(TEXT("preset"), TEXT("string"),
				TEXT("Named preset to apply across all bones before entries. Supported: biped_locomotion"))
			.Optional(TEXT("recursive"), TEXT("boolean"),
				TEXT("If true, also apply each entry's mode to the bone's children (bChildrenToo). Default false."))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("get_bone_translation_retargeting"),
		TEXT("Read the current per-bone translation retargeting mode for a Skeleton. Returns one entry "
			 "per bone {bone, mode} (mode by name). Optionally pass 'bones' (array of names) to read only "
			 "those bones."),
		FMonolithActionHandler::CreateStatic(&HandleGetBoneTranslationRetargeting),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"))
			.Optional(TEXT("bones"), TEXT("array"),
				TEXT("Optional list of bone names to read. Omit to read all bones."))
			.Build());

	// --- T2-1: IK-Rig per-solver per-bone settings ---

	Registry.RegisterAction(TEXT("animation"), TEXT("set_ik_rig_bone_settings"),
		TEXT("Set per-solver, per-bone settings on an IK Rig (UE 5.6+ struct-solver model). Per-bone "
			 "settings are SOLVER-SCOPED and the concrete fields are solver-specific (e.g. FullBodyIK "
			 "exposes RotationStiffness / per-axis limits). Provide 'ik_rig_path', 'bone_name', and a "
			 "'settings' object of {field: value} pairs written reflectively onto the concrete bone-"
			 "settings struct (scalars/enums via ImportText: numbers as numbers, enums/strings as the "
			 "display name, structs as ImportText e.g. \"(X=1.0,Y=2.0,Z=3.0)\"). By default the settings "
			 "are applied to EVERY solver in the stack that accepts the bone; pass 'solver_index' to "
			 "target one solver only. The bone-setting entry is created automatically if absent "
			 "(AddBoneSetting). Echoes the fields applied per solver."),
		FMonolithActionHandler::CreateStatic(&HandleSetIkRigBoneSettings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("ik_rig_path"), TEXT("IK Rig (UIKRigDefinition) asset path"))
			.Required(TEXT("bone_name"), TEXT("string"), TEXT("Bone to set settings on"))
			.Required(TEXT("settings"), TEXT("object"),
				TEXT("{field: value} map written reflectively onto the concrete bone-settings struct. "
					 "Fields are solver-specific — use get_ik_rig_bone_settings to discover the available fields."))
			.Optional(TEXT("solver_index"), TEXT("integer"),
				TEXT("Apply to this solver only (0-based). Omit to apply to every solver that accepts the bone."))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("get_ik_rig_bone_settings"),
		TEXT("Read per-solver, per-bone settings from an IK Rig (UE 5.6+ struct-solver model). Returns, "
			 "for each solver in the stack, the bone's concrete settings struct serialised to JSON "
			 "(fields are solver-specific). Pass 'bone_name' to read one bone; omit it to enumerate every "
			 "bone that has settings in any solver. Pass 'solver_index' to restrict to one solver. "
			 "Read-only — no transaction."),
		FMonolithActionHandler::CreateStatic(&HandleGetIkRigBoneSettings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("ik_rig_path"), TEXT("IK Rig (UIKRigDefinition) asset path"))
			.Optional(TEXT("bone_name"), TEXT("string"),
				TEXT("Bone to read. Omit to enumerate every bone that has settings in any solver."))
			.Optional(TEXT("solver_index"), TEXT("integer"),
				TEXT("Read this solver only (0-based). Omit to read across all solvers."))
			.Build());
}

// ---------------------------------------------------------------------------
// T1-R4: set_bone_translation_retargeting
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithSkeletonRetargetActions::HandleSetBoneTranslationRetargeting(
	const TSharedPtr<FJsonObject>& Params)
{
	const FString SkeletonPath = Params->GetStringField(TEXT("skeleton_path"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(SkeletonPath);
	if (!Skeleton)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath));
	}

	const FString Preset = Params->GetStringField(TEXT("preset"));
	const bool bRecursive = Params->HasField(TEXT("recursive")) && Params->GetBoolField(TEXT("recursive"));

	// Validate the preset name up-front (only biped_locomotion supported in v1).
	const bool bHasPreset = !Preset.IsEmpty();
	if (bHasPreset && !Preset.Equals(TEXT("biped_locomotion"), ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown preset '%s'. Supported presets: biped_locomotion"), *Preset));
	}

	// Parse + validate explicit entries before mutating anything.
	struct FBoneModeEntry
	{
		FString BoneName;
		int32 BoneIndex;
		EBoneTranslationRetargetingMode::Type Mode;
	};
	TArray<FBoneModeEntry> ResolvedEntries;

	const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
	const int32 NumBones = RefSkel.GetNum();

	const TArray<TSharedPtr<FJsonValue>>* EntriesArr = nullptr;
	if (Params->TryGetArrayField(TEXT("entries"), EntriesArr))
	{
		for (const TSharedPtr<FJsonValue>& Val : *EntriesArr)
		{
			const TSharedPtr<FJsonObject>* EntryObj = nullptr;
			if (!Val->TryGetObject(EntryObj) || !EntryObj->IsValid())
			{
				return FMonolithActionResult::Error(TEXT("Each item in 'entries' must be an object {bone, mode}."));
			}

			FString BoneName;
			if (!(*EntryObj)->TryGetStringField(TEXT("bone"), BoneName) || BoneName.IsEmpty())
			{
				return FMonolithActionResult::Error(TEXT("Each entry requires a non-empty 'bone' field."));
			}

			FString ModeStr;
			if (!(*EntryObj)->TryGetStringField(TEXT("mode"), ModeStr) || ModeStr.IsEmpty())
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Entry for bone '%s' requires a non-empty 'mode' field."), *BoneName));
			}

			EBoneTranslationRetargetingMode::Type Mode;
			if (!ParseTranslationRetargetMode(ModeStr, Mode))
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Invalid mode '%s' for bone '%s'. Valid: Animation, Skeleton, AnimationScaled, "
						 "AnimationRelative, OrientAndScale"), *ModeStr, *BoneName));
			}

			const int32 BoneIndex = RefSkel.FindBoneIndex(FName(*BoneName));
			if (BoneIndex == INDEX_NONE)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Bone not found in skeleton: %s"), *BoneName));
			}

			ResolvedEntries.Add({BoneName, BoneIndex, Mode});
		}
	}

	if (!bHasPreset && ResolvedEntries.Num() == 0)
	{
		return FMonolithActionResult::Error(
			TEXT("Provide 'entries' (a list of {bone, mode}) and/or 'preset':'biped_locomotion'."));
	}

	// Mutate. The preset is applied first (across every bone via the generic
	// setter); explicit entries are applied afterwards so they override the
	// preset for the bones they name.
	GEditor->BeginTransaction(FText::FromString(TEXT("Set Bone Translation Retargeting")));
	Skeleton->Modify();

	int32 PresetBonesSet = 0;
	if (bHasPreset)
	{
		for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
		{
			const FString BoneName = RefSkel.GetBoneName(BoneIndex).ToString();
			const EBoneTranslationRetargetingMode::Type Mode = BipedLocomotionModeForBone(BoneName);
			// Preset never uses bChildrenToo — it sets every bone explicitly.
			Skeleton->SetBoneTranslationRetargetingMode(BoneIndex, Mode, /*bChildrenToo=*/false);
			++PresetBonesSet;
		}
	}

	TArray<TSharedPtr<FJsonValue>> AppliedArr;
	for (const FBoneModeEntry& Entry : ResolvedEntries)
	{
		Skeleton->SetBoneTranslationRetargetingMode(Entry.BoneIndex, Entry.Mode, bRecursive);

		TSharedPtr<FJsonObject> AppliedObj = MakeShared<FJsonObject>();
		AppliedObj->SetStringField(TEXT("bone"), Entry.BoneName);
		AppliedObj->SetStringField(TEXT("mode"), TranslationRetargetModeToString(Entry.Mode));
		AppliedArr.Add(MakeShared<FJsonValueObject>(AppliedObj));
	}

	GEditor->EndTransaction();
	Skeleton->MarkPackageDirty(); // dirty is not transactional state — set after EndTransaction

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("skeleton"), SkeletonPath);
	Root->SetBoolField(TEXT("recursive"), bRecursive);
	if (bHasPreset)
	{
		Root->SetStringField(TEXT("preset"), TEXT("biped_locomotion"));
		Root->SetNumberField(TEXT("preset_bones_set"), PresetBonesSet);
	}
	Root->SetArrayField(TEXT("entries_applied"), AppliedArr);
	Root->SetNumberField(TEXT("bone_count"), NumBones);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// T1-R4: get_bone_translation_retargeting
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithSkeletonRetargetActions::HandleGetBoneTranslationRetargeting(
	const TSharedPtr<FJsonObject>& Params)
{
	const FString SkeletonPath = Params->GetStringField(TEXT("skeleton_path"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(SkeletonPath);
	if (!Skeleton)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath));
	}

	const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
	const int32 NumBones = RefSkel.GetNum();

	// Optional bone-name filter.
	TArray<FString> RequestedBones;
	const TArray<TSharedPtr<FJsonValue>>* BonesArr = nullptr;
	if (Params->TryGetArrayField(TEXT("bones"), BonesArr))
	{
		for (const TSharedPtr<FJsonValue>& Val : *BonesArr)
		{
			RequestedBones.Add(Val->AsString());
		}
	}

	TArray<TSharedPtr<FJsonValue>> EntriesArr;
	TArray<FString> NotFound;

	auto EmitBone = [&](int32 BoneIndex)
	{
		// GetBoneTranslationRetargetingMode reads BoneTree[BoneTreeIdx]; the
		// skeleton's BoneTree is index-parallel with the reference skeleton.
		const EBoneTranslationRetargetingMode::Type Mode =
			Skeleton->GetBoneTranslationRetargetingMode(BoneIndex);

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("bone"), RefSkel.GetBoneName(BoneIndex).ToString());
		Obj->SetStringField(TEXT("mode"), TranslationRetargetModeToString(Mode));
		EntriesArr.Add(MakeShared<FJsonValueObject>(Obj));
	};

	if (RequestedBones.Num() > 0)
	{
		for (const FString& BoneName : RequestedBones)
		{
			const int32 BoneIndex = RefSkel.FindBoneIndex(FName(*BoneName));
			if (BoneIndex == INDEX_NONE)
			{
				NotFound.Add(BoneName);
				continue;
			}
			EmitBone(BoneIndex);
		}
	}
	else
	{
		for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
		{
			EmitBone(BoneIndex);
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("skeleton"), SkeletonPath);
	Root->SetNumberField(TEXT("bone_count"), NumBones);
	Root->SetArrayField(TEXT("entries"), EntriesArr);
	if (NotFound.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> NotFoundArr;
		for (const FString& NF : NotFound)
		{
			NotFoundArr.Add(MakeShared<FJsonValueString>(NF));
		}
		Root->SetArrayField(TEXT("not_found"), NotFoundArr);
	}
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// T2-1: set_ik_rig_bone_settings
//
// Per-solver, per-bone settings on a UIKRigDefinition (UE 5.6+ struct solvers).
// The concrete bone-settings struct is solver-specific; fields are written
// reflectively. Access path matches the engine's FIKRigEditorController:
//   GetController -> GetSolverAtIndex(idx) -> FIKRigSolverBase*
//     -> UsesCustomBoneSettings() guard -> GetBoneSettingsType() (concrete UScriptStruct)
//     -> AddBoneSetting (create if absent) -> GetBoneSettings(bone) (live memory)
// Writes via FProperty::ImportText_Direct (the set_cdo_property scalar path).
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithSkeletonRetargetActions::HandleSetIkRigBoneSettings(
	const TSharedPtr<FJsonObject>& Params)
{
	const FString IkRigPath = Params->GetStringField(TEXT("ik_rig_path"));

	UIKRigDefinition* IkRig = FMonolithAssetUtils::LoadAssetByPath<UIKRigDefinition>(IkRigPath);
	if (!IkRig)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("IK Rig not found: %s"), *IkRigPath));
	}

	FString BoneNameStr;
	if (!Params->TryGetStringField(TEXT("bone_name"), BoneNameStr) || BoneNameStr.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'bone_name' is required and must be non-empty."));
	}
	const FName BoneName(*BoneNameStr);

	const TSharedPtr<FJsonObject>* SettingsObjPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("settings"), SettingsObjPtr) || !SettingsObjPtr->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("'settings' is required and must be an object of {field: value} pairs."));
	}
	const TSharedPtr<FJsonObject>& SettingsObj = *SettingsObjPtr;
	if (SettingsObj->Values.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("'settings' must contain at least one field to set."));
	}

	UIKRigController* Controller = UIKRigController::GetController(IkRig);
	if (!Controller)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Could not get IK Rig controller for: %s"), *IkRigPath));
	}

	const int32 NumSolvers = Controller->GetNumSolvers();
	if (NumSolvers == 0)
	{
		return FMonolithActionResult::Error(
			TEXT("IK Rig has no solvers. Add a solver (e.g. Full Body IK) before setting per-bone settings."));
	}

	// Resolve the target solver set: a specific index, or all solvers that
	// accept the bone (UsesCustomBoneSettings + CanAddBoneSetting).
	bool bHasSolverIndex = false;
	int32 RequestedSolverIndex = INDEX_NONE;
	if (Params->HasTypedField<EJson::Number>(TEXT("solver_index")))
	{
		bHasSolverIndex = true;
		RequestedSolverIndex = static_cast<int32>(Params->GetNumberField(TEXT("solver_index")));
		if (RequestedSolverIndex < 0 || RequestedSolverIndex >= NumSolvers)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("solver_index %d out of range [0, %d)."), RequestedSolverIndex, NumSolvers));
		}
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Set IK Rig Bone Settings")));
	IkRig->Modify();

	TArray<TSharedPtr<FJsonValue>> SolverResults;
	int32 SolversTouched = 0;

	for (int32 SolverIndex = 0; SolverIndex < NumSolvers; ++SolverIndex)
	{
		if (bHasSolverIndex && SolverIndex != RequestedSolverIndex)
		{
			continue;
		}

		FIKRigSolverBase* Solver = Controller->GetSolverAtIndex(SolverIndex);
		if (!Solver)
		{
			continue;
		}

		// Solvers that don't support per-bone settings are skipped silently when
		// targeting "all", but reported explicitly when a specific index was asked.
		if (!Solver->UsesCustomBoneSettings())
		{
			if (bHasSolverIndex)
			{
				GEditor->EndTransaction();
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Solver %d does not support per-bone settings."), SolverIndex));
			}
			continue;
		}

		// Guard bone eligibility for this solver (CanAddBoneSetting reflects
		// whether the bone is reachable by this solver). Only enforced for the
		// explicit-index path; the all-solvers path simply skips ineligible solvers.
		FText CanAddErr;
		const bool bAlreadyHasSetting = Solver->HasSettingsOnBone(BoneName);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		const bool bCanAdd = Controller->CanAddBoneSetting(BoneName, SolverIndex, &CanAddErr);
#else
		// UE 5.6's CanAddBoneSetting doesn't take an out-error FText* yet.
		const bool bCanAdd = Controller->CanAddBoneSetting(BoneName, SolverIndex);
#endif
		if (!bAlreadyHasSetting && !bCanAdd)
		{
			if (bHasSolverIndex)
			{
				GEditor->EndTransaction();
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Bone '%s' cannot take settings in solver %d: %s"),
					*BoneNameStr, SolverIndex, *CanAddErr.ToString()));
			}
			continue;
		}

		// Create the bone-setting entry if it doesn't exist yet.
		if (!bAlreadyHasSetting)
		{
			Controller->AddBoneSetting(BoneName, SolverIndex);
		}

		const UScriptStruct* ConcreteType = Solver->GetBoneSettingsType();
		FIKRigBoneSettingsBase* Settings = Solver->GetBoneSettings(BoneName);
		if (!ConcreteType || !Settings)
		{
			if (bHasSolverIndex)
			{
				GEditor->EndTransaction();
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Failed to resolve bone-settings struct for bone '%s' in solver %d."),
					*BoneNameStr, SolverIndex));
			}
			continue;
		}

		// Walk the requested fields, writing each reflectively onto the live
		// concrete struct. Per-field outcomes are echoed.
		TArray<TSharedPtr<FJsonValue>> AppliedFields;
		TArray<TSharedPtr<FJsonValue>> FailedFields;

		for (const auto& Pair : SettingsObj->Values)
		{
			const FString FieldName = MonolithKeyToString(Pair.Key);
			const TSharedPtr<FJsonValue>& FieldVal = Pair.Value;

			// Case-insensitive property lookup (exact then fallback).
			FProperty* Prop = ConcreteType->FindPropertyByName(FName(*FieldName));
			if (!Prop)
			{
				for (TFieldIterator<FProperty> It(ConcreteType); It; ++It)
				{
					if (It->GetName().Equals(FieldName, ESearchCase::IgnoreCase))
					{
						Prop = *It;
						break;
					}
				}
			}

			auto AddFailure = [&](const FString& Reason)
			{
				TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
				F->SetStringField(TEXT("field"), FieldName);
				F->SetStringField(TEXT("reason"), Reason);
				FailedFields.Add(MakeShared<FJsonValueObject>(F));
			};

			if (!Prop)
			{
				AddFailure(FString::Printf(TEXT("Unknown field on %s"), *ConcreteType->GetName()));
				continue;
			}
			if (IsBaseBookkeepingField(Prop))
			{
				AddFailure(TEXT("'Bone' is identity metadata and cannot be set"));
				continue;
			}

			// Stringify the JSON value the same way set_cdo_property does for the
			// scalar ImportText path (numbers -> sanitized float, bools -> true/false,
			// everything else -> raw string, which carries ImportText struct/enum forms).
			FString ValStr;
			if (FieldVal->Type == EJson::Number)
			{
				ValStr = FString::SanitizeFloat(FieldVal->AsNumber());
			}
			else if (FieldVal->Type == EJson::Boolean)
			{
				ValStr = FieldVal->AsBool() ? TEXT("true") : TEXT("false");
			}
			else
			{
				ValStr = FieldVal->AsString();
			}

			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Settings);
			const TCHAR* ImportResult = Prop->ImportText_Direct(*ValStr, ValuePtr, IkRig, PPF_None);
			if (!ImportResult)
			{
				AddFailure(FString::Printf(
					TEXT("ImportText rejected value '%s' (use display name for enums, "
						 "ImportText form e.g. \"(X=1.0,Y=2.0,Z=3.0)\" for structs)"), *ValStr));
				continue;
			}

			// Read back the accepted value for the echo (reflective round-trip).
			TSharedPtr<FJsonObject> AppliedObj = MakeShared<FJsonObject>();
			AppliedObj->SetStringField(TEXT("field"), Prop->GetName());
			AppliedObj->SetField(TEXT("value"),
				FMonolithReflectionReader::PropertyToJsonValue(Prop, ValuePtr, IkRig));
			AppliedFields.Add(MakeShared<FJsonValueObject>(AppliedObj));
		}

		TSharedPtr<FJsonObject> SolverObj = MakeShared<FJsonObject>();
		SolverObj->SetNumberField(TEXT("solver_index"), SolverIndex);
		SolverObj->SetStringField(TEXT("solver_type"), SolverDisplayName(ConcreteType));
		SolverObj->SetStringField(TEXT("bone_settings_struct"), ConcreteType->GetName());
		SolverObj->SetArrayField(TEXT("applied"), AppliedFields);
		if (FailedFields.Num() > 0)
		{
			SolverObj->SetArrayField(TEXT("failed"), FailedFields);
		}
		SolverResults.Add(MakeShared<FJsonValueObject>(SolverObj));
		++SolversTouched;
	}

	GEditor->EndTransaction();

	if (SolversTouched == 0)
	{
		// Nothing was applied — undo any (empty) transaction state by not dirtying.
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No solver in the IK Rig accepts per-bone settings for bone '%s'. "
				 "Check the bone is reachable by a solver (e.g. Full Body IK)."), *BoneNameStr));
	}

	IkRig->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("ik_rig"), IkRigPath);
	Root->SetStringField(TEXT("bone_name"), BoneNameStr);
	Root->SetNumberField(TEXT("solver_count"), NumSolvers);
	Root->SetNumberField(TEXT("solvers_touched"), SolversTouched);
	Root->SetArrayField(TEXT("solvers"), SolverResults);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// T2-1: get_ik_rig_bone_settings (read-only — no transaction)
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithSkeletonRetargetActions::HandleGetIkRigBoneSettings(
	const TSharedPtr<FJsonObject>& Params)
{
	const FString IkRigPath = Params->GetStringField(TEXT("ik_rig_path"));

	UIKRigDefinition* IkRig = FMonolithAssetUtils::LoadAssetByPath<UIKRigDefinition>(IkRigPath);
	if (!IkRig)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("IK Rig not found: %s"), *IkRigPath));
	}

	UIKRigController* Controller = UIKRigController::GetController(IkRig);
	if (!Controller)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Could not get IK Rig controller for: %s"), *IkRigPath));
	}

	const int32 NumSolvers = Controller->GetNumSolvers();

	// Optional filters.
	FString BoneFilterStr;
	const bool bHasBoneFilter =
		Params->TryGetStringField(TEXT("bone_name"), BoneFilterStr) && !BoneFilterStr.IsEmpty();
	const FName BoneFilter = bHasBoneFilter ? FName(*BoneFilterStr) : NAME_None;

	bool bHasSolverIndex = false;
	int32 RequestedSolverIndex = INDEX_NONE;
	if (Params->HasTypedField<EJson::Number>(TEXT("solver_index")))
	{
		bHasSolverIndex = true;
		RequestedSolverIndex = static_cast<int32>(Params->GetNumberField(TEXT("solver_index")));
		if (RequestedSolverIndex < 0 || RequestedSolverIndex >= NumSolvers)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("solver_index %d out of range [0, %d)."), RequestedSolverIndex, NumSolvers));
		}
	}

	TArray<TSharedPtr<FJsonValue>> SolverResults;

	for (int32 SolverIndex = 0; SolverIndex < NumSolvers; ++SolverIndex)
	{
		if (bHasSolverIndex && SolverIndex != RequestedSolverIndex)
		{
			continue;
		}

		FIKRigSolverBase* Solver = Controller->GetSolverAtIndex(SolverIndex);
		if (!Solver || !Solver->UsesCustomBoneSettings())
		{
			continue;
		}

		const UScriptStruct* ConcreteType = Solver->GetBoneSettingsType();
		if (!ConcreteType)
		{
			continue;
		}

		// Which bones to report for this solver.
		TArray<FName> BonesToRead;
		if (bHasBoneFilter)
		{
			if (Solver->HasSettingsOnBone(BoneFilter))
			{
				BonesToRead.Add(BoneFilter);
			}
		}
		else
		{
			TSet<FName> WithSettings;
			Solver->GetBonesWithSettings(WithSettings);
			BonesToRead = WithSettings.Array();
		}

		TArray<TSharedPtr<FJsonValue>> BoneEntries;
		for (const FName& Bone : BonesToRead)
		{
			FIKRigBoneSettingsBase* Settings = Solver->GetBoneSettings(Bone);
			if (!Settings)
			{
				continue;
			}
			TSharedPtr<FJsonObject> BoneObj = MakeShared<FJsonObject>();
			BoneObj->SetStringField(TEXT("bone"), Bone.ToString());
			BoneObj->SetObjectField(TEXT("settings"),
				BoneSettingsToJson(ConcreteType, Settings, IkRig));
			BoneEntries.Add(MakeShared<FJsonValueObject>(BoneObj));
		}

		// Skip solvers with nothing to report when a bone filter found no match.
		if (bHasBoneFilter && BoneEntries.Num() == 0)
		{
			continue;
		}

		TSharedPtr<FJsonObject> SolverObj = MakeShared<FJsonObject>();
		SolverObj->SetNumberField(TEXT("solver_index"), SolverIndex);
		SolverObj->SetStringField(TEXT("bone_settings_struct"), ConcreteType->GetName());
		SolverObj->SetArrayField(TEXT("bones"), BoneEntries);
		SolverResults.Add(MakeShared<FJsonValueObject>(SolverObj));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("ik_rig"), IkRigPath);
	Root->SetNumberField(TEXT("solver_count"), NumSolvers);
	if (bHasBoneFilter)
	{
		Root->SetStringField(TEXT("bone_name"), BoneFilterStr);
	}
	Root->SetArrayField(TEXT("solvers"), SolverResults);
	return FMonolithActionResult::Success(Root);
}
