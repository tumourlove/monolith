// Copyright tumourlove. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Misc/AutomationTest.h"
#include "MonolithEditorActions.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithStartPieRejectsInvalidCompilePolicyTest,
	"Monolith.Editor.PIE.StartPieRejectsInvalidCompilePolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithStartPieRejectsInvalidCompilePolicyTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("on_compile_errors"), TEXT("prompt"));

	const FMonolithActionResult Result = FMonolithEditorActions::HandleStartPIE(Params);
	TestFalse(TEXT("Invalid compile-error policy is rejected"), Result.bSuccess);
	TestEqual(TEXT("Invalid policy is an invalid-params error"), Result.ErrorCode, -32602);
	TestTrue(TEXT("Error names the accepted policies"),
		Result.ErrorMessage.Contains(TEXT("refuse")) && Result.ErrorMessage.Contains(TEXT("suppress")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithStartPieRefusesErroredBlueprintTest,
	"Monolith.Editor.PIE.StartPieRefusesErroredBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithStartPieRefusesErroredBlueprintTest::RunTest(const FString& /*Parameters*/)
{
	UBlueprint* BrokenBlueprint = NewObject<UBlueprint>(
		GetTransientPackage(),
		TEXT("MonolithStartPieBrokenBlueprint"));
	BrokenBlueprint->Status = BS_Error;
	BrokenBlueprint->bDisplayCompilePIEWarning = true;

	const FMonolithActionResult Result = FMonolithEditorActions::HandleStartPIE(MakeShared<FJsonObject>());
	TestFalse(TEXT("Default policy refuses an errored Blueprint"), Result.bSuccess);
	TestTrue(TEXT("Refusal carries structured error data"), Result.ErrorData.IsValid());

	bool bFoundTestBlueprint = false;
	if (Result.ErrorData.IsValid() && Result.ErrorData->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> ErrorData = Result.ErrorData->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* Blueprints = nullptr;
		if (ErrorData.IsValid() && ErrorData->TryGetArrayField(TEXT("errored_blueprints"), Blueprints) && Blueprints)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Blueprints)
			{
				const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
				if (Entry.IsValid() && Entry->GetStringField(TEXT("name")) == BrokenBlueprint->GetName())
				{
					bFoundTestBlueprint = true;
					break;
				}
			}
		}
	}
	TestTrue(TEXT("Structured refusal lists the offending Blueprint"), bFoundTestBlueprint);

	BrokenBlueprint->bDisplayCompilePIEWarning = false;
	BrokenBlueprint->Status = BS_UpToDate;
	BrokenBlueprint->MarkAsGarbage();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
