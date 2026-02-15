import Ajv from "ajv";
import addFormats from "ajv-formats";

export function createToolRegistry() {
  const ajv = new Ajv({ allErrors: true, strict: false });
  addFormats(ajv);

  const tools = new Map(); // name -> { name, description, inputSchema, validate, execute }

  function registerTool({ name, description, inputSchema, execute }) {
    if (!name) throw new Error("Tool missing name");
    const schema = inputSchema || { type: "object" };
    const validate = ajv.compile(schema);
    tools.set(name, { name, description: description || "", inputSchema: schema, validate, execute });
  }

  function list() {
    return Array.from(tools.values()).map((t) => ({
      name: t.name,
      description: t.description,
      inputSchema: t.inputSchema
    }));
  }

  function get(name) {
    return tools.get(name) || null;
  }

  async function executeTool({ toolName, input }) {
    const tool = get(toolName);
    if (!tool) return { ok: false, error: `Unknown tool: ${toolName}` };

    const valid = tool.validate(input ?? {});
    if (!valid) {
      return { ok: false, error: "Tool input validation failed", validationErrors: tool.validate.errors };
    }

    try {
      const result = await tool.execute(input ?? {});
      return { ok: true, result };
    } catch (err) {
      return { ok: false, error: err instanceof Error ? err.message : String(err), details: err?.details };
    }
  }

  return { registerTool, list, get, executeTool };
}

