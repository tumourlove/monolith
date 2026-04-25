#include "MonolithGASCueActions.h"
#include "MonolithParamSchema.h"
#include "MonolithGASInternal.h"
#include "MonolithAssetUtils.h"

#include "GameplayCueNotify_Static.h"
#include "GameplayCueNotify_Actor.h"
#include "GameplayEffect.h"
#include "GameplayEffectComponent.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"


// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

void FMonolithGASCueActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("gas"), TEXT("create_gameplay_cue_notify"),
		TEXT("Create a GameplayCue Notify Blueprint (Burst, Looping/Actor, or BurstLatent/Static). Auto-sets the GameplayCue tag on the CDO."),
		FMonolithActionHandler::CreateStatic(&HandleCreateGameplayCueNotify),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path for the new GCN Blueprint (e.g. '/Game/GAS/Cues/GCN_Status_Burning')"))
			.Required(TEXT("cue_tag"), TEXT("string"), TEXT("GameplayCue tag (e.g. 'GameplayCue.Status.Burning')"))
			.Optional(TEXT("type"), TEXT("string"), TEXT("Cue type: 'burst' (UGameplayCueNotify_Static, fire-and-forget), 'looping' (AGameplayCueNotify_Actor, attach to target), 'burst_latent' (UGameplayCueNotify_Static, burst with duration). Default: burst"), TEXT("burst"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("link_cue_to_effect"),
		TEXT("Add a GameplayCue tag to a GameplayEffect so it triggers the cue on application"),
		FMonolithActionHandler::CreateStatic(&HandleLinkCueToEffect),
		FParamSchemaBuilder()
			.Required(TEXT("effect_path"), TEXT("string"), TEXT("GameplayEffect Blueprint asset path"))
			.Required(TEXT("cue_tag"), TEXT("string"), TEXT("GameplayCue tag to add (e.g. 'GameplayCue.Status.Burning')"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("unlink_cue_from_effect"),
		TEXT("Remove a GameplayCue tag from a GameplayEffect"),
		FMonolithActionHandler::CreateStatic(&HandleUnlinkCueFromEffect),
		FParamSchemaBuilder()
			.Required(TEXT("effect_path"), TEXT("string"), TEXT("GameplayEffect Blueprint asset path"))
			.Required(TEXT("cue_tag"), TEXT("string"), TEXT("GameplayCue tag to remove"))
			.Build());

	// ── Phase 3: Cue Productivity ──

	Registry.RegisterAction(TEXT("gas"), TEXT("get_cue_info"),
		TEXT("Read GameplayCue Notify config: tag, type (burst/looping), particles, sounds, camera shakes"),
		FMonolithActionHandler::CreateStatic(&HandleGetCueInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GCN Blueprint or class asset path"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("list_gameplay_cues"),
		TEXT("Find all GameplayCue Notify assets in the project"),
		FMonolithActionHandler::CreateStatic(&HandleListGameplayCues),
		FParamSchemaBuilder()
			.Optional(TEXT("tag_prefix"), TEXT("string"), TEXT("Filter cues by tag prefix (e.g. 'GameplayCue.Status')"))
			.Optional(TEXT("type_filter"), TEXT("string"), TEXT("Filter by type: 'burst', 'looping', or 'all' (default: all)"), TEXT("all"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("set_cue_parameters"),
		TEXT("Configure burst/loop particles, sounds, camera shakes on a GameplayCue Notify via CDO reflection"),
		FMonolithActionHandler::CreateStatic(&HandleSetCueParameters),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("GCN Blueprint asset path"))
			.Optional(TEXT("burst_particle"), TEXT("string"), TEXT("Particle system asset path for burst effect"))
			.Optional(TEXT("burst_sound"), TEXT("string"), TEXT("Sound asset path for burst"))
			.Optional(TEXT("loop_particle"), TEXT("string"), TEXT("Particle system asset path for looping effect"))
			.Optional(TEXT("loop_sound"), TEXT("string"), TEXT("Sound asset path for looping sound"))
			.Optional(TEXT("camera_shake"), TEXT("string"), TEXT("Camera shake class asset path"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("find_cue_triggers"),
		TEXT("Reverse lookup: find all GameplayEffects that trigger a given cue tag"),
		FMonolithActionHandler::CreateStatic(&HandleFindCueTriggers),
		FParamSchemaBuilder()
			.Required(TEXT("cue_tag"), TEXT("string"), TEXT("GameplayCue tag to search for (e.g. 'GameplayCue.Status.Burning')"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("validate_cue_coverage"),
		TEXT("Find GEs with cue tags that lack matching cue handlers, and orphaned cues with no GE trigger"),
		FMonolithActionHandler::CreateStatic(&HandleValidateCueCoverage),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Restrict scan to assets under this path"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("batch_create_cues"),
		TEXT("Create multiple GameplayCue Notify Blueprints in one call"),
		FMonolithActionHandler::CreateStatic(&HandleBatchCreateCues),
		FParamSchemaBuilder()
			.Required(TEXT("cues"), TEXT("array"), TEXT("Array of objects with: save_path, cue_tag, type? (burst/looping/burst_latent)"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("scaffold_cue_library"),
		TEXT("Generate a full set of GameplayCue Notifies from a preset (combat, status, horror)"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldCueLibrary),
		FParamSchemaBuilder()
			.Required(TEXT("preset"), TEXT("string"), TEXT("Preset name: 'combat', 'status', 'horror', or 'all'"))
			.Required(TEXT("save_path_prefix"), TEXT("string"), TEXT("Base path for generated cues (e.g. '/Game/GAS/Cues')"))
			.Build());
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

/** Load a GE Blueprint (wraps MonolithGAS::LoadGameplayEffectBP). */
bool LoadGEForCue(
	const FString& EffectPath,
	UBlueprint*& OutBP,
	UGameplayEffect*& OutGE,
	FMonolithActionResult& OutError)
{
	FString Error;
	if (!MonolithGAS::LoadGameplayEffectBP(EffectPath, OutBP, OutGE, Error))
	{
		OutError = FMonolithActionResult::Error(Error);
		return false;
	}
	return true;
}

/** Access the GameplayCueTags on a GE CDO via reflection (protected member). */
FInheritedTagContainer* GetGameplayCueTags(UGameplayEffect* GE)
{
	if (!GE) return nullptr;
	FProperty* Prop = UGameplayEffect::StaticClass()->FindPropertyByName(TEXT("GameplayCues"));
	if (Prop)
	{
		return Prop->ContainerPtrToValuePtr<FInheritedTagContainer>(GE);
	}
	return nullptr;
}

/** Get the cue tags as a plain container, checking both the GEComponent and legacy property. */
FGameplayTagContainer GetCueTagContainer(UGameplayEffect* GE)
{
	FGameplayTagContainer Result;
	if (!GE) return Result;

	// Try the GEComponent approach first (5.3+)
	TArray<UObject*> Subobjects;
	GE->GetDefaultSubobjects(Subobjects);
	for (UObject* Sub : Subobjects)
	{
		if (UAssetTagsGameplayEffectComponent* AssetTags = Cast<UAssetTagsGameplayEffectComponent>(Sub))
		{
			FInheritedTagContainer TagChanges = AssetTags->GetConfiguredAssetTagChanges();
			for (const FGameplayTag& Tag : TagChanges.Added)
			{
				if (Tag.ToString().StartsWith(TEXT("GameplayCue.")))
				{
					Result.AddTag(Tag);
				}
			}
		}
	}

	// Also check legacy GameplayCues property via reflection
	FInheritedTagContainer* CueTags = GetGameplayCueTags(GE);
	if (CueTags)
	{
		Result.AppendTags(CueTags->Added);
	}

	return Result;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// create_gameplay_cue_notify
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASCueActions::HandleCreateGameplayCueNotify(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, CueTag;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("save_path"), SavePath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("cue_tag"), CueTag, Err)) return Err;

	FString TypeStr = Params->GetStringField(TEXT("type"));
	if (TypeStr.IsEmpty()) TypeStr = TEXT("burst");

	// Validate cue tag starts with GameplayCue.
	if (!CueTag.StartsWith(TEXT("GameplayCue.")))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Cue tag must start with 'GameplayCue.' — got: '%s'"), *CueTag));
	}

	// Determine parent class based on type
	UClass* ParentClass = nullptr;
	FString TypeDisplay;

	if (TypeStr.Equals(TEXT("burst"), ESearchCase::IgnoreCase) ||
		TypeStr.Equals(TEXT("burst_latent"), ESearchCase::IgnoreCase))
	{
		// Static notify: UGameplayCueNotify_Static — not an actor, fire-and-forget
		// Note: Despite the name, this IS still a UObject (not an AActor), it's lighter weight
		ParentClass = UGameplayCueNotify_Static::StaticClass();
		TypeDisplay = TypeStr.Equals(TEXT("burst"), ESearchCase::IgnoreCase) ? TEXT("burst") : TEXT("burst_latent");
	}
	else if (TypeStr.Equals(TEXT("looping"), ESearchCase::IgnoreCase))
	{
		// Actor notify: AGameplayCueNotify_Actor — spawned into the world, attaches to target
		ParentClass = AGameplayCueNotify_Actor::StaticClass();
		TypeDisplay = TEXT("looping");
	}
	else
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid cue type: '%s'. Valid: burst, looping, burst_latent"), *TypeStr));
	}

	// Extract asset name from path
	int32 LastSlash;
	if (!SavePath.FindLastChar(TEXT('/'), LastSlash))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid save_path: %s"), *SavePath));
	}
	FString AssetName = SavePath.Mid(LastSlash + 1);
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("save_path must not end with '/'"));
	}

	// Check for existing asset (AssetRegistry + in-memory multi-tier check)
	FString ExistError;
	if (!MonolithGAS::EnsureAssetPathFree(SavePath, AssetName, ExistError))
	{
		return FMonolithActionResult::Error(ExistError);
	}

	// Create package
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *SavePath));
	}
	Package->FullyLoad();

	// Create Blueprint
	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());

	if (!NewBP)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to create GameplayCue Blueprint at: %s"), *SavePath));
	}

	// Compile to ensure GeneratedClass is valid
	FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::SkipGarbageCollection);

	// Set the GameplayCue tag on the CDO
	// Both UGameplayCueNotify_Static and AGameplayCueNotify_Actor have a GameplayCueTag property
	UObject* CDO = NewBP->GeneratedClass ? NewBP->GeneratedClass->GetDefaultObject() : nullptr;
	bool bTagSet = false;
	if (CDO)
	{
		// Set via reflection — the property is FGameplayTag GameplayCueTag
		FProperty* CueTagProp = CDO->GetClass()->FindPropertyByName(TEXT("GameplayCueTag"));
		if (CueTagProp)
		{
			// Ensure the tag exists (request it, creating if needed)
			FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*CueTag), false);
			if (Tag.IsValid())
			{
				FGameplayTag* TagPtr = CueTagProp->ContainerPtrToValuePtr<FGameplayTag>(CDO);
				if (TagPtr)
				{
					*TagPtr = Tag;
					bTagSet = true;
				}
			}
			else
			{
				UE_LOG(LogMonolithGAS, Warning,
					TEXT("GameplayCue tag '%s' not registered. Create it via add_gameplay_tags first."), *CueTag);
			}
		}
	}

	// Recompile after CDO changes
	FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::SkipGarbageCollection);

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewBP);

	// Save to disk
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	bool bSaved = UPackage::SavePackage(Package, NewBP, *PackageFilename, SaveArgs);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetStringField(TEXT("cue_tag"), CueTag);
	Result->SetStringField(TEXT("type"), TypeDisplay);
	Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	Result->SetBoolField(TEXT("tag_set_on_cdo"), bTagSet);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Created %s GameplayCue '%s' for tag '%s'"),
			*TypeDisplay, *AssetName, *CueTag));

	if (!bTagSet)
	{
		Result->SetStringField(TEXT("warning"),
			FString::Printf(TEXT("Tag '%s' is not registered. Add it via add_gameplay_tags or scaffold_tag_hierarchy."), *CueTag));
	}

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// link_cue_to_effect
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASCueActions::HandleLinkCueToEffect(const TSharedPtr<FJsonObject>& Params)
{
	FString EffectPath, CueTag;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("effect_path"), EffectPath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("cue_tag"), CueTag, Err)) return Err;

	if (!CueTag.StartsWith(TEXT("GameplayCue.")))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Cue tag must start with 'GameplayCue.' — got: '%s'"), *CueTag));
	}

	FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*CueTag), false);
	if (!Tag.IsValid())
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Tag '%s' is not registered. Add it via add_gameplay_tags first."), *CueTag));
	}

	UBlueprint* BP = nullptr;
	UGameplayEffect* GE = nullptr;
	if (!LoadGEForCue(EffectPath, BP, GE, Err)) return Err;

	// Check if the tag is already on the GE
	FGameplayTagContainer ExistingCues = GetCueTagContainer(GE);
	if (ExistingCues.HasTagExact(Tag))
	{
		TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(EffectPath,
			FString::Printf(TEXT("Cue tag '%s' already present on effect"), *CueTag));
		Result->SetBoolField(TEXT("already_present"), true);
		return FMonolithActionResult::Success(Result);
	}

	// Add the tag via the GameplayCues property on the GE CDO (via reflection)
	// The GE uses FInheritedTagContainer GameplayCues which has .Added / .Removed
	FInheritedTagContainer* CueTags = GetGameplayCueTags(GE);
	if (CueTags)
	{
		CueTags->Added.AddTag(Tag);
	}
	else
	{
		// Fallback: try to set via the raw GameplayCues property
		UE_LOG(LogMonolithGAS, Warning, TEXT("Could not find GameplayCues property on GE CDO"));
		return FMonolithActionResult::Error(TEXT("Failed to access GameplayCues property on GameplayEffect CDO"));
	}

	BP->Modify();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	// Note: Do NOT compile here — GE CDO changes don't require recompilation
	// and CompileBlueprint can trigger tag container processing that crashes

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(EffectPath,
		FString::Printf(TEXT("Linked cue tag '%s' to effect '%s'"), *CueTag, *EffectPath));
	Result->SetStringField(TEXT("cue_tag"), CueTag);
	Result->SetBoolField(TEXT("already_present"), false);

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// unlink_cue_from_effect
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASCueActions::HandleUnlinkCueFromEffect(const TSharedPtr<FJsonObject>& Params)
{
	FString EffectPath, CueTag;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("effect_path"), EffectPath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("cue_tag"), CueTag, Err)) return Err;

	FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*CueTag), false);
	if (!Tag.IsValid())
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Tag '%s' is not registered"), *CueTag));
	}

	UBlueprint* BP = nullptr;
	UGameplayEffect* GE = nullptr;
	if (!LoadGEForCue(EffectPath, BP, GE, Err)) return Err;

	// Check if the tag exists on the GE
	FInheritedTagContainer* CueTags = GetGameplayCueTags(GE);
	if (!CueTags || !CueTags->Added.HasTagExact(Tag))
	{
		TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(EffectPath,
			FString::Printf(TEXT("Cue tag '%s' not found on effect"), *CueTag));
		Result->SetBoolField(TEXT("was_present"), false);
		return FMonolithActionResult::Success(Result);
	}

	CueTags->Added.RemoveTag(Tag);

	BP->Modify();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(EffectPath,
		FString::Printf(TEXT("Unlinked cue tag '%s' from effect '%s'"), *CueTag, *EffectPath));
	Result->SetStringField(TEXT("cue_tag"), CueTag);
	Result->SetBoolField(TEXT("was_present"), true);

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 3: get_cue_info
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASCueActions::HandleGetCueInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, Err)) return Err;

	FString Error;
	UObject* Obj = MonolithGAS::LoadAssetFromPath(AssetPath, Error);
	UBlueprint* BP = Cast<UBlueprint>(Obj);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load GCN Blueprint: %s — %s"), *AssetPath, *Error));
	}

	UObject* CDO = (BP->GeneratedClass) ? BP->GeneratedClass->GetDefaultObject() : nullptr;
	if (!CDO)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to get CDO for: %s"), *AssetPath));
	}

	bool bIsStatic = CDO->GetClass()->IsChildOf(UGameplayCueNotify_Static::StaticClass());
	bool bIsActor = CDO->GetClass()->IsChildOf(AGameplayCueNotify_Actor::StaticClass());

	if (!bIsStatic && !bIsActor)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("'%s' is not a GameplayCue Notify"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("type"), bIsActor ? TEXT("looping") : TEXT("burst"));
	Result->SetStringField(TEXT("parent_class"), CDO->GetClass()->GetSuperClass()->GetName());

	// Read GameplayCueTag via reflection
	FProperty* CueTagProp = CDO->GetClass()->FindPropertyByName(TEXT("GameplayCueTag"));
	if (CueTagProp)
	{
		FGameplayTag* TagPtr = CueTagProp->ContainerPtrToValuePtr<FGameplayTag>(CDO);
		if (TagPtr && TagPtr->IsValid())
		{
			Result->SetStringField(TEXT("cue_tag"), TagPtr->ToString());
		}
		else
		{
			Result->SetStringField(TEXT("cue_tag"), TEXT(""));
		}
	}

	// Collect all relevant CDO properties via reflection for particles, sounds, shakes
	TSharedPtr<FJsonObject> PropertiesObj = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> PropIt(CDO->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		FString PropName = Prop->GetName();

		// Capture object reference properties (particle systems, sounds, camera shakes)
		if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			UObject* RefObj = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(CDO));
			if (RefObj)
			{
				PropertiesObj->SetStringField(PropName, RefObj->GetPathName());
			}
		}
		// Capture soft object references
		else if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
		{
			FSoftObjectPtr* SoftPtr = SoftProp->ContainerPtrToValuePtr<FSoftObjectPtr>(CDO);
			if (SoftPtr && !SoftPtr->IsNull())
			{
				PropertiesObj->SetStringField(PropName, SoftPtr->ToString());
			}
		}
		// Capture float properties (shake scale, duration, etc.)
		else if (PropName.Contains(TEXT("Shake")) || PropName.Contains(TEXT("Duration"))
			|| PropName.Contains(TEXT("Scale")) || PropName.Contains(TEXT("Magnitude")))
		{
			if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
			{
				float Val = FloatProp->GetPropertyValue(FloatProp->ContainerPtrToValuePtr<void>(CDO));
				PropertiesObj->SetNumberField(PropName, Val);
			}
			else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
			{
				double Val = DoubleProp->GetPropertyValue(DoubleProp->ContainerPtrToValuePtr<void>(CDO));
				PropertiesObj->SetNumberField(PropName, Val);
			}
		}
		// Capture bool properties related to cue behavior
		else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			if (PropName.Contains(TEXT("Auto")) || PropName.Contains(TEXT("Destroy"))
				|| PropName.Contains(TEXT("Loop")) || PropName.Contains(TEXT("Attach")))
			{
				bool Val = BoolProp->GetPropertyValue(BoolProp->ContainerPtrToValuePtr<void>(CDO));
				PropertiesObj->SetBoolField(PropName, Val);
			}
		}
	}

	if (PropertiesObj->Values.Num() > 0)
	{
		Result->SetObjectField(TEXT("properties"), PropertiesObj);
	}

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 3: list_gameplay_cues
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASCueActions::HandleListGameplayCues(const TSharedPtr<FJsonObject>& Params)
{
	FString TagPrefix = Params->GetStringField(TEXT("tag_prefix"));
	FString TypeFilter = Params->GetStringField(TEXT("type_filter"));
	if (TypeFilter.IsEmpty()) TypeFilter = TEXT("all");

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Find all Blueprints that inherit from GCN base classes
	TArray<FAssetData> AllBPs;
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	AssetRegistry.GetAssets(Filter, AllBPs);

	TArray<TSharedPtr<FJsonValue>> CueArray;

	for (const FAssetData& AssetData : AllBPs)
	{
		// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
		FAssetTagValueRef ParentTag = AssetData.TagsAndValues.FindTag(FName("ParentClass"));
		FAssetTagValueRef NativeParentTag = AssetData.TagsAndValues.FindTag(FName("NativeParentClass"));
		FString ParentClassPath = ParentTag.IsSet() ? ParentTag.GetValue() : (NativeParentTag.IsSet() ? NativeParentTag.GetValue() : TEXT(""));
		if (!ParentClassPath.Contains(TEXT("GameplayCueNotify")))
		{
			continue;
		}

		UObject* Obj = AssetData.GetAsset();
		UBlueprint* BP = Cast<UBlueprint>(Obj);
		if (!BP || !BP->GeneratedClass) continue;

		UObject* CDO = BP->GeneratedClass->GetDefaultObject();
		if (!CDO) continue;

		bool bIsStatic = CDO->GetClass()->IsChildOf(UGameplayCueNotify_Static::StaticClass());
		bool bIsActor = CDO->GetClass()->IsChildOf(AGameplayCueNotify_Actor::StaticClass());
		if (!bIsStatic && !bIsActor) continue;

		// Type filter
		if (TypeFilter.Equals(TEXT("burst"), ESearchCase::IgnoreCase) && !bIsStatic) continue;
		if (TypeFilter.Equals(TEXT("looping"), ESearchCase::IgnoreCase) && !bIsActor) continue;

		// Read cue tag
		FString CueTagStr;
		FProperty* CueTagProp = CDO->GetClass()->FindPropertyByName(TEXT("GameplayCueTag"));
		if (CueTagProp)
		{
			FGameplayTag* TagPtr = CueTagProp->ContainerPtrToValuePtr<FGameplayTag>(CDO);
			if (TagPtr && TagPtr->IsValid())
			{
				CueTagStr = TagPtr->ToString();
			}
		}

		// Tag prefix filter
		if (!TagPrefix.IsEmpty() && !CueTagStr.StartsWith(TagPrefix)) continue;

		TSharedPtr<FJsonObject> CueObj = MakeShared<FJsonObject>();
		CueObj->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		CueObj->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		CueObj->SetStringField(TEXT("type"), bIsActor ? TEXT("looping") : TEXT("burst"));
		CueObj->SetStringField(TEXT("cue_tag"), CueTagStr);
		CueArray.Add(MakeShared<FJsonValueObject>(CueObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), CueArray.Num());
	Result->SetArrayField(TEXT("cues"), CueArray);
	if (!TagPrefix.IsEmpty())
	{
		Result->SetStringField(TEXT("tag_prefix_filter"), TagPrefix);
	}
	if (!TypeFilter.Equals(TEXT("all"), ESearchCase::IgnoreCase))
	{
		Result->SetStringField(TEXT("type_filter"), TypeFilter);
	}

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 3: set_cue_parameters
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASCueActions::HandleSetCueParameters(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, Err)) return Err;

	FString Error;
	UObject* Obj = MonolithGAS::LoadAssetFromPath(AssetPath, Error);
	UBlueprint* BP = Cast<UBlueprint>(Obj);
	if (!BP || !BP->GeneratedClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load GCN Blueprint: %s — %s"), *AssetPath, *Error));
	}

	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	if (!CDO)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to get CDO for: %s"), *AssetPath));
	}

	bool bIsGCN = CDO->GetClass()->IsChildOf(UGameplayCueNotify_Static::StaticClass())
		|| CDO->GetClass()->IsChildOf(AGameplayCueNotify_Actor::StaticClass());
	if (!bIsGCN)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("'%s' is not a GameplayCue Notify"), *AssetPath));
	}

	TArray<FString> SetProperties;

	// Map of param name -> (property search names, expected type)
	struct FParamMapping
	{
		FString ParamName;
		TArray<FString> PropertyNames;
		bool bIsAssetRef;
	};

	TArray<FParamMapping> Mappings = {
		{ TEXT("burst_particle"), { TEXT("BurstParticleEffect"), TEXT("DefaultBurstParticleSystem"), TEXT("BurstParticleSystem"), TEXT("DefaultBurstNiagara"), TEXT("BurstNiagaraSystem") }, true },
		{ TEXT("burst_sound"),    { TEXT("BurstSound"), TEXT("DefaultBurstSound"), TEXT("BurstSoundCue") }, true },
		{ TEXT("loop_particle"),  { TEXT("LoopingParticleEffect"), TEXT("DefaultLoopingParticleSystem"), TEXT("LoopingNiagaraSystem"), TEXT("LoopParticleSystem") }, true },
		{ TEXT("loop_sound"),     { TEXT("LoopingSound"), TEXT("DefaultLoopingSound"), TEXT("LoopSoundCue") }, true },
		{ TEXT("camera_shake"),   { TEXT("CameraShake"), TEXT("BurstCameraShakeClass"), TEXT("DefaultCameraShake"), TEXT("CameraShakeClass") }, true },
	};

	for (const FParamMapping& Mapping : Mappings)
	{
		FString ParamValue = Params->GetStringField(Mapping.ParamName);
		if (ParamValue.IsEmpty()) continue;

		bool bSet = false;
		for (const FString& PropName : Mapping.PropertyNames)
		{
			FProperty* Prop = CDO->GetClass()->FindPropertyByName(FName(*PropName));
			if (!Prop) continue;

			if (Mapping.bIsAssetRef)
			{
				// Try soft object property
				if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
				{
					FSoftObjectPtr* SoftPtr = SoftProp->ContainerPtrToValuePtr<FSoftObjectPtr>(CDO);
					if (SoftPtr)
					{
						*SoftPtr = FSoftObjectPath(ParamValue);
						bSet = true;
						SetProperties.Add(FString::Printf(TEXT("%s = %s"), *PropName, *ParamValue));
						break;
					}
				}
				// Try soft class property
				else if (FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop))
				{
					FSoftObjectPtr* SoftPtr = SoftClassProp->ContainerPtrToValuePtr<FSoftObjectPtr>(CDO);
					if (SoftPtr)
					{
						*SoftPtr = FSoftObjectPath(ParamValue);
						bSet = true;
						SetProperties.Add(FString::Printf(TEXT("%s = %s"), *PropName, *ParamValue));
						break;
					}
				}
				// Try hard object property
				else if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
				{
					// Use the property's expected class so registry-class-mismatch is terminal
					// (won't silently load a wrong-class object at the same path).
					UClass* PropClass = ObjProp->PropertyClass.Get();
					UClass* ExpectedClass = PropClass ? PropClass : UObject::StaticClass();
					UObject* RefObj = FMonolithAssetUtils::LoadAssetByPath(ExpectedClass, ParamValue);
					if (RefObj)
					{
						ObjProp->SetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(CDO), RefObj);
						bSet = true;
						SetProperties.Add(FString::Printf(TEXT("%s = %s"), *PropName, *ParamValue));
						break;
					}
				}
				// Try class property (for camera shakes)
				else if (FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
				{
					UObject* RefObj = FMonolithAssetUtils::LoadAssetByPath(UClass::StaticClass(), ParamValue);
					if (RefObj)
					{
						ClassProp->SetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(CDO), RefObj);
						bSet = true;
						SetProperties.Add(FString::Printf(TEXT("%s = %s"), *PropName, *ParamValue));
						break;
					}
				}
			}
		}

		if (!bSet)
		{
			UE_LOG(LogMonolithGAS, Warning, TEXT("Could not set param '%s' — no matching property found on CDO of %s"), *Mapping.ParamName, *CDO->GetClass()->GetName());
		}
	}

	if (SetProperties.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No parameters were set. Check that the property names match the cue type."));
	}

	// Mark dirty and recompile
	BP->Modify();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(AssetPath,
		FString::Printf(TEXT("Set %d properties on cue"), SetProperties.Num()));

	TArray<TSharedPtr<FJsonValue>> PropsArr;
	for (const FString& S : SetProperties) PropsArr.Add(MakeShared<FJsonValueString>(S));
	Result->SetArrayField(TEXT("set_properties"), PropsArr);

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 3: find_cue_triggers
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASCueActions::HandleFindCueTriggers(const TSharedPtr<FJsonObject>& Params)
{
	FString CueTag;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("cue_tag"), CueTag, Err)) return Err;

	if (!CueTag.StartsWith(TEXT("GameplayCue.")))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Cue tag must start with 'GameplayCue.' — got: '%s'"), *CueTag));
	}

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Find all GE Blueprints
	TArray<FAssetData> AllBPs;
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	AssetRegistry.GetAssets(Filter, AllBPs);

	TArray<TSharedPtr<FJsonValue>> TriggerEffects;

	for (const FAssetData& AssetData : AllBPs)
	{
		// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
		FAssetTagValueRef ParentTag = AssetData.TagsAndValues.FindTag(FName("ParentClass"));
		FAssetTagValueRef NativeParentTag = AssetData.TagsAndValues.FindTag(FName("NativeParentClass"));
		FString ParentPath = ParentTag.IsSet() ? ParentTag.GetValue() : (NativeParentTag.IsSet() ? NativeParentTag.GetValue() : TEXT(""));
		if (!ParentPath.Contains(TEXT("GameplayEffect")))
		{
			continue;
		}

		UObject* Obj = AssetData.GetAsset();
		UBlueprint* BP = Cast<UBlueprint>(Obj);
		if (!BP || !MonolithGAS::IsGameplayEffectBlueprint(BP)) continue;

		UGameplayEffect* GE = MonolithGAS::GetBlueprintCDO<UGameplayEffect>(BP);
		if (!GE) continue;

		FGameplayTagContainer CueTags = GetCueTagContainer(GE);

		FGameplayTag SearchTag = FGameplayTag::RequestGameplayTag(FName(*CueTag), false);
		bool bFound = false;
		if (SearchTag.IsValid())
		{
			bFound = CueTags.HasTagExact(SearchTag);
		}
		else
		{
			// Fallback: string match
			for (const FGameplayTag& Tag : CueTags)
			{
				if (Tag.ToString().Equals(CueTag))
				{
					bFound = true;
					break;
				}
			}
		}

		if (bFound)
		{
			TSharedPtr<FJsonObject> EffectObj = MakeShared<FJsonObject>();
			EffectObj->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
			EffectObj->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());

			// Include duration policy for context
			FProperty* DurProp = GE->GetClass()->FindPropertyByName(TEXT("DurationPolicy"));
			if (DurProp)
			{
				FByteProperty* ByteProp = CastField<FByteProperty>(DurProp);
				FEnumProperty* EnumProp = CastField<FEnumProperty>(DurProp);
				if (ByteProp || EnumProp)
				{
					uint8* ValPtr = DurProp->ContainerPtrToValuePtr<uint8>(GE);
					if (ValPtr)
					{
						switch (*ValPtr)
						{
						case 0: EffectObj->SetStringField(TEXT("duration_policy"), TEXT("Instant")); break;
						case 1: EffectObj->SetStringField(TEXT("duration_policy"), TEXT("Infinite")); break;
						case 2: EffectObj->SetStringField(TEXT("duration_policy"), TEXT("HasDuration")); break;
						default: EffectObj->SetStringField(TEXT("duration_policy"), TEXT("Unknown")); break;
						}
					}
				}
			}

			TriggerEffects.Add(MakeShared<FJsonValueObject>(EffectObj));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("cue_tag"), CueTag);
	Result->SetNumberField(TEXT("trigger_count"), TriggerEffects.Num());
	Result->SetArrayField(TEXT("triggering_effects"), TriggerEffects);

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 3: validate_cue_coverage
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASCueActions::HandleValidateCueCoverage(const TSharedPtr<FJsonObject>& Params)
{
	FString PathFilter = Params->GetStringField(TEXT("path_filter"));

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

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

	// Pass 1: Collect all cue tags referenced by GEs
	TSet<FString> CueTagsOnEffects;
	for (const FAssetData& AssetData : AllBPs)
	{
		// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
		FAssetTagValueRef GEParentTag = AssetData.TagsAndValues.FindTag(FName("ParentClass"));
		FAssetTagValueRef GENativeParentTag = AssetData.TagsAndValues.FindTag(FName("NativeParentClass"));
		FString GEParentPath = GEParentTag.IsSet() ? GEParentTag.GetValue() : (GENativeParentTag.IsSet() ? GENativeParentTag.GetValue() : TEXT(""));
		if (!GEParentPath.Contains(TEXT("GameplayEffect")))
		{
			continue;
		}

		UObject* Obj = AssetData.GetAsset();
		UBlueprint* BP = Cast<UBlueprint>(Obj);
		if (!BP || !MonolithGAS::IsGameplayEffectBlueprint(BP)) continue;

		UGameplayEffect* GE = MonolithGAS::GetBlueprintCDO<UGameplayEffect>(BP);
		if (!GE) continue;

		FGameplayTagContainer CueTags = GetCueTagContainer(GE);
		for (const FGameplayTag& Tag : CueTags)
		{
			CueTagsOnEffects.Add(Tag.ToString());
		}
	}

	// Pass 2: Collect all cue tags handled by GCN assets
	TSet<FString> CueTagsHandled;
	TMap<FString, FString> CueHandlerMap; // tag -> asset path
	for (const FAssetData& AssetData : AllBPs)
	{
		// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
		FAssetTagValueRef GCNParentTag = AssetData.TagsAndValues.FindTag(FName("ParentClass"));
		FAssetTagValueRef GCNNativeParentTag = AssetData.TagsAndValues.FindTag(FName("NativeParentClass"));
		FString GCNParentPath = GCNParentTag.IsSet() ? GCNParentTag.GetValue() : (GCNNativeParentTag.IsSet() ? GCNNativeParentTag.GetValue() : TEXT(""));
		if (!GCNParentPath.Contains(TEXT("GameplayCueNotify")))
		{
			continue;
		}

		UObject* Obj = AssetData.GetAsset();
		UBlueprint* BP = Cast<UBlueprint>(Obj);
		if (!BP || !BP->GeneratedClass) continue;

		UObject* CDO = BP->GeneratedClass->GetDefaultObject();
		if (!CDO) continue;

		bool bIsGCN = CDO->GetClass()->IsChildOf(UGameplayCueNotify_Static::StaticClass())
			|| CDO->GetClass()->IsChildOf(AGameplayCueNotify_Actor::StaticClass());
		if (!bIsGCN) continue;

		FProperty* CueTagProp = CDO->GetClass()->FindPropertyByName(TEXT("GameplayCueTag"));
		if (CueTagProp)
		{
			FGameplayTag* TagPtr = CueTagProp->ContainerPtrToValuePtr<FGameplayTag>(CDO);
			if (TagPtr && TagPtr->IsValid())
			{
				CueTagsHandled.Add(TagPtr->ToString());
				CueHandlerMap.Add(TagPtr->ToString(), AssetData.GetObjectPathString());
			}
		}
	}

	// Find missing handlers (GE references a cue tag but no GCN handles it)
	TArray<TSharedPtr<FJsonValue>> MissingHandlers;
	for (const FString& Tag : CueTagsOnEffects)
	{
		if (!CueTagsHandled.Contains(Tag))
		{
			MissingHandlers.Add(MakeShared<FJsonValueString>(Tag));
		}
	}

	// Find orphaned cues (GCN exists but no GE triggers it)
	TArray<TSharedPtr<FJsonValue>> OrphanedCues;
	for (const auto& Pair : CueHandlerMap)
	{
		if (!CueTagsOnEffects.Contains(Pair.Key))
		{
			TSharedPtr<FJsonObject> Orphan = MakeShared<FJsonObject>();
			Orphan->SetStringField(TEXT("cue_tag"), Pair.Key);
			Orphan->SetStringField(TEXT("handler_asset"), Pair.Value);
			OrphanedCues.Add(MakeShared<FJsonValueObject>(Orphan));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("cue_tags_on_effects"), CueTagsOnEffects.Num());
	Result->SetNumberField(TEXT("cue_handlers_found"), CueTagsHandled.Num());
	Result->SetNumberField(TEXT("missing_handler_count"), MissingHandlers.Num());
	Result->SetNumberField(TEXT("orphaned_cue_count"), OrphanedCues.Num());
	Result->SetArrayField(TEXT("missing_handlers"), MissingHandlers);
	Result->SetArrayField(TEXT("orphaned_cues"), OrphanedCues);
	Result->SetBoolField(TEXT("fully_covered"), MissingHandlers.Num() == 0 && OrphanedCues.Num() == 0);
	if (!PathFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("path_filter"), PathFilter);
	}

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 3: batch_create_cues
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASCueActions::HandleBatchCreateCues(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* CuesArray;
	if (!Params->TryGetArrayField(TEXT("cues"), CuesArray) || CuesArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: cues"));
	}

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 SuccessCount = 0;
	int32 FailCount = 0;

	for (const TSharedPtr<FJsonValue>& CueVal : *CuesArray)
	{
		const TSharedPtr<FJsonObject>* CueObj;
		if (!CueVal->TryGetObject(CueObj))
		{
			TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetBoolField(TEXT("success"), false);
			FailObj->SetStringField(TEXT("error"), TEXT("Invalid cue entry — expected object"));
			Results.Add(MakeShared<FJsonValueObject>(FailObj));
			FailCount++;
			continue;
		}

		// Delegate to create_gameplay_cue_notify
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("save_path"), (*CueObj)->GetStringField(TEXT("save_path")));
		CreateParams->SetStringField(TEXT("cue_tag"), (*CueObj)->GetStringField(TEXT("cue_tag")));

		FString Type = (*CueObj)->GetStringField(TEXT("type"));
		if (!Type.IsEmpty())
		{
			CreateParams->SetStringField(TEXT("type"), Type);
		}

		FMonolithActionResult CreateResult = HandleCreateGameplayCueNotify(CreateParams);

		TSharedPtr<FJsonObject> EntryResult = MakeShared<FJsonObject>();
		EntryResult->SetStringField(TEXT("save_path"), (*CueObj)->GetStringField(TEXT("save_path")));
		EntryResult->SetStringField(TEXT("cue_tag"), (*CueObj)->GetStringField(TEXT("cue_tag")));
		EntryResult->SetBoolField(TEXT("success"), CreateResult.bSuccess);
		if (!CreateResult.bSuccess)
		{
			EntryResult->SetStringField(TEXT("error"), CreateResult.ErrorMessage);
			FailCount++;
		}
		else
		{
			SuccessCount++;
		}
		Results.Add(MakeShared<FJsonValueObject>(EntryResult));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("total"), CuesArray->Num());
	Result->SetNumberField(TEXT("success_count"), SuccessCount);
	Result->SetNumberField(TEXT("fail_count"), FailCount);
	Result->SetArrayField(TEXT("results"), Results);

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 3: scaffold_cue_library
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

struct FCuePresetEntry
{
	FString Tag;
	FString Type; // burst, looping, burst_latent
};

TArray<FCuePresetEntry> GetCombatCuePreset()
{
	return {
		{ TEXT("GameplayCue.Combat.Hit.Melee"), TEXT("burst") },
		{ TEXT("GameplayCue.Combat.Hit.Ranged"), TEXT("burst") },
		{ TEXT("GameplayCue.Combat.Hit.Explosive"), TEXT("burst") },
		{ TEXT("GameplayCue.Combat.Muzzle"), TEXT("burst") },
		{ TEXT("GameplayCue.Combat.Impact.Flesh"), TEXT("burst") },
		{ TEXT("GameplayCue.Combat.Impact.Metal"), TEXT("burst") },
		{ TEXT("GameplayCue.Combat.Impact.Wood"), TEXT("burst") },
		{ TEXT("GameplayCue.Combat.Impact.Stone"), TEXT("burst") },
		{ TEXT("GameplayCue.Combat.Blood.Spray"), TEXT("burst") },
		{ TEXT("GameplayCue.Combat.Blood.Pool"), TEXT("burst_latent") },
		{ TEXT("GameplayCue.Combat.Death"), TEXT("burst") },
	};
}

TArray<FCuePresetEntry> GetStatusCuePreset()
{
	return {
		{ TEXT("GameplayCue.Status.Burning"), TEXT("looping") },
		{ TEXT("GameplayCue.Status.Frozen"), TEXT("looping") },
		{ TEXT("GameplayCue.Status.Bleeding"), TEXT("looping") },
		{ TEXT("GameplayCue.Status.Poisoned"), TEXT("looping") },
		{ TEXT("GameplayCue.Status.Electrified"), TEXT("looping") },
		{ TEXT("GameplayCue.Status.Stunned"), TEXT("burst_latent") },
		{ TEXT("GameplayCue.Status.Heal"), TEXT("burst") },
		{ TEXT("GameplayCue.Status.ShieldBreak"), TEXT("burst") },
	};
}

TArray<FCuePresetEntry> GetHorrorCuePreset()
{
	return {
		{ TEXT("GameplayCue.Horror.Heartbeat"), TEXT("looping") },
		{ TEXT("GameplayCue.Horror.Whisper"), TEXT("looping") },
		{ TEXT("GameplayCue.Horror.ScreenDistort"), TEXT("looping") },
		{ TEXT("GameplayCue.Horror.Hallucination"), TEXT("looping") },
		{ TEXT("GameplayCue.Horror.Jumpscare"), TEXT("burst") },
		{ TEXT("GameplayCue.Horror.SanityPulse"), TEXT("burst") },
		{ TEXT("GameplayCue.Horror.BreathFog"), TEXT("looping") },
		{ TEXT("GameplayCue.Horror.EyeAdapt"), TEXT("looping") },
	};
}

/** Convert "GameplayCue.Combat.Hit.Melee" -> "GCN_Combat_Hit_Melee" */
FString CueTagToAssetName(const FString& CueTag)
{
	FString Name = CueTag;
	Name.RemoveFromStart(TEXT("GameplayCue."));
	Name.ReplaceInline(TEXT("."), TEXT("_"));
	return TEXT("GCN_") + Name;
}

} // anonymous namespace

FMonolithActionResult FMonolithGASCueActions::HandleScaffoldCueLibrary(const TSharedPtr<FJsonObject>& Params)
{
	FString Preset, SavePathPrefix;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("preset"), Preset, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("save_path_prefix"), SavePathPrefix, Err)) return Err;

	// Normalize path — ensure no trailing slash
	while (SavePathPrefix.EndsWith(TEXT("/")))
	{
		SavePathPrefix.LeftChopInline(1);
	}

	// Build the cue list based on preset
	TArray<FCuePresetEntry> PresetEntries;

	if (Preset.Equals(TEXT("combat"), ESearchCase::IgnoreCase))
	{
		PresetEntries = GetCombatCuePreset();
	}
	else if (Preset.Equals(TEXT("status"), ESearchCase::IgnoreCase))
	{
		PresetEntries = GetStatusCuePreset();
	}
	else if (Preset.Equals(TEXT("horror"), ESearchCase::IgnoreCase))
	{
		PresetEntries = GetHorrorCuePreset();
	}
	else if (Preset.Equals(TEXT("all"), ESearchCase::IgnoreCase))
	{
		PresetEntries.Append(GetCombatCuePreset());
		PresetEntries.Append(GetStatusCuePreset());
		PresetEntries.Append(GetHorrorCuePreset());
	}
	else
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown preset: '%s'. Valid: combat, status, horror, all"), *Preset));
	}

	// Ensure all cue tags are registered
	TArray<FString> TagsToRegister;
	for (const FCuePresetEntry& Entry : PresetEntries)
	{
		TagsToRegister.Add(Entry.Tag);
	}

	// Build batch create params
	TArray<TSharedPtr<FJsonValue>> CuesArray;
	for (const FCuePresetEntry& Entry : PresetEntries)
	{
		FString AssetName = CueTagToAssetName(Entry.Tag);
		FString SavePath = SavePathPrefix / AssetName;

		TSharedPtr<FJsonObject> CueObj = MakeShared<FJsonObject>();
		CueObj->SetStringField(TEXT("save_path"), SavePath);
		CueObj->SetStringField(TEXT("cue_tag"), Entry.Tag);
		CueObj->SetStringField(TEXT("type"), Entry.Type);
		CuesArray.Add(MakeShared<FJsonValueObject>(CueObj));
	}

	// Delegate to batch_create_cues
	TSharedPtr<FJsonObject> BatchParams = MakeShared<FJsonObject>();
	BatchParams->SetArrayField(TEXT("cues"), CuesArray);

	FMonolithActionResult BatchResult = HandleBatchCreateCues(BatchParams);

	// Augment with preset info
	if (BatchResult.bSuccess && BatchResult.Result.IsValid())
	{
		BatchResult.Result->SetStringField(TEXT("preset"), Preset);
		BatchResult.Result->SetStringField(TEXT("save_path_prefix"), SavePathPrefix);
	}

	return BatchResult;
}
