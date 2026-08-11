#include "MonolithMeshSpatialRegistry.h"
#include "Containers/Queue.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogSpatialRegistry, Log, All);

// ============================================================================
// FSpatialBlock JSON serialization
// ============================================================================

TSharedPtr<FJsonObject> FSpatialBlock::ToJson() const
{
	auto J = MakeShared<FJsonObject>();
	J->SetStringField(TEXT("block_id"), BlockId);
	J->SetNumberField(TEXT("version"), 1);

	// Buildings
	TArray<TSharedPtr<FJsonValue>> BuildingsArr;
	for (const auto& [Id, B] : Buildings)
	{
		auto BJ = MakeShared<FJsonObject>();
		BJ->SetStringField(TEXT("building_id"), B.BuildingId);
		BJ->SetStringField(TEXT("asset_path"), B.AssetPath);

		TArray<TSharedPtr<FJsonValue>> OriginArr;
		OriginArr.Add(MakeShared<FJsonValueNumber>(B.WorldOrigin.X));
		OriginArr.Add(MakeShared<FJsonValueNumber>(B.WorldOrigin.Y));
		OriginArr.Add(MakeShared<FJsonValueNumber>(B.WorldOrigin.Z));
		BJ->SetArrayField(TEXT("world_origin"), OriginArr);

		TArray<TSharedPtr<FJsonValue>> FootArr;
		for (const FVector2D& V : B.FootprintPolygon)
		{
			TArray<TSharedPtr<FJsonValue>> Pt;
			Pt.Add(MakeShared<FJsonValueNumber>(V.X));
			Pt.Add(MakeShared<FJsonValueNumber>(V.Y));
			FootArr.Add(MakeShared<FJsonValueArray>(Pt));
		}
		BJ->SetArrayField(TEXT("footprint_polygon"), FootArr);

		// Floor-to-room mapping
		auto FloorsObj = MakeShared<FJsonObject>();
		for (const auto& [Floor, RoomIds] : B.FloorToRoomIds)
		{
			TArray<TSharedPtr<FJsonValue>> RoomArr;
			for (const FString& RId : RoomIds)
			{
				RoomArr.Add(MakeShared<FJsonValueString>(RId));
			}
			FloorsObj->SetArrayField(FString::FromInt(Floor), RoomArr);
		}
		BJ->SetObjectField(TEXT("floor_to_room_ids"), FloorsObj);

		TArray<TSharedPtr<FJsonValue>> ExitArr;
		for (const FString& D : B.ExteriorDoorIds)
		{
			ExitArr.Add(MakeShared<FJsonValueString>(D));
		}
		BJ->SetArrayField(TEXT("exterior_door_ids"), ExitArr);

		BuildingsArr.Add(MakeShared<FJsonValueObject>(BJ));
	}
	J->SetArrayField(TEXT("buildings"), BuildingsArr);

	// Rooms
	TArray<TSharedPtr<FJsonValue>> RoomsArr;
	for (const auto& [Id, R] : Rooms)
	{
		RoomsArr.Add(MakeShared<FJsonValueObject>(FMonolithMeshSpatialRegistry::RoomToJson(R)));
	}
	J->SetArrayField(TEXT("rooms"), RoomsArr);

	// Doors
	TArray<TSharedPtr<FJsonValue>> DoorsArr;
	for (const auto& [Id, D] : Doors)
	{
		DoorsArr.Add(MakeShared<FJsonValueObject>(FMonolithMeshSpatialRegistry::DoorToJson(D)));
	}
	J->SetArrayField(TEXT("doors"), DoorsArr);

	// Furniture
	TArray<TSharedPtr<FJsonValue>> FurnArr;
	for (const auto& [Id, F] : Furniture)
	{
		auto FJ = MakeShared<FJsonObject>();
		FJ->SetStringField(TEXT("furniture_id"), F.FurnitureId);
		FJ->SetStringField(TEXT("furniture_type"), F.FurnitureType);
		FJ->SetStringField(TEXT("actor_name"), F.ActorName);

		TArray<TSharedPtr<FJsonValue>> PosArr;
		PosArr.Add(MakeShared<FJsonValueNumber>(F.WorldPosition.X));
		PosArr.Add(MakeShared<FJsonValueNumber>(F.WorldPosition.Y));
		PosArr.Add(MakeShared<FJsonValueNumber>(F.WorldPosition.Z));
		FJ->SetArrayField(TEXT("world_position"), PosArr);

		FurnArr.Add(MakeShared<FJsonValueObject>(FJ));
	}
	J->SetArrayField(TEXT("furniture"), FurnArr);

	// Adjacency graph
	auto GraphObj = MakeShared<FJsonObject>();
	for (const auto& [RoomId, Edges] : AdjacencyGraph)
	{
		TArray<TSharedPtr<FJsonValue>> EdgeArr;
		for (const FSpatialAdjacencyEdge& E : Edges)
		{
			auto EJ = MakeShared<FJsonObject>();
			EJ->SetStringField(TEXT("connected_room_id"), E.ConnectedRoomId);
			EJ->SetStringField(TEXT("door_id"), E.DoorId);
			EdgeArr.Add(MakeShared<FJsonValueObject>(EJ));
		}
		GraphObj->SetArrayField(RoomId, EdgeArr);
	}
	J->SetObjectField(TEXT("adjacency_graph"), GraphObj);

	return J;
}

FSpatialBlock FSpatialBlock::FromJson(const TSharedPtr<FJsonObject>& Json)
{
	FSpatialBlock Block;
	if (!Json.IsValid()) return Block;

	Json->TryGetStringField(TEXT("block_id"), Block.BlockId);

	// Buildings
	const TArray<TSharedPtr<FJsonValue>>* BuildingsArr = nullptr;
	if (Json->TryGetArrayField(TEXT("buildings"), BuildingsArr) && BuildingsArr)
	{
		for (const auto& BVal : *BuildingsArr)
		{
			const TSharedPtr<FJsonObject>* BObjPtr = nullptr;
			if (!BVal->TryGetObject(BObjPtr) || !BObjPtr || !(*BObjPtr).IsValid()) continue;
			const auto& BObj = *BObjPtr;

			FSpatialBuilding B;
			BObj->TryGetStringField(TEXT("building_id"), B.BuildingId);
			BObj->TryGetStringField(TEXT("asset_path"), B.AssetPath);

			const TArray<TSharedPtr<FJsonValue>>* OriginArr = nullptr;
			if (BObj->TryGetArrayField(TEXT("world_origin"), OriginArr) && OriginArr && OriginArr->Num() >= 3)
			{
				B.WorldOrigin.X = (*OriginArr)[0]->AsNumber();
				B.WorldOrigin.Y = (*OriginArr)[1]->AsNumber();
				B.WorldOrigin.Z = (*OriginArr)[2]->AsNumber();
			}

			const TArray<TSharedPtr<FJsonValue>>* FootArr = nullptr;
			if (BObj->TryGetArrayField(TEXT("footprint_polygon"), FootArr) && FootArr)
			{
				for (const auto& PtVal : *FootArr)
				{
					const TArray<TSharedPtr<FJsonValue>>* PtArr = nullptr;
					if (PtVal->TryGetArray(PtArr) && PtArr && PtArr->Num() >= 2)
					{
						B.FootprintPolygon.Add(FVector2D((*PtArr)[0]->AsNumber(), (*PtArr)[1]->AsNumber()));
					}
				}
			}

			const TSharedPtr<FJsonObject>* FloorsObjPtr = nullptr;
			if (BObj->TryGetObjectField(TEXT("floor_to_room_ids"), FloorsObjPtr) && FloorsObjPtr && (*FloorsObjPtr).IsValid())
			{
				for (const auto& [Key, Val] : (*FloorsObjPtr)->Values)
				{
					int32 FloorIdx = FCString::Atoi(*Key);
					const TArray<TSharedPtr<FJsonValue>>* RoomArr = nullptr;
					if (Val->TryGetArray(RoomArr) && RoomArr)
					{
						TArray<FString> RoomIds;
						for (const auto& RVal : *RoomArr)
						{
							RoomIds.Add(RVal->AsString());
						}
						B.FloorToRoomIds.Add(FloorIdx, MoveTemp(RoomIds));
					}
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* ExitArr = nullptr;
			if (BObj->TryGetArrayField(TEXT("exterior_door_ids"), ExitArr) && ExitArr)
			{
				for (const auto& EVal : *ExitArr)
				{
					B.ExteriorDoorIds.Add(EVal->AsString());
				}
			}

			Block.Buildings.Add(B.BuildingId, MoveTemp(B));
		}
	}

	// Rooms
	const TArray<TSharedPtr<FJsonValue>>* RoomsArr = nullptr;
	if (Json->TryGetArrayField(TEXT("rooms"), RoomsArr) && RoomsArr)
	{
		for (const auto& RVal : *RoomsArr)
		{
			const TSharedPtr<FJsonObject>* RObjPtr = nullptr;
			if (!RVal->TryGetObject(RObjPtr) || !RObjPtr || !(*RObjPtr).IsValid()) continue;
			const auto& RObj = *RObjPtr;

			FSpatialRoom R;
			RObj->TryGetStringField(TEXT("room_id"), R.RoomId);
			RObj->TryGetStringField(TEXT("room_type"), R.RoomType);
			RObj->TryGetStringField(TEXT("building_id"), R.BuildingId);
			if (RObj->HasField(TEXT("floor_index")))
				R.FloorIndex = static_cast<int32>(RObj->GetNumberField(TEXT("floor_index")));

			const TSharedPtr<FJsonObject>* BoundsPtr = nullptr;
			if (RObj->TryGetObjectField(TEXT("world_bounds"), BoundsPtr) && BoundsPtr && (*BoundsPtr).IsValid())
			{
				FBox Box(ForceInit);
				const TArray<TSharedPtr<FJsonValue>>* MinArr = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* MaxArr = nullptr;
				if ((*BoundsPtr)->TryGetArrayField(TEXT("min"), MinArr) && MinArr && MinArr->Num() >= 3)
				{
					Box.Min = FVector((*MinArr)[0]->AsNumber(), (*MinArr)[1]->AsNumber(), (*MinArr)[2]->AsNumber());
				}
				if ((*BoundsPtr)->TryGetArrayField(TEXT("max"), MaxArr) && MaxArr && MaxArr->Num() >= 3)
				{
					Box.Max = FVector((*MaxArr)[0]->AsNumber(), (*MaxArr)[1]->AsNumber(), (*MaxArr)[2]->AsNumber());
				}
				Box.IsValid = 1;
				R.WorldBounds = Box;
			}

			const TArray<TSharedPtr<FJsonValue>>* AdjArr = nullptr;
			if (RObj->TryGetArrayField(TEXT("adjacent_room_ids"), AdjArr) && AdjArr)
			{
				for (const auto& AVal : *AdjArr)
				{
					R.AdjacentRoomIds.Add(AVal->AsString());
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* DoorArr = nullptr;
			if (RObj->TryGetArrayField(TEXT("door_ids"), DoorArr) && DoorArr)
			{
				for (const auto& DVal : *DoorArr)
				{
					R.DoorIds.Add(DVal->AsString());
				}
			}

			const TSharedPtr<FJsonObject>* TagsPtr = nullptr;
			if (RObj->TryGetObjectField(TEXT("tags"), TagsPtr) && TagsPtr && (*TagsPtr).IsValid())
			{
				for (const auto& [Key, Val] : (*TagsPtr)->Values)
				{
					R.Tags.Add(MonolithKeyToString(Key), Val->AsString());
				}
			}

			Block.Rooms.Add(R.RoomId, MoveTemp(R));
		}
	}

	// Doors
	const TArray<TSharedPtr<FJsonValue>>* DoorsArr = nullptr;
	if (Json->TryGetArrayField(TEXT("doors"), DoorsArr) && DoorsArr)
	{
		for (const auto& DVal : *DoorsArr)
		{
			const TSharedPtr<FJsonObject>* DObjPtr = nullptr;
			if (!DVal->TryGetObject(DObjPtr) || !DObjPtr || !(*DObjPtr).IsValid()) continue;
			const auto& DObj = *DObjPtr;

			FSpatialDoor D;
			DObj->TryGetStringField(TEXT("door_id"), D.DoorId);
			DObj->TryGetStringField(TEXT("wall"), D.Wall);
			DObj->TryGetStringField(TEXT("room_a"), D.RoomA);
			DObj->TryGetStringField(TEXT("room_b"), D.RoomB);
			if (DObj->HasField(TEXT("exterior")))
				D.bExterior = DObj->GetBoolField(TEXT("exterior"));
			if (DObj->HasField(TEXT("width")))
				D.Width = static_cast<float>(DObj->GetNumberField(TEXT("width")));
			if (DObj->HasField(TEXT("height")))
				D.Height = static_cast<float>(DObj->GetNumberField(TEXT("height")));

			const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
			if (DObj->TryGetArrayField(TEXT("world_position"), PosArr) && PosArr && PosArr->Num() >= 3)
			{
				D.WorldPosition = FVector((*PosArr)[0]->AsNumber(), (*PosArr)[1]->AsNumber(), (*PosArr)[2]->AsNumber());
			}

			Block.Doors.Add(D.DoorId, MoveTemp(D));
		}
	}

	// Furniture
	const TArray<TSharedPtr<FJsonValue>>* FurnArr = nullptr;
	if (Json->TryGetArrayField(TEXT("furniture"), FurnArr) && FurnArr)
	{
		for (const auto& FVal : *FurnArr)
		{
			const TSharedPtr<FJsonObject>* FObjPtr = nullptr;
			if (!FVal->TryGetObject(FObjPtr) || !FObjPtr || !(*FObjPtr).IsValid()) continue;
			const auto& FObj = *FObjPtr;

			FSpatialStreetFurniture F;
			FObj->TryGetStringField(TEXT("furniture_id"), F.FurnitureId);
			FObj->TryGetStringField(TEXT("furniture_type"), F.FurnitureType);
			FObj->TryGetStringField(TEXT("actor_name"), F.ActorName);

			const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
			if (FObj->TryGetArrayField(TEXT("world_position"), PosArr) && PosArr && PosArr->Num() >= 3)
			{
				F.WorldPosition = FVector((*PosArr)[0]->AsNumber(), (*PosArr)[1]->AsNumber(), (*PosArr)[2]->AsNumber());
			}

			Block.Furniture.Add(F.FurnitureId, MoveTemp(F));
		}
	}

	// Adjacency graph
	const TSharedPtr<FJsonObject>* GraphPtr = nullptr;
	if (Json->TryGetObjectField(TEXT("adjacency_graph"), GraphPtr) && GraphPtr && (*GraphPtr).IsValid())
	{
		for (const auto& [RoomId, Val] : (*GraphPtr)->Values)
		{
			const TArray<TSharedPtr<FJsonValue>>* EdgeArr = nullptr;
			if (Val->TryGetArray(EdgeArr) && EdgeArr)
			{
				TArray<FSpatialAdjacencyEdge> Edges;
				for (const auto& EVal : *EdgeArr)
				{
					const TSharedPtr<FJsonObject>* EObjPtr = nullptr;
					if (EVal->TryGetObject(EObjPtr) && EObjPtr && (*EObjPtr).IsValid())
					{
						FSpatialAdjacencyEdge Edge;
						(*EObjPtr)->TryGetStringField(TEXT("connected_room_id"), Edge.ConnectedRoomId);
						(*EObjPtr)->TryGetStringField(TEXT("door_id"), Edge.DoorId);
						Edges.Add(MoveTemp(Edge));
					}
				}
				Block.AdjacencyGraph.Add(MonolithKeyToString(RoomId), MoveTemp(Edges));
			}
		}
	}

	return Block;
}

// ============================================================================
// Singleton block storage
// ============================================================================

TMap<FString, FSpatialBlock>& FMonolithMeshSpatialRegistry::GetBlockMap()
{
	static TMap<FString, FSpatialBlock> GBlocks;
	return GBlocks;
}

FSpatialBlock& FMonolithMeshSpatialRegistry::GetBlock(const FString& BlockId)
{
	auto& Map = GetBlockMap();
	if (!Map.Contains(BlockId))
	{
		FSpatialBlock NewBlock;
		NewBlock.BlockId = BlockId;
		Map.Add(BlockId, MoveTemp(NewBlock));
	}
	return Map[BlockId];
}

bool FMonolithMeshSpatialRegistry::HasBlock(const FString& BlockId)
{
	return GetBlockMap().Contains(BlockId);
}

void FMonolithMeshSpatialRegistry::ClearBlock(const FString& BlockId)
{
	GetBlockMap().Remove(BlockId);
}

void FMonolithMeshSpatialRegistry::ClearAll()
{
	GetBlockMap().Empty();
}

TArray<FString> FMonolithMeshSpatialRegistry::GetLoadedBlockIds()
{
	TArray<FString> Keys;
	GetBlockMap().GetKeys(Keys);
	return Keys;
}

FString FMonolithMeshSpatialRegistry::GetSaveDirectory()
{
	return FPaths::ProjectSavedDir() / TEXT("Monolith") / TEXT("SpatialRegistry");
}

bool FMonolithMeshSpatialRegistry::EnsureSaveDirectory()
{
	const FString Dir = GetSaveDirectory();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*Dir))
	{
		return PlatformFile.CreateDirectoryTree(*Dir);
	}
	return true;
}

// ============================================================================
// Helpers
// ============================================================================

bool FMonolithMeshSpatialRegistry::ParseVector(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, FVector& OutVec)
{
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Obj->TryGetArrayField(FieldName, Arr) || !Arr || Arr->Num() < 3)
		return false;

	OutVec.X = (*Arr)[0]->AsNumber();
	OutVec.Y = (*Arr)[1]->AsNumber();
	OutVec.Z = (*Arr)[2]->AsNumber();
	return true;
}

bool FMonolithMeshSpatialRegistry::ParseBounds(const TSharedPtr<FJsonObject>& Obj, FBox& OutBox)
{
	const TArray<TSharedPtr<FJsonValue>>* MinArr = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* MaxArr = nullptr;

	if (!Obj->TryGetArrayField(TEXT("min"), MinArr) || !MinArr || MinArr->Num() < 3)
		return false;
	if (!Obj->TryGetArrayField(TEXT("max"), MaxArr) || !MaxArr || MaxArr->Num() < 3)
		return false;

	OutBox.Min = FVector((*MinArr)[0]->AsNumber(), (*MinArr)[1]->AsNumber(), (*MinArr)[2]->AsNumber());
	OutBox.Max = FVector((*MaxArr)[0]->AsNumber(), (*MaxArr)[1]->AsNumber(), (*MaxArr)[2]->AsNumber());
	OutBox.IsValid = 1;
	return true;
}

TArray<TSharedPtr<FJsonValue>> FMonolithMeshSpatialRegistry::VecToJsonArray(const FVector& V)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Add(MakeShared<FJsonValueNumber>(V.X));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
	return Arr;
}

TSharedPtr<FJsonObject> FMonolithMeshSpatialRegistry::RoomToJson(const FSpatialRoom& Room)
{
	auto J = MakeShared<FJsonObject>();
	J->SetStringField(TEXT("room_id"), Room.RoomId);
	J->SetStringField(TEXT("room_type"), Room.RoomType);
	J->SetStringField(TEXT("building_id"), Room.BuildingId);
	J->SetNumberField(TEXT("floor_index"), Room.FloorIndex);

	if (Room.WorldBounds.IsValid)
	{
		auto WB = MakeShared<FJsonObject>();
		WB->SetArrayField(TEXT("min"), VecToJsonArray(Room.WorldBounds.Min));
		WB->SetArrayField(TEXT("max"), VecToJsonArray(Room.WorldBounds.Max));
		J->SetObjectField(TEXT("world_bounds"), WB);
	}

	TArray<TSharedPtr<FJsonValue>> AdjArr;
	for (const FString& A : Room.AdjacentRoomIds)
	{
		AdjArr.Add(MakeShared<FJsonValueString>(A));
	}
	J->SetArrayField(TEXT("adjacent_room_ids"), AdjArr);

	TArray<TSharedPtr<FJsonValue>> DoorArr;
	for (const FString& D : Room.DoorIds)
	{
		DoorArr.Add(MakeShared<FJsonValueString>(D));
	}
	J->SetArrayField(TEXT("door_ids"), DoorArr);

	if (Room.Tags.Num() > 0)
	{
		auto TagsObj = MakeShared<FJsonObject>();
		for (const auto& [Key, Val] : Room.Tags)
		{
			TagsObj->SetStringField(Key, Val);
		}
		J->SetObjectField(TEXT("tags"), TagsObj);
	}

	// Computed area (XY plane, in cm^2)
	if (Room.WorldBounds.IsValid)
	{
		FVector Size = Room.WorldBounds.GetSize();
		J->SetNumberField(TEXT("area"), Size.X * Size.Y);
	}

	return J;
}

TSharedPtr<FJsonObject> FMonolithMeshSpatialRegistry::DoorToJson(const FSpatialDoor& Door)
{
	auto J = MakeShared<FJsonObject>();
	J->SetStringField(TEXT("door_id"), Door.DoorId);
	J->SetStringField(TEXT("wall"), Door.Wall);
	J->SetStringField(TEXT("room_a"), Door.RoomA);
	J->SetStringField(TEXT("room_b"), Door.RoomB);
	J->SetBoolField(TEXT("exterior"), Door.bExterior);
	J->SetNumberField(TEXT("width"), Door.Width);
	J->SetNumberField(TEXT("height"), Door.Height);
	J->SetArrayField(TEXT("world_position"), VecToJsonArray(Door.WorldPosition));
	return J;
}

// ============================================================================
// Adjacency graph construction
// ============================================================================

void FMonolithMeshSpatialRegistry::BuildAdjacencyFromBuilding(FSpatialBlock& Block,
	const FString& BuildingId, const TSharedPtr<FJsonObject>& BuildingJson, const FVector& BuildingOrigin)
{
	// Extract doors from the building descriptor's floor plans
	const TArray<TSharedPtr<FJsonValue>>* FloorsArr = nullptr;
	if (!BuildingJson->TryGetArrayField(TEXT("floors"), FloorsArr) || !FloorsArr) return;

	for (const auto& FloorVal : *FloorsArr)
	{
		const TSharedPtr<FJsonObject>* FloorObjPtr = nullptr;
		if (!FloorVal->TryGetObject(FloorObjPtr) || !FloorObjPtr || !(*FloorObjPtr).IsValid()) continue;
		const auto& FloorObj = *FloorObjPtr;

		int32 FloorIndex = 0;
		if (FloorObj->HasField(TEXT("floor_index")))
			FloorIndex = static_cast<int32>(FloorObj->GetNumberField(TEXT("floor_index")));

		// Process doors
		const TArray<TSharedPtr<FJsonValue>>* DoorsArr = nullptr;
		if (FloorObj->TryGetArrayField(TEXT("doors"), DoorsArr) && DoorsArr)
		{
			for (const auto& DoorVal : *DoorsArr)
			{
				const TSharedPtr<FJsonObject>* DObjPtr = nullptr;
				if (!DoorVal->TryGetObject(DObjPtr) || !DObjPtr || !(*DObjPtr).IsValid()) continue;
				const auto& DObj = *DObjPtr;

				FString DoorId;
				DObj->TryGetStringField(TEXT("door_id"), DoorId);

				// Qualify door ID with building
				FString QualifiedDoorId = FString::Printf(TEXT("%s/F%d/%s"), *BuildingId, FloorIndex, *DoorId);

				// Parse connects array [room_a, room_b]
				const TArray<TSharedPtr<FJsonValue>>* ConnectsArr = nullptr;
				FString RoomA, RoomB;
				if (DObj->TryGetArrayField(TEXT("connects"), ConnectsArr) && ConnectsArr && ConnectsArr->Num() >= 2)
				{
					RoomA = (*ConnectsArr)[0]->AsString();
					RoomB = (*ConnectsArr)[1]->AsString();
				}
				else
				{
					// Try room_a/room_b fields directly
					DObj->TryGetStringField(TEXT("room_a"), RoomA);
					DObj->TryGetStringField(TEXT("room_b"), RoomB);
				}

				if (RoomA.IsEmpty() || RoomB.IsEmpty()) continue;

				// Qualify room IDs with building+floor
				auto QualifyRoom = [&](const FString& RoomId) -> FString
				{
					if (RoomId == TEXT("exterior") || RoomId == TEXT("outside"))
						return TEXT("exterior");
					return FString::Printf(TEXT("%s/F%d/%s"), *BuildingId, FloorIndex, *RoomId);
				};

				FString QRoomA = QualifyRoom(RoomA);
				FString QRoomB = QualifyRoom(RoomB);

				// Parse door world position
				FVector DoorWorldPos = FVector::ZeroVector;
				const TArray<TSharedPtr<FJsonValue>>* WPosArr = nullptr;
				if (DObj->TryGetArrayField(TEXT("world_position"), WPosArr) && WPosArr && WPosArr->Num() >= 3)
				{
					DoorWorldPos = FVector((*WPosArr)[0]->AsNumber(), (*WPosArr)[1]->AsNumber(), (*WPosArr)[2]->AsNumber());
				}

				// Parse wall direction
				FString Wall;
				DObj->TryGetStringField(TEXT("wall"), Wall);

				float Width = 90.0f, Height = 220.0f;
				if (DObj->HasField(TEXT("width")))
					Width = static_cast<float>(DObj->GetNumberField(TEXT("width")));
				if (DObj->HasField(TEXT("height")))
					Height = static_cast<float>(DObj->GetNumberField(TEXT("height")));

				bool bExterior = (QRoomA == TEXT("exterior") || QRoomB == TEXT("exterior"));

				// Store door
				FSpatialDoor SDoor;
				SDoor.DoorId = QualifiedDoorId;
				SDoor.Wall = Wall;
				SDoor.WorldPosition = DoorWorldPos;
				SDoor.RoomA = QRoomA;
				SDoor.RoomB = QRoomB;
				SDoor.bExterior = bExterior;
				SDoor.Width = Width;
				SDoor.Height = Height;
				Block.Doors.Add(QualifiedDoorId, MoveTemp(SDoor));

				// Add adjacency edges (bidirectional) — skip exterior nodes for the graph
				if (QRoomA != TEXT("exterior") && QRoomB != TEXT("exterior"))
				{
					Block.AdjacencyGraph.FindOrAdd(QRoomA).Add({ QRoomB, QualifiedDoorId });
					Block.AdjacencyGraph.FindOrAdd(QRoomB).Add({ QRoomA, QualifiedDoorId });

					// Update room adjacency lists
					if (FSpatialRoom* RA = Block.Rooms.Find(QRoomA))
					{
						RA->AdjacentRoomIds.AddUnique(QRoomB);
						RA->DoorIds.AddUnique(QualifiedDoorId);
					}
					if (FSpatialRoom* RB = Block.Rooms.Find(QRoomB))
					{
						RB->AdjacentRoomIds.AddUnique(QRoomA);
						RB->DoorIds.AddUnique(QualifiedDoorId);
					}
				}
				else
				{
					// Exterior door — track on building
					if (FSpatialBuilding* Bldg = Block.Buildings.Find(BuildingId))
					{
						Bldg->ExteriorDoorIds.AddUnique(QualifiedDoorId);
					}

					// Still add adjacency for the interior room to exterior
					FString InteriorRoom = (QRoomA == TEXT("exterior")) ? QRoomB : QRoomA;
					Block.AdjacencyGraph.FindOrAdd(InteriorRoom).Add({ TEXT("exterior"), QualifiedDoorId });
					if (FSpatialRoom* IR = Block.Rooms.Find(InteriorRoom))
					{
						IR->DoorIds.AddUnique(QualifiedDoorId);
					}
				}
			}
		}

		// Process stairwells — connect rooms across floors
		const TArray<TSharedPtr<FJsonValue>>* StairArr = nullptr;
		if (FloorObj->TryGetArrayField(TEXT("stairwells"), StairArr) && StairArr)
		{
			for (const auto& StairVal : *StairArr)
			{
				const TSharedPtr<FJsonObject>* SObjPtr = nullptr;
				if (!StairVal->TryGetObject(SObjPtr) || !SObjPtr || !(*SObjPtr).IsValid()) continue;
				const auto& SObj = *SObjPtr;

				FString StairId;
				SObj->TryGetStringField(TEXT("stairwell_id"), StairId);

				// Parse connects_floors [floor_a, floor_b]
				const TArray<TSharedPtr<FJsonValue>>* FloorsConnArr = nullptr;
				if (!SObj->TryGetArrayField(TEXT("connects_floors"), FloorsConnArr) || !FloorsConnArr || FloorsConnArr->Num() < 2)
					continue;

				int32 FloorA = static_cast<int32>((*FloorsConnArr)[0]->AsNumber());
				int32 FloorB = static_cast<int32>((*FloorsConnArr)[1]->AsNumber());

				// The stairwell itself acts as a connection between a room on FloorA and a room on FloorB
				// Look for rooms that contain the stairwell cells on each floor
				// For now, create a synthetic stairwell room or use the room that contains the stairwell
				FString StairRoomA = FString::Printf(TEXT("%s/F%d/stairwell_%s"), *BuildingId, FloorA, *StairId);
				FString StairRoomB = FString::Printf(TEXT("%s/F%d/stairwell_%s"), *BuildingId, FloorB, *StairId);

				FString QualifiedStairId = FString::Printf(TEXT("%s/stair/%s"), *BuildingId, *StairId);

				// Add bidirectional stairwell adjacency
				Block.AdjacencyGraph.FindOrAdd(StairRoomA).Add({ StairRoomB, QualifiedStairId });
				Block.AdjacencyGraph.FindOrAdd(StairRoomB).Add({ StairRoomA, QualifiedStairId });
			}
		}
	}
}

// ============================================================================
// BFS Pathfinding
// ============================================================================

TArray<FString> FMonolithMeshSpatialRegistry::BFS(const FSpatialBlock& Block,
	const FString& StartRoom, const FString& EndRoom, const TSet<FString>& AvoidRooms)
{
	TQueue<FString> Queue;
	TMap<FString, FString> Parent;
	TSet<FString> Visited;

	Queue.Enqueue(StartRoom);
	Visited.Add(StartRoom);

	bool bFound = false;
	while (!Queue.IsEmpty())
	{
		FString Current;
		Queue.Dequeue(Current);

		if (Current == EndRoom)
		{
			bFound = true;
			break;
		}

		if (const auto* Adj = Block.AdjacencyGraph.Find(Current))
		{
			for (const FSpatialAdjacencyEdge& Edge : *Adj)
			{
				if (!Visited.Contains(Edge.ConnectedRoomId) && !AvoidRooms.Contains(Edge.ConnectedRoomId))
				{
					Visited.Add(Edge.ConnectedRoomId);
					Parent.Add(Edge.ConnectedRoomId, Current);
					Queue.Enqueue(Edge.ConnectedRoomId);
				}
			}
		}
	}

	// Reconstruct path
	TArray<FString> Path;
	if (!bFound) return Path;

	FString Current = EndRoom;
	while (Current != StartRoom)
	{
		Path.Insert(Current, 0);
		if (const FString* P = Parent.Find(Current))
		{
			Current = *P;
		}
		else
		{
			// Should not happen if bFound is true, but safety
			return TArray<FString>();
		}
	}
	Path.Insert(StartRoom, 0);
	return Path;
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshSpatialRegistry::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 1. register_building
	Registry.RegisterAction(TEXT("mesh"), TEXT("register_building"),
		TEXT("Register a building from its Building Descriptor JSON in the spatial registry. "
			"Extracts all rooms, doors, stairwells and builds the adjacency graph."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialRegistry::RegisterBuilding),
		FParamSchemaBuilder()
			.Required(TEXT("building_descriptor"), TEXT("object"), TEXT("The full Building Descriptor JSON from create_building_from_grid"))
			.Optional(TEXT("block_id"), TEXT("string"), TEXT("Block to register in"), TEXT("default"))
			.Build());

	// 2. register_room
	Registry.RegisterAction(TEXT("mesh"), TEXT("register_room"),
		TEXT("Register a single room in the spatial registry (for manual/incremental registration)."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialRegistry::RegisterRoom),
		FParamSchemaBuilder()
			.Required(TEXT("room_id"), TEXT("string"), TEXT("Unique room ID (e.g. 'clinic/F0/exam_room')"))
			.Required(TEXT("room_type"), TEXT("string"), TEXT("Room type (kitchen, bedroom, corridor, etc.)"))
			.Required(TEXT("building_id"), TEXT("string"), TEXT("Building this room belongs to"))
			.Required(TEXT("floor_index"), TEXT("number"), TEXT("Floor index (0-based)"))
			.Required(TEXT("world_bounds"), TEXT("object"), TEXT("Bounds as {min: [x,y,z], max: [x,y,z]}"))
			.Optional(TEXT("adjacent_rooms"), TEXT("array"), TEXT("Array of adjacent room IDs"))
			.Optional(TEXT("tags"), TEXT("object"), TEXT("Arbitrary key-value tags"))
			.Optional(TEXT("block_id"), TEXT("string"), TEXT("Block to register in"), TEXT("default"))
			.Build());

	// 3. register_street_furniture
	Registry.RegisterAction(TEXT("mesh"), TEXT("register_street_furniture"),
		TEXT("Register street furniture items (lamps, hydrants, benches, etc.) in the spatial registry."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialRegistry::RegisterStreetFurniture),
		FParamSchemaBuilder()
			.Required(TEXT("furniture_id"), TEXT("string"), TEXT("Unique furniture ID"))
			.Required(TEXT("furniture_type"), TEXT("string"), TEXT("Type: lamp, hydrant, bench, dumpster, tree, etc."))
			.Required(TEXT("world_position"), TEXT("array"), TEXT("World position as [x, y, z]"))
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Placed actor name in the level"))
			.Optional(TEXT("block_id"), TEXT("string"), TEXT("Block to register in"), TEXT("default"))
			.Build());

	// 4. query_room_at
	Registry.RegisterAction(TEXT("mesh"), TEXT("query_room_at"),
		TEXT("Find which room contains a given world position (point-in-AABB test)."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialRegistry::QueryRoomAt),
		FParamSchemaBuilder()
			.Required(TEXT("position"), TEXT("array"), TEXT("World position as [x, y, z]"))
			.Optional(TEXT("block_id"), TEXT("string"), TEXT("Block to query"), TEXT("default"))
			.Build());

	// 5. query_adjacent_rooms
	Registry.RegisterAction(TEXT("mesh"), TEXT("query_adjacent_rooms"),
		TEXT("Get all rooms connected to a given room via doors or stairwells."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialRegistry::QueryAdjacentRooms),
		FParamSchemaBuilder()
			.Required(TEXT("room_id"), TEXT("string"), TEXT("Room ID to query adjacency for"))
			.Optional(TEXT("include_stairwell"), TEXT("boolean"), TEXT("Include stairwell connections"), TEXT("true"))
			.Optional(TEXT("block_id"), TEXT("string"), TEXT("Block to query"), TEXT("default"))
			.Build());

	// 6. query_rooms_by_filter
	Registry.RegisterAction(TEXT("mesh"), TEXT("query_rooms_by_filter"),
		TEXT("Query rooms by type, floor, building, tags, or area range."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialRegistry::QueryRoomsByFilter),
		FParamSchemaBuilder()
			.Optional(TEXT("room_type"), TEXT("string"), TEXT("Room type to filter (or comma-separated list)"))
			.Optional(TEXT("floor_index"), TEXT("number"), TEXT("Floor index to filter"))
			.Optional(TEXT("building_id"), TEXT("string"), TEXT("Building ID to filter"))
			.Optional(TEXT("min_area"), TEXT("number"), TEXT("Minimum room area in cm^2"))
			.Optional(TEXT("max_area"), TEXT("number"), TEXT("Maximum room area in cm^2"))
			.Optional(TEXT("tags"), TEXT("object"), TEXT("Tags that must match (key-value pairs)"))
			.Optional(TEXT("block_id"), TEXT("string"), TEXT("Block to query"), TEXT("default"))
			.Build());

	// 7. query_building_exits
	Registry.RegisterAction(TEXT("mesh"), TEXT("query_building_exits"),
		TEXT("Get all exterior doors of a building."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialRegistry::QueryBuildingExits),
		FParamSchemaBuilder()
			.Required(TEXT("building_id"), TEXT("string"), TEXT("Building ID"))
			.Optional(TEXT("block_id"), TEXT("string"), TEXT("Block to query"), TEXT("default"))
			.Build());

	// 8. path_between_rooms
	Registry.RegisterAction(TEXT("mesh"), TEXT("path_between_rooms"),
		TEXT("BFS shortest path between two rooms through the door/stairwell adjacency graph."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialRegistry::PathBetweenRooms),
		FParamSchemaBuilder()
			.Required(TEXT("from_room_id"), TEXT("string"), TEXT("Starting room ID"))
			.Required(TEXT("to_room_id"), TEXT("string"), TEXT("Destination room ID"))
			.Optional(TEXT("avoid_rooms"), TEXT("array"), TEXT("Room IDs to exclude from pathfinding"))
			.Optional(TEXT("block_id"), TEXT("string"), TEXT("Block to query"), TEXT("default"))
			.Build());

	// 9. save_block_descriptor
	Registry.RegisterAction(TEXT("mesh"), TEXT("save_block_descriptor"),
		TEXT("Save the spatial registry for a block to a JSON file on disk."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialRegistry::SaveBlockDescriptor),
		FParamSchemaBuilder()
			.Optional(TEXT("block_id"), TEXT("string"), TEXT("Block to save"), TEXT("default"))
			.Optional(TEXT("filename"), TEXT("string"), TEXT("Custom filename (auto-generated from block_id if not provided)"))
			.Build());

	// 10. load_block_descriptor
	Registry.RegisterAction(TEXT("mesh"), TEXT("load_block_descriptor"),
		TEXT("Load a previously saved spatial registry from a JSON file."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialRegistry::LoadBlockDescriptor),
		FParamSchemaBuilder()
			.Optional(TEXT("block_id"), TEXT("string"), TEXT("Block ID to load (looks for {block_id}.json)"))
			.Optional(TEXT("filename"), TEXT("string"), TEXT("Explicit filename to load"))
			.Build());

	UE_LOG(LogSpatialRegistry, Log, TEXT("Registered 10 spatial registry actions"));
}

// ============================================================================
// Action Handlers
// ============================================================================

FMonolithActionResult FMonolithMeshSpatialRegistry::RegisterBuilding(const TSharedPtr<FJsonObject>& Params)
{
	const TSharedPtr<FJsonObject>* DescPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("building_descriptor"), DescPtr) || !DescPtr || !(*DescPtr).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: building_descriptor (object)"));
	}
	const auto& Desc = *DescPtr;

	FString BlockId = TEXT("default");
	Params->TryGetStringField(TEXT("block_id"), BlockId);

	FString BuildingId;
	if (!Desc->TryGetStringField(TEXT("building_id"), BuildingId) || BuildingId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("building_descriptor must contain a 'building_id' field"));
	}

	FSpatialBlock& Block = GetBlock(BlockId);

	// Create building entry
	FSpatialBuilding Building;
	Building.BuildingId = BuildingId;
	Desc->TryGetStringField(TEXT("asset_path"), Building.AssetPath);

	// World origin
	const TArray<TSharedPtr<FJsonValue>>* OriginArr = nullptr;
	if (Desc->TryGetArrayField(TEXT("world_origin"), OriginArr) && OriginArr && OriginArr->Num() >= 3)
	{
		Building.WorldOrigin.X = (*OriginArr)[0]->AsNumber();
		Building.WorldOrigin.Y = (*OriginArr)[1]->AsNumber();
		Building.WorldOrigin.Z = (*OriginArr)[2]->AsNumber();
	}

	// Footprint polygon
	const TArray<TSharedPtr<FJsonValue>>* FootArr = nullptr;
	if (Desc->TryGetArrayField(TEXT("footprint_polygon"), FootArr) && FootArr)
	{
		for (const auto& PtVal : *FootArr)
		{
			const TArray<TSharedPtr<FJsonValue>>* PtArr = nullptr;
			if (PtVal->TryGetArray(PtArr) && PtArr && PtArr->Num() >= 2)
			{
				Building.FootprintPolygon.Add(FVector2D((*PtArr)[0]->AsNumber(), (*PtArr)[1]->AsNumber()));
			}
		}
	}

	Block.Buildings.Add(BuildingId, MoveTemp(Building));

	// Extract rooms from floor plans
	int32 RoomCount = 0;
	int32 DoorCount = 0;
	const TArray<TSharedPtr<FJsonValue>>* FloorsArr = nullptr;
	if (Desc->TryGetArrayField(TEXT("floors"), FloorsArr) && FloorsArr)
	{
		for (const auto& FloorVal : *FloorsArr)
		{
			const TSharedPtr<FJsonObject>* FloorObjPtr = nullptr;
			if (!FloorVal->TryGetObject(FloorObjPtr) || !FloorObjPtr || !(*FloorObjPtr).IsValid()) continue;
			const auto& FloorObj = *FloorObjPtr;

			int32 FloorIndex = 0;
			if (FloorObj->HasField(TEXT("floor_index")))
				FloorIndex = static_cast<int32>(FloorObj->GetNumberField(TEXT("floor_index")));

			// Extract rooms
			const TArray<TSharedPtr<FJsonValue>>* RoomsArr = nullptr;
			if (FloorObj->TryGetArrayField(TEXT("rooms"), RoomsArr) && RoomsArr)
			{
				for (const auto& RVal : *RoomsArr)
				{
					const TSharedPtr<FJsonObject>* RObjPtr = nullptr;
					if (!RVal->TryGetObject(RObjPtr) || !RObjPtr || !(*RObjPtr).IsValid()) continue;
					const auto& RObj = *RObjPtr;

					FString RoomId;
					RObj->TryGetStringField(TEXT("room_id"), RoomId);
					if (RoomId.IsEmpty()) continue;

					// Qualify with building/floor
					FString QualifiedId = FString::Printf(TEXT("%s/F%d/%s"), *BuildingId, FloorIndex, *RoomId);

					FSpatialRoom Room;
					Room.RoomId = QualifiedId;
					RObj->TryGetStringField(TEXT("room_type"), Room.RoomType);
					Room.BuildingId = BuildingId;
					Room.FloorIndex = FloorIndex;

					// Parse world bounds
					const TSharedPtr<FJsonObject>* BoundsPtr = nullptr;
					if (RObj->TryGetObjectField(TEXT("world_bounds"), BoundsPtr) && BoundsPtr && (*BoundsPtr).IsValid())
					{
						ParseBounds(*BoundsPtr, Room.WorldBounds);
					}

					Block.Rooms.Add(QualifiedId, MoveTemp(Room));

					// Track floor-to-room mapping
					FSpatialBuilding* Bldg = Block.Buildings.Find(BuildingId);
					if (Bldg)
					{
						Bldg->FloorToRoomIds.FindOrAdd(FloorIndex).Add(QualifiedId);
					}

					++RoomCount;
				}
			}

			// Count doors
			const TArray<TSharedPtr<FJsonValue>>* DoorsArr = nullptr;
			if (FloorObj->TryGetArrayField(TEXT("doors"), DoorsArr) && DoorsArr)
			{
				DoorCount += DoorsArr->Num();
			}
		}
	}

	// Build adjacency graph from doors and stairwells
	FVector BuildingOrigin = FVector::ZeroVector;
	if (const FSpatialBuilding* Bldg = Block.Buildings.Find(BuildingId))
	{
		BuildingOrigin = Bldg->WorldOrigin;
	}
	BuildAdjacencyFromBuilding(Block, BuildingId, Desc, BuildingOrigin);

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("registered"), true);
	Result->SetStringField(TEXT("building_id"), BuildingId);
	Result->SetStringField(TEXT("block_id"), BlockId);
	Result->SetNumberField(TEXT("room_count"), RoomCount);
	Result->SetNumberField(TEXT("door_count"), DoorCount);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshSpatialRegistry::RegisterRoom(const TSharedPtr<FJsonObject>& Params)
{
	FString RoomId, RoomType, BuildingId;
	if (!Params->TryGetStringField(TEXT("room_id"), RoomId) || RoomId.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: room_id"));
	if (!Params->TryGetStringField(TEXT("room_type"), RoomType) || RoomType.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: room_type"));
	if (!Params->TryGetStringField(TEXT("building_id"), BuildingId) || BuildingId.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: building_id"));

	int32 FloorIndex = 0;
	if (Params->HasField(TEXT("floor_index")))
		FloorIndex = static_cast<int32>(Params->GetNumberField(TEXT("floor_index")));
	else
		return FMonolithActionResult::Error(TEXT("Missing required param: floor_index"));

	const TSharedPtr<FJsonObject>* BoundsPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("world_bounds"), BoundsPtr) || !BoundsPtr || !(*BoundsPtr).IsValid())
		return FMonolithActionResult::Error(TEXT("Missing required param: world_bounds (object with min/max arrays)"));

	FBox WorldBounds(ForceInit);
	if (!ParseBounds(*BoundsPtr, WorldBounds))
		return FMonolithActionResult::Error(TEXT("Invalid world_bounds: needs {min: [x,y,z], max: [x,y,z]}"));

	FString BlockId = TEXT("default");
	Params->TryGetStringField(TEXT("block_id"), BlockId);

	FSpatialBlock& Block = GetBlock(BlockId);

	FSpatialRoom Room;
	Room.RoomId = RoomId;
	Room.RoomType = RoomType;
	Room.BuildingId = BuildingId;
	Room.FloorIndex = FloorIndex;
	Room.WorldBounds = WorldBounds;

	// Optional: adjacent rooms
	const TArray<TSharedPtr<FJsonValue>>* AdjArr = nullptr;
	if (Params->TryGetArrayField(TEXT("adjacent_rooms"), AdjArr) && AdjArr)
	{
		for (const auto& AVal : *AdjArr)
		{
			FString AdjId = AVal->AsString();
			if (!AdjId.IsEmpty())
			{
				Room.AdjacentRoomIds.Add(AdjId);

				// Add bidirectional adjacency to graph
				FString SyntheticDoorId = FString::Printf(TEXT("manual_%s_%s"), *RoomId, *AdjId);
				Block.AdjacencyGraph.FindOrAdd(RoomId).Add({ AdjId, SyntheticDoorId });
				Block.AdjacencyGraph.FindOrAdd(AdjId).Add({ RoomId, SyntheticDoorId });
			}
		}
	}

	// Optional: tags
	const TSharedPtr<FJsonObject>* TagsPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("tags"), TagsPtr) && TagsPtr && (*TagsPtr).IsValid())
	{
		for (const auto& [Key, Val] : (*TagsPtr)->Values)
		{
			Room.Tags.Add(MonolithKeyToString(Key), Val->AsString());
		}
	}

	Block.Rooms.Add(RoomId, MoveTemp(Room));

	// Track in building's floor-to-room mapping
	FSpatialBuilding* Bldg = Block.Buildings.Find(BuildingId);
	if (Bldg)
	{
		Bldg->FloorToRoomIds.FindOrAdd(FloorIndex).AddUnique(RoomId);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("registered"), true);
	Result->SetStringField(TEXT("room_id"), RoomId);
	Result->SetStringField(TEXT("block_id"), BlockId);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshSpatialRegistry::RegisterStreetFurniture(const TSharedPtr<FJsonObject>& Params)
{
	FString FurnitureId, FurnitureType;
	if (!Params->TryGetStringField(TEXT("furniture_id"), FurnitureId) || FurnitureId.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: furniture_id"));
	if (!Params->TryGetStringField(TEXT("furniture_type"), FurnitureType) || FurnitureType.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: furniture_type"));

	FVector WorldPos;
	if (!ParseVector(Params, TEXT("world_position"), WorldPos))
		return FMonolithActionResult::Error(TEXT("Missing required param: world_position (array of 3 numbers)"));

	FString BlockId = TEXT("default");
	Params->TryGetStringField(TEXT("block_id"), BlockId);

	FSpatialBlock& Block = GetBlock(BlockId);

	FSpatialStreetFurniture Furn;
	Furn.FurnitureId = FurnitureId;
	Furn.FurnitureType = FurnitureType;
	Furn.WorldPosition = WorldPos;
	Params->TryGetStringField(TEXT("actor_name"), Furn.ActorName);

	Block.Furniture.Add(FurnitureId, MoveTemp(Furn));

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("registered"), true);
	Result->SetStringField(TEXT("furniture_id"), FurnitureId);
	Result->SetStringField(TEXT("block_id"), BlockId);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshSpatialRegistry::QueryRoomAt(const TSharedPtr<FJsonObject>& Params)
{
	FVector Position;
	if (!ParseVector(Params, TEXT("position"), Position))
		return FMonolithActionResult::Error(TEXT("Missing required param: position (array of 3 numbers)"));

	FString BlockId = TEXT("default");
	Params->TryGetStringField(TEXT("block_id"), BlockId);

	if (!HasBlock(BlockId))
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("found"), false);
		Result->SetStringField(TEXT("reason"), FString::Printf(TEXT("Block '%s' not loaded"), *BlockId));
		return FMonolithActionResult::Success(Result);
	}

	const FSpatialBlock& Block = GetBlock(BlockId);

	for (const auto& [Id, Room] : Block.Rooms)
	{
		if (Room.WorldBounds.IsValid && Room.WorldBounds.IsInsideOrOn(Position))
		{
			auto Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("found"), true);
			Result->SetObjectField(TEXT("room"), RoomToJson(Room));
			return FMonolithActionResult::Success(Result);
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("found"), false);
	Result->SetStringField(TEXT("reason"), TEXT("No room contains this position"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshSpatialRegistry::QueryAdjacentRooms(const TSharedPtr<FJsonObject>& Params)
{
	FString RoomId;
	if (!Params->TryGetStringField(TEXT("room_id"), RoomId) || RoomId.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: room_id"));

	FString BlockId = TEXT("default");
	Params->TryGetStringField(TEXT("block_id"), BlockId);

	bool bIncludeStairwell = true;
	if (Params->HasField(TEXT("include_stairwell")))
		bIncludeStairwell = Params->GetBoolField(TEXT("include_stairwell"));

	if (!HasBlock(BlockId))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Block '%s' not loaded"), *BlockId));

	const FSpatialBlock& Block = GetBlock(BlockId);

	if (!Block.Rooms.Contains(RoomId))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Room '%s' not found in block '%s'"), *RoomId, *BlockId));

	TArray<TSharedPtr<FJsonValue>> AdjArr;
	if (const auto* Adj = Block.AdjacencyGraph.Find(RoomId))
	{
		for (const FSpatialAdjacencyEdge& Edge : *Adj)
		{
			// Filter out stairwell connections if requested
			if (!bIncludeStairwell && Edge.DoorId.Contains(TEXT("/stair/")))
				continue;

			auto AdjObj = MakeShared<FJsonObject>();
			AdjObj->SetStringField(TEXT("room_id"), Edge.ConnectedRoomId);
			AdjObj->SetStringField(TEXT("door_id"), Edge.DoorId);

			if (const FSpatialRoom* AdjRoom = Block.Rooms.Find(Edge.ConnectedRoomId))
			{
				AdjObj->SetStringField(TEXT("room_type"), AdjRoom->RoomType);
				AdjObj->SetStringField(TEXT("building_id"), AdjRoom->BuildingId);
				AdjObj->SetNumberField(TEXT("floor_index"), AdjRoom->FloorIndex);
			}
			else if (Edge.ConnectedRoomId == TEXT("exterior"))
			{
				AdjObj->SetStringField(TEXT("room_type"), TEXT("exterior"));
			}

			AdjArr.Add(MakeShared<FJsonValueObject>(AdjObj));
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("room_id"), RoomId);
	Result->SetArrayField(TEXT("adjacent"), AdjArr);
	Result->SetNumberField(TEXT("count"), AdjArr.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshSpatialRegistry::QueryRoomsByFilter(const TSharedPtr<FJsonObject>& Params)
{
	FString BlockId = TEXT("default");
	Params->TryGetStringField(TEXT("block_id"), BlockId);

	if (!HasBlock(BlockId))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Block '%s' not loaded"), *BlockId));

	const FSpatialBlock& Block = GetBlock(BlockId);

	// Parse filters
	FString FilterRoomType, FilterBuildingId;
	Params->TryGetStringField(TEXT("room_type"), FilterRoomType);
	Params->TryGetStringField(TEXT("building_id"), FilterBuildingId);

	// Support comma-separated room types
	TArray<FString> RoomTypes;
	if (!FilterRoomType.IsEmpty())
	{
		FilterRoomType.ParseIntoArray(RoomTypes, TEXT(","), true);
		for (FString& T : RoomTypes) T.TrimStartAndEndInline();
	}

	bool bHasFloorFilter = Params->HasField(TEXT("floor_index"));
	int32 FilterFloor = bHasFloorFilter ? static_cast<int32>(Params->GetNumberField(TEXT("floor_index"))) : 0;

	bool bHasMinArea = Params->HasField(TEXT("min_area"));
	bool bHasMaxArea = Params->HasField(TEXT("max_area"));
	double MinArea = bHasMinArea ? Params->GetNumberField(TEXT("min_area")) : 0.0;
	double MaxArea = bHasMaxArea ? Params->GetNumberField(TEXT("max_area")) : 0.0;

	const TSharedPtr<FJsonObject>* TagsFilterPtr = nullptr;
	bool bHasTagsFilter = Params->TryGetObjectField(TEXT("tags"), TagsFilterPtr) && TagsFilterPtr && (*TagsFilterPtr).IsValid();

	TArray<TSharedPtr<FJsonValue>> MatchArr;
	for (const auto& [Id, Room] : Block.Rooms)
	{
		// Room type filter
		if (RoomTypes.Num() > 0 && !RoomTypes.Contains(Room.RoomType))
			continue;

		// Building filter
		if (!FilterBuildingId.IsEmpty() && Room.BuildingId != FilterBuildingId)
			continue;

		// Floor filter
		if (bHasFloorFilter && Room.FloorIndex != FilterFloor)
			continue;

		// Area filter
		if (Room.WorldBounds.IsValid && (bHasMinArea || bHasMaxArea))
		{
			FVector Size = Room.WorldBounds.GetSize();
			double Area = Size.X * Size.Y;
			if (bHasMinArea && Area < MinArea) continue;
			if (bHasMaxArea && Area > MaxArea) continue;
		}

		// Tags filter
		if (bHasTagsFilter)
		{
			bool bAllTagsMatch = true;
			for (const auto& [Key, Val] : (*TagsFilterPtr)->Values)
			{
				const FString* RoomTag = Room.Tags.Find(MonolithKeyToString(Key));
				if (!RoomTag || *RoomTag != Val->AsString())
				{
					bAllTagsMatch = false;
					break;
				}
			}
			if (!bAllTagsMatch) continue;
		}

		MatchArr.Add(MakeShared<FJsonValueObject>(RoomToJson(Room)));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), MatchArr.Num());
	Result->SetArrayField(TEXT("rooms"), MatchArr);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshSpatialRegistry::QueryBuildingExits(const TSharedPtr<FJsonObject>& Params)
{
	FString BuildingId;
	if (!Params->TryGetStringField(TEXT("building_id"), BuildingId) || BuildingId.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: building_id"));

	FString BlockId = TEXT("default");
	Params->TryGetStringField(TEXT("block_id"), BlockId);

	if (!HasBlock(BlockId))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Block '%s' not loaded"), *BlockId));

	const FSpatialBlock& Block = GetBlock(BlockId);
	const FSpatialBuilding* Bldg = Block.Buildings.Find(BuildingId);
	if (!Bldg)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Building '%s' not found in block '%s'"), *BuildingId, *BlockId));

	TArray<TSharedPtr<FJsonValue>> ExitsArr;
	for (const FString& DoorId : Bldg->ExteriorDoorIds)
	{
		if (const FSpatialDoor* Door = Block.Doors.Find(DoorId))
		{
			auto ExitObj = MakeShared<FJsonObject>();
			ExitObj->SetStringField(TEXT("door_id"), Door->DoorId);
			ExitObj->SetStringField(TEXT("wall"), Door->Wall);
			ExitObj->SetArrayField(TEXT("world_position"), VecToJsonArray(Door->WorldPosition));
			ExitObj->SetNumberField(TEXT("width"), Door->Width);
			ExitObj->SetNumberField(TEXT("height"), Door->Height);

			TArray<TSharedPtr<FJsonValue>> ConnectsArr;
			ConnectsArr.Add(MakeShared<FJsonValueString>(Door->RoomA));
			ConnectsArr.Add(MakeShared<FJsonValueString>(Door->RoomB));
			ExitObj->SetArrayField(TEXT("connects"), ConnectsArr);

			ExitsArr.Add(MakeShared<FJsonValueObject>(ExitObj));
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("building_id"), BuildingId);
	Result->SetArrayField(TEXT("exits"), ExitsArr);
	Result->SetNumberField(TEXT("count"), ExitsArr.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshSpatialRegistry::PathBetweenRooms(const TSharedPtr<FJsonObject>& Params)
{
	FString FromRoom, ToRoom;
	if (!Params->TryGetStringField(TEXT("from_room_id"), FromRoom) || FromRoom.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: from_room_id"));
	if (!Params->TryGetStringField(TEXT("to_room_id"), ToRoom) || ToRoom.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: to_room_id"));

	FString BlockId = TEXT("default");
	Params->TryGetStringField(TEXT("block_id"), BlockId);

	if (!HasBlock(BlockId))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Block '%s' not loaded"), *BlockId));

	const FSpatialBlock& Block = GetBlock(BlockId);

	if (!Block.Rooms.Contains(FromRoom))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Room '%s' not found in block '%s'"), *FromRoom, *BlockId));
	if (!Block.Rooms.Contains(ToRoom))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Room '%s' not found in block '%s'"), *ToRoom, *BlockId));

	// Parse avoid rooms
	TSet<FString> AvoidRooms;
	const TArray<TSharedPtr<FJsonValue>>* AvoidArr = nullptr;
	if (Params->TryGetArrayField(TEXT("avoid_rooms"), AvoidArr) && AvoidArr)
	{
		for (const auto& AVal : *AvoidArr)
		{
			FString Avoid = AVal->AsString();
			if (!Avoid.IsEmpty()) AvoidRooms.Add(Avoid);
		}
	}

	TArray<FString> Path = BFS(Block, FromRoom, ToRoom, AvoidRooms);

	if (Path.Num() == 0)
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("found"), false);
		Result->SetStringField(TEXT("reason"), TEXT("No path found between the specified rooms"));
		return FMonolithActionResult::Success(Result);
	}

	// Build path with room details and door IDs
	TArray<TSharedPtr<FJsonValue>> PathArr;
	for (int32 i = 0; i < Path.Num(); ++i)
	{
		auto StepObj = MakeShared<FJsonObject>();
		StepObj->SetStringField(TEXT("room_id"), Path[i]);

		if (const FSpatialRoom* Room = Block.Rooms.Find(Path[i]))
		{
			StepObj->SetStringField(TEXT("room_type"), Room->RoomType);
		}

		// Find the door connecting this room to the next in the path
		if (i < Path.Num() - 1)
		{
			if (const auto* Adj = Block.AdjacencyGraph.Find(Path[i]))
			{
				for (const FSpatialAdjacencyEdge& Edge : *Adj)
				{
					if (Edge.ConnectedRoomId == Path[i + 1])
					{
						StepObj->SetStringField(TEXT("door_to_next"), Edge.DoorId);
						break;
					}
				}
			}
		}

		PathArr.Add(MakeShared<FJsonValueObject>(StepObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("found"), true);
	Result->SetArrayField(TEXT("path"), PathArr);
	Result->SetNumberField(TEXT("distance"), Path.Num() - 1); // doors traversed = rooms - 1
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshSpatialRegistry::SaveBlockDescriptor(const TSharedPtr<FJsonObject>& Params)
{
	FString BlockId = TEXT("default");
	Params->TryGetStringField(TEXT("block_id"), BlockId);

	if (!HasBlock(BlockId))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Block '%s' not loaded — nothing to save"), *BlockId));

	if (!EnsureSaveDirectory())
		return FMonolithActionResult::Error(TEXT("Failed to create save directory"));

	FString Filename;
	if (!Params->TryGetStringField(TEXT("filename"), Filename) || Filename.IsEmpty())
	{
		Filename = BlockId + TEXT(".json");
	}
	if (!Filename.EndsWith(TEXT(".json")))
	{
		Filename += TEXT(".json");
	}

	const FSpatialBlock& Block = GetBlock(BlockId);
	TSharedPtr<FJsonObject> Json = Block.ToJson();

	FString JsonStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);

	FString FilePath = GetSaveDirectory() / Filename;

	// Atomic write: write to .tmp then rename
	FString TmpPath = FilePath + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(JsonStr, *TmpPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to write temp file: %s"), *TmpPath));
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.FileExists(*FilePath))
	{
		PlatformFile.DeleteFile(*FilePath);
	}
	if (!PlatformFile.MoveFile(*FilePath, *TmpPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to rename temp file to: %s"), *FilePath));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("saved"), true);
	Result->SetStringField(TEXT("path"), FilePath);
	Result->SetStringField(TEXT("block_id"), BlockId);
	Result->SetNumberField(TEXT("building_count"), Block.Buildings.Num());
	Result->SetNumberField(TEXT("room_count"), Block.Rooms.Num());
	Result->SetNumberField(TEXT("furniture_count"), Block.Furniture.Num());
	Result->SetNumberField(TEXT("door_count"), Block.Doors.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshSpatialRegistry::LoadBlockDescriptor(const TSharedPtr<FJsonObject>& Params)
{
	FString BlockId, Filename;
	Params->TryGetStringField(TEXT("block_id"), BlockId);
	Params->TryGetStringField(TEXT("filename"), Filename);

	if (BlockId.IsEmpty() && Filename.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Provide either 'block_id' or 'filename'"));

	if (Filename.IsEmpty())
	{
		Filename = BlockId + TEXT(".json");
	}
	if (!Filename.EndsWith(TEXT(".json")))
	{
		Filename += TEXT(".json");
	}

	FString FilePath = GetSaveDirectory() / Filename;
	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *FilePath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load file: %s"), *FilePath));
	}

	TSharedPtr<FJsonObject> Json = FMonolithJsonUtils::Parse(JsonStr);
	if (!Json.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to parse JSON from: %s"), *FilePath));
	}

	FSpatialBlock Block = FSpatialBlock::FromJson(Json);

	// Use block_id from the file if not explicitly provided
	if (BlockId.IsEmpty())
	{
		BlockId = Block.BlockId;
	}
	if (BlockId.IsEmpty())
	{
		BlockId = TEXT("default");
	}
	Block.BlockId = BlockId;

	GetBlockMap().Add(BlockId, MoveTemp(Block));

	const FSpatialBlock& Loaded = GetBlock(BlockId);

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("loaded"), true);
	Result->SetStringField(TEXT("block_id"), BlockId);
	Result->SetStringField(TEXT("path"), FilePath);
	Result->SetNumberField(TEXT("building_count"), Loaded.Buildings.Num());
	Result->SetNumberField(TEXT("room_count"), Loaded.Rooms.Num());
	Result->SetNumberField(TEXT("furniture_count"), Loaded.Furniture.Num());
	Result->SetNumberField(TEXT("door_count"), Loaded.Doors.Num());
	return FMonolithActionResult::Success(Result);
}
