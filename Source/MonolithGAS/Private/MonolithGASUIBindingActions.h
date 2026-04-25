// MonolithGASUIBindingActions.h
// Action handlers for the gas/ui attribute-binding feature (Phase H1).
// Canonical registration in `gas` namespace; thin alias in `ui` namespace.

#pragma once

#include "MonolithGASInternal.h"

class FMonolithGASUIBindingActions
{
public:
    /** Registers actions in BOTH `gas` (canonical) and `ui` (alias) namespaces. */
    static void RegisterActions(FMonolithToolRegistry& Registry);

    static FMonolithActionResult HandleBindWidgetToAttribute(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleUnbindWidgetAttribute(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleListAttributeBindings(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleClearWidgetAttributeBindings(const TSharedPtr<FJsonObject>& Params);
};
