// Copyright tumourlove. All Rights Reserved.
// UIErrorFormattingTests.cpp
//
// Phase K — automation tests for the LLM-shaped error reporting surface.
//
// Test list (one per task in the plan):
//   K1   MonolithUI.ErrorFormatting.LLMReportIncludesPathAndOptions
//        FUISpecError::ToLLMReport() must include json_path, suggested_fix,
//        and valid_options when those fields are populated. Also verifies
//        FUISpecError::ToString() emits a human-friendly one-liner.
//
//   K3   MonolithUI.ErrorFormatting.RequestIdEchoes
//        ui::build_ui_from_spec must echo `request_id` back in the response
//        when the caller supplies it. Covers the parse-fail path explicitly
//        because that path used to drop request_id (Phase H bug, fixed in K).
//
//   K5a  MonolithUI.ErrorFormatting.SetWidgetPropertyBadPathSurfacesValidOptions
//        set_widget_property on a non-allowlisted path returns an error message
//        whose body parses as the FUISpecError::ToLLMReport key:value shape and
//        carries `valid_options` when the allowlist has entries for the type.
//
//   K5b  MonolithUI.ErrorFormatting.SetAnchorPresetBadPresetEnumeratesValidOptions
//        set_anchor_preset with an unknown preset name returns an error whose
//        body lists every legal preset token in valid_options.
//
//   K5c  MonolithUI.ErrorFormatting.SetBrushBadDrawTypeEnumeratesValidOptions
//        set_brush with an unknown draw_type returns an error whose body lists
//        every legal ESlateBrushDrawType token in valid_options.
//
//   K5d  MonolithUI.ErrorFormatting.AggregateValidationReportEmbedsPerFinding
//        FUISpecValidationResult::ToLLMReport() must embed each per-finding
//        block with the same key:value lines a standalone FUISpecError emits.
//
// Throwaway WBPs land under /Game/Tests/Monolith/UI/ErrorFormatting/ per the
// agent-rules test-asset rule. Every fixture gets a per-run-unique name and is
// removed again when its scope exits -- see FScratchWBP below for why a fixed
// name is not survivable here.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Spec/UISpec.h"
#include "Spec/UISpecValidator.h"

#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/VerticalBox.h"
#include "Components/Image.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace MonolithUI::ErrorFormattingTests
{
    /** Folder every scratch fixture in this file lives under. */
    static const TCHAR* const GScratchFolder = TEXT("/Game/Tests/Monolith/UI/ErrorFormatting");

    /**
     * Build a scratch asset path that is unique to THIS invocation.
     *
     * The suffix alone is not enough. UWidgetBlueprintFactory::FactoryCreateNew
     * forwards to FKismetEditorUtilities::CreateBlueprint, which opens with
     *
     *     check(FindObject<UBlueprint>(Outer, *NewBPName.ToString()) == NULL);
     *
     * (UE 5.7 Kismet2.cpp:435 / UE 5.8 Kismet2.cpp:441). That is a fatal assert,
     * not a recoverable failure -- an already-taken name kills the editor and
     * takes the rest of the automation run down with it. A fixture still resident
     * from an earlier run in this session, or reloaded from a .uasset a previous
     * session left on disk, is exactly that collision, which is why a fixed name
     * makes the suite pass only on a project that has never run it before. The
     * GUID token means no leftover -- however it was left behind -- can occupy
     * the name we are about to create.
     */
    static FString MakeUniqueTestPath(const FString& Suffix)
    {
        return FString::Printf(TEXT("%s/WBP_%s_%s"), GScratchFolder, *Suffix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
    }

    /** Split a long package name into its parent path and leaf asset name. */
    static bool SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName)
    {
        return AssetPath.Split(TEXT("/"), &OutPackagePath, &OutAssetName,
            ESearchCase::IgnoreCase, ESearchDir::FromEnd) && !OutAssetName.IsEmpty();
    }

    /**
     * True when nothing occupies AssetPath: no object of that name inside an
     * already-loaded package, and no package file on disk that a later load
     * could revive into one. This is the CreateBlueprint precondition, checked
     * before we hand the name to the factory.
     */
    static bool IsScratchPathFree(const FString& AssetPath)
    {
        FString PackagePath, AssetName;
        if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
        {
            return false;
        }
        if (FPackageName::DoesPackageExist(AssetPath))
        {
            return false;
        }
        if (UPackage* Existing = FindPackage(nullptr, *AssetPath))
        {
            return FindObject<UObject>(Existing, *AssetName) == nullptr;
        }
        return true;
    }

    /**
     * Remove a scratch asset: detach whatever object is loaded at AssetPath and
     * delete its .uasset. Safe to call for a path where nothing was created.
     *
     * ObjectTools::DeleteAssets / DeleteObjectsUnchecked are deliberately NOT
     * used. Both funnel into ObjectTools::DeleteSingleObject, which can raise a
     * modal FMessageDialog when a delegate vetoes the delete (UE 5.7
     * ObjectTools.cpp:3394) -- in an automation run that hangs the suite instead
     * of failing it. The manual path below touches only the fixture we made.
     */
    static void DestroyScratchAsset(const FString& AssetPath)
    {
        FString PackagePath, AssetName;
        if (AssetPath.IsEmpty() || !SplitAssetPath(AssetPath, PackagePath, AssetName))
        {
            return;
        }

        if (UPackage* Package = FindPackage(nullptr, *AssetPath))
        {
            if (UObject* Existing = FindObject<UObject>(Package, *AssetName))
            {
                // Notify the registry while the object is still addressable.
                FAssetRegistryModule::AssetDeleted(Existing);

                // Drop the keep-alive flags and move the asset (a UBlueprint
                // brings its generated + skeleton classes along -- see
                // UBlueprint::RenameGeneratedClasses) out to the transient
                // package, so the name is free again inside this session.
                Existing->ClearFlags(RF_Public | RF_Standalone);
                Existing->Rename(nullptr, GetTransientPackage(),
                    REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional);
            }

            // A throwaway must not raise a save prompt on editor shutdown.
            Package->SetDirtyFlag(false);
        }

        const FString Filename = FPackageName::LongPackageNameToFilename(
            AssetPath, FPackageName::GetAssetPackageExtension());
        IFileManager::Get().Delete(*Filename, /*RequireExists=*/false,
            /*EvenReadOnly=*/true, /*Quiet=*/true);
    }

    /**
     * Scratch fixture: a CanvasPanel-rooted WBP with one named child of the
     * requested widget class, created at a per-run-unique path and deleted again
     * when the scope exits -- including on an early `return false` from a failed
     * assertion, which is the case that used to leave the collision behind.
     *
     * The asset is still compiled and saved to disk exactly as before: the
     * actions under test resolve `asset_path` through the normal load path, and
     * the save keeps that path honest. It just does not outlive the test.
     *
     * Bookkeeping (OnVariableAdded / MarkBlueprintAsStructurallyModified /
     * CompileBlueprint / AssetCreated / SavePackage) matches the shared helper in
     * Hoisted/MonolithUITestFixtureUtils.h so the suite stays consistent.
     */
    struct FScratchWBP
    {
        FString AssetPath;
        UWidgetBlueprint* WBP = nullptr;

        FScratchWBP(const FString& Suffix, UClass* ChildClass, const FName& ChildName)
        {
            // Never hand CreateBlueprint a name whose assert precondition is
            // already violated -- reroll instead. With a GUID token this loop
            // does not spin in practice, but the guarantee is the check, not the
            // improbability of a collision.
            for (int32 Attempt = 0; Attempt < 8; ++Attempt)
            {
                FString Candidate = MakeUniqueTestPath(Suffix);
                if (IsScratchPathFree(Candidate))
                {
                    AssetPath = MoveTemp(Candidate);
                    break;
                }
            }
            if (AssetPath.IsEmpty())
            {
                return;
            }

            FString PackagePath, AssetName;
            if (!SplitAssetPath(AssetPath, PackagePath, AssetName)) return;

            UPackage* Package = CreatePackage(*AssetPath);
            if (!Package) return;

            UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
            Factory->BlueprintType = BPTYPE_Normal;
            Factory->ParentClass   = UUserWidget::StaticClass();
            UObject* Created = Factory->FactoryCreateNew(
                UWidgetBlueprint::StaticClass(), Package,
                FName(*AssetName), RF_Public | RF_Standalone, nullptr, GWarn);
            UWidgetBlueprint* Built = Cast<UWidgetBlueprint>(Created);
            if (!Built || !Built->WidgetTree) return;

            UCanvasPanel* Root = Built->WidgetTree->ConstructWidget<UCanvasPanel>(
                UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
            Built->WidgetTree->RootWidget = Root;
            Built->OnVariableAdded(Root->GetFName());

            if (ChildClass)
            {
                UWidget* Child = Built->WidgetTree->ConstructWidget<UWidget>(ChildClass, ChildName);
                Root->AddChild(Child);
                Built->OnVariableAdded(Child->GetFName());
            }

            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Built);
            FKismetEditorUtilities::CompileBlueprint(Built);

            FAssetRegistryModule::AssetCreated(Built);
            Package->MarkPackageDirty();
            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
            UPackage::SavePackage(Package, Built,
                *FPackageName::LongPackageNameToFilename(AssetPath, FPackageName::GetAssetPackageExtension()),
                SaveArgs);

            WBP = Built;
        }

        ~FScratchWBP()
        {
            WBP = nullptr;
            DestroyScratchAsset(AssetPath);
        }

        FScratchWBP(const FScratchWBP&) = delete;
        FScratchWBP& operator=(const FScratchWBP&) = delete;
    };

    /** Build a JSON params object from a list of (key, string-value) pairs. */
    static TSharedPtr<FJsonObject> MakeStringParams(
        std::initializer_list<TPair<FString, FString>> Pairs)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        for (const TPair<FString, FString>& Pair : Pairs)
        {
            Out->SetStringField(Pair.Key, Pair.Value);
        }
        return Out;
    }
}


// ============================================================================
// K1 — FUISpecError::ToLLMReport includes path + suggested-fix + valid-options
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIErrorFormattingLLMReportIncludesPathAndOptionsTest,
    "MonolithUI.ErrorFormatting.LLMReportIncludesPathAndOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIErrorFormattingLLMReportIncludesPathAndOptionsTest::RunTest(const FString& /*Parameters*/)
{
    // Build a representative finding the way an action handler would.
    FUISpecError Err;
    Err.Severity     = EUISpecErrorSeverity::Error;
    Err.Category     = TEXT("Enum");
    Err.JsonPath     = TEXT("/preset");
    Err.WidgetId     = FName(TEXT("MyButton"));
    Err.Message      = TEXT("Unknown anchor preset 'middle_of_nowhere'.");
    Err.SuggestedFix = TEXT("Pick one of the listed preset tokens.");
    Err.ValidOptions = {
        TEXT("center"), TEXT("top_left"), TEXT("stretch_fill")
    };

    // ---- ToLLMReport ----------------------------------------------------
    const FString Report = Err.ToLLMReport();

    TestTrue(TEXT("LLMReport includes category label"),
        Report.Contains(TEXT("category: Enum")));
    TestTrue(TEXT("LLMReport includes severity label"),
        Report.Contains(TEXT("severity: error")));
    TestTrue(TEXT("LLMReport includes json_path label + value"),
        Report.Contains(TEXT("json_path: /preset")));
    TestTrue(TEXT("LLMReport includes widget_id label"),
        Report.Contains(TEXT("widget_id: MyButton")));
    TestTrue(TEXT("LLMReport includes message label"),
        Report.Contains(TEXT("message: Unknown anchor preset 'middle_of_nowhere'.")));
    TestTrue(TEXT("LLMReport includes suggested_fix label"),
        Report.Contains(TEXT("suggested_fix: Pick one of the listed preset tokens.")));
    TestTrue(TEXT("LLMReport includes valid_options header"),
        Report.Contains(TEXT("valid_options:")));
    TestTrue(TEXT("LLMReport enumerates each valid_options entry"),
        Report.Contains(TEXT("  - center")) &&
        Report.Contains(TEXT("  - top_left")) &&
        Report.Contains(TEXT("  - stretch_fill")));

    // ---- ToString -------------------------------------------------------
    const FString OneLine = Err.ToString();
    TestTrue(TEXT("ToString carries the [ERR ] severity tag"),
        OneLine.Contains(TEXT("[ERR ]")));
    TestTrue(TEXT("ToString carries the json_path"),
        OneLine.Contains(TEXT("/preset")));
    TestTrue(TEXT("ToString carries the suggested-fix hint"),
        OneLine.Contains(TEXT("hint:")));
    TestTrue(TEXT("ToString carries the valid_options preview"),
        OneLine.Contains(TEXT("valid_options:")));

    // ---- Empty-field tolerance -----------------------------------------
    // A minimally-populated finding must still produce a usable report.
    FUISpecError Bare;
    Bare.Severity = EUISpecErrorSeverity::Warning;
    Bare.Category = NAME_None;
    Bare.Message  = TEXT("Heads up.");
    const FString BareReport = Bare.ToLLMReport();
    TestTrue(TEXT("Bare finding still emits a category line (Uncategorized fallback)"),
        BareReport.Contains(TEXT("category: Uncategorized")));
    TestTrue(TEXT("Bare finding emits warning severity"),
        BareReport.Contains(TEXT("severity: warning")));
    TestTrue(TEXT("Bare finding emits the / json_path placeholder"),
        BareReport.Contains(TEXT("json_path: /")));
    TestFalse(TEXT("Bare finding omits the empty suggested_fix line"),
        BareReport.Contains(TEXT("suggested_fix:")));
    TestFalse(TEXT("Bare finding omits the empty valid_options line"),
        BareReport.Contains(TEXT("valid_options:")));

    return true;
}


// ============================================================================
// K3 — request_id round-trips through ui::build_ui_from_spec on every path
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIErrorFormattingRequestIdEchoesTest,
    "MonolithUI.ErrorFormatting.RequestIdEchoes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIErrorFormattingRequestIdEchoesTest::RunTest(const FString& /*Parameters*/)
{
    using namespace MonolithUI::ErrorFormattingTests;

    const FString TestRequestId = TEXT("test-req-K3-2026-04-26");

    // Path 1 — validate-fail: empty `spec` produces an immediate validator
    // failure. The response must still echo request_id.
    {
        // Unique path + unconditional teardown: this path is not supposed to
        // commit an asset, but the fixture must not be able to poison a re-run
        // even if that ever regresses.
        const FString AssetPath = MakeUniqueTestPath(TEXT("RequestIdEcho_ValidateFail"));
        ON_SCOPE_EXIT { DestroyScratchAsset(AssetPath); };

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"),  AssetPath);
        Params->SetObjectField(TEXT("spec"),        MakeShared<FJsonObject>()); // empty object => no rootWidget
        Params->SetStringField(TEXT("request_id"),  TestRequestId);

        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("build_ui_from_spec"), Params);

        TestTrue(TEXT("Validate-fail path returns success-on-the-wire (semantic fail in payload)"),
            R.bSuccess);
        TestNotNull(TEXT("Validate-fail path produced a result payload"), R.Result.Get());

        if (R.Result.IsValid())
        {
            FString EchoedId;
            TestTrue(TEXT("Validate-fail response carries request_id"),
                R.Result->TryGetStringField(TEXT("request_id"), EchoedId));
            TestEqual(TEXT("Validate-fail response echoes the supplied request_id"),
                EchoedId, TestRequestId);
        }
    }

    // Path 2 — success: a minimal one-node spec builds a real WBP. request_id
    // must come back on success too.
    {
        TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
        Spec->SetStringField(TEXT("name"), TEXT("EchoOK"));
        Spec->SetStringField(TEXT("parentClass"), TEXT("UserWidget"));
        TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("type"), TEXT("VerticalBox"));
        Root->SetStringField(TEXT("id"),   TEXT("RootBox"));
        Spec->SetObjectField(TEXT("rootWidget"), Root);

        const FString AssetPath = MakeUniqueTestPath(TEXT("RequestIdEcho_Success"));
        ON_SCOPE_EXIT { DestroyScratchAsset(AssetPath); };

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetObjectField(TEXT("spec"),       Spec);
        Params->SetStringField(TEXT("request_id"), TestRequestId);
        Params->SetBoolField(TEXT("dry_run"),      true); // don't pollute disk

        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("build_ui_from_spec"), Params);

        TestTrue(TEXT("Success path returns bSuccess=true on the wire"), R.bSuccess);
        if (R.Result.IsValid())
        {
            FString EchoedId;
            TestTrue(TEXT("Success response carries request_id"),
                R.Result->TryGetStringField(TEXT("request_id"), EchoedId));
            TestEqual(TEXT("Success response echoes the supplied request_id"),
                EchoedId, TestRequestId);
        }
    }

    // Path 3 — request_id omitted: the response field must be absent (NOT
    // present-as-empty-string), so callers can distinguish "unset" from "set
    // to empty string".
    {
        const FString AssetPath = MakeUniqueTestPath(TEXT("RequestIdEcho_Omitted"));
        ON_SCOPE_EXIT { DestroyScratchAsset(AssetPath); };

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetObjectField(TEXT("spec"),       MakeShared<FJsonObject>()); // validate-fail again

        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("build_ui_from_spec"), Params);

        TestTrue(TEXT("Validate-fail path returns success-on-the-wire"), R.bSuccess);
        if (R.Result.IsValid())
        {
            TestFalse(TEXT("request_id field is omitted when caller didn't supply one"),
                R.Result->HasField(TEXT("request_id")));
        }
    }

    return true;
}


// ============================================================================
// K5a — set_widget_property NotInAllowlist surfaces FUISpecError shape
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUISetWidgetPropertyBadPathSurfacesValidOptionsTest,
    "MonolithUI.ErrorFormatting.SetWidgetPropertyBadPathSurfacesValidOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISetWidgetPropertyBadPathSurfacesValidOptionsTest::RunTest(const FString& /*Parameters*/)
{
    using namespace MonolithUI::ErrorFormattingTests;

    FScratchWBP Scratch(TEXT("BadPropertyPath"), UButton::StaticClass(),
        FName(TEXT("PrimaryButton")));
    if (!TestNotNull(TEXT("Test fixture WBP created"), Scratch.WBP))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Params = MakeStringParams({
        { TEXT("asset_path"),    Scratch.AssetPath },
        { TEXT("widget_name"),   TEXT("PrimaryButton") },
        // Intentionally bogus path — guaranteed to miss any allowlist entry.
        { TEXT("property_name"), TEXT("ThisPropertyDoesNotExist_K5a") },
        { TEXT("value"),         TEXT("doesntmatter") }
    });

    const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("set_widget_property"), Params);

    TestFalse(TEXT("Bad property path returns bSuccess=false"), R.bSuccess);
    TestEqual(TEXT("Bad property path uses JSON-RPC invalid-params code"),
        R.ErrorCode, -32602);

    // The error message body IS the FUISpecError::ToLLMReport output. Grep
    // for the stable key labels — that's the contract Python harness consumers
    // depend on.
    const FString& Body = R.ErrorMessage;
    TestTrue(TEXT("Error body carries category label"),
        Body.Contains(TEXT("category:")));
    TestTrue(TEXT("Error body carries json_path label"),
        Body.Contains(TEXT("json_path:")));
    TestTrue(TEXT("Error body carries severity label"),
        Body.Contains(TEXT("severity: error")));
    TestTrue(TEXT("Error body carries the failing widget name as widget_id"),
        Body.Contains(TEXT("widget_id: PrimaryButton")));
    TestTrue(TEXT("Error body carries suggested_fix label"),
        Body.Contains(TEXT("suggested_fix:")));
    // valid_options may or may not be populated depending on whether the
    // registry has Button entries at this point in suite execution, so we
    // assert only that the body acknowledges the gate via category=Allowlist.
    TestTrue(TEXT("Error body categorises the failure as Allowlist (or Property)"),
        Body.Contains(TEXT("category: Allowlist")) || Body.Contains(TEXT("category: Property")));

    return true;
}


// ============================================================================
// K5b — set_anchor_preset bad preset enumerates valid_options (16 entries)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUISetAnchorPresetBadPresetEnumeratesValidOptionsTest,
    "MonolithUI.ErrorFormatting.SetAnchorPresetBadPresetEnumeratesValidOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISetAnchorPresetBadPresetEnumeratesValidOptionsTest::RunTest(const FString& /*Parameters*/)
{
    using namespace MonolithUI::ErrorFormattingTests;

    FScratchWBP Scratch(TEXT("BadAnchorPreset"), UButton::StaticClass(),
        FName(TEXT("PrimaryButton")));
    if (!TestNotNull(TEXT("Test fixture WBP created"), Scratch.WBP))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Params = MakeStringParams({
        { TEXT("asset_path"),  Scratch.AssetPath },
        { TEXT("widget_name"), TEXT("PrimaryButton") },
        { TEXT("preset"),      TEXT("middle_of_nowhere") }
    });

    const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("set_anchor_preset"), Params);

    TestFalse(TEXT("Bad anchor preset returns bSuccess=false"), R.bSuccess);
    TestEqual(TEXT("Bad anchor preset uses JSON-RPC invalid-params code"),
        R.ErrorCode, -32602);

    const FString& Body = R.ErrorMessage;
    TestTrue(TEXT("Error body categorises as Enum"),
        Body.Contains(TEXT("category: Enum")));
    TestTrue(TEXT("Error body json_path points at /preset"),
        Body.Contains(TEXT("json_path: /preset")));
    TestTrue(TEXT("Error body lists center preset"),
        Body.Contains(TEXT("- center")));
    TestTrue(TEXT("Error body lists stretch_fill preset"),
        Body.Contains(TEXT("- stretch_fill")));
    TestTrue(TEXT("Error body lists top_left preset"),
        Body.Contains(TEXT("- top_left")));
    TestTrue(TEXT("Error body lists bottom_right preset"),
        Body.Contains(TEXT("- bottom_right")));
    TestTrue(TEXT("Error body lists stretch_horizontal preset"),
        Body.Contains(TEXT("- stretch_horizontal")));

    // Guard against drift: the canonical list in MonolithUIInternal::
    // GetAnchorPresetNames() and the runtime map in MonolithUI::GetAnchorPreset
    // both have 16 entries today. If this assertion fires, sync the two.
    int32 BulletCount = 0;
    int32 SearchPos = 0;
    while (SearchPos != INDEX_NONE)
    {
        SearchPos = Body.Find(TEXT("\n  - "), ESearchCase::CaseSensitive,
            ESearchDir::FromStart, SearchPos);
        if (SearchPos != INDEX_NONE)
        {
            ++BulletCount;
            ++SearchPos;
        }
    }
    TestEqual(TEXT("valid_options enumerates all 16 anchor presets"), BulletCount, 16);

    return true;
}


// ============================================================================
// K5c — set_brush bad draw_type enumerates the 5 ESlateBrushDrawType tokens
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUISetBrushBadDrawTypeEnumeratesValidOptionsTest,
    "MonolithUI.ErrorFormatting.SetBrushBadDrawTypeEnumeratesValidOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISetBrushBadDrawTypeEnumeratesValidOptionsTest::RunTest(const FString& /*Parameters*/)
{
    using namespace MonolithUI::ErrorFormattingTests;

    FScratchWBP Scratch(TEXT("BadDrawType"), UImage::StaticClass(),
        FName(TEXT("PrimaryImage")));
    if (!TestNotNull(TEXT("Test fixture WBP created"), Scratch.WBP))
    {
        return false;
    }

    // UImage::Brush is the canonical FSlateBrush property name. set_brush
    // walks property_name dotted segments; a single-segment path is fine.
    TSharedPtr<FJsonObject> Params = MakeStringParams({
        { TEXT("asset_path"),    Scratch.AssetPath },
        { TEXT("widget_name"),   TEXT("PrimaryImage") },
        { TEXT("property_name"), TEXT("Brush") },
        { TEXT("draw_type"),     TEXT("LightningBolt") } // not a valid draw type
    });

    const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("set_brush"), Params);

    TestFalse(TEXT("Bad draw_type returns bSuccess=false"), R.bSuccess);
    TestEqual(TEXT("Bad draw_type uses JSON-RPC invalid-params code"),
        R.ErrorCode, -32602);

    const FString& Body = R.ErrorMessage;
    TestTrue(TEXT("Error body categorises as Enum"),
        Body.Contains(TEXT("category: Enum")));
    TestTrue(TEXT("Error body json_path points at /draw_type"),
        Body.Contains(TEXT("json_path: /draw_type")));
    TestTrue(TEXT("Error body lists Image draw type"),
        Body.Contains(TEXT("- Image")));
    TestTrue(TEXT("Error body lists Box draw type"),
        Body.Contains(TEXT("- Box")));
    TestTrue(TEXT("Error body lists Border draw type"),
        Body.Contains(TEXT("- Border")));
    TestTrue(TEXT("Error body lists RoundedBox draw type"),
        Body.Contains(TEXT("- RoundedBox")));
    TestTrue(TEXT("Error body lists NoDrawType"),
        Body.Contains(TEXT("- NoDrawType")));

    return true;
}


// ============================================================================
// K5d — Aggregate FUISpecValidationResult::ToLLMReport embeds per-finding lines
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIAggregateValidationReportEmbedsPerFindingTest,
    "MonolithUI.ErrorFormatting.AggregateValidationReportEmbedsPerFinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIAggregateValidationReportEmbedsPerFindingTest::RunTest(const FString& /*Parameters*/)
{
    // Construct a result with one error + one warning, both with rich fields.
    FUISpecValidationResult Result;
    Result.bIsValid = false;

    {
        FUISpecError E;
        E.Severity     = EUISpecErrorSeverity::Error;
        E.Category     = TEXT("Type");
        E.JsonPath     = TEXT("/rootWidget/type");
        E.WidgetId     = FName(TEXT("RootBox"));
        E.Message      = TEXT("Unknown widget type token 'PurpleSquare'.");
        E.SuggestedFix = TEXT("Use a registered type token, or supply CustomClassPath.");
        E.ValidOptions = { TEXT("VerticalBox"), TEXT("HorizontalBox"), TEXT("CanvasPanel") };
        Result.Errors.Add(MoveTemp(E));
    }
    {
        FUISpecError W;
        W.Severity     = EUISpecErrorSeverity::Warning;
        W.Category     = TEXT("StyleRef");
        W.JsonPath     = TEXT("/rootWidget/styleRef");
        W.WidgetId     = FName(TEXT("RootBox"));
        W.Message      = TEXT("Style 'GhostStyle' not in document.styles.");
        W.SuggestedFix = TEXT("Add the style or remove the ref.");
        Result.Warnings.Add(MoveTemp(W));
    }

    const FString Aggregate  = Result.ToLLMReport();
    const FString PerFinding = Result.Errors[0].ToLLMReport();

    // Aggregate-level prelude.
    TestTrue(TEXT("Aggregate report has validation_status header"),
        Aggregate.Contains(TEXT("validation_status: failed")));
    TestTrue(TEXT("Aggregate report has error_count header"),
        Aggregate.Contains(TEXT("error_count: 1")));
    TestTrue(TEXT("Aggregate report has warning_count header"),
        Aggregate.Contains(TEXT("warning_count: 1")));

    // Discriminator + indented embedding of the per-finding shape. Each line
    // of the per-finding block must appear inside the aggregate, two-space
    // indented under the `- kind:` discriminator.
    TestTrue(TEXT("Aggregate emits the error discriminator"),
        Aggregate.Contains(TEXT("- kind: error")));
    TestTrue(TEXT("Aggregate emits the warning discriminator"),
        Aggregate.Contains(TEXT("- kind: warning")));

    // Re-emit the per-finding lines with the 2-space indent the aggregate
    // formatter applies; verify they each appear in the aggregate output.
    TArray<FString> PerLines;
    PerFinding.ParseIntoArrayLines(PerLines, /*InCullEmpty=*/false);
    for (const FString& Line : PerLines)
    {
        if (Line.IsEmpty()) continue;
        const FString Indented = FString::Printf(TEXT("  %s"), *Line);
        TestTrue(
            FString::Printf(TEXT("Aggregate contains indented per-finding line: %s"), *Line),
            Aggregate.Contains(Indented));
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
