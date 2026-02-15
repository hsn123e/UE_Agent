import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import dotenv from "dotenv";

dotenv.config();

function readTextFileTrim(filePath) {
  try {
    return fs.readFileSync(filePath, "utf-8").trim();
  } catch {
    return null;
  }
}

export function getConfig() {
  const llmBaseUrl = process.env.LLM_BASE_URL?.trim();
  const llmApiKey = process.env.LLM_API_KEY?.trim();
  const llmModel = process.env.LLM_MODEL?.trim();

  if (!llmBaseUrl) throw new Error("Missing LLM_BASE_URL");
  if (!llmModel) throw new Error("Missing LLM_MODEL");

  const agentHost = process.env.AGENT_HOST?.trim() || "127.0.0.1";
  const agentPort = Number.parseInt(process.env.AGENT_PORT || "32123", 10);

  const rcPortEnv = process.env.UE_RC_PORT?.trim();
  const rcPortFile = process.env.UE_RC_PORT_FILE?.trim();
  let rcPort = Number.parseInt(rcPortEnv || "", 10);
  if (!Number.isFinite(rcPort)) rcPort = NaN;

  if (!Number.isFinite(rcPort) && rcPortFile) {
    const value = readTextFileTrim(rcPortFile);
    const parsed = Number.parseInt(value || "", 10);
    if (Number.isFinite(parsed)) rcPort = parsed;
  }

  if (!Number.isFinite(rcPort)) rcPort = 30010;

  const bridgePortEnv = process.env.UE_BRIDGE_PORT?.trim();
  let bridgePort = Number.parseInt(bridgePortEnv || "", 10);
  if (!Number.isFinite(bridgePort)) bridgePort = 30020;
  const bridgeToken = process.env.UE_BRIDGE_TOKEN?.trim() || null;

  return {
    agent: { host: agentHost, port: agentPort },
    llm: {
      baseUrl: llmBaseUrl.replace(/\/+$/, ""),
      apiKey: llmApiKey || null,
      model: llmModel
    },
    unreal: {
      remoteControl: {
        port: rcPort,
        portFile: rcPortFile ? path.resolve(rcPortFile) : null
      },
      bridge: {
        port: bridgePort,
        token: bridgeToken,
        baseUrl: `http://127.0.0.1:${bridgePort}`
      }
    }
  };
}
