// SPDX-License-Identifier: GPL-3.0-only
#include "imagesplitimagepreparation_p.h"

#include <QColorSpace>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <thread>
#include <vector>

namespace snow_shot::presentation {

QImage imageSplitCopyTile(const QImage& image, const QRect& region) {
    if (region.isEmpty() || !image.rect().contains(region))
        return {};
    // An explicit nonempty rectangle copies only active pixels/bits from the original image.
    // Qt supplies owned row padding and preserves native format, palette and metadata.
    return image.copy(region);
}

namespace {
using ChannelSums = std::array<std::uint64_t, 4>;
using WeightedChannels = std::array<double, 4>;

bool qtAreaDimensionsSupported(QSize source, QSize output) {
    // Qt's identity-axis sample origins use a signed 0x8000 * dimension expression.
    // Its downsampling coverage also has only fourteen fractional coefficient bits.
    return source.width() <= 65535 && source.height() <= 65535 &&
           qint64(source.width()) <= qint64(output.width()) * 16384 &&
           qint64(source.height()) <= qint64(output.height()) * 16384;
}

bool directlyScalable(QImage::Format format) {
    return format == QImage::Format_RGB32 || format == QImage::Format_ARGB32_Premultiplied ||
           format == QImage::Format_RGBX8888 || format == QImage::Format_RGBA8888_Premultiplied;
}

struct PreviewTiles final {
    QSize source;
    QSize output;
};

std::optional<PreviewTiles> previewTiles(const QImage& image, QSize output) {
    // A lattice boundary must also be exact in Qt's sixteen-bit source origins. Otherwise
    // resetting that origin in each tile changes coverage and can introduce visible seams.
    if ((qint64(image.width()) << 16) % output.width() != 0 ||
        (qint64(image.height()) << 16) % output.height() != 0 ||
        qint64(image.width()) > qint64(output.width()) * 16384 ||
        qint64(image.height()) > qint64(output.height()) * 16384)
        return std::nullopt;
    const int horizontal = std::gcd(image.width(), output.width());
    const int vertical = std::gcd(image.height(), output.height());
    const QSize sourceUnit(image.width() / horizontal, image.height() / vertical);
    const QSize outputUnit(output.width() / horizontal, output.height() / vertical);
    // Budget the owned native crop, premultiplied input and scaled output, plus bounded Qt
    // conversion/coefficient storage. High-precision sources need smaller native tiles.
    const int nativeBytesPerPixel = std::max(1, image.depth() / 8);
    const qint64 maximumPixels =
        (imageSplitPreviewMaximumWorkingBytes - 256 * 1024) / (nativeBytesPerPixel + 8);
    const qint64 unitPixels = qint64(sourceUnit.width()) * sourceUnit.height();
    if (unitPixels > maximumPixels || sourceUnit.width() > 65535 || sourceUnit.height() > 65535)
        return std::nullopt;
    int columns = static_cast<int>(std::min(
        {qint64(horizontal), qint64(65535 / sourceUnit.width()), maximumPixels / unitPixels}));
    if (image.depth() == 1 && columns != horizontal) {
        const int alignment = 8 / std::gcd(sourceUnit.width(), 8);
        columns = columns / alignment * alignment;
        if (columns == 0)
            return std::nullopt;
    }
    const int rows =
        static_cast<int>(std::min({qint64(vertical), qint64(65535 / sourceUnit.height()),
                                   maximumPixels / (unitPixels * columns)}));
    return PreviewTiles{QSize(sourceUnit.width() * columns, sourceUnit.height() * rows),
                        QSize(outputUnit.width() * columns, outputUnit.height() * rows)};
}

QImage tiledQtPreview(const QImage& image, QSize outputSize, const PreviewTiles& tiles,
                      const std::function<bool()>& cancelled) {
    QImage output(outputSize, QImage::Format_ARGB32_Premultiplied);
    if (output.isNull())
        return {};
    for (int sourceY = 0, outputY = 0; sourceY < image.height();) {
        const int sourceHeight = std::min(tiles.source.height(), image.height() - sourceY);
        const int outputHeight = std::min(tiles.output.height(), output.height() - outputY);
        for (int sourceX = 0, outputX = 0; sourceX < image.width();) {
            if (cancelled && cancelled())
                return {};
            const int sourceWidth = std::min(tiles.source.width(), image.width() - sourceX);
            const int outputWidth = std::min(tiles.output.width(), output.width() - outputX);
            // An owned crop supplies valid row padding to every Qt conversion/scaling path.
            // A pointer into the original image with its full stride cannot provide that contract.
            const QImage working =
                imageSplitCopyTile(image, QRect(sourceX, sourceY, sourceWidth, sourceHeight))
                    .convertToFormat(QImage::Format_ARGB32_Premultiplied);
            if (working.isNull() || (cancelled && cancelled()))
                return {};
            const QImage scaled = working.scaled(outputWidth, outputHeight, Qt::IgnoreAspectRatio,
                                                 Qt::SmoothTransformation);
            if (scaled.isNull() || (cancelled && cancelled()))
                return {};
            for (int row = 0; row < outputHeight; ++row)
                std::memcpy(output.scanLine(outputY + row) + qsizetype(outputX) * 4,
                            scaled.constScanLine(row), size_t(outputWidth) * 4);
            sourceX += sourceWidth;
            outputX += outputWidth;
        }
        sourceY += sourceHeight;
        outputY += outputHeight;
    }
    output.setColorSpace(image.colorSpace());
    return output;
}

struct AreaSpan final {
    int first;
    int last;
    int firstWeight;
    int lastWeight;
};

AreaSpan areaSpan(int sourceSize, int outputSize, int outputIndex) {
    const qint64 start = qint64(outputIndex) * sourceSize;
    const qint64 end = qint64(outputIndex + 1) * sourceSize;
    const int first = static_cast<int>(start / outputSize);
    const int last = static_cast<int>((end - 1) / outputSize);
    return {first, last, static_cast<int>(std::min(end, qint64(first + 1) * outputSize) - start),
            static_cast<int>(end - qint64(last) * outputSize)};
}

void addPixel(ChannelSums& sums, QRgb pixel, int weight) {
    sums[0] += std::uint64_t(qRed(pixel)) * std::uint64_t(weight);
    sums[1] += std::uint64_t(qGreen(pixel)) * std::uint64_t(weight);
    sums[2] += std::uint64_t(qBlue(pixel)) * std::uint64_t(weight);
    sums[3] += std::uint64_t(qAlpha(pixel)) * std::uint64_t(weight);
}

class PremultipliedPixels final {
  public:
    PremultipliedPixels(const QImage& image, const std::function<bool()>& cancelled)
        : m_image(image), m_cancelled(cancelled),
          m_direct(image.format() == QImage::Format_ARGB32_Premultiplied ||
                   image.format() == QImage::Format_RGB32),
          m_opaque(!image.hasAlphaChannel()) {}

    bool horizontalRow(int sourceRow, const std::vector<AreaSpan>& spans, int outputWidth,
                       std::vector<WeightedChannels>& filtered) {
        int available = 0;
        const QRgb* data = row(0, sourceRow, available);
        if (!data)
            return false;
        if (available == m_image.width() &&
            m_image.width() <= std::numeric_limits<int>::max() / 255)
            return m_opaque ? filterRow<true>(data, spans, outputWidth, filtered)
                            : filterRow<false>(data, spans, outputWidth, filtered);
        for (int x = 0; x < outputWidth; ++x) {
            if (x % 64 == 0 && m_cancelled && m_cancelled())
                return false;
            ChannelSums sums;
            if (!horizontal(sourceRow, spans[static_cast<size_t>(x)], outputWidth, sums))
                return false;
            auto& target = filtered[static_cast<size_t>(x)];
            for (size_t channel = 0; channel < target.size(); ++channel)
                target[channel] = double(sums[channel]);
        }
        return true;
    }

  private:
    template <bool Opaque>
    bool filterRow(const QRgb* data, const std::vector<AreaSpan>& spans, int outputWidth,
                   std::vector<WeightedChannels>& filtered) const {
        for (int x = 0; x < outputWidth; ++x) {
            if (m_image.width() > 65536 && x % 64 == 0 && m_cancelled && m_cancelled())
                return false;
            const auto& span = spans[static_cast<size_t>(x)];
            const QRgb first = data[span.first];
            int red = qRed(first) * span.firstWeight;
            int green = qGreen(first) * span.firstWeight;
            int blue = qBlue(first) * span.firstWeight;
            int alpha = Opaque ? 0 : qAlpha(first) * span.firstWeight;
            if (span.first != span.last) {
                int r = 0, g = 0, b = 0, a = 0;
                for (int sourceX = span.first + 1; sourceX < span.last; ++sourceX) {
                    const QRgb pixel = data[sourceX];
                    r += qRed(pixel);
                    g += qGreen(pixel);
                    b += qBlue(pixel);
                    if constexpr (!Opaque)
                        a += qAlpha(pixel);
                }
                const QRgb last = data[span.last];
                red += r * outputWidth + qRed(last) * span.lastWeight;
                green += g * outputWidth + qGreen(last) * span.lastWeight;
                blue += b * outputWidth + qBlue(last) * span.lastWeight;
                if constexpr (!Opaque)
                    alpha += a * outputWidth + qAlpha(last) * span.lastWeight;
            }
            filtered[static_cast<size_t>(x)] = {double(red), double(green), double(blue),
                                                double(Opaque ? m_image.width() * 255 : alpha)};
        }
        return true;
    }

    bool horizontal(int sourceRow, const AreaSpan& span, int outputWidth, ChannelSums& sums) {
        sums = {};
        const auto pixel = [&](int x, int weight) {
            int available = 0;
            const QRgb* data = row(x, sourceRow, available);
            if (!data)
                return false;
            addPixel(sums, m_opaque ? *data | 0xff000000U : *data, weight);
            return true;
        };
        if (!pixel(span.first, span.firstWeight))
            return false;
        if (span.first == span.last)
            return true;
        ChannelSums interior{};
        for (int first = span.first + 1; first < span.last;) {
            int available = 0;
            const QRgb* data = row(first, sourceRow, available);
            if (!data)
                return false;
            const int count = std::min(available, span.last - first);
            for (int x = 0; x < count; ++x) {
                interior[0] += std::uint64_t(qRed(data[x]));
                interior[1] += std::uint64_t(qGreen(data[x]));
                interior[2] += std::uint64_t(qBlue(data[x]));
                interior[3] += std::uint64_t(m_opaque ? 255 : qAlpha(data[x]));
            }
            first += count;
        }
        for (size_t channel = 0; channel < sums.size(); ++channel)
            sums[channel] += interior[channel] * std::uint64_t(outputWidth);
        return pixel(span.last, span.lastWeight);
    }

    const QRgb* row(int x, int y, int& available) {
        if (m_direct) {
            available = m_image.width() - x;
            return reinterpret_cast<const QRgb*>(m_image.constScanLine(y)) + x;
        }
        if (x < m_firstX || x >= m_firstX + m_cache.width() || y < m_firstY ||
            y >= m_firstY + m_cache.height()) {
            if (m_cancelled && m_cancelled())
                return nullptr;
            // Width-independent scratch, including extreme one-row and one-column rasters.
            constexpr int maximumColumns = 16384;
            m_firstX = x / maximumColumns * maximumColumns;
            const int width = std::min(maximumColumns, m_image.width() - m_firstX);
            const int nativeBytesPerPixel = std::max(1, m_image.depth() / 8);
            const int cacheRows =
                m_image.width() <= maximumColumns
                    ? std::max(1, 256 * 1024 / (width * (nativeBytesPerPixel + 4)))
                    : 1;
            m_firstY = y / cacheRows * cacheRows;
            const int height = std::min(cacheRows, m_image.height() - m_firstY);
            m_cache = imageSplitCopyTile(m_image, QRect(m_firstX, m_firstY, width, height))
                          .convertToFormat(QImage::Format_ARGB32_Premultiplied);
            if (m_cache.isNull())
                return nullptr;
        }
        available = m_cache.width() - (x - m_firstX);
        return reinterpret_cast<const QRgb*>(m_cache.constScanLine(y - m_firstY)) + x - m_firstX;
    }

    const QImage& m_image;
    const std::function<bool()>& m_cancelled;
    bool m_direct;
    bool m_opaque;
    QImage m_cache;
    int m_firstX = -1;
    int m_firstY = -1;
};

bool renderAreaRows(const QImage& image, QSize size, const std::vector<AreaSpan>& horizontal,
                    int firstRow, int lastRow, uchar* output, qsizetype stride,
                    const std::function<bool()>& cancelled) {
    std::vector<WeightedChannels> accumulated(static_cast<size_t>(size.width()));
    std::vector<WeightedChannels> filtered(static_cast<size_t>(size.width()));
    PremultipliedPixels pixels(image, cancelled);
    int filteredRow = -1;
    const int cancellationRows = std::max(1, 16384 / image.width());
    const double normalization = double(image.width()) * image.height();
    for (int y = firstRow; y < lastRow; ++y) {
        if (cancelled && cancelled())
            return false;
        std::fill(accumulated.begin(), accumulated.end(), WeightedChannels{});
        const auto vertical = areaSpan(image.height(), size.height(), y);
        for (int sourceY = vertical.first; sourceY <= vertical.last; ++sourceY) {
            if (sourceY % cancellationRows == 0 && cancelled && cancelled())
                return false;
            if (filteredRow != sourceY) {
                if (!pixels.horizontalRow(sourceY, horizontal, size.width(), filtered))
                    return false;
                filteredRow = sourceY;
            }
            const double weight = sourceY == vertical.first  ? vertical.firstWeight
                                  : sourceY == vertical.last ? vertical.lastWeight
                                                             : size.height();
            for (int x = 0; x < size.width(); ++x) {
                auto& value = accumulated[static_cast<size_t>(x)];
                const auto& sample = filtered[static_cast<size_t>(x)];
                for (size_t channel = 0; channel < value.size(); ++channel)
                    value[channel] += sample[channel] * weight;
            }
        }
        auto* destination = reinterpret_cast<QRgb*>(output + qsizetype(y) * stride);
        for (int x = 0; x < size.width(); ++x) {
            const auto& value = accumulated[static_cast<size_t>(x)];
            const auto channel = [&](size_t index) {
                return std::clamp(static_cast<int>(value[index] / normalization + .5), 0, 255);
            };
            destination[x] = qRgba(channel(0), channel(1), channel(2), channel(3));
        }
    }
    return true;
}
} // namespace

QImage imageSplitPreview(const QImage& image, std::function<bool()> cancelled) {
    if (image.isNull() || (cancelled && cancelled()))
        return {};
    if (image.width() <= 2048 && image.height() <= 2048)
        return image;
    const QSize outputSize =
        image.size().scaled(2048, 2048, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    if (directlyScalable(image.format()) && qtAreaDimensionsSupported(image.size(), outputSize)) {
        QImage output = image.scaled(outputSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (cancelled && cancelled())
            return {};
        output.setDevicePixelRatio(1);
        return output;
    }
    if (const auto tiles = previewTiles(image, outputSize))
        return tiledQtPreview(image, outputSize, *tiles, cancelled);
    QImage output(outputSize, QImage::Format_ARGB32_Premultiplied);
    if (output.isNull())
        return {};
    std::vector<AreaSpan> horizontal;
    horizontal.reserve(static_cast<size_t>(output.width()));
    for (int x = 0; x < output.width(); ++x)
        horizontal.push_back(areaSpan(image.width(), output.width(), x));
    // Workers keep private bounded conversion tiles and row accumulators. Only immutable source
    // pixels/filter spans are shared, and each worker writes disjoint final output rows.
    const unsigned workers = qint64(image.width()) * image.height() >= 4 * 1024 * 1024
                                 ? std::min({4U, std::max(1U, std::thread::hardware_concurrency()),
                                             static_cast<unsigned>(output.height())})
                                 : 1;
    std::atomic_bool failed{false};
    std::mutex cancellationMutex;
    const std::function<bool()> check = [&] {
        if (failed.load(std::memory_order_relaxed))
            return true;
        if (!cancelled)
            return false;
        const std::lock_guard lock(cancellationMutex);
        if (failed.load(std::memory_order_relaxed))
            return true;
        if (cancelled())
            failed.store(true, std::memory_order_relaxed);
        return failed.load(std::memory_order_relaxed);
    };
    uchar* destination = output.bits();
    const qsizetype stride = output.bytesPerLine();
    const auto render = [&](unsigned worker) {
        const int first = static_cast<int>(worker * unsigned(output.height()) / workers);
        const int last = static_cast<int>((worker + 1) * unsigned(output.height()) / workers);
        if (!renderAreaRows(image, outputSize, horizontal, first, last, destination, stride, check))
            failed.store(true, std::memory_order_relaxed);
    };
    std::vector<std::future<void>> pending;
    for (unsigned worker = 1; worker < workers; ++worker)
        pending.push_back(std::async(std::launch::async, render, worker));
    render(0);
    for (auto& future : pending)
        future.get();
    if (check())
        return {};
    output.setColorSpace(image.colorSpace());
    output.setDevicePixelRatio(1);
    return output;
}

} // namespace snow_shot::presentation
