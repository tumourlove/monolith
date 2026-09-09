#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Templates/Atomic.h"

class FEvent;

/**
 * Handle that lets a caller who can no longer wait for a dispatch withdraw its
 * payload before the game thread runs it.
 *
 * Why it exists: RunOnGameThreadWhenCompilerIdle's contract is "the caller waits
 * on the completion event, so the payload's captures outlive the tick". At editor
 * exit that contract inverts into a deadlock — the game thread is parked in
 * UMonolithIndexSubsystem::Deinitialize joining the indexing worker, so no tick
 * can ever run, while the worker blocks on an event only a tick can trigger. The
 * worker must abandon its wait and unwind, which retires the very stack the
 * payload's captures point at.
 *
 * The payload is therefore handed to exactly one of the two racers: the ticker
 * claims it before invoking it, the caller aborts it before unwinding, and the
 * loser backs off. Both transitions are a single compare-exchange from Unclaimed.
 */
class MONOLITHINDEX_API FMonolithDispatchAbortToken
{
public:
    /**
     * Withdraw the payload so it is never invoked.
     * @return true  the payload was still unclaimed and will never run — the
     *               caller may unwind immediately.
     *         false the game thread claimed it first and is executing it right
     *               now. The caller MUST wait for the completion event instead
     *               of unwinding: that same tick triggers the event when the
     *               payload returns, so the wait is bounded and cannot deadlock
     *               (a claiming tick proves the game thread is still running).
     */
    bool AbortIfUnclaimed();

    /**
     * Dispatcher-internal, called from the ticker immediately before invoking
     * the payload. Takes ownership of it for execution.
     * @return false the caller aborted first — do not invoke the payload.
     */
    bool ClaimForExecution();

    /** Dispatcher-internal: has the caller withdrawn the payload? */
    bool IsAborted() const;

private:
    static constexpr int32 StateUnclaimed = 0;
    static constexpr int32 StateClaimed = 1;
    static constexpr int32 StateAborted = 2;

    TAtomic<int32> State{StateUnclaimed};
};

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
 *
 *   The one caller that cannot honour that "waits before returning" half of the
 *   contract is the indexing worker at editor exit; it passes OutAbortToken and
 *   follows the protocol on FMonolithDispatchAbortToken.
 */
struct MONOLITHINDEX_API FMonolithCompilerSafeDispatch
{
    /**
     * @param OutAbortToken  optional; receives the handle a caller needs to
     *        withdraw Work if it is ever forced to abandon its wait. Callers
     *        that always wait to completion can ignore it.
     */
    static void RunOnGameThreadWhenCompilerIdle(
        TUniqueFunction<void()> Work,
        FEvent* CompletionEvent = nullptr,
        float TimeoutSeconds = 120.0f,
        TSharedPtr<FMonolithDispatchAbortToken>* OutAbortToken = nullptr);
};
