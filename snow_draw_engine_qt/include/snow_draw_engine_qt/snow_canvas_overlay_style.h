// SPDX-License-Identifier: Apache-2.0
#ifndef SNOW_DRAW_ENGINE_QT_SNOW_CANVAS_OVERLAY_STYLE_H
#define SNOW_DRAW_ENGINE_QT_SNOW_CANVAS_OVERLAY_STYLE_H

#include <QPen>

// Shared by the engine's selection frames and host-owned canvas guides. Widths are
// logical view pixels; antialiasing preserves their coverage at fractional DPI.
inline void snowCanvasApplySelectionDashStyle(QPen& pen) {
    pen.setStyle(Qt::CustomDashLine);
    pen.setCapStyle(Qt::FlatCap);
    pen.setDashPattern({5.0, 3.0});
}

#endif // SNOW_DRAW_ENGINE_QT_SNOW_CANVAS_OVERLAY_STYLE_H
