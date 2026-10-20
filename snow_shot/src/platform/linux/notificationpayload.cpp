#include "snow_shot/platform/linux/notificationpayload.h"

#include <QVariant>

#ifdef linux
#undef linux
#endif

namespace snow_shot::platform::linux::notification {
namespace {

void appendEscaped(QString& output, QChar character) {
    switch (character.unicode()) {
    case u'&':
        output += QStringLiteral("&amp;");
        break;
    case u'<':
        output += QStringLiteral("&lt;");
        break;
    case u'>':
        output += QStringLiteral("&gt;");
        break;
    default:
        output += character;
        break;
    }
}

} // namespace

QString escapeBodyMarkup(QStringView text) {
    QString escaped;
    escaped.reserve(text.size());
    for (const QChar character : text)
        appendEscaped(escaped, character);
    return escaped;
}

uchar urgencyByte(SystemNotificationSeverity severity) {
    switch (severity) {
    case SystemNotificationSeverity::Error:
        return 2;
    case SystemNotificationSeverity::Warning:
    case SystemNotificationSeverity::Information:
        break;
    }
    // Nothing in this application is "low": a bubble is only raised when the
    // user asked for something, so it must not be filed under background noise.
    return 1;
}

QString iconNameFor(SystemNotificationSeverity severity) {
    switch (severity) {
    case SystemNotificationSeverity::Error:
        return QStringLiteral("dialog-error");
    case SystemNotificationSeverity::Warning:
        return QStringLiteral("dialog-warning");
    case SystemNotificationSeverity::Information:
        break;
    }
    return QStringLiteral("dialog-information");
}

int expireTimeoutMsFor(SystemNotificationSeverity severity) {
    switch (severity) {
    case SystemNotificationSeverity::Error:
        return 12000;
    case SystemNotificationSeverity::Warning:
        return 8000;
    case SystemNotificationSeverity::Information:
        break;
    }
    return 5000;
}

QStringList actionList(SystemNotificationAction action) {
    if (action == SystemNotificationAction::None)
        return {};
    return {QStringLiteral("default"), QString()};
}

Payload buildPayload(const SystemNotificationRequest& request, const QString& desktopEntryName,
                     bool bodyMarkupSupported) {
    Payload payload;
    payload.summary = request.title;
    payload.body = bodyMarkupSupported ? escapeBodyMarkup(request.body) : request.body;
    payload.iconName = iconNameFor(request.severity);
    payload.actions = actionList(request.action);
    payload.hints.insert(QStringLiteral("urgency"),
                         QVariant::fromValue(urgencyByte(request.severity)));
    if (!desktopEntryName.isEmpty()) {
        payload.hints.insert(QStringLiteral("desktop-entry"),
                             QVariant::fromValue(desktopEntryName));
    }
    payload.expireTimeoutMs = expireTimeoutMsFor(request.severity);
    return payload;
}

QVariantList notifyArguments(const Payload& payload, const QString& appName, uint replacesId) {
    QVariantList arguments;
    arguments.reserve(8);
    arguments.push_back(QVariant::fromValue(appName));                 // s app_name
    arguments.push_back(QVariant::fromValue(replacesId));              // u replaces_id
    arguments.push_back(QVariant::fromValue(payload.iconName));        // s app_icon
    arguments.push_back(QVariant::fromValue(payload.summary));         // s summary
    arguments.push_back(QVariant::fromValue(payload.body));            // s body
    arguments.push_back(QVariant::fromValue(payload.actions));         // as actions
    arguments.push_back(QVariant::fromValue(payload.hints));           // a{sv} hints
    arguments.push_back(QVariant::fromValue(payload.expireTimeoutMs)); // i expire_timeout
    return arguments;
}

} // namespace snow_shot::platform::linux::notification
