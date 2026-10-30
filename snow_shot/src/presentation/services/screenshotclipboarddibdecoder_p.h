#ifndef SCREENSHOTCLIPBOARDDIBDECODER_P_H
#define SCREENSHOTCLIPBOARDDIBDECODER_P_H

#include <cstdint>

inline std::uint32_t screenshotClipboardDibReadPixel(const std::uint8_t* source) {
    // DIBs are little-endian serialized bytes, including borrowed buffers and
    // pixel arrays following variable-size headers. No alignment is implied.
    return std::uint32_t(source[0]) | (std::uint32_t(source[1]) << 8) |
           (std::uint32_t(source[2]) << 16) | (std::uint32_t(source[3]) << 24);
}

inline void screenshotClipboardDibDecodeBgrx(const std::uint8_t* source, std::uint32_t* destination,
                                             int pixels) {
    for (int x = 0; x < pixels; ++x)
        destination[x] = screenshotClipboardDibReadPixel(source + x * 4) | 0xff000000u;
}

bool screenshotClipboardDibAvx2Available();
bool screenshotClipboardDibDecodeBgrxAvx2(const std::uint8_t* source, std::uint32_t* destination,
                                          int pixels);
bool screenshotClipboardDibSimdAvailable();
bool screenshotClipboardDibDecodeBgrxSimd(const std::uint8_t* source, std::uint32_t* destination,
                                          int pixels);

#endif
