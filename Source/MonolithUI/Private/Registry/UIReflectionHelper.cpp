// Copyright tumourlove. All Rights Reserved.
// UIReflectionHelper.cpp
//
// Implementation: allowlist gate -> path resolve (cache-or-walk) -> property
// kind dispatch -> JSON shape parse -> property write.
//
// Design note: the hand-rolled struct parsers (FVector2D, FLinearColor,
// FMargin, FVector4, FSlateColor) accept multiple JSON shapes per the
// orchestrator brief. Each returns a typed value; on parse failure we return
// a `ParseFailed` result rather than crashing or silently zeroing.

#include "Registry/UIReflectionHelper.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Layout/Margin.h"
#include "Math/Color.h"
#include "Math/Vector2D.h"
#include "Math/Vector4.h"
#include "Misc/Char.h"
#include "MonolithUICommon.h"
#include "Registry/MonolithUIRegistrySubsystem.h"
#include "Registry/UIPropertyAllowlist.h"
#include "Registry/UIPropertyPathCache.h"
#include "Registry/UITypeRegistry.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/SlateColor.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/Field.h"
#include "UObject/UnrealType.h"

namespace
{
    // ------------------------------------------------------------------
    // JSON shape probes
    // ------------------------------------------------------------------

    // True if Value is a JSON array of at least MinNum numbers.
    bool IsNumericArray(const TSharedPtr<FJsonValue>& Value, int32 MinNum)
    {
        if (!Value.IsValid() || Value->Type != EJson::Array)
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
        if (Arr.Num() < MinNum)
        {
            return false;
        }
        for (int32 i = 0; i < MinNum; ++i)
        {
            if (!Arr[i].IsValid() || Arr[i]->Type != EJson::Number)
            {
                return false;
            }
        }
        return true;
    }

    // Read array slot as double, defaulted if out of range / non-number.
    double ArrayNum(const TArray<TSharedPtr<FJsonValue>>& Arr, int32 Idx, double Default)
    {
        if (!Arr.IsValidIndex(Idx) || !Arr[Idx].IsValid() || Arr[Idx]->Type != EJson::Number)
        {
            return Default;
        }
        return Arr[Idx]->AsNumber();
    }

    // Read object field as double with default.
    double ObjNum(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, double Default)
    {
        double Out = Default;
        if (Obj.IsValid())
        {
            Obj->TryGetNumberField(Field, Out);
        }
        return Out;
    }

    // True if Text is a number we are willing to coerce.
    //
    // FString::IsNumeric routes to TCString::IsNumeric (CString.h:139-164), which
    // skips a leading '-'/'+' and then accepts an EMPTY remainder — so "-", "+", ""
    // and "." all report numeric and would silently coerce to 0.0. Require at least
    // one actual digit so those forms fall through to the caller's failure path.
    bool IsStrictNumericText(const FString& Text)
    {
        if (!Text.IsNumeric())
        {
            return false;
        }
        for (const TCHAR Ch : Text)
        {
            if (FChar::IsDigit(Ch))
            {
                return true;
            }
        }
        return false;
    }

    // ------------------------------------------------------------------
    // String coercion — ParseMargin / ParseVector4 ONLY
    // ------------------------------------------------------------------

    // The `value` param is declared `any`, but the MCP proxy and the offline
    // CLI serialise nested params to text, so a caller-supplied object/array/
    // number still lands here as EJson::String. Re-hydrate those forms so the
    // documented shapes are reachable; anything else is returned untouched.
    // Deliberately NOT called from the other parsers — ParseVector2D and
    // ParseLinearColor own their string handling ("x,y" / "#RRGGBBAA").
    TSharedPtr<FJsonValue> CoerceStringToJson(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid() || Value->Type != EJson::String)
        {
            return Value;
        }

        const FString S = Value->AsString().TrimStartAndEnd();
        if (S.IsEmpty())
        {
            return Value;
        }

        // Braced / bracketed text -> real JSON object or array.
        if ((S.StartsWith(TEXT("{")) && S.EndsWith(TEXT("}"))) ||
            (S.StartsWith(TEXT("[")) && S.EndsWith(TEXT("]"))))
        {
            TSharedPtr<FJsonValue> Parsed;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(S);
            if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
            {
                return Parsed;
            }
            return Value;
        }

        // Bare numeric text -> JSON number (feeds Margin's uniform-scalar path).
        if (IsStrictNumericText(S))
        {
            return MakeShared<FJsonValueNumber>(FCString::Atod(*S));
        }

        return Value;
    }

    // Split "a,b,c,..." text into the first MinNum doubles. Mirrors the inline
    // comma handling in ParseVector2D; non-numeric parts fail rather than
    // silently writing zeros.
    bool ParseCommaNumbers(const FString& Text, int32 MinNum, TArray<double>& Out)
    {
        Out.Reset();

        TArray<FString> Parts;
        // bInCullEmpty defaults to true, which would silently swallow empty tokens —
        // "1,,2,3,4" would arrive as the 4-element (1,2,3,4) and pass the count check
        // below. Keep the empties so a malformed list is rejected, not renumbered.
        Text.ParseIntoArray(Parts, TEXT(","), /*bInCullEmpty=*/false);
        if (Parts.Num() < MinNum)
        {
            return false;
        }
        for (int32 i = 0; i < MinNum; ++i)
        {
            const FString Part = Parts[i].TrimStartAndEnd();
            if (!IsStrictNumericText(Part))
            {
                return false;
            }
            Out.Add(FCString::Atod(*Part));
        }
        return true;
    }

    // ------------------------------------------------------------------
    // Hand-rolled struct parsers
    // ------------------------------------------------------------------

    // FVector2D — object{x,y} OR array[x,y] OR "x,y" string.
    bool ParseVector2D(const TSharedPtr<FJsonValue>& Value, FVector2D& Out)
    {
        if (!Value.IsValid()) return false;

        if (Value->Type == EJson::Array)
        {
            if (!IsNumericArray(Value, 2)) return false;
            const auto& Arr = Value->AsArray();
            Out = FVector2D(ArrayNum(Arr, 0, 0.0), ArrayNum(Arr, 1, 0.0));
            return true;
        }
        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Obj = Value->AsObject();
            Out = FVector2D(ObjNum(Obj, TEXT("x"), 0.0), ObjNum(Obj, TEXT("y"), 0.0));
            return true;
        }
        if (Value->Type == EJson::String)
        {
            FString S = Value->AsString();
            TArray<FString> Parts;
            S.ParseIntoArray(Parts, TEXT(","));
            if (Parts.Num() >= 2)
            {
                Out = FVector2D(FCString::Atod(*Parts[0].TrimStartAndEnd()),
                                FCString::Atod(*Parts[1].TrimStartAndEnd()));
                return true;
            }
        }
        return false;
    }

    // FLinearColor — "#RRGGBBAA" OR array[r,g,b,a] OR object{r,g,b,a}.
    // Routes hex-string parses through MonolithUI::ParseColor (degamma path).
    bool ParseLinearColor(const TSharedPtr<FJsonValue>& Value, FLinearColor& Out)
    {
        if (!Value.IsValid()) return false;

        if (Value->Type == EJson::String)
        {
            FLinearColor Parsed;
            if (MonolithUI::TryParseColor(Value->AsString(), Parsed))
            {
                Out = Parsed;
                return true;
            }
            // Fall back to legacy ParseColor (degamma path) for # forms only —
            // it returns White silently on garbage so we wrap it explicitly.
            const FString S = Value->AsString().TrimStartAndEnd();
            if (S.StartsWith(TEXT("#")))
            {
                Out = MonolithUI::ParseColor(S);
                return true;
            }
            return false;
        }
        if (Value->Type == EJson::Array)
        {
            if (!IsNumericArray(Value, 3)) return false;
            const auto& Arr = Value->AsArray();
            Out = FLinearColor(
                (float)ArrayNum(Arr, 0, 0.0),
                (float)ArrayNum(Arr, 1, 0.0),
                (float)ArrayNum(Arr, 2, 0.0),
                (float)ArrayNum(Arr, 3, 1.0));
            return true;
        }
        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Obj = Value->AsObject();
            Out = FLinearColor(
                (float)ObjNum(Obj, TEXT("r"), 0.0),
                (float)ObjNum(Obj, TEXT("g"), 0.0),
                (float)ObjNum(Obj, TEXT("b"), 0.0),
                (float)ObjNum(Obj, TEXT("a"), 1.0));
            return true;
        }
        return false;
    }

    // FMargin — object{left,top,right,bottom} OR array[l,t,r,b] OR "l,t,r,b"
    // OR scalar (uniform). Stringified object/array/number forms are rehydrated
    // by CoerceStringToJson first.
    bool ParseMargin(const TSharedPtr<FJsonValue>& InValue, FMargin& Out)
    {
        const TSharedPtr<FJsonValue> Value = CoerceStringToJson(InValue);
        if (!Value.IsValid()) return false;

        if (Value->Type == EJson::Number)
        {
            const float V = (float)Value->AsNumber();
            Out = FMargin(V);
            return true;
        }
        if (Value->Type == EJson::Array)
        {
            if (!IsNumericArray(Value, 4)) return false;
            const auto& Arr = Value->AsArray();
            Out = FMargin(
                (float)ArrayNum(Arr, 0, 0.0),
                (float)ArrayNum(Arr, 1, 0.0),
                (float)ArrayNum(Arr, 2, 0.0),
                (float)ArrayNum(Arr, 3, 0.0));
            return true;
        }
        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Obj = Value->AsObject();
            Out = FMargin(
                (float)ObjNum(Obj, TEXT("left"),   0.0),
                (float)ObjNum(Obj, TEXT("top"),    0.0),
                (float)ObjNum(Obj, TEXT("right"),  0.0),
                (float)ObjNum(Obj, TEXT("bottom"), 0.0));
            return true;
        }
        if (Value->Type == EJson::String)
        {
            // "l,t,r,b" text form. A bare numeric string already became a
            // Number above, so only multi-part text reaches here.
            TArray<double> Nums;
            if (ParseCommaNumbers(Value->AsString(), 4, Nums))
            {
                Out = FMargin(
                    (float)Nums[0],
                    (float)Nums[1],
                    (float)Nums[2],
                    (float)Nums[3]);
                return true;
            }
        }
        return false;
    }

    // FVector4 — array[x,y,z,w] OR object{x,y,z,w} OR "x,y,z,w". FVector4 is
    // LWC (double). Stringified object/array forms are rehydrated by
    // CoerceStringToJson first.
    bool ParseVector4(const TSharedPtr<FJsonValue>& InValue, FVector4& Out)
    {
        const TSharedPtr<FJsonValue> Value = CoerceStringToJson(InValue);
        if (!Value.IsValid()) return false;

        if (Value->Type == EJson::Array)
        {
            if (!IsNumericArray(Value, 4)) return false;
            const auto& Arr = Value->AsArray();
            Out = FVector4(
                ArrayNum(Arr, 0, 0.0),
                ArrayNum(Arr, 1, 0.0),
                ArrayNum(Arr, 2, 0.0),
                ArrayNum(Arr, 3, 0.0));
            return true;
        }
        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Obj = Value->AsObject();
            Out = FVector4(
                ObjNum(Obj, TEXT("x"), 0.0),
                ObjNum(Obj, TEXT("y"), 0.0),
                ObjNum(Obj, TEXT("z"), 0.0),
                ObjNum(Obj, TEXT("w"), 0.0));
            return true;
        }
        if (Value->Type == EJson::String)
        {
            // "x,y,z,w" text form.
            TArray<double> Nums;
            if (ParseCommaNumbers(Value->AsString(), 4, Nums))
            {
                Out = FVector4(Nums[0], Nums[1], Nums[2], Nums[3]);
                return true;
            }
        }
        return false;
    }

    // FSlateColor — wraps an FLinearColor in UseColor_Specified mode. Accepts
    // every shape ParseLinearColor accepts.
    bool ParseSlateColor(const TSharedPtr<FJsonValue>& Value, FSlateColor& Out)
    {
        FLinearColor Parsed;
        if (!ParseLinearColor(Value, Parsed))
        {
            return false;
        }
        Out = FSlateColor(Parsed);
        return true;
    }

    // ------------------------------------------------------------------
    // FProperty value writers
    // ------------------------------------------------------------------

    // Write the leaf value into PropAddr. Returns success + error reason.
    // PropAddr is the *value* address (already stepped through any container).
    bool WriteValueToProperty(
        FProperty* LeafProp,
        void* PropAddr,
        const TSharedPtr<FJsonValue>& Value,
        FString& OutFailureReason,
        FString& OutDetail)
    {
        if (!LeafProp || !PropAddr || !Value.IsValid())
        {
            OutFailureReason = TEXT("ParseFailed");
            OutDetail = TEXT("null property, address, or value");
            return false;
        }

        // Bool — accept bool, number 0|1, or "true"|"false" string.
        if (FBoolProperty* BoolProp = CastField<FBoolProperty>(LeafProp))
        {
            bool bVal = false;
            if (Value->Type == EJson::Boolean) { bVal = Value->AsBool(); }
            else if (Value->Type == EJson::Number) { bVal = (Value->AsNumber() != 0.0); }
            else if (Value->Type == EJson::String)
            {
                const FString S = Value->AsString().TrimStartAndEnd().ToLower();
                if (S == TEXT("true") || S == TEXT("1")) bVal = true;
                else if (S == TEXT("false") || S == TEXT("0")) bVal = false;
                else { OutFailureReason = TEXT("ParseFailed"); OutDetail = TEXT("expected bool"); return false; }
            }
            else { OutFailureReason = TEXT("TypeMismatch"); OutDetail = TEXT("expected bool"); return false; }

            BoolProp->SetPropertyValue(PropAddr, bVal);
            return true;
        }

        // Enum (FEnumProperty) — accept enum name string OR underlying number.
        if (FEnumProperty* EnumProp = CastField<FEnumProperty>(LeafProp))
        {
            UEnum* Enum = EnumProp->GetEnum();
            FNumericProperty* Underlying = EnumProp->GetUnderlyingProperty();
            if (!Enum || !Underlying)
            {
                OutFailureReason = TEXT("TypeMismatch");
                OutDetail = TEXT("enum property has no UEnum");
                return false;
            }
            int64 IntVal = 0;
            if (Value->Type == EJson::Number) { IntVal = (int64)Value->AsNumber(); }
            else if (Value->Type == EJson::String)
            {
                const FString S = Value->AsString().TrimStartAndEnd();
                IntVal = Enum->GetValueByNameString(S);
                if (IntVal == INDEX_NONE)
                {
                    // Try short-name resolution (no enum-class prefix).
                    IntVal = Enum->GetValueByName(FName(*S));
                }
                if (IntVal == INDEX_NONE)
                {
                    OutFailureReason = TEXT("ParseFailed");
                    OutDetail = FString::Printf(TEXT("'%s' is not a valid enum name on %s"), *S, *Enum->GetName());
                    return false;
                }
            }
            else { OutFailureReason = TEXT("TypeMismatch"); OutDetail = TEXT("expected enum name or number"); return false; }

            Underlying->SetIntPropertyValue(PropAddr, IntVal);
            return true;
        }

        // Byte property — may or may not be enum-tagged.
        if (FByteProperty* ByteProp = CastField<FByteProperty>(LeafProp))
        {
            int64 IntVal = 0;
            if (Value->Type == EJson::Number) { IntVal = (int64)Value->AsNumber(); }
            else if (Value->Type == EJson::String && ByteProp->Enum)
            {
                const FString S = Value->AsString().TrimStartAndEnd();
                IntVal = ByteProp->Enum->GetValueByNameString(S);
                if (IntVal == INDEX_NONE) { IntVal = ByteProp->Enum->GetValueByName(FName(*S)); }
                if (IntVal == INDEX_NONE)
                {
                    OutFailureReason = TEXT("ParseFailed");
                    OutDetail = FString::Printf(TEXT("'%s' is not a valid enum name on %s"), *S, *ByteProp->Enum->GetName());
                    return false;
                }
            }
            else { OutFailureReason = TEXT("TypeMismatch"); OutDetail = TEXT("expected byte/enum value"); return false; }

            ByteProp->SetIntPropertyValue(PropAddr, IntVal);
            return true;
        }

        // Numeric — int / float / double — accept number or numeric string.
        if (FNumericProperty* NumProp = CastField<FNumericProperty>(LeafProp))
        {
            double DVal = 0.0;
            if (Value->Type == EJson::Number) { DVal = Value->AsNumber(); }
            else if (Value->Type == EJson::String) { DVal = FCString::Atod(*Value->AsString()); }
            else if (Value->Type == EJson::Boolean) { DVal = Value->AsBool() ? 1.0 : 0.0; }
            else { OutFailureReason = TEXT("TypeMismatch"); OutDetail = TEXT("expected number"); return false; }

            if (NumProp->IsFloatingPoint())
            {
                NumProp->SetFloatingPointPropertyValue(PropAddr, DVal);
            }
            else
            {
                NumProp->SetIntPropertyValue(PropAddr, (int64)DVal);
            }
            return true;
        }

        // String.
        if (FStrProperty* StrProp = CastField<FStrProperty>(LeafProp))
        {
            FString Out;
            if (Value->Type == EJson::String) { Out = Value->AsString(); }
            else if (Value->Type == EJson::Number) { Out = FString::SanitizeFloat(Value->AsNumber()); }
            else if (Value->Type == EJson::Boolean) { Out = Value->AsBool() ? TEXT("true") : TEXT("false"); }
            else { OutFailureReason = TEXT("TypeMismatch"); OutDetail = TEXT("expected string"); return false; }

            StrProp->SetPropertyValue(PropAddr, Out);
            return true;
        }

        // Name.
        if (FNameProperty* NameProp = CastField<FNameProperty>(LeafProp))
        {
            if (Value->Type != EJson::String)
            {
                OutFailureReason = TEXT("TypeMismatch");
                OutDetail = TEXT("expected string for FName");
                return false;
            }
            NameProp->SetPropertyValue(PropAddr, FName(*Value->AsString()));
            return true;
        }

        // Object — accept asset path string. Empty string clears.
        if (FObjectProperty* ObjProp = CastField<FObjectProperty>(LeafProp))
        {
            if (Value->Type != EJson::String)
            {
                OutFailureReason = TEXT("TypeMismatch");
                OutDetail = TEXT("expected asset path string for object property");
                return false;
            }
            const FString Path = Value->AsString().TrimStartAndEnd();
            UObject* Asset = Path.IsEmpty()
                ? nullptr
                : StaticLoadObject(ObjProp->PropertyClass, nullptr, *Path);
            if (!Path.IsEmpty() && !Asset)
            {
                OutFailureReason = TEXT("ParseFailed");
                OutDetail = FString::Printf(TEXT("could not load %s as %s"),
                    *Path, *ObjProp->PropertyClass->GetName());
                return false;
            }
            ObjProp->SetObjectPropertyValue(PropAddr, Asset);
            return true;
        }

        // Struct — dispatch on Struct->GetFName().
        if (FStructProperty* StructProp = CastField<FStructProperty>(LeafProp))
        {
            const FName StructName = StructProp->Struct ? StructProp->Struct->GetFName() : NAME_None;

            if (StructName == TEXT("Vector2D"))
            {
                FVector2D Parsed;
                if (!ParseVector2D(Value, Parsed))
                {
                    OutFailureReason = TEXT("ParseFailed");
                    OutDetail = TEXT("expected Vector2D shape: object{x,y}, [x,y], or 'x,y'");
                    return false;
                }
                *(FVector2D*)PropAddr = Parsed;
                return true;
            }
            if (StructName == TEXT("LinearColor"))
            {
                FLinearColor Parsed;
                if (!ParseLinearColor(Value, Parsed))
                {
                    OutFailureReason = TEXT("ParseFailed");
                    OutDetail = TEXT("expected LinearColor: '#RRGGBBAA', [r,g,b,a], or {r,g,b,a}");
                    return false;
                }
                *(FLinearColor*)PropAddr = Parsed;
                return true;
            }
            if (StructName == TEXT("Margin"))
            {
                FMargin Parsed;
                if (!ParseMargin(Value, Parsed))
                {
                    OutFailureReason = TEXT("ParseFailed");
                    OutDetail = TEXT("expected Margin: scalar, [l,t,r,b], {left,top,right,bottom}, \"l,t,r,b\", or any of these as a JSON string");
                    return false;
                }
                *(FMargin*)PropAddr = Parsed;
                return true;
            }
            if (StructName == TEXT("Vector4"))
            {
                FVector4 Parsed;
                if (!ParseVector4(Value, Parsed))
                {
                    OutFailureReason = TEXT("ParseFailed");
                    OutDetail = TEXT("expected Vector4: [x,y,z,w], {x,y,z,w}, \"x,y,z,w\", or any of these as a JSON string");
                    return false;
                }
                *(FVector4*)PropAddr = Parsed;
                return true;
            }
            if (StructName == TEXT("SlateColor"))
            {
                FSlateColor Parsed;
                if (!ParseSlateColor(Value, Parsed))
                {
                    OutFailureReason = TEXT("ParseFailed");
                    OutDetail = TEXT("expected SlateColor (LinearColor shape)");
                    return false;
                }
                *(FSlateColor*)PropAddr = Parsed;
                return true;
            }

            // Unknown struct — fall back to ImportText_Direct on the JSON
            // value's stringified form. Lossy but better than refusing every
            // unrecognised struct.
            const FString AsString = Value->AsString();
            if (LeafProp->ImportText_Direct(*AsString, PropAddr, nullptr, PPF_None))
            {
                return true;
            }
            OutFailureReason = TEXT("ParseFailed");
            OutDetail = FString::Printf(TEXT("no parser for struct %s and ImportText_Direct rejected '%s'"),
                *StructName.ToString(), *AsString);
            return false;
        }

        // Array / map / fallback — try ImportText_Direct on the stringified value.
        // The bare ImportText_Direct path matches the legacy raw-mode behaviour.
        const FString AsString = Value->AsString();
        if (LeafProp->ImportText_Direct(*AsString, PropAddr, nullptr, PPF_None))
        {
            return true;
        }

        OutFailureReason = TEXT("ParseFailed");
        OutDetail = FString::Printf(TEXT("no handler for property class %s; ImportText_Direct rejected '%s'"),
            *LeafProp->GetClass()->GetName(), *AsString);
        return false;
    }
}

// ============================================================================
// FUIReflectionHelper
// ============================================================================

FUIReflectionHelper::FUIReflectionHelper(FUIPropertyPathCache* InCache, const FUIPropertyAllowlist* InAllowlist)
    : Cache(InCache)
    , Allowlist(InAllowlist)
{
}

FUIReflectionApplyResult FUIReflectionHelper::Apply(
    UObject* Root,
    const FString& PropertyPath,
    const TSharedPtr<FJsonValue>& Value,
    bool bRawMode)
{
    FUIReflectionApplyResult Result;
    Result.PropertyPath = PropertyPath;

    if (!Root)
    {
        Result.bSuccess = false;
        Result.FailureReason = TEXT("PropertyNotFound");
        Result.Detail = TEXT("null root object");
        return Result;
    }
    if (PropertyPath.IsEmpty())
    {
        Result.bSuccess = false;
        Result.FailureReason = TEXT("PropertyNotFound");
        Result.Detail = TEXT("empty property path");
        return Result;
    }

    // Allowlist gate (front of pipeline). Bypassed when bRawMode == true.
    if (!bRawMode)
    {
        if (!Allowlist)
        {
            Result.bSuccess = false;
            Result.FailureReason = TEXT("NotInAllowlist");
            Result.Detail = TEXT("no allowlist supplied; supply one or pass bRawMode=true");
            return Result;
        }

        const FName WidgetToken = MonolithUI::MakeTokenFromClassName(Root->GetClass());
        if (!Allowlist->IsAllowed(WidgetToken, PropertyPath))
        {
            Result.bSuccess = false;
            Result.FailureReason = TEXT("NotInAllowlist");
            Result.Detail = WidgetToken.ToString();
            return Result;
        }
    }

    // Resolve the property chain. Cache when available; otherwise direct walk.
    FUIPropertyPathChain Chain;
    if (Cache)
    {
        UClass* RootClass = Root->GetClass();
        const FName RootStructName = RootClass->GetFName();
        // Pass the live UClass directly: FName-by-FindObject(nullptr,...) would
        // probe the transient package and miss UClasses, which live in
        // /Script/<Module>/ packages. The FName remains the cache key.
        Chain = Cache->Get(RootStructName, RootClass, PropertyPath);
    }
    else
    {
        // Same routine the cache would call internally — kept inline so the
        // no-cache fallback exercises identical logic.
        TArray<FString> Segments;
        PropertyPath.ParseIntoArray(Segments, TEXT("."), /*bCullEmpty=*/true);
        UStruct* CurrentStruct = Root->GetClass();
        bool bOk = (Segments.Num() > 0 && CurrentStruct != nullptr);
        for (int32 i = 0; bOk && i < Segments.Num(); ++i)
        {
            FProperty* Prop = CurrentStruct ? CurrentStruct->FindPropertyByName(FName(*Segments[i])) : nullptr;
            if (!Prop) { bOk = false; break; }
            Chain.PropertyChain.Add(Prop);
            Chain.StructChain.Add(CurrentStruct);
            const bool bIsLast = (i == Segments.Num() - 1);
            if (!bIsLast)
            {
                if (FStructProperty* SP = CastField<FStructProperty>(Prop)) { CurrentStruct = SP->Struct; }
                else if (FObjectProperty* OP = CastField<FObjectProperty>(Prop)) { CurrentStruct = OP->PropertyClass; }
                else { bOk = false; break; }
            }
        }
        Chain.bValid = bOk;
    }

    if (!Chain.bValid || Chain.PropertyChain.Num() == 0)
    {
        Result.bSuccess = false;
        Result.FailureReason = TEXT("PropertyNotFound");
        Result.Detail = FString::Printf(TEXT("path '%s' did not resolve on %s"),
            *PropertyPath, *Root->GetClass()->GetName());
        return Result;
    }

    // Walk the chain to compute the leaf value address. The first hop is on
    // the Root UObject; subsequent hops descend into struct or object payloads.
    void* CurrentAddr = Root;
    for (int32 i = 0; i < Chain.PropertyChain.Num() - 1; ++i)
    {
        FProperty* Prop = Chain.PropertyChain[i];
        void* PropValueAddr = Prop->ContainerPtrToValuePtr<void>(CurrentAddr);

        if (FStructProperty* SP = CastField<FStructProperty>(Prop))
        {
            CurrentAddr = PropValueAddr; // The struct payload is the next container.
        }
        else if (FObjectProperty* OP = CastField<FObjectProperty>(Prop))
        {
            UObject* SubObj = OP->GetObjectPropertyValue(PropValueAddr);
            if (!SubObj)
            {
                Result.bSuccess = false;
                Result.FailureReason = TEXT("PropertyNotFound");
                Result.Detail = FString::Printf(TEXT("intermediate object property '%s' is null"),
                    *Prop->GetName());
                return Result;
            }
            CurrentAddr = SubObj;
        }
        else
        {
            // Should have been caught during chain resolution.
            Result.bSuccess = false;
            Result.FailureReason = TEXT("TypeMismatch");
            Result.Detail = FString::Printf(TEXT("intermediate property '%s' is not navigable"),
                *Prop->GetName());
            return Result;
        }
    }

    FProperty* LeafProp = Chain.PropertyChain.Last();
    void* LeafAddr = LeafProp->ContainerPtrToValuePtr<void>(CurrentAddr);

    FString FailureReason;
    FString Detail;
    if (!WriteValueToProperty(LeafProp, LeafAddr, Value, FailureReason, Detail))
    {
        Result.bSuccess = false;
        Result.FailureReason = FailureReason;
        Result.Detail = Detail;
        return Result;
    }

    Result.bSuccess = true;
    return Result;
}

// Phase J -- read-side dispatch table. Mirror of WriteValueToProperty above.
// Returns a FJsonValue shaped per the property kind. ContainerAddr is the
// container that the leaf property lives on (UObject for top-level, struct
// payload for nested struct properties).
namespace
{
    TSharedPtr<FJsonValue> ReadValueFromProperty(
        const FProperty* LeafProp,
        const void* PropAddr,
        FString& OutFailureReason,
        FString& OutDetail)
    {
        if (!LeafProp || !PropAddr)
        {
            OutFailureReason = TEXT("ParseFailed");
            OutDetail = TEXT("null property or address");
            return MakeShared<FJsonValueNull>();
        }

        // Bool.
        if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(LeafProp))
        {
            return MakeShared<FJsonValueBoolean>(BoolProp->GetPropertyValue(PropAddr));
        }

        // Enum (FEnumProperty) -- emit as string (enum name).
        if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(LeafProp))
        {
            const UEnum* Enum = EnumProp->GetEnum();
            const FNumericProperty* Underlying = EnumProp->GetUnderlyingProperty();
            if (!Enum || !Underlying)
            {
                OutFailureReason = TEXT("TypeMismatch");
                OutDetail = TEXT("enum property has no UEnum");
                return MakeShared<FJsonValueNull>();
            }
            const int64 IntVal = Underlying->GetSignedIntPropertyValue(PropAddr);
            const FString Name = Enum->GetNameStringByValue(IntVal);
            return MakeShared<FJsonValueString>(Name);
        }

        // Byte property -- enum-name when tagged, number otherwise.
        if (const FByteProperty* ByteProp = CastField<FByteProperty>(LeafProp))
        {
            const int64 IntVal = ByteProp->GetSignedIntPropertyValue(PropAddr);
            if (ByteProp->Enum)
            {
                const FString Name = ByteProp->Enum->GetNameStringByValue(IntVal);
                if (!Name.IsEmpty())
                {
                    return MakeShared<FJsonValueString>(Name);
                }
            }
            return MakeShared<FJsonValueNumber>((double)IntVal);
        }

        // Numeric.
        if (const FNumericProperty* NumProp = CastField<FNumericProperty>(LeafProp))
        {
            if (NumProp->IsFloatingPoint())
            {
                return MakeShared<FJsonValueNumber>(NumProp->GetFloatingPointPropertyValue(PropAddr));
            }
            return MakeShared<FJsonValueNumber>((double)NumProp->GetSignedIntPropertyValue(PropAddr));
        }

        // String.
        if (const FStrProperty* StrProp = CastField<FStrProperty>(LeafProp))
        {
            return MakeShared<FJsonValueString>(StrProp->GetPropertyValue(PropAddr));
        }

        // Name.
        if (const FNameProperty* NameProp = CastField<FNameProperty>(LeafProp))
        {
            return MakeShared<FJsonValueString>(NameProp->GetPropertyValue(PropAddr).ToString());
        }

        // Object -- emit asset path.
        if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(LeafProp))
        {
            const UObject* Asset = ObjProp->GetObjectPropertyValue(PropAddr);
            if (!Asset)
            {
                return MakeShared<FJsonValueString>(FString());
            }
            return MakeShared<FJsonValueString>(Asset->GetPathName());
        }

        // Struct -- dispatch on Struct->GetFName().
        if (const FStructProperty* StructProp = CastField<FStructProperty>(LeafProp))
        {
            const FName StructName = StructProp->Struct ? StructProp->Struct->GetFName() : NAME_None;

            if (StructName == TEXT("Vector2D"))
            {
                const FVector2D& V = *(const FVector2D*)PropAddr;
                TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
                Obj->SetNumberField(TEXT("x"), V.X);
                Obj->SetNumberField(TEXT("y"), V.Y);
                return MakeShared<FJsonValueObject>(Obj);
            }
            if (StructName == TEXT("LinearColor"))
            {
                const FLinearColor& C = *(const FLinearColor*)PropAddr;
                TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
                Obj->SetNumberField(TEXT("r"), C.R);
                Obj->SetNumberField(TEXT("g"), C.G);
                Obj->SetNumberField(TEXT("b"), C.B);
                Obj->SetNumberField(TEXT("a"), C.A);
                return MakeShared<FJsonValueObject>(Obj);
            }
            if (StructName == TEXT("Margin"))
            {
                const FMargin& M = *(const FMargin*)PropAddr;
                TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
                Obj->SetNumberField(TEXT("left"),   M.Left);
                Obj->SetNumberField(TEXT("top"),    M.Top);
                Obj->SetNumberField(TEXT("right"),  M.Right);
                Obj->SetNumberField(TEXT("bottom"), M.Bottom);
                return MakeShared<FJsonValueObject>(Obj);
            }
            if (StructName == TEXT("Vector4"))
            {
                const FVector4& V = *(const FVector4*)PropAddr;
                TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
                Obj->SetNumberField(TEXT("x"), V.X);
                Obj->SetNumberField(TEXT("y"), V.Y);
                Obj->SetNumberField(TEXT("z"), V.Z);
                Obj->SetNumberField(TEXT("w"), V.W);
                return MakeShared<FJsonValueObject>(Obj);
            }
            if (StructName == TEXT("SlateColor"))
            {
                const FSlateColor& SC = *(const FSlateColor*)PropAddr;
                const FLinearColor C = SC.GetSpecifiedColor();
                TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
                Obj->SetNumberField(TEXT("r"), C.R);
                Obj->SetNumberField(TEXT("g"), C.G);
                Obj->SetNumberField(TEXT("b"), C.B);
                Obj->SetNumberField(TEXT("a"), C.A);
                return MakeShared<FJsonValueObject>(Obj);
            }

            // Unknown struct -- ExportText_Direct stringification fallback.
            // Lossy-but-roundtrip-able since WriteValueToProperty's matching
            // fallback uses ImportText_Direct on the same string.
            FString Buffer;
            LeafProp->ExportText_Direct(Buffer, PropAddr, PropAddr, nullptr, PPF_None);
            return MakeShared<FJsonValueString>(Buffer);
        }

        // Array / map / fallback -- ExportText_Direct stringification.
        FString Buffer;
        LeafProp->ExportText_Direct(Buffer, PropAddr, PropAddr, nullptr, PPF_None);
        return MakeShared<FJsonValueString>(Buffer);
    }
} // namespace (read-side helpers)

FUIReflectionApplyResult FUIReflectionHelper::ReadJsonPath(
    const UObject* Root,
    const FName& WidgetToken,
    const FString& JsonPath,
    TSharedPtr<FJsonValue>& OutValue) const
{
    FUIReflectionApplyResult Result;
    Result.PropertyPath = JsonPath;
    OutValue = MakeShared<FJsonValueNull>();

    if (!Root)
    {
        Result.bSuccess = false;
        Result.FailureReason = TEXT("PropertyNotFound");
        Result.Detail = TEXT("null root object");
        return Result;
    }
    if (JsonPath.IsEmpty())
    {
        Result.bSuccess = false;
        Result.FailureReason = TEXT("PropertyNotFound");
        Result.Detail = TEXT("empty json path");
        return Result;
    }

    // Allowlist gate -- mirrors ApplyJsonPath.
    if (!Allowlist)
    {
        Result.bSuccess = false;
        Result.FailureReason = TEXT("NotInAllowlist");
        Result.Detail = TEXT("no allowlist supplied");
        return Result;
    }
    if (!Allowlist->IsAllowed(WidgetToken, JsonPath))
    {
        Result.bSuccess = false;
        Result.FailureReason = TEXT("NotInAllowlist");
        Result.Detail = WidgetToken.ToString();
        return Result;
    }

    // JSON path -> engine path translation via registry mapping (subsystem).
    FString EnginePath = JsonPath;
    if (UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get())
    {
        if (const FUITypeRegistryEntry* Entry = Sub->GetTypeRegistry().FindByToken(WidgetToken))
        {
            for (const FUIPropertyMapping& M : Entry->PropertyMappings)
            {
                if (M.JsonPath == JsonPath)
                {
                    if (!M.EnginePath.IsEmpty())
                    {
                        EnginePath = M.EnginePath;
                    }
                    break;
                }
            }
        }
    }

    // Resolve property chain. Use cache when available.
    FUIPropertyPathChain Chain;
    if (Cache)
    {
        UClass* RootClass = Root->GetClass();
        const FName RootStructName = RootClass->GetFName();
        // Pass the live UClass directly (see Apply for full rationale).
        Chain = Cache->Get(RootStructName, RootClass, EnginePath);
    }
    else
    {
        TArray<FString> Segments;
        EnginePath.ParseIntoArray(Segments, TEXT("."), /*bCullEmpty=*/true);
        UStruct* CurrentStruct = Root->GetClass();
        bool bOk = (Segments.Num() > 0 && CurrentStruct != nullptr);
        for (int32 i = 0; bOk && i < Segments.Num(); ++i)
        {
            FProperty* Prop = CurrentStruct ? CurrentStruct->FindPropertyByName(FName(*Segments[i])) : nullptr;
            if (!Prop) { bOk = false; break; }
            Chain.PropertyChain.Add(Prop);
            Chain.StructChain.Add(CurrentStruct);
            const bool bIsLast = (i == Segments.Num() - 1);
            if (!bIsLast)
            {
                if (FStructProperty* SP = CastField<FStructProperty>(Prop)) { CurrentStruct = SP->Struct; }
                else if (FObjectProperty* OP = CastField<FObjectProperty>(Prop)) { CurrentStruct = OP->PropertyClass; }
                else { bOk = false; break; }
            }
        }
        Chain.bValid = bOk;
    }

    if (!Chain.bValid || Chain.PropertyChain.Num() == 0)
    {
        Result.bSuccess = false;
        Result.FailureReason = TEXT("PropertyNotFound");
        Result.Detail = FString::Printf(TEXT("path '%s' did not resolve on %s"),
            *EnginePath, *Root->GetClass()->GetName());
        return Result;
    }

    // Walk the chain to compute the leaf value address. Mirror of Apply().
    const void* CurrentAddr = static_cast<const void*>(Root);
    for (int32 i = 0; i < Chain.PropertyChain.Num() - 1; ++i)
    {
        const FProperty* Prop = Chain.PropertyChain[i];
        const void* PropValueAddr = Prop->ContainerPtrToValuePtr<void>(CurrentAddr);

        if (CastField<FStructProperty>(Prop))
        {
            CurrentAddr = PropValueAddr;
        }
        else if (const FObjectProperty* OP = CastField<FObjectProperty>(Prop))
        {
            const UObject* SubObj = OP->GetObjectPropertyValue(PropValueAddr);
            if (!SubObj)
            {
                Result.bSuccess = false;
                Result.FailureReason = TEXT("PropertyNotFound");
                Result.Detail = FString::Printf(TEXT("intermediate object property '%s' is null"),
                    *Prop->GetName());
                return Result;
            }
            CurrentAddr = SubObj;
        }
        else
        {
            Result.bSuccess = false;
            Result.FailureReason = TEXT("TypeMismatch");
            Result.Detail = FString::Printf(TEXT("intermediate property '%s' is not navigable"),
                *Prop->GetName());
            return Result;
        }
    }

    const FProperty* LeafProp = Chain.PropertyChain.Last();
    const void* LeafAddr = LeafProp->ContainerPtrToValuePtr<void>(CurrentAddr);

    FString FailureReason;
    FString Detail;
    OutValue = ReadValueFromProperty(LeafProp, LeafAddr, FailureReason, Detail);

    if (!FailureReason.IsEmpty())
    {
        Result.bSuccess = false;
        Result.FailureReason = FailureReason;
        Result.Detail = Detail;
        return Result;
    }

    Result.bSuccess = true;
    return Result;
}

FUIReflectionApplyResult FUIReflectionHelper::ApplyJsonPath(
    UObject* Root,
    const FName& WidgetToken,
    const FString& JsonPath,
    const TSharedPtr<FJsonValue>& Value)
{
    FUIReflectionApplyResult Result;
    Result.PropertyPath = JsonPath; // LLM-facing form regardless of translation.

    if (!Root)
    {
        Result.bSuccess = false;
        Result.FailureReason = TEXT("PropertyNotFound");
        Result.Detail = TEXT("null root object");
        return Result;
    }
    if (JsonPath.IsEmpty())
    {
        Result.bSuccess = false;
        Result.FailureReason = TEXT("PropertyNotFound");
        Result.Detail = TEXT("empty json path");
        return Result;
    }

    // Step 1 — allowlist gate on the LLM-facing JSON path. This mirrors the
    // Phase F per-handler version (kept verbatim here so the helper is the
    // single source of truth for the gate semantics).
    if (!Allowlist)
    {
        Result.bSuccess = false;
        Result.FailureReason = TEXT("NotInAllowlist");
        Result.Detail = TEXT("no allowlist supplied");
        return Result;
    }
    if (!Allowlist->IsAllowed(WidgetToken, JsonPath))
    {
        Result.bSuccess = false;
        Result.FailureReason = TEXT("NotInAllowlist");
        Result.Detail = WidgetToken.ToString();
        return Result;
    }

    // Step 2 — translate JSON path -> engine path via the registry's curated
    // mapping for this widget token. Lookup is via the live subsystem (which
    // owns the registry); on subsystem-absent (test contexts before init) we
    // fall through to using the JSON path verbatim, matching the per-handler
    // pattern.
    FString EnginePath = JsonPath;
    if (UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get())
    {
        if (const FUITypeRegistryEntry* Entry = Sub->GetTypeRegistry().FindByToken(WidgetToken))
        {
            for (const FUIPropertyMapping& M : Entry->PropertyMappings)
            {
                if (M.JsonPath == JsonPath)
                {
                    if (!M.EnginePath.IsEmpty())
                    {
                        EnginePath = M.EnginePath;
                    }
                    break;
                }
            }
        }
    }

    // Step 3 — let the property-path-walking Apply do its thing. We pass
    // raw_mode=true because the gate has already checked the JSON path; the
    // engine path is an internal translation that the allowlist doesn't know.
    FUIReflectionApplyResult Inner = Apply(Root, EnginePath, Value, /*bRawMode=*/true);
    // Echo the LLM-facing path back regardless of translation so callers can
    // route the failure message into "what did the LLM ask for" reporting.
    Inner.PropertyPath = JsonPath;
    return Inner;
}
