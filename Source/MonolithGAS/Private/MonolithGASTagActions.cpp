#include "MonolithGASTagActions.h"
#include "MonolithParamSchema.h"
#include "MonolithGASInternal.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsModule.h"
#include "GameplayTagsSettings.h"
#include "GameplayTagsEditorModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetDataTagMap.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/DataTable.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/FileHelper.h"
#include "UObject/SavePackage.h"


// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

void FMonolithGASTagActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("gas"), TEXT("add_gameplay_tags"),
		TEXT("Batch-add gameplay tags. Adds to DefaultGameplayTags.ini by default, or to a DataTable if table_path is provided. Auto-creates parent hierarchy."),
		FMonolithActionHandler::CreateStatic(&HandleAddGameplayTags),
		FParamSchemaBuilder()
			.Required(TEXT("tags"), TEXT("array"), TEXT("Array of tag strings (e.g. [\"Ability.Combat.Melee\", \"State.Dead\"])"))
			.OptionalAssetPath(TEXT("table_path"), TEXT("Path to a GameplayTagTableRow DataTable. If omitted, tags go to DefaultGameplayTags.ini"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("get_tag_hierarchy"),
		TEXT("Return the full gameplay tag tree, optionally filtered to a subtree"),
		FMonolithActionHandler::CreateStatic(&HandleGetTagHierarchy),
		FParamSchemaBuilder()
			.Optional(TEXT("root"), TEXT("string"), TEXT("Root tag to filter subtree (e.g. \"Ability.Combat\")"))
			.Optional(TEXT("depth"), TEXT("integer"), TEXT("Max depth to traverse (default: unlimited)"))
			.Optional(TEXT("include_usage"), TEXT("boolean"), TEXT("If true, count assets referencing each tag"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("search_tag_usage"),
		TEXT("Find assets that reference a gameplay tag"),
		FMonolithActionHandler::CreateStatic(&HandleSearchTagUsage),
		FParamSchemaBuilder()
			.Required(TEXT("tag"), TEXT("string"), TEXT("The gameplay tag to search for"))
			.Optional(TEXT("match_type"), TEXT("string"), TEXT("'exact' or 'partial' (default: exact)"), TEXT("exact"))
			.Build());

	// Phase 2: Productivity
	Registry.RegisterAction(TEXT("gas"), TEXT("scaffold_tag_hierarchy"),
		TEXT("Generate a complete gameplay tag hierarchy from a preset. Creates ~250 tags for survival horror across Ability.*, State.*, Damage.*, Status.*, Cooldown.*, GameplayCue.*, SetByCaller.*, Event.*, Director.*"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldTagHierarchy),
		FParamSchemaBuilder()
			.Required(TEXT("preset"), TEXT("string"), TEXT("Preset name: 'survival_horror'"))
			.OptionalAssetPath(TEXT("save_path"), TEXT("DataTable path to save to. If omitted, tags go to DefaultGameplayTags.ini"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("rename_tag"),
		TEXT("Rename a gameplay tag across all GAS assets (GameplayEffects, Abilities, etc.). Supports dry_run mode."),
		FMonolithActionHandler::CreateStatic(&HandleRenameTag),
		FParamSchemaBuilder()
			.Required(TEXT("old_tag"), TEXT("string"), TEXT("The tag to rename (e.g. 'Damage.Type.Fire')"))
			.Required(TEXT("new_tag"), TEXT("string"), TEXT("The new tag name (e.g. 'Damage.Element.Fire')"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("If true, report what would change without modifying anything"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("remove_gameplay_tags"),
		TEXT("Remove gameplay tags from DefaultGameplayTags.ini with optional reference check"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveGameplayTags),
		FParamSchemaBuilder()
			.Required(TEXT("tags"), TEXT("array"), TEXT("Array of tag strings to remove"))
			.Optional(TEXT("check_references"), TEXT("boolean"), TEXT("If true, check for asset references before removing (default: true)"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("validate_tag_consistency"),
		TEXT("Validate tag consistency: find orphan tags, undefined references, contradictory blocking/required tags on abilities"),
		FMonolithActionHandler::CreateStatic(&HandleValidateTagConsistency),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("path_filter"), TEXT("Restrict validation to assets under this path (e.g. '/Game/GAS')"))
			.Build());

	// ── Phase 3: Advanced ──

	Registry.RegisterAction(TEXT("gas"), TEXT("audit_tag_naming"),
		TEXT("Check gameplay tag naming conventions: PascalCase segments, correct prefixes (Ability.*, State.*, Damage.*, etc.), max depth"),
		FMonolithActionHandler::CreateStatic(&HandleAuditTagNaming),
		FParamSchemaBuilder()
			.Optional(TEXT("conventions"), TEXT("object"), TEXT("Custom conventions: { max_depth?, required_prefixes?, allow_underscores? }"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("export_tag_hierarchy"),
		TEXT("Export the full gameplay tag hierarchy for external review"),
		FMonolithActionHandler::CreateStatic(&HandleExportTagHierarchy),
		FParamSchemaBuilder()
			.Optional(TEXT("format"), TEXT("string"), TEXT("Export format: 'json', 'csv', or 'text' (default: json)"), TEXT("json"))
			.OptionalDiskPath(TEXT("output_path"), TEXT("File path to write (default: returns inline)"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("import_tag_hierarchy"),
		TEXT("Import gameplay tags from a file (JSON array or CSV)"),
		FMonolithActionHandler::CreateStatic(&HandleImportTagHierarchy),
		FParamSchemaBuilder()
			.RequiredDiskPath(TEXT("source_path"), TEXT("Absolute file path to import from"))
			.Optional(TEXT("merge_mode"), TEXT("string"), TEXT("'replace' (clear existing, add imported) or 'merge' (add imported, keep existing). Default: merge"), TEXT("merge"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("If true, report what would change without modifying"), TEXT("false"))
			.Build());
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/** Expand "A.B.C" into {"A", "A.B", "A.B.C"} */
static TArray<FString> ExpandTagHierarchy(const FString& TagString)
{
	TArray<FString> Result;
	TArray<FString> Parts;
	TagString.ParseIntoArray(Parts, TEXT("."));

	FString Accumulator;
	for (int32 i = 0; i < Parts.Num(); ++i)
	{
		if (i > 0) Accumulator += TEXT(".");
		Accumulator += Parts[i];
		Result.Add(Accumulator);
	}
	return Result;
}

/** Check whether a tag already exists in the tag dictionary (any source) */
static bool TagExistsInINI(const FString& TagString)
{
	return UGameplayTagsManager::Get().IsDictionaryTag(FName(*TagString));
}

/** Build a recursive JSON tree node for a tag */
static TSharedPtr<FJsonObject> BuildTagNode(
	const FGameplayTag& Tag,
	UGameplayTagsManager& TagManager,
	bool bIncludeUsage,
	IAssetRegistry* AssetRegistry,
	int32 CurrentDepth,
	int32 MaxDepth)
{
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("tag"), Tag.ToString());

	if (bIncludeUsage && AssetRegistry)
	{
		// Search for assets that reference this tag by scanning tag string in asset data
		FString TagStr = Tag.ToString();
		FARFilter Filter;
		Filter.TagsAndValues.Add(FName("GameplayTagContainer"), TagStr);

		TArray<FAssetData> Assets;
		AssetRegistry->GetAssets(Filter, Assets);
		Node->SetNumberField(TEXT("usage_count"), Assets.Num());
	}

	// Get direct children
	FGameplayTagContainer ChildContainer = TagManager.RequestGameplayTagChildren(Tag);

	// Filter to direct children only (one level deeper)
	FString TagPrefix = Tag.ToString() + TEXT(".");
	TArray<FGameplayTag> DirectChildren;
	for (const FGameplayTag& ChildTag : ChildContainer)
	{
		FString ChildStr = ChildTag.ToString();
		if (ChildStr.StartsWith(TagPrefix))
		{
			// Check it's a direct child (no more dots after the prefix)
			FString Remainder = ChildStr.Mid(TagPrefix.Len());
			if (!Remainder.Contains(TEXT(".")))
			{
				DirectChildren.Add(ChildTag);
			}
		}
	}

	if (DirectChildren.Num() > 0 && (MaxDepth <= 0 || CurrentDepth < MaxDepth))
	{
		TArray<TSharedPtr<FJsonValue>> ChildArray;
		for (const FGameplayTag& Child : DirectChildren)
		{
			TSharedPtr<FJsonObject> ChildNode = BuildTagNode(
				Child, TagManager, bIncludeUsage, AssetRegistry,
				CurrentDepth + 1, MaxDepth);
			ChildArray.Add(MakeShared<FJsonValueObject>(ChildNode));
		}
		Node->SetArrayField(TEXT("children"), ChildArray);
	}

	return Node;
}

// ─────────────────────────────────────────────────────────────────────────────
// add_gameplay_tags
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASTagActions::HandleAddGameplayTags(const TSharedPtr<FJsonObject>& Params)
{
	// Parse required tags array
	TArray<FString> Tags = MonolithGAS::ParseStringArray(Params, TEXT("tags"));
	if (Tags.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: tags"));
	}

	FString TablePath = Params->GetStringField(TEXT("table_path"));

	// Expand all tags to include parent hierarchy
	TSet<FString> AllTags;
	for (const FString& Tag : Tags)
	{
		TArray<FString> Expanded = ExpandTagHierarchy(Tag);
		for (const FString& T : Expanded)
		{
			AllTags.Add(T);
		}
	}

	// Sort for deterministic output
	TArray<FString> SortedTags = AllTags.Array();
	SortedTags.Sort();

	TArray<FString> Added;
	TArray<FString> Skipped;

	if (TablePath.IsEmpty())
	{
		// ── INI mode: add to DefaultGameplayTags.ini via engine API ──
		IGameplayTagsEditorModule& TagsEditor = IGameplayTagsEditorModule::Get();
		UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();

		// Suspend tag tree refresh for batch performance — one rebuild at the end
		FGuid SuspendToken = FGuid::NewGuid();
		TagManager.SuspendEditorRefreshGameplayTagTree(SuspendToken);

		for (const FString& TagStr : SortedTags)
		{
			if (TagExistsInINI(TagStr))
			{
				Skipped.Add(TagStr);
				continue;
			}

			bool bOk = TagsEditor.AddNewGameplayTagToINI(TagStr, TEXT(""), NAME_None, false, true);
			if (bOk)
			{
				Added.Add(TagStr);
			}
			else
			{
				Skipped.Add(TagStr);
			}
		}

		// Resume triggers a single tag tree rebuild
		TagManager.ResumeEditorRefreshGameplayTagTree(SuspendToken);
	}
	else
	{
		// ── DataTable mode ──
		FString Error;
		UObject* Existing = MonolithGAS::LoadAssetFromPath(TablePath, Error);
		UDataTable* DataTable = Cast<UDataTable>(Existing);

		if (!DataTable)
		{
			// Create new DataTable
			FString PackagePath = TablePath;
			FString AssetName;
			int32 LastSlash;
			if (PackagePath.FindLastChar('/', LastSlash))
			{
				AssetName = PackagePath.Mid(LastSlash + 1);
			}
			else
			{
				AssetName = PackagePath;
			}

			// Check for existing asset (AssetRegistry + in-memory multi-tier check)
			FString ExistError;
			if (!MonolithGAS::EnsureAssetPathFree(TablePath, AssetName, ExistError))
			{
				return FMonolithActionResult::Error(ExistError);
			}

			UPackage* Package = MonolithGAS::GetOrCreatePackage(TablePath, Error);
			if (!Package)
			{
				return FMonolithActionResult::Error(Error);
			}

			DataTable = NewObject<UDataTable>(Package, *AssetName, RF_Public | RF_Standalone);
			DataTable->RowStruct = FGameplayTagTableRow::StaticStruct();
		}

		if (!DataTable->RowStruct || DataTable->RowStruct != FGameplayTagTableRow::StaticStruct())
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("DataTable at %s does not use FGameplayTagTableRow"), *TablePath));
		}

		for (const FString& TagStr : SortedTags)
		{
			// Check if row already exists
			FName RowName(*TagStr);
			if (DataTable->FindRow<FGameplayTagTableRow>(RowName, TEXT(""), false))
			{
				Skipped.Add(TagStr);
				continue;
			}

			// Add new row
			FGameplayTagTableRow NewRow;
			NewRow.Tag = FName(*TagStr);
			DataTable->AddRow(RowName, NewRow);
			Added.Add(TagStr);
		}

		if (Added.Num() > 0)
		{
			DataTable->MarkPackageDirty();

			// Save the package
			FString PackageFileName;
			if (FPackageName::TryConvertLongPackageNameToFilename(
				DataTable->GetPackage()->GetName(), PackageFileName,
				FPackageName::GetAssetPackageExtension()))
			{
				FSavePackageArgs SaveArgs;
				SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
				UPackage::SavePackage(DataTable->GetPackage(), DataTable,
					*PackageFileName, SaveArgs);
			}

			// Refresh tags
			UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
			TagManager.EditorRefreshGameplayTagTree();
		}
	}

	// Build result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("added_count"), Added.Num());
	Result->SetNumberField(TEXT("skipped_count"), Skipped.Num());
	Result->SetStringField(TEXT("target"), TablePath.IsEmpty() ? TEXT("DefaultGameplayTags.ini") : TablePath);

	TArray<TSharedPtr<FJsonValue>> AddedArr;
	for (const FString& T : Added)
	{
		AddedArr.Add(MakeShared<FJsonValueString>(T));
	}
	Result->SetArrayField(TEXT("added"), AddedArr);

	TArray<TSharedPtr<FJsonValue>> SkippedArr;
	for (const FString& T : Skipped)
	{
		SkippedArr.Add(MakeShared<FJsonValueString>(T));
	}
	Result->SetArrayField(TEXT("skipped"), SkippedArr);

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// get_tag_hierarchy
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASTagActions::HandleGetTagHierarchy(const TSharedPtr<FJsonObject>& Params)
{
	FString Root = Params->GetStringField(TEXT("root"));
	int32 MaxDepth = 0;
	if (Params->HasField(TEXT("depth")))
	{
		MaxDepth = static_cast<int32>(Params->GetNumberField(TEXT("depth")));
	}
	bool bIncludeUsage = false;
	if (Params->HasField(TEXT("include_usage")))
	{
		bIncludeUsage = Params->GetBoolField(TEXT("include_usage"));
	}

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
	IAssetRegistry* AssetRegistry = bIncludeUsage ?
		&FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get() : nullptr;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (!Root.IsEmpty())
	{
		// Filter to a specific subtree
		FGameplayTag RootTag = FGameplayTag::RequestGameplayTag(FName(*Root), false);
		if (!RootTag.IsValid())
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Tag not found: %s"), *Root));
		}

		TSharedPtr<FJsonObject> RootNode = BuildTagNode(
			RootTag, TagManager, bIncludeUsage, AssetRegistry, 0, MaxDepth);
		Result->SetObjectField(TEXT("root"), RootNode);
	}
	else
	{
		// Full tree — find all root-level tags (no dots)
		FGameplayTagContainer AllTags;
		TagManager.RequestAllGameplayTags(AllTags, false);

		// Find root tags
		TArray<FGameplayTag> RootTags;
		for (const FGameplayTag& Tag : AllTags)
		{
			FString TagStr = Tag.ToString();
			if (!TagStr.Contains(TEXT(".")))
			{
				RootTags.Add(Tag);
			}
		}

		TArray<TSharedPtr<FJsonValue>> RootsArray;
		for (const FGameplayTag& RootTag : RootTags)
		{
			TSharedPtr<FJsonObject> Node = BuildTagNode(
				RootTag, TagManager, bIncludeUsage, AssetRegistry, 0, MaxDepth);
			RootsArray.Add(MakeShared<FJsonValueObject>(Node));
		}
		Result->SetArrayField(TEXT("roots"), RootsArray);
	}

	// Total tag count
	FGameplayTagContainer AllTags;
	TagManager.RequestAllGameplayTags(AllTags, false);
	Result->SetNumberField(TEXT("total_tags"), AllTags.Num());

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// search_tag_usage
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASTagActions::HandleSearchTagUsage(const TSharedPtr<FJsonObject>& Params)
{
	FString TagString;
	FMonolithActionResult ErrorResult;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("tag"), TagString, ErrorResult))
	{
		return ErrorResult;
	}

	FString MatchType = Params->GetStringField(TEXT("match_type"));
	if (MatchType.IsEmpty()) MatchType = TEXT("exact");

	bool bPartial = MatchType.Equals(TEXT("partial"), ESearchCase::IgnoreCase);

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Get all assets and search for tag references
	TArray<FAssetData> AllAssets;
	FARFilter Filter;
	AssetRegistry.GetAllAssets(AllAssets, true);

	TArray<TSharedPtr<FJsonValue>> MatchingAssets;

	for (const FAssetData& Asset : AllAssets)
	{
		// Check all tag values stored in asset registry metadata
		bool bFound = false;

		// Iterate asset tags looking for gameplay tag references
		Asset.TagsAndValues.ForEach([&](TPair<FName, FAssetTagValueRef> TagPair)
		{
			if (bFound) return; // already matched

			FString Value = TagPair.Value.GetValue();

			if (bPartial)
			{
				if (Value.Contains(TagString))
				{
					bFound = true;
				}
			}
			else
			{
				// Exact match — look for the tag string as a complete tag name
				// Tags are stored in various formats: quoted, in containers, etc.
				if (Value.Contains(TEXT("\"") + TagString + TEXT("\"")) ||
					Value.Contains(TEXT("(TagName=\"") + TagString + TEXT("\")")) ||
					Value.Equals(TagString))
				{
					bFound = true;
				}
			}
		});

		if (bFound)
		{
			TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
			AssetObj->SetStringField(TEXT("asset_path"), Asset.GetObjectPathString());
			AssetObj->SetStringField(TEXT("asset_name"), Asset.AssetName.ToString());
			AssetObj->SetStringField(TEXT("asset_class"), Asset.AssetClassPath.GetAssetName().ToString());
			MatchingAssets.Add(MakeShared<FJsonValueObject>(AssetObj));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("tag"), TagString);
	Result->SetStringField(TEXT("match_type"), MatchType);
	Result->SetNumberField(TEXT("result_count"), MatchingAssets.Num());
	Result->SetArrayField(TEXT("assets"), MatchingAssets);

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// scaffold_tag_hierarchy
// ─────────────────────────────────────────────────────────────────────────────

/** Build the survival_horror tag preset (~250 tags). */
static TArray<FString> BuildSurvivalHorrorTagPreset()
{
	TArray<FString> Tags;

	// Ability.Combat
	for (const TCHAR* Sub : {
		TEXT("Ability.Combat.Melee"), TEXT("Ability.Combat.Melee.Light"), TEXT("Ability.Combat.Melee.Heavy"),
		TEXT("Ability.Combat.Melee.Charged"), TEXT("Ability.Combat.Melee.Combo"),
		TEXT("Ability.Combat.Ranged"), TEXT("Ability.Combat.Ranged.Fire"), TEXT("Ability.Combat.Ranged.Reload"),
		TEXT("Ability.Combat.Ranged.ADS"), TEXT("Ability.Combat.Ranged.HipFire"),
		TEXT("Ability.Combat.Throw"), TEXT("Ability.Combat.Throw.Grenade"), TEXT("Ability.Combat.Throw.Molotov"),
		TEXT("Ability.Combat.Block"), TEXT("Ability.Combat.Parry"), TEXT("Ability.Combat.Shove"),
		TEXT("Ability.Combat.Execute"), TEXT("Ability.Combat.Die")
	}) { Tags.Add(Sub); }

	// Ability.Movement
	for (const TCHAR* Sub : {
		TEXT("Ability.Movement.Sprint"), TEXT("Ability.Movement.Dodge"), TEXT("Ability.Movement.Dodge.Roll"),
		TEXT("Ability.Movement.Slide"), TEXT("Ability.Movement.Jump"), TEXT("Ability.Movement.Vault"),
		TEXT("Ability.Movement.Crouch"), TEXT("Ability.Movement.Lean"), TEXT("Ability.Movement.Lean.Left"),
		TEXT("Ability.Movement.Lean.Right")
	}) { Tags.Add(Sub); }

	// Ability.Interaction
	for (const TCHAR* Sub : {
		TEXT("Ability.Interaction.Use"), TEXT("Ability.Interaction.Pickup"), TEXT("Ability.Interaction.Drop"),
		TEXT("Ability.Interaction.Open"), TEXT("Ability.Interaction.Close"), TEXT("Ability.Interaction.Lock"),
		TEXT("Ability.Interaction.Unlock"), TEXT("Ability.Interaction.Push"), TEXT("Ability.Interaction.Pull"),
		TEXT("Ability.Interaction.Examine"), TEXT("Ability.Interaction.Barricade"),
		TEXT("Ability.Interaction.Barricade.Build"), TEXT("Ability.Interaction.Barricade.Destroy")
	}) { Tags.Add(Sub); }

	// Ability.Horror
	for (const TCHAR* Sub : {
		TEXT("Ability.Horror.Investigate"), TEXT("Ability.Horror.Hide"), TEXT("Ability.Horror.Hide.Closet"),
		TEXT("Ability.Horror.Hide.UnderBed"), TEXT("Ability.Horror.HoldBreath"),
		TEXT("Ability.Horror.Panic"), TEXT("Ability.Horror.Panic.Flee"), TEXT("Ability.Horror.Panic.Freeze"),
		TEXT("Ability.Horror.Pray"), TEXT("Ability.Horror.UseLight"),
		TEXT("Ability.Horror.UseLight.Flashlight"), TEXT("Ability.Horror.UseLight.Match"),
		TEXT("Ability.Horror.UseLight.Lighter")
	}) { Tags.Add(Sub); }

	// Ability.Survival
	for (const TCHAR* Sub : {
		TEXT("Ability.Survival.Heal"), TEXT("Ability.Survival.Heal.Bandage"), TEXT("Ability.Survival.Heal.Medkit"),
		TEXT("Ability.Survival.Heal.Syringe"), TEXT("Ability.Survival.Craft"),
		TEXT("Ability.Survival.Consume"), TEXT("Ability.Survival.Consume.Food"), TEXT("Ability.Survival.Consume.Water"),
		TEXT("Ability.Survival.Revive")
	}) { Tags.Add(Sub); }

	// State.Movement
	for (const TCHAR* Sub : {
		TEXT("State.Movement.Sprinting"), TEXT("State.Movement.Crouching"), TEXT("State.Movement.Sliding"),
		TEXT("State.Movement.Jumping"), TEXT("State.Movement.Falling"), TEXT("State.Movement.Vaulting"),
		TEXT("State.Movement.Climbing"), TEXT("State.Movement.Swimming"), TEXT("State.Movement.Prone"),
		TEXT("State.Movement.Immobilized"), TEXT("State.Movement.Slowed")
	}) { Tags.Add(Sub); }

	// State.Combat
	for (const TCHAR* Sub : {
		TEXT("State.Combat.Attacking"), TEXT("State.Combat.Blocking"), TEXT("State.Combat.Reloading"),
		TEXT("State.Combat.ADS"), TEXT("State.Combat.Staggered"), TEXT("State.Combat.Stunned"),
		TEXT("State.Combat.KnockedDown"), TEXT("State.Combat.Invulnerable"), TEXT("State.Combat.Dead"),
		TEXT("State.Combat.Dying"), TEXT("State.Combat.Executing"), TEXT("State.Combat.GettingExecuted")
	}) { Tags.Add(Sub); }

	// State.Status
	for (const TCHAR* Sub : {
		TEXT("State.Status.Burning"), TEXT("State.Status.Frozen"), TEXT("State.Status.Bleeding"),
		TEXT("State.Status.Poisoned"), TEXT("State.Status.Electrified"), TEXT("State.Status.Stunned"),
		TEXT("State.Status.Slowed"), TEXT("State.Status.Weakened"), TEXT("State.Status.Empowered"),
		TEXT("State.Status.Regenerating"), TEXT("State.Status.ShieldActive")
	}) { Tags.Add(Sub); }

	// State.Horror
	for (const TCHAR* Sub : {
		TEXT("State.Horror.FearLow"), TEXT("State.Horror.FearMedium"), TEXT("State.Horror.FearHigh"),
		TEXT("State.Horror.FearMax"), TEXT("State.Horror.Panicking"), TEXT("State.Horror.Hiding"),
		TEXT("State.Horror.HoldingBreath"), TEXT("State.Horror.SanityLow"), TEXT("State.Horror.SanityCritical"),
		TEXT("State.Horror.Hallucinating"), TEXT("State.Horror.Paranoid"),
		TEXT("State.Horror.HeartRateElevated"), TEXT("State.Horror.HeartRateCritical")
	}) { Tags.Add(Sub); }

	// State.Condition
	for (const TCHAR* Sub : {
		TEXT("State.Condition.Healthy"), TEXT("State.Condition.Wounded"), TEXT("State.Condition.Critical"),
		TEXT("State.Condition.Incapacitated"), TEXT("State.Condition.LowStamina"),
		TEXT("State.Condition.Exhausted"), TEXT("State.Condition.Encumbered")
	}) { Tags.Add(Sub); }

	// State.Accessibility
	for (const TCHAR* Sub : {
		TEXT("State.Accessibility.Active"),
		TEXT("State.Accessibility.ReducedHorror"), TEXT("State.Accessibility.InvulnerabilityMode"),
		TEXT("State.Accessibility.NoJumpscares"), TEXT("State.Accessibility.HighContrast")
	}) { Tags.Add(Sub); }

	// Damage.Type
	for (const TCHAR* Sub : {
		TEXT("Damage.Type.Ballistic"), TEXT("Damage.Type.Explosive"), TEXT("Damage.Type.Fire"),
		TEXT("Damage.Type.Electric"), TEXT("Damage.Type.Poison"), TEXT("Damage.Type.Supernatural"),
		TEXT("Damage.Type.Blunt"), TEXT("Damage.Type.Slash"), TEXT("Damage.Type.Pierce"),
		TEXT("Damage.Type.Fall"), TEXT("Damage.Type.Environmental"), TEXT("Damage.Type.DoT")
	}) { Tags.Add(Sub); }

	// Damage.Zone
	for (const TCHAR* Sub : {
		TEXT("Damage.Zone.Head"), TEXT("Damage.Zone.Torso"), TEXT("Damage.Zone.LeftArm"),
		TEXT("Damage.Zone.RightArm"), TEXT("Damage.Zone.LeftLeg"), TEXT("Damage.Zone.RightLeg"),
		TEXT("Damage.Zone.Weak")
	}) { Tags.Add(Sub); }

	// Status (effect identity tags)
	for (const TCHAR* Sub : {
		TEXT("Status.Burning"), TEXT("Status.Burning.Light"), TEXT("Status.Burning.Heavy"),
		TEXT("Status.Frozen"), TEXT("Status.Frozen.Partial"), TEXT("Status.Frozen.Full"),
		TEXT("Status.Bleeding"), TEXT("Status.Bleeding.Light"), TEXT("Status.Bleeding.Heavy"),
		TEXT("Status.Poisoned"), TEXT("Status.Poisoned.Light"), TEXT("Status.Poisoned.Heavy"), TEXT("Status.Poisoned.BypassShield"),
		TEXT("Status.Electrified"), TEXT("Status.Terrified"), TEXT("Status.Possessed"),
		TEXT("Status.Stunned"), TEXT("Status.Slowed"), TEXT("Status.Weakened"),
		TEXT("Status.Cursed"), TEXT("Status.Blinded")
	}) { Tags.Add(Sub); }

	// Cooldown.Ability
	for (const TCHAR* Sub : {
		TEXT("Cooldown.Ability.Generic"),
		TEXT("Cooldown.Ability.Sprint"), TEXT("Cooldown.Ability.Dodge"), TEXT("Cooldown.Ability.Heal"),
		TEXT("Cooldown.Ability.Melee"), TEXT("Cooldown.Ability.Throw"), TEXT("Cooldown.Ability.Interact"),
		TEXT("Cooldown.Ability.Shove"), TEXT("Cooldown.Ability.Block"), TEXT("Cooldown.Ability.Barricade"),
		TEXT("Cooldown.Ability.UseLight")
	}) { Tags.Add(Sub); }

	// GameplayCue
	for (const TCHAR* Sub : {
		TEXT("GameplayCue.Combat.Hit.Melee"), TEXT("GameplayCue.Combat.Hit.Ranged"),
		TEXT("GameplayCue.Combat.Hit.Explosive"), TEXT("GameplayCue.Combat.Muzzle"),
		TEXT("GameplayCue.Combat.Impact.Flesh"), TEXT("GameplayCue.Combat.Impact.Metal"),
		TEXT("GameplayCue.Combat.Impact.Wood"), TEXT("GameplayCue.Combat.Impact.Stone"),
		TEXT("GameplayCue.Combat.Blood.Spray"), TEXT("GameplayCue.Combat.Blood.Pool"),
		TEXT("GameplayCue.Combat.Death"),
		TEXT("GameplayCue.Status.Burning"), TEXT("GameplayCue.Status.Frozen"),
		TEXT("GameplayCue.Status.Bleeding"), TEXT("GameplayCue.Status.Poisoned"),
		TEXT("GameplayCue.Status.Electrified"), TEXT("GameplayCue.Status.Stunned"),
		TEXT("GameplayCue.Status.Heal"), TEXT("GameplayCue.Status.ShieldBreak"),
		TEXT("GameplayCue.Horror.Heartbeat"), TEXT("GameplayCue.Horror.Whisper"),
		TEXT("GameplayCue.Horror.ScreenDistort"), TEXT("GameplayCue.Horror.Hallucination"),
		TEXT("GameplayCue.Horror.Jumpscare"), TEXT("GameplayCue.Horror.SanityPulse"),
		TEXT("GameplayCue.Horror.BreathFog"), TEXT("GameplayCue.Horror.EyeAdapt"),
		TEXT("GameplayCue.Player.LevelUp"), TEXT("GameplayCue.Player.Revive"),
		TEXT("GameplayCue.Player.ItemPickup"), TEXT("GameplayCue.Player.StaminaExhaust")
	}) { Tags.Add(Sub); }

	// SetByCaller
	for (const TCHAR* Sub : {
		TEXT("SetByCaller.Damage.Base"), TEXT("SetByCaller.Damage.Multiplier"),
		TEXT("SetByCaller.Damage.Type"), TEXT("SetByCaller.Damage.Zone"),
		TEXT("SetByCaller.Duration"), TEXT("SetByCaller.Stacks"),
		TEXT("SetByCaller.Horror.FearAmount"), TEXT("SetByCaller.Horror.SanityDrain"),
		TEXT("SetByCaller.Heal.Amount"), TEXT("SetByCaller.Stamina.Cost")
	}) { Tags.Add(Sub); }

	// Event
	for (const TCHAR* Sub : {
		TEXT("Event.Combat.Hit"), TEXT("Event.Combat.Kill"), TEXT("Event.Combat.Headshot"),
		TEXT("Event.Combat.WeaponSwap"), TEXT("Event.Combat.AmmoEmpty"),
		TEXT("Event.Interaction.DoorOpened"), TEXT("Event.Interaction.ItemPickedUp"),
		TEXT("Event.Interaction.SwitchActivated"), TEXT("Event.Interaction.BarricadeBuilt"),
		TEXT("Event.Horror.EnemySpotted"), TEXT("Event.Horror.EnemyLost"),
		TEXT("Event.Horror.SoundHeard"), TEXT("Event.Horror.JumpscareTriggered"),
		TEXT("Event.Horror.SanityBreak"), TEXT("Event.Horror.Hallucination"),
		TEXT("Event.Death"), TEXT("Event.Revive"), TEXT("Event.Respawn"),
		TEXT("Event.LevelStart"), TEXT("Event.LevelEnd"), TEXT("Event.CheckpointReached")
	}) { Tags.Add(Sub); }

	// Director
	for (const TCHAR* Sub : {
		TEXT("Director.Intensity.Low"), TEXT("Director.Intensity.Medium"),
		TEXT("Director.Intensity.High"), TEXT("Director.Intensity.Peak"),
		TEXT("Director.Intensity.Cooldown"),
		TEXT("Director.Phase.Buildup"), TEXT("Director.Phase.Sustain"),
		TEXT("Director.Phase.Peak"), TEXT("Director.Phase.Relax"),
		TEXT("Director.Phase.Rest"),
		TEXT("Director.Spawn.Allow"), TEXT("Director.Spawn.Deny"),
		TEXT("Director.Spawn.Boss"), TEXT("Director.Spawn.Horde"),
		TEXT("Director.Music.Ambient"), TEXT("Director.Music.Tension"),
		TEXT("Director.Music.Combat"), TEXT("Director.Music.Chase")
	}) { Tags.Add(Sub); }

	return Tags;
}

FMonolithActionResult FMonolithGASTagActions::HandleScaffoldTagHierarchy(const TSharedPtr<FJsonObject>& Params)
{
	FString Preset;
	FMonolithActionResult ErrorResult;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("preset"), Preset, ErrorResult))
	{
		return ErrorResult;
	}

	if (!Preset.Equals(TEXT("survival_horror"), ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown preset: '%s'. Available: survival_horror"), *Preset));
	}

	TArray<FString> PresetTags = BuildSurvivalHorrorTagPreset();

	// Delegate to the add_gameplay_tags logic by building the same params
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> TagArray;
	for (const FString& Tag : PresetTags)
	{
		TagArray.Add(MakeShared<FJsonValueString>(Tag));
	}
	AddParams->SetArrayField(TEXT("tags"), TagArray);

	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (!SavePath.IsEmpty())
	{
		AddParams->SetStringField(TEXT("table_path"), SavePath);
	}

	// Reuse the add_gameplay_tags handler
	FMonolithActionResult AddResult = HandleAddGameplayTags(AddParams);

	// Wrap the result with preset info
	if (AddResult.bSuccess && AddResult.Result.IsValid())
	{
		AddResult.Result->SetStringField(TEXT("preset"), Preset);
		AddResult.Result->SetNumberField(TEXT("preset_tag_count"), PresetTags.Num());
	}

	return AddResult;
}

// ─────────────────────────────────────────────────────────────────────────────
// rename_tag
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASTagActions::HandleRenameTag(const TSharedPtr<FJsonObject>& Params)
{
	FString OldTag, NewTag;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("old_tag"), OldTag, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("new_tag"), NewTag, Err)) return Err;

	bool bDryRun = false;
	if (Params->HasField(TEXT("dry_run")))
	{
		bDryRun = Params->GetBoolField(TEXT("dry_run"));
	}

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// 1. Find all assets referencing the old tag
	TArray<FAssetData> AllAssets;
	AssetRegistry.GetAllAssets(AllAssets, true);

	TArray<TSharedPtr<FJsonValue>> AffectedAssets;
	TArray<UBlueprint*> BlueprintsToUpdate;

	for (const FAssetData& Asset : AllAssets)
	{
		bool bFound = false;
		Asset.TagsAndValues.ForEach([&](TPair<FName, FAssetTagValueRef> TagPair)
		{
			if (bFound) return;
			FString Value = TagPair.Value.GetValue();
			if (Value.Contains(TEXT("\"") + OldTag + TEXT("\"")) ||
				Value.Contains(TEXT("(TagName=\"") + OldTag + TEXT("\")")) ||
				Value.Equals(OldTag))
			{
				bFound = true;
			}
		});

		if (bFound)
		{
			TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
			AssetObj->SetStringField(TEXT("asset_path"), Asset.GetObjectPathString());
			AssetObj->SetStringField(TEXT("asset_name"), Asset.AssetName.ToString());
			AssetObj->SetStringField(TEXT("asset_class"), Asset.AssetClassPath.GetAssetName().ToString());
			AffectedAssets.Add(MakeShared<FJsonValueObject>(AssetObj));

			if (!bDryRun)
			{
				// Load and try to update Blueprint CDOs
				UObject* Obj = Asset.GetAsset();
				UBlueprint* BP = Cast<UBlueprint>(Obj);
				if (BP)
				{
					BlueprintsToUpdate.Add(BP);
				}
			}
		}
	}

	int32 UpdatedCount = 0;

	if (!bDryRun)
	{
		FGameplayTag OldGPTag = FGameplayTag::RequestGameplayTag(FName(*OldTag), false);
		FGameplayTag NewGPTag = FGameplayTag::RequestGameplayTag(FName(*NewTag), false);

		// Use engine API to rename tag in INI (adds new tag, creates redirector, optionally renames children)
		IGameplayTagsEditorModule& TagsEditor = IGameplayTagsEditorModule::Get();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		bool bRenameOk = TagsEditor.RenameTagInINI(OldTag, NewTag, /*bRenameChildren=*/ false);
#else
		// UE 5.6's RenameTagInINI doesn't take a bRenameChildren parameter yet.
		bool bRenameOk = TagsEditor.RenameTagInINI(OldTag, NewTag);
#endif

		if (!bRenameOk)
		{
			// Fallback: ensure new tag exists at minimum
			if (!TagExistsInINI(NewTag))
			{
				TArray<FString> Expanded = ExpandTagHierarchy(NewTag);
				UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
				FGuid SuspendToken = FGuid::NewGuid();
				TagManager.SuspendEditorRefreshGameplayTagTree(SuspendToken);
				for (const FString& T : Expanded)
				{
					if (!TagExistsInINI(T))
					{
						TagsEditor.AddNewGameplayTagToINI(T, TEXT(""), NAME_None, false, true);
					}
				}
				TagManager.ResumeEditorRefreshGameplayTagTree(SuspendToken);
			}
		}

		// Re-request the new tag now that it exists in the dictionary
		NewGPTag = FGameplayTag::RequestGameplayTag(FName(*NewTag), false);

		// Update tag containers on Blueprint CDOs
		for (UBlueprint* BP : BlueprintsToUpdate)
		{
			bool bModified = false;
			UObject* CDO = BP->GeneratedClass ? BP->GeneratedClass->GetDefaultObject() : nullptr;
			if (!CDO) continue;

			// Scan all FGameplayTagContainer properties via reflection
			for (TFieldIterator<FProperty> PropIt(CDO->GetClass()); PropIt; ++PropIt)
			{
				FStructProperty* StructProp = CastField<FStructProperty>(*PropIt);
				if (!StructProp) continue;

				if (StructProp->Struct == FGameplayTagContainer::StaticStruct())
				{
					FGameplayTagContainer* Container = StructProp->ContainerPtrToValuePtr<FGameplayTagContainer>(CDO);
					if (Container && OldGPTag.IsValid() && Container->HasTag(OldGPTag))
					{
						Container->RemoveTag(OldGPTag);
						if (NewGPTag.IsValid())
						{
							Container->AddTag(NewGPTag);
						}
						bModified = true;
					}
				}
				else if (StructProp->Struct == FGameplayTag::StaticStruct())
				{
					FGameplayTag* Tag = StructProp->ContainerPtrToValuePtr<FGameplayTag>(CDO);
					if (Tag && OldGPTag.IsValid() && *Tag == OldGPTag)
					{
						if (NewGPTag.IsValid())
						{
							*Tag = NewGPTag;
						}
						bModified = true;
					}
				}
			}

			if (bModified)
			{
				FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
				UpdatedCount++;
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("old_tag"), OldTag);
	Result->SetStringField(TEXT("new_tag"), NewTag);
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetNumberField(TEXT("affected_asset_count"), AffectedAssets.Num());
	Result->SetArrayField(TEXT("affected_assets"), AffectedAssets);
	if (!bDryRun)
	{
		Result->SetNumberField(TEXT("updated_blueprints"), UpdatedCount);
	}

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// remove_gameplay_tags
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASTagActions::HandleRemoveGameplayTags(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> Tags = MonolithGAS::ParseStringArray(Params, TEXT("tags"));
	if (Tags.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: tags"));
	}

	bool bCheckReferences = true;
	if (Params->HasField(TEXT("check_references")))
	{
		bCheckReferences = Params->GetBoolField(TEXT("check_references"));
	}

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FString> Removed;
	TArray<FString> Skipped;
	TArray<TSharedPtr<FJsonValue>> Blocked; // tags blocked due to references

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();

	for (const FString& TagStr : Tags)
	{
		// Check if tag exists in the dictionary
		if (!TagExistsInINI(TagStr))
		{
			Skipped.Add(TagStr);
			continue;
		}

		// Check references if requested
		if (bCheckReferences)
		{
			TArray<FAssetData> AllAssets;
			AssetRegistry.GetAllAssets(AllAssets, true);

			TArray<FString> ReferencingAssets;
			for (const FAssetData& Asset : AllAssets)
			{
				bool bFound = false;
				Asset.TagsAndValues.ForEach([&](TPair<FName, FAssetTagValueRef> TagPair)
				{
					if (bFound) return;
					FString Value = TagPair.Value.GetValue();
					if (Value.Contains(TEXT("\"") + TagStr + TEXT("\"")) ||
						Value.Contains(TEXT("(TagName=\"") + TagStr + TEXT("\")")) ||
						Value.Equals(TagStr))
					{
						bFound = true;
					}
				});

				if (bFound)
				{
					ReferencingAssets.Add(Asset.AssetName.ToString());
				}
			}

			if (ReferencingAssets.Num() > 0)
			{
				TSharedPtr<FJsonObject> BlockedEntry = MakeShared<FJsonObject>();
				BlockedEntry->SetStringField(TEXT("tag"), TagStr);
				BlockedEntry->SetNumberField(TEXT("reference_count"), ReferencingAssets.Num());
				TArray<TSharedPtr<FJsonValue>> RefArr;
				for (const FString& Ref : ReferencingAssets)
				{
					RefArr.Add(MakeShared<FJsonValueString>(Ref));
				}
				BlockedEntry->SetArrayField(TEXT("referencing_assets"), RefArr);
				Blocked.Add(MakeShared<FJsonValueObject>(BlockedEntry));
				continue;
			}
		}

		// Safe to remove — use engine API via tag node
		Removed.Add(TagStr);
	}

	if (Removed.Num() > 0)
	{
		IGameplayTagsEditorModule& TagsEditor = IGameplayTagsEditorModule::Get();

		// Collect tag nodes for batch deletion
		TArray<TSharedPtr<FGameplayTagNode>> NodesToDelete;
		for (const FString& TagStr : Removed)
		{
			TSharedPtr<FGameplayTagNode> Node = TagManager.FindTagNode(FName(*TagStr));
			if (Node.IsValid())
			{
				NodesToDelete.Add(Node);
			}
		}

		if (NodesToDelete.Num() > 0)
		{
			TagsEditor.DeleteTagsFromINI(NodesToDelete);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("removed_count"), Removed.Num());
	Result->SetNumberField(TEXT("skipped_count"), Skipped.Num());
	Result->SetNumberField(TEXT("blocked_count"), Blocked.Num());

	TArray<TSharedPtr<FJsonValue>> RemovedArr;
	for (const FString& T : Removed) RemovedArr.Add(MakeShared<FJsonValueString>(T));
	Result->SetArrayField(TEXT("removed"), RemovedArr);

	TArray<TSharedPtr<FJsonValue>> SkippedArr;
	for (const FString& T : Skipped) SkippedArr.Add(MakeShared<FJsonValueString>(T));
	Result->SetArrayField(TEXT("skipped"), SkippedArr);

	if (Blocked.Num() > 0)
	{
		Result->SetArrayField(TEXT("blocked"), Blocked);
	}

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// validate_tag_consistency
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASTagActions::HandleValidateTagConsistency(const TSharedPtr<FJsonObject>& Params)
{
	FString PathFilter = Params->GetStringField(TEXT("path_filter"));

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<TSharedPtr<FJsonValue>> Issues;

	auto AddIssue = [&](const FString& Severity, const FString& Category, const FString& Message, const FString& Asset = TEXT(""))
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), Severity);
		Issue->SetStringField(TEXT("category"), Category);
		Issue->SetStringField(TEXT("message"), Message);
		if (!Asset.IsEmpty())
		{
			Issue->SetStringField(TEXT("asset"), Asset);
		}
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	};

	// Collect all registered tags
	FGameplayTagContainer AllRegisteredTags;
	TagManager.RequestAllGameplayTags(AllRegisteredTags, false);
	TSet<FString> RegisteredTagSet;
	for (const FGameplayTag& Tag : AllRegisteredTags)
	{
		RegisteredTagSet.Add(Tag.ToString());
	}

	// Scan all Blueprint assets for tag references
	TArray<FAssetData> AllBPs;
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	if (!PathFilter.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PathFilter));
		Filter.bRecursivePaths = true;
	}
	AssetRegistry.GetAssets(Filter, AllBPs);

	int32 AbilitiesChecked = 0;
	int32 EffectsChecked = 0;

	for (const FAssetData& AssetData : AllBPs)
	{
		// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
		FAssetTagValueRef ParentTag = AssetData.TagsAndValues.FindTag(FName("ParentClass"));
		FAssetTagValueRef NativeParentTag = AssetData.TagsAndValues.FindTag(FName("NativeParentClass"));
		FString ParentPath = ParentTag.IsSet() ? ParentTag.GetValue() : (NativeParentTag.IsSet() ? NativeParentTag.GetValue() : TEXT(""));
		// Only load GAS-relevant Blueprints (abilities, effects)
		if (!ParentPath.Contains(TEXT("GameplayAbility")) && !ParentPath.Contains(TEXT("GameplayEffect")))
		{
			continue;
		}

		UObject* Obj = AssetData.GetAsset();
		UBlueprint* BP = Cast<UBlueprint>(Obj);
		if (!BP || !BP->GeneratedClass) continue;

		UObject* CDO = BP->GeneratedClass->GetDefaultObject();
		if (!CDO) continue;

		FString AssetName = AssetData.AssetName.ToString();

		// Check GameplayAbility CDOs for contradictory tags
		if (BP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
		{
			AbilitiesChecked++;
			UGameplayAbility* AbilityCDO = Cast<UGameplayAbility>(CDO);
			if (!AbilityCDO) continue;

			// Get tag containers via reflection
			FGameplayTagContainer* ActivationRequired = nullptr;
			FGameplayTagContainer* ActivationBlocked = nullptr;

			FProperty* ReqProp = CDO->GetClass()->FindPropertyByName(TEXT("ActivationRequiredTags"));
			FProperty* BlockProp = CDO->GetClass()->FindPropertyByName(TEXT("ActivationBlockedTags"));

			if (ReqProp)
			{
				ActivationRequired = ReqProp->ContainerPtrToValuePtr<FGameplayTagContainer>(CDO);
			}
			if (BlockProp)
			{
				ActivationBlocked = BlockProp->ContainerPtrToValuePtr<FGameplayTagContainer>(CDO);
			}

			// Check for contradictory tags (same tag in both Required and Blocked)
			if (ActivationRequired && ActivationBlocked)
			{
				for (const FGameplayTag& ReqTag : *ActivationRequired)
				{
					if (ActivationBlocked->HasTag(ReqTag))
					{
						AddIssue(TEXT("error"), TEXT("contradictory_tags"),
							FString::Printf(TEXT("Tag '%s' is both required and blocked for activation"),
								*ReqTag.ToString()),
							AssetName);
					}
				}
			}

			// Check for undefined tag references in all tag containers
			for (TFieldIterator<FProperty> PropIt(CDO->GetClass()); PropIt; ++PropIt)
			{
				FStructProperty* StructProp = CastField<FStructProperty>(*PropIt);
				if (!StructProp) continue;

				if (StructProp->Struct == FGameplayTagContainer::StaticStruct())
				{
					FGameplayTagContainer* Container = StructProp->ContainerPtrToValuePtr<FGameplayTagContainer>(CDO);
					if (!Container) continue;

					for (const FGameplayTag& Tag : *Container)
					{
						if (!RegisteredTagSet.Contains(Tag.ToString()))
						{
							AddIssue(TEXT("warning"), TEXT("undefined_tag_reference"),
								FString::Printf(TEXT("References unregistered tag '%s' in property '%s'"),
									*Tag.ToString(), *PropIt->GetName()),
								AssetName);
						}
					}
				}
				else if (StructProp->Struct == FGameplayTag::StaticStruct())
				{
					FGameplayTag* Tag = StructProp->ContainerPtrToValuePtr<FGameplayTag>(CDO);
					if (Tag && Tag->IsValid() && !RegisteredTagSet.Contains(Tag->ToString()))
					{
						AddIssue(TEXT("warning"), TEXT("undefined_tag_reference"),
							FString::Printf(TEXT("References unregistered tag '%s' in property '%s'"),
								*Tag->ToString(), *PropIt->GetName()),
							AssetName);
					}
				}
			}
		}

		// Check GameplayEffect CDOs for undefined tags
		if (BP->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass()))
		{
			EffectsChecked++;

			for (TFieldIterator<FProperty> PropIt(CDO->GetClass()); PropIt; ++PropIt)
			{
				FStructProperty* StructProp = CastField<FStructProperty>(*PropIt);
				if (!StructProp) continue;

				if (StructProp->Struct == FGameplayTagContainer::StaticStruct())
				{
					FGameplayTagContainer* Container = StructProp->ContainerPtrToValuePtr<FGameplayTagContainer>(CDO);
					if (!Container) continue;

					for (const FGameplayTag& Tag : *Container)
					{
						if (!RegisteredTagSet.Contains(Tag.ToString()))
						{
							AddIssue(TEXT("warning"), TEXT("undefined_tag_reference"),
								FString::Printf(TEXT("GE references unregistered tag '%s' in '%s'"),
									*Tag.ToString(), *PropIt->GetName()),
								AssetName);
						}
					}
				}
			}
		}
	}

	// Check for orphan tags (registered tags with no references)
	// Only check leaf tags (tags with no children) to avoid false positives on parent hierarchy
	TSet<FString> ReferencedTags;
	TArray<FAssetData> AllAssets;
	AssetRegistry.GetAllAssets(AllAssets, true);
	for (const FAssetData& Asset : AllAssets)
	{
		Asset.TagsAndValues.ForEach([&](TPair<FName, FAssetTagValueRef> TagPair)
		{
			FString Value = TagPair.Value.GetValue();
			for (const FString& RegTag : RegisteredTagSet)
			{
				if (Value.Contains(RegTag))
				{
					ReferencedTags.Add(RegTag);
				}
			}
		});
	}

	int32 OrphanCount = 0;
	for (const FString& RegTag : RegisteredTagSet)
	{
		// Only flag leaf tags (no children)
		FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*RegTag), false);
		if (!Tag.IsValid()) continue;

		FGameplayTagContainer Children = TagManager.RequestGameplayTagChildren(Tag);
		bool bIsLeaf = true;
		for (const FGameplayTag& ChildTag : Children)
		{
			FString ChildStr = ChildTag.ToString();
			if (ChildStr.StartsWith(RegTag + TEXT(".")))
			{
				FString Remainder = ChildStr.Mid(RegTag.Len() + 1);
				if (!Remainder.Contains(TEXT(".")))
				{
					bIsLeaf = false;
					break;
				}
			}
		}

		if (bIsLeaf && !ReferencedTags.Contains(RegTag))
		{
			OrphanCount++;
			// Only report first 50 orphans to avoid huge output
			if (OrphanCount <= 50)
			{
				AddIssue(TEXT("info"), TEXT("orphan_tag"),
					FString::Printf(TEXT("Tag '%s' is registered but not referenced by any asset"), *RegTag));
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("total_registered_tags"), RegisteredTagSet.Num());
	Result->SetNumberField(TEXT("abilities_checked"), AbilitiesChecked);
	Result->SetNumberField(TEXT("effects_checked"), EffectsChecked);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());
	Result->SetNumberField(TEXT("orphan_tag_count"), OrphanCount);
	Result->SetArrayField(TEXT("issues"), Issues);

	if (!PathFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("path_filter"), PathFilter);
	}

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 3: audit_tag_naming
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASTagActions::HandleAuditTagNaming(const TSharedPtr<FJsonObject>& Params)
{
	// Parse conventions (optional overrides)
	int32 MaxDepth = 8;
	bool bAllowUnderscores = false;

	TSet<FString> RequiredPrefixes = {
		TEXT("Ability"), TEXT("State"), TEXT("Damage"), TEXT("Status"), TEXT("Cooldown"),
		TEXT("GameplayCue"), TEXT("SetByCaller"), TEXT("Event"), TEXT("Director")
	};

	const TSharedPtr<FJsonObject>* ConventionsObj;
	if (Params->TryGetObjectField(TEXT("conventions"), ConventionsObj))
	{
		if ((*ConventionsObj)->HasField(TEXT("max_depth")))
		{
			MaxDepth = static_cast<int32>((*ConventionsObj)->GetNumberField(TEXT("max_depth")));
		}
		if ((*ConventionsObj)->HasField(TEXT("allow_underscores")))
		{
			bAllowUnderscores = (*ConventionsObj)->GetBoolField(TEXT("allow_underscores"));
		}
		const TArray<TSharedPtr<FJsonValue>>* PrefixArray;
		if ((*ConventionsObj)->TryGetArrayField(TEXT("required_prefixes"), PrefixArray))
		{
			RequiredPrefixes.Empty();
			for (const auto& Val : *PrefixArray)
			{
				FString S;
				if (Val->TryGetString(S)) RequiredPrefixes.Add(S);
			}
		}
	}

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
	FGameplayTagContainer AllTags;
	TagManager.RequestAllGameplayTags(AllTags, false);

	TArray<TSharedPtr<FJsonValue>> Issues;

	auto AddIssue = [&](const FString& Severity, const FString& Tag, const FString& Rule, const FString& Message)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), Severity);
		Issue->SetStringField(TEXT("tag"), Tag);
		Issue->SetStringField(TEXT("rule"), Rule);
		Issue->SetStringField(TEXT("message"), Message);
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	};

	int32 TotalChecked = 0;
	TSet<FString> UnknownPrefixes;

	for (const FGameplayTag& Tag : AllTags)
	{
		FString TagStr = Tag.ToString();
		TotalChecked++;

		TArray<FString> Parts;
		TagStr.ParseIntoArray(Parts, TEXT("."));

		// 1. Check depth
		if (Parts.Num() > MaxDepth)
		{
			AddIssue(TEXT("warning"), TagStr, TEXT("max_depth"),
				FString::Printf(TEXT("Tag depth %d exceeds max %d"), Parts.Num(), MaxDepth));
		}

		// 2. Check PascalCase on each segment
		for (const FString& Part : Parts)
		{
			if (Part.IsEmpty()) continue;

			// First character should be uppercase
			if (!FChar::IsUpper(Part[0]))
			{
				AddIssue(TEXT("warning"), TagStr, TEXT("pascal_case"),
					FString::Printf(TEXT("Segment '%s' does not start with uppercase (PascalCase expected)"), *Part));
			}

			// Check for underscores
			if (!bAllowUnderscores && Part.Contains(TEXT("_")))
			{
				AddIssue(TEXT("warning"), TagStr, TEXT("no_underscores"),
					FString::Printf(TEXT("Segment '%s' contains underscore — use PascalCase instead"), *Part));
			}
		}

		// 3. Check recognized prefix
		if (Parts.Num() > 0 && !RequiredPrefixes.Contains(Parts[0]))
		{
			UnknownPrefixes.Add(Parts[0]);
		}
	}

	// Report unknown prefixes as info (not necessarily wrong, just flagged)
	for (const FString& Prefix : UnknownPrefixes)
	{
		AddIssue(TEXT("info"), Prefix, TEXT("unrecognized_prefix"),
			FString::Printf(TEXT("'%s' is not in the recognized prefix set"), *Prefix));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("total_tags_checked"), TotalChecked);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());
	Result->SetNumberField(TEXT("max_depth_setting"), MaxDepth);
	Result->SetBoolField(TEXT("allow_underscores"), bAllowUnderscores);
	Result->SetArrayField(TEXT("issues"), Issues);

	TArray<TSharedPtr<FJsonValue>> PrefixArr;
	for (const FString& P : RequiredPrefixes) PrefixArr.Add(MakeShared<FJsonValueString>(P));
	Result->SetArrayField(TEXT("recognized_prefixes"), PrefixArr);

	TArray<TSharedPtr<FJsonValue>> UnkArr;
	for (const FString& P : UnknownPrefixes) UnkArr.Add(MakeShared<FJsonValueString>(P));
	Result->SetArrayField(TEXT("unrecognized_prefixes"), UnkArr);

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 3: export_tag_hierarchy
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASTagActions::HandleExportTagHierarchy(const TSharedPtr<FJsonObject>& Params)
{
	FString Format = Params->GetStringField(TEXT("format"));
	if (Format.IsEmpty()) Format = TEXT("json");
	FString OutputPath = Params->GetStringField(TEXT("output_path"));

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
	FGameplayTagContainer AllTags;
	TagManager.RequestAllGameplayTags(AllTags, false);

	// Sort tags for deterministic output
	TArray<FString> SortedTags;
	for (const FGameplayTag& Tag : AllTags)
	{
		SortedTags.Add(Tag.ToString());
	}
	SortedTags.Sort();

	FString Output;

	if (Format.Equals(TEXT("json"), ESearchCase::IgnoreCase))
	{
		// JSON array of strings
		TArray<TSharedPtr<FJsonValue>> TagArray;
		for (const FString& T : SortedTags)
		{
			TagArray.Add(MakeShared<FJsonValueString>(T));
		}
		TSharedRef<FJsonValueArray> JsonArr = MakeShared<FJsonValueArray>(TagArray);

		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(TagArray, Writer);
	}
	else if (Format.Equals(TEXT("csv"), ESearchCase::IgnoreCase))
	{
		Output = TEXT("Tag,Depth,Prefix\n");
		for (const FString& T : SortedTags)
		{
			TArray<FString> Parts;
			T.ParseIntoArray(Parts, TEXT("."));
			FString Prefix = Parts.Num() > 0 ? Parts[0] : TEXT("");
			Output += FString::Printf(TEXT("%s,%d,%s\n"), *T, Parts.Num(), *Prefix);
		}
	}
	else if (Format.Equals(TEXT("text"), ESearchCase::IgnoreCase))
	{
		for (const FString& T : SortedTags)
		{
			// Indent based on depth
			TArray<FString> Parts;
			T.ParseIntoArray(Parts, TEXT("."));
			FString Indent;
			for (int32 i = 0; i < Parts.Num() - 1; ++i) Indent += TEXT("  ");
			Output += Indent + Parts.Last() + TEXT("\n");
		}
	}
	else
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid format: '%s'. Valid: json, csv, text"), *Format));
	}

	// Write to file if path provided
	bool bWritten = false;
	if (!OutputPath.IsEmpty())
	{
		bWritten = FFileHelper::SaveStringToFile(Output, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (!bWritten)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to write to: %s"), *OutputPath));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("format"), Format);
	Result->SetNumberField(TEXT("tag_count"), SortedTags.Num());

	if (!OutputPath.IsEmpty())
	{
		Result->SetStringField(TEXT("output_path"), OutputPath);
		Result->SetBoolField(TEXT("written"), bWritten);
	}
	else
	{
		// Return inline (truncate if massive)
		if (Output.Len() > 50000)
		{
			Output = Output.Left(50000) + TEXT("\n... (truncated)");
		}
		Result->SetStringField(TEXT("content"), Output);
	}

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 3: import_tag_hierarchy
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASTagActions::HandleImportTagHierarchy(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("source_path"), SourcePath, Err)) return Err;

	FString MergeMode = Params->GetStringField(TEXT("merge_mode"));
	if (MergeMode.IsEmpty()) MergeMode = TEXT("merge");

	bool bDryRun = false;
	if (Params->HasField(TEXT("dry_run")))
	{
		bDryRun = Params->GetBoolField(TEXT("dry_run"));
	}

	// Read the file
	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *SourcePath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to read file: %s"), *SourcePath));
	}

	// Parse tags from file
	TArray<FString> ImportedTags;

	// Try JSON first
	TSharedPtr<FJsonValue> JsonRoot;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
	if (FJsonSerializer::Deserialize(Reader, JsonRoot) && JsonRoot.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* JsonArr;
		if (JsonRoot->TryGetArray(JsonArr))
		{
			for (const auto& Val : *JsonArr)
			{
				FString S;
				if (Val->TryGetString(S) && !S.IsEmpty())
				{
					ImportedTags.Add(S);
				}
			}
		}
	}

	// If JSON parsing failed or returned nothing, try CSV/text
	if (ImportedTags.Num() == 0)
	{
		TArray<FString> Lines;
		FileContent.ParseIntoArrayLines(Lines);
		for (FString& Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (Line.IsEmpty() || Line.StartsWith(TEXT("#")) || Line.StartsWith(TEXT("//"))) continue;
			// CSV: take first column
			if (Line.Contains(TEXT(",")))
			{
				FString Tag;
				Line.Split(TEXT(","), &Tag, nullptr);
				Tag.TrimStartAndEndInline();
				// Skip header row
				if (Tag.Equals(TEXT("Tag"), ESearchCase::IgnoreCase)) continue;
				if (!Tag.IsEmpty()) ImportedTags.Add(Tag);
			}
			else
			{
				ImportedTags.Add(Line);
			}
		}
	}

	if (ImportedTags.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No tags found in source file"));
	}

	// Expand hierarchy for all imported tags
	TSet<FString> AllImportedTags;
	for (const FString& Tag : ImportedTags)
	{
		TArray<FString> Expanded = ExpandTagHierarchy(Tag);
		for (const FString& T : Expanded)
		{
			AllImportedTags.Add(T);
		}
	}

	// Build existing tag set from the dictionary
	TSet<FString> ExistingTagSet;
	{
		UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
		FGameplayTagContainer AllTags;
		TagManager.RequestAllGameplayTags(AllTags, true);
		for (const FGameplayTag& Tag : AllTags)
		{
			ExistingTagSet.Add(Tag.ToString());
		}
	}

	TArray<FString> ToAdd;
	TArray<FString> ToRemove;
	TArray<FString> AlreadyExists;

	if (MergeMode.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
	{
		// Replace: remove all existing, add all imported
		for (const FString& T : ExistingTagSet)
		{
			if (!AllImportedTags.Contains(T))
			{
				ToRemove.Add(T);
			}
		}
		for (const FString& T : AllImportedTags)
		{
			if (!ExistingTagSet.Contains(T))
			{
				ToAdd.Add(T);
			}
			else
			{
				AlreadyExists.Add(T);
			}
		}
	}
	else // merge
	{
		for (const FString& T : AllImportedTags)
		{
			if (!ExistingTagSet.Contains(T))
			{
				ToAdd.Add(T);
			}
			else
			{
				AlreadyExists.Add(T);
			}
		}
	}

	ToAdd.Sort();
	ToRemove.Sort();

	if (!bDryRun)
	{
		IGameplayTagsEditorModule& TagsEditor = IGameplayTagsEditorModule::Get();
		UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
		FGuid SuspendToken = FGuid::NewGuid();
		TagManager.SuspendEditorRefreshGameplayTagTree(SuspendToken);

		// Apply replace removals
		if (MergeMode.Equals(TEXT("replace"), ESearchCase::IgnoreCase) && ToRemove.Num() > 0)
		{
			TArray<TSharedPtr<FGameplayTagNode>> NodesToDelete;
			for (const FString& RemoveTag : ToRemove)
			{
				TSharedPtr<FGameplayTagNode> Node = TagManager.FindTagNode(FName(*RemoveTag));
				if (Node.IsValid())
				{
					NodesToDelete.Add(Node);
				}
			}
			if (NodesToDelete.Num() > 0)
			{
				TagsEditor.DeleteTagsFromINI(NodesToDelete);
			}
		}

		// Apply additions
		for (const FString& T : ToAdd)
		{
			TagsEditor.AddNewGameplayTagToINI(T, TEXT(""), NAME_None, false, true);
		}

		TagManager.ResumeEditorRefreshGameplayTagTree(SuspendToken);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source_path"), SourcePath);
	Result->SetStringField(TEXT("merge_mode"), MergeMode);
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetNumberField(TEXT("imported_tag_count"), ImportedTags.Num());
	Result->SetNumberField(TEXT("expanded_tag_count"), AllImportedTags.Num());
	Result->SetNumberField(TEXT("to_add_count"), ToAdd.Num());
	Result->SetNumberField(TEXT("already_exists_count"), AlreadyExists.Num());

	if (MergeMode.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
	{
		Result->SetNumberField(TEXT("to_remove_count"), ToRemove.Num());
	}

	// Include details for dry run
	if (bDryRun || ToAdd.Num() <= 50)
	{
		TArray<TSharedPtr<FJsonValue>> AddArr;
		for (const FString& T : ToAdd) AddArr.Add(MakeShared<FJsonValueString>(T));
		Result->SetArrayField(TEXT("tags_to_add"), AddArr);
	}

	if (bDryRun && MergeMode.Equals(TEXT("replace"), ESearchCase::IgnoreCase) && ToRemove.Num() <= 50)
	{
		TArray<TSharedPtr<FJsonValue>> RemArr;
		for (const FString& T : ToRemove) RemArr.Add(MakeShared<FJsonValueString>(T));
		Result->SetArrayField(TEXT("tags_to_remove"), RemArr);
	}

	return FMonolithActionResult::Success(Result);
}
