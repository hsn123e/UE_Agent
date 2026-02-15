# UE Agent (Aura-inspired) — Unreal Engine Editor Assistant

مشروع يساعدك على التحكم بمحرر Unreal Engine عبر:

- إضافة داخل المحرر `UEAgentBridge` (واجهة دردشة + أدوات Editor/Blueprint/UMG).
- (اختياري) خادم Node.js `agent-server` إذا أردت التحكم من خارج المحرر أو بناء “وكيل” أكثر تقدّمًا.

## المتطلبات

- Unreal Engine 5.7.x (مجرّب على 5.7.2)
- Windows 10/11
- لبناء إضافة C++:
  - Visual Studio Build Tools 2022 + C++ toolchain + Windows 10 SDK
  - .NET 8 (عادةً يأتي مع UE/UBT، لكن تأكد أنه مثبت)

## التثبيت (داخل مشروع Unreal)

1) انسخ الإضافة إلى مشروعك:

`ue-agent/unreal/UEAgentBridge` → `<YourProject>/Plugins/UEAgentBridge`

مثال عندك:

`C:\UE_Agent\ue-agent\unreal\UEAgentBridge` → `C:\Unreal Projects\X555\Plugins\UEAgentBridge`

2) افتح المشروع في Unreal.
- فعّل الإضافة: `Edit → Plugins → UEAgentBridge`
- أعد تشغيل المحرر إذا طُلب ذلك.

3) إذا ظهرت رسالة Compile:
- أغلق المحرر
- ابنِ المشروع/الإضافة (انظر قسم “إعادة البناء”)

## التشغيل (واجهة داخل المحرر)

من داخل Unreal Editor:

`Window → UE Agent`

ستجد:
- قائمة محادثات (New / Delete) مع حفظ تلقائي.
- منطقة محادثة + مربع إدخال:
  - `Enter` يرسل
  - `Shift+Enter` سطر جديد
- زر `Settings` لتهيئة المزوّد (Provider) والموديل والمفتاح.

### إعداد مزوّد LLM

داخل نافذة `Settings`:

**1) OpenAI-compatible**
- `Base URL`: رابط API الذي يدعم `/chat/completions`
- `API Key`: مفتاح المزود
- `Model`: اسم الموديل

**2) Ollama Cloud**
- `Base URL`: `https://ollama.com/api`
- `API Key`: من `ollama.com/settings/keys`
- `Model`: يجب أن يكون من قائمة الموديلات المتاحة لحسابك عبر Cloud API

لجلب أسماء الموديلات المتاحة (PowerShell):

```powershell
$k = "PUT_YOUR_OLLAMA_CLOUD_KEY_HERE"
(Invoke-RestMethod -Uri "https://ollama.com/api/tags" -Headers @{ Authorization = "Bearer $k" }).models |
  Select-Object -ExpandProperty name
```

**Ollama محلي (اختياري)**
- شغّل Ollama محليًا
- `Base URL`: `http://localhost:11434/api`
- `API Key`: اتركها فارغة
- `Model`: اسم موديل موجود محليًا (مثلاً من `GET /api/tags`)

## حفظ المحادثات

المحادثات محفوظة لكل مشروع داخل:

`<Project>/Saved/UEAgentBridge/conversations.json`

مثال:

`C:\Unreal Projects\X555\Saved\UEAgentBridge\conversations.json`

يمكنك حذف محادثات من الواجهة (زر Delete).

## أدوات UEAgentBridge (مختصر)

الإضافة تفتح HTTP endpoints على `localhost` بالإضافة لأدوات داخلية تستدعي نفس handlers.

Endpoints:
- `GET /ue-agent/health`
- `GET /ue-agent/tools`
- `POST /ue-agent/tools/call`

مثال استدعاء أداة عبر PowerShell:

```powershell
Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:30020/ue-agent/tools/call" `
  -ContentType "application/json" `
  -Body '{"toolName":"project.get_name","input":{}}'
```

## إعادة البناء (Build)

مهم: Unreal Editor يقفل DLL، لذلك:

1) أغلق Unreal Editor بالكامل.
2) ابنِ المشروع:

```powershell
& "C:\Unreal Engine\UE_5.7\Engine\Build\BatchFiles\Build.bat" UnrealEditor Win64 Development `
  -Project="C:\Unreal Projects\X555\X555.uproject" -WaitMutex -NoHotReload
```

## الأمان

- الإضافة مصممة للاستخدام المحلي فقط (`localhost`).
- يمكن (اختياري) وضع Token عبر:
  - `UE_AGENT_BRIDGE_TOKEN`
  - عندها يجب إرسال هيدر `Authorization: Bearer <token>`

لا تفتح المنفذ على الشبكة ولا تشارك مفاتيح API.


