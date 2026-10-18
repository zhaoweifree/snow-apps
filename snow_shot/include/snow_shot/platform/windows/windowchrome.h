#ifndef SNOW_SHOT_PLATFORM_WINDOWS_WINDOWCHROME_H
#define SNOW_SHOT_PLATFORM_WINDOWS_WINDOWCHROME_H

class QWidget;
class QObject;

#if defined(Q_OS_WIN) || defined(_WIN32)
// Qt 6 才有聚合头 <QtTypes>（qsizetype 等基础别名的统一入口）。
// Qt 5.15 里同样的内容在 <QtCore/qtypes.h>，而且 qglobal.h 并不包含它，
// 所以不能用 <QtGlobal> 代替。前面必须先引 <QtGlobal>，否则 QT_VERSION /
// QT_VERSION_CHECK 还没定义（有的头把 <QtTypes> 放在第一个 include）。
#include <QtGlobal>
#include "snow_shot/compat/qt_compat.h"
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtTypes>
#else
// Qt 5.15 **根本没有** qtypes.h（那是 Qt6 才拆出来的文件），qsizetype 直接
// typedef 在 qglobal.h 里，所以这里引 <QtGlobal> 即可。
#include <QtGlobal>
#endif
#include <optional>
#include <functional>
#include <memory>

namespace snow_shot::platform::windows {
void setupDwmShadow(QWidget* window);
void bringWindowToForeground(QWidget* window);
[[nodiscard]] bool supportsWindowCaptureExclusion();
[[nodiscard]] bool setWindowExcludedFromCapture(QWidget* window, bool excluded);
// Requires an existing layered HWND. Returns the previous state, or nullopt on failure.
[[nodiscard]] std::optional<bool> setWindowInputTransparent(QWidget* window, bool transparent);
// Request native mouse routing at the unchanged position; cursor selection is asynchronous.
[[nodiscard]] bool refreshCursorUnderPointer();
// Arm before input surfaces become transparent/hidden, then request a native mouse update.
// Local targets complete after native mouse dispatch; foreign targets complete on a
// desktop cursor update (1 s deadline). Windows may publish that event from DWM.
// Once observed, handover survives pointer movement; the caller samples the current cursor.
// Destruction cancels the operation. The callback is suppressed if its context is destroyed.
class CursorRefresh final {
  public:
    explicit CursorRefresh(QObject* context);
    ~CursorRefresh();
    void refresh(std::function<void(bool)> completed);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
[[nodiscard]] bool isNativeWindowVisible(QWidget* window);
[[nodiscard]] bool flushWindowComposition();
bool handleNativeWindowEvent(QWidget* titleBar, void* message, snowNativeEventResult* result);
namespace detail {
[[nodiscard]] bool isNativeCaptionControlHit(qintptr hitTestResult);
}
} // namespace snow_shot::platform::windows
#endif

#endif // SNOW_SHOT_PLATFORM_WINDOWS_WINDOWCHROME_H
