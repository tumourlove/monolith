#pragma once

#include "CoreMinimal.h"

class UMovieScene;
class UMovieSceneSequence;

namespace MonolithLevelSequence
{
	/**
	 * Classify a binding GUID into (name, kind, bound_class, custom_binding_class, custom_binding_pretty).
	 * Shared by the indexer and the live-asset actions so both report the same kind for a binding.
	 * Kinds: possessable / spawnable / replaceable / custom / unknown.
	 */
	void ClassifyBinding(
		UMovieSceneSequence* Seq, UMovieScene* MS, const FGuid& Guid, int32 BindingIndex,
		FString& OutName, FString& OutKind, FString& OutClass,
		FString& OutCustomClass, FString& OutCustomPretty);
}
