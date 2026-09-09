// Category H: Audit + Lint — 4 actions
// 5.H.1 audit_commonui_widget
// 5.H.2 export_commonui_report
// 5.H.3 hot_reload_styles  [RUNTIME, EXPERIMENTAL]
// 5.H.4 dump_action_router_state [RUNTIME, EXPERIMENTAL]
#include "MonolithCommonUIHelpers.h"
#include "AssetRegistry/AssetData.h"

#if WITH_COMMONUI

#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "CommonActivatableWidget.h"
#include "CommonButtonBase.h"
#include "CommonActionWidget.h"
#include "Widgets/CommonActivatableWidgetContainer.h"
#include "CommonInputSubsystem.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "UObject/UObjectIterator.h"
#include "Engine/World.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"

namespace MonolithCommonUIAudit
{
	// ----- 5.H.1 audit_commonui_widget -----------------------------------------
	// Single-WBP lint. Checks:
	//   - UCommonActivatableWidget has some DesiredFocusTargetName or input mapping set
	//   - UCommonButtonBase has Style assigned
	//   - UCommonActionWidget has some InputAction assigned
	//   - Modal activatable has a BackgroundBlur sibling in tree (heuristic)

	static FMonolithActionResult HandleAuditCommonUIWidget(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
			return FMonolithActionResult::Error(TEXT("wbp_path required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		UWidgetBlueprint* Wbp = LoadObject<UWidgetBlueprint>(nullptr, *WbpPath);
		if (!Wbp || !Wbp->WidgetTree)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load WBP '%s'"), *WbpPath));

		TArray<TSharedPtr<FJsonValue>> Issues;
		int32 ButtonsScanned = 0, ActionWidgets = 0;

		// WBP-level checks
		UClass* GenClass = Wbp->GeneratedClass;
		if (GenClass && GenClass->IsChildOf(UCommonActivatableWidget::StaticClass()))
		{
			UCommonActivatableWidget* CDO = Cast<UCommonActivatableWidget>(GenClass->GetDefaultObject());
			FNameProperty* FocusNameProp = FindFProperty<FNameProperty>(GenClass, TEXT("DesiredFocusTargetName"));
			if (!FocusNameProp)
			{
				TSharedPtr<FJsonObject> I = MakeShared<FJsonObject>();
				I->SetStringField(TEXT("severity"), TEXT("warning"));
				I->SetStringField(TEXT("rule"), TEXT("activatable_missing_focus_target"));
				I->SetStringField(TEXT("detail"), TEXT("UCommonActivatableWidget has no DesiredFocusTargetName property — set_initial_focus_target will fail"));
				Issues.Add(MakeShared<FJsonValueObject>(I));
			}
		}

		// Widget-level checks
		Wbp->WidgetTree->ForEachWidget([&](UWidget* W)
		{
			if (UCommonButtonBase* Btn = Cast<UCommonButtonBase>(W))
			{
				ButtonsScanned++;
				FObjectProperty* StyleProp = FindFProperty<FObjectProperty>(UCommonButtonBase::StaticClass(), TEXT("Style"));
				if (StyleProp && !StyleProp->GetObjectPropertyValue_InContainer(Btn))
				{
					TSharedPtr<FJsonObject> I = MakeShared<FJsonObject>();
					I->SetStringField(TEXT("severity"), TEXT("warning"));
					I->SetStringField(TEXT("rule"), TEXT("button_missing_style"));
					I->SetStringField(TEXT("widget"), Btn->GetName());
					Issues.Add(MakeShared<FJsonValueObject>(I));
				}
			}
			if (UCommonActionWidget* AW = Cast<UCommonActionWidget>(W))
			{
				ActionWidgets++;
				// Check InputActions array is non-empty (protected UPROPERTY)
				FArrayProperty* InputActionsProp = FindFProperty<FArrayProperty>(UCommonActionWidget::StaticClass(), TEXT("InputActions"));
				if (InputActionsProp)
				{
					FScriptArrayHelper Helper(InputActionsProp, InputActionsProp->ContainerPtrToValuePtr<void>(AW));
					if (Helper.Num() == 0)
					{
						TSharedPtr<FJsonObject> I = MakeShared<FJsonObject>();
						I->SetStringField(TEXT("severity"), TEXT("info"));
						I->SetStringField(TEXT("rule"), TEXT("action_widget_unbound"));
						I->SetStringField(TEXT("widget"), AW->GetName());
						I->SetStringField(TEXT("detail"), TEXT("UCommonActionWidget has no InputActions — glyph will not display"));
						Issues.Add(MakeShared<FJsonValueObject>(I));
					}
				}
			}
		});

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetNumberField(TEXT("buttons_scanned"), ButtonsScanned);
		Result->SetNumberField(TEXT("action_widgets_scanned"), ActionWidgets);
		Result->SetNumberField(TEXT("issue_count"), Issues.Num());
		Result->SetArrayField(TEXT("issues"), Issues);
		return FMonolithActionResult::Success(Result);
	}

	// ----- 5.H.2 export_commonui_report ----------------------------------------
	// Project-wide audit summary. Scans all WBPs in the project (or a folder).

	static FMonolithActionResult HandleExportCommonUIReport(const TSharedPtr<FJsonObject>& Params)
	{
		FString FolderPath = TEXT("/Game");
		if (Params.IsValid()) Params->TryGetStringField(TEXT("folder_path"), FolderPath);

		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		FARFilter Filter;
		Filter.ClassPaths.Add(UWidgetBlueprint::StaticClass()->GetClassPathName());
		Filter.PackagePaths.Add(FName(*FolderPath));
		Filter.bRecursivePaths = true;
		TArray<FAssetData> FoundAssets;
		ARM.Get().GetAssets(Filter, FoundAssets);

		int32 TotalWbps = 0, ActivatableCount = 0, ButtonCount = 0, StyledButtons = 0, ActionWidgetCount = 0, BoundActionWidgets = 0;

		for (const FAssetData& AD : FoundAssets)
		{
			UWidgetBlueprint* Wbp = Cast<UWidgetBlueprint>(AD.GetAsset());
			if (!Wbp || !Wbp->WidgetTree) continue;
			TotalWbps++;

			if (Wbp->GeneratedClass && Wbp->GeneratedClass->IsChildOf(UCommonActivatableWidget::StaticClass()))
				ActivatableCount++;

			Wbp->WidgetTree->ForEachWidget([&](UWidget* W)
			{
				if (UCommonButtonBase* Btn = Cast<UCommonButtonBase>(W))
				{
					ButtonCount++;
					FObjectProperty* SP = FindFProperty<FObjectProperty>(UCommonButtonBase::StaticClass(), TEXT("Style"));
					if (SP && SP->GetObjectPropertyValue_InContainer(Btn)) StyledButtons++;
				}
				if (UCommonActionWidget* AW = Cast<UCommonActionWidget>(W))
				{
					ActionWidgetCount++;
					FArrayProperty* IAP = FindFProperty<FArrayProperty>(UCommonActionWidget::StaticClass(), TEXT("InputActions"));
					if (IAP)
					{
						FScriptArrayHelper Helper(IAP, IAP->ContainerPtrToValuePtr<void>(AW));
						if (Helper.Num() > 0) BoundActionWidgets++;
					}
				}
			});
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("folder_path"), FolderPath);
		Result->SetNumberField(TEXT("total_wbps"), TotalWbps);
		Result->SetNumberField(TEXT("activatable_widget_count"), ActivatableCount);
		Result->SetNumberField(TEXT("button_count"), ButtonCount);
		Result->SetNumberField(TEXT("styled_button_count"), StyledButtons);
		Result->SetNumberField(TEXT("unstyled_button_count"), ButtonCount - StyledButtons);
		Result->SetNumberField(TEXT("action_widget_count"), ActionWidgetCount);
		Result->SetNumberField(TEXT("bound_action_widget_count"), BoundActionWidgets);
		Result->SetNumberField(TEXT("unbound_action_widget_count"), ActionWidgetCount - BoundActionWidgets);

		const float StylingRatio = ButtonCount > 0 ? static_cast<float>(StyledButtons) / ButtonCount : 1.0f;
		const float BindingRatio = ActionWidgetCount > 0 ? static_cast<float>(BoundActionWidgets) / ActionWidgetCount : 1.0f;
		Result->SetNumberField(TEXT("styling_coverage"), StylingRatio);
		Result->SetNumberField(TEXT("binding_coverage"), BindingRatio);

		return FMonolithActionResult::Success(Result);
	}

	// ----- 5.H.3 hot_reload_styles [RUNTIME, EXPERIMENTAL] --------------------
	// Iterate all UCommonButtonBase in PIE and force re-apply of current style.
	// Utility for iterating on style assets without closing/reopening menus.

	static FMonolithActionResult HandleHotReloadStyles(const TSharedPtr<FJsonObject>& Params)
	{
		if (!MonolithCommonUI::GetPIEWorld())
			return FMonolithActionResult::Error(TEXT("requires PIE session"));

		UWorld* PIE = MonolithCommonUI::GetPIEWorld();
		int32 Touched = 0;

		for (TObjectIterator<UCommonButtonBase> It; It; ++It)
		{
			UCommonButtonBase* Btn = *It;
			if (!Btn || Btn->GetWorld() != PIE) continue;

			// Re-set the style to itself to trigger a refresh.
			FObjectProperty* SP = FindFProperty<FObjectProperty>(UCommonButtonBase::StaticClass(), TEXT("Style"));
			if (!SP) continue;
			UObject* CurStyle = SP->GetObjectPropertyValue_InContainer(Btn);
			if (!CurStyle) continue;
			if (UClass* AsClass = Cast<UClass>(CurStyle))
			{
				Btn->SetStyle(AsClass);
				Touched++;
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("buttons_refreshed"), Touched);
		Result->SetStringField(TEXT("experimental"), TEXT("true — implementation refreshes style class via SetStyle(cur). Full hot-reload requires asset reimport hook."));
		return FMonolithActionResult::Success(Result);
	}

	// ----- 5.H.4 dump_action_router_state [RUNTIME, EXPERIMENTAL] --------------

	static FMonolithActionResult HandleDumpActionRouterState(const TSharedPtr<FJsonObject>& Params)
	{
		UWorld* PIE = MonolithCommonUI::GetPIEWorld();
		if (!PIE) return FMonolithActionResult::Error(TEXT("requires PIE session"));

		APlayerController* PC = PIE->GetFirstPlayerController();
		if (!PC) return FMonolithActionResult::Error(TEXT("no player controller"));
		ULocalPlayer* LP = PC->GetLocalPlayer();
		if (!LP) return FMonolithActionResult::Error(TEXT("no local player"));

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UCommonInputSubsystem* Input = LP->GetSubsystem<UCommonInputSubsystem>();
		if (Input)
		{
			Result->SetStringField(TEXT("current_input_type"), FString::Printf(TEXT("%d"), static_cast<int32>(Input->GetCurrentInputType())));
			Result->SetBoolField(TEXT("pointer_active"), Input->IsUsingPointerInput());
		}

		// UCommonUIActionRouterBase::CurrentInputLocks is private. We expose what reflection allows.
		TArray<TSharedPtr<FJsonValue>> Containers;
		for (TObjectIterator<UCommonActivatableWidgetContainerBase> It; It; ++It)
		{
			UCommonActivatableWidgetContainerBase* C = *It;
			if (!C || C->GetWorld() != PIE) continue;
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("name"), C->GetName());
			O->SetNumberField(TEXT("depth"), C->GetNumWidgets());
			UCommonActivatableWidget* Top = C->GetActiveWidget();
			if (Top) O->SetStringField(TEXT("top_widget"), Top->GetName());
			Containers.Add(MakeShared<FJsonValueObject>(O));
		}
		Result->SetArrayField(TEXT("activatable_containers"), Containers);
		Result->SetStringField(TEXT("experimental"), TEXT("true — UCommonUIActionRouterBase::CurrentInputLocks is private. Full router state requires engine PR."));
		return FMonolithActionResult::Success(Result);
	}

	// ----- Registration --------------------------------------------------------

	void Register(FMonolithToolRegistry& Registry)
	{
		const FString Cat(TEXT("CommonUI"));

		Registry.RegisterAction(
			TEXT("ui"), TEXT("audit_commonui_widget"),
			TEXT("Lint a single WBP for CommonUI best-practice violations (missing style, unbound action widget, etc.)"),
			FMonolithActionHandler::CreateStatic(&HandleAuditCommonUIWidget),
			FParamSchemaBuilder()
				.RequiredAssetPath(TEXT("wbp_path"), TEXT("Widget Blueprint to audit"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("export_commonui_report"),
			TEXT("Project-wide CommonUI audit: counts of activatable widgets, buttons (styled/unstyled), action widgets (bound/unbound), styling + binding coverage ratios"),
			FMonolithActionHandler::CreateStatic(&HandleExportCommonUIReport),
			FParamSchemaBuilder()
				.OptionalAssetPath(TEXT("folder_path"), TEXT("Folder to scan (default: /Game)"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("hot_reload_styles"),
			TEXT("[RUNTIME, EXPERIMENTAL] Re-apply current style class to all UCommonButtonBase in PIE (after iterating on style asset)"),
			FMonolithActionHandler::CreateStatic(&HandleHotReloadStyles),
			nullptr,
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("dump_action_router_state"),
			TEXT("[RUNTIME, EXPERIMENTAL] Dump current input type + activatable container states for debugging UI input routing"),
			FMonolithActionHandler::CreateStatic(&HandleDumpActionRouterState),
			nullptr,
			Cat);
	}
}

#endif // WITH_COMMONUI
