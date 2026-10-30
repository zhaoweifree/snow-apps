// SPDX-License-Identifier: GPL-3.0-only
#include "../src/presentation/imagesplit/imagesplitimagepreparation_p.h"
#include "imagesplit_guarded_image.h"

#include <QByteArray>
#include <QColorSpace>
#include <QCoreApplication>

#include <iostream>
#include <stdexcept>

using snow_shot::presentation::imageSplitCopyTile;
using snow_shot::presentation::imageSplitPreview;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void smallImagesShareNativePixels() {
    QImage native(17, 11, QImage::Format_RGBA64_Premultiplied);
    native.fill(QColor(17, 103, 71, 91));
    native.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    native.setDevicePixelRatio(2);
    const QImage preview = imageSplitPreview(native);
    require(preview.constBits() == native.constBits() && preview.format() == native.format() &&
                preview.colorSpace() == native.colorSpace() && preview.devicePixelRatio() == 2,
            "small preview shares immutable native pixels and metadata");
}

void ownedTilesPreserveNativePixelsAndMetadata() {
    for (const auto format :
         {QImage::Format_RGB888, QImage::Format_Indexed8, QImage::Format_Mono,
          QImage::Format_MonoLSB, QImage::Format_RGBA64_Premultiplied, QImage::Format_RGBA32FPx4}) {
        QImage pixels(37, 9, QImage::Format_RGBA8888);
        for (int y = 0; y < pixels.height(); ++y)
            for (int x = 0; x < pixels.width(); ++x)
                pixels.setPixelColor(x, y, QColor(x * 5, y * 27, (x + y) * 5, 97 + x * 3));
        QImage native = pixels.convertToFormat(format);
        native.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
        native.setDevicePixelRatio(2);
        image_split_test::GuardedImage guarded(native);
        for (const int x : {1, 8, 15, 30}) {
            const QRect region(x, 2, 37 - x, 7);
            QImage tile = imageSplitCopyTile(guarded.image(), region);
            require(!tile.isNull() && tile == native.copy(region) &&
                        tile.format() == native.format() &&
                        tile.colorTable() == native.colorTable() &&
                        tile.colorSpace() == native.colorSpace() && tile.devicePixelRatio() == 2,
                    "owned tiles preserve native pixels, bit offsets, palette and metadata");
            // Detaching/cloning the result must be safe even on its final padded scanline.
            require(tile.copy() == tile, "owned tiles expose a complete cloneable allocation");
            tile.fill(0);
            require(guarded.image() == native, "tile writes cannot mutate source storage");
        }
    }
}

void areaFilteringPreservesTransparentEdges() {
    // Each output cell covers exactly four by four pixels. One red pixel per source
    // row is opaque; hidden blue in the three transparent pixels contributes no color.
    QImage native(8192, 4, QImage::Format_RGBA8888);
    for (int y = 0; y < native.height(); ++y) {
        auto* row = native.scanLine(y);
        for (int x = 0; x < native.width(); ++x) {
            row[4 * x] = x % 4 == 0 ? 255 : 0;
            row[4 * x + 1] = 0;
            row[4 * x + 2] = x % 4 == 0 ? 0 : 255;
            row[4 * x + 3] = x % 4 == 0 ? 255 : 0;
        }
    }
    native.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    native.setDevicePixelRatio(2);
    const auto* originalPixels = native.constBits();
    const QByteArray original(reinterpret_cast<const char*>(originalPixels), native.sizeInBytes());
    const QImage preview = imageSplitPreview(native);
    require(preview.size() == QSize(2048, 1) &&
                preview.format() == QImage::Format_ARGB32_Premultiplied &&
                preview.colorSpace() == native.colorSpace() && preview.devicePixelRatio() == 1,
            "large preview bounds preserve source profile while using physical display pixels");
    const auto* pixels = reinterpret_cast<const QRgb*>(preview.constScanLine(0));
    const QImage expected = native.convertToFormat(QImage::Format_ARGB32_Premultiplied)
                                .scaled(2048, 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const auto* reference = reinterpret_cast<const QRgb*>(expected.constScanLine(0));
    for (int x = 0; x < preview.width(); ++x)
        require(pixels[x] == reference[x] && qBlue(pixels[x]) == 0 &&
                    qRed(pixels[x]) == qAlpha(pixels[x]),
                "bounded Qt area reduction preserves exact smooth coverage without hidden blue");
    require(native.constBits() == originalPixels && native.devicePixelRatio() == 2 &&
                QByteArray(reinterpret_cast<const char*>(native.constBits()),
                           native.sizeInBytes()) == original,
            "preview never detaches or modifies the native source raster");
}

void fractionalCoverageIncludesBothEdges() {
    for (const auto format : {QImage::Format_RGB32, QImage::Format_RGBA8888}) {
        // A straight-alpha 7-to-3 vertical ratio has no exact sixteen-bit Qt tile phase,
        // so the second fixture independently exercises the rational area fallback.
        const int height = format == QImage::Format_RGBA8888 ? 7 : 1;
        QImage native(4097, height, format);
        native.fill(Qt::black);
        for (int y = 0; y < height; ++y) {
            reinterpret_cast<QRgb*>(native.scanLine(y))[0] = qRgb(255, 255, 255);
            reinterpret_cast<QRgb*>(native.scanLine(y))[4096] = qRgb(255, 255, 255);
        }
        const QImage preview = imageSplitPreview(native);
        require(preview.size() == QSize(2048, height == 7 ? 3 : 1),
                "fractional-ratio preview has the expected dimensions");
        for (int y = 0; y < preview.height(); ++y) {
            const auto* row = reinterpret_cast<const QRgb*>(preview.constScanLine(y));
            require(row[0] == qRgb(127, 127, 127) && row[2047] == qRgb(127, 127, 127),
                    "fractional first and last coverage retains symmetric edge energy");
            for (int x = 1; x < 2047; ++x)
                require(row[x] == qRgb(0, 0, 0),
                        "edge samples do not bleed beyond their area cells");
        }
    }
}

void nativeFormatsKeepConstantFields() {
    for (const auto format :
         {QImage::Format_ARGB32_Premultiplied, QImage::Format_RGBA8888, QImage::Format_RGB888,
          QImage::Format_RGBA64_Premultiplied, QImage::Format_RGBA32FPx4}) {
        QImage native(4096, 13, format);
        native.fill(QColor(103, 71, 211, 97));
        native.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
        const QImage normalized = native.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const QRgb expected = reinterpret_cast<const QRgb*>(normalized.constScanLine(0))[0];
        const QImage preview = imageSplitPreview(native);
        require(!preview.isNull() && preview.colorSpace() == native.colorSpace(),
                "native-format preview retains its profile");
        for (int y = 0; y < preview.height(); ++y) {
            const auto* row = reinterpret_cast<const QRgb*>(preview.constScanLine(y));
            for (int x = 0; x < preview.width(); ++x)
                require(row[x] == expected,
                        "area filtering preserves every constant premultiplied channel");
        }
    }
}

void tiledQtPreviewMatchesGlobalSamplePhase() {
    for (const auto format :
         {QImage::Format_ARGB32_Premultiplied, QImage::Format_RGB32,
          QImage::Format_RGBA8888_Premultiplied, QImage::Format_RGBX8888, QImage::Format_RGBA8888,
          QImage::Format_RGB888, QImage::Format_Mono, QImage::Format_MonoLSB}) {
        QImage native(8000, 2000, QImage::Format_RGBA8888);
        for (int y = 0; y < native.height(); ++y) {
            auto* row = native.scanLine(y);
            for (int x = 0; x < native.width(); ++x) {
                row[4 * x] = uchar((x * 17 + y * 3) % 256);
                row[4 * x + 1] = uchar((x * 7 + y * 13) % 256);
                row[4 * x + 2] = uchar((x * 3 + y * 29) % 256);
                row[4 * x + 3] = uchar(31 + (x + y * 11) % 225);
            }
        }
        native = native.convertToFormat(format);
        native.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
        const QImage preview = imageSplitPreview(native);
        const QSize size =
            native.size().scaled(2048, 2048, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
        const QImage expected = native.convertToFormat(QImage::Format_ARGB32_Premultiplied)
                                    .scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        require(preview.convertToFormat(QImage::Format_ARGB32_Premultiplied) == expected,
                "Qt preview paths match an independent full-image smooth reference exactly");
    }
}

void extremeRatiosAndCancellation() {
    QImage horizontal(1048576, 1, QImage::Format_RGBA8888);
    horizontal.fill(QColor(71, 103, 17, 97));
    const QImage horizontalPreview = imageSplitPreview(horizontal);
    QImage verticalSource(1, 1048576, QImage::Format_RGBA8888);
    verticalSource.fill(QColor(71, 103, 17, 97));
    const QImage verticalPreview = imageSplitPreview(verticalSource);
    require(horizontalPreview.size() == QSize(2048, 1) && verticalPreview.size() == QSize(1, 2048),
            "extreme aspect ratios retain a nonempty bounded preview");
    const auto expected = qPremultiply(qRgba(71, 103, 17, 97));
    for (int x = 0; x < horizontalPreview.width(); ++x)
        require(reinterpret_cast<const QRgb*>(horizontalPreview.constScanLine(0))[x] == expected,
                "wide tiled conversion preserves constant channels");
    for (int y = 0; y < verticalPreview.height(); ++y)
        require(reinterpret_cast<const QRgb*>(verticalPreview.constScanLine(y))[0] == expected,
                "tall tiled conversion preserves constant channels");
    require(imageSplitPreview(horizontal, [] { return true; }).isNull(),
            "pre-cancelled preview allocates no output");
    int polls = 0;
    require(imageSplitPreview(horizontal, [&] { return ++polls == 10; }).isNull() && polls == 10,
            "conversion and area accumulation stop on an in-flight cancellation");
}

void nativePreviewTilesRespectAllocationBoundary() {
    for (const auto format :
         {QImage::Format_RGB888, QImage::Format_Indexed8, QImage::Format_Mono,
          QImage::Format_MonoLSB, QImage::Format_RGBA64_Premultiplied, QImage::Format_RGBA32FPx4}) {
        // The exact ratio exercises Qt tiles, the coprime ratio exercises rational row caches,
        // and the tall image exercises native tiles that end at the last source scanline.
        for (const QSize size : {QSize(65536, 3), QSize(65537, 3), QSize(3, 65536)}) {
            QImage reference(size, QImage::Format_ARGB32_Premultiplied);
            reference.fill(QColor(103, 71, 211, 97));
            QImage source = reference.convertToFormat(format);
            if (source.depth() == 1)
                source.fill(0);
            source.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
            image_split_test::GuardedImage guarded(source);
            const auto* pixels = guarded.image().constBits();
            const QImage preview = imageSplitPreview(guarded.image());
            const QRgb expected =
                source.convertToFormat(QImage::Format_ARGB32_Premultiplied).pixel(0, 0);
            require(preview.size() ==
                            size.scaled(2048, 2048, Qt::KeepAspectRatio).expandedTo(QSize(1, 1)) &&
                        preview.colorSpace() == source.colorSpace(),
                    "guarded native preview preserves dimensions and color profile");
            for (int y = 0; y < preview.height(); ++y)
                for (int x = 0; x < preview.width(); ++x)
                    require(preview.pixel(x, y) == expected,
                            "guarded native preview preserves constant premultiplied pixels");
            require(guarded.image().constBits() == pixels && guarded.image() == source,
                    "preview preserves the immutable guarded source");
        }
    }
}

void parallelRationalCoverageAndCancellation() {
    QImage native(4097, 1025, QImage::Format_RGBA8888);
    native.fill(Qt::black);
    for (int y = 0; y < native.height(); ++y) {
        reinterpret_cast<QRgb*>(native.scanLine(y))[0] = qRgb(255, 255, 255);
        reinterpret_cast<QRgb*>(native.scanLine(y))[4096] = qRgb(255, 255, 255);
    }
    for (int x = 0; x < native.width(); ++x)
        reinterpret_cast<QRgb*>(native.scanLine(512))[x] = qRgb(255, 255, 255);
    // Coprime source/output axes require the rational fallback. The source has enough pixels
    // to use bounded parallel row bands, whose independent area weights must agree exactly.
    const QImage preview = imageSplitPreview(native);
    require(preview.size() == QSize(2048, 512), "parallel rational preview dimensions are bounded");
    for (int y = 0; y < preview.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(preview.constScanLine(y));
        // The center source stripe contributes half a source row to each output row on
        // either side of the 256 boundary. This also crosses a parallel worker boundary.
        const bool stripe = y == 255 || y == 256;
        const int interior = stripe ? 64 : 0;
        const int edge = stripe ? 159 : 127;
        require(row[0] == qRgb(edge, edge, edge) && row[2047] == qRgb(edge, edge, edge),
                "parallel area bands preserve exact fractional coverage at both edges");
        for (int x = 1; x < 2047; ++x)
            require(row[x] == qRgb(interior, interior, interior),
                    "source stripe coverage is exact on both sides of the worker boundary");
    }
    int polls = 0;
    require(imageSplitPreview(native, [&] { return ++polls == 100; }).isNull() && polls == 100,
            "parallel cancellation serializes the callback and stops every worker");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        smallImagesShareNativePixels();
        ownedTilesPreserveNativePixelsAndMetadata();
        areaFilteringPreservesTransparentEdges();
        fractionalCoverageIncludesBothEdges();
        nativeFormatsKeepConstantFields();
        tiledQtPreviewMatchesGlobalSamplePhase();
        extremeRatiosAndCancellation();
        nativePreviewTilesRespectAllocationBoundary();
        parallelRationalCoverageAndCancellation();
        std::cout << "Image split preview tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
