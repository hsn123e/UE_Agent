export async function fetchBridgeTools({ baseUrl, token }) {
  const url = `${baseUrl}/ue-agent/tools`;
  const headers = {};
  if (token) headers.Authorization = `Bearer ${token}`;

  const res = await fetch(url, { method: "GET", headers });
  const text = await res.text();
  if (!res.ok) {
    throw new Error(`UE bridge tools fetch failed: ${res.status} ${res.statusText}: ${text.slice(0, 500)}`);
  }
  const json = JSON.parse(text);
  const tools = Array.isArray(json?.tools) ? json.tools : [];
  return tools;
}

export function registerBridgeTools({ registry, config, tools }) {
  for (const t of tools) {
    if (!t?.name) continue;
    const toolName = `ue.${t.name}`;
    registry.registerTool({
      name: toolName,
      description: `[UEBridge] ${t.description || ""}`.trim(),
      inputSchema: t.inputSchema || { type: "object" },
      execute: async (input) => {
        const url = `${config.unreal.bridge.baseUrl}/ue-agent/tools/call`;
        const headers = { "Content-Type": "application/json" };
        if (config.unreal.bridge.token) headers.Authorization = `Bearer ${config.unreal.bridge.token}`;

        const res = await fetch(url, {
          method: "POST",
          headers,
          body: JSON.stringify({ toolName: t.name, input: input ?? {} })
        });
        const text = await res.text();
        let json;
        try {
          json = text ? JSON.parse(text) : null;
        } catch {
          json = { raw: text };
        }
        if (!res.ok) throw new Error(`UE bridge call failed: ${res.status} ${res.statusText}`);
        return json;
      }
    });
  }
}

