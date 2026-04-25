# Cooked-Build Support for Runtime Classes — TODO

**Status:** UNRESOLVED. Affects Phase I1, I2, I3 features (UI↔GAS binding, BT↔GAS task, Audio→AI stimulus).

**Decision (2026-04-25):** Path 3 — accept PIE/editor-only for now, document loudly, revisit before Steam launch.

---

## The Problem

Monolith is an **editor-only** plugin. It does not ship to cooked game builds (Steam release etc.). However, three Phase I features introduce **runtime classes** that content assets reference:

| Phase | Runtime class | Module type | Cooked-build behaviour |
|-------|---------------|-------------|------------------------|
| **I1** | `UMonolithGASAttributeBindingClassExtension` | MonolithGAS (Editor) | Class missing → WBP load warning, bindings silently dropped |
| **I2** | `UBTTask_TryActivateAbility` | MonolithAI (Editor) | Class missing → BehaviorTree load fails, task node unresolvable |
| **I3** | `UMonolithSoundPerceptionUserData` + `UMonolithAudioPerceptionSubsystem` | MonolithAudioRuntime (Runtime) | MonolithAudioRuntime sub-module not shipped → UAssetUserData reference dangling, subsystem never instantiated |

Even though I3's sub-module is `Type: Runtime`, it's still inside the Monolith plugin. If Monolith isn't shipped with the game, the runtime sub-module isn't either.

## Current Effect

- **PIE / editor:** All three features work as designed. Author content via Monolith MCP actions; runtime behaviour fires as expected during PIE.
- **Cooked Steam build:** Content assets retain spec data, but the classes that execute that data are absent. Behaviour silently no-ops at runtime. Likely log warnings about missing classes; bindings/tasks/perception emitters do nothing.

## Resolution Options (deferred)

### Option A — Drop runtime classes entirely (cleanest)
Replace each Phase I feature with stock-UE-only mechanisms:

- **I1:** Use UMG `PropertyBindings` + a runtime delegate-driven binding system that exists in stock UE 5.7. Monolith action stamps `FDelegateRuntimeBinding` entries onto the WBP — these are engine-native and survive cooking.
- **I2:** Use the GAS-companion plugin's `BTTask_RunGameplayAbility` if available, or a stock engine BTTask if present. If neither exists, emit a Blueprint-graph BT that uses stock nodes (RunBehaviorTree → custom event → ASC->TryActivateAbility).
- **I3:** Stamp metadata onto the SoundCue via stock `UAssetUserData` subclass that lives in the *project's own* runtime module (LeviathanGameplay or similar), not in Monolith. The audio component listener subsystem also lives in project code.

**Effort:** Substantial — likely re-architects all three features (~30-40h).
**Trade-off:** Less feature flexibility; some H-plan capabilities may not be expressible via stock UE.

### Option B — Sibling shipping plugin
New plugin `LeviathanRuntimeBindings` (or similar) — Type: Runtime, ships in the cooked game. Holds the three runtime classes. Monolith authors INTO this plugin's classes.

**Effort:** ~2-3 days of refactor.
**Trade-off:** New plugin to maintain; users of Monolith outside this project would also need this plugin or write their own equivalent.

### Option C — Status quo (CURRENT)
Document the PIE-only limitation. Useful for prototyping. **Steam launch blocker if any of these features are required at runtime.**

## Pre-Steam-Launch Checklist

Before Steam release, REVISIT this TODO:

- [ ] Audit which I-phase features are actually used in shipping game content
- [ ] If unused: delete authoring actions from Monolith, keep editor functionality
- [ ] If used: pick Option A or B per feature, schedule the refactor
- [ ] Add cooked-build smoke test that loads representative WBP/BT/SoundCue assets and verifies no warnings

## Cross-References

- Phase I1 plan: `Docs/plans/2026-04-26-ui-gas-attribute-binding.md`
- Phase I2 plan: `Docs/plans/2026-04-26-bt-gas-ability-task.md`
- Phase I3 plan: `Docs/plans/2026-04-26-audio-ai-stimulus-binding.md`
- Comprehensive fix plan: `Plugins/Monolith/Docs/plans/2026-04-25-comprehensive-fix-plan.md`

## Owner

TBD — flag at next planning checkpoint.
