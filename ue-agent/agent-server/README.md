# ue-agent-server (اختياري)

خادم محلي (Node.js) لتشغيل “وكيل” خارج Unreal يمكنه:

- الاتصال بأي مزوّد LLM عبر واجهة OpenAI-compatible.
- استدعاء أدوات `UEAgentBridge` عبر `localhost` (HTTP).

> إذا كان هدفك فقط استخدام الواجهة داخل المحرر (`Window → UE Agent`) فقد لا تحتاج هذا الخادم.

## تشغيل

```powershell
cd ue-agent/agent-server
copy .env.example .env
npm install
npm run dev
```

## API (مختصر)

- `GET /health`
- `GET /tools`
- `POST /tools/execute` body: `{ "toolName": "...", "input": {...} }`
- `POST /agent/chat` body: `{ "text": "..." }`

## ربط UEAgentBridge

إذا كانت إضافة Unreal `UEAgentBridge` شغّالة على `UE_AGENT_BRIDGE_PORT` (افتراضي `30020`)،
فالخادم يستطيع جلب `/ue-agent/tools` وتسجيلها كأدوات باسم:

- `ue.<toolName>` (مثال: `ue.project.get_name`)


