#pragma once

#include <QSize>
#include <QtMath>
#include <cmath>

namespace snow_shot::presentation {
// Native preparation supplies the actual raster shape before allocating it.
// Keep the document source policy's 64 MP cap and eight bytes per pixel budget,
// including the simultaneously owned scaled output when scaling is requested.
[[nodiscard]] inline qint64 directCapturePeakRasterBytes(QSize size, double scale) {
    if (!std::isfinite(scale) || scale < 0.1 || scale > 4.0)
        return -1;
    const auto rasterBytes = [](QSize size) -> qint64 {
        const qint64 pixels = static_cast<qint64>(size.width()) * size.height();
        return size.width() > 0 && size.height() > 0 && pixels <= 64000000 ? pixels * 8 : -1;
    };
    const auto native = rasterBytes(size);
    if (native < 0)
        return -1;
    const auto output =
        scale == 1.0
            ? 0
            : rasterBytes(QSize(qCeil(size.width() * scale), qCeil(size.height() * scale)));
    return output < 0 ? -1 : native + output;
}
} // namespace snow_shot::presentation
