# FAQ / Troubleshooting

| Problem | Solution |
|---------|----------|
| **Plugin doesn't appear in editor** | Verify the folder is at `YourProject/Plugins/Monolith/` and contains `Monolith.uplugin`. Check you're on UE 5.7 or 5.8, and that you grabbed the zip for that engine. |
| **MCP connection refused** | Make sure the editor is open. Check Output Log for port conflicts. Verify `.mcp.json` is in your project root. |
| **MCP session dies when editor restarts** | Switch to the proxy config (see [[Connecting Your AI]]). The proxy keeps the stdio session alive and auto-detects when the editor comes back. |
| **Proxy says "Python 3 not found"** | Switch to the native C++ proxy (`Plugins/Monolith/Binaries/monolith_proxy.exe`) — no Python needed. Or install Python 3.8+ from [python.org](https://python.org) and make sure `python` is on your PATH. |
| **Index shows 0 assets** | Run `monolith_reindex` or restart the editor. Check Output Log for indexing errors. |
| **Source tools return empty results** | Engine source indexing is built-in — no Python required. Run `monolith_reindex` and wait for completion. If results are still empty, check Output Log for `LogMonolith` errors. For project C++ source coverage, see [[Engine Source Index]]. |
| **Claude can't find any tools** | If using the proxy: check the path in `.mcp.json` points at a file that exists (`Binaries/monolith_proxy.exe`, which only ships in release zips, or `Scripts/monolith_proxy.bat` / `.sh` for the Python fallback). If using direct HTTP: check it has `"type": "http"` (Claude Code) or `"type": "streamableHttp"` (Cline/VS Code). Restart your AI tool after creating the file. |
| **Tools fail on first try** | If using direct HTTP, restart Claude Code to refresh the MCP connection. If using the proxy, the tools should auto-recover — wait a few seconds and try again. |
| **Port 9316 already in use** | Change the port in Project Settings > Plugins > Monolith, then update `.mcp.json` to match. If using the proxy, set `MONOLITH_URL=http://127.0.0.1:YOURPORT/mcp` as an environment variable. |
| **Mac/Linux: native C++ proxy missing** | The prebuilt `monolith_proxy.exe` is Windows-only. Use the Python proxy via `Plugins/Monolith/Scripts/monolith_proxy.sh` — same protocol, same features, just needs Python 3.8+. |
| **Mac/Linux: .sh script permission denied** | Make sure the script is executable: `chmod +x Plugins/Monolith/Scripts/monolith_proxy.sh`. |
| **"The following modules are missing or built with a different engine version"** | The precompiled DLLs don't match your engine version. C++ projects: rebuild. Blueprint-only projects: make sure you downloaded the release matching your exact UE version. |
| **Auto-updater says "Access is denied"** | Something is locking files in the plugin folder. Close VS Code/IDEs, or add the folder to Windows Defender exclusions. See [[Auto-Updater]]. |
| **Auto-updater shows "Editor still running"** | The editor process hasn't fully exited yet. The updater polls automatically — just wait. If it persists past 120 seconds, force-close `UnrealEditor.exe` in Task Manager. |
