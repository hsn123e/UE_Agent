export async function executeTool({ catalog, handlers, toolName, input }) {
  const tool = catalog.get(toolName);
  if (!tool) {
    return { ok: false, error: `Unknown tool: ${toolName}` };
  }

  const valid = tool.validate(input ?? {});
  if (!valid) {
    return {
      ok: false,
      error: "Tool input validation failed",
      validationErrors: tool.validate.errors
    };
  }

  const handler = handlers[toolName];
  if (!handler) {
    return { ok: false, error: `No handler implemented for tool: ${toolName}` };
  }

  try {
    const result = await handler(input ?? {});
    return { ok: true, result };
  } catch (err) {
    return {
      ok: false,
      error: err instanceof Error ? err.message : String(err),
      details: err?.details
    };
  }
}

