#include "snow_shot/presentation/screenshotrecognitionimage.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "../../test-support/virtualmemory.h"
#include <QGuiApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QThread>
#include <future>
#include <iostream>
#include <stdexcept>
#include <limits>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
ScreenshotRecognitionImageSnapshot fixture() {
    ScreenshotRecognitionImageSnapshot snapshot;
    snapshot.image = QImage(320, 180, QImage::Format_ARGB32_Premultiplied);
    snapshot.image.fill(QColor(230, 240, 250));
    snapshot.canvasRect = QRectF(640, 360, 320, 180);
    snapshot.font = QGuiApplication::font();
    snapshot.textColor = Qt::black;
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Translated result");
    line.quad = {{660, 380}, {920, 380}, {920, 420}, {660, 420}};
    snapshot.lines.push_back(line);
    return snapshot;
}
void rendersNativeCoordinatesAndPreservesSource() {
    auto snapshot = fixture();
    snapshot.image.setDevicePixelRatio(2.0);
    const QImage source = snapshot.image;
    const QImage rendered = renderScreenshotRecognitionImage(snapshot);
    require(rendered.size() == source.size() && rendered.devicePixelRatio() == 1.0,
            "export retains physical image resolution regardless of display DPR");
    require(snapshot.image == source, "worker render must not mutate source image");
    require(rendered.pixelColor(0, 0) == source.pixelColor(0, 0),
            "non-text image pixels are preserved");
    int textPixels = 0;
    for (int y = 0; y < rendered.height(); ++y) {
        for (int x = 0; x < rendered.width(); ++x) {
            if (rendered.pixelColor(x, y) != source.pixelColor(x, y)) {
                require(QRect(20, 20, 260, 40).contains(x, y), "text stays in its output quad");
                ++textPixels;
            }
        }
    }
    require(textPixels > 100, "recognized text is rasterized");
    snapshot.lines.clear();
    require(renderScreenshotRecognitionImage(snapshot).pixelColor(70, 30) ==
                source.pixelColor(70, 30),
            "pending and empty recognition preserve the currently displayed source");
}
void displayedBackgroundAndEffectsAreAppliedOnce() {
    auto snapshot = fixture();
    snapshot.lines.clear();
    snapshot.filteredImage = QImage(40, 20, QImage::Format_ARGB32_Premultiplied);
    snapshot.filteredImage.fill(Qt::green);
    snapshot.filteredCanvasRect = QRectF(680, 380, 40, 20);
    QImage image = renderScreenshotRecognitionImage(snapshot);
    require(image.pixelColor(45, 25) == QColor(Qt::green) &&
                image.pixelColor(39, 25) == snapshot.image.pixelColor(39, 25),
            "displayed filtered patch uses canvas coordinates without covering other image pixels");
    snapshot.resultStyle = {16, 8, Qt::black};
    image = renderScreenshotRecognitionImage(snapshot);
    require(image.size() == QSize(336, 196) && image.pixelColor(53, 33) == QColor(Qt::green),
            "shadow padding is applied exactly once after recognition composition");
    require(image.pixelColor(8, 8).alpha() < 255, "rounded result corners preserve transparency");
}
void compoundSelectionClipsRecognizedText() {
    auto snapshot = fixture();
    snapshot.resultStyle.region = QRegion(0, 0, 320, 180).subtracted(QRegion(80, 0, 80, 100));
    const auto frozen = snapshot;
    snapshot.resultStyle.region = QRegion(0, 0, 320, 180);
    const QImage rendered = renderScreenshotRecognitionImage(frozen);
    require(rendered.pixelColor(100, 30).alpha() == 0 &&
                rendered.pixelColor(20, 100).alpha() == 255,
            "recognition exports clip source and recognized text with frozen geometry");
    auto scaled = frozen;
    scaled.image = scaled.image.scaled(640, 360);
    const QImage large = renderScreenshotRecognitionImage(scaled);
    require(large.pixelColor(200, 60).alpha() == 0 && large.pixelColor(40, 200).alpha() == 255,
            "recognition shape follows image backing scale");
}

void compactionFollowsRecognitionRendering() {
    for (int shadow : {0, 3}) {
        for (qreal scale : {1.0, 1.5, 2.0}) {
            auto snapshot = fixture();
            snapshot.image = snapshot.image.scaled(qRound(320 * scale), qRound(180 * scale));
            snapshot.resultStyle = {6, shadow, Qt::black,
                                    ScreenshotRegionGeometry(
                                        QRegion(0, 0, 100, 180).united(QRegion(220, 0, 100, 180)))};
            const auto reference = renderScreenshotRecognitionImage(snapshot);
            snapshot.layoutCompaction = {true, 4};
            const auto originalQuad = snapshot.lines.first().quad;
            const auto actual = renderScreenshotRecognitionImage(snapshot);
            const int padding = qRound(shadow * scale);
            const QSize footprint(qRound(100 * scale) + 2 * padding,
                                  qRound(180 * scale) + 2 * padding);
            const int spacing = qRound(4 * scale);
            QImage expected(2 * footprint.width() + spacing, footprint.height(),
                            QImage::Format_ARGB32_Premultiplied);
            expected.fill(Qt::transparent);
            QPainter painter(&expected);
            painter.drawImage(QPoint(), reference, QRect(QPoint(), footprint));
            painter.drawImage(QPoint(footprint.width() + spacing, 0), reference,
                              QRect(QPoint(qRound(220 * scale), 0), footprint));
            painter.end();
            require(actual == expected && snapshot.lines.first().quad == originalQuad,
                    "recognition compaction must preserve text coordinates and every shadow pixel");
        }
    }
}

void retainedBackgroundUsesOriginalCapture() {
    for (bool compact : {false, true}) {
        auto snapshot = fixture();
        snapshot.resultStyle = {
            12, 6, Qt::black,
            ScreenshotRegionGeometry(QRegion(0, 0, 100, 180) + QRect(220, 0, 100, 180))};
        snapshot.resultStyle.backgroundOpacityPercent = 50;
        snapshot.layoutCompaction = {compact, 8};
        QImage capture(360, 220, QImage::Format_ARGB32_Premultiplied);
        capture.fill(QColor(80, 140, 220));
        snapshot.backgroundSources = {{capture, snapshot.canvasRect.adjusted(-20, -20, 20, 20)}};
        snapshot.filteredImage = snapshot.image;
        snapshot.filteredImage.fill(Qt::green);
        snapshot.filteredCanvasRect = snapshot.canvasRect;
        const auto result = renderScreenshotRecognitionImage(snapshot);
        require(!result.isNull(), "recognition result with retained background renders");
        const int gapX = compact ? 116 : 166;
        const auto gap = result.pixelColor(gapX, 150);
        require(
            gap.blue() > gap.green() && gap.alpha() > 0 && gap.alpha() < 200,
            "recognition export gaps use capture pixels, not recognized text or filtered patches");
        require(result.pixelColor(50, 150) == QColor(Qt::green),
                "recognition result inside selected regions keeps full opacity");
        require(result.pixelColor(0, 0).alpha() > 0,
                "recognition exports retain captured pixels beyond the selection bounds");
    }
}

void layoutModesAndTransformsRenderOnWorkers() {
    for (bool vertical : {false, true}) {
        for (bool paragraph : {false, true}) {
            auto snapshot = fixture();
            snapshot.lines[0].direction = vertical ? ScreenshotOcrTextDirection::Vertical
                                                   : ScreenshotOcrTextDirection::Horizontal;
            snapshot.lines[0].paragraph = paragraph;
            snapshot.lines[0].text =
                QString::fromUtf8("繁體文字 Á العربية 👩‍💻");
            snapshot.lines[0].quad = {{675, 375}, {820, 390}, {805, 525}, {660, 510}};
            const QImage expected = renderScreenshotRecognitionImage(snapshot);
            auto result = std::async(std::launch::async, [snapshot]() {
                return renderScreenshotRecognitionImage(snapshot);
            });
            require(
                !expected.isNull() && result.get() == expected,
                "Unicode, vertical and paragraph layout are deterministic on independent workers");
            if (expected == snapshot.image)
                std::cerr << "empty layout vertical=" << vertical << " paragraph=" << paragraph
                          << '\n';
            require(expected != snapshot.image, "transformed Unicode text produces visible pixels");
        }
    }
}
void sourceRowsSurviveImageExportOnWorkers() {
    auto snapshot = fixture();
    snapshot.image = QImage(360, 160, QImage::Format_ARGB32_Premultiplied);
    snapshot.image.fill(Qt::white);
    snapshot.canvasRect = QRectF(-180, -80, 360, 160);
    auto& line = snapshot.lines[0];
    line.text = QString(31, QChar(0x7530));
    line.paragraph = true;
    line.quad = QPolygonF(QRectF(-150, -50, 300, 96));
    line.quad.removeLast();
    for (const QRectF& row :
         {QRectF(-150, -50, 300, 24), QRectF(-150, -14, 300, 24), QRectF(-150, 22, 180, 24)}) {
        QPolygonF quad(row);
        quad.removeLast();
        line.sourceLineQuads.push_back(quad);
    }
    const QImage expected = renderScreenshotRecognitionImage(snapshot);
    auto worker = std::async(std::launch::async,
                             [snapshot]() { return renderScreenshotRecognitionImage(snapshot); });
    require(worker.get() == expected, "source row export is deterministic on a worker thread");
    const auto inkBounds = [&](const QRect& region) {
        QRect bounds;
        for (int y = region.top(); y <= region.bottom(); ++y) {
            for (int x = region.left(); x <= region.right(); ++x) {
                if (expected.pixelColor(x, y).red() < 80)
                    bounds = bounds.united(QRect(x, y, 1, 1));
            }
        }
        return bounds;
    };
    for (const QRect& row :
         {QRect(30, 30, 300, 24), QRect(30, 66, 300, 24), QRect(30, 102, 180, 24)}) {
        const QRect ink = inkBounds(row);
        require(ink.width() >= row.width() * 0.9 && ink.height() >= row.height() * 0.65,
                "image export preserves the full source rows and shorter final row");
    }
    require(inkBounds(QRect(30, 56, 300, 8)).isEmpty() &&
                inkBounds(QRect(30, 92, 300, 8)).isEmpty() &&
                inkBounds(QRect(212, 102, 118, 24)).isEmpty(),
            "image export preserves source line gaps and the shorter final row boundary");
    line.sourceLineQuads.clear();
    const QImage fallback = renderScreenshotRecognitionImage(snapshot);
    line.sourceLineQuads = {line.quad, line.quad};
    require(renderScreenshotRecognitionImage(snapshot) == fallback,
            "image export falls back to paragraph layout for overlapping source rows");
}
void largeWorkerOutputRetainsManagedOwnership() {
    auto snapshot = fixture();
    snapshot.image = snowCanvasAllocateImage(QSize(1025, 513), QImage::Format_ARGB32_Premultiplied);
    require(!snapshot.image.isNull(), "large recognition fixture must allocate");
    snapshot.image.fill(Qt::white);
    snapshot.image.setDevicePixelRatio(2.0);
    const auto* original = snapshot.image.constBits();
    auto worker = std::async(std::launch::async,
                             [snapshot] { return renderScreenshotRecognitionImage(snapshot); });
    QImage rendered = worker.get();
    require(!rendered.isNull() && rendered.constBits() != original &&
                snapshot.image.constBits() == original && snapshot.image.devicePixelRatio() == 2.0,
            "recognition must detach into owned output without altering its capture lease");
    const auto* middle = rendered.constBits() + rendered.sizeInBytes() / 2;
    require(snow::test_support::virtualMemoryMapped(middle),
            "recognition output must own live mapped storage");
    rendered = {};
    require(!snow::test_support::virtualMemoryMapped(middle),
            "completed worker output must release its final pixel pages");
}

void cancellationAndInvalidInputNeverPublishPartialImages() {
    auto snapshot = fixture();
    int polls = 0;
    require(renderScreenshotRecognitionImage(snapshot, [&]() { return ++polls >= 2; }).isNull(),
            "mid-render cancellation discards partial output");
    snapshot.canvasRect = {};
    require(renderScreenshotRecognitionImage(snapshot).isNull(),
            "invalid geometry fails rendering");
    snapshot = fixture();
    snapshot.lines[0].quad[0].setX(std::numeric_limits<qreal>::quiet_NaN());
    require(renderScreenshotRecognitionImage(snapshot) == snapshot.image,
            "malformed text geometry cannot corrupt the exported image");
}
} // namespace
int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
#if defined(Q_OS_WIN)
    if (QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) < 0 ||
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc")) < 0)
        return 3;
    QGuiApplication::setFont(QFont(QStringLiteral("Segoe UI")));
#endif

    try {
        rendersNativeCoordinatesAndPreservesSource();
        displayedBackgroundAndEffectsAreAppliedOnce();
        compoundSelectionClipsRecognizedText();
        compactionFollowsRecognitionRendering();
        retainedBackgroundUsesOriginalCapture();
        layoutModesAndTransformsRenderOnWorkers();
        sourceRowsSurviveImageExportOnWorkers();
        cancellationAndInvalidInputNeverPublishPartialImages();
        largeWorkerOutputRetainsManagedOwnership();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
