#include "image/snowimageqtcodec.h"
#include "image/snowimageanimationbridge.h"
#include "snow_shot/image/screenshotanimationsource.h"
#include "pinned_animation_fixtures.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QColorSpace>
#include <QImage>
#include <QtEndian>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void roundTrip(const pinned_animation_fixtures::Fixture& fixture) {
    using namespace snow_shot::image_codec;
    QString error;
    qint64 requiredBytes = 0;
    const auto source = decodeAnimationSource(fixture.bytes, &error, {}, [&](qint64 bytes) {
        requiredBytes = bytes;
        return true;
    });
    require(bool(source) && error.isEmpty(), "decode animation source");
    require(requiredBytes >= fixture.bytes.size() + 16 * 4,
            "admission precedes animation allocation");
    require(source->canvasSize == QSize(2, 2) && source->isAnimated(),
            "animation canvas and count");
    require(source->frameDurationsNs == QVector<qint64>{100'000'000, 200'000'000},
            "normalized source delays");
    require(source->totalPlayCount == 0 && !source->firstFrame.isNull(),
            "initial preview and infinite loops");
    require(source->firstDisplayFrame.size() == source->canvasSize &&
                source->firstDisplayFrame.format() == QImage::Format_ARGB32_Premultiplied &&
                source->firstDisplayFrame.devicePixelRatio() == 1.0 &&
                source->firstDisplayFrame.colorSpace() == source->firstFrame.colorSpace() &&
                source->firstDisplayFrame ==
                    source->firstFrame.convertToFormat(QImage::Format_ARGB32_Premultiplied),
            "initial display pixels are prepared with the playback profile policy");
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    require(source->firstFrame.constBits() == source->firstDisplayFrame.constBits() &&
                source->firstFrame.cacheKey() == source->firstDisplayFrame.cacheKey(),
            "compatible initial source and display pixels share one immutable raster");
#endif
    auto decoder = openAnimationDecoder(source, &error);
    require(bool(decoder) && error.isEmpty() && decoder->frameIndex() == -1,
            "worker decoder before first frame");
    require(decoder->snapshot(&error).isNull() && !error.isEmpty(),
            "no uninitialized frame publication");
    require(decoder->advance(&error) && error.isEmpty(), "worker first advance");
    QImage first = decoder->snapshot(&error);
    require(!first.isNull() && error.isEmpty() && first == source->firstFrame,
            "worker first canvas matches preview");
    const auto firstPixel = first.pixel(0, 0);
    require(decoder->advance(&error), "worker second advance");
    QImage second = decoder->snapshot(&error);
    require(!second.isNull() && first.pixel(0, 0) == firstPixel && second.pixel(0, 0) != firstPixel,
            "immutable independently published animation frames");
    require(!decoder->advance(&error) && error.isEmpty(), "sequence end is not a decode failure");
    require(decoder->reset(&error) && decoder->frameIndex() == -1, "worker reset");
    decoder.reset();
    require(first.pixel(0, 0) == firstPixel && !second.isNull(),
            "frame owner survives decoder destruction");

    const auto rejected =
        decodeAnimationSource(fixture.bytes, &error, {}, [](qint64) { return false; });
    require(!rejected && !error.isEmpty(), "source allocation rejection reports failure");
    int initialAdmissions = 0;
    const auto rejectedDisplay = decodeAnimationSource(
        fixture.bytes, &error, {}, [&](qint64) { return ++initialAdmissions == 1; });
    require(!rejectedDisplay && !error.isEmpty() && initialAdmissions == 2,
            "initial display preparation retains its conservative admission fence");
    bool cancelledDisplay = false;
    initialAdmissions = 0;
    const auto cancelledSource = decodeAnimationSource(
        fixture.bytes, &error, [&] { return cancelledDisplay; },
        [&](qint64) {
            cancelledDisplay = ++initialAdmissions == 2;
            return true;
        });
    require(!cancelledSource && !error.isEmpty() && initialAdmissions == 2,
            "display admission cancellation prevents initial publication");
    require(!decodeAnimationSource(fixture.bytes, &error, [] { return true; }) && !error.isEmpty(),
            "source cancellation reports failure");
    require(!openAnimationDecoder(source, &error, [](qint64) { return false; }) && !error.isEmpty(),
            "worker working allocation rejection");
    int admissions = 0;
    auto limited = openAnimationDecoder(source, &error, [&](qint64) { return ++admissions == 1; });
    require(bool(limited) && limited->advance(&error),
            "limited worker opens and advances without publication");
    require(limited->snapshot(&error).isNull() && !error.isEmpty() && admissions == 2,
            "mandatory publication checks allocation budget");

    bool staticAdmission = false;
    const auto probeStatic = [&](const QByteArray& bytes) {
        const auto result = decodeAnimationSource(bytes, &error, {}, [&](qint64) {
            staticAdmission = true;
            return false;
        });
        require(!result && error.isEmpty() && !staticAdmission,
                "static import retains ordinary allocation policy");
    };
    probeStatic(encodePng(first));
    probeStatic(encodeWebp(first));
    if (fixture.suffix == QStringLiteral("gif")) {
        auto staticGif = fixture.bytes;
        const auto secondControl = staticGif.lastIndexOf(QByteArray::fromHex("21f904"));
        require(secondControl > 0, "static GIF fixture boundary");
        staticGif.truncate(secondControl);
        staticGif.append(char(0x3b));
        probeStatic(staticGif);
        auto zeroDelay = fixture.bytes;
        const auto firstControl = zeroDelay.indexOf(QByteArray::fromHex("21f904"));
        zeroDelay[firstControl + 4] = 0;
        zeroDelay[firstControl + 5] = 0;
        const auto normalized = decodeAnimationSource(zeroDelay, &error);
        require(bool(normalized) && normalized->frameDurationsNs.front() == 100'000'000,
                "zero GIF delay uses deterministic 100ms fallback");
    }
}

QByteArray profiledApng(const QColorSpace& colorSpace) {
    QImage image(1, 1, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::red);
    image.setColorSpace(colorSpace);
    QByteArray encoded;
    QBuffer buffer(&encoded);
    require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
            "encode profile fixture metadata");
    QByteArray colorChunks;
    for (qsizetype position = 8; position <= encoded.size() - 12;) {
        const auto length =
            static_cast<qsizetype>(qFromBigEndian<quint32>(encoded.constData() + position));
        require(length <= encoded.size() - position - 12, "complete profile PNG chunks");
        const auto type = encoded.mid(position + 4, 4);
        if (type == "iCCP" || type == "sRGB" || type == "gAMA" || type == "cHRM" || type == "cICP")
            colorChunks.append(encoded.mid(position, length + 12));
        position += length + 12;
    }
    require(!colorChunks.isEmpty(), "profile fixture includes color metadata");
    auto result = pinned_animation_fixtures::images().back().bytes;
    result.insert(33, colorChunks);
    return result;
}

void initialDisplayPreservesOriginalProfile() {
    using namespace snow_shot::image_codec;
    const QColorSpace srgb(QColorSpace::SRgb);
    for (const auto& profile : {srgb, QColorSpace(QColorSpace::DisplayP3)}) {
        auto source = decodeAnimationSource(profiledApng(profile));
        require(
            source && source->firstFrame.colorSpace() == profile &&
                (source->firstFrame.format() == QImage::Format_ARGB32_Premultiplied ||
                 source->firstFrame.format() == QImage::Format_RGBA8888_Premultiplied) &&
                source->firstFrame.pixelColor(0, 0) == QColor(Qt::red),
            "initial source frame retains original profile and colors in native representation");
        const QImage expected =
            profile == srgb
                ? source->firstFrame.convertToFormat(QImage::Format_ARGB32_Premultiplied)
                : source->firstFrame.convertedToColorSpace(srgb,
                                                           QImage::Format_ARGB32_Premultiplied);
        require(source->firstDisplayFrame == expected &&
                    source->firstDisplayFrame.colorSpace() == srgb &&
                    source->firstFrame.colorSpace() == profile,
                "display normalization matches Qt without modifying the original profile");
        if (profile != srgb)
            require(source->firstDisplayFrame.constBits() != source->firstFrame.constBits(),
                    "non-sRGB source pixels remain distinct from their converted display raster");
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
        else
            require(source->firstDisplayFrame.constBits() == source->firstFrame.constBits() &&
                        source->firstDisplayFrame.cacheKey() == source->firstFrame.cacheKey(),
                    "sRGB source and display pixels share their immutable raster");
#endif
        auto decoder = openAnimationDecoder(source);
        require(decoder && decoder->advance(), "open profiled animation worker");
        const QImage original = decoder->snapshot();
        require(original == source->firstFrame && decoder->advance() &&
                    decoder->snapshot().colorSpace() == profile && decoder->reset() &&
                    decoder->advance() && decoder->snapshot().colorSpace() == profile,
                "reopened snapshots reuse source color metadata across advance and reset");
        const QImage retained = source->firstDisplayFrame;
        decoder.reset();
        source.reset();
        require(retained == expected && original.colorSpace() == profile &&
                    original.pixelColor(0, 0) == QColor(Qt::red),
                "display and original color metadata survive decoder and source destruction");
    }
}
void specialPngs() {
    using namespace snow_shot::image_codec;
    const QByteArray hiddenPoster = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAACGFjVEwAAAABAAAAAcMq2TYAAAAVSURBVHic"
        "Y2Rg+P+fgYGBgQlEgDAAHRkCArFnkfYAAAAaZmNUTAAAAAAAAAACAAAAAgAAAAAAAAAAAAAAAQAA2WEaUQAA"
        "ABlmZEFUAAAAAXicY/zPwPCfgYGBgQlEgDAAHxcCAs82EfUAAAAASUVORK5CYII=");
    QString error;
    const auto source = decodeAnimationSource(hiddenPoster, &error);
    require(bool(source) && !source->isAnimated() && source->frameDurationsNs.size() == 1,
            "one-frame APNG keeps animation semantics");
    require(source->firstFrame.pixelColor(0, 0) == QColor(255, 0, 0, 255),
            "hidden poster excluded from preview");
    require(source->frameDurationsNs.front() == 100'000'000 && source->totalPlayCount == 1,
            "APNG zero delay and one play");
    const QByteArray palette = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACAgMAAAAP2OW3AAAADFBMVEX/AAAA/wAAAAAAAAAlu0AVAAAAA3RSTlP/"
        "/wDXyg1BAAAACGFjVEwAAAACAAAAAPONk3AAAAAaZmNUTAAAAAAAAAACAAAAAgAAAAAAAAAAAAEACgAA6FTcAAAA"
        "AAxJREFUeJxjYGBgAAAABAAB9hc4VQAAABpmY1RMAAAAAQAAAAIAAAACAAAAAAAAAAAAAQAFAAB4e3HpAAAAEGZk"
        "QVQAAAACeJxjCGBiAAAA+ABT376ftAAAAABJRU5ErkJggg==");
    const auto paletted = decodeAnimationSource(palette, &error);
    require(bool(paletted) && paletted->isAnimated(), "indexed APNG palette and transparency");
    auto decoder = openAnimationDecoder(paletted, &error);
    require(bool(decoder) && decoder->advance(&error) && decoder->advance(&error),
            "indexed APNG worker frames");
    require(decoder->snapshot(&error).pixelColor(0, 0) == QColor(0, 255, 0, 255),
            "shared palette across APNG frames");
    auto corrupt = palette;
    corrupt[corrupt.size() - 1] = char(corrupt.back() ^ 1);
    require(!decodeAnimationSource(corrupt, &error) && !error.isEmpty(),
            "malformed APNG cannot flatten to static preview");
    const auto lateExif = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAADCAYAAAC56t6BAAAACGFjVEwAAAACAAAAAPONk3AAAAAaZmNUTAAAAAAAA"
        "AACAAAAAwAAAAAAAAAAAAEACgAAUa8H6AAAABBJREFUeJxj+M/A8J8BKwAAMOoB/77blKwAAAAaZmNUTAAAAAEA"
        "AAACAAAAAwAAAAAAAAAAAAEABQAAwYCqAQAAABNmZEFUAAAAAnicY2DADv4z/AcABBcB/5ooKMoAAAAaZVhJZklJ"
        "KgAIAAAAAQASAQMAAQAAAAYAAAAAAAAAt0gRKQAAAABJRU5ErkJggg==");
    const auto oriented = decodeAnimationSource(lateExif, &error);
    require(bool(oriented) && oriented->canvasSize == QSize(3, 2),
            "legal post-image EXIF rotates animation canvas");
    require(oriented->firstFrame.pixelColor(2, 0) == QColor(255, 0, 0, 255),
            "EXIF orientation applies to preview");
    auto orientedDecoder = openAnimationDecoder(oriented, &error);
    require(bool(orientedDecoder) && orientedDecoder->advance(&error), "oriented worker frame");
    require(orientedDecoder->snapshot(&error) == oriented->firstFrame,
            "worker orientation matches immutable preview");
}

void malformedStaticPngsFailSafely() {
    using namespace snow_shot::image_codec;
    QImage pixels(19, 13, QImage::Format_RGBA8888);
    pixels.fill(QColor(120, 30, 70, 190));
    const QByteArray encoded = encodePng(pixels);
    require(!encoded.isEmpty(), "encode malformed PNG regression fixture");
    const auto imageData = encoded.indexOf("IDAT");
    require(imageData >= 4, "PNG regression fixture image data");
    const QList<QByteArray> truncated{encoded.mid(0, 20), encoded.mid(0, imageData + 9),
                                      encoded.mid(0, encoded.size() - 12)};
    for (int attempt = 0; attempt < 3; ++attempt) {
        for (const auto& bytes : truncated)
            require(decode(bytes, snow::image::Format::png, "malformed.png").isNull(),
                    "malformed PNG header, pixel and final-chunk decoding returns safely");
        require(inspectSize(truncated.front(), snow::image::Format::png).isEmpty(),
                "malformed PNG inspection returns safely");
    }
    require(decode(encoded, snow::image::Format::png, "valid.png").size() == pixels.size(),
            "valid PNG codec remains usable after malformed input");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    require(snow_shot_image_codec_abi_version() == SNOW_SHOT_IMAGE_CODEC_ABI_VERSION,
            "static image codec ABI version");
    require(snow_shot_animation_abi_version() == SNOW_SHOT_ANIMATION_CODEC_ABI_VERSION,
            "animation codec ABI version");
    for (const auto& fixture : pinned_animation_fixtures::images())
        roundTrip(fixture);
    specialPngs();
    malformedStaticPngsFailSafely();
    initialDisplayPreservesOriginalProfile();
    std::cout << "Animation Qt codec tests passed\n";
}
