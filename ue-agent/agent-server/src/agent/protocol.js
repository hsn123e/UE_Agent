import Ajv from "ajv";

const ajv = new Ajv({ allErrors: true, strict: false });

const agentResponseSchema = {
  type: "object",
  additionalProperties: false,
  properties: {
    type: { enum: ["final", "tool_call"] },
    message: { type: "string" },
    tool: { type: "string" },
    input: { type: "object" }
  },
  required: ["type"],
  allOf: [
    {
      if: { properties: { type: { const: "final" } } },
      then: { required: ["message"] }
    },
    {
      if: { properties: { type: { const: "tool_call" } } },
      then: { required: ["tool", "input"] }
    }
  ]
};

const validateAgentResponse = ajv.compile(agentResponseSchema);

export function parseAgentJsonResponse(text) {
  let json;
  try {
    json = JSON.parse(text);
  } catch (err) {
    return {
      ok: false,
      error: `Model output is not valid JSON: ${String(err)}`
    };
  }

  const valid = validateAgentResponse(json);
  if (!valid) {
    return {
      ok: false,
      error: "Model JSON does not match required protocol",
      validationErrors: validateAgentResponse.errors,
      json
    };
  }

  return { ok: true, json };
}

