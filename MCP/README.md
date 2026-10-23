# UE-AssetDoc MCP

The Unreal Editor with the UEAssetDoc plugin is an MCP (Model Context Protocol) server: it serves its tools at `http://127.0.0.1:8559/mcp` (Streamable HTTP) while it runs. Assets are read and written as AssetDocuments: JSON files (`<Asset>.assetdoc.json`) that the editor extracts, validates, diffs and applies with the editor's own operations. Further tools edit the open level, playtest in Play In Editor, import and export files, change settings and run Python in the editor.

This directory holds what the editor and MCP clients share:

- `tools.json`: the tool manifest (name, description and input schema of every tool, and the editor route each one calls). The editor reads it at startup; the stdio bridge reads it too, so both list the same tools.
- `schemas/`: the guides the tools serve on demand (see [Guides](#guides)).
- `src/`, `dist/`: `ue-assetdoc`, a small stdio-to-HTTP bridge for MCP clients that only start stdio servers.

## Quick start

1. Enable the UEAssetDoc plugin in your project (Unreal Engine 5.7 or 5.8, macOS or Windows) and start the editor. The editor log shows the MCP URL (`LogAssetDocumentServer: MCP (Streamable HTTP): http://127.0.0.1:8559/mcp`). It is in the log file, not necessarily in the terminal that started the editor: on macOS `~/Library/Logs/Unreal Engine/<Project>Editor/<Project>.log`, on Windows `<Project>/Saved/Logs/<Project>.log`. `health_check`, or the project's `Saved/UEAssetDoc/service.json` (written once the editor has started), confirms the port too.
2. Register the URL in your MCP client (below).
3. Ask the AI to call `health_check`: it answers the port, the project directory and the Content directory where `.assetdoc.json` files go. A first edit: `extract_asset_document` on an asset, change the document, write it to its sidecar file and call `apply_asset_document_file`.

### Claude Code

```bash
claude mcp add --transport http ue-assetdoc http://127.0.0.1:8559/mcp
```

Add `--scope project` to write it to the project's `.mcp.json`.

### Cursor

`.cursor/mcp.json` in the workspace (or `~/.cursor/mcp.json`):

```json
{
  "mcpServers": {
    "ue-assetdoc": { "url": "http://127.0.0.1:8559/mcp" }
  }
}
```

### VS Code

`.vscode/mcp.json` in the workspace:

```json
{
  "servers": {
    "ue-assetdoc": { "type": "http", "url": "http://127.0.0.1:8559/mcp" }
  }
}
```

### Codex

`~/.codex/config.toml`:

```toml
[mcp_servers.ue-assetdoc]
url = "http://127.0.0.1:8559/mcp"
```

### Clients that start stdio servers only (stdio bridge)

`ue-assetdoc` relays stdio to the editor's `/mcp` endpoint. It needs Node.js 20 or newer and its runtime dependency (`@modelcontextprotocol/sdk`), which no copy of the plugin includes: install it with `npm ci` in the `MCP` folder. A copy that already has the built bridge in `dist/` (a packaged plugin can) needs only the runtime dependencies; otherwise install everything and compile `src/` into `dist/`:

```bash
cd <plugin>/MCP
npm ci --omit=dev            # dist/ is there: runtime dependencies only
npm ci && npm run build      # no dist/: all dependencies, then build
```

```json
{
  "mcpServers": {
    "ue-assetdoc": {
      "command": "node",
      "args": ["<plugin>/MCP/dist/index.js"],
      "env": { "UE_PROJECT": "<path to the project directory or .uproject>" }
    }
  }
}
```

The bridge finds the editor in this order:

1. `UE_API_BASE`: the editor's address (`http://127.0.0.1:<port>` or its `/mcp` URL); the token is `UE_ASSETDOC_TOKEN`, or the one in the project's discovery file when that file names the same protocol, host and port (`127.0.0.1`, `localhost` and `[::1]` count as one host). An editor on another host needs `UE_ASSETDOC_TOKEN`: the local file's token is never sent there.
2. `UE_PROJECT`: the project's `Saved/UEAssetDoc/service.json` names the port the editor bound and its token. When that file is missing or names no address, the project's editor is not running: the bridge connects to no editor, and every call answers which project and file it looked for.
3. The project that contains the bridge's working directory: the nearest directory at or above it with a `.uproject` file, found whether or not its editor runs. Its `service.json` names the port and token; when that file is missing or names no address, the bridge connects to no editor, as in step 2.
4. `http://127.0.0.1:8559/mcp`, only when no project is named and none contains the working directory. No project is checked then.

When a project is known (`UE_PROJECT`, or the working directory's project in step 3), the bridge checks, right after it opens a session, that the editor serves that project (the project directory `health_check` reports). Requests and notifications go to the editor only after that check has succeeded; if opening the session or the check fails in any way, including a connection cut while the editor answers, the session is dropped and the next call starts over. An editor of another project, for example one that took the port after the project's editor closed, gets no requests: every call answers which project the editor serves instead.

The editor writes `service.json` once it has finished starting (its log shows the MCP URL a little earlier); the bridge reads the file again each time it opens a session, so it may start before the editor. When the editor is not running, the bridge still lists the tools (from `tools.json`) and every call answers where it looked and what to do. When the editor restarts, the bridge opens a new session by itself.

## Ports and several editors

The editor listens on port **8559** by default (8558 is the Zen server's); change it in **Project Settings > Plugins > UE-AssetDoc** (`HttpServerPort`; `bEnableHttpServer` turns the server off). When the port stays taken (another editor uses it), the editor retries for a few seconds, then binds the next free one of the 10 ports after it, shows a notification naming it, and records it in `Saved/UEAssetDoc/service.json` (`port`, `configuredPort`, `mcpUrl`, `pid`). A client registered with a URL then needs that URL; the stdio bridge with `UE_PROJECT` follows the file. To work with two editors at once, give each project its own port, or register one bridge per project.

## Security

The security boundary is who can connect to the server: any client that can connect can make the editor run code (Python when `execute_python` is on, Blueprints it authors and runs in Play In Editor, and more; see [SECURITY.md](../SECURITY.md)). So the server accepts requests only from this machine and refuses what a web page could send:

- **Loopback only.** The listener binds the loopback interface. A request whose `Host` is not `localhost`, `127.0.0.1` or `[::1]` is refused (a page on a domain that resolves to 127.0.0.1 sends its own domain), and so is one from another machine.
- **Origin.** A request that carries an `Origin` header is answered only when that origin is a page served from localhost.
- **JSON bodies.** `POST`, `PUT` and `PATCH` must declare `Content-Type: application/json`, which a page cannot send to another origin without a CORS preflight (and the server answers none).
- **Bearer token (optional).** With **Require Auth Token** on, every request needs `Authorization: Bearer <token>`. The editor makes a new token each time it starts and writes it to `Saved/UEAssetDoc/service.json`, which is readable by your user only; the stdio bridge reads it there. A client registered with a URL sends it as a header, for example `claude mcp add --transport http ue-assetdoc http://127.0.0.1:8559/mcp --header "Authorization: Bearer <token>"` (Codex: `bearer_token_env_var`).
- **Remote connections.** **Allow Remote Connections** binds every interface and accepts other hosts; the token is then always required.
- **Tools you can switch off.** **Enable Execute Python** and **Enable Delete Assets** (Security category) remove `execute_python` and `delete_assets` from the tool list and make their routes refuse. They keep honest clients and accidental calls away from those tools; they are not a sandbox. Both are on by default: Python is how an AI does what no other tool covers, and a client that can connect can run code anyway.

As hardening (not a boundary), no tool changes the plugin's own settings or the settings that make the editor run code (`settings` refuses those writes: `Settings.Protected`), and the play tools keep to the game: no Python, no command runners, and `set` / `get` console commands only on objects of the play session (`Play.TargetOutsidePlay`).

These checks apply to every route of the server, `/mcp` and `/assetdoc/*` alike. Settings other than the two tool switches take effect when the editor starts. What the endpoint exposes, what the checks do not cover and how to report a vulnerability: [SECURITY.md](../SECURITY.md).

## Tools

Every client sees the same list.

| Group | Tools |
|------|------|
| AssetDocument authoring | `get_asset_document_schema`, `inspect_asset_document_target`, `inspect_asset_document_profile`, `create_asset_document_template`, `extract_asset_document`, `validate_asset_document`, `diff_asset_document`, `apply_asset_document_file`, `duplicate_asset` |
| Discovery (read-only) | `find_assets` (Asset Registry by path, class, name, graph content), `list_types` (classes, structs, enums and per-profile catalogs) |
| Animation | `sample_animation_bones`, `retarget_animations` |
| Level editing | `query_level`, `edit_level`, `undo_editor`, `open_level`, `new_level` |
| Playtest | `play`, `query_play`, `get_viewport_screenshot` |
| Asset files and settings | `import_assets`, `export_assets`, `save_assets`, `reload_assets`, `delete_assets`, `asset_editor`, `settings` |
| Editor | `health_check`, `get_editor_context`, `execute_python`, `get_level_editing_guide` |

Asset changes go through files: write the document to its sidecar file, then `apply_asset_document_file`. `validate_asset_document` and `diff_asset_document` also take an inline document and never change anything. `execute_python` covers what no tool does; each run is one Undo entry. Long calls (a playtest wait, a large apply) are not cut off by the server.

## Guides

Tool descriptions stay short; the details are in guides that tools serve on demand:

- `get_asset_document_schema`: without arguments, the core AssetDocument guide (`schemas/AssetDocument.md`: workflow, document header, omitted-versus-empty rule, validate and diff, diagnostics) and the list of profile sections. With `profile` (a class path or section name): that profile's section (`schemas/assetdocument/<Name>.md`), the Body shapes it accepts. Long sections keep reference parts in `schemas/assetdocument/<Name>/<part>.md`, read with `profile` and `part`.
- `get_level_editing_guide`: topic `level` (`schemas/LevelEditing.md`), `playtest` (`schemas/Playtest.md`) or `import_settings` (`schemas/ImportAndSettings.md`).

A profile section starts with a `# Title` line, then a `Classes:` line listing the class paths it documents; that line is how a class path finds its section. The editor reads the guides at each call, so an edited guide is served without a restart.

## Packaging

`Config/FilterPlugin.ini` adds `MCP/tools.json`, `MCP/schemas/`, the bridge and this README to a packaged copy of the plugin; the editor needs `tools.json` and `schemas/` to serve its tools. For the bridge it adds what `npm ci && npm run build` needs (`MCP/src/`, `MCP/tsconfig.json`, `MCP/package.json`, `MCP/package-lock.json`) and the built `MCP/dist/`. `dist/` is not in the repository: to ship the bridge built, run `npm ci && npm run build` in `MCP/` before packaging. `node_modules/` is never packaged, so users of the package still install the runtime dependencies (`npm ci --omit=dev`, or `npm ci && npm run build` without `dist/`) as in [the stdio bridge setup](#clients-that-start-stdio-servers-only-stdio-bridge).

## Development

```bash
npm test    # builds, then runs the tests in dist/bridge/*.test.js
```

The tests check the manifest and the guide files (catalog, routes, size budgets), and run the bridge over stdio against a stub `/mcp` endpoint; no editor is needed. The editor side (endpoint, tools, guides, security) has automation tests under `UEAssetDoc.HttpServer.`. `scripts/editor_mcp_parity.mjs` compares the tool results of a running editor through `/mcp` and through the bridge with those of another stdio server build.

## License

Apache License 2.0; see the `LICENSE` and `NOTICE` files at the repository root. The npm packages the bridge installs, with their licenses, are listed in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
