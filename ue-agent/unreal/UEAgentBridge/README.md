# UEAgentBridge (Unreal Editor Plugin)

## العربية

إضافة Editor توفر:

- واجهة داخل المحرر: `Window → UE Agent`
- HTTP Bridge محلي على `localhost`

### التثبيت

انسخ هذا المجلد إلى مشروعك:

`ue-agent/unreal/UEAgentBridge` → `<YourProject>/Plugins/UEAgentBridge`

ثم فعّل الإضافة من:

`Edit → Plugins → UEAgentBridge`

### إعدادات HTTP Bridge

متغيرات البيئة (اختياري):

- `UE_AGENT_BRIDGE_PORT` (افتراضي: `30020`)
- `UE_AGENT_BRIDGE_TOKEN` (اختياري): عند ضبطه يجب إرسال
  `Authorization: Bearer <token>` مع كل طلب

### Endpoints

- `GET /ue-agent/health`
- `GET /ue-agent/tools`
- `POST /ue-agent/tools/call`

---

## English

Editor plugin that provides:

- In-editor UI: `Window → UE Agent`
- Local HTTP bridge on `localhost`

### Install

Copy this folder into your project:

`ue-agent/unreal/UEAgentBridge` → `<YourProject>/Plugins/UEAgentBridge`

Enable it in:

`Edit → Plugins → UEAgentBridge`

### HTTP bridge settings

Environment variables (optional):

- `UE_AGENT_BRIDGE_PORT` (default: `30020`)
- `UE_AGENT_BRIDGE_TOKEN` (optional): if set, send
  `Authorization: Bearer <token>` on every request

### Endpoints

- `GET /ue-agent/health`
- `GET /ue-agent/tools`
- `POST /ue-agent/tools/call`


