#include "MonolithCompilerSafeDispatch.h"
#include "MonolithIndexLog.h"

#include "AssetCompilingManager.h"
#include "Containers/Ticker.h"
#include "HAL/Event.h"
#include "HAL/PlatformTime.h"

bool FMonolithDispatchAbortToken::AbortIfUnclaimed()
{
    int32 Expected = StateUnclaimed;
    return State.CompareExchange(Expected, StateAborted);
}

bool FMonolithDispatchAbortToken::ClaimForExecution()
{
    int32 Expected = StateUnclaimed;
    return State.CompareExchange(Expected, StateClaimed);
}

bool FMonolithDispatchAbortToken::IsAborted() const
{
    return State.Load() == StateAborted;
}

void FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
    TUniqueFunction<void()> Work,
    FEvent* CompletionEvent,
    float TimeoutSeconds,
    TSharedPtr<FMonolithDispatchAbortToken>* OutAbortToken)
{
    // Start time captured by value so each tick can compute elapsed.
    const double StartTime = FPlatformTime::Seconds();

    // Shared state between the ticker lambda and itself across frames.
    // TUniqueFunction isn't copyable, so wrap in a TSharedPtr the delegate can share.
    struct FDispatchState
    {
        TUniqueFunction<void()> Work;
        FEvent* CompletionEvent = nullptr;
        double StartTime = 0.0;
        float TimeoutSeconds = 120.0f;
        TSharedPtr<FMonolithDispatchAbortToken> AbortToken;
    };

    TSharedPtr<FDispatchState> State = MakeShared<FDispatchState>();
    State->Work = MoveTemp(Work);
    State->CompletionEvent = CompletionEvent;
    State->StartTime = StartTime;
    State->TimeoutSeconds = TimeoutSeconds;
    State->AbortToken = MakeShared<FMonolithDispatchAbortToken>();
    if (OutAbortToken)
    {
        *OutAbortToken = State->AbortToken;
    }

    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([State](float /*DeltaTime*/) -> bool
        {
            // The caller abandoned its wait (editor exit) and has unwound. Its
            // payload captures reference that retired stack, so the payload must
            // never run. Trigger anyway — the abandoning caller deliberately
            // leaked the event, so this is a no-op rather than a dangling write.
            if (State->AbortToken->IsAborted())
            {
                if (State->CompletionEvent)
                {
                    State->CompletionEvent->Trigger();
                }
                return false;
            }

            const int32 RemainingAssets = FAssetCompilingManager::Get().GetNumRemainingAssets();
            const double Elapsed = FPlatformTime::Seconds() - State->StartTime;
            const bool bTimedOut = Elapsed >= static_cast<double>(State->TimeoutSeconds);

            if (RemainingAssets == 0 || bTimedOut)
            {
                if (bTimedOut && RemainingAssets != 0)
                {
                    UE_LOG(LogMonolithIndex, Warning,
                        TEXT("FMonolithCompilerSafeDispatch timed out after %.1fs with %d assets still compiling — running work anyway."),
                        Elapsed, RemainingAssets);
                }

                // Invoke payload. We are on the main game-thread tick here,
                // outside FTextureCompilingManager::PostCompilation's guard.
                // Claim it first: that closes the window where the caller aborts
                // between the check above and the call below. Losing the claim
                // means the caller withdrew the payload — skip it and just
                // release the (leaked) event.
                if (State->Work && State->AbortToken->ClaimForExecution())
                {
                    State->Work();
                }

                if (State->CompletionEvent)
                {
                    State->CompletionEvent->Trigger();
                }

                // Return false to unregister this ticker.
                return false;
            }

            // Still compiling, still under timeout — reschedule next tick.
            return true;
        }),
        0.0f);
}
