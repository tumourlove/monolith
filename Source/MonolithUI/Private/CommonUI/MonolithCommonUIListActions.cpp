// Category E: Lists, Tabs, Button Groups, Switchers, Carousel, HardwareVisibility — 7 actions
// 3.E.1 setup_common_list_view
// 3.E.2 create_tab_list_widget
// 3.E.3 register_tab
// 3.E.4 create_button_group
// 3.E.5 configure_animated_switcher
// 3.E.6 create_widget_carousel
// 3.E.7 create_hardware_visibility_border
#include "MonolithCommonUIHelpers.h"

#if WITH_COMMONUI

#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "CommonListView.h"
#include "CommonTileView.h"
#include "CommonTabListWidgetBase.h"
#include "Groups/CommonButtonGroupBase.h"
#include "CommonAnimatedSwitcher.h"
#include "CommonWidgetCarousel.h"
#include "CommonHardwareVisibilityBorder.h"
#include "CommonButtonBase.h"

#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/CanvasPanel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "MonolithAssetUtils.h"

namespace MonolithCommonUIList
{
	// ----- 3.E.1 setup_common_list_view ----------------------------------------

	static FMonolithActionResult HandleSetupCommonListView(const TSharedPtr<FJsonObject>& Params)
	{
		FString WidgetName, EntryClassPath;
		if (!Params.IsValid() ||
			!Params->TryGetStringField(TEXT("widget_name"), WidgetName) ||
			!Params->TryGetStringField(TEXT("entry_class"), EntryClassPath))
			return FMonolithActionResult::Error(TEXT("wbp_path, widget_name, entry_class required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		UWidgetBlueprint* Wbp = nullptr;
		UWidget* Target = nullptr;
		FMonolithActionResult Loaded = MonolithCommonUI::LoadWidgetForMutation(WbpPath, FName(*WidgetName), Wbp, Target);
		if (!Loaded.bSuccess) return Loaded;

		UListView* ListView = Cast<UListView>(Target);
		if (!ListView)
			return FMonolithActionResult::Error(TEXT("target is not a UListView (UCommonListView / UCommonTileView both inherit UListView)"));

		UClass* EntryClass = LoadClass<UUserWidget>(nullptr, *EntryClassPath);
		if (!EntryClass)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to resolve entry class '%s'"), *EntryClassPath));

		// EntryWidgetClass is a UPROPERTY on UListView
		if (FClassProperty* P = FindFProperty<FClassProperty>(UListView::StaticClass(), TEXT("EntryWidgetClass")))
		{
			P->SetObjectPropertyValue(P->ContainerPtrToValuePtr<void>(ListView), EntryClass);
		}

		double Spacing;
		if (Params->TryGetNumberField(TEXT("entry_spacing"), Spacing))
		{
			// UCommonListView has SetEntrySpacing
			if (UCommonListView* CLV = Cast<UCommonListView>(ListView))
			{
				CLV->SetEntrySpacing(static_cast<float>(Spacing));
			}
		}

		int32 PoolSize;
		if (Params->TryGetNumberField(TEXT("pool_size"), PoolSize))
		{
			if (FIntProperty* P = FindFProperty<FIntProperty>(UListView::StaticClass(), TEXT("NumDesignerPreviewEntries")))
			{
				P->SetPropertyValue_InContainer(ListView, PoolSize);
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		Result->SetStringField(TEXT("entry_class"), EntryClass->GetName());
		return FMonolithActionResult::Success(Result);
	}

	// ----- 3.E.2 create_tab_list_widget ----------------------------------------

	static FMonolithActionResult HandleCreateTabListWidget(const TSharedPtr<FJsonObject>& Params)
	{
		FString SavePath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("save_path"), SavePath))
			return FMonolithActionResult::Error(TEXT("save_path required"));

		FString PackagePath, AssetName;
		if (!SavePath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
			return FMonolithActionResult::Error(TEXT("save_path must contain /"));

		UPackage* Package = CreatePackage(*SavePath);
		if (!Package) return FMonolithActionResult::Error(TEXT("CreatePackage failed"));
		if (FindObject<UObject>(Package, *AssetName))
			return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *SavePath));

		UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
		Factory->BlueprintType = BPTYPE_Normal;
		Factory->ParentClass = UCommonTabListWidgetBase::StaticClass();

		UObject* Created = Factory->FactoryCreateNew(
			UWidgetBlueprint::StaticClass(), Package,
			FName(*AssetName), RF_Public | RF_Standalone,
			nullptr, GWarn);

		UWidgetBlueprint* Wbp = Cast<UWidgetBlueprint>(Created);
		if (!Wbp) return FMonolithActionResult::Error(TEXT("Factory returned null"));

		if (Wbp->WidgetTree && !Wbp->WidgetTree->RootWidget)
		{
			UWidget* Root = Wbp->WidgetTree->ConstructWidget<UWidget>(UCanvasPanel::StaticClass(), TEXT("CanvasPanel"));
			Wbp->WidgetTree->RootWidget = Root;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		FAssetRegistryModule::AssetCreated(Wbp);
		Package->MarkPackageDirty();
		FMonolithAssetUtils::SaveAssetPackage(Package);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), SavePath);
		Result->SetStringField(TEXT("parent_class"), TEXT("CommonTabListWidgetBase"));
		return FMonolithActionResult::Success(Result);
	}

	// ----- 3.E.3 register_tab [RUNTIME] ----------------------------------------

	static FMonolithActionResult HandleRegisterTab(const TSharedPtr<FJsonObject>& Params)
	{
		if (!MonolithCommonUI::GetPIEWorld())
			return FMonolithActionResult::Error(TEXT("requires PIE session (RegisterTab mutates a live instance)"));

		FString TabListName, TabId, ButtonClassPath;
		if (!Params.IsValid() ||
			!Params->TryGetStringField(TEXT("tab_list_name"), TabListName) ||
			!Params->TryGetStringField(TEXT("tab_id"), TabId) ||
			!Params->TryGetStringField(TEXT("button_class"), ButtonClassPath))
			return FMonolithActionResult::Error(TEXT("tab_list_name, tab_id, button_class required"));

		UWorld* PIE = MonolithCommonUI::GetPIEWorld();
		UCommonTabListWidgetBase* TabList = nullptr;
		for (TObjectIterator<UCommonTabListWidgetBase> It; It; ++It)
		{
			UCommonTabListWidgetBase* W = *It;
			if (!W || W->GetWorld() != PIE) continue;
			if (W->GetFName() == FName(*TabListName)) { TabList = W; break; }
		}
		if (!TabList)
			return FMonolithActionResult::Error(FString::Printf(TEXT("TabList '%s' not found in PIE"), *TabListName));

		UClass* BtnClass = LoadClass<UCommonButtonBase>(nullptr, *ButtonClassPath);
		if (!BtnClass)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to resolve button class '%s'"), *ButtonClassPath));

		int32 TabIndex = -1;
		Params->TryGetNumberField(TEXT("tab_index"), TabIndex);

		const bool Registered = TabList->RegisterTab(FName(*TabId), BtnClass, nullptr, TabIndex);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("tab_list_name"), TabListName);
		Result->SetStringField(TEXT("tab_id"), TabId);
		Result->SetBoolField(TEXT("registered"), Registered);
		Result->SetNumberField(TEXT("tab_count"), TabList->GetTabCount());
		return FMonolithActionResult::Success(Result);
	}

	// ----- 3.E.4 create_button_group [RUNTIME] ---------------------------------

	static FMonolithActionResult HandleCreateButtonGroup(const TSharedPtr<FJsonObject>& Params)
	{
		if (!MonolithCommonUI::GetPIEWorld())
			return FMonolithActionResult::Error(TEXT("requires PIE session (button group wraps live widget instances)"));

		const TArray<TSharedPtr<FJsonValue>>* ButtonNames = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("button_names"), ButtonNames))
			return FMonolithActionResult::Error(TEXT("button_names (array of FNames) required"));

		bool bSelectionRequired = false;
		Params->TryGetBoolField(TEXT("selection_required"), bSelectionRequired);

		UWorld* PIE = MonolithCommonUI::GetPIEWorld();

		// Create the group object
		UCommonButtonGroupBase* Group = NewObject<UCommonButtonGroupBase>(GetTransientPackage());
		Group->SetSelectionRequired(bSelectionRequired);

		int32 Added = 0;
		for (const TSharedPtr<FJsonValue>& V : *ButtonNames)
		{
			FString ButtonName;
			if (!V->TryGetString(ButtonName)) continue;
			UCommonButtonBase* Btn = nullptr;
			for (TObjectIterator<UCommonButtonBase> It; It; ++It)
			{
				UCommonButtonBase* B = *It;
				if (!B || B->GetWorld() != PIE) continue;
				if (B->GetFName() == FName(*ButtonName)) { Btn = B; break; }
			}
			if (Btn)
			{
				Group->AddWidget(Btn);
				Added++;
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("group_object"), Group->GetPathName());
		Result->SetNumberField(TEXT("buttons_added"), Added);
		Result->SetBoolField(TEXT("selection_required"), bSelectionRequired);
		return FMonolithActionResult::Success(Result);
	}

	// ----- 3.E.5 configure_animated_switcher -----------------------------------

	static FMonolithActionResult HandleConfigureAnimatedSwitcher(const TSharedPtr<FJsonObject>& Params)
	{
		FString WidgetName;
		if (!Params.IsValid() ||
			!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMonolithActionResult::Error(TEXT("wbp_path and widget_name required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		UWidgetBlueprint* Wbp = nullptr;
		UWidget* Target = nullptr;
		FMonolithActionResult Loaded = MonolithCommonUI::LoadWidgetForMutation(WbpPath, FName(*WidgetName), Wbp, Target);
		if (!Loaded.bSuccess) return Loaded;

		UCommonAnimatedSwitcher* Sw = Cast<UCommonAnimatedSwitcher>(Target);
		if (!Sw)
			return FMonolithActionResult::Error(TEXT("target is not a UCommonAnimatedSwitcher"));

		TArray<FString> Applied;

		FString TransStr;
		if (Params->TryGetStringField(TEXT("transition_type"), TransStr))
		{
			if (FEnumProperty* P = FindFProperty<FEnumProperty>(UCommonAnimatedSwitcher::StaticClass(), TEXT("TransitionType")))
			{
				P->ImportText_Direct(*TransStr, P->ContainerPtrToValuePtr<void>(Sw), nullptr, PPF_None);
				Applied.Add(TEXT("transition_type"));
			}
		}

		double Duration;
		if (Params->TryGetNumberField(TEXT("transition_duration"), Duration))
		{
			if (FFloatProperty* P = FindFProperty<FFloatProperty>(UCommonAnimatedSwitcher::StaticClass(), TEXT("TransitionDuration")))
			{
				P->SetPropertyValue_InContainer(Sw, static_cast<float>(Duration));
				Applied.Add(TEXT("transition_duration"));
			}
		}

		FString CurveStr;
		if (Params->TryGetStringField(TEXT("transition_curve_type"), CurveStr))
		{
			if (FEnumProperty* P = FindFProperty<FEnumProperty>(UCommonAnimatedSwitcher::StaticClass(), TEXT("TransitionCurveType")))
			{
				P->ImportText_Direct(*CurveStr, P->ContainerPtrToValuePtr<void>(Sw), nullptr, PPF_None);
				Applied.Add(TEXT("transition_curve_type"));
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		TArray<TSharedPtr<FJsonValue>> AppliedArr;
		for (const FString& A : Applied) AppliedArr.Add(MakeShared<FJsonValueString>(A));
		Result->SetArrayField(TEXT("applied"), AppliedArr);
		return FMonolithActionResult::Success(Result);
	}

	// ----- Shared: add a container widget to a WBP tree ------------------------

	static FMonolithActionResult AddPanelToWbp(UClass* Cls, const TSharedPtr<FJsonObject>& Params, const FString& ReportClassName)
	{
		FString WidgetName, ParentName;
		if (!Params.IsValid() ||
			!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMonolithActionResult::Error(TEXT("wbp_path and widget_name required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));
		Params->TryGetStringField(TEXT("parent_widget"), ParentName);

		UWidgetBlueprint* Wbp = LoadObject<UWidgetBlueprint>(nullptr, *WbpPath);
		if (!Wbp || !Wbp->WidgetTree)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load WBP '%s'"), *WbpPath));

		UPanelWidget* Parent = nullptr;
		if (ParentName.IsEmpty())
			Parent = Cast<UPanelWidget>(Wbp->WidgetTree->RootWidget);
		else
		{
			Wbp->WidgetTree->ForEachWidget([&](UWidget* W)
			{
				if (!Parent && W && W->GetFName() == FName(*ParentName))
					Parent = Cast<UPanelWidget>(W);
			});
		}
		if (!Parent)
			return FMonolithActionResult::Error(TEXT("Parent panel not found"));

		UWidget* New = Wbp->WidgetTree->ConstructWidget<UWidget>(Cls, FName(*WidgetName));
		if (!New) return FMonolithActionResult::Error(TEXT("ConstructWidget returned null"));

		Parent->AddChild(New);

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		Result->SetStringField(TEXT("widget_class"), ReportClassName);
		return FMonolithActionResult::Success(Result);
	}

	// ----- 3.E.6 create_widget_carousel ----------------------------------------

	static FMonolithActionResult HandleCreateWidgetCarousel(const TSharedPtr<FJsonObject>& Params)
	{
		return AddPanelToWbp(UCommonWidgetCarousel::StaticClass(), Params, TEXT("CommonWidgetCarousel"));
	}

	// ----- 3.E.7 create_hardware_visibility_border -----------------------------

	static FMonolithActionResult HandleCreateHardwareVisibilityBorder(const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithActionResult R = AddPanelToWbp(UCommonHardwareVisibilityBorder::StaticClass(), Params, TEXT("CommonHardwareVisibilityBorder"));
		if (!R.bSuccess) return R;

		// Optionally set VisibilityQuery tag text.
		FString QueryText;
		if (Params->TryGetStringField(TEXT("visibility_query"), QueryText))
		{
			FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
			FString WidgetName;
			Params->TryGetStringField(TEXT("widget_name"), WidgetName);

			UWidgetBlueprint* Wbp = nullptr;
			UWidget* Target = nullptr;
			FMonolithActionResult L = MonolithCommonUI::LoadWidgetForMutation(WbpPath, FName(*WidgetName), Wbp, Target);
			if (L.bSuccess && Target)
			{
				if (FStructProperty* P = FindFProperty<FStructProperty>(UCommonHardwareVisibilityBorder::StaticClass(), TEXT("VisibilityQuery")))
				{
					P->ImportText_Direct(*QueryText, P->ContainerPtrToValuePtr<void>(Target), nullptr, PPF_None);
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
					FKismetEditorUtilities::CompileBlueprint(Wbp);
				}
			}
		}
		return R;
	}

	// ----- Registration --------------------------------------------------------

	void Register(FMonolithToolRegistry& Registry)
	{
		const FString Cat(TEXT("CommonUI"));

		Registry.RegisterAction(
			TEXT("ui"), TEXT("setup_common_list_view"),
			TEXT("Configure a UCommonListView / UCommonTileView in a WBP: entry widget class, spacing, pool size"),
			FMonolithActionHandler::CreateStatic(&HandleSetupCommonListView),
			FParamSchemaBuilder()
				.RequiredAssetPath(TEXT("wbp_path"), TEXT("Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of the list view"))
				.Required(TEXT("entry_class"), TEXT("string"), TEXT("UUserWidget class path for entries"))
				.Optional(TEXT("entry_spacing"), TEXT("number"), TEXT("Spacing (px, CommonListView only)"))
				.Optional(TEXT("pool_size"), TEXT("integer"), TEXT("NumDesignerPreviewEntries"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("create_tab_list_widget"),
			TEXT("Create a WBP subclass of UCommonTabListWidgetBase"),
			FMonolithActionHandler::CreateStatic(&HandleCreateTabListWidget),
			FParamSchemaBuilder()
				.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path, e.g. /Game/UI/WBP_TabList"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("register_tab"),
			TEXT("[RUNTIME] Register a tab on a live UCommonTabListWidgetBase instance"),
			FMonolithActionHandler::CreateStatic(&HandleRegisterTab),
			FParamSchemaBuilder()
				.Required(TEXT("tab_list_name"), TEXT("string"), TEXT("FName of tab list widget in PIE"))
				.Required(TEXT("tab_id"), TEXT("string"), TEXT("FName for the new tab"))
				.Required(TEXT("button_class"), TEXT("string"), TEXT("UCommonButtonBase class path"))
				.Optional(TEXT("tab_index"), TEXT("integer"), TEXT("Insert index (-1 = append)"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("create_button_group"),
			TEXT("[RUNTIME] Create a UCommonButtonGroupBase wrapping named buttons in PIE"),
			FMonolithActionHandler::CreateStatic(&HandleCreateButtonGroup),
			FParamSchemaBuilder()
				.Required(TEXT("button_names"), TEXT("array"), TEXT("Array of FNames of UCommonButtonBase widgets"))
				.Optional(TEXT("selection_required"), TEXT("boolean"), TEXT("Require at least one selected"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("configure_animated_switcher"),
			TEXT("Configure UCommonAnimatedSwitcher: transition type, duration, curve"),
			FMonolithActionHandler::CreateStatic(&HandleConfigureAnimatedSwitcher),
			FParamSchemaBuilder()
				.RequiredAssetPath(TEXT("wbp_path"), TEXT("Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of UCommonAnimatedSwitcher"))
				.Optional(TEXT("transition_type"), TEXT("string"), TEXT("ECommonSwitcherTransition enum name"))
				.Optional(TEXT("transition_duration"), TEXT("number"), TEXT("Seconds"))
				.Optional(TEXT("transition_curve_type"), TEXT("string"), TEXT("ETransitionCurve enum name"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("create_widget_carousel"),
			TEXT("Add a UCommonWidgetCarousel to an existing WBP tree"),
			FMonolithActionHandler::CreateStatic(&HandleCreateWidgetCarousel),
			FParamSchemaBuilder()
				.RequiredAssetPath(TEXT("wbp_path"), TEXT("Target Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name for the carousel"))
				.Optional(TEXT("parent_widget"), TEXT("string"), TEXT("Parent panel (default: root)"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("create_hardware_visibility_border"),
			TEXT("Add a UCommonHardwareVisibilityBorder with an optional FGameplayTagQuery (platform/input gating)"),
			FMonolithActionHandler::CreateStatic(&HandleCreateHardwareVisibilityBorder),
			FParamSchemaBuilder()
				.RequiredAssetPath(TEXT("wbp_path"), TEXT("Target Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name for the border"))
				.Optional(TEXT("parent_widget"), TEXT("string"), TEXT("Parent panel (default: root)"))
				.Optional(TEXT("visibility_query"), TEXT("string"), TEXT("FGameplayTagQuery text, e.g. 'AnyTags=(Input.Gamepad)'"))
				.Build(),
			Cat);
	}
}

#endif // WITH_COMMONUI
