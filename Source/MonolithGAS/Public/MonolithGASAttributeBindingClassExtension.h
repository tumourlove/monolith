// MonolithGASAttributeBindingClassExtension.h
// Runtime UWidgetBlueprintGeneratedClassExtension that subscribes to GAS attribute change
// delegates on UUserWidget Construct, and pushes typed values to bound widget properties.
//
// One extension instance is shared across all UUserWidget instances of a given WBP class
// (the engine's design — see UWidgetBlueprintGeneratedClass::ForEachExtension). Per-instance
// state (delegate handles, smoothed values) is keyed by TWeakObjectPtr<UUserWidget>.

#pragma once

#include "CoreMinimal.h"
#include "Extensions/WidgetBlueprintGeneratedClassExtension.h"
#include "MonolithGASUIBindingTypes.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Delegates/IDelegateInstance.h"
#include "GameplayEffectTypes.h"

#include "MonolithGASAttributeBindingClassExtension.generated.h"

class UAbilitySystemComponent;
class UUserWidget;
class UWidget;
class FProperty;

UCLASS(MinimalAPI)
class UMonolithGASAttributeBindingClassExtension : public UWidgetBlueprintGeneratedClassExtension
{
    GENERATED_BODY()

public:
    /** Persisted bindings, copied from the editor-side UMonolithGASUIBindingBlueprintExtension at compile time. */
    UPROPERTY()
    TArray<FMonolithGASAttributeBindingSpec> Bindings;

    //~ Begin UWidgetBlueprintGeneratedClassExtension interface
    virtual void Initialize(UUserWidget* UserWidget) override;
    virtual void Construct(UUserWidget* UserWidget) override;
    virtual void Destruct(UUserWidget* UserWidget) override;
    //~ End UWidgetBlueprintGeneratedClassExtension interface

private:
    /** Per-binding-row, per-instance subscription state. */
    struct FActiveSub
    {
        TWeakObjectPtr<UAbilitySystemComponent> ASC;
        FGameplayAttribute Attribute;
        FGameplayAttribute MaxAttribute;
        FDelegateHandle PrimaryHandle;
        FDelegateHandle MaxHandle;
        // Cached resolved widget + setter property/function.
        TWeakObjectPtr<UWidget> TargetWidget;
        FProperty* CachedProperty = nullptr;
        UFunction* CachedSetter = nullptr;
        // Smoothing state.
        float CurrentDisplayedValue = 0.f;
        float TargetValue = 0.f;
        float TargetMaxValue = 1.f;
        bool bHasInitialValue = false;
        // Phase J F9: first-attempt timestamp for the owner-resolution grace window.
        // 0.0 sentinel = no attempt yet; populated on first SubscribeRow call where ASC was nullptr.
        // Used to escalate the "owner not yet resolved" log from Verbose to Warning after 1s.
        double FirstSubscribeAttemptTime = 0.0;
        // Phase J F9: latch so the post-grace Warning fires exactly once per row, not every retry.
        bool bGraceEscalated = false;
    };

    /** Per-UserWidget bookkeeping. Iterates `Bindings` index-aligned. */
    struct FInstanceState
    {
        TArray<FActiveSub> SubsByRow;
    };

    /** Strong-ish keyed by raw ptr; we only touch on Construct/Destruct of that exact UW. */
    TMap<TWeakObjectPtr<UUserWidget>, FInstanceState> Instances;

    /** Resolve the ASC for a given owner mode. Returns nullptr if not yet available. */
    UAbilitySystemComponent* ResolveASC(UUserWidget* UW, const FMonolithGASAttributeBindingSpec& Spec) const;

    /** Resolve the FGameplayAttribute from class-path + property-name. */
    static FGameplayAttribute ResolveAttribute(const FString& AttrSetClassPath, FName PropertyName);

    /** Subscribe one row, push initial value. */
    void SubscribeRow(UUserWidget* UW, int32 RowIndex, const FMonolithGASAttributeBindingSpec& Spec, FInstanceState& State);

    /** Apply pipeline — coerce float (or float pair) -> widget property. */
    void ApplyValue(UUserWidget* UW, const FMonolithGASAttributeBindingSpec& Spec, FActiveSub& Sub) const;

    /** Multicast callback. UserData is captured via lambda → AddUObject can't carry context, so we use AddLambda
     *  via a per-row helper. The bound function below adapts the multicast payload back to (UW,RowIndex). */
    void OnAttributeChanged(const FOnAttributeChangeData& Data, TWeakObjectPtr<UUserWidget> WeakUW, int32 RowIndex);
};
