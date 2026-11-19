# Installation

## Prerequisites

- **Unreal Engine 5.7 or 5.8** (Launcher or source build) — prebuilt zips are published per engine version; source builds work on either
- **Windows, macOS, or Linux** (the prebuilt `monolith_proxy.exe` is Windows-only — macOS/Linux use the Python proxy fallback, same protocol and features)
- **Claude Code, Cursor, or another MCP client**
- **(Optional) Python 3.8+** — Only needed for the Python proxy fallback (Mac/Linux) or to index your own project's C++ source. Engine source indexing is built-in.

## Step 1: Download Monolith

Every UE project has a `Plugins/` folder at the same level as your `.uproject`. Create it if it doesn't exist.

**Option A: Git clone (source builds)**

```bash
cd YourProject/Plugins
git clone https://github.com/tumourlove/monolith.git Monolith
```

A clone has no `Binaries/` folder, so there's no prebuilt `monolith_proxy.exe`. Build it with `Plugins/Monolith/Tools/MonolithProxy/build_proxy.bat`, or use the Python proxy (`Scripts/monolith_proxy.bat` / `.sh`) in Step 2.

**Option B: Download ZIP (easiest, and the only option for Blueprint-only projects)**

Each release ships one flat plugin zip **per Unreal Engine version** — download the one that matches **your** engine:

- `Monolith-vX.Y.Z-UE5.7.zip` — for Unreal Engine 5.7
- `Monolith-vX.Y.Z-UE5.8.zip` — for Unreal Engine 5.8

(A legacy `Monolith-vX.Y.Z.zip` is also published — it is the UE 5.7 build, kept for older auto-updaters.)

Download the zip for your engine from [GitHub Releases](https://github.com/tumourlove/monolith/releases), extract, and place at `YourProject/Plugins/Monolith/`. Each zip is the normal flat plugin built for that engine.

> **Picking the right zip matters.** A prebuilt plugin binary is locked to one engine version's ABI, so the 5.7 and 5.8 builds are not interchangeable — installing the wrong one will fail to load with a modules/version mismatch. (Source builds work on both engines; this only affects the prebuilt/Blueprint-only path.)

> **Blueprint-only projects:** The release ZIP includes precompiled DLLs with `"Installed": true` — you can launch the editor immediately, no build step needed. Just make sure you grabbed the zip for your exact UE version.
>
> **C++ projects:** Rebuild your project after installing (run UBT or build from your IDE).

## Step 2: Configure MCP Connection

**Option A: Let your AI do it**

If you're already in a Claude Code, Cursor, or Cline session, just tell your AI:

> *"Install Monolith for me — the plugin is in my Plugins folder."*

The AI will create `.mcp.json` and configure everything. After it finishes, **restart your AI session** so it picks up the new MCP server, then skip to [Step 3](#step-3-launch-unreal-editor).

**Option B: Manual setup**

Create `.mcp.json` in your **project root** (same directory as your `.uproject`):

### Claude Code (with auto-reconnect proxy — recommended)

The proxy keeps your MCP session alive when the Unreal Editor restarts. The standalone C++ proxy requires no Python.

```json
{
  "mcpServers": {
    "monolith": {
      "command": "Plugins/Monolith/Binaries/monolith_proxy.exe",
      "args": []
    }
  }
}
```

Or use the Python fallback if you prefer: `{"command": "Plugins/Monolith/Scripts/monolith_proxy.bat", "args": []}` (requires Python 3.8+).

> **No proxy?** Use the direct HTTP connection instead — you'll just need to restart Claude Code each time the editor restarts:
> ```json
> {
>   "mcpServers": {
>     "monolith": {
>       "type": "http",
>       "url": "http://127.0.0.1:9316/mcp"
>     }
>   }
> }
> ```

### Cursor / Cline

```json
{
  "mcpServers": {
    "monolith": {
      "type": "streamableHttp",
      "url": "http://127.0.0.1:9316/mcp"
    }
  }
}
```

> **Important:** Claude Code uses `"http"` as the transport type, while Cursor and Cline use `"streamableHttp"`. Using the wrong type will cause connection failures. The proxy sidesteps this entirely since it uses stdio.

## Step 3: Launch Unreal Editor

Open your `.uproject`. On first launch:

1. Monolith auto-indexes your project (30-60 seconds)
2. Check **Output Log** → filter for `LogMonolith`
3. You should see: `Monolith MCP server listening on port 9316`

## Step 4: Connect Your AI

1. Open Claude Code (or your MCP client) in the project directory
2. It auto-detects `.mcp.json` and connects
3. Verify: ask *"What Monolith tools do you have?"*

## Step 5: Verify

```bash
curl -X POST http://127.0.0.1:9316/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

You should get a JSON response listing all tools.

## Running Multiple AI Clients

Multiple CLI tools (e.g. Claude Code + Codex, or OpenCode) can use Monolith **at the same time**. The MCP server is in-process inside the editor — it starts with the editor automatically and is shared. Each client just needs its own MCP config entry: stdio clients each spawn their own `monolith_proxy.exe`, HTTP-capable clients can all point at `http://127.0.0.1:9316/mcp` directly. No extra setup.

Two things to know:

- **Actions execute sequentially on the editor's game thread**, so concurrent agents queue rather than truly run in parallel.
- **Two agents editing the same asset can overwrite each other**, exactly like two people would. Keep simultaneous agents on separate assets.

The one-at-a-time limit is about **editor instances**, not clients: two editors can't both bind port 9316. For a second project, change **Server Port** in Project Settings → Plugins → Monolith (and update that project's MCP config to match).

> **Remote access:** UE's HTTP server module doesn't expose a bind address, so the listener is reachable from other machines on your LAN — but it's unauthenticated. See the security note in the README before pointing a remote client at it; on anything but a trusted network, firewall port 9316 to loopback.
