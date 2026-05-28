// SPDX-License-Identifier: MIT
// Private declaration for the central bulk_fill + describe dispatcher
// registration. Pattern mirrors MonolithCoreTools.h.

#pragma once

#include "CoreMinimal.h"

/**
 * Central dispatcher action registration for the `bulk_fill` and `describe`
 * namespaces. Adapters in subsequent phases (1-5) self-register their
 * per-namespace handlers via FMonolithBulkFillRegistry; this class wires the
 * outer JSON-RPC actions that route to the registry.
 */
class FMonolithBulkFillActions
{
public:
	/** Register the 4 actions (bulk_fill.apply, bulk_fill.list_namespaces, describe.schema, describe.list_targets). */
	static void RegisterAll();

	/** Called from FMonolithCoreModule::ShutdownModule (mirrors UnregisterNamespace pattern). */
	static void UnregisterAll();
};
