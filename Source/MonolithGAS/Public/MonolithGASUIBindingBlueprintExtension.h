// MonolithGASUIBindingBlueprintExtension.h
// Editor-side UWidgetBlueprintExtension that owns the persisted GAS attribute binding rows
// for a given UWidgetBlueprint, and at compile time mints + installs the runtime
// UMonolithGASAttributeBindingClassExtension via FWidgetBlueprintCompilerContext::AddExtension.
//
// This mirrors the engine's UUIComponentWidgetBlueprintExtension reference impl
// (UMGEditor/Private/UIComponentWidgetBlueprintExtension.cpp).

#pragma once

#include "CoreMinimal.h"
#include "WidgetBlueprintExtension.h"
#include "MonolithGASUIBindingTypes.h"

#include "MonolithGASUIBindingBlueprintExtension.generated.h"

class UWidgetBlueprintGeneratedClass;

UCLASS(MinimalAPI)
class UMonolithGASUIBindingBlueprintExtension : public UWidgetBlueprintExtension
{
    GENERATED_BODY()

public:
    /** Design-time persisted binding rows. Edited via Monolith gas/ui actions. */
    UPROPERTY(EditAnywhere, Category = "Monolith GAS Binding")
    TArray<FMonolithGASAttributeBindingSpec> Bindings;

    /** Add or replace a row keyed by (TargetWidgetName, TargetPropertyName). Returns final index. */
    int32 AddOrReplaceBinding(const FMonolithGASAttributeBindingSpec& Spec, bool bReplaceExisting, bool& bOutReplaced);

    /** Remove a row by key. Returns true if a row was removed. */
    bool RemoveBinding(FName WidgetName, FName PropertyName);

    /** Remove all rows. Returns count removed. */
    int32 ClearBindings();

    /** Find row index by key, or INDEX_NONE. */
    int32 IndexOfBinding(FName WidgetName, FName PropertyName) const;

protected:
    //~ Begin UWidgetBlueprintExtension interface
    virtual void HandleBeginCompilation(class FWidgetBlueprintCompilerContext& InCreationContext) override;
    virtual void HandleFinishCompilingClass(UWidgetBlueprintGeneratedClass* Class) override;
    virtual void HandleEndCompilation() override;
    //~ End UWidgetBlueprintExtension interface

private:
    /** Cached during the compile pass; cleared in HandleEndCompilation. Not serialized. */
    class FWidgetBlueprintCompilerContext* CachedCompilerContext = nullptr;
};
