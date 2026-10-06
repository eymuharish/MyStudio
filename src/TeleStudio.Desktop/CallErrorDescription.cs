using System.Text.RegularExpressions;
using TeleStudio.Contracts;
using TeleStudio.Presentation;
namespace TeleStudio.Desktop;

internal sealed record CallErrorDescription(string Kind, string Title, string Message, string Detail, bool AllowRetry)
{
    public static bool IsLocalEnd(string reason) => reason.Trim().ToLowerInvariant() is "local-hangup" or "local hangup" or "cancelled" or "canceled" or "local-user" or "caller-abort";
    public static bool CanRetry(CallState state, bool engineReady, bool closing, string original, string current) =>
        state is CallState.Idle or CallState.Failed && engineReady && !closing && original.Length > 0 && string.Equals(original.Trim(), current.Trim(), StringComparison.OrdinalIgnoreCase);
    public static CallErrorDescription FromFailed(CallFailed failure, AppLanguage language) => Describe(failure.ErrorCode + (string.IsNullOrWhiteSpace(failure.Detail) ? "" : ": " + failure.Detail), language);
    public static CallErrorDescription FromEnded(string reason, AppLanguage language) => Describe(reason, language);
    private static CallErrorDescription Describe(string detail, AppLanguage language)
    {
        var text = detail.ToLowerInvariant();
        bool Has(params string[] tokens) => tokens.Any(text.Contains);
        bool Status(string code) => Regex.IsMatch(text, @"(?<!\d)" + code + @"(?!\d)");
        var kind = Has("cleanup-failed") ? "cleanup" : Has("busy") || Status("486") || Status("600") ? "busy"
            : Status("401") || Status("407") || Has("authentication required") ? "authentication"
            : Has("timeout", "timed out", "noanswer", "no-answer", "no answer") || Status("408") ? "timeout"
            : Has("forbidden", "denied", "no-accept", "security", "unauthor", "declin", "refus", "reject") || Status("403") || Status("603") ? "denied"
            : Has("not found", "no-user", "nouser", "invalid address", "unknown user", "invalid-number", "invalid-conference") || Status("404") || Status("484") ? "address"
            : Has("unacceptable", "not acceptable", "capability", "codec", "no common", "nomedia", "no-feature", "protocol-error") || Status("488") || Status("415") ? "media"
            : Has("unavailable", "unreachable", "no route", "connectfail", "transportfail", "connect-fail", "transport-fail", "no-endpoint", "host-offline", "temporary-failure", "congestion", "no-bandwidth", "gatekeeper") || Status("480") || Status("503") ? "unavailable"
            : Has("engine-exited", "engine-send-failed") ? "engine"
            : text is "ok" or "ended" or "remote hangup" or "remote-hangup" or "endedbyremoteuser" or "remote-user" ? "remote" : "unknown";
        var ar = language == AppLanguage.Arabic;
        var message = (kind, ar) switch
        {
            ("authentication", true) => "يتطلب الجهاز أو خادم الاتصال مصادقة لم تكتمل. تحقق من بيانات الدخول وإعدادات التسجيل لدى الطرف الآخر.",
            ("authentication", false) => "The device or call server requires authentication that did not complete. Check credentials and registration settings.",
            ("busy", true) => "الجهاز مشغول بمكالمة أخرى. يمكنك إعادة المحاولة بعد قليل أو إغلاق هذه الرسالة.",
            ("busy", false) => "The device is busy with another call. Retry shortly or close this message.",
            ("timeout", true) => "انتهت مهلة الاتصال دون إجابة. تحقق من جاهزية الجهاز واتصال الشبكة ثم حاول مجددًا.",
            ("timeout", false) => "The call timed out without an answer. Check the device and network, then retry.",
            ("denied", true) => "رفض الجهاز الاتصال أو لم يسمح به. تحقق من إعدادات قبول المكالمات والصلاحيات لدى الطرف الآخر.",
            ("denied", false) => "The device declined or denied the call. Check its call acceptance and authorization settings.",
            ("address", true) => "تعذر العثور على الجهاز المطلوب. تحقق من عنوان الجهاز وإعدادات الاتصال.",
            ("address", false) => "The requested device could not be found. Check its address and connection settings.",
            ("media", true) => "تعذر الاتفاق على إعدادات الصوت أو الفيديو مع الجهاز. راجع البروتوكول وإعدادات الوسائط.",
            ("media", false) => "The devices could not agree on audio or video settings. Review the protocol and media settings.",
            ("unavailable", true) => "الجهاز أو خدمة الاتصال غير متاح حاليًا. تحقق من الشبكة وتشغيل الجهاز ثم حاول مجددًا.",
            ("unavailable", false) => "The device or call service is unavailable. Check the network and device, then retry.",
            ("cleanup", true) => "تعذر إغلاق الاتصال السابق بالكامل. استخدم إعادة تشغيل المحركات من فحص الجهاز قبل المحاولة مجددًا.",
            ("cleanup", false) => "The previous call could not be fully closed. Restart the engines from device diagnostics before retrying.",
            ("engine", true) => "توقف مكوّن الاتصال أو تعذر إرسال الأمر إليه. أعد تشغيل المحركات من فحص الجهاز ثم حاول مجددًا.",
            ("engine", false) => "The call component stopped or could not accept the command. Restart the engines from device diagnostics, then retry.",
            ("remote", true) => "انتهت المكالمة من الطرف الآخر. يمكنك إعادة الاتصال إذا رغبت.",
            ("remote", false) => "The other side ended the call. You can call again if needed.",
            (_, true) => "تعذر استمرار الاتصال أو إكماله. السبب الذي أرسله المحرك موضح في التفاصيل أدناه.",
            _ => "The call could not continue or complete. The reason reported by the engine is shown in the details below."
        };
        return new(kind, kind == "busy" ? ar ? "الجهاز مشغول" : "Device busy" : ar ? "حالة الاتصال" : "Call status", message, detail, kind is not ("cleanup" or "engine"));
    }
}
