#include <cstdint>
#include <x264.h>

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

void encodeFrames(int depth, const char* preset, const char* tune, int threads, int width,
                  int height) {
    std::cout << "depth=" << depth << " preset=" << preset << " tune=" << (tune ? tune : "none")
              << " threads=" << threads << " size=" << width << 'x' << height << std::endl;
    x264_param_t params;
    require(x264_param_default_preset(&params, preset, tune) == 0, "configure x264 preset");
    params.i_width = width;
    params.i_height = height;
    params.i_bitdepth = depth;
    params.i_csp = X264_CSP_I420;
    params.i_threads = threads;
    params.i_log_level = X264_LOG_ERROR;
    params.i_fps_num = 30;
    params.i_fps_den = 1;
    params.i_timebase_num = 1;
    params.i_timebase_den = 30;
    params.b_vfr_input = 0;
    params.b_repeat_headers = 1;

    // Opening reaches macroblock_cache_allocate before any capture or GUI work.
    x264_t* encoder = x264_encoder_open(&params);
    require(encoder != nullptr, "open software H.264 encoder");
    x264_picture_t input;
    const int csp = X264_CSP_I420 | (depth == 10 ? X264_CSP_HIGH_DEPTH : 0);
    require(x264_picture_alloc(&input, csp, width, height) == 0, "allocate planar input");

    constexpr int frameCount = 12;
    std::array<bool, frameCount> seen{};
    int encodedFrames = 0;
    const auto encode = [&](x264_picture_t* picture) {
        x264_nal_t* nals = nullptr;
        int nalCount = 0;
        x264_picture_t output;
        const int bytes = x264_encoder_encode(encoder, &nals, &nalCount, picture, &output);
        require(bytes >= 0, "encode or drain software H.264 frame");
        if (bytes == 0) {
            return;
        }
        require(nals != nullptr && nalCount > 0, "encoded frame contains NAL units");
        int payloadBytes = 0;
        for (int index = 0; index < nalCount; ++index) {
            require(nals[index].p_payload != nullptr && nals[index].i_payload > 0,
                    "NAL payload is valid");
            payloadBytes += nals[index].i_payload;
        }
        require(payloadBytes == bytes, "all encoded bytes belong to NAL payloads");
        require(output.i_pts >= 0 && output.i_pts < frameCount, "output retains input PTS");
        const auto pts = static_cast<std::size_t>(output.i_pts);
        require(!seen[pts], "each input frame is encoded once");
        seen[pts] = true;
        ++encodedFrames;
    };

    // Multiple changing frames also exercise PREALLOC's frame buffers, reference
    // lists, lookahead, B-frame reordering and worker ownership.
    for (int frame = 0; frame < frameCount; ++frame) {
        for (int plane = 0; plane < 3; ++plane) {
            const int planeWidth = plane == 0 ? width : width / 2;
            const int planeHeight = plane == 0 ? height : height / 2;
            for (int y = 0; y < planeHeight; ++y) {
                auto* row = input.img.plane[plane] + y * input.img.i_stride[plane];
                for (int x = 0; x < planeWidth; ++x) {
                    const int sample = plane == 0 ? 32 + (x + y + frame * 7) % 160 : 128;
                    if (depth == 10) {
                        reinterpret_cast<std::uint16_t*>(row)[x] =
                            static_cast<std::uint16_t>(sample << 2);
                    } else {
                        row[x] = static_cast<std::uint8_t>(sample);
                    }
                }
            }
        }
        input.i_pts = frame;
        encode(&input);
    }
    for (int attempt = 0; x264_encoder_delayed_frames(encoder) > 0; ++attempt) {
        require(attempt < frameCount + threads + 1, "encoder drain is bounded");
        encode(nullptr);
    }
    require(encodedFrames == frameCount, "all submitted frames survive encoder finalization");
    x264_picture_clean(&input);
    x264_encoder_close(encoder);
    x264_param_cleanup(&params);
}

} // namespace

int main(int argc, char** argv) {
    require(argc == 2, "pass the bit depth to verify");
    const int depth = std::atoi(argv[1]);
    require(depth == 8 || depth == 10, "verify 8-bit or 10-bit x264");
    for (const int threads : {1, 2}) {
        encodeFrames(depth, "medium", nullptr, threads, 640, 480);
        encodeFrames(depth, "veryfast", nullptr, threads, 128, 72);
        encodeFrames(depth, "ultrafast", "zerolatency", threads, 128, 72);
    }
}
