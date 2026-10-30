#include "snow_canvas_filter_simd.h"

#if defined(Q_PROCESSOR_ARM_64)
#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif

#include <algorithm>
#include <array>

namespace snow_canvas_filter_render::detail {
namespace {

// Four native QRgb values per vector. Integer arithmetic deliberately follows the
// scalar kernels, including division by 255 and premultiplied-alpha clamping.
uint32x4_t divide255(uint32x4_t value) {
    return vshrq_n_u32(vaddq_u32(vaddq_u32(value, vdupq_n_u32(1)), vshrq_n_u32(value, 8)), 8);
}

template <int Shift> uint32x4_t channel(uint32x4_t pixels) {
    if constexpr (Shift != 0) {
        pixels = vshrq_n_u32(pixels, Shift);
    }
    return vandq_u32(pixels, vdupq_n_u32(255));
}

uint32x4_t blendPixels(uint32x4_t current, uint32x4_t effect, uint32x4_t mix) {
    const uint32x4_t inverse = vsubq_u32(vdupq_n_u32(255), mix);
    const auto blend = [&](uint32x4_t a, uint32x4_t b) {
        return divide255(
            vaddq_u32(vaddq_u32(vmulq_u32(a, inverse), vmulq_u32(b, mix)), vdupq_n_u32(127)));
    };
    return vorrq_u32(vorrq_u32(blend(channel<0>(current), channel<0>(effect)),
                               vshlq_n_u32(blend(channel<8>(current), channel<8>(effect)), 8)),
                     vorrq_u32(vshlq_n_u32(blend(channel<16>(current), channel<16>(effect)), 16),
                               vshlq_n_u32(blend(channel<24>(current), channel<24>(effect)), 24)));
}

QRgb blendPixel(QRgb current, QRgb effect, int mix) {
    const auto blend = [mix](int a, int b) { return (a * (255 - mix) + b * mix + 127) / 255; };
    return qRgba(blend(qRed(current), qRed(effect)), blend(qGreen(current), qGreen(effect)),
                 blend(qBlue(current), qBlue(effect)), blend(qAlpha(current), qAlpha(effect)));
}

uint32x4_t maskWeights(const std::uint8_t* mask, int strength) {
    const std::array<std::uint32_t, 4> values{mask[0], mask[1], mask[2], mask[3]};
    return divide255(
        vaddq_u32(vmulq_n_u32(vld1q_u32(values.data()), static_cast<std::uint32_t>(strength)),
                  vdupq_n_u32(127)));
}

uint32x4_t grayscalePixels(uint32x4_t pixels) {
    uint32x4_t gray = vaddq_u32(
        vaddq_u32(vmulq_n_u32(channel<16>(pixels), 54), vmulq_n_u32(channel<8>(pixels), 183)),
        vaddq_u32(vmulq_n_u32(channel<0>(pixels), 19), vdupq_n_u32(128)));
    gray = vminq_u32(vshrq_n_u32(gray, 8), channel<24>(pixels));
    return vorrq_u32(vmulq_n_u32(gray, 0x00010101), vshlq_n_u32(channel<24>(pixels), 24));
}

QRgb grayscalePixel(QRgb pixel) {
    const int gray = qMin(qAlpha(pixel),
                          (qRed(pixel) * 54 + qGreen(pixel) * 183 + qBlue(pixel) * 19 + 128) >> 8);
    return qRgba(gray, gray, gray, qAlpha(pixel));
}

uint32x4_t invertPixels(uint32x4_t pixels) {
    const uint8x16_t alpha = vreinterpretq_u8_u32(vmulq_n_u32(channel<24>(pixels), 0x01010101));
    const uint32x4_t inverted =
        vreinterpretq_u32_u8(vqsubq_u8(alpha, vreinterpretq_u8_u32(pixels)));
    return vorrq_u32(vandq_u32(inverted, vdupq_n_u32(0x00ffffff)),
                     vandq_u32(pixels, vdupq_n_u32(0xff000000)));
}

QRgb invertPixel(QRgb pixel) {
    const int alpha = qAlpha(pixel);
    return qRgba(alpha - qRed(pixel), alpha - qGreen(pixel), alpha - qBlue(pixel), alpha);
}

template <typename VectorTransform, typename ScalarTransform>
bool colorRectNeon(ConstImageView source, ImageView destination, AlphaView mask, QPoint maskOrigin,
                   int left, int top, int right, int bottom, int strength,
                   VectorTransform vectorTransform, ScalarTransform scalarTransform) {
    if (right - left < 4 || top >= bottom || strength <= 0) {
        return false;
    }
    for (int y = top; y < bottom; ++y) {
        const auto* from =
            reinterpret_cast<const QRgb*>(source.data + qsizetype(y) * source.stride);
        auto* to = reinterpret_cast<QRgb*>(destination.data + qsizetype(y) * destination.stride);
        const auto* alpha = mask.data == nullptr
                                ? nullptr
                                : mask.data + qsizetype(y - maskOrigin.y()) * mask.stride;
        int x = left;
        for (; x + 4 <= right; x += 4) {
            const uint32x4_t effect = vectorTransform(vld1q_u32(from + x));
            const uint32x4_t mix = alpha == nullptr
                                       ? vdupq_n_u32(static_cast<std::uint32_t>(strength))
                                       : maskWeights(alpha + x - maskOrigin.x(), strength);
            const uint32x4_t result = alpha == nullptr && strength == 255
                                          ? effect
                                          : blendPixels(vld1q_u32(to + x), effect, mix);
            vst1q_u32(to + x, result);
        }
        for (; x < right; ++x) {
            const int mix =
                alpha == nullptr ? strength : (alpha[x - maskOrigin.x()] * strength + 127) / 255;
            if (mix != 0) {
                const QRgb effect = scalarTransform(from[x]);
                to[x] = mix == 255 ? effect : blendPixel(to[x], effect, mix);
            }
        }
    }
    return true;
}

uint32x4_t interpolatePixels(uint32x4_t first, uint32x4_t second, int weight) {
    const auto interpolate = [weight](uint32x4_t a, uint32x4_t b) {
        return vshrq_n_u32(
            vaddq_u32(vaddq_u32(vmulq_n_u32(a, static_cast<std::uint32_t>(256 - weight)),
                                vmulq_n_u32(b, static_cast<std::uint32_t>(weight))),
                      vdupq_n_u32(128)),
            8);
    };
    return vorrq_u32(
        vorrq_u32(interpolate(channel<0>(first), channel<0>(second)),
                  vshlq_n_u32(interpolate(channel<8>(first), channel<8>(second)), 8)),
        vorrq_u32(vshlq_n_u32(interpolate(channel<16>(first), channel<16>(second)), 16),
                  vshlq_n_u32(interpolate(channel<24>(first), channel<24>(second)), 24)));
}

int interpolateAndBlend(const QRgb* first, const QRgb* second, QRgb* destination,
                        const std::uint8_t* mask, int count, int weight, int mix) {
    if (weight < 0 || weight > 256 || mix <= 0 || mix > 255) {
        return 0;
    }
    int x = 0;
    for (; x + 4 <= count; x += 4) {
        uint32x4_t result = interpolatePixels(vld1q_u32(first + x), vld1q_u32(second + x), weight);
        if (mask != nullptr || mix < 255) {
            const uint32x4_t weights = mask == nullptr
                                           ? vdupq_n_u32(static_cast<std::uint32_t>(mix))
                                           : maskWeights(mask + x, 255);
            result = blendPixels(vld1q_u32(destination + x), result, weights);
        }
        const uint8x16_t alpha = vreinterpretq_u8_u32(vmulq_n_u32(channel<24>(result), 0x01010101));
        vst1q_u32(destination + x,
                  vreinterpretq_u32_u8(vminq_u8(vreinterpretq_u8_u32(result), alpha)));
    }
    return x;
}

} // namespace

bool brightnessRectNeon(ConstImageView source, ImageView destination, AlphaView mask,
                        QPoint maskOrigin, const QRect& pixels, int scale, int mix) {
    const int white = qMax(0, scale - 65536);
    const int retained = scale - 2 * white;
    const auto transform = [white, retained](uint32x4_t value) {
        const uint32x4_t alpha = channel<24>(value);
        const uint32x4_t bias =
            vaddq_u32(vmulq_n_u32(alpha, static_cast<std::uint32_t>(white)), vdupq_n_u32(32768));
        const auto adjust = [&](uint32x4_t component) {
            return vminq_u32(
                alpha,
                vshrq_n_u32(
                    vaddq_u32(vmulq_n_u32(component, static_cast<std::uint32_t>(retained)), bias),
                    16));
        };
        return vorrq_u32(
            vorrq_u32(adjust(channel<0>(value)), vshlq_n_u32(adjust(channel<8>(value)), 8)),
            vorrq_u32(vshlq_n_u32(adjust(channel<16>(value)), 16), vshlq_n_u32(alpha, 24)));
    };
    return colorRectNeon(source, destination, mask, maskOrigin, pixels.left(), pixels.top(),
                         pixels.right() + 1, pixels.bottom() + 1, mix, transform,
                         [scale](QRgb pixel) { return brightnessPixel(pixel, scale); });
}

bool grayscaleRectNeon(ConstImageView source, ImageView destination, int left, int top, int right,
                       int bottom, int mix) {
    return colorRectNeon(source, destination, {}, {}, left, top, right, bottom, mix,
                         grayscalePixels, grayscalePixel);
}

bool invertRectNeon(ConstImageView source, ImageView destination, int left, int top, int right,
                    int bottom, int mix) {
    return colorRectNeon(source, destination, {}, {}, left, top, right, bottom, mix, invertPixels,
                         invertPixel);
}

bool grayscaleNeon(ImageView image, int beginRow, int endRow, int mix) {
    return grayscaleRectNeon({image.data, image.width, image.height, image.stride}, image, 0,
                             beginRow, image.width, endRow, mix);
}

bool invertNeon(ImageView image, int beginRow, int endRow, int mix) {
    return invertRectNeon({image.data, image.width, image.height, image.stride}, image, 0, beginRow,
                          image.width, endRow, mix);
}

bool grayscaleMaskedNeon(ConstImageView source, ImageView destination, AlphaView mask,
                         int maskOriginX, int maskOriginY, int left, int top, int right, int bottom,
                         int strengthMix) {
    return colorRectNeon(source, destination, mask, {maskOriginX, maskOriginY}, left, top, right,
                         bottom, strengthMix, grayscalePixels, grayscalePixel);
}

bool invertMaskedNeon(ConstImageView source, ImageView destination, AlphaView mask, int maskOriginX,
                      int maskOriginY, int left, int top, int right, int bottom, int strengthMix) {
    return colorRectNeon(source, destination, mask, {maskOriginX, maskOriginY}, left, top, right,
                         bottom, strengthMix, invertPixels, invertPixel);
}

bool downsampleFourTapNeon(ConstImageView source, int sourceLeft, int sourceTop, int sourceRight,
                           int sourceBottom, ImageView destination, int factor, int beginRow,
                           int endRow) {
    if (factor < 2 || destination.width < 4 || beginRow >= endRow) {
        return false;
    }
    for (int y = beginRow; y < endRow; ++y) {
        const int top = sourceTop + y * factor;
        const int bottom = std::min(sourceBottom, top + factor);
        const auto* row0 = reinterpret_cast<const QRgb*>(
            source.data + qsizetype(top + (bottom - top) / 4) * source.stride);
        const auto* row1 = reinterpret_cast<const QRgb*>(
            source.data + qsizetype(top + ((bottom - top) * 3) / 4) * source.stride);
        auto* output =
            reinterpret_cast<QRgb*>(destination.data + qsizetype(y) * destination.stride);
        // NEON has no gather: load the four stratified taps into lanes, then
        // average all channels in parallel. Duplicate edge taps retain exact rounding.
        for (int x = 0; x < destination.width; x += 4) {
            std::array<QRgb, 4> a{}, b{}, c{}, d{}, result{};
            const int lanes = std::min(4, destination.width - x);
            for (int lane = 0; lane < lanes; ++lane) {
                const int left = sourceLeft + (x + lane) * factor;
                const int right = std::min(sourceRight, left + factor);
                const int first = left + (right - left) / 4;
                const int second = left + ((right - left) * 3) / 4;
                a[static_cast<std::size_t>(lane)] = row0[first];
                b[static_cast<std::size_t>(lane)] = row0[second];
                c[static_cast<std::size_t>(lane)] = row1[first];
                d[static_cast<std::size_t>(lane)] = row1[second];
            }
            const uint32x4_t av = vld1q_u32(a.data()), bv = vld1q_u32(b.data());
            const uint32x4_t cv = vld1q_u32(c.data()), dv = vld1q_u32(d.data());
            const auto average = [](uint32x4_t a0, uint32x4_t b0, uint32x4_t c0, uint32x4_t d0) {
                return vshrq_n_u32(vaddq_u32(vaddq_u32(a0, b0), vaddq_u32(c0, d0)), 2);
            };
            vst1q_u32(result.data(),
                      vorrq_u32(vorrq_u32(average(channel<0>(av), channel<0>(bv), channel<0>(cv),
                                                  channel<0>(dv)),
                                          vshlq_n_u32(average(channel<8>(av), channel<8>(bv),
                                                              channel<8>(cv), channel<8>(dv)),
                                                      8)),
                                vorrq_u32(vshlq_n_u32(average(channel<16>(av), channel<16>(bv),
                                                              channel<16>(cv), channel<16>(dv)),
                                                      16),
                                          vshlq_n_u32(average(channel<24>(av), channel<24>(bv),
                                                              channel<24>(cv), channel<24>(dv)),
                                                      24))));
            std::copy_n(result.data(), lanes, output + x);
        }
    }
    return true;
}

int interpolateAndBlendConstantNeon(const QRgb* first, const QRgb* second, QRgb* destination,
                                    int count, int weight, int mix) {
    return interpolateAndBlend(first, second, destination, nullptr, count, weight, mix);
}

int interpolateAndBlendMaskedNeon(const QRgb* first, const QRgb* second, QRgb* destination,
                                  const std::uint8_t* mask, int count, int weight) {
    return mask == nullptr
               ? 0
               : interpolateAndBlend(first, second, destination, mask, count, weight, 255);
}

} // namespace snow_canvas_filter_render::detail
#endif
