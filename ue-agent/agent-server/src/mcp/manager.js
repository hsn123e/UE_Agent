import process from "node:process";
import { EventEmitter } from "node:events";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

export class McpClientManager extends EventEmitter {
  constructor({ servers }) {
    super();
    this.servers = servers || {};
    this.clients = new Map(); // name -> { client, info, tools }
  }

  async init() {
    const entries = Object.entries(this.servers);
    for (const [name, serverConfig] of entries) {
      await this.addClient(name, serverConfig);
    }
  }

  async addClient(name, serverConfig) {
    const client = new Client({ name: `ue-agent:${name}`, version: "0.1.0" });
    const transport = new StdioClientTransport({
      command: serverConfig.command,
      args: serverConfig.args || [],
      env: { ...process.env, ...(serverConfig.env || {}) },
      cwd: serverConfig.cwd || process.cwd()
    });

    await client.connect(transport);
    const listed = await client.listTools();

    const tools = (listed?.tools || []).map((t) => ({
      clientName: name,
      toolName: t.name,
      fullName: `${name}_${t.name}`,
      description: t.description || "",
      inputSchema: t.inputSchema || { type: "object" }
    }));

    const info = { name, status: "connected", toolCount: tools.length };
    this.clients.set(name, { client, info, tools });
    this.emit("connected", info);
  }

  listTools() {
    const out = [];
    for (const c of this.clients.values()) out.push(...c.tools);
    return out;
  }

  getClientByToolFullName(fullToolName) {
    const idx = fullToolName.indexOf("_");
    if (idx <= 0) return null;
    const clientName = fullToolName.slice(0, idx);
    const toolName = fullToolName.slice(idx + 1);
    const entry = this.clients.get(clientName);
    if (!entry) return null;
    return { clientName, toolName, client: entry.client };
  }

  async callTool(fullToolName, input) {
    const resolved = this.getClientByToolFullName(fullToolName);
    if (!resolved) throw new Error(`MCP tool not found: ${fullToolName}`);
    const { client, toolName } = resolved;
    return client.callTool({ name: toolName, arguments: input ?? {} });
  }

  async close() {
    const entries = Array.from(this.clients.values());
    this.clients.clear();
    await Promise.allSettled(entries.map((e) => e.client.close()));
  }
}

