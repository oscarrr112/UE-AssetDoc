// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

import test from "node:test";
import assert from "node:assert/strict";
import { existsSync, mkdirSync, mkdtempSync, rmSync, statSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, sep } from "node:path";

import { DEFAULT_MCP_URL, resolveEditorTarget, sameProjectDirectory, type IdentityFileSystem } from "./discovery.js";

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
  assert.deepEqual(target, { mcpUrl: "http://127.0.0.1:9010/mcp", token: "t", source: join(project, SERVICE_FILE), project });
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

test("a UE_PROJECT without a usable discovery file names no address: the bridge connects nowhere", () => {
  const project = makeProject();
  const file = join(project, SERVICE_FILE);
  const target = resolveEditorTarget({ UE_PROJECT: project }, emptyDirectory());
  assert.equal(target.mcpUrl, "", "no fallback to the default port");
  assert.equal(target.source, file);
  assert.ok(target.unavailable, "the target says why there is no address");
  assert.ok(target.unavailable.includes(project), "the reason names the project");
  assert.ok(target.unavailable.includes(file), "the reason names the missing file");
  assert.match(target.unavailable, /is not running/);
  assert.ok(!target.unavailable.includes("8559"));
  // An explicit token changes nothing: there is still no editor to send it to.
  assert.equal(resolveEditorTarget({ UE_PROJECT: project, UE_ASSETDOC_TOKEN: "t" }, emptyDirectory()).mcpUrl, "");

  for (const unusable of [makeProject(undefined, { raw: "{ not json" }), makeProject({}), makeProject({ port: "8559" })]) {
    const named = resolveEditorTarget({ UE_PROJECT: unusable }, emptyDirectory());
    assert.equal(named.mcpUrl, "", unusable);
    assert.ok(named.unavailable?.includes(join(unusable, SERVICE_FILE)), unusable);
  }
  // A project directory that does not exist is no different.
  const absent = join(scratch, "no-such-project");
  assert.ok(resolveEditorTarget({ UE_PROJECT: absent }, emptyDirectory()).unavailable?.includes(absent));
});

test("a named project is the project the editor must serve", () => {
  const project = makeProject({ port: 9024 });
  assert.equal(resolveEditorTarget({ UE_PROJECT: project }, emptyDirectory()).project, project);
  assert.equal(resolveEditorTarget({ UE_PROJECT: join(project, "Game.uproject") }, emptyDirectory()).project, project);
  // UE_API_BASE gives the address; the named project is still the one expected there.
  assert.equal(resolveEditorTarget({ UE_PROJECT: project, UE_API_BASE: "http://127.0.0.1:9025" }, emptyDirectory()).project, project);
  // The working directory's project, when its discovery file gives the address.
  assert.equal(resolveEditorTarget({}, nested(project, "Source")).project, project);
  // No project: nothing to check.
  assert.equal(resolveEditorTarget({ UE_API_BASE: "http://127.0.0.1:9025" }, emptyDirectory()).project, undefined);
  assert.equal(resolveEditorTarget({ UE_API_BASE: "http://127.0.0.1:9025" }, nested(project, "Source")).project, undefined, "UE_API_BASE names another editor");
  assert.equal(resolveEditorTarget({}, emptyDirectory()).project, undefined);
});

test("the discovery file's token never goes to another host on the same port", () => {
  const project = makeProject({ port: 9026, authToken: "local" });
  const env = { UE_PROJECT: project };
  for (const base of ["http://192.0.2.10:9026", "http://example.com:9026/mcp", "http://127.0.0.2:9026", "https://127.0.0.1:9026"]) {
    const target = resolveEditorTarget({ ...env, UE_API_BASE: base }, emptyDirectory());
    assert.equal(target.token, undefined, base);
    assert.equal(target.mcpUrl, `${base.replace(/\/mcp$/, "")}/mcp`, base);
  }
  // Another host needs its own token.
  assert.equal(resolveEditorTarget({ ...env, UE_API_BASE: "http://192.0.2.10:9026", UE_ASSETDOC_TOKEN: "remote" }, emptyDirectory()).token, "remote");
  // A discovery file naming another host does not lend its token to loopback either.
  const remote = makeProject({ mcpUrl: "http://192.0.2.10:9027/mcp", authToken: "remote-file" });
  assert.equal(resolveEditorTarget({ UE_PROJECT: remote, UE_API_BASE: "http://127.0.0.1:9027" }, emptyDirectory()).token, undefined);
});

test("the discovery file's token goes to the same loopback host however it is spelled", () => {
  const project = makeProject({ port: 9028, authToken: "local" });
  for (const base of ["http://localhost:9028", "http://LOCALHOST:9028/mcp", "http://127.0.0.1:9028", "http://[::1]:9028"]) {
    assert.equal(resolveEditorTarget({ UE_PROJECT: project, UE_API_BASE: base }, emptyDirectory()).token, "local", base);
  }
  const byLocalhost = makeProject({ baseUrl: "http://localhost:9029", authToken: "by-name" });
  assert.equal(resolveEditorTarget({ UE_PROJECT: byLocalhost, UE_API_BASE: "http://127.0.0.1:9029" }, emptyDirectory()).token, "by-name");
  const defaultPort = makeProject({ mcpUrl: "http://localhost/mcp", authToken: "port-80" });
  assert.equal(resolveEditorTarget({ UE_PROJECT: defaultPort, UE_API_BASE: "http://127.0.0.1:80" }, emptyDirectory()).token, "port-80");
});

test("sameProjectDirectory compares directories, not spellings", () => {
  const project = makeProject({ port: 9031 });
  assert.ok(sameProjectDirectory(project, `${project}/`));
  assert.ok(sameProjectDirectory(project, `${project}${sep}`));
  assert.ok(sameProjectDirectory(project, join(project, "Saved", "..")));
  const link = join(scratch, `link-${counter++}`);
  symlinkSync(project, link, "dir");
  assert.ok(sameProjectDirectory(link, `${project}/`), "a link to the project is the project");
  assert.ok(!sameProjectDirectory(project, makeProject({ port: 9032 })));
  assert.ok(!sameProjectDirectory(project, `${project}Other`));
  assert.ok(!sameProjectDirectory(project, ""));
});

// A volume where every spelling is its own path: realpath returns the path as given (as on a case-sensitive volume,
// where "Project" and "project" are two directories), and stat reports the ids in `ids`.
function modelVolume(ids: Record<string, bigint>): IdentityFileSystem {
  return {
    realpath: (path) => {
      if (!(path in ids)) {
        throw new Error(`ENOENT: ${path}`);
      }
      return path;
    },
    stat: (path) => {
      if (!(path in ids)) {
        throw new Error(`ENOENT: ${path}`);
      }
      return { dev: 7n, ino: ids[path] };
    },
  };
}

test("sameProjectDirectory keeps case where the volume does: two directories differing only in case are two projects", () => {
  const sensitive = modelVolume({ "/srv/work/Project": 101n, "/srv/work/project": 102n });
  assert.ok(!sameProjectDirectory("/srv/work/Project", "/srv/work/project", sensitive));
  assert.ok(!sameProjectDirectory("/srv/work/Project", "/srv/WORK/project/", sensitive), "an unknown spelling is not the project");
  assert.ok(sameProjectDirectory("/srv/work/Project", "/srv/work/Project/", sensitive));
  // On a volume that ignores case both spellings are one directory: one file id.
  const insensitive = modelVolume({ "/srv/work/Project": 101n, "/srv/work/project": 101n });
  assert.ok(sameProjectDirectory("/srv/work/Project", "/srv/work/project", insensitive));
  // A file system that reports no file id (0) proves nothing.
  const noIds = modelVolume({ "C:/Work/Project": 0n, "C:/Work/project": 0n });
  assert.ok(!sameProjectDirectory("C:/Work/Project", "C:/Work/project", noIds));
});

test("sameProjectDirectory matches a differently cased spelling on this machine only when the volume ignores case", () => {
  const project = makeProject({ port: 9033 });
  const recased = project.replace(/project-(\d+)$/, "PROJECT-$1");
  const ignoresCase = existsSync(recased);
  assert.equal(sameProjectDirectory(project, recased), ignoresCase);
  if (ignoresCase) {
    assert.equal(statSync(recased).ino, statSync(project).ino);
  }
});

test("the working directory's project is found from a nested directory", () => {
  const project = makeProject({ port: 9020, authToken: "walk" });
  for (const cwd of [project, nested(project, "Source"), nested(project, "Plugins", "Tool", "Source", "Private")]) {
    const target = resolveEditorTarget({}, cwd);
    assert.deepEqual(target, { mcpUrl: "http://127.0.0.1:9020/mcp", token: "walk", source: join(project, SERVICE_FILE), project }, cwd);
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

test("the working directory's project without a usable discovery file names no address: the bridge connects nowhere", () => {
  // The project is found from its .uproject file alone, whatever the state of its discovery file.
  const notRunning = makeProject();
  const cases: [string, string][] = [
    [notRunning, notRunning],
    [nested(notRunning, "Source", "Game"), notRunning],
  ];
  const noSaved = join(scratch, `project-${counter++}`);
  mkdirSync(join(noSaved, "Content"), { recursive: true });
  writeFileSync(join(noSaved, "Other.uproject"), "{}");
  cases.push([nested(noSaved, "Content"), noSaved]);
  for (const unusable of [makeProject(undefined, { raw: "{ not json" }), makeProject({}), makeProject({ port: "8559" })]) {
    cases.push([unusable, unusable]);
  }
  for (const [cwd, project] of cases) {
    const file = join(project, SERVICE_FILE);
    const target = resolveEditorTarget({}, cwd);
    assert.equal(target.mcpUrl, "", `no fallback to the default port (${cwd})`);
    assert.equal(target.project, project, cwd);
    assert.equal(target.source, file, cwd);
    assert.ok(target.unavailable?.includes(project), "the reason names the project");
    assert.ok(target.unavailable?.includes(file), "the reason names the discovery file");
    assert.match(target.unavailable ?? "", /is not running/);
    assert.equal(resolveEditorTarget({ UE_ASSETDOC_TOKEN: "t" }, cwd).mcpUrl, "", "a token changes nothing");
  }
  // UE_API_BASE still names an editor explicitly.
  assert.equal(resolveEditorTarget({ UE_API_BASE: "http://127.0.0.1:9033" }, notRunning).mcpUrl, "http://127.0.0.1:9033/mcp");
});

test("only without any project context is the default port used, with no project to check", () => {
  for (const cwd of [emptyDirectory(), nested(makeProject({ port: 9034 }, { uproject: false }), "Source")]) {
    const target = resolveEditorTarget({}, cwd);
    assert.deepEqual(target, { mcpUrl: DEFAULT_MCP_URL, token: undefined, source: "default port" }, cwd);
  }
});

test("nothing found means the default port", () => {
  assert.deepEqual(resolveEditorTarget({}, emptyDirectory()), { mcpUrl: DEFAULT_MCP_URL, token: undefined, source: "default port" });
});
