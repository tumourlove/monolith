#include "MonolithMetaSoundActions.h"
#include "MonolithMetaSoundHelpers.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#include "MetasoundSource.h"
#include "Metasound.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendRegistries.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using namespace MonolithMetaSoundHelpers;

namespace
{
	IMetaSoundDocumentInterface* GetDocumentInterface(UObject* Asset)
	{
		if (!Asset)
		{
			return nullptr;
		}
		return Cast<IMetaSoundDocumentInterface>(Asset);
	}

	const FMetasoundFrontendGraph* GetGraphFromDoc(
		const FMetasoundFrontendDocument& Doc,
		const FString& PageIdStr)
	{
		if (PageIdStr.IsEmpty())
		{
			return &Doc.RootGraph.GetConstDefaultGraph();
		}

		FGuid PageId;
		if (!FGuid::Parse(PageIdStr, PageId))
		{
			return nullptr;
		}

		const FMetasoundFrontendGraph* FoundGraph = nullptr;
		Doc.RootGraph.IterateGraphPages([&](const FMetasoundFrontendGraph& Page)
		{
			if (Page.PageID == PageId)
			{
				FoundGraph = &Page;
			}
		});

		return FoundGraph;
	}
}

void FMonolithMetaSoundActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("metasound"), TEXT("list_graphs"),
		TEXT("List all graph pages in a MetaSound asset"),
		FMonolithActionHandler::CreateStatic(&FMonolithMetaSoundActions::ListGraphs),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Build());

	Registry.RegisterAction(TEXT("metasound"), TEXT("get_graph_data"),
		TEXT("Get full graph data with nodes, edges, and connections"),
		FMonolithActionHandler::CreateStatic(&FMonolithMetaSoundActions::GetGraphData),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Optional(TEXT("page_id"), TEXT("string"), TEXT("Page GUID (default: default page)"))
			.Build());

	Registry.RegisterAction(TEXT("metasound"), TEXT("get_graph_summary"),
		TEXT("Get lightweight node list without full pin details"),
		FMonolithActionHandler::CreateStatic(&FMonolithMetaSoundActions::GetGraphSummary),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Optional(TEXT("page_id"), TEXT("string"), TEXT("Page GUID (default: default page)"))
			.Build());

	Registry.RegisterAction(TEXT("metasound"), TEXT("get_node_details"),
		TEXT("Get full pin dump for one node by ID or name"),
		FMonolithActionHandler::CreateStatic(&FMonolithMetaSoundActions::GetNodeDetails),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Optional(TEXT("node_id"), TEXT("string"), TEXT("Node GUID"))
			.Optional(TEXT("node_name"), TEXT("string"), TEXT("Node name"))
			.Optional(TEXT("page_id"), TEXT("string"), TEXT("Page GUID (default: default page)"))
			.Build());

	Registry.RegisterAction(TEXT("metasound"), TEXT("get_connections"),
		TEXT("Get all edges for a node or full graph"),
		FMonolithActionHandler::CreateStatic(&FMonolithMetaSoundActions::GetConnections),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Optional(TEXT("node_id"), TEXT("string"), TEXT("Filter by node GUID"))
			.Optional(TEXT("page_id"), TEXT("string"), TEXT("Page GUID (default: default page)"))
			.Build());

	Registry.RegisterAction(TEXT("metasound"), TEXT("get_variables"),
		TEXT("Get graph-local variables"),
		FMonolithActionHandler::CreateStatic(&FMonolithMetaSoundActions::GetVariables),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Optional(TEXT("page_id"), TEXT("string"), TEXT("Page GUID (default: default page)"))
			.Build());

	Registry.RegisterAction(TEXT("metasound"), TEXT("get_user_parameters"),
		TEXT("Get document input parameters (user-exposed)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMetaSoundActions::GetUserParameters),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Build());

	Registry.RegisterAction(TEXT("metasound"), TEXT("search_nodes"),
		TEXT("Find nodes by class name or node name"),
		FMonolithActionHandler::CreateStatic(&FMonolithMetaSoundActions::SearchNodes),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Required(TEXT("query"), TEXT("string"), TEXT("Search string (matches class or node name)"))
			.Optional(TEXT("page_id"), TEXT("string"), TEXT("Page GUID (default: default page)"))
			.Build());

	Registry.RegisterAction(TEXT("metasound"), TEXT("get_metasound_info"),
		TEXT("Get asset overview (type, interface, version)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMetaSoundActions::GetMetaSoundInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Build());

	Registry.RegisterAction(TEXT("metasound"), TEXT("get_dependencies"),
		TEXT("Get referenced MetaSounds and patches"),
		FMonolithActionHandler::CreateStatic(&FMonolithMetaSoundActions::GetDependencies),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Build());

	Registry.RegisterAction(TEXT("metasound"), TEXT("list_metasounds"),
		TEXT("List all MetaSound assets in project"),
		FMonolithActionHandler::CreateStatic(&FMonolithMetaSoundActions::ListMetaSounds),
		FParamSchemaBuilder()
			.Optional(TEXT("filter"), TEXT("string"), TEXT("Filter by name substring"))
			.Optional(TEXT("type"), TEXT("string"), TEXT("Filter by type: Source, Patch, or All"), TEXT("All"))
			.Build());

	Registry.RegisterAction(TEXT("metasound"), TEXT("validate_metasound"),
		TEXT("Validate a MetaSound for errors and warnings"),
		FMonolithActionHandler::CreateStatic(&FMonolithMetaSoundActions::ValidateMetaSound),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MetaSound asset path"))
			.Build());
}

FMonolithActionResult FMonolithMetaSoundActions::ListGraphs(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	
	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	IMetaSoundDocumentInterface* DocInterface = GetDocumentInterface(Asset);
	if (!DocInterface)
	{
		return FMonolithActionResult::Error(TEXT("Asset is not a MetaSound"));
	}

	const FMetasoundFrontendDocument& Doc = DocInterface->GetConstDocument();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);

	TArray<TSharedPtr<FJsonValue>> PagesArray;
	Doc.RootGraph.IterateGraphPages([&](const FMetasoundFrontendGraph& Page)
	{
		TSharedPtr<FJsonObject> PageJson = MakeShared<FJsonObject>();
		PageJson->SetStringField(TEXT("page_id"), Page.PageID.ToString());
		PageJson->SetNumberField(TEXT("node_count"), Page.Nodes.Num());
		PageJson->SetNumberField(TEXT("edge_count"), Page.Edges.Num());
		PageJson->SetNumberField(TEXT("variable_count"), Page.Variables.Num());
		PagesArray.Add(MakeShared<FJsonValueObject>(PageJson));
	});

	Result->SetArrayField(TEXT("pages"), PagesArray);
	Result->SetNumberField(TEXT("page_count"), PagesArray.Num());
	Result->SetNumberField(TEXT("subgraph_count"), Doc.Subgraphs.Num());
	Result->SetNumberField(TEXT("dependency_count"), Doc.Dependencies.Num());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMetaSoundActions::GetGraphData(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString PageIdStr = Params->HasField(TEXT("page_id")) ? Params->GetStringField(TEXT("page_id")) : FString();

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	IMetaSoundDocumentInterface* DocInterface = GetDocumentInterface(Asset);
	if (!DocInterface)
	{
		return FMonolithActionResult::Error(TEXT("Asset is not a MetaSound"));
	}

	const FMetasoundFrontendDocument& Doc = DocInterface->GetConstDocument();
	const FMetasoundFrontendGraph* Graph = GetGraphFromDoc(Doc, PageIdStr);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Graph page not found: %s"), *PageIdStr));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("page_id"), Graph->PageID.ToString());

	TArray<TSharedPtr<FJsonValue>> NodesArray;
	for (const FMetasoundFrontendNode& Node : Graph->Nodes)
	{
		NodesArray.Add(MakeShared<FJsonValueObject>(SerializeNode(Node, Doc)));
	}
	Result->SetArrayField(TEXT("nodes"), NodesArray);

	TArray<TSharedPtr<FJsonValue>> EdgesArray;
	for (const FMetasoundFrontendEdge& Edge : Graph->Edges)
	{
		EdgesArray.Add(MakeShared<FJsonValueObject>(SerializeEdge(Edge, *Graph)));
	}
	Result->SetArrayField(TEXT("edges"), EdgesArray);

	TArray<TSharedPtr<FJsonValue>> VariablesArray;
	for (const FMetasoundFrontendVariable& Variable : Graph->Variables)
	{
		VariablesArray.Add(MakeShared<FJsonValueObject>(SerializeVariable(Variable)));
	}
	Result->SetArrayField(TEXT("variables"), VariablesArray);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMetaSoundActions::GetGraphSummary(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString PageIdStr = Params->HasField(TEXT("page_id")) ? Params->GetStringField(TEXT("page_id")) : FString();

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	IMetaSoundDocumentInterface* DocInterface = GetDocumentInterface(Asset);
	if (!DocInterface)
	{
		return FMonolithActionResult::Error(TEXT("Asset is not a MetaSound"));
	}

	const FMetasoundFrontendDocument& Doc = DocInterface->GetConstDocument();
	const FMetasoundFrontendGraph* Graph = GetGraphFromDoc(Doc, PageIdStr);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Graph page not found: %s"), *PageIdStr));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("page_id"), Graph->PageID.ToString());

	TArray<TSharedPtr<FJsonValue>> NodesArray;
	for (const FMetasoundFrontendNode& Node : Graph->Nodes)
	{
		NodesArray.Add(MakeShared<FJsonValueObject>(SerializeNodeSummary(Node, Doc)));
	}
	Result->SetArrayField(TEXT("nodes"), NodesArray);
	Result->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
	Result->SetNumberField(TEXT("edge_count"), Graph->Edges.Num());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMetaSoundActions::GetNodeDetails(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString PageIdStr = Params->HasField(TEXT("page_id")) ? Params->GetStringField(TEXT("page_id")) : FString();
	FString NodeIdStr = Params->HasField(TEXT("node_id")) ? Params->GetStringField(TEXT("node_id")) : FString();
	FString NodeName = Params->HasField(TEXT("node_name")) ? Params->GetStringField(TEXT("node_name")) : FString();

	if (NodeIdStr.IsEmpty() && NodeName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Must provide either node_id or node_name"));
	}

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	IMetaSoundDocumentInterface* DocInterface = GetDocumentInterface(Asset);
	if (!DocInterface)
	{
		return FMonolithActionResult::Error(TEXT("Asset is not a MetaSound"));
	}

	const FMetasoundFrontendDocument& Doc = DocInterface->GetConstDocument();
	const FMetasoundFrontendGraph* Graph = GetGraphFromDoc(Doc, PageIdStr);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Graph page not found: %s"), *PageIdStr));
	}

	const FMetasoundFrontendNode* FoundNode = nullptr;
	if (!NodeIdStr.IsEmpty())
	{
		FGuid NodeId;
		if (FGuid::Parse(NodeIdStr, NodeId))
		{
			FoundNode = FindNodeById(*Graph, NodeId);
		}
	}
	else if (!NodeName.IsEmpty())
	{
		FName SearchName(*NodeName);
		for (const FMetasoundFrontendNode& Node : Graph->Nodes)
		{
			if (Node.Name == SearchName)
			{
				FoundNode = &Node;
				break;
			}
		}
	}

	if (!FoundNode)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Node not found: %s"), 
			!NodeIdStr.IsEmpty() ? *NodeIdStr : *NodeName));
	}

	TSharedPtr<FJsonObject> Result = SerializeNode(*FoundNode, Doc);

	TArray<const FMetasoundFrontendEdge*> InputEdges = GetNodeEdges(*Graph, FoundNode->GetID(), true, false);
	TArray<const FMetasoundFrontendEdge*> OutputEdges = GetNodeEdges(*Graph, FoundNode->GetID(), false, true);

	TArray<TSharedPtr<FJsonValue>> InputEdgesArray;
	for (const FMetasoundFrontendEdge* Edge : InputEdges)
	{
		InputEdgesArray.Add(MakeShared<FJsonValueObject>(SerializeEdge(*Edge, *Graph)));
	}
	Result->SetArrayField(TEXT("incoming_edges"), InputEdgesArray);

	TArray<TSharedPtr<FJsonValue>> OutputEdgesArray;
	for (const FMetasoundFrontendEdge* Edge : OutputEdges)
	{
		OutputEdgesArray.Add(MakeShared<FJsonValueObject>(SerializeEdge(*Edge, *Graph)));
	}
	Result->SetArrayField(TEXT("outgoing_edges"), OutputEdgesArray);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMetaSoundActions::GetConnections(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString PageIdStr = Params->HasField(TEXT("page_id")) ? Params->GetStringField(TEXT("page_id")) : FString();
	FString NodeIdStr = Params->HasField(TEXT("node_id")) ? Params->GetStringField(TEXT("node_id")) : FString();

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	IMetaSoundDocumentInterface* DocInterface = GetDocumentInterface(Asset);
	if (!DocInterface)
	{
		return FMonolithActionResult::Error(TEXT("Asset is not a MetaSound"));
	}

	const FMetasoundFrontendDocument& Doc = DocInterface->GetConstDocument();
	const FMetasoundFrontendGraph* Graph = GetGraphFromDoc(Doc, PageIdStr);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Graph page not found: %s"), *PageIdStr));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);

	TArray<TSharedPtr<FJsonValue>> EdgesArray;

	if (!NodeIdStr.IsEmpty())
	{
		FGuid NodeId;
		if (!FGuid::Parse(NodeIdStr, NodeId))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid node_id GUID: %s"), *NodeIdStr));
		}

		TArray<const FMetasoundFrontendEdge*> Edges = GetNodeEdges(*Graph, NodeId, true, true);
		for (const FMetasoundFrontendEdge* Edge : Edges)
		{
			EdgesArray.Add(MakeShared<FJsonValueObject>(SerializeEdge(*Edge, *Graph)));
		}
		Result->SetStringField(TEXT("node_id"), NodeIdStr);
	}
	else
	{
		for (const FMetasoundFrontendEdge& Edge : Graph->Edges)
		{
			EdgesArray.Add(MakeShared<FJsonValueObject>(SerializeEdge(Edge, *Graph)));
		}
	}

	Result->SetArrayField(TEXT("edges"), EdgesArray);
	Result->SetNumberField(TEXT("edge_count"), EdgesArray.Num());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMetaSoundActions::GetVariables(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString PageIdStr = Params->HasField(TEXT("page_id")) ? Params->GetStringField(TEXT("page_id")) : FString();

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	IMetaSoundDocumentInterface* DocInterface = GetDocumentInterface(Asset);
	if (!DocInterface)
	{
		return FMonolithActionResult::Error(TEXT("Asset is not a MetaSound"));
	}

	const FMetasoundFrontendDocument& Doc = DocInterface->GetConstDocument();
	const FMetasoundFrontendGraph* Graph = GetGraphFromDoc(Doc, PageIdStr);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Graph page not found: %s"), *PageIdStr));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);

	TArray<TSharedPtr<FJsonValue>> VariablesArray;
	for (const FMetasoundFrontendVariable& Variable : Graph->Variables)
	{
		VariablesArray.Add(MakeShared<FJsonValueObject>(SerializeVariable(Variable)));
	}

	Result->SetArrayField(TEXT("variables"), VariablesArray);
	Result->SetNumberField(TEXT("variable_count"), VariablesArray.Num());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMetaSoundActions::GetUserParameters(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	IMetaSoundDocumentInterface* DocInterface = GetDocumentInterface(Asset);
	if (!DocInterface)
	{
		return FMonolithActionResult::Error(TEXT("Asset is not a MetaSound"));
	}

	const FMetasoundFrontendDocument& Doc = DocInterface->GetConstDocument();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);

	const FMetasoundFrontendClassInterface& ClassInterface = Doc.RootGraph.GetDefaultInterface();

	TArray<TSharedPtr<FJsonValue>> InputsArray;
	for (const FMetasoundFrontendClassInput& Input : ClassInterface.Inputs)
	{
		TSharedPtr<FJsonObject> InputJson = MakeShared<FJsonObject>();
		InputJson->SetStringField(TEXT("name"), Input.Name.ToString());
		InputJson->SetStringField(TEXT("type"), Input.TypeName.ToString());
		InputJson->SetStringField(TEXT("vertex_id"), Input.VertexID.ToString());
		InputJson->SetStringField(TEXT("node_id"), Input.NodeID.ToString());

#if WITH_EDITORONLY_DATA
		InputJson->SetStringField(TEXT("display_name"), Input.Metadata.GetDisplayName().ToString());
		InputJson->SetStringField(TEXT("description"), Input.Metadata.GetDescription().ToString());
#endif

		if (const FMetasoundFrontendLiteral* DefaultLiteral = Input.FindConstDefault(Metasound::Frontend::DefaultPageID))
		{
			InputJson->SetObjectField(TEXT("default_value"), SerializeLiteral(*DefaultLiteral));
		}

		InputsArray.Add(MakeShared<FJsonValueObject>(InputJson));
	}

	TArray<TSharedPtr<FJsonValue>> OutputsArray;
	for (const FMetasoundFrontendClassOutput& Output : ClassInterface.Outputs)
	{
		TSharedPtr<FJsonObject> OutputJson = MakeShared<FJsonObject>();
		OutputJson->SetStringField(TEXT("name"), Output.Name.ToString());
		OutputJson->SetStringField(TEXT("type"), Output.TypeName.ToString());
		OutputJson->SetStringField(TEXT("vertex_id"), Output.VertexID.ToString());
		OutputJson->SetStringField(TEXT("node_id"), Output.NodeID.ToString());

#if WITH_EDITORONLY_DATA
		OutputJson->SetStringField(TEXT("display_name"), Output.Metadata.GetDisplayName().ToString());
		OutputJson->SetStringField(TEXT("description"), Output.Metadata.GetDescription().ToString());
#endif

		OutputsArray.Add(MakeShared<FJsonValueObject>(OutputJson));
	}

	Result->SetArrayField(TEXT("inputs"), InputsArray);
	Result->SetArrayField(TEXT("outputs"), OutputsArray);
	Result->SetNumberField(TEXT("input_count"), InputsArray.Num());
	Result->SetNumberField(TEXT("output_count"), OutputsArray.Num());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMetaSoundActions::SearchNodes(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString Query = Params->GetStringField(TEXT("query"));
	FString PageIdStr = Params->HasField(TEXT("page_id")) ? Params->GetStringField(TEXT("page_id")) : FString();

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	IMetaSoundDocumentInterface* DocInterface = GetDocumentInterface(Asset);
	if (!DocInterface)
	{
		return FMonolithActionResult::Error(TEXT("Asset is not a MetaSound"));
	}

	const FMetasoundFrontendDocument& Doc = DocInterface->GetConstDocument();
	const FMetasoundFrontendGraph* Graph = GetGraphFromDoc(Doc, PageIdStr);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Graph page not found: %s"), *PageIdStr));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("query"), Query);

	FString QueryLower = Query.ToLower();
	TArray<TSharedPtr<FJsonValue>> MatchesArray;

	for (const FMetasoundFrontendNode& Node : Graph->Nodes)
	{
		FString ClassName = ResolveClassName(Node.ClassID, Doc);
		FString NodeName = Node.Name.ToString();

		if (ClassName.ToLower().Contains(QueryLower) || NodeName.ToLower().Contains(QueryLower))
		{
			MatchesArray.Add(MakeShared<FJsonValueObject>(SerializeNodeSummary(Node, Doc)));
		}
	}

	Result->SetArrayField(TEXT("matches"), MatchesArray);
	Result->SetNumberField(TEXT("match_count"), MatchesArray.Num());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMetaSoundActions::GetMetaSoundInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	IMetaSoundDocumentInterface* DocInterface = GetDocumentInterface(Asset);
	if (!DocInterface)
	{
		return FMonolithActionResult::Error(TEXT("Asset is not a MetaSound"));
	}

	const FMetasoundFrontendDocument& Doc = DocInterface->GetConstDocument();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());

	if (Cast<UMetaSoundSource>(Asset))
	{
		Result->SetStringField(TEXT("type"), TEXT("Source"));
	}
	else if (Cast<UMetaSoundPatch>(Asset))
	{
		Result->SetStringField(TEXT("type"), TEXT("Patch"));
	}
	else
	{
		Result->SetStringField(TEXT("type"), TEXT("Unknown"));
	}

	Result->SetStringField(TEXT("root_graph_class"), Doc.RootGraph.Metadata.GetClassName().ToString());

	TSharedPtr<FJsonObject> VersionJson = MakeShared<FJsonObject>();
	VersionJson->SetNumberField(TEXT("major"), Doc.RootGraph.Metadata.GetVersion().Major);
	VersionJson->SetNumberField(TEXT("minor"), Doc.RootGraph.Metadata.GetVersion().Minor);
	Result->SetObjectField(TEXT("version"), VersionJson);

	const FMetasoundFrontendGraph& DefaultGraph = Doc.RootGraph.GetConstDefaultGraph();
	Result->SetNumberField(TEXT("node_count"), DefaultGraph.Nodes.Num());
	Result->SetNumberField(TEXT("edge_count"), DefaultGraph.Edges.Num());
	Result->SetNumberField(TEXT("variable_count"), DefaultGraph.Variables.Num());
	const FMetasoundFrontendClassInterface& InfoInterface = Doc.RootGraph.GetDefaultInterface();
	Result->SetNumberField(TEXT("input_count"), InfoInterface.Inputs.Num());
	Result->SetNumberField(TEXT("output_count"), InfoInterface.Outputs.Num());
	Result->SetNumberField(TEXT("dependency_count"), Doc.Dependencies.Num());
	Result->SetNumberField(TEXT("subgraph_count"), Doc.Subgraphs.Num());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMetaSoundActions::GetDependencies(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	IMetaSoundDocumentInterface* DocInterface = GetDocumentInterface(Asset);
	if (!DocInterface)
	{
		return FMonolithActionResult::Error(TEXT("Asset is not a MetaSound"));
	}

	const FMetasoundFrontendDocument& Doc = DocInterface->GetConstDocument();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);

	TArray<TSharedPtr<FJsonValue>> DepsArray;
	for (const FMetasoundFrontendClass& Dep : Doc.Dependencies)
	{
		TSharedPtr<FJsonObject> DepJson = MakeShared<FJsonObject>();
		DepJson->SetStringField(TEXT("id"), Dep.ID.ToString());
		DepJson->SetStringField(TEXT("class_name"), Dep.Metadata.GetClassName().ToString());

		FString TypeStr;
		switch (Dep.Metadata.GetType())
		{
		case EMetasoundFrontendClassType::External:
			TypeStr = TEXT("External");
			break;
		case EMetasoundFrontendClassType::Graph:
			TypeStr = TEXT("Graph");
			break;
		case EMetasoundFrontendClassType::Input:
			TypeStr = TEXT("Input");
			break;
		case EMetasoundFrontendClassType::Output:
			TypeStr = TEXT("Output");
			break;
		case EMetasoundFrontendClassType::Variable:
			TypeStr = TEXT("Variable");
			break;
		case EMetasoundFrontendClassType::VariableDeferredAccessor:
			TypeStr = TEXT("VariableDeferredAccessor");
			break;
		case EMetasoundFrontendClassType::VariableAccessor:
			TypeStr = TEXT("VariableAccessor");
			break;
		case EMetasoundFrontendClassType::VariableMutator:
			TypeStr = TEXT("VariableMutator");
			break;
		default:
			TypeStr = TEXT("Unknown");
			break;
		}
		DepJson->SetStringField(TEXT("type"), TypeStr);

		TSharedPtr<FJsonObject> DepVersionJson = MakeShared<FJsonObject>();
		DepVersionJson->SetNumberField(TEXT("major"), Dep.Metadata.GetVersion().Major);
		DepVersionJson->SetNumberField(TEXT("minor"), Dep.Metadata.GetVersion().Minor);
		DepJson->SetObjectField(TEXT("version"), DepVersionJson);

		DepsArray.Add(MakeShared<FJsonValueObject>(DepJson));
	}

	TArray<TSharedPtr<FJsonValue>> SubgraphsArray;
	for (const FMetasoundFrontendGraphClass& Subgraph : Doc.Subgraphs)
	{
		TSharedPtr<FJsonObject> SubJson = MakeShared<FJsonObject>();
		SubJson->SetStringField(TEXT("id"), Subgraph.ID.ToString());
		SubJson->SetStringField(TEXT("class_name"), Subgraph.Metadata.GetClassName().ToString());

		const FMetasoundFrontendGraph& SubGraph = Subgraph.GetConstDefaultGraph();
		SubJson->SetNumberField(TEXT("node_count"), SubGraph.Nodes.Num());

		SubgraphsArray.Add(MakeShared<FJsonValueObject>(SubJson));
	}

	Result->SetArrayField(TEXT("dependencies"), DepsArray);
	Result->SetArrayField(TEXT("subgraphs"), SubgraphsArray);
	Result->SetNumberField(TEXT("dependency_count"), DepsArray.Num());
	Result->SetNumberField(TEXT("subgraph_count"), SubgraphsArray.Num());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMetaSoundActions::ListMetaSounds(const TSharedPtr<FJsonObject>& Params)
{
	FString Filter = Params->HasField(TEXT("filter")) ? Params->GetStringField(TEXT("filter")) : FString();
	FString TypeFilter = Params->HasField(TEXT("type")) ? Params->GetStringField(TEXT("type")) : TEXT("All");

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetData> SourceAssets;
	TArray<FAssetData> PatchAssets;

	bool bIncludeSources = TypeFilter.Equals(TEXT("All"), ESearchCase::IgnoreCase) || TypeFilter.Equals(TEXT("Source"), ESearchCase::IgnoreCase);
	bool bIncludePatches = TypeFilter.Equals(TEXT("All"), ESearchCase::IgnoreCase) || TypeFilter.Equals(TEXT("Patch"), ESearchCase::IgnoreCase);

	if (bIncludeSources)
	{
		AssetRegistry.GetAssetsByClass(UMetaSoundSource::StaticClass()->GetClassPathName(), SourceAssets);
	}
	if (bIncludePatches)
	{
		AssetRegistry.GetAssetsByClass(UMetaSoundPatch::StaticClass()->GetClassPathName(), PatchAssets);
	}

	TArray<FAssetData> AllAssets;
	AllAssets.Append(SourceAssets);
	AllAssets.Append(PatchAssets);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> AssetsArray;

	FString FilterLower = Filter.ToLower();

	for (const FAssetData& AssetData : AllAssets)
	{
		FString AssetName = AssetData.AssetName.ToString();
		
		if (!Filter.IsEmpty() && !AssetName.ToLower().Contains(FilterLower))
		{
			continue;
		}

		TSharedPtr<FJsonObject> AssetJson = MakeShared<FJsonObject>();
		AssetJson->SetStringField(TEXT("name"), AssetName);
		AssetJson->SetStringField(TEXT("path"), AssetData.PackageName.ToString());
		AssetJson->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());

		if (AssetData.AssetClassPath == UMetaSoundSource::StaticClass()->GetClassPathName())
		{
			AssetJson->SetStringField(TEXT("type"), TEXT("Source"));
		}
		else
		{
			AssetJson->SetStringField(TEXT("type"), TEXT("Patch"));
		}

		AssetsArray.Add(MakeShared<FJsonValueObject>(AssetJson));
	}

	Result->SetArrayField(TEXT("assets"), AssetsArray);
	Result->SetNumberField(TEXT("count"), AssetsArray.Num());
	Result->SetStringField(TEXT("filter"), Filter);
	Result->SetStringField(TEXT("type_filter"), TypeFilter);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMetaSoundActions::ValidateMetaSound(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	IMetaSoundDocumentInterface* DocInterface = GetDocumentInterface(Asset);
	if (!DocInterface)
	{
		return FMonolithActionResult::Error(TEXT("Asset is not a MetaSound"));
	}

	const FMetasoundFrontendDocument& Doc = DocInterface->GetConstDocument();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);

	TArray<TSharedPtr<FJsonValue>> ErrorsArray;
	TArray<TSharedPtr<FJsonValue>> WarningsArray;

	const FMetasoundFrontendGraph& Graph = Doc.RootGraph.GetConstDefaultGraph();

	TSet<FGuid> NodeIds;
	for (const FMetasoundFrontendNode& Node : Graph.Nodes)
	{
		if (NodeIds.Contains(Node.GetID()))
		{
			TSharedPtr<FJsonObject> ErrJson = MakeShared<FJsonObject>();
			ErrJson->SetStringField(TEXT("type"), TEXT("duplicate_node_id"));
			ErrJson->SetStringField(TEXT("node_id"), Node.GetID().ToString());
			ErrJson->SetStringField(TEXT("node_name"), Node.Name.ToString());
			ErrorsArray.Add(MakeShared<FJsonValueObject>(ErrJson));
		}
		NodeIds.Add(Node.GetID());
	}

	for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
	{
		if (!NodeIds.Contains(Edge.FromNodeID))
		{
			TSharedPtr<FJsonObject> ErrJson = MakeShared<FJsonObject>();
			ErrJson->SetStringField(TEXT("type"), TEXT("dangling_edge"));
			ErrJson->SetStringField(TEXT("message"), TEXT("Edge references non-existent source node"));
			ErrJson->SetStringField(TEXT("from_node_id"), Edge.FromNodeID.ToString());
			ErrorsArray.Add(MakeShared<FJsonValueObject>(ErrJson));
		}
		if (!NodeIds.Contains(Edge.ToNodeID))
		{
			TSharedPtr<FJsonObject> ErrJson = MakeShared<FJsonObject>();
			ErrJson->SetStringField(TEXT("type"), TEXT("dangling_edge"));
			ErrJson->SetStringField(TEXT("message"), TEXT("Edge references non-existent target node"));
			ErrJson->SetStringField(TEXT("to_node_id"), Edge.ToNodeID.ToString());
			ErrorsArray.Add(MakeShared<FJsonValueObject>(ErrJson));
		}
	}

	for (const FMetasoundFrontendNode& Node : Graph.Nodes)
	{
		int32 IncomingCount = 0;
		for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
		{
			if (Edge.ToNodeID == Node.GetID())
			{
				IncomingCount++;
			}
		}

		bool bHasDefaults = Node.InputLiterals.Num() > 0;
		int32 UnconnectedInputs = Node.Interface.Inputs.Num() - IncomingCount;

		if (UnconnectedInputs > 0 && !bHasDefaults)
		{
			TSharedPtr<FJsonObject> WarnJson = MakeShared<FJsonObject>();
			WarnJson->SetStringField(TEXT("type"), TEXT("unconnected_inputs"));
			WarnJson->SetStringField(TEXT("node_id"), Node.GetID().ToString());
			WarnJson->SetStringField(TEXT("node_name"), Node.Name.ToString());
			WarnJson->SetNumberField(TEXT("unconnected_count"), UnconnectedInputs);
			WarningsArray.Add(MakeShared<FJsonValueObject>(WarnJson));
		}
	}

	Result->SetArrayField(TEXT("errors"), ErrorsArray);
	Result->SetArrayField(TEXT("warnings"), WarningsArray);
	Result->SetNumberField(TEXT("error_count"), ErrorsArray.Num());
	Result->SetNumberField(TEXT("warning_count"), WarningsArray.Num());
	Result->SetBoolField(TEXT("valid"), ErrorsArray.Num() == 0);

	return FMonolithActionResult::Success(Result);
}
