#ifndef SNOW_SHOT_TRANSLATION_TRANSLATIONLANGUAGEDETECTOR_H
#define SNOW_SHOT_TRANSLATION_TRANSLATIONLANGUAGEDETECTOR_H

#include <QString>
#include <functional>
#include <optional>

namespace snow_shot::translation {
struct TranslationPreferences;

struct DetectedTranslationLanguage {
    QString languageCode;
    double confidence = 0;
};

using TranslationLanguageDetector =
    std::function<std::optional<DetectedTranslationLanguage>(const QString&)>;

std::optional<DetectedTranslationLanguage> detectTranslationLanguage(const QString& text);
QString resolveTranslationTarget(const TranslationPreferences& preferences,
                                 const std::optional<DetectedTranslationLanguage>& detection);
} // namespace snow_shot::translation

#endif
