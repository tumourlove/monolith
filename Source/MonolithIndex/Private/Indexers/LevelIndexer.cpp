#include "Indexers/LevelIndexer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "MonolithMemoryHelper.h"
#include "MonolithSettings.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "EngineUtils.h"
#include "Components/ActorComponent.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "WorldPartition/WorldPartition.h"
#include "Subsystems/WorldSubsystem.h"

bool FLevelIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Find all World assets under indexed paths
	TArray<FAssetData> WorldAssets;
	FARFilter Filter;
	if (IndexedPaths.Num() > 0)
	{
		for (const FName& Path : IndexedPaths)
		{
			Filter.PackagePaths.Add(Path);
		}
	}
	else
	{
		Filter.PackagePaths.Add(FName(TEXT("/Game")));
	}
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
	Registry.GetAssets(Filter, WorldAssets);

	// Get settings for batching
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	const int32 BatchSize = FMath::Max(1, FMonolithMemoryHelper::GetResolvedPostPassBatchSize());
	const SIZE_T MemoryBudgetMB = static_cast<SIZE_T>(FMonolithMemoryHelper::GetResolvedMemoryBudgetMB());
	const bool bLogMemory = Settings->bLogMemoryStats;

	UE_LOG(LogMonolithIndex, Log, TEXT("LevelIndexer: Found %d World assets to index (batch size: %d)"),
		WorldAssets.Num(), BatchSize);

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("LevelIndexer start"));
	}

	int32 ActorsInserted = 0;
	int32 LevelsProcessed = 0;
	int32 BatchNumber = 0;

	for (int32 i = 0; i < WorldAssets.Num(); i += BatchSize)
	{
		// Compiler-idle gate is enforced by FMonolithCompilerSafeDispatch at the call site (see issue #19).

		// Memory budget check before each batch
		if (FMonolithMemoryHelper::ShouldThrottle(MemoryBudgetMB))
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("LevelIndexer: Memory budget exceeded, forcing GC..."));
			FMonolithMemoryHelper::ForceGarbageCollection(true);
			FMonolithMemoryHelper::YieldToEditor();

			if (bLogMemory)
			{
				FMonolithMemoryHelper::LogMemoryStats(TEXT("LevelIndexer after throttle GC"));
			}
		}

		int32 BatchEnd = FMath::Min(i + BatchSize, WorldAssets.Num());

		// Process batch
		for (int32 j = i; j < BatchEnd; ++j)
		{
			const FAssetData& WorldData = WorldAssets[j];

			int64 LevelAssetId = DB.GetAssetId(WorldData.PackageName.ToString());
			if (LevelAssetId < 0) continue;

			// Capture residency BEFORE LoadPackage (issue #81): a world already resident
			// (e.g. an open level) must keep RF_Standalone; only strip what this pass loads.
			const bool bWasLoaded = FindPackage(nullptr, *WorldData.PackageName.ToString()) != nullptr;

			// Load the package to access level data without initializing gameplay
			UPackage* Package = LoadPackage(nullptr, *WorldData.PackageName.ToString(), LOAD_NoWarn | LOAD_Quiet);
			if (!Package) continue;

			UWorld* World = FindObject<UWorld>(Package, *WorldData.AssetName.ToString());
			if (!World)
			{
				// Try the common naming convention
				World = FindObject<UWorld>(Package, TEXT("World"));
			}
			if (!World || !World->PersistentLevel)
			{
				// Mark package for unload even if we couldn't find the world
				FMonolithMemoryHelper::TryUnloadPackage(Package, bWasLoaded);
				continue;
			}

			// Only index the persistent level - skip streaming sub-levels for performance
			ULevel* Level = World->PersistentLevel;
			ActorsInserted += IndexActorsInLevel(Level, DB, LevelAssetId);

			// Detect whether this world has a landscape subsystem. LoadPackage never calls UWorld::InitWorld, but a
			// landscape actor's PostRegisterAllComponents lazily creates + Initialize()s a ULandscapeSubsystem on
			// this never-InitWorld'd world. If we then tear the world down (TryUnloadPackage + GC), the GC destroys
			// the world while ULandscapeSubsystem is still bInitialized -> handled-but-noisy ensure at
			// WorldSubsystem.cpp:158 (issue #67).
			//
			// The shipped fix kept landscape worlds resident (RF_Standalone intact) to dodge both the ensure and a
			// fatal crash in ULandscapeSubsystem::Deinitialize (its grass-builder destructor dereferences the null
			// World->Scene on a no-render-scene Inactive world). That avoided ~80 resident UWorlds per reindex.
			//
			// Issue #67 optimization (this branch): tear the landscape world down safely instead of leaving it
			// resident, by UNREGISTERING every landscape proxy's components FIRST, then driving CleanupWorld:
			//   1. UnregisterAllComponents() on each ALandscapeProxy cascades to ULandscapeComponent::OnUnregister ->
			//      ULandscapeSubsystem::UnregisterComponent (Landscape.cpp:2360) -> FLandscapeGrassMapsBuilder::
			//      UnregisterComponent (LandscapeGrassMapsBuilder.cpp:806-820), which NULLs each FComponentState
			//      (State->Component = nullptr) and touches NO render scene.
			//   2. World->CleanupWorld() then Deinitialize()s the WHOLE world subsystem collection. ULandscapeSubsystem::
			//      Deinitialize deletes the grass builder; its destructor's evict loop now early-exits on every
			//      Component==nullptr state (the bCancelAndEvictAllImmediately/Component==nullptr branch) and never
			//      reaches CanCurrentlyRender()/World->Scene -> no crash. Super::Deinitialize clears bInitialized ->
			//      no GC-time ensure. CleanupWorld also clears RF_Standalone (a superset of TryUnloadPackage) and
			//      drives WorldPartition uninit, so the landscape branch needs NEITHER the separate WP-uninit NOR
			//      TryUnloadPackage that the non-landscape branch below still uses.
			// This is the key reversal vs. the shipped fix: CleanupWorld was previously FATAL precisely because it ran
			// Deinitialize while grass states still held live components; unregister-first makes it safe.
			//
			// RESIDUAL RISK (accepted, covered by the runtime acceptance gate + rollback, NOT pre-guarded here per
			// issue #67 plan section 6): CleanupWorld drives Deinitialize on EVERY world subsystem, not just landscape.
			// We cannot call ULandscapeSubsystem::Deinitialize directly (it is private), so full CleanupWorld is the
			// only lever. Another auto-initialized world subsystem could, in its own Deinitialize, deref the null
			// World->Scene on this no-render-scene world and crash. If the acceptance gate crashes, roll back to the
			// shipped resident-skip behavior (do not regress the 0-ensure / 0-crash guarantee for the memory win).
			//
			// We detect the ULandscapeSubsystem itself rather than scanning for an ALandscapeProxy actor, because the
			// subsystem is the thing that ensures at GC, and in World Partition / streaming worlds the landscape actor
			// can live in a streaming sublevel or as an external WP actor that is NOT present in PersistentLevel->Actors
			// (issue #67 refinement: LVL_NiagaraDestructionDriver_Demo_Cube still tripped the ensure with a persistent-
			// level-only actor scan because its landscape lives outside the persistent level, but its ULandscapeSubsystem
			// was initialized). For the same reason the unregister pass below uses a world-wide TActorIterator (which
			// covers PersistentLevel + all streaming sublevels) rather than scanning PersistentLevel->Actors only.
			// Resolve both the subsystem class and the proxy class by script path so we avoid a Landscape Build.cs
			// dependency (which would force a full rebuild + hard link); if a class can't be resolved (Landscape module
			// not loaded) there can be no landscape subsystem, so we safely fall back to the original teardown.
			// GetSubsystemBase(TSubclassOf<UWorldSubsystem>) performs the lookup against the world's subsystem
			// collection without a compile-time type dependency and returns non-null iff the subsystem instance exists.
			bool bContainsLandscape = false;
			{
				static UClass* LandscapeSubsystemClass = FindObject<UClass>(nullptr, TEXT("/Script/Landscape.LandscapeSubsystem"));
				if (LandscapeSubsystemClass)
				{
					if (World->GetSubsystemBase(LandscapeSubsystemClass) != nullptr)
					{
						bContainsLandscape = true;
					}
				}
			}

			// Skip teardown if this is the world currently open in the editor - uninit would stop viewport WP cell streaming and unload would close the level
			UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (World != EditorWorld)
			{
				if (bContainsLandscape)
				{
					// Issue #81 residency guard: only tear a landscape world down if THIS pass freshly loaded
					// it. If it was already resident (referenced elsewhere), CleanupWorld would Deinitialize a
					// LIVE world's subsystems AND clear RF_Standalone (a superset of the TryUnloadPackage strip) --
					// stranding an externally-referenced world so File->Save trips the data-loss guard 0x10000002.
					// The actively-open editor world is already excluded by the World != EditorWorld guard above;
					// this additionally covers a world resident via any other outstanding reference. Mirror
					// TryUnloadPackage's early-return: a world we didn't load is left intact (indexing already ran).
					if (!bWasLoaded)
					{
						// Unregister every landscape proxy's components BEFORE teardown so the grass-builder destructor
						// (driven by CleanupWorld below) never dereferences the null World->Scene. See the block comment
						// above for the full mechanism. World-wide iteration (persistent level + all streaming sublevels)
						// is required because landscape proxies can live outside PersistentLevel->Actors in WP/streaming
						// worlds. The proxy class is resolved by script path to avoid a Landscape Build.cs dependency.
						int32 UnregisteredProxies = 0;
						static UClass* LandscapeProxyClass = FindObject<UClass>(nullptr, TEXT("/Script/Landscape.LandscapeProxy"));
						if (LandscapeProxyClass)
						{
							for (TActorIterator<AActor> It(World); It; ++It)
							{
								AActor* Actor = *It;
								if (Actor && Actor->IsA(LandscapeProxyClass))
								{
									Actor->UnregisterAllComponents();
									++UnregisteredProxies;
								}
							}
						}

						// Now safe: the grass FComponentStates are all NULLed, so ULandscapeSubsystem::Deinitialize's
						// destructor early-exits without touching the render scene, and Super::Deinitialize clears
						// bInitialized (no GC-time ensure). CleanupWorld also drives WorldPartition uninit and clears
						// RF_Standalone (superset of TryUnloadPackage), so no separate WP-uninit / TryUnloadPackage is
						// needed for landscape worlds.
						World->CleanupWorld();

						UE_LOG(LogMonolithIndex, Verbose,
							TEXT("LevelIndexer: '%s' has a landscape subsystem - unregistered %d landscape proxies and cleaned up the world (issue #67 optimization)."),
							*WorldData.PackageName.ToString(), UnregisteredProxies);
					}
				}
				else
				{
					// Uninitialize WorldPartition before unload - LoadPackage skips the editor teardown path, so GC would otherwise assert in UWorldPartitionSubsystem::Deinitialize
					if (UWorldPartition* WP = World->GetWorldPartition())
					{
						if (WP->IsInitialized())
						{
							WP->Uninitialize();
						}
					}

					// Mark world/package for unloading after indexing
					FMonolithMemoryHelper::TryUnloadPackage(World, bWasLoaded);
				}
			}

			LevelsProcessed++;
		}

		BatchNumber++;

		// GC after each batch to prevent memory accumulation
		FMonolithMemoryHelper::ForceGarbageCollection(false);
		FMonolithMemoryHelper::YieldToEditor();

		// Log progress periodically
		if (BatchNumber % 5 == 0 || BatchEnd == WorldAssets.Num())
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("LevelIndexer: processed %d / %d levels"),
				LevelsProcessed, WorldAssets.Num());

			if (bLogMemory)
			{
				FMonolithMemoryHelper::LogMemoryStats(FString::Printf(TEXT("LevelIndexer batch %d"), BatchNumber));
			}
		}
	}

	// Final GC
	FMonolithMemoryHelper::ForceGarbageCollection(true);

	UE_LOG(LogMonolithIndex, Log, TEXT("LevelIndexer: indexed %d levels, %d actors total"),
		LevelsProcessed, ActorsInserted);

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("LevelIndexer complete"));
	}

	return true;
}

int32 FLevelIndexer::IndexActorsInLevel(ULevel* Level, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!Level) return 0;

	int32 Count = 0;
	for (AActor* Actor : Level->Actors)
	{
		if (!Actor) continue;

		// Skip the world settings and default brush - they're internal
		if (Actor->IsA(AWorldSettings::StaticClass())) continue;

		FIndexedActor IndexedActor;
		IndexedActor.AssetId = AssetId;
		IndexedActor.ActorName = Actor->GetName();
		IndexedActor.ActorClass = Actor->GetClass()->GetName();
		IndexedActor.ActorLabel = Actor->GetActorLabel();
		IndexedActor.Transform = SerializeTransform(Actor->GetActorTransform());
		IndexedActor.Components = SerializeComponents(Actor);

		DB.InsertActor(IndexedActor);
		Count++;
	}
	return Count;
}

FString FLevelIndexer::SerializeTransform(const FTransform& Transform)
{
	auto Obj = MakeShared<FJsonObject>();

	const FVector& Loc = Transform.GetLocation();
	auto LocObj = MakeShared<FJsonObject>();
	LocObj->SetNumberField(TEXT("x"), Loc.X);
	LocObj->SetNumberField(TEXT("y"), Loc.Y);
	LocObj->SetNumberField(TEXT("z"), Loc.Z);
	Obj->SetObjectField(TEXT("location"), LocObj);

	const FRotator Rot = Transform.GetRotation().Rotator();
	auto RotObj = MakeShared<FJsonObject>();
	RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
	RotObj->SetNumberField(TEXT("yaw"), Rot.Yaw);
	RotObj->SetNumberField(TEXT("roll"), Rot.Roll);
	Obj->SetObjectField(TEXT("rotation"), RotObj);

	const FVector& Scale = Transform.GetScale3D();
	auto ScaleObj = MakeShared<FJsonObject>();
	ScaleObj->SetNumberField(TEXT("x"), Scale.X);
	ScaleObj->SetNumberField(TEXT("y"), Scale.Y);
	ScaleObj->SetNumberField(TEXT("z"), Scale.Z);
	Obj->SetObjectField(TEXT("scale"), ScaleObj);

	FString Result;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
	FJsonSerializer::Serialize(Obj, *Writer, true);
	return Result;
}

FString FLevelIndexer::SerializeComponents(const AActor* Actor)
{
	TArray<TSharedPtr<FJsonValue>> CompArray;

	TInlineComponentArray<UActorComponent*> Components;
	Actor->GetComponents(Components);

	for (const UActorComponent* Comp : Components)
	{
		if (!Comp) continue;

		auto CompObj = MakeShared<FJsonObject>();
		CompObj->SetStringField(TEXT("name"), Comp->GetName());
		CompObj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());

		CompArray.Add(MakeShared<FJsonValueObject>(CompObj));
	}

	FString Result;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
	FJsonSerializer::Serialize(CompArray, *Writer);
	return Result;
}
