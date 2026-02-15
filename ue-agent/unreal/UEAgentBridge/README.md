# UEAgentBridge (Unreal Editor Plugin)

إضافة Editor بسيطة توفر:

- تبويب داخل المحرر: `Window → UE Agent` (دردشة + إعدادات + محادثات محفوظة).
- HTTP endpoints على `localhost` لتوفير أدوات آمنة (JSON) يمكن استدعاؤها من عميل خارجي.

## التثبيت

انسخ هذا المجلد إلى مشروعك:

`ue-agent/unreal/UEAgentBridge` → `<YourProject>/Plugins/UEAgentBridge`

ثم افتح مشروع Unreal وفعّل الإضافة من `Edit → Plugins`.

## التشغيل داخل المحرر

افتح:

`Window → UE Agent`

- زر `Settings` لإدخال `Base URL / API Key / Model` واختيار المزوّد.
- قائمة محادثات (New / Delete) مع حفظ تلقائي.
- `Enter` يرسل، `Shift+Enter` سطر جديد.

## إعداد الجسر (HTTP Bridge)

الإضافة تقرأ إعداداتها من متغيرات البيئة عند تشغيل المحرر:

- `UE_AGENT_BRIDGE_PORT` (افتراضي: `30020`)
- `UE_AGENT_BRIDGE_TOKEN` (اختياري): إذا تم ضبطه، يجب إرسال
  `Authorization: Bearer <token>` مع كل طلب

## Endpoints

- `GET /ue-agent/health`
- `GET /ue-agent/tools`
- `POST /ue-agent/tools/call` body:
  - `{ "toolName": "project.get_name", "input": {} }`

## ملاحظة أمنية

هذا الجسر مصمم للاستخدام المحلي فقط (`localhost`). لا تفتحه على الشبكة ولا تشارك مفاتيح API أو Token.


