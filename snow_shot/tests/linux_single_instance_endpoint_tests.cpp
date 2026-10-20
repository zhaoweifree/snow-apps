#include "snow_shot/app/singleinstanceendpoint.h"

#include <QDir>
#include <QString>

#include <cstdlib>
#include <iostream>

using namespace snow_shot::app::ipc;

namespace {
// What the coordinator builds: "snow-shot-" plus 24 hex characters of SHA-256.
const QString kBaseName = QStringLiteral("snow-shot-0123456789abcdef01234567");

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

/// The session runtime directory is preferred because it is private and lives on a
/// tmpfs nothing sweeps; the temp directory is only a fallback.
void runtimeDirectoryIsPreferred() {
    const EndpointLayout layout =
        socketEndpointLayout(kBaseName, QStringLiteral("/run/user/1000"), QStringLiteral("/tmp"),
                             /*userId=*/1000);
    require(layout.error.isEmpty(), "a session runtime directory is always usable");
    requireEqual(layout.directory, QStringLiteral("/run/user/1000"),
                 "the session runtime directory wins over the temp directory");
    require(!layout.privateDirectory,
            "the session runtime directory is created by the session manager, not by us");
    requireEqual(layout.legacyEndpoint, QStringLiteral("/run/user/1000/") + kBaseName,
                 "the legacy endpoint is an absolute socket path inside that directory");
    requireEqual(layout.receiptEndpoint, layout.legacyEndpoint + QStringLiteral("-v2"),
                 "the receipt endpoint is the legacy path with the version suffix");
    requireEqual(layout.lockFilePath,
                 QStringLiteral("/run/user/1000/") + kBaseName + QStringLiteral(".lock"),
                 "the lock shares the directory with the sockets");
    require(QDir::isAbsolutePath(layout.legacyEndpoint), "a socket path is always absolute");
}

/// XDG_RUNTIME_DIR may end with a slash; the path must not come out doubled.
void trailingSlashIsNormalised() {
    const EndpointLayout layout = socketEndpointLayout(kBaseName, QStringLiteral("/run/user/1000/"),
                                                       QStringLiteral("/tmp"), 1000);
    requireEqual(layout.legacyEndpoint, QStringLiteral("/run/user/1000/") + kBaseName,
                 "a trailing slash must not produce a doubled separator");
}

/// Without a session runtime directory the fallback is a per-user subdirectory of the
/// temp directory: the temp directory itself is world-writable and must not hold our
/// socket directly.
void tempFallbackIsPerUser() {
    const EndpointLayout layout =
        socketEndpointLayout(kBaseName, QString(), QStringLiteral("/tmp"), /*userId=*/1000);
    require(layout.error.isEmpty(), "the temp directory is an acceptable fallback");
    requireEqual(layout.directory, QStringLiteral("/tmp/snow-shot-1000"),
                 "the fallback directory carries the user id");
    require(layout.privateDirectory, "the fallback directory is ours to create");
    requireEqual(layout.legacyEndpoint, QStringLiteral("/tmp/snow-shot-1000/") + kBaseName,
                 "the socket lives inside the per-user fallback directory");

    const EndpointLayout other =
        socketEndpointLayout(kBaseName, QString(), QStringLiteral("/tmp"), /*userId=*/1001);
    require(other.directory != layout.directory,
            "two users on one machine must not share a fallback directory");
    require(other.legacyEndpoint != layout.legacyEndpoint,
            "two users on one machine must not share a socket path");
}

/// The XDG specification requires an absolute path; a relative one would silently be
/// taken as a name under the temp directory by QLocalServer, so it counts as unset.
void relativeRuntimeDirectoryIsIgnored() {
    const EndpointLayout layout = socketEndpointLayout(kBaseName, QStringLiteral("run/user/1000"),
                                                       QStringLiteral("/tmp"), 1000);
    requireEqual(layout.directory, QStringLiteral("/tmp/snow-shot-1000"),
                 "a relative runtime directory must fall back instead of being used");

    const EndpointLayout dangling =
        socketEndpointLayout(kBaseName, QStringLiteral("   "), QStringLiteral("/tmp"), 1000);
    requireEqual(dangling.directory, QStringLiteral("/tmp/snow-shot-1000"),
                 "a blank runtime directory is the same as an unset one");
}

/// Nothing usable at all: the message has to be produced here, because QLocalServer's
/// only diagnostic would be a strerror string.
void noUsableDirectoryIsReported() {
    const EndpointLayout layout = socketEndpointLayout(kBaseName, QString(), QString(), 1000);
    require(!layout.error.isEmpty(), "an unusable environment must be reported");
    require(layout.directory.isEmpty(), "no directory is claimed when none is usable");
    require(layout.legacyEndpoint.isEmpty(), "no endpoint is claimed when none is usable");
}

/// The kernel stores the path in sockaddr_un::sun_path, so the limit is bytes, not
/// characters, and it includes the terminator.
void socketPathLimitIsBytes() {
    require(socketPathFits(QStringLiteral("/") + QString(102, QLatin1Char('a'))), "103 bytes fits");
    require(!socketPathFits(QStringLiteral("/") + QString(103, QLatin1Char('a'))),
            "104 bytes does not fit");

    // 34 CJK characters are 34 characters but 102 bytes; with the leading slash that is
    // exactly the limit, and one more character overflows it.
    require(socketPathFits(QStringLiteral("/") + QString(34, QChar(0x4E2D))),
            "a multi-byte path is measured in bytes, not characters");
    require(!socketPathFits(QStringLiteral("/") + QString(35, QChar(0x4E2D))),
            "a path that is short in characters but long in bytes must be rejected");
}

/// A long runtime directory is not fatal: the temp directory is tried before giving up.
void longRuntimeDirectoryFallsBack() {
    const QString longDirectory = QStringLiteral("/") + QString(90, QLatin1Char('a'));
    const EndpointLayout layout =
        socketEndpointLayout(kBaseName, longDirectory, QStringLiteral("/tmp"), 1000);
    require(layout.error.isEmpty(), "a long runtime directory falls back instead of failing");
    requireEqual(layout.directory, QStringLiteral("/tmp/snow-shot-1000"),
                 "the fallback is used when the preferred directory cannot fit a socket");
}

/// When neither candidate fits, the message names the limit and the path the user
/// actually configured -- not the fallback they have never heard of.
void bothCandidatesTooLongIsReported() {
    const QString longRuntime = QStringLiteral("/") + QString(90, QLatin1Char('a'));
    const QString longTemp = QStringLiteral("/") + QString(90, QLatin1Char('b'));
    const EndpointLayout layout = socketEndpointLayout(kBaseName, longRuntime, longTemp, 1000);
    require(!layout.error.isEmpty(), "a path that cannot fit must be reported");
    require(layout.error.contains(QStringLiteral("103")), "the message names the byte limit");
    require(layout.error.contains(longRuntime),
            "the message names the directory that was asked for");
    requireEqual(layout.directory, longRuntime,
                 "the reported layout is the preferred candidate, not the fallback");
}

/// A directory in the world-writable temp directory is not ours to adopt. Refusing to
/// start is the lesser failure: a socket inside somebody else's directory is theirs to
/// replace, and the message names the path to remove.
void foreignEndpointDirectoryIsRefused() {
    require(isAcceptableEndpointDirectory(/*exists=*/false, /*isDirectory=*/false,
                                          /*ownerId=*/0, /*userId=*/1000),
            "a path that does not exist yet is created by us and is acceptable");
    require(isAcceptableEndpointDirectory(/*exists=*/true, /*isDirectory=*/true,
                                          /*ownerId=*/1000, /*userId=*/1000),
            "a directory owned by this user is acceptable");
    require(!isAcceptableEndpointDirectory(true, true, /*ownerId=*/1001, 1000),
            "a directory owned by another user must be refused");
    require(!isAcceptableEndpointDirectory(true, /*isDirectory=*/false, 1000, 1000),
            "a regular file with the expected name must be refused");
}
} // namespace

int main() {
    runtimeDirectoryIsPreferred();
    trailingSlashIsNormalised();
    tempFallbackIsPerUser();
    relativeRuntimeDirectoryIsIgnored();
    noUsableDirectoryIsReported();
    socketPathLimitIsBytes();
    longRuntimeDirectoryFallsBack();
    bothCandidatesTooLongIsReported();
    foreignEndpointDirectoryIsRefused();
    std::cout << "Linux single-instance endpoint tests passed\n";
}
