#include "snow_shot/storage/clipboardfilecache.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
using snow_shot::storage::ClipboardFileCache;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

QDateTime fixtureTime() {
    return QDateTime::fromString(QStringLiteral("2026-10-14T12:00:00Z"), Qt::ISODate);
}

QString writeFile(const ClipboardFileCache::ExportDirectory& directory) {
    const QString path = QDir(directory.path()).filePath(QStringLiteral("image.png"));
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write("image") == 5,
            "clipboard cache fixture must be writable");
    return path;
}

void stagingAndRetainedFilesHaveSeparateLifetimes() {
    QTemporaryDir root;
    require(root.isValid(), "clipboard cache fixture root must be available");
    QString retainedPath;
    {
        ClipboardFileCache cache(root.path());
        QString error = QStringLiteral("stale error");
        auto staged = cache.createDirectory(&error, fixtureTime());
        require(staged && error.isEmpty(), "clipboard staging must clear previous errors");
        const QString discardedPath = writeFile(*staged);
        staged.reset();
        require(!QFileInfo::exists(QFileInfo(discardedPath).path()),
                "failed or cancelled staging must remove its entire directory");

        auto retained = cache.createDirectory(&error, fixtureTime());
        require(bool(retained), "completed clipboard export must have a staging directory");
        retainedPath = writeFile(*retained);
        retained->retain();
        retained.reset();
        require(QFileInfo::exists(retainedPath),
                "completed files must outlive the export directory wrapper");
    }
    require(QFileInfo::exists(retainedPath), "cache destruction must preserve completed files");
}

void expiryPreservesRecentAndActiveExports() {
    QTemporaryDir root;
    require(root.isValid(), "clipboard cache fixture root must be available");
    ClipboardFileCache cache(root.path());
    QString error;
    const QDateTime createdAt = fixtureTime();
    auto first = cache.createDirectory(&error, createdAt);
    auto second = cache.createDirectory(&error, createdAt.addSecs(1));
    auto active = cache.createDirectory(&error, createdAt);
    require(first && second && active, "clipboard exports must have independent directories");
    const QString firstPath = writeFile(*first);
    const QString secondPath = writeFile(*second);
    const QString activePath = writeFile(*active);
    require(firstPath != secondPath, "identical export filenames must never collide");
    first->retain();
    second->retain();
    active->retain();
    first.reset();
    second.reset();

    const QDateTime expiresAt = createdAt.addDays(ClipboardFileCache::RetentionDays);
    cache.removeExpired(expiresAt.addMSecs(-1));
    require(QFileInfo::exists(firstPath) && QFileInfo::exists(secondPath),
            "cache cleanup must preserve the full retention interval");
    cache.removeExpired(expiresAt);
    require(!QFileInfo::exists(QFileInfo(firstPath).path()) && QFileInfo::exists(secondPath) &&
                QFileInfo::exists(activePath),
            "expiry must remove old exports while preserving recent files and active writers");
    active.reset();
    cache.removeExpired(expiresAt.addSecs(1));
    require(!QFileInfo::exists(QFileInfo(secondPath).path()) &&
                !QFileInfo::exists(QFileInfo(activePath).path()),
            "completed writers must become eligible for independent cache expiry");
}

void newExportsCleanExpiredFilesWithoutRemovingOtherDirectories() {
    QTemporaryDir root;
    require(root.isValid(), "clipboard cache fixture root must be available");
    ClipboardFileCache cache(root.path());
    QString error;
    auto old = cache.createDirectory(&error, fixtureTime());
    require(bool(old), "old clipboard fixture must be available");
    const QString oldPath = writeFile(*old);
    old->retain();
    old.reset();
    const QString unrelated = root.filePath(QStringLiteral("other-1000000000000-abcdef"));
    const QString malformed = root.filePath(QStringLiteral("export-invalid-abcdef"));
    require(QDir().mkpath(unrelated) && QDir().mkpath(malformed),
            "unrelated cache fixtures must be available");
    auto next =
        cache.createDirectory(&error, fixtureTime().addDays(ClipboardFileCache::RetentionDays));
    require(next && !QFileInfo::exists(oldPath) && QFileInfo::exists(unrelated) &&
                QFileInfo::exists(malformed),
            "new exports must reclaim expired files without deleting other directories");
}

void abandonedWritersAreReclaimedAfterExpiry() {
    QTemporaryDir root;
    require(root.isValid(), "abandoned clipboard cache fixture must be available");
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--abandon-directory"), root.path()});
    require(child.waitForStarted(2000) && child.waitForFinished(5000) && child.exitCode() == 0 &&
                child.exitStatus() == QProcess::NormalExit,
            "abandoned clipboard staging process must finish");
    const QString path = QString::fromUtf8(child.readAllStandardOutput()).trimmed();
    require(QDir::fromNativeSeparators(path).startsWith(QDir::fromNativeSeparators(root.path()) +
                                                        u'/') &&
                QFileInfo::exists(path),
            "abandoned process must leave its own staging file");
    ClipboardFileCache cache(root.path());
    cache.removeExpired(fixtureTime().addDays(ClipboardFileCache::RetentionDays));
    require(!QFileInfo::exists(QFileInfo(path).path()),
            "expiry must reclaim abandoned directories and stale process locks");
}

void unavailableCacheReportsFilesystemFailure() {
    QTemporaryDir root;
    require(root.isValid(), "unavailable clipboard cache fixture must be available");
    const QString path = root.filePath(QStringLiteral("file"));
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "clipboard cache blocker must be writable");
    file.close();
    ClipboardFileCache cache(path);
    QString error;
    require(!cache.createDirectory(&error, fixtureTime()) && !error.isEmpty(),
            "an unavailable clipboard cache must report its filesystem error");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--abandon-directory"))) {
        ClipboardFileCache cache(app.arguments().last());
        QString error;
        auto directory = cache.createDirectory(&error, fixtureTime());
        require(bool(directory), "abandoned clipboard staging directory must be available");
        std::cout << writeFile(*directory).toStdString() << std::endl;
        // Simulate process termination before the writer can release its staging lock.
        std::_Exit(EXIT_SUCCESS);
    }
    stagingAndRetainedFilesHaveSeparateLifetimes();
    expiryPreservesRecentAndActiveExports();
    newExportsCleanExpiredFilesWithoutRemovingOtherDirectories();
    abandonedWritersAreReclaimedAfterExpiry();
    unavailableCacheReportsFilesystemFailure();
    return EXIT_SUCCESS;
}
