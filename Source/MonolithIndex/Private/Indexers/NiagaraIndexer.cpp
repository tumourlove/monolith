#include "Indexers/NiagaraIndexer.h"
#include "MonolithSettings.h"
#include "MonolithMemoryHelper.h"
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

	// Get settings for batching
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	const int32 BatchSize = FMath::Max(1, FMonolithMemoryHelper::GetResolvedPostPassBatchSize());
	const SIZE_T MemoryBudgetMB = static_cast<SIZE_T>(FMonolithMemoryHelper::GetResolvedMemoryBudgetMB());
	const bool bLogMemory = Settings->bLogMemoryStats;

	UE_LOG(LogMonolithIndex, Log, TEXT("NiagaraIndexer: Found %d Niagara systems to index (batch size: %d)"),
		NiagaraAssets.Num(), BatchSize);

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("NiagaraIndexer start"));
	}

	int32 SystemsIndexed = 0;
	int32 BatchNumber = 0;

	for (int32 i = 0; i < NiagaraAssets.Num(); i += BatchSize)
	{
		// Compiler-idle gate is enforced by FMonolithCompilerSafeDispatch at the call site (see issue #19).

		// Memory budget check before each batch
		if (FMonolithMemoryHelper::ShouldThrottle(MemoryBudgetMB))
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("NiagaraIndexer: Memory budget exceeded, forcing GC..."));
			FMonolithMemoryHelper::ForceGarbageCollection(true);
			FMonolithMemoryHelper::YieldToEditor();

			if (bLogMemory)
			{
				FMonolithMemoryHelper::LogMemoryStats(TEXT("NiagaraIndexer after throttle GC"));
			}
		}

		int32 BatchEnd = FMath::Min(i + BatchSize, NiagaraAssets.Num());

		// Process batch
		for (int32 j = i; j < BatchEnd; ++j)
		{
			const FAssetData& NiagaraAssetData = NiagaraAssets[j];

			int64 NiagaraAssetId = DB.GetAssetId(NiagaraAssetData.PackageName.ToString());
			if (NiagaraAssetId < 0) continue;

			UNiagaraSystem* System = Cast<UNiagaraSystem>(NiagaraAssetData.GetAsset());
			if (!System) continue;

			IndexNiagaraSystem(System, DB, NiagaraAssetId);
			SystemsIndexed++;

			// Mark for unloading
			FMonolithMemoryHelper::TryUnloadPackage(System);
		}

		BatchNumber++;

		// GC after each batch
		FMonolithMemoryHelper::ForceGarbageCollection(false);
		FMonolithMemoryHelper::YieldToEditor();

		// Log progress periodically
		if (BatchNumber % 5 == 0 || BatchEnd == NiagaraAssets.Num())
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("NiagaraIndexer: processed %d / %d systems"),
				BatchEnd, NiagaraAssets.Num());

			if (bLogMemory)
			{
				FMonolithMemoryHelper::LogMemoryStats(FString::Printf(TEXT("NiagaraIndexer batch %d"), BatchNumber));
			}
		}
	}

	// Final GC
	FMonolithMemoryHelper::ForceGarbageCollection(true);

	UE_LOG(LogMonolithIndex, Log, TEXT("NiagaraIndexer: indexed %d Niagara systems"), SystemsIndexed);

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("NiagaraIndexer complete"));
	}

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
