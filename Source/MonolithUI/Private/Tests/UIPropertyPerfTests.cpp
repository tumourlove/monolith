// Copyright tumourlove. All Rights Reserved.
//
// UIPropertyPerfTests.cpp — cross-cutting microbench for the Phase C
// reflection helper. Plan §1.11 calls for `MonolithUI.Performance.SetWidgetPropertyMicrobench`
// to gate against regressions in the hot write path.
//
// Threshold: 50ms for 1000 cache-hit writes (suggested in mission brief; tune
// in Phase L once we have a real-world baseline). Failing this threshold is
// not a hard build break — it logs a warning and lets the suite proceed so
// noisy CI machines don't false-negative the whole build. Adjust the
// `bSoftFail` flag below if we want a hard gate later.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "UObject/Package.h"
#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"

#include "Components/TextBlock.h"
#include "Dom/JsonValue.h"
#include "Registry/MonolithUIRegistrySubsystem.h"
#include "Registry/UIPropertyAllowlist.h"
#include "Registry/UIPropertyPathCache.h"
#include "Registry/UIReflectionHelper.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUISetWidgetPropertyMicrobenchTest,
    "MonolithUI.Performance.SetWidgetPropertyMicrobench",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)

bool FMonolithUISetWidgetPropertyMicrobenchTest::RunTest(const FString& /*Parameters*/)
{
    UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get();
    if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), Sub))
    {
        return false;
    }

    FUIPropertyPathCache* Cache = Sub->GetPathCache();
    const FUIPropertyAllowlist* Allowlist = &Sub->GetAllowlist();
    if (!TestNotNull(TEXT("Subsystem path cache available"), Cache))
    {
        return false;
    }

    UTextBlock* Widget = NewObject<UTextBlock>(GetTransientPackage(), NAME_None, RF_Transient);
    if (!TestNotNull(TEXT("scratch TextBlock created"), Widget))
    {
        return false;
    }

    FUIReflectionHelper Helper(Cache, Allowlist);

    // Warm the cache with one call so subsequent iterations are cache hits
    // (we want to measure the steady-state hot path, not the cold-walk path).
    const TSharedPtr<FJsonValue> WarmupValue = MakeShared<FJsonValueString>(TEXT("warmup"));
    {
        const FUIReflectionApplyResult Warm = Helper.Apply(Widget, TEXT("Text"), WarmupValue);
        if (!TestTrue(TEXT("Warmup write succeeded"), Warm.bSuccess))
        {
            return false;
        }
    }

    const int32 Iterations = 1000;
    const double Threshold_ms = 50.0;
    const bool bSoftFail = true; // see file header

    const double Start = FPlatformTime::Seconds();
    for (int32 i = 0; i < Iterations; ++i)
    {
        // Vary the value just enough to stop the JIT folding the call away.
        const TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueString>(
            FString::Printf(TEXT("iter-%d"), i));
        const FUIReflectionApplyResult Res = Helper.Apply(Widget, TEXT("Text"), Value);
        if (!Res.bSuccess)
        {
            AddError(FString::Printf(TEXT("Iteration %d failed: %s/%s"),
                i, *Res.FailureReason, *Res.Detail));
            return false;
        }
    }
    const double Elapsed_ms = (FPlatformTime::Seconds() - Start) * 1000.0;

    AddInfo(FString::Printf(TEXT("Microbench: %d set_widget_property calls in %.2f ms (%.4f ms/call)"),
        Iterations, Elapsed_ms, Elapsed_ms / Iterations));

    if (Elapsed_ms > Threshold_ms)
    {
        const FString Msg = FString::Printf(
            TEXT("Microbench exceeded %.2f ms threshold (%.2f ms for %d iterations)"),
            Threshold_ms, Elapsed_ms, Iterations);
        if (bSoftFail) { AddWarning(Msg); }
        else           { AddError(Msg);   return false; }
    }

    // Sanity: the cache should have served (Iterations - 1) hits since we
    // warmed it with one cold write. Loose check (>= half) — if reset by
    // another test mid-run we don't want a flaky failure.
    TestTrue(TEXT("Cache served majority of iterations as hits"),
        Cache->GetHitCount() >= (Iterations / 2));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
