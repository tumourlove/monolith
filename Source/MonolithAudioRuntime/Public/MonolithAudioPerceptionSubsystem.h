#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Containers/Set.h"
#include "Delegates/IDelegateInstance.h"
#include "MonolithAudioPerceptionSubsystem.generated.h"

class AActor;
class UAudioComponent;
enum class EAudioComponentPlayState : uint8;

/**
 * UMonolithAudioPerceptionSubsystem
 *
 * UWorldSubsystem that auto-fires AActor::MakeNoise whenever a UAudioComponent starts playing
 * a USoundBase carrying a UMonolithSoundPerceptionUserData payload.
 *
 * Hook point: UAudioComponent::OnAudioPlayStateChangedNative (cheap, native, multicast).
 * The audio engine marshals state notifications back to the game thread, so this delegate
 * fires on the game thread — confirmed empirically by existing engine broadcasts in
 * UAudioComponent::SetPlayState. We additionally guard with IsInGameThread() ensure for safety
 * (header doesn't enforce it; per H3 plan trap #1 — never call MakeNoise on the audio thread).
 *
 * Coverage:
 *  - YES: UAudioComponent::Play, SpawnSoundAtLocation, SpawnSoundAttached, CreateSound2D
 *  - NO:  UGameplayStatics::PlaySoundAtLocation (no UAudioComponent spawned). Use
 *         UMonolithAudioPerceptionStatics::PlaySoundAndReportNoise as the replacement.
 *
 * Authority gating: AActor::MakeNoise is BlueprintAuthorityOnly. We test
 * Owner->HasAuthority() before calling — silent no-op on clients in networked games.
 */
UCLASS(MinimalAPI)
class UMonolithAudioPerceptionSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin UWorldSubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void PostInitialize() override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem

private:
	/** Bound to UWorld::OnActorSpawned — newly spawned actors get their AudioComponents wired. */
	void OnActorSpawned(AActor* Actor);

	/** Walks an actor's components and binds OnAudioPlayStateChangedNative on each UAudioComponent. */
	void HookActorAudioComponents(AActor* Actor);

	/** Inspects a single component and binds the native delegate if it carries a perception-bound sound. */
	void TryHookAudioComponent(UAudioComponent* AudioComp);

	/** Native multicast handler: dispatches MakeNoise on Playing (and optionally FadingIn). */
	void OnAudioPlayStateChanged(const UAudioComponent* AudioComp, EAudioComponentPlayState NewState);

	/** Walks all existing actors once on PostInitialize / OnWorldBeginPlay (catches placed AudioComponents). */
	void HookAllExistingActors();

	/** Reentrancy guard — components that have already fired this play. Cleared on Stopped. */
	TSet<TWeakObjectPtr<const UAudioComponent>> AlreadyFiredThisPlay;

	/** Tracked DelegateHandles per AudioComponent so Deinitialize can cleanly unregister. */
	TMap<TWeakObjectPtr<const UAudioComponent>, FDelegateHandle> BoundComponents;

	/** OnActorSpawned subscription. */
	FDelegateHandle ActorSpawnedHandle;

	/** Latched after first PostInitialize run so we don't re-walk if Initialize is invoked twice. */
	bool bHasWalkedExistingActors = false;
};
