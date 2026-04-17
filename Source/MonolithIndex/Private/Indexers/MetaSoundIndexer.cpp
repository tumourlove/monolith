#include "Indexers/MetaSoundIndexer.h"
#include "MonolithSettings.h"
#include "MonolithMemoryHelper.h"
#include "MetasoundSource.h"
#include "Metasound.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetCompilingManager.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	FString ResolveMetaSoundClassName(const FGuid& ClassID, const FMetasoundFrontendDocument& Doc)
	{
		for (const FMetasoundFrontendClass& DepClass : Doc.Dependencies)
		{
			if (DepClass.ID == ClassID)
			{
				return DepClass.Metadata.GetClassName().ToString();
			}
		}

		if (Doc.RootGraph.ID == ClassID)
		{
			return Doc.RootGraph.Metadata.GetClassName().ToString();
		}

		for (const FMetasoundFrontendGraphClass& Subgraph : Doc.Subgraphs)
		{
			if (Subgraph.ID == ClassID)
			{
				return Subgraph.Metadata.GetClassName().ToString();
			}
		}

		return ClassID.ToString();
	}
}

bool FMetaSoundIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetData> MetaSoundAssets;
	FARFilter Filter;
	for (const FName& ContentPath : UMonolithSettings::GetIndexedContentPaths())
	{
		Filter.PackagePaths.Add(ContentPath);
	}
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UMetaSoundSource::StaticClass()->GetClassPathName());
	Filter.ClassPaths.Add(UMetaSoundPatch::StaticClass()->GetClassPathName());
	Registry.GetAssets(Filter, MetaSoundAssets);

	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	const int32 BatchSize = FMath::Max(1, Settings->PostPassBatchSize);
	const SIZE_T MemoryBudgetMB = static_cast<SIZE_T>(Settings->MemoryBudgetMB);
	const bool bLogMemory = Settings->bLogMemoryStats;

	UE_LOG(LogMonolithIndex, Log, TEXT("MetaSoundIndexer: Found %d MetaSound assets to index (batch size: %d)"),
		MetaSoundAssets.Num(), BatchSize);

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("MetaSoundIndexer start"));
	}

	int32 AssetsIndexed = 0;
	int32 BatchNumber = 0;

	for (int32 i = 0; i < MetaSoundAssets.Num(); i += BatchSize)
	{
		FAssetCompilingManager::Get().FinishAllCompilation();

		if (FMonolithMemoryHelper::ShouldThrottle(MemoryBudgetMB))
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("MetaSoundIndexer: Memory budget exceeded, forcing GC..."));
			FMonolithMemoryHelper::ForceGarbageCollection(true);
			FMonolithMemoryHelper::YieldToEditor();

			if (bLogMemory)
			{
				FMonolithMemoryHelper::LogMemoryStats(TEXT("MetaSoundIndexer after throttle GC"));
			}
		}

		int32 BatchEnd = FMath::Min(i + BatchSize, MetaSoundAssets.Num());

		for (int32 j = i; j < BatchEnd; ++j)
		{
			const FAssetData& MSAssetData = MetaSoundAssets[j];

			int64 MSAssetId = DB.GetAssetId(MSAssetData.PackageName.ToString());
			if (MSAssetId < 0) continue;

			UObject* MetaSound = MSAssetData.GetAsset();
			if (!MetaSound) continue;

			IndexMetaSound(MetaSound, DB, MSAssetId);
			AssetsIndexed++;

			FMonolithMemoryHelper::TryUnloadPackage(MetaSound);
		}

		BatchNumber++;

		FMonolithMemoryHelper::ForceGarbageCollection(false);
		FMonolithMemoryHelper::YieldToEditor();

		if (BatchNumber % 5 == 0 || BatchEnd == MetaSoundAssets.Num())
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("MetaSoundIndexer: processed %d / %d assets"),
				BatchEnd, MetaSoundAssets.Num());

			if (bLogMemory)
			{
				FMonolithMemoryHelper::LogMemoryStats(FString::Printf(TEXT("MetaSoundIndexer batch %d"), BatchNumber));
			}
		}
	}

	FMonolithMemoryHelper::ForceGarbageCollection(true);

	UE_LOG(LogMonolithIndex, Log, TEXT("MetaSoundIndexer: indexed %d MetaSound assets"), AssetsIndexed);

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("MetaSoundIndexer complete"));
	}

	return true;
}

void FMetaSoundIndexer::IndexMetaSound(UObject* MetaSoundAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!MetaSoundAsset) return;

	IMetaSoundDocumentInterface* DocInterface = Cast<IMetaSoundDocumentInterface>(MetaSoundAsset);
	if (!DocInterface) return;

	const FMetasoundFrontendDocument& Doc = DocInterface->GetConstDocument();
	const FMetasoundFrontendGraph& Graph = Doc.RootGraph.GetConstDefaultGraph();

	FString MetaSoundType = Cast<UMetaSoundSource>(MetaSoundAsset) ? TEXT("Source") : TEXT("Patch");

	auto AssetProps = MakeShared<FJsonObject>();
	AssetProps->SetStringField(TEXT("type"), MetaSoundType);
	AssetProps->SetStringField(TEXT("root_class"), Doc.RootGraph.Metadata.GetClassName().ToString());
	AssetProps->SetNumberField(TEXT("node_count"), Graph.Nodes.Num());
	AssetProps->SetNumberField(TEXT("edge_count"), Graph.Edges.Num());
	AssetProps->SetNumberField(TEXT("variable_count"), Graph.Variables.Num());
	const FMetasoundFrontendClassInterface& InterfaceRef = Doc.RootGraph.GetDefaultInterface();
	AssetProps->SetNumberField(TEXT("input_count"), InterfaceRef.Inputs.Num());
	AssetProps->SetNumberField(TEXT("output_count"), InterfaceRef.Outputs.Num());
	AssetProps->SetNumberField(TEXT("dependency_count"), Doc.Dependencies.Num());

	TArray<TSharedPtr<FJsonValue>> InputNames;
	for (const FMetasoundFrontendClassInput& Input : Doc.RootGraph.Interface.Inputs)
	{
		InputNames.Add(MakeShared<FJsonValueString>(Input.Name.ToString()));
	}
	AssetProps->SetArrayField(TEXT("inputs"), InputNames);

	TArray<TSharedPtr<FJsonValue>> OutputNames;
	for (const FMetasoundFrontendClassOutput& Output : Doc.RootGraph.Interface.Outputs)
	{
		OutputNames.Add(MakeShared<FJsonValueString>(Output.Name.ToString()));
	}
	AssetProps->SetArrayField(TEXT("outputs"), OutputNames);

	FString AssetPropsStr;
	{
		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&AssetPropsStr);
		FJsonSerializer::Serialize(AssetProps, *Writer, true);
	}

	FIndexedNode AssetNode;
	AssetNode.AssetId = AssetId;
	AssetNode.NodeName = MetaSoundAsset->GetName();
	AssetNode.NodeClass = TEXT("MetaSound");
	AssetNode.NodeType = MetaSoundType;
	AssetNode.Properties = AssetPropsStr;
	DB.InsertNode(AssetNode);

	TMap<FGuid, int64> NodeIdToRowId;

	for (const FMetasoundFrontendNode& Node : Graph.Nodes)
	{
		FString ClassName = ResolveMetaSoundClassName(Node.ClassID, Doc);

		auto NodeProps = MakeShared<FJsonObject>();
		NodeProps->SetStringField(TEXT("class_id"), Node.ClassID.ToString());
		NodeProps->SetStringField(TEXT("class_name"), ClassName);
		NodeProps->SetNumberField(TEXT("input_count"), Node.Interface.Inputs.Num());
		NodeProps->SetNumberField(TEXT("output_count"), Node.Interface.Outputs.Num());
		NodeProps->SetNumberField(TEXT("literal_count"), Node.InputLiterals.Num());

		FString NodePropsStr;
		{
			auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&NodePropsStr);
			FJsonSerializer::Serialize(NodeProps, *Writer, true);
		}

		FIndexedNode IndexedNode;
		IndexedNode.AssetId = AssetId;
		IndexedNode.NodeName = Node.Name.ToString();
		IndexedNode.NodeClass = ClassName;
		IndexedNode.NodeType = TEXT("MetaSoundNode");
		IndexedNode.Properties = NodePropsStr;

		int64 RowId = DB.InsertNode(IndexedNode);
		NodeIdToRowId.Add(Node.GetID(), RowId);
	}

	for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
	{
		int64* FromRowId = NodeIdToRowId.Find(Edge.FromNodeID);
		int64* ToRowId = NodeIdToRowId.Find(Edge.ToNodeID);

		if (!FromRowId || !ToRowId) continue;

		FString FromPinName;
		FString ToPinName;
		FString EdgeType;

		for (const FMetasoundFrontendNode& Node : Graph.Nodes)
		{
			if (Node.GetID() == Edge.FromNodeID)
			{
				for (const FMetasoundFrontendVertex& V : Node.Interface.Outputs)
				{
					if (V.VertexID == Edge.FromVertexID)
					{
						FromPinName = V.Name.ToString();
						EdgeType = V.TypeName.ToString();
						break;
					}
				}
			}
			if (Node.GetID() == Edge.ToNodeID)
			{
				for (const FMetasoundFrontendVertex& V : Node.Interface.Inputs)
				{
					if (V.VertexID == Edge.ToVertexID)
					{
						ToPinName = V.Name.ToString();
						break;
					}
				}
			}
		}

		FIndexedConnection Conn;
		Conn.SourceNodeId = *FromRowId;
		Conn.SourcePin = FromPinName;
		Conn.TargetNodeId = *ToRowId;
		Conn.TargetPin = ToPinName;
		Conn.PinType = EdgeType;
		DB.InsertConnection(Conn);
	}
}
