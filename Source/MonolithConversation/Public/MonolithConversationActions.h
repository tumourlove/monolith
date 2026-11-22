#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Authoring actions for the `conversation` namespace (CommonConversation graph assets).
 *
 * Nodes are created through the engine's own graph schema action (FAISchemaAction_NewNode) and the
 * result is compiled with FConversationCompiler, exactly as the Conversation editor does, so assets
 * created here open and edit normally in the graph editor.
 *
 * Only compiled when the CommonConversation plugin is present (WITH_COMMONCONVERSATION).
 */
class FMonolithConversationActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	/** Create an empty, graph-initialised ConversationDatabase asset. */
	static FMonolithActionResult CreateConversationDatabase(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Replace a database's graph with Entry Point -> Task -> Task ... (a linear chain), set its speakers,
	 * compile it and optionally save it.
	 */
	static FMonolithActionResult BuildFromSpec(const TSharedPtr<FJsonObject>& Params);
};
