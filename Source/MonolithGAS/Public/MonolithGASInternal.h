#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "GameplayAbilitySpec.h"
#include "GameplayEffect.h"
#include "AbilitySystemComponent.h"
#include "GameplayTagContainer.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMonolithGAS, Log, All);

namespace MonolithGAS
{
	// Load a Blueprint asset from the "asset_path" param. Returns nullptr + sets ErrorMsg on failure.
	UBlueprint* LoadBlueprintFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError);

	// Load any UObject asset from an asset path param
	UObject* LoadAssetFromPath(const FString& AssetPath, FString& OutError);

	// Get or create a package for asset creation
	UPackage* GetOrCreatePackage(const FString& SavePath, FString& OutError);

	// Convert a FGameplayTag to string
	FString TagToString(const FGameplayTag& Tag);

	// Parse a string into a FGameplayTag
	FGameplayTag StringToTag(const FString& TagString);

	// Parse a JSON array of strings into a FGameplayTagContainer
	FGameplayTagContainer ParseTagContainer(const TSharedPtr<FJsonObject>& Params, const FString& FieldName);

	// F.7b — Same as ParseTagContainer, but appends any string that fails FGameplayTag::RequestGameplayTag
	// to OutSkipped so callers can surface a "warnings" array on the response (model: LogicDriver set_node_tags).
	// Out-param contract: OutSkipped is append-only — caller is responsible for clearing if reused across fields.
	FGameplayTagContainer ParseTagContainer(const TSharedPtr<FJsonObject>& Params, const FString& FieldName,
		TArray<FString>& OutSkipped);

	// Convert a FGameplayTagContainer to a JSON array of strings
	TSharedPtr<FJsonValue> TagContainerToJson(const FGameplayTagContainer& Container);

	// Get CDO from a Blueprint, optionally cast to a specific type
	template<typename T>
	T* GetBlueprintCDO(UBlueprint* BP)
	{
		if (!BP || !BP->GeneratedClass)
		{
			return nullptr;
		}
		return Cast<T>(BP->GeneratedClass->GetDefaultObject());
	}

	// Check if a class inherits from UGameplayAbility
	bool IsAbilityBlueprint(UBlueprint* BP);

	// Check if a class inherits from UAttributeSet
	bool IsAttributeSetBlueprint(UBlueprint* BP);

	// Check if a class inherits from UGameplayEffect
	bool IsGameplayEffectBlueprint(UBlueprint* BP);

	// Build a standard success result with asset_path field
	TSharedPtr<FJsonObject> MakeAssetResult(const FString& AssetPath, const FString& Message = TEXT(""));

	// Parse a JSON array field into TArray<FString>
	TArray<FString> ParseStringArray(const TSharedPtr<FJsonObject>& Params, const FString& FieldName);

	// Validate required string param, return error result if missing
	bool RequireStringParam(const TSharedPtr<FJsonObject>& Params, const FString& ParamName, FString& OutValue, FMonolithActionResult& OutError);

	// ---------------------------------------------------------------------------
	// Asset Existence Guard (robust pre-check for create actions)
	// ---------------------------------------------------------------------------

	/**
	 * Check that no asset exists at the given path (on disk or in memory).
	 * Uses AssetRegistry (disk check) + FindObject (memory check).
	 * Returns true if path is free. Sets OutError if blocked.
	 */
	bool EnsureAssetPathFree(const FString& PackagePath, const FString& AssetName, FString& OutError);

	// ---------------------------------------------------------------------------
	// PIE Runtime Helpers (A2)
	// ---------------------------------------------------------------------------

	// Get the PIE world, if one exists
	UWorld* GetPIEWorld();

	// Find an actor in PIE by label, name, or path
	AActor* FindActorInPIE(const FString& ActorIdentifier);

	// Get ASC from an actor via IAbilitySystemInterface or component search fallback
	UAbilitySystemComponent* GetASCFromActor(AActor* Actor);

	// ---------------------------------------------------------------------------
	// GE Load Helper (A3)
	// ---------------------------------------------------------------------------

	// Load a GameplayEffect Blueprint and return the BP + CDO. Returns false + sets OutError on failure.
	bool LoadGameplayEffectBP(const FString& Path, UBlueprint*& OutBP, UGameplayEffect*& OutGE, FString& OutError);

	// ---------------------------------------------------------------------------
	// Project Source Helper (A4)
	// ---------------------------------------------------------------------------

	// Get the project Source directory path (e.g. "<ProjectDir>/Source/<ProjectName>")
	FString GetProjectSourceDir();
}
