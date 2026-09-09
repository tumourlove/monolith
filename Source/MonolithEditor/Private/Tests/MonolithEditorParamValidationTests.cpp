// Copyright tumourlove. All Rights Reserved.

// =============================================================================
// MonolithEditorParamValidationTests.cpp
//
// Regression tests for the invalid-params (-32602) rejections on the editor
// namespace's action entry points.
//
// SCOPE — deliberately limited to validation paths that return BEFORE any side
// effect: no PIE session is queued, no asset is loaded, nothing is rendered and
// nothing is written to disk. That keeps the suite safe to run in a live editor.
//
//   - Monolith.Editor.PIE.StartPieRejectsInvalidCompilePolicy
//       start_pie rejects an unrecognised on_compile_errors policy instead of
//       silently falling through to the strictest mode.
//   - Monolith.Editor.Preview.CaptureRejectsNonPositiveSize
//   - Monolith.Editor.Preview.CaptureRejectsOversizedCapture
//       capture_scene_preview range-checks width/height before it loads the
//       asset, so a bogus asset_path never reaches the render path.
// =============================================================================

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"

#include "MonolithEditorActions.h"
#include "MonolithToolRegistry.h" // FMonolithActionResult

namespace MonolithEditorParamValidationTests
{
	/** capture_scene_preview params with an asset that is never reached (validation errors first). */
	static TSharedPtr<FJsonObject> MakeSizeParams(double Width, double Height)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_type"), TEXT("widget"));
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Monolith/DoesNotExist_ParamValidation"));
		Params->SetNumberField(TEXT("width"), Width);
		Params->SetNumberField(TEXT("height"), Height);
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithStartPieRejectsInvalidCompilePolicyTest,
	"Monolith.Editor.PIE.StartPieRejectsInvalidCompilePolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithStartPieRejectsInvalidCompilePolicyTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("on_compile_errors"), TEXT("prompt"));

	const FMonolithActionResult Result = FMonolithEditorActions::HandleStartPIE(Params);
	TestFalse(TEXT("Unrecognised compile-error policy is rejected"), Result.bSuccess);
	TestEqual(TEXT("Rejection is an invalid-params error"), Result.ErrorCode, -32602);
	TestTrue(TEXT("Error names the accepted policies"),
		Result.ErrorMessage.Contains(TEXT("refuse")) && Result.ErrorMessage.Contains(TEXT("suppress")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithCaptureRejectsNonPositiveSizeTest,
	"Monolith.Editor.Preview.CaptureRejectsNonPositiveSize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithCaptureRejectsNonPositiveSizeTest::RunTest(const FString& /*Parameters*/)
{
	const FMonolithActionResult Result = FMonolithEditorActions::HandleCaptureScenePreview(
		MonolithEditorParamValidationTests::MakeSizeParams(0.0, 760.0));

	TestFalse(TEXT("Zero width is rejected"), Result.bSuccess);
	TestEqual(TEXT("Rejection is an invalid-params error"), Result.ErrorCode, -32602);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithCaptureRejectsOversizedCaptureTest,
	"Monolith.Editor.Preview.CaptureRejectsOversizedCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithCaptureRejectsOversizedCaptureTest::RunTest(const FString& /*Parameters*/)
{
	const FMonolithActionResult Result = FMonolithEditorActions::HandleCaptureScenePreview(
		MonolithEditorParamValidationTests::MakeSizeParams(1200.0, 100000.0));

	TestFalse(TEXT("Height beyond the per-side ceiling is rejected"), Result.bSuccess);
	TestEqual(TEXT("Rejection is an invalid-params error"), Result.ErrorCode, -32602);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
