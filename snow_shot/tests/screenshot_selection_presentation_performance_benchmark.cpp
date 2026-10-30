#include "screenshot_selection_presentation_fixture.h"
#include "snow_shot/presentation/screenshotoverlayinputhandler.h"

#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QThread>

#include <algorithm>
#include <array>
#include <numeric>
#include <vector>

namespace {
using selection_presentation_test::Fixture;
using selection_presentation_test::FrameBackend;

enum class Scenario {
    SteadySelection,
    ChangedSelection,
    Pointer,
    AnimationFrames,
    Retargeting,
    MagnifierVisible,
    MagnifierHidden,
    CombinedAnimation,
    ManualMarquee,
    ManualResize,
    ManualMarqueeMagnifier,
    BooleanAddTargets,
    BooleanSubtractTargets,
    BooleanAddPixelReuse,
    BooleanSubtractPixelReuse,
    BooleanAddAnimation,
    BooleanSubtractAnimation,
};

struct Case {
    const char* name;
    Scenario scenario;
    int requestsPerFrame;
    bool toolbarHidden = false;
};

constexpr std::array kCases{
    Case{"steady_target_1", Scenario::SteadySelection, 1},
    Case{"steady_target_16", Scenario::SteadySelection, 16},
    Case{"changed_targets_1", Scenario::ChangedSelection, 1},
    Case{"changed_targets_16", Scenario::ChangedSelection, 16},
    Case{"pointer_burst_1", Scenario::Pointer, 1},
    Case{"pointer_burst_16", Scenario::Pointer, 16},
    Case{"animation_frames_1", Scenario::AnimationFrames, 1},
    Case{"animation_frames_16", Scenario::AnimationFrames, 16},
    Case{"animation_retargeting_1", Scenario::Retargeting, 1},
    Case{"animation_retargeting_16", Scenario::Retargeting, 16},
    Case{"changed_targets_hidden_1", Scenario::ChangedSelection, 1, true},
    Case{"changed_targets_hidden_16", Scenario::ChangedSelection, 16, true},
    Case{"animation_frames_hidden_1", Scenario::AnimationFrames, 1, true},
    Case{"magnifier_visible_1", Scenario::MagnifierVisible, 1},
    Case{"magnifier_visible_16", Scenario::MagnifierVisible, 16},
    Case{"magnifier_hidden_1", Scenario::MagnifierHidden, 1},
    Case{"magnifier_hidden_16", Scenario::MagnifierHidden, 16},
    Case{"combined_animation_pointer_magnifier_1", Scenario::CombinedAnimation, 1},
    Case{"combined_animation_pointer_magnifier_16", Scenario::CombinedAnimation, 16},
    Case{"manual_marquee_1", Scenario::ManualMarquee, 1},
    Case{"manual_marquee_16", Scenario::ManualMarquee, 16},
    Case{"manual_resize_1", Scenario::ManualResize, 1},
    Case{"manual_resize_16", Scenario::ManualResize, 16},
    Case{"manual_marquee_magnifier_1", Scenario::ManualMarqueeMagnifier, 1},
    Case{"manual_marquee_magnifier_16", Scenario::ManualMarqueeMagnifier, 16},
    Case{"boolean_add_targets_1", Scenario::BooleanAddTargets, 1},
    Case{"boolean_add_targets_16", Scenario::BooleanAddTargets, 16},
    Case{"boolean_subtract_targets_1", Scenario::BooleanSubtractTargets, 1},
    Case{"boolean_subtract_targets_16", Scenario::BooleanSubtractTargets, 16},
    Case{"boolean_add_pixel_reuse_16", Scenario::BooleanAddPixelReuse, 16},
    Case{"boolean_subtract_pixel_reuse_16", Scenario::BooleanSubtractPixelReuse, 16},
    Case{"boolean_add_animation_1", Scenario::BooleanAddAnimation, 1},
    Case{"boolean_subtract_animation_1", Scenario::BooleanSubtractAnimation, 1},
};

bool booleanOperation(Scenario scenario) {
    return scenario == Scenario::BooleanAddTargets ||
           scenario == Scenario::BooleanSubtractTargets ||
           scenario == Scenario::BooleanAddPixelReuse ||
           scenario == Scenario::BooleanSubtractPixelReuse ||
           scenario == Scenario::BooleanAddAnimation ||
           scenario == Scenario::BooleanSubtractAnimation;
}

bool booleanAnimation(Scenario scenario) {
    return scenario == Scenario::BooleanAddAnimation ||
           scenario == Scenario::BooleanSubtractAnimation;
}

ScreenshotRegionGeometry complexConfirmedRegion(const QSize& size) {
    // A deterministic freehand-like contour with concave edges and two holes.
    // Boolean marquee changes cross its edge, preventing the containment fast path.
    const QPointF center(size.width() * 0.48, size.height() * 0.46);
    QPainterPath contour;
    constexpr int vertices = 96;
    constexpr qreal pi = 3.14159265358979323846;
    for (int vertex = 0; vertex < vertices; ++vertex) {
        const qreal angle = vertex * 2.0 * pi / vertices;
        const qreal radius = vertex % 2 == 0 ? 1.0 : 0.82;
        const QPointF point = center + QPointF(std::cos(angle) * size.width() * 0.3 * radius,
                                               std::sin(angle) * size.height() * 0.32 * radius);
        if (vertex == 0)
            contour.moveTo(point);
        else
            contour.lineTo(point);
    }
    contour.closeSubpath();
    contour.setFillRule(Qt::OddEvenFill);
    contour.addEllipse(QRectF(center.x() - size.width() * 0.14, center.y() - size.height() * 0.1,
                              size.width() * 0.09, size.height() * 0.14));
    contour.addEllipse(QRectF(center.x() + size.width() * 0.02, center.y() - size.height() * 0.06,
                              size.width() * 0.08, size.height() * 0.12));
    return ScreenshotRegionGeometry::fromPath(contour, ScreenshotRegionType::Freehand);
}

QJsonObject distribution(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t size = values.size();
    if (size == 0)
        return {};
    const auto percentile = [&](double fraction) {
        const double index = static_cast<double>(size - 1) * fraction;
        const auto lower = static_cast<std::size_t>(index);
        const std::size_t upper = std::min(lower + 1, size - 1);
        return values[lower] +
               (values[upper] - values[lower]) * (index - static_cast<double>(lower));
    };
    return {
        {"median", percentile(0.5)},
        {"p95", percentile(0.95)},
        {"mean", std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(size)},
        {"min", values.front()},
        {"max", values.back()}};
}

#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
QJsonObject runIndependentRegionCase(QSize size, int count, bool curves, bool duplicate,
                                     int iterations, int warmup, FrameBackend backend) {
    Fixture fixture(size, false, true, backend);
    auto& model = fixture.selection;
    const auto bounds = fixture.geometry.canvasBounds().toAlignedRect();
    const auto region = [curves](int index) {
        const QRect rect(20 + (index % 8) * 55, 20 + (index / 8) * 45, 40, 30);
        if (!curves)
            return ScreenshotRegionGeometry(rect);
        QPainterPath path;
        path.addEllipse(QRectF(rect));
        return ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Curve);
    };
    model.setSelectionRegion(region(0));
    for (int index = 1; index < count; ++index) {
        model.beginRegionOperation(ScreenshotSelectionModel::RegionOperation::Add);
        model.setDraftRegion(region(index));
        model.commitDraftRegion(bounds);
    }
    if (count == 1) {
        model.beginRegionOperation(ScreenshotSelectionModel::RegionOperation::Add);
        model.commitRegionOperation();
    }
    fixture.interaction.confirmSelection();
    ScreenshotOverlayInputActions actions;
    actions.updateOverlayState = [&] { fixture.services->updateOverlayState(); };
    actions.requestSelectionDragPresentation = [&] {
        fixture.services->requestSelectionDragPresentation();
    };
    ScreenshotOverlayInputHandler input({fixture.captureState, fixture.interaction, model,
                                         fixture.intelligentSelection, fixture.geometry,
                                         fixture.displays, actions});
    fixture.services->updateOverlayState();
    fixture.processEvents();
    const auto original = model;
    const QPointF press(40, 35);
    QElapsedTimer setup;
    setup.start();
    if (!duplicate)
        input.handleMousePress(nullptr, press);
    const double setupMs = static_cast<double>(setup.nsecsElapsed()) / 1000000.0;
    const auto stationaryCompositions =
        model.regionEditCompositionsForTesting() - original.regionEditCompositionsForTesting();
    quint64 compositions = 0;
    qsizetype retainedBytes = 0;
    qint64 paints = 0;
    qint64 paintedPixels = 0;
    qint64 requestedPixels = 0;
    std::vector<double> submission, frames;
    for (int frame = -warmup; frame < iterations; ++frame) {
        if (duplicate) {
            model = original;
            fixture.interaction.confirmSelection();
            fixture.services->updateOverlayState();
            fixture.processEvents();
        }
        fixture.paintObserver.reset();
        resetSelectionRenderDiagnosticsForCurrentThread();
        const auto before = model.regionEditCompositionsForTesting();
        QElapsedTimer timer;
        timer.start();
        if (duplicate) {
            static_cast<void>(input.duplicateLatestRegionAt(
                QPointF(bounds.width() * 0.75, bounds.height() * 0.75)));
        } else {
            const QPointF pointer =
                press + QPointF(200 + (frame + warmup) % 80, 400 + (frame + warmup) % 40);
            for (int request = 0; request < 16; ++request)
                input.handleMouseMove(nullptr, pointer);
        }
        const double submitted = static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
        fixture.advanceClock(17);
        fixture.flushFrame();
        fixture.processEvents();
        const double elapsed = static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
        const auto performed = model.regionEditCompositionsForTesting() - before;
        if (performed != 1)
            throw std::runtime_error("each changed region frame must compose exactly once");
        if (frame >= 0) {
            submission.push_back(submitted);
            frames.push_back(elapsed);
            compositions += performed;
            paints += fixture.paintObserver.canvasPaints;
            paintedPixels += fixture.paintObserver.canvasDamagePixels;
            requestedPixels += static_cast<qint64>(
                selectionRenderDiagnosticsForCurrentThread().requestedDamagePixels);
            retainedBytes =
                std::max(retainedBytes, model.regionStorageBytesForTesting() +
                                            model.selectionRegion().retainedBytesEstimate());
        }
    }
    if (!duplicate)
        input.handleMouseRelease(nullptr, press + QPointF(220, 420));
    return {{"scenario", QStringLiteral("independent_%1_%2_%3")
                             .arg(duplicate ? QStringLiteral("duplicate") : QStringLiteral("drag"))
                             .arg(curves ? QStringLiteral("curves") : QStringLiteral("rectangles"))
                             .arg(count)},
            {"region_count", count},
            {"iterations", iterations},
            {"requests_per_frame", duplicate ? 1 : 16},
            {"drag_setup_ms", setupMs},
            {"stationary_compositions", qint64(stationaryCompositions)},
            {"compositions", qint64(compositions)},
            {"canvas_paints", paints},
            {"canvas_painted_pixels", paintedPixels},
            {"requested_damage_pixels", requestedPixels},
            {"retained_geometry_bytes_upper_bound", qint64(retainedBytes)},
            {"submission_ms", distribution(submission)},
            {"frame_total_ms", distribution(frames)}};
}
#endif

#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
struct ExternalInitializationSamples {
    std::vector<double> initializationUs;
    std::vector<double> pendingFrameFlushUs;
    std::vector<double> completeUs;
    qint64 synchronousNotifications = 0;
    qint64 completeNotifications = 0;
    qint64 synchronousPickerSamples = 0;
    qint64 completePickerSamples = 0;
    qint64 completePickerPreviews = 0;

    QJsonObject report(const char* implementation, int iterations) const {
        return {{"implementation", QString::fromLatin1(implementation)},
                {"iterations", iterations},
                {"synchronous_init_us", distribution(initializationUs)},
                {"pending_frame_flush_us", distribution(pendingFrameFlushUs)},
                {"init_plus_pending_delivery_us", distribution(completeUs)},
                {"synchronous_semantic_notifications", synchronousNotifications},
                {"complete_semantic_notifications", completeNotifications},
                {"synchronous_picker_samples", synchronousPickerSamples},
                {"complete_picker_samples", completePickerSamples},
                {"complete_picker_preview_rebuilds", completePickerPreviews}};
    }
};

QJsonObject runExternalInitializationCase(const QSize& logicalSize, int iterations, int warmup,
                                          bool pickerEnabled, FrameBackend frameBackend) {
#ifndef NDEBUG
    throw std::runtime_error("external initialization measurements require a Release build");
#endif
    Fixture fixture(logicalSize, false, true, frameBackend);
    if (pickerEnabled)
        fixture.enableColorPicker(ScreenshotColorPickerDisplayMode::AlwaysShow);
    fixture.overlay.hide();
    fixture.services->hideToolbar();
    fixture.coordinator.hideColorPicker();
    fixture.displays.startup->phase = ScreenshotStartupContext::Phase::Preparing;

    ScreenshotOverlayInputActions actions;
    actions.hideMainToolbar = [&fixture] { fixture.services->hideMainToolbar(); };
    actions.updateOverlayState = [&fixture] { fixture.services->updateOverlayState(); };
    actions.requestSelectionDragPresentation = [&fixture] {
        fixture.services->requestSelectionDragPresentation();
    };
    actions.setOverlayCursor = [&fixture](ScreenshotOverlayWindow* owner,
                                          ScreenshotSelectionDragMode mode) {
        fixture.coordinator.setOverlayCursor(owner, mode);
    };
    if (pickerEnabled) {
        actions.updateColorPickerForSelectionDrag = [&fixture](const QPointF& position) {
            fixture.services->requestSelectionDragColorPickerPresentation(position);
        };
    }
    ScreenshotOverlayInputHandler input({fixture.captureState, fixture.interaction,
                                         fixture.selection, fixture.intelligentSelection,
                                         fixture.geometry, fixture.displays, std::move(actions)});
    const auto reset = [&](const QPointF& start) {
        input.setExternalDragActive(false);
        fixture.selection.clearSelection();
        fixture.interaction.enterOverlayVisible(false);
        static_cast<void>(
            fixture.interaction.enterSelectionDrag(ScreenshotSelectionDragMode::Marquee));
        fixture.intelligentSelection.clearTransientState();
        fixture.displays.startup->logicalPosition =
            fixture.geometry.logicalPositionForCanvasPoint(fixture.displays.displayAt(0), start)
                .toPoint();
        fixture.services->resetPresentation();
        fixture.services->updateOverlayState();
        fixture.flushFrame();
        fixture.services->hideToolbar();
        if (pickerEnabled) {
            fixture.colorPickerController->reset();
            fixture.coordinator.colorPicker()->resetForNewCapture();
            fixture.coordinator.prepareColorPickerSurface(fixture.displays);
        }
        fixture.processEvents();
        input.setExternalDragActive(true);
        fixture.resetCounters();
    };

    std::array<ExternalInitializationSamples, 2> samples;
    std::vector<double> pairedReductionPercent;
    pairedReductionPercent.reserve(static_cast<std::size_t>(iterations));
    for (int sample = 0; sample < iterations + warmup; ++sample) {
        const QPointF start(std::floor(logicalSize.width() * 0.1) + sample % 7,
                            std::floor(logicalSize.height() * 0.1) + sample % 5);
        const QPointF end(std::floor(logicalSize.width() * 0.65) + sample % 11,
                          std::floor(logicalSize.height() * 0.6) + sample % 13);
        const QRectF expected(QRect(start.toPoint(), end.toPoint()));
        std::array<double, 2> pairUs{};
        std::array<QRectF, 2> finalGeometry;
        for (int order = 0; order < 2; ++order) {
            const int implementation = (sample + order) % 2;
            const bool batched = implementation == 1;
            reset(start);
            QElapsedTimer timer;
            timer.start();
            if (batched) {
                input.beginExternalSelectionDrag(start, end);
            } else {
                input.beginExternalSelectionDrag(start);
                input.updateExternalSelectionDrag(end);
            }
            const qint64 initializationNs = timer.nsecsElapsed();
            const qint64 synchronousNotifications = fixture.stateNotifications;
            const auto synchronousPicker = pickerEnabled
                                               ? fixture.coordinator.colorPicker()->workCounters()
                                               : ScreenshotColorPickerWindow::WorkCounters{};
            finalGeometry[static_cast<std::size_t>(implementation)] =
                fixture.selection.normalizedSelection();
            if (finalGeometry[static_cast<std::size_t>(implementation)] != expected ||
                fixture.displayedSelection() != expected || fixture.overlay.isVisible() ||
                !input.externalDragActive() || !fixture.interaction.dragging() ||
                fixture.interaction.dragMode() != ScreenshotSelectionDragMode::Marquee ||
                synchronousNotifications != (batched ? 1 : 2)) {
                throw std::runtime_error(
                    "external initialization must commit the same hidden inclusive marquee; "
                    "two-step initialization notifies twice and batched initialization once");
            }

            // The old endpoint picker request can remain queued. Report its delivery separately
            // so the main interval measures precisely the production before-reveal calls.
            timer.restart();
            fixture.flushFrame();
            const qint64 pendingFrameFlushNs = timer.nsecsElapsed();
            const auto completePicker = pickerEnabled
                                            ? fixture.coordinator.colorPicker()->workCounters()
                                            : ScreenshotColorPickerWindow::WorkCounters{};
            const double initializationUs = static_cast<double>(initializationNs) / 1000.0;
            pairUs[static_cast<std::size_t>(implementation)] = initializationUs;
            if (sample >= warmup) {
                auto& measured = samples[static_cast<std::size_t>(implementation)];
                measured.initializationUs.push_back(initializationUs);
                measured.pendingFrameFlushUs.push_back(static_cast<double>(pendingFrameFlushNs) /
                                                       1000.0);
                measured.completeUs.push_back(
                    static_cast<double>(initializationNs + pendingFrameFlushNs) / 1000.0);
                measured.synchronousNotifications += synchronousNotifications;
                measured.completeNotifications += fixture.stateNotifications;
                measured.synchronousPickerSamples += static_cast<qint64>(synchronousPicker.samples);
                measured.completePickerSamples += static_cast<qint64>(completePicker.samples);
                measured.completePickerPreviews += static_cast<qint64>(completePicker.previews);
            }
        }
        if (finalGeometry[0] != finalGeometry[1])
            throw std::runtime_error("external initialization A/B geometry must match");
        if (sample >= warmup)
            pairedReductionPercent.push_back((pairUs[0] - pairUs[1]) / pairUs[0] * 100.0);
    }
    return {{"scenario", pickerEnabled ? "external_initialization_picker"
                                       : "external_initialization_no_picker"},
            {"overlay_count", 1},
            {"overlay_hidden", true},
            {"build_type", "Release"},
            {"capture_in_progress", fixture.captureState.captureInProgress},
            {"guide_lines_enabled", fixture.guidesEnabled},
            {"shortcut_hints_included", true},
            {"picker_included", pickerEnabled},
            {"sample_order", "alternating_two_step_then_batched_and_batched_then_two_step"},
            {"geometry_guard", "matching_model_and_rendered_inclusive_marquee_before_reveal"},
            {"paired_synchronous_time_reduction_percent", distribution(pairedReductionPercent)},
            {"implementations", QJsonArray{samples[0].report("two_step", iterations),
                                           samples[1].report("batched", iterations)}}};
}
#endif

QJsonObject runCase(const Case& benchmarkCase, const QSize& logicalSize, int iterations, int warmup,
                    int requestedFrameIntervalMs, FrameBackend frameBackend) {
    const bool animation = benchmarkCase.scenario == Scenario::AnimationFrames ||
                           benchmarkCase.scenario == Scenario::Retargeting ||
                           benchmarkCase.scenario == Scenario::CombinedAnimation ||
                           booleanAnimation(benchmarkCase.scenario);
    const int frameIntervalMs =
        requestedFrameIntervalMs > 0 ? requestedFrameIntervalMs : (animation ? 17 : 8);
    Fixture fixture(logicalSize, animation, !animation, frameBackend);
    const bool magnifier = benchmarkCase.scenario == Scenario::MagnifierVisible ||
                           benchmarkCase.scenario == Scenario::MagnifierHidden ||
                           benchmarkCase.scenario == Scenario::CombinedAnimation ||
                           benchmarkCase.scenario == Scenario::ManualMarqueeMagnifier;
    const bool manual = benchmarkCase.scenario == Scenario::ManualMarquee ||
                        benchmarkCase.scenario == Scenario::ManualResize ||
                        benchmarkCase.scenario == Scenario::ManualMarqueeMagnifier;
    if (benchmarkCase.toolbarHidden) {
        fixture.coordinator.setSelectionToolbarHidden(true);
        fixture.processEvents();
        fixture.resetCounters();
    }
    if (magnifier)
        fixture.enableColorPicker(benchmarkCase.scenario == Scenario::MagnifierHidden
                                      ? ScreenshotColorPickerDisplayMode::AlwaysHide
                                      : ScreenshotColorPickerDisplayMode::AlwaysShow);
    if (benchmarkCase.scenario == Scenario::Pointer ||
        benchmarkCase.scenario == Scenario::CombinedAnimation)
        fixture.enableGuides();
    const QRectF base = fixture.baseSelection();
    const QSize canvasSize = fixture.geometry.canvasBounds().size().toSize();
    const auto overlayPosition = [&fixture](const QPointF& canvasPosition) {
        return fixture.geometry.logicalPositionForCanvasPoint(fixture.displays.displayAt(0),
                                                              canvasPosition) -
               fixture.overlay.captureGeometry().topLeft();
    };
    std::unique_ptr<ScreenshotOverlayInputHandler> input;
    if (manual) {
        fixture.displays.startup->resumeLiveInput();
        ScreenshotOverlayInputActions actions;
        actions.updateOverlayState = [&fixture] { fixture.services->updateOverlayState(); };
#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
        actions.requestSelectionDragPresentation = [&fixture] {
            fixture.services->requestSelectionDragPresentation();
        };
#endif
        actions.updateGuideLinesForOverlay = [&fixture](ScreenshotOverlayWindow* owner,
                                                        const QPointF& position) {
            fixture.services->updatePointerPresentation(owner, position);
        };
        if (magnifier) {
            actions.updateColorPickerForOverlay = [&fixture](ScreenshotOverlayWindow* owner,
                                                             const QPointF& position) {
                fixture.services->requestColorPickerPresentation(owner, position);
            };
            actions.updateColorPickerForSelectionDrag = [&fixture](const QPointF& position) {
#if defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
                fixture.services->discardColorPickerPresentation();
                fixture.colorPickerController->updateForSelectionDrag(
                    position, fixture.services->colorPickerContext());
#else
                fixture.services->requestSelectionDragColorPickerPresentation(position);
#endif
            };
        }
        input =
            std::make_unique<ScreenshotOverlayInputHandler>(ScreenshotOverlayInputHandlerContext{
                fixture.captureState, fixture.interaction, fixture.selection,
                fixture.intelligentSelection, fixture.geometry, fixture.displays,
                std::move(actions)});
        if (benchmarkCase.scenario == Scenario::ManualResize) {
            fixture.interaction.confirmSelection();
            fixture.services->updateOverlayState();
            if (!input->beginSelectionResizeAtCanvasPosition(base.bottomRight()))
                throw std::runtime_error("manual resize benchmark requires a resize gesture");
        } else {
            fixture.selection.clearSelection();
            fixture.interaction.returnToSelectionMode(false);
            fixture.services->updateOverlayState();
            input->handleMousePress(&fixture.overlay, overlayPosition(base.topLeft()));
        }
        fixture.flushFrame();
        fixture.processEvents();
        fixture.resetCounters();
    } else if (booleanOperation(benchmarkCase.scenario)) {
        fixture.selection.setSelectionRegion(complexConfirmedRegion(canvasSize));
        const bool subtract = benchmarkCase.scenario == Scenario::BooleanSubtractTargets ||
                              benchmarkCase.scenario == Scenario::BooleanSubtractPixelReuse ||
                              benchmarkCase.scenario == Scenario::BooleanSubtractAnimation;
        fixture.selection.beginRegionOperation(
            subtract ? ScreenshotSelectionModel::RegionOperation::Subtract
                     : ScreenshotSelectionModel::RegionOperation::Add);
        fixture.requestSelection(base);
        fixture.flushFrame();
        fixture.processEvents();
        fixture.resetCounters();
    }
    const int totalFrames = iterations + warmup;
    std::vector<QRectF> targets;
    std::vector<QPointF> pointers;
    targets.reserve(static_cast<std::size_t>(totalFrames * benchmarkCase.requestsPerFrame));
    pointers.reserve(targets.capacity());
    for (int frame = 0; frame < totalFrames; ++frame) {
        for (int request = 0; request < benchmarkCase.requestsPerFrame; ++request) {
            const int sample = frame * benchmarkCase.requestsPerFrame + request;
            if (booleanOperation(benchmarkCase.scenario)) {
                const qreal x = canvasSize.width() * 0.59 + frame % 83;
                const qreal y = canvasSize.height() * 0.38 + (frame * 3) % 47;
                const bool pixelReuse =
                    benchmarkCase.scenario == Scenario::BooleanAddPixelReuse ||
                    benchmarkCase.scenario == Scenario::BooleanSubtractPixelReuse;
                const qreal delta = pixelReuse ? 0.25 + 0.01 * request : 2.0 * request;
                targets.emplace_back(std::floor(x) + delta, std::floor(y) + delta,
                                     std::floor(canvasSize.width() * 0.18),
                                     std::floor(canvasSize.height() * 0.22));
            } else {
                targets.push_back(base.translated(2.0 * (1 + sample % 121), (sample * 3) % 71));
            }
            if (magnifier) {
                // Keep the sample band below the selection and its interactive toolbar.
                pointers.emplace_back(logicalSize.width() * 0.65 +
                                          sample % std::max(1, logicalSize.width() / 4),
                                      logicalSize.height() * 0.6 +
                                          (sample * 3) % std::max(1, logicalSize.height() / 5));
            } else {
                pointers.emplace_back(40.0 + sample % (logicalSize.width() - 80),
                                      40.0 + (sample * 3) % (logicalSize.height() - 80));
            }
        }
    }

    std::vector<double> submissionMs;
    std::vector<double> frameCommitMs;
    std::vector<double> preparationMs;
    std::vector<double> eventProcessingMs;
    std::vector<double> totalMs;
    submissionMs.reserve(static_cast<std::size_t>(iterations));
    frameCommitMs.reserve(static_cast<std::size_t>(iterations));
    preparationMs.reserve(static_cast<std::size_t>(iterations));
    eventProcessingMs.reserve(static_cast<std::size_t>(iterations));
    totalMs.reserve(static_cast<std::size_t>(iterations));
    qint64 semanticNotifications = 0;
    qint64 hintTranslationRequests = 0;
    qint64 canvasPaints = 0;
    qint64 uiPaints = 0;
    qint64 canvasDamagePixels = 0;
    qint64 displayedGeometryChanges = 0;
    qint64 targetRequests = 0;
    qint64 schedulerWakeups = 0;
    qint64 colorPickerPaints = 0;
    qint64 colorPickerMoveEvents = 0;
    qint64 colorPickerOwnerChangeEvents = 0;
#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
    qint64 colorPickerSamples = 0;
    qint64 colorPickerPreviews = 0;
    qint64 colorPickerPositionMoves = 0;
    qint64 colorPickerOwnerChanges = 0;
#endif
    // Preserve the input cadence across animation models, including archived baselines.
    constexpr int targetHoldMs = 101;
    const int framesPerTarget = (targetHoldMs + frameIntervalMs - 1) / frameIntervalMs + 2;
    QRectF previousDisplayed = fixture.displayedSelection();
    QElapsedTimer cadence;
    cadence.start();
    qint64 measurementStartedNs = 0;
    for (int frame = 0; frame < totalFrames; ++frame) {
        if (frame == warmup)
            measurementStartedNs = cadence.nsecsElapsed();
        fixture.resetCounters();
        QElapsedTimer preparation;
        preparation.start();
        const bool requestTarget = (benchmarkCase.scenario != Scenario::AnimationFrames &&
                                    benchmarkCase.scenario != Scenario::CombinedAnimation &&
                                    !booleanAnimation(benchmarkCase.scenario)) ||
                                   frame % framesPerTarget == 0;
        if (requestTarget || benchmarkCase.scenario == Scenario::CombinedAnimation) {
            for (int request = 0; request < benchmarkCase.requestsPerFrame; ++request) {
                const auto index =
                    static_cast<std::size_t>(frame * benchmarkCase.requestsPerFrame + request);
                switch (benchmarkCase.scenario) {
                case Scenario::SteadySelection:
                    fixture.requestSelection(base);
                    break;
                case Scenario::Pointer:
                    fixture.requestPointer(pointers[index]);
                    break;
                case Scenario::MagnifierVisible:
                case Scenario::MagnifierHidden:
                    fixture.requestMagnifier(pointers[index]);
                    break;
                case Scenario::ChangedSelection:
                case Scenario::AnimationFrames:
                case Scenario::Retargeting:
                    fixture.requestSelection(targets[index]);
                    break;
                case Scenario::CombinedAnimation:
                    if (requestTarget)
                        fixture.requestSelection(targets[index]);
                    fixture.requestMagnifier(pointers[index]);
                    break;
                case Scenario::ManualMarquee:
                case Scenario::ManualMarqueeMagnifier:
                    input->handleMouseMove(&fixture.overlay,
                                           overlayPosition(targets[index].bottomRight()));
                    break;
                case Scenario::ManualResize:
                    input->updateSelectionResizeAtCanvasPosition(targets[index].bottomRight());
                    break;
                case Scenario::BooleanAddTargets:
                case Scenario::BooleanSubtractTargets:
                case Scenario::BooleanAddPixelReuse:
                case Scenario::BooleanSubtractPixelReuse:
                case Scenario::BooleanAddAnimation:
                case Scenario::BooleanSubtractAnimation:
                    fixture.requestSelection(targets[index]);
                    break;
                }
            }
        }
        const qint64 submissionNs = preparation.nsecsElapsed();
        if (!animation) {
            fixture.advanceClock(frameIntervalMs);
            fixture.flushFrame();
        }
        // Scheduled animation commits occur during Qt event processing. Controlled
        // frames expose their explicit clock advance and flush as a separate phase.
        const qint64 preparationNs = animation ? submissionNs : preparation.nsecsElapsed();
        const double submissionMilliseconds = static_cast<double>(submissionNs) / 1000000.0;
        const double frameCommitMilliseconds =
            static_cast<double>(preparationNs - submissionNs) / 1000000.0;
        const double preparationMilliseconds = static_cast<double>(preparationNs) / 1000000.0;

        qint64 frameGeometryChanges = 0;
        const auto observeGeometry = [&]() {
            const QRectF displayed = fixture.displayedSelection();
            if (displayed != previousDisplayed) {
                ++frameGeometryChanges;
                previousDisplayed = displayed;
            }
        };
        observeGeometry();
        QElapsedTimer events;
        qint64 eventProcessingNs = 0;
        const auto processReadyEvents = [&]() {
            events.start();
            fixture.processEvents();
            eventProcessingNs += events.nsecsElapsed();
            observeGeometry();
        };
        if (animation) {
            // Keep Qt timers and paints running throughout each input interval. Sleeping for
            // the full interval would cap both animation drivers at the input sampling rate.
            // Fixed deadlines keep the supplied input cadence independent of painting cost.
            const qint64 deadlineNs = static_cast<qint64>(frame + 1) * frameIntervalMs * 1000000;
            while (true) {
                processReadyEvents();
                const qint64 remainingNs = deadlineNs - cadence.nsecsElapsed();
                if (remainingNs <= 0)
                    break;
                const qint64 sleepNs = std::min<qint64>(remainingNs, 1000000);
                QThread::usleep(static_cast<unsigned long>((sleepNs + 999) / 1000));
            }
        } else {
            processReadyEvents();
        }
        const double eventMilliseconds = static_cast<double>(eventProcessingNs) / 1000000.0;
        if (frame >= warmup) {
            submissionMs.push_back(submissionMilliseconds);
            frameCommitMs.push_back(frameCommitMilliseconds);
            preparationMs.push_back(preparationMilliseconds);
            eventProcessingMs.push_back(eventMilliseconds);
            totalMs.push_back(preparationMilliseconds + eventMilliseconds);
            semanticNotifications += fixture.stateNotifications;
            hintTranslationRequests += fixture.hintTranslations.requests;
            canvasPaints += fixture.paintObserver.canvasPaints;
            uiPaints += fixture.paintObserver.uiPaints;
            schedulerWakeups += fixture.frameWakeObserver.wakeups;
            canvasDamagePixels += fixture.paintObserver.canvasDamagePixels;
            displayedGeometryChanges += frameGeometryChanges;
            colorPickerPaints += fixture.paintObserver.colorPickerPaints;
            colorPickerMoveEvents += fixture.paintObserver.colorPickerMoves;
            colorPickerOwnerChangeEvents += fixture.paintObserver.colorPickerOwnerChanges;
#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
            if (auto* picker = fixture.coordinator.colorPicker()) {
                const auto counters = picker->workCounters();
                colorPickerSamples += static_cast<qint64>(counters.samples);
                colorPickerPreviews += static_cast<qint64>(counters.previews);
                colorPickerPositionMoves += static_cast<qint64>(counters.moves);
                colorPickerOwnerChanges += static_cast<qint64>(counters.ownerChanges);
            }
#endif
            if (requestTarget)
                targetRequests += benchmarkCase.requestsPerFrame;
        }
    }
    const double viewportPixels =
        static_cast<double>(fixture.overlay.canvas()->width()) * fixture.overlay.canvas()->height();
    const double elapsedMilliseconds =
        static_cast<double>(cadence.nsecsElapsed() - measurementStartedNs) / 1000000.0;
    const double workMilliseconds = std::accumulate(totalMs.begin(), totalMs.end(), 0.0);
    const QJsonValue millisecondsPerPaint =
        canvasPaints > 0 ? QJsonValue(workMilliseconds / static_cast<double>(canvasPaints))
                         : QJsonValue(QJsonValue::Null);
    const QJsonValue millisecondsPerGeometryChange =
        displayedGeometryChanges > 0
            ? QJsonValue(workMilliseconds / static_cast<double>(displayedGeometryChanges))
            : QJsonValue(QJsonValue::Null);
    const qint64 inputSamples = static_cast<qint64>(iterations) * benchmarkCase.requestsPerFrame;
    const bool selectionIncluded = benchmarkCase.scenario != Scenario::Pointer &&
                                   benchmarkCase.scenario != Scenario::MagnifierVisible &&
                                   benchmarkCase.scenario != Scenario::MagnifierHidden;
    return {{"scenario", QString::fromLatin1(benchmarkCase.name)},
            {"iterations", iterations},
            {"requests_per_frame", benchmarkCase.requestsPerFrame},
            {"selection_toolbar_hidden", benchmarkCase.toolbarHidden},
            {"magnifier_included", magnifier},
            {"manual_input_handler_included", manual},
            {"boolean_region_operation_included", booleanOperation(benchmarkCase.scenario)},
            {"magnifier_display_mode", !magnifier ? "excluded"
                                       : benchmarkCase.scenario == Scenario::MagnifierHidden
                                           ? "always_hide"
                                           : "always_show"},
            {"request_count",
             benchmarkCase.scenario == Scenario::CombinedAnimation ? inputSamples : targetRequests},
            {"selection_request_count", selectionIncluded ? targetRequests : 0},
            {"pointer_request_count",
             magnifier || benchmarkCase.scenario == Scenario::Pointer ? inputSamples : 0},
            {"magnifier_request_count", magnifier ? inputSamples : 0},
            {"scheduler_wake_events", schedulerWakeups},
            {"clock", animation ? "real_monotonic" : "controlled_monotonic"},
            {"frame_commit", animation ? "scheduled_timer" : "explicit_frame"},
            {"input_interval_ms", frameIntervalMs},
            {"maximum_requested_animation_idle_wait_ms", animation ? 1 : 0},
            {"elapsed_ms", elapsedMilliseconds},
            {"submission_ms", distribution(std::move(submissionMs))},
            {"frame_commit_ms", distribution(std::move(frameCommitMs))},
            {"preparation_ms", distribution(std::move(preparationMs))},
            {"event_processing_ms", distribution(std::move(eventProcessingMs))},
            {"total_ms", distribution(std::move(totalMs))},
            {"semantic_notifications", semanticNotifications},
            {"hint_translation_requests", hintTranslationRequests},
            {"canvas_paints", canvasPaints},
            {"ui_paints", uiPaints},
            {"magnifier_paints", colorPickerPaints},
            {"magnifier_move_events", colorPickerMoveEvents},
            {"magnifier_owner_change_events", colorPickerOwnerChangeEvents},
#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
            {"magnifier_samples", colorPickerSamples},
            {"magnifier_preview_rebuilds", colorPickerPreviews},
            {"magnifier_position_moves", colorPickerPositionMoves},
            {"magnifier_owner_changes", colorPickerOwnerChanges},
#else
            {"magnifier_samples", QJsonValue(QJsonValue::Null)},
            {"magnifier_preview_rebuilds", QJsonValue(QJsonValue::Null)},
            {"magnifier_position_moves", QJsonValue(QJsonValue::Null)},
            {"magnifier_owner_changes", QJsonValue(QJsonValue::Null)},
#endif
            {"displayed_geometry_changes", displayedGeometryChanges},
            {"work_ms_per_canvas_paint", millisecondsPerPaint},
            {"work_ms_per_geometry_change", millisecondsPerGeometryChange},
            {"mean_canvas_damage_ratio",
             static_cast<double>(canvasDamagePixels) / (viewportPixels * iterations)}};
}

#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
QJsonObject runEffectWheelComparison(const QSize& size, int iterations, int warmup,
                                     FrameBackend backend) {
    Fixture fixture(size, false, true, backend);
    fixture.interaction.confirmSelection();
    fixture.interaction.setSelectionBackgroundPreviewVisible(true);
    fixture.services->updateOverlayState();
    constexpr int requests = 16;
    std::array<std::vector<double>, 2> submissions, frames;
    std::array<qint64, 2> notifications{};
    for (int frame = -warmup; frame < iterations; ++frame) {
        // Alternate the order so neither arm consistently gets colder caches.
        for (int order = 0; order < 2; ++order) {
            const auto arm = static_cast<std::size_t>((frame + warmup + order) % 2);
            static_cast<void>(fixture.selection.setCornerRadius(20));
            static_cast<void>(fixture.selection.setShadowWidth(16));
            static_cast<void>(fixture.selection.setBackgroundOpacityPercent(40));
            fixture.services->updateOverlayState();
            fixture.processEvents();
            fixture.resetCounters();
            QElapsedTimer timer;
            timer.start();
            for (int request = 0; request < requests; ++request) {
                if (request % 3 == 0)
                    static_cast<void>(
                        fixture.selection.setCornerRadius(fixture.selection.cornerRadius() + 1));
                else if (request % 3 == 1)
                    static_cast<void>(
                        fixture.selection.setShadowWidth(fixture.selection.shadowWidth() + 1));
                else
                    static_cast<void>(fixture.selection.setBackgroundOpacityPercent(
                        fixture.selection.backgroundOpacityPercent() + 1));
                if (arm == 0)
                    fixture.services->updateOverlayState();
                else
                    fixture.services->requestSelectionEffectPresentation();
            }
            const double submitted = static_cast<double>(timer.nsecsElapsed()) / 1e6;
            if (fixture.stateNotifications != (arm == 0 ? requests : 0))
                throw std::runtime_error("wheel submission must preserve its presentation policy");
            fixture.flushFrame();
            fixture.processEvents();
            const double total = static_cast<double>(timer.nsecsElapsed()) / 1e6;
            if (fixture.selection.cornerRadius() != 26 || fixture.selection.shadowWidth() != 21 ||
                fixture.selection.backgroundOpacityPercent() != 45 ||
                fixture.stateNotifications != (arm == 0 ? requests : 1))
                throw std::runtime_error("wheel frame must retain every delta and latest state");
            if (frame >= 0) {
                submissions[arm].push_back(submitted);
                frames[arm].push_back(total);
                notifications[arm] += fixture.stateNotifications;
            }
        }
    }
    QJsonArray arms;
    for (std::size_t arm = 0; arm < 2; ++arm)
        arms.append(QJsonObject{{"policy", arm == 0 ? "immediate" : "coalesced"},
                                {"submission_ms", distribution(submissions[arm])},
                                {"frame_total_ms", distribution(frames[arm])},
                                {"semantic_notifications", notifications[arm]}});
    return {{"scenario", "effect-wheel"},
            {"requests_per_frame", requests},
            {"measurement",
             "Alternating same-process comparison of identical model deltas through immediate "
             "and frame-coalesced presentation, using the same renderer. Submission excludes "
             "frame commit; total includes commit and Qt paint/event processing. Excludes native "
             "wheel delivery, settings persistence and compositor/input-to-photon latency."},
            {"arms", arms}};
}
#endif

int positiveInteger(const QCommandLineParser& parser, const QString& name) {
    bool valid = false;
    const int value = parser.value(name).toInt(&valid);
    if (!valid || value <= 0)
        throw std::runtime_error("benchmark counts and dimensions must be positive integers");
    return value;
}
} // namespace

int main(int argc, char* argv[]) {
    bool native = false;
    for (int argument = 1; argument < argc; ++argument)
        native = native || std::strcmp(argv[argument], "--native") == 0;
    if (!native)
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Real screenshot selection presentation benchmark"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("native"), QStringLiteral("Use the native Qt platform")});
    parser.addOption(
        {QStringLiteral("offscreen"), QStringLiteral("Use the offscreen Qt platform")});
    parser.addOption({QStringLiteral("external-initialization"),
                      QStringLiteral("Compare two-step and batched hidden global mouse setup")});
    parser.addOption({QStringLiteral("iterations"), QStringLiteral("Measured frames"),
                      QStringLiteral("count"), QStringLiteral("240")});
    parser.addOption({QStringLiteral("warmup"), QStringLiteral("Warmup frames"),
                      QStringLiteral("count"), QStringLiteral("30")});
    parser.addOption({QStringLiteral("width"), QStringLiteral("Logical overlay width"),
                      QStringLiteral("pixels"), QStringLiteral("1800")});
    parser.addOption({QStringLiteral("height"), QStringLiteral("Logical overlay height"),
                      QStringLiteral("pixels"), QStringLiteral("975")});
    parser.addOption({QStringLiteral("frame-interval-ms"),
                      QStringLiteral("Sample interval; default 17ms animation, 8ms other cases"),
                      QStringLiteral("milliseconds")});
    parser.addOption({QStringLiteral("scenario"),
                      QStringLiteral("Scenario name, optimizations, or all"),
                      QStringLiteral("name"), QStringLiteral("all")});
    parser.addOption({QStringLiteral("frame-backend"),
                      QStringLiteral("Presentation wakeup backend: automatic or qt_timer"),
                      QStringLiteral("backend"), QStringLiteral("automatic")});
    parser.addOption(
        {QStringLiteral("output"), QStringLiteral("JSON output path"), QStringLiteral("path")});
    parser.process(application);
    const QString backendName = parser.value(QStringLiteral("frame-backend"));
    if (backendName != QStringLiteral("automatic") && backendName != QStringLiteral("qt_timer"))
        throw std::runtime_error("frame-backend must be automatic or qt_timer");
    const auto frameBackend =
        backendName == QStringLiteral("qt_timer") ? FrameBackend::QtTimer : FrameBackend::Automatic;
    const int iterations = positiveInteger(parser, QStringLiteral("iterations"));
    const int warmup = positiveInteger(parser, QStringLiteral("warmup"));
    const QSize logicalSize(positiveInteger(parser, QStringLiteral("width")),
                            positiveInteger(parser, QStringLiteral("height")));
    if (logicalSize.width() <= 80 || logicalSize.height() <= 80)
        throw std::runtime_error("benchmark overlay dimensions must exceed 80 pixels");
    const int frameIntervalMs = parser.isSet(QStringLiteral("frame-interval-ms"))
                                    ? positiveInteger(parser, QStringLiteral("frame-interval-ms"))
                                    : 0;
    selection_presentation_test::IsolatedStorage storage;
    QJsonArray results;
    const bool externalInitialization = parser.isSet(QStringLiteral("external-initialization"));
    const QString selected = parser.value(QStringLiteral("scenario"));
    if (externalInitialization) {
#if defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
        throw std::runtime_error("external initialization comparison requires the current runtime");
#else
        for (const bool pickerEnabled : {false, true})
            results.append(runExternalInitializationCase(logicalSize, iterations, warmup,
                                                         pickerEnabled, frameBackend));
#endif
    } else if (selected == QStringLiteral("effect-wheel")) {
#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
        results.append(runEffectWheelComparison(logicalSize, iterations, warmup, frameBackend));
#endif
    } else if (selected == QStringLiteral("independent-regions")) {
#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
        for (const int count : {1, 16, 64})
            for (const bool curves : {false, true})
                for (const bool duplicate : {false, true})
                    results.append(runIndependentRegionCase(logicalSize, count, curves, duplicate,
                                                            iterations, warmup, frameBackend));
#endif
    } else {
        for (const Case& benchmarkCase : kCases) {
            const bool optimizationCase =
                booleanOperation(benchmarkCase.scenario) ||
                benchmarkCase.scenario == Scenario::ManualMarquee ||
                benchmarkCase.scenario == Scenario::ManualResize ||
                benchmarkCase.scenario == Scenario::ManualMarqueeMagnifier;
            if (selected == QStringLiteral("all") ||
                selected == QLatin1String(benchmarkCase.name) ||
                (selected == QStringLiteral("optimizations") && optimizationCase))
                results.append(runCase(benchmarkCase, logicalSize, iterations, warmup,
                                       frameIntervalMs, frameBackend));
        }
    }
    if (results.isEmpty())
        throw std::runtime_error("unknown selection presentation benchmark scenario");
    const qreal dpr = QGuiApplication::primaryScreen()->devicePixelRatio();
    QJsonObject report{
        {"benchmark", "screenshot_selection_presentation"},
        {"platform", QGuiApplication::platformName()},
        {"scheduler_backend", backendName},
#if defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
        {"implementation", "baseline"},
#else
        {"implementation", "optimized"},
#endif
        {"device_pixel_ratio", dpr},
        {"warmup", warmup},
        {"logical_width", logicalSize.width()},
        {"logical_height", logicalSize.height()},
        {"physical_width", static_cast<int>(std::ceil(logicalSize.width() * dpr))},
        {"physical_height", static_cast<int>(std::ceil(logicalSize.height() * dpr))},
        {"screen_refresh_hz", QGuiApplication::primaryScreen()->refreshRate()},
        {"animation_sample_interval_ms", frameIntervalMs > 0 ? frameIntervalMs : 17},
        {"measurement",
         selected == QStringLiteral("independent-regions")
             ? "Real input-handler and presentation-service work for independent region dragging "
               "and duplication. Each drag frame includes sixteen identical pointer events and "
               "asserts exactly one composition; stationary geometry is composed once at press. "
               "Duplication resets the baseline outside the measured interval. Submission excludes "
               "frame delivery; frame_total includes submission, explicit frame delivery, and Qt "
               "event processing/painting. Retained geometry is a conservative upper estimate of "
               "region, drag, and combined-mask storage, counting shared operands more than once; "
               "it excludes image surfaces and process memory. Native mouse delivery, selector "
               "workers, idle waits, and input-to-photon latency are excluded."
         : externalInitialization
             ? "Same-process alternating A/B comparison of the real external drag "
               "input handler and presentation services on a captured hidden "
               "overlay. Both paths start in the workflow's committed empty "
               "manual marquee state. synchronous_init_us measures exactly the "
               "before-reveal initialization calls on the GUI thread. "
               "pending_frame_flush_us measures explicit delivery of remaining "
               "presentation and picker requests separately. Reset, queued Qt "
               "events, capture, selector workers, native mouse delivery, reveal "
               "painting, and input-to-photon latency are excluded. Picker cases "
               "include the real controller and window, reset identically before "
               "each sample. Geometry and notification guards apply to every "
               "warmup and measured pair."
             : "Request submission and explicit frame commit are measured separately; preparation "
               "includes both phases. Scheduled animation frame_commit_ms is zero, with timer "
               "commits "
               "included in Qt event processing. "
               "Controlled cases commit once per supplied frame; animation uses each "
               "implementation's "
               "event-driven timer with event pumping and requested idle waits of at most 1ms. "
               "Event-processing work and counters cover the entire input interval. Compare "
               "animation "
               "costs with observed paint and geometry counts. Magnifier cases include pointer "
               "presentation and the real picker controller/window. The optional archived legacy "
               "baseline target updates hover magnifiers immediately and reports internal "
               "sampling/preview "
               "counters as null; the current target batches hover magnifiers with the "
               "presentation "
               "frame. "
               "Picker paint/move/owner-change event counts are comparable. Combined cases include "
               "animated "
               "selection, guides, and magnifier bursts. "
               "Manual cases deliver coordinates through the real input handler and include "
               "constrained "
               "drag geometry; manual magnifier cases also include the real drag anchor sampler. "
               "Boolean cases use a concave freehand contour with two holes; pixel reuse "
               "keeps sixteen subpixel requests inside one integer marquee per frame, and "
               "animation "
               "allows displayed marquees to evolve between target requests. "
               "Idle waits, selector workers, native mouse delivery, and input-to-photon latency "
               "are excluded from work timings."},
        {"results", results}};
    const QByteArray json = QJsonDocument(report).toJson(QJsonDocument::Indented);
    if (parser.isSet(QStringLiteral("output"))) {
        QFile file(parser.value(QStringLiteral("output")));
        if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size())
            throw std::runtime_error("unable to save selection presentation benchmark JSON");
    }
    QTextStream(stdout) << json;
    return 0;
}
