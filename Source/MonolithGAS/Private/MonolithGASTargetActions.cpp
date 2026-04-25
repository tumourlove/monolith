#include "MonolithGASTargetActions.h"
#include "MonolithParamSchema.h"
#include "MonolithGASInternal.h"
#include "MonolithAssetUtils.h"

#include "Abilities/GameplayAbility.h"
#include "Abilities/GameplayAbilityTargetTypes.h"
#include "Abilities/GameplayAbilityTargetActor.h"
#include "Abilities/GameplayAbilityTargetActor_SingleLineTrace.h"
#include "Abilities/GameplayAbilityTargetActor_GroundTrace.h"
#include "Abilities/GameplayAbilityTargetActor_Radius.h"
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
#include "K2Node_LatentAbilityCall.h"
#include "Abilities/Tasks/AbilityTask_WaitTargetData.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"


// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

void FMonolithGASTargetActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("gas"), TEXT("create_target_actor"),
		TEXT("Create a GameplayAbilityTargetActor Blueprint. Supports sphere, line, ground, and custom targeting types."),
		FMonolithActionHandler::CreateStatic(&HandleCreateTargetActor),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path (e.g. '/Game/GAS/Targeting/TA_Hitscan')"))
			.Required(TEXT("targeting_type"), TEXT("string"), TEXT("Targeting type: 'line' (SingleLineTrace), 'sphere' (Radius), 'ground' (GroundTrace), 'custom' (base TargetActor)"))
			.Optional(TEXT("trace_channel"), TEXT("string"), TEXT("Trace channel name (e.g. 'Visibility', 'Camera')"))
			.Optional(TEXT("max_range"), TEXT("number"), TEXT("Maximum targeting range in units"))
			.Optional(TEXT("radius"), TEXT("number"), TEXT("Radius for sphere/ground targeting"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("configure_target_actor"),
		TEXT("Configure a TargetActor Blueprint: trace channel, range, radius, filters"),
		FMonolithActionHandler::CreateStatic(&HandleConfigureTargetActor),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("TargetActor Blueprint asset path"))
			.Optional(TEXT("trace_channel"), TEXT("string"), TEXT("Trace channel name"))
			.Optional(TEXT("max_range"), TEXT("number"), TEXT("Maximum range"))
			.Optional(TEXT("radius"), TEXT("number"), TEXT("Radius for sphere/ground targeting"))
			.Optional(TEXT("start_location_type"), TEXT("string"), TEXT("'player_transform', 'socket', 'literal'"))
			.Optional(TEXT("should_produce_target_data_on_server"), TEXT("boolean"), TEXT("If true, server generates its own target data"))
			.Optional(TEXT("debug_draw"), TEXT("boolean"), TEXT("Enable debug drawing"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("add_targeting_to_ability"),
		TEXT("Wire a WaitTargetData task into an ability Blueprint graph"),
		FMonolithActionHandler::CreateStatic(&HandleAddTargetingToAbility),
		FParamSchemaBuilder()
			.Required(TEXT("ability_path"), TEXT("string"), TEXT("Ability Blueprint asset path"))
			.Optional(TEXT("target_actor_class"), TEXT("string"), TEXT("TargetActor class path to use (default: auto-detect from project)"))
			.Optional(TEXT("confirm_type"), TEXT("string"), TEXT("Confirmation mode: 'instant', 'custom', 'custom_multi' (default: instant)"), TEXT("instant"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("scaffold_fps_targeting"),
		TEXT("FPS convenience: scaffold targeting flow for hitscan, projectile, melee_sweep, or aoe"),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldFPSTargeting),
		FParamSchemaBuilder()
			.Required(TEXT("ability_path"), TEXT("string"), TEXT("Ability Blueprint asset path"))
			.Required(TEXT("mode"), TEXT("string"), TEXT("Targeting mode: 'hitscan', 'projectile', 'melee_sweep', 'aoe'"))
			.Optional(TEXT("range"), TEXT("number"), TEXT("Max range in units (default: 10000 hitscan, 500 melee, 300 aoe)"))
			.Optional(TEXT("radius"), TEXT("number"), TEXT("Radius for aoe/melee_sweep (default: 150 melee, 300 aoe)"))
			.Optional(TEXT("save_path"), TEXT("string"), TEXT("Override save path for auto-created TargetActor (default: sibling folder)"))
			.Build());

	Registry.RegisterAction(TEXT("gas"), TEXT("validate_targeting"),
		TEXT("Validate targeting setup: check TargetActor class exists, WaitTargetData is present, flow is complete"),
		FMonolithActionHandler::CreateStatic(&HandleValidateTargeting),
		FParamSchemaBuilder()
			.Required(TEXT("ability_path"), TEXT("string"), TEXT("Ability Blueprint asset path"))
			.Build());
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

/** Map targeting_type string to parent class */
UClass* GetTargetActorParentClass(const FString& TypeStr)
{
	if (TypeStr.Equals(TEXT("line"), ESearchCase::IgnoreCase))
	{
		return AGameplayAbilityTargetActor_SingleLineTrace::StaticClass();
	}
	else if (TypeStr.Equals(TEXT("sphere"), ESearchCase::IgnoreCase))
	{
		return AGameplayAbilityTargetActor_Radius::StaticClass();
	}
	else if (TypeStr.Equals(TEXT("ground"), ESearchCase::IgnoreCase))
	{
		return AGameplayAbilityTargetActor_GroundTrace::StaticClass();
	}
	else if (TypeStr.Equals(TEXT("custom"), ESearchCase::IgnoreCase))
	{
		return AGameplayAbilityTargetActor::StaticClass();
	}
	return nullptr;
}

/** Set a float property on a CDO via reflection */
bool SetFloatPropOnCDO(UObject* CDO, const FString& PropName, float Value)
{
	FProperty* Prop = CDO->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop) return false;

	if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
	{
		FloatProp->SetPropertyValue(FloatProp->ContainerPtrToValuePtr<void>(CDO), Value);
		return true;
	}
	if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
	{
		DoubleProp->SetPropertyValue(DoubleProp->ContainerPtrToValuePtr<void>(CDO), static_cast<double>(Value));
		return true;
	}
	return false;
}

/** Set a bool property on a CDO via reflection */
bool SetBoolPropOnCDO(UObject* CDO, const FString& PropName, bool Value)
{
	FProperty* Prop = CDO->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop) return false;

	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
	{
		BoolProp->SetPropertyValue(BoolProp->ContainerPtrToValuePtr<void>(CDO), Value);
		return true;
	}
	return false;
}

/** Set a byte/enum property by name on a CDO */
bool SetEnumPropOnCDO(UObject* CDO, const FString& PropName, int32 Value)
{
	FProperty* Prop = CDO->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop) return false;

	if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
	{
		ByteProp->SetPropertyValue(ByteProp->ContainerPtrToValuePtr<void>(CDO), static_cast<uint8>(Value));
		return true;
	}
	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		uint8* ValPtr = EnumProp->ContainerPtrToValuePtr<uint8>(CDO);
		if (ValPtr)
		{
			*ValPtr = static_cast<uint8>(Value);
			return true;
		}
	}
	return false;
}

/** Read a float property from CDO */
bool GetFloatPropFromCDO(UObject* CDO, const FString& PropName, float& OutValue)
{
	FProperty* Prop = CDO->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop) return false;

	if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
	{
		OutValue = FloatProp->GetPropertyValue(FloatProp->ContainerPtrToValuePtr<void>(CDO));
		return true;
	}
	if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
	{
		OutValue = static_cast<float>(DoubleProp->GetPropertyValue(DoubleProp->ContainerPtrToValuePtr<void>(CDO)));
		return true;
	}
	return false;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// create_target_actor
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASTargetActions::HandleCreateTargetActor(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, TargetingType;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("save_path"), SavePath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("targeting_type"), TargetingType, Err)) return Err;

	UClass* ParentClass = GetTargetActorParentClass(TargetingType);
	if (!ParentClass)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid targeting_type: '%s'. Valid: line, sphere, ground, custom"), *TargetingType));
	}

	// Extract asset name
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

	// Create package and Blueprint
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *SavePath));
	}
	Package->FullyLoad();

	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());

	if (!NewBP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create TargetActor Blueprint at: %s"), *SavePath));
	}

	FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::SkipGarbageCollection);

	UObject* CDO = NewBP->GeneratedClass ? NewBP->GeneratedClass->GetDefaultObject() : nullptr;
	TArray<FString> ConfiguredProps;

	if (CDO)
	{
		// Apply optional configuration
		if (Params->HasField(TEXT("max_range")))
		{
			float Range = static_cast<float>(Params->GetNumberField(TEXT("max_range")));
			// Try common property names for range
			for (const TCHAR* PropName : { TEXT("MaxRange"), TEXT("MaxDistance"), TEXT("TraceRange") })
			{
				if (SetFloatPropOnCDO(CDO, PropName, Range))
				{
					ConfiguredProps.Add(FString::Printf(TEXT("%s = %.0f"), PropName, Range));
					break;
				}
			}
		}

		if (Params->HasField(TEXT("radius")))
		{
			float Radius = static_cast<float>(Params->GetNumberField(TEXT("radius")));
			for (const TCHAR* PropName : { TEXT("Radius"), TEXT("CollisionRadius"), TEXT("SphereRadius") })
			{
				if (SetFloatPropOnCDO(CDO, PropName, Radius))
				{
					ConfiguredProps.Add(FString::Printf(TEXT("%s = %.0f"), PropName, Radius));
					break;
				}
			}
		}

		// Recompile after CDO changes
		if (ConfiguredProps.Num() > 0)
		{
			FKismetEditorUtilities::CompileBlueprint(NewBP, EBlueprintCompileOptions::SkipGarbageCollection);
		}
	}

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewBP);

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	bool bSaved = UPackage::SavePackage(Package, NewBP, *PackageFilename, SaveArgs);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetStringField(TEXT("targeting_type"), TargetingType);
	Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (ConfiguredProps.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> PropsArr;
		for (const FString& S : ConfiguredProps) PropsArr.Add(MakeShared<FJsonValueString>(S));
		Result->SetArrayField(TEXT("configured_properties"), PropsArr);
	}
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Created %s TargetActor '%s'"), *TargetingType, *AssetName));

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// configure_target_actor
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASTargetActions::HandleConfigureTargetActor(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, Err)) return Err;

	FString Error;
	UObject* Obj = MonolithGAS::LoadAssetFromPath(AssetPath, Error);
	UBlueprint* BP = Cast<UBlueprint>(Obj);
	if (!BP || !BP->GeneratedClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load TargetActor BP: %s — %s"), *AssetPath, *Error));
	}

	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	if (!CDO || !CDO->GetClass()->IsChildOf(AGameplayAbilityTargetActor::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("'%s' is not a GameplayAbilityTargetActor"), *AssetPath));
	}

	TArray<FString> SetProps;

	// Max range
	if (Params->HasField(TEXT("max_range")))
	{
		float Range = static_cast<float>(Params->GetNumberField(TEXT("max_range")));
		for (const TCHAR* PropName : { TEXT("MaxRange"), TEXT("MaxDistance"), TEXT("TraceRange") })
		{
			if (SetFloatPropOnCDO(CDO, PropName, Range))
			{
				SetProps.Add(FString::Printf(TEXT("%s = %.0f"), PropName, Range));
				break;
			}
		}
	}

	// Radius
	if (Params->HasField(TEXT("radius")))
	{
		float Radius = static_cast<float>(Params->GetNumberField(TEXT("radius")));
		for (const TCHAR* PropName : { TEXT("Radius"), TEXT("CollisionRadius"), TEXT("SphereRadius") })
		{
			if (SetFloatPropOnCDO(CDO, PropName, Radius))
			{
				SetProps.Add(FString::Printf(TEXT("%s = %.0f"), PropName, Radius));
				break;
			}
		}
	}

	// ShouldProduceTargetDataOnServer
	if (Params->HasField(TEXT("should_produce_target_data_on_server")))
	{
		bool bVal = Params->GetBoolField(TEXT("should_produce_target_data_on_server"));
		if (SetBoolPropOnCDO(CDO, TEXT("ShouldProduceTargetDataOnServer"), bVal))
		{
			SetProps.Add(FString::Printf(TEXT("ShouldProduceTargetDataOnServer = %s"), bVal ? TEXT("true") : TEXT("false")));
		}
	}

	// Debug
	if (Params->HasField(TEXT("debug_draw")))
	{
		bool bVal = Params->GetBoolField(TEXT("debug_draw"));
		for (const TCHAR* PropName : { TEXT("bDebug"), TEXT("bDebugDraw"), TEXT("Debug") })
		{
			if (SetBoolPropOnCDO(CDO, PropName, bVal))
			{
				SetProps.Add(FString::Printf(TEXT("%s = %s"), PropName, bVal ? TEXT("true") : TEXT("false")));
				break;
			}
		}
	}

	// StartLocation type
	if (Params->HasField(TEXT("start_location_type")))
	{
		FString LocType = Params->GetStringField(TEXT("start_location_type"));
		int32 EnumVal = -1;
		if (LocType.Equals(TEXT("player_transform"), ESearchCase::IgnoreCase)) EnumVal = 0;
		else if (LocType.Equals(TEXT("socket"), ESearchCase::IgnoreCase)) EnumVal = 1;
		else if (LocType.Equals(TEXT("literal"), ESearchCase::IgnoreCase)) EnumVal = 2;

		if (EnumVal >= 0)
		{
			// StartLocation is a FGameplayAbilityTargetingLocationInfo struct — try LocationType enum inside it
			FProperty* StartLocProp = CDO->GetClass()->FindPropertyByName(TEXT("StartLocation"));
			if (StartLocProp)
			{
				FStructProperty* StructProp = CastField<FStructProperty>(StartLocProp);
				if (StructProp)
				{
					void* StructPtr = StructProp->ContainerPtrToValuePtr<void>(CDO);
					FProperty* LocationTypeProp = StructProp->Struct->FindPropertyByName(TEXT("LocationType"));
					if (LocationTypeProp)
					{
						uint8* EnumPtr = LocationTypeProp->ContainerPtrToValuePtr<uint8>(StructPtr);
						if (EnumPtr)
						{
							*EnumPtr = static_cast<uint8>(EnumVal);
							SetProps.Add(FString::Printf(TEXT("StartLocation.LocationType = %s"), *LocType));
						}
					}
				}
			}
		}
	}

	if (SetProps.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No properties were changed. Check parameter names and TargetActor type."));
	}

	BP->Modify();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(AssetPath,
		FString::Printf(TEXT("Configured %d properties on TargetActor"), SetProps.Num()));

	TArray<TSharedPtr<FJsonValue>> PropsArr;
	for (const FString& S : SetProps) PropsArr.Add(MakeShared<FJsonValueString>(S));
	Result->SetArrayField(TEXT("set_properties"), PropsArr);

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// add_targeting_to_ability
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASTargetActions::HandleAddTargetingToAbility(const TSharedPtr<FJsonObject>& Params)
{
	FString AbilityPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("ability_path"), AbilityPath, Err)) return Err;

	FString Error;
	UObject* Obj = MonolithGAS::LoadAssetFromPath(AbilityPath, Error);
	UBlueprint* BP = Cast<UBlueprint>(Obj);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load ability: %s — %s"), *AbilityPath, *Error));
	}
	if (!MonolithGAS::IsAbilityBlueprint(BP))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("'%s' is not a GameplayAbility Blueprint"), *AbilityPath));
	}

	FString ConfirmTypeStr = Params->GetStringField(TEXT("confirm_type"));
	if (ConfirmTypeStr.IsEmpty()) ConfirmTypeStr = TEXT("instant");

	FString TargetActorClassPath = Params->GetStringField(TEXT("target_actor_class"));

	// Resolve target actor class
	UClass* TargetActorClass = nullptr;
	if (!TargetActorClassPath.IsEmpty())
	{
		UObject* ClassObj = FMonolithAssetUtils::LoadAssetByPath(UClass::StaticClass(), TargetActorClassPath);
		TargetActorClass = Cast<UClass>(ClassObj);
		if (!TargetActorClass)
		{
			// Try loading as Blueprint
			UBlueprint* TABP = Cast<UBlueprint>(FMonolithAssetUtils::LoadAssetByPath(UBlueprint::StaticClass(), TargetActorClassPath));
			if (TABP && TABP->GeneratedClass)
			{
				TargetActorClass = TABP->GeneratedClass;
			}
		}
		if (!TargetActorClass || !TargetActorClass->IsChildOf(AGameplayAbilityTargetActor::StaticClass()))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("'%s' is not a valid TargetActor class"), *TargetActorClassPath));
		}
	}
	else
	{
		// Default: use SingleLineTrace
		TargetActorClass = AGameplayAbilityTargetActor_SingleLineTrace::StaticClass();
	}

	// Find the event graph
	UEdGraph* EventGraph = nullptr;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph && Graph->GetFName() == TEXT("EventGraph"))
		{
			EventGraph = Graph;
			break;
		}
	}
	if (!EventGraph && BP->UbergraphPages.Num() > 0)
	{
		EventGraph = BP->UbergraphPages[0];
	}
	if (!EventGraph)
	{
		return FMonolithActionResult::Error(TEXT("No EventGraph found in ability Blueprint"));
	}

	// Create the WaitTargetData ability task node via UK2Node_LatentAbilityCall
	UK2Node_LatentAbilityCall* WaitTargetNode = NewObject<UK2Node_LatentAbilityCall>(EventGraph);
	// ProxyFactoryFunctionName/ProxyFactoryClass/ProxyClass are protected in UE 5.7 — set via reflection
	{
		FProperty* FFNProp = WaitTargetNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryFunctionName"));
		if (FFNProp) { *FFNProp->ContainerPtrToValuePtr<FName>(WaitTargetNode) = FName(TEXT("WaitTargetData")); }
		FProperty* FFCProp = WaitTargetNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryClass"));
		if (FFCProp) { *FFCProp->ContainerPtrToValuePtr<UClass*>(WaitTargetNode) = UAbilityTask_WaitTargetData::StaticClass(); }
		FProperty* PCProp = WaitTargetNode->GetClass()->FindPropertyByName(TEXT("ProxyClass"));
		if (PCProp) { *PCProp->ContainerPtrToValuePtr<UClass*>(WaitTargetNode) = UAbilityTask_WaitTargetData::StaticClass(); }
	}
	WaitTargetNode->NodePosX = 400;
	WaitTargetNode->NodePosY = 0;

	EventGraph->AddNode(WaitTargetNode, false, false);
	WaitTargetNode->CreateNewGuid();
	WaitTargetNode->PostPlacedNewNode();
	WaitTargetNode->AllocateDefaultPins();

	// Compile to ensure graph integrity
	FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

	TSharedPtr<FJsonObject> Result = MonolithGAS::MakeAssetResult(AbilityPath,
		FString::Printf(TEXT("Added WaitTargetData task to ability '%s'"), *AbilityPath));
	Result->SetStringField(TEXT("target_actor_class"), TargetActorClass->GetPathName());
	Result->SetStringField(TEXT("confirm_type"), ConfirmTypeStr);
	Result->SetStringField(TEXT("node_type"), TEXT("UK2Node_LatentAbilityCall"));

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// scaffold_fps_targeting
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASTargetActions::HandleScaffoldFPSTargeting(const TSharedPtr<FJsonObject>& Params)
{
	FString AbilityPath, Mode;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("ability_path"), AbilityPath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("mode"), Mode, Err)) return Err;

	// Validate mode
	FString TargetingType;
	float DefaultRange = 10000.f;
	float DefaultRadius = 0.f;

	if (Mode.Equals(TEXT("hitscan"), ESearchCase::IgnoreCase))
	{
		TargetingType = TEXT("line");
		DefaultRange = 10000.f;
	}
	else if (Mode.Equals(TEXT("projectile"), ESearchCase::IgnoreCase))
	{
		TargetingType = TEXT("line");
		DefaultRange = 10000.f;
	}
	else if (Mode.Equals(TEXT("melee_sweep"), ESearchCase::IgnoreCase))
	{
		TargetingType = TEXT("sphere");
		DefaultRange = 500.f;
		DefaultRadius = 150.f;
	}
	else if (Mode.Equals(TEXT("aoe"), ESearchCase::IgnoreCase))
	{
		TargetingType = TEXT("sphere");
		DefaultRange = 300.f;
		DefaultRadius = 300.f;
	}
	else
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid mode: '%s'. Valid: hitscan, projectile, melee_sweep, aoe"), *Mode));
	}

	float Range = Params->HasField(TEXT("range"))
		? static_cast<float>(Params->GetNumberField(TEXT("range")))
		: DefaultRange;
	float Radius = Params->HasField(TEXT("radius"))
		? static_cast<float>(Params->GetNumberField(TEXT("radius")))
		: DefaultRadius;

	// Verify ability exists
	FString Error;
	UObject* AbilityObj = MonolithGAS::LoadAssetFromPath(AbilityPath, Error);
	UBlueprint* AbilityBP = Cast<UBlueprint>(AbilityObj);
	if (!AbilityBP || !MonolithGAS::IsAbilityBlueprint(AbilityBP))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("'%s' is not a valid ability Blueprint — %s"), *AbilityPath, *Error));
	}

	// Determine save path for auto-created TargetActor
	FString TASavePath = Params->GetStringField(TEXT("save_path"));
	if (TASavePath.IsEmpty())
	{
		// Extract directory from ability path and put TA there
		int32 LastSlash;
		if (AbilityPath.FindLastChar(TEXT('/'), LastSlash))
		{
			FString Dir = AbilityPath.Left(LastSlash);
			FString AbilityName = AbilityPath.Mid(LastSlash + 1);
			FString TAName = TEXT("TA_") + AbilityName.Replace(TEXT("GA_"), TEXT(""));
			TASavePath = Dir / TAName;
		}
		else
		{
			TASavePath = TEXT("/Game/GAS/Targeting/TA_") + Mode;
		}
	}

	// Step 1: Create TargetActor
	TSharedPtr<FJsonObject> CreateTAParams = MakeShared<FJsonObject>();
	CreateTAParams->SetStringField(TEXT("save_path"), TASavePath);
	CreateTAParams->SetStringField(TEXT("targeting_type"), TargetingType);
	CreateTAParams->SetNumberField(TEXT("max_range"), Range);
	if (Radius > 0.f)
	{
		CreateTAParams->SetNumberField(TEXT("radius"), Radius);
	}

	FMonolithActionResult CreateResult = HandleCreateTargetActor(CreateTAParams);
	bool bTACreated = CreateResult.bSuccess;
	FString TAClassPath;

	if (bTACreated)
	{
		TAClassPath = TASavePath;
	}
	else
	{
		// TA might already exist — try to load it
		UObject* ExistingTA = MonolithGAS::LoadAssetFromPath(TASavePath, Error);
		if (ExistingTA)
		{
			TAClassPath = TASavePath;
			bTACreated = false; // existing, not created
		}
		else
		{
			// Use the engine default
			TAClassPath = GetTargetActorParentClass(TargetingType)->GetPathName();
		}
	}

	// Step 2: Wire targeting into ability
	TSharedPtr<FJsonObject> WireParams = MakeShared<FJsonObject>();
	WireParams->SetStringField(TEXT("ability_path"), AbilityPath);
	WireParams->SetStringField(TEXT("target_actor_class"), TAClassPath);
	WireParams->SetStringField(TEXT("confirm_type"), TEXT("instant"));

	FMonolithActionResult WireResult = HandleAddTargetingToAbility(WireParams);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("ability_path"), AbilityPath);
	Result->SetStringField(TEXT("mode"), Mode);
	Result->SetStringField(TEXT("targeting_type"), TargetingType);
	Result->SetNumberField(TEXT("range"), Range);
	if (Radius > 0.f) Result->SetNumberField(TEXT("radius"), Radius);
	Result->SetStringField(TEXT("target_actor_path"), TASavePath);
	Result->SetBoolField(TEXT("target_actor_created"), bTACreated);
	Result->SetBoolField(TEXT("targeting_wired"), WireResult.bSuccess);

	if (!WireResult.bSuccess)
	{
		Result->SetStringField(TEXT("wire_error"), WireResult.ErrorMessage);
	}

	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Scaffolded %s FPS targeting for '%s'"), *Mode, *AbilityPath));

	return FMonolithActionResult::Success(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// validate_targeting
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASTargetActions::HandleValidateTargeting(const TSharedPtr<FJsonObject>& Params)
{
	FString AbilityPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("ability_path"), AbilityPath, Err)) return Err;

	FString Error;
	UObject* Obj = MonolithGAS::LoadAssetFromPath(AbilityPath, Error);
	UBlueprint* BP = Cast<UBlueprint>(Obj);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load: %s — %s"), *AbilityPath, *Error));
	}
	if (!MonolithGAS::IsAbilityBlueprint(BP))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("'%s' is not a GameplayAbility Blueprint"), *AbilityPath));
	}

	TArray<TSharedPtr<FJsonValue>> Issues;

	auto AddIssue = [&](const FString& Severity, const FString& Message)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), Severity);
		Issue->SetStringField(TEXT("message"), Message);
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	};

	// Search all graphs for WaitTargetData nodes
	bool bHasWaitTargetData = false;
	int32 WaitTargetDataCount = 0;
	bool bHasValidData = false;
	bool bHasCancelled = false;

	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;

			// Check for UK2Node_LatentAbilityCall with WaitTargetData
			UK2Node_LatentAbilityCall* LatentNode = Cast<UK2Node_LatentAbilityCall>(Node);
			if (LatentNode)
			{
				// ProxyFactoryFunctionName is protected in UE 5.7 — read via reflection
				FName NodeFuncName = NAME_None;
				FProperty* FFNProp = LatentNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryFunctionName"));
				if (FFNProp) { NodeFuncName = *FFNProp->ContainerPtrToValuePtr<FName>(LatentNode); }

				if (NodeFuncName == FName(TEXT("WaitTargetData")))
				{
					bHasWaitTargetData = true;
					WaitTargetDataCount++;

					// Check delegate pins are connected
					for (UEdGraphPin* Pin : LatentNode->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Output)
						{
							if (Pin->PinName == TEXT("ValidData") && Pin->LinkedTo.Num() > 0)
							{
								bHasValidData = true;
							}
							if (Pin->PinName == TEXT("Cancelled") && Pin->LinkedTo.Num() > 0)
							{
								bHasCancelled = true;
							}
						}
					}
				}
			}

			// Also check CallFunction nodes that might reference WaitTargetData
			UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
			if (CallNode)
			{
				FString FuncName = CallNode->GetFunctionName().ToString();
				if (FuncName.Contains(TEXT("WaitTargetData")))
				{
					bHasWaitTargetData = true;
					WaitTargetDataCount++;
				}
			}
		}
	}

	// Also check function graphs
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_LatentAbilityCall* LatentNode = Cast<UK2Node_LatentAbilityCall>(Node);
			if (LatentNode)
			{
				// ProxyFactoryFunctionName is protected in UE 5.7 — read via reflection
				FName NodeFuncName = NAME_None;
				FProperty* FFNProp = LatentNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryFunctionName"));
				if (FFNProp) { NodeFuncName = *FFNProp->ContainerPtrToValuePtr<FName>(LatentNode); }

				if (NodeFuncName == FName(TEXT("WaitTargetData")))
				{
					bHasWaitTargetData = true;
					WaitTargetDataCount++;
				}
			}
		}
	}

	if (!bHasWaitTargetData)
	{
		AddIssue(TEXT("info"), TEXT("No WaitTargetData task found in ability graph"));
	}
	else
	{
		if (!bHasValidData)
		{
			AddIssue(TEXT("warning"), TEXT("WaitTargetData 'ValidData' delegate is not connected — targeting results are not consumed"));
		}
		if (!bHasCancelled)
		{
			AddIssue(TEXT("info"), TEXT("WaitTargetData 'Cancelled' delegate is not connected — consider handling cancellation"));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("ability_path"), AbilityPath);
	Result->SetBoolField(TEXT("has_wait_target_data"), bHasWaitTargetData);
	Result->SetNumberField(TEXT("wait_target_data_count"), WaitTargetDataCount);
	Result->SetBoolField(TEXT("valid_data_connected"), bHasValidData);
	Result->SetBoolField(TEXT("cancelled_connected"), bHasCancelled);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetBoolField(TEXT("valid"), bHasWaitTargetData && bHasValidData);

	return FMonolithActionResult::Success(Result);
}
