// MonolithGASUIBindingTypes.h
// USTRUCT + UENUM declarations for the Monolith UI<->GAS attribute binding feature (Phase H1).
//
// These types are persisted on a Widget Blueprint via the editor-side
// UMonolithGASUIBindingBlueprintExtension, and copied at compile time onto
// the runtime UMonolithGASAttributeBindingClassExtension which lives on the
// UWidgetBlueprintGeneratedClass.
//
// Plan-of-record: Docs/plans/2026-04-26-ui-gas-attribute-binding.md

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MonolithGASUIBindingTypes.generated.h"

/** How the binding resolves the AbilitySystemComponent owner at runtime. */
UENUM(BlueprintType)
enum class EMonolithAttrBindOwner : uint8
{
    /** UW->GetOwningPlayer()->GetPawn()->ASC */
    OwningPlayerPawn       UMETA(DisplayName = "Owning Player Pawn"),
    /** UW->GetOwningPlayerState()->ASC */
    OwningPlayerState      UMETA(DisplayName = "Owning Player State"),
    /** UW->GetOwningPlayer()->ASC (PlayerController-hosted ASCs) */
    OwningPlayerController UMETA(DisplayName = "Owning Player Controller"),
    /** Cast<AActor>(UW->GetOuter()) — for UWidgetComponent-hosted widgets */
    SelfActor              UMETA(DisplayName = "Self Actor"),
    /** First actor in world with the configured ActorTag and an ASC */
    NamedSocket            UMETA(DisplayName = "Named Socket / Tag"),
};

/** How the runtime float value is shaped before being written to the widget property. */
UENUM(BlueprintType)
enum class EMonolithAttrBindFormat : uint8
{
    /** Resolve at compile-time based on widget property type. */
    Auto         UMETA(DisplayName = "Auto"),
    /** Output = clamp(Value/Max, 0, 1). */
    PercentZeroOne UMETA(DisplayName = "Percent (0..1)"),
    /** Output = FText::AsNumber(int(Value)). */
    IntText      UMETA(DisplayName = "Int Text"),
    /** Output = FText::AsNumber(Value) with FormatPayload decimals. */
    FloatText    UMETA(DisplayName = "Float Text"),
    /** Output = FString::Format(template, {Value, Max}). FormatPayload holds the template. */
    FormatString UMETA(DisplayName = "Format String"),
    /** Output = FLinearColor::Lerp(Low, High, clamp(Value/Max, 0, 1)). FormatPayload encodes both colors. */
    Gradient     UMETA(DisplayName = "Gradient"),
    /** Threshold mode for ESlateVisibility / ECheckBoxState. FormatPayload holds threshold. */
    Threshold    UMETA(DisplayName = "Threshold"),
};

/** When the runtime push happens. */
UENUM(BlueprintType)
enum class EMonolithAttrBindUpdate : uint8
{
    /** Write only on FOnAttributeChangeData callbacks. */
    OnChange         UMETA(DisplayName = "On Change"),
    /** Write every frame. Implies extension RequiresTick. */
    Tick             UMETA(DisplayName = "Every Tick"),
    /** OnChange + smoothed lerp toward target each tick. */
    OnChangeSmoothed UMETA(DisplayName = "On Change (Smoothed)"),
};

/**
 * Single binding row. (widget_name, target_property) is the equality key for replace/unbind operations.
 * AttributeSetClassPath supports both native paths (e.g. "/Script/Leviathan.LeviathanVitalsSet")
 * and Blueprint generated-class paths (e.g. "/Game/.../BP_AS_Vitals.BP_AS_Vitals_C").
 */
USTRUCT(BlueprintType)
struct MONOLITHGAS_API FMonolithGASAttributeBindingSpec
{
    GENERATED_BODY()

    /** Widget tree variable name (e.g. "HealthBar"). */
    UPROPERTY(EditAnywhere, Category = "GAS Binding")
    FName TargetWidgetName;

    /** Property on the widget (e.g. "Percent", "Text", "ColorAndOpacity"). */
    UPROPERTY(EditAnywhere, Category = "GAS Binding")
    FName TargetPropertyName;

    /** Attribute set class path (native or Blueprint _C). */
    UPROPERTY(EditAnywhere, Category = "GAS Binding")
    FString AttributeSetClassPath;

    /** Attribute property name (e.g. "Health"). */
    UPROPERTY(EditAnywhere, Category = "GAS Binding")
    FName AttributePropertyName;

    /** Optional second-attribute denominator for ratio formats. NAME_None if unused. */
    UPROPERTY(EditAnywhere, Category = "GAS Binding")
    FString MaxAttributeSetClassPath;

    UPROPERTY(EditAnywhere, Category = "GAS Binding")
    FName MaxAttributePropertyName;

    /** ASC owner resolution strategy. */
    UPROPERTY(EditAnywhere, Category = "GAS Binding")
    EMonolithAttrBindOwner Owner = EMonolithAttrBindOwner::OwningPlayerPawn;

    /** Tag used when Owner == NamedSocket. */
    UPROPERTY(EditAnywhere, Category = "GAS Binding")
    FName OwnerSocketTag;

    /** Format mode for runtime apply. */
    UPROPERTY(EditAnywhere, Category = "GAS Binding")
    EMonolithAttrBindFormat Format = EMonolithAttrBindFormat::Auto;

    /**
     * Format-mode-dependent payload.
     *  - FloatText        : decimal count as ASCII (e.g. "2")
     *  - FormatString     : template, e.g. "{0:int} / {1:int}"
     *  - Gradient         : "Rlow,Glow,Blow,Alow|Rhigh,Ghigh,Bhigh,Ahigh" floats
     *  - Threshold        : "T" — single float threshold
     */
    UPROPERTY(EditAnywhere, Category = "GAS Binding")
    FString FormatPayload;

    /** Update strategy. */
    UPROPERTY(EditAnywhere, Category = "GAS Binding")
    EMonolithAttrBindUpdate UpdatePolicy = EMonolithAttrBindUpdate::OnChange;

    /** Lerp speed when UpdatePolicy == OnChangeSmoothed. */
    UPROPERTY(EditAnywhere, Category = "GAS Binding", meta = (ClampMin = "0.01"))
    float SmoothingSpeed = 6.0f;

    bool operator==(const FMonolithGASAttributeBindingSpec& Other) const
    {
        return TargetWidgetName == Other.TargetWidgetName
            && TargetPropertyName == Other.TargetPropertyName;
    }
};
