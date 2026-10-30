#include "screenshot_selection_presentation_fixture.h"
#include "snow_shot/presentation/screenshotoverlayinputhandler.h"
#include "snow_shot/presentation/screenshotselectioneditworkflow.h"
#include "snow_shot/presentation/screenshottoolpalette.h"

#include <QLabel>
#include <QPainterPath>
#include <QWheelEvent>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct WheelFixture {
    selection_presentation_test::Fixture presentation{QSize(1200, 800), false};
    std::unique_ptr<ScreenshotSelectionEditWorkflow> workflow;
    std::unique_ptr<ScreenshotOverlayInputHandler> input;
    int updates = 0;
    int toolbarMoves = 0;

    WheelFixture() {
        presentation.interaction.setMoveTool(true, false);
        presentation.captureState.sessionState = ScreenshotSessionState::Editing;
        ScreenshotSelectionEditUiActions ui;
        ui.updateOverlayState = [this] {
            ++updates;
            presentation.services->updateOverlayState();
        };
        ui.requestSelectionEffectPresentation = [this] {
            presentation.services->requestSelectionEffectPresentation();
        };
        ui.showSelectionToolbar = [this] { presentation.services->showSelectionToolbar(); };
        ui.moveToolbar = [this] {
            ++toolbarMoves;
            presentation.services->moveToolbar();
        };
        workflow = std::make_unique<ScreenshotSelectionEditWorkflow>(
            ScreenshotSelectionEditWorkflowContext{presentation.overlay, presentation.captureState,
                                                   presentation.displays, presentation.geometry,
                                                   presentation.interaction, presentation.selection,
                                                   std::move(ui)});
        presentation.commands.adjustSelectionEffects = [this](int radius, int shadow) {
            workflow->adjustSelectionEffectsFromWheel(radius, shadow);
        };
        presentation.commands.adjustBackgroundOpacity = [this](int delta) {
            if (presentation.selection.setBackgroundOpacityPercent(
                    presentation.selection.backgroundOpacityPercent() + delta))
                presentation.services->requestSelectionEffectPresentation();
        };
        ScreenshotOverlayInputActions actions;
        actions.adjustSelectionFromToolbar = [this](int minDx, int minDy, int maxDx, int maxDy) {
            workflow->adjustSelectionFromToolbar(minDx, minDy, maxDx, maxDy);
        };
        input =
            std::make_unique<ScreenshotOverlayInputHandler>(ScreenshotOverlayInputHandlerContext{
                presentation.captureState, presentation.interaction, presentation.selection,
                presentation.intelligentSelection, presentation.geometry, presentation.displays,
                std::move(actions)});
        select(QRectF(200, 200, 300, 180));
    }

    void select(const QRectF& selection) {
        presentation.selection.setSelectionRect(selection);
        presentation.services->updateOverlayState();
        presentation.services->showToolbar();
        presentation.services->showSelectionToolbar();
        presentation.flushFrame();
        presentation.processEvents();
        updates = 0;
        toolbarMoves = 0;
        input->resetTransientShortcuts();
    }

    bool scroll(QPoint angles, QPoint pixels = {}, Qt::ScrollPhase phase = Qt::NoScrollPhase,
                QPointF pointer = QPointF(210, 210)) {
        QWheelEvent event(pointer, presentation.overlay.mapToGlobal(pointer.toPoint()), pixels,
                          angles, Qt::NoButton, Qt::NoModifier, phase, false);
        return input->handleWheel(&presentation.overlay, event);
    }

    QRectF selection() const {
        return presentation.selection.normalizedSelection();
    }
};

void effectWheelBurstsPreserveDeltasAndCoalesceFrames() {
    WheelFixture fixture;
    auto& p = fixture.presentation;
    fixture.workflow->setSelectionCornerRadiusFromToolbar(250);
    fixture.workflow->setSelectionShadowWidthFromToolbar(63);
    p.resetCounters();
    auto* toolbar = p.coordinator.selectionToolbar();
    const auto wheel = [&](const char* source, int direction) {
        QLabel* target = nullptr;
        for (auto* label : toolbar->findChildren<QLabel*>()) {
            if (label->property("selectionToolbarTranslationSource").toString() ==
                    QLatin1String(source) &&
                label->property("selectionToolbarField").isValid())
                target = label;
        }
        require(target != nullptr, "effect wheel label must exist");
        QWheelEvent event(target->rect().center(), target->mapToGlobal(target->rect().center()), {},
                          QPoint(0, direction * 120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(target, &event);
        require(event.isAccepted(), "effect label must accept wheel input");
    };
    for (int i = 0; i < 10; ++i)
        wheel("Corner radius", 1);
    for (int i = 0; i < 7; ++i)
        wheel("Corner radius", -1);
    for (int i = 0; i < 5; ++i)
        wheel("Shadow width", 1);
    for (int i = 0; i < 3; ++i)
        wheel("Shadow width", -1);
    require(
        p.selection.cornerRadius() == 249 && p.selection.shadowWidth() == 61,
        "wheel deltas must clamp individually and reverse immediately without toolbar feedback");
    auto* renderer = p.overlay.screenshotRendererForTesting();
    require(p.stateNotifications == 0 && renderer->selectionCornerRadius() == 250 &&
                renderer->selectionShadowWidth() == 63 &&
                p.services->frameSchedulerForTesting().active(),
            "wheel bursts must defer presentation to one pending frame");
    p.flushFrame();
    require(p.stateNotifications == 1 && renderer->selectionCornerRadius() == 249 &&
                renderer->selectionShadowWidth() == 61 &&
                !p.services->frameSchedulerForTesting().active(),
            "one frame must publish the latest effects and leave no idle polling");
    p.resetCounters();
    wheel("Corner radius", 1);
    fixture.workflow->setSelectionShadowWidthFromToolbar(12);
    require(p.stateNotifications == 1 && renderer->selectionCornerRadius() == 250 &&
                renderer->selectionShadowWidth() == 12,
            "ordinary commands must flush pending wheel edits synchronously");
    p.flushFrame();
    require(p.stateNotifications == 1, "superseded wheel frames must not publish twice");
    wheel("Corner radius", 1);
    p.services->resetPresentation();
    p.resetCounters();
    p.flushFrame();
    require(p.stateNotifications == 0 && !p.services->frameSchedulerForTesting().active(),
            "capture teardown must discard pending wheel presentation");
}

void opacityWheelSharesTheEffectFrame() {
    WheelFixture fixture;
    auto& p = fixture.presentation;
    auto* toolbar = p.coordinator.toolbar();
    auto* palette = toolbar->findChild<ScreenshotToolPalette*>();
    require(palette != nullptr, "main toolbar must own the real palette");
    auto* editor =
        palette->findChild<QWidget*>(QStringLiteral("screenshotSelectionBackgroundOpacityEditor"));
    require(editor && editor->isEnabled() && editor->isVisible(), "opacity wheel editor is ready");
    p.resetCounters();
    const auto wheel = [&](int delta) {
        const QPoint point = editor->rect().center();
        QWheelEvent event(point, editor->mapToGlobal(point), {}, QPoint(0, delta * 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        require(palette->handleToolbarWheel(&event), "palette must consume opacity wheel");
    };
    wheel(1);
    require(p.selection.backgroundOpacityPercent() == 10 && p.stateNotifications == 0,
            "one opacity notch must apply ten percentage points before presentation");
    for (int i = 0; i < 14; ++i)
        wheel(1);
    for (int i = 0; i < 7; ++i)
        wheel(-1);
    fixture.workflow->adjustSelectionEffectsFromWheel(4, 3);
    require(p.selection.backgroundOpacityPercent() == 30 && p.selection.cornerRadius() == 4 &&
                p.selection.shadowWidth() == 3 && p.stateNotifications == 0,
            "ten-point opacity wheels must clamp individually and update before the shared frame");
    p.flushFrame();
    require(p.stateNotifications == 1, "mixed effect wheels must share one semantic frame");
    p.resetCounters();
    for (int i = 0; i < 15; ++i)
        wheel(-1);
    wheel(1);
    p.flushFrame();
    require(p.selection.backgroundOpacityPercent() == 10 && p.stateNotifications == 1,
            "opacity must reverse at zero and deliver the final partial burst");

    p.selection.setBackgroundOpacityPercent(95);
    p.services->updateOverlayState();
    p.flushFrame();
    p.resetCounters();
    wheel(1);
    require(p.selection.backgroundOpacityPercent() == 100 && p.stateNotifications == 0,
            "a partial opacity step must clamp the model at 100 before presentation");
    p.flushFrame();
    require(p.stateNotifications == 1, "a clamped opacity change must publish once");
    p.resetCounters();
    wheel(1);
    p.flushFrame();
    require(p.selection.backgroundOpacityPercent() == 100 && p.stateNotifications == 0 &&
                !p.services->frameSchedulerForTesting().active(),
            "opacity wheel input at the limit must not schedule a redundant preview frame");
    wheel(-1);
    require(p.selection.backgroundOpacityPercent() == 90 && p.stateNotifications == 0,
            "opacity must reverse from the maximum by ten points immediately");
    p.flushFrame();
    require(p.stateNotifications == 1, "reversing opacity must publish one preview frame");
}

void wheelMovesEveryEdgeByOneUnitAndSynchronizesToolbars() {
    WheelFixture fixture;
    const QRectF original = fixture.selection();
    auto* mainToolbar = fixture.presentation.coordinator.toolbar();
    auto* selectionToolbar = fixture.presentation.coordinator.selectionToolbar();
    require(mainToolbar && selectionToolbar && mainToolbar->isVisible() &&
                selectionToolbar->isVisible(),
            "wheel fixture must show both screenshot toolbars");
    const QPoint mainPosition = mainToolbar->pos();
    const QPoint selectionPosition = selectionToolbar->pos();
    require(fixture.scroll(QPoint(0, 120), QPoint(0, 30)) &&
                fixture.selection() == original.adjusted(-1, -1, 1, 1) &&
                fixture.selection().center() == original.center(),
            "one mouse notch must move every edge by one canvas unit around the center");
    require(fixture.updates == 1 && fixture.toolbarMoves == 1 &&
                mainToolbar->pos() != mainPosition && selectionToolbar->pos() != selectionPosition,
            "both screenshot toolbars must move synchronously with wheel geometry");
    require(fixture.scroll(QPoint(0, -120), {}, Qt::NoScrollPhase, QPointF(900, 700)) &&
                fixture.selection() == original && mainToolbar->pos() == mainPosition &&
                selectionToolbar->pos() == selectionPosition,
            "reverse scrolling must restore geometry and toolbar positions regardless of pointer");
    require(fixture.scroll(QPoint(0, 360)) &&
                fixture.selection() == original.adjusted(-3, -3, 3, 3) && fixture.updates == 3,
            "coalesced notches must apply all integer edge offsets in one update");

    fixture.select(original);
    require(fixture.presentation.selection.setAspectRatioLockEnabled(true, 1),
            "enable an existing aspect ratio lock");
    require(fixture.scroll(QPoint(0, 120)) &&
                fixture.selection() == original.adjusted(-1, -1, 1, 1),
            "centered wheel edits must use direct offsets without proportional scaling");
}

void centeredWheelStopsAtCanvasAndMinimumSize() {
    WheelFixture fixture;
    const QRectF bounds = fixture.presentation.geometry.canvasBounds();
    const QRectF nearEdge(bounds.left() + 1, bounds.top() + 20, 100, 80);
    fixture.select(nearEdge);
    require(fixture.scroll(QPoint(0, 1200)) &&
                fixture.selection() == nearEdge.adjusted(-1, -1, 1, 1) &&
                fixture.selection().center() == nearEdge.center(),
            "expansion must stop at the nearest canvas edge without moving the center");
    require(fixture.scroll(QPoint(0, 120)) && fixture.updates == 1 && fixture.toolbarMoves == 1,
            "scrolling against a canvas limit must not issue redundant presentation updates");
    const QRectF nearBottomRight(bounds.right() - 101, bounds.bottom() - 81, 100, 80);
    fixture.select(nearBottomRight);
    require(fixture.scroll(QPoint(0, 1200)) &&
                fixture.selection() == nearBottomRight.adjusted(-1, -1, 1, 1),
            "right and bottom canvas limits must constrain the same centered edge offsets");

    const QRectF small(200, 200, 3, 5);
    fixture.select(small);
    require(fixture.scroll(QPoint(0, -1200)) &&
                fixture.selection() == small.adjusted(1, 1, -1, -1) &&
                fixture.selection().center() == small.center(),
            "coalesced shrinking must stop at one unit without flipping the rectangle");
    require(fixture.scroll(QPoint(0, -120)) && fixture.updates == 1 && fixture.toolbarMoves == 1,
            "scrolling against the minimum size must leave the toolbars and geometry unchanged");
    require(fixture.scroll(QPoint(0, 120)) && fixture.selection() == small,
            "reversing direction at a size limit must respond immediately");
}

void preciseWheelAccumulatesAndResetsWithInputOwnership() {
    WheelFixture fixture;
    const QRectF original = fixture.selection();
    require(fixture.scroll({}, {}, Qt::ScrollBegin) &&
                fixture.scroll({}, QPoint(20, 0), Qt::ScrollUpdate) && fixture.updates == 0,
            "empty and horizontal wheel input must not resize the selection");
    require(fixture.scroll({}, QPoint(0, 1), Qt::ScrollUpdate) &&
                fixture.selection() == original.adjusted(-1, -1, 1, 1),
            "precise scrolling must respond with one centered unit immediately");
    require(fixture.scroll({}, QPoint(0, 99), Qt::ScrollUpdate) && fixture.updates == 1,
            "small precise movements must accumulate instead of resizing on every event");
    require(fixture.scroll({}, QPoint(0, 1), Qt::ScrollUpdate) &&
                fixture.selection() == original.adjusted(-2, -2, 2, 2),
            "the next precise threshold must apply exactly one more unit");
    require(fixture.scroll({}, QPoint(0, 500), Qt::ScrollMomentum) &&
                fixture.scroll({}, QPoint(0, 500), Qt::ScrollEnd) && fixture.updates == 2,
            "momentum and scroll-end deltas must not resize the selection");

    fixture.input->setExternalDragActive(true);
    require(fixture.scroll(QPoint(0, 120)) && fixture.updates == 2,
            "external drag must consume wheel input without changing selection geometry");
    fixture.input->setExternalDragActive(false);
    require(fixture.scroll({}, QPoint(0, 1), Qt::ScrollUpdate) && fixture.updates == 3,
            "resizing after external drag must start with a fresh precise wheel accumulator");
    fixture.presentation.interaction.setCanvasTool(ScreenshotActiveTool::Text);
    require(!fixture.scroll({}, QPoint(0, 99), Qt::ScrollUpdate) && fixture.updates == 3,
            "drawing tools must retain wheel ownership without resizing the screenshot");
    fixture.presentation.interaction.setMoveTool(true, false);
    require(fixture.scroll({}, QPoint(0, 1), Qt::ScrollUpdate) && fixture.updates == 4,
            "reactivating Edit Selection must discard the previous tool's wheel remainder");
}

void wheelDoesNotReplaceCustomRegionsOrInterruptSelectionGestures() {
    WheelFixture fixture;
    const QRectF original = fixture.selection();
    require(fixture.presentation.interaction.enterSelectionDrag(ScreenshotSelectionDragMode::All),
            "start an Edit Selection drag");
    require(!fixture.scroll(QPoint(0, 120)) && fixture.selection() == original,
            "wheel edits must not interrupt a selection drag");
    fixture.presentation.interaction.finishDrag();
    fixture.presentation.interaction.setMoveTool(true, false);
    require(fixture.presentation.interaction.beginEffectDrag(
                {ScreenshotSelectionEffectHandle::Shadow, {}, 0, 0, 0, 1, {}}),
            "start a selection effect drag");
    require(!fixture.scroll(QPoint(0, 120)) && fixture.selection() == original,
            "wheel edits must not interrupt a selection effect drag");
    fixture.presentation.interaction.finishEffectDrag();
    fixture.presentation.interaction.enterScrollingCapture();
    require(!fixture.scroll(QPoint(0, 120)) && fixture.selection() == original,
            "scrolling capture must retain the wheel without resizing the selection");
    fixture.presentation.interaction.setMoveTool(true, false);
    QPainterPath path;
    path.addEllipse(original);
    fixture.presentation.selection.setSelectionRegion(
        ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Curve));
    const auto custom = fixture.presentation.selection.selectionRegion();
    require(!fixture.scroll(QPoint(0, 120)) &&
                fixture.presentation.selection.selectionRegion() == custom && fixture.updates == 0,
            "wheel edits must not replace a custom region with a rectangle");
    fixture.presentation.selection.clearSelection();
    require(!fixture.scroll(QPoint(0, 120)) && !fixture.presentation.selection.hasPixelSelection(),
            "wheel input must not create a selection when none exists");
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    selection_presentation_test::IsolatedStorage storage;
    effectWheelBurstsPreserveDeltasAndCoalesceFrames();
    opacityWheelSharesTheEffectFrame();
    wheelMovesEveryEdgeByOneUnitAndSynchronizesToolbars();
    centeredWheelStopsAtCanvasAndMinimumSize();
    preciseWheelAccumulatesAndResetsWithInputOwnership();
    wheelDoesNotReplaceCustomRegionsOrInterruptSelectionGestures();
    return 0;
}
