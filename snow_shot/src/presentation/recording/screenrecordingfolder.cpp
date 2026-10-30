#include "snow_shot/presentation/screenrecordingfolder.h"

#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/storage/exportfilename.h"
#include "snow_shot/storage/timevariables.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

namespace snow_shot::presentation::recording {
namespace {
QStringList defaultRecordingDirectories() {
    QStringList directories;
    for (QStandardPaths::StandardLocation location :
         {QStandardPaths::MoviesLocation, QStandardPaths::DocumentsLocation}) {
        const QString directory = QStandardPaths::writableLocation(location);
        if (!directory.isEmpty() && !directories.contains(directory, Qt::CaseInsensitive)) {
            directories.push_back(directory);
        }
    }
    return directories;
}
} // namespace

QStringList screenRecordingDirectories(const QDateTime& timestamp,
                                       const storage::ScreenshotWindowNames& names) {
    QStringList directories;
    const QString configuredTemplate = storage::RecordingSettings().videoSaveDirectory();
    const QString expanded =
        storage::expandScreenshotExportVariables(configuredTemplate, timestamp, names);
    const QString configured = expanded.isEmpty() ? QString() : QDir::cleanPath(expanded);
    const QFileInfo configuredInfo(configured);
    if (!configured.isEmpty() && ((configuredInfo.isDir() && configuredInfo.isWritable()) ||
                                  storage::containsTimeVariables(configuredTemplate))) {
        directories.push_back(configured);
    }
    for (const QString& fallback : defaultRecordingDirectories()) {
        if (!directories.contains(fallback, Qt::CaseInsensitive)) {
            directories.push_back(fallback);
        }
    }
    return directories;
}

QString screenRecordingDirectory(const QDateTime& timestamp,
                                 const storage::ScreenshotWindowNames& names) {
    const QStringList directories = screenRecordingDirectories(timestamp, names);
    for (const QString& candidate : directories) {
        QDir directory(candidate);
        if ((directory.exists() || directory.mkpath(QStringLiteral("."))) &&
            QFileInfo(directory.absolutePath()).isWritable()) {
            return directory.absolutePath();
        }
    }
    return directories.isEmpty() ? QString() : directories.constFirst();
}

QString screenRecordingOutputPath(const QStringList& directories, const QString& baseName,
                                  const QString& extension) {
    const QString normalizedExtension =
        extension.startsWith(QLatin1Char('.')) ? extension : QStringLiteral(".%1").arg(extension);
    for (const QString& candidate : directories) {
        QDir directory(candidate);
        if ((!directory.exists() && !directory.mkpath(QStringLiteral("."))) ||
            !QFileInfo(directory.absolutePath()).isWritable()) {
            continue;
        }
        QString path =
            directory.filePath(storage::composeExportFileName(baseName, normalizedExtension));
        for (int suffix = 1; QFileInfo::exists(path); ++suffix) {
            path = directory.filePath(storage::composeExportFileName(
                baseName, QStringLiteral("_%1%2").arg(suffix).arg(normalizedExtension)));
        }
        return path;
    }
    return {};
}

bool openScreenRecordingFolder(const storage::ScreenshotWindowNames& names) {
    const QString path = screenRecordingDirectory(QDateTime::currentDateTime(), names);
    return !path.isEmpty() && QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

bool revealScreenRecordingFile(const QString& path) {
    const QFileInfo file(path);
    if (path.isEmpty() || !file.isFile())
        return false;
#ifdef Q_OS_WIN
    QProcess explorer;
    explorer.setProgram(QStringLiteral("explorer.exe"));
    explorer.setNativeArguments(
        QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(file.absoluteFilePath())));
    return explorer.startDetached();
#elif defined(Q_OS_MACOS)
    return QProcess::startDetached(QStringLiteral("/usr/bin/open"),
                                   {QStringLiteral("-R"), file.absoluteFilePath()});
#else
    return QDesktopServices::openUrl(QUrl::fromLocalFile(file.absolutePath()));
#endif
}
} // namespace snow_shot::presentation::recording
