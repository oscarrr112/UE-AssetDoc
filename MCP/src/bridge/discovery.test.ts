import test from "node:test";
import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { DEFAULT_MCP_URL, resolveEditorTarget } from "./discovery.js";

const SERVICE_FILE = join("Saved", "UEAssetDoc", "service.json");

let scratch: string;

test.before(() => {
  scratch = mkdtempSync(join(tmpdir(), "ue-assetdoc-discovery-"));
});

test.after(() => {
  rmSync(scratch, { recursive: true, force: true });
});

let counter = 0;

// A project directory with a .uproject file and, when given, the discovery file the editor writes.
function makeProject(service?: unknown, options: { uproject?: boolean; raw?: string } = {}): string {
  const directory = join(scratch, `project-${counter++}`);
  mkdirSync(join(directory, "Saved", "UEAssetDoc"), { recursive: true });
  if (options.uproject !== false) {
    writeFileSync(join(directory, "Game.uproject"), "{}");
  }
  if (options.raw !== undefined) {
    writeFileSync(join(directory, SERVICE_FILE), options.raw);
  } else if (service !== undefined) {
    writeFileSync(join(directory, SERVICE_FILE), JSON.stringify(service));
  }
  return directory;
}

// A directory below the project, as an MCP client's workspace may be.
function nested(project: string, ...segments: string[]): string {
  const directory = join(project, ...segments);
  mkdirSync(directory, { recursive: true });
  return directory;
}

// A working directory with no project above it.
function emptyDirectory(): string {
  const directory = join(scratch, `empty-${counter++}`);
  mkdirSync(directory, { recursive: true });
  return directory;
}

test("the default is the plugin's default port", () => {
  assert.equal(DEFAULT_MCP_URL, "http://127.0.0.1:8559/mcp");
  assert.deepEqual(resolveEditorTarget({}, emptyDirectory()), { mcpUrl: DEFAULT_MCP_URL, token: undefined, source: "default port" });
});

test("UE_API_BASE names the editor by its base address or its /mcp URL", () => {
  const cwd = emptyDirectory();
  for (const value of [
    "http://127.0.0.1:9001",
    "http://127.0.0.1:9001/",
    "http://127.0.0.1:9001///",
    "http://127.0.0.1:9001/mcp",
    "http://127.0.0.1:9001/mcp/",
    "  http://127.0.0.1:9001  ",
  ]) {
    const target = resolveEditorTarget({ UE_API_BASE: value }, cwd);
    assert.equal(target.mcpUrl, "http://127.0.0.1:9001/mcp", JSON.stringify(value));
    assert.equal(target.source, "UE_API_BASE");
    assert.equal(target.token, undefined);
  }
});

test("a blank UE_API_BASE is ignored", () => {
  const target = resolveEditorTarget({ UE_API_BASE: "   " }, emptyDirectory());
  assert.equal(target.mcpUrl, DEFAULT_MCP_URL);
  assert.equal(target.source, "default port");
});

test("the token of UE_ASSETDOC_TOKEN goes with UE_API_BASE and with the default port", () => {
  const cwd = emptyDirectory();
  assert.deepEqual(resolveEditorTarget({ UE_API_BASE: "http://127.0.0.1:9001", UE_ASSETDOC_TOKEN: "secret" }, cwd), {
    mcpUrl: "http://127.0.0.1:9001/mcp",
    token: "secret",
    source: "UE_API_BASE",
  });
  assert.deepEqual(resolveEditorTarget({ UE_ASSETDOC_TOKEN: "secret" }, cwd), { mcpUrl: DEFAULT_MCP_URL, token: "secret", source: "default port" });
  assert.equal(resolveEditorTarget({ UE_ASSETDOC_TOKEN: "" }, cwd).token, undefined, "an empty token is no token");
});

test("UE_API_BASE takes the project's token only when the discovery file names the same port", () => {
  const project = makeProject({ port: 9002, authToken: "from-file" });
  const env = { UE_PROJECT: project };
  assert.equal(resolveEditorTarget({ ...env, UE_API_BASE: "http://127.0.0.1:9002" }, emptyDirectory()).token, "from-file");
  assert.equal(resolveEditorTarget({ ...env, UE_API_BASE: "http://127.0.0.1:9002/mcp" }, emptyDirectory()).token, "from-file");
  assert.equal(resolveEditorTarget({ ...env, UE_API_BASE: "http://127.0.0.1:9003" }, emptyDirectory()).token, undefined, "another port: another editor");
  // An explicit token wins over the file's.
  assert.equal(resolveEditorTarget({ ...env, UE_API_BASE: "http://127.0.0.1:9002", UE_ASSETDOC_TOKEN: "explicit" }, emptyDirectory()).token, "explicit");
  // The address still comes from UE_API_BASE.
  assert.equal(resolveEditorTarget({ ...env, UE_API_BASE: "http://127.0.0.1:9003" }, emptyDirectory()).mcpUrl, "http://127.0.0.1:9003/mcp");
});

test("UE_API_BASE finds the token in the discovery file of the working directory's project", () => {
  const project = makeProject({ port: 9004, authToken: "from-cwd-project" });
  const cwd = nested(project, "Source", "Game");
  assert.equal(resolveEditorTarget({ UE_API_BASE: "http://127.0.0.1:9004" }, cwd).token, "from-cwd-project");
  assert.equal(resolveEditorTarget({ UE_API_BASE: "http://127.0.0.1:9005" }, cwd).token, undefined);
});

test("the port of a discovery file with only mcpUrl or baseUrl is compared too", () => {
  const byMcpUrl = makeProject({ mcpUrl: "http://127.0.0.1:9006/mcp", authToken: "a" });
  assert.equal(resolveEditorTarget({ UE_PROJECT: byMcpUrl, UE_API_BASE: "http://127.0.0.1:9006" }, emptyDirectory()).token, "a");
  const byBaseUrl = makeProject({ baseUrl: "http://127.0.0.1:9007/", authToken: "b" });
  assert.equal(resolveEditorTarget({ UE_PROJECT: byBaseUrl, UE_API_BASE: "http://127.0.0.1:9007" }, emptyDirectory()).token, "b");
  assert.equal(resolveEditorTarget({ UE_PROJECT: byBaseUrl, UE_API_BASE: "http://127.0.0.1:9008" }, emptyDirectory()).token, undefined);
});

test("UE_PROJECT: a project directory's discovery file names the port and token", () => {
  const project = makeProject({ port: 9010, authToken: "t", pid: 42 });
  const target = resolveEditorTarget({ UE_PROJECT: project }, emptyDirectory());
  assert.deepEqual(target, { mcpUrl: "http://127.0.0.1:9010/mcp", token: "t", source: join(project, SERVICE_FILE) });
});

test("UE_PROJECT may name the .uproject file", () => {
  const project = makeProject({ port: 9011 });
  const target = resolveEditorTarget({ UE_PROJECT: join(project, "Game.uproject") }, emptyDirectory());
  assert.equal(target.mcpUrl, "http://127.0.0.1:9011/mcp");
  assert.equal(target.token, undefined);
  assert.equal(target.source, join(project, SERVICE_FILE));
});

test("the discovery file's mcpUrl wins over baseUrl, which wins over port", () => {
  const cwd = emptyDirectory();
  const all = makeProject({ port: 9012, baseUrl: "http://127.0.0.1:9013", mcpUrl: "http://127.0.0.1:9014/mcp" });
  assert.equal(resolveEditorTarget({ UE_PROJECT: all }, cwd).mcpUrl, "http://127.0.0.1:9014/mcp");
  const baseAndPort = makeProject({ port: 9012, baseUrl: "http://127.0.0.1:9013/" });
  assert.equal(resolveEditorTarget({ UE_PROJECT: baseAndPort }, cwd).mcpUrl, "http://127.0.0.1:9013/mcp");
  const portOnly = makeProject({ port: 9012 });
  assert.equal(resolveEditorTarget({ UE_PROJECT: portOnly }, cwd).mcpUrl, "http://127.0.0.1:9012/mcp");
});

test("UE_ASSETDOC_TOKEN wins over the discovery file's token", () => {
  const project = makeProject({ port: 9015, authToken: "from-file" });
  assert.equal(resolveEditorTarget({ UE_PROJECT: project, UE_ASSETDOC_TOKEN: "explicit" }, emptyDirectory()).token, "explicit");
  assert.equal(resolveEditorTarget({ UE_PROJECT: project }, emptyDirectory()).token, "from-file");
});

test("UE_PROJECT wins over the working directory's project", () => {
  const named = makeProject({ port: 9016 });
  const working = makeProject({ port: 9017 });
  assert.equal(resolveEditorTarget({ UE_PROJECT: named }, nested(working, "Content")).mcpUrl, "http://127.0.0.1:9016/mcp");
});

test("UE_API_BASE wins over every discovery file", () => {
  const project = makeProject({ port: 9018, authToken: "t" });
  const target = resolveEditorTarget({ UE_API_BASE: "http://127.0.0.1:9019", UE_PROJECT: project }, nested(project, "Content"));
  assert.equal(target.mcpUrl, "http://127.0.0.1:9019/mcp");
  assert.equal(target.source, "UE_API_BASE");
});

test("a UE_PROJECT without a discovery file falls back to the default port and names the missing file", () => {
  const project = makeProject();
  const target = resolveEditorTarget({ UE_PROJECT: project }, emptyDirectory());
  assert.equal(target.mcpUrl, DEFAULT_MCP_URL);
  assert.equal(target.source, `${join(project, SERVICE_FILE)} (missing)`);
  assert.equal(resolveEditorTarget({ UE_PROJECT: project, UE_ASSETDOC_TOKEN: "t" }, emptyDirectory()).token, "t");
  const unreadable = makeProject(undefined, { raw: "{ not json" });
  assert.equal(resolveEditorTarget({ UE_PROJECT: unreadable }, emptyDirectory()).source, `${join(unreadable, SERVICE_FILE)} (missing)`);
});

test("the working directory's project is found from a nested directory", () => {
  const project = makeProject({ port: 9020, authToken: "walk" });
  for (const cwd of [project, nested(project, "Source"), nested(project, "Plugins", "Tool", "Source", "Private")]) {
    const target = resolveEditorTarget({}, cwd);
    assert.deepEqual(target, { mcpUrl: "http://127.0.0.1:9020/mcp", token: "walk", source: join(project, SERVICE_FILE) }, cwd);
  }
});

test("the nearest project above the working directory wins", () => {
  const outer = makeProject({ port: 9021 });
  const inner = join(outer, "Nested");
  mkdirSync(join(inner, "Saved", "UEAssetDoc"), { recursive: true });
  writeFileSync(join(inner, "Inner.uproject"), "{}");
  writeFileSync(join(inner, SERVICE_FILE), JSON.stringify({ port: 9022 }));
  assert.equal(resolveEditorTarget({}, nested(inner, "Source")).mcpUrl, "http://127.0.0.1:9022/mcp");
  assert.equal(resolveEditorTarget({}, outer).mcpUrl, "http://127.0.0.1:9021/mcp");
});

test("a discovery file in a directory without a .uproject file is not a project's", () => {
  const notAProject = makeProject({ port: 9023 }, { uproject: false });
  const target = resolveEditorTarget({}, nested(notAProject, "Source"));
  assert.equal(target.mcpUrl, DEFAULT_MCP_URL);
  assert.equal(target.source, "default port");
});

test("an unreadable discovery file of the working directory's project means the default port", () => {
  const project = makeProject(undefined, { raw: "{ not json" });
  assert.deepEqual(resolveEditorTarget({}, project), { mcpUrl: DEFAULT_MCP_URL, token: undefined, source: "default port" });
  const empty = makeProject({});
  assert.equal(resolveEditorTarget({}, empty).mcpUrl, DEFAULT_MCP_URL, "a file without an address names no editor");
});

test("nothing found means the default port", () => {
  assert.deepEqual(resolveEditorTarget({}, emptyDirectory()), { mcpUrl: DEFAULT_MCP_URL, token: undefined, source: "default port" });
});
