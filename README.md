# UE-AssetDoc

UE-AssetDoc is an Unreal Editor plugin that lets an AI work in the editor: the editor itself is an MCP server. Its core is **AssetDocument**: an asset is read and written as one JSON document (`<Asset>.assetdoc.json`) that the editor extracts, validates, diffs and applies with its own editing operations, so an AI edits a Blueprint, a Behavior Tree or a Niagara system the way a person does in the editor, and every change can be checked before it is made.

```
AI client  <-MCP over HTTP (http://127.0.0.1:8559/mcp)->  Unreal Editor + UEAssetDoc plugin
AI client  <-stdio->  ue-assetdoc bridge (Node.js, MCP/)  <-HTTP->  (same endpoint, for stdio-only clients)
```

Unlike tools that expose one editor action per call, an AI here works on the whole asset: it reads the document, changes it, sees exactly what would change, and the editor applies it all or nothing. [How UE-AssetDoc thinks](docs/INTRO.md) ([中文](docs/INTRO.zh-CN.md)) explains the idea in five minutes.

## What it covers

- **Assets as documents.** Exact profiles for Blueprints and Widget Blueprints, Animation Blueprints and animation assets, Behavior Trees and Blackboards, StateTrees, EQS queries, Data Tables and user-defined structs and enums, curves, Materials and Material Instances, Niagara, MetaSounds and Sound Cues, IK Rigs and retargeters, Control Rigs, Physics Assets, Level Sequences, and more. Any other asset class is handled by a generic profile over its reflected properties.
- **Asset discovery.** Asset Registry search, type catalogs, and inspection of what a class or asset can author.
- **Level editing.** Query the open level and edit it in batches that are one Undo entry each.
- **Playtesting.** Run Play In Editor, inject input, wait on conditions, read the running game and its log, take screenshots.
- **Editor utilities.** Import and export files, save, reload, delete, open and close asset editors, project and world settings, and Python in the editor for everything else.

## Requirements

- Unreal Engine 5.7 or 5.8, on macOS or Windows. The engine is not part of this repository; you need your own copy under Epic's license. Only building the plugin from source needs a C++ toolchain.
- Node.js 20 or newer, only for MCP clients that start stdio servers (the `ue-assetdoc` bridge).

## Install

There are two ways to put the plugin in a project: the prebuilt package (option A) needs no compiler and works in a Blueprint-only project; building from source (option B) needs a project that can compile C++. Both continue with [Enable the plugin](#enable-the-plugin) and [Start and connect](#start-and-connect).

### Option A: prebuilt package

1. Download the zip for your engine version and platform from [GitHub Releases](https://github.com/oscarrr112/UE-AssetDoc/releases). There is one zip per engine version and platform:

   | Zip | Engine | Editor platform |
   | --- | --- | --- |
   | `UEAssetDoc-<version>-UE5.7-Win64.zip` | Unreal Engine 5.7 | Windows |
   | `UEAssetDoc-<version>-UE5.7-Mac.zip` | Unreal Engine 5.7 | macOS (Apple silicon and Intel) |
   | `UEAssetDoc-<version>-UE5.8-Win64.zip` | Unreal Engine 5.8 | Windows |
   | `UEAssetDoc-<version>-UE5.8-Mac.zip` | Unreal Engine 5.8 | macOS (Apple silicon and Intel) |

   Pick the engine version of your project (the `EngineAssociation` of its `.uproject`, or **Help > About Unreal Editor**) and the system the editor runs on: a zip loads only in the engine version it names. Each zip is the complete plugin: its sources, content and documents, the built stdio bridge, and the editor modules for its platform. The packages are built with and tested on Unreal Engine 5.7.4 and 5.8.4. Patch releases of one engine version are meant to stay binary compatible, so other 5.7.x and 5.8.x releases should load them too; if the editor reports the plugin's modules as missing or built for another engine version, use option B.

   A team that opens the same project on Windows and on macOS unzips both zips of its engine version into the same folder: apart from `Binaries/Win64` and `Binaries/Mac`, the two hold the same files, byte for byte, so the second one only adds its binaries.
2. Unzip it into the project's `Plugins` folder (create the folder if there is none), so that the plugin's descriptor is `<Project>/Plugins/UEAssetDoc/UEAssetDoc.uplugin`.
3. [Enable the plugin](#enable-the-plugin) and open the project. Nothing is compiled, also in a Blueprint-only project. In a C++ project the plugin is part of the project build like any project plugin: the first build of the project compiles it from the included sources (several minutes) and replaces the prebuilt binaries with ones built for that machine.

### Option B: build from source

1. Put this repository in your project's `Plugins` folder, for example `<Project>/Plugins/UEAssetDoc`.
2. [Enable the plugin](#enable-the-plugin).
3. Build the project. The plugin is C++ source, so the project must be able to compile C++: a C++ project builds it as it is; a Blueprint-only project can be converted by adding a C++ class (**Tools > New C++ Class**). Either open the `.uproject`: when the plugin's binaries are missing, the editor lists the missing modules and offers to rebuild them. Or build from the command line, for example on Windows `Engine\Build\BatchFiles\Build.bat <Target>Editor Win64 Development -Project="<path>\<Project>.uproject"` (on macOS `Engine/Build/BatchFiles/Mac/Build.sh <Target>Editor Mac Development -Project=...`). `<Target>Editor` is the name of the editor build target, the file `Source/*Editor.Target.cs` of the project (without `.Target.cs`); it is usually `<Project>Editor`, but a project that was copied from a template and renamed may keep the template's name. The first full build can take tens of minutes.

### Enable the plugin

Add it to the `Plugins` list of your `.uproject` file (or tick **UE-AssetDoc** in **Edit > Plugins** and restart the editor). It depends on plugins that ship with the engine (Python Editor Script Plugin, Enhanced Input, StateTree, Niagara, MetaSound, IK Rig, Control Rig and others) and enables them.

```json
"Plugins": [
    { "Name": "UEAssetDoc", "Enabled": true }
]
```

### Start and connect

1. Start the editor. The plugin serves MCP at `http://127.0.0.1:8559/mcp`. The editor log shows the URL (`LogAssetDocumentServer: MCP (Streamable HTTP): ...`). It is in the log file, not necessarily in the terminal that started the editor: on macOS `~/Library/Logs/Unreal Engine/<Project>Editor/<Project>.log`, on Windows `<Project>/Saved/Logs/<Project>.log`. `health_check` answers with the port too, and so does `<Project>/Saved/UEAssetDoc/service.json`, which the editor writes once it has started. When another editor holds the port, the editor binds the next free one and says so.
2. Register the URL in your AI client, for example `claude mcp add --transport http ue-assetdoc http://127.0.0.1:8559/mcp`. Cursor, VS Code, Codex and stdio-only clients: see [MCP/README.md](MCP/README.md#quick-start).
3. Ask the AI to call `health_check`, then to extract an asset with `extract_asset_document` and apply a change with `apply_asset_document_file`.

### Stdio bridge

Only MCP clients that can start nothing but stdio servers need the `ue-assetdoc` bridge (Node.js 20 or newer); the others connect to the URL above. The prebuilt package already holds the built bridge in `MCP/dist/`, so it only needs its runtime dependencies: run `npm ci --omit=dev` in `<Project>/Plugins/UEAssetDoc/MCP`. A copy built from source has no `dist/`: run `npm ci && npm run build` there. The client configuration is in [MCP/README.md](MCP/README.md#clients-that-start-stdio-servers-only-stdio-bridge).

### Settings

The settings are in **Project Settings > Plugins > UE-AssetDoc**: the port, the server switch, remote connections, the bearer token, the `execute_python` and `delete_assets` switches and request logging. The editor stores them in the project's `Config/DefaultEditorPerProjectUserSettings.ini`, where you can also set them without the editor, for example a different port:

```ini
[/Script/AssetDocumentServer.AssetDocumentSettings]
HttpServerPort=8560
```

The editor reads them when it starts. A value in the per-user `Saved/Config/<Platform>Editor/EditorPerProjectUserSettings.ini` of the project, if there is one, takes precedence. The names of all settings are in [MCP/README.md](MCP/README.md#ports-and-several-editors) and [SECURITY.md](SECURITY.md).

### Troubleshooting

- **`Build.bat` says "Unable to build while Live Coding is active".** An editor of the same engine is running with Live Coding. Close it, or build from that editor if it is this project's (Live Coding, Ctrl+Alt+F11). For another project's editor, adding `-NoHotReloadFromIDE` to the `Build.bat` command line also skips the check.
- **The first build takes long.** Building the plugin for the first time compiles all of its modules, which can take tens of minutes and about 20 GB of disk for the intermediate files; later builds compile only what changed.

### Packaging the plugin

A packaged copy of the plugin keeps the stdio bridge's sources (`MCP/src/`, see [MCP/README.md](MCP/README.md#packaging)), so its users can build the bridge with `npm ci && npm run build` in its `MCP` folder. To ship the bridge built, run `npm ci && npm run build` in `MCP/` before packaging: the built `MCP/dist/` is packaged too, but `node_modules/` is not, so users of that package run `npm ci --omit=dev` in its `MCP` folder to install the bridge's runtime dependencies before starting it.

## Security

The server accepts requests from this machine only (loopback listener, `Host` and `Origin` checks, JSON request bodies), can require a bearer token, and lets you switch `execute_python` and `delete_assets` off. Who can connect is the security boundary: the switches are not a sandbox, and any client that can connect can make the editor run code, so read [SECURITY.md](SECURITY.md) (what is exposed, the defaults, how to report a vulnerability) and [MCP/README.md](MCP/README.md#security) (the settings).

## Documentation

- [MCP/README.md](MCP/README.md): client setup, ports, security, the tools and the stdio bridge.
- The guides an AI reads through the tools: `MCP/schemas/AssetDocument.md` (core AssetDocument guide), `MCP/schemas/assetdocument/` (one section per profile), and `MCP/schemas/LevelEditing.md`, `Playtest.md`, `ImportAndSettings.md` (editor tools).
- Design notes: `docs/architecture/`.

## License

Apache License 2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE). Copyright 2026 Oscar Peng, with portions copyright 2026 Houyu.

## Third-party software

The plugin has no third-party code of its own and ships no Unreal Engine code. The optional stdio bridge in `MCP/` installs npm packages (MIT, ISC and BSD licensed) when you run `npm ci` (or `npm install`). They are listed with their licenses in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), which also covers the engine and Python.
