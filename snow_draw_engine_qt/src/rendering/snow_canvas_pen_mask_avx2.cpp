#include "snow_canvas_pen_mask_avx2.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

#if defined(_M_X64) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define SNOW_PEN_MASK_AVX2_X86 1
#else
#define SNOW_PEN_MASK_AVX2_X86 0
#endif

#if defined(Q_PROCESSOR_ARM_64)
#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#endif

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace snow_canvas_pen_mask_simd {

// Explicitly dispatched AVX2/NEON kernels preserve scalar coverage quantization.
// NOLINTBEGIN(portability-simd-intrinsics)

bool rasterizeCapsuleSegment(std::uint8_t* alpha, std::ptrdiff_t stride, int tileLeft, int tileTop,
                             int beginX, int endX, int beginY, int endY, double ax, double ay,
                             double bx, double by, double transitionOuter) {
#if SNOW_PEN_MASK_AVX2_X86
    if (alpha == nullptr || endX <= beginX || endY <= beginY) {
        return true;
    }
    const double dx = bx - ax;
    const double dy = by - ay;
    const double lengthSquared = dx * dx + dy * dy;
    const __m256d zero = _mm256_setzero_pd();
    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d vectorAx = _mm256_set1_pd(ax);
    const __m256d vectorAy = _mm256_set1_pd(ay);
    const __m256d vectorDx = _mm256_set1_pd(dx);
    const __m256d vectorDy = _mm256_set1_pd(dy);
    const __m256d inverseLength = _mm256_set1_pd(lengthSquared > 0.0 ? 1.0 / lengthSquared : 0.0);
    const __m256d outer = _mm256_set1_pd(transitionOuter);
    alignas(32) double coverages[4];
    for (int y = beginY; y < endY; ++y) {
        std::uint8_t* row = alpha + static_cast<std::ptrdiff_t>(y) * stride;
        const __m256d py = _mm256_set1_pd(tileTop + y + 0.5);
        int x = beginX;
        for (; x + 4 <= endX; x += 4) {
            const __m256d px = _mm256_set_pd(tileLeft + x + 3.5, tileLeft + x + 2.5,
                                             tileLeft + x + 1.5, tileLeft + x + 0.5);
            __m256d closestX = vectorAx;
            __m256d closestY = vectorAy;
            if (lengthSquared > 0.0) {
                __m256d projection = _mm256_mul_pd(
                    _mm256_add_pd(_mm256_mul_pd(_mm256_sub_pd(px, vectorAx), vectorDx),
                                  _mm256_mul_pd(_mm256_sub_pd(py, vectorAy), vectorDy)),
                    inverseLength);
                projection = _mm256_max_pd(zero, _mm256_min_pd(one, projection));
                closestX = _mm256_add_pd(vectorAx, _mm256_mul_pd(projection, vectorDx));
                closestY = _mm256_add_pd(vectorAy, _mm256_mul_pd(projection, vectorDy));
            }
            const __m256d distanceX = _mm256_sub_pd(px, closestX);
            const __m256d distanceY = _mm256_sub_pd(py, closestY);
            const __m256d distance = _mm256_sqrt_pd(_mm256_add_pd(
                _mm256_mul_pd(distanceX, distanceX), _mm256_mul_pd(distanceY, distanceY)));
            const __m256d coverage =
                _mm256_max_pd(zero, _mm256_min_pd(one, _mm256_sub_pd(outer, distance)));
            _mm256_store_pd(coverages, coverage);
            for (int lane = 0; lane < 4; ++lane) {
                row[x + lane] = static_cast<std::uint8_t>(
                    std::max<int>(row[x + lane], qRound(coverages[lane] * 255.0)));
            }
        }
        for (; x < endX; ++x) {
            const double px = tileLeft + x + 0.5;
            const double pyScalar = tileTop + y + 0.5;
            double projection = 0.0;
            if (lengthSquared > 0.0) {
                projection =
                    std::clamp(((px - ax) * dx + (pyScalar - ay) * dy) / lengthSquared, 0.0, 1.0);
            }
            const double distanceX = px - (ax + projection * dx);
            const double distanceY = pyScalar - (ay + projection * dy);
            const double coverage = std::clamp(
                transitionOuter - std::sqrt(distanceX * distanceX + distanceY * distanceY), 0.0,
                1.0);
            row[x] = static_cast<std::uint8_t>(std::max<int>(row[x], qRound(coverage * 255.0)));
        }
    }
    return true;
#elif defined(Q_PROCESSOR_ARM_64)
    if (alpha == nullptr || endX <= beginX || endY <= beginY) {
        return false;
    }
    const double dx = bx - ax;
    const double dy = by - ay;
    const double lengthSquared = dx * dx + dy * dy;
    const float64x2_t zero = vdupq_n_f64(0.0);
    const float64x2_t one = vdupq_n_f64(1.0);
    const float64x2_t vectorAx = vdupq_n_f64(ax);
    const float64x2_t vectorAy = vdupq_n_f64(ay);
    const float64x2_t outer = vdupq_n_f64(transitionOuter);
    bool executed = false;
    for (int y = beginY; y < endY; ++y) {
        std::uint8_t* row = alpha + static_cast<std::ptrdiff_t>(y) * stride;
        const float64x2_t py = vdupq_n_f64(tileTop + y + 0.5);
        int x = beginX;
        for (; x + 2 <= endX; x += 2) {
            const double positions[2]{tileLeft + x + 0.5, tileLeft + x + 1.5};
            const float64x2_t px = vld1q_f64(positions);
            float64x2_t projection = zero;
            if (lengthSquared > 0.0) {
                projection = vdivq_f64(vaddq_f64(vmulq_n_f64(vsubq_f64(px, vectorAx), dx),
                                                 vmulq_n_f64(vsubq_f64(py, vectorAy), dy)),
                                       vdupq_n_f64(lengthSquared));
                projection = vmaxq_f64(zero, vminq_f64(one, projection));
            }
            const float64x2_t distanceX =
                vsubq_f64(px, vaddq_f64(vectorAx, vmulq_n_f64(projection, dx)));
            const float64x2_t distanceY =
                vsubq_f64(py, vaddq_f64(vectorAy, vmulq_n_f64(projection, dy)));
            const float64x2_t distance = vsqrtq_f64(
                vaddq_f64(vmulq_f64(distanceX, distanceX), vmulq_f64(distanceY, distanceY)));
            const float64x2_t coverage =
                vmaxq_f64(zero, vminq_f64(one, vsubq_f64(outer, distance)));
            double values[2];
            vst1q_f64(values, coverage);
            for (int lane = 0; lane < 2; ++lane) {
                row[x + lane] = static_cast<std::uint8_t>(
                    std::max<int>(row[x + lane], qRound(values[lane] * 255.0)));
            }
            executed = true;
        }
        for (; x < endX; ++x) {
            const double px = tileLeft + x + 0.5;
            const double pyScalar = tileTop + y + 0.5;
            const double projection =
                lengthSquared > 0.0
                    ? std::clamp(((px - ax) * dx + (pyScalar - ay) * dy) / lengthSquared, 0.0, 1.0)
                    : 0.0;
            const double distanceX = px - (ax + projection * dx);
            const double distanceY = pyScalar - (ay + projection * dy);
            const double coverage = std::clamp(
                transitionOuter - std::sqrt(distanceX * distanceX + distanceY * distanceY), 0.0,
                1.0);
            row[x] = static_cast<std::uint8_t>(std::max<int>(row[x], qRound(coverage * 255.0)));
        }
    }
    return executed;
#else
    (void)alpha;
    (void)stride;
    (void)tileLeft;
    (void)tileTop;
    (void)beginX;
    (void)endX;
    (void)beginY;
    (void)endY;
    (void)ax;
    (void)ay;
    (void)bx;
    (void)by;
    (void)transitionOuter;
    return false;
#endif
}

// NOLINTEND(portability-simd-intrinsics)

} // namespace snow_canvas_pen_mask_simd
