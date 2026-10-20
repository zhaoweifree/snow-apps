#include "snow_shot/compat/qt_compat.h"
#include "snow_shot/platform/linux/notificationpayload.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>

using namespace snow_shot::platform::linux::notification;
using snow_shot::platform::SystemNotificationAction;
using snow_shot::platform::SystemNotificationRequest;
using snow_shot::platform::SystemNotificationSeverity;

namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void requireEqual(const QString& actual, const QString& expected, const char* message) {
    if (actual != expected) {
        std::cerr << message << "\n  actual:   " << actual.toStdString()
                  << "\n  expected: " << expected.toStdString() << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void requireEqual(int actual, int expected, const char* message) {
    if (actual != expected) {
        std::cerr << message << "\n  actual:   " << actual << "\n  expected: " << expected << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void requireEqual(const QStringList& actual, const QStringList& expected, const char* message) {
    if (actual != expected) {
        std::cerr << message << "\n  actual:   " << actual.join(u'|').toStdString()
                  << "\n  expected: " << expected.join(u'|').toStdString() << '\n';
        std::exit(EXIT_FAILURE);
    }
}

/// The D-Bus signature of a variant, by the meta type name Qt marshals it as.
/// Comparing names keeps this check readable across Qt 5 and Qt 6, whose own
/// type enums are spelled differently.
///
/// `QMetaType::name()` returns a `QByteArray` by value in Qt 5.15 and a
/// `const char*` in Qt 6, so the Qt 5 branch must copy before the temporary dies.
QString wireType(const QVariant& value) {
#if SNOW_QT6
    const char* name = snowMetaTypeOf(value).name();
    return name == nullptr ? QStringLiteral("<unregistered>") : QString::fromLatin1(name);
#else
    const QString name = QString::fromLatin1(snowMetaTypeOf(value).name());
    return name.isEmpty() ? QStringLiteral("<unregistered>") : name;
#endif
}

void requireWireType(const QVariant& value, const char* expected, const char* message) {
    const QString actual = wireType(value);
    if (actual != QLatin1String(expected)) {
        std::cerr << message << "\n  actual:   " << actual.toStdString()
                  << "\n  expected: " << expected << '\n';
        std::exit(EXIT_FAILURE);
    }
}

SystemNotificationRequest sampleRequest() {
    SystemNotificationRequest request;
    request.title = QStringLiteral("Recording saved");
    request.body = QStringLiteral("Saved to /home/u/Videos/a&b<c>.mp4");
    request.severity = SystemNotificationSeverity::Information;
    request.action = SystemNotificationAction::OpenRecording;
    request.filePath = QStringLiteral("/home/u/Videos/a&b<c>.mp4");
    return request;
}

/// A body is a limited HTML subset, so the three characters that open a tag must
/// be escaped. A path is otherwise rendered as a broken fragment or vanishes.
void bodyMarkupEscaping() {
    requireEqual(escapeBodyMarkup(QStringLiteral("/home/u/a.mp4")), QStringLiteral("/home/u/a.mp4"),
                 "plain text must pass through unchanged");
    requireEqual(escapeBodyMarkup(QStringLiteral("a&b")), QStringLiteral("a&amp;b"),
                 "ampersand must be escaped");
    requireEqual(escapeBodyMarkup(QStringLiteral("a<b>c")), QStringLiteral("a&lt;b&gt;c"),
                 "angle brackets must be escaped");
    // The ampersand in the replacement text must not be re-escaped: escaping runs
    // over the input, not over its own output.
    requireEqual(escapeBodyMarkup(QStringLiteral("<&>")), QStringLiteral("&lt;&amp;&gt;"),
                 "escapes must not chain");
}

/// Urgency is a byte in the `a{sv}` hints. Sending it as an int makes the daemon
/// reject the whole call, so the type is asserted, not just the value.
void urgencyIsAByte() {
    const QVariant information =
        QVariant::fromValue(urgencyByte(SystemNotificationSeverity::Information));
    const QVariant warning = QVariant::fromValue(urgencyByte(SystemNotificationSeverity::Warning));
    const QVariant error = QVariant::fromValue(urgencyByte(SystemNotificationSeverity::Error));
    requireWireType(information, "uchar", "urgency must marshal as a byte");
    requireEqual(information.toInt(), 1, "information is normal urgency");
    requireEqual(warning.toInt(), 1, "warning is normal urgency");
    requireEqual(error.toInt(), 2, "error is critical urgency");

    requireEqual(iconNameFor(SystemNotificationSeverity::Information),
                 QStringLiteral("dialog-information"), "information icon name");
    requireEqual(iconNameFor(SystemNotificationSeverity::Warning), QStringLiteral("dialog-warning"),
                 "warning icon name");
    requireEqual(iconNameFor(SystemNotificationSeverity::Error), QStringLiteral("dialog-error"),
                 "error icon name");
}

/// Longer-lived for the severities that carry consequences, but never sticky:
/// a bubble that outlives the file it points at is worse than one that is missed.
void expiryFollowsSeverity() {
    const int information = expireTimeoutMsFor(SystemNotificationSeverity::Information);
    const int warning = expireTimeoutMsFor(SystemNotificationSeverity::Warning);
    const int error = expireTimeoutMsFor(SystemNotificationSeverity::Error);
    require(information > 0, "information must expire on its own");
    require(warning > information, "warning must outlive information");
    require(error > warning, "error must outlive warning");
}

/// Only the reserved `default` key, with an empty label: clicking the body fires
/// the action without adding a button that would need a translation.
void actionsUseTheDefaultKeyOnly() {
    require(actionList(SystemNotificationAction::None).isEmpty(),
            "a notification without an action must send no actions");
    requireEqual(actionList(SystemNotificationAction::OpenAbout),
                 QStringList{QStringLiteral("default"), QString()},
                 "about action must use the default key");
    requireEqual(actionList(SystemNotificationAction::OpenRecording),
                 QStringList{QStringLiteral("default"), QString()},
                 "recording action must use the default key");
}

void payloadCarriesTheHintsTheDaemonNeeds() {
    const SystemNotificationRequest request = sampleRequest();
    const Payload escaped = buildPayload(request, QStringLiteral("snow-shot"), true);
    requireEqual(escaped.summary, request.title, "summary is not markup and must stay verbatim");
    requireEqual(escaped.body, QStringLiteral("Saved to /home/u/Videos/a&amp;b&lt;c&gt;.mp4"),
                 "body must be escaped when the daemon supports markup");
    requireEqual(escaped.iconName, QStringLiteral("dialog-information"),
                 "icon comes from severity");
    requireEqual(escaped.expireTimeoutMs, expireTimeoutMsFor(request.severity),
                 "payload expiry must match the policy");
    requireWireType(escaped.hints.value(QStringLiteral("urgency")), "uchar",
                    "urgency hint must be a byte");
    requireEqual(escaped.hints.value(QStringLiteral("desktop-entry")).toString(),
                 QStringLiteral("snow-shot"), "desktop-entry hint must name the installed entry");

    // A daemon without `body-markup` would show the escape sequences literally.
    const Payload verbatim = buildPayload(request, QStringLiteral("snow-shot"), false);
    requireEqual(verbatim.body, request.body, "body must be verbatim without markup support");

    // An empty desktop entry name means no hint at all rather than an empty one.
    const Payload anonymous = buildPayload(request, QString(), true);
    require(!anonymous.hints.contains(QStringLiteral("desktop-entry")),
            "an empty desktop entry name must not produce a hint");
}

/// The eight arguments of `Notify`, in order and with the exact variant types the
/// daemon introspects. Getting `replaces_id` or `expire_timeout` wrong compiles,
/// links, and fails at runtime only.
void notifyArgumentsMatchTheSpecification() {
    const Payload payload = buildPayload(sampleRequest(), QStringLiteral("snow-shot"), true);
    const QVariantList arguments = notifyArguments(payload, QStringLiteral("Snow Shot"), 42u);
    requireEqual(static_cast<int>(arguments.size()), 8, "Notify takes eight arguments");

    requireWireType(arguments.at(0), "QString", "app_name must be a string");
    requireEqual(arguments.at(0).toString(), QStringLiteral("Snow Shot"),
                 "app_name is the display name");
    requireWireType(arguments.at(1), "uint", "replaces_id must be unsigned");
    require(arguments.at(1).toUInt() == 42u, "replaces_id must be passed through");
    requireWireType(arguments.at(2), "QString", "app_icon must be a string");
    requireEqual(arguments.at(2).toString(), payload.iconName,
                 "app_icon is the positional argument");
    requireWireType(arguments.at(3), "QString", "summary must be a string");
    requireEqual(arguments.at(3).toString(), payload.summary, "summary is the fourth argument");
    requireWireType(arguments.at(4), "QString", "body must be a string");
    requireEqual(arguments.at(4).toString(), payload.body, "body is the fifth argument");
    requireWireType(arguments.at(5), "QStringList", "actions must be an array of strings");
    requireWireType(arguments.at(6), "QVariantMap", "hints must be a string-to-variant map");
    requireWireType(arguments.at(7), "int", "expire_timeout must be a signed integer");
    requireEqual(arguments.at(7).toInt(), payload.expireTimeoutMs,
                 "expire_timeout is the eighth argument");

    // The first delivery of a kind has nothing to replace; zero is the
    // specification's "create a new bubble".
    const QVariantList firstDelivery = notifyArguments(payload, QStringLiteral("Snow Shot"), 0u);
    require(firstDelivery.at(1).toUInt() == 0u, "a first notification must not replace anything");
}
} // namespace

int main() {
    bodyMarkupEscaping();
    urgencyIsAByte();
    expiryFollowsSeverity();
    actionsUseTheDefaultKeyOnly();
    payloadCarriesTheHintsTheDaemonNeeds();
    notifyArgumentsMatchTheSpecification();
    std::cout << "linux notification payload tests passed\n";
    return EXIT_SUCCESS;
}
