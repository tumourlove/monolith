// Copyright tumourlove. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * MonolithUI -- UMG MVVM (ModelViewViewModel plugin) actions, issue #173.
 *
 * Read and author the MVVM view stored on a Widget Blueprint
 * (UMVVMWidgetBlueprintExtension_View -> UMVVMBlueprintView): viewmodels,
 * bindings (with conversion functions), events and conditions.
 *
 * Registered under the `ui` namespace with an `mvvm_` prefix. The whole
 * implementation is compiled only when WITH_MVVM=1 (the ModelViewViewModel
 * plugin is enabled in the project's .uproject and this is not a
 * MONOLITH_RELEASE_BUILD). With WITH_MVVM=0, Register() is a no-op, so no
 * mvvm_ actions exist and the module does not import any MVVM DLL.
 *
 * Writes go through UMVVMEditorSubsystem wherever it exposes the operation,
 * so the engine's own validation, wrapper-graph generation and transactions
 * are reused.
 */
namespace MonolithUI
{
    struct FMVVMActions
    {
        static void Register(FMonolithToolRegistry& Registry);
    };
}
