// MonolithCommonUITemplateActions.cpp
//
// Phase 3 (2026-05-16 UI gap audit) — Tier 3 Headline Scaffolders.
//
// Four one-shot scaffolders that compose Phase 1 + Phase 2 primitives into
// headline workflows the LLM (or any caller) can invoke as a single MCP call:
//
//   * scaffold_main_menu               (Item #15)
//   * scaffold_settings_panel_with_tabs (Item #16)
//   * scaffold_pause_menu              (Item #17)
//
// All three follow the same pipeline shape:
//   1. CreatePackage at save_path  (UE 5.7 gotcha — returns existing if loaded).
//   2. Use UWidgetBlueprintFactory to construct a fresh UWidgetBlueprint with
//      parent = UCommonActivatableWidget (or UTokenforgeActivatableWidget when
//      the Tokenforge plugin is enabled — Phase 2 Item #10 probe pattern).
//   3. Build the WidgetTree: root container + title + content + per-screen
//      composition (button list / tab list / etc.).
//   4. Add a UCommonBoundActionBar with ActionButtonClass set via the Phase 1
//      Bug #4 default-resolve path (FClassProperty reflection write — the
//      property is private on UCommonBoundActionBar).
//   5. Stamp DesiredFocusTargetName UPROPERTY on the WBP CDO (creating it via
//      Phase 2 Item #8 add_widget_variable internals when it doesn't exist).
//   6. Wire navigation rules between the focusable widgets.
//   7. Compile via FKismetEditorUtilities::CompileBlueprint and capture
//      FCompilerResultsLog (errors / warnings).
//   8. Return a manifest {wbp_path, widgets_created[], compile_status,
//      errors[], warnings[]}.
//
// File is .cpp-only: handlers live in the anonymous namespace, the single
// extern entry point `Register(FMonolithToolRegistry&)` is the only symbol
// visible to MonolithCommonUIActionsAggregator.cpp. WITH_COMMONUI-gated so
// the whole file compiles to an empty TU when CommonUI is absent.

#include "MonolithCommonUIHelpers.h"

#if WITH_COMMONUI

#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

// CommonUI types — verified present from Phase 1 + Phase 2 includes.
#include "CommonActivatableWidget.h"
#include "CommonButtonBase.h"
#include "CommonTextBlock.h"
#include "CommonTabListWidgetBase.h"
#include "CommonAnimatedSwitcher.h"
#include "Input/CommonBoundActionBar.h"

// UMG containers + leaves.
#include "Components/Overlay.h"
#include "Components/VerticalBox.h"
#include "Components/Border.h"
#include "Components/PanelWidget.h"

// Blueprint + reflection plumbing.
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Logging/TokenizedMessage.h"
#include "EdGraphSchema_K2.h"
#include "AssetRegistry/AssetRegistryModule.h"

// Package + save plumbing.
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

// Tokenforge plugin probe (Phase 2 Item #10 pattern). Projects module already
// listed in MonolithUI.Build.cs PrivateDependencyModuleNames.
#include "Interfaces/IPluginManager.h"

// Navigation rule plumbing (Phase 2 Item #9 pattern + 3.D.1).
// EUINavigation enum is forward-declared in Input/NavigationReply.h; the full
// definition lands via Types/NavigationMetaData.h, which the
// MonolithCommonUINavigationActions.cpp precedent (line 19) uses.
#include "Blueprint/WidgetNavigation.h"
#include "Types/NavigationMetaData.h"
#include "Types/SlateEnums.h"

namespace MonolithCommonUITemplate
{
    // ----- Shared: default UCommonButtonBase path ------------------------------
    //
    // Mirrors the Phase 1 Bug #4 contract from create_bound_action_bar
    // (MonolithCommonUIInputActions.cpp:199). Action-bar + per-button defaults
    // both resolve to the same Monolith stock button so a fresh scaffold
    // compiles without manual class-assignment.
    static const TCHAR* DefaultActionButtonClassPath =
        TEXT("/Game/Monolith/CommonUI/MonolithDefaultCommonButton.MonolithDefaultCommonButton_C");

    // ----- Shared: Tokenforge probe -------------------------------------------
    //
    // Returns UTokenforgeActivatableWidget when the TokenforgeRuntime plugin
    // is present + enabled AND the class is resolvable; else
    // UCommonActivatableWidget. Matches the Phase 2 Item #10 probe pattern
    // (MonolithCommonUIButtonActions.cpp:783-786).
    static UClass* ResolveActivatableParentClass(bool& bOutUsedTokenforge)
    {
        bOutUsedTokenforge = false;
        TSharedPtr<IPlugin> TokenforgePlugin =
            IPluginManager::Get().FindPlugin(TEXT("TokenforgeRuntime"));
        if (TokenforgePlugin.IsValid() && TokenforgePlugin->IsEnabled())
        {
            // Try without then with the engine 'U' prefix — the
            // MonolithCommonUIActivatableActions.cpp:78-80 precedent does the
            // same dance so reflection-resolved class lookups work regardless
            // of which naming convention the search string uses.
            UClass* TFClass = FindFirstObject<UClass>(
                TEXT("TokenforgeActivatableWidget"),
                EFindFirstObjectOptions::NativeFirst);
            if (!TFClass)
            {
                TFClass = FindFirstObject<UClass>(
                    TEXT("UTokenforgeActivatableWidget"),
                    EFindFirstObjectOptions::NativeFirst);
            }
            if (TFClass && TFClass->IsChildOf(UCommonActivatableWidget::StaticClass()))
            {
                bOutUsedTokenforge = true;
                return TFClass;
            }
        }
        return UCommonActivatableWidget::StaticClass();
    }

    // ----- Shared: parent-class override (caller-supplied 'parent_class') -----
    //
    // Caller can pass parent_class="/Game/SomePath.SomePath_C" or a bare class
    // short name; we validate it derives from UCommonActivatableWidget before
    // accepting. On any failure we fall through to ResolveActivatableParentClass
    // so the scaffold never aborts on a soft override.
    static UClass* ResolveParentClass(
        const TSharedPtr<FJsonObject>& Params,
        bool& bOutUsedTokenforge,
        FString& OutResolvedName)
    {
        FString OverridePath;
        if (Params.IsValid() && Params->TryGetStringField(TEXT("parent_class"), OverridePath)
            && !OverridePath.IsEmpty())
        {
            UClass* Resolved = LoadClass<UCommonActivatableWidget>(nullptr, *OverridePath);
            if (!Resolved)
            {
                // Try the bare-class-name fallback (Phase 1 convention).
                Resolved = FindFirstObject<UClass>(*OverridePath, EFindFirstObjectOptions::NativeFirst);
            }
            if (Resolved && Resolved->IsChildOf(UCommonActivatableWidget::StaticClass()))
            {
                bOutUsedTokenforge = false;
                OutResolvedName = Resolved->GetName();
                return Resolved;
            }
        }
        UClass* Auto = ResolveActivatableParentClass(bOutUsedTokenforge);
        OutResolvedName = Auto->GetName();
        return Auto;
    }

    // ----- Shared: resolve action bar button class ----------------------------
    static UClass* ResolveActionBarButtonClass(
        const TSharedPtr<FJsonObject>& Params,
        FString& OutPath,
        bool& bOutWasDefault)
    {
        FString ButtonClassPath;
        const bool bExplicit = Params.IsValid()
            && Params->TryGetStringField(TEXT("action_button_class"), ButtonClassPath)
            && !ButtonClassPath.IsEmpty();
        if (!bExplicit)
        {
            ButtonClassPath = DefaultActionButtonClassPath;
        }
        UClass* Resolved = LoadClass<UCommonButtonBase>(nullptr, *ButtonClassPath);
        if (!Resolved || !Resolved->IsChildOf(UCommonButtonBase::StaticClass()))
        {
            OutPath = ButtonClassPath;
            bOutWasDefault = !bExplicit;
            return nullptr;
        }
        OutPath = ButtonClassPath;
        bOutWasDefault = !bExplicit;
        return Resolved;
    }

    // ----- Shared: SavePath -> (PackagePath, AssetName) ------------------------
    static bool SplitSavePath(const FString& SavePath, FString& OutPackagePath, FString& OutAssetName)
    {
        return SavePath.Split(TEXT("/"), &OutPackagePath, &OutAssetName,
            ESearchCase::IgnoreCase, ESearchDir::FromEnd);
    }

    // ----- Shared: construct the empty UWidgetBlueprint ------------------------
    //
    // Mirrors the create_activatable_widget flow (MonolithCommonUIActivatableActions.cpp:55-104):
    // CreatePackage -> reject if duplicate -> WidgetBlueprintFactory ->
    // FactoryCreateNew. Caller owns subsequent WidgetTree mutation + compile +
    // save.
    static FMonolithActionResult CreateBlankActivatableWBP(
        const FString& SavePath,
        UClass* ParentClass,
        UWidgetBlueprint*& OutWbp,
        UPackage*& OutPackage,
        FString& OutAssetName)
    {
        if (!ParentClass)
        {
            return FMonolithActionResult::Error(TEXT("ParentClass null"));
        }
        FString PackagePath;
        if (!SplitSavePath(SavePath, PackagePath, OutAssetName))
        {
            return FMonolithActionResult::Error(TEXT("save_path must contain at least one / separator"));
        }
        OutPackage = CreatePackage(*SavePath);
        if (!OutPackage)
        {
            return FMonolithActionResult::Error(FString::Printf(
                TEXT("CreatePackage failed for '%s'"), *SavePath));
        }
        // UE 5.7 gotcha: CreatePackage returns an EXISTING in-memory package
        // if one is loaded. Reject overwrite collisions here so the scaffolder
        // refuses to clobber an asset the caller forgot about.
        if (FindObject<UObject>(OutPackage, *OutAssetName))
        {
            return FMonolithActionResult::Error(FString::Printf(
                TEXT("Asset already exists at '%s' — pass a fresh save_path or delete it first"),
                *SavePath));
        }

        UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
        Factory->BlueprintType = BPTYPE_Normal;
        Factory->ParentClass = ParentClass;
        UObject* Created = Factory->FactoryCreateNew(
            UWidgetBlueprint::StaticClass(), OutPackage,
            FName(*OutAssetName), RF_Public | RF_Standalone,
            nullptr, GWarn);
        OutWbp = Cast<UWidgetBlueprint>(Created);
        if (!OutWbp)
        {
            return FMonolithActionResult::Error(TEXT("UWidgetBlueprintFactory returned null"));
        }
        return FMonolithActionResult::Success(MakeShared<FJsonObject>());
    }

    // ----- Shared: stamp DesiredFocusTargetName on the WBP --------------------
    //
    // The set_initial_focus_target action (MonolithCommonUINavigationActions.cpp:124-183)
    // documents the contract: WBP must expose `DesiredFocusTargetName` (or
    // `InitialFocusTargetName`) as an FName UPROPERTY, and the CommonActivatable
    // subclass overrides NativeGetDesiredFocusTarget to look the widget up
    // by that name.
    //
    // The scaffolder pre-creates the variable when the parent class doesn't
    // already expose it — same code path as Phase 2 Item #8 add_widget_variable
    // (MonolithUIRegistryActions.cpp:248). Caller-side override does not need
    // to recompile (we compile once at the end of the scaffold).
    static bool EnsureDesiredFocusTargetVariable(UWidgetBlueprint* Wbp)
    {
        if (!Wbp || !Wbp->GeneratedClass) return false;

        // Already exposed (e.g. UTokenforgeActivatableWidget ships it natively).
        if (FindFProperty<FNameProperty>(Wbp->GeneratedClass, TEXT("DesiredFocusTargetName"))
            || FindFProperty<FNameProperty>(Wbp->GeneratedClass, TEXT("InitialFocusTargetName")))
        {
            return true;
        }

        FEdGraphPinType PinType;
        PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
        return FBlueprintEditorUtils::AddMemberVariable(
            Wbp, TEXT("DesiredFocusTargetName"), PinType, FString());
    }

    // Writes the resolved focus-target FName onto the WBP CDO.
    static bool WriteDesiredFocusTargetName(UWidgetBlueprint* Wbp, FName Target)
    {
        if (!Wbp || !Wbp->GeneratedClass) return false;
        UObject* CDO = Wbp->GeneratedClass->GetDefaultObject();
        if (!CDO) return false;
        const FName PropNames[] = { TEXT("DesiredFocusTargetName"), TEXT("InitialFocusTargetName") };
        for (const FName& PN : PropNames)
        {
            if (FNameProperty* P = FindFProperty<FNameProperty>(Wbp->GeneratedClass, PN))
            {
                P->SetPropertyValue_InContainer(CDO, Target);
                return true;
            }
        }
        return false;
    }

    // ----- Shared: stamp ActionButtonClass on a UCommonBoundActionBar ---------
    //
    // Phase 1 Bug #4 + Phase 2 Item #13 archetype-write contract. The
    // ActionButtonClass UPROPERTY is private on UCommonBoundActionBar
    // (CommonBoundActionBar.h:67-68), assignment via reflection writes
    // through BOTH the authoring tree (the bar we just constructed) AND the
    // WBP's generated-class archetype tree so the value survives recompile.
    static void StampActionBarButtonClass(
        UWidgetBlueprint* Wbp,
        UCommonBoundActionBar* Bar,
        UClass* ButtonClass)
    {
        if (!Wbp || !Bar || !ButtonClass) return;
        FClassProperty* Prop = FindFProperty<FClassProperty>(
            UCommonBoundActionBar::StaticClass(), TEXT("ActionButtonClass"));
        if (!Prop) return;

        // Authoring tree.
        Prop->SetObjectPropertyValue(Prop->ContainerPtrToValuePtr<void>(Bar), ButtonClass);

        // Archetype tree (compile-survival; Phase 2 Item #13).
        if (UWidgetBlueprintGeneratedClass* GenClass = Cast<UWidgetBlueprintGeneratedClass>(Wbp->GeneratedClass))
        {
            if (UWidgetTree* ArchTree = GenClass->GetWidgetTreeArchetype())
            {
                ArchTree->ForEachWidget([&Prop, ButtonClass, Bar](UWidget* ArchW)
                {
                    if (ArchW && ArchW->GetFName() == Bar->GetFName())
                    {
                        if (UCommonBoundActionBar* ArchBar = Cast<UCommonBoundActionBar>(ArchW))
                        {
                            Prop->SetObjectPropertyValue(
                                Prop->ContainerPtrToValuePtr<void>(ArchBar),
                                ButtonClass);
                        }
                    }
                });
            }
        }
    }

    // ----- Shared: navigation wiring for a button-stack ------------------------
    //
    // Build a top-to-bottom chain: Up/Down link each pair of adjacent buttons;
    // the LAST button's Down points at the action bar (when one exists) so
    // gamepad navigation flows the natural path screen-content -> bottom bar.
    static void WireVerticalButtonNavigation(
        const TArray<UCommonButtonBase*>& Buttons,
        UWidget* TrailingTarget)
    {
        for (int32 i = 0; i < Buttons.Num(); ++i)
        {
            if (!Buttons[i]) continue;
            if (i > 0 && Buttons[i - 1])
            {
                Buttons[i]->SetNavigationRuleExplicit(EUINavigation::Up, Buttons[i - 1]);
            }
            if (i + 1 < Buttons.Num() && Buttons[i + 1])
            {
                Buttons[i]->SetNavigationRuleExplicit(EUINavigation::Down, Buttons[i + 1]);
            }
            else if (TrailingTarget)
            {
                Buttons[i]->SetNavigationRuleExplicit(EUINavigation::Down, TrailingTarget);
            }
        }
    }

    // ----- Shared: compile + capture results log ------------------------------
    //
    // FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions,
    // FCompilerResultsLog*) is the canonical "compile + capture" entry point.
    // We pass a non-null results-log pointer so the message bag stays inside
    // our scaffolder rather than getting silently dropped on the floor.
    static void CompileAndCapture(
        UWidgetBlueprint* Wbp,
        TArray<FString>& OutErrors,
        TArray<FString>& OutWarnings,
        FString& OutStatus)
    {
        if (!Wbp) { OutStatus = TEXT("BS_Unknown"); return; }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);

        FCompilerResultsLog Results;
        Results.SetSourcePath(Wbp->GetPathName());
        // Use bSilentMode so the message dialog doesn't pop in editor sessions
        // — agents drive this and dialog-blocking would deadlock the MCP call.
        Results.bSilentMode = true;
        FKismetEditorUtilities::CompileBlueprint(
            Wbp, EBlueprintCompileOptions::None, &Results);

        // UE 5.7 EMessageSeverity enumerators: Error / Warning /
        // PerformanceWarning / Info (verified via FTokenizedMessage::GetSeverityText
        // at Engine/Source/Runtime/Core/Private/Logging/TokenizedMessage.cpp:74-81).
        // No CriticalError exists. Pattern matches the Phase 1 Bug #5 capture in
        // MonolithUIActions.cpp:821-840.
        for (const TSharedRef<FTokenizedMessage>& M : Results.Messages)
        {
            const FString Text = M->ToText().ToString();
            const EMessageSeverity::Type Sev = M->GetSeverity();
            if (Sev == EMessageSeverity::Error)
            {
                OutErrors.Add(Text);
            }
            else if (Sev == EMessageSeverity::Warning || Sev == EMessageSeverity::PerformanceWarning)
            {
                OutWarnings.Add(Text);
            }
        }
        switch (Wbp->Status)
        {
            case BS_UpToDate:               OutStatus = TEXT("BS_UpToDate"); break;
            case BS_UpToDateWithWarnings:   OutStatus = TEXT("BS_UpToDateWithWarnings"); break;
            case BS_Dirty:                  OutStatus = TEXT("BS_Dirty"); break;
            case BS_Error:                  OutStatus = TEXT("BS_Error"); break;
            default:                        OutStatus = TEXT("BS_Unknown"); break;
        }
    }

    // ----- Shared: save the package -------------------------------------------
    static void SaveScaffoldedPackage(UPackage* Package, UWidgetBlueprint* Wbp, const FString& SavePath)
    {
        if (!Package || !Wbp) return;
        FAssetRegistryModule::AssetCreated(Wbp);
        Package->MarkPackageDirty();
        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        UPackage::SavePackage(
            Package, Wbp,
            *FPackageName::LongPackageNameToFilename(SavePath, FPackageName::GetAssetPackageExtension()),
            SaveArgs);
    }

    // ----- Shared: pull a JSON string array (optional) ------------------------
    static bool PullStringArray(
        const TSharedPtr<FJsonObject>& Params,
        const TCHAR* FieldName,
        TArray<FString>& Out)
    {
        if (!Params.IsValid()) return false;
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Params->TryGetArrayField(FieldName, Arr) || !Arr) return false;
        for (const TSharedPtr<FJsonValue>& V : *Arr)
        {
            if (V.IsValid() && V->Type == EJson::String)
            {
                FString S = V->AsString();
                if (!S.IsEmpty()) Out.Add(S);
            }
        }
        return Out.Num() > 0;
    }

    // ----- Shared: pack widgets_created JSON array ----------------------------
    static TSharedPtr<FJsonValue> WidgetCreatedEntry(const FString& Name, const FString& ClassName)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Name);
        Obj->SetStringField(TEXT("class"), ClassName);
        return MakeShared<FJsonValueObject>(Obj);
    }

    // ============================================================
    // Item #15 — scaffold_main_menu
    // ============================================================
    //
    // Defaults to ["Continue", "NewGame", "Options", "Quit"]. First button gets
    // initial focus + nav-up wrap; last button's Down nav points at the bottom
    // action bar.

    static FMonolithActionResult HandleScaffoldMainMenu(const TSharedPtr<FJsonObject>& Params)
    {
        FString SavePath;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("save_path"), SavePath))
        {
            return FMonolithActionResult::Error(TEXT("save_path required"));
        }

        TArray<FString> ButtonNames;
        if (!PullStringArray(Params, TEXT("button_names"), ButtonNames))
        {
            ButtonNames = { TEXT("Continue"), TEXT("NewGame"), TEXT("Options"), TEXT("Quit") };
        }

        // ----- 1. Parent class + WBP scaffold -----------------------------------
        bool bUsedTokenforge = false;
        FString ParentClassName;
        UClass* ParentClass = ResolveParentClass(Params, bUsedTokenforge, ParentClassName);

        UWidgetBlueprint* Wbp = nullptr;
        UPackage* Package = nullptr;
        FString AssetName;
        FMonolithActionResult CreateRes = CreateBlankActivatableWBP(SavePath, ParentClass, Wbp, Package, AssetName);
        if (!CreateRes.bSuccess) return CreateRes;

        // ----- 2. Build widget tree ---------------------------------------------
        TArray<TSharedPtr<FJsonValue>> WidgetsCreated;

        UOverlay* OverlayRoot = Wbp->WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("OverlayRoot"));
        Wbp->WidgetTree->RootWidget = OverlayRoot;
        WidgetsCreated.Add(WidgetCreatedEntry(TEXT("OverlayRoot"), TEXT("Overlay")));

        UVerticalBox* MenuVBox = Wbp->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("MenuVBox"));
        OverlayRoot->AddChild(MenuVBox);
        WidgetsCreated.Add(WidgetCreatedEntry(TEXT("MenuVBox"), TEXT("VerticalBox")));

        UCommonTextBlock* TitleText = Wbp->WidgetTree->ConstructWidget<UCommonTextBlock>(
            UCommonTextBlock::StaticClass(), TEXT("TitleText"));
        TitleText->SetText(FText::FromString(TEXT("Main Menu")));
        MenuVBox->AddChild(TitleText);
        WidgetsCreated.Add(WidgetCreatedEntry(TEXT("TitleText"), TEXT("CommonTextBlock")));

        // Per-button construction. Names are FName-validated by ConstructWidget;
        // duplicates would crash, so we de-dupe at the loop boundary.
        TSet<FName> UsedNames;
        TArray<UCommonButtonBase*> CreatedButtons;
        for (const FString& BtnName : ButtonNames)
        {
            FName N(*BtnName);
            if (UsedNames.Contains(N)) continue;
            UsedNames.Add(N);
            UCommonButtonBase* Btn = Wbp->WidgetTree->ConstructWidget<UCommonButtonBase>(
                UCommonButtonBase::StaticClass(), N);
            if (Btn)
            {
                MenuVBox->AddChild(Btn);
                CreatedButtons.Add(Btn);
                WidgetsCreated.Add(WidgetCreatedEntry(BtnName, TEXT("CommonButtonBase")));
            }
        }

        // ----- 3. Bound action bar ----------------------------------------------
        FString ActionBarButtonPath;
        bool bActionBarDefault = false;
        UClass* ActionBarButtonClass = ResolveActionBarButtonClass(Params, ActionBarButtonPath, bActionBarDefault);

        UCommonBoundActionBar* ActionBar = Wbp->WidgetTree->ConstructWidget<UCommonBoundActionBar>(
            UCommonBoundActionBar::StaticClass(), TEXT("ActionBar"));
        OverlayRoot->AddChild(ActionBar);
        WidgetsCreated.Add(WidgetCreatedEntry(TEXT("ActionBar"), TEXT("CommonBoundActionBar")));

        // Even when ActionBarButtonClass is null (default asset missing on
        // disk) the scaffold continues — the compile will surface the
        // ValidateCompiledDefaults error so the caller knows to either supply
        // an action_button_class or stand up the default Monolith button.
        StampActionBarButtonClass(Wbp, ActionBar, ActionBarButtonClass);

        // ----- 4. Focus target + nav wiring ------------------------------------
        const bool bFocusVarReady = EnsureDesiredFocusTargetVariable(Wbp);
        // Compile once before stamping CDO — AddMemberVariable + the new
        // skeleton class must exist before FindFProperty can find the new
        // property (otherwise WriteDesiredFocusTargetName silently no-ops).
        if (bFocusVarReady)
        {
            FKismetEditorUtilities::CompileBlueprint(Wbp);
        }

        const bool bFocusStamped = (CreatedButtons.Num() > 0)
            ? WriteDesiredFocusTargetName(Wbp, CreatedButtons[0]->GetFName())
            : false;

        WireVerticalButtonNavigation(CreatedButtons, ActionBar);

        // ----- 5. Final compile + capture --------------------------------------
        TArray<FString> CompileErrors, CompileWarnings;
        FString CompileStatus;
        CompileAndCapture(Wbp, CompileErrors, CompileWarnings, CompileStatus);
        SaveScaffoldedPackage(Package, Wbp, SavePath);

        // ----- 6. Manifest ------------------------------------------------------
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("wbp_path"), SavePath);
        Result->SetStringField(TEXT("parent_class"), ParentClassName);
        Result->SetBoolField(TEXT("used_tokenforge"), bUsedTokenforge);
        Result->SetStringField(TEXT("action_button_class"), ActionBarButtonPath);
        Result->SetBoolField(TEXT("action_button_class_was_default"), bActionBarDefault);
        Result->SetBoolField(TEXT("focus_target_variable_ready"), bFocusVarReady);
        Result->SetBoolField(TEXT("focus_target_stamped"), bFocusStamped);
        Result->SetStringField(TEXT("compile_status"), CompileStatus);
        Result->SetArrayField(TEXT("widgets_created"), WidgetsCreated);

        TArray<TSharedPtr<FJsonValue>> ErrArr;
        for (const FString& E : CompileErrors) ErrArr.Add(MakeShared<FJsonValueString>(E));
        Result->SetArrayField(TEXT("errors"), ErrArr);

        TArray<TSharedPtr<FJsonValue>> WarnArr;
        for (const FString& W : CompileWarnings) WarnArr.Add(MakeShared<FJsonValueString>(W));
        Result->SetArrayField(TEXT("warnings"), WarnArr);

        return FMonolithActionResult::Success(Result);
    }

    // ============================================================
    // Item #16 — scaffold_settings_panel_with_tabs
    // ============================================================
    //
    // Defaults to ["Gameplay", "Audio", "Video", "Accessibility", "Controls"].
    // Produces a Tab List + Animated Switcher pair, with one stateless
    // placeholder content panel per tab.

    static FMonolithActionResult HandleScaffoldSettingsPanelWithTabs(const TSharedPtr<FJsonObject>& Params)
    {
        FString SavePath;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("save_path"), SavePath))
        {
            return FMonolithActionResult::Error(TEXT("save_path required"));
        }

        TArray<FString> TabNames;
        if (!PullStringArray(Params, TEXT("tab_names"), TabNames))
        {
            TabNames = {
                TEXT("Gameplay"),
                TEXT("Audio"),
                TEXT("Video"),
                TEXT("Accessibility"),
                TEXT("Controls")
            };
        }

        // ----- 1. Parent class + WBP scaffold -----------------------------------
        bool bUsedTokenforge = false;
        FString ParentClassName;
        UClass* ParentClass = ResolveParentClass(Params, bUsedTokenforge, ParentClassName);

        UWidgetBlueprint* Wbp = nullptr;
        UPackage* Package = nullptr;
        FString AssetName;
        FMonolithActionResult CreateRes = CreateBlankActivatableWBP(SavePath, ParentClass, Wbp, Package, AssetName);
        if (!CreateRes.bSuccess) return CreateRes;

        // ----- 2. Build widget tree ---------------------------------------------
        TArray<TSharedPtr<FJsonValue>> WidgetsCreated;

        UOverlay* OverlayRoot = Wbp->WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("OverlayRoot"));
        Wbp->WidgetTree->RootWidget = OverlayRoot;
        WidgetsCreated.Add(WidgetCreatedEntry(TEXT("OverlayRoot"), TEXT("Overlay")));

        UVerticalBox* PanelVBox = Wbp->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("PanelVBox"));
        OverlayRoot->AddChild(PanelVBox);
        WidgetsCreated.Add(WidgetCreatedEntry(TEXT("PanelVBox"), TEXT("VerticalBox")));

        UCommonTextBlock* TitleText = Wbp->WidgetTree->ConstructWidget<UCommonTextBlock>(
            UCommonTextBlock::StaticClass(), TEXT("TitleText"));
        TitleText->SetText(FText::FromString(TEXT("Settings")));
        PanelVBox->AddChild(TitleText);
        WidgetsCreated.Add(WidgetCreatedEntry(TEXT("TitleText"), TEXT("CommonTextBlock")));

        // Tab list. UCommonTabListWidgetBase is itself abstract for WBP-host
        // use (it's a UCommonUserWidget); the standard pattern is to add a
        // bare instance and let the WBP author specialise via SetTabsButtonsOptions
        // at runtime. We construct it with a default name so the action_bar /
        // switcher can reference it by FName.
        UCommonTabListWidgetBase* TabList = Wbp->WidgetTree->ConstructWidget<UCommonTabListWidgetBase>(
            UCommonTabListWidgetBase::StaticClass(), TEXT("TabList"));
        if (TabList)
        {
            PanelVBox->AddChild(TabList);
            WidgetsCreated.Add(WidgetCreatedEntry(TEXT("TabList"), TEXT("CommonTabListWidgetBase")));
        }

        // Animated switcher hosts one content panel per tab. The switcher's
        // index is wired to the tab list at runtime by the WBP author.
        UCommonAnimatedSwitcher* Switcher = Wbp->WidgetTree->ConstructWidget<UCommonAnimatedSwitcher>(
            UCommonAnimatedSwitcher::StaticClass(), TEXT("ContentSwitcher"));
        PanelVBox->AddChild(Switcher);
        WidgetsCreated.Add(WidgetCreatedEntry(TEXT("ContentSwitcher"), TEXT("CommonAnimatedSwitcher")));

        // One stateless placeholder content panel per tab. Each panel is a
        // VerticalBox so downstream callers can add real settings widgets
        // without restructuring the tree.
        TSet<FName> UsedNames;
        for (const FString& TabName : TabNames)
        {
            const FName VBoxName(*FString::Printf(TEXT("Tab_%s_Content"), *TabName));
            if (UsedNames.Contains(VBoxName)) continue;
            UsedNames.Add(VBoxName);

            UVerticalBox* TabContent = Wbp->WidgetTree->ConstructWidget<UVerticalBox>(
                UVerticalBox::StaticClass(), VBoxName);
            if (TabContent)
            {
                Switcher->AddChild(TabContent);
                WidgetsCreated.Add(WidgetCreatedEntry(VBoxName.ToString(), TEXT("VerticalBox")));
            }
        }

        // ----- 3. Bound action bar (Back / Apply pattern) -----------------------
        FString ActionBarButtonPath;
        bool bActionBarDefault = false;
        UClass* ActionBarButtonClass = ResolveActionBarButtonClass(Params, ActionBarButtonPath, bActionBarDefault);

        UCommonBoundActionBar* ActionBar = Wbp->WidgetTree->ConstructWidget<UCommonBoundActionBar>(
            UCommonBoundActionBar::StaticClass(), TEXT("ActionBar"));
        OverlayRoot->AddChild(ActionBar);
        WidgetsCreated.Add(WidgetCreatedEntry(TEXT("ActionBar"), TEXT("CommonBoundActionBar")));
        StampActionBarButtonClass(Wbp, ActionBar, ActionBarButtonClass);

        // ----- 4. Focus target -------------------------------------------------
        const bool bFocusVarReady = EnsureDesiredFocusTargetVariable(Wbp);
        if (bFocusVarReady)
        {
            FKismetEditorUtilities::CompileBlueprint(Wbp);
        }
        const bool bFocusStamped = TabList
            ? WriteDesiredFocusTargetName(Wbp, TabList->GetFName())
            : false;

        // ----- 5. Final compile + capture --------------------------------------
        TArray<FString> CompileErrors, CompileWarnings;
        FString CompileStatus;
        CompileAndCapture(Wbp, CompileErrors, CompileWarnings, CompileStatus);
        SaveScaffoldedPackage(Package, Wbp, SavePath);

        // ----- 6. Manifest -----------------------------------------------------
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("wbp_path"), SavePath);
        Result->SetStringField(TEXT("parent_class"), ParentClassName);
        Result->SetBoolField(TEXT("used_tokenforge"), bUsedTokenforge);
        Result->SetStringField(TEXT("action_button_class"), ActionBarButtonPath);
        Result->SetBoolField(TEXT("action_button_class_was_default"), bActionBarDefault);
        Result->SetBoolField(TEXT("focus_target_variable_ready"), bFocusVarReady);
        Result->SetBoolField(TEXT("focus_target_stamped"), bFocusStamped);
        Result->SetNumberField(TEXT("tab_count"), TabNames.Num());
        Result->SetStringField(TEXT("compile_status"), CompileStatus);
        Result->SetArrayField(TEXT("widgets_created"), WidgetsCreated);

        TArray<TSharedPtr<FJsonValue>> ErrArr;
        for (const FString& E : CompileErrors) ErrArr.Add(MakeShared<FJsonValueString>(E));
        Result->SetArrayField(TEXT("errors"), ErrArr);

        TArray<TSharedPtr<FJsonValue>> WarnArr;
        for (const FString& W : CompileWarnings) WarnArr.Add(MakeShared<FJsonValueString>(W));
        Result->SetArrayField(TEXT("warnings"), WarnArr);

        return FMonolithActionResult::Success(Result);
    }

    // ============================================================
    // Item #17 — scaffold_pause_menu
    // ============================================================
    //
    // Defaults to ["Resume", "Settings", "Inventory", "Quit"]. Resume gets
    // initial focus. Quit gets requires_hold=true (Phase 1.5 allowlist made
    // this settable without raw_mode; the UCommonButtonBase setter is
    // SetRequiresHold which configure_common_button already calls).

    static FMonolithActionResult HandleScaffoldPauseMenu(const TSharedPtr<FJsonObject>& Params)
    {
        FString SavePath;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("save_path"), SavePath))
        {
            return FMonolithActionResult::Error(TEXT("save_path required"));
        }

        // Action table is required by the plan signature but treated as
        // metadata only at scaffold time — the caller wires bind_common_action_widget
        // entries onto specific buttons after the scaffold. We surface it
        // in the manifest so downstream tools can find it.
        FString ActionTablePath;
        Params->TryGetStringField(TEXT("action_table"), ActionTablePath);

        TArray<FString> ButtonNames;
        if (!PullStringArray(Params, TEXT("button_names"), ButtonNames))
        {
            ButtonNames = { TEXT("Resume"), TEXT("Settings"), TEXT("Inventory"), TEXT("Quit") };
        }

        // ----- 1. Parent class + WBP scaffold -----------------------------------
        bool bUsedTokenforge = false;
        FString ParentClassName;
        UClass* ParentClass = ResolveParentClass(Params, bUsedTokenforge, ParentClassName);

        UWidgetBlueprint* Wbp = nullptr;
        UPackage* Package = nullptr;
        FString AssetName;
        FMonolithActionResult CreateRes = CreateBlankActivatableWBP(SavePath, ParentClass, Wbp, Package, AssetName);
        if (!CreateRes.bSuccess) return CreateRes;

        // ----- 2. Build widget tree --------------------------------------------
        TArray<TSharedPtr<FJsonValue>> WidgetsCreated;

        UOverlay* OverlayRoot = Wbp->WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("OverlayRoot"));
        Wbp->WidgetTree->RootWidget = OverlayRoot;
        WidgetsCreated.Add(WidgetCreatedEntry(TEXT("OverlayRoot"), TEXT("Overlay")));

        // Backing border for the pause-menu pattern — separates the menu
        // visually from the paused game underneath. Pure decoration; gives
        // downstream callers a target for backdrop-blur stamps.
        UBorder* Backdrop = Wbp->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Backdrop"));
        OverlayRoot->AddChild(Backdrop);
        WidgetsCreated.Add(WidgetCreatedEntry(TEXT("Backdrop"), TEXT("Border")));

        UVerticalBox* MenuVBox = Wbp->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("MenuVBox"));
        Backdrop->AddChild(MenuVBox);
        WidgetsCreated.Add(WidgetCreatedEntry(TEXT("MenuVBox"), TEXT("VerticalBox")));

        UCommonTextBlock* TitleText = Wbp->WidgetTree->ConstructWidget<UCommonTextBlock>(
            UCommonTextBlock::StaticClass(), TEXT("TitleText"));
        TitleText->SetText(FText::FromString(TEXT("Paused")));
        MenuVBox->AddChild(TitleText);
        WidgetsCreated.Add(WidgetCreatedEntry(TEXT("TitleText"), TEXT("CommonTextBlock")));

        TSet<FName> UsedNames;
        TArray<UCommonButtonBase*> CreatedButtons;
        UCommonButtonBase* QuitButton = nullptr;
        for (const FString& BtnName : ButtonNames)
        {
            FName N(*BtnName);
            if (UsedNames.Contains(N)) continue;
            UsedNames.Add(N);
            UCommonButtonBase* Btn = Wbp->WidgetTree->ConstructWidget<UCommonButtonBase>(
                UCommonButtonBase::StaticClass(), N);
            if (Btn)
            {
                MenuVBox->AddChild(Btn);
                CreatedButtons.Add(Btn);
                WidgetsCreated.Add(WidgetCreatedEntry(BtnName, TEXT("CommonButtonBase")));
                if (BtnName.Equals(TEXT("Quit"), ESearchCase::IgnoreCase))
                {
                    QuitButton = Btn;
                }
            }
        }

        // ----- 3. Quit button hold-to-confirm ---------------------------------
        // Phase 1.5 allowlist makes requires_hold settable without raw_mode;
        // we apply it directly via SetRequiresHold (same call configure_common_button
        // uses at MonolithCommonUIButtonActions.cpp:249).
        if (QuitButton)
        {
            QuitButton->SetRequiresHold(true);
        }

        // ----- 4. Bound action bar --------------------------------------------
        FString ActionBarButtonPath;
        bool bActionBarDefault = false;
        UClass* ActionBarButtonClass = ResolveActionBarButtonClass(Params, ActionBarButtonPath, bActionBarDefault);

        UCommonBoundActionBar* ActionBar = Wbp->WidgetTree->ConstructWidget<UCommonBoundActionBar>(
            UCommonBoundActionBar::StaticClass(), TEXT("ActionBar"));
        OverlayRoot->AddChild(ActionBar);
        WidgetsCreated.Add(WidgetCreatedEntry(TEXT("ActionBar"), TEXT("CommonBoundActionBar")));
        StampActionBarButtonClass(Wbp, ActionBar, ActionBarButtonClass);

        // ----- 5. Focus target + nav wiring -----------------------------------
        const bool bFocusVarReady = EnsureDesiredFocusTargetVariable(Wbp);
        if (bFocusVarReady)
        {
            FKismetEditorUtilities::CompileBlueprint(Wbp);
        }
        // Resume button = initial focus (plan §3.8). Falls back to first
        // created button when caller suppressed the Resume default.
        UCommonButtonBase* InitialFocusBtn = nullptr;
        for (UCommonButtonBase* B : CreatedButtons)
        {
            if (B && B->GetFName() == FName(TEXT("Resume")))
            {
                InitialFocusBtn = B; break;
            }
        }
        if (!InitialFocusBtn && CreatedButtons.Num() > 0) InitialFocusBtn = CreatedButtons[0];
        const bool bFocusStamped = InitialFocusBtn
            ? WriteDesiredFocusTargetName(Wbp, InitialFocusBtn->GetFName())
            : false;

        WireVerticalButtonNavigation(CreatedButtons, ActionBar);

        // ----- 6. Final compile + capture -------------------------------------
        TArray<FString> CompileErrors, CompileWarnings;
        FString CompileStatus;
        CompileAndCapture(Wbp, CompileErrors, CompileWarnings, CompileStatus);
        SaveScaffoldedPackage(Package, Wbp, SavePath);

        // ----- 7. Manifest -----------------------------------------------------
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("wbp_path"), SavePath);
        Result->SetStringField(TEXT("parent_class"), ParentClassName);
        Result->SetBoolField(TEXT("used_tokenforge"), bUsedTokenforge);
        Result->SetStringField(TEXT("action_button_class"), ActionBarButtonPath);
        Result->SetBoolField(TEXT("action_button_class_was_default"), bActionBarDefault);
        Result->SetBoolField(TEXT("quit_requires_hold"), QuitButton != nullptr);
        Result->SetBoolField(TEXT("focus_target_variable_ready"), bFocusVarReady);
        Result->SetBoolField(TEXT("focus_target_stamped"), bFocusStamped);
        if (!ActionTablePath.IsEmpty())
        {
            Result->SetStringField(TEXT("action_table"), ActionTablePath);
        }
        Result->SetStringField(TEXT("compile_status"), CompileStatus);
        Result->SetArrayField(TEXT("widgets_created"), WidgetsCreated);

        TArray<TSharedPtr<FJsonValue>> ErrArr;
        for (const FString& E : CompileErrors) ErrArr.Add(MakeShared<FJsonValueString>(E));
        Result->SetArrayField(TEXT("errors"), ErrArr);

        TArray<TSharedPtr<FJsonValue>> WarnArr;
        for (const FString& W : CompileWarnings) WarnArr.Add(MakeShared<FJsonValueString>(W));
        Result->SetArrayField(TEXT("warnings"), WarnArr);

        return FMonolithActionResult::Success(Result);
    }

    // ----- Aggregator entry point -------------------------------------------
    //
    // Called from MonolithCommonUIActionsAggregator.cpp. The forward-declaration
    // in the aggregator (`namespace MonolithCommonUITemplate { void Register(...); }`)
    // matches the signature below; everything else in this file is
    // file-local-by-convention (static handlers in the same named namespace
    // give linker-private symbols that match the precedent in
    // MonolithCommonUIActivatableActions.cpp etc.).
    void Register(FMonolithToolRegistry& Registry)
    {
        const FString Cat(TEXT("CommonUI"));

        Registry.RegisterAction(
            TEXT("ui"), TEXT("scaffold_main_menu"),
            TEXT("Phase 3 Tier-3 scaffolder. One-shot main-menu WBP: Overlay root + Title + "
                 "VerticalBox of UCommonButtonBase per button_names[] (default "
                 "[\"Continue\",\"NewGame\",\"Options\",\"Quit\"]) + UCommonBoundActionBar with "
                 "ActionButtonClass set to MonolithDefaultCommonButton_C (or action_button_class "
                 "override). Parent class: UTokenforgeActivatableWidget when TokenforgeRuntime "
                 "plugin is enabled, else UCommonActivatableWidget (override with parent_class). "
                 "Stamps DesiredFocusTargetName UPROPERTY (creating it if missing), points it at "
                 "the first button, wires Up/Down nav between adjacent buttons, points the last "
                 "button's Down at ActionBar. Compiles + captures FCompilerResultsLog. Returns "
                 "{wbp_path, widgets_created[], compile_status, errors[], warnings[]}."),
            FMonolithActionHandler::CreateStatic(&HandleScaffoldMainMenu),
            FParamSchemaBuilder()
                .Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path, e.g. /Game/UI/WBP_MainMenu"))
                .Optional(TEXT("button_names"), TEXT("array"),
                    TEXT("Array of button FNames (default [\"Continue\",\"NewGame\",\"Options\",\"Quit\"])"))
                .Optional(TEXT("parent_class"), TEXT("string"),
                    TEXT("Override parent class (default: UTokenforgeActivatableWidget if TF enabled, else UCommonActivatableWidget)"))
                .Optional(TEXT("action_button_class"), TEXT("string"),
                    TEXT("UCommonButtonBase subclass path with _C suffix (default: /Game/Monolith/CommonUI/MonolithDefaultCommonButton.MonolithDefaultCommonButton_C)"))
                .Optional(TEXT("action_table"), TEXT("string"),
                    TEXT("Optional UDataTable path of FCommonInputActionDataBase rows (recorded in manifest; bind via bind_common_action_widget)"))
                .Optional(TEXT("default_style_palette"), TEXT("string"),
                    TEXT("Optional style-palette token for downstream apply_style_to_widget calls"))
                .Build(),
            Cat);

        Registry.RegisterAction(
            TEXT("ui"), TEXT("scaffold_settings_panel_with_tabs"),
            TEXT("Phase 3 Tier-3 scaffolder. One-shot settings-panel WBP with tab list + animated "
                 "switcher: Overlay root + Title + UCommonTabListWidgetBase + UCommonAnimatedSwitcher "
                 "+ one stateless UVerticalBox content placeholder per tab (default tabs "
                 "[\"Gameplay\",\"Audio\",\"Video\",\"Accessibility\",\"Controls\"]) + UCommonBoundActionBar "
                 "with ActionButtonClass set to MonolithDefaultCommonButton_C. Parent class follows "
                 "the same Tokenforge probe + UCommonActivatableWidget fallback as scaffold_main_menu. "
                 "Stamps DesiredFocusTargetName -> TabList. Compiles + captures FCompilerResultsLog. "
                 "Returns {wbp_path, widgets_created[], tab_count, compile_status, errors[], warnings[]}."),
            FMonolithActionHandler::CreateStatic(&HandleScaffoldSettingsPanelWithTabs),
            FParamSchemaBuilder()
                .Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path, e.g. /Game/UI/WBP_Settings"))
                .Optional(TEXT("tab_names"), TEXT("array"),
                    TEXT("Array of tab FName tokens (default [\"Gameplay\",\"Audio\",\"Video\",\"Accessibility\",\"Controls\"])"))
                .Optional(TEXT("parent_class"), TEXT("string"),
                    TEXT("Override parent class (default: Tokenforge if enabled, else UCommonActivatableWidget)"))
                .Optional(TEXT("action_table"), TEXT("string"),
                    TEXT("Optional UDataTable path (Back / Apply rows typical; bind via bind_common_action_widget after scaffold)"))
                .Optional(TEXT("action_button_class"), TEXT("string"),
                    TEXT("UCommonButtonBase subclass path (default: /Game/Monolith/CommonUI/MonolithDefaultCommonButton.MonolithDefaultCommonButton_C)"))
                .Build(),
            Cat);

        Registry.RegisterAction(
            TEXT("ui"), TEXT("scaffold_pause_menu"),
            TEXT("Phase 3 Tier-3 scaffolder. One-shot pause-menu WBP: Overlay root + UBorder backdrop "
                 "+ Title + VerticalBox of UCommonButtonBase per button_names[] (default "
                 "[\"Resume\",\"Settings\",\"Inventory\",\"Quit\"]) + UCommonBoundActionBar. Resume "
                 "button receives initial focus (DesiredFocusTargetName CDO stamp); Quit button "
                 "gets SetRequiresHold(true) (Phase 1.5 allowlist makes this writable without "
                 "raw_mode). Parent class follows Tokenforge probe + UCommonActivatableWidget "
                 "fallback. Compiles + captures FCompilerResultsLog. Returns {wbp_path, "
                 "widgets_created[], quit_requires_hold, compile_status, errors[], warnings[]}."),
            FMonolithActionHandler::CreateStatic(&HandleScaffoldPauseMenu),
            FParamSchemaBuilder()
                .Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path, e.g. /Game/UI/WBP_PauseMenu"))
                .Required(TEXT("action_table"), TEXT("string"),
                    TEXT("UDataTable path of FCommonInputActionDataBase rows for the bottom action-bar (recorded in manifest)"))
                .Optional(TEXT("button_names"), TEXT("array"),
                    TEXT("Array of button FNames (default [\"Resume\",\"Settings\",\"Inventory\",\"Quit\"]). Buttons named \"Quit\" get requires_hold=true."))
                .Optional(TEXT("parent_class"), TEXT("string"),
                    TEXT("Override parent class (default: Tokenforge if enabled, else UCommonActivatableWidget)"))
                .Optional(TEXT("action_button_class"), TEXT("string"),
                    TEXT("UCommonButtonBase subclass path (default: MonolithDefaultCommonButton_C)"))
                .Optional(TEXT("settings_class"), TEXT("string"),
                    TEXT("Optional settings WBP class path for the Settings button — recorded in manifest only"))
                .Optional(TEXT("inventory_class"), TEXT("string"),
                    TEXT("Optional inventory WBP class path for the Inventory button — recorded in manifest only"))
                .Build(),
            Cat);
    }
}

#endif // WITH_COMMONUI
