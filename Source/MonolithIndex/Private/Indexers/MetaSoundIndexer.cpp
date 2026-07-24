#include "Indexers/MetaSoundIndexer.h"

#if WITH_METASOUND

#include "MonolithSettings.h"
#include "MonolithMemoryHelper.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Object.h"

// MetaSound runtime + frontend
#include "MetasoundSource.h"           // UMetaSoundSource
#include "Metasound.h"                 // UMetaSoundPatch
#include "MetasoundDocumentInterface.h"// IMetaSoundDocumentInterface, UMetaSoundDocumentInterface
#include "MetasoundFrontendDocument.h" // FMetasoundFrontendDocument, FMetasoundFrontendGraphClass, FMetasoundFrontendGraph, FMetasoundFrontendNode, FMetasoundFrontendEdge, FMetasoundFrontendVariable, FMetasoundFrontendClass, FMetasoundFrontendClassName, EMetasoundFrontendClassType
#include "MetasoundFrontendLiteral.h"  // FMetasoundFrontendLiteral, EMetasoundFrontendLiteralType

namespace
{
	/** Serialize a JSON object to compact string. */
	FString JsonToString(TSharedPtr<FJsonObject> JsonObj)
	{
		FString Out;
		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(JsonObj.ToSharedRef(), *Writer, true);
		return Out;
	}

	/** Map EMetasoundFrontendClassType to a stable string label (for indexed-node properties JSON). */
	FString ClassTypeToString(EMetasoundFrontendClassType InType)
	{
		switch (InType)
		{
		case EMetasoundFrontendClassType::External:                return TEXT("External");
		case EMetasoundFrontendClassType::Graph:                   return TEXT("Graph");
		case EMetasoundFrontendClassType::Input:                   return TEXT("Input");
		case EMetasoundFrontendClassType::Output:                  return TEXT("Output");
		case EMetasoundFrontendClassType::Literal:                 return TEXT("Literal");
		case EMetasoundFrontendClassType::Variable:                return TEXT("Variable");
		case EMetasoundFrontendClassType::VariableAccessor:        return TEXT("VariableAccessor");
		case EMetasoundFrontendClassType::VariableDeferredAccessor:return TEXT("VariableDeferredAccessor");
		case EMetasoundFrontendClassType::VariableMutator:         return TEXT("VariableMutator");
		default:                                                   return TEXT("Invalid");
		}
	}

	/** Map EMetasoundFrontendLiteralType to a stable string label. */
	FString LiteralTypeToString(EMetasoundFrontendLiteralType InType)
	{
		switch (InType)
		{
		case EMetasoundFrontendLiteralType::None:          return TEXT("None");
		case EMetasoundFrontendLiteralType::Boolean:       return TEXT("Boolean");
		case EMetasoundFrontendLiteralType::Integer:       return TEXT("Integer");
		case EMetasoundFrontendLiteralType::Float:         return TEXT("Float");
		case EMetasoundFrontendLiteralType::String:        return TEXT("String");
		case EMetasoundFrontendLiteralType::UObject:       return TEXT("UObject");
		case EMetasoundFrontendLiteralType::NoneArray:     return TEXT("NoneArray");
		case EMetasoundFrontendLiteralType::BooleanArray:  return TEXT("BooleanArray");
		case EMetasoundFrontendLiteralType::IntegerArray:  return TEXT("IntegerArray");
		case EMetasoundFrontendLiteralType::FloatArray:    return TEXT("FloatArray");
		case EMetasoundFrontendLiteralType::StringArray:   return TEXT("StringArray");
		case EMetasoundFrontendLiteralType::UObjectArray:  return TEXT("UObjectArray");
		default:                                           return TEXT("Invalid");
		}
	}

	/** Build a compact JSON object describing a MetaSound literal (type + best-effort value). */
	TSharedPtr<FJsonObject> SerializeLiteral(const FMetasoundFrontendLiteral& Lit)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("type"), LiteralTypeToString(Lit.GetType()));
		// Lit.ToString() handles all variants (scalar + array) without proxying UObjects.
		Obj->SetStringField(TEXT("value"), Lit.ToString());
		return Obj;
	}

	/** Look up FMetasoundFrontendClass from a node's ClassID via document Dependencies + RootGraph + Subgraphs. Returns nullptr if not found. */
	const FMetasoundFrontendClass* FindClassByID(const FMetasoundFrontendDocument& Doc, const FGuid& ClassID)
	{
		if (Doc.RootGraph.ID == ClassID)
		{
			return &Doc.RootGraph;
		}
		for (const FMetasoundFrontendGraphClass& SubGraph : Doc.Subgraphs)
		{
			if (SubGraph.ID == ClassID)
			{
				return &SubGraph;
			}
		}
		for (const FMetasoundFrontendClass& Dep : Doc.Dependencies)
		{
			if (Dep.ID == ClassID)
			{
				return &Dep;
			}
		}
		return nullptr;
	}
}

bool FMetaSoundIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Enumerate both MetaSound asset variants — Sources (playable assets) and Patches (graph templates).
	TArray<FAssetData> MetaSoundAssets;
	{
		FARFilter Filter;
		for (const FName& ContentPath : UMonolithSettings::GetIndexedContentPaths())
		{
			Filter.PackagePaths.Add(ContentPath);
		}
		Filter.bRecursivePaths = true;
		Filter.ClassPaths.Add(UMetaSoundSource::StaticClass()->GetClassPathName());
		Filter.ClassPaths.Add(UMetaSoundPatch::StaticClass()->GetClassPathName());
		Registry.GetAssets(Filter, MetaSoundAssets);
	}

	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	const int32 BatchSize = FMath::Max(1, FMonolithMemoryHelper::GetResolvedPostPassBatchSize());
	const SIZE_T MemoryBudgetMB = static_cast<SIZE_T>(FMonolithMemoryHelper::GetResolvedMemoryBudgetMB());
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
		// Compiler-idle gate is enforced by FMonolithCompilerSafeDispatch at the call site (mirrors NiagaraIndexer).

		// Memory budget check before each batch
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

		const int32 BatchEnd = FMath::Min(i + BatchSize, MetaSoundAssets.Num());

		for (int32 j = i; j < BatchEnd; ++j)
		{
			const FAssetData& MetaSoundAssetData = MetaSoundAssets[j];

			const int64 ThisAssetId = DB.GetAssetId(MetaSoundAssetData.PackageName.ToString());
			if (ThisAssetId < 0) continue;

			// Capture residency BEFORE GetAsset() may load it (issue #81).
			const bool bWasLoaded = MetaSoundAssetData.IsAssetLoaded();
			UObject* Loaded = MetaSoundAssetData.GetAsset();
			if (!Loaded) continue;

			// Distinguish UMetaSoundSource vs UMetaSoundPatch for the asset_class label in indexed properties.
			FString AssetClassLabel = TEXT("MetaSound");
			if (Loaded->IsA(UMetaSoundSource::StaticClass()))
			{
				AssetClassLabel = TEXT("MetaSoundSource");
			}
			else if (Loaded->IsA(UMetaSoundPatch::StaticClass()))
			{
				AssetClassLabel = TEXT("MetaSoundPatch");
			}

			IndexMetaSoundAsset(Loaded, AssetClassLabel, DB, ThisAssetId);
			AssetsIndexed++;

			// Mark for unloading
			FMonolithMemoryHelper::TryUnloadPackage(Loaded, bWasLoaded);
		}

		BatchNumber++;

		// GC after each batch
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

	// Final GC
	FMonolithMemoryHelper::ForceGarbageCollection(true);

	UE_LOG(LogMonolithIndex, Log, TEXT("MetaSoundIndexer: indexed %d MetaSound assets"), AssetsIndexed);

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("MetaSoundIndexer complete"));
	}

	return true;
}

void FMetaSoundIndexer::IndexMetaSoundAsset(UObject* MetaSoundAsset, const FString& AssetClassLabel, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!MetaSoundAsset) return;

	// MetaSound assets implement IMetaSoundDocumentInterface. Use the read-only GetConstDocument()
	// (the mutable counterpart GetDocument() is private and reserved for the builder subsystem).
	if (!MetaSoundAsset->GetClass()->ImplementsInterface(UMetaSoundDocumentInterface::StaticClass()))
	{
		UE_LOG(LogMonolithIndex, Verbose, TEXT("MetaSoundIndexer: '%s' does not implement IMetaSoundDocumentInterface — skipping."),
			*MetaSoundAsset->GetName());
		return;
	}

	const IMetaSoundDocumentInterface* DocInterface = Cast<IMetaSoundDocumentInterface>(MetaSoundAsset);
	if (!DocInterface)
	{
		return;
	}

	const FMetasoundFrontendDocument& Doc = DocInterface->GetConstDocument();

	// ── Asset-level node (one per asset, summarises the document) ─────────────
	int32 TotalNodes = 0;
	int32 TotalEdges = 0;
	int32 TotalVariables = 0;
	int32 PageCount = 0;

	Doc.RootGraph.IterateGraphPages([&](const FMetasoundFrontendGraph& Page)
	{
		++PageCount;
		TotalNodes += Page.Nodes.Num();
		TotalEdges += Page.Edges.Num();
		TotalVariables += Page.Variables.Num();
	});

	{
		auto AssetProps = MakeShared<FJsonObject>();
		AssetProps->SetStringField(TEXT("asset_class"), AssetClassLabel);
		AssetProps->SetNumberField(TEXT("page_count"), PageCount);
		AssetProps->SetNumberField(TEXT("node_count"), TotalNodes);
		AssetProps->SetNumberField(TEXT("edge_count"), TotalEdges);
		AssetProps->SetNumberField(TEXT("variable_count"), TotalVariables);
		AssetProps->SetNumberField(TEXT("dependency_count"), Doc.Dependencies.Num());
		AssetProps->SetNumberField(TEXT("subgraph_count"), Doc.Subgraphs.Num());
		AssetProps->SetNumberField(TEXT("interface_count"), Doc.Interfaces.Num());

		// Serialize interface versions as a JSON array of strings.
		TArray<TSharedPtr<FJsonValue>> InterfaceVersions;
		for (const FMetasoundFrontendVersion& Iface : Doc.Interfaces)
		{
			InterfaceVersions.Add(MakeShared<FJsonValueString>(Iface.ToString()));
		}
		AssetProps->SetArrayField(TEXT("interfaces"), InterfaceVersions);

		FIndexedNode AssetNode;
		AssetNode.AssetId = AssetId;
		AssetNode.NodeName = MetaSoundAsset->GetName();
		AssetNode.NodeClass = AssetClassLabel;
		AssetNode.NodeType = TEXT("Asset");
		AssetNode.Properties = JsonToString(AssetProps);
		DB.InsertNode(AssetNode);
	}

	// ── Per-page graph walk ───────────────────────────────────────────────────
	Doc.RootGraph.IterateGraphPages([&](const FMetasoundFrontendGraph& Page)
	{
		// Page-level node (aggregates per-page counts so callers can target a single page).
		{
			auto PageProps = MakeShared<FJsonObject>();
			PageProps->SetStringField(TEXT("page_id"), Page.PageID.ToString());
			PageProps->SetNumberField(TEXT("node_count"), Page.Nodes.Num());
			PageProps->SetNumberField(TEXT("edge_count"), Page.Edges.Num());
			PageProps->SetNumberField(TEXT("variable_count"), Page.Variables.Num());

			FIndexedNode PageNode;
			PageNode.AssetId = AssetId;
			PageNode.NodeName = Page.PageID.ToString();
			PageNode.NodeClass = TEXT("MetaSoundGraphPage");
			PageNode.NodeType = TEXT("Page");
			PageNode.Properties = JsonToString(PageProps);
			DB.InsertNode(PageNode);
		}

		// Map node-ID -> inserted DB row ID, so we can link edges below.
		TMap<FGuid, int64> NodeIdToDbId;
		NodeIdToDbId.Reserve(Page.Nodes.Num());

		// ── Nodes ─────────────────────────────────────────────────────────
		for (const FMetasoundFrontendNode& Node : Page.Nodes)
		{
			auto NodeProps = MakeShared<FJsonObject>();
			NodeProps->SetStringField(TEXT("page_id"), Page.PageID.ToString());
			NodeProps->SetStringField(TEXT("node_id"), Node.GetID().ToString());
			NodeProps->SetStringField(TEXT("class_id"), Node.ClassID.ToString());
			NodeProps->SetStringField(TEXT("name"), Node.Name.ToString());

			// Resolve class metadata for class name + type.
			FString ClassFullName;
			FString ClassTypeStr = TEXT("Unknown");
			if (const FMetasoundFrontendClass* ClassEntry = FindClassByID(Doc, Node.ClassID))
			{
				ClassFullName = ClassEntry->Metadata.GetClassName().ToString();
				ClassTypeStr = ClassTypeToString(ClassEntry->Metadata.GetType());
			}
			NodeProps->SetStringField(TEXT("class_name"), ClassFullName);
			NodeProps->SetStringField(TEXT("class_type"), ClassTypeStr);

			// Input vertices
			{
				TArray<TSharedPtr<FJsonValue>> InputsJson;
				for (const FMetasoundFrontendVertex& Input : Node.Interface.Inputs)
				{
					auto V = MakeShared<FJsonObject>();
					V->SetStringField(TEXT("name"), Input.Name.ToString());
					V->SetStringField(TEXT("type"), Input.TypeName.ToString());
					V->SetStringField(TEXT("vertex_id"), Input.VertexID.ToString());
					InputsJson.Add(MakeShared<FJsonValueObject>(V));
				}
				NodeProps->SetArrayField(TEXT("inputs"), InputsJson);
			}

			// Output vertices
			{
				TArray<TSharedPtr<FJsonValue>> OutputsJson;
				for (const FMetasoundFrontendVertex& Output : Node.Interface.Outputs)
				{
					auto V = MakeShared<FJsonObject>();
					V->SetStringField(TEXT("name"), Output.Name.ToString());
					V->SetStringField(TEXT("type"), Output.TypeName.ToString());
					V->SetStringField(TEXT("vertex_id"), Output.VertexID.ToString());
					OutputsJson.Add(MakeShared<FJsonValueObject>(V));
				}
				NodeProps->SetArrayField(TEXT("outputs"), OutputsJson);
			}

			FIndexedNode IdxNode;
			IdxNode.AssetId = AssetId;
			IdxNode.NodeName = Node.Name.ToString();
			IdxNode.NodeClass = ClassFullName.IsEmpty() ? TEXT("MetaSoundNode") : ClassFullName;
			IdxNode.NodeType = TEXT("Node");
			IdxNode.Properties = JsonToString(NodeProps);
			const int64 InsertedNodeId = DB.InsertNode(IdxNode);
			NodeIdToDbId.Add(Node.GetID(), InsertedNodeId);
		}

		// ── Edges (connections) ──────────────────────────────────────────
		for (const FMetasoundFrontendEdge& Edge : Page.Edges)
		{
			const int64* FromId = NodeIdToDbId.Find(Edge.FromNodeID);
			const int64* ToId = NodeIdToDbId.Find(Edge.ToNodeID);
			if (!FromId || !ToId) continue;

			FIndexedConnection Conn;
			Conn.SourceNodeId = *FromId;
			Conn.TargetNodeId = *ToId;
			Conn.SourcePin = Edge.FromVertexID.ToString();
			Conn.TargetPin = Edge.ToVertexID.ToString();
			Conn.PinType = TEXT(""); // Vertex-level type would require resolving via the node's interface — left blank for now.
			DB.InsertConnection(Conn);
		}

		// ── Variables ─────────────────────────────────────────────────────
		for (const FMetasoundFrontendVariable& Var : Page.Variables)
		{
			FIndexedVariable IdxVar;
			IdxVar.AssetId = AssetId;
			IdxVar.VarName = Var.Name.ToString();
			IdxVar.VarType = Var.TypeName.ToString();
			IdxVar.Category = TEXT("MetaSound");
			IdxVar.DefaultValue = JsonToString(SerializeLiteral(Var.Literal));
			IdxVar.bIsExposed = false;
			IdxVar.bIsReplicated = false;
			DB.InsertVariable(IdxVar);
		}
	});

	// ── Dependencies (referenced node classes) ────────────────────────────────
	for (const FMetasoundFrontendClass& Dep : Doc.Dependencies)
	{
		auto DepProps = MakeShared<FJsonObject>();
		DepProps->SetStringField(TEXT("class_id"), Dep.ID.ToString());
		DepProps->SetStringField(TEXT("class_name"), Dep.Metadata.GetClassName().ToString());
		DepProps->SetStringField(TEXT("class_type"), ClassTypeToString(Dep.Metadata.GetType()));
		const FMetasoundFrontendVersionNumber& Ver = Dep.Metadata.GetVersion();
		DepProps->SetStringField(TEXT("version"), FString::Printf(TEXT("%d.%d"), Ver.Major, Ver.Minor));

		FIndexedNode DepNode;
		DepNode.AssetId = AssetId;
		DepNode.NodeName = Dep.Metadata.GetClassName().ToString();
		DepNode.NodeClass = TEXT("MetaSoundDependency");
		DepNode.NodeType = TEXT("Dependency");
		DepNode.Properties = JsonToString(DepProps);
		DB.InsertNode(DepNode);
	}
}

#endif // WITH_METASOUND
