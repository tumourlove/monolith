// MonolithUIActions.cpp
#include "MonolithUIActions.h"
#include "MonolithUIInternal.h"
#include "MonolithParamSchema.h"
#include "MonolithPackagePathValidator.h"
#include "WidgetBlueprintFactory.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"

void FMonolithUIActions::RegisterActions(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("ui"), TEXT("create_widget_blueprint"),
        TEXT("Create a new Widget Blueprint asset"),
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
        TEXT("Set a property on a widget (text, color, opacity, visibility, etc.)"),
        FMonolithActionHandler::CreateStatic(&HandleSetWidgetProperty),
        FParamSchemaBuilder()
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Target widget name"))
            .Required(TEXT("property_name"), TEXT("string"), TEXT("Property name: Text, ColorAndOpacity, RenderOpacity, Visibility, etc."))
            .Required(TEXT("value"), TEXT("string"), TEXT("Property value as string"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after setting"), TEXT("false"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("compile_widget"),
        TEXT("Compile a Widget Blueprint"),
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
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Parent class '%s' not found or not a UUserWidget subclass"), *ParentClassName));
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
        return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *SavePath));
    }

    // Fail cleanly if the asset already exists instead of letting FactoryCreateNew assert.
    if (FindObject<UObject>(Package, *AssetName))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Widget Blueprint already exists at '%s'"), *SavePath));
    }

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *SavePath, *AssetName);
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    if (AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid())
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Widget Blueprint already exists at '%s'"), *SavePath));
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
    FString AssetPath = Params->GetStringField(TEXT("asset_path"));
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
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Unknown widget class: %s. Use list_widget_types to see available classes."), *WidgetClassName));
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
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Parent '%s' not found or is not a panel widget"), *ParentName));
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
FMonolithActionResult FMonolithUIActions::HandleSetWidgetProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    FString WidgetName = Params->GetStringField(TEXT("widget_name"));
    FString PropertyName = Params->GetStringField(TEXT("property_name"));
    FString Value = Params->GetStringField(TEXT("value"));

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
    if (!Widget)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
    }

    // Try reflection-based property set first
    FProperty* Prop = Widget->GetClass()->FindPropertyByName(FName(*PropertyName));
    if (Prop)
    {
        void* PropAddr = Prop->ContainerPtrToValuePtr<void>(Widget);
        if (Prop->ImportText_Direct(*Value, PropAddr, Widget, PPF_None))
        {
            Widget->SynchronizeProperties();
            FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

            const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), false);
            if (bCompile) FKismetEditorUtilities::CompileBlueprint(WBP);

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("widget"), WidgetName);
            Result->SetStringField(TEXT("property"), PropertyName);
            Result->SetStringField(TEXT("value"), Value);
            Result->SetBoolField(TEXT("compiled"), bCompile);
            return FMonolithActionResult::Success(Result);
        }
    }

    return FMonolithActionResult::Error(
        FString::Printf(TEXT("Property '%s' not found on %s or value '%s' could not be parsed"),
            *PropertyName, *Widget->GetClass()->GetName(), *Value));
}

// --- compile_widget ---
FMonolithActionResult FMonolithUIActions::HandleCompileWidget(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    FKismetEditorUtilities::CompileBlueprint(WBP);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetBoolField(TEXT("compiled"), true);
    Result->SetStringField(TEXT("status"),
        WBP->Status == BS_UpToDate ? TEXT("up_to_date") :
        WBP->Status == BS_Error ? TEXT("error") : TEXT("unknown"));
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
