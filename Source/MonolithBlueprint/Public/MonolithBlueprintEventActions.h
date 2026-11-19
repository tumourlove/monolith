#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Event / delegate / async node authoring (issue #162 items 5-7):
 *   add_custom_event_input     - add a typed input to a custom event (type token or copied from a pin)
 *   add_event_for_delegate     - custom event matching a delegate pin's signature, wired to it
 *   add_create_delegate_node   - Create Event node bound to a function, linked to a delegate pin
 *   add_async_action_node      - async action node from a static factory function
 *   add_construct_object_node  - Construct Object / Create Widget (any ConstructObjectFromClass node) with the class set
 */
class FMonolithBlueprintEventActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult HandleAddCustomEventInput(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddEventForDelegate(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddCreateDelegateNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddAsyncActionNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddConstructObjectNode(const TSharedPtr<FJsonObject>& Params);
};
