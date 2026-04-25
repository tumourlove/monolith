// Copyright Monolith. All Rights Reserved.
//
// MonolithAI Phase I2: BT-to-GAS direct ability activation task.
//
// Drop-in BT task that activates a Gameplay Ability on the AI controller's
// possessed pawn ASC. Uses the OnAbilityEndedWithData multicast delegate to
// finalize InProgress -> Succeeded/Failed without per-tick polling.
//
// UHT scrapcode-aversion: UnrealHeaderTool 5.7 forbids UCLASS/UPROPERTY inside
// any preprocessor block other than WITH_EDITORONLY_DATA. To stay compatible
// with projects that do not enable the GameplayAbilities engine plugin, the
// reflection surface (UCLASS + every UPROPERTY) is declared UNCONDITIONALLY.
// We type-erase the only GAS-typed property (TSubclassOf<UGameplayAbility>)
// down to TSubclassOf<UObject> and cast inside the .cpp. All implementation
// includes and method bodies remain gated behind WITH_GAMEPLAYABILITIES so
// builds without GAS still link cleanly. The handler that registers the
// `add_bt_use_ability_task` action is also gated, so this class is never
// instantiated when the GameplayAbilities plugin is absent.
//
// Plan: Docs/plans/2026-04-26-bt-gas-ability-task.md

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "GameplayTagContainer.h"
#include "Templates/SubclassOf.h"

#if WITH_GAMEPLAYABILITIES
#include "GameplayAbilitySpec.h"   // FGameplayAbilitySpecHandle (used by-value in BT memory struct)
#endif

#include "BTTask_TryActivateAbility.generated.h"   // MUST be last include — UE clobbers CURRENT_FILE_ID via subsequent .generated.h transitives

class UBehaviorTreeComponent;
class UAbilitySystemComponent;

#if WITH_GAMEPLAYABILITIES
class UGameplayAbility;
struct FAbilityEndedData;
#endif

#if WITH_GAMEPLAYABILITIES
/**
 * Per-instance BT memory for UBTTask_TryActivateAbility.
 *
 * Holds:
 *  - WeakPtr to the resolved ASC (pawn may despawn mid-ability)
 *  - The spec handle that came back from TryActivateAbility (used to filter
 *    OnAbilityEndedWithData callbacks to *our* ability instance)
 *  - The delegate handle so we can detach cleanly on finish or abort
 *
 * GetInstanceMemorySize() returns sizeof(FBTTaskTryActivateAbilityMemory) so
 * the BT memory allocator gives every parallel AI instance its own copy.
 *
 * NOTE: This is a plain C++ struct (no USTRUCT), so the WITH_GAMEPLAYABILITIES
 * gate around it does NOT trigger UHT's "must not be inside preprocessor
 * blocks" rule — that rule applies only to reflection markers. Gating here is
 * safe and keeps the FGameplayAbilitySpecHandle dependency contained.
 */
struct FBTTaskTryActivateAbilityMemory
{
	TWeakObjectPtr<UAbilitySystemComponent> ASC;
	FGameplayAbilitySpecHandle ActivatedSpec;
	FDelegateHandle EndedHandle;
	bool bAwaitingEnd = false;
	bool bWasCancelled = false;
};
#endif // WITH_GAMEPLAYABILITIES

/**
 * BT task that activates a Gameplay Ability on the AI pawn's ASC.
 *
 * The reflection surface is unconditional (UHT requirement). The runtime path
 * is gated in the .cpp via WITH_GAMEPLAYABILITIES — when GAS is unavailable,
 * ExecuteTask returns Failed immediately and the action handler refuses to
 * register a node of this class.
 */
UCLASS(MinimalAPI, meta = (DisplayName = "Try Activate Gameplay Ability"))
class UBTTask_TryActivateAbility : public UBTTaskNode
{
	GENERATED_UCLASS_BODY()

public:

	// --- Configuration (set at design-time by add_bt_use_ability_task) ---

	/**
	 * Class of ability to activate. Mutually exclusive with AbilityTags.
	 *
	 * Type-erased to TSubclassOf<UObject> so this header compiles without the
	 * GameplayAbilities module. The MetaClass hint constrains the editor
	 * picker to UGameplayAbility subclasses when GAS is present; the .cpp
	 * casts via UGameplayAbility::StaticClass()->IsChildOf-checked logic.
	 */
	UPROPERTY(EditAnywhere, Category = "Ability", meta = (MetaClass = "/Script/GameplayAbilities.GameplayAbility", AllowAbstract = "false"))
	TSubclassOf<UObject> AbilityClass;

	/** Tag query — activate ANY granted ability matching ALL of these tags. Mutually exclusive with AbilityClass. */
	UPROPERTY(EditAnywhere, Category = "Ability")
	FGameplayTagContainer AbilityTags;

	/** If true, the task holds (returns InProgress) and finishes only when the ability ends. */
	UPROPERTY(EditAnywhere, Category = "Behavior")
	bool bWaitForEnd = true;

	/** If true, return Succeeded even when activation is blocked (cooldown, missing ASC, etc.). */
	UPROPERTY(EditAnywhere, Category = "Behavior")
	bool bSucceedOnBlocked = false;

	/** Optional: send a gameplay event with this tag immediately after successful activation. */
	UPROPERTY(EditAnywhere, Category = "Behavior")
	FGameplayTag EventTagOnActivate;

	// --- BTTaskNode overrides ---

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult) override;
	virtual uint16 GetInstanceMemorySize() const override;
	virtual FString GetStaticDescription() const override;

#if WITH_GAMEPLAYABILITIES
protected:
	/** Bound to ASC->OnAbilityEndedWithData. Filters by spec handle then resolves Succeeded/Failed. */
	void HandleAbilityEnded(const FAbilityEndedData& EndedData,
		TWeakObjectPtr<UBehaviorTreeComponent> OwnerCompWeak,
		uint8* NodeMemoryPtr);

	/** Resolve the ASC from the AIController's possessed pawn (interface or component). */
	UAbilitySystemComponent* ResolveASC(UBehaviorTreeComponent& OwnerComp) const;

	/** Detach our delegate from ASC->OnAbilityEndedWithData (idempotent). */
	void UnbindEnded(FBTTaskTryActivateAbilityMemory& Mem) const;
#endif // WITH_GAMEPLAYABILITIES
};
