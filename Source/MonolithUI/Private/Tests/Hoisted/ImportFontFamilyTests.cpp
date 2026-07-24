// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

// Core / test
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

// JSON / registry
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithToolRegistry.h"

// Font verification
#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Fonts/CompositeFont.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

/**
 * MonolithUI.ImportFontFamily.Basic
 *
 * Dispatches `ui.import_font_family` with a single-face spec sourced from the
 * project's bundled Atkinson Hyperlegible TTF (SIL OFL). Asserts:
 *  - Action succeeds
 *  - family_asset_path / face_asset_paths / faces_imported returned
 *  - UFont composite asset exists in-memory at the reported path
 *  - Composite has DefaultTypeface.Fonts.Num() == 1 with Name == "Regular"
 *  - UFontFace exists at the reported face path
 *
 * Uses save=false -- assets stay in transient in-memory packages.
 *
 * Test-asset convention: /Game/Tests/Monolith/UI/Fonts/TestFamily/
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIImportFontFamilyBasicTest,
    "MonolithUI.ImportFontFamily.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIImportFontFamilyBasicTest::RunTest(const FString& Parameters)
{
    // Primary fixture: project-bundled Atkinson at Content/UI/Fonts/Atkinson/.
    const FString ProjectContentFixture =
        FPaths::ProjectContentDir() / TEXT("UI/Fonts/Atkinson/AtkinsonHyperlegible-Regular.ttf");

    FString FixturePath;
    if (FPaths::FileExists(ProjectContentFixture))
    {
        FixturePath = ProjectContentFixture;
    }
    else
    {
        AddWarning(FString::Printf(
            TEXT("TTF fixture not present at '%s' -- skipping. Orchestrator to seed fixture."),
            *ProjectContentFixture));
        return true;
    }

    const FString Destination = TEXT("/Game/Tests/Monolith/UI/Fonts/TestFamily");
    const FString FamilyName  = TEXT("TestFamily");

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("destination"), Destination);
    Params->SetStringField(TEXT("family_name"), FamilyName);
    {
        TArray<TSharedPtr<FJsonValue>> Faces;
        TSharedPtr<FJsonObject> FaceObj = MakeShared<FJsonObject>();
        FaceObj->SetStringField(TEXT("typeface"), TEXT("Regular"));
        FaceObj->SetStringField(TEXT("source_path"), FixturePath);
        Faces.Add(MakeShared<FJsonValueObject>(FaceObj));
        Params->SetArrayField(TEXT("faces"), Faces);
    }
    Params->SetStringField(TEXT("loading_policy"), TEXT("LazyLoad"));
    Params->SetStringField(TEXT("hinting"), TEXT("Default"));
    Params->SetBoolField(TEXT("save"), false);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("import_font_family"), Params);

    TestTrue(TEXT("import_font_family bSuccess"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        AddError(FString::Printf(TEXT("Action error: %s (code %d)"),
            *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }
    if (!Result.Result.IsValid())
    {
        AddError(TEXT("Result payload was null on success"));
        return false;
    }

    FString FamilyAssetPath;
    TestTrue(TEXT("family_asset_path in result"),
        Result.Result->TryGetStringField(TEXT("family_asset_path"), FamilyAssetPath));

    const TArray<TSharedPtr<FJsonValue>>* FacePaths = nullptr;
    TestTrue(TEXT("face_asset_paths in result"),
        Result.Result->TryGetArrayField(TEXT("face_asset_paths"), FacePaths));

    double FacesImported = 0.0;
    Result.Result->TryGetNumberField(TEXT("faces_imported"), FacesImported);
    TestEqual(TEXT("faces_imported == 1"), (int32)FacesImported, 1);
    if (!FacePaths || FacePaths->Num() != 1)
    {
        AddError(FString::Printf(TEXT("Expected 1 face path, got %d"),
            FacePaths ? FacePaths->Num() : 0));
        return false;
    }
    const FString FacePath = (*FacePaths)[0]->AsString();

    if (!FacePath.IsEmpty())
    {
        FString FaceLeaf;
        {
            int32 SlashIdx = INDEX_NONE;
            if (FacePath.FindLastChar(TEXT('/'), SlashIdx))
            {
                FaceLeaf = FacePath.Mid(SlashIdx + 1);
            }
        }
        const FString FaceDotted = FacePath + TEXT(".") + FaceLeaf;
        UFontFace* FaceObj = FindObject<UFontFace>(nullptr, *FaceDotted);
        TestNotNull(TEXT("UFontFace present at returned face_asset_paths[0]"), FaceObj);
    }

    if (!FamilyAssetPath.IsEmpty())
    {
        FString FamilyLeaf;
        {
            int32 SlashIdx = INDEX_NONE;
            if (FamilyAssetPath.FindLastChar(TEXT('/'), SlashIdx))
            {
                FamilyLeaf = FamilyAssetPath.Mid(SlashIdx + 1);
            }
        }
        const FString FamilyDotted = FamilyAssetPath + TEXT(".") + FamilyLeaf;
        UFont* FamilyObj = FindObject<UFont>(nullptr, *FamilyDotted);
        TestNotNull(TEXT("UFont present at returned family_asset_path"), FamilyObj);
        if (FamilyObj)
        {
            TestEqual(TEXT("UFont FontCacheType == Runtime"),
                (int32)FamilyObj->FontCacheType, (int32)EFontCacheType::Runtime);

            // UE 5.7: CompositeFont field is UE_DEPRECATED -- read via the accessor.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
            const FCompositeFont& Composite = FamilyObj->GetInternalCompositeFont();
#else
            const FCompositeFont& Composite = FamilyObj->CompositeFont;
#endif
            TestEqual(TEXT("DefaultTypeface.Fonts.Num() == 1"),
                Composite.DefaultTypeface.Fonts.Num(), 1);
            if (Composite.DefaultTypeface.Fonts.Num() == 1)
            {
                const FTypefaceEntry& Entry = Composite.DefaultTypeface.Fonts[0];
                TestEqual(TEXT("Typeface entry Name == 'Regular'"),
                    Entry.Name, FName(TEXT("Regular")));
                TestNotNull(TEXT("Typeface entry references a UFontFace asset"),
                    Entry.Font.GetFontFaceAsset());
            }
        }
    }

    return true;
}

/**
 * MonolithUI.ImportFontFamily.InvalidParams
 *
 * Negative-path coverage -- does NOT require any on-disk TTF.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIImportFontFamilyInvalidParamsTest,
    "MonolithUI.ImportFontFamily.InvalidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIImportFontFamilyInvalidParamsTest::RunTest(const FString& Parameters)
{
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("import_font_family"), P);
        TestFalse(TEXT("empty params -> failure"), R.bSuccess);
        TestEqual(TEXT("empty params -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("destination"), TEXT("/Game/Foo/Bar"));
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("import_font_family"), P);
        TestFalse(TEXT("missing family_name -> failure"), R.bSuccess);
        TestEqual(TEXT("missing family_name -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("destination"), TEXT("/Game/Foo/Bar"));
        P->SetStringField(TEXT("family_name"), TEXT("Fam"));
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("import_font_family"), P);
        TestFalse(TEXT("missing faces -> failure"), R.bSuccess);
        TestEqual(TEXT("missing faces -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("destination"), TEXT("/Game/Foo/Bar"));
        P->SetStringField(TEXT("family_name"), TEXT("Fam"));
        TArray<TSharedPtr<FJsonValue>> Empty;
        P->SetArrayField(TEXT("faces"), Empty);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("import_font_family"), P);
        TestFalse(TEXT("empty faces -> failure"), R.bSuccess);
        TestEqual(TEXT("empty faces -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("destination"), TEXT("/Game/Foo/Bar"));
        P->SetStringField(TEXT("family_name"), TEXT("Fam"));
        TArray<TSharedPtr<FJsonValue>> Faces;
        TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
        F->SetStringField(TEXT("typeface"), TEXT("Regular"));
        Faces.Add(MakeShared<FJsonValueObject>(F));
        P->SetArrayField(TEXT("faces"), Faces);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("import_font_family"), P);
        TestFalse(TEXT("face missing source_path -> failure"), R.bSuccess);
        TestEqual(TEXT("face missing source_path -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("destination"), TEXT("/Game/Foo/Bar"));
        P->SetStringField(TEXT("family_name"), TEXT("Fam"));
        TArray<TSharedPtr<FJsonValue>> Faces;
        for (int32 i = 0; i < 2; ++i)
        {
            TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
            F->SetStringField(TEXT("typeface"), TEXT("Regular"));
            F->SetStringField(TEXT("source_path"), TEXT("C:/nonexistent.ttf"));
            Faces.Add(MakeShared<FJsonValueObject>(F));
        }
        P->SetArrayField(TEXT("faces"), Faces);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("import_font_family"), P);
        TestFalse(TEXT("duplicate typeface -> failure"), R.bSuccess);
        TestEqual(TEXT("duplicate typeface -> -32602"), R.ErrorCode, -32602);
    }

    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("destination"),
            TEXT("/Game/Tests/Monolith/UI/Fonts/AllFailTest"));
        P->SetStringField(TEXT("family_name"), TEXT("AllFail"));
        TArray<TSharedPtr<FJsonValue>> Faces;
        TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
        F->SetStringField(TEXT("typeface"), TEXT("Regular"));
        F->SetStringField(TEXT("source_path"), TEXT("C:/definitely/does/not/exist/Example.ttf"));
        Faces.Add(MakeShared<FJsonValueObject>(F));
        P->SetArrayField(TEXT("faces"), Faces);
        P->SetBoolField(TEXT("save"), false);
        const FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("import_font_family"), P);
        TestFalse(TEXT("all faces missing -> failure"), R.bSuccess);
        TestEqual(TEXT("all faces missing -> -32603"), R.ErrorCode, -32603);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
