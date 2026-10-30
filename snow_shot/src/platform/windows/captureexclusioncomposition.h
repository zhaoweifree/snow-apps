#ifndef SNOW_SHOT_PLATFORM_WINDOWS_CAPTUREEXCLUSIONCOMPOSITION_H
#define SNOW_SHOT_PLATFORM_WINDOWS_CAPTUREEXCLUSIONCOMPOSITION_H

#include <QImage>
#include <QRegion>
#include <QVariant>
#include <QWidget>
#include <qt_windows.h>

namespace snow_shot::platform::windows::detail {

constexpr bool supportsCaptureExclusionCompositionFallback(int majorVersion, int buildNumber) {
    return majorVersion == 10 && buildNumber >= 19041 && buildNumber < 26100;
}

struct CaptureExclusionResult {
    bool succeeded;
    DWORD error;
};

// Coverage belongs to one native surface at one device pixel ratio. Invalidate
// its storage as well as its identity when that surface is retired or changes.
class CaptureExclusionRegionCache {
  public:
    void invalidate() {
        m_hwnd = nullptr;
        m_region = {};
    }

    bool matches(HWND hwnd, const QRegion& region) const {
        return m_hwnd == hwnd && m_region == region;
    }

    void remember(HWND hwnd, const QRegion& region) {
        m_hwnd = hwnd;
        m_region = region;
    }

    bool isEmpty() const {
        return m_hwnd == nullptr && m_region.isEmpty();
    }

  private:
    HWND m_hwnd = nullptr;
    QRegion m_region;
};

template <typename Setter, typename Fallback>
bool setCaptureExclusionWithCompositionFallback(bool excluded, int majorVersion, int buildNumber,
                                                Setter setAffinity, Fallback applyFallback) {
    const auto result = setAffinity();
    if (result.succeeded) {
        return true;
    }
    if (!excluded || result.error != ERROR_NOT_ENOUGH_MEMORY ||
        !supportsCaptureExclusionCompositionFallback(majorVersion, buildNumber) ||
        !applyFallback()) {
        return false;
    }
    return setAffinity().succeeded;
}

inline constexpr char captureExclusionCompositionFallbackProperty[] =
    "snowCaptureExclusionCompositionFallback";

inline void enableCaptureExclusionCompositionFallback(QWidget* window) {
    window->setProperty(captureExclusionCompositionFallbackProperty, true);
}

// Native pixel coverage, including every nonzero antialiased/shadow alpha value.
QRegion captureExclusionAlphaRegion(const QImage& image);
bool usesCaptureExclusionComposition(const QWidget* window);
bool applyCaptureExclusionCompositionFallback(QWidget* window);

} // namespace snow_shot::platform::windows::detail

#endif // SNOW_SHOT_PLATFORM_WINDOWS_CAPTUREEXCLUSIONCOMPOSITION_H
