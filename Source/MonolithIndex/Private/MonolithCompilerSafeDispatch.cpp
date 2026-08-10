#include "MonolithCompilerSafeDispatch.h"
#include "MonolithIndexLog.h"

#include "AssetCompilingManager.h"
#include "Containers/Ticker.h"
#include "HAL/Event.h"
#include "HAL/PlatformTime.h"

void FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
    TUniqueFunction<void()> Work,
    FEvent* CompletionEvent,
    float TimeoutSeconds,
    TSharedPtr<TAtomic<bool>>* OutAbortToken)
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
        TSharedPtr<TAtomic<bool>> bAborted;
    };

    TSharedPtr<FDispatchState> State = MakeShared<FDispatchState>();
    State->Work = MoveTemp(Work);
    State->CompletionEvent = CompletionEvent;
    State->StartTime = StartTime;
    State->TimeoutSeconds = TimeoutSeconds;
    State->bAborted = MakeShared<TAtomic<bool>>(false);
    if (OutAbortToken)
    {
        *OutAbortToken = State->bAborted;
    }

    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([State](float /*DeltaTime*/) -> bool
        {
            // Abandoned by the caller (shutdown): the Work captures may
            // dangle — never invoke it. Trigger and go away.
            if (State->bAborted->Load())
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
                if (State->Work)
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
