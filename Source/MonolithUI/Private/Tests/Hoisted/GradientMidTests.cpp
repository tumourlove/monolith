// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

// Core / test
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"

// JSON / registry
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithToolRegistry.h"

// Materials -- read-back via runtime API
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "Materials/MaterialParameters.h"
#endif

// Package / loading
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

/**
 * MonolithUI.CreateGradientMidFromSpec.Basic
 *
 * Architectural invariant: the action is parameter-driven and has NO
 * compile-time dep on any specific parent material. The TEST discovers a
 * canonical gradient parent by asset name; if that parent isn't yet present on
 * disk, the test SKIPS.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUICreateGradientMidBasicTest,
    "MonolithUI.CreateGradientMidFromSpec.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace MonolithUI::GradientMidTests
{
    static FString FindMaterialPathByAssetName(FName AssetName)
    {
        IAssetRegistry& AssetRegistry =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(AssetRegistryConstants::ModuleName).Get();
        AssetRegistry.SearchAllAssets(/*bSynchronousSearch=*/true);

        TArray<FAssetData> Assets;
        AssetRegistry.GetAllAssets(Assets, /*bIncludeOnlyOnDiskAssets=*/true);
        for (const FAssetData& Asset : Assets)
        {
            if (Asset.AssetName != AssetName)
            {
                continue;
            }

            const FString ObjectPath = Asset.GetSoftObjectPath().ToString();
            if (LoadObject<UMaterialInterface>(nullptr, *ObjectPath))
            {
                return ObjectPath;
            }
        }

        return FString();
    }
}

bool FMonolithUICreateGradientMidBasicTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::GradientMidTests;

    const FString ParentPath = FindMaterialPathByAssetName(FName(TEXT("M_TokenGradientLinear")));
    UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *ParentPath);
    if (!Parent)
    {
        AddWarning(TEXT("Canonical gradient parent material not present -- skipping. Run after the parent material exists."));
        return true;
    }

    const FString Destination = TEXT("/Game/Tests/Monolith/UI/MI_GradientBasicTest");

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("parent_material"), ParentPath);
    Params->SetStringField(TEXT("destination"), Destination);
    {
        TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
        Spec->SetNumberField(TEXT("angle_deg"), 90.0);

        TArray<TSharedPtr<FJsonValue>> Stops;
        {
            TSharedPtr<FJsonObject> S0 = MakeShared<FJsonObject>();
            S0->SetNumberField(TEXT("pos"), 0.0);
            S0->SetStringField(TEXT("color"), TEXT("#FF0000"));
            Stops.Add(MakeShared<FJsonValueObject>(S0));
        }
        {
            TSharedPtr<FJsonObject> S1 = MakeShared<FJsonObject>();
            S1->SetNumberField(TEXT("pos"), 1.0);
            S1->SetStringField(TEXT("color"), TEXT("#0000FF"));
            Stops.Add(MakeShared<FJsonValueObject>(S1));
        }
        Spec->SetArrayField(TEXT("stops"), Stops);
        Params->SetObjectField(TEXT("spec"), Spec);
    }
    Params->SetBoolField(TEXT("save"), true);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("create_gradient_mid_from_spec"), Params);

    TestTrue(TEXT("create_gradient_mid_from_spec bSuccess"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        AddError(FString::Printf(TEXT("Action error: %s (code %d)"), *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }
    if (!Result.Result.IsValid())
    {
        AddError(TEXT("Result payload was null on success"));
        return false;
    }

    FString ReturnedPath;
    TestTrue(TEXT("result has asset_path"), Result.Result->TryGetStringField(TEXT("asset_path"), ReturnedPath));
    double StopsApplied = 0.0;
    TestTrue(TEXT("result has stops_applied"), Result.Result->TryGetNumberField(TEXT("stops_applied"), StopsApplied));
    TestEqual(TEXT("stops_applied == 2"), (int32)StopsApplied, 2);

    UMaterialInstanceConstant* MIC =
        LoadObject<UMaterialInstanceConstant>(nullptr, *ReturnedPath);
    if (!MIC)
    {
        AddError(FString::Printf(TEXT("Failed to reload created MIC at '%s'"), *ReturnedPath));
        return false;
    }

    TestEqual(TEXT("MIC parent matches caller's parent_material"),
        MIC->Parent ? MIC->Parent->GetPathName() : FString(), Parent->GetPathName());

    float Stop0PosRead = -1.0f;
    TestTrue(TEXT("Stop0Pos queryable"), MIC->GetScalarParameterValue(
        FMaterialParameterInfo(FName(TEXT("Stop0Pos"))), Stop0PosRead));
    TestEqual(TEXT("Stop0Pos == 0.0"), Stop0PosRead, 0.0f);

    float Stop1PosRead = -1.0f;
    TestTrue(TEXT("Stop1Pos queryable"), MIC->GetScalarParameterValue(
        FMaterialParameterInfo(FName(TEXT("Stop1Pos"))), Stop1PosRead));
    TestEqual(TEXT("Stop1Pos == 1.0"), Stop1PosRead, 1.0f);

    FLinearColor Stop0ColorRead = FLinearColor::Black;
    TestTrue(TEXT("Stop0Color queryable"), MIC->GetVectorParameterValue(
        FMaterialParameterInfo(FName(TEXT("Stop0Color"))), Stop0ColorRead));
    TestTrue(TEXT("Stop0Color is red-dominant"),
        Stop0ColorRead.R > Stop0ColorRead.G && Stop0ColorRead.R > Stop0ColorRead.B);

    FLinearColor Stop1ColorRead = FLinearColor::Black;
    TestTrue(TEXT("Stop1Color queryable"), MIC->GetVectorParameterValue(
        FMaterialParameterInfo(FName(TEXT("Stop1Color"))), Stop1ColorRead));
    TestTrue(TEXT("Stop1Color is blue-dominant"),
        Stop1ColorRead.B > Stop1ColorRead.R && Stop1ColorRead.B > Stop1ColorRead.G);

    return true;
}

/**
 * MonolithUI.CreateGradientMidFromSpec.IncompatibleParent
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUICreateGradientMidIncompatibleParentTest,
    "MonolithUI.CreateGradientMidFromSpec.IncompatibleParent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUICreateGradientMidIncompatibleParentTest::RunTest(const FString& Parameters)
{
    const FString ProbePath = TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");
    UMaterialInterface* Probe = LoadObject<UMaterialInterface>(nullptr, *ProbePath);
    if (!Probe)
    {
        AddWarning(FString::Printf(
            TEXT("Skipping: probe engine material '%s' not loadable -- can't exercise the incompatible-parent path."),
            *ProbePath));
        return true;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("parent_material"), ProbePath);
    Params->SetStringField(TEXT("destination"),
        TEXT("/Game/Tests/Monolith/UI/MI_GradientIncompatibleTest"));
    {
        TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Stops;
        TSharedPtr<FJsonObject> S0 = MakeShared<FJsonObject>();
        S0->SetNumberField(TEXT("pos"), 0.0);
        S0->SetStringField(TEXT("color"), TEXT("#FFFFFF"));
        Stops.Add(MakeShared<FJsonValueObject>(S0));
        Spec->SetArrayField(TEXT("stops"), Stops);
        Params->SetObjectField(TEXT("spec"), Spec);
    }
    Params->SetBoolField(TEXT("save"), false);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("create_gradient_mid_from_spec"), Params);

    TestFalse(TEXT("incompatible parent -> failure"), Result.bSuccess);
    TestEqual(TEXT("incompatible parent -> -32602"), Result.ErrorCode, -32602);
    TestTrue(TEXT("error message names Stop0Pos/Stop0Color"),
        Result.ErrorMessage.Contains(TEXT("Stop0Pos")) || Result.ErrorMessage.Contains(TEXT("Stop0Color")));
    return true;
}

/**
 * MonolithUI.CreateGradientMidFromSpec.InvalidParams
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUICreateGradientMidInvalidParamsTest,
    "MonolithUI.CreateGradientMidFromSpec.InvalidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUICreateGradientMidInvalidParamsTest::RunTest(const FString& Parameters)
{
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("create_gradient_mid_from_spec"), P);
        TestFalse(TEXT("empty params -> failure"), R.bSuccess);
        TestEqual(TEXT("empty params -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("parent_material"), TEXT("/Some/Parent"));
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("create_gradient_mid_from_spec"), P);
        TestFalse(TEXT("missing destination -> failure"), R.bSuccess);
        TestEqual(TEXT("missing destination -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("parent_material"), TEXT("/Some/Parent"));
        P->SetStringField(TEXT("destination"), TEXT("/Game/Foo/Bar"));
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("create_gradient_mid_from_spec"), P);
        TestFalse(TEXT("missing spec -> failure"), R.bSuccess);
        TestEqual(TEXT("missing spec -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("parent_material"), TEXT("/Some/Parent"));
        P->SetStringField(TEXT("destination"), TEXT("/Game/Foo/Bar"));
        TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Empty;
        Spec->SetArrayField(TEXT("stops"), Empty);
        P->SetObjectField(TEXT("spec"), Spec);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("create_gradient_mid_from_spec"), P);
        TestFalse(TEXT("empty stops -> failure"), R.bSuccess);
        TestEqual(TEXT("empty stops -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("parent_material"), TEXT("/Some/Parent"));
        P->SetStringField(TEXT("destination"), TEXT("/Game/Foo/Bar"));
        TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Stops;
        TSharedPtr<FJsonObject> S0 = MakeShared<FJsonObject>();
        S0->SetNumberField(TEXT("pos"), 0.0);
        Stops.Add(MakeShared<FJsonValueObject>(S0));
        Spec->SetArrayField(TEXT("stops"), Stops);
        P->SetObjectField(TEXT("spec"), Spec);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("create_gradient_mid_from_spec"), P);
        TestFalse(TEXT("stop missing color -> failure"), R.bSuccess);
        TestEqual(TEXT("stop missing color -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("parent_material"),
            TEXT("/Game/NonExistent/Parent/Path/That/Does/Not/Load"));
        P->SetStringField(TEXT("destination"),
            TEXT("/Game/Tests/Monolith/UI/MI_GradientNoParentTest"));
        TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Stops;
        TSharedPtr<FJsonObject> S0 = MakeShared<FJsonObject>();
        S0->SetNumberField(TEXT("pos"), 0.0);
        S0->SetStringField(TEXT("color"), TEXT("#FFFFFF"));
        Stops.Add(MakeShared<FJsonValueObject>(S0));
        Spec->SetArrayField(TEXT("stops"), Stops);
        P->SetObjectField(TEXT("spec"), Spec);
        P->SetBoolField(TEXT("save"), false);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("create_gradient_mid_from_spec"), P);
        TestFalse(TEXT("unloadable parent -> failure"), R.bSuccess);
        TestEqual(TEXT("unloadable parent -> -32602"), R.ErrorCode, -32602);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
