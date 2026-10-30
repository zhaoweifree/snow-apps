// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/storage/exportfilename.h"

#include <utility>

namespace snow_shot::storage {
namespace {
QString truncateUtf8(QString name, qsizetype maximumBytes) {
    if (name.size() > maximumBytes) {
        name.truncate(maximumBytes);
        if (!name.isEmpty() && name.back().isHighSurrogate())
            name.chop(1);
    }
    while (name.toUtf8().size() > maximumBytes) {
        name.chop(1);
        if (!name.isEmpty() && name.back().isHighSurrogate())
            name.chop(1);
    }
    return name;
}
} // namespace

QString sanitizeExportFileName(QString name, QStringView fallback, ExportFileNamePolicy policy) {
    constexpr QStringView forbidden(u"<>:\"/\\|?*");
    for (QChar& character : name) {
        const ushort value = character.unicode();
        if (value < 0x20 ||
            (policy.controls == ExportFileNameControls::C0AndC1 && value >= 0x7f &&
             value <= 0x9f) ||
            forbidden.contains(character)) {
            character = u'_';
        }
    }
    name = name.trimmed();
    if (policy.maximumUtf8Bytes > 0)
        name = truncateUtf8(std::move(name), policy.maximumUtf8Bytes);
    while (name.endsWith(u'.') || name.endsWith(u' '))
        name.chop(1);
    return name.isEmpty() ? fallback.toString() : name;
}

QString composeExportFileName(QString stem, QStringView suffix, qsizetype maximumUtf8Bytes) {
    const qsizetype suffixBytes = suffix.toUtf8().size();
    Q_ASSERT(maximumUtf8Bytes >= 0 && maximumUtf8Bytes <= maximumExportPathComponentUtf8Bytes);
    Q_ASSERT(suffixBytes <= maximumUtf8Bytes);
    const qsizetype originalSize = stem.size();
    stem = truncateUtf8(std::move(stem), maximumUtf8Bytes - suffixBytes);
    // Truncating a directory component must not introduce a trailing dot or space on Windows.
    if (stem.size() != originalSize) {
        while (stem.endsWith(u'.') || stem.endsWith(u' '))
            stem.chop(1);
    }
    return stem + suffix;
}
} // namespace snow_shot::storage
