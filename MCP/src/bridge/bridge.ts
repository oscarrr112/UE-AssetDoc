import { resolveEditorTarget } from "./discovery.js";
import { EditorConnection, EditorRefusedError, EditorUnreachableError, type JsonRpcMessage } from "./editorConnection.js";
import { listedTools, type Manifest } from "./manifest.js";

export const SUPPORTED_PROTOCOL_VERSIONS = ["2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05", "2024-10-07"];

function isRequest(message: JsonRpcMessage): boolean {
  return typeof message.method === "string" && message.id !== undefined && message.id !== null;
}

/**
 * The stdio side of the bridge: answers initialize and ping itself, forwards every other request to the editor's MCP
 * endpoint and returns the editor's response unchanged. While the editor cannot be reached, tools/list answers the
 * manifest's tools and a tool call answers a tool error that says where the bridge looked for the editor.
 */
export class StdioBridge {
  private clientParams?: Record<string, unknown>;
  readonly connection: EditorConnection;

  constructor(
    private readonly manifest: Manifest,
    private readonly send: (message: JsonRpcMessage) => void,
    env: NodeJS.ProcessEnv = process.env,
    cwd: string = process.cwd(),
  ) {
    this.connection = new EditorConnection(
      () => resolveEditorTarget(env, cwd),
      () =>
        this.clientParams ?? {
          protocolVersion: SUPPORTED_PROTOCOL_VERSIONS[0],
          capabilities: {},
          clientInfo: { name: `${manifest.server.name}-bridge`, version: manifest.server.version },
        },
    );
  }

  async handle(message: JsonRpcMessage): Promise<void> {
    if (!isRequest(message)) {
      // The bridge's own session sent its initialized notification; others (cancelled, ...) go to the editor.
      if (typeof message.method === "string" && message.method !== "notifications/initialized") {
        await this.connection.notify(message);
      }
      return;
    }
    const id = message.id!;
    if (message.method === "initialize") {
      this.clientParams = message.params ?? {};
      const requested = this.clientParams.protocolVersion;
      const protocolVersion = typeof requested === "string" && SUPPORTED_PROTOCOL_VERSIONS.includes(requested) ? requested : SUPPORTED_PROTOCOL_VERSIONS[0];
      this.send({ jsonrpc: "2.0", id, result: { protocolVersion, capabilities: { tools: {} }, serverInfo: this.manifest.server } });
      return;
    }
    if (message.method === "ping") {
      this.send({ jsonrpc: "2.0", id, result: {} });
      return;
    }
    try {
      this.send({ ...(await this.connection.request(message)), id });
    } catch (error) {
      const text = error instanceof Error ? error.message : String(error);
      if (error instanceof EditorUnreachableError && message.method === "tools/list") {
        this.send({ jsonrpc: "2.0", id, result: { tools: listedTools(this.manifest) } });
      } else if ((error instanceof EditorUnreachableError || error instanceof EditorRefusedError) && message.method === "tools/call") {
        const refused = error instanceof EditorRefusedError ? `The editor refused the request: ${text}` : text;
        this.send({ jsonrpc: "2.0", id, result: { content: [{ type: "text", text: refused }], isError: true } });
      } else {
        this.send({ jsonrpc: "2.0", id, error: { code: -32603, message: text } });
      }
    }
  }
}
