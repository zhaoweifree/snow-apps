#ifndef SNOW_SHOT_PLATFORM_WINDOWS_SCROLLINPUT_H
#define SNOW_SHOT_PLATFORM_WINDOWS_SCROLLINPUT_H

#include "snow_shot/platform/screenshotscroll.h"

#include <QPoint>
#include <QRect>

namespace snow_shot::platform::windows {
// The result type is shared with every other platform; only the sender is Windows.
using ScrollInputResult = ::snow_shot::platform::ScrollInputResult;
ScrollInputResult sendScrollingWheelStep(const QRect& physicalSelection, const QPoint& wheelDelta);
} // namespace snow_shot::platform::windows

#endif // SNOW_SHOT_PLATFORM_WINDOWS_SCROLLINPUT_H
