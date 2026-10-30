#pragma once

#include <QImage>
#include <QRectF>

struct CanvasExportSource {
    QImage image;
    QRectF canvasRect;
};

// Integer source pixels relative to the complete export frame, translated without scaling.
struct CanvasExportTile {
    QRect sourcePixels;
    QPoint destination;
};
