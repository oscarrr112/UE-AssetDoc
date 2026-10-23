// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

import { sameProjectDirectory, type EditorTarget } from "./discovery.js";

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

// The editor reached is not the named project's: the bridge forwards nothing to it.
export class EditorIdentityError extends EditorUnreachableError {}

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

// The system error code (ECONNREFUSED, ECONNRESET, ...) behind a fetch failure, when there is one.
function connectionErrorCode(error: unknown): string | undefined {
  const cause = isRecord(error) && isRecord(error.cause) ? error.cause : undefined;
  return cause && typeof cause.code === "string" ? cause.code : undefined;
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
  const reason = [error instanceof Error ? error.message : String(error), connectionErrorCode(error)].filter(Boolean).join(": ");
  return (
    `The Unreal Editor's UEAssetDoc HTTP server is not listening on port ${port} (${url}: ${reason}). ` +
    "Check that the editor is running and look in its log (LogAssetDocumentServer: a port that cannot be bound is reported " +
    "there and as an editor notification), or restart the editor. The bridge finds the editor through UE_API_BASE, " +
    `UE_PROJECT or the project's Saved/UEAssetDoc/service.json${source ? ` (this time: ${source})` : ""}.`
  );
}

// The projectDir of a health_check result (a text block holding the editor's health JSON).
function projectDirOf(result: unknown): string | undefined {
  if (!isRecord(result) || result.isError === true || !Array.isArray(result.content)) {
    return undefined;
  }
  for (const block of result.content) {
    if (isRecord(block) && block.type === "text" && typeof block.text === "string") {
      try {
        const health: unknown = JSON.parse(block.text);
        if (isRecord(health) && typeof health.projectDir === "string" && health.projectDir.length > 0) {
          return health.projectDir;
        }
      } catch {
        // Not the health JSON.
      }
    }
  }
  return undefined;
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
 * opened again when the editor no longer knows it (the editor restarted) or the target changed. Requests and
 * notifications are forwarded only once the session is ready: opened and, for a known project, the editor checked to
 * serve it. Any failure on the way leaves no session behind, so the next request starts over.
 */
export class EditorConnection {
  private target?: EditorTarget;
  private sessionId?: string;
  private protocolVersion?: string;
  private opening?: Promise<void>;
  private ready = false;
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
      try {
        result = await this.post(message);
      } catch (error) {
        this.reset();
        throw error;
      }
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

  /** Forwards a notification when the session is ready. */
  async notify(message: JsonRpcMessage): Promise<void> {
    if (this.ready) {
      await this.post(message).catch(() => this.reset());
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
    this.ready = false;
    this.target = undefined;
    this.sessionId = undefined;
    this.protocolVersion = undefined;
  }

  private isSessionLost(result: PostResult): boolean {
    const code = result.message?.error?.code;
    return (result.status === 404 && code === -32001) || (result.status === 400 && /Mcp-Session-Id/.test(result.message?.error?.message ?? ""));
  }

  private open(): Promise<void> {
    // While the session is being opened (and the editor checked), requests wait for that to finish.
    if (this.ready) {
      return Promise.resolve();
    }
    this.opening ??= this.initialize().finally(() => {
      this.opening = undefined;
    });
    return this.opening;
  }

  private async initialize(): Promise<void> {
    try {
      await this.openSession();
      this.ready = true;
    } catch (error) {
      // Whatever failed (a refusal, a mismatch, a connection cut while reading an answer), no session stays usable.
      await this.close();
      this.reset();
      throw error instanceof EditorUnreachableError || error instanceof EditorRefusedError
        ? error
        : new EditorUnreachableError(`Opening the session with the editor failed: ${error instanceof Error ? error.message : String(error)}`);
    }
  }

  private async openSession(): Promise<void> {
    this.target = this.resolveTarget();
    if (this.target.unavailable) {
      const reason = this.target.unavailable;
      this.reset();
      throw new EditorUnreachableError(reason);
    }
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
    if (this.target?.project) {
      await this.verifyProject(this.target);
    }
  }

  /** Confirms through health_check that the editor serves the expected project; otherwise ends the session and refuses. */
  private async verifyProject(target: EditorTarget): Promise<void> {
    const expected = target.project!;
    const result = await this.post({ jsonrpc: "2.0", id: `bridge-${this.nextId++}`, method: "tools/call", params: { name: "health_check", arguments: {} } });
    const reported = projectDirOf(result.message?.result);
    if (reported !== undefined && sameProjectDirectory(expected, reported)) {
      return;
    }
    await this.close();
    const answer =
      reported !== undefined
        ? `serves the project ${reported}`
        : `did not say which project it serves (health_check: ${result.message?.error?.message ?? result.refusal ?? `HTTP ${result.status}`})`;
    throw new EditorIdentityError(
      `The editor at ${target.mcpUrl} (found through ${target.source}) ${answer}, not the project ${expected}. The bridge ` +
        "forwards no request to it. Start the editor of that project, or correct UE_PROJECT or UE_API_BASE; when several " +
        "editors run, give each project its own port.",
    );
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
      throw new EditorUnreachableError(describeConnectionFailure(target.mcpUrl, error, target.source));
    }

    const sessionId = response.headers.get("mcp-session-id") ?? undefined;
    let body: string;
    try {
      body = await response.text();
    } catch (error) {
      const reason = [error instanceof Error ? error.message : String(error), connectionErrorCode(error)].filter(Boolean).join(": ");
      throw new EditorUnreachableError(
        `The connection to the editor at ${target.mcpUrl} was cut while it answered (${reason}). The request may or may not ` +
          "have run: check its effect (health_check, then the asset or level it changes) before sending it again.",
      );
    }
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
