#include "snow_shot/storage/clipboardfilecache.h"

#include <QCoreApplication>
#include <QDir>
#include <QLockFile>
#include <QMutexLocker>

#include <utility>

namespace snow_shot::storage {
ClipboardFileCache::ExportDirectory::ExportDirectory(const QString& pathTemplate)
    : m_directory(pathTemplate) {
    if (!m_directory.isValid()) {
        m_error = m_directory.errorString();
        return;
    }
    m_lock = std::make_unique<QLockFile>(m_directory.filePath(QStringLiteral(".lock")));
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock(0)) {
        m_error = QCoreApplication::translate("ScreenshotController",
                                              "The clipboard export directory could not be locked");
    }
}

ClipboardFileCache::ExportDirectory::~ExportDirectory() = default;

QString ClipboardFileCache::ExportDirectory::path() const {
    return m_directory.path();
}

void ClipboardFileCache::ExportDirectory::retain() {
    m_directory.setAutoRemove(false);
}

ClipboardFileCache::ClipboardFileCache(QString root) : m_root(std::move(root)) {}

ClipboardFileCache& ClipboardFileCache::shared() {
    static ClipboardFileCache cache(QDir::temp().filePath(QStringLiteral("snow-shot-clipboard")));
    return cache;
}

std::unique_ptr<ClipboardFileCache::ExportDirectory>
ClipboardFileCache::createDirectory(QString* error, QDateTime now) {
    // If creating the cache root fails, QTemporaryDir supplies the filesystem error below.
    static_cast<void>(QDir().mkpath(m_root));
    {
        QMutexLocker lock(&m_cleanupMutex);
        if (!m_nextCleanup.isValid() || now >= m_nextCleanup) {
            removeExpired(now);
            m_nextCleanup = now.addSecs(60 * 60);
        }
    }
    auto directory = std::unique_ptr<ExportDirectory>(new ExportDirectory(
        QDir(m_root).filePath(QStringLiteral("export-%1-XXXXXX").arg(now.toMSecsSinceEpoch()))));
    if (!directory->m_error.isEmpty()) {
        if (error)
            *error = directory->m_error;
        return {};
    }
    if (error)
        error->clear();
    return directory;
}

void ClipboardFileCache::removeExpired(QDateTime now) const {
    const qint64 cutoff = now.addDays(-RetentionDays).toMSecsSinceEpoch();
    const auto directories = QDir(m_root).entryInfoList(
        {QStringLiteral("export-*")}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const auto& directory : directories) {
        const auto parts = directory.fileName().split(u'-');
        bool validTimestamp = false;
        const qint64 createdAt = parts.size() == 3 ? parts.at(1).toLongLong(&validTimestamp) : 0;
        if (!validTimestamp || createdAt <= 0 || createdAt > cutoff || parts.at(2).size() != 6)
            continue;
        QLockFile lock(QDir(directory.absoluteFilePath()).filePath(QStringLiteral(".lock")));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0))
            continue;
        // No writer can reopen an expired, uniquely named export. Unlock before
        // recursive removal so Windows can delete the lock file as well.
        lock.unlock();
        static_cast<void>(QDir(directory.absoluteFilePath()).removeRecursively());
    }
}
} // namespace snow_shot::storage
