// Copyright tumourlove. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Runtime/Launch/Resources/Version.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"

/**
 * MonolithUI test-fixture helpers.
 *
 * Throwaway-WBP construction primitives used by the MonolithUI automation
 * suite. Tests drop disposable assets under /Game/Tests/Monolith/ per
 * feedback_test_assets_throwaway.md.
 *
 * The CleanupWidgetTree helper handles a quirk of in-editor test re-runs:
 * widgets accumulated from a prior pass remain parented to the WidgetTree
 * outer chain, which trips the WBP compiler's GUID-validation walk on the
 * next compile. Renaming each widget out to GetTransientPackage() makes
 * ForEachObjectWithOuter(WidgetTree) skip them.
 */
namespace MonolithUI::TestUtils
{
    /**
     * Purge all widgets from a Widget Blueprint's WidgetTree so that a subsequent
     * ConstructWidget pass starts clean. This prevents orphan-widget accumulation
     * across re-runs in the same editor session, which would otherwise trigger
     * GUID ensures in the WBP compiler's ForEachSourceWidget walk.
     */
    inline void CleanupWidgetTree(UWidgetBlueprint* WBP)
    {
        if (!WBP || !WBP->WidgetTree)
        {
            return;
        }

        UWidgetTree* Tree = WBP->WidgetTree;

        if (UPanelWidget* RootPanel = Cast<UPanelWidget>(Tree->RootWidget))
        {
            RootPanel->ClearChildren();
        }

        TArray<UWidget*> Orphans;
        auto CollectOrphan = [&Orphans](UObject* Obj)
        {
            if (UWidget* W = Cast<UWidget>(Obj))
            {
                Orphans.Add(W);
            }
        };

        // Direct children only -- nested objects are deliberately skipped. UE 5.8
        // replaced the bIncludeNestedObjects boolean with EGetObjectsFlags, which
        // UE 5.7 does not ship, so this call needs a version gate.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
        ForEachObjectWithOuter(Tree, CollectOrphan, EGetObjectsFlags::None);
#else
        ForEachObjectWithOuter(Tree, CollectOrphan, /*bIncludeNestedObjects=*/ false);
#endif

        // REN_ForceNoResetLoaders is omitted: Rename stopped calling ResetLoaders
        // before UE 5.7 (the flag is already a no-op there) and 5.8 deprecates it.
        // REN_AllowPackageLinkerMismatch is deliberately NOT substituted -- it would
        // suppress the linker detach these renames currently perform.
        for (UWidget* W : Orphans)
        {
            W->Rename(nullptr, GetTransientPackage(), REN_DoNotDirty | REN_DontCreateRedirectors);
        }

        Tree->RootWidget = nullptr;
        WBP->WidgetVariableNameToGuidMap.Empty();
    }

    /**
     * Create (or reclaim) a throwaway Widget Blueprint at the given asset path,
     * containing a CanvasPanel root and an optional named child widget.
     *
     * @param AssetPath         Long package name (e.g. "/Game/Tests/Monolith/UI/WBP_Foo")
     * @param ChildWidgetName   If not NAME_None, a child widget of ChildWidgetClass is added under the root.
     * @param ChildWidgetClass  Class for the child widget (default: UImage). Ignored if ChildWidgetName is NAME_None.
     * @param OutError          Receives human-readable error on failure.
     * @param OutChildWidget    (Optional) Receives the constructed child widget pointer so
     *                          callers can configure it (e.g. set canvas slot geometry).
     * @return true on success.
     */
    inline bool CreateOrReuseTestWidgetBlueprint(
        const FString& AssetPath,
        FName ChildWidgetName,
        UClass* ChildWidgetClass,
        FString& OutError,
        UWidget** OutChildWidget = nullptr)
    {
        if (!ChildWidgetClass)
        {
            ChildWidgetClass = UImage::StaticClass();
        }

        FString PackagePath, AssetName;
        if (!AssetPath.Split(TEXT("/"), &PackagePath, &AssetName,
            ESearchCase::IgnoreCase, ESearchDir::FromEnd) || AssetName.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Cannot split asset_path '%s'"), *AssetPath);
            return false;
        }

        UPackage* Package = CreatePackage(*AssetPath);
        if (!Package)
        {
            OutError = FString::Printf(TEXT("CreatePackage failed for '%s'"), *AssetPath);
            return false;
        }
        Package->FullyLoad();

        UWidgetBlueprint* WBP = FindObject<UWidgetBlueprint>(Package, *AssetName);
        if (!WBP)
        {
            UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
            Factory->BlueprintType = BPTYPE_Normal;
            Factory->ParentClass = UUserWidget::StaticClass();
            UObject* Created = Factory->FactoryCreateNew(
                UWidgetBlueprint::StaticClass(), Package, FName(*AssetName),
                RF_Public | RF_Standalone, nullptr, GWarn);
            WBP = Cast<UWidgetBlueprint>(Created);
        }
        if (!WBP || !WBP->WidgetTree)
        {
            OutError = TEXT("Failed to construct test WBP");
            return false;
        }

        CleanupWidgetTree(WBP);

        UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(
            UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
        WBP->WidgetTree->RootWidget = Root;

        if (ChildWidgetName != NAME_None)
        {
            UWidget* Child = WBP->WidgetTree->ConstructWidget<UWidget>(
                ChildWidgetClass, ChildWidgetName);
            if (!Child)
            {
                OutError = FString::Printf(TEXT("ConstructWidget failed for '%s'"), *ChildWidgetName.ToString());
                return false;
            }
            Root->AddChild(Child);

            if (OutChildWidget)
            {
                *OutChildWidget = Child;
            }
        }

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        FKismetEditorUtilities::CompileBlueprint(WBP);
        FAssetRegistryModule::AssetCreated(WBP);
        Package->MarkPackageDirty();

        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        const FString PackageFilename = FPackageName::LongPackageNameToFilename(
            AssetPath, FPackageName::GetAssetPackageExtension());
        if (!UPackage::SavePackage(Package, WBP, *PackageFilename, SaveArgs))
        {
            OutError = FString::Printf(TEXT("SavePackage failed for '%s'"), *PackageFilename);
            return false;
        }
        return true;
    }
} // namespace MonolithUI::TestUtils
