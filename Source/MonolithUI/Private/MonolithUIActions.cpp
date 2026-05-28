// MonolithUIActions.cpp
#include "MonolithUIActions.h"
#include "MonolithUIInternal.h"
#include "MonolithParamSchema.h"
#include "MonolithPackagePathValidator.h"
#include "WidgetBlueprintFactory.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"

// Phase C: set_widget_property routes through the allowlist-gated reflection
// helper. The legacy bare-FProperty::ImportText_Direct path is preserved
// behind the `raw_mode=true` opt-out so existing call sites that previously
// relied on writing arbitrary properties unconditionally still work.
#include "Editor.h"
#include "Registry/MonolithUIRegistrySubsystem.h"
#include "Registry/UIPropertyAllowlist.h"
#include "Registry/UIPropertyPathCache.h"
#include "Registry/UIReflectionHelper.h"

// Bug #5 fix (2026-05-16 UI gap audit): compile_widget now surfaces
// FCompilerResultsLog messages as errors[]/warnings[] arrays in the response
// payload, matching the shape blueprint_query("compile_blueprint") returns.
// FCompilerResultsLog is the canonical channel — IWidgetCompilerLog (which
// UCommonBoundActionBar::ValidateCompiledDefaults writes through) routes back
// into the same results log for widget BPs, so capturing here covers both
// the compiler-graph errors AND the validator-emitted ones.
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"

// Phase 2 (2026-05-16 UI gap audit): rename_widget + dump_blueprint_compile_log.
// rename_widget recompiles via FBlueprintCompilationManager so the Skeleton class
// stamping (BPVAR rename in NewVariables[]) survives the post-compile reflection
// walk that get_widget_tree / set_widget_property uses.
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/Blueprint.h"

// Phase 2 (Item #7 / #14) — file-static handler forward declarations. Lives
// here rather than as static members on FMonolithUIActions because the plan
// (Phase 2 §F6) prohibits header changes; we register through the class's
// existing RegisterActions hook and dispatch into these file-static handlers.
namespace MonolithUIActionsPhase2
{
    static FMonolithActionResult HandleRenameWidget(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleDumpBlueprintCompileLog(const TSharedPtr<FJsonObject>& Params);
}

void FMonolithUIActions::RegisterActions(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("ui"), TEXT("create_widget_blueprint"),
        TEXT("Create a UWidgetBlueprint at save_path. parent_class accepts /Script/Module.Class form OR short name ('TokenforgeActivatableWidget', 'CommonActivatableWidget', 'UserWidget') — short name resolves via UClass::FindClassByName."),
        FMonolithActionHandler::CreateStatic(&HandleCreateWidgetBlueprint),
        FParamSchemaBuilder()
            .Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path, e.g. /Game/UI/WBP_MyWidget"))
            .Optional(TEXT("parent_class"), TEXT("string"), TEXT("Parent class name (default: UserWidget)"), TEXT("UserWidget"))
            .Optional(TEXT("root_widget"), TEXT("string"), TEXT("Root widget type (default: CanvasPanel)"), TEXT("CanvasPanel"))
            .Optional(TEXT("skip_save"), TEXT("boolean"), TEXT("Skip saving to disk"), TEXT("false"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("get_widget_tree"),
        TEXT("Get the full widget hierarchy of a Widget Blueprint as JSON"),
        FMonolithActionHandler::CreateStatic(&HandleGetWidgetTree),
        FParamSchemaBuilder()
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("add_widget"),
        TEXT("Add a widget to a parent panel in a Widget Blueprint"),
        FMonolithActionHandler::CreateStatic(&HandleAddWidget),
        FParamSchemaBuilder()
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_class"), TEXT("string"), TEXT("Widget class: TextBlock, Image, Button, VerticalBox, etc."))
            .Optional(TEXT("widget_name"), TEXT("string"), TEXT("Name for the new widget (auto-generated if omitted)"))
            .Optional(TEXT("parent_name"), TEXT("string"), TEXT("Parent widget name (default: root widget)"))
            .Optional(TEXT("anchor_preset"), TEXT("string"), TEXT("Anchor preset: center, top_left, stretch_fill, etc."))
            .Optional(TEXT("position"), TEXT("object"), TEXT("Canvas position: {\"x\": 0, \"y\": 0}"))
            .Optional(TEXT("size"), TEXT("object"), TEXT("Canvas size: {\"x\": 200, \"y\": 50}"))
            .Optional(TEXT("padding"), TEXT("object"), TEXT("Slot padding: {\"left\":0,\"top\":0,\"right\":0,\"bottom\":0}"))
            .Optional(TEXT("h_align"), TEXT("string"), TEXT("Horizontal alignment: Left, Center, Right, Fill"))
            .Optional(TEXT("v_align"), TEXT("string"), TEXT("Vertical alignment: Top, Center, Bottom, Fill"))
            .Optional(TEXT("auto_size"), TEXT("boolean"), TEXT("Auto-size in canvas slot"), TEXT("false"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after adding"), TEXT("true"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("remove_widget"),
        TEXT("Remove a widget from a Widget Blueprint"),
        FMonolithActionHandler::CreateStatic(&HandleRemoveWidget),
        FParamSchemaBuilder()
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of the widget to remove"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after removing"), TEXT("true"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_widget_property"),
        TEXT("Set a property on a widget (text, color, opacity, visibility, etc.). Default mode gates writes through the per-type curated allowlist; pass raw_mode=true to bypass the gate (legacy compat). The new value can be supplied as `value` OR the alias `property_value` (Bug #6 fix)."),
        FMonolithActionHandler::CreateStatic(&HandleSetWidgetProperty),
        FParamSchemaBuilder()
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Target widget name"))
            .Required(TEXT("property_name"), TEXT("string"), TEXT("Property path. Dotted segments allowed (e.g. 'Padding.Left'). Allowlist-gated unless raw_mode=true."))
            .Required(TEXT("value"), TEXT("string"), TEXT("Property value (alias: 'property_value'). Strings, numbers, booleans, JSON arrays/objects all accepted; struct types (Vector2D/LinearColor/Margin/Vector4/SlateColor) accept multiple shapes."))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after setting"), TEXT("false"))
            .Optional(TEXT("raw_mode"), TEXT("boolean"), TEXT("Bypass the allowlist gate (legacy unconditional ImportText_Direct path). Default false."), TEXT("false"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("compile_widget"),
        TEXT("Compile a Widget Blueprint. Response includes errors[] and warnings[] arrays populated when status=BS_Error (added v0.14.11)."),
        FMonolithActionHandler::CreateStatic(&HandleCompileWidget),
        FParamSchemaBuilder()
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("list_widget_types"),
        TEXT("List all available widget class types that can be added"),
        FMonolithActionHandler::CreateStatic(&HandleListWidgetTypes),
        FParamSchemaBuilder()
            .Optional(TEXT("filter"), TEXT("string"), TEXT("Filter by category: panel, leaf, input, display, layout"))
            .Build()
    );

    // Phase 2 Item #7 (2026-05-16 UI gap audit): rename a widget in-place.
    // Recompiles via FKismetEditorUtilities::CompileBlueprint so the structural
    // modification + the Skeleton class refresh + the post-compile reflection
    // walk all see the new FName.
    Registry.RegisterAction(
        TEXT("ui"), TEXT("rename_widget"),
        TEXT("Rename a UWidget's FName in a WBP's tree; updates slot references and recompiles. "
             "Uniqueness check runs against the full WidgetTree before the rename — colliding new_name returns -32602."),
        FMonolithActionHandler::CreateStatic(&MonolithUIActionsPhase2::HandleRenameWidget),
        FParamSchemaBuilder()
            .Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path (alias: asset_path)"))
            .Required(TEXT("old_name"), TEXT("string"), TEXT("Current widget FName"))
            .Required(TEXT("new_name"), TEXT("string"), TEXT("Target widget FName (must be unique in tree)"))
            .Build(),
        TEXT("WidgetCRUD")
    );

    // Phase 2 Item #14 (2026-05-16 UI gap audit): re-compile a blueprint and
    // return the last_compile_status (EBlueprintStatus → string) + the
    // FCompilerResultsLog errors[]/warnings[]/notes[]. Phase 1's HandleCompileWidget
    // captures the log on every call but does not cache it on the asset — so a
    // dump call drives a fresh compile to harvest the messages. Shape mirrors
    // blueprint_query("compile_blueprint") for parser reuse.
    Registry.RegisterAction(
        TEXT("ui"), TEXT("dump_blueprint_compile_log"),
        TEXT("Run a fresh compile and return last_compile_status + errors[]/warnings[]/notes[]. "
             "Same shape as compile_widget on success; useful when a prior call did not retain its log "
             "(orchestrator did not parse the response, retried later, etc.). Accepts UWidgetBlueprint OR "
             "UBlueprint paths — the action sniffs the type and reads ::Status accordingly."),
        FMonolithActionHandler::CreateStatic(&MonolithUIActionsPhase2::HandleDumpBlueprintCompileLog),
        FParamSchemaBuilder()
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint or Widget Blueprint asset path"))
            .Build(),
        TEXT("WidgetCRUD")
    );
}

// --- create_widget_blueprint ---
FMonolithActionResult FMonolithUIActions::HandleCreateWidgetBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult ParamError;
    FString SavePath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("save_path"), SavePath, ParamError))
    {
        return ParamError;
    }

    // Defensive: reject malformed paths (e.g. "//Game/...") before they reach CreatePackage,
    // which asserts in UObjectGlobals.cpp and kills the editor.
    if (const FString ValidationError = MonolithCore::ValidatePackagePath(SavePath); !ValidationError.IsEmpty())
    {
        return FMonolithActionResult::Error(ValidationError);
    }

    FString ParentClassName = MonolithUIInternal::GetOptionalString(Params, TEXT("parent_class"));
    if (ParentClassName.IsEmpty()) ParentClassName = TEXT("UserWidget");

    FString RootWidgetType = MonolithUIInternal::GetOptionalString(Params, TEXT("root_widget"));
    if (RootWidgetType.IsEmpty()) RootWidgetType = TEXT("CanvasPanel");

    const bool bSkipSave = MonolithUIInternal::GetOptionalBool(Params, TEXT("skip_save"), false);

    // Resolve parent class
    UClass* ParentClass = FindFirstObject<UClass>(*ParentClassName, EFindFirstObjectOptions::NativeFirst);
    if (!ParentClass)
    {
        ParentClass = FindFirstObject<UClass>(*(TEXT("U") + ParentClassName), EFindFirstObjectOptions::NativeFirst);
    }
    if (!ParentClass || !ParentClass->IsChildOf(UUserWidget::StaticClass()))
    {
        // Phase K — surface the failure in FUISpecError shape so the LLM gets
        // category/json_path/suggested_fix/valid_options fields. The valid_options
        // list intentionally enumerates only the common parent classes (the
        // FindFirstObject path supports any UUserWidget subclass — listing
        // every BP-derived UserWidget would explode).
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("Type"),
            TEXT("/parent_class"),
            FString::Printf(TEXT("Parent class '%s' not found or not a UUserWidget subclass."), *ParentClassName),
            TEXT("Use a token (UserWidget / CommonActivatableWidget / CommonUserWidget) or a full /Script/Module.Class path."),
            { TEXT("UserWidget"), TEXT("CommonActivatableWidget"), TEXT("CommonUserWidget") }));
    }

    // Create package
    FString PackagePath, AssetName;
    SavePath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
    if (AssetName.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Invalid save_path — must contain at least one / separator"));
    }

    UPackage* Package = CreatePackage(*SavePath);
    if (!Package)
    {
        // Phase K — internal error (-32603), not invalid-params: the path passed
        // earlier validation but the engine refused. Caller can't fix this from
        // their end without changing the path.
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("AssetCreate"),
            TEXT("/save_path"),
            FString::Printf(TEXT("CreatePackage failed for '%s'."), *SavePath),
            TEXT("Verify the path is writeable and not in use by the editor.")), -32603);
    }

    // Fail cleanly if the asset already exists instead of letting FactoryCreateNew assert.
    if (FindObject<UObject>(Package, *AssetName))
    {
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("AssetExists"),
            TEXT("/save_path"),
            FString::Printf(TEXT("Widget Blueprint already exists at '%s'."), *SavePath),
            TEXT("Pick a different save_path, or delete the existing asset first.")));
    }

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *SavePath, *AssetName);
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    if (AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid())
    {
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("AssetExists"),
            TEXT("/save_path"),
            FString::Printf(TEXT("Widget Blueprint already exists at '%s' (asset registry)."), *SavePath),
            TEXT("Pick a different save_path, or delete the existing asset first.")));
    }

    // Create widget blueprint via factory
    UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
    Factory->BlueprintType = BPTYPE_Normal;
    Factory->ParentClass = ParentClass;

    UObject* CreatedObj = Factory->FactoryCreateNew(
        UWidgetBlueprint::StaticClass(), Package,
        FName(*AssetName), RF_Public | RF_Standalone,
        nullptr, GWarn);

    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(CreatedObj);
    if (!WBP)
    {
        return FMonolithActionResult::Error(TEXT("UWidgetBlueprintFactory::FactoryCreateNew returned null"));
    }

    // Set root widget if tree is empty
    if (WBP->WidgetTree && !WBP->WidgetTree->RootWidget)
    {
        UClass* RootClass = MonolithUIInternal::WidgetClassFromName(RootWidgetType);
        if (RootClass && RootClass->IsChildOf(UPanelWidget::StaticClass()))
        {
            UWidget* Root = WBP->WidgetTree->ConstructWidget<UWidget>(RootClass, FName(*RootWidgetType));
            WBP->WidgetTree->RootWidget = Root;
            MonolithUIInternal::RegisterCreatedWidget(WBP, Root);
        }
    }

    // Compile
    MonolithUIInternal::ReconcileWidgetVariableGuids(WBP);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);

    // Save
    if (!bSkipSave)
    {
        FAssetRegistryModule::AssetCreated(WBP);
        Package->MarkPackageDirty();
        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        UPackage::SavePackage(Package, WBP,
            *FPackageName::LongPackageNameToFilename(SavePath, FPackageName::GetAssetPackageExtension()),
            SaveArgs);
    }

    // Build result
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), SavePath);
    Result->SetStringField(TEXT("asset_name"), AssetName);
    Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
    Result->SetStringField(TEXT("root_widget"), RootWidgetType);
    Result->SetBoolField(TEXT("compiled"), true);
    Result->SetBoolField(TEXT("saved"), !bSkipSave);

    return FMonolithActionResult::Success(Result);
}

// --- get_widget_tree ---
FMonolithActionResult FMonolithUIActions::HandleGetWidgetTree(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("missing required asset_path parameter"));
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    if (!WBP->WidgetTree)
    {
        return FMonolithActionResult::Error(TEXT("WidgetTree is null (editor-only data not available)"));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("parent_class"), WBP->ParentClass ? WBP->ParentClass->GetName() : TEXT("None"));

    // Serialize root
    if (WBP->WidgetTree->RootWidget)
    {
        Result->SetObjectField(TEXT("root"), MonolithUIInternal::SerializeWidget(WBP->WidgetTree->RootWidget));
    }

    // Widget count
    TArray<UWidget*> AllWidgets;
    WBP->WidgetTree->GetAllWidgets(AllWidgets);
    Result->SetNumberField(TEXT("widget_count"), AllWidgets.Num());

    // Animations
    TArray<TSharedPtr<FJsonValue>> AnimArray;
    for (UWidgetAnimation* Anim : WBP->Animations)
    {
        if (Anim)
        {
            TSharedPtr<FJsonObject> AnimObj = MakeShared<FJsonObject>();
            AnimObj->SetStringField(TEXT("name"), Anim->GetName());
            AnimObj->SetNumberField(TEXT("start_time"), Anim->GetStartTime());
            AnimObj->SetNumberField(TEXT("end_time"), Anim->GetEndTime());
            AnimArray.Add(MakeShared<FJsonValueObject>(AnimObj));
        }
    }
    if (AnimArray.Num() > 0)
    {
        Result->SetArrayField(TEXT("animations"), AnimArray);
    }

    return FMonolithActionResult::Success(Result);
}

// --- add_widget ---
FMonolithActionResult FMonolithUIActions::HandleAddWidget(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
    {
        return ParamError;
    }

    FString WidgetClassName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_class"), WidgetClassName, ParamError))
    {
        return ParamError;
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;
    if (!WBP->WidgetTree)
    {
        return FMonolithActionResult::Error(TEXT("WidgetTree is null"));
    }

    // Resolve widget class
    UClass* WidgetClass = MonolithUIInternal::WidgetClassFromName(WidgetClassName);
    if (!WidgetClass)
    {
        // Phase K — surface as FUISpecError. Common widget tokens go in
        // valid_options as a starter list; the full surface lives behind
        // ui::list_widget_types (referenced in suggested_fix).
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("Type"),
            TEXT("/widget_class"),
            FString::Printf(TEXT("Unknown widget class token: '%s'."), *WidgetClassName),
            TEXT("Call ui::list_widget_types for the full registered set, or pass a /Script/UMG.Class path."),
            { TEXT("CanvasPanel"), TEXT("VerticalBox"), TEXT("HorizontalBox"), TEXT("Overlay"),
              TEXT("TextBlock"), TEXT("RichTextBlock"), TEXT("Image"), TEXT("Button"),
              TEXT("Border"), TEXT("SizeBox"), TEXT("ProgressBar"), TEXT("CheckBox"),
              TEXT("Slider"), TEXT("EditableText"), TEXT("EditableTextBox"), TEXT("ScrollBox") }));
    }

    // Widget name
    FString WidgetName = MonolithUIInternal::GetOptionalString(Params, TEXT("widget_name"));
    FName WidgetFName = WidgetName.IsEmpty() ? NAME_None : FName(*WidgetName);

    // Find parent widget
    UPanelWidget* ParentPanel = nullptr;
    FString ParentName = MonolithUIInternal::GetOptionalString(Params, TEXT("parent_name"));
    if (ParentName.IsEmpty())
    {
        ParentPanel = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget);
    }
    else
    {
        UWidget* Found = WBP->WidgetTree->FindWidget(FName(*ParentName));
        ParentPanel = Cast<UPanelWidget>(Found);
    }

    if (!ParentPanel)
    {
        // Phase K — Lookup-class error. Cannot enumerate live widget names in
        // valid_options without scanning the WidgetTree (the suggested_fix
        // points the LLM at get_widget_tree for that lookup).
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("Lookup"),
            TEXT("/parent_name"),
            FString::Printf(TEXT("Parent '%s' not found or is not a panel widget."), *ParentName),
            TEXT("Call ui::get_widget_tree to enumerate live widget names; the parent must be a UPanelWidget subclass.")));
    }

    // Construct widget
    UWidget* NewWidget = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, WidgetFName);
    if (!NewWidget)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Failed to construct widget of class %s"), *WidgetClassName));
    }

    // Add to parent.
    //
    // UPanelWidget::AddChild returns nullptr in three conditions
    // (Engine/Source/Runtime/UMG/Private/Components/PanelWidget.cpp:132-142):
    //   1. Content is null — impossible here; NewWidget was just constructed.
    //   2. !bCanHaveMultipleChildren && GetChildrenCount() > 0 — the single-child
    //      invariant on UContentWidget subclasses (Border, RoundedBorder,
    //      Button, SizeBox, ScaleBox, BackgroundBlur, InvalidationBox,
    //      RetainerBox, SafeZone, NamedSlot).
    //   3. (Subclass-specific rejections via OnSlotAdded, rare in practice.)
    //
    // When case 2 fires, callers routinely waste time staring at the opaque
    // message before realizing a VerticalBox/HorizontalBox wrapper is missing.
    // Classify it here so the error speaks for itself.
    UPanelSlot* Slot = ParentPanel->AddChild(NewWidget);
    if (!Slot)
    {
        if (!ParentPanel->CanHaveMultipleChildren() && ParentPanel->GetChildrenCount() > 0)
        {
            UWidget* ExistingChild = ParentPanel->GetChildAt(0);
            const FString ExistingName = ExistingChild ? ExistingChild->GetName() : TEXT("<unknown>");
            return FMonolithActionResult::Error(FString::Printf(
                TEXT("AddChild failed: parent '%s' is a single-child container (%s) and already holds '%s'. ")
                TEXT("Wrap additional children in a VerticalBox/HorizontalBox."),
                *ParentPanel->GetName(),
                *ParentPanel->GetClass()->GetName(),
                *ExistingName));
        }
        return FMonolithActionResult::Error(TEXT("AddChild returned null slot"));
    }

    // Configure canvas slot if applicable
    if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(Slot))
    {
        // Anchor preset
        FString AnchorPreset = MonolithUIInternal::GetOptionalString(Params, TEXT("anchor_preset"));
        if (!AnchorPreset.IsEmpty())
        {
            CSlot->SetAnchors(MonolithUIInternal::GetAnchorPreset(AnchorPreset));
        }

        // Position
        const TSharedPtr<FJsonObject>* PosObj = nullptr;
        if (Params->TryGetObjectField(TEXT("position"), PosObj))
        {
            FVector2D Pos((*PosObj)->GetNumberField(TEXT("x")), (*PosObj)->GetNumberField(TEXT("y")));
            CSlot->SetPosition(Pos);
        }

        // Size
        const TSharedPtr<FJsonObject>* SizeObj = nullptr;
        if (Params->TryGetObjectField(TEXT("size"), SizeObj))
        {
            FVector2D Size((*SizeObj)->GetNumberField(TEXT("x")), (*SizeObj)->GetNumberField(TEXT("y")));
            CSlot->SetSize(Size);
        }

        // Auto-size
        CSlot->SetAutoSize(MonolithUIInternal::GetOptionalBool(Params, TEXT("auto_size"), CSlot->GetAutoSize()));
    }

    // Configure box/overlay slot alignment
    FString HAlign = MonolithUIInternal::GetOptionalString(Params, TEXT("h_align"));
    FString VAlign = MonolithUIInternal::GetOptionalString(Params, TEXT("v_align"));

    if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot))
    {
        if (!HAlign.IsEmpty())
        {
            EHorizontalAlignment HA = HAlign == TEXT("Left") ? HAlign_Left :
                                      HAlign == TEXT("Center") ? HAlign_Center :
                                      HAlign == TEXT("Right") ? HAlign_Right : HAlign_Fill;
            VS->SetHorizontalAlignment(HA);
        }
        if (!VAlign.IsEmpty())
        {
            EVerticalAlignment VA = VAlign == TEXT("Top") ? VAlign_Top :
                                    VAlign == TEXT("Center") ? VAlign_Center :
                                    VAlign == TEXT("Bottom") ? VAlign_Bottom : VAlign_Fill;
            VS->SetVerticalAlignment(VA);
        }
    }
    else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot))
    {
        if (!HAlign.IsEmpty())
        {
            EHorizontalAlignment HA = HAlign == TEXT("Left") ? HAlign_Left :
                                      HAlign == TEXT("Center") ? HAlign_Center :
                                      HAlign == TEXT("Right") ? HAlign_Right : HAlign_Fill;
            HS->SetHorizontalAlignment(HA);
        }
        if (!VAlign.IsEmpty())
        {
            EVerticalAlignment VA = VAlign == TEXT("Top") ? VAlign_Top :
                                    VAlign == TEXT("Center") ? VAlign_Center :
                                    VAlign == TEXT("Bottom") ? VAlign_Bottom : VAlign_Fill;
            HS->SetVerticalAlignment(VA);
        }
    }
    else if (UOverlaySlot* OS = Cast<UOverlaySlot>(Slot))
    {
        if (!HAlign.IsEmpty())
        {
            EHorizontalAlignment HA = HAlign == TEXT("Left") ? HAlign_Left :
                                      HAlign == TEXT("Center") ? HAlign_Center :
                                      HAlign == TEXT("Right") ? HAlign_Right : HAlign_Fill;
            OS->SetHorizontalAlignment(HA);
        }
        if (!VAlign.IsEmpty())
        {
            EVerticalAlignment VA = VAlign == TEXT("Top") ? VAlign_Top :
                                    VAlign == TEXT("Center") ? VAlign_Center :
                                    VAlign == TEXT("Bottom") ? VAlign_Bottom : VAlign_Fill;
            OS->SetVerticalAlignment(VA);
        }
    }

    // Padding
    const TSharedPtr<FJsonObject>* PadObj = nullptr;
    if (Params->TryGetObjectField(TEXT("padding"), PadObj))
    {
        FMargin Pad(
            (*PadObj)->GetNumberField(TEXT("left")),
            (*PadObj)->GetNumberField(TEXT("top")),
            (*PadObj)->GetNumberField(TEXT("right")),
            (*PadObj)->GetNumberField(TEXT("bottom"))
        );
        if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot)) VS->SetPadding(Pad);
        else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot)) HS->SetPadding(Pad);
        else if (UOverlaySlot* OS = Cast<UOverlaySlot>(Slot)) OS->SetPadding(Pad);
    }

    // Mirror editor bookkeeping so the compiler sees a GUID for the final widget name.
    MonolithUIInternal::RegisterCreatedWidget(WBP, NewWidget);

    // Mark modified
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

    // Compile if requested
    const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(WBP);
    }

    // Build result
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("widget_name"), NewWidget->GetName());
    Result->SetStringField(TEXT("widget_class"), WidgetClassName);
    Result->SetStringField(TEXT("parent_name"), ParentPanel->GetName());
    Result->SetStringField(TEXT("slot_type"), Slot->GetClass()->GetName());
    Result->SetBoolField(TEXT("compiled"), bCompile);

    return FMonolithActionResult::Success(Result);
}

// --- remove_widget ---
FMonolithActionResult FMonolithUIActions::HandleRemoveWidget(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    FString WidgetName = Params->GetStringField(TEXT("widget_name"));
    if (WidgetName.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Missing required param: widget_name"));
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
    if (!Widget)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Widget '%s' not found in widget tree"), *WidgetName));
    }

    // Cannot remove root
    if (Widget == WBP->WidgetTree->RootWidget)
    {
        return FMonolithActionResult::Error(TEXT("Cannot remove the root widget"));
    }

    TSet<UWidget*> WidgetsToDelete;
    WidgetsToDelete.Add(Widget);
    FWidgetBlueprintEditorUtils::DeleteWidgets(WBP, WidgetsToDelete, FWidgetBlueprintEditorUtils::EDeleteWidgetWarningType::DeleteSilently);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

    bool bCompile = true;
    bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);
    if (bCompile) FKismetEditorUtilities::CompileBlueprint(WBP);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("removed"), WidgetName);
    Result->SetBoolField(TEXT("compiled"), bCompile);
    return FMonolithActionResult::Success(Result);
}

// --- set_widget_property ---
//
// Phase C rewrite (2026-04-26): the handler now routes through
// FUIReflectionHelper. Default mode (`raw_mode=false`) gates the write through
// the per-type curated allowlist on FUIPropertyAllowlist; `raw_mode=true`
// preserves the legacy bare-FProperty::ImportText_Direct path so any existing
// caller that previously wrote arbitrary properties unconditionally keeps
// working by adding `raw_mode=true` to its parameter dictionary.
//
// Value handling: the action schema declares `value` as type "string", but
// the wire payload is a TSharedPtr<FJsonValue> — callers can supply numbers,
// booleans, arrays, objects, and the helper dispatches on FProperty kind.
// We grab the field via TryGetField (not GetStringField) so non-string JSON
// shapes survive.
FMonolithActionResult FMonolithUIActions::HandleSetWidgetProperty(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FMonolithActionResult::Error(TEXT("set_widget_property: Params is null"));
    }

    FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    FString WidgetName = Params->GetStringField(TEXT("widget_name"));
    FString PropertyName = Params->GetStringField(TEXT("property_name"));
    const bool bRawMode = MonolithUIInternal::GetOptionalBool(Params, TEXT("raw_mode"), false);

    // Bug #6 fix (2026-05-16 UI gap audit): accept BOTH `value` and
    // `property_value` as aliases. Discovery output's schema description
    // historically left the param name ambiguous; some callers probed
    // with `property_value` and got
    // "Missing required param" followed by a SECOND error about wbp_path
    // when the param was renamed — the dual-failure mode wasted calls. We
    // now accept either spelling and surface a single coherent error that
    // names both forms AND preserves wbp_path in the message.
    //
    // Pull as the raw JSON value so non-string shapes (numbers, booleans,
    // arrays, struct objects) survive — FUIReflectionHelper dispatches on
    // FProperty kind, not on FString shape.
    TSharedPtr<FJsonValue> ValueJson = Params->TryGetField(TEXT("value"));
    if (!ValueJson.IsValid())
    {
        ValueJson = Params->TryGetField(TEXT("property_value"));
    }
    if (!ValueJson.IsValid())
    {
        return FMonolithActionResult::Error(FString::Printf(
            TEXT("set_widget_property: missing required param 'value' (alias: 'property_value') on wbp_path='%s', widget_name='%s', property_name='%s'"),
            *AssetPath, *WidgetName, *PropertyName));
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
    if (!Widget)
    {
        // Phase K — Lookup error. valid_options is intentionally empty (would
        // require scanning the live WidgetTree, which the LLM can do via
        // ui::get_widget_tree as the suggested_fix indicates).
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("Lookup"),
            TEXT("/widget_name"),
            FString::Printf(TEXT("Widget '%s' not found in WBP '%s'."), *WidgetName, *AssetPath),
            TEXT("Call ui::get_widget_tree to enumerate live widget names.")));
    }

    // ----- Phase C primary path: gated reflection helper -----
    UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get();
    FUIPropertyPathCache* Cache = Sub ? Sub->GetPathCache() : nullptr;
    const FUIPropertyAllowlist* Allowlist = Sub ? &Sub->GetAllowlist() : nullptr;

    FUIReflectionHelper Helper(Cache, Allowlist);
    const FUIReflectionApplyResult ApplyRes = Helper.Apply(Widget, PropertyName, ValueJson, bRawMode);

    if (ApplyRes.bSuccess)
    {
        Widget->SynchronizeProperties();
        FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

        const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), false);
        if (bCompile) FKismetEditorUtilities::CompileBlueprint(WBP);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("widget"), WidgetName);
        Result->SetStringField(TEXT("property"), PropertyName);
        // Echo the value as its string serialisation for compat with existing
        // call-site expectations (legacy result included a `value` string).
        Result->SetStringField(TEXT("value"), ValueJson->AsString());
        Result->SetBoolField(TEXT("compiled"), bCompile);
        Result->SetBoolField(TEXT("raw_mode"), bRawMode);
        return FMonolithActionResult::Success(Result);
    }

    // ----- Failure path: propagate the helper's structured reason -----
    //
    // Phase K: replaced the prior `| reason=X detail=Y` interim shape with the
    // FUISpecError formatter so the payload now carries category / json_path /
    // suggested_fix / valid_options on the same key:value rails as the
    // `build_ui_from_spec` validation block. For NotInAllowlist failures we
    // populate valid_options with the actual allowlist entries for this
    // widget type so the LLM can pick a legal path on its next attempt.
    FString SuggestedFix;
    TArray<FString> ValidOptions;
    FName Category = TEXT("Property");

    if (ApplyRes.FailureReason == TEXT("NotInAllowlist"))
    {
        Category = TEXT("Allowlist");
        SuggestedFix = TEXT("Path not on the curated per-type allowlist. Pick a path from valid_options, or pass raw_mode=true to bypass the gate (legacy compat).");
        if (Allowlist)
        {
            // Pull the live allowlist for this widget type. The list can be
            // empty (registry not yet populated, type not on the allowlist):
            // in that case suggested_fix still names raw_mode as the escape.
            const FName Token = FName(*Widget->GetClass()->GetName());
            ValidOptions = Allowlist->GetAllowedPaths(Token);
        }
    }
    else if (ApplyRes.FailureReason == TEXT("PropertyNotFound"))
    {
        Category = TEXT("Property");
        SuggestedFix = TEXT("Property not found via reflection on the widget class. Verify the property name spelling and walk through any FStructProperty hops with dotted segments (e.g. 'Padding.Left').");
    }
    else if (ApplyRes.FailureReason == TEXT("ParseFailed"))
    {
        Category = TEXT("ValueParse");
        SuggestedFix = TEXT("Could not parse the value into the property's struct/scalar shape. Check the FProperty kind (Color/Vector2D/Margin/enum) and supply the matching JSON literal or struct.");
    }
    else if (ApplyRes.FailureReason == TEXT("TypeMismatch"))
    {
        Category = TEXT("TypeMismatch");
        SuggestedFix = TEXT("Value JSON shape doesn't match the FProperty's expected type. See ApplyRes.Detail for the expected kind (e.g. 'expected number', 'expected bool').");
    }
    else
    {
        SuggestedFix = TEXT("Unknown failure mode. Check the editor log for ApplyRes.Detail context.");
    }

    FUISpecError E = MonolithUIInternal::MakeSpecError(
        Category,
        FString::Printf(TEXT("/property_name (%s)"), *PropertyName),
        FString::Printf(TEXT("set_widget_property failed: %s on %s.%s (%s)"),
            *ApplyRes.FailureReason,
            *Widget->GetClass()->GetName(),
            *PropertyName,
            *ApplyRes.Detail),
        SuggestedFix,
        MoveTemp(ValidOptions));
    E.WidgetId = FName(*WidgetName);
    // -32602 is JSON-RPC "invalid params" — gate-rejection is caller-input,
    // not internal-error.
    return MonolithUIInternal::MakeErrorFromSpecError(E, -32602);
}

// --- compile_widget ---
// Bug #5 fix (2026-05-16 UI gap audit): the action now always returns
// errors[] + warnings[] + notes[] arrays harvested from FCompilerResultsLog.
// The shape mirrors blueprint_query("compile_blueprint") so callers can use
// a single parser. Pattern mirrored from
// MonolithBlueprintCompileActions.cpp:80 (HandleCompileBlueprint).
FMonolithActionResult FMonolithUIActions::HandleCompileWidget(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    // Drive the compile through the FCompilerResultsLog-capturing overload so
    // the Messages array carries every Tokenized diagnostic the validator and
    // the K2 compiler emit. SkipGarbageCollection matches the blueprint_query
    // flow's flags and keeps the action interactive-fast.
    FCompilerResultsLog Results;
    FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

    TArray<TSharedPtr<FJsonValue>> ErrorArr;
    TArray<TSharedPtr<FJsonValue>> WarnArr;
    TArray<TSharedPtr<FJsonValue>> NoteArr;
    for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
    {
        TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
        MsgObj->SetStringField(TEXT("message"), Msg->ToText().ToString());

        const EMessageSeverity::Type Sev = Msg->GetSeverity();
        if (Sev == EMessageSeverity::Error)
        {
            ErrorArr.Add(MakeShared<FJsonValueObject>(MsgObj));
        }
        else if (Sev == EMessageSeverity::Warning)
        {
            WarnArr.Add(MakeShared<FJsonValueObject>(MsgObj));
        }
        else
        {
            // Info / PerformanceWarning / unknown all surface as notes so
            // callers see them without conflating with hard errors.
            NoteArr.Add(MakeShared<FJsonValueObject>(MsgObj));
        }
    }

    // Status-string mapping shared across the two response paths (error vs
    // success). Keep aligned with blueprint_query("compile_blueprint") so
    // callers can switch on the same set of tokens.
    FString StatusStr;
    switch (WBP->Status)
    {
    case BS_Unknown:              StatusStr = TEXT("unknown"); break;
    case BS_Dirty:                StatusStr = TEXT("dirty"); break;
    case BS_Error:                StatusStr = TEXT("error"); break;
    case BS_UpToDate:             StatusStr = TEXT("up_to_date"); break;
    case BS_UpToDateWithWarnings: StatusStr = TEXT("up_to_date_with_warnings"); break;
    case BS_BeingCreated:         StatusStr = TEXT("being_created"); break;
    default:                      StatusStr = TEXT("other"); break;
    }

    // Phase K — when the compiler reports BS_Error, surface that as an
    // FUISpecError-shaped failure rather than a success-with-status=error.
    // The LLM consumer can branch cleanly on bSuccess instead of having to
    // parse a string status field from a "successful" call. Bug #5 evolution:
    // append the FCompilerResultsLog ValidOptions list with the verbatim
    // error/warning messages — the dispatcher (MonolithHttpServer:716)
    // surfaces only the FMonolithActionResult::ErrorMessage text on a failed
    // call, so we pack the diagnostic surface INTO that text via the
    // FUISpecError ValidOptions field (which ToLLMReport() renders as a
    // labelled `valid_options:` block in the error body).
    if (WBP->Status == BS_Error)
    {
        // Compose the ValidOptions list as "[Error] <msg>" / "[Warn] <msg>"
        // strings so the LLM sees both severity AND text without having to
        // parse a nested JSON object inside an error string.
        TArray<FString> Diagnostics;
        Diagnostics.Reserve(ErrorArr.Num() + WarnArr.Num());
        for (const TSharedPtr<FJsonValue>& V : ErrorArr)
        {
            const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
            FString MsgText;
            if (Obj.IsValid()) Obj->TryGetStringField(TEXT("message"), MsgText);
            Diagnostics.Add(FString::Printf(TEXT("[Error] %s"), *MsgText));
        }
        for (const TSharedPtr<FJsonValue>& V : WarnArr)
        {
            const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
            FString MsgText;
            if (Obj.IsValid()) Obj->TryGetStringField(TEXT("message"), MsgText);
            Diagnostics.Add(FString::Printf(TEXT("[Warn] %s"), *MsgText));
        }

        // Build a compact suggested_fix that names the first error message
        // verbatim so callers don't have to dig into valid_options[] for
        // the basic "what went wrong" answer.
        FString FirstErrorPreview;
        if (ErrorArr.Num() > 0)
        {
            const TSharedPtr<FJsonObject> Obj = ErrorArr[0]->AsObject();
            if (Obj.IsValid()) Obj->TryGetStringField(TEXT("message"), FirstErrorPreview);
        }
        const FString FailDetail = FirstErrorPreview.IsEmpty()
            ? FString::Printf(TEXT("Blueprint '%s' compiled with errors (BS_Error). See valid_options[] for diagnostics."), *AssetPath)
            : FString::Printf(TEXT("Blueprint '%s' compiled with errors (BS_Error): %s"), *AssetPath, *FirstErrorPreview);

        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("Compile"),
            TEXT("/asset_path"),
            FailDetail,
            TEXT("Inspect valid_options[] in this error for the full FCompilerResultsLog surface, or call blueprint_query::compile_blueprint for the same diagnostics with per-node error linkage."),
            MoveTemp(Diagnostics));
        return MonolithUIInternal::MakeErrorFromSpecError(E, -32603);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetBoolField(TEXT("compiled"), true);
    Result->SetStringField(TEXT("status"), StatusStr);
    Result->SetArrayField(TEXT("errors"), ErrorArr);
    Result->SetArrayField(TEXT("warnings"), WarnArr);
    Result->SetArrayField(TEXT("notes"), NoteArr);
    Result->SetNumberField(TEXT("error_count"), ErrorArr.Num());
    Result->SetNumberField(TEXT("warning_count"), WarnArr.Num());
    return FMonolithActionResult::Success(Result);
}

// --- list_widget_types ---
FMonolithActionResult FMonolithUIActions::HandleListWidgetTypes(const TSharedPtr<FJsonObject>& Params)
{
    FString Filter = MonolithUIInternal::GetOptionalString(Params, TEXT("filter"));

    struct FWidgetTypeInfo
    {
        FString Name;
        FString Category;
        bool bIsPanel;
    };

    TArray<FWidgetTypeInfo> Types = {
        // Panels
        {TEXT("CanvasPanel"),       TEXT("panel"), true},
        {TEXT("VerticalBox"),       TEXT("panel"), true},
        {TEXT("HorizontalBox"),     TEXT("panel"), true},
        {TEXT("Overlay"),           TEXT("panel"), true},
        {TEXT("ScrollBox"),         TEXT("panel"), true},
        {TEXT("SizeBox"),           TEXT("panel"), true},
        {TEXT("ScaleBox"),          TEXT("panel"), true},
        {TEXT("Border"),            TEXT("panel"), true},
        {TEXT("WrapBox"),           TEXT("panel"), true},
        {TEXT("UniformGridPanel"),  TEXT("panel"), true},
        {TEXT("GridPanel"),         TEXT("panel"), true},
        {TEXT("WidgetSwitcher"),    TEXT("panel"), true},
        {TEXT("BackgroundBlur"),    TEXT("panel"), true},
        {TEXT("NamedSlot"),         TEXT("panel"), true},
        // Display
        {TEXT("TextBlock"),         TEXT("display"), false},
        {TEXT("RichTextBlock"),     TEXT("display"), false},
        {TEXT("Image"),             TEXT("display"), false},
        {TEXT("ProgressBar"),       TEXT("display"), false},
        {TEXT("Spacer"),            TEXT("layout"), false},
        // Input
        {TEXT("Button"),            TEXT("input"), true},
        {TEXT("CheckBox"),          TEXT("input"), false},
        {TEXT("Slider"),            TEXT("input"), false},
        {TEXT("EditableText"),      TEXT("input"), false},
        {TEXT("EditableTextBox"),   TEXT("input"), false},
        {TEXT("ComboBoxString"),    TEXT("input"), false},
        {TEXT("InputKeySelector"),  TEXT("input"), false},
        // Data
        {TEXT("ListView"),          TEXT("data"), true},
        {TEXT("TileView"),          TEXT("data"), true},
    };

    TArray<TSharedPtr<FJsonValue>> ResultArray;
    for (const auto& T : Types)
    {
        if (!Filter.IsEmpty() && T.Category != Filter) continue;

        TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
        TypeObj->SetStringField(TEXT("name"), T.Name);
        TypeObj->SetStringField(TEXT("category"), T.Category);
        TypeObj->SetBoolField(TEXT("is_panel"), T.bIsPanel);
        ResultArray.Add(MakeShared<FJsonValueObject>(TypeObj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("widget_types"), ResultArray);
    Result->SetNumberField(TEXT("count"), ResultArray.Num());
    return FMonolithActionResult::Success(Result);
}

// =============================================================================
// Phase 2 (2026-05-16 UI gap audit) — file-static handlers
// =============================================================================
//
// Living below the class member definitions so the file's call-graph reads top
// to bottom: registration -> class statics -> Phase 2 additions. The handlers
// are file-static rather than members of FMonolithUIActions so the migration
// did not require a header change (Phase 2 §F6 prohibits new .h surface).

namespace MonolithUIActionsPhase2
{
    // ---- Phase 2 Item #7 — rename_widget --------------------------------------
    //
    // Renames a UWidget in a WBP's WidgetTree. Validates uniqueness against the
    // full tree (case-sensitive FName match) before calling Widget->Rename so
    // the rename never silently produces "Name_1" auto-suffixed via the engine's
    // collision handling — that would silently drift the caller's expected
    // identifier. Recompile through FKismetEditorUtilities::CompileBlueprint
    // matches every other mutation site in this module (e.g. HandleAddWidget,
    // HandleRemoveWidget, the CommonUI button category) for behavioural parity.

    static FMonolithActionResult HandleRenameWidget(const TSharedPtr<FJsonObject>& Params)
    {
        // Both `wbp_path` (CommonUI convention) and `asset_path` (base UMG
        // convention) are accepted — matches MonolithCommonUI::GetWbpPath.
        FString WbpPath;
        if (!Params.IsValid()
            || (!Params->TryGetStringField(TEXT("wbp_path"), WbpPath)
                && !Params->TryGetStringField(TEXT("asset_path"), WbpPath))
            || WbpPath.IsEmpty())
        {
            return FMonolithActionResult::Error(
                TEXT("wbp_path (or asset_path) required"), -32602);
        }

        FString OldName, NewName;
        FMonolithActionResult ParamError;
        if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("old_name"), OldName, ParamError))
            return ParamError;
        if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("new_name"), NewName, ParamError))
            return ParamError;

        if (OldName.Equals(NewName, ESearchCase::CaseSensitive))
        {
            return FMonolithActionResult::Error(
                TEXT("new_name is identical to old_name — nothing to do"), -32602);
        }

        FMonolithActionResult LoadErr;
        UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(WbpPath, LoadErr);
        if (!WBP) return LoadErr;
        if (!WBP->WidgetTree)
        {
            return FMonolithActionResult::Error(TEXT("WidgetTree is null (editor-only data not available)"), -32603);
        }

        // Locate the target + verify uniqueness in a single tree walk. We also
        // collect any existing widget already named `new_name` so the error
        // message reports which widget is colliding.
        UWidget* Target = nullptr;
        UWidget* Collider = nullptr;
        const FName OldFName(*OldName);
        const FName NewFName(*NewName);
        WBP->WidgetTree->ForEachWidget([&](UWidget* W)
        {
            if (!W) return;
            if (W->GetFName() == OldFName) Target = W;
            if (W->GetFName() == NewFName) Collider = W;
        });

        if (!Target)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Widget '%s' not found in WBP '%s'"), *OldName, *WbpPath),
                -32602);
        }
        if (Collider)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("new_name '%s' is already in use by another widget (%s) in WBP '%s'"),
                    *NewName, *Collider->GetClass()->GetName(), *WbpPath),
                -32602);
        }

        const FString OldClass = Target->GetClass()->GetName();
        const bool bWasBoundAsVariable = Target->bIsVariable;

        Target->Modify();
        WBP->Modify();

        // Rename to the same Outer (WidgetTree). REN_DontCreateRedirectors
        // keeps the package clean — no lingering linker-level redirect entry.
        // Pass `nullptr` Outer to keep the existing one (UWidget::Rename semantics).
        const bool bRenamed = Target->Rename(*NewName, nullptr, REN_DontCreateRedirectors);
        if (!bRenamed)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("UObject::Rename returned false for widget '%s' -> '%s'"),
                    *OldName, *NewName),
                -32603);
        }

        // If the widget was a named variable (bIsVariable=true), the WBP holds
        // a Bindings entry and a NewVariables entry keyed on the old FName.
        // FBlueprintEditorUtils::RenameMemberVariable is the canonical fixup
        // path — it walks both arrays and replaces the FName. We only invoke
        // it when bIsVariable was set, matching the engine's own widget rename
        // path in WidgetBlueprintEditorUtils.
        if (bWasBoundAsVariable)
        {
            FBlueprintEditorUtils::RenameMemberVariable(WBP, OldFName, NewFName);
        }

        // Reconcile + recompile. The reconcile pass clears stale variable GUIDs
        // for the legacy name; MarkAsStructurallyModified bumps the
        // BlueprintCompileVersion so the next CDO load picks up the new layout.
        MonolithUIInternal::ReconcileWidgetVariableGuids(WBP);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        FKismetEditorUtilities::CompileBlueprint(WBP);
        WBP->GetOutermost()->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("wbp_path"), WbpPath);
        Result->SetStringField(TEXT("old_name"), OldName);
        Result->SetStringField(TEXT("new_name"), NewName);
        Result->SetStringField(TEXT("widget_class"), OldClass);
        Result->SetBoolField(TEXT("was_bound_as_variable"), bWasBoundAsVariable);
        Result->SetBoolField(TEXT("recompiled"), true);
        return FMonolithActionResult::Success(Result);
    }

    // ---- Phase 2 Item #14 — dump_blueprint_compile_log ------------------------
    //
    // Re-drives a compile through the FCompilerResultsLog-capturing overload
    // and returns the messages in the same shape blueprint_query("compile_blueprint")
    // produces. Phase 1's HandleCompileWidget does NOT cache the FCompilerResultsLog
    // on the asset — the Phase 1 fix surfaced the log only inside its own call.
    // dump_blueprint_compile_log is intended to be called AFTER a compile when
    // the orchestrator dropped or never parsed the original payload.
    //
    // Accepts both UWidgetBlueprint and plain UBlueprint paths so the action
    // works as a general-purpose "last status + messages" probe.

    FMonolithActionResult HandleDumpBlueprintCompileLog(const TSharedPtr<FJsonObject>& Params)
    {
        FString AssetPath;
        FMonolithActionResult ParamError;
        if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
            return ParamError;

        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
        if (!Blueprint)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Failed to load Blueprint at '%s'"), *AssetPath),
                -32602);
        }

        // Capture pre-compile status so callers can tell whether the action
        // observed a stale BS_Dirty / BS_UpToDate from a previous edit
        // (vs. forcing a fresh compile because the asset was clean).
        const TEnumAsByte<EBlueprintStatus> PreStatus = Blueprint->Status;

        FCompilerResultsLog Results;
        FKismetEditorUtilities::CompileBlueprint(
            Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

        // Status-string mapping aligned with HandleCompileWidget (line ~793).
        auto StatusToString = [](EBlueprintStatus S) -> FString
        {
            switch (S)
            {
                case BS_Unknown:              return TEXT("unknown");
                case BS_Dirty:                return TEXT("dirty");
                case BS_Error:                return TEXT("error");
                case BS_UpToDate:             return TEXT("up_to_date");
                case BS_UpToDateWithWarnings: return TEXT("up_to_date_with_warnings");
                case BS_BeingCreated:         return TEXT("being_created");
                default:                      return TEXT("other");
            }
        };

        TArray<TSharedPtr<FJsonValue>> ErrorArr;
        TArray<TSharedPtr<FJsonValue>> WarnArr;
        TArray<TSharedPtr<FJsonValue>> NoteArr;
        for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
        {
            TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
            MsgObj->SetStringField(TEXT("message"), Msg->ToText().ToString());

            const EMessageSeverity::Type Sev = Msg->GetSeverity();
            if (Sev == EMessageSeverity::Error)
            {
                ErrorArr.Add(MakeShared<FJsonValueObject>(MsgObj));
            }
            else if (Sev == EMessageSeverity::Warning)
            {
                WarnArr.Add(MakeShared<FJsonValueObject>(MsgObj));
            }
            else
            {
                NoteArr.Add(MakeShared<FJsonValueObject>(MsgObj));
            }
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("asset_path"), AssetPath);
        Result->SetStringField(TEXT("blueprint_class"), Blueprint->GetClass()->GetName());
        Result->SetStringField(TEXT("pre_compile_status"), StatusToString(PreStatus));
        Result->SetStringField(TEXT("last_compile_status"), StatusToString(Blueprint->Status));
        Result->SetArrayField(TEXT("errors"),   ErrorArr);
        Result->SetArrayField(TEXT("warnings"), WarnArr);
        Result->SetArrayField(TEXT("notes"),    NoteArr);
        Result->SetNumberField(TEXT("error_count"),   ErrorArr.Num());
        Result->SetNumberField(TEXT("warning_count"), WarnArr.Num());
        Result->SetBoolField(TEXT("ran_fresh_compile"), true);
        return FMonolithActionResult::Success(Result);
    }
}
