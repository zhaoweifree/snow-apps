#include "screenshot_selection_presentation_fixture.h"
#include "snow_shot/presentation/screenshotselectioneditworkflow.h"
#include "snow_shot/presentation/screenshotselectionsettingsstore.h"
#include "widgets/select.h"

#include "snow_shot/compat/qt_compat.h"
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QPainterPath>

#include <cstdlib>
#include <iostream>

namespace {
using selection_presentation_test::Fixture;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct QuickSetFixture {
    Fixture presentation{QSize(1200, 800), false};
    ScreenshotSelectionSettingsStore settings;
    std::unique_ptr<ScreenshotSelectionEditWorkflow> workflow;
    ScreenshotApplySelectionCallback applyModal;
    int applications = 0;
    int ratioApplications = 0;
    int effectWrites = 0;

    QuickSetFixture() {
        settings.clear();
        presentation.interaction.confirmSelection();
        presentation.captureState.sessionState = ScreenshotSessionState::Editing;
        ScreenshotSelectionEditUiActions ui;
        ui.updateOverlayState = [this] { presentation.services->updateOverlayState(); };
        ui.showToolbar = [this] { presentation.services->showToolbar(); };
        ui.openResizeModal = [this](QObject*, const ScreenshotSelectionResizeRequest&,
                                    ScreenshotApplySelectionCallback apply) {
            applyModal = std::move(apply);
            return true;
        };
        workflow = std::make_unique<ScreenshotSelectionEditWorkflow>(
            ScreenshotSelectionEditWorkflowContext{
                presentation.overlay, presentation.captureState, presentation.displays,
                presentation.geometry, presentation.interaction, presentation.selection, ui,
                [this](int radius, int shadow, const QColor& color) {
                    ++effectWrites;
                    settings.setSelectionEffects(radius, shadow, color);
                }});
        presentation.commands.readSelectionQuickSets = [this] {
            ScreenshotSelectionToolbarQuickSets quickSets;
            quickSets.previous = settings.previousSelectionParams(bounds());
            quickSets.presets = sanitizeScreenshotSelectionPresets(settings.presets(), bounds());
            return quickSets;
        };
        presentation.commands.applySelectionParams =
            [this](const ScreenshotSelectionParams& params) {
                ++applications;
                workflow->applySelectionParamsFromToolbar(params);
            };
        presentation.commands.setSelectionAspectRatioPreset =
            [this](ScreenshotSelectionAspectRatioPreset preset) {
                ++ratioApplications;
                workflow->setSelectionAspectRatioPresetFromToolbar(preset);
            };
        presentation.services->updateOverlayState();
        presentation.services->showSelectionToolbar();
        flush();
    }

    QRect bounds() const {
        return ScreenshotHalfOpenRect::fromRectF(presentation.geometry.canvasBounds())
            .toAlignedQRect();
    }

    void flush() {
        presentation.flushFrame();
        presentation.processEvents();
    }

    adqt::widgets::AdSelect* select() const {
        auto* toolbar = presentation.coordinator.selectionToolbar();
        require(toolbar != nullptr, "selection toolbar must exist");
        auto* select = toolbar->findChild<adqt::widgets::AdSelect*>(
            QStringLiteral("screenshotSelectionAspectRatioSelect"));
        require(select != nullptr, "selection quick-set dropdown must exist");
        return select;
    }

    QModelIndex option(const QString& id) const {
        auto* dropdown = select();
        auto* model = dropdown->view()->model();
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex index = model->index(row, 0);
            if (index.data(dropdown->valueRole()) == id)
                return index;
        }
        return {};
    }

    void choose(const QString& id, bool keyboard) {
        auto* dropdown = select();
        dropdown->showPopup();
        flush();
        require(dropdown->popupVisible(), "selection presets must remain openable");
        const QModelIndex index = option(id);
        require(index.isValid() && index.flags().testFlag(Qt::ItemIsEnabled),
                "requested selection preset must be enabled");
        auto* view = dropdown->view();
        view->setCurrentIndex(index);
        if (keyboard) {
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(view, &enter);
        } else {
            view->scrollTo(index);
            const QPointF position(view->visualRect(index).center());
            const QPointF globalPosition(view->viewport()->mapToGlobal(position.toPoint()));
            QMouseEvent press(QEvent::MouseButtonPress, position, position, globalPosition,
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view->viewport(), &press);
            QMouseEvent release(QEvent::MouseButtonRelease, position, position, globalPosition,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(view->viewport(), &release);
        }
        flush();
        require(!dropdown->popupVisible(), "confirming a preset must dismiss the dropdown");
        auto* toolbar = presentation.coordinator.selectionToolbar();
        require(toolbar->contentSizeHint().width() == dropdown->parentWidget()->sizeHint().width(),
                "selection toolbar must fit its final content after applying a preset");
    }
};

ScreenshotSelectionParams shapedParams(bool custom, const QRect& rectangle) {
    ScreenshotSelectionParams params;
    params.selection = rectangle;
    if (custom) {
        QPainterPath path;
        path.addEllipse(QRectF(rectangle));
        params.region = ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Curve);
    } else {
        params.region = QRegion(rectangle).subtracted(
            QRect(rectangle.center() - QPoint(10, 10), QSize(20, 20)));
    }
    params.radius = 6;
    params.shadowWidth = 8;
    params.shadowColor = QColor(12, 34, 56);
    params.lockAspectRatio = true;
    params.lockDragAspectRatio = true;
    return params;
}

void compoundSelectionsCanSwitchBetweenPreviousAndSavedPresets() {
    for (const bool custom : {false, true}) {
        for (const bool keyboard : {false, true}) {
            QuickSetFixture fixture;
            const auto previous = shapedParams(custom, QRect(30, 40, 240, 160));
            ScreenshotSelectionParams saved;
            saved.selection = QRect(60, 70, 320, 180);
            saved.radius = 10;
            saved.shadowWidth = 4;
            fixture.settings.setPreviousSelectionParams(previous);
            fixture.settings.setPresets({{QStringLiteral("Saved rectangle"), saved}});
            fixture.choose(QStringLiteral("selection:previous"), keyboard);
            require(fixture.presentation.selection.params(fixture.bounds()) == previous &&
                        !fixture.presentation.selection.rectangular(),
                    "Previous must restore full compound/custom geometry and effects");
            require(fixture.settings.shadowColor() == previous.shadowColor,
                    "applying a previous selection must remember its shadow color");
            auto* dropdown = fixture.select();
            require(!dropdown->disabled(),
                    "compound geometry must retain access to selection presets");
            require(dropdown->currentValue() == QStringLiteral("free") &&
                        !dropdown->lineEdit()->text().isEmpty(),
                    "a disabled current ratio must retain its value and indicator");
            const QPointF position(dropdown->rect().center());
            SnowEnterEvent hover(position, position, dropdown->mapToGlobal(position.toPoint()));
            QApplication::sendEvent(dropdown, &hover);
            fixture.flush();
            require(dropdown->popupVisible(), "compound selection presets must open on hover");
            for (const auto& option : dropdown->options()) {
                if (!option.value.toString().startsWith(QStringLiteral("selection:"))) {
                    require(option.disabled,
                            "ratio options must be disabled for compound geometry");
                }
            }
            auto* view = dropdown->view();
            const auto freeOption = fixture.option(QStringLiteral("free"));
            // QAbstractItemView refuses disabled indexes; exercise the command guard explicitly.
            view->selectionModel()->setCurrentIndex(freeOption, QItemSelectionModel::NoUpdate);
            require(view->currentIndex() == freeOption,
                    "the disabled ratio row must be current before testing Enter");
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(view, &enter);
            require(fixture.applications == 1 && fixture.ratioApplications == 0 &&
                        fixture.presentation.selection.selectionRegion() == previous.region,
                    "confirming disabled Free must not replace compound geometry");
            dropdown->selected(QStringLiteral("16:9"), QString());
            require(fixture.ratioApplications == 0,
                    "ratio commands must remain guarded for compound geometry");
            auto* toolbar = fixture.presentation.coordinator.selectionToolbar();
            toolbar->setPointerInteractionEnabled(false);
            require(dropdown->disabled() && !dropdown->popupVisible(),
                    "a selection drag must close and disable compound selection presets");
            dropdown->selected(QStringLiteral("selection:previous"), QString());
            require(fixture.applications == 1,
                    "disabled preset input must not dispatch selection replacements");
            toolbar->setPointerInteractionEnabled(true);
            require(!dropdown->disabled(),
                    "compound selection presets must resume when pointer input is enabled");
            dropdown->hidePopup();
            fixture.choose(QStringLiteral("selection:preset:0"), !keyboard);
            require(fixture.presentation.selection.params(fixture.bounds()).selection ==
                            saved.selection &&
                        fixture.presentation.selection.rectangular() && fixture.applications == 2,
                    "a saved rectangle must replace compound geometry through the same dropdown");
            dropdown->showPopup();
            fixture.flush();
            require(fixture.option(QStringLiteral("16:9")).flags().testFlag(Qt::ItemIsEnabled),
                    "ratio choices must become enabled again for a saved rectangle");
            dropdown->hidePopup();
            fixture.choose(QStringLiteral("16:9"), !keyboard);
            require(fixture.ratioApplications == 1 &&
                        fixture.presentation.selection.aspectRatioPreset() ==
                            ScreenshotSelectionAspectRatioPreset::Landscape16x9,
                    "a restored rectangle must support aspect-ratio commands again");
            fixture.choose(QStringLiteral("selection:previous"), keyboard);
            require(fixture.applications == 3 &&
                        fixture.presentation.selection.params(fixture.bounds()) == previous,
                    "reopening the dropdown must allow repeated full-geometry restores");
        }
    }
}

void unavailableRestoresPreserveTheActiveSelection() {
    for (const bool custom : {false, true}) {
        for (const bool modal : {false, true}) {
            QuickSetFixture fixture;
            const auto original = fixture.presentation.selection.params(fixture.bounds());
            const auto outside = shapedParams(
                custom, QRect(fixture.bounds().bottomRight() + QPoint(100, 100), QSize(100, 80)));
            if (modal) {
                fixture.workflow->openSelectionResizeModalFromToolbar();
                require(static_cast<bool>(fixture.applyModal), "resize modal must provide apply");
                fixture.applyModal(outside);
            } else {
                fixture.workflow->applySelectionParamsFromToolbar(outside);
            }
            fixture.flush();
            require(fixture.presentation.selection.params(fixture.bounds()) == original &&
                        fixture.presentation.interaction.movingSelection() &&
                        fixture.presentation.captureState.sessionState ==
                            ScreenshotSessionState::Editing &&
                        fixture.effectWrites == 0,
                    "an empty clamped restore must preserve geometry, mode and stored effects");
            fixture.settings.setPreviousSelectionParams(outside);
            fixture.select()->showPopup();
            fixture.flush();
            require(!fixture.option(QStringLiteral("selection:previous"))
                         .flags()
                         .testFlag(Qt::ItemIsEnabled),
                    "Previous outside the current canvas must be unavailable");
            fixture.select()->hidePopup();
            const auto partial = shapedParams(
                custom, QRect(fixture.bounds().bottomRight() - QPoint(99, 99), QSize(200, 160)));
            fixture.settings.setPreviousSelectionParams(partial);
            fixture.choose(QStringLiteral("selection:previous"), true);
            require(fixture.presentation.selection.selectionRegion() ==
                            partial.region->intersected(QRegion(fixture.bounds())) &&
                        fixture.effectWrites == 1,
                    "partially clipped Previous must retain its nonempty geometry and effects");
        }
    }
}

void previousAvailabilityUsesCurrentCanvasWithoutChangingStorage() {
    ScreenshotSelectionSettingsStore settings;
    settings.clear();
    const QRect bounds(-100, -50, 300, 200);
    require(!settings.previousSelectionParams(bounds), "missing Previous must remain unavailable");
    require(!clampScreenshotSelectionParamsForRestore({}, bounds),
            "missing geometry must not become a one-pixel restore");
    ScreenshotSelectionParams emptyRegion;
    emptyRegion.selection = QRect(0, 0, 100, 80);
    emptyRegion.region = ScreenshotRegionGeometry{};
    require(!clampScreenshotSelectionParamsForRestore(emptyRegion, bounds),
            "an authoritative empty region must remain unavailable");
    ScreenshotSelectionParams legacy;
    legacy.selection = QRect(500, 500, 100, 80);
    settings.setPreviousSelectionParams(legacy);
    const auto clamped = settings.previousSelectionParams(bounds);
    require(clamped && clamped->selection == QRect(100, 70, 100, 80) && !clamped->region,
            "legacy rectangles must retain size and move inside the current canvas");
    require(!settings.previousSelectionParams(QRect()),
            "an unavailable canvas must not produce a restore");
    for (const bool custom : {false, true}) {
        const auto previous = shapedParams(custom, QRect(500, 500, 100, 80));
        settings.setPreviousSelectionParams(previous);
        require(!settings.previousSelectionParams(bounds) &&
                    settings.previousSelectionParams(QRect(0, 0, 1200, 800)) == previous &&
                    settings.previousSelectionParams() == previous,
                "Previous availability must follow the canvas without changing stored geometry");
        auto authoritative = previous;
        authoritative.selection = {};
        require(clampScreenshotSelectionParamsForRestore(authoritative, QRect(0, 0, 1200, 800)) ==
                    previous,
                "restore validation must honor region geometry over the legacy rectangle");
    }
    settings.clear();
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    selection_presentation_test::IsolatedStorage storage;
    const auto arguments = application.arguments();
    if (!arguments.contains(QStringLiteral("--restore-only")))
        compoundSelectionsCanSwitchBetweenPreviousAndSavedPresets();
    if (!arguments.contains(QStringLiteral("--compound-only"))) {
        unavailableRestoresPreserveTheActiveSelection();
        previousAvailabilityUsesCurrentCanvasWithoutChangingStorage();
    }
    return 0;
}
