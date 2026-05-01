// Copyright tumourlove. All Rights Reserved.
// UISpec.h
//
// Schema-driven UI document type cluster used by `ui::build_ui_from_spec` and
// `ui::dump_ui_spec`. The shapes here are intentionally typed (no
// `TMap<FString,FString>` bags) so the validator can fail fast on unknown
// keys before any asset mutation runs.
//
// Recursion model:
//   `FUISpecNode::Children` is `TArray<TSharedPtr<FUISpecNode>>` — NOT a
//   `UPROPERTY`. UHT rejects USTRUCT recursion through reflected members
//   (a struct cannot contain a `TArray<Self>` via UPROPERTY) and `TSharedPtr`
//   is not a UPROPERTY-supported type. Consequently, `FUISpecNode` is
//   `USTRUCT()` without `BlueprintType` and the JSON parser walks the tree
//   manually. Surfacing the spec tree to Blueprints is out-of-scope for v1;
//   if a hard requirement appears, swap to a `UDataAsset`-rooted layout with
//   `TArray<TObjectPtr<UUISpecNode>>` (UObjects can recurse).
//
// Leaf, non-recursive structs ARE `USTRUCT(BlueprintType)` so they can be
// used directly in BP graphs (e.g. as data-only wrappers fed into runtime
// builders).

#pragma once

#include "CoreMinimal.h"
#include "Layout/Margin.h"
#include "UISpec.generated.h"

/**
 * Severity of a validator finding.
 */
UENUM(BlueprintType)
enum class EUISpecErrorSeverity : uint8
{
    Info     UMETA(DisplayName = "Info"),
    Warning  UMETA(DisplayName = "Warning"),
    Error    UMETA(DisplayName = "Error"),
};

/**
 * One validator finding. Carries enough context for both human-readable and
 * LLM-targeted formatting (see `FUISpecValidationResult::ToString` /
 * `ToLLMReport`).
 */
USTRUCT(BlueprintType)
struct MONOLITHUI_API FUISpecError
{
    GENERATED_BODY()

    /** Where in the document this finding originated. JSON pointer style (e.g. `/root/children/0/style/width`). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FString JsonPath;

    /** Optional widget Id (when the error is attributable to a specific node). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FName WidgetId;

    /** Coarse classification — used by the LLM-formatter to group findings. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FName Category;

    /** Severity level. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    EUISpecErrorSeverity Severity = EUISpecErrorSeverity::Error;

    /** Human-readable message. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FString Message;

    /** Optional remediation hint ("did you mean X?", "valid options are A/B/C"). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FString SuggestedFix;

    /** Optional list of valid alternatives (enum values, allowed property paths). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    TArray<FString> ValidOptions;

    /** Source line number, when known (1-based; 0 = unknown). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    int32 Line = 0;

    /** Source column number, when known (1-based; 0 = unknown). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    int32 Column = 0;

    // Phase K — single-finding formatters. Same dual-audience contract as
    // FUISpecValidationResult: ToString is human-friendly one-liner; ToLLMReport
    // is a stable key:value block with explicit field labels (json_path,
    // suggested_fix, valid_options) so an LLM can grep / re-prompt against it
    // without having to parse free-form prose. Action handlers that previously
    // returned bare error strings now build a transient FUISpecError and call
    // ToLLMReport() to populate FMonolithActionResult::Error's message field.
    /** Format this single finding as a one-line human summary (matches FUISpecValidationResult::ToString style). */
    FString ToString() const;

    /** Format this single finding as a structured key:value block aimed at LLM consumers. */
    FString ToLLMReport() const;
};

/**
 * Aggregate result of a validator pass. `bIsValid == false` when any entry in
 * `Errors` is severity Error (warnings alone do not invalidate unless
 * `treat_warnings_as_errors` is set on the document).
 */
USTRUCT(BlueprintType)
struct MONOLITHUI_API FUISpecValidationResult
{
    GENERATED_BODY()

    /** True when no Error-severity findings are present. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    bool bIsValid = false;

    /** Error-severity findings (block builder execution). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    TArray<FUISpecError> Errors;

    /** Warning-severity findings (do not block unless `treat_warnings_as_errors` is set). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    TArray<FUISpecError> Warnings;

    /** Format a human-friendly multi-line summary. */
    FString ToString() const;

    /** Format a structured report aimed at LLM consumers (categorised, with suggested fixes). */
    FString ToLLMReport() const;
};

/**
 * Typed metadata bag — replaces a stringly-typed `TMap<FString,FString>` so
 * the validator can refuse unknown top-level keys.
 */
USTRUCT(BlueprintType)
struct MONOLITHUI_API FUISpecMetadata
{
    GENERATED_BODY()

    /** Free-form authoring tool identifier (e.g. "DesignTool", "Figma"). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FString AuthoringTool;

    /** Optional source-file hint. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FString SourceFile;

    /** Free-form author/owner. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FString Author;

    /** Free-form description for the spec document. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FString Description;

    /** Tags (e.g. "hud", "menu", "settings") — used by future filters/searches. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    TArray<FName> Tags;
};

/**
 * Slot sub-bag — describes how a node is laid out within its parent. The
 * validator only emits warnings for slot fields that are inapplicable to the
 * parent's slot class (e.g. `Anchors` on a vertical box slot).
 */
USTRUCT(BlueprintType)
struct MONOLITHUI_API FUISpecSlot
{
    GENERATED_BODY()

    /** Anchor preset name (one of the curated set in `MonolithUICommon::GetAnchorPreset`). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FName AnchorPreset;

    /** Position in slot coordinates. Interpretation depends on slot class. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FVector2D Position = FVector2D::ZeroVector;

    /** Size in slot coordinates. Interpretation depends on slot class. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FVector2D Size = FVector2D::ZeroVector;

    /** Pivot/alignment within the slot. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FVector2D Alignment = FVector2D::ZeroVector;

    /** Padding margin (canvas/border/etc.). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FMargin Padding;

    /** Whether the slot auto-sizes to its child. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    bool bAutoSize = false;

    /** Z order (canvas slot only). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    int32 ZOrder = 0;

    /** Horizontal alignment token (Left/Center/Right/Fill). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FName HAlign;

    /** Vertical alignment token (Top/Center/Bottom/Fill). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FName VAlign;
};

/**
 * Visual style sub-bag. Most leaf widgets honour a subset; the validator's
 * allowlist is the source of truth for which combinations are legal.
 */
USTRUCT(BlueprintType)
struct MONOLITHUI_API FUISpecStyle
{
    GENERATED_BODY()

    /** Optional explicit width (0 = unset; use the slot's natural size). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    float Width = 0.f;

    /** Optional explicit height (0 = unset). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    float Height = 0.f;

    /** Inner padding (applied via SizeBox/Border/etc. depending on widget). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FMargin Padding;

    /** Background color (post-degamma linear). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FLinearColor Background = FLinearColor::Transparent;

    /** Border color. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FLinearColor BorderColor = FLinearColor::Transparent;

    /** Border thickness. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    float BorderWidth = 0.f;

    /** Render opacity (0..1). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    float Opacity = 1.f;

    /** Visibility token (Visible/Hidden/Collapsed/HitTestInvisible/SelfHitTestInvisible). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FName Visibility;

    /** Whether the widget enforces an explicit size (drives SizeBox usage). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    bool bUseCustomSize = false;
};

/**
 * Content sub-bag — fields that map to widget-content properties (text,
 * brushes, placeholders).
 */
USTRUCT(BlueprintType)
struct MONOLITHUI_API FUISpecContent
{
    GENERATED_BODY()

    /** Display text (TextBlock, Button label, EditableText placeholder). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FString Text;

    /** Font size in points (TextBlock/RichTextBlock). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    float FontSize = 0.f;

    /** Font color (post-degamma linear). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FLinearColor FontColor = FLinearColor::White;

    /** Text wrap mode token (None/Wrap/WrapAtWordBoundary). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FName WrapMode;

    /** Brush asset path (Image widget — `/Game/...` or `/Engine/...`). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FString BrushPath;

    /** Placeholder text (EditableText/EditableTextBox). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FString Placeholder;
};

/**
 * Single drop/inner shadow layer used by `FUISpecEffect`. Mirrors the CSS
 * `box-shadow` model: offset + blur + spread + color, plus an inset bit.
 */
USTRUCT(BlueprintType)
struct MONOLITHUI_API FUISpecEffectShadow
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FVector2D Offset = FVector2D::ZeroVector;

    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    float Blur = 0.f;

    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    float Spread = 0.f;

    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FLinearColor Color = FLinearColor::Black;

    /** True for inner-shadow semantics, false for drop-shadow. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    bool bInset = false;
};

/**
 * Effect sub-bag — drives the future `UEffectSurface` widget (Phase E). All
 * fields are tolerated as no-ops in v1 if the type registry has not yet
 * surfaced `UEffectSurface`.
 */
USTRUCT(BlueprintType)
struct MONOLITHUI_API FUISpecEffect
{
    GENERATED_BODY()

    /** Per-corner radii in pixels: TL / TR / BR / BL. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FVector4 CornerRadii = FVector4(0.0, 0.0, 0.0, 0.0);

    /** SDF anti-alias smoothness (0 = hard, ~1.5 = soft). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    float Smoothness = 1.f;

    /** Solid fill color. Used unless gradient stops below are populated. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FLinearColor SolidColor = FLinearColor::White;

    /** Drop shadow stack (engine cap of 4 layers; validator warns at 5+). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    TArray<FUISpecEffectShadow> DropShadows;

    /** Inner shadow stack (engine cap of 4 layers). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    TArray<FUISpecEffectShadow> InnerShadows;

    /** Backdrop blur strength (0 = off; non-zero auto-wraps in UBackgroundBlur). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    float BackdropBlurStrength = 0.f;
};

/**
 * CommonUI sub-bag — populated when a node maps to a CommonUI widget subclass.
 * v1 stores tokens; the builder resolves them via `UMonolithUIStyleService`.
 */
USTRUCT(BlueprintType)
struct MONOLITHUI_API FUISpecCommonUI
{
    GENERATED_BODY()

    /** UI input layer token (Game/Menu/GameMenu/Modal). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FName InputLayer;

    /** Activatable input mode token (Game/Menu/All). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FName InputMode;

    /** Style asset references (named entries in `FUISpecDocument::Styles`). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    TArray<FName> StyleRefs;
};

/**
 * One animation keyframe. The variant payload is split across three optional
 * fields rather than a real `TVariant` because `TVariant` is not a UPROPERTY-
 * supported type; the writer/reader inspects which is non-default to decide.
 */
USTRUCT(BlueprintType)
struct MONOLITHUI_API FUISpecKeyframe
{
    GENERATED_BODY()

    /** Time in seconds, document-relative. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    float Time = 0.f;

    /** Scalar value (RenderOpacity, FontSize, etc.). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    float ScalarValue = 0.f;

    /** Vector2D value (Translation, Scale). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FVector2D Vector2DValue = FVector2D::ZeroVector;

    /** Color value (Tint, Color). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FLinearColor ColorValue = FLinearColor::White;

    /** Easing token (Linear/Cubic/Constant/Bezier). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FName Easing;

    /** True when ArriveTangent/LeaveTangent below should be used (cubic only). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    bool bUseCustomTangents = false;

    /** Cubic arrival tangent. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    float ArriveTangent = 0.f;

    /** Cubic leave tangent. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    float LeaveTangent = 0.f;

    /** Cubic arrival weight. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    float ArriveWeight = 0.f;

    /** Cubic leave weight. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    float LeaveWeight = 0.f;
};

/**
 * One named animation. Keyframes are explicit; the high-level (from/to + duration)
 * shape is sugar that the parser expands to two keyframes.
 */
USTRUCT(BlueprintType)
struct MONOLITHUI_API FUISpecAnimation
{
    GENERATED_BODY()

    /** Animation name (becomes the `UWidgetAnimation` UObject name). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FName Name;

    /** Target widget Id (must reference a node Id in the document). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FName TargetWidgetId;

    /** Target property token (e.g. `RenderOpacity`, `Color`, `Translation`). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FName TargetProperty;

    /** Total duration in seconds (used for the from/to sugar shape). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    float Duration = 0.f;

    /** Start delay in seconds. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    float Delay = 0.f;

    /** Easing token used by the from/to sugar shape. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FName Easing;

    /** Loop mode token (None/Loop/PingPong). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    FName LoopMode;

    /** Start automatically on widget construction. */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    bool bAutoPlay = false;

    /** Detailed keyframes (preferred over from/to sugar). */
    UPROPERTY(BlueprintReadWrite, Category = "MonolithUI|Spec")
    TArray<FUISpecKeyframe> Keyframes;
};

// Forward declaration for the recursive `Children` field below.
struct FUISpecNode;

/**
 * One node in the spec tree. `Children` is intentionally NOT a UPROPERTY —
 * see the file header comment for the recursion-via-TSharedPtr rationale.
 *
 * NOTE: Because `Children` is not reflected, GC does not traverse through it
 * automatically. Construction and lifetime are owned by the parser/builder
 * which keep the root pinned for the duration of a build pass.
 */
USTRUCT()
struct MONOLITHUI_API FUISpecNode
{
    GENERATED_BODY()

    /** Type token resolved through `FUITypeRegistry` (e.g. `VerticalBox`, `EffectSurface`). */
    UPROPERTY()
    FName Type;

    /** Becomes the widget's variable name on the WBP. */
    UPROPERTY()
    FName Id;

    /** How this node sits within its parent. */
    UPROPERTY()
    FUISpecSlot Slot;

    /** Visual style sub-bag. */
    UPROPERTY()
    FUISpecStyle Style;

    /** Content sub-bag (text, brush, placeholder). */
    UPROPERTY()
    FUISpecContent Content;

    /** Effect surface configuration (UEffectSurface only — ignored otherwise). */
    UPROPERTY()
    FUISpecEffect Effect;

    /** True when the node opts into the effect bag. Mimics TOptional's "set" bit. */
    UPROPERTY()
    bool bHasEffect = false;

    /** CommonUI configuration. */
    UPROPERTY()
    FUISpecCommonUI CommonUI;

    /** True when the node opts into the CommonUI bag. */
    UPROPERTY()
    bool bHasCommonUI = false;

    /** References to global animation names (resolved against `FUISpecDocument::Animations`). */
    UPROPERTY()
    TArray<FName> AnimationRefs;

    /** Reference to a named entry in `FUISpecDocument::Styles`. */
    UPROPERTY()
    FName StyleRef;

    /** Custom class path for unknown types (e.g. `/Game/UI/MyCustomWidget.MyCustomWidget_C`). */
    UPROPERTY()
    FString CustomClassPath;

    /**
     * Children — TArray<TSharedPtr<...>> deliberately, NOT a UPROPERTY.
     * USTRUCT recursion through reflected members is rejected by UHT and
     * `TSharedPtr` is not reflectable. The parser owns these shared pointers.
     */
    TArray<TSharedPtr<FUISpecNode>> Children;
};

/**
 * Top-level spec document. Carries the root node plus document-scoped tables
 * (named styles, named animations) and the metadata bag.
 *
 * `Root` is held by `TSharedPtr` for the same reason `Children` is — the root
 * itself is a `FUISpecNode` and the recursion rule applies.
 */
USTRUCT()
struct MONOLITHUI_API FUISpecDocument
{
    GENERATED_BODY()

    /** Schema version. Bumped when the parser ships an incompatible change. */
    UPROPERTY()
    int32 Version = 1;

    /** Widget Blueprint name. The builder maps this to the asset filename. */
    UPROPERTY()
    FString Name;

    /** Parent class — short token (UUserWidget/UCommonActivatableWidget/UCommonUserWidget) or full path. */
    UPROPERTY()
    FString ParentClass;

    /** Typed metadata. */
    UPROPERTY()
    FUISpecMetadata Metadata;

    /** Named style table (referenced from nodes via `FUISpecNode::StyleRef`). */
    UPROPERTY()
    TMap<FName, FUISpecStyle> Styles;

    /** Named animation table (each node references entries by name). */
    UPROPERTY()
    TArray<FUISpecAnimation> Animations;

    /** When true, validator warnings escalate to errors and block the build. */
    UPROPERTY()
    bool bTreatWarningsAsErrors = false;

    /** Root node (TSharedPtr — see `FUISpecNode` for the recursion rationale). */
    TSharedPtr<FUISpecNode> Root;
};
