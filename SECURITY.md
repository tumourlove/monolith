# Security Policy

## Reporting a Vulnerability

If you find a security issue in Monolith, **please report it privately first**:

- **Preferred:** Open a [GitHub Security Advisory](https://github.com/tumourlove/monolith/security/advisories/new)
- **Alternative:** Email `leviathansteam666@gmail.com` with `[Monolith Security]` in the subject

I aim to acknowledge reports within 7 days and ship a fix or mitigation in the next release. Please do **not** open a public GitHub issue for security-impacting findings before that window.

## Threat Model

Monolith is a developer tool. The MCP HTTP server runs locally inside the Unreal Editor and is intended for use by AI coding assistants on the same machine. It is **not** designed to be exposed to untrusted networks or browsers.

**In scope:**

- Cross-origin browser exploitation against `127.0.0.1:9316`
- Auto-update supply-chain integrity
- Any code path where attacker-controlled input from a remote source reaches the editor

**Out of scope** (assumed compromised — Monolith is the least of your worries):

- Local malware running on the developer's machine
- A compromised maintainer GitHub account pushing a malicious release — partially mitigated by SHA256 verification once the marker is present in release notes
- Denial of service on the local MCP server — it's a developer tool, restart the editor

## Supported Versions

Only the latest tagged release is supported. Older versions do not receive backports.

## Hardening Recommendations for Users

- Keep `bAutoUpdateEnabled = false` (the default as of v0.14.6) and apply updates manually
- Verify the `Monolith-SHA256:` value in the release notes against the SHA256 of the downloaded zip before extracting (auto-updater does this for you when enabled)
- Do not run the editor on a machine where untrusted users have local accounts
- The MCP server binds to all network interfaces (limitation of UE's `FHttpServerModule`). If your machine is on an untrusted LAN, either:
  - Add a Windows Firewall rule blocking inbound TCP on port 9316 from non-loopback addresses, OR
  - Set `bMcpServerEnabled = false` in Editor Preferences > Plugins > Monolith when not actively using AI tooling

## Acknowledgements

Thanks to security researchers who have responsibly disclosed issues:

- **@krojew** — CORS wildcard, auto-update integrity (Issue #38, 2026-04)
