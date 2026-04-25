#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"
#include "IHttpRouter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "IPAddress.h"

class FJsonObject;
class FJsonValue;
class FMonolithToolRegistry;

/**
 * Embedded MCP HTTP server.
 * Implements Streamable HTTP transport with JSON-RPC 2.0 dispatch.
 */
class MONOLITHCORE_API FMonolithHttpServer
{
public:
	FMonolithHttpServer();
	~FMonolithHttpServer();

	/** Start the HTTP server on the configured port */
	bool Start(int32 Port);

	/** Stop the server and unbind all routes */
	void Stop();

	/** Stop then Start — useful after a silent bind failure */
	bool Restart(int32 Port);

	/** Is the server currently running? */
	bool IsRunning() const { return bIsRunning; }

	/** Get the port the server is listening on */
	int32 GetPort() const { return BoundPort; }

private:
	// --- Route Handlers ---
	bool HandlePostMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleGetMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleDeleteMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleOptions(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleHealthCheck(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	// --- JSON-RPC Processing ---
	TSharedPtr<FJsonObject> ProcessJsonRpcRequest(const TSharedPtr<FJsonObject>& Request);
	TSharedPtr<FJsonObject> HandleInitialize(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleToolsList(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleToolsCall(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandlePing(const TSharedPtr<FJsonValue>& Id);

	// --- Helpers ---
	TUniquePtr<FHttpServerResponse> MakeJsonResponse(const FString& JsonBody, EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok);
	TUniquePtr<FHttpServerResponse> MakeSseResponse(const TArray<TSharedPtr<FJsonObject>>& Messages);
	void AddCorsHeaders(FHttpServerResponse& Response);

	/** Register all HTTP routes on the current HttpRouter. */
	void BindRoutes();

	/** Probe 127.0.0.1:Port via a TCP connect to verify the listener is actually bound. */
	static bool ProbePort(int32 Port);

	// --- State ---
	TSharedPtr<IHttpRouter> HttpRouter;
	TArray<FHttpRouteHandle> RouteHandles;
	int32 BoundPort = 0;
	bool bIsRunning = false;
	FDateTime StartTime;
};
