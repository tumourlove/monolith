#include "MonolithRetargetSettingsActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"

#include "Editor.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"

#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetSettings.h"
#include "Retargeter/IKRetargetOps.h"
#include "Retargeter/RetargetOps/FKChainsOp.h"
#include "Retargeter/RetargetOps/IKChainsOp.h"
#include "Retargeter/RetargetOps/PelvisMotionOp.h"
#include "Retargeter/RetargetOps/SpeedPlantingOp.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "RetargetEditor/IKRetargeterPoseGenerator.h" // ERetargetAutoAlignMethod

namespace
{
	// -----------------------------------------------------------------------
	// Shared helpers (file-local)
	// -----------------------------------------------------------------------

	/** Load an IK Retargeter asset + resolve its controller. Returns nullptr + error string on miss. */
	UIKRetargeterController* ResolveRetargeterController(const FString& AssetPath, FString& OutError)
	{
		UIKRetargeter* Asset = FMonolithAssetUtils::LoadAssetByPath<UIKRetargeter>(AssetPath);
		if (!Asset)
		{
			OutError = FString::Printf(TEXT("IKRetargeter not found: %s"), *AssetPath);
			return nullptr;
		}

		UIKRetargeterController* Controller = UIKRetargeterController::GetController(Asset);
		if (!Controller)
		{
			OutError = TEXT("Failed to get UIKRetargeterController for the asset");
			return nullptr;
		}
		return Controller;
	}

	/** Parse "source" | "target" into ERetargetSourceOrTarget. Defaults to Target. */
	bool ParseSourceOrTarget(const FString& In, ERetargetSourceOrTarget& Out)
	{
		if (In.Equals(TEXT("source"), ESearchCase::IgnoreCase)) { Out = ERetargetSourceOrTarget::Source; return true; }
		if (In.Equals(TEXT("target"), ESearchCase::IgnoreCase)) { Out = ERetargetSourceOrTarget::Target; return true; }
		return false;
	}

	/** Read a side param ("source"|"target") with a default. */
	ERetargetSourceOrTarget ReadSide(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, ERetargetSourceOrTarget Default)
	{
		FString SideStr;
		ERetargetSourceOrTarget Side = Default;
		if (Params->TryGetStringField(Key, SideStr))
		{
			ParseSourceOrTarget(SideStr, Side);
		}
		return Side;
	}

	const TCHAR* SideToString(ERetargetSourceOrTarget Side)
	{
		return Side == ERetargetSourceOrTarget::Source ? TEXT("source") : TEXT("target");
	}

	/** EFKChainRotationMode has NON-SEQUENTIAL explicit enum values — parse strictly by name. */
	bool ParseFKRotationMode(const FString& In, EFKChainRotationMode& Out)
	{
		const UEnum* E = StaticEnum<EFKChainRotationMode>();
		const int64 Value = E ? E->GetValueByNameString(In) : INDEX_NONE;
		if (Value == INDEX_NONE) { return false; }
		Out = static_cast<EFKChainRotationMode>(Value);
		return true;
	}

	bool ParseFKTranslationMode(const FString& In, EFKChainTranslationMode& Out)
	{
		const UEnum* E = StaticEnum<EFKChainTranslationMode>();
		const int64 Value = E ? E->GetValueByNameString(In) : INDEX_NONE;
		if (Value == INDEX_NONE) { return false; }
		Out = static_cast<EFKChainTranslationMode>(Value);
		return true;
	}

	FString FKRotationModeToString(EFKChainRotationMode Mode)
	{
		const UEnum* E = StaticEnum<EFKChainRotationMode>();
		return E ? E->GetNameStringByValue(static_cast<int64>(Mode)) : FString::FromInt(static_cast<int32>(Mode));
	}

	FString FKTranslationModeToString(EFKChainTranslationMode Mode)
	{
		const UEnum* E = StaticEnum<EFKChainTranslationMode>();
		return E ? E->GetNameStringByValue(static_cast<int64>(Mode)) : FString::FromInt(static_cast<int32>(Mode));
	}

	bool ParseAutoAlignMethod(const FString& In, ERetargetAutoAlignMethod& Out)
	{
		const UEnum* E = StaticEnum<ERetargetAutoAlignMethod>();
		const int64 Value = E ? E->GetValueByNameString(In) : INDEX_NONE;
		if (Value == INDEX_NONE) { return false; }
		Out = static_cast<ERetargetAutoAlignMethod>(Value);
		return true;
	}

	TSharedPtr<FJsonObject> VectorToJson(const FVector& V)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}

	bool TryReadVector(const TSharedPtr<FJsonObject>& Obj, FVector& Out)
	{
		if (!Obj.IsValid()) { return false; }
		double X = 0.0, Y = 0.0, Z = 0.0;
		Obj->TryGetNumberField(TEXT("x"), X);
		Obj->TryGetNumberField(TEXT("y"), Y);
		Obj->TryGetNumberField(TEXT("z"), Z);
		Out = FVector(X, Y, Z);
		return true;
	}

	/** Quat delta JSON echo as the matching Euler (pitch/yaw/roll) for readability. */
	TSharedPtr<FJsonObject> QuatToJson(const FQuat& Q)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), Q.X);
		O->SetNumberField(TEXT("y"), Q.Y);
		O->SetNumberField(TEXT("z"), Q.Z);
		O->SetNumberField(TEXT("w"), Q.W);
		const FRotator R = Q.Rotator();
		TSharedPtr<FJsonObject> Euler = MakeShared<FJsonObject>();
		Euler->SetNumberField(TEXT("pitch"), R.Pitch);
		Euler->SetNumberField(TEXT("yaw"), R.Yaw);
		Euler->SetNumberField(TEXT("roll"), R.Roll);
		O->SetObjectField(TEXT("euler"), Euler);
		return O;
	}

	/**
	 * Read a rotation delta from a bone_delta JSON entry. Accepts either a quaternion
	 * {x,y,z,w} or an Euler {pitch,yaw,roll}. Quaternion takes precedence when both present.
	 */
	bool TryReadRotationDelta(const TSharedPtr<FJsonObject>& Obj, FQuat& Out)
	{
		if (!Obj.IsValid()) { return false; }

		const bool bHasQuat =
			Obj->HasField(TEXT("x")) && Obj->HasField(TEXT("y")) &&
			Obj->HasField(TEXT("z")) && Obj->HasField(TEXT("w"));
		if (bHasQuat)
		{
			double X = 0.0, Y = 0.0, Z = 0.0, W = 1.0;
			Obj->TryGetNumberField(TEXT("x"), X);
			Obj->TryGetNumberField(TEXT("y"), Y);
			Obj->TryGetNumberField(TEXT("z"), Z);
			Obj->TryGetNumberField(TEXT("w"), W);
			Out = FQuat(X, Y, Z, W);
			Out.Normalize();
			return true;
		}

		const bool bHasEuler =
			Obj->HasField(TEXT("pitch")) || Obj->HasField(TEXT("yaw")) || Obj->HasField(TEXT("roll"));
		if (bHasEuler)
		{
			double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
			Obj->TryGetNumberField(TEXT("pitch"), Pitch);
			Obj->TryGetNumberField(TEXT("yaw"), Yaw);
			Obj->TryGetNumberField(TEXT("roll"), Roll);
			Out = FRotator(Pitch, Yaw, Roll).Quaternion();
			return true;
		}

		return false;
	}
}

// ===========================================================================
//  Shared op-controller resolution
// ===========================================================================

UIKRetargetOpControllerBase* FMonolithRetargetSettingsActions::ResolveOpController(
	UIKRetargeterController* Controller,
	const UScriptStruct* OpType,
	int32& OutOpIndex)
{
	OutOpIndex = INDEX_NONE;
	if (!Controller || !OpType)
	{
		return nullptr;
	}

	const int32 NumOps = Controller->GetNumRetargetOps();
	for (int32 Index = 0; Index < NumOps; ++Index)
	{
		const FIKRetargetOpBase* Op = Controller->GetRetargetOpByIndex(Index);
		if (Op && Op->GetType() == OpType)
		{
			OutOpIndex = Index;
			return Controller->GetOpController(Index);
		}
	}
	return nullptr;
}

// ===========================================================================
//  T1-R1 — align_retarget_pose
// ===========================================================================

FMonolithActionResult FMonolithRetargetSettingsActions::HandleAlignRetargetPose(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("retargeter_path"));

	FString Error;
	UIKRetargeterController* Controller = ResolveRetargeterController(AssetPath, Error);
	if (!Controller) { return FMonolithActionResult::Error(Error); }

	// mode: auto_align_all | align_to_source | align_chain
	FString ModeStr = TEXT("auto_align_all");
	Params->TryGetStringField(TEXT("mode"), ModeStr);

	// side: source | target | both (default both)
	FString SideStr = TEXT("both");
	Params->TryGetStringField(TEXT("side"), SideStr);

	const bool bDoSource = SideStr.Equals(TEXT("source"), ESearchCase::IgnoreCase) || SideStr.Equals(TEXT("both"), ESearchCase::IgnoreCase);
	const bool bDoTarget = SideStr.Equals(TEXT("target"), ESearchCase::IgnoreCase) || SideStr.Equals(TEXT("both"), ESearchCase::IgnoreCase);
	if (!bDoSource && !bDoTarget)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown side '%s' — use 'source', 'target', or 'both'"), *SideStr));
	}

	// optional alignment method (defaults to ChainToChain)
	ERetargetAutoAlignMethod Method = ERetargetAutoAlignMethod::ChainToChain;
	FString MethodStr;
	if (Params->TryGetStringField(TEXT("method"), MethodStr) && !ParseAutoAlignMethod(MethodStr, Method))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown method '%s' — use ChainToChain, MeshToMesh, LocalRotationAxes, or GlobalRotationAxes"), *MethodStr));
	}

	// optional bone subset (for align_chain mode)
	TArray<FName> BonesToAlign;
	const TArray<TSharedPtr<FJsonValue>>* ChainNamesArr = nullptr;
	if (Params->TryGetArrayField(TEXT("chain_names"), ChainNamesArr))
	{
		for (const TSharedPtr<FJsonValue>& V : *ChainNamesArr)
		{
			FString BoneStr;
			if (V.IsValid() && V->TryGetString(BoneStr) && !BoneStr.IsEmpty())
			{
				BonesToAlign.Add(FName(*BoneStr));
			}
		}
	}

	// optional snap-to-ground reference bone
	FString SnapBone;
	const bool bSnapToGround = Params->TryGetStringField(TEXT("snap_to_ground_bone"), SnapBone) && !SnapBone.IsEmpty();

	enum class EAlignMode : uint8 { AutoAlignAll, AlignToSource, AlignChain };
	EAlignMode AlignMode = EAlignMode::AutoAlignAll;
	if (ModeStr.Equals(TEXT("align_to_source"), ESearchCase::IgnoreCase))
	{
		AlignMode = EAlignMode::AlignToSource;
		Method = ERetargetAutoAlignMethod::ChainToChain; // align "to source" == chain-to-chain alignment
	}
	else if (ModeStr.Equals(TEXT("align_chain"), ESearchCase::IgnoreCase))
	{
		AlignMode = EAlignMode::AlignChain;
		if (BonesToAlign.Num() == 0)
		{
			return FMonolithActionResult::Error(TEXT("mode 'align_chain' requires a non-empty 'chain_names' array of bone names"));
		}
	}
	else if (!ModeStr.Equals(TEXT("auto_align_all"), ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown mode '%s' — use 'auto_align_all', 'align_to_source', or 'align_chain'"), *ModeStr));
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("Align Retarget Pose")));
	Controller->GetAsset()->Modify();

	auto AlignOneSide = [&](ERetargetSourceOrTarget Side)
	{
		if (AlignMode == EAlignMode::AlignChain)
		{
			Controller->AutoAlignBones(BonesToAlign, Method, Side);
		}
		else
		{
			Controller->AutoAlignAllBones(Side, Method);
		}

		if (bSnapToGround)
		{
			Controller->SnapBoneToGround(FName(*SnapBone), Side);
		}
	};

	TArray<FString> SidesAligned;
	if (bDoSource) { AlignOneSide(ERetargetSourceOrTarget::Source); SidesAligned.Add(TEXT("source")); }
	if (bDoTarget) { AlignOneSide(ERetargetSourceOrTarget::Target); SidesAligned.Add(TEXT("target")); }

	Controller->GetAsset()->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("retargeter_path"), AssetPath);
	Root->SetStringField(TEXT("mode"), ModeStr);
	{
		TArray<TSharedPtr<FJsonValue>> SidesArr;
		for (const FString& S : SidesAligned) { SidesArr.Add(MakeShared<FJsonValueString>(S)); }
		Root->SetArrayField(TEXT("sides_aligned"), SidesArr);
	}
	Root->SetStringField(TEXT("method"), StaticEnum<ERetargetAutoAlignMethod>()->GetNameStringByValue(static_cast<int64>(Method)));
	Root->SetNumberField(TEXT("bones_requested"), BonesToAlign.Num());
	if (bSnapToGround) { Root->SetStringField(TEXT("snapped_to_ground_bone"), SnapBone); }
	return FMonolithActionResult::Success(Root);
}

// ===========================================================================
//  T1-R2 — get_retarget_pose
// ===========================================================================

FMonolithActionResult FMonolithRetargetSettingsActions::HandleGetRetargetPose(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("retargeter_path"));

	FString Error;
	UIKRetargeterController* Controller = ResolveRetargeterController(AssetPath, Error);
	if (!Controller) { return FMonolithActionResult::Error(Error); }

	const ERetargetSourceOrTarget Side = ReadSide(Params, TEXT("side"), ERetargetSourceOrTarget::Target);

	const FName PoseName = Controller->GetCurrentRetargetPoseName(Side);
	const FIKRetargetPose& Pose = Controller->GetCurrentRetargetPose(Side);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("retargeter_path"), AssetPath);
	Root->SetStringField(TEXT("side"), SideToString(Side));
	Root->SetStringField(TEXT("pose_name"), PoseName.ToString());

	// Per-bone rotation deltas
	const TMap<FName, FQuat>& Deltas = Pose.GetAllDeltaRotations();
	TArray<TSharedPtr<FJsonValue>> DeltasArr;
	for (const TPair<FName, FQuat>& Pair : Deltas)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("bone"), Pair.Key.ToString());
		Entry->SetObjectField(TEXT("rotation"), QuatToJson(Pair.Value));
		DeltasArr.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Root->SetArrayField(TEXT("bone_deltas"), DeltasArr);
	Root->SetNumberField(TEXT("bone_delta_count"), DeltasArr.Num());

	// Root (pelvis) translation delta
	Root->SetObjectField(TEXT("root_translation_delta"), VectorToJson(Controller->GetRootOffsetInRetargetPose(Side)));

	// Enumerate available poses for convenience
	TArray<TSharedPtr<FJsonValue>> PoseNamesArr;
	for (const TPair<FName, FIKRetargetPose>& Pair : Controller->GetRetargetPoses(Side))
	{
		PoseNamesArr.Add(MakeShared<FJsonValueString>(Pair.Key.ToString()));
	}
	Root->SetArrayField(TEXT("available_poses"), PoseNamesArr);

	return FMonolithActionResult::Success(Root);
}

// ===========================================================================
//  T1-R2 — set_retarget_pose
//  v1 SCOPE: from_reference + bone_deltas[] ONLY. from_animation is DEFERRED
//  (editor-UI-glued — see plan Risk Register).
// ===========================================================================

FMonolithActionResult FMonolithRetargetSettingsActions::HandleSetRetargetPose(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("retargeter_path"));

	FString Error;
	UIKRetargeterController* Controller = ResolveRetargeterController(AssetPath, Error);
	if (!Controller) { return FMonolithActionResult::Error(Error); }

	const ERetargetSourceOrTarget Side = ReadSide(Params, TEXT("side"), ERetargetSourceOrTarget::Target);

	// mode: from_reference | bone_deltas  (from_animation DEFERRED, rejected with a clear message)
	FString ModeStr = TEXT("bone_deltas");
	Params->TryGetStringField(TEXT("mode"), ModeStr);

	if (ModeStr.Equals(TEXT("from_animation"), ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(TEXT(
			"mode 'from_animation' is not supported in v1 (the import-from-sequence path is editor-UI-glued). "
			"Use 'from_reference' to reset, or 'bone_deltas' to author per-bone rotation offsets."));
	}

	const bool bFromReference = ModeStr.Equals(TEXT("from_reference"), ESearchCase::IgnoreCase);
	const bool bBoneDeltas = ModeStr.Equals(TEXT("bone_deltas"), ESearchCase::IgnoreCase);
	if (!bFromReference && !bBoneDeltas)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown mode '%s' — use 'from_reference' or 'bone_deltas' (from_animation is deferred in v1)"), *ModeStr));
	}

	const FName PoseName = Controller->GetCurrentRetargetPoseName(Side);

	const FScopedTransaction Transaction(FText::FromString(TEXT("Set Retarget Pose")));
	Controller->GetAsset()->Modify();

	int32 BonesWritten = 0;
	bool bRootWritten = false;

	if (bFromReference)
	{
		// Empty bones-to-reset == reset the whole pose to the reference pose.
		TArray<FName> BonesToReset;
		const TArray<TSharedPtr<FJsonValue>>* ResetBonesArr = nullptr;
		if (Params->TryGetArrayField(TEXT("bones"), ResetBonesArr))
		{
			for (const TSharedPtr<FJsonValue>& V : *ResetBonesArr)
			{
				FString BoneStr;
				if (V.IsValid() && V->TryGetString(BoneStr) && !BoneStr.IsEmpty())
				{
					BonesToReset.Add(FName(*BoneStr));
				}
			}
		}
		Controller->ResetRetargetPose(PoseName, BonesToReset, Side);
		BonesWritten = BonesToReset.Num();
	}
	else // bBoneDeltas
	{
		const TArray<TSharedPtr<FJsonValue>>* DeltasArr = nullptr;
		if (!Params->TryGetArrayField(TEXT("bone_deltas"), DeltasArr) || DeltasArr->Num() == 0)
		{
			return FMonolithActionResult::Error(TEXT(
				"mode 'bone_deltas' requires a non-empty 'bone_deltas' array of {bone, rotation:{x,y,z,w}|{pitch,yaw,roll}} entries"));
		}

		for (const TSharedPtr<FJsonValue>& V : *DeltasArr)
		{
			const TSharedPtr<FJsonObject>* EntryPtr = nullptr;
			if (!V.IsValid() || !V->TryGetObject(EntryPtr) || !EntryPtr->IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& Entry = *EntryPtr;

			FString BoneStr;
			if (!Entry->TryGetStringField(TEXT("bone"), BoneStr) || BoneStr.IsEmpty())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* RotObjPtr = nullptr;
			FQuat RotationDelta = FQuat::Identity;
			if (Entry->TryGetObjectField(TEXT("rotation"), RotObjPtr) && RotObjPtr->IsValid() &&
				TryReadRotationDelta(*RotObjPtr, RotationDelta))
			{
				Controller->SetRotationOffsetForRetargetPoseBone(FName(*BoneStr), RotationDelta, Side);
				++BonesWritten;
			}
		}
	}

	// Optional root (pelvis) translation delta — applies to both modes.
	const TSharedPtr<FJsonObject>* RootDeltaPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("root_translation_delta"), RootDeltaPtr) && RootDeltaPtr->IsValid())
	{
		FVector RootDelta;
		if (TryReadVector(*RootDeltaPtr, RootDelta))
		{
			Controller->SetRootOffsetInRetargetPose(RootDelta, Side);
			bRootWritten = true;
		}
	}

	Controller->GetAsset()->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("retargeter_path"), AssetPath);
	Root->SetStringField(TEXT("side"), SideToString(Side));
	Root->SetStringField(TEXT("pose_name"), PoseName.ToString());
	Root->SetStringField(TEXT("mode"), bFromReference ? TEXT("from_reference") : TEXT("bone_deltas"));
	Root->SetNumberField(TEXT("bones_written"), BonesWritten);
	Root->SetBoolField(TEXT("root_translation_written"), bRootWritten);
	return FMonolithActionResult::Success(Root);
}

// ===========================================================================
//  T1-R3 — get_retarget_chain_settings
// ===========================================================================

FMonolithActionResult FMonolithRetargetSettingsActions::HandleGetRetargetChainSettings(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("retargeter_path"));

	FString Error;
	UIKRetargeterController* Controller = ResolveRetargeterController(AssetPath, Error);
	if (!Controller) { return FMonolithActionResult::Error(Error); }

	// Optional filter to a single chain by name.
	FString ChainFilter;
	const bool bHasChainFilter = Params->TryGetStringField(TEXT("chain_name"), ChainFilter) && !ChainFilter.IsEmpty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("retargeter_path"), AssetPath);
	if (bHasChainFilter) { Root->SetStringField(TEXT("chain_name"), ChainFilter); }

	// --- FK Chains op ---
	int32 FKOpIndex = INDEX_NONE;
	UIKRetargetOpControllerBase* FKBase = ResolveOpController(Controller, FIKRetargetFKChainsOp::StaticStruct(), FKOpIndex);
	if (UIKRetargetFKChainsController* FKController = Cast<UIKRetargetFKChainsController>(FKBase))
	{
		const FIKRetargetFKChainsOpSettings FKSettings = FKController->GetSettings();
		TArray<TSharedPtr<FJsonValue>> FKArr;
		for (const FRetargetFKChainSettings& Chain : FKSettings.ChainsToRetarget)
		{
			if (bHasChainFilter && !Chain.TargetChainName.ToString().Equals(ChainFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("target_chain"), Chain.TargetChainName.ToString());
			Entry->SetBoolField(TEXT("enable_fk"), Chain.EnableFK);
			Entry->SetStringField(TEXT("fk_rotation_mode"), FKRotationModeToString(Chain.RotationMode));
			Entry->SetNumberField(TEXT("rotation_alpha"), Chain.RotationAlpha);
			Entry->SetStringField(TEXT("translation_mode"), FKTranslationModeToString(Chain.TranslationMode));
			Entry->SetNumberField(TEXT("translation_alpha"), Chain.TranslationAlpha);
			FKArr.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Root->SetArrayField(TEXT("fk_chains"), FKArr);
		Root->SetNumberField(TEXT("fk_op_index"), FKOpIndex);
	}
	else
	{
		Root->SetArrayField(TEXT("fk_chains"), TArray<TSharedPtr<FJsonValue>>());
		Root->SetStringField(TEXT("fk_chains_note"), TEXT("No FK Chains op in the retargeter op stack"));
	}

	// --- IK Chains op ---
	// NOTE: read THROUGH a base op-settings pointer (no derived value-copy). Value-copying
	// FIKRetargetIKChainsOpSettings emits its vtable in this TU, which references the
	// non-IKRIG_API-exported FIKRetargetIKChainsOpSettings::PostLoad -> LNK2001 (UE 5.7 header bug).
	int32 IKOpIndex = INDEX_NONE;
	UIKRetargetOpControllerBase* IKBase = ResolveOpController(Controller, FIKRetargetIKChainsOp::StaticStruct(), IKOpIndex);
	FIKRetargetOpBase* IKOp = (IKOpIndex != INDEX_NONE) ? Controller->GetRetargetOpByIndex(IKOpIndex) : nullptr;
	if (IKBase && IKOp)
	{
		const FIKRetargetIKChainsOpSettings* IKSettings = static_cast<const FIKRetargetIKChainsOpSettings*>(IKOp->GetSettings());
		TArray<TSharedPtr<FJsonValue>> IKArr;
		for (const FRetargetIKChainSettings& Chain : IKSettings->ChainsToRetarget)
		{
			if (bHasChainFilter && !Chain.TargetChainName.ToString().Equals(ChainFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("target_chain"), Chain.TargetChainName.ToString());
			Entry->SetBoolField(TEXT("ik_enabled"), Chain.EnableIK);
			Entry->SetNumberField(TEXT("blend_to_source"), Chain.BlendToSource);
			Entry->SetObjectField(TEXT("static_offset"), VectorToJson(Chain.StaticOffset));
			Entry->SetObjectField(TEXT("static_local_offset"), VectorToJson(Chain.StaticLocalOffset));
			IKArr.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Root->SetArrayField(TEXT("ik_chains"), IKArr);
		Root->SetNumberField(TEXT("ik_op_index"), IKOpIndex);
	}
	else
	{
		Root->SetArrayField(TEXT("ik_chains"), TArray<TSharedPtr<FJsonValue>>());
		Root->SetStringField(TEXT("ik_chains_note"), TEXT("No IK Chains op in the retargeter op stack"));
	}

	return FMonolithActionResult::Success(Root);
}

// ===========================================================================
//  T1-R3 — set_retarget_chain_settings
// ===========================================================================

FMonolithActionResult FMonolithRetargetSettingsActions::HandleSetRetargetChainSettings(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("retargeter_path"));

	FString ChainName;
	if (!Params->TryGetStringField(TEXT("chain_name"), ChainName) || ChainName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'chain_name' (target chain) is required"));
	}
	const FName ChainFName(*ChainName);

	FString Error;
	UIKRetargeterController* Controller = ResolveRetargeterController(AssetPath, Error);
	if (!Controller) { return FMonolithActionResult::Error(Error); }

	const FScopedTransaction Transaction(FText::FromString(TEXT("Set Retarget Chain Settings")));
	Controller->GetAsset()->Modify();

	bool bAnyApplied = false;
	bool bFKChainFound = false;
	bool bIKChainFound = false;

	// --- FK Chains op: fk_rotation_mode, translation_mode, translation_scale (=> alpha), rotation_alpha ---
	const bool bWantsFK =
		Params->HasField(TEXT("fk_rotation_mode")) || Params->HasField(TEXT("translation_mode")) ||
		Params->HasField(TEXT("translation_scale")) || Params->HasField(TEXT("rotation_alpha")) ||
		Params->HasField(TEXT("enable_fk"));

	if (bWantsFK)
	{
		int32 FKOpIndex = INDEX_NONE;
		UIKRetargetFKChainsController* FKController =
			Cast<UIKRetargetFKChainsController>(ResolveOpController(Controller, FIKRetargetFKChainsOp::StaticStruct(), FKOpIndex));
		if (!FKController)
		{
			return FMonolithActionResult::Error(TEXT(
				"No FK Chains op in the retargeter op stack — cannot set fk_rotation_mode/translation_mode/translation_scale. "
				"Seed default ops on the retargeter first."));
		}

		// Op settings are returned BY VALUE — mutate the copy, then SetSettings() it back.
		FIKRetargetFKChainsOpSettings FKSettings = FKController->GetSettings();
		for (FRetargetFKChainSettings& Chain : FKSettings.ChainsToRetarget)
		{
			if (Chain.TargetChainName != ChainFName)
			{
				continue;
			}
			bFKChainFound = true;

			FString RotModeStr;
			if (Params->TryGetStringField(TEXT("fk_rotation_mode"), RotModeStr))
			{
				EFKChainRotationMode RotMode;
				if (!ParseFKRotationMode(RotModeStr, RotMode))
				{
					return FMonolithActionResult::Error(FString::Printf(
						TEXT("Unknown fk_rotation_mode '%s' — use None, Interpolated, OneToOne, OneToOneReversed, MatchChain, MatchScaledChain, or CopyLocal"),
						*RotModeStr));
				}
				Chain.RotationMode = RotMode;
			}

			FString TransModeStr;
			if (Params->TryGetStringField(TEXT("translation_mode"), TransModeStr))
			{
				EFKChainTranslationMode TransMode;
				if (!ParseFKTranslationMode(TransModeStr, TransMode))
				{
					return FMonolithActionResult::Error(FString::Printf(
						TEXT("Unknown translation_mode '%s' — use None, GloballyScaled, Absolute, StretchBoneLengthUniformly, StretchBoneLengthNonUniformly, or OrientAndScale"),
						*TransModeStr));
				}
				Chain.TranslationMode = TransMode;
			}

			double TranslationScale = 0.0;
			if (Params->TryGetNumberField(TEXT("translation_scale"), TranslationScale))
			{
				Chain.TranslationAlpha = TranslationScale;
			}

			double RotationAlpha = 0.0;
			if (Params->TryGetNumberField(TEXT("rotation_alpha"), RotationAlpha))
			{
				Chain.RotationAlpha = RotationAlpha;
			}

			bool bEnableFK = false;
			if (Params->TryGetBoolField(TEXT("enable_fk"), bEnableFK))
			{
				Chain.EnableFK = bEnableFK;
			}
			break;
		}

		if (bFKChainFound)
		{
			FKController->SetSettings(FKSettings);
			bAnyApplied = true;
		}
	}

	// --- IK Chains op: ik_enabled, blend_to_source, pole_vector (=> static offset), static_offset ---
	const bool bWantsIK =
		Params->HasField(TEXT("ik_enabled")) || Params->HasField(TEXT("blend_to_source")) ||
		Params->HasField(TEXT("pole_vector")) || Params->HasField(TEXT("static_offset"));

	if (bWantsIK)
	{
		int32 IKOpIndex = INDEX_NONE;
		UIKRetargetIKChainsController* IKController =
			Cast<UIKRetargetIKChainsController>(ResolveOpController(Controller, FIKRetargetIKChainsOp::StaticStruct(), IKOpIndex));
		FIKRetargetOpBase* IKOp = (IKOpIndex != INDEX_NONE) ? Controller->GetRetargetOpByIndex(IKOpIndex) : nullptr;
		if (!IKController || !IKOp)
		{
			return FMonolithActionResult::Error(TEXT(
				"No IK Chains op in the retargeter op stack — cannot set ik_enabled/blend_to_source/static_offset. "
				"Seed default ops on the retargeter first."));
		}

		// Mutate IN PLACE through the base op-settings pointer — never declare a derived value
		// (value-copy emits the vtable -> unexported FIKRetargetIKChainsOpSettings::PostLoad -> LNK2001).
		FIKRetargetOpSettingsBase* IKBaseSettings = IKOp->GetSettings();
		FIKRetargetIKChainsOpSettings* IKSettings = static_cast<FIKRetargetIKChainsOpSettings*>(IKBaseSettings);
		for (FRetargetIKChainSettings& Chain : IKSettings->ChainsToRetarget)
		{
			if (Chain.TargetChainName != ChainFName)
			{
				continue;
			}
			bIKChainFound = true;

			bool bIKEnabled = false;
			if (Params->TryGetBoolField(TEXT("ik_enabled"), bIKEnabled))
			{
				Chain.EnableIK = bIKEnabled;
			}

			double BlendToSource = 0.0;
			if (Params->TryGetNumberField(TEXT("blend_to_source"), BlendToSource))
			{
				Chain.BlendToSource = BlendToSource;
			}

			// pole_vector and static_offset both map to the chain's StaticOffset (global-space goal offset).
			const TSharedPtr<FJsonObject>* OffsetObjPtr = nullptr;
			if ((Params->TryGetObjectField(TEXT("static_offset"), OffsetObjPtr) ||
				 Params->TryGetObjectField(TEXT("pole_vector"), OffsetObjPtr)) && OffsetObjPtr->IsValid())
			{
				FVector Offset;
				if (TryReadVector(*OffsetObjPtr, Offset))
				{
					Chain.StaticOffset = Offset;
				}
			}
			break;
		}

		if (bIKChainFound)
		{
			IKOp->SetSettings(IKBaseSettings);
			bAnyApplied = true;
		}
	}

	if (!bAnyApplied)
	{
		if (bWantsFK && !bFKChainFound && !bWantsIK)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Target chain '%s' not found in the FK Chains op"), *ChainName));
		}
		if (bWantsIK && !bIKChainFound && !bWantsFK)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Target chain '%s' not found in the IK Chains op"), *ChainName));
		}
		if (!bWantsFK && !bWantsIK)
		{
			return FMonolithActionResult::Error(TEXT(
				"No settings supplied — provide one or more of: fk_rotation_mode, translation_mode, translation_scale, "
				"rotation_alpha, enable_fk, ik_enabled, blend_to_source, static_offset, pole_vector"));
		}
		return FMonolithActionResult::Error(FString::Printf(TEXT("Target chain '%s' not found in the targeted op(s)"), *ChainName));
	}

	Controller->GetAsset()->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("retargeter_path"), AssetPath);
	Root->SetStringField(TEXT("chain_name"), ChainName);
	Root->SetBoolField(TEXT("fk_applied"), bFKChainFound);
	Root->SetBoolField(TEXT("ik_applied"), bIKChainFound);
	return FMonolithActionResult::Success(Root);
}

// ===========================================================================
//  T1-R5 — set_retarget_root_settings (Pelvis Motion Op)
// ===========================================================================

FMonolithActionResult FMonolithRetargetSettingsActions::HandleSetRetargetRootSettings(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("retargeter_path"));

	FString Error;
	UIKRetargeterController* Controller = ResolveRetargeterController(AssetPath, Error);
	if (!Controller) { return FMonolithActionResult::Error(Error); }

	int32 PelvisOpIndex = INDEX_NONE;
	UIKRetargetPelvisMotionController* PelvisController =
		Cast<UIKRetargetPelvisMotionController>(ResolveOpController(Controller, FIKRetargetPelvisMotionOp::StaticStruct(), PelvisOpIndex));
	FIKRetargetOpBase* PelvisOp = (PelvisOpIndex != INDEX_NONE) ? Controller->GetRetargetOpByIndex(PelvisOpIndex) : nullptr;
	if (!PelvisController || !PelvisOp)
	{
		return FMonolithActionResult::Error(TEXT(
			"No Pelvis Motion op in the retargeter op stack. Seed default ops on the retargeter first."));
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("Set Retarget Root Settings")));
	Controller->GetAsset()->Modify();

	// Mutate IN PLACE through the base op-settings pointer — never declare a derived value
	// (value-copy emits the vtable -> unexported FIKRetargetPelvisMotionOpSettings::PostLoad -> LNK2001).
	FIKRetargetOpSettingsBase* PelvisBaseSettings = PelvisOp->GetSettings();
	FIKRetargetPelvisMotionOpSettings* Settings = static_cast<FIKRetargetPelvisMotionOpSettings*>(PelvisBaseSettings);

	double V = 0.0;
	if (Params->TryGetNumberField(TEXT("scale_horizontal"), V))        { Settings->ScaleHorizontal = V; }
	if (Params->TryGetNumberField(TEXT("scale_vertical"), V))          { Settings->ScaleVertical = V; }
	if (Params->TryGetNumberField(TEXT("affect_ik_horizontal"), V))    { Settings->AffectIKHorizontal = V; }
	if (Params->TryGetNumberField(TEXT("affect_ik_vertical"), V))      { Settings->AffectIKVertical = V; }
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	if (Params->TryGetNumberField(TEXT("floor_constraint_weight"), V)) { Settings->FloorConstraintWeight = V; }
#endif // UE 5.6's FIKRetargetPelvisMotionOpSettings has no floor constraint yet.
	if (Params->TryGetNumberField(TEXT("rotation_alpha"), V))          { Settings->RotationAlpha = V; }
	if (Params->TryGetNumberField(TEXT("translation_alpha"), V))       { Settings->TranslationAlpha = V; }
	if (Params->TryGetNumberField(TEXT("blend_to_source_translation"), V)) { Settings->BlendToSourceTranslation = V; }

	const TSharedPtr<FJsonObject>* OffsetObjPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("translation_offset_global"), OffsetObjPtr) && OffsetObjPtr->IsValid())
	{
		FVector Offset;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		if (TryReadVector(*OffsetObjPtr, Offset)) { Settings->TranslationOffsetGlobal = Offset; }
#else
		// UE 5.6 only has the (local) TranslationOffset field, no separate "Global" variant.
		if (TryReadVector(*OffsetObjPtr, Offset)) { Settings->TranslationOffset = Offset; }
#endif
	}

	PelvisOp->SetSettings(PelvisBaseSettings);

	// Optional pelvis bone reassignment (separate setters on the controller).
	FString SourcePelvis, TargetPelvis;
	if (Params->TryGetStringField(TEXT("source_pelvis_bone"), SourcePelvis) && !SourcePelvis.IsEmpty())
	{
		PelvisController->SetSourcePelvisBone(FName(*SourcePelvis));
	}
	if (Params->TryGetStringField(TEXT("target_pelvis_bone"), TargetPelvis) && !TargetPelvis.IsEmpty())
	{
		PelvisController->SetTargetPelvisBone(FName(*TargetPelvis));
	}

	Controller->GetAsset()->MarkPackageDirty();

	// Echo back the resulting settings for confirmation — read through the same base pointer
	// (no derived value-copy, same LNK2001 avoidance as above).
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("retargeter_path"), AssetPath);
	Root->SetNumberField(TEXT("pelvis_op_index"), PelvisOpIndex);
	Root->SetNumberField(TEXT("scale_horizontal"), Settings->ScaleHorizontal);
	Root->SetNumberField(TEXT("scale_vertical"), Settings->ScaleVertical);
	Root->SetNumberField(TEXT("affect_ik_horizontal"), Settings->AffectIKHorizontal);
	Root->SetNumberField(TEXT("affect_ik_vertical"), Settings->AffectIKVertical);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	Root->SetNumberField(TEXT("floor_constraint_weight"), Settings->FloorConstraintWeight);
#endif // UE 5.6's FIKRetargetPelvisMotionOpSettings has no floor constraint yet.
	Root->SetNumberField(TEXT("rotation_alpha"), Settings->RotationAlpha);
	Root->SetNumberField(TEXT("translation_alpha"), Settings->TranslationAlpha);
	Root->SetNumberField(TEXT("blend_to_source_translation"), Settings->BlendToSourceTranslation);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	Root->SetObjectField(TEXT("translation_offset_global"), VectorToJson(Settings->TranslationOffsetGlobal));
#else
	// UE 5.6 only has the (local) TranslationOffset field, no separate "Global" variant.
	Root->SetObjectField(TEXT("translation_offset_global"), VectorToJson(Settings->TranslationOffset));
#endif
	Root->SetStringField(TEXT("source_pelvis_bone"), PelvisController->GetSourcePelvisBone().ToString());
	Root->SetStringField(TEXT("target_pelvis_bone"), PelvisController->GetTargetPelvisBone().ToString());
	return FMonolithActionResult::Success(Root);
}

// ===========================================================================
//  T1-R5 — enable_foot_ground_lock (Speed Planting op + SnapBoneToGround)
// ===========================================================================

FMonolithActionResult FMonolithRetargetSettingsActions::HandleEnableFootGroundLock(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("retargeter_path"));

	FString Error;
	UIKRetargeterController* Controller = ResolveRetargeterController(AssetPath, Error);
	if (!Controller) { return FMonolithActionResult::Error(Error); }

	// chains[]: array of {target_chain, speed_curve} OR array of bare chain-name strings.
	const TArray<TSharedPtr<FJsonValue>>* ChainsArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("chains"), ChainsArr) || ChainsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT(
			"'chains' is required — an array of {target_chain, speed_curve} objects (or bare chain-name strings) to speed-plant"));
	}

	int32 SpeedOpIndex = INDEX_NONE;
	UIKRetargetSpeedPlantingController* SpeedController =
		Cast<UIKRetargetSpeedPlantingController>(ResolveOpController(Controller, FIKRetargetSpeedPlantingOp::StaticStruct(), SpeedOpIndex));
	if (!SpeedController)
	{
		return FMonolithActionResult::Error(TEXT(
			"No Speed Planting op in the retargeter op stack. Add a Speed Planting op (or seed extended default ops) first."));
	}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	// FIKRetargetSpeedPlantingOpSettings cannot be value-constructed outside the IKRig module on
	// UE 5.8: its PostLoad(FIKRigObjectVersion::Type) override ships WITHOUT IKRIG_API
	// (Engine .../IKRig/.../RetargetOps/SpeedPlantingOp.h:109), so a locally-emitted vtable
	// references an unexported symbol (LNK2001). UIKRetargetSpeedPlantingController::GetSettings()
	// returns BY VALUE, forcing that construction. Unavailable on 5.8 pending an Epic export fix.
	// See Docs/assessment/build-breaks-5.8.md (RC7).
	return FMonolithActionResult::Error(TEXT(
		"enable_foot_ground_lock is unavailable on UE 5.8: the engine ships "
		"FIKRetargetSpeedPlantingOpSettings::PostLoad without IKRIG_API export, so its settings "
		"struct cannot be constructed outside the IKRig module. Pending an Epic engine fix."));
#else
	const FScopedTransaction Transaction(FText::FromString(TEXT("Enable Foot Ground Lock")));
	Controller->GetAsset()->Modify();

	// Op settings are returned BY VALUE — mutate the copy, then SetSettings() it back.
	FIKRetargetSpeedPlantingOpSettings Settings = SpeedController->GetSettings();
	Settings.ChainsToSpeedPlant.Reset();

	TArray<FString> ResolvedChains;
	for (const TSharedPtr<FJsonValue>& V : *ChainsArr)
	{
		if (!V.IsValid()) { continue; }

		FString TargetChain;
		FString SpeedCurve;

		const TSharedPtr<FJsonObject>* EntryPtr = nullptr;
		if (V->TryGetObject(EntryPtr) && EntryPtr->IsValid())
		{
			(*EntryPtr)->TryGetStringField(TEXT("target_chain"), TargetChain);
			(*EntryPtr)->TryGetStringField(TEXT("speed_curve"), SpeedCurve);
		}
		else
		{
			V->TryGetString(TargetChain);
		}

		if (TargetChain.IsEmpty()) { continue; }

		FRetargetSpeedPlantingSettings Entry{ FName(*TargetChain) };
		if (!SpeedCurve.IsEmpty()) { Entry.SpeedCurveName = FName(*SpeedCurve); }
		Settings.ChainsToSpeedPlant.Add(Entry);
		ResolvedChains.Add(TargetChain);
	}

	if (ResolvedChains.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No valid chain entries found in 'chains' (each needs a non-empty target_chain)"));
	}

	double SpeedThreshold = 0.0;
	if (Params->TryGetNumberField(TEXT("speed_threshold"), SpeedThreshold)) { Settings.SpeedThreshold = SpeedThreshold; }
	double Stiffness = 0.0;
	if (Params->TryGetNumberField(TEXT("stiffness"), Stiffness)) { Settings.Stiffness = Stiffness; }
#if !(ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
	// FIKRetargetSpeedPlantingOpSettings::CriticalDamping was removed in UE 5.8
	// (speed-planting reworked to a spring model: SpringStrength / ArrivalSpeedGain).
	double CriticalDamping = 0.0;
	if (Params->TryGetNumberField(TEXT("critical_damping"), CriticalDamping)) { Settings.CriticalDamping = CriticalDamping; }
#endif

	SpeedController->SetSettings(Settings);

	// Optional ground snap on the target skeleton.
	FString SnapBone;
	bool bSnapped = false;
	if (Params->TryGetStringField(TEXT("snap_to_ground_bone"), SnapBone) && !SnapBone.IsEmpty())
	{
		const ERetargetSourceOrTarget Side = ReadSide(Params, TEXT("side"), ERetargetSourceOrTarget::Target);
		Controller->SnapBoneToGround(FName(*SnapBone), Side);
		bSnapped = true;
	}

	Controller->GetAsset()->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("retargeter_path"), AssetPath);
	Root->SetNumberField(TEXT("speed_planting_op_index"), SpeedOpIndex);
	{
		TArray<TSharedPtr<FJsonValue>> ChainArr;
		for (const FString& C : ResolvedChains) { ChainArr.Add(MakeShared<FJsonValueString>(C)); }
		Root->SetArrayField(TEXT("speed_plant_chains"), ChainArr);
	}
	Root->SetNumberField(TEXT("speed_threshold"), Settings.SpeedThreshold);
	Root->SetNumberField(TEXT("stiffness"), Settings.Stiffness);
#if !(ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
	Root->SetNumberField(TEXT("critical_damping"), Settings.CriticalDamping);
#endif
	if (bSnapped) { Root->SetStringField(TEXT("snapped_to_ground_bone"), SnapBone); }
	return FMonolithActionResult::Success(Root);
#endif // 5.8 foot-ground-lock gate
}

// ===========================================================================
//  Registration
// ===========================================================================

void FMonolithRetargetSettingsActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// --- align_retarget_pose (T1-R1) ---
	Registry.RegisterAction(TEXT("animation"), TEXT("align_retarget_pose"),
		TEXT("Automatically align bones in the current retarget pose of an IK Retargeter (AutoAlignAllBones / AutoAlignBones), optionally snapping a reference bone to the ground (SnapBoneToGround). mode: 'auto_align_all' (default, all mapped chains), 'align_to_source' (chain-to-chain), or 'align_chain' (a chain_names[] subset). side: 'source', 'target', or 'both' (default). Optional method: ChainToChain (default) / MeshToMesh / LocalRotationAxes / GlobalRotationAxes."),
		FMonolithActionHandler::CreateStatic(&HandleAlignRetargetPose),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("retargeter_path"), TEXT("IK Retargeter (UIKRetargeter) asset path"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("'auto_align_all' (default), 'align_to_source', or 'align_chain'"), TEXT("auto_align_all"))
			.Optional(TEXT("side"), TEXT("string"), TEXT("'source', 'target', or 'both' (default)"), TEXT("both"))
			.Optional(TEXT("method"), TEXT("string"), TEXT("Alignment method: ChainToChain (default), MeshToMesh, LocalRotationAxes, GlobalRotationAxes"))
			.Optional(TEXT("chain_names"), TEXT("array"), TEXT("For mode 'align_chain': array of bone names to align (bones not in mapped chains are ignored)"))
			.Optional(TEXT("snap_to_ground_bone"), TEXT("string"), TEXT("Optional reference bone to snap to the ground (vertical offset) after aligning"))
			.Build());

	// --- get_retarget_pose (T1-R2) ---
	Registry.RegisterAction(TEXT("animation"), TEXT("get_retarget_pose"),
		TEXT("Read the current retarget pose of an IK Retargeter: per-bone rotation deltas (quaternion + euler) and the root/pelvis translation delta, for the source or target skeleton. Also lists the available pose names."),
		FMonolithActionHandler::CreateStatic(&HandleGetRetargetPose),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("retargeter_path"), TEXT("IK Retargeter (UIKRetargeter) asset path"))
			.Optional(TEXT("side"), TEXT("string"), TEXT("'source' or 'target' (default 'target')"), TEXT("target"))
			.Build());

	// --- set_retarget_pose (T1-R2) ---
	Registry.RegisterAction(TEXT("animation"), TEXT("set_retarget_pose"),
		TEXT("Author the current retarget pose of an IK Retargeter. mode 'from_reference' resets bones to the reference pose (optional 'bones' subset; empty = whole pose). mode 'bone_deltas' applies per-bone local rotation offsets from a bone_deltas[] of {bone, rotation:{x,y,z,w}|{pitch,yaw,roll}}. An optional root_translation_delta {x,y,z} sets the pelvis translation offset. NOTE: 'from_animation' (import pose from an animation frame) is NOT supported in v1 — that path is editor-UI-glued."),
		FMonolithActionHandler::CreateStatic(&HandleSetRetargetPose),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("retargeter_path"), TEXT("IK Retargeter (UIKRetargeter) asset path"))
			.Optional(TEXT("side"), TEXT("string"), TEXT("'source' or 'target' (default 'target')"), TEXT("target"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("'from_reference' or 'bone_deltas' (default). 'from_animation' is deferred in v1."), TEXT("bone_deltas"))
			.Optional(TEXT("bones"), TEXT("array"), TEXT("For mode 'from_reference': bone names to reset; empty/absent resets the whole pose"))
			.Optional(TEXT("bone_deltas"), TEXT("array"), TEXT("For mode 'bone_deltas': array of {bone, rotation:{x,y,z,w}|{pitch,yaw,roll}} local rotation offsets"))
			.Optional(TEXT("root_translation_delta"), TEXT("object"), TEXT("Optional {x,y,z} global translation offset applied to the pelvis bone"))
			.Build());

	// --- get_retarget_chain_settings (T1-R3) ---
	Registry.RegisterAction(TEXT("animation"), TEXT("get_retarget_chain_settings"),
		TEXT("Read the per-chain FK and IK settings from an IK Retargeter's op stack (FK Chains op + IK Chains op, reached via GetOpController). FK fields: enable_fk, fk_rotation_mode, rotation_alpha, translation_mode, translation_alpha. IK fields: ik_enabled, blend_to_source, static_offset. Optional chain_name filters to one target chain."),
		FMonolithActionHandler::CreateStatic(&HandleGetRetargetChainSettings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("retargeter_path"), TEXT("IK Retargeter (UIKRetargeter) asset path"))
			.Optional(TEXT("chain_name"), TEXT("string"), TEXT("Optional: filter to a single target chain by name"))
			.Build());

	// --- set_retarget_chain_settings (T1-R3) ---
	Registry.RegisterAction(TEXT("animation"), TEXT("set_retarget_chain_settings"),
		TEXT("Set per-chain FK/IK settings on an IK Retargeter's op stack for a single target chain (copy-mutate-set via the FK/IK Chains op controllers). FK: fk_rotation_mode (None/Interpolated/OneToOne/OneToOneReversed/MatchChain/MatchScaledChain/CopyLocal — parsed by name), translation_mode (None/GloballyScaled/Absolute/StretchBoneLengthUniformly/StretchBoneLengthNonUniformly/OrientAndScale), translation_scale, rotation_alpha, enable_fk. IK: ik_enabled, blend_to_source, static_offset/pole_vector {x,y,z}. Errors clearly if the relevant op is absent from the stack."),
		FMonolithActionHandler::CreateStatic(&HandleSetRetargetChainSettings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("retargeter_path"), TEXT("IK Retargeter (UIKRetargeter) asset path"))
			.Required(TEXT("chain_name"), TEXT("string"), TEXT("Target chain name to modify"))
			.Optional(TEXT("fk_rotation_mode"), TEXT("string"), TEXT("FK rotation mode (parsed by name): None, Interpolated, OneToOne, OneToOneReversed, MatchChain, MatchScaledChain, CopyLocal"))
			.Optional(TEXT("translation_mode"), TEXT("string"), TEXT("FK translation mode: None, GloballyScaled, Absolute, StretchBoneLengthUniformly, StretchBoneLengthNonUniformly, OrientAndScale"))
			.Optional(TEXT("translation_scale"), TEXT("number"), TEXT("FK translation alpha (0..1+) scaling the applied translation"))
			.Optional(TEXT("rotation_alpha"), TEXT("number"), TEXT("FK rotation alpha (0..1) blending base->retargeted rotation"))
			.Optional(TEXT("enable_fk"), TEXT("bool"), TEXT("Toggle FK copying for this chain"))
			.Optional(TEXT("ik_enabled"), TEXT("bool"), TEXT("Toggle IK goal modification for this chain"))
			.Optional(TEXT("blend_to_source"), TEXT("number"), TEXT("IK blend-to-source (0..1): 0=retargeted goal, 1=source bone transform"))
			.Optional(TEXT("static_offset"), TEXT("object"), TEXT("IK goal static global-space offset {x,y,z}"))
			.Optional(TEXT("pole_vector"), TEXT("object"), TEXT("Alias for static_offset {x,y,z} (applied as the IK goal static offset)"))
			.Build());

	// --- set_retarget_root_settings (T1-R5, Pelvis Motion Op) ---
	Registry.RegisterAction(TEXT("animation"), TEXT("set_retarget_root_settings"),
		TEXT("Set the Pelvis Motion op settings on an IK Retargeter (copy-mutate-set via UIKRetargetPelvisMotionController): scale_horizontal, scale_vertical, affect_ik_horizontal, affect_ik_vertical, floor_constraint_weight, rotation_alpha, translation_alpha, blend_to_source_translation, translation_offset_global {x,y,z}, and optional source_pelvis_bone / target_pelvis_bone reassignment. Errors clearly if no Pelvis Motion op is in the stack."),
		FMonolithActionHandler::CreateStatic(&HandleSetRetargetRootSettings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("retargeter_path"), TEXT("IK Retargeter (UIKRetargeter) asset path"))
			.Optional(TEXT("scale_horizontal"), TEXT("number"), TEXT("Scale of pelvis translation in the horizontal plane (X,Y). Default 1"))
			.Optional(TEXT("scale_vertical"), TEXT("number"), TEXT("Scale of pelvis translation in the vertical direction (Z). Default 1"))
			.Optional(TEXT("affect_ik_horizontal"), TEXT("number"), TEXT("0..1: how much pelvis modifications affect horizontal IK positions. Default 1"))
			.Optional(TEXT("affect_ik_vertical"), TEXT("number"), TEXT("0..1: how much pelvis modifications affect vertical IK positions. Default 0"))
			.Optional(TEXT("floor_constraint_weight"), TEXT("number"), TEXT("0..1: pelvis floor constraint weight (1 = ON). Default 0"))
			.Optional(TEXT("rotation_alpha"), TEXT("number"), TEXT("0..1: amount of retargeted pelvis rotation to apply. Default 1"))
			.Optional(TEXT("translation_alpha"), TEXT("number"), TEXT("0..1: amount of retargeted pelvis translation to apply. Default 1"))
			.Optional(TEXT("blend_to_source_translation"), TEXT("number"), TEXT("0..1: blend pelvis translation toward the exact source location. Default 0"))
			.Optional(TEXT("translation_offset_global"), TEXT("object"), TEXT("Static global-space translation offset {x,y,z} for the pelvis"))
			.Optional(TEXT("source_pelvis_bone"), TEXT("string"), TEXT("Optional: reassign the source pelvis bone by name"))
			.Optional(TEXT("target_pelvis_bone"), TEXT("string"), TEXT("Optional: reassign the target pelvis bone by name"))
			.Build());

	// --- enable_foot_ground_lock (T1-R5, Speed Planting Op + SnapBoneToGround) ---
	Registry.RegisterAction(TEXT("animation"), TEXT("enable_foot_ground_lock"),
		TEXT("Configure the Speed Planting op on an IK Retargeter to lock feet to the ground by speed (copy-mutate-set via UIKRetargetSpeedPlantingController). chains[] is an array of {target_chain, speed_curve} (or bare chain-name strings) to speed-plant; optional speed_threshold (default 15), stiffness (default 250), critical_damping (default 1). Optionally snaps a reference bone to the ground. Errors clearly if no Speed Planting op is in the stack."),
		FMonolithActionHandler::CreateStatic(&HandleEnableFootGroundLock),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("retargeter_path"), TEXT("IK Retargeter (UIKRetargeter) asset path"))
			.Required(TEXT("chains"), TEXT("array"), TEXT("Array of {target_chain, speed_curve} objects (or bare chain-name strings) to speed-plant. Each target chain must have an IK Goal."))
			.Optional(TEXT("speed_threshold"), TEXT("number"), TEXT("Max source-bone speed considered 'planted' (0..100). Default 15"))
			.Optional(TEXT("stiffness"), TEXT("number"), TEXT("Spring stiffness pulling IK after unplant. Default 250"))
			.Optional(TEXT("critical_damping"), TEXT("number"), TEXT("Spring damping (0..1, 1 = critically damped). Default 1"))
			.Optional(TEXT("snap_to_ground_bone"), TEXT("string"), TEXT("Optional reference bone to snap to the ground after configuring"))
			.Optional(TEXT("side"), TEXT("string"), TEXT("Skeleton for snap_to_ground_bone: 'source' or 'target' (default 'target')"), TEXT("target"))
			.Build());
}
