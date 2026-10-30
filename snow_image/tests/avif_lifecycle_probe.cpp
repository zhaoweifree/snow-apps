#include <aom/aom_encoder.h>

#include <atomic>
#include <cstdint>

namespace {
std::atomic_uint64_t initialized{0};
std::atomic_uint64_t destroyed{0};
std::atomic_uint64_t overwritten{0};

aom_codec_err_t initialize(aom_codec_ctx_t* context, aom_codec_iface_t* interface,
                           const aom_codec_enc_cfg_t* configuration, aom_codec_flags_t flags,
                           int version) {
    if (context->priv != nullptr)
        overwritten.fetch_add(1, std::memory_order_relaxed);
    const auto result = aom_codec_enc_init_ver(context, interface, configuration, flags, version);
    if (result == AOM_CODEC_OK)
        initialized.fetch_add(1, std::memory_order_relaxed);
    return result;
}

aom_codec_err_t destroy(aom_codec_ctx_t* context) {
    const bool live = context != nullptr && context->priv != nullptr;
    const auto result = aom_codec_destroy(context);
    if (live && result == AOM_CODEC_OK)
        destroyed.fetch_add(1, std::memory_order_relaxed);
    return result;
}

// dyld redirects calls from libheif, while calls within this probe still reach
// the original AOM functions. No allocator thresholds or timing are involved.
[[gnu::used]] const struct {
    const void* replacement;
    const void* original;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {
    {reinterpret_cast<const void*>(initialize),
     reinterpret_cast<const void*>(aom_codec_enc_init_ver)},
    {reinterpret_cast<const void*>(destroy), reinterpret_cast<const void*>(aom_codec_destroy)},
};
} // namespace

extern "C" [[gnu::visibility("default")]] void
snow_image_aom_lifecycle_counts(std::uint64_t* starts, std::uint64_t* finishes,
                                std::uint64_t* replacements) {
    *starts = initialized.load(std::memory_order_relaxed);
    *finishes = destroyed.load(std::memory_order_relaxed);
    *replacements = overwritten.load(std::memory_order_relaxed);
}
