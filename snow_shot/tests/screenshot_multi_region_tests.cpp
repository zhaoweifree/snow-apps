#include "snow_shot/image/screenshotregionpoints.h"
#include <QLineF>
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshotsmartselectiontransition.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"
#include "snow_shot/storage/persistedselectioncodec.h"

#include "snow_draw_engine_qt/snow_canvas_path_geometry.h"
#include <QGuiApplication>
#include <QJsonArray>
#include <QPainterPathStroker>
#include <QPainter>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void apply(ScreenshotSelectionModel& model, ScreenshotSelectionModel::RegionOperation operation,
           const QRect& rectangle) {
    const auto before = model.selectionRegion();
    model.beginRegionOperation(operation);
    require(model.confirmedRegion() == before, "entry must preserve confirmed geometry");
    model.setSelectionRect(rectangle);
    const auto preview = model.selectionRegion();
    model.commitRegionOperation();
    require(model.selectionRegion() == preview, "committed geometry must equal preview");
}
void geometryAndTransactions() {
    using Operation = ScreenshotSelectionModel::RegionOperation;
    ScreenshotSelectionModel model;
    model.setSelectionRect(QRect(10, 10, 80, 60));
    apply(model, Operation::Add, QRect(70, 10, 50, 60));
    require(model.rectangular() && model.pixelSelection() == QRect(10, 10, 110, 60),
            "overlapping rectangles should restore rectangular resizing");
    apply(model, Operation::Add, QRect(120, 10, 10, 60));
    require(model.rectangular(), "adjacent rectangles must merge without seams");
    apply(model, Operation::Add, QRect(30, 20, 10, 10));
    require(model.rectangular(), "contained additions must be no-ops");
    apply(model, Operation::Subtract, QRect(40, 25, 30, 20));
    require(!model.rectangular() && !model.selectionRegion().contains(QPoint(50, 30)),
            "internal subtraction must leave a hole");
    const auto hole = model.selectionRegion();
    model.beginRegionOperation(Operation::Add);
    require(model.selectionRegion() == hole, "entry without a marquee must not change mask");
    model.setSelectionRect(QRect(40, 25, 30, 20));
    require(model.selectionRegion() == QRegion(QRect(10, 10, 120, 60)), "hole fill preview");
    model.cancelRegionOperation();
    require(model.selectionRegion() == hole, "cancel must restore original shape");
    model.setSelectionRegion(hole); // A restored aggregate has no creation identities.
    model.beginMoveDrag(QPointF(10, 10));
    model.setDraggedSelectionRect(QRectF(30, 40, 120, 60), ScreenshotSelectionDragMode::All);
    require(model.selectionRegion() == hole.translated(20, 30), "move must preserve holes");
    require(!model.adjustFromToolbar(0, 0, 1, 0, QRectF(0, 0, 300, 300), 1),
            "toolbar must reject complex resizing");
    require(model.adjustFromToolbar(1, 0, 1, 0, QRectF(0, 0, 300, 300), 1),
            "toolbar translation must remain available");
    apply(model, Operation::Subtract, QRect(0, 0, 300, 300));
    require(model.selectionRegion().isEmpty() && !model.hasPixelSelection(),
            "complete subtraction");
    model.setSelectionStartEnd(QPointF(5, 6), QPointF(6, 6));
    require(model.pixelSelection() == QRect(5, 6, 2, 1), "manual pointer cells stay inclusive");
    apply(model, Operation::Add, QRect(50, 50, 12, 13));
    require(model.selectionRegion().rectCount() == 2, "disconnected region");
    apply(model, Operation::Subtract, QRect(100, 100, 5, 5));
    require(model.selectionRegion().rectCount() == 2, "outside subtraction is a no-op");
    ScreenshotSelectionParams preset;
    preset.selection = QRect(20, 20, 40, 40);
    require(model.applyParams(preset, QRect(0, 0, 300, 300)) && model.rectangular(),
            "preset must replace complex selection");
    apply(model, Operation::Subtract, QRect(30, 0, 10, 100));
    require(!model.rectangular() && !model.selectionRegion().contains(QPoint(35, 35)),
            "subtraction must split a region");
}

void individualRegionSnapping() {
    using Operation = ScreenshotSelectionModel::RegionOperation;
    const QRect bounds(-200, -200, 600, 600);
    ScreenshotSelectionModel model;
    const QRect fixed(20, 30, 40, 40), moving(180, 130, 40, 40);
    model.setSelectionRect(fixed);
    apply(model, Operation::Add, moving);
    const auto id = model.regionAt({190, 140});
    model.beginMoveDrag({190, 140});
    const QRectF candidate(24, 112, 40, 40);
    require(model.snapIndividualRegionMove(candidate, bounds, 1, false) == candidate &&
                model.moveSnapGuides().isEmpty(),
            "unmodified drag never snaps");
    const auto snapped = model.snapIndividualRegionMove(candidate, bounds, 1, true);
    require(snapped.x() == 20 && !model.moveSnapGuides().isEmpty(), "nearby edges snap");
    model.setDraggedSelectionRect(snapped, ScreenshotSelectionDragMode::All);
    model.finishMoveDrag();
    require(model.regionAt(snapped.center()) == id && model.regionAt({30, 40}) != id &&
                model.moveSnapGuides().isEmpty(),
            "snap preserves identity and clears guides");
    model.beginMoveDrag(snapped.center());
    const auto origin = model.moveOriginalSelection();
    for (qreal scale : {0.5, 1.0, 1.5, 2.0}) {
        for (int x : {-100, 12, 13, 20, 24, 28, 29, 40, 100}) {
            const QRectF raw(x, 112, 40, 40);
            const auto engine = snowCanvasSnapMove(raw, {QRectF(fixed)}, scale);
            require(engine.has_value(), "reference snap succeeds");
            const QPointF expected(qRound(raw.x() + engine->correction.x()),
                                   qRound(raw.y() + engine->correction.y()));
            require(model.snapIndividualRegionMove(raw, bounds, scale, true).topLeft() == expected,
                    "region edges, centers and thresholds match rectangle movement at every scale");
        }
    }
    // Moving back near its old position cannot snap to itself.
    require(model.snapIndividualRegionMove(QRectF(181, 131, 40, 40), bounds, 1, true) ==
                QRectF(181, 131, 40, 40),
            "moving identity is excluded from snap references");
    model.rebaseMoveDrag(origin.center(), true);
    require(model.snapIndividualRegionMove(candidate, bounds, 1, true) == candidate &&
                model.moveSnapGuides().isEmpty(),
            "group drag does not snap");
    model.finishMoveDrag();

    model.setSelectionRect(QRect(20, 30, 40, 40));
    apply(model, Operation::Add, QRect(100, 30, 40, 40));
    apply(model, Operation::Add, QRect(240, 100, 40, 40));
    model.beginMoveDrag({250, 110});
    require(model.snapIndividualRegionMove(QRectF(178, 30, 40, 40), bounds, 1, true).x() == 180 &&
                std::any_of(model.moveSnapGuides().begin(), model.moveSnapGuides().end(),
                            [](const auto& guide) { return guide.gap; }),
            "equal spacing uses gap guides");
    model.finishMoveDrag();

    model.setSelectionRect(QRect(20, 20, 41, 40));
    apply(model, Operation::Add, QRect(140, 100, 20, 20));
    model.beginMoveDrag({145, 105});
    const auto odd = model.snapIndividualRegionMove(QRectF(31, 100, 20, 20), bounds, 1, true);
    require(odd.x() == 31 && odd.width() == 20 && model.moveSnapGuides().isEmpty(),
            "unrepresentable half-pixel center guides are suppressed without resizing");
    const auto mixed = model.snapIndividualRegionMove(QRectF(31, 22, 20, 20), bounds, 1, true);
    require(mixed == QRectF(31, 20, 20, 20) && !model.moveSnapGuides().isEmpty() &&
                std::all_of(model.moveSnapGuides().begin(), model.moveSnapGuides().end(),
                            [](const auto& guide) { return !guide.vertical && !guide.gap; }),
            "a fractional center on X must preserve exact top-edge alignment on Y");
    model.finishMoveDrag();
    model.setSelectionRect(QRect(28, 20, 40, 40));
    apply(model, Operation::Add, QRect(100, 100, 40, 40));
    model.beginMoveDrag({110, 110});
    const auto clamped =
        model.snapIndividualRegionMove(QRectF(24, 100, 40, 40), QRectF(0, 0, 64, 300), 1, true);
    require(clamped.x() == 24 && model.moveSnapGuides().isEmpty(),
            "capture bounds override snapping and erase misleading guides");

    for (bool curved : {false, true}) {
        model.setSelectionRect(QRect(20, 30, 40, 40));
        QPainterPath path;
        path.addEllipse(QRectF(180, 130, 40, 40));
        const ScreenshotRegionGeometry shape =
            curved ? ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Curve)
                   : ScreenshotRegionGeometry(QRegion(moving).subtracted(QRect(190, 140, 10, 10)));
        model.beginRegionOperation(Operation::Add);
        model.setDraftRegion(shape);
        model.commitDraftRegion(bounds);
        model.beginMoveDrag({185, 150});
        const QRectF result = model.snapIndividualRegionMove(candidate, bounds, 1, true);
        model.setDraggedSelectionRect(result, ScreenshotSelectionDragMode::All);
        require(model.highlightedRegion() ==
                    shape.translated(result.topLeft().toPoint() - moving.topLeft()),
                "snapping translates curves and cutouts without altering their geometry");
        model.clearSelection();
        require(model.moveSnapGuides().isEmpty(), "replacement retires snap state");
    }
}

void independentRegionsAndDuplication() {
    using Operation = ScreenshotSelectionModel::RegionOperation;
    ScreenshotSelectionModel model;
    const QRect bounds(-100, -100, 400, 300);
    const QRect first(10, 10, 40, 40), second(30, 10, 40, 40);
    model.setSelectionRect(first);
    model.beginRegionOperation(Operation::Add);
    require(model.multiRegionEditing() && model.createdRegionCount() == 1,
            "entering add seeds the original created region");
    model.cancelRegionOperation();
    require(!model.multiRegionEditing(), "cancel restores the preceding editing mode");
    apply(model, Operation::Add, second);
    require(model.rectangular() && !model.resizable() && model.createdRegionCount() == 2,
            "rectangular unions retain independent identity without combined resize handles");
    const auto older = model.regionAt({15, 15});
    const auto newer = model.regionAt({35, 15});
    require(older && newer && older != newer, "newest overlapping region must win hit testing");
    model.beginMoveDrag({35, 15});
    const auto moved =
        model.selectionRectForDrag(ScreenshotSelectionDragMode::All, {135, 65}, bounds, 1);
    model.setDraggedSelectionRect(moved, ScreenshotSelectionDragMode::All);
    require(model.selectionRegion() == QRegion(first).united(second.translated(100, 50)),
            "drag translates only the grabbed region");
    const auto revision = model.changeRevision();
    model.setDraggedSelectionRect(moved, ScreenshotSelectionDragMode::All);
    require(model.changeRevision() == revision, "unchanged drag pixels must do no semantic work");
    model.rebaseMoveDrag({135, 65});
    model.setDraggedSelectionRect(
        model.selectionRectForDrag(ScreenshotSelectionDragMode::All, {145, 65}, bounds, 1),
        ScreenshotSelectionDragMode::All);
    model.finishMoveDrag();
    require(model.regionAt({145, 65}) == newer && model.regionAt({15, 15}) == older,
            "rebasing retains identity and leaves the other region stationary");
    model.beginMoveDrag({15, 15});
    model.setDraggedSelectionRect(
        model.selectionRectForDrag(ScreenshotSelectionDragMode::All, {15, 25}, bounds, 1),
        ScreenshotSelectionDragMode::All);
    model.finishMoveDrag();
    require(model.duplicateLatestRegion({290, 190}, bounds), "duplicate newest after moving older");
    require(model.createdRegionCount() == 3 &&
                model.highlightedRegion() == ScreenshotRegionGeometry(QRect(260, 160, 40, 40)),
            "copy clamps its whole size to the capture corner");
    apply(model, Operation::Subtract, QRect(270, 160, 10, 40));
    require(!model.regionAt({275, 180}), "a cut is not a move target");
    const auto cut = QRegion(QRect(260, 160, 40, 40)).subtracted(QRect(270, 160, 10, 40));
    require(model.duplicateLatestRegion({0, 0}, bounds) &&
                model.highlightedRegion() == ScreenshotRegionGeometry(cut.translated(-280, -180)),
            "copies retain cuts and split remnants as one created region");
    const auto before = model.selectionRegion();
    const auto count = model.createdRegionCount();
    model.beginRegionOperation(Operation::Subtract);
    model.setSelectionRect(bounds);
    model.cancelRegionOperation();
    require(model.multiRegionEditing() && model.createdRegionCount() == count &&
                model.selectionRegion() == before,
            "cancel preserves every created identity");
    apply(model, Operation::Subtract, bounds);
    require(model.multiRegionEditing() && model.createdRegionCount() == 0 &&
                !model.duplicateLatestRegion({0, 0}, bounds),
            "empty multi-region editing must retain its no-op duplication context");
    model.setSelectionRect(first);
    require(!model.multiRegionEditing(), "explicit replacement ends multi-region editing");

    apply(model, Operation::Add, first);
    require(model.createdRegionCount() == 2, "identical additions retain creation identity");
    require(model.duplicateLatestRegion({30, 30}, bounds) && model.createdRegionCount() == 3,
            "an identical duplicate is still a new region");
    const auto copy = model;
    model.clearSelection();
    model = copy;
    require(model.createdRegionCount() == 3 && model.multiRegionEditing(),
            "model copies retain the editable collection");
    require(model.adjustFromToolbar(5, 7, 5, 7, bounds, 1) &&
                model.duplicateLatestRegion({30, 30}, bounds),
            "toolbar translation retains duplicate source identity");
    model.reset();
    require(!model.multiRegionEditing() && model.createdRegionCount() == 0,
            "capture reset releases all creation state");
}

void duplicateGeometryBoundariesAndCurves() {
    using Operation = ScreenshotSelectionModel::RegionOperation;
    const QRect bounds(-100, -60, 200, 120);
    for (const QSize size : {QSize(1, 1), QSize(31, 21), bounds.size()}) {
        for (int x : {-200, -100, -1, 0, 99, 200}) {
            for (int y : {-160, -60, -1, 0, 59, 160}) {
                ScreenshotSelectionModel model;
                model.setSelectionRect(QRect(bounds.topLeft(), size));
                model.beginRegionOperation(Operation::Add);
                require(model.duplicateLatestRegion({qreal(x), qreal(y)}, bounds),
                        "valid sizes duplicate at and beyond every capture edge");
                const QRect actual = model.highlightedRegion()->boundingRect();
                const QPoint expected =
                    bounds.topLeft() +
                    QPoint(std::clamp(qRound(x - bounds.x() - size.width() / 2.0), 0,
                                      bounds.width() - size.width()),
                           std::clamp(qRound(y - bounds.y() - size.height() / 2.0), 0,
                                      bounds.height() - size.height()));
                require(actual == QRect(expected, size) && bounds.contains(actual),
                        "duplicate preserves size with nearest centered integer translation and "
                        "clamping");
            }
        }
    }
    ScreenshotSelectionModel model;
    QPainterPath curve;
    curve.addEllipse(QRectF(0.25, 0.5, 30.5, 20.25));
    const auto original = ScreenshotRegionGeometry::fromPath(curve, ScreenshotRegionType::Curve);
    model.setSelectionRegion(original);
    model.beginRegionOperation(Operation::Add);
    require(model.duplicateLatestRegion({50, 30}, bounds), "custom curves can be duplicated");
    const auto copy = *model.highlightedRegion();
    const QPoint offset = copy.boundingRect().topLeft() - original.boundingRect().topLeft();
    require(copy == original.translated(offset) &&
                copy.path().elementCount() == curve.elementCount(),
            "duplication retains untouched vector control points");
    const auto state = model;
    require(!model.duplicateLatestRegion({0, 0}, {}) &&
                !model.duplicateLatestRegion({0, 0}, QRect(0, 0, 1, 1)) &&
                model.selectionRegion() == state.selectionRegion() &&
                model.createdRegionCount() == state.createdRegionCount() &&
                model.changeRevision() == state.changeRevision(),
            "invalid or too-small bounds leave creation state unchanged");
    model.beginMoveDrag(copy.boundingRect().center());
    const auto count = model.regionEditCompositionsForTesting();
    const auto revision = model.changeRevision();
    for (int i = 0; i < 1000; ++i)
        model.setDraggedSelectionRect(model.moveOriginalSelection(),
                                      ScreenshotSelectionDragMode::All);
    require(model.regionEditCompositionsForTesting() == count && model.changeRevision() == revision,
            "unchanged pointer bursts perform zero compositions and revisions");
    model.finishMoveDrag();
    const auto params = model.params(bounds);
    ScreenshotSelectionModel restored;
    require(restored.applyParams(params, bounds) && !restored.multiRegionEditing() &&
                restored.selectionRegion() == model.selectionRegion(),
            "history restores the same exported mask without session-local creation identities");
}

void containedMovesAndSurvivingCreationOrder() {
    using Operation = ScreenshotSelectionModel::RegionOperation;
    const QRect bounds(0, 0, 500, 400);
    ScreenshotSelectionModel model;
    const QRect base(20, 20, 200, 200), contained(50, 50, 31, 21);
    model.setSelectionRect(base);
    apply(model, Operation::Add, contained);
    const auto olderId = model.regionAt({25, 25});
    const auto containedId = model.regionAt({55, 55});
    ScreenshotSelectionModel before = model;
    model.beginMoveDrag({55, 55});
    model.setDraggedSelectionRect(
        model.selectionRectForDrag(ScreenshotSelectionDragMode::All, {115, 55}, bounds, 1),
        ScreenshotSelectionDragMode::All);
    require(model.selectionRegion() == before.selectionRegion() &&
                model.regionAt({115, 55}) == containedId && model.regionAt({55, 55}) == olderId,
            "contained drag changes the hit target while preserving the combined mask");
    const auto oldRevision = before.changeRevision();
    before = model;
    require(before.changeRevision() == oldRevision + 1 &&
                before.highlightedRegion() == model.highlightedRegion(),
            "copy assignment observes contained movement even when the mask is unchanged");
    model.finishMoveDrag();
    const auto settledRevision = before.changeRevision();
    before = model;
    require(before.changeRevision() == settledRevision,
            "committing the same preview does not introduce another semantic revision");
    model.beginMoveDrag({25, 25});
    model.setDraggedSelectionRect(
        model.selectionRectForDrag(ScreenshotSelectionDragMode::All, {25, 35}, bounds, 1),
        ScreenshotSelectionDragMode::All);
    model.finishMoveDrag();
    require(model.duplicateLatestRegion({350, 300}, bounds) &&
                model.highlightedRegion()->boundingRect().size() == contained.size(),
            "moving an older, larger region does not change the latest-created source");
    const QRect newest = model.highlightedRegion()->boundingRect();
    apply(model, Operation::Subtract, newest);
    require(model.createdRegionCount() == 2 && model.duplicateLatestRegion({350, 300}, bounds) &&
                model.highlightedRegion()->boundingRect().size() == contained.size(),
            "removing the latest region falls back to the newest surviving identity");
    const auto splitId = model.regionAt({350, 300});
    apply(model, Operation::Subtract, QRect(345, 280, 5, 40));
    const auto cut = model.selectionRegion();
    model.beginMoveDrag({340, 300});
    const auto original = *model.highlightedRegion();
    model.setDraggedSelectionRect(
        model.selectionRectForDrag(ScreenshotSelectionDragMode::All, {480, 390}, bounds, 1),
        ScreenshotSelectionDragMode::All);
    const auto moved = *model.highlightedRegion();
    model.finishMoveDrag();
    require(model.selectionRegion() == cut.subtracted(original).united(moved) &&
                moved == original.translated(moved.boundingRect().topLeft() -
                                             original.boundingRect().topLeft()) &&
                bounds.contains(moved.boundingRect()) &&
                model.regionAt(QPointF(moved.boundingRect().topLeft()) + QPointF(1, 1)) == splitId,
            "split remnants move together, retain their cut and identity, and clamp as one region");
}

void wholeSelectionMovesPreserveEditableRegions() {
    using Operation = ScreenshotSelectionModel::RegionOperation;
    const QRect bounds(-100, -60, 300, 240);
    for (const bool curves : {false, true}) {
        ScreenshotSelectionModel model;
        model.setSelectionRect(QRect(-80, -40, 40, 40));
        model.beginRegionOperation(Operation::Add);
        QPainterPath ellipse;
        ellipse.addEllipse(QRectF(10.25, 20.5, 50.5, 30.25));
        model.setDraftRegion(
            curves ? ScreenshotRegionGeometry::fromPath(ellipse, ScreenshotRegionType::Curve)
                   : ScreenshotRegionGeometry(QRect(10, 20, 51, 31)));
        model.commitRegionOperation();
        apply(model, Operation::Subtract, QRect(-65, -40, 10, 40));
        const QPointF firstPoint(-75, -30), latestPoint(35, 35), start(-20, 0);
        const auto firstId = model.regionAt(firstPoint);
        const auto latestId = model.regionAt(latestPoint);
        require(firstId && latestId && firstId != latestId, "group move fixture has two regions");
        static_cast<void>(model.setHoveredRegion(latestPoint));
        const auto latest = *model.highlightedRegion();
        const auto original = model.selectionRegion();
        model.beginMoveDrag(start, true);
        require(model.moveOriginalSelection() == QRectF(original.boundingRect()) &&
                    model.highlightedRegion() == original && !model.stationaryRegionsDuringMove(),
                "whole-selection drag uses the combined bounds and highlights every moving region");
        for (const QPointF endpoint : {QPointF(1000, 1000), QPointF(-1000, -1000)}) {
            const auto target =
                model.selectionRectForDrag(ScreenshotSelectionDragMode::All, endpoint, bounds, 1);
            const QPoint offset = target.topLeft().toPoint() - original.boundingRect().topLeft();
            model.setDraggedSelectionRect(target, ScreenshotSelectionDragMode::All);
            require(model.selectionRegion() == original.translated(offset) &&
                        bounds.contains(model.pixelSelection()) &&
                        model.regionAt(firstPoint + offset) == firstId &&
                        model.regionAt(latestPoint + offset) == latestId,
                    "group movement clamps the combined bounds and retains cuts, curves, and IDs");
            const auto revision = model.changeRevision();
            const auto compositions = model.regionEditCompositionsForTesting();
            model.setDraggedSelectionRect(target, ScreenshotSelectionDragMode::All);
            require(
                model.changeRevision() == revision &&
                    model.regionEditCompositionsForTesting() == compositions &&
                    !model.duplicateLatestRegion(latestPoint, bounds),
                "unchanged group moves do no geometry work and active moves reject duplication");
        }
        ScreenshotSelectionModel copied;
        copied = model;
        copied.setDraggedSelectionRect(QRectF(original.boundingRect()),
                                       ScreenshotSelectionDragMode::All);
        copied.finishMoveDrag();
        require(copied.selectionRegion() == original && copied.createdRegionCount() == 2 &&
                    copied.regionAt(firstPoint) == firstId &&
                    copied.regionAt(latestPoint) == latestId,
                "copying an active group drag retains the complete movement transaction");
        copied.beginMoveDrag(firstPoint);
        const auto first = *copied.highlightedRegion();
        copied.setDraggedSelectionRect(copied.selectionRectForDrag(ScreenshotSelectionDragMode::All,
                                                                   firstPoint + QPointF(5, 7),
                                                                   bounds, 1),
                                       ScreenshotSelectionDragMode::All);
        copied.finishMoveDrag();
        require(copied.selectionRegion() == latest.united(first.translated(5, 7)),
                "individual moves after a group move retain split remnants as one editable region");
        require(copied.duplicateLatestRegion({100, 100}, bounds) &&
                    copied.highlightedRegion() ==
                        latest.translated(copied.highlightedRegion()->boundingRect().topLeft() -
                                          latest.boundingRect().topLeft()),
                "group moves preserve the newest region's exact geometry for duplication");
        model.reset();
        model.setSelectionRect(QRect(0, 0, 20, 20));
        model.setDraggedSelectionRect(QRectF(5, 7, 20, 20), ScreenshotSelectionDragMode::All);
        require(!model.multiRegionEditing() && model.pixelSelection() == QRect(5, 7, 20, 20),
                "resetting an active group move releases its transient movement scope");
    }
}

void moveSnapshotEndsWithSelection() {
    const QRegion original = QRegion(QRect(10, 10, 20, 20)) + QRect(50, 10, 20, 20);
    for (const bool resetModel : {false, true}) {
        ScreenshotSelectionModel model;
        model.setSelectionRegion(original);
        model.beginMoveDrag(QPointF(10, 10));
        model.setDraggedSelectionRect(QRectF(15, 20, 60, 20), ScreenshotSelectionDragMode::All);
        require(model.selectionRegion() == original.translated(5, 10),
                "active move must retain its original complex geometry");

        if (resetModel)
            model.reset();
        else
            model.clearSelection();
        const QRect fresh(100, 100, 25, 25);
        model.setSelectionRect(fresh);
        model.setDraggedSelectionRect(QRectF(fresh), ScreenshotSelectionDragMode::All);
        require(model.selectionRegion() == QRegion(fresh),
                "ended selection must not reuse the previous move snapshot");
    }
}

void animatedMarqueeUsesDisplayedGeometry() {
    using Operation = ScreenshotSelectionModel::RegionOperation;
    const auto check = [](Operation operation, const QRect& confirmed, const QRect& first,
                          const QRect& target) {
        ScreenshotSelectionModel model;
        model.setSelectionRect(confirmed);
        model.beginRegionOperation(operation);
        require(model.selectionRegionForMarquee({}) == QRegion(confirmed),
                "empty animated marquee must preserve the confirmed region");
        model.setSelectionRect(first);
        ScreenshotRegionGeometry displayed;
        ScreenshotSmartSelectionTransition transition(
            [&](const QRectF& frame) { displayed = model.selectionRegionForMarquee(frame); });

        static_cast<void>(transition.update(QRectF(first), true, 0));
        const QRegion firstRegion = operation == Operation::Add
                                        ? QRegion(confirmed).united(first)
                                        : QRegion(confirmed).subtracted(first);
        require(displayed == firstRegion, "first region preview must be immediate");

        model.setSelectionRect(target);
        static_cast<void>(transition.update(QRectF(target), true, 0));
        const QRegion targetRegion = operation == Operation::Add
                                         ? QRegion(confirmed).united(target)
                                         : QRegion(confirmed).subtracted(target);
        require(transition.isRunning(), "changed region target must animate");
        require(displayed == firstRegion, "region preview must start at the previous frame");
        require(model.selectionRegion() == targetRegion,
                "animated region preview must not change the capture target");

        static_cast<void>(transition.advance(400));
        require(!transition.isRunning(), "region preview transition must finish");
        require(displayed == targetRegion, "region preview must finish at the target shape");
    };

    check(Operation::Add, QRect(10, 10, 40, 40), QRect(60, 10, 20, 30), QRect(110, 10, 30, 30));
    check(Operation::Subtract, QRect(0, 0, 100, 100), QRect(20, 0, 40, 100), QRect(60, 0, 40, 100));
}

#if defined(SNOW_SHOT_BENCH_INTERNALS)
bool sharesVectorStorage(const std::weak_ptr<const void>& left,
                         const std::weak_ptr<const void>& right) {
    return !left.expired() && !right.expired() && !left.owner_before(right) &&
           !right.owner_before(left);
}

void animatedMarqueeReusesVectorSnapshots() {
    using Operation = ScreenshotSelectionModel::RegionOperation;
    QPainterPath ellipse;
    ellipse.addEllipse(QRectF(0.25, 0.5, 160, 120));
    const QPoint translation(13, 17);
    const auto confirmed = ScreenshotRegionGeometry::fromPath(ellipse, ScreenshotRegionType::Curve)
                               .translated(translation);
    for (const auto operation : {Operation::Add, Operation::Subtract}) {
        const QRect target =
            operation == Operation::Add ? QRect(140, 60, 80, 40) : QRect(60, 50, 35, 30);
        const QRect frame = target.translated(2, 3);
        ScreenshotSelectionModel model;
        model.setSelectionRegion(confirmed);
        model.beginRegionOperation(operation);
        model.setSelectionRect(target);

        const auto displayed = model.selectionRegionForMarquee(QRectF(target));
        const auto capture = model.selectionRegion();
        require(sharesVectorStorage(displayed.storageLifetimeForTesting(),
                                    capture.storageLifetimeForTesting()),
                "display and capture must share the same composed vector snapshot");
        const auto expected =
            operation == Operation::Add ? confirmed.united(target) : confirmed.subtracted(target);
        require(capture == expected && capture.toJson() == expected.toJson(),
                "cached composition preserves the exact translated vector operands");

        const auto intermediate = model.selectionRegionForMarquee(QRectF(frame));
        require(sharesVectorStorage(displayed.storageLifetimeForTesting(),
                                    model.selectionRegion().storageLifetimeForTesting()) &&
                    sharesVectorStorage(
                        intermediate.storageLifetimeForTesting(),
                        model.selectionRegionForMarquee(QRectF(frame)).storageLifetimeForTesting()),
                "alternating target and displayed frame must retain both snapshots");
        require(model.pixelSelection() == target && model.confirmedRegion() == confirmed,
                "display frames must not mutate the capture operand or confirmed geometry");

        const QRectF subpixel(QPointF(target.topLeft()) + QPointF(0.2, 0.3),
                              QSizeF(target.size()) - QSizeF(0.4, 0.6));
        require(screenshotPixelRectForSelection(subpixel) == target,
                "subpixel fixture must resolve to the same outward-rounded pixels");
        model.setSelectionRect(subpixel);
        require(sharesVectorStorage(displayed.storageLifetimeForTesting(),
                                    model.selectionRegion().storageLifetimeForTesting()) &&
                    sharesVectorStorage(
                        displayed.storageLifetimeForTesting(),
                        model.selectionRegionForMarquee(subpixel).storageLifetimeForTesting()),
                "marquee changes within the same pixels must reuse the vector snapshot");

        const auto canonical = capture.path();
        for (const qreal scale : {2.0, 1.25, 3.0, 1.5, 1.0}) {
            const auto fresh =
                ScreenshotRegionGeometry::fromJson(expected.translated(-translation).toJson());
            require(model.selectionRegionForMarquee(subpixel).path(scale) ==
                        fresh->path(scale).translated(translation),
                    "cached previews preserve exact reconstruction at each render scale");
        }
        require(capture.path() == canonical && capture.toJson() == expected.toJson(),
                "display contour cache reuse must preserve canonical geometry and source cubics");
    }
}

void animatedMarqueeCacheRetiresSnapshots() {
    using Operation = ScreenshotSelectionModel::RegionOperation;
    QPainterPath ellipse;
    ellipse.addEllipse(QRectF(0.25, 0.5, 160, 120));
    const auto confirmed = ScreenshotRegionGeometry::fromPath(ellipse, ScreenshotRegionType::Curve);
    const QRect first(40, 30, 20, 30), second(50, 30, 20, 30), third(60, 30, 20, 30);
    ScreenshotSelectionModel model;
    model.setSelectionRegion(confirmed);
    model.beginRegionOperation(Operation::Subtract);
    model.setSelectionRect(first);
    const auto firstStorage = model.selectionRegion().storageLifetimeForTesting();
    const auto secondStorage =
        model.selectionRegionForMarquee(QRectF(second)).storageLifetimeForTesting();
    require(!firstStorage.expired() && !secondStorage.expired(),
            "target and displayed marquee must be retained without external owners");
    require(sharesVectorStorage(
                firstStorage,
                model.selectionRegionForMarquee(QRectF(first)).storageLifetimeForTesting()),
            "reading a cached marquee must retain its snapshot");
    const auto thirdStorage =
        model.selectionRegionForMarquee(QRectF(third)).storageLifetimeForTesting();
    require(!firstStorage.expired() && secondStorage.expired() && !thirdStorage.expired(),
            "a new display frame must retire the previous frame and preserve the capture target");
    require(model.selectionRegionForMarquee({}) == confirmed && !firstStorage.expired() &&
                !thirdStorage.expired(),
            "an empty marquee must preserve confirmed geometry and both useful cached frames");
    model.setSelectionRect(third);
    require(
        firstStorage.expired() &&
            sharesVectorStorage(thirdStorage, model.selectionRegion().storageLifetimeForTesting()),
        "a new target must reuse the latest displayed snapshot and retire the old target");
    std::weak_ptr<const void> latestFrame;
    for (int offset = 1; offset <= 8; ++offset) {
        const auto previousFrame = latestFrame;
        latestFrame = model.selectionRegionForMarquee(QRectF(third.translated(offset, offset)))
                          .storageLifetimeForTesting();
        require(previousFrame.expired() &&
                    sharesVectorStorage(thirdStorage,
                                        model.selectionRegion().storageLifetimeForTesting()),
                "many animation frames must keep the target pinned and retire previous frames");
    }
    model.reset();
    require(thirdStorage.expired() && latestFrame.expired(),
            "reset must release composed preview storage immediately");

    for (const int mutation : {0, 1, 2, 3, 4, 5}) {
        model.setSelectionRegion(confirmed);
        model.beginRegionOperation(Operation::Subtract);
        model.setSelectionRect(first);
        const auto storage = model.selectionRegion().storageLifetimeForTesting();
        require(!storage.expired(), "capture composition must remain cached during the operation");
        switch (mutation) {
        case 0:
            model.clearSelection();
            break;
        case 1:
            model.cancelRegionOperation();
            require(model.selectionRegion() == confirmed, "cancel restores the confirmed contour");
            break;
        case 2:
            model.beginRegionOperation(Operation::Add);
            require(model.selectionRegionForMarquee(QRectF(first)) == confirmed.united(first),
                    "switching operation must discard the old subtract composition");
            break;
        case 3: {
            const auto replacement = confirmed.subtracted(QRect(80, 40, 20, 20));
            model.setSelectionRegion(replacement);
            model.beginRegionOperation(Operation::Subtract);
            require(model.selectionRegionForMarquee(QRectF(first)) == replacement.subtracted(first),
                    "confirmed contour replacement must invalidate identical marquee pixels");
            break;
        }
        case 4:
            model.setDraftRegion(confirmed);
            require(model.selectionRegionForMarquee(QRectF(second)) == model.selectionRegion() &&
                        model.selectionRegion().isEmpty(),
                    "construction uses its exact vector operand instead of the animated marquee");
            break;
        case 5:
            model.commitRegionOperation();
            require(!model.regionOperationActive() &&
                        model.selectionRegion() == confirmed.subtracted(first),
                    "commit preserves the composed target as the confirmed selection");
            // The committed result owns its snapshot until that selection is replaced.
            model.clearSelection();
            break;
        }
        require(storage.expired(), "ending or replacing a transaction must retire its cache");
    }
}

void settledMarqueeSurvivesTargetPreparation() {
    using Operation = ScreenshotSelectionModel::RegionOperation;
    QPainterPath ellipse;
    ellipse.addEllipse(QRectF(0.25, 0.5, 160, 120));
    const auto confirmed = ScreenshotRegionGeometry::fromPath(ellipse, ScreenshotRegionType::Curve);
    for (const auto operation : {Operation::Add, Operation::Subtract}) {
        const QRect settled =
            operation == Operation::Add ? QRect(120, 40, 70, 30) : QRect(40, 30, 20, 30);
        const QRect target = settled.translated(10, 10);
        const QRect intermediate = settled.translated(2, 3);
        ScreenshotSelectionModel model;
        model.setSelectionRegion(confirmed);
        model.beginRegionOperation(operation);
        model.setSelectionRect(settled);
        const auto settledStorage = model.selectionRegion().storageLifetimeForTesting();
        const auto previousFrame =
            model.selectionRegionForMarquee(QRectF(intermediate)).storageLifetimeForTesting();
        require(
            sharesVectorStorage(
                settledStorage,
                model.selectionRegionForMarquee(QRectF(settled)).storageLifetimeForTesting()) &&
                previousFrame.expired(),
            "a settled target must become the latest displayed frame and retire its predecessor");

        model.setSelectionRect(target);
        const auto targetStorage = model.selectionRegion().storageLifetimeForTesting();
        require(
            !settledStorage.expired() &&
                sharesVectorStorage(
                    settledStorage,
                    model.selectionRegionForMarquee(QRectF(settled)).storageLifetimeForTesting()),
            "preparing a new target must preserve and reuse the last settled display snapshot");
        const auto nextFrame =
            model.selectionRegionForMarquee(QRectF(intermediate)).storageLifetimeForTesting();
        require(
            settledStorage.expired() && !nextFrame.expired() &&
                sharesVectorStorage(targetStorage,
                                    model.selectionRegion().storageLifetimeForTesting()),
            "advancing the display must retire its old target while keeping the new target pinned");
        model.reset();
        require(nextFrame.expired() && targetStorage.expired(),
                "reset must retire both the pending target and its latest displayed frame");
    }
}
#endif

void outlinesAndEffects() {
    const QRegion shape = QRegion(QRect(0, 0, 100, 80)).subtracted(QRect(30, 20, 40, 40));
    const auto path = screenshotRegionPath(shape);
    require(path.contains(QPointF(10, 10)) && !path.contains(QPointF(50, 40)), "outline holes");
    QPainterPathStroker stroker;
    stroker.setWidth(1);
    require(!stroker.createStroke(path).contains(QPointF(10, 20)), "no scanline rectangle seams");
    const auto rounded = screenshotRegionPath(shape, 8);
    require(!rounded.contains(QPointF(0.5, 0.5)) && rounded.contains(QPointF(10, 10)),
            "rounded outer corners");
    require(!rounded.contains(QPointF(50, 40)), "rounding must preserve hole");
    QImage content(100, 80, QImage::Format_ARGB32_Premultiplied);
    content.fill(Qt::red);
    ScreenshotResultStyle style;
    style.region = shape;
    const auto output = ScreenshotResultCompositor::compose(content, style);
    require(output.pixelColor(50, 40).alpha() == 0, "cutout must not export screenshot pixels");
    require(output.pixelColor(10, 10) == QColor(Qt::red), "selected pixels unchanged");
    style.cornerRadius = 8;
    style.shadowWidth = 6;
    const auto effected = ScreenshotResultCompositor::compose(content, style);
    require(effected.size() == QSize(112, 92), "shadow padding");
    require(effected.pixelColor(56, 46).alpha() == 0, "hole center remains transparent");
    require(effected.pixelColor(37, 46).alpha() > 0, "shadow follows internal cutout edge");
    const QRegion separated = QRegion(QRect(0, 0, 32, 32)) + QRegion(QRect(160, 0, 32, 32));
    const auto separatedOutline = screenshotRegionPath(separated, 8);
    require(!separatedOutline.contains(QPointF(0.5, 0.5)) &&
                separatedOutline.contains(QPointF(16, 16)) &&
                separatedOutline.contains(QPointF(176, 16)) &&
                !separatedOutline.contains(QPointF(96, 16)),
            "isolated rounded regions preserve their separate contours");
    QImage separatedContent(192, 32, QImage::Format_ARGB32_Premultiplied);
    separatedContent.fill(Qt::red);
    ScreenshotResultStyle separatedStyle;
    separatedStyle.region = separated;
    separatedStyle.cornerRadius = 8;
    separatedStyle.shadowWidth = 4;
    const QImage separatedOutput =
        ScreenshotResultCompositor::compose(separatedContent, separatedStyle);
    require(separatedOutput.pixelColor(20, 20).alpha() == 255 &&
                separatedOutput.pixelColor(180, 20).alpha() == 255 &&
                separatedOutput.pixelColor(100, 20).alpha() == 0 &&
                separatedOutput.pixelColor(37, 20).alpha() > 0,
            "sparse export retains both regions and their local shadows");
    const auto repeated = ScreenshotResultCompositor::compose(content, style);
    require(repeated == effected, "cached effects must match initial composition");
    style.regionScale = 2;
    style.cornerRadius *= 2;
    style.shadowWidth *= 2;
    const auto scaled = ScreenshotResultCompositor::compose(content.scaled(200, 160), style);
    require(scaled.pixelColor(112, 92).alpha() == 0 && scaled.pixelColor(32, 32).red() == 255,
            "geometry must scale with backing resolution");
    const QRegion thin = QRegion(QRect(0, 0, 100, 2)).united(QRect(49, 0, 2, 80));
    const auto narrowPath = screenshotRegionPath(thin, 256);
    require(narrowPath.contains(QPointF(50, 40)), "large radius must preserve narrow bridge");
}
void persistenceAndHandlePolicy() {
    using namespace snow_shot::storage;
    PersistedSelection saved;
    saved.rectangle = QRect(0, 0, 100, 100);
    saved.shadowColor = Qt::black;
    saved.region = QRegion(saved.rectangle).subtracted(QRect(20, 20, 40, 40));
    const auto decoded = normalizePersistedSelection(persistedSelectionToJson(saved));
    require(decoded.valid && decoded.value == saved, "region codec round trip");
    auto legacy = persistedSelectionToJson(saved);
    legacy.remove(QStringLiteral("regions"));
    require(normalizePersistedSelection(legacy).valid &&
                !normalizePersistedSelection(legacy).value.region,
            "legacy rectangle compatibility");
    legacy.insert(QStringLiteral("regions"), QJsonArray{QStringLiteral("bad")});
    require(!normalizePersistedSelection(legacy).valid,
            "bad regions must not expose bounding rectangle");
    auto extreme = persistedSelectionToJson(saved);
    auto rectangle = [](int x) {
        return QJsonObject{{QStringLiteral("x"), x},
                           {QStringLiteral("y"), 0},
                           {QStringLiteral("width"), 100},
                           {QStringLiteral("height"), 100}};
    };
    extreme.insert(QStringLiteral("regions"),
                   QJsonArray{rectangle(-2000000000), rectangle(2000000000)});
    require(!normalizePersistedSelection(extreme).valid,
            "region spans must not overflow integer bounds");
    ScreenshotSelectionModel model;
    ScreenshotSelectionParams params;
    params.selection = saved.rectangle;
    params.region = saved.region;
    require(model.applyParams(params, QRect(0, 0, 50, 50)), "partial restore");
    require(model.selectionRegion() == saved.region->intersected(QRect(0, 0, 50, 50)),
            "clip restore");
    require(!model.applyParams(params, QRect(200, 200, 10, 10)) && !model.hasPixelSelection(),
            "fully unavailable restored shape clears selection");
    ScreenshotInteractionState interaction;
    interaction.beginCapture();
    require(!interaction.selectionHandlesVisible(), "manual entry has no handles");
    require(interaction.enterSelectionDrag(ScreenshotSelectionDragMode::Marquee), "start marquee");
    require(!interaction.selectionHandlesVisible(), "marquee has no handles");
    require(interaction.enterSelectionDrag(ScreenshotSelectionDragMode::All), "move marquee");
    require(!interaction.selectionHandlesVisible(), "temporary move has no handles");
    interaction.finishDrag();
    interaction.confirmSelection();
    require(interaction.selectionHandlesVisible(), "confirmed rectangle handles return");
}

void customGeometryTransactionsAndPersistence() {
    const QVector<QPointF> vertices{{10.25, 10.5}, {110.75, 20.25}, {65.5, 100.75}, {20.25, 70.5}};
    QPainterPath polygon;
    polygon.addPolygon(QPolygonF(vertices));
    polygon.closeSubpath();
    const auto polyline =
        ScreenshotRegionGeometry::fromPath(polygon, ScreenshotRegionType::Polyline);
    const auto curvePath = snowCanvasCatmullRomPath(vertices, true);
    const auto curve = ScreenshotRegionGeometry::fromPath(curvePath, ScreenshotRegionType::Curve);
    require(curve.path() == curvePath &&
                curve.path().elementAt(1).type == QPainterPath::CurveToElement,
            "curve regions must retain engine cubics exactly before Boolean operations");
    require(curve.custom() && curve.rectCount() > 1,
            "custom shapes must not acquire rectangle handles");
    const auto hole = curve.subtracted(QRect(40, 35, 20, 20));
    require(!hole.contains(QPointF(50, 45)) && hole.contains(QPointF(65, 65)),
            "curve subtraction preserves holes");
    const auto expanded = hole.united(QRect(150, 20, 20, 20));
    require(expanded.contains(QPointF(155, 25)) && !expanded.contains(QPointF(130, 25)),
            "disconnected vector components");
    require(expanded.translated(9, 13).contains(QPointF(164, 38)),
            "translate every vector operand");
    const auto decoded = ScreenshotRegionGeometry::fromJson(expanded.toJson());
    require(decoded && *decoded == expanded && decoded->path(2.0) == expanded.path(2.0),
            "vector operand codec round trip at another scale");
    auto corrupt = expanded.toJson();
    auto operands = corrupt.value(QStringLiteral("operands")).toArray();
    auto first = operands.first().toObject();
    first.insert(QStringLiteral("commands"), QJsonArray{QJsonArray{2, 4, 8}});
    operands[0] = first;
    corrupt.insert(QStringLiteral("operands"), operands);
    require(!ScreenshotRegionGeometry::fromJson(corrupt), "reject orphan cubic control points");
    corrupt = expanded.toJson();
    corrupt.insert(QStringLiteral("version"), 999);
    require(!ScreenshotRegionGeometry::fromJson(corrupt), "reject unsupported geometry versions");

    ScreenshotSelectionModel model;
    model.setSelectionRect(QRect(0, 0, 200, 200));
    static_cast<void>(model.setCornerRadius(20));
    model.beginRegionOperation(ScreenshotSelectionModel::RegionOperation::Add);
    model.setDraftRegion(polyline);
    model.commitDraftRegion();
    require(model.cornerRadiusApplicable(),
            "contained custom addition must not disable rectangle rounding");
    model.beginRegionOperation(ScreenshotSelectionModel::RegionOperation::Subtract);
    model.setDraftRegion(polyline);
    require(!model.cornerRadiusApplicable(), "custom contribution suppresses rounding in preview");
    model.cancelRegionOperation();
    require(model.cornerRadiusApplicable() && model.cornerRadius() == 20,
            "cancel restores radius eligibility and value");
    model.beginRegionOperation(ScreenshotSelectionModel::RegionOperation::Subtract);
    model.setDraftRegion(polyline);
    model.commitDraftRegion();
    require(!model.cornerRadiusApplicable() && model.cornerRadius() == 20,
            "commit retains radius preference without applying it");
    const auto params = model.params(QRect(0, 0, 300, 300));
    ScreenshotSelectionModel restored;
    require(restored.applyParams(params, QRect(0, 0, 300, 300)) &&
                restored.selectionRegion() == model.selectionRegion(),
            "custom selection model restoration");
    require(ScreenshotResultCompositor::normalizedStyle(model.resultStyle()).cornerRadius == 0,
            "compositor centrally suppresses custom rounding");
    model.setSelectionRect(QRect(0, 0, 200, 200));
    require(model.cornerRadiusApplicable(), "replacement rectangle restores radius eligibility");

    snow_shot::storage::PersistedSelection persisted;
    persisted.shadowColor = Qt::black;
    persisted.rectangle = expanded.boundingRect();
    persisted.region = expanded;
    const auto normalization = snow_shot::storage::normalizePersistedSelection(
        snow_shot::storage::persistedSelectionToJson(persisted));
    require(normalization.valid && normalization.value == persisted,
            "history/previous selection vector codec");
    QImage content(200, 150, QImage::Format_ARGB32_Premultiplied);
    content.fill(Qt::red);
    ScreenshotResultStyle style;
    style.region = expanded;
    style.cornerRadius = 40;
    const auto raster = ScreenshotResultCompositor::compose(content, style);
    require(raster.pixelColor(50, 45).alpha() == 0 && raster.pixelColor(155, 25).alpha() == 255,
            "export uses holes and components");
    bool antialiased = false;
    for (int y = 0; y < raster.height(); ++y)
        for (int x = 0; x < raster.width(); ++x) {
            const int alpha = raster.pixelColor(x, y).alpha();
            antialiased |= alpha > 0 && alpha < 255;
        }
    require(antialiased, "custom contour edges must be antialiased");
    QPainterPath sparseCurves;
    sparseCurves.addEllipse(QRectF(10, 10, 30, 30));
    sparseCurves.addEllipse(QRectF(160, 10, 30, 30));
    QPainterPath wholeBounds = sparseCurves;
    wholeBounds.moveTo(35, 25);
    wholeBounds.lineTo(165, 25);
    ScreenshotResultStyle sparseCurveStyle;
    sparseCurveStyle.shadowWidth = 8;
    sparseCurveStyle.region =
        ScreenshotRegionGeometry::fromPath(sparseCurves, ScreenshotRegionType::Curve);
    QImage sparseContent(200, 50, QImage::Format_ARGB32_Premultiplied);
    sparseContent.fill(Qt::blue);
    const QImage sparseResult =
        ScreenshotResultCompositor::compose(sparseContent, sparseCurveStyle);
    sparseCurveStyle.region =
        ScreenshotRegionGeometry::fromPath(wholeBounds, ScreenshotRegionType::Curve);
    const QImage wholeResult = ScreenshotResultCompositor::compose(sparseContent, sparseCurveStyle);
    require(sparseResult == wholeResult,
            "tiled sparse export must match whole-image mask and shadow pixels");
    const auto repeated = snowCanvasCatmullRomPath({{10, 10}, {10, 10}, {50, 10}, {30, 50}}, true);
    require(!repeated.isEmpty(), "curve bridge supports degenerate neighbor fallback");
}
void translatedGeometryRetainsContoursAndPersistence() {
    QPainterPath ellipse;
    ellipse.addEllipse(QRectF(0.25, 0.5, 160, 120));
    auto geometry = ScreenshotRegionGeometry::fromPath(ellipse, ScreenshotRegionType::Curve)
                        .subtracted(QRect(60, 30, 25, 80));
    const auto original = geometry.path();
    const auto moved = geometry.translated(123, -47);
    require(moved.boundingRect() == geometry.boundingRect().translated(123, -47),
            "translated geometry retains exact bounds");
    require(moved.contains(QPointF(143, -7)) == geometry.contains(QPointF(20, 40)),
            "translated hit testing uses local geometry");
    const auto restored = ScreenshotRegionGeometry::fromJson(moved.toJson());
    require(restored && *restored == moved, "translated operands round trip in canvas coordinates");
    for (const qreal scale : {1.0, 1.25, 2.0, 1.5, 1.0, 2.0, 3.0, 1.0}) {
        const auto fresh = ScreenshotRegionGeometry::fromJson(geometry.toJson());
        require(moved.path(scale) == fresh->path(scale).translated(123, -47),
                "scaled contours are independent of previous scale requests and translations");
    }
    require(geometry.path() == original, "render scale changes preserve the canonical contour");
    const auto localCut = geometry.subtracted(QRect(10, 10, 20, 20)).translated(123, -47);
    const auto movedCut = moved.subtracted(QRect(133, -37, 20, 20));
    require(localCut == movedCut, "editing a translated snapshot retains operand coordinates");
    require(geometry.path() == original && moved.translated(-123, 47) == geometry,
            "editing and translation never mutate shared snapshots");
}

void regionMaskMatchesPainterCoverage() {
    QPainterPath path;
    path.addEllipse(QRectF(1.25, 2.75, 63.5, 48.25));
    path.addEllipse(QRectF(16.5, 15.75, 20.25, 20.5));
    path.setFillRule(Qt::OddEvenFill);
    QImage content(70, 60, QImage::Format_ARGB32_Premultiplied);
    content.fill(QColor(170, 50, 200, 180));
    ScreenshotResultStyle style;
    style.region = ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Curve);
    QImage reference = content;
    QImage mask(content.size(), QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    {
        QPainter painter(&mask);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillPath(path, Qt::white);
    }
    {
        QPainter painter(&reference);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.drawImage(QPoint(), mask);
    }
    require(ScreenshotResultCompositor::compose(content, style) == reference,
            "region mask preserves painter coverage and translucent content exactly");
    // A color or shadow-width change must not reuse the preceding effect.
    style.shadowWidth = 7;
    style.shadowColor = Qt::red;
    const auto red = ScreenshotResultCompositor::compose(content, style);
    style.shadowColor = Qt::blue;
    const auto blue = ScreenshotResultCompositor::compose(content, style);
    require(red != blue, "shadow color changes invalidate the effect independently of the mask");
    style.shadowWidth = 0;
    require(ScreenshotResultCompositor::compose(content, style) == reference,
            "disabling a cached shadow restores the original masked content");
}

void sessionCacheCleanupPreservesResults() {
    QPainterPath ellipse;
    ellipse.addEllipse(QRectF(0, 0, 100, 80));
    const auto geometry = ScreenshotRegionGeometry::fromPath(ellipse, ScreenshotRegionType::Curve)
                              .subtracted(QRect(30, 20, 20, 30));
    const auto snapshot = geometry.translated(2, 3);
    geometry.clearDerivedCache();
    const auto coldBytes = geometry.retainedBytesEstimate();
    const auto expected = snapshot.path(1.5);
    require(geometry.retainedBytesEstimate() > coldBytes, "fixture must retain a derived contour");
    snapshot.clearDerivedCache();
    require(geometry.retainedBytesEstimate() == coldBytes,
            "session cleanup releases derived contours shared with retained snapshots");
    require(snapshot.path(1.5) == expected, "cleaned geometry rebuilds the same contour");

    QImage content(100, 80, QImage::Format_ARGB32_Premultiplied);
    content.fill(Qt::white);
    ScreenshotResultStyle style;
    style.region = QRegion(content.rect()).subtracted(QRect(30, 20, 20, 30));
    style.cornerRadius = 5;
    style.shadowWidth = 7;
    const auto first = ScreenshotResultCompositor::compose(content, style);
    const auto warm = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(warm.regionCacheRetainedBytes > 0 && warm.regionPathCacheElements > 0,
            "fixture must populate both region composition and contour caches");
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
    const auto cleared = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(cleared.retainedBytes == 0 && cleared.retainedEntries == 0 &&
                cleared.regionCacheRetainedBytes == 0 && cleared.regionPathCacheElements == 0,
            "session cleanup is idempotent and releases every thread-local rendering cache");
    require(ScreenshotResultCompositor::compose(content, style) == first,
            "a subsequent session rebuilds identical export pixels");
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
}

void tiledShadowsMatchIndependentLocalCompositions(bool sparse) {
    QImage content(sparse ? 5000 : 1200, 1000, QImage::Format_ARGB32_Premultiplied);
    content.fill(QColor(70, 140, 210, 180));
    QPainterPath path;
    path.addEllipse(QRectF(1.25, 2.5, sparse ? 1095.5 : 1195.5, 990.25));
    if (sparse)
        path.addEllipse(QRectF(3900.25, 2.5, 1095.5, 990.25));
    path.addRect(QRectF(490.25, 490.5, 550, 200));
    path.setFillRule(Qt::OddEvenFill);
    for (const int width : {1, 7, 32}) {
        ScreenshotResultStyle style;
        style.region = ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Curve);
        style.shadowWidth = width;
        style.shadowColor = QColor(35, 20, 90, 190);
        const auto tiled = ScreenshotResultCompositor::compose(content, style, 1, 0.7);
        for (const int x : {16, 512, 1024, sparse ? 4412 : 1190}) {
            for (const int y : {16, 512, 990}) {
                const QPoint origin(x - width - 64, y - width - 64);
                auto localStyle = style;
                localStyle.region = ScreenshotRegionGeometry::fromPath(path.translated(-origin),
                                                                       ScreenshotRegionType::Curve);
                const auto local = ScreenshotResultCompositor::compose(
                    content.copy(QRect(origin, QSize(128, 128))), localStyle, 1, 0.7);
                for (int dy = -8; dy <= 8; ++dy)
                    for (int dx = -8; dx <= 8; ++dx) {
                        const auto a = tiled.pixelColor(x + dx, y + dy);
                        const auto b = local.pixelColor(64 + width + dx, 64 + width + dy);
                        // Compare premultiplied channels: unpremultiplication of
                        // a one-level alpha difference magnifies low-alpha RGB.
                        const auto pa = qPremultiply(a.rgba()), pb = qPremultiply(b.rgba());
                        require(
                            std::abs(qRed(pa) - qRed(pb)) <= 1 &&
                                std::abs(qGreen(pa) - qGreen(pb)) <= 1 &&
                                std::abs(qBlue(pa) - qBlue(pb)) <= 1 &&
                                std::abs(qAlpha(pa) - qAlpha(pb)) <= 1,
                            "tiled export matches independent full-support compositions at seams");
                    }
            }
        }
    }
}

void shapeCodecAndSamplingBoundaries() {
    QVector<QPointF> samples;
    for (int i = 0; i <= 1000; ++i)
        samples.append(QPointF(i * 0.1, std::sin(i * 0.01) * 20));
    const auto reduced = simplifyScreenshotRegionPoints(samples, 0.25);
    require(reduced.size() < samples.size() / 10 && reduced.first() == samples.first() &&
                reduced.last() == samples.last(),
            "Freehand removes redundant samples and retains endpoints");
    for (const auto& point : samples) {
        qreal distance = 1e9;
        for (int i = 1; i < reduced.size(); ++i) {
            const auto delta = reduced[i] - reduced[i - 1];
            const auto offset = point - reduced[i - 1];
            const auto length = QPointF::dotProduct(delta, delta);
            const auto t = std::clamp(QPointF::dotProduct(offset, delta) / length, 0.0, 1.0);
            distance = std::min(distance, QLineF(point, reduced[i - 1] + t * delta).length());
        }
        require(distance <= 0.250001, "Freehand simplification obeys subpixel error bound");
    }
    QPainterPath crossing;
    crossing.addPolygon(QPolygonF{{0, 0}, {100, 100}, {0, 100}, {100, 0}});
    crossing.closeSubpath();
    const auto region =
        ScreenshotRegionGeometry::fromPath(crossing, ScreenshotRegionType::Polyline);
    require(region.contains({50, 10}) && !region.contains({10, 50}),
            "self-intersections use odd-even filling");
    ScreenshotSelectionModel model;
    model.setDraftRegion(region);
    model.commitDraftRegion(QRect(25, 0, 50, 100));
    require(model.selectionRegion().boundingRect() == QRect(25, 0, 50, 100) &&
                !model.selectionRegion().contains({15, 5}),
            "commit clips to the available canvas");
    ScreenshotResultStyle style{20, 8, Qt::black};
    style.region = model.selectionRegion();
    style.regionScale = 1.75;
    const auto encoded = encodeScreenshotResultStyle(style);
    const auto decoded = decodeScreenshotResultStyle(encoded);
    require(decoded && decoded->region == style.region && decoded->regionScale == 1.75,
            "pinned style codec retains vector geometry and scale");
    require(!decodeScreenshotResultStyle(encoded.chopped(5)),
            "truncated pinned geometry rejects the record");
    QByteArray legacy;
    QDataStream legacyStream(&legacy, QIODevice::WriteOnly);
    legacyStream << style.cornerRadius << style.shadowWidth << style.shadowColor;
    const auto oldStyle = decodeScreenshotResultStyle(legacy);
    require(oldStyle && !oldStyle->region && oldStyle->cornerRadius == 20,
            "legacy pinned result styles remain readable");
    style.region.reset();
    require(decodeScreenshotResultStyle(encodeScreenshotResultStyle(style)).has_value(),
            "rectangle style extensions round trip without geometry");
}
} // namespace
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    sessionCacheCleanupPreservesResults();
    tiledShadowsMatchIndependentLocalCompositions(false);
    tiledShadowsMatchIndependentLocalCompositions(true);
    translatedGeometryRetainsContoursAndPersistence();
    regionMaskMatchesPainterCoverage();
    shapeCodecAndSamplingBoundaries();
    customGeometryTransactionsAndPersistence();
    geometryAndTransactions();
    individualRegionSnapping();
    independentRegionsAndDuplication();
    duplicateGeometryBoundariesAndCurves();
    containedMovesAndSurvivingCreationOrder();
    wholeSelectionMovesPreserveEditableRegions();
    moveSnapshotEndsWithSelection();
    animatedMarqueeUsesDisplayedGeometry();
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    animatedMarqueeReusesVectorSnapshots();
    animatedMarqueeCacheRetiresSnapshots();
    settledMarqueeSurvivesTargetPreparation();
#endif
    outlinesAndEffects();
    persistenceAndHandlePolicy();
    std::cout << "Multi-region selection tests passed\n";
}
