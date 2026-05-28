// Copyright tumourlove. All Rights Reserved.
// MonolithUISpecActions.cpp
//
// Phase H — registers the two LLM-facing entry points:
//
//   * `ui::build_ui_from_spec`   — the centerpiece transactional builder.
//   * `ui::dump_ui_spec_schema`  — JSON-Schema-style description of
//                                  FUISpecDocument + the live allowlist
//                                  projection.
//
// The MCP handler is intentionally thin — it's a parse + dispatch shim that
// hands off to FUISpecBuilder. All the policy decisions (atomicity, dry-run,
// strict mode) live in the builder; the action handler exists only to map
// the JSON wire shape onto FUISpecBuilderInputs and back to a JSON response.

#include "Actions/MonolithUISpecActions.h"

#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"

#include "Spec/UISpec.h"
#include "Spec/UISpecValidator.h"
#include "Spec/UISpecBuilder.h"
// Phase J: dump_ui_spec serializer.
#include "Spec/UISpecSerializer.h"

#include "Registry/MonolithUIRegistrySubsystem.h"
#include "Registry/UITypeRegistry.h"
#include "Registry/UIPropertyAllowlist.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "MonolithUICommon.h"

namespace MonolithUI::SpecActionsInternal
{
    // ------------------------------------------------------------------
    // JSON -> FUISpecDocument parser. Manual walk because FUISpecNode
    // recurses through TSharedPtr (not a UPROPERTY) — FJsonObjectConverter
    // can't traverse it.

    static void ParseSlot(const TSharedPtr<FJsonObject>& Obj, FUISpecSlot& OutSlot);
    static void ParseStyle(const TSharedPtr<FJsonObject>& Obj, FUISpecStyle& OutStyle);
    static void ParseContent(const TSharedPtr<FJsonObject>& Obj, FUISpecContent& OutContent);
    static void ParseEffect(const TSharedPtr<FJsonObject>& Obj, FUISpecEffect& OutEffect);
    static void ParseCommonUI(const TSharedPtr<FJsonObject>& Obj, FUISpecCommonUI& OutCUI);
    static TSharedPtr<FUISpecNode> ParseNode(const TSharedPtr<FJsonObject>& Obj);

    static FName GetFNameField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
    {
        FString S;
        if (Obj.IsValid() && Obj->TryGetStringField(Field, S) && !S.IsEmpty())
        {
            return FName(*S);
        }
        return NAME_None;
    }

    static double GetNumberField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, double Def = 0.0)
    {
        double V = Def;
        if (Obj.IsValid())
        {
            Obj->TryGetNumberField(Field, V);
        }
        return V;
    }

    static bool GetBoolField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, bool Def = false)
    {
        bool V = Def;
        if (Obj.IsValid())
        {
            Obj->TryGetBoolField(Field, V);
        }
        return V;
    }

    static FString GetStringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
    {
        FString S;
        if (Obj.IsValid())
        {
            Obj->TryGetStringField(Field, S);
        }
        return S;
    }

    static FVector2D ParseVec2(const TSharedPtr<FJsonObject>& Obj)
    {
        if (!Obj.IsValid()) return FVector2D::ZeroVector;
        return FVector2D(GetNumberField(Obj, TEXT("x")), GetNumberField(Obj, TEXT("y")));
    }

    static FLinearColor ParseColor(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
    {
        FString S;
        if (Obj.IsValid() && Obj->TryGetStringField(Field, S) && !S.IsEmpty())
        {
            FLinearColor C;
            if (MonolithUI::TryParseColor(S, C))
            {
                return C;
            }
        }
        // Default to white (matches the FUISpecStyle default).
        return FLinearColor::White;
    }

    static FMargin ParseMargin(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
    {
        const TSharedPtr<FJsonObject>* Sub = nullptr;
        if (!Obj.IsValid() || !Obj->TryGetObjectField(Field, Sub) || !Sub) return FMargin();
        return FMargin(
            (float)GetNumberField(*Sub, TEXT("left")),
            (float)GetNumberField(*Sub, TEXT("top")),
            (float)GetNumberField(*Sub, TEXT("right")),
            (float)GetNumberField(*Sub, TEXT("bottom")));
    }

    static void ParseSlot(const TSharedPtr<FJsonObject>& Obj, FUISpecSlot& OutSlot)
    {
        if (!Obj.IsValid()) return;
        OutSlot.AnchorPreset = GetFNameField(Obj, TEXT("anchorPreset"));
        OutSlot.bAutoSize    = GetBoolField(Obj, TEXT("autoSize"), false);
        OutSlot.ZOrder       = (int32)GetNumberField(Obj, TEXT("zOrder"));
        OutSlot.HAlign       = GetFNameField(Obj, TEXT("hAlign"));
        OutSlot.VAlign       = GetFNameField(Obj, TEXT("vAlign"));
        OutSlot.SizeRule     = GetFNameField(Obj, TEXT("sizeRule"));
        OutSlot.FillWeight   = (float)GetNumberField(Obj, TEXT("fillWeight"), 1.0);

        const TSharedPtr<FJsonObject>* Sub = nullptr;
        if (Obj->TryGetObjectField(TEXT("position"), Sub) && Sub) OutSlot.Position = ParseVec2(*Sub);
        if (Obj->TryGetObjectField(TEXT("size"), Sub)     && Sub) OutSlot.Size     = ParseVec2(*Sub);
        if (Obj->TryGetObjectField(TEXT("alignment"), Sub) && Sub) OutSlot.Alignment = ParseVec2(*Sub);
        OutSlot.Padding = ParseMargin(Obj, TEXT("padding"));
    }

    static void ParseStyle(const TSharedPtr<FJsonObject>& Obj, FUISpecStyle& OutStyle)
    {
        if (!Obj.IsValid()) return;
        OutStyle.Width        = (float)GetNumberField(Obj, TEXT("width"));
        OutStyle.Height       = (float)GetNumberField(Obj, TEXT("height"));
        OutStyle.Opacity      = (float)GetNumberField(Obj, TEXT("opacity"), 1.0);
        OutStyle.BorderWidth  = (float)GetNumberField(Obj, TEXT("borderWidth"));
        OutStyle.bUseCustomSize = GetBoolField(Obj, TEXT("useCustomSize"));
        double DesiredValue = 0.0;
        OutStyle.bOverrideMinDesiredWidth = GetBoolField(Obj, TEXT("overrideMinDesiredWidth"));
        if (Obj->TryGetNumberField(TEXT("minDesiredWidth"), DesiredValue))
        {
            OutStyle.bOverrideMinDesiredWidth = true;
            OutStyle.MinDesiredWidth = (float)DesiredValue;
        }
        OutStyle.bOverrideMinDesiredHeight = GetBoolField(Obj, TEXT("overrideMinDesiredHeight"));
        if (Obj->TryGetNumberField(TEXT("minDesiredHeight"), DesiredValue))
        {
            OutStyle.bOverrideMinDesiredHeight = true;
            OutStyle.MinDesiredHeight = (float)DesiredValue;
        }
        OutStyle.bOverrideMaxDesiredWidth = GetBoolField(Obj, TEXT("overrideMaxDesiredWidth"));
        if (Obj->TryGetNumberField(TEXT("maxDesiredWidth"), DesiredValue))
        {
            OutStyle.bOverrideMaxDesiredWidth = true;
            OutStyle.MaxDesiredWidth = (float)DesiredValue;
        }
        OutStyle.bOverrideMaxDesiredHeight = GetBoolField(Obj, TEXT("overrideMaxDesiredHeight"));
        if (Obj->TryGetNumberField(TEXT("maxDesiredHeight"), DesiredValue))
        {
            OutStyle.bOverrideMaxDesiredHeight = true;
            OutStyle.MaxDesiredHeight = (float)DesiredValue;
        }
        OutStyle.Visibility   = GetFNameField(Obj, TEXT("visibility"));
        OutStyle.Background   = ParseColor(Obj, TEXT("background"));
        OutStyle.BorderColor  = ParseColor(Obj, TEXT("borderColor"));
        OutStyle.Padding      = ParseMargin(Obj, TEXT("padding"));
    }

    static void ParseContent(const TSharedPtr<FJsonObject>& Obj, FUISpecContent& OutContent)
    {
        if (!Obj.IsValid()) return;
        OutContent.Text        = GetStringField(Obj, TEXT("text"));
        OutContent.FontSize    = (float)GetNumberField(Obj, TEXT("fontSize"));
        OutContent.WrapMode    = GetFNameField(Obj, TEXT("wrapMode"));
        OutContent.BrushPath   = GetStringField(Obj, TEXT("brushPath"));
        OutContent.Placeholder = GetStringField(Obj, TEXT("placeholder"));
        OutContent.FontColor   = ParseColor(Obj, TEXT("fontColor"));
    }

    static void ParseEffect(const TSharedPtr<FJsonObject>& Obj, FUISpecEffect& OutEffect)
    {
        if (!Obj.IsValid()) return;
        OutEffect.Smoothness = (float)GetNumberField(Obj, TEXT("smoothness"), 1.0);
        OutEffect.SolidColor = ParseColor(Obj, TEXT("solidColor"));
        OutEffect.BackdropBlurStrength = (float)GetNumberField(Obj, TEXT("backdropBlurStrength"));

        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Obj->TryGetArrayField(TEXT("cornerRadii"), Arr) && Arr && Arr->Num() >= 4)
        {
            OutEffect.CornerRadii = FVector4(
                (*Arr)[0]->AsNumber(),
                (*Arr)[1]->AsNumber(),
                (*Arr)[2]->AsNumber(),
                (*Arr)[3]->AsNumber());
        }

        // Phase J: parse drop / inner shadow arrays so the roundtrip is
        // symmetric with FUISpecSerializer's array-path read. The Phase H
        // deferral is closed here -- the arrays land in Out*Shadows and the
        // EffectSurfaceBuilder reads them via the typed FEffectShadow path.
        auto ParseShadowArray = [&Obj](const TCHAR* Field, TArray<FUISpecEffectShadow>& Out, bool bDefaultInset)
        {
            const TArray<TSharedPtr<FJsonValue>>* ShArr = nullptr;
            if (!Obj->TryGetArrayField(Field, ShArr) || !ShArr) return;
            for (const TSharedPtr<FJsonValue>& V : *ShArr)
            {
                const TSharedPtr<FJsonObject>* SObj = nullptr;
                if (!V.IsValid() || !V->TryGetObject(SObj) || !SObj) continue;
                FUISpecEffectShadow S;
                const TSharedPtr<FJsonObject>* OffObj = nullptr;
                if ((*SObj)->TryGetObjectField(TEXT("offset"), OffObj) && OffObj)
                {
                    S.Offset = FVector2D(
                        (*OffObj)->GetNumberField(TEXT("x")),
                        (*OffObj)->GetNumberField(TEXT("y")));
                }
                S.Blur   = (float)(*SObj)->GetNumberField(TEXT("blur"));
                S.Spread = (float)(*SObj)->GetNumberField(TEXT("spread"));
                S.bInset = bDefaultInset;
                (*SObj)->TryGetBoolField(TEXT("inset"), S.bInset);
                FString HexColor;
                if ((*SObj)->TryGetStringField(TEXT("color"), HexColor) && !HexColor.IsEmpty())
                {
                    FLinearColor C;
                    if (MonolithUI::TryParseColor(HexColor, C))
                    {
                        S.Color = C;
                    }
                }
                Out.Add(S);
            }
        };

        ParseShadowArray(TEXT("dropShadows"),  OutEffect.DropShadows,  /*bDefaultInset=*/false);
        ParseShadowArray(TEXT("innerShadows"), OutEffect.InnerShadows, /*bDefaultInset=*/true);
    }

    static void ParseCommonUI(const TSharedPtr<FJsonObject>& Obj, FUISpecCommonUI& OutCUI)
    {
        if (!Obj.IsValid()) return;
        OutCUI.InputLayer = GetFNameField(Obj, TEXT("inputLayer"));
        OutCUI.InputMode  = GetFNameField(Obj, TEXT("inputMode"));

        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Obj->TryGetArrayField(TEXT("styleRefs"), Arr) && Arr)
        {
            for (const TSharedPtr<FJsonValue>& V : *Arr)
            {
                if (V.IsValid() && V->Type == EJson::String)
                {
                    OutCUI.StyleRefs.Add(FName(*V->AsString()));
                }
            }
        }
    }

    static TSharedPtr<FUISpecNode> ParseNode(const TSharedPtr<FJsonObject>& Obj)
    {
        if (!Obj.IsValid()) return nullptr;
        TSharedPtr<FUISpecNode> Node = MakeShared<FUISpecNode>();

        Node->Type            = GetFNameField(Obj, TEXT("type"));
        Node->Id              = GetFNameField(Obj, TEXT("id"));
        Node->StyleRef        = GetFNameField(Obj, TEXT("styleRef"));
        Node->CustomClassPath = GetStringField(Obj, TEXT("customClassPath"));

        const TSharedPtr<FJsonObject>* Sub = nullptr;
        if (Obj->TryGetObjectField(TEXT("slot"),    Sub) && Sub) ParseSlot(*Sub, Node->Slot);
        if (Obj->TryGetObjectField(TEXT("style"),   Sub) && Sub) ParseStyle(*Sub, Node->Style);
        if (Obj->TryGetObjectField(TEXT("content"), Sub) && Sub) ParseContent(*Sub, Node->Content);

        if (Obj->TryGetObjectField(TEXT("effect"), Sub) && Sub)
        {
            ParseEffect(*Sub, Node->Effect);
            Node->bHasEffect = true;
        }

        if (Obj->TryGetObjectField(TEXT("commonUI"), Sub) && Sub)
        {
            ParseCommonUI(*Sub, Node->CommonUI);
            Node->bHasCommonUI = true;
        }

        const TArray<TSharedPtr<FJsonValue>>* AnimRefArr = nullptr;
        if (Obj->TryGetArrayField(TEXT("animationRefs"), AnimRefArr) && AnimRefArr)
        {
            for (const TSharedPtr<FJsonValue>& V : *AnimRefArr)
            {
                if (V.IsValid() && V->Type == EJson::String)
                {
                    Node->AnimationRefs.Add(FName(*V->AsString()));
                }
            }
        }

        const TArray<TSharedPtr<FJsonValue>>* ChildArr = nullptr;
        if (Obj->TryGetArrayField(TEXT("children"), ChildArr) && ChildArr)
        {
            for (const TSharedPtr<FJsonValue>& V : *ChildArr)
            {
                const TSharedPtr<FJsonObject>* ChildObj = nullptr;
                if (V.IsValid() && V->TryGetObject(ChildObj) && ChildObj)
                {
                    if (TSharedPtr<FUISpecNode> Child = ParseNode(*ChildObj))
                    {
                        Node->Children.Add(Child);
                    }
                }
            }
        }
        return Node;
    }

    /**
     * Entry point: parse a `spec` JSON object into a populated FUISpecDocument.
     * Returns false on syntactic problems with `spec` (we bubble up a single
     * error finding into OutValidation so the caller can short-circuit
     * uniformly with the validator-fail path).
     */
    static bool ParseDocument(
        const TSharedPtr<FJsonObject>& SpecObj,
        FUISpecDocument& OutDoc,
        FUISpecValidationResult& OutValidation)
    {
        if (!SpecObj.IsValid())
        {
            FUISpecError E;
            E.Severity = EUISpecErrorSeverity::Error;
            E.Category = TEXT("Parse");
            E.Message  = TEXT("`spec` is missing or not a JSON object.");
            OutValidation.Errors.Add(MoveTemp(E));
            OutValidation.bIsValid = false;
            return false;
        }

        OutDoc.Version     = (int32)GetNumberField(SpecObj, TEXT("version"), 1);
        OutDoc.Name        = GetStringField(SpecObj, TEXT("name"));
        OutDoc.ParentClass = GetStringField(SpecObj, TEXT("parentClass"));
        OutDoc.bTreatWarningsAsErrors =
            GetBoolField(SpecObj, TEXT("treatWarningsAsErrors"), false);

        // Metadata.
        const TSharedPtr<FJsonObject>* Sub = nullptr;
        if (SpecObj->TryGetObjectField(TEXT("metadata"), Sub) && Sub)
        {
            OutDoc.Metadata.AuthoringTool = GetStringField(*Sub, TEXT("authoringTool"));
            OutDoc.Metadata.SourceFile    = GetStringField(*Sub, TEXT("sourceFile"));
            OutDoc.Metadata.Author        = GetStringField(*Sub, TEXT("author"));
            OutDoc.Metadata.Description   = GetStringField(*Sub, TEXT("description"));
        }

        // Styles map.
        if (SpecObj->TryGetObjectField(TEXT("styles"), Sub) && Sub)
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Sub)->Values)
            {
                const TSharedPtr<FJsonObject>* StyleObj = nullptr;
                if (Pair.Value.IsValid() && Pair.Value->TryGetObject(StyleObj) && StyleObj)
                {
                    FUISpecStyle Style;
                    ParseStyle(*StyleObj, Style);
                    OutDoc.Styles.Add(FName(*Pair.Key), Style);
                }
            }
        }

        // Animations.
        const TArray<TSharedPtr<FJsonValue>>* AnimArr = nullptr;
        if (SpecObj->TryGetArrayField(TEXT("animations"), AnimArr) && AnimArr)
        {
            for (const TSharedPtr<FJsonValue>& V : *AnimArr)
            {
                const TSharedPtr<FJsonObject>* AnimObj = nullptr;
                if (V.IsValid() && V->TryGetObject(AnimObj) && AnimObj)
                {
                    FUISpecAnimation A;
                    A.Name           = GetFNameField(*AnimObj, TEXT("name"));
                    A.TargetWidgetId = GetFNameField(*AnimObj, TEXT("targetWidgetId"));
                    A.TargetProperty = GetFNameField(*AnimObj, TEXT("targetProperty"));
                    A.Duration       = (float)GetNumberField(*AnimObj, TEXT("duration"));
                    A.Delay          = (float)GetNumberField(*AnimObj, TEXT("delay"));
                    A.Easing         = GetFNameField(*AnimObj, TEXT("easing"));
                    A.LoopMode       = GetFNameField(*AnimObj, TEXT("loopMode"));
                    A.bAutoPlay      = GetBoolField(*AnimObj, TEXT("autoPlay"), false);
                    // Keyframes: iterate JSON array; rich tangent fields supported.
                    const TArray<TSharedPtr<FJsonValue>>* KFArr = nullptr;
                    if ((*AnimObj)->TryGetArrayField(TEXT("keyframes"), KFArr) && KFArr)
                    {
                        for (const TSharedPtr<FJsonValue>& KV : *KFArr)
                        {
                            const TSharedPtr<FJsonObject>* KObj = nullptr;
                            if (KV.IsValid() && KV->TryGetObject(KObj) && KObj)
                            {
                                FUISpecKeyframe K;
                                K.Time         = (float)GetNumberField(*KObj, TEXT("time"));
                                K.ScalarValue  = (float)GetNumberField(*KObj, TEXT("scalarValue"));
                                K.Easing       = GetFNameField(*KObj, TEXT("easing"));
                                K.bUseCustomTangents = GetBoolField(*KObj, TEXT("useCustomTangents"), false);
                                K.ArriveTangent = (float)GetNumberField(*KObj, TEXT("arriveTangent"));
                                K.LeaveTangent  = (float)GetNumberField(*KObj, TEXT("leaveTangent"));
                                K.ArriveWeight  = (float)GetNumberField(*KObj, TEXT("arriveWeight"));
                                K.LeaveWeight   = (float)GetNumberField(*KObj, TEXT("leaveWeight"));
                                A.Keyframes.Add(K);
                            }
                        }
                    }
                    OutDoc.Animations.Add(A);
                }
            }
        }

        // Root.
        if (SpecObj->TryGetObjectField(TEXT("rootWidget"), Sub) && Sub)
        {
            OutDoc.Root = ParseNode(*Sub);
        }

        return true;
    }

    /**
     * Pack a populated FUISpecBuilderResult into the JSON response shape.
     * Mirror of the documented action wire shape:
     *   { bSuccess, asset_path, request_id|null, validation, node_counts,
     *     diff?, errors?, warnings? }
     */
    static TSharedPtr<FJsonObject> PackResponse(const FUISpecBuilderResult& R, bool bDryRun)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField  (TEXT("bSuccess"),    R.bSuccess);
        Out->SetStringField(TEXT("asset_path"),  R.AssetPath);
        if (!R.RequestId.IsEmpty())
        {
            Out->SetStringField(TEXT("request_id"), R.RequestId);
        }

        // Validation block — flat shape so the LLM can grep its way through.
        TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
        Validation->SetBoolField(TEXT("is_valid"), R.Validation.bIsValid);
        Validation->SetStringField(TEXT("llm_report"), R.Validation.ToLLMReport());
        Out->SetObjectField(TEXT("validation"), Validation);

        TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
        Counts->SetNumberField(TEXT("created"),  R.NodesCreated);
        Counts->SetNumberField(TEXT("modified"), R.NodesModified);
        Counts->SetNumberField(TEXT("removed"),  R.NodesRemoved);
        Out->SetObjectField(TEXT("node_counts"), Counts);

        if (R.Errors.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Errs;
            for (const FUISpecError& E : R.Errors)
            {
                TSharedPtr<FJsonObject> EObj = MakeShared<FJsonObject>();
                EObj->SetStringField(TEXT("category"), E.Category.ToString());
                EObj->SetStringField(TEXT("widget_id"), E.WidgetId.ToString());
                EObj->SetStringField(TEXT("message"),  E.Message);
                EObj->SetStringField(TEXT("json_path"), E.JsonPath);
                EObj->SetStringField(TEXT("suggested_fix"), E.SuggestedFix);
                Errs.Add(MakeShared<FJsonValueObject>(EObj));
            }
            Out->SetArrayField(TEXT("errors"), Errs);
        }
        if (R.Warnings.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Warns;
            for (const FUISpecError& W : R.Warnings)
            {
                TSharedPtr<FJsonObject> WObj = MakeShared<FJsonObject>();
                WObj->SetStringField(TEXT("category"), W.Category.ToString());
                WObj->SetStringField(TEXT("widget_id"), W.WidgetId.ToString());
                WObj->SetStringField(TEXT("message"),  W.Message);
                WObj->SetStringField(TEXT("suggested_fix"), W.SuggestedFix);
                Warns.Add(MakeShared<FJsonValueObject>(WObj));
            }
            Out->SetArrayField(TEXT("warnings"), Warns);
        }
        if (bDryRun || R.DiffLines.Num() > 0)
        {
            TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
            TArray<TSharedPtr<FJsonValue>> Lines;
            for (const FString& L : R.DiffLines)
            {
                Lines.Add(MakeShared<FJsonValueString>(L));
            }
            Diff->SetArrayField(TEXT("lines"), Lines);
            Diff->SetBoolField(TEXT("dry_run"), bDryRun);
            Out->SetObjectField(TEXT("diff"), Diff);
        }
        return Out;
    }

    // ------------------------------------------------------------------
    // ui::build_ui_from_spec handler

    static FMonolithActionResult HandleBuildUIFromSpec(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
        }

        FString AssetPath;
        if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        {
            return FMonolithActionResult::Error(TEXT("Missing or empty required param: asset_path"), -32602);
        }

        const TSharedPtr<FJsonObject>* SpecObjPtr = nullptr;
        if (!Params->TryGetObjectField(TEXT("spec"), SpecObjPtr) || !SpecObjPtr || !(*SpecObjPtr).IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing or invalid required param: spec (must be a JSON object)"), -32602);
        }

        // Phase K: pull request_id BEFORE the parse step so the parse-fail
        // response can echo it back. The parse-fail path used to drop request_id
        // on the floor — caller correlation broke when the JSON had a syntax
        // error. Now the echo is uniform across parse-fail / validate-fail /
        // builder-fail / success.
        FString RequestId;
        Params->TryGetStringField(TEXT("request_id"), RequestId);

        FUISpecDocument Document;
        FUISpecValidationResult ParseValidation;
        if (!ParseDocument(*SpecObjPtr, Document, ParseValidation))
        {
            FUISpecBuilderResult R;
            R.AssetPath = AssetPath;
            R.RequestId = RequestId;  // Phase K — echo even on parse failure.
            R.Validation = ParseValidation;
            // Return as success-on-the-wire so the LLM gets the full shape;
            // bSuccess=false in the payload signals semantic failure.
            return FMonolithActionResult::Success(PackResponse(R, /*bDryRun=*/false));
        }

        FUISpecBuilderInputs Inputs;
        Inputs.Document  = &Document;
        Inputs.AssetPath = AssetPath;
        Inputs.bOverwrite             = true;  // default per spec
        Inputs.bDryRun                = false;
        Inputs.bTreatWarningsAsErrors = false;
        Inputs.bRawMode               = false;
        Params->TryGetBoolField(TEXT("overwrite"), Inputs.bOverwrite);
        Params->TryGetBoolField(TEXT("dry_run"), Inputs.bDryRun);
        Params->TryGetBoolField(TEXT("treat_warnings_as_errors"), Inputs.bTreatWarningsAsErrors);
        Params->TryGetBoolField(TEXT("raw_mode"), Inputs.bRawMode);
        // Phase K — request_id was already pulled above. Reuse the same value
        // so parse-fail / validate-fail / success all echo the identical token.
        Inputs.RequestId = RequestId;

        // Per-document override — the spec itself may set the strict flag.
        if (Document.bTreatWarningsAsErrors)
        {
            Inputs.bTreatWarningsAsErrors = true;
        }

        const FUISpecBuilderResult R = FUISpecBuilder::Build(Inputs);
        return FMonolithActionResult::Success(PackResponse(R, Inputs.bDryRun));
    }

    // ------------------------------------------------------------------
    // ui::dump_ui_spec_schema handler

    /**
     * Build a JSON-Schema-style projection of `FUISpecDocument` plus the
     * live allowlist projection. Intentionally informal — the LLM reads it
     * as a contract surface, not a strict validator.
     */
    static FMonolithActionResult HandleDumpUISpecSchema(const TSharedPtr<FJsonObject>& /*Params*/)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("schema_version"), TEXT("1"));
        Out->SetStringField(TEXT("document_type"), TEXT("FUISpecDocument"));

        // Document-level fields (top-level keys recognised by the parser).
        {
            TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
            auto AddField = [&Fields](const TCHAR* Name, const TCHAR* Type, const TCHAR* Desc)
            {
                TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
                F->SetStringField(TEXT("type"), Type);
                F->SetStringField(TEXT("description"), Desc);
                Fields->SetObjectField(Name, F);
            };
            AddField(TEXT("version"),     TEXT("integer"), TEXT("Schema version. Bumped on incompatible parser changes."));
            AddField(TEXT("name"),        TEXT("string"),  TEXT("Widget Blueprint name (becomes the asset filename)."));
            AddField(TEXT("parentClass"), TEXT("string"),  TEXT("Parent class token (UserWidget / CommonActivatableWidget / CommonUserWidget) or full /Script path."));
            AddField(TEXT("metadata"),    TEXT("object"),  TEXT("FUISpecMetadata bag (authoringTool / sourceFile / author / description / tags)."));
            AddField(TEXT("styles"),      TEXT("object"),  TEXT("Map<name, FUISpecStyle> — named styles referenced by node.styleRef."));
            AddField(TEXT("animations"),  TEXT("array"),   TEXT("FUISpecAnimation[] — named widget animations."));
            AddField(TEXT("treatWarningsAsErrors"), TEXT("boolean"), TEXT("When true, validator warnings escalate to errors."));
            AddField(TEXT("rootWidget"),  TEXT("object"),  TEXT("Required. FUISpecNode root of the widget tree."));
            Out->SetObjectField(TEXT("document_fields"), Fields);
        }

        // Node-level fields.
        {
            TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
            auto AddField = [&Fields](const TCHAR* Name, const TCHAR* Type, const TCHAR* Desc)
            {
                TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
                F->SetStringField(TEXT("type"), Type);
                F->SetStringField(TEXT("description"), Desc);
                Fields->SetObjectField(Name, F);
            };
            AddField(TEXT("type"),            TEXT("string"),  TEXT("Widget type token resolved through FUITypeRegistry (e.g. 'VerticalBox', 'TextBlock', 'EffectSurface')."));
            AddField(TEXT("id"),              TEXT("string"),  TEXT("Variable name of the widget on the WBP. Must be unique within the spec."));
            AddField(TEXT("slot"),            TEXT("object"),  TEXT("FUISpecSlot (anchorPreset / position / size / alignment / padding / autoSize / hAlign / vAlign / zOrder / sizeRule / fillWeight)."));
            AddField(TEXT("style"),           TEXT("object"),  TEXT("FUISpecStyle (width / height / minDesiredWidth / minDesiredHeight / maxDesiredWidth / maxDesiredHeight / override flags / padding / background / borderColor / borderWidth / opacity / visibility)."));
            AddField(TEXT("content"),         TEXT("object"),  TEXT("FUISpecContent (text / fontSize / fontColor / wrapMode / brushPath / placeholder)."));
            AddField(TEXT("effect"),          TEXT("object"),  TEXT("FUISpecEffect — UEffectSurface only. Sub-bag triggers bHasEffect."));
            AddField(TEXT("commonUI"),        TEXT("object"),  TEXT("FUISpecCommonUI (inputLayer / inputMode / styleRefs[]). Sub-bag triggers bHasCommonUI."));
            AddField(TEXT("styleRef"),        TEXT("string"),  TEXT("Named entry in document.styles."));
            AddField(TEXT("animationRefs"),   TEXT("array"),   TEXT("Names of entries in document.animations to bind to this widget."));
            AddField(TEXT("customClassPath"), TEXT("string"),  TEXT("Fallback when 'type' is not in the registry — full Blueprint class path."));
            AddField(TEXT("children"),        TEXT("array"),   TEXT("FUISpecNode[] — nested widgets."));
            Out->SetObjectField(TEXT("node_fields"), Fields);
        }

        // Live allowlist projection per type — same shape ui::dump_property_allowlist
        // gives us, embedded here so a single call returns the full contract.
        {
            TSharedPtr<FJsonObject> ByType = MakeShared<FJsonObject>();
            if (UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get())
            {
                const FUITypeRegistry& Reg = Sub->GetTypeRegistry();
                const FUIPropertyAllowlist& Allow = Sub->GetAllowlist();
                for (const FUITypeRegistryEntry& Entry : Reg.GetAll())
                {
                    const TArray<FString>& Paths = Allow.GetAllowedPaths(Entry.Token);
                    if (Paths.Num() == 0) continue;

                    TArray<TSharedPtr<FJsonValue>> Arr;
                    Arr.Reserve(Paths.Num());
                    for (const FString& P : Paths)
                    {
                        Arr.Add(MakeShared<FJsonValueString>(P));
                    }
                    ByType->SetArrayField(Entry.Token.ToString(), Arr);
                }
            }
            Out->SetObjectField(TEXT("allowlist_by_type"), ByType);
        }

        return FMonolithActionResult::Success(Out);
    }

    // ------------------------------------------------------------------
    // Phase J: ui::dump_ui_spec handler
    //
    // Reads an existing UWidgetBlueprint and emits a FUISpecDocument JSON
    // suitable for round-tripping through ui::build_ui_from_spec. Pure read;
    // no asset mutation. Mirrors the build response shape so action surfaces
    // can compose the two passes uniformly.
    // ------------------------------------------------------------------

    /** Emit FUISpecSlot as JSON. Inverse of ParseSlot above. */
    static TSharedPtr<FJsonObject> SlotToJson(const FUISpecSlot& S)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!S.AnchorPreset.IsNone()) Out->SetStringField(TEXT("anchorPreset"), S.AnchorPreset.ToString());
        if (!S.HAlign.IsNone())       Out->SetStringField(TEXT("hAlign"), S.HAlign.ToString());
        if (!S.VAlign.IsNone())       Out->SetStringField(TEXT("vAlign"), S.VAlign.ToString());
        if (!S.SizeRule.IsNone())
        {
            Out->SetStringField(TEXT("sizeRule"), S.SizeRule.ToString());
            Out->SetNumberField(TEXT("fillWeight"), S.FillWeight);
        }
        if (S.bAutoSize)              Out->SetBoolField(TEXT("autoSize"), true);
        if (S.ZOrder != 0)            Out->SetNumberField(TEXT("zOrder"), S.ZOrder);

        // Position, size, alignment as object{x,y}.
        TSharedPtr<FJsonObject> Pos = MakeShared<FJsonObject>();
        Pos->SetNumberField(TEXT("x"), S.Position.X);
        Pos->SetNumberField(TEXT("y"), S.Position.Y);
        Out->SetObjectField(TEXT("position"), Pos);

        TSharedPtr<FJsonObject> Sz = MakeShared<FJsonObject>();
        Sz->SetNumberField(TEXT("x"), S.Size.X);
        Sz->SetNumberField(TEXT("y"), S.Size.Y);
        Out->SetObjectField(TEXT("size"), Sz);

        TSharedPtr<FJsonObject> Al = MakeShared<FJsonObject>();
        Al->SetNumberField(TEXT("x"), S.Alignment.X);
        Al->SetNumberField(TEXT("y"), S.Alignment.Y);
        Out->SetObjectField(TEXT("alignment"), Al);

        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetNumberField(TEXT("left"),   S.Padding.Left);
        P->SetNumberField(TEXT("top"),    S.Padding.Top);
        P->SetNumberField(TEXT("right"),  S.Padding.Right);
        P->SetNumberField(TEXT("bottom"), S.Padding.Bottom);
        Out->SetObjectField(TEXT("padding"), P);
        return Out;
    }

    static FString ColorToHexString(const FLinearColor& C)
    {
        const FColor B = C.ToFColor(/*bSRGB=*/false);
        return FString::Printf(TEXT("#%02X%02X%02X%02X"), B.R, B.G, B.B, B.A);
    }

    /** Emit FUISpecStyle as JSON. Inverse of ParseStyle. */
    static TSharedPtr<FJsonObject> StyleToJson(const FUISpecStyle& S)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (S.Width  != 0.f) Out->SetNumberField(TEXT("width"),  S.Width);
        if (S.Height != 0.f) Out->SetNumberField(TEXT("height"), S.Height);
        if (S.bOverrideMinDesiredWidth)
        {
            Out->SetBoolField(TEXT("overrideMinDesiredWidth"), true);
            Out->SetNumberField(TEXT("minDesiredWidth"), S.MinDesiredWidth);
        }
        if (S.bOverrideMinDesiredHeight)
        {
            Out->SetBoolField(TEXT("overrideMinDesiredHeight"), true);
            Out->SetNumberField(TEXT("minDesiredHeight"), S.MinDesiredHeight);
        }
        if (S.bOverrideMaxDesiredWidth)
        {
            Out->SetBoolField(TEXT("overrideMaxDesiredWidth"), true);
            Out->SetNumberField(TEXT("maxDesiredWidth"), S.MaxDesiredWidth);
        }
        if (S.bOverrideMaxDesiredHeight)
        {
            Out->SetBoolField(TEXT("overrideMaxDesiredHeight"), true);
            Out->SetNumberField(TEXT("maxDesiredHeight"), S.MaxDesiredHeight);
        }
        if (S.BorderWidth != 0.f) Out->SetNumberField(TEXT("borderWidth"), S.BorderWidth);
        Out->SetNumberField(TEXT("opacity"), S.Opacity);
        if (S.bUseCustomSize) Out->SetBoolField(TEXT("useCustomSize"), true);
        if (!S.Visibility.IsNone()) Out->SetStringField(TEXT("visibility"), S.Visibility.ToString());
        Out->SetStringField(TEXT("background"),  ColorToHexString(S.Background));
        Out->SetStringField(TEXT("borderColor"), ColorToHexString(S.BorderColor));

        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetNumberField(TEXT("left"),   S.Padding.Left);
        P->SetNumberField(TEXT("top"),    S.Padding.Top);
        P->SetNumberField(TEXT("right"),  S.Padding.Right);
        P->SetNumberField(TEXT("bottom"), S.Padding.Bottom);
        Out->SetObjectField(TEXT("padding"), P);
        return Out;
    }

    /** Emit FUISpecContent as JSON. Inverse of ParseContent. */
    static TSharedPtr<FJsonObject> ContentToJson(const FUISpecContent& C)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!C.Text.IsEmpty())        Out->SetStringField(TEXT("text"), C.Text);
        if (C.FontSize != 0.f)        Out->SetNumberField(TEXT("fontSize"), C.FontSize);
        if (!C.WrapMode.IsNone())     Out->SetStringField(TEXT("wrapMode"), C.WrapMode.ToString());
        if (!C.BrushPath.IsEmpty())   Out->SetStringField(TEXT("brushPath"), C.BrushPath);
        if (!C.Placeholder.IsEmpty()) Out->SetStringField(TEXT("placeholder"), C.Placeholder);
        Out->SetStringField(TEXT("fontColor"), ColorToHexString(C.FontColor));
        return Out;
    }

    /** Emit a single FUISpecEffectShadow entry. */
    static TSharedPtr<FJsonObject> ShadowToJson(const FUISpecEffectShadow& S)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> Off = MakeShared<FJsonObject>();
        Off->SetNumberField(TEXT("x"), S.Offset.X);
        Off->SetNumberField(TEXT("y"), S.Offset.Y);
        Out->SetObjectField(TEXT("offset"), Off);
        Out->SetNumberField(TEXT("blur"),   S.Blur);
        Out->SetNumberField(TEXT("spread"), S.Spread);
        Out->SetStringField(TEXT("color"),  ColorToHexString(S.Color));
        Out->SetBoolField  (TEXT("inset"),  S.bInset);
        return Out;
    }

    /** Emit FUISpecEffect as JSON. Inverse of ParseEffect; closes the Phase H deferral. */
    static TSharedPtr<FJsonObject> EffectToJson(const FUISpecEffect& E)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Radii;
        Radii.Add(MakeShared<FJsonValueNumber>(E.CornerRadii.X));
        Radii.Add(MakeShared<FJsonValueNumber>(E.CornerRadii.Y));
        Radii.Add(MakeShared<FJsonValueNumber>(E.CornerRadii.Z));
        Radii.Add(MakeShared<FJsonValueNumber>(E.CornerRadii.W));
        Out->SetArrayField(TEXT("cornerRadii"), Radii);
        Out->SetNumberField(TEXT("smoothness"), E.Smoothness);
        Out->SetStringField(TEXT("solidColor"), ColorToHexString(E.SolidColor));
        Out->SetNumberField(TEXT("backdropBlurStrength"), E.BackdropBlurStrength);

        TArray<TSharedPtr<FJsonValue>> Drops;
        for (const FUISpecEffectShadow& S : E.DropShadows)
        {
            Drops.Add(MakeShared<FJsonValueObject>(ShadowToJson(S)));
        }
        if (Drops.Num() > 0) Out->SetArrayField(TEXT("dropShadows"), Drops);

        TArray<TSharedPtr<FJsonValue>> Inners;
        for (const FUISpecEffectShadow& S : E.InnerShadows)
        {
            Inners.Add(MakeShared<FJsonValueObject>(ShadowToJson(S)));
        }
        if (Inners.Num() > 0) Out->SetArrayField(TEXT("innerShadows"), Inners);
        return Out;
    }

    /** Emit FUISpecCommonUI as JSON. Inverse of ParseCommonUI. */
    static TSharedPtr<FJsonObject> CommonUIToJson(const FUISpecCommonUI& C)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!C.InputLayer.IsNone()) Out->SetStringField(TEXT("inputLayer"), C.InputLayer.ToString());
        if (!C.InputMode.IsNone())  Out->SetStringField(TEXT("inputMode"),  C.InputMode.ToString());
        if (C.StyleRefs.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Arr;
            for (const FName& N : C.StyleRefs)
            {
                Arr.Add(MakeShared<FJsonValueString>(N.ToString()));
            }
            Out->SetArrayField(TEXT("styleRefs"), Arr);
        }
        return Out;
    }

    /** Recursive node serialiser. Inverse of ParseNode. */
    static TSharedPtr<FJsonObject> NodeToJson(const FUISpecNode& N)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!N.Type.IsNone()) Out->SetStringField(TEXT("type"), N.Type.ToString());
        if (!N.Id.IsNone())   Out->SetStringField(TEXT("id"),   N.Id.ToString());
        if (!N.StyleRef.IsNone()) Out->SetStringField(TEXT("styleRef"), N.StyleRef.ToString());
        if (!N.CustomClassPath.IsEmpty()) Out->SetStringField(TEXT("customClassPath"), N.CustomClassPath);

        Out->SetObjectField(TEXT("slot"),    SlotToJson(N.Slot));
        Out->SetObjectField(TEXT("style"),   StyleToJson(N.Style));
        Out->SetObjectField(TEXT("content"), ContentToJson(N.Content));
        if (N.bHasEffect)   Out->SetObjectField(TEXT("effect"),   EffectToJson(N.Effect));
        if (N.bHasCommonUI) Out->SetObjectField(TEXT("commonUI"), CommonUIToJson(N.CommonUI));

        if (N.AnimationRefs.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Arr;
            for (const FName& A : N.AnimationRefs)
            {
                Arr.Add(MakeShared<FJsonValueString>(A.ToString()));
            }
            Out->SetArrayField(TEXT("animationRefs"), Arr);
        }

        if (N.Children.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Arr;
            for (const TSharedPtr<FUISpecNode>& C : N.Children)
            {
                if (C.IsValid())
                {
                    Arr.Add(MakeShared<FJsonValueObject>(NodeToJson(*C)));
                }
            }
            Out->SetArrayField(TEXT("children"), Arr);
        }
        return Out;
    }

    /** Pack a populated FUISpecSerializerResult into the JSON response shape. */
    static TSharedPtr<FJsonObject> PackDumpResponse(const FUISpecSerializerResult& R)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField  (TEXT("bSuccess"),    R.bSuccess);
        Out->SetStringField(TEXT("asset_path"),  R.AssetPath);
        if (!R.RequestId.IsEmpty())
        {
            Out->SetStringField(TEXT("request_id"), R.RequestId);
        }
        Out->SetNumberField(TEXT("nodes_visited"),       R.NodesVisited);
        Out->SetNumberField(TEXT("animations_captured"), R.AnimationsCaptured);

        // Document body -- mirrors the parser's input shape.
        TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
        Spec->SetNumberField(TEXT("version"), R.Document.Version);
        Spec->SetStringField(TEXT("name"), R.Document.Name);
        Spec->SetStringField(TEXT("parentClass"), R.Document.ParentClass);
        TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
        Meta->SetStringField(TEXT("authoringTool"), R.Document.Metadata.AuthoringTool);
        Meta->SetStringField(TEXT("sourceFile"),    R.Document.Metadata.SourceFile);
        Meta->SetStringField(TEXT("author"),        R.Document.Metadata.Author);
        Meta->SetStringField(TEXT("description"),   R.Document.Metadata.Description);
        Spec->SetObjectField(TEXT("metadata"), Meta);

        if (R.Document.Animations.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Arr;
            for (const FUISpecAnimation& A : R.Document.Animations)
            {
                TSharedPtr<FJsonObject> AObj = MakeShared<FJsonObject>();
                AObj->SetStringField(TEXT("name"),           A.Name.ToString());
                AObj->SetStringField(TEXT("targetWidgetId"), A.TargetWidgetId.ToString());
                AObj->SetStringField(TEXT("targetProperty"), A.TargetProperty.ToString());
                AObj->SetNumberField(TEXT("duration"),       A.Duration);
                AObj->SetNumberField(TEXT("delay"),          A.Delay);
                if (!A.Easing.IsNone())   AObj->SetStringField(TEXT("easing"),   A.Easing.ToString());
                if (!A.LoopMode.IsNone()) AObj->SetStringField(TEXT("loopMode"), A.LoopMode.ToString());
                AObj->SetBoolField(TEXT("autoPlay"), A.bAutoPlay);
                if (A.Keyframes.Num() > 0)
                {
                    TArray<TSharedPtr<FJsonValue>> Kfs;
                    for (const FUISpecKeyframe& K : A.Keyframes)
                    {
                        TSharedPtr<FJsonObject> KO = MakeShared<FJsonObject>();
                        KO->SetNumberField(TEXT("time"),         K.Time);
                        KO->SetNumberField(TEXT("scalarValue"),  K.ScalarValue);
                        if (!K.Easing.IsNone()) KO->SetStringField(TEXT("easing"), K.Easing.ToString());
                        Kfs.Add(MakeShared<FJsonValueObject>(KO));
                    }
                    AObj->SetArrayField(TEXT("keyframes"), Kfs);
                }
                Arr.Add(MakeShared<FJsonValueObject>(AObj));
            }
            Spec->SetArrayField(TEXT("animations"), Arr);
        }

        if (R.Document.Root.IsValid())
        {
            Spec->SetObjectField(TEXT("rootWidget"), NodeToJson(*R.Document.Root));
        }

        Out->SetObjectField(TEXT("spec"), Spec);

        if (R.Errors.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Errs;
            for (const FUISpecError& E : R.Errors)
            {
                TSharedPtr<FJsonObject> EObj = MakeShared<FJsonObject>();
                EObj->SetStringField(TEXT("category"), E.Category.ToString());
                EObj->SetStringField(TEXT("message"),  E.Message);
                Errs.Add(MakeShared<FJsonValueObject>(EObj));
            }
            Out->SetArrayField(TEXT("errors"), Errs);
        }
        if (R.Warnings.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Warns;
            for (const FUISpecError& W : R.Warnings)
            {
                TSharedPtr<FJsonObject> WObj = MakeShared<FJsonObject>();
                WObj->SetStringField(TEXT("category"), W.Category.ToString());
                WObj->SetStringField(TEXT("message"),  W.Message);
                Warns.Add(MakeShared<FJsonValueObject>(WObj));
            }
            Out->SetArrayField(TEXT("warnings"), Warns);
        }
        return Out;
    }

    static FMonolithActionResult HandleDumpUISpec(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
        }

        FString AssetPath;
        if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        {
            return FMonolithActionResult::Error(
                TEXT("Missing or empty required param: asset_path"), -32602);
        }

        FUISpecSerializerInputs In;
        In.AssetPath = AssetPath;
        Params->TryGetBoolField(TEXT("emit_defaults"), In.bEmitDefaults);
        Params->TryGetStringField(TEXT("request_id"), In.RequestId);

        const FUISpecSerializerResult R = FUISpecSerializer::Dump(In);
        return FMonolithActionResult::Success(PackDumpResponse(R));
    }

    // ------------------------------------------------------------------
    // Phase 3 Item #18 (2026-05-16 UI Gap Audit) — ui::build_menu_from_spec
    //
    // Conservative MVP scope (matches the orchestrator-blessed Phase 2 Item #10
    // pattern: validator + registration FULL, multi-screen builder pipeline
    // STUB-with-clear-status). The action accepts a menu-shape document of:
    //
    //     {
    //       "layers":       [{ "id": "...", "screens": ["..."] }, ...],
    //       "screens":      [{ "id": "...", "asset_path": "/Game/UI/...",
    //                          "spec": <FUISpecDocument>?, "kind": "main_menu"|"settings"|... }, ...],
    //       "focus_table":  [{ "screen": "...", "target": "..." }, ...],
    //       "nav_overrides": [{ "screen": "...", "widget": "...",
    //                           "direction": "Up", "target": "..." }, ...]
    //     }
    //
    // For each screen that supplies an embedded `spec`, the MVP dispatches it
    // through the existing FUISpecBuilder pipeline (one call per screen). The
    // focus_table / nav_overrides / layer-aggregation surface is captured in
    // the response under `status="stub"` so the LLM can see the partial
    // implementation boundary without crashing on missing functionality. Modes
    // (`dry_run`, `treat_warnings_as_errors`, `raw_mode`, `overwrite`) are
    // forwarded onto every per-screen build call so the menu-level mode flag
    // propagates uniformly.
    //
    // Full implementation (deferred to a follow-up issue): build pre-walker
    // that emits the activatable-stack layer hierarchy first, threading
    // focus_table writes into the post-compile CDO pass on each screen WBP,
    // and applying nav_overrides via SetNavigationRuleExplicit.

    static FMonolithActionResult HandleBuildMenuFromSpec(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
        }

        FString RequestId;
        Params->TryGetStringField(TEXT("request_id"), RequestId);

        bool bDryRun = false, bTreatWarningsAsErrors = false, bRawMode = false, bOverwrite = true;
        Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
        Params->TryGetBoolField(TEXT("treat_warnings_as_errors"), bTreatWarningsAsErrors);
        Params->TryGetBoolField(TEXT("raw_mode"), bRawMode);
        Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);

        // ---- Validator (Phase 3 Item #18 MVP clause) -------------------------
        // The full FUISpecValidator extension lives in UISpecValidator.cpp;
        // the MVP wires the menu-shape structural checks inline here so the
        // action surface is unblocked without dragging FUISpecValidator into
        // a partial refactor.
        TArray<TSharedPtr<FJsonValue>> StructuralErrors;
        TArray<TSharedPtr<FJsonValue>> StructuralWarnings;

        auto AddError = [&StructuralErrors](const FString& Category, const FString& JsonPath, const FString& Message)
        {
            TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
            E->SetStringField(TEXT("category"), Category);
            E->SetStringField(TEXT("json_path"), JsonPath);
            E->SetStringField(TEXT("message"), Message);
            StructuralErrors.Add(MakeShared<FJsonValueObject>(E));
        };

        auto AddWarning = [&StructuralWarnings](const FString& Category, const FString& JsonPath, const FString& Message)
        {
            TSharedPtr<FJsonObject> W = MakeShared<FJsonObject>();
            W->SetStringField(TEXT("category"), Category);
            W->SetStringField(TEXT("json_path"), JsonPath);
            W->SetStringField(TEXT("message"), Message);
            StructuralWarnings.Add(MakeShared<FJsonValueObject>(W));
        };

        // screens[] is the load-bearing array. layers[]/focus_table[]/nav_overrides[]
        // are partially-supported in the MVP — caller-supplied entries are echoed
        // back so downstream tooling can surface "expected vs delivered".
        const TArray<TSharedPtr<FJsonValue>>* Screens = nullptr;
        if (!Params->TryGetArrayField(TEXT("screens"), Screens) || !Screens || Screens->Num() == 0)
        {
            AddError(TEXT("MenuShape"), TEXT("screens"),
                TEXT("`screens` array is required and must contain at least one entry. "
                     "Each entry needs {id, asset_path} and either an embedded `spec` "
                     "(FUISpecDocument) or a `kind` token for scaffolder dispatch (kind dispatch "
                     "is STUB in this MVP)."));
        }

        const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
        const bool bHasLayers = Params->TryGetArrayField(TEXT("layers"), Layers) && Layers && Layers->Num() > 0;

        const TArray<TSharedPtr<FJsonValue>>* FocusTable = nullptr;
        const bool bHasFocusTable = Params->TryGetArrayField(TEXT("focus_table"), FocusTable) && FocusTable;

        const TArray<TSharedPtr<FJsonValue>>* NavOverrides = nullptr;
        const bool bHasNavOverrides = Params->TryGetArrayField(TEXT("nav_overrides"), NavOverrides) && NavOverrides;

        if (bHasLayers || bHasFocusTable || bHasNavOverrides)
        {
            AddWarning(TEXT("MenuShape"), TEXT("layers|focus_table|nav_overrides"),
                TEXT("layers / focus_table / nav_overrides are accepted but applied as STUB in this MVP. "
                     "Per-screen `spec` builds run FULL via FUISpecBuilder. The cross-screen "
                     "aggregation surface (activatable-stack layer hierarchy, focus-table CDO writes, "
                     "nav-override propagation) is deferred to issue #3-18b. Caller-supplied entries "
                     "echo back in the response under `deferred_aggregation`."));
        }

        // Hard-fail on structural errors. The result payload mirrors
        // PackResponse so consumers can dispatch on bSuccess uniformly.
        if (StructuralErrors.Num() > 0)
        {
            TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
            Out->SetBoolField(TEXT("bSuccess"), false);
            if (!RequestId.IsEmpty()) Out->SetStringField(TEXT("request_id"), RequestId);
            Out->SetArrayField(TEXT("errors"), StructuralErrors);
            Out->SetArrayField(TEXT("warnings"), StructuralWarnings);
            Out->SetStringField(TEXT("status"), TEXT("validation_failed"));
            return FMonolithActionResult::Success(Out);
        }

        // ---- Per-screen dispatch into FUISpecBuilder -------------------------
        TArray<TSharedPtr<FJsonValue>> ScreenResults;
        int32 TotalCreated = 0, TotalModified = 0, TotalRemoved = 0;
        bool bAllSucceeded = true;

        for (int32 i = 0; i < Screens->Num(); ++i)
        {
            const TSharedPtr<FJsonValue>& V = (*Screens)[i];
            const TSharedPtr<FJsonObject>* ScreenObj = nullptr;
            if (!V.IsValid() || !V->TryGetObject(ScreenObj) || !ScreenObj)
            {
                AddError(TEXT("MenuShape"),
                    FString::Printf(TEXT("screens[%d]"), i),
                    TEXT("screen entry must be an object"));
                bAllSucceeded = false;
                continue;
            }

            FString ScreenId, ScreenAssetPath, ScreenKind;
            (*ScreenObj)->TryGetStringField(TEXT("id"), ScreenId);
            (*ScreenObj)->TryGetStringField(TEXT("asset_path"), ScreenAssetPath);
            (*ScreenObj)->TryGetStringField(TEXT("kind"), ScreenKind);

            if (ScreenAssetPath.IsEmpty())
            {
                AddError(TEXT("MenuShape"),
                    FString::Printf(TEXT("screens[%d].asset_path"), i),
                    TEXT("each screen entry requires `asset_path`"));
                bAllSucceeded = false;
                continue;
            }

            // Per-screen result block — populated below.
            TSharedPtr<FJsonObject> ScreenOut = MakeShared<FJsonObject>();
            ScreenOut->SetStringField(TEXT("id"), ScreenId);
            ScreenOut->SetStringField(TEXT("asset_path"), ScreenAssetPath);
            if (!ScreenKind.IsEmpty())
            {
                ScreenOut->SetStringField(TEXT("kind"), ScreenKind);
            }

            const TSharedPtr<FJsonObject>* EmbeddedSpec = nullptr;
            if (!(*ScreenObj)->TryGetObjectField(TEXT("spec"), EmbeddedSpec) || !EmbeddedSpec)
            {
                // Kind-only dispatch is the STUB surface. Surface it loudly so
                // the caller knows the per-screen WBP is NOT being built.
                ScreenOut->SetStringField(TEXT("status"), TEXT("stub"));
                ScreenOut->SetStringField(TEXT("reason"),
                    TEXT("screen has no embedded `spec` — kind-based scaffolder dispatch is deferred to issue #3-18b. "
                         "Pass a full FUISpecDocument under screens[N].spec to build this screen now, or call "
                         "scaffold_main_menu / scaffold_settings_panel_with_tabs / scaffold_pause_menu directly."));
                ScreenResults.Add(MakeShared<FJsonValueObject>(ScreenOut));
                continue;
            }

            FUISpecDocument Document;
            FUISpecValidationResult ParseValidation;
            if (!ParseDocument(*EmbeddedSpec, Document, ParseValidation))
            {
                ScreenOut->SetBoolField(TEXT("bSuccess"), false);
                ScreenOut->SetStringField(TEXT("status"), TEXT("parse_failed"));
                ScreenOut->SetStringField(TEXT("llm_report"), ParseValidation.ToLLMReport());
                ScreenResults.Add(MakeShared<FJsonValueObject>(ScreenOut));
                bAllSucceeded = false;
                continue;
            }

            FUISpecBuilderInputs In;
            In.Document  = &Document;
            In.AssetPath = ScreenAssetPath;
            In.bOverwrite             = bOverwrite;
            In.bDryRun                = bDryRun;
            In.bTreatWarningsAsErrors = bTreatWarningsAsErrors;
            In.bRawMode               = bRawMode;
            In.RequestId              = FString::Printf(TEXT("%s:%s"), *RequestId, *ScreenId);
            if (Document.bTreatWarningsAsErrors)
            {
                In.bTreatWarningsAsErrors = true;
            }

            const FUISpecBuilderResult R = FUISpecBuilder::Build(In);
            TotalCreated  += R.NodesCreated;
            TotalModified += R.NodesModified;
            TotalRemoved  += R.NodesRemoved;
            if (!R.bSuccess) bAllSucceeded = false;

            // Each screen reuses the shared PackResponse shape for symmetry
            // with build_ui_from_spec callers.
            TSharedPtr<FJsonObject> Packed = PackResponse(R, bDryRun);
            ScreenOut->SetObjectField(TEXT("build_result"), Packed);
            ScreenResults.Add(MakeShared<FJsonValueObject>(ScreenOut));
        }

        // ---- Deferred aggregation echo -------------------------------------
        // Capture caller-supplied layers / focus_table / nav_overrides so
        // downstream tooling can post-process them in user-space until the
        // full builder pipeline lands.
        TSharedPtr<FJsonObject> DeferredAgg = MakeShared<FJsonObject>();
        if (bHasLayers)        DeferredAgg->SetArrayField(TEXT("layers"),        *Layers);
        if (bHasFocusTable)    DeferredAgg->SetArrayField(TEXT("focus_table"),   *FocusTable);
        if (bHasNavOverrides)  DeferredAgg->SetArrayField(TEXT("nav_overrides"), *NavOverrides);

        // ---- Response ------------------------------------------------------
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("bSuccess"), bAllSucceeded && StructuralErrors.Num() == 0);
        if (!RequestId.IsEmpty()) Out->SetStringField(TEXT("request_id"), RequestId);
        Out->SetStringField(TEXT("status"),
            (bHasLayers || bHasFocusTable || bHasNavOverrides) ? TEXT("partial_stub") : TEXT("ok"));
        Out->SetArrayField(TEXT("screens"), ScreenResults);

        TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
        Counts->SetNumberField(TEXT("created"),  TotalCreated);
        Counts->SetNumberField(TEXT("modified"), TotalModified);
        Counts->SetNumberField(TEXT("removed"),  TotalRemoved);
        Out->SetObjectField(TEXT("aggregate_node_counts"), Counts);

        if (StructuralErrors.Num() > 0)  Out->SetArrayField(TEXT("errors"),   StructuralErrors);
        if (StructuralWarnings.Num() > 0) Out->SetArrayField(TEXT("warnings"), StructuralWarnings);
        if (DeferredAgg->Values.Num() > 0)
        {
            Out->SetObjectField(TEXT("deferred_aggregation"), DeferredAgg);
        }
        return FMonolithActionResult::Success(Out);
    }
} // namespace MonolithUI::SpecActionsInternal


// Action register entry-point — called once from FMonolithUIModule::StartupModule.
// Declaration lives in Actions/MonolithUISpecActions.h.
void MonolithUI::FSpecActions::Register(FMonolithToolRegistry& Registry)
{
    using namespace MonolithUI::SpecActionsInternal;

    Registry.RegisterAction(
        TEXT("ui"), TEXT("build_ui_from_spec"),
        TEXT("Phase H — transactional UISpec -> UWidgetBlueprint builder. Parses a FUISpecDocument JSON, "
             "validates it, dry-walks the spec tree, gets-or-creates the WBP at asset_path, pre-creates "
             "referenced styles, walks the tree, compiles, rebuilds the post-compile widget-id map, "
             "saves. Atomic: any failure between get-or-create and save rolls back the new asset (or "
             "cancels the in-place edit transaction). Supports dry_run, overwrite, treat_warnings_as_errors, "
             "raw_mode (per-write allowlist bypass), request_id (echoed back). Returns "
             "{ bSuccess, asset_path, request_id?, validation, node_counts, errors?, warnings?, diff? }."),
        FMonolithActionHandler::CreateStatic(&HandleBuildUIFromSpec),
        FParamSchemaBuilder()
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Long-package asset path, e.g. /Game/UI/MyMenu"))
            .Required(TEXT("spec"),       TEXT("object"), TEXT("FUISpecDocument JSON. Use ui::dump_ui_spec_schema for the shape."))
            .Optional(TEXT("overwrite"),  TEXT("boolean"), TEXT("Replace an existing WBP at asset_path. Default true."), TEXT("true"))
            .Optional(TEXT("dry_run"),    TEXT("boolean"), TEXT("Validate + walk + report a diff but do not commit. Default false."), TEXT("false"))
            .Optional(TEXT("treat_warnings_as_errors"), TEXT("boolean"), TEXT("Promote validator warnings to errors. Default false."), TEXT("false"))
            .Optional(TEXT("raw_mode"),   TEXT("boolean"), TEXT("Bypass the per-write allowlist gate. Default false."), TEXT("false"))
            .Optional(TEXT("request_id"), TEXT("string"),  TEXT("Caller-supplied UUID echoed back in the response."))
            .Build());

    Registry.RegisterAction(
        TEXT("ui"), TEXT("dump_ui_spec_schema"),
        TEXT("Phase H — JSON-Schema-style description of FUISpecDocument + live allowlist projection. "
             "Returns { schema_version, document_type, document_fields, node_fields, allowlist_by_type }. "
             "LLMs use this to build valid spec inputs without crawling our headers."),
        FMonolithActionHandler::CreateStatic(&HandleDumpUISpecSchema),
        FParamSchemaBuilder().Build());

    // Phase J: ui::dump_ui_spec — inverse of build_ui_from_spec. Read a live
    // UWidgetBlueprint and emit a FUISpecDocument JSON suitable for round-
    // tripping. Pure read; no asset mutation. Mirrors the build response shape
    // so action surfaces can compose dump + build uniformly.
    Registry.RegisterAction(
        TEXT("ui"), TEXT("dump_ui_spec"),
        TEXT("Phase J — UISpec roundtrip serializer. Reads the UWidgetBlueprint at asset_path "
             "and produces a FUISpecDocument JSON that, when fed back into ui::build_ui_from_spec, "
             "reconstructs the same widget tree (up to the documented lossy boundary -- style asset "
             "class refs serialise as paths; native graph bindings serialise by name; rich curve "
             "tangents serialise as Linear envelopes). Covers ALL stock UMG panel-slot types "
             "(Canvas / Vertical / Horizontal / Overlay / ScrollBox / Grid / UniformGrid / SizeBox / "
             "ScaleBox / WrapBox / WidgetSwitcher / Border) and EffectSurface drop/inner shadow "
             "arrays. Returns { bSuccess, asset_path, request_id?, nodes_visited, animations_captured, "
             "spec, errors?, warnings? } where `spec` is the FUISpecDocument JSON ready for build."),
        FMonolithActionHandler::CreateStatic(&HandleDumpUISpec),
        FParamSchemaBuilder()
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Long-package asset path of the WBP to read, e.g. /Game/UI/MyMenu"))
            .Optional(TEXT("emit_defaults"), TEXT("boolean"), TEXT("Include fields that match engine defaults. Default false."), TEXT("false"))
            .Optional(TEXT("request_id"), TEXT("string"), TEXT("Caller-supplied UUID echoed back in the response."))
            .Build());

    // Phase 3 Item #18 (2026-05-16 UI Gap Audit) — build_menu_from_spec.
    // Always-on (not WITH_COMMONUI-gated): the spec system is the source
    // of the shared menu document grammar; per-screen WBPs may use CommonUI
    // types but the dispatch surface itself is engine-side. MVP-STUB —
    // per-screen `spec` builds run FULL via FUISpecBuilder; cross-screen
    // aggregation (layers / focus_table / nav_overrides) is deferred to
    // issue #3-18b. Same modes as build_ui_from_spec.
    Registry.RegisterAction(
        TEXT("ui"), TEXT("build_menu_from_spec"),
        TEXT("Phase 3 Tier-3 — multi-screen menu document builder. Accepts {layers[], screens[], "
             "focus_table[], nav_overrides[]}. For each screens[N] entry that includes an embedded "
             "`spec` (FUISpecDocument), dispatches through the existing FUISpecBuilder pipeline "
             "(same atomicity + dry-run + strict-mode semantics as build_ui_from_spec). screens[N] "
             "entries without an embedded `spec` echo back as status='stub' (kind-based scaffolder "
             "dispatch deferred to issue #3-18b). layers / focus_table / nav_overrides are accepted, "
             "validated structurally, and echoed under `deferred_aggregation` so user-space tooling "
             "can post-process — the cross-screen activatable-stack hierarchy, focus-table CDO writes, "
             "and nav-override propagation are deferred. Modes (`dry_run`, `treat_warnings_as_errors`, "
             "`raw_mode`, `overwrite`) propagate to every per-screen build call. Returns "
             "{ bSuccess, status, screens[], aggregate_node_counts, errors?, warnings?, "
             "deferred_aggregation?, request_id? } where each screens[] entry includes a full "
             "build_result object (same shape as build_ui_from_spec)."),
        FMonolithActionHandler::CreateStatic(&HandleBuildMenuFromSpec),
        FParamSchemaBuilder()
            .Required(TEXT("screens"), TEXT("array"),
                TEXT("[{ id, asset_path, spec?, kind? }, ...] — each entry triggers a per-screen FUISpecBuilder "
                     "dispatch when `spec` is set. Without `spec`, the entry echoes status='stub'."))
            .Optional(TEXT("layers"), TEXT("array"),
                TEXT("[{ id, screens[] }, ...] — activatable-stack layer hierarchy. STUB (echoed back, not applied)."))
            .Optional(TEXT("focus_table"), TEXT("array"),
                TEXT("[{ screen, target }, ...] — per-screen DesiredFocusTargetName CDO writes. STUB (echoed back)."))
            .Optional(TEXT("nav_overrides"), TEXT("array"),
                TEXT("[{ screen, widget, direction, target }, ...] — per-widget nav overrides. STUB (echoed back)."))
            .Optional(TEXT("overwrite"), TEXT("boolean"),
                TEXT("Replace existing WBPs at each screen's asset_path. Default true."), TEXT("true"))
            .Optional(TEXT("dry_run"), TEXT("boolean"),
                TEXT("Validate + walk each per-screen spec; do not commit. Default false."), TEXT("false"))
            .Optional(TEXT("treat_warnings_as_errors"), TEXT("boolean"),
                TEXT("Promote validator warnings to errors. Default false."), TEXT("false"))
            .Optional(TEXT("raw_mode"), TEXT("boolean"),
                TEXT("Bypass the per-write allowlist gate on every per-screen build. Default false."), TEXT("false"))
            .Optional(TEXT("request_id"), TEXT("string"),
                TEXT("Caller-supplied UUID echoed back; per-screen builds receive '<request_id>:<screen.id>'."))
            .Build());
}
