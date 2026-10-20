#include "snow_shot/app/singleinstanceendpoint.h"

#include <QDir>
#include <QVector>

#include <utility>

namespace snow_shot::app::ipc {
namespace {

bool usableDirectoryArgument(const QString& path) {
    return !path.trimmed().isEmpty() && QDir::isAbsolutePath(path);
}

QString userSuffix(qint64 userId) {
    return userId < 0 ? QStringLiteral("anon") : QString::number(userId);
}

EndpointLayout makeLayout(const QString& baseName, const QString& directory,
                          bool privateDirectory) {
    EndpointLayout layout;
    layout.directory = directory;
    layout.legacyEndpoint = QDir(directory).filePath(baseName);
    layout.receiptEndpoint = layout.legacyEndpoint + QStringLiteral("-v2");
    layout.lockFilePath = QDir(directory).filePath(baseName + QStringLiteral(".lock"));
    layout.privateDirectory = privateDirectory;
    return layout;
}

bool endpointsFit(const EndpointLayout& layout) {
    // The receipt endpoint is the longer of the two, but checking both keeps this
    // correct if the suffix ever changes.
    return socketPathFits(layout.legacyEndpoint) && socketPathFits(layout.receiptEndpoint);
}

QString tooLongError(const EndpointLayout& layout) {
    return QStringLiteral("The single-instance socket path does not fit the kernel's %1-byte "
                          "limit: %2 is %3 bytes. Point the session runtime directory "
                          "(XDG_RUNTIME_DIR) or TMPDIR at a shorter path.")
        .arg(kMaximumSocketPathBytes)
        .arg(layout.receiptEndpoint)
        .arg(layout.receiptEndpoint.toUtf8().size());
}

} // namespace

EndpointLayout socketEndpointLayout(const QString& baseName, const QString& runtimeDirectory,
                                    const QString& temporaryDirectory, qint64 userId) {
    // Ordered by preference: the session runtime directory is private and unswept,
    // the per-user subdirectory of the temp directory is neither.
    QVector<EndpointLayout> candidates;
    if (usableDirectoryArgument(runtimeDirectory)) {
        candidates.push_back(
            makeLayout(baseName, QDir::cleanPath(runtimeDirectory), /*privateDirectory=*/false));
    }
    if (usableDirectoryArgument(temporaryDirectory)) {
        candidates.push_back(
            makeLayout(baseName,
                       QDir(QDir::cleanPath(temporaryDirectory))
                           .filePath(QStringLiteral("snow-shot-") + userSuffix(userId)),
                       /*privateDirectory=*/true));
    }
    if (candidates.isEmpty()) {
        EndpointLayout layout;
        layout.error = QStringLiteral(
            "Neither the session runtime directory nor the temporary directory is usable");
        return layout;
    }
    for (const EndpointLayout& layout : std::as_const(candidates)) {
        if (endpointsFit(layout))
            return layout;
    }
    // Report the candidate the user asked for, not the fallback they have never
    // heard of: the first entry is the one their environment points at.
    EndpointLayout layout = candidates.front();
    layout.error = tooLongError(layout);
    return layout;
}

bool socketPathFits(const QString& absolutePath) {
    // QString::toUtf8(), not QFile::encodeName(): on Windows the latter converts to the
    // local code page and turns every unrepresentable character into a single '?', which
    // would make this check agree with the kernel on no platform but Windows.
    return absolutePath.toUtf8().size() <= kMaximumSocketPathBytes;
}

bool isAcceptableEndpointDirectory(bool exists, bool isDirectory, qint64 ownerId, qint64 userId) {
    if (!exists)
        return true;
    return isDirectory && ownerId == userId;
}

} // namespace snow_shot::app::ipc
