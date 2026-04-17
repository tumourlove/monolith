#include "MonolithMetaSoundHelpers.h"
#include "MetasoundFrontendDocument.h"

namespace MonolithMetaSoundHelpers
{
	const FMetasoundFrontendNode* FindNodeById(
		const FMetasoundFrontendGraph& Graph,
		const FGuid& NodeID)
	{
		for (const FMetasoundFrontendNode& Node : Graph.Nodes)
		{
			if (Node.GetID() == NodeID)
			{
				return &Node;
			}
		}
		return nullptr;
	}

	const FMetasoundFrontendVertex* FindInputVertex(
		const FMetasoundFrontendNode& Node,
		const FGuid& VertexID)
	{
		for (const FMetasoundFrontendVertex& V : Node.Interface.Inputs)
		{
			if (V.VertexID == VertexID)
			{
				return &V;
			}
		}
		return nullptr;
	}

	const FMetasoundFrontendVertex* FindInputVertexByName(
		const FMetasoundFrontendNode& Node,
		FName VertexName)
	{
		for (const FMetasoundFrontendVertex& V : Node.Interface.Inputs)
		{
			if (V.Name == VertexName)
			{
				return &V;
			}
		}
		return nullptr;
	}

	const FMetasoundFrontendVertex* FindOutputVertex(
		const FMetasoundFrontendNode& Node,
		const FGuid& VertexID)
	{
		for (const FMetasoundFrontendVertex& V : Node.Interface.Outputs)
		{
			if (V.VertexID == VertexID)
			{
				return &V;
			}
		}
		return nullptr;
	}

	const FMetasoundFrontendVertex* FindOutputVertexByName(
		const FMetasoundFrontendNode& Node,
		FName VertexName)
	{
		for (const FMetasoundFrontendVertex& V : Node.Interface.Outputs)
		{
			if (V.Name == VertexName)
			{
				return &V;
			}
		}
		return nullptr;
	}

	FString ResolveClassName(
		const FGuid& ClassID,
		const FMetasoundFrontendDocument& Doc)
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

	TArray<const FMetasoundFrontendEdge*> GetNodeEdges(
		const FMetasoundFrontendGraph& Graph,
		const FGuid& NodeID,
		bool bInputs,
		bool bOutputs)
	{
		TArray<const FMetasoundFrontendEdge*> Result;

		for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
		{
			if (bOutputs && Edge.FromNodeID == NodeID)
			{
				Result.Add(&Edge);
			}
			else if (bInputs && Edge.ToNodeID == NodeID)
			{
				Result.Add(&Edge);
			}
		}

		return Result;
	}

	TSharedPtr<FJsonObject> SerializeLiteral(const FMetasoundFrontendLiteral& Literal)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

		switch (Literal.GetType())
		{
		case EMetasoundFrontendLiteralType::Boolean:
			{
				Json->SetStringField(TEXT("type"), TEXT("bool"));
				bool BoolValue = false;
				if (Literal.TryGet(BoolValue))
				{
					Json->SetBoolField(TEXT("value"), BoolValue);
				}
			}
			break;
		case EMetasoundFrontendLiteralType::Integer:
			{
				Json->SetStringField(TEXT("type"), TEXT("int32"));
				int32 IntValue = 0;
				if (Literal.TryGet(IntValue))
				{
					Json->SetNumberField(TEXT("value"), IntValue);
				}
			}
			break;
		case EMetasoundFrontendLiteralType::Float:
			{
				Json->SetStringField(TEXT("type"), TEXT("float"));
				float FloatValue = 0.f;
				if (Literal.TryGet(FloatValue))
				{
					Json->SetNumberField(TEXT("value"), FloatValue);
				}
			}
			break;
		case EMetasoundFrontendLiteralType::String:
			{
				Json->SetStringField(TEXT("type"), TEXT("string"));
				FString StringValue;
				if (Literal.TryGet(StringValue))
				{
					Json->SetStringField(TEXT("value"), StringValue);
				}
			}
			break;
		case EMetasoundFrontendLiteralType::UObject:
			{
				Json->SetStringField(TEXT("type"), TEXT("object"));
				UObject* ObjValue = nullptr;
				if (Literal.TryGet(ObjValue) && ObjValue)
				{
					Json->SetStringField(TEXT("value"), ObjValue->GetPathName());
				}
			}
			break;
		default:
			Json->SetStringField(TEXT("type"), TEXT("unknown"));
			break;
		}

		return Json;
	}

	TSharedPtr<FJsonObject> SerializeVertex(const FMetasoundFrontendVertex& Vertex)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Vertex.Name.ToString());
		Json->SetStringField(TEXT("type"), Vertex.TypeName.ToString());
		Json->SetStringField(TEXT("vertex_id"), Vertex.VertexID.ToString());
		return Json;
	}

	TSharedPtr<FJsonObject> SerializeNode(
		const FMetasoundFrontendNode& Node,
		const FMetasoundFrontendDocument& Doc)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

		Json->SetStringField(TEXT("id"), Node.GetID().ToString());
		Json->SetStringField(TEXT("name"), Node.Name.ToString());
		Json->SetStringField(TEXT("class_id"), Node.ClassID.ToString());
		Json->SetStringField(TEXT("class_name"), ResolveClassName(Node.ClassID, Doc));

		TArray<TSharedPtr<FJsonValue>> InputsArray;
		for (const FMetasoundFrontendVertex& V : Node.Interface.Inputs)
		{
			InputsArray.Add(MakeShared<FJsonValueObject>(SerializeVertex(V)));
		}
		Json->SetArrayField(TEXT("inputs"), InputsArray);

		TArray<TSharedPtr<FJsonValue>> OutputsArray;
		for (const FMetasoundFrontendVertex& V : Node.Interface.Outputs)
		{
			OutputsArray.Add(MakeShared<FJsonValueObject>(SerializeVertex(V)));
		}
		Json->SetArrayField(TEXT("outputs"), OutputsArray);

		TArray<TSharedPtr<FJsonValue>> LiteralsArray;
		for (const FMetasoundFrontendVertexLiteral& Lit : Node.InputLiterals)
		{
			TSharedPtr<FJsonObject> LitJson = MakeShared<FJsonObject>();
			LitJson->SetStringField(TEXT("vertex_id"), Lit.VertexID.ToString());

			for (const FMetasoundFrontendVertex& V : Node.Interface.Inputs)
			{
				if (V.VertexID == Lit.VertexID)
				{
					LitJson->SetStringField(TEXT("vertex_name"), V.Name.ToString());
					break;
				}
			}

			LitJson->SetObjectField(TEXT("value"), SerializeLiteral(Lit.Value));
			LiteralsArray.Add(MakeShared<FJsonValueObject>(LitJson));
		}
		Json->SetArrayField(TEXT("input_literals"), LiteralsArray);

#if WITH_EDITORONLY_DATA
		if (Node.Style.Display.Locations.Num() > 0)
		{
			const FVector2D& Pos = Node.Style.Display.Locations.begin()->Value;
			Json->SetNumberField(TEXT("pos_x"), Pos.X);
			Json->SetNumberField(TEXT("pos_y"), Pos.Y);
		}
#endif

		return Json;
	}

	TSharedPtr<FJsonObject> SerializeNodeSummary(
		const FMetasoundFrontendNode& Node,
		const FMetasoundFrontendDocument& Doc)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

		Json->SetStringField(TEXT("id"), Node.GetID().ToString());
		Json->SetStringField(TEXT("name"), Node.Name.ToString());
		Json->SetStringField(TEXT("class_name"), ResolveClassName(Node.ClassID, Doc));
		Json->SetNumberField(TEXT("input_count"), Node.Interface.Inputs.Num());
		Json->SetNumberField(TEXT("output_count"), Node.Interface.Outputs.Num());

#if WITH_EDITORONLY_DATA
		if (Node.Style.Display.Locations.Num() > 0)
		{
			const FVector2D& Pos = Node.Style.Display.Locations.begin()->Value;
			Json->SetNumberField(TEXT("pos_x"), Pos.X);
			Json->SetNumberField(TEXT("pos_y"), Pos.Y);
		}
#endif

		return Json;
	}

	TSharedPtr<FJsonObject> SerializeEdge(
		const FMetasoundFrontendEdge& Edge,
		const FMetasoundFrontendGraph& Graph)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

		Json->SetStringField(TEXT("from_node"), Edge.FromNodeID.ToString());
		Json->SetStringField(TEXT("from_vertex"), Edge.FromVertexID.ToString());
		Json->SetStringField(TEXT("to_node"), Edge.ToNodeID.ToString());
		Json->SetStringField(TEXT("to_vertex"), Edge.ToVertexID.ToString());

		if (const FMetasoundFrontendNode* FromNode = FindNodeById(Graph, Edge.FromNodeID))
		{
			if (const FMetasoundFrontendVertex* V = FindOutputVertex(*FromNode, Edge.FromVertexID))
			{
				Json->SetStringField(TEXT("from_pin"), V->Name.ToString());
				Json->SetStringField(TEXT("from_type"), V->TypeName.ToString());
			}
			Json->SetStringField(TEXT("from_node_name"), FromNode->Name.ToString());
		}

		if (const FMetasoundFrontendNode* ToNode = FindNodeById(Graph, Edge.ToNodeID))
		{
			if (const FMetasoundFrontendVertex* V = FindInputVertex(*ToNode, Edge.ToVertexID))
			{
				Json->SetStringField(TEXT("to_pin"), V->Name.ToString());
				Json->SetStringField(TEXT("to_type"), V->TypeName.ToString());
			}
			Json->SetStringField(TEXT("to_node_name"), ToNode->Name.ToString());
		}

		return Json;
	}

	TSharedPtr<FJsonObject> SerializeVariable(const FMetasoundFrontendVariable& Variable)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

		Json->SetStringField(TEXT("name"), Variable.Name.ToString());
		Json->SetStringField(TEXT("type"), Variable.TypeName.ToString());
		Json->SetStringField(TEXT("id"), Variable.ID.ToString());

#if WITH_EDITORONLY_DATA
		Json->SetStringField(TEXT("display_name"), Variable.DisplayName.ToString());
		Json->SetStringField(TEXT("description"), Variable.Description.ToString());
#endif

		if (Variable.Literal.GetType() != EMetasoundFrontendLiteralType::None)
		{
			Json->SetObjectField(TEXT("default_value"), SerializeLiteral(Variable.Literal));
		}

		return Json;
	}
}
