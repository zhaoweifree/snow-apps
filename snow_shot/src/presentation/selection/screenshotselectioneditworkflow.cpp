#include "snow_shot/presentation/screenshotselectioneditworkflow.h"

#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotselectionlimits.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"

#include <algorithm>
#include <utility>

ScreenshotSelectionEditWorkflow::ScreenshotSelectionEditWorkflow(
    ScreenshotSelectionEditWorkflowContext context)
    : m_context(std::move(context)) {}

void ScreenshotSelectionEditWorkflow::adjustSelectionFromToolbar(int minDx, int minDy, int maxDx,
                                                                 int maxDy) {
    static_cast<void>(m_context.interaction.cancelEffectDrag());
    if (!m_context.interaction.canResizeSelection()) {
        return;
    }

    if (!m_context.selection.adjustFromToolbar(
            minDx, minDy, maxDx, maxDy, m_context.geometry.canvasBounds(),
            snow_shot::presentation::kScreenshotSelectionMinimumSize)) {
        return;
    }
    if (m_context.ui.updateOverlayState) {
        m_context.ui.updateOverlayState();
    }
    if (m_context.ui.showSelectionToolbar) {
        m_context.ui.showSelectionToolbar();
    }
    if (m_context.ui.moveToolbar) {
        m_context.ui.moveToolbar();
    }
}

void ScreenshotSelectionEditWorkflow::setSelectionCornerRadiusFromToolbar(int radius) {
    const bool cancelled = m_context.interaction.cancelEffectDrag();
    const bool changed = m_context.selection.setCornerRadius(radius);
    if (!changed && !cancelled) {
        return;
    }

    if (changed)
        commitSelectionEffects();

    if (m_context.ui.updateOverlayState) {
        m_context.ui.updateOverlayState();
    }
}

void ScreenshotSelectionEditWorkflow::setSelectionShadowWidthFromToolbar(int shadowWidth) {
    const bool cancelled = m_context.interaction.cancelEffectDrag();
    const bool changed = m_context.selection.setShadowWidth(shadowWidth);
    if (!changed && !cancelled) {
        return;
    }

    if (changed)
        commitSelectionEffects();

    if (m_context.ui.updateOverlayState) {
        m_context.ui.updateOverlayState();
    }
}

void ScreenshotSelectionEditWorkflow::toggleSelectionAspectRatioLockFromToolbar() {
    if (!m_context.selection.resizable() || !m_context.interaction.canResizeSelection())
        return;
    m_context.selection.toggleAspectRatioLock(
        snow_shot::presentation::kScreenshotSelectionMinimumSize);
    m_context.persistSelectionAspectRatioPreference(m_context.selection.aspectRatioPreset(),
                                                    m_context.selection.aspectRatioLocked());
    if (m_context.ui.updateOverlayState) {
        m_context.ui.updateOverlayState();
    }
}

void ScreenshotSelectionEditWorkflow::adjustSelectionEffectsFromWheel(int radiusDelta,
                                                                      int shadowDelta) {
    using namespace snow_shot::presentation;
    const bool cancelled = m_context.interaction.cancelEffectDrag();
    const bool radiusChanged = radiusDelta != 0 && m_context.selection.cornerRadiusApplicable() &&
                               m_context.selection.setCornerRadius(
                                   m_context.selection.cornerRadius() +
                                   std::clamp(radiusDelta, -kScreenshotSelectionCornerRadiusMax,
                                              kScreenshotSelectionCornerRadiusMax));
    const bool shadowChanged =
        shadowDelta != 0 && m_context.selection.setShadowWidth(
                                m_context.selection.shadowWidth() +
                                std::clamp(shadowDelta, -kScreenshotSelectionShadowWidthMax,
                                           kScreenshotSelectionShadowWidthMax));
    if (!radiusChanged && !shadowChanged && !cancelled)
        return;
    if (radiusChanged || shadowChanged)
        commitSelectionEffects();
    if (!cancelled && m_context.ui.requestSelectionEffectPresentation)
        m_context.ui.requestSelectionEffectPresentation();
    else if (m_context.ui.updateOverlayState)
        m_context.ui.updateOverlayState();
}

void ScreenshotSelectionEditWorkflow::previewSelectionEffect(ScreenshotSelectionEffectHandle handle,
                                                             int value) {
    const bool changed =
        handle == ScreenshotSelectionEffectHandle::Shadow
            ? m_context.selection.setShadowWidth(value)
            : screenshotSelectionRadiusHandle(handle) && m_context.selection.setCornerRadius(value);
    if (changed && m_context.ui.updateOverlayState)
        m_context.ui.updateOverlayState();
}

void ScreenshotSelectionEditWorkflow::commitSelectionEffects() {
    m_context.persistSelectionEffects(m_context.selection.cornerRadius(),
                                      m_context.selection.shadowWidth(),
                                      m_context.selection.shadowColor());
}

void ScreenshotSelectionEditWorkflow::setSelectionAspectRatioPresetFromToolbar(
    ScreenshotSelectionAspectRatioPreset preset) {
    if (!m_context.selection.resizable() || !m_context.interaction.canResizeSelection() ||
        (preset != ScreenshotSelectionAspectRatioPreset::Free &&
         screenshotSelectionAspectRatioHeightOverWidth(preset) <= 0.0)) {
        return;
    }
    const bool changed = m_context.selection.setAspectRatioPreset(
        preset, m_context.geometry.canvasBounds(),
        snow_shot::presentation::kScreenshotSelectionMinimumSize);
    if (!changed && (m_context.selection.aspectRatioPreset() != preset ||
                     m_context.selection.aspectRatioLocked() !=
                         (preset != ScreenshotSelectionAspectRatioPreset::Free))) {
        return;
    }
    // Exact replacements clear the active preset while retaining the next capture's preference.
    // An explicit choice must update that preference even when the active selection is unchanged.
    m_context.persistSelectionAspectRatioPreference(m_context.selection.aspectRatioPreset(),
                                                    m_context.selection.aspectRatioLocked());
    if (!changed) {
        return;
    }
    if (m_context.ui.updateOverlayState) {
        m_context.ui.updateOverlayState();
    }
    if (m_context.ui.showSelectionToolbar) {
        m_context.ui.showSelectionToolbar();
    }
    if (m_context.ui.moveToolbar) {
        m_context.ui.moveToolbar();
    }
}

void ScreenshotSelectionEditWorkflow::openSelectionResizeModalFromToolbar() {
    const QRect bounds = selectionBounds();
    const ScreenshotSelectionParams currentParams = currentSelectionParams();
    if (bounds.isEmpty() || m_context.selection.multiRegionEditing() ||
        !m_context.selection.hasPixelSelection() || !m_context.interaction.canResizeSelection()) {
        return;
    }

    hideColorPickersForScreenshotUi();
    setColorPickerSuppressedForScreenshotUi(true);
    ScreenshotSelectionResizeRequest request;
    request.currentParams = currentParams;
    request.selectionBounds = bounds;
    m_context.displaySession.forEachImageSource(
        [&](qsizetype, const CapturedDisplayModel& display) {
            request.canvasUsesPoints |= display.canvasUsesPoints;
        });
    request.ownerWindow = ownerWindowForSelectionResizeModal();
    request.onFinished = [this]() { setColorPickerSuppressedForScreenshotUi(false); };
    if (!m_context.ui.openResizeModal ||
        !m_context.ui.openResizeModal(
            &m_context.modalParent, request,
            [this](const ScreenshotSelectionParams& params) { applySelectionParams(params); })) {
        setColorPickerSuppressedForScreenshotUi(false);
        return;
    }
}

void ScreenshotSelectionEditWorkflow::applySelectionParamsFromToolbar(
    const ScreenshotSelectionParams& params) {
    if (selectionBounds().isEmpty() || !m_context.selection.hasPixelSelection() ||
        !m_context.interaction.canResizeSelection()) {
        return;
    }
    applySelectionParams(params);
}

void ScreenshotSelectionEditWorkflow::repositionToolbarForContentChange() {
    if (!m_context.interaction.canResizeSelection()) {
        return;
    }

    if (m_context.ui.repositionSelectionToolbarForContentChange) {
        m_context.ui.repositionSelectionToolbarForContentChange();
    }
}

void ScreenshotSelectionEditWorkflow::hideColorPickersForScreenshotUi() const {
    if (m_context.ui.hideColorPicker) {
        m_context.ui.hideColorPicker();
    }
}

void ScreenshotSelectionEditWorkflow::setColorPickerSuppressedForScreenshotUi(
    bool suppressed) const {
    if (m_context.ui.setColorPickerSuppressed) {
        m_context.ui.setColorPickerSuppressed(suppressed);
    }
}

void ScreenshotSelectionEditWorkflow::applySelectionParams(
    const ScreenshotSelectionParams& params) {
    const QRect bounds = selectionBounds();
    const auto restored = clampScreenshotSelectionParamsForRestore(params, bounds);
    if (!restored) {
        return;
    }
    static_cast<void>(m_context.interaction.cancelEffectDrag());
    if (!m_context.selection.applyParams(*restored, bounds)) {
        return;
    }

    commitSelectionEffects();

    m_context.interaction.applySelectionParams();
    m_context.captureState.sessionState = ScreenshotSessionState::Editing;
    if (m_context.ui.updateOverlayState) {
        m_context.ui.updateOverlayState();
    }
    if (m_context.ui.showToolbar) {
        m_context.ui.showToolbar();
    }
}

QRect ScreenshotSelectionEditWorkflow::selectionBounds() const {
    const QRectF bounds = m_context.geometry.canvasBounds();
    if (bounds.isNull() || bounds.isEmpty()) {
        return {};
    }
    return ScreenshotHalfOpenRect::fromRectF(bounds).toAlignedQRect();
}

ScreenshotSelectionParams ScreenshotSelectionEditWorkflow::currentSelectionParams() const {
    return m_context.selection.params(selectionBounds());
}

QWidget* ScreenshotSelectionEditWorkflow::ownerWindowForSelectionResizeModal() const {
    const QRectF selection = m_context.selection.normalizedSelection();
    const CapturedDisplayModel* display =
        m_context.geometry.displayForCanvasPoint(m_context.displaySession, selection.center());
    if (display == nullptr) {
        display = m_context.geometry.displayForCanvasRect(m_context.displaySession, selection);
    }
    return m_context.displaySession.overlayForDisplay(display);
}
