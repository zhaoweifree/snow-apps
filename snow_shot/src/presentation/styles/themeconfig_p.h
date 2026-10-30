#ifndef SNOW_SHOT_PRESENTATION_STYLES_THEMECONFIG_P_H
#define SNOW_SHOT_PRESENTATION_STYLES_THEMECONFIG_P_H

#include "snow_shot/presentation/styles/themecolorscheme.h"
#include "theme/theme_types.h"

namespace snow_shot::presentation::styles::detail {
inline adqt::theme::ThemeScheme toAdqtScheme(ThemeAppearance appearance) {
    return appearance == ThemeAppearance::Dark ? adqt::theme::ThemeScheme::Dark
                                               : adqt::theme::ThemeScheme::Light;
}

inline adqt::theme::ThemeDensity toAdqtDensity(ThemePreset preset) {
    return preset == ThemePreset::Compact ? adqt::theme::ThemeDensity::Compact
                                          : adqt::theme::ThemeDensity::Comfortable;
}

adqt::theme::ThemeConfig toAdqtThemeConfig(const ThemeStyleConfig& config);
} // namespace snow_shot::presentation::styles::detail

#endif // SNOW_SHOT_PRESENTATION_STYLES_THEMECONFIG_P_H
