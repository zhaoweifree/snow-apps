#ifndef SNOW_SHOT_STORAGE_PINNEDTEXTSTYLE_H
#define SNOW_SHOT_STORAGE_PINNEDTEXTSTYLE_H

#include <QColor>
#include <QString>
#include <QSize>

namespace snow_shot::storage {

// HTML is stored separately and takes precedence over this text payload format.
enum class PinnedTextFormat { PlainText, Markdown };

[[nodiscard]] inline bool isValidPinnedTextFormat(PinnedTextFormat format) {
    return format == PinnedTextFormat::PlainText || format == PinnedTextFormat::Markdown;
}

// Resolved at import time, independently of later preference or palette changes.
struct PinnedTextStyle final {
    int maximumWidth = 1024;
    QString fontFamily;
    int fontSize = 16;
    QColor textColor = Qt::black;
    QColor backgroundColor = Qt::white;
    // Color cards retain their initial layout across restore and theme changes.
    bool colorSwatch = false;
    QSize colorSwatchSize = QSize(342, 210);

    [[nodiscard]] bool isValid() const {
        return maximumWidth >= 100 && maximumWidth <= 2000 && fontSize >= 8 && fontSize <= 96 &&
               !fontFamily.isEmpty() && fontFamily.size() <= 1024 && textColor.isValid() &&
               backgroundColor.isValid() &&
               (!colorSwatch || colorSwatchSize == QSize(342, 210) ||
                colorSwatchSize == QSize(470, 288));
    }

    friend bool operator==(const PinnedTextStyle&, const PinnedTextStyle&) = default;
};

} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_PINNEDTEXTSTYLE_H
