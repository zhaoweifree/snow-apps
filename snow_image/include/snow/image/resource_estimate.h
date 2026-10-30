#pragma once

#include "snow/image/codec.h"
#include "snow/image/processing.h"

#include <cstdint>

namespace snow::image {

struct ResourceEstimate final {
    std::uint64_t private_memory_bytes = 0;
    std::uint64_t mapped_bytes = 0;
    std::uint64_t output_bytes = 0;

    [[nodiscard]] Result<std::uint64_t> total_bytes() const;
};

struct ResizeResourceEstimate final {
    // Immutable filter tables retained by prepare_resized_raster().
    std::uint64_t retained_bytes = 0;
    // Peak construction/read scratch, including codec headroom; excludes source/output pixels.
    std::uint64_t working_bytes = 0;
};

[[nodiscard]] SNOW_IMAGE_API Result<ResizeResourceEstimate>
estimate_resize_resources(std::uint32_t source_width, std::uint32_t source_height,
                          PixelFormat source_format, const ResizeOptions& options);

[[nodiscard]] SNOW_IMAGE_API Result<ResourceEstimate>
estimate_decode_resources(const DocumentInfo& document, const DecodeOptions& options = {});
[[nodiscard]] SNOW_IMAGE_API Result<ResourceEstimate>
estimate_transform_resources(const DocumentInfo& document, const TransformOptions& options);
[[nodiscard]] SNOW_IMAGE_API Result<ResourceEstimate>
estimate_export_resources(const DocumentInfo& document, Format format, bool reduce_palette);
[[nodiscard]] SNOW_IMAGE_API Result<ResourceEstimate>
estimate_preview_decode_resources(const DocumentInfo& document);
[[nodiscard]] SNOW_IMAGE_API Result<ResourceEstimate>
estimate_encode_resources(const Document& document, const EncodeOptions& options);

} // namespace snow::image
