# Monolith

[![UE 5.7 / 5.8](https://img.shields.io/badge/Unreal-5.7%20%2F%205.8-blue)](https://unrealengine.com)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![MCP](https://img.shields.io/badge/Protocol-MCP-purple)](https://modelcontextprotocol.io)

Monolith is an Unreal Engine editor plugin that lets your AI assistant (Claude Code, Cursor, Cline, or anything else that speaks MCP) read and edit your project: Blueprints, materials, animation, Niagara, UI, AI, GAS, audio, meshes, engine source lookups and a lot more.

I started it because every Unreal MCP setup I tried registered hundreds of separate tools, and the AI burned half its context just reading the list. Monolith puts everything behind a handful of `{namespace}_query` tools instead. The AI calls `monolith_discover()` when it needs to see what's there, and the rest of its context goes on your actual problem.

It's a solo hobby project that I use every day. There are roughly 1,500 actions across 27 namespaces, and `monolith_discover()` gives you the exact live count.

## Install

1. Grab the zip for your engine from [Releases](https://github.com/tumourlove/monolith/releases) (`-UE5.7` or `-UE5.8`) and extract it to `YourProject/Plugins/Monolith`. You can clone the repo there instead if you build from source, but a clone doesn't include the prebuilt proxy, so either build it with `Tools/MonolithProxy/build_proxy.bat` or point step 2 at `Plugins/Monolith/Scripts/monolith_proxy.bat` (needs Python 3.8+).
2. Add a `.mcp.json` next to your `.uproject`:
   ```json
   {
     "mcpServers": {
       "monolith": { "command": "Plugins/Monolith/Binaries/monolith_proxy.exe", "args": [] }
     }
   }
   ```
3. Open the editor and wait for `Monolith MCP server listening on port 9316` in the Output Log. Then ask your AI "what Monolith tools do you have?"

On macOS/Linux, or using Cursor/Cline? The [Installation](https://github.com/tumourlove/monolith/wiki/Installation) page covers the other setups.

## Links

- [Wiki](https://github.com/tumourlove/monolith/wiki): setup, configuration, FAQ and the full tool reference
- [API reference](Docs/API_REFERENCE.md): every action and its params
- [Changelog](CHANGELOG.md): what changed in each release
- [Contributing](CONTRIBUTING.md): PRs and issues are welcome, and every release credits the people whose work shipped in it
- [Security](SECURITY.md): the MCP server on port 9316 is reachable from your LAN, so read this if you're on a network you don't trust

## License

[MIT](LICENSE). Credits are in [ATTRIBUTION.md](ATTRIBUTION.md).
