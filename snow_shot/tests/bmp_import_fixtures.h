#ifndef SNOW_SHOT_TESTS_BMP_IMPORT_FIXTURES_H
#define SNOW_SHOT_TESTS_BMP_IMPORT_FIXTURES_H

#include "snowimageqtcodec.h"

#include <QFile>
#include <QImage>

namespace bmp_import_fixtures {

inline bool write(const QString& path, const QImage& image) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) &&
           snow_shot::image_codec::encodeToDevice(image, &file, snow::image::Format::bmp, {});
}

inline bool samePixels(const QImage& actual, const QImage& expected) {
    if (actual.size() != expected.size() || actual.isNull())
        return false;
    for (int y = 0; y < expected.height(); ++y)
        for (int x = 0; x < expected.width(); ++x)
            if (actual.pixelColor(x, y) != expected.pixelColor(x, y))
                return false;
    return true;
}

} // namespace bmp_import_fixtures

#endif // SNOW_SHOT_TESTS_BMP_IMPORT_FIXTURES_H
