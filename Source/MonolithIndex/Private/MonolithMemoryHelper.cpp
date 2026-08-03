#include "MonolithMemoryHelper.h"
#include "DynamicRHI.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMemory.h"
#include "Misc/App.h"
#include "RHICommandList.h"
#include "RHIStats.h"
#include "RenderingThread.h"
#include "Runtime/Launch/Resources/Version.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/GarbageCollection.h"
#include "Engine/Engine.h"
#include "MonolithSettings.h"

DEFINE_LOG_CATEGORY(LogMonolithMemory);

namespace
{
	double LastGCTime = 0.0;
	constexpr double MinGCIntervalSeconds = 0.5;
	constexpr uint64 BytesPerMB = 1024ULL * 1024ULL;

	void ForEachPackageObject(UPackage* Package, TFunctionRef<bool(UObject*)> Operation)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
		ForEachObjectWithPackage(Package, Operation, EGetObjectsFlags::IncludeNestedObjects);
#else
		ForEachObjectWithPackage(Package, Operation, true);
#endif
	}

	void FlushPendingRenderingResources()
	{
		if (!IsInGameThread() || !FApp::CanEverRender() || !GDynamicRHI)
		{
			return;
		}

		FlushRenderingCommands();
		ENQUEUE_RENDER_COMMAND(MonolithFlushPendingRHIResources)(
			[](FRHICommandListImmediate& RHICmdList)
			{
				RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
			});
		FlushRenderingCommands();
	}
}

SIZE_T FMonolithMemoryHelper::GetCurrentMemoryUsageMB()
{
	FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
	return Stats.UsedPhysical / (1024 * 1024);
}

SIZE_T FMonolithMemoryHelper::GetAvailableMemoryMB()
{
	FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
	return Stats.AvailablePhysical / (1024 * 1024);
}

bool FMonolithMemoryHelper::ShouldThrottle(SIZE_T BudgetMB)
{
	return ClassifyMemoryPressure(CaptureMemorySnapshot(false), BudgetMB) != EMonolithMemoryPressure::None;
}

FMonolithMemorySnapshot FMonolithMemoryHelper::CaptureMemorySnapshot(bool bIncludeGPUStats)
{
	const FPlatformMemoryStats PlatformStats = FPlatformMemory::GetStats();

	FMonolithMemorySnapshot Snapshot;
	Snapshot.ProcessUsedPhysicalMB = PlatformStats.UsedPhysical / BytesPerMB;
	Snapshot.AvailablePhysicalMB = PlatformStats.AvailablePhysical / BytesPerMB;
	Snapshot.TotalPhysicalMB = PlatformStats.TotalPhysical / BytesPerMB;

	if (!bIncludeGPUStats || !IsInGameThread() || !FApp::CanEverRender() || !GDynamicRHI)
	{
		return Snapshot;
	}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	FRHIMemoryStats RHIStats;
	RHIGetMemoryStats(RHIStats);
	if (RHIStats.BudgetLocal > 0)
	{
		Snapshot.GPUBudgetMB = RHIStats.BudgetLocal / BytesPerMB;
		Snapshot.GPUUsedMB = RHIStats.UsedLocal / BytesPerMB;
		Snapshot.bHasGPUStats = true;
	}
	else if (RHIStats.BudgetSystem > 0)
	{
		Snapshot.GPUBudgetMB = RHIStats.BudgetSystem / BytesPerMB;
		Snapshot.GPUUsedMB = RHIStats.UsedSystem / BytesPerMB;
		Snapshot.bHasGPUStats = true;
	}
#else
	FTextureMemoryStats TextureStats;
	RHIGetTextureMemoryStats(TextureStats);
	const int64 DeviceMemory = TextureStats.GetTotalDeviceWorkingMemory();
	if (DeviceMemory > 0)
	{
		Snapshot.GPUBudgetMB = static_cast<uint64>(DeviceMemory) / BytesPerMB;
		Snapshot.GPUUsedMB = (TextureStats.StreamingMemorySize + TextureStats.NonStreamingMemorySize) / BytesPerMB;
		Snapshot.bHasGPUStats = true;
	}
#endif

	return Snapshot;
}

EMonolithMemoryPressure FMonolithMemoryHelper::ClassifyMemoryPressure(const FMonolithMemorySnapshot& Snapshot, SIZE_T BudgetMB)
{
	const uint64 CriticalPhysicalHeadroomMB = FMath::Clamp<uint64>(Snapshot.TotalPhysicalMB / 16, 1024, 2048);
	const uint64 SoftPhysicalHeadroomMB = FMath::Clamp<uint64>(Snapshot.TotalPhysicalMB / 8, 2048, 4096);
	if (Snapshot.AvailablePhysicalMB < CriticalPhysicalHeadroomMB)
	{
		return EMonolithMemoryPressure::Critical;
	}

	if (Snapshot.bHasGPUStats && Snapshot.GPUBudgetMB > 0)
	{
		const uint64 AvailableGPUMB = Snapshot.GPUUsedMB < Snapshot.GPUBudgetMB
			? Snapshot.GPUBudgetMB - Snapshot.GPUUsedMB
			: 0;
		const uint64 CriticalGPUHeadroomMB = FMath::Max<uint64>(512, Snapshot.GPUBudgetMB / 10);
		if (AvailableGPUMB < CriticalGPUHeadroomMB)
		{
			return EMonolithMemoryPressure::Critical;
		}
	}

	if (Snapshot.ProcessUsedPhysicalMB > BudgetMB)
	{
		return EMonolithMemoryPressure::Soft;
	}
	if (Snapshot.AvailablePhysicalMB < SoftPhysicalHeadroomMB)
	{
		return EMonolithMemoryPressure::Soft;
	}

	if (Snapshot.bHasGPUStats && Snapshot.GPUBudgetMB > 0 &&
		Snapshot.GPUUsedMB * 100 >= Snapshot.GPUBudgetMB * 85)
	{
		return EMonolithMemoryPressure::Soft;
	}

	return EMonolithMemoryPressure::None;
}

void FMonolithMemoryHelper::ForceGarbageCollection(bool bFullPurge)
{
	if (!IsInGameThread())
	{
		UE_LOG(LogMonolithMemory, Verbose, TEXT("ForceGarbageCollection called from non-game thread - skipping"));
		return;
	}

	// Check if GC is already in progress to prevent reentrant GC crashes (GCScopeLock assertion)
	if (IsGarbageCollecting())
	{
		UE_LOG(LogMonolithMemory, Verbose, TEXT("GC already in progress - skipping to avoid reentrant GC crash"));
		return;
	}

	// Cooldown to prevent hammering GC which can cause timing-related reentrant issues
	const double CurrentTime = FPlatformTime::Seconds();
	const double TimeSinceLastGC = CurrentTime - LastGCTime;
	if (TimeSinceLastGC < MinGCIntervalSeconds)
	{
		UE_LOG(LogMonolithMemory, Verbose, TEXT("GC cooldown active (%.2fs since last GC) - skipping"), TimeSinceLastGC);
		return;
	}
	LastGCTime = CurrentTime;

	// Use TryCollectGarbage which respects GC locks and won't assert on reentry
	if (!TryCollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, bFullPurge))
	{
		UE_LOG(LogMonolithMemory, Verbose, TEXT("TryCollectGarbage returned false - GC deferred by engine"));
		return;
	}

	if (bFullPurge)
	{
		FlushPendingRenderingResources();
	}
}

bool FMonolithMemoryHelper::TryUnloadPackage(UObject* Asset, bool bWasAlreadyLoaded)
{
	if (!Asset)
	{
		return false;
	}

	// Residency guard (issue #81): if the asset/package was already resident BEFORE
	// this indexing pass loaded it, it is referenced elsewhere (e.g. an open editor
	// tab, or a package pinned by the startup map). Clearing RF_Standalone on such an
	// object does not unload it — it survives GC because of the external reference —
	// but leaves it permanently stripped, so File->Save later trips the engine's
	// data-loss guard (0x10000002). Only strip objects THIS pass brought in.
	if (bWasAlreadyLoaded)
	{
		return false;
	}

	UPackage* Package = Asset->GetOutermost();
	if (!Package || Package == GetTransientPackage())
	{
		return false;
	}

	// Clear RF_Standalone on the package and asset so they're GC-eligible.
	// Do NOT SetFlags(RF_Transient) on the package — RF_Transient doesn't
	// control GC, and it causes UObject::IsAsset() to return false on contained
	// assets (Obj.cpp:2733), which silently strips cross-package TObjectPtr refs
	// at save time ("target poisoning").
	Package->ClearFlags(RF_Standalone);
	Asset->ClearFlags(RF_Standalone);

	return true;
}

bool FMonolithMemoryHelper::ReleasePackagesLoadedForIndexing(const TArray<UPackage*>& Packages)
{
	if (!IsInGameThread() || IsGarbageCollecting())
	{
		UE_LOG(LogMonolithMemory, Warning, TEXT("Cannot release indexing packages while off the game thread or during GC"));
		return false;
	}

	TArray<TWeakObjectPtr<UObject>> ClearedStandaloneObjects;
	TSet<UPackage*> UniquePackages;
	for (UPackage* Package : Packages)
	{
		if (!Package || Package == GetTransientPackage() || Package->IsRooted() ||
			Package->HasAnyPackageFlags(PKG_ContainsScript | PKG_CompiledIn))
		{
			continue;
		}
		UniquePackages.Add(Package);
	}

	for (UPackage* Package : UniquePackages)
	{
		ForEachPackageObject(Package, [&ClearedStandaloneObjects](UObject* Object)
		{
			if (Object && !Object->IsRooted() && Object->HasAnyFlags(RF_Standalone))
			{
				Object->ClearFlags(RF_Standalone);
				ClearedStandaloneObjects.Add(Object);
			}
			return true;
		});

		if (Package->HasAnyFlags(RF_Standalone))
		{
			Package->ClearFlags(RF_Standalone);
			ClearedStandaloneObjects.Add(Package);
		}
	}

	LastGCTime = FPlatformTime::Seconds();
	const bool bCollected = TryCollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);

	// Anything still reachable belongs to another live owner. Restore the flag so
	// indexing can never strand a package and trigger the editor's save-data guard.
	for (const TWeakObjectPtr<UObject>& WeakObject : ClearedStandaloneObjects)
	{
		if (UObject* Object = WeakObject.Get())
		{
			Object->SetFlags(RF_Standalone);
		}
	}

	FlushPendingRenderingResources();
	if (!bCollected)
	{
		UE_LOG(LogMonolithMemory, Warning, TEXT("GC deferred while releasing %d indexing-owned packages"), UniquePackages.Num());
	}
	return bCollected;
}

void FMonolithMemoryHelper::YieldToEditor()
{
	if (!IsInGameThread())
	{
		return;
	}

	// Intentional no-op seam (was a nested Slate tick).
	//
	// This function previously called FSlateApplication::Get().PumpMessages() +
	// FSlateApplication::Get().Tick() to keep the editor responsive mid-batch.
	// That re-entered the engine GC / asset-compiler flow from inside a
	// worker-blocked game-thread lambda: on high-core-count machines the engine
	// would spin up parallel GC worker contexts that never got freed across the
	// nested re-entry, leaking one context per batch until the 64-context cap
	// asserted ("Exceeded max active GC worker contexts", GarbageCollection.cpp:2133)
	// during deep indexing of large (~17k-asset) projects.
	//
	// Yielding now happens naturally between batches: each batch payload is
	// dispatched via FMonolithCompilerSafeDispatch's FTSTicker, runs on one real
	// main-tick, and returns — releasing the worker thread's Event->Wait() and
	// letting the editor tick normally on the genuine main loop between batches.
	// The 12 call sites are kept (cheap game-thread guard) so this remains a safe
	// documented seam for future yield-point needs.
}

void FMonolithMemoryHelper::LogMemoryStats(const FString& Context)
{
	const FMonolithMemorySnapshot Snapshot = CaptureMemorySnapshot(true);
	UE_LOG(LogMonolithMemory, Log,
		TEXT("[%s] Memory: Process=%llu MB, Available=%llu MB, Total=%llu MB, GPU=%llu/%llu MB%s"),
		*Context,
		Snapshot.ProcessUsedPhysicalMB,
		Snapshot.AvailablePhysicalMB,
		Snapshot.TotalPhysicalMB,
		Snapshot.GPUUsedMB,
		Snapshot.GPUBudgetMB,
		Snapshot.bHasGPUStats ? TEXT("") : TEXT(" (GPU stats unavailable)"));
}

bool FMonolithMemoryHelper::IsMemoryCritical()
{
	return ClassifyMemoryPressure(CaptureMemorySnapshot(IsInGameThread()), GetResolvedMemoryBudgetMB()) == EMonolithMemoryPressure::Critical;
}

// ---- RAM tier auto-detect (v0.13.0) ----
//
// The v0.12.x PR-#17 defaults (24 GB budget, batch=8/4) were tuned for a 32+ GB
// workstation. On 16 GB dev machines (UE 5.7 minimum spec) that pushed the
// indexer straight back into OOM territory — reported in issue #16 by @MAYLYBY.
//
// These helpers auto-detect installed RAM and pick a conservative tier. Settings
// fields default to 0 (sentinel) and resolve through these functions; users can
// still override per-project via Project Settings > Monolith > Indexing > Performance.

namespace
{
	int32 ComputeAutoMemoryBudgetMB(int32 RamGB)
	{
		if (RamGB >= 64) return 32768;
		if (RamGB >= 32) return 16384;
		if (RamGB >= 16) return  6144;
		return 3072;
	}

	void ComputeAutoBatchSizes(int32 RamGB, int32& OutDeep, int32& OutPost)
	{
		if (RamGB >= 32)      { OutDeep = 8; OutPost = 4; }
		else if (RamGB >= 16) { OutDeep = 4; OutPost = 2; }
		else                  { OutDeep = 2; OutPost = 1; }
	}
}

int32 FMonolithMemoryHelper::GetInstalledRamGB()
{
	const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
	return RoundPhysicalBytesToRamGB(Stats.TotalPhysical);
}

int32 FMonolithMemoryHelper::RoundPhysicalBytesToRamGB(uint64 TotalPhysicalBytes)
{
	constexpr uint64 BytesPerGB = 1024ULL * 1024ULL * 1024ULL;
	return static_cast<int32>((TotalPhysicalBytes + BytesPerGB / 2) / BytesPerGB);
}

int32 FMonolithMemoryHelper::GetResolvedMemoryBudgetMB()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (Settings->MemoryBudgetMB > 0)
	{
		return Settings->MemoryBudgetMB;
	}
	return ComputeAutoMemoryBudgetMB(GetInstalledRamGB());
}

int32 FMonolithMemoryHelper::GetResolvedDeepIndexBatchSize()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (Settings->DeepIndexBatchSize > 0)
	{
		return Settings->DeepIndexBatchSize;
	}
	int32 Deep = 0, Post = 0;
	ComputeAutoBatchSizes(GetInstalledRamGB(), Deep, Post);
	return Deep;
}

int32 FMonolithMemoryHelper::GetResolvedPostPassBatchSize()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (Settings->PostPassBatchSize > 0)
	{
		return Settings->PostPassBatchSize;
	}
	int32 Deep = 0, Post = 0;
	ComputeAutoBatchSizes(GetInstalledRamGB(), Deep, Post);
	return Post;
}

void FMonolithMemoryHelper::LogTierStartupOnce()
{
	static bool bLogged = false;
	if (bLogged)
	{
		return;
	}
	bLogged = true;

	const int32 RamGB  = GetInstalledRamGB();
	const int32 Budget = GetResolvedMemoryBudgetMB();
	const int32 Deep   = GetResolvedDeepIndexBatchSize();
	const int32 Post   = GetResolvedPostPassBatchSize();

	UE_LOG(LogMonolithMemory, Log,
		TEXT("Indexer tier: %d GB installed -> %d MB budget, batch(deep=%d, post=%d). Override in Project Settings > Monolith > Indexing > Performance."),
		RamGB, Budget, Deep, Post);
}
