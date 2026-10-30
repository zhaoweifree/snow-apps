#ifndef SNOW_SHOT_APP_SUPPORTEDPINFILES_H
#define SNOW_SHOT_APP_SUPPORTEDPINFILES_H

#include "snow_shot/image/supportedimagefiles.h"

#include <optional>

namespace snow_shot::app {

enum class PinDocumentFormat { PlainText, Html, Markdown };

struct PinDocumentFileFormat {
    const char* suffix;
    PinDocumentFormat format;
};

inline constexpr PinDocumentFileFormat kPinDocumentFileFormats[] = {
    {"txt", PinDocumentFormat::PlainText},     {"html", PinDocumentFormat::Html},
    {"htm", PinDocumentFormat::Html},          {"md", PinDocumentFormat::Markdown},
    {"markdown", PinDocumentFormat::Markdown}, {"css", PinDocumentFormat::PlainText},
};

[[nodiscard]] inline std::optional<PinDocumentFormat> pinDocumentFileFormat(const QString& suffix) {
    for (const auto& entry : kPinDocumentFileFormats)
        if (suffix.compare(QLatin1String(entry.suffix), Qt::CaseInsensitive) == 0)
            return entry.format;
    return std::nullopt;
}

[[nodiscard]] inline QStringList supportedPinFileExtensions() {
    auto extensions = image::supportedImageFileExtensions();
    for (const auto& entry : kPinDocumentFileFormats)
        extensions.append(QString::fromLatin1(entry.suffix));
    return extensions;
}

} // namespace snow_shot::app

#endif
