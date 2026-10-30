#ifndef SNOW_SHOT_PRESENTATION_SCREENRECORDINGFOLDER_H
#define SNOW_SHOT_PRESENTATION_SCREENRECORDINGFOLDER_H

#include "snow_shot/storage/screenshotexportvariables.h"

#include <QStringList>
#include <QDateTime>

namespace snow_shot::presentation::recording {
[[nodiscard]] QStringList
screenRecordingDirectories(const QDateTime& timestamp = QDateTime::currentDateTime(),
                           const storage::ScreenshotWindowNames& names = {});
[[nodiscard]] QString
screenRecordingDirectory(const QDateTime& timestamp = QDateTime::currentDateTime(),
                         const storage::ScreenshotWindowNames& names = {});
// The caller snapshots and expands the filename format before any worker handoff.
[[nodiscard]] QString screenRecordingOutputPath(const QStringList& directories,
                                                const QString& baseName, const QString& extension);
[[nodiscard]] bool openScreenRecordingFolder(const storage::ScreenshotWindowNames& names = {});
[[nodiscard]] bool revealScreenRecordingFile(const QString& path);
} // namespace snow_shot::presentation::recording

#endif // SNOW_SHOT_PRESENTATION_SCREENRECORDINGFOLDER_H
