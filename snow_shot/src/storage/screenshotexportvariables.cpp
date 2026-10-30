// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/storage/screenshotexportvariables.h"
#include "snow_shot/storage/exportfilename.h"
#include "snow_shot/storage/timevariables.h"

#include <QRegularExpression>

namespace snow_shot::storage {
namespace {
QString pathComponent(const QString& value) {
    QString result = sanitizeExportFileName(
        value, u"Unknown", {ExportFileNameControls::C0AndC1, maximumExportPathComponentUtf8Bytes});
    // Windows device names remain reserved even when followed by an extension.
    static const QRegularExpression reserved(
        QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])(?:\\.|$)"),
        QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(result).hasMatch())
        result.prepend(u'_');
    return result;
}
} // namespace

bool containsScreenshotWindowVariables(const QString& text) {
    return text.contains(QStringLiteral("{AN}")) || text.contains(QStringLiteral("{WN}"));
}

ScreenshotWindowNameFields screenshotWindowNameFields(const QString& directory,
                                                      const QString& filename) {
    return {directory.contains(QStringLiteral("{AN}")) || filename.contains(QStringLiteral("{AN}")),
            directory.contains(QStringLiteral("{WN}")) ||
                filename.contains(QStringLiteral("{WN}"))};
}

QString expandScreenshotExportVariables(const QString& text, const QDateTime& timestamp,
                                        const ScreenshotWindowNames& names) {
    static const QRegularExpression placeholders(QStringLiteral("\\{([^{}]+)\\}"));
    const QString input = text.trimmed();
    QString result;
    qsizetype position = 0;
    auto matches = placeholders.globalMatch(input);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += QStringView(input).mid(position, match.capturedStart() - position);
        const QString token = match.captured(1);
        if (token == QStringLiteral("AN"))
            result += pathComponent(names.applicationName);
        else if (token == QStringLiteral("WN"))
            result += pathComponent(names.windowName);
        else
            result += expandTimeVariables(match.captured(), timestamp);
        position = match.capturedEnd();
    }
    result += QStringView(input).mid(position);
    // A component can combine multiple window variables with literal text and time variables.
    // Bound the complete component too, preserving separators (including drives and UNC roots).
    QString bounded;
    qsizetype componentStart = 0;
    for (qsizetype index = 0; index <= result.size(); ++index) {
        if (index != result.size() && result.at(index) != u'/' && result.at(index) != u'\\')
            continue;
        bounded += composeExportFileName(result.mid(componentStart, index - componentStart), {},
                                         maximumExportPathComponentUtf8Bytes);
        if (index != result.size())
            bounded += result.at(index);
        componentStart = index + 1;
    }
    return bounded;
}
} // namespace snow_shot::storage
