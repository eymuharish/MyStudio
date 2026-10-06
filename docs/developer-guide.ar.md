# دليل المطور: بنية MyStudio والتحقق منه

## البنية العامة

MyStudio تطبيق Windows x64 بواجهة WinUI 3 ومحرك OPAL أصلي. ملف [TeleStudio.slnx](../TeleStudio.slnx) يجمع مشاريع التطبيق والاختبارات. يحتفظ المصدر بأسماء داخلية مثل TeleStudio وEssaStudio؛ اسم التطبيق الظاهر هو MyStudio.

يمر العمل بين الطبقات التالية:

```text
MyStudio.WinUI              الواجهة وأدوات المكالمة
        |
StudioSession              تهيئة الإعدادات وربط الواجهة بالخدمات
        |
TeleStudio.Desktop         خدمات الصوت والفيديو والتشخيص وربط المحرك
        |
opal_console.exe           عامل الاتصال الأصلي: SIP وH.323
        |
OPAL / PTLib / codecs      التفاوض والوسائط والترميزات
```

تستخدم الخدمات والواجهة أيضًا مشاريع العقود والإعدادات وحالة العرض المشتركة. يملك العامل الأصلي مستمعي SIP وH.323 ويسمح بمكالمة نشطة واحدة. تبادل الأوامر والأحداث مع الجانب المُدار موضح في [دليل المحرك](../native/opal/README.md).

## مسؤوليات المشاريع

| المشروع أو المسار | المسؤولية |
| --- | --- |
| `MyStudio.WinUI` | النوافذ والصفحات والشريط العائم والمشاركة والتسجيل |
| `src/TeleStudio.Contracts` | أنواع الأوامر والأحداث والحالات المشتركة |
| `src/TeleStudio.Presentation` | حالة العرض وقواعد التفاعل مع جلسة الاتصال |
| `src/TeleStudio.Settings` | حفظ الإعدادات وملفات الأجهزة وقائمة الاستديوهات |
| `src/TeleStudio.Desktop` | خدمات التطبيق وربط العامل الأصلي والصوت والتشخيص |
| `native/opal` | العامل الأصلي وسياسات الاتصال والرقع والاختبارات |
| `native/opal/aac` | إضافة AAC المتوافقة مع نسخة OPAL المحددة |
| `native/media` | التقاط ومعالجة الوسائط والترميز والتسجيل |
| `scripts` | تنزيل مصادر الاعتمادات والبناء والتغليف وأدوات التحقق |

## متطلبات البناء

لبناء التطبيق الكامل، تحتاج بيئة Windows x64 تشمل:

- .NET 10 SDK.
- Visual Studio 2022 مع أدوات C++ ‏v143 وWindows SDK 10.
- CMake وGit وPowerShell، وPerl أصلي لـ Windows عند بناء OpenSSL.
- نسخ OPAL وPTLib وOpenSSL المحددة في [بيانات الاعتمادات](opal-dependencies.json).
- اعتمادات الوسائط المطابقة داخل `dependencies/media`، ومنها FDK-AAC وSpeexDSP وlibyuv وFFmpeg وx264 وx265.
- مكتبات Microsoft Visual C++ runtime بنسخة x64 المناسبة للتغليف.

المستودع لا يرفق حاليًا الاعتمادات المبنية أو الحزمة التنفيذية. بعض سكربتات البناء تفترض مسارات Visual Studio Community ونسخ أدوات محددة؛ راجع المعاملات وعدّل مسارات التشغيل لتناسب جهازك.

## ترتيب البناء والتغليف

نفّذ السكربتات من جذر المستودع، مع استخدام جذر إعداد واحد للاعتمادات ومخرجات البناء:

1. [Initialize-OpalSdk.ps1](../scripts/Initialize-OpalSdk.ps1): تنزيل مصادر OPAL وPTLib وOpenSSL والتحقق من مطابقة commits المحددة.
2. [BuildOpenSsl.ps1](../native/opal/aac/BuildOpenSsl.ps1): بناء OpenSSL باستخدام مسار Perl المناسب؛ مرّر `-OutputRoot` للجذر نفسه المستخدم في `-EditionRoot` بالسكربتات الأخرى.
3. [Build-OpalDependencies.ps1](../scripts/Build-OpalDependencies.ps1): تطبيق رقع التوافق وبناء PTLib وOPAL.
4. جهّز اعتمادات الوسائط المطابقة تحت `dependencies/media` قبل بناء التطبيق.
5. [Build-OpalEdition.ps1](../scripts/Build-OpalEdition.ps1): بناء المحرك ونشر التطبيق ثم تغليفه، مع `-OutputDirectory` لمجلد جديد و`-RuntimeDirectory` لمكتبات C++ المناسبة.

يكون `EditionRoot` الافتراضي للسكربتات المذكورة هو المجلد الأب للمستودع، وتوجد الاعتمادات ومخرجات البناء تحته. راجع [شرح إعادة البناء](opal-portable-readme.md) و[دليل إضافة AAC](../native/opal/aac/README.md) للتفاصيل. نجاح البناء لا يثبت جودة مكالمة فعلية.

## الاختبارات

شغّل الأوامر التالية من جذر المستودع. هذه أمثلة لتشغيل الاختبارات الموجودة، وليست تقريرًا بأنها نُفذت في بيئتك.

### اختبارات يمكن تشغيلها على Linux أو Windows

مشروعا Contracts وSettings يستهدفان `net10.0`. بعد تثبيت .NET 10 SDK وتوفر الوصول إلى NuGet:

```sh
dotnet test tests/TeleStudio.Contracts.Tests/TeleStudio.Contracts.Tests.csproj -c Release
dotnet test tests/TeleStudio.Settings.Tests/TeleStudio.Settings.Tests.csproj -c Release
```

لفحص أدوات اختبار الأجهزة بالمحاكاة، مع Python 3:

```sh
python3 -B scripts/TestOpalDeviceAuditTests.py
```

السكربت الأخير يستخدم عمليات ووسائط وهمية؛ لا يتصل بالاستديوهات ولا يثبت جودة المكالمات. تظهر داخله حالات فشل مقصودة للتأكد من أن أداة الاختبار تكتشفها؛ الحكم على نجاح التشغيل يكون بخاتمة السيناريوهات ورمز الخروج.

### اختبارات تحتاج Windows

```powershell
dotnet test tests/TeleStudio.Desktop.Tests/TeleStudio.Desktop.Tests.csproj -c Release -p:Platform=x64
```

اختبارات العامل الأصلي تحتاج SDK والاعتمادات المطابقة ومخرجات Release. راجع أوامر CMake وCTest في [دليل OPAL](../native/opal/README.md). يوفّر [Test-PortableOpal.ps1](../scripts/Test-PortableOpal.ps1) فحوص الحزمة ونقلها بعد البناء.

اختبارات الأجهزة الفعلية تُجري مكالمات، ولذلك تُشغّل فقط بتفويض صريح وأهداف محددة ومجلد بيانات معزول، وفق [دليل اختبار انتقال الاستديوهات](studio-transition-smoke.md).

## قراءة نتائج التحقق

- [سجل التحقق](opal-validation.md) هو المرجع لنتائج التجارب السابقة وحدودها.
- نتائج 503/503 للإصدار 2.2.0 مذكورة في التقرير؛ مجلد `build/release-2.2-evidence` المشار إليه غير مرفق في هذا المستودع حاليًا.
- ملفات TRX المرفقة في `tests/TeleStudio.Desktop.Tests/TestResults` تخص تشغيلات أقدم، ولا تمثل إعادة تنفيذ اختبارات المصدر الحالي.
- يلزم تحقق فعلي لتغطية شاشات Cisco، ومشاركة ARM إلى Sony، وجودة الصوت على عتاد المستخدم.

عند تغيير السلوك، شغّل الاختبارات المرتبطة به وسجّل النتائج الجديدة مع بيئة التشغيل وهوية البناء. افصل نتائج المحاكاة وlocalhost عن نتائج الأجهزة الفعلية.

[العودة إلى صفحة المشروع](../README.md)
