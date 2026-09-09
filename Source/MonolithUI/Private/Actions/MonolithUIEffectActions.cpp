// Copyright tumourlove. All Rights Reserved.
#include "Actions/MonolithUIEffectActions.h"

// Monolith registry.
#include "MonolithToolRegistry.h"

// JSON.
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// UMG editor + runtime.
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "WidgetBlueprint.h"

// Kismet editor (compile + structural mark).
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

// Reflection helper (Phase C path).
#include "Registry/MonolithUIRegistrySubsystem.h"
#include "Registry/UIPropertyAllowlist.h"
#include "Registry/UIPropertyPathCache.h"
#include "Registry/UIReflectionHelper.h"
#include "Registry/UITypeRegistry.h"

// MonolithUI shared color parser + log category + EffectSurface probe API
// (R3b — `MonolithUI::GetEffectSurfaceClass()` / `IsEffectSurfaceAvailable()`).
#include "MonolithUICommon.h"

// MonolithUIInternal — `MakeOptionalDepUnavailableError` (§5.5 contract helper).
#include "MonolithUIInternal.h"

// Required for LoadObject<UMaterialInterface>(...) in HandleApplyPreset.
#include "Materials/MaterialInterface.h"

#include "Math/Color.h"
#include "Math/Vector2D.h"
#include "Math/Vector4.h"
#include "Misc/Optional.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

// =============================================================================
// R2 (2026-04-26) -- optional EffectSurface provider decouple
// =============================================================================
//
// This file deliberately holds ZERO direct UEffectSurface symbol references
// (no #include "EffectSurface.h", no #include "EffectSurfaceConfig.h", no
// #include "EffectSurfacePresets.h", no Cast<UEffectSurface>, no enum class
// reference, no typed setter call). All EffectSurface interaction goes
// through:
//
//   1. The probe API in MonolithUICommon.h
//      (`MonolithUI::IsEffectSurfaceAvailable()` /
//       `MonolithUI::GetEffectSurfaceClass()`).
//   2. UWidget::IsA(EffectSurfaceClass) for identity checks.
//   3. The Phase C reflection helper (`FUIReflectionHelper::Apply`) for all
//      property writes — same allowlist gate, same type registry mapping,
//      same ImportText_Direct fallback as the typed code path.
//   4. UFunction::ProcessEvent for the two BP-callable side effects we need
//      (`ForceMIDRefresh` — promoted to UFUNCTION in this same wave —
//       and `SetBaseMaterial`).
//
// On EffectSurface unavailable (provider physically absent, OR loaded but the
// UClass has not yet been registered, OR the editor's
// hot-reload path has invalidated the cache), every action handler
// returns the canonical §5.5 error payload via the
// `MonolithUIInternal::MakeOptionalDepUnavailableError` helper:
//
//   { bSuccess: false, ErrorCode: -32010,
//     ErrorMessage: "EffectSurface widget unavailable -- optional provider...",
//     Result: { dep_name, widget_type, alternative, category } }
//
// Behaviour preservation: when an EffectSurface provider is loaded, every action
// produces a byte-identical paths_written / warnings / compiled response
// to the pre-R2 typed implementation. The reflection helper writes through
// to the same FProperty offsets the typed setters wrote through; the
// FeatureFlags merge does the same OR semantics; the MID push and the
// blueprint mark-modified bookkeeping run in the same order.
//
// EEffectSurfaceFeature bit values mirrored as raw constants so this file does
// not include any provider enum header. If the provider's canonical enum values
// ever change, this table must change in lockstep.
// A drift-detection note lives in the SPEC; the Final.4 acceptance gate
// re-verifies the bit semantics against a real EffectSurface UClass.
//
//   None           = 0
//   RoundedCorners = 1 << 0   (1)
//   SolidFill      = 1 << 1   (2)
//   LinearGradient = 1 << 2   (4)
//   RadialGradient = 1 << 3   (8)
//   Border         = 1 << 4   (16)
//   DropShadow     = 1 << 5   (32)
//   InnerShadow    = 1 << 6   (64)
//   Glow           = 1 << 7   (128)
//   Filter         = 1 << 8   (256)
//   BackdropBlur   = 1 << 9   (512)
//   InsetHighlight = 1 << 10  (1024)
//
// EEffectFillMode tokens (uint8 enum values mirrored — only the string
// form is used at the JSON path surface; raw values are not needed).
//
//   Solid  -> "Solid"
//   Linear -> "Linear"
//   Radial -> "Radial"
// =============================================================================

namespace MonolithUI::EffectActionsInternal
{
    // -------------------------------------------------------------------------
    // EEffectSurfaceFeature bit constants (raw — see header comment above).
    // -------------------------------------------------------------------------
    static constexpr uint32 EFFECT_FEATURE_ROUNDED_CORNERS = 1u << 0;
    static constexpr uint32 EFFECT_FEATURE_SOLID_FILL      = 1u << 1;
    static constexpr uint32 EFFECT_FEATURE_LINEAR_GRADIENT = 1u << 2;
    static constexpr uint32 EFFECT_FEATURE_RADIAL_GRADIENT = 1u << 3;
    static constexpr uint32 EFFECT_FEATURE_BORDER          = 1u << 4;
    static constexpr uint32 EFFECT_FEATURE_DROP_SHADOW     = 1u << 5;
    static constexpr uint32 EFFECT_FEATURE_INNER_SHADOW    = 1u << 6;
    static constexpr uint32 EFFECT_FEATURE_GLOW            = 1u << 7;
    static constexpr uint32 EFFECT_FEATURE_FILTER          = 1u << 8;
    static constexpr uint32 EFFECT_FEATURE_BACKDROP_BLUR   = 1u << 9;
    static constexpr uint32 EFFECT_FEATURE_INSET_HIGHLIGHT = 1u << 10;

    /**
     * Common pre-amble: section 5.5 probe FIRST (fail-fast on provider
     * absence), then load WBP, locate widget, verify it IsA the EffectSurface
     * UClass. Returns the widget as an untyped UWidget* (the typed
     * UEffectSurface* return type pre-R2 was the principal cross-module
     * symbol leak this file exists to eliminate).
     *
     * On any failure populates OutError and returns nullptr; on success
     * returns the widget pointer and stashes the WBP through OutWBP so the
     * caller can mark it modified + optionally compile.
     *
     * Defensive layering: each per-action handler MAY (and does) probe again
     * at its own top, but the resolver's probe is the contractual gate. The
     * second probe in the handler is a fail-fast diagnostic for the case
     * where the class state changes between the resolver returning and the
     * handler executing (impossible on the game thread today; cheap defense
     * for future async paths).
     */
    static UWidget* ResolveEffectSurface(
        const TSharedPtr<FJsonObject>& Params,
        UWidgetBlueprint*& OutWBP,
        FMonolithActionResult& OutError)
    {
        OutWBP = nullptr;

        // Section 5.5 contract: fail-fast on optional provider absence. Single source
        // of truth for the canonical error payload — never inlined per call
        // site (per the plan §5.5.3 "constructed via a single helper" rule).
        UClass* EffectSurfaceClass = MonolithUI::GetEffectSurfaceClass();
        if (!EffectSurfaceClass)
        {
            OutError = MonolithUIInternal::MakeOptionalDepUnavailableError(
                TEXT("EffectSurface"),
                TEXT("optional EffectSurface provider"),
                TEXT("ui::set_widget_property with a different widget type"));
            return nullptr;
        }

        if (!Params.IsValid())
        {
            OutError = FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
            return nullptr;
        }

        FString AssetPath, WidgetName;
        if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        {
            OutError = FMonolithActionResult::Error(TEXT("Missing or empty required param: asset_path"), -32602);
            return nullptr;
        }
        if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
        {
            OutError = FMonolithActionResult::Error(TEXT("Missing or empty required param: widget_name"), -32602);
            return nullptr;
        }

        UWidgetBlueprint* WBP = MonolithUI::LoadWidgetBlueprint(AssetPath, OutError);
        if (!WBP)
        {
            return nullptr; // OutError already populated.
        }
        if (!WBP->WidgetTree)
        {
            OutError = FMonolithActionResult::Error(
                FString::Printf(TEXT("Widget Blueprint '%s' has no WidgetTree"), *AssetPath), -32603);
            return nullptr;
        }

        UWidget* Target = WBP->WidgetTree->FindWidget(FName(*WidgetName));
        if (!Target)
        {
            OutError = FMonolithActionResult::Error(
                FString::Printf(TEXT("Widget '%s' not found in '%s'"), *WidgetName, *AssetPath), -32602);
            return nullptr;
        }

        if (!Target->IsA(EffectSurfaceClass))
        {
            // Error message text is byte-stable across releases — test
            // assertions on the literal substring "expected EffectSurface"
            // remain valid.
            OutError = FMonolithActionResult::Error(
                FString::Printf(TEXT("Widget '%s' is class '%s' -- expected EffectSurface"),
                    *WidgetName, *Target->GetClass()->GetName()), -32602);
            return nullptr;
        }

        OutWBP = WBP;
        return Target;
    }

    /**
     * Apply a single property path through the gated reflection helper.
     *
     * The Phase E curated mappings translate the LLM-facing JSON path
     * (e.g. `Effect.Shape.CornerRadii`) into the engine-side reflection
     * path (`Config.Shape.CornerRadii`). The Phase C reflection helper
     * walks paths DIRECTLY against UPROPERTY names -- it doesn't consult
     * the registry's translation table -- so we do the lookup here:
     *
     *   1. Allowlist gate on the JSON path (preserves Phase E contract).
     *   2. Translate JSON path -> engine path via the registry mapping
     *      (no-op when the registered mapping has identical strings).
     *   3. Helper.Apply(EnginePath, ..., bRawMode=true) -- bypass the
     *      allowlist a second time because we already gated above.
     *
     * Returns true on success; on failure pushes a structured failure
     * line into OutFailures (so the caller can return all sub-failures
     * in one shot).
     *
     * R2 (2026-04-26): Surface is now untyped (UWidget*) per the
     * decouple — the underlying reflection helper is class-agnostic.
     */
    static bool ApplyPath(
        UWidget* Surface,
        const FString& JsonPath,
        const TSharedPtr<FJsonValue>& Value,
        TArray<FString>& OutPathsWritten,
        TArray<FString>& OutFailures)
    {
        UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get();
        if (!Sub)
        {
            OutFailures.Add(FString::Printf(
                TEXT("%s: registry subsystem unavailable"), *JsonPath));
            return false;
        }

        // Step 1 -- allowlist gate on the LLM-facing JSON path.
        const FUIPropertyAllowlist& Allow = Sub->GetAllowlist();
        const FName Token = MonolithUI::MakeTokenFromClassName(Surface->GetClass());
        if (!Allow.IsAllowed(Token, JsonPath))
        {
            OutFailures.Add(FString::Printf(
                TEXT("%s: NotInAllowlist for %s"), *JsonPath, *Token.ToString()));
            return false;
        }

        // Step 2 -- look up the engine-side path.
        FString EnginePath = JsonPath;
        if (const FUITypeRegistryEntry* Entry = Sub->GetTypeRegistry().FindByToken(Token))
        {
            for (const FUIPropertyMapping& M : Entry->PropertyMappings)
            {
                if (M.JsonPath == JsonPath)
                {
                    if (!M.EnginePath.IsEmpty()) EnginePath = M.EnginePath;
                    break;
                }
            }
        }

        // Step 3 -- helper walks the engine path; raw-mode bypasses the
        // helper's allowlist gate (we already gated above on the JSON form).
        FUIPropertyPathCache* Cache = Sub->GetPathCache();
        FUIReflectionHelper Helper(Cache, &Allow);
        const FUIReflectionApplyResult Res =
            Helper.Apply(Surface, EnginePath, Value, /*bRawMode=*/true);

        if (Res.bSuccess)
        {
            OutPathsWritten.Add(JsonPath); // Report the JSON form -- LLM-facing.
            return true;
        }

        OutFailures.Add(FString::Printf(
            TEXT("%s (engine='%s'): %s (%s)"),
            *JsonPath, *EnginePath, *Res.FailureReason, *Res.Detail));
        return false;
    }

    /**
     * OR `BitMask` into the widget's Effect.FeatureFlags via the reflection
     * helper. Read-modify-write so we don't clobber bits set by a sibling
     * sub-bag setter run earlier in the same batch.
     *
     * R2 (2026-04-26): the read-back side previously cast Surface to
     * UEffectSurface* and pulled Config.FeatureFlags through a typed
     * pointer. The decoupled form walks the FProperty graph reflectively:
     * `Surface->GetClass()->FindPropertyByName("Config")` -> FStructProperty
     * -> nested `FindPropertyByName("FeatureFlags")` -> read the int via
     * `ContainerPtrToValuePtr<int32>`. No FEffectSurfaceConfig type
     * reference; no UEffectSurface::StaticClass() reference.
     */
    static bool MergeFeatureFlagBits(
        UWidget* Surface,
        uint32 BitsToOr,
        TArray<FString>& OutPathsWritten,
        TArray<FString>& OutFailures)
    {
        // Reflective Config.FeatureFlags read-back. We work against the
        // widget's own UClass (resolved from the live instance) so the
        // codepath is type-agnostic and survives any future renaming of
        // the host UClass that preserves the FProperty layout.
        UClass* WidgetClass = Surface->GetClass();
        FProperty* ConfigProp = WidgetClass ? WidgetClass->FindPropertyByName(FName(TEXT("Config"))) : nullptr;
        FStructProperty* ConfigStructProp = CastField<FStructProperty>(ConfigProp);
        if (!ConfigStructProp || !ConfigStructProp->Struct)
        {
            OutFailures.Add(TEXT("Effect.FeatureFlags: cannot resolve Config struct on widget"));
            return false;
        }

        const void* ConfigPtr = ConfigStructProp->ContainerPtrToValuePtr<void>(Surface);
        FProperty* FlagsProp = ConfigStructProp->Struct->FindPropertyByName(FName(TEXT("FeatureFlags")));
        FIntProperty* FlagsIntProp = CastField<FIntProperty>(FlagsProp);
        if (!FlagsIntProp || !ConfigPtr)
        {
            OutFailures.Add(TEXT("Effect.FeatureFlags: cannot resolve FeatureFlags int on Config"));
            return false;
        }

        const int32 ExistingSigned = FlagsIntProp->GetPropertyValue_InContainer(ConfigPtr);
        const uint32 Existing = static_cast<uint32>(ExistingSigned);
        const int32  Merged   = static_cast<int32>(Existing | BitsToOr);

        TSharedPtr<FJsonValue> AsJson = MakeShared<FJsonValueNumber>(static_cast<double>(Merged));
        return ApplyPath(Surface, TEXT("Effect.FeatureFlags"), AsJson, OutPathsWritten, OutFailures);
    }

    /** Optional[float] reader. */
    static TOptional<float> TryGetFloat(const TSharedPtr<FJsonObject>& Params, const FString& Field)
    {
        double V = 0.0;
        if (Params.IsValid() && Params->TryGetNumberField(Field, V))
        {
            return TOptional<float>(static_cast<float>(V));
        }
        return TOptional<float>();
    }

    /** Optional[FLinearColor] reader -- accepts hex string or "R,G,B[,A]" via TryParseColor. */
    static bool TryGetColor(
        const TSharedPtr<FJsonObject>& Params,
        const FString& Field,
        FLinearColor& OutColor)
    {
        FString S;
        if (!Params.IsValid() || !Params->TryGetStringField(Field, S) || S.IsEmpty())
        {
            return false;
        }
        return MonolithUI::TryParseColor(S, OutColor);
    }

    /**
     * Build the JSON value for an effect-surface FLinearColor slot.
     *
     * These slots are MD_UI material parameters, NOT Slate tints: the effect
     * shader applies its own gamma, so they hold the sRGB bytes divided by 255
     * with NO degamma -- the `TryParseColor` contract in MonolithUICommon.h.
     * The reflection helper's hex-string branch is the engine-widget-property
     * path and DOES degamma, so a colour must never be handed to it as "#RRGGBB"
     * from here. Emitting the components as a float array takes the helper's
     * verbatim linear-array branch instead, which is also what the spec-side
     * `EffectSurfaceBuilder::MakeColor` does for the same properties.
     *
     * This replaces an older "#" + ToFColor(false).ToHex() round-trip that
     * relied on the helper's hex branch being a no-degamma pass-through. The
     * stored value is unchanged for hex input and now avoids the round-trip's
     * 8-bit quantisation for "r,g,b,a" float input.
     */
    static TSharedPtr<FJsonValue> MakeEffectColorJson(const FLinearColor& C)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Reserve(4);
        Arr.Add(MakeShared<FJsonValueNumber>(C.R));
        Arr.Add(MakeShared<FJsonValueNumber>(C.G));
        Arr.Add(MakeShared<FJsonValueNumber>(C.B));
        Arr.Add(MakeShared<FJsonValueNumber>(C.A));
        return MakeShared<FJsonValueArray>(Arr);
    }

    /** Same, for a colour still in its "#RRGGBBAA" / "r,g,b[,a]" text form. */
    static TSharedPtr<FJsonValue> MakeEffectColorJsonFromText(const FString& ColorText)
    {
        FLinearColor C;
        if (MonolithUI::TryParseColor(ColorText, C))
        {
            return MakeEffectColorJson(C);
        }
        // Unparseable -- forward the caller's raw text so the helper reports
        // ParseFailed against what was actually supplied.
        return MakeShared<FJsonValueString>(ColorText);
    }

    /** Optional[FVector2D] reader -- accepts {x,y} object or [x,y] array. */
    static bool TryGetVector2D(
        const TSharedPtr<FJsonObject>& Params,
        const FString& Field,
        FVector2D& OutVec)
    {
        if (!Params.IsValid()) return false;

        TSharedPtr<FJsonValue> Val = Params->TryGetField(Field);
        if (!Val.IsValid()) return false;

        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Val->TryGetArray(Arr) && Arr && Arr->Num() >= 2)
        {
            OutVec.X = (*Arr)[0]->AsNumber();
            OutVec.Y = (*Arr)[1]->AsNumber();
            return true;
        }

        const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
        if (Val->TryGetObject(ObjPtr) && ObjPtr && (*ObjPtr).IsValid())
        {
            double X = 0.0, Y = 0.0;
            (*ObjPtr)->TryGetNumberField(TEXT("x"), X);
            (*ObjPtr)->TryGetNumberField(TEXT("y"), Y);
            OutVec.X = X;
            OutVec.Y = Y;
            return true;
        }
        return false;
    }

    /**
     * Build the UE textual form of a TArray<FEffectShadow> for ImportText_Direct.
     * Format: `((Offset=(X=0,Y=4),Blur=12,Spread=0,Color=(R=0,G=0,B=0,A=0.5)),...)`.
     *
     * UE's struct/array text serialiser is the documented input format for
     * FArrayProperty::ImportText_Direct; the reflection helper falls back to it
     * for FArrayProperty paths. Cap at 4 entries (matches the widget's MID push
     * loop).
     */
    static FString MakeShadowArrayText(const TArray<TSharedPtr<FJsonValue>>& Layers)
    {
        constexpr int32 MaxLayers = 4;
        const int32 Count = FMath::Min(Layers.Num(), MaxLayers);

        FString Out = TEXT("(");
        for (int32 i = 0; i < Count; ++i)
        {
            if (!Layers[i].IsValid()) continue;
            const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
            if (!Layers[i]->TryGetObject(ObjPtr) || !ObjPtr || !(*ObjPtr).IsValid()) continue;
            const TSharedPtr<FJsonObject>& Obj = *ObjPtr;

            // Offset -- accept array [x,y] OR object {x,y}.
            double OX = 0.0, OY = 0.0;
            const TArray<TSharedPtr<FJsonValue>>* OffArr = nullptr;
            const TSharedPtr<FJsonObject>* OffObj = nullptr;
            if (Obj->TryGetArrayField(TEXT("offset"), OffArr) && OffArr && OffArr->Num() >= 2)
            {
                OX = (*OffArr)[0]->AsNumber();
                OY = (*OffArr)[1]->AsNumber();
            }
            else if (Obj->TryGetObjectField(TEXT("offset"), OffObj) && OffObj && (*OffObj).IsValid())
            {
                (*OffObj)->TryGetNumberField(TEXT("x"), OX);
                (*OffObj)->TryGetNumberField(TEXT("y"), OY);
            }

            double Blur   = 0.0;
            double Spread = 0.0;
            Obj->TryGetNumberField(TEXT("blur"), Blur);
            Obj->TryGetNumberField(TEXT("spread"), Spread);

            FLinearColor Color(0.0f, 0.0f, 0.0f, 0.5f);
            FString ColorStr;
            if (Obj->TryGetStringField(TEXT("color"), ColorStr) && !ColorStr.IsEmpty())
            {
                MonolithUI::TryParseColor(ColorStr, Color);
            }

            if (i > 0) Out += TEXT(",");
            Out += FString::Printf(
                TEXT("(Offset=(X=%f,Y=%f),Blur=%f,Spread=%f,Color=(R=%f,G=%f,B=%f,A=%f))"),
                OX, OY, Blur, Spread, Color.R, Color.G, Color.B, Color.A);
        }
        Out += TEXT(")");
        return Out;
    }

    /**
     * Build the UE text form of a TArray<FEffectColorStop> for the gradient
     * Stops list. Format: `((Position=0,Color=(R=...)),...)`. Cap at 8 (matches
     * the widget MID push).
     */
    static FString MakeStopArrayText(const TArray<TSharedPtr<FJsonValue>>& Stops)
    {
        constexpr int32 MaxStops = 8;
        const int32 Count = FMath::Min(Stops.Num(), MaxStops);

        FString Out = TEXT("(");
        for (int32 i = 0; i < Count; ++i)
        {
            if (!Stops[i].IsValid()) continue;
            const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
            if (!Stops[i]->TryGetObject(ObjPtr) || !ObjPtr || !(*ObjPtr).IsValid()) continue;
            const TSharedPtr<FJsonObject>& Obj = *ObjPtr;

            double Pos = 0.0;
            // Accept either "position" or "pos" for compact importer specs.
            if (!Obj->TryGetNumberField(TEXT("position"), Pos))
            {
                Obj->TryGetNumberField(TEXT("pos"), Pos);
            }

            FLinearColor Color(1.0f, 1.0f, 1.0f, 1.0f);
            FString ColorStr;
            if (Obj->TryGetStringField(TEXT("color"), ColorStr) && !ColorStr.IsEmpty())
            {
                MonolithUI::TryParseColor(ColorStr, Color);
            }

            if (i > 0) Out += TEXT(",");
            Out += FString::Printf(
                TEXT("(Position=%f,Color=(R=%f,G=%f,B=%f,A=%f))"),
                Pos, Color.R, Color.G, Color.B, Color.A);
        }
        Out += TEXT(")");
        return Out;
    }

    /**
     * Reflectively invoke a no-arg, no-return UFUNCTION on the widget by
     * name. Used for `ForceMIDRefresh` (promoted to UFUNCTION on the optional
     * EffectSurface provider so this codepath survives the decouple). Returns
     * true if the function was found + invoked.
     *
     * The `nullptr` ProcessEvent param works because the function takes no
     * arguments — UE 5.7 ProcessEvent reads from the supplied params
     * struct only when the function declares parameters.
     */
    static bool CallVoidNoArgUFunction(UWidget* Surface, FName FunctionName)
    {
        if (!Surface) return false;
        UFunction* Func = Surface->FindFunction(FunctionName);
        if (!Func) return false;
        Surface->ProcessEvent(Func, nullptr);
        return true;
    }

    /**
     * Reflectively invoke a single-UObject*-param UFUNCTION on the widget
     * by name. Used for `SetBaseMaterial(UMaterialInterface*)`. Builds the
     * params struct on the stack to match the UFunction's declared
     * parameter layout — UE 5.7 ProcessEvent expects a struct whose layout
     * matches the function's parameter UPROPERTYs.
     *
     * For SetBaseMaterial the function declares one parameter:
     *   ObjectProperty InMaterial (UMaterialInterface*)
     * which is exactly one pointer-sized field at offset 0. We pass a
     * trivial struct containing the pointer; this matches the UHT-emitted
     * `EffectSurface_eventSetBaseMaterial_Parms` layout (verified via
     * EffectSurface.gen.cpp:213).
     */
    static bool CallSetBaseMaterialReflectively(UWidget* Surface, UMaterialInterface* InMaterial)
    {
        if (!Surface) return false;
        UFunction* Func = Surface->FindFunction(FName(TEXT("SetBaseMaterial")));
        if (!Func) return false;

        struct FParms { UMaterialInterface* InMaterial; };
        FParms Parms{ InMaterial };
        Surface->ProcessEvent(Func, &Parms);
        return true;
    }

    /**
     * Apply post-mutation bookkeeping: mark structurally modified, optional
     * compile, ForceMIDRefresh so the live preview reflects the new look
     * without waiting on a paint pass.
     *
     * R2 (2026-04-26): the typed `Surface->ForceMIDRefresh()` becomes a
     * reflective ProcessEvent call. ForceMIDRefresh is a UFUNCTION on
     * UEffectSurface (promoted in this same wave); on the rare branch
     * where the function cannot be resolved (theoretically impossible if
     * the widget IsA EffectSurface) we proceed without the synchronous
     * push — the next OnPaint pass will pick up the dirty state.
     */
    static void FinalizeWriteAndPush(UWidgetBlueprint* WBP, UWidget* Surface, bool bCompile)
    {
        if (!Surface || !WBP) return;

        // SynchronizeProperties is protected on UWidget — ForceMIDRefresh
        // (Phase E test/diagnostic seam, R2-promoted to UFUNCTION) is the
        // public path: it pushes the current Config into the dynamic MID,
        // which is the only piece of SynchronizeProperties' work that
        // matters here. The widget tree itself doesn't need re-syncing
        // because we wrote directly into the template's Config UPROPERTY
        // via the reflection helper.
        FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
        CallVoidNoArgUFunction(Surface, FName(TEXT("ForceMIDRefresh")));

        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(WBP);
        }
    }

    /** Common success-result builder. */
    static FMonolithActionResult MakeSuccess(
        const FString& AssetPath,
        const FString& WidgetName,
        const TArray<FString>& PathsWritten,
        const TArray<FString>& Warnings,
        bool bCompiled)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("asset_path"), AssetPath);
        Out->SetStringField(TEXT("widget_name"), WidgetName);

        TArray<TSharedPtr<FJsonValue>> WrittenArr;
        for (const FString& P : PathsWritten)
        {
            WrittenArr.Add(MakeShared<FJsonValueString>(P));
        }
        Out->SetArrayField(TEXT("paths_written"), WrittenArr);

        if (Warnings.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> WarnArr;
            for (const FString& W : Warnings)
            {
                WarnArr.Add(MakeShared<FJsonValueString>(W));
            }
            Out->SetArrayField(TEXT("warnings"), WarnArr);
        }
        Out->SetBoolField(TEXT("compiled"), bCompiled);
        return FMonolithActionResult::Success(Out);
    }
} // namespace MonolithUI::EffectActionsInternal

// =============================================================================
// HANDLERS
// =============================================================================

using namespace MonolithUI::EffectActionsInternal;

// ----- 1. set_effect_surface_corners ------------------------------------------
FMonolithActionResult MonolithUI::FEffectSurfaceActions::HandleSetCorners(const TSharedPtr<FJsonObject>& Params)
{
    UWidgetBlueprint* WBP = nullptr;
    FMonolithActionResult Err;
    UWidget* Surface = ResolveEffectSurface(Params, WBP, Err);
    if (!Surface) return Err;

    const TArray<TSharedPtr<FJsonValue>>* RadiiArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("corner_radii"), RadiiArr) || !RadiiArr || RadiiArr->Num() < 4)
    {
        return FMonolithActionResult::Error(
            TEXT("set_effect_surface_corners: missing 'corner_radii' [TL,TR,BR,BL]"), -32602);
    }

    TArray<FString> PathsWritten;
    TArray<FString> Failures;

    // Compose the corner-radii write through the JSON path surface.
    TSharedPtr<FJsonObject> Vec4Obj = MakeShared<FJsonObject>();
    Vec4Obj->SetNumberField(TEXT("x"), (*RadiiArr)[0]->AsNumber());
    Vec4Obj->SetNumberField(TEXT("y"), (*RadiiArr)[1]->AsNumber());
    Vec4Obj->SetNumberField(TEXT("z"), (*RadiiArr)[2]->AsNumber());
    Vec4Obj->SetNumberField(TEXT("w"), (*RadiiArr)[3]->AsNumber());
    ApplyPath(Surface, TEXT("Effect.Shape.CornerRadii"),
        MakeShared<FJsonValueObject>(Vec4Obj), PathsWritten, Failures);

    if (TOptional<float> Smoothness = TryGetFloat(Params, TEXT("smoothness")); Smoothness.IsSet())
    {
        ApplyPath(Surface, TEXT("Effect.Shape.Smoothness"),
            MakeShared<FJsonValueNumber>(static_cast<double>(Smoothness.GetValue())),
            PathsWritten, Failures);
    }

    MergeFeatureFlagBits(Surface, EFFECT_FEATURE_ROUNDED_CORNERS, PathsWritten, Failures);

    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    FinalizeWriteAndPush(WBP, Surface, bCompile);

    if (PathsWritten.Num() == 0)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("set_effect_surface_corners: no paths written; failures=[%s]"),
                *FString::Join(Failures, TEXT("; "))), -32603);
    }

    return MakeSuccess(
        Params->GetStringField(TEXT("asset_path")),
        Params->GetStringField(TEXT("widget_name")),
        PathsWritten, Failures, bCompile);
}

// ----- 2. set_effect_surface_fill ---------------------------------------------
FMonolithActionResult MonolithUI::FEffectSurfaceActions::HandleSetFill(const TSharedPtr<FJsonObject>& Params)
{
    UWidgetBlueprint* WBP = nullptr;
    FMonolithActionResult Err;
    UWidget* Surface = ResolveEffectSurface(Params, WBP, Err);
    if (!Surface) return Err;

    FString ModeStr;
    if (!Params->TryGetStringField(TEXT("mode"), ModeStr) || ModeStr.IsEmpty())
    {
        return FMonolithActionResult::Error(
            TEXT("set_effect_surface_fill: missing 'mode' (one of: solid|linear|radial)"), -32602);
    }
    const FString ModeKey = ModeStr.ToLower();

    // Decide both the human-facing mode token (passed to the JSON path
    // surface as a string — FByteProperty(enum) accepts the enum-name
    // form per the helper's dispatch table) AND the FeatureFlags bit to
    // OR in. No EEffectFillMode/EEffectSurfaceFeature enum reference here.
    FString ModeToken;
    uint32  ModeBit = EFFECT_FEATURE_SOLID_FILL;
    if (ModeKey == TEXT("solid"))
    {
        ModeToken = TEXT("Solid");
        ModeBit   = EFFECT_FEATURE_SOLID_FILL;
    }
    else if (ModeKey == TEXT("linear"))
    {
        ModeToken = TEXT("Linear");
        ModeBit   = EFFECT_FEATURE_LINEAR_GRADIENT;
    }
    else if (ModeKey == TEXT("radial"))
    {
        ModeToken = TEXT("Radial");
        ModeBit   = EFFECT_FEATURE_RADIAL_GRADIENT;
    }
    else
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("set_effect_surface_fill: unknown mode '%s' (expected solid|linear|radial)"),
                *ModeStr), -32602);
    }

    TArray<FString> PathsWritten;
    TArray<FString> Failures;

    ApplyPath(Surface, TEXT("Effect.Fill.Mode"),
        MakeShared<FJsonValueString>(ModeToken), PathsWritten, Failures);

    FLinearColor SolidColor;
    if (TryGetColor(Params, TEXT("color"), SolidColor))
    {
        ApplyPath(Surface, TEXT("Effect.Fill.SolidColor"),
            MakeEffectColorJson(SolidColor), PathsWritten, Failures);
    }

    if (TOptional<float> Angle = TryGetFloat(Params, TEXT("angle")); Angle.IsSet())
    {
        ApplyPath(Surface, TEXT("Effect.Fill.Angle"),
            MakeShared<FJsonValueNumber>(static_cast<double>(Angle.GetValue())),
            PathsWritten, Failures);
    }

    FVector2D RadialCenter;
    if (TryGetVector2D(Params, TEXT("radial_center"), RadialCenter))
    {
        TSharedPtr<FJsonObject> CtrObj = MakeShared<FJsonObject>();
        CtrObj->SetNumberField(TEXT("x"), RadialCenter.X);
        CtrObj->SetNumberField(TEXT("y"), RadialCenter.Y);
        ApplyPath(Surface, TEXT("Effect.Fill.RadialCenter"),
            MakeShared<FJsonValueObject>(CtrObj), PathsWritten, Failures);
    }

    const TArray<TSharedPtr<FJsonValue>>* StopsArr = nullptr;
    if (Params->TryGetArrayField(TEXT("stops"), StopsArr) && StopsArr && StopsArr->Num() > 0)
    {
        // ImportText_Direct fallback path -- supply the UE text form directly.
        const FString StopsText = MakeStopArrayText(*StopsArr);
        ApplyPath(Surface, TEXT("Effect.Fill.Stops"),
            MakeShared<FJsonValueString>(StopsText), PathsWritten, Failures);
    }

    MergeFeatureFlagBits(Surface, ModeBit, PathsWritten, Failures);

    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    FinalizeWriteAndPush(WBP, Surface, bCompile);

    if (PathsWritten.Num() == 0)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("set_effect_surface_fill: no paths written; failures=[%s]"),
                *FString::Join(Failures, TEXT("; "))), -32603);
    }

    return MakeSuccess(
        Params->GetStringField(TEXT("asset_path")),
        Params->GetStringField(TEXT("widget_name")),
        PathsWritten, Failures, bCompile);
}

// ----- 3. set_effect_surface_border -------------------------------------------
FMonolithActionResult MonolithUI::FEffectSurfaceActions::HandleSetBorder(const TSharedPtr<FJsonObject>& Params)
{
    UWidgetBlueprint* WBP = nullptr;
    FMonolithActionResult Err;
    UWidget* Surface = ResolveEffectSurface(Params, WBP, Err);
    if (!Surface) return Err;

    TOptional<float> Width = TryGetFloat(Params, TEXT("width"));
    if (!Width.IsSet())
    {
        return FMonolithActionResult::Error(
            TEXT("set_effect_surface_border: missing 'width'"), -32602);
    }

    TArray<FString> PathsWritten;
    TArray<FString> Failures;

    ApplyPath(Surface, TEXT("Effect.Border.Width"),
        MakeShared<FJsonValueNumber>(static_cast<double>(Width.GetValue())),
        PathsWritten, Failures);

    FLinearColor Color;
    if (TryGetColor(Params, TEXT("color"), Color))
    {
        ApplyPath(Surface, TEXT("Effect.Border.Color"),
            MakeEffectColorJson(Color), PathsWritten, Failures);
    }
    if (TOptional<float> Offset = TryGetFloat(Params, TEXT("offset")); Offset.IsSet())
    {
        ApplyPath(Surface, TEXT("Effect.Border.Offset"),
            MakeShared<FJsonValueNumber>(static_cast<double>(Offset.GetValue())),
            PathsWritten, Failures);
    }
    if (TOptional<float> Glow = TryGetFloat(Params, TEXT("glow")); Glow.IsSet())
    {
        ApplyPath(Surface, TEXT("Effect.Border.Glow"),
            MakeShared<FJsonValueNumber>(static_cast<double>(Glow.GetValue())),
            PathsWritten, Failures);
    }
    FLinearColor GlowColor;
    if (TryGetColor(Params, TEXT("glow_color"), GlowColor))
    {
        ApplyPath(Surface, TEXT("Effect.Border.GlowColor"),
            MakeEffectColorJson(GlowColor), PathsWritten, Failures);
    }

    MergeFeatureFlagBits(Surface, EFFECT_FEATURE_BORDER, PathsWritten, Failures);

    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    FinalizeWriteAndPush(WBP, Surface, bCompile);

    if (PathsWritten.Num() == 0)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("set_effect_surface_border: no paths written; failures=[%s]"),
                *FString::Join(Failures, TEXT("; "))), -32603);
    }

    return MakeSuccess(
        Params->GetStringField(TEXT("asset_path")),
        Params->GetStringField(TEXT("widget_name")),
        PathsWritten, Failures, bCompile);
}

// ----- shared helper for the two shadow stack handlers ------------------------
namespace MonolithUI::EffectActionsInternal
{
    static FMonolithActionResult HandleShadowStack(
        const TSharedPtr<FJsonObject>& Params,
        const FString& JsonPath,            // "Effect.DropShadow" or "Effect.InnerShadow"
        uint32 FeatureBit,
        const FString& ActionName)
    {
        UWidgetBlueprint* WBP = nullptr;
        FMonolithActionResult Err;
        UWidget* Surface = ResolveEffectSurface(Params, WBP, Err);
        if (!Surface) return Err;

        const TArray<TSharedPtr<FJsonValue>>* LayersArr = nullptr;
        if (!Params->TryGetArrayField(TEXT("layers"), LayersArr) || !LayersArr)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("%s: missing 'layers' array"), *ActionName), -32602);
        }

        TArray<FString> PathsWritten;
        TArray<FString> Failures;

        const FString ShadowsText = MakeShadowArrayText(*LayersArr);
        ApplyPath(Surface, JsonPath,
            MakeShared<FJsonValueString>(ShadowsText), PathsWritten, Failures);

        // Only flip the bit if the stack is non-empty -- empty stack should NOT
        // pay the shader permutation cost.
        if (LayersArr->Num() > 0)
        {
            MergeFeatureFlagBits(Surface, FeatureBit, PathsWritten, Failures);
        }

        bool bCompile = false;
        Params->TryGetBoolField(TEXT("compile"), bCompile);
        FinalizeWriteAndPush(WBP, Surface, bCompile);

        if (PathsWritten.Num() == 0)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("%s: no paths written; failures=[%s]"),
                    *ActionName, *FString::Join(Failures, TEXT("; "))), -32603);
        }

        return MakeSuccess(
            Params->GetStringField(TEXT("asset_path")),
            Params->GetStringField(TEXT("widget_name")),
            PathsWritten, Failures, bCompile);
    }
}

// ----- 4. set_effect_surface_dropShadow ---------------------------------------
FMonolithActionResult MonolithUI::FEffectSurfaceActions::HandleSetDropShadow(const TSharedPtr<FJsonObject>& Params)
{
    return HandleShadowStack(Params,
        TEXT("Effect.DropShadow"),
        EFFECT_FEATURE_DROP_SHADOW,
        TEXT("set_effect_surface_dropShadow"));
}

// ----- 5. set_effect_surface_innerShadow --------------------------------------
FMonolithActionResult MonolithUI::FEffectSurfaceActions::HandleSetInnerShadow(const TSharedPtr<FJsonObject>& Params)
{
    return HandleShadowStack(Params,
        TEXT("Effect.InnerShadow"),
        EFFECT_FEATURE_INNER_SHADOW,
        TEXT("set_effect_surface_innerShadow"));
}

// ----- 6. set_effect_surface_glow ---------------------------------------------
FMonolithActionResult MonolithUI::FEffectSurfaceActions::HandleSetGlow(const TSharedPtr<FJsonObject>& Params)
{
    UWidgetBlueprint* WBP = nullptr;
    FMonolithActionResult Err;
    UWidget* Surface = ResolveEffectSurface(Params, WBP, Err);
    if (!Surface) return Err;

    TOptional<float> Radius = TryGetFloat(Params, TEXT("radius"));
    if (!Radius.IsSet())
    {
        return FMonolithActionResult::Error(
            TEXT("set_effect_surface_glow: missing 'radius'"), -32602);
    }

    TArray<FString> PathsWritten;
    TArray<FString> Failures;

    ApplyPath(Surface, TEXT("Effect.Glow.Radius"),
        MakeShared<FJsonValueNumber>(static_cast<double>(Radius.GetValue())),
        PathsWritten, Failures);

    FLinearColor Color;
    if (TryGetColor(Params, TEXT("color"), Color))
    {
        ApplyPath(Surface, TEXT("Effect.Glow.Color"),
            MakeEffectColorJson(Color), PathsWritten, Failures);
    }
    if (TOptional<float> Intensity = TryGetFloat(Params, TEXT("intensity")); Intensity.IsSet())
    {
        ApplyPath(Surface, TEXT("Effect.Glow.Intensity"),
            MakeShared<FJsonValueNumber>(static_cast<double>(Intensity.GetValue())),
            PathsWritten, Failures);
    }
    if (TOptional<float> Mix = TryGetFloat(Params, TEXT("inner_outer_mix")); Mix.IsSet())
    {
        ApplyPath(Surface, TEXT("Effect.Glow.InnerOuterMix"),
            MakeShared<FJsonValueNumber>(static_cast<double>(Mix.GetValue())),
            PathsWritten, Failures);
    }

    MergeFeatureFlagBits(Surface, EFFECT_FEATURE_GLOW, PathsWritten, Failures);

    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    FinalizeWriteAndPush(WBP, Surface, bCompile);

    if (PathsWritten.Num() == 0)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("set_effect_surface_glow: no paths written; failures=[%s]"),
                *FString::Join(Failures, TEXT("; "))), -32603);
    }

    return MakeSuccess(
        Params->GetStringField(TEXT("asset_path")),
        Params->GetStringField(TEXT("widget_name")),
        PathsWritten, Failures, bCompile);
}

// ----- 7. set_effect_surface_filter -------------------------------------------
FMonolithActionResult MonolithUI::FEffectSurfaceActions::HandleSetFilter(const TSharedPtr<FJsonObject>& Params)
{
    UWidgetBlueprint* WBP = nullptr;
    FMonolithActionResult Err;
    UWidget* Surface = ResolveEffectSurface(Params, WBP, Err);
    if (!Surface) return Err;

    TArray<FString> PathsWritten;
    TArray<FString> Failures;

    if (TOptional<float> Sat = TryGetFloat(Params, TEXT("saturation")); Sat.IsSet())
    {
        ApplyPath(Surface, TEXT("Effect.Filter.Saturation"),
            MakeShared<FJsonValueNumber>(static_cast<double>(Sat.GetValue())),
            PathsWritten, Failures);
    }
    if (TOptional<float> Bright = TryGetFloat(Params, TEXT("brightness")); Bright.IsSet())
    {
        ApplyPath(Surface, TEXT("Effect.Filter.Brightness"),
            MakeShared<FJsonValueNumber>(static_cast<double>(Bright.GetValue())),
            PathsWritten, Failures);
    }
    if (TOptional<float> Contrast = TryGetFloat(Params, TEXT("contrast")); Contrast.IsSet())
    {
        ApplyPath(Surface, TEXT("Effect.Filter.Contrast"),
            MakeShared<FJsonValueNumber>(static_cast<double>(Contrast.GetValue())),
            PathsWritten, Failures);
    }

    if (PathsWritten.Num() == 0)
    {
        return FMonolithActionResult::Error(
            TEXT("set_effect_surface_filter: at least one of saturation/brightness/contrast is required"),
            -32602);
    }

    MergeFeatureFlagBits(Surface, EFFECT_FEATURE_FILTER, PathsWritten, Failures);

    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    FinalizeWriteAndPush(WBP, Surface, bCompile);

    return MakeSuccess(
        Params->GetStringField(TEXT("asset_path")),
        Params->GetStringField(TEXT("widget_name")),
        PathsWritten, Failures, bCompile);
}

// ----- 8. set_effect_surface_backdropBlur -------------------------------------
FMonolithActionResult MonolithUI::FEffectSurfaceActions::HandleSetBackdropBlur(const TSharedPtr<FJsonObject>& Params)
{
    UWidgetBlueprint* WBP = nullptr;
    FMonolithActionResult Err;
    UWidget* Surface = ResolveEffectSurface(Params, WBP, Err);
    if (!Surface) return Err;

    TOptional<float> Strength = TryGetFloat(Params, TEXT("strength"));
    if (!Strength.IsSet())
    {
        return FMonolithActionResult::Error(
            TEXT("set_effect_surface_backdropBlur: missing 'strength' (0 disables)"), -32602);
    }

    TArray<FString> PathsWritten;
    TArray<FString> Failures;

    ApplyPath(Surface, TEXT("Effect.BackdropBlur.Strength"),
        MakeShared<FJsonValueNumber>(static_cast<double>(Strength.GetValue())),
        PathsWritten, Failures);

    // Only flip the BackdropBlur bit when strength > 0 -- zero strength is the
    // explicit "off" sentinel and should NOT carry the shader permutation cost.
    if (Strength.GetValue() > 0.0f)
    {
        MergeFeatureFlagBits(Surface, EFFECT_FEATURE_BACKDROP_BLUR, PathsWritten, Failures);
    }

    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    FinalizeWriteAndPush(WBP, Surface, bCompile);

    if (PathsWritten.Num() == 0)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("set_effect_surface_backdropBlur: no paths written; failures=[%s]"),
                *FString::Join(Failures, TEXT("; "))), -32603);
    }

    return MakeSuccess(
        Params->GetStringField(TEXT("asset_path")),
        Params->GetStringField(TEXT("widget_name")),
        PathsWritten, Failures, bCompile);
}

// ----- 9. set_effect_surface_insetHighlight -----------------------------------
FMonolithActionResult MonolithUI::FEffectSurfaceActions::HandleSetInsetHighlight(const TSharedPtr<FJsonObject>& Params)
{
    UWidgetBlueprint* WBP = nullptr;
    FMonolithActionResult Err;
    UWidget* Surface = ResolveEffectSurface(Params, WBP, Err);
    if (!Surface) return Err;

    TArray<FString> PathsWritten;
    TArray<FString> Failures;

    FVector2D Offset;
    if (TryGetVector2D(Params, TEXT("offset"), Offset))
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), Offset.X);
        Obj->SetNumberField(TEXT("y"), Offset.Y);
        ApplyPath(Surface, TEXT("Effect.InsetHighlight.Offset"),
            MakeShared<FJsonValueObject>(Obj), PathsWritten, Failures);
    }
    if (TOptional<float> Blur = TryGetFloat(Params, TEXT("blur")); Blur.IsSet())
    {
        ApplyPath(Surface, TEXT("Effect.InsetHighlight.Blur"),
            MakeShared<FJsonValueNumber>(static_cast<double>(Blur.GetValue())),
            PathsWritten, Failures);
    }
    FLinearColor Color;
    if (TryGetColor(Params, TEXT("color"), Color))
    {
        ApplyPath(Surface, TEXT("Effect.InsetHighlight.Color"),
            MakeEffectColorJson(Color), PathsWritten, Failures);
    }
    if (TOptional<float> Intensity = TryGetFloat(Params, TEXT("intensity")); Intensity.IsSet())
    {
        ApplyPath(Surface, TEXT("Effect.InsetHighlight.Intensity"),
            MakeShared<FJsonValueNumber>(static_cast<double>(Intensity.GetValue())),
            PathsWritten, Failures);
    }
    int32 EdgeMask = 0;
    if (Params->TryGetNumberField(TEXT("edge_mask"), EdgeMask))
    {
        ApplyPath(Surface, TEXT("Effect.InsetHighlight.EdgeMask"),
            MakeShared<FJsonValueNumber>(static_cast<double>(EdgeMask)),
            PathsWritten, Failures);
    }

    MergeFeatureFlagBits(Surface, EFFECT_FEATURE_INSET_HIGHLIGHT, PathsWritten, Failures);

    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    FinalizeWriteAndPush(WBP, Surface, bCompile);

    if (PathsWritten.Num() == 0)
    {
        return FMonolithActionResult::Error(
            TEXT("set_effect_surface_insetHighlight: at least one of offset/blur/color/intensity/edge_mask is required"),
            -32602);
    }

    return MakeSuccess(
        Params->GetStringField(TEXT("asset_path")),
        Params->GetStringField(TEXT("widget_name")),
        PathsWritten, Failures, bCompile);
}

// ----- 10. apply_effect_surface_preset ----------------------------------------
//
// R2 (2026-04-26): the preset path used to consume
// `MonolithUI::EffectSurfacePresets::MakeConfigForPreset` (header in
// the provider preset helper, returning a config struct by value). The
// decouple plan picks trade-off (a):
// inline the preset definitions as JSON literals here so this file holds
// NO provider header includes (Wave 3 Final removes the
// PrivateDependencyModuleNames entry; the preset header would become
// unreachable).
//
// Drift-detection: any change to the provider preset helper
// values MUST be mirrored here (and vice-versa). The Phase E
// EffectSurfaceTests.cpp suite asserts the post-preset Config struct
// matches a known-good snapshot — that is the canonical regression
// gate. SPEC §5 lists this drift-detection contract.
//
// Each preset below builds a `TSharedPtr<FJsonObject> PresetSpec` whose
// schema mirrors the per-bag setter inputs; the handler walks the spec
// and calls `ApplyPath` per leaf, exactly the same way the per-bag
// setters do. FeatureFlags is written verbatim from the preset (NOT
// OR-merged) — the preset is authoritative for the active feature set.
//
// Caller may optionally supply `parent_material` (asset path string) -- when
// present we call SetBaseMaterial BEFORE the sub-bag writes so the MID is
// re-created against the new parent and the post-batch ForceMIDRefresh
// pushes every preset parameter onto a fresh MID.
namespace MonolithUI::EffectActionsInternal
{
    /**
     * Build the canonical preset spec for a known preset name. Returns
     * nullptr (and populates OutValidNames) when the name is not
     * recognised. Spec shape per preset:
     *
     *   {
     *     "shape":          { "corner_radii":[x,y,z,w], "smoothness":1.0 },
     *     "fill":           { "mode":"Solid|Linear|Radial",
     *                         "color":"#RRGGBBAA",
     *                         "angle":90.0,
     *                         "stops":[{position,color},...] },
     *     "border":         { "width":1.0, "color":"#…", "glow":12.0,
     *                         "glow_color":"#…" },
     *     "drop_shadow":    [{ offset, blur, spread, color }, ...],
     *     "inner_shadow":   [{ offset, blur, spread, color }, ...],
     *     "glow":           { "radius":16.0, "color":"#…", "intensity":1.5,
     *                         "inner_outer_mix":0.0 },
     *     "filter":         { "saturation":1, "brightness":1, "contrast":1 },
     *     "backdrop_blur":  { "strength":24.0 },
     *     "inset_highlight":{ "offset":[x,y], "blur":1.0, "color":"#…",
     *                         "intensity":1.0, "edge_mask":1 },
     *     "feature_flags":  bitmask integer
     *   }
     *
     * Optional sub-objects are simply absent — the consumer skips them.
     */
    static TSharedPtr<FJsonObject> MakePresetSpec(
        const FString& PresetName,
        TArray<FString>& OutValidNames)
    {
        OutValidNames = {
            TEXT("rounded-rect"),
            TEXT("pill"),
            TEXT("circle"),
            TEXT("glass"),
            TEXT("glowing-button"),
            TEXT("neon"),
        };

        const FString Key = PresetName.ToLower();

        // Helper lambdas to keep the preset literals compact.
        auto MakeVec4 = [](double X, double Y, double Z, double W)
        {
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetNumberField(TEXT("x"), X);
            O->SetNumberField(TEXT("y"), Y);
            O->SetNumberField(TEXT("z"), Z);
            O->SetNumberField(TEXT("w"), W);
            return O;
        };
        auto MakeShadow = [](double OX, double OY, double Blur, double Spread,
                             const FString& ColorHex)
        {
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            TArray<TSharedPtr<FJsonValue>> OffArr;
            OffArr.Add(MakeShared<FJsonValueNumber>(OX));
            OffArr.Add(MakeShared<FJsonValueNumber>(OY));
            O->SetArrayField(TEXT("offset"), OffArr);
            O->SetNumberField(TEXT("blur"), Blur);
            O->SetNumberField(TEXT("spread"), Spread);
            O->SetStringField(TEXT("color"), ColorHex);
            return O;
        };
        auto MakeStop = [](double Pos, const FString& ColorHex)
        {
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetNumberField(TEXT("position"), Pos);
            O->SetStringField(TEXT("color"), ColorHex);
            return O;
        };

        TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();

        if (Key == TEXT("rounded-rect"))
        {
            // Shape: 12px corners, smoothness 1.0
            TSharedPtr<FJsonObject> Shape = MakeShared<FJsonObject>();
            Shape->SetObjectField(TEXT("corner_radii"), MakeVec4(12.0, 12.0, 12.0, 12.0));
            Shape->SetNumberField(TEXT("smoothness"), 1.0);
            Spec->SetObjectField(TEXT("shape"), Shape);

            // Fill: solid white
            TSharedPtr<FJsonObject> Fill = MakeShared<FJsonObject>();
            Fill->SetStringField(TEXT("mode"), TEXT("Solid"));
            // FLinearColor::White -> #FFFFFFFF
            Fill->SetStringField(TEXT("color"), TEXT("#FFFFFFFF"));
            Fill->SetNumberField(TEXT("angle"), 0.0);
            Spec->SetObjectField(TEXT("fill"), Fill);

            // Drop shadow: one layer 0,4 blur 12 spread 0 color rgba(0,0,0,0.18)
            TArray<TSharedPtr<FJsonValue>> DS;
            DS.Add(MakeShared<FJsonValueObject>(MakeShadow(0.0, 4.0, 12.0, 0.0, TEXT("#0000002E"))));
            Spec->SetArrayField(TEXT("drop_shadow"), DS);

            // FeatureFlags: RoundedCorners | SolidFill | DropShadow = 1|2|32 = 35
            Spec->SetNumberField(TEXT("feature_flags"),
                static_cast<double>(EFFECT_FEATURE_ROUNDED_CORNERS
                    | EFFECT_FEATURE_SOLID_FILL
                    | EFFECT_FEATURE_DROP_SHADOW));
            return Spec;
        }

        if (Key == TEXT("pill"))
        {
            TSharedPtr<FJsonObject> Shape = MakeShared<FJsonObject>();
            Shape->SetObjectField(TEXT("corner_radii"), MakeVec4(999.0, 999.0, 999.0, 999.0));
            Shape->SetNumberField(TEXT("smoothness"), 0.0);
            Spec->SetObjectField(TEXT("shape"), Shape);

            TSharedPtr<FJsonObject> Fill = MakeShared<FJsonObject>();
            Fill->SetStringField(TEXT("mode"), TEXT("Solid"));
            // FLinearColor(0.95, 0.95, 0.95, 1.0)
            Fill->SetStringField(TEXT("color"), TEXT("#F2F2F2FF"));
            Fill->SetNumberField(TEXT("angle"), 0.0);
            Spec->SetObjectField(TEXT("fill"), Fill);

            Spec->SetNumberField(TEXT("feature_flags"),
                static_cast<double>(EFFECT_FEATURE_ROUNDED_CORNERS
                    | EFFECT_FEATURE_SOLID_FILL));
            return Spec;
        }

        if (Key == TEXT("circle"))
        {
            TSharedPtr<FJsonObject> Shape = MakeShared<FJsonObject>();
            Shape->SetObjectField(TEXT("corner_radii"), MakeVec4(999.0, 999.0, 999.0, 999.0));
            Shape->SetNumberField(TEXT("smoothness"), 0.0);
            Spec->SetObjectField(TEXT("shape"), Shape);

            TSharedPtr<FJsonObject> Fill = MakeShared<FJsonObject>();
            Fill->SetStringField(TEXT("mode"), TEXT("Solid"));
            Fill->SetStringField(TEXT("color"), TEXT("#FFFFFFFF"));
            Fill->SetNumberField(TEXT("angle"), 0.0);
            Spec->SetObjectField(TEXT("fill"), Fill);

            Spec->SetNumberField(TEXT("feature_flags"),
                static_cast<double>(EFFECT_FEATURE_ROUNDED_CORNERS
                    | EFFECT_FEATURE_SOLID_FILL));
            return Spec;
        }

        if (Key == TEXT("glass"))
        {
            TSharedPtr<FJsonObject> Shape = MakeShared<FJsonObject>();
            Shape->SetObjectField(TEXT("corner_radii"), MakeVec4(16.0, 16.0, 16.0, 16.0));
            Shape->SetNumberField(TEXT("smoothness"), 0.0);
            Spec->SetObjectField(TEXT("shape"), Shape);

            TSharedPtr<FJsonObject> Fill = MakeShared<FJsonObject>();
            Fill->SetStringField(TEXT("mode"), TEXT("Solid"));
            // FLinearColor(1, 1, 1, 0.10) → 0xFFFFFF1A (0.10 * 255 ≈ 26 = 0x1A)
            Fill->SetStringField(TEXT("color"), TEXT("#FFFFFF1A"));
            Fill->SetNumberField(TEXT("angle"), 0.0);
            Spec->SetObjectField(TEXT("fill"), Fill);

            TSharedPtr<FJsonObject> Border = MakeShared<FJsonObject>();
            Border->SetNumberField(TEXT("width"), 1.0);
            // FLinearColor(1, 1, 1, 0.25) → 0xFFFFFF40 (0.25 * 255 ≈ 64 = 0x40)
            Border->SetStringField(TEXT("color"), TEXT("#FFFFFF40"));
            Spec->SetObjectField(TEXT("border"), Border);

            TSharedPtr<FJsonObject> Backdrop = MakeShared<FJsonObject>();
            Backdrop->SetNumberField(TEXT("strength"), 24.0);
            Spec->SetObjectField(TEXT("backdrop_blur"), Backdrop);

            TSharedPtr<FJsonObject> Inset = MakeShared<FJsonObject>();
            TArray<TSharedPtr<FJsonValue>> InOff;
            InOff.Add(MakeShared<FJsonValueNumber>(0.0));
            InOff.Add(MakeShared<FJsonValueNumber>(1.0));
            Inset->SetArrayField(TEXT("offset"), InOff);
            Inset->SetNumberField(TEXT("blur"), 1.0);
            // FLinearColor(1, 1, 1, 0.6) → 0xFFFFFF99 (0.6 * 255 ≈ 153 = 0x99)
            Inset->SetStringField(TEXT("color"), TEXT("#FFFFFF99"));
            Inset->SetNumberField(TEXT("intensity"), 1.0);
            Inset->SetNumberField(TEXT("edge_mask"), 1.0); // EEffectInsetEdge::Top = 1
            Spec->SetObjectField(TEXT("inset_highlight"), Inset);

            Spec->SetNumberField(TEXT("feature_flags"),
                static_cast<double>(EFFECT_FEATURE_ROUNDED_CORNERS
                    | EFFECT_FEATURE_SOLID_FILL
                    | EFFECT_FEATURE_BORDER
                    | EFFECT_FEATURE_BACKDROP_BLUR
                    | EFFECT_FEATURE_INSET_HIGHLIGHT));
            return Spec;
        }

        if (Key == TEXT("glowing-button"))
        {
            TSharedPtr<FJsonObject> Shape = MakeShared<FJsonObject>();
            Shape->SetObjectField(TEXT("corner_radii"), MakeVec4(8.0, 8.0, 8.0, 8.0));
            Shape->SetNumberField(TEXT("smoothness"), 0.0);
            Spec->SetObjectField(TEXT("shape"), Shape);

            TSharedPtr<FJsonObject> Fill = MakeShared<FJsonObject>();
            Fill->SetStringField(TEXT("mode"), TEXT("Linear"));
            Fill->SetNumberField(TEXT("angle"), 90.0);
            // SolidColor unused for Linear, but the per-bag setter writes it
            // unconditionally — preserve byte-identity by writing white default.
            Fill->SetStringField(TEXT("color"), TEXT("#FFFFFFFF"));
            // Stops: (0.0, rgba(0.30,0.55,1.00,1.0)) → 0x4D8CFFFF;
            //        (1.0, rgba(0.10,0.30,0.85,1.0)) → 0x1A4DD9FF
            TArray<TSharedPtr<FJsonValue>> Stops;
            Stops.Add(MakeShared<FJsonValueObject>(MakeStop(0.0, TEXT("#4D8CFFFF"))));
            Stops.Add(MakeShared<FJsonValueObject>(MakeStop(1.0, TEXT("#1A4DD9FF"))));
            Fill->SetArrayField(TEXT("stops"), Stops);
            Spec->SetObjectField(TEXT("fill"), Fill);

            TSharedPtr<FJsonObject> Glow = MakeShared<FJsonObject>();
            Glow->SetNumberField(TEXT("radius"), 16.0);
            // FLinearColor(0.40, 0.65, 1.00, 1.0) → 0x66A6FFFF
            Glow->SetStringField(TEXT("color"), TEXT("#66A6FFFF"));
            Glow->SetNumberField(TEXT("intensity"), 1.5);
            Glow->SetNumberField(TEXT("inner_outer_mix"), 0.0);
            Spec->SetObjectField(TEXT("glow"), Glow);

            // Drop shadow: one layer 0,2 blur 8 spread 0 rgba(0,0,0,0.30) → 0x0000004D
            TArray<TSharedPtr<FJsonValue>> DS;
            DS.Add(MakeShared<FJsonValueObject>(MakeShadow(0.0, 2.0, 8.0, 0.0, TEXT("#0000004D"))));
            Spec->SetArrayField(TEXT("drop_shadow"), DS);

            Spec->SetNumberField(TEXT("feature_flags"),
                static_cast<double>(EFFECT_FEATURE_ROUNDED_CORNERS
                    | EFFECT_FEATURE_LINEAR_GRADIENT
                    | EFFECT_FEATURE_GLOW
                    | EFFECT_FEATURE_DROP_SHADOW));
            return Spec;
        }

        if (Key == TEXT("neon"))
        {
            TSharedPtr<FJsonObject> Shape = MakeShared<FJsonObject>();
            Shape->SetObjectField(TEXT("corner_radii"), MakeVec4(4.0, 4.0, 4.0, 4.0));
            Shape->SetNumberField(TEXT("smoothness"), 0.0);
            Spec->SetObjectField(TEXT("shape"), Shape);

            TSharedPtr<FJsonObject> Fill = MakeShared<FJsonObject>();
            Fill->SetStringField(TEXT("mode"), TEXT("Solid"));
            // FLinearColor::Transparent → 0x00000000
            Fill->SetStringField(TEXT("color"), TEXT("#00000000"));
            Fill->SetNumberField(TEXT("angle"), 0.0);
            Spec->SetObjectField(TEXT("fill"), Fill);

            TSharedPtr<FJsonObject> Border = MakeShared<FJsonObject>();
            Border->SetNumberField(TEXT("width"), 2.0);
            // FLinearColor(1.00, 0.20, 0.85, 1.0) → 0xFF33D9FF
            Border->SetStringField(TEXT("color"), TEXT("#FF33D9FF"));
            Border->SetNumberField(TEXT("glow"), 12.0);
            // FLinearColor(1.00, 0.20, 0.85, 0.85) → 0xFF33D9D9
            Border->SetStringField(TEXT("glow_color"), TEXT("#FF33D9D9"));
            Spec->SetObjectField(TEXT("border"), Border);

            Spec->SetNumberField(TEXT("feature_flags"),
                static_cast<double>(EFFECT_FEATURE_ROUNDED_CORNERS
                    | EFFECT_FEATURE_BORDER));
            return Spec;
        }

        return nullptr;
    }
} // namespace MonolithUI::EffectActionsInternal

FMonolithActionResult MonolithUI::FEffectSurfaceActions::HandleApplyPreset(const TSharedPtr<FJsonObject>& Params)
{
    UWidgetBlueprint* WBP = nullptr;
    FMonolithActionResult Err;
    UWidget* Surface = ResolveEffectSurface(Params, WBP, Err);
    if (!Surface) return Err;

    FString PresetName;
    if (!Params->TryGetStringField(TEXT("preset_name"), PresetName) || PresetName.IsEmpty())
    {
        return FMonolithActionResult::Error(
            TEXT("apply_effect_surface_preset: missing 'preset_name'"), -32602);
    }

    TArray<FString> ValidNames;
    TSharedPtr<FJsonObject> PresetSpec = MakePresetSpec(PresetName, ValidNames);
    if (!PresetSpec.IsValid())
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("apply_effect_surface_preset: unknown preset '%s' (valid: %s)"),
                *PresetName, *FString::Join(ValidNames, TEXT(", "))), -32602);
    }

    // Optional parent material swap FIRST -- SetBaseMaterial recreates the MID,
    // so any sub-bag writes that follow get pushed onto the correct parent.
    // Reflective ProcessEvent path (R2 — no typed UEffectSurface symbol here).
    FString ParentMaterialPath;
    if (Params->TryGetStringField(TEXT("parent_material"), ParentMaterialPath)
        && !ParentMaterialPath.IsEmpty())
    {
        UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *ParentMaterialPath);
        if (Parent)
        {
            CallSetBaseMaterialReflectively(Surface, Parent);
        }
        // Silent on failure -- the preset still applies; caller can poll the MID
        // via the Phase E test seam if they care about parent verification.
    }

    TArray<FString> PathsWritten;
    TArray<FString> Failures;

    // Walk the preset spec, calling ApplyPath per leaf. Schema mirrors the
    // per-bag setter inputs (see MakePresetSpec docstring above).

    // Shape
    if (const TSharedPtr<FJsonObject>* ShapeObj = nullptr; PresetSpec->TryGetObjectField(TEXT("shape"), ShapeObj) && ShapeObj && (*ShapeObj).IsValid())
    {
        const TSharedPtr<FJsonObject>& Shape = *ShapeObj;
        if (const TSharedPtr<FJsonObject>* RadiiObj = nullptr; Shape->TryGetObjectField(TEXT("corner_radii"), RadiiObj) && RadiiObj && (*RadiiObj).IsValid())
        {
            ApplyPath(Surface, TEXT("Effect.Shape.CornerRadii"),
                MakeShared<FJsonValueObject>(*RadiiObj), PathsWritten, Failures);
        }
        double Smoothness = 0.0;
        if (Shape->TryGetNumberField(TEXT("smoothness"), Smoothness))
        {
            ApplyPath(Surface, TEXT("Effect.Shape.Smoothness"),
                MakeShared<FJsonValueNumber>(Smoothness), PathsWritten, Failures);
        }
    }

    // Fill
    if (const TSharedPtr<FJsonObject>* FillObj = nullptr; PresetSpec->TryGetObjectField(TEXT("fill"), FillObj) && FillObj && (*FillObj).IsValid())
    {
        const TSharedPtr<FJsonObject>& Fill = *FillObj;

        FString ModeStr;
        if (Fill->TryGetStringField(TEXT("mode"), ModeStr))
        {
            ApplyPath(Surface, TEXT("Effect.Fill.Mode"),
                MakeShared<FJsonValueString>(ModeStr), PathsWritten, Failures);
        }
        FString ColorStr;
        if (Fill->TryGetStringField(TEXT("color"), ColorStr))
        {
            ApplyPath(Surface, TEXT("Effect.Fill.SolidColor"),
                MakeEffectColorJsonFromText(ColorStr), PathsWritten, Failures);
        }
        double Angle = 0.0;
        if (Fill->TryGetNumberField(TEXT("angle"), Angle))
        {
            ApplyPath(Surface, TEXT("Effect.Fill.Angle"),
                MakeShared<FJsonValueNumber>(Angle), PathsWritten, Failures);
        }

        const TArray<TSharedPtr<FJsonValue>>* StopsArr = nullptr;
        if (Fill->TryGetArrayField(TEXT("stops"), StopsArr) && StopsArr && StopsArr->Num() > 0)
        {
            const FString StopsText = MakeStopArrayText(*StopsArr);
            ApplyPath(Surface, TEXT("Effect.Fill.Stops"),
                MakeShared<FJsonValueString>(StopsText), PathsWritten, Failures);
        }
    }

    // Border
    if (const TSharedPtr<FJsonObject>* BorderObj = nullptr; PresetSpec->TryGetObjectField(TEXT("border"), BorderObj) && BorderObj && (*BorderObj).IsValid())
    {
        const TSharedPtr<FJsonObject>& Border = *BorderObj;
        double Width = 0.0;
        if (Border->TryGetNumberField(TEXT("width"), Width))
        {
            ApplyPath(Surface, TEXT("Effect.Border.Width"),
                MakeShared<FJsonValueNumber>(Width), PathsWritten, Failures);
        }
        FString BC;
        if (Border->TryGetStringField(TEXT("color"), BC))
        {
            ApplyPath(Surface, TEXT("Effect.Border.Color"),
                MakeEffectColorJsonFromText(BC), PathsWritten, Failures);
        }
        double BG = 0.0;
        if (Border->TryGetNumberField(TEXT("glow"), BG))
        {
            ApplyPath(Surface, TEXT("Effect.Border.Glow"),
                MakeShared<FJsonValueNumber>(BG), PathsWritten, Failures);
        }
        FString BGC;
        if (Border->TryGetStringField(TEXT("glow_color"), BGC))
        {
            ApplyPath(Surface, TEXT("Effect.Border.GlowColor"),
                MakeEffectColorJsonFromText(BGC), PathsWritten, Failures);
        }
    }

    // Drop shadow
    {
        const TArray<TSharedPtr<FJsonValue>>* DSArr = nullptr;
        if (PresetSpec->TryGetArrayField(TEXT("drop_shadow"), DSArr) && DSArr && DSArr->Num() > 0)
        {
            const FString DSText = MakeShadowArrayText(*DSArr);
            ApplyPath(Surface, TEXT("Effect.DropShadow"),
                MakeShared<FJsonValueString>(DSText), PathsWritten, Failures);
        }
    }
    // Inner shadow
    {
        const TArray<TSharedPtr<FJsonValue>>* ISArr = nullptr;
        if (PresetSpec->TryGetArrayField(TEXT("inner_shadow"), ISArr) && ISArr && ISArr->Num() > 0)
        {
            const FString ISText = MakeShadowArrayText(*ISArr);
            ApplyPath(Surface, TEXT("Effect.InnerShadow"),
                MakeShared<FJsonValueString>(ISText), PathsWritten, Failures);
        }
    }

    // Glow
    if (const TSharedPtr<FJsonObject>* GlowObj = nullptr; PresetSpec->TryGetObjectField(TEXT("glow"), GlowObj) && GlowObj && (*GlowObj).IsValid())
    {
        const TSharedPtr<FJsonObject>& Glow = *GlowObj;
        double GR = 0.0;
        if (Glow->TryGetNumberField(TEXT("radius"), GR))
        {
            ApplyPath(Surface, TEXT("Effect.Glow.Radius"),
                MakeShared<FJsonValueNumber>(GR), PathsWritten, Failures);
        }
        FString GC;
        if (Glow->TryGetStringField(TEXT("color"), GC))
        {
            ApplyPath(Surface, TEXT("Effect.Glow.Color"),
                MakeEffectColorJsonFromText(GC), PathsWritten, Failures);
        }
        double GI = 0.0;
        if (Glow->TryGetNumberField(TEXT("intensity"), GI))
        {
            ApplyPath(Surface, TEXT("Effect.Glow.Intensity"),
                MakeShared<FJsonValueNumber>(GI), PathsWritten, Failures);
        }
        double GM = 0.0;
        if (Glow->TryGetNumberField(TEXT("inner_outer_mix"), GM))
        {
            ApplyPath(Surface, TEXT("Effect.Glow.InnerOuterMix"),
                MakeShared<FJsonValueNumber>(GM), PathsWritten, Failures);
        }
    }

    // Filter
    if (const TSharedPtr<FJsonObject>* FilterObj = nullptr; PresetSpec->TryGetObjectField(TEXT("filter"), FilterObj) && FilterObj && (*FilterObj).IsValid())
    {
        const TSharedPtr<FJsonObject>& Filter = *FilterObj;
        double Sat = 0.0;
        if (Filter->TryGetNumberField(TEXT("saturation"), Sat))
        {
            ApplyPath(Surface, TEXT("Effect.Filter.Saturation"),
                MakeShared<FJsonValueNumber>(Sat), PathsWritten, Failures);
        }
        double Bright = 0.0;
        if (Filter->TryGetNumberField(TEXT("brightness"), Bright))
        {
            ApplyPath(Surface, TEXT("Effect.Filter.Brightness"),
                MakeShared<FJsonValueNumber>(Bright), PathsWritten, Failures);
        }
        double Contrast = 0.0;
        if (Filter->TryGetNumberField(TEXT("contrast"), Contrast))
        {
            ApplyPath(Surface, TEXT("Effect.Filter.Contrast"),
                MakeShared<FJsonValueNumber>(Contrast), PathsWritten, Failures);
        }
    }

    // Backdrop blur
    if (const TSharedPtr<FJsonObject>* BBObj = nullptr; PresetSpec->TryGetObjectField(TEXT("backdrop_blur"), BBObj) && BBObj && (*BBObj).IsValid())
    {
        const TSharedPtr<FJsonObject>& BB = *BBObj;
        double Strength = 0.0;
        if (BB->TryGetNumberField(TEXT("strength"), Strength))
        {
            ApplyPath(Surface, TEXT("Effect.BackdropBlur.Strength"),
                MakeShared<FJsonValueNumber>(Strength), PathsWritten, Failures);
        }
    }

    // Inset highlight
    if (const TSharedPtr<FJsonObject>* IHObj = nullptr; PresetSpec->TryGetObjectField(TEXT("inset_highlight"), IHObj) && IHObj && (*IHObj).IsValid())
    {
        const TSharedPtr<FJsonObject>& IH = *IHObj;

        const TArray<TSharedPtr<FJsonValue>>* OffArr = nullptr;
        if (IH->TryGetArrayField(TEXT("offset"), OffArr) && OffArr && OffArr->Num() >= 2)
        {
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetNumberField(TEXT("x"), (*OffArr)[0]->AsNumber());
            O->SetNumberField(TEXT("y"), (*OffArr)[1]->AsNumber());
            ApplyPath(Surface, TEXT("Effect.InsetHighlight.Offset"),
                MakeShared<FJsonValueObject>(O), PathsWritten, Failures);
        }
        double IBlur = 0.0;
        if (IH->TryGetNumberField(TEXT("blur"), IBlur))
        {
            ApplyPath(Surface, TEXT("Effect.InsetHighlight.Blur"),
                MakeShared<FJsonValueNumber>(IBlur), PathsWritten, Failures);
        }
        FString IC;
        if (IH->TryGetStringField(TEXT("color"), IC))
        {
            ApplyPath(Surface, TEXT("Effect.InsetHighlight.Color"),
                MakeShared<FJsonValueString>(IC), PathsWritten, Failures);
        }
        double II = 0.0;
        if (IH->TryGetNumberField(TEXT("intensity"), II))
        {
            ApplyPath(Surface, TEXT("Effect.InsetHighlight.Intensity"),
                MakeShared<FJsonValueNumber>(II), PathsWritten, Failures);
        }
        double IEdge = 0.0;
        if (IH->TryGetNumberField(TEXT("edge_mask"), IEdge))
        {
            ApplyPath(Surface, TEXT("Effect.InsetHighlight.EdgeMask"),
                MakeShared<FJsonValueNumber>(IEdge), PathsWritten, Failures);
        }
    }

    // FeatureFlags -- preset already calculated the canonical bitmask. We
    // overwrite (NOT OR-merge) so the preset is authoritative.
    double FlagsD = 0.0;
    if (PresetSpec->TryGetNumberField(TEXT("feature_flags"), FlagsD))
    {
        ApplyPath(Surface, TEXT("Effect.FeatureFlags"),
            MakeShared<FJsonValueNumber>(FlagsD), PathsWritten, Failures);
    }

    bool bCompile = false;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    FinalizeWriteAndPush(WBP, Surface, bCompile);

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("asset_path"), Params->GetStringField(TEXT("asset_path")));
    Out->SetStringField(TEXT("widget_name"), Params->GetStringField(TEXT("widget_name")));
    Out->SetStringField(TEXT("preset_name"), PresetName);
    Out->SetNumberField(TEXT("paths_written_count"), PathsWritten.Num());
    Out->SetBoolField(TEXT("compiled"), bCompile);
    if (Failures.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> WarnArr;
        for (const FString& W : Failures) WarnArr.Add(MakeShared<FJsonValueString>(W));
        Out->SetArrayField(TEXT("warnings"), WarnArr);
    }
    return FMonolithActionResult::Success(Out);
}

// =============================================================================
// REGISTRATION
// =============================================================================

void MonolithUI::FEffectSurfaceActions::Register(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_effect_surface_corners"),
        TEXT("Set EffectSurface corner radii (and optional smoothness). Composes against allowlist-gated "
             "JSON paths Effect.Shape.CornerRadii / Effect.Shape.Smoothness; ORs RoundedCorners into "
             "Effect.FeatureFlags. Params: asset_path, widget_name, corner_radii=[TL,TR,BR,BL], "
             "smoothness?, compile?."),
        FMonolithActionHandler::CreateStatic(&FEffectSurfaceActions::HandleSetCorners));

    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_effect_surface_fill"),
        TEXT("Set EffectSurface fill: solid colour, linear gradient, or radial gradient. ORs the matching "
             "fill-mode bit into Effect.FeatureFlags. Params: asset_path, widget_name, mode (solid|linear|"
             "radial), color?, stops? (array of {position,color}), angle?, radial_center? ([x,y] or "
             "{x,y}), compile?."),
        FMonolithActionHandler::CreateStatic(&FEffectSurfaceActions::HandleSetFill));

    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_effect_surface_border"),
        TEXT("Set EffectSurface border (width + colour + offset + optional glow halo). ORs Border into "
             "Effect.FeatureFlags. Params: asset_path, widget_name, width, color?, offset?, glow?, "
             "glow_color?, compile?."),
        FMonolithActionHandler::CreateStatic(&FEffectSurfaceActions::HandleSetBorder));

    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_effect_surface_dropShadow"),
        TEXT("Set EffectSurface drop-shadow stack (CSS-style layered list, capped at 4). ORs DropShadow "
             "into Effect.FeatureFlags only when the layer list is non-empty. Params: asset_path, "
             "widget_name, layers=[{offset:[x,y], blur, spread, color}, ...], compile?."),
        FMonolithActionHandler::CreateStatic(&FEffectSurfaceActions::HandleSetDropShadow));

    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_effect_surface_innerShadow"),
        TEXT("Set EffectSurface inner-shadow stack (CSS-style layered list, capped at 4). ORs InnerShadow "
             "into Effect.FeatureFlags only when the layer list is non-empty. Params: asset_path, "
             "widget_name, layers=[{offset:[x,y], blur, spread, color}, ...], compile?."),
        FMonolithActionHandler::CreateStatic(&FEffectSurfaceActions::HandleSetInnerShadow));

    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_effect_surface_glow"),
        TEXT("Set EffectSurface standalone glow halo. ORs Glow into Effect.FeatureFlags. Params: "
             "asset_path, widget_name, radius, color?, intensity?, inner_outer_mix?, compile?."),
        FMonolithActionHandler::CreateStatic(&FEffectSurfaceActions::HandleSetGlow));

    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_effect_surface_filter"),
        TEXT("Set EffectSurface CSS-style filter (saturation/brightness/contrast; identity = 1.0). At "
             "least one field required. ORs Filter into Effect.FeatureFlags. Params: asset_path, "
             "widget_name, saturation?, brightness?, contrast?, compile?."),
        FMonolithActionHandler::CreateStatic(&FEffectSurfaceActions::HandleSetFilter));

    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_effect_surface_backdropBlur"),
        TEXT("Set EffectSurface backdrop-blur strength (slate units). 0 disables (skips bit-flip). When "
             "strength > 0 the widget wraps its tree in SBackgroundBlur on next RebuildWidget. Params: "
             "asset_path, widget_name, strength, compile?."),
        FMonolithActionHandler::CreateStatic(&FEffectSurfaceActions::HandleSetBackdropBlur));

    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_effect_surface_insetHighlight"),
        TEXT("Set EffectSurface inset-highlight bag (Material-Design elevation overlay). ORs "
             "InsetHighlight into Effect.FeatureFlags. Params: asset_path, widget_name, offset?, blur?, "
             "color?, intensity?, edge_mask? (EEffectInsetEdge bitfield: Top=1, Right=2, Bottom=4, "
             "Left=8), compile?."),
        FMonolithActionHandler::CreateStatic(&FEffectSurfaceActions::HandleSetInsetHighlight));

    Registry.RegisterAction(
        TEXT("ui"), TEXT("apply_effect_surface_preset"),
        TEXT("Apply a curated FEffectSurfaceConfig preset onto an EffectSurface widget in one batch "
             "(20-ish allowlist writes + one ForceMIDRefresh). Optional parent_material is applied "
             "FIRST so the MID is recreated against the preset's intended shader. Recognised preset "
             "names: rounded-rect, pill, circle, glass, glowing-button, neon. Params: asset_path, "
             "widget_name, preset_name, parent_material? (asset path), compile?."),
        FMonolithActionHandler::CreateStatic(&FEffectSurfaceActions::HandleApplyPreset));
}
