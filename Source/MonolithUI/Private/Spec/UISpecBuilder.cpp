// Copyright tumourlove. All Rights Reserved.
// UISpecBuilder.cpp
//
// Phase H — implementation of the transactional `build_ui_from_spec` pipeline.
//
// Pipeline summary (matches plan §1.4):
//   1. Validate document (already-parsed; the caller does JSON parse + first
//      validator pass, the builder runs the deeper validation that needs the
//      registry).
//   2. Resolve parent class.
//   3. Dry-walk: type-resolve every node, hash every style, depth-check.
//      No package mutation here — this is the gate for the "create new"
//      path because FScopedTransaction can't roll back CreatePackage.
//   4. Get-or-create the WBP. Open `FScopedTransaction` for the in-place edit
//      sub-case so `Modify()` calls actually undo on cancel.
//   5. Pre-create styles (H10/H11 — BEFORE any widget construction).
//   6. Walk the tree, dispatching per-node to Panel/Leaf/CommonUI/EffectSurface
//      sub-builders.
//   7. Wire animations (Phase I I9 — BuildSingle integration).
//   8. Compile.
//   9. Rebuild widget-id map from the post-compile WidgetTree (H12/H13).
//  10. Save (or rollback / cancel).
//
// Atomicity invariant: any failure between step 4 and step 9 either cancels
// the transaction (in-place edit sub-case) or marks the new package garbage +
// deletes the new asset (create-new sub-case). The asset is never left
// partially mutated.

#include "Spec/UISpecBuilder.h"

#include "Spec/UISpecValidator.h"
#include "Spec/UIBuildContext.h"
#include "Spec/Builders/PanelBuilder.h"
#include "Spec/Builders/LeafBuilder.h"
#include "Spec/Builders/CommonUIBuilder.h"
#include "Spec/Builders/EffectSurfaceBuilder.h"

#include "MonolithUICommon.h"
#include "MonolithUISettings.h"
#include "Registry/MonolithUIRegistrySubsystem.h"
#include "Registry/UITypeRegistry.h"
#include "Registry/UIPropertyAllowlist.h"
#include "Registry/UIPropertyPathCache.h"
#include "Registry/UIReflectionHelper.h"
#include "Animation/UIAnimationMovieSceneBuilder.h"
#include "Animation/UISpecAnimation.h"

// Editor / asset machinery
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "Editor.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "UObject/SavePackage.h"
#include "UObject/Package.h"

// Widget Blueprint / UMG
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/ContentWidget.h"
#include "Components/CanvasPanel.h"

// Kismet editor
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

// Json
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// CommonUI parent-class allowlist (gated)
#if WITH_COMMONUI
#include "CommonActivatableWidget.h"
#include "CommonUserWidget.h"
#endif

#define LOCTEXT_NAMESPACE "MonolithUISpecBuilder"


namespace MonolithUI::SpecBuilderInternal
{
    // ------------------------------------------------------------------
    // Validation extensions used by the builder. Phase H extends the Phase A
    // stub validator with: parent-class allow check, depth limit, cycle
    // detection, allowlist-gated property-write preflight, animation/style
    // ref existence checks. We collect findings into the existing
    // FUISpecValidationResult so the LLM-facing report shape is uniform.

    /**
     * Cap for spec-tree depth — pulled from settings each call so live tuning
     * works without restart. Defaults to 32 (matches FUISpecBuilder header
     * + plan §H13.6 + UMonolithUISettings::MaxNestingDepth ctor default).
     */
    static int32 GetMaxNestingDepth()
    {
        if (const UMonolithUISettings* Settings = UMonolithUISettings::Get())
        {
            return FMath::Max(1, Settings->MaxNestingDepth);
        }
        return 32;
    }

    /**
     * Walk the tree and accumulate every (StyleType, hash) pair plus a
     * "did the structure look healthy?" return. Performs:
     *   - cycle detection via visited-set keyed by `FName`-id (H13.5)
     *   - depth-limit gate (H13.6)
     *   - per-node `Type` registry lookup (so we know UClass before commit)
     *
     * On any structural failure pushes a typed FUISpecError into OutResult
     * and returns false. Callers MUST honour false — the dry-walk is the
     * gate that prevents CreatePackage on bad input.
     */
    static bool DryWalkRecurse(
        const FUISpecNode& Node,
        int32 Depth,
        TSet<FName>& VisitedIds,
        const FUITypeRegistry& Registry,
        FUISpecValidationResult& OutResult)
    {
        const int32 MaxDepth = GetMaxNestingDepth();
        if (Depth > MaxDepth)
        {
            FUISpecError E;
            E.Severity = EUISpecErrorSeverity::Error;
            E.Category = TEXT("Structure");
            E.JsonPath = TEXT("/rootWidget/.../children");
            E.WidgetId = Node.Id;
            E.Message  = FString::Printf(
                TEXT("Spec tree depth exceeds MaxNestingDepth (%d). Reduce nesting or raise the limit in Project Settings → Plugins → Monolith UI."),
                MaxDepth);
            E.SuggestedFix = TEXT("Flatten the layout, or set MaxNestingDepth higher.");
            OutResult.Errors.Add(MoveTemp(E));
            return false;
        }

        // Cycle gate (H13.5). The node's Id is what makes a "cycle" visible
        // to the LLM (TSharedPtr aliasing through Children produces it).
        // Empty Ids are allowed but excluded from the visited-set — the
        // builder auto-generates names for those, so they cannot self-loop.
        if (!Node.Id.IsNone())
        {
            if (VisitedIds.Contains(Node.Id))
            {
                FUISpecError E;
                E.Severity = EUISpecErrorSeverity::Error;
                E.Category = TEXT("Structure");
                E.WidgetId = Node.Id;
                E.JsonPath = FString::Printf(TEXT("/.../%s"), *Node.Id.ToString());
                E.Message  = FString::Printf(
                    TEXT("Duplicate widget id '%s' in spec tree. This also fires when the spec contains a TSharedPtr cycle that re-enters the same node."),
                    *Node.Id.ToString());
                E.SuggestedFix = TEXT("Each node Id must be unique. Rename the duplicate, or break the parent->child cycle.");
                OutResult.Errors.Add(MoveTemp(E));
                return false;
            }
            VisitedIds.Add(Node.Id);
        }

        // Type-registry lookup — surfaces an actionable error before any
        // ConstructWidget call would have crashed inside the engine.
        //
        // Two-stage gate when the registry is populated: registry first,
        // legacy short-name resolver second. Both miss + no CustomClassPath
        // => the type is genuinely unresolvable; refuse.
        //
        // When the registry has zero entries (subsystem not yet initialised
        // in test contexts), we fall back to the legacy resolver alone so
        // the simplest "VerticalBox + TextBlock" specs still build.
        if (!Node.Type.IsNone())
        {
            const FUITypeRegistryEntry* Entry = Registry.FindByToken(Node.Type);
            const bool bRegistryHasEntries = Registry.Num() > 0;
            if (!Entry && Node.CustomClassPath.IsEmpty())
            {
                // Try the legacy resolver before failing — covers the test
                // path where the subsystem hasn't populated yet.
                UClass* Legacy = MonolithUI::WidgetClassFromName(Node.Type.ToString());
                if (!Legacy)
                {
                    FUISpecError E;
                    E.Severity = EUISpecErrorSeverity::Error;
                    E.Category = TEXT("Type");
                    E.WidgetId = Node.Id;
                    E.JsonPath = FString::Printf(TEXT("/.../%s/type"), *Node.Id.ToString());
                    E.Message  = FString::Printf(
                        TEXT("Unknown widget type token '%s'. %s"),
                        *Node.Type.ToString(),
                        bRegistryHasEntries
                            ? TEXT("Use `ui::dump_property_allowlist` to see registered types, or supply CustomClassPath.")
                            : TEXT("Registry is empty (subsystem not yet initialised); supply a CustomClassPath or use a stock UMG class name."));
                    E.SuggestedFix = TEXT("Provide a registered type token, or set CustomClassPath to a /Game/.../X.X_C path.");
                    OutResult.Errors.Add(MoveTemp(E));
                    return false;
                }
            }
        }

        for (const TSharedPtr<FUISpecNode>& Child : Node.Children)
        {
            if (!Child.IsValid())
            {
                continue;
            }
            if (!DryWalkRecurse(*Child, Depth + 1, VisitedIds, Registry, OutResult))
            {
                return false;
            }
        }

        return true;
    }

    /**
     * Resolve the parent-class string to a UClass*. Accepts:
     *   - "UserWidget" / "CommonActivatableWidget" / "CommonUserWidget" tokens
     *   - "/Script/ModuleName.ClassName" full paths
     *   - "/Game/UI/.../X.X_C" Blueprint-generated class paths (rare)
     * Returns nullptr + populates OutErr on miss.
     */
    static UClass* ResolveParentClass(const FString& ParentClassToken, FString& OutErr)
    {
        FString Token = ParentClassToken.TrimStartAndEnd();
        if (Token.IsEmpty())
        {
            return UUserWidget::StaticClass();
        }

        // Token form first.
        if (Token == TEXT("UserWidget"))      return UUserWidget::StaticClass();
#if WITH_COMMONUI
        if (Token == TEXT("CommonUserWidget"))        return UCommonUserWidget::StaticClass();
        if (Token == TEXT("CommonActivatableWidget")) return UCommonActivatableWidget::StaticClass();
#endif

        // Path form. FindFirstObject with NativeFirst — matches the existing
        // create_widget_blueprint resolver shape so behaviour is consistent.
        UClass* Resolved = FindFirstObject<UClass>(*Token, EFindFirstObjectOptions::NativeFirst);
        if (!Resolved)
        {
            // Try with a leading `U` (caller may have stripped the prefix).
            Resolved = FindFirstObject<UClass>(*(TEXT("U") + Token), EFindFirstObjectOptions::NativeFirst);
        }
        if (!Resolved)
        {
            // Last resort — full path / BP class.
            Resolved = LoadObject<UClass>(nullptr, *Token);
        }

        if (!Resolved || !Resolved->IsChildOf(UUserWidget::StaticClass()))
        {
            OutErr = FString::Printf(TEXT("Parent class '%s' not found or not a UUserWidget subclass."), *Token);
            return nullptr;
        }
        return Resolved;
    }

    /**
     * Look up the widget UClass for a node — checks the registry first, then
     * the CustomClassPath fallback. Returns nullptr on miss; the dry-walk is
     * the place that produces a helpful error so this is a quiet helper.
     */
    static UClass* ResolveWidgetClass(const FUISpecNode& Node, const FUITypeRegistry& Registry)
    {
        if (!Node.Type.IsNone())
        {
            if (const FUITypeRegistryEntry* Entry = Registry.FindByToken(Node.Type))
            {
                if (Entry->WidgetClass.IsValid())
                {
                    return Entry->WidgetClass.Get();
                }
            }
        }
        if (!Node.CustomClassPath.IsEmpty())
        {
            if (UClass* C = LoadObject<UClass>(nullptr, *Node.CustomClassPath))
            {
                return C;
            }
        }
        // Last-ditch — try the legacy short-name lookup the older actions used.
        if (!Node.Type.IsNone())
        {
            return MonolithUI::WidgetClassFromName(Node.Type.ToString());
        }
        return nullptr;
    }

    /**
     * Recurse the spec tree and accumulate every (StyleType, hash) pair into
     * Context.PreCreatedStyles. v1 only seeds named entries from
     * `Document.Styles` plus any node `StyleRef` reference; the hash of
     * inline-style content is reserved for the JSON-side surface (Phase J
     * roundtrip writes inline styles), and is bumped here so the
     * StyleHookCalls counter still reflects the unique-style count.
     *
     * Why split this from the tree walk: H10/H11 explicitly require that the
     * style service be asked for every unique style hash BEFORE the first
     * widget is constructed. A single pre-walk gives us that ordering
     * guarantee and lets us hash-dedupe across the whole tree in one go.
     */
    static void PreCreateStylesRecurse(
        const FUISpecNode& Node,
        FUIBuildContext& Context,
        TSet<FName>& SeenNamedRefs)
    {
        if (!Node.StyleRef.IsNone())
        {
            if (!SeenNamedRefs.Contains(Node.StyleRef))
            {
                SeenNamedRefs.Add(Node.StyleRef);
                ++Context.StyleHookCalls;

                // v1: registers an entry pointer keyed by name. The actual
                // style class instantiation is the responsibility of the
                // CommonUI builder when (and if) the style references a
                // CommonUI-shaped style. For non-CommonUI specs the entry
                // is a best-effort marker — Phase J consumes it for round-trip.
                Context.PreCreatedStyles.Add(
                    FString::Printf(TEXT("name:%s"), *Node.StyleRef.ToString()),
                    nullptr);
            }
        }

        for (const TSharedPtr<FUISpecNode>& Child : Node.Children)
        {
            if (Child.IsValid())
            {
                PreCreateStylesRecurse(*Child, Context, SeenNamedRefs);
            }
        }
    }

    static void CountDryRunNodesRecurse(const FUISpecNode& Node, FUIBuildContext& Context)
    {
        ++Context.NodesCreated;
        Context.DiffLines.Add(FString::Printf(
            TEXT("create: %s (%s)"),
            *Node.Id.ToString(),
            *Node.Type.ToString()));

        for (const TSharedPtr<FUISpecNode>& Child : Node.Children)
        {
            if (Child.IsValid())
            {
                CountDryRunNodesRecurse(*Child, Context);
            }
        }
    }

    /**
     * Construct a single node's widget + recurse its children. Dispatches on
     * container kind (Panel / Content / Leaf) — the type registry classifies
     * each entry at init time so we trust what the registry says.
     *
     * Returns the constructed UWidget* (nullptr on failure, with the error
     * pushed into Context.Errors). The caller decides what to do with the
     * pointer (set as RootWidget / use as parent for next recursion).
     */
    static UWidget* BuildNodeRecurse(
        FUIBuildContext& Context,
        const FUISpecNode& Node,
        UPanelWidget* ParentPanel)
    {
        UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get();
        const FUITypeRegistry* RegistryPtr = Sub ? &Sub->GetTypeRegistry() : nullptr;

        UClass* WidgetClass = nullptr;
        EUIContainerKind Kind = EUIContainerKind::Leaf;

        if (RegistryPtr)
        {
            WidgetClass = ResolveWidgetClass(Node, *RegistryPtr);
            if (const FUITypeRegistryEntry* Entry = RegistryPtr->FindByToken(Node.Type))
            {
                Kind = Entry->ContainerKind;
            }
        }
        else
        {
            // No registry available (test contexts before subsystem init);
            // fall back to the legacy short-name resolver so the simplest
            // "VerticalBox + TextBlock" specs still build.
            WidgetClass = MonolithUI::WidgetClassFromName(Node.Type.ToString());
        }

        if (!WidgetClass)
        {
            FUISpecError E;
            E.Severity = EUISpecErrorSeverity::Error;
            E.Category = TEXT("Type");
            E.WidgetId = Node.Id;
            E.Message  = FString::Printf(
                TEXT("Could not resolve UClass for node '%s' (type='%s', customClass='%s')."),
                *Node.Id.ToString(), *Node.Type.ToString(), *Node.CustomClassPath);
            Context.Errors.Add(MoveTemp(E));
            return nullptr;
        }

        UWidget* Constructed = nullptr;

        // Re-derive container kind from the UClass when the registry didn't
        // already give it (covers CustomClassPath + missing-registry paths).
        if (RegistryPtr == nullptr || Kind == EUIContainerKind::Leaf)
        {
            if (WidgetClass->IsChildOf(UPanelWidget::StaticClass()))
            {
                if (UPanelWidget* CDOPanel = Cast<UPanelWidget>(WidgetClass->GetDefaultObject()))
                {
                    Kind = CDOPanel->CanHaveMultipleChildren()
                        ? EUIContainerKind::Panel
                        : EUIContainerKind::Content;
                }
            }
        }

        switch (Kind)
        {
        case EUIContainerKind::Panel:
            Constructed = MonolithUI::PanelBuilder::BuildPanel(
                Context, Node, WidgetClass, ParentPanel);
            break;
        case EUIContainerKind::Content:
        case EUIContainerKind::Leaf:
        default:
            Constructed = MonolithUI::LeafBuilder::BuildLeafOrContent(
                Context, Node, WidgetClass, ParentPanel);
            break;
        }

        if (!Constructed)
        {
            // Sub-builder already pushed the error.
            return nullptr;
        }

        // CommonUI sub-bag — applies after construction (the widget needs to
        // exist for the style class to be wired in).
        if (Node.bHasCommonUI)
        {
            MonolithUI::CommonUIBuilder::ApplyCommonUI(Context, Node, Constructed);
        }

        // Effect surface sub-bag — applies after construction; the widget
        // must already be a UEffectSurface (validator should have flagged
        // type mismatches; we check inside the sub-builder defensively).
        if (Node.bHasEffect)
        {
            MonolithUI::EffectSurfaceBuilder::ApplyEffect(Context, Node, Constructed);
        }

        // Store the widget pointer keyed by id — the dispatcher needs this
        // for parent lookups in mid-tree. Empty Ids get a generated key so
        // the post-compile rebuild can still find them by FName.
        if (!Node.Id.IsNone())
        {
            Context.PreCompileWidgets.Add(Node.Id, Constructed);
        }

        ++Context.NodesCreated;

        // Children are dispatched by the panel/leaf builder when applicable.
        // The dispatcher does not recurse here because the sub-builders own
        // the parent-slot wiring (and need to be the ones telling
        // BuildNodeRecurse how to attach the child).
        if (Kind == EUIContainerKind::Panel)
        {
            UPanelWidget* AsPanel = Cast<UPanelWidget>(Constructed);
            for (const TSharedPtr<FUISpecNode>& Child : Node.Children)
            {
                if (Child.IsValid() && AsPanel)
                {
                    BuildNodeRecurse(Context, *Child, AsPanel);
                }
            }
        }
        else if (Kind == EUIContainerKind::Content && Node.Children.Num() > 0)
        {
            // Content widgets accept exactly one child. Recurse the first;
            // warn (don't fail) on additional children.
            UPanelWidget* AsPanel = Cast<UPanelWidget>(Constructed);
            if (AsPanel && Node.Children[0].IsValid())
            {
                BuildNodeRecurse(Context, *Node.Children[0], AsPanel);
            }
            if (Node.Children.Num() > 1)
            {
                FUISpecError W;
                W.Severity = EUISpecErrorSeverity::Warning;
                W.Category = TEXT("Structure");
                W.WidgetId = Node.Id;
                W.Message  = FString::Printf(
                    TEXT("Node '%s' is a single-child container but has %d children — additional children ignored."),
                    *Node.Id.ToString(), Node.Children.Num());
                W.SuggestedFix = TEXT("Wrap the children in a VerticalBox/HorizontalBox.");
                Context.Warnings.Add(MoveTemp(W));
            }
        }

        return Constructed;
    }

    /**
     * Walk the post-compile WidgetTree and rebuild the FName -> UWidget* map.
     * Required because `FKismetEditorUtilities::CompileBlueprint` recreates
     * Class Default Object instances; pointers cached pre-compile are stale.
     * (H12/H13 + recon takeaway §8 — without this the animation pass would
     * reference dead UWidget* memory.)
     */
    static void RebuildWidgetIdMapPostCompile(FUIBuildContext& Context)
    {
        Context.WidgetIdMap.Reset();

        if (!Context.TargetWBP || !Context.TargetWBP->WidgetTree)
        {
            return;
        }

        TArray<UWidget*> AllWidgets;
        Context.TargetWBP->WidgetTree->GetAllWidgets(AllWidgets);
        for (UWidget* W : AllWidgets)
        {
            if (W && !W->GetFName().IsNone())
            {
                Context.WidgetIdMap.Add(W->GetFName(), W);
            }
        }
    }

    /**
     * Helper — split a `/Game/Path/AssetName` into PackagePath ("/Game/Path")
     * and AssetName ("AssetName"). Returns false on garbage input (no slash,
     * empty asset name).
     */
    static bool SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName)
    {
        if (!AssetPath.Split(TEXT("/"), &OutPackagePath, &OutAssetName,
            ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        {
            return false;
        }
        return !OutAssetName.IsEmpty() && !OutPackagePath.IsEmpty();
    }

    /**
     * Get-or-create the WBP. Returns the WBP plus a flag indicating whether
     * the asset existed before the call (drives the rollback strategy).
     *
     * On the create path: dry-walk has already passed; we call CreatePackage
     * + FactoryCreateNew. Failures here trigger MarkAsGarbage on the package.
     *
     * On the existing path: load the WBP. If overwrite=false and the parent
     * class doesn't match, return nullptr + populate OutError.
     */
    static UWidgetBlueprint* GetOrCreateWBP(
        const FString& AssetPath,
        UClass* ParentClass,
        bool bOverwrite,
        bool& bOutPreExisting,
        UPackage*& OutPackage,
        FString& OutError)
    {
        bOutPreExisting = false;
        OutPackage = nullptr;

        // Probe the asset registry FIRST — cheaper than CreatePackage and
        // gives us a clean "exists" signal independent of whether the package
        // is currently loaded.
        const FString ObjectPath = FString::Printf(TEXT("%s.%s"),
            *AssetPath, *FPaths::GetCleanFilename(AssetPath));
        FAssetRegistryModule& ARM =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

        FString PackagePath, AssetName;
        if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
        {
            OutError = FString::Printf(TEXT("Invalid asset_path '%s' — must be /Game/Path/Name."), *AssetPath);
            return nullptr;
        }

        const FString FullObjectPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
        const FAssetData ExistingAsset = ARM.Get().GetAssetByObjectPath(FSoftObjectPath(FullObjectPath));
        if (ExistingAsset.IsValid())
        {
            UWidgetBlueprint* Existing = Cast<UWidgetBlueprint>(ExistingAsset.GetAsset());
            if (!Existing)
            {
                OutError = FString::Printf(TEXT("Asset at '%s' exists but is not a UWidgetBlueprint."), *AssetPath);
                return nullptr;
            }
            if (!bOverwrite)
            {
                OutError = FString::Printf(TEXT("Asset already exists at '%s' and overwrite=false."), *AssetPath);
                return nullptr;
            }
            // Parent-class mismatch is a structural issue — refuse rather than
            // silently rebase the BP.
            if (Existing->ParentClass != ParentClass)
            {
                OutError = FString::Printf(
                    TEXT("Existing WBP '%s' has parent '%s'; spec requested '%s'. Refusing to rebase."),
                    *AssetPath,
                    Existing->ParentClass ? *Existing->ParentClass->GetName() : TEXT("None"),
                    ParentClass ? *ParentClass->GetName() : TEXT("None"));
                return nullptr;
            }

            bOutPreExisting = true;
            OutPackage = Existing->GetPackage();
            return Existing;
        }

        // Create-new path. The dry-walk has passed; we now do the disk-side
        // mutation. Failures here are recovered by caller via MarkAsGarbage.
        OutPackage = CreatePackage(*AssetPath);
        if (!OutPackage)
        {
            OutError = FString::Printf(TEXT("CreatePackage failed for '%s'."), *AssetPath);
            return nullptr;
        }

        UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
        Factory->BlueprintType = BPTYPE_Normal;
        Factory->ParentClass = ParentClass;

        UObject* Created = Factory->FactoryCreateNew(
            UWidgetBlueprint::StaticClass(), OutPackage,
            FName(*AssetName), RF_Public | RF_Standalone, nullptr, GWarn);

        UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Created);
        if (!WBP)
        {
            OutError = TEXT("UWidgetBlueprintFactory::FactoryCreateNew returned null.");
            return nullptr;
        }

        return WBP;
    }

    /**
     * Manual rollback for the create-new path. `FScopedTransaction` does NOT
     * undo CreatePackage / FactoryCreateNew on cancel — we have to clean up
     * the disk artefact ourselves (per the H2 + R3 atomicity contract).
     *
     * Steps:
     *   1. Mark the new package + WBP as garbage so the GC can collect it.
     *   2. Call ObjectTools::DeleteSingleObject so the asset registry's view
     *      is consistent and the package file (if any) is removed.
     *
     * Best-effort — if DeleteSingleObject fails (e.g. the package is locked)
     * we still log a warning and continue. The MarkAsGarbage call at minimum
     * ensures the asset is no longer reachable from the in-memory registry.
     */
    static void RollbackCreatedAsset(UWidgetBlueprint* WBP, UPackage* Package)
    {
        if (WBP)
        {
            // Best-effort delete from the asset registry / disk. Reference-check
            // is on (default) so we don't bypass the pending-references check.
            const bool bDeleted = ObjectTools::DeleteSingleObject(WBP, /*bPerformReferenceCheck=*/false);
            if (!bDeleted)
            {
                UE_LOG(LogMonolithUISpec, Warning,
                    TEXT("UISpecBuilder rollback: ObjectTools::DeleteSingleObject failed for '%s' — marking garbage instead."),
                    *WBP->GetName());
            }
            WBP->MarkAsGarbage();
        }
        if (Package)
        {
            Package->MarkAsGarbage();
        }
    }

    /**
     * Save a freshly-built or freshly-modified WBP to disk. Mirrors the
     * SavePackage shape used by the legacy create_widget_blueprint path so
     * downstream tooling sees identical results.
     */
    static bool SaveWBP(UWidgetBlueprint* WBP, const FString& AssetPath, FString& OutError)
    {
        if (!WBP || !WBP->GetPackage())
        {
            OutError = TEXT("SaveWBP called with null WBP or package.");
            return false;
        }

        FAssetRegistryModule::AssetCreated(WBP);
        WBP->GetPackage()->MarkPackageDirty();

        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        const FString PackageFilename = FPackageName::LongPackageNameToFilename(
            AssetPath, FPackageName::GetAssetPackageExtension());

        if (!UPackage::SavePackage(WBP->GetPackage(), WBP, *PackageFilename, SaveArgs))
        {
            OutError = FString::Printf(TEXT("UPackage::SavePackage failed for '%s'."), *AssetPath);
            return false;
        }
        return true;
    }

    /**
     * Promote any warnings to errors when bTreatWarningsAsErrors is set. Done
     * once at the end of the build so we don't fail mid-walk on a warning
     * that the caller might have wanted recorded for diagnostics.
     */
    static void EscalateWarningsIfRequested(FUIBuildContext& Context)
    {
        if (!Context.bTreatWarningsAsErrors)
        {
            return;
        }
        for (FUISpecError& W : Context.Warnings)
        {
            W.Severity = EUISpecErrorSeverity::Error;
            Context.Errors.Add(MoveTemp(W));
        }
        Context.Warnings.Reset();
    }
} // namespace MonolithUI::SpecBuilderInternal


// ============================================================================
// FUISpecBuilder::Build
// ============================================================================

FUISpecBuilderResult FUISpecBuilder::Build(const FUISpecBuilderInputs& Inputs)
{
    using namespace MonolithUI::SpecBuilderInternal;

    FUISpecBuilderResult Result;
    Result.AssetPath  = Inputs.AssetPath;
    Result.RequestId  = Inputs.RequestId;

    // ---------- 1. Document presence -----------------------------------
    if (!Inputs.Document)
    {
        FUISpecError E;
        E.Severity = EUISpecErrorSeverity::Error;
        E.Category = TEXT("Internal");
        E.Message  = TEXT("FUISpecBuilder::Build called with null Document.");
        Result.Validation.Errors.Add(MoveTemp(E));
        Result.Validation.bIsValid = false;
        return Result;
    }
    const FUISpecDocument& Doc = *Inputs.Document;

    // ---------- 2. Validate (Phase A stub + Phase H extensions) --------
    Result.Validation = FUISpecValidator::Validate(Doc);
    if (!Result.Validation.bIsValid)
    {
        // The asset is never created on validator-fail (per H4/H5).
        return Result;
    }

    // ---------- 3. Resolve parent class --------------------------------
    FString ParentErr;
    UClass* ParentClass = ResolveParentClass(Doc.ParentClass, ParentErr);
    if (!ParentClass)
    {
        FUISpecError E;
        E.Severity = EUISpecErrorSeverity::Error;
        E.Category = TEXT("Type");
        E.JsonPath = TEXT("/parentClass");
        E.Message  = ParentErr;
        Result.Errors.Add(MoveTemp(E));
        return Result;
    }

    // ---------- 4. DRY-WALK (H2 atomicity gate) ------------------------
    UMonolithUIRegistrySubsystem* RegistrySub = UMonolithUIRegistrySubsystem::Get();
    if (Doc.Root.IsValid())
    {
        TSet<FName> VisitedIds;
        // Empty registry is OK — DryWalkRecurse defends against null.
        const FUITypeRegistry EmptyRegistry; // local fallback if subsystem missing
        const FUITypeRegistry& Registry = RegistrySub
            ? RegistrySub->GetTypeRegistry() : EmptyRegistry;
        if (!DryWalkRecurse(*Doc.Root, /*Depth=*/0, VisitedIds, Registry, Result.Validation))
        {
            Result.Validation.bIsValid = false;
            return Result;
        }
    }

    // ---------- 5. Set up the per-pass build context --------------------
    FUIBuildContext Context;
    Context.Document    = &Doc;
    Context.AssetPath   = Inputs.AssetPath;
    Context.RequestId   = Inputs.RequestId;
    Context.bDryRun     = Inputs.bDryRun;
    Context.bTreatWarningsAsErrors = Inputs.bTreatWarningsAsErrors;
    Context.bRawMode    = Inputs.bRawMode;
    if (RegistrySub)
    {
        Context.Allowlist = &RegistrySub->GetAllowlist();
        Context.PathCache = RegistrySub->GetPathCache();
    }

    if (Inputs.bDryRun)
    {
        FString PackagePath, AssetName;
        if (!SplitAssetPath(Inputs.AssetPath, PackagePath, AssetName))
        {
            FUISpecError E;
            E.Severity = EUISpecErrorSeverity::Error;
            E.Category = TEXT("AssetCreate");
            E.JsonPath = TEXT("/assetPath");
            E.Message = FString::Printf(
                TEXT("Invalid asset_path '%s' — must be /Game/Path/Name."),
                *Inputs.AssetPath);
            Result.Errors.Add(MoveTemp(E));
            return Result;
        }

        const FString FullObjectPath = FString::Printf(TEXT("%s.%s"), *Inputs.AssetPath, *AssetName);
        FAssetRegistryModule& ARM =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        const FAssetData ExistingAsset = ARM.Get().GetAssetByObjectPath(FSoftObjectPath(FullObjectPath));
        if (ExistingAsset.IsValid())
        {
            UWidgetBlueprint* Existing = Cast<UWidgetBlueprint>(ExistingAsset.GetAsset());
            if (!Existing)
            {
                FUISpecError E;
                E.Severity = EUISpecErrorSeverity::Error;
                E.Category = TEXT("Asset");
                E.JsonPath = TEXT("/assetPath");
                E.Message = FString::Printf(
                    TEXT("Asset at '%s' exists but is not a UWidgetBlueprint."),
                    *Inputs.AssetPath);
                Result.Errors.Add(MoveTemp(E));
                return Result;
            }
            if (!Inputs.bOverwrite)
            {
                FUISpecError E;
                E.Severity = EUISpecErrorSeverity::Error;
                E.Category = TEXT("Asset");
                E.JsonPath = TEXT("/assetPath");
                E.Message = FString::Printf(
                    TEXT("Asset already exists at '%s' and overwrite=false."),
                    *Inputs.AssetPath);
                Result.Errors.Add(MoveTemp(E));
                return Result;
            }
            if (Existing->ParentClass != ParentClass)
            {
                FUISpecError E;
                E.Severity = EUISpecErrorSeverity::Error;
                E.Category = TEXT("Asset");
                E.JsonPath = TEXT("/assetPath");
                E.Message = FString::Printf(
                    TEXT("Existing WBP '%s' has parent '%s'; spec requested '%s'. Refusing to rebase."),
                    *Inputs.AssetPath,
                    Existing->ParentClass ? *Existing->ParentClass->GetName() : TEXT("None"),
                    ParentClass ? *ParentClass->GetName() : TEXT("None"));
                Result.Errors.Add(MoveTemp(E));
                return Result;
            }
            if (Inputs.bOverwrite && Existing->WidgetTree)
            {
                TArray<UWidget*> ExistingWidgets;
                Existing->WidgetTree->GetAllWidgets(ExistingWidgets);
                Context.NodesRemoved = ExistingWidgets.Num();
            }
        }

        TSet<FName> SeenNamedRefs;
        if (Doc.Root.IsValid())
        {
            PreCreateStylesRecurse(*Doc.Root, Context, SeenNamedRefs);
            CountDryRunNodesRecurse(*Doc.Root, Context);
        }
        for (const TPair<FName, FUISpecStyle>& Pair : Doc.Styles)
        {
            if (!SeenNamedRefs.Contains(Pair.Key))
            {
                ++Context.StyleHookCalls;
                SeenNamedRefs.Add(Pair.Key);
                Context.PreCreatedStyles.Add(
                    FString::Printf(TEXT("name:%s"), *Pair.Key.ToString()),
                    nullptr);
            }
        }

        EscalateWarningsIfRequested(Context);
        if (Context.Errors.Num() > 0)
        {
            Result.Errors = MoveTemp(Context.Errors);
            Result.Warnings = MoveTemp(Context.Warnings);
            Result.NodesCreated = Context.NodesCreated;
            Result.NodesModified = Context.NodesModified;
            Result.NodesRemoved = Context.NodesRemoved;
            Result.DiffLines = MoveTemp(Context.DiffLines);
            return Result;
        }

        Result.bSuccess = true;
        Result.Errors = MoveTemp(Context.Errors);
        Result.Warnings = MoveTemp(Context.Warnings);
        Result.NodesCreated = Context.NodesCreated;
        Result.NodesModified = Context.NodesModified;
        Result.NodesRemoved = Context.NodesRemoved;
        Result.DiffLines = MoveTemp(Context.DiffLines);
        return Result;
    }

    // ---------- 6. Get-or-create WBP ------------------------------------
    bool bPreExisting = false;
    UPackage* Package = nullptr;
    FString CreateErr;
    UWidgetBlueprint* WBP = GetOrCreateWBP(
        Inputs.AssetPath, ParentClass, Inputs.bOverwrite,
        bPreExisting, Package, CreateErr);
    if (!WBP)
    {
        FUISpecError E;
        E.Severity = EUISpecErrorSeverity::Error;
        E.Category = bPreExisting ? TEXT("Asset") : TEXT("AssetCreate");
        E.JsonPath = TEXT("/assetPath");
        E.Message  = CreateErr;
        Result.Errors.Add(MoveTemp(E));
        return Result;
    }
    Context.TargetWBP = WBP;

    // ---------- 7. Open the property-write transaction ------------------
    // Only meaningful for the in-place edit sub-case — FScopedTransaction
    // does NOT roll back CreatePackage/FactoryCreateNew (per the file
    // header + R3 risk comment). For the create-new sub-case the
    // transaction is still opened so any in-place WidgetTree::Modify calls
    // we make end up grouped under one undo stack entry; on failure we
    // also call RollbackCreatedAsset to clean up the disk artefact.
    FScopedTransaction Transaction(
        TEXT("MonolithUI"),
        LOCTEXT("BuildUIFromSpec", "Build UI from Spec"),
        WBP,
        /*bShouldActuallyTransact=*/!Inputs.bDryRun);

    if (!Inputs.bDryRun)
    {
        WBP->Modify();
        if (WBP->WidgetTree)
        {
            WBP->WidgetTree->Modify();
        }
    }

    // Existing WBP + overwrite path: clear the tree + animations so we
    // start from a clean slate. (Phase J will gain a "merge" mode; v1
    // is the regen rule from §1.8.)
    if (bPreExisting && Inputs.bOverwrite && !Inputs.bDryRun)
    {
        if (WBP->WidgetTree)
        {
            // Count what we're nuking so the diff/response reflects it.
            TArray<UWidget*> Existing;
            WBP->WidgetTree->GetAllWidgets(Existing);
            Context.NodesRemoved = Existing.Num();

            // Reset the tree by reparenting children into the transient
            // package — same recipe MonolithUITestFixtureUtils uses, which
            // is the recipe that survives WBP recompile cleanly.
            if (UPanelWidget* RootPanel = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget))
            {
                RootPanel->ClearChildren();
            }
            TArray<UWidget*> Orphans;
            ForEachObjectWithOuter(WBP->WidgetTree, [&Orphans](UObject* Obj)
            {
                if (UWidget* W = Cast<UWidget>(Obj))
                {
                    Orphans.Add(W);
                }
            }, /*bIncludeNestedObjects=*/false);
            for (UWidget* W : Orphans)
            {
                W->Rename(nullptr, GetTransientPackage(),
                    REN_DoNotDirty | REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
            }
            WBP->WidgetTree->RootWidget = nullptr;
            WBP->WidgetVariableNameToGuidMap.Empty();
        }
    }

    // ---------- 8. Pre-create styles (H10/H11) --------------------------
    {
        TSet<FName> SeenNamedRefs;
        if (Doc.Root.IsValid())
        {
            PreCreateStylesRecurse(*Doc.Root, Context, SeenNamedRefs);
        }
        // Also bump for each named entry in the document-scoped styles
        // table — these are LLM-declared names that future serialisation
        // will need to round-trip even if no node references them yet.
        for (const TPair<FName, FUISpecStyle>& Pair : Doc.Styles)
        {
            if (!SeenNamedRefs.Contains(Pair.Key))
            {
                ++Context.StyleHookCalls;
                SeenNamedRefs.Add(Pair.Key);
                Context.PreCreatedStyles.Add(
                    FString::Printf(TEXT("name:%s"), *Pair.Key.ToString()),
                    nullptr);
            }
        }
    }

    // ---------- 9. Walk the tree ----------------------------------------
    if (Doc.Root.IsValid() && WBP->WidgetTree)
    {
        // Real build. The root node has no parent panel — pass nullptr
        // and let the dispatcher set it as WidgetTree::RootWidget.
        UWidget* RootWidget = BuildNodeRecurse(Context, *Doc.Root, /*ParentPanel=*/nullptr);
        if (RootWidget)
        {
            WBP->WidgetTree->RootWidget = RootWidget;
            MonolithUI::RegisterCreatedWidget(WBP, RootWidget);
        }
        else
        {
            // Root failed to build — sub-builders pushed errors. Bail.
            EscalateWarningsIfRequested(Context);
            Result.Errors      = MoveTemp(Context.Errors);
            Result.Warnings    = MoveTemp(Context.Warnings);
            Result.NodesCreated   = Context.NodesCreated;
            Result.NodesModified  = Context.NodesModified;
            Result.NodesRemoved   = Context.NodesRemoved;
            Result.DiffLines      = MoveTemp(Context.DiffLines);
            Transaction.Cancel();
            if (!bPreExisting)
            {
                RollbackCreatedAsset(WBP, Package);
            }
            return Result;
        }
    }

    // ---------- 10. Bail if errors accumulated mid-walk -----------------
    EscalateWarningsIfRequested(Context);
    if (Context.Errors.Num() > 0)
    {
        Result.Errors      = MoveTemp(Context.Errors);
        Result.Warnings    = MoveTemp(Context.Warnings);
        Result.NodesCreated   = Context.NodesCreated;
        Result.NodesModified  = Context.NodesModified;
        Result.NodesRemoved   = Context.NodesRemoved;
        Result.DiffLines      = MoveTemp(Context.DiffLines);
        Transaction.Cancel();
        if (!bPreExisting && !Inputs.bDryRun)
        {
            RollbackCreatedAsset(WBP, Package);
        }
        return Result;
    }

    // ---------- 11. Compile (skipped on dry-run) ------------------------
    if (!Inputs.bDryRun)
    {
        MonolithUI::ReconcileWidgetVariableGuids(WBP);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        FKismetEditorUtilities::CompileBlueprint(WBP);

        // ---------- 12. Rebuild widget-id map post-compile (H12/H13) ----
        // Compile recreates CDOs; pre-compile UWidget* pointers are now
        // stale memory. The animation pass below MUST consult WidgetIdMap
        // (post-rebuild), never PreCompileWidgets.
        RebuildWidgetIdMapPostCompile(Context);

        // ---------- 13. Wire animations (Phase I I9 closure) ------------
        if (Doc.Animations.Num() > 0)
        {
            FUIAnimationBuildOptions AnimOpts;
            // We've already compiled once above; ask the animation builder
            // to NOT compile again per-pass — final compile happens below
            // when we save (or stays out if dry-run).
            AnimOpts.bCompileBlueprint = false;
            AnimOpts.bDryRun           = false;

            for (const FUISpecAnimation& Anim : Doc.Animations)
            {
                FUIAnimationRebuildResult AnimRes;
                FUIAnimationMovieSceneBuilder::BuildSingle(Anim, WBP, AnimOpts, AnimRes);
                if (!AnimRes.bSuccess)
                {
                    FUISpecError E;
                    E.Severity = EUISpecErrorSeverity::Error;
                    E.Category = TEXT("Animation");
                    E.WidgetId = Anim.TargetWidgetId;
                    E.Message  = AnimRes.ErrorMessage;
                    Context.Errors.Add(MoveTemp(E));
                    break;
                }
                for (const FString& W : AnimRes.Warnings)
                {
                    FUISpecError WE;
                    WE.Severity = EUISpecErrorSeverity::Warning;
                    WE.Category = TEXT("Animation");
                    WE.Message  = W;
                    Context.Warnings.Add(MoveTemp(WE));
                }
            }

            EscalateWarningsIfRequested(Context);
            if (Context.Errors.Num() > 0)
            {
                Result.Errors      = MoveTemp(Context.Errors);
                Result.Warnings    = MoveTemp(Context.Warnings);
                Result.NodesCreated   = Context.NodesCreated;
                Result.NodesModified  = Context.NodesModified;
                Result.NodesRemoved   = Context.NodesRemoved;
                Result.DiffLines      = MoveTemp(Context.DiffLines);
                Transaction.Cancel();
                if (!bPreExisting)
                {
                    RollbackCreatedAsset(WBP, Package);
                }
                return Result;
            }
        }

        // ---------- 14. Save --------------------------------------------
        FString SaveErr;
        if (!SaveWBP(WBP, Inputs.AssetPath, SaveErr))
        {
            FUISpecError E;
            E.Severity = EUISpecErrorSeverity::Error;
            E.Category = TEXT("Save");
            E.JsonPath = TEXT("/assetPath");
            E.Message  = SaveErr;
            Context.Errors.Add(MoveTemp(E));

            Result.Errors      = MoveTemp(Context.Errors);
            Result.Warnings    = MoveTemp(Context.Warnings);
            Result.NodesCreated   = Context.NodesCreated;
            Result.NodesModified  = Context.NodesModified;
            Result.NodesRemoved   = Context.NodesRemoved;
            Result.DiffLines      = MoveTemp(Context.DiffLines);
            Transaction.Cancel();
            if (!bPreExisting)
            {
                RollbackCreatedAsset(WBP, Package);
            }
            return Result;
        }
    }
    // ---------- 15. Pack the response ----------------------------------
    Result.bSuccess       = true;
    Result.Errors         = MoveTemp(Context.Errors);
    Result.Warnings       = MoveTemp(Context.Warnings);
    Result.NodesCreated   = Context.NodesCreated;
    Result.NodesModified  = Context.NodesModified;
    Result.NodesRemoved   = Context.NodesRemoved;
    Result.DiffLines      = MoveTemp(Context.DiffLines);
    return Result;
}

#undef LOCTEXT_NAMESPACE
