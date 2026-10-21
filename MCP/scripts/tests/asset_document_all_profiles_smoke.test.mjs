import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

import {
  LayerValidationError,
  assertCleanResponse,
  assertHealthPort,
  buildPhaseACallPlan,
  canonicalMcpArguments,
  canonicalJson,
  reduceMcpTranscript,
  validateLayerEnvelope,
} from "../asset_document_all_profiles_smoke.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const goldenPath = resolve(here, "../../../Scripts/AssetDocument/tests/fixtures/layer_envelope_v1.json");
const mutationPath = resolve(here, "../../../Scripts/AssetDocument/verification/layer_envelope_mutations_v1.json");

function applyMutation(value, mutation) {
  if ("seed" in mutation) {
    let seedOwner = value;
    for (const key of mutation.seed_target.slice(0, -1)) seedOwner = seedOwner[key];
    seedOwner[mutation.seed_target.at(-1)] = structuredClone(mutation.seed);
  }
  let owner = value;
  for (const key of mutation.target.slice(0, -1)) owner = owner[key];
  const final = mutation.target.at(-1);
  if (mutation.op === "delete") delete owner[final];
  else if (mutation.op === "append") owner[final].push(structuredClone(mutation.value));
  else if (mutation.op === "duplicate") owner[final].push(structuredClone(owner[final][mutation.index || 0]));
  else owner[final] = structuredClone(mutation.value);
}

function mutationCases(table) {
  assert.deepEqual(Object.keys(table).sort(), ["closed_objects", "mutations", "schema_version"]);
  assert.equal(table.schema_version, 1);
  const cases = [...table.mutations];
  for (const group of table.closed_objects) {
    for (const field of group.fields) {
      const row = { id: `missing_${group.id}_${field.key}`, op: "delete",
        target: [...group.target, field.key], code: "Layer.InvalidShape", path: field.path };
      if ("seed" in group) Object.assign(row, { seed_target: group.target, seed: group.seed });
      cases.push(row);
    }
    const unknown = { id: `unknown_${group.id}`, op: "set", target: [...group.target, "unknown"],
      value: true, code: "Layer.InvalidShape", path: group.unknown_path };
    if ("seed" in group) Object.assign(unknown, { seed_target: group.target, seed: group.seed });
    cases.push(unknown);
  }
  assert.ok(cases.length >= 90);
  assert.equal(new Set(cases.map((row) => row.id)).size, cases.length);
  return cases;
}

function manifest() {
  return {
    apply_order: ["asset"],
    documents: [{
      id: "asset", exact_class: "/Script/Engine.DataAsset", target: "/Game/A", object_path: "/Game/A.A",
      fixture: goldenPath,
      operations: ["inspect", "profile", "template", "validate_inline", "validate_file", "apply_file", "extract", "diff_inline", "diff_file"],
      expectations: { transport: { require_mcp: true }, freshness: { post_restart_operations: ["extract", "diff_file"] } },
    }],
  };
}

function cleanResponse() {
  return { success: true, diagnostics: [], changed: [], added: [], removed: [], missing: [], extra: [], skipped: [], failed: [], errors: [] };
}

test("builds phase-a calls only from existing AssetDocument MCP tools", () => {
  const calls = buildPhaseACallPlan(manifest(), "phase-a");
  assert.deepEqual(new Set(calls.map((call) => call.tool)), new Set([
    "health_check", "inspect_asset_document_target", "inspect_asset_document_profile", "create_asset_document_template",
    "validate_asset_document", "apply_asset_document_file", "extract_asset_document", "diff_asset_document",
  ]));
  assert.equal(calls.some((call) => /execute|new_object|primitive/.test(call.tool)), false);
  const row = manifest().documents[0];
  const fixture = JSON.parse(readFileSync(goldenPath, "utf8"));
  const expected = {
    inspect: { class_or_asset: row.object_path }, profile: { class_or_asset: row.object_path },
    template: { class: row.exact_class, target: row.target }, validate_inline: { document: fixture },
    validate_file: { file_path: goldenPath },
    apply_file: { file_path: goldenPath, save_asset: true },
    extract: { asset_path: row.target, diff_only: false, include_all_writable: true },
    diff_inline: { document: fixture }, diff_file: { file_path: goldenPath },
  };
  for (const [operation, argumentsValue] of Object.entries(expected)) {
    assert.deepEqual(canonicalMcpArguments(row, operation), argumentsValue);
    assert.deepEqual(calls.find((call) => call.operation === operation).arguments, argumentsValue);
  }
});

test("keeps every document result separate and preserves apply order", () => {
  const value = manifest();
  value.documents.push({ ...value.documents[0], id: "second", target: "/Game/B", object_path: "/Game/B.B" });
  value.documents.push({ ...value.documents[0], id: "http-only", expectations: { transport: { require_mcp: false } } });
  value.apply_order = ["second", "asset"];
  const calls = buildPhaseACallPlan(value, "phase-a").filter((call) => call.document_id);
  assert.equal(calls[0].document_id, "second");
  assert.equal(calls.some((call) => call.document_id === "http-only"), false);
  assert.deepEqual(calls.map((call) => call.ordinal), [...calls.keys()].map((index) => index + 2));
});

test("rejects phase-b or any apply after restart", () => {
  assert.throws(() => buildPhaseACallPlan(manifest(), "phase-b"), /phase-a/);
  const value = manifest();
  value.phase = "phase-b";
  assert.throws(() => buildPhaseACallPlan(value, "phase-a"), /Phase-B/);
});

test("rejects nonclean response buckets diagnostics and sync skip", () => {
  for (const bucket of ["changed", "added", "removed", "missing", "extra", "skipped", "failed", "errors"]) {
    const value = cleanResponse(); value[bucket] = ["x"];
    assert.throws(() => assertCleanResponse(value, bucket), new RegExp(bucket));
  }
  for (const mutation of [(value) => value.diagnostics.push("x"), (value) => { value.sidecar_sync_update_skipped = true; }]) {
    const value = cleanResponse(); mutation(value); assert.throws(() => assertCleanResponse(value, "response"));
  }
  assert.throws(() => assertCleanResponse({}, "response"), /success/);
  assert.throws(() => assertCleanResponse({ ...cleanResponse(), saved_asset: false }, "response",
    { require_saved_asset: true }, "apply_file"), /saved_asset/);
  assert.throws(() => assertCleanResponse({ ...cleanResponse(), wrote_sidecar: false }, "response",
    { require_sidecar_write: true }, "apply_file"), /wrote_sidecar/);
  assert.doesNotThrow(() => assertCleanResponse(cleanResponse(), "inspect",
    { require_saved_asset: true, require_sidecar_write: true }, "inspect"));
});

test("health proves status and port but never pid or start time", () => {
  const accepted = assertHealthPort({ status: "ok", service: "UEAssetDoc", port: 8562, pid: 999, start_time: "fake" }, "http://127.0.0.1:8562");
  assert.deepEqual(accepted, { status: "ok", service: "UEAssetDoc", port: 8562 });
  assert.throws(() => assertHealthPort({ status: "bad", service: "UEAssetDoc", port: 8562 }, "http://127.0.0.1:8562"));
  assert.throws(() => assertHealthPort({ status: "ok", service: "UEAssetDoc", port: 9999 }, "http://127.0.0.1:8562"));
});

test("contract-test result is never labeled live or end-to-end", () => {
  const reduced = reduceMcpTranscript([{ document_id: "asset", status: "pass" }], { realTransport: false });
  assert.equal(reduced.evidence_kind, "contract_test");
  assert.equal(reduced.exit_code, 4);
  assert.equal(reduced.status, "incomplete");
  const service = { host: "127.0.0.1", port: 8562, pid: 10, command: "UnrealEditor Project.uproject",
    service_file: "Saved/service.json", start_time: "2026-01-01T00:00:00Z", service_mtime: 1,
    capture_sha256: "a".repeat(64) };
  const raw = reduceMcpTranscript([{ ordinal: 1, document_id: null, operation: "health", tool: "health_check",
    arguments: {}, status: "pass" }], { realTransport: true, serviceBefore: service, serviceAfter: service });
  assert.deepEqual(Object.keys(raw).sort(), ["calls", "evidence_kind", "exit_code", "schema_version", "service_before", "status"]);
  assert.deepEqual(raw.service_before, service);
  assert.equal("layer_envelope" in raw, false);
});

test("parses the shared layer envelope golden byte-stably", () => {
  const bytes = readFileSync(goldenPath, "utf8");
  const value = JSON.parse(bytes);
  validateLayerEnvelope(value);
  assert.equal(canonicalJson(value), bytes);
});

test("rejects every shared layer envelope enum and nested-shape mutation", () => {
  const golden = JSON.parse(readFileSync(goldenPath, "utf8"));
  const mutations = mutationCases(JSON.parse(readFileSync(mutationPath, "utf8")));
  for (const mutation of mutations) {
    const value = structuredClone(golden); applyMutation(value, mutation);
    assert.throws(() => validateLayerEnvelope(value), (error) => error instanceof LayerValidationError
      && error.code === mutation.code && error.path === mutation.path, mutation.id);
  }
  const nonfinite = structuredClone(golden);
  const service = { host: "127.0.0.1", port: 8562, pid: 10, command: "UnrealEditor Project.uproject",
    service_file: "Saved/service.json", start_time: "2026-01-01T00:00:00Z", service_mtime: Number.NaN,
    capture_sha256: "a".repeat(64) };
  Object.assign(nonfinite, { evidence_kind: "production_http", layer: "http_phase_a", phase: "phase_a",
    service_before: service, service_after: structuredClone(service) });
  assert.throws(() => validateLayerEnvelope(nonfinite), (error) => error instanceof LayerValidationError
    && error.code === "Layer.InvalidShape" && error.path === "/ServiceBefore/ServiceMtime");
});
