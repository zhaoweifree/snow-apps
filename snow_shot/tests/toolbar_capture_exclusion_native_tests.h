#pragma once

class ScreenshotFloatingToolPaletteWindow;
class QWidget;
int verifyToolbarCaptureExclusionAffinity(QWidget& window, const char* name,
                                          bool showWindow = true);
int verifyToolbarCaptureExclusion(ScreenshotFloatingToolPaletteWindow& toolbar, const char* name);
