#include "MonolithConfigActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProperties.h"

#if WITH_EDITOR
// `set_developer_setting` (dev-gated write) deps.
#include "Engine/DeveloperSettings.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "Misc/StringOutputDevice.h"
#else
// UE 5.6 hasn't split FStringOutputDevice out of UnrealString.h yet.
#include "Containers/UnrealString.h"
#endif
#endif // WITH_EDITOR

// ============================================================================
// Registration
// ============================================================================

void FMonolithConfigActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("config"), TEXT("resolve_setting"),
		TEXT("Get effective value of a config key across the full INI hierarchy"),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::ResolveSetting),
		FParamSchemaBuilder()
			.Required(TEXT("file"), TEXT("string"), TEXT("Config category (e.g. Engine, Game, Input)"))
			.Required(TEXT("section"), TEXT("string"), TEXT("Config section (e.g. /Script/Engine.RendererSettings)"))
			.Required(TEXT("key"), TEXT("string"), TEXT("Config key name"))
			.Build());

	Registry.RegisterAction(TEXT("config"), TEXT("explain_setting"),
		TEXT("Show where a config value comes from across Base->Default->User layers"),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::ExplainSetting),
		FParamSchemaBuilder()
			.Optional(TEXT("file"), TEXT("string"), TEXT("Config category (e.g. Engine, Game)"))
			.Optional(TEXT("section"), TEXT("string"), TEXT("Config section"))
			.Optional(TEXT("key"), TEXT("string"), TEXT("Config key name"))
			.Optional(TEXT("setting"), TEXT("string"), TEXT("Convenience: search for this key across common categories"))
			.Build());

	Registry.RegisterAction(TEXT("config"), TEXT("diff_from_default"),
		TEXT("Show project config overrides vs engine defaults for a category"),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::DiffFromDefault),
		FParamSchemaBuilder()
			.Required(TEXT("file"), TEXT("string"), TEXT("Config category to diff (e.g. Engine, Game)"))
			.Optional(TEXT("section"), TEXT("string"), TEXT("Filter to a specific section"))
			.Build());

	Registry.RegisterAction(TEXT("config"), TEXT("search_config"),
		TEXT("Full-text search across all config files"),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::SearchConfig),
		FParamSchemaBuilder()
			.Required(TEXT("query"), TEXT("string"), TEXT("Search text"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Filter to a config category"))
			.Build());

	Registry.RegisterAction(TEXT("config"), TEXT("get_section"),
		TEXT("Read an entire config section from a specific file"),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::GetSection),
		FParamSchemaBuilder()
			.Required(TEXT("file"), TEXT("string"), TEXT("Config file name or category"))
			.Required(TEXT("section"), TEXT("string"), TEXT("Section name"))
			.Build());

	Registry.RegisterAction(TEXT("config"), TEXT("get_config_files"),
		TEXT("List all config files with their hierarchy level"),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::GetConfigFiles),
		FParamSchemaBuilder()
			.Optional(TEXT("category"), TEXT("string"), TEXT("Filter to a specific category"))
			.Build());

#if WITH_EDITOR
	// DEV-ONLY (write): mutate a UDeveloperSettings CDO at runtime. Never registers
	// in shipping/runtime builds — wraps both registration AND handler. Solves the
	// "INI edit + editor restart fights config hierarchy" loop documented in
	// Docs/plans/2026-05-29-ri-ergonomics-improvements-handover.md (item #7).
	Registry.RegisterAction(TEXT("config"), TEXT("set_developer_setting"),
		TEXT("DEV-ONLY (write): set a property on a UDeveloperSettings CDO at runtime. "
			 "Resolves a settings class by short-name (e.g. 'MonolithReflectionIntelSettings') "
			 "or full path ('/Script/Module.Class'), parses `value` via UProperty::ImportText_Direct, "
			 "and optionally persists back to the INI via SaveConfig(). #if WITH_EDITOR-gated."),
		FMonolithActionHandler::CreateStatic(&FMonolithConfigActions::SetDeveloperSetting),
		FParamSchemaBuilder()
			.Required(TEXT("class"), TEXT("string"),
				TEXT("Settings class short-name (e.g. 'MonolithReflectionIntelSettings') or full path ('/Script/MonolithReflectionIntel.MonolithReflectionIntelSettings')."))
			.Required(TEXT("property"), TEXT("string"),
				TEXT("Property name on the CDO (e.g. 'bIndexMarketplacePluginReflection')."))
			.Required(TEXT("value"), TEXT("string"),
				TEXT("Value as text — parsed via UProperty::ImportText_Direct. Examples: 'true', '42', '0.75', '(X=1,Y=2)'."))
			.Optional(TEXT("save_config"), TEXT("boolean"),
				TEXT("Also write back to the persistent INI via UObject::SaveConfig()."),
				TEXT("false"))
			.Build());
#endif // WITH_EDITOR
}

// ============================================================================
// Helpers
// ============================================================================

FString FMonolithConfigActions::ResolveConfigFilePath(const FString& ShortName)
{
	// Handle known shortnames
	if (ShortName.StartsWith(TEXT("Base")))
	{
		// Engine base configs: e.g. BaseEngine.ini
		return FPaths::Combine(FPaths::EngineConfigDir(), ShortName + TEXT(".ini"));
	}
	else if (ShortName.StartsWith(TEXT("Default")))
	{
		// Project default configs: e.g. DefaultEngine.ini
		return FPaths::Combine(FPaths::ProjectConfigDir(), ShortName + TEXT(".ini"));
	}
	else if (ShortName.Contains(TEXT("/")) || ShortName.Contains(TEXT("\\")))
	{
		// Already a path
		return ShortName;
	}
	else
	{
		// Try project config dir first
		FString ProjectPath = FPaths::Combine(FPaths::ProjectConfigDir(), ShortName + TEXT(".ini"));
		if (FPaths::FileExists(ProjectPath))
		{
			return ProjectPath;
		}
		// Fall back to engine config dir
		return FPaths::Combine(FPaths::EngineConfigDir(), ShortName + TEXT(".ini"));
	}
}

TArray<TPair<FString, FString>> FMonolithConfigActions::GetConfigHierarchy(const FString& Category)
{
	TArray<TPair<FString, FString>> Hierarchy;

	// Engine base
	FString BaseFile = FPaths::Combine(FPaths::EngineConfigDir(), FString::Printf(TEXT("Base%s.ini"), *Category));
	if (FPaths::FileExists(BaseFile))
	{
		Hierarchy.Add(TPair<FString, FString>(TEXT("Engine Base"), BaseFile));
	}

	// Project default
	FString DefaultFile = FPaths::Combine(FPaths::ProjectConfigDir(), FString::Printf(TEXT("Default%s.ini"), *Category));
	if (FPaths::FileExists(DefaultFile))
	{
		Hierarchy.Add(TPair<FString, FString>(TEXT("Project Default"), DefaultFile));
	}

	// User saved (platform-specific)
	FString SavedFile = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Config"), FPlatformProperties::PlatformName(),
		FString::Printf(TEXT("%s.ini"), *Category));
	if (FPaths::FileExists(SavedFile))
	{
		Hierarchy.Add(TPair<FString, FString>(TEXT("User Saved"), SavedFile));
	}

	return Hierarchy;
}

// ============================================================================
// Action: resolve_setting
// Params: { "file": "Engine"|"Game"|..., "section": "/Script/...", "key": "..." }
// ============================================================================

FMonolithActionResult FMonolithConfigActions::ResolveSetting(const TSharedPtr<FJsonObject>& Params)
{
	FString Category = Params->GetStringField(TEXT("file"));
	FString Section = Params->GetStringField(TEXT("section"));
	FString Key = Params->GetStringField(TEXT("key"));

	// Use GConfig to get the effective (fully-resolved) value
	FString ConfigFilename = FString::Printf(TEXT("%s%s.ini"), *FPaths::ProjectConfigDir(), *FString::Printf(TEXT("Default%s"), *Category));

	FString Value;
	bool bFound = GConfig->GetString(*Section, *Key, Value, GConfig->GetConfigFilename(*Category));

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("category"), Category);
	ResultJson->SetStringField(TEXT("section"), Section);
	ResultJson->SetStringField(TEXT("key"), Key);
	ResultJson->SetBoolField(TEXT("found"), bFound);

	if (bFound)
	{
		ResultJson->SetStringField(TEXT("value"), Value);
	}

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: explain_setting
// Params: { "file": "Engine"|"Game"|..., "section": "/Script/...", "key": "..." }
// ============================================================================

FMonolithActionResult FMonolithConfigActions::ExplainSetting(const TSharedPtr<FJsonObject>& Params)
{
	FString Category = Params->GetStringField(TEXT("file"));
	FString Section = Params->GetStringField(TEXT("section"));
	FString Key = Params->GetStringField(TEXT("key"));

	// Convenience: if 'setting' param provided instead of file/section/key, search for it
	if (Category.IsEmpty() && Section.IsEmpty() && Key.IsEmpty())
	{
		FString Setting = Params->GetStringField(TEXT("setting"));
		if (!Setting.IsEmpty())
		{
			Key = Setting;
			// Search common config categories for this key
			TArray<FString> SearchCategories = { TEXT("Engine"), TEXT("Game"), TEXT("Input"), TEXT("Editor") };
			for (const FString& Cat : SearchCategories)
			{
				FString ConfigFile = GConfig->GetConfigFilename(*Cat);
				TArray<FString> SectionNames;
				GConfig->GetSectionNames(ConfigFile, SectionNames);
				for (const FString& Sec : SectionNames)
				{
					FString Value;
					if (GConfig->GetString(*Sec, *Setting, Value, ConfigFile))
					{
						Category = Cat;
						Section = Sec;
						break;
					}
				}
				if (!Category.IsEmpty()) break;
			}
		}
	}

	TArray<TPair<FString, FString>> Hierarchy = GetConfigHierarchy(Category);

	TArray<TSharedPtr<FJsonValue>> LayersArray;
	FString EffectiveValue;
	FString EffectiveSource;

	// Parse each layer file as text to find the key
	for (const auto& Layer : Hierarchy)
	{
		FString FileContents;
		if (!FFileHelper::LoadFileToString(FileContents, *Layer.Value))
		{
			continue;
		}

		TArray<FString> Lines;
		FileContents.ParseIntoArrayLines(Lines);

		bool bInSection = false;
		for (const FString& Line : Lines)
		{
			FString Trimmed = Line.TrimStartAndEnd();

			if (Trimmed.StartsWith(TEXT("[")) && Trimmed.Contains(TEXT("]")))
			{
				int32 EndBracket;
				if (Trimmed.FindChar(']', EndBracket))
				{
					FString SectionName = Trimmed.Mid(1, EndBracket - 1);
					bInSection = (SectionName == Section);
				}
				continue;
			}

			if (bInSection && !Trimmed.IsEmpty() && !Trimmed.StartsWith(TEXT(";")))
			{
				FString LineKey, LineValue;
				if (Trimmed.Split(TEXT("="), &LineKey, &LineValue))
				{
					// Strip +/- prefixes for array operations
					FString CleanKey = LineKey.TrimStartAndEnd();
					if (CleanKey.StartsWith(TEXT("+")) || CleanKey.StartsWith(TEXT("-")) || CleanKey.StartsWith(TEXT(".")))
					{
						CleanKey = CleanKey.Mid(1);
					}

					if (CleanKey == Key)
					{
						FString Val = LineValue.TrimStartAndEnd();

						auto LayerJson = MakeShared<FJsonObject>();
						LayerJson->SetStringField(TEXT("layer"), Layer.Key);
						LayerJson->SetStringField(TEXT("file"), Layer.Value);
						LayerJson->SetStringField(TEXT("value"), Val);
						LayerJson->SetStringField(TEXT("raw_line"), Trimmed);
						LayersArray.Add(MakeShared<FJsonValueObject>(LayerJson));

						EffectiveValue = Val;
						EffectiveSource = Layer.Key;
					}
				}
			}
		}
	}

	// Also get the final resolved value from GConfig
	FString ResolvedValue;
	bool bFound = GConfig->GetString(*Section, *Key, ResolvedValue, GConfig->GetConfigFilename(*Category));

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("category"), Category);
	ResultJson->SetStringField(TEXT("section"), Section);
	ResultJson->SetStringField(TEXT("key"), Key);
	ResultJson->SetArrayField(TEXT("layers"), LayersArray);
	ResultJson->SetBoolField(TEXT("found"), bFound);

	if (bFound)
	{
		ResultJson->SetStringField(TEXT("effective_value"), ResolvedValue);
		ResultJson->SetStringField(TEXT("effective_source"), EffectiveSource);
	}

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: diff_from_default
// Params: { "file": "Engine"|"Game"|..., "section": "/Script/..." (optional) }
// ============================================================================

/** Helper: collect entries from GConfig's public GetSection API (returns "Key=Value" pairs) */
static TMap<FString, TArray<FString>> CollectEntriesFromGConfig(const FString& SectionName, const FString& ConfigFilename)
{
	TMap<FString, TArray<FString>> Result;
	TArray<FString> Pairs;
	if (GConfig->GetSection(*SectionName, Pairs, ConfigFilename))
	{
		for (const FString& Pair : Pairs)
		{
			FString Key, Value;
			if (Pair.Split(TEXT("="), &Key, &Value))
			{
				Key.TrimStartAndEndInline();
				Value.TrimStartAndEndInline();
				Result.FindOrAdd(Key).Add(Value);
			}
		}
	}
	return Result;
}

/** Helper: parse all sections from INI file text into a nested map */
static TMap<FString, TMap<FString, TArray<FString>>> ParseIniTextSections(const FString& IniText)
{
	TMap<FString, TMap<FString, TArray<FString>>> AllSections;
	TArray<FString> Lines;
	IniText.ParseIntoArrayLines(Lines);

	FString CurrentSection;
	for (const FString& Line : Lines)
	{
		FString Trimmed = Line.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || Trimmed.StartsWith(TEXT(";")))
		{
			continue;
		}

		if (Trimmed.StartsWith(TEXT("[")) && Trimmed.Contains(TEXT("]")))
		{
			int32 EndBracket;
			Trimmed.FindChar(']', EndBracket);
			CurrentSection = Trimmed.Mid(1, EndBracket - 1);
			continue;
		}

		if (!CurrentSection.IsEmpty())
		{
			FString Key, Value;
			if (Trimmed.Split(TEXT("="), &Key, &Value))
			{
				Key.TrimStartAndEndInline();
				// Strip INI action prefixes (+, -, ., !)
				if (Key.Len() > 0 && (Key[0] == '+' || Key[0] == '-' || Key[0] == '.' || Key[0] == '!'))
				{
					Key.RightChopInline(1);
				}
				Value.TrimStartAndEndInline();
				AllSections.FindOrAdd(CurrentSection).FindOrAdd(Key).Add(Value);
			}
		}
	}
	return AllSections;
}

/** Helper: emit a single diff entry as JSON, handling scalar vs array values */
static TSharedPtr<FJsonObject> MakeDiffEntry(
	const FString& SectionName,
	const FString& Key,
	const FString& ChangeType,
	const TArray<FString>& ResolvedValues,
	const TArray<FString>* BaseValues)
{
	auto DiffJson = MakeShared<FJsonObject>();
	DiffJson->SetStringField(TEXT("section"), SectionName);
	DiffJson->SetStringField(TEXT("key"), Key);
	DiffJson->SetStringField(TEXT("change_type"), ChangeType);

	if (ResolvedValues.Num() == 1)
	{
		DiffJson->SetStringField(TEXT("project_value"), ResolvedValues[0]);
	}
	else
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& V : ResolvedValues)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(V));
		}
		DiffJson->SetArrayField(TEXT("project_values"), JsonValues);
	}

	if (BaseValues)
	{
		if (BaseValues->Num() == 1)
		{
			DiffJson->SetStringField(TEXT("engine_value"), (*BaseValues)[0]);
		}
		else
		{
			TArray<TSharedPtr<FJsonValue>> JsonValues;
			for (const FString& V : *BaseValues)
			{
				JsonValues.Add(MakeShared<FJsonValueString>(V));
			}
			DiffJson->SetArrayField(TEXT("engine_values"), JsonValues);
		}
	}

	return DiffJson;
}

FMonolithActionResult FMonolithConfigActions::DiffFromDefault(const TSharedPtr<FJsonObject>& Params)
{
	FString Category = Params->GetStringField(TEXT("file"));
	FString FilterSection = Params->HasField(TEXT("section")) ? Params->GetStringField(TEXT("section")) : TEXT("");

	// Strip 'Default' or 'Base' prefix if user passed it (e.g. "DefaultEngine" -> "Engine")
	if (Category.StartsWith(TEXT("Default")))
	{
		Category = Category.Mid(7);
	}
	else if (Category.StartsWith(TEXT("Base")))
	{
		Category = Category.Mid(4);
	}

	// Get the fully-resolved config from GConfig (all layers merged: Base + Default + Platform + Saved)
	FString ConfigFilename = GConfig->GetConfigFilename(*Category);
	if (ConfigFilename.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No config found for category '%s'"), *Category));
	}

	// Load engine base config as text for comparison (avoids private FConfigFile API)
	FString BaseConfigPath = FPaths::EngineConfigDir() / (TEXT("Base") + Category + TEXT(".ini"));
	FString BaseConfigText;
	FFileHelper::LoadFileToString(BaseConfigText, *BaseConfigPath);
	auto BaseData = ParseIniTextSections(BaseConfigText);

	// Iterate all sections in the resolved config and diff against engine base
	TArray<FString> SectionNames;
	GConfig->GetSectionNames(ConfigFilename, SectionNames);

	TArray<TSharedPtr<FJsonValue>> DiffsArray;

	for (const FString& SectionName : SectionNames)
	{
		if (!FilterSection.IsEmpty() && SectionName != FilterSection)
		{
			continue;
		}

		// Get effective (resolved) entries from GConfig — includes all merged layers
		auto ResolvedEntries = CollectEntriesFromGConfig(SectionName, ConfigFilename);

		// Get engine base entries from the parsed base config text
		TMap<FString, TArray<FString>> BaseEntries;
		if (const auto* BaseSectionPtr = BaseData.Find(SectionName))
		{
			BaseEntries = *BaseSectionPtr;
		}

		// Find keys that were added or modified by the project
		for (const auto& Entry : ResolvedEntries)
		{
			const FString& Key = Entry.Key;
			const TArray<FString>& ResolvedValues = Entry.Value;
			const TArray<FString>* BaseValues = BaseEntries.Find(Key);

			if (!BaseValues)
			{
				DiffsArray.Add(MakeShared<FJsonValueObject>(
					MakeDiffEntry(SectionName, Key, TEXT("added"), ResolvedValues, nullptr)));
			}
			else if (*BaseValues != ResolvedValues)
			{
				DiffsArray.Add(MakeShared<FJsonValueObject>(
					MakeDiffEntry(SectionName, Key, TEXT("modified"), ResolvedValues, BaseValues)));
			}
		}

		// Find keys that were removed by the project (present in base but not in resolved)
		for (const auto& BaseEntry : BaseEntries)
		{
			if (!ResolvedEntries.Contains(BaseEntry.Key))
			{
				TArray<FString> EmptyValues;
				DiffsArray.Add(MakeShared<FJsonValueObject>(
					MakeDiffEntry(SectionName, BaseEntry.Key, TEXT("removed"), EmptyValues, &BaseEntry.Value)));
			}
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("category"), Category);
	if (!FilterSection.IsEmpty())
	{
		ResultJson->SetStringField(TEXT("filter_section"), FilterSection);
	}
	ResultJson->SetNumberField(TEXT("diff_count"), DiffsArray.Num());
	ResultJson->SetArrayField(TEXT("diffs"), DiffsArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: search_config
// Params: { "query": "...", "file": "Engine" (optional) }
// ============================================================================

FMonolithActionResult FMonolithConfigActions::SearchConfig(const TSharedPtr<FJsonObject>& Params)
{
	FString Query = Params->GetStringField(TEXT("query"));
	FString FilterCategory = Params->HasField(TEXT("category")) ? Params->GetStringField(TEXT("category")) : TEXT("");

	// Gather config directories to search
	TArray<FString> ConfigDirs;
	ConfigDirs.Add(FPaths::EngineConfigDir());
	ConfigDirs.Add(FPaths::ProjectConfigDir());

	TArray<TSharedPtr<FJsonValue>> MatchesArray;
	int32 MaxResults = 100;

	for (const FString& ConfigDir : ConfigDirs)
	{
		TArray<FString> IniFiles;
		IFileManager::Get().FindFiles(IniFiles, *FPaths::Combine(ConfigDir, TEXT("*.ini")), true, false);

		for (const FString& IniFile : IniFiles)
		{
			// If filtering by category, check filename
			if (!FilterCategory.IsEmpty())
			{
				if (!IniFile.Contains(FilterCategory))
				{
					continue;
				}
			}

			FString FullPath = FPaths::Combine(ConfigDir, IniFile);
			FString FileContents;
			if (!FFileHelper::LoadFileToString(FileContents, *FullPath))
			{
				continue;
			}

			// Search line by line
			TArray<FString> Lines;
			FileContents.ParseIntoArrayLines(Lines);

			FString CurrentSection;
			for (int32 LineIdx = 0; LineIdx < Lines.Num() && MatchesArray.Num() < MaxResults; ++LineIdx)
			{
				const FString& Line = Lines[LineIdx];

				// Track sections
				if (Line.StartsWith(TEXT("[")) && Line.Contains(TEXT("]")))
				{
					int32 EndBracket;
					if (Line.FindChar(']', EndBracket))
					{
						CurrentSection = Line.Mid(1, EndBracket - 1);
					}
				}

				if (Line.Contains(Query))
				{
					auto MatchJson = MakeShared<FJsonObject>();
					MatchJson->SetStringField(TEXT("file"), IniFile);
					MatchJson->SetStringField(TEXT("path"), FullPath);
					MatchJson->SetStringField(TEXT("section"), CurrentSection);
					MatchJson->SetNumberField(TEXT("line"), LineIdx + 1);
					MatchJson->SetStringField(TEXT("text"), Line.TrimStartAndEnd());
					MatchesArray.Add(MakeShared<FJsonValueObject>(MatchJson));
				}
			}
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("query"), Query);
	ResultJson->SetNumberField(TEXT("match_count"), MatchesArray.Num());
	ResultJson->SetArrayField(TEXT("matches"), MatchesArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_section
// Params: { "file": "DefaultEngine"|"BaseEngine"|..., "section": "/Script/..." }
// ============================================================================

FMonolithActionResult FMonolithConfigActions::GetSection(const TSharedPtr<FJsonObject>& Params)
{
	FString FileShortName = Params->GetStringField(TEXT("file"));
	FString Section = Params->GetStringField(TEXT("section"));

	// Support category-style names (e.g., "Engine" -> "DefaultEngine" or "BaseEngine")
	if (!FileShortName.StartsWith(TEXT("Default")) && !FileShortName.StartsWith(TEXT("Base"))
		&& !FileShortName.Contains(TEXT("/")) && !FileShortName.Contains(TEXT("\\")))
	{
		FString DefaultPath = ResolveConfigFilePath(TEXT("Default") + FileShortName);
		FString BasePath = ResolveConfigFilePath(TEXT("Base") + FileShortName);

		if (FPaths::FileExists(DefaultPath))
		{
			FileShortName = TEXT("Default") + FileShortName;
		}
		else if (FPaths::FileExists(BasePath))
		{
			FileShortName = TEXT("Base") + FileShortName;
		}
	}

	FString FilePath = ResolveConfigFilePath(FileShortName);

	if (!FPaths::FileExists(FilePath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Config file not found: '%s' (resolved to '%s')"), *FileShortName, *FilePath));
	}

	FString FileContents;
	if (!FFileHelper::LoadFileToString(FileContents, *FilePath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to read config file: '%s'"), *FilePath));
	}

	// Parse manually to get the raw section content
	TArray<FString> Lines;
	FileContents.ParseIntoArrayLines(Lines);

	bool bInSection = false;
	auto EntriesJson = MakeShared<FJsonObject>();
	int32 EntryCount = 0;

	for (const FString& Line : Lines)
	{
		FString Trimmed = Line.TrimStartAndEnd();

		if (Trimmed.StartsWith(TEXT("[")) && Trimmed.Contains(TEXT("]")))
		{
			if (bInSection)
			{
				break; // We've passed our section
			}

			int32 EndBracket;
			if (Trimmed.FindChar(']', EndBracket))
			{
				FString SectionName = Trimmed.Mid(1, EndBracket - 1);
				if (SectionName == Section)
				{
					bInSection = true;
				}
			}
			continue;
		}

		if (bInSection && !Trimmed.IsEmpty() && !Trimmed.StartsWith(TEXT(";")))
		{
			// Parse key=value or +key=value
			FString Key, Value;
			if (Trimmed.Split(TEXT("="), &Key, &Value))
			{
				Key = Key.TrimStartAndEnd();
				Value = Value.TrimStartAndEnd();
				EntriesJson->SetStringField(Key, Value);
				EntryCount++;
			}
		}
	}

	if (!bInSection)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Section '%s' not found in '%s'"), *Section, *FileShortName));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("file"), FileShortName);
	ResultJson->SetStringField(TEXT("file_path"), FilePath);
	ResultJson->SetStringField(TEXT("section"), Section);
	ResultJson->SetNumberField(TEXT("entry_count"), EntryCount);
	ResultJson->SetObjectField(TEXT("entries"), EntriesJson);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_config_files
// Params: { "category": "Engine" (optional — if omitted, lists all) }
// ============================================================================

FMonolithActionResult FMonolithConfigActions::GetConfigFiles(const TSharedPtr<FJsonObject>& Params)
{
	FString FilterCategory = Params->HasField(TEXT("category")) ? Params->GetStringField(TEXT("category")) : TEXT("");

	TArray<TSharedPtr<FJsonValue>> FilesArray;

	// Helper to add files from a directory with a label
	auto AddFilesFromDir = [&](const FString& Dir, const FString& HierarchyLevel)
	{
		TArray<FString> IniFiles;
		IFileManager::Get().FindFiles(IniFiles, *FPaths::Combine(Dir, TEXT("*.ini")), true, false);

		for (const FString& IniFile : IniFiles)
		{
			if (!FilterCategory.IsEmpty())
			{
				if (!IniFile.Contains(FilterCategory))
				{
					continue;
				}
			}

			FString FullPath = FPaths::Combine(Dir, IniFile);
			int64 FileSize = IFileManager::Get().FileSize(*FullPath);

			auto FileJson = MakeShared<FJsonObject>();
			FileJson->SetStringField(TEXT("name"), IniFile);
			FileJson->SetStringField(TEXT("path"), FullPath);
			FileJson->SetStringField(TEXT("hierarchy_level"), HierarchyLevel);
			FileJson->SetNumberField(TEXT("size_bytes"), static_cast<double>(FileSize));
			FilesArray.Add(MakeShared<FJsonValueObject>(FileJson));
		}
	};

	AddFilesFromDir(FPaths::EngineConfigDir(), TEXT("Engine Base"));
	AddFilesFromDir(FPaths::ProjectConfigDir(), TEXT("Project Default"));

	FString SavedConfigDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Config"), FPlatformProperties::PlatformName());
	if (FPaths::DirectoryExists(SavedConfigDir))
	{
		AddFilesFromDir(SavedConfigDir, TEXT("User Saved"));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	if (!FilterCategory.IsEmpty())
	{
		ResultJson->SetStringField(TEXT("filter_category"), FilterCategory);
	}
	ResultJson->SetNumberField(TEXT("file_count"), FilesArray.Num());
	ResultJson->SetArrayField(TEXT("files"), FilesArray);

	return FMonolithActionResult::Success(ResultJson);
}

#if WITH_EDITOR
// ============================================================================
// Action: set_developer_setting   (DEV-ONLY, #if WITH_EDITOR gated)
// Params: { "class": "...", "property": "...", "value": "...", "save_config"?: bool }
// ============================================================================
//
// API verification (per .claude/rules/always/ue57-api.md, editor was down — verified
// via Grep on UE 5.7 engine source at C:\Program Files (x86)\UE_5.7\Engine\Source):
//
//   - `FProperty::ImportText_Direct(const TCHAR* Buffer, void* PropertyPtr,
//       UObject* OwnerObject, int32 PortFlags, FOutputDevice* ErrorText = (FOutputDevice*)GWarn) const`
//     — Runtime/CoreUObject/Public/UObject/UnrealType.h:623
//
//   - `FProperty::ExportText_Direct(FString& ValueStr, const void* Data, const void* Delta,
//       UObject* Parent, int32 PortFlags, UObject* ExportRootScope = nullptr) const`
//     — Runtime/CoreUObject/Public/UObject/UnrealType.h:723
//
//   - `EFindFirstObjectOptions::NativeFirst` (enum-class flag, UObject/UObjectGlobals.h:495).
//     Project pattern (28+ call sites) uses
//     `FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst)` for short-name
//     resolution. Full-path resolution uses `FindObject<UClass>(nullptr, *FullPath)` — both
//     forms are tried here for caller convenience.
//
//   - `UDeveloperSettings` (Runtime/DeveloperSettings/Public/Engine/DeveloperSettings.h:23).
//     Module is `DeveloperSettings` (NOT `Engine`) — added to MonolithConfig.Build.cs.
//
//   - `UObject::SaveConfig(uint64 Flags = CPF_Config, const TCHAR* Filename = nullptr, ...)`
//     — canonical persistence path; respects the class's `config=...` UCLASS meta and writes
//     to the resolved INI (e.g. `Config/MonolithSettings.ini`).
// ============================================================================

FMonolithActionResult FMonolithConfigActions::SetDeveloperSetting(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("set_developer_setting: missing params object"));
	}

	const FString ClassName = Params->GetStringField(TEXT("class"));
	const FString PropertyName = Params->GetStringField(TEXT("property"));
	const FString NewValueText = Params->GetStringField(TEXT("value"));
	bool bSaveConfig = false;
	Params->TryGetBoolField(TEXT("save_config"), bSaveConfig);

	if (ClassName.IsEmpty() || PropertyName.IsEmpty())
	{
		return FMonolithActionResult::Error(
			TEXT("set_developer_setting: both 'class' and 'property' are required"));
	}

	// 1) Resolve class. Try full-path first (works for '/Script/Module.Class'),
	//    then short-name lookup biased toward native classes.
	UClass* TargetClass = nullptr;
	if (ClassName.StartsWith(TEXT("/Script/")) || ClassName.Contains(TEXT(".")))
	{
		TargetClass = FindObject<UClass>(nullptr, *ClassName);
	}
	if (TargetClass == nullptr)
	{
		TargetClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	}
	// Common 'U'-prefix convenience (matches MonolithBlueprint/MonolithAnimation patterns).
	if (TargetClass == nullptr && !ClassName.StartsWith(TEXT("U")))
	{
		TargetClass = FindFirstObject<UClass>(
			*FString::Printf(TEXT("U%s"), *ClassName), EFindFirstObjectOptions::NativeFirst);
	}

	if (TargetClass == nullptr)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("set_developer_setting: unknown class '%s' — supply short name "
				 "like 'MonolithReflectionIntelSettings' or full path "
				 "'/Script/MonolithReflectionIntel.MonolithReflectionIntelSettings'"),
			*ClassName));
	}

	// 2) Verify the class is UDeveloperSettings-derived (or at minimum a UObject with a CDO).
	//    UDeveloperSettings is enforced because non-developer-settings classes have no
	//    `config=...` UCLASS meta and SaveConfig() would either no-op or write to a
	//    surprising file. Lifting this check would be a larger design decision.
	if (!TargetClass->IsChildOf(UDeveloperSettings::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("set_developer_setting: class '%s' is not derived from UDeveloperSettings "
				 "(this action only mutates settings-class CDOs)."),
			*TargetClass->GetName()));
	}

	UObject* CDO = TargetClass->GetDefaultObject(/*bCreateIfNeeded=*/true);
	if (CDO == nullptr)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("set_developer_setting: class '%s' has no CDO"), *TargetClass->GetName()));
	}

	// 3) Resolve property by name; on miss, surface up to the first 10 property
	//    names on the class as a did-you-mean hint.
	FProperty* TargetProperty = TargetClass->FindPropertyByName(*PropertyName);
	if (TargetProperty == nullptr)
	{
		TArray<FString> KnownNames;
		for (TFieldIterator<FProperty> It(TargetClass); It && KnownNames.Num() < 10; ++It)
		{
			KnownNames.Add(It->GetName());
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("set_developer_setting: unknown property '%s' on class '%s' — known properties (first %d): [%s]"),
			*PropertyName, *TargetClass->GetName(), KnownNames.Num(), *FString::Join(KnownNames, TEXT(", "))));
	}

	void* PropertyValuePtr = TargetProperty->ContainerPtrToValuePtr<void>(CDO);

	// 4) Capture old value via ExportText_Direct for the response payload.
	FString OldValueText;
	TargetProperty->ExportText_Direct(OldValueText, PropertyValuePtr, PropertyValuePtr, CDO, PPF_None);

	// 5) Parse + set via ImportText_Direct. Returns nullptr on parse failure.
	//    Suppress the engine's default GWarn output so a malformed `value` doesn't
	//    spam the log — caller gets a clean error from the action result.
	FStringOutputDevice ImportErrors;
	const TCHAR* ImportResult = TargetProperty->ImportText_Direct(
		*NewValueText, PropertyValuePtr, CDO, PPF_None, &ImportErrors);

	if (ImportResult == nullptr)
	{
		// Restore prior value defensively — ImportText_Direct may have partially
		// mutated the destination on some property kinds before returning null.
		TargetProperty->ImportText_Direct(*OldValueText, PropertyValuePtr, CDO, PPF_None, &ImportErrors);
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("set_developer_setting: failed to parse value '%s' for property '%s' (type=%s)%s"),
			*NewValueText, *PropertyName, *TargetProperty->GetCPPType(),
			ImportErrors.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" — %s"), *ImportErrors)));
	}

	// 6) Capture the post-import value (canonical text form may differ from input —
	//    e.g. "1" → "True" for bool, normalized casing for enums).
	FString NewValueCanonical;
	TargetProperty->ExportText_Direct(NewValueCanonical, PropertyValuePtr, PropertyValuePtr, CDO, PPF_None);

	// 7) Optional persistence. UCLASS(config=Foo, defaultconfig) determines the
	//    target INI; SaveConfig() walks UCLASS meta to pick the right file.
	bool bSaved = false;
	FString SaveError;
	if (bSaveConfig)
	{
		// SaveConfig is non-const + virtual; UDeveloperSettings inherits the UObject impl.
		CDO->SaveConfig();
		bSaved = true;
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("class"), TargetClass->GetName());
	ResultJson->SetStringField(TEXT("class_path"), TargetClass->GetPathName());
	ResultJson->SetStringField(TEXT("property"), PropertyName);
	ResultJson->SetStringField(TEXT("property_type"), TargetProperty->GetCPPType());
	ResultJson->SetStringField(TEXT("old_value"), OldValueText);
	ResultJson->SetStringField(TEXT("new_value"), NewValueCanonical);
	ResultJson->SetBoolField(TEXT("saved"), bSaved);
	return FMonolithActionResult::Success(ResultJson);
}
#endif // WITH_EDITOR
