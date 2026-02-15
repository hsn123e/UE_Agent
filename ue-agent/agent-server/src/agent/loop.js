import { openAICompatibleChatCompletion } from "../llm/openaiCompatible.js";
import { parseAgentJsonResponse } from "./protocol.js";
import { buildSystemPrompt } from "./systemPrompt.js";

export async function runAgentOnce({ config, registry, userText }) {
  const tools = registry.list();
  const system = buildSystemPrompt({ tools });

  const messages = [
    { role: "system", content: system },
    { role: "user", content: userText }
  ];

  const { content } = await openAICompatibleChatCompletion({
    baseUrl: config.llm.baseUrl,
    apiKey: config.llm.apiKey,
    model: config.llm.model,
    messages
  });

  const parsed = parseAgentJsonResponse(content);
  if (!parsed.ok) {
    return {
      ok: false,
      error: parsed.error,
      modelOutput: content,
      validationErrors: parsed.validationErrors
    };
  }

  const cmd = parsed.json;
  if (cmd.type === "final") {
    return { ok: true, type: "final", message: cmd.message };
  }

  const toolResult = await registry.executeTool({ toolName: cmd.tool, input: cmd.input });

  return { ok: true, type: "tool_result", toolCall: cmd, toolResult };
}

export async function runAgentLoop({ config, registry, userText, maxSteps = 6 }) {
  let currentText = userText;
  const transcript = [];

  for (let step = 1; step <= maxSteps; step++) {
    const out = await runAgentOnce({ config, registry, userText: currentText });
    transcript.push(out);

    if (!out.ok) return { ok: false, transcript, error: out.error };
    if (out.type === "final") return { ok: true, transcript, message: out.message };

    // Feed tool result back to the model in next step
    const toolName = out.toolCall.tool;
    const toolResult = out.toolResult;
    currentText = [
      "Tool result received.",
      `Tool: ${toolName}`,
      `Result: ${JSON.stringify(toolResult).slice(0, 6000)}`,
      "",
      "Now produce the next JSON response following the protocol."
    ].join("\n");
  }

  return { ok: false, transcript, error: `Max steps (${maxSteps}) reached without final answer` };
}
