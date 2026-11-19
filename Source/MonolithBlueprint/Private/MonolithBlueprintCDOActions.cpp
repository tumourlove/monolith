#include "MonolithBlueprintCDOActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "MonolithBulkFillRegistry.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "Reflection/MonolithReflectionReader.h"
#include "Reflection/MonolithDryRunGuard.h"
#include "UObject/UnrealType.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "JsonObjectConverter.h"
#include "StructUtils/InstancedStruct.h"
#include "ScopedTransaction.h"
#include "MonolithBlueprintEditCradle.h"
#include "UObject/SavePackage.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"

// Forward declaration of Phase 1 handlers (defined at bottom of file).
namespace MonolithCDOPhase1Internal
{
	FMonolithActionResult HandleSetCDOProperties(const TSharedPtr<FJsonObject>& Params);
	FMonolithActionResult HandleDescribeCDOSchema(const TSharedPtr<FJsonObject>& Params);
}

// --- Registration ---

void FMonolithBlueprintCDOActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_cdo_properties"),
		TEXT("Read all CDO (Class Default Object) properties from a Blueprint or any UObject asset. "
			 "Essential for GameplayEffects (Duration, Modifiers, Tags, Stacking), AbilitySets, InputActions, "
			 "and any asset whose config is stored as UPROPERTY defaults rather than Blueprint graph nodes. "
			 "Narrow the output with property_names (exact) or name_pattern / name_filter (substring); unrecognised params are "
			 "not applied as filters and come back in the response 'warnings' array."),
		FMonolithActionHandler::CreateStatic(&HandleGetCDOProperties),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path (e.g. /Game/Blueprints/BP_MyActor or /Game/Data/DA_MyData)"))
			.Optional(TEXT("category_filter"), TEXT("string"), TEXT("Only include properties whose category contains this string"))
			.Optional(TEXT("include_parent_defaults"), TEXT("boolean"), TEXT("If true, include properties inherited from native parent class (default: true)"))
			.Optional(TEXT("owner_class_filter"), TEXT("string"), TEXT("Only include properties whose owner_class name contains this string (case-insensitive). Lets you skip everything inherited from AActor/APawn/ACharacter when only project-level props matter."))
			.Optional(TEXT("name_pattern"), TEXT("string"), TEXT("Only include properties whose name contains this substring (case-insensitive)"), {TEXT("name_filter")})
			.Optional(TEXT("property_names"), TEXT("array"), TEXT("Only include these properties (case-insensitive exact names). Names that match no property are listed in missing_property_names."))
			.Optional(TEXT("exclude_categories"), TEXT("array"), TEXT("List of category names to skip entirely (case-insensitive exact match — e.g. [\"Replication\", \"Cooking\", \"HLOD\", \"Lighting\"])"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_cdo_property"),
		TEXT("Set a property value on a Blueprint CDO or UObject asset (DataAsset, GameplayEffect, etc.). "
			 "Write counterpart to get_cdo_properties. Supports numeric, boolean, string, enum, struct, "
			 "and any type that supports ImportText."),
		FMonolithActionHandler::CreateStatic(&HandleSetCDOProperty),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint or UObject asset path (e.g. /Game/Data/DA_MyData)"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Property name to set (case-insensitive fallback)"))
			.Required(TEXT("value"), TEXT("any"), TEXT("New value — string, number, bool, or ImportText format for structs (e.g. \"(X=1.0,Y=2.0,Z=3.0)\")"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("If true, validate only — emit the would-be write but do not persist. Phase 1."), TEXT("false"))
			.Optional(TEXT("strict"), TEXT("boolean"), TEXT("If true, promote silent drops / clamps / unknown-fields to hard errors. Phase 1."), TEXT("false"))
			.Build());

	// --- Surgical nested-path writer.
	// set_cdo_property / set_cdo_properties address top-level properties (and, for
	// the plural form, replace whole arrays/maps from a JSON tree). set_property_at_path
	// instead targets ONE leaf deep inside a struct / array index / map key via a
	// dotted+bracket path, leaving sibling entries untouched. This is the action that
	// can write EditDefaultsOnly DataAsset fields the editor refuses on saved
	// instances — the write goes through FProperty reflection + the engine edit
	// cradle, not the editor's PropertyAccessUtil edit-flag gate.
	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_property_at_path"),
		TEXT("Set a SINGLE value at a nested property path on a Blueprint CDO or UObject asset "
			 "(DataAsset, GameplayEffect, etc.) via generic reflection. Path is dotted + bracket: "
			 "'.' descends into struct members, '[N]' indexes an array, '[Key]' keys a map "
			 "(enum name, integer, FName, or string — imported through the map's key grammar). "
			 "Example: 'Standing.Gaits[Jog].Starts.Forward'. The leaf value accepts scalars, "
			 "enum names, ImportText struct literals, hard object refs and TSoftObjectPtr asset "
			 "paths, identical to set_cdo_property. Surgical: sibling array elements / map keys are "
			 "left untouched (unlike set_cdo_properties which rebuilds whole collections). Writes "
			 "EditDefaultsOnly fields the editor blocks on saved instances."),
		FMonolithActionHandler::CreateStatic(&HandleSetPropertyAtPath),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint or UObject asset path (e.g. /Game/Data/DA_MyData)"))
			.Required(TEXT("path"), TEXT("string"), TEXT("Dotted+bracket property path, e.g. 'Standing.Gaits[Jog].Starts.Forward'. '.'=struct member, '[N]'=array index, '[Key]'=map key."))
			.Required(TEXT("value"), TEXT("any"), TEXT("New leaf value — string / number / bool, an enum name, a soft/hard object asset path, or ImportText / JSON for a struct leaf."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("If true, resolve the path and validate the leaf write but do not persist."), TEXT("false"))
			.Optional(TEXT("strict"), TEXT("boolean"), TEXT("If true, promote silent drops / clamps to hard errors."), TEXT("false"))
			.Optional(TEXT("create_missing_keys"), TEXT("boolean"), TEXT("If true, a '[Key]' segment whose map key is absent is added with a default-initialised value before writing the leaf. Default: error on a missing key."), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("If true, save the asset package to disk after the write (UPackage::SavePackage). Default: MarkPackageDirty only."), TEXT("false"))
			.Build());

	// --- Phase 1: bulk_fill plural sibling to set_cdo_property.
	// Routes through FMonolithBulkFillRegistry's "blueprint" adapter. Feature parity
	// with the central `bulk_fill.apply` action — exists in the "blueprint" namespace
	// for call-shape familiarity with existing single-key callers.
	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_cdo_properties"),
		TEXT("Bulk-fill multiple CDO properties from a JSON tree in a single transaction. "
			 "Supports nested structs, arrays, maps, sets, enums, and soft-object refs. "
			 "Supports dry_run + strict. Phase 1 pilot of the bulk_fill framework."),
		FMonolithActionHandler::CreateStatic(&MonolithCDOPhase1Internal::HandleSetCDOProperties),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint or UObject asset path (e.g. /Game/Data/DA_MyData)"))
			.Required(TEXT("properties"), TEXT("object"), TEXT("Nested JSON object — keys are property names, values are scalars / structs / arrays / maps / sets per UPROPERTY layout."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("If true, validate only — emit the would-be writes but do not persist."), TEXT("false"))
			.Optional(TEXT("strict"), TEXT("boolean"), TEXT("If true, promote silent drops / clamps / unknown-fields to hard errors."), TEXT("false"))
			.Build());

	// --- Phase 1: describe — emits the FSchemaDescriptor tree (type names, ImportText
	// grammar examples, enum-value lists, clamp ranges, nested children) for an asset's
	// CDO. Counterpart to get_cdo_properties when the caller needs the schema, not the
	// current values.
	Registry.RegisterAction(TEXT("blueprint"), TEXT("describe_cdo_schema"),
		TEXT("Return the rich FSchemaDescriptor tree for a Blueprint CDO / UObject asset. "
			 "Use to discover legal types, ImportText forms, enum-value lists, clamp ranges, and nested struct/array/map children before calling set_cdo_property or set_cdo_properties."),
		FMonolithActionHandler::CreateStatic(&MonolithCDOPhase1Internal::HandleDescribeCDOSchema),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint or UObject asset path (e.g. /Game/Data/DA_MyData)"))
			.Build());
}

// --- Property serialization helpers ---
//
// The FProperty->JSON serializer was hoisted into the shared, read-only
// FMonolithReflectionReader (MonolithCore) so the CDO read path, the DataAsset
// indexer, and the seed_data_asset live-readback all share ONE implementation.
// This thin forwarder preserves the historical call signature at the call site.

namespace MonolithCDOInternal
{
	TSharedPtr<FJsonValue> PropertyToJsonValue(FProperty* Prop, const void* ValuePtr, const UObject* CDO)
	{
		return FMonolithReflectionReader::PropertyToJsonValue(Prop, ValuePtr, CDO);
	}
}

// --- Handler ---

FMonolithActionResult FMonolithBlueprintCDOActions::HandleGetCDOProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	// Try Blueprint first (has GeneratedClass -> CDO), then fall back to any UObject
	UObject* TargetObject = nullptr;
	UClass* TargetClass = nullptr;

	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (BP && BP->GeneratedClass)
	{
		TargetClass = BP->GeneratedClass;
		TargetObject = TargetClass->GetDefaultObject(false);
	}
	else
	{
		// Not a Blueprint — try as a generic UObject (DataAsset, GameplayEffect, etc.)
		TargetObject = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
		if (TargetObject)
		{
			TargetClass = TargetObject->GetClass();
		}
	}

	if (!TargetObject || !TargetClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found or has no class: %s"), *AssetPath));
	}

	// Find the native parent class
	UClass* NativeParent = TargetClass;
	while (NativeParent && !NativeParent->HasAnyClassFlags(CLASS_Native))
	{
		NativeParent = NativeParent->GetSuperClass();
	}

	FString CategoryFilter;
	if (Params->HasField(TEXT("category_filter")))
	{
		CategoryFilter = Params->GetStringField(TEXT("category_filter"));
	}

	bool bIncludeParentDefaults = true;
	if (Params->HasField(TEXT("include_parent_defaults")))
	{
		bIncludeParentDefaults = Params->GetBoolField(TEXT("include_parent_defaults"));
	}

	FString OwnerClassFilter;
	if (Params->HasField(TEXT("owner_class_filter")))
	{
		OwnerClassFilter = Params->GetStringField(TEXT("owner_class_filter"));
	}

	FString NamePattern;
	if (Params->HasField(TEXT("name_pattern")))
	{
		NamePattern = Params->GetStringField(TEXT("name_pattern"));
	}
	else if (Params->HasField(TEXT("name_filter")))
	{
		// Alias; normally rewritten by the registry, read directly for batch callers.
		NamePattern = Params->GetStringField(TEXT("name_filter"));
	}

	// property_names: case-insensitive exact-name allow-list. Tracks which names matched
	// so a typo is reported instead of silently returning nothing for it.
	TMap<FString, bool> PropertyNameSet;
	// Lowercased key -> the caller's spelling, so missing_property_names echoes what was asked for.
	TMap<FString, FString> PropertyNameOriginal;
	const TArray<TSharedPtr<FJsonValue>>* PropNamesArr = nullptr;
	if (Params->TryGetArrayField(TEXT("property_names"), PropNamesArr) && PropNamesArr)
	{
		for (const TSharedPtr<FJsonValue>& Val : *PropNamesArr)
		{
			FString Name;
			if (Val.IsValid() && Val->TryGetString(Name) && !Name.IsEmpty())
			{
				const FString Key = Name.ToLower();
				PropertyNameSet.Add(Key, false);
				PropertyNameOriginal.FindOrAdd(Key, Name);
			}
		}
	}

	TArray<FString> ExcludeCategories;
	const TArray<TSharedPtr<FJsonValue>>* ExcludeCatArr = nullptr;
	if (Params->TryGetArrayField(TEXT("exclude_categories"), ExcludeCatArr) && ExcludeCatArr)
	{
		for (const TSharedPtr<FJsonValue>& Val : *ExcludeCatArr)
		{
			if (Val.IsValid())
				ExcludeCategories.Add(Val->AsString().ToLower());
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("native_class"), NativeParent ? NativeParent->GetName() : TEXT("Unknown"));
	Root->SetStringField(TEXT("parent_class"), TargetClass->GetSuperClass() ? TargetClass->GetSuperClass()->GetName() : TEXT("None"));

	TArray<TSharedPtr<FJsonValue>> PropsArr;

	for (TFieldIterator<FProperty> It(TargetClass, EFieldIteratorFlags::IncludeSuper, EFieldIteratorFlags::ExcludeDeprecated); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop) continue;

		UClass* OwnerClass = Prop->GetOwnerClass();
		if (OwnerClass == UObject::StaticClass()) continue;

		if (!bIncludeParentDefaults && OwnerClass != TargetClass) continue;

		// owner_class_filter — case-insensitive substring match on owner class name
		if (!OwnerClassFilter.IsEmpty())
		{
			const FString OwnerName = OwnerClass ? OwnerClass->GetName() : FString();
			if (!OwnerName.Contains(OwnerClassFilter, ESearchCase::IgnoreCase))
				continue;
		}

		// name_pattern — case-insensitive substring match on property name
		if (!NamePattern.IsEmpty() && !Prop->GetName().Contains(NamePattern, ESearchCase::IgnoreCase))
			continue;

		if (PropertyNameSet.Num() > 0)
		{
			bool* Matched = PropertyNameSet.Find(Prop->GetName().ToLower());
			if (!Matched)
				continue;
			*Matched = true;
		}

		FString Category = Prop->GetMetaData(TEXT("Category"));

		// exclude_categories — case-insensitive exact match on the property's category
		if (ExcludeCategories.Num() > 0 && ExcludeCategories.Contains(Category.ToLower()))
			continue;

		if (!CategoryFilter.IsEmpty() && !Category.Contains(CategoryFilter)) continue;

		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) continue;

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(TargetObject);

		auto PropObj = MakeShared<FJsonObject>();
		PropObj->SetStringField(TEXT("name"), Prop->GetName());
		PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());
		PropObj->SetStringField(TEXT("category"), Category);
		PropObj->SetStringField(TEXT("owner_class"), OwnerClass->GetName());
		PropObj->SetField(TEXT("value"), MonolithCDOInternal::PropertyToJsonValue(Prop, ValuePtr, TargetObject));

		if (Prop->HasAnyPropertyFlags(CPF_Net))
			PropObj->SetBoolField(TEXT("replicated"), true);
		if (Prop->HasAnyPropertyFlags(CPF_EditConst))
			PropObj->SetBoolField(TEXT("edit_const"), true);

		PropsArr.Add(MakeShared<FJsonValueObject>(PropObj));
	}

	Root->SetArrayField(TEXT("properties"), PropsArr);
	Root->SetNumberField(TEXT("property_count"), PropsArr.Num());

	if (PropertyNameSet.Num() > 0)
	{
		// A name can also be "missing" because another filter (category, owner class,
		// transient) excluded it; the list only says it was not returned.
		TArray<TSharedPtr<FJsonValue>> Missing;
		for (const TPair<FString, bool>& Pair : PropertyNameSet)
		{
			if (!Pair.Value)
			{
				const FString* Original = PropertyNameOriginal.Find(Pair.Key);
				Missing.Add(MakeShared<FJsonValueString>(Original ? *Original : Pair.Key));
			}
		}
		Root->SetArrayField(TEXT("missing_property_names"), Missing);
	}

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// set_cdo_property
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithBlueprintCDOActions::HandleSetCDOProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	FString PropertyName = Params->GetStringField(TEXT("property_name"));
	if (PropertyName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: property_name"));
	}

	if (!Params->HasField(TEXT("value")))
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: value"));
	}

	// --- Load asset: Blueprint CDO or generic UObject (same dual-path as get_cdo_properties) ---
	UObject* TargetObject = nullptr;
	UClass* TargetClass = nullptr;
	UBlueprint* BP = nullptr;

	BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (BP && BP->GeneratedClass)
	{
		TargetClass = BP->GeneratedClass;
		TargetObject = TargetClass->GetDefaultObject(false);
	}
	else
	{
		BP = nullptr; // Not a Blueprint
		TargetObject = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
		if (TargetObject)
		{
			TargetClass = TargetObject->GetClass();
		}
	}

	if (!TargetObject || !TargetClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found or has no class: %s"), *AssetPath));
	}

	// --- Find property (exact match, then case-insensitive fallback) ---
	FProperty* Prop = TargetClass->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		for (TFieldIterator<FProperty> It(TargetClass); It; ++It)
		{
			if (It->GetName().Equals(PropertyName, ESearchCase::IgnoreCase))
			{
				Prop = *It;
				break;
			}
		}
	}
	if (!Prop)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Property '%s' not found on %s"), *PropertyName, *TargetClass->GetName()));
	}

	// Deprecated storage (e.g. UInputMappingContext::Mappings since UE 5.7) is not read
	// by the engine, so a write would look applied and do nothing. Issue #148.
	{
		FString DeprecatedReason;
		if (FMonolithReflectionWalker::IsDeprecatedProperty(Prop, DeprecatedReason))
		{
			return FMonolithActionResult::Error(DeprecatedReason);
		}
	}

	// --- Read old value ---
	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(TargetObject);

	FString OldValue;
	Prop->ExportText_Direct(OldValue, ValuePtr, ValuePtr, TargetObject, PPF_None);

	// --- Phase 1: dry_run branch.
	// If dry_run=true, route through the reflection walker's InspectTree (read-only)
	// and return the FDryRunReport without entering the engine edit cradle. The
	// guard pulls dry_run + strict from Params per FMonolithDryRunGuard contract.
	FMonolithDryRunGuard DryRunGuard(Params);
	if (DryRunGuard.IsDryRun())
	{
		// Bind the single-property tree the walker expects.
		const TSharedPtr<FJsonValue> JsonValForDryRun = Params->TryGetField(TEXT("value"));
		if (!JsonValForDryRun.IsValid())
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("dry_run: 'value' field missing or null for property '%s'"), *PropertyName));
		}

		FBulkFillSpec Spec;
		Spec.TargetNamespace = TEXT("blueprint");
		Spec.TargetAsset = AssetPath;
		Spec.Tree = MakeShared<FJsonObject>();
		// Use the actual (case-corrected) property name resolved above, so the
		// walker's exact-then-case-insensitive lookup matches the same FProperty.
		Spec.Tree->SetField(Prop->GetName(), JsonValForDryRun);
		Spec.bDryRun = true;
		Spec.bStrict = DryRunGuard.IsStrict();

		const FDryRunReport Report = FMonolithReflectionWalker::InspectTree(
			Spec.Tree, TargetClass, TargetObject, Spec);
		return DryRunGuard.MakeDryRunResponse(Report);
	}

	// --- Engine edit cradle (matches Details panel write path) ---
	// Without this, cross-package TObjectPtr refs survive in memory but get silently
	// dropped by FLinkerSave's harvest walk on the next save (#29). The Details panel's
	// IPropertyHandle::SetValueFromFormattedString wraps the write in exactly this cradle:
	// transaction → Modify → PreEditChange(chain) → write → PostEditChangeChainProperty.
	// Bug-investigator H1 hypothesis: the engine's edit-side bookkeeping (transaction
	// buffer, OnObjectModified delegates, FOverridableManager overridden-property map)
	// must register the change for the harvester to register the import.
	TargetObject->SetFlags(RF_Transactional);
	FScopedTransaction Transaction(NSLOCTEXT("MonolithBlueprintCDOActions", "SetCDOProperty", "Monolith Set CDO Property"));
	TargetObject->Modify();

	FEditPropertyChain PropertyChain;
	PropertyChain.AddHead(Prop);
	PropertyChain.SetActivePropertyNode(Prop);
	TargetObject->PreEditChange(PropertyChain);

	// --- Set the value (JSON-aware for structs/arrays/maps, ImportText for scalars) ---
	const TSharedPtr<FJsonValue>& JsonVal = Params->TryGetField(TEXT("value"));

	if (JsonVal->Type == EJson::Object || JsonVal->Type == EJson::Array)
	{
		// Use FJsonObjectConverter for complex types (structs, TMaps, TArrays)
		if (!FJsonObjectConverter::JsonValueToUProperty(JsonVal, Prop, ValuePtr, 0, 0))
		{
			// Serialize the JSON value for error reporting (truncated)
			FString JsonStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
			FJsonSerializer::Serialize(JsonVal.ToSharedRef(), TEXT("value"), Writer);
			Writer->Close();
			if (JsonStr.Len() > 500) { JsonStr = JsonStr.Left(500) + TEXT("..."); }

			// Cancel the transaction so the failed-write doesn't pollute the undo buffer
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to set property '%s' from JSON — FJsonObjectConverter rejected the format. "
					 "Ensure the JSON structure matches the UProperty layout. Preview: %s"),
				*PropertyName, *JsonStr));
		}
	}
	else
	{
		// Scalar types: use ImportText
		FString ValStr;
		if (JsonVal->Type == EJson::Number)
		{
			ValStr = FString::SanitizeFloat(JsonVal->AsNumber());
		}
		else if (JsonVal->Type == EJson::Boolean)
		{
			ValStr = JsonVal->AsBool() ? TEXT("true") : TEXT("false");
		}
		else
		{
			ValStr = JsonVal->AsString();
		}

		const TCHAR* ImportResult = Prop->ImportText_Direct(*ValStr, ValuePtr, TargetObject, PPF_None);
		if (!ImportResult)
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to set property '%s' to value '%s' — ImportText rejected the format. "
					 "For structs use ImportText syntax e.g. \"(X=1.0,Y=2.0,Z=3.0)\", for enums use the display name."),
				*PropertyName, *ValStr));
		}
	}

	// FJsonObjectConverter outers new subobjects to /Engine/Transient when its container
	// isn't a UObject (JsonObjectConverter.cpp:964); FLinkerSave drops those refs at save.
	// Reparent before FireFullCradle so the cradle's Pre/Post fires on correct outers.
	MonolithEditCradle::ReparentTransientInstancedSubobjects(TargetObject, Prop);

	// Recursive cradle: fires PostEditChangeChainProperty for every nested
	// sub-property that contains an object reference, ensuring FOverridableManager
	// marks each inner TObjectPtr as overridden.
	MonolithEditCradle::FireFullCradle(TargetObject, Prop);

	// --- Read back new value for confirmation ---
	FString NewValue;
	Prop->ExportText_Direct(NewValue, ValuePtr, ValuePtr, TargetObject, PPF_None);

	// --- Mark dirty (PostEditChangeChainProperty handles property-change notifications;
	//     MarkBlueprintAsModified handles BP-asset-level recompile bookkeeping) ---
	if (BP)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	}
	else
	{
		TargetObject->MarkPackageDirty();
	}

	// --- Build response ---
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("property_name"), Prop->GetName());
	Root->SetStringField(TEXT("old_value"), OldValue);
	Root->SetStringField(TEXT("new_value"), NewValue);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// set_property_at_path — surgical nested-path writer.
//
// Resolves a dotted+bracket path to ONE leaf via FMonolithReflectionWalker::
// ResolvePath, then writes the value through WriteLeaf (the same coercion the
// bulk walker uses). The write is wrapped in the SAME engine edit cradle as
// set_cdo_property (transaction -> Modify -> PreEditChange -> write ->
// ReparentTransientInstancedSubobjects -> FireFullCradle -> MarkPackageDirty),
// which is what lets it write EditDefaultsOnly fields the editor blocks on
// saved instances — the cradle is FProperty-reflection-side, never the editor's
// PropertyAccessUtil edit-flag gate.
// ---------------------------------------------------------------------------
FMonolithActionResult FMonolithBlueprintCDOActions::HandleSetPropertyAtPath(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("set_property_at_path requires params"));
	}

	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	const FString Path = Params->GetStringField(TEXT("path"));
	if (Path.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: path"));
	}

	if (!Params->HasField(TEXT("value")))
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: value"));
	}
	const TSharedPtr<FJsonValue> JsonVal = Params->TryGetField(TEXT("value"));
	if (!JsonVal.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("'value' field missing or null"));
	}

	bool bCreateMissingKeys = false;
	Params->TryGetBoolField(TEXT("create_missing_keys"), bCreateMissingKeys);
	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);

	// --- Load asset: Blueprint CDO or generic UObject (same dual-path as set_cdo_property) ---
	UObject* TargetObject = nullptr;
	UClass* TargetClass = nullptr;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (BP && BP->GeneratedClass)
	{
		TargetClass = BP->GeneratedClass;
		TargetObject = TargetClass->GetDefaultObject(false);
	}
	else
	{
		BP = nullptr;
		TargetObject = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
		if (TargetObject)
		{
			TargetClass = TargetObject->GetClass();
		}
	}

	if (!TargetObject || !TargetClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found or has no class: %s"), *AssetPath));
	}

	FMonolithDryRunGuard DryRunGuard(Params);
	FBulkFillSpec Spec;
	Spec.TargetNamespace = TEXT("blueprint");
	Spec.TargetAsset = AssetPath;
	Spec.bDryRun = DryRunGuard.IsDryRun();
	Spec.bStrict = DryRunGuard.IsStrict();

	// --- Dry run: resolve the path against the LIVE object (read-only navigation;
	// create_missing_keys is forced OFF so a dry run never mutates map storage),
	// then validate the leaf write through a scratch buffer so nothing is touched.
	if (DryRunGuard.IsDryRun())
	{
		FMonolithReflectionWalker::FPathResolveResult Resolved =
			FMonolithReflectionWalker::ResolvePath(TargetClass, TargetObject, Path, /*bCreateMissingKeys=*/false);
		if (!Resolved.bOk)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("path '%s' did not resolve: %s"), *Path, *Resolved.Error));
		}

		// Validate the leaf grammar against a scratch copy of the leaf property so
		// the live value is never mutated.
		FDryRunReport Report;
		Report.bWouldApply = false;
		void* Scratch = FMemory::Malloc(Resolved.LeafProp->GetSize(), Resolved.LeafProp->GetMinAlignment());
		Resolved.LeafProp->InitializeValue(Scratch);
		const FBulkFillFieldWrite W = FMonolithReflectionWalker::WriteLeaf(
			Resolved.LeafProp, Scratch, JsonVal, nullptr, Spec, Report, Path);
		Resolved.LeafProp->DestroyValue(Scratch);
		FMemory::Free(Scratch);

		Report.FieldWrites.Add(W);
		Report.Errors = W.bOk ? 0 : 1;

		FMonolithActionResult Result = DryRunGuard.MakeDryRunResponse(Report);
		// Annotate the resolved leaf type so the caller can confirm what was targeted.
		if (Result.Result.IsValid())
		{
			Result.Result->SetStringField(TEXT("resolved_leaf_type"), Resolved.LeafTypeName);
		}
		return Result;
	}

	// --- Engine edit cradle (matches set_cdo_property / Details-panel write path).
	TargetObject->SetFlags(RF_Transactional);
	FScopedTransaction Transaction(NSLOCTEXT("MonolithBlueprintCDOActions", "SetPropertyAtPath", "Monolith Set Property At Path"));
	TargetObject->Modify();

	// Resolve the path on the live object (may mutate map storage if create_missing_keys).
	FMonolithReflectionWalker::FPathResolveResult Resolved =
		FMonolithReflectionWalker::ResolvePath(TargetClass, TargetObject, Path, bCreateMissingKeys);
	if (!Resolved.bOk)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("path '%s' did not resolve: %s"), *Path, *Resolved.Error));
	}

	// PreEditChange on the TOP-LEVEL property the path enters, so the cradle and
	// FOverridableManager register the edit. The path's first member is the head.
	FString HeadMember = Path;
	{
		int32 DotIdx = INDEX_NONE, BracketIdx = INDEX_NONE;
		Path.FindChar(TEXT('.'), DotIdx);
		Path.FindChar(TEXT('['), BracketIdx);
		int32 Cut = Path.Len();
		if (DotIdx != INDEX_NONE) { Cut = FMath::Min(Cut, DotIdx); }
		if (BracketIdx != INDEX_NONE) { Cut = FMath::Min(Cut, BracketIdx); }
		HeadMember = Path.Left(Cut);
	}
	FProperty* HeadProp = FMonolithReflectionWalker::FindPropertyForwarding(TargetClass, HeadMember);
	if (HeadProp)
	{
		FEditPropertyChain PropertyChain;
		PropertyChain.AddHead(HeadProp);
		PropertyChain.SetActivePropertyNode(HeadProp);
		TargetObject->PreEditChange(PropertyChain);
	}
	else
	{
		TargetObject->PreEditChange(nullptr);
	}

	// Snapshot old leaf value for the response.
	FString OldValue;
	Resolved.LeafProp->ExportText_Direct(OldValue, Resolved.LeafPtr, Resolved.LeafPtr, TargetObject, PPF_None);

	// --- Write the leaf through the shared walker coercion.
	FDryRunReport Report;
	Report.bWouldApply = true;
	const FBulkFillFieldWrite W = FMonolithReflectionWalker::WriteLeaf(
		Resolved.LeafProp, Resolved.LeafPtr, JsonVal, TargetObject, Spec, Report, Path);

	if (!W.bOk)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("write to '%s' (leaf type %s) failed: %s"),
			*Path, *Resolved.LeafTypeName, *W.Reason));
	}

	// --- Post-write cradle (mirror set_cdo_property). Fire on the head property so
	// nested object refs get FOverridableManager-marked + harvested at save.
	if (HeadProp)
	{
		MonolithEditCradle::ReparentTransientInstancedSubobjects(TargetObject, HeadProp);
		MonolithEditCradle::FireFullCradle(TargetObject, HeadProp);
	}

	// Read back the new leaf value for confirmation.
	FString NewValue;
	Resolved.LeafProp->ExportText_Direct(NewValue, Resolved.LeafPtr, Resolved.LeafPtr, TargetObject, PPF_None);

	// --- Mark dirty.
	if (BP)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	}
	else
	{
		TargetObject->MarkPackageDirty();
	}

	// --- Optional save to disk (pattern: MonolithAudioPerceptionActions.cpp:96).
	bool bSaved = false;
	FString SaveError;
	if (bSave)
	{
		UPackage* Package = TargetObject->GetPackage();
		if (Package)
		{
			const FString PackageFilename = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());
			bSaved = FMonolithAssetUtils::SaveAssetPackage(Package);
			if (!bSaved)
			{
				SaveError = FString::Printf(TEXT("SavePackage failed for %s"), *PackageFilename);
			}
		}
		else
		{
			SaveError = TEXT("asset has no package to save");
		}
	}

	// --- Build response ---
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("path"), Path);
	Root->SetStringField(TEXT("resolved_leaf_type"), Resolved.LeafTypeName);
	Root->SetStringField(TEXT("old_value"), OldValue);
	Root->SetStringField(TEXT("new_value"), NewValue);
	Root->SetBoolField(TEXT("created_missing_key"), bCreateMissingKeys);
	Root->SetBoolField(TEXT("saved"), bSaved);
	if (!SaveError.IsEmpty())
	{
		Root->SetStringField(TEXT("save_error"), SaveError);
	}

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Phase 1 — set_cdo_properties (plural) + describe_cdo_schema
// Thin wrappers that route through FMonolithBulkFillRegistry's "blueprint"
// adapter (registered from FMonolithBlueprintModule::StartupModule).
// ---------------------------------------------------------------------------

namespace MonolithCDOPhase1Internal
{
	FMonolithActionResult HandleSetCDOProperties(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("set_cdo_properties requires params"));
		}

		FBulkFillSpec Spec;
		Spec.TargetNamespace = TEXT("blueprint");
		Params->TryGetStringField(TEXT("asset_path"), Spec.TargetAsset);

		if (Spec.TargetAsset.IsEmpty())
		{
			return FMonolithActionResult::Error(
				TEXT("set_cdo_properties requires 'asset_path'"),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		// 'properties' is the JSON tree of property names -> values. Required.
		const TSharedPtr<FJsonObject>* TreePtr = nullptr;
		if (!Params->TryGetObjectField(TEXT("properties"), TreePtr) || !TreePtr || !TreePtr->IsValid())
		{
			return FMonolithActionResult::Error(
				TEXT("set_cdo_properties requires 'properties' (JSON object of prop_name -> value)"),
				FMonolithJsonUtils::ErrInvalidParams);
		}
		Spec.Tree = *TreePtr;

		Params->TryGetBoolField(TEXT("dry_run"), Spec.bDryRun);
		Params->TryGetBoolField(TEXT("strict"), Spec.bStrict);

		if (!FMonolithBulkFillRegistry::Get().HasAdapter(TEXT("blueprint")))
		{
			return FMonolithActionResult::Error(
				TEXT("blueprint bulk_fill adapter is not registered — module init order issue"),
				FMonolithJsonUtils::ErrInternalError);
		}

		const FDryRunReport Report = FMonolithBulkFillRegistry::Get().DispatchBulkFill(Spec);
		return FMonolithActionResult::Success(FMonolithDryRunGuard::ReportToJson(Report));
	}

	FMonolithActionResult HandleDescribeCDOSchema(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("describe_cdo_schema requires params"));
		}

		FString AssetPath;
		Params->TryGetStringField(TEXT("asset_path"), AssetPath);
		if (AssetPath.IsEmpty())
		{
			return FMonolithActionResult::Error(
				TEXT("describe_cdo_schema requires 'asset_path'"),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		if (!FMonolithBulkFillRegistry::Get().HasAdapter(TEXT("blueprint")))
		{
			return FMonolithActionResult::Error(
				TEXT("blueprint describe adapter is not registered — module init order issue"),
				FMonolithJsonUtils::ErrInternalError);
		}

		const FSchemaDescriptor Root = FMonolithBulkFillRegistry::Get().DispatchDescribe(
			TEXT("blueprint"), AssetPath);

		// Serialise the descriptor tree to JSON. We mirror the central dispatcher's
		// shape (MonolithBulkFillActions.cpp::DescriptorToJson) but inline a tiny
		// recursive emitter here to avoid coupling this module to MonolithCore's
		// private Actions/ folder.
		TFunction<TSharedPtr<FJsonObject>(const FSchemaDescriptor&)> ToJson =
			[&ToJson](const FSchemaDescriptor& Desc) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("field_path"), Desc.FieldPath);
			O->SetStringField(TEXT("type_name"), Desc.TypeName);
			O->SetStringField(TEXT("import_text_form"), Desc.ImportTextForm);
			O->SetBoolField(TEXT("required"), Desc.bRequired);
			O->SetBoolField(TEXT("set_once"), Desc.bSetOnce);
			O->SetBoolField(TEXT("pie_blocked"), Desc.bPieBlocked);
			if (!Desc.ConditionalOn.IsEmpty())
			{
				O->SetStringField(TEXT("conditional_on"), Desc.ConditionalOn);
			}
			O->SetNumberField(TEXT("range_min"), Desc.RangeMin);
			O->SetNumberField(TEXT("range_max"), Desc.RangeMax);
			if (Desc.EnumValues.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Vals;
				for (const FString& E : Desc.EnumValues) { Vals.Add(MakeShared<FJsonValueString>(E)); }
				O->SetArrayField(TEXT("enum_values"), Vals);
			}
			if (Desc.Children.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Kids;
				for (const FSchemaDescriptor& C : Desc.Children)
				{
					Kids.Add(MakeShared<FJsonValueObject>(ToJson(C)));
				}
				O->SetArrayField(TEXT("children"), Kids);
			}
			return O;
		};

		return FMonolithActionResult::Success(ToJson(Root));
	}
} // namespace MonolithCDOPhase1Internal
