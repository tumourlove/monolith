#include "MonolithMeshLevelDesignActions.h"
#include "MonolithMeshUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/RectLight.h"
#include "Engine/DirectionalLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/LightComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "UObject/UnrealType.h"

// Reuse the transaction helper from SceneActions
namespace LevelDesignHelpers
{
	/** Scoped undo transaction */
	struct FScopedMeshTransaction
	{
		bool bOwnsTransaction;

		FScopedMeshTransaction(const FText& Description)
			: bOwnsTransaction(true)
		{
			if (GEditor)
			{
				GEditor->BeginTransaction(Description);
			}
		}

		~FScopedMeshTransaction()
		{
			if (bOwnsTransaction && GEditor)
			{
				GEditor->EndTransaction();
			}
		}

		void Cancel()
		{
			if (bOwnsTransaction && GEditor)
			{
				GEditor->CancelTransaction(0);
				bOwnsTransaction = false;
			}
		}
	};

	/** Make a JSON array from a FVector */
	TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	/** Parse mobility string to enum */
	bool ParseMobility(const FString& Str, EComponentMobility::Type& Out)
	{
		if (Str.Equals(TEXT("Static"), ESearchCase::IgnoreCase))         { Out = EComponentMobility::Static; return true; }
		if (Str.Equals(TEXT("Stationary"), ESearchCase::IgnoreCase))     { Out = EComponentMobility::Stationary; return true; }
		if (Str.Equals(TEXT("Movable"), ESearchCase::IgnoreCase))        { Out = EComponentMobility::Movable; return true; }
		return false;
	}

	/** Convert FProperty value to a JSON value for reflection output */
	TSharedPtr<FJsonValue> PropertyToJson(const FProperty* Prop, const void* ValuePtr)
	{
		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			return MakeShared<FJsonValueBoolean>(BoolProp->GetPropertyValue(ValuePtr));
		}
		if (const FIntProperty* IntProp = CastField<FIntProperty>(Prop))
		{
			return MakeShared<FJsonValueNumber>(IntProp->GetPropertyValue(ValuePtr));
		}
		if (const FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{
			return MakeShared<FJsonValueNumber>(FloatProp->GetPropertyValue(ValuePtr));
		}
		if (const FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{
			return MakeShared<FJsonValueNumber>(DoubleProp->GetPropertyValue(ValuePtr));
		}
		if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			return MakeShared<FJsonValueString>(StrProp->GetPropertyValue(ValuePtr));
		}
		if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			return MakeShared<FJsonValueString>(NameProp->GetPropertyValue(ValuePtr).ToString());
		}
		if (const FTextProperty* TextProp = CastField<FTextProperty>(Prop))
		{
			return MakeShared<FJsonValueString>(TextProp->GetPropertyValue(ValuePtr).ToString());
		}
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			UEnum* Enum = EnumProp->GetEnum();
			const FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
			int64 Val = UnderlyingProp->GetSignedIntPropertyValue(ValuePtr);
			FString EnumStr = Enum ? Enum->GetNameByValue(Val).ToString() : FString::FromInt(Val);
			return MakeShared<FJsonValueString>(EnumStr);
		}
		if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (ByteProp->Enum)
			{
				uint8 Val = ByteProp->GetPropertyValue(ValuePtr);
				return MakeShared<FJsonValueString>(ByteProp->Enum->GetNameByValue(Val).ToString());
			}
			return MakeShared<FJsonValueNumber>(ByteProp->GetPropertyValue(ValuePtr));
		}
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			// Handle common vector/rotator/color types
			if (StructProp->Struct == TBaseStructure<FVector>::Get())
			{
				const FVector& Vec = *reinterpret_cast<const FVector*>(ValuePtr);
				auto Obj = MakeShared<FJsonObject>();
				Obj->SetNumberField(TEXT("x"), Vec.X);
				Obj->SetNumberField(TEXT("y"), Vec.Y);
				Obj->SetNumberField(TEXT("z"), Vec.Z);
				return MakeShared<FJsonValueObject>(Obj);
			}
			if (StructProp->Struct == TBaseStructure<FRotator>::Get())
			{
				const FRotator& Rot = *reinterpret_cast<const FRotator*>(ValuePtr);
				auto Obj = MakeShared<FJsonObject>();
				Obj->SetNumberField(TEXT("pitch"), Rot.Pitch);
				Obj->SetNumberField(TEXT("yaw"), Rot.Yaw);
				Obj->SetNumberField(TEXT("roll"), Rot.Roll);
				return MakeShared<FJsonValueObject>(Obj);
			}
			if (StructProp->Struct == TBaseStructure<FLinearColor>::Get())
			{
				const FLinearColor& Col = *reinterpret_cast<const FLinearColor*>(ValuePtr);
				auto Obj = MakeShared<FJsonObject>();
				Obj->SetNumberField(TEXT("r"), Col.R);
				Obj->SetNumberField(TEXT("g"), Col.G);
				Obj->SetNumberField(TEXT("b"), Col.B);
				Obj->SetNumberField(TEXT("a"), Col.A);
				return MakeShared<FJsonValueObject>(Obj);
			}
			if (StructProp->Struct == TBaseStructure<FColor>::Get())
			{
				const FColor& Col = *reinterpret_cast<const FColor*>(ValuePtr);
				auto Obj = MakeShared<FJsonObject>();
				Obj->SetNumberField(TEXT("r"), Col.R);
				Obj->SetNumberField(TEXT("g"), Col.G);
				Obj->SetNumberField(TEXT("b"), Col.B);
				Obj->SetNumberField(TEXT("a"), Col.A);
				return MakeShared<FJsonValueObject>(Obj);
			}
			// Fallback: export as string
			FString ExportText;
			StructProp->ExportTextItem_Direct(ExportText, ValuePtr, nullptr, nullptr, PPF_None);
			return MakeShared<FJsonValueString>(ExportText);
		}
		if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			UObject* Obj = ObjProp->GetObjectPropertyValue(ValuePtr);
			return MakeShared<FJsonValueString>(Obj ? Obj->GetPathName() : TEXT("None"));
		}
		// Fallback: export as string
		FString ExportText;
		Prop->ExportTextItem_Direct(ExportText, ValuePtr, nullptr, nullptr, PPF_None);
		return MakeShared<FJsonValueString>(ExportText);
	}
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshLevelDesignActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("mesh"), TEXT("place_light"),
		TEXT("Spawn a light actor (point/spot/rect/directional) with full property configuration"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshLevelDesignActions::PlaceLight),
		FParamSchemaBuilder()
			.Required(TEXT("type"), TEXT("string"), TEXT("Light type: point, spot, rect, directional"))
			.Required(TEXT("location"), TEXT("array"), TEXT("World location [x, y, z]"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("Rotation [pitch, yaw, roll]"), TEXT("[0,0,0]"))
			.Optional(TEXT("intensity"), TEXT("number"), TEXT("Light intensity (candelas for point/spot, lux for directional)"), TEXT("5000"))
			.Optional(TEXT("color"), TEXT("array"), TEXT("Light color [r, g, b] normalized 0-1"), TEXT("[1,1,1]"))
			.Optional(TEXT("attenuation_radius"), TEXT("number"), TEXT("Attenuation radius (point/spot/rect)"), TEXT("1000"))
			.Optional(TEXT("cast_shadows"), TEXT("boolean"), TEXT("Enable shadow casting"), TEXT("true"))
			.Optional(TEXT("temperature"), TEXT("number"), TEXT("Color temperature in Kelvin"), TEXT("6500"))
			.Optional(TEXT("use_temperature"), TEXT("boolean"), TEXT("Use temperature instead of color"), TEXT("false"))
			.Optional(TEXT("source_radius"), TEXT("number"), TEXT("Soft source radius (point/spot)"), TEXT("0"))
			.Optional(TEXT("source_width"), TEXT("number"), TEXT("Source width (rect light only)"), TEXT("64"))
			.Optional(TEXT("source_height"), TEXT("number"), TEXT("Source height (rect light only)"), TEXT("64"))
			.Optional(TEXT("inner_cone_angle"), TEXT("number"), TEXT("Inner cone angle in degrees (spot only)"), TEXT("25"))
			.Optional(TEXT("outer_cone_angle"), TEXT("number"), TEXT("Outer cone angle in degrees (spot only)"), TEXT("44"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Actor label"))
			.Optional(TEXT("folder"), TEXT("string"), TEXT("Outliner folder path"))
			.Optional(TEXT("mobility"), TEXT("string"), TEXT("Mobility: Static, Stationary, Movable"), TEXT("Stationary"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("set_light_properties"),
		TEXT("Modify properties on an existing light actor (intensity, color, shadows, temperature, cone angles, etc.)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshLevelDesignActions::SetLightProperties),
		FParamSchemaBuilder()
			.Required(TEXT("actor_name"), TEXT("string"), TEXT("Light actor name or label"))
			.Optional(TEXT("intensity"), TEXT("number"), TEXT("Light intensity"))
			.Optional(TEXT("color"), TEXT("array"), TEXT("Light color [r, g, b] normalized 0-1"))
			.Optional(TEXT("attenuation_radius"), TEXT("number"), TEXT("Attenuation radius"))
			.Optional(TEXT("cast_shadows"), TEXT("boolean"), TEXT("Enable shadow casting"))
			.Optional(TEXT("temperature"), TEXT("number"), TEXT("Color temperature in Kelvin"))
			.Optional(TEXT("use_temperature"), TEXT("boolean"), TEXT("Use temperature instead of color"))
			.Optional(TEXT("source_radius"), TEXT("number"), TEXT("Soft source radius (point/spot)"))
			.Optional(TEXT("source_width"), TEXT("number"), TEXT("Source width (rect only)"))
			.Optional(TEXT("source_height"), TEXT("number"), TEXT("Source height (rect only)"))
			.Optional(TEXT("inner_cone_angle"), TEXT("number"), TEXT("Inner cone angle (spot only)"))
			.Optional(TEXT("outer_cone_angle"), TEXT("number"), TEXT("Outer cone angle (spot only)"))
			.Optional(TEXT("mobility"), TEXT("string"), TEXT("Mobility: Static, Stationary, Movable"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("set_actor_material"),
		TEXT("Assign a material to an actor's mesh component by slot index or slot name. SetMaterial creates override array — setting slot 2 without 0-1 fills them with defaults."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshLevelDesignActions::SetActorMaterial),
		FParamSchemaBuilder()
			.Required(TEXT("actor_name"), TEXT("string"), TEXT("Actor name or label"))
			.Required(TEXT("material"), TEXT("string"), TEXT("Material asset path (e.g. /Game/Materials/MI_Concrete)"))
			.Optional(TEXT("slot"), TEXT("integer"), TEXT("Material slot index"), TEXT("0"))
			.Optional(TEXT("slot_name"), TEXT("string"), TEXT("Material slot name (alternative to slot index)"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Specific component name (if actor has multiple mesh components)"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("swap_material_in_level"),
		TEXT("Replace all instances of material X with material Y across actors or entire level"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshLevelDesignActions::SwapMaterialInLevel),
		FParamSchemaBuilder()
			.Required(TEXT("source_material"), TEXT("string"), TEXT("Source material asset path to find"))
			.Required(TEXT("target_material"), TEXT("string"), TEXT("Target material asset path to replace with"))
			.Optional(TEXT("actors"), TEXT("array"), TEXT("Specific actor names to process (default: all actors)"))
			.Optional(TEXT("preview"), TEXT("boolean"), TEXT("If true, report what would change without modifying"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("find_replace_mesh"),
		TEXT("Swap all instances of static mesh X with mesh Y. Essential for blockout-to-art pass."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshLevelDesignActions::FindReplaceMesh),
		FParamSchemaBuilder()
			.Required(TEXT("source_mesh"), TEXT("string"), TEXT("Source mesh asset path"))
			.Required(TEXT("target_mesh"), TEXT("string"), TEXT("Target mesh asset path"))
			.Optional(TEXT("actors"), TEXT("array"), TEXT("Specific actor names to process (default: all)"))
			.Optional(TEXT("match_mode"), TEXT("string"), TEXT("Match mode: exact or contains"), TEXT("exact"))
			.Optional(TEXT("preview"), TEXT("boolean"), TEXT("If true, report without modifying"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("set_lod_screen_sizes"),
		TEXT("Set per-LOD screen size thresholds on a static mesh asset. Sizes must be monotonically decreasing."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshLevelDesignActions::SetLodScreenSizes),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Static mesh asset path"))
			.Required(TEXT("screen_sizes"), TEXT("array"), TEXT("Array of screen size floats per LOD (e.g. [1.0, 0.4, 0.15])"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("find_instancing_candidates"),
		TEXT("Identify meshes used many times that could benefit from HISM conversion. Groups by mesh and material set."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshLevelDesignActions::FindInstancingCandidates),
		FParamSchemaBuilder()
			.Optional(TEXT("min_count"), TEXT("integer"), TEXT("Minimum instance count to report"), TEXT("5"))
			.Optional(TEXT("region_min"), TEXT("array"), TEXT("Region AABB minimum [x, y, z]"))
			.Optional(TEXT("region_max"), TEXT("array"), TEXT("Region AABB maximum [x, y, z]"))
			.Optional(TEXT("include_materials"), TEXT("boolean"), TEXT("Include material override info in grouping"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("convert_to_hism"),
		TEXT("Convert grouped StaticMeshActors into a single HISM actor. Deletes originals. Single undo transaction."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshLevelDesignActions::ConvertToHism),
		FParamSchemaBuilder()
			.Required(TEXT("mesh"), TEXT("string"), TEXT("Static mesh asset path for the HISM"))
			.Required(TEXT("actors"), TEXT("array"), TEXT("Array of actor names to convert"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Label for the new HISM actor"))
			.Optional(TEXT("folder"), TEXT("string"), TEXT("Outliner folder path"))
			.Optional(TEXT("preserve_materials"), TEXT("boolean"), TEXT("Copy material overrides from first actor"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("get_actor_component_properties"),
		TEXT("Read arbitrary component properties via FProperty reflection. Returns typed values for any UPROPERTY."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshLevelDesignActions::GetActorComponentProperties),
		FParamSchemaBuilder()
			.Required(TEXT("actor_name"), TEXT("string"), TEXT("Actor name or label"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Specific component name (default: root component)"))
			.Optional(TEXT("properties"), TEXT("array"), TEXT("Array of property names to read (default: all visible)"))
			.Optional(TEXT("component_class"), TEXT("string"), TEXT("Filter by component class name"))
			.Build());
}

// ============================================================================
// Helper: Apply light properties from JSON
// ============================================================================

TArray<FString> FMonolithMeshLevelDesignActions::ApplyLightProperties(ULightComponent* LightComp, const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> PropsSet;

	// Mobility MUST be applied first — SetAttenuationRadius, SetInnerConeAngle, etc.
	// silently no-op on non-Movable lights via AreDynamicDataChangesAllowed()
	FString MobilityStr;
	if (Params->TryGetStringField(TEXT("mobility"), MobilityStr))
	{
		EComponentMobility::Type Mobility;
		if (LevelDesignHelpers::ParseMobility(MobilityStr, Mobility))
		{
			LightComp->SetMobility(Mobility);
			PropsSet.Add(TEXT("mobility"));
		}
	}

	double Intensity;
	if (Params->TryGetNumberField(TEXT("intensity"), Intensity))
	{
		LightComp->SetIntensity(static_cast<float>(Intensity));
		PropsSet.Add(TEXT("intensity"));
	}

	const TArray<TSharedPtr<FJsonValue>>* ColorArr;
	if (Params->TryGetArrayField(TEXT("color"), ColorArr) && ColorArr->Num() >= 3)
	{
		FLinearColor Color(
			static_cast<float>((*ColorArr)[0]->AsNumber()),
			static_cast<float>((*ColorArr)[1]->AsNumber()),
			static_cast<float>((*ColorArr)[2]->AsNumber())
		);
		LightComp->SetLightColor(Color);
		PropsSet.Add(TEXT("color"));
	}

	double AttenuationRadius;
	if (Params->TryGetNumberField(TEXT("attenuation_radius"), AttenuationRadius))
	{
		if (ULocalLightComponent* LocalLight = Cast<ULocalLightComponent>(LightComp))
		{
			// Direct UPROPERTY write — SetAttenuationRadius() silently no-ops on
			// non-Movable lights via AreDynamicDataChangesAllowed(). We bypass
			// that check since this is an editor tool, not runtime code.
			LocalLight->Modify();
			LocalLight->AttenuationRadius = static_cast<float>(AttenuationRadius);
			LocalLight->MarkRenderStateDirty();
		}
		PropsSet.Add(TEXT("attenuation_radius"));
	}

	// bCastShadows — no setter, use Modify() + direct assignment
	bool bCastShadows;
	if (Params->TryGetBoolField(TEXT("cast_shadows"), bCastShadows))
	{
		LightComp->Modify();
		LightComp->CastShadows = bCastShadows;
		PropsSet.Add(TEXT("cast_shadows"));
	}

	double Temperature;
	if (Params->TryGetNumberField(TEXT("temperature"), Temperature))
	{
		LightComp->Modify();
		LightComp->Temperature = static_cast<float>(Temperature);
		PropsSet.Add(TEXT("temperature"));
	}

	bool bUseTemperature;
	if (Params->TryGetBoolField(TEXT("use_temperature"), bUseTemperature))
	{
		LightComp->Modify();
		LightComp->bUseTemperature = bUseTemperature;
		PropsSet.Add(TEXT("use_temperature"));
	}

	// Source radius — point/spot only
	double SourceRadius;
	if (Params->TryGetNumberField(TEXT("source_radius"), SourceRadius))
	{
		if (UPointLightComponent* PLC = Cast<UPointLightComponent>(LightComp))
		{
			PLC->SetSourceRadius(static_cast<float>(SourceRadius));
			PropsSet.Add(TEXT("source_radius"));
		}
	}

	// Rect light: SourceWidth / SourceHeight
	if (URectLightComponent* RLC = Cast<URectLightComponent>(LightComp))
	{
		double SourceWidth;
		if (Params->TryGetNumberField(TEXT("source_width"), SourceWidth))
		{
			RLC->Modify();
			RLC->SourceWidth = static_cast<float>(SourceWidth);
			PropsSet.Add(TEXT("source_width"));
		}
		double SourceHeight;
		if (Params->TryGetNumberField(TEXT("source_height"), SourceHeight))
		{
			RLC->Modify();
			RLC->SourceHeight = static_cast<float>(SourceHeight);
			PropsSet.Add(TEXT("source_height"));
		}
	}

	// Spot light: cone angles
	if (USpotLightComponent* SLC = Cast<USpotLightComponent>(LightComp))
	{
		double InnerConeAngle;
		if (Params->TryGetNumberField(TEXT("inner_cone_angle"), InnerConeAngle))
		{
			SLC->SetInnerConeAngle(static_cast<float>(InnerConeAngle));
			PropsSet.Add(TEXT("inner_cone_angle"));
		}
		double OuterConeAngle;
		if (Params->TryGetNumberField(TEXT("outer_cone_angle"), OuterConeAngle))
		{
			SLC->SetOuterConeAngle(static_cast<float>(OuterConeAngle));
			PropsSet.Add(TEXT("outer_cone_angle"));
		}
	}

	return PropsSet;
}

// ============================================================================
// 1. place_light
// ============================================================================

FMonolithActionResult FMonolithMeshLevelDesignActions::PlaceLight(const TSharedPtr<FJsonObject>& Params)
{
	FString TypeStr;
	if (!Params->TryGetStringField(TEXT("type"), TypeStr))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: type (point, spot, rect, directional)"));
	}
	TypeStr = TypeStr.ToLower();

	FVector Location;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: location (array of 3 numbers)"));
	}

	FRotator Rotation(0, 0, 0);
	MonolithMeshUtils::ParseRotator(Params, TEXT("rotation"), Rotation);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Determine actor class
	UClass* LightClass = nullptr;
	FString ClassName;
	if (TypeStr == TEXT("point"))
	{
		LightClass = APointLight::StaticClass();
		ClassName = TEXT("PointLight");
	}
	else if (TypeStr == TEXT("spot"))
	{
		LightClass = ASpotLight::StaticClass();
		ClassName = TEXT("SpotLight");
	}
	else if (TypeStr == TEXT("rect"))
	{
		LightClass = ARectLight::StaticClass();
		ClassName = TEXT("RectLight");
	}
	else if (TypeStr == TEXT("directional"))
	{
		LightClass = ADirectionalLight::StaticClass();
		ClassName = TEXT("DirectionalLight");
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid light type: '%s'. Use point, spot, rect, or directional."), *TypeStr));
	}

	LevelDesignHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Place Light")));

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* SpawnedActor = World->SpawnActor(LightClass, &Location, &Rotation, SpawnParams);
	if (!SpawnedActor)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to spawn %s"), *ClassName));
	}

	// Get the light component
	ULightComponent* LightComp = SpawnedActor->FindComponentByClass<ULightComponent>();
	if (!LightComp)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(TEXT("Spawned light actor has no ULightComponent"));
	}

	// Apply all properties
	TArray<FString> PropsSet = ApplyLightProperties(LightComp, Params);

	// Name and folder
	FString OptionalName;
	if (Params->TryGetStringField(TEXT("name"), OptionalName) && !OptionalName.IsEmpty())
	{
		SpawnedActor->SetActorLabel(OptionalName);
	}

	FString Folder;
	Params->TryGetStringField(TEXT("folder"), Folder);
	if (!Folder.IsEmpty())
	{
		SpawnedActor->SetFolderPath(FName(*Folder));
	}
	else
	{
		SpawnedActor->SetFolderPath(FName(TEXT("Lights")));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), SpawnedActor->GetActorNameOrLabel());
	Result->SetStringField(TEXT("class"), ClassName);
	Result->SetArrayField(TEXT("location"), LevelDesignHelpers::VectorToJsonArray(SpawnedActor->GetActorLocation()));

	TArray<TSharedPtr<FJsonValue>> PropsArr;
	for (const FString& P : PropsSet)
	{
		PropsArr.Add(MakeShared<FJsonValueString>(P));
	}
	Result->SetArrayField(TEXT("properties_set"), PropsArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 2. set_light_properties
// ============================================================================

FMonolithActionResult FMonolithMeshLevelDesignActions::SetLightProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: actor_name"));
	}

	FString Error;
	AActor* Actor = MonolithMeshUtils::FindActorByName(ActorName, Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	ULightComponent* LightComp = Actor->FindComponentByClass<ULightComponent>();
	if (!LightComp)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Actor '%s' has no ULightComponent"), *ActorName));
	}

	LevelDesignHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Set Light Properties")));

	TArray<FString> PropsSet = ApplyLightProperties(LightComp, Params);

	if (PropsSet.Num() == 0)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(TEXT("No valid light properties provided"));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	Result->SetStringField(TEXT("light_class"), LightComp->GetClass()->GetName());

	TArray<TSharedPtr<FJsonValue>> PropsArr;
	for (const FString& P : PropsSet)
	{
		PropsArr.Add(MakeShared<FJsonValueString>(P));
	}
	Result->SetArrayField(TEXT("properties_set"), PropsArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 3. set_actor_material
// ============================================================================

FMonolithActionResult FMonolithMeshLevelDesignActions::SetActorMaterial(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: actor_name"));
	}

	FString MaterialPath;
	if (!Params->TryGetStringField(TEXT("material"), MaterialPath))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: material"));
	}

	FString Error;
	AActor* Actor = MonolithMeshUtils::FindActorByName(ActorName, Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Load material
	UMaterialInterface* Material = FMonolithAssetUtils::LoadAssetByPath<UMaterialInterface>(MaterialPath);
	if (!Material)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
	}

	// Find mesh component
	UMeshComponent* MeshComp = nullptr;
	FString ComponentName;
	if (Params->TryGetStringField(TEXT("component_name"), ComponentName) && !ComponentName.IsEmpty())
	{
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Comp : Components)
		{
			if (Comp->GetFName().ToString() == ComponentName)
			{
				MeshComp = Cast<UMeshComponent>(Comp);
				break;
			}
		}
		if (!MeshComp)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Mesh component '%s' not found on actor '%s'"), *ComponentName, *ActorName));
		}
	}
	else
	{
		MeshComp = Actor->FindComponentByClass<UMeshComponent>();
		if (!MeshComp)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Actor '%s' has no mesh component"), *ActorName));
		}
	}

	// Determine slot
	int32 SlotIndex = 0;
	FString SlotName;
	bool bUseSlotName = Params->TryGetStringField(TEXT("slot_name"), SlotName) && !SlotName.IsEmpty();

	if (!bUseSlotName)
	{
		double SlotVal;
		if (Params->TryGetNumberField(TEXT("slot"), SlotVal))
		{
			SlotIndex = static_cast<int32>(SlotVal);
		}
	}

	// Get old material for return value
	FString OldMaterialPath = TEXT("None");
	if (bUseSlotName)
	{
		// Find index by slot name from the underlying mesh asset
		int32 FoundIndex = INDEX_NONE;
		if (UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(MeshComp))
		{
			if (UStaticMesh* SM = SMC->GetStaticMesh())
			{
				const TArray<FStaticMaterial>& Mats = SM->GetStaticMaterials();
				for (int32 i = 0; i < Mats.Num(); ++i)
				{
					if (Mats[i].MaterialSlotName.ToString().Equals(SlotName, ESearchCase::IgnoreCase))
					{
						FoundIndex = i;
						break;
					}
				}
			}
		}
		else
		{
			// For skeletal or other mesh types, try GetMaterialIndex if available
			int32 MaterialIndex = MeshComp->GetMaterialIndex(FName(*SlotName));
			if (MaterialIndex != INDEX_NONE)
			{
				FoundIndex = MaterialIndex;
			}
		}
		if (FoundIndex == INDEX_NONE)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Material slot name '%s' not found on component"), *SlotName));
		}
		SlotIndex = FoundIndex;
	}

	if (SlotIndex >= MeshComp->GetNumMaterials())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Slot index %d out of range (component has %d slots)"), SlotIndex, MeshComp->GetNumMaterials()));
	}

	UMaterialInterface* OldMat = MeshComp->GetMaterial(SlotIndex);
	if (OldMat)
	{
		OldMaterialPath = OldMat->GetPathName();
	}

	LevelDesignHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Set Actor Material")));

	MeshComp->Modify();
	MeshComp->SetMaterial(SlotIndex, Material);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	Result->SetStringField(TEXT("component"), MeshComp->GetFName().ToString());
	Result->SetNumberField(TEXT("slot"), SlotIndex);
	Result->SetStringField(TEXT("old_material"), OldMaterialPath);
	Result->SetStringField(TEXT("new_material"), Material->GetPathName());

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 4. swap_material_in_level
// ============================================================================

FMonolithActionResult FMonolithMeshLevelDesignActions::SwapMaterialInLevel(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath, TargetPath;
	if (!Params->TryGetStringField(TEXT("source_material"), SourcePath))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: source_material"));
	}
	if (!Params->TryGetStringField(TEXT("target_material"), TargetPath))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: target_material"));
	}

	bool bPreview = false;
	Params->TryGetBoolField(TEXT("preview"), bPreview);

	UMaterialInterface* SourceMat = FMonolithAssetUtils::LoadAssetByPath<UMaterialInterface>(SourcePath);
	if (!SourceMat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source material not found: %s"), *SourcePath));
	}

	UMaterialInterface* TargetMat = nullptr;
	if (!bPreview)
	{
		TargetMat = FMonolithAssetUtils::LoadAssetByPath<UMaterialInterface>(TargetPath);
		if (!TargetMat)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Target material not found: %s"), *TargetPath));
		}
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Optional: specific actor filter
	TSet<FString> ActorFilter;
	const TArray<TSharedPtr<FJsonValue>>* ActorsArr;
	if (Params->TryGetArrayField(TEXT("actors"), ActorsArr))
	{
		for (const auto& Val : *ActorsArr)
		{
			ActorFilter.Add(Val->AsString());
		}
	}

	TSharedPtr<LevelDesignHelpers::FScopedMeshTransaction> Transaction;
	if (!bPreview)
	{
		Transaction = MakeShared<LevelDesignHelpers::FScopedMeshTransaction>(FText::FromString(TEXT("Monolith: Swap Material In Level")));
	}

	int32 ActorsModified = 0;
	int32 SlotsModified = 0;
	TArray<TSharedPtr<FJsonValue>> Details;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;

		// Apply actor filter
		if (ActorFilter.Num() > 0)
		{
			if (!ActorFilter.Contains(Actor->GetActorNameOrLabel()) && !ActorFilter.Contains(Actor->GetFName().ToString()))
			{
				continue;
			}
		}

		TArray<UMeshComponent*> MeshComps;
		Actor->GetComponents<UMeshComponent>(MeshComps);

		bool bActorModified = false;
		for (UMeshComponent* MC : MeshComps)
		{
			for (int32 i = 0; i < MC->GetNumMaterials(); ++i)
			{
				UMaterialInterface* CurMat = MC->GetMaterial(i);
				if (CurMat && CurMat == SourceMat)
				{
					if (!bPreview)
					{
						MC->Modify();
						MC->SetMaterial(i, TargetMat);
					}
					SlotsModified++;
					bActorModified = true;

					auto Detail = MakeShared<FJsonObject>();
					Detail->SetStringField(TEXT("actor"), Actor->GetActorNameOrLabel());
					Detail->SetStringField(TEXT("component"), MC->GetFName().ToString());
					Detail->SetNumberField(TEXT("slot"), i);
					Details.Add(MakeShared<FJsonValueObject>(Detail));
				}
			}
		}

		if (bActorModified)
		{
			ActorsModified++;
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("actors_modified"), ActorsModified);
	Result->SetNumberField(TEXT("slots_modified"), SlotsModified);
	Result->SetBoolField(TEXT("preview"), bPreview);
	Result->SetArrayField(TEXT("details"), Details);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 5. find_replace_mesh
// ============================================================================

FMonolithActionResult FMonolithMeshLevelDesignActions::FindReplaceMesh(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath, TargetPath;
	if (!Params->TryGetStringField(TEXT("source_mesh"), SourcePath))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: source_mesh"));
	}
	if (!Params->TryGetStringField(TEXT("target_mesh"), TargetPath))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: target_mesh"));
	}

	FString MatchMode = TEXT("exact");
	Params->TryGetStringField(TEXT("match_mode"), MatchMode);

	bool bPreview = false;
	Params->TryGetBoolField(TEXT("preview"), bPreview);

	UStaticMesh* SourceMesh = FMonolithAssetUtils::LoadAssetByPath<UStaticMesh>(SourcePath);
	if (!SourceMesh)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source mesh not found: %s"), *SourcePath));
	}

	UStaticMesh* TargetMesh = nullptr;
	if (!bPreview)
	{
		TargetMesh = FMonolithAssetUtils::LoadAssetByPath<UStaticMesh>(TargetPath);
		if (!TargetMesh)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Target mesh not found: %s"), *TargetPath));
		}
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Optional: specific actor filter
	TSet<FString> ActorFilter;
	const TArray<TSharedPtr<FJsonValue>>* ActorsArr;
	if (Params->TryGetArrayField(TEXT("actors"), ActorsArr))
	{
		for (const auto& Val : *ActorsArr)
		{
			ActorFilter.Add(Val->AsString());
		}
	}

	bool bContains = MatchMode.Equals(TEXT("contains"), ESearchCase::IgnoreCase);
	FString SourceMeshPath = SourceMesh->GetPathName();

	TSharedPtr<LevelDesignHelpers::FScopedMeshTransaction> Transaction;
	if (!bPreview)
	{
		Transaction = MakeShared<LevelDesignHelpers::FScopedMeshTransaction>(FText::FromString(TEXT("Monolith: Find Replace Mesh")));
	}

	int32 ReplacedCount = 0;
	TArray<TSharedPtr<FJsonValue>> ModifiedActors;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;

		// Skip ISM/HISM actors unless explicitly targeted
		if (ActorFilter.Num() == 0)
		{
			if (Actor->FindComponentByClass<UInstancedStaticMeshComponent>())
			{
				continue;
			}
		}

		if (ActorFilter.Num() > 0)
		{
			if (!ActorFilter.Contains(Actor->GetActorNameOrLabel()) && !ActorFilter.Contains(Actor->GetFName().ToString()))
			{
				continue;
			}
		}

		TArray<UStaticMeshComponent*> SMCs;
		Actor->GetComponents<UStaticMeshComponent>(SMCs);

		for (UStaticMeshComponent* SMC : SMCs)
		{
			// Skip ISM components
			if (Cast<UInstancedStaticMeshComponent>(SMC))
			{
				continue;
			}

			UStaticMesh* CurMesh = SMC->GetStaticMesh();
			if (!CurMesh) continue;

			bool bMatch = false;
			if (bContains)
			{
				bMatch = CurMesh->GetPathName().Contains(SourcePath);
			}
			else
			{
				bMatch = (CurMesh == SourceMesh);
			}

			if (bMatch)
			{
				if (!bPreview)
				{
					// Modify() BEFORE SetStaticMesh for undo
					SMC->Modify();
					SMC->SetStaticMesh(TargetMesh);

					// Warn about material slot count mismatch
					if (TargetMesh->GetStaticMaterials().Num() != SourceMesh->GetStaticMaterials().Num())
					{
						UE_LOG(LogTemp, Warning, TEXT("Monolith: Mesh swap on '%s' — material slot count differs (source: %d, target: %d). Material overrides may be invalid."),
							*Actor->GetActorNameOrLabel(),
							SourceMesh->GetStaticMaterials().Num(),
							TargetMesh->GetStaticMaterials().Num());
					}
				}
				ReplacedCount++;
				ModifiedActors.Add(MakeShared<FJsonValueString>(Actor->GetActorNameOrLabel()));
			}
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("replaced"), ReplacedCount);
	Result->SetArrayField(TEXT("actors_modified"), ModifiedActors);
	Result->SetStringField(TEXT("source_mesh"), SourcePath);
	Result->SetStringField(TEXT("target_mesh"), TargetPath);
	Result->SetBoolField(TEXT("preview"), bPreview);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 6. set_lod_screen_sizes
// ============================================================================

FMonolithActionResult FMonolithMeshLevelDesignActions::SetLodScreenSizes(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: asset_path"));
	}

	const TArray<TSharedPtr<FJsonValue>>* ScreenSizesArr;
	if (!Params->TryGetArrayField(TEXT("screen_sizes"), ScreenSizesArr) || ScreenSizesArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: screen_sizes"));
	}

	UStaticMesh* SM = FMonolithAssetUtils::LoadAssetByPath<UStaticMesh>(AssetPath);
	if (!SM)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Static mesh not found: %s"), *AssetPath));
	}

	int32 LODCount = SM->GetNumSourceModels();
	if (ScreenSizesArr->Num() > LODCount)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Provided %d screen sizes but mesh only has %d LODs"), ScreenSizesArr->Num(), LODCount));
	}

	// Parse and validate monotonically decreasing
	TArray<float> NewSizes;
	for (int32 i = 0; i < ScreenSizesArr->Num(); ++i)
	{
		float Val = static_cast<float>((*ScreenSizesArr)[i]->AsNumber());
		if (i > 0 && Val >= NewSizes.Last())
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Screen sizes must be monotonically decreasing. Size[%d]=%.4f >= Size[%d]=%.4f"), i, Val, i - 1, NewSizes.Last()));
		}
		NewSizes.Add(Val);
	}

	// Record previous sizes
	TArray<TSharedPtr<FJsonValue>> PrevSizesArr;
	for (int32 i = 0; i < LODCount; ++i)
	{
		PrevSizesArr.Add(MakeShared<FJsonValueNumber>(SM->GetSourceModel(i).ScreenSize.Default));
	}

	LevelDesignHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Set LOD Screen Sizes")));

	SM->Modify();
	for (int32 i = 0; i < NewSizes.Num(); ++i)
	{
		SM->GetSourceModel(i).ScreenSize.Default = NewSizes[i];
	}

	// Rebuild render data
	SM->Build(false);
	SM->PostEditChange();
	SM->MarkPackageDirty();

	TArray<TSharedPtr<FJsonValue>> NewSizesArr;
	for (int32 i = 0; i < LODCount; ++i)
	{
		NewSizesArr.Add(MakeShared<FJsonValueNumber>(SM->GetSourceModel(i).ScreenSize.Default));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("lod_count"), LODCount);
	Result->SetArrayField(TEXT("screen_sizes"), NewSizesArr);
	Result->SetArrayField(TEXT("previous_sizes"), PrevSizesArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 7. find_instancing_candidates
// ============================================================================

FMonolithActionResult FMonolithMeshLevelDesignActions::FindInstancingCandidates(const TSharedPtr<FJsonObject>& Params)
{
	int32 MinCount = 5;
	double MinCountVal;
	if (Params->TryGetNumberField(TEXT("min_count"), MinCountVal))
	{
		MinCount = static_cast<int32>(MinCountVal);
	}

	bool bIncludeMaterials = true;
	Params->TryGetBoolField(TEXT("include_materials"), bIncludeMaterials);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Optional region filter
	FVector RegionMin(ForceInit), RegionMax(ForceInit);
	bool bHasRegion = MonolithMeshUtils::ParseVector(Params, TEXT("region_min"), RegionMin)
	                && MonolithMeshUtils::ParseVector(Params, TEXT("region_max"), RegionMax);

	// Group key: mesh path + optional material set hash
	struct FInstanceGroup
	{
		FString MeshPath;
		TArray<FString> MaterialPaths;
		TArray<FString> ActorNames;
		int32 Count = 0;
	};

	// Key: mesh path (+ material hash if include_materials)
	TMap<FString, FInstanceGroup> Groups;

	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		AStaticMeshActor* SMA = *It;
		UStaticMeshComponent* SMC = SMA->GetStaticMeshComponent();
		if (!SMC) continue;

		// Skip existing ISM/HISM
		if (Cast<UInstancedStaticMeshComponent>(SMC)) continue;

		UStaticMesh* Mesh = SMC->GetStaticMesh();
		if (!Mesh) continue;

		// Region filter
		if (bHasRegion)
		{
			FVector Loc = SMA->GetActorLocation();
			if (Loc.X < RegionMin.X || Loc.Y < RegionMin.Y || Loc.Z < RegionMin.Z ||
				Loc.X > RegionMax.X || Loc.Y > RegionMax.Y || Loc.Z > RegionMax.Z)
			{
				continue;
			}
		}

		FString MeshPath = Mesh->GetPathName();
		FString GroupKey = MeshPath;

		TArray<FString> MatPaths;
		if (bIncludeMaterials)
		{
			for (int32 i = 0; i < SMC->GetNumMaterials(); ++i)
			{
				UMaterialInterface* Mat = SMC->GetMaterial(i);
				MatPaths.Add(Mat ? Mat->GetPathName() : TEXT("None"));
			}
			// Append material hash to key
			FString MatKey;
			for (const FString& MP : MatPaths)
			{
				MatKey += MP + TEXT("|");
			}
			GroupKey += TEXT("::") + MatKey;
		}

		FInstanceGroup& Group = Groups.FindOrAdd(GroupKey);
		if (Group.Count == 0)
		{
			Group.MeshPath = MeshPath;
			Group.MaterialPaths = MatPaths;
		}
		Group.ActorNames.Add(SMA->GetActorNameOrLabel());
		Group.Count++;
	}

	// Filter and sort by count
	TArray<FInstanceGroup> Candidates;
	for (auto& Pair : Groups)
	{
		if (Pair.Value.Count >= MinCount)
		{
			Candidates.Add(MoveTemp(Pair.Value));
		}
	}
	Candidates.Sort([](const FInstanceGroup& A, const FInstanceGroup& B) { return A.Count > B.Count; });

	int32 TotalSavings = 0;
	TArray<TSharedPtr<FJsonValue>> CandidateArr;
	for (const FInstanceGroup& Group : Candidates)
	{
		auto CandObj = MakeShared<FJsonObject>();
		CandObj->SetStringField(TEXT("mesh"), Group.MeshPath);
		CandObj->SetNumberField(TEXT("count"), Group.Count);
		// HISM reduces N draw calls to ~1 (or a few for LOD transitions)
		int32 Savings = Group.Count - 1;
		CandObj->SetNumberField(TEXT("estimated_draw_call_savings"), Savings);
		TotalSavings += Savings;

		if (bIncludeMaterials && Group.MaterialPaths.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> MatArr;
			for (const FString& MP : Group.MaterialPaths)
			{
				MatArr.Add(MakeShared<FJsonValueString>(MP));
			}
			CandObj->SetArrayField(TEXT("materials"), MatArr);
		}

		TArray<TSharedPtr<FJsonValue>> ActorArr;
		for (const FString& AN : Group.ActorNames)
		{
			ActorArr.Add(MakeShared<FJsonValueString>(AN));
		}
		CandObj->SetArrayField(TEXT("actors"), ActorArr);

		CandidateArr.Add(MakeShared<FJsonValueObject>(CandObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("candidates"), CandidateArr);
	Result->SetNumberField(TEXT("total_potential_savings"), TotalSavings);
	Result->SetNumberField(TEXT("groups_found"), Candidates.Num());

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 8. convert_to_hism
// ============================================================================

FMonolithActionResult FMonolithMeshLevelDesignActions::ConvertToHism(const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (!Params->TryGetStringField(TEXT("mesh"), MeshPath))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: mesh"));
	}

	const TArray<TSharedPtr<FJsonValue>>* ActorsArr;
	if (!Params->TryGetArrayField(TEXT("actors"), ActorsArr) || ActorsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: actors"));
	}

	bool bPreserveMaterials = true;
	Params->TryGetBoolField(TEXT("preserve_materials"), bPreserveMaterials);

	UStaticMesh* Mesh = FMonolithAssetUtils::LoadAssetByPath<UStaticMesh>(MeshPath);
	if (!Mesh)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Static mesh not found: %s"), *MeshPath));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Validate ALL actors exist before starting
	TArray<AActor*> SourceActors;
	for (const auto& Val : *ActorsArr)
	{
		FString Name = Val->AsString();
		FString Error;
		AActor* Actor = MonolithMeshUtils::FindActorByName(Name, Error);
		if (!Actor)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Actor not found: %s"), *Name));
		}
		SourceActors.Add(Actor);
	}

	// Collect transforms
	TArray<FTransform> Transforms;
	Transforms.Reserve(SourceActors.Num());
	for (AActor* Actor : SourceActors)
	{
		Transforms.Add(Actor->GetActorTransform());
	}

	// Single undo transaction wrapping create + destroy
	LevelDesignHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Convert to HISM")));

	// Spawn a plain actor to host the HISM component
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Use the average position as the HISM actor origin
	FVector AvgLocation = FVector::ZeroVector;
	for (const FTransform& T : Transforms)
	{
		AvgLocation += T.GetLocation();
	}
	AvgLocation /= Transforms.Num();

	FRotator DefaultRot = FRotator::ZeroRotator;
	AActor* HISMActor = World->SpawnActor(AActor::StaticClass(), &AvgLocation, &DefaultRot, SpawnParams);
	if (!HISMActor)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(TEXT("Failed to spawn HISM host actor"));
	}

	HISMActor->Modify();

	// Add root scene component
	USceneComponent* RootComp = NewObject<USceneComponent>(HISMActor, USceneComponent::StaticClass(), TEXT("RootComponent"), RF_Transactional);
	HISMActor->SetRootComponent(RootComp);
	HISMActor->AddInstanceComponent(RootComp);
	RootComp->RegisterComponent();
	RootComp->SetWorldLocation(AvgLocation);

	// Create HISM component
	UHierarchicalInstancedStaticMeshComponent* HISM = NewObject<UHierarchicalInstancedStaticMeshComponent>(HISMActor, UHierarchicalInstancedStaticMeshComponent::StaticClass(), TEXT("HISMComponent"), RF_Transactional);
	HISM->SetupAttachment(RootComp);
	HISMActor->AddInstanceComponent(HISM);
	HISM->RegisterComponent();
	HISM->SetStaticMesh(Mesh);
	HISM->SetMobility(EComponentMobility::Static);

	// Copy material overrides from first source actor
	if (bPreserveMaterials && SourceActors.Num() > 0)
	{
		UStaticMeshComponent* FirstSMC = SourceActors[0]->FindComponentByClass<UStaticMeshComponent>();
		if (FirstSMC)
		{
			for (int32 i = 0; i < FirstSMC->GetNumMaterials(); ++i)
			{
				UMaterialInterface* Mat = FirstSMC->GetMaterial(i);
				if (Mat)
				{
					HISM->SetMaterial(i, Mat);
				}
			}
		}
	}

	// Convert transforms to local space relative to the HISM actor
	FTransform HISMWorldTransform = HISMActor->GetActorTransform();
	TArray<FTransform> LocalTransforms;
	LocalTransforms.Reserve(Transforms.Num());
	for (const FTransform& WorldTransform : Transforms)
	{
		LocalTransforms.Add(WorldTransform.GetRelativeTransform(HISMWorldTransform));
	}

	// Batch add instances — AddInstances(Transforms, bShouldReturnIndices, bWorldSpace=false)
	HISM->AddInstances(LocalTransforms, false);

	// Name and folder
	FString OptionalName;
	if (Params->TryGetStringField(TEXT("name"), OptionalName) && !OptionalName.IsEmpty())
	{
		HISMActor->SetActorLabel(OptionalName);
	}
	else
	{
		HISMActor->SetActorLabel(FString::Printf(TEXT("HISM_%s"), *FMonolithAssetUtils::GetAssetName(MeshPath)));
	}

	FString Folder;
	if (Params->TryGetStringField(TEXT("folder"), Folder) && !Folder.IsEmpty())
	{
		HISMActor->SetFolderPath(FName(*Folder));
	}

	// Persistence sanity-check before destroying sources.
	if (HISM->GetInstanceCount() != Transforms.Num())
	{
		Transaction.Cancel();
		World->DestroyActor(HISMActor);
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("HISM persistence guard failed: expected %d instances, got %d. Source actors preserved."),
			Transforms.Num(), HISM->GetInstanceCount()));
	}

	// Delete original actors
	TArray<TSharedPtr<FJsonValue>> RemovedArr;
	for (AActor* Actor : SourceActors)
	{
		RemovedArr.Add(MakeShared<FJsonValueString>(Actor->GetActorNameOrLabel()));
		Actor->Modify();
		World->DestroyActor(Actor);
	}

	HISMActor->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("hism_actor"), HISMActor->GetActorNameOrLabel());
	Result->SetStringField(TEXT("mesh"), MeshPath);
	Result->SetNumberField(TEXT("instance_count"), Transforms.Num());
	Result->SetArrayField(TEXT("actors_removed"), RemovedArr);
	Result->SetNumberField(TEXT("draw_call_savings"), FMath::Max(0, Transforms.Num() - 1));

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 9. get_actor_component_properties
// ============================================================================

FMonolithActionResult FMonolithMeshLevelDesignActions::GetActorComponentProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: actor_name"));
	}

	FString Error;
	AActor* Actor = MonolithMeshUtils::FindActorByName(ActorName, Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Find target component
	UActorComponent* TargetComp = nullptr;
	FString ComponentName;
	FString ComponentClass;
	Params->TryGetStringField(TEXT("component_name"), ComponentName);
	Params->TryGetStringField(TEXT("component_class"), ComponentClass);

	if (!ComponentName.IsEmpty())
	{
		TArray<UActorComponent*> AllComps;
		Actor->GetComponents(AllComps);
		for (UActorComponent* Comp : AllComps)
		{
			if (Comp->GetFName().ToString() == ComponentName)
			{
				TargetComp = Comp;
				break;
			}
		}
		if (!TargetComp)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Component '%s' not found on actor '%s'"), *ComponentName, *ActorName));
		}
	}
	else if (!ComponentClass.IsEmpty())
	{
		TArray<UActorComponent*> AllComps;
		Actor->GetComponents(AllComps);
		for (UActorComponent* Comp : AllComps)
		{
			if (Comp->GetClass()->GetName() == ComponentClass || Comp->GetClass()->GetName() == (TEXT("U") + ComponentClass))
			{
				TargetComp = Comp;
				break;
			}
		}
		if (!TargetComp)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("No component of class '%s' found on actor '%s'"), *ComponentClass, *ActorName));
		}
	}
	else
	{
		// Default to root component
		TargetComp = Actor->GetRootComponent();
		if (!TargetComp)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Actor '%s' has no root component"), *ActorName));
		}
	}

	// Optional: specific property names
	TSet<FString> PropertyFilter;
	const TArray<TSharedPtr<FJsonValue>>* PropsArr;
	if (Params->TryGetArrayField(TEXT("properties"), PropsArr))
	{
		for (const auto& Val : *PropsArr)
		{
			PropertyFilter.Add(Val->AsString());
		}
	}

	// Read properties via FProperty reflection
	auto PropsObj = MakeShared<FJsonObject>();
	UClass* CompClass = TargetComp->GetClass();
	int32 PropCount = 0;
	const int32 MaxProps = 200; // Safety cap

	for (TFieldIterator<FProperty> PropIt(CompClass); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;

		// If filter specified, only include those
		if (PropertyFilter.Num() > 0)
		{
			if (!PropertyFilter.Contains(Prop->GetName()))
			{
				continue;
			}
		}
		else
		{
			// Without filter, only include visible (edit/visible) properties
			if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
			{
				continue;
			}
		}

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(TargetComp);
		TSharedPtr<FJsonValue> JsonVal = LevelDesignHelpers::PropertyToJson(Prop, ValuePtr);
		if (JsonVal.IsValid())
		{
			PropsObj->SetField(Prop->GetName(), JsonVal);
			PropCount++;
		}

		if (PropCount >= MaxProps) break;
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	Result->SetStringField(TEXT("component_name"), TargetComp->GetFName().ToString());
	Result->SetStringField(TEXT("component_class"), CompClass->GetName());
	Result->SetNumberField(TEXT("property_count"), PropCount);
	Result->SetObjectField(TEXT("properties"), PropsObj);

	return FMonolithActionResult::Success(Result);
}
