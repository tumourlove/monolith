#include "MonolithMeshHandlePool.h"

#if !WITH_GEOMETRYSCRIPT
#include "Dom/JsonObject.h"
// Stub implementations when GeometryScript is not available
void UMonolithMeshHandlePool::Initialize() {}
void UMonolithMeshHandlePool::Teardown() {}
bool UMonolithMeshHandlePool::CreateHandle(const FString&, const FString&, FString& OutError) { OutError = TEXT("GeometryScript not available"); return false; }
UDynamicMesh* UMonolithMeshHandlePool::GetHandle(const FString&, FString& OutError) { OutError = TEXT("GeometryScript not available"); return nullptr; }
bool UMonolithMeshHandlePool::ReleaseHandle(const FString&) { return false; }
bool UMonolithMeshHandlePool::SaveHandle(const FString&, const FString&, bool, FString& OutError, const FString&, int32) { OutError = TEXT("GeometryScript not available"); return false; }
TSharedPtr<FJsonObject> UMonolithMeshHandlePool::ListHandles() const { return MakeShared<FJsonObject>(); }
#else // WITH_GEOMETRYSCRIPT
#include "MonolithJsonUtils.h"

#include "UDynamicMesh.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "DynamicMeshToMeshDescription.h"
#include "MeshDescription.h"

#include "Engine/StaticMesh.h"
#include "StaticMeshAttributes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/Linker.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"

#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/CollisionFunctions.h"
#include "PhysicsEngine/BodySetup.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using namespace UE::Geometry;

// ============================================================================
// Lifecycle
// ============================================================================

void UMonolithMeshHandlePool::Initialize()
{
	EvictionTimerHandle = FTSTicker::GetCoreTicker().AddTicker(
		TEXT("MonolithMeshHandlePool_Eviction"),
		EvictionCheckIntervalSeconds,
		[this](float DeltaTime) -> bool
		{
			return OnEvictionCheck(DeltaTime);
		}
	);
}

void UMonolithMeshHandlePool::Teardown()
{
	if (EvictionTimerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(EvictionTimerHandle);
		EvictionTimerHandle.Reset();
	}

	// Clear all handles
	Handles.Empty();
	HandleSources.Empty();
	LastAccessTime.Empty();
}

// ============================================================================
// Handle Management
// ============================================================================

void UMonolithMeshHandlePool::TouchHandle(const FString& HandleName)
{
	LastAccessTime.FindOrAdd(HandleName) = FPlatformTime::Seconds();
}

bool UMonolithMeshHandlePool::CreateHandle(const FString& HandleName, const FString& Source, FString& OutError)
{
	if (Handles.Contains(HandleName))
	{
		OutError = FString::Printf(TEXT("Handle '%s' already exists. Use release_handle first."), *HandleName);
		return false;
	}

	if (Handles.Num() >= MaxHandles)
	{
		OutError = FString::Printf(TEXT("Handle pool full (%d/%d). Release unused handles with release_handle."), Handles.Num(), MaxHandles);
		return false;
	}

	UDynamicMesh* NewMesh = nullptr;

	if (Source.StartsWith(TEXT("primitive:")))
	{
		FString PrimitiveType = Source.Mid(10); // len("primitive:") == 10
		NewMesh = CreateFromPrimitive(PrimitiveType, OutError);
	}
	else if (Source.StartsWith(TEXT("internal:")))
	{
		// Internal source: create empty mesh for operations (boolean results, LODs, etc.)
		NewMesh = NewObject<UDynamicMesh>(this);
		if (!NewMesh)
		{
			OutError = TEXT("Failed to allocate UDynamicMesh");
		}
	}
	else
	{
		NewMesh = CreateFromAsset(Source, OutError);
	}

	if (!NewMesh)
	{
		return false;
	}

	Handles.Add(HandleName, NewMesh);
	HandleSources.Add(HandleName, Source);
	TouchHandle(HandleName);

	return true;
}

UDynamicMesh* UMonolithMeshHandlePool::GetHandle(const FString& HandleName, FString& OutError)
{
	TObjectPtr<UObject>* Found = Handles.Find(HandleName);
	if (!Found || !(*Found))
	{
		OutError = FString::Printf(TEXT("Handle '%s' not found. Use list_handles to see available handles."), *HandleName);
		return nullptr;
	}

	TouchHandle(HandleName);
	return Cast<UDynamicMesh>(*Found);
}

bool UMonolithMeshHandlePool::ReleaseHandle(const FString& HandleName)
{
	if (!Handles.Contains(HandleName))
	{
		return false;
	}

	Handles.Remove(HandleName);
	HandleSources.Remove(HandleName);
	LastAccessTime.Remove(HandleName);
	return true;
}

bool UMonolithMeshHandlePool::SaveHandle(const FString& HandleName, const FString& TargetPath, bool bOverwrite, FString& OutError,
	const FString& CollisionMode, int32 MaxHulls)
{
	FString Error;
	UDynamicMesh* DynMesh = GetHandle(HandleName, Error);
	if (!DynMesh)
	{
		OutError = Error;
		return false;
	}

	// Check if package already exists
	FString PackagePath = TargetPath;
	FString AssetName;
	// Extract asset name from path: "/Game/Meshes/MyMesh" -> PackagePath="/Game/Meshes/MyMesh", AssetName="MyMesh"
	int32 LastSlash;
	if (PackagePath.FindLastChar('/', LastSlash))
	{
		AssetName = PackagePath.Mid(LastSlash + 1);
	}
	else
	{
		AssetName = PackagePath;
	}

	if (!bOverwrite && FPackageName::DoesPackageExist(PackagePath))
	{
		OutError = FString::Printf(TEXT("Package '%s' already exists. Set overwrite:true to replace it."), *PackagePath);
		return false;
	}

	// Convert DynamicMesh to MeshDescription
	FMeshDescription MeshDesc;
	FStaticMeshAttributes StaticMeshAttrs(MeshDesc);
	StaticMeshAttrs.Register();

	FDynamicMeshToMeshDescription Converter;
	Converter.Convert(&DynMesh->GetMeshRef(), MeshDesc);

	// ---- Handle existing in-memory packages ----
	// CreatePackage returns an existing in-memory package if one exists at the same path.
	// That package may be partially loaded (e.g., from asset registry scan or a prior failed save),
	// which causes "cannot be saved as it has only been partially loaded" errors.
	// We must fully unload any existing package before creating a fresh one.
	FString PackageFileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());

	UPackage* ExistingPackage = FindPackage(nullptr, *PackagePath);
	if (ExistingPackage)
	{
		// Fully unload the existing package so CreatePackage returns a clean one
		TArray<UPackage*> PackagesToUnload;
		PackagesToUnload.Add(ExistingPackage);
		UPackageTools::UnloadPackages(PackagesToUnload);

		// Reset the loader so the package is truly detached from disk state
		ResetLoaders(ExistingPackage);

		// After unload, FindPackage may still return the old object if GC hasn't run.
		// Rename it out of the way so CreatePackage makes a fresh one.
		ExistingPackage = FindPackage(nullptr, *PackagePath);
		if (ExistingPackage)
		{
			FString TrashedName = FString::Printf(TEXT("/Temp/__monolith_evicted_%s_%s"),
				*AssetName, *FGuid::NewGuid().ToString(EGuidFormats::Short));
			ExistingPackage->Rename(*TrashedName, nullptr, REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders);
		}
	}

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Failed to create package at '%s'"), *PackagePath);
		return false;
	}

	// Ensure the package is fully loaded and clean for saving
	Package->FullyLoad();
	Package->SetPackageFlags(PKG_NewlyCreated);

	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!StaticMesh)
	{
		OutError = TEXT("Failed to create StaticMesh object");
		return false;
	}

	// Build the static mesh from mesh description
	TArray<const FMeshDescription*> MeshDescs;
	MeshDescs.Add(&MeshDesc);

	UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
	BuildParams.bMarkPackageDirty = true;
	BuildParams.bBuildSimpleCollision = false;
	BuildParams.bCommitMeshDescription = true;

	StaticMesh->BuildFromMeshDescriptions(MeshDescs, BuildParams);

	// Auto-generate collision based on CollisionMode
	if (CollisionMode != TEXT("none"))
	{
		StaticMesh->CreateBodySetup();
		UBodySetup* BS = StaticMesh->GetBodySetup();
		check(BS);

		if (CollisionMode == TEXT("complex_as_simple"))
		{
			BS->CollisionTraceFlag = CTF_UseComplexAsSimple;
			BS->CreatePhysicsMeshes();
		}
		else if (CollisionMode == TEXT("box"))
		{
			FKBoxElem BoxElem;
			BoxElem.Center = StaticMesh->GetRenderData()->Bounds.Origin;
			BoxElem.X = StaticMesh->GetRenderData()->Bounds.BoxExtent.X * 2.0f;
			BoxElem.Y = StaticMesh->GetRenderData()->Bounds.BoxExtent.Y * 2.0f;
			BoxElem.Z = StaticMesh->GetRenderData()->Bounds.BoxExtent.Z * 2.0f;
			BS->AggGeom.BoxElems.Add(BoxElem);
			BS->CreatePhysicsMeshes();
		}
		else // "auto" or "convex"
		{
			if (DynMesh)
			{
				FGeometryScriptCollisionFromMeshOptions CollisionOpts;
				CollisionOpts.bEmitTransaction = false;
				CollisionOpts.Method = EGeometryScriptCollisionGenerationMethod::ConvexHulls;
				CollisionOpts.MaxConvexHullsPerMesh = FMath::Max(1, MaxHulls);
				CollisionOpts.bSimplifyHulls = true;
				CollisionOpts.ConvexHullTargetFaceCount = 25;

				UGeometryScriptLibrary_CollisionFunctions::SetStaticMeshCollisionFromMesh(
					DynMesh, StaticMesh, CollisionOpts);
			}
			else
			{
				// DynMesh was released before save — fall back to AABB box collision
				FKBoxElem BoxElem;
				BoxElem.Center = StaticMesh->GetRenderData()->Bounds.Origin;
				BoxElem.X = StaticMesh->GetRenderData()->Bounds.BoxExtent.X * 2.0f;
				BoxElem.Y = StaticMesh->GetRenderData()->Bounds.BoxExtent.Y * 2.0f;
				BoxElem.Z = StaticMesh->GetRenderData()->Bounds.BoxExtent.Z * 2.0f;
				BS->AggGeom.BoxElems.Add(BoxElem);
				BS->CreatePhysicsMeshes();
			}
		}
	}

	// Save the package
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Package, StaticMesh, *PackageFileName, SaveArgs);

	// Notify asset registry
	FAssetRegistryModule::AssetCreated(StaticMesh);
	Package->MarkPackageDirty();

	return true;
}

TSharedPtr<FJsonObject> UMonolithMeshHandlePool::ListHandles() const
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> HandleArray;

	for (const auto& Pair : Handles)
	{
		TSharedPtr<FJsonObject> HandleObj = MakeShared<FJsonObject>();
		HandleObj->SetStringField(TEXT("handle"), Pair.Key);

		const FString* Source = HandleSources.Find(Pair.Key);
		HandleObj->SetStringField(TEXT("source"), Source ? *Source : TEXT("unknown"));

		UDynamicMesh* DynMesh = Cast<UDynamicMesh>(Pair.Value.Get());
		if (DynMesh)
		{
			HandleObj->SetNumberField(TEXT("triangle_count"), DynMesh->GetTriangleCount());
		}
		else
		{
			HandleObj->SetNumberField(TEXT("triangle_count"), 0);
		}

		const double* AccessTime = LastAccessTime.Find(Pair.Key);
		if (AccessTime)
		{
			double SecondsSinceAccess = FPlatformTime::Seconds() - *AccessTime;
			HandleObj->SetNumberField(TEXT("seconds_since_last_access"), FMath::RoundToFloat(SecondsSinceAccess));
		}

		HandleArray.Add(MakeShared<FJsonValueObject>(HandleObj));
	}

	Result->SetArrayField(TEXT("handles"), HandleArray);
	Result->SetNumberField(TEXT("count"), Handles.Num());
	Result->SetNumberField(TEXT("max"), MaxHandles);

	return Result;
}

// ============================================================================
// Eviction
// ============================================================================

bool UMonolithMeshHandlePool::OnEvictionCheck(float /*DeltaTime*/)
{
	const double Now = FPlatformTime::Seconds();
	TArray<FString> ToEvict;

	for (const auto& Pair : LastAccessTime)
	{
		if ((Now - Pair.Value) > EvictionTimeoutSeconds)
		{
			ToEvict.Add(Pair.Key);
		}
	}

	for (const FString& HandleName : ToEvict)
	{
		UE_LOG(LogMonolith, Log, TEXT("Monolith — Evicting idle mesh handle '%s'"), *HandleName);
		ReleaseHandle(HandleName);
	}

	return true; // Keep ticking
}

// ============================================================================
// Creation Helpers
// ============================================================================

UDynamicMesh* UMonolithMeshHandlePool::CreateFromPrimitive(const FString& PrimitiveType, FString& OutError)
{
	UDynamicMesh* DynMesh = NewObject<UDynamicMesh>(this);
	if (!DynMesh)
	{
		OutError = TEXT("Failed to allocate UDynamicMesh");
		return nullptr;
	}

	FGeometryScriptPrimitiveOptions PrimOpts;
	FTransform Identity = FTransform::Identity;

	FString TypeLower = PrimitiveType.ToLower().TrimStartAndEnd();

	if (TypeLower == TEXT("box"))
	{
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
			DynMesh, PrimOpts, Identity, 100, 100, 100, 0, 0, 0, EGeometryScriptPrimitiveOriginMode::Center);
	}
	else if (TypeLower == TEXT("sphere"))
	{
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
			DynMesh, PrimOpts, Identity, 50, 10, 16, EGeometryScriptPrimitiveOriginMode::Center);
	}
	else if (TypeLower == TEXT("cylinder"))
	{
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(
			DynMesh, PrimOpts, Identity, 50, 100, 12, 0, true, EGeometryScriptPrimitiveOriginMode::Base);
	}
	else if (TypeLower == TEXT("cone"))
	{
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCone(
			DynMesh, PrimOpts, Identity, 50, 5, 100, 12, 4, true, EGeometryScriptPrimitiveOriginMode::Base);
	}
	else if (TypeLower == TEXT("torus"))
	{
		FGeometryScriptRevolveOptions RevolveOpts;
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendTorus(
			DynMesh, PrimOpts, Identity, RevolveOpts, 50, 25, 16, 8, EGeometryScriptPrimitiveOriginMode::Center);
	}
	else if (TypeLower == TEXT("plane"))
	{
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendDisc(
			DynMesh, PrimOpts, Identity, 100, 16, 0, 0, 360, 0);
	}
	else
	{
		OutError = FString::Printf(
			TEXT("Unknown primitive type '%s'. Valid types: box, sphere, cylinder, cone, torus, plane"), *PrimitiveType);
		return nullptr;
	}

	return DynMesh;
}

UDynamicMesh* UMonolithMeshHandlePool::CreateFromAsset(const FString& AssetPath, FString& OutError)
{
	UStaticMesh* StaticMesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
	if (!StaticMesh)
	{
		OutError = FString::Printf(TEXT("Could not load StaticMesh at '%s'"), *AssetPath);
		return nullptr;
	}

	// Get LOD0 MeshDescription
	FMeshDescription* MeshDesc = StaticMesh->GetMeshDescription(0);
	if (!MeshDesc)
	{
		OutError = FString::Printf(TEXT("StaticMesh '%s' has no LOD0 MeshDescription"), *AssetPath);
		return nullptr;
	}

	// Convert to DynamicMesh
	FDynamicMesh3 DynMesh3;
	FMeshDescriptionToDynamicMesh Converter;
	Converter.Convert(MeshDesc, DynMesh3);

	UDynamicMesh* DynMesh = NewObject<UDynamicMesh>(this);
	if (!DynMesh)
	{
		OutError = TEXT("Failed to allocate UDynamicMesh");
		return nullptr;
	}

	DynMesh->SetMesh(MoveTemp(DynMesh3));
	return DynMesh;
}

#endif // WITH_GEOMETRYSCRIPT
