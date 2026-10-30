#ifndef SNOW_SHOT_PRESENTATION_SCREENRECORDINGTOOLBARWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENRECORDINGTOOLBARWINDOW_H

#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"

#include <QRect>
#include <functional>
#include <memory>
#if defined(SNOW_SHOT_TEST_HOOKS)
#include <optional>
#endif

class ScreenRecordingToolbarWindow final : public ScreenshotFloatingToolPaletteWindow {
    Q_OBJECT

  public:
    explicit ScreenRecordingToolbarWindow(QWidget* parent = nullptr);
    ~ScreenRecordingToolbarWindow() override;

    void placeForRecordingRegion(const QRect& recordingRegion);
    void showAndActivate();
    void showWithoutActivating();
    void hide();
    void beginRegionInteraction();
    void endRegionInteraction(const QRect& recordingRegion);
    QWidget* autoHideControlWindow();
    void setAutoHideControlOwner(QWidget* owner);
    void setAutoHideControlShowGuard(std::function<bool(QWidget*)> guard);
    void refreshAutoHideControl();
    void dismissAutoHide();
    bool recordingUiPresented() const;
    void setAutoHideBlocked(QObject* owner, bool blocked);

  signals:
    void closeRequested();
    void autoHideControlRequested(QWidget* surface);

  protected:
    void closeEvent(QCloseEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    class AutoHide;
    std::unique_ptr<AutoHide> m_autoHide;
    QRect m_recordingRegion;
    bool m_manuallyDragged = false;
    bool m_placing = false;
    bool m_regionInteractionActive = false;
#if defined(SNOW_SHOT_TEST_HOOKS)
    std::optional<QPoint> m_autoHideCursorPositionOverride;
#endif
    friend class ScreenRecordingToolbarWindowTestAccess;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENRECORDINGTOOLBARWINDOW_H
