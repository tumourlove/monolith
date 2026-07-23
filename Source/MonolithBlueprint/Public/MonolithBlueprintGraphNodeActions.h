#pragma once
#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Actions for authoring individual nodes INSIDE an asset's UEdGraph — addressed by
 * their full sub-object path (e.g. "/Game/.../CO.CO:CustomizableObjectGraph_0.NodeFoo_1").
 *
 * Motivating case: Mutable CustomizableObject graphs. The concrete node classes
 * (e.g. UCustomizableObjectNodeSkeletalMesh) live in a plugin's /Private/ headers
 * and cannot be #included from another module, so they can't be typed directly.
 * These actions stay engine-generic: they resolve the node by soft-object path,
 * set a UPROPERTY through FProperty reflection (the write path the Details panel
 * uses), then — crucially — call UEdGraphNode::ReconstructNode() so the node
 * rebuilds its pins/sections (assigning a mesh with no reconstruct leaves the node
 * half-configured and the graph fails to compile). Works for ANY UEdGraphNode in
 * ANY asset graph (Mutable, AnimGraph, Material, Niagara, Blueprint), not just CO.
 */
class FMonolithBlueprintGraphNodeActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult HandleSetGraphNodeProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetGraphNodeProperties(const TSharedPtr<FJsonObject>& Params);
};
