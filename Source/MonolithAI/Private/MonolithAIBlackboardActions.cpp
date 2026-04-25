#include "MonolithAIBlackboardActions.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_NativeEnum.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "ScopedTransaction.h"

// ============================================================
//  Helpers
// ============================================================

namespace
{
	/** Get a string for the key type class (human-readable) */
	FString KeyTypeToString(const UBlackboardKeyType* KeyType)
	{
		if (!KeyType) return TEXT("None");

		if (KeyType->IsA<UBlackboardKeyType_Bool>())     return TEXT("Bool");
		if (KeyType->IsA<UBlackboardKeyType_Int>())      return TEXT("Int");
		if (KeyType->IsA<UBlackboardKeyType_Float>())    return TEXT("Float");
		if (KeyType->IsA<UBlackboardKeyType_String>())   return TEXT("String");
		if (KeyType->IsA<UBlackboardKeyType_Name>())     return TEXT("Name");
		if (KeyType->IsA<UBlackboardKeyType_Vector>())   return TEXT("Vector");
		if (KeyType->IsA<UBlackboardKeyType_Rotator>())  return TEXT("Rotator");
		if (KeyType->IsA<UBlackboardKeyType_Object>())   return TEXT("Object");
		if (KeyType->IsA<UBlackboardKeyType_Class>())    return TEXT("Class");
		if (KeyType->IsA<UBlackboardKeyType_Enum>())     return TEXT("Enum");
		if (KeyType->IsA<UBlackboardKeyType_NativeEnum>()) return TEXT("NativeEnum");

		return KeyType->GetClass()->GetName();
	}

	/** Create a UBlackboardKeyType subobject from a type string. Returns nullptr on unknown type. */
	UBlackboardKeyType* CreateKeyTypeFromString(UBlackboardData* Outer, const FString& TypeStr, const TSharedPtr<FJsonObject>& ExtraParams, FString& OutError)
	{
		UBlackboardKeyType* KeyType = nullptr;

		if (TypeStr == TEXT("Bool"))
		{
			KeyType = NewObject<UBlackboardKeyType_Bool>(Outer);
		}
		else if (TypeStr == TEXT("Int"))
		{
			KeyType = NewObject<UBlackboardKeyType_Int>(Outer);
		}
		else if (TypeStr == TEXT("Float"))
		{
			KeyType = NewObject<UBlackboardKeyType_Float>(Outer);
		}
		else if (TypeStr == TEXT("String"))
		{
			KeyType = NewObject<UBlackboardKeyType_String>(Outer);
		}
		else if (TypeStr == TEXT("Name"))
		{
			KeyType = NewObject<UBlackboardKeyType_Name>(Outer);
		}
		else if (TypeStr == TEXT("Vector"))
		{
			KeyType = NewObject<UBlackboardKeyType_Vector>(Outer);
		}
		else if (TypeStr == TEXT("Rotator"))
		{
			KeyType = NewObject<UBlackboardKeyType_Rotator>(Outer);
		}
		else if (TypeStr == TEXT("Object"))
		{
			UBlackboardKeyType_Object* ObjKey = NewObject<UBlackboardKeyType_Object>(Outer);
			// Set base class filter if provided
			if (ExtraParams.IsValid())
			{
				FString BaseClassName = ExtraParams->GetStringField(TEXT("base_class"));
				if (!BaseClassName.IsEmpty())
				{
					UClass* BaseClass = FindFirstObject<UClass>(*BaseClassName, EFindFirstObjectOptions::EnsureIfAmbiguous);
					if (!BaseClass)
					{
						// Try loading it as a full path
						BaseClass = LoadObject<UClass>(nullptr, *BaseClassName);
					}
					if (BaseClass)
					{
						ObjKey->BaseClass = BaseClass;
					}
					else
					{
						UE_LOG(LogMonolithAI, Warning, TEXT("Could not find base_class '%s', defaulting to UObject"), *BaseClassName);
					}
				}
			}
			if (!ObjKey->BaseClass)
			{
				ObjKey->BaseClass = UObject::StaticClass();
			}
			KeyType = ObjKey;
		}
		else if (TypeStr == TEXT("Class"))
		{
			UBlackboardKeyType_Class* ClassKey = NewObject<UBlackboardKeyType_Class>(Outer);
			if (ExtraParams.IsValid())
			{
				FString BaseClassName = ExtraParams->GetStringField(TEXT("base_class"));
				if (!BaseClassName.IsEmpty())
				{
					UClass* BaseClass = FindFirstObject<UClass>(*BaseClassName, EFindFirstObjectOptions::EnsureIfAmbiguous);
					if (!BaseClass)
					{
						BaseClass = LoadObject<UClass>(nullptr, *BaseClassName);
					}
					if (BaseClass)
					{
						ClassKey->BaseClass = BaseClass;
					}
				}
			}
			if (!ClassKey->BaseClass)
			{
				ClassKey->BaseClass = UObject::StaticClass();
			}
			KeyType = ClassKey;
		}
		else if (TypeStr == TEXT("Enum"))
		{
			UBlackboardKeyType_Enum* EnumKey = NewObject<UBlackboardKeyType_Enum>(Outer);
			if (ExtraParams.IsValid())
			{
				FString EnumName = ExtraParams->GetStringField(TEXT("enum_type"));
				if (!EnumName.IsEmpty())
				{
					UEnum* FoundEnum = FindFirstObject<UEnum>(*EnumName, EFindFirstObjectOptions::EnsureIfAmbiguous);
					if (FoundEnum)
					{
						EnumKey->EnumType = FoundEnum;
						EnumKey->EnumName = EnumName;
						EnumKey->bIsEnumNameValid = 1;
					}
					else
					{
						// Still store the name — editor can resolve later
						EnumKey->EnumName = EnumName;
						UE_LOG(LogMonolithAI, Warning, TEXT("Could not find enum '%s', storing name for later resolution"), *EnumName);
					}
				}
			}
			KeyType = EnumKey;
		}
		else if (TypeStr == TEXT("NativeEnum"))
		{
			UBlackboardKeyType_NativeEnum* NativeEnumKey = NewObject<UBlackboardKeyType_NativeEnum>(Outer);
			if (ExtraParams.IsValid())
			{
				FString EnumName = ExtraParams->GetStringField(TEXT("enum_type"));
				if (!EnumName.IsEmpty())
				{
					NativeEnumKey->EnumName = EnumName;
					UEnum* FoundEnum = FindFirstObject<UEnum>(*EnumName, EFindFirstObjectOptions::EnsureIfAmbiguous);
					if (FoundEnum)
					{
						NativeEnumKey->EnumType = FoundEnum;
					}
				}
			}
			KeyType = NativeEnumKey;
		}
		else
		{
			OutError = FString::Printf(TEXT("Unknown key type: '%s'. Valid types: Bool, Int, Float, String, Name, Vector, Rotator, Object, Class, Enum, NativeEnum"), *TypeStr);
		}

		return KeyType;
	}

	/** Serialize a single FBlackboardEntry to JSON */
	TSharedPtr<FJsonObject> EntryToJson(const FBlackboardEntry& Entry, bool bIsInherited = false)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("key_name"), Entry.EntryName.ToString());
		Obj->SetStringField(TEXT("key_type"), KeyTypeToString(Entry.KeyType));
		Obj->SetBoolField(TEXT("instance_synced"), !!Entry.bInstanceSynced);
		Obj->SetBoolField(TEXT("inherited"), bIsInherited);

#if WITH_EDITORONLY_DATA
		if (!Entry.EntryDescription.IsEmpty())
		{
			Obj->SetStringField(TEXT("description"), Entry.EntryDescription);
		}
		if (!Entry.EntryCategory.IsNone())
		{
			Obj->SetStringField(TEXT("category"), Entry.EntryCategory.ToString());
		}
#endif

		// Extra info for typed keys
		if (const UBlackboardKeyType_Object* ObjKey = Cast<UBlackboardKeyType_Object>(Entry.KeyType))
		{
			if (ObjKey->BaseClass)
			{
				Obj->SetStringField(TEXT("base_class"), ObjKey->BaseClass->GetPathName());
			}
		}
		else if (const UBlackboardKeyType_Class* ClassKey = Cast<UBlackboardKeyType_Class>(Entry.KeyType))
		{
			if (ClassKey->BaseClass)
			{
				Obj->SetStringField(TEXT("base_class"), ClassKey->BaseClass->GetPathName());
			}
		}
		else if (const UBlackboardKeyType_Enum* EnumKey = Cast<UBlackboardKeyType_Enum>(Entry.KeyType))
		{
			if (EnumKey->EnumType)
			{
				Obj->SetStringField(TEXT("enum_type"), EnumKey->EnumType->GetPathName());
			}
			if (!EnumKey->EnumName.IsEmpty())
			{
				Obj->SetStringField(TEXT("enum_name"), EnumKey->EnumName);
			}
		}
		else if (const UBlackboardKeyType_NativeEnum* NativeEnumKey = Cast<UBlackboardKeyType_NativeEnum>(Entry.KeyType))
		{
			if (!NativeEnumKey->EnumName.IsEmpty())
			{
				Obj->SetStringField(TEXT("enum_name"), NativeEnumKey->EnumName);
			}
		}

		return Obj;
	}

	/** Find a key entry index in the BB's own Keys array by name */
	int32 FindKeyIndex(const UBlackboardData* BB, const FName& KeyName)
	{
		for (int32 i = 0; i < BB->Keys.Num(); ++i)
		{
			if (BB->Keys[i].EntryName == KeyName)
			{
				return i;
			}
		}
		return INDEX_NONE;
	}

	/** Check if a key name exists in parent chain */
	bool IsKeyInParentChain(const UBlackboardData* BB, const FName& KeyName)
	{
		const UBlackboardData* Current = BB->Parent;
		while (Current)
		{
			for (const FBlackboardEntry& Entry : Current->Keys)
			{
				if (Entry.EntryName == KeyName)
				{
					return true;
				}
			}
			Current = Current->Parent;
		}
		return false;
	}
}

// ============================================================
//  Registration
// ============================================================

void FMonolithAIBlackboardActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 1. create_blackboard
	Registry.RegisterAction(TEXT("ai"), TEXT("create_blackboard"),
		TEXT("Create a new Blackboard Data asset"),
		FMonolithActionHandler::CreateStatic(&HandleCreateBlackboard),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path (e.g. /Game/AI/BB_Enemy)"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Asset name (derived from save_path if omitted)"))
			.Optional(TEXT("parent_bb"), TEXT("string"), TEXT("Parent blackboard asset path for key inheritance"))
			.Build());

	// 2. get_blackboard
	Registry.RegisterAction(TEXT("ai"), TEXT("get_blackboard"),
		TEXT("Full JSON dump of all blackboard keys with types; inherited keys marked"),
		FMonolithActionHandler::CreateStatic(&HandleGetBlackboard),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blackboard asset path"))
			.Build());

	// 3. list_blackboards
	Registry.RegisterAction(TEXT("ai"), TEXT("list_blackboards"),
		TEXT("List all UBlackboardData assets in the project"),
		FMonolithActionHandler::CreateStatic(&HandleListBlackboards),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Only include assets under this path prefix"))
			.Build());

	// 4. delete_blackboard
	Registry.RegisterAction(TEXT("ai"), TEXT("delete_blackboard"),
		TEXT("Delete a Blackboard Data asset"),
		FMonolithActionHandler::CreateStatic(&HandleDeleteBlackboard),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blackboard asset path to delete"))
			.Build());

	// 5. duplicate_blackboard
	Registry.RegisterAction(TEXT("ai"), TEXT("duplicate_blackboard"),
		TEXT("Deep copy a Blackboard Data asset to a new path"),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateBlackboard),
		FParamSchemaBuilder()
			.Required(TEXT("source_path"), TEXT("string"), TEXT("Source blackboard asset path"))
			.Required(TEXT("dest_path"), TEXT("string"), TEXT("Destination asset path for the copy"))
			.Build());

	// 6. add_bb_key
	Registry.RegisterAction(TEXT("ai"), TEXT("add_bb_key"),
		TEXT("Add a key to a Blackboard (Bool/Int/Float/String/Name/Vector/Rotator/Object/Class/Enum/NativeEnum)"),
		FMonolithActionHandler::CreateStatic(&HandleAddBBKey),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blackboard asset path"))
			.Required(TEXT("key_name"), TEXT("string"), TEXT("Name for the new key"))
			.Required(TEXT("key_type"), TEXT("string"), TEXT("Key type: Bool, Int, Float, String, Name, Vector, Rotator, Object, Class, Enum, NativeEnum"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Optional description for the key"))
			.Optional(TEXT("base_class"), TEXT("string"), TEXT("Base class filter for Object/Class key types"))
			.Optional(TEXT("enum_type"), TEXT("string"), TEXT("Enum type name for Enum/NativeEnum key types"))
			.Optional(TEXT("instance_synced"), TEXT("boolean"), TEXT("If true, key is synced across all blackboard instances"), TEXT("false"))
			.Build());

	// 7. remove_bb_key
	Registry.RegisterAction(TEXT("ai"), TEXT("remove_bb_key"),
		TEXT("Remove a key from a Blackboard"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveBBKey),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blackboard asset path"))
			.Required(TEXT("key_name"), TEXT("string"), TEXT("Key name to remove"))
			.Build());

	// 8. rename_bb_key
	Registry.RegisterAction(TEXT("ai"), TEXT("rename_bb_key"),
		TEXT("Rename a key in a Blackboard"),
		FMonolithActionHandler::CreateStatic(&HandleRenameBBKey),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blackboard asset path"))
			.Required(TEXT("old_name"), TEXT("string"), TEXT("Current key name"))
			.Required(TEXT("new_name"), TEXT("string"), TEXT("New key name"))
			.Build());

	// 9. get_bb_key_details
	Registry.RegisterAction(TEXT("ai"), TEXT("get_bb_key_details"),
		TEXT("Detailed info for a single blackboard key (type, base class filter, allowed types, etc.)"),
		FMonolithActionHandler::CreateStatic(&HandleGetBBKeyDetails),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blackboard asset path"))
			.Required(TEXT("key_name"), TEXT("string"), TEXT("Key name to inspect"))
			.Build());

	// 10. batch_add_bb_keys
	Registry.RegisterAction(TEXT("ai"), TEXT("batch_add_bb_keys"),
		TEXT("Add multiple keys to a Blackboard at once"),
		FMonolithActionHandler::CreateStatic(&HandleBatchAddBBKeys),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blackboard asset path"))
			.Required(TEXT("keys"), TEXT("array"), TEXT("Array of key objects, each with: name (string), type (string), description? (string), base_class? (string), enum_type? (string)"))
			.Build());

	// 11. set_bb_parent
	Registry.RegisterAction(TEXT("ai"), TEXT("set_bb_parent"),
		TEXT("Set or change the parent Blackboard for key inheritance"),
		FMonolithActionHandler::CreateStatic(&HandleSetBBParent),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blackboard asset path"))
			.Required(TEXT("parent_path"), TEXT("string"), TEXT("Parent blackboard path (empty string to clear parent)"))
			.Build());

	// 12. compare_blackboards
	Registry.RegisterAction(TEXT("ai"), TEXT("compare_blackboards"),
		TEXT("Diff two Blackboards: added, removed, and changed keys"),
		FMonolithActionHandler::CreateStatic(&HandleCompareBlackboards),
		FParamSchemaBuilder()
			.Required(TEXT("path_a"), TEXT("string"), TEXT("First blackboard asset path"))
			.Required(TEXT("path_b"), TEXT("string"), TEXT("Second blackboard asset path"))
			.Build());
}

// ============================================================
//  1. create_blackboard
// ============================================================

FMonolithActionResult FMonolithAIBlackboardActions::HandleCreateBlackboard(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, ErrResult))
	{
		return ErrResult;
	}

	// Derive asset name from path or explicit param
	FString AssetName = Params->GetStringField(TEXT("name"));
	if (AssetName.IsEmpty())
	{
		AssetName = FPackageName::GetShortName(SavePath);
	}

	// Normalize save_path to package path (strip asset name portion if included)
	FString PackagePath = SavePath;
	if (FPackageName::GetShortName(PackagePath) == AssetName)
	{
		// save_path includes the asset name, use it as-is for the package
	}

	// Check path is free
	FString PathError;
	if (!MonolithAI::EnsureAssetPathFree(PackagePath, AssetName, PathError))
	{
		return FMonolithActionResult::Error(PathError);
	}

	// Create package
	FString PkgError;
	UPackage* Package = MonolithAI::GetOrCreatePackage(PackagePath, PkgError);
	if (!Package)
	{
		return FMonolithActionResult::Error(PkgError);
	}

	// Create the BB asset
	UBlackboardData* BB = NewObject<UBlackboardData>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!BB)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UBlackboardData object"));
	}

	// Set parent if specified
	FString ParentPath = Params->GetStringField(TEXT("parent_bb"));
	if (!ParentPath.IsEmpty())
	{
		UObject* ParentObj = FMonolithAssetUtils::LoadAssetByPath(UBlackboardData::StaticClass(), ParentPath);
		UBlackboardData* ParentBB = Cast<UBlackboardData>(ParentObj);
		if (!ParentBB)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Parent blackboard not found: %s"), *ParentPath));
		}
		BB->Parent = ParentBB;
		BB->UpdateParentKeys();
		BB->UpdateKeyIDs();
	}

	// Notify asset registry and mark dirty
	FAssetRegistryModule::AssetCreated(BB);
	BB->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(PackagePath, TEXT("Blackboard created"));
	Result->SetStringField(TEXT("name"), AssetName);
	if (BB->Parent)
	{
		Result->SetStringField(TEXT("parent"), BB->Parent->GetPathName());
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  2. get_blackboard
// ============================================================

FMonolithActionResult FMonolithAIBlackboardActions::HandleGetBlackboard(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBlackboardData* BB = MonolithAI::LoadBlackboardFromParams(Params, AssetPath, Error);
	if (!BB)
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("name"), BB->GetName());

	// Parent
	if (BB->Parent)
	{
		Result->SetStringField(TEXT("parent"), BB->Parent->GetPathName());
	}

	// Collect inherited keys from parent chain
	TArray<TSharedPtr<FJsonValue>> AllKeys;

#if WITH_EDITORONLY_DATA
	BB->UpdateParentKeys();
	for (const FBlackboardEntry& Entry : BB->ParentKeys)
	{
		AllKeys.Add(MakeShared<FJsonValueObject>(EntryToJson(Entry, /*bIsInherited=*/true)));
	}
#else
	// Without editor data, walk parent chain manually
	const UBlackboardData* Current = BB->Parent;
	while (Current)
	{
		for (const FBlackboardEntry& Entry : Current->Keys)
		{
			AllKeys.Add(MakeShared<FJsonValueObject>(EntryToJson(Entry, /*bIsInherited=*/true)));
		}
		Current = Current->Parent;
	}
#endif

	// Own keys
	for (const FBlackboardEntry& Entry : BB->Keys)
	{
		AllKeys.Add(MakeShared<FJsonValueObject>(EntryToJson(Entry, /*bIsInherited=*/false)));
	}

	Result->SetArrayField(TEXT("keys"), AllKeys);
	Result->SetNumberField(TEXT("own_key_count"), BB->Keys.Num());
	Result->SetNumberField(TEXT("total_key_count"), AllKeys.Num());

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  3. list_blackboards
// ============================================================

FMonolithActionResult FMonolithAIBlackboardActions::HandleListBlackboards(const TSharedPtr<FJsonObject>& Params)
{
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetData> Assets;
	AR.GetAssetsByClass(UBlackboardData::StaticClass()->GetClassPathName(), Assets);

	FString PathFilter = Params->GetStringField(TEXT("path_filter"));

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FAssetData& Asset : Assets)
	{
		FString AssetPath = Asset.GetObjectPathString();
		if (!PathFilter.IsEmpty() && !AssetPath.StartsWith(PathFilter))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("asset_path"), Asset.PackageName.ToString());
		Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());

		// Check for parent
		FAssetTagValueRef ParentTag = Asset.TagsAndValues.FindTag(FName(TEXT("Parent")));
		if (ParentTag.IsSet())
		{
			Item->SetStringField(TEXT("parent"), ParentTag.GetValue());
		}

		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("blackboards"), Items);
	Result->SetNumberField(TEXT("count"), Items.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  4. delete_blackboard
// ============================================================

FMonolithActionResult FMonolithAIBlackboardActions::HandleDeleteBlackboard(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("asset_path"), AssetPath, ErrResult))
	{
		return ErrResult;
	}

	// Load the asset to delete
	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(UBlackboardData::StaticClass(), AssetPath);
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
	Result->SetStringField(TEXT("message"), TEXT("Blackboard deleted"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  5. duplicate_blackboard
// ============================================================

FMonolithActionResult FMonolithAIBlackboardActions::HandleDuplicateBlackboard(const TSharedPtr<FJsonObject>& Params)
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

	bool bOverwrite = false;
	Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);

	// Load source
	UBlackboardData* SourceBB = Cast<UBlackboardData>(FMonolithAssetUtils::LoadAssetByPath(UBlackboardData::StaticClass(), SourcePath));
	if (!SourceBB)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source blackboard not found: %s"), *SourcePath));
	}

	// Refuse silent overwrite. EnsureAssetPathFree returns false when the asset already exists.
	FString DestAssetName = FPackageName::GetShortName(DestPath);
	FString PathError;
	const bool bDestFree = MonolithAI::EnsureAssetPathFree(DestPath, DestAssetName, PathError);
	if (!bDestFree && !bOverwrite)
	{
		const FString FullObjectPath = DestPath + TEXT(".") + DestAssetName;
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Destination already exists at '%s'. Pass overwrite=true to replace, or choose a different dest_path. (existing asset: %s)"),
			*DestPath, *FullObjectPath));
	}

	// Create dest package
	FString PkgError;
	UPackage* DestPackage = MonolithAI::GetOrCreatePackage(DestPath, PkgError);
	if (!DestPackage)
	{
		return FMonolithActionResult::Error(PkgError);
	}

	// Snapshot source key names for post-copy diagnostic.
	const int32 SourceKeyCount = SourceBB->Keys.Num();

	// Deep duplicate
	UBlackboardData* NewBB = Cast<UBlackboardData>(
		StaticDuplicateObject(SourceBB, DestPackage, *DestAssetName));
	if (!NewBB)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to duplicate '%s' to '%s'"), *SourcePath, *DestPath));
	}

	NewBB->SetFlags(RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(NewBB);
	NewBB->MarkPackageDirty();

	// Best-effort skipped_keys diagnostic: any source key not present on the destination
	// (e.g. KeyType class unloaded or invalid). StaticDuplicateObject is atomic, so
	// individual key drops only happen in pathological cases — we still report them.
	TArray<TSharedPtr<FJsonValue>> SkippedKeys;
	{
		TSet<FName> DestNames;
		for (const FBlackboardEntry& E : NewBB->Keys)
		{
			DestNames.Add(E.EntryName);
		}
		for (const FBlackboardEntry& E : SourceBB->Keys)
		{
			if (!DestNames.Contains(E.EntryName))
			{
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("key_name"), E.EntryName.ToString());
				Obj->SetStringField(TEXT("reason"), TEXT("Key present on source but missing after duplication (KeyType class may be unloaded or invalid)"));
				SkippedKeys.Add(MakeShared<FJsonValueObject>(Obj));
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source_path"), SourcePath);
	Result->SetStringField(TEXT("asset_path"), DestPath);
	Result->SetStringField(TEXT("name"), NewBB->GetName());
	Result->SetNumberField(TEXT("key_count"), NewBB->Keys.Num());
	Result->SetNumberField(TEXT("source_key_count"), SourceKeyCount);
	Result->SetBoolField(TEXT("overwrite"), bOverwrite);
	Result->SetBoolField(TEXT("destination_existed"), !bDestFree);
	Result->SetArrayField(TEXT("skipped_keys"), SkippedKeys);
	Result->SetStringField(TEXT("message"),
		(!bDestFree && bOverwrite)
			? TEXT("Blackboard duplicated (overwrote existing destination)")
			: TEXT("Blackboard duplicated"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  6. add_bb_key
// ============================================================

FMonolithActionResult FMonolithAIBlackboardActions::HandleAddBBKey(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBlackboardData* BB = MonolithAI::LoadBlackboardFromParams(Params, AssetPath, Error);
	if (!BB)
	{
		return FMonolithActionResult::Error(Error);
	}

	FString KeyName;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("key_name"), KeyName, ErrResult))
	{
		return ErrResult;
	}

	FString KeyTypeStr;
	if (!MonolithAI::RequireStringParam(Params, TEXT("key_type"), KeyTypeStr, ErrResult))
	{
		return ErrResult;
	}

	// Check for duplicate key name (own keys + parent chain)
	FName KeyFName(*KeyName);
	if (FindKeyIndex(BB, KeyFName) != INDEX_NONE)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Key '%s' already exists in this blackboard"), *KeyName));
	}
	if (IsKeyInParentChain(BB, KeyFName))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Key '%s' already exists in parent blackboard chain"), *KeyName));
	}

	// Create the key type
	FString TypeError;
	UBlackboardKeyType* KeyType = CreateKeyTypeFromString(BB, KeyTypeStr, Params, TypeError);
	if (!KeyType)
	{
		return FMonolithActionResult::Error(TypeError);
	}

	// Build entry
	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add BB Key")));
	BB->Modify();

	FBlackboardEntry NewEntry;
	NewEntry.EntryName = KeyFName;
	NewEntry.KeyType = KeyType;

#if WITH_EDITORONLY_DATA
	FString Description = Params->GetStringField(TEXT("description"));
	if (!Description.IsEmpty())
	{
		NewEntry.EntryDescription = Description;
	}
#endif

	// Instance synced
	bool bInstanceSynced = false;
	if (Params->TryGetBoolField(TEXT("instance_synced"), bInstanceSynced))
	{
		NewEntry.bInstanceSynced = bInstanceSynced ? 1 : 0;
	}

	BB->Keys.Add(NewEntry);
	BB->UpdateKeyIDs();
	BB->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(AssetPath, FString::Printf(TEXT("Key '%s' (%s) added"), *KeyName, *KeyTypeStr));
	Result->SetNumberField(TEXT("key_count"), BB->Keys.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  7. remove_bb_key
// ============================================================

FMonolithActionResult FMonolithAIBlackboardActions::HandleRemoveBBKey(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBlackboardData* BB = MonolithAI::LoadBlackboardFromParams(Params, AssetPath, Error);
	if (!BB)
	{
		return FMonolithActionResult::Error(Error);
	}

	FString KeyName;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("key_name"), KeyName, ErrResult))
	{
		return ErrResult;
	}

	FName KeyFName(*KeyName);
	int32 Idx = FindKeyIndex(BB, KeyFName);
	if (Idx == INDEX_NONE)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Key '%s' not found in blackboard (not in own keys — inherited keys cannot be removed from child)"), *KeyName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Remove BB Key")));
	BB->Modify();
	BB->Keys.RemoveAt(Idx);
	BB->UpdateKeyIDs();
	BB->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(AssetPath, FString::Printf(TEXT("Key '%s' removed"), *KeyName));
	Result->SetNumberField(TEXT("key_count"), BB->Keys.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  8. rename_bb_key
// ============================================================

FMonolithActionResult FMonolithAIBlackboardActions::HandleRenameBBKey(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBlackboardData* BB = MonolithAI::LoadBlackboardFromParams(Params, AssetPath, Error);
	if (!BB)
	{
		return FMonolithActionResult::Error(Error);
	}

	FString OldName, NewName;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("old_name"), OldName, ErrResult))
	{
		return ErrResult;
	}
	if (!MonolithAI::RequireStringParam(Params, TEXT("new_name"), NewName, ErrResult))
	{
		return ErrResult;
	}

	FName OldFName(*OldName);
	FName NewFName(*NewName);

	int32 Idx = FindKeyIndex(BB, OldFName);
	if (Idx == INDEX_NONE)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Key '%s' not found"), *OldName));
	}

	// Check new name doesn't conflict
	if (FindKeyIndex(BB, NewFName) != INDEX_NONE)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Key '%s' already exists"), *NewName));
	}
	if (IsKeyInParentChain(BB, NewFName))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Key '%s' conflicts with inherited key"), *NewName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Rename BB Key")));
	BB->Modify();
	BB->Keys[Idx].EntryName = NewFName;
	BB->UpdateKeyIDs();
	BB->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(AssetPath, FString::Printf(TEXT("Key renamed: '%s' -> '%s'"), *OldName, *NewName));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  9. get_bb_key_details
// ============================================================

FMonolithActionResult FMonolithAIBlackboardActions::HandleGetBBKeyDetails(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBlackboardData* BB = MonolithAI::LoadBlackboardFromParams(Params, AssetPath, Error);
	if (!BB)
	{
		return FMonolithActionResult::Error(Error);
	}

	FString KeyName;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("key_name"), KeyName, ErrResult))
	{
		return ErrResult;
	}

	FName KeyFName(*KeyName);

	// Search own keys first
	bool bIsInherited = false;
	const FBlackboardEntry* FoundEntry = nullptr;

	int32 Idx = FindKeyIndex(BB, KeyFName);
	if (Idx != INDEX_NONE)
	{
		FoundEntry = &BB->Keys[Idx];
	}
	else
	{
		// Check parent chain
		const UBlackboardData* Current = BB->Parent;
		while (Current && !FoundEntry)
		{
			for (const FBlackboardEntry& Entry : Current->Keys)
			{
				if (Entry.EntryName == KeyFName)
				{
					FoundEntry = &Entry;
					bIsInherited = true;
					break;
				}
			}
			Current = Current->Parent;
		}
	}

	if (!FoundEntry)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Key '%s' not found in blackboard or parent chain"), *KeyName));
	}

	TSharedPtr<FJsonObject> Result = EntryToJson(*FoundEntry, bIsInherited);
	Result->SetStringField(TEXT("asset_path"), AssetPath);

	// Add key type class name for extra detail
	if (FoundEntry->KeyType)
	{
		Result->SetStringField(TEXT("key_type_class"), FoundEntry->KeyType->GetClass()->GetPathName());
		Result->SetStringField(TEXT("key_type_description"), FoundEntry->KeyType->DescribeSelf());
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  10. batch_add_bb_keys
// ============================================================

FMonolithActionResult FMonolithAIBlackboardActions::HandleBatchAddBBKeys(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBlackboardData* BB = MonolithAI::LoadBlackboardFromParams(Params, AssetPath, Error);
	if (!BB)
	{
		return FMonolithActionResult::Error(Error);
	}

	const TArray<TSharedPtr<FJsonValue>>* KeysArray;
	if (!Params->TryGetArrayField(TEXT("keys"), KeysArray))
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: keys (array)"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Batch Add BB Keys")));
	BB->Modify();

	TArray<TSharedPtr<FJsonValue>> AddedKeys;
	TArray<TSharedPtr<FJsonValue>> Errors;
	int32 AddedCount = 0;

	for (int32 i = 0; i < KeysArray->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>* KeyObj;
		if (!(*KeysArray)[i]->TryGetObject(KeyObj))
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetNumberField(TEXT("index"), i);
			ErrObj->SetStringField(TEXT("error"), TEXT("Array element is not an object"));
			Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
			continue;
		}

		FString KName = (*KeyObj)->GetStringField(TEXT("name"));
		FString KType = (*KeyObj)->GetStringField(TEXT("type"));

		if (KName.IsEmpty() || KType.IsEmpty())
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetNumberField(TEXT("index"), i);
			ErrObj->SetStringField(TEXT("error"), TEXT("Each key needs 'name' and 'type' fields"));
			Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
			continue;
		}

		FName KFName(*KName);

		// Check duplicates
		if (FindKeyIndex(BB, KFName) != INDEX_NONE || IsKeyInParentChain(BB, KFName))
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetNumberField(TEXT("index"), i);
			ErrObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Key '%s' already exists"), *KName));
			Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
			continue;
		}

		// Create key type — pass the key object itself for extra params like base_class, enum_type
		FString TypeError;
		UBlackboardKeyType* KeyType = CreateKeyTypeFromString(BB, KType, *KeyObj, TypeError);
		if (!KeyType)
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetNumberField(TEXT("index"), i);
			ErrObj->SetStringField(TEXT("error"), TypeError);
			Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
			continue;
		}

		FBlackboardEntry NewEntry;
		NewEntry.EntryName = KFName;
		NewEntry.KeyType = KeyType;

#if WITH_EDITORONLY_DATA
		FString Desc = (*KeyObj)->GetStringField(TEXT("description"));
		if (!Desc.IsEmpty())
		{
			NewEntry.EntryDescription = Desc;
		}
#endif

		BB->Keys.Add(NewEntry);
		AddedCount++;

		TSharedPtr<FJsonObject> Added = MakeShared<FJsonObject>();
		Added->SetStringField(TEXT("name"), KName);
		Added->SetStringField(TEXT("type"), KType);
		AddedKeys.Add(MakeShared<FJsonValueObject>(Added));
	}

	BB->UpdateKeyIDs();
	BB->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("added_count"), AddedCount);
	Result->SetNumberField(TEXT("total_key_count"), BB->Keys.Num());
	Result->SetArrayField(TEXT("added"), AddedKeys);
	if (Errors.Num() > 0)
	{
		Result->SetArrayField(TEXT("errors"), Errors);
	}
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Added %d keys"), AddedCount));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  11. set_bb_parent
// ============================================================

FMonolithActionResult FMonolithAIBlackboardActions::HandleSetBBParent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UBlackboardData* BB = MonolithAI::LoadBlackboardFromParams(Params, AssetPath, Error);
	if (!BB)
	{
		return FMonolithActionResult::Error(Error);
	}

	// parent_path can be empty/"none"/"null" to clear parent
	FString ParentPath = Params->GetStringField(TEXT("parent_path"));

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Set BB Parent")));
	BB->Modify();

	if (ParentPath.IsEmpty() || ParentPath == TEXT("none") || ParentPath == TEXT("null"))
	{
		// Clear parent
		BB->Parent = nullptr;
		BB->UpdateParentKeys();
		BB->UpdateKeyIDs();
		BB->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(AssetPath, TEXT("Parent cleared"));
		return FMonolithActionResult::Success(Result);
	}

	// Load parent
	UObject* ParentObj = FMonolithAssetUtils::LoadAssetByPath(UBlackboardData::StaticClass(), ParentPath);
	UBlackboardData* ParentBB = Cast<UBlackboardData>(ParentObj);
	if (!ParentBB)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Parent blackboard not found: %s"), *ParentPath));
	}

	// Circular reference check
	if (ParentBB == BB)
	{
		return FMonolithActionResult::Error(TEXT("Cannot set a blackboard as its own parent"));
	}
	{
		const UBlackboardData* Check = ParentBB->Parent;
		while (Check)
		{
			if (Check == BB)
			{
				return FMonolithActionResult::Error(TEXT("Circular parent reference detected"));
			}
			Check = Check->Parent;
		}
	}

	BB->Parent = ParentBB;
	BB->UpdateParentKeys();
	BB->UpdateKeyIDs();
	BB->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MonolithAI::MakeAssetResult(AssetPath, FString::Printf(TEXT("Parent set to '%s'"), *ParentPath));
	Result->SetStringField(TEXT("parent"), ParentBB->GetPathName());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  12. compare_blackboards
// ============================================================

FMonolithActionResult FMonolithAIBlackboardActions::HandleCompareBlackboards(const TSharedPtr<FJsonObject>& Params)
{
	FString PathA, PathB;
	FMonolithActionResult ErrResult;
	if (!MonolithAI::RequireStringParam(Params, TEXT("path_a"), PathA, ErrResult))
	{
		return ErrResult;
	}
	if (!MonolithAI::RequireStringParam(Params, TEXT("path_b"), PathB, ErrResult))
	{
		return ErrResult;
	}

	UBlackboardData* BBA = Cast<UBlackboardData>(FMonolithAssetUtils::LoadAssetByPath(UBlackboardData::StaticClass(), PathA));
	UBlackboardData* BBB = Cast<UBlackboardData>(FMonolithAssetUtils::LoadAssetByPath(UBlackboardData::StaticClass(), PathB));

	if (!BBA) return FMonolithActionResult::Error(FString::Printf(TEXT("Blackboard A not found: %s"), *PathA));
	if (!BBB) return FMonolithActionResult::Error(FString::Printf(TEXT("Blackboard B not found: %s"), *PathB));

	// Build key maps: name -> type string
	auto BuildKeyMap = [](const UBlackboardData* BB) -> TMap<FName, const FBlackboardEntry*>
	{
		TMap<FName, const FBlackboardEntry*> Map;
		// Collect from parent chain
		const UBlackboardData* Current = BB;
		while (Current)
		{
			for (const FBlackboardEntry& Entry : Current->Keys)
			{
				if (!Map.Contains(Entry.EntryName))
				{
					Map.Add(Entry.EntryName, &Entry);
				}
			}
			Current = Current->Parent;
		}
		return Map;
	};

	TMap<FName, const FBlackboardEntry*> MapA = BuildKeyMap(BBA);
	TMap<FName, const FBlackboardEntry*> MapB = BuildKeyMap(BBB);

	TArray<TSharedPtr<FJsonValue>> OnlyInA;
	TArray<TSharedPtr<FJsonValue>> OnlyInB;
	TArray<TSharedPtr<FJsonValue>> Changed;
	TArray<TSharedPtr<FJsonValue>> Same;

	// Keys in A
	for (const auto& Pair : MapA)
	{
		const FBlackboardEntry** InB = MapB.Find(Pair.Key);
		if (!InB)
		{
			OnlyInA.Add(MakeShared<FJsonValueObject>(EntryToJson(*Pair.Value)));
		}
		else
		{
			FString TypeA = KeyTypeToString(Pair.Value->KeyType);
			FString TypeB = KeyTypeToString((*InB)->KeyType);
			if (TypeA != TypeB)
			{
				TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
				Diff->SetStringField(TEXT("key_name"), Pair.Key.ToString());
				Diff->SetStringField(TEXT("type_in_a"), TypeA);
				Diff->SetStringField(TEXT("type_in_b"), TypeB);
				Changed.Add(MakeShared<FJsonValueObject>(Diff));
			}
			else
			{
				TSharedPtr<FJsonObject> SameObj = MakeShared<FJsonObject>();
				SameObj->SetStringField(TEXT("key_name"), Pair.Key.ToString());
				SameObj->SetStringField(TEXT("key_type"), TypeA);
				Same.Add(MakeShared<FJsonValueObject>(SameObj));
			}
		}
	}

	// Keys only in B
	for (const auto& Pair : MapB)
	{
		if (!MapA.Contains(Pair.Key))
		{
			OnlyInB.Add(MakeShared<FJsonValueObject>(EntryToJson(*Pair.Value)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path_a"), PathA);
	Result->SetStringField(TEXT("path_b"), PathB);
	Result->SetArrayField(TEXT("only_in_a"), OnlyInA);
	Result->SetArrayField(TEXT("only_in_b"), OnlyInB);
	Result->SetArrayField(TEXT("type_changed"), Changed);
	Result->SetArrayField(TEXT("same"), Same);
	Result->SetNumberField(TEXT("total_a"), MapA.Num());
	Result->SetNumberField(TEXT("total_b"), MapB.Num());
	Result->SetBoolField(TEXT("identical"), OnlyInA.Num() == 0 && OnlyInB.Num() == 0 && Changed.Num() == 0);
	return FMonolithActionResult::Success(Result);
}
