#ifndef SNOW_SHOT_APP_SINGLEINSTANCEENDPOINT_H
#define SNOW_SHOT_APP_SINGLEINSTANCEENDPOINT_H

#include <QString>

#include <QtGlobal>

namespace snow_shot::app::ipc {

/// A Unix domain socket lives in a fixed-size field: `sockaddr_un::sun_path` is 108
/// bytes on Linux and 104 on the BSDs, terminator included. Staying under the smaller
/// limit costs five bytes and keeps one layout valid on every host we might be asked
/// to run on; nothing is gained by spending them.
inline constexpr int kMaximumSocketPathBytes = 103;

/// Where the single-instance files live and what each endpoint is called.
struct EndpointLayout {
    /// Directory holding both sockets and the lock file. Empty on Windows, where the
    /// endpoint is a named pipe in a global namespace and only the lock has a path.
    QString directory;
    /// Handed to `QLocalServer::listen()` and `removeServer()`: a bare pipe name on
    /// Windows, an absolute socket path otherwise.
    QString legacyEndpoint;
    QString receiptEndpoint;
    QString lockFilePath;
    /// True when `directory` is the temp-directory fallback, which we have to create
    /// ourselves with owner-only permissions. The session runtime directory is created
    /// and protected by the session manager, so it is not ours to change.
    bool privateDirectory = false;
    /// Empty when the layout is usable, otherwise why it is not. Failing here beats
    /// failing inside `listen()`, whose only diagnostic is the strerror text.
    QString error;
};

/// Builds the layout for a socket-based endpoint.
///
/// `runtimeDirectory` is the session runtime directory (`$XDG_RUNTIME_DIR` on Linux,
/// usually `/run/user/<uid>`): per-user, mode 0700, and on tmpfs, so no cleaner ever
/// unlinks the socket of a long-running process. `temporaryDirectory` is the fallback
/// when that is unset or unusable; the uid is appended to keep two users on one machine
/// from sharing a directory, since the shared temp directory is world-writable.
///
/// Absolute paths are required: a relative `runtimeDirectory` is a configuration error
/// (the XDG spec requires an absolute path) and would silently turn into a name under
/// the temp directory, so it is treated as absent.
///
/// If the preferred directory makes an endpoint longer than
/// `kMaximumSocketPathBytes`, the other candidate is tried before giving up.
EndpointLayout socketEndpointLayout(const QString& baseName, const QString& runtimeDirectory,
                                    const QString& temporaryDirectory, qint64 userId);

/// Byte length of the UTF-8 name, not the number of code points: 35 CJK characters are
/// 105 bytes and do not fit even though they are only 35 characters.
[[nodiscard]] bool socketPathFits(const QString& absolutePath);

/// Whether an endpoint directory that already exists may be used. It must be a
/// directory owned by this user: one planted in a world-writable temp directory by
/// somebody else would hold a socket they can replace. Refusing to start is the lesser
/// failure and the message names the path to remove. A path that does not exist yet is
/// acceptable -- the caller creates it.
[[nodiscard]] bool isAcceptableEndpointDirectory(bool exists, bool isDirectory, qint64 ownerId,
                                                 qint64 userId);

} // namespace snow_shot::app::ipc

#endif // SNOW_SHOT_APP_SINGLEINSTANCEENDPOINT_H
