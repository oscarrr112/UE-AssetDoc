import { readFileSync } from "fs";
import { dirname, join } from "path";
import { fileURLToPath } from "url";

// The plugin's tool manifest (MCP/tools.json): the editor's MCP endpoint serves these tools. The bridge reads it only to
// answer tools/list while the editor cannot be reached.
export type ManifestTool = {
  name: string;
  description: string;
  inputSchema: Record<string, unknown>;
  call: Record<string, unknown>;
};

export type Manifest = {
  server: { name: string; version: string };
  tools: ManifestTool[];
};

// dist/bridge/manifest.js -> MCP/tools.json
export const PACKAGE_DIR = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
export const MANIFEST_PATH = join(PACKAGE_DIR, "tools.json");

export function loadManifest(path: string = MANIFEST_PATH): Manifest {
  const manifest = JSON.parse(readFileSync(path, "utf-8")) as Manifest;
  if (!manifest || !Array.isArray(manifest.tools) || typeof manifest.server?.name !== "string") {
    throw new Error(`${path} is not an MCP tool manifest`);
  }
  return manifest;
}

// The tools/list entries, as the editor lists them.
export function listedTools(manifest: Manifest): { name: string; description: string; inputSchema: Record<string, unknown> }[] {
  return manifest.tools.map(({ name, description, inputSchema }) => ({ name, description, inputSchema }));
}
