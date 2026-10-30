#ifndef SNOW_SHOT_PRESENTATION_PINNEDCOLORSWATCH_H
#define SNOW_SHOT_PRESENTATION_PINNEDCOLORSWATCH_H

#include <QColor>
#include <QFont>
#include <QImage>
#include <QRectF>
#include <QString>
#include <array>
#include <memory>
#include <optional>

class QTextDocument;

struct PinnedColorSwatchStyle final {
    QFont font;
    QColor background = Qt::white;
    QColor foreground = QColor(38, 38, 38);
    QColor secondary = QColor(140, 140, 140);
    QColor link = QColor(22, 119, 255);
    QString colorLabel;
    std::array<QString, 3> labels;
    std::array<QString, 3> copyLabels;
    QString rgbaLabel;
    QString hslaLabel;
};

struct PinnedColorSwatchValues final {
    QColor color;
    std::array<QString, 3> display;
    std::array<QString, 3> clipboard;
};

struct PinnedColorSwatchRendering final {
    QImage image;
    std::shared_ptr<QTextDocument> document;
    std::array<QRectF, 3> copyRects;
};

[[nodiscard]] std::optional<PinnedColorSwatchValues> parsePinnedColorSwatch(const QString& text);
// GUI-thread capture; render and parse only consume immutable values.
[[nodiscard]] PinnedColorSwatchStyle capturePinnedColorSwatchStyle();
[[nodiscard]] PinnedColorSwatchRendering
renderPinnedColorSwatch(const PinnedColorSwatchValues& values, const PinnedColorSwatchStyle& style,
                        qreal dpr, const QSize& logicalSize = QSize(342, 210));

#endif
