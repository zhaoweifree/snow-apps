// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_STYLES_SEQUENCENUMBERBADGESTYLE_H
#define SNOW_SHOT_PRESENTATION_STYLES_SEQUENCENUMBERBADGESTYLE_H

#include <QFontMetrics>
#include <QPainter>

#include <algorithm>

namespace snow_shot::presentation::styles {

struct SequenceNumberBadgeStyle final {
    static constexpr int inset = 4;
    static constexpr int height = 22;
    static constexpr int radius = 4;
    static constexpr int backgroundAlpha = 130;

    [[nodiscard]] static QSize size(const QFontMetrics& metrics, const QString& label) {
        return {std::max(24, metrics.horizontalAdvance(label) + 12), height};
    }

    [[nodiscard]] static QString styleSheet() {
        return QStringLiteral("color: white; background-color: rgba(0, 0, 0, %1); "
                              "border-radius: %2px;")
            .arg(backgroundAlpha)
            .arg(radius);
    }

    static void paint(QPainter& painter, const QRectF& rect, const QString& label) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, backgroundAlpha));
        painter.drawRoundedRect(rect, radius, radius);
        painter.setPen(Qt::white);
        painter.drawText(rect, Qt::AlignCenter, label);
        painter.restore();
    }
};

} // namespace snow_shot::presentation::styles

#endif // SNOW_SHOT_PRESENTATION_STYLES_SEQUENCENUMBERBADGESTYLE_H
