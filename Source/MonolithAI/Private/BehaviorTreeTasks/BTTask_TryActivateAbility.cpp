// Copyright Monolith. All Rights Reserved.
//
// MonolithAI Phase I2: BT-to-GAS direct ability activation task — implementation.
// Plan: Docs/plans/2026-04-26-bt-gas-ability-task.md
//
// The reflection surface (UCLASS + UPROPERTY) lives unconditionally in the
// header (UHT 5.7 forbids preprocessor-gating those markers). All linkage
// against the GameplayAbilities module is contained here, behind
// WITH_GAMEPLAYABILITIES. When GAS is absent, the class still links (UE will
// expect ctor + virtual override symbols) but every method is a defensive
// no-op that returns Failed; the action handler also refuses to register a
// node of this class in that build, so the no-op path is unreachable in
// production.

#include "BehaviorTreeTasks/BTTask_TryActivateAbility.h"
#include "MonolithAIInternal.h"

UBTTask_TryActivateAbility::UBTTask_TryActivateAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NodeName = TEXT("Try Activate Gameplay Ability");

	// We don't override TickTask; we drive completion via FinishLatentTask
	// from the OnAbilityEndedWithData delegate. INIT_TASK_NODE_NOTIFY_FLAGS is
	// not strictly required (we never use OnTaskFinished for state) but the
	// override is harmless if we leave it as default.
	bNotifyTaskFinished = true;
}

uint16 UBTTask_TryActivateAbility::GetInstanceMemorySize() const
{
#if WITH_GAMEPLAYABILITIES
	return sizeof(FBTTaskTryActivateAbilityMemory);
#else
	return 0;
#endif
}

#if WITH_GAMEPLAYABILITIES

#include "BehaviorTree/BehaviorTreeComponent.h"
#include "AIController.h"
#include "GameFramework/Pawn.h"
#include "AbilitySystemInterface.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemGlobals.h"
#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayAbilitySpec.h"

UAbilitySystemComponent* UBTTask_TryActivateAbility::ResolveASC(UBehaviorTreeComponent& OwnerComp) const
{
	const AAIController* AIOwner = OwnerComp.GetAIOwner();
	if (!AIOwner)
	{
		return nullptr;
	}

	APawn* Pawn = AIOwner->GetPawn();
	if (!Pawn)
	{
		return nullptr;
	}

	// Canonical lookup: covers IAbilitySystemInterface, ASC component on actor,
	// and ASC on a CDO-owned subobject. AbilitySystemGlobals is the authoritative
	// resolver used across the engine.
	return UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Pawn);
}

void UBTTask_TryActivateAbility::UnbindEnded(FBTTaskTryActivateAbilityMemory& Mem) const
{
	if (Mem.ASC.IsValid() && Mem.EndedHandle.IsValid())
	{
		// UE 5.7: ASC->OnAbilityEnded is FGameplayAbilityEndedDelegate
		// (multicast, passes const FAbilityEndedData&). The "WithData" suffix
		// belongs to the per-ability UGameplayAbility::OnGameplayAbilityEndedWithData
		// — not the per-ASC delegate.
		Mem.ASC->OnAbilityEnded.Remove(Mem.EndedHandle);
	}
	Mem.EndedHandle.Reset();
	Mem.bAwaitingEnd = false;
}

EBTNodeResult::Type UBTTask_TryActivateAbility::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	FBTTaskTryActivateAbilityMemory* Mem = reinterpret_cast<FBTTaskTryActivateAbilityMemory*>(NodeMemory);
	// Reset every field — the BT memory allocator hands us raw bytes that may
	// hold residue from a previous run on the same node memory. Plain field
	// assignment matches the canonical UE BT-task idiom (no placement-new).
	Mem->ASC.Reset();
	Mem->ActivatedSpec = FGameplayAbilitySpecHandle();
	Mem->EndedHandle.Reset();
	Mem->bAwaitingEnd = false;
	Mem->bWasCancelled = false;

	// 1. Validate config (also validated at design-time but defend at runtime).
	//    AbilityClass is type-erased to TSubclassOf<UObject> in the header for
	//    UHT compatibility; narrow it back to UGameplayAbility here.
	TSubclassOf<UGameplayAbility> ResolvedAbilityClass;
	if (AbilityClass && AbilityClass->IsChildOf(UGameplayAbility::StaticClass()))
	{
		ResolvedAbilityClass = TSubclassOf<UGameplayAbility>(AbilityClass);
	}
	else if (AbilityClass)
	{
		// Type-erased pointer set to a non-GameplayAbility class — config error.
		UE_LOG(LogMonolithAI, Warning,
			TEXT("BTTask_TryActivateAbility[%s]: AbilityClass '%s' is not a UGameplayAbility subclass"),
			*GetNodeName(), *AbilityClass->GetName());
		return bSucceedOnBlocked ? EBTNodeResult::Succeeded : EBTNodeResult::Failed;
	}

	if (!ResolvedAbilityClass && AbilityTags.IsEmpty())
	{
		UE_LOG(LogMonolithAI, Warning,
			TEXT("BTTask_TryActivateAbility[%s]: no AbilityClass or AbilityTags configured"),
			*GetNodeName());
		return bSucceedOnBlocked ? EBTNodeResult::Succeeded : EBTNodeResult::Failed;
	}

	// 2. Resolve ASC from AI -> Pawn
	UAbilitySystemComponent* ASC = ResolveASC(OwnerComp);
	if (!ASC)
	{
		UE_LOG(LogMonolithAI, Verbose,
			TEXT("BTTask_TryActivateAbility[%s]: AI pawn has no AbilitySystemComponent"),
			*GetNodeName());
		return bSucceedOnBlocked ? EBTNodeResult::Succeeded : EBTNodeResult::Failed;
	}
	Mem->ASC = ASC;

	// 3. Capture the spec(s) we are about to activate so we can filter the
	//    end delegate by spec handle. UE 5.4+ has a 3-param overload of
	//    TryActivateAbilitiesByTag that fills OutAbilitySpecHandles, but for
	//    portability we look up the spec ourselves before activation.
	FGameplayAbilitySpec* TargetSpec = nullptr;
	if (ResolvedAbilityClass)
	{
		TargetSpec = ASC->FindAbilitySpecFromClass(ResolvedAbilityClass);
	}
	else
	{
		// First spec whose ability tags match the query.
		// NOTE: GetActivatableAbilities() returns const& here — we only read,
		// so no need for the non-const overload.
		// AbilityTags is deprecated since 5.5 in favor of GetAssetTags().
		for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
		{
			if (Spec.Ability && Spec.Ability->GetAssetTags().HasAll(AbilityTags))
			{
				TargetSpec = const_cast<FGameplayAbilitySpec*>(&Spec);
				break;
			}
		}
	}

	if (TargetSpec)
	{
		Mem->ActivatedSpec = TargetSpec->Handle;
	}

	// 4. Try activation
	bool bActivated = false;
	if (ResolvedAbilityClass)
	{
		bActivated = ASC->TryActivateAbilityByClass(ResolvedAbilityClass, /*bAllowRemoteActivation=*/true);
	}
	else
	{
		bActivated = ASC->TryActivateAbilitiesByTag(AbilityTags, /*bAllowRemoteActivation=*/true);
	}

	if (!bActivated)
	{
		UE_LOG(LogMonolithAI, Verbose,
			TEXT("BTTask_TryActivateAbility[%s]: activation blocked (cooldown/tags/CanActivate failed)"),
			*GetNodeName());
		return bSucceedOnBlocked ? EBTNodeResult::Succeeded : EBTNodeResult::Failed;
	}

	// 5. Optional: fire gameplay event right after successful activation.
	if (EventTagOnActivate.IsValid())
	{
		FGameplayEventData Payload;
		Payload.EventTag = EventTagOnActivate;
		Payload.Instigator = ASC->GetOwner();
		Payload.Target = ASC->GetOwner();
		UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(
			ASC->GetOwner(), EventTagOnActivate, Payload);
	}

	// 6. Fire-and-forget short-circuit
	if (!bWaitForEnd)
	{
		return EBTNodeResult::Succeeded;
	}

	// 7. Bind to ASC->OnAbilityEnded and return InProgress.
	//    NodeMemory pointer is stable for the lifetime of the running instance,
	//    so capturing it raw in the delegate is safe; OwnerComp is captured as
	//    a weak pointer to defend against pawn despawn during the ability.
	Mem->bAwaitingEnd = true;
	Mem->bWasCancelled = false;

	TWeakObjectPtr<UBehaviorTreeComponent> OwnerCompWeak(&OwnerComp);
	// UE 5.7: ASC->OnAbilityEnded is the multicast that fires on every ability
	// end with full FAbilityEndedData (AbilityThatEnded, AbilitySpecHandle,
	// bWasCancelled). Filtering by spec handle inside HandleAbilityEnded keeps
	// this scoped to OUR activation.
	Mem->EndedHandle = ASC->OnAbilityEnded.AddUObject(
		this, &UBTTask_TryActivateAbility::HandleAbilityEnded,
		OwnerCompWeak, NodeMemory);

	return EBTNodeResult::InProgress;
}

void UBTTask_TryActivateAbility::HandleAbilityEnded(
	const FAbilityEndedData& EndedData,
	TWeakObjectPtr<UBehaviorTreeComponent> OwnerCompWeak,
	uint8* NodeMemoryPtr)
{
	if (!NodeMemoryPtr || !OwnerCompWeak.IsValid())
	{
		return;
	}

	FBTTaskTryActivateAbilityMemory* Mem = reinterpret_cast<FBTTaskTryActivateAbilityMemory*>(NodeMemoryPtr);
	if (!Mem || !Mem->bAwaitingEnd)
	{
		return;
	}

	// Filter: was THIS the ability we activated?
	// Prefer spec-handle match (precise — survives multiple instances of the
	// same class). Fall back to class/tag match when we couldn't capture a
	// spec handle at activation time (defensive).
	bool bIsOurs = false;
	if (Mem->ActivatedSpec.IsValid())
	{
		bIsOurs = (EndedData.AbilitySpecHandle == Mem->ActivatedSpec);
	}
	else if (EndedData.AbilityThatEnded)
	{
		const UGameplayAbility* Ended = EndedData.AbilityThatEnded;
		if (AbilityClass)
		{
			bIsOurs = Ended->GetClass() == AbilityClass
				|| Ended->GetClass()->IsChildOf(AbilityClass);
		}
		else if (!AbilityTags.IsEmpty())
		{
			// UE 5.5+: AbilityTags is deprecated — use GetAssetTags().
			bIsOurs = Ended->GetAssetTags().HasAll(AbilityTags);
		}
	}

	if (!bIsOurs)
	{
		return;
	}

	// Capture cancellation status from the data struct (sidesteps the
	// UGameplayAbility::WasCancelled() availability question — see plan
	// open-question #1).
	Mem->bWasCancelled = EndedData.bWasCancelled;

	// Detach from delegate FIRST to avoid re-entry on multicast iteration.
	UnbindEnded(*Mem);

	const EBTNodeResult::Type Result = !Mem->bWasCancelled
		? EBTNodeResult::Succeeded
		: (bSucceedOnBlocked ? EBTNodeResult::Succeeded : EBTNodeResult::Failed);

	UBehaviorTreeComponent* OwnerComp = OwnerCompWeak.Get();
	if (OwnerComp)
	{
		FinishLatentTask(*OwnerComp, Result);
	}
}

EBTNodeResult::Type UBTTask_TryActivateAbility::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	FBTTaskTryActivateAbilityMemory* Mem = reinterpret_cast<FBTTaskTryActivateAbilityMemory*>(NodeMemory);
	if (!Mem)
	{
		return EBTNodeResult::Aborted;
	}

	if (Mem->ASC.IsValid())
	{
		// Cancel the in-flight ability so we don't leak a running instance.
		// Prefer the spec-handle path (precise, instance-targeted). If we never
		// captured a spec handle, fall back to a tag-container cancel — but
		// ONLY when the user explicitly drove activation by tag, never by
		// class (we don't want to mass-cancel unrelated abilities).
		if (Mem->ActivatedSpec.IsValid())
		{
			Mem->ASC->CancelAbilityHandle(Mem->ActivatedSpec);
		}
		else if (!AbilityTags.IsEmpty())
		{
			static const FGameplayTagContainer EmptyIgnore;
			Mem->ASC->CancelAbilities(&AbilityTags, &EmptyIgnore);
		}
		// else: no precise target captured + class-mode — emit a debug log
		// rather than risk a mass-cancel that aborts the player's other
		// active abilities. The handle should always be captured for class
		// activations; missing means the spec wasn't granted.
		else if (AbilityClass)
		{
			UE_LOG(LogMonolithAI, Verbose,
				TEXT("BTTask_TryActivateAbility[%s]: AbortTask had no captured spec handle; ability not granted at activation time"),
				*GetNodeName());
		}

		UnbindEnded(*Mem);
	}

	return EBTNodeResult::Aborted;
}

void UBTTask_TryActivateAbility::OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult)
{
	// Defensive cleanup if the task is finishing for a reason other than our
	// own delegate callback (e.g. parent composite aborts on a different
	// branch). Idempotent — already-detached handle is a no-op.
	if (NodeMemory)
	{
		FBTTaskTryActivateAbilityMemory* Mem = reinterpret_cast<FBTTaskTryActivateAbilityMemory*>(NodeMemory);
		UnbindEnded(*Mem);
	}

	Super::OnTaskFinished(OwnerComp, NodeMemory, TaskResult);
}

FString UBTTask_TryActivateAbility::GetStaticDescription() const
{
	FString Identity;
	if (AbilityClass)
	{
		Identity = FString::Printf(TEXT("Class: %s"), *AbilityClass->GetName());
	}
	else if (!AbilityTags.IsEmpty())
	{
		Identity = FString::Printf(TEXT("Tags: %s"), *AbilityTags.ToStringSimple());
	}
	else
	{
		Identity = TEXT("(unconfigured)");
	}

	return FString::Printf(
		TEXT("%s\n%s | WaitForEnd=%s | SucceedOnBlocked=%s%s"),
		*Super::GetStaticDescription(),
		*Identity,
		bWaitForEnd ? TEXT("yes") : TEXT("no"),
		bSucceedOnBlocked ? TEXT("yes") : TEXT("no"),
		EventTagOnActivate.IsValid()
			? *FString::Printf(TEXT(" | Event=%s"), *EventTagOnActivate.ToString())
			: TEXT(""));
}

#else // WITH_GAMEPLAYABILITIES == 0

// Stub implementations for projects without the GameplayAbilities plugin.
// The action handler refuses to register a node of this class in that build,
// so these paths are unreachable at runtime — but the symbols must exist for
// the UCLASS vtable to link.

EBTNodeResult::Type UBTTask_TryActivateAbility::ExecuteTask(UBehaviorTreeComponent& /*OwnerComp*/, uint8* /*NodeMemory*/)
{
	UE_LOG(LogMonolithAI, Warning,
		TEXT("BTTask_TryActivateAbility::ExecuteTask called in a build without GameplayAbilities — this should be unreachable"));
	return EBTNodeResult::Failed;
}

EBTNodeResult::Type UBTTask_TryActivateAbility::AbortTask(UBehaviorTreeComponent& /*OwnerComp*/, uint8* /*NodeMemory*/)
{
	return EBTNodeResult::Aborted;
}

void UBTTask_TryActivateAbility::OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult)
{
	Super::OnTaskFinished(OwnerComp, NodeMemory, TaskResult);
}

FString UBTTask_TryActivateAbility::GetStaticDescription() const
{
	return Super::GetStaticDescription() + TEXT("\n[GameplayAbilities plugin disabled]");
}

#endif // WITH_GAMEPLAYABILITIES
