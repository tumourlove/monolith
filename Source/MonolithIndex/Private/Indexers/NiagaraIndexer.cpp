#include "Indexers/NiagaraIndexer.h"
#include "MonolithSettings.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraRendererProperties.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

bool FNiagaraIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetData> NiagaraAssets;
	FARFilter Filter;
	for (const FName& ContentPath : UMonolithSettings::GetIndexedContentPaths())
	{
		Filter.PackagePaths.Add(ContentPath);
	}
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UNiagaraSystem::StaticClass()->GetClassPathName());
	Registry.GetAssets(Filter, NiagaraAssets);

	int32 SystemsIndexed = 0;

	for (const FAssetData& NiagaraAssetData : NiagaraAssets)
	{
		int64 NiagaraAssetId = DB.GetAssetId(NiagaraAssetData.PackageName.ToString());
		if (NiagaraAssetId < 0) continue;

		UNiagaraSystem* System = Cast<UNiagaraSystem>(NiagaraAssetData.GetAsset());
		if (!System) continue;

		IndexNiagaraSystem(System, DB, NiagaraAssetId);
		SystemsIndexed++;
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("NiagaraIndexer: indexed %d Niagara systems"), SystemsIndexed);
	return true;
}

void FNiagaraIndexer::IndexNiagaraSystem(UNiagaraSystem* System, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!System) return;

	const TArray<FNiagaraEmitterHandle>& EmitterHandles = System->GetEmitterHandles();

	// Build system-level metadata
	auto SystemProps = MakeShared<FJsonObject>();
	SystemProps->SetBoolField(TEXT("has_fixed_bounds"), System->bFixedBounds);
	if (System->bFixedBounds)
	{
		const FBox& Bounds = System->GetFixedBounds();
		SystemProps->SetStringField(TEXT("fixed_bounds_min"), Bounds.Min.ToString());
		SystemProps->SetStringField(TEXT("fixed_bounds_max"), Bounds.Max.ToString());
	}
	SystemProps->SetNumberField(TEXT("emitter_count"), EmitterHandles.Num());
	SystemProps->SetBoolField(TEXT("is_valid"), System->IsValid());

	TArray<TSharedPtr<FJsonValue>> EmitterNames;
	for (const FNiagaraEmitterHandle& Handle : EmitterHandles)
	{
		EmitterNames.Add(MakeShared<FJsonValueString>(Handle.GetName().ToString()));
	}
	SystemProps->SetArrayField(TEXT("emitter_names"), EmitterNames);

	// Serialize system-level node
	FString SystemPropsStr;
	{
		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&SystemPropsStr);
		FJsonSerializer::Serialize(SystemProps, *Writer, true);
	}

	FIndexedNode SystemNode;
	SystemNode.AssetId = AssetId;
	SystemNode.NodeName = System->GetName();
	SystemNode.NodeClass = TEXT("NiagaraSystem");
	SystemNode.NodeType = TEXT("System");
	SystemNode.Properties = SystemPropsStr;
	DB.InsertNode(SystemNode);

	// Index each emitter
	for (const FNiagaraEmitterHandle& Handle : EmitterHandles)
	{
		auto EmitterProps = MakeShared<FJsonObject>();
		EmitterProps->SetStringField(TEXT("name"), Handle.GetName().ToString());
		EmitterProps->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());

		FVersionedNiagaraEmitter VersionedEmitter = Handle.GetInstance();
		if (VersionedEmitter.Emitter)
		{
			FVersionedNiagaraEmitterData* EmitterData = VersionedEmitter.GetEmitterData();
			if (EmitterData)
			{
				// Sim target
				switch (EmitterData->SimTarget)
				{
				case ENiagaraSimTarget::CPUSim:
					EmitterProps->SetStringField(TEXT("sim_target"), TEXT("CPU"));
					break;
				case ENiagaraSimTarget::GPUComputeSim:
					EmitterProps->SetStringField(TEXT("sim_target"), TEXT("GPU"));
					break;
				default:
					EmitterProps->SetStringField(TEXT("sim_target"), TEXT("Unknown"));
					break;
				}

				// Renderer info
				const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
				TArray<TSharedPtr<FJsonValue>> RendererArray;
				for (const UNiagaraRendererProperties* Renderer : Renderers)
				{
					if (Renderer)
					{
						RendererArray.Add(MakeShared<FJsonValueString>(Renderer->GetClass()->GetName()));
					}
				}
				EmitterProps->SetArrayField(TEXT("renderers"), RendererArray);

				// Script presence per stage
				EmitterProps->SetBoolField(TEXT("has_spawn_script"), EmitterData->SpawnScriptProps.Script != nullptr);
				EmitterProps->SetBoolField(TEXT("has_update_script"), EmitterData->UpdateScriptProps.Script != nullptr);
			}
		}

		// Serialize emitter node
		FString EmitterPropsStr;
		{
			auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&EmitterPropsStr);
			FJsonSerializer::Serialize(EmitterProps, *Writer, true);
		}

		FIndexedNode EmitterNode;
		EmitterNode.AssetId = AssetId;
		EmitterNode.NodeName = Handle.GetName().ToString();
		EmitterNode.NodeClass = TEXT("NiagaraEmitter");
		EmitterNode.NodeType = TEXT("Emitter");
		EmitterNode.Properties = EmitterPropsStr;
		DB.InsertNode(EmitterNode);
	}
}
