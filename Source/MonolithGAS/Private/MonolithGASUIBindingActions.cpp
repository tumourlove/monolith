// MonolithGASUIBindingActions.cpp
// Implements gas::bind_widget_to_attribute, gas::unbind_widget_attribute,
// gas::list_attribute_bindings, gas::clear_widget_attribute_bindings.
// Each action also registers under the `ui` namespace as an alias.

#include "MonolithGASUIBindingActions.h"
#include "MonolithGASUIBindingBlueprintExtension.h"
#include "MonolithGASUIBindingTypes.h"
#include "MonolithGASInternal.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#include "AttributeSet.h"
#include "GameplayEffectTypes.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintExtension.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/RichTextBlock.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/Image.h"
#include "Components/Border.h"
#include "Components/CheckBox.h"
#include "Components/Slider.h"
#include "Components/SpinBox.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

namespace
{
    UWidgetBlueprint* LoadWBP(const FString& AssetPath, FString& OutError)
    {
        // FMonolithAssetUtils::ResolveAssetPath normalizes leading-`/`-missing paths
        // (relative -> /Game/), so the previous /Game/-prefix fallback is now redundant.
        // Phase J F5 #6: split the asset-not-found vs wrong-asset-class branches so callers
        // can distinguish "I typo'd the path" from "I pointed at a non-WBP asset".
        // Use the unconstrained LoadAssetByPath overload first to detect "asset exists but
        // is the wrong UClass"; the type-checked overload would null both cases.
        UObject* Raw = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
        if (!Raw)
        {
            OutError = FString::Printf(TEXT("Widget Blueprint asset not found: %s"), *AssetPath);
            return nullptr;
        }
        UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Raw);
        if (!WBP)
        {
            OutError = FString::Printf(
                TEXT("Asset at %s is not a Widget Blueprint (got %s)"),
                *AssetPath, *Raw->GetClass()->GetName());
            return nullptr;
        }
        return WBP;
    }

    UWidget* FindWidgetInWBP(UWidgetBlueprint* WBP, FName WidgetName)
    {
        if (!WBP || !WBP->WidgetTree || WidgetName.IsNone()) return nullptr;
        return WBP->WidgetTree->FindWidget(WidgetName);
    }

    /**
     * Phase J F5 #5: build a human-readable "Available: [...]" enumeration of widget-tree variable names
     * for use in "Widget '<name>' not found" error messages. Caps at 20 entries to avoid wall-of-text;
     * past the cap, defers to SPEC_GAS_UIBinding.md.
     */
    FString BuildAvailableWidgetsClause(UWidgetBlueprint* WBP)
    {
        if (!WBP || !WBP->WidgetTree) return TEXT("[]");
        TArray<UWidget*> All;
        WBP->WidgetTree->GetAllWidgets(All);
        if (All.Num() == 0) return TEXT("[]");
        if (All.Num() > 20)
        {
            return TEXT("(available widgets exceeded 20; see SPEC_GAS_UIBinding.md)");
        }
        TArray<FString> Names;
        Names.Reserve(All.Num());
        for (UWidget* W : All)
        {
            if (W) Names.Add(W->GetName());
        }
        Names.Sort();
        return FString::Printf(TEXT("[%s]"), *FString::Join(Names, TEXT(", ")));
    }

    /**
     * Phase J F5 #5: build a "Valid: [...]" enumeration of properties supported by the binder for a
     * given widget class, for use in the "(widget=..., target_property=...)" Unsupported error.
     * The compatibility table here mirrors ValidateWidgetProperty's accept branches.
     */
    FString BuildValidPropertiesClause(UClass* WidgetClass)
    {
        if (!WidgetClass) return TEXT("[]");
        TArray<FString> Valid;
        if (WidgetClass->IsChildOf(UProgressBar::StaticClass()))                  Valid.Add(TEXT("Percent"));
        if (WidgetClass->IsChildOf(UTextBlock::StaticClass())
         || WidgetClass->IsChildOf(URichTextBlock::StaticClass())
         || WidgetClass->IsChildOf(UEditableText::StaticClass())
         || WidgetClass->IsChildOf(UEditableTextBox::StaticClass()))              Valid.Add(TEXT("Text"));
        if (WidgetClass->IsChildOf(UImage::StaticClass()))                        Valid.Add(TEXT("ColorAndOpacity"));
        if (WidgetClass->IsChildOf(UBorder::StaticClass()))                       Valid.Add(TEXT("BrushColor"));
        if (WidgetClass->IsChildOf(USlider::StaticClass())
         || WidgetClass->IsChildOf(USpinBox::StaticClass()))                      Valid.Add(TEXT("Value"));
        if (WidgetClass->IsChildOf(UCheckBox::StaticClass()))                     Valid.Add(TEXT("CheckedState"));
        // Universal widget properties (always available).
        Valid.Add(TEXT("RenderOpacity"));
        Valid.Add(TEXT("Visibility"));
        if (Valid.Num() == 0) return TEXT("[]");
        return FString::Printf(TEXT("[%s]"), *FString::Join(Valid, TEXT(", ")));
    }

    /**
     * Phase J F5 #2: synthesize a "ClassName.PropertyName" composite from a stored class path
     * + property name. Used by the list-response builder so callers can echo back the bind input
     * without having to re-parse the split fields.
     */
    FString BuildAttributeComposite(const FString& AttributeSetClassPath, FName PropertyName)
    {
        if (AttributeSetClassPath.IsEmpty() || PropertyName.IsNone()) return FString();
        const FString ShortClass = FPackageName::ObjectPathToObjectName(AttributeSetClassPath);
        return FString::Printf(TEXT("%s.%s"), *ShortClass, *PropertyName.ToString());
    }

    UClass* ResolveAttributeSetClass(const FString& Path)
    {
        if (Path.IsEmpty()) return nullptr;
        if (UClass* C = LoadObject<UClass>(nullptr, *Path)) return C;
        if (UClass* C = FindFirstObject<UClass>(*Path, EFindFirstObjectOptions::NativeFirst)) return C;
        // UE native UClass names are stored WITHOUT the type prefix (UClass for "class UFoo" is GetName()=="Foo").
        // Try with prefix added (caller passed bare name like "LeviathanVitalsSet"), then with prefix stripped
        // (caller passed full C++ class name like "ULeviathanVitalsSet" — natural for source_query users).
        if (UClass* C = FindFirstObject<UClass>(*(TEXT("U") + Path), EFindFirstObjectOptions::NativeFirst)) return C;
        if (Path.StartsWith(TEXT("U")) && Path.Len() > 1)
            if (UClass* C = FindFirstObject<UClass>(*Path.RightChop(1), EFindFirstObjectOptions::NativeFirst)) return C;
        return nullptr;
    }

    bool ParseAttributeString(const FString& AttrStr, FString& OutClassPath, FName& OutPropName, FString& OutError)
    {
        FString L, R;
        if (!AttrStr.Split(TEXT("."), &L, &R, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        {
            OutError = FString::Printf(TEXT("Attribute must be 'ClassName.PropertyName', got: %s"), *AttrStr);
            return false;
        }
        // L might be a class name OR a full path. Resolve into class then return its full path.
        UClass* C = ResolveAttributeSetClass(L);
        if (!C)
        {
            OutError = FString::Printf(TEXT("AttributeSet class not found: %s"), *L);
            return false;
        }
        FProperty* P = FindFProperty<FProperty>(C, FName(*R));
        if (!P)
        {
            // Build a list of valid property names for diagnostics.
            TArray<FString> ValidProps;
            UScriptStruct* AttrDataStruct = FGameplayAttributeData::StaticStruct();
            for (TFieldIterator<FStructProperty> It(C); It; ++It)
            {
                if (It->Struct && It->Struct->IsChildOf(AttrDataStruct))
                {
                    ValidProps.Add(It->GetName());
                }
            }
            OutError = FString::Printf(
                TEXT("Property '%s' not found on AttributeSet '%s'. Valid: [%s]"),
                *R, *C->GetName(), *FString::Join(ValidProps, TEXT(", ")));
            return false;
        }
        OutClassPath = C->GetPathName();
        OutPropName = FName(*R);
        return true;
    }

    /**
     * Phase J F2: validating owner_resolver parser.
     * Empty input => OwningPlayerPawn default (back-compat).
     * Non-empty input that matches no recognized literal => false + populated OutError.
     * On match, OutSocketTag is also populated for the named_socket:<tag> form.
     */
    bool ParseOwner(const FString& S, EMonolithAttrBindOwner& OutOwner, FName& OutSocketTag, FString& OutError)
    {
        if (S.IsEmpty() || S.Equals(TEXT("owning_player_pawn"), ESearchCase::IgnoreCase))
        {
            OutOwner = EMonolithAttrBindOwner::OwningPlayerPawn;
            return true;
        }
        if (S.Equals(TEXT("owning_player_state"), ESearchCase::IgnoreCase))
        {
            OutOwner = EMonolithAttrBindOwner::OwningPlayerState;
            return true;
        }
        if (S.Equals(TEXT("owning_player_controller"), ESearchCase::IgnoreCase))
        {
            OutOwner = EMonolithAttrBindOwner::OwningPlayerController;
            return true;
        }
        if (S.Equals(TEXT("self_actor"), ESearchCase::IgnoreCase))
        {
            OutOwner = EMonolithAttrBindOwner::SelfActor;
            return true;
        }
        if (S.StartsWith(TEXT("named_socket"), ESearchCase::IgnoreCase))
        {
            OutOwner = EMonolithAttrBindOwner::NamedSocket;
            FString L, R;
            if (S.Split(TEXT(":"), &L, &R) && !R.IsEmpty())
            {
                OutSocketTag = FName(*R);
            }
            return true;
        }
        OutError = FString::Printf(
            TEXT("Unknown owner_resolver '%s'. Valid: [owning_player_pawn, owning_player_state, owning_player_controller, self_actor, named_socket:<tag>]"),
            *S);
        return false;
    }

    FString OwnerToString(EMonolithAttrBindOwner Owner)
    {
        switch (Owner)
        {
            case EMonolithAttrBindOwner::OwningPlayerPawn:       return TEXT("owning_player_pawn");
            case EMonolithAttrBindOwner::OwningPlayerState:      return TEXT("owning_player_state");
            case EMonolithAttrBindOwner::OwningPlayerController: return TEXT("owning_player_controller");
            case EMonolithAttrBindOwner::SelfActor:              return TEXT("self_actor");
            case EMonolithAttrBindOwner::NamedSocket:            return TEXT("named_socket");
        }
        return TEXT("owning_player_pawn");
    }

    EMonolithAttrBindFormat ParseFormat(const FString& S, FString& OutPayload)
    {
        // Recognized: auto | percent_0_1 | int_text | float_text:N | format_string:<tpl> | gradient:<lo->hi>
        if (S.IsEmpty() || S.Equals(TEXT("auto"), ESearchCase::IgnoreCase)) return EMonolithAttrBindFormat::Auto;
        if (S.Equals(TEXT("percent_0_1"), ESearchCase::IgnoreCase)) return EMonolithAttrBindFormat::PercentZeroOne;
        if (S.Equals(TEXT("int_text"), ESearchCase::IgnoreCase)) return EMonolithAttrBindFormat::IntText;
        if (S.StartsWith(TEXT("float_text"), ESearchCase::IgnoreCase))
        {
            FString L, R;
            if (S.Split(TEXT(":"), &L, &R)) OutPayload = R;
            return EMonolithAttrBindFormat::FloatText;
        }
        if (S.StartsWith(TEXT("format_string"), ESearchCase::IgnoreCase))
        {
            FString L, R;
            if (S.Split(TEXT(":"), &L, &R)) OutPayload = R;
            return EMonolithAttrBindFormat::FormatString;
        }
        if (S.StartsWith(TEXT("gradient"), ESearchCase::IgnoreCase))
        {
            FString L, R;
            if (S.Split(TEXT(":"), &L, &R)) OutPayload = R;
            return EMonolithAttrBindFormat::Gradient;
        }
        if (S.StartsWith(TEXT("threshold"), ESearchCase::IgnoreCase))
        {
            FString L, R;
            if (S.Split(TEXT(":"), &L, &R)) OutPayload = R;
            return EMonolithAttrBindFormat::Threshold;
        }
        return EMonolithAttrBindFormat::Auto;
    }

    /**
     * Phase J F3: validate that a format=format_string payload contains the slots
     * the binding actually needs. {0} is always required when format_string is selected;
     * {1} is required additionally when a max_attribute is bound.
     * Accepts both bare slot ({0}) and typed slot ({0:int}) forms.
     */
    bool ValidateFormatStringPayload(const FString& Payload, bool bHasMaxAttribute, FString& OutError)
    {
        const bool bHas0 = Payload.Contains(TEXT("{0}")) || Payload.Contains(TEXT("{0:int}"));
        if (!bHas0)
        {
            OutError = FString::Printf(
                TEXT("format=format_string requires '{0}' (and '{1}' if max_attribute set) in template, got: %s"),
                *Payload);
            return false;
        }
        if (bHasMaxAttribute)
        {
            const bool bHas1 = Payload.Contains(TEXT("{1}")) || Payload.Contains(TEXT("{1:int}"));
            if (!bHas1)
            {
                OutError = FString::Printf(
                    TEXT("format=format_string requires '{0}' (and '{1}' if max_attribute set) in template, got: %s"),
                    *Payload);
                return false;
            }
        }
        return true;
    }

    FString FormatToString(EMonolithAttrBindFormat F)
    {
        switch (F)
        {
            case EMonolithAttrBindFormat::Auto:           return TEXT("auto");
            case EMonolithAttrBindFormat::PercentZeroOne: return TEXT("percent_0_1");
            case EMonolithAttrBindFormat::IntText:        return TEXT("int_text");
            case EMonolithAttrBindFormat::FloatText:      return TEXT("float_text");
            case EMonolithAttrBindFormat::FormatString:   return TEXT("format_string");
            case EMonolithAttrBindFormat::Gradient:       return TEXT("gradient");
            case EMonolithAttrBindFormat::Threshold:      return TEXT("threshold");
        }
        return TEXT("auto");
    }

    EMonolithAttrBindUpdate ParseUpdate(const FString& S, float& OutSmoothing)
    {
        if (S.IsEmpty() || S.Equals(TEXT("on_change"), ESearchCase::IgnoreCase)) return EMonolithAttrBindUpdate::OnChange;
        if (S.Equals(TEXT("tick"), ESearchCase::IgnoreCase)) return EMonolithAttrBindUpdate::Tick;
        if (S.StartsWith(TEXT("on_change_smoothed"), ESearchCase::IgnoreCase))
        {
            FString L, R;
            if (S.Split(TEXT(":"), &L, &R) && !R.IsEmpty()) OutSmoothing = FCString::Atof(*R);
            return EMonolithAttrBindUpdate::OnChangeSmoothed;
        }
        return EMonolithAttrBindUpdate::OnChange;
    }

    FString UpdateToString(EMonolithAttrBindUpdate U)
    {
        switch (U)
        {
            case EMonolithAttrBindUpdate::OnChange:         return TEXT("on_change");
            case EMonolithAttrBindUpdate::Tick:             return TEXT("tick");
            case EMonolithAttrBindUpdate::OnChangeSmoothed: return TEXT("on_change_smoothed");
        }
        return TEXT("on_change");
    }

    /** Validate (widget_class, target_property) is a supported pair. Adds warnings for borderline cases. */
    bool ValidateWidgetProperty(UWidget* Widget, FName Prop, EMonolithAttrBindFormat& Format,
                                bool bHasMaxAttribute, TArray<FString>& Warnings, FString& OutError)
    {
        if (!Widget)
        {
            OutError = TEXT("Target widget is null");
            return false;
        }
        UClass* C = Widget->GetClass();
        const FString WClass = C->GetName();

        // ProgressBar.Percent
        if (C->IsChildOf(UProgressBar::StaticClass()) && Prop == TEXT("Percent"))
        {
            if (!bHasMaxAttribute)
            {
                Warnings.Add(TEXT("ProgressBar.Percent without max_attribute — assuming attribute already in [0,1]"));
            }
            if (Format == EMonolithAttrBindFormat::Auto) Format = EMonolithAttrBindFormat::PercentZeroOne;
            return true;
        }

        // Text on text-bearing widgets
        if (Prop == TEXT("Text") && (
                C->IsChildOf(UTextBlock::StaticClass())
             || C->IsChildOf(URichTextBlock::StaticClass())
             || C->IsChildOf(UEditableText::StaticClass())
             || C->IsChildOf(UEditableTextBox::StaticClass())))
        {
            if (Format == EMonolithAttrBindFormat::Auto)
            {
                Format = bHasMaxAttribute ? EMonolithAttrBindFormat::FormatString : EMonolithAttrBindFormat::IntText;
            }
            if (C->IsChildOf(UEditableText::StaticClass()) || C->IsChildOf(UEditableTextBox::StaticClass()))
            {
                Warnings.Add(TEXT("Binding to EditableText/EditableTextBox.Text — set bIsReadOnly=true to prevent typing being stomped"));
            }
            return true;
        }

        // Image.ColorAndOpacity (gradient required)
        if (C->IsChildOf(UImage::StaticClass()) && Prop == TEXT("ColorAndOpacity"))
        {
            if (Format != EMonolithAttrBindFormat::Gradient)
            {
                OutError = TEXT("Image.ColorAndOpacity requires format=gradient (e.g. \"gradient:1,0,0,1|0,1,0,1\") — a single attribute can't fill 4 channels");
                return false;
            }
            return true;
        }
        // Border.BrushColor
        if (C->IsChildOf(UBorder::StaticClass()) && Prop == TEXT("BrushColor"))
        {
            if (Format != EMonolithAttrBindFormat::Gradient)
            {
                OutError = TEXT("Border.BrushColor requires format=gradient");
                return false;
            }
            return true;
        }

        // Generic Widget.RenderOpacity
        if (Prop == TEXT("RenderOpacity"))
        {
            if (Format == EMonolithAttrBindFormat::Auto) Format = EMonolithAttrBindFormat::PercentZeroOne;
            return true;
        }
        // Generic Widget.Visibility
        if (Prop == TEXT("Visibility"))
        {
            if (Format == EMonolithAttrBindFormat::Auto) Format = EMonolithAttrBindFormat::Threshold;
            return true;
        }
        // Slider/SpinBox.Value
        if (Prop == TEXT("Value") && (
                C->IsChildOf(USlider::StaticClass())
             || C->IsChildOf(USpinBox::StaticClass())))
        {
            if (Format == EMonolithAttrBindFormat::Auto) Format = EMonolithAttrBindFormat::FloatText;
            Warnings.Add(TEXT("Binding to Slider/SpinBox.Value — user input will be stomped each frame"));
            return true;
        }
        // CheckBox.CheckedState
        if (C->IsChildOf(UCheckBox::StaticClass()) && Prop == TEXT("CheckedState"))
        {
            if (Format == EMonolithAttrBindFormat::Auto) Format = EMonolithAttrBindFormat::Threshold;
            return true;
        }

        // Phase J F5 #5: enrich with the per-widget Valid-property list.
        OutError = FString::Printf(
            TEXT("Property '%s' invalid for %s. Valid: %s. See Docs/plans/2026-04-26-ui-gas-attribute-binding.md compatibility table."),
            *Prop.ToString(), *WClass, *BuildValidPropertiesClause(C));
        return false;
    }

    UMonolithGASUIBindingBlueprintExtension* GetOrCreateExtension(UWidgetBlueprint* WBP)
    {
        if (!WBP) return nullptr;
        UMonolithGASUIBindingBlueprintExtension* Existing =
            UWidgetBlueprintExtension::GetExtension<UMonolithGASUIBindingBlueprintExtension>(WBP);
        if (Existing) return Existing;
        return UWidgetBlueprintExtension::RequestExtension<UMonolithGASUIBindingBlueprintExtension>(WBP);
    }

    void CompileAndSaveWBP(UWidgetBlueprint* WBP, const FString& AssetPath, bool& bOutCompiled, bool& bOutSaved)
    {
        bOutCompiled = false;
        bOutSaved = false;
        if (!WBP) return;
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        FKismetEditorUtilities::CompileBlueprint(WBP);
        bOutCompiled = true;

        WBP->GetPackage()->MarkPackageDirty();
        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        const FString FileName = FPackageName::LongPackageNameToFilename(AssetPath, FPackageName::GetAssetPackageExtension());
        UPackage::SavePackage(WBP->GetPackage(), WBP, *FileName, SaveArgs);
        bOutSaved = true;
    }

    TSharedPtr<FJsonObject> SerializeBindingRow(const FMonolithGASAttributeBindingSpec& Spec, int32 Index, UWidgetBlueprint* WBP)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        // Phase J F5 #1: rename `index` to `binding_index` to match the bind-response field
        // (was the only inconsistency; both shapes now agree).
        O->SetNumberField(TEXT("binding_index"), Index);
        O->SetStringField(TEXT("widget_name"), Spec.TargetWidgetName.ToString());
        // Phase J F5 #3: include `widget_class` so callers can filter without a separate ui::get_widget_class call.
        if (UWidget* W = FindWidgetInWBP(WBP, Spec.TargetWidgetName))
        {
            if (UClass* WC = W->GetClass())
            {
                O->SetStringField(TEXT("widget_class"), WC->GetName());
            }
        }
        O->SetStringField(TEXT("target_property"), Spec.TargetPropertyName.ToString());
        // Phase J F5 #2: synthesize composite `attribute` (and `max_attribute`) alongside the split form
        // for round-trip parity with the bind-input contract. Split fields kept for back-compat.
        const FString AttrComposite = BuildAttributeComposite(Spec.AttributeSetClassPath, Spec.AttributePropertyName);
        if (!AttrComposite.IsEmpty()) O->SetStringField(TEXT("attribute"), AttrComposite);
        O->SetStringField(TEXT("attribute_set_class"), Spec.AttributeSetClassPath);
        O->SetStringField(TEXT("attribute_name"), Spec.AttributePropertyName.ToString());
        if (!Spec.MaxAttributePropertyName.IsNone())
        {
            const FString MaxComposite = BuildAttributeComposite(Spec.MaxAttributeSetClassPath, Spec.MaxAttributePropertyName);
            if (!MaxComposite.IsEmpty()) O->SetStringField(TEXT("max_attribute"), MaxComposite);
            O->SetStringField(TEXT("max_attribute_set_class"), Spec.MaxAttributeSetClassPath);
            O->SetStringField(TEXT("max_attribute_name"), Spec.MaxAttributePropertyName.ToString());
        }
        O->SetStringField(TEXT("owner_resolver"), OwnerToString(Spec.Owner));
        if (!Spec.OwnerSocketTag.IsNone())
        {
            O->SetStringField(TEXT("owner_socket_tag"), Spec.OwnerSocketTag.ToString());
        }
        O->SetStringField(TEXT("format"), FormatToString(Spec.Format));
        if (!Spec.FormatPayload.IsEmpty())
        {
            O->SetStringField(TEXT("format_payload"), Spec.FormatPayload);
        }
        O->SetStringField(TEXT("update_policy"), UpdateToString(Spec.UpdatePolicy));
        O->SetNumberField(TEXT("smoothing_speed"), Spec.SmoothingSpeed);
        return O;
    }
}

// =================================================================================
//  Registration
// =================================================================================

void FMonolithGASUIBindingActions::RegisterActions(FMonolithToolRegistry& Registry)
{
    auto BuildBindSchema = []()
    {
        return FParamSchemaBuilder()
            .Required(TEXT("wbp_path"),         TEXT("string"),  TEXT("Widget Blueprint asset path (e.g. /Game/UI/WBP_HUD)"), { TEXT("asset_path") })
            .Required(TEXT("widget_name"),      TEXT("string"),  TEXT("Widget tree variable name (e.g. HealthBar)"))
            .Required(TEXT("target_property"),  TEXT("string"),  TEXT("Property on the widget (Percent, Text, ColorAndOpacity, RenderOpacity, Visibility, Value, BrushColor, CheckedState)"))
            .Required(TEXT("attribute"),        TEXT("string"),  TEXT("Attribute as ClassName.PropertyName (e.g. ULeviathanVitalsSet.Health)"), { TEXT("attribute_name") })
            .Optional(TEXT("max_attribute"),    TEXT("string"),  TEXT("Optional second attribute used as denominator. Required for ProgressBar.Percent if attribute > 1."))
            .Optional(TEXT("owner_resolver"),   TEXT("string"),  TEXT("ASC owner: owning_player_pawn (default) | owning_player_state | owning_player_controller | self_actor | named_socket:<Tag>"), TEXT("owning_player_pawn"))
            .Optional(TEXT("format"),           TEXT("string"),  TEXT("auto | percent_0_1 | int_text | float_text:<decimals> | format_string:<tpl> | gradient:<lo>|<hi> | threshold:<T>"), TEXT("auto"))
            .Optional(TEXT("update_policy"),    TEXT("string"),  TEXT("on_change (default) | tick | on_change_smoothed:<lerp_speed>"), TEXT("on_change"))
            .Optional(TEXT("replace_existing"), TEXT("boolean"), TEXT("If a binding already exists for (widget_name, target_property), overwrite. Default: true"), TEXT("true"))
            .Build();
    };

    auto BuildUnbindSchema = []()
    {
        return FParamSchemaBuilder()
            .Required(TEXT("wbp_path"),        TEXT("string"), TEXT("Widget Blueprint asset path"), { TEXT("asset_path") })
            .Required(TEXT("widget_name"),     TEXT("string"), TEXT("Widget tree variable name"))
            .Required(TEXT("target_property"), TEXT("string"), TEXT("Property on the widget"))
            .Build();
    };

    auto BuildListSchema = []()
    {
        return FParamSchemaBuilder()
            .Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint asset path"), { TEXT("asset_path") })
            .Build();
    };

    auto BuildClearSchema = []()
    {
        return FParamSchemaBuilder()
            .Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint asset path"), { TEXT("asset_path") })
            .Build();
    };

    const FString BindDesc = TEXT("Bind a UMG widget property to a GAS attribute. Installs a runtime class extension on the WBP that subscribes to UAbilitySystemComponent::GetGameplayAttributeValueChangeDelegate at NativeConstruct and pushes typed values to the target widget property. Compiles + saves the WBP. NOTE: complementary to ui::get_widget_bindings, which reads the orthogonal UMG property-binding system.");
    const FString UnbindDesc = TEXT("Remove one GAS attribute binding from a WBP, keyed by (widget_name, target_property). Recompiles + saves.");
    const FString ListDesc = TEXT("List all GAS attribute bindings on a WBP installed by gas::bind_widget_to_attribute. Distinct from ui::get_widget_bindings which reads UMG's FDelegateRuntimeBinding array.");
    const FString ClearDesc = TEXT("Remove ALL GAS attribute bindings from a WBP. Recompiles + saves. Returns count removed.");

    // ---- gas namespace (canonical) ----
    Registry.RegisterAction(TEXT("gas"), TEXT("bind_widget_to_attribute"),    BindDesc,
        FMonolithActionHandler::CreateStatic(&HandleBindWidgetToAttribute), BuildBindSchema());
    Registry.RegisterAction(TEXT("gas"), TEXT("unbind_widget_attribute"),    UnbindDesc,
        FMonolithActionHandler::CreateStatic(&HandleUnbindWidgetAttribute), BuildUnbindSchema());
    Registry.RegisterAction(TEXT("gas"), TEXT("list_attribute_bindings"),    ListDesc,
        FMonolithActionHandler::CreateStatic(&HandleListAttributeBindings), BuildListSchema());
    Registry.RegisterAction(TEXT("gas"), TEXT("clear_widget_attribute_bindings"), ClearDesc,
        FMonolithActionHandler::CreateStatic(&HandleClearWidgetAttributeBindings), BuildClearSchema());

    // ---- ui namespace (alias — same handlers, mirror schemas) ----
    Registry.RegisterAction(TEXT("ui"), TEXT("bind_widget_to_attribute"),    BindDesc + TEXT(" [alias of gas::bind_widget_to_attribute]"),
        FMonolithActionHandler::CreateStatic(&HandleBindWidgetToAttribute), BuildBindSchema());
    Registry.RegisterAction(TEXT("ui"), TEXT("unbind_widget_attribute"),    UnbindDesc + TEXT(" [alias of gas::unbind_widget_attribute]"),
        FMonolithActionHandler::CreateStatic(&HandleUnbindWidgetAttribute), BuildUnbindSchema());
    Registry.RegisterAction(TEXT("ui"), TEXT("list_attribute_bindings"),    ListDesc + TEXT(" [alias of gas::list_attribute_bindings]"),
        FMonolithActionHandler::CreateStatic(&HandleListAttributeBindings), BuildListSchema());
    Registry.RegisterAction(TEXT("ui"), TEXT("clear_widget_attribute_bindings"), ClearDesc + TEXT(" [alias of gas::clear_widget_attribute_bindings]"),
        FMonolithActionHandler::CreateStatic(&HandleClearWidgetAttributeBindings), BuildClearSchema());
}

// =================================================================================
//  bind_widget_to_attribute
// =================================================================================

FMonolithActionResult FMonolithGASUIBindingActions::HandleBindWidgetToAttribute(const TSharedPtr<FJsonObject>& Params)
{
    FString WbpPath, WidgetNameStr, TargetPropStr, AttrStr;
    FMonolithActionResult Err;
    if (!MonolithGAS::RequireStringParam(Params, TEXT("wbp_path"),        WbpPath,       Err)) return Err;
    if (!MonolithGAS::RequireStringParam(Params, TEXT("widget_name"),     WidgetNameStr, Err)) return Err;
    if (!MonolithGAS::RequireStringParam(Params, TEXT("target_property"), TargetPropStr, Err)) return Err;
    if (!MonolithGAS::RequireStringParam(Params, TEXT("attribute"),       AttrStr,       Err)) return Err;

    FString MaxAttrStr;          Params->TryGetStringField(TEXT("max_attribute"), MaxAttrStr);
    FString OwnerStr           = TEXT("owning_player_pawn");  Params->TryGetStringField(TEXT("owner_resolver"), OwnerStr);
    FString FormatStr          = TEXT("auto");                Params->TryGetStringField(TEXT("format"), FormatStr);
    FString UpdateStr          = TEXT("on_change");           Params->TryGetStringField(TEXT("update_policy"), UpdateStr);
    bool    bReplaceExisting   = true;                        Params->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);

    FString LoadErr;
    UWidgetBlueprint* WBP = LoadWBP(WbpPath, LoadErr);
    if (!WBP) return FMonolithActionResult::Error(LoadErr);

    UWidget* TargetWidget = FindWidgetInWBP(WBP, FName(*WidgetNameStr));
    if (!TargetWidget)
    {
        // Phase J F5 #5: enrich with widget-tree enumeration so callers can pick the right name in one round-trip.
        return FMonolithActionResult::Error(FString::Printf(
            TEXT("Widget '%s' not found in WBP tree. Available: %s"),
            *WidgetNameStr, *BuildAvailableWidgetsClause(WBP)));
    }

    // Resolve attribute (and optional max).
    FString AttrSetPath; FName AttrName;
    FString ParseErr;
    if (!ParseAttributeString(AttrStr, AttrSetPath, AttrName, ParseErr))
    {
        return FMonolithActionResult::Error(ParseErr);
    }

    FString MaxAttrSetPath; FName MaxAttrName;
    bool bHasMax = false;
    if (!MaxAttrStr.IsEmpty())
    {
        FString MaxParseErr;
        if (!ParseAttributeString(MaxAttrStr, MaxAttrSetPath, MaxAttrName, MaxParseErr))
        {
            return FMonolithActionResult::Error(FString::Printf(TEXT("max_attribute parse error: %s"), *MaxParseErr));
        }
        bHasMax = true;
    }

    // Format + payload.
    FString FormatPayload;
    EMonolithAttrBindFormat Format = ParseFormat(FormatStr, FormatPayload);

    // Phase J F3: pre-validate user-supplied format_string payloads for required slots.
    if (Format == EMonolithAttrBindFormat::FormatString)
    {
        FString FmtErr;
        if (!ValidateFormatStringPayload(FormatPayload, bHasMax, FmtErr))
        {
            return FMonolithActionResult::Error(FmtErr);
        }
    }

    // Validate widget property compatibility (may auto-resolve Format).
    TArray<FString> Warnings;
    FString ValidErr;
    if (!ValidateWidgetProperty(TargetWidget, FName(*TargetPropStr), Format, bHasMax, Warnings, ValidErr))
    {
        return FMonolithActionResult::Error(ValidErr);
    }

    // Phase J F3 (post-validate guard): if ValidateWidgetProperty auto-promoted Format to FormatString
    // (Text widget + max_attribute bound), the payload was never user-supplied. Error with guidance.
    if (Format == EMonolithAttrBindFormat::FormatString && FormatPayload.IsEmpty())
    {
        return FMonolithActionResult::Error(
            TEXT("format=auto resolved to format_string for this widget but no template was supplied. "
                 "Pass format=format_string:<template> with '{0}' (and '{1}' if max_attribute set)."));
    }

    // Update policy.
    float Smoothing = 6.f;
    EMonolithAttrBindUpdate UpdatePolicy = ParseUpdate(UpdateStr, Smoothing);

    // Owner resolver. Honor "named_socket:<Tag>" payload.
    // Phase J F2: ParseOwner now validates — invalid non-empty owner_resolver errors out instead of silently coercing to OwningPlayerPawn.
    EMonolithAttrBindOwner Owner = EMonolithAttrBindOwner::OwningPlayerPawn;
    FName OwnerTag;
    FString OwnerErr;
    if (!ParseOwner(OwnerStr, Owner, OwnerTag, OwnerErr))
    {
        return FMonolithActionResult::Error(OwnerErr);
    }

    // Build the spec.
    FMonolithGASAttributeBindingSpec Spec;
    Spec.TargetWidgetName = FName(*WidgetNameStr);
    Spec.TargetPropertyName = FName(*TargetPropStr);
    Spec.AttributeSetClassPath = AttrSetPath;
    Spec.AttributePropertyName = AttrName;
    if (bHasMax)
    {
        Spec.MaxAttributeSetClassPath = MaxAttrSetPath;
        Spec.MaxAttributePropertyName = MaxAttrName;
    }
    Spec.Owner = Owner;
    Spec.OwnerSocketTag = OwnerTag;
    Spec.Format = Format;
    Spec.FormatPayload = FormatPayload;
    Spec.UpdatePolicy = UpdatePolicy;
    Spec.SmoothingSpeed = Smoothing;

    // Get-or-create the editor-side extension and stash the row.
    UMonolithGASUIBindingBlueprintExtension* Ext = GetOrCreateExtension(WBP);
    if (!Ext)
    {
        return FMonolithActionResult::Error(TEXT("Failed to obtain UMonolithGASUIBindingBlueprintExtension"));
    }

    bool bReplaced = false;
    int32 Idx = Ext->AddOrReplaceBinding(Spec, bReplaceExisting, bReplaced);
    if (Idx == INDEX_NONE)
    {
        return FMonolithActionResult::Error(FString::Printf(
            TEXT("Binding (%s, %s) already exists and replace_existing=false"),
            *WidgetNameStr, *TargetPropStr));
    }

    // Compile + save.
    bool bCompiled = false, bSaved = false;
    CompileAndSaveWBP(WBP, WbpPath, bCompiled, bSaved);

    // Result.
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("ok"), true);
    Result->SetStringField(TEXT("wbp_path"), WbpPath);
    Result->SetStringField(TEXT("widget_name"), WidgetNameStr);
    Result->SetStringField(TEXT("widget_class"), TargetWidget->GetClass()->GetName());
    Result->SetStringField(TEXT("target_property"), TargetPropStr);
    Result->SetStringField(TEXT("attribute"), AttrStr);
    if (bHasMax) Result->SetStringField(TEXT("max_attribute"), MaxAttrStr);
    Result->SetStringField(TEXT("format"), FormatToString(Format));
    if (!FormatPayload.IsEmpty()) Result->SetStringField(TEXT("format_payload"), FormatPayload);
    Result->SetStringField(TEXT("owner_resolver"), OwnerToString(Owner));
    Result->SetStringField(TEXT("update_policy"), UpdateToString(UpdatePolicy));
    Result->SetStringField(TEXT("extension_class"), TEXT("UMonolithGASAttributeBindingClassExtension"));
    Result->SetNumberField(TEXT("binding_index"), Idx);
    Result->SetBoolField(TEXT("replaced"), bReplaced);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    Result->SetBoolField(TEXT("saved"), bSaved);

    if (Warnings.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& W : Warnings) Arr.Add(MakeShared<FJsonValueString>(W));
        Result->SetArrayField(TEXT("warnings"), Arr);
    }

    // Phase J F9: handler-entry/exit observability. `replaced=true` is augmented onto the same line
    // so callers can grep one log to know whether the bind clobbered an existing row.
    UE_LOG(LogMonolithGAS, Log,
        TEXT("[GASBind] BindWidget: WBP=%s widget=%s property=%s attr=%s -> binding_index=%d replaced=%s"),
        *WbpPath, *WidgetNameStr, *TargetPropStr, *AttrStr, Idx, bReplaced ? TEXT("true") : TEXT("false"));

    return FMonolithActionResult::Success(Result);
}

// =================================================================================
//  unbind_widget_attribute
// =================================================================================

FMonolithActionResult FMonolithGASUIBindingActions::HandleUnbindWidgetAttribute(const TSharedPtr<FJsonObject>& Params)
{
    FString WbpPath, WidgetNameStr, TargetPropStr;
    FMonolithActionResult Err;
    if (!MonolithGAS::RequireStringParam(Params, TEXT("wbp_path"),        WbpPath,       Err)) return Err;
    if (!MonolithGAS::RequireStringParam(Params, TEXT("widget_name"),     WidgetNameStr, Err)) return Err;
    if (!MonolithGAS::RequireStringParam(Params, TEXT("target_property"), TargetPropStr, Err)) return Err;

    FString LoadErr;
    UWidgetBlueprint* WBP = LoadWBP(WbpPath, LoadErr);
    if (!WBP) return FMonolithActionResult::Error(LoadErr);

    UMonolithGASUIBindingBlueprintExtension* Ext =
        UWidgetBlueprintExtension::GetExtension<UMonolithGASUIBindingBlueprintExtension>(WBP);
    if (!Ext)
    {
        return FMonolithActionResult::Error(TEXT("No GAS attribute bindings on this WBP"));
    }

    // Phase J F5 #4: capture the pre-removal index so the response can echo `removed_binding_index`
    // for log/audit. Reuses the existing IndexOfBinding accessor — no need to refactor RemoveBinding.
    const int32 RemovedIdx = Ext->IndexOfBinding(FName(*WidgetNameStr), FName(*TargetPropStr));
    bool bRemoved = Ext->RemoveBinding(FName(*WidgetNameStr), FName(*TargetPropStr));
    if (!bRemoved)
    {
        return FMonolithActionResult::Error(FString::Printf(
            TEXT("Binding (%s, %s) not found"), *WidgetNameStr, *TargetPropStr));
    }

    bool bCompiled = false, bSaved = false;
    CompileAndSaveWBP(WBP, WbpPath, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("ok"), true);
    Result->SetStringField(TEXT("wbp_path"), WbpPath);
    Result->SetStringField(TEXT("widget_name"), WidgetNameStr);
    Result->SetStringField(TEXT("target_property"), TargetPropStr);
    Result->SetBoolField(TEXT("removed"), true);
    Result->SetNumberField(TEXT("removed_binding_index"), RemovedIdx);
    Result->SetNumberField(TEXT("remaining_count"), Ext->Bindings.Num());
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    Result->SetBoolField(TEXT("saved"), bSaved);

    // Phase J F9: handler-entry/exit observability.
    UE_LOG(LogMonolithGAS, Log,
        TEXT("[GASBind] UnbindWidget: WBP=%s widget=%s property=%s -> removed_index=%d"),
        *WbpPath, *WidgetNameStr, *TargetPropStr, RemovedIdx);

    return FMonolithActionResult::Success(Result);
}

// =================================================================================
//  list_attribute_bindings
// =================================================================================

FMonolithActionResult FMonolithGASUIBindingActions::HandleListAttributeBindings(const TSharedPtr<FJsonObject>& Params)
{
    FString WbpPath;
    FMonolithActionResult Err;
    if (!MonolithGAS::RequireStringParam(Params, TEXT("wbp_path"), WbpPath, Err)) return Err;

    FString LoadErr;
    UWidgetBlueprint* WBP = LoadWBP(WbpPath, LoadErr);
    if (!WBP) return FMonolithActionResult::Error(LoadErr);

    UMonolithGASUIBindingBlueprintExtension* Ext =
        UWidgetBlueprintExtension::GetExtension<UMonolithGASUIBindingBlueprintExtension>(WBP);

    TArray<TSharedPtr<FJsonValue>> Arr;
    if (Ext)
    {
        for (int32 i = 0; i < Ext->Bindings.Num(); ++i)
        {
            Arr.Add(MakeShared<FJsonValueObject>(SerializeBindingRow(Ext->Bindings[i], i, WBP)));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("ok"), true);
    Result->SetStringField(TEXT("wbp_path"), WbpPath);
    Result->SetArrayField(TEXT("bindings"), Arr);
    Result->SetNumberField(TEXT("count"), Arr.Num());
    Result->SetStringField(TEXT("note"),
        TEXT("These are GAS attribute bindings (Monolith). Distinct from ui::get_widget_bindings which reads UMG FDelegateRuntimeBinding."));

    // Phase J F9: read-only listing — Verbose so it doesn't spam under frequent UI inspection traffic.
    UE_LOG(LogMonolithGAS, Verbose,
        TEXT("[GASBind] ListBindings: WBP=%s count=%d"), *WbpPath, Arr.Num());

    return FMonolithActionResult::Success(Result);
}

// =================================================================================
//  clear_widget_attribute_bindings
// =================================================================================

FMonolithActionResult FMonolithGASUIBindingActions::HandleClearWidgetAttributeBindings(const TSharedPtr<FJsonObject>& Params)
{
    FString WbpPath;
    FMonolithActionResult Err;
    if (!MonolithGAS::RequireStringParam(Params, TEXT("wbp_path"), WbpPath, Err)) return Err;

    FString LoadErr;
    UWidgetBlueprint* WBP = LoadWBP(WbpPath, LoadErr);
    if (!WBP) return FMonolithActionResult::Error(LoadErr);

    UMonolithGASUIBindingBlueprintExtension* Ext =
        UWidgetBlueprintExtension::GetExtension<UMonolithGASUIBindingBlueprintExtension>(WBP);
    int32 Removed = 0;
    if (Ext)
    {
        Removed = Ext->ClearBindings();
    }

    bool bCompiled = false, bSaved = false;
    if (Removed > 0)
    {
        CompileAndSaveWBP(WBP, WbpPath, bCompiled, bSaved);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("ok"), true);
    Result->SetStringField(TEXT("wbp_path"), WbpPath);
    Result->SetNumberField(TEXT("removed_count"), Removed);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    Result->SetBoolField(TEXT("saved"), bSaved);

    // Phase J F9: handler-entry/exit observability.
    UE_LOG(LogMonolithGAS, Log,
        TEXT("[GASBind] ClearBindings: WBP=%s -> removed=%d"), *WbpPath, Removed);

    return FMonolithActionResult::Success(Result);
}
