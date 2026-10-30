#include "common.h"
#include "primitives.h"
#include "cpu.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(SNOW_X265_TEST_MAIN10)
namespace x265_10bit {
const x265_api* x265_api_get(int bitDepth);
}
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

void dispatch() {
    using namespace X265_NS;
    require(cpu_detect(false) == X265_CPU_NEON,
            "intrinsics-only dispatch advertises baseline NEON");
    EncoderPrimitives scalar{}, selected{};
    setupCPrimitives(scalar);
    selected = scalar;
    setupIntrinsicPrimitives(selected, X265_CPU_NEON);
#if defined(SNOW_X265_TEST_ASSEMBLY)
    setupAssemblyPrimitives(selected, X265_CPU_NEON);
#endif
    require(selected.pu[LUMA_8x8].sad != scalar.pu[LUMA_8x8].sad,
            "baseline NEON pixel kernels must replace C entries");
    require(selected.cu[BLOCK_8x8].dct != scalar.cu[BLOCK_8x8].dct,
            "baseline NEON DCT kernels must replace C entries");
#if defined(SNOW_X265_TEST_ASSEMBLY)
    require(selected.cu[BLOCK_16x16].dct != scalar.cu[BLOCK_16x16].dct &&
                selected.cu[BLOCK_16x16].idct != scalar.cu[BLOCK_16x16].idct,
            "ARMASM64 DCT/IDCT entries must replace C fallbacks");
#if !HIGH_BIT_DEPTH
    require(selected.cu[BLOCK_8x8].intra_pred[0] != scalar.cu[BLOCK_8x8].intra_pred[0] &&
                selected.cu[BLOCK_16x16].intra_pred[0] != scalar.cu[BLOCK_16x16].intra_pred[0],
            "ARMASM64 planar entries must replace C fallbacks");
#endif
#else
    require(selected.cu[BLOCK_16x16].dct == scalar.cu[BLOCK_16x16].dct &&
                selected.cu[BLOCK_16x16].idct == scalar.cu[BLOCK_16x16].idct,
            "assembly-only DCT/IDCT entries must retain C fallbacks");
    require(selected.cu[BLOCK_8x8].intra_pred[0] == scalar.cu[BLOCK_8x8].intra_pred[0] &&
                selected.cu[BLOCK_16x16].intra_pred[0] == scalar.cu[BLOCK_16x16].intra_pred[0],
            "assembly-only planar entries must retain C fallbacks");
#endif
    std::array<pixel, 64 * 64> first{}, second{};
    for (std::size_t i = 0; i < first.size(); ++i) {
        first[i] = static_cast<pixel>((i * 73 + i / 11) & ((1 << X265_DEPTH) - 1));
        second[i] = static_cast<pixel>((i * 29 + i / 7) & ((1 << X265_DEPTH) - 1));
    }
    require(selected.pu[LUMA_8x8].sad(first.data(), 64, second.data(), 64) ==
                scalar.pu[LUMA_8x8].sad(first.data(), 64, second.data(), 64),
            "selected NEON pixel operation must execute and match C");
}

template <typename Sample> void encode(int depth, bool neon, const char* path) {
    const x265_api* api = x265_api_get(depth);
    require(api && api->bit_depth == depth, "requested bit-depth API must remain available");
    x265_param* param = api->param_alloc();
    require(param != nullptr, "allocate encoder parameters");
    require(api->param_default_preset(param, "ultrafast", "zerolatency") == 0, "configure encoder");
    require(param->internalBitDepth == depth,
            "selected API must initialize its own internal depth");
    param->sourceWidth = 64;
    param->sourceHeight = 64;
    param->fpsNum = 30;
    param->fpsDenom = 1;
    param->frameNumThreads = 1;
    param->logLevel = X265_LOG_NONE;
    param->bEmitInfoSEI = 0;
    param->cpuid = neon ? X265_CPU_NEON : 0;
    require(api->param_parse(param, "pools", "none") == 0,
            "disable thread pools deterministically");
    x265_encoder* encoder = api->encoder_open(param);
    require(encoder != nullptr, "open scalar/NEON encoder");
    FILE* file = std::fopen(path, "wb");
    require(file != nullptr, "open bitstream output");
    x265_nal* nals = nullptr;
    std::uint32_t count = 0;
    const auto write = [&] {
        for (std::uint32_t i = 0; i < count; ++i) {
            require(std::fwrite(nals[i].payload, 1, nals[i].sizeBytes, file) == nals[i].sizeBytes,
                    "write bitstream payload");
        }
    };
    require(api->encoder_headers(encoder, &nals, &count) > 0, "generate HEVC headers");
    write();
    std::array<Sample, 64 * 64 * 3 / 2> pixels{};
    x265_picture picture{};
    api->picture_init(param, &picture);
    require(picture.bitDepth == depth, "selected API must initialize matching sample storage");
    picture.planes[0] = pixels.data();
    picture.planes[1] = pixels.data() + 64 * 64;
    picture.planes[2] = pixels.data() + 64 * 64 + 32 * 32;
    picture.stride[0] = 64 * sizeof(Sample);
    picture.stride[1] = picture.stride[2] = 32 * sizeof(Sample);
    int frames = 0;
    for (int frame = 0; frame < 4; ++frame) {
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = static_cast<Sample>((i * 73 + i / 7 + frame * 19) & ((1 << depth) - 1));
        }
        picture.pts = frame;
        const int result = api->encoder_encode(encoder, &nals, &count, &picture, nullptr);
        require(result >= 0, "encode deterministic frame");
        frames += result;
        write();
    }
    while (true) {
        const int result = api->encoder_encode(encoder, &nals, &count, nullptr, nullptr);
        require(result >= 0, "flush deterministic frames");
        if (result == 0)
            break;
        frames += result;
        write();
    }
    require(frames == 4, "all deterministic frames are encoded");
    require(std::fclose(file) == 0, "close bitstream output");
    api->encoder_close(encoder);
    api->param_free(param);
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        dispatch();
        return 0;
    }
#if defined(SNOW_X265_TEST_MAIN10)
    if (argc == 2 && std::strcmp(argv[1], "--combined-api") == 0) {
        const x265_api* main8 = x265_api_get(8);
        const x265_api* main10 = x265_api_get(10);
        require(main8 && main8->bit_depth == 8,
                "combined public API must expose its 8-bit encoder");
        require(main10 && main10->bit_depth == 10,
                "combined public API must expose the namespace-isolated Main10 encoder");
        require(main10 == x265_10bit::x265_api_get(0),
                "Main10 API must come from the linked private namespace archive");
        require(main8 != main10 && main8->encoder_open != main10->encoder_open,
                "combined public API must select distinct encoder implementations");
        return 0;
    }
#endif
    require(argc == 3 || argc == 4, "expected scalar/neon, output path, and optional Main10 depth");
    require(std::strcmp(argv[1], "scalar") == 0 || std::strcmp(argv[1], "neon") == 0,
            "encoder mode must be scalar or neon");
    const bool neon = std::strcmp(argv[1], "neon") == 0;
    if (argc == 3) {
        encode<pixel>(X265_DEPTH, neon, argv[2]);
        return 0;
    }
#if defined(SNOW_X265_TEST_MAIN10)
    require(std::strcmp(argv[3], "10") == 0, "combined fixture only supports added depth 10");
    encode<std::uint16_t>(10, neon, argv[2]);
#else
    require(false, "a private Main10 archive is required to test the combined API");
#endif
}
