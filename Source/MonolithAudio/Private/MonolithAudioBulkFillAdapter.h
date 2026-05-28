// SPDX-License-Identifier: MIT
// Private declaration for Phase 5 Step 4 audio adapter.

#pragma once

#include "CoreMinimal.h"

struct FBulkFillSpec;
struct FDryRunReport;
struct FSchemaDescriptor;

/**
 * Phase 5 Step 4 — bulk_fill / describe adapter for target_namespace="audio".
 * Self-registers with FMonolithBulkFillRegistry from FMonolithAudioModule::StartupModule.
 *
 * **H5 stub-adapter invariant:** Register() ALWAYS runs from StartupModule,
 * regardless of WITH_METASOUND. The adapter body splits per fill_kind / per-call:
 *   * VANILLA AUDIO PATHS (gate-free): USoundAttenuation / USoundConcurrency
 *     reflection writes. These run regardless of WITH_METASOUND.
 *   * METASOUND PATHS (`#if WITH_METASOUND` gated): MetaSound builder-handle
 *     fill_kind for inputs/defaults. `#else` branch returns a clean
 *     "MetaSound not available — WITH_METASOUND=0 in this build" error.
 *
 * M6 invariant (post-review lock): MetaSoundEngine + MetaSoundFrontend are
 * dumpbin-sentinel-listed in make_release.ps1 $LeakSentinels — hard-linking
 * them would trip the release smoke. MonolithAudio.Build.cs already does the
 * single-location probe; this adapter respects that gate.
 *
 * Tree shape (per design B.2 / plan §Phase 5 Step 4):
 *
 *   1. fill_kind=Attenuation — vanilla path (always available):
 *      {
 *        "fill_kind": "Attenuation",
 *        "properties": {
 *          "bAttenuate": true,
 *          "AttenuationShape": "Sphere",
 *          "FalloffDistance": 2000.0,
 *          "AttenuationLowpassFilterFrequency": 8000.0
 *        }
 *      }
 *
 *   2. fill_kind=Concurrency — vanilla path:
 *      {
 *        "fill_kind": "Concurrency",
 *        "properties": {
 *          "MaxCount": 16,
 *          "ResolutionRule": "StopOldest",
 *          "VolumeScale": 0.5
 *        }
 *      }
 *
 *   3. fill_kind=MetaSoundInputs — gated `#if WITH_METASOUND`:
 *      {
 *        "fill_kind": "MetaSoundInputs",
 *        "inputs": { "Gain": 0.5, "PitchShift": 1.2, ... }
 *      }
 */
class FMonolithAudioBulkFillAdapter
{
public:
	static void Register();
	static void Unregister();
	static FDryRunReport AudioBulkFill(const FBulkFillSpec& Spec);
	static FSchemaDescriptor AudioDescribe(const FString& TargetAsset);
};
