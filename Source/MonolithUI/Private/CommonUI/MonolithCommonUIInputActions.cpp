// Category C: Input, Actions, Glyphs — 7 actions
// 2.C.1 create_input_action_data_table
// 2.C.2 add_input_action_row
// 2.C.3 bind_common_action_widget
// 2.C.4 create_bound_action_bar
// 2.C.5 get_active_input_type [RUNTIME]
// 2.C.6 set_input_type_override [RUNTIME]
// 2.C.7 list_platform_input_tables
#include "MonolithCommonUIHelpers.h"

#if WITH_COMMONUI

#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "CommonActionWidget.h"
#include "CommonUITypes.h"
#include "CommonInputSubsystem.h"
#include "CommonInputSettings.h"
#include "CommonInputBaseTypes.h"
#include "Input/CommonBoundActionBar.h"

#include "Engine/DataTable.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"

namespace MonolithCommonUIInput
{
	// ----- 2.C.1 create_input_action_data_table --------------------------------

	static FMonolithActionResult HandleCreateInputActionDataTable(const TSharedPtr<FJsonObject>& Params)
	{
		FString PackagePath, AssetName;
		if (!Params.IsValid() ||
			!Params->TryGetStringField(TEXT("package_path"), PackagePath) ||
			!Params->TryGetStringField(TEXT("asset_name"), AssetName))
			return FMonolithActionResult::Error(TEXT("package_path and asset_name required"));

		UObject* Created = nullptr;
		FMonolithActionResult R = MonolithCommonUI::CreateAsset(
			UDataTable::StaticClass(), PackagePath, AssetName, /*bSkipSave*/ true, Created);
		if (!R.bSuccess) return R;

		UDataTable* DT = Cast<UDataTable>(Created);
		if (!DT)
			return FMonolithActionResult::Error(TEXT("CreateAsset returned non-DataTable"));

		// RowStruct must be FCommonInputActionDataBase
		DT->RowStruct = FCommonInputActionDataBase::StaticStruct();
		DT->MarkPackageDirty();

		// Save now that RowStruct is set.
		UPackage* Pkg = DT->GetOutermost();
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		const FString PackageName = Pkg->GetName();
		UPackage::SavePackage(
			Pkg, DT,
			*FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()),
			SaveArgs);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), PackageName);
		Result->SetStringField(TEXT("row_struct"), TEXT("FCommonInputActionDataBase"));
		return FMonolithActionResult::Success(Result);
	}

	// ----- 2.C.2 add_input_action_row ------------------------------------------

	static FMonolithActionResult HandleAddInputActionRow(const TSharedPtr<FJsonObject>& Params)
	{
		FString TablePath, RowName, DisplayName;
		if (!Params.IsValid() ||
			!Params->TryGetStringField(TEXT("table_path"), TablePath) ||
			!Params->TryGetStringField(TEXT("row_name"), RowName) ||
			!Params->TryGetStringField(TEXT("display_name"), DisplayName))
			return FMonolithActionResult::Error(TEXT("table_path, row_name, display_name required"));

		UDataTable* DT = LoadObject<UDataTable>(nullptr, *TablePath);
		if (!DT) return FMonolithActionResult::Error(FString::Printf(TEXT("DataTable '%s' not found"), *TablePath));
		if (DT->RowStruct != FCommonInputActionDataBase::StaticStruct())
			return FMonolithActionResult::Error(TEXT("DataTable RowStruct is not FCommonInputActionDataBase"));

		FCommonInputActionDataBase NewRow;
		NewRow.DisplayName = FText::FromString(DisplayName);

		FString HoldName;
		if (Params->TryGetStringField(TEXT("hold_display_name"), HoldName))
			NewRow.HoldDisplayName = FText::FromString(HoldName);

		int32 NavPriority = 0;
		if (Params->TryGetNumberField(TEXT("nav_bar_priority"), NavPriority))
			NewRow.NavBarPriority = NavPriority;

		// Keyboard/gamepad/touch key fields are protected; set via reflection using ImportText.
		UScriptStruct* RowStruct = FCommonInputActionDataBase::StaticStruct();

		auto TrySetStructField = [&](const TCHAR* FieldName, const TCHAR* JsonField)
		{
			FString TextVal;
			if (!Params->TryGetStringField(JsonField, TextVal)) return;
			FProperty* P = RowStruct->FindPropertyByName(FieldName);
			if (!P) return;
			P->ImportText_Direct(*TextVal, P->ContainerPtrToValuePtr<void>(&NewRow), nullptr, PPF_None);
		};

		// Consumers pass pre-serialized FCommonInputTypeInfo struct text — e.g.
		//   "(Key=(KeyName=Gamepad_FaceButton_Bottom))"
		TrySetStructField(TEXT("KeyboardInputTypeInfo"), TEXT("keyboard_key"));
		TrySetStructField(TEXT("DefaultGamepadInputTypeInfo"), TEXT("gamepad_key"));
		TrySetStructField(TEXT("TouchInputTypeInfo"), TEXT("touch_key"));

		DT->AddRow(FName(*RowName), NewRow);
		DT->MarkPackageDirty();

		// Save
		UPackage* Pkg = DT->GetOutermost();
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::SavePackage(
			Pkg, DT,
			*FPackageName::LongPackageNameToFilename(Pkg->GetName(), FPackageName::GetAssetPackageExtension()),
			SaveArgs);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("table_path"), TablePath);
		Result->SetStringField(TEXT("row_name"), RowName);
		Result->SetStringField(TEXT("display_name"), DisplayName);
		return FMonolithActionResult::Success(Result);
	}

	// ----- 2.C.3 bind_common_action_widget -------------------------------------

	static FMonolithActionResult HandleBindCommonActionWidget(const TSharedPtr<FJsonObject>& Params)
	{
		FString WidgetName, TablePath, RowName;
		if (!Params.IsValid() ||
			!Params->TryGetStringField(TEXT("widget_name"), WidgetName) ||
			!Params->TryGetStringField(TEXT("table_path"), TablePath) ||
			!Params->TryGetStringField(TEXT("row_name"), RowName))
			return FMonolithActionResult::Error(TEXT("wbp_path, widget_name, table_path, row_name required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		UDataTable* DT = LoadObject<UDataTable>(nullptr, *TablePath);
		if (!DT) return FMonolithActionResult::Error(FString::Printf(TEXT("DataTable '%s' not found"), *TablePath));

		if (!DT->FindRowUnchecked(FName(*RowName)))
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Row '%s' not found in DataTable '%s'"), *RowName, *TablePath));

		UWidgetBlueprint* Wbp = nullptr;
		UWidget* Target = nullptr;
		FMonolithActionResult Loaded = MonolithCommonUI::LoadWidgetForMutation(WbpPath, FName(*WidgetName), Wbp, Target);
		if (!Loaded.bSuccess) return Loaded;

		UCommonActionWidget* ActionWidget = Cast<UCommonActionWidget>(Target);
		if (!ActionWidget)
			return FMonolithActionResult::Error(TEXT("target is not a UCommonActionWidget"));

		FDataTableRowHandle Handle;
		Handle.DataTable = DT;
		Handle.RowName = FName(*RowName);

		ActionWidget->SetInputAction(Handle);

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		Result->SetStringField(TEXT("table_path"), TablePath);
		Result->SetStringField(TEXT("row_name"), RowName);
		return FMonolithActionResult::Success(Result);
	}

	// ----- 2.C.4 create_bound_action_bar ---------------------------------------

	// Bug #4 fix (2026-05-16 UI gap audit): WBPs containing a freshly-created
	// UCommonBoundActionBar failed to compile (BS_Error: "ActionBar has no
	// ActionButtonClass specified") because nothing wired the REQUIRED
	// ActionButtonClass property. The validator is at
	// CommonBoundActionBar.cpp:91 (ValidateCompiledDefaults). We now accept an
	// optional action_button_class param and default to the Monolith stock
	// button class when omitted. UE 5.7 API verified: TSubclassOf<UCommonButtonBase>
	// ActionButtonClass at CommonBoundActionBar.h:68 (orchestrator-cleared).
	static const FString DefaultActionButtonClassPath = TEXT("/Game/Monolith/CommonUI/MonolithDefaultCommonButton.MonolithDefaultCommonButton_C");

	static FMonolithActionResult HandleCreateBoundActionBar(const TSharedPtr<FJsonObject>& Params)
	{
		FString WidgetName, ParentWidgetName;
		if (!Params.IsValid() ||
			!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMonolithActionResult::Error(TEXT("wbp_path and widget_name required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));
		Params->TryGetStringField(TEXT("parent_widget"), ParentWidgetName);

		// Resolve the button class up front so we can surface a clean error
		// before mutating the widget tree. The caller's explicit path wins;
		// otherwise we fall back to the Monolith default. LoadClass with the
		// `_C` suffix is the standard UE 5.7 idiom for grabbing a Blueprint's
		// generated class.
		FString ButtonClassPath;
		const bool bExplicitClass = Params->TryGetStringField(TEXT("action_button_class"), ButtonClassPath);
		if (!bExplicitClass || ButtonClassPath.IsEmpty())
		{
			ButtonClassPath = DefaultActionButtonClassPath;
		}

		UClass* ResolvedButtonClass = LoadClass<UCommonButtonBase>(nullptr, *ButtonClassPath);
		if (!ResolvedButtonClass)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to resolve action_button_class '%s' (must be a UCommonButtonBase subclass path with _C suffix). Default Monolith button is at '%s' — ensure it exists or pass an explicit action_button_class."),
				*ButtonClassPath, *DefaultActionButtonClassPath));
		}
		if (!ResolvedButtonClass->IsChildOf(UCommonButtonBase::StaticClass()))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("action_button_class '%s' does not derive from UCommonButtonBase"),
				*ButtonClassPath));
		}

		UWidgetBlueprint* Wbp = LoadObject<UWidgetBlueprint>(nullptr, *WbpPath);
		if (!Wbp || !Wbp->WidgetTree)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load WBP '%s'"), *WbpPath));

		UPanelWidget* Parent = nullptr;
		if (ParentWidgetName.IsEmpty())
			Parent = Cast<UPanelWidget>(Wbp->WidgetTree->RootWidget);
		else
		{
			Wbp->WidgetTree->ForEachWidget([&Parent, &ParentWidgetName](UWidget* W)
			{
				if (!Parent && W && W->GetFName() == FName(*ParentWidgetName))
					Parent = Cast<UPanelWidget>(W);
			});
		}
		if (!Parent)
			return FMonolithActionResult::Error(TEXT("Parent panel not found"));

		UCommonBoundActionBar* Bar = Wbp->WidgetTree->ConstructWidget<UCommonBoundActionBar>(
			UCommonBoundActionBar::StaticClass(), FName(*WidgetName));
		if (!Bar) return FMonolithActionResult::Error(TEXT("ConstructWidget returned null"));

		// CRITICAL: write the ActionButtonClass BEFORE AddChild + compile so
		// the validator (CommonBoundActionBar::ValidateCompiledDefaults at
		// CommonBoundActionBar.cpp:91) sees the assigned class and accepts the
		// blueprint. Without this, every WBP shipped with a fresh action bar
		// previously compiled BS_Error.
		//
		// UCommonBoundActionBar::ActionButtonClass is a private UPROPERTY
		// (CommonBoundActionBar.h:67-68, EditAnywhere) -- direct assignment
		// fails C2248. Use the canonical FClassProperty reflection-write
		// idiom (mirrors MonolithCommonUIListActions.cpp:67-70 setting
		// UListView::EntryWidgetClass).
		if (FClassProperty* ActionButtonClassProp = FindFProperty<FClassProperty>(
			UCommonBoundActionBar::StaticClass(), TEXT("ActionButtonClass")))
		{
			ActionButtonClassProp->SetObjectPropertyValue(
				ActionButtonClassProp->ContainerPtrToValuePtr<void>(Bar),
				ResolvedButtonClass);
		}
		else
		{
			return FMonolithActionResult::Error(TEXT(
				"Failed to resolve UCommonBoundActionBar::ActionButtonClass FClassProperty via reflection"));
		}

		Parent->AddChild(Bar);

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		Result->SetStringField(TEXT("widget_class"), TEXT("CommonBoundActionBar"));
		Result->SetStringField(TEXT("action_button_class"), ResolvedButtonClass->GetPathName());
		Result->SetBoolField(TEXT("action_button_class_was_default"), !bExplicitClass);
		return FMonolithActionResult::Success(Result);
	}

	// ----- Runtime helper: resolve UCommonInputSubsystem in PIE ----------------

	static UCommonInputSubsystem* GetPIEInputSubsystem()
	{
		UWorld* PIE = MonolithCommonUI::GetPIEWorld();
		if (!PIE) return nullptr;

		APlayerController* PC = PIE->GetFirstPlayerController();
		if (!PC) return nullptr;

		ULocalPlayer* LP = PC->GetLocalPlayer();
		if (!LP) return nullptr;

		return LP->GetSubsystem<UCommonInputSubsystem>();
	}

	static FString InputTypeToString(ECommonInputType T)
	{
		switch (T)
		{
			case ECommonInputType::MouseAndKeyboard: return TEXT("MouseAndKeyboard");
			case ECommonInputType::Gamepad: return TEXT("Gamepad");
			case ECommonInputType::Touch: return TEXT("Touch");
			default: return TEXT("Unknown");
		}
	}

	static bool ParseInputTypeString(const FString& S, ECommonInputType& Out)
	{
		if (S.Equals(TEXT("MouseAndKeyboard"), ESearchCase::IgnoreCase)) { Out = ECommonInputType::MouseAndKeyboard; return true; }
		if (S.Equals(TEXT("Gamepad"), ESearchCase::IgnoreCase)) { Out = ECommonInputType::Gamepad; return true; }
		if (S.Equals(TEXT("Touch"), ESearchCase::IgnoreCase)) { Out = ECommonInputType::Touch; return true; }
		return false;
	}

	// ----- 2.C.5 get_active_input_type [RUNTIME] -------------------------------

	static FMonolithActionResult HandleGetActiveInputType(const TSharedPtr<FJsonObject>& Params)
	{
		UCommonInputSubsystem* Sub = GetPIEInputSubsystem();
		if (!Sub) return FMonolithActionResult::Error(TEXT("requires PIE with a local player"));

		const ECommonInputType Current = Sub->GetCurrentInputType();
		const ECommonInputType Default = Sub->GetDefaultInputType();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("current"), InputTypeToString(Current));
		Result->SetStringField(TEXT("default"), InputTypeToString(Default));
		Result->SetBoolField(TEXT("mouse_and_keyboard_active"), Sub->IsInputMethodActive(ECommonInputType::MouseAndKeyboard));
		Result->SetBoolField(TEXT("gamepad_active"), Sub->IsInputMethodActive(ECommonInputType::Gamepad));
		Result->SetBoolField(TEXT("touch_active"), Sub->IsInputMethodActive(ECommonInputType::Touch));
		return FMonolithActionResult::Success(Result);
	}

	// ----- 2.C.6 set_input_type_override [RUNTIME] -----------------------------

	static FMonolithActionResult HandleSetInputTypeOverride(const TSharedPtr<FJsonObject>& Params)
	{
		FString InputTypeStr;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("input_type"), InputTypeStr))
			return FMonolithActionResult::Error(TEXT("input_type required (MouseAndKeyboard|Gamepad|Touch)"));

		ECommonInputType Type;
		if (!ParseInputTypeString(InputTypeStr, Type))
			return FMonolithActionResult::Error(TEXT("input_type must be one of: MouseAndKeyboard, Gamepad, Touch"));

		UCommonInputSubsystem* Sub = GetPIEInputSubsystem();
		if (!Sub) return FMonolithActionResult::Error(TEXT("requires PIE with a local player"));

		Sub->SetCurrentInputType(Type);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("set_input_type"), InputTypeStr);
		Result->SetStringField(TEXT("now_current"), InputTypeToString(Sub->GetCurrentInputType()));
		return FMonolithActionResult::Success(Result);
	}

	// ----- 2.C.7 list_platform_input_tables ------------------------------------

	static FMonolithActionResult HandleListPlatformInputTables(const TSharedPtr<FJsonObject>& Params)
	{
		const UCommonInputSettings* Settings = GetDefault<UCommonInputSettings>();
		if (!Settings)
			return FMonolithActionResult::Error(TEXT("UCommonInputSettings CDO unavailable"));

		// ControllerData is protected TArray<TSoftClassPtr<UCommonInputBaseControllerData>> — access via reflection.
		TArray<TSharedPtr<FJsonValue>> Entries;
		if (FArrayProperty* P = FindFProperty<FArrayProperty>(UCommonInputSettings::StaticClass(), TEXT("ControllerData")))
		{
			FScriptArrayHelper Helper(P, P->ContainerPtrToValuePtr<void>(Settings));
			for (int32 i = 0; i < Helper.Num(); ++i)
			{
				if (FSoftClassProperty* InnerProp = CastField<FSoftClassProperty>(P->Inner))
				{
					FSoftObjectPtr* SoftPtr = reinterpret_cast<FSoftObjectPtr*>(Helper.GetRawPtr(i));
					if (SoftPtr)
					{
						TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("path"), SoftPtr->ToString());
						Entries.Add(MakeShared<FJsonValueObject>(Entry));
					}
				}
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("controller_data"), Entries);
		Result->SetNumberField(TEXT("count"), Entries.Num());
		return FMonolithActionResult::Success(Result);
	}

	// ----- Registration --------------------------------------------------------

	void Register(FMonolithToolRegistry& Registry)
	{
		const FString Cat(TEXT("CommonUI"));

		Registry.RegisterAction(
			TEXT("ui"), TEXT("create_input_action_data_table"),
			TEXT("Create a UDataTable of FCommonInputActionDataBase rows (Accept/Back/Navigate etc.)"),
			FMonolithActionHandler::CreateStatic(&HandleCreateInputActionDataTable),
			FParamSchemaBuilder()
				.Required(TEXT("package_path"), TEXT("string"), TEXT("Folder path"))
				.Required(TEXT("asset_name"), TEXT("string"), TEXT("Asset name"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("add_input_action_row"),
			TEXT("Add a FCommonInputActionDataBase row to an input-action DataTable. Struct fields use UE text format."),
			FMonolithActionHandler::CreateStatic(&HandleAddInputActionRow),
			FParamSchemaBuilder()
				.Required(TEXT("table_path"), TEXT("string"), TEXT("Input-action DataTable path"))
				.Required(TEXT("row_name"), TEXT("string"), TEXT("Row name (FName)"))
				.Required(TEXT("display_name"), TEXT("string"), TEXT("User-facing display text"))
				.Optional(TEXT("hold_display_name"), TEXT("string"), TEXT("Display when hold-variant used"))
				.Optional(TEXT("nav_bar_priority"), TEXT("integer"), TEXT("Priority in nav bar"))
				.Optional(TEXT("keyboard_key"), TEXT("string"), TEXT("FCommonInputTypeInfo text, e.g. '(Key=(KeyName=E))'"))
				.Optional(TEXT("gamepad_key"), TEXT("string"), TEXT("FCommonInputTypeInfo text, e.g. '(Key=(KeyName=Gamepad_FaceButton_Bottom))'"))
				.Optional(TEXT("touch_key"), TEXT("string"), TEXT("FCommonInputTypeInfo text"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("bind_common_action_widget"),
			TEXT("Configure a UCommonActionWidget in a WBP to point at a DataTable row (glyph + hold behavior)"),
			FMonolithActionHandler::CreateStatic(&HandleBindCommonActionWidget),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of UCommonActionWidget"))
				.Required(TEXT("table_path"), TEXT("string"), TEXT("Input-action DataTable path"))
				.Required(TEXT("row_name"), TEXT("string"), TEXT("Row name (FName)"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("create_bound_action_bar"),
			TEXT("Add a UCommonBoundActionBar to an existing WBP's tree (auto-populated action glyph bar). Writes ActionButtonClass = action_button_class (default: MonolithDefaultCommonButton_C) so the blueprint compiles cleanly — bare bars fail validation in UCommonBoundActionBar::ValidateCompiledDefaults."),
			FMonolithActionHandler::CreateStatic(&HandleCreateBoundActionBar),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("Target Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name to assign the bar"))
				.Optional(TEXT("parent_widget"), TEXT("string"), TEXT("Parent panel (default: root)"))
				.Optional(TEXT("action_button_class"), TEXT("string"), TEXT("Path to a UCommonButtonBase subclass with _C suffix (default: /Game/Monolith/CommonUI/MonolithDefaultCommonButton.MonolithDefaultCommonButton_C). Required to be a UCommonButtonBase subclass."))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("get_active_input_type"),
			TEXT("[RUNTIME] Get current ECommonInputType + per-method active flags from PIE LocalPlayer's CommonInputSubsystem"),
			FMonolithActionHandler::CreateStatic(&HandleGetActiveInputType),
			nullptr,
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("set_input_type_override"),
			TEXT("[RUNTIME] Force the UCommonInputSubsystem's current input type (useful for testing platform-specific UI paths)"),
			FMonolithActionHandler::CreateStatic(&HandleSetInputTypeOverride),
			FParamSchemaBuilder()
				.Required(TEXT("input_type"), TEXT("string"), TEXT("MouseAndKeyboard | Gamepad | Touch"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("list_platform_input_tables"),
			TEXT("List registered UCommonInputBaseControllerData entries from CommonInputSettings.ControllerData"),
			FMonolithActionHandler::CreateStatic(&HandleListPlatformInputTables),
			nullptr,
			Cat);
	}
}

#endif // WITH_COMMONUI
