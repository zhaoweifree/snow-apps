#include "screenshotclipboarddibdecoder_p.h"

#if defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#endif

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#include <immintrin.h>
#elif defined(__AVX2__)
#include <immintrin.h>
#endif

#if defined(__AVX2__) || defined(_M_AVX2)
namespace {
bool cpuSupportsAvx2() {
#if defined(_MSC_VER)
    int registers[4] = {};
    __cpuidex(registers, 0, 0);
    if (registers[0] < 1)
        return false;
    __cpuidex(registers, 1, 0);
    const bool osxsave = (registers[2] & (1 << 27)) != 0;
    const bool avx = (registers[2] & (1 << 28)) != 0;
    if (!osxsave || !avx)
        return false;
    const unsigned __int64 xcr0 = _xgetbv(0);
    if ((xcr0 & 0x6) != 0x6)
        return false;
    __cpuidex(registers, 0, 0);
    if (registers[0] < 7)
        return false;
    __cpuidex(registers, 7, 0);
    return (registers[1] & (1 << 5)) != 0;
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}
} // namespace
#endif

bool screenshotClipboardDibAvx2Available() {
#if defined(__AVX2__) || defined(_M_AVX2)
    static const bool available = cpuSupportsAvx2();
    return available;
#else
    return false;
#endif
}

bool screenshotClipboardDibSimdAvailable() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return true;
#else
    return screenshotClipboardDibAvx2Available();
#endif
}

bool screenshotClipboardDibDecodeBgrxSimd(const std::uint8_t* source, std::uint32_t* destination,
                                          int pixels) {
#if defined(__aarch64__) || defined(_M_ARM64)
    if (source == nullptr || destination == nullptr || pixels <= 0) {
        return false;
    }
    int x = 0;
    const uint32x4_t alpha = vdupq_n_u32(0xff000000u);
    for (; x + 4 <= pixels; x += 4) {
        // Read serialized bytes without imposing uint32_t alignment on clipboard memory.
        uint32x4_t values = vreinterpretq_u32_u8(vld1q_u8(source + x * 4));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        values = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(values)));
#endif
        vst1q_u32(destination + x, vorrq_u32(values, alpha));
    }
    screenshotClipboardDibDecodeBgrx(source + x * 4, destination + x, pixels - x);
    return true;
#else
    return screenshotClipboardDibDecodeBgrxAvx2(source, destination, pixels);
#endif
}

bool screenshotClipboardDibDecodeBgrxAvx2(const std::uint8_t* source, std::uint32_t* destination,
                                          int pixels) {
#if defined(__AVX2__) || defined(_M_AVX2)
    if (!screenshotClipboardDibAvx2Available() || source == nullptr || destination == nullptr ||
        pixels <= 0) {
        return false;
    }
    const __m256i alpha = _mm256_set1_epi32(static_cast<int>(0xff000000u));
    int x = 0;
    for (; x + 8 <= pixels; x += 8) {
        const __m256i values = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source + x * 4));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + x),
                            _mm256_or_si256(values, alpha));
    }
    screenshotClipboardDibDecodeBgrx(source + x * 4, destination + x, pixels - x);
    return true;
#else
    (void)source;
    (void)destination;
    (void)pixels;
    return false;
#endif
}
