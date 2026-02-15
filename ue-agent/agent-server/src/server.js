import Fastify from "fastify";
import { getConfig } from "./config.js";
import { loadToolCatalog } from "./tools/catalog.js";
import { createToolHandlers } from "./tools/handlers.js";
import { runAgentLoop } from "./agent/loop.js";
import { createToolRegistry } from "./tools/registry.js";
import { loadMcpConfig } from "./mcp/config.js";
import { McpClientManager } from "./mcp/manager.js";
import { fetchBridgeTools, registerBridgeTools } from "./unreal/bridgeTools.js";

const config = getConfig();
const staticCatalog = loadToolCatalog();
const handlers = createToolHandlers({ config });
const registry = createToolRegistry();

for (const tool of staticCatalog.list()) {
  registry.registerTool({
    name: tool.name,
    description: tool.description,
    inputSchema: tool.inputSchema,
    execute: async (input) => {
      const handler = handlers[tool.name];
      if (!handler) throw new Error(`No handler implemented for tool: ${tool.name}`);
      return handler(input ?? {});
    }
  });
}

if ((process.env.UE_BRIDGE_ENABLE || "1").trim() === "1") {
  try {
    const tools = await fetchBridgeTools({
      baseUrl: config.unreal.bridge.baseUrl,
      token: config.unreal.bridge.token
    });
    registerBridgeTools({ registry, config, tools });
  } catch (err) {
    // Bridge is optional; ignore if not running
    void err;
  }
}

let mcp = null;
if ((process.env.MCP_ENABLE || "").trim() === "1") {
  const { servers } = loadMcpConfig();
  mcp = new McpClientManager({ servers });
  await mcp.init();
  for (const t of mcp.listTools()) {
    registry.registerTool({
      name: t.fullName,
      description: `[MCP:${t.clientName}] ${t.description}`,
      inputSchema: t.inputSchema,
      execute: async (input) => mcp.callTool(t.fullName, input ?? {})
    });
  }
}

const app = Fastify({ logger: true });

app.get("/health", async () => ({ ok: true }));

app.get("/tools", async () => ({ tools: registry.list() }));

app.post("/tools/execute", async (req, reply) => {
  const { toolName, input } = req.body || {};
  if (typeof toolName !== "string" || toolName.length === 0) {
    return reply.code(400).send({ ok: false, error: "Missing toolName" });
  }

  const result = await registry.executeTool({ toolName, input });
  return reply.send(result);
});

app.post("/agent/chat", async (req, reply) => {
  const { text, maxSteps } = req.body || {};
  if (typeof text !== "string" || text.trim().length === 0) {
    return reply.code(400).send({ ok: false, error: "Missing text" });
  }

  const out = await runAgentLoop({
    config,
    registry,
    userText: text,
    maxSteps: Number.isFinite(maxSteps) ? maxSteps : 6
  });

  return reply.send(out);
});

app.listen({ host: config.agent.host, port: config.agent.port }).catch((err) => {
  app.log.error(err);
  process.exit(1);
});

process.on("SIGINT", async () => {
  if (mcp) await mcp.close();
  process.exit(0);
});
