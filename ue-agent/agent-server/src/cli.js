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
    // optional
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

const userText = process.argv.slice(2).join(" ").trim();
if (!userText) {
  console.error('Usage: npm run cli -- "your request"');
  process.exit(2);
}

const out = await runAgentLoop({ config, registry, userText });
if (mcp) await mcp.close();
if (!out.ok) {
  console.error(JSON.stringify(out, null, 2));
  process.exit(1);
}

console.log(out.message);
