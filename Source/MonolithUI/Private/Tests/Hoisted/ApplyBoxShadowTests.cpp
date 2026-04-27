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

// UMG -- build a throwaway WBP + probe the result
#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"
#include "Components/Border.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/SizeBox.h"

// Asset / package
#include "Editor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

// Materials -- read-back
#include "Materials/MaterialInterface.h"

/**
 * MonolithUI.ApplyBoxShadow.Basic
 *
 * Architectural invariant: apply_box_shadow has ZERO compile dep on any
 * specific shadow material. The TEST discovers a canonical shadow material by
 * asset name; if that parent isn't yet present on disk, the test SKIPS.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIApplyBoxShadowBasicTest,
    "MonolithUI.ApplyBoxShadow.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace MonolithUI::ApplyBoxShadowTests
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

bool FMonolithUIApplyBoxShadowBasicTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::TestUtils;
    using namespace MonolithUI::ApplyBoxShadowTests;

    const FString ShadowMaterialPath = FindMaterialPathByAssetName(FName(TEXT("M_TokenShadow")));
    UMaterialInterface* ShadowParent = LoadObject<UMaterialInterface>(nullptr, *ShadowMaterialPath);
    if (!ShadowParent)
    {
        AddWarning(TEXT("Canonical shadow material not present -- skipping. Run after the parent material exists."));
        return true;
    }

    const FString WBPPath = TEXT("/Game/Tests/Monolith/UI/WBP_ShadowTest");
    FString FixtureError;
    UWidget* TargetWidget = nullptr;
    if (!CreateOrReuseTestWidgetBlueprint(WBPPath, FName(TEXT("Target")), nullptr, FixtureError, &TargetWidget))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }
    if (TargetWidget)
    {
        if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(TargetWidget->Slot))
        {
            CSlot->SetPosition(FVector2D(100.0f, 100.0f));
            CSlot->SetSize(FVector2D(200.0f, 120.0f));
        }
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), WBPPath);
    Params->SetStringField(TEXT("widget_name"), TEXT("Target"));
    Params->SetStringField(TEXT("shadow_material_path"), ShadowMaterialPath);
    {
        TSharedPtr<FJsonObject> Shadow = MakeShared<FJsonObject>();
        Shadow->SetNumberField(TEXT("x"), 0.0);
        Shadow->SetNumberField(TEXT("y"), 4.0);
        Shadow->SetNumberField(TEXT("blur"), 8.0);
        Shadow->SetStringField(TEXT("color"), TEXT("#000000AA"));
        Params->SetObjectField(TEXT("shadow"), Shadow);
    }
    Params->SetBoolField(TEXT("compile"), true);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("apply_box_shadow"), Params);

    TestTrue(TEXT("apply_box_shadow bSuccess"), Result.bSuccess);
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
    double LayersApplied = 0.0;
    TestTrue(TEXT("result has layers_applied"),
        Result.Result->TryGetNumberField(TEXT("layers_applied"), LayersApplied));
    TestEqual(TEXT("layers_applied == 1"), (int32)LayersApplied, 1);

    const TArray<TSharedPtr<FJsonValue>>* ShadowNamesArr = nullptr;
    TestTrue(TEXT("result has shadow_widgets array"),
        Result.Result->TryGetArrayField(TEXT("shadow_widgets"), ShadowNamesArr));

    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
    if (WBP && WBP->WidgetTree)
    {
        UWidget* Shadow = WBP->WidgetTree->FindWidget(FName(TEXT("Target_Shadow0")));
        TestNotNull(TEXT("Target_Shadow0 exists in widget tree"), Shadow);

        UImage* ShadowImg = Cast<UImage>(Shadow);
        TestNotNull(TEXT("Shadow is a UImage"), ShadowImg);

        UWidget* Target = WBP->WidgetTree->FindWidget(FName(TEXT("Target")));
        if (Shadow && Target)
        {
            UPanelWidget* Parent = Target->GetParent();
            if (Parent)
            {
                const int32 ShadowIdx = Parent->GetChildIndex(Shadow);
                const int32 TargetIdx = Parent->GetChildIndex(Target);
                TestTrue(TEXT("Shadow inserted BEFORE Target in parent child list"),
                    ShadowIdx >= 0 && TargetIdx >= 0 && ShadowIdx < TargetIdx);
            }
        }

        if (ShadowImg)
        {
            UObject* Res = ShadowImg->GetBrush().GetResourceObject();
            TestNotNull(TEXT("Shadow brush has a resource object"), Res);
            TestTrue(TEXT("Shadow brush resource is a UMaterialInterface"),
                Res != nullptr && Res->IsA<UMaterialInterface>());
        }
    }
    return true;
}

/**
 * MonolithUI.ApplyBoxShadow.SizeBoxParent
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIApplyBoxShadowSizeBoxParentTest,
    "MonolithUI.ApplyBoxShadow.SizeBoxParent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIApplyBoxShadowSizeBoxParentTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::TestUtils;
    using namespace MonolithUI::ApplyBoxShadowTests;

    const FString ShadowMaterialPath = FindMaterialPathByAssetName(FName(TEXT("M_TokenShadow")));
    UMaterialInterface* ShadowParent = LoadObject<UMaterialInterface>(nullptr, *ShadowMaterialPath);
    if (!ShadowParent)
    {
        AddWarning(TEXT("Canonical shadow material not present -- skipping SizeBox-parent test."));
        return true;
    }

    const FString WBPPath = TEXT("/Game/Tests/Monolith/UI/WBP_ShadowSizeBoxParentTest");
    FString FixtureError;
    if (!CreateOrReuseTestWidgetBlueprint(WBPPath, NAME_None, nullptr, FixtureError))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }

    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
    if (!WBP || !WBP->WidgetTree)
    {
        AddError(TEXT("Fixture WBP reload failed"));
        return false;
    }

    UCanvasPanel* Root = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
    if (!Root)
    {
        AddError(TEXT("Fixture root is not a CanvasPanel"));
        return false;
    }

    USizeBox* Wrapper = WBP->WidgetTree->ConstructWidget<USizeBox>(
        USizeBox::StaticClass(), FName(TEXT("Target_SizeBox")));
    UImage* Target = WBP->WidgetTree->ConstructWidget<UImage>(
        UImage::StaticClass(), FName(TEXT("Target")));
    if (!Wrapper || !Target)
    {
        AddError(TEXT("Failed to construct SizeBox wrapper or target image"));
        return false;
    }

    UPanelSlot* WrapperSlot = Root->AddChild(Wrapper);
    Wrapper->SetContent(Target);
    if (UCanvasPanelSlot* WrapperCanvasSlot = Cast<UCanvasPanelSlot>(WrapperSlot))
    {
        WrapperCanvasSlot->SetPosition(FVector2D(48.0f, 72.0f));
        WrapperCanvasSlot->SetSize(FVector2D(160.0f, 96.0f));
        WrapperCanvasSlot->SetZOrder(5);
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), WBPPath);
    Params->SetStringField(TEXT("widget_name"), TEXT("Target"));
    Params->SetStringField(TEXT("shadow_material_path"), ShadowMaterialPath);
    {
        TSharedPtr<FJsonObject> Shadow = MakeShared<FJsonObject>();
        Shadow->SetNumberField(TEXT("x"), 2.0);
        Shadow->SetNumberField(TEXT("y"), 6.0);
        Shadow->SetNumberField(TEXT("blur"), 8.0);
        Shadow->SetStringField(TEXT("color"), TEXT("#00000088"));
        Params->SetObjectField(TEXT("shadow"), Shadow);
    }
    Params->SetBoolField(TEXT("compile"), false);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("apply_box_shadow"), Params);

    TestTrue(TEXT("apply_box_shadow bSuccess with SizeBox parent"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        AddError(FString::Printf(TEXT("Action error: %s (code %d)"), *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }

    UWidget* Shadow = WBP->WidgetTree->FindWidget(FName(TEXT("Target_Shadow0")));
    TestNotNull(TEXT("Target_Shadow0 exists in widget tree"), Shadow);
    TestEqual(TEXT("Target remains inside SizeBox"), Target->GetParent(), Cast<UPanelWidget>(Wrapper));
    TestEqual(TEXT("Shadow inserted into SizeBox grandparent"), Shadow ? Shadow->GetParent() : nullptr, Cast<UPanelWidget>(Root));
    TestFalse(TEXT("Shadow was not inserted inside the SizeBox wrapper"),
        Shadow != nullptr && Wrapper->GetChildIndex(Shadow) != INDEX_NONE);

    if (Shadow)
    {
        const int32 ShadowIdx = Root->GetChildIndex(Shadow);
        const int32 WrapperIdx = Root->GetChildIndex(Wrapper);
        TestTrue(TEXT("Shadow inserted BEFORE SizeBox wrapper in grandparent child list"),
            ShadowIdx >= 0 && WrapperIdx >= 0 && ShadowIdx < WrapperIdx);

        if (UCanvasPanelSlot* ShadowCanvasSlot = Cast<UCanvasPanelSlot>(Shadow->Slot))
        {
            TestEqual(TEXT("Shadow inherits wrapper canvas z-order minus one"),
                ShadowCanvasSlot->GetZOrder(), 4);
        }
        else
        {
            AddError(TEXT("Shadow slot is not a CanvasPanelSlot"));
        }
    }

    return true;
}

/**
 * MonolithUI.ApplyBoxShadow.NestedSingleChildWrappers
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIApplyBoxShadowNestedSingleChildWrappersTest,
    "MonolithUI.ApplyBoxShadow.NestedSingleChildWrappers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIApplyBoxShadowNestedSingleChildWrappersTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::TestUtils;
    using namespace MonolithUI::ApplyBoxShadowTests;

    const FString ShadowMaterialPath = FindMaterialPathByAssetName(FName(TEXT("M_TokenShadow")));
    UMaterialInterface* ShadowParent = LoadObject<UMaterialInterface>(nullptr, *ShadowMaterialPath);
    if (!ShadowParent)
    {
        AddWarning(TEXT("Canonical shadow material not present -- skipping nested single-child-wrapper test."));
        return true;
    }

    const FString WBPPath = TEXT("/Game/Tests/Monolith/UI/WBP_ShadowNestedSingleChildWrapperTest");
    FString FixtureError;
    if (!CreateOrReuseTestWidgetBlueprint(WBPPath, NAME_None, nullptr, FixtureError))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }

    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
    if (!WBP || !WBP->WidgetTree)
    {
        AddError(TEXT("Fixture WBP reload failed"));
        return false;
    }

    UCanvasPanel* Root = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
    if (!Root)
    {
        AddError(TEXT("Fixture root is not a CanvasPanel"));
        return false;
    }

    UBorder* BorderWrapper = WBP->WidgetTree->ConstructWidget<UBorder>(
        UBorder::StaticClass(), FName(TEXT("Target_Border")));
    USizeBox* SizeBoxWrapper = WBP->WidgetTree->ConstructWidget<USizeBox>(
        USizeBox::StaticClass(), FName(TEXT("Target_SizeBox")));
    UImage* Target = WBP->WidgetTree->ConstructWidget<UImage>(
        UImage::StaticClass(), FName(TEXT("Target")));
    if (!BorderWrapper || !SizeBoxWrapper || !Target)
    {
        AddError(TEXT("Failed to construct nested wrapper test widgets"));
        return false;
    }

    UPanelSlot* BorderSlot = Root->AddChild(BorderWrapper);
    BorderWrapper->SetContent(SizeBoxWrapper);
    SizeBoxWrapper->SetContent(Target);
    if (UCanvasPanelSlot* BorderCanvasSlot = Cast<UCanvasPanelSlot>(BorderSlot))
    {
        BorderCanvasSlot->SetPosition(FVector2D(32.0f, 40.0f));
        BorderCanvasSlot->SetSize(FVector2D(180.0f, 112.0f));
        BorderCanvasSlot->SetZOrder(7);
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), WBPPath);
    Params->SetStringField(TEXT("widget_name"), TEXT("Target"));
    Params->SetStringField(TEXT("shadow_material_path"), ShadowMaterialPath);
    {
        TSharedPtr<FJsonObject> Shadow = MakeShared<FJsonObject>();
        Shadow->SetNumberField(TEXT("x"), 1.0);
        Shadow->SetNumberField(TEXT("y"), 5.0);
        Shadow->SetNumberField(TEXT("blur"), 9.0);
        Shadow->SetStringField(TEXT("color"), TEXT("#00000088"));
        Params->SetObjectField(TEXT("shadow"), Shadow);
    }
    Params->SetBoolField(TEXT("compile"), false);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("apply_box_shadow"), Params);

    TestTrue(TEXT("apply_box_shadow bSuccess with nested single-child wrappers"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        AddError(FString::Printf(TEXT("Action error: %s (code %d)"), *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }

    UWidget* Shadow = WBP->WidgetTree->FindWidget(FName(TEXT("Target_Shadow0")));
    TestNotNull(TEXT("Target_Shadow0 exists in widget tree"), Shadow);
    TestEqual(TEXT("Target remains inside SizeBox"), Target->GetParent(), Cast<UPanelWidget>(SizeBoxWrapper));
    TestEqual(TEXT("SizeBox remains inside Border"), SizeBoxWrapper->GetParent(), Cast<UPanelWidget>(BorderWrapper));
    TestEqual(TEXT("Shadow inserted into outer wrapper parent"), Shadow ? Shadow->GetParent() : nullptr, Cast<UPanelWidget>(Root));
    TestFalse(TEXT("Shadow was not inserted inside Border"),
        Shadow != nullptr && BorderWrapper->GetChildIndex(Shadow) != INDEX_NONE);
    TestFalse(TEXT("Shadow was not inserted inside SizeBox"),
        Shadow != nullptr && SizeBoxWrapper->GetChildIndex(Shadow) != INDEX_NONE);

    if (Shadow)
    {
        const int32 ShadowIdx = Root->GetChildIndex(Shadow);
        const int32 BorderIdx = Root->GetChildIndex(BorderWrapper);
        TestTrue(TEXT("Shadow inserted BEFORE outermost wrapper in parent child list"),
            ShadowIdx >= 0 && BorderIdx >= 0 && ShadowIdx < BorderIdx);

        if (UCanvasPanelSlot* ShadowCanvasSlot = Cast<UCanvasPanelSlot>(Shadow->Slot))
        {
            TestEqual(TEXT("Shadow inherits outer wrapper canvas z-order minus one"),
                ShadowCanvasSlot->GetZOrder(), 6);
        }
        else
        {
            AddError(TEXT("Shadow slot is not a CanvasPanelSlot"));
        }
    }

    return true;
}

/**
 * MonolithUI.ApplyBoxShadow.MultiLayerCap
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIApplyBoxShadowMultiLayerCapTest,
    "MonolithUI.ApplyBoxShadow.MultiLayerCap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIApplyBoxShadowMultiLayerCapTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::TestUtils;
    using namespace MonolithUI::ApplyBoxShadowTests;

    const FString ShadowMaterialPath = FindMaterialPathByAssetName(FName(TEXT("M_TokenShadow")));
    UMaterialInterface* ShadowParent = LoadObject<UMaterialInterface>(nullptr, *ShadowMaterialPath);
    if (!ShadowParent)
    {
        AddWarning(TEXT("Shadow material not present -- skipping multi-layer-cap test."));
        return true;
    }

    const FString WBPPath = TEXT("/Game/Tests/Monolith/UI/WBP_ShadowMultiTest");
    FString FixtureError;
    if (!CreateOrReuseTestWidgetBlueprint(WBPPath, FName(TEXT("Target")), nullptr, FixtureError))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), WBPPath);
    Params->SetStringField(TEXT("widget_name"), TEXT("Target"));
    Params->SetStringField(TEXT("shadow_material_path"), ShadowMaterialPath);
    {
        TArray<TSharedPtr<FJsonValue>> Shadows;
        for (int32 i = 0; i < 3; ++i)
        {
            TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
            S->SetNumberField(TEXT("x"), 0.0);
            S->SetNumberField(TEXT("y"), (double)(i + 1) * 2.0);
            S->SetNumberField(TEXT("blur"), (double)(i + 1) * 4.0);
            S->SetStringField(TEXT("color"), TEXT("#00000033"));
            Shadows.Add(MakeShared<FJsonValueObject>(S));
        }
        Params->SetArrayField(TEXT("shadows"), Shadows);
    }
    Params->SetBoolField(TEXT("compile"), false);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("apply_box_shadow"), Params);

    TestTrue(TEXT("apply_box_shadow bSuccess"), Result.bSuccess);
    if (!Result.bSuccess || !Result.Result.IsValid())
    {
        AddError(TEXT("Expected success payload for 3-layer shadow input"));
        return false;
    }

    double LayersApplied = 0.0;
    Result.Result->TryGetNumberField(TEXT("layers_applied"), LayersApplied);
    TestEqual(TEXT("layers_applied clamped to 2"), (int32)LayersApplied, 2);

    const TArray<TSharedPtr<FJsonValue>>* WarningsArr = nullptr;
    if (Result.Result->TryGetArrayField(TEXT("warnings"), WarningsArr) && WarningsArr)
    {
        bool bFoundCapWarning = false;
        for (const TSharedPtr<FJsonValue>& V : *WarningsArr)
        {
            FString S;
            if (V.IsValid() && V->TryGetString(S) && S.Contains(TEXT("only first 2 shadow layers applied")))
            {
                bFoundCapWarning = true;
                break;
            }
        }
        TestTrue(TEXT("warning 'only first 2 shadow layers applied' emitted"), bFoundCapWarning);
    }
    else
    {
        AddError(TEXT("Expected warnings array in result payload"));
    }
    return true;
}

/**
 * MonolithUI.ApplyBoxShadow.TargetIsRoot
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIApplyBoxShadowTargetIsRootTest,
    "MonolithUI.ApplyBoxShadow.TargetIsRoot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIApplyBoxShadowTargetIsRootTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::TestUtils;

    const FString WBPPath = TEXT("/Game/Tests/Monolith/UI/WBP_ShadowRootTest");
    FString FixtureError;
    if (!CreateOrReuseTestWidgetBlueprint(WBPPath, FName(TEXT("Target")), nullptr, FixtureError))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), WBPPath);
    Params->SetStringField(TEXT("widget_name"), TEXT("RootCanvas")); // root has no parent
    Params->SetStringField(TEXT("shadow_material_path"),
        TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
    {
        TSharedPtr<FJsonObject> Shadow = MakeShared<FJsonObject>();
        Shadow->SetStringField(TEXT("color"), TEXT("#000000"));
        Params->SetObjectField(TEXT("shadow"), Shadow);
    }
    Params->SetBoolField(TEXT("compile"), false);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("apply_box_shadow"), Params);

    TestFalse(TEXT("root-widget target -> failure"), Result.bSuccess);
    TestEqual(TEXT("root-widget target -> -32603"), Result.ErrorCode, -32603);
    TestTrue(TEXT("error message mentions 'no parent panel'"),
        Result.ErrorMessage.Contains(TEXT("no parent panel")));
    return true;
}

/**
 * MonolithUI.ApplyBoxShadow.InvalidParams
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIApplyBoxShadowInvalidParamsTest,
    "MonolithUI.ApplyBoxShadow.InvalidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIApplyBoxShadowInvalidParamsTest::RunTest(const FString& Parameters)
{
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("apply_box_shadow"), P);
        TestFalse(TEXT("empty params -> failure"), R.bSuccess);
        TestEqual(TEXT("empty params -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("asset_path"), TEXT("/Game/Foo/WBP_Bar"));
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("apply_box_shadow"), P);
        TestFalse(TEXT("missing widget_name -> failure"), R.bSuccess);
        TestEqual(TEXT("missing widget_name -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("asset_path"), TEXT("/Game/Foo/WBP_Bar"));
        P->SetStringField(TEXT("widget_name"), TEXT("Target"));
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("apply_box_shadow"), P);
        TestFalse(TEXT("missing shadow_material_path -> failure"), R.bSuccess);
        TestEqual(TEXT("missing shadow_material_path -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("asset_path"), TEXT("/Game/Foo/WBP_Bar"));
        P->SetStringField(TEXT("widget_name"), TEXT("Target"));
        P->SetStringField(TEXT("shadow_material_path"), TEXT("/Some/Mat"));
        TSharedPtr<FJsonObject> Single = MakeShared<FJsonObject>();
        Single->SetStringField(TEXT("color"), TEXT("#000"));
        P->SetObjectField(TEXT("shadow"), Single);
        TArray<TSharedPtr<FJsonValue>> Multi;
        TSharedPtr<FJsonObject> M0 = MakeShared<FJsonObject>();
        M0->SetStringField(TEXT("color"), TEXT("#FFF"));
        Multi.Add(MakeShared<FJsonValueObject>(M0));
        P->SetArrayField(TEXT("shadows"), Multi);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("apply_box_shadow"), P);
        TestFalse(TEXT("both shadow+shadows -> failure"), R.bSuccess);
        TestEqual(TEXT("both shadow+shadows -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("asset_path"), TEXT("/Game/Foo/WBP_Bar"));
        P->SetStringField(TEXT("widget_name"), TEXT("Target"));
        P->SetStringField(TEXT("shadow_material_path"), TEXT("/Some/Mat"));
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("apply_box_shadow"), P);
        TestFalse(TEXT("no shadow spec -> failure"), R.bSuccess);
        TestEqual(TEXT("no shadow spec -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("asset_path"), TEXT("/Game/Foo/WBP_Bar"));
        P->SetStringField(TEXT("widget_name"), TEXT("Target"));
        P->SetStringField(TEXT("shadow_material_path"), TEXT("/Some/Mat"));
        TSharedPtr<FJsonObject> Shadow = MakeShared<FJsonObject>();
        Shadow->SetNumberField(TEXT("x"), 0.0);
        Shadow->SetNumberField(TEXT("blur"), 4.0);
        P->SetObjectField(TEXT("shadow"), Shadow);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("apply_box_shadow"), P);
        TestFalse(TEXT("shadow missing color -> failure"), R.bSuccess);
        TestEqual(TEXT("shadow missing color -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("asset_path"), TEXT("/Game/Foo/WBP_Bar"));
        P->SetStringField(TEXT("widget_name"), TEXT("Target"));
        P->SetStringField(TEXT("shadow_material_path"), TEXT("/Some/Mat"));
        TArray<TSharedPtr<FJsonValue>> Empty;
        P->SetArrayField(TEXT("shadows"), Empty);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("apply_box_shadow"), P);
        TestFalse(TEXT("empty shadows array -> failure"), R.bSuccess);
        TestEqual(TEXT("empty shadows array -> -32602"), R.ErrorCode, -32602);
    }
    return true;
}

/**
 * MonolithUI.ApplyBoxShadow.IncompatibleParent
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIApplyBoxShadowIncompatibleParentTest,
    "MonolithUI.ApplyBoxShadow.IncompatibleParent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIApplyBoxShadowIncompatibleParentTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::TestUtils;

    const FString ProbePath = TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");
    UMaterialInterface* Probe = LoadObject<UMaterialInterface>(nullptr, *ProbePath);
    if (!Probe)
    {
        AddWarning(FString::Printf(
            TEXT("Skipping: probe engine material '%s' not loadable"), *ProbePath));
        return true;
    }

    const FString WBPPath = TEXT("/Game/Tests/Monolith/UI/WBP_ShadowIncompatTest");
    FString FixtureError;
    if (!CreateOrReuseTestWidgetBlueprint(WBPPath, FName(TEXT("Target")), nullptr, FixtureError))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), WBPPath);
    Params->SetStringField(TEXT("widget_name"), TEXT("Target"));
    Params->SetStringField(TEXT("shadow_material_path"), ProbePath);
    {
        TSharedPtr<FJsonObject> Shadow = MakeShared<FJsonObject>();
        Shadow->SetStringField(TEXT("color"), TEXT("#000"));
        Params->SetObjectField(TEXT("shadow"), Shadow);
    }
    Params->SetBoolField(TEXT("compile"), false);

    const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("apply_box_shadow"), Params);
    TestFalse(TEXT("incompatible parent -> failure"), R.bSuccess);
    TestEqual(TEXT("incompatible parent -> -32602"), R.ErrorCode, -32602);
    TestTrue(TEXT("error names ShadowColor or BlurRadius"),
        R.ErrorMessage.Contains(TEXT("ShadowColor")) || R.ErrorMessage.Contains(TEXT("BlurRadius")));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
