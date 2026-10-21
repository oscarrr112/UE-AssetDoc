# Security

## What the plugin exposes

While the Unreal Editor runs with the plugin enabled, it serves an HTTP endpoint (MCP over Streamable HTTP at `/mcp`, plus the `/assetdoc/*` routes the tools use) on port 8559 by default. Whoever can call that endpoint can drive the editor as the user who runs it:

- `execute_python` runs arbitrary Python inside the editor. It can read and write any file the editor's user can, start processes and use the network.
- `delete_assets` deletes assets from the project.
- Other tools change assets, levels and editor settings, import files from local paths, write exported files and save packages (`apply_asset_document_file`, `edit_level`, `import_assets`, `export_assets`, `save_assets`, `settings` and others), and read project content, screenshots and the running game.

Treat access to the endpoint like a shell session of the editor's user. An MCP client you register is trusted with that, and so is every prompt it follows: an AI that reads untrusted text (web pages, documents, asset names) can be steered into calling these tools. Use clients that ask before calling tools, and turn off the tools you do not need (below).

## Defaults

- **Loopback only.** The listener binds the loopback interface. A request from another machine is refused, and so is one whose `Host` header is not `localhost`, `127.0.0.1` or `[::1]` (which keeps out DNS-rebinding pages).
- **Origin check.** A request that carries an `Origin` header is answered only when the origin is a page served from localhost.
- **JSON bodies.** `POST`, `PUT` and `PATCH` must declare `Content-Type: application/json`. A web page cannot send that to another origin without a CORS preflight, and the server answers none.
- **Bearer token (optional).** With **Require Auth Token** on (`bRequireAuthToken`), every request needs `Authorization: Bearer <token>`. The editor makes a new token each time it starts and writes it to `Saved/UEAssetDoc/service.json` of the project, readable by the current user only. The `ue-assetdoc` stdio bridge reads it from there.
- **Tools you can switch off.** **Enable Execute Python** (`bEnableExecutePython`) and **Enable Delete Assets** (`bEnableDeleteAssets`), in **Project Settings > Plugins > UE-AssetDoc > Security**, remove `execute_python` and `delete_assets` from the tool list and make their routes refuse. Both are on by default.
- **Remote connections are off.** **Allow Remote Connections** (`bAllowRemoteConnections`) binds every interface and accepts other hosts, and then always requires the token. The traffic is plain HTTP: use it only on a network you trust or behind a tunnel that encrypts it.

These checks apply to every route. They do not separate processes that run as the same user on the same machine: without the token, any of them can call the endpoint. Turn on **Require Auth Token** when other software you do not trust runs as you, and keep `Saved/UEAssetDoc/service.json` out of version control and backups that others can read.

Settings other than the two tool switches take effect when the editor starts. The details are in [MCP/README.md](MCP/README.md#security).

## Reporting a vulnerability

Please report it privately through a GitHub private security advisory on the repository: <https://github.com/oscarrr112/UE-AssetDoc/security/advisories/new>. Do not open a public issue for it. Include the editor and plugin versions, the settings that were on, and the request or steps that show the problem. You will get an answer as soon as the maintainer can read the report; a fix is released before the details are made public.

Reports that matter most: a way to call the endpoint without the token when it is required, to reach it from a web page or another machine against the defaults above, to make a switched-off tool run, or to read or write files outside what the called tool is meant to touch.
