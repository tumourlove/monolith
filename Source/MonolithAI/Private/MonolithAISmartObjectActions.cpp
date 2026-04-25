#include "MonolithAISmartObjectActions.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#if WITH_SMARTOBJECTS
#include "SmartObjectDefinition.h"
#include "SmartObjectComponent.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "ScopedTransaction.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#endif // WITH_SMARTOBJECTS

void FMonolithAISmartObjectActions::RegisterActions(FMonolithToolRegistry& Registry)
{
#if WITH_SMARTOBJECTS

	// 127. create_smart_object_definition
	Registry.RegisterAction(TEXT("ai"), TEXT("create_smart_object_definition"),
		TEXT("Create a new USmartObjectDefinition data asset"),
		FMonolithActionHandler::CreateStatic(&HandleCreateSmartObjectDefinition),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path (e.g. /Game/AI/SmartObjects/SO_HideSpot)"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Asset name (derived from save_path if omitted)"))
			.Build());

	// 128. get_smart_object_definition
	Registry.RegisterAction(TEXT("ai"), TEXT("get_smart_object_definition"),
		TEXT("Full dump of a Smart Object definition: slots, tags, behaviors, shapes"),
		FMonolithActionHandler::CreateStatic(&HandleGetSmartObjectDefinition),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Smart Object Definition asset path"))
			.Build());

	// 129. list_smart_object_definitions
	Registry.RegisterAction(TEXT("ai"), TEXT("list_smart_object_definitions"),
		TEXT("List all USmartObjectDefinition assets in the project"),
		FMonolithActionHandler::CreateStatic(&HandleListSmartObjectDefinitions),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Only include assets under this path prefix"))
			.Build());

	// 130. delete_smart_object_definition
	Registry.RegisterAction(TEXT("ai"), TEXT("delete_smart_object_definition"),
		TEXT("Delete a Smart Object Definition asset"),
		FMonolithActionHandler::CreateStatic(&HandleDeleteSmartObjectDefinition),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Smart Object Definition asset path to delete"))
			.Build());

	// 131. add_so_slot
	Registry.RegisterAction(TEXT("ai"), TEXT("add_so_slot"),
		TEXT("Add a slot to a Smart Object definition"),
		FMonolithActionHandler::CreateStatic(&HandleAddSOSlot),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Smart Object Definition asset path"))
			.Optional(TEXT("offset"), TEXT("object"), TEXT("Slot offset {x, y, z} relative to parent"))
			.Optional(TEXT("rotation"), TEXT("object"), TEXT("Slot rotation {pitch, yaw, roll}"))
			.Optional(TEXT("activity_tags"), TEXT("array"), TEXT("Activity gameplay tags (e.g. [\"SmartObject.Activity.Sit\"])"))
			.Optional(TEXT("user_tags"), TEXT("array"), TEXT("User tag filter tags"))
			.Optional(TEXT("enabled"), TEXT("boolean"), TEXT("Whether slot is initially enabled (default: true)"))
			.Build());

	// 132. remove_so_slot
	Registry.RegisterAction(TEXT("ai"), TEXT("remove_so_slot"),
		TEXT("Remove a slot from a Smart Object definition by index"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveSOSlot),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Smart Object Definition asset path"))
			.Required(TEXT("slot_index"), TEXT("integer"), TEXT("Index of the slot to remove"))
			.Build());

	// 133. configure_so_slot
	Registry.RegisterAction(TEXT("ai"), TEXT("configure_so_slot"),
		TEXT("Edit properties of an existing slot on a Smart Object definition"),
		FMonolithActionHandler::CreateStatic(&HandleConfigureSOSlot),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Smart Object Definition asset path"))
			.Required(TEXT("slot_index"), TEXT("integer"), TEXT("Index of the slot to configure"))
			.Optional(TEXT("offset"), TEXT("object"), TEXT("Slot offset {x, y, z} relative to parent"))
			.Optional(TEXT("rotation"), TEXT("object"), TEXT("Slot rotation {pitch, yaw, roll}"))
			.Optional(TEXT("activity_tags"), TEXT("array"), TEXT("Activity gameplay tags"))
			.Optional(TEXT("user_tags"), TEXT("array"), TEXT("User tag filter tags"))
			.Optional(TEXT("enabled"), TEXT("boolean"), TEXT("Whether slot is enabled"))
			.Optional(TEXT("slot_name"), TEXT("string"), TEXT("Editor-only slot display name (FName)"), { TEXT("name") })
			.Build());

	// 134. add_so_behavior_definition
	Registry.RegisterAction(TEXT("ai"), TEXT("add_so_behavior_definition"),
		TEXT("Add a behavior definition to a Smart Object slot"),
		FMonolithActionHandler::CreateStatic(&HandleAddSOBehaviorDefinition),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Smart Object Definition asset path"))
			.Required(TEXT("slot_index"), TEXT("integer"), TEXT("Index of the slot"))
			.Required(TEXT("behavior_class"), TEXT("string"), TEXT("Behavior definition class name (e.g. USmartObjectGameplayBehaviorDefinition)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("Optional properties to set on the behavior definition"))
			.Build());

	// 135. remove_so_behavior_definition
	Registry.RegisterAction(TEXT("ai"), TEXT("remove_so_behavior_definition"),
		TEXT("Remove a behavior definition from a Smart Object slot"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveSOBehaviorDefinition),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Smart Object Definition asset path"))
			.Required(TEXT("slot_index"), TEXT("integer"), TEXT("Index of the slot"))
			.Required(TEXT("behavior_index"), TEXT("integer"), TEXT("Index of the behavior definition to remove"))
			.Build());

	// 136. set_so_tags
	Registry.RegisterAction(TEXT("ai"), TEXT("set_so_tags"),
		TEXT("Set definition-level activity tags and user tag filter on a Smart Object"),
		FMonolithActionHandler::CreateStatic(&HandleSetSOTags),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Smart Object Definition asset path"))
			.Optional(TEXT("activity_tags"), TEXT("array"), TEXT("Definition-level activity tags"))
			.Optional(TEXT("user_tags"), TEXT("array"), TEXT("Definition-level user tag filter tags"))
			.Build());

	// 137. add_smart_object_component
	Registry.RegisterAction(TEXT("ai"), TEXT("add_smart_object_component"),
		TEXT("Add USmartObjectComponent to an actor Blueprint via SCS"),
		FMonolithActionHandler::CreateStatic(&HandleAddSmartObjectComponent),
		FParamSchemaBuilder()
			.Required(TEXT("blueprint_path"), TEXT("string"), TEXT("Actor Blueprint asset path"))
			.Required(TEXT("definition_path"), TEXT("string"), TEXT("Smart Object Definition asset path to assign"))
			.Build());

	// 138. place_smart_object_actor
	Registry.RegisterAction(TEXT("ai"), TEXT("place_smart_object_actor"),
		TEXT("Spawn an actor with USmartObjectComponent in the current level"),
		FMonolithActionHandler::CreateStatic(&HandlePlaceSmartObjectActor),
		FParamSchemaBuilder()
			.Required(TEXT("definition_path"), TEXT("string"), TEXT("Smart Object Definition asset path"))
			.Required(TEXT("location"), TEXT("object"), TEXT("World location {x, y, z}"))
			.Optional(TEXT("rotation"), TEXT("object"), TEXT("World rotation {pitch, yaw, roll}"))
			.Optional(TEXT("folder_path"), TEXT("string"), TEXT("World Outliner folder (default: AI/SmartObjects)"))
			.Build());

	// 139. find_smart_objects_in_level
	Registry.RegisterAction(TEXT("ai"), TEXT("find_smart_objects_in_level"),
		TEXT("List all placed Smart Object components in the current level"),
		FMonolithActionHandler::CreateStatic(&HandleFindSmartObjectsInLevel),
		FParamSchemaBuilder()
			.Optional(TEXT("level"), TEXT("string"), TEXT("Level name filter"))
			.Optional(TEXT("definition_filter"), TEXT("string"), TEXT("Filter by definition asset path"))
			.Optional(TEXT("tag_filter"), TEXT("array"), TEXT("Filter by activity tags"))
			.Build());

	// 140. validate_smart_object_definition
	Registry.RegisterAction(TEXT("ai"), TEXT("validate_smart_object_definition"),
		TEXT("Validate a Smart Object definition: slots have behaviors, tags valid, etc."),
		FMonolithActionHandler::CreateStatic(&HandleValidateSmartObjectDefinition),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Smart Object Definition asset path"))
			.Build());

	// 141. create_so_from_template
	Registry.RegisterAction(TEXT("ai"), TEXT("create_so_from_template"),
		TEXT("Create a Smart Object definition from a preset template (hide_spot, sit_chair, workstation, door_interaction, pickup_item)"),
		FMonolithActionHandler::CreateStatic(&HandleCreateSOFromTemplate),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path"))
			.Required(TEXT("template"), TEXT("string"), TEXT("Template name: hide_spot, sit_chair, workstation, door_interaction, pickup_item"))
			.Build());

	// 142. duplicate_smart_object_definition
	Registry.RegisterAction(TEXT("ai"), TEXT("duplicate_smart_object_definition"),
		TEXT("Deep copy a Smart Object definition to a new path"),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateSmartObjectDefinition),
		FParamSchemaBuilder()
			.Required(TEXT("source_path"), TEXT("string"), TEXT("Source definition asset path"))
			.Required(TEXT("dest_path"), TEXT("string"), TEXT("Destination asset path for the copy"))
			.Build());

#endif // WITH_SMARTOBJECTS
}

// ============================================================
//  Everything below is Smart Object implementation
// ============================================================

#if WITH_SMARTOBJECTS

// ============================================================
//  Helpers
// ============================================================

USmartObjectDefinition* FMonolithAISmartObjectActions::LoadSODefinition(
	const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
{
	OutAssetPath = Params->GetStringField(TEXT("asset_path"));
	if (OutAssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required parameter: asset_path");
		return nullptr;
	}

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(USmartObjectDefinition::StaticClass(), OutAssetPath);
	USmartObjectDefinition* Def = Cast<USmartObjectDefinition>(Asset);
	if (!Def)
	{
		OutError = FString::Printf(TEXT("Smart Object Definition not found: %s"), *OutAssetPath);
		return nullptr;
	}

	return Def;
}

namespace
{
	/**
	 * F.7a — Parse a JSON tag-string array into a FGameplayTagContainer, surfacing dropped (unregistered) tag strings.
	 *
	 * Drop-silently was the prior behavior; testers never saw typos. New callers should consume OutSkipped and
	 * emit a "warnings" array on the response (model: MonolithLogicDriverNodeActions::set_node_tags / Phase B B5).
	 *
	 * The header-declared FMonolithAISmartObjectActions::ParseTagContainer one-arg overload is preserved
	 * (delegates here, discards OutSkipped) so any external caller stays source-compatible.
	 */
	void ParseTagContainerWithSkipped(
		const TSharedPtr<FJsonObject>& Params,
		const FString& FieldName,
		FGameplayTagContainer& OutTags,
		TArray<FString>& OutSkipped)
	{
		OutTags.Reset();
		// OutSkipped is append-only; do NOT Reset() so chained callers can accumulate across multiple fields.
		TArray<FString> TagStrings = MonolithAI::ParseStringArray(Params, FieldName);
		for (const FString& TagStr : TagStrings)
		{
			FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagStr), /*bErrorIfNotFound=*/false);
			if (Tag.IsValid())
			{
				OutTags.AddTag(Tag);
			}
			else
			{
				UE_LOG(LogMonolithAI, Warning, TEXT("Gameplay tag not found: '%s' (field=%s)"), *TagStr, *FieldName);
				OutSkipped.Add(TagStr);
			}
		}
	}

	/**
	 * F.7a — Append warning entries to an in-progress array for one (field_name, skipped-list) pair.
	 * Caller invokes once per parsed tag-container field, then attaches the array via SetArrayField if non-empty.
	 */
	void AppendTagWarnings(
		TArray<TSharedPtr<FJsonValue>>& InOutWarnings,
		const TCHAR* FieldName,
		const TArray<FString>& Skipped)
	{
		for (const FString& T : Skipped)
		{
			InOutWarnings.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("%s '%s' is not a registered GameplayTag — dropped"), FieldName, *T)));
		}
	}
}

void FMonolithAISmartObjectActions::ParseTagContainer(
	const TSharedPtr<FJsonObject>& Params, const FString& FieldName, FGameplayTagContainer& OutTags)
{
	// Backwards-compat one-arg overload — delegates to the skipped-aware free helper and discards OutSkipped.
	TArray<FString> Discarded;
	ParseTagContainerWithSkipped(Params, FieldName, OutTags, Discarded);
}

TSharedPtr<FJsonObject> FMonolithAISmartObjectActions::SlotToJson(
	const FSmartObjectSlotDefinition& Slot, int32 Index)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("index"), Index);
	Obj->SetBoolField(TEXT("enabled"), Slot.bEnabled);

	// Offset
	TSharedPtr<FJsonObject> OffsetObj = MakeShared<FJsonObject>();
	OffsetObj->SetNumberField(TEXT("x"), Slot.Offset.X);
	OffsetObj->SetNumberField(TEXT("y"), Slot.Offset.Y);
	OffsetObj->SetNumberField(TEXT("z"), Slot.Offset.Z);
	Obj->SetObjectField(TEXT("offset"), OffsetObj);

	// Rotation
	TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
	RotObj->SetNumberField(TEXT("pitch"), Slot.Rotation.Pitch);
	RotObj->SetNumberField(TEXT("yaw"), Slot.Rotation.Yaw);
	RotObj->SetNumberField(TEXT("roll"), Slot.Rotation.Roll);
	Obj->SetObjectField(TEXT("rotation"), RotObj);

	// Activity Tags
	if (!Slot.ActivityTags.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> TagArr;
		for (const FGameplayTag& Tag : Slot.ActivityTags)
		{
			TagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		Obj->SetArrayField(TEXT("activity_tags"), TagArr);
	}

	// User Tag Filter
	FString UserFilterDesc = Slot.UserTagFilter.GetDescription();
	if (!UserFilterDesc.IsEmpty())
	{
		Obj->SetStringField(TEXT("user_tag_filter"), UserFilterDesc);
	}

	// Runtime Tags
	if (!Slot.RuntimeTags.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> RuntimeTagArr;
		for (const FGameplayTag& Tag : Slot.RuntimeTags)
		{
			RuntimeTagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		Obj->SetArrayField(TEXT("runtime_tags"), RuntimeTagArr);
	}

	// Behavior definitions
	TArray<TSharedPtr<FJsonValue>> BehaviorArr;
	for (int32 i = 0; i < Slot.BehaviorDefinitions.Num(); ++i)
	{
		const USmartObjectBehaviorDefinition* BehaviorDef = Slot.BehaviorDefinitions[i];
		if (!BehaviorDef) continue;

		TSharedPtr<FJsonObject> BehaviorObj = MakeShared<FJsonObject>();
		BehaviorObj->SetNumberField(TEXT("index"), i);
		BehaviorObj->SetStringField(TEXT("class"), BehaviorDef->GetClass()->GetName());
		BehaviorObj->SetStringField(TEXT("class_path"), BehaviorDef->GetClass()->GetPathName());
		BehaviorArr.Add(MakeShared<FJsonValueObject>(BehaviorObj));
	}
	Obj->SetArrayField(TEXT("behavior_definitions"), BehaviorArr);
	Obj->SetNumberField(TEXT("behavior_count"), BehaviorArr.Num());

#if WITH_EDITORONLY_DATA
	if (!Slot.Name.IsNone())
	{
		Obj->SetStringField(TEXT("name"), Slot.Name.ToString());
	}
#endif

	return Obj;
}

TSharedPtr<FJsonObject> FMonolithAISmartObjectActions::DefinitionToJson(
	USmartObjectDefinition* Def, const FString& AssetPath)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("name"), Def->GetName());

	// Definition-level tags
	const FGameplayTagContainer& ActivityTags = Def->GetActivityTags();
	if (!ActivityTags.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> TagArr;
		for (const FGameplayTag& Tag : ActivityTags)
		{
			TagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		Result->SetArrayField(TEXT("activity_tags"), TagArr);
	}

	FString UserFilterDesc = Def->GetUserTagFilter().GetDescription();
	if (!UserFilterDesc.IsEmpty())
	{
		Result->SetStringField(TEXT("user_tag_filter"), UserFilterDesc);
	}

	// Slots
	TConstArrayView<FSmartObjectSlotDefinition> Slots = Def->GetSlots();
	TArray<TSharedPtr<FJsonValue>> SlotArr;
	for (int32 i = 0; i < Slots.Num(); ++i)
	{
		SlotArr.Add(MakeShared<FJsonValueObject>(SlotToJson(Slots[i], i)));
	}
	Result->SetArrayField(TEXT("slots"), SlotArr);
	Result->SetNumberField(TEXT("slot_count"), Slots.Num());

	return Result;
}

// ============================================================
//  127. create_smart_object_definition
// ============================================================

FMonolithActionResult FMonolithAISmartObjectActions::HandleCreateSmartObjectDefinition(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult))
	{
		return ErrResult;
	}

	FString AssetName = Params->GetStringField(TEXT("name"));
	if (AssetName.IsEmpty())
	{
		AssetName = FPackageName::GetShortName(SavePath);
	}

	FString PackagePath = SavePath;

	FString PathError;
	if (!MonolithAI::EnsureAssetPathFree(PackagePath, AssetName, PathError))
	{
		return FMonolithActionResult::Error(PathError);
	}

	FString PkgError;
	UPackage* Package = MonolithAI::GetOrCreatePackage(PackagePath, PkgError);
	if (!Package)
	{
		return FMonolithActionResult::Error(PkgError);
	}

	USmartObjectDefinition* Def = NewObject<USmartObjectDefinition>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!Def)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create USmartObjectDefinition object"));
	}

	FAssetRegistryModule::AssetCreated(Def);
	Def->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(PackagePath, TEXT("Smart Object Definition created"));
	Result->SetStringField(TEXT("name"), AssetName);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  128. get_smart_object_definition
// ============================================================

FMonolithActionResult FMonolithAISmartObjectActions::HandleGetSmartObjectDefinition(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	USmartObjectDefinition* Def = LoadSODefinition(Params, AssetPath, Error);
	if (!Def)
	{
		return FMonolithActionResult::Error(Error);
	}

	return FMonolithActionResult::Success(DefinitionToJson(Def, AssetPath));
}

// ============================================================
//  129. list_smart_object_definitions
// ============================================================

FMonolithActionResult FMonolithAISmartObjectActions::HandleListSmartObjectDefinitions(const TSharedPtr<FJsonObject>& Params)
{
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetData> Assets;
	AR.GetAssetsByClass(USmartObjectDefinition::StaticClass()->GetClassPathName(), Assets);

	FString PathFilter = Params->GetStringField(TEXT("path_filter"));

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FAssetData& Asset : Assets)
	{
		FString ObjPath = Asset.GetObjectPathString();
		if (!PathFilter.IsEmpty() && !ObjPath.StartsWith(PathFilter))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("asset_path"), Asset.PackageName.ToString());
		Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("definitions"), Items);
	Result->SetNumberField(TEXT("count"), Items.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  130. delete_smart_object_definition
// ============================================================

FMonolithActionResult FMonolithAISmartObjectActions::HandleDeleteSmartObjectDefinition(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("asset_path"), AssetPath, ErrResult))
	{
		return ErrResult;
	}

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(USmartObjectDefinition::StaticClass(), AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	TArray<UObject*> ObjectsToDelete;
	ObjectsToDelete.Add(Asset);

	int32 NumDeleted = ObjectTools::ForceDeleteObjects(ObjectsToDelete, /*bShowConfirmation=*/false);
	if (NumDeleted == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to delete: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("message"), TEXT("Smart Object Definition deleted"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  131. add_so_slot
// ============================================================

FMonolithActionResult FMonolithAISmartObjectActions::HandleAddSOSlot(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	USmartObjectDefinition* Def = LoadSODefinition(Params, AssetPath, Error);
	if (!Def)
	{
		return FMonolithActionResult::Error(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add Smart Object Slot")));
	Def->Modify();

	FSmartObjectSlotDefinition& NewSlot = Def->DebugAddSlot();

	// Offset
	const TSharedPtr<FJsonObject>* OffsetObj = nullptr;
	if (Params->TryGetObjectField(TEXT("offset"), OffsetObj) && OffsetObj && (*OffsetObj)->Values.Num() > 0)
	{
		NewSlot.Offset.X = static_cast<float>((*OffsetObj)->GetNumberField(TEXT("x")));
		NewSlot.Offset.Y = static_cast<float>((*OffsetObj)->GetNumberField(TEXT("y")));
		NewSlot.Offset.Z = static_cast<float>((*OffsetObj)->GetNumberField(TEXT("z")));
	}

	// Rotation
	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (Params->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj && (*RotObj)->Values.Num() > 0)
	{
		NewSlot.Rotation.Pitch = static_cast<float>((*RotObj)->GetNumberField(TEXT("pitch")));
		NewSlot.Rotation.Yaw = static_cast<float>((*RotObj)->GetNumberField(TEXT("yaw")));
		NewSlot.Rotation.Roll = static_cast<float>((*RotObj)->GetNumberField(TEXT("roll")));
	}

	// Activity tags + user tags — F.7a: surface dropped tags as warnings on the response
	TArray<FString> SkippedActivity, SkippedUser;
	if (Params->HasField(TEXT("activity_tags")))
	{
		ParseTagContainerWithSkipped(Params, TEXT("activity_tags"), NewSlot.ActivityTags, SkippedActivity);
	}

	if (Params->HasField(TEXT("user_tags")))
	{
		FGameplayTagContainer UserTags;
		ParseTagContainerWithSkipped(Params, TEXT("user_tags"), UserTags, SkippedUser);
		if (!UserTags.IsEmpty())
		{
			NewSlot.UserTagFilter = FGameplayTagQuery::MakeQuery_MatchAllTags(UserTags);
		}
	}

	// Enabled
	if (Params->HasField(TEXT("enabled")))
	{
		NewSlot.bEnabled = Params->GetBoolField(TEXT("enabled"));
	}

#if WITH_EDITORONLY_DATA
	NewSlot.ID = FGuid::NewGuid();
#endif

	Def->MarkPackageDirty();

	int32 SlotIndex = Def->GetSlots().Num() - 1;
	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(AssetPath, TEXT("Slot added"));
	Result->SetNumberField(TEXT("slot_index"), SlotIndex);
	Result->SetNumberField(TEXT("total_slots"), Def->GetSlots().Num());

	TArray<TSharedPtr<FJsonValue>> Warnings;
	AppendTagWarnings(Warnings, TEXT("activity_tag"), SkippedActivity);
	AppendTagWarnings(Warnings, TEXT("user_tag"), SkippedUser);
	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  132. remove_so_slot
// ============================================================

FMonolithActionResult FMonolithAISmartObjectActions::HandleRemoveSOSlot(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	USmartObjectDefinition* Def = LoadSODefinition(Params, AssetPath, Error);
	if (!Def)
	{
		return FMonolithActionResult::Error(Error);
	}

	int32 SlotIndex = static_cast<int32>(Params->GetNumberField(TEXT("slot_index")));
	if (!Def->IsValidSlotIndex(SlotIndex))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid slot_index %d (definition has %d slots)"), SlotIndex, Def->GetSlots().Num()));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Remove Smart Object Slot")));
	Def->Modify();

	// GetMutableSlots() returns TArrayView — can't resize through it.
	// Use the property system to access the underlying TArray and remove the element.
	FProperty* SlotsProp = USmartObjectDefinition::StaticClass()->FindPropertyByName(FName(TEXT("Slots")));
	if (!SlotsProp)
	{
		return FMonolithActionResult::Error(TEXT("Internal error: could not find Slots property on USmartObjectDefinition"));
	}

	FArrayProperty* ArrayProp = CastField<FArrayProperty>(SlotsProp);
	if (!ArrayProp)
	{
		return FMonolithActionResult::Error(TEXT("Internal error: Slots property is not an array"));
	}

	void* ArrayPtr = ArrayProp->ContainerPtrToValuePtr<void>(Def);
	FScriptArrayHelper ArrayHelper(ArrayProp, ArrayPtr);
	ArrayHelper.RemoveValues(SlotIndex, 1);

	Def->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(AssetPath, TEXT("Slot removed"));
	Result->SetNumberField(TEXT("removed_index"), SlotIndex);
	Result->SetNumberField(TEXT("remaining_slots"), Def->GetSlots().Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  133. configure_so_slot
// ============================================================

FMonolithActionResult FMonolithAISmartObjectActions::HandleConfigureSOSlot(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	USmartObjectDefinition* Def = LoadSODefinition(Params, AssetPath, Error);
	if (!Def)
	{
		return FMonolithActionResult::Error(Error);
	}

	int32 SlotIndex = static_cast<int32>(Params->GetNumberField(TEXT("slot_index")));
	if (!Def->IsValidSlotIndex(SlotIndex))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid slot_index %d (definition has %d slots)"), SlotIndex, Def->GetSlots().Num()));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Configure Smart Object Slot")));
	Def->Modify();

	FSmartObjectSlotDefinition& Slot = Def->GetMutableSlot(SlotIndex);

	// Offset
	const TSharedPtr<FJsonObject>* OffsetObj = nullptr;
	if (Params->TryGetObjectField(TEXT("offset"), OffsetObj) && OffsetObj && (*OffsetObj)->Values.Num() > 0)
	{
		Slot.Offset.X = static_cast<float>((*OffsetObj)->GetNumberField(TEXT("x")));
		Slot.Offset.Y = static_cast<float>((*OffsetObj)->GetNumberField(TEXT("y")));
		Slot.Offset.Z = static_cast<float>((*OffsetObj)->GetNumberField(TEXT("z")));
	}

	// Rotation
	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (Params->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj && (*RotObj)->Values.Num() > 0)
	{
		Slot.Rotation.Pitch = static_cast<float>((*RotObj)->GetNumberField(TEXT("pitch")));
		Slot.Rotation.Yaw = static_cast<float>((*RotObj)->GetNumberField(TEXT("yaw")));
		Slot.Rotation.Roll = static_cast<float>((*RotObj)->GetNumberField(TEXT("roll")));
	}

	// Activity tags + user tags — F.7a: surface dropped tags as warnings on the response
	TArray<FString> SkippedActivity, SkippedUser;
	if (Params->HasField(TEXT("activity_tags")))
	{
		ParseTagContainerWithSkipped(Params, TEXT("activity_tags"), Slot.ActivityTags, SkippedActivity);
	}

	if (Params->HasField(TEXT("user_tags")))
	{
		FGameplayTagContainer UserTags;
		ParseTagContainerWithSkipped(Params, TEXT("user_tags"), UserTags, SkippedUser);
		if (!UserTags.IsEmpty())
		{
			Slot.UserTagFilter = FGameplayTagQuery::MakeQuery_MatchAllTags(UserTags);
		}
		else
		{
			Slot.UserTagFilter = FGameplayTagQuery();
		}
	}

	// Enabled
	if (Params->HasField(TEXT("enabled")))
	{
		Slot.bEnabled = Params->GetBoolField(TEXT("enabled"));
	}

	// Slot name (editor-only data — FSmartObjectSlotDefinition::Name is gated on WITH_EDITORONLY_DATA).
	bool bRequestedSlotName = false;
	FString RequestedSlotName;
	bool bSlotNameAppliedAtRuntime = false;
	if (Params->HasField(TEXT("slot_name")))
	{
		RequestedSlotName = Params->GetStringField(TEXT("slot_name"));
		bRequestedSlotName = true;
#if WITH_EDITORONLY_DATA
		Slot.Name = FName(*RequestedSlotName);
		bSlotNameAppliedAtRuntime = true;
#endif
	}

	Def->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(AssetPath, TEXT("Slot configured"));
	Result->SetNumberField(TEXT("slot_index"), SlotIndex);
	Result->SetObjectField(TEXT("slot"), SlotToJson(Slot, SlotIndex));

	// Build a single warnings array combining slot_name + dropped-tag warnings (F.7a).
	TArray<TSharedPtr<FJsonValue>> Warnings;

	// verified_value smoking gun
	if (bRequestedSlotName)
	{
		TSharedPtr<FJsonObject> Verified = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> NameEntry = MakeShared<FJsonObject>();
		NameEntry->SetStringField(TEXT("requested"), RequestedSlotName);
#if WITH_EDITORONLY_DATA
		const FString ActualName = Slot.Name.ToString();
		NameEntry->SetStringField(TEXT("actual"), ActualName);
		NameEntry->SetBoolField(TEXT("match"), ActualName == RequestedSlotName);
#else
		NameEntry->SetStringField(TEXT("actual"), TEXT(""));
		NameEntry->SetBoolField(TEXT("match"), false);
#endif
		Verified->SetObjectField(TEXT("slot_name"), NameEntry);
		Result->SetObjectField(TEXT("verified_value"), Verified);

		if (!bSlotNameAppliedAtRuntime)
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				TEXT("slot_name was ignored: FSmartObjectSlotDefinition::Name is editor-only (WITH_EDITORONLY_DATA).")));
		}
	}

	AppendTagWarnings(Warnings, TEXT("activity_tag"), SkippedActivity);
	AppendTagWarnings(Warnings, TEXT("user_tag"), SkippedUser);
	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  134. add_so_behavior_definition
// ============================================================

FMonolithActionResult FMonolithAISmartObjectActions::HandleAddSOBehaviorDefinition(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	USmartObjectDefinition* Def = LoadSODefinition(Params, AssetPath, Error);
	if (!Def)
	{
		return FMonolithActionResult::Error(Error);
	}

	int32 SlotIndex = static_cast<int32>(Params->GetNumberField(TEXT("slot_index")));
	if (!Def->IsValidSlotIndex(SlotIndex))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid slot_index %d (definition has %d slots)"), SlotIndex, Def->GetSlots().Num()));
	}

	FString BehaviorClassName;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("behavior_class"), BehaviorClassName, ErrResult))
	{
		return ErrResult;
	}

	// Resolve behavior class
	// Strip leading 'U' if provided (common convention)
	FString ClassToFind = BehaviorClassName;
	UClass* BehaviorClass = FindFirstObject<UClass>(*ClassToFind, EFindFirstObjectOptions::EnsureIfAmbiguous);
	if (!BehaviorClass && ClassToFind.StartsWith(TEXT("U")))
	{
		// Try without U prefix
		ClassToFind = ClassToFind.Mid(1);
		BehaviorClass = FindFirstObject<UClass>(*ClassToFind, EFindFirstObjectOptions::EnsureIfAmbiguous);
	}
	if (!BehaviorClass)
	{
		// Try loading as a full path
		BehaviorClass = LoadObject<UClass>(nullptr, *BehaviorClassName);
	}

	if (!BehaviorClass || !BehaviorClass->IsChildOf(USmartObjectBehaviorDefinition::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("'%s' is not a valid USmartObjectBehaviorDefinition subclass"), *BehaviorClassName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add SO Behavior Definition")));
	Def->Modify();

	FSmartObjectSlotDefinition& Slot = Def->GetMutableSlot(SlotIndex);

	// Create the behavior definition as a subobject of the definition asset
	USmartObjectBehaviorDefinition* NewBehavior = NewObject<USmartObjectBehaviorDefinition>(
		Def, BehaviorClass, NAME_None, RF_Public | RF_Transactional);
	if (!NewBehavior)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create behavior definition object"));
	}

	Slot.BehaviorDefinitions.Add(NewBehavior);
	Def->MarkPackageDirty();

	int32 BehaviorIndex = Slot.BehaviorDefinitions.Num() - 1;
	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(AssetPath, TEXT("Behavior definition added"));
	Result->SetNumberField(TEXT("slot_index"), SlotIndex);
	Result->SetNumberField(TEXT("behavior_index"), BehaviorIndex);
	Result->SetStringField(TEXT("behavior_class"), BehaviorClass->GetName());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  135. remove_so_behavior_definition
// ============================================================

FMonolithActionResult FMonolithAISmartObjectActions::HandleRemoveSOBehaviorDefinition(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	USmartObjectDefinition* Def = LoadSODefinition(Params, AssetPath, Error);
	if (!Def)
	{
		return FMonolithActionResult::Error(Error);
	}

	int32 SlotIndex = static_cast<int32>(Params->GetNumberField(TEXT("slot_index")));
	if (!Def->IsValidSlotIndex(SlotIndex))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid slot_index %d (definition has %d slots)"), SlotIndex, Def->GetSlots().Num()));
	}

	int32 BehaviorIndex = static_cast<int32>(Params->GetNumberField(TEXT("behavior_index")));
	FSmartObjectSlotDefinition& Slot = Def->GetMutableSlot(SlotIndex);
	if (!Slot.BehaviorDefinitions.IsValidIndex(BehaviorIndex))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid behavior_index %d (slot %d has %d behaviors)"),
			BehaviorIndex, SlotIndex, Slot.BehaviorDefinitions.Num()));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Remove SO Behavior Definition")));
	Def->Modify();

	Slot.BehaviorDefinitions.RemoveAt(BehaviorIndex);
	Def->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(AssetPath, TEXT("Behavior definition removed"));
	Result->SetNumberField(TEXT("slot_index"), SlotIndex);
	Result->SetNumberField(TEXT("removed_behavior_index"), BehaviorIndex);
	Result->SetNumberField(TEXT("remaining_behaviors"), Slot.BehaviorDefinitions.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  136. set_so_tags
// ============================================================

FMonolithActionResult FMonolithAISmartObjectActions::HandleSetSOTags(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	USmartObjectDefinition* Def = LoadSODefinition(Params, AssetPath, Error);
	if (!Def)
	{
		return FMonolithActionResult::Error(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Set SO Tags")));
	Def->Modify();

	// F.7a: surface dropped tags as warnings on the response
	TArray<FString> SkippedActivity, SkippedUser;
	if (Params->HasField(TEXT("activity_tags")))
	{
		FGameplayTagContainer Tags;
		ParseTagContainerWithSkipped(Params, TEXT("activity_tags"), Tags, SkippedActivity);
		Def->SetActivityTags(Tags);
	}

	if (Params->HasField(TEXT("user_tags")))
	{
		FGameplayTagContainer UserTags;
		ParseTagContainerWithSkipped(Params, TEXT("user_tags"), UserTags, SkippedUser);
		if (!UserTags.IsEmpty())
		{
			Def->SetUserTagFilter(FGameplayTagQuery::MakeQuery_MatchAllTags(UserTags));
		}
		else
		{
			Def->SetUserTagFilter(FGameplayTagQuery());
		}
	}

	Def->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(AssetPath, TEXT("Tags updated"));

	const FGameplayTagContainer& ActivityTags = Def->GetActivityTags();
	if (!ActivityTags.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> TagArr;
		for (const FGameplayTag& Tag : ActivityTags)
		{
			TagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		Result->SetArrayField(TEXT("activity_tags"), TagArr);
	}

	FString UserFilterDesc = Def->GetUserTagFilter().GetDescription();
	if (!UserFilterDesc.IsEmpty())
	{
		Result->SetStringField(TEXT("user_tag_filter"), UserFilterDesc);
	}

	TArray<TSharedPtr<FJsonValue>> Warnings;
	AppendTagWarnings(Warnings, TEXT("activity_tag"), SkippedActivity);
	AppendTagWarnings(Warnings, TEXT("user_tag"), SkippedUser);
	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  137. add_smart_object_component
// ============================================================

FMonolithActionResult FMonolithAISmartObjectActions::HandleAddSmartObjectComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintPath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("blueprint_path"), BlueprintPath, ErrResult))
	{
		return ErrResult;
	}

	FString DefinitionPath;
	if (!MonolithAI::RequireStringParam(Params, TEXT("definition_path"), DefinitionPath, ErrResult))
	{
		return ErrResult;
	}

	// Load the Blueprint
	FString LoadError;
	UObject* BPObj = MonolithAI::LoadAssetFromPath(BlueprintPath, LoadError);
	if (!BPObj)
	{
		return FMonolithActionResult::Error(LoadError);
	}

	UBlueprint* BP = Cast<UBlueprint>(BPObj);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("'%s' is not a Blueprint"), *BlueprintPath));
	}

	if (!BP->SimpleConstructionScript)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript — is it an Actor Blueprint?"));
	}

	// Load the SO definition
	USmartObjectDefinition* SODef = Cast<USmartObjectDefinition>(
		FMonolithAssetUtils::LoadAssetByPath(USmartObjectDefinition::StaticClass(), DefinitionPath));
	if (!SODef)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Smart Object Definition not found: %s"), *DefinitionPath));
	}

	// Check if SO component already exists
	for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
	{
		if (Node && Node->ComponentClass &&
			Node->ComponentClass->IsChildOf(USmartObjectComponent::StaticClass()))
		{
			return FMonolithActionResult::Error(
				TEXT("This Blueprint already has a USmartObjectComponent."));
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add SmartObject Component")));

	const EBlueprintStatus SavedStatus = BP->Status;
	BP->Status = BS_BeingCreated;

	USCS_Node* NewNode = BP->SimpleConstructionScript->CreateNode(
		USmartObjectComponent::StaticClass(), FName(TEXT("SmartObjectComponent")));
	if (!NewNode)
	{
		BP->Status = SavedStatus;
		return FMonolithActionResult::Error(TEXT("Failed to create SmartObjectComponent SCS node"));
	}

	BP->SimpleConstructionScript->AddNode(NewNode);

	// Set the definition on the component template
	USmartObjectComponent* SOComp = Cast<USmartObjectComponent>(NewNode->ComponentTemplate);
	if (SOComp)
	{
		SOComp->SetDefinition(SODef);
	}

	BP->Status = SavedStatus;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(BlueprintPath, TEXT("SmartObjectComponent added"));
	Result->SetStringField(TEXT("variable_name"), NewNode->GetVariableName().ToString());
	Result->SetStringField(TEXT("definition"), DefinitionPath);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  138. place_smart_object_actor
// ============================================================

FMonolithActionResult FMonolithAISmartObjectActions::HandlePlaceSmartObjectActor(const TSharedPtr<FJsonObject>& Params)
{
	FString DefinitionPath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("definition_path"), DefinitionPath, ErrResult))
	{
		return ErrResult;
	}

	// Load the definition
	USmartObjectDefinition* SODef = Cast<USmartObjectDefinition>(
		FMonolithAssetUtils::LoadAssetByPath(USmartObjectDefinition::StaticClass(), DefinitionPath));
	if (!SODef)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Smart Object Definition not found: %s"), *DefinitionPath));
	}

	// Parse location
	const TSharedPtr<FJsonObject>* LocObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("location"), LocObj) || !LocObj || (*LocObj)->Values.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: location {x, y, z}"));
	}

	FVector Location(
		(*LocObj)->GetNumberField(TEXT("x")),
		(*LocObj)->GetNumberField(TEXT("y")),
		(*LocObj)->GetNumberField(TEXT("z"))
	);

	// Parse rotation (optional)
	FRotator Rotation = FRotator::ZeroRotator;
	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (Params->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj && (*RotObj)->Values.Num() > 0)
	{
		Rotation.Pitch = (*RotObj)->GetNumberField(TEXT("pitch"));
		Rotation.Yaw = (*RotObj)->GetNumberField(TEXT("yaw"));
		Rotation.Roll = (*RotObj)->GetNumberField(TEXT("roll"));
	}

	// Get the editor world
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Place Smart Object Actor")));

	// Spawn a basic actor
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* NewActor = World->SpawnActor(AActor::StaticClass(), &Location, &Rotation, SpawnParams);
	if (!NewActor)
	{
		return FMonolithActionResult::Error(TEXT("Failed to spawn actor"));
	}

	// Set label
	FString Label = FString::Printf(TEXT("SO_%s"), *SODef->GetName());
	NewActor->SetActorLabel(Label);

	// Add a scene root component (AActor has no root by default)
	USceneComponent* RootComp = NewObject<USceneComponent>(NewActor, TEXT("RootComponent"));
	RootComp->SetWorldLocation(Location);
	RootComp->SetWorldRotation(Rotation);
	NewActor->SetRootComponent(RootComp);
	RootComp->RegisterComponent();

	// Add SmartObject component
	USmartObjectComponent* SOComp = NewObject<USmartObjectComponent>(
		NewActor, USmartObjectComponent::StaticClass(), FName(TEXT("SmartObjectComponent")),
		RF_Transactional);
	SOComp->SetDefinition(SODef);
	SOComp->SetupAttachment(RootComp);
	SOComp->RegisterComponent();
	NewActor->AddInstanceComponent(SOComp);

	// Folder path — MUST organize in World Outliner
	FString FolderPath = Params->GetStringField(TEXT("folder_path"));
	if (FolderPath.IsEmpty())
	{
		FolderPath = TEXT("AI/SmartObjects");
	}
	NewActor->SetFolderPath(FName(*FolderPath));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), NewActor->GetActorNameOrLabel());
	Result->SetStringField(TEXT("actor_label"), Label);
	Result->SetStringField(TEXT("definition"), DefinitionPath);
	Result->SetStringField(TEXT("folder_path"), FolderPath);

	TSharedPtr<FJsonObject> LocResult = MakeShared<FJsonObject>();
	LocResult->SetNumberField(TEXT("x"), Location.X);
	LocResult->SetNumberField(TEXT("y"), Location.Y);
	LocResult->SetNumberField(TEXT("z"), Location.Z);
	Result->SetObjectField(TEXT("location"), LocResult);

	Result->SetStringField(TEXT("message"), TEXT("Smart Object actor placed"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  139. find_smart_objects_in_level
// ============================================================

FMonolithActionResult FMonolithAISmartObjectActions::HandleFindSmartObjectsInLevel(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FString LevelFilter = Params->GetStringField(TEXT("level"));
	FString DefinitionFilter = Params->GetStringField(TEXT("definition_filter"));

	FGameplayTagContainer TagFilter;
	TArray<FString> SkippedTagFilter;
	if (Params->HasField(TEXT("tag_filter")))
	{
		ParseTagContainerWithSkipped(Params, TEXT("tag_filter"), TagFilter, SkippedTagFilter);
	}

	TArray<TSharedPtr<FJsonValue>> Items;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;

		USmartObjectComponent* SOComp = Actor->FindComponentByClass<USmartObjectComponent>();
		if (!SOComp) continue;

		// Level filter
		if (!LevelFilter.IsEmpty())
		{
			FString ActorLevel = Actor->GetLevel()->GetOuter()->GetName();
			if (!ActorLevel.Contains(LevelFilter))
			{
				continue;
			}
		}

		// Definition filter
		const USmartObjectDefinition* Def = SOComp->GetDefinition();
		if (!DefinitionFilter.IsEmpty() && Def)
		{
			if (!Def->GetPathName().Contains(DefinitionFilter))
			{
				continue;
			}
		}

		// Tag filter
		if (!TagFilter.IsEmpty() && Def)
		{
			const FGameplayTagContainer& DefTags = Def->GetActivityTags();
			if (!DefTags.HasAny(TagFilter))
			{
				continue;
			}
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
		Item->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());

		FVector Loc = Actor->GetActorLocation();
		TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
		LocObj->SetNumberField(TEXT("x"), Loc.X);
		LocObj->SetNumberField(TEXT("y"), Loc.Y);
		LocObj->SetNumberField(TEXT("z"), Loc.Z);
		Item->SetObjectField(TEXT("location"), LocObj);

		if (Def)
		{
			Item->SetStringField(TEXT("definition"), Def->GetPathName());
			Item->SetNumberField(TEXT("slot_count"), Def->GetSlots().Num());
		}

		FString Folder = Actor->GetFolderPath().ToString();
		if (!Folder.IsEmpty())
		{
			Item->SetStringField(TEXT("folder_path"), Folder);
		}

		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("smart_objects"), Items);
	Result->SetNumberField(TEXT("count"), Items.Num());

	TArray<TSharedPtr<FJsonValue>> Warnings;
	AppendTagWarnings(Warnings, TEXT("tag_filter"), SkippedTagFilter);
	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  140. validate_smart_object_definition
// ============================================================

FMonolithActionResult FMonolithAISmartObjectActions::HandleValidateSmartObjectDefinition(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	USmartObjectDefinition* Def = LoadSODefinition(Params, AssetPath, Error);
	if (!Def)
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TPair<EMessageSeverity::Type, FText>> ValidationErrors;
	bool bIsValid = Def->Validate(&ValidationErrors);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetBoolField(TEXT("valid"), bIsValid);

	// Custom validation on top of engine validation
	TArray<TSharedPtr<FJsonValue>> Issues;

	// Engine validation errors
	for (const auto& Err : ValidationErrors)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		switch (Err.Key)
		{
		case EMessageSeverity::Error:
			Issue->SetStringField(TEXT("severity"), TEXT("error"));
			break;
		case EMessageSeverity::Warning:
			Issue->SetStringField(TEXT("severity"), TEXT("warning"));
			break;
		default:
			Issue->SetStringField(TEXT("severity"), TEXT("info"));
			break;
		}
		Issue->SetStringField(TEXT("message"), Err.Value.ToString());
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	}

	// Additional checks
	TConstArrayView<FSmartObjectSlotDefinition> Slots = Def->GetSlots();

	if (Slots.Num() == 0)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), TEXT("warning"));
		Issue->SetStringField(TEXT("message"), TEXT("Definition has no slots"));
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	}

	for (int32 i = 0; i < Slots.Num(); ++i)
	{
		const FSmartObjectSlotDefinition& Slot = Slots[i];
		if (Slot.BehaviorDefinitions.Num() == 0)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("warning"));
			Issue->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Slot %d has no behavior definitions"), i));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}

		if (Slot.ActivityTags.IsEmpty() && Def->GetActivityTags().IsEmpty())
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("info"));
			Issue->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Slot %d has no activity tags (and no definition-level tags)"), i));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}
	}

	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());
	Result->SetNumberField(TEXT("slot_count"), Slots.Num());

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  141. create_so_from_template
// ============================================================

FMonolithActionResult FMonolithAISmartObjectActions::HandleCreateSOFromTemplate(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult))
	{
		return ErrResult;
	}

	FString TemplateName;
	if (!MonolithAI::RequireStringParam(Params, TEXT("template"), TemplateName, ErrResult))
	{
		return ErrResult;
	}

	TemplateName = TemplateName.ToLower();

	// Validate template name
	static const TArray<FString> ValidTemplates = {
		TEXT("hide_spot"), TEXT("sit_chair"), TEXT("workstation"),
		TEXT("door_interaction"), TEXT("pickup_item")
	};

	if (!ValidTemplates.Contains(TemplateName))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown template: '%s'. Valid templates: hide_spot, sit_chair, workstation, door_interaction, pickup_item"),
			*TemplateName));
	}

	FString AssetName = FPackageName::GetShortName(SavePath);
	FString PackagePath = SavePath;

	FString PathError;
	if (!MonolithAI::EnsureAssetPathFree(PackagePath, AssetName, PathError))
	{
		return FMonolithActionResult::Error(PathError);
	}

	FString PkgError;
	UPackage* Package = MonolithAI::GetOrCreatePackage(PackagePath, PkgError);
	if (!Package)
	{
		return FMonolithActionResult::Error(PkgError);
	}

	USmartObjectDefinition* Def = NewObject<USmartObjectDefinition>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!Def)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create USmartObjectDefinition"));
	}

	// Configure based on template
	auto AddSlotWithTags = [&](const FVector3f& Offset, const FRotator3f& Rot, const TArray<FString>& TagNames) -> FSmartObjectSlotDefinition&
	{
		FSmartObjectSlotDefinition& Slot = Def->DebugAddSlot();
		Slot.Offset = Offset;
		Slot.Rotation = Rot;
#if WITH_EDITORONLY_DATA
		Slot.ID = FGuid::NewGuid();
#endif
		for (const FString& TagName : TagNames)
		{
			FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagName), /*bErrorIfNotFound=*/false);
			if (Tag.IsValid())
			{
				Slot.ActivityTags.AddTag(Tag);
			}
		}
		return Slot;
	};

	if (TemplateName == TEXT("hide_spot"))
	{
		// Single slot at origin, crouched/hidden
		FGameplayTagContainer ActivityTags;
		FGameplayTag HideTag = FGameplayTag::RequestGameplayTag(FName(TEXT("SmartObject.Activity.Hide")), false);
		if (HideTag.IsValid()) ActivityTags.AddTag(HideTag);
		Def->SetActivityTags(ActivityTags);

		AddSlotWithTags(FVector3f::ZeroVector, FRotator3f::ZeroRotator,
			{TEXT("SmartObject.Activity.Hide")});
	}
	else if (TemplateName == TEXT("sit_chair"))
	{
		// Single slot, slightly offset for sitting
		FGameplayTagContainer ActivityTags;
		FGameplayTag SitTag = FGameplayTag::RequestGameplayTag(FName(TEXT("SmartObject.Activity.Sit")), false);
		if (SitTag.IsValid()) ActivityTags.AddTag(SitTag);
		Def->SetActivityTags(ActivityTags);

		AddSlotWithTags(FVector3f(0.f, 0.f, 45.f), FRotator3f::ZeroRotator,
			{TEXT("SmartObject.Activity.Sit")});
	}
	else if (TemplateName == TEXT("workstation"))
	{
		// Two slots: one for the worker, one for an observer
		FGameplayTagContainer ActivityTags;
		FGameplayTag WorkTag = FGameplayTag::RequestGameplayTag(FName(TEXT("SmartObject.Activity.Work")), false);
		if (WorkTag.IsValid()) ActivityTags.AddTag(WorkTag);
		Def->SetActivityTags(ActivityTags);

		AddSlotWithTags(FVector3f(0.f, 0.f, 0.f), FRotator3f::ZeroRotator,
			{TEXT("SmartObject.Activity.Work")});

		AddSlotWithTags(FVector3f(0.f, 100.f, 0.f), FRotator3f(0.f, 180.f, 0.f),
			{TEXT("SmartObject.Activity.Work")});
	}
	else if (TemplateName == TEXT("door_interaction"))
	{
		// Two slots on either side of a door
		FGameplayTagContainer ActivityTags;
		FGameplayTag UseTag = FGameplayTag::RequestGameplayTag(FName(TEXT("SmartObject.Activity.Use")), false);
		if (UseTag.IsValid()) ActivityTags.AddTag(UseTag);
		Def->SetActivityTags(ActivityTags);

		AddSlotWithTags(FVector3f(80.f, 0.f, 0.f), FRotator3f(0.f, 180.f, 0.f),
			{TEXT("SmartObject.Activity.Use")});

		AddSlotWithTags(FVector3f(-80.f, 0.f, 0.f), FRotator3f::ZeroRotator,
			{TEXT("SmartObject.Activity.Use")});
	}
	else if (TemplateName == TEXT("pickup_item"))
	{
		// Single slot at ground level
		FGameplayTagContainer ActivityTags;
		FGameplayTag PickupTag = FGameplayTag::RequestGameplayTag(FName(TEXT("SmartObject.Activity.Pickup")), false);
		if (PickupTag.IsValid()) ActivityTags.AddTag(PickupTag);
		Def->SetActivityTags(ActivityTags);

		AddSlotWithTags(FVector3f::ZeroVector, FRotator3f::ZeroRotator,
			{TEXT("SmartObject.Activity.Pickup")});
	}

	FAssetRegistryModule::AssetCreated(Def);
	Def->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(PackagePath,
		FString::Printf(TEXT("Smart Object created from template '%s'"), *TemplateName));
	Result->SetStringField(TEXT("name"), AssetName);
	Result->SetStringField(TEXT("template"), TemplateName);
	Result->SetNumberField(TEXT("slot_count"), Def->GetSlots().Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  142. duplicate_smart_object_definition
// ============================================================

FMonolithActionResult FMonolithAISmartObjectActions::HandleDuplicateSmartObjectDefinition(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath, DestPath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("source_path"), SourcePath, ErrResult))
	{
		return ErrResult;
	}
	if (!MonolithAI::RequireStringParam(Params, TEXT("dest_path"), DestPath, ErrResult))
	{
		return ErrResult;
	}

	USmartObjectDefinition* SourceDef = Cast<USmartObjectDefinition>(
		FMonolithAssetUtils::LoadAssetByPath(USmartObjectDefinition::StaticClass(), SourcePath));
	if (!SourceDef)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Source definition not found: %s"), *SourcePath));
	}

	FString DestAssetName = FPackageName::GetShortName(DestPath);
	FString PkgError;
	UPackage* DestPackage = MonolithAI::GetOrCreatePackage(DestPath, PkgError);
	if (!DestPackage)
	{
		return FMonolithActionResult::Error(PkgError);
	}

	USmartObjectDefinition* NewDef = Cast<USmartObjectDefinition>(
		StaticDuplicateObject(SourceDef, DestPackage, *DestAssetName));
	if (!NewDef)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to duplicate '%s' to '%s'"), *SourcePath, *DestPath));
	}

	NewDef->SetFlags(RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(NewDef);
	NewDef->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source_path"), SourcePath);
	Result->SetStringField(TEXT("asset_path"), DestPath);
	Result->SetStringField(TEXT("name"), NewDef->GetName());
	Result->SetNumberField(TEXT("slot_count"), NewDef->GetSlots().Num());
	Result->SetStringField(TEXT("message"), TEXT("Smart Object Definition duplicated"));
	return FMonolithActionResult::Success(Result);
}

#endif // WITH_SMARTOBJECTS
