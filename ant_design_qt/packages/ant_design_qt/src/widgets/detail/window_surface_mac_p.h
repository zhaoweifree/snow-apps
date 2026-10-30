#pragma once

class QWidget;
class QWindow;

namespace adqt::widgets::detail {

void applyMacWindowSurfaceChrome(QWidget* surface, bool minimizeEnabled, bool maximizeEnabled);
void updateMacWindowSurfaceShadow(QWidget* surface);
void releaseMacWindowSurfaceCursor(QWindow* surface);

}  // namespace adqt::widgets::detail
