#include "MonolithBlueprintStructActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/DataTable.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "MonolithBlueprintEditCradle.h"
#include "ScopedTransaction.h"

// ============================================================
//  Registration
// ============================================================

void FMonolithBlueprintStructActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("create_user_defined_struct"),
		TEXT("Create a new User Defined Struct asset with the specified fields. Each field has a name, type (same type strings as add_variable), and optional default_value."),
		FMonolithActionHandler::CreateStatic(&HandleCreateUserDefinedStruct),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path, e.g. /Game/Data/S_MyStruct"))
			.Required(TEXT("fields"),    TEXT("array"),  TEXT("Array of field objects: [{name, type, default_value?}]. Type uses same strings as add_variable (bool, int, float, string, name, text, Vector, Rotator, Transform, object:ClassName, etc.)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("create_user_defined_enum"),
		TEXT("Create a new User Defined Enum asset with the specified enumerator values."),
		FMonolithActionHandler::CreateStatic(&HandleCreateUserDefinedEnum),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path, e.g. /Game/Data/E_MyEnum"))
			.Required(TEXT("values"),    TEXT("array"),  TEXT("Array of enumerator display name strings, e.g. [\"Idle\", \"Running\", \"Jumping\"]"))
			.Build());

	// DataTable actions (Phase 3C)
	Registry.RegisterAction(TEXT("blueprint"), TEXT("create_data_table"),
		TEXT("Create a new DataTable asset backed by the specified row struct (UScriptStruct). The struct must already exist (native or user-defined)."),
		FMonolithActionHandler::CreateStatic(&HandleCreateDataTable),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"),  TEXT("string"), TEXT("Asset save path, e.g. /Game/Data/DT_Weapons"))
			.Required(TEXT("row_struct"), TEXT("string"), TEXT("Name of the row struct, e.g. FMyRowStruct, MyRowStruct, or a full path like /Script/MyModule.MyRowStruct"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_data_table_row"),
		TEXT("Add a row to an existing DataTable. Values are a JSON object mapping column names to values (uses ImportText per field)."),
		FMonolithActionHandler::CreateStatic(&HandleAddDataTableRow),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("DataTable asset path, e.g. /Game/Data/DT_Weapons"))
			.Required(TEXT("row_name"),   TEXT("string"), TEXT("Row name / key"))
			.Required(TEXT("values"),     TEXT("object"), TEXT("JSON object of {column_name: value, ...}. Values are converted via ImportText."))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_data_table_rows"),
		TEXT("Read rows from a DataTable. Returns all rows, or a single row if row_name is specified."),
		FMonolithActionHandler::CreateStatic(&HandleGetDataTableRows),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("DataTable asset path, e.g. /Game/Data/DT_Weapons"))
			.Optional(TEXT("row_name"),   TEXT("string"), TEXT("If provided, return only this row. Otherwise return all rows."))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("create_data_asset"),
		TEXT("Create a raw UObject asset (NOT a Blueprint). Use for DataAssets, MaterialParameterCollections, PhysicalMaterials, CurveFloats, and any UObject-derived class that needs to exist as a direct instance rather than a Blueprint-generated class. Resolves class_name via FindFirstObject with U/A prefix fallback. Rejects abstract, deprecated, Actor-derived, and Blueprint classes."),
		FMonolithActionHandler::CreateStatic(&HandleCreateDataAsset),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"),  TEXT("string"),  TEXT("Asset save path, e.g. /Game/Data/DA_ResponseMap"))
			.Required(TEXT("class_name"), TEXT("string"),  TEXT("UObject class name, e.g. CarnageFXResponseMap, MaterialParameterCollection, PhysicalMaterial, CurveFloat. Can also use full path /Script/Module.ClassName for disambiguation"))
			.Optional(TEXT("skip_save"),  TEXT("boolean"), TEXT("Skip synchronous package save (default: false)"), TEXT("false"))
			.Build());
}

// ============================================================
//  create_user_defined_struct
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleCreateUserDefinedStruct(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: save_path"));
	}

	const TArray<TSharedPtr<FJsonValue>>* FieldsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("fields"), FieldsArray) || !FieldsArray || FieldsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: fields (array of {name, type, default_value?})"));
	}

	// Extract asset name from save path
	int32 LastSlash;
	if (!SavePath.FindLastChar(TEXT('/'), LastSlash))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid save_path — must contain at least one '/': %s"), *SavePath));
	}
	FString AssetName = SavePath.Mid(LastSlash + 1);
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("save_path must not end with '/': %s"), *SavePath));
	}

	// Guard against existing asset (same pattern as create_blueprint)
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData ExistingAsset = AR.GetAssetByObjectPath(FSoftObjectPath(SavePath + TEXT(".") + AssetName));
	if (ExistingAsset.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists at '%s'. Delete it first."), *SavePath));
	}
	if (FindObject<UObject>(nullptr, *(SavePath + TEXT(".") + AssetName)))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists in memory at '%s'. Delete it first."), *SavePath));
	}

	// Create package
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at path: %s"), *SavePath));
	}
	Package->FullyLoad();

	// Create the user defined struct
	UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(Package, FName(*AssetName), RF_Public | RF_Standalone);
	if (!Struct)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("FStructureEditorUtils::CreateUserDefinedStruct failed for: %s"), *AssetName));
	}

	// CreateUserDefinedStruct creates one default member variable. We'll track fields added.
	TArray<TSharedPtr<FJsonValue>> FieldResults;
	int32 FieldIndex = 0;

	for (const TSharedPtr<FJsonValue>& FieldVal : *FieldsArray)
	{
		const TSharedPtr<FJsonObject>* FieldObjPtr = nullptr;
		if (!FieldVal.IsValid() || !FieldVal->TryGetObject(FieldObjPtr) || !FieldObjPtr || !(*FieldObjPtr).IsValid())
		{
			FieldResults.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Field %d: skipped (not a valid JSON object)"), FieldIndex)));
			FieldIndex++;
			continue;
		}

		const TSharedPtr<FJsonObject>& FieldObj = *FieldObjPtr;
		FString FieldName = FieldObj->GetStringField(TEXT("name"));
		FString TypeStr = FieldObj->GetStringField(TEXT("type"));

		if (FieldName.IsEmpty() || TypeStr.IsEmpty())
		{
			FieldResults.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Field %d: skipped (missing name or type)"), FieldIndex)));
			FieldIndex++;
			continue;
		}

		// Parse the type string to FEdGraphPinType
		FEdGraphPinType PinType = MonolithBlueprintInternal::ParsePinTypeFromString(TypeStr);

		// The first field replaces the default member created by CreateUserDefinedStruct.
		// Subsequent fields need AddVariable.
		if (FieldIndex > 0)
		{
			bool bAdded = FStructureEditorUtils::AddVariable(Struct, PinType);
			if (!bAdded)
			{
				TSharedPtr<FJsonObject> FieldResult = MakeShared<FJsonObject>();
				FieldResult->SetStringField(TEXT("name"), FieldName);
				FieldResult->SetStringField(TEXT("error"), TEXT("AddVariable failed"));
				FieldResults.Add(MakeShared<FJsonValueObject>(FieldResult));
				FieldIndex++;
				continue;
			}
		}
		else
		{
			// For the first field, change the type of the default variable
			TArray<FStructVariableDescription>& VarDesc = FStructureEditorUtils::GetVarDesc(Struct);
			if (VarDesc.Num() > 0)
			{
				FStructureEditorUtils::ChangeVariableType(Struct, VarDesc[0].VarGuid, PinType);
			}
		}

		// Get the VarDesc for the field we just added/modified (it's the last entry for added, or first for index 0)
		TArray<FStructVariableDescription>& VarDesc = FStructureEditorUtils::GetVarDesc(Struct);
		int32 DescIndex = (FieldIndex == 0) ? 0 : VarDesc.Num() - 1;

		if (VarDesc.IsValidIndex(DescIndex))
		{
			FGuid VarGuid = VarDesc[DescIndex].VarGuid;

			// Rename the variable to the desired display name
			FStructureEditorUtils::RenameVariable(Struct, VarGuid, FieldName);

			// Set default value if provided
			FString DefaultValue = FieldObj->GetStringField(TEXT("default_value"));
			if (!DefaultValue.IsEmpty())
			{
				FStructureEditorUtils::ChangeVariableDefaultValue(Struct, VarGuid, DefaultValue);
			}

			TSharedPtr<FJsonObject> FieldResult = MakeShared<FJsonObject>();
			FieldResult->SetStringField(TEXT("name"), FieldName);
			FieldResult->SetStringField(TEXT("type"), TypeStr);
			FieldResult->SetStringField(TEXT("guid"), VarGuid.ToString());
			if (!DefaultValue.IsEmpty())
			{
				FieldResult->SetStringField(TEXT("default_value"), DefaultValue);
			}
			FieldResults.Add(MakeShared<FJsonValueObject>(FieldResult));
		}

		FieldIndex++;
	}

	// Compile the struct
	FStructureEditorUtils::CompileStructure(Struct);

	// Save
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Struct);
	bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(Struct, false);

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), SavePath);
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetNumberField(TEXT("field_count"), FieldResults.Num());
	Root->SetArrayField(TEXT("fields"), FieldResults);
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  create_user_defined_enum
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleCreateUserDefinedEnum(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: save_path"));
	}

	const TArray<TSharedPtr<FJsonValue>>* ValuesArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("values"), ValuesArray) || !ValuesArray || ValuesArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: values (array of strings)"));
	}

	// Extract asset name from save path
	int32 LastSlash;
	if (!SavePath.FindLastChar(TEXT('/'), LastSlash))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid save_path — must contain at least one '/': %s"), *SavePath));
	}
	FString AssetName = SavePath.Mid(LastSlash + 1);
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("save_path must not end with '/': %s"), *SavePath));
	}

	// Guard against existing asset
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData ExistingAsset = AR.GetAssetByObjectPath(FSoftObjectPath(SavePath + TEXT(".") + AssetName));
	if (ExistingAsset.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists at '%s'. Delete it first."), *SavePath));
	}
	if (FindObject<UObject>(nullptr, *(SavePath + TEXT(".") + AssetName)))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists in memory at '%s'. Delete it first."), *SavePath));
	}

	// Create package
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at path: %s"), *SavePath));
	}
	Package->FullyLoad();

	// Create the user defined enum — returns UEnum*, cast to UUserDefinedEnum*
	UEnum* RawEnum = FEnumEditorUtils::CreateUserDefinedEnum(Package, FName(*AssetName), RF_Public | RF_Standalone);
	UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(RawEnum);
	if (!Enum)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("FEnumEditorUtils::CreateUserDefinedEnum failed for: %s"), *AssetName));
	}

	// CreateUserDefinedEnum creates one default entry plus the hidden _MAX.
	// We need to add (N - 1) more enumerators for N total values.
	int32 NumValues = ValuesArray->Num();
	for (int32 i = 1; i < NumValues; i++)
	{
		FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
	}

	// Set display names for each enumerator
	TArray<TSharedPtr<FJsonValue>> ValueResults;
	for (int32 i = 0; i < NumValues; i++)
	{
		FString DisplayName;
		if ((*ValuesArray)[i].IsValid())
		{
			DisplayName = (*ValuesArray)[i]->AsString();
		}

		if (!DisplayName.IsEmpty())
		{
			FEnumEditorUtils::SetEnumeratorDisplayName(Enum, i, FText::FromString(DisplayName));
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("index"), i);
		Entry->SetStringField(TEXT("display_name"), DisplayName);
		// Get the internal name for reference
		if (i < Enum->NumEnums() - 1) // -1 to skip _MAX
		{
			Entry->SetStringField(TEXT("internal_name"), Enum->GetNameStringByIndex(i));
		}
		ValueResults.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Save
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Enum);
	bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(Enum, false);

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), SavePath);
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetNumberField(TEXT("enumerator_count"), NumValues);
	Root->SetArrayField(TEXT("values"), ValueResults);
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  Helper: Resolve a UScriptStruct by name
// ============================================================

static UScriptStruct* ResolveScriptStruct(const FString& StructName)
{
	// Try as-is first
	UScriptStruct* Found = FindFirstObject<UScriptStruct>(*StructName, EFindFirstObjectOptions::NativeFirst);
	if (Found) return Found;

	// Try with F prefix (common C++ convention: FMyStruct)
	if (!StructName.StartsWith(TEXT("F")))
	{
		Found = FindFirstObject<UScriptStruct>(*(TEXT("F") + StructName), EFindFirstObjectOptions::NativeFirst);
		if (Found) return Found;
	}

	// Try stripping F prefix if provided
	if (StructName.StartsWith(TEXT("F")) && StructName.Len() > 1)
	{
		Found = FindFirstObject<UScriptStruct>(*StructName.Mid(1), EFindFirstObjectOptions::NativeFirst);
		if (Found) return Found;
	}

	return nullptr;
}

// ============================================================
//  Helper: Serialize a single DataTable row to JSON
// ============================================================

// Get a user-friendly property name — display name for UDS properties, internal name otherwise
static FString GetFriendlyPropertyName(FProperty* Prop)
{
	FString DisplayName = Prop->GetMetaData(TEXT("DisplayName"));
	if (!DisplayName.IsEmpty()) return DisplayName;
	// Fallback: strip GUID suffix from UDS names (e.g., "Name_2_C392053F..." → "Name")
	FString Name = Prop->GetName();
	// UDS properties follow pattern: DisplayName_N_GUID
	int32 FirstUnderscore;
	if (Name.FindChar(TEXT('_'), FirstUnderscore))
	{
		FString Prefix = Name.Left(FirstUnderscore);
		// Check if next char after underscore is a digit (UDS naming pattern)
		if (FirstUnderscore + 1 < Name.Len() && FChar::IsDigit(Name[FirstUnderscore + 1]))
		{
			return Prefix;
		}
	}
	return Name;
}

static TSharedPtr<FJsonObject> SerializeRowToJson(const UScriptStruct* RowStruct, const uint8* RowData)
{
	TSharedPtr<FJsonObject> ValuesObj = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	{
		FProperty* Prop = *It;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);
		FString ValueStr;
		Prop->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, nullptr, PPF_None);
		ValuesObj->SetStringField(GetFriendlyPropertyName(Prop), ValueStr);
	}
	return ValuesObj;
}

// ============================================================
//  create_data_table
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleCreateDataTable(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: save_path"));
	}

	FString RowStructName = Params->GetStringField(TEXT("row_struct"));
	if (RowStructName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: row_struct"));
	}

	// Resolve the row struct
	UScriptStruct* RowStruct = ResolveScriptStruct(RowStructName);
	if (!RowStruct)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Could not find UScriptStruct '%s'. Tried as-is, with 'F' prefix, and without 'F' prefix."), *RowStructName));
	}

	// Extract asset name from save path
	int32 LastSlash;
	if (!SavePath.FindLastChar(TEXT('/'), LastSlash))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid save_path — must contain at least one '/': %s"), *SavePath));
	}
	FString AssetName = SavePath.Mid(LastSlash + 1);
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("save_path must not end with '/': %s"), *SavePath));
	}

	// Guard against existing asset
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData ExistingAsset = AR.GetAssetByObjectPath(FSoftObjectPath(SavePath + TEXT(".") + AssetName));
	if (ExistingAsset.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists at '%s'. Delete it first."), *SavePath));
	}
	if (FindObject<UObject>(nullptr, *(SavePath + TEXT(".") + AssetName)))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists in memory at '%s'. Delete it first."), *SavePath));
	}

	// Create package
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at path: %s"), *SavePath));
	}
	Package->FullyLoad();

	// Create the DataTable
	UDataTable* DataTable = NewObject<UDataTable>(Package, FName(*AssetName), RF_Public | RF_Standalone);
	if (!DataTable)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create UDataTable: %s"), *AssetName));
	}

	DataTable->RowStruct = RowStruct;

	// Save
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(DataTable);
	bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(DataTable, false);

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), SavePath);
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetStringField(TEXT("row_struct"), RowStruct->GetName());
	Root->SetStringField(TEXT("row_struct_path"), RowStruct->GetPathName());
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  add_data_table_row
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleAddDataTableRow(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	FString RowName = Params->GetStringField(TEXT("row_name"));
	if (RowName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: row_name"));
	}

	const TSharedPtr<FJsonObject>* ValuesObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("values"), ValuesObj) || !ValuesObj || !(*ValuesObj).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: values (JSON object of column->value)"));
	}

	// Load the DataTable
	UDataTable* DataTable = FMonolithAssetUtils::LoadAssetByPath<UDataTable>(AssetPath);
	if (!DataTable)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("DataTable not found: %s"), *AssetPath));
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (!RowStruct)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("DataTable '%s' has no RowStruct set"), *AssetPath));
	}

	// Check if row already exists
	FName RowFName(*RowName);
	if (DataTable->GetRowMap().Contains(RowFName))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Row '%s' already exists in DataTable '%s'. Remove it first or use a different name."), *RowName, *AssetPath));
	}

	// Allocate and initialize row memory
	const int32 StructSize = RowStruct->GetStructureSize();
	uint8* RowData = static_cast<uint8*>(FMemory::Malloc(StructSize));
	RowStruct->InitializeStruct(RowData);

	// Populate fields from the values JSON object
	TArray<FString> SetFields;
	TArray<FString> SkippedFields;

	for (const auto& Pair : (*ValuesObj)->Values)
	{
		const FString& FieldName = Pair.Key;
		const TSharedPtr<FJsonValue>& JsonVal = Pair.Value;

		// Find property by name — try exact, case-insensitive, then display name
		// UDS properties have GUID-encoded names (e.g., "Name_2_C392053F...") but
		// callers use friendly display names ("Name"). Check DisplayName metadata.
		FProperty* Prop = RowStruct->FindPropertyByName(FName(*FieldName));
		if (!Prop)
		{
			for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
			{
				// Case-insensitive internal name match
				if (It->GetName().Equals(FieldName, ESearchCase::IgnoreCase))
				{
					Prop = *It;
					break;
				}
				// Display name match (critical for UserDefinedStructs)
				FString DisplayName = It->GetMetaData(TEXT("DisplayName"));
				if (!DisplayName.IsEmpty() && DisplayName.Equals(FieldName, ESearchCase::IgnoreCase))
				{
					Prop = *It;
					break;
				}
				// Also try stripping the GUID suffix: "Name_2_GUID" → check if starts with "FieldName_"
				FString PropName = It->GetName();
				int32 UnderscoreIdx;
				if (PropName.FindChar(TEXT('_'), UnderscoreIdx))
				{
					FString ShortName = PropName.Left(UnderscoreIdx);
					if (ShortName.Equals(FieldName, ESearchCase::IgnoreCase))
					{
						Prop = *It;
						break;
					}
				}
			}
		}

		if (!Prop)
		{
			SkippedFields.Add(FString::Printf(TEXT("%s (not found)"), *FieldName));
			continue;
		}

		// Convert JSON value to string for ImportText
		FString ValueStr;
		if (JsonVal.IsValid())
		{
			if (JsonVal->Type == EJson::Number)
			{
				ValueStr = FString::SanitizeFloat(JsonVal->AsNumber());
			}
			else if (JsonVal->Type == EJson::Boolean)
			{
				ValueStr = JsonVal->AsBool() ? TEXT("true") : TEXT("false");
			}
			else
			{
				ValueStr = JsonVal->AsString();
			}
		}

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);
		const TCHAR* ImportResult = Prop->ImportText_Direct(*ValueStr, ValuePtr, nullptr, PPF_None);
		if (ImportResult)
		{
			SetFields.Add(FieldName);
		}
		else
		{
			SkippedFields.Add(FString::Printf(TEXT("%s (ImportText failed for value: %s)"), *FieldName, *ValueStr));
		}
	}

	// Add the row to the DataTable — uses the uint8*/UScriptStruct overload which copies internally
	DataTable->AddRow(RowFName, RowData, RowStruct);

	// Free our temporary copy
	RowStruct->DestroyStruct(RowData);
	FMemory::Free(RowData);

	DataTable->Modify();
	DataTable->MarkPackageDirty();

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("row_name"), RowName);
	Root->SetNumberField(TEXT("fields_set"), SetFields.Num());

	TArray<TSharedPtr<FJsonValue>> SetArr;
	for (const FString& F : SetFields) SetArr.Add(MakeShared<FJsonValueString>(F));
	Root->SetArrayField(TEXT("set_fields"), SetArr);

	if (SkippedFields.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> SkipArr;
		for (const FString& F : SkippedFields) SkipArr.Add(MakeShared<FJsonValueString>(F));
		Root->SetArrayField(TEXT("skipped_fields"), SkipArr);
	}

	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  get_data_table_rows
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleGetDataTableRows(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	FString RowNameFilter;
	Params->TryGetStringField(TEXT("row_name"), RowNameFilter);

	// Load the DataTable
	UDataTable* DataTable = FMonolithAssetUtils::LoadAssetByPath<UDataTable>(AssetPath);
	if (!DataTable)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("DataTable not found: %s"), *AssetPath));
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (!RowStruct)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("DataTable '%s' has no RowStruct set"), *AssetPath));
	}

	const TMap<FName, uint8*>& RowMap = DataTable->GetRowMap();

	TArray<TSharedPtr<FJsonValue>> RowResults;

	if (!RowNameFilter.IsEmpty())
	{
		// Single row lookup
		const FName RowFName(*RowNameFilter);
		const uint8* const* FoundRow = RowMap.Find(RowFName);
		if (!FoundRow || !(*FoundRow))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Row '%s' not found in DataTable '%s'"), *RowNameFilter, *AssetPath));
		}

		TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
		RowObj->SetStringField(TEXT("row_name"), RowNameFilter);
		RowObj->SetObjectField(TEXT("values"), SerializeRowToJson(RowStruct, *FoundRow));
		RowResults.Add(MakeShared<FJsonValueObject>(RowObj));
	}
	else
	{
		// All rows
		for (const auto& Pair : RowMap)
		{
			TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
			RowObj->SetStringField(TEXT("row_name"), Pair.Key.ToString());
			RowObj->SetObjectField(TEXT("values"), SerializeRowToJson(RowStruct, Pair.Value));
			RowResults.Add(MakeShared<FJsonValueObject>(RowObj));
		}
	}

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("row_struct"), RowStruct->GetName());
	Root->SetNumberField(TEXT("row_count"), RowResults.Num());
	Root->SetNumberField(TEXT("total_rows"), RowMap.Num());
	Root->SetArrayField(TEXT("rows"), RowResults);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  create_data_asset
// ============================================================

FMonolithActionResult FMonolithBlueprintStructActions::HandleCreateDataAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: save_path"));
	}

	FString ClassName = Params->GetStringField(TEXT("class_name"));
	if (ClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: class_name"));
	}

	// Extract asset name from save path
	int32 LastSlash;
	if (!SavePath.FindLastChar(TEXT('/'), LastSlash))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid save_path — must contain at least one '/': %s"), *SavePath));
	}
	FString AssetName = SavePath.Mid(LastSlash + 1);
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("save_path must not end with '/': %s"), *SavePath));
	}

	// Resolve class_name → UClass*
	UClass* ResolvedClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (!ResolvedClass)
	{
		ResolvedClass = FindFirstObject<UClass>(*(TEXT("U") + ClassName), EFindFirstObjectOptions::NativeFirst);
	}
	if (!ResolvedClass)
	{
		ResolvedClass = FindFirstObject<UClass>(*(TEXT("A") + ClassName), EFindFirstObjectOptions::NativeFirst);
	}
	if (!ResolvedClass)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Class not found: '%s'. Tried as-is, with 'U' prefix, and with 'A' prefix. "
				 "Use full path (e.g. /Script/Module.ClassName) for disambiguation."), *ClassName));
	}

	// Guard: reject Blueprint and BlueprintGeneratedClass (use create_blueprint instead)
	if (ResolvedClass->IsChildOf(UBlueprint::StaticClass()) ||
		ResolvedClass->IsChildOf(UBlueprintGeneratedClass::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Class '%s' is a Blueprint class. Use create_blueprint instead."), *ResolvedClass->GetName()));
	}

	// Guard: reject abstract, deprecated, or superseded classes
	if (ResolvedClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		FString Reason;
		if (ResolvedClass->HasAnyClassFlags(CLASS_Abstract)) Reason = TEXT("abstract");
		else if (ResolvedClass->HasAnyClassFlags(CLASS_Deprecated)) Reason = TEXT("deprecated");
		else Reason = TEXT("superseded by a newer version");
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Cannot instantiate class '%s': it is %s."), *ResolvedClass->GetName(), *Reason));
	}

	// Guard: reject Actor-derived classes (use spawn_actor instead)
	if (ResolvedClass->IsChildOf(AActor::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Class '%s' is Actor-derived. Actors must live in a ULevel — use spawn_actor or create_blueprint instead."),
			*ResolvedClass->GetName()));
	}

	// Guard against existing asset (2-tier: Asset Registry + FindObject)
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData ExistingAsset = AR.GetAssetByObjectPath(FSoftObjectPath(SavePath + TEXT(".") + AssetName));
	if (ExistingAsset.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists at '%s'. Delete it first."), *SavePath));
	}
	if (FindObject<UObject>(nullptr, *(SavePath + TEXT(".") + AssetName)))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists in memory at '%s'. Delete it first."), *SavePath));
	}

	// Create package
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at path: %s"), *SavePath));
	}
	Package->FullyLoad();

	// Create the raw UObject instance
	UObject* NewAsset = NewObject<UObject>(Package, ResolvedClass, FName(*AssetName), RF_Public | RF_Standalone);
	if (!NewAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("NewObject failed for class '%s' at path '%s'."), *ResolvedClass->GetName(), *SavePath));
	}

	// Fire edit cradle on all properties — initializes FOverridableManager state (#29).
	NewAsset->SetFlags(RF_Transactional);
	FScopedTransaction Transaction(NSLOCTEXT("MonolithBlueprintStructActions",
		"CreateDataAsset", "Monolith Create Data Asset"));
	NewAsset->Modify();

	for (TFieldIterator<FProperty> It(ResolvedClass); It; ++It)
	{
		FProperty* Prop = *It;
		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			continue;
		MonolithEditCradle::FireFullCradle(NewAsset, Prop);
	}

	// Read skip_save param
	bool bSkipSave = false;
	if (Params->HasField(TEXT("skip_save")))
	{
		bSkipSave = Params->GetBoolField(TEXT("skip_save"));
	}

	// Notify and save
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewAsset);

	bool bSaved = false;
	if (!bSkipSave)
	{
		bSaved = UEditorAssetLibrary::SaveLoadedAsset(NewAsset, false);
	}

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), SavePath);
	Root->SetStringField(TEXT("class_name"), ClassName);
	Root->SetStringField(TEXT("actual_class"), ResolvedClass->GetName());
	Root->SetStringField(TEXT("class_path"), ResolvedClass->GetPathName());
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}
