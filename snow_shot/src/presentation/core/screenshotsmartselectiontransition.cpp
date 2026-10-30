#include "snow_shot/presentation/screenshotsmartselectiontransition.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
bool withinSnapTolerance(const QRectF& selection, const QRectF& target) {
    constexpr qreal tolerance = ScreenshotSmartSelectionTransition::kSnapTolerance;
    return std::abs(selection.left() - target.left()) <= tolerance &&
           std::abs(selection.top() - target.top()) <= tolerance &&
           std::abs(selection.right() - target.right()) <= tolerance &&
           std::abs(selection.bottom() - target.bottom()) <= tolerance;
}
} // namespace

ScreenshotSmartSelectionTransition::ScreenshotSmartSelectionTransition(UpdateCallback update)
    : m_update(std::move(update)) {}

void ScreenshotSmartSelectionTransition::setAnimation(
    snow_shot::SelectionTransitionAnimation animation, qint64 nowMs) {
    animation = snow_shot::normalizedSelectionTransitionAnimation(animation);
    if (m_animation == animation) {
        return;
    }
    static_cast<void>(advance(nowMs));
    nowMs = std::max(nowMs, m_lastAdvancedAtMs);
    if (m_running) {
        // Preserve the curve's progress; only the remaining time uses the new duration.
        m_progressAtStart = progressAt(nowMs);
        m_startedAtMs = nowMs;
    }
    m_lastAdvancedAtMs = nowMs;
    m_animation = animation;
    if (m_animation == snow_shot::SelectionTransitionAnimation::Disabled && m_running) {
        static_cast<void>(presentDirectly(m_targetSelection));
    }
}

snow_shot::SelectionTransitionAnimation ScreenshotSmartSelectionTransition::animation() const {
    return m_animation;
}

bool ScreenshotSmartSelectionTransition::update(const QRectF& selection, bool smartFraming,
                                                qint64 nowMs) {
    const bool hasSelection = selection.isValid() && !selection.isEmpty();
    if (!smartFraming || !hasSelection) {
        m_hasPresentedSmartSelection = false;
        return presentDirectly(selection);
    }

    if (m_animation == snow_shot::SelectionTransitionAnimation::Disabled ||
        !m_hasPresentedSmartSelection) {
        m_hasPresentedSmartSelection = true;
        return presentDirectly(selection);
    }

    if (selection == m_targetSelection) {
        return false;
    }

    // Sample the preceding trajectory before starting a curve toward the new target.
    // Presentation owns the clock and decides when frames are committed.
    static_cast<void>(advance(nowMs));
    m_lastAdvancedAtMs = std::max(nowMs, m_lastAdvancedAtMs);
    m_startedAtMs = m_lastAdvancedAtMs;
    m_progressAtStart = 0.0;
    m_startSelection = m_displayedSelection;
    m_targetSelection = selection;
    if (withinSnapTolerance(m_displayedSelection, m_targetSelection)) {
        static_cast<void>(presentDirectly(selection));
    } else {
        m_running = true;
    }
    return true;
}

bool ScreenshotSmartSelectionTransition::advance(qint64 nowMs) {
    if (!m_running || nowMs <= m_lastAdvancedAtMs) {
        return false;
    }
    m_lastAdvancedAtMs = nowMs;
    const qreal progress = progressAt(nowMs);
    QRectF selection;
    if (progress >= 1.0) {
        m_running = false;
        selection = m_targetSelection;
    } else {
        const qreal easedProgress = progress * (2.0 - progress);
        selection = QRectF(
            m_startSelection.x() + (m_targetSelection.x() - m_startSelection.x()) * easedProgress,
            m_startSelection.y() + (m_targetSelection.y() - m_startSelection.y()) * easedProgress,
            m_startSelection.width() +
                (m_targetSelection.width() - m_startSelection.width()) * easedProgress,
            m_startSelection.height() +
                (m_targetSelection.height() - m_startSelection.height()) * easedProgress);
    }
    if (selection == m_displayedSelection) {
        return false;
    }
    m_displayedSelection = selection;
    notifyUpdate();
    return true;
}

bool ScreenshotSmartSelectionTransition::isRunning() const {
    return m_running;
}

QRectF ScreenshotSmartSelectionTransition::displayedSelection() const {
    return m_displayedSelection;
}

int ScreenshotSmartSelectionTransition::durationMs() const {
    return m_animation == snow_shot::SelectionTransitionAnimation::Fast ? kFastDurationMs
                                                                        : kDurationMs;
}

qreal ScreenshotSmartSelectionTransition::progressAt(qint64 nowMs) const {
    return m_progressAtStart + static_cast<qreal>(nowMs - m_startedAtMs) / durationMs();
}

bool ScreenshotSmartSelectionTransition::presentDirectly(const QRectF& selection) {
    m_running = false;
    m_targetSelection = selection;
    if (m_displayedSelection == selection) {
        return false;
    }
    m_displayedSelection = selection;
    notifyUpdate();
    return true;
}

void ScreenshotSmartSelectionTransition::notifyUpdate() {
    if (m_update) {
        m_update(m_displayedSelection);
    }
}
