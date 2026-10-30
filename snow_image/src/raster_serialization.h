#pragma once

#include "snow/image/raster.h"

#include <type_traits>

namespace snow::image::detail::raster_serialization {

// SNOWRS and SNOWRB v1 share this little-endian descriptor encoding. Their
// headers, storage manifests, checksums, and publication rules remain separate.
class BinaryWriter final {
  public:
    template <typename T> void integer(T value) {
        using U = std::make_unsigned_t<T>;
        write_integer(static_cast<std::uint64_t>(static_cast<U>(value)), sizeof(T));
    }

    void boolean(bool value);
    void floating(float value);
    void floating(double value);
    void bytes(std::span<const std::byte> value);
    void string(const std::string& value);
    std::vector<std::byte> take() &&;

  private:
    void write_integer(std::uint64_t value, std::size_t size);
    std::vector<std::byte> bytes_;
};

class BinaryReader final {
  public:
    explicit BinaryReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    template <typename T> bool integer(T* value) {
        std::uint64_t encoded = 0;
        if (!value || !read_integer(&encoded, sizeof(T)))
            return false;
        using U = std::make_unsigned_t<T>;
        *value = static_cast<T>(static_cast<U>(encoded));
        return true;
    }

    bool boolean(bool* value);
    bool floating(float* value);
    bool floating(double* value);
    bool bytes(std::vector<std::byte>* value);
    bool string(std::string* value);
    [[nodiscard]] std::size_t remaining() const noexcept;

  private:
    bool read_integer(std::uint64_t* value, std::size_t size);
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};

std::uint32_t crc32c_update(std::uint32_t state, std::span<const std::byte> bytes);
std::uint32_t crc32c(std::span<const std::byte> bytes);
bool crc32c_uses_hardware();
void write_descriptor(BinaryWriter* writer, const DocumentDescriptor& descriptor);
bool read_descriptor(BinaryReader* reader, const RasterStoreLimits& limits,
                     DocumentDescriptor* descriptor);

} // namespace snow::image::detail::raster_serialization
