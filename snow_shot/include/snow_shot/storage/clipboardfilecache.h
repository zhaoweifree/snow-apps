#ifndef SNOW_SHOT_STORAGE_CLIPBOARDFILECACHE_H
#define SNOW_SHOT_STORAGE_CLIPBOARDFILECACHE_H

#include <QDateTime>
#include <QMutex>
#include <QString>
#include <QTemporaryDir>

#include <memory>

class QLockFile;

namespace snow_shot::storage {
// Completed exports have a disk lifetime independent of clipboard publications,
// receivers and application shutdown. Staging directories still use RAII cleanup.
class ClipboardFileCache final {
  public:
    static constexpr int RetentionDays = 7;

    class ExportDirectory final {
      public:
        ~ExportDirectory();
        [[nodiscard]] QString path() const;
        void retain();

      private:
        friend class ClipboardFileCache;
        explicit ExportDirectory(const QString& pathTemplate);
        QTemporaryDir m_directory;
        // Destroy the lock before removing an uncommitted staging directory.
        std::unique_ptr<QLockFile> m_lock;
        QString m_error;
    };

    explicit ClipboardFileCache(QString root);
    [[nodiscard]] static ClipboardFileCache& shared();
    [[nodiscard]] std::unique_ptr<ExportDirectory>
    createDirectory(QString* error, QDateTime now = QDateTime::currentDateTimeUtc());
    // Filesystem work belongs on the export worker. Expiry uses creation time,
    // never clipboard ownership or the image's requested filename timestamp.
    void removeExpired(QDateTime now = QDateTime::currentDateTimeUtc()) const;

  private:
    QString m_root;
    QMutex m_cleanupMutex;
    QDateTime m_nextCleanup;
};
} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_CLIPBOARDFILECACHE_H
