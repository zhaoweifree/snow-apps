#ifndef SNOW_SHOT_PLATFORM_LINUX_DESKTOPENTRY_H
#define SNOW_SHOT_PLATFORM_LINUX_DESKTOPENTRY_H

#include <QByteArray>
#include <QString>
#include <QStringList>

// GCC used to define `linux` as a macro. Current toolchains no longer do, but a
// translation unit that pulls in an old system header still can, and that would
// turn the namespace below into `platform::1`. Clearing it here costs nothing.
#ifdef linux
#undef linux
#endif

namespace snow_shot::platform::linux::desktopentry {

/// The group every autostart entry lives in. Keys outside it are irrelevant.
inline constexpr char desktopEntryGroup[] = "Desktop Entry";

/// One `Exec` argument, escaped as the specification requires.
///
/// Two escaping layers apply and their order matters: the value's string
/// escapes run before the quoting rules, so a literal backslash inside a quoted
/// argument needs four of them.
[[nodiscard]] QString escapeExecArgument(const QString& argument);

/// A complete `Exec` value: arguments joined by single spaces, which is what the
/// specification says separates them.
[[nodiscard]] QString execValue(const QStringList& command);

/// Splits an `Exec` value back into arguments.
///
/// This undoes the quoting layer, which `readValue` cannot: the string escapes
/// are resolved there, then quotes are removed and the four characters the
/// quoting rule escapes are restored. Field codes are left in place, so an entry
/// that expects the desktop to pass it a file never compares equal to a plain
/// command line.
[[nodiscard]] QStringList parseExec(const QString& value);

/// The body of an autostart entry. `exec` must already be escaped.
///
/// `NoDisplay` keeps the entry out of application menus on desktops that would
/// otherwise list it, and `X-GNOME-Autostart-enabled` is written as well as
/// omitting `Hidden`: GNOME's own startup panel reads that key, and a user who
/// toggles the app there should see the same state we report.
[[nodiscard]] QByteArray autoStartEntry(const QString& name, const QString& exec,
                                        const QString& comment = QString());

/// The same entry with `Hidden=true` and nothing else, used to mask a
/// system-wide entry that we cannot delete.
[[nodiscard]] QByteArray hiddenEntry(const QString& name);

/// A value from `group`, or a null string when the group or key is absent.
///
/// Only the syntax needed to read back what this module writes is implemented:
/// group headers, `key=value`, `#` comments, and the string escapes the
/// specification defines. Anything else is left as written rather than guessed
/// at, so an entry we did not write never parses into a value we did not expect.
[[nodiscard]] QString readValue(const QByteArray& contents, const QString& group,
                                const QString& key);

/// Whether the entry asks the desktop to start the application.
///
/// A missing file is reported by passing a null `QByteArray`, which is disabled.
/// `Hidden=true` is the specified way to disable, and
/// `X-GNOME-Autostart-enabled=false` is how GNOME's startup panel records the
/// same decision without rewriting the entry; both mean disabled.
///
/// `OnlyShowIn`, `NotShowIn` and `TryExec` are deliberately ignored: they decide
/// whether *this* desktop starts the entry, not whether the user asked for it to
/// start, and reporting them as "off" would make the setting disagree with the
/// entry we would write next.
[[nodiscard]] bool requestsAutoStart(const QByteArray& contents);

/// Whether the entry is an application at all. Entries of any other type are
/// never started at login, so a stray file must not read as enabled.
[[nodiscard]] bool isApplicationEntry(const QByteArray& contents);

} // namespace snow_shot::platform::linux::desktopentry

#endif // SNOW_SHOT_PLATFORM_LINUX_DESKTOPENTRY_H
