# J3: Audio->AI Stimulus Binding Test Spec

**Status:** Spec only — implementation tests pending I3 landing + editor restart.

**Source plan:** `Docs/plans/2026-04-26-audio-ai-stimulus-binding.md` (H3 design, I3 implementation)
**Phase J brief:** `Plugins/Monolith/Docs/plans/2026-04-25-comprehensive-fix-plan.md:341-346`
**Action surface under test:** `audio::bind_sound_to_perception`, `audio::unbind_sound_from_perception`, `audio::get_sound_perception_binding`, `audio::list_perception_bound_sounds`
**Runtime classes under test:** `UMonolithSoundPerceptionUserData`, `UMonolithAudioPerceptionSubsystem`, `UMonolithAudioPerceptionStatics::PlaySoundAndReportNoise`

---

## Setup

### Prerequisites
1. **I3 has landed** — UBT clean, `MonolithAudio` rebuilt with AIModule dep, NEW `MonolithAudioRuntime` sub-module shipped + loaded, `Monolith.uplugin` Modules array updated.
2. **Editor up**, MCP connected, project loaded.
3. **MonolithAudioRuntime module loaded** — preferred check: `editor_query("get_module_status", {module_names: ["MonolithAudioRuntime"]})` (Phase J F8) returns a row with `enabled=true, loaded=true, plugin_name="Monolith"`. Cross-check / fallback: `monolith_discover("audio")` should still return the four perception actions (`bind_sound_to_perception`, `unbind_sound_from_perception`, `get_sound_perception_binding`, `list_perception_bound_sounds`); their presence confirms the runtime sub-module ran `RegisterActions` and is loaded.

4. **Disposable test audio assets** under `/Game/Tests/Monolith/Audio/` — **NONE pre-exist** (verified 2026-04-26 — no `Content/Tests/Monolith/Audio/` directory). Author each as part of test setup:
   - `SC_PerceptionTestCue` — simple SoundCue with WavePlayer + Looping=false. ~0.5s duration. Used as the canonical bind target. Author: `audio_query("create_sound_cue", {asset_path: "/Game/Tests/Monolith/Audio/SC_PerceptionTestCue", wave_path: "<small_test_wave>"})`.
   - `SC_PerceptionLoopCue` — same but Looping=true, 1s wave. Used for loop-fire-once test. Author: `audio_query("create_sound_cue", {asset_path: "/Game/Tests/Monolith/Audio/SC_PerceptionLoopCue", wave_path: "<small_test_wave>", looping: true})` (use the SoundCue's loop node if `looping` param not exposed).
   - `MS_PerceptionTestSource` — empty MetaSoundSource (just an output gain). Used to verify AssetUserData on MetaSoundSource. Author: `audio_query("create_metasound_source", {asset_path: "/Game/Tests/Monolith/Audio/MS_PerceptionTestSource"})`.
   - `SW_PerceptionTestWave` — bare USoundWave (5kB blip). Used to verify AssetUserData on USoundWave directly. SoundWave creation requires a real `.wav` import — either reuse a project-existing tiny wave (numerous available under `Content\Sounds\` and `Plugins\Resonance\Content\`) by reference, or import a 0.5s 1kHz tone via an editor import action. If neither is reachable via MCP, fall back to Lucas-driven manual import.
   - `SC_PerceptionDisabled` — same as `SC_PerceptionTestCue` but bound with `enabled=false`. Author: `audio_query("create_sound_cue", {asset_path: "/Game/Tests/Monolith/Audio/SC_PerceptionDisabled", wave_path: "<same_wave>"})`.

5. **Disposable test scene** `/Game/Tests/Monolith/Audio/Map_AudioPerceptionH3.umap` — **DOES NOT pre-exist** (verified 2026-04-26 — no `Map_*` assets under `Content/Tests/Monolith/`). Author and populate:
   - **Map authoring:** `editor_query("create_empty_map", {path: "/Game/Tests/Monolith/Audio/Map_AudioPerceptionH3", map_template: "blank"})` (Phase J F8) — creates a fully blank UWorld asset and saves the package.
   - **Listener AI pawn `BP_TestHearingPawn`** — does NOT pre-exist; create as disposable:
     1. `blueprint_query("create_blueprint", {parent_class: "/Script/Engine.Pawn", path: "/Game/Tests/Monolith/Audio/BP_TestHearingPawn"})`
     2. `ai_query("add_perception_to_actor", {actor_bp_path: "/Game/Tests/Monolith/Audio/BP_TestHearingPawn", senses: ["Hearing"], hearing_range: 3000})` (Phase J F8) — adds the perception component + Hearing sense in one call. Replaces the prior `blueprint_query("add_component")` + manual SenseConfig wiring path.
     3. Author or assign `AAIController` BP that possesses on spawn.
     4. Test-only listener: a Blueprint event graph (or C++ helper) that subscribes to `Perception->OnPerceptionUpdated` and pushes `FActorPerceptionUpdateInfo` entries into an array readable by the test agent.
     5. `blueprint_query("compile_blueprint", {bp_path: "/Game/Tests/Monolith/Audio/BP_TestHearingPawn"})` — assert `ok=true`.
   - **Noise-source actor `BP_TestNoiseEmitter`** — does NOT pre-exist; create as disposable:
     1. `blueprint_query("create_blueprint", {parent_class: "/Script/Engine.Actor", path: "/Game/Tests/Monolith/Audio/BP_TestNoiseEmitter"})`
     2. `blueprint_query("add_component", {bp_path: "/Game/Tests/Monolith/Audio/BP_TestNoiseEmitter", component_class: "/Script/Engine.AudioComponent", component_name: "Audio"})` — set `bAutoActivate=false`; sound assigned per-test.
   - **Wall actor `BP_TestWall`** (used by TC3.3) — does NOT pre-exist; create as disposable. Either:
     - `editor_query("spawn_actor", {class_path: "/Script/Engine.StaticMeshActor", static_mesh_path: "/Engine/BasicShapes/Cube", world_transform: {...}, bBlockVisibility: true})` (preferred — pure spawn, no asset to clean up after the run), OR
     - Lucas-driven manual placement of a BSP block in the test map.
   - **Place fixtures:** Listener at world origin, emitter at (1500,0,0), wall (TC3.3 only) between them at (200,0,0) blocking visibility.

6. **No-owner emitter actor** for 2D-style sounds: not strictly required since the subsystem path falls back to `ReportNoiseEvent` directly when AC has no owner — testable via creating a transient `UAudioComponent` programmatically.

### Initial-state assertions before any test runs
- `monolith_discover("audio")` includes `bind_sound_to_perception`, `unbind_sound_from_perception`, `get_sound_perception_binding`, `list_perception_bound_sounds`.
- All test sounds initially have NO `UMonolithSoundPerceptionUserData` (verify via `get_sound_perception_binding` returns `bound: false`).
- Listener pawn in scene receives no perception events on idle (5-frame baseline check).

---

## Test Cases

### TC3.1 — Golden path: AC plays bound cue, AI hears
**Goal:** Author binding, play sound through UAudioComponent on owning actor, AI receives `Hearing` stimulus at correct loudness/range/location.
**Setup:**
- Bind `SC_PerceptionTestCue` with `loudness=1.0, max_range=2000, tag="TestPing"`.
- Open `Map_AudioPerceptionH3.umap`, place listener at (0,0,0) and emitter at (1500,0,0).
- Assign `SC_PerceptionTestCue` to emitter's UAudioComponent.
**Steps:**
1. Start PIE.
2. Wait 0.5s (subsystem init).
3. Snapshot listener perception event count.
4. Trigger `emitter->AudioComponent->Play()`.
5. Wait 10 frames (~0.16s @ 60fps).
6. Read perception event list.
**Expected:** Exactly 1 `OnPerceptionUpdated` with sense == Hearing. Stimulus location == emitter component location ((1500,0,0) ± float epsilon). Stimulus tag == "TestPing".
**Pass criteria:** Event count delta == 1. Sense matches `UAISense_Hearing`. `FAIStimulus.StimulusLocation` within `±0.5` of emitter component world location. `FAIStimulus.Tag == "TestPing"`. `FAIStimulus.Strength` proportional to (loudness * (max_range / distance)) — verify >0 (exact value depends on engine internals; at minimum non-zero).

### TC3.2 — Sound out of range
**Goal:** Bind says max_range=500, emitter is 1500uu away — listener should NOT hear.
**Setup:**
- Bind `SC_PerceptionTestCue` with `max_range=500`.
- Distance: 1500uu.
**Steps:**
1. Play sound.
2. Wait 10 frames.
3. Read event list.
**Expected:** Zero perception events.
**Pass criteria:** Event count delta == 0. No `OnPerceptionUpdated` calls. `FAINoiseEvent` was still dispatched (verify via `LogMonolithAudioRuntime Verbose` or test hook on `MakeNoise` call) — the engine's hearing-sense range check is what filters, not the action.

### TC3.3 — Hearing has no LOS check (max_range matters, walls don't)
**Goal:** Confirm hearing sense ignores LOS even with a wall between emitter and listener.
**Setup:**
- Add `BP_TestWall` (BSP block, bBlockVisibility=true) between emitter (200uu) and listener at world origin.
- Distance: 200uu < max_range=2000.
**Steps:**
1. Play sound.
2. Read event list.
**Expected:** Listener still hears (UE 5.7 hearing sense does NOT use LineOfSight). Event count == 1.
**Pass criteria:** Event count delta == 1. Confirms test against any future "I added LOS to hearing" regression. NB: this is the expected UE engine behaviour; if Leviathan project ever adds custom hearing override that does LOS, this test should FAIL and the spec should be updated.

### TC3.4 — Multiple senses (sight + hearing)
**Goal:** Listener with both Sight and Hearing senses should still receive Hearing stimulus correctly without sense-routing confusion.
**Setup:** Listener has both `UAISenseConfig_Sight` and `UAISenseConfig_Hearing`. Emitter is in line of sight at distance 1500.
**Steps:**
1. Play sound (no visible source mesh — pure audio).
2. Read event list.
**Expected:** One Hearing event. Zero Sight events (no visible target).
**Pass criteria:** Event filtered by sense class. `OnPerceptionUpdated` fired exactly once for Hearing. No false Sight event from incidental code paths.

### TC3.5 — No owning actor (2D / detached AC)
**Goal:** UAudioComponent with no owner — subsystem falls back to `ReportNoiseEvent` directly.
**Setup:**
- Create a transient UAudioComponent programmatically via `NewObject<UAudioComponent>()` on a UObject that is NOT an AActor (or AC->SetOwner(nullptr)).
- Bind `SC_PerceptionTestCue` with `loudness=1.0, max_range=2000, bRequireOwningActor=false`.
**Steps:**
1. Set component world location to (1500,0,0) (or pass it via `SetWorldLocation` — for an AC without an actor, location may need to be explicit via `SetVolumeMultiplier` + `SetPitchMultiplier` aren't location; use `SetWorldLocation`).
2. AC->Play().
3. Read event list.
**Expected:** One Hearing event. Stimulus location at AC component location.
**Pass criteria:** Event count == 1. `FAIStimulus.Source == nullptr` (no instigator). Subsystem dispatched via `UAISense_Hearing::ReportNoiseEvent` rather than `MakeNoise` — verify via separate log lines or telemetry counter.

### TC3.6 — `bRequireOwningActor=true` (default) skips no-owner
**Goal:** With default flag, no-owner AC plays do NOT fire perception.
**Setup:** As TC3.5 but `bRequireOwningActor=true` (default).
**Steps:**
1. AC with no owner plays.
2. Read event list.
**Expected:** Zero events.
**Pass criteria:** Event count delta == 0. `LogMonolithAudioRuntime Verbose` line: "skipping perception fire (no owner, bRequireOwningActor=true)".

### TC3.7 — Authority gate (HasAuthority check)
**Goal:** `MakeNoise` is `BlueprintAuthorityOnly`; on a remote client (non-authority), the subsystem must NOT fire.
**Setup:**
- Configure PIE with `Net Mode = Play As Listen Server` + 2 clients.
- Listener pawn on server. Emitter actor on server (replicated).
- Bind `SC_PerceptionTestCue`.
**Steps (per net role):**
1. On server (HasAuthority==true): emitter plays sound -> assert event fires.
2. On client (HasAuthority==false): same emitter plays sound (replicated audio) -> assert NO `MakeNoise` fired locally on the client (verify via client-side log capture; only server should fire).
3. Server's perception system should still see the event from server-side fire.
**Expected:** Server fires. Client does NOT fire (would otherwise double-count and corrupt server perception state).
**Pass criteria:** Server log shows 1 `MakeNoise` call. Client log shows 0. Subsystem implementation includes `if (!Owner || !Owner->HasAuthority()) return;` guard. If guard is missing, this TC FAILS and is a release blocker.

### TC3.8 — Dedicated server smoke
**Goal:** With dedicated server PIE config, perception still works (no dependency on client audio engine).
**Setup:** Run dedicated server (no client audio device on server).
**Note:** UAudioComponent on dedicated server typically does NOT play audio (no audio device). The subsystem hooks `OnAudioPlayStateChangedNative` — if that delegate fires on server (just because Play() was called) the perception still triggers. If audio engine is null on server, Play() may early-out.
**Steps:**
1. On dedicated server, attempt AC->Play().
2. Read perception event list (server-side).
**Expected:** Either:
  - (a) Audio engine plays the sound logically (FActiveSound exists, OnAudioPlayStateChangedNative fires) -> perception fires.
  - (b) Audio engine no-ops (no FActiveSound) -> perception does NOT fire.
**Pass criteria:** Either outcome is acceptable as long as it's CONSISTENT and DOCUMENTED. The TC's role is to verify that whichever behaviour the implementation chose, it's intentional and not a crash. NO crash, NO ensure, NO null-deref. Implementation note required if path (b): document that dedicated-server perception requires the static helper `PlaySoundAndReportNoise` rather than the auto-hook subsystem.

### TC3.9 — Fire-and-forget mitigation: `PlaySoundAndReportNoise` static
**Goal:** `UMonolithAudioPerceptionStatics::PlaySoundAndReportNoise` covers the gap that `UGameplayStatics::PlaySoundAtLocation` leaves.
**Setup:** Bind `SC_PerceptionTestCue`. NO UAudioComponent — just call the static.
**Steps:**
1. Snapshot perception event count.
2. Call `UMonolithAudioPerceptionStatics::PlaySoundAndReportNoise(World, SC_PerceptionTestCue, FVector(1500,0,0), 1.0, -1, instigator)` where `LoudnessOverride=-1` means "use UserData".
3. Wait 5 frames.
4. Read events.
**Expected:** Listener receives stimulus at (1500,0,0) with the bound loudness/range/tag.
**Pass criteria:** Event count delta == 1. Stimulus location at the call's Location parameter (NOT instigator's location). Loudness reflects UserData (since override was -1).

### TC3.10 — `LoudnessOverride` and `TagOverride` parameters
**Goal:** Static helper's override params take precedence over UserData.
**Steps:**
1. Bind cue with `loudness=1.0, tag="Default"`.
2. Call `PlaySoundAndReportNoise(..., LoudnessOverride=2.5, TagOverride="Override")`.
3. Read event.
**Expected:** Event uses Loudness=2.5 and Tag="Override".
**Pass criteria:** `FAINoiseEvent.Loudness == 2.5f`, `FAINoiseEvent.Tag == "Override"`.

### TC3.11 — `enabled=false` skips perception
**Goal:** Bound cue with `enabled=false` does NOT fire on Play.
**Setup:** Bind `SC_PerceptionDisabled` with `enabled=false`.
**Steps:**
1. AC plays the cue.
2. Read event list.
**Expected:** Zero events.
**Pass criteria:** Event count delta == 0. No `MakeNoise` call (verify via log/counter).

### TC3.12 — Audio-thread safety: 100Hz buffer doesn't multi-fire
**Goal:** A looping cue OR a long-duration sound must not spam `MakeNoise` per audio buffer (~100Hz).
**Setup:** Bind `SC_PerceptionLoopCue` (1s wave, looping). `loudness=1.0, max_range=2000`.
**Steps:**
1. AC->Play() once.
2. Wait 5.0s (ample loop iterations).
3. Read event list.
**Expected:** Exactly 1 `MakeNoise` invocation per `Play()` call (not per audio buffer, not per loop).
**Pass criteria:** Event count == 1 over 5 seconds. The reentrancy guard `AlreadyFiredThisPlay` (per H3 plan §4.4) is engaged. If the count exceeds 1 by more than 5x (allowing for legitimate restart-on-Stop/Play pairs if the test toggles play), this TC FAILS. Note: looping cues stay in `Playing` state — the engine does NOT re-enter Playing per loop iteration.

### TC3.13 — Manual stop+replay re-fires perception
**Goal:** AC->Stop() then AC->Play() should fire perception twice (legitimate use).
**Setup:** As TC3.12 but with non-looping cue.
**Steps:**
1. AC->Play(). Wait 1s.
2. AC->Stop().
3. AC->Play() again. Wait 1s.
4. Read event list.
**Expected:** Two perception events (one per Play call).
**Pass criteria:** Event count delta == 2. Reentrancy guard properly clears on `Stopped` state transition.

### TC3.14 — `fire_on_fade_in=true` does not double-fire with `Playing`
**Goal:** When `fire_on_fade_in=true`, FadingIn -> Playing transition fires exactly once total.
**Setup:** Bind cue with `fire_on_fade_in=true`. AC has fade-in time = 0.5s.
**Steps:**
1. AC->FadeIn(0.5).
2. Wait 1s (covers FadingIn -> Playing).
3. Read event list.
**Expected:** Exactly 1 perception event. Reentrancy guard fires once across both states.
**Pass criteria:** Event count == 1, not 2. Verify the guard handles the FadingIn -> Playing chain correctly.

### TC3.15 — `fire_on_fade_in=false` waits for Playing
**Goal:** With `fire_on_fade_in=false`, FadingIn does NOT fire; only Playing does.
**Setup:** Bind cue with `fire_on_fade_in=false`. AC fade-in 0.5s.
**Steps:**
1. AC->FadeIn(0.5). Snapshot at 0.25s (mid-fade).
2. Snapshot at 0.6s (post-Playing transition).
**Expected:** At 0.25s: 0 events. At 0.6s: 1 event.
**Pass criteria:** Event timing matches Playing-state arrival, not FadingIn.

### TC3.16 — Late actor spawn (subsystem auto-hooks newly spawned ACs)
**Goal:** Subsystem's `OnActorSpawned` delegate hooks ACs on actors spawned post-PIE-start.
**Setup:** PIE running. Listener exists. Subsystem initialized.
**Steps:**
1. Programmatically spawn a fresh `BP_TestNoiseEmitter` at runtime.
2. Wait 1 frame (subsystem hooks the new actor's AC).
3. AC->Play() bound cue.
4. Read events.
**Expected:** Event fires (subsystem hooked the late-spawned actor).
**Pass criteria:** Event count == 1. Confirms `World->OnActorSpawned` delegate path works.

### TC3.17 — Subsystem walks existing actors on init
**Goal:** Pre-existing actors at world-init time are hooked.
**Setup:** Place 5 emitters in `Map_AudioPerceptionH3.umap` at design time. Each has an AC with bound cue.
**Steps:**
1. Open the map, start PIE.
2. Wait 0.5s (init).
3. Trigger all 5 emitters in sequence.
4. Read events.
**Expected:** 5 events, one per emitter.
**Pass criteria:** Event count == 5, each at the correct location. Confirms `PostLoadMapWithWorld`-style startup walk works.

### TC3.18 — MetaSoundSource binding
**Goal:** AssetUserData mechanism works on `UMetaSoundSource` identically to `USoundCue` (per H3 plan open question 6).
**Setup:** Bind `MS_PerceptionTestSource` with `loudness=0.8, tag="Meta"`.
**Steps:**
1. Verify via `get_sound_perception_binding(asset_path=MS_PerceptionTestSource)` returns the binding.
2. Assign MS to emitter's AC and Play.
3. Read events.
**Expected:** Event fires with the bound loudness/tag.
**Pass criteria:** `bound: true` from get_*. Event count == 1. Tag == "Meta". Loudness == 0.8.

### TC3.19 — USoundWave direct binding
**Goal:** Same mechanism on `USoundWave`.
**Setup:** Bind `SW_PerceptionTestWave`.
**Steps:** Assign Wave to AC, play, read events.
**Expected:** Event fires.
**Pass criteria:** Event count == 1.

### TC3.20 — `unbind_sound_from_perception` removes UserData
**Goal:** Unbind cleanly removes ONLY our UserData, not other UserData on the asset.
**Setup:**
- Add a fake other UserData class instance to `SC_PerceptionTestCue` (via test helper or pre-existing engine UserData).
- Bind perception.
- Verify both UserData entries present.
**Steps:**
1. `unbind_sound_from_perception(asset_path=...)`.
2. Inspect `AssetUserData` array.
**Expected:** Only the perception UserData removed; other UserData intact.
**Pass criteria:** Array length decreased by exactly 1. Other UserData class instance still present and untouched.

### TC3.21 — `bind_sound_to_perception` is idempotent
**Goal:** Calling bind twice updates in place (no duplicate UserData).
**Steps:**
1. Bind with `loudness=0.5`.
2. Bind again with `loudness=1.5`.
3. Inspect.
**Expected:** One UserData instance, loudness=1.5.
**Pass criteria:** AssetUserData count for our class == 1. `get_*` returns the latest values.

### TC3.22 — `list_perception_bound_sounds` round-trip
**Goal:** After binding 3 distinct sounds, list returns exactly those 3.
**Setup:** Bind `SC_PerceptionTestCue, MS_PerceptionTestSource, SW_PerceptionTestWave`.
**Steps:** `list_perception_bound_sounds()`.
**Expected:** Returns exactly 3 paths in the order they were bound (or sorted — implementation choice; test asserts SET equality).
**Pass criteria:** Set equality of returned paths and the 3 bound assets. Each entry has a `class` field identifying SoundCue / MetaSoundSource / SoundWave.

---

## Per-Action Sanity

### TC3.23 — `bind_sound_to_perception` response shape
**Expected:**
```jsonc
{
  "ok": true,
  "asset_path": "/Game/Tests/Monolith/Audio/SC_PerceptionTestCue",
  "asset_class": "SoundCue",
  "binding": {
    "loudness": 1.0,
    "max_range": 2000.0,
    "tag": "TestPing",
    "sense_class": "Hearing",
    "enabled": true,
    "fire_on_fade_in": true,
    "require_owning_actor": true
  },
  "message": "..."
}
```
**Pass criteria:** All fields present. Types match.

### TC3.24 — `get_sound_perception_binding` on unbound asset
**Steps:** Call get on a fresh sound with no binding.
**Expected:**
```jsonc
{ "ok": true, "asset_path": "...", "bound": false }
```
**Pass criteria:** `bound: false`, no error.

### TC3.25 — `unbind_sound_from_perception` on unbound asset
**Expected:** `ok: true, removed: false, message: "no binding present"` (idempotent, not an error).
**Pass criteria:** `removed: false`, no error thrown.

---

## Failure Modes

| Case | Input | Expected Error |
|------|-------|----------------|
| `asset_path` doesn't exist | `/Game/Bogus/SC_None` | `ok=false, error="Sound asset not found: /Game/Bogus/SC_None"` |
| `asset_path` is not a USoundBase | path to a Static Mesh | `ok=false, error="Asset at /Game/.../SM_Foo is not a USoundBase (got StaticMesh)"` |
| `loudness` negative | `loudness=-0.5` | `ok=false, error="loudness must be >= 0"` |
| `max_range` negative | `max_range=-100` | `ok=false, error="max_range must be >= 0 (use 0 for listener default)"` |
| `tag` over 255 chars | huge string | `ok=false, error="tag exceeds 255 characters"` (FName length limit) |
| `sense_class` unknown | `sense_class="Smell"` | `ok=false, error="Unsupported sense_class 'Smell'. v1 supports: [Hearing]"` |
| `sense_class` valid future class but not mapped | `sense_class="Sight"` | `ok=false, error="sense_class 'Sight' deferred to v2"` (or similar — until visual stimuli are spec'd) |
| Subsystem hook fails (audio engine null) | dedicated server, no audio | `LogMonolithAudioRuntime Warning: skipping AC hook (no audio device)` — runtime path, not action error |
| `PlaySoundAndReportNoise` with null Sound | `Sound=nullptr` | runtime no-op, `LogMonolithAudioRuntime Warning: PlaySoundAndReportNoise called with null Sound`, no crash |
| `unbind` on path that's not a USoundBase | path to BP | `ok=false, error="Asset is not a USoundBase"` |
| `list_perception_bound_sounds` empty world | no bound assets | `ok=true, sounds=[]` (NOT an error) |
| Binding action called when AssetRegistry not ready | called during cooker startup | `ok=false, error="AssetRegistry not yet scanned; retry in editor session"` |

### Phase F11 — Audio Perception Validation Coverage (added 2026-04-26)

These rows pin the F11 validation patch — `ValidateBindingParams` pre-flight + strict `ParseSenseClass` allowlist — that closed five silent-accept seams previously logged as J3 FAILs (results doc lines 103–107). Pattern mirrors the F2/F3 hardening pass on `MonolithGASUIBindingActions.cpp`. All checks apply to `bind_sound_to_perception` only; sibling read/list actions take no validation-relevant params.

| TC ID | Case | Input | Expected |
|-------|------|-------|----------|
| J3-Validate-Loudness-Negative | `bind_sound_to_perception` rejects negative loudness | `loudness=-0.5` | `ok=false, error="loudness must be >= 0"` |
| J3-Validate-Loudness-Zero | `bind_sound_to_perception` accepts zero loudness (mute is valid) | `loudness=0.0` | `ok=true, binding.loudness=0.0` (only negatives error; zero is legitimate mute value) |
| J3-Validate-MaxRange-Negative | `bind_sound_to_perception` rejects negative max_range | `max_range=-100.0` | `ok=false, error="max_range must be >= 0 (use 0 for listener default)"` |
| J3-Validate-MaxRange-Zero | `bind_sound_to_perception` accepts zero max_range (listener-default sentinel) | `max_range=0.0` | `ok=true, binding.max_range=0.0` |
| J3-Validate-Tag-Length-256 | `bind_sound_to_perception` rejects 256-char tag (one over project soft-cap) | `tag` of exactly 256 chars | `ok=false, error="tag exceeds 255 characters"` |
| J3-Validate-Tag-Length-255 | `bind_sound_to_perception` accepts 255-char tag (boundary inclusive) | `tag` of exactly 255 chars | `ok=true` |
| J3-Validate-Tag-Empty | `bind_sound_to_perception` accepts empty tag (back-compat — stored as NAME_None) | `tag=""` | `ok=true, binding.tag=""` (round-trips as empty FName) |
| J3-Validate-SenseClass-Unknown | `bind_sound_to_perception` rejects unknown sense_class | `sense_class="Smell"` | `ok=false, error="Unsupported sense_class 'Smell'. v1 supports: [Hearing]"` |
| J3-Validate-SenseClass-Sight | `bind_sound_to_perception` rejects deferred-to-v2 Sight class with distinct error | `sense_class="Sight"` | `ok=false, error="sense_class 'Sight' deferred to v2"` |
| J3-Validate-SenseClass-Damage | `bind_sound_to_perception` rejects deferred-to-v2 Damage class with distinct error | `sense_class="Damage"` | `ok=false, error="sense_class 'Damage' deferred to v2"` |
| J3-Validate-SenseClass-Empty | `bind_sound_to_perception` accepts empty sense_class (back-compat — defaults to UAISense_Hearing) | `sense_class=""` | `ok=true, binding.sense_class="AISense_Hearing"` |
| J3-Validate-SenseClass-LowercaseHearing | `bind_sound_to_perception` accepts lowercase 'hearing' (case-insensitive match) | `sense_class="hearing"` | `ok=true, binding.sense_class="AISense_Hearing"` (preserved from prior `ResolveSenseClass` behaviour) |

---

## Notes

### Open questions for I3 implementation team
1. **AC enumeration strategy (H3 plan open question 1)** — TC3.16 + TC3.17 both pass only if the implementation handles BOTH paths (OnActorSpawned for new + startup walk for existing). If implementation chose only one, the missing TC will FAIL.
2. **Phase-2 SoundNode (H3 plan open question 2)** — out of scope for J3. Reserve TC3.26+ for if/when `USoundNode_PerceptionEmitter` lands.
3. **Authority gate (H3 plan open question 7)** — TC3.7 is critical. If the subsystem fires on clients, multiplayer perception will desync. Implementation MUST gate on `HasAuthority()` for owning-actor path.
4. **Audio thread name (H3 plan open question 8)** — TC3.12 + TC3.13 + TC3.14 indirectly verify game-thread dispatch (any audio-thread fire would race-corrupt). Implementation should add `IsInGameThread()` ensure in the handler.
5. **Dedicated server behaviour (TC3.8)** — implementation should DOCUMENT whichever path it took. If audio engine no-ops on dedicated server, the perception path doesn't fire, and projects needing AI-on-server-from-sound need the static helper. This is acceptable but must be in the spec doc.
6. **2D-sound location (TC3.5)** — UAudioComponent on a non-actor UObject doesn't have a sensible world location. Implementation may need to require an explicit `SetWorldLocation` or fall back to listener location. Test currently assumes explicit location via `SetWorldLocation`; flag if the implementation uses the listener fallback.
7. **TC3.7 client log capture** — capturing client-side logs from a 2-player PIE requires either a test-only listener pawn on each client or a multiplayer log harness. If unavailable, defer TC3.7 to manual smoke test (Lucas-driven, agents can't simulate multiplayer per `feedback_agents_cant_play_game`).

### Dependencies on I-phase landing
- I3 must ship before any TC runs.
- New `MonolithAudioRuntime` sub-module must be added to `Monolith.uplugin` Modules array AND build as Type:Runtime.
- `Map_AudioPerceptionH3.umap` is a test fixture, authored during test setup, not in I3 deliverables.
- TC3.7 / TC3.8 require multiplayer or dedicated-server PIE configs — manual setup per Lucas (agents can spawn but can't drive multiplayer playthroughs).

### Coverage map (per H3 plan §6 Test Plan)
| H3 plan test | This spec maps to | Notes |
|---|---|---|
| BindUserDataPersists | TC3.21 + reload check | Add explicit reload step to TC3.21 |
| UnbindRemovesUserData | TC3.20 | Direct map |
| BindIsIdempotent | TC3.21 | Direct map |
| GetReturnsCurrent | TC3.24 + TC3.21 second-bind | Direct map |
| ListSurfacesAllBound | TC3.22 | Direct map |
| MakeNoiseFiresOnPlay | TC3.1 | Direct map |
| NoFireWhenDisabled | TC3.11 | Direct map |
| NoOwnerSkipsWhenRequired | TC3.6 | Direct map |
| RangeAndLoudnessForwarded | TC3.1 + TC3.10 | Direct map |
| MetaSoundSourceCovered | TC3.18 | Direct map |
| PlaySoundAndReportNoiseHelperFires | TC3.9 + TC3.10 | Direct map |
| PIE seed scene smoke | TC3.1 | Direct map |
| Negative seed (enabled=false) | TC3.11 | Direct map |

Total this spec: **25 test cases + 11 failure-mode rows**.

### Issues uncovered while reading the H3 plan
- **The H3 plan §3.3 Approach C "Cons" list** flags subsystem actor-spawn enumeration as the "most likely friction point" but does not make a final decision. TC3.16 + TC3.17 lock-in both paths as required behaviour; if I3 ships only one, the other test fails. Recommend I3 ships BOTH paths in the same patch.
- **The fire-and-forget gap (`PlaySoundAtLocation`)** is documented but covered ONLY by the static helper. Projects using legacy `PlaySoundAtLocation` in untouched code will silently miss perception. Spec doc should call this out prominently in `SPEC_MonolithAudio.md`.
- **`bRequireOwningActor` default** — H3 plan defaults it `true`. Test TC3.5 / TC3.6 both pass against this default. If a project author wants 2D ambient audio to fire perception (e.g., a thunderclap), they must explicitly set `bRequireOwningActor=false`. Recommend a TC3.27 manual-confirmation test for this UX.
