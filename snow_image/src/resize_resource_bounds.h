#pragma once

#include "snow/image/result.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace snow::image::detail {
inline Result<std::uint64_t> resize_checked_add(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return Status::error(ErrorCode::limit_exceeded, "Resize resource estimate overflows.");
    return left + right;
}
inline Result<std::uint64_t> resize_checked_multiply(std::uint64_t left, std::uint64_t right) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
        return Status::error(ErrorCode::limit_exceeded, "Resize resource estimate overflows.");
    return left * right;
}

struct ResizeAxisResources final {
    std::uint64_t maximum_count = 0;
    std::uint64_t coefficient_count = 0;
    std::uint64_t retained_bytes = 0;
    std::uint64_t construction_bytes = 0;
};

inline Result<ResizeAxisResources> resize_axis_resources(std::uint32_t source, std::uint32_t target,
                                                         double support, bool nearest = false) {
    if (source == 0 || target == 0 || !std::isfinite(support) || support < 0)
        return Status::error(ErrorCode::invalid_argument, "Resize dimensions are invalid.");
    // Match the filter's float scale. Extra boundary samples cover rounding before clamping;
    // each clamped source index is retained at most once, including edge repeats.
    const float scale = static_cast<float>(target) / static_cast<float>(source);
    const double radius = static_cast<double>(static_cast<float>(support) / std::min(1.0F, scale));
    const auto count = nearest ? std::uint64_t{1}
                               : static_cast<std::uint64_t>(std::min(
                                     static_cast<double>(source), std::ceil(radius * 2.0) + 3.0));
    auto coefficients = resize_checked_multiply(count, target);
    if (!coefficients)
        return coefficients.error();
    // Index/float vector capacities can each grow to twice their final size. Admission also
    // covers old and new storage during growth and the temporary coefficient row.
    auto retained = resize_checked_multiply(coefficients.value(), 16U);
    auto offsets =
        resize_checked_multiply(static_cast<std::uint64_t>(target) + 1U, sizeof(std::size_t));
    auto temporary = resize_checked_multiply(count, 16U);
    if (!retained || !offsets || !temporary)
        return !retained ? retained.error() : !offsets ? offsets.error() : temporary.error();
    auto construction = resize_checked_add(retained.value(), temporary.value());
    retained = resize_checked_add(retained.value(), offsets.value());
    if (!construction || !retained)
        return !construction ? construction.error() : retained.error();
    return ResizeAxisResources{count, coefficients.value(), retained.value(), construction.value()};
}
} // namespace snow::image::detail
