#include <libheif/heif.h>
#include <libheif/heif_sequences.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

extern "C" void snow_image_aom_lifecycle_counts(std::uint64_t*, std::uint64_t*, std::uint64_t*);

namespace {
using Context = std::unique_ptr<heif_context, decltype(&heif_context_free)>;
using Encoder = std::unique_ptr<heif_encoder, decltype(&heif_encoder_release)>;
using Image = std::unique_ptr<heif_image, decltype(&heif_image_release)>;
using Handle = std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)>;
constexpr int width = 64;
constexpr int height = 64;

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

void check(heif_error error, std::string_view operation) {
    if (error.code != heif_error_Ok)
        throw std::runtime_error(std::string(operation) + ": " + error.message);
}

Context context() {
    Context result(heif_context_alloc(), heif_context_free);
    require(result != nullptr, "Allocate HEIF context");
    return result;
}

Encoder encoder(heif_context* owner) {
    const heif_encoder_descriptor* descriptor = nullptr;
    require(heif_get_encoder_descriptors(heif_compression_AV1, "aom", &descriptor, 1) == 1,
            "The lifecycle regression requires the AOM AVIF encoder");
    heif_encoder* raw = nullptr;
    check(heif_context_get_encoder(owner, descriptor, &raw), "Create AOM encoder");
    Encoder result(raw, heif_encoder_release);
    check(heif_encoder_set_lossless(raw, 1), "Set lossless");
    check(heif_encoder_set_parameter_integer(raw, "threads", 1), "Set one encoder thread");
    check(heif_encoder_set_parameter_integer(raw, "speed", 9), "Set encoding speed");
    check(heif_encoder_set_parameter_string(raw, "chroma", "444"), "Set full chroma");
    check(heif_encoder_set_parameter_boolean(raw, "enable-intrabc", 0), "Disable intrablock copy");
    return result;
}

Image image(bool alpha) {
    heif_image* raw = nullptr;
    check(heif_image_create(width, height, heif_colorspace_RGB,
                            alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB,
                            &raw),
          "Create image");
    Image result(raw, heif_image_release);
    check(heif_image_add_plane(raw, heif_channel_interleaved, width, height, 8), "Create plane");
    int stride = 0;
    auto* pixels = heif_image_get_plane(raw, heif_channel_interleaved, &stride);
    require(pixels != nullptr, "Get image plane");
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto* pixel = pixels + y * stride + x * (alpha ? 4 : 3);
            pixel[0] = static_cast<std::uint8_t>(x * 3);
            pixel[1] = static_cast<std::uint8_t>(y * 3);
            pixel[2] = static_cast<std::uint8_t>(x + y);
            if (alpha)
                pixel[3] = static_cast<std::uint8_t>((x + y) % 256);
        }
    }
    return result;
}

void encode_still(heif_context* owner, heif_encoder* codec, bool alpha) {
    auto source = image(alpha);
    heif_image_handle* raw = nullptr;
    check(heif_context_encode_image(owner, source.get(), codec, nullptr, &raw), "Encode still");
    Handle handle(raw, heif_image_handle_release);
}

void write(heif_context* owner) {
    std::uint64_t bytes = 0;
    heif_writer writer{};
    writer.writer_api_version = 1;
    writer.write = [](heif_context*, const void*, std::size_t size, void* user) {
        *static_cast<std::uint64_t*>(user) += size;
        return heif_error{heif_error_Ok, heif_suberror_Unspecified, ""};
    };
    check(heif_context_write(owner, &writer, &bytes), "Write AVIF");
    require(bytes != 0, "AVIF output must be nonempty");
}

void still(bool alpha, int images_per_encoder) {
    for (int iteration = 0; iteration < 3; ++iteration) {
        auto owner = context();
        auto codec = encoder(owner.get());
        for (int index = 0; index < images_per_encoder; ++index)
            encode_still(owner.get(), codec.get(), alpha);
        write(owner.get());
    }
}

void sequence() {
    auto owner = context();
    auto codec = encoder(owner.get());
    // Snow's sequence route reuses its still-poster encoder for sequence startup.
    encode_still(owner.get(), codec.get(), true);
    const auto free_options = &heif_sequence_encoding_options_release;
    std::unique_ptr<heif_sequence_encoding_options, decltype(free_options)> options(
        heif_sequence_encoding_options_alloc(), free_options);
    require(options != nullptr, "Create sequence options");
    options->save_alpha_channel = 1;
    options->gop_structure = heif_sequence_gop_structure_intra_only;
    heif_track* raw = nullptr;
    check(heif_context_add_visual_sequence_track(owner.get(), width, height,
                                                 heif_track_type_image_sequence, nullptr,
                                                 options.get(), &raw),
          "Create sequence track");
    std::unique_ptr<heif_track, decltype(&heif_track_release)> track(raw, heif_track_release);
    for (int frame = 0; frame < 2; ++frame) {
        auto source = image(true);
        heif_image_set_duration(source.get(), 1);
        check(
            heif_track_encode_sequence_image(track.get(), source.get(), codec.get(), options.get()),
            "Encode sequence frame");
    }
    check(heif_track_encode_end_of_sequence(track.get(), codec.get()), "Finish sequence");
    write(owner.get());
}

void failed_configuration() {
    auto owner = context();
    auto codec = encoder(owner.get());
    // AOM applies custom options after allocating its context. Force that error
    // path, then retry on the same encoder so both failed and live state retire.
    check(heif_encoder_set_parameter_string(codec.get(), "aom:cpu-used", "invalid"),
          "Configure deliberate AOM failure");
    auto source = image(true);
    heif_image_handle* raw = nullptr;
    const auto error =
        heif_context_encode_image(owner.get(), source.get(), codec.get(), nullptr, &raw);
    Handle handle(raw, heif_image_handle_release);
    require(error.code != heif_error_Ok, "Invalid AOM option must fail after initialization");
    check(heif_encoder_set_parameter_string(codec.get(), "aom:cpu-used", "9"),
          "Replace invalid AOM option");
    encode_still(owner.get(), codec.get(), true);
    write(owner.get());
}

struct Counts {
    std::uint64_t initialized = 0;
    std::uint64_t destroyed = 0;
    std::uint64_t overwritten = 0;
};

Counts counts() {
    Counts result;
    snow_image_aom_lifecycle_counts(&result.initialized, &result.destroyed, &result.overwritten);
    return result;
}

template <typename Work>
bool run(std::string_view name, Work work, bool expect_initialization = true) {
    const auto before = counts();
    try {
        work();
        const auto after = counts();
        const auto initialized = after.initialized - before.initialized;
        const auto destroyed = after.destroyed - before.destroyed;
        const auto overwritten = after.overwritten - before.overwritten;
        std::cout << name << ": initialized=" << initialized << " destroyed=" << destroyed
                  << " overwritten=" << overwritten << '\n';
        require((initialized != 0) == expect_initialization,
                "Lifecycle probe must observe exactly the expected native initialization");
        require(overwritten == 0, "AOM contexts must be destroyed before reinitialization");
        require(initialized == destroyed, "Every initialized AOM context must be destroyed");
        return true;
    } catch (const std::exception& error) {
        std::cerr << name << ": " << error.what() << '\n';
        return false;
    }
}
} // namespace

int main() {
    bool passed = run(
        "unused encoder",
        [] {
            auto owner = context();
            auto codec = encoder(owner.get());
        },
        false);
    passed = run("opaque still", [] { still(false, 1); }) && passed;
    passed = run("nonopaque still", [] { still(true, 1); }) && passed;
    passed = run("reused still encoder", [] { still(true, 2); }) && passed;
    passed = run("poster and alpha sequence", sequence) && passed;
    passed = run("failed initialization and retry", failed_configuration) && passed;
    return passed ? 0 : 1;
}
