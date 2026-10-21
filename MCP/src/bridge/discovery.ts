import { existsSync, readdirSync, readFileSync, statSync } from "fs";
import { dirname, join, resolve } from "path";

export const DEFAULT_MCP_URL = "http://127.0.0.1:8559/mcp";
const DISCOVERY_FILE = join("Saved", "UEAssetDoc", "service.json");

// Where the editor's MCP endpoint is and the token it requires (if any), with what told the bridge so.
export type EditorTarget = {
  mcpUrl: string;
  token?: string;
  source: string;
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

// The discovery file of the project containing the working directory (MCP clients start the bridge in the workspace).
function findDiscoveryFileFrom(start: string): string | undefined {
  let directory = resolve(start);
  for (;;) {
    const candidate = join(directory, DISCOVERY_FILE);
    if (existsSync(candidate) && statSync(candidate).isFile() && isProjectDirectory(directory)) {
      return candidate;
    }
    const parent = dirname(directory);
    if (parent === directory) {
      return undefined;
    }
    directory = parent;
  }
}

function portOf(url: string): string | undefined {
  try {
    const parsed = new URL(url);
    return parsed.port || (parsed.protocol === "https:" ? "443" : "80");
  } catch {
    return undefined;
  }
}

/**
 * Finds the editor to talk to, in this order:
 *  1. UE_API_BASE: the editor's address (http://127.0.0.1:<port>, or its /mcp URL); the token is UE_ASSETDOC_TOKEN, or
 *     the one in the discovery file of UE_PROJECT / the working directory's project when that file names the same port.
 *  2. UE_PROJECT: a project directory or .uproject file; its Saved/UEAssetDoc/service.json names the port and token.
 *  3. The project containing the working directory, through its Saved/UEAssetDoc/service.json.
 *  4. http://127.0.0.1:8559/mcp (the plugin's default port).
 */
export function resolveEditorTarget(env: NodeJS.ProcessEnv = process.env, cwd: string = process.cwd()): EditorTarget {
  const projectFile = env.UE_PROJECT ? join(projectDirectory(env.UE_PROJECT), DISCOVERY_FILE) : findDiscoveryFileFrom(cwd);
  const service = projectFile ? readServiceFile(projectFile) : undefined;

  const base = env.UE_API_BASE?.trim();
  if (base) {
    const trimmed = base.replace(/\/+$/, "");
    const mcpUrl = /\/mcp$/.test(trimmed) ? trimmed : `${trimmed}/mcp`;
    const serviceUrl = service ? mcpUrlOf(service) : undefined;
    const token = env.UE_ASSETDOC_TOKEN || (serviceUrl && portOf(serviceUrl) === portOf(mcpUrl) ? service?.authToken : undefined);
    return { mcpUrl, token: token || undefined, source: "UE_API_BASE" };
  }
  if (projectFile) {
    const mcpUrl = service ? mcpUrlOf(service) : undefined;
    if (mcpUrl) {
      return { mcpUrl, token: env.UE_ASSETDOC_TOKEN || service?.authToken || undefined, source: projectFile };
    }
    if (env.UE_PROJECT) {
      // The project's editor is not running (no discovery file): name the file in the connection error.
      return { mcpUrl: DEFAULT_MCP_URL, token: env.UE_ASSETDOC_TOKEN || undefined, source: `${projectFile} (missing)` };
    }
  }
  return { mcpUrl: DEFAULT_MCP_URL, token: env.UE_ASSETDOC_TOKEN || undefined, source: "default port" };
}
