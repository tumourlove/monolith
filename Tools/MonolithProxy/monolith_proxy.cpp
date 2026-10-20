/*
 * Monolith MCP stdio-to-HTTP proxy (C++ / WinHTTP).
 *
 * Sits between Claude Code (stdio JSON-RPC) and Monolith (HTTP on localhost).
 * Handles initialize locally, forwards tool calls to Monolith.
 * Survives editor restarts -- proxy process never dies.
 * Background health poll auto-detects when the editor comes online.
 *
 * Build: see build.bat or CMakeLists.txt
 * Usage (in .mcp.json):
 *   {"mcpServers":{"monolith":{"command":"path/to/monolith_proxy.exe"}}}
 */

// ============================================================================
// Includes
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <io.h>
#include <fcntl.h>

#include <iostream>
#include <string>
#include <sstream>
#include <thread>
#include <mutex>
#include <chrono>
#include <ctime>
#include <set>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <optional>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <vector>
#include <iomanip>

#include <nlohmann/json.hpp>

#pragma comment(lib, "bcrypt.lib")

using json = nlohmann::json;

// ============================================================================
// Constants
// ============================================================================

static const char* PROXY_NAME    = "monolith-proxy";
static const char* PROXY_VERSION = "1.2.0";

// Default request timeout for forwarded calls; override with MONOLITH_PROXY_TIMEOUT_S.
static constexpr double DEFAULT_TIMEOUT          = 120.0;
static constexpr double CONNECT_TIMEOUT          = 10.0;
static constexpr double POLL_INTERVAL            = 5.0;
static constexpr double POLL_START_DELAY         = 3.0;
static constexpr double REPEAT_TOOL_CALL_WINDOW  = 3.0;

static const std::set<std::string> SUPPORTED_VERSIONS = {
    "2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"
};

static const std::set<std::string> EDITOR_BUILD_ACTIONS = {
    "trigger_build", "live_compile"
};

static const std::set<std::string> EDITOR_READ_ACTIONS = {
    "get_build_errors",
    "get_build_status",
    "get_build_summary",
    "search_build_output",
    "get_recent_logs",
    "search_logs",
    "tail_log",
    "get_log_categories",
    "get_log_stats",
    "get_compile_output",
    "get_crash_context",
};

// ============================================================================
// Globals
// ============================================================================

static std::string g_monolith_url;        // e.g. "http://localhost:9316/mcp"
static std::string g_monolith_host;       // e.g. "localhost"
static int         g_monolith_port = 0;   // e.g. 9316
static double      g_request_timeout_s = DEFAULT_TIMEOUT;

// Why the last post_monolith() call returned no body.
enum class PostFailure { None, Timeout, Unreachable };
static std::string g_monolith_path_mcp;   // e.g. "/mcp"
static std::string g_monolith_path_health;// e.g. "/health"

static bool g_split_editor_query = false;
static std::set<std::string> g_editor_action_allowlist;
static std::set<std::string> g_editor_action_denylist;

// State tracking
static std::optional<bool> g_monolith_was_up; // nullopt = unknown
static std::mutex g_stdout_lock;
static std::unordered_map<std::string, double> g_recent_tool_calls;

// Call-log state (Phase 4 / survivor F)
//
// NOTE: Saved/Logs/MonolithCalls.jsonl is project-root-relative and excluded
// from crash zip generation by UE's crash reporter (Saved/Logs/ tail capture
// only includes editor logs, not arbitrary jsonl). If a crash collector pattern
// elsewhere DOES sweep Saved/Logs/*, the user should add MonolithCalls.jsonl to
// the exclusion list. Single-user local dev tool; no phone-home.
static bool      g_call_log_enabled = false;     // resolved once at startup
static HANDLE    g_call_log_handle  = INVALID_HANDLE_VALUE;
static std::mutex g_call_log_lock;

static const std::vector<std::string> CORE_QUERY_TOOLS = {
    "blueprint_query",
    "material_query",
    "animation_query",
    "niagara_query",
    "editor_query",
    "config_query",
    "project_query",
    "source_query",
    "ui_query",
    "mesh_query",
    "gas_query",
    "combograph_query",
    "ai_query",
    "logicdriver_query",
    "audio_query",
    "level_sequence_query",
};

// ============================================================================
// Logging
// ============================================================================

static void log_msg(const std::string& msg)
{
    std::cerr << "[monolith-proxy] " << msg << std::endl;
}

// ============================================================================
// Utility: environment variable helpers
// ============================================================================

static std::string get_env(const char* name, const char* default_val = "")
{
    const char* val = std::getenv(name);
    return val ? std::string(val) : std::string(default_val);
}

static std::set<std::string> parse_csv_env(const char* name)
{
    std::set<std::string> result;
    std::string raw = get_env(name);
    if (raw.empty()) return result;

    std::istringstream ss(raw);
    std::string part;
    while (std::getline(ss, part, ','))
    {
        // trim whitespace
        size_t start = part.find_first_not_of(" \t\r\n");
        size_t end   = part.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        std::string trimmed = part.substr(start, end - start + 1);
        // lowercase
        std::transform(trimmed.begin(), trimmed.end(), trimmed.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        if (!trimmed.empty())
            result.insert(std::move(trimmed));
    }
    return result;
}

// ============================================================================
// Utility: URL parsing
// ============================================================================

static void parse_monolith_url(const std::string& url)
{
    g_monolith_url = url;

    // Strip "http://"
    std::string rest = url;
    if (rest.rfind("http://", 0) == 0)
        rest = rest.substr(7);
    else if (rest.rfind("https://", 0) == 0)
        rest = rest.substr(8);

    // Split host:port/path
    auto slash_pos = rest.find('/');
    std::string host_port = (slash_pos != std::string::npos) ? rest.substr(0, slash_pos) : rest;
    g_monolith_path_mcp = (slash_pos != std::string::npos) ? rest.substr(slash_pos) : "/mcp";

    auto colon_pos = host_port.find(':');
    if (colon_pos != std::string::npos)
    {
        g_monolith_host = host_port.substr(0, colon_pos);
        g_monolith_port = std::stoi(host_port.substr(colon_pos + 1));
    }
    else
    {
        g_monolith_host = host_port;
        g_monolith_port = 80;
    }

    // Derive health path: replace trailing /mcp with /health
    g_monolith_path_health = g_monolith_path_mcp;
    auto mcp_pos = g_monolith_path_health.rfind("/mcp");
    if (mcp_pos != std::string::npos)
        g_monolith_path_health = g_monolith_path_health.substr(0, mcp_pos) + "/health";
    else
        g_monolith_path_health = "/health";
}

// ============================================================================
// Time helper
// ============================================================================

static double now_seconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ============================================================================
// JSONL call log (Phase 4 / survivor F)
//
// One line per upstream HTTP roundtrip:
//   {"ts":"2026-05-27T18:14:56Z","namespace":"editor","action":"get_build_errors",
//    "params_hash":"<40-char-sha1-hex>","duration_ms":42.5,"ok":true,
//    "error_code":null,"result_bytes":1834}
//
// Path: <project-root>/Saved/Logs/MonolithCalls.jsonl
// Opt-out: env var MONOLITH_CALL_LOG=0
// Append semantics on Win32: CreateFile w/ FILE_APPEND_DATA — OS guarantees
// atomic end-of-file positioning for writes < 4KB. Never seek before write.
// ============================================================================

static std::string sha1_hex(const std::string& data)
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0)
        return std::string();

    DWORD hashObjSize = 0;
    DWORD cb = 0;
    if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&hashObjSize), sizeof(DWORD), &cb, 0) != 0)
    {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::string();
    }

    std::vector<UCHAR> hashObj(hashObjSize);
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjSize, nullptr, 0, 0) != 0)
    {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::string();
    }

    if (BCryptHashData(hHash,
            reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
            static_cast<ULONG>(data.size()), 0) != 0)
    {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::string();
    }

    UCHAR digest[20] = {0};
    if (BCryptFinishHash(hHash, digest, sizeof(digest), 0) != 0)
    {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::string();
    }

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    static const char* kHex = "0123456789abcdef";
    std::string out(40, '0');
    for (size_t i = 0; i < 20; ++i)
    {
        out[i * 2]     = kHex[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[digest[i]        & 0x0F];
    }
    return out;
}

static std::string iso8601_utc_now()
{
    // Second-precision ISO-8601 UTC, e.g. "2026-05-27T18:14:56Z".
    std::time_t t = std::time(nullptr);
    std::tm tm_buf;
    gmtime_s(&tm_buf, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return std::string(buf);
}

// Resolve <project-root>/Saved/Logs/MonolithCalls.jsonl
// Priority:
//   1. MONOLITH_PROJECT_ROOT env var (explicit override)
//   2. Current working directory (proxy CWD is the project root when launched
//      by Claude Code's MCP config)
static std::string resolve_call_log_path()
{
    std::string root = get_env("MONOLITH_PROJECT_ROOT");
    if (root.empty())
    {
        char cwd_buf[MAX_PATH];
        DWORD n = GetCurrentDirectoryA(MAX_PATH, cwd_buf);
        if (n > 0 && n < MAX_PATH)
            root = std::string(cwd_buf, n);
        else
            root = ".";
    }

    // Strip any trailing slash so we can append uniformly
    while (!root.empty() && (root.back() == '\\' || root.back() == '/'))
        root.pop_back();

    std::string saved   = root + "\\Saved";
    std::string logsdir = saved + "\\Logs";

    CreateDirectoryA(saved.c_str(), nullptr);    // OK if already exists
    CreateDirectoryA(logsdir.c_str(), nullptr);

    return logsdir + "\\MonolithCalls.jsonl";
}

static void init_call_log()
{
    // Default-enabled; only "0" disables. Read once at startup, cache the bool.
    g_call_log_enabled = get_env("MONOLITH_CALL_LOG", "1") != "0";
    if (!g_call_log_enabled)
    {
        log_msg("Call log disabled (MONOLITH_CALL_LOG=0)");
        return;
    }

    std::string path = resolve_call_log_path();
    g_call_log_handle = CreateFileA(
        path.c_str(),
        FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (g_call_log_handle == INVALID_HANDLE_VALUE)
    {
        log_msg("Failed to open call log at " + path + " -- logging disabled");
        g_call_log_enabled = false;
        return;
    }

    log_msg("Call log: " + path);
}

// Canonicalised JSON over a params-style object: sorted keys, no whitespace.
// Matches Python json.dumps(sort_keys=True, separators=(",",":")).
static std::string canonical_json(const json& value)
{
    // nlohmann::json's underlying object is std::map (alphabetically sorted by
    // key); dump(-1) emits compact form (no spaces). For nested objects, the
    // library walks std::map-order recursively. This matches the sorted-keys
    // contract for hashing purposes.
    if (value.is_null())
        return "{}";
    return value.dump(-1);
}

// Extract (namespace, action) from a JSON-RPC message.
//   - tools/call w/ name matching "*_query": namespace = prefix, action = args.action
//   - tools/call w/ name matching "monolith_*": namespace = "monolith", action = suffix
//   - tools/call other: namespace = name, action = ""
//   - non-tools/call (initialize, ping, tools/list): namespace = method, action = ""
static void extract_namespace_action(const json& msg, std::string& ns, std::string& action)
{
    std::string method = msg.value("method", "");
    if (method != "tools/call")
    {
        ns = method;
        action = "";
        return;
    }

    auto params_it = msg.find("params");
    if (params_it == msg.end() || !params_it->is_object())
    {
        ns = "tools/call";
        action = "";
        return;
    }

    std::string name = params_it->value("name", "");
    const std::string query_suffix = "_query";
    if (name.size() > query_suffix.size() &&
        name.compare(name.size() - query_suffix.size(), query_suffix.size(), query_suffix) == 0)
    {
        ns = name.substr(0, name.size() - query_suffix.size());
        json args = params_it->value("arguments", json::object());
        if (args.is_object())
            action = args.value("action", "");
        else
            action = "";
        return;
    }

    const std::string monolith_prefix = "monolith_";
    if (name.rfind(monolith_prefix, 0) == 0)
    {
        ns = "monolith";
        action = name.substr(monolith_prefix.size());
        return;
    }

    ns = name;
    action = "";
}

// Extract params dict for hashing. For tools/call, that's params.arguments;
// for other methods, that's the whole params object.
static json extract_params_for_hash(const json& msg)
{
    std::string method = msg.value("method", "");
    auto params_it = msg.find("params");
    if (params_it == msg.end() || !params_it->is_object())
        return json::object();

    if (method == "tools/call")
    {
        json args = params_it->value("arguments", json::object());
        if (!args.is_object())
            return json::object();
        return args;
    }
    return *params_it;
}

// Inspect a forwarded HTTP response to extract (ok, error_code, result_bytes).
//   - ok = true iff response is valid JSON-RPC AND has no top-level "error"
//   - error_code = response.error.code if present, else null
//   - result_bytes = length of serialised result payload (or full response if no result)
static void inspect_response(const std::string& resp, bool& ok,
    std::optional<int>& error_code, size_t& result_bytes)
{
    ok = false;
    error_code.reset();
    result_bytes = 0;

    if (resp.empty())
        return;

    try
    {
        json parsed = json::parse(resp);
        auto err_it = parsed.find("error");
        if (err_it != parsed.end() && err_it->is_object())
        {
            ok = false;
            auto code_it = err_it->find("code");
            if (code_it != err_it->end() && code_it->is_number_integer())
                error_code = code_it->get<int>();
        }
        else
        {
            ok = true;
        }

        auto result_it = parsed.find("result");
        if (result_it != parsed.end())
            result_bytes = result_it->dump(-1).size();
        else
            result_bytes = resp.size();
    }
    catch (...)
    {
        // Unparseable -- treat as failure with no error_code; record full body size
        ok = false;
        result_bytes = resp.size();
    }
}

static void write_call_log_line(const json& msg, const std::string& resp, double duration_ms)
{
    if (!g_call_log_enabled || g_call_log_handle == INVALID_HANDLE_VALUE)
        return;

    try
    {
        std::string ns;
        std::string action;
        extract_namespace_action(msg, ns, action);

        json params_for_hash = extract_params_for_hash(msg);
        std::string canonical = canonical_json(params_for_hash);
        std::string params_hash = sha1_hex(canonical);

        bool ok = false;
        std::optional<int> error_code;
        size_t result_bytes = 0;
        inspect_response(resp, ok, error_code, result_bytes);

        json line;
        line["ts"]           = iso8601_utc_now();
        line["namespace"]    = ns;
        line["action"]       = action;
        line["params_hash"]  = params_hash;
        line["duration_ms"]  = duration_ms;
        line["ok"]           = ok;
        if (error_code.has_value())
            line["error_code"] = error_code.value();
        else
            line["error_code"] = nullptr;
        line["result_bytes"] = static_cast<int64_t>(result_bytes);

        std::string serialised = line.dump(-1);
        serialised.push_back('\n');

        // FILE_APPEND_DATA guarantees the kernel positions writes at EOF
        // atomically; single WriteFile call keeps the line indivisible up to
        // PIPE_BUF / 4KB. Mutex guards our handle from cross-thread races
        // (defence in depth -- only the dispatcher main thread writes today).
        std::lock_guard<std::mutex> lock(g_call_log_lock);
        DWORD written = 0;
        WriteFile(g_call_log_handle,
            serialised.data(),
            static_cast<DWORD>(serialised.size()),
            &written,
            nullptr);
    }
    catch (const std::exception& e)
    {
        log_msg(std::string("Call-log write failed: ") + e.what());
    }
    catch (...)
    {
        // never let logging crash the proxy
    }
}

// ============================================================================
// WinHTTP client
// ============================================================================

static std::wstring to_wide(const std::string& s)
{
    if (s.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], sz);
    return ws;
}

// POST JSON to Monolith. Returns response body or empty string on failure; when
// out_failure is given it says whether the failure was a timeout (the editor accepted
// the request but did not answer in time) or the editor was unreachable.
static std::string post_monolith(const std::string& body, PostFailure* out_failure = nullptr)
{
    if (out_failure) *out_failure = PostFailure::Unreachable;
    const double timeout_sec = g_request_timeout_s;
    HINTERNET hSession = WinHttpOpen(
        L"MonolithProxy/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) return {};

    std::wstring whost = to_wide(g_monolith_host);
    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), (INTERNET_PORT)g_monolith_port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return {}; }

    std::wstring wpath = to_wide(g_monolith_path_mcp);
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"POST", wpath.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }

    // Set timeouts (milliseconds). Resolve/connect stay short so a closed editor is
    // reported promptly; send/receive use the full request timeout.
    const DWORD timeout_ms = (DWORD)(timeout_sec * 1000);
    const DWORD connect_ms = (DWORD)((std::min)(timeout_sec, CONNECT_TIMEOUT) * 1000);
    WinHttpSetTimeouts(hRequest, connect_ms, connect_ms, timeout_ms, timeout_ms);

    // Send
    const wchar_t* hdrs = L"Content-Type: application/json";
    BOOL ok = WinHttpSendRequest(
        hRequest, hdrs, (DWORD)-1,
        (LPVOID)body.c_str(), (DWORD)body.size(),
        (DWORD)body.size(), 0);

    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok)
    {
        const DWORD err = GetLastError();
        if (out_failure && err == ERROR_WINHTTP_TIMEOUT) *out_failure = PostFailure::Timeout;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    // Read response
    std::string response;
    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0)
    {
        std::string chunk(bytesAvailable, '\0');
        DWORD bytesRead = 0;
        WinHttpReadData(hRequest, &chunk[0], bytesAvailable, &bytesRead);
        response.append(chunk.c_str(), bytesRead);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (out_failure) *out_failure = response.empty() ? PostFailure::Unreachable : PostFailure::None;
    return response;
}

// GET health endpoint. Returns true if 200 OK.
static bool check_monolith_up()
{
    HINTERNET hSession = WinHttpOpen(
        L"MonolithProxy/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) return false;

    std::wstring whost = to_wide(g_monolith_host);
    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), (INTERNET_PORT)g_monolith_port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    std::wstring wpath = to_wide(g_monolith_path_health);
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", wpath.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    // 3-second timeout for health check
    DWORD timeout_ms = 3000;
    WinHttpSetTimeouts(hRequest, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    BOOL ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok || !WinHttpReceiveResponse(hRequest, nullptr))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return statusCode == 200;
}

// ============================================================================
// JSON-RPC helpers
// ============================================================================

static std::string make_result(const json& id, const json& result)
{
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"] = result;
    return resp.dump();
}

static std::string make_tool_error(const json& id, const std::string& message)
{
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"] = {
        {"content", json::array({{{"type", "text"}, {"text", message}}})},
        {"isError", true}
    };
    return resp.dump();
}

static std::string make_jsonrpc_error(const json& id, int code, const std::string& message)
{
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["error"] = {{"code", code}, {"message", message}};
    return resp.dump();
}

// ============================================================================
// Stable tools/list fallback
// ============================================================================

static std::string sanitize_cache_part(std::string value)
{
    for (char& c : value)
    {
        const bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_';
        if (!ok)
            c = '_';
    }
    return value;
}

static std::string tools_cache_path()
{
    std::string base = get_env("LOCALAPPDATA");
    if (base.empty())
        base = get_env("TEMP", ".");

    std::string dir = base + "\\Monolith";
    CreateDirectoryA(dir.c_str(), nullptr);

    return dir + "\\monolith_proxy_tools_" +
        sanitize_cache_part(g_monolith_host) + "_" +
        std::to_string(g_monolith_port) + ".json";
}

static json make_query_tool_schema()
{
    return {
        {"type", "object"},
        {"properties", {
            {"action", {
                {"type", "string"},
                {"description", "The action to execute. Use monolith_discover first when the editor is available."}
            }},
            {"params", {
                {"type", "object"},
                {"description", "Parameters for the selected action."}
            }},
            {"_fields", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit."}
            }},
            {"_omit", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields."}
            }},
            {"_compact_json", {
                {"type", "boolean"},
                {"description", "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object."}
            }}
        }},
        {"required", json::array({"action"})}
    };
}

static json make_empty_object_schema()
{
    return {
        {"type", "object"},
        {"properties", {
            {"_fields", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit."}
            }},
            {"_omit", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields."}
            }},
            {"_compact_json", {
                {"type", "boolean"},
                {"description", "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object."}
            }}
        }}
    };
}

static json make_tool(const std::string& name, const std::string& description, const json& schema)
{
    return {
        {"name", name},
        {"description", description},
        {"inputSchema", schema}
    };
}

static json make_seed_tools()
{
    json tools = json::array();

    for (const std::string& name : CORE_QUERY_TOOLS)
    {
        std::string domain = name;
        const std::string suffix = "_query";
        if (domain.size() > suffix.size() &&
            domain.compare(domain.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            domain.resize(domain.size() - suffix.size());
        }

        tools.push_back(make_tool(
            name,
            "Query the " + domain + " domain. The editor may be offline at session start; retry after Monolith is healthy.",
            make_query_tool_schema()));
    }

    tools.push_back(make_tool(
        "monolith_discover",
        "List available tool namespaces and their actions. Pass namespace and optional category to filter.",
        {
            {"type", "object"},
            {"properties", {
                {"namespace", {
                    {"type", "string"},
                    {"description", "Optional: filter to a specific namespace"}
                }},
                {"category", {
                    {"type", "string"},
                    {"description", "Optional: filter actions within the namespace by category"}
                }},
                {"_fields", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description", "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit."}
                }},
                {"_omit", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description", "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields."}
                }},
                {"_compact_json", {
                    {"type", "boolean"},
                    {"description", "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object."}
                }}
            }}
        }));

    tools.push_back(make_tool(
        "monolith_status",
        "Get Monolith server health: version, uptime, port, registered action count, and module status.",
        make_empty_object_schema()));

    tools.push_back(make_tool(
        "monolith_update",
        "Check for or install Monolith updates from GitHub Releases.",
        {
            {"type", "object"},
            {"properties", {
                {"action", {
                    {"type", "string"},
                    {"description", "'check' to compare versions, 'install' to download and stage update"},
                    {"default", "check"}
                }},
                {"_fields", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description", "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit."}
                }},
                {"_omit", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description", "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields."}
                }},
                {"_compact_json", {
                    {"type", "boolean"},
                    {"description", "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object."}
                }}
            }}
        }));

    tools.push_back(make_tool(
        "monolith_reindex",
        "Re-index the Monolith project database. Requires the editor-side Monolith server.",
        make_empty_object_schema()));

    return tools;
}

static void write_tools_cache(const std::string& response)
{
    try
    {
        json payload = json::parse(response);
        auto result_it = payload.find("result");
        if (result_it == payload.end() || !result_it->is_object())
            return;

        auto tools_it = result_it->find("tools");
        if (tools_it == result_it->end() || !tools_it->is_array() || tools_it->empty())
            return;

        std::ofstream out(tools_cache_path(), std::ios::binary | std::ios::trunc);
        if (out)
            out << tools_it->dump();
    }
    catch (const std::exception& e)
    {
        log_msg(std::string("Failed to write tools/list cache: ") + e.what());
    }
}

static std::optional<json> read_tools_cache()
{
    try
    {
        std::ifstream in(tools_cache_path(), std::ios::binary);
        if (!in)
            return std::nullopt;

        json tools;
        in >> tools;
        if (!tools.is_array() || tools.empty())
            return std::nullopt;

        return tools;
    }
    catch (const std::exception& e)
    {
        log_msg(std::string("Failed to read tools/list cache: ") + e.what());
        return std::nullopt;
    }
}

static std::string make_fallback_tools_list_response(const json& msg)
{
    if (auto cached = read_tools_cache())
    {
        log_msg("Monolith down during tools/list -- returning cached tools");
        return make_result(msg.value("id", json()), {{"tools", cached.value()}});
    }

    log_msg("Monolith down during tools/list -- returning seed tools");
    return make_result(msg.value("id", json()), {{"tools", make_seed_tools()}});
}

// ============================================================================
// stdout writing (thread-safe)
// ============================================================================

static void write_stdout(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(g_stdout_lock);
    std::cout << msg << "\n";
    std::cout.flush();
}

// ============================================================================
// Dedup tracking
// ============================================================================

static std::string tool_signature(const json& msg)
{
    auto params_it = msg.find("params");
    if (params_it == msg.end() || !params_it->is_object())
        return {};

    auto name_it = params_it->find("name");
    if (name_it == params_it->end() || !name_it->is_string() || name_it->get<std::string>().empty())
        return {};

    // Build signature object using json (std::map-backed, sorts keys alphabetically)
    // This matches Python's json.dumps(sort_keys=True, separators=(",",":"))
    json sig;
    sig["name"] = *name_it;
    sig["arguments"] = params_it->value("arguments", json::object());

    // dump(-1) = compact, no spaces — matches Python separators=(",",":")
    return sig.dump(-1);
}

static bool is_repeated_tool_call(const json& msg)
{
    std::string sig = tool_signature(msg);
    if (sig.empty()) return false;

    auto it = g_recent_tool_calls.find(sig);
    if (it == g_recent_tool_calls.end()) return false;

    return (now_seconds() - it->second) < REPEAT_TOOL_CALL_WINDOW;
}

static void record_tool_call(const json& msg)
{
    std::string sig = tool_signature(msg);
    if (!sig.empty())
        g_recent_tool_calls[sig] = now_seconds();
}

// ============================================================================
// State check + health poll
// ============================================================================

static bool send_list_changed()
{
    try
    {
        json notification;
        notification["jsonrpc"] = "2.0";
        notification["method"] = "notifications/tools/list_changed";
        write_stdout(notification.dump());
        return true;
    }
    catch (...)
    {
        return false;
    }
}

static void check_monolith_state_change()
{
    bool is_up = check_monolith_up();

    if (g_monolith_was_up.has_value() && is_up != g_monolith_was_up.value())
    {
        const char* direction = is_up ? "online" : "offline";
        log_msg(std::string("Monolith went ") + direction + " -- sending tools/list_changed");
        send_list_changed();
    }

    g_monolith_was_up = is_up;
}

static void health_poll_thread()
{
    // Initial delay
    std::this_thread::sleep_for(
        std::chrono::milliseconds((int)(POLL_START_DELAY * 1000)));
    log_msg("Health poll started (interval=" + std::to_string((int)POLL_INTERVAL) + "s)");

    while (true)
    {
        try
        {
            check_monolith_state_change();
        }
        catch (...)
        {
            log_msg("Health poll error");
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds((int)(POLL_INTERVAL * 1000)));
    }
}

// ============================================================================
// Handlers
// ============================================================================

static std::string handle_initialize(const json& msg)
{
    std::string client_version = "2025-11-25";
    auto params_it = msg.find("params");
    if (params_it != msg.end() && params_it->is_object())
    {
        auto pv_it = params_it->find("protocolVersion");
        if (pv_it != params_it->end() && pv_it->is_string())
            client_version = pv_it->get<std::string>();
    }

    std::string version = (SUPPORTED_VERSIONS.count(client_version) > 0)
        ? client_version : "2025-11-25";

    json result;
    result["protocolVersion"] = version;
    result["capabilities"] = {{"tools", {{"listChanged", true}}}};
    result["serverInfo"] = {{"name", PROXY_NAME}, {"version", PROXY_VERSION}};
    result["instructions"] =
        "Monolith MCP proxy. Tools are forwarded to the Unreal Editor. "
        "If tools return errors about the editor not running, wait and retry.";

    return make_result(msg.value("id", json()), result);
}

static std::string handle_ping(const json& msg)
{
    return make_result(msg.value("id", json()), json::object());
}

static std::string handle_tools_list(const json& msg)
{
    double t0 = now_seconds();
    std::string resp = post_monolith(msg.dump());
    double duration_ms = (now_seconds() - t0) * 1000.0;
    write_call_log_line(msg, resp, duration_ms);

    if (!resp.empty())
    {
        if (g_split_editor_query)
        {
            try
            {
                json payload = json::parse(resp);
                auto result_it = payload.find("result");
                if (result_it != payload.end() && result_it->is_object())
                {
                    auto tools_it = result_it->find("tools");
                    if (tools_it != result_it->end() && tools_it->is_array())
                    {
                        json rewritten_tools = json::array();
                        for (auto& tool : *tools_it)
                        {
                            if (tool.is_object() && tool.value("name", "") == "editor_query")
                            {
                                // Create read tool
                                json read_tool = tool;
                                read_tool["name"] = "editor_read_query";
                                read_tool["description"] =
                                    "Read-only Unreal editor diagnostics and log access. "
                                    "Use for build status, build errors, build summary, compile output, crash context, "
                                    "and recent log queries. Never use this tool to trigger a build.";

                                // Create build tool
                                json build_tool = tool;
                                build_tool["name"] = "editor_build_query";
                                build_tool["description"] =
                                    "Mutating Unreal editor build actions only. "
                                    "Use only when the user explicitly asks to trigger a full build or a Live Coding compile.";

                                rewritten_tools.push_back(std::move(read_tool));
                                rewritten_tools.push_back(std::move(build_tool));
                                continue;
                            }
                            rewritten_tools.push_back(tool);
                        }
                        (*result_it)["tools"] = std::move(rewritten_tools);
                        resp = payload.dump();
                    }
                }
            }
            catch (const std::exception& e)
            {
                log_msg(std::string("Failed to rewrite tools/list response: ") + e.what());
            }
        }
        write_tools_cache(resp);
        return resp;
    }

    return make_fallback_tools_list_response(msg);
}

static std::string handle_tools_call(const json& msg)
{
    json id = msg.value("id", json());

    // Extract params (copy so we can modify)
    json params = msg.value("params", json::object());
    std::string tool_name = params.value("name", "unknown");
    std::string forwarded_name = tool_name;
    json args = params.value("arguments", json::object());
    if (args.is_null()) args = json::object();

    // --- Split editor_query handling ---
    if (tool_name == "editor_read_query" || tool_name == "editor_build_query")
    {
        // Validate action arg exists
        std::string action;
        auto action_it = args.find("action");
        if (action_it == args.end() || !action_it->is_string() || action_it->get<std::string>().empty())
        {
            return make_tool_error(id,
                "Tool '" + tool_name + "' requires an 'action' string argument.");
        }
        action = action_it->get<std::string>();

        // Normalize
        std::string normalized = action;
        // trim
        size_t s = normalized.find_first_not_of(" \t\r\n");
        size_t e = normalized.find_last_not_of(" \t\r\n");
        if (s != std::string::npos) normalized = normalized.substr(s, e - s + 1);
        // lowercase
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });

        if (tool_name == "editor_read_query" && EDITOR_BUILD_ACTIONS.count(normalized))
        {
            return make_tool_error(id,
                "Tool '" + tool_name + "' is read-only. Use the build-capable editor-open preset if you intentionally want '" + action + "'.");
        }
        if (tool_name == "editor_build_query" && !EDITOR_BUILD_ACTIONS.count(normalized))
        {
            // Build sorted action list string
            std::string actions_str;
            for (auto it = EDITOR_BUILD_ACTIONS.begin(); it != EDITOR_BUILD_ACTIONS.end(); ++it)
            {
                if (!actions_str.empty()) actions_str += ", ";
                actions_str += *it;
            }
            return make_tool_error(id,
                "Tool '" + tool_name + "' only supports build actions (" + actions_str + "). "
                "Use 'editor_read_query' for diagnostics and logs.");
        }

        // Remap to editor_query for forwarding
        forwarded_name = "editor_query";
        params["name"] = forwarded_name;
    }
    else if (tool_name == "editor_query")
    {
        auto action_it = args.find("action");
        if (action_it != args.end() && action_it->is_string() && !action_it->get<std::string>().empty())
        {
            std::string action = action_it->get<std::string>();
            std::string normalized = action;
            size_t s = normalized.find_first_not_of(" \t\r\n");
            size_t e = normalized.find_last_not_of(" \t\r\n");
            if (s != std::string::npos) normalized = normalized.substr(s, e - s + 1);
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                           [](unsigned char c){ return (char)std::tolower(c); });

            if (g_split_editor_query && EDITOR_BUILD_ACTIONS.count(normalized))
            {
                return make_tool_error(id,
                    "Generic 'editor_query' is not available in split-editor mode for build actions. "
                    "Use 'editor_build_query' from the build-capable preset for '" + action + "'.");
            }
        }
    }

    // --- Dedup check ---
    // Build the message we'll actually forward (with possibly rewritten params)
    json forwarded_msg = msg;
    forwarded_msg["params"] = params;

    if (is_repeated_tool_call(forwarded_msg))
    {
        return make_tool_error(id,
            "Tool '" + tool_name + "' with the same arguments was just called. "
            "Reuse the previous result and answer the user instead of repeating the same call.");
    }

    // --- Allowlist/denylist check ---
    if (forwarded_name == "editor_query")
    {
        auto action_it = args.find("action");
        if (action_it != args.end() && action_it->is_string() && !action_it->get<std::string>().empty())
        {
            std::string action = action_it->get<std::string>();
            std::string normalized = action;
            size_t s = normalized.find_first_not_of(" \t\r\n");
            size_t e = normalized.find_last_not_of(" \t\r\n");
            if (s != std::string::npos) normalized = normalized.substr(s, e - s + 1);
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                           [](unsigned char c){ return (char)std::tolower(c); });

            if (!g_editor_action_allowlist.empty() && !g_editor_action_allowlist.count(normalized))
            {
                return make_tool_error(id,
                    "Monolith editor action '" + action + "' is blocked by this preset. "
                    "Switch to the build-capable editor-open preset if you want mutating editor actions.");
            }
            if (g_editor_action_denylist.count(normalized))
            {
                return make_tool_error(id,
                    "Monolith editor action '" + action + "' is blocked by this preset. "
                    "Use the build-capable editor-open preset when you intentionally want compile or build actions.");
            }
        }
    }

    // --- Record and forward ---
    record_tool_call(forwarded_msg);

    double t0 = now_seconds();
    PostFailure failure = PostFailure::None;
    std::string resp = post_monolith(forwarded_msg.dump(), &failure);
    double duration_ms = (now_seconds() - t0) * 1000.0;
    write_call_log_line(forwarded_msg, resp, duration_ms);

    if (!resp.empty())
        return resp;

    if (failure == PostFailure::Timeout)
    {
        char secs[32];
        std::snprintf(secs, sizeof(secs), "%g", g_request_timeout_s);
        return make_tool_error(id,
            "Monolith request for tool '" + tool_name + "' timed out after " + secs + "s. "
            "The Unreal Editor is still running and may have completed the action -- "
            "verify the result before retrying. Raise the limit with MONOLITH_PROXY_TIMEOUT_S.");
    }

    return make_tool_error(id,
        "Monolith MCP is not available (Unreal Editor not running). "
        "Tool '" + tool_name + "' cannot execute. Start the editor and try again.");
}

// ============================================================================
// Main loop
// ============================================================================

int main()
{
    // Binary-safe stdin/stdout on Windows
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    // Parse configuration from environment
    std::string url = get_env("MONOLITH_URL", "http://127.0.0.1:9316/mcp");
    parse_monolith_url(url);

    {
        const std::string timeout_env = get_env("MONOLITH_PROXY_TIMEOUT_S");
        if (!timeout_env.empty())
        {
            char* end = nullptr;
            const double parsed = std::strtod(timeout_env.c_str(), &end);
            if (end != timeout_env.c_str() && parsed > 0.0 && parsed <= 86400.0)
                g_request_timeout_s = parsed;
            else
                log_msg("ignoring invalid MONOLITH_PROXY_TIMEOUT_S='" + timeout_env + "'");
        }
    }

    g_split_editor_query   = get_env("MONOLITH_SPLIT_EDITOR_QUERY", "0") == "1";
    g_editor_action_allowlist = parse_csv_env("MONOLITH_EDITOR_ACTION_ALLOWLIST");
    g_editor_action_denylist  = parse_csv_env("MONOLITH_EDITOR_ACTION_DENYLIST");

    log_msg(std::string("Started. Forwarding to ") + g_monolith_url);

    init_call_log();

    // Start background health poll thread (detached = daemon)
    std::thread poller(health_poll_thread);
    poller.detach();

    // Main stdin read loop
    std::string line;
    while (std::getline(std::cin, line))
    {
        // Trim
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty())
            continue;

        // Parse JSON
        json msg;
        try
        {
            msg = json::parse(line);
        }
        catch (const json::parse_error& e)
        {
            log_msg(std::string("Bad JSON: ") + e.what());
            continue;
        }

        std::string method = msg.value("method", "");
        bool has_id = msg.contains("id");
        std::string response;

        if (method == "initialize")
        {
            response = handle_initialize(msg);
            log_msg("Initialized");
        }
        else if (method == "notifications/initialized" || method == "initialized")
        {
            // Notification -- no response. Check if Monolith is up.
            check_monolith_state_change();
        }
        else if (method == "ping")
        {
            response = handle_ping(msg);
        }
        else if (method == "tools/list")
        {
            check_monolith_state_change();
            response = handle_tools_list(msg);
        }
        else if (method == "tools/call")
        {
            response = handle_tools_call(msg);
        }
        else
        {
            // Forward unknown methods to Monolith
            double t0 = now_seconds();
            std::string resp = post_monolith(msg.dump());
            double duration_ms = (now_seconds() - t0) * 1000.0;
            write_call_log_line(msg, resp, duration_ms);

            if (!resp.empty())
            {
                response = resp;
            }
            else if (has_id)
            {
                response = make_jsonrpc_error(msg["id"], -32601,
                    "Method not found: " + method);
            }
            // else: notification with no id, silently drop
        }

        if (!response.empty())
            write_stdout(response);
    }

    // EOF on stdin -- clean exit
    log_msg("stdin closed, exiting");
    return 0;
}
