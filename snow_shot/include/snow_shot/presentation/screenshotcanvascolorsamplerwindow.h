#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASCOLORSAMPLERWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASCOLORSAMPLERWINDOW_H

#include <QColor>
#include <QCursor>
#include <QImage>
#include <QPoint>
#include <QWidget>
#include <QPointer>
#include <functional>
#include <memory>

class QPaintEvent;
namespace adqt::widgets {
class AdColorPicker;
}

class ScreenshotCanvasColorSamplerWindow final : public QWidget {
  public:
    explicit ScreenshotCanvasColorSamplerWindow(QWidget* parent = nullptr);

    void beginSampling(QWidget* owner);
    void updateSample(const QImage& previewImage, const QPoint& globalCursorPosition);
    void endSampling();

    [[nodiscard]] static QCursor samplingCursor();
    [[nodiscard]] QSize sizeHint() const override;

  protected:
    bool nativeEvent(const QByteArray& eventType, void* message, snowNativeEventResult* result) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    void updatePosition(const QPoint& globalCursorPosition);

    QImage m_previewImage;
    QColor m_currentColor;
    QColor m_panelBackground;
    QColor m_previewBorder;
    QColor m_textColor;
    QColor m_secondaryTextColor;
    bool m_sampling = false;
};

// Shared picker/HUD ownership. Hosts supply raster and coordinate policies and
// restore their cursor/input state before a sampled value can emit callbacks.
class ScreenshotCanvasColorSamplingSession final {
  public:
    explicit ScreenshotCanvasColorSamplingSession(QObject& owner);
    ~ScreenshotCanvasColorSamplingSession();
    ScreenshotCanvasColorSamplingSession(const ScreenshotCanvasColorSamplingSession&) = delete;
    ScreenshotCanvasColorSamplingSession&
    operator=(const ScreenshotCanvasColorSamplingSession&) = delete;

    void begin(adqt::widgets::AdColorPicker* picker, std::function<void()> cancel);
    void end();
    void hidePreview();
    void updatePreview(const QImage& preview, const QPoint& globalPosition);
    [[nodiscard]] bool active() const;
    [[nodiscard]] bool commit(const QImage& preview, const std::function<void()>& cleanup);

  private:
    QObject& m_owner;
    QPointer<adqt::widgets::AdColorPicker> m_target;
    QMetaObject::Connection m_destroyed;
    std::unique_ptr<ScreenshotCanvasColorSamplerWindow> m_window;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASCOLORSAMPLERWINDOW_H
