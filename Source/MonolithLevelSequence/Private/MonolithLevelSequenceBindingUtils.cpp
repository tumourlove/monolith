#include "MonolithLevelSequenceBindingUtils.h"
#include "MovieScene.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneSequence.h"
#include "MovieSceneBindingReferences.h"
#include "Bindings/MovieSceneCustomBinding.h"
#include "Bindings/MovieSceneSpawnableBinding.h"
#include "Bindings/MovieSceneReplaceableBinding.h"

/**
 * Classify a binding GUID into (name, kind, bound_class, custom_binding_class, custom_binding_pretty).
 *
 * UE 5.7 introduced UMovieSceneCustomBinding (in BindingReferences) which sits ALONGSIDE
 * the legacy FMovieScenePossessable / FMovieSceneSpawnable structures. The migration path
 * (see ULevelSequence::ConvertOldSpawnables) registers modern Spawnables AS Possessables
 * inside UMovieScene while attaching their real spawnable identity to BindingReferences.
 * That means FindPossessable() returns non-null for them, and they would be mis-classified
 * as "possessable" if we didn't also consult Sequence->GetBindingReferences().
 *
 * Output meaning:
 *   OutKind             — editor-facing kind: possessable / spawnable / replaceable / custom / unknown
 *   OutClass            — class of the bound object (preferred from CustomBinding when present)
 *   OutCustomClass      — exact UCLASS name of the custom binding (e.g. MovieSceneSpawnableActorBinding)
 *   OutCustomPretty     — human-readable type label from GetBindingTypePrettyName()
 */
void MonolithLevelSequence::ClassifyBinding(
	UMovieSceneSequence* Seq, UMovieScene* MS, const FGuid& Guid, int32 BindingIndex,
	FString& OutName, FString& OutKind, FString& OutClass,
	FString& OutCustomClass, FString& OutCustomPretty)
{
	OutName.Reset();
	OutKind.Reset();
	OutClass.Reset();
	OutCustomClass.Reset();
	OutCustomPretty.Reset();
	if (!MS) return;

	FMovieScenePossessable* Possessable = MS->FindPossessable(Guid);
	FMovieSceneSpawnable*   LegacySpawn = MS->FindSpawnable(Guid);

	if (Possessable)
	{
		OutName = Possessable->GetName();
#if WITH_EDITORONLY_DATA
		if (const UClass* Cls = Possessable->GetPossessedObjectClass())
		{
			OutClass = Cls->GetName();
		}
#endif
	}
	else if (LegacySpawn)
	{
		OutName = LegacySpawn->GetName();
		OutKind = TEXT("spawnable");
		if (const UObject* Tmpl = LegacySpawn->GetObjectTemplate())
		{
			OutClass = Tmpl->GetClass()->GetName();
		}
	}

	const UMovieSceneCustomBinding* Custom = nullptr;
	if (Seq)
	{
		if (const FMovieSceneBindingReferences* Refs = Seq->GetBindingReferences())
		{
			Custom = Refs->GetCustomBinding(Guid, BindingIndex);
		}
	}

	if (Custom)
	{
		OutCustomClass  = Custom->GetClass()->GetName();
		OutCustomPretty = Custom->GetBindingTypePrettyName().ToString();

		if (UClass* BoundCls = Custom->GetBoundObjectClass())
		{
			// CustomBinding's bound class is more authoritative than the
			// possessable upgrade-stub class — prefer it when both exist.
			OutClass = BoundCls->GetName();
		}

		if (Custom->IsA<UMovieSceneSpawnableBindingBase>())
		{
			OutKind = TEXT("spawnable");
		}
		else if (Custom->IsA<UMovieSceneReplaceableBindingBase>())
		{
			OutKind = TEXT("replaceable");
		}
		else
		{
			OutKind = TEXT("custom");
		}
	}
	else if (OutKind.IsEmpty())
	{
		OutKind = Possessable ? TEXT("possessable") : TEXT("unknown");
	}
}
