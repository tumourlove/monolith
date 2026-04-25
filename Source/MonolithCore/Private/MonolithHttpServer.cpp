#include "MonolithHttpServer.h"
#include "MonolithCoreModule.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "HttpServerModule.h"
#include "HttpServerResponse.h"
#include "GenericPlatform/GenericPlatformProcess.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "IPAddress.h"

FMonolithHttpServer::FMonolithHttpServer()
{
}

FMonolithHttpServer::~FMonolithHttpServer()
{
	Stop();
}

bool FMonolithHttpServer::Start(int32 Port)
{
	if (bIsRunning)
	{
		UE_LOG(LogMonolith, Warning, TEXT("HTTP server already running on port %d"), BoundPort);
		return true;
	}

	// On a fresh editor launch, the OS keeps the port in TIME_WAIT for up to
	// 2*MSL (~30s on macOS/Linux) after the previous editor shut down. UE's
	// HttpServerModule also caches a broken listener internally and won't
	// rebind until StopAllListeners() is called. Budget ~40s total so a
	// rapid close+reopen cycle doesn't drop the MCP server on the floor.
	constexpr int32 MaxAttempts = 20;
	constexpr float BackoffSeconds = 2.0f;

	for (int32 Attempt = 1; Attempt <= MaxAttempts; ++Attempt)
	{
		if (Attempt > 1)
		{
			UE_LOG(LogMonolith, Warning, TEXT("HTTP bind attempt %d/%d on port %d — waiting %.1fs"),
				Attempt, MaxAttempts, Port, BackoffSeconds);
			FPlatformProcess::Sleep(BackoffSeconds);

			// Drop our router handle + routes so GetHttpRouter can evict failed listener.
			if (HttpRouter.IsValid())
			{
				for (const FHttpRouteHandle& Handle : RouteHandles)
				{
					HttpRouter->UnbindRoute(Handle);
				}
			}
			RouteHandles.Empty();
			HttpRouter.Reset();

			// Full module reset — the HttpServerModule caches a failed listener
			// and refuses to re-bind the same port until we explicitly stop it.
			FHttpServerModule::Get().StopAllListeners();
		}

		HttpRouter = FHttpServerModule::Get().GetHttpRouter(Port, true);
		if (!HttpRouter.IsValid())
		{
			UE_LOG(LogMonolith, Warning, TEXT("GetHttpRouter failed on port %d (attempt %d)"), Port, Attempt);
			continue;
		}

		BindRoutes();
		FHttpServerModule::Get().StartAllListeners();

		// Brief wait for OS to complete bind before probing
		FPlatformProcess::Sleep(0.1f);

		if (ProbePort(Port))
		{
			bIsRunning = true;
			BoundPort = Port;
			StartTime = FDateTime::UtcNow();
			UE_LOG(LogMonolith, Log, TEXT("Monolith MCP server listening on port %d (attempt %d)"), Port, Attempt);
			return true;
		}

		UE_LOG(LogMonolith, Warning, TEXT("Port %d not listening after StartAllListeners (attempt %d)"), Port, Attempt);
	}

	UE_LOG(LogMonolith, Error, TEXT("Failed to bind Monolith MCP server on port %d after %d attempts (~%ds total)"),
		Port, MaxAttempts, static_cast<int32>(MaxAttempts * BackoffSeconds));
	// Clean up
	if (HttpRouter.IsValid())
	{
		for (const FHttpRouteHandle& Handle : RouteHandles)
		{
			HttpRouter->UnbindRoute(Handle);
		}
	}
	RouteHandles.Empty();
	HttpRouter.Reset();
	return false;
}

void FMonolithHttpServer::Stop()
{
	if (!bIsRunning)
	{
		return;
	}

	if (HttpRouter.IsValid())
	{
		for (const FHttpRouteHandle& Handle : RouteHandles)
		{
			HttpRouter->UnbindRoute(Handle);
		}
		RouteHandles.Empty();
	}

	FHttpServerModule::Get().StopAllListeners();
	HttpRouter.Reset();

	bIsRunning = false;
	UE_LOG(LogMonolith, Log, TEXT("Monolith MCP server stopped"));
}

void FMonolithHttpServer::BindRoutes()
{
	if (!HttpRouter.IsValid()) return;

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandlePostMcp)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandleGetMcp)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_DELETE,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandleDeleteMcp)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_OPTIONS,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandleOptions)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/health")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandleHealthCheck)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/health")),
		EHttpServerRequestVerbs::VERB_OPTIONS,
		FHttpRequestHandler::CreateRaw(this, &FMonolithHttpServer::HandleOptions)));
}

bool FMonolithHttpServer::ProbePort(int32 Port)
{
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem) return false;

	TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
	bool bValid = false;
	Addr->SetIp(TEXT("127.0.0.1"), bValid);
	if (!bValid) return false;
	Addr->SetPort(Port);

	FSocket* Socket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("MonolithProbe"), false);
	if (!Socket) return false;

	Socket->SetNonBlocking(false);
	const bool bConnected = Socket->Connect(*Addr);
	Socket->Close();
	SocketSubsystem->DestroySocket(Socket);
	return bConnected;
}

bool FMonolithHttpServer::Restart(int32 Port)
{
	// Unbind our routes
	if (HttpRouter.IsValid())
	{
		for (const FHttpRouteHandle& Handle : RouteHandles)
		{
			HttpRouter->UnbindRoute(Handle);
		}
	}
	RouteHandles.Empty();
	HttpRouter.Reset();

	// Full stop — safe here because we own the listener
	FHttpServerModule::Get().StopAllListeners();

	bIsRunning = false;
	BoundPort = 0;
	return Start(Port);
}

// ============================================================================
// Route Handlers
// ============================================================================

bool FMonolithHttpServer::HandlePostMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Parse body as UTF-8 JSON (Body is NOT null-terminated — must add terminator)
	TArray<uint8> NullTermBody(Request.Body);
	NullTermBody.Add(0);
	FString BodyString = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(NullTermBody.GetData())));
	if (BodyString.IsEmpty())
	{
		TSharedPtr<FJsonObject> Err = FMonolithJsonUtils::ErrorResponse(
			nullptr, FMonolithJsonUtils::ErrParseError, TEXT("Empty request body"));
		auto Response = MakeJsonResponse(FMonolithJsonUtils::Serialize(Err), EHttpServerResponseCodes::BadRequest);
		AddCorsHeaders(*Response);
		OnComplete(MoveTemp(Response));
		return true;
	}

	// Try parse as JSON
	TSharedPtr<FJsonObject> JsonRequest = FMonolithJsonUtils::Parse(BodyString);

	// Could be a single request or a batch (array)
	TArray<TSharedPtr<FJsonObject>> Requests;
	TArray<TSharedPtr<FJsonObject>> Responses;

	if (JsonRequest.IsValid())
	{
		// Single request
		Requests.Add(JsonRequest);
	}
	else
	{
		// Try parsing as array (batch)
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyString);
		if (FJsonSerializer::Deserialize(Reader, JsonArray) && JsonArray.Num() > 0)
		{
			for (const TSharedPtr<FJsonValue>& Value : JsonArray)
			{
				if (Value.IsValid() && Value->Type == EJson::Object)
				{
					Requests.Add(Value->AsObject());
				}
			}
		}
		else
		{
			TSharedPtr<FJsonObject> Err = FMonolithJsonUtils::ErrorResponse(
				nullptr, FMonolithJsonUtils::ErrParseError, TEXT("Invalid JSON"));
			auto Response = MakeJsonResponse(FMonolithJsonUtils::Serialize(Err), EHttpServerResponseCodes::BadRequest);
			AddCorsHeaders(*Response);
			OnComplete(MoveTemp(Response));
			return true;
		}
	}

	// Process each request
	for (const TSharedPtr<FJsonObject>& Req : Requests)
	{
		TSharedPtr<FJsonObject> Resp = ProcessJsonRpcRequest(Req);
		if (Resp.IsValid())
		{
			// Only add response if it's not a notification (notifications have no id)
			Responses.Add(Resp);
		}
	}

	// Build response
	FString ResponseBody;
	if (Responses.Num() == 0)
	{
		// All notifications — 202 Accepted with no body
		auto Response = FHttpServerResponse::Ok();
		Response->Code = EHttpServerResponseCodes::Accepted;
		AddCorsHeaders(*Response);
		OnComplete(MoveTemp(Response));
		return true;
	}
	else if (Responses.Num() == 1)
	{
		ResponseBody = FMonolithJsonUtils::Serialize(Responses[0]);
	}
	else
	{
		// Batch response — serialize as array
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		for (const TSharedPtr<FJsonObject>& Resp : Responses)
		{
			JsonArray.Add(MakeShared<FJsonValueObject>(Resp));
		}
		FString ArrayStr;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ArrayStr);
		FJsonSerializer::Serialize(JsonArray, Writer);
		ResponseBody = ArrayStr;
	}

	auto Response = MakeJsonResponse(ResponseBody);
	AddCorsHeaders(*Response);
	OnComplete(MoveTemp(Response));
	return true;
}

bool FMonolithHttpServer::HandleGetMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// SSE endpoint — return a single SSE event with an endpoint message.
	// UE's HTTP server doesn't natively support long-lived SSE connections,
	// so we return a single SSE event and close.
	FString SseBody = TEXT("event: endpoint\ndata: \"/mcp\"\n\n");
	auto Response = FHttpServerResponse::Create(SseBody, TEXT("text/event-stream"));
	Response->Code = EHttpServerResponseCodes::Ok;
	AddCorsHeaders(*Response);
	Response->Headers.Add(TEXT("Cache-Control"), {TEXT("no-cache")});
	Response->Headers.Add(TEXT("Connection"), {TEXT("keep-alive")});
	OnComplete(MoveTemp(Response));
	return true;
}

bool FMonolithHttpServer::HandleDeleteMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	auto Response = FHttpServerResponse::Ok();
	AddCorsHeaders(*Response);
	OnComplete(MoveTemp(Response));
	return true;
}

bool FMonolithHttpServer::HandleOptions(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	auto Response = FHttpServerResponse::Ok();
	AddCorsHeaders(*Response);
	OnComplete(MoveTemp(Response));
	return true;
}

bool FMonolithHttpServer::HandleHealthCheck(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Health = MakeShared<FJsonObject>();
	Health->SetStringField(TEXT("status"), TEXT("ok"));
	Health->SetNumberField(TEXT("port"), BoundPort);
	Health->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
	Health->SetStringField(TEXT("version"), MONOLITH_VERSION);

	const FTimespan Uptime = FDateTime::UtcNow() - StartTime;
	Health->SetNumberField(TEXT("uptime_seconds"), static_cast<double>(Uptime.GetTotalSeconds()));

	Health->SetNumberField(TEXT("tools_registered"), FMonolithToolRegistry::Get().GetActionCount());

	FString Body;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	FJsonSerializer::Serialize(Health.ToSharedRef(), Writer);

	auto Response = MakeJsonResponse(Body);
	AddCorsHeaders(*Response);
	OnComplete(MoveTemp(Response));
	return true;
}

// ============================================================================
// JSON-RPC 2.0 Processing
// ============================================================================

TSharedPtr<FJsonObject> FMonolithHttpServer::ProcessJsonRpcRequest(const TSharedPtr<FJsonObject>& Request)
{
	if (!Request.IsValid())
	{
		return FMonolithJsonUtils::ErrorResponse(nullptr, FMonolithJsonUtils::ErrInvalidRequest, TEXT("Invalid request object"));
	}

	// Validate jsonrpc version
	FString Version;
	if (!Request->TryGetStringField(TEXT("jsonrpc"), Version) || Version != TEXT("2.0"))
	{
		return FMonolithJsonUtils::ErrorResponse(nullptr, FMonolithJsonUtils::ErrInvalidRequest, TEXT("Missing or invalid jsonrpc version"));
	}

	// Get method
	FString Method;
	if (!Request->TryGetStringField(TEXT("method"), Method))
	{
		return FMonolithJsonUtils::ErrorResponse(nullptr, FMonolithJsonUtils::ErrInvalidRequest, TEXT("Missing method field"));
	}

	// Get id (null for notifications)
	TSharedPtr<FJsonValue> Id = Request->TryGetField(TEXT("id"));
	bool bIsNotification = !Id.IsValid() || Id->IsNull();

	// Get params
	TSharedPtr<FJsonObject> Params;
	const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
	if (Request->TryGetObjectField(TEXT("params"), ParamsObj) && ParamsObj)
	{
		Params = *ParamsObj;
	}
	if (!Params.IsValid())
	{
		Params = MakeShared<FJsonObject>();
	}

	UE_LOG(LogMonolith, Verbose, TEXT("JSON-RPC: %s (id=%s)"), *Method, Id.IsValid() ? *Id->AsString() : TEXT("notification"));

	// Dispatch by method
	TSharedPtr<FJsonObject> Response;

	if (Method == TEXT("initialize"))
	{
		Response = HandleInitialize(Id, Params);
	}
	else if (Method == TEXT("notifications/initialized"))
	{
		// Notification — no response
		return nullptr;
	}
	else if (Method == TEXT("tools/list"))
	{
		Response = HandleToolsList(Id, Params);
	}
	else if (Method == TEXT("tools/call"))
	{
		Response = HandleToolsCall(Id, Params);
	}
	else if (Method == TEXT("ping"))
	{
		Response = HandlePing(Id);
	}
	else
	{
		Response = FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrMethodNotFound,
			FString::Printf(TEXT("Unknown method: %s"), *Method));
	}

	// Notifications don't get responses
	if (bIsNotification)
	{
		return nullptr;
	}

	return Response;
}

TSharedPtr<FJsonObject> FMonolithHttpServer::HandleInitialize(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Protocol version negotiation: echo the client's requested version if we
	// support it, otherwise fall back to the latest we support.
	FString ClientVersion;
	if (Params.IsValid() && Params->TryGetStringField(TEXT("protocolVersion"), ClientVersion)
		&& (ClientVersion == TEXT("2024-11-05") || ClientVersion == TEXT("2025-03-26")))
	{
		Result->SetStringField(TEXT("protocolVersion"), ClientVersion);
	}
	else
	{
		Result->SetStringField(TEXT("protocolVersion"), TEXT("2025-03-26"));
	}

	// Server info
	TSharedPtr<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
	ServerInfo->SetStringField(TEXT("name"), TEXT("monolith"));
	ServerInfo->SetStringField(TEXT("version"), MONOLITH_VERSION);
	Result->SetObjectField(TEXT("serverInfo"), ServerInfo);

	// Capabilities
	TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();

	// We support tools
	TSharedPtr<FJsonObject> ToolsCap = MakeShared<FJsonObject>();
	ToolsCap->SetBoolField(TEXT("listChanged"), false);
	Capabilities->SetObjectField(TEXT("tools"), ToolsCap);

	Result->SetObjectField(TEXT("capabilities"), Capabilities);

	return FMonolithJsonUtils::SuccessResponse(Id, MakeShared<FJsonValueObject>(Result));
}

TSharedPtr<FJsonObject> FMonolithHttpServer::HandleToolsList(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	TArray<TSharedPtr<FJsonValue>> ToolsArray;

	// Each namespace becomes a tool
	TArray<FString> Namespaces = Registry.GetNamespaces();
	for (const FString& Namespace : Namespaces)
	{
		TArray<FMonolithActionInfo> Actions = Registry.GetActions(Namespace);
		if (Actions.Num() == 0) continue;

		// Build the tool entry for this namespace
		// Format: "namespace_query" with action as a parameter
		TSharedPtr<FJsonObject> Tool = MakeShared<FJsonObject>();

		if (Namespace == TEXT("monolith"))
		{
			// Core tools are individual: monolith_discover, monolith_status
			for (const FMonolithActionInfo& ActionInfo : Actions)
			{
				TSharedPtr<FJsonObject> CoreTool = MakeShared<FJsonObject>();
				CoreTool->SetStringField(TEXT("name"), FString::Printf(TEXT("monolith_%s"), *ActionInfo.Action));
				CoreTool->SetStringField(TEXT("description"), ActionInfo.Description);

				// Input schema
				TSharedPtr<FJsonObject> InputSchema = MakeShared<FJsonObject>();
				InputSchema->SetStringField(TEXT("type"), TEXT("object"));
				if (ActionInfo.ParamSchema.IsValid())
				{
					InputSchema->SetObjectField(TEXT("properties"), ActionInfo.ParamSchema);
				}
				else
				{
					InputSchema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
				}
				CoreTool->SetObjectField(TEXT("inputSchema"), InputSchema);

				ToolsArray.Add(MakeShared<FJsonValueObject>(CoreTool));
			}
		}
		else
		{
			// Domain tools use the dispatch pattern: namespace_query (underscore, not dot)
			// Dots in tool names break Claude Code's mcp__server__tool mapping.
			FString ToolName = FString::Printf(TEXT("%s_query"), *Namespace);
			Tool->SetStringField(TEXT("name"), ToolName);

			// Build description with action list
			FString Description = FString::Printf(TEXT("Query the %s domain. Available actions: "), *Namespace);
			TArray<FString> ActionNames;
			for (const FMonolithActionInfo& ActionInfo : Actions)
			{
				ActionNames.Add(ActionInfo.Action);
			}
			Description += FString::Join(ActionNames, TEXT(", "));
			Tool->SetStringField(TEXT("description"), Description);

			// Build input schema
			TSharedPtr<FJsonObject> InputSchema = MakeShared<FJsonObject>();
			InputSchema->SetStringField(TEXT("type"), TEXT("object"));

			TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

			// "action" property (required)
			TSharedPtr<FJsonObject> ActionProp = MakeShared<FJsonObject>();
			ActionProp->SetStringField(TEXT("type"), TEXT("string"));
			ActionProp->SetStringField(TEXT("description"), TEXT("The action to execute"));
			TArray<TSharedPtr<FJsonValue>> EnumValues;
			for (const FString& Name : ActionNames)
			{
				EnumValues.Add(MakeShared<FJsonValueString>(Name));
			}
			ActionProp->SetArrayField(TEXT("enum"), EnumValues);
			Properties->SetObjectField(TEXT("action"), ActionProp);

			// "params" property — keep lightweight; full per-action schemas available via monolith_discover
			TSharedPtr<FJsonObject> ParamsProp = MakeShared<FJsonObject>();
			ParamsProp->SetStringField(TEXT("type"), TEXT("object"));
			ParamsProp->SetStringField(TEXT("description"),
				FString::Printf(TEXT("Parameters for the action. Call monolith_discover(\"%s\") for full parameter schemas."), *Namespace));
			Properties->SetObjectField(TEXT("params"), ParamsProp);

			InputSchema->SetObjectField(TEXT("properties"), Properties);
			InputSchema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("action"))});

			Tool->SetObjectField(TEXT("inputSchema"), InputSchema);
			ToolsArray.Add(MakeShared<FJsonValueObject>(Tool));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("tools"), ToolsArray);

	return FMonolithJsonUtils::SuccessResponse(Id, MakeShared<FJsonValueObject>(Result));
}

TSharedPtr<FJsonObject> FMonolithHttpServer::HandleToolsCall(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("Missing params"));
	}

	FString ToolName;
	if (!Params->TryGetStringField(TEXT("name"), ToolName))
	{
		return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams, TEXT("Missing tool name"));
	}

	// Get arguments
	TSharedPtr<FJsonObject> Arguments;
	const TSharedPtr<FJsonObject>* ArgsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("arguments"), ArgsObj) && ArgsObj)
	{
		Arguments = *ArgsObj;
	}
	if (!Arguments.IsValid())
	{
		Arguments = MakeShared<FJsonObject>();
	}

	FString Namespace;
	FString Action;

	// Determine dispatch pattern
	if (ToolName.StartsWith(TEXT("monolith_")))
	{
		// Core tool: monolith_discover -> namespace="monolith", action="discover"
		Namespace = TEXT("monolith");
		Action = ToolName.Mid(9);
	}
	else if (ToolName.EndsWith(TEXT("_query")) || ToolName.EndsWith(TEXT(".query")))
	{
		// Domain tool: blueprint_query (or legacy blueprint.query) -> namespace="blueprint"
		Namespace = ToolName.Left(ToolName.Len() - 6); // strip "_query" or ".query"

		if (!Arguments->TryGetStringField(TEXT("action"), Action))
		{
			return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrInvalidParams,
				TEXT("Missing 'action' in arguments"));
		}

		// Collect top-level fields (excluding reserved keys) — MCP clients may
		// place optional params like members_only alongside "action" rather than
		// nesting them inside "params".
		TSharedPtr<FJsonObject> TopLevelExtras = MakeShared<FJsonObject>();
		for (const auto& Pair : Arguments->Values)
		{
			if (Pair.Key != TEXT("action") && Pair.Key != TEXT("params"))
			{
				TopLevelExtras->SetField(Pair.Key, Pair.Value);
			}
		}

		// Extract nested params if present, then merge in any top-level extras
		// NOTE: Claude Code sends "params" as a JSON-encoded string, not a nested object.
		// We must handle both cases.
		const TSharedPtr<FJsonObject>* NestedParams = nullptr;
		TSharedPtr<FJsonObject> ParsedParamsObj; // lifetime holder for string-parsed params
		bool bHasNestedParams = false;

		if (Arguments->TryGetObjectField(TEXT("params"), NestedParams) && NestedParams)
		{
			bHasNestedParams = true;
		}
		else
		{
			// Try parsing "params" as a JSON string (Claude Code serializes objects to strings)
			FString ParamsStr;
			if (Arguments->TryGetStringField(TEXT("params"), ParamsStr) && !ParamsStr.IsEmpty())
			{
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ParamsStr);
				if (FJsonSerializer::Deserialize(Reader, ParsedParamsObj) && ParsedParamsObj.IsValid())
				{
					NestedParams = &ParsedParamsObj;
					bHasNestedParams = true;
				}
			}
		}

		if (bHasNestedParams && NestedParams)
		{
			Arguments = MakeShared<FJsonObject>();
			// Start with top-level extras (lower priority)
			for (const auto& Pair : TopLevelExtras->Values)
			{
				Arguments->SetField(Pair.Key, Pair.Value);
			}
			// Overlay nested params (higher priority)
			for (const auto& Pair : (*NestedParams)->Values)
			{
				Arguments->SetField(Pair.Key, Pair.Value);
			}
		}
		else
		{
			// No nested "params" — use top-level fields as params directly
			Arguments = TopLevelExtras;
		}
	}
	else
	{
		return FMonolithJsonUtils::ErrorResponse(Id, FMonolithJsonUtils::ErrMethodNotFound,
			FString::Printf(TEXT("Unknown tool: %s"), *ToolName));
	}

	// Execute via registry
	FMonolithActionResult ActionResult = FMonolithToolRegistry::Get().ExecuteAction(Namespace, Action, Arguments);

	// Build MCP tool result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Content;

	if (ActionResult.bSuccess)
	{
		TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
		if (ActionResult.Result.IsValid())
		{
			TextContent->SetStringField(TEXT("text"), FMonolithJsonUtils::Serialize(ActionResult.Result));
		}
		else
		{
			TextContent->SetStringField(TEXT("text"), TEXT("{}"));
		}
		Content.Add(MakeShared<FJsonValueObject>(TextContent));
		Result->SetBoolField(TEXT("isError"), false);
	}
	else
	{
		TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
		TextContent->SetStringField(TEXT("text"), ActionResult.ErrorMessage);
		Content.Add(MakeShared<FJsonValueObject>(TextContent));
		Result->SetBoolField(TEXT("isError"), true);
	}

	Result->SetArrayField(TEXT("content"), Content);

	return FMonolithJsonUtils::SuccessResponse(Id, MakeShared<FJsonValueObject>(Result));
}

TSharedPtr<FJsonObject> FMonolithHttpServer::HandlePing(const TSharedPtr<FJsonValue>& Id)
{
	return FMonolithJsonUtils::SuccessResponse(Id, MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
}

// ============================================================================
// Helpers
// ============================================================================

TUniquePtr<FHttpServerResponse> FMonolithHttpServer::MakeJsonResponse(const FString& JsonBody, EHttpServerResponseCodes Code)
{
	auto Response = FHttpServerResponse::Create(JsonBody, TEXT("application/json"));
	Response->Code = Code;
	return Response;
}

TUniquePtr<FHttpServerResponse> FMonolithHttpServer::MakeSseResponse(const TArray<TSharedPtr<FJsonObject>>& Messages)
{
	FString SseBody;
	for (const TSharedPtr<FJsonObject>& Msg : Messages)
	{
		SseBody += TEXT("event: message\ndata: ");
		SseBody += FMonolithJsonUtils::Serialize(Msg);
		SseBody += TEXT("\n\n");
	}

	auto Response = FHttpServerResponse::Create(SseBody, TEXT("text/event-stream"));
	Response->Code = EHttpServerResponseCodes::Ok;
	return Response;
}

void FMonolithHttpServer::AddCorsHeaders(FHttpServerResponse& Response)
{
	Response.Headers.Add(TEXT("Access-Control-Allow-Origin"), {TEXT("*")});
	Response.Headers.Add(TEXT("Access-Control-Allow-Methods"), {TEXT("GET, POST, DELETE, OPTIONS")});
	Response.Headers.Add(TEXT("Access-Control-Allow-Headers"), {TEXT("Content-Type")});
}

