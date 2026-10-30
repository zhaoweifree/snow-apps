#include "../src/presentation/services/screenshotclipboarddibdecoder_p.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main() {
    std::cout << "AVX2 available: " << screenshotClipboardDibAvx2Available() << '\n';
#if defined(__aarch64__) || defined(_M_ARM64)
    require(screenshotClipboardDibSimdAvailable(), "ARM64 must select the NEON DIB decoder");
#endif
    constexpr std::uint32_t sentinel = 0xa5a5a5a5u;
    for (int pixels : {0, 1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33}) {
        for (int offset = 0; offset < 8; ++offset) {
            alignas(32) std::array<std::uint8_t, 33 * 4 + 8> input{};
            std::array<std::uint32_t, 35> expected;
            expected.fill(sentinel);
            auto* source = input.data() + offset;
            for (int pixel = 0; pixel < pixels; ++pixel) {
                source[pixel * 4] = static_cast<std::uint8_t>(pixel * 3 + 1);
                source[pixel * 4 + 1] = static_cast<std::uint8_t>(pixel * 5 + 2);
                source[pixel * 4 + 2] = static_cast<std::uint8_t>(pixel * 7 + 3);
                source[pixel * 4 + 3] = 19;
                expected[static_cast<std::size_t>(pixel + 1)] =
                    0xff000000u | source[pixel * 4] | (std::uint32_t(source[pixel * 4 + 1]) << 8) |
                    (std::uint32_t(source[pixel * 4 + 2]) << 16);
            }
            const auto original = input;
            std::array<std::uint32_t, 35> output;
            output.fill(sentinel);
            screenshotClipboardDibDecodeBgrx(source, output.data() + 1, pixels);
            require(output == expected, "scalar DIB rows preserve pixels and output bounds");
            output.fill(sentinel);
            const bool decoded =
                screenshotClipboardDibDecodeBgrxAvx2(source, output.data() + 1, pixels);
            require(decoded == (pixels > 0 && screenshotClipboardDibAvx2Available()),
                    "AVX2 availability determines whether the optimized path runs");
            if (decoded) {
                require(output == expected, "AVX2 DIB rows and scalar tails match byte decoding");
            } else {
                for (auto value : output)
                    require(value == sentinel, "unavailable AVX2 leaves the output untouched");
            }
            require(input == original, "DIB decoding never changes the serialized input");
            output.fill(sentinel);
            const bool simdDecoded =
                screenshotClipboardDibDecodeBgrxSimd(source, output.data() + 1, pixels);
            require(simdDecoded == (pixels > 0 && screenshotClipboardDibSimdAvailable()),
                    "the selected DIB SIMD backend must execute when available");
            if (simdDecoded) {
                require(output == expected, "selected SIMD DIB rows and tails match byte decoding");
            } else {
                for (auto value : output)
                    require(value == sentinel, "unavailable SIMD leaves output untouched");
            }
            require(input == original, "SIMD DIB decoding preserves serialized input");
        }
    }
}
