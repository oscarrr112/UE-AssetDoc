// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

import test from "node:test";
import assert from "node:assert/strict";
import { existsSync, readdirSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";

import { listedTools, loadManifest, PACKAGE_DIR, type ManifestTool } from "./manifest.js";

const manifest = loadManifest();
const SCHEMAS_DIR = join(PACKAGE_DIR, "schemas");
const SECTIONS_DIR = join(SCHEMAS_DIR, "assetdocument");

const CATALOG = [
  "get_asset_document_schema",
  "find_assets",
  "list_types",
  "inspect_asset_document_target",
  "inspect_asset_document_profile",
  "create_asset_document_template",
  "extract_asset_document",
  "validate_asset_document",
  "diff_asset_document",
  "apply_asset_document_file",
  "duplicate_asset",
  "sync_asset_document",
  "sample_animation_bones",
  "retarget_animations",
  "health_check",
  "get_editor_context",
  "query_level",
  "edit_level",
  "undo_editor",
  "open_level",
  "new_level",
  "get_level_editing_guide",
  "play",
  "query_play",
  "execute_python",
  "get_viewport_screenshot",
  "delete_assets",
  "import_assets",
  "settings",
  "save_assets",
  "reload_assets",
  "asset_editor",
  "export_assets",
];

const REMOVED_TOOLS = [
  "generate_assets",
  "get_generator_schema",
  "list_generators",
  "extract_assets",
  "query_asset",
  "update_datatable_rows",
  "extract_blueprint_graph",
  "extract_blueprint_as_bsl",
  "apply_blueprint_as_bsl",
  "chat_completion",
  "apply_blueprint_change",
  "generate_blueprint_change",
  "repair_blueprint_json",
  "orchestrate_modify_request",
  "validate_blueprint_json",
  "layout_blueprint_graph",
  "apply_asset_document",
  "inspect_anim_montage_document",
  "apply_anim_sequence_document",
];

function tool(name: string): ManifestTool {
  const found = manifest.tools.find((candidate) => candidate.name === name);
  assert.ok(found, `${name} is in the manifest`);
  return found;
}

function schemaProperties(entry: ManifestTool): Record<string, unknown> {
  const properties = (entry.inputSchema as { properties?: Record<string, unknown> }).properties;
  return properties ?? {};
}

function schemaJson(name: string): string {
  return JSON.stringify(tool(name).inputSchema);
}

function readText(...segments: string[]): string {
  return readFileSync(join(...segments), "utf-8");
}

function markdownFiles(directory: string): string[] {
  return readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) {
      return markdownFiles(path);
    }
    return entry.name.endsWith(".md") ? [path] : [];
  });
}

test("the manifest names the server like the package and lists exactly the tool catalog, in order", () => {
  const packageJson = JSON.parse(readText(PACKAGE_DIR, "package.json")) as { name: string; version: string };
  assert.equal(manifest.server.name, packageJson.name);
  assert.equal(manifest.server.version, packageJson.version);
  const names = manifest.tools.map((entry) => entry.name);
  assert.deepEqual(names, CATALOG);
  assert.equal(new Set(names).size, names.length, "no duplicate tool names");
  assert.equal(names.length, 33);
});

test("removed tools are not in the manifest", () => {
  const names = new Set(manifest.tools.map((entry) => entry.name));
  for (const removed of REMOVED_TOOLS) {
    assert.ok(!names.has(removed), `${removed} is not listed`);
  }
});

test("every tool has a description, an object input schema and a call spec", () => {
  for (const entry of manifest.tools) {
    assert.ok(typeof entry.description === "string" && entry.description.length > 0, `${entry.name} has a description`);
    assert.equal((entry.inputSchema as { type?: string }).type, "object", `${entry.name} input schema is an object`);
    assert.ok(typeof entry.call === "object" && entry.call !== null, `${entry.name} has a call spec`);
    assert.ok(!("outputSchema" in entry), `${entry.name} has no output schema`);
  }
});

// Every client loads the whole tool list on each session, so it has a budget. Size as measured with AI clients: the
// tools/list result as JSON with one-space indentation (34,871 bytes for 32 tools). Budget = that + 10%.
// Details belong in the guides the tools serve on demand (get_asset_document_schema, get_level_editing_guide);
// a description keeps what an AI needs to choose the tool and call it correctly.
const TOOL_LIST_BUDGET = 38_360;
const TOOL_BUDGET = 2_600;

test("the tool list stays within its size budget", () => {
  const tools = listedTools(manifest);
  const size = JSON.stringify(tools, null, 1).length;
  assert.ok(size <= TOOL_LIST_BUDGET, `tool list is ${size} bytes, budget ${TOOL_LIST_BUDGET}: move details into the guides`);
  for (const entry of tools) {
    const own = JSON.stringify(entry, null, 1).length;
    assert.ok(own <= TOOL_BUDGET, `${entry.name} is ${own} bytes, budget ${TOOL_BUDGET} per tool`);
  }
});

test("the listed tools carry only name, description and input schema", () => {
  for (const entry of listedTools(manifest)) {
    assert.deepEqual(Object.keys(entry).sort(), ["description", "inputSchema", "name"]);
  }
});

const CALL_KEYS = new Set(["guide", "argument", "topics", "default", "method", "route", "required", "query", "query_flags", "body", "response", "result", "setting"]);
const RESPONSE_KINDS = new Set(["document", "plain"]);
const RESULT_KINDS = new Set(["poll", "execute_python", "screenshot", "delete_assets"]);

test("every call spec is well formed", () => {
  for (const entry of manifest.tools) {
    const call = entry.call as Record<string, unknown>;
    const properties = schemaProperties(entry);
    const hasProperty = (argument: unknown, where: string) => {
      assert.equal(typeof argument, "string", `${entry.name}: ${where} names an argument`);
      assert.ok(Object.hasOwn(properties, argument as string), `${entry.name}: ${where} '${String(argument)}' is a property of the input schema`);
    };
    for (const key of Object.keys(call)) {
      assert.ok(CALL_KEYS.has(key), `${entry.name}: unknown call key ${key}`);
    }

    if (call.guide !== undefined) {
      assert.ok(call.guide === "asset_document" || call.guide === "editor", `${entry.name}: guide '${String(call.guide)}' is asset_document or editor`);
      assert.equal(call.method, undefined, `${entry.name}: a guide is not a route`);
      assert.equal(call.route, undefined, `${entry.name}: a guide is not a route`);
      if (call.guide === "asset_document") {
        for (const argument of ["profile", "part"]) {
          hasProperty(argument, "guide argument");
        }
        assert.ok(existsSync(join(SCHEMAS_DIR, "AssetDocument.md")), "the core AssetDocument guide exists");
        assert.ok(statSync(SECTIONS_DIR).isDirectory(), "the profile sections directory exists");
      } else {
        hasProperty(call.argument, "topic argument");
        const topics = call.topics as Record<string, string>;
        assert.ok(topics && typeof topics === "object" && Object.keys(topics).length > 0, `${entry.name}: topics`);
        for (const [topic, file] of Object.entries(topics)) {
          assert.ok(existsSync(join(SCHEMAS_DIR, file)), `topic ${topic}: ${file} exists in schemas/`);
        }
        assert.equal(typeof call.default, "string", `${entry.name}: default guide file`);
        assert.ok(existsSync(join(SCHEMAS_DIR, call.default as string)), `default guide ${String(call.default)} exists in schemas/`);
      }
      continue;
    }

    assert.ok(call.method === "GET" || call.method === "POST", `${entry.name}: method ${String(call.method)} is GET or POST`);
    assert.ok(typeof call.route === "string" && call.route.startsWith("/"), `${entry.name}: route starts with /`);
    assert.ok(RESPONSE_KINDS.has(call.response as string), `${entry.name}: response kind ${String(call.response)}`);
    if (call.result !== undefined) {
      assert.ok(RESULT_KINDS.has(call.result as string), `${entry.name}: result kind ${String(call.result)}`);
    }

    const schemaRequired = new Set(((entry.inputSchema as { required?: string[] }).required ?? []) as string[]);
    for (const argument of (call.required as string[] | undefined) ?? []) {
      hasProperty(argument, "required argument");
      assert.ok(schemaRequired.has(argument), `${entry.name}: required '${argument}' is required by the input schema`);
    }
    for (const argument of [...((call.query as string[] | undefined) ?? []), ...((call.query_flags as string[] | undefined) ?? [])]) {
      hasProperty(argument, "query argument");
    }
    if (call.method === "GET") {
      assert.equal(call.body, undefined, `${entry.name}: a GET has no body`);
    }

    const body = call.body;
    if (body !== undefined) {
      if (typeof body === "string") {
        assert.ok(body === "arguments" || body === "document_or_file", `${entry.name}: body mode ${body}`);
        if (body === "document_or_file") {
          hasProperty("document", "body source");
          hasProperty("file_path", "body source");
        }
      } else {
        assert.ok(typeof body === "object" && body !== null && !Array.isArray(body), `${entry.name}: body fields`);
        for (const [field, spec] of Object.entries(body as Record<string, { argument?: unknown; default?: unknown }>)) {
          assert.ok(field.length > 0);
          hasProperty(spec.argument, `body field ${field}`);
        }
      }
    }

    if (call.setting !== undefined) {
      assert.ok(["execute_python", "delete_assets"].includes(entry.name), `${entry.name}: only execute_python and delete_assets are gated by a setting`);
      assert.equal(call.setting, entry.name);
    }
  }
  assert.equal(
    manifest.tools.filter((entry) => (entry.call as Record<string, unknown>).setting !== undefined).map((entry) => entry.name).join(),
    "execute_python,delete_assets",
  );
});

test("the guide tools name the files the editor reads", () => {
  assert.deepEqual(tool("get_asset_document_schema").call, { guide: "asset_document" });
  assert.deepEqual(tool("get_level_editing_guide").call, {
    guide: "editor",
    argument: "topic",
    topics: { playtest: "Playtest.md", import_settings: "ImportAndSettings.md" },
    default: "LevelEditing.md",
  });
});

test("the route table matches the editor's HTTP routes", () => {
  const routes: Record<string, [string, string]> = {
    duplicate_asset: ["POST", "/assetdocument/duplicate"],
    sync_asset_document: ["POST", "/assetdocument/sync"],
    validate_asset_document: ["POST", "/assetdocument/validate"],
    diff_asset_document: ["POST", "/assetdocument/diff"],
    undo_editor: ["POST", "/editor/undo"],
    open_level: ["POST", "/level/open"],
    new_level: ["POST", "/level/new"],
    import_assets: ["POST", "/import"],
    settings: ["POST", "/settings"],
    query_level: ["POST", "/level/query"],
    edit_level: ["POST", "/level/edit"],
    save_assets: ["POST", "/assets/save"],
    reload_assets: ["POST", "/assets/reload"],
    asset_editor: ["POST", "/assets/editor"],
    export_assets: ["POST", "/assets/export"],
    sample_animation_bones: ["POST", "/animation/sample-bones"],
    retarget_animations: ["POST", "/animation/retarget"],
    execute_python: ["POST", "/execute"],
    delete_assets: ["POST", "/delete"],
    health_check: ["GET", "/health"],
    get_editor_context: ["GET", "/context"],
  };
  for (const [name, [method, route]] of Object.entries(routes)) {
    const call = tool(name).call as { method?: string; route?: string };
    assert.deepEqual([call.method, call.route], [method, route], name);
  }
});

test("sync_asset_document posts its action, targets and regions", () => {
  const sync = tool("sync_asset_document").call as { body: Record<string, { argument: string }>; required: string[] };
  assert.deepEqual(sync.body, { action: { argument: "action" }, targets: { argument: "targets" }, regions: { argument: "regions" } });
  assert.deepEqual(sync.required, ["action"]);
  const action = (tool("sync_asset_document").inputSchema as { properties: Record<string, { enum?: string[] }> }).properties.action;
  assert.deepEqual(action.enum, ["status", "accept_sidecar", "accept_asset", "materialize"]);
});

test("execute_python posts its code and description; duplicate_asset its source and target", () => {
  const python = tool("execute_python").call as { body: Record<string, { argument: string }> };
  assert.deepEqual(python.body, { Code: { argument: "code" }, Description: { argument: "description" } });
  assert.deepEqual((tool("execute_python").inputSchema as { required?: string[] }).required, ["code"]);
  const duplicate = tool("duplicate_asset").call as { body: Record<string, { argument: string }>; required: string[] };
  assert.deepEqual(duplicate.body, { source: { argument: "source" }, target: { argument: "target" } });
  assert.deepEqual(duplicate.required, ["source", "target"]);
});

test("AssetDocument tools are listed; validate and diff expose an object schema without top-level combinators", () => {
  for (const name of [
    "apply_asset_document_file",
    "get_asset_document_schema",
    "find_assets",
    "list_types",
    "inspect_asset_document_target",
    "inspect_asset_document_profile",
    "create_asset_document_template",
    "extract_asset_document",
    "validate_asset_document",
    "diff_asset_document",
    "duplicate_asset",
    "sync_asset_document",
  ]) {
    tool(name);
  }
  for (const name of ["validate_asset_document", "diff_asset_document"]) {
    const schema = tool(name).inputSchema;
    assert.equal(schema.type, "object", `${name} exposes an object input schema`);
    for (const keyword of ["oneOf", "anyOf", "allOf", "enum", "const", "not"]) {
      assert.equal(Object.hasOwn(schema, keyword), false, `${name} has no top-level ${keyword}`);
    }
    for (const field of ["document", "file_path"]) {
      assert.ok(Object.hasOwn(schemaProperties(tool(name)), field), `${name} accepts ${field}`);
    }
  }
});

test("level tool schemas and descriptions", () => {
  const query = schemaJson("query_level");
  for (const field of ["what", "actors", "class", "exact_class", "name", "folder", "tag", "layer", "data_layer", "level",
    "near", "bounds", "selected_only", "include_unloaded", "detail", "diff_only", "properties", "limit", "offset"]) {
    assert.match(query, new RegExp(`"${field}"`), `query_level should accept ${field}`);
  }
  assert.match(query, /"full"/, "query_level detail accepts full, as query_play");

  const edit = schemaJson("edit_level");
  for (const field of ["description", "dry_run", "save", "ops", "after"]) {
    assert.match(edit, new RegExp(`"${field}"`), `edit_level should accept ${field}`);
  }
  for (const op of ["spawn", "delete", "duplicate", "set", "add_component", "remove_component", "reparent_component", "attach",
    "detach", "create_folder", "rename_folder", "delete_folder", "pin", "unpin"]) {
    assert.match(edit, new RegExp(`"${op}"`), `edit_level should list the ${op} op`);
  }
  assert.match(tool("edit_level").description, /one Edit > Undo entry/);

  const python = tool("execute_python").description;
  assert.match(python, /query_level/);
  assert.match(python, /edit_level/);
  assert.doesNotMatch(python, /until edit_level exists/);
  assert.match(python, /only for what no tool covers/);
  assert.match(python, /Do not use it for saving, reloading, editor windows, exporting or level edits/);
  for (const pattern of [/not_undoable/, /rolled_back_objects/, /exactly one \.py file path/, /save_assets/]) {
    assert.match(python, pattern);
  }
});

test("playtest tool schemas and descriptions", () => {
  const play = schemaJson("play");
  for (const field of ["action", "mode", "map", "allow_blueprint_errors", "seconds", "until", "inputs", "settle", "command", "timeout",
    "console", "actor", "function", "args", "values", "location", "rotation"]) {
    assert.match(play, new RegExp(`"${field}"`), `play should accept ${field}`);
  }
  for (const action of ["start", "stop", "status", "pause", "resume", "wait", "input", "console", "call", "set"]) {
    assert.match(play, new RegExp(`"${action}"`), `play should list the ${action} action`);
  }
  assert.match(tool("play").description, /without execute_python/);
  assert.match(play, /state\?: \\"tap\\"/, "inputs can be held (state down / up)");
  assert.match(play, /"reset_input_settings"/, "play resets the saved Enhanced Input user settings");

  const query = schemaJson("query_play");
  assert.match(query, /"input"/, "query_play what input");
  for (const field of ["what", "actors", "class", "name", "tag", "near", "properties", "detail", "limit", "offset", "since", "category", "verbosity", "text",
    "include_inactive"]) {
    assert.match(query, new RegExp(`"${field}"`), `query_play should accept ${field}`);
  }

  const screenshot = schemaJson("get_viewport_screenshot");
  for (const field of ["view", "hud", "max_width", "quality"]) {
    assert.match(screenshot, new RegExp(`"${field}"`), `get_viewport_screenshot should accept ${field}`);
  }
});

test("level, import, settings and asset file tools: schemas and descriptions", () => {
  const importSchema = schemaJson("import_assets");
  for (const field of ["action", "files", "destination", "name", "options", "pipeline", "replace_existing", "assets", "source", "save"]) {
    assert.match(importSchema, new RegExp(`"${field}"`), `import_assets should accept ${field}`);
  }
  assert.match(tool("import_assets").description, /Content Browser/);
  assert.match(tool("settings").description, /World Settings/);
  assert.match(tool("settings").description, /Config\/DefaultEngine\.ini/);

  for (const [name, fields] of [
    ["save_assets", ["assets", "force", "all_dirty", "only"]],
    ["reload_assets", ["assets", "force"]],
    ["asset_editor", ["action", "assets", "discard_unapplied", "apply_editor"]],
    ["export_assets", ["assets", "folder", "format", "overwrite"]],
  ] as const) {
    for (const field of fields) {
      assert.match(schemaJson(name), new RegExp(`"${field}"`), `${name} should accept ${field}`);
    }
  }
  assert.match(tool("delete_assets").description, /refused_in_batch/);
  assert.match(tool("duplicate_asset").description, /World Partition/);
});

test("animation data tool schemas", () => {
  for (const [name, fields] of [
    ["sample_animation_bones", ["asset", "bones", "times", "start", "end", "step", "space", "root", "speed", "mesh", "slot"]],
    ["retarget_animations", ["retargeter", "assets", "folder", "recursive", "source_mesh", "target_mesh", "target_folder", "prefix", "suffix",
      "search", "replace", "existing", "include_referenced", "save"]],
  ] as const) {
    for (const field of fields) {
      assert.match(schemaJson(name), new RegExp(`"${field}"`), `${name} should accept ${field}`);
    }
  }
});

// The guide files the editor serves.

const NO_REPO_REFERENCES = [/docs\/architecture/, /E:\//, /C:\/Proj/];

test("no guide file points at repository documents or machine-specific paths", () => {
  const files = markdownFiles(SCHEMAS_DIR);
  assert.ok(files.length > 30, "the guide files are found");
  for (const file of files) {
    const text = readFileSync(file, "utf-8");
    for (const pattern of NO_REPO_REFERENCES) {
      assert.doesNotMatch(text, pattern, `${file} must not mention ${pattern}`);
    }
  }
});

function assertPatterns(text: string, patterns: RegExp[], what: string): void {
  for (const pattern of patterns) {
    assert.match(text, pattern, `${what} should include ${pattern}`);
  }
}

test("the core AssetDocument guide covers the document shape, diagnostics and the discovery tools", () => {
  const sectionNames = readdirSync(SECTIONS_DIR)
    .filter((file) => file.endsWith(".md"))
    .map((file) => file.slice(0, -3))
    .sort();
  // The editor appends "Sections: <names>." after the file, so each new profile section costs its name, not its class list.
  const listing = `\nSections: ${sectionNames.join(", ")}.`;
  const text = readText(SCHEMAS_DIR, "AssetDocument.md");
  assert.ok((text + listing).length <= 10_000, `the core guide stays small (${text.length + listing.length} chars with the section listing)`);
  assert.ok(listing.length <= 60 * sectionNames.length, `the listing costs at most 60 chars per section (${listing.length})`);
  assert.match(text, /^# AssetDocument Guide/m);
  assertPatterns(
    text,
    [
      /SchemaVersion/,
      /Target/,
      /Omitted Versus Empty/,
      /file watcher/i,
      /contentDir/,
      /severity/,
      /details/,
      /UpToDateWithWarnings/,
      /find_assets/,
      /list_types/,
      /extract_asset_document/,
      /diff_asset_document/,
      /class path/,
    ],
    "AssetDocument guide",
  );
  assert.doesNotMatch(text, /RowStruct.*Rows/, "profile Body shapes are not in the core guide");
  assert.doesNotMatch(text, /^# Level Editing Guide/m, "the level guide is not part of the AssetDocument guide");
});

// Per section file: the title line and what the section has to say.
const SECTION_PATTERNS: Record<string, RegExp[]> = {
  DataTable: [/^# DataTable Family Profiles/m, /RowStruct/, /list_types/, /CurveTableMode/],
  WidgetBlueprint: [/^# WidgetBlueprint Profile/m, /WidgetTree/, /Animations/],
  BehaviorTree: [/^# BehaviorTree Profile/m, /BehaviorTree\.BlackboardNotFound/],
  AnimMontage: [/SlotAnimTracks/, /SlotGroups/, /RateScale/],
  Generic: [/FactoryContext/, /Instanced subobjects/, /profile: "MaterialInstance"/, /^# Generic Reflected Profile/m],
  MaterialInstance: [/^# MaterialInstance Profile/m, /ParameterSchema/, /MaterialInstance\.UnknownParameter/, /LayerParameters/],
  Material: [/^# Material Profile/m, /Material\.CompileFailed/, /ParameterGroups/, /FunctionOutput/],
  Blueprint: [/^# Blueprint Profile/m, /K2:Branch/, /Graph\.ConnectNeedsConversion/],
  BlackboardData: [/^# BlackboardData Profile/m],
  PolicyMetadata: [/^# Profile Policy Metadata/m],
  AnimSequence: [/sample_animation_bones/, /RootDisplacement/, /PlayTime/, /PinBone/],
  IKRig: [/retarget_animations/, /"Type": "PinBone"/, /BonesToPin/, /ik_hand_gun/],
};

test("each profile section covers what an AI needs to author a document", () => {
  for (const [section, patterns] of Object.entries(SECTION_PATTERNS)) {
    const text = readText(SECTIONS_DIR, `${section}.md`);
    assertPatterns(text, patterns, `${section} section`);
  }
});

test("every section file has a title and a Classes line of class paths", () => {
  const files = readdirSync(SECTIONS_DIR).filter((file) => file.endsWith(".md"));
  assert.ok(files.length >= 20);
  // These two sections apply to classes without a section of their own, so their Classes line describes them in words.
  const DESCRIBED_IN_WORDS = new Set(["Generic.md", "PolicyMetadata.md"]);
  for (const file of files) {
    const text = readText(SECTIONS_DIR, file);
    assert.match(text, /^# .+/, `${file} starts with a title`);
    assert.ok(text.startsWith("# "), `${file} starts with its title line`);
    const classes = /^Classes: (.+)$/m.exec(text)?.[1];
    assert.ok(classes, `${file} has a Classes line`);
    if (DESCRIBED_IN_WORDS.has(file)) {
      continue;
    }
    const entries = classes.split(",").map((entry) => entry.trim());
    assert.ok(entries.length > 0);
    for (const entry of entries) {
      assert.match(entry, /^\/[A-Za-z0-9_]+\/[A-Za-z0-9_]+\.[A-Za-z0-9_]+$/, `${file}: '${entry}' is a class path`);
    }
  }
});

test("the classes of the sections found by class path or short name", () => {
  const classesOf = (section: string) =>
    (/^Classes: (.+)$/m.exec(readText(SECTIONS_DIR, `${section}.md`))?.[1] ?? "").split(",").map((entry) => entry.trim());
  assert.ok(classesOf("DataTable").includes("/Script/Engine.CurveTable"));
  assert.ok(classesOf("WidgetBlueprint").includes("/Script/UMGEditor.WidgetBlueprint"));
  assert.ok(classesOf("MaterialInstance").includes("/Script/Engine.MaterialInstanceConstant"));
  assert.ok(classesOf("Material").includes("/Script/Engine.MaterialFunction"));
  assert.ok(classesOf("Material").some((entry) => entry.endsWith(".MaterialFunctionMaterialLayerBlend")));
  assert.ok(classesOf("BehaviorTree").includes("/Script/AIModule.BehaviorTree"));
});

// Long sections keep what an AI needs to author a correct document and serve the reference material as parts
// (schemas/assetdocument/<Name>/<part>.md, read with {profile, part}).
const GUIDE_PARTS: Record<string, Record<string, { title: RegExp; patterns: RegExp[] }>> = {
  Niagara: {
    scripts: { title: /^# Niagara reference: Scripts and scratch pads/m, patterns: [/Niagara\.ExposedVersionEdit/, /Niagara\.UsageImmutable/, /Niagara:CustomHlsl/, /Niagara\.ScratchPadScope/, /ScratchPad:<name>/] },
    emitter_assets: { title: /^# Niagara reference: Emitter assets/m, patterns: [/Niagara\.StatelessEmitter/, /bIsInheritable/] },
    parameter_collections: { title: /^# Niagara reference: Parameter collections/m, patterns: [/Niagara\.NamespaceInUse/, /Niagara\.CollectionParameterOwned/] },
    stages_and_lightweight: { title: /^# Niagara reference: Stages/m, patterns: [/Niagara\.ModeChange/, /Simulation stages run on GPU emitters only/, /EmitterTemplate/] },
    compile_inspect: { title: /^# Niagara reference: Compile, failures, Inspect/m, patterns: [/Niagara\.CompileFailed/, /Niagara\.OpenInEditorUnsavedEdits/, /a failed Create leaves no asset/, /NiagaraDetails/, /UnsupportedProperties/] },
    update_example: { title: /^# Niagara reference: Update example/m, patterns: [/DirectionalBurst/, /User\.Turbulence/] },
  },
  Blueprint: {
    layout: { title: /^# Blueprint reference: Layout/m, patterns: [/"Layout": "Auto"/, /Graph\.NodeNotResizable/, /CommentBubbleVisible/] },
    find_assets_uses: { title: /^# Blueprint reference: find_assets uses/m, patterns: [/NotSearched/, /NodeGuid/, /Every term must match/] },
    operators_select_arrays: { title: /^# Blueprint reference: Operator, Select and array keys/m, patterns: [/K2:EqualEnum/, /Pin Conversions/, /Array_Length/, /Graph\.ActionAmbiguous/] },
    members: { title: /^# Blueprint reference: Types, variables, components, timelines/m, patterns: [/UBlueprint\.BlueprintTypeImmutable/, /BlueprintTypeRegionUnsupported/, /Change Variable Type/, /RootShakePattern/, /MoveTable/, /TickGroup/] },
    graph_details: { title: /^# Blueprint reference: Graph details/m, patterns: [/K2:Composite/, /Graph\.UserPinsUnsupported/, /Graph\.PrunedNodeValueRead/, /Graph\.ActionNotFound/] },
    known_limits: { title: /^# Blueprint reference: Known issues/m, patterns: [/Graph\.OrphanedPinDefault/, /Graph\.OrphanedPinLinked/, /Graph\.GraphOperationUnsupported/] },
    editor_effects: { title: /^# Blueprint reference: Dependents/m, patterns: [/Graph\.PieActive/, /GenericRuntime\.EditorDialogAnswered/, /Graph\.DefaultsPropagated/] },
  },
  MetaSound: {
    body_shape: { title: /^# MetaSound reference: Body shape/m, patterns: [/"Wave_Player"/, /"Comments"/] },
    presets: { title: /^# MetaSound reference: Presets/m, patterns: [/MetaSound\.InvalidPresetParent/, /MetaSound\.PresetGraphDerived/, /MSS_Blip_High/] },
    pages: { title: /^# MetaSound reference: Pages/m, patterns: [/MetaSound\.UnknownPage/, /PageDefaults/, /DefaultMetaSound\.ini/] },
    apply_details: { title: /^# MetaSound reference: Apply details/m, patterns: [/UnsupportedNodeClass/, /MetaSound\.InvalidConfiguration/, /MetaSound\.UpdateNodeNotSupported/, /MetaSound\.AudioDeviceRequired/, /MetaSound\.DependentsUpdated/] },
  },
  Material: {
    reference: { title: /^# Material reference: Engine materials/m, patterns: [/Material\.InteractiveEditRefused/, /Material\.OpenInEditorUnsavedEdits/, /Document\.DuplicateKey/, /Material\.ExpressionNotFound/] },
  },
};

test("sections with reference parts: the parts on disk are the parts this test knows", () => {
  const partDirectories = readdirSync(SECTIONS_DIR, { withFileTypes: true })
    .filter((entry) => entry.isDirectory())
    .map((entry) => entry.name)
    .sort();
  assert.deepEqual(partDirectories, Object.keys(GUIDE_PARTS).sort());
  for (const [profile, parts] of Object.entries(GUIDE_PARTS)) {
    assert.ok(existsSync(join(SECTIONS_DIR, `${profile}.md`)), `${profile} has its section file`);
    const onDisk = readdirSync(join(SECTIONS_DIR, profile))
      .filter((file) => file.endsWith(".md"))
      .map((file) => file.slice(0, -3))
      .sort();
    // A new part needs its assertions here.
    assert.deepEqual(onDisk, Object.keys(parts).sort(), `${profile} parts on disk`);
  }
});

test("each reference part has its title and content, and its text is moved out of the section, not copied", () => {
  for (const [profile, parts] of Object.entries(GUIDE_PARTS)) {
    const section = readText(SECTIONS_DIR, `${profile}.md`);
    for (const [part, expected] of Object.entries(parts)) {
      const file = readText(SECTIONS_DIR, profile, `${part}.md`);
      const title = /^# (.+)$/m.exec(file)?.[1] ?? "";
      assert.ok(title.length > 0 && file.startsWith(`# ${title}`), `${profile}/${part} starts with its title line`);
      assert.match(file, expected.title, `${profile}/${part} title`);
      assertPatterns(file, expected.patterns, `${profile}/${part}`);
      // The part's text is moved, not copied: no long line of it is in the section's authoring text.
      for (const line of file.split("\n").filter((candidate) => /^(- |[A-Za-z])/.test(candidate) && candidate.trim().length >= 30)) {
        assert.ok(!section.includes(line.trim()), `${profile}/${part}: "${line.slice(0, 50)}" is also in the section`);
      }
    }
  }
});

test("the level, playtest and import/settings guides", () => {
  const level = readText(SCHEMAS_DIR, "LevelEditing.md");
  assert.ok(level.length <= 12_000, `the level guide stays small (${level.length} chars)`);
  assertPatterns(
    level,
    [
      /^# Level Editing Guide/m,
      /query_level/,
      /PersistentLevel\./,
      /Level\.ActorRefAmbiguous/,
      /Level\.PlaySessionActive/,
      /include_unloaded/,
      /diff_only/,
      /EditDefaultsOnly/,
      /Undo/,
      /edit_level/,
      /dry_run/,
      /"@/,
      /Level\.DeleteReferenced/,
      /Level\.PropertyValueNotStoredExactly/,
      /rolled_back/,
      /saved_packages/,
      /add_component/,
      /attach/,
      /rename_folder/,
      /data_layers/,
      /Level\.AttachRejected/,
      /pin/,
      /undo_editor/,
      /Undo\.EntryMismatch/,
      /open_level/,
      /new_level/,
      /Level\.UnsavedChangesWouldBeLost/,
      /import_settings/,
    ],
    "level guide",
  );

  const playtest = readText(SCHEMAS_DIR, "Playtest.md");
  assert.ok(playtest.length <= 12_000, `the playtest guide stays small (${playtest.length} chars)`);
  assertPatterns(
    playtest,
    [
      /^# Playtest Guide/m,
      /query_play/,
      /get_viewport_screenshot/,
      /Play\.BlueprintCompileErrors/,
      /allow_blueprint_errors/,
      /Play\.NoSession/,
      /Level\.PlaySessionActive/,
      /input_action/,
      /"key"|`key`/,
      /duration/,
      /listeners/,
      /Play\.ActionNotMapped/,
      /until/,
      /Play\.WaitTimedOut/,
      /LogBlueprintUserMessages/,
      /Accessed None/,
      /last_seq/,
      /blackboard/,
      /montage/,
      /vitals/,
      /hud/,
      /### call and set/,
      /state: "down"/,
      /unpaused_real_time/,
      /what: "effects"/,
      /what: "audio"/,
      /Play\.AudioDisabled/,
      /max_width/,
      /"png"/,
      /Play\.FunctionNotCallable/,
      /ke <actor>/,
      /what: "input"/,
      /reset_input_settings/,
      /Play\.InputUserSettingsOverride/,
      /Play\.InputValueTypeMismatch/,
    ],
    "playtest guide",
  );

  const importSettings = readText(SCHEMAS_DIR, "ImportAndSettings.md");
  assert.ok(importSettings.length <= 8_000, `the import and settings guide stays small (${importSettings.length} chars)`);
  assertPatterns(
    importSettings,
    [
      /^# Import and Settings Guide/m,
      /import_assets/,
      /reimport/,
      /replace_existing/,
      /Import\.FormatNotSupported/,
      /"world"/,
      /Maps & Modes/,
      /DefaultEngine\.ini/,
      /undo_editor/,
      /Settings\.ConfigFileNotInProject/,
      /save_assets/,
      /all_dirty/,
      /reload_assets/,
      /Assets\.OpenMap/,
      /asset_editor/,
      /export_assets/,
      /Assets\.NoExporter/,
      /discard_unapplied/,
      /apply_editor/,
    ],
    "import and settings guide",
  );
});

// Guide size: an AI building a gameplay feature reads these guides in one session. Each budget is the measured size
// of the served text + 10% rounded up to the next 100; the core guide and the eight authoring sections together
// may not exceed 72,000 chars. Reference material lives in parts read on demand with {profile, part}.
test("the authoring guides stay within their size budgets", () => {
  const GUIDE_BUDGETS: Record<string, number> = {
    UserDefinedTypes: 5400,
    DataTable: 8300,
    Material: 8200,
    MaterialInstance: 4400,
    Niagara: 12700,
    MetaSound: 9600,
    Blueprint: 13000,
    WidgetBlueprint: 4600,
  };
  const CORE_BUDGET = 11_000;
  const TOTAL_BUDGET = 72_000;
  const core = readText(SCHEMAS_DIR, "AssetDocument.md");
  assert.ok(core.length <= CORE_BUDGET, `core guide ${core.length} chars exceeds ${CORE_BUDGET}`);
  let total = core.length;
  for (const [profile, budget] of Object.entries(GUIDE_BUDGETS)) {
    const text = readText(SECTIONS_DIR, `${profile}.md`);
    assert.ok(text.length <= budget, `${profile} section ${text.length} chars exceeds its budget ${budget}`);
    total += text.length;
  }
  assert.ok(total <= TOTAL_BUDGET, `the nine guides total ${total} chars, over ${TOTAL_BUDGET}`);
});
