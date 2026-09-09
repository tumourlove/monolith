#pragma once

#include "CoreMinimal.h"
#include "Misc/CoreDelegates.h"
#include "Runtime/Launch/Resources/Version.h"

namespace MonolithCoreDelegatesCompat
{
/**
 * Return the post-engine-init multicast delegate without exposing engine-version
 * API drift to each module that listens on it.
 *
 * UE 5.8 deprecated the FCoreDelegates::OnPostEngineInit data member in favour of
 * the GetOnPostEngineInit() accessor. UE 5.7 ships only the data member, so the
 * version gate lives here rather than at every AddLambda / Remove call site.
 */
FORCEINLINE FSimpleMulticastDelegate& GetOnPostEngineInit()
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	return FCoreDelegates::GetOnPostEngineInit();
#else
	return FCoreDelegates::OnPostEngineInit;
#endif
}
}
