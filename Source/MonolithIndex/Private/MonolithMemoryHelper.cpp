#include "MonolithMemoryHelper.h"
#include "HAL/PlatformMemory.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/GarbageCollection.h"
#include "Engine/Engine.h"
#include "Framework/Application/SlateApplication.h"
#include "MonolithSettings.h"

DEFINE_LOG_CATEGORY(LogMonolithMemory);

namespace
{
	double LastGCTime = 0.0;
	constexpr double MinGCIntervalSeconds = 0.5;
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
	SIZE_T CurrentUsageMB = GetCurrentMemoryUsageMB();
	return CurrentUsageMB > BudgetMB;
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
}

bool FMonolithMemoryHelper::TryUnloadPackage(UObject* Asset)
{
	if (!Asset)
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

void FMonolithMemoryHelper::YieldToEditor()
{
	if (!IsInGameThread())
	{
		return;
	}

	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().PumpMessages();
		FSlateApplication::Get().Tick();
	}
}

void FMonolithMemoryHelper::LogMemoryStats(const FString& Context)
{
	FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
	
	SIZE_T UsedMB = Stats.UsedPhysical / (1024 * 1024);
	SIZE_T AvailableMB = Stats.AvailablePhysical / (1024 * 1024);
	SIZE_T TotalMB = Stats.TotalPhysical / (1024 * 1024);
	SIZE_T UsedVirtualMB = Stats.UsedVirtual / (1024 * 1024);

	UE_LOG(LogMonolithMemory, Log, 
		TEXT("[%s] Memory: Used=%llu MB, Available=%llu MB, Total=%llu MB, Virtual=%llu MB"),
		*Context, UsedMB, AvailableMB, TotalMB, UsedVirtualMB);
}

bool FMonolithMemoryHelper::IsMemoryCritical()
{
	constexpr SIZE_T CriticalThresholdMB = 2048; // 2GB
	return GetAvailableMemoryMB() < CriticalThresholdMB;
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
	return static_cast<int32>(Stats.TotalPhysical / (1024ULL * 1024ULL * 1024ULL));
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
