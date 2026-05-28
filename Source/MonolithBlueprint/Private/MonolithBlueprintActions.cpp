#include "MonolithBlueprintActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/SceneComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectHash.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CreateDelegate.h"

// --- Registration ---

void FMonolithBlueprintActions::RegisterActions()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(TEXT("blueprint"), TEXT("list_graphs"),
		TEXT("List all graphs in a Blueprint asset"),
		FMonolithActionHandler::CreateStatic(&HandleListGraphs),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_graph_data"),
		TEXT("Get full graph data with all nodes, pins, and connections. Optional node_class_filter to include only matching node classes."),
		FMonolithActionHandler::CreateStatic(&HandleGetGraphData),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (defaults to first event graph)"))
			.Optional(TEXT("node_class_filter"), TEXT("string"), TEXT("Only include nodes whose class contains this substring"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_graph_summary"),
		TEXT("Get lightweight graph summary with node id/class/title and exec-only connections. Returns all graphs when graph_name is empty."),
		FMonolithActionHandler::CreateStatic(&HandleGetGraphSummary),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (returns all graphs when empty)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_variables"),
		TEXT("Get all variables defined in a Blueprint. Set include_bind_widgets=true on a Widget Blueprint to also enumerate, under a 'bind_widgets' array, both C++ BindWidget/BindWidgetOptional references (source=bind_widget_meta) and pure-Blueprint tree widgets exposed as variables (source=tree_variable). Neither kind appears in NewVariables."),
		FMonolithActionHandler::CreateStatic(&HandleGetVariables),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Optional(TEXT("include_bind_widgets"), TEXT("boolean"), TEXT("Widget Blueprints only: also list designer-bound widgets in 'bind_widgets' -- C++ BindWidget refs (source=bind_widget_meta) and bIsVariable tree widgets (source=tree_variable) (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_execution_flow"),
		TEXT("Get linearized execution flow from an entry point"),
		FMonolithActionHandler::CreateStatic(&HandleGetExecutionFlow),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("entry_point"), TEXT("string"), TEXT("Event or function entry point name"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("search_nodes"),
		TEXT("Search for nodes in a Blueprint by title or function name"),
		FMonolithActionHandler::CreateStatic(&HandleSearchNodes),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("query"), TEXT("string"), TEXT("Search string to match against node titles and function names"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_components"),
		TEXT("Get component hierarchy for a Blueprint — names, classes, parent-child tree, attach sockets"),
		FMonolithActionHandler::CreateStatic(&HandleGetComponents),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_component_details"),
		TEXT("Get full property dump for a specific component in a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleGetComponentDetails),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Component variable name"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_functions"),
		TEXT("Get all Blueprint-defined functions with inputs, outputs, metadata, and flags"),
		FMonolithActionHandler::CreateStatic(&HandleGetFunctions),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_event_dispatchers"),
		TEXT("Get all event dispatchers (multicast delegates) defined in a Blueprint with their signature pins"),
		FMonolithActionHandler::CreateStatic(&HandleGetEventDispatchers),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_parent_class"),
		TEXT("Get parent class info, Blueprint type, and class flags for a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleGetParentClass),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_interfaces"),
		TEXT("Get all interfaces implemented by a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleGetInterfaces),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_construction_script"),
		TEXT("Get all nodes in the Construction Script graph"),
		FMonolithActionHandler::CreateStatic(&HandleGetConstructionScript),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("search_functions"),
		TEXT("Search for Blueprint-callable functions across all loaded C++ classes. Returns function names, classes, param lists. Results are cached on first call for the session. At least one of 'query' or 'class_filter' is required."),
		FMonolithActionHandler::CreateStatic(&HandleSearchFunctions),
		FParamSchemaBuilder()
			.Optional(TEXT("query"),             TEXT("string"),  TEXT("Search string matched against function name, class name, and category (case-insensitive contains). Required if class_filter is empty."))
			.Optional(TEXT("class_filter"),      TEXT("string"),  TEXT("Restrict results to a specific class name (case-insensitive contains). Required if query is empty."))
			.Optional(TEXT("include_inherited"), TEXT("boolean"), TEXT("Include inherited functions (default: true)"))
			.Optional(TEXT("pure_only"),         TEXT("boolean"), TEXT("Only return pure (no exec pins) functions (default: false)"))
			.Optional(TEXT("limit"),             TEXT("integer"), TEXT("Max results to return (default: 50)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_node_details"),
		TEXT("Get full pin dump for a single node by node_id. Returns same data as get_graph_data for one node, including orphaned pin flag."),
		FMonolithActionHandler::CreateStatic(&HandleGetNodeDetails),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("node_id"),    TEXT("string"), TEXT("Node ID (from get_graph_data or add_node response)"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name to narrow search (searches all graphs if omitted)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_interface_functions"),
		TEXT("Query the function signatures required by a Blueprint interface. Works for both C++ and Blueprint interfaces."),
		FMonolithActionHandler::CreateStatic(&HandleGetInterfaceFunctions),
		FParamSchemaBuilder()
			.Required(TEXT("interface_class"), TEXT("string"), TEXT("Interface class name (e.g. BPI_Interactable or IInteractable)"))
			.Build());

	// ---- Wave 6 ----

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_function_signature"),
		TEXT("Get full signature for a single named function: inputs, outputs, flags, local variables, and source (blueprint/native/interface). Use include_inherited to also search parent class native functions."),
		FMonolithActionHandler::CreateStatic(&HandleGetFunctionSignature),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"),         TEXT("string"),  TEXT("Blueprint asset path"))
			.Required(TEXT("function_name"),       TEXT("string"),  TEXT("Function name to look up"))
			.Optional(TEXT("include_inherited"),   TEXT("boolean"), TEXT("Also search inherited native functions on the parent class (default: false)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_event_dispatcher_details"),
		TEXT("Get full details for a single event dispatcher: signature pins plus all graph nodes that reference it (CreateDelegate, Call, Bind, Unbind)."),
		FMonolithActionHandler::CreateStatic(&HandleGetEventDispatcherDetails),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"),       TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("dispatcher_name"),  TEXT("string"), TEXT("Event dispatcher name (without _Signature suffix)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_blueprint_info"),
		TEXT("Comprehensive Blueprint overview in one call: parent class, graph names, tick/construction script presence, variable/function/component/interface counts, and compile status."),
		FMonolithActionHandler::CreateStatic(&HandleGetBlueprintInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());
}

// --- Shared helper ---

UBlueprint* FMonolithBlueprintActions::LoadBlueprint(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath)
{
	return MonolithBlueprintInternal::LoadBlueprintFromParams(Params, OutAssetPath);
}

// --- list_graphs ---

FMonolithActionResult FMonolithBlueprintActions::HandleListGraphs(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("class"), BP->GetClass()->GetName());

	if (BP->ParentClass)
	{
		Root->SetStringField(TEXT("parent_class"), BP->ParentClass->GetName());
	}

	TArray<TSharedPtr<FJsonValue>> GraphsArr;
	MonolithBlueprintInternal::AddGraphArray(GraphsArr, BP->UbergraphPages, TEXT("event_graph"));
	MonolithBlueprintInternal::AddGraphArray(GraphsArr, BP->FunctionGraphs, TEXT("function"));
	MonolithBlueprintInternal::AddGraphArray(GraphsArr, BP->MacroGraphs, TEXT("macro"));
	MonolithBlueprintInternal::AddGraphArray(GraphsArr, BP->DelegateSignatureGraphs, TEXT("delegate_signature"));
	Root->SetArrayField(TEXT("graphs"), GraphsArr);

	return FMonolithActionResult::Success(Root);
}

// --- get_graph_data ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetGraphData(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString GraphName = Params->GetStringField(TEXT("graph_name"));
	FString ClassFilter = Params->GetStringField(TEXT("node_class_filter"));
	UEdGraph* Graph = MonolithBlueprintInternal::FindGraphByName(BP, GraphName);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("graph_name"), Graph->GetName());

	FString GraphType = TEXT("unknown");
	if (BP->UbergraphPages.Contains(Graph)) GraphType = TEXT("event_graph");
	else if (BP->FunctionGraphs.Contains(Graph)) GraphType = TEXT("function");
	else if (BP->MacroGraphs.Contains(Graph)) GraphType = TEXT("macro");
	else if (BP->DelegateSignatureGraphs.Contains(Graph)) GraphType = TEXT("delegate_signature");
	Root->SetStringField(TEXT("graph_type"), GraphType);

	TArray<TSharedPtr<FJsonValue>> NodesArr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		if (!ClassFilter.IsEmpty() && !Node->GetClass()->GetName().Contains(ClassFilter)) continue;
		NodesArr.Add(MakeShared<FJsonValueObject>(MonolithBlueprintInternal::SerializeNode(Node)));
	}
	Root->SetArrayField(TEXT("nodes"), NodesArr);

	return FMonolithActionResult::Success(Root);
}

// --- get_graph_summary ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetGraphSummary(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	// Lambda: summarize a single graph into a node array
	auto SummarizeGraph = [](UEdGraph* Graph, TArray<TSharedPtr<FJsonValue>>& OutNodes)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;

			TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
			NObj->SetStringField(TEXT("id"), Node->GetName());
			NObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
			NObj->SetStringField(TEXT("title"),
				Node->GetNodeTitle(ENodeTitleType::ListView).ToString());

			if (!Node->NodeComment.IsEmpty())
			{
				NObj->SetStringField(TEXT("comment"), Node->NodeComment);
			}

			// Collect exec-only output connections
			TArray<TSharedPtr<FJsonValue>> ExecTargets;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;

				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (!Linked || !Linked->GetOwningNode()) continue;
					ExecTargets.Add(MakeShared<FJsonValueString>(Linked->GetOwningNode()->GetName()));
				}
			}
			if (ExecTargets.Num() > 0)
			{
				NObj->SetArrayField(TEXT("exec_targets"), ExecTargets);
			}

			OutNodes.Add(MakeShared<FJsonValueObject>(NObj));
		}
	};

	FString GraphName = Params->GetStringField(TEXT("graph_name"));

	// When graph_name is provided, return single graph at root level (backward compat)
	if (!GraphName.IsEmpty())
	{
		UEdGraph* Graph = MonolithBlueprintInternal::FindGraphByName(BP, GraphName);
		if (!Graph)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("graph_name"), Graph->GetName());

		TArray<TSharedPtr<FJsonValue>> NodesArr;
		SummarizeGraph(Graph, NodesArr);
		Root->SetArrayField(TEXT("nodes"), NodesArr);
		Root->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());

		return FMonolithActionResult::Success(Root);
	}

	// When graph_name is empty, return all graphs
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> GraphsArr;
	int32 TotalNodeCount = 0;

	struct FGraphArrayInfo
	{
		const TArray<TObjectPtr<UEdGraph>>* Graphs;
		const TCHAR* Type;
	};

	TArray<FGraphArrayInfo> GraphArrays = {
		{ &BP->UbergraphPages, TEXT("event_graph") },
		{ &BP->FunctionGraphs, TEXT("function") },
		{ &BP->MacroGraphs, TEXT("macro") },
		{ &BP->DelegateSignatureGraphs, TEXT("delegate_signature") }
	};

	for (const FGraphArrayInfo& Info : GraphArrays)
	{
		for (const auto& Graph : *Info.Graphs)
		{
			if (!Graph) continue;

			TSharedPtr<FJsonObject> GObj = MakeShared<FJsonObject>();
			GObj->SetStringField(TEXT("graph_name"), Graph->GetName());
			GObj->SetStringField(TEXT("graph_type"), Info.Type);

			TArray<TSharedPtr<FJsonValue>> NodesArr;
			SummarizeGraph(Graph, NodesArr);
			GObj->SetArrayField(TEXT("nodes"), NodesArr);
			GObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());

			TotalNodeCount += Graph->Nodes.Num();
			GraphsArr.Add(MakeShared<FJsonValueObject>(GObj));
		}
	}

	Root->SetArrayField(TEXT("graphs"), GraphsArr);
	Root->SetNumberField(TEXT("graph_count"), GraphsArr.Num());
	Root->SetNumberField(TEXT("total_node_count"), TotalNodeCount);

	return FMonolithActionResult::Success(Root);
}

// --- get_variables ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetVariables(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> VarsArr;

	UClass* GenClass = BP->GeneratedClass;
	UObject* CDO = GenClass ? GenClass->GetDefaultObject(false) : nullptr;

	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		TSharedPtr<FJsonObject> VObj = MakeShared<FJsonObject>();
		VObj->SetStringField(TEXT("name"), Var.VarName.ToString());
		VObj->SetStringField(TEXT("type"),
			MonolithBlueprintInternal::ContainerPrefix(Var.VarType) +
			MonolithBlueprintInternal::PinTypeToString(Var.VarType));

		FString DefaultStr = Var.DefaultValue;
		if (DefaultStr.IsEmpty() && CDO && GenClass)
		{
			FProperty* Prop = GenClass->FindPropertyByName(Var.VarName);
			if (Prop)
			{
				const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
				Prop->ExportTextItem_Direct(DefaultStr, ValuePtr, nullptr, CDO, PPF_None);
			}
		}
		VObj->SetStringField(TEXT("default_value"), DefaultStr);
		VObj->SetStringField(TEXT("category"), Var.Category.ToString());

		VObj->SetBoolField(TEXT("instance_editable"),
			(Var.PropertyFlags & CPF_DisableEditOnInstance) == 0);
		VObj->SetBoolField(TEXT("blueprint_read_only"),
			(Var.PropertyFlags & CPF_BlueprintReadOnly) != 0);
		VObj->SetBoolField(TEXT("expose_on_spawn"),
			(Var.PropertyFlags & CPF_ExposeOnSpawn) != 0);
		VObj->SetBoolField(TEXT("replicated"),
			(Var.PropertyFlags & CPF_Net) != 0);
		VObj->SetBoolField(TEXT("transient"),
			(Var.PropertyFlags & CPF_Transient) != 0);

		VarsArr.Add(MakeShared<FJsonValueObject>(VObj));
	}

	// Optional: also surface designer-bound widget-tree references for Widget
	// Blueprints. These are NOT in BP->NewVariables. Two distinct kinds are
	// enumerated into a single 'bind_widgets' array, distinguished by 'source':
	//   - "bind_widget_meta": C++ FObjectProperty fields on the generated class
	//     carrying BindWidget/BindWidgetOptional metadata (the parent-class case).
	//   - "tree_variable": pure-Blueprint UWidgets in the WidgetTree marked
	//     "Is Variable" (UWidget::bIsVariable == true). This is the common case
	//     for a Widget Blueprint whose parent class declares no BindWidget props.
	// Both passes are reflection-only (FObjectProperty/FBoolProperty + a
	// string-resolved UWidget base + GetObjectsWithOuter), so this module needs
	// no UMG link dependency and references no sibling/marketplace widget type.
	bool bIncludeBindWidgets = false;
	Params->TryGetBoolField(TEXT("include_bind_widgets"), bIncludeBindWidgets);
	if (bIncludeBindWidgets && GenClass)
	{
		// Resolve UMG's base widget class by path without compile-time UMG dep.
		UClass* WidgetBase = FindObject<UClass>(nullptr, TEXT("/Script/UMG.Widget"));
		if (!WidgetBase)
		{
			WidgetBase = FindFirstObject<UClass>(TEXT("Widget"), EFindFirstObjectOptions::NativeFirst);
		}

		TArray<TSharedPtr<FJsonValue>> BindArr;
		// Names already emitted as bind_widget_meta, so a tree widget that is also
		// a C++ BindWidget property is not reported twice.
		TSet<FString> EmittedNames;
		if (WidgetBase)
		{
			for (TFieldIterator<FObjectProperty> PropIt(GenClass); PropIt; ++PropIt)
			{
				FObjectProperty* ObjProp = *PropIt;
				if (!ObjProp || !ObjProp->PropertyClass || !ObjProp->PropertyClass->IsChildOf(WidgetBase))
				{
					continue;
				}

				// BindWidget (required) or BindWidgetOptional mark designer-bound refs.
				const bool bHasBindWidget         = ObjProp->HasMetaData(TEXT("BindWidget"));
				const bool bHasBindWidgetOptional = ObjProp->HasMetaData(TEXT("BindWidgetOptional"));
				if (!bHasBindWidget && !bHasBindWidgetOptional)
				{
					continue;
				}

				const bool bOptional =
					bHasBindWidgetOptional || ObjProp->GetBoolMetaData(TEXT("OptionalWidget"));

				TSharedPtr<FJsonObject> BObj = MakeShared<FJsonObject>();
				BObj->SetStringField(TEXT("name"), ObjProp->GetName());
				BObj->SetStringField(TEXT("widget_class"), ObjProp->PropertyClass->GetName());
				BObj->SetBoolField(TEXT("optional"), bOptional);
				BObj->SetStringField(TEXT("category"), ObjProp->GetMetaData(TEXT("Category")));
				BObj->SetStringField(TEXT("source"), TEXT("bind_widget_meta"));
				BObj->SetBoolField(TEXT("is_bind_widget"), true);
				BindArr.Add(MakeShared<FJsonValueObject>(BObj));
				EmittedNames.Add(ObjProp->GetName());
			}

			// Second pass: bIsVariable widgets in the authoring WidgetTree. The
			// tree is a UPROPERTY (UBaseWidgetBlueprint::WidgetTree) reachable by
			// reflection off the Blueprint asset itself, and every UWidget it owns
			// is outered to it -- so GetObjectsWithOuter collects them without any
			// UMG-typed call. bIsVariable is a reflected FBoolProperty on UWidget.
			if (FObjectProperty* TreeProp = CastField<FObjectProperty>(BP->GetClass()->FindPropertyByName(TEXT("WidgetTree"))))
			{
				UObject* WidgetTreeObj = TreeProp->GetObjectPropertyValue_InContainer(BP);
				if (WidgetTreeObj)
				{
					TArray<UObject*> TreeChildren;
					GetObjectsWithOuter(WidgetTreeObj, TreeChildren, /*bIncludeNestedObjects=*/true);
					for (UObject* Child : TreeChildren)
					{
						if (!Child || !Child->IsA(WidgetBase))
						{
							continue;
						}

						const FBoolProperty* IsVarProp =
							CastField<FBoolProperty>(Child->GetClass()->FindPropertyByName(TEXT("bIsVariable")));
						if (!IsVarProp || !IsVarProp->GetPropertyValue_InContainer(Child))
						{
							continue;
						}

						const FString WidgetName = Child->GetName();
						if (EmittedNames.Contains(WidgetName))
						{
							continue;
						}

						TSharedPtr<FJsonObject> TObj = MakeShared<FJsonObject>();
						TObj->SetStringField(TEXT("name"), WidgetName);
						TObj->SetStringField(TEXT("widget_class"), Child->GetClass()->GetName());
						TObj->SetBoolField(TEXT("optional"), false);
						TObj->SetStringField(TEXT("category"), FString());
						TObj->SetStringField(TEXT("source"), TEXT("tree_variable"));
						TObj->SetBoolField(TEXT("is_bind_widget"), false);
						BindArr.Add(MakeShared<FJsonValueObject>(TObj));
						EmittedNames.Add(WidgetName);
					}
				}
			}
		}
		Root->SetArrayField(TEXT("bind_widgets"), BindArr);
	}

	Root->SetArrayField(TEXT("variables"), VarsArr);
	return FMonolithActionResult::Success(Root);
}

// --- get_execution_flow ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetExecutionFlow(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString EntryPoint = Params->GetStringField(TEXT("entry_point"));
	if (EntryPoint.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: entry_point"));
	}

	UEdGraphNode* EntryNode = nullptr;
	UEdGraph* FoundGraph = nullptr;

	auto SearchGraphs = [&](const TArray<TObjectPtr<UEdGraph>>& Graphs)
	{
		for (const auto& Graph : Graphs)
		{
			if (!Graph) continue;
			UEdGraphNode* Node = MonolithBlueprintInternal::FindEntryNode(Graph, EntryPoint);
			if (Node)
			{
				EntryNode = Node;
				FoundGraph = Graph;
				return;
			}
		}
	};

	SearchGraphs(BP->UbergraphPages);
	if (!EntryNode) SearchGraphs(BP->FunctionGraphs);
	if (!EntryNode) SearchGraphs(BP->MacroGraphs);

	if (!EntryNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Entry point not found: %s"), *EntryPoint));
	}

	TSet<UEdGraphNode*> Visited;
	TSharedPtr<FJsonObject> Flow = MonolithBlueprintInternal::TraceExecFlow(EntryNode, Visited);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("entry"), EntryPoint);
	Root->SetStringField(TEXT("graph"), FoundGraph->GetName());
	if (Flow)
	{
		Root->SetObjectField(TEXT("flow"), Flow);
	}

	return FMonolithActionResult::Success(Root);
}

// --- search_nodes ---

FMonolithActionResult FMonolithBlueprintActions::HandleSearchNodes(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString Query = Params->GetStringField(TEXT("query"));
	if (Query.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: query"));
	}

	FString QueryLower = Query.ToLower();
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Results;

	auto SearchGraphs = [&](const TArray<TObjectPtr<UEdGraph>>& Graphs, const FString& Type)
	{
		for (const auto& Graph : Graphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;
				FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				FString ClassName = Node->GetClass()->GetName();
				FString FuncName;

				if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
				{
					FuncName = CallNode->FunctionReference.GetMemberName().ToString();
				}

				bool bMatch = Title.ToLower().Contains(QueryLower) ||
							  ClassName.ToLower().Contains(QueryLower) ||
							  FuncName.ToLower().Contains(QueryLower);

				if (bMatch)
				{
					TSharedPtr<FJsonObject> RObj = MakeShared<FJsonObject>();
					RObj->SetStringField(TEXT("graph"), Graph->GetName());
					RObj->SetStringField(TEXT("graph_type"), Type);
					RObj->SetStringField(TEXT("node_id"), Node->GetName());
					RObj->SetStringField(TEXT("class"), ClassName);
					RObj->SetStringField(TEXT("title"), Title);
					if (!FuncName.IsEmpty())
					{
						RObj->SetStringField(TEXT("function"), FuncName);
					}
					Results.Add(MakeShared<FJsonValueObject>(RObj));
				}
			}
		}
	};

	SearchGraphs(BP->UbergraphPages, TEXT("event_graph"));
	SearchGraphs(BP->FunctionGraphs, TEXT("function"));
	SearchGraphs(BP->MacroGraphs, TEXT("macro"));
	SearchGraphs(BP->DelegateSignatureGraphs, TEXT("delegate_signature"));

	Root->SetStringField(TEXT("query"), Query);
	Root->SetNumberField(TEXT("match_count"), Results.Num());
	Root->SetArrayField(TEXT("results"), Results);

	return FMonolithActionResult::Success(Root);
}

// --- get_components ---

namespace
{
	// Forward declaration for recursive serialization
	TSharedPtr<FJsonObject> SerializeSCSNode(USCS_Node* Node);

	TSharedPtr<FJsonObject> SerializeSCSNode(USCS_Node* Node)
	{
		if (!Node) return nullptr;

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

		FString VarName = Node->GetVariableName().ToString();
		Obj->SetStringField(TEXT("variable_name"), VarName);
		Obj->SetStringField(TEXT("name"), VarName);

		if (Node->ComponentClass)
		{
			Obj->SetStringField(TEXT("class"), Node->ComponentClass->GetName());
			Obj->SetBoolField(TEXT("is_scene_component"),
				Node->ComponentClass->IsChildOf(USceneComponent::StaticClass()));
		}
		else
		{
			Obj->SetStringField(TEXT("class"), TEXT("unknown"));
			Obj->SetBoolField(TEXT("is_scene_component"), false);
		}

		Obj->SetBoolField(TEXT("is_root"), Node->IsRootNode());

		FName AttachSocket = Node->AttachToName;
		if (!AttachSocket.IsNone())
		{
			Obj->SetStringField(TEXT("attach_to"), AttachSocket.ToString());
		}

		// Serialize children recursively
		TArray<TSharedPtr<FJsonValue>> ChildrenArr;
		for (USCS_Node* Child : Node->GetChildNodes())
		{
			TSharedPtr<FJsonObject> ChildObj = SerializeSCSNode(Child);
			if (ChildObj)
			{
				ChildrenArr.Add(MakeShared<FJsonValueObject>(ChildObj));
			}
		}
		Obj->SetArrayField(TEXT("children"), ChildrenArr);

		return Obj;
	}
} // anonymous namespace

FMonolithActionResult FMonolithBlueprintActions::HandleGetComponents(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	TArray<TSharedPtr<FJsonValue>> ComponentsArr;
	int32 ComponentCount = 0;

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	if (SCS)
	{
		// Walk root nodes — each represents a top-level component
		for (USCS_Node* RootNode : SCS->GetRootNodes())
		{
			if (!RootNode) continue;
			TSharedPtr<FJsonObject> NodeObj = SerializeSCSNode(RootNode);
			if (NodeObj)
			{
				ComponentsArr.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
		}

		// Count all nodes (flat) for the component_count field
		TArray<USCS_Node*> AllNodes = SCS->GetAllNodes();
		ComponentCount = AllNodes.Num();
	}

	// Also surface inherited native components from the parent class chain
	TArray<TSharedPtr<FJsonValue>> NativeComponentsArr;
	if (BP->ParentClass && BP->ParentClass->IsChildOf(AActor::StaticClass()))
	{
		AActor* CDO = Cast<AActor>(BP->ParentClass->GetDefaultObject(false));
		if (CDO)
		{
			TArray<UActorComponent*> NativeComps;
			CDO->GetComponents(NativeComps);
			for (UActorComponent* Comp : NativeComps)
			{
				if (!Comp) continue;
				TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
				NObj->SetStringField(TEXT("name"), Comp->GetName());
				NObj->SetStringField(TEXT("variable_name"), Comp->GetName());
				NObj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());
				NObj->SetBoolField(TEXT("is_scene_component"), Comp->IsA(USceneComponent::StaticClass()));
				NObj->SetBoolField(TEXT("is_native"), true);

				if (USceneComponent* SceneComp = Cast<USceneComponent>(Comp))
				{
					if (USceneComponent* AttachParent = SceneComp->GetAttachParent())
					{
						NObj->SetStringField(TEXT("parent"), AttachParent->GetName());
					}
					NObj->SetBoolField(TEXT("is_root"), SceneComp->GetAttachParent() == nullptr);
				}
				NativeComponentsArr.Add(MakeShared<FJsonValueObject>(NObj));
			}
		}
	}

	Root->SetArrayField(TEXT("components"), ComponentsArr);
	Root->SetNumberField(TEXT("component_count"), ComponentCount);
	if (NativeComponentsArr.Num() > 0)
	{
		Root->SetArrayField(TEXT("inherited_native_components"), NativeComponentsArr);
	}

	return FMonolithActionResult::Success(Root);
}

// --- get_component_details ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetComponentDetails(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString ComponentName = Params->GetStringField(TEXT("component_name"));
	if (ComponentName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: component_name"));
	}

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	if (!SCS)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript (not an Actor Blueprint?)"));
	}

	USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
	if (!Node)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
	}

	UActorComponent* Template = Node->ComponentTemplate;
	if (!Template)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component template is null for: %s"), *ComponentName));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("component_name"), ComponentName);
	Root->SetStringField(TEXT("class"), Template->GetClass()->GetName());
	Root->SetBoolField(TEXT("is_scene_component"), Template->IsA(USceneComponent::StaticClass()));

	// For USceneComponent, include transform explicitly
	if (USceneComponent* SceneTemplate = Cast<USceneComponent>(Template))
	{
		TSharedPtr<FJsonObject> TransformObj = MakeShared<FJsonObject>();

		FVector Loc = SceneTemplate->GetRelativeLocation();
		TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
		LocObj->SetNumberField(TEXT("x"), Loc.X);
		LocObj->SetNumberField(TEXT("y"), Loc.Y);
		LocObj->SetNumberField(TEXT("z"), Loc.Z);
		TransformObj->SetObjectField(TEXT("relative_location"), LocObj);

		FRotator Rot = SceneTemplate->GetRelativeRotation();
		TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
		RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
		RotObj->SetNumberField(TEXT("yaw"), Rot.Yaw);
		RotObj->SetNumberField(TEXT("roll"), Rot.Roll);
		TransformObj->SetObjectField(TEXT("relative_rotation"), RotObj);

		FVector Scale = SceneTemplate->GetRelativeScale3D();
		TSharedPtr<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
		ScaleObj->SetNumberField(TEXT("x"), Scale.X);
		ScaleObj->SetNumberField(TEXT("y"), Scale.Y);
		ScaleObj->SetNumberField(TEXT("z"), Scale.Z);
		TransformObj->SetObjectField(TEXT("relative_scale"), ScaleObj);

		Root->SetObjectField(TEXT("transform"), TransformObj);
	}

	// Iterate properties via reflection — include CPF_Edit or CPF_BlueprintVisible
	TArray<TSharedPtr<FJsonValue>> PropsArr;
	UClass* ComponentClass = Template->GetClass();

	// Walk class hierarchy, stop at UActorComponent (don't dump UObject internals)
	for (TFieldIterator<FProperty> PropIt(ComponentClass); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop) continue;

		// Only expose editor-visible or Blueprint-accessible properties
		if (!(Prop->PropertyFlags & (CPF_Edit | CPF_BlueprintVisible))) continue;

		// Skip properties that belong to UObject itself (too low-level / internal)
		if (Prop->GetOwnerClass() == UObject::StaticClass()) continue;

		FString ValueStr;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Template);
		Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, Template, PPF_None);

		TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
		PropObj->SetStringField(TEXT("name"), Prop->GetName());
		PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());
		PropObj->SetStringField(TEXT("value"), ValueStr);

		// Include the category metadata if present
		FString Category = Prop->GetMetaData(TEXT("Category"));
		if (!Category.IsEmpty())
		{
			PropObj->SetStringField(TEXT("category"), Category);
		}

		PropsArr.Add(MakeShared<FJsonValueObject>(PropObj));
	}

	Root->SetArrayField(TEXT("properties"), PropsArr);
	Root->SetNumberField(TEXT("property_count"), PropsArr.Num());

	return FMonolithActionResult::Success(Root);
}

// --- get_functions ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetFunctions(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TArray<TSharedPtr<FJsonValue>> FuncsArr;

	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (!Graph) continue;

		TSharedPtr<FJsonObject> FObj = MakeShared<FJsonObject>();
		FObj->SetStringField(TEXT("name"), Graph->GetName());

		// Find entry node for metadata and input pins
		UK2Node_FunctionEntry* EntryNode = nullptr;
		UK2Node_FunctionResult* ResultNode = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!EntryNode) EntryNode = Cast<UK2Node_FunctionEntry>(Node);
			if (!ResultNode) ResultNode = Cast<UK2Node_FunctionResult>(Node);
			if (EntryNode && ResultNode) break;
		}

		// Flags from entry node metadata
		if (EntryNode)
		{
			const uint32 ExtraFlags = EntryNode->GetFunctionFlags();
			FObj->SetBoolField(TEXT("is_pure"), (ExtraFlags & FUNC_BlueprintPure) != 0);
			FObj->SetBoolField(TEXT("is_const"), (ExtraFlags & FUNC_Const) != 0);
			FObj->SetBoolField(TEXT("is_static"), (ExtraFlags & FUNC_Static) != 0);
			FObj->SetBoolField(TEXT("call_in_editor"), EntryNode->MetaData.bCallInEditor);
			FObj->SetStringField(TEXT("category"), EntryNode->MetaData.Category.ToString());
			FObj->SetStringField(TEXT("description"), EntryNode->MetaData.ToolTip.ToString());
			FObj->SetStringField(TEXT("access_specifier"),
				(ExtraFlags & FUNC_Protected) ? TEXT("Protected") :
				(ExtraFlags & FUNC_Private)   ? TEXT("Private")   : TEXT("Public"));

			// Inputs: output pins on the entry node (excluding exec)
			TArray<TSharedPtr<FJsonValue>> InputsArr;
			for (const UEdGraphPin* Pin : EntryNode->Pins)
			{
				if (!Pin || Pin->bHidden) continue;
				if (Pin->Direction != EGPD_Output) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
				PObj->SetStringField(TEXT("type"),
					MonolithBlueprintInternal::ContainerPrefix(Pin->PinType) +
					MonolithBlueprintInternal::PinTypeToString(Pin->PinType));
				InputsArr.Add(MakeShared<FJsonValueObject>(PObj));
			}
			FObj->SetArrayField(TEXT("inputs"), InputsArr);
		}
		else
		{
			FObj->SetBoolField(TEXT("is_pure"), false);
			FObj->SetBoolField(TEXT("is_const"), false);
			FObj->SetBoolField(TEXT("is_static"), false);
			FObj->SetBoolField(TEXT("call_in_editor"), false);
			FObj->SetStringField(TEXT("category"), TEXT(""));
			FObj->SetStringField(TEXT("description"), TEXT(""));
			FObj->SetStringField(TEXT("access_specifier"), TEXT("Public"));
			FObj->SetArrayField(TEXT("inputs"), TArray<TSharedPtr<FJsonValue>>());
		}

		// Outputs: input pins on the result node (excluding exec)
		TArray<TSharedPtr<FJsonValue>> OutputsArr;
		if (ResultNode)
		{
			for (const UEdGraphPin* Pin : ResultNode->Pins)
			{
				if (!Pin || Pin->bHidden) continue;
				if (Pin->Direction != EGPD_Input) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
				PObj->SetStringField(TEXT("type"),
					MonolithBlueprintInternal::ContainerPrefix(Pin->PinType) +
					MonolithBlueprintInternal::PinTypeToString(Pin->PinType));
				OutputsArr.Add(MakeShared<FJsonValueObject>(PObj));
			}
		}
		FObj->SetArrayField(TEXT("outputs"), OutputsArr);

		FuncsArr.Add(MakeShared<FJsonValueObject>(FObj));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("functions"), FuncsArr);
	Root->SetNumberField(TEXT("function_count"), FuncsArr.Num());
	return FMonolithActionResult::Success(Root);
}

// --- get_event_dispatchers ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetEventDispatchers(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TArray<TSharedPtr<FJsonValue>> DispatchersArr;

	for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
	{
		if (!Graph) continue;

		TSharedPtr<FJsonObject> DObj = MakeShared<FJsonObject>();

		// Strip the "_Signature" suffix that UE appends to delegate graph names
		FString RawName = Graph->GetName();
		FString DisplayName = RawName;
		if (DisplayName.EndsWith(TEXT("_Signature")))
		{
			DisplayName.LeftChopInline(10, EAllowShrinking::No);
		}
		DObj->SetStringField(TEXT("name"), DisplayName);

		// Signature pins from the entry node (excluding exec)
		TArray<TSharedPtr<FJsonValue>> PinsArr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node);
			if (!EntryNode) continue;

			for (const UEdGraphPin* Pin : EntryNode->Pins)
			{
				if (!Pin || Pin->bHidden) continue;
				if (Pin->Direction != EGPD_Output) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
				PObj->SetStringField(TEXT("type"),
					MonolithBlueprintInternal::ContainerPrefix(Pin->PinType) +
					MonolithBlueprintInternal::PinTypeToString(Pin->PinType));
				PinsArr.Add(MakeShared<FJsonValueObject>(PObj));
			}
			break; // only one entry node per graph
		}
		DObj->SetArrayField(TEXT("signature_pins"), PinsArr);
		DispatchersArr.Add(MakeShared<FJsonValueObject>(DObj));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("event_dispatchers"), DispatchersArr);
	Root->SetNumberField(TEXT("count"), DispatchersArr.Num());
	return FMonolithActionResult::Success(Root);
}

// --- get_parent_class ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetParentClass(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	// Parent class info
	if (BP->ParentClass)
	{
		TSharedPtr<FJsonObject> ParentObj = MakeShared<FJsonObject>();
		ParentObj->SetStringField(TEXT("name"), BP->ParentClass->GetName());
		ParentObj->SetStringField(TEXT("path"), BP->ParentClass->GetPathName());
		Root->SetObjectField(TEXT("parent_class"), ParentObj);
	}

	// Blueprint type
	FString BPTypeStr;
	switch (BP->BlueprintType)
	{
	case BPTYPE_Normal:          BPTypeStr = TEXT("Normal"); break;
	case BPTYPE_Const:           BPTypeStr = TEXT("Const"); break;
	case BPTYPE_MacroLibrary:    BPTypeStr = TEXT("MacroLibrary"); break;
	case BPTYPE_Interface:       BPTypeStr = TEXT("Interface"); break;
	case BPTYPE_LevelScript:     BPTypeStr = TEXT("LevelScript"); break;
	case BPTYPE_FunctionLibrary: BPTypeStr = TEXT("FunctionLibrary"); break;
	default:                     BPTypeStr = TEXT("Normal"); break;
	}
	Root->SetStringField(TEXT("blueprint_type"), BPTypeStr);

	// Status
	FString StatusStr;
	switch (BP->Status)
	{
	case BS_Unknown:       StatusStr = TEXT("Unknown"); break;
	case BS_Dirty:         StatusStr = TEXT("Dirty"); break;
	case BS_Error:         StatusStr = TEXT("Error"); break;
	case BS_UpToDate:      StatusStr = TEXT("UpToDate"); break;
	case BS_BeingCreated:  StatusStr = TEXT("BeingCreated"); break;
	default:               StatusStr = TEXT("Unknown"); break;
	}
	Root->SetStringField(TEXT("status"), StatusStr);

	// Generated class name
	if (BP->GeneratedClass)
	{
		Root->SetStringField(TEXT("generated_class_name"), BP->GeneratedClass->GetName());
	}

	// Flags
	Root->SetBoolField(TEXT("is_data_only"), FBlueprintEditorUtils::IsDataOnlyBlueprint(BP));
	Root->SetBoolField(TEXT("is_actor_based"), FBlueprintEditorUtils::IsActorBased(BP));

	return FMonolithActionResult::Success(Root);
}

// --- get_interfaces ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetInterfaces(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TArray<TSharedPtr<FJsonValue>> InterfacesArr;

	for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
	{
		if (!Iface.Interface) continue;

		TSharedPtr<FJsonObject> IObj = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> ClassObj = MakeShared<FJsonObject>();
		ClassObj->SetStringField(TEXT("name"), Iface.Interface->GetName());
		ClassObj->SetStringField(TEXT("path"), Iface.Interface->GetPathName());
		IObj->SetObjectField(TEXT("interface_class"), ClassObj);
		IObj->SetBoolField(TEXT("is_inherited"), false);
		InterfacesArr.Add(MakeShared<FJsonValueObject>(IObj));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("interfaces"), InterfacesArr);
	Root->SetNumberField(TEXT("count"), InterfacesArr.Num());
	return FMonolithActionResult::Success(Root);
}

// --- get_construction_script ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetConstructionScript(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	UEdGraph* CSGraph = FBlueprintEditorUtils::FindUserConstructionScript(BP);
	if (!CSGraph)
	{
		return FMonolithActionResult::Error(TEXT("No Construction Script found (Blueprint may not have one)"));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("graph_name"), CSGraph->GetName());
	Root->SetStringField(TEXT("graph_type"), TEXT("construction_script"));

	TArray<TSharedPtr<FJsonValue>> NodesArr;
	for (UEdGraphNode* Node : CSGraph->Nodes)
	{
		if (!Node) continue;
		NodesArr.Add(MakeShared<FJsonValueObject>(MonolithBlueprintInternal::SerializeNode(Node)));
	}
	Root->SetArrayField(TEXT("nodes"), NodesArr);
	Root->SetNumberField(TEXT("node_count"), NodesArr.Num());

	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  search_functions  (Wave 3)
// ============================================================

namespace
{
	// Cache entry for a single BP-callable function
	struct FFunctionCacheEntry
	{
		FString FunctionName;    // bare name, e.g. K2_GetActorLocation
		FString ClassName;       // e.g. Actor
		FString Category;
		bool bIsPure;
		bool bIsStatic;
		TArray<TSharedPtr<FJsonObject>> Inputs;
		TArray<TSharedPtr<FJsonObject>> Outputs;
	};

	// Build and cache the full list of BP-callable functions across all loaded C++ classes.
	// Called once per editor session; class/function list is stable after module load.
	static TArray<FFunctionCacheEntry>& GetFunctionCache()
	{
		static TArray<FFunctionCacheEntry> Cache;
		static bool bBuilt = false;
		if (bBuilt) return Cache;
		bBuilt = true;

		for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			UClass* Class = *ClassIt;
			if (!Class) continue;

			// Skip Blueprint-generated classes — they duplicate native entries and bloat the cache
			if (Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint)) continue;

			// Skip abstract interface classes (their functions appear on implementing classes)
			// but DO include interface classes themselves — callers may want to discover them
			const FString ClassName = Class->GetName();

			for (TFieldIterator<UFunction> FuncIt(Class, EFieldIterationFlags::None); FuncIt; ++FuncIt)
			{
				UFunction* Func = *FuncIt;
				if (!Func) continue;

				// Must be Blueprint-callable
				if (!Func->HasAnyFunctionFlags(FUNC_BlueprintCallable)) continue;

				// Skip deprecated functions
				if (Func->HasMetaData(TEXT("DeprecatedFunction"))) continue;

				// Only include functions owned by this class (not inherited ones in this iteration pass)
				// We'll add inherited as a separate pass below if requested
				if (Func->GetOwnerClass() != Class) continue;

				FFunctionCacheEntry Entry;
				Entry.FunctionName = Func->GetName();
				Entry.ClassName    = ClassName;
				Entry.Category     = Func->GetMetaData(TEXT("Category"));
				Entry.bIsPure      = Func->HasAnyFunctionFlags(FUNC_BlueprintPure);
				Entry.bIsStatic    = Func->HasAnyFunctionFlags(FUNC_Static);

				// Collect parameters
				for (TFieldIterator<FProperty> PropIt(Func); PropIt && (PropIt->PropertyFlags & CPF_Parm); ++PropIt)
				{
					FProperty* Prop = *PropIt;
					if (!Prop) continue;

					// Build a simple {name, type} object
					TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
					ParamObj->SetStringField(TEXT("name"), Prop->GetName());
					ParamObj->SetStringField(TEXT("type"), Prop->GetCPPType());

					if (Prop->PropertyFlags & CPF_ReturnParm)
					{
						Entry.Outputs.Add(ParamObj);
					}
					else if (Prop->PropertyFlags & CPF_OutParm)
					{
						Entry.Outputs.Add(ParamObj);
					}
					else
					{
						Entry.Inputs.Add(ParamObj);
					}
				}

				Cache.Add(MoveTemp(Entry));
			}
		}

		return Cache;
	}
} // anonymous namespace

FMonolithActionResult FMonolithBlueprintActions::HandleSearchFunctions(const TSharedPtr<FJsonObject>& Params)
{
	FString Query       = Params->GetStringField(TEXT("query"));
	FString ClassFilter = Params->GetStringField(TEXT("class_filter"));

	if (Query.IsEmpty() && ClassFilter.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("At least one of 'query' or 'class_filter' is required to avoid a full scan"));
	}

	bool bPureOnly = false;
	Params->TryGetBoolField(TEXT("pure_only"), bPureOnly);

	// include_inherited controls whether the cache is built with inherited entries exposed.
	// The cache itself only stores functions owned by their declaring class (no duplication).
	// Searches always hit all entries; include_inherited currently controls a future filter path.
	// For now we always search the full cache and let class_filter narrow it.

	int32 Limit = 50;
	{
		double LimitNum = 0.0;
		if (Params->TryGetNumberField(TEXT("limit"), LimitNum) && LimitNum > 0)
		{
			Limit = (int32)LimitNum;
		}
	}

	const FString QueryLower       = Query.ToLower();
	const FString ClassFilterLower = ClassFilter.ToLower();

	const TArray<FFunctionCacheEntry>& Cache = GetFunctionCache();

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FFunctionCacheEntry& Entry : Cache)
	{
		if (Results.Num() >= Limit) break;

		// Class filter
		if (!ClassFilterLower.IsEmpty() && !Entry.ClassName.ToLower().Contains(ClassFilterLower))
		{
			continue;
		}

		// Pure filter
		if (bPureOnly && !Entry.bIsPure) continue;

		// Query match — split on spaces and AND all tokens across function/class/category
		if (!QueryLower.IsEmpty())
		{
			TArray<FString> Tokens;
			QueryLower.ParseIntoArray(Tokens, TEXT(" "), true);
			FString FuncLower = Entry.FunctionName.ToLower();
			FString ClassLower = Entry.ClassName.ToLower();
			FString CatLower = Entry.Category.ToLower();
			bool bAllTokensMatch = true;
			for (const FString& Token : Tokens)
			{
				if (!FuncLower.Contains(Token) && !ClassLower.Contains(Token) && !CatLower.Contains(Token))
				{
					bAllTokensMatch = false;
					break;
				}
			}
			if (!bAllTokensMatch) continue;
		}

		// Build result object
		TSharedPtr<FJsonObject> RObj = MakeShared<FJsonObject>();
		RObj->SetStringField(TEXT("function_name"), Entry.FunctionName);
		RObj->SetStringField(TEXT("class_name"),    Entry.ClassName);
		RObj->SetStringField(TEXT("category"),      Entry.Category);
		RObj->SetBoolField(TEXT("is_pure"),         Entry.bIsPure);
		RObj->SetBoolField(TEXT("is_static"),       Entry.bIsStatic);

		// K2 name: if the function starts with K2_, report both forms
		FString K2Name = Entry.FunctionName;
		if (K2Name.StartsWith(TEXT("K2_")))
		{
			RObj->SetStringField(TEXT("k2_name"),      Entry.FunctionName);
			RObj->SetStringField(TEXT("friendly_name"), Entry.FunctionName.Mid(3));
		}
		else
		{
			RObj->SetStringField(TEXT("k2_name"), Entry.FunctionName);
		}

		TArray<TSharedPtr<FJsonValue>> InputsArr;
		for (const auto& P : Entry.Inputs)
		{
			InputsArr.Add(MakeShared<FJsonValueObject>(P));
		}
		RObj->SetArrayField(TEXT("inputs"), InputsArr);

		TArray<TSharedPtr<FJsonValue>> OutputsArr;
		for (const auto& P : Entry.Outputs)
		{
			OutputsArr.Add(MakeShared<FJsonValueObject>(P));
		}
		RObj->SetArrayField(TEXT("outputs"), OutputsArr);

		Results.Add(MakeShared<FJsonValueObject>(RObj));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("results"), Results);
	Root->SetNumberField(TEXT("count"), Results.Num());
	Root->SetNumberField(TEXT("cache_size"), Cache.Num());

	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  get_node_details  (Wave 3)
// ============================================================

FMonolithActionResult FMonolithBlueprintActions::HandleGetNodeDetails(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString NodeId = Params->GetStringField(TEXT("node_id"));
	if (NodeId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: node_id"));
	}

	FString GraphName = Params->GetStringField(TEXT("graph_name"));
	UEdGraphNode* Node = MonolithBlueprintInternal::FindNodeById(BP, GraphName, NodeId);
	if (!Node)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Node not found: %s"), *NodeId));
	}

	// Use the existing SerializeNode for the base data, then augment with orphan info
	TSharedPtr<FJsonObject> NodeObj = MonolithBlueprintInternal::SerializeNode(Node);

	// Add orphan flag per pin — SerializeNode already includes pins but without bOrphanedPin
	// Rebuild the pins array with the extra field rather than patching the existing one
	TArray<TSharedPtr<FJsonValue>> PinsArr;
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden) continue;

		TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("id"),        Pin->PinId.ToString());
		PinObj->SetStringField(TEXT("name"),      Pin->PinName.ToString());
		PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
		PinObj->SetStringField(TEXT("type"),
			MonolithBlueprintInternal::ContainerPrefix(Pin->PinType) +
			MonolithBlueprintInternal::PinTypeToString(Pin->PinType));
		PinObj->SetBoolField(TEXT("is_exec"),     Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
		PinObj->SetBoolField(TEXT("is_orphaned"), Pin->bOrphanedPin);

		if (!Pin->DefaultValue.IsEmpty())
		{
			PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		}
		if (Pin->DefaultObject)
		{
			PinObj->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
		}

		TArray<TSharedPtr<FJsonValue>> ConnArr;
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (!Linked || !Linked->GetOwningNode()) continue;
			FString ConnId = FString::Printf(TEXT("%s.%s"),
				*Linked->GetOwningNode()->GetName(),
				*Linked->PinName.ToString());
			ConnArr.Add(MakeShared<FJsonValueString>(ConnId));
		}
		PinObj->SetArrayField(TEXT("connected_to"), ConnArr);

		PinsArr.Add(MakeShared<FJsonValueObject>(PinObj));
	}

	// Override the pins array with the augmented version
	NodeObj->SetArrayField(TEXT("pins"), PinsArr);

	// Add graph context
	UEdGraph* OwningGraph = Node->GetGraph();
	if (OwningGraph)
	{
		NodeObj->SetStringField(TEXT("graph_name"), OwningGraph->GetName());
	}
	NodeObj->SetStringField(TEXT("asset_path"), AssetPath);

	return FMonolithActionResult::Success(NodeObj);
}

// ============================================================
//  get_interface_functions  (Wave 3)
// ============================================================

FMonolithActionResult FMonolithBlueprintActions::HandleGetInterfaceFunctions(const TSharedPtr<FJsonObject>& Params)
{
	FString InterfaceClassName = Params->GetStringField(TEXT("interface_class"));
	if (InterfaceClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: interface_class"));
	}

	// Resolve class — try as-is, then with U/I prefix variants
	UClass* InterfaceClass = FindFirstObject<UClass>(*InterfaceClassName, EFindFirstObjectOptions::NativeFirst);
	if (!InterfaceClass)
	{
		// Try with U prefix (C++ convention for UInterface base)
		InterfaceClass = FindFirstObject<UClass>(*FString::Printf(TEXT("U%s"), *InterfaceClassName), EFindFirstObjectOptions::NativeFirst);
	}
	if (!InterfaceClass)
	{
		// Strip leading I (BP convention: BPI_Foo -> search for UBPI_Foo)
		if (InterfaceClassName.StartsWith(TEXT("I")))
		{
			InterfaceClass = FindFirstObject<UClass>(*FString::Printf(TEXT("U%s"), *InterfaceClassName.Mid(1)), EFindFirstObjectOptions::NativeFirst);
		}
	}

	if (!InterfaceClass)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Interface class not found: '%s'. Tried as-is, with 'U' prefix, and with 'I' stripped + 'U' prepended. "
			     "For C++ interfaces use the I-prefixed name (e.g. IGameplayTagAssetInterface). "
			     "For Blueprint interfaces use the asset name (e.g. BPI_Interactable)."),
			*InterfaceClassName));
	}

	if (!InterfaceClass->HasAnyClassFlags(CLASS_Interface))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("'%s' is not an interface class"), *InterfaceClassName));
	}

	TArray<TSharedPtr<FJsonValue>> FunctionsArr;

	// Iterate functions on the interface class (not inherited — interface functions are declared here)
	for (TFieldIterator<UFunction> FuncIt(InterfaceClass, EFieldIterationFlags::None); FuncIt; ++FuncIt)
	{
		UFunction* Func = *FuncIt;
		if (!Func) continue;
		if (Func->GetOwnerClass() != InterfaceClass) continue;

		// Interface functions are BlueprintCallable or BlueprintImplementableEvent
		if (!Func->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintEvent)) continue;

		TSharedPtr<FJsonObject> FObj = MakeShared<FJsonObject>();
		FObj->SetStringField(TEXT("name"), Func->GetName());
		FObj->SetBoolField(TEXT("is_const"), Func->HasAnyFunctionFlags(FUNC_Const));

		// A function becomes an event in BP if it has no return/out params
		// (Blueprint fires it as a one-way event rather than a callable function)
		bool bHasOutParms = false;
		for (TFieldIterator<FProperty> PropIt(Func); PropIt && (PropIt->PropertyFlags & CPF_Parm); ++PropIt)
		{
			if (PropIt->PropertyFlags & (CPF_ReturnParm | CPF_OutParm))
			{
				bHasOutParms = true;
				break;
			}
		}
		FObj->SetBoolField(TEXT("is_event"), !bHasOutParms && Func->HasAnyFunctionFlags(FUNC_BlueprintEvent));

		// Collect inputs and outputs
		TArray<TSharedPtr<FJsonValue>> InputsArr;
		TArray<TSharedPtr<FJsonValue>> OutputsArr;

		for (TFieldIterator<FProperty> PropIt(Func); PropIt && (PropIt->PropertyFlags & CPF_Parm); ++PropIt)
		{
			FProperty* Prop = *PropIt;
			if (!Prop) continue;

			TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
			ParamObj->SetStringField(TEXT("name"), Prop->GetName());
			ParamObj->SetStringField(TEXT("type"), Prop->GetCPPType());

			if (Prop->PropertyFlags & CPF_ReturnParm)
			{
				OutputsArr.Add(MakeShared<FJsonValueObject>(ParamObj));
			}
			else if (Prop->PropertyFlags & CPF_OutParm)
			{
				OutputsArr.Add(MakeShared<FJsonValueObject>(ParamObj));
			}
			else
			{
				InputsArr.Add(MakeShared<FJsonValueObject>(ParamObj));
			}
		}

		FObj->SetArrayField(TEXT("inputs"),  InputsArr);
		FObj->SetArrayField(TEXT("outputs"), OutputsArr);

		FunctionsArr.Add(MakeShared<FJsonValueObject>(FObj));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("interface_name"), InterfaceClass->GetName());
	Root->SetStringField(TEXT("interface_path"), InterfaceClass->GetPathName());
	Root->SetArrayField(TEXT("functions"),       FunctionsArr);
	Root->SetNumberField(TEXT("function_count"), FunctionsArr.Num());

	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  get_function_signature  (Wave 6)
// ============================================================

FMonolithActionResult FMonolithBlueprintActions::HandleGetFunctionSignature(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString FuncName = Params->GetStringField(TEXT("function_name"));
	if (FuncName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: function_name"));
	}

	bool bIncludeInherited = false;
	Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	// --- 1. Search BP-defined function graphs ---
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (!Graph || Graph->GetName() != FuncName) continue;

		Root->SetStringField(TEXT("name"), Graph->GetName());
		Root->SetStringField(TEXT("source"), TEXT("blueprint"));

		UK2Node_FunctionEntry* EntryNode = nullptr;
		UK2Node_FunctionResult* ResultNode = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!EntryNode) EntryNode = Cast<UK2Node_FunctionEntry>(Node);
			if (!ResultNode) ResultNode = Cast<UK2Node_FunctionResult>(Node);
			if (EntryNode && ResultNode) break;
		}

		if (EntryNode)
		{
			const uint32 Flags = EntryNode->GetFunctionFlags();
			Root->SetBoolField(TEXT("is_pure"),   (Flags & FUNC_BlueprintPure) != 0);
			Root->SetBoolField(TEXT("is_const"),  (Flags & FUNC_Const) != 0);
			Root->SetBoolField(TEXT("is_static"), (Flags & FUNC_Static) != 0);
			Root->SetBoolField(TEXT("call_in_editor"), EntryNode->MetaData.bCallInEditor);
			Root->SetStringField(TEXT("category"),    EntryNode->MetaData.Category.ToString());
			Root->SetStringField(TEXT("description"), EntryNode->MetaData.ToolTip.ToString());
			Root->SetStringField(TEXT("access"),
				(Flags & FUNC_Protected) ? TEXT("Protected") :
				(Flags & FUNC_Private)   ? TEXT("Private")   : TEXT("Public"));

			// Inputs (output pins on entry, excl. exec)
			TArray<TSharedPtr<FJsonValue>> InputsArr;
			for (const UEdGraphPin* Pin : EntryNode->Pins)
			{
				if (!Pin || Pin->bHidden) continue;
				if (Pin->Direction != EGPD_Output) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
				PObj->SetStringField(TEXT("type"),
					MonolithBlueprintInternal::ContainerPrefix(Pin->PinType) +
					MonolithBlueprintInternal::PinTypeToString(Pin->PinType));
				if (!Pin->DefaultValue.IsEmpty())
				{
					PObj->SetStringField(TEXT("default"), Pin->DefaultValue);
				}
				InputsArr.Add(MakeShared<FJsonValueObject>(PObj));
			}
			Root->SetArrayField(TEXT("inputs"), InputsArr);

			// Local variables from the entry node
			TArray<TSharedPtr<FJsonValue>> LocalsArr;
			for (const FBPVariableDescription& LVar : EntryNode->LocalVariables)
			{
				TSharedPtr<FJsonObject> LObj = MakeShared<FJsonObject>();
				LObj->SetStringField(TEXT("name"), LVar.VarName.ToString());
				// Build type string from the variable description's pin type
				FEdGraphPinType PT;
				PT.PinCategory         = LVar.VarType.PinCategory;
				PT.PinSubCategory      = LVar.VarType.PinSubCategory;
				PT.PinSubCategoryObject = LVar.VarType.PinSubCategoryObject;
				PT.ContainerType       = LVar.VarType.ContainerType;
				LObj->SetStringField(TEXT("type"),
					MonolithBlueprintInternal::ContainerPrefix(PT) +
					MonolithBlueprintInternal::PinTypeToString(PT));
				LocalsArr.Add(MakeShared<FJsonValueObject>(LObj));
			}
			Root->SetArrayField(TEXT("local_variables"), LocalsArr);
		}

		// Outputs (input pins on result, excl. exec)
		TArray<TSharedPtr<FJsonValue>> OutputsArr;
		if (ResultNode)
		{
			for (const UEdGraphPin* Pin : ResultNode->Pins)
			{
				if (!Pin || Pin->bHidden) continue;
				if (Pin->Direction != EGPD_Input) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
				PObj->SetStringField(TEXT("type"),
					MonolithBlueprintInternal::ContainerPrefix(Pin->PinType) +
					MonolithBlueprintInternal::PinTypeToString(Pin->PinType));
				OutputsArr.Add(MakeShared<FJsonValueObject>(PObj));
			}
		}
		Root->SetArrayField(TEXT("outputs"), OutputsArr);

		return FMonolithActionResult::Success(Root);
	}

	// --- 2. Check delegate signature graphs (event dispatchers) ---
	for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
	{
		if (!Graph) continue;
		FString DisplayName = Graph->GetName();
		if (DisplayName.EndsWith(TEXT("_Signature")))
		{
			DisplayName.LeftChopInline(10, EAllowShrinking::No);
		}
		if (DisplayName != FuncName) continue;

		Root->SetStringField(TEXT("name"), DisplayName);
		Root->SetStringField(TEXT("source"), TEXT("event_dispatcher"));
		Root->SetBoolField(TEXT("is_pure"), false);
		Root->SetBoolField(TEXT("is_const"), false);
		Root->SetBoolField(TEXT("is_static"), false);

		TArray<TSharedPtr<FJsonValue>> InputsArr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node);
			if (!EntryNode) continue;
			for (const UEdGraphPin* Pin : EntryNode->Pins)
			{
				if (!Pin || Pin->bHidden) continue;
				if (Pin->Direction != EGPD_Output) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
				PObj->SetStringField(TEXT("type"),
					MonolithBlueprintInternal::ContainerPrefix(Pin->PinType) +
					MonolithBlueprintInternal::PinTypeToString(Pin->PinType));
				InputsArr.Add(MakeShared<FJsonValueObject>(PObj));
			}
			break;
		}
		Root->SetArrayField(TEXT("inputs"), InputsArr);
		Root->SetArrayField(TEXT("outputs"), TArray<TSharedPtr<FJsonValue>>());
		Root->SetArrayField(TEXT("local_variables"), TArray<TSharedPtr<FJsonValue>>());
		return FMonolithActionResult::Success(Root);
	}

	// --- 3. Inherited native function lookup ---
	if (bIncludeInherited && BP->ParentClass)
	{
		UFunction* Func = BP->ParentClass->FindFunctionByName(FName(*FuncName));
		if (!Func)
		{
			// Try K2_ variant
			Func = BP->ParentClass->FindFunctionByName(FName(*FString::Printf(TEXT("K2_%s"), *FuncName)));
		}

		if (Func)
		{
			// Determine if from an interface
			FString Source = TEXT("native");
			for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
			{
				if (Iface.Interface && Iface.Interface->FindFunctionByName(Func->GetFName()))
				{
					Source = TEXT("interface");
					break;
				}
			}

			Root->SetStringField(TEXT("name"), Func->GetName());
			Root->SetStringField(TEXT("source"), Source);
			Root->SetBoolField(TEXT("is_pure"),   Func->HasAnyFunctionFlags(FUNC_BlueprintPure));
			Root->SetBoolField(TEXT("is_const"),  Func->HasAnyFunctionFlags(FUNC_Const));
			Root->SetBoolField(TEXT("is_static"), Func->HasAnyFunctionFlags(FUNC_Static));
			Root->SetStringField(TEXT("category"),    Func->GetMetaData(TEXT("Category")));
			Root->SetStringField(TEXT("description"), Func->GetMetaData(TEXT("ToolTip")));
			Root->SetStringField(TEXT("access"),
				Func->HasAnyFunctionFlags(FUNC_Protected) ? TEXT("Protected") :
				Func->HasAnyFunctionFlags(FUNC_Private)   ? TEXT("Private")   : TEXT("Public"));

			TArray<TSharedPtr<FJsonValue>> InputsArr;
			TArray<TSharedPtr<FJsonValue>> OutputsArr;
			for (TFieldIterator<FProperty> PropIt(Func); PropIt && (PropIt->PropertyFlags & CPF_Parm); ++PropIt)
			{
				FProperty* Prop = *PropIt;
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"), Prop->GetName());
				PObj->SetStringField(TEXT("type"), Prop->GetCPPType());
				if (Prop->PropertyFlags & (CPF_ReturnParm | CPF_OutParm))
					OutputsArr.Add(MakeShared<FJsonValueObject>(PObj));
				else
					InputsArr.Add(MakeShared<FJsonValueObject>(PObj));
			}
			Root->SetArrayField(TEXT("inputs"),  InputsArr);
			Root->SetArrayField(TEXT("outputs"), OutputsArr);
			Root->SetArrayField(TEXT("local_variables"), TArray<TSharedPtr<FJsonValue>>());
			return FMonolithActionResult::Success(Root);
		}
	}

	FString Hint = bIncludeInherited ? TEXT("") : TEXT(" (try include_inherited: true for native parent class functions)");
	return FMonolithActionResult::Error(FString::Printf(
		TEXT("Function '%s' not found in Blueprint '%s'%s"), *FuncName, *AssetPath, *Hint));
}

// ============================================================
//  get_event_dispatcher_details  (Wave 6)
// ============================================================

FMonolithActionResult FMonolithBlueprintActions::HandleGetEventDispatcherDetails(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString DispatcherName = Params->GetStringField(TEXT("dispatcher_name"));
	if (DispatcherName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: dispatcher_name"));
	}

	// Find the delegate signature graph
	UEdGraph* SigGraph = nullptr;
	for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
	{
		if (!Graph) continue;
		FString DisplayName = Graph->GetName();
		if (DisplayName.EndsWith(TEXT("_Signature")))
		{
			DisplayName.LeftChopInline(10, EAllowShrinking::No);
		}
		if (DisplayName == DispatcherName)
		{
			SigGraph = Graph;
			break;
		}
	}

	if (!SigGraph)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Event dispatcher not found: %s"), *DispatcherName));
	}

	// Collect signature pins from entry node
	TArray<TSharedPtr<FJsonValue>> PinsArr;
	for (UEdGraphNode* Node : SigGraph->Nodes)
	{
		UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (!EntryNode) continue;
		for (const UEdGraphPin* Pin : EntryNode->Pins)
		{
			if (!Pin || Pin->bHidden) continue;
			if (Pin->Direction != EGPD_Output) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
			PObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PObj->SetStringField(TEXT("type"),
				MonolithBlueprintInternal::ContainerPrefix(Pin->PinType) +
				MonolithBlueprintInternal::PinTypeToString(Pin->PinType));
			PinsArr.Add(MakeShared<FJsonValueObject>(PObj));
		}
		break;
	}

	// Scan all graphs for nodes referencing this dispatcher
	// We match on the graph name (stripped suffix) against CreateDelegate/Event node names
	TArray<TSharedPtr<FJsonValue>> ReferencingNodes;
	FName SigGraphFName = SigGraph->GetFName();

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			bool bReferences = false;
			FString NodeRole;

			if (UK2Node_CreateDelegate* CreateDel = Cast<UK2Node_CreateDelegate>(Node))
			{
				if (CreateDel->GetDelegateSignature() &&
					CreateDel->GetDelegateSignature()->GetOuter() == SigGraph)
				{
					bReferences = true;
					NodeRole = TEXT("CreateDelegate");
				}
			}
			else if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				// Call/Broadcast/Bind/Unbind dispatcher nodes are CallFunction nodes
				// targeting functions whose names match the dispatcher
				FString FuncName = CallNode->FunctionReference.GetMemberName().ToString();
				if (FuncName.Contains(DispatcherName))
				{
					bReferences = true;
					NodeRole = FuncName; // e.g. "OnMyEvent__DelegateSignature" or Broadcast
				}
			}
			else if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				FString EventName = EventNode->EventReference.GetMemberName().ToString();
				if (EventName.Contains(DispatcherName))
				{
					bReferences = true;
					NodeRole = TEXT("Event");
				}
			}

			if (bReferences)
			{
				TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
				NObj->SetStringField(TEXT("node_id"),   Node->GetName());
				NObj->SetStringField(TEXT("title"),     Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
				NObj->SetStringField(TEXT("graph"),     Graph->GetName());
				NObj->SetStringField(TEXT("node_role"), NodeRole);
				ReferencingNodes.Add(MakeShared<FJsonValueObject>(NObj));
			}
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("dispatcher_name"), DispatcherName);
	Root->SetStringField(TEXT("graph_name"),      SigGraph->GetName());
	Root->SetArrayField(TEXT("signature_pins"),   PinsArr);
	Root->SetArrayField(TEXT("referencing_nodes"), ReferencingNodes);
	Root->SetNumberField(TEXT("reference_count"),  ReferencingNodes.Num());
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  get_blueprint_info  (Wave 6)
// ============================================================

FMonolithActionResult FMonolithBlueprintActions::HandleGetBlueprintInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	// Parent class
	if (BP->ParentClass)
	{
		Root->SetStringField(TEXT("parent_class"), BP->ParentClass->GetName());
		Root->SetStringField(TEXT("parent_class_path"), BP->ParentClass->GetPathName());
	}

	// Blueprint type
	FString BPTypeStr;
	switch (BP->BlueprintType)
	{
	case BPTYPE_Normal:          BPTypeStr = TEXT("Normal"); break;
	case BPTYPE_Const:           BPTypeStr = TEXT("Const"); break;
	case BPTYPE_MacroLibrary:    BPTypeStr = TEXT("MacroLibrary"); break;
	case BPTYPE_Interface:       BPTypeStr = TEXT("Interface"); break;
	case BPTYPE_LevelScript:     BPTypeStr = TEXT("LevelScript"); break;
	case BPTYPE_FunctionLibrary: BPTypeStr = TEXT("FunctionLibrary"); break;
	default:                     BPTypeStr = TEXT("Normal"); break;
	}
	Root->SetStringField(TEXT("blueprint_type"), BPTypeStr);

	// Compile status
	FString StatusStr;
	switch (BP->Status)
	{
	case BS_Unknown:              StatusStr = TEXT("Unknown"); break;
	case BS_Dirty:                StatusStr = TEXT("Dirty"); break;
	case BS_Error:                StatusStr = TEXT("Error"); break;
	case BS_UpToDate:             StatusStr = TEXT("UpToDate"); break;
	case BS_UpToDateWithWarnings: StatusStr = TEXT("UpToDateWithWarnings"); break;
	case BS_BeingCreated:         StatusStr = TEXT("BeingCreated"); break;
	default:                      StatusStr = TEXT("Unknown"); break;
	}
	Root->SetStringField(TEXT("compile_status"), StatusStr);

	// Graph names
	TArray<TSharedPtr<FJsonValue>> GraphNamesArr;
	for (const UEdGraph* G : BP->UbergraphPages)
	{
		if (G) GraphNamesArr.Add(MakeShared<FJsonValueString>(G->GetName()));
	}
	for (const UEdGraph* G : BP->FunctionGraphs)
	{
		if (G) GraphNamesArr.Add(MakeShared<FJsonValueString>(G->GetName()));
	}
	for (const UEdGraph* G : BP->MacroGraphs)
	{
		if (G) GraphNamesArr.Add(MakeShared<FJsonValueString>(G->GetName()));
	}
	Root->SetArrayField(TEXT("graph_names"), GraphNamesArr);

	// has_tick — scan ubergraph pages for ReceiveTick event node
	bool bHasTick = false;
	for (const UEdGraph* G : BP->UbergraphPages)
	{
		if (!G) continue;
		for (const UEdGraphNode* Node : G->Nodes)
		{
			if (const UK2Node_Event* EvNode = Cast<UK2Node_Event>(Node))
			{
				FString EvName = EvNode->EventReference.GetMemberName().ToString();
				if (EvName == TEXT("ReceiveTick") || EvName == TEXT("Tick"))
				{
					bHasTick = true;
					break;
				}
			}
		}
		if (bHasTick) break;
	}
	Root->SetBoolField(TEXT("has_tick"), bHasTick);

	// has_construction_script — check if UserConstructionScript has more than the entry node
	bool bHasConstructionScript = false;
	UEdGraph* CSGraph = FBlueprintEditorUtils::FindUserConstructionScript(BP);
	if (CSGraph)
	{
		// More than just the entry node means the user added something
		int32 MeaningfulNodes = 0;
		for (const UEdGraphNode* Node : CSGraph->Nodes)
		{
			if (!Node) continue;
			if (Cast<UK2Node_FunctionEntry>(Node)) continue;
			MeaningfulNodes++;
		}
		bHasConstructionScript = MeaningfulNodes > 0;
	}
	Root->SetBoolField(TEXT("has_construction_script"), bHasConstructionScript);

	// Counts
	Root->SetNumberField(TEXT("variable_count"),  BP->NewVariables.Num());
	Root->SetNumberField(TEXT("function_count"),  BP->FunctionGraphs.Num());
	Root->SetNumberField(TEXT("interface_count"), BP->ImplementedInterfaces.Num());

	// Component count — SCS nodes minus the root
	int32 ComponentCount = 0;
	if (BP->SimpleConstructionScript)
	{
		const TArray<USCS_Node*>& AllNodes = BP->SimpleConstructionScript->GetAllNodes();
		ComponentCount = AllNodes.Num();
		// Subtract the default scene root if present
		if (BP->SimpleConstructionScript->GetDefaultSceneRootNode() != nullptr)
		{
			ComponentCount = FMath::Max(0, ComponentCount - 1);
		}
	}
	Root->SetNumberField(TEXT("component_count"), ComponentCount);

	// Generated class
	if (BP->GeneratedClass)
	{
		Root->SetStringField(TEXT("generated_class"), BP->GeneratedClass->GetName());
	}

	Root->SetBoolField(TEXT("is_data_only"),   FBlueprintEditorUtils::IsDataOnlyBlueprint(BP));
	Root->SetBoolField(TEXT("is_actor_based"),  FBlueprintEditorUtils::IsActorBased(BP));

	return FMonolithActionResult::Success(Root);
}
