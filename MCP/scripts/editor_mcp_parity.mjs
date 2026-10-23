#!/usr/bin/env node
// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Parity check of the editor's MCP endpoint against a reference MCP server (e.g. an earlier Node broker that served the
// same tools over stdio from the editor's HTTP routes): the same tools/list and the same tools/call results, call by
// call, for every tool. Also checks that the stdio bridge (dist/index.js) passes the editor's results through unchanged.
//
//   node scripts/editor_mcp_parity.mjs --reference <path/to/reference/dist/index.js> [--editor http://127.0.0.1:8559]
//     [--token <bearer token>] [--report <file.json>]
//
// Needs a running editor with the UEAssetDoc plugin. The calls only read, or are refused, or change nothing
// (dry runs, a document applied unchanged, a play session started and stopped), so they can run on any project.
// Values that differ from one call to the next (Undo entry ids, timings, image bytes) are masked before comparing.

import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { isDeepStrictEqual } from "node:util";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";

const scriptDir = dirname(fileURLToPath(import.meta.url));
const bridgeEntry = resolve(scriptDir, "..", "dist", "index.js");

function option(name, fallback) {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : fallback;
}

const reference = option("--reference");
const editor = (option("--editor", "http://127.0.0.1:8559")).replace(/\/+$/, "");
const token = option("--token", process.env.UE_ASSETDOC_TOKEN ?? "");
const reportPath = option("--report");
if (!reference) {
  console.error("usage: editor_mcp_parity.mjs --reference <reference dist/index.js> [--editor <base url>] [--token <token>] [--report <file>]");
  process.exit(2);
}

const childEnv = Object.fromEntries(Object.entries(process.env).filter((entry) => typeof entry[1] === "string"));

async function stdioClient(entry, extraEnv) {
  const transport = new StdioClientTransport({ command: process.execPath, args: [entry], env: { ...childEnv, ...extraEnv }, stderr: "pipe" });
  const client = new Client({ name: "parity-check", version: "1.0" }, { capabilities: {} });
  await client.connect(transport);
  return client;
}

async function httpClient() {
  const transport = new StreamableHTTPClientTransport(new URL(`${editor}/mcp`), token ? { requestInit: { headers: { Authorization: `Bearer ${token}` } } } : {});
  const client = new Client({ name: "parity-check", version: "1.0" }, { capabilities: {} });
  await client.connect(transport);
  return client;
}

// Values that legitimately differ between two runs of the same call.
const VOLATILE_KEYS = new Set(["undo_id", "id", "startTime", "elapsed_ms", "duration_ms", "bytes", "data", "world_time", "real_time",
  "unpaused_real_time", "final_world_time", "played_real_seconds", "played_unpaused_real_seconds", "waited_game_seconds",
  "waited_real_seconds", "seq", "session_start_seq", "last_seq"]);

function mask(value) {
  if (Array.isArray(value)) return value.map(mask);
  if (value && typeof value === "object") {
    return Object.fromEntries(Object.entries(value).map(([key, entry]) => [key, VOLATILE_KEYS.has(key) && typeof entry !== "object" ? "<volatile>" : mask(entry)]));
  }
  if (typeof value === "string") {
    return value
      .replace(/\b[0-9A-Fa-f]{32}\b/g, "<guid>")
      .replace(/\b[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\b/g, "<guid>")
      .replace(/\d+\.\d KB/g, "<kb> KB")
      .replace(/world time [\d.e+-]+ s/g, "world time <t> s")
      .replace(/Advanced [\d.]+ game seconds/g, "Advanced <t> game seconds");
  }
  return value;
}

// A result as compared: text blocks parsed as JSON when they are JSON (then masked), images without their bytes.
function comparable(result) {
  return {
    isError: result.isError === true,
    content: (result.content ?? []).map((block) => {
      if (block.type === "image") return { type: "image", mimeType: block.mimeType };
      if (block.type !== "text") return block;
      try {
        return { type: "text", json: mask(JSON.parse(block.text)) };
      } catch {
        const lines = block.text.split("\n");
        // Failure texts end with the full JSON: compare it parsed, so masked values do not fail the comparison.
        const at = lines.indexOf("Full response JSON:");
        if (at >= 0 && at + 1 < lines.length) {
          try {
            return { type: "text", text: mask(lines.slice(0, at + 1).join("\n")), json: mask(JSON.parse(lines.slice(at + 1).join("\n"))) };
          } catch {
            // fall through
          }
        }
        return { type: "text", text: mask(block.text) };
      }
    }),
  };
}

async function main() {
  const work = mkdtempSync(join(tmpdir(), "ue-assetdoc-parity-"));
  const referenceClient = await stdioClient(reference, { UE_API_BASE: editor });
  const editorClient = await httpClient();
  const bridgeClient = await stdioClient(bridgeEntry, { UE_API_BASE: editor, ...(token ? { UE_ASSETDOC_TOKEN: token } : {}) });
  const report = { editor, reference, tools: {}, calls: [] };
  let failures = 0;

  try {
    const [referenceTools, editorTools, bridgeTools] = await Promise.all([referenceClient, editorClient, bridgeClient].map((client) => client.listTools()));
    const toolsEqual = isDeepStrictEqual(referenceTools.tools, editorTools.tools);
    const bridgeEqual = isDeepStrictEqual(editorTools.tools, bridgeTools.tools);
    report.tools = { count: editorTools.tools.length, equal: toolsEqual, bridgeEqual };
    console.log(`tools/list: ${editorTools.tools.length} tools, reference ${toolsEqual ? "equal" : "DIFFERENT"}, bridge ${bridgeEqual ? "equal" : "DIFFERENT"}`);
    if (!toolsEqual || !bridgeEqual) failures++;

    const call = (client, name, args) => client.callTool({ name, arguments: args }, undefined, { timeout: 300_000 });
    const cases = [];
    const health = JSON.parse((await call(editorClient, "health_check", {})).content[0].text);
    const contentDir = health.contentDir;

    // A project asset (preferably a Blueprint) to validate, diff and apply unchanged.
    const findFirst = async (args) => JSON.parse((await call(editorClient, "find_assets", { path: "/Game", limit: 1, ...args })).content[0].text).payload?.Assets?.[0]?.ObjectPath;
    const assetPath = ((await findFirst({ class: "/Script/Engine.Blueprint" })) ?? (await findFirst({})))?.split(".")[0];
    const extracted = assetPath ? await call(editorClient, "extract_asset_document", { asset_path: assetPath }) : undefined;
    const document = extracted && !extracted.isError ? JSON.parse(extracted.content[0].text).payload : undefined;
    const sidecar = assetPath ? join(contentDir, `${assetPath.slice("/Game/".length)}.assetdoc.json`) : undefined;
    if (assetPath) {
      cases.push(["extract_asset_document", { asset_path: assetPath }], ["extract_asset_document", { asset_path: assetPath, diff_only: false }]);
    }

    cases.unshift(
      ["get_asset_document_schema", {}],
      ["get_asset_document_schema", { profile: "Blueprint" }],
      ["get_asset_document_schema", { profile: "Niagara", part: "scripts" }],
      ["get_asset_document_schema", { profile: "DataTable", part: "layout" }],
      ["get_asset_document_schema", { part: "layout" }],
      ["get_asset_document_schema", { profile: "NoSuchProfile" }],
      ["get_asset_document_schema", { profile: "/Script/MyGame.MyDataAsset" }],
      ["get_asset_document_schema", { profile: "all" }],
      ["find_assets", { path: "/Game", limit: 5 }],
      ["find_assets", { name: "*Trigger Any*", limit: 5 }],
      ["find_assets", { uses: ["K2:Branch"], limit: 3 }],
      ["list_types", { kind: "class", name: "Character*", limit: 5 }],
      ["list_types", {}],
      ["list_types", { kind: "struct", base: "/Script/Engine.TableRowBase", limit: 3 }],
      ["inspect_asset_document_target", { class_or_asset: "/Script/Engine.CurveFloat" }],
      ["inspect_asset_document_target", { class_or_asset: "/Script/Engine.CurveFloat", detail: true }],
      ["inspect_asset_document_target", { class_or_asset: "/Game/NoSuch/Asset" }],
      ["inspect_asset_document_profile", { class_or_asset: "/Script/AIModule.BehaviorTree" }],
      ["inspect_asset_document_profile", {}],
      ["create_asset_document_template", { class: "/Script/AIModule.BlackboardData", target: "/Game/ParityCheck/BB_Template" }],
      ["create_asset_document_template", { class: "/Script/AIModule.BlackboardData" }],
      ["extract_asset_document", { asset_path: "/Engine/EngineMaterials/WorldGridMaterial" }],
      ["extract_asset_document", { asset_path: "/Game/NoSuch/Asset" }],
      ["validate_asset_document", {}],
      ["diff_asset_document", { document: { SchemaVersion: 1 }, file_path: "/tmp/x.assetdoc.json" }],
      ["validate_asset_document", { document: { SchemaVersion: 1, Class: "/Script/AIModule.BlackboardData", Target: "/Game/ParityCheck/BB_X", Properties: { NoSuchProperty: 1 } } }],
      ["apply_asset_document_file", { file_path: join(work, "missing.assetdoc.json") }],
      ["duplicate_asset", { source: "/Engine/EngineMaterials/WorldGridMaterial", target: "/Engine/Taken" }],
      ["duplicate_asset", { source: "/Engine/EngineMaterials/WorldGridMaterial" }],
      ["sample_animation_bones", { asset: "/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle", bones: ["root", "no_such_bone"], times: [0, 0.5] }],
      ["sample_animation_bones", { asset: "/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle", bones: ["root"], times: [0.25], speed: true }],
      ["retarget_animations", { retargeter: "/Game/NoSuch/RTG" }],
      ["health_check", {}],
      ["get_editor_context", {}],
      ["query_level", { what: "world" }],
      ["query_level", { limit: 3 }],
      ["edit_level", { description: "parity dry run", dry_run: true, ops: [{ op: "spawn", class: "/Script/Engine.PointLight", id: "lamp" }] }],
      ["edit_level", { ops: [{ op: "no_such_op" }] }],
      ["undo_editor", { expect_title: "no such entry" }],
      ["open_level", { path: "/Game/NoSuch/Map" }],
      ["new_level", { path: "/Engine/Maps/Entry" }],
      ["get_level_editing_guide", {}],
      ["get_level_editing_guide", { topic: "playtest" }],
      ["get_level_editing_guide", { topic: "import_settings" }],
      ["play", { action: "status" }],
      ["query_play", { what: "actors" }],
      ["execute_python", { code: "print(1 + 1)", description: "parity check" }],
      ["execute_python", { code: "raise ValueError('parity')" }],
      ["get_viewport_screenshot", { view: "editor", max_width: 64 }],
      ["delete_assets", { assets: ["/Game/NoSuch/Asset"] }],
      ["delete_assets", {}],
      ["import_assets", { action: "options", files: [join(work, "missing.fbx")] }],
      ["settings", { action: "list" }],
      ["settings", { section: "world" }],
      ["save_assets", { assets: ["/Game/NoSuch/Asset"] }],
      ["reload_assets", { assets: ["/Game/NoSuch/Asset"] }],
      ["asset_editor", { action: "list" }],
      ["export_assets", { assets: ["/Game/NoSuch/Asset"] }],
      ["no_such_tool", {}],
    );
    if (document && sidecar) {
      cases.push(["validate_asset_document", { document }], ["diff_asset_document", { document }]);
    }

    const run = async (name, args, prepare) => {
      if (prepare) prepare();
      const referenceResult = await call(referenceClient, name, args);
      if (prepare) prepare();
      const editorResult = await call(editorClient, name, args);
      if (prepare) prepare();
      const bridgeResult = await call(bridgeClient, name, args);
      const equal = isDeepStrictEqual(comparable(referenceResult), comparable(editorResult));
      const exact = isDeepStrictEqual(referenceResult, editorResult);
      const bridgeEqual = isDeepStrictEqual(comparable(editorResult), comparable(bridgeResult));
      report.calls.push({ name, args: JSON.stringify(args).slice(0, 200), equal, exact, bridgeEqual, isError: editorResult.isError === true,
        ...(equal ? {} : { reference: referenceResult, editor: editorResult }) });
      console.log(`${equal && bridgeEqual ? "ok  " : "DIFF"} ${exact ? "exact " : "masked"} ${name} ${JSON.stringify(args).slice(0, 100)}${editorResult.isError ? " (tool error)" : ""}`);
      if (!equal || !bridgeEqual) failures++;
    };

    for (const [name, args] of cases) {
      await run(name, args);
    }
    if (document && sidecar) {
      // A sidecar of the asset as extracted: applying it changes nothing, through every path.
      await run("apply_asset_document_file", { file_path: sidecar, save_asset: false }, () => writeFileSync(sidecar, JSON.stringify(document, null, 2)));
      rmSync(sidecar, { force: true });
    }
    // A pending play operation (start, wait, stop) is polled to its end: the sequence runs on each path in turn.
    const sequence = [{ action: "start", paused: true }, { action: "wait", seconds: 0.2 }, { action: "stop" }];
    const referenceResults = [];
    for (const action of sequence) referenceResults.push(await call(referenceClient, "play", action));
    for (const [index, action] of sequence.entries()) {
      const editorResult = await call(editorClient, "play", action);
      const equal = isDeepStrictEqual(comparable(referenceResults[index]), comparable(editorResult));
      const exact = isDeepStrictEqual(referenceResults[index], editorResult);
      report.calls.push({ name: "play", args: JSON.stringify(action), equal, exact, isError: editorResult.isError === true,
        ...(equal ? {} : { reference: referenceResults[index], editor: editorResult }) });
      console.log(`${equal ? "ok  " : "DIFF"} ${exact ? "exact " : "masked"} play ${JSON.stringify(action)}${editorResult.isError ? " (tool error)" : ""}`);
      if (!equal) failures++;
    }
  } finally {
    await Promise.all([referenceClient, editorClient, bridgeClient].map((client) => client.close().catch(() => undefined)));
    rmSync(work, { recursive: true, force: true });
  }

  const compared = report.calls.length;
  const exact = report.calls.filter((entry) => entry.exact).length;
  console.log(`${compared} calls compared, ${exact} byte-identical, ${failures} difference(s)`);
  if (reportPath) writeFileSync(reportPath, JSON.stringify(report, null, 2));
  process.exit(failures === 0 ? 0 : 1);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
