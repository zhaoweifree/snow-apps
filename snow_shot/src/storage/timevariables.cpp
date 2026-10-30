// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/storage/timevariables.h"

#include <QRegularExpression>

namespace snow_shot::storage {
namespace {
const QRegularExpression& placeholderPattern() {
    static const QRegularExpression pattern(QStringLiteral("\\{([^{}]+)\\}"));
    return pattern;
}
} // namespace

bool containsTimeVariables(const QString& text) {
    return placeholderPattern().match(text).hasMatch();
}

QString expandTimeVariables(const QString& text, const QDateTime& timestamp) {
    QString result = text.trimmed();
    QList<QRegularExpressionMatch> matches;
    auto iterator = placeholderPattern().globalMatch(result);
    while (iterator.hasNext()) {
        matches.push_back(iterator.next());
    }
    for (auto match = matches.crbegin(); match != matches.crend(); ++match) {
        QString dateTimeFormat = match->captured(1);
        dateTimeFormat.replace(QStringLiteral("YYYY"), QStringLiteral("yyyy"));
        dateTimeFormat.replace(QStringLiteral("YY"), QStringLiteral("yy"));
        dateTimeFormat.replace(QStringLiteral("DD"), QStringLiteral("dd"));
        result.replace(match->capturedStart(), match->capturedLength(),
                       timestamp.toString(dateTimeFormat));
    }
    return result;
}
} // namespace snow_shot::storage
