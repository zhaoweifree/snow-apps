#include "snow_shot/presentation/pinnedcolorswatch.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"

#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QThread>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
std::optional<double> number(QString text, double maximum, bool percentage = false,
                             double minimum = 0) {
    if (percentage) {
        if (!text.endsWith(QLatin1Char('%')))
            return {};
        text.chop(1);
    }
    static const QRegularExpression syntax(
        QStringLiteral("^[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)$"));
    if (!syntax.match(text).hasMatch())
        return {};
    bool ok = false;
    const double value = text.toDouble(&ok);
    if (!ok || !std::isfinite(value) || value < minimum || value > maximum)
        return {};
    return value;
}

std::optional<QColor> parse(const QString& input) {
    const auto text = input.trimmed();
    if (text.size() > 256)
        return {};
    static const QRegularExpression hexPattern(
        QStringLiteral(
            "^(?:#([0-9a-f]{3}|[0-9a-f]{4}|[0-9a-f]{6}|[0-9a-f]{8})|([0-9a-f]{6}|[0-9a-f]{8}))$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto hexMatch = hexPattern.match(text);
    if (hexMatch.hasMatch()) {
        auto hex = hexMatch.captured(1).isEmpty() ? hexMatch.captured(2) : hexMatch.captured(1);
        if (hex.size() < 5) {
            QString expanded;
            for (const auto c : hex) {
                expanded += c;
                expanded += c;
            }
            hex = expanded;
        }
        return QColor(hex.mid(0, 2).toInt(nullptr, 16), hex.mid(2, 2).toInt(nullptr, 16),
                      hex.mid(4, 2).toInt(nullptr, 16),
                      hex.size() == 8 ? hex.mid(6, 2).toInt(nullptr, 16) : 255);
    }
    static const QRegularExpression functionPattern(
        QStringLiteral("^(rgb|rgba|hsl|hsla)\\(\\s*([^()]*)\\s*\\)$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = functionPattern.match(text);
    if (!match.hasMatch())
        return {};
    const auto function = match.captured(1).toLower();
    const auto body = match.captured(2).trimmed();
    QStringList components;
    QString alpha;
    if (body.contains(QLatin1Char(','))) {
        if (body.contains(QLatin1Char('/')))
            return {};
        components = body.split(QLatin1Char(','));
        for (auto& component : components)
            component = component.trimmed();
        if (components.size() == 4) {
            alpha = components.takeLast();
            if (alpha.isEmpty())
                return {};
        }
        if (components.size() != 3 || (function.endsWith(QLatin1Char('a')) && alpha.isEmpty()))
            return {};
    } else {
        const auto parts = body.split(QLatin1Char('/'));
        if (parts.size() > 2)
            return {};
        static const QRegularExpression whitespace(QStringLiteral("\\s+"));
        components = parts.first().trimmed().split(whitespace, Qt::SkipEmptyParts);
        if (parts.size() == 2) {
            alpha = parts.last().trimmed();
            if (alpha.isEmpty())
                return {};
        }
        if (components.size() != 3)
            return {};
    }
    double opacity = 1;
    if (!alpha.isEmpty()) {
        const bool percent = alpha.endsWith(QLatin1Char('%'));
        const auto value = number(alpha, percent ? 100 : 1, percent);
        if (!value)
            return {};
        opacity = *value / (percent ? 100 : 1);
    }
    QColor color;
    if (function.startsWith(QStringLiteral("rgb"))) {
        std::array<int, 3> channels{};
        for (std::size_t i = 0; i < channels.size(); ++i) {
            const auto& component = components[static_cast<qsizetype>(i)];
            const bool percent = component.endsWith(QLatin1Char('%'));
            if (body.contains(QLatin1Char(',')) &&
                percent != components.first().endsWith(QLatin1Char('%')))
                return {};
            const auto value = number(component, percent ? 100 : 255, percent);
            if (!value)
                return {};
            channels[i] = qRound(percent ? *value * 255 / 100 : *value);
        }
        color = QColor(channels[0], channels[1], channels[2]);
    } else {
        auto hueText = components.first().toLower();
        if (hueText.endsWith(QStringLiteral("deg")))
            hueText.chop(3);
        const auto hue = number(hueText, 1e9, false, -1e9);
        const auto saturation = number(components[1], 100, true);
        const auto lightness = number(components[2], 100, true);
        if (!hue || !saturation || !lightness)
            return {};
        const double degrees = std::fmod(*hue, 360.0);
        color = QColor::fromHslF(static_cast<float>((degrees < 0 ? degrees + 360 : degrees) / 360),
                                 static_cast<float>(*saturation / 100),
                                 static_cast<float>(*lightness / 100))
                    .toRgb();
    }
    return QColor(color.red(), color.green(), color.blue(), qRound(opacity * 255));
}
} // namespace

std::optional<PinnedColorSwatchValues> parsePinnedColorSwatch(const QString& text) {
    const auto color = parse(text);
    if (!color)
        return {};
    PinnedColorSwatchValues result;
    result.color = *color;
    const bool opaque = color->alpha() == 255;
    QString alpha = QString::number(color->alpha() / 255.0, 'f', 3);
    while (alpha.endsWith(QLatin1Char('0')))
        alpha.chop(1);
    if (alpha.endsWith(QLatin1Char('.')))
        alpha.chop(1);
    result.display[0] = color->name(QColor::HexRgb).toUpper();
    if (!opaque)
        result.display[0] +=
            QStringLiteral("%1").arg(color->alpha(), 2, 16, QLatin1Char('0')).toUpper();
    result.display[1] =
        QStringLiteral("%1, %2, %3").arg(color->red()).arg(color->green()).arg(color->blue());
    const auto hsl = color->toHsl();
    const QString hue = QString::number(std::max(0.0, double(hsl.hslHueF()) * 360), 'f', 1);
    const QString saturation = QString::number(double(hsl.hslSaturationF()) * 100, 'f', 1);
    const QString lightness = QString::number(double(hsl.lightnessF()) * 100, 'f', 1);
    result.display[2] = QStringLiteral("%1, %2, %3").arg(hue, saturation, lightness);
    result.clipboard[0] = result.display[0];
    result.clipboard[1] = (opaque ? QStringLiteral("rgb(%1)") : QStringLiteral("rgba(%1, %2)"))
                              .arg(result.display[1]);
    if (!opaque)
        result.clipboard[1] = result.clipboard[1].arg(alpha);
    result.clipboard[2] =
        opaque ? QStringLiteral("hsl(%1, %2%, %3%)").arg(hue, saturation, lightness)
               : QStringLiteral("hsla(%1, %2%, %3%, %4)").arg(hue, saturation, lightness, alpha);
    if (!opaque) {
        result.display[1] += QStringLiteral(", %1").arg(alpha);
        result.display[2] += QStringLiteral(", %1").arg(alpha);
    }
    return result;
}

PinnedColorSwatchRendering renderPinnedColorSwatch(const PinnedColorSwatchValues& values,
                                                   const PinnedColorSwatchStyle& style, qreal dpr,
                                                   const QSize& logicalSize) {
    PinnedColorSwatchRendering result;
    if (!std::isfinite(dpr) || dpr <= 0 || dpr > 16 ||
        (logicalSize != QSize(342, 210) && logicalSize != QSize(470, 288)))
        return result;
    const qreal scale = logicalSize == QSize(470, 288) ? 1.0 : 16.0 / 22.0;
    result.document =
        std::shared_ptr<QTextDocument>(new QTextDocument, [](QTextDocument* document) {
            if (document->thread() == QThread::currentThread())
                delete document;
            else
                document->deleteLater();
        });
    auto& document = *result.document;
    QFont font = style.font;
    font.setPixelSize(16);
    document.setDefaultFont(font);
    document.setDocumentMargin(28 * scale);
    document.setTextWidth(logicalSize.width());
    QTextCursor cursor(&document);
    const bool opaque = values.color.alpha() == 255;
    for (std::size_t row = 0; row < 4; ++row) {
        if (row)
            cursor.insertBlock();
        QTextBlockFormat block;
        block.setLineHeight(30 * scale, QTextBlockFormat::FixedHeight);
        block.setBottomMargin(row < 3 ? 36 * scale : 0);
        QTextOption::Tab tab;
        tab.position = 78 * scale;
        block.setTabPositions({tab});
        cursor.setBlockFormat(block);
        QTextCharFormat label;
        label.setForeground(style.secondary);
        const QString labelText = row == 0              ? style.colorLabel
                                  : row == 2 && !opaque ? style.rgbaLabel
                                  : row == 3 && !opaque ? style.hslaLabel
                                                        : style.labels[row - 1];
        cursor.insertText(labelText, label);
        if (row) {
            QTextCharFormat value;
            value.setForeground(style.foreground);
            cursor.insertText(QStringLiteral("\t") + values.display[row - 1], value);
        }
    }
    static_cast<void>(document.documentLayout()->documentSize());
    result.image = snowCanvasAllocateImage(
        QSize(qCeil(logicalSize.width() * dpr), qCeil(logicalSize.height() * dpr)),
        QImage::Format_ARGB32_Premultiplied);
    if (result.image.isNull())
        return {};
    result.image.setDevicePixelRatio(dpr);
    result.image.fill(style.background);
    QPainter painter(&result.image);
    painter.setRenderHint(QPainter::Antialiasing);
    document.drawContents(&painter);
    const QRectF swatch(106 * scale, 31 * scale, 28 * scale, 28 * scale);
    painter.save();
    QPainterPath clip;
    clip.addRoundedRect(swatch, 3 * scale, 3 * scale);
    painter.setClipPath(clip);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            painter.fillRect(QRectF(swatch.x() + x * 7 * scale, swatch.y() + y * 7 * scale,
                                    7 * scale, 7 * scale),
                             (x + y) % 2 ? QColor(190, 190, 190) : QColor(240, 240, 240));
    painter.fillRect(swatch, values.color);
    painter.restore();
    QColor swatchBorder = style.secondary;
    swatchBorder.setAlpha(100);
    painter.setPen(QPen(swatchBorder, scale));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(swatch.adjusted(0.5 * scale, 0.5 * scale, -0.5 * scale, -0.5 * scale),
                            3 * scale, 3 * scale);
    auto block = document.begin().next();
    for (std::size_t row = 0; row < result.copyRects.size(); ++row, block = block.next()) {
        const auto bounds = document.documentLayout()->blockBoundingRect(block);
        const auto line = block.layout()->lineAt(0);
        const qreal x = bounds.x() + line.cursorToX(block.length() - 1) + 6 * scale;
        const QRectF button(x, bounds.y() + scale, 28 * scale, 28 * scale);
        result.copyRects[row] = button;
        painter.setPen(QPen(style.link, 1.6 * scale));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(button.adjusted(10 * scale, 4 * scale, -5 * scale, -8 * scale));
        painter.fillRect(button.adjusted(6 * scale, 8 * scale, -9 * scale, -4 * scale),
                         style.background);
        painter.drawRect(button.adjusted(6 * scale, 8 * scale, -9 * scale, -4 * scale));
    }
    painter.end();
    if (QCoreApplication::instance() && document.thread() != QCoreApplication::instance()->thread())
        document.moveToThread(QCoreApplication::instance()->thread());
    return result;
}
