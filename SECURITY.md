# Security

## The boundary: who can connect

While the Unreal Editor runs with the plugin enabled, it serves an HTTP endpoint (MCP over Streamable HTTP at `/mcp`, plus the `/assetdoc/*` routes the tools use) on port 8559 by default. The security boundary is **who can connect to that endpoint**. The plugin enforces it with these checks, on every route:

- **Loopback only.** The listener binds the loopback interface. A request from another machine is refused, and so is one whose `Host` header is not `localhost`, `127.0.0.1` or `[::1]` (which keeps out DNS-rebinding pages).
- **Origin check.** A request that carries an `Origin` header is answered only when the origin is a page served from localhost.
- **JSON bodies.** `POST`, `PUT` and `PATCH` must declare `Content-Type: application/json`. A web page cannot send that to another origin without a CORS preflight, and the server answers none.
- **Bearer token (optional).** With **Require Auth Token** on (`bRequireAuthToken`), every request needs `Authorization: Bearer <token>`. The editor makes a new token each time it starts and writes it to `Saved/UEAssetDoc/service.json` of the project, readable by the current user only. The `ue-assetdoc` stdio bridge reads it from there.
- **Remote connections are off.** **Allow Remote Connections** (`bAllowRemoteConnections`) binds every interface and accepts other hosts, and then always requires the token. The traffic is plain HTTP: use it only on a network you trust or behind a tunnel that encrypts it.

These checks do not separate processes that run as the same user on the same machine: without the token, any of them can call the endpoint. Turn on **Require Auth Token** when other software you do not trust runs as you, and keep `Saved/UEAssetDoc/service.json` out of version control and backups that others can read.

## What a connected client can do

**Any client that can connect can make the editor run code**, as the user who runs the editor, whatever the tool switches below say. For example, it can:

- run Python with `execute_python` (when that tool is on), which can read and write any file the editor's user can, start processes and use the network;
- author a Blueprint (`apply_asset_document_file`) that calls console commands, Python script nodes or any function the engine exposes to Blueprints, and run it with `play`;
- change assets, levels and project settings, import files from local paths, write exported files and save packages (`edit_level`, `import_assets`, `export_assets`, `save_assets`, `settings` and others), and read project content, screenshots and the running game;
- delete assets with `delete_assets` (when that tool is on).

Treat access to the endpoint like a shell session of the editor's user. An MCP client you register is trusted with that, and so is every prompt it follows: an AI that reads untrusted text (web pages, documents, asset names) can be steered into calling these tools. Use clients that ask before calling tools, and give an AI client this endpoint only when you would let it edit and run your project.

## What the tool switches do, and what they do not

**Enable Execute Python** (`bEnableExecutePython`) and **Enable Delete Assets** (`bEnableDeleteAssets`), in **Project Settings > Plugins > UE-AssetDoc > Security**, remove `execute_python` and `delete_assets` from the tool list and make their routes refuse. Both are on by default. They keep honest clients and accidental calls away from those two tools. They are **not a sandbox**: a client that can connect can still run code in the editor in other ways, as above.

## Hardening (defence in depth, not a boundary)

The tools also refuse some things that would let a client get around the switches by accident or change what the user decided. These limits make misuse harder; they do not make a connected client untrusted-safe, and a way around them is not a security boundary failure.

- **The plugin's own settings.** No tool reads or writes the plugin's settings (the port, remote connections, the token, the switches): `settings` refuses their section, and the play tools refuse console commands and values that name them.
- **Settings that run code.** `settings` reads but never writes a short list of sections that make the editor run code or open a channel that runs it: the Python plugin's settings (startup scripts, script paths, remote execution), the Remote Control and the engine's own MCP server settings, the command lines Movie Render Queue runs, the Python script game feature templates run, and the editor terminal's shell and startup commands.
- **Play reaches the game only.** The play tools never run Python. Their console refuses `py`, editor commands and every command that hands text on as another command (ServerExec, `defer`, `ke`/`ce` calls of such functions), in any command of a line and in any word that unescapes to other text; `call` refuses functions that run console commands or Python and exec functions that take text; and `input` refuses keys whose debug key binding runs a refused command. The property commands `set`, `setnopec` and `get` act only on objects of the play session (the play world, its game instance and local players): their target is resolved as the engine resolves it, and class defaults, settings objects, editor-world objects and class-wide sets of config classes are refused. Play's own `set` action follows object references only into the play session.

## Reporting a vulnerability

Please report it privately through a GitHub private security advisory on the repository: <https://github.com/oscarrr112/UE-AssetDoc/security/advisories/new>. Do not open a public issue for it. Include the editor and plugin versions, the settings that were on, and the request or steps that show the problem. You will get an answer as soon as the maintainer can read the report; a fix is released before the details are made public.

Reports that matter most: a way to call the endpoint without the token when it is required, to get past the `Host` or `Origin` checks, to reach the endpoint from a web page or another machine against the defaults above, or to make a tool read or write outside its stated scope (files outside what the tool is meant to touch, for example). Running code while a tool switch is off is expected for a connected client (see above); a gap in the hardening limits is welcome as an ordinary issue.

Settings other than the two tool switches take effect when the editor starts. The details are in [MCP/README.md](MCP/README.md#security).
