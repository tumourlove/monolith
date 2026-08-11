// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "UObject/Package.h"
#include "Misc/AutomationTest.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Layout/Margin.h"
#include "Math/Color.h"
#include "Registry/MonolithUIRegistrySubsystem.h"
#include "Registry/UIPropertyAllowlist.h"
#include "Registry/UIPropertyPathCache.h"
#include "Registry/UIReflectionHelper.h"

namespace
{
    // Build a transient TextBlock for inline write-tests. Outer is the
    // transient package — no editor world / WBP needed.
    UTextBlock* MakeScratchTextBlock()
    {
        return NewObject<UTextBlock>(GetTransientPackage(), NAME_None, RF_Transient);
    }

    UBorder* MakeScratchBorder()
    {
        return NewObject<UBorder>(GetTransientPackage(), NAME_None, RF_Transient);
    }

    UImage* MakeScratchImage()
    {
        return NewObject<UImage>(GetTransientPackage(), NAME_None, RF_Transient);
    }

    // Helper: subsystem-bound helper (cache + allowlist live there).
    FUIReflectionHelper MakeSubsystemHelper()
    {
        UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get();
        FUIPropertyPathCache* Cache = Sub ? Sub->GetPathCache() : nullptr;
        const FUIPropertyAllowlist* Allowlist = Sub ? &Sub->GetAllowlist() : nullptr;
        return FUIReflectionHelper(Cache, Allowlist);
    }
}

/**
 * MonolithUI.Reflection.SetVisibilityHidden (C3)
 *
 * `Visibility` is a curated path on every common UMG leaf. Apply with the
 * string "Hidden" must succeed and the widget's Visibility must change.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIReflectionSetVisibilityTest,
    "MonolithUI.Reflection.SetVisibilityHidden",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIReflectionSetVisibilityTest::RunTest(const FString& /*Parameters*/)
{
    UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get();
    if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), Sub))
    {
        return false;
    }

    UTextBlock* Widget = MakeScratchTextBlock();
    if (!TestNotNull(TEXT("scratch TextBlock created"), Widget))
    {
        return false;
    }

    // Visibility curated mapping isn't wired by Phase B (no entry on TextBlock
    // for "Visibility"). To keep this test allowlist-honest, run with
    // bRawMode=true. The dedicated allowlist gate test below confirms the
    // gate path with a known-curated path (Text).
    FUIReflectionHelper Helper = MakeSubsystemHelper();
    const TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueString>(TEXT("Hidden"));

    const FUIReflectionApplyResult Res = Helper.Apply(Widget, TEXT("Visibility"), Value, /*bRawMode=*/true);

    TestTrue(FString::Printf(TEXT("Apply succeeded: %s/%s"), *Res.FailureReason, *Res.Detail), Res.bSuccess);
    TestTrue(TEXT("Widget visibility flipped to Hidden"),
        Widget->GetVisibility() == ESlateVisibility::Hidden);

    return true;
}

/**
 * MonolithUI.Reflection.SetTextOnTextBlock (C3 — allowlist-honoured)
 *
 * `Text` IS curated for TextBlock (see RegisterCuratedMappings). Default
 * (non-raw) mode must succeed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIReflectionSetTextTest,
    "MonolithUI.Reflection.SetTextOnTextBlock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIReflectionSetTextTest::RunTest(const FString& /*Parameters*/)
{
    UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get();
    if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), Sub))
    {
        return false;
    }

    UTextBlock* Widget = MakeScratchTextBlock();
    if (!TestNotNull(TEXT("scratch TextBlock created"), Widget))
    {
        return false;
    }

    FUIReflectionHelper Helper = MakeSubsystemHelper();
    const TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueString>(TEXT("Hello, Omnissiah."));

    // Default mode (bRawMode=false) — allowlist gate must permit `Text`.
    const FUIReflectionApplyResult Res = Helper.Apply(Widget, TEXT("Text"), Value);

    TestTrue(FString::Printf(TEXT("Apply succeeded: %s/%s"), *Res.FailureReason, *Res.Detail), Res.bSuccess);
    TestEqual(TEXT("Widget text matches written value"),
        Widget->GetText().ToString(), FString(TEXT("Hello, Omnissiah.")));

    return true;
}

/**
 * MonolithUI.Reflection.SetTextBlockLineHeight
 *
 * TextBlock line-height controls are inherited from UTextLayoutWidget and are
 * curated generic text-layout paths. Default non-raw mode must write both the
 * scalar line-height percentage and the final-line spacing flag.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIReflectionSetTextBlockLineHeightTest,
    "MonolithUI.Reflection.SetTextBlockLineHeight",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIReflectionSetTextBlockLineHeightTest::RunTest(const FString& /*Parameters*/)
{
    UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get();
    if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), Sub))
    {
        return false;
    }

    UTextBlock* Widget = MakeScratchTextBlock();
    if (!TestNotNull(TEXT("scratch TextBlock created"), Widget))
    {
        return false;
    }

    FUIReflectionHelper Helper = MakeSubsystemHelper();
    const FName Token(TEXT("TextBlock"));

    const TSharedPtr<FJsonValue> LineHeight = MakeShared<FJsonValueNumber>(1.35);
    const FUIReflectionApplyResult LineHeightRes = Helper.Apply(Widget, TEXT("LineHeightPercentage"), LineHeight);
    TestTrue(FString::Printf(TEXT("LineHeightPercentage apply succeeded: %s/%s"), *LineHeightRes.FailureReason, *LineHeightRes.Detail), LineHeightRes.bSuccess);

    TSharedPtr<FJsonValue> LineHeightRead;
    const FUIReflectionApplyResult LineHeightReadRes = Helper.ReadJsonPath(Widget, Token, TEXT("LineHeightPercentage"), LineHeightRead);
    TestTrue(FString::Printf(TEXT("LineHeightPercentage read succeeded: %s/%s"), *LineHeightReadRes.FailureReason, *LineHeightReadRes.Detail), LineHeightReadRes.bSuccess);
    TestTrue(TEXT("LineHeightPercentage read value is numeric"), LineHeightRead.IsValid() && LineHeightRead->Type == EJson::Number);
    if (LineHeightRead.IsValid() && LineHeightRead->Type == EJson::Number)
    {
        TestEqual(TEXT("LineHeightPercentage written value"), static_cast<float>(LineHeightRead->AsNumber()), 1.35f);
    }

    const TSharedPtr<FJsonValue> ApplyToBottomLine = MakeShared<FJsonValueBoolean>(false);
    const FUIReflectionApplyResult ApplyToBottomLineRes = Helper.Apply(Widget, TEXT("ApplyLineHeightToBottomLine"), ApplyToBottomLine);
    TestTrue(FString::Printf(TEXT("ApplyLineHeightToBottomLine apply succeeded: %s/%s"), *ApplyToBottomLineRes.FailureReason, *ApplyToBottomLineRes.Detail), ApplyToBottomLineRes.bSuccess);

    TSharedPtr<FJsonValue> ApplyToBottomLineRead;
    const FUIReflectionApplyResult ApplyToBottomLineReadRes = Helper.ReadJsonPath(Widget, Token, TEXT("ApplyLineHeightToBottomLine"), ApplyToBottomLineRead);
    TestTrue(FString::Printf(TEXT("ApplyLineHeightToBottomLine read succeeded: %s/%s"), *ApplyToBottomLineReadRes.FailureReason, *ApplyToBottomLineReadRes.Detail), ApplyToBottomLineReadRes.bSuccess);
    TestTrue(TEXT("ApplyLineHeightToBottomLine read value is boolean"), ApplyToBottomLineRead.IsValid() && ApplyToBottomLineRead->Type == EJson::Boolean);
    if (ApplyToBottomLineRead.IsValid() && ApplyToBottomLineRead->Type == EJson::Boolean)
    {
        TestFalse(TEXT("ApplyLineHeightToBottomLine written value"), ApplyToBottomLineRead->AsBool());
    }

    return true;
}

/**
 * MonolithUI.Reflection.GateRejectsUnlistedPath (C3 / C8)
 *
 * Default mode + non-allowlisted path must return bSuccess=false with
 * FailureReason=NotInAllowlist. The widget's actual property may or may not
 * exist — the gate runs BEFORE the property walk, so even a real engine flag
 * gets refused unless curated.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIReflectionGateRejectsTest,
    "MonolithUI.Reflection.GateRejectsUnlistedPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIReflectionGateRejectsTest::RunTest(const FString& /*Parameters*/)
{
    UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get();
    if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), Sub))
    {
        return false;
    }

    UTextBlock* Widget = MakeScratchTextBlock();
    if (!TestNotNull(TEXT("scratch TextBlock created"), Widget))
    {
        return false;
    }

    FUIReflectionHelper Helper = MakeSubsystemHelper();
    const TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueString>(TEXT("anything"));

    // Phase B does NOT curate "MinDesiredHeight" on TextBlock (it's curated
    // on SizeBox only). Default mode must refuse.
    const FUIReflectionApplyResult Res = Helper.Apply(Widget, TEXT("MinDesiredHeight"), Value);

    TestFalse(TEXT("Default-mode apply on non-allowlisted path is REJECTED"), Res.bSuccess);
    TestEqual(TEXT("Failure reason is NotInAllowlist"),
        Res.FailureReason, FString(TEXT("NotInAllowlist")));
    TestEqual(TEXT("PropertyPath echoed in result"),
        Res.PropertyPath, FString(TEXT("MinDesiredHeight")));

    return true;
}

/**
 * MonolithUI.Reflection.RawModeBypassesGate
 *
 * bRawMode=true must skip the allowlist gate AND succeed for an
 * actually-existing property — verifies the legacy raw-mode contract.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIReflectionRawModeTest,
    "MonolithUI.Reflection.RawModeBypassesGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIReflectionRawModeTest::RunTest(const FString& /*Parameters*/)
{
    UTextBlock* Widget = MakeScratchTextBlock();
    if (!TestNotNull(TEXT("scratch TextBlock created"), Widget))
    {
        return false;
    }

    FUIReflectionHelper Helper = MakeSubsystemHelper();
    const TSharedPtr<FJsonValue> OpacityVal = MakeShared<FJsonValueNumber>(0.5);

    // RenderOpacity exists on UWidget but Phase B doesn't curate it for
    // TextBlock — raw mode must bypass the gate and write through.
    const FUIReflectionApplyResult Res = Helper.Apply(Widget, TEXT("RenderOpacity"), OpacityVal, /*bRawMode=*/true);
    TestTrue(FString::Printf(TEXT("RawMode write succeeded: %s/%s"), *Res.FailureReason, *Res.Detail), Res.bSuccess);
    TestEqual(TEXT("RenderOpacity written"), Widget->GetRenderOpacity(), 0.5f);

    return true;
}

/**
 * MonolithUI.Reflection.ParseLinearColorHex (C4 — struct parser)
 *
 * Writes a "#FF8800" string into UImage.ColorAndOpacity (curated FLinearColor
 * mapping). Verifies the FLinearColor parser accepts hex strings.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIReflectionLinearColorHexTest,
    "MonolithUI.Reflection.ParseLinearColorHex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIReflectionLinearColorHexTest::RunTest(const FString& /*Parameters*/)
{
    UImage* Widget = MakeScratchImage();
    if (!TestNotNull(TEXT("scratch Image created"), Widget))
    {
        return false;
    }

    FUIReflectionHelper Helper = MakeSubsystemHelper();
    const TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueString>(TEXT("#FF8800"));

    const FUIReflectionApplyResult Res = Helper.Apply(Widget, TEXT("ColorAndOpacity"), Value);
    TestTrue(FString::Printf(TEXT("Apply succeeded: %s/%s"), *Res.FailureReason, *Res.Detail), Res.bSuccess);

    // Round-trip the written linear color back through ToFColor(true) (sRGB).
    const FColor Sampled = Widget->GetColorAndOpacity().ToFColor(true);
    TestEqual(TEXT("R byte round-trips"), (int32)Sampled.R, 0xFF);
    TestEqual(TEXT("G byte round-trips"), (int32)Sampled.G, 0x88);
    TestEqual(TEXT("B byte round-trips"), (int32)Sampled.B, 0x00);

    return true;
}

/**
 * MonolithUI.Reflection.ParseLinearColorArray (C4 — struct parser)
 *
 * Writes a [r,g,b,a] array shape.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIReflectionLinearColorArrayTest,
    "MonolithUI.Reflection.ParseLinearColorArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIReflectionLinearColorArrayTest::RunTest(const FString& /*Parameters*/)
{
    UImage* Widget = MakeScratchImage();
    if (!TestNotNull(TEXT("scratch Image created"), Widget))
    {
        return false;
    }

    TArray<TSharedPtr<FJsonValue>> Components;
    Components.Add(MakeShared<FJsonValueNumber>(0.25));
    Components.Add(MakeShared<FJsonValueNumber>(0.5));
    Components.Add(MakeShared<FJsonValueNumber>(0.75));
    Components.Add(MakeShared<FJsonValueNumber>(1.0));
    const TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueArray>(Components);

    FUIReflectionHelper Helper = MakeSubsystemHelper();
    const FUIReflectionApplyResult Res = Helper.Apply(Widget, TEXT("ColorAndOpacity"), Value);
    TestTrue(FString::Printf(TEXT("Apply succeeded: %s/%s"), *Res.FailureReason, *Res.Detail), Res.bSuccess);

    const FLinearColor C = Widget->GetColorAndOpacity();
    TestEqual(TEXT("R component"), C.R, 0.25f);
    TestEqual(TEXT("G component"), C.G, 0.5f);
    TestEqual(TEXT("B component"), C.B, 0.75f);
    TestEqual(TEXT("A component"), C.A, 1.0f);

    return true;
}

/**
 * MonolithUI.Reflection.ParseMarginScalar (C4 — struct parser)
 *
 * Scalar JSON number applies uniform Margin to all four sides.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIReflectionMarginScalarTest,
    "MonolithUI.Reflection.ParseMarginScalar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIReflectionMarginScalarTest::RunTest(const FString& /*Parameters*/)
{
    UBorder* Widget = MakeScratchBorder();
    if (!TestNotNull(TEXT("scratch Border created"), Widget))
    {
        return false;
    }

    FUIReflectionHelper Helper = MakeSubsystemHelper();
    const TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueNumber>(8.0);

    const FUIReflectionApplyResult Res = Helper.Apply(Widget, TEXT("Padding"), Value);
    TestTrue(FString::Printf(TEXT("Apply succeeded: %s/%s"), *Res.FailureReason, *Res.Detail), Res.bSuccess);

    const FMargin Pad = Widget->GetPadding();
    TestEqual(TEXT("Left"), Pad.Left, 8.0f);
    TestEqual(TEXT("Top"), Pad.Top, 8.0f);
    TestEqual(TEXT("Right"), Pad.Right, 8.0f);
    TestEqual(TEXT("Bottom"), Pad.Bottom, 8.0f);

    return true;
}

/**
 * MonolithUI.Reflection.ParseMarginObject (C4 — struct parser)
 *
 * Object form {left,top,right,bottom}.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIReflectionMarginObjectTest,
    "MonolithUI.Reflection.ParseMarginObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIReflectionMarginObjectTest::RunTest(const FString& /*Parameters*/)
{
    UBorder* Widget = MakeScratchBorder();
    if (!TestNotNull(TEXT("scratch Border created"), Widget))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetNumberField(TEXT("left"),   1.0);
    Obj->SetNumberField(TEXT("top"),    2.0);
    Obj->SetNumberField(TEXT("right"),  3.0);
    Obj->SetNumberField(TEXT("bottom"), 4.0);

    FUIReflectionHelper Helper = MakeSubsystemHelper();
    const TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueObject>(Obj);
    const FUIReflectionApplyResult Res = Helper.Apply(Widget, TEXT("Padding"), Value);
    TestTrue(FString::Printf(TEXT("Apply succeeded: %s/%s"), *Res.FailureReason, *Res.Detail), Res.bSuccess);

    const FMargin Pad = Widget->GetPadding();
    TestEqual(TEXT("Left"), Pad.Left, 1.0f);
    TestEqual(TEXT("Top"), Pad.Top, 2.0f);
    TestEqual(TEXT("Right"), Pad.Right, 3.0f);
    TestEqual(TEXT("Bottom"), Pad.Bottom, 4.0f);

    return true;
}

/**
 * MonolithUI.Reflection.NullRootRejected
 *
 * Defensive — null Root must not crash; returns PropertyNotFound.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIReflectionNullRootTest,
    "MonolithUI.Reflection.NullRootRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIReflectionNullRootTest::RunTest(const FString& /*Parameters*/)
{
    FUIReflectionHelper Helper = MakeSubsystemHelper();
    const TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueString>(TEXT("ignored"));

    const FUIReflectionApplyResult Res = Helper.Apply(nullptr, TEXT("Text"), Value);
    TestFalse(TEXT("Null root rejected"), Res.bSuccess);
    TestEqual(TEXT("Reason is PropertyNotFound"), Res.FailureReason, FString(TEXT("PropertyNotFound")));

    return true;
}

/**
 * MonolithUI.Reflection.UnknownPropertyRejected
 *
 * In raw mode (no allowlist gate), an unresolved path returns
 * PropertyNotFound — distinguishes from NotInAllowlist failures.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIReflectionUnknownPropertyTest,
    "MonolithUI.Reflection.UnknownPropertyRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIReflectionUnknownPropertyTest::RunTest(const FString& /*Parameters*/)
{
    UTextBlock* Widget = MakeScratchTextBlock();
    if (!TestNotNull(TEXT("scratch TextBlock created"), Widget))
    {
        return false;
    }

    FUIReflectionHelper Helper = MakeSubsystemHelper();
    const TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueString>(TEXT("anything"));

    const FUIReflectionApplyResult Res = Helper.Apply(Widget, TEXT("DefinitelyNotARealProperty_91827463"), Value, /*bRawMode=*/true);
    TestFalse(TEXT("Unknown property in raw mode rejected"), Res.bSuccess);
    TestEqual(TEXT("Reason is PropertyNotFound"), Res.FailureReason, FString(TEXT("PropertyNotFound")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
