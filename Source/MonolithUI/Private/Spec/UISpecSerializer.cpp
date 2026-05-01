// Copyright tumourlove. All Rights Reserved.
// UISpecSerializer.cpp
//
// Phase J -- inverse of UISpecBuilder. Reads a live UWidgetBlueprint and
// produces an FUISpecDocument such that Build(Dump(WBP)) reconstructs the
// same WBP up to the documented lossy boundary (header).
//
// Pipeline:
//   1. Load WBP (already-loaded variant skips this).
//   2. Capture document-level metadata (Name, ParentClass, Version).
//   3. Walk WidgetTree from RootWidget; per node build FUISpecNode with:
//        - Type token (registry-mapped or fallback to GetClass()->GetName())
//        - Id (widget FName)
//        - Slot (per UPanelSlot derived class -- 12+ branches)
//        - Style (curated reflection reads through ReadJsonPath)
//        - Content (TextBlock/RichTextBlock/Image/EditableText/Button etc.)
//        - Effect (UEffectSurface -- corner radii / fill / shadow arrays)
//        - CommonUI (#if WITH_COMMONUI)
//   4. Capture WBP->Animations into FUISpecAnimation entries.
//   5. Produce FUISpecDocument with non-null Root.
//
// Atomicity: pure read; no asset mutation. The UPackage's dirty flag stays
// untouched. Callers can dump dozens of WBPs in a row without forcing saves.
//
// Clean-room: per-slot-class branch table + per-sub-bag readers are OURS.
// The shape mirrors the build-side dispatch -- for every Apply* on the
// builder there's a matching Read* here.

#include "Spec/UISpecSerializer.h"

#include "MonolithUICommon.h"
#include "Registry/MonolithUIRegistrySubsystem.h"
#include "Registry/UITypeRegistry.h"
#include "Registry/UIPropertyAllowlist.h"
#include "Registry/UIPropertyPathCache.h"
#include "Registry/UIReflectionHelper.h"

// Widget Blueprint / UMG core
#include "WidgetBlueprint.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"

// Stock UMG widgets
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/GridPanel.h"
#include "Components/GridSlot.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/SizeBox.h"
#include "Components/SizeBoxSlot.h"
#include "Components/ScaleBox.h"
#include "Components/ScaleBoxSlot.h"
#include "Components/WrapBox.h"
#include "Components/WrapBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Components/WidgetSwitcherSlot.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/BackgroundBlur.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/RichTextBlock.h"
#include "Components/Image.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/ProgressBar.h"
#include "Components/Slider.h"

// Animation
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "MovieScene.h"

// Effect surface -- optional provider decouple (R3): no typed includes here.
// Access flows through MonolithUI::GetEffectSurfaceClass() (UClass* probe) +
// the curated `Effect.*` JSON path mapping in MonolithUIRegistrySubsystem.
// See the optional provider decouple notes in the Monolith UI spec.

// CommonUI (optional)
#if WITH_COMMONUI
#include "CommonActivatableWidget.h"
#include "CommonUserWidget.h"
#include "CommonButtonBase.h"
#endif

// JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Misc
#include "Layout/Margin.h"
#include "Styling/SlateColor.h"
#include "Fonts/SlateFontInfo.h"

// FProperty descent for reflective EffectSurface reads (R3 — replaces the
// pre-R3 typed `Cast<UEffectSurface>` + `Config.*` field reads).
#include "UObject/UnrealType.h"
#include "UObject/Field.h"

namespace MonolithUI::SpecSerializerInternal
{
    /**
     * Map a UClass to a registry token (e.g. UVerticalBox -> "VerticalBox").
     * Falls back to MakeTokenFromClassName when registry is unavailable.
     * Pure helper -- never touches mutable state.
     */
    static FName ClassToToken(const UClass* WidgetClass)
    {
        if (!WidgetClass) return NAME_None;

        if (UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get())
        {
            if (const FUITypeRegistryEntry* Entry = Sub->GetTypeRegistry().FindByClass(WidgetClass))
            {
                return Entry->Token;
            }
        }
        return MonolithUI::MakeTokenFromClassName(WidgetClass);
    }

    /** Convert EHorizontalAlignment to the spec-side token (Left/Center/Right/Fill). */
    static FName HAlignToToken(EHorizontalAlignment H)
    {
        switch (H)
        {
        case HAlign_Left:   return FName(TEXT("Left"));
        case HAlign_Center: return FName(TEXT("Center"));
        case HAlign_Right:  return FName(TEXT("Right"));
        case HAlign_Fill:   return FName(TEXT("Fill"));
        default:            return FName(TEXT("Fill"));
        }
    }

    /** Convert EVerticalAlignment to the spec-side token (Top/Center/Bottom/Fill). */
    static FName VAlignToToken(EVerticalAlignment V)
    {
        switch (V)
        {
        case VAlign_Top:    return FName(TEXT("Top"));
        case VAlign_Center: return FName(TEXT("Center"));
        case VAlign_Bottom: return FName(TEXT("Bottom"));
        case VAlign_Fill:   return FName(TEXT("Fill"));
        default:            return FName(TEXT("Fill"));
        }
    }

    /** Convert ESlateVisibility to the spec-side token. */
    static FName VisibilityToToken(ESlateVisibility V)
    {
        switch (V)
        {
        case ESlateVisibility::Visible:                return FName(TEXT("Visible"));
        case ESlateVisibility::Collapsed:              return FName(TEXT("Collapsed"));
        case ESlateVisibility::Hidden:                 return FName(TEXT("Hidden"));
        case ESlateVisibility::HitTestInvisible:       return FName(TEXT("HitTestInvisible"));
        case ESlateVisibility::SelfHitTestInvisible:   return FName(TEXT("SelfHitTestInvisible"));
        default:                                       return FName(TEXT("Visible"));
        }
    }

    /** Reverse-map FAnchors to the curated builder-side anchorPreset tokens. */
    static FName AnchorPresetFromAnchors(const FAnchors& Anchors)
    {
        static const TCHAR* PresetNames[] =
        {
            TEXT("top_left"),
            TEXT("top_center"),
            TEXT("top_right"),
            TEXT("center_left"),
            TEXT("center"),
            TEXT("center_right"),
            TEXT("bottom_left"),
            TEXT("bottom_center"),
            TEXT("bottom_right"),
            TEXT("stretch_top"),
            TEXT("stretch_bottom"),
            TEXT("stretch_left"),
            TEXT("stretch_right"),
            TEXT("stretch_horizontal"),
            TEXT("stretch_vertical"),
            TEXT("stretch_fill"),
        };

        for (const TCHAR* PresetName : PresetNames)
        {
            const FAnchors Candidate = MonolithUI::GetAnchorPreset(FString(PresetName));
            if (Anchors.Minimum == Candidate.Minimum
                && Anchors.Maximum == Candidate.Maximum)
            {
                return FName(PresetName);
            }
        }

        return NAME_None;
    }

    // -------------------------------------------------------------------------
    // SLOT SERIALIZATION
    // -------------------------------------------------------------------------
    // One branch per UPanelSlot derived class. Closes surface-map gap §6.3.3
    // (legacy `SerializeSlotProperties` covered only Canvas/Vertical/Horizontal/
    // Overlay -- 4 of 12+).

    /** Read a slot into the FUISpecSlot sub-bag. Branches on slot UClass. */
    static void SerializeSlot(UPanelSlot* Slot, FUISpecSlot& OutSlot)
    {
        if (!Slot) return;

        // CanvasPanelSlot -- the most expressive slot type. Carries anchors,
        // offsets, alignment, z-order, autoSize.
        if (UCanvasPanelSlot* CS = Cast<UCanvasPanelSlot>(Slot))
        {
            const FAnchorData& Layout = CS->GetLayout();
            // Position = upper-left offset; Size = right-bottom delta when
            // anchors are non-stretched. We capture both directly so the
            // builder can rehydrate without re-deriving.
            OutSlot.Position  = FVector2D(Layout.Offsets.Left, Layout.Offsets.Top);
            OutSlot.Size      = FVector2D(Layout.Offsets.Right, Layout.Offsets.Bottom);
            OutSlot.Alignment = FVector2D(Layout.Alignment.X, Layout.Alignment.Y);
            OutSlot.bAutoSize = CS->GetAutoSize();
            OutSlot.ZOrder    = CS->GetZOrder();

            // AnchorPreset reverse-lookup: compare against the same curated
            // preset names accepted by FUISpecBuilder / GetAnchorPreset.
            OutSlot.AnchorPreset = AnchorPresetFromAnchors(Layout.Anchors);
            return;
        }

        // VerticalBoxSlot / HorizontalBoxSlot / OverlaySlot / ScrollBoxSlot /
        // SizeBoxSlot / ScaleBoxSlot / WrapBoxSlot / WidgetSwitcherSlot /
        // BorderSlot / GridSlot / UniformGridSlot all expose halign + valign +
        // (most) padding via getters.

        if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot))
        {
            OutSlot.HAlign  = HAlignToToken(VS->GetHorizontalAlignment());
            OutSlot.VAlign  = VAlignToToken(VS->GetVerticalAlignment());
            OutSlot.Padding = VS->GetPadding();
            return;
        }
        if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot))
        {
            OutSlot.HAlign  = HAlignToToken(HS->GetHorizontalAlignment());
            OutSlot.VAlign  = VAlignToToken(HS->GetVerticalAlignment());
            OutSlot.Padding = HS->GetPadding();
            return;
        }
        if (UOverlaySlot* OS = Cast<UOverlaySlot>(Slot))
        {
            OutSlot.HAlign  = HAlignToToken(OS->GetHorizontalAlignment());
            OutSlot.VAlign  = VAlignToToken(OS->GetVerticalAlignment());
            OutSlot.Padding = OS->GetPadding();
            return;
        }
        if (UScrollBoxSlot* SBS = Cast<UScrollBoxSlot>(Slot))
        {
            OutSlot.HAlign  = HAlignToToken(SBS->GetHorizontalAlignment());
            OutSlot.VAlign  = VAlignToToken(SBS->GetVerticalAlignment());
            OutSlot.Padding = SBS->GetPadding();
            return;
        }
        if (UGridSlot* GS = Cast<UGridSlot>(Slot))
        {
            OutSlot.HAlign  = HAlignToToken(GS->GetHorizontalAlignment());
            OutSlot.VAlign  = VAlignToToken(GS->GetVerticalAlignment());
            OutSlot.Padding = GS->GetPadding();
            // Grid coordinates fold into Position (row, col) and Size (rowSpan, colSpan).
            OutSlot.Position = FVector2D((double)GS->GetColumn(), (double)GS->GetRow());
            OutSlot.Size     = FVector2D((double)GS->GetColumnSpan(), (double)GS->GetRowSpan());
            OutSlot.ZOrder   = GS->GetLayer();
            return;
        }
        if (UUniformGridSlot* UGS = Cast<UUniformGridSlot>(Slot))
        {
            OutSlot.HAlign   = HAlignToToken(UGS->GetHorizontalAlignment());
            OutSlot.VAlign   = VAlignToToken(UGS->GetVerticalAlignment());
            OutSlot.Position = FVector2D((double)UGS->GetColumn(), (double)UGS->GetRow());
            return;
        }
        if (USizeBoxSlot* SZS = Cast<USizeBoxSlot>(Slot))
        {
            OutSlot.HAlign  = HAlignToToken(SZS->GetHorizontalAlignment());
            OutSlot.VAlign  = VAlignToToken(SZS->GetVerticalAlignment());
            OutSlot.Padding = SZS->GetPadding();
            return;
        }
        if (UScaleBoxSlot* SCS = Cast<UScaleBoxSlot>(Slot))
        {
            OutSlot.HAlign = HAlignToToken(SCS->GetHorizontalAlignment());
            OutSlot.VAlign = VAlignToToken(SCS->GetVerticalAlignment());
            // ScaleBoxSlot has no public Padding accessor (5.1+ deprecation).
            return;
        }
        if (UWrapBoxSlot* WBS = Cast<UWrapBoxSlot>(Slot))
        {
            OutSlot.HAlign  = HAlignToToken(WBS->GetHorizontalAlignment());
            OutSlot.VAlign  = VAlignToToken(WBS->GetVerticalAlignment());
            OutSlot.Padding = WBS->GetPadding();
            return;
        }
        if (UWidgetSwitcherSlot* WSS = Cast<UWidgetSwitcherSlot>(Slot))
        {
            OutSlot.HAlign  = HAlignToToken(WSS->GetHorizontalAlignment());
            OutSlot.VAlign  = VAlignToToken(WSS->GetVerticalAlignment());
            OutSlot.Padding = WSS->GetPadding();
            return;
        }
        if (UBorderSlot* BS = Cast<UBorderSlot>(Slot))
        {
            OutSlot.HAlign  = HAlignToToken(BS->GetHorizontalAlignment());
            OutSlot.VAlign  = VAlignToToken(BS->GetVerticalAlignment());
            OutSlot.Padding = BS->GetPadding();
            return;
        }

        // Unknown slot subclass -- silently leave OutSlot at default. The
        // rebuild side will use the panel's natural slot defaults. The
        // serializer caller may emit a warning; we don't from here because
        // we lack the Out-warning surface and the lossy boundary documents
        // this pattern.
    }

    // -------------------------------------------------------------------------
    // CONTENT SERIALIZATION
    // -------------------------------------------------------------------------
    // Per-leaf-widget content fields (text, brush, font, placeholder).

    static void SerializeContent(UWidget* Widget, FUISpecContent& OutContent)
    {
        if (!Widget) return;

        if (UTextBlock* T = Cast<UTextBlock>(Widget))
        {
            OutContent.Text     = T->GetText().ToString();
            OutContent.FontSize = (float)T->GetFont().Size;
            OutContent.FontColor = T->GetColorAndOpacity().GetSpecifiedColor();
            // WrapMode -- UE 5.7 made the AutoWrapText bitfield protected; use the
            // public getter `GetAutoWrapText()` instead of direct field access.
            OutContent.WrapMode = T->GetAutoWrapText() ? FName(TEXT("Wrap")) : FName(TEXT("None"));
            return;
        }
        if (URichTextBlock* RT = Cast<URichTextBlock>(Widget))
        {
            OutContent.Text = RT->GetText().ToString();
            return;
        }
        if (UImage* I = Cast<UImage>(Widget))
        {
            const FSlateBrush& B = I->GetBrush();
            if (B.GetResourceObject())
            {
                OutContent.BrushPath = B.GetResourceObject()->GetPathName();
            }
            return;
        }
        if (UButton* Btn = Cast<UButton>(Widget))
        {
            // Button text comes from a child TextBlock (UMG idiom). The spec
            // captures it via the child node's Content.Text -- nothing to do
            // at the button level itself.
            (void)Btn;
            return;
        }
        if (UEditableText* E = Cast<UEditableText>(Widget))
        {
            OutContent.Text        = E->GetText().ToString();
            OutContent.Placeholder = E->GetHintText().ToString();
            return;
        }
        if (UEditableTextBox* EB = Cast<UEditableTextBox>(Widget))
        {
            OutContent.Text        = EB->GetText().ToString();
            OutContent.Placeholder = EB->GetHintText().ToString();
            return;
        }
        if (UCheckBox* CB = Cast<UCheckBox>(Widget))
        {
            // Checkbox state is captured via Style (Background / FontColor are
            // slot-level). v1 does not surface bIsChecked into Content; it's
            // an interaction state, not authored config.
            (void)CB;
            return;
        }
    }

    // -------------------------------------------------------------------------
    // STYLE SERIALIZATION
    // -------------------------------------------------------------------------

    static void SerializeStyle(UWidget* Widget, FUISpecStyle& OutStyle)
    {
        if (!Widget) return;

        // Widget-level fields visible on every UWidget.
        OutStyle.Opacity    = Widget->GetRenderOpacity();
        OutStyle.Visibility = VisibilityToToken(Widget->GetVisibility());

        // SizeBox-style width / height (when present). We call the public
        // getters unconditionally; an unset override returns 0 (or the engine
        // default), which we then mark via bUseCustomSize when non-zero.
        if (USizeBox* SB = Cast<USizeBox>(Widget))
        {
            const float W = SB->GetWidthOverride();
            const float H = SB->GetHeightOverride();
            OutStyle.Width  = W;
            OutStyle.Height = H;
            OutStyle.bUseCustomSize = (W > 0.f) || (H > 0.f);
        }

        // Border background / brush colour. GetBrushColor() returns the
        // FLinearColor tint applied to the brush (the engine getter; preferred
        // over the deprecated direct `Background` UPROPERTY access).
        if (UBorder* Br = Cast<UBorder>(Widget))
        {
            OutStyle.Background = Br->GetBrushColor();
            OutStyle.Padding    = Br->GetPadding();
        }

        // ProgressBar / Slider expose a fill colour we treat as Background.
        if (UProgressBar* PB = Cast<UProgressBar>(Widget))
        {
            OutStyle.Background = PB->GetFillColorAndOpacity();
        }
    }

    // -------------------------------------------------------------------------
    // EFFECT SERIALIZATION (UEffectSurface — reflective, post-R3)
    // -------------------------------------------------------------------------
    // History:
    //   * Pre-R3: typed `Cast<UEffectSurface>` + direct `Config.*` field reads
    //     pulled UEffectSurface / FEffectSurfaceConfig symbols into MonolithUI,
    //     forcing the optional provider to be a hard compile-time dep.
    //   * R3 (this file): the cast becomes a UClass* probe via
    //     `MonolithUI::GetEffectSurfaceClass()`; scalar reads route through
    //     `FUIReflectionHelper::ReadJsonPath` against the curated `Effect.*`
    //     JSON paths (the same mapping the Phase H builder writes through, so
    //     dump → build → dump round-trips byte-identically); shadow arrays are
    //     walked manually via FArrayProperty / FStructProperty descent because
    //     `ReadJsonPath` deliberately does not navigate TArray container
    //     properties (the Phase J planner's documented limit).
    //
    // §5.5.4 contract — silent no-op on absent class:
    //   When `MonolithUI::GetEffectSurfaceClass()` returns nullptr (provider
    //   physically absent from the build), this function MUST set
    //   `bOutHasEffect = false` and early-return. NO entry added to
    //   Context.Errors / Context.Warnings. NO `skipped: true` metadata. The
    //   absence of the `effect` field in the resulting JSON IS the signal —
    //   downstream tools key on field presence, not on a sentinel marker.
    //   Same contract when `Widget` is non-null but is not an EffectSurface
    //   instance (the common case: most widgets are not EffectSurfaces).
    //
    // Single source of truth: the probe is `MonolithUI::IsEffectSurfaceAvailable()`
    // (R3b.4) and the class lookup is `MonolithUI::GetEffectSurfaceClass()`
    // (R3b.4) — both share one cache invalidated on Live Coding reloads.

    /**
     * Read one shadow-struct element by FProperty descent. Mirrors the
     * pre-R3 typed assignment from FEffectShadow → FUISpecEffectShadow
     * field-for-field. Returns a default-constructed entry on any reflective
     * miss so the output array length matches the engine-side array length
     * (round-trip invariant — `dump → build → dump` must produce identical
     * shadow stacks).
     */
    static FUISpecEffectShadow ReadShadowReflective(
        const UScriptStruct* ShadowStruct,
        const void* ShadowAddr,
        bool bInset)
    {
        FUISpecEffectShadow Out;
        Out.bInset = bInset;
        if (!ShadowStruct || !ShadowAddr) return Out;

        if (const FStructProperty* OffsetProp = CastField<FStructProperty>(
                ShadowStruct->FindPropertyByName(TEXT("Offset"))))
        {
            Out.Offset = *(const FVector2D*)OffsetProp->ContainerPtrToValuePtr<void>(ShadowAddr);
        }
        if (const FFloatProperty* BlurProp = CastField<FFloatProperty>(
                ShadowStruct->FindPropertyByName(TEXT("Blur"))))
        {
            Out.Blur = BlurProp->GetPropertyValue_InContainer(ShadowAddr);
        }
        if (const FFloatProperty* SpreadProp = CastField<FFloatProperty>(
                ShadowStruct->FindPropertyByName(TEXT("Spread"))))
        {
            Out.Spread = SpreadProp->GetPropertyValue_InContainer(ShadowAddr);
        }
        if (const FStructProperty* ColorProp = CastField<FStructProperty>(
                ShadowStruct->FindPropertyByName(TEXT("Color"))))
        {
            Out.Color = *(const FLinearColor*)ColorProp->ContainerPtrToValuePtr<void>(ShadowAddr);
        }
        return Out;
    }

    /**
     * Walk `Config.<ArrayFieldName>` (a TArray<FEffectShadow>) reflectively
     * and append each element into `OutShadows` via ReadShadowReflective.
     *
     * Returns silently on any structural mismatch — schema drift inside
     * the optional provider (e.g. FEffectShadow gaining a new field) leaves the
     * shadow stack empty rather than crashing the dump call. Round-trip
     * tests will catch any drift via the v0 → v1 build delta.
     */
    static void ReadShadowArrayReflective(
        UObject* Widget,
        const FStructProperty* ConfigProp,
        const TCHAR* ArrayFieldName,
        bool bInset,
        TArray<FUISpecEffectShadow>& OutShadows)
    {
        if (!Widget || !ConfigProp || !ConfigProp->Struct) return;

        const FArrayProperty* ArrProp = CastField<FArrayProperty>(
            ConfigProp->Struct->FindPropertyByName(FName(ArrayFieldName)));
        if (!ArrProp) return;

        const FStructProperty* InnerStructProp = CastField<FStructProperty>(ArrProp->Inner);
        if (!InnerStructProp || !InnerStructProp->Struct) return;

        // Address of the Config sub-bag inside the widget instance.
        const void* ConfigAddr = ConfigProp->ContainerPtrToValuePtr<void>(Widget);
        // Address of the TArray header inside the Config sub-bag.
        const void* ArrayAddr = ArrProp->ContainerPtrToValuePtr<void>(ConfigAddr);

        FScriptArrayHelper ArrHelper(ArrProp, ArrayAddr);
        const int32 NumElems = ArrHelper.Num();
        OutShadows.Reserve(OutShadows.Num() + NumElems);
        for (int32 i = 0; i < NumElems; ++i)
        {
            const void* ElemAddr = ArrHelper.GetRawPtr(i);
            OutShadows.Add(ReadShadowReflective(InnerStructProp->Struct, ElemAddr, bInset));
        }
    }

    /**
     * Convenience: read a curated `Effect.*` JSON path through the helper and
     * silently default OutValue if the read fails (e.g. registry not loaded
     * in a release build). Pure helper — never throws.
     */
    static bool ReadEffectScalar(
        FUIReflectionHelper& Helper,
        const UWidget* Widget,
        const FName& WidgetToken,
        const TCHAR* JsonPath,
        TSharedPtr<FJsonValue>& OutValue)
    {
        const FUIReflectionApplyResult R = Helper.ReadJsonPath(
            Widget, WidgetToken, JsonPath, OutValue);
        return R.bSuccess && OutValue.IsValid();
    }

    static void SerializeEffect(UWidget* Widget, FUISpecEffect& OutEffect, bool& bOutHasEffect)
    {
        // §5.5.4 contract — see comment block above.
        bOutHasEffect = false;

        UClass* EffectSurfaceClass = MonolithUI::GetEffectSurfaceClass();
        if (!EffectSurfaceClass || !Widget || !Widget->IsA(EffectSurfaceClass))
        {
            // Silent no-op: the absence of the `effect` field in the emitted
            // JSON IS the §5.5.4 signal. No errors, no warnings, no metadata.
            return;
        }

        // Cheap one-call boolean preamble via the R3b UFUNCTION accessor —
        // lets the serializer short-circuit on EffectSurfaces with zero
        // configured effects (FeatureFlags == 0 + empty sub-bags) without
        // walking the full FEffectSurfaceConfig payload reflectively.
        if (UFunction* HasEffectFn = Widget->FindFunction(FName(TEXT("HasAnyEffectConfigured"))))
        {
            bool bHasAny = false;
            Widget->ProcessEvent(HasEffectFn, &bHasAny);
            if (!bHasAny) return;
        }
        // If HasAnyEffectConfigured is missing (provider present but
        // older than R3b), fall through and emit whatever the field walk
        // produces — defaults read as empty/zero, round-trip stays clean.

        bOutHasEffect = true;

        // ---- Scalar reads via the curated `Effect.*` JSON path mapping. ----
        UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get();
        FUIPropertyPathCache* PathCache = Sub ? Sub->GetPathCache() : nullptr;
        const FUIPropertyAllowlist* Allowlist = Sub ? &Sub->GetAllowlist() : nullptr;
        FUIReflectionHelper Helper(PathCache, Allowlist);

        const FName WidgetToken = MonolithUI::MakeTokenFromClassName(EffectSurfaceClass);
        TSharedPtr<FJsonValue> Tmp;

        // Effect.Shape.CornerRadii → FVector4 (JSON object {x,y,z,w}).
        if (ReadEffectScalar(Helper, Widget, WidgetToken, TEXT("Effect.Shape.CornerRadii"), Tmp)
            && Tmp->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Obj = Tmp->AsObject();
            if (Obj.IsValid())
            {
                OutEffect.CornerRadii = FVector4(
                    Obj->GetNumberField(TEXT("x")),
                    Obj->GetNumberField(TEXT("y")),
                    Obj->GetNumberField(TEXT("z")),
                    Obj->GetNumberField(TEXT("w")));
            }
        }

        // Effect.Shape.Smoothness → float.
        if (ReadEffectScalar(Helper, Widget, WidgetToken, TEXT("Effect.Shape.Smoothness"), Tmp)
            && Tmp->Type == EJson::Number)
        {
            OutEffect.Smoothness = (float)Tmp->AsNumber();
        }

        // Effect.Fill.SolidColor → FLinearColor (JSON object {r,g,b,a}).
        if (ReadEffectScalar(Helper, Widget, WidgetToken, TEXT("Effect.Fill.SolidColor"), Tmp)
            && Tmp->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Obj = Tmp->AsObject();
            if (Obj.IsValid())
            {
                OutEffect.SolidColor = FLinearColor(
                    Obj->GetNumberField(TEXT("r")),
                    Obj->GetNumberField(TEXT("g")),
                    Obj->GetNumberField(TEXT("b")),
                    Obj->GetNumberField(TEXT("a")));
            }
        }

        // Effect.BackdropBlur.Strength → float.
        if (ReadEffectScalar(Helper, Widget, WidgetToken, TEXT("Effect.BackdropBlur.Strength"), Tmp)
            && Tmp->Type == EJson::Number)
        {
            OutEffect.BackdropBlurStrength = (float)Tmp->AsNumber();
        }

        // ---- Shadow arrays via direct FProperty descent. -------------------
        // ReadJsonPath does not navigate TArray container properties (Phase J
        // planner's documented limit, mirrored on the write side where the
        // EffectSurfaceBuilder also bypasses the helper for shadow arrays).
        OutEffect.DropShadows.Reset();
        OutEffect.InnerShadows.Reset();

        if (const FStructProperty* ConfigProp = CastField<FStructProperty>(
                EffectSurfaceClass->FindPropertyByName(TEXT("Config"))))
        {
            ReadShadowArrayReflective(Widget, ConfigProp, TEXT("DropShadow"),
                /*bInset=*/false, OutEffect.DropShadows);
            ReadShadowArrayReflective(Widget, ConfigProp, TEXT("InnerShadow"),
                /*bInset=*/true,  OutEffect.InnerShadows);
        }

        // Stops carry through the SolidColor field for v1 — the FUISpecEffect
        // schema doesn't yet model multi-stop gradients (Stops live on the
        // FEffectFill side; surfaced via the styles map in a follow-up).
    }

    // -------------------------------------------------------------------------
    // COMMONUI SERIALIZATION
    // -------------------------------------------------------------------------

    static void SerializeCommonUI(UWidget* Widget, FUISpecCommonUI& OutCUI, bool& bOutHasCommonUI)
    {
        bOutHasCommonUI = false;

#if WITH_COMMONUI
        // Detect any CommonUI-derived widget. We capture style class refs as
        // path strings (resolved on rebuild via the style service).
        if (UCommonButtonBase* Btn = Cast<UCommonButtonBase>(Widget))
        {
            bOutHasCommonUI = true;
            // GetStyleCDO returns the per-class CDO; capture the class path
            // so the rebuild side can re-load the same TSubclassOf<UCommonButtonStyle>.
            if (const UCommonButtonStyle* StyleCDO = Btn->GetStyleCDO())
            {
                if (UClass* StyleClass = StyleCDO->GetClass())
                {
                    OutCUI.StyleRefs.Add(FName(*StyleClass->GetPathName()));
                }
            }
            return;
        }
        if (UCommonActivatableWidget* Act = Cast<UCommonActivatableWidget>(Widget))
        {
            bOutHasCommonUI = true;
            // Input mode token -- map the CommonUI input-mode enum if any.
            // v1 leaves InputMode default; Phase K will add the explicit
            // enum-to-token mapping when the per-action wire surfaces it.
            (void)Act;
            return;
        }
        if (UCommonUserWidget* CU = Cast<UCommonUserWidget>(Widget))
        {
            bOutHasCommonUI = true;
            (void)CU;
            return;
        }
#else
        (void)Widget;
        (void)OutCUI;
#endif
    }

    // -------------------------------------------------------------------------
    // ANIMATION SERIALIZATION
    // -------------------------------------------------------------------------

    /**
     * Convert UWidgetAnimation entries on the WBP into FUISpecAnimation entries.
     *
     * v1 captures: animation name (display label), duration, target widget id
     * (from the first binding), and a synthetic two-keyframe pair representing
     * the from/to envelope. Rich curve tangent capture is deferred -- see the
     * lossy boundary catalogue.
     */
    static void SerializeAnimations(UWidgetBlueprint* WBP, FUISpecDocument& OutDoc, int32& OutCount)
    {
        OutCount = 0;
        if (!WBP) return;

#if WITH_EDITORONLY_DATA
        for (UWidgetAnimation* Anim : WBP->Animations)
        {
            if (!Anim) continue;

            FUISpecAnimation Out;
            Out.Name = FName(*Anim->GetDisplayLabel());

            // Pull duration from MovieScene's playback range when present.
            if (Anim->MovieScene)
            {
                const TRange<FFrameNumber> Range = Anim->MovieScene->GetPlaybackRange();
                const FFrameRate           Rate  = Anim->MovieScene->GetTickResolution();
                if (!Range.IsEmpty() && Rate.AsDecimal() > 0.0)
                {
                    const FFrameNumber StartFN = Range.GetLowerBoundValue();
                    const FFrameNumber EndFN   = Range.GetUpperBoundValue();
                    const double Start = Rate.AsSeconds(FFrameTime(StartFN));
                    const double End   = Rate.AsSeconds(FFrameTime(EndFN));
                    Out.Duration = (float)FMath::Max(0.0, End - Start);
                }
            }

            // Bindings -- target widget id is the first binding's WidgetName
            // (most authored animations target a single widget; multi-binding
            // animations are flagged via warnings in the rebuild path).
            const TArray<FWidgetAnimationBinding>& Bindings = Anim->GetBindings();
            if (Bindings.Num() > 0)
            {
                Out.TargetWidgetId = Bindings[0].WidgetName;
            }

            // Synthesise a two-keyframe envelope so the rebuild path produces
            // a compile-clean animation. Real curve-data capture awaits a
            // dedicated MovieScene reader (out of scope for v1 -- documented
            // in the lossy boundary catalogue).
            FUISpecKeyframe K0;
            K0.Time = 0.f;
            K0.Easing = FName(TEXT("Linear"));
            FUISpecKeyframe K1;
            K1.Time = Out.Duration;
            K1.Easing = FName(TEXT("Linear"));
            Out.Keyframes.Add(K0);
            Out.Keyframes.Add(K1);

            OutDoc.Animations.Add(MoveTemp(Out));
            ++OutCount;
        }
#endif // WITH_EDITORONLY_DATA
    }

    // -------------------------------------------------------------------------
    // RECURSIVE WIDGET WALK
    // -------------------------------------------------------------------------

    /**
     * Build an FUISpecNode for the given widget and recurse into its children
     * if it's a UPanelWidget. Pure read; no mutation.
     */
    static TSharedPtr<FUISpecNode> SerializeWidgetRecurse(UWidget* Widget, int32& OutVisited)
    {
        if (!Widget) return nullptr;

        TSharedPtr<FUISpecNode> Node = MakeShared<FUISpecNode>();
        Node->Type = ClassToToken(Widget->GetClass());
        Node->Id   = Widget->GetFName();

        // Slot.
        if (UPanelSlot* Slot = Widget->Slot)
        {
            SerializeSlot(Slot, Node->Slot);
        }

        // Style + Content sub-bags.
        SerializeStyle(Widget, Node->Style);
        SerializeContent(Widget, Node->Content);

        // Effect surface (UEffectSurface only).
        SerializeEffect(Widget, Node->Effect, Node->bHasEffect);

        // CommonUI sub-bag (gated).
        SerializeCommonUI(Widget, Node->CommonUI, Node->bHasCommonUI);

        // Custom class path -- when the registry doesn't recognise the type
        // we still capture the full UClass path so rebuild can resolve it.
        if (Node->Type.IsNone())
        {
            Node->CustomClassPath = Widget->GetClass()->GetPathName();
        }

        ++OutVisited;

        // Children -- only when this is a UPanelWidget (multi or single child).
        if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
        {
            const int32 N = Panel->GetChildrenCount();
            for (int32 i = 0; i < N; ++i)
            {
                if (UWidget* Child = Panel->GetChildAt(i))
                {
                    if (TSharedPtr<FUISpecNode> ChildNode = SerializeWidgetRecurse(Child, OutVisited))
                    {
                        Node->Children.Add(ChildNode);
                    }
                }
            }
        }

        return Node;
    }
} // namespace MonolithUI::SpecSerializerInternal


// ============================================================================
// FUISpecSerializer::Dump
// ============================================================================

FUISpecSerializerResult FUISpecSerializer::Dump(const FUISpecSerializerInputs& Inputs)
{
    FUISpecSerializerResult Result;
    Result.AssetPath = Inputs.AssetPath;
    Result.RequestId = Inputs.RequestId;

    if (Inputs.AssetPath.IsEmpty())
    {
        FUISpecError E;
        E.Severity = EUISpecErrorSeverity::Error;
        E.Category = TEXT("Asset");
        E.Message  = TEXT("FUISpecSerializer::Dump called with empty AssetPath.");
        Result.Errors.Add(MoveTemp(E));
        return Result;
    }

    // Resolve asset_path (with the same `/Game/...` prepending logic as the
    // legacy LoadWidgetBlueprint helper).
    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *Inputs.AssetPath);
    if (!WBP)
    {
        // Try with `/Game/` prefix when the caller supplied a relative path.
        FString Prefixed = Inputs.AssetPath;
        if (!Prefixed.StartsWith(TEXT("/")))
        {
            Prefixed = TEXT("/Game/") + Prefixed;
            WBP = LoadObject<UWidgetBlueprint>(nullptr, *Prefixed);
        }
    }
    if (!WBP)
    {
        FUISpecError E;
        E.Severity = EUISpecErrorSeverity::Error;
        E.Category = TEXT("Asset");
        E.Message  = FString::Printf(TEXT("Could not load UWidgetBlueprint at '%s'."), *Inputs.AssetPath);
        Result.Errors.Add(MoveTemp(E));
        return Result;
    }

    return DumpFromWBP(WBP, Inputs.AssetPath, Inputs.RequestId);
}

FUISpecSerializerResult FUISpecSerializer::DumpFromWBP(
    UWidgetBlueprint* WBP,
    const FString& AssetPath,
    const FString& RequestId)
{
    using namespace MonolithUI::SpecSerializerInternal;

    FUISpecSerializerResult Result;
    Result.AssetPath = AssetPath;
    Result.RequestId = RequestId;

    if (!WBP)
    {
        FUISpecError E;
        E.Severity = EUISpecErrorSeverity::Error;
        E.Category = TEXT("Internal");
        E.Message  = TEXT("FUISpecSerializer::DumpFromWBP called with null WBP.");
        Result.Errors.Add(MoveTemp(E));
        return Result;
    }
    if (!WBP->WidgetTree)
    {
        FUISpecError E;
        E.Severity = EUISpecErrorSeverity::Error;
        E.Category = TEXT("Asset");
        E.Message  = FString::Printf(TEXT("WBP '%s' has no WidgetTree."), *WBP->GetName());
        Result.Errors.Add(MoveTemp(E));
        return Result;
    }

    FUISpecDocument& Doc = Result.Document;

    // Document-level metadata.
    Doc.Version     = 1;
    Doc.Name        = WBP->GetName();
    if (WBP->ParentClass)
    {
        // Token-form parent-class for known native parents; full path otherwise.
        const FString ParentName = WBP->ParentClass->GetName();
        if (ParentName == TEXT("UserWidget"))
        {
            Doc.ParentClass = TEXT("UserWidget");
        }
#if WITH_COMMONUI
        else if (ParentName == TEXT("CommonUserWidget"))
        {
            Doc.ParentClass = TEXT("CommonUserWidget");
        }
        else if (ParentName == TEXT("CommonActivatableWidget"))
        {
            Doc.ParentClass = TEXT("CommonActivatableWidget");
        }
#endif
        else
        {
            Doc.ParentClass = WBP->ParentClass->GetPathName();
        }
    }
    else
    {
        Doc.ParentClass = TEXT("UserWidget");
    }

    Doc.Metadata.AuthoringTool = TEXT("MonolithUI.SpecSerializer");
    Doc.Metadata.SourceFile    = AssetPath;

    // Widget tree walk.
    if (UWidget* RootWidget = WBP->WidgetTree->RootWidget)
    {
        Doc.Root = SerializeWidgetRecurse(RootWidget, Result.NodesVisited);
    }
    else
    {
        // Empty WBP -- still emit an empty document with no root. The rebuild
        // side rejects this via the validator's "missing rootWidget" error.
        FUISpecError W;
        W.Severity = EUISpecErrorSeverity::Warning;
        W.Category = TEXT("Structure");
        W.Message  = FString::Printf(TEXT("WBP '%s' has no RootWidget -- empty document."), *WBP->GetName());
        Result.Warnings.Add(MoveTemp(W));
    }

    // Animations.
    SerializeAnimations(WBP, Doc, Result.AnimationsCaptured);

    // Document-scoped Styles map -- v1 leaves empty (no UMG counterpart for
    // free-standing named-style entries; Phase H's pre-create-styles still
    // works because the style hook iterates document.Styles defensively).

    Result.bSuccess = (Doc.Root.IsValid());
    return Result;
}
