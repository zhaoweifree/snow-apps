// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QImage>
#include <QRect>

#include <functional>

namespace snow_shot::presentation {

inline constexpr qint64 imageSplitPreviewMaximumWorkingBytes = 8 * 1024 * 1024;

// Own exactly this native crop, including valid scanline padding, before passing it to Qt's
// conversion/scaling APIs. Preserve the palette, color space and pixel ratio without source writes.
[[nodiscard]] QImage imageSplitCopyTile(const QImage& image, const QRect& region);

// A bounded, smooth premultiplied area preview in the source color space. Native source storage and
// metadata remain immutable; images already within the display bound keep sharing their pixels.
[[nodiscard]] QImage imageSplitPreview(const QImage& image, std::function<bool()> cancelled = {});

} // namespace snow_shot::presentation
