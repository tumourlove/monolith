#include "Actions/ProjectFindByTypeAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

FMonolithActionResult FProjectFindByTypeAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetClass = Params->GetStringField(TEXT("asset_type"));
	if (AssetClass.IsEmpty())
	{
		AssetClass = Params->GetStringField(TEXT("asset_class"));
	}
	int32 Limit = Params->HasField(TEXT("limit")) ? Params->GetIntegerField(TEXT("limit")) : 100;
	int32 Offset = Params->HasField(TEXT("offset")) ? Params->GetIntegerField(TEXT("offset")) : 0;
	FString ModuleFilter;
	if (Params->HasField(TEXT("module")))
	{
		ModuleFilter = Params->GetStringField(TEXT("module"));
	}

	if (AssetClass.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'asset_type' (or 'asset_class') parameter is required"), -32602);
	}

	UMonolithIndexSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
	if (!Subsystem)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem not available"));
	}

	TArray<FIndexedAsset> Assets = Subsystem->FindByType(AssetClass, Limit, Offset);

	if (!ModuleFilter.IsEmpty())
	{
		Assets.RemoveAll([&ModuleFilter](const FIndexedAsset& A) { return A.ModuleName != ModuleFilter; });
		if (Assets.Num() > Limit)
		{
			Assets.SetNum(Limit);
		}
	}

	auto Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> AssetsArr;
	for (const FIndexedAsset& Asset : Assets)
	{
		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("package_path"), Asset.PackagePath);
		Entry->SetStringField(TEXT("asset_name"), Asset.AssetName);
		Entry->SetStringField(TEXT("asset_class"), Asset.AssetClass);
		Entry->SetStringField(TEXT("module_name"), Asset.ModuleName);
		Entry->SetNumberField(TEXT("file_size_bytes"), Asset.FileSizeBytes);
		Entry->SetStringField(TEXT("indexed_at"), Asset.IndexedAt);
		AssetsArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("assets"), AssetsArr);
	Result->SetNumberField(TEXT("count"), Assets.Num());
	Result->SetNumberField(TEXT("offset"), Offset);
	Result->SetNumberField(TEXT("limit"), Limit);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectFindByTypeAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("asset_type"), TEXT("string"), TEXT("Asset class name (e.g. Blueprint, Material, StaticMesh, Texture2D)"))
		.Optional(TEXT("module"), TEXT("string"), TEXT("Filter by plugin/module name (e.g. ExampleInventory)"))
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum results"), TEXT("100"))
		.Optional(TEXT("offset"), TEXT("integer"), TEXT("Pagination offset"), TEXT("0"))
		.Build();
}
