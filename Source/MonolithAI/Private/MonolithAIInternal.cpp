#include "MonolithAIInternal.h"
#include "MonolithPackagePathValidator.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"

namespace MonolithAI
{

UBlackboardData* LoadBlackboardFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
{
	OutAssetPath = Params->GetStringField(TEXT("asset_path"));
	if (OutAssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required parameter: asset_path");
		return nullptr;
	}

	UObject* Obj = StaticLoadObject(UBlackboardData::StaticClass(), nullptr, *OutAssetPath);
	if (UBlackboardData* BB = Cast<UBlackboardData>(Obj))
	{
		return BB;
	}

	OutError = FString::Printf(TEXT("Blackboard not found: %s"), *OutAssetPath);
	return nullptr;
}

UBehaviorTree* LoadBehaviorTreeFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
{
	OutAssetPath = Params->GetStringField(TEXT("asset_path"));
	if (OutAssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required parameter: asset_path");
		return nullptr;
	}

	UObject* Obj = StaticLoadObject(UBehaviorTree::StaticClass(), nullptr, *OutAssetPath);
	if (UBehaviorTree* BT = Cast<UBehaviorTree>(Obj))
	{
		return BT;
	}

	OutError = FString::Printf(TEXT("BehaviorTree not found: %s"), *OutAssetPath);
	return nullptr;
}

UBlueprint* LoadAIControllerFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
{
	OutAssetPath = Params->GetStringField(TEXT("asset_path"));
	if (OutAssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required parameter: asset_path");
		return nullptr;
	}

	UObject* Obj = StaticLoadObject(UBlueprint::StaticClass(), nullptr, *OutAssetPath);
	UBlueprint* BP = Cast<UBlueprint>(Obj);
	if (!BP)
	{
		OutError = FString::Printf(TEXT("AIController Blueprint not found: %s"), *OutAssetPath);
		return nullptr;
	}

	if (!BP->GeneratedClass || !BP->GeneratedClass->IsChildOf(AAIController::StaticClass()))
	{
		OutError = FString::Printf(TEXT("'%s' is not an AIController Blueprint"), *OutAssetPath);
		return nullptr;
	}

	return BP;
}

UObject* LoadAssetFromPath(const FString& AssetPath, FString& OutError)
{
	UObject* Obj = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (!Obj)
	{
		OutError = FString::Printf(TEXT("Asset not found: %s"), *AssetPath);
	}
	return Obj;
}

UPackage* GetOrCreatePackage(const FString& SavePath, FString& OutError)
{
	FString PackageName = SavePath;
	if (PackageName.StartsWith(TEXT("/Game/")))
	{
		// Already in game content format
	}
	else if (!PackageName.StartsWith(TEXT("/")))
	{
		PackageName = TEXT("/Game/") + PackageName;
	}

	// Defensive: reject malformed paths (e.g. "//Game/...") before CreatePackage asserts and kills the editor.
	if (const FString ValidationError = MonolithCore::ValidatePackagePath(PackageName); !ValidationError.IsEmpty())
	{
		UE_LOG(LogMonolithAI, Warning, TEXT("GetOrCreatePackage rejected path: %s"), *ValidationError);
		OutError = ValidationError;
		return nullptr;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Failed to create package: %s"), *PackageName);
		return nullptr;
	}
	Package->FullyLoad();
	return Package;
}

bool RequireStringParam(const TSharedPtr<FJsonObject>& Params, const FString& ParamName, FString& OutValue, FMonolithActionResult& OutError)
{
	OutValue = Params->GetStringField(ParamName);
	if (OutValue.IsEmpty())
	{
		OutError = FMonolithActionResult::Error(
			FString::Printf(TEXT("Missing required parameter: %s"), *ParamName));
		return false;
	}
	return true;
}

bool EnsureAssetPathFree(const FString& PackagePath, const FString& AssetName, FString& OutError)
{
	FString FullObjectPath = PackagePath + TEXT(".") + AssetName;

	// Tier 1: Asset Registry
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData ExistingAsset = AR.GetAssetByObjectPath(FSoftObjectPath(FullObjectPath));
	if (ExistingAsset.IsValid())
	{
		OutError = FString::Printf(TEXT("Asset already exists at '%s'. Delete it first or use a different path."), *PackagePath);
		return false;
	}

	// Tier 2: FindObject global
	if (FindObject<UObject>(nullptr, *FullObjectPath))
	{
		OutError = FString::Printf(TEXT("Asset already exists in memory at '%s'. Delete it first or use a different path."), *PackagePath);
		return false;
	}

	// Tier 3: FindPackage + FindObject scoped
	UPackage* ExistingPkg = FindPackage(nullptr, *PackagePath);
	if (ExistingPkg && FindObject<UObject>(ExistingPkg, *AssetName))
	{
		OutError = FString::Printf(TEXT("Asset already exists in package '%s'. Delete it first or use a different path."), *PackagePath);
		return false;
	}

	return true;
}

TSharedPtr<FJsonObject> MakeAssetResult(const FString& AssetPath, const FString& Message)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	if (!Message.IsEmpty())
	{
		Result->SetStringField(TEXT("message"), Message);
	}
	return Result;
}

TArray<FString> ParseStringArray(const TSharedPtr<FJsonObject>& Params, const FString& FieldName)
{
	TArray<FString> Result;
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (Params->TryGetArrayField(FieldName, Arr))
	{
		for (const auto& Val : *Arr)
		{
			FString Str;
			if (Val->TryGetString(Str))
			{
				Result.Add(Str);
			}
		}
	}
	return Result;
}

UWorld* GetPIEWorld()
{
	if (!GEngine) return nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.World())
		{
			return Context.World();
		}
	}
	return nullptr;
}

AActor* FindActorInPIE(const FString& ActorIdentifier)
{
	UWorld* World = GetPIEWorld();
	if (!World) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->GetActorLabel() == ActorIdentifier ||
			It->GetName() == ActorIdentifier ||
			It->GetPathName() == ActorIdentifier)
		{
			return *It;
		}
	}
	return nullptr;
}

FString GetProjectSourceDir()
{
	return FPaths::ProjectDir() / TEXT("Source") / FApp::GetProjectName();
}

} // namespace MonolithAI
