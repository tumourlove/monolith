#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

class FEvent;

/**
 * Schedules Work to run on the game thread only when the asset compiler is idle
 * (GetNumRemainingAssets() == 0). Uses FTSTicker (not AsyncTask(GT)) so the
 * delegate fires from the main game-thread tick loop AFTER the asset compiling
 * manager has finished its per-frame PostCompilation routing.
 *
 * Why this matters (GitHub issue #19, fix regressed by commit 168c087):
 *   FTextureCompilingManager::PostCompilation enters inside a
 *   TGuardValue<bool> PostCompilationGuard(bIsRoutingPostCompilation, true).
 *   During that guard, the engine pumps the task graph (via UpdateResource
 *   and OnObjectPropertyChanged broadcasts), which can drain queued
 *   AsyncTask(ENamedThreads::GameThread, ...) lambdas. If one of those
 *   lambdas then loads an asset that triggers FinishAllCompilation, Epic's
 *   internal guard at TextureCompiler.cpp:454 fatals with:
 *     "Calling FinishCompilation is not allowed during PostCompilation."
 *   FTSTicker delegates, by contrast, fire from the main editor tick — well
 *   outside that guard.
 *
 * Behaviour:
 *   - Every main-loop tick the helper queries
 *     FAssetCompilingManager::Get().GetNumRemainingAssets().
 *   - When that returns 0, the helper invokes Work() on the game thread and
 *     triggers CompletionEvent (if non-null), then unregisters.
 *   - If the compiler is still busy, the helper reschedules itself.
 *   - TimeoutSeconds (default 120s) is a last-resort safety net: after it
 *     elapses the helper runs Work() anyway with a UE_LOG warning. A rare
 *     stall is preferable to an indefinite hang if something upstream leaves
 *     the compiler permanently non-idle.
 *
 * Lifetime note:
 *   No module-level shutdown sentinel is needed. FTSTicker::GetCoreTicker()
 *   is owned by the engine and drained/torn down on exit; any in-flight
 *   delegates are dropped at that point. The captured Work lambda and
 *   FEvent* are owned by the caller's stack frame — the caller already
 *   Waits on the event before returning, so the capture outlives the tick.
 */
struct MONOLITHINDEX_API FMonolithCompilerSafeDispatch
{
    static void RunOnGameThreadWhenCompilerIdle(
        TUniqueFunction<void()> Work,
        FEvent* CompletionEvent = nullptr,
        float TimeoutSeconds = 120.0f);
};
