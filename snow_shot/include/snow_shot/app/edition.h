#ifndef SNOW_SHOT_APP_EDITION_H
#define SNOW_SHOT_APP_EDITION_H

#include <QCoreApplication>
#include <QString>

#ifndef SNOW_SHOT_EDITION_MINI
#define SNOW_SHOT_EDITION_MINI 0
#endif
#ifndef SNOW_SHOT_ENABLE_QR_RECOGNITION
#define SNOW_SHOT_ENABLE_QR_RECOGNITION (!SNOW_SHOT_EDITION_MINI)
#endif
#ifndef SNOW_SHOT_ENABLE_TABLE_RECOGNITION
#define SNOW_SHOT_ENABLE_TABLE_RECOGNITION (!SNOW_SHOT_EDITION_MINI)
#endif
#ifndef SNOW_SHOT_ENABLE_IMAGE_CONVERSION
#define SNOW_SHOT_ENABLE_IMAGE_CONVERSION (!SNOW_SHOT_EDITION_MINI)
#endif
#ifndef SNOW_SHOT_ENABLE_LATEX_RECOGNITION
#define SNOW_SHOT_ENABLE_LATEX_RECOGNITION (!SNOW_SHOT_EDITION_MINI)
#endif
#ifndef SNOW_SHOT_ENABLE_TEXT_TRANSLATION
#define SNOW_SHOT_ENABLE_TEXT_TRANSLATION (!SNOW_SHOT_EDITION_MINI)
#endif
#ifndef SNOW_SHOT_ENABLE_API_CONFIGURATION
#define SNOW_SHOT_ENABLE_API_CONFIGURATION (!SNOW_SHOT_EDITION_MINI)
#endif
#ifndef SNOW_SHOT_ENABLE_EXTENDED_FEATURES
#define SNOW_SHOT_ENABLE_EXTENDED_FEATURES (!SNOW_SHOT_EDITION_MINI)
#endif

namespace snow_shot::app::edition {
inline constexpr bool isMini = SNOW_SHOT_EDITION_MINI != 0;
inline constexpr bool qrRecognition = SNOW_SHOT_ENABLE_QR_RECOGNITION != 0;
inline constexpr bool tableRecognition = SNOW_SHOT_ENABLE_TABLE_RECOGNITION != 0;
inline constexpr bool imageConversion = SNOW_SHOT_ENABLE_IMAGE_CONVERSION != 0;
inline constexpr bool latexRecognition = SNOW_SHOT_ENABLE_LATEX_RECOGNITION != 0;
inline constexpr bool textTranslation = SNOW_SHOT_ENABLE_TEXT_TRANSLATION != 0;
inline constexpr bool apiConfiguration = SNOW_SHOT_ENABLE_API_CONFIGURATION != 0;
inline constexpr bool extendedFeatures = SNOW_SHOT_ENABLE_EXTENDED_FEATURES != 0;

[[nodiscard]] inline QString productName() {
    return isMini ? QCoreApplication::translate("EditionMetadata", "Snow Shot Mini")
                  : QCoreApplication::translate("EditionMetadata", "Snow Shot");
}
[[nodiscard]] inline QString applicationName() {
    return isMini ? QStringLiteral("snow_shot_mini") : QStringLiteral("snow_shot");
}
[[nodiscard]] inline QString registryName() {
    return isMini ? QStringLiteral("SnowShotMini") : QStringLiteral("SnowShot");
}
[[nodiscard]] inline QString executableName() {
#ifdef Q_OS_WIN
    return applicationName() + QStringLiteral(".exe");
#else
    return applicationName();
#endif
}
[[nodiscard]] inline QString productId() {
    return isMini ? QStringLiteral("snow-shot-mini") : QStringLiteral("snow-shot");
}
/// The reverse-DNS identity the Linux desktop entry and the AppStream component
/// are both built from, and the same string as the macOS bundle id.
///
/// It is *not* `productId()`, and that is the point: `productId()` is the
/// executable and settings name (`snow_shot`, `snow-shot`), while AppStream 1.0
/// requires a desktop application's component id to be a reverse-DNS name. CI
/// run 42 rejected `snow-shot.desktop` with `cid-desktopapp-is-not-rdns`, and
/// `appstreamcli` fails the command on a warning, so the packaging files had to
/// be renamed rather than the validator relaxed:
///
///     /usr/share/applications/com.snowshot.snow_shot.desktop
///     /usr/share/metainfo/com.snowshot.snow_shot.metainfo.xml
///
/// `QGuiApplication::setDesktopFileName()` is given this value plus `.desktop`,
/// because a mismatch is what leaves a second, iconless dock slot. The autostart
/// entry deliberately keeps the old `<productId>.desktop` name: it lives in a
/// different directory, it is what users' existing `~/.config/autostart` stubs
/// are named, and renaming it would resurrect auto-start for everyone who turned
/// it off.
[[nodiscard]] inline QString desktopEntryId() {
    return isMini ? QStringLiteral("com.snowshot.snow_shot_mini")
                  : QStringLiteral("com.snowshot.snow_shot");
}
[[nodiscard]] inline QString portableMarkerName() {
    return isMini ? QStringLiteral("__mini_data_directory") : QStringLiteral("__data_directory");
}
[[nodiscard]] inline QString updaterName() {
#ifdef Q_OS_WIN
    return productId() + QStringLiteral("-updater.exe");
#else
    return productId() + QStringLiteral("-updater");
#endif
}
[[nodiscard]] inline QString mcpName() {
#ifdef Q_OS_WIN
    return productId() + QStringLiteral("-mcp.exe");
#else
    return productId() + QStringLiteral("-mcp");
#endif
}
[[nodiscard]] inline QString bundleId() {
    return QStringLiteral("com.snowshot.") + applicationName();
}
} // namespace snow_shot::app::edition

#endif // SNOW_SHOT_APP_EDITION_H
