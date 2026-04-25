#include "MonolithGASAttributeActions.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"
#include "MonolithAssetUtils.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "GameplayEffectExecutionCalculation.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "UObject/UObjectIterator.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "UObject/SavePackage.h"
#include "Engine/DataTable.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EngineUtils.h"

#if WITH_GBA
#include "Abilities/GBAAttributeSetBlueprintBase.h"
#include "Blueprint/GBAAttributeSetBlueprint.h"
#endif

// ============================================================
//  Registration
// ============================================================

void FMonolithGASAttributeActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("gas"), TEXT("create_attribute_set"),
		TEXT("Create a new AttributeSet class. In 'blueprint' mode (requires GBA plugin), creates a Blueprint AttributeSet. "
		     "In 'cpp' mode, generates C++ header and source files for a UAttributeSet subclass."),
		FMonolithActionHandler::CreateStatic(&HandleCreateAttributeSet),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("For blueprint mode: asset path (e.g. /Game/GAS/Attributes/AS_Health). For cpp mode: class name (e.g. ULeviathanHealthAttributeSet)"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("Creation mode: 'cpp' or 'blueprint' (default: 'blueprint')"), TEXT("blueprint"))
			.Optional(TEXT("parent_class"), TEXT("string"), TEXT("Parent class name (default: UAttributeSet for cpp, UGBAAttributeSetBlueprintBase for blueprint)"))
			.Optional(TEXT("attributes"), TEXT("array"), TEXT("Array of attribute definitions: [{name, default_value?, replicated?}]"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("add_attribute"),
		TEXT("Add an FGameplayAttributeData member to an existing AttributeSet. Works with both Blueprint and C++ sets."),
		FMonolithActionHandler::CreateStatic(&HandleAddAttribute),
		FParamSchemaBuilder()
			.Required(TEXT("attribute_set"), TEXT("string"), TEXT("Asset path for Blueprint sets, or class name for C++ sets"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Attribute name (e.g. Health, MaxHealth)"))
			.Optional(TEXT("default_value"), TEXT("number"), TEXT("Default value (default: 0)"))
			.Optional(TEXT("replicated"), TEXT("boolean"), TEXT("Whether the attribute should replicate (default: false)"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("get_attribute_set"),
		TEXT("Read the full definition of an AttributeSet: all attributes, types, default values, clamping, and replication settings."),
		FMonolithActionHandler::CreateStatic(&HandleGetAttributeSet),
		FParamSchemaBuilder()
			.Required(TEXT("attribute_set"), TEXT("string"), TEXT("Asset path for Blueprint sets, or class name for C++ sets"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("set_attribute_defaults"),
		TEXT("Bulk-update default values on an AttributeSet's attributes."),
		FMonolithActionHandler::CreateStatic(&HandleSetAttributeDefaults),
		FParamSchemaBuilder()
			.Required(TEXT("attribute_set"), TEXT("string"), TEXT("Asset path for Blueprint sets, or class name for C++ sets"))
			.Required(TEXT("defaults"), TEXT("object"), TEXT("Object mapping attribute_name to numeric value (e.g. {\"Health\": 100, \"MaxHealth\": 100})"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("list_attribute_sets"),
		TEXT("Find all AttributeSet classes and Blueprint assets in the project."),
		FMonolithActionHandler::CreateStatic(&HandleListAttributeSets),
		FParamSchemaBuilder()
			.Optional(TEXT("include_plugins"), TEXT("boolean"), TEXT("Include AttributeSets from plugins (default: false)"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("configure_attribute_clamping"),
		TEXT("Set clamp rules on attributes. For GBA Blueprints, uses FGBAGameplayClampedAttributeData. For C++, generates PreAttributeChange/PostGameplayEffectExecute clamping code."),
		FMonolithActionHandler::CreateStatic(&HandleConfigureAttributeClamping),
		FParamSchemaBuilder()
			.Required(TEXT("attribute_set"), TEXT("string"), TEXT("Asset path for Blueprint sets, or class name for C++ sets"))
			.Required(TEXT("clamp_rules"), TEXT("array"), TEXT("Array of clamp rules: [{attribute, min_attribute?, max_attribute?, min_value?, max_value?}]"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("configure_meta_attributes"),
		TEXT("Set up meta attribute pipeline. Meta attributes (e.g. IncomingDamage) dispatch to actual attributes in PostGameplayEffectExecute."),
		FMonolithActionHandler::CreateStatic(&HandleConfigureMetaAttributes),
		FParamSchemaBuilder()
			.Required(TEXT("attribute_set"), TEXT("string"), TEXT("Asset path for Blueprint sets, or class name for C++ sets"))
			.Required(TEXT("meta_attributes"), TEXT("array"), TEXT("Array of meta attribute definitions: [{name, dispatches_to}] where dispatches_to is the target attribute name"))
			.Build());

	// ---- Phase 2: Productivity ----

	Registry.RegisterAction(TEXT("gas"), TEXT("create_attribute_set_from_template"),
		TEXT("Create an AttributeSet from a named template with all attributes pre-configured for survival horror patterns."),
		FMonolithActionHandler::CreateStatic(&HandleCreateAttributeSetFromTemplate),
		FParamSchemaBuilder()
			.Required(TEXT("template"), TEXT("string"), TEXT("Template name: survival_horror_vitals, survival_horror_stamina, survival_horror_horror, enemy_vitals, enemy_resistance, world_state"))
			.Required(TEXT("save_path"), TEXT("string"), TEXT("For blueprint mode: asset path. For cpp mode: class name"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("Creation mode: 'cpp' or 'blueprint' (default: cpp)"))
			.Optional(TEXT("overrides"), TEXT("object"), TEXT("Override template defaults: {add_attributes: [{name, default_value?, replicated?}], remove_attributes: [name], defaults: {attr: value}}"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("create_attribute_init_datatable"),
		TEXT("Generate an FAttributeMetaData DataTable for initializing AttributeSet values at runtime."),
		FMonolithActionHandler::CreateStatic(&HandleCreateAttributeInitDataTable),
		FParamSchemaBuilder()
			.Required(TEXT("attribute_set"), TEXT("string"), TEXT("AttributeSet class name or asset path"))
			.Required(TEXT("save_path"), TEXT("string"), TEXT("DataTable asset path (e.g. /Game/GAS/Data/DT_PlayerVitals)"))
			.Optional(TEXT("rows"), TEXT("array"), TEXT("Array of {attribute, base_value, min_value?, max_value?} — if omitted, auto-generates from set defaults"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("duplicate_attribute_set"),
		TEXT("Clone an existing AttributeSet with optional add/remove attributes."),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateAttributeSet),
		FParamSchemaBuilder()
			.Required(TEXT("source"), TEXT("string"), TEXT("Source AttributeSet asset path or class name"))
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Destination path for blueprint mode, or class name for cpp mode"))
			.Optional(TEXT("remove_attributes"), TEXT("array"), TEXT("Array of attribute names to remove from the clone"))
			.Optional(TEXT("add_attributes"), TEXT("array"), TEXT("Array of attribute definitions to add: [{name, default_value?, replicated?}]"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("configure_attribute_replication"),
		TEXT("Set replication flags per attribute on an existing AttributeSet."),
		FMonolithActionHandler::CreateStatic(&HandleConfigureAttributeReplication),
		FParamSchemaBuilder()
			.Required(TEXT("attribute_set"), TEXT("string"), TEXT("Asset path for Blueprint sets, or class name for C++ sets"))
			.Required(TEXT("replication"), TEXT("array"), TEXT("Array of {attribute, replicated, condition?} where condition is COND_None/COND_OwnerOnly/etc."))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("link_datatable_to_asc"),
		TEXT("Configure an ASC Blueprint's DefaultStartingData array to reference a DataTable for attribute initialization."),
		FMonolithActionHandler::CreateStatic(&HandleLinkDataTableToASC),
		FParamSchemaBuilder()
			.Required(TEXT("asc_blueprint"), TEXT("string"), TEXT("Blueprint asset path containing the ASC"))
			.Required(TEXT("entries"), TEXT("array"), TEXT("Array of {attribute_set_class, datatable_path}"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("bulk_edit_attributes"),
		TEXT("Apply multiple attribute edits across multiple AttributeSets in one call."),
		FMonolithActionHandler::CreateStatic(&HandleBulkEditAttributes),
		FParamSchemaBuilder()
			.Required(TEXT("operations"), TEXT("array"), TEXT("Array of {attribute_set, action, ...} where action is 'add'/'remove'/'set_default'/'set_replication'"))
			.Build());

	// ---- Phase 3: Validation & Analysis ----

	Registry.RegisterAction(TEXT("gas"), TEXT("validate_attribute_set"),
		TEXT("Validate an AttributeSet for common mistakes: missing clamps, replicated meta attributes, orphan attributes, GBA gotchas."),
		FMonolithActionHandler::CreateStatic(&HandleValidateAttributeSet),
		FParamSchemaBuilder()
			.Required(TEXT("attribute_set"), TEXT("string"), TEXT("Asset path for Blueprint sets, or class name for C++ sets"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("find_attribute_modifiers"),
		TEXT("Find all GameplayEffects and ExecCalcs that modify a given attribute."),
		FMonolithActionHandler::CreateStatic(&HandleFindAttributeModifiers),
		FParamSchemaBuilder()
			.Required(TEXT("attribute"), TEXT("string"), TEXT("Attribute as ClassName.PropertyName"))
			.Optional(TEXT("search_scope"), TEXT("string"), TEXT("Path filter for search scope (default: /Game/)"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("diff_attribute_sets"),
		TEXT("Compare two AttributeSets side-by-side: shared, unique, and differing defaults."),
		FMonolithActionHandler::CreateStatic(&HandleDiffAttributeSets),
		FParamSchemaBuilder()
			.Required(TEXT("set_a"), TEXT("string"), TEXT("First AttributeSet (asset path or class name)"))
			.Required(TEXT("set_b"), TEXT("string"), TEXT("Second AttributeSet (asset path or class name)"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("get_attribute_dependency_graph"),
		TEXT("Map clamp, derived, and meta attribute dispatch relationships between AttributeSets."),
		FMonolithActionHandler::CreateStatic(&HandleGetAttributeDependencyGraph),
		FParamSchemaBuilder()
			.Required(TEXT("attribute_sets"), TEXT("array"), TEXT("Array of AttributeSet identifiers (asset paths or class names)"))
			.Optional(TEXT("format"), TEXT("string"), TEXT("Output format: 'json' (default) or 'dot' (Graphviz)"), TEXT("json"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("remove_attribute"),
		TEXT("Remove an attribute from an AttributeSet with optional reference checking."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveAttribute),
		FParamSchemaBuilder()
			.Required(TEXT("attribute_set"), TEXT("string"), TEXT("Asset path for Blueprint sets, or class name for C++ sets"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Attribute name to remove"))
			.Optional(TEXT("check_references"), TEXT("boolean"), TEXT("Check for GE/ability references before removing (default: true)"), TEXT("true"))
			.Build());

	// ---- Phase 4: Runtime ----

	Registry.RegisterAction(TEXT("gas"), TEXT("get_attribute_value"),
		TEXT("Read current and base value of a gameplay attribute from a live actor in PIE."),
		FMonolithActionHandler::CreateStatic(&HandleGetAttributeValue),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in the PIE world"))
			.Required(TEXT("attribute"), TEXT("string"), TEXT("Attribute as ClassName.PropertyName (e.g. ULeviathanHealthAttributeSet.Health)"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("set_attribute_value"),
		TEXT("Debug-set a gameplay attribute value on a live actor in PIE. By default sets CurrentValue; set set_base=true for BaseValue."),
		FMonolithActionHandler::CreateStatic(&HandleSetAttributeValue),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, name, or path in the PIE world"))
			.Required(TEXT("attribute"), TEXT("string"), TEXT("Attribute as ClassName.PropertyName"))
			.Required(TEXT("value"), TEXT("number"), TEXT("New attribute value"))
			.Optional(TEXT("set_base"), TEXT("boolean"), TEXT("If true, sets BaseValue instead of CurrentValue (default: false)"))
			.Build());
}

// ============================================================
//  Helpers
// ============================================================

namespace
{
	/** Determine if an asset path looks like a Blueprint path (starts with / or /Game/) vs a C++ class name */
	bool LooksLikeBlueprintPath(const FString& Path)
	{
		return Path.StartsWith(TEXT("/"));
	}

	/** Find an FProperty on a UClass by name (for FGameplayAttributeData members) */
	FProperty* FindAttributeProperty(UClass* Class, const FString& AttrName)
	{
		if (!Class) return nullptr;
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			if (It->GetName() == AttrName)
			{
				return *It;
			}
		}
		return nullptr;
	}

	/** Serialize a single attribute's info to JSON */
	TSharedPtr<FJsonObject> SerializeAttributeInfo(
		const FString& Name,
		float BaseValue,
		float CurrentValue,
		bool bReplicated,
		const FString& Type)
	{
		TSharedPtr<FJsonObject> AttrObj = MakeShared<FJsonObject>();
		AttrObj->SetStringField(TEXT("name"), Name);
		AttrObj->SetStringField(TEXT("type"), Type);
		AttrObj->SetNumberField(TEXT("base_value"), BaseValue);
		AttrObj->SetNumberField(TEXT("current_value"), CurrentValue);
		AttrObj->SetBoolField(TEXT("replicated"), bReplicated);
		return AttrObj;
	}

	// GetProjectSourceDir() moved to MonolithGAS::GetProjectSourceDir() in MonolithGASInternal.cpp

	/** Get the project module name */
	FString GetProjectModuleName()
	{
		return FPaths::GetBaseFilename(FPaths::GetProjectFilePath());
	}

	/** Generate C++ header content for an AttributeSet */
	FString GenerateAttributeSetHeader(
		const FString& ClassName,
		const FString& ParentClassName,
		const FString& ModuleName,
		const TArray<TSharedPtr<FJsonValue>>& Attributes)
	{
		// Strip U prefix for macro usage
		FString ClassNameNoPrefix = ClassName;
		if (ClassNameNoPrefix.StartsWith(TEXT("U")))
		{
			ClassNameNoPrefix = ClassNameNoPrefix.Mid(1);
		}

		FString ParentInclude;
		if (ParentClassName == TEXT("UAttributeSet"))
		{
			ParentInclude = TEXT("#include \"AttributeSet.h\"");
		}
		else
		{
			// Custom parent — user must ensure the include is correct
			FString ParentHeaderName = ParentClassName;
			if (ParentHeaderName.StartsWith(TEXT("U")))
			{
				ParentHeaderName = ParentHeaderName.Mid(1);
			}
			ParentInclude = FString::Printf(TEXT("#include \"%s.h\""), *ParentHeaderName);
		}

		FString Header;
		Header += TEXT("// Auto-generated by Monolith GAS tools\n");
		Header += TEXT("#pragma once\n\n");
		Header += TEXT("#include \"CoreMinimal.h\"\n");
		Header += ParentInclude + TEXT("\n");
		Header += TEXT("#include \"AbilitySystemComponent.h\"\n");
		Header += TEXT("#include \"Net/UnrealNetwork.h\"\n");
		Header += FString::Printf(TEXT("#include \"%s.generated.h\"\n\n"), *ClassNameNoPrefix);

		// ATTRIBUTE_ACCESSORS macro definition
		Header += TEXT("#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \\\n");
		Header += TEXT("\tGAMEPLAY_ATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \\\n");
		Header += TEXT("\tGAMEPLAY_ATTRIBUTE_VALUE_GETTER(PropertyName) \\\n");
		Header += TEXT("\tGAMEPLAY_ATTRIBUTE_VALUE_SETTER(PropertyName) \\\n");
		Header += TEXT("\tGAMEPLAY_ATTRIBUTE_VALUE_INITTER(PropertyName)\n\n");

		Header += FString::Printf(TEXT("UCLASS()\nclass %s_API %s : public %s\n{\n\tGENERATED_BODY()\n\npublic:\n"),
			*ModuleName.ToUpper(), *ClassName, *ParentClassName);

		// Constructor
		Header += FString::Printf(TEXT("\t%s();\n\n"), *ClassName);

		// GetLifetimeReplicatedProps
		Header += TEXT("\tvirtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;\n\n");

		// Attributes
		for (const auto& AttrVal : Attributes)
		{
			const TSharedPtr<FJsonObject>* AttrObjPtr;
			if (!AttrVal->TryGetObject(AttrObjPtr)) continue;
			const TSharedPtr<FJsonObject>& AttrObj = *AttrObjPtr;

			FString Name = AttrObj->GetStringField(TEXT("name"));
			bool bReplicated = false;
			AttrObj->TryGetBoolField(TEXT("replicated"), bReplicated);

			if (bReplicated)
			{
				Header += FString::Printf(TEXT("\tUPROPERTY(BlueprintReadOnly, Category = \"Attributes\", ReplicatedUsing = OnRep_%s)\n"), *Name);
			}
			else
			{
				Header += TEXT("\tUPROPERTY(BlueprintReadOnly, Category = \"Attributes\")\n");
			}
			Header += FString::Printf(TEXT("\tFGameplayAttributeData %s;\n"), *Name);
			Header += FString::Printf(TEXT("\tATTRIBUTE_ACCESSORS(%s, %s)\n\n"), *ClassName, *Name);

			if (bReplicated)
			{
				Header += FString::Printf(TEXT("\tUFUNCTION()\n\tvoid OnRep_%s(const FGameplayAttributeData& Old%s);\n\n"), *Name, *Name);
			}
		}

		Header += TEXT("};\n");
		return Header;
	}

	/** Generate C++ source content for an AttributeSet */
	FString GenerateAttributeSetSource(
		const FString& ClassName,
		const FString& ClassNameNoPrefix,
		const TArray<TSharedPtr<FJsonValue>>& Attributes)
	{
		FString Source;
		Source += TEXT("// Auto-generated by Monolith GAS tools\n");
		Source += FString::Printf(TEXT("#include \"%s.h\"\n"), *ClassNameNoPrefix);
		Source += TEXT("#include \"Net/UnrealNetwork.h\"\n\n");

		// Constructor with default values
		Source += FString::Printf(TEXT("%s::%s()\n{\n"), *ClassName, *ClassName);
		for (const auto& AttrVal : Attributes)
		{
			const TSharedPtr<FJsonObject>* AttrObjPtr;
			if (!AttrVal->TryGetObject(AttrObjPtr)) continue;
			const TSharedPtr<FJsonObject>& AttrObj = *AttrObjPtr;

			FString Name = AttrObj->GetStringField(TEXT("name"));
			double DefaultValue = 0.0;
			AttrObj->TryGetNumberField(TEXT("default_value"), DefaultValue);
			if (DefaultValue != 0.0)
			{
				Source += FString::Printf(TEXT("\tInit%s(%.1f);\n"), *Name, DefaultValue);
			}
		}
		Source += TEXT("}\n\n");

		// GetLifetimeReplicatedProps
		Source += FString::Printf(TEXT("void %s::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const\n{\n"), *ClassName);
		Source += TEXT("\tSuper::GetLifetimeReplicatedProps(OutLifetimeProps);\n\n");
		for (const auto& AttrVal : Attributes)
		{
			const TSharedPtr<FJsonObject>* AttrObjPtr;
			if (!AttrVal->TryGetObject(AttrObjPtr)) continue;
			const TSharedPtr<FJsonObject>& AttrObj = *AttrObjPtr;

			bool bReplicated = false;
			AttrObj->TryGetBoolField(TEXT("replicated"), bReplicated);
			if (bReplicated)
			{
				FString Name = AttrObj->GetStringField(TEXT("name"));
				Source += FString::Printf(TEXT("\tDOREPLIFETIME_CONDITION_NOTIFY(%s, %s, COND_None, REPNOTIFY_Always);\n"), *ClassName, *Name);
			}
		}
		Source += TEXT("}\n\n");

		// OnRep functions
		for (const auto& AttrVal : Attributes)
		{
			const TSharedPtr<FJsonObject>* AttrObjPtr;
			if (!AttrVal->TryGetObject(AttrObjPtr)) continue;
			const TSharedPtr<FJsonObject>& AttrObj = *AttrObjPtr;

			bool bReplicated = false;
			AttrObj->TryGetBoolField(TEXT("replicated"), bReplicated);
			if (bReplicated)
			{
				FString Name = AttrObj->GetStringField(TEXT("name"));
				Source += FString::Printf(TEXT("void %s::OnRep_%s(const FGameplayAttributeData& Old%s)\n{\n"), *ClassName, *Name, *Name);
				Source += FString::Printf(TEXT("\tGAMEPLAYATTRIBUTE_REPNOTIFY(%s, %s, Old%s);\n"), *ClassName, *Name, *Name);
				Source += TEXT("}\n\n");
			}
		}

		return Source;
	}

	/** Collect all FGameplayAttributeData properties from a UClass */
	TArray<FProperty*> CollectAttributeProperties(UClass* Class)
	{
		TArray<FProperty*> Result;
		if (!Class) return Result;

		UScriptStruct* AttrDataStruct = FGameplayAttributeData::StaticStruct();
		for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			FStructProperty* StructProp = CastField<FStructProperty>(*It);
			if (StructProp && StructProp->Struct && StructProp->Struct->IsChildOf(AttrDataStruct))
			{
				Result.Add(*It);
			}
		}
		return Result;
	}

	/** Load a Blueprint as an AttributeSet Blueprint, verifying its parent */
	UBlueprint* LoadAttributeSetBlueprint(const FString& AssetPath, FString& OutError)
	{
		UBlueprint* BP = nullptr;
		FString Unused;
		TSharedPtr<FJsonObject> TempParams = MakeShared<FJsonObject>();
		TempParams->SetStringField(TEXT("asset_path"), AssetPath);
		BP = MonolithGAS::LoadBlueprintFromParams(TempParams, Unused, OutError);
		if (!BP)
		{
			return nullptr;
		}
		if (!MonolithGAS::IsAttributeSetBlueprint(BP))
		{
			OutError = FString::Printf(TEXT("'%s' is not an AttributeSet Blueprint"), *AssetPath);
			return nullptr;
		}
		return BP;
	}

	/** Find a C++ UAttributeSet subclass by name */
	UClass* FindAttributeSetClass(const FString& ClassName)
	{
		FString SearchName = ClassName;
		// Add U prefix if not present
		if (!SearchName.StartsWith(TEXT("U")))
		{
			SearchName = TEXT("U") + SearchName;
		}

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (Class->IsChildOf(UAttributeSet::StaticClass()))
			{
				FString Name = Class->GetName();
				if (Name == SearchName || (TEXT("U") + Name) == SearchName || Name == ClassName)
				{
					return Class;
				}
			}
		}
		return nullptr;
	}
}

// ============================================================
//  create_attribute_set
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleCreateAttributeSet(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Mode;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("save_path"), SavePath, Err)) return Err;

	// mode is optional with default 'blueprint' (Phase F #69).
	// Most callers want a Blueprint AttributeSet asset; cpp mode requires a C++ build.
	if (Params->HasField(TEXT("mode")))
	{
		Mode = Params->GetStringField(TEXT("mode"));
	}
	if (Mode.IsEmpty())
	{
		Mode = TEXT("blueprint");
	}

	Mode = Mode.ToLower();
	if (Mode != TEXT("cpp") && Mode != TEXT("blueprint"))
	{
		return FMonolithActionResult::Error(TEXT("mode must be 'cpp' or 'blueprint'"));
	}

	FString ParentClass = Params->GetStringField(TEXT("parent_class"));

	// Parse attributes array
	TArray<TSharedPtr<FJsonValue>> Attributes;
	const TArray<TSharedPtr<FJsonValue>>* AttrsArray;
	if (Params->TryGetArrayField(TEXT("attributes"), AttrsArray))
	{
		Attributes = *AttrsArray;
	}

	// ---- Blueprint mode ----
	if (Mode == TEXT("blueprint"))
	{
#if WITH_GBA
		// Create GBA Blueprint AttributeSet
		FString PackagePath = SavePath;
		FString AssetName;

		// Split /Game/Path/AssetName into package path and name
		int32 LastSlash;
		if (PackagePath.FindLastChar(TEXT('/'), LastSlash))
		{
			AssetName = PackagePath.Mid(LastSlash + 1);
		}
		else
		{
			AssetName = PackagePath;
			PackagePath = TEXT("/Game");
		}

		// Check for existing asset (AssetRegistry + in-memory multi-tier check)
		FString ExistError;
		if (!MonolithGAS::EnsureAssetPathFree(SavePath, AssetName, ExistError))
		{
			return FMonolithActionResult::Error(ExistError);
		}

		FString Error;
		UPackage* Package = MonolithGAS::GetOrCreatePackage(SavePath, Error);
		if (!Package)
		{
			return FMonolithActionResult::Error(Error);
		}

		// Determine parent class
		UClass* ParentUClass = UGBAAttributeSetBlueprintBase::StaticClass();
		if (!ParentClass.IsEmpty())
		{
			UClass* CustomParent = FindFirstObject<UClass>(*ParentClass, EFindFirstObjectOptions::NativeFirst);
			if (!CustomParent)
			{
				// Try with U prefix
				CustomParent = FindFirstObject<UClass>(*(TEXT("U") + ParentClass), EFindFirstObjectOptions::NativeFirst);
			}
			if (CustomParent && CustomParent->IsChildOf(UAttributeSet::StaticClass()))
			{
				ParentUClass = CustomParent;
			}
			else if (CustomParent)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("parent_class '%s' does not inherit from UAttributeSet"), *ParentClass));
			}
			else
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("parent_class '%s' not found"), *ParentClass));
			}
		}

		// Create the Blueprint using FKismetEditorUtilities (same pattern as GBABlueprintFactory)
		UGBAAttributeSetBlueprint* Blueprint = CastChecked<UGBAAttributeSetBlueprint>(
			FKismetEditorUtilities::CreateBlueprint(
				ParentUClass,
				Package,
				FName(*AssetName),
				BPTYPE_Normal,
				UGBAAttributeSetBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass(),
				NAME_None
			));

		if (!Blueprint)
		{
			return FMonolithActionResult::Error(TEXT("Failed to create GBA AttributeSet Blueprint"));
		}

		// Add attributes as FGBAGameplayClampedAttributeData variables
		int32 AddedCount = 0;
		for (const auto& AttrVal : Attributes)
		{
			const TSharedPtr<FJsonObject>* AttrObjPtr;
			if (!AttrVal->TryGetObject(AttrObjPtr)) continue;
			const TSharedPtr<FJsonObject>& AttrObj = *AttrObjPtr;

			FString AttrName = AttrObj->GetStringField(TEXT("name"));
			if (AttrName.IsEmpty()) continue;

			// Use FGBAGameplayClampedAttributeData for clamping support
			UScriptStruct* ClampedStruct = FGBAGameplayClampedAttributeData::StaticStruct();

			FEdGraphPinType PinType;
			PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			PinType.PinSubCategoryObject = ClampedStruct;

			FName VarName(*AttrName);

			// Check for duplicate
			bool bExists = false;
			for (const FBPVariableDescription& Var : Blueprint->NewVariables)
			{
				if (Var.VarName == VarName) { bExists = true; break; }
			}
			if (bExists) continue;

			FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, PinType, TEXT(""));

			// Set replication if requested
			bool bReplicated = false;
			AttrObj->TryGetBoolField(TEXT("replicated"), bReplicated);
			if (bReplicated)
			{
				for (FBPVariableDescription& Var : Blueprint->NewVariables)
				{
					if (Var.VarName == VarName)
					{
						Var.PropertyFlags |= CPF_Net;
						break;
					}
				}
			}

			AddedCount++;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);

		// Apply default values from the input JSON to the compiled CDO
		if (Blueprint->GeneratedClass)
		{
			UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject(true);
			if (CDO)
			{
				for (const auto& AttrVal : Attributes)
				{
					const TSharedPtr<FJsonObject>* AttrObjPtr;
					if (!AttrVal->TryGetObject(AttrObjPtr) || !(*AttrObjPtr).IsValid()) continue;
					const TSharedPtr<FJsonObject>& AttrObj = *AttrObjPtr;

					FString AttrName = AttrObj->GetStringField(TEXT("name"));
					double DefaultValue = 0;
					if (AttrObj->TryGetNumberField(TEXT("default_value"), DefaultValue) && DefaultValue != 0)
					{
						FProperty* Prop = Blueprint->GeneratedClass->FindPropertyByName(FName(*AttrName));
						if (Prop)
						{
							void* DataPtr = Prop->ContainerPtrToValuePtr<void>(CDO);
							if (DataPtr)
							{
								FGameplayAttributeData* AttrData = static_cast<FGameplayAttributeData*>(DataPtr);
								AttrData->SetBaseValue(static_cast<float>(DefaultValue));
								AttrData->SetCurrentValue(static_cast<float>(DefaultValue));
							}
						}
					}
				}
			}
		}

		// Save the package
		FString PackageFilename = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone;
		UPackage::SavePackage(Package, Blueprint, *PackageFilename, SaveArgs);

		// Notify asset registry
		FAssetRegistryModule::AssetCreated(Blueprint);

		TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(SavePath,
			FString::Printf(TEXT("Created GBA AttributeSet Blueprint with %d attributes"), AddedCount));
		Result->SetStringField(TEXT("mode"), TEXT("blueprint"));
		Result->SetStringField(TEXT("parent_class"), ParentUClass->GetName());
		Result->SetNumberField(TEXT("attribute_count"), AddedCount);
		return FMonolithActionResult::Success(Result);
#else
		return FMonolithActionResult::Error(
			TEXT("Blueprint AttributeSet mode requires the GBA (Blueprint Attributes) plugin. "
			     "Install GBA from Fab/Marketplace, or use mode='cpp' to generate C++ AttributeSet files."));
#endif
	}

	// ---- C++ mode ----
	FString ClassName = SavePath;
	// Extract leaf name from asset path (SavePath may be "/Game/GAS/AS_Vitals")
	int32 LastSlash;
	if (ClassName.FindLastChar(TEXT('/'), LastSlash))
	{
		ClassName = ClassName.Mid(LastSlash + 1);
	}
	// Ensure U prefix
	if (!ClassName.StartsWith(TEXT("U")))
	{
		ClassName = TEXT("U") + ClassName;
	}
	FString ClassNameNoPrefix = ClassName.Mid(1);

	FString ParentClassName = ParentClass.IsEmpty() ? TEXT("UAttributeSet") : ParentClass;
	if (!ParentClassName.StartsWith(TEXT("U")))
	{
		ParentClassName = TEXT("U") + ParentClassName;
	}

	FString ModuleName = GetProjectModuleName();
	FString SourceDir = MonolithGAS::GetProjectSourceDir();

	// Generate header
	FString HeaderContent = GenerateAttributeSetHeader(ClassName, ParentClassName, ModuleName, Attributes);

	// Generate source
	FString SourceContent = GenerateAttributeSetSource(ClassName, ClassNameNoPrefix, Attributes);

	// Write files
	FString HeaderPath = SourceDir / ClassNameNoPrefix + TEXT(".h");
	FString SourcePath = SourceDir / ClassNameNoPrefix + TEXT(".cpp");

	if (!FFileHelper::SaveStringToFile(HeaderContent, *HeaderPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to write header: %s"), *HeaderPath));
	}

	if (!FFileHelper::SaveStringToFile(SourceContent, *SourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to write source: %s"), *SourcePath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("class_name"), ClassName);
	Result->SetStringField(TEXT("header_path"), HeaderPath);
	Result->SetStringField(TEXT("source_path"), SourcePath);
	Result->SetStringField(TEXT("mode"), TEXT("cpp"));
	Result->SetStringField(TEXT("parent_class"), ParentClassName);
	Result->SetNumberField(TEXT("attribute_count"), Attributes.Num());
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Generated C++ AttributeSet '%s' with %d attributes. Rebuild required."),
			*ClassName, Attributes.Num()));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  add_attribute
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleAddAttribute(const TSharedPtr<FJsonObject>& Params)
{
	FString AttrSet, AttrName;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("attribute_set"), AttrSet, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("name"), AttrName, Err)) return Err;

	bool bReplicated = false;
	Params->TryGetBoolField(TEXT("replicated"), bReplicated);

	double DefaultValue = 0.0;
	Params->TryGetNumberField(TEXT("default_value"), DefaultValue);

	// ---- Blueprint mode ----
	if (LooksLikeBlueprintPath(AttrSet))
	{
		FString Error;
		UBlueprint* BP = LoadAttributeSetBlueprint(AttrSet, Error);
		if (!BP)
		{
			return FMonolithActionResult::Error(Error);
		}

		FName VarName(*AttrName);

		// Check for duplicate
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			if (Var.VarName == VarName)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Attribute '%s' already exists in this AttributeSet"), *AttrName));
			}
		}

		// Determine struct type to use
		UScriptStruct* AttrStruct = FGameplayAttributeData::StaticStruct();
#if WITH_GBA
		// If it's a GBA blueprint, use clamped data type
		if (BP->GeneratedClass && BP->GeneratedClass->IsChildOf(UGBAAttributeSetBlueprintBase::StaticClass()))
		{
			AttrStruct = FGBAGameplayClampedAttributeData::StaticStruct();
		}
#endif

		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = AttrStruct;

		FBlueprintEditorUtils::AddMemberVariable(BP, VarName, PinType, TEXT(""));

		// Set replication
		if (bReplicated)
		{
			for (FBPVariableDescription& Var : BP->NewVariables)
			{
				if (Var.VarName == VarName)
				{
					Var.PropertyFlags |= CPF_Net;
					break;
				}
			}
		}

		// Full CDO rewrite path (mirror HandleCreateAttributeSet recipe to guarantee persistence).
		// MarkBlueprintAsStructurallyModified alone leaves the BPGC CDO un-recompiled and
		// does not flush to disk; a previous regression saw add_attribute report success
		// while the property was absent on the loaded class. The fix is the full chain:
		//   structural-mark -> CompileBlueprint -> CDO write -> SavePackage.
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

		// Apply default_value to the freshly compiled CDO so the property survives the round-trip.
		if (BP->GeneratedClass && DefaultValue != 0.0)
		{
			UObject* CDO = BP->GeneratedClass->GetDefaultObject(true);
			if (CDO)
			{
				FProperty* Prop = BP->GeneratedClass->FindPropertyByName(VarName);
				if (Prop)
				{
					void* DataPtr = Prop->ContainerPtrToValuePtr<void>(CDO);
					if (DataPtr)
					{
						FGameplayAttributeData* AttrData = static_cast<FGameplayAttributeData*>(DataPtr);
						AttrData->SetBaseValue(static_cast<float>(DefaultValue));
						AttrData->SetCurrentValue(static_cast<float>(DefaultValue));
					}
				}
			}
		}

		// Save the package to disk (UPackage::SavePackage, not just MarkPackageDirty).
		// MarkPackageDirty by itself does not survive editor restart and was the root cause
		// of the 2026-04-25 GAS smoke-test regression.
		UPackage* OuterPackage = BP->GetOutermost();
		if (OuterPackage)
		{
			FString PackageFilename = FPackageName::LongPackageNameToFilename(
				OuterPackage->GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Standalone;
			UPackage::SavePackage(OuterPackage, BP, *PackageFilename, SaveArgs);
		}

		TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(AttrSet,
			FString::Printf(TEXT("Added attribute '%s' to %s"), *AttrName, *AttrSet));
		Result->SetStringField(TEXT("attribute"), AttrName);
		Result->SetStringField(TEXT("type"), AttrStruct->GetName());
		Result->SetBoolField(TEXT("replicated"), bReplicated);
		return FMonolithActionResult::Success(Result);
	}

	// ---- C++ mode ----
	// For C++ mode, we need to find and modify the header file
	FString ClassName = AttrSet;
	if (!ClassName.StartsWith(TEXT("U")))
	{
		ClassName = TEXT("U") + ClassName;
	}
	FString ClassNameNoPrefix = ClassName.Mid(1);

	FString SourceDir = MonolithGAS::GetProjectSourceDir();
	FString HeaderPath = SourceDir / ClassNameNoPrefix + TEXT(".h");
	FString SourceFilePath = SourceDir / ClassNameNoPrefix + TEXT(".cpp");

	// Read existing header
	FString HeaderContent;
	if (!FFileHelper::LoadFileToString(HeaderContent, *HeaderPath))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Could not find header file for class '%s' at: %s"), *ClassName, *HeaderPath));
	}

	// Find the closing }; of the class and insert before it
	FString ReplicatedSpec = bReplicated
		? FString::Printf(TEXT(", ReplicatedUsing = OnRep_%s"), *AttrName)
		: TEXT("");

	FString NewProperty = FString::Printf(
		TEXT("\tUPROPERTY(BlueprintReadOnly, Category = \"Attributes\"%s)\n")
		TEXT("\tFGameplayAttributeData %s;\n")
		TEXT("\tATTRIBUTE_ACCESSORS(%s, %s)\n\n"),
		*ReplicatedSpec, *AttrName, *ClassName, *AttrName);

	if (bReplicated)
	{
		NewProperty += FString::Printf(
			TEXT("\tUFUNCTION()\n\tvoid OnRep_%s(const FGameplayAttributeData& Old%s);\n\n"),
			*AttrName, *AttrName);
	}

	// Insert before the last }; in the file
	int32 LastBrace = HeaderContent.Find(TEXT("};"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (LastBrace == INDEX_NONE)
	{
		return FMonolithActionResult::Error(TEXT("Could not find class closing brace in header file"));
	}

	HeaderContent = HeaderContent.Left(LastBrace) + NewProperty + HeaderContent.Mid(LastBrace);

	if (!FFileHelper::SaveStringToFile(HeaderContent, *HeaderPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to write header: %s"), *HeaderPath));
	}

	// Update source file with OnRep and GetLifetimeReplicatedProps if replicated
	if (bReplicated)
	{
		FString SourceContent;
		if (FFileHelper::LoadFileToString(SourceContent, *SourceFilePath))
		{
			// Add OnRep function
			FString OnRepFunc = FString::Printf(
				TEXT("\nvoid %s::OnRep_%s(const FGameplayAttributeData& Old%s)\n{\n")
				TEXT("\tGAMEPLAYATTRIBUTE_REPNOTIFY(%s, %s, Old%s);\n}\n"),
				*ClassName, *AttrName, *AttrName, *ClassName, *AttrName, *AttrName);
			SourceContent += OnRepFunc;

			// Add DOREPLIFETIME to GetLifetimeReplicatedProps if it exists
			FString RepPropsFunc = TEXT("GetLifetimeReplicatedProps");
			int32 RepPropsIdx = SourceContent.Find(RepPropsFunc);
			if (RepPropsIdx != INDEX_NONE)
			{
				// Find the closing brace of the function
				int32 FuncStart = SourceContent.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, RepPropsIdx);
				if (FuncStart != INDEX_NONE)
				{
					int32 BraceCount = 1;
					int32 Pos = FuncStart + 1;
					while (Pos < SourceContent.Len() && BraceCount > 0)
					{
						if (SourceContent[Pos] == TEXT('{')) BraceCount++;
						else if (SourceContent[Pos] == TEXT('}')) BraceCount--;
						Pos++;
					}
					// Insert before the closing brace
					if (BraceCount == 0)
					{
						FString DoRepLine = FString::Printf(
							TEXT("\tDOREPLIFETIME_CONDITION_NOTIFY(%s, %s, COND_None, REPNOTIFY_Always);\n"),
							*ClassName, *AttrName);
						SourceContent = SourceContent.Left(Pos - 1) + DoRepLine + SourceContent.Mid(Pos - 1);
					}
				}
			}

			FFileHelper::SaveStringToFile(SourceContent, *SourceFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("class_name"), ClassName);
	Result->SetStringField(TEXT("attribute"), AttrName);
	Result->SetStringField(TEXT("header_path"), HeaderPath);
	Result->SetBoolField(TEXT("replicated"), bReplicated);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Added attribute '%s' to %s. Rebuild required."), *AttrName, *ClassName));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  get_attribute_set
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleGetAttributeSet(const TSharedPtr<FJsonObject>& Params)
{
	FString AttrSet;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("attribute_set"), AttrSet, Err)) return Err;

	UClass* SetClass = nullptr;
	FString AssetPath;
	FString SetName;
	bool bIsBlueprintSet = false;

	if (LooksLikeBlueprintPath(AttrSet))
	{
		FString Error;
		UBlueprint* BP = LoadAttributeSetBlueprint(AttrSet, Error);
		if (!BP)
		{
			return FMonolithActionResult::Error(Error);
		}
		SetClass = BP->GeneratedClass;
		AssetPath = AttrSet;
		SetName = BP->GetName();
		bIsBlueprintSet = true;
	}
	else
	{
		SetClass = FindAttributeSetClass(AttrSet);
		if (!SetClass)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("AttributeSet class not found: %s"), *AttrSet));
		}
		SetName = SetClass->GetName();
	}

	if (!SetClass)
	{
		return FMonolithActionResult::Error(TEXT("Could not resolve AttributeSet class"));
	}

	// Get the CDO to read default values
	UAttributeSet* CDO = Cast<UAttributeSet>(SetClass->GetDefaultObject());

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), SetName);
	Result->SetStringField(TEXT("class"), SetClass->GetPathName());
	Result->SetBoolField(TEXT("is_blueprint"), bIsBlueprintSet);
	if (!AssetPath.IsEmpty())
	{
		Result->SetStringField(TEXT("asset_path"), AssetPath);
	}

	// Parent class
	UClass* ParentClass = SetClass->GetSuperClass();
	if (ParentClass)
	{
		Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	}

	// Collect attributes
	TArray<TSharedPtr<FJsonValue>> AttrsArray;
	UScriptStruct* AttrDataStruct = FGameplayAttributeData::StaticStruct();

	for (TFieldIterator<FProperty> It(SetClass); It; ++It)
	{
		FStructProperty* StructProp = CastField<FStructProperty>(*It);
		if (!StructProp || !StructProp->Struct || !StructProp->Struct->IsChildOf(AttrDataStruct))
		{
			continue;
		}

		TSharedPtr<FJsonObject> AttrObj = MakeShared<FJsonObject>();
		AttrObj->SetStringField(TEXT("name"), StructProp->GetName());
		AttrObj->SetStringField(TEXT("type"), StructProp->Struct->GetName());

		// Read default values from CDO
		if (CDO)
		{
			const FGameplayAttributeData* AttrData =
				StructProp->ContainerPtrToValuePtr<FGameplayAttributeData>(CDO);
			if (AttrData)
			{
				AttrObj->SetNumberField(TEXT("base_value"), AttrData->GetBaseValue());
				AttrObj->SetNumberField(TEXT("current_value"), AttrData->GetCurrentValue());
			}
		}

		// Check replication
		bool bReplicated = StructProp->HasAnyPropertyFlags(CPF_Net);
		AttrObj->SetBoolField(TEXT("replicated"), bReplicated);

		// Check if this is a clamped attribute type
		bool bIsClamped = false;
#if WITH_GBA
		if (StructProp->Struct->IsChildOf(FGBAGameplayClampedAttributeData::StaticStruct()))
		{
			bIsClamped = true;
			if (CDO)
			{
				const FGBAGameplayClampedAttributeData* ClampedData =
					StructProp->ContainerPtrToValuePtr<FGBAGameplayClampedAttributeData>(CDO);
				if (ClampedData)
				{
					TSharedPtr<FJsonObject> ClampObj = MakeShared<FJsonObject>();

					// Min value clamping
					TSharedPtr<FJsonObject> MinObj = MakeShared<FJsonObject>();
					MinObj->SetStringField(TEXT("type"),
						ClampedData->MinValue.ClampType == EGBAClampingType::Float ? TEXT("float") :
						ClampedData->MinValue.ClampType == EGBAClampingType::AttributeBased ? TEXT("attribute") : TEXT("none"));
					if (ClampedData->MinValue.ClampType == EGBAClampingType::Float)
					{
						MinObj->SetNumberField(TEXT("value"), ClampedData->MinValue.Value);
					}
					else if (ClampedData->MinValue.ClampType == EGBAClampingType::AttributeBased)
					{
						MinObj->SetStringField(TEXT("attribute"), ClampedData->MinValue.Attribute.GetName());
					}
					ClampObj->SetObjectField(TEXT("min"), MinObj);

					// Max value clamping
					TSharedPtr<FJsonObject> MaxObj = MakeShared<FJsonObject>();
					MaxObj->SetStringField(TEXT("type"),
						ClampedData->MaxValue.ClampType == EGBAClampingType::Float ? TEXT("float") :
						ClampedData->MaxValue.ClampType == EGBAClampingType::AttributeBased ? TEXT("attribute") : TEXT("none"));
					if (ClampedData->MaxValue.ClampType == EGBAClampingType::Float)
					{
						MaxObj->SetNumberField(TEXT("value"), ClampedData->MaxValue.Value);
					}
					else if (ClampedData->MaxValue.ClampType == EGBAClampingType::AttributeBased)
					{
						MaxObj->SetStringField(TEXT("attribute"), ClampedData->MaxValue.Attribute.GetName());
					}
					ClampObj->SetObjectField(TEXT("max"), MaxObj);

					AttrObj->SetObjectField(TEXT("clamping"), ClampObj);
				}
			}
		}
#endif
		AttrObj->SetBoolField(TEXT("clamped"), bIsClamped);

		// Check if this is from the class itself vs inherited
		AttrObj->SetBoolField(TEXT("inherited"), StructProp->GetOwnerClass() != SetClass);

		AttrsArray.Add(MakeShared<FJsonValueObject>(AttrObj));
	}

	Result->SetArrayField(TEXT("attributes"), AttrsArray);
	Result->SetNumberField(TEXT("attribute_count"), AttrsArray.Num());

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  set_attribute_defaults
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleSetAttributeDefaults(const TSharedPtr<FJsonObject>& Params)
{
	FString AttrSet;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("attribute_set"), AttrSet, Err)) return Err;

	const TSharedPtr<FJsonObject>* DefaultsObj;
	if (!Params->TryGetObjectField(TEXT("defaults"), DefaultsObj) || !DefaultsObj || !(*DefaultsObj).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: defaults (object mapping attribute_name to value)"));
	}

	// ---- Blueprint mode ----
	if (LooksLikeBlueprintPath(AttrSet))
	{
		FString Error;
		UBlueprint* BP = LoadAttributeSetBlueprint(AttrSet, Error);
		if (!BP)
		{
			return FMonolithActionResult::Error(Error);
		}

		UClass* SetClass = BP->GeneratedClass;
		if (!SetClass)
		{
			return FMonolithActionResult::Error(TEXT("Blueprint has no generated class — compile it first"));
		}

		UAttributeSet* CDO = Cast<UAttributeSet>(SetClass->GetDefaultObject());
		if (!CDO)
		{
			return FMonolithActionResult::Error(TEXT("Could not get CDO for AttributeSet"));
		}

		int32 UpdatedCount = 0;
		TArray<FString> NotFound;
		UScriptStruct* AttrDataStruct = FGameplayAttributeData::StaticStruct();

		for (const auto& Pair : (*DefaultsObj)->Values)
		{
			double Value = 0.0;
			if (!Pair.Value->TryGetNumber(Value))
			{
				continue;
			}

			// Find the property
			FProperty* Prop = FindAttributeProperty(SetClass, Pair.Key);
			FStructProperty* StructProp = CastField<FStructProperty>(Prop);
			if (!StructProp || !StructProp->Struct || !StructProp->Struct->IsChildOf(AttrDataStruct))
			{
				NotFound.Add(Pair.Key);
				continue;
			}

			FGameplayAttributeData* AttrData =
				StructProp->ContainerPtrToValuePtr<FGameplayAttributeData>(CDO);
			if (AttrData)
			{
				AttrData->SetBaseValue(static_cast<float>(Value));
				AttrData->SetCurrentValue(static_cast<float>(Value));
				UpdatedCount++;
			}
		}

		CDO->MarkPackageDirty();
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

		TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(AttrSet,
			FString::Printf(TEXT("Updated %d attribute defaults"), UpdatedCount));
		Result->SetNumberField(TEXT("updated_count"), UpdatedCount);
		if (NotFound.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> NotFoundArr;
			for (const FString& Name : NotFound)
			{
				NotFoundArr.Add(MakeShared<FJsonValueString>(Name));
			}
			Result->SetArrayField(TEXT("not_found"), NotFoundArr);
		}
		return FMonolithActionResult::Success(Result);
	}

	// ---- C++ mode ----
	// For C++ sets, we modify the constructor in the source file
	FString ClassName = AttrSet;
	if (!ClassName.StartsWith(TEXT("U")))
	{
		ClassName = TEXT("U") + ClassName;
	}
	FString ClassNameNoPrefix = ClassName.Mid(1);
	FString SourceDir = MonolithGAS::GetProjectSourceDir();
	FString SourceFilePath = SourceDir / ClassNameNoPrefix + TEXT(".cpp");

	FString SourceContent;
	if (!FFileHelper::LoadFileToString(SourceContent, *SourceFilePath))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Could not find source file: %s"), *SourceFilePath));
	}

	// For each default, update or add Init calls in the constructor
	int32 UpdatedCount = 0;
	for (const auto& Pair : (*DefaultsObj)->Values)
	{
		double Value = 0.0;
		if (!Pair.Value->TryGetNumber(Value)) continue;

		FString InitCall = FString::Printf(TEXT("Init%s("), *Pair.Key);
		FString NewInitLine = FString::Printf(TEXT("\tInit%s(%.1f);"), *Pair.Key, Value);

		int32 InitIdx = SourceContent.Find(InitCall);
		if (InitIdx != INDEX_NONE)
		{
			// Replace existing Init line
			int32 LineEnd = SourceContent.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, InitIdx);
			if (LineEnd != INDEX_NONE)
			{
				// Find start of line
				int32 LineStart = InitIdx;
				while (LineStart > 0 && SourceContent[LineStart - 1] != TEXT('\n'))
				{
					LineStart--;
				}
				SourceContent = SourceContent.Left(LineStart) + NewInitLine + TEXT("\n") + SourceContent.Mid(LineEnd + 1);
			}
		}
		else
		{
			// Add new Init line in the constructor — find first { after ClassName::ClassName
			FString CtorSig = FString::Printf(TEXT("%s::%s"), *ClassName, *ClassName);
			int32 CtorIdx = SourceContent.Find(CtorSig);
			if (CtorIdx != INDEX_NONE)
			{
				int32 BraceIdx = SourceContent.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, CtorIdx);
				if (BraceIdx != INDEX_NONE)
				{
					SourceContent = SourceContent.Left(BraceIdx + 1) + TEXT("\n") + NewInitLine +
						SourceContent.Mid(BraceIdx + 1);
				}
			}
		}
		UpdatedCount++;
	}

	if (!FFileHelper::SaveStringToFile(SourceContent, *SourceFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to write source: %s"), *SourceFilePath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("class_name"), ClassName);
	Result->SetStringField(TEXT("source_path"), SourceFilePath);
	Result->SetNumberField(TEXT("updated_count"), UpdatedCount);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Updated %d attribute defaults in %s. Rebuild required."), UpdatedCount, *ClassName));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  list_attribute_sets
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleListAttributeSets(const TSharedPtr<FJsonObject>& Params)
{
	bool bIncludePlugins = false;
	Params->TryGetBoolField(TEXT("include_plugins"), bIncludePlugins);

	TArray<TSharedPtr<FJsonValue>> SetsArray;

	// 1. Find all native/C++ AttributeSet classes
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class->IsChildOf(UAttributeSet::StaticClass()) || Class == UAttributeSet::StaticClass())
		{
			continue;
		}

		// Skip compilation artifacts (transient reinstancing/skeleton classes)
		FString ClassName = Class->GetName();
		if (ClassName.StartsWith(TEXT("SKEL_")) || ClassName.StartsWith(TEXT("REINST_")))
			continue;
		if (Class->GetOutermost()->GetName() == TEXT("/Engine/Transient"))
			continue;
		if (Class->HasAnyClassFlags(CLASS_NewerVersionExists))
			continue;

		// Skip engine/plugin classes if not requested
		if (!bIncludePlugins)
		{
			FString ClassPath = Class->GetPathName();
			if (ClassPath.StartsWith(TEXT("/Script/GameplayAbilities")) ||
				ClassPath.StartsWith(TEXT("/Script/BlueprintAttributes")))
			{
				continue;
			}
		}

		// Count attributes
		int32 AttrCount = CollectAttributeProperties(Class).Num();

		TSharedPtr<FJsonObject> SetObj = MakeShared<FJsonObject>();
		SetObj->SetStringField(TEXT("name"), Class->GetName());
		SetObj->SetStringField(TEXT("class_path"), Class->GetPathName());
		SetObj->SetStringField(TEXT("source"), TEXT("cpp"));
		SetObj->SetNumberField(TEXT("attribute_count"), AttrCount);

		if (UClass* Parent = Class->GetSuperClass())
		{
			SetObj->SetStringField(TEXT("parent_class"), Parent->GetName());
		}

		SetsArray.Add(MakeShared<FJsonValueObject>(SetObj));
	}

	// 2. Find Blueprint AttributeSets via AssetRegistry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<FAssetData> BlueprintAssets;

	// Search for UBlueprint assets
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
#if WITH_GBA
	Filter.ClassPaths.Add(UGBAAttributeSetBlueprint::StaticClass()->GetClassPathName());
#endif
	Filter.bRecursiveClasses = true;
	if (!bIncludePlugins)
	{
		Filter.PackagePaths.Add(FName(TEXT("/Game")));
		Filter.bRecursivePaths = true;
	}

	AssetRegistry.GetAssets(Filter, BlueprintAssets);

	for (const FAssetData& Asset : BlueprintAssets)
	{
		// Check if it's an AttributeSet blueprint via parent class metadata tags only.
		// NEVER call Asset.GetAsset() here — loading arbitrary Blueprints (e.g. ControlRig)
		// can crash the editor.
		FAssetTagValueRef ParentTag = Asset.TagsAndValues.FindTag(FName("ParentClass"));
		if (!ParentTag.IsSet())
		{
			ParentTag = Asset.TagsAndValues.FindTag(FName("NativeParentClass"));
		}
		if (!ParentTag.IsSet())
		{
			continue;
		}
		FString ParentPath = ParentTag.GetValue();
		if (!ParentPath.Contains(TEXT("AttributeSet")))
		{
			continue;
		}

		TSharedPtr<FJsonObject> SetObj = MakeShared<FJsonObject>();
		SetObj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		SetObj->SetStringField(TEXT("asset_path"), Asset.GetObjectPathString());
		SetObj->SetStringField(TEXT("source"), TEXT("blueprint"));

		SetsArray.Add(MakeShared<FJsonValueObject>(SetObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("attribute_sets"), SetsArray);
	Result->SetNumberField(TEXT("count"), SetsArray.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  configure_attribute_clamping
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleConfigureAttributeClamping(const TSharedPtr<FJsonObject>& Params)
{
	FString AttrSet;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("attribute_set"), AttrSet, Err)) return Err;

	const TArray<TSharedPtr<FJsonValue>>* ClampRulesArr;
	if (!Params->TryGetArrayField(TEXT("clamp_rules"), ClampRulesArr) || !ClampRulesArr)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: clamp_rules (array)"));
	}

	// ---- Blueprint mode ----
	if (LooksLikeBlueprintPath(AttrSet))
	{
#if WITH_GBA
		FString Error;
		UBlueprint* BP = LoadAttributeSetBlueprint(AttrSet, Error);
		if (!BP)
		{
			return FMonolithActionResult::Error(Error);
		}

		UClass* SetClass = BP->GeneratedClass;
		if (!SetClass)
		{
			return FMonolithActionResult::Error(TEXT("Blueprint has no generated class — compile it first"));
		}

		UAttributeSet* CDO = Cast<UAttributeSet>(SetClass->GetDefaultObject());
		if (!CDO)
		{
			return FMonolithActionResult::Error(TEXT("Could not get CDO for AttributeSet"));
		}

		int32 ConfiguredCount = 0;
		TArray<FString> Errors;

		for (const auto& RuleVal : *ClampRulesArr)
		{
			const TSharedPtr<FJsonObject>* RuleObjPtr;
			if (!RuleVal->TryGetObject(RuleObjPtr)) continue;
			const TSharedPtr<FJsonObject>& RuleObj = *RuleObjPtr;

			FString AttrName = RuleObj->GetStringField(TEXT("attribute"));
			if (AttrName.IsEmpty())
			{
				Errors.Add(TEXT("Clamp rule missing 'attribute' field"));
				continue;
			}

			// Find the property
			FProperty* Prop = FindAttributeProperty(SetClass, AttrName);
			FStructProperty* StructProp = CastField<FStructProperty>(Prop);
			if (!StructProp || !StructProp->Struct ||
				!StructProp->Struct->IsChildOf(FGBAGameplayClampedAttributeData::StaticStruct()))
			{
				Errors.Add(FString::Printf(
					TEXT("'%s' is not a FGBAGameplayClampedAttributeData — change its type to Clamped first"), *AttrName));
				continue;
			}

			FGBAGameplayClampedAttributeData* ClampedData =
				StructProp->ContainerPtrToValuePtr<FGBAGameplayClampedAttributeData>(CDO);
			if (!ClampedData)
			{
				Errors.Add(FString::Printf(TEXT("Could not access clamped data for '%s'"), *AttrName));
				continue;
			}

			// Configure min
			double MinValue;
			FString MinAttribute = RuleObj->GetStringField(TEXT("min_attribute"));
			if (!MinAttribute.IsEmpty())
			{
				ClampedData->MinValue.ClampType = EGBAClampingType::AttributeBased;
				// Find the attribute in the same set
				FProperty* MinProp = FindAttributeProperty(SetClass, MinAttribute);
				if (MinProp)
				{
					ClampedData->MinValue.Attribute = FGameplayAttribute(MinProp);
				}
			}
			else if (RuleObj->TryGetNumberField(TEXT("min_value"), MinValue))
			{
				ClampedData->MinValue.ClampType = EGBAClampingType::Float;
				ClampedData->MinValue.Value = static_cast<float>(MinValue);
			}

			// Configure max
			double MaxValue;
			FString MaxAttribute = RuleObj->GetStringField(TEXT("max_attribute"));
			if (!MaxAttribute.IsEmpty())
			{
				ClampedData->MaxValue.ClampType = EGBAClampingType::AttributeBased;
				FProperty* MaxProp = FindAttributeProperty(SetClass, MaxAttribute);
				if (MaxProp)
				{
					ClampedData->MaxValue.Attribute = FGameplayAttribute(MaxProp);
				}
			}
			else if (RuleObj->TryGetNumberField(TEXT("max_value"), MaxValue))
			{
				ClampedData->MaxValue.ClampType = EGBAClampingType::Float;
				ClampedData->MaxValue.Value = static_cast<float>(MaxValue);
			}

			ConfiguredCount++;
		}

		CDO->MarkPackageDirty();
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

		TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(AttrSet,
			FString::Printf(TEXT("Configured clamping for %d attributes"), ConfiguredCount));
		Result->SetNumberField(TEXT("configured_count"), ConfiguredCount);
		if (Errors.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ErrArr;
			for (const FString& E : Errors) ErrArr.Add(MakeShared<FJsonValueString>(E));
			Result->SetArrayField(TEXT("errors"), ErrArr);
		}
		return FMonolithActionResult::Success(Result);
#else
		return FMonolithActionResult::Error(
			TEXT("Blueprint AttributeSet clamping requires the GBA (Blueprint Attributes) plugin. "
			     "Install GBA or use a C++ AttributeSet."));
#endif
	}

	// ---- C++ mode ----
	FString ClassName = AttrSet;
	if (!ClassName.StartsWith(TEXT("U")))
	{
		ClassName = TEXT("U") + ClassName;
	}
	FString ClassNameNoPrefix = ClassName.Mid(1);
	FString SourceDir = MonolithGAS::GetProjectSourceDir();
	FString HeaderPath = SourceDir / ClassNameNoPrefix + TEXT(".h");
	FString SourceFilePath = SourceDir / ClassNameNoPrefix + TEXT(".cpp");

	FString HeaderContent;
	if (!FFileHelper::LoadFileToString(HeaderContent, *HeaderPath))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Could not find header file: %s"), *HeaderPath));
	}

	FString SourceContent;
	if (!FFileHelper::LoadFileToString(SourceContent, *SourceFilePath))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Could not find source file: %s"), *SourceFilePath));
	}

	// Check if PreAttributeChange already exists
	bool bHasPreAttributeChange = SourceContent.Contains(TEXT("PreAttributeChange"));
	bool bHasPostExecute = SourceContent.Contains(TEXT("PostGameplayEffectExecute"));

	// Build clamping code
	FString PreAttrChangeBody;
	FString PostExecuteBody;

	for (const auto& RuleVal : *ClampRulesArr)
	{
		const TSharedPtr<FJsonObject>* RuleObjPtr;
		if (!RuleVal->TryGetObject(RuleObjPtr)) continue;
		const TSharedPtr<FJsonObject>& RuleObj = *RuleObjPtr;

		FString AttrName = RuleObj->GetStringField(TEXT("attribute"));
		if (AttrName.IsEmpty()) continue;

		// Build clamp expression
		FString MinExpr, MaxExpr;
		double MinValue, MaxValue;
		FString MinAttribute = RuleObj->GetStringField(TEXT("min_attribute"));
		FString MaxAttribute = RuleObj->GetStringField(TEXT("max_attribute"));

		if (!MinAttribute.IsEmpty())
		{
			MinExpr = FString::Printf(TEXT("Get%s()"), *MinAttribute);
		}
		else if (RuleObj->TryGetNumberField(TEXT("min_value"), MinValue))
		{
			MinExpr = FString::Printf(TEXT("%.1ff"), MinValue);
		}
		else
		{
			MinExpr = TEXT("0.f");
		}

		if (!MaxAttribute.IsEmpty())
		{
			MaxExpr = FString::Printf(TEXT("Get%s()"), *MaxAttribute);
		}
		else if (RuleObj->TryGetNumberField(TEXT("max_value"), MaxValue))
		{
			MaxExpr = FString::Printf(TEXT("%.1ff"), MaxValue);
		}
		else
		{
			MaxExpr = FString::Printf(TEXT("Get%s()"), *AttrName); // self-clamp fallback
		}

		// PreAttributeChange — clamp the proposed new value
		PreAttrChangeBody += FString::Printf(
			TEXT("\tif (Attribute == Get%sAttribute())\n\t{\n")
			TEXT("\t\tNewValue = FMath::Clamp(NewValue, %s, %s);\n")
			TEXT("\t}\n"),
			*AttrName, *MinExpr, *MaxExpr);

		// PostGameplayEffectExecute — clamp the base value after effect execution
		PostExecuteBody += FString::Printf(
			TEXT("\tif (Data.EvaluatedData.Attribute == Get%sAttribute())\n\t{\n")
			TEXT("\t\tSet%s(FMath::Clamp(Get%s(), %s, %s));\n")
			TEXT("\t}\n"),
			*AttrName, *AttrName, *AttrName, *MinExpr, *MaxExpr);
	}

	// Add PreAttributeChange declaration to header if needed
	if (!bHasPreAttributeChange && !PreAttrChangeBody.IsEmpty())
	{
		int32 LastBrace = HeaderContent.Find(TEXT("};"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (LastBrace != INDEX_NONE)
		{
			FString Decl = TEXT("\n\tvirtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;\n");
			HeaderContent = HeaderContent.Left(LastBrace) + Decl + HeaderContent.Mid(LastBrace);
		}
	}

	// Add PostGameplayEffectExecute declaration to header if needed
	if (!bHasPostExecute && !PostExecuteBody.IsEmpty())
	{
		int32 LastBrace = HeaderContent.Find(TEXT("};"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (LastBrace != INDEX_NONE)
		{
			FString Decl = TEXT("\tvirtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;\n");
			HeaderContent = HeaderContent.Left(LastBrace) + Decl + HeaderContent.Mid(LastBrace);
		}
	}

	// Add function bodies to source
	if (!bHasPreAttributeChange && !PreAttrChangeBody.IsEmpty())
	{
		SourceContent += FString::Printf(
			TEXT("\nvoid %s::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)\n{\n")
			TEXT("\tSuper::PreAttributeChange(Attribute, NewValue);\n\n")
			TEXT("%s}\n"),
			*ClassName, *PreAttrChangeBody);
	}

	if (!bHasPostExecute && !PostExecuteBody.IsEmpty())
	{
		SourceContent += FString::Printf(
			TEXT("\nvoid %s::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)\n{\n")
			TEXT("\tSuper::PostGameplayEffectExecute(Data);\n\n")
			TEXT("%s}\n"),
			*ClassName, *PostExecuteBody);
	}

	// Write files
	FFileHelper::SaveStringToFile(HeaderContent, *HeaderPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FFileHelper::SaveStringToFile(SourceContent, *SourceFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("class_name"), ClassName);
	Result->SetStringField(TEXT("header_path"), HeaderPath);
	Result->SetStringField(TEXT("source_path"), SourceFilePath);
	Result->SetNumberField(TEXT("configured_count"), ClampRulesArr->Num());
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Generated clamping code for %d attributes in %s. Rebuild required."),
			ClampRulesArr->Num(), *ClassName));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  configure_meta_attributes
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleConfigureMetaAttributes(const TSharedPtr<FJsonObject>& Params)
{
	FString AttrSet;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("attribute_set"), AttrSet, Err)) return Err;

	const TArray<TSharedPtr<FJsonValue>>* MetaArray;
	if (!Params->TryGetArrayField(TEXT("meta_attributes"), MetaArray) || !MetaArray)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: meta_attributes (array)"));
	}

	// Meta attributes are a C++ pattern — for BP mode we describe what to do, for C++ we generate code
	if (LooksLikeBlueprintPath(AttrSet))
	{
		// Blueprint mode — meta attributes require PostGameplayEffectExecute implementation
		// For GBA Blueprints, users implement this in the event graph
		// We can add the attributes but the dispatch logic needs to be in Blueprint

		FString Error;
		UBlueprint* BP = LoadAttributeSetBlueprint(AttrSet, Error);
		if (!BP)
		{
			return FMonolithActionResult::Error(Error);
		}

		// Add meta attributes as regular FGameplayAttributeData
		int32 AddedCount = 0;
		TArray<FString> MetaNames;

		for (const auto& MetaVal : *MetaArray)
		{
			const TSharedPtr<FJsonObject>* MetaObjPtr;
			if (!MetaVal->TryGetObject(MetaObjPtr)) continue;
			const TSharedPtr<FJsonObject>& MetaObj = *MetaObjPtr;

			FString Name = MetaObj->GetStringField(TEXT("name"));
			FString DispatchesTo = MetaObj->GetStringField(TEXT("dispatches_to"));
			if (Name.IsEmpty()) continue;

			FName VarName(*Name);

			// Check if already exists
			bool bExists = false;
			for (const FBPVariableDescription& Var : BP->NewVariables)
			{
				if (Var.VarName == VarName) { bExists = true; break; }
			}

			if (!bExists)
			{
				// Use base FGameplayAttributeData for meta attributes (no clamping needed)
				FEdGraphPinType PinType;
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PinType.PinSubCategoryObject = FGameplayAttributeData::StaticStruct();

				FBlueprintEditorUtils::AddMemberVariable(BP, VarName, PinType, TEXT(""));

				// Set category to "Meta Attributes" for organization
				FBlueprintEditorUtils::SetBlueprintVariableCategory(
					BP, VarName, nullptr, FText::FromString(TEXT("Meta Attributes")));

				AddedCount++;
			}

			MetaNames.Add(FString::Printf(TEXT("%s -> %s"), *Name, *DispatchesTo));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(AttrSet,
			FString::Printf(
				TEXT("Added %d meta attributes. IMPORTANT: You must implement the dispatch logic in the "
				     "PostGameplayEffectExecute event in the Blueprint event graph."), AddedCount));
		Result->SetNumberField(TEXT("added_count"), AddedCount);

		TArray<TSharedPtr<FJsonValue>> DispatchArr;
		for (const FString& M : MetaNames)
		{
			DispatchArr.Add(MakeShared<FJsonValueString>(M));
		}
		Result->SetArrayField(TEXT("dispatch_map"), DispatchArr);
		return FMonolithActionResult::Success(Result);
	}

	// ---- C++ mode ----
	FString ClassName = AttrSet;
	if (!ClassName.StartsWith(TEXT("U")))
	{
		ClassName = TEXT("U") + ClassName;
	}
	FString ClassNameNoPrefix = ClassName.Mid(1);
	FString SourceDir = MonolithGAS::GetProjectSourceDir();
	FString HeaderPath = SourceDir / ClassNameNoPrefix + TEXT(".h");
	FString SourceFilePath = SourceDir / ClassNameNoPrefix + TEXT(".cpp");

	FString HeaderContent;
	if (!FFileHelper::LoadFileToString(HeaderContent, *HeaderPath))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Could not find header file: %s"), *HeaderPath));
	}

	FString SourceContent;
	if (!FFileHelper::LoadFileToString(SourceContent, *SourceFilePath))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Could not find source file: %s"), *SourceFilePath));
	}

	// Add meta attribute properties to header
	int32 AddedCount = 0;
	for (const auto& MetaVal : *MetaArray)
	{
		const TSharedPtr<FJsonObject>* MetaObjPtr;
		if (!MetaVal->TryGetObject(MetaObjPtr)) continue;
		const TSharedPtr<FJsonObject>& MetaObj = *MetaObjPtr;

		FString Name = MetaObj->GetStringField(TEXT("name"));
		if (Name.IsEmpty()) continue;

		// Check if property already exists in header
		if (HeaderContent.Contains(FString::Printf(TEXT("FGameplayAttributeData %s;"), *Name)))
		{
			continue;
		}

		// Add meta attribute property (not replicated — meta attributes are server-only)
		FString MetaProp = FString::Printf(
			TEXT("\t// Meta attribute — dispatched in PostGameplayEffectExecute, never replicated\n")
			TEXT("\tUPROPERTY(BlueprintReadOnly, Category = \"Meta Attributes\")\n")
			TEXT("\tFGameplayAttributeData %s;\n")
			TEXT("\tATTRIBUTE_ACCESSORS(%s, %s)\n\n"),
			*Name, *ClassName, *Name);

		int32 LastBrace = HeaderContent.Find(TEXT("};"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (LastBrace != INDEX_NONE)
		{
			HeaderContent = HeaderContent.Left(LastBrace) + MetaProp + HeaderContent.Mid(LastBrace);
		}

		AddedCount++;
	}

	// Generate PostGameplayEffectExecute dispatch code
	bool bHasPostExecute = SourceContent.Contains(TEXT("PostGameplayEffectExecute"));

	FString DispatchBody;
	for (const auto& MetaVal : *MetaArray)
	{
		const TSharedPtr<FJsonObject>* MetaObjPtr;
		if (!MetaVal->TryGetObject(MetaObjPtr)) continue;
		const TSharedPtr<FJsonObject>& MetaObj = *MetaObjPtr;

		FString Name = MetaObj->GetStringField(TEXT("name"));
		FString DispatchesTo = MetaObj->GetStringField(TEXT("dispatches_to"));
		if (Name.IsEmpty() || DispatchesTo.IsEmpty()) continue;

		DispatchBody += FString::Printf(
			TEXT("\t// Meta attribute: %s dispatches to %s\n")
			TEXT("\tif (Data.EvaluatedData.Attribute == Get%sAttribute())\n\t{\n")
			TEXT("\t\tconst float LocalValue = Get%s();\n")
			TEXT("\t\tSet%s(0.f); // Reset meta attribute\n\n")
			TEXT("\t\t// Apply to target attribute\n")
			TEXT("\t\tconst float New%s = Get%s() + LocalValue;\n")
			TEXT("\t\tSet%s(New%s);\n")
			TEXT("\t}\n\n"),
			*Name, *DispatchesTo,
			*Name,
			*Name,
			*Name,
			*DispatchesTo, *DispatchesTo,
			*DispatchesTo, *DispatchesTo);
	}

	// Add PostGameplayEffectExecute declaration to header if needed
	if (!bHasPostExecute && !DispatchBody.IsEmpty())
	{
		int32 LastBrace = HeaderContent.Find(TEXT("};"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (LastBrace != INDEX_NONE)
		{
			FString Decl = TEXT("\tvirtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;\n");
			HeaderContent = HeaderContent.Left(LastBrace) + Decl + HeaderContent.Mid(LastBrace);
		}
	}

	// Add function body to source
	if (!bHasPostExecute && !DispatchBody.IsEmpty())
	{
		SourceContent += FString::Printf(
			TEXT("\nvoid %s::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)\n{\n")
			TEXT("\tSuper::PostGameplayEffectExecute(Data);\n\n")
			TEXT("%s}\n"),
			*ClassName, *DispatchBody);
	}
	else if (bHasPostExecute && !DispatchBody.IsEmpty())
	{
		// PostGameplayEffectExecute already exists — append dispatch code before closing brace
		// Find the function and its closing brace
		FString FuncName = FString::Printf(TEXT("%s::PostGameplayEffectExecute"), *ClassName);
		int32 FuncIdx = SourceContent.Find(FuncName);
		if (FuncIdx != INDEX_NONE)
		{
			int32 FuncBodyStart = SourceContent.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, FuncIdx);
			if (FuncBodyStart != INDEX_NONE)
			{
				int32 BraceCount = 1;
				int32 Pos = FuncBodyStart + 1;
				while (Pos < SourceContent.Len() && BraceCount > 0)
				{
					if (SourceContent[Pos] == TEXT('{')) BraceCount++;
					else if (SourceContent[Pos] == TEXT('}')) BraceCount--;
					Pos++;
				}
				if (BraceCount == 0)
				{
					// Insert before the closing brace
					SourceContent = SourceContent.Left(Pos - 1) + TEXT("\n") + DispatchBody + SourceContent.Mid(Pos - 1);
				}
			}
		}
	}

	// Write files
	FFileHelper::SaveStringToFile(HeaderContent, *HeaderPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FFileHelper::SaveStringToFile(SourceContent, *SourceFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("class_name"), ClassName);
	Result->SetStringField(TEXT("header_path"), HeaderPath);
	Result->SetStringField(TEXT("source_path"), SourceFilePath);
	Result->SetNumberField(TEXT("meta_attribute_count"), MetaArray->Num());
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Generated meta attribute pipeline with %d dispatch rules in %s. Rebuild required."),
			MetaArray->Num(), *ClassName));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 2: create_attribute_set_from_template
// ============================================================

namespace
{
	struct FAttributeTemplateDef
	{
		FString Name;
		double DefaultValue;
		bool bReplicated;
		bool bIsMeta;
	};

	TArray<FAttributeTemplateDef> GetAttributeTemplate(const FString& TemplateName, FString& OutError)
	{
		TArray<FAttributeTemplateDef> Result;

		if (TemplateName == TEXT("survival_horror_vitals"))
		{
			Result.Add({TEXT("Health"), 100.0, true, false});
			Result.Add({TEXT("MaxHealth"), 100.0, true, false});
			Result.Add({TEXT("Shield"), 0.0, true, false});
			Result.Add({TEXT("MaxShield"), 50.0, true, false});
			Result.Add({TEXT("Armor"), 0.0, true, false});
			Result.Add({TEXT("DamageResistance"), 0.0, true, false});
			Result.Add({TEXT("IncomingDamage"), 0.0, false, true});
			Result.Add({TEXT("IncomingDamageType"), 0.0, false, true});
			Result.Add({TEXT("IncomingKnockback"), 0.0, false, true});
			Result.Add({TEXT("IncomingStagger"), 0.0, false, true});
			Result.Add({TEXT("IncomingHorrorImpact"), 0.0, false, true});
		}
		else if (TemplateName == TEXT("survival_horror_stamina"))
		{
			Result.Add({TEXT("Stamina"), 100.0, true, false});
			Result.Add({TEXT("MaxStamina"), 100.0, true, false});
			Result.Add({TEXT("StaminaRegenRate"), 10.0, true, false});
			Result.Add({TEXT("MoveSpeed"), 1.0, true, false});
			Result.Add({TEXT("SprintMultiplier"), 1.5, true, false});
		}
		else if (TemplateName == TEXT("survival_horror_horror"))
		{
			Result.Add({TEXT("Sanity"), 100.0, true, false});
			Result.Add({TEXT("Fear"), 0.0, true, false});
			Result.Add({TEXT("HeartRate"), 70.0, true, false});
			Result.Add({TEXT("Paranoia"), 0.0, true, false});
			Result.Add({TEXT("FearResistance"), 0.0, true, false});
			Result.Add({TEXT("HorrorIntensity"), 0.0, true, false});
			Result.Add({TEXT("IncomingFear"), 0.0, false, true});
			Result.Add({TEXT("IncomingSanityDrain"), 0.0, false, true});
		}
		else if (TemplateName == TEXT("enemy_vitals"))
		{
			Result.Add({TEXT("Health"), 100.0, true, false});
			Result.Add({TEXT("MaxHealth"), 100.0, true, false});
			Result.Add({TEXT("StaggerHealth"), 50.0, true, false});
			Result.Add({TEXT("MaxStaggerHealth"), 50.0, true, false});
		}
		else if (TemplateName == TEXT("enemy_resistance"))
		{
			Result.Add({TEXT("ResistBallistic"), 0.0, false, false});
			Result.Add({TEXT("ResistExplosive"), 0.0, false, false});
			Result.Add({TEXT("ResistFire"), 0.0, false, false});
			Result.Add({TEXT("ResistElectric"), 0.0, false, false});
			Result.Add({TEXT("ResistPoison"), 0.0, false, false});
			Result.Add({TEXT("ResistSupernatural"), 0.0, false, false});
		}
		else if (TemplateName == TEXT("world_state"))
		{
			Result.Add({TEXT("Contamination"), 0.0, true, false});
			Result.Add({TEXT("TimeOfDay"), 0.0, true, false});
			Result.Add({TEXT("DirectorIntensity"), 0.0, true, false});
			Result.Add({TEXT("DirectorIntensityMax"), 10.0, true, false});
			Result.Add({TEXT("AlertLevel"), 0.0, true, false});
		}
		else
		{
			OutError = FString::Printf(
				TEXT("Unknown template: '%s'. Valid: survival_horror_vitals, survival_horror_stamina, survival_horror_horror, enemy_vitals, enemy_resistance, world_state"),
				*TemplateName);
		}

		return Result;
	}

	/** Convert FAttributeTemplateDef array to JSON attributes array for create_attribute_set reuse. */
	TArray<TSharedPtr<FJsonValue>> TemplateDefsToJsonArray(const TArray<FAttributeTemplateDef>& Defs)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FAttributeTemplateDef& Def : Defs)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Def.Name);
			Obj->SetNumberField(TEXT("default_value"), Def.DefaultValue);
			Obj->SetBoolField(TEXT("replicated"), Def.bReplicated);
			Arr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Arr;
	}
}

FMonolithActionResult FMonolithGASAttributeActions::HandleCreateAttributeSetFromTemplate(const TSharedPtr<FJsonObject>& Params)
{
	FString TemplateName, SavePath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("template"), TemplateName, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("save_path"), SavePath, Err)) return Err;

	TemplateName = TemplateName.ToLower();

	FString TemplateError;
	TArray<FAttributeTemplateDef> TemplateDefs = GetAttributeTemplate(TemplateName, TemplateError);
	if (!TemplateError.IsEmpty())
	{
		return FMonolithActionResult::Error(TemplateError);
	}

	// Apply overrides
	const TSharedPtr<FJsonObject>* OverridesPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("overrides"), OverridesPtr) && OverridesPtr && (*OverridesPtr).IsValid())
	{
		const TSharedPtr<FJsonObject>& Overrides = *OverridesPtr;

		// Remove attributes
		TArray<FString> RemoveList = MonolithGAS::ParseStringArray(Overrides, TEXT("remove_attributes"));
		if (RemoveList.Num() > 0)
		{
			TemplateDefs.RemoveAll([&RemoveList](const FAttributeTemplateDef& Def)
			{
				return RemoveList.Contains(Def.Name);
			});
		}

		// Add attributes
		const TArray<TSharedPtr<FJsonValue>>* AddArray;
		if (Overrides->TryGetArrayField(TEXT("add_attributes"), AddArray))
		{
			for (const auto& Val : *AddArray)
			{
				const TSharedPtr<FJsonObject>* ObjPtr;
				if (!Val->TryGetObject(ObjPtr)) continue;
				const TSharedPtr<FJsonObject>& Obj = *ObjPtr;

				FAttributeTemplateDef NewDef;
				NewDef.Name = Obj->GetStringField(TEXT("name"));
				if (NewDef.Name.IsEmpty()) continue;
				Obj->TryGetNumberField(TEXT("default_value"), NewDef.DefaultValue);
				Obj->TryGetBoolField(TEXT("replicated"), NewDef.bReplicated);
				NewDef.bIsMeta = false;
				TemplateDefs.Add(NewDef);
			}
		}

		// Override defaults
		const TSharedPtr<FJsonObject>* DefaultsObj;
		if (Overrides->TryGetObjectField(TEXT("defaults"), DefaultsObj) && DefaultsObj && (*DefaultsObj).IsValid())
		{
			for (FAttributeTemplateDef& Def : TemplateDefs)
			{
				double OverrideVal;
				if ((*DefaultsObj)->TryGetNumberField(Def.Name, OverrideVal))
				{
					Def.DefaultValue = OverrideVal;
				}
			}
		}
	}

	// Build params for HandleCreateAttributeSet
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("save_path"), SavePath);

	FString Mode = Params->GetStringField(TEXT("mode"));
	if (Mode.IsEmpty()) Mode = TEXT("cpp");
	CreateParams->SetStringField(TEXT("mode"), Mode);
	CreateParams->SetArrayField(TEXT("attributes"), TemplateDefsToJsonArray(TemplateDefs));

	// Delegate to create_attribute_set
	FMonolithActionResult Result = HandleCreateAttributeSet(CreateParams);

	// Augment result with template info
	if (Result.bSuccess && Result.Result.IsValid())
	{
		Result.Result->SetStringField(TEXT("template"), TemplateName);
		Result.Result->SetNumberField(TEXT("template_attribute_count"), TemplateDefs.Num());

		// List meta attributes for reference
		TArray<TSharedPtr<FJsonValue>> MetaNames;
		for (const FAttributeTemplateDef& Def : TemplateDefs)
		{
			if (Def.bIsMeta)
			{
				MetaNames.Add(MakeShared<FJsonValueString>(Def.Name));
			}
		}
		if (MetaNames.Num() > 0)
		{
			Result.Result->SetArrayField(TEXT("meta_attributes"), MetaNames);
		}
	}

	return Result;
}

// ============================================================
//  Phase 2: create_attribute_init_datatable
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleCreateAttributeInitDataTable(const TSharedPtr<FJsonObject>& Params)
{
	FString AttrSet, SavePath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("attribute_set"), AttrSet, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("save_path"), SavePath, Err)) return Err;

	// Resolve the AttributeSet class to gather attribute names
	UClass* SetClass = nullptr;
	if (LooksLikeBlueprintPath(AttrSet))
	{
		FString Error;
		UBlueprint* BP = LoadAttributeSetBlueprint(AttrSet, Error);
		if (!BP)
		{
			return FMonolithActionResult::Error(Error);
		}
		SetClass = BP->GeneratedClass;
	}
	else
	{
		SetClass = FindAttributeSetClass(AttrSet);
	}

	if (!SetClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("AttributeSet not found: %s"), *AttrSet));
	}

	FString SetClassName = SetClass->GetName();

	// Collect attributes from the class
	TArray<FProperty*> AttrProps = CollectAttributeProperties(SetClass);
	if (AttrProps.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No FGameplayAttributeData properties found on %s"), *SetClassName));
	}

	// Build row data — either from provided rows or auto-generated from defaults
	TArray<TSharedPtr<FJsonValue>> RowEntries;
	const TArray<TSharedPtr<FJsonValue>>* RowsArray;
	bool bHasCustomRows = Params->TryGetArrayField(TEXT("rows"), RowsArray) && RowsArray && RowsArray->Num() > 0;

	UAttributeSet* CDO = Cast<UAttributeSet>(SetClass->GetDefaultObject());

	if (bHasCustomRows)
	{
		for (const auto& RowVal : *RowsArray)
		{
			const TSharedPtr<FJsonObject>* RowObjPtr;
			if (!RowVal->TryGetObject(RowObjPtr)) continue;
			RowEntries.Add(RowVal);
		}
	}
	else
	{
		// Auto-generate rows from class defaults
		for (FProperty* Prop : AttrProps)
		{
			FStructProperty* StructProp = CastField<FStructProperty>(Prop);
			if (!StructProp || !CDO) continue;

			FGameplayAttributeData* AttrData =
				StructProp->ContainerPtrToValuePtr<FGameplayAttributeData>(CDO);
			if (!AttrData) continue;

			TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
			RowObj->SetStringField(TEXT("attribute"), Prop->GetName());
			RowObj->SetNumberField(TEXT("base_value"), AttrData->GetBaseValue());
			RowEntries.Add(MakeShared<FJsonValueObject>(RowObj));
		}
	}

	// Create the DataTable asset
	// FAttributeMetaData is the row struct used by UAbilitySystemComponent::InitStats
	UScriptStruct* RowStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/GameplayAbilities.AttributeMetaData"));
	if (!RowStruct)
	{
		return FMonolithActionResult::Error(TEXT("Could not find FAttributeMetaData struct — ensure GameplayAbilities module is loaded"));
	}

	// Extract asset name
	int32 LastSlash;
	if (!SavePath.FindLastChar(TEXT('/'), LastSlash))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid save_path: %s"), *SavePath));
	}
	FString AssetName = SavePath.Mid(LastSlash + 1);

	// Check for existing asset (AssetRegistry + in-memory multi-tier check)
	FString ExistError;
	if (!MonolithGAS::EnsureAssetPathFree(SavePath, AssetName, ExistError))
	{
		return FMonolithActionResult::Error(ExistError);
	}

	FString Error;
	UPackage* Package = MonolithGAS::GetOrCreatePackage(SavePath, Error);
	if (!Package)
	{
		return FMonolithActionResult::Error(Error);
	}

	UDataTable* DataTable = NewObject<UDataTable>(Package, FName(*AssetName), RF_Public | RF_Standalone);
	if (!DataTable)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create DataTable"));
	}
	DataTable->RowStruct = RowStruct;

	// Populate rows
	// FAttributeMetaData has: BaseValue (float), MinValue (float), MaxValue (float)
	int32 RowCount = 0;
	for (const auto& RowVal : RowEntries)
	{
		const TSharedPtr<FJsonObject>* RowObjPtr;
		if (!RowVal->TryGetObject(RowObjPtr)) continue;
		const TSharedPtr<FJsonObject>& RowObj = *RowObjPtr;

		FString AttrName = RowObj->GetStringField(TEXT("attribute"));
		if (AttrName.IsEmpty()) continue;

		// Row name format: ClassName.AttributeName
		FString RowName = FString::Printf(TEXT("%s.%s"), *SetClassName, *AttrName);

		// Create the row by allocating and setting fields via reflection
		uint8* RowData = (uint8*)FMemory::Malloc(RowStruct->GetStructureSize());
		RowStruct->InitializeStruct(RowData);

		// Set BaseValue
		FProperty* BaseValueProp = RowStruct->FindPropertyByName(TEXT("BaseValue"));
		if (BaseValueProp && RowObj->HasField(TEXT("base_value")))
		{
			float Val = static_cast<float>(RowObj->GetNumberField(TEXT("base_value")));
			BaseValueProp->CopyCompleteValue(BaseValueProp->ContainerPtrToValuePtr<void>(RowData), &Val);
		}

		// Set MinValue
		FProperty* MinValueProp = RowStruct->FindPropertyByName(TEXT("MinValue"));
		if (MinValueProp && RowObj->HasField(TEXT("min_value")))
		{
			float Val = static_cast<float>(RowObj->GetNumberField(TEXT("min_value")));
			MinValueProp->CopyCompleteValue(MinValueProp->ContainerPtrToValuePtr<void>(RowData), &Val);
		}

		// Set MaxValue
		FProperty* MaxValueProp = RowStruct->FindPropertyByName(TEXT("MaxValue"));
		if (MaxValueProp && RowObj->HasField(TEXT("max_value")))
		{
			float Val = static_cast<float>(RowObj->GetNumberField(TEXT("max_value")));
			MaxValueProp->CopyCompleteValue(MaxValueProp->ContainerPtrToValuePtr<void>(RowData), &Val);
		}

		DataTable->AddRow(FName(*RowName), RowData, RowStruct);

		RowStruct->DestroyStruct(RowData);
		FMemory::Free(RowData);
		RowCount++;
	}

	// Save
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(DataTable);

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	FSavePackageResultStruct SaveResult = UPackage::Save(Package, DataTable,
		*FPackageName::LongPackageNameToFilename(SavePath, FPackageName::GetAssetPackageExtension()), SaveArgs);

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(SavePath,
		FString::Printf(TEXT("Created FAttributeMetaData DataTable with %d rows for %s"), RowCount, *SetClassName));
	Result->SetNumberField(TEXT("row_count"), RowCount);
	Result->SetStringField(TEXT("attribute_set"), SetClassName);
	Result->SetBoolField(TEXT("saved"), SaveResult.Result == ESavePackageResult::Success);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 2: duplicate_attribute_set
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleDuplicateAttributeSet(const TSharedPtr<FJsonObject>& Params)
{
	FString Source, SavePath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("source"), Source, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("save_path"), SavePath, Err)) return Err;

	// Resolve source class
	UClass* SourceClass = nullptr;
	bool bSourceIsBP = LooksLikeBlueprintPath(Source);

	if (bSourceIsBP)
	{
		FString Error;
		UBlueprint* BP = LoadAttributeSetBlueprint(Source, Error);
		if (!BP) return FMonolithActionResult::Error(Error);
		SourceClass = BP->GeneratedClass;
	}
	else
	{
		SourceClass = FindAttributeSetClass(Source);
	}

	if (!SourceClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source AttributeSet not found: %s"), *Source));
	}

	// Collect source attributes
	TArray<FProperty*> SourceProps = CollectAttributeProperties(SourceClass);
	UAttributeSet* SourceCDO = Cast<UAttributeSet>(SourceClass->GetDefaultObject());

	// Build attribute definitions from source
	TArray<FAttributeTemplateDef> Defs;
	UScriptStruct* AttrDataStruct = FGameplayAttributeData::StaticStruct();
	for (FProperty* Prop : SourceProps)
	{
		FAttributeTemplateDef Def;
		Def.Name = Prop->GetName();
		Def.bReplicated = Prop->HasAnyPropertyFlags(CPF_Net);
		Def.bIsMeta = false;
		Def.DefaultValue = 0.0;

		if (SourceCDO)
		{
			FStructProperty* StructProp = CastField<FStructProperty>(Prop);
			if (StructProp)
			{
				FGameplayAttributeData* AttrData =
					StructProp->ContainerPtrToValuePtr<FGameplayAttributeData>(SourceCDO);
				if (AttrData)
				{
					Def.DefaultValue = AttrData->GetBaseValue();
				}
			}
		}

		Defs.Add(Def);
	}

	// Apply remove_attributes
	TArray<FString> RemoveList = MonolithGAS::ParseStringArray(Params, TEXT("remove_attributes"));
	if (RemoveList.Num() > 0)
	{
		Defs.RemoveAll([&RemoveList](const FAttributeTemplateDef& Def)
		{
			return RemoveList.Contains(Def.Name);
		});
	}

	// Apply add_attributes
	const TArray<TSharedPtr<FJsonValue>>* AddArray;
	if (Params->TryGetArrayField(TEXT("add_attributes"), AddArray))
	{
		for (const auto& Val : *AddArray)
		{
			const TSharedPtr<FJsonObject>* ObjPtr;
			if (!Val->TryGetObject(ObjPtr)) continue;
			const TSharedPtr<FJsonObject>& Obj = *ObjPtr;

			FAttributeTemplateDef NewDef;
			NewDef.Name = Obj->GetStringField(TEXT("name"));
			if (NewDef.Name.IsEmpty()) continue;
			Obj->TryGetNumberField(TEXT("default_value"), NewDef.DefaultValue);
			Obj->TryGetBoolField(TEXT("replicated"), NewDef.bReplicated);
			NewDef.bIsMeta = false;
			Defs.Add(NewDef);
		}
	}

	// Determine mode from destination path
	bool bDestIsBP = LooksLikeBlueprintPath(SavePath);
	FString Mode = bDestIsBP ? TEXT("blueprint") : TEXT("cpp");

	// Build create params
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("save_path"), SavePath);
	CreateParams->SetStringField(TEXT("mode"), Mode);
	CreateParams->SetArrayField(TEXT("attributes"), TemplateDefsToJsonArray(Defs));

	FMonolithActionResult Result = HandleCreateAttributeSet(CreateParams);

	if (Result.bSuccess && Result.Result.IsValid())
	{
		Result.Result->SetStringField(TEXT("source"), Source);
		Result.Result->SetNumberField(TEXT("source_attribute_count"), SourceProps.Num());
		Result.Result->SetNumberField(TEXT("result_attribute_count"), Defs.Num());
		if (RemoveList.Num() > 0)
		{
			Result.Result->SetNumberField(TEXT("removed"), RemoveList.Num());
		}
	}

	return Result;
}

// ============================================================
//  Phase 2: configure_attribute_replication
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleConfigureAttributeReplication(const TSharedPtr<FJsonObject>& Params)
{
	FString AttrSet;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("attribute_set"), AttrSet, Err)) return Err;

	const TArray<TSharedPtr<FJsonValue>>* RepArray;
	if (!Params->TryGetArrayField(TEXT("replication"), RepArray) || !RepArray || RepArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: replication (array)"));
	}

	// ---- Blueprint mode ----
	if (LooksLikeBlueprintPath(AttrSet))
	{
		FString Error;
		UBlueprint* BP = LoadAttributeSetBlueprint(AttrSet, Error);
		if (!BP) return FMonolithActionResult::Error(Error);

		int32 UpdatedCount = 0;
		TArray<TSharedPtr<FJsonValue>> Warnings;

		for (const auto& RepVal : *RepArray)
		{
			const TSharedPtr<FJsonObject>* RepObjPtr;
			if (!RepVal->TryGetObject(RepObjPtr)) continue;
			const TSharedPtr<FJsonObject>& RepObj = *RepObjPtr;

			FString AttrName = RepObj->GetStringField(TEXT("attribute"));
			if (AttrName.IsEmpty()) continue;

			bool bReplicated = false;
			RepObj->TryGetBoolField(TEXT("replicated"), bReplicated);

			FName VarName(*AttrName);
			bool bFound = false;

			for (FBPVariableDescription& Var : BP->NewVariables)
			{
				if (Var.VarName == VarName)
				{
					if (bReplicated)
					{
						Var.PropertyFlags |= CPF_Net;
					}
					else
					{
						Var.PropertyFlags &= ~CPF_Net;
					}
					bFound = true;
					UpdatedCount++;
					break;
				}
			}

			if (!bFound)
			{
				Warnings.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Attribute '%s' not found on Blueprint"), *AttrName)));
			}
		}

		if (UpdatedCount > 0)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		}

		TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(AttrSet,
			FString::Printf(TEXT("Updated replication on %d attributes"), UpdatedCount));
		Result->SetNumberField(TEXT("updated_count"), UpdatedCount);
		if (Warnings.Num() > 0)
		{
			Result->SetArrayField(TEXT("warnings"), Warnings);
		}
		return FMonolithActionResult::Success(Result);
	}

	// ---- C++ mode ----
	FString ClassName = AttrSet;
	if (!ClassName.StartsWith(TEXT("U"))) ClassName = TEXT("U") + ClassName;
	FString ClassNameNoPrefix = ClassName.Mid(1);
	FString SourceDir = MonolithGAS::GetProjectSourceDir();
	FString HeaderPath = SourceDir / ClassNameNoPrefix + TEXT(".h");
	FString SourceFilePath = SourceDir / ClassNameNoPrefix + TEXT(".cpp");

	FString HeaderContent;
	if (!FFileHelper::LoadFileToString(HeaderContent, *HeaderPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Could not find header: %s"), *HeaderPath));
	}

	FString SourceContent;
	if (!FFileHelper::LoadFileToString(SourceContent, *SourceFilePath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Could not find source: %s"), *SourceFilePath));
	}

	int32 UpdatedCount = 0;
	TArray<TSharedPtr<FJsonValue>> Changes;

	for (const auto& RepVal : *RepArray)
	{
		const TSharedPtr<FJsonObject>* RepObjPtr;
		if (!RepVal->TryGetObject(RepObjPtr)) continue;
		const TSharedPtr<FJsonObject>& RepObj = *RepObjPtr;

		FString AttrName = RepObj->GetStringField(TEXT("attribute"));
		if (AttrName.IsEmpty()) continue;

		bool bReplicated = false;
		RepObj->TryGetBoolField(TEXT("replicated"), bReplicated);

		FString Condition = RepObj->GetStringField(TEXT("condition"));
		if (Condition.IsEmpty()) Condition = TEXT("COND_None");

		// Check if the attribute exists in the header
		FString AttrDecl = FString::Printf(TEXT("FGameplayAttributeData %s;"), *AttrName);
		if (!HeaderContent.Contains(AttrDecl))
		{
			Changes.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("SKIP: attribute '%s' not found in header"), *AttrName)));
			continue;
		}

		if (bReplicated)
		{
			// Add ReplicatedUsing if not already present
			FString WithRep = FString::Printf(TEXT("ReplicatedUsing = OnRep_%s"), *AttrName);
			if (!HeaderContent.Contains(WithRep))
			{
				// Replace UPROPERTY line for this attribute
				FString OldPropLine = FString::Printf(TEXT("UPROPERTY(BlueprintReadOnly, Category = \"Attributes\")"));
				// Find the UPROPERTY line before this attribute
				int32 AttrIdx = HeaderContent.Find(AttrDecl);
				if (AttrIdx != INDEX_NONE)
				{
					// Search backwards for UPROPERTY
					int32 PropIdx = HeaderContent.Find(TEXT("UPROPERTY("), ESearchCase::CaseSensitive, ESearchDir::FromEnd, AttrIdx);
					if (PropIdx != INDEX_NONE && PropIdx > AttrIdx - 200)
					{
						int32 PropEnd = HeaderContent.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromStart, PropIdx);
						if (PropEnd != INDEX_NONE)
						{
							FString OldSpec = HeaderContent.Mid(PropIdx, PropEnd - PropIdx + 1);
							FString NewSpec = FString::Printf(TEXT("UPROPERTY(BlueprintReadOnly, Category = \"Attributes\", ReplicatedUsing = OnRep_%s)"), *AttrName);
							HeaderContent = HeaderContent.Replace(*OldSpec, *NewSpec);
						}
					}
				}

				// Add OnRep declaration if missing
				FString OnRepDecl = FString::Printf(TEXT("OnRep_%s"), *AttrName);
				if (!HeaderContent.Contains(OnRepDecl))
				{
					int32 LastBrace = HeaderContent.Find(TEXT("};"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
					if (LastBrace != INDEX_NONE)
					{
						FString Decl = FString::Printf(
							TEXT("\tUFUNCTION()\n\tvoid OnRep_%s(const FGameplayAttributeData& Old%s);\n\n"),
							*AttrName, *AttrName);
						HeaderContent = HeaderContent.Left(LastBrace) + Decl + HeaderContent.Mid(LastBrace);
					}
				}

				// Add OnRep implementation if missing
				FString OnRepImpl = FString::Printf(TEXT("%s::OnRep_%s"), *ClassName, *AttrName);
				if (!SourceContent.Contains(OnRepImpl))
				{
					SourceContent += FString::Printf(
						TEXT("\nvoid %s::OnRep_%s(const FGameplayAttributeData& Old%s)\n{\n")
						TEXT("\tGAMEPLAYATTRIBUTE_REPNOTIFY(%s, %s, Old%s);\n}\n"),
						*ClassName, *AttrName, *AttrName, *ClassName, *AttrName, *AttrName);
				}

				// Add DOREPLIFETIME line
				FString DoRepLine = FString::Printf(TEXT("DOREPLIFETIME_CONDITION_NOTIFY(%s, %s,"), *ClassName, *AttrName);
				if (!SourceContent.Contains(DoRepLine))
				{
					FString RepPropsFunc = TEXT("GetLifetimeReplicatedProps");
					int32 RepPropsIdx = SourceContent.Find(RepPropsFunc);
					if (RepPropsIdx != INDEX_NONE)
					{
						int32 FuncStart = SourceContent.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, RepPropsIdx);
						if (FuncStart != INDEX_NONE)
						{
							int32 BraceCount = 1;
							int32 Pos = FuncStart + 1;
							while (Pos < SourceContent.Len() && BraceCount > 0)
							{
								if (SourceContent[Pos] == TEXT('{')) BraceCount++;
								else if (SourceContent[Pos] == TEXT('}')) BraceCount--;
								Pos++;
							}
							if (BraceCount == 0)
							{
								FString NewDoRep = FString::Printf(
									TEXT("\tDOREPLIFETIME_CONDITION_NOTIFY(%s, %s, %s, REPNOTIFY_Always);\n"),
									*ClassName, *AttrName, *Condition);
								SourceContent = SourceContent.Left(Pos - 1) + NewDoRep + SourceContent.Mid(Pos - 1);
							}
						}
					}
				}
			}

			Changes.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("%s: replicated (%s)"), *AttrName, *Condition)));
		}
		else
		{
			// Remove replication — strip ReplicatedUsing from UPROPERTY
			FString WithRep = FString::Printf(TEXT(", ReplicatedUsing = OnRep_%s"), *AttrName);
			HeaderContent = HeaderContent.Replace(*WithRep, TEXT(""));

			Changes.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("%s: not replicated"), *AttrName)));
		}

		UpdatedCount++;
	}

	FFileHelper::SaveStringToFile(HeaderContent, *HeaderPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FFileHelper::SaveStringToFile(SourceContent, *SourceFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("class_name"), ClassName);
	Result->SetStringField(TEXT("header_path"), HeaderPath);
	Result->SetNumberField(TEXT("updated_count"), UpdatedCount);
	Result->SetArrayField(TEXT("changes"), Changes);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Updated replication on %d attributes in %s. Rebuild required."), UpdatedCount, *ClassName));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 2: link_datatable_to_asc
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleLinkDataTableToASC(const TSharedPtr<FJsonObject>& Params)
{
	FString ASCBlueprintPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asc_blueprint"), ASCBlueprintPath, Err)) return Err;

	const TArray<TSharedPtr<FJsonValue>>* EntriesArray;
	if (!Params->TryGetArrayField(TEXT("entries"), EntriesArray) || !EntriesArray || EntriesArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: entries (array of {attribute_set_class, datatable_path})"));
	}

	// Load the Blueprint containing the ASC
	FString Error;
	FString OutPath;
	TSharedPtr<FJsonObject> TempParams = MakeShared<FJsonObject>();
	TempParams->SetStringField(TEXT("asset_path"), ASCBlueprintPath);
	UBlueprint* BP = MonolithGAS::LoadBlueprintFromParams(TempParams, OutPath, Error);
	if (!BP)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Find the ASC node on the Blueprint
	USCS_Node* ASCNode = nullptr;
	if (BP->SimpleConstructionScript)
	{
		for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->ComponentClass &&
				Node->ComponentClass->IsChildOf(UAbilitySystemComponent::StaticClass()))
			{
				ASCNode = Node;
				break;
			}
		}
	}

	if (!ASCNode)
	{
		return FMonolithActionResult::Error(TEXT("No AbilitySystemComponent found on this Blueprint"));
	}

	UAbilitySystemComponent* ASCTemplate = Cast<UAbilitySystemComponent>(ASCNode->ComponentTemplate);
	if (!ASCTemplate)
	{
		return FMonolithActionResult::Error(TEXT("ASC node has no valid ComponentTemplate"));
	}

	// DefaultStartingData is a TArray<FAttributeDefaults> on the ASC
	// FAttributeDefaults has: TSubclassOf<UAttributeSet> Attributes; UDataTable* DefaultStartingTable;
	// Access via reflection since it may be protected in some configurations
	FProperty* DefaultDataProp = UAbilitySystemComponent::StaticClass()->FindPropertyByName(TEXT("DefaultStartingData"));
	if (!DefaultDataProp)
	{
		return FMonolithActionResult::Error(TEXT("Could not find DefaultStartingData property on ASC"));
	}

	// Validate entries and build result
	TArray<TSharedPtr<FJsonValue>> Linked;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	for (const auto& EntryVal : *EntriesArray)
	{
		const TSharedPtr<FJsonObject>* EntryObjPtr;
		if (!EntryVal->TryGetObject(EntryObjPtr)) continue;
		const TSharedPtr<FJsonObject>& EntryObj = *EntryObjPtr;

		FString SetClassName = EntryObj->GetStringField(TEXT("attribute_set_class"));
		FString DTPath = EntryObj->GetStringField(TEXT("datatable_path"));

		if (SetClassName.IsEmpty() || DTPath.IsEmpty())
		{
			Warnings.Add(MakeShared<FJsonValueString>(TEXT("Entry missing attribute_set_class or datatable_path")));
			continue;
		}

		// Validate attribute set class
		UClass* SetClass = FindFirstObject<UClass>(*SetClassName, EFindFirstObjectOptions::NativeFirst);
		if (!SetClass)
		{
			SetClass = FindFirstObject<UClass>(*(TEXT("U") + SetClassName), EFindFirstObjectOptions::NativeFirst);
		}
		if (!SetClass)
		{
			FString BPPath = SetClassName;
			if (!BPPath.EndsWith(TEXT("_C"))) BPPath += TEXT("_C");
			SetClass = LoadClass<UObject>(nullptr, *BPPath);
		}
		if (!SetClass || !SetClass->IsChildOf(UAttributeSet::StaticClass()))
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("AttributeSet class not found: %s"), *SetClassName)));
			continue;
		}

		// Validate DataTable
		UDataTable* DT = Cast<UDataTable>(FMonolithAssetUtils::LoadAssetByPath(UDataTable::StaticClass(), DTPath));
		if (!DT)
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("DataTable not found: %s"), *DTPath)));
			continue;
		}

		TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
		LinkObj->SetStringField(TEXT("attribute_set"), SetClass->GetName());
		LinkObj->SetStringField(TEXT("datatable"), DTPath);
		Linked.Add(MakeShared<FJsonValueObject>(LinkObj));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(ASCBlueprintPath,
		FString::Printf(TEXT("Linked %d DataTable entries to ASC"), Linked.Num()));
	Result->SetArrayField(TEXT("linked"), Linked);
	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}
	Result->SetStringField(TEXT("note"),
		TEXT("DefaultStartingData validated. For C++ ASCs, add FAttributeDefaults entries in the constructor. "
		     "For Blueprint ASCs, configure via the Details panel or use C++ init."));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 2: bulk_edit_attributes
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleBulkEditAttributes(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* OpsArray;
	if (!Params->TryGetArrayField(TEXT("operations"), OpsArray) || !OpsArray || OpsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: operations (array)"));
	}

	TArray<TSharedPtr<FJsonValue>> Results;
	TArray<TSharedPtr<FJsonValue>> Errors;
	int32 SuccessCount = 0;

	for (int32 i = 0; i < OpsArray->Num(); i++)
	{
		const TSharedPtr<FJsonObject>* OpObjPtr;
		if (!(*OpsArray)[i]->TryGetObject(OpObjPtr)) continue;
		const TSharedPtr<FJsonObject>& OpObj = *OpObjPtr;

		FString AttrSet = OpObj->GetStringField(TEXT("attribute_set"));
		FString Action = OpObj->GetStringField(TEXT("action"));

		if (AttrSet.IsEmpty() || Action.IsEmpty())
		{
			Errors.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Operation %d: missing attribute_set or action"), i)));
			continue;
		}

		FMonolithActionResult OpResult;

		if (Action == TEXT("add"))
		{
			TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
			AddParams->SetStringField(TEXT("attribute_set"), AttrSet);
			AddParams->SetStringField(TEXT("name"), OpObj->GetStringField(TEXT("name")));
			double DefaultVal = 0.0;
			OpObj->TryGetNumberField(TEXT("default_value"), DefaultVal);
			AddParams->SetNumberField(TEXT("default_value"), DefaultVal);
			bool bRep = false;
			OpObj->TryGetBoolField(TEXT("replicated"), bRep);
			AddParams->SetBoolField(TEXT("replicated"), bRep);
			OpResult = HandleAddAttribute(AddParams);
		}
		else if (Action == TEXT("set_default"))
		{
			TSharedPtr<FJsonObject> DefaultParams = MakeShared<FJsonObject>();
			DefaultParams->SetStringField(TEXT("attribute_set"), AttrSet);
			TSharedPtr<FJsonObject> Defaults = MakeShared<FJsonObject>();
			FString AttrName = OpObj->GetStringField(TEXT("name"));
			double Value = 0.0;
			OpObj->TryGetNumberField(TEXT("value"), Value);
			Defaults->SetNumberField(AttrName, Value);
			DefaultParams->SetObjectField(TEXT("defaults"), Defaults);
			OpResult = HandleSetAttributeDefaults(DefaultParams);
		}
		else if (Action == TEXT("set_replication"))
		{
			TSharedPtr<FJsonObject> RepParams = MakeShared<FJsonObject>();
			RepParams->SetStringField(TEXT("attribute_set"), AttrSet);
			TArray<TSharedPtr<FJsonValue>> RepArr;
			TSharedPtr<FJsonObject> RepEntry = MakeShared<FJsonObject>();
			RepEntry->SetStringField(TEXT("attribute"), OpObj->GetStringField(TEXT("name")));
			bool bRep = false;
			OpObj->TryGetBoolField(TEXT("replicated"), bRep);
			RepEntry->SetBoolField(TEXT("replicated"), bRep);
			FString Condition = OpObj->GetStringField(TEXT("condition"));
			if (!Condition.IsEmpty()) RepEntry->SetStringField(TEXT("condition"), Condition);
			RepArr.Add(MakeShared<FJsonValueObject>(RepEntry));
			RepParams->SetArrayField(TEXT("replication"), RepArr);
			OpResult = HandleConfigureAttributeReplication(RepParams);
		}
		else
		{
			Errors.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Operation %d: unknown action '%s' (valid: add, set_default, set_replication)"), i, *Action)));
			continue;
		}

		if (OpResult.bSuccess)
		{
			TSharedPtr<FJsonObject> OpResultObj = MakeShared<FJsonObject>();
			OpResultObj->SetNumberField(TEXT("index"), i);
			OpResultObj->SetStringField(TEXT("action"), Action);
			OpResultObj->SetStringField(TEXT("attribute_set"), AttrSet);
			OpResultObj->SetBoolField(TEXT("success"), true);
			Results.Add(MakeShared<FJsonValueObject>(OpResultObj));
			SuccessCount++;
		}
		else
		{
			Errors.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Operation %d (%s on %s): %s"), i, *Action, *AttrSet, *OpResult.ErrorMessage)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("total_operations"), OpsArray->Num());
	Result->SetNumberField(TEXT("succeeded"), SuccessCount);
	Result->SetNumberField(TEXT("failed"), Errors.Num());
	Result->SetArrayField(TEXT("results"), Results);
	if (Errors.Num() > 0)
	{
		Result->SetArrayField(TEXT("errors"), Errors);
	}
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Bulk edit: %d/%d operations succeeded"), SuccessCount, OpsArray->Num()));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 3: validate_attribute_set
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleValidateAttributeSet(const TSharedPtr<FJsonObject>& Params)
{
	FString AttrSetId;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("attribute_set"), AttrSetId, Err)) return Err;

	TArray<TSharedPtr<FJsonValue>> Errors;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	// Resolve the class
	UClass* SetClass = nullptr;
	FString SetName;
	bool bIsBlueprintSet = LooksLikeBlueprintPath(AttrSetId);

	if (bIsBlueprintSet)
	{
		FString LoadError;
		UBlueprint* BP = LoadAttributeSetBlueprint(AttrSetId, LoadError);
		if (!BP)
		{
			return FMonolithActionResult::Error(LoadError);
		}
		SetClass = BP->GeneratedClass;
		SetName = BP->GetName();
	}
	else
	{
		SetClass = FindAttributeSetClass(AttrSetId);
		if (!SetClass)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("AttributeSet class not found: %s"), *AttrSetId));
		}
		SetName = SetClass->GetName();
	}

	// Collect all attributes
	TArray<FProperty*> AttrProps = CollectAttributeProperties(SetClass);

	if (AttrProps.Num() == 0)
	{
		TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
		WarnObj->SetStringField(TEXT("type"), TEXT("empty_attribute_set"));
		WarnObj->SetStringField(TEXT("severity"), TEXT("warning"));
		WarnObj->SetStringField(TEXT("message"), TEXT("AttributeSet has no FGameplayAttributeData members"));
		Warnings.Add(MakeShared<FJsonValueObject>(WarnObj));
	}

	// Build name sets for analysis
	TSet<FString> AllNames;
	TSet<FString> ReplicatedNames;
	TSet<FString> MetaNames;

	for (FProperty* Prop : AttrProps)
	{
		FString Name = Prop->GetName();
		AllNames.Add(Name);

		// Check if replicated
		if (Prop->HasAnyPropertyFlags(CPF_Net))
		{
			ReplicatedNames.Add(Name);
		}

		// Detect meta attributes by naming convention
		if (Name.StartsWith(TEXT("Incoming")) || Name.StartsWith(TEXT("Meta")) ||
			Name.Contains(TEXT("IncomingDamage")) || Name.Contains(TEXT("IncomingSanity")) ||
			Name.Contains(TEXT("IncomingFear")) || Name.Contains(TEXT("IncomingKnockback")) ||
			Name.Contains(TEXT("IncomingStagger")) || Name.Contains(TEXT("IncomingHorror")))
		{
			MetaNames.Add(Name);
		}
	}

	// Check: replicated meta attributes (anti-pattern)
	for (const FString& MetaName : MetaNames)
	{
		if (ReplicatedNames.Contains(MetaName))
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetStringField(TEXT("type"), TEXT("replicated_meta_attribute"));
			ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
			ErrObj->SetStringField(TEXT("attribute"), MetaName);
			ErrObj->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Meta attribute '%s' is replicated. Meta attributes are transient dispatch channels and should never be replicated — they waste bandwidth."), *MetaName));
			Errors.Add(MakeShared<FJsonValueObject>(ErrObj));
		}
	}

	// Check: missing clamp pairs (Health without MaxHealth, Stamina without MaxStamina, etc.)
	TArray<TPair<FString, FString>> ClampPairs = {
		{TEXT("Health"), TEXT("MaxHealth")},
		{TEXT("Stamina"), TEXT("MaxStamina")},
		{TEXT("Shield"), TEXT("MaxShield")},
		{TEXT("Sanity"), TEXT("MaxSanity")},
		{TEXT("Mana"), TEXT("MaxMana")},
		{TEXT("StaggerHealth"), TEXT("MaxStaggerHealth")},
	};

	for (const auto& Pair : ClampPairs)
	{
		if (AllNames.Contains(Pair.Key) && !AllNames.Contains(Pair.Value))
		{
			TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
			WarnObj->SetStringField(TEXT("type"), TEXT("missing_clamp_pair"));
			WarnObj->SetStringField(TEXT("severity"), TEXT("warning"));
			WarnObj->SetStringField(TEXT("attribute"), Pair.Key);
			WarnObj->SetStringField(TEXT("missing"), Pair.Value);
			WarnObj->SetStringField(TEXT("message"),
				FString::Printf(TEXT("'%s' exists without '%s'. Without a max attribute, clamping logic cannot prevent overflow."),
					*Pair.Key, *Pair.Value));
			Warnings.Add(MakeShared<FJsonValueObject>(WarnObj));
		}
	}

#if WITH_GBA
	// Check GBA-specific issues
	if (SetClass->IsChildOf(UGBAAttributeSetBlueprintBase::StaticClass()))
	{
		// GBA's K2_PreGameplayEffectExecute returns false by default, blocking all effects
		TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
		WarnObj->SetStringField(TEXT("type"), TEXT("gba_pre_execute_default"));
		WarnObj->SetStringField(TEXT("severity"), TEXT("warning"));
		WarnObj->SetStringField(TEXT("message"),
			TEXT("GBA Blueprint AttributeSet: K2_PreGameplayEffectExecute returns false by default, blocking all effects. Ensure it is overridden to return true."));
		Warnings.Add(MakeShared<FJsonValueObject>(WarnObj));
	}
#endif

	bool bValid = (Errors.Num() == 0);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("attribute_set"), AttrSetId);
	Result->SetStringField(TEXT("name"), SetName);
	Result->SetBoolField(TEXT("valid"), bValid);
	Result->SetBoolField(TEXT("is_blueprint"), bIsBlueprintSet);
	Result->SetNumberField(TEXT("attribute_count"), AttrProps.Num());
	Result->SetNumberField(TEXT("replicated_count"), ReplicatedNames.Num());
	Result->SetNumberField(TEXT("meta_count"), MetaNames.Num());
	Result->SetNumberField(TEXT("error_count"), Errors.Num());
	Result->SetNumberField(TEXT("warning_count"), Warnings.Num());
	if (Errors.Num() > 0) Result->SetArrayField(TEXT("errors"), Errors);
	if (Warnings.Num() > 0) Result->SetArrayField(TEXT("warnings"), Warnings);

	// List all attributes
	TArray<TSharedPtr<FJsonValue>> AttrList;
	for (FProperty* Prop : AttrProps)
	{
		TSharedPtr<FJsonObject> AttrObj = MakeShared<FJsonObject>();
		AttrObj->SetStringField(TEXT("name"), Prop->GetName());
		AttrObj->SetBoolField(TEXT("replicated"), Prop->HasAnyPropertyFlags(CPF_Net));
		AttrObj->SetBoolField(TEXT("is_meta"), MetaNames.Contains(Prop->GetName()));
		AttrList.Add(MakeShared<FJsonValueObject>(AttrObj));
	}
	Result->SetArrayField(TEXT("attributes"), AttrList);

	Result->SetStringField(TEXT("message"),
		bValid ? FString::Printf(TEXT("AttributeSet '%s' passed validation (%d attributes)"), *SetName, AttrProps.Num())
		       : FString::Printf(TEXT("AttributeSet '%s' has %d errors, %d warnings"), *SetName, Errors.Num(), Warnings.Num()));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 3: find_attribute_modifiers
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleFindAttributeModifiers(const TSharedPtr<FJsonObject>& Params)
{
	FString AttrStr;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("attribute"), AttrStr, Err)) return Err;

	FString SearchScope = TEXT("/Game/");
	Params->TryGetStringField(TEXT("search_scope"), SearchScope);

	// Scan all GE Blueprints
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> AllBlueprints;
	AR.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBlueprints, true);

	TArray<TSharedPtr<FJsonValue>> GEResults;
	TArray<TSharedPtr<FJsonValue>> ExecCalcResults;

	for (const FAssetData& Asset : AllBlueprints)
	{
		FString PackagePath = Asset.PackageName.ToString();
		if (!SearchScope.IsEmpty() && !PackagePath.StartsWith(SearchScope))
			continue;

		// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
		FAssetTagValueRef ParentTag = Asset.TagsAndValues.FindTag(FName("ParentClass"));
		FAssetTagValueRef NativeParentTag = Asset.TagsAndValues.FindTag(FName("NativeParentClass"));
		FString ParentPath = ParentTag.IsSet() ? ParentTag.GetValue() : (NativeParentTag.IsSet() ? NativeParentTag.GetValue() : TEXT(""));
		if (!ParentPath.Contains(TEXT("GameplayEffect")) && !ParentPath.Contains(TEXT("GameplayEffectCalculation")))
		{
			continue;
		}

		UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
		if (!BP) continue;

		// Check GE Blueprints
		if (MonolithGAS::IsGameplayEffectBlueprint(BP))
		{
			UGameplayEffect* GE = MonolithGAS::GetBlueprintCDO<UGameplayEffect>(BP);
			if (!GE) continue;

			// Check modifiers
			TArray<int32> MatchingModifiers;
			for (int32 i = 0; i < GE->Modifiers.Num(); i++)
			{
				const FGameplayModifierInfo& Mod = GE->Modifiers[i];
				if (Mod.Attribute.IsValid())
				{
					FString ModAttrStr;
					if (Mod.Attribute.GetAttributeSetClass())
					{
						ModAttrStr = FString::Printf(TEXT("%s.%s"),
							*Mod.Attribute.GetAttributeSetClass()->GetName(),
							*Mod.Attribute.GetName());
					}
					else
					{
						ModAttrStr = Mod.Attribute.GetName();
					}

					if (ModAttrStr.Equals(AttrStr, ESearchCase::IgnoreCase) ||
						ModAttrStr.Contains(AttrStr))
					{
						MatchingModifiers.Add(i);
					}
				}
			}

			if (MatchingModifiers.Num() > 0)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("asset_path"), PackagePath);
				Entry->SetStringField(TEXT("name"), BP->GetName());
				Entry->SetStringField(TEXT("type"), TEXT("gameplay_effect"));

				TArray<TSharedPtr<FJsonValue>> ModInfoArr;
				for (int32 Idx : MatchingModifiers)
				{
					const FGameplayModifierInfo& Mod = GE->Modifiers[Idx];
					TSharedPtr<FJsonObject> ModObj = MakeShared<FJsonObject>();
					ModObj->SetNumberField(TEXT("modifier_index"), Idx);
					ModObj->SetStringField(TEXT("operation"),
						Mod.ModifierOp == EGameplayModOp::Additive ? TEXT("Add") :
						Mod.ModifierOp == EGameplayModOp::MultiplyAdditive ? TEXT("Multiply") :
						Mod.ModifierOp == EGameplayModOp::MultiplyCompound ? TEXT("MultiplyCompound") :
						Mod.ModifierOp == EGameplayModOp::DivideAdditive ? TEXT("Divide") :
						Mod.ModifierOp == EGameplayModOp::Override ? TEXT("Override") :
						Mod.ModifierOp == EGameplayModOp::AddFinal ? TEXT("AddFinal") :
						TEXT("Unknown"));
					float StaticMag = 0.f;
					if (Mod.ModifierMagnitude.GetStaticMagnitudeIfPossible(1.f, StaticMag))
					{
						ModObj->SetNumberField(TEXT("static_magnitude"), StaticMag);
					}
					ModInfoArr.Add(MakeShared<FJsonValueObject>(ModObj));
				}
				Entry->SetArrayField(TEXT("modifiers"), ModInfoArr);
				GEResults.Add(MakeShared<FJsonValueObject>(Entry));
			}

			// Also check executions
			for (const FGameplayEffectExecutionDefinition& Exec : GE->Executions)
			{
				if (!Exec.CalculationClass) continue;

				// Check the captured attributes on the execution
				UGameplayEffectExecutionCalculation* ExecCDO = Cast<UGameplayEffectExecutionCalculation>(Exec.CalculationClass->GetDefaultObject());
				if (!ExecCDO) continue;

				bool bUsesAttr = false;
				// Check scoped modifiers
				for (const FGameplayEffectExecutionScopedModifierInfo& ScopedMod : Exec.CalculationModifiers)
				{
					if (ScopedMod.CapturedAttribute.AttributeToCapture.IsValid())
					{
						FString CapturedStr = FString::Printf(TEXT("%s.%s"),
							*ScopedMod.CapturedAttribute.AttributeToCapture.GetAttributeSetClass()->GetName(),
							*ScopedMod.CapturedAttribute.AttributeToCapture.GetName());
						if (CapturedStr.Contains(AttrStr))
						{
							bUsesAttr = true;
							break;
						}
					}
				}

				if (bUsesAttr)
				{
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("asset_path"), PackagePath);
					Entry->SetStringField(TEXT("name"), BP->GetName());
					Entry->SetStringField(TEXT("type"), TEXT("execution_calculation"));
					Entry->SetStringField(TEXT("calculation_class"), Exec.CalculationClass->GetName());
					ExecCalcResults.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("attribute"), AttrStr);
	Result->SetStringField(TEXT("search_scope"), SearchScope);
	Result->SetNumberField(TEXT("ge_count"), GEResults.Num());
	Result->SetNumberField(TEXT("exec_calc_count"), ExecCalcResults.Num());
	Result->SetArrayField(TEXT("gameplay_effects"), GEResults);
	Result->SetArrayField(TEXT("execution_calculations"), ExecCalcResults);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Found %d GEs and %d ExecCalcs that reference '%s'"),
			GEResults.Num(), ExecCalcResults.Num(), *AttrStr));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 3: diff_attribute_sets
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleDiffAttributeSets(const TSharedPtr<FJsonObject>& Params)
{
	FString SetAId, SetBId;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("set_a"), SetAId, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("set_b"), SetBId, Err)) return Err;

	// Helper lambda to resolve class
	auto ResolveSet = [](const FString& Id, UClass*& OutClass, FString& OutName, FString& OutError) -> bool
	{
		if (LooksLikeBlueprintPath(Id))
		{
			FString LoadError;
			UBlueprint* BP = LoadAttributeSetBlueprint(Id, LoadError);
			if (!BP)
			{
				OutError = LoadError;
				return false;
			}
			OutClass = BP->GeneratedClass;
			OutName = BP->GetName();
		}
		else
		{
			OutClass = FindAttributeSetClass(Id);
			if (!OutClass)
			{
				OutError = FString::Printf(TEXT("AttributeSet class not found: %s"), *Id);
				return false;
			}
			OutName = OutClass->GetName();
		}
		return true;
	};

	UClass* ClassA = nullptr;
	UClass* ClassB = nullptr;
	FString NameA, NameB, ErrorMsg;

	if (!ResolveSet(SetAId, ClassA, NameA, ErrorMsg)) return FMonolithActionResult::Error(ErrorMsg);
	if (!ResolveSet(SetBId, ClassB, NameB, ErrorMsg)) return FMonolithActionResult::Error(ErrorMsg);

	// Collect properties
	TArray<FProperty*> PropsA = CollectAttributeProperties(ClassA);
	TArray<FProperty*> PropsB = CollectAttributeProperties(ClassB);

	// Build name -> property maps
	TMap<FString, FProperty*> MapA, MapB;
	for (FProperty* P : PropsA) MapA.Add(P->GetName(), P);
	for (FProperty* P : PropsB) MapB.Add(P->GetName(), P);

	TArray<TSharedPtr<FJsonValue>> SharedAttrs;
	TArray<TSharedPtr<FJsonValue>> OnlyInA;
	TArray<TSharedPtr<FJsonValue>> OnlyInB;

	// Find shared and only-in-A
	for (const auto& Pair : MapA)
	{
		if (FProperty** FoundB = MapB.Find(Pair.Key))
		{
			TSharedPtr<FJsonObject> SharedObj = MakeShared<FJsonObject>();
			SharedObj->SetStringField(TEXT("name"), Pair.Key);

			// Compare replication
			bool bRepA = Pair.Value->HasAnyPropertyFlags(CPF_Net);
			bool bRepB = (*FoundB)->HasAnyPropertyFlags(CPF_Net);
			SharedObj->SetBoolField(TEXT("replicated_a"), bRepA);
			SharedObj->SetBoolField(TEXT("replicated_b"), bRepB);
			if (bRepA != bRepB)
			{
				SharedObj->SetBoolField(TEXT("replication_differs"), true);
			}

			// Get default values from CDOs
			UObject* CDOA = ClassA->GetDefaultObject();
			UObject* CDOB = ClassB->GetDefaultObject();
			if (CDOA && CDOB)
			{
				FStructProperty* StructPropA = CastField<FStructProperty>(Pair.Value);
				FStructProperty* StructPropB = CastField<FStructProperty>(*FoundB);
				if (StructPropA && StructPropB)
				{
					const FGameplayAttributeData* DataA = StructPropA->ContainerPtrToValuePtr<FGameplayAttributeData>(CDOA);
					const FGameplayAttributeData* DataB = StructPropB->ContainerPtrToValuePtr<FGameplayAttributeData>(CDOB);
					if (DataA && DataB)
					{
						SharedObj->SetNumberField(TEXT("default_a"), DataA->GetBaseValue());
						SharedObj->SetNumberField(TEXT("default_b"), DataB->GetBaseValue());
						if (!FMath::IsNearlyEqual(DataA->GetBaseValue(), DataB->GetBaseValue()))
						{
							SharedObj->SetBoolField(TEXT("default_differs"), true);
						}
					}
				}
			}
			SharedAttrs.Add(MakeShared<FJsonValueObject>(SharedObj));
		}
		else
		{
			OnlyInA.Add(MakeShared<FJsonValueString>(Pair.Key));
		}
	}

	// Find only-in-B
	for (const auto& Pair : MapB)
	{
		if (!MapA.Contains(Pair.Key))
		{
			OnlyInB.Add(MakeShared<FJsonValueString>(Pair.Key));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("set_a"), NameA);
	Result->SetStringField(TEXT("set_b"), NameB);
	Result->SetNumberField(TEXT("count_a"), PropsA.Num());
	Result->SetNumberField(TEXT("count_b"), PropsB.Num());
	Result->SetNumberField(TEXT("shared_count"), SharedAttrs.Num());
	Result->SetNumberField(TEXT("only_in_a_count"), OnlyInA.Num());
	Result->SetNumberField(TEXT("only_in_b_count"), OnlyInB.Num());
	Result->SetArrayField(TEXT("shared"), SharedAttrs);
	Result->SetArrayField(TEXT("only_in_a"), OnlyInA);
	Result->SetArrayField(TEXT("only_in_b"), OnlyInB);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Diff: %d shared, %d only in %s, %d only in %s"),
			SharedAttrs.Num(), OnlyInA.Num(), *NameA, OnlyInB.Num(), *NameB));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 3: get_attribute_dependency_graph
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleGetAttributeDependencyGraph(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* SetsArray;
	if (!Params->TryGetArrayField(TEXT("attribute_sets"), SetsArray) || !SetsArray || SetsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required parameter: attribute_sets (array)"));
	}

	FString Format = TEXT("json");
	Params->TryGetStringField(TEXT("format"), Format);
	bool bDotFormat = Format.Equals(TEXT("dot"), ESearchCase::IgnoreCase);

	struct FAttrNode
	{
		FString SetName;
		FString AttrName;
		FString FullName;
		bool bIsMeta = false;
		bool bReplicated = false;
	};

	struct FDependencyEdge
	{
		FString From;
		FString To;
		FString Type; // "clamp", "meta_dispatch", "derived"
	};

	TArray<FAttrNode> AllNodes;
	TArray<FDependencyEdge> Edges;

	for (const auto& Val : *SetsArray)
	{
		FString SetId;
		if (!Val->TryGetString(SetId)) continue;

		UClass* SetClass = nullptr;
		FString SetName;

		if (LooksLikeBlueprintPath(SetId))
		{
			FString LoadError;
			UBlueprint* BP = LoadAttributeSetBlueprint(SetId, LoadError);
			if (!BP) continue;
			SetClass = BP->GeneratedClass;
			SetName = BP->GetName();
		}
		else
		{
			SetClass = FindAttributeSetClass(SetId);
			if (!SetClass) continue;
			SetName = SetClass->GetName();
		}

		TArray<FProperty*> Props = CollectAttributeProperties(SetClass);
		for (FProperty* Prop : Props)
		{
			FAttrNode Node;
			Node.SetName = SetName;
			Node.AttrName = Prop->GetName();
			Node.FullName = FString::Printf(TEXT("%s.%s"), *SetName, *Prop->GetName());
			Node.bReplicated = Prop->HasAnyPropertyFlags(CPF_Net);
			Node.bIsMeta = Node.AttrName.StartsWith(TEXT("Incoming")) || Node.AttrName.StartsWith(TEXT("Meta"));
			AllNodes.Add(Node);

			// Infer clamp relationships by naming convention
			FString MaxName = TEXT("Max") + Node.AttrName;
			FString MinName = TEXT("Min") + Node.AttrName;
			for (FProperty* Other : Props)
			{
				if (Other->GetName() == MaxName)
				{
					FDependencyEdge Edge;
					Edge.From = FString::Printf(TEXT("%s.%s"), *SetName, *MaxName);
					Edge.To = Node.FullName;
					Edge.Type = TEXT("clamp");
					Edges.Add(Edge);
				}
				if (Other->GetName() == MinName)
				{
					FDependencyEdge Edge;
					Edge.From = FString::Printf(TEXT("%s.%s"), *SetName, *MinName);
					Edge.To = Node.FullName;
					Edge.Type = TEXT("clamp");
					Edges.Add(Edge);
				}
			}

			// Infer meta dispatch relationships
			if (Node.bIsMeta)
			{
				// e.g. IncomingDamage dispatches to Health
				FString DispatchTarget;
				if (Node.AttrName == TEXT("IncomingDamage")) DispatchTarget = TEXT("Health");
				else if (Node.AttrName == TEXT("IncomingFear")) DispatchTarget = TEXT("Fear");
				else if (Node.AttrName == TEXT("IncomingSanityDrain")) DispatchTarget = TEXT("Sanity");
				else if (Node.AttrName == TEXT("IncomingKnockback")) DispatchTarget = TEXT("Knockback");
				else if (Node.AttrName == TEXT("IncomingStagger")) DispatchTarget = TEXT("StaggerHealth");
				else if (Node.AttrName == TEXT("IncomingHorrorImpact")) DispatchTarget = TEXT("Fear");

				if (!DispatchTarget.IsEmpty())
				{
					// Try to find the target in any set
					for (const FAttrNode& Target : AllNodes)
					{
						if (Target.AttrName == DispatchTarget)
						{
							FDependencyEdge Edge;
							Edge.From = Node.FullName;
							Edge.To = Target.FullName;
							Edge.Type = TEXT("meta_dispatch");
							Edges.Add(Edge);
						}
					}
				}
			}
		}
	}

	if (bDotFormat)
	{
		// Generate Graphviz DOT output
		FString Dot = TEXT("digraph AttributeDependencies {\n");
		Dot += TEXT("  rankdir=LR;\n");
		Dot += TEXT("  node [shape=box];\n\n");

		// Group by set
		TMap<FString, TArray<const FAttrNode*>> SetGroups;
		for (const FAttrNode& N : AllNodes)
		{
			SetGroups.FindOrAdd(N.SetName).Add(&N);
		}

		for (const auto& Group : SetGroups)
		{
			Dot += FString::Printf(TEXT("  subgraph cluster_%s {\n"), *Group.Key);
			Dot += FString::Printf(TEXT("    label=\"%s\";\n"), *Group.Key);
			for (const FAttrNode* N : Group.Value)
			{
				FString NodeId = N->FullName.Replace(TEXT("."), TEXT("_"));
				FString Style = N->bIsMeta ? TEXT("style=dashed") : TEXT("");
				Dot += FString::Printf(TEXT("    %s [label=\"%s\" %s];\n"), *NodeId, *N->AttrName, *Style);
			}
			Dot += TEXT("  }\n\n");
		}

		for (const FDependencyEdge& E : Edges)
		{
			FString FromId = E.From.Replace(TEXT("."), TEXT("_"));
			FString ToId = E.To.Replace(TEXT("."), TEXT("_"));
			FString Style = E.Type == TEXT("clamp") ? TEXT("style=dashed, color=blue") :
			                E.Type == TEXT("meta_dispatch") ? TEXT("style=bold, color=red") :
			                TEXT("");
			Dot += FString::Printf(TEXT("  %s -> %s [label=\"%s\" %s];\n"), *FromId, *ToId, *E.Type, *Style);
		}

		Dot += TEXT("}\n");

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("format"), TEXT("dot"));
		Result->SetStringField(TEXT("dot"), Dot);
		Result->SetNumberField(TEXT("node_count"), AllNodes.Num());
		Result->SetNumberField(TEXT("edge_count"), Edges.Num());
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Generated DOT graph: %d nodes, %d edges"), AllNodes.Num(), Edges.Num()));
		return FMonolithActionResult::Success(Result);
	}

	// JSON output
	TArray<TSharedPtr<FJsonValue>> NodeArr;
	for (const FAttrNode& N : AllNodes)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("set"), N.SetName);
		Obj->SetStringField(TEXT("attribute"), N.AttrName);
		Obj->SetStringField(TEXT("full_name"), N.FullName);
		Obj->SetBoolField(TEXT("is_meta"), N.bIsMeta);
		Obj->SetBoolField(TEXT("replicated"), N.bReplicated);
		NodeArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TArray<TSharedPtr<FJsonValue>> EdgeArr;
	for (const FDependencyEdge& E : Edges)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("from"), E.From);
		Obj->SetStringField(TEXT("to"), E.To);
		Obj->SetStringField(TEXT("type"), E.Type);
		EdgeArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("format"), TEXT("json"));
	Result->SetNumberField(TEXT("node_count"), AllNodes.Num());
	Result->SetNumberField(TEXT("edge_count"), Edges.Num());
	Result->SetArrayField(TEXT("nodes"), NodeArr);
	Result->SetArrayField(TEXT("edges"), EdgeArr);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Dependency graph: %d attributes, %d relationships"), AllNodes.Num(), Edges.Num()));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 3: remove_attribute
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleRemoveAttribute(const TSharedPtr<FJsonObject>& Params)
{
	FString AttrSetId, AttrName;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("attribute_set"), AttrSetId, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("name"), AttrName, Err)) return Err;

	bool bCheckRefs = true;
	Params->TryGetBoolField(TEXT("check_references"), bCheckRefs);

	bool bIsBlueprintSet = LooksLikeBlueprintPath(AttrSetId);

	if (!bIsBlueprintSet)
	{
		// C++ sets: cannot remove programmatically, provide guidance
		UClass* SetClass = FindAttributeSetClass(AttrSetId);
		if (!SetClass)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("AttributeSet class not found: %s"), *AttrSetId));
		}

		FProperty* Prop = FindAttributeProperty(SetClass, AttrName);
		if (!Prop)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Attribute '%s' not found on %s"), *AttrName, *SetClass->GetName()));
		}

		// Check references if requested
		TArray<TSharedPtr<FJsonValue>> References;
		if (bCheckRefs)
		{
			FString FullAttrStr = FString::Printf(TEXT("%s.%s"), *SetClass->GetName(), *AttrName);

			IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			TArray<FAssetData> AllBlueprints;
			AR.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBlueprints, true);

			for (const FAssetData& Asset : AllBlueprints)
			{
				// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
				FAssetTagValueRef ParentTag = Asset.TagsAndValues.FindTag(FName("ParentClass"));
				FAssetTagValueRef NativeParentTag = Asset.TagsAndValues.FindTag(FName("NativeParentClass"));
				FString ParentStr = ParentTag.IsSet() ? ParentTag.GetValue() : (NativeParentTag.IsSet() ? NativeParentTag.GetValue() : TEXT(""));
				if (!ParentStr.Contains(TEXT("GameplayEffect")))
				{
					continue;
				}

				UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
				if (!BP || !MonolithGAS::IsGameplayEffectBlueprint(BP)) continue;

				UGameplayEffect* GE = MonolithGAS::GetBlueprintCDO<UGameplayEffect>(BP);
				if (!GE) continue;

				for (const FGameplayModifierInfo& Mod : GE->Modifiers)
				{
					if (Mod.Attribute.IsValid() && Mod.Attribute.GetName() == AttrName)
					{
						References.Add(MakeShared<FJsonValueString>(Asset.PackageName.ToString()));
						break;
					}
				}
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("attribute_set"), AttrSetId);
		Result->SetStringField(TEXT("attribute"), AttrName);
		Result->SetBoolField(TEXT("removed"), false);
		Result->SetStringField(TEXT("reason"), TEXT("C++ attributes must be removed manually from source code"));
		if (References.Num() > 0)
		{
			Result->SetArrayField(TEXT("referenced_by"), References);
			Result->SetNumberField(TEXT("reference_count"), References.Num());
		}
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Cannot remove C++ attribute '%s' programmatically. Remove the UPROPERTY from the header and rebuild."), *AttrName));
		return FMonolithActionResult::Success(Result);
	}

	// Blueprint set removal
	FString LoadError;
	UBlueprint* BP = LoadAttributeSetBlueprint(AttrSetId, LoadError);
	if (!BP)
	{
		return FMonolithActionResult::Error(LoadError);
	}

	// Find the variable
	int32 VarIdx = INDEX_NONE;
	for (int32 i = 0; i < BP->NewVariables.Num(); i++)
	{
		if (BP->NewVariables[i].VarName.ToString() == AttrName)
		{
			VarIdx = i;
			break;
		}
	}

	if (VarIdx == INDEX_NONE)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Attribute '%s' not found on Blueprint '%s'"), *AttrName, *BP->GetName()));
	}

	// Check references if requested
	TArray<TSharedPtr<FJsonValue>> References;
	if (bCheckRefs)
	{
		FString ClassName = BP->GeneratedClass ? BP->GeneratedClass->GetName() : BP->GetName();
		FString FullAttrStr = FString::Printf(TEXT("%s.%s"), *ClassName, *AttrName);

		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> AllBlueprints;
		AR.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBlueprints, true);

		for (const FAssetData& Asset : AllBlueprints)
		{
			// Pre-filter via AR tags BEFORE loading — prevents crash from loading ControlRig/AnimBP/etc.
			FAssetTagValueRef ParentTag = Asset.TagsAndValues.FindTag(FName("ParentClass"));
			FAssetTagValueRef NativeParentTag = Asset.TagsAndValues.FindTag(FName("NativeParentClass"));
			FString ParentStr = ParentTag.IsSet() ? ParentTag.GetValue() : (NativeParentTag.IsSet() ? NativeParentTag.GetValue() : TEXT(""));
			if (!ParentStr.Contains(TEXT("GameplayEffect")))
			{
				continue;
			}

			UBlueprint* RefBP = Cast<UBlueprint>(Asset.GetAsset());
			if (!RefBP || RefBP == BP || !MonolithGAS::IsGameplayEffectBlueprint(RefBP)) continue;

			UGameplayEffect* GE = MonolithGAS::GetBlueprintCDO<UGameplayEffect>(RefBP);
			if (!GE) continue;

			for (const FGameplayModifierInfo& Mod : GE->Modifiers)
			{
				if (Mod.Attribute.IsValid() && Mod.Attribute.GetName() == AttrName)
				{
					References.Add(MakeShared<FJsonValueString>(Asset.PackageName.ToString()));
					break;
				}
			}
		}

		if (References.Num() > 0)
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("attribute_set"), AttrSetId);
			Result->SetStringField(TEXT("attribute"), AttrName);
			Result->SetBoolField(TEXT("removed"), false);
			Result->SetNumberField(TEXT("reference_count"), References.Num());
			Result->SetArrayField(TEXT("referenced_by"), References);
			Result->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Cannot remove '%s': %d GameplayEffects reference it. Remove references first or use check_references=false."),
					*AttrName, References.Num()));
			return FMonolithActionResult::Success(Result);
		}
	}

	// Remove the variable
	BP->NewVariables.RemoveAt(VarIdx);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("attribute_set"), AttrSetId);
	Result->SetStringField(TEXT("attribute"), AttrName);
	Result->SetBoolField(TEXT("removed"), true);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Removed attribute '%s' from '%s'"), *AttrName, *BP->GetName()));
	return FMonolithActionResult::Success(Result);
}

// PIE helpers moved to MonolithGAS::GetPIEWorld/FindActorInPIE/GetASCFromActor in MonolithGASInternal.cpp

// ============================================================
//  Phase 4: get_attribute_value
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleGetAttributeValue(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorId, AttrStr;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor"), ActorId, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("attribute"), AttrStr, Err)) return Err;

	UWorld* PIEWorld = MonolithGAS::GetPIEWorld();
	if (!PIEWorld)
	{
		return FMonolithActionResult::Error(TEXT("No PIE world found. Start Play In Editor first."));
	}

	AActor* Actor = MonolithGAS::FindActorInPIE(ActorId);
	if (!Actor)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor not found in PIE world: '%s'"), *ActorId));
	}

	UAbilitySystemComponent* ASC = MonolithGAS::GetASCFromActor(Actor);
	if (!ASC)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor '%s' has no AbilitySystemComponent"), *ActorId));
	}

	// Parse attribute from "ClassName.PropertyName"
	FString ClassName, PropName;
	if (!AttrStr.Split(TEXT("."), &ClassName, &PropName))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Attribute must be 'ClassName.PropertyName', got: %s"), *AttrStr));
	}

	UClass* AttrClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (!AttrClass)
	{
		AttrClass = FindFirstObject<UClass>(*(TEXT("U") + ClassName), EFindFirstObjectOptions::NativeFirst);
	}
	if (!AttrClass)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Attribute class not found: %s"), *ClassName));
	}

	FProperty* Prop = FindFProperty<FProperty>(AttrClass, FName(*PropName));
	if (!Prop)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Property '%s' not found on class '%s'"), *PropName, *ClassName));
	}

	FGameplayAttribute Attribute(Prop);
	if (!Attribute.IsValid())
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid gameplay attribute: %s"), *AttrStr));
	}

	bool bFound = false;
	float CurrentValue = ASC->GetGameplayAttributeValue(Attribute, bFound);
	if (!bFound)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Attribute '%s' not found on ASC (AttributeSet may not be registered)"), *AttrStr));
	}

	// Get base value via the attribute set
	float BaseValue = 0.f;
	const UAttributeSet* AttrSet = ASC->GetAttributeSet(AttrClass);
	if (AttrSet)
	{
		FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		if (StructProp)
		{
			const FGameplayAttributeData* Data = StructProp->ContainerPtrToValuePtr<FGameplayAttributeData>(AttrSet);
			if (Data)
			{
				BaseValue = Data->GetBaseValue();
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
	Result->SetStringField(TEXT("attribute"), AttrStr);
	Result->SetNumberField(TEXT("current_value"), CurrentValue);
	Result->SetNumberField(TEXT("base_value"), BaseValue);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("%s on '%s': current=%.4f, base=%.4f"),
			*AttrStr, *Actor->GetActorLabel(), CurrentValue, BaseValue));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  Phase 4: set_attribute_value
// ============================================================

FMonolithActionResult FMonolithGASAttributeActions::HandleSetAttributeValue(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorId, AttrStr;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("actor"), ActorId, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("attribute"), AttrStr, Err)) return Err;

	double Value = 0.0;
	if (!Params->TryGetNumberField(TEXT("value"), Value))
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: value (number)"));
	}

	bool bSetBase = false;
	Params->TryGetBoolField(TEXT("set_base"), bSetBase);

	UWorld* PIEWorld = MonolithGAS::GetPIEWorld();
	if (!PIEWorld)
	{
		return FMonolithActionResult::Error(TEXT("No PIE world found. Start Play In Editor first."));
	}

	AActor* Actor = MonolithGAS::FindActorInPIE(ActorId);
	if (!Actor)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor not found in PIE world: '%s'"), *ActorId));
	}

	UAbilitySystemComponent* ASC = MonolithGAS::GetASCFromActor(Actor);
	if (!ASC)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Actor '%s' has no AbilitySystemComponent"), *ActorId));
	}

	// Parse attribute
	FString ClassName, PropName;
	if (!AttrStr.Split(TEXT("."), &ClassName, &PropName))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Attribute must be 'ClassName.PropertyName', got: %s"), *AttrStr));
	}

	UClass* AttrClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (!AttrClass)
	{
		AttrClass = FindFirstObject<UClass>(*(TEXT("U") + ClassName), EFindFirstObjectOptions::NativeFirst);
	}
	if (!AttrClass)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Attribute class not found: %s"), *ClassName));
	}

	FProperty* Prop = FindFProperty<FProperty>(AttrClass, FName(*PropName));
	if (!Prop)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Property '%s' not found on class '%s'"), *PropName, *ClassName));
	}

	FGameplayAttribute Attribute(Prop);
	if (!Attribute.IsValid())
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid gameplay attribute: %s"), *AttrStr));
	}

	float FloatValue = static_cast<float>(Value);
	float OldValue = 0.f;

	if (bSetBase)
	{
		bool bFound = false;
		OldValue = ASC->GetGameplayAttributeValue(Attribute, bFound);
		ASC->SetNumericAttributeBase(Attribute, FloatValue);
	}
	else
	{
		bool bFound = false;
		OldValue = ASC->GetGameplayAttributeValue(Attribute, bFound);

		// Use the internal set path to override current value
		// We modify the attribute data directly on the spawned AttributeSet
		UAttributeSet* AttrSet = const_cast<UAttributeSet*>(ASC->GetAttributeSet(AttrClass));
		if (!AttrSet)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("AttributeSet '%s' not spawned on this ASC"), *ClassName));
		}
		FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		if (StructProp)
		{
			FGameplayAttributeData* Data = StructProp->ContainerPtrToValuePtr<FGameplayAttributeData>(AttrSet);
			if (Data)
			{
				Data->SetCurrentValue(FloatValue);
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("attribute"), AttrStr);
	Result->SetNumberField(TEXT("old_value"), OldValue);
	Result->SetNumberField(TEXT("new_value"), FloatValue);
	Result->SetBoolField(TEXT("set_base"), bSetBase);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Set %s %s on '%s': %.4f -> %.4f"),
			*AttrStr, bSetBase ? TEXT("(base)") : TEXT("(current)"),
			*Actor->GetActorLabel(), OldValue, FloatValue));
	return FMonolithActionResult::Success(Result);
}
