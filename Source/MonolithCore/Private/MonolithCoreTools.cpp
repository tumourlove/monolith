#include "MonolithCoreTools.h"
#include "MonolithCoreModule.h"
#include "MonolithJsonUtils.h"
#include "MonolithHttpServer.h"
#include "MonolithSettings.h"
#include "MonolithUpdateSubsystem.h"
#include "EditorSubsystem.h"
#include "Misc/App.h"
#include "Editor.h"

// Known optional modules — namespaces that may not have registered actions
// depending on settings or missing plugin dependencies.
struct FKnownOptionalModule
{
	FString Namespace;
	FString SettingsField;   // bool property name on UMonolithSettings
	FString ToolName;        // MCP tool name (namespace_query)
	FString InstallHint;
};

static const TArray<FKnownOptionalModule>& GetKnownOptionalModules()
{
	static const TArray<FKnownOptionalModule> Modules = {
		{
			TEXT("gas"),
			TEXT("bEnableGAS"),
			TEXT("gas_query"),
			TEXT("MonolithGAS module provides Gameplay Ability System tooling (attributes, abilities, effects, cues). Requires GameplayAbilities plugin (engine-bundled).")
		},
		{
			TEXT("combograph"),
			TEXT("bEnableComboGraph"),
			TEXT("combograph_query"),
			TEXT("MonolithComboGraph module provides combo graph tooling (nodes, edges, transitions, effects). Requires ComboGraph plugin (Fab marketplace).")
		}
	};
	return Modules;
}

void FMonolithCoreTools::RegisterAll()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// monolith_discover
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> NsProp = MakeShared<FJsonObject>();
		NsProp->SetStringField(TEXT("type"), TEXT("string"));
		NsProp->SetStringField(TEXT("description"), TEXT("Optional: filter to a specific namespace"));
		Schema->SetObjectField(TEXT("namespace"), NsProp);

		TSharedPtr<FJsonObject> CatProp = MakeShared<FJsonObject>();
		CatProp->SetStringField(TEXT("type"), TEXT("string"));
		CatProp->SetStringField(TEXT("description"), TEXT("Optional: filter actions within the namespace by category (e.g. 'CommonUI' inside 'ui')"));
		Schema->SetObjectField(TEXT("category"), CatProp);

		Registry.RegisterAction(
			TEXT("monolith"), TEXT("discover"),
			TEXT("List available tool namespaces and their actions. Pass namespace (and optional category) to filter."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleDiscover),
			Schema
		);
	}

	// monolith_status
	{
		Registry.RegisterAction(
			TEXT("monolith"), TEXT("status"),
			TEXT("Get Monolith server health: version, uptime, port, registered action count, module status."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleStatus)
		);
	}

	// monolith_update
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> ActionProp = MakeShared<FJsonObject>();
		ActionProp->SetStringField(TEXT("type"), TEXT("string"));
		ActionProp->SetStringField(TEXT("description"), TEXT("'check' to compare versions, 'install' to download and stage update"));
		ActionProp->SetStringField(TEXT("default"), TEXT("check"));
		Schema->SetObjectField(TEXT("action"), ActionProp);

		Registry.RegisterAction(
			TEXT("monolith"), TEXT("update"),
			TEXT("Check for or install Monolith updates from GitHub Releases."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleUpdate),
			Schema
		);
	}

	// monolith_reindex
	{
		Registry.RegisterAction(
			TEXT("monolith"), TEXT("reindex"),
			TEXT("Re-index the Monolith project database. Incremental by default (delta only). Pass force=true for full wipe+rebuild."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleReindex)
		);
	}
}

FMonolithActionResult FMonolithCoreTools::HandleDiscover(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	FString FilterNamespace;
	FString FilterCategory;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("namespace"), FilterNamespace);
		Params->TryGetStringField(TEXT("category"), FilterCategory);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	TArray<FString> Namespaces = Registry.GetNamespaces();

	if (!FilterNamespace.IsEmpty())
	{
		// Filter to specific namespace — return detailed action list
		TArray<FMonolithActionInfo> Actions = Registry.GetActions(FilterNamespace);
		if (Actions.Num() == 0)
		{
			// Check if this is a known optional module
			const TArray<FKnownOptionalModule>& OptionalModules = GetKnownOptionalModules();
			const FKnownOptionalModule* Found = nullptr;
			for (const FKnownOptionalModule& Mod : OptionalModules)
			{
				if (Mod.Namespace.Equals(FilterNamespace, ESearchCase::IgnoreCase))
				{
					Found = &Mod;
					break;
				}
			}

			if (Found)
			{
				// Determine disabled vs not_installed by checking the settings toggle
				const UMonolithSettings* Settings = UMonolithSettings::Get();
				bool bSettingEnabled = false;
				if (Settings)
				{
					const FBoolProperty* Prop = CastField<FBoolProperty>(
						UMonolithSettings::StaticClass()->FindPropertyByName(*Found->SettingsField));
					if (Prop)
					{
						bSettingEnabled = Prop->GetPropertyValue_InContainer(Settings);
					}
				}

				Result->SetStringField(TEXT("namespace"), Found->Namespace);
				Result->SetNumberField(TEXT("actions"), 0);

				if (!bSettingEnabled)
				{
					Result->SetStringField(TEXT("status"), TEXT("disabled"));
					Result->SetStringField(TEXT("hint"),
						FString::Printf(TEXT("Enable in Project Settings > Plugins > Monolith > Modules > Optional (%s), then restart the editor."),
							*Found->SettingsField));
				}
				else
				{
					Result->SetStringField(TEXT("status"), TEXT("not_installed"));
					Result->SetStringField(TEXT("hint"), Found->InstallHint);
				}

				return FMonolithActionResult::Success(Result);
			}

			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Unknown namespace: %s"), *FilterNamespace),
				FMonolithJsonUtils::ErrInvalidParams
			);
		}

		// Apply optional category filter (only meaningful when namespace is specified).
		if (!FilterCategory.IsEmpty())
		{
			Actions = Actions.FilterByPredicate([&FilterCategory](const FMonolithActionInfo& Info)
			{
				return Info.Category.Equals(FilterCategory, ESearchCase::IgnoreCase);
			});
		}

		Result->SetStringField(TEXT("namespace"), FilterNamespace);
		if (!FilterCategory.IsEmpty())
		{
			Result->SetStringField(TEXT("category"), FilterCategory);
		}
		TArray<TSharedPtr<FJsonValue>> ActionArray;
		for (const FMonolithActionInfo& ActionInfo : Actions)
		{
			TSharedPtr<FJsonObject> ActionObj = MakeShared<FJsonObject>();
			ActionObj->SetStringField(TEXT("action"), ActionInfo.Action);
			ActionObj->SetStringField(TEXT("description"), ActionInfo.Description);
			if (!ActionInfo.Category.IsEmpty())
			{
				ActionObj->SetStringField(TEXT("category"), ActionInfo.Category);
			}
			if (ActionInfo.ParamSchema.IsValid())
			{
				ActionObj->SetObjectField(TEXT("params"), ActionInfo.ParamSchema);
			}
			ActionArray.Add(MakeShared<FJsonValueObject>(ActionObj));
		}
		Result->SetArrayField(TEXT("actions"), ActionArray);
	}
	else
	{
		// Return all namespaces with action counts
		TArray<TSharedPtr<FJsonValue>> NsArray;
		for (const FString& Ns : Namespaces)
		{
			TArray<FMonolithActionInfo> Actions = Registry.GetActions(Ns);
			TSharedPtr<FJsonObject> NsObj = MakeShared<FJsonObject>();
			NsObj->SetStringField(TEXT("namespace"), Ns);
			NsObj->SetNumberField(TEXT("action_count"), Actions.Num());

			TArray<TSharedPtr<FJsonValue>> ActionNames;
			for (const FMonolithActionInfo& ActionInfo : Actions)
			{
				ActionNames.Add(MakeShared<FJsonValueString>(ActionInfo.Action));
			}
			NsObj->SetArrayField(TEXT("actions"), ActionNames);
			NsArray.Add(MakeShared<FJsonValueObject>(NsObj));
		}
		// Append known optional modules that aren't already registered
		const TArray<FKnownOptionalModule>& OptionalModules = GetKnownOptionalModules();
		const UMonolithSettings* Settings = UMonolithSettings::Get();

		TArray<TSharedPtr<FJsonValue>> OptionalArray;
		for (const FKnownOptionalModule& Mod : OptionalModules)
		{
			// Skip if this namespace already has registered actions (it's active)
			if (Namespaces.Contains(Mod.Namespace))
			{
				continue;
			}

			TSharedPtr<FJsonObject> OptObj = MakeShared<FJsonObject>();
			OptObj->SetStringField(TEXT("namespace"), Mod.Namespace);
			OptObj->SetStringField(TEXT("tool"), Mod.ToolName);
			OptObj->SetNumberField(TEXT("action_count"), 0);

			bool bSettingEnabled = false;
			if (Settings)
			{
				const FBoolProperty* Prop = CastField<FBoolProperty>(
					UMonolithSettings::StaticClass()->FindPropertyByName(*Mod.SettingsField));
				if (Prop)
				{
					bSettingEnabled = Prop->GetPropertyValue_InContainer(Settings);
				}
			}

			OptObj->SetStringField(TEXT("status"), bSettingEnabled ? TEXT("not_installed") : TEXT("disabled"));
			OptObj->SetStringField(TEXT("hint"), bSettingEnabled ? Mod.InstallHint
				: FString::Printf(TEXT("Enable in Project Settings > Plugins > Monolith > Modules > Optional (%s), then restart the editor."), *Mod.SettingsField));

			OptionalArray.Add(MakeShared<FJsonValueObject>(OptObj));
		}

		if (OptionalArray.Num() > 0)
		{
			Result->SetArrayField(TEXT("optional_modules"), OptionalArray);
		}

		Result->SetArrayField(TEXT("namespaces"), NsArray);
		Result->SetNumberField(TEXT("total_actions"), Registry.GetActionCount());
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Version
	Result->SetStringField(TEXT("version"), MONOLITH_VERSION);

	// Server status
	FMonolithHttpServer* Server = FMonolithCoreModule::Get().GetHttpServer();
	Result->SetBoolField(TEXT("server_running"), Server != nullptr && Server->IsRunning());
	Result->SetNumberField(TEXT("server_port"), Server ? Server->GetPort() : 0);

	// Registry stats
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Result->SetNumberField(TEXT("total_actions"), Registry.GetActionCount());
	Result->SetNumberField(TEXT("namespaces"), Registry.GetNamespaces().Num());

	// Engine info
	Result->SetStringField(TEXT("engine_version"), FApp::GetBuildVersion());

	// Project info
	Result->SetStringField(TEXT("project_name"), FApp::GetProjectName());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleUpdate(const TSharedPtr<FJsonObject>& Params)
{
	FString Action = TEXT("check");
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("action"), Action);
	}

	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}

	UMonolithUpdateSubsystem* UpdateSubsystem = GEditor->GetEditorSubsystem<UMonolithUpdateSubsystem>();
	if (!UpdateSubsystem)
	{
		return FMonolithActionResult::Error(TEXT("MonolithUpdateSubsystem not available"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (Action == TEXT("check"))
	{
		const FMonolithVersionInfo& Info = UpdateSubsystem->GetVersionInfo();
		Result->SetStringField(TEXT("current_version"), Info.Current);
		Result->SetStringField(TEXT("pending_version"), Info.Pending.IsEmpty() ? TEXT("none") : Info.Pending);
		Result->SetBoolField(TEXT("staging"), Info.bStaging);
		Result->SetStringField(TEXT("status"), TEXT("check_initiated"));

		// Trigger async check — result will come via notification
		UpdateSubsystem->CheckForUpdate();

		return FMonolithActionResult::Success(Result);
	}
	else if (Action == TEXT("install"))
	{
		// Install requires a previous check to have found a version
		const FMonolithVersionInfo& Info = UpdateSubsystem->GetVersionInfo();
		if (Info.bStaging)
		{
			Result->SetStringField(TEXT("status"), TEXT("already_staged"));
			Result->SetStringField(TEXT("pending_version"), Info.Pending);
			Result->SetStringField(TEXT("message"), TEXT("Update already staged. Restart the editor to apply."));
			return FMonolithActionResult::Success(Result);
		}

		// Trigger a check that will show the notification with install button
		UpdateSubsystem->CheckForUpdate();
		Result->SetStringField(TEXT("status"), TEXT("checking_for_installable_update"));
		Result->SetStringField(TEXT("message"), TEXT("Checking GitHub for latest release. If available, an install notification will appear."));
		return FMonolithActionResult::Success(Result);
	}

	return FMonolithActionResult::Error(
		FString::Printf(TEXT("Unknown update action: %s. Use 'check' or 'install'."), *Action),
		FMonolithJsonUtils::ErrInvalidParams
	);
}

FMonolithActionResult FMonolithCoreTools::HandleReindex(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (!FModuleManager::Get().IsModuleLoaded(TEXT("MonolithIndex")))
	{
		Result->SetStringField(TEXT("status"), TEXT("module_not_loaded"));
		Result->SetStringField(TEXT("message"), TEXT("MonolithIndex module is not loaded."));
		return FMonolithActionResult::Success(Result);
	}

	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}

	bool bForce = Params.IsValid() && Params->HasField(TEXT("force"))
	              && Params->GetBoolField(TEXT("force"));

	UClass* IndexSubsystemClass = FindObject<UClass>(nullptr, TEXT("/Script/MonolithIndex.MonolithIndexSubsystem"));
	if (!IndexSubsystemClass)
	{
		Result->SetStringField(TEXT("status"), TEXT("subsystem_unavailable"));
		Result->SetStringField(TEXT("message"), TEXT("MonolithIndexSubsystem class not found."));
		return FMonolithActionResult::Success(Result);
	}

	UEditorSubsystem* IndexSubsystem = GEditor->GetEditorSubsystemBase(IndexSubsystemClass);
	if (!IndexSubsystem)
	{
		Result->SetStringField(TEXT("status"), TEXT("subsystem_unavailable"));
		Result->SetStringField(TEXT("message"), TEXT("MonolithIndexSubsystem instance not available."));
		return FMonolithActionResult::Success(Result);
	}

	FString FuncName;
	if (bForce)
	{
		FuncName = TEXT("StartFullIndex");
	}
	else
	{
		// Check if incremental is possible
		UFunction* CanIncrementalFunc = IndexSubsystemClass->FindFunctionByName(TEXT("CanDoIncrementalIndex"));
		if (CanIncrementalFunc)
		{
			struct { uint8 ReturnValue = 0; } Parms;
			FMemory::Memzero(&Parms, sizeof(Parms));
			IndexSubsystem->ProcessEvent(CanIncrementalFunc, &Parms);
			FuncName = Parms.ReturnValue != 0 ? TEXT("StartIncrementalIndex") : TEXT("StartFullIndex");
		}
		else
		{
			// Fallback if CanDoIncrementalIndex not found (old MonolithIndex version)
			FuncName = TEXT("StartFullIndex");
		}
	}

	UFunction* Func = IndexSubsystemClass->FindFunctionByName(*FuncName);
	if (Func)
	{
		IndexSubsystem->ProcessEvent(Func, nullptr);
		Result->SetStringField(TEXT("status"), TEXT("reindex_started"));
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("%s triggered successfully."),
				FuncName == TEXT("StartFullIndex") ? TEXT("Full re-index") : TEXT("Incremental index")));
	}
	else
	{
		Result->SetStringField(TEXT("status"), TEXT("function_not_found"));
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Function %s not found."), *FuncName));
	}

	return FMonolithActionResult::Success(Result);
}
