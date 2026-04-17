#include "MonolithMemoryHelper.h"
#include "HAL/PlatformMemory.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/GarbageCollection.h"
#include "Engine/Engine.h"
#include "Framework/Application/SlateApplication.h"

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

	// Mark the package as pending kill so it gets cleaned up on next GC
	Package->ClearFlags(RF_Standalone);
	Package->SetFlags(RF_Transient);

	// Clear any references we might have
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
