#ifndef SNOW_SHOT_APP_EDITIONFEATURES_H
#define SNOW_SHOT_APP_EDITIONFEATURES_H

#include "snow_shot/app/edition.h"

#include <QString>

// Edition gating is application policy layered on app/edition.h, not
// presentation code: the storage layer needs it too (the default floating
// toolbar layout and the configuration schema both drop tools the running
// edition does not ship), and storage must not include presentation.
namespace snow_shot::app {
[[nodiscard]] inline bool editionActionToolAvailable(const QString& id) {
    if (id == QStringLiteral("barcode-recognition"))
        return edition::qrRecognition;
    if (id == QStringLiteral("table-recognition"))
        return edition::tableRecognition;
    if (id == QStringLiteral("latex-recognition"))
        return edition::latexRecognition;
    if (id == QStringLiteral("convert-to-markdown") || id == QStringLiteral("convert-to-html"))
        return edition::imageConversion;
    if (id == QStringLiteral("text-translation"))
        return edition::textTranslation;
    return true;
}

[[nodiscard]] inline bool editionRecognitionModeAvailable(int mode) {
    switch (mode) {
    case 0:
        return true;
    case 1:
        return edition::tableRecognition;
    case 2:
        return edition::qrRecognition;
    case 3:
    case 4:
        return edition::imageConversion;
    case 5:
        return edition::latexRecognition;
    default:
        return false;
    }
}

[[nodiscard]] inline bool editionConfigurationKeyAvailable(const QString& key) {
    if (!edition::apiConfiguration && key.startsWith(QStringLiteral("api_configuration/")))
        return false;
    if (!edition::extendedFeatures && key.startsWith(QStringLiteral("extended_features/")))
        return false;
    if (!edition::textTranslation && (key == QStringLiteral("interface/translation_window_size") ||
                                      key.startsWith(QStringLiteral("screenshot_translation/")) ||
                                      key.endsWith(QStringLiteral("/screenshot_translation")) ||
                                      key.endsWith(QStringLiteral("/translate_selected_text")) ||
                                      key.endsWith(QStringLiteral("/text_translation"))))
        return false;
    if (!edition::qrRecognition && (key == QStringLiteral("screenshot/auto_recognize_qr_code") ||
                                    key.endsWith(QStringLiteral("/qr_code_recognition"))))
        return false;
    if (!edition::tableRecognition && key.endsWith(QStringLiteral("/table_recognition")))
        return false;
    if (!edition::tableRecognition && !edition::qrRecognition &&
        key == QStringLiteral("screenshot_toolbar/table_qr_tool"))
        return false;
    if (!edition::imageConversion && key.startsWith(QStringLiteral("screenshot_conversion/")))
        return false;
    return true;
}
} // namespace snow_shot::app

namespace snow_shot {
// These used to live in snow_shot::presentation. The using-declarations keep
// the unqualified call sites in presentation and storage compiling unchanged
// while the definitions sit in the layer that owns them.
using app::editionActionToolAvailable;
using app::editionConfigurationKeyAvailable;
using app::editionRecognitionModeAvailable;
} // namespace snow_shot

#endif // SNOW_SHOT_APP_EDITIONFEATURES_H
