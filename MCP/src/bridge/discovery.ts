// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

import { existsSync, readdirSync, readFileSync, realpathSync, statSync } from "fs";
import { dirname, join, resolve } from "path";

export const DEFAULT_MCP_URL = "http://127.0.0.1:8559/mcp";
const DISCOVERY_FILE = join("Saved", "UEAssetDoc", "service.json");

/**
 * Where the editor's MCP endpoint is and the token it requires (if any), with what told the bridge so. `project` is
 * the project directory the editor there must serve; `unavailable` (with an empty `mcpUrl`) says why there is no
 * editor to connect to.
 */
export type EditorTarget = {
  mcpUrl: string;
  token?: string;
  source: string;
  project?: string;
  unavailable?: string;
};

type ServiceFile = { port?: number; baseUrl?: string; mcpUrl?: string; authToken?: string; pid?: number };

function readServiceFile(path: string): ServiceFile | undefined {
  try {
    const parsed = JSON.parse(readFileSync(path, "utf-8")) as ServiceFile;
    return parsed && typeof parsed === "object" ? parsed : undefined;
  } catch {
    return undefined;
  }
}

function mcpUrlOf(service: ServiceFile): string | undefined {
  if (typeof service.mcpUrl === "string" && service.mcpUrl.length > 0) {
    return service.mcpUrl;
  }
  if (typeof service.baseUrl === "string" && service.baseUrl.length > 0) {
    return `${service.baseUrl.replace(/\/+$/, "")}/mcp`;
  }
  return typeof service.port === "number" ? `http://127.0.0.1:${service.port}/mcp` : undefined;
}

// The project directory of UE_PROJECT: the directory itself, or the directory of a .uproject file.
function projectDirectory(path: string): string {
  const absolute = resolve(path);
  return absolute.toLowerCase().endsWith(".uproject") ? dirname(absolute) : absolute;
}

function isProjectDirectory(directory: string): boolean {
  try {
    return readdirSync(directory).some((entry) => entry.toLowerCase().endsWith(".uproject"));
  } catch {
    return false;
  }
}

// The project containing the working directory (MCP clients start the bridge in the workspace): the nearest directory
// at or above it that holds a .uproject file, whether or not its editor is running.
function findProjectFrom(start: string): string | undefined {
  let directory = resolve(start);
  for (;;) {
    if (isProjectDirectory(directory)) {
      return directory;
    }
    const parent = dirname(directory);
    if (parent === directory) {
      return undefined;
    }
    directory = parent;
  }
}

const LOOPBACK_HOSTS = new Set(["127.0.0.1", "localhost", "[::1]"]);

// Protocol, host and port of a URL; the loopback names are one host.
function endpointOf(url: string): string | undefined {
  try {
    const parsed = new URL(url);
    const host = parsed.hostname.toLowerCase().replace(/\.$/, "");
    const port = parsed.port || (parsed.protocol === "https:" ? "443" : "80");
    return `${parsed.protocol}//${LOOPBACK_HOSTS.has(host) ? "loopback" : host}:${port}`;
  } catch {
    return undefined;
  }
}

function sameEndpoint(a: string, b: string): boolean {
  const endpoint = endpointOf(a);
  return endpoint !== undefined && endpoint === endpointOf(b);
}

/** The file system calls the project identity check uses (a test passes its own to model another volume). */
export type IdentityFileSystem = {
  realpath(path: string): string;
  stat(path: string): { dev: bigint; ino: bigint };
};

const nodeFileSystem: IdentityFileSystem = {
  realpath: (path) => realpathSync.native(path),
  stat: (path) => statSync(path, { bigint: true }),
};

// A path as the file system resolves it (links followed), or as written when it does not exist here; separators and
// a trailing separator do not count. Case is kept: whether it matters is the file system's decision, not the OS's.
function resolvedPath(path: string, fileSystem: IdentityFileSystem): string {
  let canonical = path;
  try {
    canonical = fileSystem.realpath(path);
  } catch {
    // A path that does not exist here is compared as written.
  }
  canonical = canonical.replace(/\\/g, "/");
  return canonical.length > 1 ? canonical.replace(/\/+$/, "") : canonical;
}

// The volume and file id of a directory (inode, or the NTFS file id through a bigint stat), or undefined when it
// cannot be read or the file system reports no id.
function fileIdentity(path: string, fileSystem: IdentityFileSystem): string | undefined {
  try {
    const stat = fileSystem.stat(path);
    return stat.ino === 0n ? undefined : `${stat.dev}:${stat.ino}`;
  } catch {
    return undefined;
  }
}

/**
 * Whether two spellings of a project directory are one directory: the same resolved path (trailing separator, links
 * and ".." do not count), or the same volume and file id, which also matches spellings that differ only in case on
 * a volume that ignores case. Two directories whose names differ only in case on a case-sensitive volume are two
 * projects.
 */
export function sameProjectDirectory(expected: string, reported: string, fileSystem: IdentityFileSystem = nodeFileSystem): boolean {
  if (expected.length === 0 || reported.length === 0) {
    return false;
  }
  if (resolvedPath(expected, fileSystem) === resolvedPath(reported, fileSystem)) {
    return true;
  }
  const identity = fileIdentity(expected, fileSystem);
  return identity !== undefined && identity === fileIdentity(reported, fileSystem);
}

/**
 * Finds the editor to talk to, in this order:
 *  1. UE_API_BASE: the editor's address (http://127.0.0.1:<port>, or its /mcp URL); the token is UE_ASSETDOC_TOKEN, or
 *     the one in the discovery file of UE_PROJECT / the working directory's project when that file names the same
 *     protocol, host and port (127.0.0.1, localhost and [::1] are one host). Another host needs UE_ASSETDOC_TOKEN.
 *  2. UE_PROJECT: a project directory or .uproject file; its Saved/UEAssetDoc/service.json names the port and token.
 *     Without a usable file the project's editor is not running, and there is no address: the bridge connects nowhere.
 *  3. The project containing the working directory (the nearest directory above it with a .uproject file), through
 *     its Saved/UEAssetDoc/service.json; without a usable file it is not running either: the bridge connects nowhere.
 *  4. http://127.0.0.1:8559/mcp (the plugin's default port), only when no project is named or contains the working
 *     directory; there is then no project to check.
 * A named project (UE_PROJECT), or the working directory's project when UE_API_BASE is not set, is the `project` the
 * editor must serve.
 */
export function resolveEditorTarget(env: NodeJS.ProcessEnv = process.env, cwd: string = process.cwd()): EditorTarget {
  const named = env.UE_PROJECT ? projectDirectory(env.UE_PROJECT) : undefined;
  const known = named ?? findProjectFrom(cwd);
  const projectFile = known ? join(known, DISCOVERY_FILE) : undefined;
  const service = projectFile ? readServiceFile(projectFile) : undefined;
  const serviceUrl = service ? mcpUrlOf(service) : undefined;

  const base = env.UE_API_BASE?.trim();
  if (base) {
    const trimmed = base.replace(/\/+$/, "");
    const mcpUrl = /\/mcp$/.test(trimmed) ? trimmed : `${trimmed}/mcp`;
    const token = env.UE_ASSETDOC_TOKEN || (serviceUrl && sameEndpoint(serviceUrl, mcpUrl) ? service?.authToken : undefined);
    return { mcpUrl, token: token || undefined, source: "UE_API_BASE", ...(named ? { project: named } : {}) };
  }
  if (known && projectFile && serviceUrl) {
    return { mcpUrl: serviceUrl, token: env.UE_ASSETDOC_TOKEN || service?.authToken || undefined, source: projectFile, project: known };
  }
  if (known && projectFile) {
    const problem = existsSync(projectFile) ? "names no editor address" : "does not exist";
    const how = named ? "UE_PROJECT" : "the project containing the working directory";
    return {
      mcpUrl: "",
      source: projectFile,
      project: known,
      unavailable:
        `The Unreal Editor of the project ${known} (${how}) is not running: ${projectFile} ${problem}. The editor writes ` +
        "that file once its UEAssetDoc server has started. Start the editor of that project, or set UE_PROJECT to the " +
        "project whose editor is open; the bridge does not send this project's requests to any other editor.",
    };
  }
  return { mcpUrl: DEFAULT_MCP_URL, token: env.UE_ASSETDOC_TOKEN || undefined, source: "default port" };
}
