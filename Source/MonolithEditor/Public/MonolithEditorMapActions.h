// MonolithEditorMapActions.h
// Phase F8 (J-phase) — empty map authoring + module/plugin reflection.
//
//   editor::create_empty_map       — Create a fully blank UWorld asset on disk
//                                    via UWorldFactory + IAssetTools::CreateAsset.
//   editor::get_module_status      — Reflect plugin enable + module load status
//                                    for Monolith (or arbitrary) modules. Wraps
//                                    IPluginManager::GetDiscoveredPlugins +
//                                    FModuleManager::IsModuleLoaded.
//
// Both actions are project-agnostic (no Leviathan-specific symbols) and live in
// the editor-only MonolithEditor module. Registration is invoked from
// FMonolithEditorModule::StartupModule via FMonolithEditorMapActions::RegisterActions.
#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithEditorMapActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult HandleCreateEmptyMap(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetModuleStatus(const TSharedPtr<FJsonObject>& Params);
};
