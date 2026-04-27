#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Level Sequence domain action handlers for Monolith.
 * Introspection of ULevelSequence Director Blueprints, their functions/variables,
 * and event-track binding GUID -> director-function mappings.
 */
class FMonolithLevelSequenceActions
{
public:
	/** Register all level_sequence actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// --- Action handlers ---
	static FMonolithActionResult Ping(const TSharedPtr<FJsonObject>& Params);

	/**
	 * List all Level Sequences that have a Director Blueprint, with the
	 * director's name and total function/variable counts. Optional
	 * asset_path_filter is a glob (* and ?) matched against ls_path.
	 */
	static FMonolithActionResult ListDirectors(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Get summary information for a single Level Sequence Director:
	 * function counts grouped by kind, variable count, event-binding
	 * counts (total + how many resolve to a Director function), and a
	 * sample of up to 10 functions for quick orientation.
	 */
	static FMonolithActionResult GetDirectorInfo(const TSharedPtr<FJsonObject>& Params);

	/**
	 * List a Director's own functions, optionally filtered by kind:
	 *   "user"               — declared in DirBP->FunctionGraphs
	 *   "custom_event"       — UK2Node_CustomEvent inside an event graph
	 *   "sequencer_endpoint" — UE-generated UFunction backing a Sequencer
	 *                          "Quick Bind" / "Create New Endpoint" event entry
	 *   "event"              — alias for (custom_event ∪ sequencer_endpoint)
	 *   "all" / unset        — every kind
	 * Each row carries name, kind, and parameter signature (parsed from JSON).
	 */
	static FMonolithActionResult ListDirectorFunctions(const TSharedPtr<FJsonObject>& Params);

	/**
	 * List all event-track bindings inside one Level Sequence, grouped by
	 * binding GUID. Each binding describes either a Possessable (existing
	 * level actor), a Spawnable (template-spawned), or a master track
	 * (no GUID). Inside each binding is an array of sections (trigger /
	 * repeater) with the Director function each one fires (resolved name
	 * and kind when available).
	 */
	static FMonolithActionResult ListEventBindings(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Cross-sequence reverse lookup: given a Director function name,
	 * return every event-track section across the project that fires it
	 * (with the LS path and binding context). Optional asset_path_filter
	 * glob narrows the search.
	 */
	static FMonolithActionResult FindDirectorFunctionCallers(const TSharedPtr<FJsonObject>& Params);

	/**
	 * List a Director's variables (name + K2-schema-formatted type) in
	 * declaration order. Variables come from DirBP->NewVariables.
	 */
	static FMonolithActionResult ListDirectorVariables(const TSharedPtr<FJsonObject>& Params);

	/**
	 * List ALL bindings inside one Level Sequence (one row per Guid×BindingIndex),
	 * regardless of whether the binding has event tracks. UE 5.7 distinguishes:
	 *   possessable  — plain possessable, no UMovieSceneCustomBinding
	 *   spawnable    — legacy FMovieSceneSpawnable OR custom UMovieSceneSpawnableBindingBase
	 *   replaceable  — custom UMovieSceneReplaceableBindingBase
	 *   custom       — any other UMovieSceneCustomBinding subclass
	 * For custom bindings, custom_binding_class names the exact UCLASS (e.g.
	 * MovieSceneSpawnableActorBinding) and custom_binding_pretty carries the
	 * editor-facing label.
	 */
	static FMonolithActionResult ListBindings(const TSharedPtr<FJsonObject>& Params);
};
