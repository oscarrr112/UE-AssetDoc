#!/usr/bin/env node
// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// ue-assetdoc: the stdio bridge to the MCP endpoint the Unreal Editor serves itself (http://127.0.0.1:<port>/mcp).
// MCP clients that speak Streamable HTTP connect to the editor directly; stdio-only clients start this bridge.

import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import type { JSONRPCMessage } from "@modelcontextprotocol/sdk/types.js";
import { StdioBridge } from "./bridge/bridge.js";
import type { JsonRpcMessage } from "./bridge/editorConnection.js";
import { loadManifest } from "./bridge/manifest.js";

async function main(): Promise<void> {
  const transport = new StdioServerTransport();
  const bridge = new StdioBridge(loadManifest(), (message) => {
    void transport.send(message as JSONRPCMessage).catch((error) => console.error(error));
  });
  transport.onmessage = (message) => {
    void bridge.handle(message as JsonRpcMessage).catch((error) => console.error(error));
  };
  transport.onerror = (error) => console.error(error);
  let closing = false;
  const shutdown = () => {
    if (closing) {
      return;
    }
    closing = true;
    void bridge.connection.close().finally(() => process.exit(0));
  };
  process.stdin.on("end", shutdown);
  process.stdin.on("close", shutdown);
  await transport.start();
  console.error("ue-assetdoc: stdio bridge to the Unreal Editor's MCP endpoint");
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
