#pragma once

#include "snow_canvas_filter_avx2.h"

namespace snow_canvas_filter_render::detail {

#if defined(Q_PROCESSOR_ARM_64)
bool brightnessRectNeon(ConstImageView source, ImageView destination, AlphaView mask,
                        QPoint maskOrigin, const QRect& pixels, int scale, int mix);

bool grayscaleNeon(ImageView image, int beginRow, int endRow, int mix);
bool invertNeon(ImageView image, int beginRow, int endRow, int mix);
bool grayscaleRectNeon(ConstImageView source, ImageView destination, int left, int top, int right,
                       int bottom, int mix);
bool invertRectNeon(ConstImageView source, ImageView destination, int left, int top, int right,
                    int bottom, int mix);
bool grayscaleMaskedNeon(ConstImageView source, ImageView destination, AlphaView mask,
                         int maskOriginX, int maskOriginY, int left, int top, int right, int bottom,
                         int strengthMix);
bool invertMaskedNeon(ConstImageView source, ImageView destination, AlphaView mask, int maskOriginX,
                      int maskOriginY, int left, int top, int right, int bottom, int strengthMix);
bool downsampleFourTapNeon(ConstImageView source, int sourceLeft, int sourceTop, int sourceRight,
                           int sourceBottom, ImageView destination, int factor, int beginRow,
                           int endRow);
int interpolateAndBlendConstantNeon(const QRgb* first, const QRgb* second, QRgb* destination,
                                    int count, int weight, int mix);
int interpolateAndBlendMaskedNeon(const QRgb* first, const QRgb* second, QRgb* destination,
                                  const std::uint8_t* mask, int count, int weight);

inline constexpr auto brightnessRectSimd = brightnessRectNeon;
inline constexpr auto grayscaleSimd = grayscaleNeon;
inline constexpr auto invertSimd = invertNeon;
inline constexpr auto grayscaleRectSimd = grayscaleRectNeon;
inline constexpr auto invertRectSimd = invertRectNeon;
inline constexpr auto grayscaleMaskedSimd = grayscaleMaskedNeon;
inline constexpr auto invertMaskedSimd = invertMaskedNeon;
inline constexpr auto downsampleFourTapSimd = downsampleFourTapNeon;
inline constexpr auto interpolateAndBlendConstantSimd = interpolateAndBlendConstantNeon;
inline constexpr auto interpolateAndBlendMaskedSimd = interpolateAndBlendMaskedNeon;
#else
inline constexpr auto brightnessRectSimd = brightnessRectAvx2;
inline constexpr auto grayscaleSimd = grayscaleAvx2;
inline constexpr auto invertSimd = invertAvx2;
inline constexpr auto grayscaleRectSimd = grayscaleRectAvx2;
inline constexpr auto invertRectSimd = invertRectAvx2;
inline constexpr auto grayscaleMaskedSimd = grayscaleMaskedAvx2;
inline constexpr auto invertMaskedSimd = invertMaskedAvx2;
inline constexpr auto downsampleFourTapSimd = downsampleFourTapAvx2;
inline constexpr auto interpolateAndBlendConstantSimd = interpolateAndBlendConstantAvx2;
inline constexpr auto interpolateAndBlendMaskedSimd = interpolateAndBlendMaskedAvx2;
#endif

} // namespace snow_canvas_filter_render::detail
