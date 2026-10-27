// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

import test from "node:test";
import assert from "node:assert/strict";
import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { createServer, type IncomingHttpHeaders, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createInterface } from "node:readline";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { ListResourcesResultSchema } from "@modelcontextprotocol/sdk/types.js";

import { describeConnectionFailure } from "./editorConnection.js";
import { listedTools, loadManifest, PACKAGE_DIR } from "./manifest.js";

const SERVER_ENTRY = join(PACKAGE_DIR, "dist", "index.js");
const manifest = loadManifest();

let scratch: string;

test.before(() => {
  scratch = mkdtempSync(join(tmpdir(), "ue-assetdoc-bridge-"));
});

test.after(() => {
  rmSync(scratch, { recursive: true, force: true });
});

let counter = 0;

function emptyDirectory(): string {
  const directory = join(scratch, `dir-${counter++}`);
  mkdirSync(directory, { recursive: true });
  return directory;
}

type JsonObject = Record<string, any>;

type Recorded = { method: string; url: string; headers: IncomingHttpHeaders; body?: JsonObject };

type Hook = (request: Recorded, response: ServerResponse) => boolean;

const STUB_PROTOCOL_VERSION = "2025-06-18";
const STUB_TOOLS = [
  { name: "stub_echo", description: "Echoes its arguments.", inputSchema: { type: "object", properties: { text: { type: "string" } } } },
  { name: "stub_other", description: "Another tool.", inputSchema: { type: "object", properties: {} } },
];

function sendJson(response: ServerResponse, status: number, body: unknown, headers: Record<string, string> = {}): void {
  response.writeHead(status, { "Content-Type": "application/json", ...headers });
  response.end(JSON.stringify(body));
}

/** A minimal MCP Streamable HTTP endpoint at /mcp that records every request. */
class StubEditor {
  readonly requests: Recorded[] = [];
  readonly sessions = new Set<string>();
  hook?: Hook;
  // The project directory its health_check reports, as the editor's does (absolute, with a trailing slash).
  projectDir?: string;
  // How long its health_check takes to answer.
  healthDelayMs = 0;
  private issued = 0;
  private constructor(
    private readonly server: Server,
    readonly port: number,
  ) {}

  static async start(): Promise<StubEditor> {
    let stub: StubEditor | undefined;
    const server = createServer((request, response) => stub?.handle(request, response));
    await new Promise<void>((resolve, reject) => {
      server.once("error", reject);
      server.listen(0, "127.0.0.1", () => resolve());
    });
    const address = server.address();
    assert.ok(address && typeof address === "object");
    stub = new StubEditor(server, address.port);
    return stub;
  }

  get baseUrl(): string {
    return `http://127.0.0.1:${this.port}`;
  }

  async close(): Promise<void> {
    this.server.closeAllConnections();
    await new Promise<void>((resolve) => this.server.close(() => resolve()));
  }

  /** The recorded JSON-RPC requests (not notifications) of one method. */
  withMethod(method: string): Recorded[] {
    return this.requests.filter((request) => request.method === "POST" && request.body?.method === method);
  }

  /** The recorded tools/call requests of one tool. */
  callsOf(tool: string): Recorded[] {
    return this.withMethod("tools/call").filter((request) => request.body?.params?.name === tool);
  }

  private handle(request: IncomingMessage, response: ServerResponse): void {
    let raw = "";
    request.on("data", (chunk) => {
      raw += chunk;
    });
    request.on("end", () => {
      const recorded: Recorded = { method: request.method ?? "", url: request.url ?? "", headers: request.headers };
      if (raw.length > 0) {
        recorded.body = JSON.parse(raw) as JsonObject;
      }
      this.requests.push(recorded);
      if (this.hook?.(recorded, response)) {
        return;
      }
      this.answer(recorded, response);
    });
  }

  private answer(request: Recorded, response: ServerResponse): void {
    if (request.url !== "/mcp") {
      sendJson(response, 404, { success: false, error: "no such route" });
      return;
    }
    const session = request.headers["mcp-session-id"];
    if (request.method === "DELETE") {
      this.sessions.delete(String(session));
      response.writeHead(204);
      response.end();
      return;
    }
    const body = request.body;
    assert.ok(body, "a POST has a JSON-RPC body");
    if (body.method === "initialize") {
      const id = `session-${++this.issued}`;
      this.sessions.add(id);
      sendJson(
        response,
        200,
        {
          jsonrpc: "2.0",
          id: body.id,
          result: { protocolVersion: STUB_PROTOCOL_VERSION, capabilities: { tools: {} }, serverInfo: { name: "ue-assetdoc", version: "stub" } },
        },
        { "Mcp-Session-Id": id },
      );
      return;
    }
    if (typeof session !== "string" || !this.sessions.has(session)) {
      sendJson(response, 404, { jsonrpc: "2.0", id: body.id ?? null, error: { code: -32001, message: "Session not found" } });
      return;
    }
    if (body.id === undefined) {
      response.writeHead(202);
      response.end();
      return;
    }
    if (body.method === "tools/list") {
      sendJson(response, 200, { jsonrpc: "2.0", id: body.id, result: { tools: STUB_TOOLS } });
    } else if (body.method === "tools/call" && body.params.name === "health_check") {
      const health = { status: "ok", service: "UEAssetDoc", port: this.port, ...(this.projectDir === undefined ? {} : { projectDir: this.projectDir }) };
      const answer = { jsonrpc: "2.0", id: body.id, result: { content: [{ type: "text", text: JSON.stringify(health) }] } };
      setTimeout(() => sendJson(response, 200, answer), this.healthDelayMs);
    } else if (body.method === "tools/call") {
      const text = JSON.stringify({ name: body.params.name, arguments: body.params.arguments });
      sendJson(response, 200, { jsonrpc: "2.0", id: body.id, result: { content: [{ type: "text", text }] } });
    } else {
      sendJson(response, 200, { jsonrpc: "2.0", id: body.id, error: { code: -32601, message: `Method not found: ${body.method}` } });
    }
  }
}

async function withStub(run: (stub: StubEditor) => Promise<void>): Promise<void> {
  const stub = await StubEditor.start();
  try {
    await run(stub);
  } finally {
    await stub.close();
  }
}

/** The base URL of an address nothing listens on. */
async function closedBaseUrl(): Promise<string> {
  const server = createServer();
  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", () => resolve());
  });
  const address = server.address();
  assert.ok(address && typeof address === "object");
  await new Promise<void>((resolve) => server.close(() => resolve()));
  return `http://127.0.0.1:${address.port}`;
}

const CLIENT_NAME = "bridge-test-client";

async function withClient(env: Record<string, string>, run: (client: Client) => Promise<void>): Promise<void> {
  const transport = new StdioClientTransport({ command: process.execPath, args: [SERVER_ENTRY], cwd: emptyDirectory(), env, stderr: "pipe" });
  const client = new Client({ name: CLIENT_NAME, version: "9.9" }, { capabilities: {} });
  try {
    await client.connect(transport);
    await run(client);
  } finally {
    await client.close().catch(() => undefined);
    await transport.close().catch(() => undefined);
  }
}

// The bridge forwards notifications without delaying its own answers, so the editor sees them a moment later.
async function waitFor(condition: () => boolean, what: string): Promise<void> {
  for (let attempt = 0; attempt < 100; attempt++) {
    if (condition()) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  assert.fail(`timed out: ${what}`);
}

function textOf(result: unknown): string {
  assert.ok(typeof result === "object" && result !== null && "content" in result && Array.isArray(result.content));
  return result.content
    .filter((entry): entry is { type: "text"; text: string } => typeof entry === "object" && entry !== null && entry.type === "text" && typeof entry.text === "string")
    .map((entry) => entry.text)
    .join("\n");
}

test("the bridge answers initialize itself with the manifest's server identity", async () => {
  const offline = await closedBaseUrl();
  await withClient({ UE_API_BASE: offline }, async (client) => {
    assert.deepEqual(client.getServerVersion(), manifest.server);
    assert.deepEqual(client.getServerCapabilities(), { tools: {} });
    await client.ping();
  });
});

test("tools/list returns the editor's tools", async () => {
  await withStub(async (stub) => {
    await withClient({ UE_API_BASE: stub.baseUrl }, async (client) => {
      const { tools } = await client.listTools();
      assert.deepEqual(
        tools.map((tool) => tool.name),
        STUB_TOOLS.map((tool) => tool.name),
      );
      assert.equal(tools[0].description, STUB_TOOLS[0].description);
    });
  });
});

test("tools/call is forwarded in the editor session with the negotiated protocol version", async () => {
  await withStub(async (stub) => {
    await withClient({ UE_API_BASE: stub.baseUrl }, async (client) => {
      const result = await client.callTool({ name: "stub_echo", arguments: { text: "hello" } });
      assert.notEqual(result.isError, true);
      assert.deepEqual(JSON.parse(textOf(result)), { name: "stub_echo", arguments: { text: "hello" } });
    });

    const [initialize] = stub.withMethod("initialize");
    assert.ok(initialize, "the bridge opened a session with the editor");
    assert.equal(stub.withMethod("initialize").length, 1);
    assert.equal(initialize.url, "/mcp");
    assert.equal(initialize.body?.params.clientInfo.name, CLIENT_NAME, "the editor sees the client's identity");
    assert.equal(initialize.body?.params.clientInfo.version, "9.9");
    assert.equal(initialize.headers["mcp-session-id"], undefined, "initialize opens the session");
    assert.equal(initialize.headers["mcp-protocol-version"], undefined);
    assert.match(String(initialize.headers.accept), /application\/json/);
    assert.match(String(initialize.headers.accept), /text\/event-stream/);
    assert.match(String(initialize.headers["content-type"]), /application\/json/);

    const initialized = stub.requests.filter((request) => request.body?.method === "notifications/initialized");
    assert.equal(initialized.length, 1, "the bridge's own session is initialized once");
    assert.equal(initialized[0].headers["mcp-session-id"], "session-1");

    const [call] = stub.withMethod("tools/call");
    assert.equal(call.headers["mcp-session-id"], "session-1");
    assert.equal(call.headers["mcp-protocol-version"], STUB_PROTOCOL_VERSION);
    assert.equal(call.headers.authorization, undefined, "no token, no Authorization header");
    assert.equal(call.body?.params.name, "stub_echo");
  });
});

test("later requests reuse the session", async () => {
  await withStub(async (stub) => {
    await withClient({ UE_API_BASE: stub.baseUrl }, async (client) => {
      await client.listTools();
      await client.callTool({ name: "stub_echo", arguments: {} });
      await client.callTool({ name: "stub_other", arguments: {} });
    });
    assert.equal(stub.withMethod("initialize").length, 1);
    for (const request of [...stub.withMethod("tools/list"), ...stub.withMethod("tools/call")]) {
      assert.equal(request.headers["mcp-session-id"], "session-1");
    }
  });
});

test("the token of UE_ASSETDOC_TOKEN arrives as a bearer token on every request", async () => {
  await withStub(async (stub) => {
    await withClient({ UE_API_BASE: `${stub.baseUrl}/mcp`, UE_ASSETDOC_TOKEN: "s3cret" }, async (client) => {
      await client.callTool({ name: "stub_echo", arguments: {} });
    });
    const posts = stub.requests.filter((request) => request.method === "POST");
    assert.ok(posts.length >= 3, "initialize, initialized and the call");
    for (const request of stub.requests) {
      assert.equal(request.headers.authorization, "Bearer s3cret", `${request.method} ${request.body?.method ?? ""}`);
    }
    assert.ok(stub.requests.some((request) => request.method === "DELETE"), "closing the session is authorized too");
  });
});

test("UE_PROJECT's discovery file names the editor's port and token", async () => {
  await withStub(async (stub) => {
    const project = emptyDirectory();
    stub.projectDir = `${project}/`;
    mkdirSync(join(project, "Saved", "UEAssetDoc"), { recursive: true });
    writeFileSync(join(project, "Saved", "UEAssetDoc", "service.json"), JSON.stringify({ port: stub.port, authToken: "from-file" }));
    await withClient({ UE_PROJECT: project }, async (client) => {
      const result = await client.callTool({ name: "stub_echo", arguments: { text: "found" } });
      assert.notEqual(result.isError, true);
    });
    assert.equal(stub.callsOf("stub_echo")[0].headers.authorization, "Bearer from-file");
  });
});

test("a session the editor forgot (404, code -32001) is opened again and the call succeeds", async () => {
  await withStub(async (stub) => {
    await withClient({ UE_API_BASE: stub.baseUrl }, async (client) => {
      await client.listTools();
      stub.sessions.clear();
      const result = await client.callTool({ name: "stub_echo", arguments: { text: "again" } });
      assert.notEqual(result.isError, true);
      assert.deepEqual(JSON.parse(textOf(result)).arguments, { text: "again" });
    });
    assert.equal(stub.withMethod("initialize").length, 2);
    const calls = stub.withMethod("tools/call");
    assert.deepEqual(
      calls.map((call) => call.headers["mcp-session-id"]),
      ["session-1", "session-2"],
      "the refused call and its retry",
    );
  });
});

test("a request without a known session (400 naming Mcp-Session-Id) is retried in a new session", async () => {
  await withStub(async (stub) => {
    let refused = false;
    stub.hook = (request, response) => {
      if (request.body?.method === "tools/call" && !refused) {
        refused = true;
        sendJson(response, 400, { jsonrpc: "2.0", id: request.body.id, error: { code: -32600, message: "Bad Request: Mcp-Session-Id header is required" } });
        return true;
      }
      return false;
    };
    await withClient({ UE_API_BASE: stub.baseUrl }, async (client) => {
      const result = await client.callTool({ name: "stub_echo", arguments: {} });
      assert.notEqual(result.isError, true);
    });
    assert.equal(stub.withMethod("initialize").length, 2);
    assert.equal(stub.withMethod("tools/call").length, 2);
  });
});

test("a 401 is retried once with a new session; a second 401 is a tool error", async () => {
  await withStub(async (stub) => {
    let refusals = 0;
    stub.hook = (request, response) => {
      if (request.body?.method === "tools/call" && refusals < 1) {
        refusals++;
        sendJson(response, 401, { success: false, error: "Missing or invalid token" });
        return true;
      }
      return false;
    };
    await withClient({ UE_API_BASE: stub.baseUrl }, async (client) => {
      const result = await client.callTool({ name: "stub_echo", arguments: {} });
      assert.notEqual(result.isError, true);
    });
    assert.equal(stub.withMethod("initialize").length, 2);
  });

  await withStub(async (stub) => {
    stub.hook = (request, response) => {
      if (request.body?.method === "tools/call") {
        sendJson(response, 401, { success: false, error: "Missing or invalid token" });
        return true;
      }
      return false;
    };
    await withClient({ UE_API_BASE: stub.baseUrl }, async (client) => {
      const result = await client.callTool({ name: "stub_echo", arguments: {} });
      assert.equal(result.isError, true);
      assert.match(textOf(result), /HTTP 401: Missing or invalid token/);
    });
    assert.equal(stub.withMethod("tools/call").length, 2, "retried once, not forever");
  });
});

test("an event-stream answer is parsed", async () => {
  await withStub(async (stub) => {
    stub.hook = (request, response) => {
      if (request.body?.method !== "tools/call") {
        return false;
      }
      const progress = { jsonrpc: "2.0", method: "notifications/message", params: { level: "info", data: "working" } };
      const reply = { jsonrpc: "2.0", id: request.body.id, result: { content: [{ type: "text", text: "from the stream" }] } };
      response.writeHead(200, { "Content-Type": "text/event-stream" });
      response.end(`event: message\ndata: ${JSON.stringify(progress)}\n\nevent: message\ndata: ${JSON.stringify(reply)}\n\n`);
      return true;
    };
    await withClient({ UE_API_BASE: stub.baseUrl }, async (client) => {
      const result = await client.callTool({ name: "stub_echo", arguments: {} });
      assert.notEqual(result.isError, true);
      assert.equal(textOf(result), "from the stream");
    });
  });
});

test("an HTTP refusal that is not a JSON-RPC message becomes a tool error with the editor's text", async () => {
  await withStub(async (stub) => {
    let refused = false;
    stub.hook = (request, response) => {
      if (request.body?.method === "tools/call" && !refused) {
        refused = true;
        sendJson(response, 403, { success: false, error: "Origin http://evil.example is not allowed" });
        return true;
      }
      return false;
    };
    await withClient({ UE_API_BASE: stub.baseUrl }, async (client) => {
      const result = await client.callTool({ name: "stub_echo", arguments: {} });
      assert.equal(result.isError, true);
      assert.match(textOf(result), /Origin http:\/\/evil\.example is not allowed/);
      assert.match(textOf(result), /HTTP 403/);
      // The session is still usable.
      assert.notEqual((await client.callTool({ name: "stub_other", arguments: {} })).isError, true);
    });
  });
});

test("a non-JSON error page becomes a tool error with an excerpt of its text", async () => {
  await withStub(async (stub) => {
    stub.hook = (request, response) => {
      if (request.body?.method === "tools/call") {
        response.writeHead(500, { "Content-Type": "text/plain" });
        response.end("the editor is shutting down");
        return true;
      }
      return false;
    };
    await withClient({ UE_API_BASE: stub.baseUrl }, async (client) => {
      const result = await client.callTool({ name: "stub_echo", arguments: {} });
      assert.equal(result.isError, true);
      assert.match(textOf(result), /HTTP 500/);
      assert.match(textOf(result), /the editor is shutting down/);
    });
  });
});

test("a refusal of tools/list or an unknown method is a JSON-RPC error, an editor error is passed through", async () => {
  await withStub(async (stub) => {
    stub.hook = (request, response) => {
      if (request.body?.method === "tools/list") {
        sendJson(response, 403, { success: false, error: "Origin http://evil.example is not allowed" });
        return true;
      }
      return false;
    };
    await withClient({ UE_API_BASE: stub.baseUrl }, async (client) => {
      await assert.rejects(client.listTools(), /Origin http:\/\/evil\.example is not allowed/);
      await assert.rejects(client.request({ method: "resources/list" }, ListResourcesResultSchema), /Method not found: resources\/list/);
    });
  });
});

test("while the editor is not listening, tools/list answers the manifest's tools and a tool call is a tool error", async () => {
  const offline = await closedBaseUrl();
  await withClient({ UE_API_BASE: offline }, async (client) => {
    const { tools } = await client.listTools();
    assert.equal(tools.length, 33);
    assert.deepEqual(
      tools.map((tool) => tool.name),
      manifest.tools.map((tool) => tool.name),
    );
    assert.deepEqual(tools, listedTools(manifest));

    const result = await client.callTool({ name: "find_assets", arguments: { name: "Hero" } });
    assert.equal(result.isError, true);
    const text = textOf(result);
    assert.match(text, /UEAssetDoc HTTP server is not listening on port \d+/);
    assert.ok(text.includes(`port ${new URL(offline).port} `), "it names the port the bridge tried");
    assert.match(text, /restart the editor/);
    assert.match(text, /UE_API_BASE/, "it says where the address came from");
  });
});

test("after the editor restarted on another port, the bridge finds it again through the discovery file", async () => {
  await withStub(async (first) => {
    await withStub(async (second) => {
      const project = emptyDirectory();
      first.projectDir = second.projectDir = `${project}/`;
      mkdirSync(join(project, "Saved", "UEAssetDoc"), { recursive: true });
      const serviceFile = join(project, "Saved", "UEAssetDoc", "service.json");
      writeFileSync(serviceFile, JSON.stringify({ port: first.port }));
      await withClient({ UE_PROJECT: project }, async (client) => {
        const before = await client.callTool({ name: "stub_echo", arguments: { text: "one" } });
        assert.notEqual(before.isError, true);
        assert.equal(first.callsOf("stub_echo").length, 1);

        // The first editor is gone: it no longer knows the session and the discovery file names the new one.
        first.sessions.clear();
        writeFileSync(serviceFile, JSON.stringify({ port: second.port }));
        const after = await client.callTool({ name: "stub_echo", arguments: { text: "two" } });
        assert.notEqual(after.isError, true);
        assert.deepEqual(JSON.parse(textOf(after)).arguments, { text: "two" });
      });
      assert.equal(second.withMethod("initialize").length, 1);
      assert.equal(second.callsOf("stub_echo").length, 1);
    });
  });
});

/** A project directory whose discovery file names the stub editor's port, with the token when given. */
function projectServedBy(stub: StubEditor, authToken?: string): string {
  const project = emptyDirectory();
  mkdirSync(join(project, "Saved", "UEAssetDoc"), { recursive: true });
  writeFileSync(join(project, "Saved", "UEAssetDoc", "service.json"), JSON.stringify({ port: stub.port, ...(authToken ? { authToken } : {}) }));
  return project;
}

test("a named project whose editor is not running reaches no editor: every call says so", async () => {
  await withStub(async (stub) => {
    const project = emptyDirectory();
    const serviceFile = join(project, "Saved", "UEAssetDoc", "service.json");
    await withClient({ UE_PROJECT: project }, async (client) => {
      const { tools } = await client.listTools();
      assert.deepEqual(tools, listedTools(manifest), "the tools are still listed from the manifest");
      for (const name of ["find_assets", "stub_echo"]) {
        const result = await client.callTool({ name, arguments: {} });
        assert.equal(result.isError, true, name);
        const text = textOf(result);
        assert.match(text, /is not running/, text);
        assert.ok(text.includes(project), "it names the project");
        assert.ok(text.includes(serviceFile), "it names the missing discovery file");
        assert.ok(!/port 8559/.test(text), "it tried no default port");
      }
      // The editor starts later: the next call finds it.
      stub.projectDir = `${project}/`;
      mkdirSync(join(project, "Saved", "UEAssetDoc"), { recursive: true });
      writeFileSync(serviceFile, JSON.stringify({ port: stub.port }));
      const later = await client.callTool({ name: "stub_echo", arguments: { text: "up" } });
      assert.notEqual(later.isError, true, textOf(later));
    });
    assert.equal(stub.callsOf("stub_echo").length, 1);
  });
});

test("an editor serving another project is refused before any request is forwarded to it", async () => {
  await withStub(async (stub) => {
    const project = projectServedBy(stub, "from-file");
    const other = emptyDirectory();
    stub.projectDir = `${other}/`;
    await withClient({ UE_PROJECT: project }, async (client) => {
      const result = await client.callTool({ name: "stub_echo", arguments: { text: "must not arrive" } });
      assert.equal(result.isError, true);
      const text = textOf(result);
      assert.ok(text.includes(project), "it names the project asked for");
      assert.ok(text.includes(other), "it names the project the editor serves");
      assert.ok(text.includes(String(stub.port)), "it names the editor's address");
      assert.equal((await client.callTool({ name: "stub_other", arguments: {} })).isError, true, "every call is refused");
      // A call that arrives while the bridge is still checking the editor (the session is open) waits for the check.
      stub.healthDelayMs = 300;
      const checks = stub.callsOf("health_check").length;
      const first = client.callTool({ name: "stub_echo", arguments: { text: "first" } });
      await waitFor(() => stub.callsOf("health_check").length > checks, "the bridge checks the editor again");
      const during = client.callTool({ name: "stub_echo", arguments: { text: "during the check" } });
      assert.equal((await first).isError, true);
      assert.equal((await during).isError, true);
    });
    assert.equal(stub.callsOf("stub_echo").length, 0);
    assert.equal(stub.callsOf("stub_other").length, 0);
    assert.ok(stub.requests.some((request) => request.method === "DELETE"), "the session with the wrong editor is closed");
  });
});

test("UE_API_BASE with UE_PROJECT: an editor serving another project is refused too", async () => {
  await withStub(async (stub) => {
    const project = emptyDirectory();
    stub.projectDir = `${emptyDirectory()}/`;
    await withClient({ UE_API_BASE: stub.baseUrl, UE_PROJECT: project }, async (client) => {
      const result = await client.callTool({ name: "stub_echo", arguments: {} });
      assert.equal(result.isError, true);
      assert.ok(textOf(result).includes(project));
    });
    assert.equal(stub.callsOf("stub_echo").length, 0);
    stub.projectDir = `${project}/`;
    await withClient({ UE_API_BASE: stub.baseUrl, UE_PROJECT: project }, async (client) => {
      assert.notEqual((await client.callTool({ name: "stub_echo", arguments: {} })).isError, true, "the right project is served");
    });
    assert.equal(stub.callsOf("stub_echo").length, 1);
  });
});

test("an editor that does not say which project it serves is refused for a named project", async () => {
  await withStub(async (stub) => {
    const project = projectServedBy(stub);
    await withClient({ UE_PROJECT: project }, async (client) => {
      const result = await client.callTool({ name: "stub_echo", arguments: {} });
      assert.equal(result.isError, true);
      assert.match(textOf(result), /project/);
    });
    assert.equal(stub.callsOf("stub_echo").length, 0);
  });
});

test("a connection that drops while the editor is checked leaves no usable session: nothing is forwarded unchecked", async () => {
  await withStub(async (stub) => {
    const project = projectServedBy(stub);
    stub.projectDir = `${project}/`;
    let drop = true;
    stub.hook = (request, response) => {
      if (!drop || request.body?.method !== "tools/call" || request.body?.params?.name !== "health_check") {
        return false;
      }
      // The answer's headers arrive, then the connection is cut in the middle of its body.
      response.writeHead(200, { "Content-Type": "application/json", "Content-Length": "4096" });
      response.write('{"jsonrpc":"2.0","id":');
      setTimeout(() => response.socket?.destroy(), 20);
      return true;
    };
    await withClient({ UE_PROJECT: project }, async (client) => {
      const outcomes: string[] = [];
      for (const text of ["first", "second"]) {
        try {
          const result = await client.callTool({ name: "stub_echo", arguments: { text } });
          outcomes.push(result.isError === true ? "tool error" : "forwarded");
        } catch (error) {
          outcomes.push(`JSON-RPC error: ${error instanceof Error ? error.message : String(error)}`);
        }
      }
      assert.equal(stub.callsOf("stub_echo").length, 0, `no call reached the editor without a completed check (${outcomes.join("; ")})`);
      assert.deepEqual(outcomes, ["tool error", "tool error"], "each call answers a tool error saying the editor could not be reached");
      assert.ok(stub.callsOf("health_check").length >= 2, "each attempt checks the editor again");
      // The connection holds again: the next call checks the editor and goes through.
      drop = false;
      const later = await client.callTool({ name: "stub_echo", arguments: { text: "checked" } });
      assert.notEqual(later.isError, true, textOf(later));
    });
    assert.equal(stub.callsOf("stub_echo").length, 1);
  });
});

/** The bridge as a bare process speaking newline-delimited JSON-RPC. */
class RawBridge {
  private readonly child: ChildProcessWithoutNullStreams;
  private readonly lines: string[] = [];
  private waiting?: (line: string) => void;
  readonly exited: Promise<number | null>;

  constructor(env: Record<string, string>) {
    this.child = spawn(process.execPath, [SERVER_ENTRY], { cwd: emptyDirectory(), env, stdio: "pipe" });
    createInterface({ input: this.child.stdout }).on("line", (line) => {
      if (this.waiting) {
        const resolve = this.waiting;
        this.waiting = undefined;
        resolve(line);
      } else {
        this.lines.push(line);
      }
    });
    this.child.stderr.resume();
    this.exited = new Promise((resolve) => this.child.once("exit", (code) => resolve(code)));
  }

  send(message: JsonObject): void {
    this.child.stdin.write(`${JSON.stringify(message)}\n`);
  }

  async receive(): Promise<JsonObject> {
    const line =
      this.lines.shift() ??
      (await new Promise<string>((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error("the bridge did not answer")), 10_000);
        this.waiting = (value) => {
          clearTimeout(timer);
          resolve(value);
        };
      }));
    return JSON.parse(line) as JsonObject;
  }

  async request(message: JsonObject): Promise<JsonObject> {
    this.send(message);
    return this.receive();
  }

  async closeInput(): Promise<number | null> {
    this.child.stdin.end();
    return this.exited;
  }

  kill(): void {
    this.child.kill();
  }
}

test("initialize negotiates a supported protocol version; ping is answered without the editor", async () => {
  const offline = await closedBaseUrl();
  const bridge = new RawBridge({ UE_API_BASE: offline });
  try {
    const supported = await bridge.request({ jsonrpc: "2.0", id: 1, method: "initialize", params: { protocolVersion: "2025-03-26", capabilities: {}, clientInfo: { name: "raw", version: "1" } } });
    assert.equal(supported.id, 1);
    assert.equal(supported.result.protocolVersion, "2025-03-26");
    assert.deepEqual(supported.result.serverInfo, manifest.server);
    assert.deepEqual(supported.result.capabilities, { tools: {} });

    const unknown = await bridge.request({ jsonrpc: "2.0", id: "x", method: "initialize", params: { protocolVersion: "1999-01-01", capabilities: {}, clientInfo: { name: "raw", version: "1" } } });
    assert.equal(unknown.id, "x");
    assert.equal(unknown.result.protocolVersion, "2025-11-25", "an unknown version gets the newest the bridge supports");

    bridge.send({ jsonrpc: "2.0", method: "notifications/initialized" });
    const pong = await bridge.request({ jsonrpc: "2.0", id: 2, method: "ping" });
    assert.deepEqual(pong, { jsonrpc: "2.0", id: 2, result: {} });
    assert.equal(await bridge.closeInput(), 0, "closing stdin ends the bridge, with no editor to tell");
  } finally {
    bridge.kill();
  }
});

test("the client's own initialize parameters open the editor session; notifications are forwarded; closing stdin deletes the session", async () => {
  await withStub(async (stub) => {
    const bridge = new RawBridge({ UE_API_BASE: stub.baseUrl, UE_ASSETDOC_TOKEN: "tok" });
    try {
      const params = { protocolVersion: "2025-03-26", capabilities: { roots: {} }, clientInfo: { name: "raw-client", version: "3.1" } };
      await bridge.request({ jsonrpc: "2.0", id: 1, method: "initialize", params });
      bridge.send({ jsonrpc: "2.0", method: "notifications/initialized" });
      assert.equal(stub.requests.length, 0, "nothing reaches the editor before the first request that needs it");

      const listed = await bridge.request({ jsonrpc: "2.0", id: 7, method: "tools/list" });
      assert.equal(listed.id, 7);
      assert.deepEqual(listed.result.tools, STUB_TOOLS);
      assert.deepEqual(stub.withMethod("initialize")[0].body?.params, params);

      bridge.send({ jsonrpc: "2.0", method: "notifications/roots/list_changed" });
      const pong = await bridge.request({ jsonrpc: "2.0", id: 8, method: "ping" });
      assert.equal(pong.id, 8);
      await waitFor(
        () => stub.requests.some((request) => request.body?.method === "notifications/roots/list_changed" && request.headers["mcp-session-id"] === "session-1"),
        "other notifications reach the editor",
      );
      assert.equal(
        stub.requests.filter((request) => request.body?.method === "notifications/initialized").length,
        1,
        "the client's initialized notification is not forwarded a second time",
      );
      assert.equal(stub.requests.filter((request) => request.method === "DELETE").length, 0);

      assert.equal(await bridge.closeInput(), 0);
      const deletes = stub.requests.filter((request) => request.method === "DELETE");
      assert.equal(deletes.length, 1, "the session is ended when stdin closes");
      assert.equal(deletes[0].headers["mcp-session-id"], "session-1");
      assert.equal(deletes[0].headers.authorization, "Bearer tok");
      assert.equal(stub.sessions.size, 0);
    } finally {
      bridge.kill();
    }
  });
});

test("describeConnectionFailure names the port, the cause, where to look and what to do", () => {
  const refused = Object.assign(new TypeError("fetch failed"), { cause: { code: "ECONNREFUSED" } });
  const text = describeConnectionFailure("http://localhost:8734/mcp", refused);
  assert.match(text, /UEAssetDoc HTTP server is not listening on port 8734/);
  assert.match(text, /http:\/\/localhost:8734\/mcp/);
  assert.match(text, /fetch failed: ECONNREFUSED/);
  assert.match(text, /editor's log|its log/);
  assert.match(text, /restart the editor/);
  assert.match(text, /UE_API_BASE/);
  assert.doesNotMatch(text, /\(this time:/, "no source, no source note");

  assert.match(describeConnectionFailure("http://localhost/mcp", new Error("boom")), /port 80 /);
  assert.match(describeConnectionFailure("https://editor.example/mcp", new Error("boom")), /port 443 /);
  assert.match(describeConnectionFailure("not a url", "plain failure"), /port \(unknown\) .*plain failure/);
  assert.match(describeConnectionFailure("http://127.0.0.1:8559/mcp", new Error("boom"), "default port"), /\(this time: default port\)/);
});

test("a notification is forwarded only in a session whose editor was checked", async () => {
  await withStub(async (stub) => {
    const project = projectServedBy(stub);
    stub.projectDir = `${emptyDirectory()}/`;
    const bridge = new RawBridge({ UE_PROJECT: project });
    try {
      await bridge.request({ jsonrpc: "2.0", id: 1, method: "initialize", params: { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "raw", version: "1" } } });
      bridge.send({ jsonrpc: "2.0", method: "notifications/initialized" });
      const refused = await bridge.request({ jsonrpc: "2.0", id: 2, method: "tools/call", params: { name: "stub_echo", arguments: {} } });
      assert.equal(refused.result?.isError, true);
      bridge.send({ jsonrpc: "2.0", method: "notifications/cancelled", params: { requestId: 2 } });
      assert.equal((await bridge.request({ jsonrpc: "2.0", id: 3, method: "ping" })).id, 3);
      await new Promise((resolve) => setTimeout(resolve, 100));
      assert.equal(stub.requests.filter((request) => request.body?.method === "notifications/cancelled").length, 0);
    } finally {
      bridge.kill();
    }
  });
});
