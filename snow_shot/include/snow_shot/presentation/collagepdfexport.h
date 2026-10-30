// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COLLAGEPDFEXPORT_H
#define SNOW_SHOT_PRESENTATION_COLLAGEPDFEXPORT_H

#include "snow_shot/presentation/collagedocument.h"

#include <QStringList>

#include <functional>

namespace snow_shot::presentation {

struct CollagePdfDestination final {
    // A filename for combined export, or a directory for per-page export.
    QString path;
    bool overwriteAuthorized = false;
};

struct CollagePdfExportProgress final {
    qsizetype completedPages = 0;
    qsizetype totalPages = 0;
    qsizetype pageNumber = 0;
    qreal pageFraction = 0;
};

struct CollagePdfExportResult final {
    qsizetype totalCount = 0;
    QVector<qsizetype> completedPages;
    QStringList paths;
    bool cancelled = false;
    QString error;
    QString errorSource;
    QStringList errorArguments;

    [[nodiscard]] bool succeeded() const;
    [[nodiscard]] QString errorMessage() const;
};

// Run on an export worker with an immutable item/settings snapshot. Cancellation retires the
// current staging file; complete per-page PDFs remain available and are returned in page order.
[[nodiscard]] CollagePdfExportResult
exportCollagePdf(const QVector<CollageItem>& items, const CollagePdfSettings& settings,
                 const CollagePdfDestination& destination, std::function<bool()> cancelled = {},
                 std::function<void(const CollagePdfExportProgress&)> progress = {});

} // namespace snow_shot::presentation

#endif
