// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_STORAGE_EXPORTFILENAME_H
#define SNOW_SHOT_STORAGE_EXPORTFILENAME_H

#include <QString>
#include <QStringView>

namespace snow_shot::storage {
// UTF-8 bytes are a conservative limit for both byte- and UTF-16-based filesystems.
inline constexpr qsizetype maximumExportPathComponentUtf8Bytes = 255;
// QSaveFile appends .XXXXXX while staging an atomic write in the destination directory.
inline constexpr qsizetype maximumExportFileNameUtf8Bytes = maximumExportPathComponentUtf8Bytes - 7;

enum class ExportFileNameControls { C0, C0AndC1 };

struct ExportFileNamePolicy {
    ExportFileNameControls controls = ExportFileNameControls::C0;
    // Zero preserves the complete stem. Positive limits never split a surrogate pair.
    qsizetype maximumUtf8Bytes = 0;
};

// The caller owns path/extension interpretation and the feature's fallback stem.
QString sanitizeExportFileName(QString name, QStringView fallback,
                               ExportFileNamePolicy policy = {});

// Compose a stem and a caller-owned suffix (extension, collision index, etc.).
// Only the stem is shortened, without splitting Unicode; suffix must fit maximumUtf8Bytes.
// File names reserve atomic-write space by default; directories can use the full component limit.
[[nodiscard]] QString
composeExportFileName(QString stem, QStringView suffix,
                      qsizetype maximumUtf8Bytes = maximumExportFileNameUtf8Bytes);
} // namespace snow_shot::storage

#endif
