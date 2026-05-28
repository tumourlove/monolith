#include "MonolithUIModule.h"
#include "MonolithUIActions.h"
#include "MonolithUISlotActions.h"
#include "MonolithUITemplateActions.h"
#include "MonolithUIStylingActions.h"
#include "MonolithUIAnimationActions.h"
#include "MonolithUIBindingActions.h"
#include "MonolithUISettingsActions.h"
#include "MonolithUIAccessibilityActions.h"
#include "MonolithUIRegistryActions.h"
#include "MonolithSettings.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"   // Phase G: dump_style_cache_stats schema
#include "MonolithUICommon.h"
#include "Editor.h"
#include "Misc/CoreDelegates.h"
#include "Registry/MonolithUIRegistrySubsystem.h"

// Hoisted action handlers (texture/font ingest, animation v2, eased segment,
// spring bake, animation event tracks + delegate bindings, rounded corners,
// box shadow compositor, gradient MID factory).
#include "Actions/Hoisted/TextureIngestActions.h"
#include "Actions/Hoisted/FontIngestActions.h"
#include "Actions/Hoisted/AnimationCoreActions.h"
#include "Actions/Hoisted/AnimationEventActions.h"
#include "Actions/Hoisted/RoundedCornerActions.h"
#include "Actions/Hoisted/ShadowActions.h"
#include "Actions/Hoisted/GradientActions.h"

// Phase F (2026-04-26) -- 9 sub-bag setters + 1 preset action targeting
// the optional EffectSurface provider widget. Composes against the
// allowlist-gated JSON path surface seeded in Phase E.
#include "Actions/MonolithUIEffectActions.h"

// Phase H (2026-04-26) -- transactional `build_ui_from_spec` + schema dump.
// The centerpiece LLM-facing entry point: takes a FUISpecDocument JSON and
// produces a working UWidgetBlueprint in one MCP call. Composes everything
// from Phases A-G+I.
#include "Actions/MonolithUISpecActions.h"

// Phase 4 (2026-05-11) — bulk_fill / describe adapter for target_namespace="ui".
// Per H5 stub-adapter invariant: Register() ALWAYS runs from StartupModule,
// regardless of WITH_COMMONUI. The adapter BODY splits per fill_kind:
//   * vanilla UMG paths (DataTableRows, WidgetProperties, slot-prop describe)
//     run gate-free
//   * CommonUI-specific paths (InputActionDataTable) are #if WITH_COMMONUI
//     gated INSIDE the adapter with a clean stub error in the #else branch.
// SINGLE-TRANSACTION INVARIANT: the 40-row input-action DT write commits as
// ONE FScopedTransaction + ONE Modify + N AddRow + ONE MarkPackageDirty —
// THE fix for the 2026-04-25 parallel-burst editor crash.
#include "MonolithUIBulkFillAdapter.h"

#if WITH_COMMONUI
#include "CommonUI/MonolithCommonUIActions.h"
#include "Style/MonolithUIStyleService.h"   // Phase G: cache stats + shutdown
#endif

#define LOCTEXT_NAMESPACE "MonolithUI"

namespace
{
    /**
     * Module-scoped delegate handle for the OnPostEngineInit re-scan. Owned at
     * file scope so the module's StartupModule/ShutdownModule symmetry can
     * unregister it cleanly. The subsystem itself can't own this handle —
     * editor subsystems initialise AFTER OnPostEngineInit, so the listener
     * has to be installed earlier (here).
     */
    FDelegateHandle GMonolithUIPostEngineInitHandle;
}

void FMonolithUIModule::StartupModule()
{
    if (!GetDefault<UMonolithSettings>()->bEnableUI) return;

    FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
    FMonolithUIActions::RegisterActions(Registry);
    FMonolithUISlotActions::RegisterActions(Registry);
    FMonolithUITemplateActions::RegisterActions(Registry);
    FMonolithUIStylingActions::RegisterActions(Registry);
    FMonolithUIAnimationActions::RegisterActions(Registry);
    FMonolithUIBindingActions::RegisterActions(Registry);
    FMonolithUISettingsActions::RegisterActions(Registry);
    FMonolithUIAccessibilityActions::RegisterActions(Registry);
    FMonolithUIRegistryActions::RegisterActions(Registry);

    // Hoisted action set -- generic verbs registered under the ui:: namespace.
    MonolithUI::FTextureIngestActions::Register(Registry);
    MonolithUI::FFontIngestActions::Register(Registry);
    MonolithUI::FAnimationCoreActions::Register(Registry);
    MonolithUI::FAnimationEventActions::Register(Registry);
    MonolithUI::FRoundedCornerActions::Register(Registry);
    MonolithUI::FShadowActions::Register(Registry);
    MonolithUI::FGradientActions::Register(Registry);

    // Phase F (2026-04-26) -- EffectSurface sub-bag setters + preset.
    // 10 actions total in ui::set_effect_surface_* + ui::apply_effect_surface_preset.
    MonolithUI::FEffectSurfaceActions::Register(Registry);

    // Phase H (2026-04-26) -- transactional spec builder + schema dump.
    // 2 actions: ui::build_ui_from_spec (the centerpiece) + ui::dump_ui_spec_schema.
    MonolithUI::FSpecActions::Register(Registry);

    // Phase 4 (2026-05-11) — bulk_fill / describe adapter registration.
    // H5 stub-adapter invariant: register call ALWAYS runs, regardless of
    // WITH_COMMONUI. Body splits on fill_kind inside the adapter — vanilla UMG
    // paths run gate-free; CommonUI paths (InputActionDataTable) are gated
    // #if WITH_COMMONUI INSIDE the adapter with a clean stub error fallback.
    FMonolithUIBulkFillAdapter::Register();

#if WITH_COMMONUI
    FMonolithCommonUIActions::RegisterAll(Registry);

    // Phase G — diagnostic action that exposes the style service cache state.
    // Lives here (in the module file) rather than in a per-feature actions
    // file because it has zero dependencies beyond the service singleton, and
    // a one-action module-level register keeps Phase G's footprint minimal.
    Registry.RegisterAction(
        TEXT("ui"), TEXT("dump_style_cache_stats"),
        TEXT("Return live FMonolithUIStyleService cache stats: cache_size, hits, misses, evictions, "
             "and per-type counts (Button/Text/Border). Diagnostic for the Phase G dedup work."),
        FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>& /*Params*/) -> FMonolithActionResult
        {
            const FUIStyleCacheStats Stats = FMonolithUIStyleService::Get().GetStats();

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("cache_size"),  Stats.CacheSize);
            Result->SetNumberField(TEXT("hits"),        static_cast<double>(Stats.Hits));
            Result->SetNumberField(TEXT("misses"),      static_cast<double>(Stats.Misses));
            Result->SetNumberField(TEXT("evictions"),   static_cast<double>(Stats.Evictions));

            TSharedPtr<FJsonObject> ByType = MakeShared<FJsonObject>();
            ByType->SetNumberField(TEXT("Button"), Stats.ButtonCount);
            ByType->SetNumberField(TEXT("Text"),   Stats.TextCount);
            ByType->SetNumberField(TEXT("Border"), Stats.BorderCount);
            Result->SetObjectField(TEXT("by_type"), ByType);

            return FMonolithActionResult::Success(Result);
        }),
        FParamSchemaBuilder().Build(),
        TEXT("Diagnostics"));
#endif

    // OnPostEngineInit re-scan: editor subsystems initialise AFTER OnPostEngineInit
    // has fired, so the SUBSYSTEM's own Initialize cannot listen to it. The
    // module's StartupModule runs earlier — register here, dispatch into the
    // subsystem when it fires.
    //
    // Why this matters: marketplace/plugin-provided UMG widget classes (e.g.
    // CommonUI on a fresh install) load AFTER stock UMG. Without this re-scan,
    // those classes are missing from the registry until something forces a
    // RescanWidgetTypes call.
    GMonolithUIPostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddLambda([]()
    {
        if (UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get())
        {
            Sub->RescanWidgetTypes();
            UE_LOG(LogMonolithUISpec, Log,
                TEXT("MonolithUI: OnPostEngineInit re-scan dispatched to UMonolithUIRegistrySubsystem"));
        }
        else
        {
            UE_LOG(LogMonolithUISpec, Verbose,
                TEXT("MonolithUI: OnPostEngineInit fired but registry subsystem not available yet — initial Initialize walk will cover stock UMG"));
        }
    });

    // Dynamic action count — reflects base UMG + any conditionally-registered CommonUI actions.
    const int32 UINamespaceActions = Registry.GetActions(TEXT("ui")).Num();
    UE_LOG(LogMonolith, Log, TEXT("Monolith — UI module loaded (%d ui actions)"), UINamespaceActions);
}

void FMonolithUIModule::ShutdownModule()
{
    if (GMonolithUIPostEngineInitHandle.IsValid())
    {
        FCoreDelegates::OnPostEngineInit.Remove(GMonolithUIPostEngineInitHandle);
        GMonolithUIPostEngineInitHandle.Reset();
    }

    // Phase 4 (2026-05-11) — symmetric unregister of the bulk_fill / describe
    // adapter. Mirrors the unconditional Register() in StartupModule.
    FMonolithUIBulkFillAdapter::Unregister();

    FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("ui"));

#if WITH_COMMONUI
    // Phase G — release cached UClass strong-refs before the module DLL
    // unloads. Otherwise TStrongObjectPtr<UClass> entries log a "leaked"
    // warning at editor exit.
    FMonolithUIStyleService::Shutdown();
#endif
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithUIModule, MonolithUI)
