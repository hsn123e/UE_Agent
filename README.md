# UEAgentBridge — Unreal Engine Editor AI Assistant

## العربية

`UEAgentBridge` هي إضافة (Plugin) داخل محرر Unreal Engine توفر:

- تبويب `Window → UE Agent` (دردشة + محادثات محفوظة + إعدادات).
- أدوات Editor/Blueprint/UMG يمكن للـ LLM استخدامها.
- HTTP Bridge محلي على `localhost` (اختياري) لاستدعاء الأدوات من خارج المحرر.

### المتطلبات

- Unreal Engine 5.7.x (تم الاختبار على 5.7.2)
- Windows 10/11
- لبناء إضافة C++: Visual Studio Build Tools 2022 + Windows 10 SDK

### التثبيت داخل مشروعك

انسخ:

`UEAgentBridge` → `<YourProject>/Plugins/UEAgentBridge`

ثم افتح المشروع وفعّل الإضافة من:

`Edit → Plugins → UEAgentBridge`

### التشغيل

داخل المحرر:

`Window → UE Agent`

### الإعدادات (LLM)

داخل صفحة `Settings`:

- اختر `Provider preset`
- أدخل `API Key` (إذا كان مطلوبًا)
- اختر `Model` (أو اضغط `Refresh` لجلب قائمة الموديلات)
- اضغط `Save`

**Ollama (Local)**
- اختر preset: `Ollama (Local)`
- لا تحتاج API Key
- اضغط `Refresh` ثم اختر موديل من القائمة

**Ollama Cloud**
- اختر preset: `Ollama Cloud`
- ضع API Key من `ollama.com/settings/keys`
- اضغط `Refresh` واختر موديل

### حفظ المحادثات

يتم الحفظ لكل مشروع في:

`<Project>/Saved/UEAgentBridge/conversations.json`

### إعادة البناء (Build)

أغلق Unreal Editor ثم:

```powershell
& "C:\Unreal Engine\UE_5.7\Engine\Build\BatchFiles\Build.bat" UnrealEditor Win64 Development `
  -Project="C:\Unreal Projects\X555\X555.uproject" -WaitMutex -NoHotReload
```

### الأمان

الجسر مصمم للاستخدام المحلي فقط (`localhost`). لا تفتحه على الشبكة ولا تشارك مفاتيح API.

---

## English

`UEAgentBridge` is an Unreal Editor plugin that provides:

- `Window → UE Agent` tab (chat + persistent conversations + settings).
- Editor/Blueprint/UMG tools the LLM can call.
- Optional local HTTP bridge on `localhost` to call tools from outside the editor.

### Requirements

- Unreal Engine 5.7.x (tested on 5.7.2)
- Windows 10/11
- To build the C++ plugin: Visual Studio Build Tools 2022 + Windows 10 SDK

### Install into your project

Copy:

`UEAgentBridge` → `<YourProject>/Plugins/UEAgentBridge`

Then enable in:

`Edit → Plugins → UEAgentBridge`

### Run

In the editor:

`Window → UE Agent`

### Settings (LLM)

In the `Settings` page:

- Pick a `Provider preset`
- Enter an `API Key` (if required)
- Pick a `Model` (or press `Refresh` to load model list)
- Press `Save`

### Chat persistence

Saved per project at:

`<Project>/Saved/UEAgentBridge/conversations.json`



