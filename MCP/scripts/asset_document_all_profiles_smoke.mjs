#!/usr/bin/env node
// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const scriptDir = dirname(fileURLToPath(import.meta.url));
const mcpDir = resolve(scriptDir, "..");
const transportContract = JSON.parse(readFileSync(resolve(scriptDir, "../../Scripts/AssetDocument/verification/transport_contract_v1.json"), "utf8"));
const BUCKETS = ["changed", "added", "removed", "missing", "extra", "skipped", "failed", "errors"];
export class LayerValidationError extends Error {
  constructor(code, path, message) {
    super(`${code} at ${path}: ${message}`);
    this.code = code;
    this.path = path;
  }
}

function fail(path, message, code = "Layer.InvalidShape") {
  throw new LayerValidationError(code, path, message);
}

function sortedValue(value) {
  if (Array.isArray(value)) return value.map(sortedValue);
  if (value && typeof value === "object") {
    return Object.fromEntries(Object.keys(value).sort().map((key) => [key, sortedValue(value[key])]));
  }
  return value;
}

export function canonicalJson(value) {
  return `${JSON.stringify(sortedValue(value), null, 2)}\n`;
}

function closed(value, keys, path) {
  if (!value || typeof value !== "object" || Array.isArray(value)) fail(path, "expected object");
  const actual = Object.keys(value);
  const missing = keys.filter((key) => !actual.includes(key));
  const unknown = actual.filter((key) => !keys.includes(key));
  if (missing.length) fail(`${path}/${pointerName(missing.sort()[0])}`.replace(/^\/\//, "/"), "missing required key");
  if (unknown.length) fail(`${path}/${pointerName(unknown.sort()[0])}`.replace(/^\/\//, "/"), "unknown key");
  return value;
}

function pointerName(key) {
  const special = { evidence_kind: "EvidenceKind", run_id: "RunId", plugin_commit: "PluginCommit", manifest_digest: "ManifestDigest",
    service_before: "ServiceBefore", service_after: "ServiceAfter", strict_buckets: "StrictBuckets", raw_artifacts: "RawArtifacts",
    provenance_digest: "ProvenanceDigest", schema_version: "SchemaVersion", materialized_digests: "MaterializedDigests",
    http_status: "HttpStatus", diagnostic_code: "DiagnosticCode", diagnostic_path: "DiagnosticPath", request_artifact: "RequestArtifact",
    response_artifact: "ResponseArtifact", service_file: "ServiceFile", start_time: "StartTime", service_mtime: "ServiceMtime",
    capture_sha256: "CaptureSha256", sha256: "Sha256" };
  return special[key] || key.split("_").map((part) => part[0].toUpperCase() + part.slice(1)).join("");
}

function sha(value, path) {
  if (typeof value !== "string" || !/^[0-9a-f]{64}$/.test(value)) fail(path, "expected lowercase SHA-256");
}

function relativePath(value, path) {
  if (typeof value !== "string" || !value || value.includes("\\") || value.startsWith("/")
      || value.split("/").includes("..") || value.split("/").includes(".") || value.includes("//")) {
    fail(path, "expected canonical relative path");
  }
}

function buckets(value, path) {
  const keys = [...BUCKETS]; closed(value, keys, path);
  for (const key of keys) if (!Number.isInteger(value[key]) || value[key] < 0) fail(`${path}/${pointerName(key)}`, "expected non-negative integer");
}

function digest(value, path, raw = false) {
  const keys = raw ? ["path", "sha256", "size"] : ["kind", "path", "sha256", "size"];
  closed(value, keys, path);
  if (!raw && !["asset", "package", "sidecar", "sync_state", "report"].includes(value.kind)) fail(`${path}/Kind`, "unknown digest kind");
  relativePath(value.path, `${path}/Path`);
  sha(value.sha256, `${path}/Sha256`);
  if (!Number.isInteger(value.size) || value.size < 0) fail(`${path}/Size`, "size required");
}

function nonemptyOrNull(value, path) {
  if (value === null) return;
  if (typeof value !== "string" || !value) fail(path, "expected null or nonempty string");
}

function service(value, path, required) {
  if (value === null && !required) return;
  const keys = ["host", "port", "pid", "command", "service_file", "start_time", "service_mtime", "capture_sha256"];
  closed(value, keys, path);
  for (const key of ["host", "command", "service_file", "start_time"]) if (!value[key] || typeof value[key] !== "string") fail(`${path}/${pointerName(key)}`, "string required");
  for (const key of ["port", "pid"]) if (!Number.isInteger(value[key]) || value[key] <= 0) fail(`${path}/${pointerName(key)}`, "positive integer required");
  if (typeof value.service_mtime !== "number" || !Number.isFinite(value.service_mtime) || value.service_mtime <= 0) fail(`${path}/ServiceMtime`, "positive mtime required");
  sha(value.capture_sha256, `${path}/CaptureSha256`);
}

export function validateLayerEnvelope(value) {
  const top = ["schema_version", "evidence_kind", "layer", "status", "exit_code", "run_id", "plugin_commit", "manifest_digest", "phase", "service_before", "service_after", "documents", "strict_buckets", "raw_artifacts", "provenance_digest"];
  closed(value, top, "");
  if (value.schema_version !== 1) fail("/SchemaVersion", "schema version must be 1");
  if (!["contract_test", "manifest_contract", "foundation_contract", "production_provenance", "production_fixture_validation", "production_architecture", "production_automation", "production_failure_injection", "production_http", "production_mcp", "production_materialized_files"].includes(value.evidence_kind)) fail("/EvidenceKind", "unknown evidence kind");
  if (!["manifest_contract", "foundation_contract", "provenance", "fixture_validation", "architecture", "automation_focused", "automation_assetdoc_namespace", "automation_bare_namespace", "failure_injection", "http_phase_a", "mcp_phase_a", "http_phase_b_fresh", "materialized_files_phase_a", "materialized_files_phase_b"].includes(value.layer)) fail("/Layer", "unknown layer");
  if (!["none", "contract", "phase_a", "phase_b", "final"].includes(value.phase)) fail("/Phase", "unknown phase");
  const statusExit = { pass: 0, fail: 1, blocked: 3, incomplete: 4, restart_required: 4 };
  if (!(value.status in statusExit)) fail("/Status", "unknown status");
  if (value.exit_code !== statusExit[value.status]) fail("/ExitCode", "status/exit mismatch");
  if (!value.run_id || typeof value.run_id !== "string") fail("/RunId", "run id required");
  if (typeof value.plugin_commit !== "string" || !/^[0-9a-f]{40}$/.test(value.plugin_commit)) fail("/PluginCommit", "commit required");
  sha(value.manifest_digest, "/ManifestDigest"); sha(value.provenance_digest, "/ProvenanceDigest");
  const transport = ["http_phase_a", "mcp_phase_a", "http_phase_b_fresh"].includes(value.layer);
  service(value.service_before, "/ServiceBefore", transport); service(value.service_after, "/ServiceAfter", transport);
  if (!Array.isArray(value.documents)) fail("/Documents", "array required");
  value.documents.forEach((document, index) => validateDocument(document, index));
  const ids = value.documents.map((row) => row.id); if (new Set(ids).size !== ids.length || ids.join() !== [...ids].sort().join()) fail("/Documents", "unique sorted IDs required");
  buckets(value.strict_buckets, "/StrictBuckets");
  if (!Array.isArray(value.raw_artifacts)) fail("/RawArtifacts", "array required");
  value.raw_artifacts.forEach((row, index) => digest(row, `/RawArtifacts/${index}`, true));
  if (value.raw_artifacts.map((row) => row.path).join() !== [...value.raw_artifacts].sort((a, b) => a.path.localeCompare(b.path)).map((row) => row.path).join()) fail("/RawArtifacts", "artifacts are not sorted");
  if (new Set(value.raw_artifacts.map((row) => row.path)).size !== value.raw_artifacts.length) fail("/RawArtifacts", "artifacts must be unique");
  return value;
}

function validateDocument(document, index) {
  const path = `/Documents/${index}`;
  closed(document, ["id", "status", "operations", "outcomes", "strict_buckets", "materialized_digests"], path);
  if (typeof document.id !== "string" || !document.id) fail(`${path}/Id`, "nonempty ID required");
  if (!["pass", "fail", "blocked", "incomplete", "restart_required"].includes(document.status)) fail(`${path}/Status`, "unknown status");
  if (!Array.isArray(document.operations) || !Array.isArray(document.outcomes) || document.operations.length !== document.outcomes.length) fail(`${path}/Operations`, "operation/outcome mismatch");
  const operations = ["schema", "inspect", "profile", "template", "validate_inline", "validate_file", "apply_file", "extract", "diff_inline", "diff_file"];
  document.operations.forEach((operation, offset) => {
    if (!operations.includes(operation)) fail(`${path}/Operations/${offset}`, "unknown operation");
    const outcome = document.outcomes[offset]; closed(outcome, ["operation", "status", "http_status", "diagnostic_code", "diagnostic_path", "request_artifact", "response_artifact"], `${path}/Outcomes/${offset}`);
    if (outcome.operation !== operation) fail(`${path}/Outcomes/${offset}/Operation`, "operation mismatch");
    if (!["pass", "fail", "blocked", "incomplete", "restart_required"].includes(outcome.status)) fail(`${path}/Outcomes/${offset}/Status`, "unknown status");
    if (outcome.http_status !== null && (!Number.isInteger(outcome.http_status) || outcome.http_status < 100 || outcome.http_status > 599)) fail(`${path}/Outcomes/${offset}/HttpStatus`, "invalid HTTP status");
    for (const key of ["diagnostic_code", "diagnostic_path", "request_artifact", "response_artifact"]) nonemptyOrNull(outcome[key], `${path}/Outcomes/${offset}/${pointerName(key)}`);
  });
  buckets(document.strict_buckets, `${path}/StrictBuckets`);
  if (!Array.isArray(document.materialized_digests)) fail(`${path}/MaterializedDigests`, "array required");
  document.materialized_digests.forEach((row, offset) => digest(row, `${path}/MaterializedDigests/${offset}`));
  const sorted = [...document.materialized_digests].sort((a, b) => a.path.localeCompare(b.path) || a.kind.localeCompare(b.kind));
  if (JSON.stringify(document.materialized_digests) !== JSON.stringify(sorted)) fail(`${path}/MaterializedDigests`, "digests are not sorted");
  if (new Set(document.materialized_digests.map((row) => `${row.path}\u0000${row.kind}`)).size !== document.materialized_digests.length) fail(`${path}/MaterializedDigests`, "digests must be unique");
}

function readJson(path) { return JSON.parse(readFileSync(path, "utf8")); }
function writeJson(path, value) { writeFileSync(path, canonicalJson(value), "utf8"); }

export function canonicalMcpArguments(document, operation) {
  const row = transportContract.operations[operation];
  if (!row?.mcp_tool || !row.mcp_arguments || typeof row.mcp_arguments !== "object" || Array.isArray(row.mcp_arguments)) {
    throw new Error(`operation ${operation} has no production MCP argument projection`);
  }
  return Object.fromEntries(Object.entries(row.mcp_arguments).map(([key, descriptor]) => {
    const descriptorKeys = descriptor && typeof descriptor === "object" && !Array.isArray(descriptor)
      ? Object.keys(descriptor).sort() : [];
    let value;
    if (descriptor?.source === "field" && JSON.stringify(descriptorKeys) === JSON.stringify(["field", "source"])) {
      value = document[descriptor.field];
    } else if (descriptor?.source === "fixture_json" && JSON.stringify(descriptorKeys) === JSON.stringify(["source"])) {
      value = readJson(document.fixture);
    } else if (descriptor?.source === "literal" && JSON.stringify(descriptorKeys) === JSON.stringify(["source", "value"])) {
      value = descriptor.value;
    } else {
      throw new Error(`invalid MCP projection for ${operation}.${key}`);
    }
    if (value === null || value === undefined || value === "") throw new Error(`empty MCP projection for ${operation}.${key}`);
    return [key, value];
  }));
}

export function buildPhaseACallPlan(manifest, phase = "phase-a") {
  if (phase !== "phase-a") throw new Error("only phase-a is supported by the MCP driver");
  if (manifest.phase === "phase-b") throw new Error("Phase-B manifest cannot enter Phase-A driver");
  const documents = new Map((manifest.documents || []).map((row) => [row.id, row]));
  const order = [...(manifest.apply_order || []), ...[...documents.keys()].filter((id) => !(manifest.apply_order || []).includes(id)).sort()];
  const calls = [{ ordinal: 1, document_id: null, operation: "health", tool: "health_check", arguments: {} }];
  for (const id of order) {
    const row = documents.get(id);
    if (row.expectations?.transport?.[transportContract.transport_flags.mcp] !== true) continue;
    for (const operation of row.operations || []) {
      if (operation === "schema") {
        if (row.expectations?.transport?.require_mcp) throw new Error("schema is HTTP-only and cannot require MCP");
        continue;
      }
      const tool = transportContract.operations[operation]?.mcp_tool;
      if (!tool) throw new Error(`unknown or non-MCP operation ${operation}`);
      calls.push({ ordinal: calls.length + 1, document_id: id, operation, tool,
        arguments: canonicalMcpArguments(row, operation),
        response_expectations: row.expectations?.transport || {} });
    }
  }
  return calls;
}

export function assertCleanResponse(response, label, expectations = {}, operation = null) {
  if (!response || response.success !== true) throw new Error(`${label} success was not true`);
  if (Array.isArray(response.diagnostics) && response.diagnostics.length) throw new Error(`${label} diagnostics were nonempty`);
  const payload = response.payload && typeof response.payload === "object" ? response.payload : {};
  for (const [containerName, container] of [["response", response], ["payload", payload]]) {
    for (const bucket of BUCKETS) if ((Array.isArray(container[bucket]) && container[bucket].length) || (!Array.isArray(container[bucket]) && container[bucket])) throw new Error(`${label} ${containerName}.${bucket} was nonempty`);
    if (container.sidecar_sync_update_skipped || container.sidecar_sync_update_skip_reason) throw new Error(`${label} sync skip`);
  }
  for (const [expectation, row] of Object.entries(transportContract.expectation_claims)) {
    if (expectations[expectation] === true && row.operations.includes(operation)
      && (response[row.claim] ?? payload[row.claim]) !== true) throw new Error(`${label} ${row.claim} was not true`);
  }
  return payload;
}

export function assertHealthPort(health, baseUrl) {
  const expected = new URL(baseUrl);
  const port = Number(expected.port || (expected.protocol === "https:" ? 443 : 80));
  if (health?.status !== "ok" || health?.service !== "UEAssetDoc" || health?.port !== port) throw new Error("health status/service/port mismatch");
  return { status: health.status, service: health.service, port: health.port };
}

export function reduceMcpTranscript(calls, options = {}) {
  const failed = calls.filter((call) => call.status !== "pass");
  const real = options.realTransport === true && options.serviceBefore && options.serviceAfter;
  const result = { schema_version: 1, evidence_kind: real ? "mcp_transport_transcript_v1" : "contract_test",
    status: failed.length ? "fail" : (real ? "pass" : "incomplete"), exit_code: failed.length ? 1 : (real ? 0 : 4), calls };
  if (real) result.service_before = options.serviceBefore;
  return result;
}

function parseArgs(argv) {
  const options = {};
  for (let index = 0; index < argv.length; index += 1) {
    const key = argv[index];
    if (key === "--help" || key === "-h") options.help = true;
    else if (["--manifest", "--evidence-dir", "--base-url", "--service-before", "--phase"].includes(key)) options[key.slice(2).replaceAll("-", "_")] = argv[++index];
    else throw new Error(`unknown argument ${key}`);
  }
  if (!options.help && (!options.manifest || !options.evidence_dir || !options.base_url || !options.service_before || options.phase !== "phase-a")) throw new Error("--manifest --evidence-dir --base-url --service-before --phase phase-a are required");
  return options;
}

function textResult(result) {
  return (result.content || []).filter((row) => row?.type === "text" && typeof row.text === "string").map((row) => row.text).join("\n");
}

async function main() {
  let options;
  try { options = parseArgs(process.argv.slice(2)); } catch (error) { console.error(error.message); return 2; }
  if (options.help) { console.log("AssetDocument production MCP phase-A driver"); return 0; }
  const evidenceDir = resolve(options.evidence_dir);
  try { mkdirSync(evidenceDir, { recursive: false }); } catch (error) { console.error(error.message); return 2; }
  const manifest = readJson(resolve(options.manifest));
  const serviceBefore = readJson(resolve(options.service_before));
  const transport = new StdioClientTransport({ command: "node", args: [join(mcpDir, "dist", "index.js")], cwd: mcpDir,
    env: { ...process.env, UE_API_BASE: options.base_url.replace(/\/$/, "") }, stderr: "pipe" });
  const client = new Client({ name: "assetdocument-all-profiles-verifier", version: "1.0.0" });
  const stderr = []; const calls = [];
  try {
    if (transport.stderr?.on) transport.stderr.on("data", (chunk) => stderr.push(String(chunk)));
    await client.connect(transport);
    for (const call of buildPhaseACallPlan(manifest, options.phase)) {
      const prefix = `${String(call.ordinal).padStart(3, "0")}_${call.document_id || "service"}_${call.operation}`;
      writeJson(join(evidenceDir, `${prefix}.request.json`), { tool: call.tool, arguments: call.arguments });
      const result = await client.callTool({ name: call.tool, arguments: call.arguments });
      const parsed = JSON.parse(textResult(result));
      writeJson(join(evidenceDir, `${prefix}.response.json`), { isError: result.isError === true, body: parsed });
      if (result.isError === true) throw new Error(`${call.tool} returned isError`);
      if (call.tool === "health_check") assertHealthPort(parsed, options.base_url); else assertCleanResponse(parsed, call.tool, call.response_expectations, call.operation);
      const { response_expectations: _, ...recorded } = call;
      calls.push({ ...recorded, status: "pass" });
    }
    const transcript = reduceMcpTranscript(calls, { realTransport: true, serviceBefore, serviceAfter: serviceBefore });
    writeJson(join(evidenceDir, "mcp_transport_transcript_v1.json"), transcript);
    return transcript.exit_code;
  } catch (error) {
    writeJson(join(evidenceDir, "mcp_transport_transcript_v1.json"), { schema_version: 1, evidence_kind: "mcp_transport_transcript_v1", status: "fail", exit_code: 1, error: String(error), calls });
    return /health|identity|port/i.test(String(error)) ? 3 : (/unavailable|required route/i.test(String(error)) ? 4 : 1);
  } finally {
    writeFileSync(join(evidenceDir, "broker.stderr.log"), stderr.join(""), "utf8");
    await client.close().catch(() => {});
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  process.exitCode = await main();
}
