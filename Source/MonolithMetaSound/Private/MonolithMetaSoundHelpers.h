#pragma once

#include "CoreMinimal.h"
#include "MetasoundFrontendDocument.h"
#include "Dom/JsonObject.h"

namespace MonolithMetaSoundHelpers
{
	const FMetasoundFrontendNode* FindNodeById(
		const FMetasoundFrontendGraph& Graph,
		const FGuid& NodeID);

	const FMetasoundFrontendVertex* FindInputVertex(
		const FMetasoundFrontendNode& Node,
		const FGuid& VertexID);

	const FMetasoundFrontendVertex* FindInputVertexByName(
		const FMetasoundFrontendNode& Node,
		FName VertexName);

	const FMetasoundFrontendVertex* FindOutputVertex(
		const FMetasoundFrontendNode& Node,
		const FGuid& VertexID);

	const FMetasoundFrontendVertex* FindOutputVertexByName(
		const FMetasoundFrontendNode& Node,
		FName VertexName);

	FString ResolveClassName(
		const FGuid& ClassID,
		const FMetasoundFrontendDocument& Doc);

	TArray<const FMetasoundFrontendEdge*> GetNodeEdges(
		const FMetasoundFrontendGraph& Graph,
		const FGuid& NodeID,
		bool bInputs,
		bool bOutputs);

	TSharedPtr<FJsonObject> SerializeLiteral(
		const FMetasoundFrontendLiteral& Literal);

	TSharedPtr<FJsonObject> SerializeVertex(
		const FMetasoundFrontendVertex& Vertex);

	TSharedPtr<FJsonObject> SerializeNode(
		const FMetasoundFrontendNode& Node,
		const FMetasoundFrontendDocument& Doc);

	TSharedPtr<FJsonObject> SerializeNodeSummary(
		const FMetasoundFrontendNode& Node,
		const FMetasoundFrontendDocument& Doc);

	TSharedPtr<FJsonObject> SerializeEdge(
		const FMetasoundFrontendEdge& Edge,
		const FMetasoundFrontendGraph& Graph);

	TSharedPtr<FJsonObject> SerializeVariable(
		const FMetasoundFrontendVariable& Variable);
}
