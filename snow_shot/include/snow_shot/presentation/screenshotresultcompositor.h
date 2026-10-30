#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRESULTCOMPOSITOR_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRESULTCOMPOSITOR_H

#include "snow_shot/image/screenshotlayoutcompaction.h"

#include <QColor>
#include <QDataStream>
#include <QImage>
#include <QMargins>
#include <QRect>
#include <QRectF>
#include <QSize>
#include "snow_shot/compat/qt_compat.h"
#include "snow_shot/image/screenshotregiongeometry.h"
#include <QPainterPath>
#include <QIODevice>
#include <optional>

class QPainter;

struct ScreenshotResultStyle {
    int cornerRadius = 0;
    int shadowWidth = 0;
    QColor shadowColor = QColor(0x33, 0x33, 0x33);
    // Immutable geometry snapshot, relative to the content origin, in canvas units.
    std::optional<ScreenshotRegionGeometry> region = std::nullopt;
    qreal regionScale = 1.0;
    int backgroundOpacityPercent = 0;
};

inline QByteArray encodeScreenshotResultStyle(const ScreenshotResultStyle& style) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    // These bytes go to disk without a Qt stream-version header, so the format
    // has to match the records written by earlier builds. QDataStream::Version
    // tops out at Qt_5_15 on Qt 5.15 (the Qt_6_* values are Qt6-only), so the
    // version comes from snowDataStreamVersion(), which reports the highest one
    // this build supports. The encoding of the basic types used here
    // (int/qreal/QColor/QByteArray) is identical between the two versions, so
    // existing records stay readable.
    // Keep this comment ASCII: some TUs that include this header are not
    // compiled with /utf-8.
    stream.setVersion(snowDataStreamVersion());
    stream << style.cornerRadius << style.shadowWidth << style.shadowColor;
    stream << quint32(0x53535247) << quint8(style.backgroundOpacityPercent == 0 ? 1 : 2)
           << style.regionScale
           << (style.region ? QJsonDocument(style.region->toJson()).toJson(QJsonDocument::Compact)
                            : QByteArray());
    if (style.backgroundOpacityPercent != 0)
        stream << style.backgroundOpacityPercent;
    return bytes;
}

// An invalid extension rejects the record rather than discarding persisted selection geometry.
inline std::optional<ScreenshotResultStyle> decodeScreenshotResultStyle(const QByteArray& bytes) {
    ScreenshotResultStyle style;
    if (bytes.isEmpty())
        return style;
    QDataStream stream(bytes);
    stream.setVersion(snowDataStreamVersion());
    stream >> style.cornerRadius >> style.shadowWidth >> style.shadowColor;
    if (stream.status() != QDataStream::Ok)
        return std::nullopt;
    if (stream.atEnd())
        return style;
    quint32 marker = 0;
    quint8 version = 0;
    QByteArray geometry;
    stream >> marker >> version >> style.regionScale >> geometry;
    if (version == 2)
        stream >> style.backgroundOpacityPercent;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || marker != 0x53535247 ||
        (version != 1 && version != 2) || style.backgroundOpacityPercent < 0 ||
        style.backgroundOpacityPercent > 100 || !std::isfinite(style.regionScale) ||
        style.regionScale <= 0 || style.regionScale > 256)
        return std::nullopt;
    if (!geometry.isEmpty()) {
        style.region =
            ScreenshotRegionGeometry::fromJson(QJsonDocument::fromJson(geometry).object());
        if (!style.region || style.region->isEmpty())
            return std::nullopt;
    }
    return style;
}

struct ScreenshotResultLayout {
    QRect contentRect;
    QRect outputRect;
    QMargins effectInsets;
    qreal devicePixelRatio = 1.0;

    [[nodiscard]] bool isValid() const {
        return contentRect.isValid() && !contentRect.isEmpty() && outputRect.isValid() &&
               !outputRect.isEmpty() && outputRect.contains(contentRect);
    }
};

[[nodiscard]] QPainterPath screenshotRegionPath(const ScreenshotRegionGeometry& region,
                                                qreal radius = 0.0);

class ScreenshotResultCompositor final {
  public:
    [[nodiscard]] static ScreenshotResultStyle normalizedStyle(const ScreenshotResultStyle& style);
    [[nodiscard]] static ScreenshotResultLayout layoutForContent(const QSize& contentPixelSize,
                                                                 const ScreenshotResultStyle& style,
                                                                 qreal devicePixelRatio = 1.0);
    [[nodiscard]] static QImage normalizeImage(const QImage& image);
    // Prepare an output-aligned, premultiplied background for destination-over
    // painting. Geometric coverage keeps translucent selected pixels unchanged.
    // Preview surfaces use the same operation as exports before adding checkerboard.
    [[nodiscard]] static QImage maskBackgroundForSelection(QImage background, const QImage& content,
                                                           const QPainterPath& selectionPath,
                                                           const QPoint& contentOffset = {});
    // Background uses output pixel coordinates, including effect padding, and contains
    // capture pixels only. Its opacity is applied beneath the masked content and shadow.
    [[nodiscard]] static QImage compose(const QImage& content, const ScreenshotResultStyle& style,
                                        qreal devicePixelRatio = 1.0, qreal outputOpacity = 1.0,
                                        const QImage& background = {});

    // Clear the part of the viewport outside the result shape, then place the
    // shadow behind the content. Content may cross the viewport boundary:
    // native pixel extents and integer-DIP widget extents round independently.
    static void finishLiveSurface(QPainter& painter, const QRectF& viewportBounds,
                                  const QRectF& contentBounds, const ScreenshotResultStyle& style,
                                  qreal devicePixelRatio, qreal canvasToViewScale = 1.0);
};

// The compaction plan operates on pixels; bake the original mask before moving
// it so newly touching regions do not lose their individually rounded corners.
inline ScreenshotResultStyle
compactedScreenshotResultStyle(const ScreenshotResultStyle& style,
                               const ScreenshotLayoutCompactionPlan& plan) {
    if (!plan.changed || !style.region)
        return style;
    auto result = ScreenshotResultCompositor::normalizedStyle(style);
    const qreal scale = result.regionScale;
    const auto mask =
        QTransform::fromScale(scale, scale)
            .map(result.region->custom()
                     ? result.region->path(scale)
                     : screenshotRegionPath(*result.region, result.cornerRadius / scale));
    result.region =
        ScreenshotRegionGeometry::fromPath(plan.mapPath(mask), ScreenshotRegionType::Curve);
    result.regionScale = 1.0;
    result.cornerRadius = 0;
    return result;
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRESULTCOMPOSITOR_H
