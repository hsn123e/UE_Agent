import fs from "node:fs";
import path from "node:path";
import process from "node:process";

export function getMcpConfigPath() {
  const p = process.env.MCP_CONFIG_PATH?.trim();
  if (p) return path.resolve(p);
  return path.resolve(process.cwd(), ".mcp-config.json");
}

export function loadMcpConfig() {
  const configPath = getMcpConfigPath();
  if (!fs.existsSync(configPath)) return { configPath, servers: {} };
  const raw = fs.readFileSync(configPath, "utf-8");
  const json = JSON.parse(raw);
  return { configPath, servers: json || {} };
}

