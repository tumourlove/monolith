// Copyright tumourlove. All Rights Reserved.
// MonolithUIRegistrySubsystem.cpp

#include "Registry/MonolithUIRegistrySubsystem.h"

#include "MonolithUICommon.h"
#include "Components/ContentWidget.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Editor.h"
#include "Misc/CoreDelegates.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

namespace
{
    /**
     * `UPanelWidget::GetSlotClass` (and its `UContentWidget` override) is
     * declared `protected` in the engine — there is no public accessor. The
     * standard C++ idiom for legitimately reading a protected member from
     * outside the hierarchy is a derived helper that re-exposes the symbol
     * via `using`-declaration. The helper adds no state and the cast is
     * static (upcast through derivation), so this is well-defined.
     */
    struct FPanelSlotClassAccessor : public UPanelWidget
    {
        using UPanelWidget::GetSlotClass;
    };

    UClass* GetPanelSlotClass(UPanelWidget* PanelCDO)
    {
        if (!PanelCDO)
        {
            return nullptr;
        }
        return static_cast<FPanelSlotClassAccessor*>(PanelCDO)->GetSlotClass();
    }
}

void UMonolithUIRegistrySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    // Allowlist holds a const-ref to TypeRegistry — construct once.
    Allowlist = MakeUnique<FUIPropertyAllowlist>(TypeRegistry);

    // Phase C: property path cache. Default cap (256) matches the
    // UMonolithUISettings.PathCacheCap default scheduled for Phase G.
    PathCache = MakeUnique<FUIPropertyPathCache>();

    PopulateFromReflectionWalk();
    RegisterCuratedMappings();
    Allowlist->Invalidate();

    // Bind reload-complete so hot-reload cycles refresh the registry.
    ReloadCompleteHandle = FCoreUObjectDelegates::ReloadCompleteDelegate.AddUObject(
        this, &UMonolithUIRegistrySubsystem::OnReloadComplete);

    UE_LOG(LogMonolithUISpec, Log,
        TEXT("UMonolithUIRegistrySubsystem initialised — %d widget types registered"),
        TypeRegistry.Num());
}

void UMonolithUIRegistrySubsystem::Deinitialize()
{
    if (ReloadCompleteHandle.IsValid())
    {
        FCoreUObjectDelegates::ReloadCompleteDelegate.Remove(ReloadCompleteHandle);
        ReloadCompleteHandle.Reset();
    }

    Allowlist.Reset();
    PathCache.Reset();
    TypeRegistry.Reset();

    Super::Deinitialize();
}

UMonolithUIRegistrySubsystem* UMonolithUIRegistrySubsystem::Get()
{
    if (GEditor)
    {
        return GEditor->GetEditorSubsystem<UMonolithUIRegistrySubsystem>();
    }
    return nullptr;
}

void UMonolithUIRegistrySubsystem::RescanWidgetTypes()
{
    PopulateFromReflectionWalk();
    RegisterCuratedMappings();
    if (Allowlist.IsValid())
    {
        Allowlist->Invalidate();
    }
    if (PathCache.IsValid())
    {
        PathCache->Reset();
    }

    UE_LOG(LogMonolithUISpec, Log,
        TEXT("UMonolithUIRegistrySubsystem rescan complete — %d widget types registered"),
        TypeRegistry.Num());
}

int32 UMonolithUIRegistrySubsystem::RefreshAfterReload()
{
    const int32 Refreshed = TypeRegistry.RefreshStaleEntries();
    if (Allowlist.IsValid())
    {
        Allowlist->Invalidate();
    }
    // Phase C: drop every cached property chain. Per-hit re-validation in
    // FUIPropertyPathCache::Get also handles individual stale entries, but
    // wholesale Reset here matches the allowlist Invalidate rhythm and avoids
    // the (cheap-but-unnecessary) re-validate-then-evict ping every entry
    // would pay on next access.
    if (PathCache.IsValid())
    {
        PathCache->Reset();
    }

    UE_LOG(LogMonolithUISpec, Log,
        TEXT("UMonolithUIRegistrySubsystem reload refresh — %d entries refreshed"),
        Refreshed);

    return Refreshed;
}

void UMonolithUIRegistrySubsystem::PopulateFromReflectionWalk()
{
    TypeRegistry.Reset();

    UClass* WidgetBaseClass = UWidget::StaticClass();
    if (!WidgetBaseClass)
    {
        UE_LOG(LogMonolithUISpec, Error,
            TEXT("UWidget::StaticClass() returned null — cannot populate registry"));
        return;
    }

    int32 ConsideredCount = 0;
    int32 SkippedAbstract = 0;
    int32 SkippedDeprecated = 0;

    for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
    {
        UClass* Class = *ClassIt;
        if (!Class)
        {
            continue;
        }

        // Filter to UWidget descendants. `IsChildOf(UWidget::StaticClass())`
        // is the explicit form (cleaner than the template overload here).
        if (!Class->IsChildOf(WidgetBaseClass))
        {
            continue;
        }

        ++ConsideredCount;

        // Skip the UWidget base itself — it has no useful token.
        if (Class == WidgetBaseClass)
        {
            ++SkippedAbstract;
            continue;
        }

        // Skip abstract / deprecated / hot-reload-orphan classes. The
        // CLASS_NewerVersionExists flag flips on the OLD UClass after a hot
        // reload — refusing it here means stale UClasses stay out of the
        // registry until the OnReloadComplete refresh re-points them.
        if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
        {
            if (Class->HasAnyClassFlags(CLASS_Abstract))
            {
                ++SkippedAbstract;
            }
            else
            {
                ++SkippedDeprecated;
            }
            continue;
        }

        // Skip Blueprint-generated widget classes — the registry catalogues
        // native widget types only. Blueprint subclasses of UUserWidget show
        // up via TObjectIterator once they're loaded; including them would
        // drown the registry in WBP_* entries from the project's Content/.
        if (Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
        {
            continue;
        }

        FUITypeRegistryEntry Entry;
        Entry.Token = MonolithUI::MakeTokenFromClassName(Class);
        Entry.WidgetClass = Class;

        UClass* SlotClass = nullptr;
        ClassifyWidgetClass(Class, Entry.ContainerKind, Entry.MaxChildren, SlotClass);
        Entry.SlotClass = SlotClass;

        TypeRegistry.RegisterType(MoveTemp(Entry));
    }

    UE_LOG(LogMonolithUISpec, Verbose,
        TEXT("UMonolithUIRegistrySubsystem reflection walk: considered=%d, skipped(abstract)=%d, skipped(deprecated)=%d, registered=%d"),
        ConsideredCount, SkippedAbstract, SkippedDeprecated, TypeRegistry.Num());
}

void UMonolithUIRegistrySubsystem::ClassifyWidgetClass(
    UClass* WidgetClass,
    EUIContainerKind& OutKind,
    int32& OutMaxChildren,
    UClass*& OutSlotClass)
{
    OutKind = EUIContainerKind::Leaf;
    OutMaxChildren = 0;
    OutSlotClass = nullptr;

    if (!WidgetClass)
    {
        return;
    }

    // Content widget = single-child wrapper (UBorder, USizeBox, USafeZone, etc.).
    // Order matters: UContentWidget descends from UPanelWidget, so check Content
    // FIRST and only fall through to the multi-child Panel branch if not Content.
    if (WidgetClass->IsChildOf(UContentWidget::StaticClass()))
    {
        OutKind = EUIContainerKind::Content;
        OutMaxChildren = 1;

        // GetSlotClass is virtual + protected — read via the file-local accessor.
        if (UContentWidget* CDO = Cast<UContentWidget>(WidgetClass->GetDefaultObject()))
        {
            OutSlotClass = GetPanelSlotClass(CDO);
        }
        return;
    }

    if (WidgetClass->IsChildOf(UPanelWidget::StaticClass()))
    {
        OutKind = EUIContainerKind::Panel;

        // CDO drives both `CanHaveMultipleChildren()` and `GetSlotClass()`.
        // CanHaveMultipleChildren is set in each panel's UCLASS_BODY ctor —
        // false for single-child panels (e.g. UPanelWidget descendants that
        // override the bool), true for the common case.
        if (UPanelWidget* CDO = Cast<UPanelWidget>(WidgetClass->GetDefaultObject()))
        {
            OutMaxChildren = CDO->CanHaveMultipleChildren() ? -1 : 1;
            OutSlotClass = GetPanelSlotClass(CDO);
        }
        else
        {
            // Defensive default: treat as unbounded panel.
            OutMaxChildren = -1;
        }
        return;
    }

    // Leaf widget — already initialised to (Leaf, 0, null).
}

void UMonolithUIRegistrySubsystem::OnReloadComplete(EReloadCompleteReason /*Reason*/)
{
    RefreshAfterReload();
}

// -----------------------------------------------------------------------------
// Curated property mappings
// -----------------------------------------------------------------------------
//
// One-by-one mappings for the widgets the spec/builder pipeline writes most
// often. Keeping the list explicit (rather than reflectively walking every
// UPROPERTY) is deliberate — the allowlist is a SECURITY surface; auto-walking
// would surface engine-internal transient flags that should never be writable
// from a JSON spec.
//
// For each widget, mappings cover (a) the curated visible properties and
// (b) the slot-side `Slot.*` paths that the parent's slot class supports.
// Unknown widgets get an empty mapping list — they're still registered so
// the type registry knows they exist, but the allowlist refuses every write
// until a curated entry lands. This is the safe default.

namespace
{
    // Convenience shorthand to drop a property mapping onto an existing
    // registry entry. No-op if the entry is missing (widget UClass not loaded
    // in this build — common in stripped release builds).
    void AddMappingTo(FUITypeRegistry& Registry, const FName& Token, const FString& JsonPath,
        const FString& EnginePath, const FString& Description = FString())
    {
        const FUITypeRegistryEntry* Existing = Registry.FindByToken(Token);
        if (!Existing)
        {
            return;
        }

        // FindByToken returns const* — we need to mutate. Rebuild the entry
        // through a non-const lookup via a local copy + re-register. Cheap;
        // mappings are seeded once at init.
        FUITypeRegistryEntry Updated = *Existing;
        Updated.PropertyMappings.Emplace(JsonPath, EnginePath, Description);
        Registry.RegisterType(MoveTemp(Updated));
    }

    // Slot-shape helpers — the same Slot.* mapping applies across every
    // widget that lives inside a given parent slot class. Each helper attaches
    // the appropriate Slot.* JSON paths to a child widget's mapping list.

    void AddCanvasSlotMappings(FUITypeRegistry& Registry, const FName& Token)
    {
        AddMappingTo(Registry, Token, TEXT("Slot.Anchors"),    TEXT("Slot.Anchors"),    TEXT("Anchor preset on a Canvas Panel slot."));
        AddMappingTo(Registry, Token, TEXT("Slot.Position"),   TEXT("Slot.LayoutData.Offsets"), TEXT("Position offset within the canvas slot."));
        AddMappingTo(Registry, Token, TEXT("Slot.Size"),       TEXT("Slot.LayoutData.Offsets"), TEXT("Size override within the canvas slot."));
        AddMappingTo(Registry, Token, TEXT("Slot.Alignment"),  TEXT("Slot.LayoutData.Anchors"), TEXT("Pivot alignment within the canvas slot."));
        AddMappingTo(Registry, Token, TEXT("Slot.AutoSize"),   TEXT("Slot.bAutoSize"),  TEXT("Auto-size to child within the canvas slot."));
        AddMappingTo(Registry, Token, TEXT("Slot.ZOrder"),     TEXT("Slot.ZOrder"),     TEXT("Z-order within the canvas."));
    }

    void AddBoxSlotMappings(FUITypeRegistry& Registry, const FName& Token)
    {
        AddMappingTo(Registry, Token, TEXT("Slot.Padding"),    TEXT("Slot.Padding"),    TEXT("Padding margin within the box slot."));
        AddMappingTo(Registry, Token, TEXT("Slot.HAlign"),     TEXT("Slot.HorizontalAlignment"), TEXT("Horizontal alignment within the box slot."));
        AddMappingTo(Registry, Token, TEXT("Slot.VAlign"),     TEXT("Slot.VerticalAlignment"),   TEXT("Vertical alignment within the box slot."));
        AddMappingTo(Registry, Token, TEXT("Slot.SizeRule"),   TEXT("Slot.Size.SizeRule"), TEXT("Automatic/Fill size rule within the box slot."));
        AddMappingTo(Registry, Token, TEXT("Slot.FillWeight"), TEXT("Slot.Size.Value"),    TEXT("Fill weight within the box slot."));
    }
}

void UMonolithUIRegistrySubsystem::RegisterCuratedMappings()
{
    // ----- Panels -----
    // VerticalBox / HorizontalBox have no own visual properties; only Slot.*
    // mappings on their children matter. We still attach an empty curated
    // mapping list so the diagnostic knows the type is "supported".
    {
        // Touch the entries so allowlist diag returns an empty array (not "unknown").
        // A future curated property (e.g. layout direction) would land here.
    }

    // CanvasPanel — leaf panel; the children carry Anchors/Position/Size on
    // the slot. No own visual properties to expose.

    // Overlay — Slot.HAlign / Slot.VAlign / Slot.Padding live on the child slot
    // and are added via AddBoxSlotMappings on each typed child widget below.

    // ----- Content widgets -----

    // Border — colour tint + brush + padding.
    AddMappingTo(TypeRegistry, FName(TEXT("Border")), TEXT("Background"),     TEXT("Background"),     TEXT("Background brush of the border."));
    AddMappingTo(TypeRegistry, FName(TEXT("Border")), TEXT("BrushColor"),     TEXT("BrushColor"),     TEXT("Tint applied to the background brush."));
    AddMappingTo(TypeRegistry, FName(TEXT("Border")), TEXT("Padding"),        TEXT("Padding"),        TEXT("Inner padding of the border."));
    AddMappingTo(TypeRegistry, FName(TEXT("Border")), TEXT("ContentColorAndOpacity"), TEXT("ContentColorAndOpacity"), TEXT("Tint applied to the border's child."));

    // SizeBox — explicit size constraints.
    AddMappingTo(TypeRegistry, FName(TEXT("SizeBox")), TEXT("WidthOverride"),  TEXT("WidthOverride"),  TEXT("Forced width override."));
    AddMappingTo(TypeRegistry, FName(TEXT("SizeBox")), TEXT("HeightOverride"), TEXT("HeightOverride"), TEXT("Forced height override."));
    AddMappingTo(TypeRegistry, FName(TEXT("SizeBox")), TEXT("MinDesiredWidth"),  TEXT("MinDesiredWidth"),  TEXT("Minimum desired width."));
    AddMappingTo(TypeRegistry, FName(TEXT("SizeBox")), TEXT("MinDesiredHeight"), TEXT("MinDesiredHeight"), TEXT("Minimum desired height."));
    AddMappingTo(TypeRegistry, FName(TEXT("SizeBox")), TEXT("MaxDesiredWidth"),  TEXT("MaxDesiredWidth"),  TEXT("Maximum desired width."));
    AddMappingTo(TypeRegistry, FName(TEXT("SizeBox")), TEXT("MaxDesiredHeight"), TEXT("MaxDesiredHeight"), TEXT("Maximum desired height."));

    // ----- Leaves -----

    // TextBlock — most-common spec leaf.
    AddMappingTo(TypeRegistry, FName(TEXT("TextBlock")), TEXT("Text"),             TEXT("Text"),             TEXT("Display text."));
    AddMappingTo(TypeRegistry, FName(TEXT("TextBlock")), TEXT("Font"),             TEXT("Font"),             TEXT("Font info (family, size, type face)."));
    AddMappingTo(TypeRegistry, FName(TEXT("TextBlock")), TEXT("ColorAndOpacity"),  TEXT("ColorAndOpacity"),  TEXT("Foreground color and opacity."));
    AddMappingTo(TypeRegistry, FName(TEXT("TextBlock")), TEXT("Justification"),    TEXT("Justification"),    TEXT("Horizontal text justification token."));
    AddMappingTo(TypeRegistry, FName(TEXT("TextBlock")), TEXT("AutoWrapText"),     TEXT("AutoWrapText"),     TEXT("Whether the text wraps to fit the slot."));
    AddMappingTo(TypeRegistry, FName(TEXT("TextBlock")), TEXT("LineHeightPercentage"), TEXT("LineHeightPercentage"), TEXT("Line height scale for text layout."));
    AddMappingTo(TypeRegistry, FName(TEXT("TextBlock")), TEXT("ApplyLineHeightToBottomLine"), TEXT("ApplyLineHeightToBottomLine"), TEXT("Whether line height adds space below the final line."));
    AddMappingTo(TypeRegistry, FName(TEXT("TextBlock")), TEXT("ShadowOffset"),     TEXT("ShadowOffset"),     TEXT("Drop shadow offset."));
    AddMappingTo(TypeRegistry, FName(TEXT("TextBlock")), TEXT("ShadowColorAndOpacity"), TEXT("ShadowColorAndOpacity"), TEXT("Drop shadow color."));

    // Image — brush + tint.
    AddMappingTo(TypeRegistry, FName(TEXT("Image")), TEXT("Brush"),            TEXT("Brush"),            TEXT("FSlateBrush — brush asset and draw type."));
    AddMappingTo(TypeRegistry, FName(TEXT("Image")), TEXT("ColorAndOpacity"),  TEXT("ColorAndOpacity"),  TEXT("Tint applied to the brush."));

    // Button — clickable wrapper.
    AddMappingTo(TypeRegistry, FName(TEXT("Button")), TEXT("ColorAndOpacity"),       TEXT("ColorAndOpacity"),       TEXT("Tint applied to the button frame."));
    AddMappingTo(TypeRegistry, FName(TEXT("Button")), TEXT("BackgroundColor"),       TEXT("BackgroundColor"),       TEXT("Background color of the button."));
    AddMappingTo(TypeRegistry, FName(TEXT("Button")), TEXT("Style"),                 TEXT("WidgetStyle"),           TEXT("FButtonStyle struct."));
    AddMappingTo(TypeRegistry, FName(TEXT("Button")), TEXT("ClickMethod"),           TEXT("ClickMethod"),           TEXT("EButtonClickMethod token."));
    AddMappingTo(TypeRegistry, FName(TEXT("Button")), TEXT("IsFocusable"),           TEXT("IsFocusable"),           TEXT("Whether the button accepts focus."));

    // CommonButtonBase — CommonUI action-bar wiring properties.
    //
    // Bug #2 fix (2026-05-16 UI gap audit): these two properties were the
    // hottest "needs raw_mode=true" papercut for callers wiring action-bar
    // bindings. Both ship on the curated allowlist now so the standard
    // set_widget_property path works without the raw-mode bypass.
    //
    // Token comes from MakeTokenFromClassName(UCommonButtonBase::StaticClass())
    // which strips the leading 'U' and returns "CommonButtonBase".
    AddMappingTo(TypeRegistry, FName(TEXT("CommonButtonBase")), TEXT("TriggeringInputAction"), TEXT("TriggeringInputAction"), TEXT("FDataTableRowHandle pointing at the CommonInputActionDataBase row this button binds."));
    AddMappingTo(TypeRegistry, FName(TEXT("CommonButtonBase")), TEXT("bDisplayInActionBar"),   TEXT("bDisplayInActionBar"),   TEXT("Whether this button surfaces in a bound action bar."));

    // ProgressBar — value + style.
    AddMappingTo(TypeRegistry, FName(TEXT("ProgressBar")), TEXT("Percent"),     TEXT("Percent"),     TEXT("Fill percent (0..1)."));
    AddMappingTo(TypeRegistry, FName(TEXT("ProgressBar")), TEXT("FillColorAndOpacity"), TEXT("FillColorAndOpacity"), TEXT("Color of the fill region."));
    AddMappingTo(TypeRegistry, FName(TEXT("ProgressBar")), TEXT("BarFillType"), TEXT("BarFillType"), TEXT("EProgressBarFillType token."));

    // ----- Plugin widgets -----

    // RoundedBorder (optional provider) -- pre-work for Phase E. The class may
    // not be loaded in stripped builds; AddMappingTo no-ops in that case.
    // DEPRECATED in favour of EffectSurface (Phase E, 2026-04-26) but kept
    // for backwards compatibility — the curated mapping stays so existing
    // WBPs continue to author correctly via the spec/builder pipeline.
    {
        const FName RoundedToken = FName(TEXT("RoundedBorder"));
        AddMappingTo(TypeRegistry, RoundedToken, TEXT("CornerRadii"),  TEXT("CornerRadii"),  TEXT("FVector4 per-corner radii: TL/TR/BR/BL."));
        AddMappingTo(TypeRegistry, RoundedToken, TEXT("OutlineColor"), TEXT("OutlineColor"), TEXT("Outline color (linear)."));
        AddMappingTo(TypeRegistry, RoundedToken, TEXT("OutlineWidth"), TEXT("OutlineWidth"), TEXT("Outline thickness in slate units."));
        AddMappingTo(TypeRegistry, RoundedToken, TEXT("FillColor"),    TEXT("FillColor"),    TEXT("Fill color of the rounded box."));

        // RoundedBorder is a UBorder subclass — it inherits Border's properties,
        // so re-publish the same Background / BrushColor / Padding mapping on
        // the child token. This keeps allowlist diagnostics self-contained per
        // type rather than forcing callers to know about inheritance.
        AddMappingTo(TypeRegistry, RoundedToken, TEXT("Background"),     TEXT("Background"),     TEXT("Inherited from Border."));
        AddMappingTo(TypeRegistry, RoundedToken, TEXT("BrushColor"),     TEXT("BrushColor"),     TEXT("Inherited from Border."));
        AddMappingTo(TypeRegistry, RoundedToken, TEXT("Padding"),        TEXT("Padding"),        TEXT("Inherited from Border."));
    }

    // EffectSurface (optional provider) -- Phase E successor to RoundedBorder.
    // SDF-driven, single FSlateDrawElement::MakeBox per paint. The curated
    // mapping covers every FEffectSurfaceConfig sub-bag path so the Phase F
    // action wrappers (set_effect_surface_corners, set_effect_surface_fill,
    // ...) have a known-allowed surface area to write against. Slot.* paths
    // are added below in the common-children loop.
    //
    // Engine reflection paths are dotted UPROPERTY chains: `Config.Shape.CornerRadii`
    // resolves through the FEffectSurfaceConfig USTRUCT cluster. The Phase C
    // FUIReflectionHelper handles nested struct walks via FStructProperty.
    {
        const FName EffectToken = FName(TEXT("EffectSurface"));

        // Shape sub-bag
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Shape.CornerRadii"), TEXT("Config.Shape.CornerRadii"), TEXT("FVector4 per-corner radii: TL/TR/BR/BL (slate units)."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Shape.Smoothness"),  TEXT("Config.Shape.Smoothness"),  TEXT("SDF anti-alias band width in slate units."));

        // Fill sub-bag
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Fill.Mode"),         TEXT("Config.Fill.Mode"),         TEXT("EEffectFillMode token: Solid / Linear / Radial."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Fill.SolidColor"),   TEXT("Config.Fill.SolidColor"),   TEXT("Uniform fill colour (Solid mode)."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Fill.Stops"),        TEXT("Config.Fill.Stops"),        TEXT("Gradient colour-stop array (cap 8, dropped extras logged)."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Fill.Angle"),        TEXT("Config.Fill.Angle"),        TEXT("Linear gradient direction in degrees (CSS: 0=right, 90=down)."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Fill.RadialCenter"), TEXT("Config.Fill.RadialCenter"), TEXT("Radial gradient origin in normalised rect coords (0..1)."));

        // Border sub-bag
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Border.Width"),     TEXT("Config.Border.Width"),     TEXT("Border thickness in slate units."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Border.Color"),     TEXT("Config.Border.Color"),     TEXT("Border colour (linear)."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Border.Offset"),    TEXT("Config.Border.Offset"),    TEXT("Positive=inset, negative=outset."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Border.Glow"),      TEXT("Config.Border.Glow"),      TEXT("Border halo radius (slate units)."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Border.GlowColor"), TEXT("Config.Border.GlowColor"), TEXT("Border halo colour."));

        // Drop / inner shadow stacks (CSS-style layered list, cap 4 per stack)
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.DropShadow"),  TEXT("Config.DropShadow"),  TEXT("TArray<FEffectShadow> drop-shadow layers (cap 4)."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.InnerShadow"), TEXT("Config.InnerShadow"), TEXT("TArray<FEffectShadow> inner-shadow layers (cap 4)."));

        // Glow sub-bag
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Glow.Radius"),        TEXT("Config.Glow.Radius"),        TEXT("Glow halo radius (slate units)."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Glow.Color"),         TEXT("Config.Glow.Color"),         TEXT("Glow colour."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Glow.Intensity"),     TEXT("Config.Glow.Intensity"),     TEXT("Glow brightness multiplier."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Glow.InnerOuterMix"), TEXT("Config.Glow.InnerOuterMix"), TEXT("0=outer glow, 1=inner glow, mid blends."));

        // Filter sub-bag (saturation/brightness/contrast — hue/blur deferred to v2)
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Filter.Saturation"), TEXT("Config.Filter.Saturation"), TEXT("CSS filter saturation (1.0 = identity)."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Filter.Brightness"), TEXT("Config.Filter.Brightness"), TEXT("CSS filter brightness (1.0 = identity)."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.Filter.Contrast"),   TEXT("Config.Filter.Contrast"),   TEXT("CSS filter contrast (1.0 = identity)."));

        // Backdrop blur (engine SBackgroundBlur wrap; structural toggle)
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.BackdropBlur.Strength"), TEXT("Config.BackdropBlur.Strength"), TEXT("0=off, >0 wraps tree in SBackgroundBlur."));

        // Inset highlight
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.InsetHighlight.Offset"),    TEXT("Config.InsetHighlight.Offset"),    TEXT("Inset highlight offset (slate units)."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.InsetHighlight.Blur"),      TEXT("Config.InsetHighlight.Blur"),      TEXT("Inset highlight blur radius."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.InsetHighlight.Color"),     TEXT("Config.InsetHighlight.Color"),     TEXT("Inset highlight colour."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.InsetHighlight.Intensity"), TEXT("Config.InsetHighlight.Intensity"), TEXT("Inset highlight brightness multiplier."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.InsetHighlight.EdgeMask"),  TEXT("Config.InsetHighlight.EdgeMask"),  TEXT("EEffectInsetEdge bitmask: combine via bitwise OR."));

        // Top-level
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.ContentPadding"), TEXT("Config.ContentPadding"), TEXT("FMargin inside the SDF surface, applied to the content child."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.FeatureFlags"),   TEXT("Config.FeatureFlags"),   TEXT("EEffectSurfaceFeature bitmask — drives shader permutation."));
        AddMappingTo(TypeRegistry, EffectToken, TEXT("Effect.BaseMaterial"),   TEXT("BaseMaterial"),          TEXT("TSoftObjectPtr<UMaterialInterface> — parent material for the MID."));
    }

    // ----- Slot mappings -----
    //
    // Each child widget gets the Slot.* paths that are LIKELY to be valid. The
    // builder still validates per-parent at write time; this allowlist is the
    // generous-but-bounded outer envelope of what's permissible.
    //
    // We attach all common slot shapes to common leaves so that a TextBlock
    // dropped into a CanvasPanel OR a VerticalBox both have the right slot
    // paths in their per-type allowlist. The reflection helper rejects writes
    // whose actual slot class doesn't have the field — the allowlist isn't
    // the only line of defence.
    {
        const TArray<FName> CommonChildren = {
            FName(TEXT("TextBlock")),
            FName(TEXT("Image")),
            FName(TEXT("Button")),
            FName(TEXT("Border")),
            FName(TEXT("SizeBox")),
            FName(TEXT("ProgressBar")),
            FName(TEXT("VerticalBox")),
            FName(TEXT("HorizontalBox")),
            FName(TEXT("Overlay")),
            FName(TEXT("CanvasPanel")),
            FName(TEXT("ScrollBox")),
            FName(TEXT("RoundedBorder")),
            FName(TEXT("EffectSurface")),  // Phase E
        };

        for (const FName& ChildToken : CommonChildren)
        {
            AddCanvasSlotMappings(TypeRegistry, ChildToken);
            AddBoxSlotMappings(TypeRegistry, ChildToken);
        }
    }
}
