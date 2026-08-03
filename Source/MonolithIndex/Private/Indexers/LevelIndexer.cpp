#include "Indexers/LevelIndexer.h"
#include "AssetCompilingManager.h"
#include "MonolithMemoryHelper.h"
#include "MonolithSettings.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/Set.h"
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
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "WorldPartition/WorldPartition.h"
#include "Subsystems/WorldSubsystem.h"
#include "Misc/ScopeLock.h"
#include "Runtime/Launch/Resources/Version.h"

namespace
{
	const TCHAR* LevelIndexStateMetaKey = TEXT("level_index_state");

	void ForEachPackageObject(UPackage* Package, TFunctionRef<bool(UObject*)> Operation)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
		ForEachObjectWithPackage(Package, Operation, EGetObjectsFlags::IncludeNestedObjects);
#else
		ForEachObjectWithPackage(Package, Operation, true);
#endif
	}

	class FScopedPackageLoadCapture
	{
	public:
		FScopedPackageLoadCapture()
		{
			Handle = FCoreUObjectDelegates::PackageCreatedForLoad.AddRaw(this, &FScopedPackageLoadCapture::OnPackageCreated);
		}

		~FScopedPackageLoadCapture()
		{
			Stop();
		}

		TArray<UPackage*> Stop()
		{
			if (Handle.IsValid())
			{
				FCoreUObjectDelegates::PackageCreatedForLoad.Remove(Handle);
				Handle.Reset();
			}

			FScopeLock Lock(&Mutex);
			TArray<UPackage*> Result;
			for (const TWeakObjectPtr<UPackage>& WeakPackage : CapturedPackages)
			{
				if (UPackage* Package = WeakPackage.Get())
				{
					Result.AddUnique(Package);
				}
			}
			return Result;
		}

	private:
		void OnPackageCreated(UPackage* Package)
		{
			if (Package && Package != GetTransientPackage())
			{
				FScopeLock Lock(&Mutex);
				CapturedPackages.Add(Package);
			}
		}

		FCriticalSection Mutex;
		FDelegateHandle Handle;
		TArray<TWeakObjectPtr<UPackage>> CapturedPackages;
	};

	void GatherPackageObjects(const TArray<UPackage*>& Packages, TArray<UObject*>& OutObjects)
	{
		for (UPackage* Package : Packages)
		{
			if (!Package)
			{
				continue;
			}
			ForEachPackageObject(Package, [&OutObjects](UObject* Object)
			{
				if (Object)
				{
					OutObjects.AddUnique(Object);
				}
				return true;
			});
		}
	}

	void PrepareWorldForIndexingUnload(UWorld* World)
	{
		if (!World || (GEditor && World == GEditor->GetEditorWorldContext().World()))
		{
			return;
		}

		static UClass* LandscapeSubsystemClass = FindObject<UClass>(nullptr, TEXT("/Script/Landscape.LandscapeSubsystem"));
		const bool bContainsLandscape = LandscapeSubsystemClass && World->GetSubsystemBase(LandscapeSubsystemClass) != nullptr;
		if (bContainsLandscape)
		{
			static UClass* LandscapeProxyClass = FindObject<UClass>(nullptr, TEXT("/Script/Landscape.LandscapeProxy"));
			if (LandscapeProxyClass)
			{
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (AActor* Actor = *It; Actor && Actor->IsA(LandscapeProxyClass))
					{
						Actor->UnregisterAllComponents();
					}
				}
			}
			World->CleanupWorld();
			return;
		}

		if (UWorldPartition* WorldPartition = World->GetWorldPartition())
		{
			if (WorldPartition->IsInitialized())
			{
				WorldPartition->Uninitialize();
			}
		}
	}

	const TCHAR* PressureName(EMonolithMemoryPressure Pressure)
	{
		switch (Pressure)
		{
		case EMonolithMemoryPressure::Soft: return TEXT("soft");
		case EMonolithMemoryPressure::Critical: return TEXT("critical");
		default: return TEXT("none");
		}
	}
}

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

	if (!DB.DeleteMeta(LevelIndexStateMetaKey))
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("LevelIndexer: failed to clear the previous level-pass state"));
		return false;
	}

	// A world can pull an unbounded graph of sublevels and render resources. It is
	// deliberately processed one load at a time; the generic post-pass batch size
	// remains applicable to bounded asset indexers such as mesh and animation.
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	const SIZE_T MemoryBudgetMB = static_cast<SIZE_T>(FMonolithMemoryHelper::GetResolvedMemoryBudgetMB());
	const bool bLogMemory = Settings->bLogMemoryStats;

	TMap<FName, int64> CandidateAssetIds;
	for (const FAssetData& WorldData : WorldAssets)
	{
		const int64 LevelAssetId = DB.GetAssetId(WorldData.PackageName.ToString());
		if (LevelAssetId >= 0)
		{
			CandidateAssetIds.Add(WorldData.PackageName, LevelAssetId);
		}
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("LevelIndexer: Found %d World assets to index (serialized loads, including linked sublevels)"),
		CandidateAssetIds.Num());

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("LevelIndexer start"));
	}

	int32 ActorsInserted = 0;
	int32 LevelsProcessed = 0;
	int32 LevelsFailed = 0;
	TSet<FName> ProcessedPackages;
	FString DegradedReason;

	for (const FAssetData& WorldData : WorldAssets)
	{
		if (!CandidateAssetIds.Contains(WorldData.PackageName))
		{
			continue;
		}
		if (ProcessedPackages.Contains(WorldData.PackageName))
		{
			continue;
		}

		EMonolithMemoryPressure Pressure = FMonolithMemoryHelper::ClassifyMemoryPressure(
			FMonolithMemoryHelper::CaptureMemorySnapshot(true), MemoryBudgetMB);
		if (Pressure != EMonolithMemoryPressure::None)
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("LevelIndexer: %s memory pressure before '%s'; draining resources"),
				PressureName(Pressure), *WorldData.PackageName.ToString());
			FMonolithMemoryHelper::ForceGarbageCollection(true);
			Pressure = FMonolithMemoryHelper::ClassifyMemoryPressure(
				FMonolithMemoryHelper::CaptureMemorySnapshot(true), MemoryBudgetMB);
			if (Pressure == EMonolithMemoryPressure::Critical)
			{
				const FMonolithMemorySnapshot Snapshot = FMonolithMemoryHelper::CaptureMemorySnapshot(true);
				DegradedReason = FString::Printf(
					TEXT("critical memory pressure before %s (process=%llu MB, available RAM=%llu MB, GPU=%llu/%llu MB%s)"),
					*WorldData.PackageName.ToString(), Snapshot.ProcessUsedPhysicalMB, Snapshot.AvailablePhysicalMB,
					Snapshot.GPUUsedMB, Snapshot.GPUBudgetMB, Snapshot.bHasGPUStats ? TEXT("") : TEXT(", GPU stats unavailable"));
				break;
			}
		}

		FlushAsyncLoading();
		const bool bWasAlreadyLoaded = FindPackage(nullptr, *WorldData.PackageName.ToString()) != nullptr;
		FScopedPackageLoadCapture LoadCapture;
		UPackage* Package = LoadPackage(nullptr, *WorldData.PackageName.ToString(), LOAD_NoWarn | LOAD_Quiet | LOAD_EditorOnly);
		TArray<UPackage*> NewlyLoadedPackages = LoadCapture.Stop();
		if (Package && !bWasAlreadyLoaded)
		{
			NewlyLoadedPackages.AddUnique(Package);
		}

		if (!Package)
		{
			++LevelsFailed;
			ProcessedPackages.Add(WorldData.PackageName);
			TArray<UObject*> PartiallyLoadedObjects;
			GatherPackageObjects(NewlyLoadedPackages, PartiallyLoadedObjects);
			if (PartiallyLoadedObjects.Num() > 0)
			{
				FAssetCompilingManager::Get().FinishCompilationForObjects(PartiallyLoadedObjects);
			}
			for (UObject* Object : PartiallyLoadedObjects)
			{
				if (UWorld* PartialWorld = Cast<UWorld>(Object))
				{
					PrepareWorldForIndexingUnload(PartialWorld);
				}
			}
			if (NewlyLoadedPackages.Num() > 0 && !FMonolithMemoryHelper::ReleasePackagesLoadedForIndexing(NewlyLoadedPackages))
			{
				DegradedReason = FString::Printf(TEXT("Unreal deferred cleanup after the failed load of %s"), *WorldData.PackageName.ToString());
				break;
			}
			continue;
		}

		TArray<UWorld*> LoadedWorlds;
		ForEachPackageObject(Package, [&LoadedWorlds](UObject* Object)
		{
			if (UWorld* World = Cast<UWorld>(Object))
			{
				LoadedWorlds.AddUnique(World);
			}
			return true;
		});
		for (UPackage* LoadedPackage : NewlyLoadedPackages)
		{
			ForEachPackageObject(LoadedPackage, [&LoadedWorlds](UObject* Object)
			{
				if (UWorld* World = Cast<UWorld>(Object))
				{
					LoadedWorlds.AddUnique(World);
				}
				return true;
			});
		}

		for (UWorld* World : LoadedWorlds)
		{
			if (!World)
			{
				continue;
			}

			const FName PackageName(*World->GetOutermost()->GetName());
			const int64* LevelAssetId = CandidateAssetIds.Find(PackageName);
			if (!LevelAssetId || ProcessedPackages.Contains(PackageName))
			{
				continue;
			}

			ProcessedPackages.Add(PackageName);
			if (!World->PersistentLevel)
			{
				++LevelsFailed;
				continue;
			}
			if (!DB.ClearActorsForAsset(*LevelAssetId))
			{
				UE_LOG(LogMonolithIndex, Error, TEXT("LevelIndexer: failed to replace actor rows for '%s'"), *PackageName.ToString());
				return false;
			}

			ActorsInserted += IndexActorsInLevel(World->PersistentLevel, DB, *LevelAssetId);
			++LevelsProcessed;
		}
		if (!ProcessedPackages.Contains(WorldData.PackageName))
		{
			ProcessedPackages.Add(WorldData.PackageName);
			++LevelsFailed;
		}

		// Loads can queue mesh/texture compilation that retains packages and RHI
		// resources. Finish only work owned by this load before tearing its worlds down.
		TArray<UObject*> ObjectsLoadedForIndexing;
		GatherPackageObjects(NewlyLoadedPackages, ObjectsLoadedForIndexing);
		if (ObjectsLoadedForIndexing.Num() > 0)
		{
			FAssetCompilingManager::Get().FinishCompilationForObjects(ObjectsLoadedForIndexing);
		}

		TSet<UPackage*> NewlyLoadedSet;
		NewlyLoadedSet.Append(NewlyLoadedPackages);
		for (UWorld* World : LoadedWorlds)
		{
			if (World && NewlyLoadedSet.Contains(World->GetOutermost()))
			{
				PrepareWorldForIndexingUnload(World);
			}
		}

		if (NewlyLoadedPackages.Num() > 0 && !FMonolithMemoryHelper::ReleasePackagesLoadedForIndexing(NewlyLoadedPackages))
		{
			DegradedReason = FString::Printf(TEXT("Unreal deferred garbage collection after %s"), *WorldData.PackageName.ToString());
			break;
		}

		Pressure = FMonolithMemoryHelper::ClassifyMemoryPressure(
			FMonolithMemoryHelper::CaptureMemorySnapshot(true), MemoryBudgetMB);
		if (Pressure == EMonolithMemoryPressure::Critical)
		{
			const FMonolithMemorySnapshot Snapshot = FMonolithMemoryHelper::CaptureMemorySnapshot(true);
			DegradedReason = FString::Printf(
				TEXT("critical memory pressure after %s (process=%llu MB, available RAM=%llu MB, GPU=%llu/%llu MB%s)"),
				*WorldData.PackageName.ToString(), Snapshot.ProcessUsedPhysicalMB, Snapshot.AvailablePhysicalMB,
				Snapshot.GPUUsedMB, Snapshot.GPUBudgetMB, Snapshot.bHasGPUStats ? TEXT("") : TEXT(", GPU stats unavailable"));
			break;
		}

		if (LevelsProcessed % 10 == 0 || ProcessedPackages.Num() == CandidateAssetIds.Num())
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("LevelIndexer: processed %d / %d levels"),
				ProcessedPackages.Num(), CandidateAssetIds.Num());
			if (bLogMemory)
			{
				FMonolithMemoryHelper::LogMemoryStats(FString::Printf(TEXT("LevelIndexer level %d"), ProcessedPackages.Num()));
			}
		}
	}

	if (!DegradedReason.IsEmpty())
	{
		const FString State = FString::Printf(TEXT("degraded: processed %d/%d levels; %s"),
			ProcessedPackages.Num(), CandidateAssetIds.Num(), *DegradedReason);
		if (!DB.WriteMeta(LevelIndexStateMetaKey, State))
		{
			return false;
		}
		UE_LOG(LogMonolithIndex, Error, TEXT("LevelIndexer: %s. Remaining levels were skipped to protect the editor; see project get_stats 'level_index_state'."), *State);
	}
	else if (LevelsFailed > 0)
	{
		const FString State = FString::Printf(TEXT("partial: %d level package(s) could not be loaded"), LevelsFailed);
		if (!DB.WriteMeta(LevelIndexStateMetaKey, State))
		{
			return false;
		}
		UE_LOG(LogMonolithIndex, Warning, TEXT("LevelIndexer: %s"), *State);
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("LevelIndexer: indexed %d levels, %d actors total, %d load failures"),
		LevelsProcessed, ActorsInserted, LevelsFailed);

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
