// Category A: Activatable Lifecycle — 8 actions
// 2.A.1 create_activatable_widget
// 2.A.2 create_activatable_stack
// 2.A.3 create_activatable_switcher
// 2.A.4 configure_activatable
// 2.A.5 push_to_activatable_stack [RUNTIME]
// 2.A.6 pop_activatable_stack [RUNTIME]
// 2.A.7 get_activatable_stack_state [RUNTIME]
// 2.A.8 set_activatable_transition
#include "MonolithCommonUIHelpers.h"

#if WITH_COMMONUI

#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "CommonActivatableWidget.h"
#include "Widgets/CommonActivatableWidgetContainer.h"
#include "CommonInputBaseTypes.h"

#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/CanvasPanel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "Engine/World.h"
#include "MonolithAssetUtils.h"

namespace MonolithCommonUIActivatable
{
	// ----- 2.A.1 create_activatable_widget -------------------------------------

	static FMonolithActionResult HandleCreateActivatableWidget(const TSharedPtr<FJsonObject>& Params)
	{
		FString SavePath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("save_path"), SavePath))
			return FMonolithActionResult::Error(TEXT("save_path required"));

		FString RootWidgetType = TEXT("CanvasPanel");
		Params->TryGetStringField(TEXT("root_widget"), RootWidgetType);

		// Split save_path into folder + asset name
		FString PackagePath, AssetName;
		if (!SavePath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
			return FMonolithActionResult::Error(TEXT("save_path must contain at least one / separator"));

		// Create package + factory-instantiated WBP with parent class UCommonActivatableWidget
		UPackage* Package = CreatePackage(*SavePath);
		if (!Package)
			return FMonolithActionResult::Error(FString::Printf(TEXT("CreatePackage failed for '%s'"), *SavePath));

		if (FindObject<UObject>(Package, *AssetName))
			return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *SavePath));

		UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
		Factory->BlueprintType = BPTYPE_Normal;
		Factory->ParentClass = UCommonActivatableWidget::StaticClass();

		UObject* Created = Factory->FactoryCreateNew(
			UWidgetBlueprint::StaticClass(), Package,
			FName(*AssetName), RF_Public | RF_Standalone,
			nullptr, GWarn);

		UWidgetBlueprint* Wbp = Cast<UWidgetBlueprint>(Created);
		if (!Wbp)
			return FMonolithActionResult::Error(TEXT("WidgetBlueprintFactory returned null"));

		// Install a root panel so the tree isn't empty
		if (Wbp->WidgetTree && !Wbp->WidgetTree->RootWidget)
		{
			UClass* RootClass = FindFirstObject<UClass>(*RootWidgetType, EFindFirstObjectOptions::NativeFirst);
			if (!RootClass)
				RootClass = FindFirstObject<UClass>(*(TEXT("U") + RootWidgetType), EFindFirstObjectOptions::NativeFirst);
			if (!RootClass || !RootClass->IsChildOf(UPanelWidget::StaticClass()))
				RootClass = UCanvasPanel::StaticClass();

			UWidget* Root = Wbp->WidgetTree->ConstructWidget<UWidget>(RootClass, FName(*RootWidgetType));
			Wbp->WidgetTree->RootWidget = Root;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);

		FAssetRegistryModule::AssetCreated(Wbp);
		Package->MarkPackageDirty();
		FMonolithAssetUtils::SaveAssetPackage(Package);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), SavePath);
		Result->SetStringField(TEXT("asset_name"), AssetName);
		Result->SetStringField(TEXT("parent_class"), TEXT("CommonActivatableWidget"));
		Result->SetStringField(TEXT("root_widget"), RootWidgetType);
		return FMonolithActionResult::Success(Result);
	}

	// ----- Shared: add a container widget into an existing WBP ------------------

	static FMonolithActionResult AddContainerToWbp(
		UClass* ContainerClass,
		const TSharedPtr<FJsonObject>& Params)
	{
		FString ParentWidgetName, WidgetName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMonolithActionResult::Error(TEXT("wbp_path and widget_name required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));
		Params->TryGetStringField(TEXT("parent_widget"), ParentWidgetName);

		UWidgetBlueprint* Wbp = LoadObject<UWidgetBlueprint>(nullptr, *WbpPath);
		if (!Wbp || !Wbp->WidgetTree)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load WBP '%s'"), *WbpPath));

		// Resolve parent panel (defaults to root)
		UPanelWidget* Parent = nullptr;
		if (ParentWidgetName.IsEmpty())
		{
			Parent = Cast<UPanelWidget>(Wbp->WidgetTree->RootWidget);
		}
		else
		{
			Wbp->WidgetTree->ForEachWidget([&Parent, &ParentWidgetName](UWidget* W)
			{
				if (!Parent && W && W->GetFName() == FName(*ParentWidgetName))
					Parent = Cast<UPanelWidget>(W);
			});
		}

		if (!Parent)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Parent panel '%s' not found or not a UPanelWidget"), *ParentWidgetName));

		UWidget* New = Wbp->WidgetTree->ConstructWidget<UWidget>(ContainerClass, FName(*WidgetName));
		if (!New)
			return FMonolithActionResult::Error(FString::Printf(TEXT("ConstructWidget failed for class '%s'"), *ContainerClass->GetName()));

		Parent->AddChild(New);

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		Result->SetStringField(TEXT("widget_class"), ContainerClass->GetName());
		Result->SetStringField(TEXT("parent_widget"), Parent->GetName());
		return FMonolithActionResult::Success(Result);
	}

	// ----- 2.A.2 create_activatable_stack ---------------------------------------

	static FMonolithActionResult HandleCreateActivatableStack(const TSharedPtr<FJsonObject>& Params)
	{
		return AddContainerToWbp(UCommonActivatableWidgetStack::StaticClass(), Params);
	}

	// ----- 2.A.3 create_activatable_switcher ------------------------------------

	static FMonolithActionResult HandleCreateActivatableSwitcher(const TSharedPtr<FJsonObject>& Params)
	{
		// UCommonActivatableWidgetSwitcher exists in CommonUI — same pattern as stack
		UClass* SwitcherClass = FindFirstObject<UClass>(TEXT("CommonActivatableWidgetSwitcher"), EFindFirstObjectOptions::NativeFirst);
		if (!SwitcherClass)
			return FMonolithActionResult::Error(TEXT("UCommonActivatableWidgetSwitcher class not found"));
		return AddContainerToWbp(SwitcherClass, Params);
	}

	// ----- 2.A.4 configure_activatable ------------------------------------------

	static FMonolithActionResult HandleConfigureActivatable(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
			return FMonolithActionResult::Error(TEXT("wbp_path required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		UWidgetBlueprint* Wbp = LoadObject<UWidgetBlueprint>(nullptr, *WbpPath);
		if (!Wbp)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load WBP '%s'"), *WbpPath));

		UClass* GenClass = Wbp->GeneratedClass;
		if (!GenClass || !GenClass->IsChildOf(UCommonActivatableWidget::StaticClass()))
			return FMonolithActionResult::Error(TEXT("WBP generated class is not a UCommonActivatableWidget"));

		UCommonActivatableWidget* CDO = Cast<UCommonActivatableWidget>(GenClass->GetDefaultObject());
		if (!CDO)
			return FMonolithActionResult::Error(TEXT("CDO null"));

		TArray<FString> Applied;

		// Mutate known UPROPERTYs via reflection so we don't require friend access.
		auto TrySetBool = [&](const TCHAR* PropName, const TCHAR* JsonField)
		{
			bool Val;
			if (Params->TryGetBoolField(JsonField, Val))
			{
				if (FBoolProperty* P = FindFProperty<FBoolProperty>(GenClass, PropName))
				{
					P->SetPropertyValue_InContainer(CDO, Val);
					Applied.Add(FString(JsonField));
				}
			}
		};
		auto TrySetString = [&](const TCHAR* PropName, const TCHAR* JsonField)
		{
			FString Val;
			if (Params->TryGetStringField(JsonField, Val))
			{
				if (FProperty* P = FindFProperty<FProperty>(GenClass, PropName))
				{
					P->ImportText_Direct(*Val, P->ContainerPtrToValuePtr<void>(CDO), nullptr, PPF_None);
					Applied.Add(FString(JsonField));
				}
			}
		};
		auto TrySetObject = [&](const TCHAR* PropName, const TCHAR* JsonField)
		{
			FString Val;
			if (Params->TryGetStringField(JsonField, Val))
			{
				if (FObjectProperty* P = FindFProperty<FObjectProperty>(GenClass, PropName))
				{
					UObject* Resolved = LoadObject<UObject>(nullptr, *Val);
					if (Resolved && Resolved->IsA(P->PropertyClass))
					{
						P->SetObjectPropertyValue(P->ContainerPtrToValuePtr<void>(CDO), Resolved);
						Applied.Add(FString(JsonField));
					}
				}
			}
		};
		auto TrySetInt = [&](const TCHAR* PropName, const TCHAR* JsonField)
		{
			int32 Val;
			if (Params->TryGetNumberField(JsonField, Val))
			{
				if (FIntProperty* P = FindFProperty<FIntProperty>(GenClass, PropName))
				{
					P->SetPropertyValue_InContainer(CDO, Val);
					Applied.Add(FString(JsonField));
				}
			}
		};

		TrySetBool(TEXT("bAutoActivate"), TEXT("bAutoActivate"));
		TrySetBool(TEXT("bIsModal"), TEXT("bIsModal"));
		TrySetBool(TEXT("bIsBackHandler"), TEXT("bIsBackHandler"));
		TrySetString(TEXT("ActivatedVisibility"), TEXT("activated_visibility"));
		TrySetString(TEXT("DeactivatedVisibility"), TEXT("deactivated_visibility"));
		TrySetObject(TEXT("InputMapping"), TEXT("input_mapping"));
		TrySetInt(TEXT("InputMappingPriority"), TEXT("input_mapping_priority"));

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		TArray<TSharedPtr<FJsonValue>> AppliedArr;
		for (const FString& A : Applied) AppliedArr.Add(MakeShared<FJsonValueString>(A));
		Result->SetArrayField(TEXT("applied"), AppliedArr);
		return FMonolithActionResult::Success(Result);
	}

	// ----- Runtime helper: find a UCommonActivatableWidgetContainerBase in PIE by name

	static UCommonActivatableWidgetContainerBase* FindContainerInPIE(const FName& ContainerName)
	{
		UWorld* PIE = MonolithCommonUI::GetPIEWorld();
		if (!PIE) return nullptr;

		for (TObjectIterator<UCommonActivatableWidgetContainerBase> It; It; ++It)
		{
			UCommonActivatableWidgetContainerBase* C = *It;
			if (!C || C->GetWorld() != PIE) continue;
			if (C->GetFName() == ContainerName) return C;
		}
		return nullptr;
	}

	// ----- 2.A.5 push_to_activatable_stack [RUNTIME] ----------------------------

	static FMonolithActionResult HandlePushToActivatableStack(const TSharedPtr<FJsonObject>& Params)
	{
		if (!MonolithCommonUI::GetPIEWorld())
			return FMonolithActionResult::Error(TEXT("requires PIE session"), FMonolithJsonUtils::ErrInvalidParams);

		FString ContainerName, WidgetClassPath;
		if (!Params.IsValid() ||
			!Params->TryGetStringField(TEXT("container_name"), ContainerName) ||
			!Params->TryGetStringField(TEXT("widget_class"), WidgetClassPath))
			return FMonolithActionResult::Error(TEXT("container_name and widget_class required"));

		UCommonActivatableWidgetContainerBase* Container = FindContainerInPIE(FName(*ContainerName));
		if (!Container)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Container '%s' not found in PIE"), *ContainerName));

		UClass* ResolvedClass = LoadClass<UCommonActivatableWidget>(nullptr, *WidgetClassPath);
		if (!ResolvedClass)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to resolve widget class '%s'"), *WidgetClassPath));

		// Public template AddWidget<>() — default template param is UCommonActivatableWidget.
		UCommonActivatableWidget* Pushed = Container->AddWidget(ResolvedClass);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("container_name"), ContainerName);
		Result->SetStringField(TEXT("pushed_class"), ResolvedClass->GetName());
		Result->SetNumberField(TEXT("stack_depth"), Container->GetNumWidgets());
		Result->SetBoolField(TEXT("success"), Pushed != nullptr);
		return FMonolithActionResult::Success(Result);
	}

	// ----- 2.A.6 pop_activatable_stack [RUNTIME] --------------------------------

	static FMonolithActionResult HandlePopActivatableStack(const TSharedPtr<FJsonObject>& Params)
	{
		if (!MonolithCommonUI::GetPIEWorld())
			return FMonolithActionResult::Error(TEXT("requires PIE session"), FMonolithJsonUtils::ErrInvalidParams);

		FString ContainerName, Mode = TEXT("top");
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("container_name"), ContainerName))
			return FMonolithActionResult::Error(TEXT("container_name required"));
		Params->TryGetStringField(TEXT("mode"), Mode);

		UCommonActivatableWidgetContainerBase* Container = FindContainerInPIE(FName(*ContainerName));
		if (!Container)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Container '%s' not found in PIE"), *ContainerName));

		FString Action;
		if (Mode.Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			Container->ClearWidgets();
			Action = TEXT("cleared");
		}
		else
		{
			// Default: pop top
			UCommonActivatableWidget* Top = Container->GetActiveWidget();
			if (Top)
			{
				Container->RemoveWidget(*Top);
				Action = TEXT("popped_top");
			}
			else
			{
				Action = TEXT("stack_empty");
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("container_name"), ContainerName);
		Result->SetStringField(TEXT("action"), Action);
		Result->SetNumberField(TEXT("stack_depth"), Container->GetNumWidgets());
		return FMonolithActionResult::Success(Result);
	}

	// ----- 2.A.7 get_activatable_stack_state [RUNTIME] --------------------------

	static FMonolithActionResult HandleGetActivatableStackState(const TSharedPtr<FJsonObject>& Params)
	{
		if (!MonolithCommonUI::GetPIEWorld())
			return FMonolithActionResult::Error(TEXT("requires PIE session"), FMonolithJsonUtils::ErrInvalidParams);

		FString ContainerName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("container_name"), ContainerName))
			return FMonolithActionResult::Error(TEXT("container_name required"));

		UCommonActivatableWidgetContainerBase* Container = FindContainerInPIE(FName(*ContainerName));
		if (!Container)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Container '%s' not found in PIE"), *ContainerName));

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("container_name"), ContainerName);
		Result->SetStringField(TEXT("container_class"), Container->GetClass()->GetName());
		Result->SetNumberField(TEXT("stack_depth"), Container->GetNumWidgets());

		UCommonActivatableWidget* Top = Container->GetActiveWidget();
		if (Top)
		{
			Result->SetStringField(TEXT("active_widget_class"), Top->GetClass()->GetName());
			Result->SetStringField(TEXT("active_widget_name"), Top->GetName());
		}
		else
		{
			Result->SetBoolField(TEXT("empty"), true);
		}

		return FMonolithActionResult::Success(Result);
	}

	// ----- 2.A.8 set_activatable_transition -------------------------------------

	static FMonolithActionResult HandleSetActivatableTransition(const TSharedPtr<FJsonObject>& Params)
	{
		FString WidgetName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMonolithActionResult::Error(TEXT("wbp_path and widget_name required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		UWidgetBlueprint* Wbp = nullptr;
		UWidget* Target = nullptr;
		FMonolithActionResult Loaded = MonolithCommonUI::LoadWidgetForMutation(WbpPath, FName(*WidgetName), Wbp, Target);
		if (!Loaded.bSuccess) return Loaded;

		UCommonActivatableWidgetContainerBase* Container = Cast<UCommonActivatableWidgetContainerBase>(Target);
		if (!Container)
			return FMonolithActionResult::Error(TEXT("target widget is not a UCommonActivatableWidgetContainerBase"));

		TArray<FString> Applied;

		FString TransitionTypeStr;
		if (Params->TryGetStringField(TEXT("transition_type"), TransitionTypeStr))
		{
			if (FEnumProperty* P = FindFProperty<FEnumProperty>(Container->GetClass(), TEXT("TransitionType")))
			{
				P->ImportText_Direct(*TransitionTypeStr, P->ContainerPtrToValuePtr<void>(Container), nullptr, PPF_None);
				Applied.Add(TEXT("transition_type"));
			}
		}

		double Duration;
		if (Params->TryGetNumberField(TEXT("transition_duration"), Duration))
		{
			Container->SetTransitionDuration(static_cast<float>(Duration));
			Applied.Add(TEXT("transition_duration"));
		}

		FString CurveTypeStr;
		if (Params->TryGetStringField(TEXT("transition_curve_type"), CurveTypeStr))
		{
			if (FEnumProperty* P = FindFProperty<FEnumProperty>(Container->GetClass(), TEXT("TransitionCurveType")))
			{
				P->ImportText_Direct(*CurveTypeStr, P->ContainerPtrToValuePtr<void>(Container), nullptr, PPF_None);
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

	// ----- Registration ---------------------------------------------------------

	void Register(FMonolithToolRegistry& Registry)
	{
		const FString Cat(TEXT("CommonUI"));

		Registry.RegisterAction(
			TEXT("ui"), TEXT("create_activatable_widget"),
			TEXT("Create a UCommonActivatableWidget blueprint (modal/screen/pause widget base)"),
			FMonolithActionHandler::CreateStatic(&HandleCreateActivatableWidget),
			FParamSchemaBuilder()
				.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path, e.g. /Game/UI/WBP_PauseMenu"))
				.Optional(TEXT("root_widget"), TEXT("string"), TEXT("Root panel widget type"), TEXT("CanvasPanel"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("create_activatable_stack"),
			TEXT("Add a UCommonActivatableWidgetStack to an existing WBP's tree (modal layer container)"),
			FMonolithActionHandler::CreateStatic(&HandleCreateActivatableStack),
			FParamSchemaBuilder()
				.RequiredAssetPath(TEXT("wbp_path"), TEXT("Target Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name to assign the stack"))
				.Optional(TEXT("parent_widget"), TEXT("string"), TEXT("Name of parent panel (default: root)"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("create_activatable_switcher"),
			TEXT("Add a UCommonActivatableWidgetSwitcher to an existing WBP's tree (tab/screen switch container)"),
			FMonolithActionHandler::CreateStatic(&HandleCreateActivatableSwitcher),
			FParamSchemaBuilder()
				.RequiredAssetPath(TEXT("wbp_path"), TEXT("Target Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name to assign the switcher"))
				.Optional(TEXT("parent_widget"), TEXT("string"), TEXT("Name of parent panel (default: root)"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("configure_activatable"),
			TEXT("Set UCommonActivatableWidget CDO flags (bAutoActivate, bIsModal, bIsBackHandler, visibility, InputMapping, priority)"),
			FMonolithActionHandler::CreateStatic(&HandleConfigureActivatable),
			FParamSchemaBuilder()
				.RequiredAssetPath(TEXT("wbp_path"), TEXT("Target UCommonActivatableWidget blueprint"))
				.Optional(TEXT("bAutoActivate"), TEXT("boolean"), TEXT("Auto-activate when added to layout"))
				.Optional(TEXT("bIsModal"), TEXT("boolean"), TEXT("Modal — obscures widgets below in stack"))
				.Optional(TEXT("bIsBackHandler"), TEXT("boolean"), TEXT("Handles back action input"))
				.Optional(TEXT("activated_visibility"), TEXT("string"), TEXT("ESlateVisibility enum name when activated"))
				.Optional(TEXT("deactivated_visibility"), TEXT("string"), TEXT("ESlateVisibility enum name when deactivated"))
				.Optional(TEXT("input_mapping"), TEXT("string"), TEXT("UInputMappingContext asset path"))
				.Optional(TEXT("input_mapping_priority"), TEXT("integer"), TEXT("Priority for InputMappingContext"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("push_to_activatable_stack"),
			TEXT("[RUNTIME] Push a widget class onto a named activatable widget container in PIE"),
			FMonolithActionHandler::CreateStatic(&HandlePushToActivatableStack),
			FParamSchemaBuilder()
				.Required(TEXT("container_name"), TEXT("string"), TEXT("FName of the container widget in the live UMG tree"))
				.Required(TEXT("widget_class"), TEXT("string"), TEXT("TSubclassOf<UCommonActivatableWidget> class path"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("pop_activatable_stack"),
			TEXT("[RUNTIME] Pop widget(s) from a named activatable container. mode=top|all"),
			FMonolithActionHandler::CreateStatic(&HandlePopActivatableStack),
			FParamSchemaBuilder()
				.Required(TEXT("container_name"), TEXT("string"), TEXT("FName of the container widget in the live UMG tree"))
				.Optional(TEXT("mode"), TEXT("string"), TEXT("'top' (default) or 'all'"), TEXT("top"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("get_activatable_stack_state"),
			TEXT("[RUNTIME] Inspect activatable container state: depth, active widget class/name"),
			FMonolithActionHandler::CreateStatic(&HandleGetActivatableStackState),
			FParamSchemaBuilder()
				.Required(TEXT("container_name"), TEXT("string"), TEXT("FName of the container widget in the live UMG tree"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("set_activatable_transition"),
			TEXT("Set TransitionType / TransitionDuration / TransitionCurveType on an activatable container widget (stack/container only, not switcher — use configure_animated_switcher for switchers)"),
			FMonolithActionHandler::CreateStatic(&HandleSetActivatableTransition),
			FParamSchemaBuilder()
				.RequiredAssetPath(TEXT("wbp_path"), TEXT("Target Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("FName of container widget in tree"))
				.Optional(TEXT("transition_type"), TEXT("string"), TEXT("ECommonSwitcherTransition enum name"))
				.Optional(TEXT("transition_duration"), TEXT("number"), TEXT("Duration in seconds"))
				.Optional(TEXT("transition_curve_type"), TEXT("string"), TEXT("ETransitionCurve enum name"))
				.Build(),
			Cat);
	}
}

#endif // WITH_COMMONUI
