#include "MonolithAnimationPhysicsAssetActions.h"
#include "Runtime/Launch/Resources/Version.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "Editor.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"

#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsPublic.h"

#include "PhysicsAssetUtils.h"     // PhysicsUtilities: FPhysicsAssetUtils, FPhysAssetCreateParams
#include "MeshUtilitiesEngine.h"   // MeshUtilitiesEngine: FMeshUtilitiesEngine::CalcBoneVertInfos
#include "MeshUtilitiesCommon.h"   // MeshUtilitiesCommon: FBoneVertInfo (complete type)

namespace
{
	// -----------------------------------------------------------------------
	// File-local helpers. Names carry the PhysAssetAct prefix because the
	// module can be built as one unity blob.
	// -----------------------------------------------------------------------

	UPhysicsAsset* PhysAssetAct_Load(const FString& AssetPath, FString& OutError)
	{
		UPhysicsAsset* PhysAsset = FMonolithAssetUtils::LoadAssetByPath<UPhysicsAsset>(AssetPath);
		if (!PhysAsset)
		{
			OutError = FString::Printf(TEXT("Physics asset not found: %s"), *AssetPath);
		}
		return PhysAsset;
	}

	/** Resolve the mesh used for bone lookup and vertex fitting: explicit skeletal_mesh param, else the asset's preview mesh. */
	USkeletalMesh* PhysAssetAct_ResolveMesh(UPhysicsAsset* PhysAsset, const TSharedPtr<FJsonObject>& Params, FString& OutError)
	{
		FString MeshPath;
		if (Params->TryGetStringField(TEXT("skeletal_mesh"), MeshPath) && !MeshPath.IsEmpty())
		{
			USkeletalMesh* Mesh = FMonolithAssetUtils::LoadAssetByPath<USkeletalMesh>(MeshPath);
			if (!Mesh)
			{
				OutError = FString::Printf(TEXT("Skeletal mesh not found: %s"), *MeshPath);
			}
			return Mesh;
		}

		USkeletalMesh* Preview = PhysAsset->GetPreviewMesh();
		if (!Preview)
		{
			OutError = TEXT("Physics asset has no preview mesh. Pass skeletal_mesh explicitly.");
		}
		return Preview;
	}

	/** Read an object param that may arrive either as a JSON object or as a JSON-encoded string. */
	TSharedPtr<FJsonObject> PhysAssetAct_GetObjectParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name)
	{
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		if (Params->TryGetObjectField(Name, ObjPtr) && ObjPtr && ObjPtr->IsValid())
		{
			return *ObjPtr;
		}
		FString AsString;
		if (Params->TryGetStringField(Name, AsString) && !AsString.IsEmpty())
		{
			return FMonolithJsonUtils::Parse(AsString);
		}
		return nullptr;
	}

	bool PhysAssetAct_ParseGeomType(const FString& In, EPhysAssetFitGeomType& Out)
	{
		const FString S = In.ToLower();
		if (S == TEXT("capsule") || S == TEXT("sphyl"))                  { Out = EFG_Sphyl; return true; }
		if (S == TEXT("sphere"))                                          { Out = EFG_Sphere; return true; }
		if (S == TEXT("box"))                                             { Out = EFG_Box; return true; }
		if (S == TEXT("convex") || S == TEXT("single_convex_hull"))       { Out = EFG_SingleConvexHull; return true; }
		if (S == TEXT("multi_convex") || S == TEXT("multi_convex_hull"))  { Out = EFG_MultiConvexHull; return true; }
		return false;
	}

	FString PhysAssetAct_GeomTypeToString(EPhysAssetFitGeomType In)
	{
		switch (In)
		{
		case EFG_Sphyl:            return TEXT("capsule");
		case EFG_Sphere:           return TEXT("sphere");
		case EFG_Box:              return TEXT("box");
		case EFG_SingleConvexHull: return TEXT("single_convex_hull");
		case EFG_MultiConvexHull:  return TEXT("multi_convex_hull");
		default:                   return TEXT("other");
		}
	}

	/**
	 * Fill FPhysAssetCreateParams from an optional body_params object. Starts from the engine
	 * defaults (FPhysAssetCreateParams constructor). A top-level geom_type, if present, overrides
	 * body_params.geom_type. Returns false + error on an unrecognised enum string.
	 */
	bool PhysAssetAct_ReadCreateParams(const TSharedPtr<FJsonObject>& Params, FPhysAssetCreateParams& Out, FString& OutError)
	{
		Out = FPhysAssetCreateParams();

		TSharedPtr<FJsonObject> BP = PhysAssetAct_GetObjectParam(Params, TEXT("body_params"));
		FString GeomStr;
		if (BP.IsValid())
		{
			double Num = 0.0;
			bool bVal = false;
			FString Str;

			if (BP->TryGetNumberField(TEXT("min_bone_size"), Num)) { Out.MinBoneSize = static_cast<float>(Num); }
			if (BP->TryGetNumberField(TEXT("min_weld_size"), Num)) { Out.MinWeldSize = static_cast<float>(Num); }
			if (BP->TryGetNumberField(TEXT("lod_index"), Num))     { Out.LodIndex = FMath::Max(0, static_cast<int32>(Num)); }
			if (BP->TryGetNumberField(TEXT("hull_count"), Num))    { Out.HullCount = FMath::Clamp(static_cast<int32>(Num), 1, 64); }
			if (BP->TryGetNumberField(TEXT("max_hull_verts"), Num)){ Out.MaxHullVerts = FMath::Clamp(static_cast<int32>(Num), 4, 32); }
			if (BP->TryGetBoolField(TEXT("create_constraints"), bVal))            { Out.bCreateConstraints = bVal; }
			if (BP->TryGetBoolField(TEXT("auto_orient_to_bone"), bVal))           { Out.bAutoOrientToBone = bVal; }
			if (BP->TryGetBoolField(TEXT("walk_past_small"), bVal))               { Out.bWalkPastSmall = bVal; }
			if (BP->TryGetBoolField(TEXT("body_for_all"), bVal))                  { Out.bBodyForAll = bVal; }
			if (BP->TryGetBoolField(TEXT("disable_collisions_by_default"), bVal)) { Out.bDisableCollisionsByDefault = bVal; }

			BP->TryGetStringField(TEXT("geom_type"), GeomStr);

			if (BP->TryGetStringField(TEXT("vertex_weighting"), Str) && !Str.IsEmpty())
			{
				const FString L = Str.ToLower();
				if (L == TEXT("dominant") || L == TEXT("dominant_weight"))  { Out.VertWeight = EVW_DominantWeight; }
				else if (L == TEXT("any") || L == TEXT("any_weight"))       { Out.VertWeight = EVW_AnyWeight; }
				else
				{
					OutError = FString::Printf(TEXT("Unknown vertex_weighting '%s'. Use dominant or any."), *Str);
					return false;
				}
			}

			if (BP->TryGetStringField(TEXT("angular_constraint_mode"), Str) && !Str.IsEmpty())
			{
				const FString L = Str.ToLower();
				if (L == TEXT("limited"))     { Out.AngularConstraintMode = ACM_Limited; }
				else if (L == TEXT("free"))   { Out.AngularConstraintMode = ACM_Free; }
				else if (L == TEXT("locked")) { Out.AngularConstraintMode = ACM_Locked; }
				else
				{
					OutError = FString::Printf(TEXT("Unknown angular_constraint_mode '%s'. Use limited, free or locked."), *Str);
					return false;
				}
			}
		}

		FString TopGeom;
		if (Params->TryGetStringField(TEXT("geom_type"), TopGeom) && !TopGeom.IsEmpty())
		{
			GeomStr = TopGeom;
		}
		if (!GeomStr.IsEmpty())
		{
			EPhysAssetFitGeomType Geom = EFG_Sphyl;
			if (!PhysAssetAct_ParseGeomType(GeomStr, Geom))
			{
				OutError = FString::Printf(TEXT("Unknown geom_type '%s'. Use capsule, sphere, box, single_convex_hull or multi_convex_hull."), *GeomStr);
				return false;
			}
			Out.GeomType = Geom;
		}
		return true;
	}

	/** Mirror of the physics asset editor's constraint init: child = Bone1, parent = Bone2, snapped to the child bone, collision between the pair disabled. */
	void PhysAssetAct_InitConstraint(UPhysicsAsset* PhysAsset, UPhysicsConstraintTemplate* Constraint, int32 ChildBodyIndex, int32 ParentBodyIndex, bool bDisableCollision)
	{
		Constraint->Modify();
		Constraint->DefaultInstance.ConstraintBone1 = PhysAsset->SkeletalBodySetups[ChildBodyIndex]->BoneName;
		Constraint->DefaultInstance.ConstraintBone2 = PhysAsset->SkeletalBodySetups[ParentBodyIndex]->BoneName;
		Constraint->DefaultInstance.SnapTransformsToDefault(EConstraintTransformComponentFlags::All, PhysAsset);
		// Same angular default as FPhysicsAssetUtils::CreateFromSkeletalMesh (FPhysAssetCreateParams::AngularConstraintMode).
		Constraint->DefaultInstance.SetAngularSwing1Motion(ACM_Limited);
		Constraint->DefaultInstance.SetAngularSwing2Motion(ACM_Limited);
		Constraint->DefaultInstance.SetAngularTwistMotion(ACM_Limited);
		Constraint->SetDefaultProfile(Constraint->DefaultInstance);

		if (bDisableCollision)
		{
			PhysAsset->DisableCollision(ChildBodyIndex, ParentBodyIndex);
		}
	}

	/** Find a constraint joining two bones in either order. */
	int32 PhysAssetAct_FindConstraintByPair(const UPhysicsAsset* PhysAsset, FName BoneA, FName BoneB)
	{
		for (int32 i = 0; i < PhysAsset->ConstraintSetup.Num(); ++i)
		{
			const UPhysicsConstraintTemplate* CT = PhysAsset->ConstraintSetup[i];
			if (!CT) continue;
			const FConstraintInstance& CI = CT->DefaultInstance;
			if ((CI.ConstraintBone1 == BoneA && CI.ConstraintBone2 == BoneB) ||
				(CI.ConstraintBone1 == BoneB && CI.ConstraintBone2 == BoneA))
			{
				return i;
			}
		}
		return INDEX_NONE;
	}

	FString PhysAssetAct_MakeUniqueConstraintName(const UPhysicsAsset* PhysAsset, const FString& Preferred)
	{
		if (!Preferred.IsEmpty() && PhysAsset->FindConstraintIndex(FName(*Preferred)) == INDEX_NONE)
		{
			return Preferred;
		}
		const FString Base = Preferred.IsEmpty() ? FString(TEXT("UserConstraint")) : Preferred;
		int32 Suffix = 0;
		FString Candidate;
		do
		{
			Candidate = FString::Printf(TEXT("%s_%d"), *Base, Suffix++);
		}
		while (PhysAsset->FindConstraintIndex(FName(*Candidate)) != INDEX_NONE);
		return Candidate;
	}

	TSharedPtr<FJsonObject> PhysAssetAct_ConstraintToJson(const UPhysicsAsset* PhysAsset, int32 Index)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("index"), Index);
		if (const UPhysicsConstraintTemplate* CT = PhysAsset->ConstraintSetup.IsValidIndex(Index) ? PhysAsset->ConstraintSetup[Index].Get() : nullptr)
		{
			Obj->SetStringField(TEXT("name"), CT->DefaultInstance.JointName.ToString());
			Obj->SetStringField(TEXT("child_bone"), CT->DefaultInstance.ConstraintBone1.ToString());
			Obj->SetStringField(TEXT("parent_bone"), CT->DefaultInstance.ConstraintBone2.ToString());
		}
		return Obj;
	}

	void PhysAssetAct_FinishEdit(UPhysicsAsset* PhysAsset)
	{
		PhysAsset->UpdateBodySetupIndexMap();
		PhysAsset->UpdateBoundsBodiesArray();
		PhysAsset->RefreshPhysicsAssetChange();
		FPhysicsDelegates::OnPhysicsAssetChanged.Broadcast(PhysAsset);
		PhysAsset->MarkPackageDirty();
	}
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithAnimationPhysicsAssetActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	static const TCHAR* BodyParamsDesc =
		TEXT("Body generation settings (engine defaults when omitted): {min_bone_size: number (default 20), min_weld_size: number, ")
		TEXT("geom_type: capsule|sphere|box|single_convex_hull|multi_convex_hull (default capsule), vertex_weighting: dominant|any (default dominant), ")
		TEXT("create_constraints: bool (default true), auto_orient_to_bone: bool, walk_past_small: bool, body_for_all: bool, ")
		TEXT("disable_collisions_by_default: bool (default true), angular_constraint_mode: limited|free|locked, lod_index: int, hull_count: int, max_hull_verts: int}");

	Registry.RegisterAction(TEXT("animation"), TEXT("create_physics_asset"),
		TEXT("Create a new physics asset from a skeletal mesh, auto-generating bodies and constraints (same generator as the editor's Create Physics Asset). Does not assign it to the mesh unless assign_to_mesh is true."),
		FMonolithActionHandler::CreateStatic(&HandleCreatePhysicsAsset),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("skeletal_mesh"), TEXT("Source skeletal mesh asset path"), { TEXT("mesh_path") })
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Destination asset path for the new physics asset, e.g. /Game/Physics/PA_MyMesh"), { TEXT("path") })
			.Optional(TEXT("body_params"), TEXT("object"), BodyParamsDesc)
			.Optional(TEXT("geom_type"), TEXT("string"), TEXT("Shortcut for body_params.geom_type: capsule, sphere, box, single_convex_hull, multi_convex_hull"))
			.Optional(TEXT("assign_to_mesh"), TEXT("boolean"), TEXT("Set the new asset as the skeletal mesh's physics asset (dirties the mesh)"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("add_physics_body"),
		TEXT("Add a physics body for one bone, fitted to the vertices skinned to it. Creates a constraint to the nearest physical ancestor and re-parents constraints of physical descendants onto the new body, matching the physics asset editor."),
		FMonolithActionHandler::CreateStatic(&HandleAddPhysicsBody),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Physics asset path"), { TEXT("asset") })
			.Required(TEXT("bone"), TEXT("string"), TEXT("Bone name to add a body for"), { TEXT("bone_name") })
			.Optional(TEXT("geom_type"), TEXT("string"), TEXT("capsule (default), sphere, box, single_convex_hull, multi_convex_hull"))
			.Optional(TEXT("body_params"), TEXT("object"), BodyParamsDesc)
			.OptionalAssetPath(TEXT("skeletal_mesh"), TEXT("Mesh used for bone lookup and vertex fitting (default: the physics asset's preview mesh)"), { TEXT("mesh_path") })
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("remove_physics_body"),
		TEXT("Remove the physics body for a bone, together with every constraint attached to it"),
		FMonolithActionHandler::CreateStatic(&HandleRemovePhysicsBody),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Physics asset path"), { TEXT("asset") })
			.Required(TEXT("bone"), TEXT("string"), TEXT("Bone name of the body to remove"), { TEXT("bone_name") })
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("add_physics_constraint"),
		TEXT("Add a constraint between two existing bodies. bone_a is the child (constraint Bone1), bone_b the parent (Bone2). The joint frame snaps to the child bone and collision between the pair is disabled by default."),
		FMonolithActionHandler::CreateStatic(&HandleAddPhysicsConstraint),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Physics asset path"), { TEXT("asset") })
			.Required(TEXT("bone_a"), TEXT("string"), TEXT("Child body bone name"), { TEXT("child_bone"), TEXT("bone_1") })
			.Required(TEXT("bone_b"), TEXT("string"), TEXT("Parent body bone name"), { TEXT("parent_bone"), TEXT("bone_2") })
			.Optional(TEXT("constraint_name"), TEXT("string"), TEXT("Joint name (default: child bone name, uniquified if taken)"))
			.Optional(TEXT("disable_collision"), TEXT("boolean"), TEXT("Disable collision between the two bodies"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("remove_physics_constraint"),
		TEXT("Remove a constraint identified by constraint_index, constraint_name, or a bone_a + bone_b pair (either order). Pair collision state is left unchanged; use set_body_collision to re-enable it."),
		FMonolithActionHandler::CreateStatic(&HandleRemovePhysicsConstraint),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Physics asset path"), { TEXT("asset") })
			.Optional(TEXT("constraint_index"), TEXT("integer"), TEXT("Constraint index (see get_physics_asset_info)"))
			.Optional(TEXT("constraint_name"), TEXT("string"), TEXT("Constraint joint name"))
			.Optional(TEXT("bone_a"), TEXT("string"), TEXT("One bone of the pair"), { TEXT("bone_1"), TEXT("child_bone") })
			.Optional(TEXT("bone_b"), TEXT("string"), TEXT("Other bone of the pair"), { TEXT("bone_2"), TEXT("parent_bone") })
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("set_body_collision"),
		TEXT("Enable or disable collision between two bodies of a physics asset (the asset's collision-disable table)"),
		FMonolithActionHandler::CreateStatic(&HandleSetBodyCollision),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Physics asset path"), { TEXT("asset") })
			.Required(TEXT("body_a"), TEXT("string"), TEXT("Bone name of the first body"), { TEXT("bone_a") })
			.Required(TEXT("body_b"), TEXT("string"), TEXT("Bone name of the second body"), { TEXT("bone_b") })
			.Required(TEXT("enabled"), TEXT("boolean"), TEXT("true = the bodies collide, false = collision disabled"))
			.Build());
}

// ---------------------------------------------------------------------------
// create_physics_asset
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationPhysicsAssetActions::HandleCreatePhysicsAsset(const TSharedPtr<FJsonObject>& Params)
{
	const FString MeshPath = Params->GetStringField(TEXT("skeletal_mesh"));
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	USkeletalMesh* Mesh = FMonolithAssetUtils::LoadAssetByPath<USkeletalMesh>(MeshPath);
	if (!Mesh)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Skeletal mesh not found: %s"), *MeshPath));
	}
	// CreateFromSkeletalMesh asserts on missing render data.
	if (!Mesh->GetResourceForRendering())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Skeletal mesh '%s' has no render data (still compiling or failed to build); retry once it has finished building."), *MeshPath));
	}

	int32 LastSlash = INDEX_NONE;
	if (!AssetPath.StartsWith(TEXT("/")) || !AssetPath.FindLastChar(TEXT('/'), LastSlash) || LastSlash == AssetPath.Len() - 1)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset_path '%s'. Expected a long package path such as /Game/Physics/PA_MyMesh."), *AssetPath));
	}
	const FString AssetName = AssetPath.Mid(LastSlash + 1);
	if (AssetName.Contains(TEXT(".")))
	{
		return FMonolithActionResult::Error(TEXT("asset_path must be a package path without an object suffix (no '.')."));
	}

	// CreatePackage returns an existing in-memory package, so refuse up front.
	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));
	}

	FPhysAssetCreateParams CreateParams;
	FString Error;
	if (!PhysAssetAct_ReadCreateParams(Params, CreateParams, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	if (CreateParams.bCreateConstraints && !FPhysicsAssetUtils::CanCreateConstraints())
	{
		CreateParams.bCreateConstraints = false;
	}

	bool bAssignToMesh = false;
	Params->TryGetBoolField(TEXT("assign_to_mesh"), bAssignToMesh);

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));
	}

	UPhysicsAsset* PhysAsset = NewObject<UPhysicsAsset>(Pkg, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (!PhysAsset)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UPhysicsAsset object"));
	}

	if (bAssignToMesh)
	{
		Mesh->Modify();
	}

	FText CreateError;
	const bool bOk = FPhysicsAssetUtils::CreateFromSkeletalMesh(PhysAsset, Mesh, CreateParams, CreateError, /*bSetToMesh=*/bAssignToMesh, /*bShowProgress=*/false);
	if (!bOk)
	{
		// Leave nothing half-made behind: strip the object so the path stays free.
		PhysAsset->ClearFlags(RF_Public | RF_Standalone);
		PhysAsset->MarkAsGarbage();
		Pkg->SetDirtyFlag(false);
		return FMonolithActionResult::Error(FString::Printf(TEXT("Physics asset generation failed: %s. Try a smaller body_params.min_bone_size or body_for_all=true."),
			CreateError.IsEmpty() ? TEXT("no bodies could be created") : *CreateError.ToString()));
	}

	PhysAsset->SetPreviewMesh(Mesh, /*bMarkAsDirty=*/false);
	FAssetRegistryModule::AssetCreated(PhysAsset);
	PhysAsset->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), PhysAsset->GetPathName());
	Root->SetStringField(TEXT("skeletal_mesh"), Mesh->GetPathName());
	Root->SetNumberField(TEXT("body_count"), PhysAsset->SkeletalBodySetups.Num());
	Root->SetNumberField(TEXT("constraint_count"), PhysAsset->ConstraintSetup.Num());
	Root->SetStringField(TEXT("geom_type"), PhysAssetAct_GeomTypeToString(CreateParams.GeomType));
	Root->SetNumberField(TEXT("min_bone_size"), CreateParams.MinBoneSize);
	Root->SetBoolField(TEXT("assigned_to_mesh"), bAssignToMesh);

	TArray<TSharedPtr<FJsonValue>> Bodies;
	for (const USkeletalBodySetup* BS : PhysAsset->SkeletalBodySetups)
	{
		if (BS)
		{
			Bodies.Add(MakeShared<FJsonValueString>(BS->BoneName.ToString()));
		}
	}
	Root->SetArrayField(TEXT("bodies"), Bodies);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// add_physics_body
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationPhysicsAssetActions::HandleAddPhysicsBody(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString BoneStr = Params->GetStringField(TEXT("bone"));
	if (BoneStr.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("bone is required"));
	}
	const FName BoneName(*BoneStr);

	FString Error;
	UPhysicsAsset* PhysAsset = PhysAssetAct_Load(AssetPath, Error);
	if (!PhysAsset) return FMonolithActionResult::Error(Error);

	USkeletalMesh* Mesh = PhysAssetAct_ResolveMesh(PhysAsset, Params, Error);
	if (!Mesh) return FMonolithActionResult::Error(Error);
	if (!Mesh->GetResourceForRendering())
	{
		return FMonolithActionResult::Error(TEXT("Skeletal mesh has no render data yet; retry once it has finished building."));
	}

	const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
	const int32 BoneIndex = RefSkel.FindBoneIndex(BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Bone '%s' not found on skeletal mesh %s"), *BoneStr, *Mesh->GetPathName()));
	}
	if (PhysAsset->FindBodyIndex(BoneName) != INDEX_NONE)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("A body for bone '%s' already exists. Use remove_physics_body first to replace it."), *BoneStr));
	}

	FPhysAssetCreateParams CreateParams;
	if (!PhysAssetAct_ReadCreateParams(Params, CreateParams, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	const bool bCreateConstraints = CreateParams.bCreateConstraints && FPhysicsAssetUtils::CanCreateConstraints();

	TArray<FBoneVertInfo> Infos;
	FMeshUtilitiesEngine::CalcBoneVertInfos(Mesh, Infos, CreateParams.VertWeight == EVW_DominantWeight, CreateParams.LodIndex);
	if (!Infos.IsValidIndex(BoneIndex))
	{
		return FMonolithActionResult::Error(TEXT("Could not read skinned vertex data from the mesh (render data has no CPU-accessible vertex/skin-weight buffers for that LOD)."));
	}

	// Nearest physical ancestor, resolved before the new body exists.
	const int32 ParentBodyIndex = PhysAsset->FindControllingBodyIndex(Mesh, BoneIndex);

	const FScopedTransaction Transaction(FText::FromString(TEXT("Add Physics Body")));
	PhysAsset->Modify();

	const int32 NewBodyIndex = FPhysicsAssetUtils::CreateNewBody(PhysAsset, BoneName, CreateParams);
	if (!PhysAsset->SkeletalBodySetups.IsValidIndex(NewBodyIndex) || !PhysAsset->SkeletalBodySetups[NewBodyIndex])
	{
		return FMonolithActionResult::Error(TEXT("FPhysicsAssetUtils::CreateNewBody failed"));
	}
	USkeletalBodySetup* BodySetup = PhysAsset->SkeletalBodySetups[NewBodyIndex];
	BodySetup->Modify();

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	const bool bShapeOk = FPhysicsAssetUtils::CreateCollisionFromBone(BodySetup, Mesh, BoneIndex, CreateParams, Infos[BoneIndex], PhysAsset);
#else
	const bool bShapeOk = FPhysicsAssetUtils::CreateCollisionFromBone(BodySetup, Mesh, BoneIndex, CreateParams, Infos[BoneIndex]);
#endif
	if (!bShapeOk)
	{
		FPhysicsAssetUtils::DestroyBody(PhysAsset, NewBodyIndex);
		PhysAssetAct_FinishEdit(PhysAsset);
		return FMonolithActionResult::Error(FString::Printf(TEXT("Could not fit a %s to bone '%s' (no usable skinned vertices). Try vertex_weighting=any or a different geom_type."),
			*PhysAssetAct_GeomTypeToString(CreateParams.GeomType), *BoneStr));
	}

	TArray<TSharedPtr<FJsonValue>> CreatedConstraints;
	TArray<TSharedPtr<FJsonValue>> ReparentedConstraints;

	// Physical descendants whose nearest physical ancestor is now the new body: point their constraint at it.
	for (int32 OtherBody = 0; OtherBody < PhysAsset->SkeletalBodySetups.Num(); ++OtherBody)
	{
		if (OtherBody == NewBodyIndex || !PhysAsset->SkeletalBodySetups[OtherBody]) continue;
		const FName OtherBone = PhysAsset->SkeletalBodySetups[OtherBody]->BoneName;
		const int32 OtherBoneIndex = RefSkel.FindBoneIndex(OtherBone);
		if (OtherBoneIndex == INDEX_NONE || !RefSkel.BoneIsChildOf(OtherBoneIndex, BoneIndex)) continue;

		const int32 OtherParentBone = RefSkel.GetParentIndex(OtherBoneIndex);
		if (OtherParentBone == INDEX_NONE || PhysAsset->FindControllingBodyIndex(Mesh, OtherParentBone) != NewBodyIndex) continue;

		int32 ConstraintIndex = PhysAsset->FindConstraintIndex(OtherBone);
		if (ConstraintIndex == INDEX_NONE)
		{
			if (!bCreateConstraints) continue;
			ConstraintIndex = FPhysicsAssetUtils::CreateNewConstraint(PhysAsset, OtherBone);
			if (ConstraintIndex == INDEX_NONE) continue;
			PhysAssetAct_InitConstraint(PhysAsset, PhysAsset->ConstraintSetup[ConstraintIndex], OtherBody, NewBodyIndex, /*bDisableCollision=*/true);
			CreatedConstraints.Add(MakeShared<FJsonValueObject>(PhysAssetAct_ConstraintToJson(PhysAsset, ConstraintIndex)));
		}
		else
		{
			UPhysicsConstraintTemplate* Existing = PhysAsset->ConstraintSetup[ConstraintIndex];
			if (!Existing || Existing->DefaultInstance.ConstraintBone2 == BoneName) continue;

			// The constraint previously skipped over this bone to a higher body: restore collision with that body.
			const int32 OldParentBody = PhysAsset->FindBodyIndex(Existing->DefaultInstance.ConstraintBone2);
			if (OldParentBody != INDEX_NONE && OldParentBody == ParentBodyIndex)
			{
				PhysAsset->EnableCollision(OtherBody, OldParentBody);
			}
			PhysAssetAct_InitConstraint(PhysAsset, Existing, OtherBody, NewBodyIndex, /*bDisableCollision=*/true);
			ReparentedConstraints.Add(MakeShared<FJsonValueObject>(PhysAssetAct_ConstraintToJson(PhysAsset, ConstraintIndex)));
		}
	}

	// Joint to the nearest physical ancestor.
	if (ParentBodyIndex != INDEX_NONE && bCreateConstraints)
	{
		const int32 ConstraintIndex = FPhysicsAssetUtils::CreateNewConstraint(PhysAsset, BoneName);
		if (PhysAsset->ConstraintSetup.IsValidIndex(ConstraintIndex))
		{
			PhysAssetAct_InitConstraint(PhysAsset, PhysAsset->ConstraintSetup[ConstraintIndex], NewBodyIndex, ParentBodyIndex, /*bDisableCollision=*/true);
			CreatedConstraints.Add(MakeShared<FJsonValueObject>(PhysAssetAct_ConstraintToJson(PhysAsset, ConstraintIndex)));
		}
	}

	PhysAssetAct_FinishEdit(PhysAsset);

	const FKAggregateGeom& Agg = BodySetup->AggGeom;
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("bone"), BoneStr);
	Root->SetNumberField(TEXT("body_index"), PhysAsset->FindBodyIndex(BoneName));
	Root->SetStringField(TEXT("geom_type"), PhysAssetAct_GeomTypeToString(CreateParams.GeomType));
	Root->SetNumberField(TEXT("shape_count"), Agg.GetElementCount());
	Root->SetStringField(TEXT("parent_body"), ParentBodyIndex != INDEX_NONE && PhysAsset->SkeletalBodySetups.IsValidIndex(ParentBodyIndex)
		? PhysAsset->SkeletalBodySetups[ParentBodyIndex]->BoneName.ToString() : FString());
	Root->SetArrayField(TEXT("created_constraints"), CreatedConstraints);
	Root->SetArrayField(TEXT("reparented_constraints"), ReparentedConstraints);
	Root->SetNumberField(TEXT("body_count"), PhysAsset->SkeletalBodySetups.Num());
	Root->SetNumberField(TEXT("constraint_count"), PhysAsset->ConstraintSetup.Num());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// remove_physics_body
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationPhysicsAssetActions::HandleRemovePhysicsBody(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString BoneStr = Params->GetStringField(TEXT("bone"));

	FString Error;
	UPhysicsAsset* PhysAsset = PhysAssetAct_Load(AssetPath, Error);
	if (!PhysAsset) return FMonolithActionResult::Error(Error);

	const int32 BodyIndex = PhysAsset->FindBodyIndex(FName(*BoneStr));
	if (BodyIndex == INDEX_NONE)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No body for bone '%s' in %s"), *BoneStr, *AssetPath));
	}

	TArray<int32> AttachedConstraints;
	PhysAsset->BodyFindConstraints(BodyIndex, AttachedConstraints);
	TArray<TSharedPtr<FJsonValue>> RemovedConstraints;
	for (int32 CI : AttachedConstraints)
	{
		RemovedConstraints.Add(MakeShared<FJsonValueObject>(PhysAssetAct_ConstraintToJson(PhysAsset, CI)));
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("Remove Physics Body")));
	PhysAsset->Modify();
	FPhysicsAssetUtils::DestroyBody(PhysAsset, BodyIndex);
	PhysAssetAct_FinishEdit(PhysAsset);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("removed_body"), BoneStr);
	Root->SetArrayField(TEXT("removed_constraints"), RemovedConstraints);
	Root->SetNumberField(TEXT("body_count"), PhysAsset->SkeletalBodySetups.Num());
	Root->SetNumberField(TEXT("constraint_count"), PhysAsset->ConstraintSetup.Num());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// add_physics_constraint
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationPhysicsAssetActions::HandleAddPhysicsConstraint(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString ChildStr = Params->GetStringField(TEXT("bone_a"));
	const FString ParentStr = Params->GetStringField(TEXT("bone_b"));

	FString Error;
	UPhysicsAsset* PhysAsset = PhysAssetAct_Load(AssetPath, Error);
	if (!PhysAsset) return FMonolithActionResult::Error(Error);

	if (ChildStr.IsEmpty() || ParentStr.IsEmpty() || ChildStr == ParentStr)
	{
		return FMonolithActionResult::Error(TEXT("bone_a (child) and bone_b (parent) must name two different bodies"));
	}

	const int32 ChildBody = PhysAsset->FindBodyIndex(FName(*ChildStr));
	const int32 ParentBody = PhysAsset->FindBodyIndex(FName(*ParentStr));
	if (ChildBody == INDEX_NONE || ParentBody == INDEX_NONE)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Both bones need a body. Missing: %s%s%s. Use add_physics_body first."),
			ChildBody == INDEX_NONE ? *ChildStr : TEXT(""),
			(ChildBody == INDEX_NONE && ParentBody == INDEX_NONE) ? TEXT(", ") : TEXT(""),
			ParentBody == INDEX_NONE ? *ParentStr : TEXT("")));
	}

	const int32 ExistingIndex = PhysAssetAct_FindConstraintByPair(PhysAsset, FName(*ChildStr), FName(*ParentStr));
	if (ExistingIndex != INDEX_NONE)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("A constraint between '%s' and '%s' already exists (index %d). Edit it with set_constraint_properties."),
			*ChildStr, *ParentStr, ExistingIndex));
	}

	if (!FPhysicsAssetUtils::CanCreateConstraints())
	{
		return FMonolithActionResult::Error(TEXT("Constraint creation is blocked by the asset-class permission list (UPhysicsConstraintTemplate filtered)."));
	}

	FString RequestedName;
	Params->TryGetStringField(TEXT("constraint_name"), RequestedName);
	const FString ConstraintName = PhysAssetAct_MakeUniqueConstraintName(PhysAsset, RequestedName.IsEmpty() ? ChildStr : RequestedName);

	bool bDisableCollision = true;
	Params->TryGetBoolField(TEXT("disable_collision"), bDisableCollision);

	const FScopedTransaction Transaction(FText::FromString(TEXT("Add Physics Constraint")));
	PhysAsset->Modify();

	const int32 NewIndex = FPhysicsAssetUtils::CreateNewConstraint(PhysAsset, FName(*ConstraintName));
	if (!PhysAsset->ConstraintSetup.IsValidIndex(NewIndex) || !PhysAsset->ConstraintSetup[NewIndex])
	{
		return FMonolithActionResult::Error(TEXT("FPhysicsAssetUtils::CreateNewConstraint failed"));
	}
	PhysAssetAct_InitConstraint(PhysAsset, PhysAsset->ConstraintSetup[NewIndex], ChildBody, ParentBody, bDisableCollision);
	PhysAssetAct_FinishEdit(PhysAsset);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetObjectField(TEXT("constraint"), PhysAssetAct_ConstraintToJson(PhysAsset, NewIndex));
	Root->SetBoolField(TEXT("collision_enabled"), PhysAsset->IsCollisionEnabled(ChildBody, ParentBody));
	Root->SetNumberField(TEXT("constraint_count"), PhysAsset->ConstraintSetup.Num());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// remove_physics_constraint
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationPhysicsAssetActions::HandleRemovePhysicsConstraint(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	FString Error;
	UPhysicsAsset* PhysAsset = PhysAssetAct_Load(AssetPath, Error);
	if (!PhysAsset) return FMonolithActionResult::Error(Error);

	int32 ConstraintIndex = INDEX_NONE;
	double IdxVal = 0.0;
	FString NameStr, BoneA, BoneB;
	if (Params->TryGetNumberField(TEXT("constraint_index"), IdxVal))
	{
		ConstraintIndex = static_cast<int32>(IdxVal);
	}
	else if (Params->TryGetStringField(TEXT("constraint_name"), NameStr) && !NameStr.IsEmpty())
	{
		ConstraintIndex = PhysAsset->FindConstraintIndex(FName(*NameStr));
	}
	else if (Params->TryGetStringField(TEXT("bone_a"), BoneA) && Params->TryGetStringField(TEXT("bone_b"), BoneB)
		&& !BoneA.IsEmpty() && !BoneB.IsEmpty())
	{
		ConstraintIndex = PhysAssetAct_FindConstraintByPair(PhysAsset, FName(*BoneA), FName(*BoneB));
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Provide constraint_index, constraint_name, or bone_a + bone_b."));
	}

	if (!PhysAsset->ConstraintSetup.IsValidIndex(ConstraintIndex))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Constraint not found (asset has %d constraints; list them with get_physics_asset_info)."), PhysAsset->ConstraintSetup.Num()));
	}

	TSharedPtr<FJsonObject> Removed = PhysAssetAct_ConstraintToJson(PhysAsset, ConstraintIndex);

	const FScopedTransaction Transaction(FText::FromString(TEXT("Remove Physics Constraint")));
	PhysAsset->Modify();
	FPhysicsAssetUtils::DestroyConstraint(PhysAsset, ConstraintIndex);
	PhysAssetAct_FinishEdit(PhysAsset);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetObjectField(TEXT("removed_constraint"), Removed);
	Root->SetNumberField(TEXT("constraint_count"), PhysAsset->ConstraintSetup.Num());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// set_body_collision
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAnimationPhysicsAssetActions::HandleSetBodyCollision(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString BodyAStr = Params->GetStringField(TEXT("body_a"));
	const FString BodyBStr = Params->GetStringField(TEXT("body_b"));

	bool bEnabled = false;
	if (!Params->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		return FMonolithActionResult::Error(TEXT("enabled (boolean) is required"));
	}

	FString Error;
	UPhysicsAsset* PhysAsset = PhysAssetAct_Load(AssetPath, Error);
	if (!PhysAsset) return FMonolithActionResult::Error(Error);

	const int32 BodyA = PhysAsset->FindBodyIndex(FName(*BodyAStr));
	const int32 BodyB = PhysAsset->FindBodyIndex(FName(*BodyBStr));
	if (BodyA == INDEX_NONE || BodyB == INDEX_NONE)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Body not found: %s"), BodyA == INDEX_NONE ? *BodyAStr : *BodyBStr));
	}
	if (BodyA == BodyB)
	{
		return FMonolithActionResult::Error(TEXT("body_a and body_b must be different bodies"));
	}

	const bool bBefore = PhysAsset->IsCollisionEnabled(BodyA, BodyB);

	const FScopedTransaction Transaction(FText::FromString(TEXT("Set Body Collision")));
	PhysAsset->Modify();
	if (bEnabled)
	{
		PhysAsset->EnableCollision(BodyA, BodyB);
	}
	else
	{
		PhysAsset->DisableCollision(BodyA, BodyB);
	}
	PhysAssetAct_FinishEdit(PhysAsset);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("body_a"), BodyAStr);
	Root->SetStringField(TEXT("body_b"), BodyBStr);
	Root->SetBoolField(TEXT("was_enabled"), bBefore);
	Root->SetBoolField(TEXT("collision_enabled"), PhysAsset->IsCollisionEnabled(BodyA, BodyB));
	return FMonolithActionResult::Success(Root);
}
