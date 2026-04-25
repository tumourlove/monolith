#include "MonolithGASInternal.h"
#include "MonolithPackagePathValidator.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Abilities/GameplayAbility.h"
#include "AbilitySystemInterface.h"
#include "AttributeSet.h"
#include "GameplayEffect.h"
#include "EngineUtils.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"

namespace MonolithGAS
{

UBlueprint* LoadBlueprintFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
{
	OutAssetPath = Params->GetStringField(TEXT("asset_path"));
	if (OutAssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required parameter: asset_path");
		return nullptr;
	}

	// Try loading as Blueprint first
	FString FullPath = OutAssetPath;
	if (!FullPath.EndsWith(TEXT("_C")))
	{
		UObject* Obj = StaticLoadObject(UBlueprint::StaticClass(), nullptr, *FullPath);
		if (UBlueprint* BP = Cast<UBlueprint>(Obj))
		{
			return BP;
		}
	}

	OutError = FString::Printf(TEXT("Blueprint not found: %s"), *OutAssetPath);
	return nullptr;
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
		UE_LOG(LogMonolithGAS, Warning, TEXT("GetOrCreatePackage rejected path: %s"), *ValidationError);
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

FString TagToString(const FGameplayTag& Tag)
{
	return Tag.IsValid() ? Tag.ToString() : TEXT("");
}

FGameplayTag StringToTag(const FString& TagString)
{
	if (TagString.IsEmpty())
	{
		return FGameplayTag();
	}
	return FGameplayTag::RequestGameplayTag(FName(*TagString), false);
}

FGameplayTagContainer ParseTagContainer(const TSharedPtr<FJsonObject>& Params, const FString& FieldName)
{
	FGameplayTagContainer Container;
	const TArray<TSharedPtr<FJsonValue>>* TagArray;
	if (Params->TryGetArrayField(FieldName, TagArray))
	{
		for (const auto& Val : *TagArray)
		{
			FString TagStr;
			if (Val->TryGetString(TagStr) && !TagStr.IsEmpty())
			{
				FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagStr), false);
				if (Tag.IsValid())
				{
					Container.AddTag(Tag);
				}
			}
		}
	}
	return Container;
}

TSharedPtr<FJsonValue> TagContainerToJson(const FGameplayTagContainer& Container)
{
	TArray<TSharedPtr<FJsonValue>> TagArray;
	for (const FGameplayTag& Tag : Container)
	{
		TagArray.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}
	return MakeShared<FJsonValueArray>(TagArray);
}

bool IsAbilityBlueprint(UBlueprint* BP)
{
	if (!BP || !BP->GeneratedClass)
	{
		return false;
	}
	return BP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass());
}

bool IsAttributeSetBlueprint(UBlueprint* BP)
{
	if (!BP || !BP->GeneratedClass)
	{
		return false;
	}
	return BP->GeneratedClass->IsChildOf(UAttributeSet::StaticClass());
}

bool IsGameplayEffectBlueprint(UBlueprint* BP)
{
	if (!BP || !BP->GeneratedClass)
	{
		return false;
	}
	return BP->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass());
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

// ---------------------------------------------------------------------------
// Asset Existence Guard
// ---------------------------------------------------------------------------

bool EnsureAssetPathFree(const FString& PackagePath, const FString& AssetName, FString& OutError)
{
	FString FullObjectPath = PackagePath + TEXT(".") + AssetName;

	// Tier 1: Asset Registry (catches on-disk assets without loading them)
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData ExistingAsset = AR.GetAssetByObjectPath(FSoftObjectPath(FullObjectPath));
	if (ExistingAsset.IsValid())
	{
		OutError = FString::Printf(TEXT("Asset already exists at '%s'. Delete it first or use a different path."), *PackagePath);
		return false;
	}

	// Tier 2: FindObject global (catches in-memory assets not yet in AR)
	if (FindObject<UObject>(nullptr, *FullObjectPath))
	{
		OutError = FString::Printf(TEXT("Asset already exists in memory at '%s'. Delete it first or use a different path."), *PackagePath);
		return false;
	}

	// Tier 3: FindPackage + FindObject scoped (edge case: package loaded but object path didn't match)
	UPackage* ExistingPkg = FindPackage(nullptr, *PackagePath);
	if (ExistingPkg && FindObject<UObject>(ExistingPkg, *AssetName))
	{
		OutError = FString::Printf(TEXT("Asset already exists in package '%s'. Delete it first or use a different path."), *PackagePath);
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------
// PIE Runtime Helpers (A2)
// ---------------------------------------------------------------------------

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

UAbilitySystemComponent* GetASCFromActor(AActor* Actor)
{
	if (!Actor) return nullptr;
	if (IAbilitySystemInterface* ASI = Cast<IAbilitySystemInterface>(Actor))
	{
		return ASI->GetAbilitySystemComponent();
	}
	return Actor->FindComponentByClass<UAbilitySystemComponent>();
}

// ---------------------------------------------------------------------------
// GE Load Helper (A3)
// ---------------------------------------------------------------------------

bool LoadGameplayEffectBP(const FString& Path, UBlueprint*& OutBP, UGameplayEffect*& OutGE, FString& OutError)
{
	OutBP = nullptr;
	OutGE = nullptr;

	UObject* Obj = LoadAssetFromPath(Path, OutError);
	OutBP = Cast<UBlueprint>(Obj);
	if (!OutBP)
	{
		OutError = FString::Printf(TEXT("Failed to load GameplayEffect Blueprint: %s — %s"), *Path, *OutError);
		return false;
	}
	if (!IsGameplayEffectBlueprint(OutBP))
	{
		OutError = FString::Printf(TEXT("'%s' is not a GameplayEffect Blueprint"), *Path);
		return false;
	}
	OutGE = GetBlueprintCDO<UGameplayEffect>(OutBP);
	if (!OutGE)
	{
		OutError = FString::Printf(TEXT("Failed to get CDO for GameplayEffect: %s"), *Path);
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Project Source Helper (A4)
// ---------------------------------------------------------------------------

FString GetProjectSourceDir()
{
	return FPaths::ProjectDir() / TEXT("Source") / FApp::GetProjectName();
}

} // namespace MonolithGAS
