#ifndef SNOW_SHOT_PRESENTATION_CROSSHAIRCURSOR_H
#define SNOW_SHOT_PRESENTATION_CROSSHAIRCURSOR_H

#include <QCursor>

class SnowCanvasWidget;

namespace snow_shot::presentation {

[[nodiscard]] QCursor crosshairCursor(qreal devicePixelRatio = 1.0);
void configureCanvasCrosshairCursor(SnowCanvasWidget& canvas);
[[nodiscard]] bool applyNativeCrosshairCursor(qreal devicePixelRatio);

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_CROSSHAIRCURSOR_H
