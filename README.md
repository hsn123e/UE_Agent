# UEAgentBridge — Unreal Engine Editor AI Assistant

## العربية

`UEAgentBridge` إضافة (Plugin) لمحرر Unreal Engine.

الفكرة: اربط الإضافة مع **أي مزوّد/نموذج** يدعم واجهة **OpenAI-compatible** (أو اختر Preset جاهز)، ثم اكتب أوامر نصية لتنفيذ مهام داخل المحرر.

### التركيب

1) انسخ مجلد `UEAgentBridge` إلى مشروعك:

`UEAgentBridge` → `<YourProject>/Plugins/UEAgentBridge`

2) افتح المشروع وفعّل الإضافة:

`Edit → Plugins → UEAgentBridge`

3) أعد تشغيل المحرر إذا طُلب ذلك.

### الاستخدام

1) افتح:

`Window → UE Agent`

2) من `Settings`:
- اختر `Provider preset` أو `Custom` لمزوّدك الخاص
- ضع `API Key` إذا كان مطلوبًا
- اضغط `Refresh` لتحميل قائمة `Model` (إذا كان المزوّد يدعم ذلك)
- اختر الموديل واضغط `Save`

3) اكتب طلبك واضغط Enter.
- إذا كان طلبك يعتمد على تحديد Actor، حدّده أولًا في الـ Outliner.
- زر `Selection → Describe` يساعد على إعطاء تفاصيل عن الـ Actor المحدد.

### تشغيل نموذج محلي (Local)

- اختر preset محلي مثل `Ollama (Local)` أو `LM Studio (Local)` ثم اضغط `Refresh` واختر الموديل.
- إذا لم تظهر قائمة موديلات، اكتب اسم الموديل يدويًا في `Model`.

### حفظ المحادثات

يتم الحفظ لكل مشروع في:

`<Project>/Saved/UEAgentBridge/conversations.json`

### إعادة البناء (Build)

أغلق Unreal Editor ثم نفّذ:

```powershell
& "C:\Unreal Engine\UE_5.7\Engine\Build\BatchFiles\Build.bat" UnrealEditor Win64 Development `
  -Project="C:\Unreal Projects\X555\X555.uproject" -WaitMutex -NoHotReload
```

### ملاحظة أمان

الجسر مصمم للاستخدام المحلي فقط (`localhost`). لا تفتحه على الشبكة ولا تشارك مفاتيح API.

---

## English

`UEAgentBridge` is an Unreal Editor plugin.

Idea: connect it to **any provider/model** that supports an **OpenAI-compatible** API (or pick a preset), then write natural-language requests to perform actions inside the editor.

### Install

1) Copy `UEAgentBridge` into your project:

`UEAgentBridge` → `<YourProject>/Plugins/UEAgentBridge`

2) Enable the plugin:

`Edit → Plugins → UEAgentBridge`

3) Restart the editor if prompted.

### Use

1) Open:

`Window → UE Agent`

2) In `Settings`:
- pick a `Provider preset` or `Custom`
- enter an `API Key` if required
- press `Refresh` to load the `Model` list (if supported)
- select a model and press `Save`

3) Type your request and press Enter.
- If your request depends on an actor selection, select it in the Outliner first.
- Use `Selection → Describe` to capture details about the selected actor.

### Run a local model

- Pick a local preset like `Ollama (Local)` or `LM Studio (Local)`, then press `Refresh` and choose a model.
- If model listing isn’t available, type the model name manually in `Model`.

### Chat persistence

Saved per project at:

`<Project>/Saved/UEAgentBridge/conversations.json`


