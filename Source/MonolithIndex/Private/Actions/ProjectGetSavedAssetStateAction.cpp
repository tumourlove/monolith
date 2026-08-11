#include "Actions/ProjectGetSavedAssetStateAction.h"
#include "AssetRegistry/AssetData.h"
#include "MonolithParamSchema.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"

FMonolithActionResult FProjectGetSavedAssetStateAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePath = Params->GetStringField(TEXT("asset_path"));
	if (PackagePath.IsEmpty())
	{
		PackagePath = Params->GetStringField(TEXT("package_path"));
	}
	if (PackagePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'asset_path' (or 'package_path') parameter is required"), -32602);
	}
	PackagePath.TrimStartAndEndInline();

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	const FName PackageName(*PackagePath);

	// Resolve class + asset name from the registry. A package can hold more than
	// one asset; report the primary asset (skip redirectors when an alternative exists).
	TArray<FAssetData> AssetsInPackage;
	AR.GetAssetsByPackageName(PackageName, AssetsInPackage, /*bIncludeOnlyOnDiskAssets=*/false);

	FString ClassName;
	FString AssetName;
	if (AssetsInPackage.Num() > 0)
	{
		const FAssetData* Primary = &AssetsInPackage[0];
		for (const FAssetData& Candidate : AssetsInPackage)
		{
			if (Candidate.AssetClassPath.GetAssetName() != FName(TEXT("ObjectRedirector")))
			{
				Primary = &Candidate;
				break;
			}
		}
		ClassName = Primary->AssetClassPath.GetAssetName().ToString();
		AssetName = Primary->AssetName.ToString();
	}

	// Resolve disk path (.uasset or .umap) and gather size + mtime.
	FString DiskPath;
	int64 FileSize = -1;
	FDateTime TimeStamp = FDateTime::MinValue();
	bool bExistsOnDisk = false;

	FString Candidate;
	if (FPackageName::TryConvertLongPackageNameToFilename(PackagePath, Candidate, FPackageName::GetAssetPackageExtension())
		&& IFileManager::Get().FileExists(*Candidate))
	{
		DiskPath = Candidate;
	}
	else if (FPackageName::TryConvertLongPackageNameToFilename(PackagePath, Candidate, FPackageName::GetMapPackageExtension())
		&& IFileManager::Get().FileExists(*Candidate))
	{
		DiskPath = Candidate;
	}

	if (!DiskPath.IsEmpty())
	{
		bExistsOnDisk = true;
		FileSize = IFileManager::Get().FileSize(*DiskPath);
		TimeStamp = IFileManager::Get().GetTimeStamp(*DiskPath);
	}

	if (ClassName.IsEmpty() && !bExistsOnDisk)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' not found in registry or on disk"), *PackagePath));
	}

	// Dependencies + referencers (package-level, on-disk references only).
	TArray<FName> Dependencies;
	AR.GetDependencies(PackageName, Dependencies, UE::AssetRegistry::EDependencyCategory::Package);
	TArray<FName> Referencers;
	AR.GetReferencers(PackageName, Referencers, UE::AssetRegistry::EDependencyCategory::Package);

	auto ToStringArray = [](const TArray<FName>& Names)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Names.Num());
		for (const FName& N : Names)
		{
			Out.Add(MakeShared<FJsonValueString>(N.ToString()));
		}
		return Out;
	};

	auto State = MakeShared<FJsonObject>();
	State->SetStringField(TEXT("package_path"), PackagePath);
	State->SetStringField(TEXT("asset_name"), AssetName);
	State->SetStringField(TEXT("class"), ClassName);
	State->SetBoolField(TEXT("exists_on_disk"), bExistsOnDisk);
	State->SetStringField(TEXT("disk_path"), DiskPath);
	State->SetNumberField(TEXT("file_size"), static_cast<double>(FileSize));
	State->SetStringField(TEXT("modified_time_utc"), bExistsOnDisk ? TimeStamp.ToIso8601() : FString());
	State->SetArrayField(TEXT("dependencies"), ToStringArray(Dependencies));
	State->SetArrayField(TEXT("referencers"), ToStringArray(Referencers));

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetObjectField(TEXT("asset_state"), State);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectGetSavedAssetStateAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Package path of the asset (e.g. /Game/Tests/Monolith/Foo)"))
		.Build();
}
