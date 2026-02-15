import fs from "node:fs";
import path from "node:path";
import Ajv from "ajv";
import addFormats from "ajv-formats";

export function loadToolCatalog() {
  const catalogPath = new URL("./catalog.json", import.meta.url);
  const raw = fs.readFileSync(catalogPath, "utf-8");
  const json = JSON.parse(raw);

  const ajv = new Ajv({ allErrors: true, strict: false });
  addFormats(ajv);

  const tools = new Map();

  for (const tool of json.tools || []) {
    if (!tool?.name) continue;
    const validate = ajv.compile(tool.inputSchema || { type: "object" });
    tools.set(tool.name, { ...tool, validate });
  }

  return {
    tools,
    list() {
      return Array.from(tools.values()).map((t) => ({
        name: t.name,
        description: t.description,
        inputSchema: t.inputSchema
      }));
    },
    get(name) {
      return tools.get(name) || null;
    }
  };
}

