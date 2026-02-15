import process from "node:process";

export function createToolHandlers({ config }) {
  async function ueRcCall(input) {
    const rcPort = input.rcPort ?? config.unreal.remoteControl.port;
    const url = `http://127.0.0.1:${rcPort}/remote/object/call`;
    const body = {
      ObjectPath: input.objectPath,
      FunctionName: input.functionName,
      Parameters: input.parameters ?? null
    };

    const res = await fetch(url, {
      method: "PUT",
      headers: {
        "Content-Type": "application/json",
        Connection: "keep-alive"
      },
      body: JSON.stringify(body)
    });

    const text = await res.text();
    let json;
    try {
      json = text ? JSON.parse(text) : null;
    } catch {
      json = { raw: text };
    }

    if (!res.ok) {
      const message = `RemoteControl call failed: ${res.status} ${res.statusText}`;
      const err = new Error(message);
      err.details = { url, body, response: json };
      throw err;
    }

    return json;
  }

  async function ueRcGetPort() {
    return {
      port: config.unreal.remoteControl.port,
      portFile: config.unreal.remoteControl.portFile
    };
  }

  async function ueBridgeHealth(input) {
    const port = input.port ?? config.unreal.bridge.port;
    const url = `http://127.0.0.1:${port}/ue-agent/health`;
    const headers = {};
    if (config.unreal.bridge.token) headers.Authorization = `Bearer ${config.unreal.bridge.token}`;

    const res = await fetch(url, { method: "GET", headers });
    const text = await res.text();
    let json;
    try {
      json = text ? JSON.parse(text) : null;
    } catch {
      json = { raw: text };
    }
    if (!res.ok) throw new Error(`UE bridge health failed: ${res.status} ${res.statusText}`);
    return json;
  }

  async function ueBridgeCallTool(input) {
    const url = `${config.unreal.bridge.baseUrl}/ue-agent/tools/call`;
    const headers = { "Content-Type": "application/json" };
    if (config.unreal.bridge.token) headers.Authorization = `Bearer ${config.unreal.bridge.token}`;

    const res = await fetch(url, {
      method: "POST",
      headers,
      body: JSON.stringify({ toolName: input.toolName, input: input.input ?? {} })
    });
    const text = await res.text();
    let json;
    try {
      json = text ? JSON.parse(text) : null;
    } catch {
      json = { raw: text };
    }
    if (!res.ok) {
      const err = new Error(`UE bridge tool call failed: ${res.status} ${res.statusText}`);
      err.details = { response: json };
      throw err;
    }
    return json;
  }

  async function systemInfo() {
    return {
      node: process.version,
      platform: process.platform,
      arch: process.arch
    };
  }

  return {
    "ue.rc.call": ueRcCall,
    "ue.rc.get_port": ueRcGetPort,
    "ue.bridge.health": ueBridgeHealth,
    "ue.bridge.call_tool": ueBridgeCallTool,
    "system.info": systemInfo
  };
}
