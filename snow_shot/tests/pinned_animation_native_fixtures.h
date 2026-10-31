#pragma once

#include "pinned_animation_fixtures.h"

#include <QBuffer>
#include <QImage>

#include <array>
#include <cstring>
#include <stdexcept>

namespace pinned_animation_native_fixtures {
namespace detail {
inline void append32(QByteArray& bytes, quint32 value) {
    for (unsigned shift : {24U, 16U, 8U, 0U})
        bytes.append(static_cast<char>((value >> shift) & 255U));
}

inline quint32 read32(const char* bytes) {
    const auto* data = reinterpret_cast<const uchar*>(bytes);
    return (quint32{data[0]} << 24U) | (quint32{data[1]} << 16U) | (quint32{data[2]} << 8U) |
           data[3];
}

inline quint32 crc32(const QByteArray& bytes) {
    quint32 value = 0xffffffffU;
    for (const char byte : bytes) {
        value ^= static_cast<uchar>(byte);
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1U) ^ ((value & 1U) ? 0xedb88320U : 0U);
    }
    return value ^ 0xffffffffU;
}

inline void chunk(QByteArray& target, const char* type, const QByteArray& data) {
    append32(target, static_cast<quint32>(data.size()));
    QByteArray payload(type, 4);
    payload.append(data);
    target.append(payload);
    append32(target, crc32(payload));
}

struct Png final {
    QByteArray header;
    QByteArray common;
    QVector<QByteArray> data;
};

inline Png png(QRgb color) {
    QImage image(320, 180, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        throw std::runtime_error("Could not encode the native animation PNG fixture.");
    Png result;
    for (qsizetype position = 8; position <= encoded.size() - 12;) {
        const auto length = static_cast<qsizetype>(read32(encoded.constData() + position));
        if (length > encoded.size() - position - 12)
            throw std::runtime_error("Native fixture PNG has an invalid chunk length.");
        const auto raw = encoded.mid(position, length + 12);
        const char* type = encoded.constData() + position + 4;
        if (std::memcmp(type, "IHDR", 4) == 0)
            result.header = encoded.mid(position + 8, length);
        else if (std::memcmp(type, "IDAT", 4) == 0)
            result.data.push_back(encoded.mid(position + 8, length));
        else if (std::memcmp(type, "IEND", 4) != 0)
            result.common.append(raw);
        position += length + 12;
    }
    if (result.header.size() != 13 || result.data.isEmpty())
        throw std::runtime_error("Native fixture PNG has no complete image.");
    return result;
}

inline QByteArray apng() {
    const auto first = png(qRgb(255, 0, 0));
    const auto second = png(qRgb(0, 0, 255));
    if (first.header != second.header || first.common != second.common)
        throw std::runtime_error("Native APNG fixtures must use matching PNG declarations.");
    QByteArray result("\x89PNG\r\n\x1a\n", 8);
    chunk(result, "IHDR", first.header);
    result.append(first.common);
    QByteArray control;
    append32(control, 2);
    append32(control, 0);
    chunk(result, "acTL", control);
    quint32 sequence = 0;
    const auto frameControl = [&](quint16 numerator) {
        QByteArray frame;
        append32(frame, sequence++);
        append32(frame, 320);
        append32(frame, 180);
        append32(frame, 0);
        append32(frame, 0);
        frame.append(static_cast<char>(numerator >> 8U));
        frame.append(static_cast<char>(numerator & 255U));
        frame.append('\x00');
        frame.append('\x0a');
        frame.append('\x00'); // Keep the composed frame.
        frame.append('\x00'); // Each full frame replaces the canvas.
        chunk(result, "fcTL", frame);
    };
    frameControl(1);
    for (const auto& data : first.data)
        chunk(result, "IDAT", data);
    frameControl(2);
    for (const auto& data : second.data) {
        QByteArray frame;
        append32(frame, sequence++);
        frame.append(data);
        chunk(result, "fdAT", frame);
    }
    chunk(result, "IEND", {});
    return result;
}
} // namespace detail

inline QVector<pinned_animation_fixtures::Fixture> images() {
    auto result = pinned_animation_fixtures::images();
    // GIF image descriptors remain genuine 2x2 patches inside a larger logical
    // canvas. WebP likewise permits partial ANMF frames within its VP8X canvas.
    // The compressed pixels remain untouched and the encoded streams are valid.
    result[0].bytes[6] = '\x40';
    result[0].bytes[7] = '\x01';
    result[0].bytes[8] = static_cast<char>(180);
    result[0].bytes[9] = '\x00';
    constexpr std::array<quint32, 2> extents{319, 179};
    for (std::size_t axis = 0; axis < extents.size(); ++axis) {
        for (unsigned byte = 0; byte < 3; ++byte)
            // Qt 5 offers both operator[](int) and operator[](uint) on
            // QByteArray, so a qsizetype index is ambiguous (C2666); the offset
            // is single digits either way.
            result[1].bytes[static_cast<int>(24 + static_cast<qsizetype>(axis) * 3 + byte)] =
                static_cast<char>((extents[axis] >> (byte * 8U)) & 255U);
    }
    result[2].bytes = detail::apng();
    return result;
}
} // namespace pinned_animation_native_fixtures
