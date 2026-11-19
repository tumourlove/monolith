// MonolithUIInternal.h
#pragma once

#include "CoreMinimal.h"
#include "WidgetBlueprint.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Components/Button.h"
#include "Components/ProgressBar.h"
#include "Components/CheckBox.h"
#include "Components/Slider.h"
#include "Components/ScrollBox.h"
#include "Components/Border.h"
#include "Components/Spacer.h"
#include "Components/SizeBox.h"
#include "Components/ScaleBox.h"
#include "Components/WrapBox.h"
#include "Components/UniformGridPanel.h"
#include "Components/GridPanel.h"
#include "Components/BackgroundBlur.h"
#include "Components/RichTextBlock.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/ComboBoxString.h"
#include "Components/InputKeySelector.h"
#include "Components/WidgetSwitcher.h"
#include "Components/ListView.h"
#include "Components/TileView.h"
#include "Components/NamedSlot.h"
#include "Dom/JsonObject.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "WidgetBlueprintEditorUtils.h"
#include "MonolithJsonUtils.h"        // R3b: ErrOptionalDepUnavailable for the §5.5 helper
#include "MonolithToolRegistry.h"
#include "MonolithAssetUtils.h"
#include "WidgetBlueprintFactory.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "Blueprint/UserWidget.h"
#include "Styling/SlateColor.h"
#include "Styling/SlateBrush.h"

// Phase A hoist (2026-04-25): the helpers below now forward to the exported
// `MonolithUI::` namespace in `MonolithUICommon.h`. The inline wrappers are
// retained so existing call sites compile unchanged during the migration.
// New code should call `MonolithUI::Foo` directly.
#include "MonolithUICommon.h"

// Phase K — UISpec error shape is reused at the per-action error path so a
// dropped JSON-RPC error payload carries the same key:value labels the LLM
// already greps in `build_ui_from_spec` responses (`json_path`, `category`,
// `valid_options`, `suggested_fix`). One contract end-to-end.
#include "Spec/UISpec.h"

namespace MonolithUIInternal
{
    inline bool TryGetRequiredString(
        const TSharedPtr<FJsonObject>& Object,
        const TCHAR* FieldName,
        FString& OutValue,
        FMonolithActionResult& OutError)
    {
        if (!Object.IsValid() || !Object->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
        {
            OutError = FMonolithActionResult::Error(
                FString::Printf(TEXT("Missing required param: %s"), FieldName));
            return false;
        }

        return true;
    }

    // R3b / §5.5 — canonical "optional sibling plugin unavailable" error
    // payload. Built once here so every EffectSurface action handler (and any
    // future optional-dep handler) emits the EXACT same shape:
    //
    //   {
    //     "bSuccess": false,
    //     "ErrorCode": -32010,                       // ErrOptionalDepUnavailable
    //     "ErrorMessage": "<WidgetType> widget unavailable — <DepName> plugin
    //                      not loaded. Install the plugin or use
    //                      <Alternative> with a different widget type.",
    //     "Result": {
    //       "dep_name":     "<DepName>",
    //       "widget_type":  "<WidgetType>",
    //       "alternative":  "<Alternative>",
    //       "category":     "OptionalDepUnavailable"  // sibling of FUISpecError categories
    //     }
    //   }
    //
    // The `category` value mirrors the `FUIReflectionApplyResult::FailureReason`
    // taxonomy used elsewhere in the action surface — adds
    // "OptionalDepUnavailable" as an LLM-greppable sibling. The structured
    // `Result` payload travels even though `bSuccess=false`, matching the
    // `MakeErrorFromSpecError` convention above (same pattern: error +
    // structured machine-readable detail in one response).
    //
    // Phase R2 in Wave 2 calls this from inside `ResolveEffectSurface` —
    // never per-handler, single point of truth.
    inline FMonolithActionResult MakeOptionalDepUnavailableError(
        const FString& WidgetType,
        const FString& DepName,
        const FString& Alternative)
    {
        const FString Message = FString::Printf(
            TEXT("%s widget unavailable -- %s not loaded. Install/enable the provider or use %s with a different widget type."),
            *WidgetType, *DepName, *Alternative);

        FMonolithActionResult Result = FMonolithActionResult::Error(
            Message, FMonolithJsonUtils::ErrOptionalDepUnavailable);

        // Populate the structured payload alongside the error message. The
        // FMonolithActionResult::Error static initialises Result to nullptr;
        // we attach a fresh JSON object so downstream tooling has both the
        // human message AND the greppable fields without parsing the message.
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("dep_name"),    DepName);
        Payload->SetStringField(TEXT("widget_type"), WidgetType);
        Payload->SetStringField(TEXT("alternative"), Alternative);
        Payload->SetStringField(TEXT("category"),    TEXT("OptionalDepUnavailable"));
        Result.Result = Payload;
        return Result;
    }

    // Phase K — convert an FUISpecError to an FMonolithActionResult::Error. The
    // error message body is the per-finding ToLLMReport(), which gives the LLM
    // stable `json_path:` / `category:` / `valid_options:` / `suggested_fix:`
    // labels to grep against. Defaults to JSON-RPC -32602 (invalid-params)
    // because action-level rejections (bad widget name, missing required field,
    // gate-fail) are caller-input issues, not internal errors. Pass -32603 for
    // the rare "internal error" case (asset registry corruption etc.).
    inline FMonolithActionResult MakeErrorFromSpecError(const FUISpecError& Err, int32 Code = -32602)
    {
        return FMonolithActionResult::Error(Err.ToLLMReport(), Code);
    }

    // Phase K — convenience builder for the common "missing widget" / "bad
    // enum value" / "type mismatch" shape. Centralises the FUISpecError field
    // population so we get consistent category names ("Asset", "Type",
    // "Lookup", "Enum", "Property") across the action surface.
    inline FUISpecError MakeSpecError(
        FName Category,
        const FString& JsonPath,
        const FString& Message,
        const FString& SuggestedFix = FString(),
        TArray<FString> ValidOptions = TArray<FString>())
    {
        FUISpecError E;
        E.Severity     = EUISpecErrorSeverity::Error;
        E.Category     = Category;
        E.JsonPath     = JsonPath;
        E.Message      = Message;
        E.SuggestedFix = SuggestedFix;
        E.ValidOptions = MoveTemp(ValidOptions);
        return E;
    }

    // Phase K — canonical anchor-preset list. Mirrored from the static map in
    // MonolithUI::GetAnchorPreset (MonolithUICommon.cpp:193-214). Kept here in
    // a `valid_options`-shaped form so action handlers can drop it directly
    // into FUISpecError::ValidOptions on a bad-preset failure.
    //
    // If you add a preset to GetAnchorPreset, add it here too — there is no
    // single source of truth because GetAnchorPreset returns an FAnchors by
    // value (no enumeration API). The set is small + stable, so this is
    // pragmatic. A test in UIErrorFormattingTests verifies the count matches.
    inline const TArray<FString>& GetAnchorPresetNames()
    {
        static const TArray<FString> Names = {
            TEXT("top_left"),    TEXT("top_center"),    TEXT("top_right"),
            TEXT("center_left"), TEXT("center"),        TEXT("center_right"),
            TEXT("bottom_left"), TEXT("bottom_center"), TEXT("bottom_right"),
            TEXT("stretch_top"), TEXT("stretch_bottom"),
            TEXT("stretch_left"), TEXT("stretch_right"),
            TEXT("stretch_horizontal"), TEXT("stretch_vertical"),
            TEXT("stretch_fill")
        };
        return Names;
    }

    inline FString GetOptionalString(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, const FString& DefaultValue = FString())
    {
        if (!Object.IsValid())
        {
            return DefaultValue;
        }

        FString Value;
        return Object->TryGetStringField(FieldName, Value) ? Value : DefaultValue;
    }

    inline bool GetOptionalBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, bool DefaultValue)
    {
        if (!Object.IsValid())
        {
            return DefaultValue;
        }

        bool Value = DefaultValue;
        return Object->TryGetBoolField(FieldName, Value) ? Value : DefaultValue;
    }

    // Load a widget blueprint by asset path. Forwards to MonolithUI::LoadWidgetBlueprint
    // (Phase A hoist — see MonolithUICommon.h).
    inline UWidgetBlueprint* LoadWidgetBlueprint(const FString& AssetPath, FMonolithActionResult& OutError)
    {
        return MonolithUI::LoadWidgetBlueprint(AssetPath, OutError);
    }

    // Map of widget class short names to UClass pointers. Forwards to
    // MonolithUI::WidgetClassFromName (Phase A hoist — see MonolithUICommon.h).
    inline UClass* WidgetClassFromName(const FString& ClassName)
    {
        return MonolithUI::WidgetClassFromName(ClassName);
    }

    // Serialize a single widget's slot properties to JSON
    inline TSharedPtr<FJsonObject> SerializeSlotProperties(UPanelSlot* Slot)
    {
        TSharedPtr<FJsonObject> SlotJson = MakeShared<FJsonObject>();
        if (!Slot) return SlotJson;

        SlotJson->SetStringField(TEXT("slot_type"), Slot->GetClass()->GetName());

        if (UCanvasPanelSlot* CS = Cast<UCanvasPanelSlot>(Slot))
        {
            const FAnchorData& Layout = CS->GetLayout();
            TSharedPtr<FJsonObject> AnchorsJson = MakeShared<FJsonObject>();
            AnchorsJson->SetNumberField(TEXT("min_x"), Layout.Anchors.Minimum.X);
            AnchorsJson->SetNumberField(TEXT("min_y"), Layout.Anchors.Minimum.Y);
            AnchorsJson->SetNumberField(TEXT("max_x"), Layout.Anchors.Maximum.X);
            AnchorsJson->SetNumberField(TEXT("max_y"), Layout.Anchors.Maximum.Y);
            SlotJson->SetObjectField(TEXT("anchors"), AnchorsJson);

            TSharedPtr<FJsonObject> OffsetsJson = MakeShared<FJsonObject>();
            OffsetsJson->SetNumberField(TEXT("left"), Layout.Offsets.Left);
            OffsetsJson->SetNumberField(TEXT("top"), Layout.Offsets.Top);
            OffsetsJson->SetNumberField(TEXT("right"), Layout.Offsets.Right);
            OffsetsJson->SetNumberField(TEXT("bottom"), Layout.Offsets.Bottom);
            SlotJson->SetObjectField(TEXT("offsets"), OffsetsJson);

            TSharedPtr<FJsonObject> AlignJson = MakeShared<FJsonObject>();
            AlignJson->SetNumberField(TEXT("x"), Layout.Alignment.X);
            AlignJson->SetNumberField(TEXT("y"), Layout.Alignment.Y);
            SlotJson->SetObjectField(TEXT("alignment"), AlignJson);

            SlotJson->SetNumberField(TEXT("z_order"), CS->GetZOrder());
            SlotJson->SetBoolField(TEXT("auto_size"), CS->GetAutoSize());
        }
        else if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot))
        {
            SlotJson->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(VS->GetHorizontalAlignment()));
            SlotJson->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(VS->GetVerticalAlignment()));
        }
        else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot))
        {
            SlotJson->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(HS->GetHorizontalAlignment()));
            SlotJson->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(HS->GetVerticalAlignment()));
        }
        else if (UOverlaySlot* OS = Cast<UOverlaySlot>(Slot))
        {
            SlotJson->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(OS->GetHorizontalAlignment()));
            SlotJson->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(OS->GetVerticalAlignment()));
        }

        return SlotJson;
    }

    // Recursively serialize widget tree to JSON
    inline TSharedPtr<FJsonObject> SerializeWidget(UWidget* Widget)
    {
        if (!Widget) return nullptr;

        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Widget->GetName());
        Obj->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
        Obj->SetStringField(TEXT("visibility"), UEnum::GetValueAsString(Widget->GetVisibility()));
        Obj->SetNumberField(TEXT("render_opacity"), Widget->GetRenderOpacity());
        Obj->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);

        // Slot info (how this widget sits in its parent)
        if (UPanelSlot* Slot = Widget->Slot)
        {
            Obj->SetObjectField(TEXT("slot"), SerializeSlotProperties(Slot));
        }

        // Children (if panel)
        if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
        {
            TArray<TSharedPtr<FJsonValue>> ChildArray;
            for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
            {
                if (UWidget* Child = Panel->GetChildAt(i))
                {
                    TSharedPtr<FJsonObject> ChildObj = SerializeWidget(Child);
                    if (ChildObj.IsValid())
                    {
                        ChildArray.Add(MakeShared<FJsonValueObject>(ChildObj));
                    }
                }
            }
            if (ChildArray.Num() > 0)
            {
                Obj->SetArrayField(TEXT("children"), ChildArray);
            }
        }

        return Obj;
    }

    // Anchor preset map. Forwards to MonolithUI::GetAnchorPreset (Phase A hoist).
    inline FAnchors GetAnchorPreset(const FString& PresetName)
    {
        return MonolithUI::GetAnchorPreset(PresetName);
    }

    // Parse hex color string to FLinearColor (degamma'd via FLinearColor(FColor)).
    // Supports: "#RRGGBB", "#RRGGBBAA", "R,G,B,A" (0-1 floats).
    // Forwards to MonolithUI::ParseColor (Phase A hoist).
    inline FLinearColor ParseColor(const FString& ColorStr)
    {
        return MonolithUI::ParseColor(ColorStr);
    }

    // Configure a canvas panel slot with anchor preset + position/size
    inline void ConfigureCanvasSlot(UPanelSlot* Slot, const FString& AnchorPreset,
        FVector2D Position = FVector2D::ZeroVector, FVector2D Size = FVector2D::ZeroVector,
        bool bAutoSize = false, FVector2D Alignment = FVector2D(0.f, 0.f))
    {
        if (UCanvasPanelSlot* CS = Cast<UCanvasPanelSlot>(Slot))
        {
            if (!AnchorPreset.IsEmpty())
            {
                CS->SetAnchors(GetAnchorPreset(AnchorPreset));
            }
            if (!Position.IsNearlyZero())
            {
                CS->SetPosition(Position);
            }
            if (!Size.IsNearlyZero())
            {
                CS->SetSize(Size);
            }
            if (bAutoSize)
            {
                CS->SetAutoSize(true);
            }
            if (!Alignment.IsNearlyZero())
            {
                CS->SetAlignment(Alignment);
            }
        }
    }

    // Variable-name registration helpers. Forward to MonolithUI:: (Phase A hoist).
    inline void RegisterVariableName(UWidgetBlueprint* WBP, const FName& VariableName)
    {
        MonolithUI::RegisterVariableName(WBP, VariableName);
    }

    inline void RegisterCreatedWidget(UWidgetBlueprint* WBP, UWidget* Widget)
    {
        MonolithUI::RegisterCreatedWidget(WBP, Widget);
    }

    inline void ReconcileWidgetVariableGuids(UWidgetBlueprint* WBP)
    {
        MonolithUI::ReconcileWidgetVariableGuids(WBP);
    }

    inline UWidgetBlueprint* CreateNewWidgetBlueprint(const FString& SavePath, FMonolithActionResult& OutError)
    {
        FString PackagePath, AssetName;
        SavePath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
        if (AssetName.IsEmpty())
        {
            OutError = FMonolithActionResult::Error(TEXT("Invalid save_path — must contain at least one / separator"));
            return nullptr;
        }

        UPackage* Package = CreatePackage(*SavePath);
        if (!Package)
        {
            OutError = FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *SavePath));
            return nullptr;
        }

        UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
        Factory->BlueprintType = BPTYPE_Normal;
        Factory->ParentClass = UUserWidget::StaticClass();

        UObject* CreatedObj = Factory->FactoryCreateNew(
            UWidgetBlueprint::StaticClass(), Package,
            FName(*AssetName), RF_Public | RF_Standalone,
            nullptr, GWarn);

        UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(CreatedObj);
        if (!WBP)
        {
            OutError = FMonolithActionResult::Error(TEXT("UWidgetBlueprintFactory::FactoryCreateNew returned null"));
            return nullptr;
        }

        // Set CanvasPanel as root
        if (WBP->WidgetTree && !WBP->WidgetTree->RootWidget)
        {
            UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
            WBP->WidgetTree->RootWidget = Root;
            RegisterCreatedWidget(WBP, Root);
        }

        return WBP;
    }

    // Save and compile a widget blueprint
    inline void SaveAndCompileWidgetBlueprint(UWidgetBlueprint* WBP, const FString& SavePath)
    {
        ReconcileWidgetVariableGuids(WBP);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        FKismetEditorUtilities::CompileBlueprint(WBP);
        FAssetRegistryModule::AssetCreated(WBP);
        WBP->GetPackage()->MarkPackageDirty();
        FMonolithAssetUtils::SaveAssetPackage(WBP->GetPackage());
    }

    // Helper: add a child widget to a panel, returning the slot
    inline UWidget* AddChildWidget(UWidgetBlueprint* WBP, UPanelWidget* Parent,
        UClass* WidgetClass, const FString& Name)
    {
        UWidget* W = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*Name));
        if (W)
        {
            Parent->AddChild(W);
            RegisterCreatedWidget(WBP, W);
        }
        return W;
    }

    // Alignment parsers. Forward to MonolithUI:: (Phase A hoist).
    inline EHorizontalAlignment ParseHAlign(const FString& S)
    {
        return MonolithUI::ParseHAlign(S);
    }

    inline EVerticalAlignment ParseVAlign(const FString& S)
    {
        return MonolithUI::ParseVAlign(S);
    }
}
