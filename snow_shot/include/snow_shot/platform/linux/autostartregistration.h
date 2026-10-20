#ifndef SNOW_SHOT_PLATFORM_LINUX_AUTOSTARTREGISTRATION_H
#define SNOW_SHOT_PLATFORM_LINUX_AUTOSTARTREGISTRATION_H

#include <QByteArray>
#include <QString>

// See the note in desktopentry.h: an old system header may still define this.
#ifdef linux
#undef linux
#endif

namespace snow_shot::platform::linux {

/// Outcome of a reconciliation, with the same field names as the Windows result
/// so the call sites in `main.cpp` read identically on both platforms.
struct AutoStartResult {
    bool success = true;
    QString error;
};

/// The user's autostart entry as it was before a change, kept so a failed
/// settings write can put it back.
///
/// `contents` is retained byte for byte rather than regenerated: the previous
/// entry may have been written by a packager or edited by hand, and restoring a
/// normalised version of it would not be a restore.
struct AutoStartSnapshot {
    bool valid = false;
    bool existed = false;
    QByteArray contents;
    QString error;
};

/// Auto-start through the freedesktop autostart specification: a `.desktop`
/// file in `$XDG_CONFIG_HOME/autostart`.
///
/// There is no privileged registry here, which makes two Windows concepts
/// disappear: nothing needs elevation, and there is no separate "elevated
/// auto-start" mode because a user-session process has no way to ask for one.
class AutoStartRegistration final {
  public:
    /// Always true on Linux: every desktop environment reads this directory.
    /// A failure to write is reported by `setEnabled`, not predicted here, so a
    /// read-only home still shows an error instead of a switch that cannot move.
    [[nodiscard]] static bool isSupported();

    /// `$XDG_CONFIG_HOME/autostart/<product id>.desktop`.
    [[nodiscard]] static QString entryPath();

    /// The `Exec` value this application expects to find in its own entry.
    [[nodiscard]] static QString expectedExec();

    /// Whether the desktop would start the application at login, taking the
    /// user's entry when present and the system-wide one otherwise.
    [[nodiscard]] static bool isEnabled();

    /// Whether the entry in effect already launches this executable. Used to
    /// skip a rewrite on every start; a stale path or a masking stub reads as a
    /// mismatch and gets repaired.
    [[nodiscard]] static bool matchesExpectedEntry();

    [[nodiscard]] static AutoStartSnapshot snapshot();

    /// Turns auto-start on or off.
    ///
    /// Disabling deletes the user's entry, except when a system-wide entry with
    /// the same name exists: deleting ours would let that one take effect, so a
    /// `Hidden=true` stub is written in its place, which is what the
    /// specification prescribes for masking a packaged entry.
    [[nodiscard]] static bool setEnabled(bool enabled, QString* error = nullptr);

    [[nodiscard]] static bool restore(const AutoStartSnapshot& previous, QString* error = nullptr);

    /// Brings the desktop's state in line with the stored preference and reports
    /// whether it now agrees. Runs at every start, so it must be cheap and
    /// must not touch anything when the entry already matches.
    [[nodiscard]] static AutoStartResult reconcile(bool enabled);
};

} // namespace snow_shot::platform::linux

#endif // SNOW_SHOT_PLATFORM_LINUX_AUTOSTARTREGISTRATION_H
