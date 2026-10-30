#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSMARTSELECTIONTRANSITION_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSMARTSELECTIONTRANSITION_H

#include "snow_shot/selectiontransitionanimation.h"

#include <QRectF>
#include <QtGlobal>

#include <functional>

class ScreenshotSmartSelectionTransition final {
  public:
    static constexpr int kDurationMs = 101;
    static constexpr int kFastDurationMs = 67;
    static constexpr qreal kSnapTolerance = 0.4;

    using UpdateCallback = std::function<void(const QRectF&)>;

    explicit ScreenshotSmartSelectionTransition(UpdateCallback update);
    ScreenshotSmartSelectionTransition(const ScreenshotSmartSelectionTransition&) = delete;
    ScreenshotSmartSelectionTransition&
    operator=(const ScreenshotSmartSelectionTransition&) = delete;

    void setAnimation(snow_shot::SelectionTransitionAnimation animation, qint64 nowMs);
    [[nodiscard]] snow_shot::SelectionTransitionAnimation animation() const;
    // Both entry points use the presentation scheduler's monotonic clock in milliseconds.
    [[nodiscard]] bool update(const QRectF& selection, bool smartFraming, qint64 nowMs);
    // Returns whether the displayed geometry changed; no autonomous animation timer runs.
    [[nodiscard]] bool advance(qint64 nowMs);

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] QRectF displayedSelection() const;

  private:
    [[nodiscard]] int durationMs() const;
    [[nodiscard]] qreal progressAt(qint64 nowMs) const;
    [[nodiscard]] bool presentDirectly(const QRectF& selection);
    void notifyUpdate();

    UpdateCallback m_update;
    QRectF m_displayedSelection;
    QRectF m_startSelection;
    QRectF m_targetSelection;
    qint64 m_startedAtMs = 0;
    qint64 m_lastAdvancedAtMs = 0;
    qreal m_progressAtStart = 0.0;
    bool m_running = false;
    bool m_hasPresentedSmartSelection = false;
    snow_shot::SelectionTransitionAnimation m_animation =
        snow_shot::SelectionTransitionAnimation::Soft;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSMARTSELECTIONTRANSITION_H
