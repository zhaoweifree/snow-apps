#ifndef SNOW_SHOT_PLATFORM_LINUX_NOTIFICATIONPAYLOAD_H
#define SNOW_SHOT_PLATFORM_LINUX_NOTIFICATIONPAYLOAD_H

#include "snow_shot/platform/systemnotification.h"

#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

// GCC used to define `linux` as a macro. Current toolchains no longer do, but a
// translation unit that pulls in an old system header still can, and that would
// turn the namespace below into `platform::1`. Clearing it here costs nothing.
#ifdef linux
#undef linux
#endif

namespace snow_shot::platform::linux::notification {

/// Names from the freedesktop notification specification. A desktop that ships
/// its own daemon (dde-daemon's `notifications` service, `dunst`, `mako`) still
/// answers on this name: it is the only one the specification defines.
inline constexpr char serviceName[] = "org.freedesktop.Notifications";
inline constexpr char objectPath[] = "/org/freedesktop/Notifications";
inline constexpr char interfaceName[] = "org.freedesktop.Notifications";

/// `-1` asks the daemon for its own expiry, `0` means "until dismissed".
inline constexpr int serverDefaultTimeout = -1;

/// What one notification looks like on the wire, minus the two fields the
/// caller owns (`app_name` and `replaces_id`).
struct Payload {
    QString summary;
    QString body;
    QString iconName;
    QStringList actions;
    QVariantMap hints;
    int expireTimeoutMs = serverDefaultTimeout;
};

/// Escapes the three characters that would otherwise be read as markup.
///
/// The specification allows a small HTML subset in the body (`b`, `i`, `u`, `a`,
/// `img`), so a file path containing `&` or `<` renders as a broken fragment -
/// or disappears - unless it is escaped. Bodies this application sends are plain
/// text, which is why escaping the whole string is the right default rather than
/// letting callers embed markup.
[[nodiscard]] QString escapeBodyMarkup(QStringView text);

/// The `urgency` hint, which the specification defines as a byte:
/// 0 low, 1 normal, 2 critical.
///
/// It must be a byte. The daemon introspects the call and rejects the whole
/// message when the hint carries an `int` instead, which is why the value goes
/// through `uchar` rather than the enum's underlying type.
[[nodiscard]] uchar urgencyByte(SystemNotificationSeverity severity);

/// A freedesktop icon theme name. Every desktop that implements the
/// specification also ships the standard dialog icons.
[[nodiscard]] QString iconNameFor(SystemNotificationSeverity severity);

/// How long the bubble stays up.
///
/// Severity is the only input: a warning is worth more reading time than a
/// confirmation, and an error is worth more than both. Nothing asks for `0`
/// (sticky) - the daemon's "critical" styling already keeps urgent bubbles
/// around, and a notification that never leaves would outlive the file it
/// refers to.
[[nodiscard]] int expireTimeoutMsFor(SystemNotificationSeverity severity);

/// The `actions` array: alternating key/label pairs.
///
/// Only the reserved `default` key is used, with an empty label. Clicking the
/// bubble body then raises `ActionInvoked("default")` without adding a button
/// whose text would need a translation for every desktop language.
[[nodiscard]] QStringList actionList(SystemNotificationAction action);

/// Builds the payload for a request.
///
/// `desktopEntryName` goes into the `desktop-entry` hint, which is how the
/// daemon finds the installed `.desktop` file for its title bar and icon.
/// `bodyMarkupSupported` comes from the daemon's `GetCapabilities` answer: when
/// markup is not supported the body is sent verbatim, because escaping it would
/// make the ampersands and angle brackets visible.
[[nodiscard]] Payload buildPayload(const SystemNotificationRequest& request,
                                   const QString& desktopEntryName, bool bodyMarkupSupported);

/// The `Notify` arguments in the order the specification requires:
/// `app_name`, `replaces_id`, `app_icon`, `summary`, `body`, `actions`, `hints`,
/// `expire_timeout`.
///
/// The list is returned rather than a `QDBusMessage` so the order and the
/// variant types can be asserted without a session bus. Two of them are easy to
/// get wrong and fail at runtime only: `replaces_id` must be `uint` and
/// `expire_timeout` must be `int`.
[[nodiscard]] QVariantList notifyArguments(const Payload& payload, const QString& appName,
                                           uint replacesId);

} // namespace snow_shot::platform::linux::notification

#endif // SNOW_SHOT_PLATFORM_LINUX_NOTIFICATIONPAYLOAD_H
