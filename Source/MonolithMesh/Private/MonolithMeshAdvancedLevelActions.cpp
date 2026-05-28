#include "MonolithMeshAdvancedLevelActions.h"
#include "MonolithMeshUtils.h"
#include "MonolithMeshAnalysis.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingDynamic.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorLevelUtils.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "LevelInstance/LevelInstanceTypes.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "UObject/UnrealType.h"
#include "UObject/SavePackage.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"

// ============================================================================
// Helpers
// ============================================================================

namespace AdvancedLevelHelpers
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

	/** Resolve actors from a JSON array of names. Returns false + error if any not found. */
	bool ResolveActors(const TArray<TSharedPtr<FJsonValue>>& Names, TArray<AActor*>& OutActors, FString& OutError)
	{
		for (const auto& Val : Names)
		{
			FString Name = Val->AsString();
			if (Name.IsEmpty()) continue;

			FString Err;
			AActor* Actor = MonolithMeshUtils::FindActorByName(Name, Err);
			if (!Actor)
			{
				OutError = Err;
				return false;
			}
			OutActors.Add(Actor);
		}
		return true;
	}

	/** Parse a point from JSON: either "actor_name" string or [x,y,z] array */
	bool ParsePointOrActor(const TSharedPtr<FJsonObject>& Params, const FString& Key, FVector& OutPoint, FString& OutError)
	{
		TSharedPtr<FJsonValue> Val = Params->TryGetField(Key);
		if (!Val.IsValid())
		{
			OutError = FString::Printf(TEXT("Missing required param: %s"), *Key);
			return false;
		}

		// Array -> coordinate
		if (Val->Type == EJson::Array)
		{
			if (!MonolithMeshUtils::ParseVector(Params, Key, OutPoint))
			{
				OutError = FString::Printf(TEXT("Invalid vector for param: %s"), *Key);
				return false;
			}
			return true;
		}

		// String -> actor name
		if (Val->Type == EJson::String)
		{
			FString Name = Val->AsString();
			FString Err;
			AActor* Actor = MonolithMeshUtils::FindActorByName(Name, Err);
			if (!Actor)
			{
				OutError = Err;
				return false;
			}
			OutPoint = Actor->GetActorLocation();
			return true;
		}

		OutError = FString::Printf(TEXT("Param '%s' must be an actor name (string) or [x,y,z] array"), *Key);
		return false;
	}
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshAdvancedLevelActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 1. manage_sublevel
	Registry.RegisterAction(TEXT("mesh"), TEXT("manage_sublevel"),
		TEXT("Create/load/unload streaming sublevels or move actors between levels. sub_action: create, add, remove, move_actors."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAdvancedLevelActions::ManageSublevel),
		FParamSchemaBuilder()
			.Required(TEXT("sub_action"), TEXT("string"), TEXT("Action: create, add, remove, move_actors"))
			.Optional(TEXT("level_path"), TEXT("string"), TEXT("Level asset path (e.g. /Game/Maps/Sublevel_Basement). Required for create/add/remove."))
			.Optional(TEXT("streaming_class"), TEXT("string"), TEXT("Streaming class: LevelStreamingDynamic or LevelStreamingAlwaysLoaded"), TEXT("LevelStreamingDynamic"))
			.Optional(TEXT("actor_names"), TEXT("array"), TEXT("Actor names to move (for move_actors sub_action)"))
			.Optional(TEXT("dest_level"), TEXT("string"), TEXT("Destination level name or path (for move_actors)"))
			.Build());

	// 2. place_blueprint_actor
	Registry.RegisterAction(TEXT("mesh"), TEXT("place_blueprint_actor"),
		TEXT("Spawn a Blueprint actor in the world with optional property configuration via reflection."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAdvancedLevelActions::PlaceBlueprintActor),
		FParamSchemaBuilder()
			.Required(TEXT("blueprint"), TEXT("string"), TEXT("Blueprint asset path (e.g. /Game/Blueprints/BP_Door)"))
			.Required(TEXT("location"), TEXT("array"), TEXT("World location [x, y, z]"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("Rotation [pitch, yaw, roll]"), TEXT("[0,0,0]"))
			.Optional(TEXT("scale"), TEXT("array"), TEXT("Scale [x, y, z]"), TEXT("[1,1,1]"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("Property name->value pairs to set via reflection"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Actor label in the outliner"))
			.Optional(TEXT("folder"), TEXT("string"), TEXT("Outliner folder path"))
			.Build());

	// 3. place_spline
	Registry.RegisterAction(TEXT("mesh"), TEXT("place_spline"),
		TEXT("Spawn an actor with a spline component. Optionally places mesh segments along the spline (pipes, cables, railings)."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAdvancedLevelActions::PlaceSpline),
		FParamSchemaBuilder()
			.Required(TEXT("points"), TEXT("array"), TEXT("Array of [x,y,z] spline points (minimum 2)"))
			.Optional(TEXT("mesh_path"), TEXT("string"), TEXT("Static mesh path for spline mesh segments"))
			.Optional(TEXT("forward_axis"), TEXT("string"), TEXT("Mesh forward axis: X, Y, or Z"), TEXT("X"))
			.Optional(TEXT("point_type"), TEXT("string"), TEXT("Spline point type: Linear, Curve, Constant, CurveClamped, CurveCustomTangent"), TEXT("Curve"))
			.Optional(TEXT("scale"), TEXT("array"), TEXT("Mesh segment scale [x, y]"), TEXT("[1,1]"))
			.Optional(TEXT("close_loop"), TEXT("boolean"), TEXT("Close the spline into a loop"), TEXT("false"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Actor label"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("Actor origin location [x,y,z]"), TEXT("[0,0,0]"))
			.Build());

	// 4. create_prefab
	Registry.RegisterAction(TEXT("mesh"), TEXT("create_prefab"),
		TEXT("Create a Level Instance (prefab) from existing actors. WARNING: Source actors are MOVED into the new level. "
			 "NOTE: This action triggers a Save As dialog which blocks MCP calls. For dialog-free prefab creation, use create_blueprint_prefab instead."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAdvancedLevelActions::CreatePrefab),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Actor names to include in the prefab"))
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Save path for the level (e.g. /Game/Prefabs/PF_DoorFrame)"))
			.Optional(TEXT("type"), TEXT("string"), TEXT("Level instance type: LevelInstance or PackedLevelActor"), TEXT("LevelInstance"))
			.Build());

	// 4b. create_blueprint_prefab (dialog-free alternative)
	Registry.RegisterAction(TEXT("mesh"), TEXT("create_blueprint_prefab"),
		TEXT("Create a Blueprint prefab from existing world actors. Harvests all components into a new Actor Blueprint's SCS. "
			 "Dialog-free — safe for MCP/automation. Use place_blueprint_actor to spawn instances."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAdvancedLevelActions::CreateBlueprintPrefab),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Actor names to include in the prefab"))
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Blueprint save path (e.g. /Game/Prefabs/BP_FurnitureSet)"))
			.Optional(TEXT("center_pivot"), TEXT("boolean"), TEXT("Recenter components to group centroid"), TEXT("true"))
			.Optional(TEXT("keep_source_actors"), TEXT("boolean"), TEXT("Keep original actors (don't delete)"), TEXT("true"))
			.Build());

	// 5. spawn_prefab
	Registry.RegisterAction(TEXT("mesh"), TEXT("spawn_prefab"),
		TEXT("Spawn a Level Instance (prefab) at a location."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAdvancedLevelActions::SpawnPrefab),
		FParamSchemaBuilder()
			.Required(TEXT("prefab_path"), TEXT("string"), TEXT("Level asset path (e.g. /Game/Prefabs/PF_DoorFrame)"))
			.Required(TEXT("location"), TEXT("array"), TEXT("World location [x, y, z]"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("Rotation [pitch, yaw, roll]"), TEXT("[0,0,0]"))
			.Optional(TEXT("scale"), TEXT("array"), TEXT("Scale [x, y, z]"), TEXT("[1,1,1]"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Actor label"))
			.Build());

	// 6. randomize_transforms
	Registry.RegisterAction(TEXT("mesh"), TEXT("randomize_transforms"),
		TEXT("Apply random offset/rotation/scale variation to actors for an organic feel. Deterministic with seed."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAdvancedLevelActions::RandomizeTransforms),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Actor names to randomize"))
			.Optional(TEXT("offset_range"), TEXT("array"), TEXT("Min/max offset in cm [min, max]"), TEXT("[0,0]"))
			.Optional(TEXT("yaw_range"), TEXT("array"), TEXT("Min/max yaw rotation [min, max] degrees"), TEXT("[0,360]"))
			.Optional(TEXT("pitch_range"), TEXT("array"), TEXT("Min/max pitch [min, max] degrees"), TEXT("[0,0]"))
			.Optional(TEXT("roll_range"), TEXT("array"), TEXT("Min/max roll [min, max] degrees"), TEXT("[0,0]"))
			.Optional(TEXT("scale_range"), TEXT("array"), TEXT("Min/max uniform scale [min, max]"), TEXT("[1,1]"))
			.Optional(TEXT("seed"), TEXT("number"), TEXT("Random seed for deterministic results"), TEXT("0"))
			.Build());

	// 7. get_level_actors
	Registry.RegisterAction(TEXT("mesh"), TEXT("get_level_actors"),
		TEXT("Enumerate actors in the editor world with multi-filter AND logic. Returns name, class, location, mesh, sublevel, tags."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAdvancedLevelActions::GetLevelActors),
		FParamSchemaBuilder()
			.Optional(TEXT("class_filter"), TEXT("string"), TEXT("Filter by class name (e.g. StaticMeshActor)"))
			.Optional(TEXT("tag_filter"), TEXT("string"), TEXT("Filter by actor tag"))
			.Optional(TEXT("sublevel_filter"), TEXT("string"), TEXT("Filter by sublevel name"))
			.Optional(TEXT("mesh_wildcard"), TEXT("string"), TEXT("Wildcard filter on mesh path (e.g. *pipe*)"))
			.Optional(TEXT("name_wildcard"), TEXT("string"), TEXT("Wildcard filter on actor name/label"))
			.Optional(TEXT("volume_name"), TEXT("string"), TEXT("Only actors within this volume's bounds"))
			.Optional(TEXT("radius"), TEXT("number"), TEXT("Radius filter around center point"))
			.Optional(TEXT("center"), TEXT("array"), TEXT("Center point [x,y,z] for radius filter"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Max actors to return"), TEXT("200"))
			.Build());

	// 8. measure_distance
	Registry.RegisterAction(TEXT("mesh"), TEXT("measure_distance"),
		TEXT("Measure distance between two actors or world points. Returns euclidean, horizontal, height difference, and optional navmesh path distance."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAdvancedLevelActions::MeasureDistance),
		FParamSchemaBuilder()
			.Required(TEXT("from"), TEXT("any"), TEXT("Start: actor name (string) or [x,y,z] array"))
			.Required(TEXT("to"), TEXT("any"), TEXT("End: actor name (string) or [x,y,z] array"))
			.Optional(TEXT("include_navmesh_path"), TEXT("boolean"), TEXT("Also compute navmesh path distance"), TEXT("false"))
			.Build());
}

// ============================================================================
// 1. manage_sublevel
// ============================================================================

FMonolithActionResult FMonolithMeshAdvancedLevelActions::ManageSublevel(const TSharedPtr<FJsonObject>& Params)
{
	FString SubAction;
	if (!Params->TryGetStringField(TEXT("sub_action"), SubAction))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: sub_action"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// ── create ──
	if (SubAction.Equals(TEXT("create"), ESearchCase::IgnoreCase))
	{
		FString LevelPath;
		if (!Params->TryGetStringField(TEXT("level_path"), LevelPath))
		{
			return FMonolithActionResult::Error(TEXT("Missing required param: level_path (for create)"));
		}

		// Determine streaming class
		FString StreamingClassStr;
		Params->TryGetStringField(TEXT("streaming_class"), StreamingClassStr);

		TSubclassOf<ULevelStreaming> StreamingClass = ULevelStreamingDynamic::StaticClass();
		if (StreamingClassStr.Equals(TEXT("LevelStreamingAlwaysLoaded"), ESearchCase::IgnoreCase))
		{
			StreamingClass = ULevelStreaming::StaticClass();
		}

		AdvancedLevelHelpers::FScopedMeshTransaction Transaction(NSLOCTEXT("Monolith", "CreateSublevel", "Monolith: Create Sublevel"));

		ULevelStreaming* NewLevel = UEditorLevelUtils::CreateNewStreamingLevelForWorld(
			*World, StreamingClass, LevelPath, /*bMoveSelectedActorsIntoNewLevel=*/false);

		if (!NewLevel)
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create sublevel at: %s"), *LevelPath));
		}

		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("sub_action"), TEXT("create"));
		Result->SetStringField(TEXT("level_path"), LevelPath);
		Result->SetStringField(TEXT("package_name"), NewLevel->GetWorldAssetPackageName());
		Result->SetBoolField(TEXT("success"), true);
		return FMonolithActionResult::Success(Result);
	}

	// ── add (load existing) ──
	if (SubAction.Equals(TEXT("add"), ESearchCase::IgnoreCase))
	{
		FString LevelPath;
		if (!Params->TryGetStringField(TEXT("level_path"), LevelPath))
		{
			return FMonolithActionResult::Error(TEXT("Missing required param: level_path (for add)"));
		}

		AdvancedLevelHelpers::FScopedMeshTransaction Transaction(NSLOCTEXT("Monolith", "AddSublevel", "Monolith: Add Sublevel"));

		TSubclassOf<ULevelStreaming> AddStreamingClass = ULevelStreamingDynamic::StaticClass();
		ULevelStreaming* AddedLevel = UEditorLevelUtils::AddLevelToWorld(
			World, *LevelPath, AddStreamingClass);

		if (!AddedLevel)
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to add sublevel: %s"), *LevelPath));
		}

		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("sub_action"), TEXT("add"));
		Result->SetStringField(TEXT("level_path"), LevelPath);
		Result->SetBoolField(TEXT("loaded"), AddedLevel->HasLoadedLevel());
		return FMonolithActionResult::Success(Result);
	}

	// ── remove (unload) ──
	if (SubAction.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
	{
		FString LevelPath;
		if (!Params->TryGetStringField(TEXT("level_path"), LevelPath))
		{
			return FMonolithActionResult::Error(TEXT("Missing required param: level_path (for remove)"));
		}

		// Find the streaming level
		ULevelStreaming* FoundLevel = nullptr;
		for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
		{
			if (StreamingLevel && StreamingLevel->GetWorldAssetPackageName().Contains(LevelPath))
			{
				FoundLevel = StreamingLevel;
				break;
			}
		}

		if (!FoundLevel)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Sublevel not found: %s"), *LevelPath));
		}

		// Don't allow removing persistent level
		if (FoundLevel->GetLoadedLevel() == World->PersistentLevel)
		{
			return FMonolithActionResult::Error(TEXT("Cannot remove the persistent level"));
		}

		AdvancedLevelHelpers::FScopedMeshTransaction Transaction(NSLOCTEXT("Monolith", "RemoveSublevel", "Monolith: Remove Sublevel"));

		UEditorLevelUtils::RemoveLevelFromWorld(FoundLevel->GetLoadedLevel());

		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("sub_action"), TEXT("remove"));
		Result->SetStringField(TEXT("level_path"), LevelPath);
		Result->SetBoolField(TEXT("success"), true);
		return FMonolithActionResult::Success(Result);
	}

	// ── move_actors ──
	if (SubAction.Equals(TEXT("move_actors"), ESearchCase::IgnoreCase))
	{
		const TArray<TSharedPtr<FJsonValue>>* ActorNamesJson = nullptr;
		if (!Params->TryGetArrayField(TEXT("actor_names"), ActorNamesJson) || !ActorNamesJson || ActorNamesJson->Num() == 0)
		{
			return FMonolithActionResult::Error(TEXT("Missing or empty required param: actor_names (for move_actors)"));
		}

		FString DestLevel;
		if (!Params->TryGetStringField(TEXT("dest_level"), DestLevel))
		{
			return FMonolithActionResult::Error(TEXT("Missing required param: dest_level (for move_actors)"));
		}

		// Resolve actors
		TArray<AActor*> Actors;
		FString ResolveError;
		if (!AdvancedLevelHelpers::ResolveActors(*ActorNamesJson, Actors, ResolveError))
		{
			return FMonolithActionResult::Error(ResolveError);
		}

		// Find destination level
		ULevel* DestLevelPtr = nullptr;
		for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
		{
			if (StreamingLevel && StreamingLevel->GetLoadedLevel() &&
				StreamingLevel->GetWorldAssetPackageName().Contains(DestLevel))
			{
				DestLevelPtr = StreamingLevel->GetLoadedLevel();
				break;
			}
		}

		// Also check persistent level
		if (!DestLevelPtr && (DestLevel.Equals(TEXT("Persistent"), ESearchCase::IgnoreCase) ||
			DestLevel.Equals(TEXT("PersistentLevel"), ESearchCase::IgnoreCase)))
		{
			DestLevelPtr = World->PersistentLevel;
		}

		if (!DestLevelPtr)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Destination level not found: %s"), *DestLevel));
		}

		AdvancedLevelHelpers::FScopedMeshTransaction Transaction(NSLOCTEXT("Monolith", "MoveActorsToLevel", "Monolith: Move Actors to Level"));

		int32 MovedCount = UEditorLevelUtils::MoveActorsToLevel(Actors, DestLevelPtr);

		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("sub_action"), TEXT("move_actors"));
		Result->SetStringField(TEXT("dest_level"), DestLevel);
		Result->SetNumberField(TEXT("moved_count"), MovedCount);
		Result->SetNumberField(TEXT("requested_count"), Actors.Num());
		return FMonolithActionResult::Success(Result);
	}

	return FMonolithActionResult::Error(FString::Printf(
		TEXT("Unknown sub_action: '%s'. Valid: create, add, remove, move_actors"), *SubAction));
}

// ============================================================================
// 2. place_blueprint_actor
// ============================================================================

FMonolithActionResult FMonolithMeshAdvancedLevelActions::PlaceBlueprintActor(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintPath;
	if (!Params->TryGetStringField(TEXT("blueprint"), BlueprintPath))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: blueprint"));
	}

	FVector Location;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: location"));
	}

	FRotator Rotation(0, 0, 0);
	MonolithMeshUtils::ParseRotator(Params, TEXT("rotation"), Rotation);

	FVector Scale(1, 1, 1);
	MonolithMeshUtils::ParseVector(Params, TEXT("scale"), Scale);

	FString Label;
	Params->TryGetStringField(TEXT("label"), Label);

	FString Folder;
	Params->TryGetStringField(TEXT("folder"), Folder);

	// Normalize Blueprint path for class loading:
	// "/Game/Foo/BP_Bar" → "/Game/Foo/BP_Bar.BP_Bar_C"
	FString ClassPath = BlueprintPath;
	if (!ClassPath.Contains(TEXT(".")))
	{
		FString BaseName = FPaths::GetBaseFilename(ClassPath);
		ClassPath = ClassPath + TEXT(".") + BaseName + TEXT("_C");
	}
	else if (!ClassPath.EndsWith(TEXT("_C")))
	{
		ClassPath += TEXT("_C");
	}

	UClass* BPClass = StaticLoadClass(AActor::StaticClass(), nullptr, *ClassPath);
	if (!BPClass)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to load Blueprint class: %s (tried %s)"), *BlueprintPath, *ClassPath));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	AdvancedLevelHelpers::FScopedMeshTransaction Transaction(NSLOCTEXT("Monolith", "PlaceBPActor", "Monolith: Place Blueprint Actor"));

	FTransform SpawnTransform(Rotation.Quaternion(), Location);
	AActor* NewActor = GEditor->AddActor(World->PersistentLevel, BPClass, SpawnTransform);

	if (!NewActor)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(TEXT("Failed to spawn Blueprint actor"));
	}

	// Apply scale manually (AddActor ignores scale from FTransform)
	if (!Scale.Equals(FVector::OneVector))
	{
		NewActor->SetActorScale3D(Scale);
	}

	if (!Label.IsEmpty())
	{
		NewActor->SetActorLabel(Label);
	}
	if (!Folder.IsEmpty())
	{
		NewActor->SetFolderPath(FName(*Folder));
	}
	else
	{
		NewActor->SetFolderPath(FName(TEXT("Prefabs")));
	}

	// Set properties via reflection
	TArray<FString> PropertiesSet;
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid() && (*PropsObj)->Values.Num() > 0)
	{
		for (const auto& Pair : (*PropsObj)->Values)
		{
			const FString& PropName = Pair.Key;
			const TSharedPtr<FJsonValue>& PropValue = Pair.Value;

			// Find the property on the actor
			FProperty* Prop = NewActor->GetClass()->FindPropertyByName(FName(*PropName));
			if (!Prop)
			{
				// Try components
				bool bFoundOnComponent = false;
				TArray<UActorComponent*> Components;
				NewActor->GetComponents(Components);
				for (UActorComponent* Comp : Components)
				{
					Prop = Comp->GetClass()->FindPropertyByName(FName(*PropName));
					if (Prop)
					{
						FString ValueStr;
						if (PropValue->Type == EJson::Boolean)
						{
							ValueStr = PropValue->AsBool() ? TEXT("True") : TEXT("False");
						}
						else if (PropValue->Type == EJson::Number)
						{
							ValueStr = FString::SanitizeFloat(PropValue->AsNumber());
						}
						else
						{
							ValueStr = PropValue->AsString();
						}

						void* ContainerPtr = Comp;
						if (Prop->ImportText_Direct(*ValueStr, Prop->ContainerPtrToValuePtr<void>(ContainerPtr), Comp, PPF_None))
						{
							FPropertyChangedEvent ChangedEvent(Prop);
							Comp->PostEditChangeProperty(ChangedEvent);
							PropertiesSet.Add(PropName);
						}
						bFoundOnComponent = true;
						break;
					}
				}
				if (bFoundOnComponent) continue;

				// Property not found anywhere — skip silently, note in return
				continue;
			}

			FString ValueStr;
			if (PropValue->Type == EJson::Boolean)
			{
				ValueStr = PropValue->AsBool() ? TEXT("True") : TEXT("False");
			}
			else if (PropValue->Type == EJson::Number)
			{
				ValueStr = FString::SanitizeFloat(PropValue->AsNumber());
			}
			else
			{
				ValueStr = PropValue->AsString();
			}

			void* ContainerPtr = NewActor;
			if (Prop->ImportText_Direct(*ValueStr, Prop->ContainerPtrToValuePtr<void>(ContainerPtr), NewActor, PPF_None))
			{
				FPropertyChangedEvent ChangedEvent(Prop);
				NewActor->PostEditChangeProperty(ChangedEvent);
				PropertiesSet.Add(PropName);
			}
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), NewActor->GetFName().ToString());
	Result->SetStringField(TEXT("actor_label"), NewActor->GetActorLabel());
	Result->SetStringField(TEXT("class"), NewActor->GetClass()->GetName());
	Result->SetArrayField(TEXT("location"), AdvancedLevelHelpers::VectorToJsonArray(NewActor->GetActorLocation()));

	TArray<TSharedPtr<FJsonValue>> PropsSetArr;
	for (const FString& P : PropertiesSet)
	{
		PropsSetArr.Add(MakeShared<FJsonValueString>(P));
	}
	Result->SetArrayField(TEXT("properties_set"), PropsSetArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 3. place_spline
// ============================================================================

FMonolithActionResult FMonolithMeshAdvancedLevelActions::PlaceSpline(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* PointsJson = nullptr;
	if (!Params->TryGetArrayField(TEXT("points"), PointsJson) || !PointsJson || PointsJson->Num() < 2)
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: points (array of [x,y,z], minimum 2)"));
	}

	// Parse spline points
	TArray<FVector> Points;
	for (int32 i = 0; i < PointsJson->Num(); ++i)
	{
		const TArray<TSharedPtr<FJsonValue>>* PointArr = nullptr;
		if ((*PointsJson)[i]->TryGetArray(PointArr) && PointArr && PointArr->Num() >= 3)
		{
			Points.Add(FVector(
				(*PointArr)[0]->AsNumber(),
				(*PointArr)[1]->AsNumber(),
				(*PointArr)[2]->AsNumber()));
		}
		else
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Invalid point at index %d: expected [x, y, z]"), i));
		}
	}

	// Optional mesh
	FString MeshPath;
	Params->TryGetStringField(TEXT("mesh_path"), MeshPath);
	UStaticMesh* SplineMesh = nullptr;
	if (!MeshPath.IsEmpty())
	{
		SplineMesh = FMonolithAssetUtils::LoadAssetByPath<UStaticMesh>(MeshPath);
		if (!SplineMesh)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Mesh not found: %s"), *MeshPath));
		}
	}

	// Forward axis
	FString ForwardAxisStr;
	Params->TryGetStringField(TEXT("forward_axis"), ForwardAxisStr);
	ESplineMeshAxis::Type ForwardAxis = ESplineMeshAxis::X;
	if (ForwardAxisStr.Equals(TEXT("Y"), ESearchCase::IgnoreCase)) ForwardAxis = ESplineMeshAxis::Y;
	else if (ForwardAxisStr.Equals(TEXT("Z"), ESearchCase::IgnoreCase)) ForwardAxis = ESplineMeshAxis::Z;

	// Point type
	FString PointTypeStr;
	Params->TryGetStringField(TEXT("point_type"), PointTypeStr);
	ESplinePointType::Type PointType = ESplinePointType::Curve;
	if (PointTypeStr.Equals(TEXT("Linear"), ESearchCase::IgnoreCase)) PointType = ESplinePointType::Linear;
	else if (PointTypeStr.Equals(TEXT("Constant"), ESearchCase::IgnoreCase)) PointType = ESplinePointType::Constant;
	else if (PointTypeStr.Equals(TEXT("CurveClamped"), ESearchCase::IgnoreCase)) PointType = ESplinePointType::CurveClamped;
	else if (PointTypeStr.Equals(TEXT("CurveCustomTangent"), ESearchCase::IgnoreCase)) PointType = ESplinePointType::CurveCustomTangent;

	// Scale
	FVector2D MeshScale(1.0, 1.0);
	{
		const TArray<TSharedPtr<FJsonValue>>* ScaleArr = nullptr;
		if (Params->TryGetArrayField(TEXT("scale"), ScaleArr) && ScaleArr && ScaleArr->Num() >= 2)
		{
			MeshScale.X = (*ScaleArr)[0]->AsNumber();
			MeshScale.Y = (*ScaleArr)[1]->AsNumber();
		}
	}

	bool bCloseLoop = false;
	Params->TryGetBoolField(TEXT("close_loop"), bCloseLoop);

	FString Label;
	Params->TryGetStringField(TEXT("label"), Label);

	FVector ActorLocation(0, 0, 0);
	MonolithMeshUtils::ParseVector(Params, TEXT("location"), ActorLocation);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	AdvancedLevelHelpers::FScopedMeshTransaction Transaction(NSLOCTEXT("Monolith", "PlaceSpline", "Monolith: Place Spline"));

	// Spawn a plain AActor
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	FRotator DefaultRot = FRotator::ZeroRotator;
	AActor* SplineActor = World->SpawnActor(AActor::StaticClass(), &ActorLocation, &DefaultRot, SpawnParams);

	if (!SplineActor)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(TEXT("Failed to spawn spline actor"));
	}

	SplineActor->Modify();

	// Add a scene root component (Static so Static USplineMeshComponents can attach to the hierarchy)
	USceneComponent* RootComp = NewObject<USceneComponent>(SplineActor, USceneComponent::StaticClass(), TEXT("RootComponent"), RF_Transactional);
	RootComp->SetMobility(EComponentMobility::Static);
	RootComp->SetWorldLocation(ActorLocation);
	SplineActor->SetRootComponent(RootComp);
	SplineActor->AddInstanceComponent(RootComp);
	RootComp->RegisterComponent();

	// Add USplineComponent (Static — pipes/cables/railings are placed decoration; matches USplineMeshComponent's default Static mobility)
	USplineComponent* SplineComp = NewObject<USplineComponent>(SplineActor, USplineComponent::StaticClass(), TEXT("SplineComponent"), RF_Transactional);
	SplineComp->SetMobility(EComponentMobility::Static);
	SplineComp->SetupAttachment(RootComp);
	SplineActor->AddInstanceComponent(SplineComp);

	// Convert points to local space (relative to actor origin)
	TArray<FVector> LocalPoints;
	for (const FVector& P : Points)
	{
		LocalPoints.Add(P - ActorLocation);
	}

	SplineComp->SetSplinePoints(LocalPoints, ESplineCoordinateSpace::Local, /*bUpdateSpline=*/false);
	SplineComp->SetClosedLoop(bCloseLoop, /*bUpdateSpline=*/false);

	// Set point types
	for (int32 i = 0; i < LocalPoints.Num(); ++i)
	{
		SplineComp->SetSplinePointType(i, PointType, /*bUpdateSpline=*/false);
	}

	SplineComp->UpdateSpline();
	SplineComp->RegisterComponent();

	// Add USplineMeshComponents if a mesh is specified
	int32 SegmentCount = 0;
	float TotalLength = SplineComp->GetSplineLength();

	if (SplineMesh)
	{
		int32 NumSegments = bCloseLoop ? LocalPoints.Num() : (LocalPoints.Num() - 1);

		if (NumSegments > 50)
		{
			UE_LOG(LogMonolith, Warning, TEXT("place_spline: %d segments — each is a separate draw call. Consider reducing point count."), NumSegments);
		}

		for (int32 i = 0; i < NumSegments; ++i)
		{
			int32 NextIdx = (i + 1) % LocalPoints.Num();

			FVector StartPos, StartTangent, EndPos, EndTangent;
			SplineComp->GetLocationAndTangentAtSplinePoint(i, StartPos, StartTangent, ESplineCoordinateSpace::Local);
			SplineComp->GetLocationAndTangentAtSplinePoint(NextIdx, EndPos, EndTangent, ESplineCoordinateSpace::Local);

			FName CompName(*FString::Printf(TEXT("SplineMesh_%d"), i));
			USplineMeshComponent* SMC = NewObject<USplineMeshComponent>(SplineActor, USplineMeshComponent::StaticClass(), CompName, RF_Transactional);
			SMC->SetupAttachment(SplineComp);
			SplineActor->AddInstanceComponent(SMC);
			SMC->SetStaticMesh(SplineMesh);
			SMC->SetStartAndEnd(StartPos, StartTangent, EndPos, EndTangent, /*bUpdateMesh=*/true);
			SMC->SetForwardAxis(ForwardAxis);
			SMC->SetStartScale(MeshScale);
			SMC->SetEndScale(MeshScale);
			SMC->RegisterComponent();

			SegmentCount++;
		}
	}

	SplineActor->MarkPackageDirty();

	if (!Label.IsEmpty())
	{
		SplineActor->SetActorLabel(Label);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), SplineActor->GetFName().ToString());
	Result->SetStringField(TEXT("actor_label"), SplineActor->GetActorLabel());
	Result->SetNumberField(TEXT("segment_count"), SegmentCount);
	Result->SetNumberField(TEXT("total_length"), TotalLength);
	Result->SetNumberField(TEXT("point_count"), Points.Num());

	TArray<TSharedPtr<FJsonValue>> PointsArr;
	for (const FVector& P : Points)
	{
		PointsArr.Add(MakeShared<FJsonValueArray>(AdvancedLevelHelpers::VectorToJsonArray(P)));
	}
	Result->SetArrayField(TEXT("spline_points"), PointsArr);

	if (SegmentCount > 50)
	{
		Result->SetStringField(TEXT("warning"), FString::Printf(
			TEXT("%d spline mesh segments — each is a separate draw call. Consider reducing points."), SegmentCount));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 4. create_prefab
// ============================================================================

FMonolithActionResult FMonolithMeshAdvancedLevelActions::CreatePrefab(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* ActorNamesJson = nullptr;
	if (!Params->TryGetArrayField(TEXT("actor_names"), ActorNamesJson) || !ActorNamesJson || ActorNamesJson->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: actor_names"));
	}

	FString SavePath;
	if (!Params->TryGetStringField(TEXT("save_path"), SavePath))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: save_path"));
	}

	FString TypeStr;
	Params->TryGetStringField(TEXT("type"), TypeStr);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	ULevelInstanceSubsystem* LISubsystem = World->GetSubsystem<ULevelInstanceSubsystem>();
	if (!LISubsystem)
	{
		return FMonolithActionResult::Error(TEXT("LevelInstanceSubsystem not available — ensure Level Instance editor support is enabled"));
	}

	// Resolve actors
	TArray<AActor*> Actors;
	FString ResolveError;
	if (!AdvancedLevelHelpers::ResolveActors(*ActorNamesJson, Actors, ResolveError))
	{
		return FMonolithActionResult::Error(ResolveError);
	}

	AdvancedLevelHelpers::FScopedMeshTransaction Transaction(NSLOCTEXT("Monolith", "CreatePrefab", "Monolith: Create Prefab"));

	// Determine type
	ELevelInstanceCreationType CreationType = ELevelInstanceCreationType::LevelInstance;
	if (TypeStr.Equals(TEXT("PackedLevelActor"), ESearchCase::IgnoreCase) ||
		TypeStr.Equals(TEXT("packed"), ESearchCase::IgnoreCase))
	{
		CreationType = ELevelInstanceCreationType::PackedLevelActor;
	}

	FNewLevelInstanceParams CreationParams;
	CreationParams.Type = CreationType;
	CreationParams.LevelPackageName = SavePath;
	CreationParams.bAlwaysShowDialog = false;
	CreationParams.bPromptForSave = false;

	// Use pivot center of all actors
	FVector PivotCenter = FVector::ZeroVector;
	for (AActor* A : Actors)
	{
		PivotCenter += A->GetActorLocation();
	}
	PivotCenter /= Actors.Num();
	CreationParams.PivotType = ELevelInstancePivotType::CenterMinZ;

	ILevelInstanceInterface* LevelInstance = LISubsystem->CreateLevelInstanceFrom(Actors, CreationParams);

	if (!LevelInstance)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(TEXT("Failed to create Level Instance. The engine's CreateLevelInstanceFrom triggers a Save As dialog (bUseSaveAs=true) which blocks MCP calls. Use spawn_prefab with an existing prefab instead, or create Level Instances manually in the editor."));
	}

	AActor* LIActor = Cast<AActor>(LevelInstance);
	if (!LIActor)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(TEXT("Level Instance created but could not be cast to AActor"));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), LIActor->GetFName().ToString());
	Result->SetStringField(TEXT("actor_label"), LIActor->GetActorLabel());
	Result->SetStringField(TEXT("level_path"), SavePath);
	Result->SetStringField(TEXT("type"), TypeStr.IsEmpty() ? TEXT("LevelInstance") : TypeStr);
	Result->SetNumberField(TEXT("source_actor_count"), Actors.Num());
	Result->SetStringField(TEXT("warning"), TEXT("Source actors have been MOVED into the Level Instance level. They no longer exist in the original level."));

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 5. spawn_prefab
// ============================================================================

FMonolithActionResult FMonolithMeshAdvancedLevelActions::SpawnPrefab(const TSharedPtr<FJsonObject>& Params)
{
	FString PrefabPath;
	if (!Params->TryGetStringField(TEXT("prefab_path"), PrefabPath))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: prefab_path"));
	}

	FVector Location;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: location"));
	}

	FRotator Rotation(0, 0, 0);
	MonolithMeshUtils::ParseRotator(Params, TEXT("rotation"), Rotation);

	FVector Scale(1, 1, 1);
	MonolithMeshUtils::ParseVector(Params, TEXT("scale"), Scale);

	FString Label;
	Params->TryGetStringField(TEXT("label"), Label);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Verify the level asset exists
	FString LongPackageName;
	if (!FPackageName::TryConvertFilenameToLongPackageName(PrefabPath, LongPackageName))
	{
		LongPackageName = PrefabPath; // Already a package path
	}

	AdvancedLevelHelpers::FScopedMeshTransaction Transaction(NSLOCTEXT("Monolith", "SpawnPrefab", "Monolith: Spawn Prefab"));

	// Spawn a LevelInstance actor via GEditor->AddActor
	UClass* LIClass = ALevelInstance::StaticClass();
	FTransform SpawnTransform(Rotation, Location, Scale);
	AActor* NewActor = GEditor->AddActor(World->PersistentLevel, LIClass, SpawnTransform);

	if (!NewActor)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(TEXT("Failed to spawn LevelInstance actor"));
	}

	// Set the world asset on the level instance
	ALevelInstance* LI = Cast<ALevelInstance>(NewActor);
	if (LI)
	{
		// Set world asset via property reflection (avoids TSoftObjectPtr construction issues)
		if (FProperty* WorldProp = LI->GetClass()->FindPropertyByName(TEXT("WorldAsset")))
		{
			FString ImportValue = PrefabPath;
			WorldProp->ImportText_Direct(*ImportValue, WorldProp->ContainerPtrToValuePtr<void>(LI), LI, PPF_None);
			FPropertyChangedEvent ChangeEvent(WorldProp);
			LI->PostEditChangeProperty(ChangeEvent);
		}
	}

	if (!Label.IsEmpty())
	{
		NewActor->SetActorLabel(Label);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), NewActor->GetFName().ToString());
	Result->SetStringField(TEXT("actor_label"), NewActor->GetActorLabel());
	Result->SetStringField(TEXT("prefab_path"), PrefabPath);
	Result->SetArrayField(TEXT("location"), AdvancedLevelHelpers::VectorToJsonArray(Location));

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 6. randomize_transforms
// ============================================================================

FMonolithActionResult FMonolithMeshAdvancedLevelActions::RandomizeTransforms(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* ActorNamesJson = nullptr;
	if (!Params->TryGetArrayField(TEXT("actor_names"), ActorNamesJson) || !ActorNamesJson || ActorNamesJson->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: actor_names"));
	}

	// Parse ranges (all [min, max] arrays)
	auto ParseRange = [&Params](const FString& Key, double DefaultMin, double DefaultMax, double& OutMin, double& OutMax)
	{
		OutMin = DefaultMin;
		OutMax = DefaultMax;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params->TryGetArrayField(Key, Arr) && Arr && Arr->Num() >= 2)
		{
			OutMin = (*Arr)[0]->AsNumber();
			OutMax = (*Arr)[1]->AsNumber();
		}
	};

	double OffsetMin, OffsetMax;
	ParseRange(TEXT("offset_range"), 0, 0, OffsetMin, OffsetMax);

	double YawMin, YawMax;
	ParseRange(TEXT("yaw_range"), 0, 360, YawMin, YawMax);

	double PitchMin, PitchMax;
	ParseRange(TEXT("pitch_range"), 0, 0, PitchMin, PitchMax);

	double RollMin, RollMax;
	ParseRange(TEXT("roll_range"), 0, 0, RollMin, RollMax);

	double ScaleMin, ScaleMax;
	ParseRange(TEXT("scale_range"), 1, 1, ScaleMin, ScaleMax);

	int32 Seed = 0;
	{
		double SeedD;
		if (Params->TryGetNumberField(TEXT("seed"), SeedD))
		{
			Seed = static_cast<int32>(SeedD);
		}
	}

	// Resolve actors
	TArray<AActor*> Actors;
	FString ResolveError;
	if (!AdvancedLevelHelpers::ResolveActors(*ActorNamesJson, Actors, ResolveError))
	{
		return FMonolithActionResult::Error(ResolveError);
	}

	AdvancedLevelHelpers::FScopedMeshTransaction Transaction(NSLOCTEXT("Monolith", "RandomizeTransforms", "Monolith: Randomize Transforms"));

	FRandomStream Rng(Seed);
	TArray<TSharedPtr<FJsonValue>> ActorResults;

	for (AActor* Actor : Actors)
	{
		auto ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("name"), Actor->GetActorLabel());

		FVector OrigLoc = Actor->GetActorLocation();
		FRotator OrigRot = Actor->GetActorRotation();
		FVector OrigScale = Actor->GetActorScale3D();

		// Offset: random direction on XY plane, random magnitude
		if (OffsetMax > 0)
		{
			double Mag = Rng.FRandRange(OffsetMin, OffsetMax);
			double Angle = Rng.FRandRange(0.0, 360.0);
			FVector Offset(
				Mag * FMath::Cos(FMath::DegreesToRadians(Angle)),
				Mag * FMath::Sin(FMath::DegreesToRadians(Angle)),
				0.0);
			Actor->SetActorLocation(OrigLoc + Offset);
		}

		// Rotation
		if (YawMax > YawMin || PitchMax > PitchMin || RollMax > RollMin)
		{
			float NewYaw = OrigRot.Yaw + Rng.FRandRange(YawMin, YawMax);
			float NewPitch = OrigRot.Pitch + Rng.FRandRange(PitchMin, PitchMax);
			float NewRoll = OrigRot.Roll + Rng.FRandRange(RollMin, RollMax);
			Actor->SetActorRotation(FRotator(NewPitch, NewYaw, NewRoll));
		}

		// Scale
		if (ScaleMin != 1.0 || ScaleMax != 1.0)
		{
			float S = Rng.FRandRange(ScaleMin, ScaleMax);
			Actor->SetActorScale3D(OrigScale * S);
		}

		ActorObj->SetArrayField(TEXT("original_location"), AdvancedLevelHelpers::VectorToJsonArray(OrigLoc));
		ActorObj->SetArrayField(TEXT("new_location"), AdvancedLevelHelpers::VectorToJsonArray(Actor->GetActorLocation()));
		ActorObj->SetNumberField(TEXT("new_yaw"), Actor->GetActorRotation().Yaw);
		ActorObj->SetNumberField(TEXT("new_scale"), Actor->GetActorScale3D().X);
		ActorResults.Add(MakeShared<FJsonValueObject>(ActorObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("modified_count"), Actors.Num());
	Result->SetNumberField(TEXT("seed"), Seed);
	Result->SetArrayField(TEXT("actors"), ActorResults);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 7. get_level_actors
// ============================================================================

FMonolithActionResult FMonolithMeshAdvancedLevelActions::GetLevelActors(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Parse filters
	FString ClassFilter, TagFilter, SublevelFilter, MeshWildcard, NameWildcard, VolumeName;
	Params->TryGetStringField(TEXT("class_filter"), ClassFilter);
	Params->TryGetStringField(TEXT("tag_filter"), TagFilter);
	Params->TryGetStringField(TEXT("sublevel_filter"), SublevelFilter);
	Params->TryGetStringField(TEXT("mesh_wildcard"), MeshWildcard);
	Params->TryGetStringField(TEXT("name_wildcard"), NameWildcard);
	Params->TryGetStringField(TEXT("volume_name"), VolumeName);

	double Radius = 0.0;
	Params->TryGetNumberField(TEXT("radius"), Radius);

	FVector Center(0, 0, 0);
	MonolithMeshUtils::ParseVector(Params, TEXT("center"), Center);

	int32 Limit = 200;
	{
		double LimitD;
		if (Params->TryGetNumberField(TEXT("limit"), LimitD))
		{
			Limit = FMath::Clamp(static_cast<int32>(LimitD), 1, 10000);
		}
	}

	// Resolve volume bounds if specified
	FBox VolumeBounds(ForceInit);
	if (!VolumeName.IsEmpty())
	{
		FString Err;
		AActor* VolumeActor = MonolithMeshUtils::FindActorByName(VolumeName, Err);
		if (!VolumeActor)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Volume actor not found: %s"), *VolumeName));
		}
		FVector Origin, Extent;
		VolumeActor->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent);
		VolumeBounds = FBox(Origin - Extent, Origin + Extent);
	}

	TArray<TSharedPtr<FJsonValue>> ActorsArr;
	int32 Count = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (Count >= Limit) break;

		AActor* Actor = *It;
		if (!IsValid(Actor)) continue;

		// Class filter
		if (!ClassFilter.IsEmpty())
		{
			bool bClassMatch = false;
			for (UClass* C = Actor->GetClass(); C; C = C->GetSuperClass())
			{
				if (C->GetName().Equals(ClassFilter, ESearchCase::IgnoreCase) ||
					C->GetName().Equals(TEXT("A") + ClassFilter, ESearchCase::IgnoreCase))
				{
					bClassMatch = true;
					break;
				}
			}
			if (!bClassMatch) continue;
		}

		// Tag filter
		if (!TagFilter.IsEmpty())
		{
			bool bHasTag = false;
			for (const FName& Tag : Actor->Tags)
			{
				if (Tag.ToString().Equals(TagFilter, ESearchCase::IgnoreCase))
				{
					bHasTag = true;
					break;
				}
			}
			if (!bHasTag) continue;
		}

		// Sublevel filter
		if (!SublevelFilter.IsEmpty())
		{
			ULevel* ActorLevel = Actor->GetLevel();
			if (!ActorLevel) continue;
			FString LevelName = ActorLevel->GetOutermost()->GetName();
			if (!LevelName.Contains(SublevelFilter))
			{
				continue;
			}
		}

		// Mesh wildcard
		if (!MeshWildcard.IsEmpty())
		{
			UStaticMeshComponent* SMC = Actor->FindComponentByClass<UStaticMeshComponent>();
			if (!SMC || !SMC->GetStaticMesh()) continue;
			FString MeshName = SMC->GetStaticMesh()->GetPathName();
			if (!MeshName.MatchesWildcard(MeshWildcard, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		// Name wildcard
		if (!NameWildcard.IsEmpty())
		{
			FString ActorLabel = Actor->GetActorLabel();
			FString ActorFName = Actor->GetFName().ToString();
			if (!ActorLabel.MatchesWildcard(NameWildcard, ESearchCase::IgnoreCase) &&
				!ActorFName.MatchesWildcard(NameWildcard, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		// Volume bounds filter
		if (VolumeBounds.IsValid)
		{
			if (!VolumeBounds.IsInsideOrOn(Actor->GetActorLocation()))
			{
				continue;
			}
		}

		// Radius filter
		if (Radius > 0)
		{
			if (FVector::Dist(Actor->GetActorLocation(), Center) > Radius)
			{
				continue;
			}
		}

		// Build actor info
		auto ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("name"), Actor->GetFName().ToString());
		ActorObj->SetStringField(TEXT("label"), Actor->GetActorLabel());
		ActorObj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
		ActorObj->SetArrayField(TEXT("location"), AdvancedLevelHelpers::VectorToJsonArray(Actor->GetActorLocation()));

		// Mesh
		UStaticMeshComponent* SMC = Actor->FindComponentByClass<UStaticMeshComponent>();
		if (SMC && SMC->GetStaticMesh())
		{
			ActorObj->SetStringField(TEXT("mesh"), SMC->GetStaticMesh()->GetPathName());
		}

		// Sublevel
		ULevel* ActorLevel = Actor->GetLevel();
		if (ActorLevel)
		{
			ActorObj->SetStringField(TEXT("sublevel"), ActorLevel->GetOutermost()->GetName());
		}

		// Tags
		if (Actor->Tags.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> TagArr;
			for (const FName& Tag : Actor->Tags)
			{
				TagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
			}
			ActorObj->SetArrayField(TEXT("tags"), TagArr);
		}

		ActorsArr.Add(MakeShared<FJsonValueObject>(ActorObj));
		Count++;
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Count);
	Result->SetArrayField(TEXT("actors"), ActorsArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 8. measure_distance
// ============================================================================

FMonolithActionResult FMonolithMeshAdvancedLevelActions::MeasureDistance(const TSharedPtr<FJsonObject>& Params)
{
	FString FromError, ToError;
	FVector FromPoint, ToPoint;

	if (!AdvancedLevelHelpers::ParsePointOrActor(Params, TEXT("from"), FromPoint, FromError))
	{
		return FMonolithActionResult::Error(FromError);
	}
	if (!AdvancedLevelHelpers::ParsePointOrActor(Params, TEXT("to"), ToPoint, ToError))
	{
		return FMonolithActionResult::Error(ToError);
	}

	double EuclideanDist = FVector::Dist(FromPoint, ToPoint);
	double HorizontalDist = FVector::Dist2D(FromPoint, ToPoint);
	double HeightDiff = ToPoint.Z - FromPoint.Z;

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("euclidean_distance"), EuclideanDist);
	Result->SetNumberField(TEXT("horizontal_distance"), HorizontalDist);
	Result->SetNumberField(TEXT("height_difference"), HeightDiff);
	Result->SetArrayField(TEXT("from"), AdvancedLevelHelpers::VectorToJsonArray(FromPoint));
	Result->SetArrayField(TEXT("to"), AdvancedLevelHelpers::VectorToJsonArray(ToPoint));

	// Navmesh path distance (optional)
	bool bIncludeNavmesh = false;
	Params->TryGetBoolField(TEXT("include_navmesh_path"), bIncludeNavmesh);

	if (bIncludeNavmesh)
	{
		UWorld* World = MonolithMeshUtils::GetEditorWorld();
		if (World)
		{
			TArray<FVector> PathPoints;
			float PathDist = 0.0f;
			if (MonolithMeshAnalysis::FindNavPath(World, FromPoint, ToPoint, PathPoints, PathDist))
			{
				Result->SetNumberField(TEXT("navmesh_distance"), PathDist);
				Result->SetBoolField(TEXT("navmesh_path_found"), true);
				Result->SetNumberField(TEXT("navmesh_path_points"), PathPoints.Num());
			}
			else
			{
				Result->SetBoolField(TEXT("navmesh_path_found"), false);
				Result->SetStringField(TEXT("navmesh_note"), TEXT("No navmesh path found. Build navmesh first or check connectivity."));
			}
		}
		else
		{
			Result->SetBoolField(TEXT("navmesh_path_found"), false);
			Result->SetStringField(TEXT("navmesh_note"), TEXT("No editor world available for navmesh query"));
		}
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 9. create_blueprint_prefab (dialog-free)
// ============================================================================

FMonolithActionResult FMonolithMeshAdvancedLevelActions::CreateBlueprintPrefab(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* ActorNamesJson = nullptr;
	if (!Params->TryGetArrayField(TEXT("actor_names"), ActorNamesJson) || !ActorNamesJson || ActorNamesJson->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: actor_names"));
	}

	FString SavePath;
	if (!Params->TryGetStringField(TEXT("save_path"), SavePath))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: save_path"));
	}

	bool bCenterPivot = true;
	Params->TryGetBoolField(TEXT("center_pivot"), bCenterPivot);

	bool bKeepSourceActors = true;
	Params->TryGetBoolField(TEXT("keep_source_actors"), bKeepSourceActors);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Resolve actors
	TArray<AActor*> Actors;
	FString ResolveError;
	if (!AdvancedLevelHelpers::ResolveActors(*ActorNamesJson, Actors, ResolveError))
	{
		return FMonolithActionResult::Error(ResolveError);
	}

	if (Actors.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No valid actors resolved from actor_names"));
	}

	AdvancedLevelHelpers::FScopedMeshTransaction Transaction(
		NSLOCTEXT("Monolith", "CreateBlueprintPrefab", "Monolith: Create Blueprint Prefab"));

	// Create package (no dialog)
	UPackage* Package = CreatePackage(*SavePath);
	if (!Package)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at: %s"), *SavePath));
	}
	Package->FullyLoad();

	FString AssetName = FPaths::GetBaseFilename(SavePath);

	// Configure harvest params — dialog-free overload
	FKismetEditorUtilities::FHarvestBlueprintFromActorsParams HarvestParams;
	HarvestParams.bReplaceActors = !bKeepSourceActors;
	HarvestParams.bOpenBlueprint = false;
	HarvestParams.ParentClass = AActor::StaticClass();

	// Harvest — FName/UPackage* overload is completely dialog-free
	UBlueprint* BP = FKismetEditorUtilities::HarvestBlueprintFromActors(
		FName(*AssetName), Package, Actors, HarvestParams);

	if (!BP)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(TEXT("HarvestBlueprintFromActors failed — could not create Blueprint from the given actors"));
	}

	// Center pivot: offset all SCS component templates by -centroid
	int32 ComponentCount = 0;
	if (bCenterPivot && BP->SimpleConstructionScript)
	{
		// Compute centroid from actor world locations
		FVector Centroid = FVector::ZeroVector;
		for (AActor* A : Actors)
		{
			Centroid += A->GetActorLocation();
		}
		Centroid /= Actors.Num();

		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		ComponentCount = SCS->GetAllNodes().Num();

		// Only offset root-level nodes — child nodes already have correct
		// relative transforms to their parent component
		const TArray<USCS_Node*>& RootNodes = SCS->GetRootNodes();
		for (USCS_Node* Node : RootNodes)
		{
			if (!Node || !Node->ComponentTemplate) continue;

			USceneComponent* Template = Cast<USceneComponent>(Node->ComponentTemplate);
			if (Template)
			{
				Template->SetRelativeLocation_Direct(
					Template->GetRelativeLocation() - Centroid);
			}
		}
	}
	else if (BP->SimpleConstructionScript)
	{
		ComponentCount = BP->SimpleConstructionScript->GetAllNodes().Num();
	}

	// Compile the blueprint
	FKismetEditorUtilities::CompileBlueprint(BP);

	// Register with asset registry and save
	FAssetRegistryModule::AssetCreated(BP);
	Package->MarkPackageDirty();

	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		SavePath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Package, BP, *PackageFilename, SaveArgs);

	// Build result
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint_path"), SavePath);
	Result->SetStringField(TEXT("asset_name"), AssetName);
	Result->SetNumberField(TEXT("source_actor_count"), Actors.Num());
	Result->SetNumberField(TEXT("component_count"), ComponentCount);
	Result->SetBoolField(TEXT("center_pivot"), bCenterPivot);
	Result->SetBoolField(TEXT("keep_source_actors"), bKeepSourceActors);
	Result->SetStringField(TEXT("spawn_with"), TEXT("mesh_query(\"place_blueprint_actor\", {blueprint: \"") + SavePath + TEXT("\", location: [x,y,z]})"));

	return FMonolithActionResult::Success(Result);
}
