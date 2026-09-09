#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MonolithToolRegistry.h"

class ABlockingVolume;
class UMaterialInstanceDynamic;

/**
 * Phase 4: Blockout System (15 actions)
 * Tag-based blockout volumes, asset matching, atomic replacement,
 * layout export/import, volume scanning, and prop scattering.
 * The core value proposition of the mesh module.
 */
class FMonolithMeshBlockoutActions
{
public:
	/** Register all 15 blockout actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// --- Volume management ---
	static FMonolithActionResult GetBlockoutVolumes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetBlockoutVolumeInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetupBlockoutVolume(const TSharedPtr<FJsonObject>& Params);

	// --- Primitive creation ---
	static FMonolithActionResult CreateBlockoutPrimitive(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateBlockoutPrimitivesBatch(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateBlockoutGrid(const TSharedPtr<FJsonObject>& Params);

	// --- Asset matching ---
	static FMonolithActionResult MatchAssetToBlockout(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult MatchAllInVolume(const TSharedPtr<FJsonObject>& Params);

	// --- Replacement ---
	static FMonolithActionResult ApplyReplacement(const TSharedPtr<FJsonObject>& Params);

	// --- Tagging ---
	static FMonolithActionResult SetActorTags(const TSharedPtr<FJsonObject>& Params);

	// --- Cleanup ---
	static FMonolithActionResult ClearBlockout(const TSharedPtr<FJsonObject>& Params);

	// --- Layout export/import ---
	static FMonolithActionResult ExportBlockoutLayout(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ImportBlockoutLayout(const TSharedPtr<FJsonObject>& Params);

	// --- Spatial analysis ---
	static FMonolithActionResult ScanVolume(const TSharedPtr<FJsonObject>& Params);

	// --- Prop scattering ---
	static FMonolithActionResult ScatterProps(const TSharedPtr<FJsonObject>& Params);

	// --- Blueprint volume creation ---
	static FMonolithActionResult CreateBlockoutBlueprint(const TSharedPtr<FJsonObject>& Params);

	// --- Helpers ---

	/** Find a BlockingVolume by name. Returns nullptr and sets OutError on failure. */
	static ABlockingVolume* FindBlockingVolume(const FString& VolumeName, FString& OutError);

	/** Find a blockout volume by name — searches both ABlockingVolume (tag-based) and
	 *  BP_MonolithBlockoutVolume actors (Blueprint property-based). Returns AActor* since
	 *  callers only need GetActorBounds/Tags which live on AActor. */
	static AActor* FindBlockoutVolumeAny(const FString& VolumeName, FString& OutError);

	/** Get or create a transient blockout material instance for a category color */
	static UMaterialInstanceDynamic* GetBlockoutMaterial(const FString& Category);

	/** Check if an actor has a specific Monolith.* tag (case-insensitive prefix match) */
	static bool HasMonolithTag(const AActor* Actor, const FString& TagPrefix);

	/** Get the value portion of a Monolith tag (e.g. "Kitchen" from "Monolith.Room:Kitchen") */
	static FString GetMonolithTagValue(const AActor* Actor, const FString& TagPrefix);

	/** Build a JSON array from FVector */
	static TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V);

	/** Build a JSON array from FRotator */
	static TArray<TSharedPtr<FJsonValue>> RotatorToJsonArray(const FRotator& R);

	/** Get the basic shape mesh path for a shape name */
	static FString GetBasicShapePath(const FString& ShapeName, bool& bValid);

	/** Compute blockout actor bounds from scale * basic shape default size (100cm cube) */
	static FVector GetBlockoutActorSize(const AActor* Actor);

	/** Scoped undo transaction that respects FMonolithMeshSceneActions::bBatchTransactionActive */
	struct FScopedMeshTransaction
	{
		bool bOwnsTransaction;

		FScopedMeshTransaction(const FText& Description);
		~FScopedMeshTransaction();
		void Cancel();
	};

	/** Cached blockout material instances (transient, not saved to disk) */
	static TMap<FString, TWeakObjectPtr<UMaterialInstanceDynamic>> CachedBlockoutMaterials;
};
