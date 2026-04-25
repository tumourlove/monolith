// MonolithGASAttributeBindingClassExtension.cpp

#include "MonolithGASAttributeBindingClassExtension.h"
#include "MonolithGASInternal.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "AttributeSet.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
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
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/Pawn.h"
#include "Internationalization/Text.h"
#include "Math/Color.h"
#include "Misc/DefaultValueHelper.h"
#include "Slate/SlateBrushAsset.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateColor.h"
#include "UObject/UnrealType.h"

// Phase J F9: file-static `LogMonolithGAS` retired in favor of the parent
// LogMonolithGAS category (declared in MonolithGASInternal.h, defined in MonolithGASModule.cpp).

namespace
{
    UClass* ResolveAttributeSetClass(const FString& Path)
    {
        if (Path.IsEmpty()) return nullptr;
        // Try as full object path first (Blueprint _C or native)
        if (UClass* C = LoadObject<UClass>(nullptr, *Path))
        {
            return C;
        }
        // Try as native classname
        if (UClass* C = FindFirstObject<UClass>(*Path, EFindFirstObjectOptions::NativeFirst))
        {
            return C;
        }
        // Try with U prefix
        if (UClass* C = FindFirstObject<UClass>(*(TEXT("U") + Path), EFindFirstObjectOptions::NativeFirst))
        {
            return C;
        }
        return nullptr;
    }

    UWidget* FindNamedWidget(UUserWidget* UW, FName WidgetName)
    {
        if (!UW || !UW->WidgetTree || WidgetName.IsNone()) return nullptr;
        return UW->WidgetTree->FindWidget(WidgetName);
    }

    UAbilitySystemComponent* GetASCFromActorRuntime(AActor* Actor)
    {
        if (!Actor) return nullptr;
        if (IAbilitySystemInterface* I = Cast<IAbilitySystemInterface>(Actor))
        {
            if (UAbilitySystemComponent* ASC = I->GetAbilitySystemComponent())
            {
                return ASC;
            }
        }
        return Actor->FindComponentByClass<UAbilitySystemComponent>();
    }

    /** Parse "R,G,B,A|R,G,B,A" into two FLinearColors. Returns true on success. */
    bool ParseGradientPayload(const FString& Payload, FLinearColor& OutLow, FLinearColor& OutHigh)
    {
        FString Lo, Hi;
        if (!Payload.Split(TEXT("|"), &Lo, &Hi)) return false;
        auto Parse = [](const FString& S, FLinearColor& Out) -> bool
        {
            TArray<FString> Parts;
            S.ParseIntoArray(Parts, TEXT(","));
            if (Parts.Num() < 3) return false;
            Out.R = FCString::Atof(*Parts[0]);
            Out.G = FCString::Atof(*Parts[1]);
            Out.B = FCString::Atof(*Parts[2]);
            Out.A = Parts.Num() >= 4 ? FCString::Atof(*Parts[3]) : 1.f;
            return true;
        };
        return Parse(Lo, OutLow) && Parse(Hi, OutHigh);
    }

    /** Try invoking SetText / SetPercent / SetColorAndOpacity / etc. via UFunction reflection. */
    bool TryInvokeSetterByName(UWidget* Target, FName SetterName, void* ParmsBlob)
    {
        if (!Target) return false;
        UFunction* Func = Target->FindFunction(SetterName);
        if (!Func) return false;
        Target->ProcessEvent(Func, ParmsBlob);
        return true;
    }

    struct FFloatParm   { float V; };
    struct FTextParm    { FText V; };
    struct FColorParm   { FLinearColor V; };
    struct FVisParm     { ESlateVisibility V; };
    struct FCheckParm   { ECheckBoxState V; };
}

FGameplayAttribute UMonolithGASAttributeBindingClassExtension::ResolveAttribute(const FString& AttrSetClassPath, FName PropertyName)
{
    if (PropertyName.IsNone()) return FGameplayAttribute();
    UClass* AttrClass = ResolveAttributeSetClass(AttrSetClassPath);
    if (!AttrClass) return FGameplayAttribute();
    FProperty* Prop = FindFProperty<FProperty>(AttrClass, PropertyName);
    if (!Prop) return FGameplayAttribute();
    return FGameplayAttribute(Prop);
}

UAbilitySystemComponent* UMonolithGASAttributeBindingClassExtension::ResolveASC(UUserWidget* UW, const FMonolithGASAttributeBindingSpec& Spec) const
{
    if (!UW) return nullptr;
    switch (Spec.Owner)
    {
        case EMonolithAttrBindOwner::OwningPlayerPawn:
        {
            if (APlayerController* PC = UW->GetOwningPlayer())
            {
                return GetASCFromActorRuntime(PC->GetPawn());
            }
            return nullptr;
        }
        case EMonolithAttrBindOwner::OwningPlayerState:
        {
            if (APlayerController* PC = UW->GetOwningPlayer())
            {
                return GetASCFromActorRuntime(PC->PlayerState);
            }
            return nullptr;
        }
        case EMonolithAttrBindOwner::OwningPlayerController:
        {
            return GetASCFromActorRuntime(UW->GetOwningPlayer());
        }
        case EMonolithAttrBindOwner::SelfActor:
        {
            return GetASCFromActorRuntime(Cast<AActor>(UW->GetOuter()));
        }
        case EMonolithAttrBindOwner::NamedSocket:
        {
            UWorld* World = UW->GetWorld();
            if (!World || Spec.OwnerSocketTag.IsNone()) return nullptr;
            for (TActorIterator<AActor> It(World); It; ++It)
            {
                AActor* A = *It;
                if (A && A->ActorHasTag(Spec.OwnerSocketTag))
                {
                    if (UAbilitySystemComponent* ASC = GetASCFromActorRuntime(A))
                    {
                        return ASC;
                    }
                }
            }
            return nullptr;
        }
    }
    return nullptr;
}

void UMonolithGASAttributeBindingClassExtension::Initialize(UUserWidget* UserWidget)
{
    Super::Initialize(UserWidget);
    // Per-row resolution happens in Construct (after WidgetTree is fully populated and owner can resolve).
}

void UMonolithGASAttributeBindingClassExtension::Construct(UUserWidget* UserWidget)
{
    Super::Construct(UserWidget);
    if (!UserWidget) return;

    FInstanceState& State = Instances.FindOrAdd(UserWidget);
    State.SubsByRow.Reset();
    State.SubsByRow.SetNum(Bindings.Num());

    for (int32 i = 0; i < Bindings.Num(); ++i)
    {
        SubscribeRow(UserWidget, i, Bindings[i], State);
    }
}

void UMonolithGASAttributeBindingClassExtension::Destruct(UUserWidget* UserWidget)
{
    if (UserWidget)
    {
        FInstanceState* StatePtr = Instances.Find(UserWidget);
        if (StatePtr)
        {
            for (FActiveSub& Sub : StatePtr->SubsByRow)
            {
                if (UAbilitySystemComponent* ASC = Sub.ASC.Get())
                {
                    if (Sub.PrimaryHandle.IsValid() && Sub.Attribute.IsValid())
                    {
                        ASC->GetGameplayAttributeValueChangeDelegate(Sub.Attribute).Remove(Sub.PrimaryHandle);
                    }
                    if (Sub.MaxHandle.IsValid() && Sub.MaxAttribute.IsValid())
                    {
                        ASC->GetGameplayAttributeValueChangeDelegate(Sub.MaxAttribute).Remove(Sub.MaxHandle);
                    }
                }
            }
            Instances.Remove(UserWidget);
        }
    }
    Super::Destruct(UserWidget);
}

void UMonolithGASAttributeBindingClassExtension::SubscribeRow(UUserWidget* UW, int32 RowIndex, const FMonolithGASAttributeBindingSpec& Spec, FInstanceState& State)
{
    if (!UW || !State.SubsByRow.IsValidIndex(RowIndex)) return;

    FActiveSub& Sub = State.SubsByRow[RowIndex];
    Sub.TargetWidget = FindNamedWidget(UW, Spec.TargetWidgetName);
    if (!Sub.TargetWidget.IsValid())
    {
        UE_LOG(LogMonolithGAS, Warning,
            TEXT("[GASBind] Widget '%s' not found in UserWidget '%s'"),
            *Spec.TargetWidgetName.ToString(), *UW->GetName());
        return;
    }

    Sub.Attribute = ResolveAttribute(Spec.AttributeSetClassPath, Spec.AttributePropertyName);
    if (!Sub.Attribute.IsValid())
    {
        UE_LOG(LogMonolithGAS, Warning,
            TEXT("[GASBind] Attribute %s.%s could not be resolved"),
            *Spec.AttributeSetClassPath, *Spec.AttributePropertyName.ToString());
        return;
    }
    if (!Spec.MaxAttributePropertyName.IsNone())
    {
        Sub.MaxAttribute = ResolveAttribute(
            Spec.MaxAttributeSetClassPath.IsEmpty() ? Spec.AttributeSetClassPath : Spec.MaxAttributeSetClassPath,
            Spec.MaxAttributePropertyName);
    }

    UAbilitySystemComponent* ASC = ResolveASC(UW, Spec);
    if (!ASC)
    {
        // Phase J F9: time-gated escalation per spec.
        //  - First attempt and within 1s: silent-ish (Verbose) to avoid spamming during normal owner-spawn race.
        //  - After 1s without resolution: escalate to Warning ONCE so misconfigured rows surface in shipping logs.
        const double Now = FPlatformTime::Seconds();
        if (Sub.FirstSubscribeAttemptTime <= 0.0)
        {
            Sub.FirstSubscribeAttemptTime = Now;
        }
        const double Elapsed = Now - Sub.FirstSubscribeAttemptTime;
        if (Elapsed > 1.0 && !Sub.bGraceEscalated)
        {
            Sub.bGraceEscalated = true;
            UE_LOG(LogMonolithGAS, Warning,
                TEXT("[GASBind] ApplyValue: owner failed to resolve after grace for widget=%s (binding %s -> %s, elapsed=%.2fs)"),
                *Spec.TargetWidgetName.ToString(),
                *Spec.TargetWidgetName.ToString(), *Spec.AttributePropertyName.ToString(), Elapsed);
        }
        else
        {
            UE_LOG(LogMonolithGAS, Verbose,
                TEXT("[GASBind] ApplyValue: owner not yet resolved for widget=%s; deferring (binding %s -> %s)"),
                *Spec.TargetWidgetName.ToString(),
                *Spec.TargetWidgetName.ToString(), *Spec.AttributePropertyName.ToString());
        }
        return;
    }
    Sub.ASC = ASC;

    // Subscribe primary attribute.
    TWeakObjectPtr<UUserWidget> WeakUW(UW);
    Sub.PrimaryHandle = ASC->GetGameplayAttributeValueChangeDelegate(Sub.Attribute).AddLambda(
        [this, WeakUW, RowIndex](const FOnAttributeChangeData& Data)
        {
            this->OnAttributeChanged(Data, WeakUW, RowIndex);
        });

    // Subscribe max attribute (re-applies on max change so ratios stay current).
    if (Sub.MaxAttribute.IsValid())
    {
        Sub.MaxHandle = ASC->GetGameplayAttributeValueChangeDelegate(Sub.MaxAttribute).AddLambda(
            [this, WeakUW, RowIndex](const FOnAttributeChangeData& Data)
            {
                this->OnAttributeChanged(Data, WeakUW, RowIndex);
            });
    }

    // Push initial value so the bar is correct on first frame.
    bool bFound = false;
    Sub.TargetValue = ASC->GetGameplayAttributeValue(Sub.Attribute, bFound);
    if (Sub.MaxAttribute.IsValid())
    {
        bool bMaxFound = false;
        Sub.TargetMaxValue = ASC->GetGameplayAttributeValue(Sub.MaxAttribute, bMaxFound);
        if (!bMaxFound) Sub.TargetMaxValue = 1.f;
    }
    Sub.CurrentDisplayedValue = Sub.TargetValue;
    Sub.bHasInitialValue = bFound;
    if (bFound)
    {
        ApplyValue(UW, Spec, Sub);
    }
}

void UMonolithGASAttributeBindingClassExtension::OnAttributeChanged(const FOnAttributeChangeData& Data, TWeakObjectPtr<UUserWidget> WeakUW, int32 RowIndex)
{
    UUserWidget* UW = WeakUW.Get();
    if (!UW) return;
    FInstanceState* State = Instances.Find(UW);
    if (!State || !State->SubsByRow.IsValidIndex(RowIndex)) return;
    if (!Bindings.IsValidIndex(RowIndex)) return;

    FActiveSub& Sub = State->SubsByRow[RowIndex];
    const FMonolithGASAttributeBindingSpec& Spec = Bindings[RowIndex];

    // Determine which attribute fired and update the cached pair.
    if (Sub.MaxAttribute.IsValid() && Data.Attribute == Sub.MaxAttribute)
    {
        Sub.TargetMaxValue = Data.NewValue;
    }
    else
    {
        Sub.TargetValue = Data.NewValue;
    }

    // For non-smoothed, snap to target immediately.
    if (Spec.UpdatePolicy != EMonolithAttrBindUpdate::OnChangeSmoothed)
    {
        Sub.CurrentDisplayedValue = Sub.TargetValue;
    }

    ApplyValue(UW, Spec, Sub);
}

void UMonolithGASAttributeBindingClassExtension::ApplyValue(UUserWidget* UW, const FMonolithGASAttributeBindingSpec& Spec, FActiveSub& Sub) const
{
    UWidget* Target = Sub.TargetWidget.Get();
    if (!Target) return;

    const float V = Sub.CurrentDisplayedValue;
    const float Mx = (Sub.TargetMaxValue > 0.f) ? Sub.TargetMaxValue : 1.f;
    const float Ratio = FMath::Clamp(V / Mx, 0.f, 1.f);

    // Phase J F9: per-fire trace at Verbose (shipping-silent by default; enable LogMonolithGAS Verbose to surface).
    // `formatted=<s>` per spec is the post-format payload — for branches that compute it (Text), we'd ideally
    // log the final string. To keep the trace branch-agnostic and a single line, log the raw value pair plus
    // ratio; format-specific details can be derived from the format/payload fields if a deeper trace is needed.
    UE_LOG(LogMonolithGAS, Verbose,
        TEXT("[GASBind] ApplyValue: widget=%s property=%s attr=%s.%s raw_value=%.4f max=%.4f ratio=%.4f"),
        *Target->GetName(),
        *Spec.TargetPropertyName.ToString(),
        *Spec.AttributeSetClassPath, *Spec.AttributePropertyName.ToString(),
        V, Mx, Ratio);

    // Resolve effective format (Auto -> per-property type).
    EMonolithAttrBindFormat Format = Spec.Format;

    // Dispatch by widget class + target property.
    const FName Prop = Spec.TargetPropertyName;

    // ProgressBar.Percent  (float 0..1)
    if (UProgressBar* PB = Cast<UProgressBar>(Target))
    {
        if (Prop == TEXT("Percent"))
        {
            FFloatParm P; P.V = Sub.MaxAttribute.IsValid() ? Ratio : FMath::Clamp(V, 0.f, 1.f);
            TryInvokeSetterByName(Target, TEXT("SetPercent"), &P);
            return;
        }
    }

    // TextBlock / RichTextBlock / EditableText / EditableTextBox -> Text
    if (Prop == TEXT("Text"))
    {
        FString Out;
        if (Format == EMonolithAttrBindFormat::FormatString && !Spec.FormatPayload.IsEmpty())
        {
            FString Tpl = Spec.FormatPayload;
            // Minimal positional formatter: {0} -> V, {1} -> Mx. Honor :int suffix.
            auto ReplaceTok = [&](const TCHAR* Tok, float Value)
            {
                FString IntTok = FString::Printf(TEXT("{%s:int}"), Tok);
                FString DefTok = FString::Printf(TEXT("{%s}"),     Tok);
                Tpl.ReplaceInline(*IntTok, *FString::Printf(TEXT("%d"), FMath::RoundToInt(Value)));
                Tpl.ReplaceInline(*DefTok, *FString::SanitizeFloat(Value));
            };
            ReplaceTok(TEXT("0"), V);
            ReplaceTok(TEXT("1"), Mx);
            Out = Tpl;
        }
        else if (Format == EMonolithAttrBindFormat::IntText
              || (Format == EMonolithAttrBindFormat::Auto && FMath::IsNearlyEqual(V, FMath::TruncToFloat(V))))
        {
            Out = FString::Printf(TEXT("%d"), FMath::RoundToInt(V));
        }
        else if (Format == EMonolithAttrBindFormat::FloatText)
        {
            int32 Decimals = 0;
            if (!Spec.FormatPayload.IsEmpty()) FDefaultValueHelper::ParseInt(Spec.FormatPayload, Decimals);
            Out = FString::Printf(TEXT("%.*f"), FMath::Clamp(Decimals, 0, 6), V);
        }
        else
        {
            Out = FString::SanitizeFloat(V);
        }

        FTextParm P; P.V = FText::FromString(Out);
        TryInvokeSetterByName(Target, TEXT("SetText"), &P);
        return;
    }

    // Image.ColorAndOpacity / Border.BrushColor -> FLinearColor (gradient)
    if (Prop == TEXT("ColorAndOpacity") || Prop == TEXT("BrushColor"))
    {
        FLinearColor Lo(1.f, 0.f, 0.f, 1.f), Hi(0.f, 1.f, 0.f, 1.f);
        if (Format == EMonolithAttrBindFormat::Gradient)
        {
            ParseGradientPayload(Spec.FormatPayload, Lo, Hi);
        }
        FLinearColor Out = FMath::Lerp(Lo, Hi, Ratio);

        FColorParm P; P.V = Out;
        if (Cast<UImage>(Target) && Prop == TEXT("ColorAndOpacity"))
        {
            TryInvokeSetterByName(Target, TEXT("SetColorAndOpacity"), &P);
            return;
        }
        if (Cast<UBorder>(Target) && Prop == TEXT("BrushColor"))
        {
            TryInvokeSetterByName(Target, TEXT("SetBrushColor"), &P);
            return;
        }
    }

    // Any.RenderOpacity (float 0..1)
    if (Prop == TEXT("RenderOpacity"))
    {
        FFloatParm P; P.V = Sub.MaxAttribute.IsValid() ? Ratio : FMath::Clamp(V, 0.f, 1.f);
        TryInvokeSetterByName(Target, TEXT("SetRenderOpacity"), &P);
        return;
    }

    // Any.Visibility (threshold)
    if (Prop == TEXT("Visibility"))
    {
        float Threshold = 0.f;
        if (!Spec.FormatPayload.IsEmpty()) Threshold = FCString::Atof(*Spec.FormatPayload);
        FVisParm P;
        P.V = (V > Threshold) ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
        TryInvokeSetterByName(Target, TEXT("SetVisibility"), &P);
        return;
    }

    // Slider/SpinBox.Value
    if (Prop == TEXT("Value"))
    {
        FFloatParm P; P.V = V;
        TryInvokeSetterByName(Target, TEXT("SetValue"), &P);
        return;
    }

    // CheckBox.CheckedState (threshold)
    if (Prop == TEXT("CheckedState"))
    {
        float Threshold = 0.f;
        if (!Spec.FormatPayload.IsEmpty()) Threshold = FCString::Atof(*Spec.FormatPayload);
        FCheckParm P;
        P.V = (V > Threshold) ? ECheckBoxState::Checked
            : (V <= 0.f       ? ECheckBoxState::Unchecked : ECheckBoxState::Undetermined);
        TryInvokeSetterByName(Target, TEXT("SetCheckedState"), &P);
        return;
    }

    UE_LOG(LogMonolithGAS, Warning,
        TEXT("[GASBind] Apply: unhandled property '%s' on widget '%s' (%s)"),
        *Prop.ToString(), *Target->GetName(), *Target->GetClass()->GetName());
}
