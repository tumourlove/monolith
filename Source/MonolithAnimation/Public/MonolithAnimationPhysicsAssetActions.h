#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Physics asset structural authoring actions for Monolith (animation namespace).
 *
 * Complements the existing tuning actions (get_physics_asset_info, set_body_properties,
 * set_constraint_properties) with structure edits:
 *   - create_physics_asset        generate a physics asset from a skeletal mesh (FPhysicsAssetUtils::CreateFromSkeletalMesh)
 *   - add_physics_body            add a body for one bone, fitted to the bone's skinned vertices
 *   - remove_physics_body         remove a body and every constraint attached to it
 *   - add_physics_constraint      add a constraint between two existing bodies (child, parent)
 *   - remove_physics_constraint   remove a constraint by index, name, or body pair
 *   - set_body_collision          enable or disable collision between two bodies
 */
class MONOLITHANIMATION_API FMonolithAnimationPhysicsAssetActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleCreatePhysicsAsset(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddPhysicsBody(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemovePhysicsBody(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddPhysicsConstraint(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemovePhysicsConstraint(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetBodyCollision(const TSharedPtr<FJsonObject>& Params);
};
