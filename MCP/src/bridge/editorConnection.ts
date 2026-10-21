import type { EditorTarget } from "./discovery.js";

export type JsonRpcId = string | number;
export type JsonRpcMessage = {
  jsonrpc: "2.0";
  id?: JsonRpcId | null;
  method?: string;
  params?: Record<string, unknown>;
  result?: unknown;
  error?: { code: number; message: string; data?: unknown };
};

// The editor could not be reached (not running, wrong port): the message says where the bridge looked.
export class EditorUnreachableError extends Error {}

// The editor answered with an HTTP refusal that is not a JSON-RPC message (security checks: Host, Origin, token).
export class EditorRefusedError extends Error {
  constructor(
    readonly status: number,
    message: string,
  ) {
    super(message);
  }
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

/**
 * The error of a request that never reached the editor (connection refused, reset, DNS...). It names the port the
 * editor's UEAssetDoc HTTP server should listen on, where the bridge got the address and what to do.
 */
export function describeConnectionFailure(url: string, error: unknown, source?: string): string {
  let port = "";
  try {
    const parsed = new URL(url);
    port = parsed.port || (parsed.protocol === "https:" ? "443" : "80");
  } catch {
    port = "(unknown)";
  }
  const cause = isRecord(error) && isRecord(error.cause) ? error.cause : undefined;
  const code = cause && typeof cause.code === "string" ? cause.code : undefined;
  const reason = [error instanceof Error ? error.message : String(error), code].filter(Boolean).join(": ");
  return (
    `The Unreal Editor's UEAssetDoc HTTP server is not listening on port ${port} (${url}: ${reason}). ` +
    "Check that the editor is running and look in its log (LogAssetDocumentServer: a port that cannot be bound is reported " +
    "there and as an editor notification), or restart the editor. The bridge finds the editor through UE_API_BASE, " +
    `UE_PROJECT or the project's Saved/UEAssetDoc/service.json${source ? ` (this time: ${source})` : ""}.`
  );
}

function parseEventStream(text: string): JsonRpcMessage[] {
  const messages: JsonRpcMessage[] = [];
  for (const event of text.split(/\r?\n\r?\n/)) {
    const data = event
      .split(/\r?\n/)
      .filter((line) => line.startsWith("data:"))
      .map((line) => line.slice(5).replace(/^ /, ""))
      .join("\n");
    if (data.length > 0) {
      messages.push(JSON.parse(data) as JsonRpcMessage);
    }
  }
  return messages;
}

// What the editor answered: its JSON-RPC message, or the text of an HTTP refusal (security checks: Host, Origin, token).
type PostResult = { status: number; message?: JsonRpcMessage; sessionId?: string; refusal?: string };

/**
 * One MCP session with the editor's /mcp endpoint, opened on first use with the client's initialize parameters and
 * opened again when the editor no longer knows it (the editor restarted) or the target changed.
 */
export class EditorConnection {
  private target?: EditorTarget;
  private sessionId?: string;
  private protocolVersion?: string;
  private opening?: Promise<void>;
  private nextId = 1;

  constructor(
    private readonly resolveTarget: () => EditorTarget,
    private readonly initializeParams: () => Record<string, unknown>,
  ) {}

  get currentTarget(): EditorTarget | undefined {
    return this.target;
  }

  /** Forwards a request and returns the editor's response. */
  async request(message: JsonRpcMessage): Promise<JsonRpcMessage> {
    let result: PostResult = { status: 0 };
    for (let attempt = 0; attempt < 2; attempt++) {
      await this.open();
      result = await this.post(message);
      if (!this.isSessionLost(result) && result.status !== 401) {
        break;
      }
      // The editor restarted (new session table, new token, maybe a new port): find it again, once.
      this.reset();
    }
    if (result.message) {
      return result.message;
    }
    throw new EditorRefusedError(result.status, result.refusal ?? `the editor answered HTTP ${result.status} without a JSON-RPC message`);
  }

  /** Forwards a notification when a session is open. */
  async notify(message: JsonRpcMessage): Promise<void> {
    if (this.sessionId) {
      await this.post(message).catch(() => undefined);
    }
  }

  /** Ends the session (best effort). */
  async close(): Promise<void> {
    if (!this.target || !this.sessionId) {
      return;
    }
    const headers: Record<string, string> = { "Mcp-Session-Id": this.sessionId };
    if (this.target.token) {
      headers.Authorization = `Bearer ${this.target.token}`;
    }
    await fetch(this.target.mcpUrl, { method: "DELETE", headers, signal: AbortSignal.timeout(1000) }).catch(() => undefined);
    this.reset();
  }

  private reset(): void {
    this.target = undefined;
    this.sessionId = undefined;
    this.protocolVersion = undefined;
  }

  private isSessionLost(result: PostResult): boolean {
    const code = result.message?.error?.code;
    return (result.status === 404 && code === -32001) || (result.status === 400 && /Mcp-Session-Id/.test(result.message?.error?.message ?? ""));
  }

  private open(): Promise<void> {
    if (this.sessionId) {
      return Promise.resolve();
    }
    this.opening ??= this.initialize().finally(() => {
      this.opening = undefined;
    });
    return this.opening;
  }

  private async initialize(): Promise<void> {
    this.target = this.resolveTarget();
    const result = await this.post({ jsonrpc: "2.0", id: `bridge-${this.nextId++}`, method: "initialize", params: this.initializeParams() });
    if (!result.message || result.message.error || !result.sessionId) {
      const reason = result.message?.error?.message ?? result.refusal ?? `HTTP ${result.status}`;
      this.reset();
      throw new EditorRefusedError(result.status, reason);
    }
    this.sessionId = result.sessionId;
    const negotiated = isRecord(result.message.result) ? result.message.result.protocolVersion : undefined;
    this.protocolVersion = typeof negotiated === "string" ? negotiated : undefined;
    await this.post({ jsonrpc: "2.0", method: "notifications/initialized" });
  }

  private async post(message: JsonRpcMessage): Promise<PostResult> {
    const target = this.target ?? (this.target = this.resolveTarget());
    const headers: Record<string, string> = {
      "Content-Type": "application/json",
      Accept: "application/json, text/event-stream",
    };
    if (this.sessionId) {
      headers["Mcp-Session-Id"] = this.sessionId;
    }
    if (this.protocolVersion) {
      headers["MCP-Protocol-Version"] = this.protocolVersion;
    }
    if (target.token) {
      headers.Authorization = `Bearer ${target.token}`;
    }

    let response: Response;
    try {
      response = await fetch(target.mcpUrl, { method: "POST", headers, body: JSON.stringify(message) });
    } catch (error) {
      const text = describeConnectionFailure(target.mcpUrl, error, target.source);
      this.reset();
      throw new EditorUnreachableError(text);
    }

    const sessionId = response.headers.get("mcp-session-id") ?? undefined;
    const body = await response.text();
    if (response.status === 202 || body.length === 0) {
      return { status: response.status, sessionId };
    }
    let messages: JsonRpcMessage[];
    try {
      const contentType = response.headers.get("content-type") ?? "";
      messages = contentType.includes("text/event-stream") ? parseEventStream(body) : [JSON.parse(body) as JsonRpcMessage];
    } catch {
      return { status: response.status, sessionId, refusal: `HTTP ${response.status}: ${body.slice(0, 500)}` };
    }
    const reply = messages.find((candidate) => isRecord(candidate) && candidate.jsonrpc === "2.0" && ("result" in candidate || "error" in candidate));
    if (!reply) {
      // A refusal of the HTTP server's security checks: {success: false, error}.
      const refusal = messages[0] as unknown;
      const error = isRecord(refusal) && typeof refusal.error === "string" ? refusal.error : body.slice(0, 500);
      return { status: response.status, sessionId, refusal: `HTTP ${response.status}: ${error}` };
    }
    return { status: response.status, message: reply, sessionId };
  }
}
